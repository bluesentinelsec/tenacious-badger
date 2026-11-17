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
 * The aim throughout is that a worker which has work touches nothing that
 * another thread writes. It takes jobs from its own queue, takes slots for
 * new jobs from a store of its own, and counts what it submits and finishes
 * in counters of its own. The locks are for threads that are not workers,
 * for dependencies, and for the moments when a worker runs out. */

#define TB_JOBS_PRIORITY_COUNT ((size_t)2)

#define TB_JOBS_QUEUE_CAPACITY_DEFAULT ((size_t)256)

/* Slots and edges are made this many at a time, in blocks that are found
 * through a table of fixed size, which is what bounds how many unfinished
 * jobs there can be: 1,048,576, and four times as many dependencies. */
#define TB_JOBS_SLOTS_PER_BLOCK ((uint32_t)1024)
#define TB_JOBS_SLOT_BLOCKS_MAX ((uint32_t)1024)
#define TB_JOBS_EDGES_PER_BLOCK ((uint32_t)1024)
#define TB_JOBS_EDGE_BLOCKS_MAX ((uint32_t)4096)

/* Free slots that a worker moves between its own store and the shared one
 * at a time, so that it takes the lock of the shared one once for this many
 * jobs. Its own store holds up to twice as many. */
#define TB_JOBS_SLOT_STORE_BATCH ((uint32_t)64)

/* Jobs that a worker moves from a shared queue to its own beyond the one it
 * came for, for the same reason. */
#define TB_JOBS_SHARED_BATCH ((size_t)16)

/* How long a thread with nothing to run goes on looking through the queues
 * before it sleeps. Putting a thread to sleep and waking it costs several
 * microseconds, far more than a small job, and while one thread is looking
 * no other is woken. So a thread that would have work again within this
 * time should not sleep, and the price is a processor kept busy for this
 * long after the work has run out. */
#define TB_JOBS_SEARCH_NANOSECONDS UINT64_C(100000)

/* Times it looks between two readings of the clock. */
#define TB_JOBS_SEARCH_PASSES_PER_READING ((size_t)16)

/* Workers that go on looking at one time. The rest sleep as soon as one look
 * has found nothing. Idle workers that all keep looking do not find work any
 * sooner: they take turns at the same few cache lines, and the thread that
 * is trying to give them a job waits for its turn among them. One is enough
 * to take a job the moment it appears, and it wakes the next when there is
 * more than it can take. */
#define TB_JOBS_LOOKING_WORKERS_MAX ((size_t)1)

/* Keeps what one thread writes away from what another reads: twice the
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
    /* The next free slot, in whichever store the slot is in. */
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

/* Jobs that are ready, for any thread to take, oldest first.
 *
 * It is two lists. A thread that makes a job ready adds it to the arrivals,
 * with one compare-and-exchange and no lock, so that a thread which submits
 * many jobs is never made to wait by the threads that take them. A thread
 * that takes a job takes it from the departures, under a lock, and when
 * those run out it takes all the arrivals at once and turns them round,
 * which puts the oldest first. Each job is turned round once. */
typedef struct tb_job_shared_queue
{
    /* The job that arrived last; each leads to the one before it. Written
     * by the threads that make jobs ready, and kept apart from what the
     * threads that take them write. */
    tb_platform_atomic_pointer newest_arrival;
    char padding_after_arrivals[TB_JOBS_CACHE_LINE_SIZE];

    tb_mutex lock; /* Guards `first_departure`. */
    /* The oldest job that is waiting to be taken. */
    tb_job_slot *first_departure;
    /* How many departures there are, for a thread that only wants to know
     * whether to take the lock. */
    tb_platform_atomic_size departure_count;
    char padding_after_departures[TB_JOBS_CACHE_LINE_SIZE];
} tb_job_shared_queue;

