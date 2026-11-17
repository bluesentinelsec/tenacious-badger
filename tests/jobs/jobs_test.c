/* The scheduler must run every job exactly once, never start a job before
 * the jobs it depends on have finished, take foreground work first, let a job
 * wait for jobs it submitted without the pool running dry, and wake a worker
 * for every job. Each of those is checked here through the public interface,
 * with schedulers of one worker and of several.
 *
 * A job only records what it saw, in memory that the case gave it, mostly in
 * atomic values, and the case checks after waiting. A wait that a broken
 * scheduler would never end is replaced by a look at the clock, so that the
 * case fails where it would otherwise hang. */
#include "badger/jobs.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/system.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>

#define NANOSECONDS_PER_MICROSECOND UINT64_C(1000)
#define NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* Longest that a case waits for something that must happen. No machine is so
 * slow, so reaching it means that the scheduler lost a job or a wake. */
#define PATIENCE_NANOSECONDS (UINT64_C(20) * NANOSECONDS_PER_SECOND)

/* ---------------------------------------------------------------------------
 * Helpers.
 * ------------------------------------------------------------------------- */

static tb_jobs *create_with_workers(size_t worker_count)
{
    const tb_jobs_options options = {.worker_count = worker_count};

    return tb_jobs_create(&options);
}

static void count_one(void *argument)
{
    (void)tb_platform_atomic_size_add(argument, 1);
}

static void do_nothing(void *argument)
{
    (void)argument;
}

/* How many of `cells` do not hold exactly `expected`. */
static size_t count_cells_that_are_not(tb_platform_atomic_size *cells, size_t cell_count,
                                       size_t expected)
{
    size_t wrong_count = 0;

    for (size_t index = 0; index < cell_count; index++)
    {
        if (tb_platform_atomic_size_load(&cells[index]) != expected)
        {
            wrong_count++;
        }
    }
    return wrong_count;
}

/* Whether a job finishes without the calling thread running anything: the
 * workers alone must do it. Returns false when it has not in all the time it
 * could take. */
static bool finishes_without_help(tb_jobs *jobs, tb_job job)
{
    const uint64_t start = tb_platform_clock_nanoseconds();

    while (!tb_jobs_is_finished(jobs, job))
    {
        if (tb_platform_clock_nanoseconds() - start > PATIENCE_NANOSECONDS)
        {
            return false;
        }
    }
    return true;
}

/* Keeps the calling thread busy for a length of time, without sleeping. */
static void pass_the_time(uint64_t nanoseconds)
{
    const uint64_t start = tb_platform_clock_nanoseconds();

    while (tb_platform_clock_nanoseconds() - start < nanoseconds)
    {
    }
}

/* Longer than a thread goes on looking for a job before it sleeps, many
 * times over: after this long with nothing to do, every worker is asleep. */
#define LONG_ENOUGH_TO_FALL_ASLEEP (UINT64_C(5) * NANOSECONDS_PER_MILLISECOND)

/* A job that takes long enough for a thread that waits for it to give up
 * looking for other work and go to sleep. */
static void take_a_long_time(void *argument)
{
    pass_the_time(LONG_ENOUGH_TO_FALL_ASLEEP);
    (void)tb_platform_atomic_size_add(argument, 1);
}

/* A place where jobs stop until the case lets them go. */
typedef struct gate
{
    tb_mutex lock;       /* Guards `is_open`. */
    tb_condition opened; /* Woken when `is_open` becomes true. */
    bool is_open;
    tb_platform_atomic_size arrived; /* Jobs that have reached the gate. */
} gate;

static void wait_at_the_gate(void *argument)
{
    gate *const barrier = argument;

    (void)tb_platform_atomic_size_add(&barrier->arrived, 1);
    tb_mutex_lock(&barrier->lock);
    while (!barrier->is_open)
    {
        tb_condition_wait(&barrier->opened, &barrier->lock);
    }
    tb_mutex_unlock(&barrier->lock);
}

static void open_the_gate(gate *barrier)
{
    tb_mutex_lock(&barrier->lock);
    barrier->is_open = true;
    tb_condition_wake_all(&barrier->opened);
    tb_mutex_unlock(&barrier->lock);
}

/* Returns once `count` jobs are stopped at the gate, or false when they did
 * not get there. */
static bool have_arrived(gate *barrier, size_t count)
{
    const uint64_t start = tb_platform_clock_nanoseconds();

    while (tb_platform_atomic_size_load(&barrier->arrived) < count)
    {
        if (tb_platform_clock_nanoseconds() - start > PATIENCE_NANOSECONDS)
        {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Starting and stopping.
 * ------------------------------------------------------------------------- */

TB_TEST(jobs, a_scheduler_starts_the_workers_it_was_asked_for)
{
    const size_t counts[] = {1, 2, 3, 8, 33};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(counts[index]);

        TB_TEST_CONTEXT("%zu workers", counts[index]);
        TB_TEST_EXPECT_UINT(tb_jobs_worker_count(jobs), ==, counts[index]);
        tb_jobs_destroy(jobs);
    }
}

TB_TEST(jobs, a_scheduler_with_no_options_has_a_worker_for_every_processor_but_one)
{
    const tb_jobs_options nothing = {0};
    tb_jobs *const from_null = tb_jobs_create(NULL);
    tb_jobs *const from_zeros = tb_jobs_create(&nothing);
    tb_system_processors processors = {0};
    size_t expected = 1;

    /* One for each logical processor, less one for the thread that submits
     * and waits, and never none. */
    tb_system_get_processors(&processors);
    if (processors.logical_count > 2)
    {
        expected = processors.logical_count - 1;
    }
    TB_TEST_EXPECT_UINT(tb_jobs_worker_count(from_null), ==, expected);
    TB_TEST_EXPECT_UINT(tb_jobs_worker_count(from_zeros), ==, expected);
    tb_jobs_destroy(from_null);
    tb_jobs_destroy(from_zeros);
    /* Destroying nothing does nothing. */
    tb_jobs_destroy(NULL);
}

/* Schedulers are independent: two at once, each with its own jobs. */
TB_TEST(jobs, two_schedulers_run_their_own_jobs)
{
    tb_jobs *const first = create_with_workers(2);
    tb_jobs *const second = create_with_workers(2);
    tb_platform_atomic_size first_count = {0};
    tb_platform_atomic_size second_count = {0};

    for (size_t round = 0; round < 500; round++)
    {
        (void)tb_jobs_run(first, count_one, &first_count);
        (void)tb_jobs_run(second, count_one, &second_count);
        (void)tb_jobs_run(second, count_one, &second_count);
    }
    tb_jobs_wait_for_all(first);
    tb_jobs_wait_for_all(second);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&first_count), ==, 500);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&second_count), ==, 1000);
    tb_jobs_destroy(first);
    tb_jobs_destroy(second);
}

