/* The parser's foundation: reading tokens, reporting errors and recovering
 * from them, the names in scope that tell a type from a value, the built-in
 * types, and the entry point. */
#include "shader_parse_internal.h"

#include "badger/arena.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/str.h"

#include <stdarg.h>
#include <string.h>

/* The longest spelling of a token an error quotes. */
#define MAX_QUOTED_TOKEN 40
/* The longest qualified name looked up without allocating. */
#define LOOKUP_BUFFER_SIZE 256

/* -------------------------------------------------------------------------
 * Tokens.
 * ------------------------------------------------------------------------- */

static const char *const keywords[] = {
    "break",
    "case",
    "cbuffer",
    "class",
    "column_major",
    "const",
    "continue",
    "default",
    "discard",
    "do",
    "else",
    "enum",
    "export",
    "extern",
    "false",
    "for",
    "globallycoherent",
    "groupshared",
    "if",
    "in",
    "inline",
    "inout",
    "interface",
    "namespace",
    "nointerpolation",
    "out",
    "precise",
    "return",
    "row_major",
    "shared",
    "sizeof",
    "snorm",
    "static",
    "struct",
    "switch",
    "tbuffer",
    "template",
    "true",
    "typedef",
    "typename",
    "uniform",
    "unorm",
    "volatile",
    "while",
};

bool tb_shader_token_equals(const tb_shader_token *token, const char *text)
{
    const size_t length = tb_str_length(text);
    return token->length == length && memcmp(token->text, text, length) == 0;
}

tb_shader_name tb_shader_token_name(const tb_shader_token *token)
{
    const tb_shader_name name = {token->text, token->length};
    return name;
}

bool tb_shader_is_keyword(const tb_shader_token *token)
{
    if (token->kind != TB_SHADER_TOKEN_IDENTIFIER)
    {
        return false;
    }
    for (size_t index = 0; index < sizeof(keywords) / sizeof(keywords[0]); index++)
    {
        if (tb_shader_token_equals(token, keywords[index]))
        {
            return true;
        }
    }
    return false;
}

const tb_shader_token *tb_shader_peek(tb_shader_parser *parser)
{
    if (parser->stopped || parser->position >= parser->count)
    {
        return &parser->end;
    }
    if (parser->split_shift)
    {
        return &parser->half_shift;
    }
    return &parser->tokens[parser->position];
}

const tb_shader_token *tb_shader_peek_at(tb_shader_parser *parser, size_t ahead)
{
    if (ahead == 0)
    {
        return tb_shader_peek(parser);
    }
    if (parser->stopped || ahead >= parser->count - parser->position)
    {
        return &parser->end;
    }
    return &parser->tokens[parser->position + ahead];
}

void tb_shader_advance(tb_shader_parser *parser)
{
    if (parser->position < parser->count)
    {
        parser->split_shift = false;
        parser->position++;
    }
}

bool tb_shader_at_end(tb_shader_parser *parser)
{
    return tb_shader_peek(parser)->kind == TB_SHADER_TOKEN_END;
}

bool tb_shader_is(tb_shader_parser *parser, const char *spelling)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (token->kind == TB_SHADER_TOKEN_PUNCTUATOR)
    {
        return tb_shader_token_is(token, spelling);
    }
    return token->kind == TB_SHADER_TOKEN_IDENTIFIER && tb_shader_token_equals(token, spelling);
}

bool tb_shader_accept(tb_shader_parser *parser, const char *spelling)
{
    if (!tb_shader_is(parser, spelling))
    {
        return false;
    }
    tb_shader_advance(parser);
    return true;
}

bool tb_shader_expect(tb_shader_parser *parser, const char *spelling)
{
    if (tb_shader_accept(parser, spelling))
    {
        return true;
    }
    char quoted[16] = {0};
    (void)tb_fmt_buffer(quoted, sizeof(quoted), "'%s'", spelling);
    tb_shader_parse_expected(parser, quoted);
    return false;
}

bool tb_shader_expect_closing_angle(tb_shader_parser *parser)
{
    if (tb_shader_accept(parser, ">"))
    {
        return true;
    }
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (token->kind == TB_SHADER_TOKEN_PUNCTUATOR && tb_shader_token_is(token, ">>"))
    {
        /* `Buffer<vector<float, 4>>`: the first `>` closes this list, and the
         * second is read next. */
        parser->half_shift = *token;
        parser->half_shift.text = token->text + 1;
        parser->half_shift.length = 1;
        parser->half_shift.location.column++;
        parser->half_shift.flags = 0;
        parser->split_shift = true;
        return true;
    }
    tb_shader_parse_expected(parser, "'>'");
    return false;
}

