/* HLSL of Shader Model 5, written from the compiler's own form.
 *
 * The text follows the form: its structs, its globals, a declaration of
 * each function, then each function's body. A function's parameters are
 * `pN` and its locals `lN`; each value an EMIT evaluates is a constant
 * `eN`, declared where it is evaluated, so the text keeps the form's order
 * of evaluation. What only names a place (a pointer) or a resource is
 * written where it is used, as HLSL writes it: `l2.m_Color.x`, `tb_t0`.
 * Functions are `fN_name` but the entry point, which keeps its name; a
 * struct's members are `m_name`, as a member may have a name Shader Model
 * 5 keeps for itself, such as `Buffer`.
 *
 * Each line of the text records where in the sources the statement or
 * expression it was written for is, so that what the compiler says of a
 * line is said of the shader's own file. */
#include "shader_hlsl.h"

#include "shader_intrinsics.h"
#include "shader_methods.h"

#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/str.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

/* Bytes of a register of a constant buffer. */
#define REGISTER_BYTES 16u

/* The first `u` register a pixel shader's slots take: D3D11 shares the
 * registers below with the render targets. */
#define PIXEL_FIRST_UAV 8u

/* What a value of a resource-typed local is, where it is stored once:
 * the text of what was stored, which the local stands for. */
typedef struct local_alias
{
    const char *text; /* NULL where the local is declared. */
    uint32_t store;   /* The statement that stores it, which is not written. */
} local_alias;

/* A slot, with the text it is declared by. */
typedef struct slot_entry
{
    tb_shader_dxbc_slot slot;
    const char *type; /* As HLSL names the resource. */
    const char *name; /* `tb_t0` */
} slot_entry;

typedef struct writer
{
    const tb_shader_hlsl_request *request;
    const tb_shader_form *form;
    tb_arena *arena;
    tb_allocator allocator;
    tb_array *diagnostics;
    size_t error_count;
    tb_shader_form_stage stage;
    uint32_t entry_function;

    tb_array *text;  /* char */
    tb_array *lines; /* tb_shader_location */
    size_t indent;
    tb_shader_location location; /* What the line being written came from. */

    slot_entry slots[TB_SHADER_DXBC_MAX_SLOTS];
    uint32_t slot_count;
    uint32_t next_register[3]; /* t, u, s */
    /* The slot of each HEAP expression of each function, as their indices:
     * slot_of[function][expression], NONE where there is none. */
    uint32_t **slot_of;

    /* The function being written. */
    const tb_shader_form_function *function;
    uint32_t function_index;
    local_alias *aliases;
} writer;

/* -------------------------------------------------------------------------
 * Text and diagnostics.
 * ------------------------------------------------------------------------- */

static void put(writer *out, const char *text)
{
    tb_array_append(out->text, text, tb_str_length(text));
}

static void putf(writer *out, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const char *const text = tb_fmt_vstring(&out->allocator, format, arguments);
    va_end(arguments);
    put(out, text);
}

static const char *format(writer *out, const char *format_text, ...)
{
    va_list arguments;
    va_start(arguments, format_text);
    const char *const text = tb_fmt_vstring(&out->allocator, format_text, arguments);
    va_end(arguments);
    return text;
}

/* Start a line, which records where the writer is in the sources. */
static void start_line(writer *out)
{
    (void)tb_array_push(out->lines, &out->location);
    for (size_t level = 0; level < out->indent; level++)
    {
        put(out, "    ");
    }
}

static void end_line(writer *out)
{
    put(out, "\n");
}

static void error_at(writer *out, tb_shader_location location, const char *message)
{
    /* What is said once is not said again: an expression of a type Shader
     * Model 5 lacks is usually among many of that type, and the first is
     * where it is first used. */
    for (size_t index = 0; index < tb_array_length(out->diagnostics); index++)
    {
        const tb_shader_diagnostic *const said =
            TB_ARRAY_AT(out->diagnostics, tb_shader_diagnostic, index);
        if (tb_str_equal(said->message, message))
        {
            return;
        }
    }
    tb_shader_diagnostic diagnostic = {0};
    diagnostic.severity = TB_SHADER_ERROR;
    diagnostic.path = "";
    if (location.line > 0 && location.file < out->request->file_count)
    {
        diagnostic.path = out->request->files[location.file];
        diagnostic.line = location.line;
        diagnostic.column = location.column;
    }
    diagnostic.message = message;
    (void)tb_array_push(out->diagnostics, &diagnostic);
    out->error_count++;
}

/* An error of something Shader Model 5 does not have. */
static void lacks(writer *out, tb_shader_location location, const char *what)
{
    error_at(out, location,
             format(out, "%s: Shader Model 5, which D3D11 runs, has no such thing", what));
}

/* -------------------------------------------------------------------------
 * Names and types.
 * ------------------------------------------------------------------------- */

/* `name` with every byte that is not a letter, a digit or `_` made `_`. */
static const char *identifier(writer *out, const char *name)
{
    const size_t length = tb_str_length(name);
    char *const copy = tb_arena_alloc(out->arena, length + 1, 1);
    for (size_t at = 0; at < length; at++)
    {
        const char byte = name[at];
        const bool letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        const bool digit = byte >= '0' && byte <= '9';
        copy[at] = letter || digit ? byte : '_';
    }
    copy[length] = '\0';
    return copy;
}

static const tb_shader_form_type *type_at(const writer *out, uint32_t type)
{
    return tb_shader_form_type_at(out->form, type);
}

static const char *struct_name(writer *out, uint32_t type)
{
    return format(out, "s%u_%s", (unsigned)type, identifier(out, type_at(out, type)->name));
}

static const char *function_name(writer *out, uint32_t function)
{
    const tb_shader_form_function *const found = tb_shader_form_function_at(out->form, function);
    if (function == out->entry_function)
    {
        return found->name;
    }
    return format(out, "f%u_%s", (unsigned)function, identifier(out, found->name));
}

/* A scalar's component that Shader Model 5 lacks, or NULL. */
static const char *missing_scalar(tb_shader_scalar scalar)
{
    switch (scalar)
    {
    case TB_SHADER_SCALAR_INT16:
    case TB_SHADER_SCALAR_UINT16:
        return "16-bit integers";
    case TB_SHADER_SCALAR_INT64:
    case TB_SHADER_SCALAR_UINT64:
        return "64-bit integers";
    case TB_SHADER_SCALAR_DOUBLE:
        return "64-bit floats, which D3D11 has only as an option";
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
        break;
    }
    return NULL;
}

static const char *type_text(writer *out, uint32_t type);

/* A resource's type as HLSL writes it, its element named as this text names it. */
static const char *resource_text(writer *out, const tb_shader_form_type *type)
{
    const tb_shader_object_info *const info = tb_shader_object_info_of(type->object);
    const char *const name = info != NULL ? info->name : type->name;
    if (type->element == TB_SHADER_FORM_NONE || info == NULL ||
        info->template_kind == TB_SHADER_TEMPLATE_NONE ||
        info->template_kind == TB_SHADER_TEMPLATE_FLAGS)
    {
        return name;
    }
    /* A resource of numbers is named as the source names it, `unorm` and
     * `snorm` kept; only a struct's name is this text's own. */
    if (tb_shader_form_is_numeric(out->form, type->element))
    {
        return type->name;
    }
    const char *const element = type_text(out, type->element);
    if ((info->template_kind == TB_SHADER_TEMPLATE_SAMPLES ||
         info->template_kind == TB_SHADER_TEMPLATE_PATCH) &&
        type->length > 0)
    {
        return format(out, "%s<%s, %u>", name, element, (unsigned)type->length);
    }
    return format(out, "%s<%s>", name, element);
}

/* A type as a declaration names it before the name: an array's element. */
static const char *type_text(writer *out, uint32_t type)
{
    const tb_shader_form_type *const found = type_at(out, type);
    switch (found->kind)
    {
    case TB_SHADER_FORM_STRUCT:
        return struct_name(out, type);
    case TB_SHADER_FORM_ARRAY:
        return type_text(out, found->element);
    case TB_SHADER_FORM_RESOURCE:
        return resource_text(out, found);
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_VECTOR:
    case TB_SHADER_FORM_MATRIX:
    case TB_SHADER_FORM_STRING:
        break;
    }
    return found->name;
}

