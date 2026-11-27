/* Sprite descriptions and exported Aseprite/TexturePacker JSON. Mutations are
 * append-only until the last field succeeds, so a failed import rolls back. */
#include "sprite_internal.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/vfs.h"

#include <float.h>

#define SPRITE_FILE_BYTES ((size_t)16 << 20)
#define SPRITE_FILE_FRAMES 65536u
#define SPRITE_FILE_STEPS ((size_t)1 << 20)
#define SPRITE_DEFAULT_MS 100u

typedef struct sprite_reader
{
    tb_sprite_sheet *sheet;
    tb_arena *arena;
    const char *text;
    const char *name;
    tb_sprite_error *error;
    char path[TB_SPRITE_ERROR_CAPACITY];
    bool native;
    size_t steps_read;
} sprite_reader;

static const tb_json_value *member(const tb_json_value *object, const char *key)
{
    return object != NULL ? tb_json_find(object, key, tb_str_length(key)) : NULL;
}

static bool is_type(const tb_json_value *value, tb_json_type type)
{
    return value != NULL && tb_json_type_of(value) == type;
}

static const char *string_value(const tb_json_value *value)
{
    if (!is_type(value, TB_JSON_STRING))
    {
        return NULL;
    }
    size_t length = 0;
    const char *const text = tb_json_as_text(value, &length);
    return tb_str_length(text) == length ? text : NULL;
}

static bool unsigned_field(const tb_json_value *object, const char *key, uint32_t *out,
                           bool optional)
{
    const tb_json_value *const value = member(object, key);
    if (value == NULL)
    {
        return optional;
    }
    uint64_t num = 0;
    if (!tb_json_as_uint64(value, &num) || num > UINT32_MAX)
    {
        return false;
    }
    *out = (uint32_t)num;
    return true;
}

static bool coordinate(const tb_json_value *object, const char *key, int32_t *out)
{
    uint32_t n = 0;
    if (!unsigned_field(object, key, &n, false) || n > INT32_MAX)
    {
        return false;
    }
    *out = (int32_t)n;
    return true;
}

static bool float_field(const tb_json_value *object, const char *key, double *out)
{
    const tb_json_value *const value = member(object, key);
    return value != NULL && tb_json_as_double(value, out);
}

static void describe(tb_sprite_error *error, const char *name)
{
    if (error->line != 0)
    {
        (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s%s%zu:%zu: %s: %s",
                            name != NULL ? name : "", name != NULL ? ":" : "", error->line,
                            error->column, error->path, error->reason);
    }
    else
    {
        (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s%s%s",
                            name != NULL ? name : "", name != NULL ? ": " : "", error->reason);
    }
}

static tb_sprite_result fail(sprite_reader *reader, const tb_json_value *where, const char *field,
                             tb_sprite_result result, const char *reason)
{
    const size_t offset = tb_json_offset_of(where);
    tb_sprite_error *const e = reader->error;
    *e = (tb_sprite_error){.result = result, .reason = reason, .line = 1, .column = 1};
    (void)tb_fmt_buffer(e->path, sizeof(e->path), "%s%s%s", reader->path, field != NULL ? "." : "",
                        field != NULL ? field : "");
    for (size_t i = 0; i < offset; i++)
    {
        if (reader->text[i] == '\n')
        {
            e->line++;
            e->column = 1;
        }
        else
        {
            e->column++;
        }
    }
    describe(e, reader->name);
    return result;
}

