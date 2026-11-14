/* Checked size arithmetic decides whether an allocation or an offset is safe to
 * compute, so a wrong answer in either direction is a defect: accepting an
 * overflow corrupts memory, and refusing a representable size fails a valid
 * request. The functions must be exact at every limit, agree with arithmetic
 * of twice the width, and leave their output untouched when they refuse. */
#include "badger/size.h"

#include "badger/assert.h"
#include "tb_test.h"

#include <limits.h>
#include <stdint.h>

/* Distinguishes "left unchanged" from every result the cases can produce. */
#define OUTPUT_SENTINEL ((size_t)0x5A5A5A5Au)

/* size_t is split into two halves to build arithmetic of twice its width. */
#define HALF_WIDTH (sizeof(size_t) * CHAR_BIT / 2)
#define HALF_MASK (((size_t)1 << HALF_WIDTH) - 1)

/* The smallest value that does not fit in half the width of size_t. Two
 * factors below it cannot overflow; this is where multiplication starts to. */
#define HALF_LIMIT ((size_t)1 << HALF_WIDTH)

/* --- a reference of twice the width ------------------------------------------ */

/* An unsigned integer twice as wide as size_t: high * 2^width + low. */
typedef struct wide_size
{
    size_t high;
    size_t low;
} wide_size;

/* Schoolbook addition, one half at a time, so that no step can overflow. */
static wide_size wide_add(size_t left, size_t right)
{
    const size_t low_sum = (left & HALF_MASK) + (right & HALF_MASK);
    const size_t high_sum = (left >> HALF_WIDTH) + (right >> HALF_WIDTH) + (low_sum >> HALF_WIDTH);
    const wide_size sum = {.high = high_sum >> HALF_WIDTH,
                           .low = ((high_sum & HALF_MASK) << HALF_WIDTH) | (low_sum & HALF_MASK)};

    return sum;
}

/* Schoolbook multiplication of four half-width partial products. */
static wide_size wide_multiply(size_t left, size_t right)
{
    const size_t left_low = left & HALF_MASK;
    const size_t left_high = left >> HALF_WIDTH;
    const size_t right_low = right & HALF_MASK;
    const size_t right_high = right >> HALF_WIDTH;
    const size_t low_low = left_low * right_low;
    const size_t low_high = left_low * right_high;
    const size_t high_low = left_high * right_low;
    const size_t high_high = left_high * right_high;
    const size_t middle = (low_low >> HALF_WIDTH) + (low_high & HALF_MASK) + (high_low & HALF_MASK);
    const wide_size product = {.high = high_high + (low_high >> HALF_WIDTH) +
                                       (high_low >> HALF_WIDTH) + (middle >> HALF_WIDTH),
                               .low = ((middle & HALF_MASK) << HALF_WIDTH) | (low_low & HALF_MASK)};

    return product;
}

TB_TEST(core_size, the_wide_reference_is_itself_correct)
{
    /* The randomized cases trust the reference, so its carries are pinned
     * against values worked out by hand. */
    const wide_size largest_sum = wide_add(SIZE_MAX, SIZE_MAX);
    const wide_size carried_sum = wide_add(SIZE_MAX, 1);
    const wide_size half_carried_sum = wide_add(HALF_MASK, 1);
    const wide_size largest_product = wide_multiply(SIZE_MAX, SIZE_MAX);
    const wide_size shifted_product = wide_multiply(HALF_LIMIT, HALF_LIMIT);
    const wide_size mixed_product = wide_multiply(SIZE_MAX, 2);

    /* (2^w - 1) + (2^w - 1) = 2^w + (2^w - 2) */
    TB_TEST_EXPECT_HEX(largest_sum.high, ==, 1);
    TB_TEST_EXPECT_HEX(largest_sum.low, ==, SIZE_MAX - 1);
    TB_TEST_EXPECT_HEX(carried_sum.high, ==, 1);
    TB_TEST_EXPECT_HEX(carried_sum.low, ==, 0);
    TB_TEST_EXPECT_HEX(half_carried_sum.high, ==, 0);
    TB_TEST_EXPECT_HEX(half_carried_sum.low, ==, HALF_LIMIT);
    /* (2^w - 1)^2 = (2^w - 2) * 2^w + 1 */
    TB_TEST_EXPECT_HEX(largest_product.high, ==, SIZE_MAX - 1);
    TB_TEST_EXPECT_HEX(largest_product.low, ==, 1);
    TB_TEST_EXPECT_HEX(shifted_product.high, ==, 1);
    TB_TEST_EXPECT_HEX(shifted_product.low, ==, 0);
    TB_TEST_EXPECT_HEX(mixed_product.high, ==, 1);
    TB_TEST_EXPECT_HEX(mixed_product.low, ==, SIZE_MAX - 1);
}

