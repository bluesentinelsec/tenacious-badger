/* Random inputs are only useful if a failure can be replayed. These cases pin
 * the generator's exact output, so a seed means the same thing on every
 * platform and compiler, and check that ranges are honored at their limits. */
#include "internal.h"

#include <string.h>

TB_TEST(random, generator_produces_the_reference_sequence)
{
    /* SplitMix64 outputs for seed 0, from the published reference code. */
    static const uint64_t expected[] = {
        UINT64_C(0xE220A8397B1DCDAF),
        UINT64_C(0x6E789E6AA1B965F4),
        UINT64_C(0x06C45D188009454F),
        UINT64_C(0xF88BB8A8724C81EC),
    };
    tb_test_generator generator = TB_TEST_GENERATOR_INIT;

    tb_test_generator_seed(&generator, 0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected); index++)
    {
        TB_TEST_CONTEXT("output %zu", index);
        TB_TEST_EXPECT_HEX(tb_test_generator_next(&generator), ==, expected[index]);
    }
}

TB_TEST(random, equal_seeds_give_equal_sequences)
{
    enum
    {
        DRAW_COUNT = 1000
    };
    tb_test_generator first = TB_TEST_GENERATOR_INIT;
    tb_test_generator second = TB_TEST_GENERATOR_INIT;
    tb_test_generator other = TB_TEST_GENERATOR_INIT;
    size_t differing = {0};

    tb_test_generator_seed(&first, TB_TEST_DEFAULT_SEED);
    tb_test_generator_seed(&second, TB_TEST_DEFAULT_SEED);
    tb_test_generator_seed(&other, TB_TEST_DEFAULT_SEED + 1);
    for (size_t index = 0; index < DRAW_COUNT; index++)
    {
        const uint64_t value = tb_test_generator_next(&first);

        TB_TEST_CONTEXT("draw %zu", index);
        TB_TEST_EXPECT_HEX(tb_test_generator_next(&second), ==, value);
        if (tb_test_generator_next(&other) != value)
        {
            differing++;
        }
    }
    /* Adjacent seeds must not produce related sequences. */
    TB_TEST_EXPECT_UINT(differing, ==, DRAW_COUNT);
}

TB_TEST(random, below_stays_under_every_bound)
{
    static const uint64_t bounds[] = {
        1,
        2,
        3,
        7,
        10,
        255,
        256,
        1000,
        UINT64_C(0x100000000),
        UINT64_C(0x8000000000000000),
        UINT64_C(0x8000000000000001),
        UINT64_MAX - 1,
        UINT64_MAX,
    };
    enum
    {
        DRAW_COUNT = 2000
    };
    tb_test_generator generator = TB_TEST_GENERATOR_INIT;

    tb_test_generator_seed(&generator, tb_test_random_u64());
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bounds); index++)
    {
        for (size_t draw = 0; draw < DRAW_COUNT; draw++)
        {
            TB_TEST_CONTEXT("bound 0x%llX, draw %zu", (unsigned long long)bounds[index], draw);
            TB_TEST_EXPECT_UINT(tb_test_generator_below(&generator, bounds[index]), <,
                                bounds[index]);
        }
    }
}

TB_TEST(random, below_spreads_evenly_over_a_small_bound)
{
    enum
    {
        BOUND = 6,
        DRAW_COUNT = 60000,
        EXPECTED_PER_VALUE = DRAW_COUNT / BOUND,
        /* About eight standard deviations: a fair generator stays inside. */
        TOLERANCE = 750
    };
    size_t counts[BOUND] = {0};
    tb_test_generator generator = TB_TEST_GENERATOR_INIT;

    tb_test_generator_seed(&generator, TB_TEST_DEFAULT_SEED);
    for (size_t draw = 0; draw < DRAW_COUNT; draw++)
    {
        const uint64_t value = tb_test_generator_below(&generator, BOUND);

        TB_TEST_REQUIRE_UINT(value, <, BOUND);
        counts[value]++;
    }
    for (size_t value = 0; value < BOUND; value++)
    {
        TB_TEST_CONTEXT("value %zu drawn %zu times", value, counts[value]);
        TB_TEST_EXPECT_UINT(counts[value], >, EXPECTED_PER_VALUE - TOLERANCE);
        TB_TEST_EXPECT_UINT(counts[value], <, EXPECTED_PER_VALUE + TOLERANCE);
    }
}

TB_TEST(random, case_seed_depends_on_every_part_of_the_identity)
{
    const uint64_t base = tb_test_generator_seed_for_case(1, "list", "push");

    TB_TEST_EXPECT_HEX(tb_test_generator_seed_for_case(1, "list", "push"), ==, base);
    TB_TEST_EXPECT_HEX(tb_test_generator_seed_for_case(2, "list", "push"), !=, base);
    TB_TEST_EXPECT_HEX(tb_test_generator_seed_for_case(1, "ring", "push"), !=, base);
    TB_TEST_EXPECT_HEX(tb_test_generator_seed_for_case(1, "list", "pop"), !=, base);
    /* The suite and case names are separate fields, not one joined string. */
    TB_TEST_EXPECT_HEX(tb_test_generator_seed_for_case(1, "a", "bc"), !=,
                       tb_test_generator_seed_for_case(1, "ab", "c"));
}

