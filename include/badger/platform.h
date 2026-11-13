/** @file platform.h
 * @brief Native platform service interfaces. Implementations are build-selected.
 */
#ifndef TB_PLATFORM_H
#define TB_PLATFORM_H

#include "badger/api.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Read the native monotonic performance counter in opaque ticks.
 * @return Counter value on Windows. The epoch and tick period are unspecified;
 * use values only to compare readings during the current process lifetime.
 * @par Errors Native counter failure terminates with a diagnostic. Linux and
 * macOS terminate with a fatal-stub diagnostic naming the planned native API.
 * @par Ownership No allocations or caller-owned resources.
 * @par Threading Thread-safe; no shared mutable engine state.
 */
TB_API uint64_t tb_platform_ticks(void);

/**
 * @brief End the process abnormally, at once.
 *
 * The last step of an unrecoverable failure, such as a failed assertion, after
 * the failure has been reported. An attached debugger stops at the call. With
 * no debugger the process ends with a failure status and never waits on an
 * error dialog, so unattended runs fail fast.
 *
 * @par Errors Does not return.
 * @par Ownership Releases nothing and runs no exit handlers. Flush any output
 * that must survive before calling.
 * @par Threading Thread-safe; the first call ends the process.
 */
TB_API _Noreturn void tb_platform_abort(void);

/**
 * @brief A `size_t` that several threads may update without a lock.
 *
 * Zero-initialize it. Once it is shared between threads, read and write
 * `value` only through the `tb_platform_atomic_size_*` functions.
 */
typedef struct tb_platform_atomic_size
{
    size_t value; /**< Current value; private to the atomic functions. */
} tb_platform_atomic_size;

/**
 * @brief Add to an atomic value as one indivisible step.
 * @param atomic Required non-null.
 * @param amount Amount to add; the sum wraps around like any `size_t`.
 * @return The value after the addition.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API size_t tb_platform_atomic_size_add(tb_platform_atomic_size *atomic, size_t amount);

/**
 * @brief Subtract from an atomic value as one indivisible step.
 * @param atomic Required non-null.
 * @param amount Amount to subtract; the difference wraps around like any `size_t`.
 * @return The value after the subtraction.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API size_t tb_platform_atomic_size_subtract(tb_platform_atomic_size *atomic, size_t amount);

/**
 * @brief Read an atomic value.
 * @param atomic Required non-null.
 * @return The value at some instant during the call.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API size_t tb_platform_atomic_size_load(tb_platform_atomic_size *atomic);

#endif