/* --- tables ------------------------------------------------------------------- */

typedef struct operand_row
{
    size_t left;
    size_t right;
    size_t result;
} operand_row;

typedef struct overflow_row
{
    size_t left;
    size_t right;
} overflow_row;

TB_TEST(core_size, add_returns_representable_sums)
{
    static const operand_row rows[] = {
        {0, 0, 0},
        {0, 1, 1},
        {1, 0, 1},
        {2, 3, 5},
        {SIZE_MAX, 0, SIZE_MAX},
        {0, SIZE_MAX, SIZE_MAX},
        {SIZE_MAX - 1, 1, SIZE_MAX},
        {1, SIZE_MAX - 1, SIZE_MAX},
        {SIZE_MAX / 2, SIZE_MAX / 2, SIZE_MAX - 1},
        {SIZE_MAX / 2, SIZE_MAX / 2 + 1, SIZE_MAX},
        {HALF_MASK, 1, HALF_LIMIT},
        {HALF_LIMIT, HALF_LIMIT, HALF_LIMIT * 2},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: %zu + %zu", index, rows[index].left, rows[index].right);
        TB_TEST_EXPECT(tb_size_add(rows[index].left, rows[index].right, &result));
        TB_TEST_EXPECT_UINT(result, ==, rows[index].result);
    }
}

TB_TEST(core_size, add_rejects_overflow_and_preserves_the_output)
{
    static const overflow_row rows[] = {
        {SIZE_MAX, 1},
        {1, SIZE_MAX},
        {SIZE_MAX, SIZE_MAX},
        {SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1},
        {SIZE_MAX / 2 + 2, SIZE_MAX / 2},
        {SIZE_MAX - 1, 2},
        {2, SIZE_MAX - 1},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: %zu + %zu", index, rows[index].left, rows[index].right);
        TB_TEST_EXPECT(!tb_size_add(rows[index].left, rows[index].right, &result));
        TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
    }
}

TB_TEST(core_size, multiply_returns_representable_products)
{
    static const operand_row rows[] = {
        {0, 0, 0},
        {0, 1, 0},
        {1, 0, 0},
        {1, 1, 1},
        {2, 3, 6},
        {0, SIZE_MAX, 0},
        {SIZE_MAX, 0, 0},
        {1, SIZE_MAX, SIZE_MAX},
        {SIZE_MAX, 1, SIZE_MAX},
        {2, SIZE_MAX / 2, SIZE_MAX - 1},
        {SIZE_MAX / 2, 2, SIZE_MAX - 1},
        /* SIZE_MAX is a multiple of 3 and of 5 at every width that is a
         * multiple of four bits. */
        {3, SIZE_MAX / 3, SIZE_MAX},
        {SIZE_MAX / 5, 5, SIZE_MAX},
        /* Both factors just fit in half the width: the largest product that
         * can be accepted without dividing. */
        {HALF_MASK, HALF_MASK, SIZE_MAX - 2 * HALF_MASK},
        /* One factor needs more than half the width. */
        {HALF_LIMIT, HALF_MASK, SIZE_MAX - HALF_MASK},
        {HALF_MASK, HALF_LIMIT, SIZE_MAX - HALF_MASK},
        {HALF_LIMIT + 1, HALF_MASK, SIZE_MAX},
        {HALF_LIMIT * 2, HALF_LIMIT / 4, SIZE_MAX / 2 + 1},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: %zu * %zu", index, rows[index].left, rows[index].right);
        TB_TEST_EXPECT(tb_size_multiply(rows[index].left, rows[index].right, &result));
        TB_TEST_EXPECT_UINT(result, ==, rows[index].result);
    }
}

TB_TEST(core_size, multiply_rejects_overflow_and_preserves_the_output)
{
    static const overflow_row rows[] = {
        {SIZE_MAX, 2},
        {2, SIZE_MAX},
        {SIZE_MAX, SIZE_MAX},
        {2, SIZE_MAX / 2 + 1},
        {SIZE_MAX / 2 + 1, 2},
        {3, SIZE_MAX / 3 + 1},
        {SIZE_MAX / 3 + 1, 3},
        /* The smallest product that needs one more bit than size_t has. */
        {HALF_LIMIT, HALF_LIMIT},
        {HALF_LIMIT + 1, HALF_LIMIT},
        {HALF_LIMIT, HALF_LIMIT + 1},
        {HALF_LIMIT * 2, HALF_LIMIT / 2},
        {HALF_LIMIT * 2, HALF_LIMIT / 2 + 1},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: %zu * %zu", index, rows[index].left, rows[index].right);
        TB_TEST_EXPECT(!tb_size_multiply(rows[index].left, rows[index].right, &result));
        TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
    }
}

