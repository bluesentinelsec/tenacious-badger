/* Checking that a form keeps its rules (shader_form.h). Lowering and every
 * later pass are checked against these in tests, so a backend can rely on
 * them without checking again. */
#include "shader_form.h"

#include "badger/allocator.h"
#include "badger/arena.h"
#include "badger/fmt.h"

#include <stdarg.h>

/* Where an expression is, as the statements are walked. */
typedef enum visibility
{
    NOT_YET, /* Not evaluated yet. */
    VISIBLE, /* Evaluated, in a block still open. */
    OUT_OF_SCOPE
} visibility;

typedef struct verifier
{
    const tb_shader_form *form;
    const tb_shader_form_function *function;
    uint32_t function_index;
    char *why;
    size_t size;
    bool failed;
    uint8_t *states;  /* One visibility for each expression. */
    uint8_t *seen;    /* One for each statement: whether a block holds it already. */
    uint32_t *opened; /* Expressions evaluated, in order, for closing scopes. */
    size_t opened_count;
    size_t loop_depth;  /* Loops open, where `continue` may be. */
    size_t break_depth; /* Loops and switches open, where `break` may be. */
    bool in_continuing; /* In a continuing block, where neither may be. */
} verifier;

static bool fail(verifier *check, const char *format, ...)
{
    if (check->failed)
    {
        return false;
    }
    check->failed = true;
    char message[256] = {0};
    va_list arguments;
    va_start(arguments, format);
    (void)tb_fmt_vbuffer(message, sizeof(message), format, arguments);
    va_end(arguments);
    const char *const name = check->function == NULL ? "module" : check->function->name;
    (void)tb_fmt_buffer(check->why, check->size, "%s: %s", name, message);
    return false;
}

static bool valid_type(const verifier *check, uint32_t type)
{
    return type < tb_array_length(check->form->types);
}

static const tb_shader_form_type *type_at(const verifier *check, uint32_t type)
{
    return tb_shader_form_type_at(check->form, type);
}

static bool is_scalar_of(const verifier *check, uint32_t type, bool (*accepts)(tb_shader_scalar))
{
    const tb_shader_form_type *const found = type_at(check, type);
    return found->kind == TB_SHADER_FORM_SCALAR && accepts(found->scalar);
}

static bool is_bool(tb_shader_scalar scalar)
{
    return scalar == TB_SHADER_SCALAR_BOOL;
}

static bool is_integer(tb_shader_scalar scalar)
{
    return tb_shader_scalar_is_integer(scalar);
}

/* -------------------------------------------------------------------------
 * The module.
 * ------------------------------------------------------------------------- */

