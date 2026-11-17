/* What the clock costs to read, and how long a sleep and a wait really take.
 *
 * For the last two the number is not a cost but an error: each case asks for
 * a length of time, and what is printed is how long it took. The difference
 * is how late the system woke the thread. */
#include "tb_bench.h"

#include "badger/platform.h"

#include <stddef.h>
#include <stdint.h>

#define TIME_CLOCK_READS ((size_t)2000000)
/* Few of them: each takes as long as it asks for. */
#define TIME_WAITS ((size_t)60)

#define TIME_MICROSECOND UINT64_C(1000)
#define TIME_MILLISECOND UINT64_C(1000000)

typedef struct time_state
{
    size_t count;    /* Readings, sleeps, or waits in a run. */
    uint64_t length; /* Nanoseconds that each sleep or wait asks for. */
} time_state;

static void run_clock(void *context)
{
    const time_state *const state = context;
    uint64_t sum = 0;

    for (size_t index = 0; index < state->count; index++)
    {
        sum += tb_platform_clock_nanoseconds();
    }
    tb_bench_keep(sum);
}

static void run_sleeps(void *context)
{
    const time_state *const state = context;

    for (size_t index = 0; index < state->count; index++)
    {
        tb_platform_sleep(state->length);
    }
}

static void run_waits(void *context)
{
    const time_state *const state = context;
    uint64_t deadline = tb_platform_clock_nanoseconds();

    /* As a frame loop does: each moment is a length after the one before. */
    for (size_t index = 0; index < state->count; index++)
    {
        deadline += state->length;
        tb_platform_wait_until(deadline);
    }
}

void tb_bench_time(tb_bench *bench)
{
    time_state clock = {0};
    time_state short_ones = {.length = 100 * TIME_MICROSECOND};
    time_state long_ones = {.length = TIME_MILLISECOND};
    time_state frames = {.length = 16667 * TIME_MICROSECOND};
    tb_bench_case read = {.name = "read the clock", .variant = "badger", .run = run_clock};
    tb_bench_case sleeps[] = {
        {.name = "ask for 100 microseconds", .variant = "tb_platform_sleep", .run = run_sleeps},
        {.name = "ask for 100 microseconds", .variant = "tb_platform_wait_until", .run = run_waits},
    };
    tb_bench_case longer[] = {
        {.name = "ask for 1 millisecond", .variant = "tb_platform_sleep", .run = run_sleeps},
        {.name = "ask for 1 millisecond", .variant = "tb_platform_wait_until", .run = run_waits},
    };
    tb_bench_case frame[] = {
        {.name = "ask for a frame at 60 a second, 16.667 milliseconds",
         .variant = "tb_platform_sleep",
         .run = run_sleeps},
        {.name = "ask for a frame at 60 a second, 16.667 milliseconds",
         .variant = "tb_platform_wait_until",
         .run = run_waits},
    };

    if (!tb_bench_wants(bench, "time"))
    {
        return;
    }
    clock.count = tb_bench_size(bench, TIME_CLOCK_READS);
    read.operations = (double)clock.count;
    read.context = &clock;
    tb_bench_measure(bench, "time", &read, 1);

    /* In quick mode one of each is enough to see that it runs. */
    short_ones.count = tb_bench_size(bench, TIME_WAITS);
    long_ones.count = tb_bench_size(bench, TIME_WAITS);
    frames.count = tb_bench_size(bench, TIME_WAITS);
    if (short_ones.count < TIME_WAITS)
    {
        short_ones.count = 1;
        long_ones.count = 1;
        frames.count = 1;
    }
    for (size_t index = 0; index < 2; index++)
    {
        sleeps[index].operations = (double)short_ones.count;
        sleeps[index].context = &short_ones;
        longer[index].operations = (double)long_ones.count;
        longer[index].context = &long_ones;
        frame[index].operations = (double)frames.count;
        frame[index].context = &frames;
    }
    tb_bench_measure(bench, "time", sleeps, 2);
    tb_bench_measure(bench, "time", longer, 2);
    tb_bench_measure(bench, "time", frame, 2);
}