TB_TEST(core_size, an_operand_may_also_be_the_output)
{
    /* Accumulating into one variable is the common use: total += part. */
    size_t total = 40;

    TB_TEST_EXPECT(tb_size_add(total, 2, &total));
    TB_TEST_EXPECT_UINT(total, ==, 42);
    TB_TEST_EXPECT(tb_size_multiply(total, 2, &total));
    TB_TEST_EXPECT_UINT(total, ==, 84);
    TB_TEST_EXPECT(tb_size_grow(total, total + 1, 1, &total));
    TB_TEST_EXPECT_UINT(total, ==, 168);
    TB_TEST_EXPECT(!tb_size_add(total, SIZE_MAX, &total));
    TB_TEST_EXPECT(!tb_size_multiply(total, SIZE_MAX, &total));
    TB_TEST_EXPECT(!tb_size_grow(total, SIZE_MAX, 2, &total));
    TB_TEST_EXPECT_UINT(total, ==, 168);
}

/* --- random operands ---------------------------------------------------------- */

/* Most operands sit within a few steps of a value where an overflow decision
 * changes: zero, the half-width limit, and SIZE_MAX. The rest are spread over
 * every magnitude, so that products land on both sides of the limit. */
static size_t random_operand(void)
{
    enum
    {
        NEAR_DISTANCE = 4,
        WIDTH = sizeof(size_t) * CHAR_BIT
    };
    const size_t offset = (size_t)tb_test_random_below(NEAR_DISTANCE + 1);

    switch (tb_test_random_below(6))
    {
    case 0:
        return offset;
    case 1:
        return SIZE_MAX - offset;
    case 2:
        return HALF_LIMIT - offset;
    case 3:
        return HALF_LIMIT + offset;
    case 4:
        return (size_t)tb_test_random_u64() >> tb_test_random_below(WIDTH);
    default:
        return (size_t)tb_test_random_u64();
    }
}

/* A capacity that a buffer of the given element size could have. */
static size_t random_capacity(size_t largest_capacity)
{
    const size_t operand = random_operand();

    if (operand <= largest_capacity)
    {
        return operand;
    }
    return largest_capacity - operand % largest_capacity;
}

enum
{
    RANDOM_ITERATION_COUNT = 100000
};

TB_TEST(core_size, add_agrees_with_wide_arithmetic_on_random_operands)
{
    for (size_t iteration = 0; iteration < RANDOM_ITERATION_COUNT; iteration++)
    {
        const size_t left = random_operand();
        const size_t right = random_operand();
        const wide_size wide_sum = wide_add(left, right);
        const bool representable = wide_sum.high == 0;
        size_t result = OUTPUT_SENTINEL;
        size_t swapped_result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("iteration %zu: %zu + %zu", iteration, left, right);
        TB_TEST_EXPECT(tb_size_add(left, right, &result) == representable);
        /* Addition is commutative, including the decision to refuse. */
        TB_TEST_EXPECT(tb_size_add(right, left, &swapped_result) == representable);
        TB_TEST_EXPECT_UINT(result, ==, swapped_result);
        if (representable)
        {
            TB_TEST_EXPECT_UINT(result, ==, wide_sum.low);
        }
        else
        {
            TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
        }
    }
}

TB_TEST(core_size, multiply_agrees_with_wide_arithmetic_on_random_operands)
{
    size_t accepted = {0};
    size_t refused = {0};

    for (size_t iteration = 0; iteration < RANDOM_ITERATION_COUNT; iteration++)
    {
        const size_t left = random_operand();
        const size_t right = random_operand();
        const wide_size wide_product = wide_multiply(left, right);
        const bool representable = wide_product.high == 0;
        size_t result = OUTPUT_SENTINEL;
        size_t swapped_result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("iteration %zu: %zu * %zu", iteration, left, right);
        TB_TEST_EXPECT(tb_size_multiply(left, right, &result) == representable);
        /* Multiplication is commutative, including the decision to refuse. */
        TB_TEST_EXPECT(tb_size_multiply(right, left, &swapped_result) == representable);
        TB_TEST_EXPECT_UINT(result, ==, swapped_result);
        if (representable)
        {
            TB_TEST_EXPECT_UINT(result, ==, wide_product.low);
            accepted++;
        }
        else
        {
            TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
            refused++;
        }
    }
    /* The operands are only a fair test if they exercise both answers. */
    TB_TEST_CONTEXT("%zu accepted, %zu refused", accepted, refused);
    TB_TEST_EXPECT_UINT(accepted, >, RANDOM_ITERATION_COUNT / 10);
    TB_TEST_EXPECT_UINT(refused, >, RANDOM_ITERATION_COUNT / 10);
}

