#include "shader_msl.h"

#include "badger/fmt.h"
#include "badger/str.h"

#include <stdarg.h>
#include <string.h>

typedef struct msl_writer
{
    const tb_shader_form *form;
    const tb_shader_form_function *function;
    tb_array *text;
    tb_allocator allocator;
    const char **values;
    const char *reason;
    tb_shader_location location;
} msl_writer;

static void append(msl_writer *writer, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *const line = tb_fmt_vstring(&writer->allocator, format, arguments);
    va_end(arguments);
    tb_array_append(writer->text, line, tb_str_length(line));
}

static const char *type_name(msl_writer *writer, uint32_t index)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(writer->form, index);
    if (type->kind == TB_SHADER_FORM_STRUCT)
    {
        return tb_fmt_string(&writer->allocator, "s%u", index);
    }
    if ((type->kind == TB_SHADER_FORM_SCALAR || type->kind == TB_SHADER_FORM_VECTOR) &&
        (type->scalar == TB_SHADER_SCALAR_BOOL || type->scalar == TB_SHADER_SCALAR_INT ||
         type->scalar == TB_SHADER_SCALAR_UINT || type->scalar == TB_SHADER_SCALAR_FLOAT))
    {
        return type->name;
    }
    writer->reason = "Metal output does not support this value type yet";
    return "";
}

static const char *literal(msl_writer *writer, const tb_shader_form_expression *expr)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(writer->form, expr->type);
    const uint32_t bits = (uint32_t)expr->value.bits;
    switch (type->scalar)
    {
    case TB_SHADER_SCALAR_BOOL:
        return bits != 0 ? "true" : "false";
    case TB_SHADER_SCALAR_INT:
        return tb_fmt_string(&writer->allocator, "as_type<int>(0x%xu)", bits);
    case TB_SHADER_SCALAR_UINT:
        return tb_fmt_string(&writer->allocator, "%uu", bits);
    case TB_SHADER_SCALAR_FLOAT: {
        const float number = (float)expr->value.value;
        uint32_t representation = 0;
        memcpy(&representation, &number, sizeof(number));
        return tb_fmt_string(&writer->allocator, "as_type<float>(0x%xu)", representation);
    }
    case TB_SHADER_SCALAR_INT16:
    case TB_SHADER_SCALAR_UINT16:
    case TB_SHADER_SCALAR_INT64:
    case TB_SHADER_SCALAR_UINT64:
    case TB_SHADER_SCALAR_MIN16INT:
    case TB_SHADER_SCALAR_MIN16UINT:
    case TB_SHADER_SCALAR_MIN16FLOAT:
    case TB_SHADER_SCALAR_HALF:
    case TB_SHADER_SCALAR_DOUBLE:
    case TB_SHADER_SCALAR_LITERAL_INT:
    case TB_SHADER_SCALAR_LITERAL_FLOAT:
    default:
        writer->reason = "Metal output does not support this literal type yet";
        return "";
    }
}

static const char *expression(msl_writer *writer, const tb_shader_form_expression *expr)
{
    static const char *const operators[] = {"",   "-",  "!",  "~",  "+", "-", "*",  "/",
                                            "%",  "<<", ">>", "&",  "|", "^", "&&", "||",
                                            "==", "!=", "<",  "<=", ">", ">="};
    tb_allocator *const allocator = &writer->allocator;
    const char **const values = writer->values;
    switch (expr->kind)
    {
    case TB_SHADER_FORM_LITERAL:
        return literal(writer, expr);
    case TB_SHADER_FORM_ARGUMENT:
        return tb_fmt_string(allocator, "p%u", expr->index);
    case TB_SHADER_FORM_GLOBAL:
        return tb_fmt_string(allocator, "g%u", expr->index);
    case TB_SHADER_FORM_LOCAL:
        return tb_fmt_string(allocator, "l%u", expr->index);
    case TB_SHADER_FORM_LOAD:
        return values[expr->operands[0]];
    case TB_SHADER_FORM_ACCESS_INDEX: {
        const tb_shader_form_expression *const base =
            tb_shader_form_expression_at(writer->function, expr->operands[0]);
        const tb_shader_form_type *const type = tb_shader_form_type_at(writer->form, base->type);
        if (type->kind == TB_SHADER_FORM_STRUCT)
        {
            return tb_fmt_string(allocator, "%s.m%u", values[expr->operands[0]], expr->index);
        }
        return tb_fmt_string(allocator, "%s[%u]", values[expr->operands[0]], expr->index);
    }
    case TB_SHADER_FORM_CONVERT:
    case TB_SHADER_FORM_SPLAT:
        return tb_fmt_string(allocator, "%s(%s)", type_name(writer, expr->type),
                             values[expr->operands[0]]);
    case TB_SHADER_FORM_COMPOSE: {
        const char *args = "";
        for (uint32_t i = 0; i < expr->operand_count; i++)
        {
            args = tb_fmt_string(
                allocator, "%s%s%s", args, i == 0 ? "" : ", ",
                values[tb_shader_form_operand_at(writer->function, expr->first_operand + i)]);
        }
        return tb_fmt_string(allocator, "%s(%s)", type_name(writer, expr->type), args);
    }
    case TB_SHADER_FORM_UNARY:
        return tb_fmt_string(allocator, "(%s%s)", operators[expr->op], values[expr->operands[0]]);
    case TB_SHADER_FORM_BINARY:
        return tb_fmt_string(allocator, "(%s %s %s)", values[expr->operands[0]],
                             operators[expr->op], values[expr->operands[1]]);
    case TB_SHADER_FORM_ACCESS:
    case TB_SHADER_FORM_SWIZZLE:
    case TB_SHADER_FORM_SELECT:
    case TB_SHADER_FORM_INTRINSIC:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
    case TB_SHADER_FORM_TEXEL:
    case TB_SHADER_FORM_HEAP:
    default:
        writer->reason = "Metal output does not support this expression yet";
        return "";
    }
}