static tb_sprite_result read_frame(sprite_reader *r, const tb_json_value *object, const char *name,
                                   uint32_t *duration)
{
    tb_sprite_frame frame = {.name = name};
    const tb_json_value *const box = member(object, "frame");
    const tb_json_value *const source = member(object, "sourceSize");
    const tb_json_value *const trimmed = member(object, "spriteSourceSize");
    const tb_json_value *const pivot = member(object, "pivot");
    const tb_json_value *const rotated = member(object, "rotated");
    bool turns = false;
    if (rotated != NULL && (!tb_json_as_bool(rotated, &turns) || turns))
    {
        return fail(r, rotated, "rotated", TB_SPRITE_UNSUPPORTED, "rotated frames cannot be drawn");
    }
    if (!coordinate(box, "x", &frame.rect.x) || !coordinate(box, "y", &frame.rect.y) ||
        !unsigned_field(box, "w", &frame.rect.width, false) ||
        !unsigned_field(box, "h", &frame.rect.height, false))
    {
        return fail(r, box != NULL ? box : object, "frame", TB_SPRITE_FILE_INVALID,
                    "expected a pixel rectangle");
    }
    frame.source_width = frame.rect.width;
    frame.source_height = frame.rect.height;
    if (source != NULL && (!unsigned_field(source, "w", &frame.source_width, false) ||
                           !unsigned_field(source, "h", &frame.source_height, false) ||
                           !frame.source_width || !frame.source_height))
    {
        return fail(r, source, "sourceSize", TB_SPRITE_FILE_INVALID, "expected the untrimmed size");
    }
    if (trimmed != NULL)
    {
        if (!coordinate(trimmed, "x", &frame.offset_x) ||
            !coordinate(trimmed, "y", &frame.offset_y))
        {
            return fail(r, trimmed, "spriteSourceSize", TB_SPRITE_FILE_INVALID,
                        "expected a trim offset");
        }
    }
    if (pivot != NULL)
    {
        double x = 0, y = 0;
        if (!float_field(pivot, "x", &x) || !float_field(pivot, "y", &y))
        {
            return fail(r, pivot, "pivot", TB_SPRITE_FILE_INVALID,
                        "expected two pivot coordinates");
        }
        const double sx = r->native || frame.source_width == 0 ? 1.0 : (double)frame.source_width;
        const double sy = r->native || frame.source_height == 0 ? 1.0 : (double)frame.source_height;
        if (x > (double)FLT_MAX / sx || x < -(double)FLT_MAX / sx || y > (double)FLT_MAX / sy ||
            y < -(double)FLT_MAX / sy)
        {
            return fail(r, pivot, "pivot", TB_SPRITE_FILE_INVALID,
                        "pivot exceeds the coordinate range");
        }
        frame.pivot_x = (float)(x * sx);
        frame.pivot_y = (float)(y * sy);
    }
    if (!unsigned_field(object, "duration", duration, true) || *duration == 0)
    {
        return fail(r, object, "duration", TB_SPRITE_FILE_INVALID, "expected a positive duration");
    }
    const tb_sprite_result added = tb_sprite_sheet_add_frame(r->sheet, &frame, NULL);
    if (added != TB_SPRITE_OK)
    {
        return fail(r, object, NULL, TB_SPRITE_FILE_INVALID, tb_sprite_result_name(added));
    }
    return TB_SPRITE_OK;
}

static tb_sprite_result read_grid(sprite_reader *r, const tb_json_value *object)
{
    tb_sprite_grid grid = {.name = string_value(member(object, "name"))};
    if (!unsigned_field(object, "cellWidth", &grid.cell_width, false) ||
        !unsigned_field(object, "cellHeight", &grid.cell_height, false) ||
        !unsigned_field(object, "imageWidth", &grid.image_width, false) ||
        !unsigned_field(object, "imageHeight", &grid.image_height, false) ||
        !unsigned_field(object, "spacingX", &grid.spacing_x, true) ||
        !unsigned_field(object, "spacingY", &grid.spacing_y, true) ||
        !unsigned_field(object, "marginX", &grid.margin_x, true) ||
        !unsigned_field(object, "marginY", &grid.margin_y, true) ||
        !unsigned_field(object, "count", &grid.count, true) || !grid.cell_width ||
        !grid.cell_height)
    {
        return fail(r, object, "grid", TB_SPRITE_FILE_INVALID, "invalid grid dimensions");
    }
    const uint64_t width = grid.image_width >= grid.margin_x ? grid.image_width - grid.margin_x : 0;
    const uint64_t height =
        grid.image_height >= grid.margin_y ? grid.image_height - grid.margin_y : 0;
    const uint64_t cols = (width + grid.spacing_x) / ((uint64_t)grid.cell_width + grid.spacing_x);
    const uint64_t rows = (height + grid.spacing_y) / ((uint64_t)grid.cell_height + grid.spacing_y);
    const uint64_t count = grid.count != 0 ? grid.count : cols * rows;
    if (count > SPRITE_FILE_FRAMES)
    {
        return fail(r, object, "grid", TB_SPRITE_TOO_LARGE, "too many grid frames");
    }
    /* JSON names each grid once, but the sheet owns one name per cell. */
    if (grid.name != NULL && count != 0 && tb_str_length(grid.name) > SPRITE_FILE_BYTES / count)
    {
        return fail(r, object, "grid", TB_SPRITE_TOO_LARGE, "expanded grid names exceed 16 MiB");
    }
    const tb_sprite_result added = tb_sprite_sheet_add_grid(r->sheet, &grid, NULL, NULL);
    return added == TB_SPRITE_OK
               ? added
               : fail(r, object, "grid", TB_SPRITE_FILE_INVALID, tb_sprite_result_name(added));
}

