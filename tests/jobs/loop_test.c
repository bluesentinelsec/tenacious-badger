/* A parallel loop must run every iteration exactly once, in ranges that are
 * never longer than was asked and never outside the loop, and must return
 * only when all of them are done. These cases check that for counts on each
 * side of every multiple of the grain, for schedulers of one worker and of
 * several, and for loops inside loops and inside jobs.
 *
 * The body of a loop only records, in atomic values that the case gave it,
 * and the case checks after the loop has returned. */
#include "badger/jobs.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>

static tb_jobs *create_with_workers(size_t worker_count)
{
    const tb_jobs_options options = {.worker_count = worker_count};

    return tb_jobs_create(&options);
}

/* What the ranges of one loop did. */
typedef struct visits
{
    tb_platform_atomic_size *by_iteration;   /* Times each iteration ran. */
    size_t count;                            /* Iterations in the loop. */
    size_t grain;                            /* The longest range allowed; 0 for any. */
    tb_platform_atomic_size range_count;     /* Calls of the body. */
    tb_platform_atomic_size bad_range_count; /* Ranges that were empty, too long, or outside. */
} visits;

static void visit_a_range(void *argument, size_t first, size_t count)
{
    visits *const seen = argument;
    const bool is_too_long = seen->grain != 0 && count > seen->grain;

    (void)tb_platform_atomic_size_add(&seen->range_count, 1);
    if (count == 0 || is_too_long || first >= seen->count || count > seen->count - first)
    {
        (void)tb_platform_atomic_size_add(&seen->bad_range_count, 1);
        return;
    }
    for (size_t index = first; index < first + count; index++)
    {
        (void)tb_platform_atomic_size_add(&seen->by_iteration[index], 1);
    }
}

/* Iterations that did not run exactly once. */
static size_t count_wrong_visits(const visits *seen)
{
    size_t wrong_count = 0;

    for (size_t index = 0; index < seen->count; index++)
    {
        if (tb_platform_atomic_size_load(&seen->by_iteration[index]) != 1)
        {
            wrong_count++;
        }
    }
    return wrong_count;
}

/* Runs one loop, and checks that every iteration ran once in good ranges.
 * Returns the number of ranges. */
static size_t run_and_check_a_loop(tb_jobs *jobs, size_t count, size_t grain)
{
    visits seen = {.count = count, .grain = grain};
    size_t range_count = 0;

    seen.by_iteration = tb_calloc(count, sizeof(*seen.by_iteration));
    tb_jobs_for(jobs, count, grain, visit_a_range, &seen);
    range_count = tb_platform_atomic_size_load(&seen.range_count);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&seen.bad_range_count), ==, 0);
    TB_TEST_EXPECT_UINT(count_wrong_visits(&seen), ==, 0);
    tb_free(seen.by_iteration);
    return range_count;
}

static size_t divide_rounding_up(size_t dividend, size_t divisor)
{
    size_t quotient = dividend / divisor;

    if (dividend % divisor != 0)
    {
        quotient++;
    }
    return quotient;
}

TB_TEST(jobs_loop, every_iteration_runs_once_around_every_multiple_of_the_grain)
{
    const size_t grains[] = {1, 2, 7, 64};
    tb_jobs *const jobs = create_with_workers(4);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(grains); index++)
    {
        const size_t grain = grains[index];

        /* Each side of the first few multiples, from one iteration up. */
        for (size_t count = 1; count <= grain * 5 + 1; count++)
        {
            TB_TEST_CONTEXT("%zu iterations in ranges of %zu", count, grain);
            /* A range is as long as the grain, except for the last. */
            TB_TEST_EXPECT_UINT(run_and_check_a_loop(jobs, count, grain), ==,
                                divide_rounding_up(count, grain));
        }
    }
    tb_jobs_destroy(jobs);
}

TB_TEST(jobs_loop, a_long_loop_runs_once_whatever_the_number_of_workers)
{
    const size_t worker_counts[] = {1, 2, 8};
    const size_t count = 100003;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(worker_counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(worker_counts[index]);

        TB_TEST_CONTEXT("%zu workers", worker_counts[index]);
        TB_TEST_EXPECT_UINT(run_and_check_a_loop(jobs, count, 16), ==,
                            divide_rounding_up(count, 16));
        tb_jobs_destroy(jobs);
    }
}

/* Left to the scheduler, the grain gives about four ranges for each thread,
 * and never an empty one. */
