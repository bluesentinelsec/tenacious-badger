/* POSIX threads and sched_yield; request the POSIX API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "badger/thread.h"

#include "badger/assert.h"
#include "badger/platform.h"
#include "condition_native.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

/* A tb_mutex and a tb_condition are ready when they are all zeros, but the
 * system's objects are not: they have to be made by a function. The first
 * word of each therefore records how far the making has got, and the
 * system's object lies in the words after it. */
#define TB_POSIX_UNSTARTED ((uintptr_t)0)
#define TB_POSIX_STARTING ((uintptr_t)1)
#define TB_POSIX_READY ((uintptr_t)2)

#define TB_POSIX_NATIVE_BYTES(words) (sizeof(uintptr_t) * ((size_t)(words) - 1))

TB_STATIC_ASSERT(sizeof(pthread_mutex_t) <= TB_POSIX_NATIVE_BYTES(TB_MUTEX_STORAGE_WORDS),
                 "a tb_mutex has room for a pthread_mutex_t");
TB_STATIC_ASSERT(_Alignof(pthread_mutex_t) <= _Alignof(uintptr_t),
                 "a tb_mutex is aligned for a pthread_mutex_t");
TB_STATIC_ASSERT(sizeof(pthread_cond_t) <= TB_POSIX_NATIVE_BYTES(TB_CONDITION_STORAGE_WORDS),
                 "a tb_condition has room for a pthread_cond_t");
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
    uintptr_t *const state = &mutex->storage[0];
    pthread_mutex_t *const native = (pthread_mutex_t *)&mutex->storage[1];

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
    uintptr_t *const state = &condition->storage[0];
    pthread_cond_t *const native = (pthread_cond_t *)&condition->storage[1];

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
 * Threads.
 * ------------------------------------------------------------------------- */

static void *tb_posix_thread_entry(void *parameter)
{
    const tb_thread *const thread = parameter;

    thread->entry(thread->argument);
    return NULL;
}

void tb_thread_create(tb_thread *thread, tb_thread_function *entry, void *argument)
{
    pthread_t native = {0};

    thread->entry = entry;
    thread->argument = argument;
    if (pthread_create(&native, NULL, tb_posix_thread_entry, thread) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_create on posix: pthread_create "
                             "failed\n");
    }
    thread->native = (uintptr_t)native;
}

void tb_thread_join(tb_thread *thread)
{
    if (thread->native == 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_join on posix: the thread was not "
                             "created, or was joined already\n");
    }
    if (pthread_join((pthread_t)thread->native, NULL) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_thread_join on posix: pthread_join failed\n");
    }
    thread->native = 0;
}

uint64_t tb_thread_current_id(void)
{
    /* The address of the thread's own record, or a number that stands for
     * it: never zero, and no two running threads share one. */
    return (uint64_t)(uintptr_t)pthread_self();
}

/* ---------------------------------------------------------------------------
 * Mutexes.
 * ------------------------------------------------------------------------- */

void tb_mutex_lock(tb_mutex *mutex)
{
    if (pthread_mutex_lock(tb_posix_mutex(mutex)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_mutex_lock on posix: pthread_mutex_lock "
                             "failed\n");
    }
}

bool tb_mutex_try_lock(tb_mutex *mutex)
{
    const int error = pthread_mutex_trylock(tb_posix_mutex(mutex));

    if (error == EBUSY)
    {
        return false;
    }
    if (error != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_mutex_try_lock on posix: pthread_mutex_trylock "
                             "failed\n");
    }
    return true;
}

void tb_mutex_unlock(tb_mutex *mutex)
{
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
    if (pthread_cond_wait(tb_posix_condition(condition), tb_posix_mutex(mutex)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_condition_wait on posix: pthread_cond_wait "
                             "failed\n");
    }
}

bool tb_condition_wait_for(tb_condition *condition, tb_mutex *mutex, uint64_t nanoseconds)
{
    pthread_cond_t *const native_condition = tb_posix_condition(condition);
    pthread_mutex_t *const native_mutex = tb_posix_mutex(mutex);
    const uint64_t start = tb_platform_clock_nanoseconds();
    uint64_t elapsed = 0;

    /* The system may count the limit on another clock than the engine's, and
     * waits for a very long one in parts. The engine's clock decides when the
     * limit has passed. */
    while (elapsed < nanoseconds)
    {
        if (tb_posix_condition_wait_up_to(native_condition, native_mutex, nanoseconds - elapsed))
        {
            return true;
        }
        elapsed = tb_platform_clock_nanoseconds() - start;
    }
    return false;
}

void tb_condition_wake_one(tb_condition *condition)
{
    if (pthread_cond_signal(tb_posix_condition(condition)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_condition_wake_one on posix: "
                             "pthread_cond_signal failed\n");
    }
}

void tb_condition_wake_all(tb_condition *condition)
{
    if (pthread_cond_broadcast(tb_posix_condition(condition)) != 0)
    {
        tb_posix_thread_fail("platform.thread: tb_condition_wake_all on posix: "
                             "pthread_cond_broadcast failed\n");
    }
}
