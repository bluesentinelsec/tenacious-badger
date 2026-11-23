/* A form as text: its structs, its globals, then each function with its
 * locals and its statements, one to a line. An expression an EMIT evaluates
 * is a line `%3:float = add %1 %2`; an operand that only names something is
 * written where it is used: `1.5:float`, `arg0`, `local2`, `global0`. */
#include "shader_form.h"
#include "shader_intrinsics.h"

#include "badger/arena.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/str.h"

#include <inttypes.h>
#include <stdarg.h>

typedef struct printer
{
    const tb_shader_form *form;
    const tb_shader_form_function *function;
    tb_allocator allocator;
    tb_array *text;
    size_t indent;
} printer;

static void put(printer *out, const char *text)
{
    tb_array_append(out->text, text, tb_str_length(text));
}

static void putf(printer *out, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *const text = tb_fmt_vstring(&out->allocator, format, arguments);
    va_end(arguments);
    put(out, text);
}

static void start_line(printer *out)
{
    for (size_t level = 0; level < out->indent; level++)
    {
        put(out, "  ");
    }
}

static const char *type_name(const printer *out, uint32_t type)
{
    return tb_shader_form_type_at(out->form, type)->name;
}

static const char *op_name(tb_shader_form_op op)
{
    static const char *const names[] = {
        "none",        "negate",        "not",       "bit_not",   "add",
        "subtract",    "multiply",      "divide",    "remainder", "shift_left",
        "shift_right", "bit_and",       "bit_or",    "bit_xor",   "and",
        "or",          "equal",         "not_equal", "less",      "less_equal",
        "greater",     "greater_equal",
    };
    const size_t index = (size_t)op;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "?";
}

static const char *space_name(tb_shader_form_space space)
{
    switch (space)
    {
    case TB_SHADER_FORM_VALUE:
        return "value";
    case TB_SHADER_FORM_FUNCTION:
        return "function";
    case TB_SHADER_FORM_PRIVATE:
        return "private";
    case TB_SHADER_FORM_GROUPSHARED:
        return "groupshared";
    }
    return "?";
}

static void put_literal(printer *out, const tb_shader_form_expression *literal)
{
    const tb_shader_form_type *const type = tb_shader_form_type_at(out->form, literal->type);
    if (type->scalar == TB_SHADER_SCALAR_BOOL)
    {
        put(out, literal->value.bits != 0 ? "true" : "false");
    }
    else if (tb_shader_scalar_is_float(type->scalar))
    {
        char digits[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;
        (void)tb_decimal_shortest(literal->value.value, digits, sizeof(digits), &required);
        put(out, digits);
    }
    else if (type->scalar == TB_SHADER_SCALAR_UINT || type->scalar == TB_SHADER_SCALAR_UINT16 ||
             type->scalar == TB_SHADER_SCALAR_UINT64 || type->scalar == TB_SHADER_SCALAR_MIN16UINT)
    {
        putf(out, "%" PRIu64, literal->value.bits);
    }
    else
    {
        putf(out, "%" PRId64, (int64_t)literal->value.bits);
    }
    putf(out, ":%s", type->name);
}

/* An operand: what names something, as it is; anything else, by its number. */
static void put_operand(printer *out, uint32_t index)
{
    if (index == TB_SHADER_FORM_NONE)
    {
        put(out, "none");
        return;
    }
    const tb_shader_form_expression *const expression =
        tb_shader_form_expression_at(out->function, index);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_LITERAL:
        put_literal(out, expression);
        return;
    case TB_SHADER_FORM_ARGUMENT:
        putf(out, "arg%u", (unsigned)expression->index);
        return;
    case TB_SHADER_FORM_LOCAL:
        putf(out, "local%u", (unsigned)expression->index);
        return;
    case TB_SHADER_FORM_GLOBAL:
        putf(out, "global%u", (unsigned)expression->index);
        return;
    case TB_SHADER_FORM_STRING_LITERAL:
        putf(out, "\"%s\"", tb_shader_form_string_at(out->form, expression->index));
        return;
    case TB_SHADER_FORM_LOAD:
    case TB_SHADER_FORM_ACCESS:
    case TB_SHADER_FORM_ACCESS_INDEX:
    case TB_SHADER_FORM_SWIZZLE:
    case TB_SHADER_FORM_COMPOSE:
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_BINARY:
    case TB_SHADER_FORM_SELECT:
    case TB_SHADER_FORM_CONVERT:
    case TB_SHADER_FORM_INTRINSIC:
    case TB_SHADER_FORM_CALL_RESULT:
        putf(out, "%%%u", (unsigned)index);
        return;
    }
}

