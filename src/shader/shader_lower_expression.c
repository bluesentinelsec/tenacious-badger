/* Lowering expressions. A value is computed into an expression; a place a
 * value can be stored to becomes a pointer, then a load or a store. What
 * HLSL does implicitly is written out:
 *
 * - a conversion, cast, constructor or initializer list that changes a
 *   shape is taken apart into scalars and built again, unless it is a
 *   conversion of each component, a splat, or a truncation;
 * - `&&`, `||` and `?:` evaluate their second operand only when they must,
 *   as HLSL 2021 does, by a branch when that operand has an effect, and as
 *   an operator or a select when it has none;
 * - `a += b` loads, operates and stores, converting as checking decided;
 * - a store through a swizzle stores each component;
 * - an `out` or `inout` argument goes through a local, copied back after the
 *   call, as HLSL passes them. */
#include "shader_intrinsics.h"
#include "shader_lower_internal.h"
#include "shader_methods.h"

#include "badger/arena.h"
#include "badger/assert.h"

#include <string.h>

/* What a call of a function that returns nothing gives, which nothing uses. */
#define VOID_RESULT (TB_SHADER_FORM_NONE - 1)

/* The most scalars a value is taken apart into. */
#define MAX_FLAT_SCALARS 4096

static tb_shader_form_expression new_expression(tb_shader_form_expression_kind kind, uint32_t type)
{
    tb_shader_form_expression expression = {0};
    expression.kind = kind;
    expression.type = type;
    expression.space = TB_SHADER_FORM_VALUE;
    expression.operands[0] = TB_SHADER_FORM_NONE;
    expression.operands[1] = TB_SHADER_FORM_NONE;
    expression.operands[2] = TB_SHADER_FORM_NONE;
    return expression;
}

uint32_t tb_shader_lower_add(tb_shader_lowerer *lowerer,
                             const tb_shader_form_expression *expression)
{
    return tb_shader_form_add_expression(lowerer->function, expression);
}

uint32_t tb_shader_lower_type_of(const tb_shader_lowerer *lowerer, uint32_t expression)
{
    return tb_shader_form_expression_at(lowerer->function, expression)->type;
}

static tb_shader_form_space space_of(const tb_shader_lowerer *lowerer, uint32_t expression)
{
    return tb_shader_form_expression_at(lowerer->function, expression)->space;
}

static const tb_shader_form_type *form_type(const tb_shader_lowerer *lowerer, uint32_t type)
{
    return tb_shader_form_type_at(lowerer->form, type);
}

/* -------------------------------------------------------------------------
 * Making expressions.
 * ------------------------------------------------------------------------- */

static uint32_t literal(tb_shader_lowerer *lowerer, uint32_t type, tb_shader_constant value)
{
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_LITERAL, type);
    expression.value = value;
    return tb_shader_lower_add(lowerer, &expression);
}

uint32_t tb_shader_lower_local(tb_shader_lowerer *lowerer, uint32_t local)
{
    const tb_shader_form_local *const entry =
        TB_ARRAY_AT(lowerer->function->locals, tb_shader_form_local, local);
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_LOCAL, entry->type);
    expression.space = TB_SHADER_FORM_FUNCTION;
    expression.index = local;
    return tb_shader_lower_add(lowerer, &expression);
}

uint32_t tb_shader_lower_argument(tb_shader_lowerer *lowerer, uint32_t parameter)
{
    const tb_shader_form_parameter *const entry =
        TB_ARRAY_AT(lowerer->function->parameters, tb_shader_form_parameter, parameter);
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_ARGUMENT, entry->type);
    expression.space =
        entry->direction == TB_SHADER_FORM_IN ? TB_SHADER_FORM_VALUE : TB_SHADER_FORM_FUNCTION;
    expression.index = parameter;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t global(tb_shader_lowerer *lowerer, uint32_t index)
{
    const tb_shader_form_global *const entry =
        TB_ARRAY_AT(lowerer->form->globals, tb_shader_form_global, index);
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_GLOBAL, entry->type);
    expression.space = entry->space;
    expression.index = index;
    return tb_shader_lower_add(lowerer, &expression);
}

uint32_t tb_shader_lower_load(tb_shader_lowerer *lowerer, uint32_t pointer)
{
    tb_shader_form_expression expression =
        new_expression(TB_SHADER_FORM_LOAD, tb_shader_lower_type_of(lowerer, pointer));
    expression.operands[0] = pointer;
    return tb_shader_lower_add(lowerer, &expression);
}

void tb_shader_lower_store(tb_shader_lowerer *lowerer, uint32_t pointer, uint32_t value)
{
    tb_shader_form_statement store = {0};
    store.kind = TB_SHADER_FORM_STORE;
    store.next = TB_SHADER_FORM_NONE;
    store.operands[0] = pointer;
    store.operands[1] = value;
    store.first = TB_SHADER_FORM_NONE;
    store.accept = TB_SHADER_FORM_NONE;
    store.reject = TB_SHADER_FORM_NONE;
    store.body = TB_SHADER_FORM_NONE;
    store.continuing = TB_SHADER_FORM_NONE;
    store.function = TB_SHADER_FORM_NONE;
    store.result = TB_SHADER_FORM_NONE;
    (void)tb_shader_lower_statement(lowerer, &store);
}

/* The type `base[index]` has: an array's element, a vector's component, a
 * matrix's row, a struct's member. */
static uint32_t element_of(tb_shader_lowerer *lowerer, uint32_t base_type, uint32_t index)
{
    const tb_shader_form_type *const type = form_type(lowerer, base_type);
    switch (type->kind)
    {
    case TB_SHADER_FORM_ARRAY:
        return type->element;
    case TB_SHADER_FORM_VECTOR:
        return tb_shader_form_scalar(lowerer->form, type->scalar);
    case TB_SHADER_FORM_MATRIX:
        return tb_shader_form_vector(lowerer->form, type->scalar, type->columns);
    case TB_SHADER_FORM_STRUCT:
        return tb_shader_form_member_at(lowerer->form, type->first_member + index)->type;
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_STRING:
    case TB_SHADER_FORM_RESOURCE:
        break;
    }
    return TB_SHADER_FORM_NONE;
}

