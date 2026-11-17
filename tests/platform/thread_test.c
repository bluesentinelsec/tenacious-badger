/* Threads, mutexes, and condition variables must do what badger/thread.h
 * says on every platform.
 *
 * The test framework's checks are for one thread, so a thread started by a
 * case only records what it saw, and the case checks the record after joining
 * it. A wait that a broken adapter would never end has a time limit instead,
 * so that the case fails where it would otherwise hang. */
#include "badger/platform.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <inttypes.h>
#include <stddef.h>

#define NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)
#define NANOSECONDS_PER_SECOND UINT64_C(1000000000)

/* Threads that contend with one another. More than the cores of a hosted CI
 * machine, so that some of them are always waiting to run. */
#define THREAD_COUNT 8

/* Longest that a thread waits for something that must happen. No machine is
 * so slow, so reaching it means that the wake was lost. */
#define PATIENCE_NANOSECONDS (UINT64_C(20) * NANOSECONDS_PER_SECOND)

/* Waits on a condition until `*is_true`, which the mutex guards. Returns
 * false when a wait reached its limit instead: nothing woke the thread in
 * all the time a wake could take, whatever has become true since. */
static bool wait_until(tb_condition *condition, tb_mutex *mutex, const bool *is_true)
{
    while (!*is_true)
    {
        if (!tb_condition_wait_for(condition, mutex, PATIENCE_NANOSECONDS))
        {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

#define STARTED_THREAD_COUNT 64
#define START_ROUND_COUNT 4

typedef struct visit
{
    tb_platform_atomic_size run_count; /* Times the thread's function ran. */
    void *argument_seen;               /* What it was given. */
} visit;

static void record_visit(void *argument)
{
    visit *const visited = argument;

    visited->argument_seen = argument;
    (void)tb_platform_atomic_size_add(&visited->run_count, 1);
}

TB_TEST(platform_thread, every_thread_runs_its_function_once_with_its_argument)
{
    visit visits[STARTED_THREAD_COUNT] = {0};
    tb_thread threads[STARTED_THREAD_COUNT] = {0};

    for (size_t round = 1; round <= START_ROUND_COUNT; round++)
    {
        for (size_t index = 0; index < STARTED_THREAD_COUNT; index++)
        {
            tb_thread_create(&threads[index], record_visit, &visits[index]);
        }
        for (size_t index = 0; index < STARTED_THREAD_COUNT; index++)
        {
            tb_thread_join(&threads[index]);
        }
        for (size_t index = 0; index < STARTED_THREAD_COUNT; index++)
        {
            TB_TEST_CONTEXT("round %zu, thread %zu", round, index);
            TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&visits[index].run_count), ==, round);
            TB_TEST_EXPECT_POINTER(visits[index].argument_seen, ==, &visits[index]);
            /* A joined thread is zeroed, which is what lets it be created again. */
            TB_TEST_EXPECT_UINT(threads[index].native, ==, 0);
        }
    }
}

/* A value of its own in every thread, by the C keyword that badger/thread.h
 * names as the way to have one. */
static _Thread_local size_t thread_local_value = 0;

#define MAIN_THREAD_VALUE ((size_t)0xBADCAFE)

/* The threads of a case that must all be running at one time. Each reports
 * and then waits at a gate that the case opens. */
typedef struct gathering
{
    tb_mutex lock;        /* Guards the fields below. */
    tb_condition arrival; /* Woken when `arrived_count` rises. */
    tb_condition opening; /* Woken when `is_open` becomes true. */
    size_t arrived_count; /* Threads that have reported. */
    bool has_all_arrived; /* arrived_count == THREAD_COUNT. */
    bool is_open;         /* The threads may go on. */
    bool has_given_up;    /* A thread stopped waiting for the gate. */
} gathering;

typedef struct member
{
    gathering *group;
    size_t number;           /* 1 to THREAD_COUNT. */
    uint64_t id;             /* tb_thread_current_id() when it arrived. */
    uint64_t id_at_the_end;  /* The same, read again after the gate opened. */
    size_t local_at_start;   /* thread_local_value before the thread wrote it. */
    size_t local_at_the_end; /* thread_local_value after the gate opened. */
} member;

static void arrive_and_wait(void *argument)
{
    member *const self = argument;
    gathering *const group = self->group;

    self->id = tb_thread_current_id();
    self->local_at_start = thread_local_value;
    thread_local_value = self->number;

    tb_mutex_lock(&group->lock);
    group->arrived_count++;
    group->has_all_arrived = group->arrived_count == THREAD_COUNT;
    tb_condition_wake_all(&group->arrival);
    if (!wait_until(&group->opening, &group->lock, &group->is_open))
    {
        group->has_given_up = true;
    }
    tb_mutex_unlock(&group->lock);

    self->id_at_the_end = tb_thread_current_id();
    self->local_at_the_end = thread_local_value;
}

/* Starts the members, and returns once every one of them is waiting at the
 * gate, or false when they did not all arrive. */
static bool gather(gathering *group, member *members, tb_thread *threads)
{
    bool has_gathered = false;

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        members[index].group = group;
        members[index].number = index + 1;
        tb_thread_create(&threads[index], arrive_and_wait, &members[index]);
    }
    tb_mutex_lock(&group->lock);
    has_gathered = wait_until(&group->arrival, &group->lock, &group->has_all_arrived);
    tb_mutex_unlock(&group->lock);
    return has_gathered;
}