/* -------------------------------------------------------------------------
 * Errors and recovery.
 * ------------------------------------------------------------------------- */

static void add_diagnostic(tb_shader_parser *parser, tb_shader_location location,
                           const char *message)
{
    const tb_shader_diagnostic diagnostic = {TB_SHADER_ERROR, parser->unit->files[location.file],
                                             location.line, location.column, message};
    (void)tb_array_push(parser->diagnostics, &diagnostic);
    parser->error_count++;
    if (parser->error_count >= TB_SHADER_MAX_ERRORS)
    {
        const tb_shader_diagnostic last = {TB_SHADER_ERROR, diagnostic.path, location.line,
                                           location.column, "too many errors; stopping"};
        (void)tb_array_push(parser->diagnostics, &last);
        parser->stopped = true;
    }
}

void tb_shader_parse_error(tb_shader_parser *parser, const tb_shader_token *at, const char *format,
                           ...)
{
    if (parser->panic || parser->stopped)
    {
        return;
    }
    parser->panic = true;
    va_list arguments;
    va_start(arguments, format);
    const char *const message = tb_fmt_vstring(&parser->allocator, format, arguments);
    va_end(arguments);
    add_diagnostic(parser, at->location, message);
}

void tb_shader_parse_expected(tb_shader_parser *parser, const char *what)
{
    const tb_shader_token *const token = tb_shader_peek(parser);
    if (token->kind == TB_SHADER_TOKEN_END)
    {
        tb_shader_parse_error(parser, token, "expected %s at the end of the file", what);
        return;
    }
    const int length = token->length > MAX_QUOTED_TOKEN ? MAX_QUOTED_TOKEN : (int)token->length;
    tb_shader_parse_error(parser, token, "expected %s, found '%.*s'", what, length, token->text);
}

static bool opens_group(const tb_shader_token *token)
{
    return token->kind == TB_SHADER_TOKEN_PUNCTUATOR &&
           (tb_shader_token_is(token, "(") || tb_shader_token_is(token, "[") ||
            tb_shader_token_is(token, "{"));
}

static bool closes_group(const tb_shader_token *token)
{
    return token->kind == TB_SHADER_TOKEN_PUNCTUATOR &&
           (tb_shader_token_is(token, ")") || tb_shader_token_is(token, "]") ||
            tb_shader_token_is(token, "}"));
}

void tb_shader_recover(tb_shader_parser *parser)
{
    size_t nesting = 0;
    while (!tb_shader_at_end(parser))
    {
        const tb_shader_token *const token = tb_shader_peek(parser);
        const bool is_brace = tb_shader_token_is(token, "}");
        if (nesting == 0 && is_brace)
        {
            break;
        }
        if (nesting == 0 && tb_shader_token_is(token, ";"))
        {
            tb_shader_advance(parser);
            break;
        }
        if (opens_group(token))
        {
            nesting++;
        }
        else if (closes_group(token) && nesting > 0)
        {
            nesting--;
            if (nesting == 0 && is_brace)
            {
                tb_shader_advance(parser);
                break;
            }
        }
        tb_shader_advance(parser);
    }
    parser->panic = false;
}

bool tb_shader_enter(tb_shader_parser *parser)
{
    if (parser->stopped)
    {
        return false;
    }
    if (parser->depth == TB_SHADER_MAX_SYNTAX_NESTING)
    {
        const tb_shader_token *const token = tb_shader_peek(parser);
        parser->panic = false;
        tb_shader_parse_error(parser, token, "nesting is deeper than %zu levels",
                              TB_SHADER_MAX_SYNTAX_NESTING);
        parser->stopped = true;
        return false;
    }
    parser->depth++;
    return true;
}

void tb_shader_leave(tb_shader_parser *parser)
{
    parser->depth--;
}

/* -------------------------------------------------------------------------
 * Nodes and names.
 * ------------------------------------------------------------------------- */

tb_shader_node *tb_shader_new_node(tb_shader_parser *parser, tb_shader_node_kind kind,
                                   tb_shader_location location)
{
    tb_shader_node *const node = TB_ARENA_NEW(parser->arena, tb_shader_node);
    node->kind = kind;
    node->location = location;
    return node;
}

