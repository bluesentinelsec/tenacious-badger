#include "shader_msl.h"
#include "shader_intrinsics.h"
#include "shader_methods.h"
#include "shader_msl_bindings.h"

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
    uint32_t vertex_input;
    bool heaps[TB_MSL_HEAP_COUNT];
    const char *context_args;
    const char *context_params;
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

static tb_msl_heap heap_for(tb_shader_object object)
{
    static const struct
    {
        tb_shader_object object;
        tb_msl_heap heap;
    } kinds[] = {{TB_SHADER_OBJECT_BYTE_ADDRESS_BUFFER, TB_MSL_BUFFERS},
                 {TB_SHADER_OBJECT_RW_BYTE_ADDRESS_BUFFER, TB_MSL_BUFFERS},
                 {TB_SHADER_OBJECT_TEXTURE2D, TB_MSL_TEXTURES},
                 {TB_SHADER_OBJECT_TEXTURE2D_ARRAY, TB_MSL_ARRAY_TEXTURES},
                 {TB_SHADER_OBJECT_TEXTURE_CUBE, TB_MSL_CUBES},
                 {TB_SHADER_OBJECT_RW_TEXTURE2D, TB_MSL_STORAGE_TEXTURES},
                 {TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY, TB_MSL_STORAGE_ARRAYS},
                 {TB_SHADER_OBJECT_SAMPLER, TB_MSL_SAMPLERS}};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
    {
        if (object == kinds[i].object)
        {
            return kinds[i].heap;
        }
    }
    return TB_MSL_HEAP_COUNT;
}

static const char *type_name(msl_writer *writer, uint32_t index)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(writer->form, index);
    if (type->kind == TB_SHADER_FORM_VOID)
    {
        return "void";
    }
    if (type->kind == TB_SHADER_FORM_RESOURCE && heap_for(type->object) != TB_MSL_HEAP_COUNT)
    {
        return "uint";
    }
    if (type->kind == TB_SHADER_FORM_MATRIX && type->scalar == TB_SHADER_SCALAR_FLOAT &&
        type->rows == 4 && type->columns == 4)
    {
        return "float4x4";
    }
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

static const char *arguments(msl_writer *writer, uint32_t first, uint32_t count)
{
    const char *args = "";
    for (uint32_t i = 0; i < count; i++)
    {
        args =
            tb_fmt_string(&writer->allocator, "%s%s%s", args, i ? ", " : "",
                          writer->values[tb_shader_form_operand_at(writer->function, first + i)]);
    }
    return args;
}

