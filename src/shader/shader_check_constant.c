/* Folding constants: the values of expressions known while checking, for
 * array sizes, case values, template arguments and constants. Integers wrap
 * at their width, as HLSL's do, and shift counts are masked to it. */
#include "shader_check_internal.h"

#include <float.h>

static bool is_signed(tb_shader_scalar scalar)
{
    return scalar == TB_SHADER_SCALAR_INT || scalar == TB_SHADER_SCALAR_INT16 ||
           scalar == TB_SHADER_SCALAR_INT64 || scalar == TB_SHADER_SCALAR_MIN16INT ||
           scalar == TB_SHADER_SCALAR_LITERAL_INT;
}

static unsigned width_of(tb_shader_scalar scalar)
{
    if (scalar == TB_SHADER_SCALAR_INT16 || scalar == TB_SHADER_SCALAR_UINT16)
    {
        return 16;
    }
    if (scalar == TB_SHADER_SCALAR_INT64 || scalar == TB_SHADER_SCALAR_UINT64 ||
        scalar == TB_SHADER_SCALAR_LITERAL_INT)
    {
        return 64;
    }
    return 32;
}

/* Bits wrapped to a scalar's width and extended by its sign. */
static uint64_t normalize(uint64_t bits, tb_shader_scalar scalar)
{
    if (scalar == TB_SHADER_SCALAR_BOOL)
    {
        return bits != 0 ? 1u : 0u;
    }
    const unsigned width = width_of(scalar);
    if (width == 64)
    {
        return bits;
    }
    const uint64_t mask = (UINT64_C(1) << width) - 1;
    const uint64_t value = bits & mask;
    const uint64_t sign = UINT64_C(1) << (width - 1);
    if (is_signed(scalar) && (value & sign) != 0)
    {
        return value | ~mask;
    }
    return value;
}

static double integer_value(uint64_t bits, tb_shader_scalar scalar)
{
    return is_signed(scalar) ? (double)(int64_t)bits : (double)bits;
}

/* A double rounded to a floating-point scalar's precision. */
static double round_to(double value, tb_shader_scalar scalar)
{
    const bool is_single = scalar == TB_SHADER_SCALAR_FLOAT || scalar == TB_SHADER_SCALAR_HALF ||
                           scalar == TB_SHADER_SCALAR_MIN16FLOAT;
    const bool fits = value >= -(double)FLT_MAX && value <= (double)FLT_MAX;
    return is_single && fits ? (double)(float)value : value;
}

/* A double truncated toward zero to 64 bits; 0 for NaN, saturated beyond. */
static uint64_t truncate_to_integer(double value, tb_shader_scalar scalar)
{
    const double limit = 9223372036854775808.0; /* 2^63 */
    if (value != value)
    {
        return 0;
    }
    if (!is_signed(scalar) && value >= limit)
    {
        return value >= 2.0 * limit ? UINT64_MAX : (uint64_t)(value - limit) + (UINT64_C(1) << 63);
    }
    if (value >= limit)
    {
        return (uint64_t)INT64_MAX;
    }
    if (value < -limit)
    {
        return (uint64_t)INT64_MIN;
    }
    return (uint64_t)(int64_t)value;
}

tb_shader_constant tb_shader_constant_convert(tb_shader_constant constant, tb_shader_scalar from,
                                              tb_shader_scalar to)
{
    tb_shader_constant result = {0};
    if (to == TB_SHADER_SCALAR_BOOL)
    {
        result.bits =
            constant.is_float ? (constant.value != 0.0 ? 1u : 0u) : (constant.bits != 0 ? 1u : 0u);
        return result;
    }
    if (tb_shader_scalar_is_float(to))
    {
        result.is_float = true;
        const double value =
            constant.is_float ? constant.value : integer_value(constant.bits, from);
        result.value = round_to(value, to);
        return result;
    }
    const uint64_t bits =
        constant.is_float ? truncate_to_integer(constant.value, to) : constant.bits;
    result.bits = normalize(bits, to);
    return result;
}

/* -------------------------------------------------------------------------
 * Operators.
 * ------------------------------------------------------------------------- */

static bool fold_division(tb_shader_operator op, uint64_t a, uint64_t b, bool is_signed_type,
                          uint64_t *result)
{
    if (b == 0)
    {
        return false;
    }
    const bool is_divide = op == TB_SHADER_OP_DIVIDE;
    if (!is_signed_type)
    {
        *result = is_divide ? a / b : a % b;
        return true;
    }
    /* INT64_MIN / -1 overflows in C: it wraps to itself here. */
    if ((int64_t)b == -1)
    {
        *result = is_divide ? 0 - a : 0;
        return true;
    }
    *result = (uint64_t)(is_divide ? (int64_t)a / (int64_t)b : (int64_t)a % (int64_t)b);
    return true;
}