static void put_operand_list(printer *out, uint32_t first, uint32_t count)
{
    for (uint32_t at = 0; at < count; at++)
    {
        put(out, " ");
        put_operand(out, tb_shader_form_operand_at(out->function, first + at));
    }
}

static void put_value_type(printer *out, const tb_shader_form_expression *expression)
{
    put(out, type_name(out, expression->type));
    if (expression->space != TB_SHADER_FORM_VALUE)
    {
        putf(out, "* %s", space_name(expression->space));
    }
}

static const char *intrinsic_name(uint32_t intrinsic)
{
    const tb_shader_intrinsic_info *const info =
        tb_shader_intrinsic_info_of((tb_shader_intrinsic)intrinsic);
    return info == NULL ? "?" : info->name;
}

/* What an emitted expression computes: `add %1 %2`. */
static void put_computation(printer *out, const tb_shader_form_expression *expression)
{
    switch (expression->kind)
    {
    case TB_SHADER_FORM_LOAD:
        put(out, "load ");
        put_operand(out, expression->operands[0]);
        return;
    case TB_SHADER_FORM_ACCESS:
        put(out, "access ");
        put_operand(out, expression->operands[0]);
        put(out, " ");
        put_operand(out, expression->operands[1]);
        return;
    case TB_SHADER_FORM_ACCESS_INDEX:
        put(out, "access_index ");
        put_operand(out, expression->operands[0]);
        putf(out, " %u", (unsigned)expression->index);
        return;
    case TB_SHADER_FORM_SWIZZLE:
        put(out, "swizzle ");
        put_operand(out, expression->operands[0]);
        put(out, " ");
        for (uint8_t at = 0; at < expression->swizzle_count; at++)
        {
            const char letter[2] = {"xyzw"[expression->swizzle[at] & 3u], '\0'};
            put(out, letter);
        }
        return;
    case TB_SHADER_FORM_COMPOSE:
        put(out, "compose");
        put_operand_list(out, expression->first_operand, expression->operand_count);
        return;
    case TB_SHADER_FORM_SPLAT:
        put(out, "splat ");
        put_operand(out, expression->operands[0]);
        return;
    case TB_SHADER_FORM_UNARY:
        putf(out, "%s ", op_name(expression->op));
        put_operand(out, expression->operands[0]);
        return;
    case TB_SHADER_FORM_BINARY:
        putf(out, "%s ", op_name(expression->op));
        put_operand(out, expression->operands[0]);
        put(out, " ");
        put_operand(out, expression->operands[1]);
        return;
    case TB_SHADER_FORM_SELECT:
        put(out, "select ");
        put_operand(out, expression->operands[0]);
        put(out, " ");
        put_operand(out, expression->operands[1]);
        put(out, " ");
        put_operand(out, expression->operands[2]);
        return;
    case TB_SHADER_FORM_CONVERT:
        put(out, "convert ");
        put_operand(out, expression->operands[0]);
        return;
    case TB_SHADER_FORM_INTRINSIC:
        put(out, intrinsic_name(expression->intrinsic));
        put_operand_list(out, expression->first_operand, expression->operand_count);
        return;
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
        put_operand(out, (uint32_t)(expression - TB_ARRAY_DATA(out->function->expressions,
                                                               tb_shader_form_expression)));
        return;
    }
}

