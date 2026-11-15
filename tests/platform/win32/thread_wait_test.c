/* The Windows adapter gives a system wait its time limit in milliseconds. The
 * limit must never be shorter than the nanoseconds asked for, or a wait would
 * end too soon, and must stay below the value that means no limit. */
#include "thread_wait.h"

#include "tb_test.h"

#include <inttypes.h>

#define NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)

typedef struct wait_row
{
    uint64_t nanoseconds;
    uint32_t milliseconds;
} wait_row;

TB_TEST(platform_win32_thread_wait, rounds_a_limit_up_to_whole_milliseconds)
{
    static const wait_row rows[] = {
        {0, 0},
        {1, 1},
        {NANOSECONDS_PER_MILLISECOND - 1, 1},
        {NANOSECONDS_PER_MILLISECOND, 1},
        {NANOSECONDS_PER_MILLISECOND + 1, 2},
        {50 * NANOSECONDS_PER_MILLISECOND, 50},
        /* The longest limit a wait accepts, and those beyond it. */
        {(TB_WIN32_WAIT_MILLISECONDS_MAX - 1) * NANOSECONDS_PER_MILLISECOND + 1,
         TB_WIN32_WAIT_MILLISECONDS_MAX},
        {TB_WIN32_WAIT_MILLISECONDS_MAX * NANOSECONDS_PER_MILLISECOND,
         TB_WIN32_WAIT_MILLISECONDS_MAX},
        {TB_WIN32_WAIT_MILLISECONDS_MAX * NANOSECONDS_PER_MILLISECOND + 1,
         TB_WIN32_WAIT_MILLISECONDS_MAX},
        {(TB_WIN32_WAIT_MILLISECONDS_MAX + UINT64_C(1)) * NANOSECONDS_PER_MILLISECOND,
         TB_WIN32_WAIT_MILLISECONDS_MAX},
        {UINT64_MAX - 1, TB_WIN32_WAIT_MILLISECONDS_MAX},
        {UINT64_MAX, TB_WIN32_WAIT_MILLISECONDS_MAX},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu: %" PRIu64 " nanoseconds", index, rows[index].nanoseconds);
        TB_TEST_EXPECT_UINT(tb_win32_wait_milliseconds(rows[index].nanoseconds), ==,
                            rows[index].milliseconds);
    }
}
