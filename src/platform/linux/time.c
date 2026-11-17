/* clock_gettime is a POSIX function; request the POSIX API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "badger/platform.h"

#include <stdio.h>
#include <time.h>

#define TB_LINUX_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

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
