/* DEFLATE decompression: streams made by zlib decode to what was compressed,
 * in one call or fed a byte at a time; every truncation and every flipped bit
 * of them is a result, never a crash; and streams built by hand break each
 * rule of RFC 1950, 1951, and 1952 in turn. */
#include "badger/assert.h"
#include "badger/deflate.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "deflate_vectors.h"
#include "tb_test.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * The inputs the vectors compress: see tools/generate_deflate_vectors.py.
 * ------------------------------------------------------------------------- */

enum
{
    SAMPLE_TEXT = 0,
    SAMPLE_RANDOM = 1,
    SAMPLE_ZEROS = 2,
    SAMPLE_FAR_MATCH = 3,
    FAR_PREFIX = 300,
    FAR_GAP = 31700
};

static void lcg_bytes(unsigned char *output, size_t length, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t index = 0; index < length; index++)
    {
        state = state * 1103515245u + 12345u;
        output[index] = (unsigned char)((state >> 16) & 0xFFu);
    }
}

static void text_bytes(unsigned char *output, size_t length)
{
    static const char tail[] = ": the quick brown fox jumps over the lazy dog\n";
    size_t done = 0;
    unsigned line = 0;
    while (done < length)
    {
        char row[64] = {0};
        size_t row_length = 0;
        unsigned value = line;
        /* "line %05d" without the C library's formatting. */
        memcpy(row, "line ", 5);
        for (size_t digit = 0; digit < 5; digit++)
        {
            row[9 - digit] = (char)('0' + value % 10);
            value /= 10;
        }
        memcpy(row + 10, tail, sizeof(tail) - 1);
        row_length = 10 + sizeof(tail) - 1;
        for (size_t index = 0; index < row_length && done < length; index++)
        {
            output[done] = (unsigned char)row[index];
            done++;
        }
        line++;
    }
}

static void sample_bytes(int kind, unsigned char *output, size_t length)
{
    switch (kind)
    {
    case SAMPLE_TEXT:
        text_bytes(output, length);
        break;
    case SAMPLE_RANDOM:
        lcg_bytes(output, length, 12345u);
        break;
    case SAMPLE_ZEROS:
        memset(output, 0, length);
        break;
    default:
        TB_TEST_REQUIRE_UINT(length, ==, 2 * FAR_PREFIX + FAR_GAP);
        lcg_bytes(output, FAR_PREFIX, 777u);
        text_bytes(output + FAR_PREFIX, FAR_GAP);
        memcpy(output + FAR_PREFIX + FAR_GAP, output, FAR_PREFIX);
        break;
    }
}

/* What a vector decodes to, in a block from tb_malloc. */
static unsigned char *expected_output(const deflate_vector *vector, size_t *length)
{
    unsigned char *const bytes = tb_malloc(vector->length * vector->members + 1);
    for (size_t member = 0; member < vector->members; member++)
    {
        sample_bytes(vector->kind, bytes + member * vector->length, vector->length);
    }
    *length = vector->length * vector->members;
    return bytes;
}

/* -------------------------------------------------------------------------
 * The reference streams.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_inflate, every_reference_stream_decodes_in_one_call)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(deflate_vectors); row++)
    {
        const deflate_vector *const vector = &deflate_vectors[row];
        size_t length = 0;
        size_t written = 0;
        unsigned char *expected = NULL;
        unsigned char *output = NULL;
        if (vector->members != 1)
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", vector->name);
        expected = expected_output(vector, &length);
        output = tb_malloc(length + 1);
        /* Exactly the room it needs. */
        TB_TEST_EXPECT_UINT(
            tb_inflate(vector->format, vector->data, vector->size, output, length, &written), ==,
            TB_DEFLATE_OK);
        TB_TEST_EXPECT_UINT(written, ==, length);
        TB_TEST_EXPECT_BYTES(output, expected, length);
        /* One byte less is too little. */
        if (length > 0)
        {
            TB_TEST_EXPECT_UINT(tb_inflate(vector->format, vector->data, vector->size, output,
                                           length - 1, &written),
                                ==, TB_DEFLATE_TOO_LARGE);
            TB_TEST_EXPECT_UINT(written, ==, length - 1);
        }
        tb_free(output);
        tb_free(expected);
    }
}

