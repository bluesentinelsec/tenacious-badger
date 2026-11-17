#include "badger/jobs.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/size.h"
#include "badger/system.h"
#include "badger/thread.h"
#include "deque.h"

/* How the scheduler is put together.
 *
 * A job lives in a slot from the time it is submitted until it has finished.
 * Slots are made in blocks that never move, so a pointer to one stays good,
 * and a finished job's slot is used again for a later job. Each use has its
 * own generation, which is how a handle to a finished job is told from a
 * handle to the job that has the slot now.
 *
 * A job that is ready to run is in one of three kinds of place:
 *
 * - the queue of the worker that made it ready, which that worker pushes to
 *   and pops from without a lock, and which idle threads steal from;
 * - a queue that all threads share, behind a lock, for jobs made ready by a
 *   thread that is not a worker, and for what a worker's own queue has no
 *   room for;
 * - nowhere, while it waits for the jobs it depends on. Each of those keeps a
 *   list of the jobs that wait for it, and makes them ready as it finishes.
 *
 * There is one of each queue for each priority, and a thread looks through
 * all the foreground ones before any background one.
 *
 * A thread with nothing to run sleeps on one condition variable, whether it
 * is a worker or a thread that waits for a job. It is woken when a job
 * becomes ready, and when a job finishes while someone waits for one. */

#define TB_JOBS_PRIORITY_COUNT ((size_t)2)

#define TB_JOBS_QUEUE_CAPACITY_DEFAULT ((size_t)256)

/* Slots and edges are made this many at a time, in blocks that are found
 * through a table of fixed size, which is what bounds how many unfinished
 * jobs there can be: 1,048,576, and four times as many dependencies. */
#define TB_JOBS_SLOTS_PER_BLOCK ((uint32_t)1024)
#define TB_JOBS_SLOT_BLOCKS_MAX ((uint32_t)1024)
#define TB_JOBS_EDGES_PER_BLOCK ((uint32_t)1024)
#define TB_JOBS_EDGE_BLOCKS_MAX ((uint32_t)4096)

/* Keeps what one worker writes away from what its neighbor reads: twice the
 * usual size of a cache line, which is the size of one on Apple processors. */
#define TB_JOBS_CACHE_LINE_SIZE 128

typedef struct tb_job_slot tb_job_slot;

/* A job, from submission until it has finished. A slot and an edge are named
 * by an identifier, which is one more than its index, so that zero is none. */
struct tb_job_slot
{
    tb_job_function *function;
    void *argument;
    /* The next job in a shared queue. Guarded by that queue's lock. */
    tb_job_slot *next_queued;
    /* Guards `first_edge`, and the moment at which `generation` changes. */
    tb_mutex lock;
    /* Which use of the slot this is. It changes when the job finishes. */
    tb_platform_atomic_u32 generation;
    /* Dependencies that have not finished, and one more while the job is
     * being submitted. The job is ready when it reaches zero. */
    tb_platform_atomic_size pending;
    /* The first edge to a job that waits for this one. */
    uint32_t first_edge;
    /* The next free slot. Guarded by the lock of the pools. */
    uint32_t next_free;
    /* This slot. */
    uint32_t id;
    tb_job_priority priority;
};

/* One job waiting for another. */
typedef struct tb_job_edge
{
    /* The slot of the job that waits. */
    uint32_t dependent;
    /* The next edge of the job that is waited for, or the next free edge. */
    uint32_t next;
} tb_job_edge;

/* Jobs that are ready, in the order they became so. */
typedef struct tb_job_shared_queue
{
    tb_mutex lock; /* Guards `first` and `last`. */
    tb_job_slot *first;
    tb_job_slot *last;
    /* How many there are, for a thread that only wants to know whether to
     * take the lock. */
    tb_platform_atomic_size count;
} tb_job_shared_queue;

typedef struct tb_jobs_worker
{
    tb_jobs *jobs;
    tb_thread thread;
    /* Its own ready jobs, for each priority. */
    tb_job_deque queues[TB_JOBS_PRIORITY_COUNT];
    /* Where it looks first when it steals. Only its own thread uses it. */
    size_t next_victim;
    char name[TB_THREAD_NAME_CAPACITY];
    char padding[TB_JOBS_CACHE_LINE_SIZE];
} tb_jobs_worker;

