/* Compiling for D3D11 through D3DCompiler. D3DCompiler compiles the HLSL
 * written from the form, so it places its messages in that text; each line
 * of which records where in the sources it was written from. */
#include "shader_dxbc.h"

#include "shader_dxil.h"

#include "badger/dxc.h"
#include "badger/fmt.h"
#include "badger/str.h"

#include <string.h>

/* Digits of a line or a column D3DCompiler writes, at most. */
#define MAX_DIGITS ((size_t)9)

unsigned tb_shader_target_model(const tb_shader_compile_request *request)
{
    if (request->target == TB_SHADER_TARGET_DXBC)
    {
        return request->shader_model == 0 ? TB_SHADER_DXBC_MODEL : request->shader_model;
    }
    return tb_shader_dxil_model(request);
}

const char *tb_shader_compiler_name(const tb_shader_compile_request *request)
{
    switch (request->target)
    {
    case TB_SHADER_TARGET_DXIL:
        return request->dxc != NULL ? tb_dxc_version(request->dxc) : NULL;
    case TB_SHADER_TARGET_DXBC:
        return request->d3dcompiler != NULL ? tb_d3dcompiler_version(request->d3dcompiler) : NULL;
    case TB_SHADER_TARGET_NONE:
        break;
    }
    return NULL;
}

bool tb_shader_dxbc_profile(tb_shader_stage stage, char *buffer)
{
    static const char *const profiles[] = {"vs_5_0", "ps_5_0", "gs_5_0",
                                           "hs_5_0", "ds_5_0", "cs_5_0"};
    const size_t index = (size_t)stage;
    if (index >= sizeof(profiles) / sizeof(profiles[0]))
    {
        buffer[0] = '\0';
        return false;
    }
    (void)tb_fmt_buffer(buffer, TB_SHADER_DXBC_PROFILE_CAPACITY, "%s", profiles[index]);
    return true;
}

/* -------------------------------------------------------------------------
 * Reading a message: `name(12,5-9): error X3004: what`.
 * ------------------------------------------------------------------------- */

typedef struct message
{
    tb_shader_severity severity;
    size_t path_begin; /* The place: `name(line,column)`, or none. */
    size_t path_end;
    size_t line;
    size_t column;
    size_t text_begin;
    size_t text_end;
} message;

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

/* Read `name(line,column)` or `name(line,column-column)` from [begin, end);
 * anything else is all path. */
static void read_place(const char *text, size_t begin, size_t end, message *out)
{
    out->path_begin = begin;
    out->path_end = end;
    if (end <= begin || text[end - 1] != ')')
    {
        return;
    }
    const size_t open = tb_str_rfind_char(text, begin, end, '(');
    if (open == TB_STR_NOT_FOUND)
    {
        return;
    }
    const size_t comma = tb_str_find_char(text, open, end, ',');
    if (comma == TB_STR_NOT_FOUND)
    {
        return;
    }
    size_t column_end = tb_str_find_char(text, comma, end, '-');
    column_end = column_end == TB_STR_NOT_FOUND ? end - 1 : column_end;
    size_t line = 0;
    size_t column = 0;
    if (!read_number(text, open + 1, comma, &line) ||
        !read_number(text, comma + 1, column_end, &column))
    {
        return;
    }
    out->path_end = open;
    out->line = line;
    out->column = column;
}

/* Read a line of the messages: false when it is no error or warning. A
 * marker is `error X` or `warning X` at the line's start or after `: `. */
static bool read_message(const char *text, size_t begin, size_t end, message *out)
{
    static const struct
    {
        const char *text;
        tb_shader_severity severity;
    } markers[] = {{"error X", TB_SHADER_ERROR}, {"warning X", TB_SHADER_WARNING}};
    const message empty = {0};
    *out = empty;
    size_t found = TB_STR_NOT_FOUND;
    for (size_t index = 0; index < sizeof(markers) / sizeof(markers[0]); index++)
    {
        size_t at = tb_str_find(text, begin, end, markers[index].text);
        const bool placed =
            at != TB_STR_NOT_FOUND &&
            (at == begin || (at >= begin + 2 && text[at - 1] == ' ' && text[at - 2] == ':'));
        if (placed && (found == TB_STR_NOT_FOUND || at < found))
        {
            found = at;
            out->severity = markers[index].severity;
        }
    }
    if (found == TB_STR_NOT_FOUND)
    {
        return false;
    }
    /* What it says follows the code: `X3004: `. */
    const size_t code_end = tb_str_find(text, found, end, ": ");
    out->text_begin = code_end == TB_STR_NOT_FOUND ? found : code_end + 2;
    if (found > begin)
    {
        read_place(text, begin, found - 2, out);
    }
    out->text_end = end;
    while (out->text_end > out->text_begin &&
           (text[out->text_end - 1] == '\r' || text[out->text_end - 1] == '\n' ||
            text[out->text_end - 1] == ' ' || text[out->text_end - 1] == '.'))
    {
        out->text_end--;
    }
    return true;
}

