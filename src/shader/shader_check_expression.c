/* Checking expressions: the type of each, the conversions HLSL makes
 * implicitly, overloads, swizzles, members, indexing, constructors, casts,
 * and what may be assigned. */
#include "shader_check_internal.h"

#include "badger/arena.h"
#include "badger/decimal.h"
#include "badger/str.h"

#include <string.h>

static int name_length(tb_shader_name name)
{
    return (int)name.length;
}

static bool is_error(const tb_shader_type *type)
{
    return type == NULL || type->type_class == TB_SHADER_CLASS_ERROR;
}

static const tb_shader_type *error_type(tb_shader_checker *checker)
{
    return tb_shader_error_type(checker->types);
}

static const tb_shader_type *scalar(tb_shader_checker *checker, tb_shader_scalar component)
{
    return tb_shader_scalar_type(checker->types, component);
}

/* The type of a node, the error type for a node the parser could not make. */
static const tb_shader_type *type_of(tb_shader_checker *checker, const tb_shader_node *node)
{
    return node == NULL || node->checked == NULL ? error_type(checker) : node->checked;
}

/* An unscoped enum takes part in arithmetic as its integer. */
const tb_shader_type *tb_shader_check_arithmetic_view(tb_shader_checker *checker,
                                                       const tb_shader_type *type)
{
    if (type->type_class == TB_SHADER_CLASS_ENUM && !type->enumeration->is_scoped)
    {
        return scalar(checker, type->scalar);
    }
    return type;
}

/* -------------------------------------------------------------------------
 * Conversions.
 * ------------------------------------------------------------------------- */

typedef enum conversion
{
    CONVERSION_NONE,     /* Not allowed. */
    CONVERSION_IDENTITY, /* Nothing to do. */
    CONVERSION_ALLOWED,
    CONVERSION_TRUNCATES /* Allowed, dropping components: a warning when implicit. */
} conversion;

static conversion numeric_conversion(const tb_shader_type *from, const tb_shader_type *to,
                                     bool is_explicit)
{
    const uint32_t from_count = tb_shader_component_count(from);
    const uint32_t to_count = tb_shader_component_count(to);
    if (from_count == 1)
    {
        return CONVERSION_ALLOWED;
    }
    if (is_explicit)
    {
        return from_count >= to_count ? CONVERSION_ALLOWED : CONVERSION_NONE;
    }
    if (to->type_class == TB_SHADER_CLASS_SCALAR)
    {
        return CONVERSION_TRUNCATES;
    }
    if (from->type_class == to->type_class)
    {
        if (to->rows > from->rows || to->columns > from->columns)
        {
            return CONVERSION_NONE;
        }
        return to_count < from_count ? CONVERSION_TRUNCATES : CONVERSION_ALLOWED;
    }
    /* A vector and a matrix of as many components convert to each other. */
    if (from_count == to_count)
    {
        return CONVERSION_ALLOWED;
    }
    const bool is_shorter_vector =
        to->type_class == TB_SHADER_CLASS_VECTOR && to_count < from_count;
    return is_shorter_vector ? CONVERSION_TRUNCATES : CONVERSION_NONE;
}

/* What converting a value of `from` to `to` does. */
static conversion classify(tb_shader_checker *checker, const tb_shader_type *from,
                           const tb_shader_type *to, bool is_explicit)
{
    if (from == to || is_error(from) || is_error(to))
    {
        return CONVERSION_IDENTITY;
    }
    if (to->type_class == TB_SHADER_CLASS_ENUM || from->type_class == TB_SHADER_CLASS_ENUM)
    {
        /* An enum converts to its integer implicitly when unscoped; anything
         * numeric becomes one only by a cast. */
        const tb_shader_type *const view = tb_shader_check_arithmetic_view(checker, from);
        const bool numeric_from = tb_shader_type_is_numeric(view) || is_explicit;
        const bool to_enum_ok = to->type_class != TB_SHADER_CLASS_ENUM || is_explicit;
        if (numeric_from && to_enum_ok && tb_shader_component_count(from) == 1)
        {
            const tb_shader_type *const target =
                to->type_class == TB_SHADER_CLASS_ENUM ? scalar(checker, to->scalar) : to;
            return tb_shader_type_is_numeric(target) ? CONVERSION_ALLOWED : CONVERSION_NONE;
        }
        return CONVERSION_NONE;
    }
    if (tb_shader_type_is_numeric(from) && tb_shader_type_is_numeric(to))
    {
        return numeric_conversion(from, to, is_explicit);
    }
    if (!is_explicit)
    {
        return CONVERSION_NONE;
    }
    /* A cast between aggregates goes component by component, as HLSL flattens them. */
    const uint32_t from_flat = tb_shader_flat_count(from);
    const uint32_t to_flat = tb_shader_flat_count(to);
    if (from_flat == 0 || to_flat == 0)
    {
        return CONVERSION_NONE;
    }
    return from_flat == 1 || from_flat >= to_flat ? CONVERSION_ALLOWED : CONVERSION_NONE;
}

static tb_shader_node *new_node(tb_shader_checker *checker, tb_shader_node_kind kind,
                                tb_shader_location location)
{
    tb_shader_node *const node = TB_ARENA_NEW(checker->arena, tb_shader_node);
    node->kind = kind;
    node->location = location;
    return node;
}

static tb_shader_node *wrap(tb_shader_checker *checker, tb_shader_node *node,
                            const tb_shader_type *type)
{
    tb_shader_node *const converted = new_node(checker, TB_SHADER_NODE_CONVERT, node->location);
    converted->left = node;
    converted->checked = type;
    converted->next = node->next;
    node->next = NULL;
    tb_shader_fold(checker, converted);
    return converted;
}

/* A literal floating-point constant that loses its fraction as an integer. */
static void warn_lost_fraction(tb_shader_checker *checker, const tb_shader_node *node,
                               const tb_shader_type *to)
{
    const bool to_integer =
        tb_shader_type_is_numeric(to) && tb_shader_scalar_is_integer(to->scalar);
    if (!to_integer || !node->is_constant || !node->constant.is_float)
    {
        return;
    }
    const double value = node->constant.value;
    if (value == (double)(int64_t)value)
    {
        return;
    }
    tb_shader_check_warning(checker, node->location, "converting %g to '%s' drops its fraction",
                            value, to->name);
}