/* Feed the inflater input and room in pieces of the given sizes, cycling. */
static void decode_in_pieces(const deflate_vector *vector, const unsigned char *expected,
                             size_t length, size_t input_piece, size_t output_piece, bool random)
{
    tb_inflater *const inflater = tb_inflater_create(vector->format, NULL);
    unsigned char *const output = tb_malloc(length + 1);
    size_t consumed = 0;
    size_t produced = 0;
    size_t stalls = 0;
    while (!tb_inflater_finished(inflater))
    {
        size_t give = input_piece;
        size_t room = output_piece;
        size_t used = 0;
        size_t written = 0;
        if (random)
        {
            give = 1 + (size_t)tb_test_random_below(input_piece);
            room = 1 + (size_t)tb_test_random_below(output_piece);
        }
        if (give > vector->size - consumed)
        {
            give = vector->size - consumed;
        }
        if (room > length + 1 - produced)
        {
            room = length + 1 - produced;
        }
        TB_TEST_REQUIRE_UINT(tb_inflater_run(inflater, vector->data + consumed, give, &used,
                                             output + produced, room, &written),
                             ==, TB_DEFLATE_OK);
        TB_TEST_REQUIRE_UINT(used, <=, give);
        TB_TEST_REQUIRE_UINT(written, <=, room);
        consumed += used;
        produced += written;
        /* A call with input and room left must make progress, or the stream is
         * waiting for what it was not given. */
        if (used == 0 && written == 0)
        {
            stalls++;
            TB_TEST_REQUIRE_UINT(stalls, <, 3);
        }
        else
        {
            stalls = 0;
        }
    }
    /* Every byte of the stream, and no byte past it, was consumed. */
    TB_TEST_EXPECT_UINT(consumed, ==, vector->size);
    TB_TEST_EXPECT_UINT(produced, ==, length);
    TB_TEST_EXPECT_BYTES(output, expected, length);
    tb_free(output);
    tb_inflater_destroy(inflater);
}

TB_TEST(codec_inflate, pieces_of_any_size_decode_the_same)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(deflate_vectors); row++)
    {
        const deflate_vector *const vector = &deflate_vectors[row];
        size_t length = 0;
        unsigned char *expected = NULL;
        if (vector->members != 1)
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", vector->name);
        expected = expected_output(vector, &length);
        decode_in_pieces(vector, expected, length, 1, 1, false);
        decode_in_pieces(vector, expected, length, 1, 4096, false);
        decode_in_pieces(vector, expected, length, 4096, 1, false);
        /* Whole DEFLATE matches fit at 258 bytes. Check both sides of the
         * boundary and calls large enough to defer a full window flush. */
        decode_in_pieces(vector, expected, length, 8, 258, false);
        decode_in_pieces(vector, expected, length, 512, 257, false);
        decode_in_pieces(vector, expected, length, 65536, 40000, false);
        for (size_t trial = 0; trial < 5; trial++)
        {
            decode_in_pieces(vector, expected, length, 64, 300, true);
        }
        tb_free(expected);
    }
}

/* A caller may immediately reuse its output. Matches spanning calls must
 * survive a single run longer than the window, including a partial wrap. */
TB_TEST(codec_inflate, repeated_calls_can_overwrite_the_previous_output)
{
    const size_t span = 32749;
    const size_t length = span * 5u;
    const size_t room = 65537;
    unsigned char *const original = tb_malloc(length);
    unsigned char *const chunk = tb_malloc(room);
    const size_t bound = tb_deflate_bound(length);
    unsigned char *const encoded = tb_malloc(bound + 8u);
    size_t bytes = 0, consumed = 0, produced = 0;
    /* Only sixteen literal values, then matches nearly a whole window back. */
    lcg_bytes(original, span, 777);
    for (size_t i = 0; i < span; i++)
    {
        original[i] &= 15u;
    }
    for (size_t i = span; i < length; i++)
    {
        original[i] = original[i - span];
    }
    TB_TEST_REQUIRE_UINT(tb_deflate(TB_DEFLATE_RAW, 6, original, length, encoded, bound, &bytes),
                         ==, TB_DEFLATE_OK);
    memset(encoded + bytes, 0xAC, 8);
    tb_inflater *const inflater = tb_inflater_create(TB_DEFLATE_RAW, NULL);
    while (!tb_inflater_finished(inflater))
    {
        size_t used = 0, written = 0;
        memset(chunk, 0xCC, room);
        TB_TEST_REQUIRE_UINT(tb_inflater_run(inflater, encoded + consumed, bytes + 8u - consumed,
                                             &used, chunk, room, &written),
                             ==, TB_DEFLATE_OK);
        TB_TEST_REQUIRE(used != 0 || written != 0 || tb_inflater_finished(inflater));
        TB_TEST_REQUIRE_UINT(used, <=, bytes - consumed);
        TB_TEST_REQUIRE_UINT(written, <=, length - produced);
        TB_TEST_EXPECT_BYTES(chunk, original + produced, written);
        consumed += used;
        produced += written;
    }
    TB_TEST_EXPECT_UINT(consumed, ==, bytes);
    TB_TEST_EXPECT_UINT(produced, ==, length);
    tb_inflater_destroy(inflater);
    tb_free(encoded);
    tb_free(chunk);
    tb_free(original);
}

