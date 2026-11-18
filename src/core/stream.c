#include "badger/assert.h"
#include "stream_internal.h"

/* -------------------------------------------------------------------------
 * Shared by the implementations.
 * ------------------------------------------------------------------------- */

tb_stream_result tb_stream_resolve_seek(uint64_t current, uint64_t size, int64_t offset,
                                        tb_stream_origin origin, uint64_t *position)
{
    uint64_t base = {0};
    uint64_t target = {0};
    TB_ASSERT(position != NULL);
    switch (origin)
    {
    case TB_STREAM_FROM_START:
        base = 0;
        break;
    case TB_STREAM_FROM_CURRENT:
        base = current;
        break;
    case TB_STREAM_FROM_END:
        base = size;
        break;
    default:
        return TB_STREAM_INVALID;
    }
    if (offset < 0)
    {
        /* -(offset + 1) + 1 is the magnitude without overflowing at INT64_MIN. */
        const uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
        if (magnitude > base)
        {
            return TB_STREAM_INVALID;
        }
        target = base - magnitude;
    }
    else
    {
        if ((uint64_t)offset > UINT64_MAX - base)
        {
            return TB_STREAM_TOO_LARGE;
        }
        target = base + (uint64_t)offset;
    }
    if (target > (uint64_t)INT64_MAX)
    {
        return TB_STREAM_TOO_LARGE;
    }
    *position = target;
    return TB_STREAM_OK;
}

size_t tb_stream_available(uint64_t size, uint64_t offset, size_t capacity)
{
    uint64_t left = {0};
    if (offset >= size)
    {
        return 0;
    }
    left = size - offset;
    if (left < (uint64_t)capacity)
    {
        return (size_t)left;
    }
    return capacity;
}

/* -------------------------------------------------------------------------
 * Operations on any stream. Each checks the caller's contract and the
 * implementation's, so a broken implementation is found where it breaks.
 * ------------------------------------------------------------------------- */

static void tb_stream_check_open(const tb_stream *stream)
{
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions != NULL, "the stream is not open");
}

tb_stream_result tb_stream_read(tb_stream *stream, void *bytes, size_t capacity,
                                size_t *transferred)
{
    tb_stream_result result = {0};
    tb_stream_check_open(stream);
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || capacity == 0);
    *transferred = 0;
    if (stream->functions->read_bytes == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    if (capacity == 0)
    {
        return TB_STREAM_OK;
    }
    result = stream->functions->read_bytes(stream->state, bytes, capacity, transferred);
    TB_ASSERT_MSG(*transferred <= capacity, "a stream read more than it was asked for");
    return result;
}

