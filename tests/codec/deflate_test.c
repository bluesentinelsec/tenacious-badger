/* DEFLATE compression: what is compressed at every level and in every framing
 * decompresses to what it was; the bytes do not depend on how the input was
 * split; flushes end on a byte that a reader can decode up to; and the codes
 * of a block stay within 15 bits however skewed the data. */
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/memory.h"
#include "tb_test.h"

#include <string.h>

static const tb_deflate_format formats[] = {TB_DEFLATE_RAW, TB_DEFLATE_ZLIB, TB_DEFLATE_GZIP};

static void random_bytes(unsigned char *output, size_t length, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t index = 0; index < length; index++)
    {
        state = state * 1103515245u + 12345u;
        output[index] = (unsigned char)(state >> 16);
    }
}

/* Words from a small vocabulary in a random order: compressible like prose. */
static void text_bytes(unsigned char *output, size_t length, uint32_t seed)
{
    static const char *const words[] = {"the ",     "engine ", "badger ", "renders ", "a ",
                                        "frame ",   "of ",     "light ",  "while ",   "jobs ",
                                        "steal ",   "work. ",  "Every ",  "burrow ",  "has ",
                                        "tunnels ", "\n"};
    uint32_t state = seed;
    size_t done = 0;
    while (done < length)
    {
        const char *word = NULL;
        state = state * 1103515245u + 12345u;
        word = words[(state >> 16) % TB_TEST_COUNT_OF(words)];
        while (*word != '\0' && done < length)
        {
            output[done] = (unsigned char)*word;
            done++;
            word++;
        }
    }
}

/* Compress in one call, decompress in one call, and compare. Returns the size. */
static size_t round_trip(tb_deflate_format format, int level, const unsigned char *input,
                         size_t length)
{
    const size_t bound = tb_deflate_bound(length);
    unsigned char *const compressed = tb_malloc(bound);
    unsigned char *const output = tb_malloc(length + 1);
    size_t compressed_length = 0;
    size_t output_length = 0;
    TB_TEST_REQUIRE_UINT(
        tb_deflate(format, level, input, length, compressed, bound, &compressed_length), ==,
        TB_DEFLATE_OK);
    TB_TEST_REQUIRE_UINT(compressed_length, <=, bound);
    TB_TEST_REQUIRE_UINT(
        tb_inflate(format, compressed, compressed_length, output, length, &output_length), ==,
        TB_DEFLATE_OK);
    TB_TEST_REQUIRE_UINT(output_length, ==, length);
    TB_TEST_REQUIRE_BYTES(output, input, length);
    tb_free(output);
    tb_free(compressed);
    return compressed_length;
}

TB_TEST(codec_deflate, every_level_and_format_round_trips)
{
    enum
    {
        LARGEST = 1 << 20
    };
    static const struct
    {
        const char *name;
        int kind; /* 0 zeros, 1 random, 2 text, 3 a short pattern repeated */
        size_t length;
    } inputs[] = {
        {"empty", 0, 0},
        {"one byte", 2, 1},
        {"1 MiB of zeros", 0, LARGEST},
        {"random", 1, 100000},
        {"text", 2, 200000},
        {"a pattern", 3, 70000},
        {"text, one less than the window", 2, 32767},
        {"text, the window", 2, 32768},
        {"text, one more than the window", 2, 32769},
        {"random, one less than a stored block", 1, 65534},
        {"random, a stored block", 1, 65535},
        {"random, one more than a stored block", 1, 65536},
        {"random, two windows and one", 1, 65537},
    };
    unsigned char *const input = tb_malloc(LARGEST);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(inputs); row++)
    {
        switch (inputs[row].kind)
        {
        case 0:
            memset(input, 0, inputs[row].length);
            break;
        case 1:
            random_bytes(input, inputs[row].length, (uint32_t)row);
            break;
        case 2:
            text_bytes(input, inputs[row].length, (uint32_t)row);
            break;
        default:
            for (size_t index = 0; index < inputs[row].length; index++)
            {
                input[index] = (unsigned char)("pattern"[index % 7]);
            }
            break;
        }
        for (size_t format = 0; format < TB_TEST_COUNT_OF(formats); format++)
        {
            for (int level = 0; level <= TB_DEFLATE_BEST_LEVEL; level++)
            {
                TB_TEST_CONTEXT("%s, format %zu, level %d", inputs[row].name, format, level);
                (void)round_trip(formats[format], level, input, inputs[row].length);
            }
        }
    }
    tb_free(input);
}