/* What follows a declaration's name: an array's sizes, `[4][2]`. */
static const char *array_suffix(writer *out, uint32_t type)
{
    const char *suffix = "";
    for (const tb_shader_form_type *found = type_at(out, type); found->kind == TB_SHADER_FORM_ARRAY;
         found = type_at(out, found->element))
    {
        suffix = found->length == 0 ? format(out, "%s[]", suffix)
                                    : format(out, "%s[%u]", suffix, (unsigned)found->length);
    }
    return suffix;
}

static const char *declaration(writer *out, uint32_t type, const char *name)
{
    return format(out, "%s %s%s", type_text(out, type), name, array_suffix(out, type));
}

static bool is_resource(const writer *out, uint32_t type)
{
    const tb_shader_form_type *found = type_at(out, type);
    while (found->kind == TB_SHADER_FORM_ARRAY)
    {
        found = type_at(out, found->element);
    }
    return found->kind == TB_SHADER_FORM_RESOURCE;
}

/* Whether a value of a type can start as `(T)0`: numbers, and structs of them. */
static bool zeroable(const writer *out, uint32_t type)
{
    const tb_shader_form_type *const found = type_at(out, type);
    switch (found->kind)
    {
    case TB_SHADER_FORM_SCALAR:
    case TB_SHADER_FORM_VECTOR:
    case TB_SHADER_FORM_MATRIX:
        return true;
    case TB_SHADER_FORM_STRUCT:
        for (uint32_t at = 0; at < found->member_count; at++)
        {
            const tb_shader_form_member *const member =
                tb_shader_form_member_at(out->form, found->first_member + at);
            const tb_shader_form_type *const member_type = type_at(out, member->type);
            if (member_type->kind == TB_SHADER_FORM_ARRAY || !zeroable(out, member->type))
            {
                return false;
            }
        }
        return true;
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_ARRAY:
    case TB_SHADER_FORM_STRING:
    case TB_SHADER_FORM_RESOURCE:
        break;
    }
    return false;
}

/* `nointerpolation `, `row_major `: the modifiers of a member or a parameter. */
static void put_modifiers(writer *out, uint64_t modifiers)
{
    for (unsigned bit = 0; bit < 64; bit++)
    {
        const uint64_t modifier = UINT64_C(1) << bit;
        if ((modifiers & modifier) != 0)
        {
            putf(out, "%s ", tb_shader_modifier_name(modifier));
        }
    }
}

/* A member's modifiers, without a layout where it is no matrix: the form
 * keeps `#pragma pack_matrix` on every member, which D3DCompiler refuses on
 * any other type. */
static uint64_t member_modifiers(const writer *out, const tb_shader_form_member *member)
{
    const tb_shader_form_type *type = type_at(out, member->type);
    while (type->kind == TB_SHADER_FORM_ARRAY)
    {
        type = type_at(out, type->element);
    }
    if (type->kind == TB_SHADER_FORM_MATRIX)
    {
        return member->modifiers;
    }
    return member->modifiers & ~(uint64_t)TB_SHADER_FORM_LAYOUT_MODIFIERS;
}

static void put_semantic(writer *out, const char *semantic)
{
    if (semantic != NULL)
    {
        putf(out, " : %s", semantic);
    }
}

/* -------------------------------------------------------------------------
 * Literals.
 * ------------------------------------------------------------------------- */

static const char *float_text(writer *out, double value, tb_shader_scalar scalar)
{
    if (isnan(value) || isinf(value))
    {
        /* An infinity or a NaN is the same as a float: no value changes. */
        const float single = isnan(value) ? NAN : value > 0.0 ? INFINITY : -INFINITY;
        uint32_t bits = 0;
        memcpy(&bits, &single, sizeof(bits));
        return format(out, "asfloat(0x%08" PRIX32 "u)", bits);
    }
    char digits[TB_DECIMAL_SHORTEST_SIZE] = {0};
    size_t required = 0;
    (void)tb_decimal_shortest(value, digits, sizeof(digits), &required);
    if (scalar == TB_SHADER_SCALAR_MIN16FLOAT)
    {
        return format(out, "min16float(%s)", digits);
    }
    const char *const suffix = scalar == TB_SHADER_SCALAR_DOUBLE ? "L" : "f";
    /* A negative value, -0.0 included, is parenthesized, as an operand. */
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return format(out, (bits >> 63) != 0 ? "(%s%s)" : "%s%s", digits, suffix);
}

static const char *literal_text(writer *out, const tb_shader_form_expression *literal)
{
    const tb_shader_form_type *const type = type_at(out, literal->type);
    const uint64_t bits = literal->value.bits;
    switch (type->scalar)
    {
    case TB_SHADER_SCALAR_BOOL:
        return bits != 0 ? "true" : "false";
    case TB_SHADER_SCALAR_UINT:
    case TB_SHADER_SCALAR_UINT16:
    case TB_SHADER_SCALAR_UINT64:
        return format(out, "%" PRIu64 "u", bits & UINT64_C(0xFFFFFFFF));
    case TB_SHADER_SCALAR_MIN16UINT:
        return format(out, "min16uint(%" PRIu64 "u)", bits & UINT64_C(0xFFFF));
    case TB_SHADER_SCALAR_MIN16INT:
        return format(out, "min16int(%" PRId32 ")", (int32_t)(int16_t)(bits & 0xFFFFu));
    case TB_SHADER_SCALAR_INT:
    case TB_SHADER_SCALAR_INT16:
    case TB_SHADER_SCALAR_INT64:
    case TB_SHADER_SCALAR_LITERAL_INT: {
        uint32_t low = (uint32_t)(bits & UINT64_C(0xFFFFFFFF));
        int32_t value = 0;
        memcpy(&value, &low, sizeof(value));
        if (value == INT32_MIN)
        {
            return "(-2147483647 - 1)";
        }
        return format(out, value < 0 ? "(%" PRId32 ")" : "%" PRId32, value);
    }
    case TB_SHADER_SCALAR_MIN16FLOAT:
    case TB_SHADER_SCALAR_HALF:
    case TB_SHADER_SCALAR_FLOAT:
    case TB_SHADER_SCALAR_DOUBLE:
    case TB_SHADER_SCALAR_LITERAL_FLOAT:
        break;
    }
    return float_text(out, literal->value.value, type->scalar);
}

/* -------------------------------------------------------------------------
 * Descriptor heaps: a register for each resource or sampler taken from one.
 * ------------------------------------------------------------------------- */

static const tb_shader_form_expression *expression_at(const tb_shader_form_function *function,
                                                      uint32_t index)
{
    return tb_shader_form_expression_at(function, index);
}

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

static const tb_shader_binding *binding_named(const writer *out, const char *name)
{
    const tb_shader_reflection *const reflection = out->request->reflection;
    for (size_t index = 0; index < reflection->binding_count; index++)
    {
        if (tb_str_equal(reflection->bindings[index].name, name))
        {
            return &reflection->bindings[index];
        }
    }
    return NULL;
}

static const tb_shader_constant_buffer *buffer_named(const writer *out, const char *name)
{
    const tb_shader_reflection *const reflection = out->request->reflection;
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
static bool step_into(const writer *out, index_source *source, uint32_t index)
{
    const tb_shader_form_type *const type = type_at(out, source->type);
    if (type->kind == TB_SHADER_FORM_STRUCT && index < type->member_count)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(out->form, type->first_member + index);
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
            tb_shader_form_find_numeric(out->form, TB_SHADER_FORM_SCALAR, type->scalar, 1, 1);
        return source->type != TB_SHADER_FORM_NONE;
    }
    return false;
}

/* Trace where a value or a place an expression of `function` gives comes
 * from: false unless it is read from a constant buffer at a place known
 * when the shader compiles. */