static void release(gathering *group, tb_thread *threads)
{
    tb_mutex_lock(&group->lock);
    group->is_open = true;
    tb_condition_wake_all(&group->opening);
    tb_mutex_unlock(&group->lock);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
}

TB_TEST(platform_thread, running_threads_have_different_identifiers)
{
    gathering group = {0};
    member members[THREAD_COUNT] = {0};
    tb_thread threads[THREAD_COUNT] = {0};
    const uint64_t own_id = tb_thread_current_id();
    const bool has_gathered = gather(&group, members, threads);

    release(&group, threads);
    TB_TEST_REQUIRE(has_gathered);
    TB_TEST_EXPECT(!group.has_given_up);
    TB_TEST_EXPECT_UINT(own_id, !=, 0);
    TB_TEST_EXPECT_UINT(tb_thread_current_id(), ==, own_id);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_CONTEXT("thread %zu", index);
        TB_TEST_EXPECT_UINT(members[index].id, !=, 0);
        TB_TEST_EXPECT_UINT(members[index].id, !=, own_id);
        TB_TEST_EXPECT_UINT(members[index].id_at_the_end, ==, members[index].id);
        for (size_t other = index + 1; other < THREAD_COUNT; other++)
        {
            TB_TEST_EXPECT_UINT(members[index].id, !=, members[other].id);
        }
    }
}

TB_TEST(platform_thread, a_thread_local_variable_is_separate_in_every_thread)
{
    gathering group = {0};
    member members[THREAD_COUNT] = {0};
    tb_thread threads[THREAD_COUNT] = {0};
    bool has_gathered = false;

    thread_local_value = MAIN_THREAD_VALUE;
    has_gathered = gather(&group, members, threads);
    release(&group, threads);

    TB_TEST_REQUIRE(has_gathered);
    TB_TEST_EXPECT(!group.has_given_up);
    /* Every thread wrote its own number while all of them were running. */
    TB_TEST_EXPECT_UINT(thread_local_value, ==, MAIN_THREAD_VALUE);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_CONTEXT("thread %zu", index);
        TB_TEST_EXPECT_UINT(members[index].local_at_start, ==, 0);
        TB_TEST_EXPECT_UINT(members[index].local_at_the_end, ==, members[index].number);
    }
    thread_local_value = 0;
}

/* ---------------------------------------------------------------------------
 * Mutexes.
 * ------------------------------------------------------------------------- */

#define LOCK_ROUND_COUNT ((size_t)20000)