static bool verify_types(verifier *check)
{
    const size_t count = tb_array_length(check->form->types);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_type *const type = type_at(check, (uint32_t)index);
        if (type->kind == TB_SHADER_FORM_ARRAY && type->element >= index)
        {
            return fail(check, "type %zu: an array of a type made after it", index);
        }
        if (type->kind != TB_SHADER_FORM_STRUCT)
        {
            continue;
        }
        if ((size_t)type->first_member + type->member_count > tb_array_length(check->form->members))
        {
            return fail(check, "type %zu: members past the end", index);
        }
        for (uint32_t at = 0; at < type->member_count; at++)
        {
            const uint32_t member =
                tb_shader_form_member_at(check->form, type->first_member + at)->type;
            if (member >= index)
            {
                return fail(check, "type %zu: a member of a type made after it", index);
            }
        }
    }
    const size_t global_count = tb_array_length(check->form->globals);
    for (size_t index = 0; index < global_count; index++)
    {
        const tb_shader_form_global *const global =
            TB_ARRAY_AT(check->form->globals, tb_shader_form_global, index);
        if (!valid_type(check, global->type))
        {
            return fail(check, "global %zu: no such type", index);
        }
        if (global->space != TB_SHADER_FORM_PRIVATE && global->space != TB_SHADER_FORM_GROUPSHARED)
        {
            return fail(check, "global %zu: a global is private or groupshared", index);
        }
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Expressions, each by itself.
 * ------------------------------------------------------------------------- */

static const tb_shader_form_expression *expression_at(const verifier *check, uint32_t index)
{
    return tb_shader_form_expression_at(check->function, index);
}

static bool is_value(const verifier *check, uint32_t index)
{
    return expression_at(check, index)->space == TB_SHADER_FORM_VALUE;
}

/* The type `base[index]` has: an array's element, a vector's component, a
 * matrix's row; NONE when `base` cannot be indexed. */
static uint32_t element_type(const verifier *check, uint32_t base, uint32_t *length)
{
    const tb_shader_form_type *const type = type_at(check, base);
    switch (type->kind)
    {
    case TB_SHADER_FORM_ARRAY:
        *length = type->length;
        return type->element;
    case TB_SHADER_FORM_VECTOR:
        *length = type->columns;
        return tb_shader_form_find_numeric(check->form, TB_SHADER_FORM_SCALAR, type->scalar, 1, 1);
    case TB_SHADER_FORM_MATRIX:
        *length = type->rows;
        return tb_shader_form_find_numeric(check->form, TB_SHADER_FORM_VECTOR, type->scalar, 1,
                                           type->columns);
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_STRUCT:
    case TB_SHADER_FORM_STRING:
        break;
    }
    *length = 0;
    return TB_SHADER_FORM_NONE;
}

static bool verify_access(verifier *check, uint32_t index, const tb_shader_form_expression *access)
{
    const tb_shader_form_expression *const base = expression_at(check, access->operands[0]);
    if (access->space != base->space)
    {
        return fail(check, "%%%u: an access is a pointer when its base is", index);
    }
    const tb_shader_form_type *const base_type = type_at(check, base->type);
    if (access->kind == TB_SHADER_FORM_ACCESS_INDEX && base_type->kind == TB_SHADER_FORM_STRUCT)
    {
        if (access->index >= base_type->member_count)
        {
            return fail(check, "%%%u: no member %u", index, (unsigned)access->index);
        }
        const uint32_t member =
            tb_shader_form_member_at(check->form, base_type->first_member + access->index)->type;
        return access->type == member || fail(check, "%%%u: not the member's type", index);
    }
    uint32_t length = 0;
    const uint32_t element = element_type(check, base->type, &length);
    if (element == TB_SHADER_FORM_NONE)
    {
        return fail(check, "%%%u: '%s' cannot be indexed", index, base_type->name);
    }
    if (access->type != element)
    {
        return fail(check, "%%%u: not the element's type", index);
    }
    if (access->kind == TB_SHADER_FORM_ACCESS_INDEX)
    {
        return access->index < length ||
               fail(check, "%%%u: index %u past the end", index, (unsigned)access->index);
    }
    const uint32_t offset = access->operands[1];
    return (is_value(check, offset) &&
            is_scalar_of(check, expression_at(check, offset)->type, is_integer)) ||
           fail(check, "%%%u: an index is an integer", index);
}

static bool verify_compose(verifier *check, uint32_t index,
                           const tb_shader_form_expression *compose)
{
    const tb_shader_form_type *const type = type_at(check, compose->type);
    if ((size_t)compose->first_operand + compose->operand_count >
        tb_array_length(check->function->operands))
    {
        return fail(check, "%%%u: operands past the end", index);
    }
    uint32_t components = 0;
    for (uint32_t at = 0; at < compose->operand_count; at++)
    {
        const uint32_t operand =
            tb_shader_form_operand_at(check->function, compose->first_operand + at);
        if (operand >= index || !is_value(check, operand))
        {
            return fail(check, "%%%u: an operand is an earlier value", index);
        }
        const uint32_t part = expression_at(check, operand)->type;
        const tb_shader_form_type *const part_type = type_at(check, part);
        uint32_t wanted = TB_SHADER_FORM_NONE;
        switch (type->kind)
        {
        case TB_SHADER_FORM_VECTOR:
            if ((part_type->kind != TB_SHADER_FORM_SCALAR &&
                 part_type->kind != TB_SHADER_FORM_VECTOR) ||
                part_type->scalar != type->scalar)
            {
                return fail(check, "%%%u: a vector is made of its scalars and vectors", index);
            }
            components += tb_shader_form_component_count(check->form, part);
            continue;
        case TB_SHADER_FORM_MATRIX:
            wanted = tb_shader_form_find_numeric(check->form, TB_SHADER_FORM_VECTOR, type->scalar,
                                                 1, type->columns);
            break;
        case TB_SHADER_FORM_ARRAY:
            wanted = type->element;
            break;
        case TB_SHADER_FORM_STRUCT:
            wanted = at < type->member_count
                         ? tb_shader_form_member_at(check->form, type->first_member + at)->type
                         : TB_SHADER_FORM_NONE;
            break;
        case TB_SHADER_FORM_VOID:
        case TB_SHADER_FORM_SCALAR:
        case TB_SHADER_FORM_STRING:
            return fail(check, "%%%u: '%s' cannot be composed", index, type->name);
        }
        if (part != wanted)
        {
            return fail(check, "%%%u: operand %u is not of the part's type", index, (unsigned)at);
        }
    }
    uint32_t expected = 0;
    switch (type->kind)
    {
    case TB_SHADER_FORM_VECTOR:
        return components == type->columns || fail(check, "%%%u: %u components make a '%s'", index,
                                                   (unsigned)components, type->name);
    case TB_SHADER_FORM_MATRIX:
        expected = type->rows;
        break;
    case TB_SHADER_FORM_ARRAY:
        expected = type->length;
        break;
    case TB_SHADER_FORM_STRUCT:
        expected = type->member_count;
        break;
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_STRING:
        break;
    }
    return compose->operand_count == expected || fail(check, "%%%u: %u parts make a '%s'", index,
                                                      (unsigned)compose->operand_count, type->name);
}

static bool is_comparison(tb_shader_form_op op)
{
    return op >= TB_SHADER_FORM_OP_EQUAL && op <= TB_SHADER_FORM_OP_GREATER_EQUAL;
}

static bool is_bitwise(tb_shader_form_op op)
{
    return op == TB_SHADER_FORM_OP_SHIFT_LEFT || op == TB_SHADER_FORM_OP_SHIFT_RIGHT ||
           op == TB_SHADER_FORM_OP_BIT_AND || op == TB_SHADER_FORM_OP_BIT_OR ||
           op == TB_SHADER_FORM_OP_BIT_XOR || op == TB_SHADER_FORM_OP_BIT_NOT;
}

static bool verify_operator(verifier *check, uint32_t index,
                            const tb_shader_form_expression *expression)
{
    const uint32_t operand = expression_at(check, expression->operands[0])->type;
    const tb_shader_form_type *const operand_type = type_at(check, operand);
    if (!tb_shader_form_is_numeric(check->form, operand))
    {
        return fail(check, "%%%u: an operator takes numbers", index);
    }
    if (expression->kind == TB_SHADER_FORM_BINARY &&
        expression_at(check, expression->operands[1])->type != operand)
    {
        return fail(check, "%%%u: the operands differ", index);
    }
    const tb_shader_form_op op = expression->op;
    if (is_bitwise(op) && !tb_shader_scalar_is_integer(operand_type->scalar))
    {
        return fail(check, "%%%u: '%s' is not an integer", index, operand_type->name);
    }
    const bool takes_bools =
        op == TB_SHADER_FORM_OP_NOT || op == TB_SHADER_FORM_OP_AND || op == TB_SHADER_FORM_OP_OR;
    if (takes_bools && operand_type->scalar != TB_SHADER_SCALAR_BOOL)
    {
        return fail(check, "%%%u: a logical operator takes bools", index);
    }
    if ((op == TB_SHADER_FORM_OP_AND || op == TB_SHADER_FORM_OP_OR) &&
        operand_type->kind != TB_SHADER_FORM_SCALAR)
    {
        return fail(check, "%%%u: '&&' and '||' take scalars", index);
    }
    const uint32_t result =
        is_comparison(op) ? tb_shader_form_find_shaped(check->form, TB_SHADER_SCALAR_BOOL, operand)
                          : operand;
    return expression->type == result || fail(check, "%%%u: not the operator's type", index);
}

static bool verify_shapes(verifier *check, uint32_t index,
                          const tb_shader_form_expression *expression)
{
    const uint32_t operand = expression_at(check, expression->operands[0])->type;
    const tb_shader_form_type *const from = type_at(check, operand);
    const tb_shader_form_type *const to = type_at(check, expression->type);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_SWIZZLE:
        if (from->kind != TB_SHADER_FORM_VECTOR || expression->swizzle_count == 0 ||
            expression->swizzle_count > TB_SHADER_FORM_MAX_SWIZZLE)
        {
            return fail(check, "%%%u: a swizzle selects from 1 to 4 of a vector's", index);
        }
        for (uint8_t at = 0; at < expression->swizzle_count; at++)
        {
            if (expression->swizzle[at] >= from->columns)
            {
                return fail(check, "%%%u: component %u is past the vector", index,
                            (unsigned)expression->swizzle[at]);
            }
        }
        return to->scalar == from->scalar &&
                       tb_shader_form_component_count(check->form, expression->type) ==
                           expression->swizzle_count &&
                       (to->kind == TB_SHADER_FORM_VECTOR) == (expression->swizzle_count > 1)
                   ? true
                   : fail(check, "%%%u: not the swizzle's type", index);
    case TB_SHADER_FORM_SPLAT:
        return (from->kind == TB_SHADER_FORM_SCALAR &&
                (to->kind == TB_SHADER_FORM_VECTOR || to->kind == TB_SHADER_FORM_MATRIX) &&
                to->scalar == from->scalar) ||
               fail(check, "%%%u: a splat makes a scalar a vector or a matrix", index);
    case TB_SHADER_FORM_CONVERT:
        return (tb_shader_form_is_numeric(check->form, operand) &&
                tb_shader_form_find_shaped(check->form, to->scalar, operand) == expression->type) ||
               fail(check, "%%%u: a conversion keeps the shape", index);
    case TB_SHADER_FORM_SELECT:
        return (is_scalar_of(check, operand, is_bool) &&
                expression_at(check, expression->operands[1])->type == expression->type &&
                expression_at(check, expression->operands[2])->type == expression->type) ||
               fail(check, "%%%u: a select takes a bool and two values of its type", index);
    case TB_SHADER_FORM_LOAD:
        return (expression_at(check, expression->operands[0])->space != TB_SHADER_FORM_VALUE &&
                operand == expression->type) ||
               fail(check, "%%%u: a load reads a pointer of its type", index);
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
    case TB_SHADER_FORM_ACCESS:
    case TB_SHADER_FORM_ACCESS_INDEX:
    case TB_SHADER_FORM_COMPOSE:
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_BINARY:
    case TB_SHADER_FORM_INTRINSIC:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
        break;
    }
    return true;
}