typedef struct tb_jobs_worker
{
    tb_jobs *jobs;
    tb_thread thread;
    char name[TB_THREAD_NAME_CAPACITY];
    char padding_before_queues[TB_JOBS_CACHE_LINE_SIZE];

    /* Its own ready jobs, for each priority. Thieves read and write these. */
    tb_job_deque queues[TB_JOBS_PRIORITY_COUNT];
    char padding_before_own[TB_JOBS_CACHE_LINE_SIZE];

    /* The rest is written by this worker's thread alone. */

    /* Where it looks first when it steals. */
    size_t next_victim;
    /* Its own store of free slots, and how many it holds. */
    uint32_t free_slot;
    uint32_t free_slot_count;
    /* Jobs that this thread has submitted, and finished. Other threads read
     * them to learn whether anything is unfinished. */
    tb_platform_atomic_size submitted_count;
    tb_platform_atomic_size finished_count;
    char padding_after[TB_JOBS_CACHE_LINE_SIZE];
} tb_jobs_worker;

/* The fields are grouped by who writes them, and the groups are kept a cache
 * line apart. A thread that writes a field takes its cache line away from
 * every thread that reads anything on it, so a count that one thread writes
 * for every job must not share a line with one that others read for every
 * job. */
struct tb_jobs
{
    /* Set when the scheduler is made, and only read afterwards. */
    tb_jobs_worker *workers;
    size_t worker_count;
    /* The storage of every worker's queues, in one block. */
    tb_platform_atomic_pointer *queue_storage;
    char padding_after_constants[TB_JOBS_CACHE_LINE_SIZE];

    tb_job_shared_queue shared[TB_JOBS_PRIORITY_COUNT];

    /* Threads with nothing to run sleep here. These are written when a
     * thread goes to sleep or is woken, and read for every job. */
    tb_mutex sleep_lock;
    tb_condition wake;
    /* Threads that are asleep on `wake`, or about to be. */
    tb_platform_atomic_size sleeper_count;
    /* Those of them that wait for one job to finish. */
    tb_platform_atomic_size job_waiter_count;
    /* Those of them that wait for every job to finish. */
    tb_platform_atomic_size all_waiter_count;
    /* Not zero once the workers are to end. */
    tb_platform_atomic_u32 is_stopping;
    char padding_after_sleepers[TB_JOBS_CACHE_LINE_SIZE];

    /* Threads that have no job and are looking for one. While there is one,
     * a job that becomes ready wakes nobody. Written whenever a thread runs
     * out of jobs of its own. */
    tb_platform_atomic_size searcher_count;
    /* Workers among them that go on looking after a first look found
     * nothing; at most TB_JOBS_LOOKING_WORKERS_MAX. */
    tb_platform_atomic_size looking_worker_count;
    char padding_after_searchers[TB_JOBS_CACHE_LINE_SIZE];

    /* What the threads that are not workers have submitted and finished.
     * Written by them for every job. */
    tb_platform_atomic_size other_submitted_count;
    tb_platform_atomic_size other_finished_count;
    char padding_after_other_counts[TB_JOBS_CACHE_LINE_SIZE];

