/* ChaCha20 as RFC 8439 defines it, and a stream that applies it as bytes pass. */
#include "badger/chacha20.h"

#include "badger/assert.h"

#include <stdbool.h>
#include <string.h>

/* Bytes transformed at a time on their way to the inner stream. */
#define TB_CHACHA20_WRITE_BUFFER ((size_t)4096)

/* Clear memory that held a key: stores through a volatile pointer may not be
 * removed as dead. (This is not synchronization, for which volatile is never used.) */
static void tb_chacha20_wipe(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;
    while (length > 0)
    {
        *bytes = 0;
        bytes++;
        length--;
    }
}

static uint32_t tb_chacha20_load(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
           (uint32_t)bytes[3] << 24;
}

static uint32_t tb_chacha20_rotate(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32u - count));
}

#define TB_CHACHA20_QUARTER(a, b, c, d)                                                            \
    do                                                                                             \
    {                                                                                              \
        (a) += (b);                                                                                \
        (d) = tb_chacha20_rotate((d) ^ (a), 16);                                                   \
        (c) += (d);                                                                                \
        (b) = tb_chacha20_rotate((b) ^ (c), 12);                                                   \
        (a) += (b);                                                                                \
        (d) = tb_chacha20_rotate((d) ^ (a), 8);                                                    \
        (c) += (d);                                                                                \
        (b) = tb_chacha20_rotate((b) ^ (c), 7);                                                    \
    } while (0)

void tb_chacha20_block(const uint8_t key[TB_CHACHA20_KEY_SIZE], uint32_t counter,
                       const uint8_t nonce[TB_CHACHA20_NONCE_SIZE],
                       uint8_t output[TB_CHACHA20_BLOCK])
{
    /* "expand 32-byte k", then the key, the counter, and the nonce. */
    uint32_t input[16] = {0x61707865u, 0x3320646Eu, 0x79622D32u, 0x6B206574u};
    uint32_t state[16] = {0};
    TB_ASSERT(key != NULL);
    TB_ASSERT(nonce != NULL);
    TB_ASSERT(output != NULL);
    for (size_t index = 0; index < 8; index++)
    {
        input[4 + index] = tb_chacha20_load(key + 4 * index);
    }
    input[12] = counter;
    for (size_t index = 0; index < 3; index++)
    {
        input[13 + index] = tb_chacha20_load(nonce + 4 * index);
    }
    memcpy(state, input, sizeof(state));
    /* Twenty rounds: ten of a column round and a diagonal round. */
    for (size_t round = 0; round < 10; round++)
    {
        TB_CHACHA20_QUARTER(state[0], state[4], state[8], state[12]);
        TB_CHACHA20_QUARTER(state[1], state[5], state[9], state[13]);
        TB_CHACHA20_QUARTER(state[2], state[6], state[10], state[14]);
        TB_CHACHA20_QUARTER(state[3], state[7], state[11], state[15]);
        TB_CHACHA20_QUARTER(state[0], state[5], state[10], state[15]);
        TB_CHACHA20_QUARTER(state[1], state[6], state[11], state[12]);
        TB_CHACHA20_QUARTER(state[2], state[7], state[8], state[13]);
        TB_CHACHA20_QUARTER(state[3], state[4], state[9], state[14]);
    }
    for (size_t index = 0; index < 16; index++)
    {
        const uint32_t word = state[index] + input[index];
        output[4 * index] = (uint8_t)(word & 0xFFu);
        output[4 * index + 1] = (uint8_t)((word >> 8) & 0xFFu);
        output[4 * index + 2] = (uint8_t)((word >> 16) & 0xFFu);
        output[4 * index + 3] = (uint8_t)(word >> 24);
    }
}

void tb_chacha20_xor(const uint8_t key[TB_CHACHA20_KEY_SIZE],
                     const uint8_t nonce[TB_CHACHA20_NONCE_SIZE], uint64_t offset,
                     const void *input, void *output, size_t length)
{
    const uint8_t *source = input;
    uint8_t *destination = output;
    uint8_t block[TB_CHACHA20_BLOCK] = {0};
    TB_ASSERT(key != NULL);
    TB_ASSERT(nonce != NULL);
    TB_ASSERT((input != NULL && output != NULL) || length == 0);
    TB_ASSERT_MSG(offset <= TB_CHACHA20_MAX_LENGTH &&
                      (uint64_t)length <= TB_CHACHA20_MAX_LENGTH - offset,
                  "past the end of the ChaCha20 keystream");
    while (length > 0)
    {
        /* The bytes of this block to use: from `within` to `end`. */
        const size_t within = (size_t)(offset % TB_CHACHA20_BLOCK);
        const size_t end =
            length < TB_CHACHA20_BLOCK - within ? within + length : TB_CHACHA20_BLOCK;
        const size_t piece = end - within;
        tb_chacha20_block(key, (uint32_t)(offset / TB_CHACHA20_BLOCK), nonce, block);
        for (size_t index = within; index < end && index < TB_CHACHA20_BLOCK; index++)
        {
            destination[index - within] = (uint8_t)(source[index - within] ^ block[index]);
        }
        source += piece;
        destination += piece;
        offset += piece;
        length -= piece;
    }
    tb_chacha20_wipe(block, sizeof(block));
}

/* -------------------------------------------------------------------------
 * The stream.
 * ------------------------------------------------------------------------- */

typedef struct tb_chacha20_stream
{
    tb_stream_functions functions; /* What the inner stream offers, through this one. */
    tb_stream inner;
    tb_allocator allocator;
    uint64_t position; /* The inner stream's position, as this one has moved it. */
    uint8_t key[TB_CHACHA20_KEY_SIZE];
    uint8_t nonce[TB_CHACHA20_NONCE_SIZE];
} tb_chacha20_stream;