/* What follows a stream is not consumed: the next gzip member, or whatever an
 * archive holds after an entry. tb_inflate treats it as malformed. */
TB_TEST(codec_inflate, bytes_after_the_end_are_left_alone)
{
    const deflate_vector *const vector = &deflate_vectors[0];
    unsigned char *input = tb_malloc(vector->size + 5);
    unsigned char *output = NULL;
    size_t length = 0;
    size_t used = 0;
    size_t written = 0;
    unsigned char *const expected = expected_output(vector, &length);
    tb_inflater *const inflater = tb_inflater_create(vector->format, NULL);
    memcpy(input, vector->data, vector->size);
    memcpy(input + vector->size, "tail!", 5);
    output = tb_malloc(length);
    TB_TEST_REQUIRE_UINT(
        tb_inflater_run(inflater, input, vector->size + 5, &used, output, length, &written), ==,
        TB_DEFLATE_OK);
    TB_TEST_EXPECT(tb_inflater_finished(inflater));
    TB_TEST_EXPECT_UINT(used, ==, vector->size);
    TB_TEST_EXPECT_UINT(written, ==, length);
    /* A finished inflater takes nothing more. */
    TB_TEST_EXPECT_UINT(tb_inflater_run(inflater, input + used, 5, &used, output, length, &written),
                        ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(used, ==, 0);
    TB_TEST_EXPECT_UINT(written, ==, 0);
    TB_TEST_EXPECT_UINT(
        tb_inflate(vector->format, input, vector->size + 5, output, length, &written), ==,
        TB_DEFLATE_MALFORMED);
    tb_inflater_destroy(inflater);
    tb_free(output);
    tb_free(expected);
    tb_free(input);
}

/* -------------------------------------------------------------------------
 * Damaged streams.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_inflate, every_truncation_is_reported)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(deflate_vectors); row++)
    {
        const deflate_vector *const vector = &deflate_vectors[row];
        size_t length = 0;
        unsigned char *expected = NULL;
        unsigned char *output = NULL;
        /* tb_inflate reads one member; the stream tests cover several. */
        if (vector->members != 1)
        {
            continue;
        }
        expected = expected_output(vector, &length);
        output = tb_malloc(length + 1);
        TB_TEST_CONTEXT("%s", vector->name);
        for (size_t prefix = 0; prefix < vector->size; prefix++)
        {
            size_t written = 0;
            const tb_deflate_result result =
                tb_inflate(vector->format, vector->data, prefix, output, length, &written);
            TB_TEST_REQUIRE_UINT(result, ==, TB_DEFLATE_TRUNCATED);
            TB_TEST_REQUIRE_UINT(written, <=, length);
            TB_TEST_REQUIRE_BYTES(output, expected, written);
        }
        tb_free(output);
        tb_free(expected);
    }
}

/* A flipped bit may leave a valid stream, or make any failure; it never
 * makes the decoder read or write out of bounds, which the sanitizer builds
 * of this test check. */
TB_TEST(codec_inflate, every_flipped_bit_of_small_streams_is_survived)
{
    enum
    {
        LARGEST = 1000,
        ROOM = 40000
    };
    unsigned char *const output = tb_malloc(ROOM);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(deflate_vectors); row++)
    {
        const deflate_vector *const vector = &deflate_vectors[row];
        unsigned char damaged[LARGEST] = {0};
        if (vector->size > LARGEST)
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", vector->name);
        memcpy(damaged, vector->data, vector->size);
        for (size_t bit = 0; bit < vector->size * 8; bit++)
        {
            size_t written = 0;
            tb_deflate_result result = TB_DEFLATE_OK;
            damaged[bit / 8] ^= (unsigned char)(1u << (bit % 8));
            result = tb_inflate(vector->format, damaged, vector->size, output, ROOM, &written);
            damaged[bit / 8] ^= (unsigned char)(1u << (bit % 8));
            TB_TEST_REQUIRE_UINT(written, <=, ROOM);
            TB_TEST_REQUIRE(result != TB_DEFLATE_INVALID);
        }
    }
    tb_free(output);
}

/* -------------------------------------------------------------------------
 * Streams built bit by bit, each breaking one rule.
 * ------------------------------------------------------------------------- */