tb_stream_result tb_stream_write(tb_stream *stream, const void *bytes, size_t length,
                                 size_t *transferred)
{
    tb_stream_result result = {0};
    tb_stream_check_open(stream);
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    *transferred = 0;
    if (stream->functions->write_bytes == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    if (length == 0)
    {
        return TB_STREAM_OK;
    }
    result = stream->functions->write_bytes(stream->state, bytes, length, transferred);
    TB_ASSERT_MSG(*transferred <= length, "a stream wrote more than it was given");
    TB_ASSERT_MSG(result != TB_STREAM_OK || *transferred > 0,
                  "a stream wrote nothing and reported no failure");
    return result;
}

tb_stream_result tb_stream_seek(tb_stream *stream, int64_t offset, tb_stream_origin origin,
                                uint64_t *position)
{
    tb_stream_result result = {0};
    uint64_t arrived = {0};
    tb_stream_check_open(stream);
    if (stream->functions->seek == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    result = stream->functions->seek(stream->state, offset, origin, &arrived);
    if (result == TB_STREAM_OK && position != NULL)
    {
        *position = arrived;
    }
    return result;
}

tb_stream_result tb_stream_tell(tb_stream *stream, uint64_t *position)
{
    TB_ASSERT(position != NULL);
    return tb_stream_seek(stream, 0, TB_STREAM_FROM_CURRENT, position);
}

tb_stream_result tb_stream_size(tb_stream *stream, uint64_t *size)
{
    tb_stream_check_open(stream);
    TB_ASSERT(size != NULL);
    if (stream->functions->size == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    return stream->functions->size(stream->state, size);
}

tb_stream_result tb_stream_read_at(tb_stream *stream, uint64_t offset, void *bytes, size_t capacity,
                                   size_t *transferred)
{
    tb_stream_result result = {0};
    tb_stream_check_open(stream);
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || capacity == 0);
    *transferred = 0;
    if (stream->functions->read_bytes_at == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    if (capacity == 0)
    {
        return TB_STREAM_OK;
    }
    result = stream->functions->read_bytes_at(stream->state, offset, bytes, capacity, transferred);
    TB_ASSERT_MSG(*transferred <= capacity, "a stream read more than it was asked for");
    return result;
}

tb_stream_result tb_stream_flush(tb_stream *stream)
{
    tb_stream_check_open(stream);
    if (stream->functions->flush == NULL)
    {
        return TB_STREAM_OK;
    }
    return stream->functions->flush(stream->state);
}

tb_stream_result tb_stream_close(tb_stream *stream)
{
    tb_stream_release_function *release = NULL;
    void *state = NULL;
    TB_ASSERT(stream != NULL);
    if (stream->functions == NULL)
    {
        return TB_STREAM_OK;
    }
    TB_ASSERT_MSG(stream->functions->release != NULL, "a stream has no release function");
    release = stream->functions->release;
    state = stream->state;
    *stream = (tb_stream){0};
    return release(state);
}

tb_stream_result tb_stream_read_exact(tb_stream *stream, void *bytes, size_t length)
{
    unsigned char *const output = bytes;
    size_t done = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    while (done < length)
    {
        size_t count = {0};
        const tb_stream_result result =
            tb_stream_read(stream, output + done, length - done, &count);
        if (result != TB_STREAM_OK)
        {
            return result;
        }
        if (count == 0)
        {
            return TB_STREAM_END;
        }
        done += count;
    }
    return TB_STREAM_OK;
}

tb_stream_result tb_stream_read_exact_at(tb_stream *stream, uint64_t offset, void *bytes,
                                         size_t length)
{
    unsigned char *const output = bytes;
    size_t done = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    if ((uint64_t)length > UINT64_MAX - offset)
    {
        return TB_STREAM_TOO_LARGE;
    }
    while (done < length)
    {
        size_t count = {0};
        const tb_stream_result result =
            tb_stream_read_at(stream, offset + done, output + done, length - done, &count);
        if (result != TB_STREAM_OK)
        {
            return result;
        }
        if (count == 0)
        {
            return TB_STREAM_END;
        }
        done += count;
    }
    return TB_STREAM_OK;
}

tb_stream_result tb_stream_write_all(tb_stream *stream, const void *bytes, size_t length)
{
    const unsigned char *const input = bytes;
    size_t done = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    while (done < length)
    {
        size_t count = {0};
        const tb_stream_result result =
            tb_stream_write(stream, input + done, length - done, &count);
        if (result != TB_STREAM_OK)
        {
            return result;
        }
        done += count;
    }
    return TB_STREAM_OK;
}

tb_stream_result tb_stream_copy(tb_stream *source, tb_stream *destination, void *buffer,
                                size_t capacity, uint64_t limit, uint64_t *copied)
{
    uint64_t total = {0};
    tb_stream_result result = {0};
    tb_stream_check_open(source);
    tb_stream_check_open(destination);
    TB_ASSERT(buffer != NULL);
    TB_ASSERT(capacity > 0);
    TB_ASSERT_MSG(source->state != destination->state ||
                      source->functions != destination->functions,
                  "a stream cannot be copied into itself");
    for (;;)
    {
        size_t wanted = capacity;
        size_t count = {0};
        const uint64_t room = limit - total;
        /* At the limit, one more byte tells a source that ends from one that would exceed it. */
        if (room == 0)
        {
            wanted = 1;
        }
        else if (room < (uint64_t)wanted)
        {
            wanted = (size_t)room;
        }
        result = tb_stream_read(source, buffer, wanted, &count);
        if (result != TB_STREAM_OK || count == 0)
        {
            break;
        }
        if (room == 0)
        {
            result = TB_STREAM_TOO_LARGE;
            break;
        }
        result = tb_stream_write_all(destination, buffer, count);
        if (result != TB_STREAM_OK)
        {
            break;
        }
        total += count;
    }
    if (copied != NULL)
    {
        *copied = total;
    }
    return result;
}

const char *tb_stream_result_name(tb_stream_result result)
{
    switch (result)
    {
    case TB_STREAM_OK:
        return "ok";
    case TB_STREAM_INVALID:
        return "invalid argument";
    case TB_STREAM_NO_MEMORY:
        return "out of memory";
    case TB_STREAM_TOO_LARGE:
        return "too large";
    case TB_STREAM_UNSUPPORTED:
        return "not supported";
    case TB_STREAM_END:
        return "unexpected end";
    case TB_STREAM_NO_SPACE:
        return "no space left";
    case TB_STREAM_MALFORMED:
        return "malformed data";
    case TB_STREAM_ACCESS:
        return "access denied";
    case TB_STREAM_OS_ERROR:
        return "operating system error";
    default:
        return "unknown result";
    }
}

/* -------------------------------------------------------------------------
 * A range: a window of another stream, read through its reads at an offset.
 * ------------------------------------------------------------------------- */

typedef struct tb_stream_range
{
    tb_stream parent;
    uint64_t offset;
    uint64_t length;
    uint64_t position;
    tb_allocator allocator;
} tb_stream_range;

static tb_stream_result tb_stream_range_read_at(void *state, uint64_t offset, void *bytes,
                                                size_t capacity, size_t *transferred)
{
    tb_stream_range *const range = state;
    const size_t wanted = tb_stream_available(range->length, offset, capacity);
    if (wanted == 0)
    {
        return TB_STREAM_OK;
    }
    /* offset < length, and offset + length fits below INT64_MAX by construction. */
    return tb_stream_read_at(&range->parent, range->offset + offset, bytes, wanted, transferred);
}

static tb_stream_result tb_stream_range_read(void *state, void *bytes, size_t capacity,
                                             size_t *transferred)
{
    tb_stream_range *const range = state;
    const tb_stream_result result =
        tb_stream_range_read_at(state, range->position, bytes, capacity, transferred);
    range->position += *transferred;
    return result;
}

static tb_stream_result tb_stream_range_seek(void *state, int64_t offset, tb_stream_origin origin,
                                             uint64_t *position)
{
    tb_stream_range *const range = state;
    const tb_stream_result result =
        tb_stream_resolve_seek(range->position, range->length, offset, origin, &range->position);
    *position = range->position;
    return result;
}

static tb_stream_result tb_stream_range_size(void *state, uint64_t *size)
{
    const tb_stream_range *const range = state;
    *size = range->length;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_range_close(void *state)
{
    tb_stream_range *const range = state;
    const tb_allocator allocator = range->allocator;
    TB_ALLOCATOR_DELETE(&allocator, tb_stream_range, range);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_stream_range_functions = {
    .read_bytes = tb_stream_range_read,
    .seek = tb_stream_range_seek,
    .size = tb_stream_range_size,
    .read_bytes_at = tb_stream_range_read_at,
    .release = tb_stream_range_close,
};

tb_stream_result tb_stream_open_range(tb_stream *parent, uint64_t offset, uint64_t length,
                                      const tb_allocator *allocator, tb_stream *stream)
{
    tb_allocator resolved = {0};
    tb_stream_range *range = NULL;
    tb_stream_check_open(parent);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    if (parent->functions->read_bytes_at == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    if (offset > (uint64_t)INT64_MAX || length > (uint64_t)INT64_MAX - offset)
    {
        return TB_STREAM_TOO_LARGE;
    }
    resolved = tb_allocator_resolve(allocator);
    range = TB_ALLOCATOR_NEW(&resolved, tb_stream_range);
    *range = (tb_stream_range){0};
    range->parent = *parent;
    range->offset = offset;
    range->length = length;
    range->allocator = resolved;
    stream->functions = &tb_stream_range_functions;
    stream->state = range;
    return TB_STREAM_OK;
}