static tb_sprite_result animation(sprite_reader *r, const tb_json_value *object, const char *name,
                                  const tb_sprite_step *steps, size_t count, tb_sprite_loop loop)
{
    if (name == NULL)
    {
        return fail(r, object, "name", TB_SPRITE_FILE_INVALID, "expected an animation name");
    }
    const tb_sprite_result added =
        tb_sprite_sheet_add_animation(r->sheet, name, steps, count, loop, NULL);
    return added == TB_SPRITE_OK
               ? added
               : fail(r, object, NULL, TB_SPRITE_FILE_INVALID, tb_sprite_result_name(added));
}

static tb_sprite_result read_animation(sprite_reader *r, const tb_json_value *object)
{
    const tb_json_value *const list = member(object, "steps");
    if (!is_type(list, TB_JSON_ARRAY) || tb_json_count(list) == 0)
    {
        return fail(r, object, "steps", TB_SPRITE_FILE_INVALID,
                    "expected one or more animation steps");
    }
    const size_t count = tb_json_count(list);
    if (count > SPRITE_FILE_STEPS - r->steps_read)
    {
        return fail(r, object, "steps", TB_SPRITE_TOO_LARGE, "too many animation steps");
    }
    r->steps_read += count;
    tb_sprite_step *const steps = TB_ARENA_NEW_ARRAY(r->arena, tb_sprite_step, count);
    for (size_t i = 0; i < count; i++)
    {
        const tb_json_value *const value = tb_json_at(list, i);
        const char *const name = string_value(member(value, "frame"));
        steps[i] = (tb_sprite_step){
            .frame = name != NULL ? tb_sprite_sheet_find_frame(r->sheet, name) : TB_SPRITE_NONE};
        if (steps[i].frame == TB_SPRITE_NONE ||
            !unsigned_field(value, "duration", &steps[i].duration_ms, false) ||
            steps[i].duration_ms == 0)
        {
            return fail(r, value, "steps", TB_SPRITE_FILE_INVALID,
                        "unknown frame or invalid duration");
        }
    }
    tb_sprite_loop loop = TB_SPRITE_LOOP;
    const tb_json_value *const mode = member(object, "loop");
    if (mode != NULL)
    {
        const char *const text = string_value(mode);
        if (text != NULL && tb_str_equal(text, "once"))
        {
            loop = TB_SPRITE_ONCE;
        }
        else if (text != NULL && tb_str_equal(text, "ping-pong"))
        {
            loop = TB_SPRITE_PING_PONG;
        }
        else if (text == NULL || !tb_str_equal(text, "loop"))
        {
            return fail(r, mode, "loop", TB_SPRITE_FILE_INVALID, "unknown animation loop");
        }
    }
    return animation(r, object, string_value(member(object, "name")), steps, count, loop);
}

static tb_sprite_result read_tag(sprite_reader *r, const tb_json_value *tag,
                                 const tb_sprite_step *frames, size_t count)
{
    uint32_t from = 0, to = 0;
    if (!unsigned_field(tag, "from", &from, false) || !unsigned_field(tag, "to", &to, false) ||
        from > to || to >= count)
    {
        return fail(r, tag, NULL, TB_SPRITE_FILE_INVALID, "frame range outside this export");
    }
    bool backwards = false;
    tb_sprite_loop loop = TB_SPRITE_LOOP;
    const tb_json_value *const dir = member(tag, "direction");
    if (dir != NULL)
    {
        const char *const text = string_value(dir);
        if (text == NULL)
        {
            return fail(r, dir, "direction", TB_SPRITE_FILE_INVALID,
                        "expected a playback direction");
        }
        backwards = tb_str_equal(text, "reverse") || tb_str_equal(text, "pingpong_reverse");
        if (tb_str_equal(text, "pingpong") || tb_str_equal(text, "pingpong_reverse"))
        {
            loop = TB_SPRITE_PING_PONG;
        }
        else if (!backwards && !tb_str_equal(text, "forward"))
        {
            return fail(r, dir, "direction", TB_SPRITE_UNSUPPORTED, "unknown playback direction");
        }
    }
    const size_t length = (size_t)to - from + 1u;
    if (length > SPRITE_FILE_STEPS - r->steps_read)
    {
        return fail(r, tag, NULL, TB_SPRITE_TOO_LARGE, "too many animation steps");
    }
    r->steps_read += length;
    const tb_sprite_step *steps = frames + from;
    if (backwards)
    {
        tb_sprite_step *const reverse = TB_ARENA_NEW_ARRAY(r->arena, tb_sprite_step, length);
        for (size_t i = 0; i < length; i++)
        {
            reverse[i] = frames[to - i];
        }
        steps = reverse;
    }
    return animation(r, tag, string_value(member(tag, "name")), steps, length, loop);
}

