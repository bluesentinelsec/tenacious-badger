/* Extended-precision arithmetic is the engine room of the big integers and of
 * exact floating-point conversion, and a wrong digit there is silent: the
 * result is a plausible number. So every operation is checked at its limits
 * (carries that ripple through every digit, zero and one as operands, shifts
 * by every kind of count, the rare steps of long division), against native
 * 64-bit arithmetic where the operands fit, through identities that tie the
 * operations together on random operands of random lengths, and against known
 * answers that Python computed independently. */
#include "badger/xp.h"

#include "badger/assert.h"
#include "badger/str.h"
#include "tb_test.h"
#include "xp_answers.h"

#include <stdint.h>
#include <string.h>

#define MAX_DIGIT UINT32_MAX
#define LONG_LENGTH ((size_t)1024)
#define RANDOM_MAX_LENGTH ((size_t)40)
#define RANDOM_ROUNDS ((size_t)400)
#define NATIVE_ROUNDS ((size_t)2000)

/* Enough digits for every number of the tests built from the known answers:
 * 2^4096 needs 129, 200! and F(2000) fewer. */
#define ANSWER_DIGITS ((size_t)130)

/* Enough bytes for the text of any number in the tests: 2 * LONG_LENGTH
 * digits in base 2. */
#define TEXT_SIZE ((size_t)(2 * LONG_LENGTH * TB_XP_DIGIT_BITS + 64))

/* The lengths that every operation is tried with: one digit, a few, and many. */
static const size_t lengths[] = {1, 2, 3, LONG_LENGTH};

/* --- helpers ------------------------------------------------------------------------ */

static void fill(tb_xp_digit *x, size_t n, tb_xp_digit digit)
{
    for (size_t index = 0; index < n; index++)
    {
        x[index] = digit;
    }
}

static bool all_digits_are(const tb_xp_digit *x, size_t n, tb_xp_digit digit)
{
    for (size_t index = 0; index < n; index++)
    {
        if (x[index] != digit)
        {
            return false;
        }
    }
    return true;
}

static bool is_zero(const tb_xp_digit *x, size_t n)
{
    return all_digits_are(x, n, 0);
}

/* A random digit that is often one where decisions are made: zero, one, the
 * largest digit, or the top bit alone. */
static tb_xp_digit random_digit(void)
{
    static const tb_xp_digit limits[] = {0, 1, MAX_DIGIT, MAX_DIGIT - 1, 0x80000000u, 0x7FFFFFFFu};
    const uint64_t choice = tb_test_random_below(4);

    if (choice == 0)
    {
        return limits[tb_test_random_below(TB_TEST_COUNT_OF(limits))];
    }
    return (tb_xp_digit)tb_test_random_u64();
}

/* A random number of n digits, whose significant length is itself random, so
 * that leading zeros of every count are covered. */
static void random_number(tb_xp_digit *x, size_t n)
{
    const size_t significant = 1 + (size_t)tb_test_random_below(n);

    for (size_t index = 0; index < n; index++)
    {
        x[index] = index < significant ? random_digit() : 0;
    }
}

static size_t random_length(void)
{
    return 1 + (size_t)tb_test_random_below(RANDOM_MAX_LENGTH);
}

/* The text of a number in a base, through the code under test. */
static void text_of(const tb_xp_digit *x, size_t n, unsigned base, char *text, size_t size)
{
    tb_xp_digit scratch[2 * LONG_LENGTH] = {0};
    size_t required = 0;

    TB_TEST_REQUIRE_UINT(n, <=, TB_TEST_COUNT_OF(scratch));
    TB_TEST_REQUIRE_INT(tb_xp_to_string(n, x, base, scratch, text, size, &required), ==, TB_XP_OK);
}

/* A simple reference: the text of a native integer in a base. */
static void native_text(uint64_t value, unsigned base, char *text)
{
    static const char characters[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char reversed[72] = {0};
    size_t count = 0;

    do
    {
        reversed[count] = characters[value % base];
        count++;
        value /= base;
    } while (value != 0);
    for (size_t index = 0; index < count; index++)
    {
        text[index] = reversed[count - 1 - index];
    }
    text[count] = '\0';
}

/* --- results ---------------------------------------------------------------------- */

TB_TEST(core_xp, names_every_result)
{
    TB_TEST_EXPECT_STRING(tb_xp_result_name(TB_XP_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_xp_result_name(TB_XP_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_xp_result_name(TB_XP_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_xp_result_name(TB_XP_BUFFER_TOO_SMALL), ==, "buffer too small");
    TB_TEST_EXPECT_STRING(tb_xp_result_name((tb_xp_result)99), ==, "unknown result");
}

/* --- addition and subtraction ----------------------------------------------------- */

TB_TEST(core_xp, a_carry_ripples_through_every_digit)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit ones[LONG_LENGTH] = {0};
        tb_xp_digit one[LONG_LENGTH] = {0};
        tb_xp_digit zero[LONG_LENGTH] = {0};
        tb_xp_digit z[LONG_LENGTH] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        fill(ones, n, MAX_DIGIT);
        one[0] = 1;

        /* All ones plus one is zero with a carry out. */
        TB_TEST_EXPECT_UINT(tb_xp_add(n, z, ones, one, 0), ==, 1);
        TB_TEST_EXPECT(is_zero(z, n));
        /* The same through the carry in. */
        TB_TEST_EXPECT_UINT(tb_xp_add(n, z, ones, zero, 1), ==, 1);
        TB_TEST_EXPECT(is_zero(z, n));
        TB_TEST_EXPECT_UINT(tb_xp_sum(n, z, ones, 1), ==, 1);
        TB_TEST_EXPECT(is_zero(z, n));
        /* All ones plus all ones is all ones but the lowest bit, with a carry. */
        TB_TEST_EXPECT_UINT(tb_xp_add(n, z, ones, ones, 0), ==, 1);
        TB_TEST_EXPECT_HEX(z[0], ==, MAX_DIGIT - 1);
        TB_TEST_EXPECT(all_digits_are(z + 1, n - 1, MAX_DIGIT));
        /* All ones plus all ones plus a carry is all ones. */
        TB_TEST_EXPECT_UINT(tb_xp_add(n, z, ones, ones, 1), ==, 1);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        /* No carry when the sum fits. */
        TB_TEST_EXPECT_UINT(tb_xp_add(n, z, ones, zero, 0), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        TB_TEST_EXPECT_UINT(tb_xp_sum(n, z, zero, MAX_DIGIT), ==, 0);
        TB_TEST_EXPECT_HEX(z[0], ==, MAX_DIGIT);
        TB_TEST_EXPECT(is_zero(z + 1, n - 1));
    }
}

TB_TEST(core_xp, a_borrow_ripples_through_every_digit)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit one[LONG_LENGTH] = {0};
        tb_xp_digit zero[LONG_LENGTH] = {0};
        tb_xp_digit power[LONG_LENGTH] = {0};
        tb_xp_digit z[LONG_LENGTH] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        one[0] = 1;

        /* Zero minus one wraps to all ones, with a borrow. */
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, z, zero, one, 0), ==, 1);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, z, zero, zero, 1), ==, 1);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        TB_TEST_EXPECT_UINT(tb_xp_diff(n, z, zero, 1), ==, 1);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        /* One minus one is zero, without a borrow. */
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, z, one, one, 0), ==, 0);
        TB_TEST_EXPECT(is_zero(z, n));
        TB_TEST_EXPECT_UINT(tb_xp_diff(n, z, one, 1), ==, 0);
        TB_TEST_EXPECT(is_zero(z, n));
        /* The highest power of 2^32 minus one borrows from every digit
         * between, leaving them all ones. */
        power[n - 1] = 1;
        TB_TEST_EXPECT_UINT(tb_xp_diff(n, z, power, 1), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n - 1, MAX_DIGIT));
        TB_TEST_EXPECT_HEX(z[n - 1], ==, 0);
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, z, power, one, 0), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n - 1, MAX_DIGIT));
        TB_TEST_EXPECT_HEX(z[n - 1], ==, 0);
    }
}

