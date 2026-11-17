/* POSIX threads and sched_yield; request the POSIX API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "badger/thread.h"

#include "../shared/name.h"
#include "badger/assert.h"
#include "badger/platform.h"
#include "condition_native.h"
#include "thread_native.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

/* A tb_mutex and a tb_condition are ready when they are all zeros, but the
 * system's objects are not: they have to be made by a function. The first
 * word of each therefore records how far the making has got, and the
 * system's object lies in the words after it. The last word of a tb_mutex is
 * for the thread that holds it. */
#define TB_POSIX_UNSTARTED ((uintptr_t)0)
#define TB_POSIX_STARTING ((uintptr_t)1)
#define TB_POSIX_READY ((uintptr_t)2)

#define TB_POSIX_STATE_WORD 0
#define TB_POSIX_NATIVE_WORD 1
#define TB_POSIX_MUTEX_OWNER_WORD (TB_MUTEX_STORAGE_WORDS - 1)

/* Bytes for the system's object in storage of `words` words, of which
 * `other_words` hold something else. */
#define TB_POSIX_NATIVE_BYTES(words, other_words)                                                  \
    (sizeof(uintptr_t) * ((size_t)(words) - (size_t)(other_words)))

TB_STATIC_ASSERT(sizeof(pthread_mutex_t) <= TB_POSIX_NATIVE_BYTES(TB_MUTEX_STORAGE_WORDS, 2),
                 "a tb_mutex has room for a pthread_mutex_t, its state, and its holder");
TB_STATIC_ASSERT(_Alignof(pthread_mutex_t) <= _Alignof(uintptr_t),
                 "a tb_mutex is aligned for a pthread_mutex_t");
TB_STATIC_ASSERT(sizeof(pthread_cond_t) <= TB_POSIX_NATIVE_BYTES(TB_CONDITION_STORAGE_WORDS, 1),
                 "a tb_condition has room for a pthread_cond_t and its state");
TB_STATIC_ASSERT(_Alignof(pthread_cond_t) <= _Alignof(uintptr_t),
                 "a tb_condition is aligned for a pthread_cond_t");
TB_STATIC_ASSERT(sizeof(pthread_t) <= sizeof(uintptr_t), "a pthread_t fits in tb_thread");

static _Noreturn void tb_posix_thread_fail(const char *diagnostic)
{
    fputs(diagnostic, stderr);
    tb_platform_abort();
}

/* ---------------------------------------------------------------------------
 * Making the system's object behind a zeroed one, once.
 * ------------------------------------------------------------------------- */

static bool tb_posix_is_ready(uintptr_t *state)
{
    return __atomic_load_n(state, __ATOMIC_SEQ_CST) == TB_POSIX_READY;
}

/* Returns true to the one thread that is to make the object, which then
 * calls tb_posix_finish_start(). Every other thread returns false once the
 * object is ready. The wait is a loop because it lasts a few instructions of
 * another thread, once in the life of the object. */