/* ---------------------------------------------------------------------------
 * Every job runs exactly once.
 * ------------------------------------------------------------------------- */

TB_TEST(jobs, a_job_runs_once_and_is_then_finished)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};
    const tb_job job = tb_jobs_run(jobs, count_one, &count);

    tb_jobs_wait(jobs, job);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, 1);
    TB_TEST_EXPECT(tb_jobs_is_finished(jobs, job));
    /* Waiting again, for a job that is long gone, returns at once. */
    tb_jobs_wait(jobs, job);
    tb_jobs_wait_for_all(jobs);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, 1);
    tb_jobs_destroy(jobs);
}

#define MANY_JOBS ((size_t)20000)

TB_TEST(jobs, every_job_runs_once_whatever_the_number_of_workers)
{
    const size_t counts[] = {1, 2, 8};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(counts[index]);
        tb_platform_atomic_size *const cells = tb_calloc(MANY_JOBS, sizeof(*cells));

        TB_TEST_CONTEXT("%zu workers", counts[index]);
        for (size_t job = 0; job < MANY_JOBS; job++)
        {
            (void)tb_jobs_run(jobs, count_one, &cells[job]);
        }
        tb_jobs_wait_for_all(jobs);
        TB_TEST_EXPECT_UINT(count_cells_that_are_not(cells, MANY_JOBS, 1), ==, 0);
        tb_jobs_destroy(jobs);
        tb_free(cells);
    }
}

#define SUBMITTER_COUNT 8
#define JOBS_PER_SUBMITTER ((size_t)5000)

typedef struct submitter
{
    tb_jobs *jobs;
    tb_platform_atomic_size *cells; /* JOBS_PER_SUBMITTER of them, this thread's own. */
} submitter;

static void submit_many(void *argument)
{
    const submitter *const self = argument;

    for (size_t job = 0; job < JOBS_PER_SUBMITTER; job++)
    {
        (void)tb_jobs_run(self->jobs, count_one, &self->cells[job]);
    }
}

