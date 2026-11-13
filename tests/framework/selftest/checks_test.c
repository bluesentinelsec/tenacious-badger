/* The passing side of every check: values that must be accepted, at their
 * limits, and the guarantee that operands are evaluated exactly once. The
 * failing side is covered by the fixture scenarios in RunScenario.cmake. */
#include "internal.h"

#include <float.h>
#include <limits.h>
#include <stdlib.h>

static int evaluation_count = {0};

static int count_evaluation(int value)
{
    evaluation_count++;
    return value;
}

static const char *count_string_evaluation(const char *value)
{
    evaluation_count++;
    return value;
}

TB_TEST(checks, boolean_checks_accept_any_true_scalar)
{
    const int object = 7;
    const int *const address = &object;
    const double fraction = 0.5;

    TB_TEST_EXPECT(object == 7);
    TB_TEST_EXPECT(object);
    TB_TEST_EXPECT(-object);
    TB_TEST_EXPECT(address);
    TB_TEST_EXPECT(fraction);
    TB_TEST_REQUIRE(object > 1);
}

TB_TEST(checks, integer_checks_cover_the_full_range)
{
    TB_TEST_EXPECT_INT(0, ==, 0);
    TB_TEST_EXPECT_INT(-1, <, 0);
    TB_TEST_EXPECT_INT(INTMAX_MIN, <, INTMAX_MAX);
    TB_TEST_EXPECT_INT(INTMAX_MIN, ==, INTMAX_MIN);
    TB_TEST_EXPECT_INT(INTMAX_MAX, >=, INTMAX_MAX);
    TB_TEST_EXPECT_INT(INT_MIN, !=, INT_MAX);
    TB_TEST_REQUIRE_INT((signed char)-128, <=, (short)-128);

    TB_TEST_EXPECT_UINT(0u, ==, 0u);
    TB_TEST_EXPECT_UINT(UINTMAX_MAX, >, UINTMAX_MAX - 1);
    TB_TEST_EXPECT_UINT(sizeof(char), ==, 1);
    TB_TEST_EXPECT_UINT(SIZE_MAX, <=, UINTMAX_MAX);
    TB_TEST_REQUIRE_UINT((unsigned char)255, !=, 0u);

    TB_TEST_EXPECT_HEX(UINT64_C(0xDEADBEEF), ==, UINT64_C(0xDEADBEEF));
    TB_TEST_EXPECT_HEX(0x0Fu, <, 0xF0u);
    TB_TEST_REQUIRE_HEX(UINT64_MAX, >=, UINT64_MAX);
}