static void print_emit(printer *out, const tb_shader_form_statement *emit)
{
    for (uint32_t index = emit->operands[0]; index < emit->operands[1]; index++)
    {
        const tb_shader_form_expression *const expression =
            tb_shader_form_expression_at(out->function, index);
        if (!tb_shader_form_is_emitted(expression->kind))
        {
            continue;
        }
        start_line(out);
        putf(out, "%%%u:", (unsigned)index);
        put_value_type(out, expression);
        put(out, " = ");
        put_computation(out, expression);
        put(out, "\n");
    }
}

static void print_block(printer *out, uint32_t block);

static void print_call(printer *out, const tb_shader_form_statement *call)
{
    start_line(out);
    if (call->result != TB_SHADER_FORM_NONE)
    {
        const tb_shader_form_expression *const result =
            tb_shader_form_expression_at(out->function, call->result);
        putf(out, "%%%u:%s = ", (unsigned)call->result, type_name(out, result->type));
    }
    if (call->function != TB_SHADER_FORM_NONE)
    {
        putf(out, "call %s", tb_shader_form_function_at(out->form, call->function)->name);
    }
    else
    {
        putf(out, "call %s", intrinsic_name(call->intrinsic));
    }
    put_operand_list(out, call->first_operand, call->operand_count);
    put(out, "\n");
}

static void print_switch(printer *out, const tb_shader_form_statement *statement)
{
    start_line(out);
    put(out, "switch ");
    put_operand(out, statement->operands[0]);
    put(out, "\n");
    for (uint32_t index = statement->first; index != TB_SHADER_FORM_NONE;
         index = tb_shader_form_statement_at(out->function, index)->next)
    {
        const tb_shader_form_statement *const label =
            tb_shader_form_statement_at(out->function, index);
        start_line(out);
        if (label->is_default)
        {
            put(out, "default");
        }
        else
        {
            putf(out, "case %" PRId64, (int64_t)label->value);
        }
        put(out, label->falls_through ? " falls through\n" : "\n");
        print_block(out, label->body);
    }
}

static void print_statement(printer *out, uint32_t index)
{
    const tb_shader_form_statement *const statement =
        tb_shader_form_statement_at(out->function, index);
    switch (statement->kind)
    {
    case TB_SHADER_FORM_BLOCK:
        print_block(out, index);
        return;
    case TB_SHADER_FORM_EMIT:
        print_emit(out, statement);
        return;
    case TB_SHADER_FORM_IF:
        start_line(out);
        put(out, "if ");
        put_operand(out, statement->operands[0]);
        put(out, "\n");
        print_block(out, statement->accept);
        if (statement->reject != TB_SHADER_FORM_NONE)
        {
            start_line(out);
            put(out, "else\n");
            print_block(out, statement->reject);
        }
        return;
    case TB_SHADER_FORM_LOOP:
        start_line(out);
        put(out, "loop\n");
        print_block(out, statement->body);
        start_line(out);
        put(out, "continuing\n");
        print_block(out, statement->continuing);
        if (statement->operands[0] != TB_SHADER_FORM_NONE)
        {
            start_line(out);
            put(out, "break if ");
            put_operand(out, statement->operands[0]);
            put(out, "\n");
        }
        return;
    case TB_SHADER_FORM_SWITCH:
        print_switch(out, statement);
        return;
    case TB_SHADER_FORM_CASE:
        return;
    case TB_SHADER_FORM_BREAK:
        start_line(out);
        put(out, "break\n");
        return;
    case TB_SHADER_FORM_CONTINUE:
        start_line(out);
        put(out, "continue\n");
        return;
    case TB_SHADER_FORM_RETURN:
        start_line(out);
        put(out, "return");
        if (statement->operands[0] != TB_SHADER_FORM_NONE)
        {
            put(out, " ");
            put_operand(out, statement->operands[0]);
        }
        put(out, "\n");
        return;
    case TB_SHADER_FORM_DISCARD:
        start_line(out);
        put(out, "discard\n");
        return;
    case TB_SHADER_FORM_STORE:
        start_line(out);
        put(out, "store ");
        put_operand(out, statement->operands[0]);
        put(out, " ");
        put_operand(out, statement->operands[1]);
        put(out, "\n");
        return;
    case TB_SHADER_FORM_CALL:
        print_call(out, statement);
        return;
    }
}