/* Whether a path D3DCompiler names is `name`, the name it was given: it
 * names it so, or after the directory it ran in, with either separator. */
static bool names_text(const char *path, const char *name)
{
    const size_t path_length = tb_str_length(path);
    const size_t name_length = tb_str_length(name);
    if (path_length < name_length)
    {
        return false;
    }
    const size_t start = path_length - name_length;
    if (start > 0 && path[start - 1] != '\\' && path[start - 1] != '/')
    {
        return false;
    }
    for (size_t at = 0; at < name_length; at++)
    {
        const char given = name[at] == '/' ? '\\' : name[at];
        const char named = path[start + at] == '/' ? '\\' : path[start + at];
        if (given != named)
        {
            return false;
        }
    }
    return true;
}

static const char *copy_range(tb_arena *arena, const char *text, size_t begin, size_t end)
{
    char *const copy = tb_arena_alloc(arena, end - begin + 1, 1);
    memcpy(copy, text + begin, end - begin);
    copy[end - begin] = '\0';
    return copy;
}

size_t tb_shader_d3dcompiler_diagnostics(const char *messages, size_t length, const char *name,
                                         const tb_shader_hlsl *hlsl, const char *const *files,
                                         size_t file_count, tb_arena *arena, tb_array *diagnostics)
{
    size_t errors = 0;
    for (size_t begin = 0; begin < length;)
    {
        size_t end = tb_str_find_char(messages, begin, length, '\n');
        end = end == TB_STR_NOT_FOUND ? length : end;
        const size_t next = end + 1;
        message read = {0};
        if (!read_message(messages, begin, end, &read))
        {
            begin = next;
            continue;
        }
        tb_shader_diagnostic diagnostic = {0};
        diagnostic.severity = read.severity;
        diagnostic.message = copy_range(arena, messages, read.text_begin, read.text_end);
        diagnostic.path = "";
        const char *const path = copy_range(arena, messages, read.path_begin, read.path_end);
        const bool in_text = read.line > 0 && names_text(path, name);
        if (in_text && read.line <= hlsl->line_count)
        {
            const tb_shader_location location = hlsl->lines[read.line - 1];
            if (location.line > 0 && location.file < file_count)
            {
                diagnostic.path = files[location.file];
                diagnostic.line = location.line;
                diagnostic.column = location.column;
            }
        }
        else if (!in_text)
        {
            diagnostic.path = path;
            diagnostic.line = read.line;
            diagnostic.column = read.column;
        }
        (void)tb_array_push(diagnostics, &diagnostic);
        errors += diagnostic.severity == TB_SHADER_ERROR ? 1u : 0u;
        begin = next;
    }
    return errors;
}

/* -------------------------------------------------------------------------
 * The shader for D3D11: the slots, then the DXBC.
 * ------------------------------------------------------------------------- */

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/* Where the DXBC starts after `slot_count` slots: a multiple of 4. */
static size_t code_offset(uint32_t slot_count)
{
    return TB_SHADER_DXBC_HEADER_BYTES + (size_t)slot_count * TB_SHADER_DXBC_SLOT_BYTES;
}

static void *wrap(const tb_shader_hlsl *hlsl, const void *code, size_t code_size, tb_arena *arena,
                  size_t *size)
{
    const size_t start = code_offset(hlsl->slot_count);
    uint8_t *const bytes = tb_arena_calloc(arena, start + code_size, 1, sizeof(uint32_t));
    put_u32(bytes, TB_SHADER_DXBC_MAGIC);
    put_u32(bytes + 4, TB_SHADER_DXBC_VERSION);
    put_u32(bytes + 8, hlsl->slot_count);
    for (size_t index = 0; index < TB_SHADER_DXBC_CONSTANT_BUFFERS; index++)
    {
        put_u32(bytes + 12 + 4 * index, hlsl->constant_bytes[index]);
    }
    put_u32(bytes + 24, (uint32_t)start);
    put_u32(bytes + 28, (uint32_t)code_size);
    for (uint32_t index = 0; index < hlsl->slot_count; index++)
    {
        const tb_shader_dxbc_slot *const slot = &hlsl->slots[index];
        uint8_t *const at = bytes + TB_SHADER_DXBC_HEADER_BYTES + index * TB_SHADER_DXBC_SLOT_BYTES;
        at[0] = slot->heap;
        at[1] = (uint8_t)slot->register_class;
        at[2] = slot->view;
        at[3] = slot->source;
        put_u32(at + 4, slot->offset);
        put_u32(at + 8, slot->register_index);
    }
    memcpy(bytes + start, code, code_size);
    *size = start + code_size;
    return bytes;
}

