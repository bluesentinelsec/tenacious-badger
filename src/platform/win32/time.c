#include "badger/platform.h"

#include "counter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>

/* At the usual frequency a count is a whole number of nanoseconds. */
#define TB_WIN32_NANOSECONDS_PER_USUAL_COUNT                                                       \
    (TB_WIN32_NANOSECONDS_PER_SECOND / TB_WIN32_USUAL_COUNTS_PER_SECOND)

uint64_t tb_win32_counter_to_nanoseconds(uint64_t count, uint64_t counts_per_second)
{
    uint64_t seconds = {0};
    uint64_t rest = {0};

    /* One multiplication gives what the divisions below give, and takes a
     * third off the cost of reading the clock. */
    if (counts_per_second == TB_WIN32_USUAL_COUNTS_PER_SECOND)
    {
        return count * TB_WIN32_NANOSECONDS_PER_USUAL_COUNT;
    }
    /* Multiplying the count by a billion first would overflow half an hour
     * after the system starts. The whole seconds are converted exactly, so
     * rounding the rest down rounds the sum down. */
    seconds = count / counts_per_second;
    rest = count % counts_per_second;
    return seconds * TB_WIN32_NANOSECONDS_PER_SECOND +
           rest * TB_WIN32_NANOSECONDS_PER_SECOND / counts_per_second;
}

static _Noreturn void tb_win32_clock_fail(const char *diagnostic)
{
    fputs(diagnostic, stderr);
    tb_platform_abort();
}

uint64_t tb_platform_clock_nanoseconds(void)
{
    LARGE_INTEGER frequency = {0};
    LARGE_INTEGER counter = {0};

    /* The frequency is fixed when the system starts, and reading it costs
     * less than reading the counter, so nothing is kept between calls. */
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        tb_win32_clock_fail("platform.time: tb_platform_clock_nanoseconds on win32: "
                            "QueryPerformanceFrequency failed\n");
    }
    if ((uint64_t)frequency.QuadPart > TB_WIN32_COUNTER_FREQUENCY_MAX)
    {
        tb_win32_clock_fail("platform.time: tb_platform_clock_nanoseconds on win32: the "
                            "performance counter is too fast to convert to nanoseconds\n");
    }
    if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
    {
        tb_win32_clock_fail("platform.time: tb_platform_clock_nanoseconds on win32: "
                            "QueryPerformanceCounter failed\n");
    }
    return tb_win32_counter_to_nanoseconds((uint64_t)counter.QuadPart,
                                           (uint64_t)frequency.QuadPart);
}