static bool tb_posix_begin_start(uintptr_t *state)
{
    uintptr_t expected = TB_POSIX_UNSTARTED;

    if (__atomic_compare_exchange_n(state, &expected, TB_POSIX_STARTING, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
    {
        return true;
    }
    while (!tb_posix_is_ready(state))
    {
        (void)sched_yield();
    }
    return false;
}

static void tb_posix_finish_start(uintptr_t *state)
{
    __atomic_store_n(state, TB_POSIX_READY, __ATOMIC_SEQ_CST);
}

static pthread_mutex_t *tb_posix_mutex(tb_mutex *mutex)
{
    uintptr_t *const state = &mutex->storage[TB_POSIX_STATE_WORD];
    pthread_mutex_t *const native = (pthread_mutex_t *)&mutex->storage[TB_POSIX_NATIVE_WORD];

    if (tb_posix_is_ready(state))
    {
        return native;
    }
    if (tb_posix_begin_start(state))
    {
        if (pthread_mutex_init(native, NULL) != 0)
        {
            tb_posix_thread_fail("platform.thread: a mutex on posix: pthread_mutex_init failed\n");
        }
        tb_posix_finish_start(state);
    }
    return native;
}

static pthread_cond_t *tb_posix_condition(tb_condition *condition)
{
    uintptr_t *const state = &condition->storage[TB_POSIX_STATE_WORD];
    pthread_cond_t *const native = (pthread_cond_t *)&condition->storage[TB_POSIX_NATIVE_WORD];

    if (tb_posix_is_ready(state))
    {
        return native;
    }
    if (tb_posix_begin_start(state))
    {
        tb_posix_condition_start(native);
        tb_posix_finish_start(state);
    }
    return native;
}

/* ---------------------------------------------------------------------------
 * Who holds a mutex.
 *
 * What the system does with a mutex that is unlocked by the wrong thread,
 * locked twice, or waited on without being held is undefined, and differs
 * between Linux and macOS. A build with assertions therefore records the
 * holder, the thread's identifier or zero, and fails an assertion instead.
 * A build without assertions keeps the word and never touches it, so that a
 * tb_mutex is the same size in both.
 *
 * A thread reads the word of a mutex it does not hold, to see that it is not
 * the holder, so the word is read and written as one step.
 * ------------------------------------------------------------------------- */

static uintptr_t tb_posix_calling_thread(void)
{
    return (uintptr_t)pthread_self();
}

static bool tb_posix_caller_holds(tb_mutex *mutex)
{
    const uintptr_t owner =
        __atomic_load_n(&mutex->storage[TB_POSIX_MUTEX_OWNER_WORD], __ATOMIC_SEQ_CST);

    return owner == tb_posix_calling_thread();
}

/* Records that the calling thread now holds the mutex. Call it only while
 * that is so. */
static void tb_posix_note_held(tb_mutex *mutex)
{
#if TB_ASSERT_ENABLED
    __atomic_store_n(&mutex->storage[TB_POSIX_MUTEX_OWNER_WORD], tb_posix_calling_thread(),
                     __ATOMIC_SEQ_CST);
#else
    (void)mutex;
#endif
}

/* Records that nobody holds the mutex. Call it while the calling thread
 * still does, just before it gives the mutex up. */
static void tb_posix_note_released(tb_mutex *mutex)
{
#if TB_ASSERT_ENABLED
    __atomic_store_n(&mutex->storage[TB_POSIX_MUTEX_OWNER_WORD], (uintptr_t)0, __ATOMIC_SEQ_CST);
#else
    (void)mutex;
#endif
}

/* ---------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

/* Used where the system does not say how small a stack or how large a page
 * may be. */
#define TB_POSIX_STACK_SIZE_FLOOR ((size_t)16384)
#define TB_POSIX_PAGE_SIZE_FALLBACK ((size_t)4096)

static bool tb_posix_is_priority(tb_thread_priority priority)
{
    return priority == TB_THREAD_PRIORITY_NORMAL || priority == TB_THREAD_PRIORITY_BACKGROUND ||
           priority == TB_THREAD_PRIORITY_HIGH;
}

/* A value of the system's configuration as a size, or `fallback` when the
 * system has none. */
static size_t tb_posix_configured_size(int name, size_t fallback)
{
    const long value = sysconf(name);

    if (value <= 0)
    {
        return fallback;
    }
    return (size_t)value;
}

/* The stack size to ask the system for: at least `requested`, at least the
 * system's minimum, and a whole number of pages, which macOS insists on. */
static size_t tb_posix_stack_size(size_t requested)
{
    const size_t page = tb_posix_configured_size(_SC_PAGESIZE, TB_POSIX_PAGE_SIZE_FALLBACK);
    const size_t minimum =
        tb_posix_configured_size(_SC_THREAD_STACK_MIN, TB_POSIX_STACK_SIZE_FLOOR);
    size_t size = requested;
    size_t rest = 0;

    if (size < minimum)
    {
        size = minimum;
    }
    rest = size % page;
    if (rest == 0)
    {
        return size;
    }
    if (size > SIZE_MAX - (page - rest))
    {
        tb_posix_thread_fail("platform.thread: tb_thread_create on posix: the stack size is "
                             "larger than any address space\n");
    }
    return size + (page - rest);
}

static void *tb_posix_thread_entry(void *parameter)
{
    const tb_thread *const thread = parameter;

    if (thread->name[0] != '\0')
    {
        tb_posix_thread_apply_name(thread->name);
    }
    if (thread->priority != TB_THREAD_PRIORITY_NORMAL)
    {
        tb_posix_thread_apply_priority(thread->priority);
    }
    thread->entry(thread->argument);
    return NULL;
}

void tb_thread_create(tb_thread *thread, tb_thread_function *entry, void *argument)
{
    tb_thread_create_with_options(thread, NULL, entry, argument);
}

void tb_thread_create_with_options(tb_thread *thread, const tb_thread_options *options,
                                   tb_thread_function *entry, void *argument)
{
    pthread_attr_t attributes = {0};
    pthread_t native = {0};
    int error = 0;

    TB_ASSERT(thread != NULL);
    TB_ASSERT(entry != NULL);
    TB_ASSERT_MSG(thread->native == 0, "the thread object holds no thread that is not joined");
    TB_ASSERT_MSG(options == NULL || tb_posix_is_priority(options->priority),
                  "the priority is one of tb_thread_priority");
    thread->entry = entry;
    thread->argument = argument;
    thread->name[0] = '\0';
    thread->priority = TB_THREAD_PRIORITY_NORMAL;
    if (pthread_attr_init(&attributes) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_create on posix: pthread_attr_init "
                             "failed\n");
    }
    if (options != NULL)
    {
        (void)tb_platform_copy_name(thread->name, sizeof(thread->name), options->name);
        thread->priority = options->priority;
    }
    if (options != NULL && options->stack_size != 0)
    {
        if (pthread_attr_setstacksize(&attributes, tb_posix_stack_size(options->stack_size)) != 0)
        {
            tb_posix_thread_fail("platform.thread: tb_thread_create on posix: the system "
                                 "refused the stack size\n");
        }
    }
    error = pthread_create(&native, &attributes, tb_posix_thread_entry, thread);
    (void)pthread_attr_destroy(&attributes);
    if (error != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_create on posix: pthread_create "
                             "failed\n");
    }
    thread->native = (uintptr_t)native;
}