static bool trace(const writer *out, const tb_shader_form_function *function, uint32_t index,
                  index_source *source)
{
    const tb_shader_form_expression *const expression = expression_at(function, index);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_GLOBAL: {
        const tb_shader_form_global *const global =
            TB_ARRAY_AT(out->form->globals, tb_shader_form_global, expression->index);
        const tb_shader_binding *const binding = binding_named(out, global->name);
        if (global->space != TB_SHADER_FORM_UNIFORM || binding == NULL || binding->space != 0 ||
            binding->register_index >= TB_SHADER_DXBC_CONSTANT_BUFFERS)
        {
            return false;
        }
        source->buffer = buffer_named(out, global->name);
        source->buffer_register = binding->register_index;
        source->type = global->type;
        source->variable = SIZE_MAX;
        source->offset = 0;
        return source->buffer != NULL;
    }
    case TB_SHADER_FORM_LOAD:
        return trace(out, function, expression->operands[0], source);
    case TB_SHADER_FORM_ACCESS_INDEX:
        return trace(out, function, expression->operands[0], source) &&
               step_into(out, source, expression->index);
    case TB_SHADER_FORM_ACCESS: {
        const tb_shader_form_expression *const at =
            expression_at(function, expression->operands[1]);
        return at->kind == TB_SHADER_FORM_LITERAL &&
               trace(out, function, expression->operands[0], source) &&
               step_into(out, source, (uint32_t)at->value.bits);
    }
    case TB_SHADER_FORM_SWIZZLE:
        return expression->swizzle_count == 1 &&
               trace(out, function, expression->operands[0], source) &&
               step_into(out, source, expression->swizzle[0]);
    case TB_SHADER_FORM_CONVERT: {
        const tb_shader_form_type *const from =
            type_at(out, expression_at(function, expression->operands[0])->type);
        const bool same_bits =
            from->scalar == TB_SHADER_SCALAR_INT || from->scalar == TB_SHADER_SCALAR_UINT;
        return same_bits && trace(out, function, expression->operands[0], source);
    }
    case TB_SHADER_FORM_INTRINSIC:
        if (expression->intrinsic == TB_SHADER_INTRINSIC_NON_UNIFORM_RESOURCE_INDEX &&
            expression->operand_count == 1)
        {
            return trace(out, function,
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

/* What a slot's register holds, for a resource of `object`; false for one
 * a slot cannot hold yet. */
static bool view_of(tb_shader_object object, tb_shader_dxbc_view *view)
{
    static const struct
    {
        tb_shader_object object;
        tb_shader_dxbc_view view;
    } views[] = {{TB_SHADER_OBJECT_BYTE_ADDRESS_BUFFER, TB_SHADER_DXBC_VIEW_RAW_BUFFER},
                 {TB_SHADER_OBJECT_RW_BYTE_ADDRESS_BUFFER, TB_SHADER_DXBC_VIEW_RAW_BUFFER},
                 {TB_SHADER_OBJECT_TEXTURE2D, TB_SHADER_DXBC_VIEW_TEXTURE2D},
                 {TB_SHADER_OBJECT_RW_TEXTURE2D, TB_SHADER_DXBC_VIEW_TEXTURE2D},
                 {TB_SHADER_OBJECT_TEXTURE2D_ARRAY, TB_SHADER_DXBC_VIEW_TEXTURE2D_ARRAY},
                 {TB_SHADER_OBJECT_RW_TEXTURE2D_ARRAY, TB_SHADER_DXBC_VIEW_TEXTURE2D_ARRAY},
                 {TB_SHADER_OBJECT_TEXTURE_CUBE, TB_SHADER_DXBC_VIEW_TEXTURE_CUBE},
                 {TB_SHADER_OBJECT_SAMPLER, TB_SHADER_DXBC_VIEW_SAMPLER},
                 {TB_SHADER_OBJECT_SAMPLER_COMPARISON, TB_SHADER_DXBC_VIEW_SAMPLER}};
    for (size_t index = 0; index < sizeof(views) / sizeof(views[0]); index++)
    {
        if (views[index].object == object)
        {
            *view = views[index].view;
            return true;
        }
    }
    return false;
}

static size_t class_index(char register_class)
{
    return register_class == 't' ? 0u : register_class == 'u' ? 1u : 2u;
}

/* The slot of a HEAP expression: one already given to the same resource,
 * read from the same place, or a new one. NONE after reporting why there
 * is none. */
static uint32_t slot_for(writer *out, const tb_shader_form_function *function,
                         const tb_shader_form_expression *heap)
{
    const tb_shader_form_type *const type = type_at(out, heap->type);
    index_source source = {0};
    tb_shader_dxbc_view view = TB_SHADER_DXBC_VIEW_COUNT;
    if (type->kind != TB_SHADER_FORM_RESOURCE || !view_of(type->object, &view))
    {
        error_at(out, heap->location,
                 format(out,
                        "a %s from a descriptor heap: Shader Model 5, which D3D11 runs, binds "
                        "only textures, byte-address buffers and samplers from one",
                        type->kind == TB_SHADER_FORM_RESOURCE ? type->name : "constant buffer"));
        return TB_SHADER_FORM_NONE;
    }
    if (!trace(out, function, heap->operands[0], &source) ||
        type_at(out, source.type)->kind != TB_SHADER_FORM_SCALAR ||
        (type_at(out, source.type)->scalar != TB_SHADER_SCALAR_UINT &&
         type_at(out, source.type)->scalar != TB_SHADER_SCALAR_INT))
    {
        error_at(out, heap->location,
                 "a descriptor heap's index the shader computes: at Shader Model 5, which D3D11 "
                 "runs, a heap's index must be read as it is from the push constants or a "
                 "constant buffer, the same for the whole draw");
        return TB_SHADER_FORM_NONE;
    }
    const tb_shader_object_info *const info = tb_shader_object_info_of(type->object);
    const bool sampler = view == TB_SHADER_DXBC_VIEW_SAMPLER;
    tb_shader_dxbc_slot slot = {0};
    slot.heap = sampler ? TB_SHADER_DXBC_HEAP_SAMPLER : TB_SHADER_DXBC_HEAP_RESOURCE;
    slot.register_class = sampler ? 's' : info != NULL && info->is_writable ? 'u' : 't';
    slot.view = (uint8_t)view;
    slot.source = (uint8_t)source.buffer_register;
    slot.offset = source.offset;
    const char *const declared = type_text(out, heap->type);
    for (uint32_t index = 0; index < out->slot_count; index++)
    {
        const slot_entry *const entry = &out->slots[index];
        if (entry->slot.register_class == slot.register_class &&
            entry->slot.source == slot.source && entry->slot.offset == slot.offset &&
            tb_str_equal(entry->type, declared))
        {
            return index;
        }
    }
    if (out->slot_count == TB_SHADER_DXBC_MAX_SLOTS)
    {
        error_at(out, heap->location, "more resources from the descriptor heaps than D3D11 binds");
        return TB_SHADER_FORM_NONE;
    }
    const size_t class_at = class_index(slot.register_class);
    slot.register_index = out->next_register[class_at]++;
    slot_entry *const entry = &out->slots[out->slot_count];
    entry->slot = slot;
    entry->type = declared;
    entry->name = format(out, "tb_%c%u", slot.register_class, (unsigned)slot.register_index);
    return out->slot_count++;
}

/* The first register of each class that no binding of the shader takes. */
static void first_free_registers(writer *out)
{
    const tb_shader_reflection *const reflection = out->request->reflection;
    for (size_t index = 0; index < reflection->binding_count; index++)
    {
        const tb_shader_binding *const binding = &reflection->bindings[index];
        char register_class = 't';
        switch (binding->kind)
        {
        case TB_SHADER_BINDING_CONSTANT_BUFFER:
            continue;
        case TB_SHADER_BINDING_TEXTURE_BUFFER:
        case TB_SHADER_BINDING_READ:
            register_class = 't';
            break;
        case TB_SHADER_BINDING_READ_WRITE:
            register_class = 'u';
            break;
        case TB_SHADER_BINDING_SAMPLER:
            register_class = 's';
            break;
        }
        const unsigned count = binding->count == 0 ? 1u : binding->count;
        const uint32_t end = binding->register_index + count;
        uint32_t *const next = &out->next_register[class_index(register_class)];
        *next = end > *next ? end : *next;
    }
    if (out->stage == TB_SHADER_FORM_STAGE_PIXEL && out->next_register[1] < PIXEL_FIRST_UAV)
    {
        out->next_register[1] = PIXEL_FIRST_UAV;
    }
}

/* Give a slot to every HEAP expression of every function. */
static void assign_slots(writer *out)
{
    const size_t function_count = tb_shader_form_function_count(out->form);
    first_free_registers(out);
    out->slot_of = tb_arena_calloc(out->arena, function_count == 0 ? 1 : function_count,
                                   sizeof(uint32_t *), _Alignof(uint32_t *));
    for (size_t function_index = 0; function_index < function_count; function_index++)
    {
        const tb_shader_form_function *const function =
            tb_shader_form_function_at(out->form, (uint32_t)function_index);
        const size_t count = tb_shader_form_expression_count(function);
        uint32_t *const slots = tb_arena_alloc(
            out->arena, (count == 0 ? 1 : count) * sizeof(uint32_t), _Alignof(uint32_t));
        for (size_t index = 0; index < count; index++)
        {
            const tb_shader_form_expression *const expression =
                expression_at(function, (uint32_t)index);
            slots[index] = expression->kind == TB_SHADER_FORM_HEAP
                               ? slot_for(out, function, expression)
                               : TB_SHADER_FORM_NONE;
        }
        out->slot_of[function_index] = slots;
    }
}

/* -------------------------------------------------------------------------
 * Expressions.
 * ------------------------------------------------------------------------- */

static const char *operand(writer *out, uint32_t index);

static const tb_shader_form_global *global_at(const writer *out, uint32_t index)
{
    return TB_ARRAY_AT(out->form->globals, tb_shader_form_global, index);
}

/* The name a cbuffer's variable is declared by: `u2_Color`. */
static const char *uniform_name(writer *out, uint32_t global, uint32_t member)
{
    const tb_shader_form_type *const type = type_at(out, global_at(out, global)->type);
    const tb_shader_form_member *const found =
        tb_shader_form_member_at(out->form, type->first_member + member);
    return format(out, "u%u_%s", (unsigned)global, identifier(out, found->name));
}

static const char *global_name(writer *out, uint32_t global)
{
    return format(out, "g%u_%s", (unsigned)global, identifier(out, global_at(out, global)->name));
}

/* Whether an expression is written where it is used, not as a constant of its own. */
static bool written_in_place(const writer *out, const tb_shader_form_expression *expression)
{
    return expression->space != TB_SHADER_FORM_VALUE || is_resource(out, expression->type) ||
           expression->kind == TB_SHADER_FORM_HEAP;
}

/* `x`, `y`, `z` or `w`. */
static const char *component_name(uint32_t index)
{
    static const char *const names[] = {"x", "y", "z", "w"};
    return index < 4 ? names[index] : "x";
}

/* The member, component, row or element `index` of what `base` names. */
static const char *index_text(writer *out, const tb_shader_form_expression *access)
{
    const tb_shader_form_expression *const base = expression_at(out->function, access->operands[0]);
    const tb_shader_form_type *const type = type_at(out, base->type);
    if (base->kind == TB_SHADER_FORM_GLOBAL && base->space == TB_SHADER_FORM_UNIFORM)
    {
        return uniform_name(out, base->index, access->index);
    }
    const char *const text = operand(out, access->operands[0]);
    switch (type->kind)
    {
    case TB_SHADER_FORM_STRUCT: {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(out->form, type->first_member + access->index);
        return format(out, "%s.m_%s", text, member->name);
    }
    case TB_SHADER_FORM_VECTOR:
    case TB_SHADER_FORM_SCALAR:
        return format(out, "%s.%s", text, component_name(access->index));
    case TB_SHADER_FORM_VOID:
    case TB_SHADER_FORM_MATRIX:
    case TB_SHADER_FORM_ARRAY:
    case TB_SHADER_FORM_STRING:
    case TB_SHADER_FORM_RESOURCE:
        break;
    }
    return format(out, "%s[%u]", text, (unsigned)access->index);
}

/* A whole constant buffer, as an initializer of its struct: `{u0_a, u0_b}`. */
static const char *uniform_struct(writer *out, uint32_t global)
{
    const tb_shader_form_type *const type = type_at(out, global_at(out, global)->type);
    const char *text = "{";
    for (uint32_t at = 0; at < type->member_count; at++)
    {
        text = format(out, "%s%s%s", text, at > 0 ? ", " : "", uniform_name(out, global, at));
    }
    return format(out, "%s}", text);
}

static const char *texel_text(writer *out, const tb_shader_form_expression *texel)
{
    const char *const handle = operand(out, texel->operands[0]);
    const char *const location = operand(out, texel->operands[1]);
    if (texel->index == 1)
    {
        return format(out, "%s.mips[%s][%s]", handle, operand(out, texel->operands[2]), location);
    }
    if (texel->index == 2)
    {
        return format(out, "%s.sample[%s][%s]", handle, operand(out, texel->operands[2]), location);
    }
    return format(out, "%s[%s]", handle, location);
}

/* What an expression written in place is: a place or a resource. */
static const char *in_place_text(writer *out, uint32_t index)
{
    const tb_shader_form_expression *const expression = expression_at(out->function, index);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_LOAD: {
        const tb_shader_form_expression *const pointer =
            expression_at(out->function, expression->operands[0]);
        if (pointer->kind == TB_SHADER_FORM_LOCAL && out->aliases[pointer->index].text != NULL)
        {
            return out->aliases[pointer->index].text;
        }
        return operand(out, expression->operands[0]);
    }
    case TB_SHADER_FORM_ACCESS:
        return format(out, "%s[%s]", operand(out, expression->operands[0]),
                      operand(out, expression->operands[1]));
    case TB_SHADER_FORM_ACCESS_INDEX:
        return index_text(out, expression);
    case TB_SHADER_FORM_TEXEL:
        return texel_text(out, expression);
    case TB_SHADER_FORM_HEAP: {
        const uint32_t slot = out->slot_of[out->function_index][index];
        return slot == TB_SHADER_FORM_NONE ? "tb_unbound" : out->slots[slot].name;
    }
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
    case TB_SHADER_FORM_SWIZZLE:
    case TB_SHADER_FORM_COMPOSE:
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_UNARY:
    case TB_SHADER_FORM_BINARY:
    case TB_SHADER_FORM_SELECT:
    case TB_SHADER_FORM_CONVERT:
    case TB_SHADER_FORM_INTRINSIC:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
        break;
    }
    return format(out, "e%u", (unsigned)index);
}

/* An operand: what names something, as it is; a value evaluated before, by its constant. */
static const char *operand(writer *out, uint32_t index)
{
    const tb_shader_form_expression *const expression = expression_at(out->function, index);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_LITERAL:
        return literal_text(out, expression);
    case TB_SHADER_FORM_ARGUMENT:
        return format(out, "p%u", (unsigned)expression->index);
    case TB_SHADER_FORM_LOCAL:
        if (out->aliases[expression->index].text != NULL)
        {
            return out->aliases[expression->index].text;
        }
        return format(out, "l%u", (unsigned)expression->index);
    case TB_SHADER_FORM_GLOBAL:
        return global_name(out, expression->index);
    case TB_SHADER_FORM_STRING_LITERAL:
        return format(out, "\"%s\"", tb_shader_form_string_at(out->form, expression->index));
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
    case TB_SHADER_FORM_TEXEL:
    case TB_SHADER_FORM_HEAP:
        break;
    }
    if (written_in_place(out, expression))
    {
        return in_place_text(out, index);
    }
    return format(out, "e%u", (unsigned)index);
}

static const char *operand_list(writer *out, uint32_t first, uint32_t count, uint32_t skip)
{
    const char *text = "";
    for (uint32_t at = skip; at < count; at++)
    {
        text = format(out, "%s%s%s", text, at > skip ? ", " : "",
                      operand(out, tb_shader_form_operand_at(out->function, first + at)));
    }
    return text;
}

static const char *op_text(tb_shader_form_op op)
{
    static const char *const texts[] = {
        "?", "-", "!", "~",  "+",  "-",  "*",  "/", "%",  "<<", ">>",
        "&", "|", "^", "&&", "||", "==", "!=", "<", "<=", ">",  ">=",
    };
    const size_t index = (size_t)op;
    return index < sizeof(texts) / sizeof(texts[0]) ? texts[index] : "?";
}

/* `asfloat`, `asint`, or nothing: what makes a `uint` value of a byte-address
 * buffer's words into a value of `scalar`. */
static const char *reinterpret_from_words(tb_shader_scalar scalar)
{
    switch (scalar)
    {
    case TB_SHADER_SCALAR_FLOAT:
    case TB_SHADER_SCALAR_HALF:
        return "asfloat";
    case TB_SHADER_SCALAR_INT:
        return "asint";
    case TB_SHADER_SCALAR_BOOL:
    case TB_SHADER_SCALAR_UINT:
    case TB_SHADER_SCALAR_INT16:
    case TB_SHADER_SCALAR_UINT16:
    case TB_SHADER_SCALAR_INT64:
    case TB_SHADER_SCALAR_UINT64:
    case TB_SHADER_SCALAR_MIN16INT:
    case TB_SHADER_SCALAR_MIN16UINT:
    case TB_SHADER_SCALAR_MIN16FLOAT:
    case TB_SHADER_SCALAR_DOUBLE:
    case TB_SHADER_SCALAR_LITERAL_INT:
    case TB_SHADER_SCALAR_LITERAL_FLOAT:
        break;
    }
    return "";
}

/* Words of a byte-address buffer a value of `type` is: 1 to 4 for a 32-bit
 * scalar or vector; 0 for any other. */
static uint32_t words_of(const writer *out, uint32_t type)
{
    const tb_shader_form_type *const found = type_at(out, type);
    const bool word =
        found->scalar == TB_SHADER_SCALAR_FLOAT || found->scalar == TB_SHADER_SCALAR_INT ||
        found->scalar == TB_SHADER_SCALAR_UINT || found->scalar == TB_SHADER_SCALAR_HALF;
    if (found->kind == TB_SHADER_FORM_SCALAR && word)
    {
        return 1;
    }
    if (found->kind == TB_SHADER_FORM_VECTOR && word)
    {
        return found->columns;
    }
    return 0;
}

static const char *words_suffix(uint32_t words)
{
    static const char *const suffixes[] = {"", "", "2", "3", "4"};
    return words < 5 ? suffixes[words] : "";
}

/* A method's call, `object.Method(arguments)`, as Shader Model 5 writes it. */
static const char *method_text(writer *out, uint32_t method, uint32_t result_type, uint32_t first,
                               uint32_t count, tb_shader_location location)
{
    const tb_shader_method_info *const info = tb_shader_method_info_of((tb_shader_method)method);
    const char *const object = operand(out, tb_shader_form_operand_at(out->function, first));
    const char *const name = info != NULL ? info->name : "?";
    if (method == TB_SHADER_METHOD_LOAD_BYTES && result_type != TB_SHADER_FORM_NONE)
    {
        const uint32_t words = words_of(out, result_type);
        if (words == 0)
        {
            lacks(out, location, "Load<T> of a byte-address buffer of other than 32-bit values");
        }
        return format(out, "%s(%s.Load%s(%s))",
                      reinterpret_from_words(type_at(out, result_type)->scalar), object,
                      words_suffix(words), operand_list(out, first, count, 1));
    }
    if (method == TB_SHADER_METHOD_STORE && count == 3)
    {
        const uint32_t value = tb_shader_form_operand_at(out->function, first + 2);
        const uint32_t value_type = expression_at(out->function, value)->type;
        const uint32_t words = words_of(out, value_type);
        if (words == 0)
        {
            lacks(out, location, "Store<T> of a byte-address buffer of other than 32-bit values");
        }
        return format(out, "%s.Store%s(%s, asuint(%s))", object, words_suffix(words),
                      operand(out, tb_shader_form_operand_at(out->function, first + 1)),
                      operand(out, value));
    }
    const size_t name_length = tb_str_length(name);
    if (info != NULL &&
        (tb_str_ends_with(info->name, "64") ||
         tb_str_find(info->name, 0, name_length, "Float") != TB_STR_NOT_FOUND ||
         tb_str_starts_with(info->name, "SampleCmpLevel") ||
         tb_str_starts_with(info->name, "SampleCmpBias") ||
         tb_str_starts_with(info->name, "SampleCmpGrad")) &&
        !tb_str_equal(info->name, "SampleCmpLevelZero"))
    {
        lacks(out, location, format(out, "the method %s", info->name));
    }
    return format(out, "%s.%s(%s)", object, name, operand_list(out, first, count, 1));
}

/* Whether Shader Model 5 lacks an intrinsic. */
static bool missing_intrinsic(const char *name)
{
    static const char *const prefixes[] = {"Wave",
                                           "Quad",
                                           "dot4add",
                                           "dot2add",
                                           "asfloat16",
                                           "asint16",
                                           "asuint16",
                                           "printf",
                                           "abort",
                                           "GetAttributeAtVertex",
                                           "InterlockedCompareExchangeFloat",
                                           "InterlockedCompareStoreFloat"};
    for (size_t index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); index++)
    {
        if (tb_str_starts_with(name, prefixes[index]))
        {
            return true;
        }
    }
    return false;
}

