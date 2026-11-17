/* clock_gettime_nsec_np is an Apple function; request it from the strict C17 headers. */
#define _DARWIN_C_SOURCE

#include "badger/platform.h"

#include "../shared/wait.h"

#include <stdio.h>
#include <time.h>

#define TB_MACOS_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* The longest sleep that is asked of the system at one time: 31 years. */
#define TB_MACOS_SLEEP_SECONDS_MAX UINT64_C(1000000000)

/* How late a sleep may wake on a machine that is not busy: the system
 * gathers the timers of ordinary threads so that they fire together.
 * tb_platform_wait_until() stays awake for this long before its deadline. */
#define TB_MACOS_SLEEP_SLACK_NANOSECONDS UINT64_C(1000000)

uint64_t tb_platform_clock_nanoseconds(void)
{
    /* CLOCK_UPTIME_RAW is the counter that mach_absolute_time reads, already
     * converted to nanoseconds. It is never set, and stops while the system
     * is asleep. The function returns zero when it fails. */
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    /* coverage-off: the clock exists on every macOS the engine supports */
    if (now == 0)
    {
        fputs("platform.time: tb_platform_clock_nanoseconds on macos: clock_gettime_nsec_np "
              "failed\n",
              stderr);
        tb_platform_abort();
    }
    /* coverage-on */
    return now;
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
    /* nanosleep counts time as the clock above does, and returns early when
     * a signal interrupts it. The clock says how much is left either way. */
    while (waited < nanoseconds)
    {
        const uint64_t left = nanoseconds - waited;
        struct timespec request = {0};
        uint64_t seconds = left / TB_MACOS_NANOSECONDS_PER_SECOND;
        uint64_t rest = left % TB_MACOS_NANOSECONDS_PER_SECOND;

        if (seconds >= TB_MACOS_SLEEP_SECONDS_MAX)
        {
            seconds = TB_MACOS_SLEEP_SECONDS_MAX;
            rest = 0;
        }
        request.tv_sec = (time_t)seconds;
        request.tv_nsec = (long)rest;
        (void)nanosleep(&request, NULL);
        waited = tb_platform_clock_nanoseconds() - start;
    }
}

void tb_platform_wait_until(uint64_t deadline)
{
    tb_shared_wait_until(deadline, TB_MACOS_SLEEP_SLACK_NANOSECONDS);
}
