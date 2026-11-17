/* What a job costs beyond the work it does: one job submitted and waited
 * for, a batch submitted by one thread and run by all, a tree of jobs that
 * submit jobs, which is the pattern the scheduler is built for, and a chain
 * in which each job waits for the one before. Every job does the same small
 * thing, an atomic addition, so the numbers are nearly all scheduler. */
#include "tb_bench.h"

#include "badger/jobs.h"
#include "badger/platform.h"

#include <stddef.h>
#include <stdint.h>

/* Worker threads of the scheduler that is measured. With the thread that
 * submits and waits, which runs jobs too, eight threads are at work, as in
 * the other cases that run on several threads. */
#define JOBS_WORKERS ((size_t)7)

#define JOBS_ONE_AT_A_TIME ((size_t)100000)
#define JOBS_BATCH ((size_t)200000)
#define JOBS_CHAIN ((size_t)100000)

/* A tree in which every job submits two more: 2^17 - 1 jobs. */
#define JOBS_TREE_DEPTH_FULL ((size_t)16)
#define JOBS_TREE_DEPTH_QUICK ((size_t)6)

typedef struct jobs_state
{
    tb_jobs *jobs;
    tb_platform_atomic_size count; /* What every job adds to. */
    size_t operations;             /* Jobs that one run submits. */
    size_t tree_depth;
} jobs_state;

static void add_one(void *argument)
{
    (void)tb_platform_atomic_size_add(argument, 1);
}

static void keep_count(void *context)
{
    jobs_state *const state = context;

    tb_bench_keep(tb_platform_atomic_size_load(&state->count));
}

/* The work alone, with no scheduler: what the others are compared with. */
static void run_directly(void *context)
{
    jobs_state *const state = context;

    for (size_t index = 0; index < state->operations; index++)
    {
        add_one(&state->count);
    }
}

static void run_one_at_a_time(void *context)
{
    jobs_state *const state = context;

    for (size_t index = 0; index < state->operations; index++)
    {
        tb_jobs_wait(state->jobs, tb_jobs_run(state->jobs, add_one, &state->count));
    }
}

static void run_a_batch(void *context)
{
    jobs_state *const state = context;

    for (size_t index = 0; index < state->operations; index++)
    {
        (void)tb_jobs_run(state->jobs, add_one, &state->count);
    }
    tb_jobs_wait_for_all(state->jobs);
}

static void run_a_chain(void *context)
{
    jobs_state *const state = context;
    tb_job previous = {0};

    for (size_t index = 0; index < state->operations; index++)
    {
        const tb_job_description description = {.function = add_one,
                                                .argument = &state->count,
                                                .dependencies = &previous,
                                                .dependency_count = 1};

        previous = tb_jobs_submit(state->jobs, &description);
    }
    tb_jobs_wait(state->jobs, previous);
}

typedef struct branch
{
    jobs_state *state;
    size_t depth; /* Levels below this job. */
} branch;

/* One for each depth, so that a job can name its children's without
 * allocating: every job of one depth shares one. */
static branch jobs_branches[JOBS_TREE_DEPTH_FULL + 1] = {0};

static void add_one_and_branch(void *argument)
{
    const branch *const self = argument;

    add_one(&self->state->count);
    if (self->depth > 0)
    {
        (void)tb_jobs_run(self->state->jobs, add_one_and_branch, &jobs_branches[self->depth - 1]);
        (void)tb_jobs_run(self->state->jobs, add_one_and_branch, &jobs_branches[self->depth - 1]);
    }
}

static void run_a_tree(void *context)
{
    jobs_state *const state = context;

    for (size_t depth = 0; depth <= state->tree_depth; depth++)
    {
        jobs_branches[depth].state = state;
        jobs_branches[depth].depth = depth;
    }
    (void)tb_jobs_run(state->jobs, add_one_and_branch, &jobs_branches[state->tree_depth]);
    tb_jobs_wait_for_all(state->jobs);
}

void tb_bench_jobs(tb_bench *bench)
{
    const tb_jobs_options options = {.worker_count = JOBS_WORKERS};
    jobs_state one = {0};
    jobs_state batch = {0};
    jobs_state tree = {0};
    jobs_state chain = {0};
    tb_bench_case alone[2] = {
        {.name = "add to an atomic size, the work of every job below",
         .variant = "no scheduler",
         .run = run_directly,
         .teardown = keep_count},
        {.name = "submit a job and wait for it",
         .variant = "badger",
         .run = run_one_at_a_time,
         .teardown = keep_count},
    };
    tb_bench_case together[3] = {
        {.name = "submit a batch of jobs from one thread, run by 8",
         .variant = "badger",
         .run = run_a_batch,
         .teardown = keep_count},
        {.name = "a tree of jobs that each submit two more, run by 8 threads",
         .variant = "badger",
         .run = run_a_tree,
         .teardown = keep_count},
        {.name = "a chain of jobs that each wait for the one before",
         .variant = "badger",
         .run = run_a_chain,
         .teardown = keep_count},
    };

    if (!tb_bench_wants(bench, "jobs"))
    {
        return;
    }
    one.jobs = tb_jobs_create(&options);
    batch.jobs = one.jobs;
    tree.jobs = one.jobs;
    chain.jobs = one.jobs;
    one.operations = tb_bench_size(bench, JOBS_ONE_AT_A_TIME);
    batch.operations = tb_bench_size(bench, JOBS_BATCH);
    chain.operations = tb_bench_size(bench, JOBS_CHAIN);
    tree.tree_depth = JOBS_TREE_DEPTH_FULL;
    if (tb_bench_size(bench, JOBS_BATCH) != JOBS_BATCH)
    {
        tree.tree_depth = JOBS_TREE_DEPTH_QUICK;
    }
    tree.operations = ((size_t)1 << (tree.tree_depth + 1)) - 1;

    alone[0].operations = (double)one.operations;
    alone[0].context = &one;
    alone[1].operations = (double)one.operations;
    alone[1].context = &one;
    together[0].operations = (double)batch.operations;
    together[0].context = &batch;
    together[1].operations = (double)tree.operations;
    together[1].context = &tree;
    together[2].operations = (double)chain.operations;
    together[2].context = &chain;
    tb_bench_measure(bench, "jobs", alone, 2);
    tb_bench_measure(bench, "jobs", together, 3);
    tb_jobs_destroy(one.jobs);
}