static tb_shader_node *convert_to(tb_shader_checker *checker, tb_shader_node *node,
                                  const tb_shader_type *type, bool warn)
{
    if (node == NULL)
    {
        return NULL;
    }
    const tb_shader_type *const from = type_of(checker, node);
    const conversion kind = classify(checker, from, type, false);
    if (kind == CONVERSION_IDENTITY)
    {
        return node;
    }
    if (kind == CONVERSION_NONE)
    {
        tb_shader_check_error(checker, node->location, "cannot convert from '%s' to '%s'",
                              from->name, type->name);
        return node;
    }
    if (kind == CONVERSION_TRUNCATES && warn)
    {
        tb_shader_check_warning(checker, node->location, "'%s' is truncated to '%s'", from->name,
                                type->name);
    }
    if (warn)
    {
        warn_lost_fraction(checker, node, type);
    }
    return wrap(checker, node, type);
}

tb_shader_node *tb_shader_check_convert(tb_shader_checker *checker, tb_shader_node *node,
                                        const tb_shader_type *type)
{
    return convert_to(checker, node, type, true);
}

/* -------------------------------------------------------------------------
 * Names and literals.
 * ------------------------------------------------------------------------- */

static void check_this(tb_shader_checker *checker, tb_shader_node *node)
{
    if (checker->method_owner == NULL)
    {
        tb_shader_check_error(checker, node->location, "'this' is used outside a method");
        node->checked = error_type(checker);
        return;
    }
    node->checked = checker->method_owner;
    node->is_lvalue = true;
    node->is_assignable = true;
}

static void check_name(tb_shader_checker *checker, tb_shader_node *node)
{
    node->checked = error_type(checker);
    if (tb_shader_name_equal(&node->name, &(tb_shader_name){"this", 4}))
    {
        check_this(checker, node);
        return;
    }
    tb_shader_symbol *const symbol = tb_shader_check_lookup(checker, node->name);
    const int length = name_length(node->name);
    if (symbol == NULL)
    {
        tb_shader_check_error(checker, node->location, "'%.*s' is not declared", length,
                              node->name.text);
        return;
    }
    node->symbol = symbol;
    switch (symbol->kind)
    {
    case TB_SHADER_SYMBOL_VARIABLE:
    case TB_SHADER_SYMBOL_PARAMETER:
    case TB_SHADER_SYMBOL_FIELD:
        node->checked = symbol->type;
        node->is_lvalue = true;
        node->is_assignable = !symbol->is_const;
        node->is_constant = symbol->has_value;
        node->constant = symbol->value;
        return;
    case TB_SHADER_SYMBOL_ENUMERATOR:
        node->checked = symbol->type;
        node->is_constant = true;
        node->constant = symbol->value;
        return;
    case TB_SHADER_SYMBOL_FUNCTION:
        tb_shader_check_error(checker, node->location, "'%.*s' is a function, and must be called",
                              length, node->name.text);
        return;
    case TB_SHADER_SYMBOL_TYPE:
        tb_shader_check_error(checker, node->location, "'%.*s' is a type, not a value", length,
                              node->name.text);
        return;
    case TB_SHADER_SYMBOL_NAMESPACE:
        tb_shader_check_error(checker, node->location, "'%.*s' is a namespace, not a value", length,
                              node->name.text);
        return;
    }
}

static tb_shader_scalar integer_literal_scalar(tb_shader_name suffix)
{
    bool is_unsigned = false;
    bool is_long = false;
    for (size_t index = 0; index < suffix.length; index++)
    {
        const char byte = suffix.text[index];
        is_unsigned = is_unsigned || byte == 'u' || byte == 'U';
        is_long = is_long || byte == 'l' || byte == 'L';
    }
    if (is_long)
    {
        return is_unsigned ? TB_SHADER_SCALAR_UINT64 : TB_SHADER_SCALAR_INT64;
    }
    return is_unsigned ? TB_SHADER_SCALAR_UINT : TB_SHADER_SCALAR_LITERAL_INT;
}

static tb_shader_scalar float_literal_scalar(tb_shader_name suffix)
{
    if (suffix.length == 0)
    {
        return TB_SHADER_SCALAR_LITERAL_FLOAT;
    }
    const char byte = suffix.text[0];
    if (byte == 'h' || byte == 'H')
    {
        return TB_SHADER_SCALAR_HALF;
    }
    if (byte == 'l' || byte == 'L')
    {
        return TB_SHADER_SCALAR_DOUBLE;
    }
    return TB_SHADER_SCALAR_FLOAT;
}

static void check_literal(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node->kind == TB_SHADER_NODE_STRING)
    {
        node->checked = tb_shader_string_type(checker->types);
        return;
    }
    if (node->kind == TB_SHADER_NODE_BOOL)
    {
        node->checked = scalar(checker, TB_SHADER_SCALAR_BOOL);
        node->is_constant = true;
        node->constant.bits = node->integer;
        return;
    }
    if (node->kind == TB_SHADER_NODE_INTEGER)
    {
        const tb_shader_scalar component = integer_literal_scalar(node->suffix);
        node->checked = scalar(checker, component);
        node->is_constant = true;
        node->constant.bits =
            component == TB_SHADER_SCALAR_UINT ? (uint32_t)node->integer : node->integer;
        return;
    }
    const tb_shader_scalar component = float_literal_scalar(node->suffix);
    node->checked = scalar(checker, component);
    double value = 0;
    const size_t digits = node->name.length - node->suffix.length;
    if (tb_decimal_parse(node->name.text, digits, &value) == TB_DECIMAL_OK)
    {
        tb_shader_constant constant = {0};
        constant.is_float = true;
        constant.value = value;
        node->is_constant = true;
        node->constant = tb_shader_constant_convert(constant, TB_SHADER_SCALAR_DOUBLE, component);
    }
}

/* -------------------------------------------------------------------------
 * Operators.
 * ------------------------------------------------------------------------- */

static bool is_one_component(const tb_shader_type *type)
{
    return tb_shader_type_is_numeric(type) && tb_shader_component_count(type) == 1;
}

/* The shape two numeric operands combine to: the larger, when one is a
 * scalar; otherwise the smaller, with a warning that components are dropped. */
static const tb_shader_type *combined_shape(tb_shader_checker *checker, const tb_shader_type *left,
                                            const tb_shader_type *right,
                                            tb_shader_location location)
{
    if (is_one_component(left))
    {
        return right;
    }
    if (is_one_component(right))
    {
        return left;
    }
    if (left->type_class != right->type_class)
    {
        tb_shader_check_error(checker, location, "cannot combine '%s' and '%s'", left->name,
                              right->name);
        return NULL;
    }
    const uint32_t rows = left->rows < right->rows ? left->rows : right->rows;
    const uint32_t columns = left->columns < right->columns ? left->columns : right->columns;
    if (rows != left->rows || rows != right->rows || columns != left->columns ||
        columns != right->columns)
    {
        tb_shader_check_warning(checker, location,
                                "'%s' and '%s' are combined as their first %u components",
                                left->name, right->name, (unsigned)(rows * columns));
    }
    if (left->type_class == TB_SHADER_CLASS_MATRIX)
    {
        return tb_shader_matrix_type(checker->types, left->scalar, rows, columns);
    }
    return tb_shader_vector_type(checker->types, left->scalar, columns);
}