TB_TEST(core_size, multiply_is_exact_on_each_side_of_the_overflow_boundary)
{
    /* For any factor, the largest partner that fits is SIZE_MAX / factor, and
     * the next one does not. */
    for (size_t iteration = 0; iteration < RANDOM_ITERATION_COUNT; iteration++)
    {
        const size_t operand = random_operand();
        /* A factor of 0 or 1 has no partner that overflows. */
        const size_t factor = operand < 2 ? 2 : operand;
        const size_t largest_partner = SIZE_MAX / factor;
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("iteration %zu: factor %zu", iteration, factor);
        TB_TEST_EXPECT(tb_size_multiply(factor, largest_partner, &result));
        TB_TEST_EXPECT_UINT(result, ==, factor * largest_partner);
        TB_TEST_EXPECT_UINT(SIZE_MAX - result, <, factor);

        result = OUTPUT_SENTINEL;
        TB_TEST_EXPECT(!tb_size_multiply(factor, largest_partner + 1, &result));
        TB_TEST_EXPECT(!tb_size_multiply(largest_partner + 1, factor, &result));
        TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
    }
}

/* --- growing a capacity ------------------------------------------------------- */

typedef struct grow_row
{
    size_t capacity;
    size_t required;
    size_t element_size;
    size_t result;
} grow_row;

TB_TEST(core_size, grow_doubles_until_the_requirement_is_larger)
{
    static const grow_row rows[] = {
        /* A buffer that does not exist yet gets exactly what is required. */
        {0, 0, 1, 0},
        {0, 1, 1, 1},
        {0, 100, 8, 100},
        /* Enough room already: nothing changes. */
        {16, 0, 4, 16},
        {16, 15, 4, 16},
        {16, 16, 4, 16},
        {SIZE_MAX, SIZE_MAX, 1, SIZE_MAX},
        /* One more than fits: double. */
        {1, 2, 4, 2},
        {16, 17, 4, 32},
        {16, 32, 4, 32},
        {1000, 1001, 24, 2000},
        /* More than double: exactly what is required. */
        {16, 33, 4, 33},
        {16, 1000, 4, 1000},
        /* An element size of zero is served as one. */
        {16, 17, 0, 32},
        {0, SIZE_MAX, 0, SIZE_MAX},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("row %zu: capacity %zu, required %zu, element size %zu", index,
                        rows[index].capacity, rows[index].required, rows[index].element_size);
        TB_TEST_EXPECT(tb_size_grow(rows[index].capacity, rows[index].required,
                                    rows[index].element_size, &result));
        TB_TEST_EXPECT_UINT(result, ==, rows[index].result);
    }
}

TB_TEST(core_size, grow_stops_at_the_largest_capacity_that_fits_in_memory)
{
    static const size_t element_sizes[] = {1, 2, 3, 8, 24, 4096, SIZE_MAX / 2, SIZE_MAX};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(element_sizes); index++)
    {
        const size_t element_size = element_sizes[index];
        const size_t largest_capacity = SIZE_MAX / element_size;
        /* The largest capacity that can still be doubled, and the next one. */
        const size_t last_doubling = largest_capacity / 2;
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("element size %zu", element_size);
        if (last_doubling > 0)
        {
            TB_TEST_EXPECT(tb_size_grow(last_doubling, last_doubling + 1, element_size, &result));
            TB_TEST_EXPECT_UINT(result, ==, last_doubling * 2);
        }
        if (last_doubling + 1 < largest_capacity)
        {
            /* Doubling would overflow, so growth is limited instead. */
            TB_TEST_EXPECT(
                tb_size_grow(last_doubling + 1, last_doubling + 2, element_size, &result));
            TB_TEST_EXPECT_UINT(result, ==, largest_capacity);
        }
        TB_TEST_EXPECT(tb_size_grow(0, largest_capacity, element_size, &result));
        TB_TEST_EXPECT_UINT(result, ==, largest_capacity);
        TB_TEST_EXPECT(tb_size_grow(largest_capacity, largest_capacity, element_size, &result));
        TB_TEST_EXPECT_UINT(result, ==, largest_capacity);
    }
}