static void block(msl_writer *writer, uint32_t index)
{
    const tb_shader_form_function *const fn = writer->function;
    for (uint32_t at = tb_shader_form_statement_at(fn, index)->first;
         at != TB_SHADER_FORM_NONE && writer->reason == NULL;
         at = tb_shader_form_statement_at(fn, at)->next)
    {
        const tb_shader_form_statement *const stmt = tb_shader_form_statement_at(fn, at);
        writer->location = stmt->location;
        switch (stmt->kind)
        {
        case TB_SHADER_FORM_EMIT:
            for (uint32_t i = stmt->operands[0]; i < stmt->operands[1] && writer->reason == NULL;
                 i++)
            {
                const tb_shader_form_expression *const expr = tb_shader_form_expression_at(fn, i);
                writer->location = expr->location;
                writer->values[i] = expression(writer, expr);
                if (tb_shader_form_is_emitted(expr->kind) && expr->space == TB_SHADER_FORM_VALUE)
                {
                    append(writer, "    const %s e%u = %s;\n", type_name(writer, expr->type), i,
                           writer->values[i]);
                    writer->values[i] = tb_fmt_string(&writer->allocator, "e%u", i);
                }
            }
            break;
        case TB_SHADER_FORM_STORE:
            append(writer, "    %s = %s;\n", writer->values[stmt->operands[0]],
                   writer->values[stmt->operands[1]]);
            break;
        case TB_SHADER_FORM_RETURN:
            append(writer, "    return %s;\n", writer->values[stmt->operands[0]]);
            break;
        case TB_SHADER_FORM_BLOCK:
            append(writer, "{\n");
            block(writer, at);
            append(writer, "}\n");
            break;
        case TB_SHADER_FORM_IF:
        case TB_SHADER_FORM_LOOP:
        case TB_SHADER_FORM_SWITCH:
        case TB_SHADER_FORM_CASE:
        case TB_SHADER_FORM_BREAK:
        case TB_SHADER_FORM_CONTINUE:
        case TB_SHADER_FORM_DISCARD:
        case TB_SHADER_FORM_CALL:
        default:
            writer->reason = "Metal output does not support this statement yet";
            break;
        }
    }
}

static bool uniform_layout(const tb_shader_form *form, uint32_t index)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(form, index);
    if (type->kind != TB_SHADER_FORM_STRUCT)
    {
        return type->kind == TB_SHADER_FORM_VECTOR && type->columns == 4 &&
               (type->scalar == TB_SHADER_SCALAR_FLOAT || type->scalar == TB_SHADER_SCALAR_INT ||
                type->scalar == TB_SHADER_SCALAR_UINT);
    }
    for (uint32_t i = 0; i < type->member_count; i++)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(form, type->first_member + i);
        if (member->packoffset != TB_SHADER_FORM_NONE || !uniform_layout(form, member->type))
        {
            return false;
        }
    }
    return true;
}

