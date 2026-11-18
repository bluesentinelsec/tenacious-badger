/* CRC-32 and Adler-32: published answers, agreement with the definitions
 * computed one bit and one byte at a time, and incremental results that do
 * not depend on how the input is split. */
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/str.h"
#include "tb_test.h"

#include <string.h>

/* CRC-32 straight from its definition: one bit at a time. */
static uint32_t reference_crc32(const unsigned char *bytes, size_t length)
{
    uint32_t value = 0xFFFFFFFFu;
    for (size_t index = {0}; index < length; index++)
    {
        value ^= bytes[index];
        for (int bit = {0}; bit < 8; bit++)
        {
            const uint32_t mask = 0u - (value & 1u);
            value = (value >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~value;
}

/* Adler-32 straight from RFC 1950: both sums reduced after every byte. */
static uint32_t reference_adler32(const unsigned char *bytes, size_t length)
{
    uint32_t low = 1;
    uint32_t high = 0;
    for (size_t index = {0}; index < length; index++)
    {
        low = (low + bytes[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

TB_TEST(codec_checksum, published_answers)
{
    static const struct
    {
        const char *text;
        uint32_t crc32;
        uint32_t adler32;
    } rows[] = {
        {"", 0x00000000u, 0x00000001u},
        {"a", 0xE8B7BE43u, 0x00620062u},
        {"abc", 0x352441C2u, 0x024D0127u},
        {"123456789", 0xCBF43926u, 0x091E01DEu},
        {"Wikipedia", 0xADAAC02Eu, 0x11E60398u},
        {"The quick brown fox jumps over the lazy dog", 0x414FA339u, 0x5BDC0FDAu},
    };
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(rows); row++)
    {
        const size_t length = tb_str_length(rows[row].text);
        TB_TEST_CONTEXT("\"%s\"", rows[row].text);
        TB_TEST_EXPECT_HEX(tb_crc32(rows[row].text, length), ==, rows[row].crc32);
        TB_TEST_EXPECT_HEX(tb_adler32(rows[row].text, length), ==, rows[row].adler32);
    }
    TB_TEST_EXPECT_HEX(tb_crc32(NULL, 0), ==, TB_CRC32_INITIAL);
    TB_TEST_EXPECT_HEX(tb_adler32(NULL, 0), ==, TB_ADLER32_INITIAL);
}

/* Every length up to a few slices, at every alignment of the input, agrees
 * with the definitions: the eight-byte steps and the byte-at-a-time tail meet
 * correctly wherever the input starts and ends. */
TB_TEST(codec_checksum, every_length_and_alignment_agrees_with_the_definition)
{
    unsigned char bytes[96] = {0};
    tb_test_random_bytes(bytes, sizeof(bytes));
    for (size_t start = {0}; start < 8; start++)
    {
        for (size_t length = {0}; start + length <= sizeof(bytes); length++)
        {
            TB_TEST_CONTEXT("start %zu, length %zu", start, length);
            TB_TEST_REQUIRE_HEX(tb_crc32(bytes + start, length), ==,
                                reference_crc32(bytes + start, length));
            TB_TEST_REQUIRE_HEX(tb_adler32(bytes + start, length), ==,
                                reference_adler32(bytes + start, length));
        }
    }
}

/* Adler-32 defers its reductions for thousands of bytes. Bytes of 0xFF make
 * the sums grow fastest, so they show a run that is too long overflowing. */
TB_TEST(codec_checksum, adler32_reduces_its_sums_before_they_overflow)
{
    enum
    {
        LENGTH = 100000
    };
    static unsigned char bytes[LENGTH] = {0};
    memset(bytes, 0xFF, sizeof(bytes));
    TB_TEST_EXPECT_HEX(tb_adler32(bytes, LENGTH), ==, reference_adler32(bytes, LENGTH));
    /* Continuing from a value whose sums are both at the modulus less one. */
    TB_TEST_EXPECT_HEX(tb_adler32_update(0xFFF0FFF0u, bytes, 5553), ==,
                       tb_adler32_update(tb_adler32_update(0xFFF0FFF0u, bytes, 1), bytes, 5552));
}

/* Incremental results equal the one-call result, however the input is split. */
TB_TEST(codec_checksum, incremental_results_do_not_depend_on_the_split)
{
    enum
    {
        LENGTH = 20000
    };
    static unsigned char bytes[LENGTH] = {0};
    uint32_t crc_whole = {0};
    uint32_t adler_whole = {0};
    tb_test_random_bytes(bytes, sizeof(bytes));
    crc_whole = tb_crc32(bytes, LENGTH);
    adler_whole = tb_adler32(bytes, LENGTH);
    TB_TEST_EXPECT_HEX(crc_whole, ==, reference_crc32(bytes, LENGTH));
    TB_TEST_EXPECT_HEX(adler_whole, ==, reference_adler32(bytes, LENGTH));
    for (size_t trial = {0}; trial < 50; trial++)
    {
        uint32_t crc = TB_CRC32_INITIAL;
        uint32_t adler = TB_ADLER32_INITIAL;
        size_t done = {0};
        TB_TEST_CONTEXT("trial %zu", trial);
        while (done < LENGTH)
        {
            /* Pieces from empty to longer than an Adler-32 run. */
            size_t piece = (size_t)tb_test_random_below(7000);
            if (piece > LENGTH - done)
            {
                piece = LENGTH - done;
            }
            crc = tb_crc32_update(crc, bytes + done, piece);
            adler = tb_adler32_update(adler, bytes + done, piece);
            done += piece;
        }
        TB_TEST_REQUIRE_HEX(crc, ==, crc_whole);
        TB_TEST_REQUIRE_HEX(adler, ==, adler_whole);
    }
    /* One byte at a time. */
    {
        uint32_t crc = TB_CRC32_INITIAL;
        uint32_t adler = TB_ADLER32_INITIAL;
        for (size_t index = {0}; index < LENGTH; index++)
        {
            crc = tb_crc32_update(crc, bytes + index, 1);
            adler = tb_adler32_update(adler, bytes + index, 1);
        }
        TB_TEST_EXPECT_HEX(crc, ==, crc_whole);
        TB_TEST_EXPECT_HEX(adler, ==, adler_whole);
    }
}

static void null_with_length(void *context)
{
    const size_t choice = *(const size_t *)context;
    if (choice == 0)
    {
        (void)tb_crc32(NULL, 1);
    }
    else
    {
        (void)tb_adler32(NULL, 1);
    }
}

TB_TEST(codec_checksum, null_input_with_a_length_is_a_contract_violation)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 2; choice++)
    {
        TB_TEST_EXPECT_TRAP(null_with_length, &choice, "assertion failed");
    }
}
