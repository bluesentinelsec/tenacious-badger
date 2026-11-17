#include "badger/assert.h"
#include "badger/utf8.h"
#include "tb_test.h"
#include <string.h>

TB_TEST(core_utf8, boundary_values_have_known_encodings)
{
    static const struct
    {
        uint32_t scalar;
        unsigned char bytes[4];
        size_t length;
    } rows[] = {{0, {0}, 1},
                {0x7F, {0x7F}, 1},
                {0x80, {0xC2, 0x80}, 2},
                {0x7FF, {0xDF, 0xBF}, 2},
                {0x800, {0xE0, 0xA0, 0x80}, 3},
                {0xD7FF, {0xED, 0x9F, 0xBF}, 3},
                {0xE000, {0xEE, 0x80, 0x80}, 3},
                {0xFFFF, {0xEF, 0xBF, 0xBF}, 3},
                {0x10000, {0xF0, 0x90, 0x80, 0x80}, 4},
                {0x10FFFF, {0xF4, 0x8F, 0xBF, 0xBF}, 4}};
    for (size_t index = {0}; index < TB_TEST_COUNT_OF(rows); index++)
    {
        char bytes[5] = {0};
        uint32_t decoded = {0};
        size_t next = {0};
        TB_TEST_CONTEXT("scalar %u", rows[index].scalar);
        TB_TEST_REQUIRE_UINT(tb_utf8_encode(rows[index].scalar, bytes, 4), ==, rows[index].length);
        TB_TEST_EXPECT(memcmp(bytes, rows[index].bytes, rows[index].length) == 0);
        TB_TEST_EXPECT(tb_utf8_decode(bytes, rows[index].length, 0, &decoded, &next));
        TB_TEST_EXPECT_UINT(decoded, ==, rows[index].scalar);
        TB_TEST_EXPECT_UINT(next, ==, rows[index].length);
        for (size_t short_size = {0}; short_size < rows[index].length; short_size++)
        {
            memset(bytes, 0x55, sizeof(bytes));
            TB_TEST_EXPECT_UINT(tb_utf8_encode(rows[index].scalar, bytes, short_size), ==, 0);
            for (size_t byte = {0}; byte < sizeof(bytes); byte++)
            {
                TB_TEST_EXPECT_UINT((unsigned char)bytes[byte], ==, 0x55);
            }
        }
    }
}

TB_TEST(core_utf8, every_scalar_round_trips_in_both_encodings)
{
    for (uint32_t scalar = {0}; scalar <= 0x10FFFFu; scalar++)
    {
        char bytes[4] = {0};
        char restored[4] = {0};
        uint16_t units[2] = {0};
        size_t length = {0};
        size_t next = {0};
        size_t unit_count = {0};
        size_t byte_count = {0};
        uint32_t decoded = {0};
        length = tb_utf8_encode(scalar, bytes, sizeof(bytes));
        if (scalar >= 0xD800u && scalar <= 0xDFFFu)
        {
            TB_TEST_REQUIRE_UINT(length, ==, 0);
            continue;
        }
        TB_TEST_REQUIRE_UINT(length, >, 0);
        TB_TEST_REQUIRE(tb_utf8_decode(bytes, length, 0, &decoded, &next));
        TB_TEST_REQUIRE_UINT(decoded, ==, scalar);
        TB_TEST_REQUIRE_UINT(next, ==, length);
        TB_TEST_REQUIRE(tb_utf8_to_utf16(bytes, length, units, 2, &unit_count));
        if (scalar <= 0xFFFFu)
        {
            TB_TEST_REQUIRE_UINT(unit_count, ==, 1);
            TB_TEST_REQUIRE_UINT(units[0], ==, scalar);
        }
        else
        {
            TB_TEST_REQUIRE_UINT(unit_count, ==, 2);
            TB_TEST_REQUIRE_UINT(units[0], ==, 0xD800u + ((scalar - 0x10000u) >> 10));
            TB_TEST_REQUIRE_UINT(units[1], ==, 0xDC00u + ((scalar - 0x10000u) & 0x3FFu));
        }
        TB_TEST_REQUIRE(
            tb_utf8_from_utf16(units, unit_count, restored, sizeof(restored), &byte_count));
        TB_TEST_REQUIRE_UINT(byte_count, ==, length);
        TB_TEST_REQUIRE(memcmp(bytes, restored, length) == 0);
    }
    TB_TEST_EXPECT_UINT(tb_utf8_encoded_size(UINT32_MAX), ==, 0);
    TB_TEST_EXPECT_UINT(tb_utf8_encoded_size(0x110000u), ==, 0);
}

