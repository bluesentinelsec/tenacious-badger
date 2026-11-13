/** @file engine_test.h
 * @brief Support shared by the engine's test executables.
 *
 * Engine tests run with an assertion handler that reports a failed TB_ASSERT()
 * to the test framework as a trap. An unexpected assertion fails the running
 * case instead of ending the process, and TB_TEST_EXPECT_TRAP() can prove that
 * a contract violation is caught.
 */
#ifndef TB_ENGINE_TEST_H
#define TB_ENGINE_TEST_H

#include "badger/assert.h"

/**
 * @brief Route failed engine assertions to the test framework as traps.
 *
 * The engine test `main` calls it before any case runs. A case that replaces
 * the assertion handler calls it again to restore this routing.
 *
 * @par Threading Stage-owned: call from the thread that runs the test cases.
 */
void tb_engine_test_install_assert_handler(void);

/**
 * @brief The assertion failure most recently reported to the test framework.
 * @return Borrowed description, valid until the next assertion fails. Its
 * `expression` is NULL until the first failure.
 * @par Threading Stage-owned: call from the thread that runs the test case.
 */
const tb_assert_failure *tb_engine_test_last_assert_failure(void);

#endif
