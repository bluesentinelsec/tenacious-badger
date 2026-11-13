#include "badger/platform.h"

#include <stdio.h>
#include <stdlib.h>

uint64_t tb_platform_ticks(void)
{
    fputs("platform.time: tb_platform_ticks on macos is not implemented; implement "
          "mach_absolute_time "
          "in src/platform/macos/time.c or run the Windows build\n",
          stderr);
    exit(EXIT_FAILURE);
}
