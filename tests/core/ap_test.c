/* Arbitrary-precision integers are only useful if they are exactly right for
 * every sign and size, so they are checked several independent ways: every
 * operation with every combination of signs and zero, against native 64-bit
 * arithmetic where the operands and results fit, through identities on random
 * operands, against known answers, and against vectors that Python's integers
 * computed. Division is checked to round toward negative infinity, values are
 * checked to be canonical after every operation (no negative zero, no leading
 * zero digits), and every allocation is checked to come back to its allocator,
 * including when one fails part of the way through an operation. */
#include "badger/ap.h"

#include "ap_vectors.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_allocator.h"
#include "xp_answers.h"

#include <stdint.h>
#include <string.h>

#define TEXT_SIZE ((size_t)8192)
#define RANDOM_ROUNDS ((size_t)300)
#define NATIVE_ROUNDS ((size_t)2000)

/* --- helpers ------------------------------------------------------------------------ */

/* A value from text in base 16 that the test knows to be valid. */
static tb_ap *hex(const char *text)
{
    tb_ap *value = NULL;

    TB_TEST_REQUIRE_INT(tb_ap_from_string(NULL, text, tb_str_length(text), 16, &value), ==,
                        TB_AP_OK);
    return value;
}

static void text_of(const tb_ap *x, unsigned base, char *text, size_t size)
{
    size_t required = 0;

    TB_TEST_REQUIRE_INT(tb_ap_to_string(x, base, text, size, &required), ==, TB_AP_OK);
}

/* Checks a value's text in base 16, then frees the value. */
static void expect_hex_and_free(tb_ap *value, const char *expected)
{
    char text[TEXT_SIZE] = {0};

    text_of(value, 16, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, expected);
    tb_ap_free(value);
}

/* A value has one representation: zero is positive and has one digit, and
 * no value has leading zero digits. Comparison relies on it, so a value
 * that broke it would compare unequal with the same value read from text. */
static void expect_canonical(const tb_ap *x)
{
    char text[TEXT_SIZE] = {0};
    tb_ap *reread = NULL;

    text_of(x, 16, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, !=, "-0");
    TB_TEST_EXPECT(text[0] != '0' || text[1] == '\0');
    TB_TEST_EXPECT(text[0] != '-' || text[1] != '0');
    reread = hex(text);
    TB_TEST_EXPECT_INT(tb_ap_compare(x, reread), ==, 0);
    tb_ap_free(reread);
}

static void expect_equals_i64(tb_ap *value, int64_t expected)
{
    int64_t actual = 0;

    expect_canonical(value);
    TB_TEST_EXPECT(tb_ap_to_i64(value, &actual));
    TB_TEST_EXPECT_INT(actual, ==, expected);
    TB_TEST_EXPECT_INT(tb_ap_compare_i64(value, expected), ==, 0);
    tb_ap_free(value);
}

static int64_t floor_div(int64_t x, int64_t y)
{
    const int64_t quotient = x / y;

    if ((x % y != 0) && ((x < 0) != (y < 0)))
    {
        return quotient - 1;
    }
    return quotient;
}

static int64_t floor_mod(int64_t x, int64_t y)
{
    return x - floor_div(x, y) * y;
}

/* A random value: random digits in base 16 of a random length and sign. */
static tb_ap *random_value(void)
{
    static const char digits[] = "0123456789abcdef";
    char text[200] = {0};
    const size_t length = 1 + (size_t)tb_test_random_below(150);
    size_t position = 0;

    if (tb_test_random_below(2) == 0)
    {
        text[position] = '-';
        position++;
    }
    for (size_t index = 0; index < length; index++)
    {
        /* Runs of f and 0 make carries and borrows that ripple. */
        const uint64_t roll = tb_test_random_below(8);
        const char digit = roll == 0 ? 'f' : (roll == 1 ? '0' : digits[tb_test_random_below(16)]);

        text[position] = digit;
        position++;
    }
    return hex(text);
}

static tb_ap *random_nonzero_value(void)
{
    tb_ap *value = random_value();

    while (tb_ap_sign(value) == 0)
    {
        tb_ap_free(value);
        value = random_value();
    }
    return value;
}

/* --- results ------------------------------------------------------------------------ */

TB_TEST(core_ap, names_every_result)
{
    TB_TEST_EXPECT_STRING(tb_ap_result_name(TB_AP_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_ap_result_name(TB_AP_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_ap_result_name(TB_AP_BUFFER_TOO_SMALL), ==, "buffer too small");
    TB_TEST_EXPECT_STRING(tb_ap_result_name((tb_ap_result)99), ==, "unknown result");
}

/* --- native integers ------------------------------------------------------------------ */

TB_TEST(core_ap, native_integers_round_trip_at_their_limits)
{
    static const int64_t signed_values[] = {0,
                                            1,
                                            -1,
                                            INT64_MAX,
                                            INT64_MIN,
                                            INT64_MIN + 1,
                                            (int64_t)UINT32_MAX,
                                            -(int64_t)UINT32_MAX,
                                            (int64_t)UINT32_MAX + 1,
                                            -((int64_t)UINT32_MAX + 1)};
    static const uint64_t unsigned_values[] = {0, 1, UINT32_MAX, (uint64_t)UINT32_MAX + 1,
                                               UINT64_MAX};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(signed_values); row++)
    {
        TB_TEST_CONTEXT("%lld", (long long)signed_values[row]);
        expect_equals_i64(tb_ap_from_i64(NULL, signed_values[row]), signed_values[row]);
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(unsigned_values); row++)
    {
        tb_ap *const value = tb_ap_from_u64(NULL, unsigned_values[row]);
        uint64_t back = 0;

        TB_TEST_CONTEXT("%llu", (unsigned long long)unsigned_values[row]);
        expect_canonical(value);
        TB_TEST_EXPECT(tb_ap_to_u64(value, &back));
        TB_TEST_EXPECT_UINT(back, ==, unsigned_values[row]);
        tb_ap_free(value);
    }
}

TB_TEST(core_ap, a_value_that_does_not_fit_leaves_the_native_integer_alone)
{
    static const char *const too_large_for_i64[] = {"8000000000000000", "-8000000000000001",
                                                    "10000000000000000", "-10000000000000000"};
    static const char *const too_large_for_u64[] = {"10000000000000000", "-1",
                                                    "100000000000000000000000000"};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(too_large_for_i64); row++)
    {
        tb_ap *const value = hex(too_large_for_i64[row]);
        int64_t native = 42;

        TB_TEST_CONTEXT("%s", too_large_for_i64[row]);
        TB_TEST_EXPECT(!tb_ap_to_i64(value, &native));
        TB_TEST_EXPECT_INT(native, ==, 42);
        tb_ap_free(value);
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(too_large_for_u64); row++)
    {
        tb_ap *const value = hex(too_large_for_u64[row]);
        uint64_t native = 42;

        TB_TEST_CONTEXT("%s", too_large_for_u64[row]);
        TB_TEST_EXPECT(!tb_ap_to_u64(value, &native));
        TB_TEST_EXPECT_UINT(native, ==, 42);
        tb_ap_free(value);
    }
}