/* Compression shrinks what can shrink, and stores what cannot. */
TB_TEST(codec_deflate, sizes_are_what_each_kind_of_data_allows)
{
    enum
    {
        LENGTH = 1 << 20
    };
    unsigned char *const input = tb_malloc(LENGTH);
    size_t fast = 0;
    size_t best = 0;
    memset(input, 0, LENGTH);
    /* A run of 258 bytes takes a few bits, so 1 MiB of zeros is about 1 KiB. */
    TB_TEST_EXPECT_UINT(round_trip(TB_DEFLATE_RAW, TB_DEFLATE_DEFAULT_LEVEL, input, LENGTH), <,
                        2048);
    /* Level 0 stores: a little more than the input. */
    TB_TEST_EXPECT_UINT(round_trip(TB_DEFLATE_RAW, 0, input, LENGTH), >, LENGTH);
    random_bytes(input, LENGTH, 7u);
    /* Random bytes are stored, at the cost of five bytes per block. */
    TB_TEST_EXPECT_UINT(round_trip(TB_DEFLATE_RAW, TB_DEFLATE_BEST_LEVEL, input, LENGTH), <,
                        LENGTH + LENGTH / 1000);
    text_bytes(input, LENGTH, 7u);
    fast = round_trip(TB_DEFLATE_RAW, 1, input, LENGTH);
    best = round_trip(TB_DEFLATE_RAW, TB_DEFLATE_BEST_LEVEL, input, LENGTH);
    TB_TEST_EXPECT_UINT(fast, <, LENGTH / 2);
    TB_TEST_EXPECT_UINT(best, <=, fast);
    tb_free(input);
}

/* Compress through a deflater fed pieces of the given largest sizes, at random
 * or exactly; return the stream in a block from tb_malloc. */
static unsigned char *compress_in_pieces(tb_deflate_format format, int level,
                                         const unsigned char *input, size_t length,
                                         size_t input_piece, size_t output_piece, bool random,
                                         size_t *compressed_length)
{
    const size_t bound = tb_deflate_bound(length);
    unsigned char *const output = tb_malloc(bound);
    tb_deflater *const deflater = tb_deflater_create(format, level, NULL);
    size_t taken = 0;
    size_t produced = 0;
    while (!tb_deflater_finished(deflater))
    {
        size_t give = random ? 1 + (size_t)tb_test_random_below(input_piece) : input_piece;
        size_t room = random ? 1 + (size_t)tb_test_random_below(output_piece) : output_piece;
        size_t used = 0;
        size_t written = 0;
        tb_deflate_flush flush = TB_DEFLATE_NO_FLUSH;
        if (give >= length - taken)
        {
            give = length - taken;
            flush = TB_DEFLATE_FINISH;
        }
        if (room > bound - produced)
        {
            room = bound - produced;
        }
        TB_TEST_REQUIRE_UINT(tb_deflater_run(deflater, input + taken, give, &used,
                                             output + produced, room, &written, flush),
                             ==, TB_DEFLATE_OK);
        TB_TEST_REQUIRE_UINT(used, <=, give);
        TB_TEST_REQUIRE_UINT(written, <=, room);
        taken += used;
        produced += written;
        TB_TEST_REQUIRE(used > 0 || written > 0 || tb_deflater_finished(deflater));
    }
    TB_TEST_EXPECT_UINT(taken, ==, length);
    tb_deflater_destroy(deflater);
    *compressed_length = produced;
    return output;
}