static uint32_t access_index(tb_shader_lowerer *lowerer, uint32_t base, uint32_t index)
{
    tb_shader_form_expression expression =
        new_expression(TB_SHADER_FORM_ACCESS_INDEX,
                       element_of(lowerer, tb_shader_lower_type_of(lowerer, base), index));
    expression.space = space_of(lowerer, base);
    expression.operands[0] = base;
    expression.index = index;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t access(tb_shader_lowerer *lowerer, uint32_t base, uint32_t index)
{
    tb_shader_form_expression expression = new_expression(
        TB_SHADER_FORM_ACCESS, element_of(lowerer, tb_shader_lower_type_of(lowerer, base), 0));
    expression.space = space_of(lowerer, base);
    expression.operands[0] = base;
    expression.operands[1] = index;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t swizzle(tb_shader_lowerer *lowerer, uint32_t value, const uint8_t *components,
                        uint32_t count)
{
    const tb_shader_scalar scalar =
        form_type(lowerer, tb_shader_lower_type_of(lowerer, value))->scalar;
    const uint32_t type = count == 1 ? tb_shader_form_scalar(lowerer->form, scalar)
                                     : tb_shader_form_vector(lowerer->form, scalar, count);
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_SWIZZLE, type);
    expression.operands[0] = value;
    expression.swizzle_count = (uint8_t)count;
    for (uint32_t at = 0; at < count; at++)
    {
        expression.swizzle[at] = components[at];
    }
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t compose(tb_shader_lowerer *lowerer, uint32_t type, const uint32_t *parts,
                        uint32_t count)
{
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_COMPOSE, type);
    expression.first_operand = tb_shader_form_add_operands(lowerer->function, parts, count);
    expression.operand_count = count;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t splat(tb_shader_lowerer *lowerer, uint32_t scalar, uint32_t type)
{
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_SPLAT, type);
    expression.operands[0] = scalar;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t unary(tb_shader_lowerer *lowerer, tb_shader_form_op op, uint32_t operand,
                      uint32_t type)
{
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_UNARY, type);
    expression.op = op;
    expression.operands[0] = operand;
    return tb_shader_lower_add(lowerer, &expression);
}

uint32_t tb_shader_lower_not(tb_shader_lowerer *lowerer, uint32_t value)
{
    return unary(lowerer, TB_SHADER_FORM_OP_NOT, value, tb_shader_lower_type_of(lowerer, value));
}

static uint32_t binary(tb_shader_lowerer *lowerer, tb_shader_form_op op, uint32_t left,
                       uint32_t right, uint32_t type)
{
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_BINARY, type);
    expression.op = op;
    expression.operands[0] = left;
    expression.operands[1] = right;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t select_value(tb_shader_lowerer *lowerer, uint32_t condition, uint32_t accept,
                             uint32_t reject)
{
    tb_shader_form_expression expression =
        new_expression(TB_SHADER_FORM_SELECT, tb_shader_lower_type_of(lowerer, accept));
    expression.operands[0] = condition;
    expression.operands[1] = accept;
    expression.operands[2] = reject;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t convert_components(tb_shader_lowerer *lowerer, uint32_t value, uint32_t type)
{
    const uint32_t from = tb_shader_lower_type_of(lowerer, value);
    if (from == type)
    {
        return value;
    }
    /* A literal converted is a literal of the new type. */
    const tb_shader_form_expression *const source =
        tb_shader_form_expression_at(lowerer->function, value);
    if (source->kind == TB_SHADER_FORM_LITERAL &&
        form_type(lowerer, type)->kind == TB_SHADER_FORM_SCALAR)
    {
        const tb_shader_constant converted = tb_shader_constant_convert(
            source->value, form_type(lowerer, from)->scalar, form_type(lowerer, type)->scalar);
        return literal(lowerer, type, converted);
    }
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_CONVERT, type);
    expression.operands[0] = value;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t call_result(tb_shader_lowerer *lowerer, uint32_t type)
{
    const tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_CALL_RESULT, type);
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t new_temporary(tb_shader_lowerer *lowerer, uint32_t type)
{
    return tb_shader_form_add_local(lowerer->function, "temporary", type);
}

/* -------------------------------------------------------------------------
 * Conversions between any two types HLSL converts between.
 * ------------------------------------------------------------------------- */

/* The scalars a value is taken apart into: a tb_array of uint32_t. */
static tb_array *new_scalar_list(tb_shader_lowerer *lowerer)
{
    return TB_ARRAY_CREATE(&lowerer->allocator, uint32_t);
}

static uint32_t scalar_count(const tb_array *scalars)
{
    return (uint32_t)tb_array_length(scalars);
}

static uint32_t scalar_at(const tb_array *scalars, uint32_t at)
{
    return *TB_ARRAY_AT(scalars, uint32_t, at);
}

/* Take a value apart into its scalars, in HLSL's order: a matrix by rows, a
 * struct by members. */
static bool flatten(tb_shader_lowerer *lowerer, uint32_t value, tb_array *out)
{
    const uint32_t type = tb_shader_lower_type_of(lowerer, value);
    const tb_shader_form_type *const found = form_type(lowerer, type);
    uint32_t parts = 0;
    switch (found->kind)
    {
    case TB_SHADER_FORM_SCALAR:
        if (tb_array_length(out) == MAX_FLAT_SCALARS)
        {
            return false;
        }
        TB_ARRAY_PUSH(out, uint32_t, value);
        return true;
    case TB_SHADER_FORM_VECTOR:
        parts = found->columns;
        break;
    case TB_SHADER_FORM_MATRIX:
        parts = found->rows;
        break;
    case TB_SHADER_FORM_ARRAY:
        parts = found->length;
        break;
    case TB_SHADER_FORM_STRUCT:
        parts = found->member_count;
        break;
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_STRING:
    case TB_SHADER_FORM_RESOURCE:
        return false;
    }
    for (uint32_t index = 0; index < parts; index++)
    {
        if (!flatten(lowerer, access_index(lowerer, value, index), out))
        {
            return false;
        }
    }
    return true;
}

/* Build a value of `type` from scalars, each converted to the component it
 * fills; one scalar fills every component. */
static uint32_t rebuild(tb_shader_lowerer *lowerer, uint32_t type, const tb_array *scalars,
                        uint32_t *at)
{
    const tb_shader_form_type *const found = form_type(lowerer, type);
    if (found->kind == TB_SHADER_FORM_SCALAR)
    {
        const uint32_t count = scalar_count(scalars);
        if (count == 0 || (count > 1 && *at >= count))
        {
            return TB_SHADER_FORM_NONE;
        }
        const uint32_t scalar = scalar_at(scalars, count == 1 ? 0 : (*at)++);
        return convert_components(lowerer, scalar, type);
    }
    uint32_t parts = 0;
    switch (found->kind)
    {
    case TB_SHADER_FORM_VECTOR:
        parts = found->columns;
        break;
    case TB_SHADER_FORM_MATRIX:
        parts = found->rows;
        break;
    case TB_SHADER_FORM_ARRAY:
        parts = found->length;
        break;
    case TB_SHADER_FORM_STRUCT:
        parts = found->member_count;
        break;
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_STRING:
    case TB_SHADER_FORM_RESOURCE:
        return TB_SHADER_FORM_NONE;
    }
    uint32_t *const built = TB_ARENA_NEW_ARRAY(lowerer->arena, uint32_t, parts + 1);
    for (uint32_t index = 0; index < parts; index++)
    {
        built[index] = rebuild(lowerer, element_of(lowerer, type, index), scalars, at);
        if (built[index] == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    return compose(lowerer, type, built, parts);
}

static uint32_t rebuild_from(tb_shader_lowerer *lowerer, uint32_t type, tb_array *scalars,
                             tb_shader_location location)
{
    uint32_t at = 0;
    const uint32_t built = rebuild(lowerer, type, scalars, &at);
    if (built == TB_SHADER_FORM_NONE)
    {
        tb_shader_lower_unsupported(lowerer, location, "a value this large");
    }
    return built;
}

/* The first component of a scalar, vector or matrix. */
static uint32_t first_component(tb_shader_lowerer *lowerer, uint32_t value)
{
    const tb_shader_form_type *const type =
        form_type(lowerer, tb_shader_lower_type_of(lowerer, value));
    if (type->kind == TB_SHADER_FORM_MATRIX)
    {
        return access_index(lowerer, access_index(lowerer, value, 0), 0);
    }
    return type->kind == TB_SHADER_FORM_VECTOR ? access_index(lowerer, value, 0) : value;
}

/* The first `columns` components of a vector. */
static uint32_t first_columns(tb_shader_lowerer *lowerer, uint32_t vector, uint32_t columns)
{
    const uint8_t order[TB_SHADER_FORM_MAX_SWIZZLE] = {0, 1, 2, 3};
    const uint32_t have = form_type(lowerer, tb_shader_lower_type_of(lowerer, vector))->columns;
    return columns == have ? vector : swizzle(lowerer, vector, order, columns);
}

/* A smaller matrix of a matrix's first rows and columns. */
static uint32_t truncate_matrix(tb_shader_lowerer *lowerer, uint32_t matrix, uint32_t type)
{
    const tb_shader_form_type *const to = form_type(lowerer, type);
    const uint32_t row_type = tb_shader_form_vector(lowerer->form, to->scalar, to->columns);
    uint32_t rows[4] = {0};
    for (uint32_t row = 0; row < to->rows; row++)
    {
        const uint32_t whole = access_index(lowerer, matrix, row);
        const uint32_t part = first_columns(lowerer, whole, to->columns);
        rows[row] = convert_components(lowerer, part, row_type);
    }
    return compose(lowerer, type, rows, to->rows);
}

/* A value converted to `type`, as HLSL converts it implicitly or by a cast. */
static uint32_t convert(tb_shader_lowerer *lowerer, uint32_t value, uint32_t type,
                        tb_shader_location location)
{
    const uint32_t from = tb_shader_lower_type_of(lowerer, value);
    if (from == type)
    {
        return value;
    }
    const tb_shader_form_type *const source = form_type(lowerer, from);
    const tb_shader_form_type *const target = form_type(lowerer, type);
    const uint32_t have = tb_shader_form_component_count(lowerer->form, from);
    const uint32_t want = tb_shader_form_component_count(lowerer->form, type);
    if (have > 0 && want > 0)
    {
        if (source->kind == target->kind && source->rows == target->rows &&
            source->columns == target->columns)
        {
            return convert_components(lowerer, value, type);
        }
        if (target->kind == TB_SHADER_FORM_SCALAR || have == 1)
        {
            const uint32_t scalar =
                convert_components(lowerer, first_component(lowerer, value),
                                   tb_shader_form_scalar(lowerer->form, target->scalar));
            return target->kind == TB_SHADER_FORM_SCALAR ? scalar : splat(lowerer, scalar, type);
        }
        if (source->kind == TB_SHADER_FORM_VECTOR && target->kind == TB_SHADER_FORM_VECTOR &&
            target->columns < source->columns)
        {
            return convert_components(lowerer, first_columns(lowerer, value, target->columns),
                                      type);
        }
        if (source->kind == TB_SHADER_FORM_MATRIX && target->kind == TB_SHADER_FORM_MATRIX &&
            target->rows <= source->rows && target->columns <= source->columns)
        {
            return truncate_matrix(lowerer, value, type);
        }
    }
    tb_array *const scalars = new_scalar_list(lowerer);
    if (!flatten(lowerer, value, scalars))
    {
        tb_shader_lower_unsupported(lowerer, location, "a value this large");
        return TB_SHADER_FORM_NONE;
    }
    return rebuild_from(lowerer, type, scalars, location);
}

/* -------------------------------------------------------------------------
 * Names and places.
 * ------------------------------------------------------------------------- */

static uint32_t type_of_node(tb_shader_lowerer *lowerer, const tb_shader_node *node)
{
    return tb_shader_lower_type(lowerer, node->checked, node->location);
}

static uint32_t binding_of(const tb_shader_lowerer *lowerer, const tb_shader_node *name)
{
    return name->symbol == NULL ? TB_SHADER_FORM_NONE
                                : tb_shader_lower_bound(lowerer, lowerer->bindings, name->symbol);
}

static bool is_aggregate(const tb_shader_type *type)
{
    return type != NULL && (type->type_class == TB_SHADER_CLASS_ARRAY ||
                            type->type_class == TB_SHADER_CLASS_VECTOR ||
                            type->type_class == TB_SHADER_CLASS_MATRIX ||
                            type->type_class == TB_SHADER_CLASS_STRUCT);
}

/* Whether an expression names a place the form can point to: a variable, or
 * an element or member of one. */
/* `this`, or a field named alone in a method: of the object it was called on. */
static bool is_this(const tb_shader_node *node)
{
    return node->kind == TB_SHADER_NODE_NAME && node->symbol == NULL && node->name.length == 4 &&
           memcmp(node->name.text, "this", 4) == 0;
}

static bool is_own_field(const tb_shader_lowerer *lowerer, const tb_shader_node *node)
{
    return lowerer->has_this && node->kind == TB_SHADER_NODE_NAME && node->symbol != NULL &&
           node->symbol->kind == TB_SHADER_SYMBOL_FIELD;
}

static bool is_buffer_member(const tb_shader_lowerer *lowerer, const tb_shader_node *node)
{
    return node->kind == TB_SHADER_NODE_NAME && node->symbol != NULL &&
           tb_shader_lower_bound(lowerer, lowerer->members, node->symbol) != TB_SHADER_FORM_NONE;
}

static bool is_object(const tb_shader_type *type, tb_shader_object object)
{
    return type != NULL && type->type_class == TB_SHADER_CLASS_OBJECT && type->object == object;
}

/* `t[p]`, `b[i]`, `t.mips[m][p]`: an element of a resource. */
static bool is_resource_element(const tb_shader_node *node)
{
    if (node->kind != TB_SHADER_NODE_INDEX || node->left->checked == NULL ||
        node->left->checked->type_class != TB_SHADER_CLASS_OBJECT)
    {
        return false;
    }
    const tb_shader_type *const base = node->left->checked;
    if (is_object(base, TB_SHADER_OBJECT_MIPS_SLICE) ||
        is_object(base, TB_SHADER_OBJECT_SAMPLES_SLICE))
    {
        return true;
    }
    const tb_shader_object_info *const info = tb_shader_object_info_of(base->object);
    return info != NULL && info->index_dimensions > 0;
}

static bool is_place(const tb_shader_lowerer *lowerer, const tb_shader_node *node)
{
    if ((lowerer->has_this && is_this(node)) || is_own_field(lowerer, node) ||
        is_buffer_member(lowerer, node) || is_resource_element(node))
    {
        return true;
    }
    const bool is_buffer_field =
        node->kind == TB_SHADER_NODE_MEMBER && node->swizzle_count == 0 && node->symbol != NULL &&
        node->symbol->kind == TB_SHADER_SYMBOL_FIELD &&
        (is_object(node->left->checked, TB_SHADER_OBJECT_CONSTANT_BUFFER) ||
         is_object(node->left->checked, TB_SHADER_OBJECT_TEXTURE_BUFFER));
    if (is_buffer_field)
    {
        return is_place(lowerer, node->left);
    }
    if (node->kind == TB_SHADER_NODE_NAME)
    {
        return binding_of(lowerer, node) != TB_SHADER_FORM_NONE;
    }
    if (node->kind == TB_SHADER_NODE_INDEX)
    {
        return is_aggregate(node->left->checked) &&
               node->left->checked->type_class != TB_SHADER_CLASS_STRUCT &&
               is_place(lowerer, node->left);
    }
    return node->kind == TB_SHADER_NODE_MEMBER && node->swizzle_count == 0 &&
           node->symbol != NULL && node->symbol->kind == TB_SHADER_SYMBOL_FIELD &&
           node->left->checked != NULL &&
           node->left->checked->type_class == TB_SHADER_CLASS_STRUCT &&
           is_place(lowerer, node->left);
}

static bool constant_index(const tb_shader_node *node, uint32_t *index)
{
    if (!node->is_constant || node->constant.is_float || node->constant.bits > UINT32_MAX)
    {
        return false;
    }
    *index = (uint32_t)node->constant.bits;
    return true;
}

static uint32_t index_into(tb_shader_lowerer *lowerer, uint32_t base, tb_shader_node *index)
{
    uint32_t constant = 0;
    if (constant_index(index, &constant))
    {
        return access_index(lowerer, base, constant);
    }
    const uint32_t value = tb_shader_lower_value(lowerer, index);
    return value == TB_SHADER_FORM_NONE ? TB_SHADER_FORM_NONE : access(lowerer, base, value);
}

/* A pointer to a place `is_place` accepts. */
static uint32_t texel(tb_shader_lowerer *lowerer, tb_shader_node *node);

static uint32_t lower_place(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (is_this(node))
    {
        return tb_shader_lower_argument(lowerer, 0);
    }
    if (is_buffer_member(lowerer, node))
    {
        const uint32_t member = tb_shader_lower_bound(lowerer, lowerer->members, node->symbol);
        return access_index(lowerer, global(lowerer, tb_shader_lower_member_global(member)),
                            tb_shader_lower_member_index(member));
    }
    if (is_resource_element(node))
    {
        return texel(lowerer, node);
    }
    if (is_own_field(lowerer, node))
    {
        return access_index(lowerer, tb_shader_lower_argument(lowerer, 0),
                            (uint32_t)node->symbol->field_index);
    }
    if (node->kind == TB_SHADER_NODE_NAME)
    {
        const uint32_t binding = binding_of(lowerer, node);
        const uint32_t index = tb_shader_lower_binding_index(binding);
        switch (tb_shader_lower_binding_kind(binding))
        {
        case TB_SHADER_BINDING_LOCAL:
            return tb_shader_lower_local(lowerer, index);
        case TB_SHADER_BINDING_GLOBAL:
            return global(lowerer, index);
        case TB_SHADER_BINDING_ARGUMENT:
            return tb_shader_lower_argument(lowerer, index);
        }
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t base = lower_place(lowerer, node->left);
    if (base == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    if (node->kind == TB_SHADER_NODE_MEMBER)
    {
        return access_index(lowerer, base, (uint32_t)node->symbol->field_index);
    }
    return index_into(lowerer, base, node->right);
}

static uint32_t lower_name(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (is_place(lowerer, node))
    {
        return tb_shader_lower_load(lowerer, lower_place(lowerer, node));
    }
    const tb_shader_symbol *const symbol = node->symbol;
    const bool is_input = symbol != NULL && symbol->storage == TB_SHADER_STORAGE_UNIFORM;
    tb_shader_lower_unsupported(lowerer, node->location,
                                is_input ? "a shader input (a constant, a buffer or a resource)"
                                         : "this name");
    return TB_SHADER_FORM_NONE;
}

/* Store into what an assignment's left side names: a place, or a swizzle
 * of one, component by component. */
static bool store_to(tb_shader_lowerer *lowerer, tb_shader_node *target, uint32_t value)
{
    if (is_place(lowerer, target))
    {
        tb_shader_lower_store(lowerer, lower_place(lowerer, target), value);
        return true;
    }
    const bool is_swizzle = target->kind == TB_SHADER_NODE_MEMBER && target->swizzle_count > 0 &&
                            is_place(lowerer, target->left) && target->left->checked != NULL;
    if (!is_swizzle)
    {
        tb_shader_lower_unsupported(lowerer, target->location, "storing to this expression");
        return false;
    }
    const uint32_t base = lower_place(lowerer, target->left);
    const bool is_matrix = target->left->checked->type_class == TB_SHADER_CLASS_MATRIX;
    const bool is_scalar = target->left->checked->type_class == TB_SHADER_CLASS_SCALAR;
    for (uint8_t at = 0; at < target->swizzle_count; at++)
    {
        const uint8_t component = target->swizzle[at];
        uint32_t pointer = base;
        if (is_matrix)
        {
            pointer =
                access_index(lowerer, access_index(lowerer, base, component / 4u), component % 4u);
        }
        else if (!is_scalar)
        {
            pointer = access_index(lowerer, base, component);
        }
        const uint32_t part = target->swizzle_count == 1 ? value : access_index(lowerer, value, at);
        tb_shader_lower_store(lowerer, pointer, part);
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Literals, members and indexing.
 * ------------------------------------------------------------------------- */

static uint32_t constant_value(tb_shader_lowerer *lowerer, const tb_shader_node *node)
{
    const tb_shader_scalar scalar = tb_shader_concrete_scalar(node->checked->scalar);
    const tb_shader_constant value =
        tb_shader_constant_convert(node->constant, node->checked->scalar, scalar);
    return literal(lowerer, tb_shader_form_scalar(lowerer->form, scalar), value);
}

static bool is_scalar_constant(const tb_shader_node *node)
{
    return node->is_constant && node->checked != NULL &&
           (node->checked->type_class == TB_SHADER_CLASS_SCALAR ||
            node->checked->type_class == TB_SHADER_CLASS_ENUM);
}

static uint32_t lower_swizzle(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t base = tb_shader_lower_value(lowerer, node->left);
    if (base == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const tb_shader_form_type *const type =
        form_type(lowerer, tb_shader_lower_type_of(lowerer, base));
    const uint32_t result = type_of_node(lowerer, node);
    if (type->kind == TB_SHADER_FORM_VECTOR)
    {
        return swizzle(lowerer, base, node->swizzle, node->swizzle_count);
    }
    if (type->kind == TB_SHADER_FORM_SCALAR)
    {
        return node->swizzle_count == 1 ? base : splat(lowerer, base, result);
    }
    uint32_t parts[TB_SHADER_FORM_MAX_SWIZZLE] = {0};
    for (uint8_t at = 0; at < node->swizzle_count; at++)
    {
        const uint8_t component = node->swizzle[at];
        parts[at] =
            access_index(lowerer, access_index(lowerer, base, component / 4u), component % 4u);
    }
    return node->swizzle_count == 1 ? parts[0]
                                    : compose(lowerer, result, parts, node->swizzle_count);
}

static uint32_t lower_member(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (node->swizzle_count > 0)
    {
        return lower_swizzle(lowerer, node);
    }
    if (is_place(lowerer, node))
    {
        return tb_shader_lower_load(lowerer, lower_place(lowerer, node));
    }
    const bool is_field = node->symbol != NULL && node->symbol->kind == TB_SHADER_SYMBOL_FIELD &&
                          node->left->checked != NULL &&
                          node->left->checked->type_class == TB_SHADER_CLASS_STRUCT;
    if (!is_field)
    {
        tb_shader_lower_unsupported(lowerer, node->location, "a member of a buffer or an object");
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t base = tb_shader_lower_value(lowerer, node->left);
    return base == TB_SHADER_FORM_NONE
               ? TB_SHADER_FORM_NONE
               : access_index(lowerer, base, (uint32_t)node->symbol->field_index);
}

/* A pointer to what a resource holds at a location, and at a level or a
 * sample for `t.mips[m][p]` and `t.sample[s][p]`. */
static uint32_t texel(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const tb_shader_type *const base = node->left->checked;
    tb_shader_node *resource = node->left;
    uint32_t level = TB_SHADER_FORM_NONE;
    uint32_t which = 0;
    if (is_object(base, TB_SHADER_OBJECT_MIPS_SLICE) ||
        is_object(base, TB_SHADER_OBJECT_SAMPLES_SLICE))
    {
        which = is_object(base, TB_SHADER_OBJECT_MIPS_SLICE) ? 1u : 2u;
        resource = node->left->left->left;
        level = tb_shader_lower_value(lowerer, node->left->right);
        if (level == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    const uint32_t handle = tb_shader_lower_value(lowerer, resource);
    const uint32_t location = tb_shader_lower_value(lowerer, node->right);
    if (handle == TB_SHADER_FORM_NONE || location == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t element = form_type(lowerer, tb_shader_lower_type_of(lowerer, handle))->element;
    if (element == TB_SHADER_FORM_NONE)
    {
        tb_shader_lower_unsupported(lowerer, node->location, "indexing this resource");
        return TB_SHADER_FORM_NONE;
    }
    tb_shader_form_expression expression = new_expression(TB_SHADER_FORM_TEXEL, element);
    expression.space = TB_SHADER_FORM_ELEMENT;
    expression.operands[0] = handle;
    expression.operands[1] = location;
    expression.operands[2] = level;
    expression.index = which;
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t lower_index(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (is_place(lowerer, node))
    {
        return tb_shader_lower_load(lowerer, lower_place(lowerer, node));
    }
    if (!is_aggregate(node->left->checked) ||
        node->left->checked->type_class == TB_SHADER_CLASS_STRUCT)
    {
        tb_shader_lower_unsupported(lowerer, node->location, "reading a resource");
        return TB_SHADER_FORM_NONE;
    }
    if (is_place(lowerer, node))
    {
        return tb_shader_lower_load(lowerer, lower_place(lowerer, node));
    }
    const uint32_t base = tb_shader_lower_value(lowerer, node->left);
    return base == TB_SHADER_FORM_NONE ? TB_SHADER_FORM_NONE
                                       : index_into(lowerer, base, node->right);
}

/* -------------------------------------------------------------------------
 * Operators.
 * ------------------------------------------------------------------------- */

static tb_shader_form_op form_op(tb_shader_operator op)
{
    switch (op)
    {
    case TB_SHADER_OP_NEGATE:
        return TB_SHADER_FORM_OP_NEGATE;
    case TB_SHADER_OP_NOT:
        return TB_SHADER_FORM_OP_NOT;
    case TB_SHADER_OP_BIT_NOT:
        return TB_SHADER_FORM_OP_BIT_NOT;
    case TB_SHADER_OP_MULTIPLY:
    case TB_SHADER_OP_MULTIPLY_ASSIGN:
        return TB_SHADER_FORM_OP_MULTIPLY;
    case TB_SHADER_OP_DIVIDE:
    case TB_SHADER_OP_DIVIDE_ASSIGN:
        return TB_SHADER_FORM_OP_DIVIDE;
    case TB_SHADER_OP_REMAINDER:
    case TB_SHADER_OP_REMAINDER_ASSIGN:
        return TB_SHADER_FORM_OP_REMAINDER;
    case TB_SHADER_OP_ADD:
    case TB_SHADER_OP_ADD_ASSIGN:
    case TB_SHADER_OP_PRE_INCREMENT:
    case TB_SHADER_OP_POST_INCREMENT:
        return TB_SHADER_FORM_OP_ADD;
    case TB_SHADER_OP_SUBTRACT:
    case TB_SHADER_OP_SUBTRACT_ASSIGN:
    case TB_SHADER_OP_PRE_DECREMENT:
    case TB_SHADER_OP_POST_DECREMENT:
        return TB_SHADER_FORM_OP_SUBTRACT;
    case TB_SHADER_OP_SHIFT_LEFT:
    case TB_SHADER_OP_SHIFT_LEFT_ASSIGN:
        return TB_SHADER_FORM_OP_SHIFT_LEFT;
    case TB_SHADER_OP_SHIFT_RIGHT:
    case TB_SHADER_OP_SHIFT_RIGHT_ASSIGN:
        return TB_SHADER_FORM_OP_SHIFT_RIGHT;
    case TB_SHADER_OP_LESS:
        return TB_SHADER_FORM_OP_LESS;
    case TB_SHADER_OP_GREATER:
        return TB_SHADER_FORM_OP_GREATER;
    case TB_SHADER_OP_LESS_EQUAL:
        return TB_SHADER_FORM_OP_LESS_EQUAL;
    case TB_SHADER_OP_GREATER_EQUAL:
        return TB_SHADER_FORM_OP_GREATER_EQUAL;
    case TB_SHADER_OP_EQUAL:
        return TB_SHADER_FORM_OP_EQUAL;
    case TB_SHADER_OP_NOT_EQUAL:
        return TB_SHADER_FORM_OP_NOT_EQUAL;
    case TB_SHADER_OP_BIT_AND:
    case TB_SHADER_OP_BIT_AND_ASSIGN:
        return TB_SHADER_FORM_OP_BIT_AND;
    case TB_SHADER_OP_BIT_XOR:
    case TB_SHADER_OP_BIT_XOR_ASSIGN:
        return TB_SHADER_FORM_OP_BIT_XOR;
    case TB_SHADER_OP_BIT_OR:
    case TB_SHADER_OP_BIT_OR_ASSIGN:
        return TB_SHADER_FORM_OP_BIT_OR;
    case TB_SHADER_OP_AND:
        return TB_SHADER_FORM_OP_AND;
    case TB_SHADER_OP_OR:
        return TB_SHADER_FORM_OP_OR;
    case TB_SHADER_OP_NONE:
    case TB_SHADER_OP_PLUS:
    case TB_SHADER_OP_COMMA:
    case TB_SHADER_OP_ASSIGN:
        break;
    }
    return TB_SHADER_FORM_OP_NONE;
}

static bool is_step(tb_shader_operator op)
{
    return op == TB_SHADER_OP_PRE_INCREMENT || op == TB_SHADER_OP_PRE_DECREMENT ||
           op == TB_SHADER_OP_POST_INCREMENT || op == TB_SHADER_OP_POST_DECREMENT;
}

/* Whether an expression may be evaluated or not without anyone telling: it
 * calls nothing that has an effect, and assigns nothing. */
static bool is_pure(const tb_shader_node *node)
{
    if (node == NULL)
    {
        return true;
    }
    if (node->kind == TB_SHADER_NODE_ASSIGN ||
        (node->kind == TB_SHADER_NODE_UNARY && is_step(node->op)))
    {
        return false;
    }
    if (node->kind == TB_SHADER_NODE_CALL)
    {
        const bool pure_intrinsic = node->intrinsic != 0 && node->outputs == 0 &&
                                    node->checked != NULL &&
                                    node->checked->type_class != TB_SHADER_CLASS_VOID &&
                                    node->intrinsic != (uint32_t)TB_SHADER_INTRINSIC_REPORT_HIT;
        if (!pure_intrinsic)
        {
            return false;
        }
    }
    for (const tb_shader_node *child = node->children; child != NULL; child = child->next)
    {
        if (!is_pure(child))
        {
            return false;
        }
    }
    return is_pure(node->left) && is_pure(node->right) && is_pure(node->condition);
}

/* `++x`, `x--`: add or take one, store, and give the new or the old value. */
static uint32_t lower_step(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    tb_shader_node *const target = node->left;
    const uint32_t type = type_of_node(lowerer, node);
    const uint32_t target_type = type_of_node(lowerer, target);
    if (type == TB_SHADER_FORM_NONE || target_type == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const bool through_pointer = is_place(lowerer, target);
    const uint32_t pointer = through_pointer ? lower_place(lowerer, target) : TB_SHADER_FORM_NONE;
    const uint32_t loaded = through_pointer ? tb_shader_lower_load(lowerer, pointer)
                                            : tb_shader_lower_value(lowerer, target);
    if (loaded == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t old_value = convert(lowerer, loaded, type, node->location);
    const tb_shader_form_type *const shape = form_type(lowerer, type);
    const uint32_t scalar = tb_shader_form_scalar(lowerer->form, shape->scalar);
    const tb_shader_constant one_value = tb_shader_constant_convert(
        (tb_shader_constant){false, 1, 0.0}, TB_SHADER_SCALAR_INT, shape->scalar);
    uint32_t one = literal(lowerer, scalar, one_value);
    if (shape->kind != TB_SHADER_FORM_SCALAR)
    {
        one = splat(lowerer, one, type);
    }
    const uint32_t new_value = binary(lowerer, form_op(node->op), old_value, one, type);
    const uint32_t stored = convert(lowerer, new_value, target_type, node->location);
    if (through_pointer)
    {
        tb_shader_lower_store(lowerer, pointer, stored);
    }
    else if (!store_to(lowerer, target, stored))
    {
        return TB_SHADER_FORM_NONE;
    }
    const bool is_prefix =
        node->op == TB_SHADER_OP_PRE_INCREMENT || node->op == TB_SHADER_OP_PRE_DECREMENT;
    return is_prefix ? new_value : old_value;
}

static uint32_t lower_unary(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (is_step(node->op))
    {
        return lower_step(lowerer, node);
    }
    const uint32_t operand = tb_shader_lower_value(lowerer, node->left);
    if (operand == TB_SHADER_FORM_NONE || node->op == TB_SHADER_OP_PLUS)
    {
        return operand;
    }
    return unary(lowerer, form_op(node->op), operand, type_of_node(lowerer, node));
}

/* A bool local, assigned on each path, for an operator that branches. */
static uint32_t branch_value(tb_shader_lowerer *lowerer, uint32_t condition, uint32_t type,
                             tb_shader_node *accept, tb_shader_node *reject, uint32_t otherwise)
{
    const uint32_t temporary = new_temporary(lowerer, type);
    if (otherwise != TB_SHADER_FORM_NONE)
    {
        tb_shader_lower_store(lowerer, tb_shader_lower_local(lowerer, temporary), otherwise);
    }
    tb_shader_lower_flush(lowerer);
    tb_shader_form_statement test = {0};
    test.kind = TB_SHADER_FORM_IF;
    test.next = TB_SHADER_FORM_NONE;
    test.operands[0] = condition;
    test.operands[1] = TB_SHADER_FORM_NONE;
    test.first = TB_SHADER_FORM_NONE;
    test.reject = TB_SHADER_FORM_NONE;
    test.body = TB_SHADER_FORM_NONE;
    test.continuing = TB_SHADER_FORM_NONE;
    test.function = TB_SHADER_FORM_NONE;
    test.result = TB_SHADER_FORM_NONE;
    tb_shader_node *const arms[2] = {accept, reject};
    uint32_t *const blocks[2] = {&test.accept, &test.reject};
    for (size_t arm = 0; arm < 2; arm++)
    {
        if (arms[arm] == NULL)
        {
            continue;
        }
        tb_shader_block_mark mark = {0};
        *blocks[arm] = tb_shader_lower_open_block(lowerer, &mark);
        const uint32_t value = tb_shader_lower_value(lowerer, arms[arm]);
        if (value != TB_SHADER_FORM_NONE)
        {
            tb_shader_lower_store(lowerer, tb_shader_lower_local(lowerer, temporary), value);
        }
        tb_shader_lower_close_block(lowerer, &mark);
    }
    (void)tb_shader_lower_statement(lowerer, &test);
    return tb_shader_lower_load(lowerer, tb_shader_lower_local(lowerer, temporary));
}

/* `a && b` and `a || b`: `b` is evaluated only when it decides. */
static uint32_t lower_logical(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t left = tb_shader_lower_value(lowerer, node->left);
    if (left == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t type = tb_shader_lower_type_of(lowerer, left);
    if (is_pure(node->right))
    {
        const uint32_t right = tb_shader_lower_value(lowerer, node->right);
        return right == TB_SHADER_FORM_NONE ? TB_SHADER_FORM_NONE
                                            : binary(lowerer, form_op(node->op), left, right, type);
    }
    const uint32_t condition =
        node->op == TB_SHADER_OP_AND ? left : tb_shader_lower_not(lowerer, left);
    return branch_value(lowerer, condition, type, node->right, NULL, left);
}

static uint32_t lower_binary(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (node->op == TB_SHADER_OP_COMMA)
    {
        tb_shader_lower_effect(lowerer, node->left);
        return tb_shader_lower_value(lowerer, node->right);
    }
    if (node->op == TB_SHADER_OP_AND || node->op == TB_SHADER_OP_OR)
    {
        return lower_logical(lowerer, node);
    }
    const uint32_t left = tb_shader_lower_value(lowerer, node->left);
    const uint32_t right = tb_shader_lower_value(lowerer, node->right);
    if (left == TB_SHADER_FORM_NONE || right == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    return binary(lowerer, form_op(node->op), left, right, type_of_node(lowerer, node));
}

static uint32_t lower_conditional(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t condition = tb_shader_lower_value(lowerer, node->condition);
    const uint32_t type = type_of_node(lowerer, node);
    if (condition == TB_SHADER_FORM_NONE || type == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    if (is_pure(node->left) && is_pure(node->right))
    {
        const uint32_t accept = tb_shader_lower_value(lowerer, node->left);
        const uint32_t reject = tb_shader_lower_value(lowerer, node->right);
        return accept == TB_SHADER_FORM_NONE || reject == TB_SHADER_FORM_NONE
                   ? TB_SHADER_FORM_NONE
                   : select_value(lowerer, condition, accept, reject);
    }
    return branch_value(lowerer, condition, type, node->left, node->right, TB_SHADER_FORM_NONE);
}

static uint32_t lower_assign(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    tb_shader_node *const target = node->left;
    if (node->op == TB_SHADER_OP_ASSIGN)
    {
        const uint32_t value = tb_shader_lower_value(lowerer, node->right);
        return value != TB_SHADER_FORM_NONE && store_to(lowerer, target, value)
                   ? value
                   : TB_SHADER_FORM_NONE;
    }
    /* `a op= b`: `a` converted to the operation's type, which `b` has. */
    const uint32_t target_type = type_of_node(lowerer, target);
    const bool through_pointer = is_place(lowerer, target);
    const uint32_t pointer = through_pointer ? lower_place(lowerer, target) : TB_SHADER_FORM_NONE;
    const uint32_t old_value = through_pointer ? tb_shader_lower_load(lowerer, pointer)
                                               : tb_shader_lower_value(lowerer, target);
    const uint32_t right = tb_shader_lower_value(lowerer, node->right);
    if (old_value == TB_SHADER_FORM_NONE || right == TB_SHADER_FORM_NONE ||
        target_type == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t type = tb_shader_lower_type_of(lowerer, right);
    const uint32_t left = convert(lowerer, old_value, type, node->location);
    const uint32_t result = binary(lowerer, form_op(node->op), left, right, type);
    const uint32_t stored = convert(lowerer, result, target_type, node->location);
    if (through_pointer)
    {
        tb_shader_lower_store(lowerer, pointer, stored);
        return stored;
    }
    return store_to(lowerer, target, stored) ? stored : TB_SHADER_FORM_NONE;
}

/* -------------------------------------------------------------------------
 * Constructors, casts and initializer lists.
 * ------------------------------------------------------------------------- */

static bool flatten_list(tb_shader_lowerer *lowerer, tb_shader_node *list, tb_array *out)
{
    for (tb_shader_node *child = list->children; child != NULL; child = child->next)
    {
        if (child->kind == TB_SHADER_NODE_INITIALIZER_LIST)
        {
            if (!flatten_list(lowerer, child, out))
            {
                return false;
            }
            continue;
        }
        const uint32_t value = tb_shader_lower_value(lowerer, child);
        if (value == TB_SHADER_FORM_NONE || !flatten(lowerer, value, out))
        {
            return false;
        }
    }
    return true;
}

static uint32_t lower_list(tb_shader_lowerer *lowerer, tb_shader_node *list, uint32_t type)
{
    tb_array *const scalars = new_scalar_list(lowerer);
    if (!flatten_list(lowerer, list, scalars))
    {
        return TB_SHADER_FORM_NONE;
    }
    return rebuild_from(lowerer, type, scalars, list->location);
}

uint32_t tb_shader_lower_initializer(tb_shader_lowerer *lowerer, tb_shader_node *node,
                                     const tb_shader_type *type)
{
    if (node->kind != TB_SHADER_NODE_INITIALIZER_LIST)
    {
        return tb_shader_lower_value(lowerer, node);
    }
    const uint32_t form =
        tb_shader_lower_type(lowerer, node->checked != NULL ? node->checked : type, node->location);
    return form == TB_SHADER_FORM_NONE ? TB_SHADER_FORM_NONE : lower_list(lowerer, node, form);
}

/* `float4(a, b.xy, 1)`: its arguments are of its component already. A
 * vector is made of them as they are; a matrix of its rows when they are
 * rows; anything else, from their scalars. */
static uint32_t lower_construct(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t type = type_of_node(lowerer, node);
    if (type == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const tb_shader_form_type *const target = form_type(lowerer, type);
    uint32_t *const parts = TB_ARENA_NEW_ARRAY(lowerer->arena, uint32_t, node->child_count + 1);
    uint32_t count = 0;
    bool as_they_are =
        target->kind == TB_SHADER_FORM_VECTOR || target->kind == TB_SHADER_FORM_MATRIX;
    const uint32_t row = target->kind == TB_SHADER_FORM_MATRIX
                             ? tb_shader_form_vector(lowerer->form, target->scalar, target->columns)
                             : TB_SHADER_FORM_NONE;
    for (tb_shader_node *child = node->children; child != NULL; child = child->next)
    {
        const uint32_t value = tb_shader_lower_value(lowerer, child);
        if (value == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
        const tb_shader_form_type_kind kind =
            form_type(lowerer, tb_shader_lower_type_of(lowerer, value))->kind;
        if (target->kind == TB_SHADER_FORM_VECTOR)
        {
            as_they_are = as_they_are && kind != TB_SHADER_FORM_MATRIX;
        }
        else if (target->kind == TB_SHADER_FORM_MATRIX)
        {
            as_they_are = as_they_are && tb_shader_lower_type_of(lowerer, value) == row;
        }
        parts[count++] = value;
    }
    if (as_they_are)
    {
        return compose(lowerer, type, parts, count);
    }
    tb_array *const scalars = new_scalar_list(lowerer);
    for (uint32_t at = 0; at < count; at++)
    {
        if (!flatten(lowerer, parts[at], scalars))
        {
            tb_shader_lower_unsupported(lowerer, node->location, "a value this large");
            return TB_SHADER_FORM_NONE;
        }
    }
    return rebuild_from(lowerer, type, scalars, node->location);
}

/* A cast, or a conversion checking made: the operand as its new type. */
static uint32_t lower_conversion(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t value = tb_shader_lower_value(lowerer, node->left);
    const uint32_t type = type_of_node(lowerer, node);
    if (value == TB_SHADER_FORM_NONE || type == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    return convert(lowerer, value, type, node->location);
}

/* -------------------------------------------------------------------------
 * Calls.
 * ------------------------------------------------------------------------- */

/* An argument passed `out` or `inout` to a function, or to an intrinsic
 * whose parameter is of another type than the place: a local holds it, and
 * is copied back after the call. */
typedef struct output
{
    tb_shader_node *target;
    uint32_t local;
} output;

static uint32_t through_local(tb_shader_lowerer *lowerer, tb_shader_node *argument, uint32_t type,
                              bool copy_in, output *saved)
{
    const uint32_t local = new_temporary(lowerer, type);
    if (copy_in)
    {
        const uint32_t value = tb_shader_lower_value(lowerer, argument);
        if (value == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
        tb_shader_lower_store(lowerer, tb_shader_lower_local(lowerer, local),
                              convert(lowerer, value, type, argument->location));
    }
    saved->target = argument;
    saved->local = local;
    return tb_shader_lower_local(lowerer, local);
}

static void copy_back(tb_shader_lowerer *lowerer, const output *outputs, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        const uint32_t value =
            tb_shader_lower_load(lowerer, tb_shader_lower_local(lowerer, outputs[index].local));
        const uint32_t type = type_of_node(lowerer, outputs[index].target);
        if (type != TB_SHADER_FORM_NONE)
        {
            (void)store_to(lowerer, outputs[index].target,
                           convert(lowerer, value, type, outputs[index].target->location));
        }
    }
}

static uint32_t add_call(tb_shader_lowerer *lowerer, uint32_t function, uint32_t intrinsic,
                         uint32_t method, const uint32_t *arguments, uint32_t count,
                         uint32_t result_type)
{
    tb_shader_form_statement call = {0};
    call.kind = TB_SHADER_FORM_CALL;
    call.next = TB_SHADER_FORM_NONE;
    call.operands[0] = TB_SHADER_FORM_NONE;
    call.operands[1] = TB_SHADER_FORM_NONE;
    call.first = TB_SHADER_FORM_NONE;
    call.accept = TB_SHADER_FORM_NONE;
    call.reject = TB_SHADER_FORM_NONE;
    call.body = TB_SHADER_FORM_NONE;
    call.continuing = TB_SHADER_FORM_NONE;
    call.function = function;
    call.intrinsic = intrinsic;
    call.method = method;
    call.first_operand = tb_shader_form_add_operands(lowerer->function, arguments, count);
    call.operand_count = count;
    call.result = TB_SHADER_FORM_NONE;
    const bool returns = form_type(lowerer, result_type)->kind != TB_SHADER_FORM_VOID;
    if (returns)
    {
        call.result = call_result(lowerer, result_type);
    }
    (void)tb_shader_lower_statement(lowerer, &call);
    return returns ? call.result : VOID_RESULT;
}

/* The object a method is called on, as a pointer of the method's `this`:
 * the object's own place when it is a local, through a local otherwise. */
static uint32_t object_pointer(tb_shader_lowerer *lowerer, tb_shader_node *node, uint32_t type,
                               output *saved, size_t *saved_count)
{
    tb_shader_node *const object =
        node->left->kind == TB_SHADER_NODE_MEMBER ? node->left->left : NULL;
    if (object == NULL)
    {
        if (!lowerer->has_this)
        {
            tb_shader_lower_unsupported(lowerer, node->location,
                                        "calling a method without an object");
            return TB_SHADER_FORM_NONE;
        }
        return tb_shader_lower_argument(lowerer, 0);
    }
    if (type_of_node(lowerer, object) != type)
    {
        tb_shader_lower_unsupported(lowerer, node->location,
                                    "calling a base struct's method on a derived struct");
        return TB_SHADER_FORM_NONE;
    }
    if (is_place(lowerer, object))
    {
        const uint32_t pointer = lower_place(lowerer, object);
        if (space_of(lowerer, pointer) == TB_SHADER_FORM_FUNCTION)
        {
            return pointer;
        }
        return through_local(lowerer, object, type, true, &saved[(*saved_count)++]);
    }
    const uint32_t value = tb_shader_lower_value(lowerer, object);
    if (value == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t local = new_temporary(lowerer, type);
    tb_shader_lower_store(lowerer, tb_shader_lower_local(lowerer, local), value);
    return tb_shader_lower_local(lowerer, local);
}

static uint32_t lower_function_call(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t index = tb_shader_lower_bound(lowerer, lowerer->functions, node->symbol);
    if (index == TB_SHADER_FORM_NONE)
    {
        tb_shader_lower_unsupported(lowerer, node->location, "calling a function without a body");
        return TB_SHADER_FORM_NONE;
    }
    const tb_shader_form_function *const callee = tb_shader_form_function_at(lowerer->form, index);
    const uint32_t count = (uint32_t)tb_array_length(callee->parameters);
    uint32_t *const arguments = TB_ARENA_NEW_ARRAY(lowerer->arena, uint32_t, count + 1);
    output *const outputs = TB_ARENA_NEW_ARRAY(lowerer->arena, output, count + 1);
    size_t output_count = 0;
    uint32_t at = 0;
    if (tb_shader_lower_takes_this(node->symbol))
    {
        const uint32_t self = TB_ARRAY_AT(callee->parameters, tb_shader_form_parameter, 0)->type;
        arguments[0] = object_pointer(lowerer, node, self, outputs, &output_count);
        if (arguments[0] == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
        at = 1;
    }
    /* An argument left out is its parameter's default value. */
    tb_shader_node *declared =
        node->symbol->declaration != NULL ? node->symbol->declaration->parameters : NULL;
    tb_shader_node *given = node->children;
    for (; at < count; at++)
    {
        tb_shader_node *const argument =
            given != NULL ? given : (declared != NULL ? declared->value : NULL);
        given = given != NULL ? given->next : NULL;
        declared = declared != NULL ? declared->next : NULL;
        if (argument == NULL)
        {
            tb_shader_lower_unsupported(lowerer, node->location, "a call missing an argument");
            return TB_SHADER_FORM_NONE;
        }
        const tb_shader_form_parameter *const parameter =
            TB_ARRAY_AT(callee->parameters, tb_shader_form_parameter, at);
        if (parameter->direction == TB_SHADER_FORM_IN)
        {
            arguments[at] = tb_shader_lower_value(lowerer, argument);
        }
        else
        {
            arguments[at] = through_local(lowerer, argument, parameter->type,
                                          parameter->direction == TB_SHADER_FORM_INOUT,
                                          &outputs[output_count++]);
        }
        if (arguments[at] == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    const uint32_t result = add_call(lowerer, index, 0, 0, arguments, at, callee->result);
    copy_back(lowerer, outputs, output_count);
    return result;
}

/* Whether a method changes what it is called on, or what it returns
 * depends on when it is called: every method of a RayQuery, and those that
 * count or consume. */
static bool method_has_effect(const tb_shader_node *node)
{
    const tb_shader_method method = (tb_shader_method)node->method;
    return is_object(node->left->left->checked, TB_SHADER_OBJECT_RAY_QUERY) ||
           method == TB_SHADER_METHOD_INCREMENT_COUNTER ||
           method == TB_SHADER_METHOD_DECREMENT_COUNTER || method == TB_SHADER_METHOD_CONSUME;
}

/* The object a method is called on: a pointer to a RayQuery, which its
 * methods change, or a resource's handle. */
static uint32_t method_object(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    tb_shader_node *const object = node->left->left;
    if (!is_object(object->checked, TB_SHADER_OBJECT_RAY_QUERY))
    {
        return tb_shader_lower_value(lowerer, object);
    }
    if (!is_place(lowerer, object))
    {
        tb_shader_lower_unsupported(lowerer, node->location, "a RayQuery that is not a variable");
        return TB_SHADER_FORM_NONE;
    }
    return lower_place(lowerer, object);
}

/* An intrinsic or a method with no output and no effect is an expression;
 * any other is a call, whose outputs point to their places when the types
 * agree. A method's object is its first operand. */
static uint32_t lower_intrinsic_call(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    const uint32_t result_type = type_of_node(lowerer, node);
    if (result_type == TB_SHADER_FORM_NONE)
    {
        return TB_SHADER_FORM_NONE;
    }
    const bool is_method = node->method != 0;
    const uint32_t first = is_method ? 1u : 0u;
    const uint32_t count = (uint32_t)node->child_count + first;
    uint32_t *const arguments = TB_ARENA_NEW_ARRAY(lowerer->arena, uint32_t, count + 1);
    output *const outputs = TB_ARENA_NEW_ARRAY(lowerer->arena, output, count + 1);
    size_t output_count = 0;
    if (is_method)
    {
        arguments[0] = method_object(lowerer, node);
        if (arguments[0] == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    uint32_t at = 0;
    for (tb_shader_node *argument = node->children; argument != NULL;
         argument = argument->next, at++)
    {
        const bool is_output = at < 64 && (node->outputs & (UINT64_C(1) << at)) != 0;
        uint32_t *const slot = &arguments[at + first];
        if (!is_output)
        {
            *slot = tb_shader_lower_value(lowerer, argument);
        }
        else
        {
            const uint32_t parameter =
                tb_shader_lower_type(lowerer, node->parameter_types[at], argument->location);
            if (parameter == TB_SHADER_FORM_NONE)
            {
                return TB_SHADER_FORM_NONE;
            }
            const bool direct =
                is_place(lowerer, argument) && type_of_node(lowerer, argument) == parameter;
            *slot = direct ? lower_place(lowerer, argument)
                           : through_local(lowerer, argument, parameter, true,
                                           &outputs[output_count++]);
        }
        if (*slot == TB_SHADER_FORM_NONE)
        {
            return TB_SHADER_FORM_NONE;
        }
    }
    const bool has_effect = node->outputs != 0 ||
                            form_type(lowerer, result_type)->kind == TB_SHADER_FORM_VOID ||
                            node->intrinsic == (uint32_t)TB_SHADER_INTRINSIC_REPORT_HIT ||
                            (is_method && method_has_effect(node));
    if (!has_effect)
    {
        tb_shader_form_expression expression =
            new_expression(TB_SHADER_FORM_INTRINSIC, result_type);
        expression.intrinsic = node->intrinsic;
        expression.method = node->method;
        expression.first_operand = tb_shader_form_add_operands(lowerer->function, arguments, count);
        expression.operand_count = count;
        return tb_shader_lower_add(lowerer, &expression);
    }
    const uint32_t result = add_call(lowerer, TB_SHADER_FORM_NONE, node->intrinsic, node->method,
                                     arguments, count, result_type);
    copy_back(lowerer, outputs, output_count);
    return result;
}

static uint32_t lower_call(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (node->intrinsic != 0 || node->method != 0)
    {
        return lower_intrinsic_call(lowerer, node);
    }
    if (node->symbol == NULL)
    {
        tb_shader_lower_unsupported(lowerer, node->location, "calling this");
        return TB_SHADER_FORM_NONE;
    }
    return lower_function_call(lowerer, node);
}

/* -------------------------------------------------------------------------
 * Expressions.
 * ------------------------------------------------------------------------- */

/* `"x is %d"`: its text between the quotes, as written. */
static uint32_t string_literal(tb_shader_lowerer *lowerer, const tb_shader_node *node)
{
    const tb_shader_name spelling = node->name;
    const size_t length = spelling.length >= 2 ? spelling.length - 2 : 0;
    tb_shader_form_expression expression =
        new_expression(TB_SHADER_FORM_STRING_LITERAL, tb_shader_form_string(lowerer->form));
    expression.index = tb_shader_form_add_string(lowerer->form, spelling.text + 1, length);
    return tb_shader_lower_add(lowerer, &expression);
}

static uint32_t lower_expression(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (is_scalar_constant(node))
    {
        return constant_value(lowerer, node);
    }
    switch (node->kind)
    {
    case TB_SHADER_NODE_NAME:
        return lower_name(lowerer, node);
    case TB_SHADER_NODE_UNARY:
        return lower_unary(lowerer, node);
    case TB_SHADER_NODE_BINARY:
        return lower_binary(lowerer, node);
    case TB_SHADER_NODE_ASSIGN:
        return lower_assign(lowerer, node);
    case TB_SHADER_NODE_CONDITIONAL:
        return lower_conditional(lowerer, node);
    case TB_SHADER_NODE_CALL:
        return lower_call(lowerer, node);
    case TB_SHADER_NODE_CONSTRUCT:
        return lower_construct(lowerer, node);
    case TB_SHADER_NODE_CAST:
    case TB_SHADER_NODE_CONVERT:
        return lower_conversion(lowerer, node);
    case TB_SHADER_NODE_INDEX:
        return lower_index(lowerer, node);
    case TB_SHADER_NODE_MEMBER:
        return lower_member(lowerer, node);
    case TB_SHADER_NODE_INITIALIZER_LIST: {
        const uint32_t type = type_of_node(lowerer, node);
        return type == TB_SHADER_FORM_NONE ? TB_SHADER_FORM_NONE : lower_list(lowerer, node, type);
    }
    case TB_SHADER_NODE_STRING:
        return string_literal(lowerer, node);
    case TB_SHADER_NODE_INTEGER:
    case TB_SHADER_NODE_FLOAT:
    case TB_SHADER_NODE_BOOL:
    case TB_SHADER_NODE_SIZEOF:
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
        break;
    }
    tb_shader_lower_unsupported(lowerer, node->location, "this expression");
    return TB_SHADER_FORM_NONE;
}

uint32_t tb_shader_lower_value(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (node == NULL)
    {
        return TB_SHADER_FORM_NONE;
    }
    const uint32_t value = lower_expression(lowerer, node);
    if (value == VOID_RESULT)
    {
        tb_shader_lower_unsupported(lowerer, node->location, "the value of a call of nothing");
        return TB_SHADER_FORM_NONE;
    }
    return value;
}

void tb_shader_lower_effect(tb_shader_lowerer *lowerer, tb_shader_node *node)
{
    if (node != NULL)
    {
        (void)lower_expression(lowerer, node);
    }
}