typedef struct guarded_counter
{
    tb_mutex lock;                         /* Guards `count`. */
    size_t count;                          /* Raised once in every round. */
    tb_platform_atomic_size inside_count;  /* Threads between lock and unlock. */
    tb_platform_atomic_size overlap_count; /* Times a thread found another inside. */
} guarded_counter;

static void count_under_the_lock(void *argument)
{
    guarded_counter *const counter = argument;

    for (size_t round = 0; round < LOCK_ROUND_COUNT; round++)
    {
        tb_mutex_lock(&counter->lock);
        if (tb_platform_atomic_size_add(&counter->inside_count, 1) != 1)
        {
            (void)tb_platform_atomic_size_add(&counter->overlap_count, 1);
        }
        counter->count++;
        (void)tb_platform_atomic_size_subtract(&counter->inside_count, 1);
        tb_mutex_unlock(&counter->lock);
    }
}

/* Without the lock two threads are inside together within a few rounds, and
 * additions to the plain counter are lost. */
TB_TEST(platform_mutex, one_thread_at_a_time_holds_a_lock)
{
    guarded_counter counter = {0};
    tb_thread threads[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], count_under_the_lock, &counter);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&counter.overlap_count), ==, 0);
    TB_TEST_EXPECT_UINT(counter.count, ==, THREAD_COUNT * LOCK_ROUND_COUNT);
}

typedef struct attempt
{
    tb_mutex *lock;
    bool was_taken; /* tb_mutex_try_lock() returned true. */
} attempt;

/* Tries the lock from another thread than the one that may hold it, and
 * gives it up again when it got it. */
static void try_the_lock(void *argument)
{
    attempt *const tried = argument;

    tried->was_taken = tb_mutex_try_lock(tried->lock);
    if (tried->was_taken)
    {
        tb_mutex_unlock(tried->lock);
    }
}

static bool another_thread_can_take(tb_mutex *lock)
{
    attempt tried = {.lock = lock};
    tb_thread thread = {0};

    tb_thread_create(&thread, try_the_lock, &tried);
    tb_thread_join(&thread);
    return tried.was_taken;
}

TB_TEST(platform_mutex, try_lock_takes_a_free_lock_and_refuses_a_held_one)
{
    tb_mutex lock = {0};

    TB_TEST_EXPECT(another_thread_can_take(&lock));
    TB_TEST_REQUIRE(tb_mutex_try_lock(&lock));
    TB_TEST_EXPECT(!another_thread_can_take(&lock));
    tb_mutex_unlock(&lock);
    TB_TEST_EXPECT(another_thread_can_take(&lock));

    tb_mutex_lock(&lock);
    TB_TEST_EXPECT(!another_thread_can_take(&lock));
    tb_mutex_unlock(&lock);
    TB_TEST_EXPECT(another_thread_can_take(&lock));
}

/* ---------------------------------------------------------------------------
 * Condition variables.
 * ------------------------------------------------------------------------- */

#define PRODUCER_COUNT 4
#define CONSUMER_COUNT 4
#define VALUES_PER_PRODUCER ((size_t)5000)
#define VALUE_COUNT (PRODUCER_COUNT * VALUES_PER_PRODUCER)

/* Small, so that producers find the queue full and consumers find it empty
 * all the time, and both kinds of wait happen. */
#define QUEUE_CAPACITY ((size_t)4)

typedef struct queue
{
    tb_mutex lock;          /* Guards the fields below. */
    tb_condition has_room;  /* Woken when a value is taken. */
    tb_condition has_value; /* Woken when a value is put, and when the queue closes. */
    size_t values[QUEUE_CAPACITY];
    size_t first;                      /* Index of the oldest value. */
    size_t count;                      /* Values in the queue. */
    bool is_not_full;                  /* count < QUEUE_CAPACITY. */
    bool is_ready;                     /* count > 0, or the queue is closed. */
    bool is_closed;                    /* No more values will be put. */
    bool has_given_up;                 /* A thread stopped waiting. */
    uint8_t taken_counts[VALUE_COUNT]; /* Times each value was taken. */
} queue;

