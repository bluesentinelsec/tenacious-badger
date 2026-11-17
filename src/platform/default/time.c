#include "badger/platform.h"

#include "not_implemented.h"

uint64_t tb_platform_clock_nanoseconds(void)
{
    tb_default_not_implemented("platform.time", "tb_platform_clock_nanoseconds");
}

void tb_platform_sleep(uint64_t nanoseconds)
{
    (void)nanoseconds;
    tb_default_not_implemented("platform.time", "tb_platform_sleep");
}

void tb_platform_wait_until(uint64_t deadline)
{
    (void)deadline;
    tb_default_not_implemented("platform.time", "tb_platform_wait_until");
}
