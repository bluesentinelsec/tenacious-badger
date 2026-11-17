/* The clock of a condition variable is a POSIX attribute; request the POSIX
 * API from the strict C17 headers. */
#define _POSIX_C_SOURCE 200809L

#include "../posix/condition_native.h"

#include "badger/platform.h"

#include <errno.h>
#include <stdio.h>
#include <time.h>

#define TB_LINUX_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* The longest wait given to the system in one call, about 31 years: adding
 * it to the time now cannot overflow any type that holds seconds. */
#define TB_LINUX_WAIT_SECONDS_MAX UINT64_C(1000000000)

/* coverage-off: runs only when a system call below fails, which no test can arrange */
static _Noreturn void tb_linux_condition_fail(const char *diagnostic)
{
    fputs(diagnostic, stderr);
    tb_platform_abort();
}
/* coverage-on */

void tb_posix_condition_start(pthread_cond_t *condition)
{
    pthread_condattr_t attributes = {0};
    int error = pthread_condattr_init(&attributes);

    /* The default clock of a condition variable is the time of day, which can
     * be set. This is the clock that tb_platform_clock_nanoseconds reads. */
    if (error == 0)
    {
        error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    }
    if (error == 0)
    {
        error = pthread_cond_init(condition, &attributes);
    }
    /* coverage-off: pthread_cond_init fails only when the system is out of memory */
    if (error != 0)
    {
        tb_linux_condition_fail("platform.thread: a condition variable on linux: "
                                "pthread_cond_init failed\n");
    }
    /* coverage-on */
    (void)pthread_condattr_destroy(&attributes);
}

bool tb_posix_condition_wait_up_to(pthread_cond_t *condition, pthread_mutex_t *mutex,
                                   uint64_t nanoseconds)
{
    struct timespec deadline = {0};
    uint64_t seconds = nanoseconds / TB_LINUX_NANOSECONDS_PER_SECOND;
    uint64_t rest = nanoseconds % TB_LINUX_NANOSECONDS_PER_SECOND;
    int error = 0;

    if (seconds >= TB_LINUX_WAIT_SECONDS_MAX)
    {
        seconds = TB_LINUX_WAIT_SECONDS_MAX;
        rest = 0;
    }
    /* coverage-off: the monotonic clock exists on every Linux the engine supports */
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    {
        tb_linux_condition_fail("platform.thread: tb_condition_wait_for on linux: clock_gettime "
                                "failed\n");
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
    error = pthread_cond_timedwait(condition, mutex, &deadline);
    if (error == ETIMEDOUT)
    {
        return false;
    }
    /* coverage-off: pthread_cond_timedwait fails otherwise only for arguments that the callers
     * above rule out */
    if (error != 0)
    {
        tb_linux_condition_fail("platform.thread: tb_condition_wait_for on linux: "
                                "pthread_cond_timedwait failed\n");
    }
    /* coverage-on */
    return true;
}