TB_TEST(core_xp, negation_is_the_complement_plus_the_carry)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit zero[LONG_LENGTH] = {0};
        tb_xp_digit one[LONG_LENGTH] = {0};
        tb_xp_digit z[LONG_LENGTH] = {0};
        tb_xp_digit back[LONG_LENGTH] = {0};
        tb_xp_digit sum[LONG_LENGTH] = {0};
        tb_xp_digit x[LONG_LENGTH] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        one[0] = 1;

        /* -0 is 0, with the carry out that says so. */
        TB_TEST_EXPECT_UINT(tb_xp_neg(n, z, zero, 1), ==, 1);
        TB_TEST_EXPECT(is_zero(z, n));
        /* ~0 is all ones. */
        TB_TEST_EXPECT_UINT(tb_xp_neg(n, z, zero, 0), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        /* -1 is all ones. */
        TB_TEST_EXPECT_UINT(tb_xp_neg(n, z, one, 1), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));

        /* x + (-x) is zero, with a carry when x is not zero; and -(-x) is x. */
        random_number(x, n);
        x[0] |= 1;
        (void)tb_xp_neg(n, z, x, 1);
        TB_TEST_EXPECT_UINT(tb_xp_add(n, sum, x, z, 0), ==, 1);
        TB_TEST_EXPECT(is_zero(sum, n));
        (void)tb_xp_neg(n, back, z, 1);
        TB_TEST_EXPECT_INT(tb_xp_compare(n, back, x), ==, 0);
    }
}

/* --- multiplication ---------------------------------------------------------------- */

TB_TEST(core_xp, multiplies_by_zero_one_and_the_largest_digit)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit x[LONG_LENGTH] = {0};
        tb_xp_digit zero[LONG_LENGTH] = {0};
        tb_xp_digit one[LONG_LENGTH] = {0};
        tb_xp_digit z[2 * LONG_LENGTH] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        random_number(x, n);
        one[0] = 1;

        TB_TEST_EXPECT_HEX(tb_xp_product(n, z, x, 0), ==, 0);
        TB_TEST_EXPECT(is_zero(z, n));
        TB_TEST_EXPECT_HEX(tb_xp_product(n, z, x, 1), ==, 0);
        TB_TEST_EXPECT_INT(tb_xp_compare(n, z, x), ==, 0);

        tb_xp_mul(z, n, x, n, zero);
        TB_TEST_EXPECT(is_zero(z, 2 * n));
        tb_xp_mul(z, n, zero, n, x);
        TB_TEST_EXPECT(is_zero(z, 2 * n));
        tb_xp_mul(z, n, x, n, one);
        TB_TEST_EXPECT_INT(tb_xp_compare(n, z, x), ==, 0);
        TB_TEST_EXPECT(is_zero(z + n, n));
        tb_xp_mul(z, n, one, n, x);
        TB_TEST_EXPECT_INT(tb_xp_compare(n, z, x), ==, 0);
        TB_TEST_EXPECT(is_zero(z + n, n));
    }
}

TB_TEST(core_xp, the_largest_numbers_multiply_without_losing_a_carry)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit ones[LONG_LENGTH] = {0};
        tb_xp_digit z[2 * LONG_LENGTH] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        fill(ones, n, MAX_DIGIT);

        /* (2^32n - 1)^2 = 2^64n - 2^(32n+1) + 1: a one, zeros, then a digit
         * one less than the largest, then all ones. The operand is squared
         * through one array. */
        tb_xp_mul(z, n, ones, n, ones);
        TB_TEST_EXPECT_HEX(z[0], ==, 1);
        TB_TEST_EXPECT(is_zero(z + 1, n - 1));
        TB_TEST_EXPECT_HEX(z[n], ==, MAX_DIGIT - 1);
        TB_TEST_EXPECT(all_digits_are(z + n + 1, n - 1, MAX_DIGIT));

        /* (2^32n - 1) * (2^32 - 1) = (2^32 - 2) * 2^32n + ... + 1 in one
         * digit more. */
        TB_TEST_EXPECT_HEX(tb_xp_product(n, z, ones, MAX_DIGIT), ==, MAX_DIGIT - 1);
        TB_TEST_EXPECT_HEX(z[0], ==, 1);
        TB_TEST_EXPECT(all_digits_are(z + 1, n - 1, MAX_DIGIT));
    }
}

TB_TEST(core_xp, operands_of_different_lengths_multiply)
{
    tb_xp_digit x[3] = {0x89ABCDEFu, 0x01234567u, 0};
    tb_xp_digit y[1] = {0x10u};
    tb_xp_digit z[4] = {0};
    tb_xp_digit swapped[4] = {0};

    /* 0x0123456789ABCDEF * 0x10 = 0x123456789ABCDEF0 */
    tb_xp_mul(z, 3, x, 1, y);
    TB_TEST_EXPECT_HEX(z[0], ==, 0x9ABCDEF0u);
    TB_TEST_EXPECT_HEX(z[1], ==, 0x12345678u);
    TB_TEST_EXPECT_HEX(z[2], ==, 0);
    TB_TEST_EXPECT_HEX(z[3], ==, 0);
    tb_xp_mul(swapped, 1, y, 3, x);
    TB_TEST_EXPECT_BYTES(swapped, z, sizeof(z));
}

/* --- division ------------------------------------------------------------------------ */

/* Checks q and r from tb_xp_div: q * y + r == x and r < y. Together these
 * say that q and r are the quotient and remainder, whatever x and y are. */