static tb_sprite_result read_document(sprite_reader *r, const tb_json_value *root)
{
    const tb_json_value *const format = member(root, "format");
    const tb_json_value *const frames = member(root, "frames");
    const tb_json_value *const meta = member(root, "meta");
    const tb_json_value *const grid = member(root, "grid");
    r->native = format != NULL;
    if (r->native)
    {
        const char *const name = string_value(format);
        uint32_t version = 0;
        if (name == NULL || !tb_str_equal(name, TB_SPRITE_FILE_FORMAT) ||
            !unsigned_field(root, "version", &version, false) || version != TB_SPRITE_FILE_VERSION)
        {
            return fail(r, root, NULL, TB_SPRITE_UNSUPPORTED,
                        "unknown sprite sheet format or version");
        }
    }
    if (grid != NULL && r->native)
    {
        const tb_sprite_result added = read_grid(r, grid);
        if (added != TB_SPRITE_OK)
        {
            return added;
        }
    }
    const bool hash = !r->native && is_type(frames, TB_JSON_OBJECT);
    if (!hash && !is_type(frames, TB_JSON_ARRAY) && !(r->native && grid != NULL && frames == NULL))
    {
        return fail(r, root, "frames", TB_SPRITE_FILE_INVALID, "expected a frame list");
    }
    const size_t count = frames != NULL ? tb_json_count(frames) : 0;
    if (count > SPRITE_FILE_FRAMES)
    {
        return fail(r, root, "frames", TB_SPRITE_TOO_LARGE, "too many frames");
    }
    tb_sprite_step *const sequence =
        count != 0 ? TB_ARENA_NEW_ARRAY(r->arena, tb_sprite_step, count) : NULL;
    for (size_t i = 0; i < count; i++)
    {
        const char *name = NULL;
        size_t key_length = 0;
        const tb_json_value *const object =
            hash ? tb_json_member_at(frames, i, &name, &key_length) : tb_json_at(frames, i);
        if (hash)
        {
            if (tb_str_length(name) != key_length)
            {
                name = NULL;
            }
        }
        else
        {
            name = string_value(member(object, r->native ? "name" : "filename"));
        }
        (void)tb_fmt_buffer(r->path, sizeof(r->path), "$.frames[%zu]", i);
        sequence[i] = (tb_sprite_step){.frame = tb_sprite_sheet_frame_count(r->sheet),
                                       .duration_ms = SPRITE_DEFAULT_MS};
        const tb_sprite_result added = read_frame(r, object, name, &sequence[i].duration_ms);
        if (added != TB_SPRITE_OK)
        {
            return added;
        }
    }
    const tb_json_value *const animations =
        r->native ? member(root, "animations") : member(meta, "frameTags");
    if (animations != NULL)
    {
        if (!is_type(animations, TB_JSON_ARRAY))
        {
            (void)tb_fmt_buffer(r->path, sizeof(r->path), "$");
            return fail(r, animations, r->native ? "animations" : "meta.frameTags",
                        TB_SPRITE_FILE_INVALID, "expected an animation list");
        }
        for (size_t i = 0; i < tb_json_count(animations); i++)
        {
            (void)tb_fmt_buffer(r->path, sizeof(r->path),
                                r->native ? "$.animations[%zu]" : "$.meta.frameTags[%zu]", i);
            const tb_json_value *const object = tb_json_at(animations, i);
            const tb_sprite_result added =
                r->native ? read_animation(r, object) : read_tag(r, object, sequence, count);
            if (added != TB_SPRITE_OK)
            {
                return added;
            }
        }
    }
    const tb_json_value *const image = member(r->native ? root : meta, "image");
    if (image != NULL)
    {
        const char *const name = string_value(image);
        if (name == NULL)
        {
            (void)tb_fmt_buffer(r->path, sizeof(r->path), "$");
            return fail(r, image, r->native ? "image" : "meta.image", TB_SPRITE_FILE_INVALID,
                        "expected an image path");
        }
        tb_sprite_sheet_set_image(r->sheet, name);
    }
    return TB_SPRITE_OK;
}