TB_TEST(jobs, every_job_runs_once_when_many_threads_submit)
{
    tb_jobs *const jobs = create_with_workers(4);
    tb_platform_atomic_size *const cells =
        tb_calloc(SUBMITTER_COUNT * JOBS_PER_SUBMITTER, sizeof(*cells));
    submitter submitters[SUBMITTER_COUNT] = {0};
    tb_thread threads[SUBMITTER_COUNT] = {0};

    for (size_t index = 0; index < SUBMITTER_COUNT; index++)
    {
        submitters[index].jobs = jobs;
        submitters[index].cells = &cells[index * JOBS_PER_SUBMITTER];
        tb_thread_create(&threads[index], submit_many, &submitters[index]);
    }
    for (size_t index = 0; index < SUBMITTER_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    tb_jobs_wait_for_all(jobs);
    TB_TEST_EXPECT_UINT(count_cells_that_are_not(cells, SUBMITTER_COUNT * JOBS_PER_SUBMITTER, 1),
                        ==, 0);
    tb_jobs_destroy(jobs);
    tb_free(cells);
}

/* A tree of jobs: each submits two more, down to a depth. */
#define TREE_DEPTH 12
#define TREE_NODE_COUNT (((size_t)1 << (TREE_DEPTH + 1)) - 1)

typedef struct tree
{
    tb_jobs *jobs;
    tb_platform_atomic_size *visits; /* By node: times its job ran. */
    struct tree_node *nodes;
} tree;

typedef struct tree_node
{
    tree *whole;
    size_t index; /* The root is 0; the children of n are 2n + 1 and 2n + 2. */
} tree_node;

static void visit_and_branch(void *argument)
{
    const tree_node *const node = argument;
    tree *const whole = node->whole;
    const size_t first_child = node->index * 2 + 1;

    (void)tb_platform_atomic_size_add(&whole->visits[node->index], 1);
    if (first_child + 1 < TREE_NODE_COUNT)
    {
        (void)tb_jobs_run(whole->jobs, visit_and_branch, &whole->nodes[first_child]);
        (void)tb_jobs_run(whole->jobs, visit_and_branch, &whole->nodes[first_child + 1]);
    }
}

/* Runs the tree on a scheduler made with `options`, and returns how many
 * nodes were not visited exactly once. */
static size_t grow_a_tree(const tb_jobs_options *options, bool waits_before_destroying)
{
    tree whole = {0};
    size_t wrong_count = 0;

    whole.jobs = tb_jobs_create(options);
    whole.visits = tb_calloc(TREE_NODE_COUNT, sizeof(*whole.visits));
    whole.nodes = tb_calloc(TREE_NODE_COUNT, sizeof(*whole.nodes));
    for (size_t index = 0; index < TREE_NODE_COUNT; index++)
    {
        whole.nodes[index].whole = &whole;
        whole.nodes[index].index = index;
    }
    (void)tb_jobs_run(whole.jobs, visit_and_branch, &whole.nodes[0]);
    if (waits_before_destroying)
    {
        tb_jobs_wait_for_all(whole.jobs);
        wrong_count = count_cells_that_are_not(whole.visits, TREE_NODE_COUNT, 1);
    }
    tb_jobs_destroy(whole.jobs);
    if (!waits_before_destroying)
    {
        wrong_count = count_cells_that_are_not(whole.visits, TREE_NODE_COUNT, 1);
    }
    tb_free(whole.nodes);
    tb_free(whole.visits);
    return wrong_count;
}

TB_TEST(jobs, jobs_that_jobs_submit_each_run_once)
{
    const tb_jobs_options one_worker = {.worker_count = 1};
    const tb_jobs_options four_workers = {.worker_count = 4};

    TB_TEST_EXPECT_UINT(grow_a_tree(&one_worker, true), ==, 0);
    TB_TEST_EXPECT_UINT(grow_a_tree(&four_workers, true), ==, 0);
}

/* A worker keeps the jobs it makes in a queue of its own, and shares what
 * does not fit. With room for two, nearly every job of the tree overflows. */
TB_TEST(jobs, jobs_beyond_a_workers_own_queue_are_not_lost)
{
    const tb_jobs_options tiny_queues = {.worker_count = 4, .queue_capacity = 2};
    const tb_jobs_options one_slot = {.worker_count = 2, .queue_capacity = 1};

    TB_TEST_EXPECT_UINT(grow_a_tree(&tiny_queues, true), ==, 0);
    TB_TEST_EXPECT_UINT(grow_a_tree(&one_slot, true), ==, 0);
}

/* A meeting that needs everyone: each job that comes waits until all of them
 * have come. Jobs that ran one after another could never meet. */
typedef struct meeting
{
    size_t size;                     /* How many are expected. */
    tb_platform_atomic_size arrived; /* How many have come. */
    tb_platform_atomic_size gave_up; /* Jobs that waited in vain. */
} meeting;

static void come_to_the_meeting(void *argument)
{
    meeting *const all = argument;
    const uint64_t start = tb_platform_clock_nanoseconds();

    (void)tb_platform_atomic_size_add(&all->arrived, 1);
    while (tb_platform_atomic_size_load(&all->arrived) < all->size)
    {
        if (tb_platform_clock_nanoseconds() - start > PATIENCE_NANOSECONDS)
        {
            (void)tb_platform_atomic_size_add(&all->gave_up, 1);
            return;
        }
    }
}

/* As many jobs as there are workers, none of which can finish until all of
 * them are running. The case runs nothing itself, so each needs a worker of
 * its own. */
TB_TEST(jobs, jobs_run_at_the_same_time_on_every_worker)
{
    const size_t counts[] = {2, 4};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(counts[index]);
        meeting all = {.size = counts[index]};
        tb_job last = {0};

        tb_platform_atomic_size warm_up = {0};

        TB_TEST_CONTEXT("%zu workers", counts[index]);
        /* A job first, so that one worker is awake and looking for the
         * next, and takes the jobs that follow in one batch. It then has to
         * wake the others for them: a job that is ready wakes nobody while a
         * thread is looking. */
        TB_TEST_EXPECT(finishes_without_help(jobs, tb_jobs_run(jobs, count_one, &warm_up)));
        for (size_t job = 0; job < counts[index]; job++)
        {
            last = tb_jobs_run(jobs, come_to_the_meeting, &all);
        }
        TB_TEST_EXPECT(finishes_without_help(jobs, last));
        tb_jobs_wait_for_all(jobs);
        TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&all.gave_up), ==, 0);
        TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&all.arrived), ==, counts[index]);
        tb_jobs_destroy(jobs);
    }
}

/* ---------------------------------------------------------------------------
 * Stopping.
 * ------------------------------------------------------------------------- */

TB_TEST(jobs, destroying_a_scheduler_runs_every_job_that_is_outstanding)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};

    for (size_t job = 0; job < MANY_JOBS; job++)
    {
        (void)tb_jobs_run(jobs, count_one, &count);
    }
    tb_jobs_destroy(jobs);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, MANY_JOBS);
}

/* Jobs that are still submitting jobs when the scheduler is destroyed. */
TB_TEST(jobs, destroying_a_scheduler_runs_what_its_jobs_go_on_to_submit)
{
    const tb_jobs_options options = {.worker_count = 3};

    TB_TEST_EXPECT_UINT(grow_a_tree(&options, false), ==, 0);
}

/* ---------------------------------------------------------------------------
 * Dependencies.
 * ------------------------------------------------------------------------- */

#define GRAPH_JOB_COUNT ((size_t)3000)
#define GRAPH_DEPENDENCIES_MAX 3

typedef struct graph
{
    tb_platform_atomic_size *is_finished; /* By job: 1 once its function has returned. */
    tb_platform_atomic_size too_early;    /* Jobs that started before one they depend on ended. */
} graph;

typedef struct graph_node
{
    graph *whole;
    size_t index;
    size_t dependencies[GRAPH_DEPENDENCIES_MAX]; /* Indexes of earlier jobs. */
    size_t dependency_count;
} graph_node;

static void check_dependencies_and_finish(void *argument)
{
    const graph_node *const node = argument;
    graph *const whole = node->whole;

    for (size_t index = 0; index < node->dependency_count; index++)
    {
        if (tb_platform_atomic_size_load(&whole->is_finished[node->dependencies[index]]) == 0)
        {
            (void)tb_platform_atomic_size_add(&whole->too_early, 1);
        }
    }
    tb_platform_atomic_size_store(&whole->is_finished[node->index], 1);
}