typedef struct bit_writer
{
    unsigned char bytes[1024];
    size_t length;
    unsigned bit;
} bit_writer;

/* Bits of a value, lowest first, as DEFLATE packs header fields and extra bits. */
static void put_bits(bit_writer *writer, unsigned value, unsigned count)
{
    for (unsigned index = 0; index < count; index++)
    {
        if (writer->bit == 0)
        {
            TB_TEST_REQUIRE_UINT(writer->length, <, sizeof(writer->bytes));
            writer->bytes[writer->length] = 0;
            writer->length++;
        }
        writer->bytes[writer->length - 1] |=
            (unsigned char)(((value >> index) & 1u) << writer->bit);
        writer->bit = (writer->bit + 1) & 7u;
    }
}

/* A Huffman code, highest bit first. */
static void put_code(bit_writer *writer, unsigned code, unsigned length)
{
    for (unsigned index = length; index > 0; index--)
    {
        put_bits(writer, (code >> (index - 1)) & 1u, 1);
    }
}

static void put_fixed_literal(bit_writer *writer, unsigned symbol)
{
    if (symbol < 144)
    {
        put_code(writer, 0x30u + symbol, 8);
    }
    else if (symbol < 256)
    {
        put_code(writer, 0x190u + symbol - 144, 9);
    }
    else if (symbol < 280)
    {
        put_code(writer, symbol - 256, 7);
    }
    else
    {
        put_code(writer, 0xC0u + symbol - 280, 8);
    }
}

static tb_deflate_result inflate_raw(const bit_writer *writer, unsigned char *output,
                                     size_t capacity, size_t *written)
{
    return tb_inflate(TB_DEFLATE_RAW, writer->bytes, writer->length, output, capacity, written);
}

TB_TEST(codec_inflate, fixed_blocks_check_every_symbol_and_distance)
{
    unsigned char output[16] = {0};
    size_t written = 0;
    bit_writer writer = {0};

    /* 'a', then a match of 3 at distance 1: "aaaa". */
    put_bits(&writer, 1, 1);
    put_bits(&writer, 1, 2);
    put_fixed_literal(&writer, 'a');
    put_fixed_literal(&writer, 257);
    put_code(&writer, 0, 5);
    put_fixed_literal(&writer, 256);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(written, ==, 4);
    TB_TEST_EXPECT_BYTES(output, "aaaa", 4);

    /* Distance 2 after one byte reaches before the start. */
    writer = (bit_writer){0};
    put_bits(&writer, 1, 1);
    put_bits(&writer, 1, 2);
    put_fixed_literal(&writer, 'a');
    put_fixed_literal(&writer, 257);
    put_code(&writer, 1, 5);
    put_fixed_literal(&writer, 256);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* Length symbols 286 and 287, and distance symbols 30 and 31, have codes
     * in the fixed code but no meaning. */
    for (unsigned symbol = 286; symbol <= 287; symbol++)
    {
        writer = (bit_writer){0};
        put_bits(&writer, 1, 1);
        put_bits(&writer, 1, 2);
        put_fixed_literal(&writer, symbol);
        TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                            TB_DEFLATE_MALFORMED);
    }
    for (unsigned symbol = 30; symbol <= 31; symbol++)
    {
        writer = (bit_writer){0};
        put_bits(&writer, 1, 1);
        put_bits(&writer, 1, 2);
        put_fixed_literal(&writer, 'a');
        put_fixed_literal(&writer, 257);
        put_code(&writer, symbol, 5);
        TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                            TB_DEFLATE_MALFORMED);
    }

    /* Block type 3 is reserved. */
    writer = (bit_writer){0};
    put_bits(&writer, 1, 1);
    put_bits(&writer, 3, 2);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);
}

