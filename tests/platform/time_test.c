/* The engine's clock must count nanoseconds and never go backward. Its unit
 * is checked against the C library's clock, which shares no code with the
 * platform adapters. */
#include "badger/platform.h"

#include "tb_test.h"

#include <inttypes.h>
#include <time.h>

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* Enough readings to cross many steps of the native counter. */
#define READING_COUNT ((size_t)1000000)

/* Long enough that the cost of reading a clock is lost in it, short enough
 * that nobody waits for the test. */
#define INTERVAL_NANOSECONDS UINT64_C(50000000)

/* The two clocks may disagree over the interval by one part in this many. A
 * clock in the wrong unit is wrong a thousandfold. */
#define TOLERANCE_DIVISOR UINT64_C(10)

/* The C library's clock tells the time of day, so the system may set it in
 * the middle of a measurement. One measurement that agrees, out of this many,
 * is enough. */
#define ATTEMPT_COUNT 3

typedef struct interval
{
    uint64_t engine; /* By the engine's clock. */
    uint64_t inner;  /* By the C library's clock, read inside the engine's readings. */
    uint64_t outer;  /* By the C library's clock, read outside them. */
} interval;

static uint64_t c_library_nanoseconds(void)
{
    struct timespec now = {0};

    if (timespec_get(&now, TIME_UTC) != TIME_UTC)
    {
        TB_TEST_FAIL("the C library's clock cannot be read");
    }
    return (uint64_t)now.tv_sec * NANOSECONDS_PER_SECOND + (uint64_t)now.tv_nsec;
}

/* Lets an interval pass and measures it three ways. The engine's readings lie
 * between the C library's, so however long the thread is made to wait between
 * two readings, the engine's interval is no shorter than `inner` and no longer
 * than `outer`. */
static interval measure_interval(void)
{
    interval measured = {0};
    const uint64_t outer_start = c_library_nanoseconds();
    const uint64_t engine_start = tb_platform_clock_nanoseconds();
    const uint64_t inner_start = c_library_nanoseconds();
    uint64_t inner_end = inner_start;
    uint64_t engine_end = {0};

    while (inner_end - inner_start < INTERVAL_NANOSECONDS)
    {
        inner_end = c_library_nanoseconds();
    }
    engine_end = tb_platform_clock_nanoseconds();
    measured.outer = c_library_nanoseconds() - outer_start;
    measured.inner = inner_end - inner_start;
    measured.engine = engine_end - engine_start;
    return measured;
}

static bool interval_agrees(const interval *measured)
{
    const uint64_t shortest = measured->inner - measured->inner / TOLERANCE_DIVISOR;
    const uint64_t longest = measured->outer + measured->outer / TOLERANCE_DIVISOR;

    return measured->engine >= shortest && measured->engine <= longest;
}

TB_TEST(platform_time, readings_never_go_backward)
{
    uint64_t previous = tb_platform_clock_nanoseconds();

    for (size_t index = 0; index < READING_COUNT; index++)
    {
        const uint64_t reading = tb_platform_clock_nanoseconds();

        if (reading < previous)
        {
            TB_TEST_FAIL("reading %zu is %" PRIu64 ", before the %" PRIu64 " read just before it",
                         index, reading, previous);
        }
        previous = reading;
    }
}

/* A clock that stands still never goes backward either. */
TB_TEST(platform_time, readings_advance)
{
    const uint64_t first = tb_platform_clock_nanoseconds();
    uint64_t reading = first;

    for (size_t index = 0; index < READING_COUNT && reading == first; index++)
    {
        reading = tb_platform_clock_nanoseconds();
    }
    TB_TEST_EXPECT_UINT(reading, >, first);
}

TB_TEST(platform_time, an_interval_is_as_long_as_the_c_library_measures_it)
{
    interval measured = {0};

    for (int attempt = 0; attempt < ATTEMPT_COUNT; attempt++)
    {
        measured = measure_interval();
        if (interval_agrees(&measured))
        {
            return;
        }
    }
    TB_TEST_FAIL("the engine's clock measured %" PRIu64
                 " ns where the C library's measured from %" PRIu64 " to %" PRIu64 " ns",
                 measured.engine, measured.inner, measured.outer);
}
