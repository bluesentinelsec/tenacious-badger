/* Floating-point text is right only if it is exactly right: one digit off in
 * a saved file or a JSON message is a different number on the next load, and
 * a rounding that differs between platforms breaks replays. So the
 * conversions are checked against vectors from an independent, correctly
 * rounded implementation (Python's, see tests/tools/make_decimal_vectors.py)
 * at the places where conversion is hard: halfway cases in both directions,
 * subnormals, the extremes, powers of two and ten, and texts of hundreds of
 * digits. The shortest text is also checked to read back as the identical
 * bits for a large number of random values. */
#include "badger/decimal.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "decimal_vectors.h"
#include "tb_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Random values whose shortest text must read back as the same bits. Every
 * value is drawn from all 2^64 bit patterns, so every exponent is as likely
 * as every other. A million take seconds in an optimized build, and many
 * times as long in a debug or instrumented one, which checks a tenth of them;
 * --repeat explores further. */
#if defined(NDEBUG) && !defined(TB_SANITIZE_ADDRESS) && !defined(TB_SANITIZE_THREAD)
#define ROUND_TRIP_COUNT ((size_t)1000000)
#else
#define ROUND_TRIP_COUNT ((size_t)100000)
#endif

static double value_of(uint64_t bits)
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

static bool is_nan_bits(uint64_t bits)
{
    return (bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000) &&
           (bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0;
}

static double parse(const char *text)
{
    double value = 0.0;

    TB_TEST_REQUIRE_INT(tb_decimal_parse(text, tb_str_length(text), &value), ==, TB_DECIMAL_OK);
    return value;
}

TB_TEST(core_decimal, names_every_result)
{
    TB_TEST_EXPECT_STRING(tb_decimal_result_name(TB_DECIMAL_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_decimal_result_name(TB_DECIMAL_INVALID), ==, "invalid argument");
    TB_TEST_EXPECT_STRING(tb_decimal_result_name(TB_DECIMAL_BUFFER_TOO_SMALL), ==,
                          "buffer too small");
    TB_TEST_EXPECT_STRING(tb_decimal_result_name((tb_decimal_result)99), ==, "unknown result");
}

/* --- the shortest text -------------------------------------------------------------- */

TB_TEST(core_decimal, the_shortest_text_matches_python_repr)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(decimal_shortest_vectors); row++)
    {
        const decimal_text_vector *const vector = &decimal_shortest_vectors[row];
        char text[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;

        TB_TEST_CONTEXT("0x%016llx: %s", (unsigned long long)vector->bits, vector->text);
        TB_TEST_REQUIRE_INT(
            tb_decimal_shortest(value_of(vector->bits), text, sizeof(text), &required), ==,
            TB_DECIMAL_OK);
        TB_TEST_EXPECT_STRING(text, ==, vector->text);
        TB_TEST_EXPECT_UINT(required, ==, tb_str_length(vector->text) + 1);
    }
}

TB_TEST(core_decimal, the_shortest_text_reads_back_as_the_same_bits)
{
    uint64_t largest_length = 0;

    for (size_t round = 0; round < ROUND_TRIP_COUNT; round++)
    {
        const uint64_t bits = tb_test_random_u64();
        char text[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;
        double back = 0.0;

        if (is_nan_bits(bits))
        {
            continue;
        }
        (void)tb_decimal_shortest(value_of(bits), text, sizeof(text), &required);
        if (required > largest_length)
        {
            largest_length = required;
        }
        if (tb_decimal_parse(text, required - 1, &back) != TB_DECIMAL_OK || bits_of(back) != bits)
        {
            TB_TEST_FAIL("0x%016llx wrote \"%s\", which reads as 0x%016llx",
                         (unsigned long long)bits, text, (unsigned long long)bits_of(back));
        }
    }
    TB_TEST_EXPECT_UINT(largest_length, <=, TB_DECIMAL_SHORTEST_SIZE);
}

TB_TEST(core_decimal, the_longest_shortest_texts_fit_the_documented_size)
{
    /* Seventeen digits, a sign, a point, and an exponent of three digits. */
    static const uint64_t longest[] = {UINT64_C(0x8000000000000001), UINT64_C(0x800FFFFFFFFFFFFF),
                                       UINT64_C(0x8010000000000001), UINT64_C(0xFFEFFFFFFFFFFFFF)};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(longest); row++)
    {
        char text[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;

        TB_TEST_CONTEXT("0x%016llx", (unsigned long long)longest[row]);
        TB_TEST_EXPECT_INT(
            tb_decimal_shortest(value_of(longest[row]), text, sizeof(text), &required), ==,
            TB_DECIMAL_OK);
        TB_TEST_EXPECT_UINT(required, <=, 25);
    }
}

TB_TEST(core_decimal, a_buffer_that_is_too_small_is_left_untouched)
{
    char buffer[8] = {0};
    size_t required = 0;

    memset(buffer, 'x', sizeof(buffer));
    TB_TEST_EXPECT_INT(tb_decimal_shortest(0.125, buffer, 5, &required), ==,
                       TB_DECIMAL_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_UINT(required, ==, 6);
    for (size_t index = 0; index < sizeof(buffer); index++)
    {
        TB_TEST_EXPECT_INT(buffer[index], ==, 'x');
    }
    TB_TEST_EXPECT_INT(tb_decimal_shortest(0.125, NULL, 0, &required), ==,
                       TB_DECIMAL_BUFFER_TOO_SMALL);
    TB_TEST_EXPECT_INT(tb_decimal_shortest(0.125, buffer, 6, &required), ==, TB_DECIMAL_OK);
    TB_TEST_EXPECT_STRING(buffer, ==, "0.125");
}

/* --- formatting ------------------------------------------------------------------------- */

TB_TEST(core_decimal, the_formatter_matches_c_rounding_exactly)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(decimal_format_vectors); row++)
    {
        const decimal_format_vector *const vector = &decimal_format_vectors[row];
        char *const text = tb_fmt_string(NULL, vector->format, value_of(vector->bits));

        TB_TEST_CONTEXT("\"%s\" of 0x%016llx", vector->format, (unsigned long long)vector->bits);
        TB_TEST_EXPECT_STRING(text, ==, vector->text);
        TB_TEST_EXPECT_UINT(tb_fmt_buffer(NULL, 0, vector->format, value_of(vector->bits)), ==,
                            tb_str_length(vector->text));
        tb_str_free(NULL, text);
    }
}

/* glibc and the C library of Apple's systems round every digit correctly, as
 * the engine does, so where they are the C library they are a second,
 * independent reference. Other C libraries round differently, or spell
 * infinity differently, and are not compared. Every format is a literal, so
 * that the compiler checks its arguments. */
#if defined(__GLIBC__) || defined(__APPLE__)
#define DECIMAL_HAS_CORRECTLY_ROUNDED_LIBRARY 1
#else
#define DECIMAL_HAS_CORRECTLY_ROUNDED_LIBRARY 0
#endif

#if DECIMAL_HAS_CORRECTLY_ROUNDED_LIBRARY
static char library_text[2048] = {0};
static char engine_text[2048] = {0};

#define EXPECT_SAME_AS_C_LIBRARY(format, ...)                                                      \
    do                                                                                             \
    {                                                                                              \
        const int library_length =                                                                 \
            snprintf(library_text, sizeof(library_text), format, __VA_ARGS__);                     \
        const size_t engine_length =                                                               \
            tb_fmt_buffer(engine_text, sizeof(engine_text), format, __VA_ARGS__);                  \
        TB_TEST_EXPECT_STRING(engine_text, ==, library_text);                                      \
        TB_TEST_EXPECT_INT((int)engine_length, ==, library_length);                                \
    } while (0)
#endif

TB_TEST(core_decimal, the_formatter_agrees_with_a_correctly_rounded_c_library)
{
#if DECIMAL_HAS_CORRECTLY_ROUNDED_LIBRARY
    for (size_t round = 0; round < 20000; round++)
    {
        const uint64_t bits = tb_test_random_u64();
        const int precision = (int)tb_test_random_below(41);
        const double value = value_of(bits);

        if ((bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000))
        {
            continue;
        }
        TB_TEST_CONTEXT("0x%016llx with a precision of %d", (unsigned long long)bits, precision);
        EXPECT_SAME_AS_C_LIBRARY("%.*e", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%.*E", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%.*f", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%.*g", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%#.*g", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%+.*G", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%-30.*e|", precision, value);
        EXPECT_SAME_AS_C_LIBRARY("%030.*f", precision, value);
        /* Apple's C library writes a subnormal %a in normalized form. */
        if ((bits & UINT64_C(0x7FF0000000000000)) != 0)
        {
            EXPECT_SAME_AS_C_LIBRARY("%a", value);
            EXPECT_SAME_AS_C_LIBRARY("%.*a", precision % 14, value);
        }
    }
#else
    TB_TEST_SKIP("this C library is not known to round floating-point output correctly");
#endif
}

TB_TEST(core_decimal, hexadecimal_output_is_exact)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(decimal_hex_vectors); row++)
    {
        const decimal_text_vector *const vector = &decimal_hex_vectors[row];
        char text[64] = {0};
        char upper[64] = {0};

        TB_TEST_CONTEXT("0x%016llx: %s", (unsigned long long)vector->bits, vector->text);
        (void)tb_fmt_buffer(text, sizeof(text), "%a", value_of(vector->bits));
        TB_TEST_EXPECT_STRING(text, ==, vector->text);
        (void)tb_fmt_buffer(upper, sizeof(upper), "%A", value_of(vector->bits));
        for (size_t index = 0; text[index] != '\0'; index++)
        {
            const char expected = text[index] >= 'a' && text[index] <= 'z'
                                      ? (char)(text[index] - 'a' + 'A')
                                      : text[index];

            TB_TEST_EXPECT_INT(upper[index], ==, expected);
        }
        /* The text reads back as the same value. */
        if (!is_nan_bits(vector->bits))
        {
            TB_TEST_EXPECT_HEX(bits_of(parse(text)), ==, vector->bits);
        }
    }
}