/* The type both operands of an arithmetic, bitwise or comparison operator
 * are converted to, or NULL after an error. */
static const tb_shader_type *operand_type(tb_shader_checker *checker, tb_shader_node *node,
                                          const tb_shader_type *left, const tb_shader_type *right)
{
    left = tb_shader_check_arithmetic_view(checker, left);
    right = tb_shader_check_arithmetic_view(checker, right);
    if (!tb_shader_type_is_numeric(left) || !tb_shader_type_is_numeric(right))
    {
        tb_shader_check_error(checker, node->location, "'%s' cannot be applied to '%s' and '%s'",
                              tb_shader_operator_spelling(node->op), left->name, right->name);
        return NULL;
    }
    const tb_shader_type *const shape = combined_shape(checker, left, right, node->location);
    if (shape == NULL)
    {
        return NULL;
    }
    const tb_shader_scalar component = tb_shader_common_scalar(left->scalar, right->scalar);
    return tb_shader_shaped_type(checker->types, component, shape);
}

static bool is_comparison(tb_shader_operator op)
{
    return op == TB_SHADER_OP_LESS || op == TB_SHADER_OP_GREATER || op == TB_SHADER_OP_LESS_EQUAL ||
           op == TB_SHADER_OP_GREATER_EQUAL || op == TB_SHADER_OP_EQUAL ||
           op == TB_SHADER_OP_NOT_EQUAL;
}

static bool is_bitwise(tb_shader_operator op)
{
    return op == TB_SHADER_OP_BIT_AND || op == TB_SHADER_OP_BIT_OR || op == TB_SHADER_OP_BIT_XOR ||
           op == TB_SHADER_OP_SHIFT_LEFT || op == TB_SHADER_OP_SHIFT_RIGHT;
}

static bool is_logical(tb_shader_operator op)
{
    return op == TB_SHADER_OP_AND || op == TB_SHADER_OP_OR;
}

/* `&&` and `||` take scalars in HLSL 2021; vectors use and() and or(). */
static tb_shader_node *logical_operand(tb_shader_checker *checker, tb_shader_node *operand)
{
    const tb_shader_type *const type = tb_shader_check_arithmetic_view(checker, type_of(checker, operand));
    if (is_error(type))
    {
        return operand;
    }
    if (!is_one_component(type))
    {
        tb_shader_check_error(checker, operand->location,
                              "'&&' and '||' take scalars, not '%s'; use and() or or() for vectors",
                              type->name);
        return operand;
    }
    return convert_to(checker, operand, scalar(checker, TB_SHADER_SCALAR_BOOL), true);
}

/* The type `left op right` has, its operands converted; the error type after an error. */
static const tb_shader_type *binary_result(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_type *const left = type_of(checker, node->left);
    const tb_shader_type *const right = type_of(checker, node->right);
    if (node->op == TB_SHADER_OP_COMMA)
    {
        return right;
    }
    if (is_logical(node->op))
    {
        node->left = logical_operand(checker, node->left);
        node->right = logical_operand(checker, node->right);
        return scalar(checker, TB_SHADER_SCALAR_BOOL);
    }
    if (is_error(left) || is_error(right))
    {
        return error_type(checker);
    }
    const tb_shader_type *const operands = operand_type(checker, node, left, right);
    if (operands == NULL)
    {
        return error_type(checker);
    }
    if (is_bitwise(node->op) && tb_shader_scalar_is_float(operands->scalar))
    {
        tb_shader_check_error(checker, node->location, "'%s' takes integers, not '%s'",
                              tb_shader_operator_spelling(node->op), operands->name);
        return error_type(checker);
    }
    node->left = convert_to(checker, node->left, operands, false);
    node->right = convert_to(checker, node->right, operands, false);
    if (is_comparison(node->op))
    {
        return tb_shader_shaped_type(checker->types, TB_SHADER_SCALAR_BOOL, operands);
    }
    return operands;
}

static void check_binary(tb_shader_checker *checker, tb_shader_node *node)
{
    node->left = tb_shader_check_expression(checker, node->left);
    node->right = tb_shader_check_expression(checker, node->right);
    node->checked = binary_result(checker, node);
    tb_shader_fold(checker, node);
}

static void check_unary(tb_shader_checker *checker, tb_shader_node *node)
{
    node->left = tb_shader_check_expression(checker, node->left);
    const tb_shader_type *type = tb_shader_check_arithmetic_view(checker, type_of(checker, node->left));
    node->checked = error_type(checker);
    if (is_error(type))
    {
        return;
    }
    const char *const spelling = tb_shader_operator_spelling(node->op);
    if (!tb_shader_type_is_numeric(type))
    {
        tb_shader_check_error(checker, node->location, "'%s' cannot be applied to '%s'", spelling,
                              type->name);
        return;
    }
    const tb_shader_operator op = node->op;
    const bool is_step = op == TB_SHADER_OP_PRE_INCREMENT || op == TB_SHADER_OP_PRE_DECREMENT ||
                         op == TB_SHADER_OP_POST_INCREMENT || op == TB_SHADER_OP_POST_DECREMENT;
    if (is_step)
    {
        if (type->scalar == TB_SHADER_SCALAR_BOOL)
        {
            tb_shader_check_error(checker, node->location, "'%s' cannot be applied to '%s'",
                                  spelling, type->name);
            return;
        }
        if (!node->left->is_assignable)
        {
            tb_shader_check_error(checker, node->location,
                                  "'%s' needs a variable that can be assigned", spelling);
        }
        node->checked = type;
        return;
    }
    if (op == TB_SHADER_OP_NOT)
    {
        node->checked = tb_shader_shaped_type(checker->types, TB_SHADER_SCALAR_BOOL, type);
        node->left = convert_to(checker, node->left, node->checked, false);
    }
    else if (op == TB_SHADER_OP_BIT_NOT)
    {
        if (!tb_shader_scalar_is_integer(type->scalar))
        {
            tb_shader_check_error(checker, node->location, "'~' takes integers, not '%s'",
                                  type->name);
            return;
        }
        node->checked = type;
    }
    else
    {
        /* `+` and `-`: bools take part as ints. */
        if (type->scalar == TB_SHADER_SCALAR_BOOL)
        {
            type = tb_shader_shaped_type(checker->types, TB_SHADER_SCALAR_INT, type);
            node->left = convert_to(checker, node->left, type, false);
        }
        node->checked = type;
    }
    tb_shader_fold(checker, node);
}