/* The same input gives the same bytes, a byte at a time or all at once. */
TB_TEST(codec_deflate, the_output_does_not_depend_on_the_split)
{
    enum
    {
        LENGTH = 100000
    };
    static const int levels[] = {0, 1, 4, 6, 9};
    unsigned char *const input = tb_malloc(LENGTH);
    unsigned char *const whole = tb_malloc(tb_deflate_bound(LENGTH));
    text_bytes(input, LENGTH, 3u);
    /* Random bytes in the middle make blocks of every form. */
    random_bytes(input + 40000, 5000, 3u);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(levels); row++)
    {
        size_t whole_length = 0;
        TB_TEST_CONTEXT("level %d", levels[row]);
        TB_TEST_REQUIRE_UINT(tb_deflate(TB_DEFLATE_ZLIB, levels[row], input, LENGTH, whole,
                                        tb_deflate_bound(LENGTH), &whole_length),
                             ==, TB_DEFLATE_OK);
        for (size_t trial = 0; trial < 3; trial++)
        {
            size_t length = 0;
            unsigned char *pieces = NULL;
            if (trial == 0)
            {
                pieces = compress_in_pieces(TB_DEFLATE_ZLIB, levels[row], input, LENGTH, 1, 1,
                                            false, &length);
            }
            else
            {
                pieces = compress_in_pieces(TB_DEFLATE_ZLIB, levels[row], input, LENGTH, 5000, 700,
                                            true, &length);
            }
            TB_TEST_EXPECT_UINT(length, ==, whole_length);
            TB_TEST_EXPECT_BYTES(pieces, whole, whole_length);
            tb_free(pieces);
        }
    }
    tb_free(whole);
    tb_free(input);
}

/* Records of a little-endian counter, a tag, and padding: binary data with
 * matches at fixed distances and lengths of every size. */
static void record_bytes(unsigned char *output, size_t length, uint32_t seed)
{
    uint32_t state = seed;
    for (size_t index = 0; index < length; index++)
    {
        const size_t record = index / 24;
        const size_t field = index % 24;
        if (field < 4)
        {
            output[index] = (unsigned char)(record >> (8 * field));
        }
        else if (field < 8)
        {
            state = state * 1103515245u + 12345u;
            output[index] = (unsigned char)("ABCD"[(state >> 16) % 4]);
        }
        else
        {
            output[index] = (unsigned char)(field * 7);
        }
    }
}

/* The exact bytes of every level, held by their CRC-32 and length, so a
 * change made for speed cannot change a single byte of the output. The
 * inputs make every form of block, slide the window many times, and reach
 * every length and distance code. Should the output ever change on purpose,
 * these are the numbers to update, with the reason in the change. */
