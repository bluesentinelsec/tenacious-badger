/* TB_ASSERT guards every contract in the engine, so its own behavior is pinned
 * here: it evaluates its condition once, reports exactly where and why it
 * failed, and costs nothing when compiled out. The default report-and-abort
 * path is checked by assert_probe.c in a child process. */
#include "badger/assert.h"

#include "engine_test.h"
#include "tb_test.h"

#include <string.h>

TB_STATIC_ASSERT(sizeof(char) == 1, "a static assertion must compile at file scope");
TB_STATIC_ASSERT(TB_ASSERT_ENABLED == 0 || TB_ASSERT_ENABLED == 1,
                 "TB_ASSERT_ENABLED must be usable as a truth value");

/* Whether TB_ASSERT() is active in this translation unit. */
static bool assertions_are_enabled_here(void)
{
    return TB_ASSERT_ENABLED != 0;
}

static int evaluation_count = {0};

static bool count_evaluation(bool value)
{
    evaluation_count++;
    return value;
}

TB_TEST(core_assert, static_assert_compiles_in_block_scope)
{
    /* Compiling is the test; a false condition would fail the build. */
    TB_STATIC_ASSERT(sizeof(int) >= 2, "a static assertion must compile in a block");
}

TB_TEST(core_assert, library_and_tests_share_one_assertion_setting)
{
    /* The cases below ask the library whether assertions are enabled and then
     * use the macros of this file, so the two settings must agree. */
    TB_TEST_EXPECT(tb_assert_library_enabled() == assertions_are_enabled_here());
}

TB_TEST(core_assert, true_condition_is_evaluated_once_when_enabled)
{
    evaluation_count = 0;
    TB_ASSERT(count_evaluation(true));
    TB_ASSERT_MSG(count_evaluation(true), "never shown");
    if (tb_assert_library_enabled())
    {
        TB_TEST_EXPECT_INT(evaluation_count, ==, 2);
    }
    else
    {
        /* Compiled out: the condition is type-checked but never run. */
        TB_TEST_EXPECT_INT(evaluation_count, ==, 0);
    }
}

TB_TEST(core_assert, disabled_assertions_do_nothing)
{
    if (tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions are enabled in this build");
    }
    evaluation_count = 0;
    TB_ASSERT(count_evaluation(false));
    TB_ASSERT_MSG(count_evaluation(false), "never shown");
    TB_TEST_EXPECT_INT(evaluation_count, ==, 0);
}

/* Each body records the line of its assertion for the case to compare. */
static int failing_assert_line = {0};

static void fail_plain_assert(void *context)
{
    const int *const value = context;

    failing_assert_line = __LINE__ + 1;
    TB_ASSERT(*value == 2);
}

static void fail_assert_with_message(void *context)
{
    const int *const value = context;

    failing_assert_line = __LINE__ + 1;
    TB_ASSERT_MSG(*value > 100, "value must be large");
}

TB_TEST(core_assert, failure_reports_expression_and_location)
{
    int value = 1;
    const tb_assert_failure *failure = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions are compiled out of this build");
    }
    TB_TEST_REQUIRE_TRAP(fail_plain_assert, &value, "assertion failed: *value == 2");
    failure = tb_engine_test_last_assert_failure();
    TB_TEST_EXPECT_STRING(failure->expression, ==, "*value == 2");
    TB_TEST_EXPECT_POINTER(failure->message, ==, NULL);
    TB_TEST_EXPECT_STRING(failure->function, ==, "fail_plain_assert");
    TB_TEST_EXPECT_INT(failure->line, ==, failing_assert_line);
    TB_TEST_EXPECT(strstr(failure->file, "assert_test.c") != NULL);
}

TB_TEST(core_assert, failure_carries_its_message)
{
    int value = 1;
    const tb_assert_failure *failure = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions are compiled out of this build");
    }
    TB_TEST_REQUIRE_TRAP(fail_assert_with_message, &value,
                         "assertion failed: *value > 100 (value must be large)");
    failure = tb_engine_test_last_assert_failure();
    TB_TEST_EXPECT_STRING(failure->expression, ==, "*value > 100");
    TB_TEST_EXPECT_STRING(failure->message, ==, "value must be large");
    TB_TEST_EXPECT_STRING(failure->function, ==, "fail_assert_with_message");
    TB_TEST_EXPECT_INT(failure->line, ==, failing_assert_line);
}

/* A handler with its own context, to prove replacement and context delivery. */
typedef struct recorded_failure
{
    int calls;
    const char *expression;
} recorded_failure;

static void record_failure_then_trap(const tb_assert_failure *failure, void *context)
{
    recorded_failure *const record = context;

    record->calls++;
    record->expression = failure->expression;
    tb_test_trap(failure->file, failure->line, "recorded by the replacement handler");
}

static void fail_through_the_function(void *context)
{
    (void)context;
    tb_assert_fail("direct == call", NULL, "direct_file.c", 77, "direct_function");
}

TB_TEST(core_assert, handler_can_be_replaced_and_receives_its_context)
{
    /* tb_assert_fail() is the function behind the macros, so this case runs
     * in every build, including those that compile the macros out. */
    recorded_failure record = {0};

    tb_assert_set_handler(record_failure_then_trap, &record);
    TB_TEST_EXPECT_TRAP(fail_through_the_function, NULL, "recorded by the replacement handler");
    TB_TEST_EXPECT_TRAP(fail_through_the_function, NULL, "recorded by the replacement handler");
    /* Put the shared handler back before any check can end this case. */
    tb_engine_test_install_assert_handler();
    TB_TEST_EXPECT_INT(record.calls, ==, 2);
    TB_TEST_EXPECT_STRING(record.expression, ==, "direct == call");
}