/* Submits GRAPH_JOB_COUNT jobs, each of which depends on up to three that
 * were submitted before it, and returns how many started too early or never
 * finished. With `holds_them_back`, every job also depends on one that waits
 * at a gate, so that none has finished while the others are submitted and
 * every dependency is a real wait; without, jobs finish while later ones are
 * being made to depend on them. */
static size_t run_a_random_graph(size_t worker_count, bool holds_them_back)
{
    tb_jobs *const jobs = create_with_workers(worker_count);
    graph whole = {0};
    gate barrier = {0};
    graph_node *const nodes = tb_calloc(GRAPH_JOB_COUNT, sizeof(*nodes));
    tb_job *const handles = tb_calloc(GRAPH_JOB_COUNT, sizeof(*handles));
    tb_job held = {0};
    size_t wrong_count = 0;

    whole.is_finished = tb_calloc(GRAPH_JOB_COUNT, sizeof(*whole.is_finished));
    if (holds_them_back)
    {
        held = tb_jobs_run(jobs, wait_at_the_gate, &barrier);
    }
    for (size_t index = 0; index < GRAPH_JOB_COUNT; index++)
    {
        graph_node *const node = &nodes[index];
        tb_job needs[GRAPH_DEPENDENCIES_MAX + 1] = {0};
        tb_job_description description = {
            .function = check_dependencies_and_finish, .argument = node, .dependencies = needs};

        node->whole = &whole;
        node->index = index;
        if (index > 0)
        {
            node->dependency_count = (size_t)tb_test_random_below(GRAPH_DEPENDENCIES_MAX + 1);
        }
        for (size_t slot = 0; slot < node->dependency_count; slot++)
        {
            node->dependencies[slot] = (size_t)tb_test_random_below(index);
            needs[slot] = handles[node->dependencies[slot]];
        }
        needs[node->dependency_count] = held;
        description.dependency_count = node->dependency_count + 1;
        handles[index] = tb_jobs_submit(jobs, &description);
    }
    if (holds_them_back)
    {
        open_the_gate(&barrier);
    }
    tb_jobs_wait_for_all(jobs);
    wrong_count = tb_platform_atomic_size_load(&whole.too_early) +
                  count_cells_that_are_not(whole.is_finished, GRAPH_JOB_COUNT, 1);
    tb_jobs_destroy(jobs);
    tb_free(whole.is_finished);
    tb_free(handles);
    tb_free(nodes);
    return wrong_count;
}

TB_TEST(jobs, a_job_starts_only_after_the_jobs_it_depends_on_have_finished)
{
    TB_TEST_CONTEXT("every dependency unfinished when it is named");
    TB_TEST_EXPECT_UINT(run_a_random_graph(4, true), ==, 0);
    TB_TEST_EXPECT_UINT(run_a_random_graph(1, true), ==, 0);
    TB_TEST_CONTEXT("dependencies finishing while they are named");
    TB_TEST_EXPECT_UINT(run_a_random_graph(4, false), ==, 0);
    TB_TEST_EXPECT_UINT(run_a_random_graph(1, false), ==, 0);
}

TB_TEST(jobs, a_dependency_that_has_finished_or_names_no_job_is_not_waited_for)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};
    const tb_job finished = tb_jobs_run(jobs, count_one, &count);
    const tb_job none = {0};
    tb_job needs[4] = {0};
    tb_job_description description = {
        .function = count_one, .argument = &count, .dependencies = needs, .dependency_count = 4};

    tb_jobs_wait(jobs, finished);
    /* The same finished job twice, and no job twice. */
    needs[0] = finished;
    needs[1] = none;
    needs[2] = finished;
    needs[3] = none;
    TB_TEST_EXPECT(finishes_without_help(jobs, tb_jobs_submit(jobs, &description)));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, 2);
    tb_jobs_destroy(jobs);
}

/* A job may be named more than once among the dependencies of another. */
TB_TEST(jobs, depending_on_one_job_several_times_waits_for_it_once)
{
    tb_jobs *const jobs = create_with_workers(2);
    gate barrier = {0};
    tb_platform_atomic_size count = {0};
    const tb_job held = tb_jobs_run(jobs, wait_at_the_gate, &barrier);
    const tb_job needs[3] = {held, held, held};
    const tb_job_description description = {
        .function = count_one, .argument = &count, .dependencies = needs, .dependency_count = 3};
    const tb_job dependent = tb_jobs_submit(jobs, &description);
    const bool has_arrived = have_arrived(&barrier, 1);

    TB_TEST_EXPECT(!tb_jobs_is_finished(jobs, dependent));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, 0);
    open_the_gate(&barrier);
    tb_jobs_wait(jobs, dependent);
    TB_TEST_REQUIRE(has_arrived);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, 1);
    tb_jobs_destroy(jobs);
}

#define GROUP_SIZE 100

/* A job with no function is a way to wait for many jobs at once. */
TB_TEST(jobs, a_job_with_no_function_finishes_when_its_dependencies_have)
{
    tb_jobs *const jobs = create_with_workers(4);
    tb_platform_atomic_size cells[GROUP_SIZE] = {0};
    tb_job members[GROUP_SIZE] = {0};
    tb_job_description group = {.dependencies = members, .dependency_count = GROUP_SIZE};
    const tb_job_description alone = {0};

    for (size_t index = 0; index < GROUP_SIZE; index++)
    {
        members[index] = tb_jobs_run(jobs, count_one, &cells[index]);
    }
    tb_jobs_wait(jobs, tb_jobs_submit(jobs, &group));
    TB_TEST_EXPECT_UINT(count_cells_that_are_not(cells, GROUP_SIZE, 1), ==, 0);
    for (size_t index = 0; index < GROUP_SIZE; index++)
    {
        TB_TEST_EXPECT(tb_jobs_is_finished(jobs, members[index]));
    }
    /* With no dependencies either, it finishes as soon as it is taken. */
    tb_jobs_wait(jobs, tb_jobs_submit(jobs, &alone));
    tb_jobs_destroy(jobs);
}