/* --- every sign combination ----------------------------------------------------------- */

/* Small operands of every sign, zero among them, whose results all fit in 64
 * bits, so that native arithmetic is the reference: 3037000499 is the largest
 * integer whose square fits. */
static const int64_t sign_operands[] = {0,
                                        1,
                                        -1,
                                        2,
                                        -2,
                                        7,
                                        -7,
                                        6,
                                        -6,
                                        3,
                                        -3,
                                        2147483647LL,
                                        -2147483648LL,
                                        3037000499LL,
                                        -3037000499LL,
                                        1000000007LL,
                                        -1000000007LL};

TB_TEST(core_ap, every_operation_agrees_with_native_arithmetic_for_every_sign)
{
    for (size_t left_row = 0; left_row < TB_TEST_COUNT_OF(sign_operands); left_row++)
    {
        for (size_t right_row = 0; right_row < TB_TEST_COUNT_OF(sign_operands); right_row++)
        {
            const int64_t left = sign_operands[left_row];
            const int64_t right = sign_operands[right_row];
            tb_ap *const x = tb_ap_from_i64(NULL, left);
            tb_ap *const y = tb_ap_from_i64(NULL, right);

            TB_TEST_CONTEXT("%lld and %lld", (long long)left, (long long)right);
            expect_equals_i64(tb_ap_add(x, y), left + right);
            expect_equals_i64(tb_ap_sub(x, y), left - right);
            expect_equals_i64(tb_ap_mul(x, y), left * right);
            expect_equals_i64(tb_ap_add_i64(x, right), left + right);
            expect_equals_i64(tb_ap_sub_i64(x, right), left - right);
            expect_equals_i64(tb_ap_mul_i64(x, right), left * right);
            expect_equals_i64(tb_ap_neg(x), -left);
            TB_TEST_EXPECT_INT(tb_ap_compare(x, y), ==, left < right ? -1 : (left > right ? 1 : 0));
            TB_TEST_EXPECT_INT(tb_ap_compare_i64(x, right), ==,
                               left < right ? -1 : (left > right ? 1 : 0));
            TB_TEST_EXPECT_INT(tb_ap_sign(x), ==, left < 0 ? -1 : (left > 0 ? 1 : 0));
            if (right != 0)
            {
                expect_equals_i64(tb_ap_div_floor(x, y), floor_div(left, right));
                expect_equals_i64(tb_ap_mod_floor(x, y), floor_mod(left, right));
                expect_equals_i64(tb_ap_div_floor_i64(x, right), floor_div(left, right));
                TB_TEST_EXPECT_INT(tb_ap_mod_floor_i64(x, right), ==, floor_mod(left, right));
            }
            tb_ap_free(x);
            tb_ap_free(y);
        }
    }
}

TB_TEST(core_ap, division_rounds_toward_negative_infinity)
{
    static const struct
    {
        int64_t x;
        int64_t y;
        int64_t quotient;
        int64_t remainder;
    } rows[] = {
        {7, 2, 3, 1},   {-7, 2, -4, 1}, {7, -2, -4, -1}, {-7, -2, 3, -1}, {6, 3, 2, 0},
        {-6, 3, -2, 0}, {6, -3, -2, 0}, {-6, -3, 2, 0},  {0, 5, 0, 0},    {0, -5, 0, 0},
        {1, 5, 0, 1},   {-1, 5, -1, 4}, {1, -5, -1, -4}, {-1, -5, 0, -1},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_ap *const x = tb_ap_from_i64(NULL, rows[row].x);
        tb_ap *const y = tb_ap_from_i64(NULL, rows[row].y);

        TB_TEST_CONTEXT("%lld / %lld", (long long)rows[row].x, (long long)rows[row].y);
        expect_equals_i64(tb_ap_div_floor(x, y), rows[row].quotient);
        expect_equals_i64(tb_ap_mod_floor(x, y), rows[row].remainder);
        expect_equals_i64(tb_ap_div_floor_i64(x, rows[row].y), rows[row].quotient);
        TB_TEST_EXPECT_INT(tb_ap_mod_floor_i64(x, rows[row].y), ==, rows[row].remainder);
        tb_ap_free(x);
        tb_ap_free(y);
    }
}

TB_TEST(core_ap, native_operands_reach_the_limits_of_their_type)
{
    tb_ap *const big = hex("123456789abcdef0123456789abcdef");
    tb_ap *const zero = tb_ap_from_i64(NULL, 0);
    tb_ap *const minimum = tb_ap_from_i64(NULL, INT64_MIN);
    tb_ap *const negated = tb_ap_neg(minimum);

    /* -INT64_MIN does not fit in 64 bits, but does in a value. */
    expect_hex_and_free(tb_ap_mul_i64(minimum, -1), "8000000000000000");
    expect_hex_and_free(tb_ap_sub_i64(zero, INT64_MIN), "8000000000000000");
    expect_hex_and_free(tb_ap_div_floor_i64(minimum, -1), "8000000000000000");
    TB_TEST_EXPECT_INT(tb_ap_compare_i64(negated, INT64_MAX), >, 0);
    /* Computed by Python: big // -2**63 and big % -2**63. */
    expect_hex_and_free(tb_ap_div_floor_i64(big, INT64_MIN), "-2468acf13579bdf");
    TB_TEST_EXPECT_INT(tb_ap_mod_floor_i64(big, INT64_MIN), ==, -9141386507638288913LL);
    TB_TEST_EXPECT_INT(tb_ap_mod_floor_i64(minimum, INT64_MIN), ==, 0);
    tb_ap_free(negated);
    tb_ap_free(minimum);
    tb_ap_free(zero);
    tb_ap_free(big);
}

