/* Cases that fail, skip, trap, and crash on purpose. RunScenario.cmake runs this
 * executable and checks its exit status and report, which is how the framework's
 * failure paths are tested. It is never registered with CTest directly. */
#include "tb_test.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* --- pass ------------------------------------------------------------------ */

TB_TEST(pass, no_checks)
{
}

TB_TEST(pass, several_checks)
{
    TB_TEST_EXPECT(true);
    TB_TEST_EXPECT_INT(2 + 2, ==, 4);
    TB_TEST_REQUIRE_STRING("badger", ==, "badger");
}

/* --- fail ------------------------------------------------------------------ */

TB_TEST(fail, expect_continues)
{
    /* The scenario compares this printed line with the reported location. */
    printf("check-line=%d\n", __LINE__ + 1);
    TB_TEST_EXPECT(1 + 1 == 3);
    puts("after-first-expect");
    TB_TEST_EXPECT_INT(2 + 2, ==, 5);
    puts("after-second-expect");
}

TB_TEST(fail, require_stops)
{
    TB_TEST_REQUIRE_INT(7, <, 3);
    puts("after-require");
}

static void require_a_false_condition(void)
{
    TB_TEST_REQUIRE(false);
    puts("after-require-in-helper");
}

TB_TEST(fail, require_in_helper)
{
    require_a_false_condition();
    puts("after-helper");
}

TB_TEST(fail, uint_values)
{
    const size_t used = 3;
    const size_t capacity = 2;

    TB_TEST_EXPECT_UINT(used, <=, capacity);
}

TB_TEST(fail, hex_values)
{
    TB_TEST_EXPECT_HEX(0xFFu, ==, 0xF0u);
}

TB_TEST(fail, float_values)
{
    TB_TEST_EXPECT_FLOAT(0.5, >, 1.5);
}

TB_TEST(fail, float_near)
{
    TB_TEST_EXPECT_FLOAT_NEAR(1.0, 2.0, 0.25);
}

TB_TEST(fail, pointer_values)
{
    const int object = 0;

    TB_TEST_EXPECT_POINTER(&object, ==, NULL);
}

TB_TEST(fail, string_values)
{
    TB_TEST_EXPECT_STRING("badger\n", ==, "badge\tr");
}

TB_TEST(fail, string_null)
{
    const char *const missing = NULL;

    TB_TEST_EXPECT_STRING(missing, ==, "text");
}

TB_TEST(fail, bytes_values)
{
    enum
    {
        BYTE_COUNT = 32,
        DIFFERENT_OFFSET = 20
    };
    unsigned char actual[BYTE_COUNT] = {0};
    unsigned char expected[BYTE_COUNT] = {0};

    for (size_t index = 0; index < BYTE_COUNT; index++)
    {
        actual[index] = (unsigned char)index;
        expected[index] = (unsigned char)index;
    }
    expected[DIFFERENT_OFFSET] = 0xEE;
    TB_TEST_EXPECT_BYTES(actual, expected, sizeof(actual));
}

TB_TEST(fail, bytes_null)
{
    const unsigned char expected[4] = {1, 2, 3, 4};
    const unsigned char *const missing = NULL;

    TB_TEST_EXPECT_BYTES(missing, expected, sizeof(expected));
}

TB_TEST(fail, context_is_reported)
{
    TB_TEST_CONTEXT("row %d of %s", 3, "table");
    TB_TEST_EXPECT(false);
}

TB_TEST(fail, context_is_cleared)
{
    TB_TEST_CONTEXT("stale description");
    tb_test_context_clear();
    TB_TEST_EXPECT(false);
}

TB_TEST(fail, explicit_failure)
{
    TB_TEST_FAIL("unexpected tag %d", 42);
}

static void return_without_trapping(void *context)
{
    (void)context;
}

static void trap_with_alpha(void *context)
{
    (void)context;
    tb_test_trap(__FILE__, __LINE__, "alpha condition");
}

TB_TEST(fail, unexpected_trap)
{
    tb_test_trap("engine_source.c", 1234, "fixture trap message");
}

TB_TEST(fail, missing_trap)
{
    TB_TEST_EXPECT_TRAP(return_without_trapping, NULL, "");
    puts("after-missing-trap");
}

TB_TEST(fail, wrong_trap_message)
{
    TB_TEST_EXPECT_TRAP(trap_with_alpha, NULL, "beta");
}

TB_TEST(fail, require_trap_stops)
{
    TB_TEST_REQUIRE_TRAP(return_without_trapping, NULL, "");
    puts("after-require-trap");
}

/* --- skip ------------------------------------------------------------------ */

TB_TEST(skip, with_reason)
{
    TB_TEST_SKIP("needs %s", "a GPU");
}

TB_TEST(skip, after_failure)
{
    TB_TEST_EXPECT(false);
    TB_TEST_SKIP("a failed case stays failed");
}

/* --- trap ------------------------------------------------------------------ */

TB_TEST(trap, expected_trap_passes)
{
    TB_TEST_EXPECT_TRAP(trap_with_alpha, NULL, "alpha");
    puts("after-expected-trap");
}

/* --- random ---------------------------------------------------------------- */

static void print_random_values(const char *label)
{
    const uint64_t first = tb_test_random_u64();
    const uint64_t second = tb_test_random_u64();

    printf("random-values[%s]=%016" PRIx64 ",%016" PRIx64 "\n", label, first, second);
    printf("random-seed[%s]=0x%" PRIX64 "\n", label, tb_test_random_seed());
}

TB_TEST(random, first_stream)
{
    print_random_values("first");
}

TB_TEST(random, second_stream)
{
    print_random_values("second");
}

/* --- order ----------------------------------------------------------------- */

TB_TEST(order, a)
{
}

TB_TEST(order, b)
{
}

TB_TEST(order, c)
{
}

TB_TEST(order, d)
{
}

TB_TEST(order, e)
{
}

TB_TEST(order, f)
{
}

/* --- crash ----------------------------------------------------------------- */

TB_TEST(crash, aborts)
{
    abort();
}
