/* The generator must give exactly the numbers its algorithms define, so that a
 * seed repeats on every platform: the raw outputs, the seeding, and the jump
 * are checked against the published reference code's outputs, and what the
 * functions built on them return is checked against one digest that every
 * platform must agree on. The bounded functions must also be unbiased, stay
 * in their ranges, and cover them. */
#include "badger/rng.h"

#include "badger/assert.h"
#include "badger/hash.h"

#include "tb_test.h"

#include <stdint.h>
#include <string.h>

/* A generator in a state given word by word, as the reference code's tests
 * start from. */
static tb_rng rng_in_state(uint64_t first, uint64_t second, uint64_t third, uint64_t fourth)
{
    tb_rng rng = {.state = {first, second, third, fourth}};

    return rng;
}

/* ---------------------------------------------------------------------------
 * Reference outputs.
 *
 * Produced by a Python port of the public-domain reference code of
 * xoshiro256** and SplitMix64 (https://prng.di.unimi.it/), which shares
 * nothing with the engine's code. The port reproduces the outputs other
 * implementations publish: SplitMix64 from 0 begins 0xE220A8397B1DCDAF, and
 * xoshiro256** from {1, 2, 3, 4} begins 11520, 0, 1509978240.
 * ------------------------------------------------------------------------- */

TB_TEST(core_rng, the_outputs_are_those_of_the_reference_xoshiro256_star_star)
{
    static const uint64_t expected[] = {
        UINT64_C(0x0000000000002D00), UINT64_C(0x0000000000000000), UINT64_C(0x000000005A007080),
        UINT64_C(0x10E0000000009D80), UINT64_C(0x10E0B61CE1009D80), UINT64_C(0x0870021CE143AD00),
    };
    tb_rng rng = rng_in_state(1, 2, 3, 4);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected); index++)
    {
        TB_TEST_CONTEXT("output %zu", index);
        TB_TEST_EXPECT_HEX(tb_rng_next(&rng), ==, expected[index]);
    }
}

/* Seeding fills the four words with the first four outputs of SplitMix64
 * from the seed. */
TB_TEST(core_rng, a_seed_fills_the_state_with_splitmix64)
{
    static const struct
    {
        uint64_t seed;
        uint64_t state[4];
        uint64_t outputs[4];
    } cases[] = {
        {0,
         {UINT64_C(0xE220A8397B1DCDAF), UINT64_C(0x6E789E6AA1B965F4), UINT64_C(0x06C45D188009454F),
          UINT64_C(0xF88BB8A8724C81EC)},
         {UINT64_C(0x99EC5F36CB75F2B4), UINT64_C(0xBF6E1F784956452A), UINT64_C(0x1A5F849D4933E6E0),
          UINT64_C(0x6AA594F1262D2D2C)}},
        {42,
         {UINT64_C(0xBDD732262FEB6E95), UINT64_C(0x28EFE333B266F103), UINT64_C(0x47526757130F9F52),
          UINT64_C(0x581CE1FF0E4AE394)},
         {UINT64_C(0x15780B2E0C2EC716), UINT64_C(0x6104D9866D113A7E), UINT64_C(0xAE17533239E499A1),
          UINT64_C(0xECB8AD4703B360A1)}},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        tb_rng rng = {0};

        TB_TEST_CONTEXT("seed %llu", (unsigned long long)cases[index].seed);
        tb_rng_seed(&rng, cases[index].seed);
        for (size_t word = 0; word < 4; word++)
        {
            TB_TEST_EXPECT_HEX(rng.state[word], ==, cases[index].state[word]);
        }
        for (size_t output = 0; output < 4; output++)
        {
            TB_TEST_EXPECT_HEX(tb_rng_next(&rng), ==, cases[index].outputs[output]);
        }
    }
}

TB_TEST(core_rng, a_jump_lands_where_the_reference_jump_does)
{
    static const uint64_t expected_state[4] = {
        UINT64_C(0x8C7A153956B5F3D1), UINT64_C(0x701F1A713401D85E), UINT64_C(0x6527F66A65469085),
        UINT64_C(0x8386B786C4408050)};
    tb_rng rng = rng_in_state(1, 2, 3, 4);

    tb_rng_jump(&rng);
    for (size_t word = 0; word < 4; word++)
    {
        TB_TEST_EXPECT_HEX(rng.state[word], ==, expected_state[word]);
    }
    TB_TEST_EXPECT_HEX(tb_rng_next(&rng), ==, UINT64_C(0xBBD2F312298443D8));
    TB_TEST_EXPECT_HEX(tb_rng_next(&rng), ==, UINT64_C(0x62E57DB2D5706577));
}