/* Why a target cannot be assigned, from the innermost reason out. */
static void explain_unassignable(tb_shader_checker *checker, const tb_shader_node *target)
{
    const tb_shader_node *root = target;
    while (root->kind == TB_SHADER_NODE_MEMBER || root->kind == TB_SHADER_NODE_INDEX)
    {
        if (root->kind == TB_SHADER_NODE_MEMBER && root->swizzle_count > 0 &&
            root->left->is_assignable)
        {
            tb_shader_check_error(checker, root->location,
                                  "a swizzle that repeats a component cannot be assigned");
            return;
        }
        const tb_shader_type *const base = type_of(checker, root->left);
        if (root->kind == TB_SHADER_NODE_INDEX && base->type_class == TB_SHADER_CLASS_OBJECT)
        {
            tb_shader_check_error(checker, root->location, "'%s' is read-only", base->name);
            return;
        }
        root = root->left;
    }
    if (root->kind == TB_SHADER_NODE_NAME && root->symbol != NULL)
    {
        const tb_shader_symbol *const symbol = root->symbol;
        const char *const what =
            symbol->storage == TB_SHADER_STORAGE_UNIFORM ? "a shader input" : "const";
        tb_shader_check_error(checker, root->location, "'%.*s' is %s, and cannot be assigned",
                              name_length(symbol->name), symbol->name.text, what);
        return;
    }
    tb_shader_check_error(checker, target->location, "this expression cannot be assigned");
}

/* The binary operator of a compound assignment: `+` of `+=`. */
static tb_shader_operator compound_operator(tb_shader_operator op)
{
    static const tb_shader_operator pairs[][2] = {
        {TB_SHADER_OP_MULTIPLY_ASSIGN, TB_SHADER_OP_MULTIPLY},
        {TB_SHADER_OP_DIVIDE_ASSIGN, TB_SHADER_OP_DIVIDE},
        {TB_SHADER_OP_REMAINDER_ASSIGN, TB_SHADER_OP_REMAINDER},
        {TB_SHADER_OP_ADD_ASSIGN, TB_SHADER_OP_ADD},
        {TB_SHADER_OP_SUBTRACT_ASSIGN, TB_SHADER_OP_SUBTRACT},
        {TB_SHADER_OP_SHIFT_LEFT_ASSIGN, TB_SHADER_OP_SHIFT_LEFT},
        {TB_SHADER_OP_SHIFT_RIGHT_ASSIGN, TB_SHADER_OP_SHIFT_RIGHT},
        {TB_SHADER_OP_BIT_AND_ASSIGN, TB_SHADER_OP_BIT_AND},
        {TB_SHADER_OP_BIT_XOR_ASSIGN, TB_SHADER_OP_BIT_XOR},
        {TB_SHADER_OP_BIT_OR_ASSIGN, TB_SHADER_OP_BIT_OR},
    };
    for (size_t index = 0; index < sizeof(pairs) / sizeof(pairs[0]); index++)
    {
        if (pairs[index][0] == op)
        {
            return pairs[index][1];
        }
    }
    return TB_SHADER_OP_NONE;
}

static void check_assign(tb_shader_checker *checker, tb_shader_node *node)
{
    node->left = tb_shader_check_expression(checker, node->left);
    node->right = tb_shader_check_expression(checker, node->right);
    const tb_shader_type *const target = type_of(checker, node->left);
    node->checked = target;
    if (node->left != NULL && !is_error(target) && !node->left->is_assignable)
    {
        explain_unassignable(checker, node->left);
    }
    if (node->op == TB_SHADER_OP_ASSIGN)
    {
        node->right = tb_shader_check_convert(checker, node->right, target);
        return;
    }
    /* `a += b` is `a = a + b`: the operation decides the type, then the
     * result converts back to a's. */
    tb_shader_node operation = {0};
    operation.kind = TB_SHADER_NODE_BINARY;
    operation.location = node->location;
    operation.op = compound_operator(node->op);
    operation.left = node->left;
    operation.right = node->right;
    const tb_shader_type *const result = binary_result(checker, &operation);
    node->right = operation.right;
    if (is_error(result) || is_error(target))
    {
        return;
    }
    const conversion back = classify(checker, result, target, false);
    if (back == CONVERSION_NONE)
    {
        tb_shader_check_error(checker, node->location, "cannot convert from '%s' to '%s'",
                              result->name, target->name);
    }
    else if (back == CONVERSION_TRUNCATES)
    {
        tb_shader_check_warning(checker, node->location, "'%s' is truncated to '%s'", result->name,
                                target->name);
    }
}

static void check_conditional(tb_shader_checker *checker, tb_shader_node *node)
{
    node->condition = tb_shader_check_expression(checker, node->condition);
    const tb_shader_type *const condition =
        tb_shader_check_arithmetic_view(checker, type_of(checker, node->condition));
    if (!is_error(condition) && !is_one_component(condition))
    {
        tb_shader_check_error(
            checker, node->condition->location,
            "the condition of '?:' must be a scalar, not '%s'; use select() for vectors",
            condition->name);
    }
    else
    {
        node->condition =
            convert_to(checker, node->condition, scalar(checker, TB_SHADER_SCALAR_BOOL), true);
    }
    node->left = tb_shader_check_expression(checker, node->left);
    node->right = tb_shader_check_expression(checker, node->right);
    const tb_shader_type *const left = type_of(checker, node->left);
    const tb_shader_type *const right = type_of(checker, node->right);
    if (is_error(left) || is_error(right))
    {
        node->checked = error_type(checker);
        return;
    }
    if (left == right)
    {
        node->checked = left;
    }
    else if (tb_shader_type_is_numeric(tb_shader_check_arithmetic_view(checker, left)) &&
             tb_shader_type_is_numeric(tb_shader_check_arithmetic_view(checker, right)))
    {
        const tb_shader_type *const common = operand_type(checker, node, left, right);
        node->checked = common != NULL ? common : error_type(checker);
        if (common != NULL)
        {
            node->left = convert_to(checker, node->left, common, false);
            node->right = convert_to(checker, node->right, common, false);
        }
    }
    else
    {
        tb_shader_check_error(checker, node->location, "the branches of '?:' differ: '%s' and '%s'",
                              left->name, right->name);
        node->checked = error_type(checker);
    }
    tb_shader_fold(checker, node);
}