static bool fold_integer(tb_shader_operator op, uint64_t a, uint64_t b, tb_shader_scalar scalar,
                         uint64_t *result)
{
    const bool is_signed_type = is_signed(scalar);
    const uint64_t shift_mask = width_of(scalar) - 1;
    if (op == TB_SHADER_OP_DIVIDE || op == TB_SHADER_OP_REMAINDER)
    {
        return fold_division(op, a, b, is_signed_type, result);
    }
    if (op == TB_SHADER_OP_SHIFT_RIGHT)
    {
        const unsigned count = (unsigned)(b & shift_mask);
        const bool negative = is_signed_type && (a >> 63) != 0;
        *result = negative ? ~(~a >> count) : a >> count;
        return true;
    }
    if (op == TB_SHADER_OP_ADD)
    {
        *result = a + b;
    }
    else if (op == TB_SHADER_OP_SUBTRACT)
    {
        *result = a - b;
    }
    else if (op == TB_SHADER_OP_MULTIPLY)
    {
        *result = a * b;
    }
    else if (op == TB_SHADER_OP_SHIFT_LEFT)
    {
        *result = a << (b & shift_mask);
    }
    else if (op == TB_SHADER_OP_BIT_AND)
    {
        *result = a & b;
    }
    else if (op == TB_SHADER_OP_BIT_OR)
    {
        *result = a | b;
    }
    else if (op == TB_SHADER_OP_BIT_XOR)
    {
        *result = a ^ b;
    }
    else
    {
        return false;
    }
    return true;
}

static bool fold_float(tb_shader_operator op, double a, double b, double *result)
{
    if (op == TB_SHADER_OP_ADD)
    {
        *result = a + b;
        return true;
    }
    if (op == TB_SHADER_OP_SUBTRACT)
    {
        *result = a - b;
        return true;
    }
    if (op == TB_SHADER_OP_MULTIPLY)
    {
        *result = a * b;
        return true;
    }
    if (op == TB_SHADER_OP_DIVIDE && b != 0.0)
    {
        *result = a / b;
        return true;
    }
    return false;
}

/* -1, 0 or 1 as a compares to b; false for NaN. */
static bool compare(const tb_shader_constant *a, const tb_shader_constant *b,
                    tb_shader_scalar scalar, int *order)
{
    if (a->is_float)
    {
        if (a->value != a->value || b->value != b->value)
        {
            return false;
        }
        *order = a->value < b->value ? -1 : (a->value > b->value ? 1 : 0);
        return true;
    }
    if (is_signed(scalar))
    {
        const int64_t x = (int64_t)a->bits;
        const int64_t y = (int64_t)b->bits;
        *order = x < y ? -1 : (x > y ? 1 : 0);
        return true;
    }
    *order = a->bits < b->bits ? -1 : (a->bits > b->bits ? 1 : 0);
    return true;
}

static bool fold_comparison(tb_shader_operator op, int order, uint64_t *result)
{
    bool value = false;
    if (op == TB_SHADER_OP_LESS)
    {
        value = order < 0;
    }
    else if (op == TB_SHADER_OP_GREATER)
    {
        value = order > 0;
    }
    else if (op == TB_SHADER_OP_LESS_EQUAL)
    {
        value = order <= 0;
    }
    else if (op == TB_SHADER_OP_GREATER_EQUAL)
    {
        value = order >= 0;
    }
    else if (op == TB_SHADER_OP_EQUAL)
    {
        value = order == 0;
    }
    else if (op == TB_SHADER_OP_NOT_EQUAL)
    {
        value = order != 0;
    }
    else
    {
        return false;
    }
    *result = value ? 1u : 0u;
    return true;
}

/* -------------------------------------------------------------------------
 * Folding a node.
 * ------------------------------------------------------------------------- */

/* The scalar of a value that can be folded: a scalar or an enum. */
static bool foldable(const struct tb_shader_type *type, tb_shader_scalar *scalar)
{
    if (type == NULL)
    {
        return false;
    }
    const bool is_scalar = type->type_class == TB_SHADER_CLASS_SCALAR ||
                           type->type_class == TB_SHADER_CLASS_ENUM ||
                           (type->type_class == TB_SHADER_CLASS_VECTOR && type->columns == 1);
    *scalar = type->scalar;
    return is_scalar;
}