TB_TEST(core_utf8, malformed_sequences_are_rejected_without_changing_outputs)
{
    static const struct
    {
        unsigned char bytes[5];
        size_t length;
    } rows[] = {{{0x80}, 1},
                {{0xBF}, 1},
                {{0xC0, 0x80}, 2},
                {{0xC1, 0xBF}, 2},
                {{0xC2}, 1},
                {{0xC2, 0x7F}, 2},
                {{0xE0, 0x9F, 0xBF}, 3},
                {{0xED, 0xA0, 0x80}, 3},
                {{0xED, 0xBF, 0xBF}, 3},
                {{0xF0, 0x8F, 0xBF, 0xBF}, 4},
                {{0xF4, 0x90, 0x80, 0x80}, 4},
                {{0xF5, 0x80, 0x80, 0x80}, 4},
                {{0xF8, 0x80, 0x80, 0x80, 0x80}, 5},
                {{0xFF}, 1},
                {{0xE1, 0x80}, 2},
                {{0xF1, 0x80, 0x80}, 3},
                {{0xE1, 0x80, 0x41}, 3},
                {{0xF1, 0x80, 0x41, 0x80}, 4}};
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(rows); row++)
    {
        char bytes[6] = {0};
        uint32_t scalar = {0};
        size_t offset = {0};
        size_t error = {0};
        size_t count = {0};
        uint16_t output[8] = {0};
        size_t written = {0};
        scalar = 42;
        offset = 99;
        count = 77;
        bytes[0] = 'a';
        memcpy(bytes + 1, rows[row].bytes, rows[row].length);
        TB_TEST_EXPECT(!tb_utf8_decode(bytes, rows[row].length + 1, 1, &scalar, &offset));
        TB_TEST_EXPECT_UINT(scalar, ==, 42);
        TB_TEST_EXPECT_UINT(offset, ==, 99);
        TB_TEST_EXPECT(!tb_utf8_validate(bytes, rows[row].length + 1, &error));
        TB_TEST_EXPECT_UINT(error, ==, 1);
        TB_TEST_EXPECT(!tb_utf8_count(bytes, rows[row].length + 1, &count));
        TB_TEST_EXPECT_UINT(count, ==, 77);
        output[0] = 0x1234;
        TB_TEST_EXPECT(!tb_utf8_to_utf16(bytes, rows[row].length + 1, output, 8, &written));
        TB_TEST_EXPECT_UINT(written, ==, 0);
        TB_TEST_EXPECT_UINT(output[0], ==, 0x1234);
    }
}

/* A reference based on RFC 3629's byte ranges, independent of scalar assembly. */
static bool reference_valid(const unsigned char *bytes, size_t length)
{
    size_t position = {0};
    while (position < length)
    {
        unsigned char lead = {0};
        unsigned char second_min = {0};
        unsigned char second_max = {0};
        size_t tail = {0};
        lead = bytes[position++];
        second_min = 0x80;
        second_max = 0xBF;
        if (lead < 0x80)
        {
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            tail = 1;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            tail = 2;
            if (lead == 0xE0)
            {
                second_min = 0xA0;
            }
            if (lead == 0xED)
            {
                second_max = 0x9F;
            }
        }
        else if (lead >= 0xF0 && lead <= 0xF4)
        {
            tail = 3;
            if (lead == 0xF0)
            {
                second_min = 0x90;
            }
            if (lead == 0xF4)
            {
                second_max = 0x8F;
            }
        }
        else
        {
            return false;
        }
        if (tail > length - position || bytes[position] < second_min ||
            bytes[position] > second_max)
        {
            return false;
        }
        position++;
        tail--;
        while (tail > 0)
        {
            if (bytes[position] < 0x80 || bytes[position] > 0xBF)
            {
                return false;
            }
            position++;
            tail--;
        }
    }
    return true;
}

TB_TEST(core_utf8, random_bytes_agree_with_the_byte_grammar)
{
    for (size_t trial = {0}; trial < 100000; trial++)
    {
        unsigned char bytes[32] = {0};
        size_t length = {0};
        size_t error = {0};
        length = (size_t)tb_test_random_below(sizeof(bytes) + 1);
        tb_test_random_bytes(bytes, length);
        TB_TEST_REQUIRE(tb_utf8_validate((const char *)bytes, length, &error) ==
                        reference_valid(bytes, length));
        TB_TEST_REQUIRE_UINT(error, <=, length);
    }
}

