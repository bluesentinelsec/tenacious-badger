/** @file thread.h
 * @brief Threads, mutexes, and condition variables. Implementations are
 * build-selected.
 *
 * A mutex and a condition variable are plain objects that the caller owns:
 * put one in a structure or make it a global, zero-initialize it, and use it.
 * They hold no system resource, so nothing has to destroy them, and a global
 * one is ready before any code runs.
 *
 * @code
 * typedef struct queue
 * {
 *     tb_mutex lock;         // Guards the fields below.
 *     tb_condition is_ready; // Woken when `count` rises above zero.
 *     size_t count;
 * } queue;
 *
 * tb_mutex_lock(&queue->lock);
 * while (queue->count == 0)
 * {
 *     tb_condition_wait(&queue->is_ready, &queue->lock);
 * }
 * queue->count--;
 * tb_mutex_unlock(&queue->lock);
 * @endcode
 *
 * **A variable for each thread** is declared with the C keyword
 * `_Thread_local`, which every supported compiler provides. It starts at zero
 * in each new thread.
 *
 * **Contracts.** What a function requires of its caller is checked in a build
 * with assertions (see badger/assert.h), on every platform: a null pointer,
 * unlocking a mutex that the calling thread does not hold, locking one that
 * it holds already, waiting on a condition without holding the mutex, and
 * joining a thread that is not there each fail an assertion before anything
 * is changed. A build without assertions does not check, and what the system
 * then does is undefined.
 */
#ifndef TB_THREAD_H
#define TB_THREAD_H

#include "badger/api.h"
#include "badger/thread_storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

/**
 * @brief The function a thread runs. The thread ends when it returns.
 * @param argument The pointer given to tb_thread_create(); may be NULL.
 */
typedef void tb_thread_function(void *argument);

/** @brief Bytes of a thread's name that the engine keeps, terminator included. */
#define TB_THREAD_NAME_CAPACITY 32

/** @brief How urgently a thread should be scheduled, beside the others. */
typedef enum tb_thread_priority
{
    /** What a thread has when nothing is asked: the system's default. */
    TB_THREAD_PRIORITY_NORMAL = 0,
    /** Work that may wait for everything else: loading, streaming, baking.
     * It still runs when a processor is free. */
    TB_THREAD_PRIORITY_BACKGROUND,
    /** Work that must not wait behind normal threads, such as the audio
     * mixer. It asks for more than most processes may have on Linux, where
     * the thread then stays at the normal priority. */
    TB_THREAD_PRIORITY_HIGH
} tb_thread_priority;

/** @brief A stack size that suits engine threads on every platform: 1 MiB.
 * The systems' own defaults range from 512 KiB to 8 MiB. */
#define TB_THREAD_STACK_SIZE_DEFAULT ((size_t)1 << 20)

/**
 * @brief What a new thread is given beyond its function.
 *
 * Zero-initialize it, which asks for nothing: no name, the system's default
 * stack, and the normal priority.
 *
 * @code
 * const tb_thread_options options = {.name = "badger-worker-3",
 *                                    .stack_size = TB_THREAD_STACK_SIZE_DEFAULT,
 *                                    .priority = TB_THREAD_PRIORITY_BACKGROUND};
 *
 * tb_thread_create_with_options(&thread, &options, run_worker, worker);
 * @endcode
 */
typedef struct tb_thread_options
{
    /** A name for debuggers, profilers, and crash reports to show: UTF-8,
     * NUL-terminated, or NULL for none. It is copied. The engine keeps the
     * first TB_THREAD_NAME_CAPACITY - 1 bytes and a system may show fewer,
     * 15 on Linux; a name is always cut between characters. */
    const char *name;
    /** Bytes of stack the thread can use, or 0 for the system's default.
     * The system rounds it up, to its page size and to its own minimum. */
    size_t stack_size;
    /** One of tb_thread_priority. */
    tb_thread_priority priority;
} tb_thread_options;

/**
 * @brief A thread that was created and has not been joined yet.
 *
 * Zero-initialize it. The fields are private to the thread functions. The
 * running thread reads this object, so it must stay at the same address from
 * tb_thread_create() until tb_thread_join() returns.
 */
typedef struct tb_thread
{
    tb_thread_function *entry;          /**< The function the thread runs; private. */
    void *argument;                     /**< Passed to `entry`; private. */
    uintptr_t native;                   /**< The system's handle; zero when there is no thread. */
    char name[TB_THREAD_NAME_CAPACITY]; /**< The name it gives itself; private. */
    tb_thread_priority priority;        /**< The priority it gives itself; private. */
} tb_thread;

