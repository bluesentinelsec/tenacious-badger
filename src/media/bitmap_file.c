/* Image files: recognizing a format by its bytes, reading a whole file from
 * a stream or a VFS, and the errors of every format. */
#include "bitmap_internal.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/vfs.h"

#include <string.h>

/* Bytes a stream is read in, at first: buffers grow from here. */
#define TB_BITMAP_READ_CHUNK ((size_t)64 << 10)

const char *tb_bitmap_result_name(tb_bitmap_result result)
{
    switch (result)
    {
    case TB_BITMAP_OK:
        return "ok";
    case TB_BITMAP_INVALID:
        return "invalid argument";
    case TB_BITMAP_NO_MEMORY:
        return "out of memory";
    case TB_BITMAP_TOO_LARGE:
        return "too large";
    case TB_BITMAP_UNKNOWN_FORMAT:
        return "unknown format";
    case TB_BITMAP_UNSUPPORTED:
        return "unsupported";
    case TB_BITMAP_MALFORMED:
        return "malformed";
    case TB_BITMAP_NOT_FOUND:
        return "not found";
    case TB_BITMAP_INPUT_FAILED:
        return "input failed";
    case TB_BITMAP_OUTPUT_FAILED:
        return "output failed";
    }
    return "unknown result";
}

tb_bitmap_result tb_bitmap_fail(tb_bitmap_error *error, tb_bitmap_result result, size_t offset,
                                const char *reason)
{
    *error = (tb_bitmap_error){.result = result, .offset = offset, .reason = reason};
    return result;
}

void tb_bitmap_describe(tb_bitmap_error *error, const char *name)
{
    size_t used = 0;
    const bool at_offset = error->offset != TB_BITMAP_NO_OFFSET;

    error->message[0] = '\0';
    if (!at_offset)
    {
        error->offset = 0;
    }
    if (error->result == TB_BITMAP_OK)
    {
        return;
    }
    if (name != NULL)
    {
        used += tb_fmt_buffer(error->message + used, sizeof(error->message) - used, "%s: ", name);
    }
    if (at_offset && used < sizeof(error->message))
    {
        used += tb_fmt_buffer(error->message + used, sizeof(error->message) - used,
                              "byte %zu: ", error->offset);
    }
    if (used < sizeof(error->message))
    {
        (void)tb_fmt_buffer(error->message + used, sizeof(error->message) - used, "%s",
                            error->reason);
    }
}

/* The limits of the options, or their defaults; false for options that are wrong. */
typedef struct tb_bitmap_limits
{
    uint32_t max_side;
    uint64_t max_pixels;
    uint64_t max_bytes;
    const char *name;
} tb_bitmap_limits;

static bool tb_bitmap_limits_of(const tb_bitmap_decode_options *options, tb_bitmap_limits *limits)
{
    const tb_bitmap_decode_options none = {0};
    const tb_bitmap_decode_options *const given = options != NULL ? options : &none;

    *limits = (tb_bitmap_limits){
        .max_side = given->max_side != 0 ? given->max_side : TB_BITMAP_DEFAULT_MAX_SIDE,
        .max_pixels = given->max_pixels != 0 ? given->max_pixels : TB_BITMAP_DEFAULT_MAX_PIXELS,
        .max_bytes = given->max_bytes != 0 ? given->max_bytes : TB_BITMAP_DEFAULT_MAX_BYTES,
        .name = given->name};
    return limits->max_side <= TB_BITMAP_MAX_DIMENSION && limits->max_bytes < SIZE_MAX;
}

static tb_bitmap_result tb_bitmap_decode_limited(const void *bytes, size_t length,
                                                 const tb_bitmap_limits *limits,
                                                 const tb_allocator *allocator, tb_bitmap **bitmap,
                                                 tb_bitmap_error *error)
{
    static const uint8_t bmp_signature[2] = {'B', 'M'};

    if (length >= sizeof(bmp_signature) && memcmp(bytes, bmp_signature, sizeof(bmp_signature)) == 0)
    {
        return tb_bitmap_decode_bmp(bytes, length, limits->max_side, limits->max_pixels, allocator,
                                    bitmap, error);
    }
    return tb_bitmap_fail(error, TB_BITMAP_UNKNOWN_FORMAT, TB_BITMAP_NO_OFFSET,
                          "bytes of no image format the engine reads");
}

/* Set up an error and a bitmap to report into; the error is `ignored` when
 * nobody asks. */
static tb_bitmap_error *tb_bitmap_begin(tb_bitmap **bitmap, tb_bitmap_error *error,
                                        tb_bitmap_error *ignored)
{
    TB_ASSERT(bitmap != NULL);
    *bitmap = NULL;
    tb_bitmap_error *const found = error != NULL ? error : ignored;
    *found = (tb_bitmap_error){.reason = "", .offset = TB_BITMAP_NO_OFFSET};
    return found;
}

tb_bitmap_result tb_bitmap_decode(const void *bytes, size_t length,
                                  const tb_bitmap_decode_options *options,
                                  const tb_allocator *allocator, tb_bitmap **bitmap,
                                  tb_bitmap_error *error)
{
    tb_bitmap_error ignored = {0};
    tb_bitmap_error *const found = tb_bitmap_begin(bitmap, error, &ignored);
    tb_bitmap_limits limits = {0};
    tb_bitmap_result result = TB_BITMAP_OK;

    TB_ASSERT(bytes != NULL || length == 0);
    if (!tb_bitmap_limits_of(options, &limits))
    {
        result = tb_bitmap_fail(found, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "limits over what a bitmap can have");
    }
    else
    {
        result = tb_bitmap_decode_limited(bytes, length, &limits, allocator, bitmap, found);
    }
    tb_bitmap_describe(found, limits.name);
    return result;
}