TB_TEST(core_utf8, navigation_agrees_in_both_directions)
{
    char bytes[400] = {0};
    size_t starts[101] = {0};
    uint32_t scalars[100] = {0};
    size_t length = {0};
    size_t count = {0};
    for (size_t index = {0}; index < 100; index++)
    {
        starts[index] = length;
        do
        {
            scalars[index] = (uint32_t)tb_test_random_below(0x110000);
        } while (tb_utf8_encoded_size(scalars[index]) == 0);
        length += tb_utf8_encode(scalars[index], bytes + length, sizeof(bytes) - length);
    }
    starts[100] = length;
    TB_TEST_REQUIRE(tb_utf8_count(bytes, length, &count));
    TB_TEST_EXPECT_UINT(count, ==, 100);
    for (size_t index = {0}; index < 100; index++)
    {
        size_t offset = {0};
        uint32_t scalar = {0};
        TB_TEST_EXPECT(tb_utf8_offset(bytes, length, index, &offset));
        TB_TEST_EXPECT_UINT(offset, ==, starts[index]);
        TB_TEST_EXPECT(tb_utf8_previous(bytes, length, starts[index + 1], &scalar, &offset));
        TB_TEST_EXPECT_UINT(offset, ==, starts[index]);
        TB_TEST_EXPECT_UINT(scalar, ==, scalars[index]);
        for (size_t inside = {0}; inside < starts[index + 1] - starts[index]; inside++)
        {
            TB_TEST_EXPECT(tb_utf8_boundary(bytes, length, starts[index] + inside, &offset));
            TB_TEST_EXPECT_UINT(offset, ==, starts[index]);
        }
    }
    TB_TEST_EXPECT(tb_utf8_offset(bytes, length, 100, &count));
    TB_TEST_EXPECT_UINT(count, ==, length);
    TB_TEST_EXPECT(!tb_utf8_offset(bytes, length, 101, &count));
    TB_TEST_EXPECT(!tb_utf8_boundary(bytes, length, SIZE_MAX, &count));
    TB_TEST_EXPECT(tb_utf8_boundary(bytes, length, length, &count));
    TB_TEST_EXPECT_UINT(count, ==, length);
}

TB_TEST(core_utf8, conversion_measures_first_and_never_partially_writes)
{
    const char bytes[] = "A\xF0\x9F\x98\x80\0Z";
    const uint16_t units[] = {0x41, 0xD83D, 0xDE00, 0, 0x5A};
    static const uint16_t invalid[][3] = {{0xDC00, 0, 0}, {0xD800, 0x41, 0}, {0xD800, 0xD800, 0}};
    size_t required = {0};
    TB_TEST_EXPECT(tb_utf8_to_utf16(bytes, sizeof(bytes) - 1, NULL, 0, &required));
    TB_TEST_EXPECT_UINT(required, ==, 5);
    TB_TEST_EXPECT(tb_utf8_from_utf16(units, 5, NULL, 0, &required));
    TB_TEST_EXPECT_UINT(required, ==, sizeof(bytes) - 1);
    for (size_t capacity = {0}; capacity < 9; capacity++)
    {
        uint16_t wide[9] = {0};
        char narrow[9] = {0};
        bool result = {0};
        memset(wide, 0x55, sizeof(wide));
        memset(narrow, 0x55, sizeof(narrow));
        result = tb_utf8_to_utf16(bytes, sizeof(bytes) - 1, wide, capacity, &required);
        TB_TEST_EXPECT(result == (capacity >= 5));
        TB_TEST_EXPECT_UINT(required, ==, 5);
        if (result)
        {
            TB_TEST_EXPECT(memcmp(wide, units, sizeof(units)) == 0);
        }
        else
        {
            for (size_t index = {0}; index < 9; index++)
            {
                TB_TEST_EXPECT_UINT(wide[index], ==, 0x5555);
            }
        }
        result = tb_utf8_from_utf16(units, 5, narrow, capacity, &required);
        TB_TEST_EXPECT(result == (capacity >= sizeof(bytes) - 1));
        TB_TEST_EXPECT_UINT(required, ==, sizeof(bytes) - 1);
        if (result)
        {
            TB_TEST_EXPECT(memcmp(narrow, bytes, sizeof(bytes) - 1) == 0);
        }
        else
        {
            for (size_t index = {0}; index < 9; index++)
            {
                TB_TEST_EXPECT_INT(narrow[index], ==, 0x55);
            }
        }
    }
    for (size_t row = {0}; row < TB_TEST_COUNT_OF(invalid); row++)
    {
        char output[16] = {0};
        output[0] = 42;
        TB_TEST_EXPECT(!tb_utf8_from_utf16(invalid[row], 3, output, sizeof(output), &required));
        TB_TEST_EXPECT_UINT(required, ==, 0);
        TB_TEST_EXPECT_INT(output[0], ==, 42);
    }
    TB_TEST_EXPECT(!tb_utf8_from_utf16(invalid[1], 1, NULL, 0, &required));
}

