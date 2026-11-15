/* Private to the Linux and macOS adapters. POSIX threads are the same on
 * both, except in how a condition variable waits for a limited time by a
 * clock that is never set. src/platform/posix/thread.c is shared, and each
 * system supplies these two functions in its own condition.c. */
#ifndef TB_POSIX_CONDITION_NATIVE_H
#define TB_POSIX_CONDITION_NATIVE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* Makes a condition variable in storage that holds none yet. A failure ends
 * the process with a diagnostic. */
void tb_posix_condition_start(pthread_cond_t *condition);

/* Waits on a condition variable for at most `nanoseconds`, which is more
 * than zero. `mutex` is held by the caller, and held again on return. Returns
 * true when the wait ended before the limit, and false when the limit, or a
 * shorter one that the system could count to, passed. A failure ends the
 * process with a diagnostic. */
bool tb_posix_condition_wait_up_to(pthread_cond_t *condition, pthread_mutex_t *mutex,
                                   uint64_t nanoseconds);

#endif