static const char *intrinsic_text(writer *out, uint32_t intrinsic, uint32_t first, uint32_t count,
                                  tb_shader_location location)
{
    const tb_shader_intrinsic_info *const info =
        tb_shader_intrinsic_info_of((tb_shader_intrinsic)intrinsic);
    const char *const name = info != NULL ? info->name : "?";
    if (intrinsic == TB_SHADER_INTRINSIC_SELECT)
    {
        return format(out, "(%s ? %s : %s)",
                      operand(out, tb_shader_form_operand_at(out->function, first)),
                      operand(out, tb_shader_form_operand_at(out->function, first + 1)),
                      operand(out, tb_shader_form_operand_at(out->function, first + 2)));
    }
    if (intrinsic == TB_SHADER_INTRINSIC_AND || intrinsic == TB_SHADER_INTRINSIC_OR)
    {
        return format(out, "(%s %s %s)",
                      operand(out, tb_shader_form_operand_at(out->function, first)),
                      intrinsic == TB_SHADER_INTRINSIC_AND ? "&&" : "||",
                      operand(out, tb_shader_form_operand_at(out->function, first + 1)));
    }
    if (intrinsic == TB_SHADER_INTRINSIC_NON_UNIFORM_RESOURCE_INDEX)
    {
        return operand(out, tb_shader_form_operand_at(out->function, first));
    }
    if (info == NULL || missing_intrinsic(name))
    {
        lacks(out, location, format(out, "the intrinsic %s", name));
    }
    return format(out, "%s(%s)", name, operand_list(out, first, count, 0));
}