TB_TEST(jobs_loop, a_grain_left_to_the_scheduler_gives_a_few_ranges_for_each_thread)
{
    const size_t worker_count = 3;
    const size_t thread_count = worker_count + 1;
    const size_t counts[] = {1, 2, 15, 16, 17, 1000, 100003};
    tb_jobs *const jobs = create_with_workers(worker_count);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        const size_t count = counts[index];
        const size_t range_count = run_and_check_a_loop(jobs, count, 0);

        TB_TEST_CONTEXT("%zu iterations", count);
        TB_TEST_EXPECT_UINT(range_count, >=, 1);
        TB_TEST_EXPECT_UINT(range_count, <=, thread_count * 4);
        /* Enough of them to share out, where there are iterations enough. */
        if (count >= thread_count * 4)
        {
            TB_TEST_EXPECT_UINT(range_count, >, thread_count);
        }
        else
        {
            TB_TEST_EXPECT_UINT(range_count, ==, count);
        }
    }
    tb_jobs_destroy(jobs);
}

static void count_a_call(void *argument, size_t first, size_t count)
{
    (void)first;
    (void)count;
    (void)tb_platform_atomic_size_add(argument, 1);
}

TB_TEST(jobs_loop, a_loop_of_no_iterations_calls_nothing)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size calls = {0};

    tb_jobs_for(jobs, 0, 0, count_a_call, &calls);
    tb_jobs_for(jobs, 0, 8, count_a_call, &calls);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&calls), ==, 0);
    tb_jobs_destroy(jobs);
}

TB_TEST(jobs_loop, a_grain_larger_than_the_loop_gives_one_range)
{
    tb_jobs *const jobs = create_with_workers(2);

    TB_TEST_EXPECT_UINT(run_and_check_a_loop(jobs, 10, 11), ==, 1);
    TB_TEST_EXPECT_UINT(run_and_check_a_loop(jobs, 10, 10), ==, 1);
    TB_TEST_EXPECT_UINT(run_and_check_a_loop(jobs, 10, SIZE_MAX), ==, 1);
    TB_TEST_EXPECT_UINT(run_and_check_a_loop(jobs, 1, SIZE_MAX), ==, 1);
    tb_jobs_destroy(jobs);
}

/* The loop has returned only when every range is done: a sum that the body
 * builds is complete the moment the call comes back. */
typedef struct sum
{
    const uint32_t *values;
    tb_platform_atomic_size total;
} sum;

static void add_a_range(void *argument, size_t first, size_t count)
{
    sum *const whole = argument;
    size_t part = 0;

    for (size_t index = first; index < first + count; index++)
    {
        part += whole->values[index];
    }
    (void)tb_platform_atomic_size_add(&whole->total, part);
}

#define SUM_COUNT ((size_t)200000)
#define SUM_ROUNDS 50

TB_TEST(jobs_loop, the_work_is_done_when_the_loop_returns)
{
    tb_jobs *const jobs = create_with_workers(4);
    uint32_t *const values = tb_calloc(SUM_COUNT, sizeof(*values));
    size_t expected = 0;
    size_t wrong_count = 0;

    for (size_t index = 0; index < SUM_COUNT; index++)
    {
        values[index] = (uint32_t)tb_test_random_below(1000);
        expected += values[index];
    }
    for (size_t round = 0; round < SUM_ROUNDS; round++)
    {
        sum whole = {.values = values};

        tb_jobs_for(jobs, SUM_COUNT, 0, add_a_range, &whole);
        if (tb_platform_atomic_size_load(&whole.total) != expected)
        {
            wrong_count++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong_count, ==, 0);
    tb_jobs_destroy(jobs);
    tb_free(values);
}

#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* Longest that a range waits for the others. No machine is so slow, so
 * reaching it means that the ranges do not run at the same time. */
#define PATIENCE_NANOSECONDS (UINT64_C(20) * NANOSECONDS_PER_SECOND)

#define MEETING_SIZE ((size_t)4)

/* Every range waits until all of them have started, which ranges that ran
 * one after another never could. Each notes which thread it ran on. */
typedef struct meeting
{
    tb_platform_atomic_size arrived;
    tb_platform_atomic_size gave_up;
    uint64_t thread_ids[MEETING_SIZE]; /* By iteration: the thread that ran it. */
} meeting;

static void meet_the_other_ranges(void *argument, size_t first, size_t count)
{
    meeting *const all = argument;
    const uint64_t start = tb_platform_clock_nanoseconds();

    (void)count;
    all->thread_ids[first] = tb_thread_current_id();
    (void)tb_platform_atomic_size_add(&all->arrived, 1);
    while (tb_platform_atomic_size_load(&all->arrived) < MEETING_SIZE)
    {
        if (tb_platform_clock_nanoseconds() - start > PATIENCE_NANOSECONDS)
        {
            (void)tb_platform_atomic_size_add(&all->gave_up, 1);
            return;
        }
    }
}

/* Three workers and the calling thread, and four ranges that cannot finish
 * until all four are running: the loop is parallel, and the caller is one of
 * the threads that runs it. */
TB_TEST(jobs_loop, ranges_run_at_the_same_time_on_the_workers_and_the_caller)
{
    tb_jobs *const jobs = create_with_workers(MEETING_SIZE - 1);
    meeting all = {0};
    const uint64_t own_id = tb_thread_current_id();
    size_t own_count = 0;

    tb_jobs_for(jobs, MEETING_SIZE, 1, meet_the_other_ranges, &all);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&all.gave_up), ==, 0);
    for (size_t index = 0; index < MEETING_SIZE; index++)
    {
        if (all.thread_ids[index] == own_id)
        {
            own_count++;
        }
        for (size_t other = index + 1; other < MEETING_SIZE; other++)
        {
            TB_TEST_CONTEXT("ranges %zu and %zu", index, other);
            TB_TEST_EXPECT_UINT(all.thread_ids[index], !=, all.thread_ids[other]);
        }
    }
    TB_TEST_EXPECT_UINT(own_count, ==, 1);
    tb_jobs_destroy(jobs);
}