struct tb_jobs
{
    tb_jobs_worker *workers;
    size_t worker_count;
    /* The storage of every worker's queues, in one block. */
    tb_platform_atomic_pointer *queue_storage;
    tb_job_shared_queue shared[TB_JOBS_PRIORITY_COUNT];

    /* Threads with nothing to run sleep here. */
    tb_mutex sleep_lock;
    tb_condition wake;
    /* Threads that are asleep on `wake`, or about to be. */
    tb_platform_atomic_size sleeper_count;
    /* Those of them that wait for a job to finish, not only for work. */
    tb_platform_atomic_size waiter_count;
    /* Jobs submitted and not yet finished. */
    tb_platform_atomic_size unfinished_count;
    /* Not zero once the workers are to end. */
    tb_platform_atomic_u32 is_stopping;
    /* Where a thread that is not a worker looks first when it steals. */
    tb_platform_atomic_size next_victim;

    /* Guards the free lists, and the making of new blocks. */
    tb_mutex pool_lock;
    tb_platform_atomic_pointer slot_blocks[TB_JOBS_SLOT_BLOCKS_MAX];
    tb_platform_atomic_u32 slot_count; /* Slots ever made. */
    uint32_t free_slot;
    tb_platform_atomic_pointer edge_blocks[TB_JOBS_EDGE_BLOCKS_MAX];
    uint32_t edge_count; /* Edges ever made. */
    uint32_t free_edge;
};

TB_STATIC_ASSERT((size_t)TB_JOB_PRIORITY_BACKGROUND + 1 == TB_JOBS_PRIORITY_COUNT,
                 "a priority is the index of its queues");

/* The worker that the calling thread is, of whichever scheduler, or NULL. */
static _Thread_local tb_jobs_worker *tb_jobs_this_worker = NULL;

static _Noreturn void tb_jobs_fail(const char *diagnostic)
{
    (void)tb_fmt_print_error("%s", diagnostic);
    (void)tb_fmt_flush();
    tb_platform_abort();
}

static bool tb_jobs_is_priority(tb_job_priority priority)
{
    return priority == TB_JOB_PRIORITY_FOREGROUND || priority == TB_JOB_PRIORITY_BACKGROUND;
}