static uint32_t fixed_operand_count(tb_shader_form_expression_kind kind)
{
    switch (kind)
    {
    case TB_SHADER_FORM_LOAD:
    case TB_SHADER_FORM_ACCESS_INDEX:
    case TB_SHADER_FORM_SWIZZLE:
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_CONVERT:
        return 1;
    case TB_SHADER_FORM_ACCESS:
    case TB_SHADER_FORM_BINARY:
        return 2;
    case TB_SHADER_FORM_SELECT:
        return 3;
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
    case TB_SHADER_FORM_COMPOSE:
    case TB_SHADER_FORM_INTRINSIC:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
        break;
    }
    return 0;
}

/* What names a variable: its index, type and space. */
static bool verify_name(verifier *check, uint32_t index, const tb_shader_form_expression *name)
{
    uint32_t type = TB_SHADER_FORM_NONE;
    tb_shader_form_space space = TB_SHADER_FORM_FUNCTION;
    if (name->kind == TB_SHADER_FORM_ARGUMENT)
    {
        if (name->index >= tb_array_length(check->function->parameters))
        {
            return fail(check, "%%%u: no argument %u", index, (unsigned)name->index);
        }
        const tb_shader_form_parameter *const parameter =
            TB_ARRAY_AT(check->function->parameters, tb_shader_form_parameter, name->index);
        type = parameter->type;
        space = parameter->direction == TB_SHADER_FORM_IN ? TB_SHADER_FORM_VALUE
                                                          : TB_SHADER_FORM_FUNCTION;
    }
    else if (name->kind == TB_SHADER_FORM_LOCAL)
    {
        if (name->index >= tb_array_length(check->function->locals))
        {
            return fail(check, "%%%u: no local %u", index, (unsigned)name->index);
        }
        type = TB_ARRAY_AT(check->function->locals, tb_shader_form_local, name->index)->type;
    }
    else
    {
        if (name->index >= tb_array_length(check->form->globals))
        {
            return fail(check, "%%%u: no global %u", index, (unsigned)name->index);
        }
        const tb_shader_form_global *const global =
            TB_ARRAY_AT(check->form->globals, tb_shader_form_global, name->index);
        type = global->type;
        space = global->space;
    }
    return (name->type == type && name->space == space) ||
           fail(check, "%%%u: not the variable's type and space", index);
}