TB_TEST(random, case_stream_is_derived_from_the_run_seed_and_case_name)
{
    /* This is the contract behind "rerun with --seed": the running case draws
     * exactly the sequence that its seed and name determine. */
    tb_test_generator expected = TB_TEST_GENERATOR_INIT;

    tb_test_generator_seed(
        &expected,
        tb_test_generator_seed_for_case(tb_test_random_seed(), "random",
                                        "case_stream_is_derived_from_the_run_seed_and_case_name"));
    for (size_t draw = 0; draw < 16; draw++)
    {
        TB_TEST_CONTEXT("draw %zu", draw);
        TB_TEST_EXPECT_HEX(tb_test_random_u64(), ==, tb_test_generator_next(&expected));
    }
}

typedef struct range_row
{
    int64_t minimum;
    int64_t maximum;
} range_row;

TB_TEST(random, between_honors_inclusive_limits)
{
    static const range_row rows[] = {
        {0, 0},
        {-1, 1},
        {-5, 5},
        {0, 255},
        {INT64_MIN, INT64_MIN},
        {INT64_MAX, INT64_MAX},
        {INT64_MIN, INT64_MIN + 1},
        {INT64_MAX - 1, INT64_MAX},
        {INT64_MIN, 0},
        {0, INT64_MAX},
        {INT64_MIN, INT64_MAX},
        {INT64_MIN + 1, INT64_MAX},
        {INT64_MIN, INT64_MAX - 1},
    };
    enum
    {
        DRAW_COUNT = 2000
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        for (size_t draw = 0; draw < DRAW_COUNT; draw++)
        {
            const int64_t value = tb_test_random_between(rows[index].minimum, rows[index].maximum);

            TB_TEST_CONTEXT("row %zu, draw %zu", index, draw);
            TB_TEST_EXPECT_INT(value, >=, rows[index].minimum);
            TB_TEST_EXPECT_INT(value, <=, rows[index].maximum);
        }
    }
}

TB_TEST(random, between_reaches_both_ends_of_a_small_range)
{
    enum
    {
        DRAW_COUNT = 500
    };
    bool reached_minimum = false;
    bool reached_maximum = false;
    bool reached_negative_limit = false;

    for (size_t draw = 0; draw < DRAW_COUNT; draw++)
    {
        const int64_t value = tb_test_random_between(-2, 2);
        const int64_t near_limit = tb_test_random_between(INT64_MIN, INT64_MIN + 3);

        reached_minimum = reached_minimum || value == -2;
        reached_maximum = reached_maximum || value == 2;
        reached_negative_limit = reached_negative_limit || near_limit == INT64_MIN;
    }
    TB_TEST_EXPECT(reached_minimum);
    TB_TEST_EXPECT(reached_maximum);
    TB_TEST_EXPECT(reached_negative_limit);
}

TB_TEST(random, bytes_fill_exactly_the_requested_range)
{
    enum
    {
        GUARD_SIZE = 8,
        MAXIMUM_SIZE = 64,
        GUARD_BYTE = 0xA5
    };
    unsigned char buffer[GUARD_SIZE + MAXIMUM_SIZE + GUARD_SIZE] = {0};
    unsigned char guard[GUARD_SIZE] = {0};

    memset(guard, GUARD_BYTE, sizeof(guard));
    for (size_t size = 0; size <= MAXIMUM_SIZE; size++)
    {
        TB_TEST_CONTEXT("size %zu", size);
        memset(buffer, GUARD_BYTE, sizeof(buffer));
        tb_test_random_bytes(buffer + GUARD_SIZE, size);
        TB_TEST_EXPECT_BYTES(buffer, guard, GUARD_SIZE);
        TB_TEST_EXPECT_BYTES(buffer + GUARD_SIZE + size, guard, GUARD_SIZE);
    }
    /* No destination is needed, or touched, for an empty range. */
    tb_test_random_bytes(NULL, 0);
}

TB_TEST(random, bytes_are_not_constant)
{
    enum
    {
        BYTE_COUNT = 256
    };
    unsigned char bytes[BYTE_COUNT] = {0};
    size_t different_from_first = {0};

    tb_test_random_bytes(bytes, sizeof(bytes));
    for (size_t index = 1; index < BYTE_COUNT; index++)
    {
        if (bytes[index] != bytes[0])
        {
            different_from_first++;
        }
    }
    /* Each byte equals the first with probability 1/256. */
    TB_TEST_EXPECT_UINT(different_from_first, >, BYTE_COUNT / 2);
}