/**
 * @brief Start a thread that runs `entry(argument)`.
 *
 * The thread has the system's default stack size and priority. It may begin
 * to run before this function returns.
 *
 * @param thread Required non-null; zero-initialized, or joined since it was
 * last created. Creating over a thread that is not joined is a contract
 * violation.
 * @param entry Function to run; required non-null.
 * @param argument Passed to `entry`; may be NULL. Whatever it points to must
 * outlive the thread's use of it.
 * @par Errors A system that cannot start another thread ends the process with
 * a diagnostic, as running out of memory does.
 * @par Ownership The caller owns the thread and must pass it to
 * tb_thread_join() exactly once.
 * @par Threading Thread-safe for distinct `thread` objects.
 */
TB_API void tb_thread_create(tb_thread *thread, tb_thread_function *entry, void *argument);

/**
 * @brief Start a thread with a name, a stack size, or a priority.
 *
 * As tb_thread_create(), which is this function with no options. The thread
 * gives itself its name and priority before it calls `entry`.
 *
 * A name or a priority that the system does not grant is not an error: the
 * thread runs without it. A stack size is always honored.
 *
 * @param thread Required non-null; zero-initialized, or joined since it was
 * last created.
 * @param options What to give the thread, or NULL for nothing. Borrowed for
 * the call; the name is copied.
 * @param entry Function to run; required non-null.
 * @param argument Passed to `entry`; may be NULL.
 * @par Errors A system that cannot start another thread, or cannot give it
 * the stack that was asked for, ends the process with a diagnostic. A
 * priority that is not one of tb_thread_priority is a contract violation.
 * @par Ownership The caller owns the thread and must pass it to
 * tb_thread_join() exactly once.
 * @par Threading Thread-safe for distinct `thread` objects.
 */
TB_API void tb_thread_create_with_options(tb_thread *thread, const tb_thread_options *options,
                                          tb_thread_function *entry, void *argument);

/**
 * @brief Wait for a thread to end, and release it.
 * @param thread Required non-null; created and not yet joined. It is zeroed,
 * and may be given to tb_thread_create() again.
 * @par Errors Joining a thread that was not created, or twice, is a contract
 * violation; a build without assertions ends the process with a diagnostic.
 * A thread that joins itself never returns, and that is not checked.
 * @par Threading Externally synchronized for one `thread` object; any thread
 * but the one being joined may call it.
 */
TB_API void tb_thread_join(tb_thread *thread);

/**
 * @brief Identify the calling thread.
 * @return A number that is not zero and that no other running thread has. It
 * stays the same for the life of the thread, and may be given to a later
 * thread once this one has ended.
 * @par Threading Thread-safe.
 */
TB_API uint64_t tb_thread_current_id(void);

/**
 * @brief Read the calling thread's name, as the system has it.
 *
 * This is what a debugger shows, so it is the name to put in a log or a
 * report. It may be shorter than the name the thread was created with, where
 * the system keeps fewer bytes. A thread that was given no name has whatever
 * the system gives it: nothing on Windows and macOS, the program's name on
 * Linux.
 *
 * @param[out] buffer Required non-null; receives the name, NUL-terminated
 * and cut between characters if it does not fit.
 * @param capacity Bytes in `buffer`; at least 1.
 * @return Bytes written, without the terminator; 0 when there is no name.
 * @par Threading Thread-safe.
 */
TB_API size_t tb_thread_current_name(char *buffer, size_t capacity);

/* -------------------------------------------------------------------------
 * Mutexes.
 * ------------------------------------------------------------------------- */

/**
 * @brief A lock that one thread holds at a time.
 *
 * Zero-initialize it; a zeroed mutex is unlocked and ready. It needs no
 * destruction. It must be unlocked, and must not be moved or copied, from the
 * moment a thread first locks it until no thread uses it any more.
 *
 * A build with assertions records in it which thread holds it, which is how
 * the contracts below are checked. The mutex is the same size in every build.
 */
typedef struct tb_mutex
{
    uintptr_t storage[TB_MUTEX_STORAGE_WORDS]; /**< The system's lock; private. */
} tb_mutex;

/**
 * @brief Take a mutex, waiting until no other thread holds it.
 * @param mutex Required non-null. The calling thread must not hold it
 * already: a mutex is not recursive. Locking it twice is a contract
 * violation, and never returns in a build without assertions.
 * @par Threading Thread-safe.
 */
TB_API void tb_mutex_lock(tb_mutex *mutex);