TB_TEST(core_utf8, empty_and_invalid_navigation_has_defined_results)
{
    size_t output = {0};
    uint32_t scalar = {0};
    TB_TEST_EXPECT(tb_utf8_validate(NULL, 0, &output));
    TB_TEST_EXPECT(tb_utf8_count(NULL, 0, &output));
    TB_TEST_EXPECT_UINT(output, ==, 0);
    TB_TEST_EXPECT(tb_utf8_offset(NULL, 0, 0, &output));
    TB_TEST_EXPECT(tb_utf8_boundary(NULL, 0, 0, &output));
    TB_TEST_EXPECT(!tb_utf8_decode(NULL, 0, 0, &scalar, &output));
    TB_TEST_EXPECT(!tb_utf8_previous(NULL, 0, 0, &scalar, &output));
    TB_TEST_EXPECT(!tb_utf8_previous("a", 1, 2, &scalar, &output));
    TB_TEST_EXPECT(!tb_utf8_previous("\xE2\x82\xAC", 3, 2, &scalar, &output));
    TB_TEST_EXPECT(!tb_utf8_previous("\x80\x80\x80\x80\x80", 5, 5, &scalar, &output));
    TB_TEST_EXPECT(!tb_utf8_boundary("a\xFF", 2, 2, &output));
    TB_TEST_EXPECT(tb_utf8_to_utf16(NULL, 0, NULL, 0, &output));
    TB_TEST_EXPECT(tb_utf8_from_utf16(NULL, 0, NULL, 0, &output));
    TB_TEST_EXPECT_UINT(tb_utf8_encode(42, NULL, 0), ==, 0);
}

static void violate_contract(void *context)
{
    size_t choice = {0};
    size_t offset = {0};
    uint32_t scalar = {0};
    choice = *(const size_t *)context;
    switch (choice)
    {
    case 0:
        (void)tb_utf8_decode(NULL, 1, 0, &scalar, &offset);
        break;
    case 1:
        (void)tb_utf8_decode("a", 1, 0, NULL, &offset);
        break;
    case 2:
        (void)tb_utf8_decode("a", 1, 0, &scalar, NULL);
        break;
    case 3:
        (void)tb_utf8_encode(42, NULL, 1);
        break;
    case 4:
        (void)tb_utf8_validate("", 0, NULL);
        break;
    case 5:
        (void)tb_utf8_count("", 0, NULL);
        break;
    case 6:
        (void)tb_utf8_offset("", 0, 0, NULL);
        break;
    case 7:
        (void)tb_utf8_previous("a", 1, 1, NULL, &offset);
        break;
    case 8:
        (void)tb_utf8_previous("a", 1, 1, &scalar, NULL);
        break;
    case 9:
        (void)tb_utf8_boundary("", 0, 0, NULL);
        break;
    case 10:
        (void)tb_utf8_to_utf16("", 0, NULL, 1, &offset);
        break;
    case 11:
        (void)tb_utf8_to_utf16("", 0, NULL, 0, NULL);
        break;
    case 12:
        (void)tb_utf8_from_utf16(NULL, 1, NULL, 0, &offset);
        break;
    case 13:
        (void)tb_utf8_from_utf16(NULL, 0, NULL, 1, &offset);
        break;
    case 14:
        (void)tb_utf8_from_utf16(NULL, 0, NULL, 0, NULL);
        break;
    default:
        break;
    }
}
TB_TEST(core_utf8, required_pointers_are_contracts)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t choice = {0}; choice < 15; choice++)
    {
        TB_TEST_EXPECT_TRAP(violate_contract, &choice, "!= NULL");
    }
}