bool tb_shader_dxbc_read(const void *bytes, size_t size, tb_shader_dxbc_shader *shader)
{
    const uint8_t *const at = bytes;
    const tb_shader_dxbc_shader empty = {0};
    *shader = empty;
    if (bytes == NULL || size < TB_SHADER_DXBC_HEADER_BYTES ||
        get_u32(at) != TB_SHADER_DXBC_MAGIC || get_u32(at + 4) != TB_SHADER_DXBC_VERSION)
    {
        return false;
    }
    const uint32_t slot_count = get_u32(at + 8);
    const uint32_t start = get_u32(at + 24);
    const uint32_t code_size = get_u32(at + 28);
    if (slot_count > TB_SHADER_DXBC_MAX_SLOTS || start != code_offset(slot_count) || start > size ||
        code_size == 0 || code_size != size - start)
    {
        return false;
    }
    for (size_t index = 0; index < TB_SHADER_DXBC_CONSTANT_BUFFERS; index++)
    {
        shader->constant_bytes[index] = get_u32(at + 12 + 4 * index);
    }
    for (uint32_t index = 0; index < slot_count; index++)
    {
        const uint8_t *const entry =
            at + TB_SHADER_DXBC_HEADER_BYTES + index * TB_SHADER_DXBC_SLOT_BYTES;
        tb_shader_dxbc_slot *const slot = &shader->slots[index];
        slot->heap = entry[0];
        slot->register_class = (char)entry[1];
        slot->view = entry[2];
        slot->source = entry[3];
        slot->offset = get_u32(entry + 4);
        slot->register_index = get_u32(entry + 8);
        const bool known_class = slot->register_class == 't' || slot->register_class == 'u' ||
                                 slot->register_class == 's';
        if (slot->heap > TB_SHADER_DXBC_HEAP_SAMPLER || !known_class ||
            slot->view >= TB_SHADER_DXBC_VIEW_COUNT ||
            slot->source >= TB_SHADER_DXBC_CONSTANT_BUFFERS)
        {
            return false;
        }
    }
    shader->slot_count = slot_count;
    shader->code = at + start;
    shader->code_size = code_size;
    return true;
}

/* -------------------------------------------------------------------------
 * Compiling.
 * ------------------------------------------------------------------------- */

bool tb_shader_write_dxbc(const tb_shader_hlsl *hlsl, const tb_shader_compile_request *request,
                          const char *const *files, size_t file_count,
                          const tb_shader_d3dcompiler_functions *compiler, tb_arena *arena,
                          tb_array *diagnostics, tb_shader_dxbc *dxbc)
{
    const tb_allocator allocator = tb_arena_allocator(arena);
    char profile[TB_SHADER_DXBC_PROFILE_CAPACITY] = {0};
    const tb_shader_dxbc empty = {0};
    *dxbc = empty;
    (void)tb_shader_dxbc_profile(request->stage, profile);
    tb_d3dcompiler_request compile = {0};
    compile.source = hlsl->text;
    compile.source_length = hlsl->length;
    /* The main path without its first `/`, as DXC's is named. */
    compile.name = request->source.path + 1;
    compile.profile = profile;
    compile.entry_point = request->entry_point;
    compile.flags = (request->flags & TB_SHADER_COMPILE_DEBUG) != 0 ? TB_D3DCOMPILER_DEBUG : 0u;
    tb_d3dcompiler_output output = {0};
    tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
    const tb_d3dcompiler_result result =
        compiler->compile(request->d3dcompiler, &compile, &output, &error);
    dxbc->error_count =
        tb_shader_d3dcompiler_diagnostics(output.messages, output.messages_length, compile.name,
                                          hlsl, files, file_count, arena, diagnostics);
    if (result == TB_D3DCOMPILER_OK)
    {
        dxbc->code = wrap(hlsl, output.code, output.code_size, arena, &dxbc->code_size);
    }
    else if (dxbc->error_count == 0)
    {
        /* A failure must say why, though the compiler's messages did not. */
        tb_shader_diagnostic diagnostic = {0};
        diagnostic.severity = TB_SHADER_ERROR;
        diagnostic.path = "";
        diagnostic.message = tb_fmt_string(&allocator, "d3dcompiler: %s", error.message);
        (void)tb_array_push(diagnostics, &diagnostic);
        dxbc->error_count = 1;
    }
    compiler->release(&output);
    return result == TB_D3DCOMPILER_OK;
}
