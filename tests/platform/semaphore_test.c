/* A semaphore promises two things. It counts: every signal is taken by
 * exactly one wait, none is lost and none is taken twice, whatever threads
 * signal and wait at the same moment. And it remembers: a signal that nobody
 * was waiting for is there for the next wait.
 *
 * A thread started by a case only records; the case checks after joining. */
#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>

#define NANOSECONDS_PER_MILLISECOND UINT64_C(1000000)
/* Long enough that only a wait that is never going to end takes this long. */
#define PATIENCE_NANOSECONDS (UINT64_C(20000) * NANOSECONDS_PER_MILLISECOND)

TB_TEST(platform_semaphore, a_signal_is_remembered_until_it_is_taken)
{
    tb_semaphore semaphore = {0};

    /* A zeroed semaphore has no signals. */
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));
    TB_TEST_EXPECT(!tb_semaphore_wait_for(&semaphore, 0));

    tb_semaphore_signal(&semaphore, 3);
    tb_semaphore_signal(&semaphore, 0);
    TB_TEST_EXPECT(tb_semaphore_try_wait(&semaphore));
    TB_TEST_EXPECT(tb_semaphore_wait_for(&semaphore, 0));
    tb_semaphore_wait(&semaphore);
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));

    /* One at a time, and with a limit that would be a long wait. */
    tb_semaphore_signal(&semaphore, 1);
    TB_TEST_EXPECT(tb_semaphore_wait_for(&semaphore, PATIENCE_NANOSECONDS));
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));
}

#define LIMIT_NANOSECONDS (UINT64_C(30) * NANOSECONDS_PER_MILLISECOND)

TB_TEST(platform_semaphore, a_wait_with_a_time_limit_lasts_at_least_the_limit)
{
    tb_semaphore semaphore = {0};
    const uint64_t start = tb_platform_clock_nanoseconds();
    uint64_t waited = 0;

    TB_TEST_EXPECT(!tb_semaphore_wait_for(&semaphore, LIMIT_NANOSECONDS));
    waited = tb_platform_clock_nanoseconds() - start;
    TB_TEST_EXPECT_UINT(waited, >=, LIMIT_NANOSECONDS);
    TB_TEST_EXPECT_UINT(waited, <, PATIENCE_NANOSECONDS);
    /* Nothing was taken, and nothing is owed. */
    tb_semaphore_signal(&semaphore, 1);
    TB_TEST_EXPECT(tb_semaphore_try_wait(&semaphore));
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));
}

typedef struct sleeper
{
    tb_semaphore *semaphore;
    uint64_t limit;                     /* 0 to wait without one. */
    tb_platform_atomic_size is_started; /* 1 once the thread is about to wait. */
    bool was_signalled;
} sleeper;

static void wait_for_a_signal(void *argument)
{
    sleeper *const self = argument;

    tb_platform_atomic_size_store(&self->is_started, 1);
    if (self->limit == 0)
    {
        tb_semaphore_wait(self->semaphore);
        self->was_signalled = true;
        return;
    }
    self->was_signalled = tb_semaphore_wait_for(self->semaphore, self->limit);
}

/* A thread that is waiting, with a limit and without, is woken by a signal
 * that comes after it began to wait. */
TB_TEST(platform_semaphore, a_waiting_thread_is_woken_by_a_signal)
{
    const uint64_t limits[] = {0, PATIENCE_NANOSECONDS, UINT64_MAX};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(limits); index++)
    {
        tb_semaphore semaphore = {0};
        sleeper self = {.semaphore = &semaphore, .limit = limits[index]};
        tb_thread thread = {0};
        uint64_t start = 0;

        TB_TEST_CONTEXT("a limit of %llu nanoseconds", (unsigned long long)limits[index]);
        tb_thread_create(&thread, wait_for_a_signal, &self);
        while (tb_platform_atomic_size_load(&self.is_started) == 0)
        {
            tb_thread_relax();
        }
        /* Give it a moment to be waiting, and not only about to. */
        start = tb_platform_clock_nanoseconds();
        while (tb_platform_clock_nanoseconds() - start < NANOSECONDS_PER_MILLISECOND)
        {
            tb_thread_yield();
        }
        tb_semaphore_signal(&semaphore, 1);
        tb_thread_join(&thread);
        TB_TEST_EXPECT(self.was_signalled);
        TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));
    }
}

#define TRADER_COUNT 4
#define SIGNALS_PER_TRADER ((size_t)6000)

typedef struct trader
{
    tb_semaphore *semaphore;
    size_t number; /* Which of its kind this thread is, from 0. */
} trader;

/* Signals SIGNALS_PER_TRADER times in all, in ones and in batches. */
static void give_signals(void *argument)
{
    const trader *const self = argument;
    size_t given = 0;

    while (given < SIGNALS_PER_TRADER)
    {
        size_t batch = 1 + (given + self->number) % 5;

        if (batch > SIGNALS_PER_TRADER - given)
        {
            batch = SIGNALS_PER_TRADER - given;
        }
        tb_semaphore_signal(self->semaphore, batch);
        given += batch;
    }
}

/* Takes exactly as many signals as one thread gives, each in one of the three
 * ways there are to take one. */