static const tb_shader_form_expression *arg(msl_writer *w, uint32_t first, uint32_t index)
{
    return tb_shader_form_expression_at(w->function,
                                        tb_shader_form_operand_at(w->function, first + index));
}
static const char *arg_value(msl_writer *w, uint32_t first, uint32_t index)
{
    return w->values[tb_shader_form_operand_at(w->function, first + index)];
}
static const char *resource(msl_writer *w, const tb_shader_form_expression *expr, const char *index)
{
    const tb_msl_heap heap = heap_for(tb_shader_form_type_at(w->form, expr->type)->object);
    if (heap == TB_MSL_HEAP_COUNT)
    {
        w->reason = "unsupported Metal resource type";
        return "";
    }
    return tb_fmt_string(&w->allocator, "h%u.items[%s]", (unsigned)heap, index);
}
static const char *method(msl_writer *w, const tb_shader_form_expression *expr)
{
    const uint32_t first = expr->first_operand;
    if (expr->operand_count < 2)
    {
        w->reason = "unsupported Metal resource method";
        return "";
    }
    const char *const object = resource(w, arg(w, first, 0), arg_value(w, first, 0));
    const char *const coordinate = arg_value(w, first, 1);
    const tb_shader_form_type *const type = tb_shader_form_type_at(w->form, expr->type);
    const char *const suffix = type->columns == 1   ? ".x"
                               : type->columns == 2 ? ".xy"
                               : type->columns == 3 ? ".xyz"
                                                    : "";
    const uint32_t id = expr->method;
    if (id == TB_SHADER_METHOD_LOAD_BYTES && expr->operand_count == 2)
    {
        const char *words = "";
        for (uint32_t i = 0; i < type->columns; i++)
        {
            words = tb_fmt_string(&w->allocator, "%s%s%s[(%s >> 2) + %uu]", words, i ? ", " : "",
                                  object, coordinate, i);
        }
        const char *const uint_type =
            type->columns == 1 ? "uint" : tb_fmt_string(&w->allocator, "uint%u", type->columns);
        return tb_fmt_string(&w->allocator, "as_type<%s>(%s(%s))", type_name(w, expr->type),
                             uint_type, words);
    }
    if (id == TB_SHADER_METHOD_LOAD_MIP && expr->operand_count == 2)
    {
        return tb_fmt_string(&w->allocator, "%s.read(uint2(%s.xy),uint(%s.z))%s", object,
                             coordinate, coordinate, suffix);
    }
    if ((id == TB_SHADER_METHOD_SAMPLE || id == TB_SHADER_METHOD_SAMPLE_LEVEL) &&
        (expr->operand_count == 3 || expr->operand_count == 4))
    {
        const char *const sampler = resource(w, arg(w, first, 1), coordinate);
        const char *const level =
            expr->operand_count == 4
                ? tb_fmt_string(&w->allocator, ",level(%s)", arg_value(w, first, 3))
                : "";
        return tb_fmt_string(&w->allocator, "%s.sample(%s,%s%s)%s", object, sampler,
                             arg_value(w, first, 2), level, suffix);
    }
    w->reason = tb_fmt_string(&w->allocator, "unsupported Metal resource method %s",
                              tb_shader_method_info_of((tb_shader_method)id)->name);
    return "";
}
static const char *intrinsic(msl_writer *writer, const tb_shader_form_expression *expr)
{
    if (expr->method != 0)
    {
        return method(writer, expr);
    }
    const tb_shader_intrinsic id = (tb_shader_intrinsic)expr->intrinsic;
    const char *const name = tb_shader_intrinsic_info_of(id)->name;
    if (id == TB_SHADER_INTRINSIC_SELECT)
    {
        return tb_fmt_string(
            &writer->allocator, "select(%s,%s,%s)", arg_value(writer, expr->first_operand, 2),
            arg_value(writer, expr->first_operand, 1), arg_value(writer, expr->first_operand, 0));
    }
    if (id == TB_SHADER_INTRINSIC_MUL)
    {
        const uint32_t left = tb_shader_form_operand_at(writer->function, expr->first_operand);
        const uint32_t right = tb_shader_form_operand_at(writer->function, expr->first_operand + 1);
        const tb_shader_form_type *const lhs = tb_shader_form_type_at(
            writer->form, tb_shader_form_expression_at(writer->function, left)->type);
        const tb_shader_form_type *const rhs = tb_shader_form_type_at(
            writer->form, tb_shader_form_expression_at(writer->function, right)->type);
        if (lhs->kind == TB_SHADER_FORM_VECTOR && rhs->kind == TB_SHADER_FORM_VECTOR)
        {
            return tb_fmt_string(&writer->allocator, "dot(%s, %s)", writer->values[left],
                                 writer->values[right]);
        }
        return tb_fmt_string(&writer->allocator, "(%s * %s)", writer->values[left],
                             writer->values[right]);
    }
    static const char *const supported[] = {
        "abs",  "all", "any", "clamp",     "cross", "dot",     "exp2",  "length",
        "lerp", "max", "min", "normalize", "pow",   "reflect", "rsqrt", "saturate",
        "sqrt", "ddx", "ddy", "floor",     "round", "fwidth",  "exp",   "smoothstep"};
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++)
    {
        if (tb_str_equal(name, supported[i]))
        {
            const char *const translated = id == TB_SHADER_INTRINSIC_LERP  ? "mix"
                                           : id == TB_SHADER_INTRINSIC_DDX ? "dfdx"
                                           : id == TB_SHADER_INTRINSIC_DDY ? "dfdy"
                                                                           : name;
            return tb_fmt_string(&writer->allocator, "%s(%s)", translated,
                                 arguments(writer, expr->first_operand, expr->operand_count));
        }
    }
    writer->reason =
        tb_fmt_string(&writer->allocator, "Metal output does not support %s yet", name);
    return "";
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
            if (base->type == writer->vertex_input)
            {
                const tb_shader_form_member *const member =
                    tb_shader_form_member_at(writer->form, type->first_member + expr->index);
                if (member->semantic)
                {
                    return tb_fmt_string(allocator, "%s.a_%s", values[expr->operands[0]],
                                         member->semantic);
                }
            }
            return tb_fmt_string(allocator, "%s.m%u", values[expr->operands[0]], expr->index);
        }
        return tb_fmt_string(allocator, "%s[%u]", values[expr->operands[0]], expr->index);
    }
    case TB_SHADER_FORM_CONVERT:
    case TB_SHADER_FORM_SPLAT:
        return tb_fmt_string(allocator, "%s(%s)", type_name(writer, expr->type),
                             values[expr->operands[0]]);
    case TB_SHADER_FORM_COMPOSE: {
        const char *const args = arguments(writer, expr->first_operand, expr->operand_count);
        const tb_shader_form_type *const type = tb_shader_form_type_at(writer->form, expr->type);
        return tb_fmt_string(allocator, type->kind == TB_SHADER_FORM_STRUCT ? "%s{%s}" : "%s(%s)",
                             type_name(writer, expr->type), args);
    }
    case TB_SHADER_FORM_UNARY:
        return tb_fmt_string(allocator, "(%s%s)", operators[expr->op], values[expr->operands[0]]);
    case TB_SHADER_FORM_BINARY:
        return tb_fmt_string(allocator, "(%s %s %s)", values[expr->operands[0]],
                             operators[expr->op], values[expr->operands[1]]);
    case TB_SHADER_FORM_ACCESS:
        return tb_fmt_string(allocator, "%s[%s]", values[expr->operands[0]],
                             values[expr->operands[1]]);
    case TB_SHADER_FORM_SWIZZLE: {
        char letters[TB_SHADER_FORM_MAX_SWIZZLE + 1] = {0};
        for (uint8_t i = 0; i < expr->swizzle_count; i++)
        {
            letters[i] = "xyzw"[expr->swizzle[i]];
        }
        return tb_fmt_string(allocator, "%s.%s", values[expr->operands[0]], letters);
    }
    case TB_SHADER_FORM_SELECT:
        return tb_fmt_string(allocator, "select(%s,%s,%s)", values[expr->operands[2]],
                             values[expr->operands[1]], values[expr->operands[0]]);
    case TB_SHADER_FORM_INTRINSIC:
        return intrinsic(writer, expr);
    case TB_SHADER_FORM_HEAP:
        return values[expr->operands[0]];
    case TB_SHADER_FORM_TEXEL: {
        const tb_shader_form_expression *const handle =
            tb_shader_form_expression_at(writer->function, expr->operands[0]);
        const char *const texture = resource(writer, handle, values[expr->operands[0]]);
        const char *const coord = values[expr->operands[1]];
        const bool array = tb_shader_form_type_at(writer->form, handle->type)->object ==
                           TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY;
        return tb_fmt_string(allocator, "%s.read(uint2(%s.xy)%s)", texture, coord,
                             array ? tb_fmt_string(allocator, ",uint(%s.z)", coord) : "");
    }
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
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
                if (!tb_shader_form_is_emitted(expr->kind))
                {
                    continue;
                }
                writer->values[i] = expression(writer, expr);
                if (tb_shader_form_is_emitted(expr->kind) && expr->space == TB_SHADER_FORM_VALUE)
                {
                    append(writer, "    const %s e%u = %s;\n", type_name(writer, expr->type), i,
                           writer->values[i]);
                    writer->values[i] = tb_fmt_string(&writer->allocator, "e%u", i);
                }
            }
            break;
        case TB_SHADER_FORM_STORE: {
            const tb_shader_form_expression *const ptr =
                tb_shader_form_expression_at(fn, stmt->operands[0]);
            if (ptr->kind == TB_SHADER_FORM_TEXEL)
            {
                const tb_shader_form_expression *const handle =
                    tb_shader_form_expression_at(fn, ptr->operands[0]);
                const char *const coord = writer->values[ptr->operands[1]];
                const bool array = tb_shader_form_type_at(writer->form, handle->type)->object ==
                                   TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY;
                append(writer, "%s.write(%s,uint2(%s.xy)%s);\n",
                       resource(writer, handle, writer->values[ptr->operands[0]]),
                       writer->values[stmt->operands[1]], coord,
                       array ? tb_fmt_string(&writer->allocator, ",uint(%s.z)", coord) : "");
                break;
            }
            append(writer, "    %s = %s;\n", writer->values[stmt->operands[0]],
                   writer->values[stmt->operands[1]]);
            break;
        }
        case TB_SHADER_FORM_RETURN:
            append(writer, "    return %s;\n",
                   stmt->operands[0] == TB_SHADER_FORM_NONE ? ""
                                                            : writer->values[stmt->operands[0]]);
            break;
        case TB_SHADER_FORM_BLOCK:
            append(writer, "{\n");
            block(writer, at);
            append(writer, "}\n");
            break;
        case TB_SHADER_FORM_IF:
            append(writer, "if (%s) {\n", writer->values[stmt->operands[0]]);
            block(writer, stmt->accept);
            if (stmt->reject != TB_SHADER_FORM_NONE)
            {
                append(writer, "} else {\n");
                block(writer, stmt->reject);
            }
            append(writer, "}\n");
            break;
        case TB_SHADER_FORM_LOOP:
            append(writer, "bool first%u=true;\nwhile (true) {\nif(!first%u) {\n", at, at);
            block(writer, stmt->continuing);
            if (stmt->operands[0] != TB_SHADER_FORM_NONE)
            {
                append(writer, "if (%s) { break; }\n", writer->values[stmt->operands[0]]);
            }
            append(writer, "}\nfirst%u=false;\n", at);
            block(writer, stmt->body);
            append(writer, "}\n");
            break;
        case TB_SHADER_FORM_BREAK:
            append(writer, "break;\n");
            break;
        case TB_SHADER_FORM_CONTINUE:
            append(writer, "continue;\n");
            break;
        case TB_SHADER_FORM_CALL: {
            const uint32_t first = stmt->first_operand;
            if (stmt->method == TB_SHADER_METHOD_STORE && stmt->operand_count == 3)
            {
                const char *const buffer =
                    resource(writer, arg(writer, first, 0), arg_value(writer, first, 0));
                const uint32_t columns =
                    tb_shader_form_type_at(writer->form, arg(writer, first, 2)->type)->columns;
                for (uint32_t i = 0; i < columns; i++)
                {
                    const char *const component =
                        columns == 1 ? "" : tb_fmt_string(&writer->allocator, "[%u]", i);
                    append(writer, "%s[(%s >> 2)+%uu]=as_type<uint>(%s%s);\n", buffer,
                           arg_value(writer, first, 1), i, arg_value(writer, first, 2), component);
                }
                break;
            }
            if (stmt->function == TB_SHADER_FORM_NONE)
            {
                writer->reason = "Metal output does not support this statement intrinsic";
                break;
            }
            if (stmt->result != TB_SHADER_FORM_NONE)
            {
                writer->values[stmt->result] =
                    tb_fmt_string(&writer->allocator, "e%u", stmt->result);
                append(writer, "const %s e%u = ",
                       type_name(writer, tb_shader_form_expression_at(fn, stmt->result)->type),
                       stmt->result);
            }
            append(writer, "f%u(%s%s%s);\n", stmt->function,
                   arguments(writer, first, stmt->operand_count),
                   stmt->operand_count && writer->context_args[0] ? ", " : "",
                   writer->context_args);
            break;
        }
        case TB_SHADER_FORM_DISCARD:
            append(writer, "discard_fragment();\n");
            break;
        case TB_SHADER_FORM_SWITCH:
        case TB_SHADER_FORM_CASE:
        default:
            writer->reason = "Metal output does not support this statement yet";
            break;
        }
    }
}

