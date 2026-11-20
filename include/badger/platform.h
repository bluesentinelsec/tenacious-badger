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
 * @brief Stop the calling thread for a length of time.
 *
 * The thread uses no processor while it sleeps. It sleeps for at least the
 * time asked, by the clock of tb_platform_clock_nanoseconds(), and wakes as
 * soon after that as the system gets round to it: typically within a
 * millisecond on a machine that is not busy, and after any length of time on
 * one that is. To wait for a moment more exactly than that, use
 * tb_platform_wait_until().
 *
 * A sleep cannot be ended early. A thread that may have to be woken waits on
 * a condition or a semaphore with a time limit instead.
 *
 * @param nanoseconds How long to sleep at the least; 0 returns at once. Any
 * value is valid.
 * @par Errors A native timer that cannot be made or set ends the process
 * with a diagnostic.
 * @par Threading Thread-safe.
 */
TB_API void tb_platform_sleep(uint64_t nanoseconds);

/**
 * @brief Wait until the clock reaches a reading, as exactly as the clock is.
 *
 * This is what holds a frame loop to a rate: add the length of a frame to a
 * deadline each frame, and wait for it.
 *
 * @code
 * uint64_t deadline = tb_platform_clock_nanoseconds();
 *
 * for (;;)
 * {
 *     run_a_frame();
 *     deadline += frame_nanoseconds;
 *     tb_platform_wait_until(deadline);
 * }
 * @endcode
 *
 * Because each deadline is worked out from the one before and not from the
 * time now, a frame that wakes late does not make the next one late.
 *
 * The thread sleeps for most of the wait. For the last stretch of it, about
 * a millisecond, it runs and watches the clock, because a sleep cannot be
 * trusted to end in time: the wait is exact at the price of a processor for
 * that stretch.
 *
 * @param deadline A reading of tb_platform_clock_nanoseconds(). One that has
 * passed returns at once.
 * @par Errors As tb_platform_sleep().
 * @par Threading Thread-safe.
 */
TB_API void tb_platform_wait_until(uint64_t deadline);

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
 * @brief Write bytes to the standard error stream with the system's own
 * call, taking no lock of the C library's and allocating nothing.
 *
 * For reports written by a process that may be failing, where the C
 * library's stream may be locked by the thread that failed. The standard
 * error stream is unbuffered, so what was written to it before stays in
 * order.
 *
 * @param bytes The bytes; NULL only at length zero.
 * @param length How many.
 * @par Errors A stream that refuses them loses them: nothing is reported.
 * @par Threading Thread-safe; writes from two threads at once may interleave.
 */
TB_API void tb_platform_write_error(const void *bytes, size_t length);

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
