/* A stream of one entry's data, ending at its size and checking its CRC-32. */
#include "entry_stream.h"

#include "badger/assert.h"
#include "badger/checksum.h"

#include <stdbool.h>

typedef struct tb_entry_stream
{
    tb_entry_layers layers;
    tb_allocator allocator;
    uint64_t remaining;
    uint32_t crc32;
    uint32_t expected_crc32;
    bool failed;
    bool verified;
} tb_entry_stream;

static tb_stream *tb_entry_data(tb_entry_stream *stream)
{
    return &stream->layers.layers[stream->layers.count - 1];
}

/* At the end of the size: nothing may follow, and the CRC-32 must match. */
static bool tb_entry_verify(tb_entry_stream *stream)
{
    unsigned char extra = 0;
    size_t count = 0;
    const tb_stream_result result = tb_stream_read(tb_entry_data(stream), &extra, 1, &count);
    stream->verified = true;
    return result == TB_STREAM_OK && count == 0 && stream->crc32 == stream->expected_crc32;
}

static tb_stream_result tb_entry_read(void *state, void *bytes, size_t capacity,
                                      size_t *transferred)
{
    tb_entry_stream *const stream = state;
    size_t want = capacity;
    tb_stream_result result = TB_STREAM_OK;
    *transferred = 0;
    if (stream->failed)
    {
        return TB_STREAM_MALFORMED;
    }
    if (stream->remaining < want)
    {
        want = (size_t)stream->remaining;
    }
    if (want > 0)
    {
        result = tb_stream_read(tb_entry_data(stream), bytes, want, transferred);
        stream->crc32 = tb_crc32_update(stream->crc32, bytes, *transferred);
        stream->remaining -= *transferred;
        if (result == TB_STREAM_END || result == TB_STREAM_MALFORMED ||
            (result == TB_STREAM_OK && *transferred == 0))
        {
            stream->failed = true;
            return TB_STREAM_MALFORMED;
        }
        if (result != TB_STREAM_OK)
        {
            return result;
        }
    }
    if (stream->remaining == 0 && !stream->verified && !tb_entry_verify(stream))
    {
        stream->failed = true;
        return TB_STREAM_MALFORMED;
    }
    return TB_STREAM_OK;
}

void tb_entry_layers_close(tb_entry_layers *layers)
{
    TB_ASSERT(layers != NULL);
    while (layers->count > 0)
    {
        layers->count--;
        (void)tb_stream_close(&layers->layers[layers->count]);
    }
}

static tb_stream_result tb_entry_release(void *state)
{
    tb_entry_stream *const stream = state;
    const tb_allocator allocator = stream->allocator;
    tb_entry_layers_close(&stream->layers);
    TB_ALLOCATOR_DELETE(&allocator, tb_entry_stream, stream);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_entry_functions = {
    .read_bytes = tb_entry_read,
    .release = tb_entry_release,
};

void tb_entry_stream_open(const tb_entry_layers *layers, uint64_t size, uint32_t crc32,
                          const tb_allocator *allocator, tb_stream *stream)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_entry_stream *state = NULL;
    TB_ASSERT(layers != NULL && layers->count > 0 && layers->count <= TB_ENTRY_MAX_LAYERS);
    TB_ASSERT(stream != NULL && stream->functions == NULL);
    state = TB_ALLOCATOR_NEW(&resolved, tb_entry_stream);
    state->layers = *layers;
    state->allocator = resolved;
    state->remaining = size;
    state->expected_crc32 = crc32;
    stream->functions = &tb_entry_functions;
    stream->state = state;
}