static void function_body(msl_writer *writer, const tb_shader_form_function *fn, tb_arena *arena)
{
    writer->function = fn;
    writer->location = fn->location;
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

/* Compare the layout of supported native MSL fields with reflection before
 * borrowing a constant buffer as a struct. A scalar cannot silently move
 * across a 16-byte HLSL register or into Metal float3 padding. */
static bool uniform_layout(const tb_shader_form *form, const tb_shader_form_global *global,
                           const tb_shader_constant_buffer *buffer)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(form, global->type);
    const uint32_t start = global->typed_buffer ? buffer->variables[0].first_member : 0;
    if (type->kind != TB_SHADER_FORM_STRUCT || start > buffer->variable_count ||
        type->member_count > buffer->variable_count - start)
    {
        return false;
    }
    size_t native_offset = 0;
    for (uint32_t i = 0; i < type->member_count; i++)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(form, type->first_member + i);
        const tb_shader_form_type *const field = tb_shader_form_type_at(form, member->type);
        const tb_shader_variable *const layout = &buffer->variables[start + i];
        if (field->kind != TB_SHADER_FORM_SCALAR && field->kind != TB_SHADER_FORM_VECTOR &&
            field->kind != TB_SHADER_FORM_MATRIX)
        {
            return false;
        }
        if (field->scalar != TB_SHADER_SCALAR_UINT && field->scalar != TB_SHADER_SCALAR_INT &&
            field->scalar != TB_SHADER_SCALAR_FLOAT)
        {
            return false;
        }
        if (field->kind == TB_SHADER_FORM_MATRIX &&
            (layout->variable_class != TB_SHADER_VARIABLE_MATRIX_COLUMNS || field->rows != 4 ||
             field->columns != 4))
        {
            return false;
        }
        size_t alignment = field->columns == 3 ? 16u : 4u * field->columns;
        native_offset = (native_offset + alignment - 1) / alignment * alignment;
        if (native_offset != layout->offset)
        {
            return false;
        }
        native_offset += field->kind == TB_SHADER_FORM_MATRIX ? 64 : alignment;
    }
    return true;
}