/* Everything built on the outputs: bounded integers on both paths, ranges,
 * floats, and doubles. Their results are fixed by the arithmetic, so one
 * digest of them must come out the same on every platform and in every
 * build. If an intended change to one of the functions changes the number,
 * update it here and say so in the change: every seed's numbers move with it. */
TB_TEST(core_rng, what_a_seed_gives_is_the_same_on_every_platform)
{
    tb_rng rng = {0};
    uint64_t digest = 0;

    tb_rng_seed(&rng, 2026);
    for (size_t round = 0; round < 1000; round++)
    {
        const uint64_t below_small = tb_rng_below(&rng, 1000);
        const uint64_t below_large = tb_rng_below(&rng, (UINT64_C(1) << 40) + 7);
        const int64_t between = tb_rng_between(&rng, -500, 500);
        const float single = tb_rng_float(&rng);
        const double wide = tb_rng_double(&rng);
        const float ranged = tb_rng_float_between(&rng, -2.5f, 7.25f);
        const uint32_t half = tb_rng_next_u32(&rng);
        unsigned char bytes[52] = {0};

        memcpy(bytes, &below_small, 8);
        memcpy(bytes + 8, &below_large, 8);
        memcpy(bytes + 16, &between, 8);
        memcpy(bytes + 24, &single, 4);
        memcpy(bytes + 28, &wide, 8);
        memcpy(bytes + 36, &ranged, 4);
        memcpy(bytes + 40, &half, 4);
        memcpy(bytes + 44, &digest, 8);
        digest = tb_hash_bytes(bytes, sizeof(bytes));
    }
    TB_TEST_EXPECT_HEX(digest, ==, UINT64_C(0xDB471E83AF43A8E8));
}

/* ---------------------------------------------------------------------------
 * Seeding.
 * ------------------------------------------------------------------------- */

TB_TEST(core_rng, the_same_seed_gives_the_same_numbers)
{
    tb_rng first = {0};
    tb_rng second = {0};
    size_t differences = 0;

    tb_rng_seed(&first, 12345);
    tb_rng_seed(&second, 12345);
    for (size_t index = 0; index < 1000; index++)
    {
        if (tb_rng_next(&first) != tb_rng_next(&second))
        {
            differences++;
        }
    }
    TB_TEST_EXPECT_UINT(differences, ==, 0);
}

/* Seeds next to each other, as a caller counting levels or frames would pass,
 * start sequences that share nothing. */
TB_TEST(core_rng, seeds_one_bit_apart_give_unrelated_numbers)
{
    tb_rng base = {0};
    uint64_t base_output = 0;

    tb_rng_seed(&base, 0);
    base_output = tb_rng_next(&base);
    for (unsigned bit = 0; bit < 64; bit++)
    {
        tb_rng other = {0};
        uint64_t differing = 0;
        unsigned count = 0;

        tb_rng_seed(&other, UINT64_C(1) << bit);
        differing = tb_rng_next(&other) ^ base_output;
        while (differing != 0)
        {
            count += (unsigned)(differing & 1u);
            differing >>= 1;
        }
        /* About half of 64 bits differ; far fewer would mean the seeds leak
         * into the output. */
        TB_TEST_CONTEXT("seed bit %u", bit);
        TB_TEST_EXPECT_UINT(count, >=, 16);
        TB_TEST_EXPECT_UINT(count, <=, 48);
    }
}

TB_TEST(core_rng, a_seed_from_the_system_is_reported_so_it_can_be_repeated)
{
    tb_rng drawn = {0};
    tb_rng repeated = {0};
    uint64_t seed = 0;
    size_t differences = 0;

    TB_TEST_REQUIRE(tb_rng_seed_from_system(&drawn, &seed));
    tb_rng_seed(&repeated, seed);
    for (size_t index = 0; index < 100; index++)
    {
        if (tb_rng_next(&drawn) != tb_rng_next(&repeated))
        {
            differences++;
        }
    }
    TB_TEST_EXPECT_UINT(differences, ==, 0);
}

