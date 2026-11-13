#include "badger/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

uint64_t tb_platform_ticks(void)
{
    LARGE_INTEGER counter = {0};

    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
    {
        fputs("platform.time: tb_platform_ticks on win32: QueryPerformanceCounter failed\n",
              stderr);
        exit(EXIT_FAILURE);
    }
    return (uint64_t)counter.QuadPart;
}