/* --- digit boundaries ----------------------------------------------------------------- */

TB_TEST(core_ap, values_cross_digit_boundaries_in_both_directions)
{
    /* Each value, one more, and one less, with both signs: the carries and
     * borrows that change the number of digits. */
    static const struct
    {
        const char *value;
        const char *plus_one;
        const char *minus_one;
    } rows[] = {
        {"ffffffff", "100000000", "fffffffe"},
        {"100000000", "100000001", "ffffffff"},
        {"ffffffffffffffff", "10000000000000000", "fffffffffffffffe"},
        {"10000000000000000", "10000000000000001", "ffffffffffffffff"},
        {"-ffffffff", "-fffffffe", "-100000000"},
        {"-100000000", "-ffffffff", "-100000001"},
        {"-ffffffffffffffff", "-fffffffffffffffe", "-10000000000000000"},
        {"-10000000000000000", "-ffffffffffffffff", "-10000000000000001"},
        {"0", "1", "-1"},
        {"1", "2", "0"},
        {"-1", "0", "-2"},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_ap *const value = hex(rows[row].value);
        tb_ap *const one = tb_ap_from_i64(NULL, 1);

        TB_TEST_CONTEXT("%s", rows[row].value);
        expect_hex_and_free(tb_ap_add_i64(value, 1), rows[row].plus_one);
        expect_hex_and_free(tb_ap_sub_i64(value, 1), rows[row].minus_one);
        expect_hex_and_free(tb_ap_add(value, one), rows[row].plus_one);
        expect_hex_and_free(tb_ap_sub(value, one), rows[row].minus_one);
        expect_hex_and_free(tb_ap_add_i64(value, -1), rows[row].minus_one);
        expect_hex_and_free(tb_ap_sub_i64(value, -1), rows[row].plus_one);
        tb_ap_free(one);
        tb_ap_free(value);
    }
}

TB_TEST(core_ap, a_value_minus_itself_is_a_positive_zero)
{
    static const char *const values[] = {"0", "1", "-1", "ffffffffffffffffffff",
                                         "-123456789abcdef0123456789"};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(values); row++)
    {
        tb_ap *const value = hex(values[row]);
        tb_ap *const negated = tb_ap_neg(value);
        tb_ap *const difference = tb_ap_sub(value, value);
        tb_ap *const sum = tb_ap_add(value, negated);
        tb_ap *const product = tb_ap_mul_i64(negated, 0);

        TB_TEST_CONTEXT("%s", values[row]);
        TB_TEST_EXPECT_INT(tb_ap_sign(difference), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_sign(sum), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_sign(product), ==, 0);
        expect_hex_and_free(difference, "0");
        expect_hex_and_free(sum, "0");
        expect_hex_and_free(product, "0");
        tb_ap_free(negated);
        tb_ap_free(value);
    }
}

/* --- powers --------------------------------------------------------------------------- */

TB_TEST(core_ap, powers_with_exponents_zero_one_and_large)
{
    tb_ap *const zero = tb_ap_from_i64(NULL, 0);
    tb_ap *const one = tb_ap_from_i64(NULL, 1);
    tb_ap *const two = tb_ap_from_i64(NULL, 2);
    tb_ap *const minus_three = tb_ap_from_i64(NULL, -3);
    tb_ap *const exponent = tb_ap_from_i64(NULL, 4096);
    tb_ap *const odd_exponent = tb_ap_from_i64(NULL, 3);
    tb_ap *const shifted = tb_ap_shift_left(one, 4096);

    expect_equals_i64(tb_ap_pow(zero, zero, NULL), 1);
    expect_equals_i64(tb_ap_pow(minus_three, zero, NULL), 1);
    expect_equals_i64(tb_ap_pow(minus_three, one, NULL), -3);
    expect_equals_i64(tb_ap_pow(minus_three, odd_exponent, NULL), -27);
    expect_equals_i64(tb_ap_pow(zero, exponent, NULL), 0);
    expect_equals_i64(tb_ap_pow(one, exponent, NULL), 1);
    {
        tb_ap *const power = tb_ap_pow(two, exponent, NULL);

        TB_TEST_EXPECT_INT(tb_ap_compare(power, shifted), ==, 0);
        tb_ap_free(power);
    }
    /* Modulo 1 everything is 0, 0^0 included. */
    expect_equals_i64(tb_ap_pow(zero, zero, one), 0);
    expect_equals_i64(tb_ap_pow(two, exponent, one), 0);
    tb_ap_free(shifted);
    tb_ap_free(odd_exponent);
    tb_ap_free(exponent);
    tb_ap_free(minus_three);
    tb_ap_free(two);
    tb_ap_free(one);
    tb_ap_free(zero);
}

TB_TEST(core_ap, modular_powers_agree_with_repeated_multiplication)
{
    for (size_t round = 0; round < 40; round++)
    {
        tb_ap *const base = random_value();
        tb_ap *const modulus_source = random_nonzero_value();
        tb_ap *const modulus = tb_ap_sign(modulus_source) < 0 ? tb_ap_neg(modulus_source)
                                                              : tb_ap_copy(modulus_source, NULL);
        const int64_t exponent_value = (int64_t)tb_test_random_below(40);
        tb_ap *const exponent = tb_ap_from_i64(NULL, exponent_value);
        tb_ap *expected = tb_ap_from_i64(NULL, 1);
        tb_ap *reduced = NULL;
        tb_ap *power = NULL;

        TB_TEST_CONTEXT("round %zu, exponent %lld", round, (long long)exponent_value);
        for (int64_t step = 0; step < exponent_value; step++)
        {
            tb_ap *const product = tb_ap_mul(expected, base);

            tb_ap_free(expected);
            expected = product;
        }
        reduced = tb_ap_mod_floor(expected, modulus);
        power = tb_ap_pow(base, exponent, modulus);
        TB_TEST_EXPECT_INT(tb_ap_compare(power, reduced), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_sign(power), >=, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(power, modulus), <, 0);
        tb_ap_free(power);
        tb_ap_free(reduced);
        power = tb_ap_pow(base, exponent, NULL);
        TB_TEST_EXPECT_INT(tb_ap_compare(power, expected), ==, 0);
        tb_ap_free(power);
        tb_ap_free(expected);
        tb_ap_free(exponent);
        tb_ap_free(modulus);
        tb_ap_free(modulus_source);
        tb_ap_free(base);
    }
}