/* -------------------------------------------------------------------------
 * Lists of expressions.
 * ------------------------------------------------------------------------- */

/* Check each node of a list, keeping what replaces it. */
void tb_shader_check_list(tb_shader_checker *checker, tb_shader_node **first)
{
    for (tb_shader_node **link = first; *link != NULL; link = &(*link)->next)
    {
        tb_shader_node *const next = (*link)->next;
        (*link)->next = NULL;
        tb_shader_node *const checked = tb_shader_check_expression(checker, *link);
        checked->next = next;
        *link = checked;
    }
}

/* -------------------------------------------------------------------------
 * Overloads: what an argument costs.
 * ------------------------------------------------------------------------- */

/* How far a scalar is from another, for choosing an overload: 0 the same,
 * 1 a literal or a wider type of the same kind, 2 a narrower one, 3 another
 * kind. */
static int scalar_cost(tb_shader_scalar from, tb_shader_scalar to)
{
    if (from == to)
    {
        return 0;
    }
    if (from == TB_SHADER_SCALAR_LITERAL_INT)
    {
        if (to == TB_SHADER_SCALAR_INT)
        {
            return 0;
        }
        return tb_shader_scalar_is_integer(to) ? 1 : 3;
    }
    if (from == TB_SHADER_SCALAR_LITERAL_FLOAT)
    {
        if (to == TB_SHADER_SCALAR_FLOAT)
        {
            return 0;
        }
        return tb_shader_scalar_is_float(to) ? 1 : 3;
    }
    const bool same_kind = (tb_shader_scalar_is_integer(from) && tb_shader_scalar_is_integer(to)) ||
                           (tb_shader_scalar_is_float(from) && tb_shader_scalar_is_float(to));
    if (!same_kind)
    {
        return 3;
    }
    return tb_shader_common_scalar(from, to) == to ? 1 : 2;
}

/* The cost of passing a `from` where a `to` is taken, or -1 when it cannot be. */
int tb_shader_check_argument_cost(tb_shader_checker *checker, const tb_shader_type *from,
                                  const tb_shader_type *to)
{
    const conversion kind = classify(checker, from, to, false);
    if (kind == CONVERSION_NONE)
    {
        return -1;
    }
    if (kind == CONVERSION_IDENTITY)
    {
        return 0;
    }
    const tb_shader_type *const view = tb_shader_check_arithmetic_view(checker, from);
    int shape = 0;
    if (kind == CONVERSION_TRUNCATES)
    {
        /* Dropping all but one component is worse than dropping some. */
        shape = to->type_class == TB_SHADER_CLASS_SCALAR ? 4 : 3;
    }
    else if (tb_shader_component_count(view) == 1 &&
             (tb_shader_component_count(to) > 1 || view->type_class != to->type_class))
    {
        /* A scalar made a vector or a matrix, `float1` included. */
        shape = 1;
    }
    else if (view->type_class != to->type_class)
    {
        shape = 2;
    }
    return shape * 10 + scalar_cost(view->scalar, to->scalar);
}

bool tb_shader_check_converts(tb_shader_checker *checker, const tb_shader_type *from,
                              const tb_shader_type *to)
{
    return classify(checker, from, to, false) != CONVERSION_NONE;
}

/* -------------------------------------------------------------------------
 * Constructors, casts and initializers.
 * ------------------------------------------------------------------------- */

static void check_construct(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_type *const type = tb_shader_check_type(checker, node->type);
    tb_shader_check_list(checker, &node->children);
    node->checked = type;
    if (is_error(type))
    {
        return;
    }
    if (!tb_shader_type_is_numeric(type))
    {
        tb_shader_check_error(checker, node->location,
                              "'%s' cannot be built from values; use a cast or an initializer list",
                              type->name);
        node->checked = error_type(checker);
        return;
    }
    uint32_t total = 0;
    for (tb_shader_node **link = &node->children; *link != NULL; link = &(*link)->next)
    {
        const tb_shader_type *const argument = tb_shader_check_arithmetic_view(checker, type_of(checker, *link));
        if (is_error(argument))
        {
            return;
        }
        if (!tb_shader_type_is_numeric(argument))
        {
            tb_shader_check_error(checker, (*link)->location, "'%s' cannot build a '%s'",
                                  argument->name, type->name);
            return;
        }
        total += tb_shader_component_count(argument);
        *link = convert_to(checker, *link,
                           tb_shader_shaped_type(checker->types, type->scalar, argument), false);
    }
    const uint32_t wanted = tb_shader_component_count(type);
    if (total != wanted)
    {
        tb_shader_check_error(checker, node->location, "'%s' needs %u components, but %u %s given",
                              type->name, (unsigned)wanted, (unsigned)total,
                              total == 1 ? "was" : "were");
        return;
    }
    tb_shader_fold(checker, node);
}

static void check_cast(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_type *const type = tb_shader_check_type(checker, node->type);
    node->left = tb_shader_check_expression(checker, node->left);
    const tb_shader_type *const from = type_of(checker, node->left);
    node->checked = type;
    if (classify(checker, from, type, true) == CONVERSION_NONE)
    {
        tb_shader_check_error(checker, node->location, "cannot cast from '%s' to '%s'", from->name,
                              type->name);
        return;
    }
    tb_shader_fold(checker, node);
}

/* The scalar values of an initializer list, its nested lists flattened; 0
 * after an error. */
static uint32_t count_initializer(tb_shader_checker *checker, tb_shader_node *list, bool *ok)
{
    uint32_t total = 0;
    list->checked = error_type(checker);
    for (tb_shader_node **link = &list->children; *link != NULL; link = &(*link)->next)
    {
        uint32_t count = 0;
        if ((*link)->kind == TB_SHADER_NODE_INITIALIZER_LIST)
        {
            count = count_initializer(checker, *link, ok);
        }
        else
        {
            tb_shader_node *const next = (*link)->next;
            (*link)->next = NULL;
            tb_shader_node *const checked = tb_shader_check_expression(checker, *link);
            checked->next = next;
            *link = checked;
            const tb_shader_type *const type = type_of(checker, checked);
            count = tb_shader_flat_count(type);
            if (count == 0 && !is_error(type))
            {
                tb_shader_check_error(checker, checked->location,
                                      "'%s' cannot be part of an initializer list", type->name);
            }
            *ok = *ok && count > 0;
        }
        total = total + count < total ? UINT32_MAX : total + count;
    }
    return total;
}

