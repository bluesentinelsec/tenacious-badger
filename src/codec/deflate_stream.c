/* A stream that compresses what is written to it into another. */
#include "badger/deflate.h"

#include "badger/assert.h"

/* Compressed bytes gathered before they are written to the sink. */
#define TB_DEFLATE_STREAM_BUFFER ((size_t)16384)

typedef struct tb_deflate_stream
{
    tb_stream sink;
    tb_deflater *deflater;
    tb_allocator allocator;
    unsigned char output[TB_DEFLATE_STREAM_BUFFER];
} tb_deflate_stream;

/* Run the deflater over `bytes` until it has taken them all and, for a flush,
 * written everything out; pass each piece of output to the sink. */
static tb_stream_result tb_deflate_stream_pump(tb_deflate_stream *stream,
                                               const unsigned char *bytes, size_t length,
                                               tb_deflate_flush flush)
{
    size_t taken = 0;
    for (;;)
    {
        size_t used = 0;
        size_t written = 0;
        tb_stream_result result = TB_STREAM_OK;
        const tb_deflate_result compressed =
            tb_deflater_run(stream->deflater, bytes + taken, length - taken, &used, stream->output,
                            sizeof(stream->output), &written, flush);
        TB_ASSERT(compressed == TB_DEFLATE_OK);
        (void)compressed;
        taken += used;
        result = tb_stream_write_all(&stream->sink, stream->output, written);
        if (result != TB_STREAM_OK)
        {
            return result;
        }
        /* Done when every byte is taken and the deflater stopped short of the room. */
        if (taken == length && written < sizeof(stream->output))
        {
            return TB_STREAM_OK;
        }
    }
}

static tb_stream_result tb_deflate_stream_write(void *state, const void *bytes, size_t length,
                                                size_t *transferred)
{
    tb_deflate_stream *const stream = state;
    const tb_stream_result result =
        tb_deflate_stream_pump(stream, bytes, length, TB_DEFLATE_NO_FLUSH);
    if (result == TB_STREAM_OK)
    {
        *transferred = length;
    }
    return result;
}

static tb_stream_result tb_deflate_stream_flush(void *state)
{
    tb_deflate_stream *const stream = state;
    const tb_stream_result result = tb_deflate_stream_pump(stream, NULL, 0, TB_DEFLATE_SYNC_FLUSH);
    if (result != TB_STREAM_OK)
    {
        return result;
    }
    return tb_stream_flush(&stream->sink);
}

static tb_stream_result tb_deflate_stream_release(void *state)
{
    tb_deflate_stream *const stream = state;
    const tb_allocator allocator = stream->allocator;
    const tb_stream_result result = tb_deflate_stream_pump(stream, NULL, 0, TB_DEFLATE_FINISH);
    tb_deflater_destroy(stream->deflater);
    TB_ALLOCATOR_DELETE(&allocator, tb_deflate_stream, stream);
    return result;
}

static const tb_stream_functions tb_deflate_stream_functions = {
    .write_bytes = tb_deflate_stream_write,
    .flush = tb_deflate_stream_flush,
    .release = tb_deflate_stream_release,
};

tb_stream_result tb_deflate_open_stream(tb_stream *sink, tb_deflate_format format, int level,
                                        const tb_allocator *allocator, tb_stream *stream)
{
    tb_allocator resolved = {0};
    tb_deflate_stream *state = NULL;
    TB_ASSERT(sink != NULL && sink->functions != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    if (format != TB_DEFLATE_RAW && format != TB_DEFLATE_ZLIB && format != TB_DEFLATE_GZIP)
    {
        return TB_STREAM_INVALID;
    }
    if (level < 0 || level > TB_DEFLATE_BEST_LEVEL)
    {
        return TB_STREAM_INVALID;
    }
    if (sink->functions->write_bytes == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    resolved = tb_allocator_resolve(allocator);
    state = TB_ALLOCATOR_NEW(&resolved, tb_deflate_stream);
    state->sink = *sink;
    state->deflater = tb_deflater_create(format, level, &resolved);
    state->allocator = resolved;
    stream->functions = &tb_deflate_stream_functions;
    stream->state = state;
    return TB_STREAM_OK;
}