/**
 * @brief Take a mutex if no thread holds it, without waiting.
 * @param mutex Required non-null.
 * @return true when the calling thread now holds the mutex; false when it is
 * held, by another thread or by the caller, in which case nothing changed.
 * @par Threading Thread-safe.
 */
TB_API bool tb_mutex_try_lock(tb_mutex *mutex);

/**
 * @brief Give up a mutex.
 * @param mutex Required non-null; held by the calling thread. Unlocking a
 * mutex that nobody holds, or that another thread holds, is a contract
 * violation.
 * @par Threading Thread-safe.
 */
TB_API void tb_mutex_unlock(tb_mutex *mutex);

/* -------------------------------------------------------------------------
 * Condition variables.
 * ------------------------------------------------------------------------- */

/**
 * @brief A place where threads wait for something that a mutex guards.
 *
 * Zero-initialize it. It needs no destruction, and must not be moved or
 * copied while a thread waits on it. Every thread that waits on it at one
 * time must pass the same mutex.
 *
 * A wait may return when nobody woke it. Decide whether to go on by looking
 * at the guarded state, in a loop, as the example at the top of this file
 * does.
 */
typedef struct tb_condition
{
    uintptr_t storage[TB_CONDITION_STORAGE_WORDS]; /**< The system's object; private. */
} tb_condition;

/**
 * @brief Give up a mutex and wait to be woken, then take the mutex again.
 *
 * Giving up the mutex and beginning to wait is one step: a thread that takes
 * the mutex afterwards and wakes the condition cannot be missed.
 *
 * @param condition Required non-null.
 * @param mutex Required non-null; held by the calling thread. It is held
 * again when the function returns. Waiting without holding it is a contract
 * violation.
 * @par Threading Thread-safe.
 */
TB_API void tb_condition_wait(tb_condition *condition, tb_mutex *mutex);

/**
 * @brief Wait to be woken, for a limited time.
 *
 * As tb_condition_wait(), except that the wait ends by itself when the time
 * has passed on the clock of tb_platform_clock_nanoseconds(). To wait for a
 * condition until a deadline, call this in a loop with the time that is left.
 *
 * @param condition Required non-null.
 * @param mutex Required non-null; held by the calling thread, whatever the
 * limit. It is held again when the function returns.
 * @param nanoseconds Longest time to wait. Zero returns false at once.
 * @return true when the wait ended early, woken or not; false when at least
 * `nanoseconds` passed. The thread may not run again until some time after
 * the limit: the system decides when.
 * @par Threading Thread-safe.
 */
TB_API bool tb_condition_wait_for(tb_condition *condition, tb_mutex *mutex, uint64_t nanoseconds);

/**
 * @brief Wake one of the threads waiting on a condition, if any is.
 * @param condition Required non-null.
 * @par Threading Thread-safe; the caller may hold the mutex or not.
 */
TB_API void tb_condition_wake_one(tb_condition *condition);

/**
 * @brief Wake every thread waiting on a condition.
 * @param condition Required non-null.
 * @par Threading Thread-safe; the caller may hold the mutex or not.
 */
TB_API void tb_condition_wake_all(tb_condition *condition);

/* -------------------------------------------------------------------------
 * Semaphores.
 * ------------------------------------------------------------------------- */

/**
 * @brief A count of signals that threads wait for and take.
 *
 * A signal adds to the count, and a wait takes one from it, waiting while it
 * is zero. Unlike a condition, a semaphore remembers a signal that nobody was
 * waiting for: the next wait takes it at once. That makes it the thing to
 * hand work to a thread with, one unit for each signal.
 *
 * Zero-initialize it; a zeroed semaphore has no signals. It needs no
 * destruction, and must not be moved or copied while a thread uses it.
 */
typedef struct tb_semaphore
{
    tb_mutex lock;             /**< Guards the count; private. */
    tb_condition is_signalled; /**< Woken when the count rises; private. */
    size_t count;              /**< Signals not yet taken; private. */
} tb_semaphore;

/**
 * @brief Add signals to a semaphore, waking threads that wait for them.
 * @param semaphore Required non-null.
 * @param count How many signals to add; 0 adds none. A count that would take
 * the semaphore past what a `size_t` holds is a contract violation, and adds
 * nothing.
 * @par Threading Thread-safe.
 */
TB_API void tb_semaphore_signal(tb_semaphore *semaphore, size_t count);

/**
 * @brief Take one signal from a semaphore, waiting until there is one.
 * @param semaphore Required non-null.
 * @par Threading Thread-safe.
 */
TB_API void tb_semaphore_wait(tb_semaphore *semaphore);