/* The calling thread as a worker of `jobs`, or NULL when it is not one. */
static tb_jobs_worker *tb_jobs_worker_here(const tb_jobs *jobs)
{
    tb_jobs_worker *const worker = tb_jobs_this_worker;

    if (worker != NULL && worker->jobs == jobs)
    {
        return worker;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Slots and edges.
 * ------------------------------------------------------------------------- */

static tb_job_slot *tb_jobs_slot(tb_jobs *jobs, uint32_t id)
{
    const uint32_t index = id - 1;
    tb_job_slot *const block =
        tb_platform_atomic_pointer_load(&jobs->slot_blocks[index / TB_JOBS_SLOTS_PER_BLOCK]);

    return &block[index % TB_JOBS_SLOTS_PER_BLOCK];
}

static tb_job_edge *tb_jobs_edge(tb_jobs *jobs, uint32_t id)
{
    const uint32_t index = id - 1;
    tb_job_edge *const block =
        tb_platform_atomic_pointer_load(&jobs->edge_blocks[index / TB_JOBS_EDGES_PER_BLOCK]);

    return &block[index % TB_JOBS_EDGES_PER_BLOCK];
}

/* Whether `id` names a slot that this scheduler has made. */
static bool tb_jobs_has_slot(tb_jobs *jobs, uint32_t id)
{
    return id != 0 && id <= tb_platform_atomic_u32_load(&jobs->slot_count);
}

/* A slot for a new job: one that a finished job gave back, or a new one. */
static tb_job_slot *tb_jobs_take_slot(tb_jobs *jobs)
{
    tb_job_slot *slot = NULL;
    uint32_t made = 0;

    tb_mutex_lock(&jobs->pool_lock);
    if (jobs->free_slot != 0)
    {
        slot = tb_jobs_slot(jobs, jobs->free_slot);
        jobs->free_slot = slot->next_free;
        tb_mutex_unlock(&jobs->pool_lock);
        return slot;
    }
    made = tb_platform_atomic_u32_load(&jobs->slot_count);
    if (made == TB_JOBS_SLOT_BLOCKS_MAX * TB_JOBS_SLOTS_PER_BLOCK)
    {
        tb_jobs_fail("jobs: more than 1,048,576 jobs are unfinished at once\n");
    }
    if (made % TB_JOBS_SLOTS_PER_BLOCK == 0)
    {
        /* Zeroed: an unlocked lock, no edges, and the first generation. */
        tb_platform_atomic_pointer_store(&jobs->slot_blocks[made / TB_JOBS_SLOTS_PER_BLOCK],
                                         tb_calloc(TB_JOBS_SLOTS_PER_BLOCK, sizeof(tb_job_slot)));
    }
    /* Counted once its block is in place: a slot that has an identifier can
     * be found. */
    tb_platform_atomic_u32_store(&jobs->slot_count, made + 1);
    slot = tb_jobs_slot(jobs, made + 1);
    slot->id = made + 1;
    tb_mutex_unlock(&jobs->pool_lock);
    return slot;
}

static uint32_t tb_jobs_take_edge(tb_jobs *jobs)
{
    uint32_t id = 0;

    tb_mutex_lock(&jobs->pool_lock);
    if (jobs->free_edge != 0)
    {
        id = jobs->free_edge;
        jobs->free_edge = tb_jobs_edge(jobs, id)->next;
        tb_mutex_unlock(&jobs->pool_lock);
        return id;
    }
    if (jobs->edge_count == TB_JOBS_EDGE_BLOCKS_MAX * TB_JOBS_EDGES_PER_BLOCK)
    {
        tb_jobs_fail("jobs: more than 4,194,304 dependencies are unfinished at once\n");
    }
    if (jobs->edge_count % TB_JOBS_EDGES_PER_BLOCK == 0)
    {
        tb_platform_atomic_pointer_store(
            &jobs->edge_blocks[jobs->edge_count / TB_JOBS_EDGES_PER_BLOCK],
            tb_calloc(TB_JOBS_EDGES_PER_BLOCK, sizeof(tb_job_edge)));
    }
    jobs->edge_count++;
    id = jobs->edge_count;
    tb_mutex_unlock(&jobs->pool_lock);
    return id;
}

/* Gives back a list of edges, from `first` to `last` by their `next`, or
 * nothing when `first` is zero. */
static void tb_jobs_give_back_edges(tb_jobs *jobs, uint32_t first, uint32_t last)
{
    if (first == 0)
    {
        return;
    }
    tb_mutex_lock(&jobs->pool_lock);
    tb_jobs_edge(jobs, last)->next = jobs->free_edge;
    jobs->free_edge = first;
    tb_mutex_unlock(&jobs->pool_lock);
}

static void tb_jobs_give_back_slot(tb_jobs *jobs, tb_job_slot *slot)
{
    tb_mutex_lock(&jobs->pool_lock);
    slot->next_free = jobs->free_slot;
    jobs->free_slot = slot->id;
    tb_mutex_unlock(&jobs->pool_lock);
}

/* ---------------------------------------------------------------------------
 * Sleeping and waking.
 *
 * A thread that is about to sleep counts itself, and only then looks once
 * more for a reason not to. A thread that gives it such a reason, a job that
 * is ready or one that has finished, does so first, and only then looks at
 * the count. Whichever of the two looks second sees what the other did: the
 * sleeper stays awake, or the waker takes the lock and wakes it. The sleeper
 * holds the lock from before it counts itself until it is asleep, so a wake
 * cannot fall between its last look and its sleep.
 * ------------------------------------------------------------------------- */

/* Why a thread is looking for work, which is also when it should stop. */
typedef struct tb_jobs_reason
{
    /* A worker: until the scheduler stops. */
    bool is_worker;
    /* Until nothing is unfinished. */
    bool is_for_all;
    /* Otherwise: until the job that had `generation` of `slot` finishes. */
    tb_job_slot *slot;
    uint32_t generation;
} tb_jobs_reason;

static bool tb_jobs_reason_has_ended(tb_jobs *jobs, const tb_jobs_reason *reason)
{
    if (reason->is_worker)
    {
        return tb_platform_atomic_u32_load(&jobs->is_stopping) != 0;
    }
    if (reason->is_for_all)
    {
        return tb_platform_atomic_size_load(&jobs->unfinished_count) == 0;
    }
    return tb_platform_atomic_u32_load(&reason->slot->generation) != reason->generation;
}

/* Whether any queue holds a job. A moment's reading. */
static bool tb_jobs_has_ready_job(tb_jobs *jobs)
{
    for (size_t priority = 0; priority < TB_JOBS_PRIORITY_COUNT; priority++)
    {
        if (tb_platform_atomic_size_load(&jobs->shared[priority].count) != 0)
        {
            return true;
        }
        for (size_t index = 0; index < jobs->worker_count; index++)
        {
            if (tb_job_deque_count(&jobs->workers[index].queues[priority]) != 0)
            {
                return true;
            }
        }
    }
    return false;
}

/* Sleeps until woken, unless there is a job to run or the reason has ended.
 * It may return with neither true; the caller looks again. */
static void tb_jobs_sleep(tb_jobs *jobs, const tb_jobs_reason *reason)
{
    tb_mutex_lock(&jobs->sleep_lock);
    (void)tb_platform_atomic_size_add(&jobs->sleeper_count, 1);
    if (!reason->is_worker)
    {
        (void)tb_platform_atomic_size_add(&jobs->waiter_count, 1);
    }
    if (!tb_jobs_has_ready_job(jobs) && !tb_jobs_reason_has_ended(jobs, reason))
    {
        tb_condition_wait(&jobs->wake, &jobs->sleep_lock);
    }
    if (!reason->is_worker)
    {
        (void)tb_platform_atomic_size_subtract(&jobs->waiter_count, 1);
    }
    (void)tb_platform_atomic_size_subtract(&jobs->sleeper_count, 1);
    tb_mutex_unlock(&jobs->sleep_lock);
}

/* Call after a job has been put in a queue. */
static void tb_jobs_wake_one(tb_jobs *jobs)
{
    if (tb_platform_atomic_size_load(&jobs->sleeper_count) == 0)
    {
        return;
    }
    tb_mutex_lock(&jobs->sleep_lock);
    tb_condition_wake_one(&jobs->wake);
    tb_mutex_unlock(&jobs->sleep_lock);
}

/* Call after whatever the sleepers are to notice has been done. */
static void tb_jobs_wake_all(tb_jobs *jobs)
{
    if (tb_platform_atomic_size_load(&jobs->sleeper_count) == 0)
    {
        return;
    }
    tb_mutex_lock(&jobs->sleep_lock);
    tb_condition_wake_all(&jobs->wake);
    tb_mutex_unlock(&jobs->sleep_lock);
}

/* ---------------------------------------------------------------------------
 * Ready jobs.
 * ------------------------------------------------------------------------- */

static void tb_jobs_share(tb_job_shared_queue *queue, tb_job_slot *slot)
{
    tb_mutex_lock(&queue->lock);
    slot->next_queued = NULL;
    if (queue->last != NULL)
    {
        queue->last->next_queued = slot;
    }
    else
    {
        queue->first = slot;
    }
    queue->last = slot;
    (void)tb_platform_atomic_size_add(&queue->count, 1);
    tb_mutex_unlock(&queue->lock);
}

static tb_job_slot *tb_jobs_take_shared(tb_job_shared_queue *queue)
{
    tb_job_slot *slot = NULL;

    if (tb_platform_atomic_size_load(&queue->count) == 0)
    {
        return NULL;
    }
    tb_mutex_lock(&queue->lock);
    slot = queue->first;
    if (slot != NULL)
    {
        queue->first = slot->next_queued;
        if (queue->first == NULL)
        {
            queue->last = NULL;
        }
        (void)tb_platform_atomic_size_subtract(&queue->count, 1);
    }
    tb_mutex_unlock(&queue->lock);
    return slot;
}

/* Puts a job where a thread will find it, and wakes one that is asleep. A
 * worker keeps the job in its own queue while there is room. */
static void tb_jobs_make_ready(tb_jobs *jobs, tb_job_slot *slot)
{
    tb_jobs_worker *const worker = tb_jobs_worker_here(jobs);
    bool is_queued = false;

    if (worker != NULL)
    {
        is_queued = tb_job_deque_push(&worker->queues[slot->priority], slot);
    }
    if (!is_queued)
    {
        tb_jobs_share(&jobs->shared[slot->priority], slot);
    }
    tb_jobs_wake_one(jobs);
}

/* Takes a job from another worker's queue. `worker` is the calling thread
 * as a worker, or NULL. */
static tb_job_slot *tb_jobs_steal(tb_jobs *jobs, tb_jobs_worker *worker, size_t priority)
{
    size_t start = 0;

    /* A different victim first each time, so that thieves spread out. */
    if (worker != NULL)
    {
        start = worker->next_victim;
        worker->next_victim++;
    }
    else
    {
        start = tb_platform_atomic_size_add(&jobs->next_victim, 1);
    }
    for (size_t offset = 0; offset < jobs->worker_count; offset++)
    {
        tb_jobs_worker *const victim = &jobs->workers[(start + offset) % jobs->worker_count];
        tb_job_slot *slot = NULL;

        if (victim == worker)
        {
            continue;
        }
        slot = tb_job_deque_steal(&victim->queues[priority]);
        if (slot != NULL)
        {
            return slot;
        }
    }
    return NULL;
}

/* The next job for the calling thread, or NULL when it found none: every
 * foreground job before any background one, and at each priority its own
 * queue, then the shared one, then the other workers'. */
static tb_job_slot *tb_jobs_find(tb_jobs *jobs, tb_jobs_worker *worker)
{
    for (size_t priority = 0; priority < TB_JOBS_PRIORITY_COUNT; priority++)
    {
        tb_job_slot *slot = NULL;

        if (worker != NULL)
        {
            slot = tb_job_deque_pop(&worker->queues[priority]);
            if (slot != NULL)
            {
                return slot;
            }
        }
        slot = tb_jobs_take_shared(&jobs->shared[priority]);
        if (slot != NULL)
        {
            return slot;
        }
        slot = tb_jobs_steal(jobs, worker, priority);
        if (slot != NULL)
        {
            return slot;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Running a job.
 * ------------------------------------------------------------------------- */

/* One of the things a job waited for is done. */
static void tb_jobs_count_down(tb_jobs *jobs, tb_job_slot *slot)
{
    if (tb_platform_atomic_size_subtract(&slot->pending, 1) == 0)
    {
        tb_jobs_make_ready(jobs, slot);
    }
}

static void tb_jobs_run_slot(tb_jobs *jobs, tb_job_slot *slot)
{
    uint32_t first_edge = 0;
    uint32_t last_edge = 0;
    uint32_t edge_id = 0;

    if (slot->function != NULL)
    {
        slot->function(slot->argument);
    }

    /* Finished. From here a handle to the job is out of date, and under the
     * same lock the list of jobs that wait for it is closed: a job that
     * comes to depend on it now sees the new generation and does not wait. */
    tb_mutex_lock(&slot->lock);
    tb_platform_atomic_u32_store(&slot->generation,
                                 tb_platform_atomic_u32_load(&slot->generation) + 1);
    first_edge = slot->first_edge;
    slot->first_edge = 0;
    tb_mutex_unlock(&slot->lock);

    edge_id = first_edge;
    while (edge_id != 0)
    {
        const tb_job_edge *const edge = tb_jobs_edge(jobs, edge_id);

        tb_jobs_count_down(jobs, tb_jobs_slot(jobs, edge->dependent));
        last_edge = edge_id;
        edge_id = edge->next;
    }
    tb_jobs_give_back_edges(jobs, first_edge, last_edge);
    tb_jobs_give_back_slot(jobs, slot);

    /* Counted as finished after the jobs that waited for it were made
     * ready, so that the count does not touch zero in between. A thread that
     * waits for a job to finish is woken to look at the one it waits for. */
    if (tb_platform_atomic_size_subtract(&jobs->unfinished_count, 1) == 0 ||
        tb_platform_atomic_size_load(&jobs->waiter_count) != 0)
    {
        tb_jobs_wake_all(jobs);
    }
}

/* Runs jobs on the calling thread until the reason has ended, and sleeps
 * while there are none. */
static void tb_jobs_work_until(tb_jobs *jobs, tb_jobs_worker *worker, const tb_jobs_reason *reason)
{
    while (!tb_jobs_reason_has_ended(jobs, reason))
    {
        tb_job_slot *const slot = tb_jobs_find(jobs, worker);

        if (slot != NULL)
        {
            tb_jobs_run_slot(jobs, slot);
        }
        else
        {
            tb_jobs_sleep(jobs, reason);
        }
    }
}

static void tb_jobs_worker_main(void *argument)
{
    tb_jobs_worker *const worker = argument;
    const tb_jobs_reason reason = {.is_worker = true};

    tb_jobs_this_worker = worker;
    tb_jobs_work_until(worker->jobs, worker, &reason);
    tb_jobs_this_worker = NULL;
}

/* ---------------------------------------------------------------------------
 * The public functions.
 * ------------------------------------------------------------------------- */

/* One worker for each logical processor, less one for the thread that
 * submits and waits, which runs jobs as well. */
static size_t tb_jobs_default_worker_count(void)
{
    tb_system_processors processors = {0};

    tb_system_get_processors(&processors);
    if (processors.logical_count < 2)
    {
        return 1;
    }
    return processors.logical_count - 1;
}

static bool tb_jobs_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

tb_jobs *tb_jobs_create(const tb_jobs_options *options)
{
    tb_jobs *jobs = NULL;
    size_t worker_count = 0;
    size_t queue_capacity = 0;
    size_t queue_count = 0;
    size_t storage_count = 0;

    if (options != NULL)
    {
        worker_count = options->worker_count;
        queue_capacity = options->queue_capacity;
    }
    if (worker_count == 0)
    {
        worker_count = tb_jobs_default_worker_count();
    }
    if (queue_capacity == 0)
    {
        queue_capacity = TB_JOBS_QUEUE_CAPACITY_DEFAULT;
    }
    TB_ASSERT_MSG(tb_jobs_is_power_of_two(queue_capacity), "the queue capacity is a power of two");
    if (!tb_size_multiply(worker_count, TB_JOBS_PRIORITY_COUNT, &queue_count) ||
        !tb_size_multiply(queue_count, queue_capacity, &storage_count))
    {
        tb_jobs_fail("jobs: the queues of the workers are larger than any address space\n");
    }

    jobs = tb_calloc(1, sizeof(*jobs));
    jobs->worker_count = worker_count;
    jobs->workers = tb_calloc(worker_count, sizeof(tb_jobs_worker));
    jobs->queue_storage = tb_calloc(storage_count, sizeof(tb_platform_atomic_pointer));
    for (size_t index = 0; index < worker_count; index++)
    {
        tb_jobs_worker *const worker = &jobs->workers[index];

        worker->jobs = jobs;
        worker->next_victim = index + 1;
        (void)tb_fmt_buffer(worker->name, sizeof(worker->name), "badger-worker-%zu", index);
        for (size_t priority = 0; priority < TB_JOBS_PRIORITY_COUNT; priority++)
        {
            const size_t queue = index * TB_JOBS_PRIORITY_COUNT + priority;

            tb_job_deque_init(&worker->queues[priority],
                              &jobs->queue_storage[queue * queue_capacity], queue_capacity);
        }
    }
    /* Started last: a worker looks at every other worker's queue. */
    for (size_t index = 0; index < worker_count; index++)
    {
        tb_jobs_worker *const worker = &jobs->workers[index];
        const tb_thread_options thread_options = {.name = worker->name,
                                                  .stack_size = TB_THREAD_STACK_SIZE_DEFAULT};

        tb_thread_create_with_options(&worker->thread, &thread_options, tb_jobs_worker_main,
                                      worker);
    }
    return jobs;
}

void tb_jobs_destroy(tb_jobs *jobs)
{
    if (jobs == NULL)
    {
        return;
    }
    TB_ASSERT_MSG(tb_jobs_worker_here(jobs) == NULL,
                  "a scheduler is not destroyed from one of its own jobs");
    tb_jobs_wait_for_all(jobs);
    tb_platform_atomic_u32_store(&jobs->is_stopping, 1);
    tb_jobs_wake_all(jobs);
    for (size_t index = 0; index < jobs->worker_count; index++)
    {
        tb_thread_join(&jobs->workers[index].thread);
    }
    for (uint32_t block = 0; block < TB_JOBS_SLOT_BLOCKS_MAX; block++)
    {
        tb_free(tb_platform_atomic_pointer_load(&jobs->slot_blocks[block]));
    }
    for (uint32_t block = 0; block < TB_JOBS_EDGE_BLOCKS_MAX; block++)
    {
        tb_free(tb_platform_atomic_pointer_load(&jobs->edge_blocks[block]));
    }
    tb_free(jobs->queue_storage);
    tb_free(jobs->workers);
    tb_free(jobs);
}

size_t tb_jobs_worker_count(const tb_jobs *jobs)
{
    TB_ASSERT(jobs != NULL);
    return jobs->worker_count;
}

/* Makes `slot` wait for the job that `on` names, unless that job has
 * finished. */
static void tb_jobs_depend(tb_jobs *jobs, tb_job_slot *slot, tb_job on)
{
    tb_job_slot *other = NULL;
    tb_job_edge *edge = NULL;
    uint32_t edge_id = 0;
    bool is_waiting = false;

    if (on.slot == 0)
    {
        return;
    }
    other = tb_jobs_slot(jobs, on.slot);
    /* Most often it has finished already, and nothing is needed. */
    if (tb_platform_atomic_u32_load(&other->generation) != on.generation)
    {
        return;
    }
    edge_id = tb_jobs_take_edge(jobs);
    edge = tb_jobs_edge(jobs, edge_id);
    edge->dependent = slot->id;
    /* Looked at again under the lock that the job finishes under: either
     * it sees this edge when it finishes, or it has finished. */
    tb_mutex_lock(&other->lock);
    if (tb_platform_atomic_u32_load(&other->generation) == on.generation)
    {
        edge->next = other->first_edge;
        other->first_edge = edge_id;
        (void)tb_platform_atomic_size_add(&slot->pending, 1);
        is_waiting = true;
    }
    tb_mutex_unlock(&other->lock);
    if (!is_waiting)
    {
        tb_jobs_give_back_edges(jobs, edge_id, edge_id);
    }
}

/* Whether every dependency of a description names no job or a slot that
 * this scheduler has made. */
static bool tb_jobs_made_every_dependency(tb_jobs *jobs, const tb_job_description *description)
{
    for (size_t index = 0; index < description->dependency_count; index++)
    {
        const uint32_t id = description->dependencies[index].slot;

        if (id != 0 && !tb_jobs_has_slot(jobs, id))
        {
            return false;
        }
    }
    return true;
}

tb_job tb_jobs_submit(tb_jobs *jobs, const tb_job_description *description)
{
    tb_job_slot *slot = NULL;
    tb_job job = {0};

    TB_ASSERT(jobs != NULL);
    TB_ASSERT(description != NULL);
    TB_ASSERT_MSG(tb_jobs_is_priority(description->priority),
                  "the priority is one of tb_job_priority");
    TB_ASSERT_MSG(description->dependency_count == 0 || description->dependencies != NULL,
                  "dependencies are given when they are counted");
    /* Checked before anything is taken, so that a refused job leaves no
     * trace. */
    TB_ASSERT_MSG(tb_jobs_made_every_dependency(jobs, description),
                  "every dependency comes from this scheduler");
    slot = tb_jobs_take_slot(jobs);
    slot->function = description->function;
    slot->argument = description->argument;
    slot->priority = description->priority;
    job.slot = slot->id;
    job.generation = tb_platform_atomic_u32_load(&slot->generation);
    /* One more than its dependencies, so that it cannot become ready while
     * they are still being counted. */
    tb_platform_atomic_size_store(&slot->pending, 1);
    (void)tb_platform_atomic_size_add(&jobs->unfinished_count, 1);
    for (size_t index = 0; index < description->dependency_count; index++)
    {
        tb_jobs_depend(jobs, slot, description->dependencies[index]);
    }
    /* From here the job may run, finish, and give its slot to another. */
    tb_jobs_count_down(jobs, slot);
    return job;
}

tb_job tb_jobs_run(tb_jobs *jobs, tb_job_function *function, void *argument)
{
    const tb_job_description description = {.function = function, .argument = argument};

    TB_ASSERT(function != NULL);
    return tb_jobs_submit(jobs, &description);
}

bool tb_jobs_is_finished(tb_jobs *jobs, tb_job job)
{
    TB_ASSERT(jobs != NULL);
    if (job.slot == 0)
    {
        return true;
    }
    TB_ASSERT_MSG(tb_jobs_has_slot(jobs, job.slot), "the job comes from this scheduler");
    return tb_platform_atomic_u32_load(&tb_jobs_slot(jobs, job.slot)->generation) != job.generation;
}

void tb_jobs_wait(tb_jobs *jobs, tb_job job)
{
    tb_jobs_reason reason = {0};

    TB_ASSERT(jobs != NULL);
    if (job.slot == 0)
    {
        return;
    }
    TB_ASSERT_MSG(tb_jobs_has_slot(jobs, job.slot), "the job comes from this scheduler");
    reason.slot = tb_jobs_slot(jobs, job.slot);
    reason.generation = job.generation;
    tb_jobs_work_until(jobs, tb_jobs_worker_here(jobs), &reason);
}

void tb_jobs_wait_for_all(tb_jobs *jobs)
{
    const tb_jobs_reason reason = {.is_for_all = true};

    TB_ASSERT(jobs != NULL);
    tb_jobs_work_until(jobs, tb_jobs_worker_here(jobs), &reason);
}