static void expect_division(size_t n, const tb_xp_digit *x, size_t m, const tb_xp_digit *y)
{
    tb_xp_digit q[LONG_LENGTH] = {0};
    tb_xp_digit r[LONG_LENGTH] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(LONG_LENGTH, LONG_LENGTH)] = {0};
    tb_xp_digit product[2 * LONG_LENGTH] = {0};
    tb_xp_digit widened_r[2 * LONG_LENGTH] = {0};
    tb_xp_digit widened_x[2 * LONG_LENGTH] = {0};

    TB_TEST_REQUIRE_UINT(n, <=, LONG_LENGTH);
    TB_TEST_REQUIRE_UINT(m, <=, LONG_LENGTH);
    tb_xp_div(n, q, x, m, y, r, scratch);

    tb_xp_mul(product, n, q, m, y);
    memcpy(widened_r, r, m * sizeof(tb_xp_digit));
    memcpy(widened_x, x, n * sizeof(tb_xp_digit));
    TB_TEST_EXPECT_UINT(tb_xp_add(n + m, product, product, widened_r, 0), ==, 0);
    TB_TEST_EXPECT_INT(tb_xp_compare(n + m, product, widened_x), ==, 0);

    TB_TEST_EXPECT_INT(tb_xp_compare(m, r, y), <, 0);
}

TB_TEST(core_xp, divides_by_one_by_itself_and_by_a_longer_divisor)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit x[LONG_LENGTH] = {0};
        tb_xp_digit one[LONG_LENGTH] = {0};
        tb_xp_digit longer[LONG_LENGTH + 1] = {0};
        tb_xp_digit q[LONG_LENGTH] = {0};
        tb_xp_digit r[LONG_LENGTH + 1] = {0};
        tb_xp_digit scratch[TB_XP_DIV_SCRATCH(LONG_LENGTH, LONG_LENGTH + 1)] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        random_number(x, n);
        x[0] |= 1;
        one[0] = 1;

        tb_xp_div(n, q, x, n, one, r, scratch);
        TB_TEST_EXPECT_INT(tb_xp_compare(n, q, x), ==, 0);
        TB_TEST_EXPECT(is_zero(r, n));

        tb_xp_div(n, q, x, n, x, r, scratch);
        TB_TEST_EXPECT_HEX(q[0], ==, 1);
        TB_TEST_EXPECT(is_zero(q + 1, n - 1));
        TB_TEST_EXPECT(is_zero(r, n));

        /* A divisor with more significant digits than the dividend leaves a
         * quotient of zero and the dividend as the remainder. */
        longer[n] = 1;
        tb_xp_div(n, q, x, n + 1, longer, r, scratch);
        TB_TEST_EXPECT(is_zero(q, n));
        TB_TEST_EXPECT_INT(tb_xp_compare(n, r, x), ==, 0);
        TB_TEST_EXPECT_HEX(r[n], ==, 0);

        /* Zero divided by anything is zero. */
        fill(x, n, 0);
        tb_xp_div(n, q, x, n, one, r, scratch);
        TB_TEST_EXPECT(is_zero(q, n));
        TB_TEST_EXPECT(is_zero(r, n));
    }
}

TB_TEST(core_xp, long_division_reaches_its_correction_and_add_back_steps)
{
    /* Found by running a copy of the algorithm over operands made of digits
     * near the limits. The estimate of a quotient digit is corrected twice
     * for some, and for others is still one too large after its corrections,
     * which only the add-back step repairs. */
    static const struct
    {
        tb_xp_digit x[4];
        tb_xp_digit y[3];
    } rows[] = {
        /* add back: 2^96 / (2^64 + 1) */
        {{0, 0, 0, 1}, {1, 0, 1}},
        {{0, 0, 0, 1}, {1, 0, 2}},
        {{0, 0, 0, 1}, {1, 0, 0x8000u}},
        {{0, 0, 0, 1}, {1, 0, 0x20000000u}},
        /* two corrections */
        {{0, 0, 0, 1}, {0, 1, 1}},
        {{0, 0, 0, 1}, {1, 1, 1}},
        /* Knuth's and Warren's examples that need the add-back step. */
        {{3, 0, 0x80000000u, 0}, {1, 0, 0x20000000u}},
        {{3, 0, 0x8000u, 0}, {1, 0, 0x2000u}},
        {{0, 0, 0x8000u, 0x7FFFu}, {1, 0, 0x8000u}},
        {{0, 0xFFFFFFFEu, 0, 0x80000000u}, {0xFFFFu, 0, 0x80000000u}},
        /* A remainder that keeps the top digit of a normalized divisor. */
        {{MAX_DIGIT, MAX_DIGIT, MAX_DIGIT, MAX_DIGIT}, {MAX_DIGIT, MAX_DIGIT, 1}},
        {{MAX_DIGIT, MAX_DIGIT, MAX_DIGIT, MAX_DIGIT}, {0, 0, 0x80000000u}},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("row %zu", row);
        expect_division(4, rows[row].x, 3, rows[row].y);
    }
}

TB_TEST(core_xp, add_back_gives_the_known_quotient_and_remainder)
{
    /* 2^96 / (2^64 + 1) = 2^32 - 1, remainder 2^64 - 2^32 + 1. */
    const tb_xp_digit x[4] = {0, 0, 0, 1};
    const tb_xp_digit y[3] = {1, 0, 1};
    tb_xp_digit q[4] = {0};
    tb_xp_digit r[3] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(4, 3)] = {0};

    tb_xp_div(4, q, x, 3, y, r, scratch);
    TB_TEST_EXPECT_HEX(q[0], ==, MAX_DIGIT);
    TB_TEST_EXPECT_HEX(q[1], ==, 0);
    TB_TEST_EXPECT_HEX(q[2], ==, 0);
    TB_TEST_EXPECT_HEX(q[3], ==, 0);
    TB_TEST_EXPECT_HEX(r[0], ==, 1);
    TB_TEST_EXPECT_HEX(r[1], ==, MAX_DIGIT);
    TB_TEST_EXPECT_HEX(r[2], ==, 0);
}

TB_TEST(core_xp, single_digit_division_keeps_the_remainder)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        tb_xp_digit ones[LONG_LENGTH] = {0};
        tb_xp_digit z[LONG_LENGTH] = {0};

        TB_TEST_CONTEXT("%zu digits", n);
        fill(ones, n, MAX_DIGIT);

        TB_TEST_EXPECT_HEX(tb_xp_quotient(n, z, ones, 1), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n, MAX_DIGIT));
        /* 2^32 - 1 divides every number whose digits are all ones, with a
         * quotient of 1 in every digit. */
        TB_TEST_EXPECT_HEX(tb_xp_quotient(n, z, ones, MAX_DIGIT), ==, 0);
        TB_TEST_EXPECT(all_digits_are(z, n, 1));
        /* Dividing by 2 is a shift: a remainder of 1 and the top bit gone. */
        TB_TEST_EXPECT_HEX(tb_xp_quotient(n, z, ones, 2), ==, 1);
        TB_TEST_EXPECT(all_digits_are(z, n - 1, MAX_DIGIT));
        TB_TEST_EXPECT_HEX(z[n - 1], ==, 0x7FFFFFFFu);
    }
}

/* --- comparing, shifting, and length ------------------------------------------------- */

