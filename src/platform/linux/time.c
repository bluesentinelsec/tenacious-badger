#include "badger/platform.h"

#include <stdio.h>
#include <stdlib.h>

uint64_t tb_platform_ticks(void)
{
    fputs("platform.time: tb_platform_ticks on linux is not implemented; implement clock_gettime "
          "in src/platform/linux/time.c or run the Windows build\n",
          stderr);
    exit(EXIT_FAILURE);
}
