/* The Windows adapter converts counts of the performance counter to
 * nanoseconds. The conversion must be exact for every count whose nanoseconds
 * fit in 64 bits, at any frequency: the direct formula, count * 1e9 /
 * frequency, overflows half an hour after the system starts. */
#include "counter.h"

#include "tb_test.h"

#include <inttypes.h>

#define RANDOM_CASE_COUNT 20000

/* Bits in half of a 64-bit value, and the mask that keeps the low half. */
#define HALF_BITS 32
#define HALF_MASK UINT64_C(0xFFFFFFFF)

typedef struct conversion_row
{
    uint64_t count;
    uint64_t counts_per_second;
    uint64_t nanoseconds;
} conversion_row;

TB_TEST(platform_win32_counter, converts_known_counts)
{
    static const conversion_row rows[] = {
        /* The usual frequency: a count is 100 nanoseconds. */
        {0, TB_WIN32_USUAL_COUNTS_PER_SECOND, 0},
        {1, TB_WIN32_USUAL_COUNTS_PER_SECOND, 100},
        {10000000, TB_WIN32_USUAL_COUNTS_PER_SECOND, 1000000000},
        /* Its neighbors take the general route. */
        {9999999, 9999999, 1000000000},
        {10000001, 10000001, 1000000000},
        {UINT64_C(1) << 40, 9999999, UINT64_C(109951173772717)},
        {UINT64_C(1) << 40, 10000001, UINT64_C(109951151782484)},
        /* A count that is not a whole number of nanoseconds is rounded down. */
        {1, 3, 333333333},
        {2, 3, 666666666},
        {3, 3, 1000000000},
        {4, 3, 1333333333},
        /* The slowest and the fastest counters that are accepted. */
        {1, 1, 1000000000},
        {1, TB_WIN32_COUNTER_FREQUENCY_MAX, 0},
        {19, TB_WIN32_COUNTER_FREQUENCY_MAX, 1},
        {UINT64_MAX, TB_WIN32_COUNTER_FREQUENCY_MAX, UINT64_C(1000000000038464870)},
        /* Counts that overflow when they are multiplied by a billion. */
        {UINT64_C(1) << 40, 10000000, (UINT64_C(1) << 40) * 100},
        {UINT64_C(12345678901234567), UINT64_C(2343750), UINT64_C(5267489664526748586)},
        {UINT64_MAX, 1000000000, UINT64_MAX},
        {UINT64_MAX, UINT64_C(2999999999), UINT64_C(6148914693286155436)},
        /* The last count whose nanoseconds fit, at frequencies counters have had. */
        {UINT64_MAX / 100, 10000000, UINT64_MAX / 100 * 100},
        {UINT64_C(66030950515326656), UINT64_C(3579545), UINT64_C(18446744073709551353)},
        {UINT64_C(22010322987356910), UINT64_C(1193182), UINT64_C(18446744073709551434)},
        {UINT64_C(442721857769029238), UINT64_C(24000000), UINT64_C(18446744073709551583)},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu: %" PRIu64 " counts at %" PRIu64 " a second", index,
                        rows[index].count, rows[index].counts_per_second);
        TB_TEST_EXPECT_UINT(
            tb_win32_counter_to_nanoseconds(rows[index].count, rows[index].counts_per_second), ==,
            rows[index].nanoseconds);
    }
}

/* A 128-bit value, for a reference that cannot overflow. */
typedef struct wide
{
    uint64_t high;
    uint64_t low;
} wide;

/* The full product of two 64-bit values, by long multiplication in halves. */
static wide wide_multiply(uint64_t left, uint64_t right)
{
    const uint64_t left_low = left & HALF_MASK;
    const uint64_t left_high = left >> HALF_BITS;
    const uint64_t right_low = right & HALF_MASK;
    const uint64_t right_high = right >> HALF_BITS;
    const uint64_t low_low = left_low * right_low;
    const uint64_t low_high = left_low * right_high;
    const uint64_t high_low = left_high * right_low;
    const uint64_t high_high = left_high * right_high;
    const uint64_t middle =
        (low_low >> HALF_BITS) + (low_high & HALF_MASK) + (high_low & HALF_MASK);
    wide product = {0};

    product.low = (middle << HALF_BITS) | (low_low & HALF_MASK);
    product.high =
        high_high + (low_high >> HALF_BITS) + (high_low >> HALF_BITS) + (middle >> HALF_BITS);
    return product;
}

/* A frequency of any magnitude from 1 to the highest accepted. */
static uint64_t random_frequency(void)
{
    const uint64_t magnitude = tb_test_random_u64() >> tb_test_random_below(64);

    return magnitude % TB_WIN32_COUNTER_FREQUENCY_MAX + 1;
}

/* A count of any magnitude whose nanoseconds fit in 64 bits. */
static uint64_t random_count(uint64_t counts_per_second)
{
    const uint64_t magnitude = tb_test_random_u64() >> tb_test_random_below(64);

    if (counts_per_second >= TB_WIN32_NANOSECONDS_PER_SECOND)
    {
        /* A count is a nanosecond or less, so every count fits. */
        return magnitude;
    }
    /* Whole seconds only, which is slightly fewer counts than would fit. */
    return magnitude % (TB_WIN32_COUNTER_FREQUENCY_MAX * counts_per_second + 1);
}

/* The result is the count times a billion, divided by the frequency and
 * rounded down, exactly when result * frequency is not above count * 1e9 and
 * falls short of it by less than the frequency. */
static void expect_exact_conversion(uint64_t count, uint64_t counts_per_second)
{
    const uint64_t nanoseconds = tb_win32_counter_to_nanoseconds(count, counts_per_second);
    const wide exact = wide_multiply(count, TB_WIN32_NANOSECONDS_PER_SECOND);
    const wide rounded = wide_multiply(nanoseconds, counts_per_second);
    const uint64_t borrow = exact.low < rounded.low ? 1 : 0;

    TB_TEST_CONTEXT("%" PRIu64 " counts at %" PRIu64 " a second gave %" PRIu64, count,
                    counts_per_second, nanoseconds);
    TB_TEST_REQUIRE(rounded.high < exact.high ||
                    (rounded.high == exact.high && rounded.low <= exact.low));
    TB_TEST_EXPECT_UINT(exact.high - rounded.high - borrow, ==, 0);
    TB_TEST_EXPECT_UINT(exact.low - rounded.low, <, counts_per_second);
}

TB_TEST(platform_win32_counter, agrees_with_wide_arithmetic_at_random_frequencies)
{
    for (int index = 0; index < RANDOM_CASE_COUNT; index++)
    {
        const uint64_t counts_per_second = random_frequency();

        expect_exact_conversion(random_count(counts_per_second), counts_per_second);
    }
}

/* The usual frequency is converted by a shorter route of its own. */
TB_TEST(platform_win32_counter, agrees_with_wide_arithmetic_at_the_usual_frequency)
{
    for (int index = 0; index < RANDOM_CASE_COUNT; index++)
    {
        expect_exact_conversion(random_count(TB_WIN32_USUAL_COUNTS_PER_SECOND),
                                TB_WIN32_USUAL_COUNTS_PER_SECOND);
    }
}
