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
 * Zero is their initial state, and the system keeps nothing else for them.
 * A tb_mutex has a second word, for the thread that holds it. */
#define TB_WIN32_MUTEX_LOCK_WORD 0
#define TB_WIN32_MUTEX_OWNER_WORD 1

TB_STATIC_ASSERT(TB_MUTEX_STORAGE_WORDS == 2, "a tb_mutex is a lock and the thread that holds it");
TB_STATIC_ASSERT(sizeof(SRWLOCK) == sizeof(uintptr_t), "an SRWLOCK fills one word of a tb_mutex");
TB_STATIC_ASSERT(_Alignof(tb_mutex) >= _Alignof(SRWLOCK), "a tb_mutex is aligned for an SRWLOCK");
TB_STATIC_ASSERT(sizeof(PVOID) == sizeof(uintptr_t), "a thread identifier is kept as a pointer");
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
    return (SRWLOCK *)&mutex->storage[TB_WIN32_MUTEX_LOCK_WORD];
}

/* ---------------------------------------------------------------------------
 * Who holds a mutex.
 *
 * The system does not say which thread holds a slim lock, so a build with
 * assertions records it: the identifier of the holder, or zero. That is what
 * lets a mutex that is unlocked by the wrong thread, locked twice, or waited
 * on without being held fail an assertion, where the system would corrupt
 * the lock or never return. A build without assertions keeps the word and
 * never touches it, so that a tb_mutex is the same size in both.
 *
 * A thread reads the word of a mutex it does not hold, to see that it is not
 * the holder, so the word is read and written as one step.
 * ------------------------------------------------------------------------- */

static uintptr_t tb_win32_calling_thread(void)
{
    return (uintptr_t)GetCurrentThreadId();
}

static PVOID *tb_win32_owner_word(tb_mutex *mutex)
{
    return (PVOID *)&mutex->storage[TB_WIN32_MUTEX_OWNER_WORD];
}

static bool tb_win32_caller_holds(tb_mutex *mutex)
{
    const PVOID owner = InterlockedCompareExchangePointer(tb_win32_owner_word(mutex), NULL, NULL);

    return (uintptr_t)owner == tb_win32_calling_thread();
}

/* Records that the calling thread now holds the mutex. Call it only while
 * that is so. */
static void tb_win32_note_held(tb_mutex *mutex)
{
#if TB_ASSERT_ENABLED
    (void)InterlockedExchangePointer(tb_win32_owner_word(mutex), (PVOID)tb_win32_calling_thread());
#else
    (void)mutex;
#endif
}

/* Records that nobody holds the mutex. Call it while the calling thread
 * still does, just before it gives the mutex up. */
static void tb_win32_note_released(tb_mutex *mutex)
{
#if TB_ASSERT_ENABLED
    (void)InterlockedExchangePointer(tb_win32_owner_word(mutex), NULL);
#else
    (void)mutex;
#endif
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

    TB_ASSERT(thread != NULL);
    TB_ASSERT(entry != NULL);
    TB_ASSERT_MSG(thread->native == 0, "the thread object holds no thread that is not joined");
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
    HANDLE handle = NULL;

    TB_ASSERT(thread != NULL);
    TB_ASSERT_MSG(thread->native != 0, "the thread was created and is not joined yet");
    handle = (HANDLE)thread->native;
    /* A build without assertions still refuses, with a diagnostic. */
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
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(!tb_win32_caller_holds(mutex),
                  "the calling thread does not hold the mutex already");
    AcquireSRWLockExclusive(tb_win32_lock(mutex));
    tb_win32_note_held(mutex);
}

bool tb_mutex_try_lock(tb_mutex *mutex)
{
    TB_ASSERT(mutex != NULL);
    if (TryAcquireSRWLockExclusive(tb_win32_lock(mutex)) == 0)
    {
        return false;
    }
    tb_win32_note_held(mutex);
    return true;
}

void tb_mutex_unlock(tb_mutex *mutex)
{
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(tb_win32_caller_holds(mutex), "the calling thread holds the mutex");
    /* Cleared before the lock is given up: afterwards the word belongs to
     * the next holder. */
    tb_win32_note_released(mutex);
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
    TB_ASSERT(condition != NULL);
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(tb_win32_caller_holds(mutex), "the calling thread holds the mutex");
    if (!SleepConditionVariableSRW(tb_win32_condition(condition), tb_win32_lock(mutex), INFINITE,
                                   0))
    {
        tb_win32_thread_fail("platform.thread: tb_condition_wait on win32: "
                             "SleepConditionVariableSRW failed\n");
    }
    /* The wait gave the mutex up, and whoever took it meanwhile recorded
     * itself. The record is not cleared before the wait: until the mutex is
     * taken again it names this thread, which is asleep and cannot be misled
     * by it. */
    tb_win32_note_held(mutex);
}

bool tb_condition_wait_for(tb_condition *condition, tb_mutex *mutex, uint64_t nanoseconds)
{
    uint64_t start = 0;
    uint64_t elapsed = 0;
    bool ended_early = false;

    TB_ASSERT(condition != NULL);
    TB_ASSERT(mutex != NULL);
    TB_ASSERT_MSG(tb_win32_caller_holds(mutex), "the calling thread holds the mutex");
    start = tb_platform_clock_nanoseconds();
    /* The system counts a wait in ticks of its timer and sometimes ends one
     * a fraction of a millisecond early, and a limit beyond 49 days is waited
     * for in parts. The clock decides when the limit has passed. */
    while (elapsed < nanoseconds && !ended_early)
    {
        const DWORD milliseconds = tb_win32_wait_milliseconds(nanoseconds - elapsed);

        if (SleepConditionVariableSRW(tb_win32_condition(condition), tb_win32_lock(mutex),
                                      milliseconds, 0))
        {
            ended_early = true;
        }
        else if (GetLastError() != ERROR_TIMEOUT)
        {
            tb_win32_thread_fail("platform.thread: tb_condition_wait_for on win32: "
                                 "SleepConditionVariableSRW failed\n");
        }
        else
        {
            elapsed = tb_platform_clock_nanoseconds() - start;
        }
    }
    /* As after tb_condition_wait(): whoever held the mutex meanwhile
     * recorded itself. */
    tb_win32_note_held(mutex);
    return ended_early;
}

void tb_condition_wake_one(tb_condition *condition)
{
    TB_ASSERT(condition != NULL);
    WakeConditionVariable(tb_win32_condition(condition));
}

void tb_condition_wake_all(tb_condition *condition)
{
    TB_ASSERT(condition != NULL);
    WakeAllConditionVariable(tb_win32_condition(condition));
}