/* A finished job's slot is given to a later job. A handle to the finished
 * one must go on meaning that job, which has finished, and never the job
 * that has its slot now.
 *
 * The one worker is held at a gate, so the case runs the first job itself by
 * waiting for it, and its slot goes back where the next job that the case
 * submits will take it from. That job waits for the gate too, so it has the
 * slot for as long as the case needs. */
TB_TEST(jobs, a_handle_to_a_finished_job_is_not_a_handle_to_the_job_in_its_slot)
{
    tb_jobs *const jobs = create_with_workers(1);
    gate barrier = {0};
    tb_platform_atomic_size later_count = {0};
    tb_platform_atomic_size dependent_count = {0};
    const tb_job held = tb_jobs_run(jobs, wait_at_the_gate, &barrier);
    const bool has_arrived = have_arrived(&barrier, 1);
    const tb_job early = tb_jobs_run(jobs, do_nothing, NULL);
    tb_job_description description = {
        .function = count_one, .argument = &later_count, .dependency_count = 1};
    tb_job later = {0};

    tb_jobs_wait(jobs, early);
    description.dependencies = &held;
    later = tb_jobs_submit(jobs, &description);
    TB_TEST_EXPECT_UINT(later.slot, ==, early.slot);
    TB_TEST_EXPECT_UINT(later.generation, !=, early.generation);
    TB_TEST_EXPECT(tb_jobs_is_finished(jobs, early));
    TB_TEST_EXPECT(!tb_jobs_is_finished(jobs, later));
    /* Returns at once, though the job in the slot cannot run yet. */
    tb_jobs_wait(jobs, early);
    TB_TEST_EXPECT(!tb_jobs_is_finished(jobs, later));
    /* A job that depends on the finished one is ready, and does not wait
     * for the job in the slot: the case runs it by waiting for it. */
    description.argument = &dependent_count;
    description.dependencies = &early;
    tb_jobs_wait(jobs, tb_jobs_submit(jobs, &description));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&dependent_count), ==, 1);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&later_count), ==, 0);
    TB_TEST_EXPECT(!tb_jobs_is_finished(jobs, later));

    open_the_gate(&barrier);
    tb_jobs_wait(jobs, later);
    TB_TEST_REQUIRE(has_arrived);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&later_count), ==, 1);
    tb_jobs_destroy(jobs);
}

/* ---------------------------------------------------------------------------
 * Priorities.
 * ------------------------------------------------------------------------- */

#define BACKLOG_SIZE 50

typedef struct turn_taker
{
    tb_platform_atomic_size *next_turn; /* Shared: the turns taken so far. */
    size_t turn;                        /* The turn this job got, from 1. */
} turn_taker;

static void take_a_turn(void *argument)
{
    turn_taker *const self = argument;

    self->turn = tb_platform_atomic_size_add(self->next_turn, 1);
}

/* One worker, held at a gate while a backlog of background jobs and then one
 * foreground job are submitted. When the worker is let go it must take the
 * foreground job first. The case runs nothing itself until that is done. */
TB_TEST(jobs, a_foreground_job_runs_before_background_jobs_that_were_ready_first)
{
    tb_jobs *const jobs = create_with_workers(1);
    gate barrier = {0};
    tb_platform_atomic_size next_turn = {0};
    turn_taker background[BACKLOG_SIZE] = {0};
    turn_taker foreground = {.next_turn = &next_turn};
    tb_job_description description = {.function = take_a_turn,
                                      .priority = TB_JOB_PRIORITY_BACKGROUND};
    tb_job urgent = {0};
    bool has_arrived = false;
    bool has_finished = false;
    size_t earlier_count = 0;

    (void)tb_jobs_run(jobs, wait_at_the_gate, &barrier);
    has_arrived = have_arrived(&barrier, 1);
    for (size_t index = 0; index < BACKLOG_SIZE; index++)
    {
        background[index].next_turn = &next_turn;
        description.argument = &background[index];
        (void)tb_jobs_submit(jobs, &description);
    }
    description.argument = &foreground;
    description.priority = TB_JOB_PRIORITY_FOREGROUND;
    urgent = tb_jobs_submit(jobs, &description);

    open_the_gate(&barrier);
    has_finished = finishes_without_help(jobs, urgent);
    tb_jobs_wait_for_all(jobs);
    TB_TEST_REQUIRE(has_arrived);
    TB_TEST_REQUIRE(has_finished);
    TB_TEST_EXPECT_UINT(foreground.turn, ==, 1);
    for (size_t index = 0; index < BACKLOG_SIZE; index++)
    {
        if (background[index].turn < foreground.turn)
        {
            earlier_count++;
        }
    }
    TB_TEST_EXPECT_UINT(earlier_count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&next_turn), ==, BACKLOG_SIZE + 1);
    tb_jobs_destroy(jobs);
}

/* Background jobs are run when there is nothing else, by workers alone. */
TB_TEST(jobs, background_jobs_run_when_no_foreground_job_is_ready)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};
    const tb_job_description description = {
        .function = count_one, .argument = &count, .priority = TB_JOB_PRIORITY_BACKGROUND};
    tb_job last = {0};

    for (size_t index = 0; index < 1000; index++)
    {
        last = tb_jobs_submit(jobs, &description);
    }
    TB_TEST_EXPECT(finishes_without_help(jobs, last));
    tb_jobs_wait_for_all(jobs);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, 1000);
    tb_jobs_destroy(jobs);
}

