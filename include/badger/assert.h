/** @file assert.h
 * @brief Developer assertions: build-time checks and debug-only invariants.
 *
 * Assertions state what the surrounding code relies on. They are for internal
 * invariants and caller contract violations, never for validating external
 * input or reporting runtime failures: release builds compile them out.
 */
#ifndef TB_ASSERT_H
#define TB_ASSERT_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

#if !defined(TB_ASSERT_ENABLED)
#if defined(NDEBUG)
/** @brief 1 when TB_ASSERT() checks its condition in this translation unit, otherwise 0.
 *
 * Follows NDEBUG like the standard assert(). Define it to 0 or 1 for a whole
 * target to override that default.
 */
#define TB_ASSERT_ENABLED 0
#else
/** @brief 1 when TB_ASSERT() checks its condition in this translation unit, otherwise 0.
 *
 * Follows NDEBUG like the standard assert(). Define it to 0 or 1 for a whole
 * target to override that default.
 */
#define TB_ASSERT_ENABLED 1
#endif
#endif

/**
 * @brief Describes one failed assertion.
 *
 * Every string is borrowed and has static storage duration, so a handler may
 * keep a copy of the structure for as long as it likes.
 */
typedef struct tb_assert_failure
{
    const char *expression; /**< Source text of the condition that was false; never NULL. */
    const char *message;    /**< Explanation from TB_ASSERT_MSG(); NULL for TB_ASSERT(). */
    const char *file;       /**< Source file of the assertion; never NULL. */
    const char *function;   /**< Enclosing function of the assertion; never NULL. */
    int line;               /**< Source line of the assertion. */
} tb_assert_failure;

/**
 * @brief Receives a failed assertion in place of the default report.
 * @param failure Borrowed description, valid for the duration of the call.
 * @param context The pointer registered with tb_assert_set_handler().
 * @par Errors A handler should not return. If it does, the process aborts,
 * because the code after a failed assertion cannot run safely.
 */
typedef void tb_assert_handler(const tb_assert_failure *failure, void *context);

/**
 * @brief Replace what happens when an assertion fails.
 *
 * The default writes the failure to stderr and aborts. A test harness installs
 * a handler to turn a failure into a test result instead of losing the process.
 *
 * @param handler Handler to install, or NULL to restore the default.
 * @param context Passed to every handler call; may be NULL. The caller keeps
 * ownership and must keep it valid while the handler is installed.
 * @par Threading Stage-owned: call during single-threaded startup or test
 * setup, before any other thread can fail an assertion.
 */
TB_API void tb_assert_set_handler(tb_assert_handler *handler, void *context);

/**
 * @brief Report a failed assertion and never return. Use the macros instead.
 * @param expression Source text of the failed condition; required non-null.
 * @param message Optional explanation, or NULL.
 * @param file Source file of the assertion; required non-null.
 * @param line Source line of the assertion.
 * @param function Enclosing function name; required non-null.
 * @par Ownership Every string must have static storage duration.
 * @par Threading Thread-safe once the handler is installed; concurrent failures
 * each reach the handler.
 */
TB_API _Noreturn void tb_assert_fail(const char *expression, const char *message, const char *file,
                                     int line, const char *function);

/**
 * @brief Report whether the engine library itself was built with assertions.
 *
 * TB_ASSERT_ENABLED describes the including translation unit; this describes
 * the library. Tests use it to skip checks of the library's own contract
 * assertions in builds that compile them out.
 *
 * @return true when the library's TB_ASSERT() checks are active.
 * @par Threading Thread-safe; the result is fixed at build time.
 */
TB_API bool tb_assert_library_enabled(void);

/**
 * @brief Fail the build when a constant condition is false.
 * @param condition Integer constant expression that must be non-zero.
 * @param message String literal shown by the compiler when the check fails.
 */
#define TB_STATIC_ASSERT(condition, message) _Static_assert(condition, message)

#if TB_ASSERT_ENABLED
/**
 * @brief Check an internal invariant in builds with assertions enabled.
 *
 * The condition is evaluated exactly once when assertions are enabled and not
 * at all when they are disabled, so it must not have side effects.
 *
 * @param condition Scalar expression that must be non-zero.
 */
#define TB_ASSERT(condition)                                                                       \
    ((condition) ? (void)0 : tb_assert_fail(#condition, NULL, __FILE__, __LINE__, __func__))

/**
 * @brief Check an internal invariant and explain it when it fails.
 * @param condition Scalar expression that must be non-zero; no side effects.
 * @param message String literal describing the violated expectation.
 */
#define TB_ASSERT_MSG(condition, message)                                                          \
    ((condition) ? (void)0 : tb_assert_fail(#condition, message, __FILE__, __LINE__, __func__))
#else
/**
 * @brief Check an internal invariant in builds with assertions enabled.
 *
 * Assertions are disabled here: the condition is compiled, so it cannot rot,
 * but it is never evaluated.
 *
 * @param condition Scalar expression that must be non-zero.
 */
#define TB_ASSERT(condition) ((void)sizeof((condition) ? 1 : 0))

/**
 * @brief Check an internal invariant and explain it when it fails.
 *
 * Assertions are disabled here: the condition is compiled but never evaluated.
 *
 * @param condition Scalar expression that must be non-zero; no side effects.
 * @param message String literal describing the violated expectation.
 */
#define TB_ASSERT_MSG(condition, message) ((void)sizeof((condition) ? 1 : 0))
#endif

#endif