/* What an emitted expression computes. */
static const char *computation(writer *out, uint32_t index)
{
    const tb_shader_form_expression *const expression = expression_at(out->function, index);
    switch (expression->kind)
    {
    case TB_SHADER_FORM_LOAD: {
        const tb_shader_form_expression *const pointer =
            expression_at(out->function, expression->operands[0]);
        if (pointer->kind == TB_SHADER_FORM_GLOBAL && pointer->space == TB_SHADER_FORM_UNIFORM)
        {
            return uniform_struct(out, pointer->index);
        }
        return operand(out, expression->operands[0]);
    }
    case TB_SHADER_FORM_ACCESS:
        return format(out, "%s[%s]", operand(out, expression->operands[0]),
                      operand(out, expression->operands[1]));
    case TB_SHADER_FORM_ACCESS_INDEX:
        return index_text(out, expression);
    case TB_SHADER_FORM_SWIZZLE: {
        char letters[TB_SHADER_FORM_MAX_SWIZZLE + 1] = {0};
        for (uint8_t at = 0; at < expression->swizzle_count && at < TB_SHADER_FORM_MAX_SWIZZLE;
             at++)
        {
            letters[at] = "xyzw"[expression->swizzle[at] & 3u];
        }
        return format(out, "%s.%s", operand(out, expression->operands[0]), letters);
    }
    case TB_SHADER_FORM_COMPOSE: {
        const tb_shader_form_type *const type = type_at(out, expression->type);
        const char *const list =
            operand_list(out, expression->first_operand, expression->operand_count, 0);
        if (type->kind == TB_SHADER_FORM_ARRAY || type->kind == TB_SHADER_FORM_STRUCT)
        {
            return format(out, "{%s}", list);
        }
        return format(out, "%s(%s)", type->name, list);
    }
    case TB_SHADER_FORM_SPLAT:
    case TB_SHADER_FORM_CONVERT:
        return format(out, "(%s)%s", type_at(out, expression->type)->name,
                      operand(out, expression->operands[0]));
    case TB_SHADER_FORM_UNARY:
        return format(out, "%s%s", op_text(expression->op), operand(out, expression->operands[0]));
    case TB_SHADER_FORM_BINARY:
        return format(out, "%s %s %s", operand(out, expression->operands[0]),
                      op_text(expression->op), operand(out, expression->operands[1]));
    case TB_SHADER_FORM_SELECT:
        return format(out, "%s ? %s : %s", operand(out, expression->operands[0]),
                      operand(out, expression->operands[1]), operand(out, expression->operands[2]));
    case TB_SHADER_FORM_INTRINSIC:
        if (expression->method != 0)
        {
            return method_text(out, expression->method, expression->type, expression->first_operand,
                               expression->operand_count, expression->location);
        }
        return intrinsic_text(out, expression->intrinsic, expression->first_operand,
                              expression->operand_count, expression->location);
    case TB_SHADER_FORM_LITERAL:
    case TB_SHADER_FORM_ARGUMENT:
    case TB_SHADER_FORM_LOCAL:
    case TB_SHADER_FORM_GLOBAL:
    case TB_SHADER_FORM_CALL_RESULT:
    case TB_SHADER_FORM_STRING_LITERAL:
    case TB_SHADER_FORM_TEXEL:
    case TB_SHADER_FORM_HEAP:
        break;
    }
    return operand(out, index);
}

