/* Fails assertions with the engine's default handler, which reports to stderr
 * and ends the process. Each case first restores that handler, because the
 * test entry point replaces it. ExpectAssertAbort.cmake runs each case as a child
 * process and checks how it ends; they are not registered with CTest directly. */
#include "badger/assert.h"

#include "badger/fmt.h"
#include "tb_test.h"

TB_TEST(assert_probe, failed_macro_reports_and_aborts)
{
    const int two = 2;

    tb_assert_set_handler(NULL, NULL);
    if (tb_assert_library_enabled())
    {
        (void)tb_fmt_print("assertions=enabled\n");
    }
    else
    {
        (void)tb_fmt_print("assertions=disabled\n");
    }
    (void)tb_fmt_print("assert-line=%d\n", __LINE__ + 2);
    (void)tb_fmt_flush();
    TB_ASSERT_MSG(two + two == 5, "arithmetic is intact");
    (void)tb_fmt_print("after-assert\n");
}

static void fail_through_the_function(void)
{
    tb_assert_fail("probe == call", NULL, "probe_file.c", 77, "probe_function");
}

TB_TEST(assert_probe, failure_function_aborts_in_every_build)
{
    /* The function behind the macros exists in builds that compile the macros
     * out, so this case reaches the abort in Release as well. */
    tb_assert_set_handler(NULL, NULL);
    fail_through_the_function();
}