TB_TEST(core_xp, compares_from_the_most_significant_digit)
{
    const tb_xp_digit small[3] = {MAX_DIGIT, MAX_DIGIT, 0};
    const tb_xp_digit large[3] = {0, 0, 1};
    const tb_xp_digit same[3] = {0, 0, 1};

    TB_TEST_EXPECT_INT(tb_xp_compare(3, small, large), <, 0);
    TB_TEST_EXPECT_INT(tb_xp_compare(3, large, small), >, 0);
    TB_TEST_EXPECT_INT(tb_xp_compare(3, large, same), ==, 0);
    TB_TEST_EXPECT_INT(tb_xp_compare(1, small, large), >, 0);
}

TB_TEST(core_xp, shifts_by_every_kind_of_count)
{
    /* Counts of zero, less than a digit, exact multiples of a digit, a digit
     * and a part, all but one bit, the full width, and beyond it. */
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        const size_t n = lengths[row];
        const size_t width = n * TB_XP_DIGIT_BITS;
        const size_t counts[] = {0,  1,  5,         31,    32,        33,
                                 64, 70, width - 1, width, width + 1, SIZE_MAX};
        tb_xp_digit one[LONG_LENGTH] = {0};

        one[0] = 1;
        for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
        {
            const size_t count = counts[index];
            tb_xp_digit left[LONG_LENGTH] = {0};
            tb_xp_digit right[LONG_LENGTH] = {0};

            TB_TEST_CONTEXT("%zu digits, shift by %zu", n, count);
            /* 1 << count is one bit, or nothing when it is shifted out. */
            tb_xp_shift_left(n, left, one, count);
            for (size_t digit = 0; digit < n; digit++)
            {
                const bool holds_the_bit = count < width && digit == count / TB_XP_DIGIT_BITS;
                const tb_xp_digit expected =
                    holds_the_bit ? (tb_xp_digit)1 << (count % TB_XP_DIGIT_BITS) : 0;

                TB_TEST_EXPECT_HEX(left[digit], ==, expected);
            }
            /* Shifting the bit back brings back one, unless it was lost. */
            tb_xp_shift_right(n, right, left, count);
            TB_TEST_EXPECT_HEX(right[0], ==, count < width ? 1 : 0);
            TB_TEST_EXPECT(is_zero(right + 1, n - 1));
        }
    }
}

TB_TEST(core_xp, a_right_shift_rounds_down_and_brings_bits_across_digits)
{
    const tb_xp_digit x[3] = {0x00000001u, 0x80000000u, 0x00000003u};
    tb_xp_digit z[3] = {0};

    tb_xp_shift_right(3, z, x, 1);
    TB_TEST_EXPECT_HEX(z[0], ==, 0);
    TB_TEST_EXPECT_HEX(z[1], ==, 0xC0000000u);
    TB_TEST_EXPECT_HEX(z[2], ==, 1);
    tb_xp_shift_right(3, z, x, 33);
    TB_TEST_EXPECT_HEX(z[0], ==, 0xC0000000u);
    TB_TEST_EXPECT_HEX(z[1], ==, 1);
    TB_TEST_EXPECT_HEX(z[2], ==, 0);
    tb_xp_shift_left(3, z, x, 33);
    TB_TEST_EXPECT_HEX(z[0], ==, 0);
    TB_TEST_EXPECT_HEX(z[1], ==, 2);
    TB_TEST_EXPECT_HEX(z[2], ==, 0);
}

TB_TEST(core_xp, the_significant_length_ignores_leading_zeros)
{
    const tb_xp_digit zero[3] = {0};
    const tb_xp_digit one[3] = {1, 0, 0};
    const tb_xp_digit top[3] = {0, 0, 1};
    const tb_xp_digit middle[3] = {0, 7, 0};

    TB_TEST_EXPECT_UINT(tb_xp_length(3, zero), ==, 1);
    TB_TEST_EXPECT_UINT(tb_xp_length(3, one), ==, 1);
    TB_TEST_EXPECT_UINT(tb_xp_length(3, top), ==, 3);
    TB_TEST_EXPECT_UINT(tb_xp_length(3, middle), ==, 2);
    TB_TEST_EXPECT_UINT(tb_xp_length(1, top), ==, 1);
}

/* --- aliasing ------------------------------------------------------------------------ */

TB_TEST(core_xp, every_permitted_alias_gives_the_separate_result)
{
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        const size_t n = random_length();
        const tb_xp_digit digit = random_digit();
        const size_t bits = (size_t)tb_test_random_below(n * TB_XP_DIGIT_BITS + 40);
        tb_xp_digit x[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit y[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit expected[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit alias[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit both[RANDOM_MAX_LENGTH] = {0};
        unsigned expected_carry = 0;
        tb_xp_digit expected_digit = 0;

        TB_TEST_CONTEXT("round %zu, %zu digits", round, n);
        random_number(x, n);
        random_number(y, n);

        /* z is x, z is y, and z is both. */
        expected_carry = tb_xp_add(n, expected, x, y, 1);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_add(n, alias, alias, y, 1), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));
        memcpy(alias, y, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_add(n, alias, x, alias, 1), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));
        expected_carry = tb_xp_add(n, expected, x, x, 0);
        memcpy(both, x, sizeof(both));
        TB_TEST_EXPECT_UINT(tb_xp_add(n, both, both, both, 0), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(both, expected, sizeof(both));

        expected_carry = tb_xp_sub(n, expected, x, y, 1);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, alias, alias, y, 1), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));
        memcpy(alias, y, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, alias, x, alias, 1), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));
        memcpy(both, x, sizeof(both));
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, both, both, both, 0), ==, 0);
        TB_TEST_EXPECT(is_zero(both, n));

        expected_carry = tb_xp_neg(n, expected, x, 1);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_neg(n, alias, alias, 1), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));

        expected_carry = tb_xp_sum(n, expected, x, digit);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_sum(n, alias, alias, digit), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));

        expected_carry = tb_xp_diff(n, expected, x, digit);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_UINT(tb_xp_diff(n, alias, alias, digit), ==, expected_carry);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));

        expected_digit = tb_xp_product(n, expected, x, digit);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_HEX(tb_xp_product(n, alias, alias, digit), ==, expected_digit);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));

        expected_digit = tb_xp_quotient(n, expected, x, digit | 1u);
        memcpy(alias, x, sizeof(alias));
        TB_TEST_EXPECT_HEX(tb_xp_quotient(n, alias, alias, digit | 1u), ==, expected_digit);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));

        tb_xp_shift_left(n, expected, x, bits);
        memcpy(alias, x, sizeof(alias));
        tb_xp_shift_left(n, alias, alias, bits);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));

        tb_xp_shift_right(n, expected, x, bits);
        memcpy(alias, x, sizeof(alias));
        tb_xp_shift_right(n, alias, alias, bits);
        TB_TEST_EXPECT_BYTES(alias, expected, sizeof(alias));
    }
}

/* --- identities on random operands --------------------------------------------------- */