tb_shader_type_syntax *tb_shader_new_type_syntax(tb_shader_parser *parser,
                                                 tb_shader_type_syntax_kind kind,
                                                 tb_shader_location location)
{
    tb_shader_type_syntax *const type = TB_ARENA_NEW(parser->arena, tb_shader_type_syntax);
    type->kind = kind;
    type->location = location;
    return type;
}

tb_shader_name tb_shader_join_names(tb_shader_parser *parser, tb_shader_name left,
                                    const char *separator, tb_shader_name right)
{
    const size_t separator_length = tb_str_length(separator);
    const size_t length = left.length + separator_length + right.length;
    char *const text = tb_arena_alloc(parser->arena, length + 1, 1);
    memcpy(text, left.text, left.length);
    memcpy(text + left.length, separator, separator_length);
    memcpy(text + left.length + separator_length, right.text, right.length);
    text[length] = '\0';
    const tb_shader_name joined = {text, length};
    return joined;
}

void tb_shader_list_append(tb_shader_node_list *list, tb_shader_node *node)
{
    if (list->last == NULL)
    {
        list->first = node;
    }
    else
    {
        list->last->next = node;
    }
    list->last = node;
    list->count++;
}

/* -------------------------------------------------------------------------
 * Scopes.
 * ------------------------------------------------------------------------- */

typedef struct scope_change
{
    const tb_shader_name *key;
    void *previous;
    bool existed;
} scope_change;

static uint64_t name_hash(const void *key)
{
    const tb_shader_name *const name = key;
    return tb_hash_bytes(name->text, name->length);
}

