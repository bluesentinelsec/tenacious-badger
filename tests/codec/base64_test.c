/* Base64: the vectors of RFC 4648 and of an independent implementation, round
 * trips in every form, buffers of exactly the size and one short, and strict
 * rejection of every malformed text, with the offset of the fault. */
#include "badger/assert.h"
#include "badger/base64.h"
#include "badger/str.h"
#include "base64_vectors.h"
#include "tb_test.h"

#include <string.h>

static const unsigned forms[4] = {0, TB_BASE64_NO_PADDING, TB_BASE64_URL_SAFE,
                                  TB_BASE64_URL_SAFE | TB_BASE64_NO_PADDING};

/* Encode in one form and compare with `expected`; decode it back. */
static void check_vector(const unsigned char *bytes, size_t length, unsigned flags,
                         const char *expected)
{
    char text[512] = {0};
    unsigned char decoded[512] = {0};
    const size_t expected_length = tb_str_length(expected);
    size_t written = 0;
    size_t size = 0;
    TB_TEST_REQUIRE_UINT(tb_base64_encoded_size(length, flags, &size), ==, TB_BASE64_OK);
    TB_TEST_EXPECT_UINT(size, ==, expected_length);
    TB_TEST_REQUIRE_UINT(tb_base64_encode(bytes, length, flags, text, sizeof(text), &written), ==,
                         TB_BASE64_OK);
    TB_TEST_EXPECT_UINT(written, ==, expected_length);
    TB_TEST_EXPECT_BYTES(text, expected, expected_length);
    TB_TEST_REQUIRE_UINT(tb_base64_decode(expected, expected_length, flags, decoded,
                                          sizeof(decoded), &written, NULL),
                         ==, TB_BASE64_OK);
    TB_TEST_EXPECT_UINT(written, ==, length);
    TB_TEST_EXPECT_BYTES(decoded, bytes, length);
    TB_TEST_EXPECT_UINT(written, <=, tb_base64_decoded_size_bound(expected_length));
}

TB_TEST(codec_base64, the_vectors_of_rfc_4648)
{
    static const char *const rows[][2] = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("\"%s\"", rows[row][0]);
        check_vector((const unsigned char *)rows[row][0], tb_str_length(rows[row][0]), 0,
                     rows[row][1]);
    }
}

TB_TEST(codec_base64, agrees_with_an_independent_implementation)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(base64_vectors); row++)
    {
        const base64_vector *const vector = &base64_vectors[row];
        TB_TEST_CONTEXT("length %zu", vector->length);
        check_vector(vector->bytes, vector->length, forms[0], vector->standard);
        check_vector(vector->bytes, vector->length, forms[1], vector->standard_unpadded);
        check_vector(vector->bytes, vector->length, forms[2], vector->url);
        check_vector(vector->bytes, vector->length, forms[3], vector->url_unpadded);
    }
}

/* Random bytes of every length to 1024 survive a round trip in every form. */
TB_TEST(codec_base64, every_length_round_trips_in_every_form)
{
    enum
    {
        LONGEST = 1024
    };
    unsigned char bytes[LONGEST] = {0};
    char text[2 * LONGEST] = {0};
    unsigned char decoded[LONGEST + 4] = {0};
    tb_test_random_bytes(bytes, sizeof(bytes));
    for (size_t length = 0; length <= LONGEST; length++)
    {
        for (size_t form = 0; form < 4; form++)
        {
            size_t text_length = 0;
            size_t decoded_length = 0;
            TB_TEST_CONTEXT("length %zu, form %zu", length, form);
            TB_TEST_REQUIRE_UINT(
                tb_base64_encode(bytes, length, forms[form], text, sizeof(text), &text_length), ==,
                TB_BASE64_OK);
            TB_TEST_REQUIRE_UINT(tb_base64_decode(text, text_length, forms[form], decoded,
                                                  sizeof(decoded), &decoded_length, NULL),
                                 ==, TB_BASE64_OK);
            TB_TEST_REQUIRE_UINT(decoded_length, ==, length);
            TB_TEST_REQUIRE_BYTES(decoded, bytes, length);
        }
    }
}

/* Every byte value, alone and in each position of a group of three. */
TB_TEST(codec_base64, every_byte_value_in_every_position)
{
    for (unsigned value = 0; value < 256; value++)
    {
        for (size_t position = 0; position < 3; position++)
        {
            unsigned char bytes[3] = {0x5A, 0xA5, 0x3C};
            char text[8] = {0};
            unsigned char decoded[4] = {0};
            size_t text_length = 0;
            size_t decoded_length = 0;
            bytes[position] = (unsigned char)value;
            TB_TEST_REQUIRE_UINT(
                tb_base64_encode(bytes, position + 1, 0, text, sizeof(text), &text_length), ==,
                TB_BASE64_OK);
            TB_TEST_REQUIRE_UINT(tb_base64_decode(text, text_length, 0, decoded, sizeof(decoded),
                                                  &decoded_length, NULL),
                                 ==, TB_BASE64_OK);
            TB_TEST_REQUIRE_UINT(decoded_length, ==, position + 1);
            TB_TEST_REQUIRE_BYTES(decoded, bytes, position + 1);
        }
    }
}

