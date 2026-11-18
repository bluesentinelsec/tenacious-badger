#include "badger/assert.h"
#include "badger/size.h"
#include "stream_internal.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Borrowed bytes, read only.
 * ------------------------------------------------------------------------- */

typedef struct tb_stream_memory
{
    const unsigned char *bytes;
    size_t length;
    uint64_t position;
    tb_allocator allocator;
} tb_stream_memory;

static tb_stream_result tb_stream_memory_read_at(void *state, uint64_t offset, void *bytes,
                                                 size_t capacity, size_t *transferred)
{
    const tb_stream_memory *const memory = state;
    const size_t count = tb_stream_available(memory->length, offset, capacity);
    if (count > 0)
    {
        memcpy(bytes, memory->bytes + (size_t)offset, count);
    }
    *transferred = count;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_memory_read(void *state, void *bytes, size_t capacity,
                                              size_t *transferred)
{
    tb_stream_memory *const memory = state;
    const tb_stream_result result =
        tb_stream_memory_read_at(state, memory->position, bytes, capacity, transferred);
    memory->position += *transferred;
    return result;
}

static tb_stream_result tb_stream_memory_seek(void *state, int64_t offset, tb_stream_origin origin,
                                              uint64_t *position)
{
    tb_stream_memory *const memory = state;
    const tb_stream_result result =
        tb_stream_resolve_seek(memory->position, memory->length, offset, origin, &memory->position);
    *position = memory->position;
    return result;
}

static tb_stream_result tb_stream_memory_size(void *state, uint64_t *size)
{
    const tb_stream_memory *const memory = state;
    *size = memory->length;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_memory_close(void *state)
{
    tb_stream_memory *const memory = state;
    const tb_allocator allocator = memory->allocator;
    TB_ALLOCATOR_DELETE(&allocator, tb_stream_memory, memory);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_stream_memory_functions = {
    .read_bytes = tb_stream_memory_read,
    .seek = tb_stream_memory_seek,
    .size = tb_stream_memory_size,
    .read_bytes_at = tb_stream_memory_read_at,
    .release = tb_stream_memory_close,
};

void tb_stream_open_memory(const void *bytes, size_t length, const tb_allocator *allocator,
                           tb_stream *stream)
{
    tb_allocator resolved = {0};
    tb_stream_memory *memory = NULL;
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    resolved = tb_allocator_resolve(allocator);
    memory = TB_ALLOCATOR_NEW(&resolved, tb_stream_memory);
    *memory = (tb_stream_memory){0};
    memory->bytes = bytes;
    memory->length = length;
    memory->allocator = resolved;
    stream->functions = &tb_stream_memory_functions;
    stream->state = memory;
}

/* -------------------------------------------------------------------------
 * Owned bytes that grow as they are written.
 * ------------------------------------------------------------------------- */

typedef struct tb_stream_buffer
{
    unsigned char *bytes;
    size_t length;
    size_t capacity;
    uint64_t position;
    tb_allocator allocator;
} tb_stream_buffer;

static tb_stream_result tb_stream_buffer_read_at(void *state, uint64_t offset, void *bytes,
                                                 size_t capacity, size_t *transferred)
{
    const tb_stream_buffer *const buffer = state;
    const size_t count = tb_stream_available(buffer->length, offset, capacity);
    if (count > 0)
    {
        memcpy(bytes, buffer->bytes + (size_t)offset, count);
    }
    *transferred = count;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_buffer_read(void *state, void *bytes, size_t capacity,
                                              size_t *transferred)
{
    tb_stream_buffer *const buffer = state;
    const tb_stream_result result =
        tb_stream_buffer_read_at(state, buffer->position, bytes, capacity, transferred);
    buffer->position += *transferred;
    return result;
}

/* Make room for `required` bytes, keeping the buffer as it is on failure. */
static tb_stream_result tb_stream_buffer_reserve(tb_stream_buffer *buffer, size_t required)
{
    size_t grown = {0};
    unsigned char *resized = NULL;
    if (required <= buffer->capacity)
    {
        return TB_STREAM_OK;
    }
    if (!tb_size_grow(buffer->capacity, required, 1, &grown))
    {
        return TB_STREAM_TOO_LARGE;
    }
    resized =
        tb_allocator_try_realloc(&buffer->allocator, buffer->bytes, buffer->capacity, grown, 1);
    if (resized == NULL)
    {
        return TB_STREAM_NO_MEMORY;
    }
    buffer->bytes = resized;
    buffer->capacity = grown;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_buffer_write(void *state, const void *bytes, size_t length,
                                               size_t *transferred)
{
    tb_stream_buffer *const buffer = state;
    size_t start = {0};
    size_t end = {0};
    tb_stream_result result = {0};
    if (buffer->position > (uint64_t)SIZE_MAX)
    {
        return TB_STREAM_TOO_LARGE;
    }
    start = (size_t)buffer->position;
    if (!tb_size_add(start, length, &end) || (uint64_t)end > (uint64_t)INT64_MAX)
    {
        return TB_STREAM_TOO_LARGE;
    }
    result = tb_stream_buffer_reserve(buffer, end);
    if (result != TB_STREAM_OK)
    {
        return result;
    }
    if (start > buffer->length)
    {
        memset(buffer->bytes + buffer->length, 0, start - buffer->length);
    }
    memcpy(buffer->bytes + start, bytes, length);
    if (end > buffer->length)
    {
        buffer->length = end;
    }
    buffer->position = end;
    *transferred = length;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_buffer_seek(void *state, int64_t offset, tb_stream_origin origin,
                                              uint64_t *position)
{
    tb_stream_buffer *const buffer = state;
    const tb_stream_result result =
        tb_stream_resolve_seek(buffer->position, buffer->length, offset, origin, &buffer->position);
    *position = buffer->position;
    return result;
}

static tb_stream_result tb_stream_buffer_size(void *state, uint64_t *size)
{
    const tb_stream_buffer *const buffer = state;
    *size = buffer->length;
    return TB_STREAM_OK;
}

static tb_stream_result tb_stream_buffer_close(void *state)
{
    tb_stream_buffer *const buffer = state;
    const tb_allocator allocator = buffer->allocator;
    if (buffer->bytes != NULL)
    {
        tb_allocator_free(&allocator, buffer->bytes, buffer->capacity, 1);
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_stream_buffer, buffer);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_stream_buffer_functions = {
    .read_bytes = tb_stream_buffer_read,
    .write_bytes = tb_stream_buffer_write,
    .seek = tb_stream_buffer_seek,
    .size = tb_stream_buffer_size,
    .read_bytes_at = tb_stream_buffer_read_at,
    .release = tb_stream_buffer_close,
};

void tb_stream_open_buffer(const tb_allocator *allocator, tb_stream *stream)
{
    tb_allocator resolved = {0};
    tb_stream_buffer *buffer = NULL;
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    resolved = tb_allocator_resolve(allocator);
    buffer = TB_ALLOCATOR_NEW(&resolved, tb_stream_buffer);
    *buffer = (tb_stream_buffer){0};
    buffer->allocator = resolved;
    stream->functions = &tb_stream_buffer_functions;
    stream->state = buffer;
}

const unsigned char *tb_stream_buffer_bytes(const tb_stream *stream, size_t *length)
{
    const tb_stream_buffer *buffer = NULL;
    TB_ASSERT(stream != NULL);
    TB_ASSERT(length != NULL);
    TB_ASSERT_MSG(stream->functions == &tb_stream_buffer_functions,
                  "the stream is not from tb_stream_open_buffer");
    buffer = stream->state;
    *length = buffer->length;
    return buffer->bytes;
}