/**
 * @brief Take one signal from a semaphore if there is one, without waiting.
 * @param semaphore Required non-null.
 * @return true when a signal was taken.
 * @par Threading Thread-safe.
 */
TB_API bool tb_semaphore_try_wait(tb_semaphore *semaphore);

/**
 * @brief Take one signal from a semaphore, waiting for a limited time.
 * @param semaphore Required non-null.
 * @param nanoseconds The longest to wait, by the clock of
 * tb_platform_clock_nanoseconds(). Any value is valid, and 0 does not wait.
 * @return true when a signal was taken; false when the time passed without
 * one. The thread may not run again until some time after the limit: the
 * system decides when.
 * @par Threading Thread-safe.
 */
TB_API bool tb_semaphore_wait_for(tb_semaphore *semaphore, uint64_t nanoseconds);

/* -------------------------------------------------------------------------
 * Values that each thread has for itself.
 * ------------------------------------------------------------------------- */

/**
 * @brief A place where every thread keeps a pointer of its own.
 *
 * `_Thread_local` does this for a variable that is known when the program is
 * compiled, and costs less. Use a tb_thread_local when the place is made at
 * run time, or when something must be done with a thread's value as the
 * thread ends, which `_Thread_local` cannot do.
 *
 * Zero-initialize it, then give it to tb_thread_local_create(). Every thread
 * starts with NULL in it.
 */
typedef struct tb_thread_local
{
    uintptr_t key; /**< The system's key, and zero for none; private. */
} tb_thread_local;

/**
 * @brief Receives the value of a thread that is ending.
 * @param value What the thread last stored; never NULL. It must not use the
 * tb_thread_local that the value was stored in.
 */
typedef void tb_thread_local_destructor(void *value);

/**
 * @brief Make a place for a value of each thread.
 *
 * @param local Required non-null; zero-initialized, or destroyed since it was
 * last created.
 * @param destructor Called on a thread, as it ends, with the value that the
 * thread stored, when that is not NULL; or NULL to do nothing. It runs for a
 * thread that tb_thread_create() started and whose function returned. Whether
 * it runs for any other thread, and for the thread on which the process
 * ends, is up to the system.
 * @par Errors A system gives a process a limited number of these, never
 * fewer than a hundred. Running out of them ends the process.
 * @par Threading Thread-safe for distinct `local` objects.
 */
TB_API void tb_thread_local_create(tb_thread_local *local, tb_thread_local_destructor *destructor);

/**
 * @brief Give a place for thread-local values back to the system.
 *
 * Destroy it when no thread has a value in it any more. What becomes of a
 * value that is still stored is up to the system: Windows calls the
 * destructor for it at once, on the calling thread, and the others never do.
 *
 * @param local Required non-null; created and not yet destroyed. It is
 * zeroed, and may be created again.
 * @par Threading Externally synchronized: no other thread may be using
 * `local`.
 */
TB_API void tb_thread_local_destroy(tb_thread_local *local);

/**
 * @brief Get the calling thread's value.
 * @param local Required non-null; created and not yet destroyed.
 * @return What this thread last stored, or NULL when it has stored nothing.
 * @par Threading Thread-safe.
 */
TB_API void *tb_thread_local_get(const tb_thread_local *local);

/**
 * @brief Set the calling thread's value.
 * @param local Required non-null; created and not yet destroyed.
 * @param value The value; NULL for none. The caller keeps ownership until
 * the thread ends, when the destructor is given it.
 * @par Threading Thread-safe.
 */
TB_API void tb_thread_local_set(const tb_thread_local *local, void *value);

/* -------------------------------------------------------------------------
 * Waiting without sleeping.
 * ------------------------------------------------------------------------- */

/**
 * @brief Tell the processor that the calling thread is waiting in a loop.
 *
 * Call it once each time round a loop that waits for another thread to change
 * something. The processor then spends less power on the loop, gives more of
 * a shared core to the thread beside it, and leaves the loop faster when what
 * it waits for arrives. It takes a few nanoseconds to some tens, and never
 * gives up the thread's turn.
 *
 * @par Threading Thread-safe.
 */
TB_API void tb_thread_relax(void);

/**
 * @brief Give up the rest of the calling thread's turn to another thread
 * that is ready to run, if there is one.
 *
 * For a loop that has waited longer than a tb_thread_relax() is worth. It is
 * no way to wait for a length of time: with nothing else ready, it returns at
 * once.
 *
 * @par Threading Thread-safe.
 */
TB_API void tb_thread_yield(void);

#endif