/* Whether `length` bytes from `offset` lie within the keystream. */
static bool tb_chacha20_fits(uint64_t offset, size_t length)
{
    return offset <= TB_CHACHA20_MAX_LENGTH && (uint64_t)length <= TB_CHACHA20_MAX_LENGTH - offset;
}

static tb_stream_result tb_chacha20_stream_read(void *state, void *bytes, size_t capacity,
                                                size_t *transferred)
{
    tb_chacha20_stream *const stream = state;
    tb_stream_result result = TB_STREAM_OK;
    *transferred = 0;
    if (!tb_chacha20_fits(stream->position, capacity))
    {
        return TB_STREAM_TOO_LARGE;
    }
    result = tb_stream_read(&stream->inner, bytes, capacity, transferred);
    tb_chacha20_xor(stream->key, stream->nonce, stream->position, bytes, bytes, *transferred);
    stream->position += *transferred;
    return result;
}

static tb_stream_result tb_chacha20_stream_read_at(void *state, uint64_t offset, void *bytes,
                                                   size_t capacity, size_t *transferred)
{
    tb_chacha20_stream *const stream = state;
    tb_stream_result result = TB_STREAM_OK;
    *transferred = 0;
    if (!tb_chacha20_fits(offset, capacity))
    {
        return TB_STREAM_TOO_LARGE;
    }
    result = tb_stream_read_at(&stream->inner, offset, bytes, capacity, transferred);
    tb_chacha20_xor(stream->key, stream->nonce, offset, bytes, bytes, *transferred);
    return result;
}

static tb_stream_result tb_chacha20_stream_write(void *state, const void *bytes, size_t length,
                                                 size_t *transferred)
{
    tb_chacha20_stream *const stream = state;
    uint8_t buffer[TB_CHACHA20_WRITE_BUFFER] = {0};
    const size_t piece = length < sizeof(buffer) ? length : sizeof(buffer);
    tb_stream_result result = TB_STREAM_OK;
    *transferred = 0;
    if (!tb_chacha20_fits(stream->position, piece))
    {
        return TB_STREAM_TOO_LARGE;
    }
    /* One buffer at a time: a short write is allowed, and the caller loops. */
    tb_chacha20_xor(stream->key, stream->nonce, stream->position, bytes, buffer, piece);
    result = tb_stream_write(&stream->inner, buffer, piece, transferred);
    stream->position += *transferred;
    tb_chacha20_wipe(buffer, piece);
    return result;
}

static tb_stream_result tb_chacha20_stream_seek(void *state, int64_t offset,
                                                tb_stream_origin origin, uint64_t *position)
{
    tb_chacha20_stream *const stream = state;
    const tb_stream_result result = tb_stream_seek(&stream->inner, offset, origin, position);
    if (result == TB_STREAM_OK)
    {
        stream->position = *position;
    }
    return result;
}

static tb_stream_result tb_chacha20_stream_size(void *state, uint64_t *size)
{
    tb_chacha20_stream *const stream = state;
    return tb_stream_size(&stream->inner, size);
}

static tb_stream_result tb_chacha20_stream_flush(void *state)
{
    tb_chacha20_stream *const stream = state;
    return tb_stream_flush(&stream->inner);
}

static tb_stream_result tb_chacha20_stream_release(void *state)
{
    tb_chacha20_stream *const stream = state;
    const tb_allocator allocator = stream->allocator;
    tb_chacha20_wipe(stream->key, sizeof(stream->key));
    TB_ALLOCATOR_DELETE(&allocator, tb_chacha20_stream, stream);
    return TB_STREAM_OK;
}

tb_stream_result tb_chacha20_open_stream(tb_stream *inner, const uint8_t key[TB_CHACHA20_KEY_SIZE],
                                         const uint8_t nonce[TB_CHACHA20_NONCE_SIZE],
                                         const tb_allocator *allocator, tb_stream *stream)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    const tb_stream_functions *offered = NULL;
    tb_chacha20_stream *state = NULL;
    uint64_t position = 0;
    TB_ASSERT(inner != NULL && inner->functions != NULL);
    TB_ASSERT(key != NULL);
    TB_ASSERT(nonce != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    offered = inner->functions;
    if (offered->seek != NULL)
    {
        const tb_stream_result told = tb_stream_tell(inner, &position);
        if (told != TB_STREAM_OK)
        {
            return told;
        }
    }
    state = TB_ALLOCATOR_NEW(&resolved, tb_chacha20_stream);
    state->functions = (tb_stream_functions){
        .read_bytes = offered->read_bytes != NULL ? tb_chacha20_stream_read : NULL,
        .write_bytes = offered->write_bytes != NULL ? tb_chacha20_stream_write : NULL,
        .seek = offered->seek != NULL ? tb_chacha20_stream_seek : NULL,
        .size = offered->size != NULL ? tb_chacha20_stream_size : NULL,
        .read_bytes_at = offered->read_bytes_at != NULL ? tb_chacha20_stream_read_at : NULL,
        .flush = offered->flush != NULL ? tb_chacha20_stream_flush : NULL,
        .release = tb_chacha20_stream_release,
    };
    state->inner = *inner;
    state->allocator = resolved;
    state->position = position;
    memcpy(state->key, key, sizeof(state->key));
    memcpy(state->nonce, nonce, sizeof(state->nonce));
    stream->functions = &state->functions;
    stream->state = state;
    return TB_STREAM_OK;
}
