/* clock_gettime_nsec_np is an Apple function; request it from the strict C17 headers. */
#define _DARWIN_C_SOURCE

#include "badger/platform.h"

#include <stdio.h>
#include <time.h>

uint64_t tb_platform_clock_nanoseconds(void)
{
    /* CLOCK_UPTIME_RAW is the counter that mach_absolute_time reads, already
     * converted to nanoseconds. It is never set, and stops while the system
     * is asleep. The function returns zero when it fails. */
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    if (now == 0)
    {
        fputs("platform.time: tb_platform_clock_nanoseconds on macos: clock_gettime_nsec_np "
              "failed\n",
              stderr);
        tb_platform_abort();
    }
    return now;
}