/* --- shifts ----------------------------------------------------------------------------- */

TB_TEST(core_ap, a_right_shift_rounds_toward_negative_infinity)
{
    static const struct
    {
        int64_t value;
        size_t bits;
        int64_t expected;
    } rows[] = {
        {5, 1, 2},
        {-5, 1, -3},
        {-4, 1, -2},
        {-1, 1, -1},
        {-1, 1000, -1},
        {1, 1000, 0},
        {0, 3, 0},
        {-6, 0, -6},
        {-6, 1, -3},
        {-7, 2, -2},
        {INT64_MIN, 63, -1},
        {INT64_MIN, 64, -1},
        {INT64_MAX, 62, 1},
        {-4294967296LL, 32, -1},
        {-4294967297, 32, -2},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_ap *const value = tb_ap_from_i64(NULL, rows[row].value);

        TB_TEST_CONTEXT("%lld >> %zu", (long long)rows[row].value, rows[row].bits);
        expect_equals_i64(tb_ap_shift_right(value, rows[row].bits), rows[row].expected);
        tb_ap_free(value);
    }
}

TB_TEST(core_ap, a_left_shift_multiplies_by_a_power_of_two)
{
    tb_ap *const two = tb_ap_from_i64(NULL, 2);
    tb_ap *const zero = tb_ap_from_i64(NULL, 0);

    expect_equals_i64(tb_ap_shift_left(zero, 100000), 0);
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        tb_ap *const value = random_value();
        const size_t bits = (size_t)tb_test_random_below(300);
        tb_ap *const count = tb_ap_from_u64(NULL, bits);
        tb_ap *const power = tb_ap_pow(two, count, NULL);
        tb_ap *const shifted = tb_ap_shift_left(value, bits);
        tb_ap *const product = tb_ap_mul(value, power);
        tb_ap *const back = tb_ap_shift_right(shifted, bits);
        tb_ap *const quotient = tb_ap_div_floor(value, power);
        tb_ap *const right = tb_ap_shift_right(value, bits);

        TB_TEST_CONTEXT("round %zu, %zu bits", round, bits);
        expect_canonical(shifted);
        TB_TEST_EXPECT_INT(tb_ap_compare(shifted, product), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(back, value), ==, 0);
        /* A right shift is floor division by the same power. */
        TB_TEST_EXPECT_INT(tb_ap_compare(right, quotient), ==, 0);
        tb_ap_free(right);
        tb_ap_free(quotient);
        tb_ap_free(back);
        tb_ap_free(product);
        tb_ap_free(shifted);
        tb_ap_free(power);
        tb_ap_free(count);
        tb_ap_free(value);
    }
    tb_ap_free(zero);
    tb_ap_free(two);
}

/* --- known answers ---------------------------------------------------------------------- */

TB_TEST(core_ap, factorials_powers_and_fibonacci_numbers_match_the_known_answers)
{
    char text[TEXT_SIZE] = {0};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(xp_factorials); row++)
    {
        tb_ap *product = tb_ap_from_i64(NULL, 1);

        TB_TEST_CONTEXT("%zu!", xp_factorials[row].argument);
        for (size_t factor = 2; factor <= xp_factorials[row].argument; factor++)
        {
            tb_ap *const next = tb_ap_mul_i64(product, (int64_t)factor);

            tb_ap_free(product);
            product = next;
        }
        text_of(product, 10, text, sizeof(text));
        TB_TEST_EXPECT_STRING(text, ==, xp_factorials[row].decimal);
        tb_ap_free(product);
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(xp_powers_of_two); row++)
    {
        tb_ap *const two = tb_ap_from_i64(NULL, 2);
        tb_ap *const exponent = tb_ap_from_u64(NULL, xp_powers_of_two[row].argument);
        tb_ap *const power = tb_ap_pow(two, exponent, NULL);

        TB_TEST_CONTEXT("2^%zu", xp_powers_of_two[row].argument);
        text_of(power, 10, text, sizeof(text));
        TB_TEST_EXPECT_STRING(text, ==, xp_powers_of_two[row].decimal);
        tb_ap_free(power);
        tb_ap_free(exponent);
        tb_ap_free(two);
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(xp_fibonacci); row++)
    {
        tb_ap *previous = tb_ap_from_i64(NULL, 1);
        tb_ap *current = tb_ap_from_i64(NULL, 0);

        TB_TEST_CONTEXT("F(%zu)", xp_fibonacci[row].argument);
        for (size_t step = 0; step < xp_fibonacci[row].argument; step++)
        {
            tb_ap *const next = tb_ap_add(current, previous);

            tb_ap_free(previous);
            previous = current;
            current = next;
        }
        text_of(current, 10, text, sizeof(text));
        TB_TEST_EXPECT_STRING(text, ==, xp_fibonacci[row].decimal);
        /* The same number, negated, has a sign and the same digits. */
        {
            tb_ap *const negated = tb_ap_neg(current);

            text_of(negated, 16, text, sizeof(text));
            if (tb_ap_sign(current) != 0)
            {
                TB_TEST_EXPECT_INT(text[0], ==, '-');
                TB_TEST_EXPECT_STRING(text + 1, ==, xp_fibonacci[row].hexadecimal);
            }
            tb_ap_free(negated);
        }
        tb_ap_free(current);
        tb_ap_free(previous);
    }
}

/* --- vectors from an independent implementation --------------------------------------- */

TB_TEST(core_ap, every_operation_matches_the_independent_vectors)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(ap_pairs); row++)
    {
        const ap_pair_vector *const vector = &ap_pairs[row];
        tb_ap *const a = hex(vector->a);
        tb_ap *const b = hex(vector->b);

        TB_TEST_CONTEXT("pair %zu", row);
        expect_hex_and_free(tb_ap_add(a, b), vector->sum);
        expect_hex_and_free(tb_ap_sub(a, b), vector->difference);
        expect_hex_and_free(tb_ap_mul(a, b), vector->product);
        expect_hex_and_free(tb_ap_div_floor(a, b), vector->quotient);
        expect_hex_and_free(tb_ap_mod_floor(a, b), vector->remainder);
        tb_ap_free(b);
        tb_ap_free(a);
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(ap_powers); row++)
    {
        const ap_power_vector *const vector = &ap_powers[row];
        tb_ap *const base = hex(vector->base);
        tb_ap *const exponent = hex(vector->exponent);
        tb_ap *const modulus = vector->modulus != NULL ? hex(vector->modulus) : NULL;

        TB_TEST_CONTEXT("power %zu", row);
        expect_hex_and_free(tb_ap_pow(base, exponent, modulus), vector->result);
        tb_ap_free(modulus);
        tb_ap_free(exponent);
        tb_ap_free(base);
    }
}