/* A type an expression gives that Shader Model 5 lacks is an error there. */
static void check_type(writer *out, const tb_shader_form_expression *expression)
{
    const tb_shader_form_type *const type = type_at(out, expression->type);
    if (type->kind == TB_SHADER_FORM_SCALAR || type->kind == TB_SHADER_FORM_VECTOR ||
        type->kind == TB_SHADER_FORM_MATRIX)
    {
        const char *const missing = missing_scalar(type->scalar);
        if (missing != NULL)
        {
            lacks(out, expression->location, missing);
        }
    }
}

/* -------------------------------------------------------------------------
 * Statements.
 * ------------------------------------------------------------------------- */

static const tb_shader_form_statement *statement_at(const writer *out, uint32_t index)
{
    return tb_shader_form_statement_at(out->function, index);
}

static void write_block(writer *out, uint32_t block);

static void write_emit(writer *out, const tb_shader_form_statement *emit)
{
    for (uint32_t index = emit->operands[0]; index < emit->operands[1]; index++)
    {
        const tb_shader_form_expression *const expression = expression_at(out->function, index);
        if (!tb_shader_form_is_emitted(expression->kind))
        {
            continue;
        }
        out->location = expression->location;
        check_type(out, expression);
        const char *const value = computation(out, index);
        if (written_in_place(out, expression))
        {
            continue;
        }
        start_line(out);
        putf(out, "const %s = %s;", declaration(out, expression->type, format(out, "e%u", index)),
             value);
        end_line(out);
    }
}

static void write_call(writer *out, const tb_shader_form_statement *call)
{
    const char *callee = NULL;
    if (call->function != TB_SHADER_FORM_NONE)
    {
        callee = format(out, "%s(%s)", function_name(out, call->function),
                        operand_list(out, call->first_operand, call->operand_count, 0));
    }
    else if (call->method != 0)
    {
        const uint32_t result_type = call->result != TB_SHADER_FORM_NONE
                                         ? expression_at(out->function, call->result)->type
                                         : TB_SHADER_FORM_NONE;
        callee = method_text(out, call->method, result_type, call->first_operand,
                             call->operand_count, call->location);
    }
    else
    {
        callee = intrinsic_text(out, call->intrinsic, call->first_operand, call->operand_count,
                                call->location);
    }
    start_line(out);
    if (call->result != TB_SHADER_FORM_NONE)
    {
        const tb_shader_form_expression *const result = expression_at(out->function, call->result);
        putf(out, "%s = ", declaration(out, result->type, format(out, "e%u", call->result)));
    }
    putf(out, "%s;", callee);
    end_line(out);
}

/* Whether a block's last statement leaves it: a break, a continue or a return. */
static bool ends_in_jump(const writer *out, uint32_t block)
{
    uint32_t last = TB_SHADER_FORM_NONE;
    for (uint32_t index = statement_at(out, block)->first; index != TB_SHADER_FORM_NONE;
         index = statement_at(out, index)->next)
    {
        last = index;
    }
    if (last == TB_SHADER_FORM_NONE)
    {
        return false;
    }
    const tb_shader_form_statement_kind kind = statement_at(out, last)->kind;
    return kind == TB_SHADER_FORM_BREAK || kind == TB_SHADER_FORM_CONTINUE ||
           kind == TB_SHADER_FORM_RETURN;
}

static void write_line(writer *out, const char *text)
{
    start_line(out);
    put(out, text);
    end_line(out);
}

/* A case's body, and those of the cases it falls through to, since Shader
 * Model 5 falls through from no case that does anything. */
static void write_case_bodies(writer *out, uint32_t label)
{
    for (uint32_t index = label; index != TB_SHADER_FORM_NONE;
         index = statement_at(out, index)->next)
    {
        const tb_shader_form_statement *const current = statement_at(out, index);
        write_block(out, current->body);
        if (!current->falls_through)
        {
            if (!ends_in_jump(out, current->body))
            {
                write_line(out, "break;");
            }
            return;
        }
    }
}

static bool is_empty_block(const writer *out, uint32_t block)
{
    return statement_at(out, block)->first == TB_SHADER_FORM_NONE;
}

static void write_switch(writer *out, const tb_shader_form_statement *statement)
{
    const tb_shader_form_type *const selector =
        type_at(out, expression_at(out->function, statement->operands[0])->type);
    start_line(out);
    putf(out, "switch (%s)", operand(out, statement->operands[0]));
    end_line(out);
    write_line(out, "{");
    out->indent++;
    for (uint32_t index = statement->first; index != TB_SHADER_FORM_NONE;
         index = statement_at(out, index)->next)
    {
        const tb_shader_form_statement *const label = statement_at(out, index);
        out->location = label->location;
        start_line(out);
        if (label->is_default)
        {
            put(out, "default:");
        }
        else if (selector->scalar == TB_SHADER_SCALAR_UINT)
        {
            putf(out, "case %" PRIu32 "u:", (uint32_t)(label->value & 0xFFFFFFFFu));
        }
        else
        {
            const uint32_t low = (uint32_t)(label->value & 0xFFFFFFFFu);
            int32_t value = 0;
            memcpy(&value, &low, sizeof(value));
            if (value == INT32_MIN)
            {
                put(out, "case (-2147483647 - 1):");
            }
            else
            {
                putf(out, "case %" PRId32 ":", value);
            }
        }
        end_line(out);
        /* Labels with no body of their own share the next one's. */
        if (label->falls_through && is_empty_block(out, label->body))
        {
            continue;
        }
        out->indent++;
        write_case_bodies(out, index);
        out->indent--;
    }
    out->indent--;
    write_line(out, "}");
}

/* A loop: the continuing block runs before each pass but the first, so that
 * a `continue` of the body, which goes to the top, runs it. */
static void write_loop(writer *out, const tb_shader_form_statement *statement, uint32_t index)
{
    const bool continues = !is_empty_block(out, statement->continuing) ||
                           statement->operands[0] != TB_SHADER_FORM_NONE;
    const char *const first = format(out, "loop%u_first", (unsigned)index);
    if (continues)
    {
        start_line(out);
        putf(out, "bool %s = true;", first);
        end_line(out);
    }
    write_line(out, "while (true)");
    write_line(out, "{");
    out->indent++;
    if (continues)
    {
        start_line(out);
        putf(out, "if (!%s)", first);
        end_line(out);
        write_line(out, "{");
        out->indent++;
        write_block(out, statement->continuing);
        if (statement->operands[0] != TB_SHADER_FORM_NONE)
        {
            start_line(out);
            putf(out, "if (%s) { break; }", operand(out, statement->operands[0]));
            end_line(out);
        }
        out->indent--;
        write_line(out, "}");
        start_line(out);
        putf(out, "%s = false;", first);
        end_line(out);
    }
    write_block(out, statement->body);
    out->indent--;
    write_line(out, "}");
}

static void write_statement(writer *out, uint32_t index)
{
    const tb_shader_form_statement *const statement = statement_at(out, index);
    out->location = statement->location;
    switch (statement->kind)
    {
    case TB_SHADER_FORM_BLOCK:
        write_line(out, "{");
        out->indent++;
        write_block(out, index);
        out->indent--;
        write_line(out, "}");
        return;
    case TB_SHADER_FORM_EMIT:
        write_emit(out, statement);
        return;
    case TB_SHADER_FORM_IF:
        start_line(out);
        putf(out, "if (%s)", operand(out, statement->operands[0]));
        end_line(out);
        write_line(out, "{");
        out->indent++;
        write_block(out, statement->accept);
        out->indent--;
        write_line(out, "}");
        if (statement->reject != TB_SHADER_FORM_NONE)
        {
            write_line(out, "else");
            write_line(out, "{");
            out->indent++;
            write_block(out, statement->reject);
            out->indent--;
            write_line(out, "}");
        }
        return;
    case TB_SHADER_FORM_LOOP:
        write_loop(out, statement, index);
        return;
    case TB_SHADER_FORM_SWITCH:
        write_switch(out, statement);
        return;
    case TB_SHADER_FORM_CASE:
        return;
    case TB_SHADER_FORM_BREAK:
        write_line(out, "break;");
        return;
    case TB_SHADER_FORM_CONTINUE:
        write_line(out, "continue;");
        return;
    case TB_SHADER_FORM_RETURN:
        if (statement->operands[0] == TB_SHADER_FORM_NONE)
        {
            write_line(out, "return;");
            return;
        }
        start_line(out);
        putf(out, "return %s;", operand(out, statement->operands[0]));
        end_line(out);
        return;
    case TB_SHADER_FORM_DISCARD:
        write_line(out, "discard;");
        return;
    case TB_SHADER_FORM_STORE: {
        const tb_shader_form_expression *const pointer =
            expression_at(out->function, statement->operands[0]);
        if (pointer->kind == TB_SHADER_FORM_LOCAL && out->aliases[pointer->index].store == index)
        {
            return;
        }
        start_line(out);
        putf(out, "%s = %s;", operand(out, statement->operands[0]),
             operand(out, statement->operands[1]));
        end_line(out);
        return;
    }
    case TB_SHADER_FORM_CALL:
        write_call(out, statement);
        return;
    }
}