void tb_thread_join(tb_thread *thread)
{
    const tb_thread joined = {0};

    TB_ASSERT(thread != NULL);
    TB_ASSERT_MSG(thread->native != 0, "the thread was created and is not joined yet");
    /* A build without assertions still refuses, with a diagnostic. */
    if (thread->native == 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_join on posix: the thread was not "
                             "created, or was joined already\n");
    }
    if (pthread_join((pthread_t)thread->native, NULL) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_join on posix: pthread_join failed\n");
    }
    *thread = joined;
}

uint64_t tb_thread_current_id(void)
{
    /* The address of the thread's own record, or a number that stands for
     * it: never zero, and no two running threads share one. */
    return (uint64_t)(uintptr_t)pthread_self();
}

size_t tb_thread_current_name(char *buffer, size_t capacity)
{
    TB_ASSERT(buffer != NULL);
    TB_ASSERT(capacity >= 1);
    return tb_posix_thread_read_name(buffer, capacity);
}

/* ---------------------------------------------------------------------------
 * Mutexes.
 * ------------------------------------------------------------------------- */

void tb_mutex_lock(tb_mutex *mutex)
{
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(!tb_posix_caller_holds(mutex),
                  "the calling thread does not hold the mutex already");
    if (pthread_mutex_lock(tb_posix_mutex(mutex)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_mutex_lock on posix: pthread_mutex_lock "
                             "failed\n");
    }
    tb_posix_note_held(mutex);
}

bool tb_mutex_try_lock(tb_mutex *mutex)
{
    int error = 0;

    TB_ASSERT(mutex != NULL);
    error = pthread_mutex_trylock(tb_posix_mutex(mutex));
    if (error == EBUSY)
    {
        return false;
    }
    if (error != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_mutex_try_lock on posix: pthread_mutex_trylock "
                             "failed\n");
    }
    tb_posix_note_held(mutex);
    return true;
}

void tb_mutex_unlock(tb_mutex *mutex)
{
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(tb_posix_caller_holds(mutex), "the calling thread holds the mutex");
    /* Cleared before the lock is given up: afterwards the word belongs to
     * the next holder. */
    tb_posix_note_released(mutex);
    if (pthread_mutex_unlock(tb_posix_mutex(mutex)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_mutex_unlock on posix: pthread_mutex_unlock "
                             "failed\n");
    }
}

/* ---------------------------------------------------------------------------
 * Condition variables.
 * ------------------------------------------------------------------------- */

void tb_condition_wait(tb_condition *condition, tb_mutex *mutex)
{
    TB_ASSERT(condition != NULL);
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(tb_posix_caller_holds(mutex), "the calling thread holds the mutex");
    /* The wait gives the mutex up and takes it again. */
    tb_posix_note_released(mutex);
    if (pthread_cond_wait(tb_posix_condition(condition), tb_posix_mutex(mutex)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_condition_wait on posix: pthread_cond_wait "
                             "failed\n");
    }
    tb_posix_note_held(mutex);
}

bool tb_condition_wait_for(tb_condition *condition, tb_mutex *mutex, uint64_t nanoseconds)
{
    pthread_cond_t *native_condition = NULL;
    pthread_mutex_t *native_mutex = NULL;
    uint64_t start = 0;
    uint64_t elapsed = 0;
    bool ended_early = false;

    TB_ASSERT(condition != NULL);
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(tb_posix_caller_holds(mutex), "the calling thread holds the mutex");
    native_condition = tb_posix_condition(condition);
    native_mutex = tb_posix_mutex(mutex);
    start = tb_platform_clock_nanoseconds();
    /* Each wait gives the mutex up and takes it again. */
    tb_posix_note_released(mutex);
    /* The system may count the limit on another clock than the engine's, and
     * waits for a very long one in parts. The engine's clock decides when the
     * limit has passed. */
    while (elapsed < nanoseconds && !ended_early)
    {
        if (tb_posix_condition_wait_up_to(native_condition, native_mutex, nanoseconds - elapsed))
        {
            ended_early = true;
        }
        else
        {
            elapsed = tb_platform_clock_nanoseconds() - start;
        }
    }
    tb_posix_note_held(mutex);
    return ended_early;
}

void tb_condition_wake_one(tb_condition *condition)
{
    TB_ASSERT(condition != NULL);
    if (pthread_cond_signal(tb_posix_condition(condition)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_condition_wake_one on posix: "
                             "pthread_cond_signal failed\n");
    }
}

void tb_condition_wake_all(tb_condition *condition)
{
    TB_ASSERT(condition != NULL);
    if (pthread_cond_broadcast(tb_posix_condition(condition)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_condition_wake_all on posix: "
                             "pthread_cond_broadcast failed\n");
    }
}
