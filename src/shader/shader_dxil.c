/* Compiling for D3D12 through DXC. DXC compiles the text the preprocessor
 * rendered, so it places its messages in that text; each is moved back to
 * the token at its place, and so to the shader's own file, line and column. */
#include "shader_dxil.h"

#include "badger/fmt.h"
#include "badger/str.h"

#include <string.h>

/* Digits of a line or a column DXC writes, at most: more is not a number of its. */
#define MAX_DIGITS ((size_t)9)

/* What DXC calls its summary of validation errors, which the errors follow. */
#define VALIDATION_SUMMARY "validation errors"

void tb_shader_dxil_profile(tb_shader_stage stage, unsigned shader_model, char *buffer)
{
    static const char *const prefixes[] = {"vs", "ps", "gs", "hs", "ds", "cs", "as", "ms"};
    const size_t stage_index = (size_t)stage;
    const char *const prefix =
        stage_index < sizeof(prefixes) / sizeof(prefixes[0]) ? prefixes[stage_index] : "lib";
    (void)tb_fmt_buffer(buffer, TB_SHADER_PROFILE_CAPACITY, "%s_%u_%u", prefix,
                        (shader_model >> 8) & 0xFFu, shader_model & 0xFFu);
}

/* -------------------------------------------------------------------------
 * Reading a message.
 * ------------------------------------------------------------------------- */

/* One line of DXC's messages, read. */
typedef struct message
{
    tb_shader_severity severity;
    size_t path_begin; /* The place DXC names: `path:line:column`, `path:line`, or none. */
    size_t path_end;
    size_t line;
    size_t column;
    size_t text_begin; /* What it says. */
    size_t text_end;
} message;

/* The kinds of message DXC writes, as it marks them, and what each is. */
typedef struct marker
{
    const char *text;
    size_t length;
    tb_shader_severity severity;
} marker;

static const marker markers[] = {
    {"fatal error: ", sizeof("fatal error: ") - 1, TB_SHADER_ERROR},
    {"error: ", sizeof("error: ") - 1, TB_SHADER_ERROR},
    {"warning: ", sizeof("warning: ") - 1, TB_SHADER_WARNING},
};

/* The number of the digits in [begin, end), or false when they are none,
 * or more than a line or a column has. */
static bool read_number(const char *text, size_t begin, size_t end, size_t *value)
{
    if (begin >= end || end - begin > MAX_DIGITS)
    {
        return false;
    }
    size_t number = 0;
    for (size_t at = begin; at < end; at++)
    {
        if (text[at] < '0' || text[at] > '9')
        {
            return false;
        }
        number = number * 10 + (size_t)(text[at] - '0');
    }
    *value = number;
    return true;
}

/* Read the place before a marker, `path:line:column` or `path:line`, from
 * the right; a place that is neither is all path. */
static void read_place(const char *text, size_t begin, size_t end, message *out)
{
    out->path_begin = begin;
    out->path_end = end;
    const size_t last = tb_str_rfind_char(text, begin, end, ':');
    size_t number = 0;
    if (last == TB_STR_NOT_FOUND || !read_number(text, last + 1, end, &number))
    {
        return;
    }
    out->path_end = last;
    out->line = number;
    const size_t before = tb_str_rfind_char(text, begin, last, ':');
    if (before != TB_STR_NOT_FOUND && read_number(text, before + 1, last, &number))
    {
        out->path_end = before;
        out->column = out->line;
        out->line = number;
    }
}

/* Read a line of DXC's messages: false when it is no error or warning. A
 * marker counts at the start of the line, or after the `: ` that ends a place. */