/* The statements of a block, without its braces. */
static void write_block(writer *out, uint32_t block)
{
    for (uint32_t index = statement_at(out, block)->first; index != TB_SHADER_FORM_NONE;
         index = statement_at(out, index)->next)
    {
        write_statement(out, index);
    }
}

/* -------------------------------------------------------------------------
 * Functions.
 * ------------------------------------------------------------------------- */

static const char *direction_text(tb_shader_form_direction direction)
{
    switch (direction)
    {
    case TB_SHADER_FORM_OUT:
        return "out ";
    case TB_SHADER_FORM_INOUT:
        return "inout ";
    case TB_SHADER_FORM_IN:
        break;
    }
    return "";
}

/* A function's attributes, with `[patchconstantfunc]` naming the function
 * as this text names it. */
static void write_attributes(writer *out, uint32_t function_index)
{
    const tb_shader_form_function *const function =
        tb_shader_form_function_at(out->form, function_index);
    const size_t count = tb_array_length(function->attributes);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_attribute *const attribute =
            TB_ARRAY_AT(function->attributes, tb_shader_form_attribute, index);
        const bool names_function = tb_str_equal(attribute->name, "patchconstantfunc");
        start_line(out);
        putf(out, "[%s", attribute->name);
        for (uint32_t at = 0; at < attribute->argument_count; at++)
        {
            const tb_shader_form_argument *const argument = &attribute->arguments[at];
            put(out, at == 0 ? "(" : ", ");
            switch (argument->kind)
            {
            case TB_SHADER_FORM_ARGUMENT_INTEGER:
                putf(out, "%" PRIu64, argument->integer);
                break;
            case TB_SHADER_FORM_ARGUMENT_FLOAT:
                put(out, float_text(out, argument->value, TB_SHADER_SCALAR_LITERAL_FLOAT));
                break;
            case TB_SHADER_FORM_ARGUMENT_STRING:
                if (names_function)
                {
                    const uint32_t patch =
                        tb_shader_form_patch_constant_function(out->form, function_index);
                    putf(out, "\"%s\"",
                         patch == TB_SHADER_FORM_NONE ? argument->text : function_name(out, patch));
                    break;
                }
                putf(out, "\"%s\"", argument->text);
                break;
            case TB_SHADER_FORM_ARGUMENT_NAME:
                put(out, argument->text);
                break;
            }
        }
        put(out, attribute->argument_count > 0 ? ")]" : "]");
        end_line(out);
    }
}

/* `float4 f2_shade(float2 p0 : TEXCOORD) : SV_Target` */
static void write_signature(writer *out, uint32_t function_index)
{
    const tb_shader_form_function *const function =
        tb_shader_form_function_at(out->form, function_index);
    start_line(out);
    putf(out, "%s %s(", type_text(out, function->result), function_name(out, function_index));
    const size_t count = tb_array_length(function->parameters);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_parameter *const parameter =
            TB_ARRAY_AT(function->parameters, tb_shader_form_parameter, index);
        put(out, index > 0 ? ", " : "");
        put(out, direction_text(parameter->direction));
        put_modifiers(out, parameter->modifiers);
        put(out, declaration(out, parameter->type, format(out, "p%u", (unsigned)index)));
        put_semantic(out, parameter->semantic);
    }
    put(out, ")");
    put_semantic(out, function->result_semantic);
}

/* Which resource-typed locals stand for what they are stored once: Shader
 * Model 5 resolves every resource when it compiles, which a local stored
 * more than once may defeat. */
static void find_aliases(writer *out)
{
    const tb_shader_form_function *const function = out->function;
    const size_t local_count = tb_array_length(function->locals);
    const size_t statement_count = tb_shader_form_statement_count(function);
    uint32_t *const stores = tb_arena_calloc(out->arena, local_count == 0 ? 1 : local_count,
                                             sizeof(uint32_t), _Alignof(uint32_t));
    out->aliases = tb_arena_calloc(out->arena, local_count == 0 ? 1 : local_count,
                                   sizeof(local_alias), _Alignof(local_alias));
    for (size_t index = 0; index < local_count; index++)
    {
        out->aliases[index].store = TB_SHADER_FORM_NONE;
    }
    for (size_t index = 0; index < statement_count; index++)
    {
        const tb_shader_form_statement *const statement = statement_at(out, (uint32_t)index);
        if (statement->kind != TB_SHADER_FORM_STORE)
        {
            continue;
        }
        const tb_shader_form_expression *const pointer =
            expression_at(function, statement->operands[0]);
        if (pointer->kind == TB_SHADER_FORM_LOCAL && pointer->index < local_count)
        {
            stores[pointer->index]++;
            out->aliases[pointer->index].store = (uint32_t)index;
        }
    }
    for (size_t index = 0; index < local_count; index++)
    {
        const tb_shader_form_local *const local =
            TB_ARRAY_AT(function->locals, tb_shader_form_local, index);
        if (stores[index] != 1 || type_at(out, local->type)->kind != TB_SHADER_FORM_RESOURCE)
        {
            out->aliases[index].store = TB_SHADER_FORM_NONE;
            continue;
        }
        const tb_shader_form_statement *const store = statement_at(out, out->aliases[index].store);
        out->aliases[index].text = operand(out, store->operands[1]);
    }
}

static void write_function(writer *out, uint32_t function_index)
{
    const tb_shader_form_function *const function =
        tb_shader_form_function_at(out->form, function_index);
    out->function = function;
    out->function_index = function_index;
    out->location = function->location;
    find_aliases(out);
    write_attributes(out, function_index);
    write_signature(out, function_index);
    end_line(out);
    write_line(out, "{");
    out->indent++;
    const size_t local_count = tb_array_length(function->locals);
    for (size_t index = 0; index < local_count; index++)
    {
        const tb_shader_form_local *const local =
            TB_ARRAY_AT(function->locals, tb_shader_form_local, index);
        if (out->aliases[index].text != NULL)
        {
            continue;
        }
        start_line(out);
        put(out, declaration(out, local->type, format(out, "l%u", (unsigned)index)));
        if (zeroable(out, local->type))
        {
            putf(out, " = (%s)0", type_text(out, local->type));
        }
        put(out, ";");
        end_line(out);
    }
    if (function_index == out->entry_function && out->form->initializer != TB_SHADER_FORM_NONE)
    {
        start_line(out);
        putf(out, "%s();", function_name(out, out->form->initializer));
        end_line(out);
    }
    write_block(out, function->body);
    out->indent--;
    write_line(out, "}");
}

/* -------------------------------------------------------------------------
 * Structs and globals.
 * ------------------------------------------------------------------------- */

static void write_member(writer *out, const tb_shader_form_member *member, const char *name)
{
    start_line(out);
    put_modifiers(out, member_modifiers(out, member));
    put(out, declaration(out, member->type, name));
    put_semantic(out, member->semantic);
    put(out, ";");
    end_line(out);
}

static void write_structs(writer *out)
{
    const size_t count = tb_array_length(out->form->types);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_type *const type = type_at(out, (uint32_t)index);
        if (type->kind != TB_SHADER_FORM_STRUCT)
        {
            continue;
        }
        start_line(out);
        putf(out, "struct %s", struct_name(out, (uint32_t)index));
        end_line(out);
        write_line(out, "{");
        out->indent++;
        for (uint32_t at = 0; at < type->member_count; at++)
        {
            const tb_shader_form_member *const member =
                tb_shader_form_member_at(out->form, type->first_member + at);
            write_member(out, member, format(out, "m_%s", member->name));
        }
        out->indent--;
        write_line(out, "};");
    }
}

