/* A semaphore, built from the adapter's own mutex and condition variable.
 *
 * Every adapter that has those two shares this. The systems' own semaphores
 * are not used: each needs to be destroyed, and macOS has none that can live
 * inside another object, so a semaphore would stop being a value that is
 * zero-initialized and then forgotten, as a mutex and a condition are. */
#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/thread.h"

#include <stdint.h>

void tb_semaphore_signal(tb_semaphore *semaphore, size_t count)
{
    bool is_too_many = false;

    TB_ASSERT(semaphore != NULL);
    tb_mutex_lock(&semaphore->lock);
    is_too_many = count > SIZE_MAX - semaphore->count;
    if (!is_too_many)
    {
        semaphore->count += count;
    }
    tb_mutex_unlock(&semaphore->lock);
    /* Reported with the lock released, so that a handler which does not
     * return leaves the semaphore usable. */
    TB_ASSERT_MSG(!is_too_many, "a semaphore cannot count more signals than a size_t holds");
    if (count == 1)
    {
        tb_condition_wake_one(&semaphore->is_signalled);
    }
    else if (count > 1)
    {
        tb_condition_wake_all(&semaphore->is_signalled);
    }
}

void tb_semaphore_wait(tb_semaphore *semaphore)
{
    TB_ASSERT(semaphore != NULL);
    tb_mutex_lock(&semaphore->lock);
    while (semaphore->count == 0)
    {
        tb_condition_wait(&semaphore->is_signalled, &semaphore->lock);
    }
    semaphore->count--;
    tb_mutex_unlock(&semaphore->lock);
}

bool tb_semaphore_try_wait(tb_semaphore *semaphore)
{
    bool is_taken = false;

    TB_ASSERT(semaphore != NULL);
    tb_mutex_lock(&semaphore->lock);
    if (semaphore->count > 0)
    {
        semaphore->count--;
        is_taken = true;
    }
    tb_mutex_unlock(&semaphore->lock);
    return is_taken;
}

bool tb_semaphore_wait_for(tb_semaphore *semaphore, uint64_t nanoseconds)
{
    uint64_t start = 0;
    bool is_taken = false;

    TB_ASSERT(semaphore != NULL);
    start = tb_platform_clock_nanoseconds();
    tb_mutex_lock(&semaphore->lock);
    /* A wait on the condition may end early, woken for a signal that another
     * thread then took, so the time that is left is worked out again each
     * time round. */
    while (semaphore->count == 0)
    {
        const uint64_t waited = tb_platform_clock_nanoseconds() - start;

        if (waited >= nanoseconds)
        {
            break;
        }
        (void)tb_condition_wait_for(&semaphore->is_signalled, &semaphore->lock,
                                    nanoseconds - waited);
    }
    if (semaphore->count > 0)
    {
        semaphore->count--;
        is_taken = true;
    }
    tb_mutex_unlock(&semaphore->lock);
    return is_taken;
}