TB_TEST(codec_inflate, stored_blocks_check_their_length_and_reach_the_limit)
{
    enum
    {
        FIRST = 65535,
        SECOND = 10
    };
    unsigned char *const input = tb_malloc(FIRST + SECOND + 10);
    unsigned char *const output = tb_malloc(FIRST + SECOND);
    size_t written = 0;
    size_t at = 0;
    /* A block of the largest stored length, then a final one. */
    input[at++] = 0x00; /* Not final, stored; the rest of the byte is padding. */
    input[at++] = 0xFF;
    input[at++] = 0xFF;
    input[at++] = 0x00;
    input[at++] = 0x00;
    lcg_bytes(input + at, FIRST, 99u);
    at += FIRST;
    input[at++] = 0x01; /* Final, stored. */
    input[at++] = SECOND;
    input[at++] = 0x00;
    input[at++] = (unsigned char)~SECOND;
    input[at++] = 0xFF;
    memset(input + at, 'z', SECOND);
    at += SECOND;
    TB_TEST_EXPECT_UINT(tb_inflate(TB_DEFLATE_RAW, input, at, output, FIRST + SECOND, &written), ==,
                        TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(written, ==, FIRST + SECOND);
    TB_TEST_EXPECT_BYTES(output, input + 5, FIRST);
    /* A length whose complement does not match. */
    input[3] = 0x01;
    TB_TEST_EXPECT_UINT(tb_inflate(TB_DEFLATE_RAW, input, at, output, FIRST + SECOND, &written), ==,
                        TB_DEFLATE_MALFORMED);
    tb_free(output);
    tb_free(input);
}

/* A dynamic block whose code-length code has eight symbols of three bits:
 * 0, 1, 2, 3, 8, 16, 17, 18, coded 0 to 7 in that order. */
static const unsigned dynamic_alphabet[8] = {0, 1, 2, 3, 8, 16, 17, 18};

static unsigned dynamic_code_of(unsigned symbol)
{
    unsigned index = 0;
    while (index < 8 && dynamic_alphabet[index] != symbol)
    {
        index++;
    }
    TB_TEST_REQUIRE_UINT(index, <, 8);
    return index;
}

static void put_dynamic_start(bit_writer *writer, unsigned literal_count, unsigned distance_count,
                              unsigned code_length_symbols)
{
    static const unsigned order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};
    put_bits(writer, 1, 1);
    put_bits(writer, 2, 2);
    put_bits(writer, literal_count - 257, 5);
    put_bits(writer, distance_count - 1, 5);
    /* Symbol 1 is 18th in the order. */
    put_bits(writer, 18 - 4, 4);
    for (unsigned index = 0; index < 18; index++)
    {
        bool listed = false;
        for (unsigned symbol = 0; symbol < code_length_symbols; symbol++)
        {
            if (dynamic_alphabet[symbol] == order[index])
            {
                listed = true;
            }
        }
        put_bits(writer, listed ? 3u : 0u, 3);
    }
}

static void put_length_symbol(bit_writer *writer, unsigned symbol)
{
    put_code(writer, dynamic_code_of(symbol), 3);
}

/* Code lengths written one by one, each 0, 1, 2, 3, or 8. */
static void put_lengths(bit_writer *writer, const unsigned char *lengths, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        put_length_symbol(writer, lengths[index]);
    }
}

/* Literal and length lengths: 8 bits for 0 to 253, 256, and 257: a complete code
 * in which symbol s below 254 is coded s, 256 is 254, and 257 is 255. */
static void dynamic_literal_lengths(unsigned char lengths[257 + 1])
{
    memset(lengths, 0, 258);
    memset(lengths, 8, 254);
    lengths[256] = 8;
    lengths[257] = 8;
}

TB_TEST(codec_inflate, dynamic_blocks_check_their_codes)
{
    unsigned char lengths[258 + 2] = {0};
    unsigned char output[16] = {0};
    size_t written = 0;
    bit_writer writer = {0};

    /* One distance code of one bit is allowed: 'a', a match of 3 at distance 1. */
    dynamic_literal_lengths(lengths);
    put_dynamic_start(&writer, 258, 1, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 1);
    put_code(&writer, 'a', 8);
    put_code(&writer, 255, 8);
    put_code(&writer, 0, 1);
    put_code(&writer, 254, 8);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_BYTES(output, "aaaa", 4);

    /* No distance codes is allowed, for a block of literals, until a match. */
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 258, 1, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 0);
    put_code(&writer, 'b', 8);
    put_code(&writer, 254, 8);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(written, ==, 1);
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 258, 1, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 0);
    put_code(&writer, 'b', 8);
    put_code(&writer, 255, 8);
    put_code(&writer, 0, 1);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* Too many literal and length codes, and too many distance codes. */
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 287, 1, 8);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 258, 31, 8);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* A code-length code that is incomplete: seven codes of three bits. */
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 258, 1, 7);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* A repeat of the previous length with no previous length. */
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 258, 1, 8);
    put_length_symbol(&writer, 16);
    put_bits(&writer, 0, 2);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* Repeats that run past the number of lengths. */
    writer = (bit_writer){0};
    put_dynamic_start(&writer, 258, 1, 8);
    put_length_symbol(&writer, 18);
    put_bits(&writer, 127, 7);
    put_length_symbol(&writer, 18);
    put_bits(&writer, 127, 7);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* No code for the end of the block. */
    writer = (bit_writer){0};
    lengths[256] = 0;
    lengths[255] = 8;
    put_dynamic_start(&writer, 258, 1, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 1);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* Over-subscribed: 257 codes of eight bits. */
    writer = (bit_writer){0};
    dynamic_literal_lengths(lengths);
    lengths[255] = 8;
    put_dynamic_start(&writer, 258, 1, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 1);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* Incomplete with more than one code: 255 codes of eight bits. */
    writer = (bit_writer){0};
    dynamic_literal_lengths(lengths);
    lengths[253] = 0;
    put_dynamic_start(&writer, 258, 1, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 1);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);

    /* An incomplete distance code with two codes. */
    writer = (bit_writer){0};
    dynamic_literal_lengths(lengths);
    put_dynamic_start(&writer, 258, 2, 8);
    put_lengths(&writer, lengths, 258);
    put_length_symbol(&writer, 2);
    put_length_symbol(&writer, 2);
    TB_TEST_EXPECT_UINT(inflate_raw(&writer, output, sizeof(output), &written), ==,
                        TB_DEFLATE_MALFORMED);
}