TB_TEST(core_xp, addition_and_subtraction_undo_each_other)
{
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        const size_t n = random_length();
        tb_xp_digit a[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit b[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit sum[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit back[RANDOM_MAX_LENGTH] = {0};
        unsigned carry = 0;

        TB_TEST_CONTEXT("round %zu, %zu digits", round, n);
        random_number(a, n);
        random_number(b, n);
        /* (a + b) - b == a, the carry of the sum matching the borrow. */
        carry = tb_xp_add(n, sum, a, b, 0);
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, back, sum, b, 0), ==, carry);
        TB_TEST_EXPECT_INT(tb_xp_compare(n, back, a), ==, 0);
        /* a - b is negative exactly when a < b. */
        TB_TEST_EXPECT_UINT(tb_xp_sub(n, back, a, b, 0), ==, tb_xp_compare(n, a, b) < 0 ? 1u : 0u);
    }
}

TB_TEST(core_xp, multiplication_commutes_and_division_undoes_it)
{
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        const size_t n = random_length();
        const size_t m = random_length();
        tb_xp_digit a[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit b[RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit ab[2 * RANDOM_MAX_LENGTH] = {0};
        tb_xp_digit ba[2 * RANDOM_MAX_LENGTH] = {0};

        TB_TEST_CONTEXT("round %zu, %zu and %zu digits", round, n, m);
        random_number(a, n);
        random_number(b, m);
        tb_xp_mul(ab, n, a, m, b);
        tb_xp_mul(ba, m, b, n, a);
        TB_TEST_EXPECT_INT(tb_xp_compare(n + m, ab, ba), ==, 0);

        /* (a / b) * b + a % b == a, for any dividend and nonzero divisor. */
        if (tb_xp_length(m, b) == 1 && b[0] == 0)
        {
            b[0] = 1;
        }
        expect_division(n, a, m, b);
        expect_division(n + m, ab, m, b);
    }
}

TB_TEST(core_xp, long_operands_divide_exactly)
{
    /* Long operands make many quotient digits, each with its own estimate. */
    tb_xp_digit a[LONG_LENGTH] = {0};
    tb_xp_digit b[LONG_LENGTH / 2] = {0};

    for (size_t round = 0; round < 4; round++)
    {
        TB_TEST_CONTEXT("round %zu", round);
        random_number(a, LONG_LENGTH);
        random_number(b, LONG_LENGTH / 2);
        b[0] |= 1;
        expect_division(LONG_LENGTH, a, LONG_LENGTH / 2, b);
    }
}

TB_TEST(core_xp, a_left_shift_is_undone_by_a_right_shift)
{
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        const size_t n = random_length();
        const size_t bits = (size_t)tb_test_random_below(n * TB_XP_DIGIT_BITS);
        tb_xp_digit x[2 * RANDOM_MAX_LENGTH + 1] = {0};
        tb_xp_digit shifted[2 * RANDOM_MAX_LENGTH + 1] = {0};
        tb_xp_digit back[2 * RANDOM_MAX_LENGTH + 1] = {0};
        tb_xp_digit power[2 * RANDOM_MAX_LENGTH + 1] = {0};
        tb_xp_digit one[2 * RANDOM_MAX_LENGTH + 1] = {0};
        tb_xp_digit product[3 * RANDOM_MAX_LENGTH + 2] = {0};
        const size_t wide = 2 * n + 1;

        TB_TEST_CONTEXT("round %zu, %zu digits, shift by %zu", round, n, bits);
        /* The number fills the low n digits of a number twice as wide, so
         * no bit is shifted out. */
        random_number(x, n);
        tb_xp_shift_left(wide, shifted, x, bits);
        tb_xp_shift_right(wide, back, shifted, bits);
        TB_TEST_EXPECT_INT(tb_xp_compare(wide, back, x), ==, 0);

        /* A left shift multiplies by a power of two. */
        one[0] = 1;
        tb_xp_shift_left(wide, power, one, bits);
        tb_xp_mul(product, n, x, wide, power);
        TB_TEST_EXPECT_INT(tb_xp_compare(wide, product, shifted), ==, 0);
        TB_TEST_EXPECT(is_zero(product + wide, n));
    }
}

/* --- native arithmetic as a reference ------------------------------------------------ */

static uint64_t random_u64_near_limits(void)
{
    static const uint64_t limits[] = {
        0, 1, UINT32_MAX, (uint64_t)UINT32_MAX + 1, UINT64_MAX, UINT64_MAX - 1, (uint64_t)1 << 63};

    if (tb_test_random_below(4) == 0)
    {
        return limits[tb_test_random_below(TB_TEST_COUNT_OF(limits))];
    }
    /* A random number of significant bits, so that small values are common. */
    return tb_test_random_u64() >> tb_test_random_below(64);
}

TB_TEST(core_xp, agrees_with_native_arithmetic_where_it_fits)
{
    for (size_t round = 0; round < NATIVE_ROUNDS; round++)
    {
        const uint64_t left = random_u64_near_limits();
        const uint64_t right = random_u64_near_limits();
        const unsigned shift = (unsigned)tb_test_random_below(64);
        tb_xp_digit x[2] = {0};
        tb_xp_digit y[2] = {0};
        tb_xp_digit z[4] = {0};
        tb_xp_digit q[2] = {0};
        tb_xp_digit r[2] = {0};
        tb_xp_digit scratch[TB_XP_DIV_SCRATCH(2, 2)] = {0};
        uint64_t value = 0;

        TB_TEST_CONTEXT("round %zu: 0x%llx and 0x%llx", round, (unsigned long long)left,
                        (unsigned long long)right);
        TB_TEST_REQUIRE(tb_xp_from_u64(2, x, left));
        TB_TEST_REQUIRE(tb_xp_from_u64(2, y, right));

        TB_TEST_EXPECT_UINT(tb_xp_add(2, z, x, y, 0), ==, left + right < left ? 1u : 0u);
        TB_TEST_REQUIRE(tb_xp_to_u64(2, z, &value));
        TB_TEST_EXPECT_HEX(value, ==, left + right);

        TB_TEST_EXPECT_UINT(tb_xp_sub(2, z, x, y, 0), ==, left < right ? 1u : 0u);
        TB_TEST_REQUIRE(tb_xp_to_u64(2, z, &value));
        TB_TEST_EXPECT_HEX(value, ==, left - right);

        TB_TEST_EXPECT_INT(tb_xp_compare(2, x, y), ==, left < right ? -1 : (left > right ? 1 : 0));

        /* 32-bit factors make a product that fits in 64 bits. */
        tb_xp_mul(z, 1, x, 1, y);
        TB_TEST_REQUIRE(tb_xp_to_u64(2, z, &value));
        TB_TEST_EXPECT_HEX(value, ==, (left & UINT32_MAX) * (right & UINT32_MAX));

        tb_xp_shift_left(2, z, x, shift);
        TB_TEST_REQUIRE(tb_xp_to_u64(2, z, &value));
        TB_TEST_EXPECT_HEX(value, ==, left << shift);
        tb_xp_shift_right(2, z, x, shift);
        TB_TEST_REQUIRE(tb_xp_to_u64(2, z, &value));
        TB_TEST_EXPECT_HEX(value, ==, left >> shift);

        if (right != 0)
        {
            tb_xp_div(2, q, x, 2, y, r, scratch);
            TB_TEST_REQUIRE(tb_xp_to_u64(2, q, &value));
            TB_TEST_EXPECT_HEX(value, ==, left / right);
            TB_TEST_REQUIRE(tb_xp_to_u64(2, r, &value));
            TB_TEST_EXPECT_HEX(value, ==, left % right);
        }
        if ((right & UINT32_MAX) != 0)
        {
            const tb_xp_digit divisor = (tb_xp_digit)right;
            const tb_xp_digit remainder = tb_xp_quotient(2, z, x, divisor);

            TB_TEST_REQUIRE(tb_xp_to_u64(2, z, &value));
            TB_TEST_EXPECT_HEX(value, ==, left / divisor);
            TB_TEST_EXPECT_HEX(remainder, ==, left % divisor);
        }
    }
}

