/* The shader module's public functions: preprocessing a request into text,
 * loading files from a VFS, and naming results and diagnostics. */
#include "shader_internal.h"

#include "badger/array.h"
#include "badger/fmt.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include <string.h>

/* Lines the text skips with blank lines before it marks the jump with `#line` instead. */
#define MAX_BLANK_LINES ((uint32_t)8)

/* -------------------------------------------------------------------------
 * The preprocessed text.
 * ------------------------------------------------------------------------- */

static bool is_word_byte(char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

/* Whether two tokens written with nothing between them would read back as
 * something else, so a space must separate them. */
static bool would_merge(const tb_shader_token *left, const tb_shader_token *right)
{
    const char last = left->text[left->length - 1];
    const char first = right->text[0];
    if (is_word_byte(last) && is_word_byte(first))
    {
        return true;
    }
    /* A number takes a `.` and a sign after it into itself: `1.`, `1e+`. */
    if (left->kind == TB_SHADER_TOKEN_NUMBER && (first == '+' || first == '-' || first == '.'))
    {
        return true;
    }
    if (last == '.' && first >= '0' && first <= '9')
    {
        return true;
    }
    if (last == '/' && (first == '/' || first == '*'))
    {
        return true;
    }
    if (left->kind != TB_SHADER_TOKEN_PUNCTUATOR || right->kind != TB_SHADER_TOKEN_PUNCTUATOR)
    {
        return false;
    }
    char joined[4] = {0};
    const size_t left_part = left->length < 2 ? left->length : 2;
    const size_t right_part = right->length < 2 ? right->length : 2;
    memcpy(joined, left->text + left->length - left_part, left_part);
    memcpy(joined + left_part, right->text, right_part);
    return tb_shader_punctuator_length(joined, left_part + right_part) > left_part;
}

static void append(tb_array *text, const char *bytes, size_t length)
{
    tb_array_append(text, bytes, length);
}

static void append_string(tb_array *text, const char *string)
{
    append(text, string, tb_str_length(string));
}

static void append_line_marker(tb_array *text, const tb_shader_unit *unit,
                               tb_shader_location location)
{
    char number[16] = {0};
    const size_t length = tb_fmt_buffer(number, sizeof(number), "%u", (unsigned)location.line);
    append_string(text, "#line ");
    append(text, number, length);
    append_string(text, " \"");
    append_string(text, unit->files[location.file]);
    append_string(text, "\"\n");
}

void tb_shader_render(const tb_shader_unit *unit, bool line_markers, tb_array *text,
                      tb_array *offsets)
{
    const tb_shader_token *previous = NULL;
    bool line_open = false; /* Something is written on the current line. */
    for (size_t index = 0; index < unit->token_count; index++)
    {
        const tb_shader_token *const token = &unit->tokens[index];
        const tb_shader_location location = token->location;
        const bool pragma = token->kind == TB_SHADER_TOKEN_PRAGMA;
        if (previous == NULL && line_markers)
        {
            append_line_marker(text, unit, location);
        }
        else if (previous == NULL)
        {
            /* The text starts at line 1 of the first token's file. */
            const uint32_t leading = location.line - 1;
            const uint32_t breaks = leading <= MAX_BLANK_LINES ? leading : 1;
            for (uint32_t line = 0; line < breaks; line++)
            {
                append_string(text, "\n");
            }
        }
        else
        {
            const tb_shader_location last = previous->location;
            const bool same_file = location.file == last.file;
            const bool forward = same_file && location.line >= last.line;
            const uint32_t gap = forward ? location.line - last.line : 0;
            const bool new_line =
                !forward || gap > 0 || pragma || previous->kind == TB_SHADER_TOKEN_PRAGMA;
            if (!new_line)
            {
                if ((token->flags & TB_SHADER_TOKEN_SPACE_BEFORE) != 0 ||
                    would_merge(previous, token))
                {
                    append_string(text, " ");
                }
            }
            else if (forward && gap <= MAX_BLANK_LINES)
            {
                const uint32_t breaks = gap == 0 ? 1 : gap;
                for (uint32_t line = 0; line < breaks; line++)
                {
                    append_string(text, "\n");
                }
            }
            else
            {
                append_string(text, "\n");
                if (line_markers)
                {
                    append_line_marker(text, unit, location);
                }
            }
        }
        if (pragma)
        {
            append_string(text, "#pragma");
            if (token->length > 0)
            {
                append_string(text, " ");
            }
        }
        if (offsets != NULL)
        {
            const size_t offset = tb_array_length(text);
            tb_array_push(offsets, &offset);
        }
        append(text, token->text, token->length);
        line_open = true;
        previous = token;
    }
    if (line_open)
    {
        append_string(text, "\n");
    }
}

/* -------------------------------------------------------------------------
 * Copying the output to the caller's arena.
 * ------------------------------------------------------------------------- */

static const char *copy_bytes(tb_arena *arena, const char *bytes, size_t length)
{
    char *const copy = tb_arena_try_alloc(arena, length + 1, 1);
    if (copy == NULL)
    {
        return NULL;
    }
    if (length > 0)
    {
        memcpy(copy, bytes, length);
    }
    copy[length] = '\0';
    return copy;
}

static const char *copy_string(tb_arena *arena, const char *string)
{
    return copy_bytes(arena, string, tb_str_length(string));
}

static bool copy_diagnostics(const tb_shader_unit *unit, tb_arena *arena,
                             tb_shader_preprocessed *output)
{
    const size_t count = unit->diagnostic_count;
    tb_shader_diagnostic *const diagnostics =
        tb_arena_try_calloc(arena, count == 0 ? 1 : count, sizeof(tb_shader_diagnostic),
                            _Alignof(tb_shader_diagnostic));
    if (diagnostics == NULL)
    {
        return false;
    }
    for (size_t index = 0; index < count; index++)
    {
        diagnostics[index] = unit->diagnostics[index];
        diagnostics[index].path = copy_string(arena, unit->diagnostics[index].path);
        diagnostics[index].message = copy_string(arena, unit->diagnostics[index].message);
        if (diagnostics[index].path == NULL || diagnostics[index].message == NULL)
        {
            return false;
        }
    }
    output->diagnostics = diagnostics;
    output->diagnostic_count = count;
    return true;
}

static bool copy_dependencies(const tb_shader_unit *unit, tb_arena *arena,
                              tb_shader_preprocessed *output)
{
    const char **const dependencies = tb_arena_try_calloc(
        arena, unit->file_count == 0 ? 1 : unit->file_count, sizeof(char *), _Alignof(char *));
    if (dependencies == NULL)
    {
        return false;
    }
    size_t count = 0;
    for (size_t index = 0; index < unit->file_count; index++)
    {
        if (!unit->files_read[index])
        {
            continue;
        }
        dependencies[count] = copy_string(arena, unit->files[index]);
        if (dependencies[count] == NULL)
        {
            return false;
        }
        count++;
    }
    output->dependencies = dependencies;
    output->dependency_count = count;
    return true;
}

/* Fill the output in the caller's arena. Returns false when it cannot hold it. */
static bool copy_output(const tb_shader_unit *unit, const char *text, size_t length,
                        tb_arena *arena, tb_shader_preprocessed *output)
{
    output->text = copy_bytes(arena, text, length);
    output->length = length;
    return output->text != NULL && copy_dependencies(unit, arena, output) &&
           copy_diagnostics(unit, arena, output);
}

tb_shader_result tb_shader_preprocess(const tb_shader_preprocess_request *request, tb_arena *arena,
                                      tb_shader_preprocessed *output)
{
    const tb_shader_preprocessed empty = {0};
    *output = empty;
    tb_arena *const scratch = tb_arena_create(0);
    tb_shader_unit unit = {0};
    tb_shader_result result = tb_shader_preprocess_unit(request, scratch, &unit);
    const tb_allocator allocator = tb_arena_allocator(scratch);
    tb_array *const text = TB_ARRAY_CREATE(&allocator, char);
    if (result == TB_SHADER_OK)
    {
        tb_shader_render(&unit, (request->flags & TB_SHADER_LINE_MARKERS) != 0, text, NULL);
    }
    const size_t length = tb_array_length(text);
    const char *const bytes = length == 0 ? "" : tb_array_data(text);
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    if (!copy_output(&unit, bytes, length, arena, output))
    {
        tb_arena_rewind(arena, mark);
        *output = empty;
        result = TB_SHADER_NO_MEMORY;
    }
    tb_arena_destroy(scratch);
    return result;
}

/* -------------------------------------------------------------------------
 * The VFS loader.
 * ------------------------------------------------------------------------- */

static tb_shader_load_result load_from_vfs(void *context, const char *path, size_t length,
                                           tb_arena *arena, const char **text, size_t *text_length)
{
    tb_vfs *const vfs = context;
    /* `/Engine/Public/Common.hlsli` is the name `Engine/Public/Common.hlsli`. */
    const char *const name = path + 1;
    const size_t name_length = length - 1;
    tb_vfs_info info = {0};
    const tb_vfs_result queried = tb_vfs_query(vfs, name, name_length, &info);
    if (queried == TB_VFS_NOT_FOUND || (queried == TB_VFS_OK && info.kind != TB_VFS_FILE))
    {
        return TB_SHADER_LOAD_NOT_FOUND;
    }
    if (queried != TB_VFS_OK || info.size > TB_SHADER_MAX_FILE_SIZE)
    {
        return TB_SHADER_LOAD_FAILED;
    }
    const size_t size = (size_t)info.size;
    tb_stream stream = {0};
    const tb_vfs_result opened = tb_vfs_open(vfs, name, name_length, NULL, &stream);
    if (opened != TB_VFS_OK)
    {
        return opened == TB_VFS_NOT_FOUND ? TB_SHADER_LOAD_NOT_FOUND : TB_SHADER_LOAD_FAILED;
    }
    char *const bytes = tb_arena_alloc(arena, size + 1, 1);
    const tb_stream_result read =
        size == 0 ? TB_STREAM_OK : tb_stream_read_exact(&stream, bytes, size);
    (void)tb_stream_close(&stream);
    if (read != TB_STREAM_OK)
    {
        return TB_SHADER_LOAD_FAILED;
    }
    bytes[size] = '\0';
    *text = bytes;
    *text_length = size;
    return TB_SHADER_LOAD_OK;
}

tb_shader_loader tb_shader_vfs_loader(struct tb_vfs *vfs)
{
    const tb_shader_loader loader = {load_from_vfs, vfs};
    return loader;
}

/* -------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

size_t tb_shader_diagnostic_format(const tb_shader_diagnostic *diagnostic, char *buffer,
                                   size_t size)
{
    const char *const severity = diagnostic->severity == TB_SHADER_ERROR ? "error" : "warning";
    if (diagnostic->path[0] == '\0')
    {
        return tb_fmt_buffer(buffer, size, "%s: %s", severity, diagnostic->message);
    }
    return tb_fmt_buffer(buffer, size, "%s(%zu,%zu): %s: %s", diagnostic->path, diagnostic->line,
                         diagnostic->column, severity, diagnostic->message);
}

const char *tb_shader_result_name(tb_shader_result result)
{
    switch (result)
    {
    case TB_SHADER_OK:
        return "ok";
    case TB_SHADER_INVALID:
        return "invalid request";
    case TB_SHADER_FAILED:
        return "failed";
    case TB_SHADER_NO_MEMORY:
        return "no memory";
    }
    return "unknown result";
}
