/* clock_gettime is a POSIX function; request the POSIX API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "badger/platform.h"

#include "../shared/wait.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

#define TB_LINUX_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* The longest sleep that is asked of the system: 31 years. Adding it to the
 * time now cannot overflow any type that holds seconds. */
#define TB_LINUX_SLEEP_SECONDS_MAX UINT64_C(1000000000)

/* How late a sleep may wake on a machine that is not busy: the kernel lets a
 * timer of an ordinary thread run 50 microseconds late, and the thread then
 * has to be given a processor. tb_platform_wait_until() stays awake for this
 * long before its deadline. */
#define TB_LINUX_SLEEP_SLACK_NANOSECONDS UINT64_C(200000)

uint64_t tb_platform_clock_nanoseconds(void)
{
    struct timespec now = {0};

    /* CLOCK_MONOTONIC counts from an unspecified moment, is never set, and
     * stops while the system is suspended. */
    /* coverage-off: the monotonic clock exists on every Linux the engine supports */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        fputs("platform.time: tb_platform_clock_nanoseconds on linux: clock_gettime failed\n",
              stderr);
        tb_platform_abort();
    }
    /* coverage-on */
    return (uint64_t)now.tv_sec * TB_LINUX_NANOSECONDS_PER_SECOND + (uint64_t)now.tv_nsec;
}

void tb_platform_sleep(uint64_t nanoseconds)
{
    struct timespec deadline = {0};
    uint64_t seconds = nanoseconds / TB_LINUX_NANOSECONDS_PER_SECOND;
    uint64_t rest = nanoseconds % TB_LINUX_NANOSECONDS_PER_SECOND;
    int error = EINTR;

    if (nanoseconds == 0)
    {
        return;
    }
    if (seconds >= TB_LINUX_SLEEP_SECONDS_MAX)
    {
        seconds = TB_LINUX_SLEEP_SECONDS_MAX;
        rest = 0;
    }
    /* coverage-off: the monotonic clock exists on every Linux the engine supports */
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    {
        fputs("platform.time: tb_platform_sleep on linux: clock_gettime failed\n", stderr);
        tb_platform_abort();
    }
    /* coverage-on */
    /* Both parts of a second are less than a billion, so their sum is less
     * than two. */
    rest += (uint64_t)deadline.tv_nsec;
    if (rest >= TB_LINUX_NANOSECONDS_PER_SECOND)
    {
        rest -= TB_LINUX_NANOSECONDS_PER_SECOND;
        seconds += 1;
    }
    deadline.tv_sec += (time_t)seconds;
    deadline.tv_nsec = (long)rest;
    /* Until a moment, not for a length of time, so that a signal which
     * interrupts the sleep does not change when it ends. */
    while (error == EINTR)
    {
        error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    /* coverage-off: clock_nanosleep fails otherwise only for arguments that are ruled out above */
    if (error != 0)
    {
        fputs("platform.time: tb_platform_sleep on linux: clock_nanosleep failed\n", stderr);
        tb_platform_abort();
    }
    /* coverage-on */
}

void tb_platform_wait_until(uint64_t deadline)
{
    tb_shared_wait_until(deadline, TB_LINUX_SLEEP_SLACK_NANOSECONDS);
}
