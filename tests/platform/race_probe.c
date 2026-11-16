/* Shows that ThreadSanitizer sees a data race in this build, and that it
 * accepts each thing the engine offers to prevent one.
 *
 * A sanitized run that reports nothing proves something only if the
 * sanitizer would have reported a race. The first case races on purpose and
 * must be reported. The others do the same work behind a mutex, an atomic
 * value, the start and end of a thread, and an atomic pointer, and must not
 * be: a report there would mean that the sanitizer does not understand the
 * engine's own synchronization, and every report from the real tests would be
 * in doubt.
 *
 * ExpectDataRace.cmake runs each case as a child process and checks how it
 * ends. They are not registered with CTest directly. */
#include "badger/platform.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>

#define WRITER_COUNT 2

/* Enough writes that the two writers are running at the same time. The
 * sanitizer does not need them to be: it reports two writes that nothing
 * orders, whenever they happen. */
#define WRITE_COUNT ((size_t)1000)

typedef struct tally
{
    tb_mutex lock;                        /* Guards `count` in the case that locks. */
    size_t count;                         /* Written by every writer. */
    tb_platform_atomic_size atomic_count; /* The same, as an atomic value. */
} tally;

static void run_writers(tb_thread_function *write, tally *shared)
{
    tb_thread threads[WRITER_COUNT] = {0};

    for (size_t index = 0; index < WRITER_COUNT; index++)
    {
        tb_thread_create(&threads[index], write, shared);
    }
    for (size_t index = 0; index < WRITER_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
}

static void write_with_nothing_to_order_it(void *argument)
{
    tally *const shared = argument;

    for (size_t round = 0; round < WRITE_COUNT; round++)
    {
        shared->count++;
    }
}

/* The race. Additions are lost when the writers meet, so the count is not
 * checked: the sanitizer's report is the result. */
TB_TEST(race_probe, unsynchronized_writes)
{
    tally shared = {0};

    run_writers(write_with_nothing_to_order_it, &shared);
}

static void write_under_the_mutex(void *argument)
{
    tally *const shared = argument;

    for (size_t round = 0; round < WRITE_COUNT; round++)
    {
        tb_mutex_lock(&shared->lock);
        shared->count++;
        tb_mutex_unlock(&shared->lock);
    }
}

/* The mutex is zero-initialized and first locked by both writers, so this is
 * also the first use that Linux and macOS have to make the system's lock in. */
TB_TEST(race_probe, writes_under_a_mutex)
{
    tally shared = {0};

    run_writers(write_under_the_mutex, &shared);
    TB_TEST_EXPECT_UINT(shared.count, ==, WRITER_COUNT * WRITE_COUNT);
}

static void write_atomically(void *argument)
{
    tally *const shared = argument;

    for (size_t round = 0; round < WRITE_COUNT; round++)
    {
        (void)tb_platform_atomic_size_add(&shared->atomic_count, 1);
    }
}

TB_TEST(race_probe, atomic_updates)
{
    tally shared = {0};

    run_writers(write_atomically, &shared);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&shared.atomic_count), ==,
                        WRITER_COUNT * WRITE_COUNT);
}

/* One thread at a time: the case writes, then a thread, then the case again.
 * Starting a thread and joining it are what order the three. */
TB_TEST(race_probe, writes_before_a_thread_starts_and_after_it_ends)
{
    tally shared = {0};
    tb_thread thread = {0};

    shared.count = 1;
    tb_thread_create(&thread, write_with_nothing_to_order_it, &shared);
    tb_thread_join(&thread);
    shared.count++;
    TB_TEST_EXPECT_UINT(shared.count, ==, WRITE_COUNT + 2);
}

typedef struct mailbox
{
    tb_platform_atomic_pointer letter; /* NULL until the writer publishes. */
    size_t written;                    /* What the letter points to. */
    size_t read;                       /* What the reader found there. */
} mailbox;

#define LETTER_VALUE ((size_t)0xC0FFEE)

static void publish_a_letter(void *argument)
{
    mailbox *const box = argument;

    /* Written before the pointer to it is published, with no lock. */
    box->written = LETTER_VALUE;
    tb_platform_atomic_pointer_store(&box->letter, &box->written);
}

static void read_the_letter(void *argument)
{
    mailbox *const box = argument;
    const size_t *letter = NULL;

    while (letter == NULL)
    {
        letter = tb_platform_atomic_pointer_load(&box->letter);
    }
    box->read = *letter;
}

/* How a lock-free queue hands an object from one thread to another: the
 * plain write is ordered before the plain read by the atomic store and load
 * between them. */
TB_TEST(race_probe, an_object_published_through_an_atomic_pointer)
{
    mailbox box = {0};
    tb_thread reader = {0};
    tb_thread writer = {0};

    tb_thread_create(&reader, read_the_letter, &box);
    tb_thread_create(&writer, publish_a_letter, &box);
    tb_thread_join(&writer);
    tb_thread_join(&reader);
    TB_TEST_EXPECT_UINT(box.read, ==, LETTER_VALUE);
}
