/* pthread_cond_timedwait_relative_np is an Apple function; request it from
 * the strict C17 headers. */
#define _DARWIN_C_SOURCE

#include "../posix/condition_native.h"

#include "badger/fatal.h"
#include "badger/platform.h"

#include <errno.h>
#include <time.h>

#define TB_MACOS_NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* The longest wait given to the system in one call, about 31 years. */
#define TB_MACOS_WAIT_SECONDS_MAX UINT64_C(1000000000)

static _Noreturn void tb_macos_condition_fail(const char *diagnostic)
{
    tb_fatal_message(diagnostic);
}

void tb_posix_condition_start(pthread_cond_t *condition)
{
    if (pthread_cond_init(condition, NULL) != 0)
    {
        tb_macos_condition_fail("platform.thread: a condition variable on macos: "
                                "pthread_cond_init failed\n");
    }
}

bool tb_posix_condition_wait_up_to(pthread_cond_t *condition, pthread_mutex_t *mutex,
                                   uint64_t nanoseconds)
{
    struct timespec limit = {0};
    uint64_t seconds = nanoseconds / TB_MACOS_NANOSECONDS_PER_SECOND;
    uint64_t rest = nanoseconds % TB_MACOS_NANOSECONDS_PER_SECOND;
    int error = 0;

    if (seconds >= TB_MACOS_WAIT_SECONDS_MAX)
    {
        seconds = TB_MACOS_WAIT_SECONDS_MAX;
        rest = 0;
    }
    limit.tv_sec = (time_t)seconds;
    limit.tv_nsec = (long)rest;
    /* macOS cannot give a condition variable a clock that is never set, as
     * Linux can. This function takes a length of time and no clock at all. */
    error = pthread_cond_timedwait_relative_np(condition, mutex, &limit);
    if (error == ETIMEDOUT)
    {
        return false;
    }
    if (error != 0)
    {
        tb_macos_condition_fail("platform.thread: tb_condition_wait_for on macos: "
                                "pthread_cond_timedwait_relative_np failed\n");
    }
    return true;
}
