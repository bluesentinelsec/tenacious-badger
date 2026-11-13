/** @file platform.h
 * @brief Native services the test runner needs. One implementation per
 * platform lives in src/platform/ and is selected by the build.
 */
#ifndef TB_TEST_PLATFORM_H
#define TB_TEST_PLATFORM_H

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