static void context_argument(msl_writer *w, const char *type, const char *name)
{
    const char *const separator = w->context_args[0] ? ", " : "";
    w->context_params =
        tb_fmt_string(&w->allocator, "%s%s%s %s", w->context_params, separator, type, name);
    w->context_args = tb_fmt_string(&w->allocator, "%s%s%s", w->context_args, separator, name);
}

static void helper_signature(msl_writer *w, const tb_shader_form_function *fn, uint32_t index)
{
    append(w, "%s f%u(", type_name(w, fn->result), index);
    size_t count = tb_array_length(fn->parameters);
    for (size_t i = 0; i < count; i++)
    {
        const tb_shader_form_parameter *const p =
            TB_ARRAY_AT(fn->parameters, tb_shader_form_parameter, i);
        if (p->direction != TB_SHADER_FORM_IN)
        {
            w->reason = "Metal output supports input helper parameters";
            return;
        }
        append(w, "%s%s p%zu", i ? ", " : "", type_name(w, p->type), i);
    }
    append(w, "%s%s)", count && w->context_params[0] ? ", " : "", w->context_params);
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
    const bool struct_result = result->kind == TB_SHADER_FORM_STRUCT;
    const bool color = fn->result_semantic && (tb_str_equal(fn->result_semantic, "SV_Target") ||
                                               tb_str_equal(fn->result_semantic, "SV_Target0"));
    const bool position = fn->result_semantic && tb_str_equal(fn->result_semantic, "SV_Position");
    const bool compute = entry->stage == TB_SHADER_FORM_STAGE_COMPUTE;
    if ((!vertex && !compute && entry->stage != TB_SHADER_FORM_STAGE_PIXEL) ||
        (!compute && !struct_result &&
         (!(vertex ? position : color) || result->kind != TB_SHADER_FORM_VECTOR ||
          result->scalar != TB_SHADER_SCALAR_FLOAT || result->columns != 4)) ||
        (compute && result->kind != TB_SHADER_FORM_VOID))
    {
        writer->reason = "Metal output needs a vertex or pixel entry point with raster outputs";
        return;
    }
    const size_t params = tb_array_length(fn->parameters);
    const uint32_t input = params == 1
                               ? TB_ARRAY_AT(fn->parameters, tb_shader_form_parameter, 0)->type
                               : TB_SHADER_FORM_NONE;
    writer->vertex_input = vertex ? input : TB_SHADER_FORM_NONE;
    if (compute)
    {
        append(writer, "// tb_numthreads %u %u %u\n", reflection->thread_group[0],
               reflection->thread_group[1], reflection->thread_group[2]);
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
            const char *const semantic = member->semantic;
            if (i == writer->vertex_input && semantic)
            {
                append(writer, "    %s a_%s", type_name(writer, member->type), semantic);
            }
            else
            {
                append(writer, "    %s m%u", type_name(writer, member->type), j);
            }
            if ((i == input || i == fn->result) && semantic)
            {
                if (vertex && i == input)
                {
                    append(writer, " [[attribute(%u)]]", j);
                }
                else if (tb_str_equal(semantic, "SV_Position"))
                {
                    append(writer, " [[position]]");
                }
                else if (!vertex && i == fn->result)
                {
                    if (!tb_str_starts_with(semantic, "SV_Target") ||
                        tb_str_length(semantic) > 10 ||
                        (semantic[9] && (semantic[9] < '0' || semantic[9] > '7')))
                    {
                        writer->reason = "Metal output needs a color target semantic";
                        return;
                    }
                    append(writer, " [[color(%u)]]",
                           semantic[9] ? (unsigned)(semantic[9] - '0') : 0u);
                }
                else
                {
                    append(writer, " [[user(tb_%s)%s]]", semantic,
                           (member->modifiers & TB_SHADER_MODIFIER_NOINTERPOLATION) != 0 ? ", flat"
                                                                                         : "");
                }
            }
            append(writer, ";\n");
        }
        append(writer, "};\n");
    }
    static const char *const resources[TB_MSL_HEAP_COUNT] = {
        "device uint *",
        "texture2d<float>",
        "texture2d_array<float>",
        "texturecube<float>",
        "texture2d<float,access::read_write>",
        "texture2d_array<float,access::read_write>",
        "sampler"};
    const size_t functions = tb_shader_form_function_count(form);
    writer->context_args = "";
    writer->context_params = "";
    const char *bindings = "";
    for (size_t i = 0; i < functions; i++)
    {
        const tb_shader_form_function *const function =
            tb_shader_form_function_at(form, (uint32_t)i);
        for (size_t j = 0; j < tb_shader_form_expression_count(function); j++)
        {
            const tb_shader_form_expression *const expr =
                tb_shader_form_expression_at(function, (uint32_t)j);
            if (expr->kind == TB_SHADER_FORM_HEAP)
            {
                const tb_msl_heap heap = heap_for(tb_shader_form_type_at(form, expr->type)->object);
                if (heap == TB_MSL_HEAP_COUNT)
                {
                    writer->reason = "unsupported type taken from a Metal argument buffer";
                    return;
                }
                writer->heaps[heap] = true;
            }
        }
    }
    for (size_t i = 0; i < tb_array_length(form->globals); i++)
    {
        const tb_shader_form_global *const g = TB_ARRAY_AT(form->globals, tb_shader_form_global, i);
        const tb_shader_form_type *const t = tb_shader_form_type_at(form, g->type);
        if (t->kind == TB_SHADER_FORM_RESOURCE && (t->object == TB_SHADER_OBJECT_RESOURCE_HEAP ||
                                                   t->object == TB_SHADER_OBJECT_SAMPLER_HEAP))
        {
            continue;
        }
        const tb_shader_binding *binding = NULL;
        const tb_shader_constant_buffer *buffer = NULL;
        for (size_t j = 0; j < reflection->binding_count; j++)
        {
            if (tb_str_equal(reflection->bindings[j].name, g->name))
            {
                binding = &reflection->bindings[j];
                break;
            }
        }
        for (size_t j = 0; j < reflection->buffer_count; j++)
        {
            if (tb_str_equal(reflection->buffers[j].name, g->name))
            {
                buffer = &reflection->buffers[j];
                break;
            }
        }
        if (!buffer || !binding || binding->kind != TB_SHADER_BINDING_CONSTANT_BUFFER ||
            binding->space != 0 || binding->register_index > 2 || !uniform_layout(form, g, buffer))
        {
            writer->reason =
                "Metal output requires a compatible constant-buffer layout at b0 through b2";
            return;
        }
        const char *const type =
            tb_fmt_string(&writer->allocator, "constant %s &", type_name(writer, g->type));
        const char *const name = tb_fmt_string(&writer->allocator, "g%zu", i);
        context_argument(writer, type, name);
        bindings = tb_fmt_string(&writer->allocator, "%s%s%s %s [[buffer(%u)]]", bindings,
                                 bindings[0] ? ", " : "", type, name, binding->register_index);
    }
    for (unsigned i = 0; i < TB_MSL_HEAP_COUNT; i++)
    {
        if (!writer->heaps[i])
        {
            continue;
        }
        append(writer, "struct heap%u { array<%s,%u> items; };\n", i, resources[i],
               i == TB_MSL_SAMPLERS ? TB_MSL_SAMPLER_COUNT : TB_MSL_RESOURCE_COUNT);
        const char *const type = tb_fmt_string(&writer->allocator, "constant heap%u &", i);
        const char *const name = tb_fmt_string(&writer->allocator, "h%u", i);
        context_argument(writer, type, name);
        bindings = tb_fmt_string(&writer->allocator, "%s%s%s %s [[buffer(%u)]]", bindings,
                                 bindings[0] ? ", " : "", type, name, TB_MSL_FIRST_HEAP_SLOT + i);
    }
    for (size_t i = 0; i < functions; i++)
    {
        if (i == entry->function)
        {
            continue;
        }
        helper_signature(writer, tb_shader_form_function_at(form, (uint32_t)i), (uint32_t)i);
        append(writer, ";\n");
    }
    for (size_t i = 0; i < functions && writer->reason == NULL; i++)
    {
        if (i == entry->function)
        {
            continue;
        }
        const tb_shader_form_function *const helper = tb_shader_form_function_at(form, (uint32_t)i);
        helper_signature(writer, helper, (uint32_t)i);
        append(writer, " {\n");
        function_body(writer, helper, arena);
    }
    append(writer, "%s %s tb_main(",
           compute  ? "kernel"
           : vertex ? "vertex"
                    : "fragment",
           type_name(writer, fn->result));
    for (size_t i = 0; i < params; i++)
    {
        const tb_shader_form_parameter *const p =
            TB_ARRAY_AT(fn->parameters, tb_shader_form_parameter, i);
        const tb_shader_form_type *const type = tb_shader_form_type_at(form, p->type);
        if (i != 0)
        {
            append(writer, ", ");
        }
        if (compute && p->semantic && tb_str_equal(p->semantic, "SV_DispatchThreadID") &&
            tb_str_equal(type->name, "uint3"))
        {
            append(writer, "uint3 p%zu [[thread_position_in_grid]]", i);
        }
        else if (type->kind == TB_SHADER_FORM_STRUCT && p->direction == TB_SHADER_FORM_IN &&
                 params == 1)
        {
            append(writer, "%s p%zu [[stage_in]]", type_name(writer, p->type), i);
        }
        else if (vertex && p->semantic &&
                 (tb_str_equal(p->semantic, "SV_VertexID") ||
                  tb_str_equal(p->semantic, "SV_InstanceID")) &&
                 p->direction == TB_SHADER_FORM_IN && type->kind == TB_SHADER_FORM_SCALAR &&
                 type->scalar == TB_SHADER_SCALAR_UINT)
        {
            append(writer, "uint p%zu [[%s]]", i,
                   tb_str_equal(p->semantic, "SV_VertexID") ? "vertex_id" : "instance_id");
        }
        else
        {
            writer->reason =
                "Metal output needs a stage input struct or an unsigned vertex or instance ID";
            return;
        }
    }
    append(writer, "%s%s) {\n", params && bindings[0] ? ", " : "", bindings);
    function_body(writer, fn, arena);
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
