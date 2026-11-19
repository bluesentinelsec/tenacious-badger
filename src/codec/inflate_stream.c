/* A stream that decompresses another as it is read. */
#include "badger/deflate.h"

#include "badger/assert.h"

/* Compressed bytes read from the source at a time. */
#define TB_INFLATE_STREAM_BUFFER ((size_t)16384)
#define TB_INFLATE_STREAM_GZIP_ID1 0x1Fu

typedef struct tb_inflate_stream
{
    tb_stream source;
    tb_inflater *inflater;
    tb_deflate_format format;
    tb_allocator allocator;
    size_t start; /* The first byte of `input` not yet given to the inflater. */
    size_t end;   /* One past the last byte read from the source. */
    unsigned char input[TB_INFLATE_STREAM_BUFFER];
} tb_inflate_stream;

static tb_stream_result tb_inflate_stream_failure(tb_deflate_result result)
{
    switch (result)
    {
    case TB_DEFLATE_OK:
        return TB_STREAM_OK;
    case TB_DEFLATE_INVALID:
        return TB_STREAM_INVALID;
    case TB_DEFLATE_TOO_LARGE:
        return TB_STREAM_TOO_LARGE;
    case TB_DEFLATE_TRUNCATED:
        return TB_STREAM_END;
    /* A wrong checksum and a preset dictionary make the data unreadable here. */
    case TB_DEFLATE_MALFORMED:
    case TB_DEFLATE_CHECKSUM:
    case TB_DEFLATE_UNSUPPORTED:
        return TB_STREAM_MALFORMED;
    default:
        return TB_STREAM_MALFORMED;
    }
}

/* Read more compressed bytes once those held are used. False at the end of
 * the source, with `result` OK; or on a failure of the source, in `result`. */
static bool tb_inflate_stream_refill(tb_inflate_stream *stream, tb_stream_result *result)
{
    size_t count = 0;
    TB_ASSERT(stream->start == stream->end);
    *result = tb_stream_read(&stream->source, stream->input, sizeof(stream->input), &count);
    stream->start = 0;
    stream->end = count;
    return *result == TB_STREAM_OK && count > 0;
}

/* At the end of a stream. In gzip format another member may follow, and is
 * read as a continuation; anything else after a member is malformed. Sets
 * `more` when there is another member to read. */
static tb_stream_result tb_inflate_stream_next_member(tb_inflate_stream *stream, bool *more)
{
    tb_stream_result result = TB_STREAM_OK;
    *more = false;
    if (stream->format != TB_DEFLATE_GZIP)
    {
        return TB_STREAM_OK;
    }
    if (stream->start == stream->end && !tb_inflate_stream_refill(stream, &result))
    {
        return result;
    }
    if (stream->input[stream->start] != TB_INFLATE_STREAM_GZIP_ID1)
    {
        return TB_STREAM_MALFORMED;
    }
    tb_inflater_reset(stream->inflater);
    *more = true;
    return TB_STREAM_OK;
}

static tb_stream_result tb_inflate_stream_read(void *state, void *bytes, size_t capacity,
                                               size_t *transferred)
{
    tb_inflate_stream *const stream = state;
    for (;;)
    {
        size_t used = 0;
        size_t written = 0;
        tb_deflate_result decoded = TB_DEFLATE_OK;
        tb_stream_result result = TB_STREAM_OK;
        if (tb_inflater_finished(stream->inflater))
        {
            bool more = false;
            result = tb_inflate_stream_next_member(stream, &more);
            if (result != TB_STREAM_OK || !more)
            {
                return result;
            }
        }
        decoded = tb_inflater_run(stream->inflater, stream->input + stream->start,
                                  stream->end - stream->start, &used, bytes, capacity, &written);
        stream->start += used;
        *transferred = written;
        if (decoded != TB_DEFLATE_OK)
        {
            return tb_inflate_stream_failure(decoded);
        }
        if (written > 0)
        {
            return TB_STREAM_OK;
        }
        if (used > 0 || tb_inflater_finished(stream->inflater))
        {
            continue;
        }
        /* No progress with room to write: the inflater has used every byte
         * it was given and needs more. */
        if (!tb_inflate_stream_refill(stream, &result))
        {
            return result == TB_STREAM_OK ? TB_STREAM_END : result;
        }
    }
}

static tb_stream_result tb_inflate_stream_release(void *state)
{
    tb_inflate_stream *const stream = state;
    const tb_allocator allocator = stream->allocator;
    tb_inflater_destroy(stream->inflater);
    TB_ALLOCATOR_DELETE(&allocator, tb_inflate_stream, stream);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_inflate_stream_functions = {
    .read_bytes = tb_inflate_stream_read,
    .release = tb_inflate_stream_release,
};

tb_stream_result tb_inflate_open_stream(tb_stream *source, tb_deflate_format format,
                                        const tb_allocator *allocator, tb_stream *stream)
{
    tb_allocator resolved = {0};
    tb_inflate_stream *state = NULL;
    TB_ASSERT(source != NULL && source->functions != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    if (format != TB_DEFLATE_RAW && format != TB_DEFLATE_ZLIB && format != TB_DEFLATE_GZIP)
    {
        return TB_STREAM_INVALID;
    }
    if (source->functions->read_bytes == NULL)
    {
        return TB_STREAM_UNSUPPORTED;
    }
    resolved = tb_allocator_resolve(allocator);
    state = TB_ALLOCATOR_NEW(&resolved, tb_inflate_stream);
    state->source = *source;
    state->inflater = tb_inflater_create(format, &resolved);
    state->format = format;
    state->allocator = resolved;
    stream->functions = &tb_inflate_stream_functions;
    stream->state = state;
    return TB_STREAM_OK;
}