/* Exactly the room needed is enough; one less reports the need and writes nothing. */
TB_TEST(codec_base64, buffers_one_short_report_what_they_need)
{
    static const unsigned char bytes[5] = {1, 2, 3, 4, 5};
    char text[16] = {0};
    unsigned char decoded[8] = {0};
    size_t written = 0;
    memset(text, '#', sizeof(text));
    TB_TEST_EXPECT_UINT(tb_base64_encode(bytes, 5, 0, text, 7, &written), ==,
                        TB_BASE64_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_UINT(written, ==, 8);
    TB_TEST_EXPECT_UINT((unsigned char)text[0], ==, '#');
    TB_TEST_EXPECT_UINT(tb_base64_encode(bytes, 5, 0, text, 8, &written), ==, TB_BASE64_OK);
    TB_TEST_EXPECT_BYTES(text, "AQIDBAU=", 8);
    TB_TEST_EXPECT_UINT((unsigned char)text[8], ==, '#');
    memset(decoded, 0xEE, sizeof(decoded));
    TB_TEST_EXPECT_UINT(tb_base64_decode("AQIDBAU=", 8, 0, decoded, 4, &written, NULL), ==,
                        TB_BASE64_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_UINT(written, ==, 5);
    TB_TEST_EXPECT_UINT(decoded[0], ==, 0xEE);
    TB_TEST_EXPECT_UINT(tb_base64_decode("AQIDBAU=", 8, 0, decoded, 5, &written, NULL), ==,
                        TB_BASE64_OK);
    TB_TEST_EXPECT_BYTES(decoded, bytes, 5);
    TB_TEST_EXPECT_UINT(decoded[5], ==, 0xEE);
    /* No room is a way to measure. */
    TB_TEST_EXPECT_UINT(tb_base64_decode("AQIDBAU=", 8, 0, NULL, 0, &written, NULL), ==,
                        TB_BASE64_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_UINT(written, ==, 5);
}

static void expect_invalid(const char *text, unsigned flags, size_t offset)
{
    unsigned char decoded[32] = {0};
    size_t written = 99;
    size_t error_offset = 1234;
    TB_TEST_CONTEXT("\"%s\", flags %u", text, flags);
    TB_TEST_EXPECT_UINT(tb_base64_decode(text, tb_str_length(text), flags, decoded, sizeof(decoded),
                                         &written, &error_offset),
                        ==, TB_BASE64_INVALID);
    TB_TEST_EXPECT_UINT(written, ==, 0);
    TB_TEST_EXPECT_UINT(error_offset, ==, offset);
}

TB_TEST(codec_base64, malformed_text_is_rejected_with_its_offset)
{
    /* Characters outside the alphabet. */
    expect_invalid("Zm9v!mFy", 0, 4);
    expect_invalid("Zm9\x80", 0, 3);
    /* Each alphabet's two own characters, given to the other. */
    expect_invalid("ab+/", TB_BASE64_URL_SAFE, 2);
    expect_invalid("ab-_", 0, 2);
    /* Padding in the middle, too much, too little, or where none belongs. */
    expect_invalid("Zg==Zm8=", 0, 4);
    expect_invalid("Z===", 0, 1);
    expect_invalid("Zm9v=", 0, 4);
    expect_invalid("Zg=", 0, 3);
    expect_invalid("Zg", 0, 2);
    expect_invalid("Zg==", TB_BASE64_NO_PADDING, 2);
    expect_invalid("Zm8=", TB_BASE64_NO_PADDING, 3);
    /* A last group of one character holds no byte. */
    expect_invalid("Zm9vY", TB_BASE64_NO_PADDING, 5);
    /* Non-zero bits past the last byte: "Zh==" would also be "f". */
    expect_invalid("Zh==", 0, 1);
    expect_invalid("Zm9=", 0, 2);
    expect_invalid("Zh", TB_BASE64_NO_PADDING, 1);
    /* Whitespace, unless asked to skip it. */
    expect_invalid("Zm9v Ym", 0, 4);
    expect_invalid("Zm9v\n", 0, 4);
    /* Unknown flags. */
    {
        size_t written = 0;
        size_t size = 0;
        TB_TEST_EXPECT_UINT(tb_base64_decode("Zg==", 4, 8, NULL, 0, &written, NULL), ==,
                            TB_BASE64_INVALID);
        TB_TEST_EXPECT_UINT(tb_base64_encoded_size(1, 64, &size), ==, TB_BASE64_INVALID);
    }
}

/* A bad character at every position of a valid text is found where it is. */
TB_TEST(codec_base64, a_bad_character_is_found_at_every_position)
{
    static const char valid[] = "Zm9vYmFyYmF6cXV4";
    for (size_t position = 0; position < sizeof(valid) - 1; position++)
    {
        char text[sizeof(valid)] = {0};
        memcpy(text, valid, sizeof(valid));
        text[position] = '*';
        expect_invalid(text, 0, position);
    }
}

TB_TEST(codec_base64, whitespace_is_skipped_anywhere_when_asked)
{
    static const char *const texts[] = {" Zm9vYmFy", "Zm9v\r\nYmFy", "Z m 9 v Y m F y",
                                        "Zm9vYmFy\n", "\tZm9v\vYm\fFy  "};
    for (size_t row = 0; row < TB_TEST_COUNT_OF(texts); row++)
    {
        unsigned char decoded[8] = {0};
        size_t written = 0;
        TB_TEST_CONTEXT("row %zu", row);
        TB_TEST_EXPECT_UINT(tb_base64_decode(texts[row], tb_str_length(texts[row]),
                                             TB_BASE64_SKIP_WHITESPACE, decoded, sizeof(decoded),
                                             &written, NULL),
                            ==, TB_BASE64_OK);
        TB_TEST_EXPECT_UINT(written, ==, 6);
        TB_TEST_EXPECT_BYTES(decoded, "foobar", 6);
    }
    /* Whitespace between padding characters, and after them. */
    {
        unsigned char decoded[4] = {0};
        size_t written = 0;
        TB_TEST_EXPECT_UINT(tb_base64_decode("Zg= =\n", 6, TB_BASE64_SKIP_WHITESPACE, decoded,
                                             sizeof(decoded), &written, NULL),
                            ==, TB_BASE64_OK);
        TB_TEST_EXPECT_UINT(written, ==, 1);
    }
}

TB_TEST(codec_base64, sizes_near_the_limit_are_reported)
{
    size_t size = 0;
    TB_TEST_EXPECT_UINT(tb_base64_encoded_size(SIZE_MAX, 0, &size), ==, TB_BASE64_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_base64_encoded_size(SIZE_MAX / 4 * 3, 0, &size), ==,
                        TB_BASE64_TOO_LARGE);
    TB_TEST_EXPECT_UINT(tb_base64_encoded_size((SIZE_MAX - 4) / 4 * 3, 0, &size), ==, TB_BASE64_OK);
    TB_TEST_EXPECT_UINT(size, ==, (SIZE_MAX - 4) / 4 * 4);
    TB_TEST_EXPECT_UINT(tb_base64_decoded_size_bound(SIZE_MAX), <=, SIZE_MAX / 4 * 3 + 3);
    TB_TEST_EXPECT_UINT(tb_base64_decoded_size_bound(0), ==, 0);
    TB_TEST_EXPECT_UINT(tb_base64_decoded_size_bound(4), ==, 3);
}

TB_TEST(codec_base64, the_allocating_forms_own_their_results)
{
    char *text = NULL;
    unsigned char *bytes = NULL;
    size_t length = 0;
    size_t offset = 0;
    TB_TEST_REQUIRE_UINT(
        tb_base64_encode_alloc("foobar", 6, TB_BASE64_URL_SAFE, NULL, &text, &length), ==,
        TB_BASE64_OK);
    TB_TEST_EXPECT_STRING(text, ==, "Zm9vYmFy");
    TB_TEST_REQUIRE_UINT(
        tb_base64_decode_alloc(text, length, TB_BASE64_URL_SAFE, NULL, &bytes, &length, NULL), ==,
        TB_BASE64_OK);
    TB_TEST_EXPECT_UINT(length, ==, 6);
    TB_TEST_EXPECT_STRING((const char *)bytes, ==, "foobar");
    tb_allocator_free(NULL, bytes, length + 1, 1);
    tb_allocator_free(NULL, text, 8 + 1, 1);
    TB_TEST_EXPECT_UINT(tb_base64_decode_alloc("Zg=", 3, 0, NULL, &bytes, &length, &offset), ==,
                        TB_BASE64_INVALID);
    TB_TEST_EXPECT_POINTER(bytes, ==, NULL);
    TB_TEST_EXPECT_UINT(offset, ==, 3);
    /* Nothing encodes to nothing, which still ends with a NUL. */
    TB_TEST_REQUIRE_UINT(tb_base64_encode_alloc(NULL, 0, 0, NULL, &text, &length), ==,
                         TB_BASE64_OK);
    TB_TEST_EXPECT_UINT(length, ==, 0);
    TB_TEST_EXPECT_STRING(text, ==, "");
    tb_allocator_free(NULL, text, 1, 1);
}

static void base64_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    char text[4] = {0};
    size_t written = 0;
    switch (choice)
    {
    case 0:
        (void)tb_base64_encode(NULL, 1, 0, text, 4, &written);
        break;
    case 1:
        (void)tb_base64_encode("a", 1, 0, NULL, 4, &written);
        break;
    case 2:
        (void)tb_base64_encode("a", 1, 0, text, 4, NULL);
        break;
    case 3:
        (void)tb_base64_decode(NULL, 1, 0, text, 4, &written, NULL);
        break;
    case 4:
        (void)tb_base64_decode("Zg==", 4, 0, text, 4, NULL, NULL);
        break;
    default:
        (void)tb_base64_encoded_size(1, 0, NULL);
        break;
    }
}

TB_TEST(codec_base64, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = 0; choice < 6; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(base64_contract, &choice, "assertion failed");
    }
}