TB_TEST(core_decimal, hexadecimal_output_rounds_to_a_precision_ties_to_even)
{
    static const struct
    {
        const char *format;
        double value;
        const char *text;
    } rows[] = {
        {"%.0a", 1.0, "0x1p+0"},
        {"%.0a", 1.5, "0x2p+0"},       /* a tie, and 1 is odd */
        {"%.0a", 2.5, "0x1p+1"},       /* 0x1.4p+1: below half */
        {"%.0a", 3.0, "0x2p+1"},       /* 0x1.8p+1: a tie, up to even */
        {"%.1a", 1.03125, "0x1.0p+0"}, /* 0x1.08p+0: a tie, 0 is even */
        {"%.1a", 1.09375, "0x1.2p+0"}, /* 0x1.18p+0: a tie, 1 is odd */
        {"%.3a", 1.9999999999999998, "0x2.000p+0"},
        {"%.3a", 5e-324, "0x0.000p-1022"},
        {"%.2a", 0.0, "0x0.00p+0"},
        {"%.15a", 1.0, "0x1.000000000000000p+0"},
        {"%#.0a", 1.0, "0x1.p+0"},
        {"%+a", -0.0, "-0x0p+0"},
        {"% a", 1.0, " 0x1p+0"},
        {"%010a", 1.0, "0x00001p+0"},
        {"%-10a|", 1.0, "0x1p+0    |"},
        {"%A", 255.5, "0X1.FFP+7"},
        {"%12A", -2.0, "     -0X1P+1"},
        {"%012a", -2.0, "-0x000001p+1"},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        char text[64] = {0};

        TB_TEST_CONTEXT("\"%s\" of %s", rows[row].format, rows[row].text);
        (void)tb_fmt_buffer(text, sizeof(text), rows[row].format, rows[row].value);
        TB_TEST_EXPECT_STRING(text, ==, rows[row].text);
    }
}