typedef struct producer
{
    queue *shared;
    size_t first_value;
} producer;

static void update_queue_flags(queue *shared)
{
    shared->is_not_full = shared->count < QUEUE_CAPACITY;
    shared->is_ready = shared->count > 0 || shared->is_closed;
}

static void produce(void *argument)
{
    const producer *const self = argument;
    queue *const shared = self->shared;

    for (size_t offset = 0; offset < VALUES_PER_PRODUCER; offset++)
    {
        tb_mutex_lock(&shared->lock);
        if (!wait_until(&shared->has_room, &shared->lock, &shared->is_not_full))
        {
            shared->has_given_up = true;
            tb_mutex_unlock(&shared->lock);
            return;
        }
        shared->values[(shared->first + shared->count) % QUEUE_CAPACITY] =
            self->first_value + offset;
        shared->count++;
        update_queue_flags(shared);
        tb_mutex_unlock(&shared->lock);
        /* Woken without the lock held, which badger/thread.h allows. */
        tb_condition_wake_one(&shared->has_value);
    }
}

static void consume(void *argument)
{
    queue *const shared = argument;

    tb_mutex_lock(&shared->lock);
    for (;;)
    {
        if (!wait_until(&shared->has_value, &shared->lock, &shared->is_ready))
        {
            shared->has_given_up = true;
            break;
        }
        if (shared->count == 0)
        {
            /* Closed and empty. */
            break;
        }
        shared->taken_counts[shared->values[shared->first]]++;
        shared->first = (shared->first + 1) % QUEUE_CAPACITY;
        shared->count--;
        update_queue_flags(shared);
        /* Woken with the lock held, which it also allows. */
        tb_condition_wake_one(&shared->has_room);
    }
    tb_mutex_unlock(&shared->lock);
}

TB_TEST(platform_condition, every_value_passes_from_a_producer_to_one_consumer)
{
    queue shared = {0};
    producer producers[PRODUCER_COUNT] = {0};
    tb_thread producer_threads[PRODUCER_COUNT] = {0};
    tb_thread consumer_threads[CONSUMER_COUNT] = {0};
    size_t wrong_count = 0;

    update_queue_flags(&shared);
    for (size_t index = 0; index < CONSUMER_COUNT; index++)
    {
        tb_thread_create(&consumer_threads[index], consume, &shared);
    }
    for (size_t index = 0; index < PRODUCER_COUNT; index++)
    {
        producers[index].shared = &shared;
        producers[index].first_value = index * VALUES_PER_PRODUCER;
        tb_thread_create(&producer_threads[index], produce, &producers[index]);
    }
    for (size_t index = 0; index < PRODUCER_COUNT; index++)
    {
        tb_thread_join(&producer_threads[index]);
    }
    tb_mutex_lock(&shared.lock);
    shared.is_closed = true;
    update_queue_flags(&shared);
    tb_condition_wake_all(&shared.has_value);
    tb_mutex_unlock(&shared.lock);
    for (size_t index = 0; index < CONSUMER_COUNT; index++)
    {
        tb_thread_join(&consumer_threads[index]);
    }

    TB_TEST_EXPECT(!shared.has_given_up);
    TB_TEST_EXPECT_UINT(shared.count, ==, 0);
    for (size_t value = 0; value < VALUE_COUNT; value++)
    {
        if (shared.taken_counts[value] != 1)
        {
            wrong_count++;
        }
    }
    TB_TEST_EXPECT_UINT(wrong_count, ==, 0);
}

typedef struct sleepers
{
    tb_mutex lock;         /* Guards the fields below. */
    tb_condition arrival;  /* Woken when `waiting_count` rises. */
    tb_condition wake;     /* Where the sleepers wait. */
    size_t waiting_count;  /* Sleepers that are waiting on `wake`. */
    bool are_all_waiting;  /* waiting_count == THREAD_COUNT. */
    bool is_time_to_wake;  /* The sleepers may go on. */
    size_t woken_count;    /* Sleepers that went on. */
    size_t given_up_count; /* Sleepers that stopped waiting. */
} sleepers;