/* Applies one step of a chain to a value, which it frees. */
static tb_ap *apply_step(tb_ap *value, const ap_chain_step *step)
{
    tb_ap *const operand = hex(step->operand);
    tb_ap *result = NULL;
    uint64_t count = 0;

    switch (step->operation)
    {
    case '+':
        result = tb_ap_add(value, operand);
        break;
    case '-':
        result = tb_ap_sub(value, operand);
        break;
    case '*':
        result = tb_ap_mul(value, operand);
        break;
    case '/':
        result = tb_ap_div_floor(value, operand);
        break;
    case '%':
        result = tb_ap_mod_floor(value, operand);
        break;
    case '<':
        TB_TEST_REQUIRE(tb_ap_to_u64(operand, &count));
        result = tb_ap_shift_left(value, (size_t)count);
        break;
    case '>':
        TB_TEST_REQUIRE(tb_ap_to_u64(operand, &count));
        result = tb_ap_shift_right(value, (size_t)count);
        break;
    default:
        TB_TEST_REQUIRE_INT(step->operation, ==, '^');
        result = tb_ap_pow(value, operand, NULL);
        break;
    }
    tb_ap_free(operand);
    tb_ap_free(value);
    return result;
}

TB_TEST(core_ap, long_chains_of_operations_match_the_independent_vectors)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(ap_chains); row++)
    {
        const ap_chain_vector *const chain = &ap_chains[row];
        tb_ap *value = hex(chain->start);

        TB_TEST_CONTEXT("chain %zu", row);
        for (size_t step = 0; step < chain->step_count; step++)
        {
            value = apply_step(value, &chain->steps[step]);
        }
        expect_canonical(value);
        expect_hex_and_free(value, chain->result);
    }
}

/* --- identities on random operands ------------------------------------------------------ */

TB_TEST(core_ap, addition_and_multiplication_commute_associate_and_distribute)
{
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        tb_ap *const a = random_value();
        tb_ap *const b = random_value();
        tb_ap *const c = random_value();
        tb_ap *const ab = tb_ap_add(a, b);
        tb_ap *const ba = tb_ap_add(b, a);
        tb_ap *const a_times_b = tb_ap_mul(a, b);
        tb_ap *const b_times_a = tb_ap_mul(b, a);
        tb_ap *const ab_c = tb_ap_add(ab, c);
        tb_ap *const bc = tb_ap_add(b, c);
        tb_ap *const a_bc = tb_ap_add(a, bc);
        tb_ap *const ab_times_c = tb_ap_mul(a_times_b, c);
        tb_ap *const b_times_c = tb_ap_mul(b, c);
        tb_ap *const a_times_bc = tb_ap_mul(a, b_times_c);
        tb_ap *const a_times_sum = tb_ap_mul(a, bc);
        tb_ap *const a_times_c = tb_ap_mul(a, c);
        tb_ap *const sum_of_products = tb_ap_add(a_times_b, a_times_c);
        tb_ap *const back = tb_ap_sub(ab, b);

        TB_TEST_CONTEXT("round %zu", round);
        expect_canonical(ab);
        expect_canonical(a_times_b);
        expect_canonical(back);
        TB_TEST_EXPECT_INT(tb_ap_compare(ab, ba), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(a_times_b, b_times_a), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(ab_c, a_bc), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(ab_times_c, a_times_bc), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(a_times_sum, sum_of_products), ==, 0);
        TB_TEST_EXPECT_INT(tb_ap_compare(back, a), ==, 0);

        tb_ap_free(back);
        tb_ap_free(sum_of_products);
        tb_ap_free(a_times_c);
        tb_ap_free(a_times_sum);
        tb_ap_free(a_times_bc);
        tb_ap_free(b_times_c);
        tb_ap_free(ab_times_c);
        tb_ap_free(a_bc);
        tb_ap_free(bc);
        tb_ap_free(ab_c);
        tb_ap_free(b_times_a);
        tb_ap_free(a_times_b);
        tb_ap_free(ba);
        tb_ap_free(ab);
        tb_ap_free(c);
        tb_ap_free(b);
        tb_ap_free(a);
    }
}

TB_TEST(core_ap, quotient_times_divisor_plus_remainder_is_the_dividend)
{
    for (size_t round = 0; round < RANDOM_ROUNDS; round++)
    {
        tb_ap *const a = random_value();
        tb_ap *const b = random_nonzero_value();
        tb_ap *const quotient = tb_ap_div_floor(a, b);
        tb_ap *const remainder = tb_ap_mod_floor(a, b);
        tb_ap *const product = tb_ap_mul(quotient, b);
        tb_ap *const back = tb_ap_add(product, remainder);
        tb_ap *const absolute_b = tb_ap_sign(b) < 0 ? tb_ap_neg(b) : tb_ap_copy(b, NULL);
        tb_ap *const absolute_r =
            tb_ap_sign(remainder) < 0 ? tb_ap_neg(remainder) : tb_ap_copy(remainder, NULL);

        TB_TEST_CONTEXT("round %zu", round);
        expect_canonical(quotient);
        expect_canonical(remainder);
        TB_TEST_EXPECT_INT(tb_ap_compare(back, a), ==, 0);
        /* |r| < |b|, and r is zero or has the sign of b. */
        TB_TEST_EXPECT_INT(tb_ap_compare(absolute_r, absolute_b), <, 0);
        TB_TEST_EXPECT(tb_ap_sign(remainder) == 0 || tb_ap_sign(remainder) == tb_ap_sign(b));
        tb_ap_free(absolute_r);
        tb_ap_free(absolute_b);
        tb_ap_free(back);
        tb_ap_free(product);
        tb_ap_free(remainder);
        tb_ap_free(quotient);
        tb_ap_free(b);
        tb_ap_free(a);
    }
}

