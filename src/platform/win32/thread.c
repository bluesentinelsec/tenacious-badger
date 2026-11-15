#include "badger/thread.h"

#include "badger/assert.h"
#include "badger/platform.h"
#include "thread_wait.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>

#define TB_WIN32_NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)

/* A slim reader/writer lock and a condition variable are each one pointer.
 * Zero is their initial state, and the system keeps nothing else for them. */
TB_STATIC_ASSERT(sizeof(tb_mutex) == sizeof(SRWLOCK), "a tb_mutex holds exactly an SRWLOCK");
TB_STATIC_ASSERT(_Alignof(tb_mutex) >= _Alignof(SRWLOCK), "a tb_mutex is aligned for an SRWLOCK");
TB_STATIC_ASSERT(sizeof(tb_condition) == sizeof(CONDITION_VARIABLE),
                 "a tb_condition holds exactly a CONDITION_VARIABLE");
TB_STATIC_ASSERT(_Alignof(tb_condition) >= _Alignof(CONDITION_VARIABLE),
                 "a tb_condition is aligned for a CONDITION_VARIABLE");
TB_STATIC_ASSERT(sizeof(uintptr_t) == sizeof(HANDLE), "a thread handle fits in tb_thread");
TB_STATIC_ASSERT(TB_WIN32_WAIT_MILLISECONDS_MAX < INFINITE,
                 "the longest limited wait is not the unlimited one");

static _Noreturn void tb_win32_thread_fail(const char *diagnostic)
{
    fputs(diagnostic, stderr);
    tb_platform_abort();
}

static SRWLOCK *tb_win32_lock(tb_mutex *mutex)
{
    return (SRWLOCK *)mutex->storage;
}

static CONDITION_VARIABLE *tb_win32_condition(tb_condition *condition)
{
    return (CONDITION_VARIABLE *)condition->storage;
}

/* ---------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

static DWORD WINAPI tb_win32_thread_entry(LPVOID parameter)
{
    const tb_thread *const thread = parameter;

    thread->entry(thread->argument);
    return 0;
}

void tb_thread_create(tb_thread *thread, tb_thread_function *entry, void *argument)
{
    HANDLE handle = NULL;

    thread->entry = entry;
    thread->argument = argument;
    handle = CreateThread(NULL, 0, tb_win32_thread_entry, thread, 0, NULL);
    if (handle == NULL)
    {
        tb_win32_thread_fail("platform.thread: tb_thread_create on win32: CreateThread failed\n");
    }
    thread->native = (uintptr_t)handle;
}

void tb_thread_join(tb_thread *thread)
{
    const HANDLE handle = (HANDLE)thread->native;

    if (handle == NULL)
    {
        tb_win32_thread_fail("platform.thread: tb_thread_join on win32: the thread was not "
                             "created, or was joined already\n");
    }
    if (WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0)
    {
        tb_win32_thread_fail("platform.thread: tb_thread_join on win32: WaitForSingleObject "
                             "failed\n");
    }
    if (!CloseHandle(handle))
    {
        tb_win32_thread_fail("platform.thread: tb_thread_join on win32: CloseHandle failed\n");
    }
    thread->native = 0;
}

uint64_t tb_thread_current_id(void)
{
    /* Never zero: the system gives that identifier to no thread. */
    return GetCurrentThreadId();
}

/* ---------------------------------------------------------------------------
 * Mutexes.
 * ------------------------------------------------------------------------- */

void tb_mutex_lock(tb_mutex *mutex)
{
    AcquireSRWLockExclusive(tb_win32_lock(mutex));
}

bool tb_mutex_try_lock(tb_mutex *mutex)
{
    return TryAcquireSRWLockExclusive(tb_win32_lock(mutex)) != 0;
}

void tb_mutex_unlock(tb_mutex *mutex)
{
    ReleaseSRWLockExclusive(tb_win32_lock(mutex));
}

/* ---------------------------------------------------------------------------
 * Condition variables.
 * ------------------------------------------------------------------------- */

uint32_t tb_win32_wait_milliseconds(uint64_t nanoseconds)
{
    const uint64_t whole = nanoseconds / TB_WIN32_NANOSECONDS_PER_MILLISECOND;
    const uint64_t rest = nanoseconds % TB_WIN32_NANOSECONDS_PER_MILLISECOND;
    uint64_t milliseconds = whole;

    /* Part of a millisecond is waited for as a whole one. */
    if (rest != 0)
    {
        milliseconds = whole + 1;
    }
    if (milliseconds > TB_WIN32_WAIT_MILLISECONDS_MAX)
    {
        return TB_WIN32_WAIT_MILLISECONDS_MAX;
    }
    return (uint32_t)milliseconds;
}

void tb_condition_wait(tb_condition *condition, tb_mutex *mutex)
{
    if (!SleepConditionVariableSRW(tb_win32_condition(condition), tb_win32_lock(mutex), INFINITE,
                                   0))
    {
        tb_win32_thread_fail("platform.thread: tb_condition_wait on win32: "
                             "SleepConditionVariableSRW failed\n");
    }
}

bool tb_condition_wait_for(tb_condition *condition, tb_mutex *mutex, uint64_t nanoseconds)
{
    const uint64_t start = tb_platform_clock_nanoseconds();
    uint64_t elapsed = 0;

    /* The system counts a wait in ticks of its timer and sometimes ends one
     * a fraction of a millisecond early, and a limit beyond 49 days is waited
     * for in parts. The clock decides when the limit has passed. */
    while (elapsed < nanoseconds)
    {
        const DWORD milliseconds = tb_win32_wait_milliseconds(nanoseconds - elapsed);

        if (SleepConditionVariableSRW(tb_win32_condition(condition), tb_win32_lock(mutex),
                                      milliseconds, 0))
        {
            return true;
        }
        if (GetLastError() != ERROR_TIMEOUT)
        {
            tb_win32_thread_fail("platform.thread: tb_condition_wait_for on win32: "
                                 "SleepConditionVariableSRW failed\n");
        }
        elapsed = tb_platform_clock_nanoseconds() - start;
    }
    return false;
}

void tb_condition_wake_one(tb_condition *condition)
{
    WakeConditionVariable(tb_win32_condition(condition));
}

void tb_condition_wake_all(tb_condition *condition)
{
    WakeAllConditionVariable(tb_win32_condition(condition));
}