tb_sprite_result tb_sprite_sheet_load(tb_sprite_sheet *sheet, const char *text, size_t length,
                                      const char *name, tb_sprite_error *error)
{
    TB_ASSERT(sheet != NULL);
    TB_ASSERT(text != NULL || length == 0);
    tb_sprite_error ignored = {0};
    sprite_reader r = {.sheet = sheet,
                       .text = text,
                       .name = name,
                       .error = error != NULL ? error : &ignored,
                       .path = "$"};
    *r.error = (tb_sprite_error){.reason = ""};
    if (length > SPRITE_FILE_BYTES)
    {
        *r.error = (tb_sprite_error){.result = TB_SPRITE_TOO_LARGE,
                                     .reason = "sprite file exceeds 16 MiB"};
        describe(r.error, name);
        return TB_SPRITE_TOO_LARGE;
    }
    const size_t frames = tb_sprite_sheet_frame_count(sheet);
    const size_t animations = tb_sprite_sheet_animation_count(sheet);
    r.arena = tb_arena_create(0);
    const tb_json_value *root = NULL;
    tb_json_error parse = {0};
    const tb_json_options options = {.max_depth = 32};
    tb_sprite_result result = TB_SPRITE_OK;
    if (tb_json_parse(text, length, &options, r.arena, &root, &parse) != TB_JSON_OK)
    {
        *r.error = (tb_sprite_error){.result = TB_SPRITE_MALFORMED,
                                     .line = parse.line,
                                     .column = parse.column,
                                     .reason = parse.reason,
                                     .path = "$"};
        describe(r.error, name);
        result = TB_SPRITE_MALFORMED;
    }
    else
    {
        result = read_document(&r, root);
        if (result != TB_SPRITE_OK)
        {
            tb_sprite_sheet_truncate(sheet, frames, animations);
        }
    }
    tb_arena_destroy(r.arena);
    return result;
}

tb_sprite_result tb_sprite_sheet_load_file(tb_sprite_sheet *sheet, tb_vfs *vfs, const char *path,
                                           tb_sprite_error *error)
{
    TB_ASSERT(sheet != NULL && vfs != NULL && path != NULL);
    tb_sprite_error ignored = {0};
    tb_sprite_error *const e = error != NULL ? error : &ignored;
    tb_stream stream = {0};
    uint64_t length = 0;
    char *text = NULL;
    const tb_vfs_result opened = tb_vfs_open(vfs, path, tb_str_length(path), NULL, &stream);
    if (opened != TB_VFS_OK)
    {
        const tb_sprite_result result =
            opened == TB_VFS_NOT_FOUND || opened == TB_VFS_NOT_A_FILE || opened == TB_VFS_INVALID
                ? TB_SPRITE_NOT_FOUND
                : TB_SPRITE_INPUT_FAILED;
        *e = (tb_sprite_error){.result = result, .reason = tb_vfs_result_name(opened)};
        describe(e, path);
        return result;
    }
    tb_sprite_result result = TB_SPRITE_INPUT_FAILED;
    if (tb_stream_size(&stream, &length) != TB_STREAM_OK || length > SPRITE_FILE_BYTES)
    {
        *e = (tb_sprite_error){.result = result,
                               .reason = "could not read file size or file exceeds 16 MiB"};
        describe(e, path);
        goto done;
    }
    text = tb_malloc((size_t)length + 1u);
    if (tb_stream_read_exact(&stream, text, (size_t)length) != TB_STREAM_OK)
    {
        *e = (tb_sprite_error){.result = result, .reason = "could not read sprite file"};
        describe(e, path);
        goto done;
    }
    result = tb_sprite_sheet_load(sheet, text, (size_t)length, path, e);
done:
    tb_free(text);
    (void)tb_stream_close(&stream);
    return result;
}