TB_TEST(core_rng, a_copy_saves_the_place_in_the_sequence)
{
    tb_rng rng = {0};
    tb_rng saved = {0};
    uint64_t first = 0;

    tb_rng_seed(&rng, 99);
    (void)tb_rng_next(&rng);
    saved = rng;
    first = tb_rng_next(&rng);
    (void)tb_rng_next(&rng);
    rng = saved;
    TB_TEST_EXPECT_HEX(tb_rng_next(&rng), ==, first);
}

TB_TEST(core_rng, the_32_bit_output_is_the_upper_half)
{
    tb_rng full = {0};
    tb_rng half = {0};

    tb_rng_seed(&full, 5);
    tb_rng_seed(&half, 5);
    for (size_t index = 0; index < 100; index++)
    {
        TB_TEST_EXPECT_HEX(tb_rng_next_u32(&half), ==, tb_rng_next(&full) >> 32);
    }
}

/* ---------------------------------------------------------------------------
 * Splitting.
 * ------------------------------------------------------------------------- */

TB_TEST(core_rng, a_split_child_continues_and_the_parent_jumps)
{
    tb_rng parent = {0};
    tb_rng expected_child = {0};
    tb_rng expected_parent = {0};
    tb_rng child = {0};

    tb_rng_seed(&parent, 77);
    expected_child = parent;
    expected_parent = parent;
    tb_rng_jump(&expected_parent);
    child = tb_rng_split(&parent);
    TB_TEST_EXPECT_BYTES(&child, &expected_child, sizeof(child));
    TB_TEST_EXPECT_BYTES(&parent, &expected_parent, sizeof(parent));
}

TB_TEST(core_rng, splits_give_different_numbers)
{
    tb_rng parent = {0};
    tb_rng first = {0};
    tb_rng second = {0};
    size_t equal = 0;

    tb_rng_seed(&parent, 1);
    first = tb_rng_split(&parent);
    second = tb_rng_split(&parent);
    for (size_t index = 0; index < 1000; index++)
    {
        const uint64_t from_first = tb_rng_next(&first);
        const uint64_t from_second = tb_rng_next(&second);
        const uint64_t from_parent = tb_rng_next(&parent);

        if (from_first == from_second || from_first == from_parent || from_second == from_parent)
        {
            equal++;
        }
    }
    TB_TEST_EXPECT_UINT(equal, ==, 0);
}

/* ---------------------------------------------------------------------------
 * Bounded integers.
 * ------------------------------------------------------------------------- */

TB_TEST(core_rng, a_bounded_integer_is_below_its_bound)
{
    static const uint64_t bounds[] = {
        1,
        2,
        3,
        1000,
        UINT32_MAX - 1,
        UINT32_MAX,
        (uint64_t)UINT32_MAX + 1,
        (uint64_t)UINT32_MAX + 2,
        (UINT64_C(1) << 63) + 1,
        UINT64_MAX,
    };
    tb_rng rng = {0};

    tb_rng_seed(&rng, 3);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bounds); index++)
    {
        size_t outside = 0;

        for (size_t draw = 0; draw < 2000; draw++)
        {
            if (tb_rng_below(&rng, bounds[index]) >= bounds[index])
            {
                outside++;
            }
        }
        TB_TEST_CONTEXT("bound %llu", (unsigned long long)bounds[index]);
        TB_TEST_EXPECT_UINT(outside, ==, 0);
    }
}

/* Counts of tb_rng_below(rng, bound) % 3 over many draws.
 *
 * For a bound of 3 * 2^(w - 2), where w is the width of the draw, Lemire's
 * product without its redraw would give values that are multiples of 3 half
 * the time, and each other remainder a quarter: the draws whose low two bits
 * are 0 and 1 both land on a multiple of 3. The redraw removes exactly those
 * draws whose low bits are 0, which leaves the three remainders equally
 * likely. A taken remainder (`draw % bound`) is biased the same way. */
static void count_remainders(uint64_t bound, size_t draws, size_t counts[3])
{
    tb_rng rng = {0};

    tb_rng_seed(&rng, 11);
    for (size_t draw = 0; draw < draws; draw++)
    {
        counts[tb_rng_below(&rng, bound) % 3]++;
    }
}

