/* ChaCha20 against the published vectors of RFC 8439, the keystream from
 * every offset, and the stream that applies it: reads, reads at an offset,
 * writes, and seeks all line up with the keystream, and the stream offers
 * only what its inner stream does. */
#include "badger/assert.h"
#include "badger/chacha20.h"
#include "badger/stream.h"
#include "tb_test.h"

#include "chacha20_vectors.h"

#include <string.h>

TB_TEST(codec_chacha20, the_block_function_matches_rfc_8439)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(chacha20_block_vectors); index++)
    {
        const chacha20_block_vector *const vector = &chacha20_block_vectors[index];
        uint8_t block[TB_CHACHA20_BLOCK] = {0};
        TB_TEST_CONTEXT("vector %zu", index + 1);
        tb_chacha20_block(vector->key, vector->counter, vector->nonce, block);
        TB_TEST_EXPECT_BYTES(block, vector->keystream, TB_CHACHA20_BLOCK);
    }
}

TB_TEST(codec_chacha20, encryption_matches_rfc_8439_in_place_and_not)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(chacha20_cipher_vectors); index++)
    {
        const chacha20_cipher_vector *const vector = &chacha20_cipher_vectors[index];
        const uint64_t offset = (uint64_t)vector->counter * TB_CHACHA20_BLOCK;
        uint8_t output[375] = {0};
        TB_TEST_CONTEXT("vector %zu", index + 1);
        tb_chacha20_xor(vector->key, vector->nonce, offset, vector->plaintext, output,
                        vector->length);
        TB_TEST_EXPECT_BYTES(output, vector->ciphertext, vector->length);
        /* The same call restores, in place. */
        tb_chacha20_xor(vector->key, vector->nonce, offset, output, output, vector->length);
        TB_TEST_EXPECT_BYTES(output, vector->plaintext, vector->length);
    }
}

/* Any range of the data, combined from its own offset, is that range of the
 * whole: what makes reading at an offset possible. */
TB_TEST(codec_chacha20, every_range_matches_the_whole)
{
    const chacha20_cipher_vector *const vector = &chacha20_cipher_vectors[1];
    const uint64_t base = (uint64_t)vector->counter * TB_CHACHA20_BLOCK;
    for (size_t start = 0; start < vector->length; start += 7)
    {
        for (size_t length = 0; start + length <= vector->length; length += 13)
        {
            uint8_t output[375] = {0};
            TB_TEST_CONTEXT("bytes %zu to %zu", start, start + length);
            tb_chacha20_xor(vector->key, vector->nonce, base + start, vector->plaintext + start,
                            output, length);
            TB_TEST_EXPECT_BYTES(output, vector->ciphertext + start, length);
        }
    }
}

TB_TEST(codec_chacha20, the_keystream_reaches_its_last_block)
{
    const uint8_t key[TB_CHACHA20_KEY_SIZE] = {1};
    const uint8_t nonce[TB_CHACHA20_NONCE_SIZE] = {2};
    uint8_t expected[TB_CHACHA20_BLOCK] = {0};
    uint8_t bytes[TB_CHACHA20_BLOCK] = {0};
    tb_chacha20_block(key, UINT32_MAX, nonce, expected);
    tb_chacha20_xor(key, nonce, TB_CHACHA20_MAX_LENGTH - TB_CHACHA20_BLOCK, bytes, bytes,
                    sizeof(bytes));
    TB_TEST_EXPECT_BYTES(bytes, expected, sizeof(bytes));
}

/* -------------------------------------------------------------------------
 * The stream.
 * ------------------------------------------------------------------------- */

static const uint8_t stream_key[TB_CHACHA20_KEY_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const uint8_t stream_nonce[TB_CHACHA20_NONCE_SIZE] = {0, 0, 0, 0x09, 0, 0, 0, 0x4a};

static void fill(uint8_t *bytes, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        bytes[index] = (uint8_t)((index * 13 + 5) & 0xFFu);
    }
}

/* Written through the stream, the sink holds the data XORed from offset 0;
 * read back through another, the data comes back, by every means. */