/* -------------------------------------------------------------------------
 * zlib and gzip framing.
 * ------------------------------------------------------------------------- */

static const deflate_vector *find_vector(const char *name)
{
    size_t row = 0;
    while (row < TB_TEST_COUNT_OF(deflate_vectors) &&
           !tb_str_equal(deflate_vectors[row].name, name))
    {
        row++;
    }
    TB_TEST_REQUIRE_UINT(row, <, TB_TEST_COUNT_OF(deflate_vectors));
    return &deflate_vectors[row];
}

/* Decode a copy of a vector with one byte replaced. */
static tb_deflate_result inflate_changed(const deflate_vector *vector, size_t offset,
                                         unsigned char value)
{
    unsigned char *const copy = tb_malloc(vector->size);
    unsigned char *const output = tb_malloc(vector->length + 1);
    size_t written = 0;
    tb_deflate_result result = TB_DEFLATE_OK;
    memcpy(copy, vector->data, vector->size);
    copy[offset] = value;
    result = tb_inflate(vector->format, copy, vector->size, output, vector->length, &written);
    tb_free(output);
    tb_free(copy);
    return result;
}

TB_TEST(codec_inflate, zlib_headers_and_checksums_are_checked)
{
    const deflate_vector *const vector = find_vector("zlib_text_level6");
    unsigned char output[4] = {0};
    size_t written = 0;
    /* The header's check bits. */
    TB_TEST_EXPECT_UINT(inflate_changed(vector, 1, (unsigned char)(vector->data[1] ^ 1u)), ==,
                        TB_DEFLATE_MALFORMED);
    /* A method other than DEFLATE, and a window larger than 32 KiB; each with a valid check. */
    {
        static const unsigned char method_seven[2] = {0x77, 0x00};
        static const unsigned char window_too_large[2] = {0x88, 0x00};
        unsigned char header[2] = {0};
        memcpy(header, method_seven, 2);
        header[1] = (unsigned char)(31 - ((unsigned)header[0] << 8) % 31);
        TB_TEST_EXPECT_UINT(tb_inflate(TB_DEFLATE_ZLIB, header, 2, output, 4, &written), ==,
                            TB_DEFLATE_MALFORMED);
        memcpy(header, window_too_large, 2);
        header[1] = (unsigned char)(31 - ((unsigned)header[0] << 8) % 31);
        TB_TEST_EXPECT_UINT(tb_inflate(TB_DEFLATE_ZLIB, header, 2, output, 4, &written), ==,
                            TB_DEFLATE_MALFORMED);
    }
    /* A preset dictionary is valid zlib that this decoder cannot read. */
    {
        unsigned char header[2] = {0x78, 0x20};
        header[1] = (unsigned char)(header[1] + (31 - ((0x78u << 8) | header[1]) % 31));
        TB_TEST_EXPECT_UINT(tb_inflate(TB_DEFLATE_ZLIB, header, 2, output, 4, &written), ==,
                            TB_DEFLATE_UNSUPPORTED);
    }
    /* The Adler-32 at the end. */
    TB_TEST_EXPECT_UINT(inflate_changed(vector, vector->size - 1,
                                        (unsigned char)(vector->data[vector->size - 1] ^ 0x80u)),
                        ==, TB_DEFLATE_CHECKSUM);
}