static void sleep_until_woken(void *argument)
{
    sleepers *const group = argument;

    tb_mutex_lock(&group->lock);
    group->waiting_count++;
    group->are_all_waiting = group->waiting_count == THREAD_COUNT;
    tb_condition_wake_all(&group->arrival);
    /* The lock is held from the count above until the wait gives it up, so
     * whoever sees the count is certain that this thread is waiting. */
    if (wait_until(&group->wake, &group->lock, &group->is_time_to_wake))
    {
        group->woken_count++;
    }
    else
    {
        group->given_up_count++;
    }
    tb_mutex_unlock(&group->lock);
}

TB_TEST(platform_condition, wake_all_wakes_every_waiting_thread)
{
    sleepers group = {0};
    tb_thread threads[THREAD_COUNT] = {0};

    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], sleep_until_woken, &group);
    }
    tb_mutex_lock(&group.lock);
    /* The wait without a time limit: a lost wake here hangs the case. */
    while (!group.are_all_waiting)
    {
        tb_condition_wait(&group.arrival, &group.lock);
    }
    group.is_time_to_wake = true;
    tb_condition_wake_all(&group.wake);
    tb_mutex_unlock(&group.lock);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    TB_TEST_EXPECT_UINT(group.woken_count, ==, THREAD_COUNT);
    TB_TEST_EXPECT_UINT(group.given_up_count, ==, 0);
}

/* A wait that nothing wakes. Long enough to tell from no wait at all, on a
 * machine whose timer ticks every 16 milliseconds. */
#define LIMIT_NANOSECONDS (UINT64_C(50) * NANOSECONDS_PER_MILLISECOND)

/* Waits that may end early, with nobody to wake them, before the case
 * concludes that the limit is not being kept at all. */
#define EARLY_RETURN_COUNT_MAX 1000

TB_TEST(platform_condition, a_wait_with_a_time_limit_lasts_at_least_the_limit)
{
    tb_mutex lock = {0};
    tb_condition never_woken = {0};
    uint64_t start = {0};
    uint64_t waited = {0};
    int early_return_count = 0;

    tb_mutex_lock(&lock);
    TB_TEST_EXPECT(!tb_condition_wait_for(&never_woken, &lock, 0));

    start = tb_platform_clock_nanoseconds();
    while (tb_condition_wait_for(&never_woken, &lock, LIMIT_NANOSECONDS))
    {
        early_return_count++;
        if (early_return_count > EARLY_RETURN_COUNT_MAX)
        {
            tb_mutex_unlock(&lock);
            TB_TEST_FAIL("the wait keeps ending early with nobody to wake it");
        }
        start = tb_platform_clock_nanoseconds();
    }
    waited = tb_platform_clock_nanoseconds() - start;
    TB_TEST_EXPECT_UINT(waited, >=, LIMIT_NANOSECONDS);
    TB_TEST_EXPECT_UINT(waited, <, PATIENCE_NANOSECONDS);
    /* The lock is held again after the wait. */
    TB_TEST_EXPECT(!another_thread_can_take(&lock));
    tb_mutex_unlock(&lock);
}

typedef struct limited_waiter
{
    tb_mutex lock;        /* Guards the fields below. */
    tb_condition arrival; /* Woken when `is_waiting` becomes true. */
    tb_condition wake;    /* Where the waiter waits. */
    bool is_waiting;      /* The waiter is waiting on `wake`. */
    bool is_time_to_wake;
    bool was_woken;  /* The wait ended before its limit. */
    uint64_t waited; /* Nanoseconds the waiter spent waiting. */
} limited_waiter;