/* ---------------------------------------------------------------------------
 * Waiting inside a job.
 * ------------------------------------------------------------------------- */

typedef struct nest
{
    tb_jobs *jobs;
    size_t depth;                        /* Levels below this one. */
    tb_platform_atomic_size *levels_run; /* Shared: jobs that have run. */
    tb_platform_atomic_size *unfinished; /* Shared: waits that returned too soon. */
} nest;

/* Submits a job like itself, one level down, waits for it, and checks that
 * it had finished when the wait returned. */
static void wait_for_a_job_of_its_own(void *argument)
{
    const nest *const self = argument;
    tb_platform_atomic_size child_ran = {0};
    nest child = *self;
    tb_job job = {0};

    (void)tb_platform_atomic_size_add(self->levels_run, 1);
    if (self->depth == 0)
    {
        return;
    }
    child.depth = self->depth - 1;
    if (child.depth == 0)
    {
        /* The last level only counts, in a cell that this level can see. */
        job = tb_jobs_run(self->jobs, count_one, &child_ran);
        tb_jobs_wait(self->jobs, job);
        (void)tb_platform_atomic_size_add(self->levels_run, 1);
        if (tb_platform_atomic_size_load(&child_ran) != 1)
        {
            (void)tb_platform_atomic_size_add(self->unfinished, 1);
        }
        return;
    }
    job = tb_jobs_run(self->jobs, wait_for_a_job_of_its_own, &child);
    tb_jobs_wait(self->jobs, job);
    if (!tb_jobs_is_finished(self->jobs, job))
    {
        (void)tb_platform_atomic_size_add(self->unfinished, 1);
    }
}

#define WAITING_PARENTS 16
#define NEST_DEPTH 40

/* More jobs wait than there are workers, so every worker is inside a wait at
 * once, and the jobs they wait for can only be run by threads that wait. */
TB_TEST(jobs, a_job_can_wait_for_jobs_it_submitted_with_every_worker_waiting)
{
    const size_t counts[] = {1, 2, 4};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(counts[index]);
        tb_platform_atomic_size levels_run = {0};
        tb_platform_atomic_size unfinished = {0};
        nest parents[WAITING_PARENTS] = {0};

        TB_TEST_CONTEXT("%zu workers", counts[index]);
        for (size_t parent = 0; parent < WAITING_PARENTS; parent++)
        {
            parents[parent].jobs = jobs;
            parents[parent].depth = 2;
            parents[parent].levels_run = &levels_run;
            parents[parent].unfinished = &unfinished;
            (void)tb_jobs_run(jobs, wait_for_a_job_of_its_own, &parents[parent]);
        }
        tb_jobs_wait_for_all(jobs);
        TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&unfinished), ==, 0);
        /* Each parent, its child, and the child's own child. */
        TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&levels_run), ==, WAITING_PARENTS * 3);
        tb_jobs_destroy(jobs);
    }
}

TB_TEST(jobs, waits_nest_as_deep_as_jobs_submit_jobs)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size levels_run = {0};
    tb_platform_atomic_size unfinished = {0};
    nest top = {
        .jobs = jobs, .depth = NEST_DEPTH, .levels_run = &levels_run, .unfinished = &unfinished};

    tb_jobs_wait(jobs, tb_jobs_run(jobs, wait_for_a_job_of_its_own, &top));
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&unfinished), ==, 0);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&levels_run), ==, NEST_DEPTH + 1);
    tb_jobs_destroy(jobs);
}

TB_TEST(jobs, waiting_for_no_job_returns_at_once)
{
    tb_jobs *const jobs = create_with_workers(1);
    gate barrier = {0};
    const tb_job none = {0};
    const tb_job held = tb_jobs_run(jobs, wait_at_the_gate, &barrier);
    const bool has_arrived = have_arrived(&barrier, 1);

    /* With the only worker stopped at the gate, a wait that waited for
     * anything would not come back. */
    tb_jobs_wait(jobs, none);
    TB_TEST_EXPECT(tb_jobs_is_finished(jobs, none));
    TB_TEST_EXPECT(!tb_jobs_is_finished(jobs, held));
    open_the_gate(&barrier);
    tb_jobs_wait(jobs, held);
    TB_TEST_REQUIRE(has_arrived);
    tb_jobs_destroy(jobs);
}

/* ---------------------------------------------------------------------------
 * Waking.
 * ------------------------------------------------------------------------- */

#define WAKE_ROUNDS ((size_t)3000)

/* One job at a time, with the workers idle in between, and the case running
 * nothing itself: a worker has to be woken for every one of them. A wake that
 * was lost would leave a job in a queue with every worker asleep. */
TB_TEST(jobs, a_worker_wakes_for_every_job)
{
    const size_t counts[] = {1, 2, 4};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(counts[index]);
        tb_platform_atomic_size count = {0};
        size_t lost_count = 0;

        TB_TEST_CONTEXT("%zu workers", counts[index]);
        for (size_t round = 0; round < WAKE_ROUNDS && lost_count == 0; round++)
        {
            if (!finishes_without_help(jobs, tb_jobs_run(jobs, count_one, &count)))
            {
                lost_count++;
            }
        }
        TB_TEST_EXPECT_UINT(lost_count, ==, 0);
        tb_jobs_wait_for_all(jobs);
        TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, WAKE_ROUNDS);
        tb_jobs_destroy(jobs);
    }
}

/* The same for a job that becomes ready when the job it depends on finishes,
 * which is a second place where a wake is owed. */
