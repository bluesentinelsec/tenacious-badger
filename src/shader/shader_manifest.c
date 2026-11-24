/* Shader manifests: the shaders `badger-shaderc` compiles into a library, one
 * to a line. The format is docs/shader-cache.md's. */
#include "badger/arena.h"
#include "badger/array.h"
#include "badger/fmt.h"
#include "badger/shader.h"
#include "badger/shader_library.h"
#include "badger/str.h"

#include <string.h>

#define STAGE_COUNT ((size_t)TB_SHADER_STAGE_CALLABLE + 1)

/* A line's fields: a path, a stage, an entry point, then its defines. */
#define FIXED_FIELDS ((size_t)3)
#define MOST_FIELDS (FIXED_FIELDS + TB_SHADER_MANIFEST_MAX_DEFINES)

typedef struct field
{
    const char *text;
    size_t length;
    size_t column; /* From 1. */
} field;

typedef struct reader
{
    tb_arena *arena;
    tb_allocator allocator;
    const char *name;
    tb_array *entries;     /* tb_shader_manifest_entry */
    tb_array *diagnostics; /* tb_shader_diagnostic */
    size_t error_count;
} reader;

/* Report an error at a line and a column of the manifest. */
static void report(reader *state, size_t line, size_t column, const char *message)
{
    tb_shader_diagnostic diagnostic = {0};
    diagnostic.severity = TB_SHADER_ERROR;
    diagnostic.path = state->name;
    diagnostic.line = line;
    diagnostic.column = column;
    diagnostic.message = message;
    (void)tb_array_push(state->diagnostics, &diagnostic);
    state->error_count++;
}

static bool is_space(char character)
{
    return character == ' ' || character == '\t' || character == '\r';
}

static bool is_identifier(const char *text, size_t length)
{
    if (length == 0 || (text[0] >= '0' && text[0] <= '9'))
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        const char character = text[index];
        const bool letter = (character >= 'a' && character <= 'z') ||
                            (character >= 'A' && character <= 'Z') || character == '_';
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit)
        {
            return false;
        }
    }
    return true;
}

static const char *copy_text(reader *state, const char *text, size_t length)
{
    char *const copy = tb_arena_alloc(state->arena, length + 1, 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* Split a line, its comment left out, into fields; false, reported, when it
 * has more than MOST_FIELDS. */
static bool split(reader *state, const char *line, size_t length, size_t number, field *fields,
                  size_t *count)
{
    size_t at = 0;
    *count = 0;
    while (at < length && line[at] != '#')
    {
        if (is_space(line[at]))
        {
            at++;
            continue;
        }
        const size_t start = at;
        while (at < length && line[at] != '#' && !is_space(line[at]))
        {
            at++;
        }
        if (*count == MOST_FIELDS)
        {
            report(state, number, start + 1,
                   tb_fmt_string(&state->allocator, "a shader has at most %zu defines",
                                 TB_SHADER_MANIFEST_MAX_DEFINES));
            return false;
        }
        fields[*count].text = line + start;
        fields[*count].length = at - start;
        fields[*count].column = start + 1;
        (*count)++;
    }
    return true;
}

static bool find_stage(const field *text, tb_shader_stage *stage)
{
    for (size_t index = 0; index < STAGE_COUNT; index++)
    {
        const char *const name = tb_shader_stage_name((tb_shader_stage)index);
        if (tb_str_length(name) == text->length && memcmp(name, text->text, text->length) == 0)
        {
            *stage = (tb_shader_stage)index;
            return true;
        }
    }
    return false;
}

/* Report a field as what it is not. */
static void report_field(reader *state, size_t number, const field *text, const char *what)
{
    report(state, number, text->column,
           tb_fmt_string(&state->allocator, "'%.*s' is %s", (int)text->length, text->text, what));
}

/* Read a define, `NAME` or `NAME=VALUE`; false after an error. */
static bool read_define(reader *state, const field *text, size_t number, tb_shader_define *define)
{
    const char *const equals = memchr(text->text, '=', text->length);
    const size_t name_length = equals == NULL ? text->length : (size_t)(equals - text->text);
    if (!is_identifier(text->text, name_length))
    {
        report_field(state, number, text, "no define: write NAME or NAME=VALUE");
        return false;
    }
    define->name = copy_text(state, text->text, name_length);
    define->value = NULL;
    if (equals != NULL)
    {
        define->value = copy_text(state, equals + 1, text->length - name_length - 1);
    }
    return true;
}

/* Read one line's shader, from its fields, of which there is one at least. */
static void read_entry(reader *state, const field *fields, size_t count, size_t number)
{
    tb_shader_manifest_entry entry = {0};
    entry.line = number;
    if (count < FIXED_FIELDS)
    {
        report_field(state, number, &fields[0],
                     "not a shader: write its path, its stage and its entry point");
        return;
    }
    if (!tb_shader_path_check(fields[0].text, fields[0].length))
    {
        report_field(state, number, &fields[0], "not a virtual shader path");
        return;
    }
    if (!find_stage(&fields[1], &entry.stage))
    {
        report_field(state, number, &fields[1],
                     "no stage: write one as [shader(\"...\")] names it, such as pixel");
        return;
    }
    if (!is_identifier(fields[2].text, fields[2].length))
    {
        report_field(state, number, &fields[2], "no function's name");
        return;
    }
    entry.path = copy_text(state, fields[0].text, fields[0].length);
    entry.entry_point = copy_text(state, fields[2].text, fields[2].length);
    entry.define_count = count - FIXED_FIELDS;
    tb_shader_define *const defines =
        tb_arena_calloc(state->arena, entry.define_count == 0 ? 1 : entry.define_count,
                        sizeof(tb_shader_define), _Alignof(tb_shader_define));
    for (size_t index = 0; index < entry.define_count; index++)
    {
        if (!read_define(state, &fields[FIXED_FIELDS + index], number, &defines[index]))
        {
            return;
        }
    }
    entry.defines = defines;
    (void)tb_array_push(state->entries, &entry);
}

tb_shader_result tb_shader_manifest_read(const char *text, size_t length, const char *name,
                                         tb_arena *arena, tb_shader_manifest *manifest)
{
    const tb_shader_manifest empty = {0};
    *manifest = empty;
    reader state = {0};
    state.arena = arena;
    state.allocator = tb_arena_allocator(arena);
    state.name = copy_text(&state, name, tb_str_length(name));
    state.entries = TB_ARRAY_CREATE(&state.allocator, tb_shader_manifest_entry);
    state.diagnostics = TB_ARRAY_CREATE(&state.allocator, tb_shader_diagnostic);
    size_t number = 0;
    size_t start = 0;
    while (start < length && state.error_count < TB_SHADER_MAX_ERRORS)
    {
        const char *const line = text + start;
        const char *const newline = memchr(line, '\n', length - start);
        const size_t line_length = newline == NULL ? length - start : (size_t)(newline - line);
        field fields[MOST_FIELDS] = {{0}};
        size_t count = 0;
        number++;
        if (memchr(line, '\0', line_length) != NULL)
        {
            report(&state, number, 1, "a manifest holds no NUL byte");
        }
        else if (split(&state, line, line_length, number, fields, &count) && count > 0)
        {
            read_entry(&state, fields, count, number);
        }
        start += line_length + 1;
    }
    manifest->entries = tb_array_data(state.entries);
    manifest->entry_count = tb_array_length(state.entries);
    manifest->diagnostics = tb_array_data(state.diagnostics);
    manifest->diagnostic_count = tb_array_length(state.diagnostics);
    return state.error_count == 0 ? TB_SHADER_OK : TB_SHADER_FAILED;
}