static bool verify_expression(verifier *check, uint32_t index)
{
    const tb_shader_form_expression *const expression = expression_at(check, index);
    if (!valid_type(check, expression->type))
    {
        return fail(check, "%%%u: no such type", index);
    }
    const uint32_t count = fixed_operand_count(expression->kind);
    for (uint32_t at = 0; at < count; at++)
    {
        if (expression->operands[at] >= index)
        {
            return fail(check, "%%%u: an operand is not an earlier expression", index);
        }
    }
    const bool takes_values = expression->kind != TB_SHADER_FORM_LOAD &&
                              expression->kind != TB_SHADER_FORM_ACCESS &&
                              expression->kind != TB_SHADER_FORM_ACCESS_INDEX;
    for (uint32_t at = 0; at < count && takes_values; at++)
    {
        if (!is_value(check, expression->operands[at]))
        {
            return fail(check, "%%%u: a pointer where a value is taken", index);
        }
    }
    const bool is_pointer_kind =
        expression->kind == TB_SHADER_FORM_ARGUMENT || expression->kind == TB_SHADER_FORM_LOCAL ||
        expression->kind == TB_SHADER_FORM_GLOBAL || expression->kind == TB_SHADER_FORM_ACCESS ||
        expression->kind == TB_SHADER_FORM_ACCESS_INDEX;
    if (!is_pointer_kind && expression->space != TB_SHADER_FORM_VALUE)
    {
        return fail(check, "%%%u: only a variable or an access of one is a pointer", index);
    }
    switch (expression->kind)
    {
    case TB_SHADER_FORM_LITERAL:
        return type_at(check, expression->type)->kind == TB_SHADER_FORM_SCALAR ||
               fail(check, "%%%u: a literal is a scalar", index);
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
        return verify_name(check, index, expression);
    case TB_SHADER_FORM_ACCESS:
    case TB_SHADER_FORM_ACCESS_INDEX:
        return verify_access(check, index, expression);
    case TB_SHADER_FORM_COMPOSE:
        return verify_compose(check, index, expression);
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_BINARY:
        return verify_operator(check, index, expression);
    case TB_SHADER_FORM_INTRINSIC:
        return ((size_t)expression->first_operand + expression->operand_count <=
                tb_array_length(check->function->operands)) ||
               fail(check, "%%%u: operands past the end", index);
    case TB_SHADER_FORM_LOAD:
    case TB_SHADER_FORM_SWIZZLE:
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_SELECT:
    case TB_SHADER_FORM_CONVERT:
        return verify_shapes(check, index, expression);
    case TB_SHADER_FORM_CALL_RESULT:
        return true;
    case TB_SHADER_FORM_STRING_LITERAL:
        return (expression->index < tb_array_length(check->form->strings) &&
                type_at(check, expression->type)->kind == TB_SHADER_FORM_STRING) ||
               fail(check, "%%%u: no string %u", index, (unsigned)expression->index);
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Statements, and where expressions are evaluated.
 * ------------------------------------------------------------------------- */

static bool visible(verifier *check, uint32_t index)
{
    if (index >= tb_shader_form_expression_count(check->function))
    {
        return fail(check, "no expression %u", (unsigned)index);
    }
    const tb_shader_form_expression *const expression = expression_at(check, index);
    if (expression->kind != TB_SHADER_FORM_CALL_RESULT &&
        !tb_shader_form_is_emitted(expression->kind))
    {
        return true;
    }
    if (check->states[index] == VISIBLE)
    {
        return true;
    }
    return fail(check, "%%%u is used where it is not evaluated", (unsigned)index);
}

static void open_expression(verifier *check, uint32_t index)
{
    check->states[index] = VISIBLE;
    check->opened[check->opened_count++] = index;
}

/* The expressions an expression uses must be evaluated before it is. */
static bool operands_visible(verifier *check, uint32_t index)
{
    const tb_shader_form_expression *const expression = expression_at(check, index);
    const uint32_t count = fixed_operand_count(expression->kind);
    for (uint32_t at = 0; at < count; at++)
    {
        if (!visible(check, expression->operands[at]))
        {
            return false;
        }
    }
    const bool has_list =
        expression->kind == TB_SHADER_FORM_COMPOSE || expression->kind == TB_SHADER_FORM_INTRINSIC;
    for (uint32_t at = 0; has_list && at < expression->operand_count; at++)
    {
        if (!visible(check,
                     tb_shader_form_operand_at(check->function, expression->first_operand + at)))
        {
            return false;
        }
    }
    return true;
}

static bool verify_emit(verifier *check, const tb_shader_form_statement *emit)
{
    const uint32_t start = emit->operands[0];
    const uint32_t end = emit->operands[1];
    if (start > end || end > tb_shader_form_expression_count(check->function))
    {
        return fail(check, "an emit of %u to %u", (unsigned)start, (unsigned)end);
    }
    for (uint32_t index = start; index < end; index++)
    {
        if (!tb_shader_form_is_emitted(expression_at(check, index)->kind))
        {
            continue;
        }
        if (check->states[index] != NOT_YET)
        {
            return fail(check, "%%%u is evaluated twice", (unsigned)index);
        }
        if (!operands_visible(check, index))
        {
            return false;
        }
        open_expression(check, index);
    }
    return true;
}

static bool verify_block(verifier *check, uint32_t block);

/* A statement in its one place: no statement is in two. */
static bool claim(verifier *check, uint32_t statement)
{
    if (statement >= tb_shader_form_statement_count(check->function))
    {
        return fail(check, "no statement %u", (unsigned)statement);
    }
    if (check->seen[statement] != 0)
    {
        return fail(check, "statement %u is in two places", (unsigned)statement);
    }
    check->seen[statement] = 1;
    return true;
}

/* A block a statement holds: claimed, then checked. */
static bool verify_held_block(verifier *check, uint32_t block)
{
    return claim(check, block) && verify_block(check, block);
}

static void close_scope(verifier *check, size_t mark)
{
    while (check->opened_count > mark)
    {
        check->states[check->opened[--check->opened_count]] = OUT_OF_SCOPE;
    }
}

static bool verify_statements(verifier *check, uint32_t block);

static bool is_bool_scalar(verifier *check, uint32_t index)
{
    return is_value(check, index) &&
           is_scalar_of(check, expression_at(check, index)->type, is_bool);
}

static bool verify_call(verifier *check, const tb_shader_form_statement *call)
{
    if ((size_t)call->first_operand + call->operand_count >
        tb_array_length(check->function->operands))
    {
        return fail(check, "a call's operands are past the end");
    }
    for (uint32_t at = 0; at < call->operand_count; at++)
    {
        if (!visible(check, tb_shader_form_operand_at(check->function, call->first_operand + at)))
        {
            return false;
        }
    }
    uint32_t result_type = TB_SHADER_FORM_NONE;
    if (call->function != TB_SHADER_FORM_NONE)
    {
        if (call->function >= tb_shader_form_function_count(check->form))
        {
            return fail(check, "a call of no function");
        }
        const tb_shader_form_function *const callee =
            tb_shader_form_function_at(check->form, call->function);
        if (call->operand_count != tb_array_length(callee->parameters))
        {
            return fail(check, "a call of '%s' with %u arguments", callee->name,
                        (unsigned)call->operand_count);
        }
        for (uint32_t at = 0; at < call->operand_count; at++)
        {
            const tb_shader_form_parameter *const parameter =
                TB_ARRAY_AT(callee->parameters, tb_shader_form_parameter, at);
            const tb_shader_form_expression *const argument = expression_at(
                check, tb_shader_form_operand_at(check->function, call->first_operand + at));
            const tb_shader_form_space wanted = parameter->direction == TB_SHADER_FORM_IN
                                                    ? TB_SHADER_FORM_VALUE
                                                    : TB_SHADER_FORM_FUNCTION;
            if (argument->type != parameter->type || argument->space != wanted)
            {
                return fail(check, "argument %u of '%s' is not of its parameter's type",
                            (unsigned)at + 1, callee->name);
            }
        }
        result_type = callee->result;
    }
    else if (call->intrinsic == 0)
    {
        return fail(check, "a call of nothing");
    }
    if (call->result == TB_SHADER_FORM_NONE)
    {
        return result_type == TB_SHADER_FORM_NONE ||
               type_at(check, result_type)->kind == TB_SHADER_FORM_VOID ||
               fail(check, "a call that returns a value has no result");
    }
    if (call->result >= tb_shader_form_expression_count(check->function) ||
        expression_at(check, call->result)->kind != TB_SHADER_FORM_CALL_RESULT ||
        check->states[call->result] != NOT_YET)
    {
        return fail(check, "a call's result is a call result of its own");
    }
    if (result_type != TB_SHADER_FORM_NONE &&
        expression_at(check, call->result)->type != result_type)
    {
        return fail(check, "a call's result is not of the type returned");
    }
    open_expression(check, call->result);
    return true;
}

static bool verify_switch(verifier *check, const tb_shader_form_statement *statement)
{
    const uint32_t selector = statement->operands[0];
    if (!visible(check, selector) || !is_value(check, selector) ||
        !is_scalar_of(check, expression_at(check, selector)->type, is_integer))
    {
        return fail(check, "a switch selects by an integer");
    }
    bool has_default = false;
    check->break_depth++;
    for (uint32_t index = statement->first; index != TB_SHADER_FORM_NONE;)
    {
        if (!claim(check, index))
        {
            return false;
        }
        const tb_shader_form_statement *const label =
            tb_shader_form_statement_at(check->function, index);
        if (label->kind != TB_SHADER_FORM_CASE)
        {
            return fail(check, "a switch holds cases");
        }
        if (label->is_default && has_default)
        {
            return fail(check, "a switch has two defaults");
        }
        has_default = has_default || label->is_default;
        if (label->next == TB_SHADER_FORM_NONE && label->falls_through)
        {
            return fail(check, "the last case falls through");
        }
        if (!verify_held_block(check, label->body))
        {
            return false;
        }
        index = label->next;
    }
    check->break_depth--;
    return true;
}

static bool verify_loop(verifier *check, const tb_shader_form_statement *loop)
{
    check->loop_depth++;
    check->break_depth++;
    if (!verify_held_block(check, loop->body))
    {
        return false;
    }
    check->loop_depth--;
    check->break_depth--;
    /* The continuing block's expressions stay in scope for `break if`. */
    const size_t mark = check->opened_count;
    const bool was_continuing = check->in_continuing;
    check->in_continuing = true;
    if (!claim(check, loop->continuing) || !verify_statements(check, loop->continuing))
    {
        return false;
    }
    check->in_continuing = was_continuing;
    if (loop->operands[0] != TB_SHADER_FORM_NONE && !is_bool_scalar(check, loop->operands[0]))
    {
        return fail(check, "a loop breaks on a bool");
    }
    if (loop->operands[0] != TB_SHADER_FORM_NONE && !visible(check, loop->operands[0]))
    {
        return false;
    }
    close_scope(check, mark);
    return true;
}

static bool verify_statement(verifier *check, uint32_t index)
{
    const tb_shader_form_statement *const statement =
        tb_shader_form_statement_at(check->function, index);
    switch (statement->kind)
    {
    case TB_SHADER_FORM_BLOCK:
        return verify_block(check, index);
    case TB_SHADER_FORM_EMIT:
        return verify_emit(check, statement);
    case TB_SHADER_FORM_IF:
        if (!visible(check, statement->operands[0]) ||
            !is_bool_scalar(check, statement->operands[0]))
        {
            return fail(check, "an if tests a bool");
        }
        return verify_held_block(check, statement->accept) &&
               (statement->reject == TB_SHADER_FORM_NONE ||
                verify_held_block(check, statement->reject));
    case TB_SHADER_FORM_LOOP:
        return verify_loop(check, statement);
    case TB_SHADER_FORM_SWITCH:
        return verify_switch(check, statement);
    case TB_SHADER_FORM_CASE:
        return fail(check, "a case outside a switch");
    case TB_SHADER_FORM_BREAK:
        return (check->break_depth > 0 && !check->in_continuing) ||
               fail(check, "a break outside a loop or switch");
    case TB_SHADER_FORM_CONTINUE:
        return (check->loop_depth > 0 && !check->in_continuing) ||
               fail(check, "a continue outside a loop");
    case TB_SHADER_FORM_RETURN: {
        const uint32_t value = statement->operands[0];
        const bool returns_void =
            type_at(check, check->function->result)->kind == TB_SHADER_FORM_VOID;
        if (value == TB_SHADER_FORM_NONE)
        {
            return returns_void || fail(check, "a return without the value returned");
        }
        return visible(check, value) &&
               ((is_value(check, value) &&
                 expression_at(check, value)->type == check->function->result) ||
                fail(check, "a return of another type"));
    }
    case TB_SHADER_FORM_DISCARD:
        return true;
    case TB_SHADER_FORM_STORE: {
        const uint32_t pointer = statement->operands[0];
        const uint32_t value = statement->operands[1];
        if (!visible(check, pointer) || !visible(check, value))
        {
            return false;
        }
        return (!is_value(check, pointer) && is_value(check, value) &&
                expression_at(check, pointer)->type == expression_at(check, value)->type) ||
               fail(check, "a store of a value of the pointer's type");
    }
    case TB_SHADER_FORM_CALL:
        return verify_call(check, statement);
    }
    return true;
}

/* The statements of a block, leaving what they evaluate in scope. */
static bool verify_statements(verifier *check, uint32_t block)
{
    const tb_shader_form_statement *const statement =
        tb_shader_form_statement_at(check->function, block);
    if (statement->kind != TB_SHADER_FORM_BLOCK)
    {
        return fail(check, "statement %u is not a block", (unsigned)block);
    }
    for (uint32_t index = statement->first; index != TB_SHADER_FORM_NONE;)
    {
        if (!claim(check, index) || !verify_statement(check, index))
        {
            return false;
        }
        index = tb_shader_form_statement_at(check->function, index)->next;
    }
    return true;
}

/* A block, whose expressions go out of scope where it ends. */
static bool verify_block(verifier *check, uint32_t block)
{
    const size_t mark = check->opened_count;
    if (!verify_statements(check, block))
    {
        return false;
    }
    close_scope(check, mark);
    return true;
}

static bool verify_function(verifier *check, tb_arena *scratch)
{
    const tb_shader_form_function *const function = check->function;
    if (!valid_type(check, function->result))
    {
        return fail(check, "no such type returned");
    }
    const size_t parameter_count = tb_array_length(function->parameters);
    for (size_t index = 0; index < parameter_count; index++)
    {
        if (!valid_type(check,
                        TB_ARRAY_AT(function->parameters, tb_shader_form_parameter, index)->type))
        {
            return fail(check, "parameter %zu: no such type", index);
        }
    }
    const size_t local_count = tb_array_length(function->locals);
    for (size_t index = 0; index < local_count; index++)
    {
        if (!valid_type(check, TB_ARRAY_AT(function->locals, tb_shader_form_local, index)->type))
        {
            return fail(check, "local %zu: no such type", index);
        }
    }
    const size_t expression_count = tb_shader_form_expression_count(function);
    for (size_t index = 0; index < expression_count; index++)
    {
        if (!verify_expression(check, (uint32_t)index))
        {
            return false;
        }
    }
    const size_t statement_count = tb_shader_form_statement_count(function);
    check->states = TB_ARENA_NEW_ARRAY(scratch, uint8_t, expression_count + 1);
    check->opened = TB_ARENA_NEW_ARRAY(scratch, uint32_t, expression_count + 1);
    check->seen = TB_ARENA_NEW_ARRAY(scratch, uint8_t, statement_count + 1);
    check->opened_count = 0;
    if (!verify_held_block(check, function->body))
    {
        return false;
    }
    for (size_t index = 0; index < expression_count; index++)
    {
        const tb_shader_form_expression *const expression = expression_at(check, (uint32_t)index);
        const bool needs = tb_shader_form_is_emitted(expression->kind) ||
                           expression->kind == TB_SHADER_FORM_CALL_RESULT;
        if (needs && check->states[index] == NOT_YET)
        {
            return fail(check, "%%%zu is never evaluated", index);
        }
    }
    return true;
}

bool tb_shader_form_verify(const tb_shader_form *form, char *why, size_t size)
{
    verifier check = {0};
    check.form = form;
    check.why = why;
    check.size = size;
    if (size > 0)
    {
        why[0] = '\0';
    }
    if (!verify_types(&check))
    {
        return false;
    }
    const uint32_t initializer = form->initializer;
    if (initializer != TB_SHADER_FORM_NONE)
    {
        const bool valid =
            initializer < tb_shader_form_function_count(form) &&
            tb_array_length(tb_shader_form_function_at(form, initializer)->parameters) == 0;
        if (!valid)
        {
            return fail(&check, "the initializer is a function of no parameters");
        }
    }
    tb_arena *const scratch = tb_arena_create(0);
    const size_t count = tb_shader_form_function_count(form);
    bool ok = true;
    for (size_t index = 0; index < count && ok; index++)
    {
        check.function = tb_shader_form_function_at(form, (uint32_t)index);
        check.function_index = (uint32_t)index;
        ok = verify_function(&check, scratch);
        tb_arena_reset(scratch);
    }
    tb_arena_destroy(scratch);
    return ok;
}
