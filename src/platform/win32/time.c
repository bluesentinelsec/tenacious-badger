#include "badger/fatal.h"
#include "badger/platform.h"

#include "../shared/wait.h"
#include "counter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

/* At the usual frequency a count is a whole number of nanoseconds. */
#define TB_WIN32_NANOSECONDS_PER_USUAL_COUNT                                                       \
    (TB_WIN32_NANOSECONDS_PER_SECOND / TB_WIN32_USUAL_COUNTS_PER_SECOND)

uint64_t tb_win32_counter_to_nanoseconds(uint64_t count, uint64_t counts_per_second)
{
    uint64_t seconds = {0};
    uint64_t rest = {0};

    /* One multiplication gives what the divisions below give, and takes a
     * third off the cost of reading the clock. */
    if (counts_per_second == TB_WIN32_USUAL_COUNTS_PER_SECOND)
    {
        return count * TB_WIN32_NANOSECONDS_PER_USUAL_COUNT;
    }
    /* Multiplying the count by a billion first would overflow half an hour
     * after the system starts. The whole seconds are converted exactly, so
     * rounding the rest down rounds the sum down. */
    seconds = count / counts_per_second;
    rest = count % counts_per_second;
    return seconds * TB_WIN32_NANOSECONDS_PER_SECOND +
           rest * TB_WIN32_NANOSECONDS_PER_SECOND / counts_per_second;
}

/* coverage-off: runs only when the performance counter fails, which Windows documents it never does
 */
static _Noreturn void tb_win32_clock_fail(const char *diagnostic)
{
    tb_fatal_message(diagnostic);
}
/* coverage-on */

uint64_t tb_platform_clock_nanoseconds(void)
{
    LARGE_INTEGER frequency = {0};
    LARGE_INTEGER counter = {0};

    /* The frequency is fixed when the system starts, and reading it costs
     * less than reading the counter, so nothing is kept between calls. */
    /* coverage-off: as above, and no machine has a counter that fast */
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        tb_win32_clock_fail("platform.time: tb_platform_clock_nanoseconds on win32: "
                            "QueryPerformanceFrequency failed\n");
    }
    if ((uint64_t)frequency.QuadPart > TB_WIN32_COUNTER_FREQUENCY_MAX)
    {
        tb_win32_clock_fail("platform.time: tb_platform_clock_nanoseconds on win32: the "
                            "performance counter is too fast to convert to nanoseconds\n");
    }
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
    {
        tb_win32_clock_fail("platform.time: tb_platform_clock_nanoseconds on win32: "
                            "QueryPerformanceCounter failed\n");
    }
    /* coverage-on */
    return tb_win32_counter_to_nanoseconds((uint64_t)counter.QuadPart,
                                           (uint64_t)frequency.QuadPart);
}

/* ---------------------------------------------------------------------------
 * Sleeping.
 * ------------------------------------------------------------------------- */

/* A timer that is not bound to the system's timer interrupt, which comes
 * every 15.6 milliseconds unless something has asked for it more often.
 * Windows 10 version 1803 and later have it; older SDK headers do not name
 * it. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* A timer counts in units of 100 nanoseconds. */
#define TB_WIN32_NANOSECONDS_PER_TIMER_UNIT UINT64_C(100)

/* How late a sleep on a high-resolution timer may wake on a machine that is
 * not busy. tb_platform_wait_until() stays awake for this long before its
 * deadline. */
#define TB_WIN32_SLEEP_SLACK_NANOSECONDS UINT64_C(1000000)

/* Sleeps once on a timer of its own. The timer may fire a little before the
 * time is up by the performance counter, so the caller looks at the clock. */
static void tb_win32_sleep_once(uint64_t nanoseconds)
{
    HANDLE timer =
        CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due = {0};
    uint64_t units = nanoseconds / TB_WIN32_NANOSECONDS_PER_TIMER_UNIT;

    /* coverage-off: needs a Windows from before 2018, or a process that is out of handles */
    if (timer == NULL)
    {
        timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    if (timer == NULL)
    {
        tb_win32_clock_fail("platform.time: tb_platform_sleep on win32: "
                            "CreateWaitableTimerExW failed\n");
    }
    /* coverage-on */
    /* Rounded up to a whole unit, and no more than a timer can be given. */
    if (nanoseconds % TB_WIN32_NANOSECONDS_PER_TIMER_UNIT != 0)
    {
        units++;
    }
    if (units > (uint64_t)INT64_MAX)
    {
        units = (uint64_t)INT64_MAX;
    }
    /* A negative time is a length of time from now. */
    due.QuadPart = -(LONGLONG)units;
    /* coverage-off: setting and waiting on a timer of this thread's own does not fail */
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) ||
        WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0)
    {
        tb_win32_clock_fail("platform.time: tb_platform_sleep on win32: the timer could not "
                            "be waited for\n");
    }
    /* coverage-on */
    (void)CloseHandle(timer);
}

void tb_platform_sleep(uint64_t nanoseconds)
{
    uint64_t start = 0;
    uint64_t waited = 0;

    if (nanoseconds == 0)
    {
        return;
    }
    start = tb_platform_clock_nanoseconds();
    while (waited < nanoseconds)
    {
        tb_win32_sleep_once(nanoseconds - waited);
        waited = tb_platform_clock_nanoseconds() - start;
    }
}

void tb_platform_wait_until(uint64_t deadline)
{
    tb_shared_wait_until(deadline, TB_WIN32_SLEEP_SLACK_NANOSECONDS);
}