    /* Guards the shared stores of free slots and edges, and the making of
     * new blocks. */
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

/* Where a thread that is not a worker looks first when it steals. A count
 * of its own, so that looking writes nothing that is shared. */
static _Thread_local size_t tb_jobs_next_victim_here = 0;

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

/* A slot from the shared store, or a new one. The caller holds the lock of
 * the pools. */
static tb_job_slot *tb_jobs_take_shared_slot(tb_jobs *jobs)
{
    tb_job_slot *slot = NULL;
    uint32_t made = 0;

    if (jobs->free_slot != 0)
    {
        slot = tb_jobs_slot(jobs, jobs->free_slot);
        jobs->free_slot = slot->next_free;
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
    return slot;
}

/* A slot for a new job. A worker takes it from its own store, and fills that
 * from the shared one when it is empty, a batch at a time. */
static tb_job_slot *tb_jobs_take_slot(tb_jobs *jobs, tb_jobs_worker *worker)
{
    tb_job_slot *slot = NULL;

    if (worker != NULL && worker->free_slot != 0)
    {
        slot = tb_jobs_slot(jobs, worker->free_slot);
        worker->free_slot = slot->next_free;
        worker->free_slot_count--;
        return slot;
    }
    tb_mutex_lock(&jobs->pool_lock);
    slot = tb_jobs_take_shared_slot(jobs);
    if (worker != NULL)
    {
        /* Only slots that are free already: new ones are made as needed. */
        while (worker->free_slot_count < TB_JOBS_SLOT_STORE_BATCH && jobs->free_slot != 0)
        {
            tb_job_slot *const spare = tb_jobs_slot(jobs, jobs->free_slot);

            jobs->free_slot = spare->next_free;
            spare->next_free = worker->free_slot;
            worker->free_slot = spare->id;
            worker->free_slot_count++;
        }
    }
    tb_mutex_unlock(&jobs->pool_lock);
    return slot;
}

/* Gives back the slot of a finished job. A worker keeps it, and passes a
 * batch on to the shared store when it holds two. */
static void tb_jobs_give_back_slot(tb_jobs *jobs, tb_jobs_worker *worker, tb_job_slot *slot)
{
    tb_job_slot *last = NULL;
    uint32_t first = 0;

    if (worker == NULL)
    {
        tb_mutex_lock(&jobs->pool_lock);
        slot->next_free = jobs->free_slot;
        jobs->free_slot = slot->id;
        tb_mutex_unlock(&jobs->pool_lock);
        return;
    }
    slot->next_free = worker->free_slot;
    worker->free_slot = slot->id;
    worker->free_slot_count++;
    if (worker->free_slot_count < TB_JOBS_SLOT_STORE_BATCH * 2)
    {
        return;
    }
    /* The newest batch, from the head of the list to its last slot. */
    first = worker->free_slot;
    last = tb_jobs_slot(jobs, first);
    for (uint32_t step = 1; step < TB_JOBS_SLOT_STORE_BATCH; step++)
    {
        last = tb_jobs_slot(jobs, last->next_free);
    }
    worker->free_slot = last->next_free;
    worker->free_slot_count -= TB_JOBS_SLOT_STORE_BATCH;
    tb_mutex_lock(&jobs->pool_lock);
    last->next_free = jobs->free_slot;
    jobs->free_slot = first;
    tb_mutex_unlock(&jobs->pool_lock);
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

/* ---------------------------------------------------------------------------
 * Counting what is unfinished.
 *
 * Every thread counts the jobs it submits and the jobs it finishes, a worker
 * in counters of its own and the other threads in two that they share. One
 * count of unfinished jobs would be written by every thread for every job,
 * and the threads would spend their time passing it between them.
 * ------------------------------------------------------------------------- */

static void tb_jobs_count_submitted(tb_jobs *jobs, tb_jobs_worker *worker)
{
    if (worker != NULL)
    {
        (void)tb_platform_atomic_size_add(&worker->submitted_count, 1);
        return;
    }
    (void)tb_platform_atomic_size_add(&jobs->other_submitted_count, 1);
}

static void tb_jobs_count_finished(tb_jobs *jobs, tb_jobs_worker *worker)
{
    if (worker != NULL)
    {
        (void)tb_platform_atomic_size_add(&worker->finished_count, 1);
        return;
    }
    (void)tb_platform_atomic_size_add(&jobs->other_finished_count, 1);
}

/* Whether every job that was submitted has finished.
 *
 * The finished counts are all read before any submitted count. The counts
 * only rise, so the first sum is no more than what had finished when its
 * reading ended, and the second no less than what had been submitted by
 * then. When the two are equal, nothing was unfinished at that moment. */
static bool tb_jobs_is_idle(tb_jobs *jobs)
{
    size_t finished = tb_platform_atomic_size_load(&jobs->other_finished_count);
    size_t submitted = 0;

    for (size_t index = 0; index < jobs->worker_count; index++)
    {
        finished += tb_platform_atomic_size_load(&jobs->workers[index].finished_count);
    }
    submitted = tb_platform_atomic_size_load(&jobs->other_submitted_count);
    for (size_t index = 0; index < jobs->worker_count; index++)
    {
        submitted += tb_platform_atomic_size_load(&jobs->workers[index].submitted_count);
    }
    return submitted == finished;
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
        return tb_jobs_is_idle(jobs);
    }
    return tb_platform_atomic_u32_load(&reason->slot->generation) != reason->generation;
}

/* The count of sleepers that a thread with this reason is one of, beside
 * the count of them all; NULL for a worker, which waits only for work. */
static tb_platform_atomic_size *tb_jobs_waiter_count(tb_jobs *jobs, const tb_jobs_reason *reason)
{
    if (reason->is_worker)
    {
        return NULL;
    }
    if (reason->is_for_all)
    {
        return &jobs->all_waiter_count;
    }
    return &jobs->job_waiter_count;
}

/* Whether a shared queue holds a job. A moment's reading. */
static bool tb_jobs_shared_has_job(tb_job_shared_queue *queue)
{
    return tb_platform_atomic_pointer_load(&queue->newest_arrival) != NULL ||
           tb_platform_atomic_size_load(&queue->departure_count) != 0;
}

/* Whether any queue holds a job. A moment's reading. */
static bool tb_jobs_has_ready_job(tb_jobs *jobs)
{
    for (size_t priority = 0; priority < TB_JOBS_PRIORITY_COUNT; priority++)
    {
        if (tb_jobs_shared_has_job(&jobs->shared[priority]))
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
    tb_platform_atomic_size *const waiter_count = tb_jobs_waiter_count(jobs, reason);

    tb_mutex_lock(&jobs->sleep_lock);
    (void)tb_platform_atomic_size_add(&jobs->sleeper_count, 1);
    if (waiter_count != NULL)
    {
        (void)tb_platform_atomic_size_add(waiter_count, 1);
    }
    if (!tb_jobs_has_ready_job(jobs) && !tb_jobs_reason_has_ended(jobs, reason))
    {
        tb_condition_wait(&jobs->wake, &jobs->sleep_lock);
    }
    if (waiter_count != NULL)
    {
        (void)tb_platform_atomic_size_subtract(waiter_count, 1);
    }
    (void)tb_platform_atomic_size_subtract(&jobs->sleeper_count, 1);
    tb_mutex_unlock(&jobs->sleep_lock);
}

/* Wakes one sleeper, if any. Call after what it is to notice has been done. */
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

/* Wakes every sleeper. Call after what they are to notice has been done. */
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
    void *newest = tb_platform_atomic_pointer_load(&queue->newest_arrival);

    /* Written before the job is published, and written again when another
     * thread got in first. */
    do
    {
        slot->next_queued = newest;
    } while (!tb_platform_atomic_pointer_compare_exchange(&queue->newest_arrival, &newest, slot));
}

/* Makes the arrivals the departures, oldest first. The caller holds the
 * queue's lock, and there are no departures. */
static void tb_jobs_admit_arrivals(tb_job_shared_queue *queue)
{
    tb_job_slot *arrival = tb_platform_atomic_pointer_exchange(&queue->newest_arrival, NULL);
    tb_job_slot *first = NULL;
    size_t count = 0;

    /* The newest leads to the one before it; turned round, the oldest
     * leads to the one after it. */
    while (arrival != NULL)
    {
        tb_job_slot *const older = arrival->next_queued;

        arrival->next_queued = first;
        first = arrival;
        arrival = older;
        count++;
    }
    queue->first_departure = first;
    tb_platform_atomic_size_store(&queue->departure_count, count);
}

/* The oldest departure, removed, or NULL. The caller holds the queue's
 * lock. */
static tb_job_slot *tb_jobs_remove_first(tb_job_shared_queue *queue)
{
    tb_job_slot *const slot = queue->first_departure;

    if (slot == NULL)
    {
        return NULL;
    }
    queue->first_departure = slot->next_queued;
    (void)tb_platform_atomic_size_subtract(&queue->departure_count, 1);
    return slot;
}

/* Puts a job back as the oldest departure. The caller holds the queue's
 * lock. */
static void tb_jobs_put_back_first(tb_job_shared_queue *queue, tb_job_slot *slot)
{
    slot->next_queued = queue->first_departure;
    queue->first_departure = slot;
    (void)tb_platform_atomic_size_add(&queue->departure_count, 1);
}

/* Takes the oldest job of a shared queue, or returns NULL when it has none
 * or another thread is at it. A worker takes some of the next ones into its
 * own queue as well, so that it comes back for the lock once in a batch, and
 * idle threads can steal them without it.
 *
 * A thread that finds the lock taken does not wait for it. The thread that
 * has it may be turning round a long list of arrivals, and there are other
 * queues to look in meanwhile; a thread that finds nothing anywhere comes
 * back. */
static tb_job_slot *tb_jobs_take_shared(tb_job_shared_queue *queue, tb_job_deque *own_queue)
{
    tb_job_slot *slot = NULL;

    if (!tb_jobs_shared_has_job(queue) || !tb_mutex_try_lock(&queue->lock))
    {
        return NULL;
    }
    if (queue->first_departure == NULL)
    {
        tb_jobs_admit_arrivals(queue);
    }
    slot = tb_jobs_remove_first(queue);
    if (slot != NULL && own_queue != NULL)
    {
        for (size_t moved = 0; moved < TB_JOBS_SHARED_BATCH; moved++)
        {
            /* Removed before it is pushed: once it is in the worker's own
             * queue a thief may run it, and its slot may be in another
             * queue before this one has been stepped past it. */
            tb_job_slot *const next = tb_jobs_remove_first(queue);

            if (next == NULL)
            {
                break;
            }
            if (!tb_job_deque_push(own_queue, next))
            {
                tb_jobs_put_back_first(queue, next);
                break;
            }
        }
    }
    tb_mutex_unlock(&queue->lock);
    return slot;
}

/* Puts a job where a thread will find it, and sees to it that one looks. A
 * worker keeps the job in its own queue while there is room.
 *
 * A sleeper is woken only when no thread is searching. A searcher will find
 * the job, or will look once more as it goes to sleep and see it, and when
 * it finds one it wakes the next thread itself. So a burst of jobs wakes
 * threads one after another, not one for each job. */
static void tb_jobs_make_ready(tb_jobs *jobs, tb_jobs_worker *worker, tb_job_slot *slot)
{
    bool is_queued = false;

    if (worker != NULL)
    {
        is_queued = tb_job_deque_push(&worker->queues[slot->priority], slot);
    }
    if (!is_queued)
    {
        tb_jobs_share(&jobs->shared[slot->priority], slot);
    }
    if (tb_platform_atomic_size_load(&jobs->searcher_count) == 0)
    {
        tb_jobs_wake_one(jobs);
    }
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
        start = tb_jobs_next_victim_here;
        tb_jobs_next_victim_here++;
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
        tb_job_deque *own_queue = NULL;
        tb_job_slot *slot = NULL;

        if (worker != NULL)
        {
            own_queue = &worker->queues[priority];
            slot = tb_job_deque_pop(own_queue);
            if (slot != NULL)
            {
                return slot;
            }
        }
        slot = tb_jobs_take_shared(&jobs->shared[priority], own_queue);
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

/* Whether the calling thread may go on looking for a job after a first look
 * found none. A thread that waits for a job always may: its job is on the
 * way. Of the workers, only a few at a time; one that may must say when it
 * stops, with tb_jobs_stop_looking(). */
static bool tb_jobs_may_go_on_looking(tb_jobs *jobs, const tb_jobs_reason *reason)
{
    if (!reason->is_worker)
    {
        return true;
    }
    if (tb_platform_atomic_size_add(&jobs->looking_worker_count, 1) <= TB_JOBS_LOOKING_WORKERS_MAX)
    {
        return true;
    }
    (void)tb_platform_atomic_size_subtract(&jobs->looking_worker_count, 1);
    return false;
}

static void tb_jobs_stop_looking(tb_jobs *jobs, const tb_jobs_reason *reason)
{
    if (reason->is_worker)
    {
        (void)tb_platform_atomic_size_subtract(&jobs->looking_worker_count, 1);
    }
}

/* Looks for a job in every queue: once, and then, if the thread may, again
 * and again for a while, before it gives up and sleeps. Returns the job, or
 * NULL when there was none or the reason ended.
 *
 * While it looks, the thread is counted as a searcher, and threads that make
 * jobs ready do not wake a sleeper. When it finds a job, no other thread is
 * searching, and more jobs are ready, it wakes one to search in its place.
 *
 * A worker comes here whenever its own foreground queue is empty, so it is
 * counted from the moment it has nothing of its own to do. Counting it only
 * once a first look had failed left a gap, after every job, in which a new
 * job found no searcher and woke a sleeping thread for nothing. */
static tb_job_slot *tb_jobs_search(tb_jobs *jobs, tb_jobs_worker *worker,
                                   const tb_jobs_reason *reason)
{
    tb_job_slot *slot = NULL;
    uint64_t start = 0;
    size_t searchers_left = 0;
    bool has_started_the_clock = false;
    bool is_over = false;
    bool is_looking_on = false;

    (void)tb_platform_atomic_size_add(&jobs->searcher_count, 1);
    slot = tb_jobs_find(jobs, worker);
    is_over = slot != NULL || tb_jobs_reason_has_ended(jobs, reason);
    if (!is_over)
    {
        is_looking_on = tb_jobs_may_go_on_looking(jobs, reason);
        is_over = !is_looking_on;
    }
    while (!is_over)
    {
        for (size_t pass = 0; pass < TB_JOBS_SEARCH_PASSES_PER_READING && !is_over; pass++)
        {
            slot = tb_jobs_find(jobs, worker);
            is_over = slot != NULL || tb_jobs_reason_has_ended(jobs, reason);
        }
        /* The clock is not read at all by a search that soon finds a job. */
        if (!is_over && !has_started_the_clock)
        {
            start = tb_platform_clock_nanoseconds();
            has_started_the_clock = true;
        }
        else if (!is_over)
        {
            is_over = tb_platform_clock_nanoseconds() - start >= TB_JOBS_SEARCH_NANOSECONDS;
        }
    }
    if (is_looking_on)
    {
        tb_jobs_stop_looking(jobs, reason);
    }
    searchers_left = tb_platform_atomic_size_subtract(&jobs->searcher_count, 1);
    /* Looked at after this thread is no longer counted: a job that became
     * ready while it was counted woke nobody, and is seen here. With one job
     * and no more, there is nothing for a second thread to do. */
    if (slot != NULL && searchers_left == 0 && tb_jobs_has_ready_job(jobs))
    {
        tb_jobs_wake_one(jobs);
    }
    return slot;
}

/* ---------------------------------------------------------------------------
 * Running a job.
 * ------------------------------------------------------------------------- */

/* One of the things a job waited for is done. */
static void tb_jobs_count_down(tb_jobs *jobs, tb_jobs_worker *worker, tb_job_slot *slot)
{
    if (tb_platform_atomic_size_subtract(&slot->pending, 1) == 0)
    {
        tb_jobs_make_ready(jobs, worker, slot);
    }
}

static void tb_jobs_run_slot(tb_jobs *jobs, tb_jobs_worker *worker, tb_job_slot *slot)
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

        tb_jobs_count_down(jobs, worker, tb_jobs_slot(jobs, edge->dependent));
        last_edge = edge_id;
        edge_id = edge->next;
    }
    tb_jobs_give_back_edges(jobs, first_edge, last_edge);
    tb_jobs_give_back_slot(jobs, worker, slot);

    /* Counted as finished after the jobs that waited for it were made
     * ready and counted, so that the scheduler does not look idle in
     * between. Then whoever sleeps waiting for a job to finish is woken to
     * look at the one it waits for, and whoever waits for all of them is
     * woken when this was the last. */
    tb_jobs_count_finished(jobs, worker);
    if (tb_platform_atomic_size_load(&jobs->job_waiter_count) != 0)
    {
        tb_jobs_wake_all(jobs);
    }
    else if (tb_platform_atomic_size_load(&jobs->all_waiter_count) != 0 && tb_jobs_is_idle(jobs))
    {
        tb_jobs_wake_all(jobs);
    }
}

/* Runs jobs on the calling thread until the reason has ended, and sleeps
 * while there are none. */
static void tb_jobs_work_until(tb_jobs *jobs, tb_jobs_worker *worker, const tb_jobs_reason *reason)
{
    for (;;)
    {
        tb_job_slot *slot = NULL;

        /* A thread that waits for one job looks at it between jobs, which
         * costs one reading. The other reasons cost more to look at, and
         * are looked at when there is nothing to run. */
        if (reason->slot != NULL && tb_jobs_reason_has_ended(jobs, reason))
        {
            return;
        }
        /* A worker's next job is most often in its own foreground queue,
         * where taking it touches nothing that is shared. */
        if (worker != NULL)
        {
            slot = tb_job_deque_pop(&worker->queues[TB_JOB_PRIORITY_FOREGROUND]);
        }
        if (slot == NULL)
        {
            slot = tb_jobs_search(jobs, worker, reason);
        }
        if (slot != NULL)
        {
            tb_jobs_run_slot(jobs, worker, slot);
            continue;
        }
        if (tb_jobs_reason_has_ended(jobs, reason))
        {
            /* This thread was counted as searching, so a job that became
             * ready meanwhile woke nobody. It is not going to sleep, where
             * it would look once more; it looks now. */
            if (tb_jobs_has_ready_job(jobs))
            {
                tb_jobs_wake_one(jobs);
            }
            return;
        }
        tb_jobs_sleep(jobs, reason);
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
    tb_jobs_worker *worker = NULL;
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
    worker = tb_jobs_worker_here(jobs);
    slot = tb_jobs_take_slot(jobs, worker);
    slot->function = description->function;
    slot->argument = description->argument;
    slot->priority = description->priority;
    job.slot = slot->id;
    job.generation = tb_platform_atomic_u32_load(&slot->generation);
    /* One more than its dependencies, so that it cannot become ready while
     * they are still being counted. */
    tb_platform_atomic_size_store(&slot->pending, 1);
    tb_jobs_count_submitted(jobs, worker);
    for (size_t index = 0; index < description->dependency_count; index++)
    {
        tb_jobs_depend(jobs, slot, description->dependencies[index]);
    }
    /* From here the job may run, finish, and give its slot to another. */
    tb_jobs_count_down(jobs, worker, slot);
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

/* ---------------------------------------------------------------------------
 * Loops.
 *
 * A loop is its iterations and a count of how many have been handed out.
 * Every thread that takes part claims the next range by moving the count
 * on, runs it, and comes back for another, until none is left. The calling
 * thread takes part, and so does a job for each worker; a job that starts
 * after the last range has been claimed finds nothing and ends.
 * ------------------------------------------------------------------------- */

/* Ranges for each thread when the caller leaves the grain to the scheduler:
 * enough that a thread which gets slow iterations is not left with a large
 * share, and few enough that claiming them costs little. */
#define TB_JOBS_RANGES_PER_THREAD ((size_t)4)

/* Most jobs that take part in one loop, beside the calling thread. */
#define TB_JOBS_LOOP_HELPERS_MAX ((size_t)64)

typedef struct tb_jobs_loop
{
    tb_job_range_function *function;
    void *argument;
    size_t count; /* Iterations in the loop. */
    size_t grain; /* Most iterations in a range; at least 1. */
    /* Iterations handed out so far; never more than `count`. */
    tb_platform_atomic_size claimed;
} tb_jobs_loop;

/* Runs ranges of a loop until none is left. Also the function of the jobs
 * that take part. */
static void tb_jobs_run_ranges(void *argument)
{
    tb_jobs_loop *const loop = argument;
    size_t first = tb_platform_atomic_size_load(&loop->claimed);

    while (first < loop->count)
    {
        size_t length = loop->count - first;

        if (length > loop->grain)
        {
            length = loop->grain;
        }
        /* On failure `first` is what another thread moved the count to. */
        if (tb_platform_atomic_size_compare_exchange(&loop->claimed, &first, first + length))
        {
            loop->function(loop->argument, first, length);
            first = tb_platform_atomic_size_load(&loop->claimed);
        }
    }
}

/* `dividend` divided by `divisor`, which is not zero, rounded up. */
static size_t tb_jobs_divide_rounding_up(size_t dividend, size_t divisor)
{
    size_t quotient = dividend / divisor;

    if (dividend % divisor != 0)
    {
        quotient++;
    }
    return quotient;
}

/* The grain of a loop whose caller left it to the scheduler. */
static size_t tb_jobs_choose_grain(const tb_jobs *jobs, size_t count)
{
    /* The workers and the calling thread. A count of threads that makes
     * the product overflow is larger than any count of iterations. */
    size_t range_count = 0;

    if (!tb_size_multiply(jobs->worker_count + 1, TB_JOBS_RANGES_PER_THREAD, &range_count) ||
        range_count >= count)
    {
        return 1;
    }
    /* Rounded up, so that there are no more ranges than were aimed at. */
    return tb_jobs_divide_rounding_up(count, range_count);
}

void tb_jobs_for(tb_jobs *jobs, size_t count, size_t grain, tb_job_range_function *function,
                 void *argument)
{
    tb_jobs_loop loop = {.function = function, .argument = argument, .count = count};
    tb_job helpers[TB_JOBS_LOOP_HELPERS_MAX] = {0};
    size_t helper_count = 0;
    size_t range_count = 0;

    TB_ASSERT(jobs != NULL);
    TB_ASSERT(function != NULL);
    if (count == 0)
    {
        return;
    }
    loop.grain = grain;
    if (grain == 0)
    {
        loop.grain = tb_jobs_choose_grain(jobs, count);
    }
    /* A job for each worker, and no more jobs than there are ranges beyond
     * the one that the calling thread starts with. */
    range_count = tb_jobs_divide_rounding_up(count, loop.grain);
    helper_count = jobs->worker_count;
    if (helper_count > TB_JOBS_LOOP_HELPERS_MAX)
    {
        helper_count = TB_JOBS_LOOP_HELPERS_MAX;
    }
    if (helper_count > range_count - 1)
    {
        helper_count = range_count - 1;
    }
    for (size_t index = 0; index < helper_count; index++)
    {
        helpers[index] = tb_jobs_run(jobs, tb_jobs_run_ranges, &loop);
    }
    tb_jobs_run_ranges(&loop);
    /* Every range has been claimed, and the jobs that claimed some may
     * still be running them. `loop` is on this stack until they are done. */
    for (size_t index = 0; index < helper_count; index++)
    {
        tb_jobs_wait(jobs, helpers[index]);
    }
}