static void wait_with_a_long_limit(void *argument)
{
    limited_waiter *const waiter = argument;
    uint64_t start = {0};

    tb_mutex_lock(&waiter->lock);
    waiter->is_waiting = true;
    tb_condition_wake_one(&waiter->arrival);
    /* The lock is held until the wait gives it up, so whoever sees the flag
     * is certain that this thread is waiting. */
    start = tb_platform_clock_nanoseconds();
    waiter->was_woken = wait_until(&waiter->wake, &waiter->lock, &waiter->is_time_to_wake);
    waiter->waited = tb_platform_clock_nanoseconds() - start;
    tb_mutex_unlock(&waiter->lock);
}

TB_TEST(platform_condition, a_wait_with_a_time_limit_ends_when_it_is_woken)
{
    limited_waiter waiter = {0};
    tb_thread thread = {0};

    tb_thread_create(&thread, wait_with_a_long_limit, &waiter);
    tb_mutex_lock(&waiter.lock);
    while (!waiter.is_waiting)
    {
        tb_condition_wait(&waiter.arrival, &waiter.lock);
    }
    waiter.is_time_to_wake = true;
    tb_mutex_unlock(&waiter.lock);
    tb_condition_wake_one(&waiter.wake);
    tb_thread_join(&thread);

    TB_TEST_EXPECT(waiter.was_woken);
    TB_TEST_EXPECT_UINT(waiter.waited, <, PATIENCE_NANOSECONDS);
}

typedef struct patient_waiter
{
    tb_mutex lock;        /* Guards the fields below. */
    tb_condition arrival; /* Woken when `is_waiting` becomes true. */
    tb_condition wake;    /* Where the waiter waits. */
    uint64_t limit;       /* Nanoseconds that the waiter is prepared to wait. */
    bool is_waiting;      /* The waiter is waiting on `wake`. */
    bool is_time_to_wake;
    bool was_woken; /* No wait ended at its limit. */
} patient_waiter;

static void wait_with_the_limit_it_was_given(void *argument)
{
    patient_waiter *const waiter = argument;

    tb_mutex_lock(&waiter->lock);
    waiter->is_waiting = true;
    waiter->was_woken = true;
    tb_condition_wake_one(&waiter->arrival);
    /* Again after a wake that nobody sent, which a wait is allowed. */
    while (!waiter->is_time_to_wake && waiter->was_woken)
    {
        waiter->was_woken = tb_condition_wait_for(&waiter->wake, &waiter->lock, waiter->limit);
    }
    tb_mutex_unlock(&waiter->lock);
}

/* A limit is any number of nanoseconds. An adapter turns it into a moment on
 * the system's clock, and two things can go wrong on the way: the moment is
 * later than the system's time can hold, and the nanoseconds of the limit
 * and of the time now add up to more than a second. A wait with such a limit
 * must still begin, and still end when it is woken. */
TB_TEST(platform_condition, a_wait_with_any_limit_ends_when_it_is_woken)
{
    static const uint64_t limits[] = {
        UINT64_MAX,                    /* 584 years. */
        UINT64_C(4000000000000000000), /* 126 years, a whole number of seconds. */
        UINT64_C(60999999999),         /* A minute, less a nanosecond: carries into the seconds. */
        UINT64_C(60000000001),         /* A minute and a nanosecond: almost never carries. */
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(limits); index++)
    {
        patient_waiter waiter = {.limit = limits[index]};
        tb_thread thread = {0};

        TB_TEST_CONTEXT("a limit of %llu nanoseconds", (unsigned long long)limits[index]);
        tb_thread_create(&thread, wait_with_the_limit_it_was_given, &waiter);
        tb_mutex_lock(&waiter.lock);
        while (!waiter.is_waiting)
        {
            tb_condition_wait(&waiter.arrival, &waiter.lock);
        }
        waiter.is_time_to_wake = true;
        tb_mutex_unlock(&waiter.lock);
        tb_condition_wake_one(&waiter.wake);
        tb_thread_join(&thread);
        TB_TEST_EXPECT(waiter.was_woken);
    }
}