TB_TEST(checks, float_checks_compare_exactly_and_within_tolerance)
{
    const double infinity = strtod("inf", NULL);
    const double not_a_number = strtod("nan", NULL);

    TB_TEST_EXPECT_FLOAT(1.5, ==, 1.5);
    TB_TEST_EXPECT_FLOAT(-0.0, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(1.0f, <, 1.5f);
    TB_TEST_EXPECT_FLOAT(DBL_MAX, >, -DBL_MAX);
    TB_TEST_EXPECT_FLOAT(not_a_number, !=, not_a_number);
    TB_TEST_REQUIRE_FLOAT(infinity, >, DBL_MAX);

    TB_TEST_EXPECT_FLOAT_NEAR(1.0, 1.0, 0.0);
    TB_TEST_EXPECT_FLOAT_NEAR(1.0, 1.25, 0.25);
    TB_TEST_EXPECT_FLOAT_NEAR(-1.0, 1.0, 2.0);
    TB_TEST_EXPECT_FLOAT_NEAR(0.1 + 0.2, 0.3, 1e-12);
    /* Equal infinities are near even though their difference is not a number. */
    TB_TEST_EXPECT_FLOAT_NEAR(infinity, infinity, 0.0);
    TB_TEST_REQUIRE_FLOAT_NEAR(1.0f, 1.0f, 0.0f);
}

TB_TEST(checks, pointer_checks_compare_addresses)
{
    const int values[2] = {1, 2};
    const int *const missing = NULL;

    TB_TEST_EXPECT_POINTER(&values[0], ==, values);
    TB_TEST_EXPECT_POINTER(&values[0], !=, &values[1]);
    TB_TEST_EXPECT_POINTER(&values[0], <, &values[1]);
    TB_TEST_EXPECT_POINTER(missing, ==, NULL);
    TB_TEST_REQUIRE_POINTER(values, !=, NULL);
}

TB_TEST(checks, string_checks_order_by_content)
{
    const char *const missing = NULL;
    char copy[] = "badger";

    TB_TEST_EXPECT_STRING(copy, ==, "badger");
    TB_TEST_EXPECT_STRING("", ==, "");
    TB_TEST_EXPECT_STRING("badger", !=, "Badger");
    TB_TEST_EXPECT_STRING("a", <, "b");
    TB_TEST_EXPECT_STRING("ab", >, "a");
    TB_TEST_EXPECT_STRING("abc", <=, "abc");
    TB_TEST_EXPECT_STRING("abd", >=, "abc");
    TB_TEST_EXPECT_STRING(missing, ==, NULL);
    TB_TEST_EXPECT_STRING(missing, !=, "");
    TB_TEST_REQUIRE_STRING(missing, <, "");
}

typedef struct string_order_row
{
    const char *left;
    const char *right;
    int order;
} string_order_row;

TB_TEST(checks, string_order_is_normalized_and_total_over_null)
{
    static const string_order_row rows[] = {
        {NULL, NULL, 0},
        {NULL, "", -1},
        {"", NULL, 1},
        {"", "", 0},
        {"a", "a", 0},
        {"a", "b", -1},
        {"b", "a", 1},
        {"a", "ab", -1},
        {"ab", "a", 1},
        {"abc", "abd", -1},
        {"Z", "a", -1},
        {"a", "Z", 1},
        /* Bytes compare as unsigned, as strcmp requires. */
        {"\x7f", "\x80", -1},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_INT(tb_test_compare_strings(rows[index].left, rows[index].right), ==,
                           rows[index].order);
        /* Swapping the operands must negate the order. */
        TB_TEST_EXPECT_INT(tb_test_compare_strings(rows[index].right, rows[index].left), ==,
                           -rows[index].order);
    }
}

TB_TEST(checks, byte_checks_accept_equal_and_empty_ranges)
{
    const unsigned char first[] = {0x00, 0x01, 0xFE, 0xFF};
    const unsigned char second[] = {0x00, 0x01, 0xFE, 0xFF, 0x55};
    const unsigned char *const missing = NULL;

    TB_TEST_EXPECT_BYTES(first, first, sizeof(first));
    TB_TEST_EXPECT_BYTES(first, second, sizeof(first));
    /* Bytes beyond the given size are not part of the comparison. */
    TB_TEST_EXPECT_BYTES(second, first, sizeof(first));
    TB_TEST_EXPECT_BYTES(first, second, 0);
    TB_TEST_EXPECT_BYTES(missing, missing, 0);
    TB_TEST_REQUIRE_BYTES("text", "text", sizeof("text"));
}

TB_TEST(checks, operands_are_evaluated_exactly_once)
{
    /* A check that evaluated an operand twice would break every test that
     * passes a call with side effects, such as popping from a container. */
    evaluation_count = 0;
    TB_TEST_EXPECT(count_evaluation(1));
    TB_TEST_EXPECT_INT(evaluation_count, ==, 1);

    evaluation_count = 0;
    TB_TEST_EXPECT_INT(count_evaluation(4), ==, count_evaluation(4));
    TB_TEST_EXPECT_INT(evaluation_count, ==, 2);

    evaluation_count = 0;
    TB_TEST_EXPECT_UINT((unsigned int)count_evaluation(4), <, (unsigned int)count_evaluation(5));
    TB_TEST_EXPECT_HEX((unsigned int)count_evaluation(4), !=, (unsigned int)count_evaluation(5));
    TB_TEST_EXPECT_INT(evaluation_count, ==, 4);

    evaluation_count = 0;
    TB_TEST_EXPECT_FLOAT(count_evaluation(1), <, count_evaluation(2));
    TB_TEST_EXPECT_FLOAT_NEAR(count_evaluation(1), count_evaluation(1), count_evaluation(0));
    TB_TEST_EXPECT_INT(evaluation_count, ==, 5);

    evaluation_count = 0;
    TB_TEST_EXPECT_STRING(count_string_evaluation("a"), <, count_string_evaluation("b"));
    TB_TEST_EXPECT_POINTER(count_string_evaluation("a"), !=, count_string_evaluation(NULL));
    TB_TEST_EXPECT_BYTES(count_string_evaluation("ab"), count_string_evaluation("ab"),
                         (size_t)count_evaluation(2));
    TB_TEST_EXPECT_INT(evaluation_count, ==, 7);
}

TB_TEST(checks, count_of_measures_arrays)
{
    const int three[3] = {0};
    const char text[] = "four";
    const double matrix[2][5] = {{0}};

    TB_TEST_EXPECT_UINT(TB_TEST_COUNT_OF(three), ==, 3);
    TB_TEST_EXPECT_UINT(TB_TEST_COUNT_OF(text), ==, 5);
    TB_TEST_EXPECT_UINT(TB_TEST_COUNT_OF(matrix), ==, 2);
    TB_TEST_EXPECT_UINT(TB_TEST_COUNT_OF(matrix[0]), ==, 5);
}

TB_TEST(checks, context_does_not_affect_passing_checks)
{
    static const char long_text[] =
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789";

    TB_TEST_CONTEXT("plain text");
    TB_TEST_EXPECT(true);
    TB_TEST_CONTEXT("%s %d %zu", "formatted", -1, (size_t)2);
    TB_TEST_EXPECT(true);
    /* A description longer than the buffer is cut off, never overrun. */
    TB_TEST_CONTEXT("%s%s%s%s%s", long_text, long_text, long_text, long_text, long_text);
    TB_TEST_EXPECT(true);
    tb_test_context_clear();
    tb_test_context_clear();
    TB_TEST_EXPECT(true);
}

typedef struct trap_request
{
    const char *message;
    int calls;
} trap_request;

static void raise_requested_trap(void *context)
{
    trap_request *const request = context;

    request->calls++;
    tb_test_trap(__FILE__, __LINE__, request->message);
}

TB_TEST(checks, trap_checks_accept_a_matching_trap)
{
    static const char long_message[] =
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789012345678901234567890123456789";
    trap_request request = {.message = "index 9 is out of range for 4 elements", .calls = 0};

    TB_TEST_EXPECT_TRAP(raise_requested_trap, &request, "out of range");
    TB_TEST_EXPECT_TRAP(raise_requested_trap, &request, "index 9");
    /* An empty part accepts any trap. */
    TB_TEST_EXPECT_TRAP(raise_requested_trap, &request, "");
    TB_TEST_REQUIRE_TRAP(raise_requested_trap, &request, "index 9 is out of range for 4 elements");
    /* The body ran once per check, and the case continued after each trap. */
    TB_TEST_EXPECT_INT(request.calls, ==, 4);

    /* A message longer than the runner's buffer is cut off, never overrun. */
    request.message = long_message;
    TB_TEST_EXPECT_TRAP(raise_requested_trap, &request, "0123456789");
}
