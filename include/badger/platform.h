/** @file platform.h
 * @brief Native platform service interfaces. Implementations are build-selected.
 */
#ifndef TB_PLATFORM_H
#define TB_PLATFORM_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Read the monotonic clock, in nanoseconds.
 *
 * The clock counts from a moment that is fixed while the process runs and is
 * otherwise unspecified, so one reading means nothing alone: subtract an
 * earlier reading from a later one for the time between them. It never goes
 * backward, and a change to the date or the time of day does not move it.
 * Whether it advances while the system is suspended depends on the platform.
 *
 * @return Nanoseconds since the clock's starting moment. Readings are as fine
 * as the native counter, typically 100 nanoseconds or finer. The count does
 * not wrap for more than 500 years.
 * @par Errors A native clock that cannot be read ends the process with a
 * diagnostic.
 * @par Ownership No allocations or caller-owned resources.
 * @par Threading Thread-safe; no shared mutable engine state.
 */
TB_API uint64_t tb_platform_clock_nanoseconds(void);

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
 *
 * A null pointer given to any atomic function of this header, where one is
 * required, fails an assertion in a build with assertions.
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
 * @brief Read an atomic value. Nothing is written, so the value may be part
 * of an object that the caller has only for reading.
 * @param atomic Required non-null.
 * @return The value at some instant during the call.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API size_t tb_platform_atomic_size_load(const tb_platform_atomic_size *atomic);

/**
 * @brief Write an atomic value.
 * @param atomic Required non-null.
 * @param value The new value.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API void tb_platform_atomic_size_store(tb_platform_atomic_size *atomic, size_t value);

/**
 * @brief Write an atomic value and learn what it replaced, as one indivisible step.
 * @param atomic Required non-null.
 * @param value The new value.
 * @return The value from just before the write.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API size_t tb_platform_atomic_size_exchange(tb_platform_atomic_size *atomic, size_t value);

/**
 * @brief Write an atomic value only if it still holds what the caller last saw.
 *
 * As tb_platform_atomic_u32_compare_exchange(), which explains the use.
 *
 * @param atomic Required non-null.
 * @param[in,out] expected Required non-null. On entry, the value the caller
 * expects to find. On failure it receives the value found; on success it is
 * unchanged.
 * @param desired The value to write when `*expected` is found.
 * @return true when `desired` was written; false when the value was not
 * `*expected`, in which case nothing was written. It does not fail when the
 * value is as expected.
 * @par Threading Thread-safe; sequentially consistent. `expected` belongs to
 * the calling thread.
 */
TB_API bool tb_platform_atomic_size_compare_exchange(tb_platform_atomic_size *atomic,
                                                     size_t *expected, size_t desired);

/**
 * @brief A 32-bit value that several threads may read and write without a lock.
 *
 * Zero-initialize it. Once it is shared between threads, read and write
 * `value` only through the `tb_platform_atomic_u32_*` functions.
 */
typedef struct tb_platform_atomic_u32
{
    uint32_t value; /**< Current value; private to the atomic functions. */
} tb_platform_atomic_u32;

/**
 * @brief Read an atomic value.
 * @param atomic Required non-null.
 * @return The value at some instant during the call.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API uint32_t tb_platform_atomic_u32_load(const tb_platform_atomic_u32 *atomic);

/**
 * @brief Write an atomic value.
 * @param atomic Required non-null.
 * @param value The new value.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API void tb_platform_atomic_u32_store(tb_platform_atomic_u32 *atomic, uint32_t value);

/**
 * @brief Write an atomic value and learn what it replaced, as one indivisible step.
 * @param atomic Required non-null.
 * @param value The new value.
 * @return The value from just before the write.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API uint32_t tb_platform_atomic_u32_exchange(tb_platform_atomic_u32 *atomic, uint32_t value);

/**
 * @brief Write an atomic value only if it still holds what the caller last saw.
 *
 * Comparing and writing are one indivisible step. This is how a thread
 * changes a value that it computed from an earlier reading: when another
 * thread got there first, the call fails and reports what is there now, and
 * the caller computes again.
 *
 * @code
 * uint32_t seen = tb_platform_atomic_u32_load(&atomic);
 *
 * while (!tb_platform_atomic_u32_compare_exchange(&atomic, &seen, seen * 2))
 * {
 *     // `seen` now holds the value another thread wrote; try again with it.
 * }
 * @endcode
 *
 * @param atomic Required non-null.
 * @param[in,out] expected Required non-null. On entry, the value the caller
 * expects to find. On failure it receives the value found; on success it is
 * unchanged.
 * @param desired The value to write when `*expected` is found.
 * @return true when `desired` was written; false when the value was not
 * `*expected`, in which case nothing was written. It does not fail when the
 * value is as expected.
 * @par Threading Thread-safe; sequentially consistent. `expected` belongs to
 * the calling thread.
 */
TB_API bool tb_platform_atomic_u32_compare_exchange(tb_platform_atomic_u32 *atomic,
                                                    uint32_t *expected, uint32_t desired);

/**
 * @brief A pointer that several threads may read and write without a lock.
 *
 * Zero-initialize it, which makes it NULL. Once it is shared between threads,
 * read and write `value` only through the `tb_platform_atomic_pointer_*`
 * functions. The functions move the pointer and never touch what it points
 * to; who owns that, and when it may be released, is for the caller to
 * arrange.
 */
typedef struct tb_platform_atomic_pointer
{
    void *value; /**< Current value; private to the atomic functions. */
} tb_platform_atomic_pointer;

/**
 * @brief Read an atomic pointer.
 * @param atomic Required non-null.
 * @return The pointer at some instant during the call; may be NULL.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API void *tb_platform_atomic_pointer_load(const tb_platform_atomic_pointer *atomic);

/**
 * @brief Write an atomic pointer.
 * @param atomic Required non-null.
 * @param value The new pointer; may be NULL.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API void tb_platform_atomic_pointer_store(tb_platform_atomic_pointer *atomic, void *value);

/**
 * @brief Write an atomic pointer and learn what it replaced, as one indivisible step.
 * @param atomic Required non-null.
 * @param value The new pointer; may be NULL.
 * @return The pointer from just before the write; may be NULL.
 * @par Threading Thread-safe; sequentially consistent.
 */
TB_API void *tb_platform_atomic_pointer_exchange(tb_platform_atomic_pointer *atomic, void *value);

/**
 * @brief Write an atomic pointer only if it still holds what the caller last saw.
 *
 * As tb_platform_atomic_u32_compare_exchange(). Equal pointers do not prove
 * that nothing happened in between: an object can be released and another
 * allocated at the same address.
 *
 * @param atomic Required non-null.
 * @param[in,out] expected Required non-null. On entry, the pointer the caller
 * expects to find. On failure it receives the pointer found; on success it is
 * unchanged.
 * @param desired The pointer to write when `*expected` is found; may be NULL.
 * @return true when `desired` was written; false when the pointer was not
 * `*expected`, in which case nothing was written.
 * @par Threading Thread-safe; sequentially consistent. `expected` belongs to
 * the calling thread.
 */
TB_API bool tb_platform_atomic_pointer_compare_exchange(tb_platform_atomic_pointer *atomic,
                                                        void **expected, void *desired);

#endif