static bool name_equal(const void *left, const void *right)
{
    const tb_shader_name *const a = left;
    const tb_shader_name *const b = right;
    return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

static void *kind_value(tb_shader_name_kind kind)
{
    return (void *)(uintptr_t)kind;
}

static const tb_shader_name *stored_name(tb_shader_parser *parser, tb_shader_name name)
{
    tb_shader_name *const key = TB_ARENA_NEW(parser->arena, tb_shader_name);
    *key = name;
    return key;
}

size_t tb_shader_push_scope(tb_shader_parser *parser)
{
    return tb_array_length(parser->scope_changes);
}

void tb_shader_pop_scope(tb_shader_parser *parser, size_t mark)
{
    while (tb_array_length(parser->scope_changes) > mark)
    {
        scope_change change = {0};
        tb_array_pop(parser->scope_changes, &change);
        if (change.existed)
        {
            (void)tb_table_put(parser->scoped, change.key, change.previous);
        }
        else
        {
            (void)tb_table_remove(parser->scoped, change.key, NULL);
        }
    }
}

void tb_shader_declare_scoped(tb_shader_parser *parser, tb_shader_name name,
                              tb_shader_name_kind kind)
{
    scope_change change = {0};
    tb_table_entry entry = {0};
    change.existed = tb_table_find(parser->scoped, &name, &entry);
    change.key = change.existed ? entry.key : stored_name(parser, name);
    change.previous = entry.value;
    (void)tb_array_push(parser->scope_changes, &change);
    (void)tb_table_put(parser->scoped, change.key, kind_value(kind));
}

void tb_shader_declare(tb_shader_parser *parser, tb_shader_name name, tb_shader_name_kind kind)
{
    if (name.length == 0)
    {
        return;
    }
    if (parser->local_depth > 0)
    {
        tb_shader_declare_scoped(parser, name, kind);
        return;
    }
    const tb_shader_name qualified =
        parser->path.length == 0 ? name : tb_shader_join_names(parser, parser->path, "::", name);
    (void)tb_table_put(parser->names, stored_name(parser, qualified), kind_value(kind));
}

tb_shader_name tb_shader_enter_path(tb_shader_parser *parser, tb_shader_name name)
{
    const tb_shader_name previous = parser->path;
    parser->path = previous.length == 0 ? name : tb_shader_join_names(parser, previous, "::", name);
    return previous;
}

/* The scalar, vector and matrix types: `float`, `uint3`, `half4x4`. */
static bool is_numeric_type(tb_shader_name name)
{
    static const char *const scalars[] = {
        "bool",       "int",        "uint",     "dword",    "half",      "float",     "double",
        "min16float", "min10float", "min16int", "min12int", "min16uint", "int16_t",   "uint16_t",
        "int32_t",    "uint32_t",   "int64_t",  "uint64_t", "float16_t", "float32_t", "float64_t",
    };
    for (size_t index = 0; index < sizeof(scalars) / sizeof(scalars[0]); index++)
    {
        const size_t length = tb_str_length(scalars[index]);
        if (name.length < length || memcmp(name.text, scalars[index], length) != 0)
        {
            continue;
        }
        const char *const rest = name.text + length;
        const size_t rest_length = name.length - length;
        const bool is_dimension = rest_length > 0 && rest[0] >= '1' && rest[0] <= '4';
        if (rest_length == 0)
        {
            return true;
        }
        if (rest_length == 1 && is_dimension)
        {
            return true;
        }
        if (rest_length == 3 && is_dimension && rest[1] == 'x' && rest[2] >= '1' && rest[2] <= '4')
        {
            return true;
        }
    }
    return false;
}

static tb_shader_name_kind table_kind(const tb_table *table, tb_shader_name name)
{
    tb_table_entry entry = {0};
    if (!tb_table_find(table, &name, &entry))
    {
        return TB_SHADER_NAME_UNKNOWN;
    }
    return (tb_shader_name_kind)(uintptr_t)entry.value;
}

/* A name looked up as `prefix::name`, or as `name` when the prefix is empty. */
static tb_shader_name_kind lookup_in(tb_shader_parser *parser, tb_shader_name prefix,
                                     tb_shader_name name)
{
    if (prefix.length == 0)
    {
        return table_kind(parser->names, name);
    }
    char buffer[LOOKUP_BUFFER_SIZE] = {0};
    const size_t length = prefix.length + 2 + name.length;
    if (length > sizeof(buffer))
    {
        return table_kind(parser->names, tb_shader_join_names(parser, prefix, "::", name));
    }
    memcpy(buffer, prefix.text, prefix.length);
    memcpy(buffer + prefix.length, "::", 2);
    memcpy(buffer + prefix.length + 2, name.text, name.length);
    const tb_shader_name joined = {buffer, length};
    return table_kind(parser->names, joined);
}

/* The prefix without its last component: `N` of `N::S`, and empty for `N`. */
static tb_shader_name outer_prefix(tb_shader_name prefix)
{
    size_t length = prefix.length;
    while (length >= 2 && !(prefix.text[length - 2] == ':' && prefix.text[length - 1] == ':'))
    {
        length--;
    }
    const tb_shader_name outer = {prefix.text, length >= 2 ? length - 2 : 0};
    return outer;
}

tb_shader_name_kind tb_shader_lookup(tb_shader_parser *parser, tb_shader_name name)
{
    if (name.length > 2 && name.text[0] == ':' && name.text[1] == ':')
    {
        const tb_shader_name global = {name.text + 2, name.length - 2};
        return table_kind(parser->names, global);
    }
    const tb_shader_name_kind local = table_kind(parser->scoped, name);
    if (local != TB_SHADER_NAME_UNKNOWN)
    {
        return local;
    }
    tb_shader_name prefix = parser->path;
    for (;;)
    {
        const tb_shader_name_kind found = lookup_in(parser, prefix, name);
        if (found != TB_SHADER_NAME_UNKNOWN)
        {
            return found;
        }
        if (prefix.length == 0)
        {
            break;
        }
        prefix = outer_prefix(prefix);
    }
    return is_numeric_type(name) ? TB_SHADER_NAME_TYPE : TB_SHADER_NAME_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * Built-in types.
 * ------------------------------------------------------------------------- */

static const char *const builtin_types[] = {
    "void",
    "string",
    "sampler",
    "sampler1D",
    "sampler2D",
    "sampler3D",
    "samplerCUBE",
    "SamplerState",
    "SamplerComparisonState",
    "texture",
    "ByteAddressBuffer",
    "RWByteAddressBuffer",
    "RasterizerOrderedByteAddressBuffer",
    "RaytracingAccelerationStructure",
    "RayDesc",
    "BuiltInTriangleIntersectionAttributes",
    "RaytracingPipelineConfig",
    "RaytracingPipelineConfig1",
    "RaytracingShaderConfig",
    "GlobalRootSignature",
    "LocalRootSignature",
    "StateObjectConfig",
    "SubobjectToExportsAssociation",
    "TriangleHitGroup",
    "ProceduralPrimitiveHitGroup",
    "int8_t4_packed",
    "uint8_t4_packed",
    "dx::HitObject",
};

static const char *const builtin_template_types[] = {
    "vector",
    "matrix",
    "Buffer",
    "RWBuffer",
    "StructuredBuffer",
    "RWStructuredBuffer",
    "AppendStructuredBuffer",
    "ConsumeStructuredBuffer",
    "RasterizerOrderedBuffer",
    "RasterizerOrderedStructuredBuffer",
    "Texture1D",
    "Texture1DArray",
    "Texture2D",
    "Texture2DArray",
    "Texture2DMS",
    "Texture2DMSArray",
    "Texture3D",
    "TextureCube",
    "TextureCubeArray",
    "RWTexture1D",
    "RWTexture1DArray",
    "RWTexture2D",
    "RWTexture2DArray",
    "RWTexture2DMS",
    "RWTexture2DMSArray",
    "RWTexture3D",
    "RasterizerOrderedTexture1D",
    "RasterizerOrderedTexture1DArray",
    "RasterizerOrderedTexture2D",
    "RasterizerOrderedTexture2DArray",
    "RasterizerOrderedTexture3D",
    "FeedbackTexture2D",
    "FeedbackTexture2DArray",
    "ConstantBuffer",
    "TextureBuffer",
    "RayQuery",
    "InputPatch",
    "OutputPatch",
    "PointStream",
    "LineStream",
    "TriangleStream",
};

static void declare_builtins(tb_shader_parser *parser)
{
    for (size_t index = 0; index < sizeof(builtin_types) / sizeof(builtin_types[0]); index++)
    {
        const tb_shader_name name = {builtin_types[index], tb_str_length(builtin_types[index])};
        (void)tb_table_put(parser->names, stored_name(parser, name),
                           kind_value(TB_SHADER_NAME_TYPE));
    }
    const size_t template_count =
        sizeof(builtin_template_types) / sizeof(builtin_template_types[0]);
    for (size_t index = 0; index < template_count; index++)
    {
        const char *const text = builtin_template_types[index];
        const tb_shader_name name = {text, tb_str_length(text)};
        (void)tb_table_put(parser->names, stored_name(parser, name),
                           kind_value(TB_SHADER_NAME_TEMPLATE_TYPE));
    }
    const tb_shader_name namespaces[] = {{"dx", 2}, {"vk", 2}};
    for (size_t index = 0; index < sizeof(namespaces) / sizeof(namespaces[0]); index++)
    {
        (void)tb_table_put(parser->names, stored_name(parser, namespaces[index]),
                           kind_value(TB_SHADER_NAME_NAMESPACE));
    }
}

/* -------------------------------------------------------------------------
 * Reading ahead.
 * ------------------------------------------------------------------------- */

static const tb_shader_token *token_at(tb_shader_parser *parser, size_t index)
{
    if (parser->stopped || index >= parser->count)
    {
        return &parser->end;
    }
    if (index == parser->position && parser->split_shift)
    {
        return &parser->half_shift;
    }
    return &parser->tokens[index];
}

static bool is_name_token(const tb_shader_token *token)
{
    return token->kind == TB_SHADER_TOKEN_IDENTIFIER && !tb_shader_is_keyword(token);
}

static bool is_punctuator(const tb_shader_token *token, const char *spelling)
{
    return token->kind == TB_SHADER_TOKEN_PUNCTUATOR && tb_shader_token_is(token, spelling);
}

size_t tb_shader_scan_name(tb_shader_parser *parser, size_t index)
{
    size_t cursor = index;
    if (is_punctuator(token_at(parser, cursor), "::"))
    {
        cursor++;
    }
    if (!is_name_token(token_at(parser, cursor)))
    {
        return index;
    }
    cursor++;
    while (is_punctuator(token_at(parser, cursor), "::") &&
           is_name_token(token_at(parser, cursor + 1)))
    {
        cursor += 2;
    }
    return cursor;
}

tb_shader_name tb_shader_scanned_name(tb_shader_parser *parser, size_t begin, size_t end)
{
    if (end - begin == 1)
    {
        return tb_shader_token_name(token_at(parser, begin));
    }
    size_t length = 0;
    for (size_t index = begin; index < end; index++)
    {
        length += token_at(parser, index)->length;
    }
    char *const text = tb_arena_alloc(parser->arena, length + 1, 1);
    size_t offset = 0;
    for (size_t index = begin; index < end; index++)
    {
        const tb_shader_token *const token = token_at(parser, index);
        memcpy(text + offset, token->text, token->length);
        offset += token->length;
    }
    text[length] = '\0';
    const tb_shader_name name = {text, length};
    return name;
}

size_t tb_shader_scan_angles(tb_shader_parser *parser, size_t index)
{
    if (!is_punctuator(token_at(parser, index), "<"))
    {
        return index;
    }
    size_t angles = 0;
    size_t groups = 0;
    for (size_t cursor = index; cursor < parser->count; cursor++)
    {
        const tb_shader_token *const token = token_at(parser, cursor);
        if (token->kind != TB_SHADER_TOKEN_PUNCTUATOR)
        {
            continue;
        }
        if (tb_shader_token_is(token, "(") || tb_shader_token_is(token, "["))
        {
            groups++;
            /* Deeper than the parser would read: no type, and no reason to read on. */
            if (groups > TB_SHADER_MAX_SYNTAX_NESTING)
            {
                return index;
            }
        }
        else if (tb_shader_token_is(token, ")") || tb_shader_token_is(token, "]"))
        {
            if (groups == 0)
            {
                return index;
            }
            groups--;
        }
        else if (tb_shader_token_is(token, ";") || tb_shader_token_is(token, "{") ||
                 tb_shader_token_is(token, "}"))
        {
            return index;
        }
        else if (groups > 0)
        {
            continue;
        }
        else if (tb_shader_token_is(token, "<"))
        {
            angles++;
            if (angles > TB_SHADER_MAX_SYNTAX_NESTING)
            {
                return index;
            }
        }
        else if (tb_shader_token_is(token, ">") || tb_shader_token_is(token, ">>"))
        {
            const size_t closed = token->length;
            if (angles <= closed)
            {
                return cursor + 1;
            }
            angles -= closed;
        }
    }
    return index;
}

size_t tb_shader_scan_type(tb_shader_parser *parser, size_t index)
{
    const size_t end = tb_shader_scan_name(parser, index);
    if (end == index)
    {
        return index;
    }
    const tb_shader_name_kind kind =
        tb_shader_lookup(parser, tb_shader_scanned_name(parser, index, end));
    if (kind == TB_SHADER_NAME_TYPE)
    {
        return end;
    }
    if (kind == TB_SHADER_NAME_TEMPLATE_TYPE)
    {
        const size_t after = tb_shader_scan_angles(parser, end);
        return after == end ? end : after;
    }
    return index;
}

/* -------------------------------------------------------------------------
 * Parsing a unit.
 * ------------------------------------------------------------------------- */

static tb_shader_token end_token(const tb_shader_unit *unit)
{
    tb_shader_token end = {0};
    end.kind = TB_SHADER_TOKEN_END;
    end.text = "";
    end.location.file = 0;
    end.location.line = 1;
    end.location.column = 1;
    if (unit->token_count > 0)
    {
        const tb_shader_token *const last = &unit->tokens[unit->token_count - 1];
        end.location = last->location;
        end.location.column += (uint32_t)last->length;
    }
    return end;
}

tb_shader_result tb_shader_parse(const tb_shader_unit *unit, tb_arena *arena,
                                 tb_shader_syntax *syntax)
{
    tb_shader_parser parser = {0};
    parser.unit = unit;
    parser.arena = arena;
    parser.allocator = tb_arena_allocator(arena);
    parser.tokens = unit->tokens;
    parser.count = unit->token_count;
    parser.end = end_token(unit);
    parser.diagnostics = TB_ARRAY_CREATE(&parser.allocator, tb_shader_diagnostic);
    tb_array_append(parser.diagnostics, unit->diagnostics, unit->diagnostic_count);
    parser.error_count = unit->error_count;
    parser.stopped = unit->error_count >= TB_SHADER_MAX_ERRORS;
    parser.names = tb_table_create(&parser.allocator, 256, name_hash, name_equal);
    parser.scoped = tb_table_create(&parser.allocator, 64, name_hash, name_equal);
    parser.scope_changes = TB_ARRAY_CREATE(&parser.allocator, scope_change);
    declare_builtins(&parser);

    tb_shader_node_list declarations = {0};
    tb_shader_parse_declarations(&parser, &declarations, false);

    syntax->declarations = declarations.first;
    syntax->declaration_count = declarations.count;
    syntax->files = unit->files;
    syntax->file_count = unit->file_count;
    syntax->diagnostics = tb_array_data(parser.diagnostics);
    syntax->diagnostic_count = tb_array_length(parser.diagnostics);
    syntax->error_count = parser.error_count;
    return parser.error_count > 0 ? TB_SHADER_FAILED : TB_SHADER_OK;
}