static void fold_binary(tb_shader_node *node, tb_shader_scalar result_scalar)
{
    const tb_shader_node *const left = node->left;
    const tb_shader_node *const right = node->right;
    tb_shader_scalar operands = TB_SHADER_SCALAR_INT;
    if (left == NULL || right == NULL || !left->is_constant || !right->is_constant ||
        !foldable(left->checked, &operands))
    {
        return;
    }
    tb_shader_constant result = {0};
    bool folded = false;
    if (node->op == TB_SHADER_OP_AND || node->op == TB_SHADER_OP_OR)
    {
        const bool a = left->constant.bits != 0;
        const bool b = right->constant.bits != 0;
        result.bits = (node->op == TB_SHADER_OP_AND ? a && b : a || b) ? 1u : 0u;
        folded = true;
    }
    else if (node->op == TB_SHADER_OP_COMMA)
    {
        result = right->constant;
        folded = true;
    }
    else if (result_scalar == TB_SHADER_SCALAR_BOOL && operands != TB_SHADER_SCALAR_BOOL)
    {
        int order = 0;
        folded = compare(&left->constant, &right->constant, operands, &order) &&
                 fold_comparison(node->op, order, &result.bits);
    }
    else if (left->constant.is_float)
    {
        result.is_float = true;
        folded = fold_float(node->op, left->constant.value, right->constant.value, &result.value);
        result.value = round_to(result.value, result_scalar);
    }
    else
    {
        folded = fold_integer(node->op, left->constant.bits, right->constant.bits, operands,
                              &result.bits);
        result.bits = normalize(result.bits, result_scalar);
    }
    if (folded)
    {
        node->is_constant = true;
        node->constant = result;
    }
}

static void fold_unary(tb_shader_node *node, tb_shader_scalar scalar)
{
    const tb_shader_node *const operand = node->left;
    if (operand == NULL || !operand->is_constant)
    {
        return;
    }
    tb_shader_constant result = operand->constant;
    if (node->op == TB_SHADER_OP_NEGATE)
    {
        if (result.is_float)
        {
            result.value = -result.value;
        }
        else
        {
            result.bits = normalize(0 - result.bits, scalar);
        }
    }
    else if (node->op == TB_SHADER_OP_BIT_NOT)
    {
        result.bits = normalize(~result.bits, scalar);
    }
    else if (node->op == TB_SHADER_OP_NOT)
    {
        result.bits = result.bits == 0 ? 1u : 0u;
    }
    else if (node->op != TB_SHADER_OP_PLUS)
    {
        return;
    }
    node->is_constant = true;
    node->constant = result;
}

/* A conversion of one constant scalar: a CONVERT, a cast, `float(1)`, `x.x`. */
static void fold_conversion(tb_shader_node *node, const tb_shader_node *operand,
                            tb_shader_scalar to)
{
    tb_shader_scalar from = TB_SHADER_SCALAR_INT;
    if (operand == NULL || !operand->is_constant || !foldable(operand->checked, &from))
    {
        return;
    }
    node->is_constant = true;
    node->constant = tb_shader_constant_convert(operand->constant, from, to);
}

void tb_shader_fold(tb_shader_checker *checker, tb_shader_node *node)
{
    (void)checker;
    tb_shader_scalar scalar = TB_SHADER_SCALAR_INT;
    if (node->is_constant || !foldable(node->checked, &scalar))
    {
        return;
    }
    const tb_shader_node_kind kind = node->kind;
    if (kind == TB_SHADER_NODE_CONVERT || kind == TB_SHADER_NODE_CAST)
    {
        fold_conversion(node, node->left, scalar);
    }
    else if (kind == TB_SHADER_NODE_CONSTRUCT && node->child_count == 1)
    {
        fold_conversion(node, node->children, scalar);
    }
    else if (kind == TB_SHADER_NODE_MEMBER && node->swizzle_count == 1)
    {
        fold_conversion(node, node->left, scalar);
    }
    else if (kind == TB_SHADER_NODE_UNARY)
    {
        fold_unary(node, scalar);
    }
    else if (kind == TB_SHADER_NODE_BINARY)
    {
        fold_binary(node, scalar);
    }
    else if (kind == TB_SHADER_NODE_CONDITIONAL && node->condition != NULL &&
             node->condition->is_constant)
    {
        const tb_shader_node *const chosen =
            node->condition->constant.bits != 0 ? node->left : node->right;
        fold_conversion(node, chosen, scalar);
    }
}