TB_TEST(core_decimal, infinity_and_nan_have_one_spelling)
{
    static const struct
    {
        const char *format;
        uint64_t bits;
        const char *text;
    } rows[] = {
        {"%f", UINT64_C(0x7FF0000000000000), "inf"},
        {"%e", UINT64_C(0xFFF0000000000000), "-inf"},
        {"%G", UINT64_C(0x7FF0000000000000), "INF"},
        {"%A", UINT64_C(0xFFF0000000000000), "-INF"},
        {"%a", UINT64_C(0x7FF8000000000000), "nan"},
        /* NaN has no sign, whatever its bits. */
        {"%f", UINT64_C(0xFFF8000000000000), "nan"},
        {"%F", UINT64_C(0x7FF0000000000001), "NAN"},
        {"%+f", UINT64_C(0x7FF8000000000000), "+nan"},
        /* Zeros never pad a word. */
        {"%08f", UINT64_C(0xFFF0000000000000), "    -inf"},
        {"%-8f|", UINT64_C(0x7FF8000000000000), "nan     |"},
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        char text[32] = {0};

        TB_TEST_CONTEXT("\"%s\" of 0x%016llx", rows[row].format,
                        (unsigned long long)rows[row].bits);
        (void)tb_fmt_buffer(text, sizeof(text), rows[row].format, value_of(rows[row].bits));
        TB_TEST_EXPECT_STRING(text, ==, rows[row].text);
    }
}