tb_shader_node *tb_shader_check_initializer(tb_shader_checker *checker, tb_shader_node *node,
                                            const tb_shader_type *type, uint32_t *length)
{
    *length = 0;
    if (node == NULL)
    {
        return NULL;
    }
    if (node->kind != TB_SHADER_NODE_INITIALIZER_LIST)
    {
        tb_shader_node *const value = tb_shader_check_expression(checker, node);
        const tb_shader_type *const from = type_of(checker, value);
        if (type->type_class == TB_SHADER_CLASS_ARRAY && type->length == 0 &&
            from->type_class == TB_SHADER_CLASS_ARRAY && from->element == type->element)
        {
            *length = from->length;
            return value;
        }
        return tb_shader_check_convert(checker, value, type);
    }
    bool ok = true;
    const uint32_t total = count_initializer(checker, node, &ok);
    node->checked = type;
    if (!ok || is_error(type))
    {
        return node;
    }
    const bool is_unsized = type->type_class == TB_SHADER_CLASS_ARRAY && type->length == 0;
    if (is_unsized)
    {
        const uint32_t element = tb_shader_flat_count(type->element);
        if (element == 0 || total == 0 || total % element != 0)
        {
            tb_shader_check_error(checker, node->location,
                                  "%u values do not make whole elements of '%s'", (unsigned)total,
                                  type->element->name);
            return node;
        }
        *length = total / element;
        node->checked = tb_shader_array_type(checker->types, type->element, *length);
        return node;
    }
    const uint32_t wanted = tb_shader_flat_count(type);
    if (wanted == 0)
    {
        tb_shader_check_error(checker, node->location, "'%s' cannot be initialized by a list",
                              type->name);
    }
    else if (total != wanted)
    {
        tb_shader_check_error(checker, node->location, "'%s' needs %u values, but %u %s given",
                              type->name, (unsigned)wanted, (unsigned)total,
                              total == 1 ? "was" : "were");
    }
    return node;
}

/* -------------------------------------------------------------------------
 * Indexing and members.
 * ------------------------------------------------------------------------- */

static bool constant_out_of_range(const tb_shader_node *index, uint32_t length)
{
    return index != NULL && index->is_constant && !index->constant.is_float &&
           index->constant.bits >= length;
}

/* The element `object[index]` reads, and the index's type, for an object. */
static const tb_shader_type *object_element(tb_shader_checker *checker, tb_shader_node *node,
                                            const tb_shader_type *object)
{
    const tb_shader_object_info *const info = tb_shader_object_info_of(object->object);
    if (info == NULL || info->index_dimensions == 0 || object->element == NULL)
    {
        tb_shader_check_error(checker, node->location, "'%s' cannot be indexed", object->name);
        return error_type(checker);
    }
    const tb_shader_type *const index_type =
        info->index_dimensions == 1
            ? scalar(checker, TB_SHADER_SCALAR_UINT)
            : tb_shader_vector_type(checker->types, TB_SHADER_SCALAR_UINT, info->index_dimensions);
    node->right = tb_shader_check_convert(checker, node->right, index_type);
    node->is_lvalue = true;
    node->is_assignable = info->is_writable;
    return object->element;
}

static void check_index(tb_shader_checker *checker, tb_shader_node *node)
{
    node->left = tb_shader_check_expression(checker, node->left);
    node->right = tb_shader_check_expression(checker, node->right);
    const tb_shader_type *const base = type_of(checker, node->left);
    const tb_shader_type *const index = tb_shader_check_arithmetic_view(checker, type_of(checker, node->right));
    node->checked = error_type(checker);
    if (is_error(base) || is_error(index))
    {
        return;
    }
    if (base->type_class == TB_SHADER_CLASS_OBJECT)
    {
        node->checked = object_element(checker, node, base);
        return;
    }
    if (!is_one_component(index))
    {
        tb_shader_check_error(checker, node->right->location,
                              "an index must be an integer, not '%s'", index->name);
        return;
    }
    uint32_t length = 0;
    if (base->type_class == TB_SHADER_CLASS_ARRAY)
    {
        node->checked = base->element;
        length = base->length;
    }
    else if (base->type_class == TB_SHADER_CLASS_VECTOR)
    {
        node->checked = scalar(checker, base->scalar);
        length = base->columns;
    }
    else if (base->type_class == TB_SHADER_CLASS_MATRIX)
    {
        node->checked = tb_shader_vector_type(checker->types, base->scalar, base->columns);
        length = base->rows;
    }
    else
    {
        tb_shader_check_error(checker, node->location, "'%s' cannot be indexed", base->name);
        return;
    }
    node->right = convert_to(checker, node->right, scalar(checker, TB_SHADER_SCALAR_UINT), true);
    if (length > 0 && constant_out_of_range(node->right, length))
    {
        tb_shader_check_error(checker, node->right->location, "index %llu is past the end of '%s'",
                              (unsigned long long)node->right->constant.bits, base->name);
    }
    node->is_lvalue = node->left->is_lvalue;
    node->is_assignable = node->left->is_assignable;
}

/* The component a swizzle letter selects, or -1; `set` is 0 for xyzw, 1 for rgba. */
static int swizzle_component(char letter, int *set)
{
    static const char sets[2][5] = {"xyzw", "rgba"};
    for (int which = 0; which < 2; which++)
    {
        for (int index = 0; index < 4; index++)
        {
            if (sets[which][index] == letter)
            {
                *set = which;
                return index;
            }
        }
    }
    return -1;
}

static bool check_vector_swizzle(tb_shader_checker *checker, tb_shader_node *node,
                                 const tb_shader_type *base)
{
    const tb_shader_name name = node->name;
    if (name.length > 4)
    {
        tb_shader_check_error(checker, node->location, "a swizzle selects at most 4 components");
        return false;
    }
    int first_set = -1;
    for (size_t index = 0; index < name.length; index++)
    {
        int set = 0;
        const int component = swizzle_component(name.text[index], &set);
        if (component < 0)
        {
            return false;
        }
        if (first_set >= 0 && set != first_set)
        {
            tb_shader_check_error(checker, node->location, "a swizzle cannot mix xyzw and rgba");
            return false;
        }
        first_set = set;
        if ((uint32_t)component >= base->columns)
        {
            tb_shader_check_error(checker, node->location, "'%c' is out of range for '%s'",
                                  name.text[index], base->name);
            return false;
        }
        node->swizzle[index] = (uint8_t)component;
    }
    node->swizzle_count = (uint8_t)name.length;
    return true;
}