TB_TEST(codec_deflate, the_output_of_every_level_stays_the_same)
{
    enum
    {
        LENGTH = 400000,
        KINDS = 4,
        LEVELS = 10
    };
    static const uint32_t expected_crc[KINDS][LEVELS] = {
        {0x8D98CEF2u, 0xA8AC292Du, 0x75A629B6u, 0x65A16A16u, 0x8F82D2B2u, 0x2B97F8D2u, 0xD36FC09Eu,
         0xC6875476u, 0x8359B76Au, 0x8359B76Au},
        {0x79B8A7A1u, 0x6189002Cu, 0x6189002Cu, 0x6189002Cu, 0x25BFAE24u, 0x25BFAE24u, 0x25BFAE24u,
         0x25BFAE24u, 0x25BFAE24u, 0x25BFAE24u},
        {0x78789425u, 0x5986AC94u, 0x4738D4FBu, 0xDD6631FFu, 0xD0AB4B83u, 0x6780A992u, 0x834737C3u,
         0x90DBA96Bu, 0x5FF4FA16u, 0x5FF4FA16u},
        {0x1786C7FFu, 0xD179D74Au, 0x195FFAC2u, 0x50D7DB06u, 0xA9EF4DA8u, 0x2AD62544u, 0xF642EBA9u,
         0xE1680C6Du, 0xB5E30648u, 0xB5E30648u}};
    static const size_t expected_length[KINDS][LEVELS] = {
        {400175u, 88222u, 83862u, 72392u, 78045u, 72182u, 67001u, 64567u, 63210u, 63210u},
        {400175u, 400125u, 400125u, 400125u, 400125u, 400125u, 400125u, 400125u, 400125u, 400125u},
        {400175u, 79224u, 77301u, 73336u, 75295u, 73692u, 67894u, 65354u, 65117u, 65117u},
        {400175u, 95977u, 93039u, 85887u, 89395u, 85450u, 82129u, 80905u, 80455u, 80455u}};
    static const char *const kinds[KINDS] = {"text", "random", "records", "mixed"};
    unsigned char *const input = tb_malloc(LENGTH);
    const size_t bound = tb_deflate_bound(LENGTH);
    unsigned char *const output = tb_malloc(bound);
    for (size_t kind = 0; kind < KINDS; kind++)
    {
        switch (kind)
        {
        case 0:
            text_bytes(input, LENGTH, 11u);
            break;
        case 1:
            random_bytes(input, LENGTH, 11u);
            break;
        case 2:
            record_bytes(input, LENGTH, 11u);
            break;
        default:
            text_bytes(input, LENGTH, 13u);
            random_bytes(input + 100000, 30000, 13u);
            memset(input + 200000, 'z', 70000);
            record_bytes(input + 300000, 60000, 13u);
            break;
        }
        for (int level = 0; level < LEVELS; level++)
        {
            size_t written = 0;
            TB_TEST_CONTEXT("%s at level %d", kinds[kind], level);
            TB_TEST_REQUIRE_UINT(
                tb_deflate(TB_DEFLATE_RAW, level, input, LENGTH, output, bound, &written), ==,
                TB_DEFLATE_OK);
            TB_TEST_EXPECT_UINT(written, ==, expected_length[kind][level]);
            TB_TEST_EXPECT_UINT(tb_crc32(output, written), ==, expected_crc[kind][level]);
        }
    }
    tb_free(output);
    tb_free(input);
}