TB_TEST(core_decimal, a_float_argument_is_formatted_as_the_double_it_becomes)
{
    const float third = 1.0f / 3.0f;
    char text[64] = {0};

    /* Passed through `...`, a float becomes a double; the casts say so. */
    (void)tb_fmt_buffer(text, sizeof(text), "%.10f %lf %g", (double)third, 0.5, (double)1e-5f);
    TB_TEST_EXPECT_STRING(text, ==, "0.3333333433 0.500000 1e-05");
}

TB_TEST(core_decimal, seventeen_significant_digits_read_back_as_the_same_bits)
{
    for (size_t round = 0; round < 20000; round++)
    {
        const uint64_t bits = tb_test_random_u64();
        char text[48] = {0};

        if (is_nan_bits(bits) ||
            (bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000))
        {
            continue;
        }
        (void)tb_fmt_buffer(text, sizeof(text), "%.17g", value_of(bits));
        TB_TEST_CONTEXT("0x%016llx: %s", (unsigned long long)bits, text);
        TB_TEST_EXPECT_HEX(bits_of(parse(text)), ==, bits);
    }
}

/* --- reading -------------------------------------------------------------------------- */

TB_TEST(core_decimal, reading_matches_python_float_exactly)
{
    for (size_t row = 0; row < TB_TEST_COUNT_OF(decimal_parse_vectors); row++)
    {
        const decimal_parse_vector *const vector = &decimal_parse_vectors[row];
        double value = 0.0;

        TB_TEST_CONTEXT("row %zu: \"%.80s\"", row, vector->text);
        TB_TEST_REQUIRE_INT(tb_decimal_parse(vector->text, tb_str_length(vector->text), &value), ==,
                            TB_DECIMAL_OK);
        TB_TEST_EXPECT_HEX(bits_of(value), ==, vector->bits);
    }
}

