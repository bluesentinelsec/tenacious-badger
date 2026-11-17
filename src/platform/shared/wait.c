#include "wait.h"

#include "badger/platform.h"
#include "badger/thread.h"

void tb_shared_wait_until(uint64_t deadline, uint64_t slack)
{
    for (;;)
    {
        const uint64_t now = tb_platform_clock_nanoseconds();

        if (now >= deadline)
        {
            return;
        }
        if (deadline - now > slack)
        {
            /* Asleep for all of the wait but the end of it, where a sleep
             * cannot be trusted to stop in time. */
            tb_platform_sleep(deadline - now - slack);
        }
        else
        {
            tb_thread_relax();
        }
    }
}