/* After a sync flush, a reader decodes everything written so far. */
TB_TEST(codec_deflate, a_sync_flush_lets_a_reader_decode_what_came_before)
{
    enum
    {
        LENGTH = 50000,
        FIRST = 12345,
        ROOM = 120000
    };
    unsigned char *const input = tb_malloc(LENGTH);
    unsigned char *const compressed = tb_malloc(ROOM);
    unsigned char *const output = tb_malloc(LENGTH);
    tb_deflater *const deflater = tb_deflater_create(TB_DEFLATE_ZLIB, 6, NULL);
    tb_inflater *const inflater = tb_inflater_create(TB_DEFLATE_ZLIB, NULL);
    size_t used = 0;
    size_t written = 0;
    size_t total = 0;
    size_t decoded = 0;
    text_bytes(input, LENGTH, 11u);
    TB_TEST_REQUIRE_UINT(tb_deflater_run(deflater, input, FIRST, &used, compressed, ROOM, &written,
                                         TB_DEFLATE_SYNC_FLUSH),
                         ==, TB_DEFLATE_OK);
    TB_TEST_REQUIRE_UINT(used, ==, FIRST);
    total = written;
    /* A sync flush ends with an empty stored block: 00 00 FF FF. */
    TB_TEST_REQUIRE_UINT(total, >=, 4);
    TB_TEST_EXPECT_BYTES(compressed + total - 4, "\x00\x00\xFF\xFF", 4);
    /* A second flush with nothing new adds nothing. */
    TB_TEST_EXPECT_UINT(tb_deflater_run(deflater, NULL, 0, &used, compressed + total, ROOM - total,
                                        &written, TB_DEFLATE_SYNC_FLUSH),
                        ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(written, ==, 0);
    TB_TEST_REQUIRE_UINT(
        tb_inflater_run(inflater, compressed, total, &used, output, LENGTH, &decoded), ==,
        TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(used, ==, total);
    TB_TEST_EXPECT_UINT(decoded, ==, FIRST);
    TB_TEST_EXPECT_BYTES(output, input, FIRST);
    TB_TEST_EXPECT(!tb_inflater_finished(inflater));
    /* The rest, finished. */
    TB_TEST_REQUIRE_UINT(tb_deflater_run(deflater, input + FIRST, LENGTH - FIRST, &used,
                                         compressed + total, ROOM - total, &written,
                                         TB_DEFLATE_FINISH),
                         ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT(tb_deflater_finished(deflater));
    TB_TEST_REQUIRE_UINT(tb_inflater_run(inflater, compressed + total, written, &used,
                                         output + FIRST, LENGTH - FIRST, &decoded),
                         ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT(tb_inflater_finished(inflater));
    TB_TEST_EXPECT_UINT(decoded, ==, LENGTH - FIRST);
    TB_TEST_EXPECT_BYTES(output, input, LENGTH);
    tb_inflater_destroy(inflater);
    tb_deflater_destroy(deflater);
    tb_free(output);
    tb_free(compressed);
    tb_free(input);
}

/* Frequencies that grow like the Fibonacci numbers would want codes far
 * longer than 15 bits; the encoder limits them, and the decoder accepts. */
TB_TEST(codec_deflate, skewed_data_keeps_codes_within_fifteen_bits)
{
    enum
    {
        SYMBOLS = 25
    };
    size_t counts[SYMBOLS] = {0};
    size_t length = 0;
    unsigned char *input = NULL;
    uint32_t state = 5;
    counts[0] = 1;
    counts[1] = 1;
    for (size_t symbol = 2; symbol < SYMBOLS; symbol++)
    {
        counts[symbol] = counts[symbol - 1] + counts[symbol - 2];
    }
    for (size_t symbol = 0; symbol < SYMBOLS; symbol++)
    {
        length += counts[symbol];
    }
    input = tb_malloc(length);
    {
        size_t at = 0;
        for (size_t symbol = 0; symbol < SYMBOLS; symbol++)
        {
            memset(input + at, (int)(symbol * 7), counts[symbol]);
            at += counts[symbol];
        }
    }
    /* Shuffle, so that few matches hide the literal frequencies. */
    for (size_t index = length - 1; index > 0; index--)
    {
        unsigned char swap = 0;
        size_t other = 0;
        state = state * 1103515245u + 12345u;
        other = ((size_t)state << 8 ^ (size_t)(state >> 8)) % (index + 1);
        swap = input[index];
        input[index] = input[other];
        input[other] = swap;
    }
    for (int level = 1; level <= TB_DEFLATE_BEST_LEVEL; level += 4)
    {
        TB_TEST_CONTEXT("level %d", level);
        (void)round_trip(TB_DEFLATE_RAW, level, input, length);
    }
    tb_free(input);
}

TB_TEST(codec_deflate, the_one_call_form_reports_room_and_arguments)
{
    static const unsigned char input[1000] = {0};
    unsigned char output[8] = {0};
    size_t written = 0;
    TB_TEST_EXPECT_UINT(tb_deflate(TB_DEFLATE_ZLIB, 6, input, sizeof(input), output, 4, &written),
                        ==, TB_DEFLATE_TOO_LARGE);
    TB_TEST_EXPECT_UINT(written, ==, 4);
    TB_TEST_EXPECT_UINT(tb_deflate(TB_DEFLATE_ZLIB, 10, input, 1, output, 8, &written), ==,
                        TB_DEFLATE_INVALID);
    TB_TEST_EXPECT_UINT(tb_deflate(TB_DEFLATE_ZLIB, -1, input, 1, output, 8, &written), ==,
                        TB_DEFLATE_INVALID);
    TB_TEST_EXPECT_UINT(tb_deflate((tb_deflate_format)3, 6, input, 1, output, 8, &written), ==,
                        TB_DEFLATE_INVALID);
    TB_TEST_EXPECT_UINT(tb_deflate_bound(SIZE_MAX), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_deflate_bound(0), >, 0);
}

/* A deflater can make one stream after another, each the same. */
TB_TEST(codec_deflate, a_reset_deflater_makes_the_same_stream_again)
{
    enum
    {
        LENGTH = 40000,
        ROOM = 50000
    };
    unsigned char *const input = tb_malloc(LENGTH);
    unsigned char *const first = tb_malloc(ROOM);
    unsigned char *const second = tb_malloc(ROOM);
    tb_deflater *const deflater = tb_deflater_create(TB_DEFLATE_GZIP, 9, NULL);
    size_t used = 0;
    size_t first_length = 0;
    size_t second_length = 0;
    text_bytes(input, LENGTH, 21u);
    TB_TEST_REQUIRE_UINT(tb_deflater_run(deflater, input, LENGTH, &used, first, ROOM, &first_length,
                                         TB_DEFLATE_FINISH),
                         ==, TB_DEFLATE_OK);
    TB_TEST_REQUIRE(tb_deflater_finished(deflater));
    /* Finished: no more input is taken. */
    TB_TEST_EXPECT_UINT(tb_deflater_run(deflater, input, LENGTH, &used, second, ROOM,
                                        &second_length, TB_DEFLATE_FINISH),
                        ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(used, ==, 0);
    TB_TEST_EXPECT_UINT(second_length, ==, 0);
    tb_deflater_reset(deflater);
    TB_TEST_REQUIRE_UINT(tb_deflater_run(deflater, input, LENGTH, &used, second, ROOM,
                                         &second_length, TB_DEFLATE_FINISH),
                         ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(second_length, ==, first_length);
    TB_TEST_EXPECT_BYTES(second, first, first_length);
    TB_TEST_EXPECT_UINT(
        tb_deflater_run(deflater, NULL, 0, &used, NULL, 0, &second_length, (tb_deflate_flush)9), ==,
        TB_DEFLATE_INVALID);
    tb_deflater_destroy(deflater);
    tb_deflater_destroy(NULL);
    tb_free(second);
    tb_free(first);
    tb_free(input);
}

/* -------------------------------------------------------------------------
 * The compressing stream.
 * ------------------------------------------------------------------------- */

TB_TEST(codec_deflate, a_stream_compresses_what_is_written_into_its_sink)
{
    enum
    {
        LENGTH = 90000
    };
    unsigned char *const input = tb_malloc(LENGTH);
    unsigned char *const whole = tb_malloc(tb_deflate_bound(LENGTH));
    unsigned char *const output = tb_malloc(LENGTH);
    size_t whole_length = 0;
    size_t sink_length = 0;
    size_t decoded = 0;
    const unsigned char *bytes = NULL;
    tb_stream sink = {0};
    tb_stream compressing = {0};
    text_bytes(input, LENGTH, 31u);
    TB_TEST_REQUIRE_UINT(tb_deflate(TB_DEFLATE_GZIP, 6, input, LENGTH, whole,
                                    tb_deflate_bound(LENGTH), &whole_length),
                         ==, TB_DEFLATE_OK);
    /* Written in pieces and closed: the same bytes as one call. */
    tb_stream_open_buffer(NULL, &sink);
    TB_TEST_REQUIRE_UINT(tb_deflate_open_stream(&sink, TB_DEFLATE_GZIP, 6, NULL, &compressing), ==,
                         TB_STREAM_OK);
    for (size_t at = 0; at < LENGTH; at += 7000)
    {
        const size_t piece = LENGTH - at < 7000 ? LENGTH - at : 7000;
        TB_TEST_REQUIRE_UINT(tb_stream_write_all(&compressing, input + at, piece), ==,
                             TB_STREAM_OK);
    }
    TB_TEST_EXPECT_UINT(tb_stream_read(&compressing, output, 1, &decoded), ==,
                        TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_stream_close(&compressing), ==, TB_STREAM_OK);
    bytes = tb_stream_buffer_bytes(&sink, &sink_length);
    TB_TEST_EXPECT_UINT(sink_length, ==, whole_length);
    TB_TEST_EXPECT_BYTES(bytes, whole, whole_length);
    TB_TEST_EXPECT_UINT(tb_stream_close(&sink), ==, TB_STREAM_OK);

    /* With a flush in the middle: different bytes, the same data. */
    tb_stream_open_buffer(NULL, &sink);
    TB_TEST_REQUIRE_UINT(tb_deflate_open_stream(&sink, TB_DEFLATE_ZLIB, 9, NULL, &compressing), ==,
                         TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_write_all(&compressing, input, 30000), ==, TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_flush(&compressing), ==, TB_STREAM_OK);
    TB_TEST_REQUIRE_UINT(tb_stream_write_all(&compressing, input + 30000, LENGTH - 30000), ==,
                         TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&compressing), ==, TB_STREAM_OK);
    bytes = tb_stream_buffer_bytes(&sink, &sink_length);
    TB_TEST_REQUIRE_UINT(tb_inflate(TB_DEFLATE_ZLIB, bytes, sink_length, output, LENGTH, &decoded),
                         ==, TB_DEFLATE_OK);
    TB_TEST_EXPECT_UINT(decoded, ==, LENGTH);
    TB_TEST_EXPECT_BYTES(output, input, LENGTH);
    TB_TEST_EXPECT_UINT(tb_stream_close(&sink), ==, TB_STREAM_OK);
    tb_free(output);
    tb_free(whole);
    tb_free(input);
}

static tb_stream_result full_sink_write(void *state, const void *bytes, size_t length,
                                        size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)length;
    (void)transferred;
    return TB_STREAM_NO_SPACE;
}

static tb_stream_result nothing_to_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

TB_TEST(codec_deflate, a_stream_reports_its_sink_and_its_arguments)
{
    static const tb_stream_functions full = {.write_bytes = full_sink_write,
                                             .release = nothing_to_release};
    static const unsigned char bytes[4] = {0};
    tb_stream sink = {&full, NULL};
    tb_stream read_only = {0};
    tb_stream compressing = {0};
    /* A sink that refuses: closing, which writes the end, reports it. */
    TB_TEST_REQUIRE_UINT(tb_deflate_open_stream(&sink, TB_DEFLATE_RAW, 1, NULL, &compressing), ==,
                         TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_stream_close(&compressing), ==, TB_STREAM_NO_SPACE);
    tb_stream_open_memory(bytes, sizeof(bytes), NULL, &read_only);
    TB_TEST_EXPECT_UINT(tb_deflate_open_stream(&read_only, TB_DEFLATE_RAW, 1, NULL, &compressing),
                        ==, TB_STREAM_UNSUPPORTED);
    TB_TEST_EXPECT_UINT(tb_deflate_open_stream(&sink, TB_DEFLATE_RAW, 12, NULL, &compressing), ==,
                        TB_STREAM_INVALID);
    TB_TEST_EXPECT_UINT(tb_deflate_open_stream(&sink, (tb_deflate_format)4, 1, NULL, &compressing),
                        ==, TB_STREAM_INVALID);
    TB_TEST_EXPECT_POINTER(compressing.functions, ==, NULL);
    TB_TEST_EXPECT_UINT(tb_stream_close(&read_only), ==, TB_STREAM_OK);
}

/* -------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void deflate_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    unsigned char byte = 0;
    size_t count = 0;
    switch (choice)
    {
    case 0:
        tb_deflater_destroy(tb_deflater_create(TB_DEFLATE_RAW, 10, NULL));
        break;
    case 1:
        tb_deflater_destroy(tb_deflater_create((tb_deflate_format)5, 6, NULL));
        break;
    case 2:
        (void)tb_deflater_run(NULL, &byte, 1, &count, &byte, 1, &count, TB_DEFLATE_FINISH);
        break;
    case 3:
        (void)tb_deflate(TB_DEFLATE_RAW, 6, NULL, 1, &byte, 1, &count);
        break;
    case 4:
        (void)tb_deflate(TB_DEFLATE_RAW, 6, &byte, 1, &byte, 1, NULL);
        break;
    case 5:
        tb_deflater_reset(NULL);
        break;
    default:
        (void)tb_deflater_finished(NULL);
        break;
    }
}

TB_TEST(codec_deflate, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 7; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(deflate_contract, &choice, "assertion failed");
    }
}