static void take_signals(void *argument)
{
    const trader *const self = argument;

    for (size_t taken = 0; taken < SIGNALS_PER_TRADER; taken++)
    {
        const size_t way = (taken + self->number) % 3;

        if (way == 0)
        {
            tb_semaphore_wait(self->semaphore);
        }
        else if (way == 1)
        {
            while (!tb_semaphore_wait_for(self->semaphore, PATIENCE_NANOSECONDS))
            {
            }
        }
        else
        {
            while (!tb_semaphore_try_wait(self->semaphore))
            {
                tb_thread_yield();
            }
        }
    }
}

/* Four threads give 24,000 signals between them and four take as many. If a
 * signal were lost, a taker would wait for ever; if one were taken twice,
 * signals would be left over at the end. */
TB_TEST(platform_semaphore, signals_are_neither_lost_nor_taken_twice)
{
    tb_semaphore semaphore = {0};
    trader givers[TRADER_COUNT] = {0};
    trader takers[TRADER_COUNT] = {0};
    tb_thread giving[TRADER_COUNT] = {0};
    tb_thread taking[TRADER_COUNT] = {0};

    for (size_t index = 0; index < TRADER_COUNT; index++)
    {
        takers[index].semaphore = &semaphore;
        takers[index].number = index;
        tb_thread_create(&taking[index], take_signals, &takers[index]);
    }
    for (size_t index = 0; index < TRADER_COUNT; index++)
    {
        givers[index].semaphore = &semaphore;
        givers[index].number = index;
        tb_thread_create(&giving[index], give_signals, &givers[index]);
    }
    for (size_t index = 0; index < TRADER_COUNT; index++)
    {
        tb_thread_join(&giving[index]);
        tb_thread_join(&taking[index]);
    }
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));
}

#define GUEST_COUNT 8
#define ROOM_SIZE ((size_t)3)
#define VISITS ((size_t)1500)

typedef struct room
{
    tb_semaphore places;             /* A signal for each free place. */
    tb_platform_atomic_size inside;  /* Threads in the room now. */
    tb_platform_atomic_size crowded; /* Times a thread found too many inside. */
} room;

static void visit_the_room(void *argument)
{
    room *const shared = argument;

    for (size_t visit = 0; visit < VISITS; visit++)
    {
        tb_semaphore_wait(&shared->places);
        if (tb_platform_atomic_size_add(&shared->inside, 1) > ROOM_SIZE)
        {
            (void)tb_platform_atomic_size_add(&shared->crowded, 1);
        }
        tb_thread_relax();
        (void)tb_platform_atomic_size_subtract(&shared->inside, 1);
        tb_semaphore_signal(&shared->places, 1);
    }
}

/* A semaphore with three signals lets three threads through at a time, which
 * is how a pool of three things is shared between eight threads. */
TB_TEST(platform_semaphore, no_more_threads_pass_than_there_are_signals)
{
    room shared = {0};
    tb_thread threads[GUEST_COUNT] = {0};

    tb_semaphore_signal(&shared.places, ROOM_SIZE);
    for (size_t index = 0; index < GUEST_COUNT; index++)
    {
        tb_thread_create(&threads[index], visit_the_room, &shared);
    }
    for (size_t index = 0; index < GUEST_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&shared.crowded), ==, 0);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&shared.inside), ==, 0);
    /* Every place was given back. */
    for (size_t place = 0; place < ROOM_SIZE; place++)
    {
        TB_TEST_EXPECT(tb_semaphore_try_wait(&shared.places));
    }
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&shared.places));
}

/* --- contracts ------------------------------------------------------------------- */

static void signal_null(void *context)
{
    (void)context;
    tb_semaphore_signal(NULL, 1);
}

static void wait_on_null(void *context)
{
    (void)context;
    tb_semaphore_wait(NULL);
}

static void try_null(void *context)
{
    (void)context;
    (void)tb_semaphore_try_wait(NULL);
}

static void wait_for_null(void *context)
{
    (void)context;
    (void)tb_semaphore_wait_for(NULL, 0);
}

static void signal_past_the_limit(void *context)
{
    tb_semaphore_signal(context, SIZE_MAX);
}

TB_TEST(platform_semaphore, bad_arguments_violate_the_contract)
{
    tb_semaphore semaphore = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the platform adapter was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(signal_null, NULL, "semaphore != NULL");
    TB_TEST_EXPECT_TRAP(wait_on_null, NULL, "semaphore != NULL");
    TB_TEST_EXPECT_TRAP(try_null, NULL, "semaphore != NULL");
    TB_TEST_EXPECT_TRAP(wait_for_null, NULL, "semaphore != NULL");

    /* More signals than can be counted are refused, and none of them is
     * added: the semaphore is as it was, and still works. */
    tb_semaphore_signal(&semaphore, 2);
    TB_TEST_EXPECT_TRAP(signal_past_the_limit, &semaphore, "cannot count more signals");
    TB_TEST_EXPECT(tb_semaphore_try_wait(&semaphore));
    TB_TEST_EXPECT(tb_semaphore_try_wait(&semaphore));
    TB_TEST_EXPECT(!tb_semaphore_try_wait(&semaphore));
}
