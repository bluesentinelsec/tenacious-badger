/** @file engine_test.h
 * @brief Support shared by the engine's test cases.
 *
 * The test executable prepares the engine so that its fatal paths become test
 * results instead of ending the process:
 *
 * - A failed TB_ASSERT() is reported to the test framework as a trap.
 * - A fail-fast allocation that runs out of memory is reported as a trap.
 * - Every case is checked for leaked allocations when it ends.
 *
 * An unexpected trap fails the running case, and TB_TEST_EXPECT_TRAP() can
 * prove that a contract violation or an exhausted allocation is handled.
 *
 * @par Threading
 * Stage-owned: call everything here from the thread that runs the test cases.
 */
#ifndef TB_ENGINE_TEST_H
#define TB_ENGINE_TEST_H

#include "badger/assert.h"

#include <stddef.h>

/**
 * @brief Route failed engine assertions to the test framework as traps.
 *
 * Done once before the cases run. A case that replaces the assertion handler
 * calls it again to restore this routing.
 */
void tb_engine_test_install_assert_handler(void);

/**
 * @brief The assertion failure most recently reported to the test framework.
 * @return Borrowed description, valid until the next assertion fails. Its
 * `expression` is NULL until the first failure.
 */
const tb_assert_failure *tb_engine_test_last_assert_failure(void);

/**
 * @brief Install the memory functions that let cases make allocations fail.
 *
 * Done once before the cases run. A case that replaces the engine's memory
 * functions calls it again, with nothing allocated, to restore these.
 */
void tb_engine_test_install_memory_functions(void);

/**
 * @brief Route exhausted fail-fast allocations to the test framework as traps.
 *
 * The trap message starts with "out of memory". Done once before the cases
 * run. A case that replaces the failure handler calls it again to restore this
 * routing.
 */
void tb_engine_test_install_memory_failure_handler(void);

/**
 * @brief Make the engine's allocations start failing, to test how code copes.
 *
 * Counts every attempt to allocate or resize a block. Ends automatically when
 * the case ends.
 *
 * @param successes Number of further attempts that still succeed. Every
 * attempt after them fails as if memory were exhausted.
 */
void tb_engine_test_fail_allocations_after(size_t successes);

/** @brief Let allocations succeed again. */
void tb_engine_test_stop_failing_allocations(void);

/** @brief Case hook: note what is allocated before the case body runs. */
void tb_engine_test_before_case(void *context);

/**
 * @brief Case hook: fail a case that passed its checks but leaked allocations.
 *
 * A case that already failed is not checked, because a failed `REQUIRE` skips
 * the cleanup that follows it.
 */
void tb_engine_test_after_case(void *context);

#endif