static int64_t random_i64_near_limits(int64_t limit)
{
    static const int64_t specials[] = {
        0, 1, -1, 4294967295LL, -4294967295LL, 4294967296LL, -4294967296LL};
    const uint64_t roll = tb_test_random_below(4);

    if (roll == 0)
    {
        const int64_t special = specials[tb_test_random_below(TB_TEST_COUNT_OF(specials))];

        if (special >= -limit && special <= limit)
        {
            return special;
        }
    }
    {
        /* A random number of significant bits, so that small values are common. */
        const int64_t magnitude =
            (int64_t)(tb_test_random_below((uint64_t)limit + 1) >> tb_test_random_below(40));

        return tb_test_random_below(2) == 0 ? -magnitude : magnitude;
    }
}

TB_TEST(core_ap, agrees_with_native_arithmetic_where_everything_fits)
{
    for (size_t round = 0; round < NATIVE_ROUNDS; round++)
    {
        /* Operands below 2^62 cannot overflow a sum, below 2^31 a product. */
        const int64_t big_left = random_i64_near_limits(INT64_MAX / 2);
        const int64_t big_right = random_i64_near_limits(INT64_MAX / 2);
        const int64_t small_left = random_i64_near_limits(INT32_MAX);
        const int64_t small_right = random_i64_near_limits(INT32_MAX);
        tb_ap *const x = tb_ap_from_i64(NULL, big_left);
        tb_ap *const y = tb_ap_from_i64(NULL, big_right);
        tb_ap *const small_x = tb_ap_from_i64(NULL, small_left);
        tb_ap *const small_y = tb_ap_from_i64(NULL, small_right);

        TB_TEST_CONTEXT("round %zu: %lld, %lld, %lld, %lld", round, (long long)big_left,
                        (long long)big_right, (long long)small_left, (long long)small_right);
        expect_equals_i64(tb_ap_add(x, y), big_left + big_right);
        expect_equals_i64(tb_ap_sub(x, y), big_left - big_right);
        expect_equals_i64(tb_ap_mul(small_x, small_y), small_left * small_right);
        if (big_right != 0)
        {
            expect_equals_i64(tb_ap_div_floor(x, y), floor_div(big_left, big_right));
            expect_equals_i64(tb_ap_mod_floor(x, y), floor_mod(big_left, big_right));
            TB_TEST_EXPECT_INT(tb_ap_mod_floor_i64(x, big_right), ==,
                               floor_mod(big_left, big_right));
        }
        tb_ap_free(small_y);
        tb_ap_free(small_x);
        tb_ap_free(y);
        tb_ap_free(x);
    }
}

/* --- text ------------------------------------------------------------------------------- */

TB_TEST(core_ap, text_round_trips_in_every_base)
{
    for (unsigned base = 2; base <= 36; base++)
    {
        for (size_t round = 0; round < 10; round++)
        {
            tb_ap *const value = random_value();
            char text[TEXT_SIZE] = {0};
            size_t required = 0;
            tb_ap *back = NULL;

            TB_TEST_CONTEXT("base %u, round %zu", base, round);
            TB_TEST_REQUIRE_INT(tb_ap_to_string(value, base, text, sizeof(text), &required), ==,
                                TB_AP_OK);
            TB_TEST_EXPECT_UINT(required, ==, tb_str_length(text) + 1);
            TB_TEST_EXPECT_UINT(tb_ap_string_size(value, base), >=, required);
            TB_TEST_REQUIRE_INT(tb_ap_from_string(NULL, text, required - 1, base, &back), ==,
                                TB_AP_OK);
            TB_TEST_EXPECT_INT(tb_ap_compare(back, value), ==, 0);
            tb_ap_free(back);
            tb_ap_free(value);
        }
    }
}

TB_TEST(core_ap, text_takes_a_sign_and_leading_zeros)
{
    static const struct
    {
        const char *text;
        unsigned base;
        int64_t value;
    } rows[] = {
        {"0", 10, 0},
        {"-0", 10, 0},
        {"+0", 10, 0},
        {"000", 10, 0},
        {"-000", 10, 0},
        {"+42", 10, 42},
        {"-42", 10, -42},
        {"0042", 10, 42},
        {"-0042", 10, -42},
        {"-ZZ", 36, -1295},
        {"-zz", 36, -1295},
        {"-101", 2, -5},
        {"-9223372036854775808", 10, INT64_MIN},
        {"9223372036854775807", 10, INT64_MAX},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_ap *value = NULL;

        TB_TEST_CONTEXT("\"%s\" in base %u", rows[row].text, rows[row].base);
        TB_TEST_REQUIRE_INT(tb_ap_from_string(NULL, rows[row].text, tb_str_length(rows[row].text),
                                              rows[row].base, &value),
                            ==, TB_AP_OK);
        expect_equals_i64(value, rows[row].value);
    }
}

TB_TEST(core_ap, rejects_malformed_text_and_leaves_the_output_alone)
{
    static const struct
    {
        const char *text;
        unsigned base;
    } rows[] = {
        {"", 10},    {"+", 10},   {"-", 10},   {"--1", 10},   {"+-1", 10}, {"-+1", 10},
        {" 1", 10},  {"1 ", 10},  {"- 1", 10}, {"0x10", 16},  {"12a", 10}, {"2", 2},
        {"1.5", 10}, {"1e3", 10}, {"-g", 16},  {"1_000", 10},
    };
    /* An address no value has, to show that the output was not written. */
    static uint64_t sentinel_storage = 0;
    tb_ap *const sentinel = (tb_ap *)(void *)&sentinel_storage;

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_ap *value = sentinel;

        TB_TEST_CONTEXT("\"%s\" in base %u", rows[row].text, rows[row].base);
        TB_TEST_EXPECT_INT(tb_ap_from_string(NULL, rows[row].text, tb_str_length(rows[row].text),
                                             rows[row].base, &value),
                           ==, TB_AP_INVALID);
        TB_TEST_EXPECT_POINTER(value, ==, sentinel);
    }
}