static void check_unbiased(uint64_t bound)
{
    enum
    {
        DRAWS = 30000
    };
    size_t counts[3] = {0};

    count_remainders(bound, DRAWS, counts);
    for (size_t remainder = 0; remainder < 3; remainder++)
    {
        /* 10,000 expected; a biased method gives 15,000 and 7,500. The
         * standard deviation is about 82, so 600 is more than 7 of them. */
        TB_TEST_CONTEXT("bound %llu, remainder %zu", (unsigned long long)bound, remainder);
        TB_TEST_EXPECT_UINT(counts[remainder], >=, DRAWS / 3 - 600);
        TB_TEST_EXPECT_UINT(counts[remainder], <=, DRAWS / 3 + 600);
    }
}

TB_TEST(core_rng, bounded_integers_are_unbiased_on_the_32_bit_path)
{
    check_unbiased(UINT64_C(3) << 30);
}

TB_TEST(core_rng, bounded_integers_are_unbiased_on_the_64_bit_path)
{
    check_unbiased(UINT64_C(3) << 62);
}

TB_TEST(core_rng, a_range_covers_both_ends_and_nothing_outside)
{
    tb_rng rng = {0};
    size_t seen[7] = {0};
    size_t outside = 0;

    tb_rng_seed(&rng, 8);
    for (size_t draw = 0; draw < 7000; draw++)
    {
        const int64_t value = tb_rng_between(&rng, -3, 3);

        if (value < -3 || value > 3)
        {
            outside++;
            continue;
        }
        seen[value + 3]++;
    }
    TB_TEST_EXPECT_UINT(outside, ==, 0);
    for (size_t index = 0; index < 7; index++)
    {
        TB_TEST_CONTEXT("value %d", (int)index - 3);
        TB_TEST_EXPECT_UINT(seen[index], >, 800);
    }
}

TB_TEST(core_rng, ranges_at_the_limits_of_int64_are_drawn_from)
{
    tb_rng rng = {0};
    bool saw_negative = false;
    bool saw_positive = false;

    tb_rng_seed(&rng, 21);
    TB_TEST_EXPECT_INT(tb_rng_between(&rng, 42, 42), ==, 42);
    TB_TEST_EXPECT_INT(tb_rng_between(&rng, INT64_MIN, INT64_MIN), ==, INT64_MIN);
    TB_TEST_EXPECT_INT(tb_rng_between(&rng, INT64_MAX, INT64_MAX), ==, INT64_MAX);
    for (size_t draw = 0; draw < 100; draw++)
    {
        const int64_t whole = tb_rng_between(&rng, INT64_MIN, INT64_MAX);
        const int64_t lower = tb_rng_between(&rng, INT64_MIN, 0);
        const int64_t upper = tb_rng_between(&rng, -1, INT64_MAX);

        saw_negative = saw_negative || whole < 0;
        saw_positive = saw_positive || whole > 0;
        TB_TEST_EXPECT_INT(lower, <=, 0);
        TB_TEST_EXPECT_INT(upper, >=, -1);
    }
    TB_TEST_EXPECT(saw_negative);
    TB_TEST_EXPECT(saw_positive);
}

/* ---------------------------------------------------------------------------
 * Floating point.
 * ------------------------------------------------------------------------- */

TB_TEST(core_rng, floats_are_steps_of_2_to_the_minus_24_below_1)
{
    tb_rng rng = {0};
    double sum = 0;

    tb_rng_seed(&rng, 13);
    for (size_t draw = 0; draw < 10000; draw++)
    {
        const float value = tb_rng_float(&rng);
        const float steps = value * 16777216.0f;

        TB_TEST_REQUIRE_FLOAT(value, >=, 0.0f);
        TB_TEST_REQUIRE_FLOAT(value, <, 1.0f);
        /* An exact multiple: the float holds the step count exactly. */
        TB_TEST_REQUIRE_FLOAT(steps, ==, (float)(uint32_t)steps);
        sum += (double)value;
    }
    TB_TEST_EXPECT_FLOAT_NEAR(sum / 10000.0, 0.5, 0.02);
}