TB_TEST(core_size, grow_rejects_a_requirement_that_cannot_exist)
{
    static const size_t element_sizes[] = {2, 3, 8, 24, 4096, SIZE_MAX / 2, SIZE_MAX};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(element_sizes); index++)
    {
        const size_t element_size = element_sizes[index];
        const size_t largest_capacity = SIZE_MAX / element_size;
        size_t result = OUTPUT_SENTINEL;

        TB_TEST_CONTEXT("element size %zu", element_size);
        TB_TEST_EXPECT(!tb_size_grow(0, largest_capacity + 1, element_size, &result));
        TB_TEST_EXPECT(
            !tb_size_grow(largest_capacity, largest_capacity + 1, element_size, &result));
        TB_TEST_EXPECT(!tb_size_grow(1, SIZE_MAX, element_size, &result));
        TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
    }
}

TB_TEST(core_size, grow_keeps_its_promises_on_random_operands)
{
    for (size_t iteration = 0; iteration < RANDOM_ITERATION_COUNT; iteration++)
    {
        const size_t operand = random_operand();
        const size_t element_size = operand == 0 ? 1 : operand;
        const size_t largest_capacity = SIZE_MAX / element_size;
        const size_t capacity = random_capacity(largest_capacity);
        const size_t required = random_operand();
        size_t result = OUTPUT_SENTINEL;
        const bool grew = tb_size_grow(capacity, required, element_size, &result);

        TB_TEST_CONTEXT("iteration %zu: capacity %zu, required %zu, element size %zu", iteration,
                        capacity, required, element_size);
        TB_TEST_EXPECT(grew == (required <= largest_capacity));
        if (!grew)
        {
            TB_TEST_EXPECT_UINT(result, ==, OUTPUT_SENTINEL);
            continue;
        }
        TB_TEST_EXPECT_UINT(result, >=, required);
        TB_TEST_EXPECT_UINT(result, >=, capacity);
        /* The result is safe to multiply by the element size. */
        TB_TEST_EXPECT_UINT(result, <=, largest_capacity);
        if (required <= capacity)
        {
            TB_TEST_EXPECT_UINT(result, ==, capacity);
        }
    }
}

TB_TEST(core_size, grow_makes_appending_one_element_at_a_time_cheap)
{
    enum
    {
        APPEND_COUNT = 1000000,
        /* Doubling from one element reaches a million in twenty steps. */
        EXPECTED_REALLOCATION_LIMIT = 21
    };
    size_t capacity = {0};
    size_t reallocation_count = {0};

    for (size_t length = 0; length < APPEND_COUNT; length++)
    {
        const size_t capacity_before = capacity;

        TB_TEST_REQUIRE(tb_size_grow(capacity, length + 1, sizeof(double), &capacity));
        if (capacity != capacity_before)
        {
            reallocation_count++;
        }
    }
    TB_TEST_EXPECT_UINT(capacity, >=, APPEND_COUNT);
    TB_TEST_EXPECT_UINT(reallocation_count, <=, EXPECTED_REALLOCATION_LIMIT);
}

/* --- contract violations ------------------------------------------------------ */

static void add_into_null(void *context)
{
    (void)context;
    (void)tb_size_add(1, 2, NULL);
}

static void multiply_into_null(void *context)
{
    (void)context;
    (void)tb_size_multiply(1, 2, NULL);
}

static void grow_into_null(void *context)
{
    (void)context;
    (void)tb_size_grow(1, 2, 1, NULL);
}

static void grow_an_impossible_capacity(void *context)
{
    size_t result = {0};

    (void)context;
    /* No buffer holds SIZE_MAX elements of two bytes each. */
    (void)tb_size_grow(SIZE_MAX, 1, 2, &result);
}

TB_TEST(core_size, null_outputs_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(add_into_null, NULL, "result != NULL");
    TB_TEST_EXPECT_TRAP(multiply_into_null, NULL, "result != NULL");
    TB_TEST_EXPECT_TRAP(grow_into_null, NULL, "result != NULL");
}

TB_TEST(core_size, growing_a_capacity_that_cannot_exist_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(grow_an_impossible_capacity, NULL, "capacity <= largest_capacity");
}