TB_TEST(core_xp, native_integers_convert_both_ways)
{
    tb_xp_digit one_digit[1] = {0};
    tb_xp_digit three[3] = {MAX_DIGIT, MAX_DIGIT, MAX_DIGIT};
    uint64_t value = 0;

    /* A value that needs two digits does not fit in one; the low digit is kept. */
    TB_TEST_EXPECT(!tb_xp_from_u64(1, one_digit, 0x123456789ull));
    TB_TEST_EXPECT_HEX(one_digit[0], ==, 0x23456789u);
    TB_TEST_EXPECT(tb_xp_from_u64(1, one_digit, UINT32_MAX));
    TB_TEST_EXPECT(tb_xp_to_u64(1, one_digit, &value));
    TB_TEST_EXPECT_HEX(value, ==, UINT32_MAX);

    /* Every digit above the value is cleared. */
    TB_TEST_EXPECT(tb_xp_from_u64(3, three, 5));
    TB_TEST_EXPECT_HEX(three[1], ==, 0);
    TB_TEST_EXPECT_HEX(three[2], ==, 0);
    TB_TEST_EXPECT(tb_xp_to_u64(3, three, &value));
    TB_TEST_EXPECT_HEX(value, ==, 5);

    /* A third digit means the number does not fit; the low 64 bits are kept. */
    three[2] = 1;
    TB_TEST_EXPECT(!tb_xp_to_u64(3, three, &value));
    TB_TEST_EXPECT_HEX(value, ==, 5);
}

/* --- text ------------------------------------------------------------------------------- */

typedef enum known_kind
{
    KNOWN_FACTORIAL,
    KNOWN_POWER_OF_TWO,
    KNOWN_FIBONACCI
} known_kind;

/* Computes n!, 2^n, or F(n) with the code under test. */
static void compute_known(known_kind kind, size_t argument, tb_xp_digit *z)
{
    tb_xp_digit previous[ANSWER_DIGITS] = {0};
    tb_xp_digit next[ANSWER_DIGITS] = {0};

    memset(z, 0, ANSWER_DIGITS * sizeof(tb_xp_digit));
    switch (kind)
    {
    case KNOWN_FACTORIAL:
        z[0] = 1;
        for (size_t factor = 2; factor <= argument; factor++)
        {
            TB_TEST_REQUIRE_HEX(tb_xp_product(ANSWER_DIGITS, z, z, (tb_xp_digit)factor), ==, 0);
        }
        break;
    case KNOWN_POWER_OF_TWO:
        z[0] = 1;
        tb_xp_shift_left(ANSWER_DIGITS, z, z, argument);
        break;
    case KNOWN_FIBONACCI:
        /* z holds F(i) and previous F(i - 1), starting from F(0) = 0 and
         * F(-1) = 1. */
        previous[0] = 1;
        for (size_t step = 0; step < argument; step++)
        {
            TB_TEST_REQUIRE_UINT(tb_xp_add(ANSWER_DIGITS, next, z, previous, 0), ==, 0);
            memcpy(previous, z, sizeof(previous));
            memcpy(z, next, sizeof(next));
        }
        break;
    }
}

static void expect_known_answers(known_kind kind, const xp_answer *answers, size_t count)
{
    for (size_t row = 0; row < count; row++)
    {
        const xp_answer *const answer = &answers[row];
        tb_xp_digit computed[ANSWER_DIGITS] = {0};
        tb_xp_digit parsed[ANSWER_DIGITS] = {0};
        char text[TEXT_SIZE] = {0};

        TB_TEST_CONTEXT("argument %zu", answer->argument);
        compute_known(kind, answer->argument, computed);

        text_of(computed, ANSWER_DIGITS, 10, text, sizeof(text));
        TB_TEST_EXPECT_STRING(text, ==, answer->decimal);
        text_of(computed, ANSWER_DIGITS, 16, text, sizeof(text));
        TB_TEST_EXPECT_STRING(text, ==, answer->hexadecimal);

        TB_TEST_REQUIRE_INT(tb_xp_from_string(ANSWER_DIGITS, parsed, answer->decimal,
                                              tb_str_length(answer->decimal), 10),
                            ==, TB_XP_OK);
        TB_TEST_EXPECT_INT(tb_xp_compare(ANSWER_DIGITS, parsed, computed), ==, 0);
        TB_TEST_REQUIRE_INT(tb_xp_from_string(ANSWER_DIGITS, parsed, answer->hexadecimal,
                                              tb_str_length(answer->hexadecimal), 16),
                            ==, TB_XP_OK);
        TB_TEST_EXPECT_INT(tb_xp_compare(ANSWER_DIGITS, parsed, computed), ==, 0);
    }
}

TB_TEST(core_xp, factorials_match_the_known_answers)
{
    expect_known_answers(KNOWN_FACTORIAL, xp_factorials, TB_TEST_COUNT_OF(xp_factorials));
}

TB_TEST(core_xp, powers_of_two_match_the_known_answers)
{
    expect_known_answers(KNOWN_POWER_OF_TWO, xp_powers_of_two, TB_TEST_COUNT_OF(xp_powers_of_two));
}

TB_TEST(core_xp, fibonacci_numbers_match_the_known_answers)
{
    expect_known_answers(KNOWN_FIBONACCI, xp_fibonacci, TB_TEST_COUNT_OF(xp_fibonacci));
}

TB_TEST(core_xp, text_agrees_with_native_conversion_in_every_base)
{
    for (unsigned base = TB_XP_BASE_MIN; base <= TB_XP_BASE_MAX; base++)
    {
        for (size_t round = 0; round < 50; round++)
        {
            const uint64_t value = random_u64_near_limits();
            tb_xp_digit x[2] = {0};
            tb_xp_digit parsed[2] = {0};
            char expected[72] = {0};
            char text[72] = {0};

            TB_TEST_CONTEXT("base %u, 0x%llx", base, (unsigned long long)value);
            (void)tb_xp_from_u64(2, x, value);
            native_text(value, base, expected);
            text_of(x, 2, base, text, sizeof(text));
            TB_TEST_EXPECT_STRING(text, ==, expected);
            TB_TEST_REQUIRE_INT(
                tb_xp_from_string(2, parsed, expected, tb_str_length(expected), base), ==,
                TB_XP_OK);
            TB_TEST_EXPECT_INT(tb_xp_compare(2, parsed, x), ==, 0);
        }
    }
}