TB_TEST(jobs, a_worker_wakes_for_a_job_whose_dependency_finishes)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};
    size_t lost_count = 0;

    for (size_t round = 0; round < WAKE_ROUNDS && lost_count == 0; round++)
    {
        const tb_job first = tb_jobs_run(jobs, count_one, &count);
        const tb_job_description description = {.function = count_one,
                                                .argument = &count,
                                                .dependencies = &first,
                                                .dependency_count = 1};

        if (!finishes_without_help(jobs, tb_jobs_submit(jobs, &description)))
        {
            lost_count++;
        }
    }
    TB_TEST_EXPECT_UINT(lost_count, ==, 0);
    tb_jobs_wait_for_all(jobs);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, WAKE_ROUNDS * 2);
    tb_jobs_destroy(jobs);
}

typedef struct relay
{
    tb_jobs *jobs;
    meeting all;
    tb_job maker;                        /* The job that leaves the others. */
    tb_platform_atomic_size has_maker;   /* 1 once `maker` is set. */
    tb_platform_atomic_size has_started; /* 1 once the maker is running. */
} relay;

/* Long enough for the thread of the case to notice that the maker has
 * started and to begin waiting, and far shorter than a thread goes on
 * looking before it sleeps. */
#define HEAD_START (UINT64_C(20) * NANOSECONDS_PER_MICROSECOND)

#define RELAY_JOB_COUNT 3

/* Leaves three jobs that wait for this one, so that all three become ready
 * in the same moment, when it returns. Each of them needs the other two
 * running at the same time. */
static void leave_three_for_the_meeting(void *argument)
{
    relay *const shared = argument;
    tb_job_description description = {.function = come_to_the_meeting};

    while (tb_platform_atomic_size_load(&shared->has_maker) == 0)
    {
    }
    description.argument = &shared->all;
    description.dependencies = &shared->maker;
    description.dependency_count = 1;
    tb_platform_atomic_size_store(&shared->has_started, 1);
    pass_the_time(HEAD_START);
    for (size_t index = 0; index < RELAY_JOB_COUNT; index++)
    {
        (void)tb_jobs_submit(shared->jobs, &description);
    }
}

#define RELAY_ROUNDS 20

/* Jobs that become ready while a thread is looking for work wake nobody: the
 * thread that is looking will find them. Here three become ready at once on
 * one worker. That worker runs one, and the thread that is looking, which is
 * the case itself, takes another. The other workers are asleep, so unless
 * the case wakes one of them when it takes its job, the third waits for a
 * thread that is not coming.
 *
 * The case can take its job before the last of the three is ready, and then
 * that one wakes a worker by itself. So a scheduler that left out the wake
 * could pass a round, and the case runs many. */
TB_TEST(jobs, a_thread_that_takes_one_of_several_new_jobs_wakes_a_worker_for_the_rest)
{
    tb_jobs *const jobs = create_with_workers(3);
    size_t gave_up_count = 0;

    for (size_t round = 0; round < RELAY_ROUNDS && gave_up_count == 0; round++)
    {
        relay shared = {.jobs = jobs, .all = {.size = RELAY_JOB_COUNT}};

        pass_the_time(LONG_ENOUGH_TO_FALL_ASLEEP);
        shared.maker = tb_jobs_run(jobs, leave_three_for_the_meeting, &shared);
        tb_platform_atomic_size_store(&shared.has_maker, 1);
        /* Not before a worker has the maker, or the case would run it. */
        while (tb_platform_atomic_size_load(&shared.has_started) == 0)
        {
        }
        tb_jobs_wait_for_all(jobs);
        gave_up_count += tb_platform_atomic_size_load(&shared.all.gave_up);
    }
    TB_TEST_EXPECT_UINT(gave_up_count, ==, 0);
    tb_jobs_destroy(jobs);
}

#define SLEEP_ROUNDS 20

/* The workers are left idle until all of them are asleep, and then given one
 * job, which the case does not run itself. A job that woke nobody would
 * stay in its queue for ever. */
TB_TEST(jobs, a_sleeping_worker_is_woken_for_a_job)
{
    const size_t counts[] = {1, 3};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(counts); index++)
    {
        tb_jobs *const jobs = create_with_workers(counts[index]);
        tb_platform_atomic_size count = {0};
        size_t lost_count = 0;

        TB_TEST_CONTEXT("%zu workers", counts[index]);
        for (size_t round = 0; round < SLEEP_ROUNDS && lost_count == 0; round++)
        {
            pass_the_time(LONG_ENOUGH_TO_FALL_ASLEEP);
            if (!finishes_without_help(jobs, tb_jobs_run(jobs, count_one, &count)))
            {
                lost_count++;
            }
        }
        TB_TEST_EXPECT_UINT(lost_count, ==, 0);
        tb_jobs_destroy(jobs);
    }
}

#define FALLING_ASLEEP_ROUNDS ((size_t)4000)

/* The same, with the job arriving at every moment around the one at which
 * the workers fall asleep: while the last of them is still looking, while it
 * is deciding to sleep, and just after. A worker that did not look once more
 * before it slept would miss a job that arrived while it decided. */
TB_TEST(jobs, a_job_that_arrives_as_the_workers_fall_asleep_is_run)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};
    size_t lost_count = 0;

    for (size_t round = 0; round < FALLING_ASLEEP_ROUNDS && lost_count == 0; round++)
    {
        /* From no wait at all to three times as long as a worker looks. */
        pass_the_time((round % 300) * NANOSECONDS_PER_MICROSECOND);
        if (!finishes_without_help(jobs, tb_jobs_run(jobs, count_one, &count)))
        {
            lost_count++;
        }
    }
    TB_TEST_EXPECT_UINT(lost_count, ==, 0);
    tb_jobs_destroy(jobs);
}