/* ---------------------------------------------------------------------------
 * Loops inside loops, and inside jobs.
 * ------------------------------------------------------------------------- */

#define OUTER_COUNT ((size_t)24)
#define INNER_COUNT ((size_t)500)

typedef struct grid
{
    tb_jobs *jobs;
    tb_platform_atomic_size *cells; /* OUTER_COUNT rows of INNER_COUNT. */
} grid;

typedef struct grid_row
{
    grid *whole;
    size_t row;
} grid_row;

static void visit_cells_of_a_row(void *argument, size_t first, size_t count)
{
    const grid_row *const row = argument;

    for (size_t index = first; index < first + count; index++)
    {
        (void)tb_platform_atomic_size_add(&row->whole->cells[row->row * INNER_COUNT + index], 1);
    }
}

static void loop_over_rows(void *argument, size_t first, size_t count)
{
    grid *const whole = argument;

    for (size_t row = first; row < first + count; row++)
    {
        grid_row inner = {.whole = whole, .row = row};

        tb_jobs_for(whole->jobs, INNER_COUNT, 16, visit_cells_of_a_row, &inner);
    }
}

static size_t count_cells_that_are_not_one(const grid *whole)
{
    size_t wrong_count = 0;

    for (size_t index = 0; index < OUTER_COUNT * INNER_COUNT; index++)
    {
        if (tb_platform_atomic_size_load(&whole->cells[index]) != 1)
        {
            wrong_count++;
        }
    }
    return wrong_count;
}

/* Every thread of the outer loop is inside an inner loop at once, waiting
 * for ranges that only threads that wait can run. */
TB_TEST(jobs_loop, a_loop_inside_a_loop_finishes)
{
    const size_t worker_counts[] = {1, 3};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(worker_counts); index++)
    {
        grid whole = {.jobs = create_with_workers(worker_counts[index])};

        TB_TEST_CONTEXT("%zu workers", worker_counts[index]);
        whole.cells = tb_calloc(OUTER_COUNT * INNER_COUNT, sizeof(*whole.cells));
        tb_jobs_for(whole.jobs, OUTER_COUNT, 1, loop_over_rows, &whole);
        TB_TEST_EXPECT_UINT(count_cells_that_are_not_one(&whole), ==, 0);
        tb_jobs_destroy(whole.jobs);
        tb_free(whole.cells);
    }
}

static void loop_over_the_grid_from_a_job(void *argument)
{
    grid *const whole = argument;

    tb_jobs_for(whole->jobs, OUTER_COUNT, 2, loop_over_rows, whole);
}

TB_TEST(jobs_loop, a_loop_runs_from_inside_a_job)
{
    grid whole = {.jobs = create_with_workers(2)};

    whole.cells = tb_calloc(OUTER_COUNT * INNER_COUNT, sizeof(*whole.cells));
    tb_jobs_wait(whole.jobs, tb_jobs_run(whole.jobs, loop_over_the_grid_from_a_job, &whole));
    TB_TEST_EXPECT_UINT(count_cells_that_are_not_one(&whole), ==, 0);
    tb_jobs_destroy(whole.jobs);
    tb_free(whole.cells);
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void loop_with_null_jobs(void *context)
{
    tb_jobs_for(NULL, 10, 1, count_a_call, context);
}

static void loop_with_no_body(void *context)
{
    tb_jobs_for(context, 10, 1, NULL, NULL);
}

static void loop_of_nothing_with_no_body(void *context)
{
    tb_jobs_for(context, 0, 1, NULL, NULL);
}

TB_TEST(jobs_loop, bad_arguments_violate_the_contract)
{
    tb_jobs *jobs = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    jobs = create_with_workers(1);
    TB_TEST_EXPECT_TRAP(loop_with_null_jobs, NULL, "jobs != NULL");
    TB_TEST_EXPECT_TRAP(loop_with_no_body, jobs, "function != NULL");
    /* A body is required even where it would not be called. */
    TB_TEST_EXPECT_TRAP(loop_of_nothing_with_no_body, jobs, "function != NULL");
    tb_jobs_destroy(jobs);
}