TB_TEST(codec_inflate, gzip_headers_and_trailers_are_checked)
{
    const deflate_vector *const plain = find_vector("gzip_text_plain");
    const deflate_vector *const fields = find_vector("gzip_text_every_header_field");
    TB_TEST_EXPECT_UINT(inflate_changed(plain, 0, 0x1E), ==, TB_DEFLATE_MALFORMED);
    TB_TEST_EXPECT_UINT(inflate_changed(plain, 1, 0x8A), ==, TB_DEFLATE_MALFORMED);
    TB_TEST_EXPECT_UINT(inflate_changed(plain, 2, 7), ==, TB_DEFLATE_MALFORMED);
    TB_TEST_EXPECT_UINT(inflate_changed(plain, 3, 0x20), ==, TB_DEFLATE_MALFORMED);
    /* The CRC-32, then the length, of the trailer. */
    TB_TEST_EXPECT_UINT(
        inflate_changed(plain, plain->size - 8, (unsigned char)(plain->data[plain->size - 8] ^ 1u)),
        ==, TB_DEFLATE_CHECKSUM);
    TB_TEST_EXPECT_UINT(
        inflate_changed(plain, plain->size - 4, (unsigned char)(plain->data[plain->size - 4] ^ 1u)),
        ==, TB_DEFLATE_CHECKSUM);
    /* A byte of the file name changes the header's CRC-16. */
    TB_TEST_EXPECT_UINT(inflate_changed(fields, 20, 'X'), ==, TB_DEFLATE_CHECKSUM);
}

/* -------------------------------------------------------------------------
 * The inflater's life.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_inflate, a_failure_is_kept_until_a_reset)
{
    static const unsigned char reserved[1] = {0x07};
    const deflate_vector *const vector = find_vector("raw_text_level6");
    tb_inflater *const inflater = tb_inflater_create(TB_DEFLATE_RAW, NULL);
    unsigned char *const output = tb_malloc(vector->length);
    size_t used = 0;
    size_t written = 0;
    TB_TEST_EXPECT_UINT(tb_inflater_run(inflater, reserved, 1, &used, output, 16, &written), ==,
                        TB_DEFLATE_MALFORMED);
    TB_TEST_EXPECT(!tb_inflater_finished(inflater));
    TB_TEST_EXPECT_UINT(tb_inflater_run(inflater, vector->data, vector->size, &used, output,
                                        vector->length, &written),
                        ==, TB_DEFLATE_MALFORMED);
    TB_TEST_EXPECT_UINT(used, ==, 0);
    TB_TEST_EXPECT_UINT(written, ==, 0);
    tb_inflater_reset(inflater);
    TB_TEST_EXPECT_UINT(tb_inflater_run(inflater, vector->data, vector->size, &used, output,
                                        vector->length, &written),
                        ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT(tb_inflater_finished(inflater));
    TB_TEST_EXPECT_UINT(written, ==, vector->length);
    /* An inflater can be used for one stream after another. */
    tb_inflater_reset(inflater);
    TB_TEST_EXPECT_UINT(tb_inflater_run(inflater, vector->data, vector->size, &used, output,
                                        vector->length, &written),
                        ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT(tb_inflater_finished(inflater));
    tb_free(output);
    tb_inflater_destroy(inflater);
    tb_inflater_destroy(NULL);
}

TB_TEST(codec_inflate, an_unknown_format_is_invalid_for_the_one_call_form)
{
    unsigned char output[1] = {0};
    size_t written = 0;
    TB_TEST_EXPECT_UINT(tb_inflate((tb_deflate_format)9, NULL, 0, output, 1, &written), ==,
                        TB_DEFLATE_INVALID);
}

/* -------------------------------------------------------------------------
 * The decompressing stream.
 * ------------------------------------------------------------------------- */

static tb_stream_result inflate_through_streams(const unsigned char *data, size_t size,
                                                tb_deflate_format format, uint64_t limit,
                                                tb_stream *sink, uint64_t *copied)
{
    tb_stream source = {0};
    tb_stream inflating = {0};
    unsigned char buffer[700] = {0};
    tb_stream_result result = TB_STREAM_OK;
    tb_stream_open_memory(data, size, NULL, &source);
    TB_TEST_REQUIRE_UINT(tb_inflate_open_stream(&source, format, NULL, &inflating), ==,
                         TB_STREAM_OK);
    result = tb_stream_copy(&inflating, sink, buffer, sizeof(buffer), limit, copied);
    TB_TEST_EXPECT_UINT(tb_stream_close(&inflating), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&source), ==, TB_STREAM_OK);
    return result;
}

