/* Cases that misbehave on purpose, to prove that the test support catches it.
 * ExpectTestSupport.cmake runs them in a child process and checks the result;
 * they are not registered with CTest directly. */
#include "badger/memory.h"

#include "engine_test.h"
#include "tb_test.h"

TB_TEST(support_probe, leaked_block_fails_the_case)
{
    unsigned char *const kept = tb_malloc(16);
    unsigned char *const leaked = tb_malloc(16);
    unsigned char *const also_leaked = tb_calloc(4, 4);

    /* Every check passes; only the leak check after the case can object. */
    TB_TEST_EXPECT_POINTER(kept, !=, leaked);
    TB_TEST_EXPECT_POINTER(leaked, !=, also_leaked);
    tb_free(kept);
}

TB_TEST(support_probe, failed_case_is_not_checked_for_leaks)
{
    unsigned char *const block = tb_malloc(16);

    /* The REQUIRE ends the case before the free below can run. Reporting that
     * as a leak would bury the real failure. */
    TB_TEST_REQUIRE_POINTER(block, ==, NULL);
    tb_free(block);
}

TB_TEST(support_probe, failure_injection_is_left_on)
{
    tb_engine_test_fail_allocations_after(0);
    TB_TEST_EXPECT_POINTER(tb_try_malloc(8), ==, NULL);
}

TB_TEST(support_probe, next_case_allocates_normally)
{
    /* Run after failure_injection_is_left_on: its injection must not leak
     * into this case. */
    void *const block = tb_try_malloc(8);

    TB_TEST_EXPECT_POINTER(block, !=, NULL);
    tb_free(block);
}
