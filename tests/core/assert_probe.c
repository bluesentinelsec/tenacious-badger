/* Fails an assertion with the engine's default handler, which reports to
 * stderr and aborts. ExpectAssertAbort.cmake runs this executable as a child
 * process and checks how it ends; it is not registered with CTest directly. */
#include "badger/assert.h"

#include "tb_test.h"

#include <stdio.h>

TB_TEST(assert_probe, default_handler_reports_and_aborts)
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