static bool read_message(const char *text, size_t begin, size_t end, message *out)
{
    const message empty = {0};
    *out = empty;
    size_t found = TB_STR_NOT_FOUND;
    for (size_t index = 0; index < sizeof(markers) / sizeof(markers[0]); index++)
    {
        const marker *const kind = &markers[index];
        size_t at = TB_STR_NOT_FOUND;
        if (tb_str_matches_at(text, begin, kind->text))
        {
            at = begin;
        }
        else
        {
            const size_t after_place = tb_str_find(text, begin, end, kind->text);
            const bool after_colon = after_place != TB_STR_NOT_FOUND && after_place >= begin + 2 &&
                                     text[after_place - 1] == ' ' && text[after_place - 2] == ':';
            at = after_colon ? after_place : TB_STR_NOT_FOUND;
        }
        if (at != TB_STR_NOT_FOUND && (found == TB_STR_NOT_FOUND || at < found))
        {
            found = at;
            out->severity = kind->severity;
            out->text_begin = at + kind->length;
        }
    }
    if (found == TB_STR_NOT_FOUND)
    {
        return false;
    }
    if (found > begin)
    {
        read_place(text, begin, found - 2, out);
    }
    out->text_end = end;
    /* A message ends without a period, and without the line's carriage return. */
    while (out->text_end > out->text_begin &&
           (text[out->text_end - 1] == '\r' || text[out->text_end - 1] == '.'))
    {
        out->text_end--;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Placing a message in the sources.
 * ------------------------------------------------------------------------- */

/* The offset in the rendered text of a line and a column, from 1; a column
 * of 0, or past the line's end, is the line's start or end. False when the
 * text has no such line. */
static bool offset_of(const tb_shader_rendered *rendered, size_t line, size_t column,
                      size_t *offset)
{
    size_t start = 0;
    for (size_t current = 1; current < line; current++)
    {
        const size_t next = tb_str_find_char(rendered->text, start, rendered->length, '\n');
        if (next == TB_STR_NOT_FOUND)
        {
            return false;
        }
        start = next + 1;
    }
    size_t end = tb_str_find_char(rendered->text, start, rendered->length, '\n');
    end = end == TB_STR_NOT_FOUND ? rendered->length : end;
    const size_t wanted = column == 0 ? start : start + (column - 1);
    *offset = wanted < end ? wanted : end;
    return true;
}

/* The last token that starts at or before an offset; the first when none does. */
static size_t token_at(const tb_shader_rendered *rendered, size_t offset)
{
    size_t low = 0;
    size_t high = rendered->unit->token_count;
    while (high - low > 1)
    {
        const size_t middle = low + (high - low) / 2;
        if (rendered->offsets[middle] <= offset)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

/* Place a diagnostic at the token of a line and a column of the rendered
 * text; false when the text has no tokens or no such line. */
static bool place(const tb_shader_rendered *rendered, size_t line, size_t column,
                  tb_shader_diagnostic *diagnostic)
{
    size_t offset = 0;
    if (rendered->unit->token_count == 0 || !offset_of(rendered, line, column, &offset))
    {
        return false;
    }
    const size_t index = token_at(rendered, offset);
    const tb_shader_token *const token = &rendered->unit->tokens[index];
    const size_t start = rendered->offsets[index];
    const bool inside = offset >= start && offset < start + token->length;
    diagnostic->path = rendered->unit->files[token->location.file];
    diagnostic->line = token->location.line;
    diagnostic->column = token->location.column + (inside ? offset - start : 0);
    return true;
}

static const char *copy_range(tb_arena *arena, const char *text, size_t begin, size_t end)
{
    char *const copy = tb_arena_alloc(arena, end - begin + 1, 1);
    memcpy(copy, text + begin, end - begin);
    copy[end - begin] = '\0';
    return copy;
}

size_t tb_shader_dxc_diagnostics(const char *messages, size_t length, const char *name,
                                 const tb_shader_rendered *rendered, tb_arena *arena,
                                 tb_array *diagnostics)
{
    size_t errors = 0;
    for (size_t begin = 0; begin < length;)
    {
        size_t end = tb_str_find_char(messages, begin, length, '\n');
        end = end == TB_STR_NOT_FOUND ? length : end;
        message read = {0};
        const size_t next = end + 1;
        if (!read_message(messages, begin, end, &read))
        {
            begin = next;
            continue;
        }
        tb_shader_diagnostic diagnostic = {0};
        diagnostic.severity = read.severity;
        diagnostic.message = copy_range(arena, messages, read.text_begin, read.text_end);
        diagnostic.path = copy_range(arena, messages, read.path_begin, read.path_end);
        diagnostic.line = read.line;
        diagnostic.column = read.column;
        const bool in_rendered = read.line > 0 && tb_str_equal(diagnostic.path, name);
        if (in_rendered && !place(rendered, read.line, read.column, &diagnostic))
        {
            /* A place past the text's end is no place in the sources. */
            diagnostic.path = "";
            diagnostic.line = 0;
            diagnostic.column = 0;
        }
        if (!tb_str_equal(diagnostic.message, VALIDATION_SUMMARY))
        {
            tb_array_push(diagnostics, &diagnostic);
            errors += diagnostic.severity == TB_SHADER_ERROR ? 1u : 0u;
        }
        begin = next;
    }
    return errors;
}