TB_TEST(codec_inflate, a_stream_decompresses_every_vector_and_every_gzip_member)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(deflate_vectors); row++)
    {
        const deflate_vector *const vector = &deflate_vectors[row];
        tb_stream sink = {0};
        uint64_t copied = 0;
        size_t length = 0;
        size_t sink_length = 0;
        const unsigned char *bytes = NULL;
        unsigned char *const expected = expected_output(vector, &length);
        TB_TEST_CONTEXT("%s", vector->name);
        tb_stream_open_buffer(NULL, &sink);
        TB_TEST_EXPECT_UINT(inflate_through_streams(vector->data, vector->size, vector->format,
                                                    UINT64_MAX, &sink, &copied),
                            ==, TB_STREAM_OK);
        bytes = tb_stream_buffer_bytes(&sink, &sink_length);
        TB_TEST_EXPECT_UINT(sink_length, ==, length);
        TB_TEST_EXPECT_BYTES(bytes, expected, length);
        TB_TEST_EXPECT_UINT(tb_stream_close(&sink), ==, TB_STREAM_OK);
        tb_free(expected);
    }
}

TB_TEST(codec_inflate, a_stream_reports_damage_truncation_and_its_limit)
{
    const deflate_vector *const gzip = find_vector("gzip_text_plain");
    const deflate_vector *const zeros = find_vector("zlib_zeros_level9");
    unsigned char *const padded = tb_malloc(gzip->size + 3);
    tb_stream sink = {0};
    uint64_t copied = 0;
    tb_stream_open_buffer(NULL, &sink);

    /* A source that ends early. */
    TB_TEST_EXPECT_UINT(inflate_through_streams(gzip->data, gzip->size - 1, gzip->format,
                                                UINT64_MAX, &sink, &copied),
                        ==, TB_STREAM_END);
    /* Bytes after a gzip member that do not start another. */
    memcpy(padded, gzip->data, gzip->size);
    memcpy(padded + gzip->size, "\0\0\0", 3);
    TB_TEST_EXPECT_UINT(
        inflate_through_streams(padded, gzip->size + 3, gzip->format, UINT64_MAX, &sink, &copied),
        ==, TB_STREAM_MALFORMED);
    /* A damaged checksum. */
    memcpy(padded, gzip->data, gzip->size);
    padded[gzip->size - 8] ^= 1u;
    TB_TEST_EXPECT_UINT(
        inflate_through_streams(padded, gzip->size, gzip->format, UINT64_MAX, &sink, &copied), ==,
        TB_STREAM_MALFORMED);
    /* 100,000 zeros from 120 bytes stop at the limit the caller sets. */
    TB_TEST_EXPECT_UINT(
        inflate_through_streams(zeros->data, zeros->size, zeros->format, 1000, &sink, &copied), ==,
        TB_STREAM_TOO_LARGE);
    TB_TEST_EXPECT_UINT(copied, ==, 1000);
    TB_TEST_EXPECT_UINT(tb_stream_close(&sink), ==, TB_STREAM_OK);
    tb_free(padded);
}

TB_TEST(codec_inflate, a_stream_needs_a_source_that_reads_and_a_known_format)
{
    static const tb_stream_functions write_only = {.release = NULL};
    tb_stream source = {&write_only, NULL};
    tb_stream stream = {0};
    TB_TEST_EXPECT_UINT(tb_inflate_open_stream(&source, TB_DEFLATE_RAW, NULL, &stream), ==,
                        TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_inflate_open_stream(&source, (tb_deflate_format)7, NULL, &stream), ==,
                        TB_STREAM_INVALID);
    TB_TEST_EXPECT_POINTER(stream.functions, ==, NULL);
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void inflate_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    tb_inflater *inflater = NULL;
    unsigned char byte = 0;
    size_t count = 0;
    switch (choice)
    {
    case 0:
        inflater = tb_inflater_create((tb_deflate_format)5, NULL);
        break;
    case 1:
        (void)tb_inflater_run(NULL, &byte, 1, &count, &byte, 1, &count);
        break;
    case 2:
        (void)tb_inflate(TB_DEFLATE_RAW, NULL, 1, &byte, 1, &count);
        break;
    case 3:
        (void)tb_inflate(TB_DEFLATE_RAW, &byte, 1, NULL, 1, &count);
        break;
    case 4:
        (void)tb_inflate(TB_DEFLATE_RAW, &byte, 1, &byte, 1, NULL);
        break;
    case 5:
        tb_inflater_reset(NULL);
        break;
    default:
        (void)tb_inflater_finished(NULL);
        break;
    }
    tb_inflater_destroy(inflater);
}

TB_TEST(codec_inflate, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 7; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(inflate_contract, &choice, "assertion failed");
    }
}
