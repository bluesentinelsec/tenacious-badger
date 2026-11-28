#include "shader_uniform.h"
#include "badger/str.h"
#include "shader_intrinsics.h"

/* Where a heap index is: a constant buffer's register, a place in it, and
 * the type there and the layout that describes it. */
typedef struct index_source
{
    const tb_shader_constant_buffer *buffer;
    uint32_t buffer_register;
    uint32_t type;      /* What is at `offset`. */
    size_t variable;    /* The layout's variable of `type`, or SIZE_MAX for the buffer. */
    uint32_t offset;    /* Bytes from the buffer's start. */
    bool is_array_item; /* An element of the variable's array, not the array. */
} index_source;

const tb_shader_binding *tb_shader_binding_named(const tb_shader_reflection *reflection,
                                                 const char *name)
{
    for (size_t index = 0; index < reflection->binding_count; index++)
    {
        if (tb_str_equal(reflection->bindings[index].name, name))
        {
            return &reflection->bindings[index];
        }
    }
    return NULL;
}

static const tb_shader_constant_buffer *buffer_named(const tb_shader_hlsl_request *request,
                                                     const char *name)
{
    const tb_shader_reflection *const reflection = request->reflection;
    for (size_t index = 0; index < reflection->buffer_count; index++)
    {
        if (tb_str_equal(reflection->buffers[index].name, name))
        {
            return &reflection->buffers[index];
        }
    }
    return NULL;
}

/* Step into a member, a component or an element, `index`, of what a source is at. */
static bool step_into(const tb_shader_hlsl_request *request, index_source *source, uint32_t index)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(request->form, source->type);
    if (type->kind == TB_SHADER_FORM_STRUCT && index < type->member_count)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(request->form, type->first_member + index);
        size_t variable = index;
        uint32_t base = 0;
        if (source->variable != SIZE_MAX)
        {
            const tb_shader_variable *const owner = &source->buffer->variables[source->variable];
            variable = owner->first_member + index;
            base = owner->offset;
        }
        if (variable >= source->buffer->variable_count)
        {
            return false;
        }
        /* A struct's members are laid out for its first element: offsets
         * from where that element starts carry to any. */
        const uint32_t start = source->variable == SIZE_MAX ? 0u : base;
        source->offset = source->offset + source->buffer->variables[variable].offset - start;
        source->variable = variable;
        source->type = member->type;
        source->is_array_item = false;
        return true;
    }
    if (type->kind == TB_SHADER_FORM_ARRAY && source->variable != SIZE_MAX &&
        !source->is_array_item && (type->length == 0 || index < type->length))
    {
        source->offset += source->buffer->variables[source->variable].stride * index;
        source->type = type->element;
        source->is_array_item = true;
        return true;
    }
    if (type->kind == TB_SHADER_FORM_VECTOR && index < type->columns)
    {
        source->offset += 4u * index;
        source->type =
            tb_shader_form_find_numeric(request->form, TB_SHADER_FORM_SCALAR, type->scalar, 1, 1);
        return source->type != TB_SHADER_FORM_NONE;
    }
    return false;
}

/* Trace where a value or a place an expression of `function` gives comes
 * from: false unless it is read from a constant buffer at a place known
 * when the shader compiles. */
static bool trace(const tb_shader_hlsl_request *request, const tb_shader_form_function *function,
                  uint32_t index, index_source *source)
{
    const tb_shader_form_expression *const expression =
        tb_shader_form_expression_at(function, index);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_GLOBAL: {
        const tb_shader_form_global *const global =
            TB_ARRAY_AT(request->form->globals, tb_shader_form_global, expression->index);
        const tb_shader_binding *const binding =
            tb_shader_binding_named(request->reflection, global->name);
        if (global->space != TB_SHADER_FORM_UNIFORM || binding == NULL || binding->space != 0 ||
            binding->register_index >= TB_SHADER_DXBC_CONSTANT_BUFFERS)
        {
            return false;
        }
        source->buffer = buffer_named(request, global->name);
        source->buffer_register = binding->register_index;
        source->type = global->type;
        source->variable = global->typed_buffer ? 0 : SIZE_MAX;
        source->offset = 0;
        return source->buffer != NULL;
    }
    case TB_SHADER_FORM_LOAD:
        return trace(request, function, expression->operands[0], source);
    case TB_SHADER_FORM_ACCESS_INDEX:
        return trace(request, function, expression->operands[0], source) &&
               step_into(request, source, expression->index);
    case TB_SHADER_FORM_ACCESS: {
        const tb_shader_form_expression *const at =
            tb_shader_form_expression_at(function, expression->operands[1]);
        return at->kind == TB_SHADER_FORM_LITERAL &&
               trace(request, function, expression->operands[0], source) &&
               step_into(request, source, (uint32_t)at->value.bits);
    }
    case TB_SHADER_FORM_SWIZZLE:
        return expression->swizzle_count == 1 &&
               trace(request, function, expression->operands[0], source) &&
               step_into(request, source, expression->swizzle[0]);
    case TB_SHADER_FORM_CONVERT: {
        const tb_shader_form_type *const from = tb_shader_form_type_at(
            request->form, tb_shader_form_expression_at(function, expression->operands[0])->type);
        const bool same_bits =
            from->scalar == TB_SHADER_SCALAR_INT || from->scalar == TB_SHADER_SCALAR_UINT;
        return same_bits && trace(request, function, expression->operands[0], source);
    }
    case TB_SHADER_FORM_INTRINSIC:
        if (expression->intrinsic == TB_SHADER_INTRINSIC_NON_UNIFORM_RESOURCE_INDEX &&
            expression->operand_count == 1)
        {
            return trace(request, function,
                         tb_shader_form_operand_at(function, expression->first_operand), source);
        }
        return false;
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_COMPOSE:
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_BINARY:
    case TB_SHADER_FORM_SELECT:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
    case TB_SHADER_FORM_TEXEL:
    case TB_SHADER_FORM_HEAP:
        break;
    }
    return false;
}

bool tb_shader_uniform_index(const tb_shader_hlsl_request *request,
                             const tb_shader_form_function *function, uint32_t expression,
                             uint32_t *buffer, uint32_t *offset)
{
    index_source source = {0};
    if (!trace(request, function, expression, &source))
    {
        return false;
    }
    const tb_shader_form_type *const type = tb_shader_form_type_at(request->form, source.type);
    if (type->kind != TB_SHADER_FORM_SCALAR ||
        (type->scalar != TB_SHADER_SCALAR_INT && type->scalar != TB_SHADER_SCALAR_UINT))
    {
        return false;
    }
    *buffer = source.buffer_register;
    *offset = source.offset;
    return true;
}