TB_TEST(core_xp, text_round_trips_in_every_base)
{
    for (unsigned base = TB_XP_BASE_MIN; base <= TB_XP_BASE_MAX; base++)
    {
        for (size_t round = 0; round < 20; round++)
        {
            const size_t n = random_length();
            tb_xp_digit x[RANDOM_MAX_LENGTH] = {0};
            tb_xp_digit parsed[RANDOM_MAX_LENGTH] = {0};
            char text[TEXT_SIZE] = {0};

            TB_TEST_CONTEXT("base %u, round %zu, %zu digits", base, round, n);
            random_number(x, n);
            text_of(x, n, base, text, sizeof(text));
            TB_TEST_EXPECT(text[0] != '0' || text[1] == '\0');
            TB_TEST_REQUIRE_INT(tb_xp_from_string(n, parsed, text, tb_str_length(text), base), ==,
                                TB_XP_OK);
            TB_TEST_EXPECT_INT(tb_xp_compare(n, parsed, x), ==, 0);
        }
    }
}

TB_TEST(core_xp, the_string_size_is_always_enough)
{
    /* The largest number of each length needs the most text; the size must
     * cover it and stay within its documented margin. */
    for (unsigned base = TB_XP_BASE_MIN; base <= TB_XP_BASE_MAX; base++)
    {
        for (size_t n = 1; n <= 70; n += (n < 10 ? 1 : 7))
        {
            tb_xp_digit ones[70] = {0};
            tb_xp_digit scratch[70] = {0};
            char text[TEXT_SIZE] = {0};
            size_t required = 0;
            const size_t size = tb_xp_string_size(n, base);

            TB_TEST_CONTEXT("base %u, %zu digits", base, n);
            fill(ones, n, MAX_DIGIT);
            TB_TEST_REQUIRE_INT(
                tb_xp_to_string(n, ones, base, scratch, text, sizeof(text), &required), ==,
                TB_XP_OK);
            TB_TEST_EXPECT_UINT(size, >=, required);
            TB_TEST_EXPECT_UINT(size, <=, required + 2 + n / 2048);
        }
    }
}

TB_TEST(core_xp, letters_are_read_in_either_case_and_leading_zeros_are_allowed)
{
    tb_xp_digit x[2] = {0};
    uint64_t value = 0;

    TB_TEST_REQUIRE_INT(tb_xp_from_string(2, x, "00DeadBEEF", 10, 16), ==, TB_XP_OK);
    TB_TEST_EXPECT(tb_xp_to_u64(2, x, &value));
    TB_TEST_EXPECT_HEX(value, ==, 0xDEADBEEFu);
    TB_TEST_REQUIRE_INT(tb_xp_from_string(2, x, "zZ", 2, 36), ==, TB_XP_OK);
    TB_TEST_EXPECT(tb_xp_to_u64(2, x, &value));
    TB_TEST_EXPECT_UINT(value, ==, 35 * 36 + 35);
    TB_TEST_REQUIRE_INT(tb_xp_from_string(2, x, "0000", 4, 10), ==, TB_XP_OK);
    TB_TEST_EXPECT(tb_xp_to_u64(2, x, &value));
    TB_TEST_EXPECT_UINT(value, ==, 0);
}

TB_TEST(core_xp, rejects_text_that_is_not_digits_and_leaves_the_number_alone)
{
    static const struct
    {
        const char *text;
        unsigned base;
    } rows[] = {
        {"2", 2},   {"12", 2},  {"9", 9},   {"a", 10},  {"g", 16},   {"G", 16},
        {"-1", 10}, {"+1", 10}, {" 1", 10}, {"1 ", 10}, {"0x1", 16}, {"1.0", 10},
        {"z", 35},  {"{", 36},  {"@", 36},  {"/", 10},  {":", 10},   {"`", 36},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_xp_digit x[2] = {0x11111111u, 0x22222222u};

        TB_TEST_CONTEXT("\"%s\" in base %u", rows[row].text, rows[row].base);
        TB_TEST_EXPECT_INT(
            tb_xp_from_string(2, x, rows[row].text, tb_str_length(rows[row].text), rows[row].base),
            ==, TB_XP_INVALID);
        TB_TEST_EXPECT_HEX(x[0], ==, 0x11111111u);
        TB_TEST_EXPECT_HEX(x[1], ==, 0x22222222u);
    }
}

TB_TEST(core_xp, rejects_empty_text_and_text_with_a_nul_byte)
{
    tb_xp_digit x[1] = {7};
    const char with_nul[3] = {'1', '\0', '2'};

    TB_TEST_EXPECT_INT(tb_xp_from_string(1, x, NULL, 0, 10), ==, TB_XP_INVALID);
    TB_TEST_EXPECT_INT(tb_xp_from_string(1, x, "", 0, 10), ==, TB_XP_INVALID);
    TB_TEST_EXPECT_INT(tb_xp_from_string(1, x, with_nul, sizeof(with_nul), 10), ==, TB_XP_INVALID);
    TB_TEST_EXPECT_HEX(x[0], ==, 7);
}

TB_TEST(core_xp, rejects_a_value_one_past_the_largest_that_fits)
{
    tb_xp_digit x[2] = {0};
    char power_of_two[65] = {0}; /* 2^64 in base 2: a one and 64 zeros */
    uint64_t value = 0;

    /* 2^64 - 1 fits in two digits and 2^64 does not, in base 10 and base 2. */
    TB_TEST_EXPECT_INT(tb_xp_from_string(2, x, "18446744073709551615", 20, 10), ==, TB_XP_OK);
    TB_TEST_EXPECT(tb_xp_to_u64(2, x, &value));
    TB_TEST_EXPECT_HEX(value, ==, UINT64_MAX);
    TB_TEST_EXPECT_INT(tb_xp_from_string(2, x, "18446744073709551616", 20, 10), ==,
                       TB_XP_TOO_LARGE);
    memset(power_of_two, '0', sizeof(power_of_two));
    power_of_two[0] = '1';
    TB_TEST_EXPECT_INT(tb_xp_from_string(2, x, power_of_two, sizeof(power_of_two), 2), ==,
                       TB_XP_TOO_LARGE);
    TB_TEST_EXPECT_INT(tb_xp_from_string(2, x, power_of_two + 1, sizeof(power_of_two) - 1, 2), ==,
                       TB_XP_OK);
    /* Leading zeros do not count against the length. */
    TB_TEST_EXPECT_INT(
        tb_xp_from_string(2, x, "000000000000000000000018446744073709551615", 42, 10), ==,
        TB_XP_OK);
    TB_TEST_EXPECT_INT(tb_xp_from_string(1, x, "4294967296", 10, 10), ==, TB_XP_TOO_LARGE);
}

