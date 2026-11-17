/** @file platform.h
 * @brief Native services the test runner needs. One implementation per
 * platform lives in src/platform/ and is selected by the build.
 */
#ifndef TB_TEST_PLATFORM_H
#define TB_TEST_PLATFORM_H
#include <stddef.h>

/** @brief Identify a crashing case using an already formatted diagnostic.
 * @param message Immutable bytes borrowed until crash_end, including a newline.
 * @param length Byte count, at most 2047.
 * @par Threading Runner-owned; begin before starting case workers and end after
 * joining them. Existing handlers are preserved. Never call from a handler.
 * Reporting is best effort and preserves the native termination status.
 */
void tb_test_platform_crash_begin(const char *message, size_t length);
/** @brief Restore handlers installed by crash_begin and release its borrow. */
void tb_test_platform_crash_end(void);

/**
 * @brief Make a crash end the process instead of opening a blocking dialog.
 *
 * Unattended runs (CTest, CI) must fail fast. Call once before running cases.
 */
void tb_test_platform_disable_error_dialogs(void);

/**
 * @brief Stop in an attached debugger at the call site.
 * @par Errors Without a debugger the platform's default action applies, which
 * normally terminates the process.
 */
void tb_test_platform_debug_break(void);

/**
 * @brief Print a source location to stdout in the form the platform's
 * compilers use, so editors and IDEs turn it into a link.
 * @param file Source file; required non-null.
 * @param line Source line.
 */
void tb_test_platform_print_location(const char *file, int line);

#endif