/* `_m01` and `_12`: one component of a matrix, zero- or one-based. */
static size_t matrix_component(const char *text, size_t length, int *row, int *column)
{
    if (length >= 4 && text[0] == '_' && text[1] == 'm')
    {
        *row = text[2] - '0';
        *column = text[3] - '0';
        return 4;
    }
    if (length >= 3 && text[0] == '_')
    {
        *row = text[1] - '1';
        *column = text[2] - '1';
        return 3;
    }
    return 0;
}

static bool check_matrix_swizzle(tb_shader_checker *checker, tb_shader_node *node,
                                 const tb_shader_type *base)
{
    const tb_shader_name name = node->name;
    size_t offset = 0;
    uint8_t count = 0;
    while (offset < name.length)
    {
        int row = -1;
        int column = -1;
        const size_t used =
            matrix_component(name.text + offset, name.length - offset, &row, &column);
        if (used == 0 || count == 4)
        {
            return false;
        }
        if (row < 0 || column < 0 || (uint32_t)row >= base->rows ||
            (uint32_t)column >= base->columns)
        {
            tb_shader_check_error(checker, node->location, "'%.*s' is out of range for '%s'",
                                  (int)used, name.text + offset, base->name);
            node->swizzle_count = 0;
            return true;
        }
        node->swizzle[count++] = (uint8_t)(row * 4 + column);
        offset += used;
    }
    node->swizzle_count = count;
    return count > 0;
}

static bool has_duplicates(const tb_shader_node *node)
{
    for (uint8_t a = 0; a < node->swizzle_count; a++)
    {
        for (uint8_t b = (uint8_t)(a + 1); b < node->swizzle_count; b++)
        {
            if (node->swizzle[a] == node->swizzle[b])
            {
                return true;
            }
        }
    }
    return false;
}

static void check_swizzle(tb_shader_checker *checker, tb_shader_node *node,
                          const tb_shader_type *base)
{
    const bool is_matrix = base->type_class == TB_SHADER_CLASS_MATRIX;
    const bool valid = is_matrix ? check_matrix_swizzle(checker, node, base)
                                 : check_vector_swizzle(checker, node, base);
    if (!valid)
    {
        if (node->swizzle_count == 0 && checker->stopped == false)
        {
            tb_shader_check_error(checker, node->location, "'%s' has no member '%.*s'", base->name,
                                  name_length(node->name), node->name.text);
        }
        node->swizzle_count = 0;
        return;
    }
    if (node->swizzle_count == 0)
    {
        return;
    }
    node->checked = node->swizzle_count == 1
                        ? scalar(checker, base->scalar)
                        : tb_shader_vector_type(checker->types, base->scalar, node->swizzle_count);
    const bool repeats = has_duplicates(node);
    node->is_lvalue = node->left->is_lvalue && !repeats;
    node->is_assignable = node->left->is_assignable && !repeats;
    tb_shader_fold(checker, node);
}

static void check_field_access(tb_shader_checker *checker, tb_shader_node *node,
                               const tb_shader_type *structure, bool through_buffer)
{
    tb_shader_symbol *const member = tb_shader_find_member(structure, node->name);
    if (member == NULL)
    {
        tb_shader_check_error(checker, node->location, "'%s' has no member '%.*s'", structure->name,
                              name_length(node->name), node->name.text);
        return;
    }
    if (member->kind == TB_SHADER_SYMBOL_FUNCTION)
    {
        tb_shader_check_error(checker, node->location, "'%.*s' is a method, and must be called",
                              name_length(node->name), node->name.text);
        return;
    }
    node->symbol = member;
    node->checked = member->type;
    node->is_lvalue = through_buffer || node->left->is_lvalue;
    node->is_assignable = !through_buffer && node->left->is_assignable;
}

static void check_member(tb_shader_checker *checker, tb_shader_node *node)
{
    node->left = tb_shader_check_expression(checker, node->left);
    const tb_shader_type *const base = tb_shader_check_arithmetic_view(checker, type_of(checker, node->left));
    node->checked = error_type(checker);
    if (is_error(base))
    {
        return;
    }
    if (base->type_class == TB_SHADER_CLASS_STRUCT)
    {
        check_field_access(checker, node, base, false);
        return;
    }
    const bool is_buffer = base->type_class == TB_SHADER_CLASS_OBJECT &&
                           (base->object == TB_SHADER_OBJECT_CONSTANT_BUFFER ||
                            base->object == TB_SHADER_OBJECT_TEXTURE_BUFFER);
    if (is_buffer)
    {
        check_field_access(checker, node, base->element, true);
        return;
    }
    if (tb_shader_type_is_numeric(base))
    {
        check_swizzle(checker, node, base);
        return;
    }
    tb_shader_check_error(checker, node->location, "'%s' has no member '%.*s'", base->name,
                          name_length(node->name), node->name.text);
}

/* -------------------------------------------------------------------------
 * sizeof.
 * ------------------------------------------------------------------------- */

static uint32_t scalar_size(tb_shader_scalar component)
{
    switch (component)
    {
    case TB_SHADER_SCALAR_INT16:
    case TB_SHADER_SCALAR_UINT16:
        return 2;
    case TB_SHADER_SCALAR_INT64:
    case TB_SHADER_SCALAR_UINT64:
    case TB_SHADER_SCALAR_DOUBLE:
        return 8;
    case TB_SHADER_SCALAR_BOOL:
    case TB_SHADER_SCALAR_INT:
    case TB_SHADER_SCALAR_UINT:
    case TB_SHADER_SCALAR_MIN16INT:
    case TB_SHADER_SCALAR_MIN16UINT:
    case TB_SHADER_SCALAR_MIN16FLOAT:
    case TB_SHADER_SCALAR_HALF:
    case TB_SHADER_SCALAR_FLOAT:
    case TB_SHADER_SCALAR_LITERAL_INT:
    case TB_SHADER_SCALAR_LITERAL_FLOAT:
        return 4;
    }
    return 4;
}

/* Bytes of a type laid out as a structured buffer lays it out: each scalar
 * aligned to its size; 0 for a type with no size. */
static uint32_t type_size(const tb_shader_type *type, uint32_t *alignment)
{
    *alignment = 1;
    if (tb_shader_type_is_numeric(type) || type->type_class == TB_SHADER_CLASS_ENUM)
    {
        *alignment = scalar_size(type->scalar);
        return tb_shader_component_count(type) * *alignment;
    }
    if (type->type_class == TB_SHADER_CLASS_ARRAY)
    {
        return type_size(type->element, alignment) * type->length;
    }
    if (type->type_class != TB_SHADER_CLASS_STRUCT)
    {
        return 0;
    }
    uint32_t offset = 0;
    for (size_t index = 0; index < type->structure->field_count; index++)
    {
        uint32_t field_alignment = 1;
        const uint32_t size = type_size(type->structure->fields[index]->type, &field_alignment);
        offset = (offset + field_alignment - 1) / field_alignment * field_alignment + size;
        *alignment = field_alignment > *alignment ? field_alignment : *alignment;
    }
    return (offset + *alignment - 1) / *alignment * *alignment;
}