TB_TEST(core_ap, a_buffer_that_is_too_small_is_left_untouched)
{
    tb_ap *const negative = tb_ap_from_i64(NULL, -12345);
    char buffer[8] = {0};
    size_t required = 0;

    for (size_t size = 0; size < 7; size++)
    {
        TB_TEST_CONTEXT("%zu bytes", size);
        memset(buffer, 'x', sizeof(buffer));
        TB_TEST_EXPECT_INT(
            tb_ap_to_string(negative, 10, size == 0 ? NULL : buffer, size, &required), ==,
            TB_AP_BUFFER_TOO_SMALL);
        TB_TEST_EXPECT_UINT(required, ==, 7);
        for (size_t index = 0; index < sizeof(buffer); index++)
        {
            TB_TEST_EXPECT_INT(buffer[index], ==, 'x');
        }
    }
    TB_TEST_EXPECT_INT(tb_ap_to_string(negative, 10, buffer, 7, &required), ==, TB_AP_OK);
    TB_TEST_EXPECT_STRING(buffer, ==, "-12345");
    TB_TEST_EXPECT_UINT(required, ==, 7);
    tb_ap_free(negative);
}

/* --- formatting ------------------------------------------------------------------------- */

/* The conversion is registered for each case that uses it, and the earlier
 * one restored, so that no other case sees it. */
#define AP_CONVERSION 'Z'

TB_TEST(core_ap, the_conversion_formats_like_d_for_every_flag)
{
    static const char *const formats[][2] = {
        {"%Z", "%lld"},         {"%+Z", "%+lld"},           {"% Z", "% lld"},
        {"%12Z", "%12lld"},     {"%-12Z|", "%-12lld|"},     {"%012Z", "%012lld"},
        {"%+012Z", "%+012lld"}, {"%.5Z", "%.5lld"},         {"%.0Z", "%.0lld"},
        {"%8.3Z", "%8.3lld"},   {"%-+8.3Z|", "%-+8.3lld|"}, {"%08.3Z", "%08.3lld"},
        {"% 05Z", "% 05lld"},   {"%2Z", "%2lld"},           {"%*Z", "%*lld"},
    };
    static const int64_t values[] = {0, 1, -1, 42, -42, 123456789, INT64_MAX, INT64_MIN};
    tb_fmt_conversion *const previous = tb_fmt_register(AP_CONVERSION, tb_ap_format);

    for (size_t value_row = 0; value_row < TB_TEST_COUNT_OF(values); value_row++)
    {
        tb_ap *const value = tb_ap_from_i64(NULL, values[value_row]);

        for (size_t row = 0; row < TB_TEST_COUNT_OF(formats); row++)
        {
            char actual[64] = {0};
            char expected[64] = {0};
            const bool takes_width = formats[row][0][1] == '*';

            TB_TEST_CONTEXT("%s with %lld", formats[row][0], (long long)values[value_row]);
            if (takes_width)
            {
                (void)tb_fmt_buffer(actual, sizeof(actual), formats[row][0], 25, value);
                (void)tb_fmt_buffer(expected, sizeof(expected), formats[row][1], 25,
                                    (long long)values[value_row]);
            }
            else
            {
                (void)tb_fmt_buffer(actual, sizeof(actual), formats[row][0], value);
                (void)tb_fmt_buffer(expected, sizeof(expected), formats[row][1],
                                    (long long)values[value_row]);
            }
            TB_TEST_EXPECT_STRING(actual, ==, expected);
        }
        tb_ap_free(value);
    }
    (void)tb_fmt_register(AP_CONVERSION, previous);
}

TB_TEST(core_ap, the_conversion_writes_values_longer_than_any_native_one)
{
    tb_fmt_conversion *const previous = tb_fmt_register(AP_CONVERSION, tb_ap_format);
    tb_ap *const two = tb_ap_from_i64(NULL, 2);
    tb_ap *const exponent = tb_ap_from_i64(NULL, 4096);
    tb_ap *const power = tb_ap_pow(two, exponent, NULL);
    tb_ap *const negative = tb_ap_neg(power);
    char *text = NULL;

    /* More than 256 bytes, which the formatter measures before it writes. */
    TB_TEST_REQUIRE_UINT(xp_powers_of_two[14].argument, ==, 4096);
    text = tb_fmt_string(NULL, "[%Z]", negative);
    TB_TEST_EXPECT_INT(text[0], ==, '[');
    TB_TEST_EXPECT_INT(text[1], ==, '-');
    TB_TEST_EXPECT_UINT(tb_str_length(text), ==, tb_str_length(xp_powers_of_two[14].decimal) + 3);
    TB_TEST_EXPECT(tb_str_starts_with(text + 2, xp_powers_of_two[14].decimal));
    tb_str_free(NULL, text);
    tb_ap_free(negative);
    tb_ap_free(power);
    tb_ap_free(exponent);
    tb_ap_free(two);
    (void)tb_fmt_register(AP_CONVERSION, previous);
}

/* --- allocators --------------------------------------------------------------------------- */

/* Operands made from a test allocator, for one operation under test. */
typedef struct operation_context
{
    const tb_allocator *allocator;
    tb_ap *x;
    tb_ap *y;
    tb_ap *p;
    char *text;
    size_t text_size;
} operation_context;

typedef void operation(operation_context *context);

static void run_add(operation_context *context)
{
    tb_ap_free(tb_ap_add(context->x, context->y));
}

static void run_mul(operation_context *context)
{
    tb_ap_free(tb_ap_mul(context->x, context->y));
}

static void run_div_floor(operation_context *context)
{
    tb_ap_free(tb_ap_div_floor(context->x, context->y));
}

static void run_mod_floor_i64(operation_context *context)
{
    (void)tb_ap_mod_floor_i64(context->x, -1000003);
}

static void run_pow(operation_context *context)
{
    tb_ap_free(tb_ap_pow(context->x, context->y, context->p));
}

static void run_shift_left(operation_context *context)
{
    tb_ap_free(tb_ap_shift_left(context->x, 77));
}

static void run_from_string(operation_context *context)
{
    tb_ap *value = NULL;

    (void)tb_ap_from_string(context->allocator, "-123456789012345678901234567890", 31, 10, &value);
    tb_ap_free(value);
}

static void run_to_string(operation_context *context)
{
    size_t required = 0;

    (void)tb_ap_to_string(context->x, 7, context->text, context->text_size, &required);
}

/* The case registers the conversion around every run of this operation. */
static void run_format(operation_context *context)
{
    (void)tb_fmt_buffer(context->text, context->text_size, "%+40Z", context->x);
}