TB_TEST(core_rng, doubles_are_steps_of_2_to_the_minus_53_below_1)
{
    tb_rng rng = {0};
    double sum = 0;

    tb_rng_seed(&rng, 17);
    for (size_t draw = 0; draw < 10000; draw++)
    {
        const double value = tb_rng_double(&rng);
        const double steps = value * 9007199254740992.0;

        TB_TEST_REQUIRE_FLOAT(value, >=, 0.0);
        TB_TEST_REQUIRE_FLOAT(value, <, 1.0);
        TB_TEST_REQUIRE_FLOAT(steps, ==, (double)(uint64_t)steps);
        sum += value;
    }
    TB_TEST_EXPECT_FLOAT_NEAR(sum / 10000.0, 0.5, 0.02);
}

/* The output depends only on the second word of the state, through steps
 * that can each be undone: multiplying by 5, rotating, multiplying by 9. This
 * is the word that undoes them from an output of all ones. */
#define ALL_ONES_NEXT UINT64_C(0x4FC71C71C71C71C7)

/* The largest output gives the largest value below 1, never 1 itself. */
TB_TEST(core_rng, the_largest_output_stays_below_1)
{
    tb_rng rng = rng_in_state(1, ALL_ONES_NEXT, 1, 1);
    tb_rng again = rng;

    TB_TEST_REQUIRE_HEX(tb_rng_next(&again), ==, UINT64_MAX);
    again = rng;
    TB_TEST_EXPECT_FLOAT(tb_rng_float(&again), ==, 1.0f - 1.0f / 16777216.0f);
    again = rng;
    TB_TEST_EXPECT_FLOAT(tb_rng_double(&again), ==, 1.0 - 1.0 / 9007199254740992.0);
    again = rng;
    TB_TEST_EXPECT_FLOAT(tb_rng_float_between(&again, 0.0f, 1.0f), <, 1.0f);
}

TB_TEST(core_rng, a_float_range_stays_within_its_ends)
{
    tb_rng rng = {0};

    tb_rng_seed(&rng, 19);
    TB_TEST_EXPECT_FLOAT(tb_rng_float_between(&rng, 3.5f, 3.5f), ==, 3.5f);
    for (size_t draw = 0; draw < 10000; draw++)
    {
        const float value = tb_rng_float_between(&rng, -1.0f, 4.0f);

        TB_TEST_REQUIRE_FLOAT(value, >=, -1.0f);
        TB_TEST_REQUIRE_FLOAT(value, <=, 4.0f);
    }
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void draw_unseeded(void *context)
{
    tb_rng rng = {0};

    (void)context;
    (void)tb_rng_next(&rng);
}

static void draw_below_zero(void *context)
{
    tb_rng rng = {0};

    (void)context;
    tb_rng_seed(&rng, 1);
    (void)tb_rng_below(&rng, 0);
}

static void draw_between_reversed(void *context)
{
    tb_rng rng = {0};

    (void)context;
    tb_rng_seed(&rng, 1);
    (void)tb_rng_between(&rng, 5, 4);
}

static void draw_float_between_reversed(void *context)
{
    tb_rng rng = {0};

    (void)context;
    tb_rng_seed(&rng, 1);
    (void)tb_rng_float_between(&rng, 1.0f, 0.0f);
}

static void draw_float_between_overflowing(void *context)
{
    tb_rng rng = {0};

    (void)context;
    tb_rng_seed(&rng, 1);
    (void)tb_rng_float_between(&rng, -3.0e38f, 3.0e38f);
}

static void seed_null(void *context)
{
    (void)context;
    tb_rng_seed(NULL, 1);
}

static void seed_from_system_into_null(void *context)
{
    tb_rng rng = {0};

    (void)context;
    (void)tb_rng_seed_from_system(&rng, NULL);
}

TB_TEST(core_rng, bad_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(draw_unseeded, NULL, "the generator is seeded");
    TB_TEST_EXPECT_TRAP(draw_below_zero, NULL, "bound >= 1");
    TB_TEST_EXPECT_TRAP(draw_between_reversed, NULL, "minimum <= maximum");
    TB_TEST_EXPECT_TRAP(draw_float_between_reversed, NULL, "minimum <= maximum");
    TB_TEST_EXPECT_TRAP(draw_float_between_overflowing, NULL, "their difference are finite");
    TB_TEST_EXPECT_TRAP(seed_null, NULL, "rng != NULL");
    TB_TEST_EXPECT_TRAP(seed_from_system_into_null, NULL, "seed != NULL");
}
