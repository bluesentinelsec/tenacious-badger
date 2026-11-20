/* The shortest digits of a double are found fast, with Schubfach and a table
 * of powers of ten, and the result must be exactly the one that the exact
 * definition gives: the fewest significant digits that read back, and of
 * those the nearest, ties to even. These cases hold the fast conversion to
 * the exact one over every binary exponent, which also reaches every entry
 * of the table, and over values of every kind. */
#include "badger/decimal.h"
#include "core/decimal_internal.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

#define FRACTION_BITS 52
#define FRACTION_MASK ((UINT64_C(1) << FRACTION_BITS) - 1)
#define HIGHEST_FINITE_EXPONENT 2046

static double double_of(uint64_t bits)
{
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Fails the case unless both conversions give the same digits and exponent. */
static void expect_same(uint64_t bits)
{
    const tb_decimal_parts parts = tb_decimal_split(double_of(bits));
    tb_decimal_exact fast = {0};
    tb_decimal_exact exact = {0};

    if (parts.is_nan || parts.is_infinite || parts.mantissa == 0)
    {
        return;
    }
    tb_decimal_shortest_fast(&parts, &fast);
    tb_decimal_shortest_exact(&parts, &exact);
    if (fast.count != exact.count || fast.exponent != exact.exponent ||
        memcmp(fast.digits, exact.digits, exact.count) != 0)
    {
        TB_TEST_CONTEXT("bits 0x%016llx: fast %.*se%d, exact %.*se%d", (unsigned long long)bits,
                        (int)fast.count, fast.digits, fast.exponent, (int)exact.count, exact.digits,
                        exact.exponent);
        TB_TEST_FAIL("the fast shortest digits differ from the exact ones");
    }
}

TB_TEST(core_decimal_fast, every_binary_exponent_agrees_with_the_exact_digits)
{
    for (uint64_t exponent = 0; exponent <= HIGHEST_FINITE_EXPONENT; exponent++)
    {
        const uint64_t base = exponent << FRACTION_BITS;
        expect_same(base | 1u);
        expect_same(base | 2u);
        expect_same(base | FRACTION_MASK);
        expect_same(base | (FRACTION_MASK - 1u));
        if (exponent != 0)
        {
            /* A power of two, whose gap below is half its gap above. */
            expect_same(base);
        }
        for (int round = 0; round < 6; round++)
        {
            expect_same(base | (tb_test_random_u64() & FRACTION_MASK));
        }
    }
}

TB_TEST(core_decimal_fast, random_doubles_agree_with_the_exact_digits)
{
    for (int round = 0; round < 20000; round++)
    {
        expect_same(tb_test_random_u64() & ~(UINT64_C(1) << 63));
    }
}

/* Values written with few digits, as people and files write them, are where
 * the shorter candidate and the ties are met. */
TB_TEST(core_decimal_fast, short_decimals_agree_with_the_exact_digits)
{
    for (int round = 0; round < 20000; round++)
    {
        char text[64] = {0};
        double value = 0.0;
        uint64_t bits = 0;
        const unsigned digits = 1u + (unsigned)tb_test_random_below(9);
        uint64_t mantissa = 1u + tb_test_random_below(9);
        const int exponent = (int)tb_test_random_below(640) - 320;
        size_t length = 0;

        for (unsigned digit = 1; digit < digits; digit++)
        {
            mantissa = mantissa * 10u + tb_test_random_below(10);
        }
        /* Written by hand: the formatter is what is under test. */
        {
            char reversed[24] = {0};
            size_t count = 0;
            int written_exponent = exponent;
            uint64_t rest = mantissa;
            while (rest != 0)
            {
                reversed[count] = (char)('0' + (int)(rest % 10u));
                count++;
                rest /= 10u;
            }
            while (count > 0)
            {
                count--;
                text[length] = reversed[count];
                length++;
            }
            text[length] = 'e';
            length++;
            if (written_exponent < 0)
            {
                text[length] = '-';
                length++;
                written_exponent = -written_exponent;
            }
            if (written_exponent >= 100)
            {
                text[length] = (char)('0' + written_exponent / 100);
                length++;
            }
            if (written_exponent >= 10)
            {
                text[length] = (char)('0' + (written_exponent / 10) % 10);
                length++;
            }
            text[length] = (char)('0' + written_exponent % 10);
            length++;
        }
        TB_TEST_REQUIRE_UINT(tb_decimal_parse(text, length, &value), ==, TB_DECIMAL_OK);
        memcpy(&bits, &value, sizeof(bits));
        expect_same(bits);
        /* And the doubles on either side of it. */
        expect_same(bits + 1u);
        if ((bits & ~(UINT64_C(1) << 63)) > 1u)
        {
            expect_same(bits - 1u);
        }
    }
}

TB_TEST(core_decimal_fast, integers_and_powers_of_ten_agree_with_the_exact_digits)
{
    double power = 1.0;
    for (int round = 0; round < 20000; round++)
    {
        const double integer = (double)(tb_test_random_u64() >> (11 + tb_test_random_below(53)));
        uint64_t bits = 0;
        memcpy(&bits, &integer, sizeof(bits));
        expect_same(bits);
    }
    for (int exponent = 0; exponent <= 308; exponent++)
    {
        uint64_t bits = 0;
        memcpy(&bits, &power, sizeof(bits));
        expect_same(bits);
        expect_same(bits - 1u);
        expect_same(bits + 1u);
        power *= 10.0;
    }
    power = 1.0;
    for (int exponent = 0; exponent <= 323; exponent++)
    {
        uint64_t bits = 0;
        memcpy(&bits, &power, sizeof(bits));
        if (bits > 1u)
        {
            expect_same(bits);
            expect_same(bits - 1u);
            expect_same(bits + 1u);
        }
        power /= 10.0;
    }
}