/* A thread that waits for a job that takes a long time runs out of other
 * work and goes to sleep. It has to be woken when the job finishes, by the
 * worker that finished it. */
TB_TEST(jobs, a_thread_that_sleeps_while_it_waits_for_a_job_is_woken_when_it_finishes)
{
    tb_jobs *const jobs = create_with_workers(1);
    tb_platform_atomic_size count = {0};

    for (size_t round = 0; round < SLEEP_ROUNDS; round++)
    {
        const tb_job job = tb_jobs_run(jobs, take_a_long_time, &count);

        /* The one worker has taken the job before the case can. */
        pass_the_time(LONG_ENOUGH_TO_FALL_ASLEEP / 10);
        tb_jobs_wait(jobs, job);
        if (!tb_jobs_is_finished(jobs, job))
        {
            TB_TEST_FAIL("the wait returned before its job had finished, in round %zu", round);
        }
    }
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&count), ==, SLEEP_ROUNDS);
    tb_jobs_destroy(jobs);
}

/* The same for a thread that waits for every job. */
TB_TEST(jobs, a_thread_that_sleeps_while_it_waits_for_all_jobs_is_woken_by_the_last)
{
    tb_jobs *const jobs = create_with_workers(2);
    tb_platform_atomic_size count = {0};

    for (size_t round = 0; round < SLEEP_ROUNDS; round++)
    {
        (void)tb_jobs_run(jobs, take_a_long_time, &count);
        (void)tb_jobs_run(jobs, take_a_long_time, &count);
        /* Both workers have taken a job before the case can. */
        pass_the_time(LONG_ENOUGH_TO_FALL_ASLEEP / 10);
        tb_jobs_wait_for_all(jobs);
        if (tb_platform_atomic_size_load(&count) != (round + 1) * 2)
        {
            TB_TEST_FAIL("the wait returned with jobs unfinished, in round %zu", round);
        }
    }
    tb_jobs_destroy(jobs);
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void create_with_a_queue_of_three(void *context)
{
    const tb_jobs_options options = {.worker_count = 1, .queue_capacity = 3};

    (void)context;
    (void)tb_jobs_create(&options);
}

static void count_workers_of_null(void *context)
{
    (void)context;
    (void)tb_jobs_worker_count(NULL);
}

static void submit_to_null(void *context)
{
    const tb_job_description description = {.function = do_nothing};

    (void)context;
    (void)tb_jobs_submit(NULL, &description);
}

static void submit_no_description(void *context)
{
    (void)tb_jobs_submit(context, NULL);
}

static void submit_a_priority_that_is_none(void *context)
{
    tb_job_description description = {.function = do_nothing};

    description.priority = (tb_job_priority)7;
    (void)tb_jobs_submit(context, &description);
}

static void submit_counted_dependencies_that_are_missing(void *context)
{
    const tb_job_description description = {.function = do_nothing, .dependency_count = 2};

    (void)tb_jobs_submit(context, &description);
}

static void submit_a_dependency_from_nowhere(void *context)
{
    const tb_job stranger = {.slot = UINT32_MAX, .generation = 1};
    const tb_job_description description = {
        .function = do_nothing, .dependencies = &stranger, .dependency_count = 1};

    (void)tb_jobs_submit(context, &description);
}

static void run_no_function(void *context)
{
    (void)tb_jobs_run(context, NULL, NULL);
}

static void wait_with_null(void *context)
{
    const tb_job none = {0};

    (void)context;
    tb_jobs_wait(NULL, none);
}

static void wait_for_a_job_from_nowhere(void *context)
{
    const tb_job stranger = {.slot = UINT32_MAX, .generation = 1};

    tb_jobs_wait(context, stranger);
}

static void ask_about_a_job_from_nowhere(void *context)
{
    const tb_job stranger = {.slot = UINT32_MAX, .generation = 1};

    (void)tb_jobs_is_finished(context, stranger);
}

static void wait_for_all_of_null(void *context)
{
    (void)context;
    tb_jobs_wait_for_all(NULL);
}

TB_TEST(jobs, bad_arguments_violate_the_contract)
{
    tb_jobs *jobs = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    jobs = create_with_workers(1);
    TB_TEST_EXPECT_TRAP(create_with_a_queue_of_three, NULL, "the queue capacity is a power of two");
    TB_TEST_EXPECT_TRAP(count_workers_of_null, NULL, "jobs != NULL");
    TB_TEST_EXPECT_TRAP(submit_to_null, NULL, "jobs != NULL");
    TB_TEST_EXPECT_TRAP(submit_no_description, jobs, "description != NULL");
    TB_TEST_EXPECT_TRAP(submit_a_priority_that_is_none, jobs,
                        "the priority is one of tb_job_priority");
    TB_TEST_EXPECT_TRAP(submit_counted_dependencies_that_are_missing, jobs,
                        "dependencies are given when they are counted");
    TB_TEST_EXPECT_TRAP(submit_a_dependency_from_nowhere, jobs,
                        "every dependency comes from this scheduler");
    TB_TEST_EXPECT_TRAP(run_no_function, jobs, "function != NULL");
    TB_TEST_EXPECT_TRAP(wait_with_null, NULL, "jobs != NULL");
    TB_TEST_EXPECT_TRAP(wait_for_a_job_from_nowhere, jobs, "the job comes from this scheduler");
    TB_TEST_EXPECT_TRAP(ask_about_a_job_from_nowhere, jobs, "the job comes from this scheduler");
    TB_TEST_EXPECT_TRAP(wait_for_all_of_null, NULL, "jobs != NULL");
    /* The refused calls left the scheduler as it was. */
    tb_jobs_wait_for_all(jobs);
    tb_jobs_destroy(jobs);
}
