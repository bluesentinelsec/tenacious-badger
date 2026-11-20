/* %e, %g, and %f round a double to a number of digits, and reading a
 * decimal rounds it to a double. Each has a fast form in 64 by 128 bits,
 * which may give up where its arithmetic cannot decide, and an exact form
 * in big integers, which is the definition. These cases hold every answer
 * the fast form gives to the exact one, over every binary exponent, random
 * values, and the ties where the two are most likely to part, and check that
 * the fast form does answer for nearly every value it is meant for. */
#include "badger/decimal.h"
#include "badger/str.h"
#include "core/decimal_internal.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

#define FRACTION_BITS 52
#define FRACTION_MASK ((UINT64_C(1) << FRACTION_BITS) - 1)
#define HIGHEST_FINITE_EXPONENT 2046
#define MOST_FAST_SIGNIFICANT 17
#define MOST_TESTED_FRACTION_DIGITS 40
#define MOST_READ_DIGITS 19

static double double_of(uint64_t bits)
{
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t bits_of(double value)
{
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool same_digits(const tb_decimal_exact *left, const tb_decimal_exact *right)
{
    return left->count == right->count && left->exponent == right->exponent &&
           memcmp(left->digits, right->digits, left->count) == 0;
}

/* How many answers the fast form gave, of how many it was asked for. */
typedef struct tally
{
    size_t answered;
    size_t asked;
} tally;

/* Every count of significant digits the fast form takes, for one value,
 * against the exact digits rounded to as many. */
static void expect_significant(uint64_t bits, tally *count)
{
    const tb_decimal_parts parts = tb_decimal_split(double_of(bits));
    tb_decimal_exact exact = {0};

    if (parts.is_nan || parts.is_infinite)
    {
        return;
    }
    tb_decimal_exact_of(parts.mantissa, parts.exponent, &exact);
    for (size_t significant = 1; significant <= MOST_FAST_SIGNIFICANT; significant++)
    {
        tb_decimal_exact rounded = exact;
        tb_decimal_exact fast = {0};

        tb_decimal_round(&rounded, (ptrdiff_t)significant);
        count->asked++;
        if (!tb_decimal_round_significant_fast(parts.mantissa, parts.exponent, significant, &fast))
        {
            continue;
        }
        count->answered++;
        if (!same_digits(&fast, &rounded))
        {
            TB_TEST_CONTEXT("bits 0x%016llx to %zu digits: fast %.*se%d, exact %.*se%d",
                            (unsigned long long)bits, significant, (int)fast.count, fast.digits,
                            fast.exponent, (int)rounded.count, rounded.digits, rounded.exponent);
            TB_TEST_FAIL("the fast digits differ from the exact ones");
            return;
        }
    }
}

/* Every count of fraction digits up to MOST_TESTED_FRACTION_DIGITS. */
static void expect_fraction(uint64_t bits, tally *count)
{
    const tb_decimal_parts parts = tb_decimal_split(double_of(bits));
    tb_decimal_exact exact = {0};

    if (parts.is_nan || parts.is_infinite)
    {
        return;
    }
    tb_decimal_exact_of(parts.mantissa, parts.exponent, &exact);
    for (size_t fraction = 0; fraction <= MOST_TESTED_FRACTION_DIGITS; fraction++)
    {
        tb_decimal_exact rounded = exact;
        tb_decimal_exact fast = {0};

        if (rounded.count > 0)
        {
            tb_decimal_round(&rounded, (ptrdiff_t)rounded.exponent + 1 + (ptrdiff_t)fraction);
        }
        count->asked++;
        if (!tb_decimal_round_fraction_fast(parts.mantissa, parts.exponent, fraction, &fast))
        {
            continue;
        }
        count->answered++;
        if (!same_digits(&fast, &rounded))
        {
            TB_TEST_CONTEXT("bits 0x%016llx to %zu fraction digits: fast %.*se%d, exact %.*se%d",
                            (unsigned long long)bits, fraction, (int)fast.count, fast.digits,
                            fast.exponent, (int)rounded.count, rounded.digits, rounded.exponent);
            TB_TEST_FAIL("the fast fraction digits differ from the exact ones");
            return;
        }
    }
}

/* A decimal read fast, where it answers, against the exact reading. */
static void expect_read(uint64_t digits, int64_t exponent, tally *count)
{
    double fast = 0.0;
    const double exact = tb_decimal_read_exact(digits, exponent, false);

    count->asked++;
    if (!tb_decimal_read_fast(digits, exponent, false, &fast))
    {
        return;
    }
    count->answered++;
    if (bits_of(fast) != bits_of(exact))
    {
        TB_TEST_CONTEXT("%llue%lld: fast 0x%016llx, exact 0x%016llx", (unsigned long long)digits,
                        (long long)exponent, (unsigned long long)bits_of(fast),
                        (unsigned long long)bits_of(exact));
        TB_TEST_FAIL("the fast reading differs from the exact one");
    }
}

static uint64_t random_below(uint64_t limit)
{
    return tb_test_random_u64() % limit;
}

/* --- significant digits: %e and %g ------------------------------------------------------ */

TB_TEST(core_decimal_rounding, every_binary_exponent_rounds_as_the_exact_digits)
{
    tally count = {0};

    for (uint64_t exponent = 0; exponent <= HIGHEST_FINITE_EXPONENT; exponent++)
    {
        const uint64_t base = exponent << FRACTION_BITS;
        expect_significant(base | 1u, &count);
        expect_significant(base | FRACTION_MASK, &count);
        expect_significant(base, &count);
        expect_significant(base | (tb_test_random_u64() & FRACTION_MASK), &count);
    }
    /* All but the few exponents beyond the table, and the ties. */
    TB_TEST_EXPECT_UINT(count.answered * 100u, >=, count.asked * 95u);
}

TB_TEST(core_decimal_rounding, random_doubles_round_as_the_exact_digits)
{
    tally count = {0};

    for (int round = 0; round < 3000; round++)
    {
        expect_significant(tb_test_random_u64() & ~(UINT64_C(1) << 63), &count);
    }
    TB_TEST_EXPECT_UINT(count.answered * 100u, >=, count.asked * 95u);
}

/* A double whose decimal expansion is short has ties at the digit where it
 * ends: an integer, or an integer over a small power of two, such as 0.125
 * or 2.5. The fast form must break each tie to even, or give up. */
TB_TEST(core_decimal_rounding, ties_round_to_even_as_the_exact_digits)
{
    tally count = {0};

    for (int round = 0; round < 3000; round++)
    {
        const double integer = (double)(random_below(UINT64_C(1) << 53) >> random_below(50));
        const int halvings = (int)random_below(30);
        double value = integer;

        for (int halving = 0; halving < halvings; halving++)
        {
            value /= 2.0;
        }
        expect_significant(bits_of(value), &count);
        expect_fraction(bits_of(value), &count);
    }
    TB_TEST_EXPECT_UINT(count.answered, >, 0);
}

/* --- fraction digits: %f ------------------------------------------------------------------ */

TB_TEST(core_decimal_rounding, fraction_digits_round_as_the_exact_digits)
{
    tally count = {0};

    for (int round = 0; round < 3000; round++)
    {
        /* Values from 2^-70 to 2^70. Large values with many fraction digits
         * do not fit in 64 bits, and are left to the exact form. */
        const uint64_t exponent = 1023u - 70u + random_below(141);
        expect_fraction((exponent << FRACTION_BITS) | (tb_test_random_u64() & FRACTION_MASK),
                        &count);
    }
    TB_TEST_EXPECT_UINT(count.answered, >, 0);
}

/* Where %f is most used, with up to 9 fraction digits of values below 2^30,
 * every value scaled fits in 64 bits, and the fast form answers for all but
 * a rare tie it cannot see. */
TB_TEST(core_decimal_rounding, everyday_fractions_are_rounded_fast)
{
    tally count = {0};

    for (int round = 0; round < 3000; round++)
    {
        const uint64_t exponent = 1023u - 20u + random_below(50);
        const tb_decimal_parts parts = tb_decimal_split(
            double_of((exponent << FRACTION_BITS) | (tb_test_random_u64() & FRACTION_MASK)));

        for (size_t fraction = 0; fraction <= 9; fraction++)
        {
            tb_decimal_exact fast = {0};
            count.asked++;
            if (tb_decimal_round_fraction_fast(parts.mantissa, parts.exponent, fraction, &fast))
            {
                count.answered++;
            }
        }
    }
    TB_TEST_EXPECT_UINT(count.answered * 1000u, >=, count.asked * 999u);
}

TB_TEST(core_decimal_rounding, every_binary_exponent_rounds_fractions_as_the_exact_digits)
{
    tally count = {0};

    for (uint64_t exponent = 0; exponent <= HIGHEST_FINITE_EXPONENT; exponent += 3)
    {
        expect_fraction((exponent << FRACTION_BITS) | (tb_test_random_u64() & FRACTION_MASK),
                        &count);
    }
    TB_TEST_EXPECT_UINT(count.answered, >, 0);
}

/* --- reading ------------------------------------------------------------------------------- */

/* The shortest digits of random doubles: what files hold. Each must read
 * back as itself, and nearly all fast. */
TB_TEST(core_decimal_rounding, shortest_digits_of_random_doubles_read_fast_and_back)
{
    tally count = {0};

    for (int round = 0; round < 20000; round++)
    {
        /* Every normal exponent: a subnormal result is left to the exact
         * form. */
        const uint64_t exponent = 1u + random_below(HIGHEST_FINITE_EXPONENT);
        const uint64_t bits = (exponent << FRACTION_BITS) | (tb_test_random_u64() & FRACTION_MASK);
        const tb_decimal_parts parts = tb_decimal_split(double_of(bits));
        tb_decimal_exact shortest = {0};
        uint64_t digits = 0;
        double fast = 0.0;

        if (parts.is_nan || parts.is_infinite || parts.mantissa == 0)
        {
            continue;
        }
        tb_decimal_shortest_fast(&parts, &shortest);
        for (size_t index = 0; index < shortest.count; index++)
        {
            digits = digits * 10u + (uint64_t)(shortest.digits[index] - '0');
        }
        expect_read(digits, (int64_t)shortest.exponent - (int64_t)shortest.count + 1, &count);
        if (tb_decimal_read_fast(digits, (int64_t)shortest.exponent - (int64_t)shortest.count + 1,
                                 false, &fast))
        {
            TB_TEST_EXPECT_UINT(bits_of(fast), ==, bits);
        }
    }
    TB_TEST_EXPECT_UINT(count.answered * 1000u, >=, count.asked * 999u);
}

/* Random digits of every length, at every exponent the table holds and a
 * little beyond. */
TB_TEST(core_decimal_rounding, random_decimals_read_as_the_exact_reading)
{
    tally count = {0};

    for (int round = 0; round < 20000; round++)
    {
        const size_t length = 1 + (size_t)random_below(MOST_READ_DIGITS);
        uint64_t limit = 1;
        int64_t exponent = (int64_t)random_below(700) - 350;

        for (size_t digit = 0; digit < length; digit++)
        {
            limit *= 10u;
        }
        expect_read(1 + random_below(limit - 1), exponent, &count);
    }
    TB_TEST_EXPECT_UINT(count.answered, >, 0);
}

/* Halfway between two doubles: an odd integer just past 2^53 to 2^60,
 * where doubles are 2 to 256 apart, and its neighbors. Written with a
 * power of ten that the table holds exactly, the fast form must round the
 * tie to even. Written with a negative power, as 90071992547409930e-1, the
 * table's entry is rounded, the tie is hidden under its error, and the fast
 * form must give up rather than guess. */
TB_TEST(core_decimal_rounding, decimals_halfway_between_doubles_read_as_the_exact_reading)
{
    tally count = {0};

    for (unsigned power = 53; power <= 60; power++)
    {
        const uint64_t spacing = UINT64_C(1) << (power - 52);
        for (int round = 0; round < 200; round++)
        {
            const uint64_t below = (UINT64_C(1) << power) + spacing * random_below(1u << 20);
            const uint64_t halfway = below + spacing / 2u;
            tally hidden = {0};
            double fast = 0.0;

            expect_read(halfway, 0, &count);
            expect_read(halfway - 1u, 0, &count);
            expect_read(halfway + 1u, 0, &count);
            expect_read(halfway, 1, &count);
            expect_read(halfway * 10u, -1, &hidden);
            TB_TEST_EXPECT(!tb_decimal_read_fast(halfway * 10u, -1, false, &fast));
            if (power <= 56)
            {
                expect_read(halfway * 100u, -2, &hidden);
            }
        }
    }
    /* With an exact power of ten, every one is answered fast. */
    TB_TEST_EXPECT_UINT(count.answered, ==, count.asked);
}

/* Reading through the public function takes the fast form and gives what
 * the exact one does, for text as people write it. */
TB_TEST(core_decimal_rounding, parsed_text_matches_the_exact_reading)
{
    static const struct
    {
        const char *text;
        uint64_t digits;
        int64_t exponent;
    } cases[] = {
        {"0.1", 1, -1},
        {"123.456", 123456, -3},
        {"1e23", 1, 23},
        {"9007199254740993", 9007199254740993u, 0},
        {"2.2250738585072014e-308", 22250738585072014u, -324},
        {"1.7976931348623157e308", 17976931348623157u, 292},
        {"4.9e-324", 49, -325},
        {"6.02214076e23", 602214076, 15},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        double parsed = 0.0;
        TB_TEST_CONTEXT("%s", cases[index].text);
        TB_TEST_REQUIRE_UINT(
            tb_decimal_parse(cases[index].text, tb_str_length(cases[index].text), &parsed), ==,
            TB_DECIMAL_OK);
        TB_TEST_EXPECT_UINT(
            bits_of(parsed), ==,
            bits_of(tb_decimal_read_exact(cases[index].digits, cases[index].exponent, false)));
    }
}