static void check_sizeof(tb_shader_checker *checker, tb_shader_node *node)
{
    const tb_shader_type *type = NULL;
    if (node->type != NULL)
    {
        type = tb_shader_check_type(checker, node->type);
    }
    else
    {
        node->left = tb_shader_check_expression(checker, node->left);
        type = type_of(checker, node->left);
    }
    node->checked = scalar(checker, TB_SHADER_SCALAR_UINT);
    if (is_error(type))
    {
        return;
    }
    uint32_t alignment = 1;
    const uint32_t size = type_size(type, &alignment);
    if (size == 0)
    {
        tb_shader_check_error(checker, node->location, "'%s' has no size", type->name);
        return;
    }
    node->is_constant = true;
    node->constant.bits = size;
}

/* -------------------------------------------------------------------------
 * Expressions.
 * ------------------------------------------------------------------------- */

static void check_kind(tb_shader_checker *checker, tb_shader_node *node)
{
    switch (node->kind)
    {
    case TB_SHADER_NODE_INTEGER:
    case TB_SHADER_NODE_FLOAT:
    case TB_SHADER_NODE_BOOL:
    case TB_SHADER_NODE_STRING:
        check_literal(checker, node);
        break;
    case TB_SHADER_NODE_NAME:
        check_name(checker, node);
        if (node->template_arguments != NULL)
        {
            tb_shader_check_error(checker, node->location, "templates are not supported yet");
        }
        break;
    case TB_SHADER_NODE_UNARY:
        check_unary(checker, node);
        break;
    case TB_SHADER_NODE_BINARY:
        check_binary(checker, node);
        break;
    case TB_SHADER_NODE_ASSIGN:
        check_assign(checker, node);
        break;
    case TB_SHADER_NODE_CONDITIONAL:
        check_conditional(checker, node);
        break;
    case TB_SHADER_NODE_CALL:
        tb_shader_check_call(checker, node);
        break;
    case TB_SHADER_NODE_CONSTRUCT:
        check_construct(checker, node);
        break;
    case TB_SHADER_NODE_CAST:
        check_cast(checker, node);
        break;
    case TB_SHADER_NODE_INDEX:
        check_index(checker, node);
        break;
    case TB_SHADER_NODE_MEMBER:
        check_member(checker, node);
        break;
    case TB_SHADER_NODE_SIZEOF:
        check_sizeof(checker, node);
        break;
    case TB_SHADER_NODE_INITIALIZER_LIST:
        tb_shader_check_error(checker, node->location,
                              "an initializer list is allowed only as an initializer");
        node->checked = error_type(checker);
        break;
    case TB_SHADER_NODE_CONVERT:
        break;
    case TB_SHADER_NODE_STRUCT:
    case TB_SHADER_NODE_INTERFACE:
    case TB_SHADER_NODE_FUNCTION:
    case TB_SHADER_NODE_VARIABLE:
    case TB_SHADER_NODE_PARAMETER:
    case TB_SHADER_NODE_CBUFFER:
    case TB_SHADER_NODE_TBUFFER:
    case TB_SHADER_NODE_TYPEDEF:
    case TB_SHADER_NODE_NAMESPACE:
    case TB_SHADER_NODE_ENUM:
    case TB_SHADER_NODE_ENUMERATOR:
    case TB_SHADER_NODE_TEMPLATE_PARAMETER:
    case TB_SHADER_NODE_PRAGMA:
    case TB_SHADER_NODE_BLOCK:
    case TB_SHADER_NODE_DECLARATION:
    case TB_SHADER_NODE_EXPRESSION_STATEMENT:
    case TB_SHADER_NODE_IF:
    case TB_SHADER_NODE_FOR:
    case TB_SHADER_NODE_WHILE:
    case TB_SHADER_NODE_DO:
    case TB_SHADER_NODE_SWITCH:
    case TB_SHADER_NODE_CASE:
    case TB_SHADER_NODE_DEFAULT:
    case TB_SHADER_NODE_BREAK:
    case TB_SHADER_NODE_CONTINUE:
    case TB_SHADER_NODE_RETURN:
    case TB_SHADER_NODE_DISCARD:
    case TB_SHADER_NODE_EMPTY:
        /* Not expressions: the parser makes none of these one. */
        node->checked = error_type(checker);
        break;
    }
}

tb_shader_node *tb_shader_check_expression(tb_shader_checker *checker, tb_shader_node *node)
{
    if (node == NULL)
    {
        return NULL;
    }
    if (node->checked != NULL)
    {
        return node;
    }
    check_kind(checker, node);
    if (node->checked == NULL)
    {
        node->checked = error_type(checker);
    }
    return node;
}

tb_shader_node *tb_shader_check_condition(tb_shader_checker *checker, tb_shader_node *node,
                                          const char *what)
{
    tb_shader_node *const checked = tb_shader_check_expression(checker, node);
    const tb_shader_type *const type = tb_shader_check_arithmetic_view(checker, type_of(checker, checked));
    if (checked == NULL || is_error(type))
    {
        return checked;
    }
    if (!is_one_component(type))
    {
        tb_shader_check_error(checker, checked->location,
                              "the condition of %s must be a scalar, not '%s'", what, type->name);
        return checked;
    }
    return convert_to(checker, checked, scalar(checker, TB_SHADER_SCALAR_BOOL), true);
}

bool tb_shader_check_constant_integer(tb_shader_checker *checker, tb_shader_node *node,
                                      int64_t *value)
{
    tb_shader_node *const checked = tb_shader_check_expression(checker, node);
    if (checked == NULL)
    {
        return false;
    }
    const tb_shader_type *const type = tb_shader_check_arithmetic_view(checker, type_of(checker, checked));
    if (is_error(type))
    {
        return false;
    }
    const bool is_integer =
        type->type_class == TB_SHADER_CLASS_SCALAR &&
        (tb_shader_scalar_is_integer(type->scalar) || type->scalar == TB_SHADER_SCALAR_BOOL);
    if (!is_integer || !checked->is_constant)
    {
        tb_shader_check_error(checker, checked->location, "expected a constant integer");
        return false;
    }
    *value = (int64_t)checked->constant.bits;
    return true;
}