TB_TEST(core_decimal, rejects_malformed_text_and_leaves_the_value_alone)
{
    static const char *const rows[] = {
        "",     "+",    "-",     ".",       "e5",  "1e",    "1e+",   "1e-",      "1.2.3",
        " 1",   "1 ",   "1,5",   "--1",     "+-1", "1e5.5", "1e 5",  "0x",       "0x.",
        "0xp1", "0x1p", "0x1p+", "0x1.2.3", "0xg", "0x1e+", "in",    "infinit",  "infinityy",
        "nana", "na",   "inf ",  "1f",      "1d",  "0b101", "1_000", "\xD9\xA1", "1\xC2\xA0",
    };

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        double value = 42.0;

        TB_TEST_CONTEXT("\"%s\"", rows[row]);
        TB_TEST_EXPECT_INT(tb_decimal_parse(rows[row], tb_str_length(rows[row]), &value), ==,
                           TB_DECIMAL_INVALID);
        TB_TEST_EXPECT_FLOAT(value, ==, 42.0);
    }
}

TB_TEST(core_decimal, text_with_a_nul_byte_is_malformed)
{
    const char text[3] = {'1', '\0', '2'};
    double value = 42.0;

    TB_TEST_EXPECT_INT(tb_decimal_parse(text, sizeof(text), &value), ==, TB_DECIMAL_INVALID);
    TB_TEST_EXPECT_INT(tb_decimal_parse(NULL, 0, &value), ==, TB_DECIMAL_INVALID);
    TB_TEST_EXPECT_FLOAT(value, ==, 42.0);
}

TB_TEST(core_decimal, every_digit_of_a_long_text_counts)
{
    /* 1 + 2^-53 is exactly halfway between 1 and the next double, and ties
     * to 1. A single nonzero digit far beyond it tips the value up. */
    static const char halfway[] = "1.00000000000000011102230246251565404236316680908203125";
    const size_t tail = 5000;
    char *const text = tb_malloc(sizeof(halfway) + tail + 1);
    const size_t length = sizeof(halfway) - 1 + tail + 1;

    memcpy(text, halfway, sizeof(halfway) - 1);
    memset(text + sizeof(halfway) - 1, '0', tail);
    text[length - 1] = '0';
    TB_TEST_EXPECT_HEX(bits_of(parse(halfway)), ==, UINT64_C(0x3FF0000000000000));
    {
        double value = 0.0;

        TB_TEST_REQUIRE_INT(tb_decimal_parse(text, length, &value), ==, TB_DECIMAL_OK);
        TB_TEST_EXPECT_HEX(bits_of(value), ==, UINT64_C(0x3FF0000000000000));
        text[length - 1] = '1';
        TB_TEST_REQUIRE_INT(tb_decimal_parse(text, length, &value), ==, TB_DECIMAL_OK);
        TB_TEST_EXPECT_HEX(bits_of(value), ==, UINT64_C(0x3FF0000000000001));
    }
    tb_free(text);
}

/* --- contract violations -------------------------------------------------------------- */

static void parse_into_null(void *context)
{
    (void)context;
    (void)tb_decimal_parse("1", 1, NULL);
}

static void parse_null_text(void *context)
{
    double value = 0.0;

    (void)context;
    (void)tb_decimal_parse(NULL, 1, &value);
}

static void write_without_a_size_output(void *context)
{
    char text[TB_DECIMAL_SHORTEST_SIZE] = {0};

    (void)context;
    (void)tb_decimal_shortest(1.0, text, sizeof(text), NULL);
}

static void write_to_null_with_a_size(void *context)
{
    size_t required = 0;

    (void)context;
    (void)tb_decimal_shortest(1.0, NULL, 4, &required);
}

TB_TEST(core_decimal, contract_violations_are_caught)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(parse_into_null, NULL, "value != NULL");
    TB_TEST_EXPECT_TRAP(parse_null_text, NULL, "text != NULL || length == 0");
    TB_TEST_EXPECT_TRAP(write_without_a_size_output, NULL, "required != NULL");
    TB_TEST_EXPECT_TRAP(write_to_null_with_a_size, NULL, "buffer != NULL || size == 0");
}