/* A constant or texture buffer: a `cbuffer` or a `tbuffer` whose variables
 * are named after it, as Shader Model 5 has no ConstantBuffer<T>. */
static void write_buffer(writer *out, uint32_t global_index, const tb_shader_binding *binding)
{
    const tb_shader_form_global *const global = global_at(out, global_index);
    const tb_shader_form_type *const type = type_at(out, global->type);
    const bool texture = binding->kind == TB_SHADER_BINDING_TEXTURE_BUFFER;
    start_line(out);
    putf(out, "%s b%u_%s : register(%c%u)", texture ? "tbuffer" : "cbuffer", (unsigned)global_index,
         identifier(out, global->name), texture ? 't' : 'b', binding->register_index);
    end_line(out);
    write_line(out, "{");
    out->indent++;
    for (uint32_t at = 0; at < type->member_count; at++)
    {
        const tb_shader_form_member *const member =
            tb_shader_form_member_at(out->form, type->first_member + at);
        start_line(out);
        put_modifiers(out, member_modifiers(out, member));
        put(out, declaration(out, member->type, uniform_name(out, global_index, at)));
        if (member->packoffset != TB_SHADER_FORM_NONE)
        {
            putf(out, " : packoffset(c%u.%s)", (unsigned)(member->packoffset / REGISTER_BYTES),
                 component_name((member->packoffset % REGISTER_BYTES) / 4u));
        }
        put(out, ";");
        end_line(out);
    }
    out->indent--;
    write_line(out, "};");
}

static bool is_heap(const writer *out, uint32_t type)
{
    const tb_shader_form_type *const found = type_at(out, type);
    return found->kind == TB_SHADER_FORM_RESOURCE &&
           (found->object == TB_SHADER_OBJECT_RESOURCE_HEAP ||
            found->object == TB_SHADER_OBJECT_SAMPLER_HEAP);
}

static void write_globals(writer *out)
{
    const size_t count = tb_array_length(out->form->globals);
    for (size_t index = 0; index < count; index++)
    {
        const tb_shader_form_global *const global = global_at(out, (uint32_t)index);
        const char *const name = global_name(out, (uint32_t)index);
        if (is_heap(out, global->type))
        {
            continue;
        }
        switch (global->space)
        {
        case TB_SHADER_FORM_PRIVATE:
            start_line(out);
            putf(out, "static %s;", declaration(out, global->type, name));
            end_line(out);
            break;
        case TB_SHADER_FORM_GROUPSHARED:
            start_line(out);
            putf(out, "groupshared %s;", declaration(out, global->type, name));
            end_line(out);
            break;
        case TB_SHADER_FORM_UNIFORM:
        case TB_SHADER_FORM_HANDLE: {
            const tb_shader_binding *const binding = binding_named(out, global->name);
            if (binding == NULL)
            {
                break;
            }
            if (binding->space != 0)
            {
                lacks(out, out->location, "a register space other than 0");
            }
            if (global->space == TB_SHADER_FORM_UNIFORM)
            {
                write_buffer(out, (uint32_t)index, binding);
                break;
            }
            const char register_class = binding->kind == TB_SHADER_BINDING_READ_WRITE ? 'u'
                                        : binding->kind == TB_SHADER_BINDING_SAMPLER  ? 's'
                                                                                      : 't';
            start_line(out);
            putf(out, "%s : register(%c%u);", declaration(out, global->type, name), register_class,
                 binding->register_index);
            end_line(out);
            break;
        }
        case TB_SHADER_FORM_VALUE:
        case TB_SHADER_FORM_FUNCTION:
        case TB_SHADER_FORM_ELEMENT:
            break;
        }
    }
    for (uint32_t index = 0; index < out->slot_count; index++)
    {
        const slot_entry *const entry = &out->slots[index];
        start_line(out);
        putf(out, "%s %s : register(%c%u);", entry->type, entry->name, entry->slot.register_class,
             (unsigned)entry->slot.register_index);
        end_line(out);
    }
}

/* What the whole shader needs that Shader Model 5 lacks. */
static void check_features(writer *out)
{
    /* Wave operations and 64-bit integers are found where they are used. */
    static const tb_shader_feature missing[] = {
        TB_SHADER_FEATURE_BINDLESS,    TB_SHADER_FEATURE_ATOMIC64,
        TB_SHADER_FEATURE_RAY_QUERY,   TB_SHADER_FEATURE_RAY_TRACING,
        TB_SHADER_FEATURE_MESH_SHADER, TB_SHADER_FEATURE_SAMPLER_FEEDBACK};
    const unsigned features = out->request->reflection->features;
    const tb_shader_location nowhere = {0};
    for (size_t index = 0; index < sizeof(missing) / sizeof(missing[0]); index++)
    {
        if ((features & (unsigned)missing[index]) == 0)
        {
            continue;
        }
        /* Bindless resources are counted for NonUniformResourceIndex() too,
         * which slots take apart; only an array of no size is missing. */
        if (missing[index] == TB_SHADER_FEATURE_BINDLESS &&
            (features & (TB_SHADER_FEATURE_RESOURCE_HEAP | TB_SHADER_FEATURE_SAMPLER_HEAP)) != 0)
        {
            continue;
        }
        lacks(out, nowhere, tb_shader_feature_name(missing[index]));
    }
    if (out->stage > TB_SHADER_FORM_STAGE_COMPUTE)
    {
        lacks(out, nowhere, format(out, "the %s stage", tb_shader_form_stage_name(out->stage)));
    }
}

/* What the shader reads of each constant buffer the RHI binds. */
static void constant_sizes(writer *out, tb_shader_hlsl *hlsl)
{
    const tb_shader_reflection *const reflection = out->request->reflection;
    for (size_t index = 0; index < reflection->buffer_count; index++)
    {
        const tb_shader_binding *const binding =
            binding_named(out, reflection->buffers[index].name);
        if (binding != NULL && binding->kind == TB_SHADER_BINDING_CONSTANT_BUFFER &&
            binding->space == 0 && binding->register_index < TB_SHADER_DXBC_CONSTANT_BUFFERS)
        {
            hlsl->constant_bytes[binding->register_index] = reflection->buffers[index].size;
        }
    }
}

bool tb_shader_write_hlsl(const tb_shader_hlsl_request *request, tb_arena *arena,
                          tb_array *diagnostics, tb_shader_hlsl *hlsl)
{
    const tb_shader_hlsl empty = {0};
    *hlsl = empty;
    writer out = {0};
    out.request = request;
    out.form = request->form;
    out.arena = arena;
    out.allocator = tb_arena_allocator(arena);
    out.diagnostics = diagnostics;
    out.text = TB_ARRAY_CREATE(&out.allocator, char);
    out.lines = TB_ARRAY_CREATE(&out.allocator, tb_shader_location);
    const tb_shader_form_entry_point *const entry =
        TB_ARRAY_AT(request->form->entry_points, tb_shader_form_entry_point, 0);
    out.stage = entry->stage;
    out.entry_function = entry->function;
    check_features(&out);
    assign_slots(&out);
    /* A warning of D3DCompiler's that is about this text, not the source:
     * a value used before it is set on some path. The form sets every local
     * before it is read, and returns a value on every path, which checking
     * made sure of; D3DCompiler cannot always see so through the text. */
    write_line(&out, "#pragma warning(disable : 4000)");
    write_structs(&out);
    write_globals(&out);
    const size_t function_count = tb_shader_form_function_count(out.form);
    for (size_t index = 0; index < function_count; index++)
    {
        /* Nothing calls the entry point, and its attributes belong to its
         * first declaration. */
        if (index == out.entry_function)
        {
            continue;
        }
        out.location = tb_shader_form_function_at(out.form, (uint32_t)index)->location;
        write_signature(&out, (uint32_t)index);
        put(&out, ";");
        end_line(&out);
    }
    for (size_t index = 0; index < function_count; index++)
    {
        write_function(&out, (uint32_t)index);
    }
    hlsl->error_count = out.error_count;
    if (out.error_count > 0)
    {
        return false;
    }
    const char nul = '\0';
    hlsl->length = tb_array_length(out.text);
    (void)tb_array_push(out.text, &nul);
    hlsl->text = tb_array_data(out.text);
    hlsl->lines = tb_array_data(out.lines);
    hlsl->line_count = tb_array_length(out.lines);
    for (uint32_t index = 0; index < out.slot_count; index++)
    {
        hlsl->slots[index] = out.slots[index].slot;
    }
    hlsl->slot_count = out.slot_count;
    constant_sizes(&out, hlsl);
    return true;
}