TB_TEST(core_xp, a_buffer_that_is_too_small_is_left_untouched)
{
    const tb_xp_digit x[2] = {0, 1}; /* 2^32 = 4294967296: ten digits */
    tb_xp_digit scratch[2] = {0};
    char buffer[11] = {0};
    size_t required = 0;

    memset(buffer, 'x', sizeof(buffer));
    TB_TEST_EXPECT_INT(tb_xp_to_string(2, x, 10, scratch, buffer, 10, &required), ==,
                       TB_XP_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_UINT(required, ==, 11);
    for (size_t index = 0; index < sizeof(buffer); index++)
    {
        TB_TEST_EXPECT_INT(buffer[index], ==, 'x');
    }
    TB_TEST_EXPECT_INT(tb_xp_to_string(2, x, 10, scratch, NULL, 0, &required), ==,
                       TB_XP_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_UINT(required, ==, 11);

    /* Exactly the size required is enough, though less than the bound. */
    TB_TEST_REQUIRE_UINT(tb_xp_string_size(2, 10), >, sizeof(buffer));
    TB_TEST_EXPECT_INT(tb_xp_to_string(2, x, 10, scratch, buffer, sizeof(buffer), &required), ==,
                       TB_XP_OK);
    TB_TEST_EXPECT_STRING(buffer, ==, "4294967296");
    TB_TEST_EXPECT_UINT(required, ==, 11);
}

TB_TEST(core_xp, zero_is_written_as_one_digit)
{
    const tb_xp_digit zero[3] = {0};
    char text[8] = {0};

    for (unsigned base = TB_XP_BASE_MIN; base <= TB_XP_BASE_MAX; base++)
    {
        TB_TEST_CONTEXT("base %u", base);
        text_of(zero, 3, base, text, sizeof(text));
        TB_TEST_EXPECT_STRING(text, ==, "0");
    }
}

TB_TEST(core_xp, a_piece_of_zeros_inside_a_number_is_written_in_full)
{
    /* 10^19 + 1 in base 10 crosses a piece of nine digits that are all zero. */
    tb_xp_digit x[2] = {0};
    char text[32] = {0};

    TB_TEST_REQUIRE(tb_xp_from_u64(2, x, 10000000000000000001ull));
    text_of(x, 2, 10, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "10000000000000000001");
    TB_TEST_REQUIRE(tb_xp_from_u64(2, x, 1000000000ull));
    text_of(x, 2, 10, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "1000000000");
}

/* --- contract violations -------------------------------------------------------------- */

static void add_with_no_digits(void *context)
{
    tb_xp_digit digit = 0;

    (void)context;
    (void)tb_xp_add(0, &digit, &digit, &digit, 0);
}

static void add_with_a_carry_of_two(void *context)
{
    tb_xp_digit digit = 0;

    (void)context;
    (void)tb_xp_add(1, &digit, &digit, &digit, 2);
}

static void add_into_a_partial_overlap(void *context)
{
    tb_xp_digit digits[3] = {0};

    (void)context;
    (void)tb_xp_add(2, digits + 1, digits, digits, 0);
}

static void sub_from_null(void *context)
{
    tb_xp_digit digit = 0;

    (void)context;
    (void)tb_xp_sub(1, &digit, NULL, &digit, 0);
}

static void quotient_by_zero(void *context)
{
    tb_xp_digit digit = 1;

    (void)context;
    (void)tb_xp_quotient(1, &digit, &digit, 0);
}

static void divide_by_zero(void *context)
{
    const tb_xp_digit x[2] = {1, 2};
    const tb_xp_digit y[2] = {0};
    tb_xp_digit q[2] = {0};
    tb_xp_digit r[2] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(2, 2)] = {0};

    (void)context;
    tb_xp_div(2, q, x, 2, y, r, scratch);
}

static void divide_into_the_dividend(void *context)
{
    tb_xp_digit x[2] = {1, 2};
    const tb_xp_digit y[2] = {3};
    tb_xp_digit r[2] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(2, 2)] = {0};

    (void)context;
    tb_xp_div(2, x, x, 2, y, r, scratch);
}

static void divide_with_the_remainder_in_the_scratch(void *context)
{
    const tb_xp_digit x[2] = {1, 2};
    const tb_xp_digit y[2] = {3};
    tb_xp_digit q[2] = {0};
    tb_xp_digit shared[TB_XP_DIV_SCRATCH(2, 2)] = {0};

    (void)context;
    tb_xp_div(2, q, x, 2, y, shared + 1, shared);
}

static void divide_with_the_scratch_on_the_divisor(void *context)
{
    const tb_xp_digit x[2] = {1, 2};
    tb_xp_digit q[2] = {0};
    tb_xp_digit r[2] = {0};
    tb_xp_digit scratch[TB_XP_DIV_SCRATCH(2, 2)] = {3};

    (void)context;
    tb_xp_div(2, q, x, 2, scratch, r, scratch);
}

static void multiply_into_an_operand(void *context)
{
    tb_xp_digit digits[4] = {1, 2, 0, 0};

    (void)context;
    tb_xp_mul(digits, 2, digits, 2, digits + 2);
}

static void parse_in_base_one(void *context)
{
    tb_xp_digit digit = 0;

    (void)context;
    (void)tb_xp_from_string(1, &digit, "0", 1, 1);
}

static void write_in_base_37(void *context)
{
    const tb_xp_digit digit = 0;
    tb_xp_digit scratch = 0;
    char text[8] = {0};
    size_t required = 0;

    (void)context;
    (void)tb_xp_to_string(1, &digit, 37, &scratch, text, sizeof(text), &required);
}

static void write_with_the_scratch_on_the_number(void *context)
{
    tb_xp_digit digit = 5;
    char text[8] = {0};
    size_t required = 0;

    (void)context;
    (void)tb_xp_to_string(1, &digit, 10, &digit, text, sizeof(text), &required);
}

TB_TEST(core_xp, contract_violations_are_caught)
{
    static const struct
    {
        tb_test_trap_body *body;
        const char *message;
    } rows[] = {
        {add_with_no_digits, "n > 0"},
        {add_with_a_carry_of_two, "carry <= 1"},
        {add_into_a_partial_overlap, "xp_same_or_apart"},
        {sub_from_null, "x != NULL"},
        {quotient_by_zero, "division by zero"},
        {divide_by_zero, "division by zero"},
        {divide_into_the_dividend, "the quotient must not overlap"},
        {divide_with_the_remainder_in_the_scratch, "the remainder must not overlap"},
        {divide_with_the_scratch_on_the_divisor, "the scratch digits must not overlap"},
        {multiply_into_an_operand, "the product must not overlap"},
        {parse_in_base_one, "xp_base_is_valid(base)"},
        {write_in_base_37, "xp_base_is_valid(base)"},
        {write_with_the_scratch_on_the_number, "the scratch digits must not overlap"},
    };

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("row %zu", row);
        TB_TEST_EXPECT_TRAP(rows[row].body, NULL, rows[row].message);
    }
}