static void write_source(msl_writer *writer, const tb_shader_reflection *reflection,
                         tb_arena *arena)
{
    const tb_shader_form *const form = writer->form;
    const tb_shader_form_entry_point *const entry =
        TB_ARRAY_AT(form->entry_points, tb_shader_form_entry_point, 0);
    const tb_shader_form_function *const fn = tb_shader_form_function_at(form, entry->function);
    writer->function = fn;
    writer->location = fn->location;
    const bool vertex = entry->stage == TB_SHADER_FORM_STAGE_VERTEX;
    const tb_shader_form_type *const result = tb_shader_form_type_at(form, fn->result);
    if ((!vertex && entry->stage != TB_SHADER_FORM_STAGE_PIXEL) || !fn->result_semantic ||
        !(tb_str_equal(fn->result_semantic, vertex ? "SV_Position" : "SV_Target") ||
          (!vertex && tb_str_equal(fn->result_semantic, "SV_Target0"))) ||
        result->kind != TB_SHADER_FORM_VECTOR || result->scalar != TB_SHADER_SCALAR_FLOAT ||
        result->columns != 4 || tb_shader_form_function_count(form) != 1)
    {
        writer->reason = "Metal output needs a vertex position or pixel color returned as float4 "
                         "from one function";
        return;
    }
    append(writer, "#include <metal_stdlib>\nusing namespace metal;\n");
    for (size_t i = 0; i < tb_array_length(form->types); i++)
    {
        const tb_shader_form_type *const type = tb_shader_form_type_at(form, (uint32_t)i);
        if (type->kind != TB_SHADER_FORM_STRUCT)
        {
            continue;
        }
        append(writer, "struct s%zu {\n", i);
        for (uint32_t j = 0; j < type->member_count; j++)
        {
            const tb_shader_form_member *const member =
                tb_shader_form_member_at(form, type->first_member + j);
            append(writer, "    %s m%u;\n", type_name(writer, member->type), j);
        }
        append(writer, "};\n");
    }
    append(writer, "%s float4 tb_main(", vertex ? "vertex" : "fragment");
    const size_t params = tb_array_length(fn->parameters);
    for (size_t i = 0; i < params; i++)
    {
        const tb_shader_form_parameter *const p =
            TB_ARRAY_AT(fn->parameters, tb_shader_form_parameter, i);
        const tb_shader_form_type *const type = tb_shader_form_type_at(form, p->type);
        if (!vertex || !p->semantic || !tb_str_equal(p->semantic, "SV_VertexID") ||
            p->direction != TB_SHADER_FORM_IN || type->kind != TB_SHADER_FORM_SCALAR ||
            type->scalar != TB_SHADER_SCALAR_UINT || params != 1)
        {
            writer->reason = "Metal output supports a uint SV_VertexID vertex input";
            return;
        }
        append(writer, "uint p%zu [[vertex_id]]", i);
    }
    for (size_t i = 0; i < tb_array_length(form->globals); i++)
    {
        const tb_shader_form_global *const g = TB_ARRAY_AT(form->globals, tb_shader_form_global, i);
        const tb_shader_binding *binding = NULL;
        for (size_t j = 0; j < reflection->binding_count; j++)
        {
            if (tb_str_equal(g->name, reflection->bindings[j].name))
            {
                binding = &reflection->bindings[j];
                break;
            }
        }
        if (!binding || binding->kind != TB_SHADER_BINDING_CONSTANT_BUFFER || binding->space != 0 ||
            binding->register_index > 2 || !uniform_layout(form, g->type))
        {
            writer->reason = "Metal output supports constant buffers of four-component 32-bit "
                             "vectors at b0 through b2";
            return;
        }
        append(writer, "%sconstant %s &g%zu [[buffer(%u)]]", params != 0 || i != 0 ? ", " : "",
               type_name(writer, g->type), i, binding->register_index);
    }
    append(writer, ") {\n");
    for (size_t i = 0; i < tb_array_length(fn->locals); i++)
    {
        append(writer, "    %s l%zu{};\n",
               type_name(writer, TB_ARRAY_AT(fn->locals, tb_shader_form_local, i)->type), i);
    }
    const size_t count = tb_array_length(fn->expressions);
    writer->values = tb_arena_calloc(arena, count == 0 ? 1 : count, sizeof(*writer->values),
                                     _Alignof(const char *));
    for (size_t i = 0; i < count; i++)
    {
        const tb_shader_form_expression *const expr = tb_shader_form_expression_at(fn, (uint32_t)i);
        if (expr->kind == TB_SHADER_FORM_LITERAL || expr->kind == TB_SHADER_FORM_LOCAL ||
            expr->kind == TB_SHADER_FORM_GLOBAL || expr->kind == TB_SHADER_FORM_ARGUMENT)
        {
            writer->values[i] = expression(writer, expr);
        }
    }
    block(writer, fn->body);
    append(writer, "}\n");
}

bool tb_shader_write_msl(const tb_shader_form *form, const tb_shader_reflection *reflection,
                         tb_arena *arena, const char **source, size_t *size,
                         tb_shader_location *location, const char **reason)
{
    msl_writer writer = {0};
    writer.form = form;
    writer.allocator = tb_arena_allocator(arena);
    writer.text = TB_ARRAY_CREATE(&writer.allocator, char);
    write_source(&writer, reflection, arena);
    *location = writer.location;
    *reason = writer.reason;
    if (writer.reason != NULL)
    {
        return false;
    }
    *source = tb_array_data(writer.text);
    *size = tb_array_length(writer.text);
    return true;
}