/* Read a stream from its position to its end, into memory of the heap. The
 * buffer grows to one byte past the limit at most: a stream that fills that
 * goes on past the limit. */
static tb_bitmap_result tb_bitmap_read_all(tb_stream *stream, uint64_t max_bytes, uint8_t **bytes,
                                           size_t *length, tb_bitmap_error *error)
{
    uint8_t *buffer = NULL;
    size_t capacity = 0;
    size_t used = 0;

    for (;;)
    {
        if (used == capacity)
        {
            if ((uint64_t)capacity > max_bytes)
            {
                tb_free(buffer);
                return tb_bitmap_fail(error, TB_BITMAP_TOO_LARGE, TB_BITMAP_NO_OFFSET,
                                      "a file longer than the limit");
            }
            /* One byte past the limit is room to see that the stream goes on. */
            const uint64_t wanted = capacity == 0 ? TB_BITMAP_READ_CHUNK : (uint64_t)capacity * 2u;
            const size_t grown = (size_t)(wanted < max_bytes + 1u ? wanted : max_bytes + 1u);
            uint8_t *const larger = tb_try_realloc(buffer, grown);
            /* coverage-off: the heap refuses a block of at most the limit only when it is exhausted
             */
            if (larger == NULL)
            {
                tb_free(buffer);
                return tb_bitmap_fail(error, TB_BITMAP_NO_MEMORY, TB_BITMAP_NO_OFFSET,
                                      "no memory for the file");
            }
            /* coverage-on */
            buffer = larger;
            capacity = grown;
        }
        size_t transferred = 0;
        const tb_stream_result read =
            tb_stream_read(stream, buffer + used, capacity - used, &transferred);
        used += transferred;
        if (read != TB_STREAM_OK)
        {
            tb_free(buffer);
            return tb_bitmap_fail(error, TB_BITMAP_INPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                  tb_stream_result_name(read));
        }
        if (transferred == 0)
        {
            break;
        }
    }
    *bytes = buffer;
    *length = used;
    return TB_BITMAP_OK;
}

static tb_bitmap_result tb_bitmap_read_limited(tb_stream *stream, const tb_bitmap_limits *limits,
                                               const tb_allocator *allocator, tb_bitmap **bitmap,
                                               tb_bitmap_error *error)
{
    uint8_t *bytes = NULL;
    size_t length = 0;
    tb_bitmap_result result = tb_bitmap_read_all(stream, limits->max_bytes, &bytes, &length, error);

    if (result == TB_BITMAP_OK)
    {
        result = tb_bitmap_decode_limited(bytes, length, limits, allocator, bitmap, error);
    }
    tb_free(bytes);
    return result;
}

tb_bitmap_result tb_bitmap_read(tb_stream *stream, const tb_bitmap_decode_options *options,
                                const tb_allocator *allocator, tb_bitmap **bitmap,
                                tb_bitmap_error *error)
{
    tb_bitmap_error ignored = {0};
    tb_bitmap_error *const found = tb_bitmap_begin(bitmap, error, &ignored);
    tb_bitmap_limits limits = {0};
    tb_bitmap_result result = TB_BITMAP_OK;

    TB_ASSERT(stream != NULL);
    if (!tb_bitmap_limits_of(options, &limits))
    {
        result = tb_bitmap_fail(found, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "limits over what a bitmap can have");
    }
    else
    {
        result = tb_bitmap_read_limited(stream, &limits, allocator, bitmap, found);
    }
    tb_bitmap_describe(found, limits.name);
    return result;
}

tb_bitmap_result tb_bitmap_load(struct tb_vfs *vfs, const char *path,
                                const tb_bitmap_decode_options *options,
                                const tb_allocator *allocator, tb_bitmap **bitmap,
                                tb_bitmap_error *error)
{
    tb_bitmap_error ignored = {0};
    tb_bitmap_error *const found = tb_bitmap_begin(bitmap, error, &ignored);
    tb_bitmap_limits limits = {0};
    tb_stream stream = {0};
    tb_bitmap_result result = TB_BITMAP_OK;

    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    if (!tb_bitmap_limits_of(options, &limits))
    {
        result = tb_bitmap_fail(found, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "limits over what a bitmap can have");
        goto done;
    }
    const tb_vfs_result opened = tb_vfs_open(vfs, path, tb_str_length(path), NULL, &stream);
    if (opened == TB_VFS_NOT_FOUND || opened == TB_VFS_NOT_A_FILE || opened == TB_VFS_INVALID)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_NOT_FOUND, TB_BITMAP_NO_OFFSET, "no such file");
        goto done;
    }
    if (opened != TB_VFS_OK)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_INPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                tb_vfs_result_name(opened));
        goto done;
    }
    result = tb_bitmap_read_limited(&stream, &limits, allocator, bitmap, found);
    (void)tb_stream_close(&stream);
done:
    tb_bitmap_describe(found, path);
    return result;
}
