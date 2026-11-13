/* Fails assertions with the engine's default handler, which reports to stderr
 * and ends the process. ExpectAssertAbort.cmake runs each case as a child
 * process and checks how it ends; they are not registered with CTest directly. */
#include "badger/assert.h"

#include "tb_test.h"

#include <stdio.h>

TB_TEST(assert_probe, failed_macro_reports_and_aborts)
{
    const int two = 2;

    if (tb_assert_library_enabled())
    {
        puts("assertions=enabled");
    }
    else
    {
        puts("assertions=disabled");
    }
    printf("assert-line=%d\n", __LINE__ + 2);
    (void)fflush(stdout);
    TB_ASSERT_MSG(two + two == 5, "arithmetic is intact");
    puts("after-assert");
}

static void fail_through_the_function(void)
{
    tb_assert_fail("probe == call", NULL, "probe_file.c", 77, "probe_function");
}

TB_TEST(assert_probe, failure_function_aborts_in_every_build)
{
    /* The function behind the macros exists in builds that compile the macros
     * out, so this case reaches the abort in Release as well. */
    fail_through_the_function();
}
