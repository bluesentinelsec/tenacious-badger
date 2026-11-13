/* tb_sum_i32 must add exactly when the result is representable and otherwise
 * refuse without touching its output. */
#include "badger/core.h"

#include "badger/assert.h"
#include "tb_test.h"

#include <inttypes.h>

/* Distinguishes "left unchanged" from every result the cases can produce. */
#define OUTPUT_SENTINEL ((int32_t)123456789)

typedef struct sum_row
{
    int32_t left;
    int32_t right;
    int32_t sum;
} sum_row;

TB_TEST(core_sum, adds_representable_operands)
{
    static const sum_row rows[] = {
        {2, 3, 5},
        {0, 0, 0},
        {-2, -3, -5},
        {-2, 3, 1},
        {INT32_MAX, 0, INT32_MAX},
        {INT32_MIN, 0, INT32_MIN},
        {0, INT32_MAX, INT32_MAX},
        {0, INT32_MIN, INT32_MIN},
        {INT32_MAX - 1, 1, INT32_MAX},
        {INT32_MIN + 1, -1, INT32_MIN},
        {INT32_MIN, INT32_MAX, -1},
        {INT32_MAX, INT32_MIN, -1},
        {INT32_MAX, -INT32_MAX, 0},
        {INT32_MAX / 2, INT32_MAX / 2 + 1, INT32_MAX},
        {INT32_MIN / 2, INT32_MIN / 2, INT32_MIN},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        int32_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: %" PRId32 " + %" PRId32, index, rows[index].left,
                        rows[index].right);
        TB_TEST_EXPECT(tb_sum_i32(rows[index].left, rows[index].right, &result));
        TB_TEST_EXPECT_INT(result, ==, rows[index].sum);
    }
}

typedef struct overflow_row
{
    int32_t left;
    int32_t right;
} overflow_row;

TB_TEST(core_sum, rejects_overflow_and_preserves_the_output)
{
    static const overflow_row rows[] = {
        {INT32_MAX, 1},
        {1, INT32_MAX},
        {INT32_MIN, -1},
        {-1, INT32_MIN},
        {INT32_MAX, INT32_MAX},
        {INT32_MIN, INT32_MIN},
        {INT32_MAX / 2 + 1, INT32_MAX / 2 + 1},
        {INT32_MIN / 2 - 1, INT32_MIN / 2},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        int32_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: %" PRId32 " + %" PRId32, index, rows[index].left,
                        rows[index].right);
        TB_TEST_EXPECT(!tb_sum_i32(rows[index].left, rows[index].right, &result));
        TB_TEST_EXPECT_INT(result, ==, OUTPUT_SENTINEL);
    }
}

/* Half of the operands sit within a few steps of a limit, where overflow
 * decisions are made; the rest cover the whole range. */
static int32_t random_operand(void)
{
    enum
    {
        NEAR_LIMIT_DISTANCE = 4
    };

    switch (tb_test_random_below(4))
    {
    case 0:
        return (int32_t)tb_test_random_between(INT32_MAX - NEAR_LIMIT_DISTANCE, INT32_MAX);
    case 1:
        return (int32_t)tb_test_random_between(INT32_MIN, INT32_MIN + NEAR_LIMIT_DISTANCE);
    default:
        return (int32_t)tb_test_random_between(INT32_MIN, INT32_MAX);
    }
}

TB_TEST(core_sum, agrees_with_wide_arithmetic_on_random_operands)
{
    enum
    {
        ITERATION_COUNT = 100000
    };

    for (size_t iteration = 0; iteration < ITERATION_COUNT; iteration++)
    {
        const int32_t left = random_operand();
        const int32_t right = random_operand();
        /* 64-bit addition of two 32-bit values cannot overflow. */
        const int64_t wide_sum = (int64_t)left + (int64_t)right;
        const bool representable = wide_sum >= INT32_MIN && wide_sum <= INT32_MAX;
        int32_t result = OUTPUT_SENTINEL;
        int32_t swapped_result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("iteration %zu: %" PRId32 " + %" PRId32, iteration, left, right);
        TB_TEST_EXPECT(tb_sum_i32(left, right, &result) == representable);
        /* Addition is commutative, including the decision to refuse. */
        TB_TEST_EXPECT(tb_sum_i32(right, left, &swapped_result) == representable);
        TB_TEST_EXPECT_INT(result, ==, swapped_result);
        if (representable)
        {
            TB_TEST_EXPECT_INT(result, ==, wide_sum);
        }
        else
        {
            TB_TEST_EXPECT_INT(result, ==, OUTPUT_SENTINEL);
        }
    }
}

static void sum_into_null(void *context)
{
    (void)context;
    (void)tb_sum_i32(1, 2, NULL);
}

TB_TEST(core_sum, null_output_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(sum_into_null, NULL, "result != NULL");
}