static void print_block(printer *out, uint32_t block)
{
    start_line(out);
    put(out, "{\n");
    out->indent++;
    for (uint32_t index = tb_shader_form_statement_at(out->function, block)->first;
         index != TB_SHADER_FORM_NONE;
         index = tb_shader_form_statement_at(out->function, index)->next)
    {
        print_statement(out, index);
    }
    out->indent--;
    start_line(out);
    put(out, "}\n");
}

static const char *direction_name(tb_shader_form_direction direction)
{
    switch (direction)
    {
    case TB_SHADER_FORM_IN:
        return "in";
    case TB_SHADER_FORM_OUT:
        return "out";
    case TB_SHADER_FORM_INOUT:
        return "inout";
    }
    return "?";
}

static void print_function(printer *out, const tb_shader_form_function *function)
{
    out->function = function;
    putf(out, "function %s %s(", type_name(out, function->result), function->name);
    const size_t parameter_count = tb_array_length(function->parameters);
    for (size_t index = 0; index < parameter_count; index++)
    {
        const tb_shader_form_parameter *const parameter =
            TB_ARRAY_AT(function->parameters, tb_shader_form_parameter, index);
        putf(out, "%s%s %s %s", index > 0 ? ", " : "", direction_name(parameter->direction),
             type_name(out, parameter->type), parameter->name);
    }
    put(out, ")\n");
    const size_t local_count = tb_array_length(function->locals);
    for (size_t index = 0; index < local_count; index++)
    {
        const tb_shader_form_local *const local =
            TB_ARRAY_AT(function->locals, tb_shader_form_local, index);
        putf(out, "  local%u %s %s\n", (unsigned)index, type_name(out, local->type), local->name);
    }
    out->indent = 1;
    print_block(out, function->body);
    out->indent = 0;
}

static void print_structs(printer *out)
{
    const size_t count = tb_array_length(out->form->types);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_type *const type = tb_shader_form_type_at(out->form, (uint32_t)index);
        if (type->kind != TB_SHADER_FORM_STRUCT)
        {
            continue;
        }
        putf(out, "struct %s {", type->name);
        for (uint32_t at = 0; at < type->member_count; at++)
        {
            const tb_shader_form_member *const member =
                tb_shader_form_member_at(out->form, type->first_member + at);
            putf(out, "%s %s %s", at > 0 ? "," : "", type_name(out, member->type), member->name);
        }
        put(out, " }\n");
    }
}

const char *tb_shader_form_print(const tb_shader_form *form, tb_arena *arena)
{
    const tb_allocator allocator = tb_arena_allocator(arena);
    printer out = {0};
    out.form = form;
    out.allocator = allocator;
    out.text = TB_ARRAY_CREATE(&allocator, char);
    print_structs(&out);
    const size_t global_count = tb_array_length(form->globals);
    for (size_t index = 0; index < global_count; index++)
    {
        const tb_shader_form_global *const global =
            TB_ARRAY_AT(form->globals, tb_shader_form_global, index);
        putf(&out, "global%u %s %s %s\n", (unsigned)index, space_name(global->space),
             type_name(&out, global->type), global->name);
    }
    if (form->initializer != TB_SHADER_FORM_NONE)
    {
        putf(&out, "initializer %s\n", tb_shader_form_function_at(form, form->initializer)->name);
    }
    const size_t function_count = tb_shader_form_function_count(form);
    for (size_t index = 0; index < function_count; index++)
    {
        print_function(&out, tb_shader_form_function_at(form, (uint32_t)index));
    }
    const char nul = '\0';
    (void)tb_array_push(out.text, &nul);
    return tb_array_data(out.text);
}