static const struct
{
    const char *name;
    operation *run;
} operations[] = {
    {"add", run_add},
    {"mul", run_mul},
    {"div_floor", run_div_floor},
    {"mod_floor_i64", run_mod_floor_i64},
    {"pow", run_pow},
    {"shift_left", run_shift_left},
    {"from_string", run_from_string},
    {"to_string", run_to_string},
    {"format", run_format},
};

static void make_operands(operation_context *context, const tb_allocator *allocator, char *text,
                          size_t text_size)
{
    tb_ap *value = NULL;

    context->allocator = allocator;
    TB_TEST_REQUIRE_INT(
        tb_ap_from_string(allocator, "-fedcba9876543210fedcba9876543210", 33, 16, &value), ==,
        TB_AP_OK);
    context->x = value;
    context->y = tb_ap_from_i64(allocator, 123456789);
    context->p = tb_ap_from_i64(allocator, 1000000007LL);
    context->text = text;
    context->text_size = text_size;
}

static void free_operands(operation_context *context)
{
    tb_ap_free(context->p);
    tb_ap_free(context->y);
    tb_ap_free(context->x);
}

TB_TEST(core_ap, every_block_goes_back_to_its_allocator)
{
    tb_fmt_conversion *const previous = tb_fmt_register(AP_CONVERSION, tb_ap_format);

    for (size_t row = 0; row < TB_TEST_COUNT_OF(operations); row++)
    {
        tb_test_allocator tracker = {0};
        const tb_allocator allocator = tb_test_allocator_start(&tracker);
        operation_context context = {0};
        char text[128] = {0};

        TB_TEST_CONTEXT("%s", operations[row].name);
        make_operands(&context, &allocator, text, sizeof(text));
        operations[row].run(&context);
        free_operands(&context);
        TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    }
    (void)tb_fmt_register(AP_CONVERSION, previous);
}

static void run_trapped_operation(void *argument)
{
    const struct
    {
        operation *run;
        operation_context *context;
    } *const call = argument;

    call->run(call->context);
}

TB_TEST(core_ap, exhaustion_at_every_step_reaches_the_memory_failure_handler)
{
    /* Each operation is run once to count its allocations, then again with
     * each of them refused in turn. Running out of memory is fatal, so every
     * refusal must end the operation in the memory failure handler, which the
     * test turns into a trap, rather than in a crash or a wrong result. */
    tb_fmt_conversion *const previous = tb_fmt_register(AP_CONVERSION, tb_ap_format);

    for (size_t row = 0; row < TB_TEST_COUNT_OF(operations); row++)
    {
        tb_test_allocator tracker = {0};
        const tb_allocator allocator = tb_test_allocator_start(&tracker);
        operation_context context = {0};
        char text[128] = {0};
        size_t requests = 0;

        make_operands(&context, &allocator, text, sizeof(text));
        requests = tracker.request_count;
        operations[row].run(&context);
        requests = tracker.request_count - requests;
        free_operands(&context);
        TB_TEST_EXPECT_UINT(requests, >, 0);

        for (size_t successes = 0; successes < requests; successes++)
        {
            struct
            {
                operation *run;
                operation_context *context;
            } call = {operations[row].run, &context};

            TB_TEST_CONTEXT("%s, refusing after %zu of %zu", operations[row].name, successes,
                            requests);
            make_operands(&context, &allocator, text, sizeof(text));
            tb_test_allocator_fail_after(&tracker, successes);
            TB_TEST_EXPECT_TRAP(run_trapped_operation, &call, "out of memory");
            tb_test_allocator_stop_failing(&tracker);
            tb_test_allocator_release_all(&tracker);
        }
        TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    }
    (void)tb_fmt_register(AP_CONVERSION, previous);
}

/* --- contract violations ---------------------------------------------------------------- */

static void add_to_null(void *context)
{
    tb_ap *const one = context;

    tb_ap_free(tb_ap_add(NULL, one));
}

static void divide_by_zero(void *context)
{
    tb_ap *const one = context;
    tb_ap *const zero = tb_ap_sub(one, one);

    tb_ap_free(tb_ap_div_floor(one, zero));
}

static void take_a_remainder_by_native_zero(void *context)
{
    (void)tb_ap_mod_floor_i64(context, 0);
}

static void raise_to_a_negative_power(void *context)
{
    tb_ap *const minus_one = tb_ap_neg(context);

    tb_ap_free(tb_ap_pow(context, minus_one, NULL));
}

static void raise_modulo_zero(void *context)
{
    tb_ap *const zero = tb_ap_sub(context, context);

    tb_ap_free(tb_ap_pow(context, context, zero));
}

static void read_base_one(void *context)
{
    tb_ap *value = NULL;

    (void)context;
    (void)tb_ap_from_string(NULL, "0", 1, 1, &value);
}

static void write_base_37(void *context)
{
    char text[8] = {0};
    size_t required = 0;

    (void)tb_ap_to_string(context, 37, text, sizeof(text), &required);
}

static void format_null(void *context)
{
    char text[8] = {0};

    (void)context;
    (void)tb_fmt_buffer(text, sizeof(text), "%Z", (const tb_ap *)NULL);
}

TB_TEST(core_ap, contract_violations_are_caught)
{
    static const struct
    {
        tb_test_trap_body *body;
        const char *message;
    } rows[] = {
        {add_to_null, "x != NULL"},
        {divide_by_zero, "division by zero"},
        {take_a_remainder_by_native_zero, "division by zero"},
        {raise_to_a_negative_power, "the exponent must not be negative"},
        {raise_modulo_zero, "the modulus must be positive"},
        {read_base_one, "ap_base_is_valid(base)"},
        {write_base_37, "ap_base_is_valid(base)"},
        {format_null, "the argument of an arbitrary-precision conversion is NULL"},
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_fmt_conversion *const previous = tb_fmt_register(AP_CONVERSION, tb_ap_format);
    tb_ap *const one = tb_ap_from_i64(&allocator, 1);

    if (!tb_assert_library_enabled())
    {
        tb_ap_free(one);
        (void)tb_fmt_register(AP_CONVERSION, previous);
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("row %zu", row);
        TB_TEST_EXPECT_TRAP(rows[row].body, one, rows[row].message);
    }
    /* A trap leaves what the body made; the allocator releases it. */
    tb_test_allocator_release_all(&tracker);
    (void)tb_fmt_register(AP_CONVERSION, previous);
}