TB_TEST(codec_chacha20, a_stream_obfuscates_what_it_writes_and_restores_what_it_reads)
{
    static uint8_t plain[10000];
    static uint8_t expected[10000];
    static uint8_t read[10000];
    tb_stream buffer = {0};
    tb_stream writer = {0};
    tb_stream source = {0};
    tb_stream reader = {0};
    const unsigned char *bytes = NULL;
    size_t length = 0;
    size_t count = 0;
    uint64_t position = 0;
    fill(plain, sizeof(plain));
    tb_chacha20_xor(stream_key, stream_nonce, 0, plain, expected, sizeof(plain));
    tb_stream_open_buffer(NULL, &buffer);
    TB_TEST_REQUIRE_UINT(tb_chacha20_open_stream(&buffer, stream_key, stream_nonce, NULL, &writer),
                         ==, TB_STREAM_OK);
    /* More than the stream's own buffer, so it writes in pieces. */
    TB_TEST_REQUIRE_UINT(tb_stream_write_all(&writer, plain, sizeof(plain)), ==, TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_flush(&writer), ==, TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_close(&writer), ==, TB_STREAM_OK);
    bytes = tb_stream_buffer_bytes(&buffer, &length);
    TB_TEST_REQUIRE_UINT(length, ==, sizeof(plain));
    TB_TEST_EXPECT_BYTES(bytes, expected, length);

    tb_stream_open_memory(bytes, length, NULL, &source);
    TB_TEST_REQUIRE_UINT(tb_chacha20_open_stream(&source, stream_key, stream_nonce, NULL, &reader),
                         ==, TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_read_exact(&reader, read, sizeof(read)), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(read, plain, sizeof(plain));
    /* At an offset, without moving. */
    memset(read, 0, sizeof(read));
    TB_TEST_REQUIRE_UINT(tb_stream_read_exact_at(&reader, 4321, read, 1000), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(read, plain + 4321, 1000);
    /* After a seek. */
    TB_TEST_REQUIRE_UINT(tb_stream_seek(&reader, 77, TB_STREAM_FROM_START, &position), ==,
                         TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_read(&reader, read, 100, &count), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(read, plain + 77, count);
    TB_TEST_REQUIRE_UINT(tb_stream_size(&reader, &position), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(position, ==, sizeof(plain));
    (void)tb_stream_close(&reader);
    (void)tb_stream_close(&source);
    (void)tb_stream_close(&buffer);
}

/* A stream opened where its inner stream already is starts at that place in
 * the keystream. */
TB_TEST(codec_chacha20, a_stream_starts_at_its_inner_streams_position)
{
    static uint8_t plain[300];
    static uint8_t obfuscated[300];
    uint8_t read[100] = {0};
    tb_stream source = {0};
    tb_stream reader = {0};
    fill(plain, sizeof(plain));
    tb_chacha20_xor(stream_key, stream_nonce, 0, plain, obfuscated, sizeof(plain));
    tb_stream_open_memory(obfuscated, sizeof(obfuscated), NULL, &source);
    TB_TEST_REQUIRE_UINT(tb_stream_seek(&source, 150, TB_STREAM_FROM_START, NULL), ==,
                         TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_chacha20_open_stream(&source, stream_key, stream_nonce, NULL, &reader),
                         ==, TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_read_exact(&reader, read, sizeof(read)), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_BYTES(read, plain + 150, sizeof(read));
    (void)tb_stream_close(&reader);
    (void)tb_stream_close(&source);
}

static tb_stream_result read_nothing(void *state, void *bytes, size_t capacity, size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)capacity;
    *transferred = 0;
    return TB_STREAM_OK;
}

static tb_stream_result release_nothing(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

/* A stream that only reads gives a stream that only reads: what callers
 * check before they read at an offset or seek. */
TB_TEST(codec_chacha20, a_stream_offers_what_its_inner_stream_offers)
{
    static const tb_stream_functions sequential = {.read_bytes = read_nothing,
                                                   .release = release_nothing};
    tb_stream inner = {&sequential, NULL};
    tb_stream stream = {0};
    uint8_t byte = 0;
    size_t count = 0;
    TB_TEST_REQUIRE_UINT(tb_chacha20_open_stream(&inner, stream_key, stream_nonce, NULL, &stream),
                         ==, TB_STREAM_OK);
    TB_TEST_EXPECT(stream.functions->read_bytes != NULL);
    TB_TEST_EXPECT(stream.functions->write_bytes == NULL);
    TB_TEST_EXPECT(stream.functions->seek == NULL);
    TB_TEST_EXPECT(stream.functions->size == NULL);
    TB_TEST_EXPECT(stream.functions->read_bytes_at == NULL);
    TB_TEST_EXPECT(stream.functions->flush == NULL);
    TB_TEST_EXPECT_UINT(tb_stream_read_at(&stream, 0, &byte, 1, &count), ==, TB_STREAM_UNSUPPORTED);
    (void)tb_stream_close(&stream);
}

TB_TEST(codec_chacha20, a_stream_refuses_positions_past_the_keystream)
{
    static const uint8_t zeros[16] = {0};
    tb_stream source = {0};
    tb_stream reader = {0};
    uint8_t bytes[16] = {0};
    size_t count = 0;
    tb_stream_open_memory(zeros, sizeof(zeros), NULL, &source);
    TB_TEST_REQUIRE_UINT(tb_chacha20_open_stream(&source, stream_key, stream_nonce, NULL, &reader),
                         ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(
        tb_stream_read_at(&reader, TB_CHACHA20_MAX_LENGTH - 8, bytes, sizeof(bytes), &count), ==,
        TB_STREAM_TOO_LARGE);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    (void)tb_stream_close(&reader);
    (void)tb_stream_close(&source);
}

static void chacha20_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    uint8_t bytes[4] = {0};
    uint8_t block[TB_CHACHA20_BLOCK] = {0};
    tb_stream stream = {0};
    switch (choice)
    {
    case 0:
        tb_chacha20_xor(NULL, stream_nonce, 0, bytes, bytes, 1);
        break;
    case 1:
        tb_chacha20_xor(stream_key, stream_nonce, 0, NULL, bytes, 1);
        break;
    case 2:
        tb_chacha20_xor(stream_key, stream_nonce, TB_CHACHA20_MAX_LENGTH - 2, bytes, bytes, 4);
        break;
    case 3:
        tb_chacha20_block(stream_key, 0, NULL, block);
        break;
    default:
        (void)tb_chacha20_open_stream(NULL, stream_key, stream_nonce, NULL, &stream);
        break;
    }
}

TB_TEST(codec_chacha20, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 5; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(chacha20_contract, &choice, "assertion failed");
    }
}
