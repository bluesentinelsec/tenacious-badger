/* The queue that a worker thread keeps its work in is the part of the job
 * system that has no lock, so it is the part where a mistake hides best. It
 * is checked three ways:
 *
 * - On one thread, against a queue that is too simple to be wrong: an array
 *   and two counts. Random sequences of pushes, pops, and steals must give
 *   the same answers from both, at every capacity and wherever the counts of
 *   the two ends start, including where they wrap around.
 * - With an owner and several thieves at work at once, where every item that
 *   is pushed must come out exactly once, to the owner or to one thief.
 * - The same with a queue of one slot, where every item is the last one and
 *   the owner and the thieves race for each.
 *
 * A thread started by a case only records; the case checks after joining. */
#include "jobs/deque.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/system.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * One thread, against a model.
 * ------------------------------------------------------------------------- */

#define MODEL_CAPACITY_MAX ((size_t)64)

/* Items are addresses in this array, so that none of them is NULL and each
 * says which it is. */
static char item_storage[4096] = {0};

static void *item_number(size_t number)
{
    return &item_storage[number % sizeof(item_storage)];
}

/* A queue with both ends in the open: the oldest item is at `first`, and
 * there are `count` of them. */
typedef struct model
{
    void *items[MODEL_CAPACITY_MAX];
    size_t capacity;
    size_t first;
    size_t count;
} model;

static bool model_push(model *queue, void *item)
{
    if (queue->count == queue->capacity)
    {
        return false;
    }
    queue->items[(queue->first + queue->count) % queue->capacity] = item;
    queue->count++;
    return true;
}

static void *model_pop(model *queue)
{
    if (queue->count == 0)
    {
        return NULL;
    }
    queue->count--;
    return queue->items[(queue->first + queue->count) % queue->capacity];
}

static void *model_steal(model *queue)
{
    void *item = NULL;

    if (queue->count == 0)
    {
        return NULL;
    }
    item = queue->items[queue->first];
    queue->first = (queue->first + 1) % queue->capacity;
    queue->count--;
    return item;
}

#define OPERATION_COUNT ((size_t)20000)

/* Of every CHOICE_COUNT operations, how many are pushes: most while the
 * queue is being filled, few while it is being emptied. The rest are pops
 * and steals in equal parts. */
#define CHOICE_COUNT 8
#define PUSH_CHOICES_WHILE_FILLING 6
#define PUSH_CHOICES_WHILE_EMPTYING 2

/* Runs random operations on a queue of `capacity` whose counts start at
 * `first_count`, and on the model, and compares every answer. */
static void compare_with_the_model(size_t capacity, size_t first_count)
{
    tb_platform_atomic_pointer slots[MODEL_CAPACITY_MAX] = {0};
    tb_job_deque deque = {0};
    model expected = {.capacity = capacity};
    size_t next_item = 1;

    tb_job_deque_init_at(&deque, slots, capacity, first_count);
    /* The counts start where they were asked to, or the cases that start
     * them near the largest count would test nothing of the kind. */
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&deque.top), ==, first_count);
    TB_TEST_EXPECT_UINT(tb_platform_atomic_size_load(&deque.bottom), ==, first_count);
    for (size_t step = 0; step < OPERATION_COUNT; step++)
    {
        /* Runs of pushes and runs of removals, so that the queue fills up
         * and runs dry again and again. */
        const bool is_filling = ((step / (capacity * 3 + 1)) % 2) == 0;
        const uint64_t choice = tb_test_random_below(CHOICE_COUNT);
        uint64_t push_choices = PUSH_CHOICES_WHILE_EMPTYING;

        if (is_filling)
        {
            push_choices = PUSH_CHOICES_WHILE_FILLING;
        }
        TB_TEST_CONTEXT("capacity %zu, first count %zu, step %zu", capacity, first_count, step);
        if (choice < push_choices)
        {
            void *const item = item_number(next_item);

            next_item++;
            TB_TEST_EXPECT(tb_job_deque_push(&deque, item) == model_push(&expected, item));
        }
        else if (choice % 2 == 0)
        {
            TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, model_pop(&expected));
        }
        else
        {
            TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, model_steal(&expected));
        }
        TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, expected.count);
        if (tb_test_case_has_failed())
        {
            return;
        }
    }
}

TB_TEST(jobs_deque, an_empty_queue_gives_nothing)
{
    tb_platform_atomic_pointer slots[4] = {0};
    tb_job_deque deque = {0};

    tb_job_deque_init(&deque, slots, TB_TEST_COUNT_OF(slots));
    TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, 0);
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, NULL);
    /* Asking an empty queue changes nothing: it still takes and gives. */
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, NULL);
    TB_TEST_REQUIRE(tb_job_deque_push(&deque, item_number(1)));
    TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, 1);
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, item_number(1));
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, 0);
}

TB_TEST(jobs_deque, the_owner_takes_the_newest_and_a_thief_the_oldest)
{
    tb_platform_atomic_pointer slots[8] = {0};
    tb_job_deque deque = {0};

    tb_job_deque_init(&deque, slots, TB_TEST_COUNT_OF(slots));
    for (size_t number = 1; number <= 5; number++)
    {
        TB_TEST_REQUIRE(tb_job_deque_push(&deque, item_number(number)));
    }
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, item_number(5));
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, item_number(1));
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, item_number(4));
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, item_number(2));
    TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, 1);
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, item_number(3));
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, NULL);
}

TB_TEST(jobs_deque, a_full_queue_refuses_an_item_and_keeps_what_it_has)
{
    tb_platform_atomic_pointer slots[4] = {0};
    tb_job_deque deque = {0};

    tb_job_deque_init(&deque, slots, TB_TEST_COUNT_OF(slots));
    for (size_t number = 1; number <= 4; number++)
    {
        TB_TEST_REQUIRE(tb_job_deque_push(&deque, item_number(number)));
    }
    TB_TEST_EXPECT(!tb_job_deque_push(&deque, item_number(5)));
    TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, 4);
    /* Room at either end is room. */
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, item_number(1));
    TB_TEST_EXPECT(tb_job_deque_push(&deque, item_number(6)));
    TB_TEST_EXPECT(!tb_job_deque_push(&deque, item_number(7)));
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, item_number(6));
    TB_TEST_EXPECT_POINTER(tb_job_deque_pop(&deque), ==, item_number(4));
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, item_number(2));
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, item_number(3));
    TB_TEST_EXPECT_POINTER(tb_job_deque_steal(&deque), ==, NULL);
}

TB_TEST(jobs_deque, random_operations_agree_with_a_simple_queue_at_every_capacity)
{
    for (size_t capacity = 1; capacity <= MODEL_CAPACITY_MAX; capacity *= 2)
    {
        compare_with_the_model(capacity, 0);
    }
}

/* The counts of the two ends are unsigned and wrap around. A queue that
 * starts just below the largest count crosses it within a few items, with
 * the ends on either side of it for a while. */
TB_TEST(jobs_deque, the_counts_of_the_ends_wrap_around)
{
    const size_t first_counts[] = {
        SIZE_MAX, SIZE_MAX - 1, SIZE_MAX - 3, SIZE_MAX - 63, SIZE_MAX / 2, SIZE_MAX / 2 + 1, 1};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(first_counts); index++)
    {
        for (size_t capacity = 1; capacity <= MODEL_CAPACITY_MAX; capacity *= 4)
        {
            compare_with_the_model(capacity, first_counts[index]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * An owner and thieves.
 * ------------------------------------------------------------------------- */

#define THIEF_COUNT 7
#define SHARED_ITEM_COUNT ((size_t)200000)

/* Times each item was received, by anyone. An item is its index here, plus
 * one so that the first is not NULL. */
typedef struct shared_queue
{
    tb_job_deque deque;
    /* 1 once the owner has pushed and popped all it will. */
    tb_platform_atomic_size is_done;
    /* Thieves that are running. */
    tb_platform_atomic_size thieves_started;
    /* Items received by anyone. */
    tb_platform_atomic_size received_count;
    /* By item: times it was received. */
    uint8_t *received;
    size_t item_count;
} shared_queue;

/* What one thread received. Each thread has its own, so that nothing but the
 * queue is shared while they run, and the case adds them up afterwards. */
typedef struct receiver
{
    shared_queue *shared;
    size_t *items; /* The items this thread received, in order. */
    size_t count;
    size_t capacity;
    bool has_overflowed; /* It received more than can be: something was duplicated. */
} receiver;

static void receive(receiver *self, void *item)
{
    if (self->count == self->capacity)
    {
        self->has_overflowed = true;
        return;
    }
    self->items[self->count] = (size_t)(uintptr_t)item;
    self->count++;
    (void)tb_platform_atomic_size_add(&self->shared->received_count, 1);
}

static void steal_until_done(void *argument)
{
    receiver *const self = argument;
    shared_queue *const shared = self->shared;

    (void)tb_platform_atomic_size_add(&shared->thieves_started, 1);
    for (;;)
    {
        void *const item = tb_job_deque_steal(&shared->deque);

        if (item != NULL)
        {
            receive(self, item);
            continue;
        }
        /* Nothing to steal: finished only if the owner has finished too,
         * and the queue is still empty after that is known. */
        if (tb_platform_atomic_size_load(&shared->is_done) != 0 &&
            tb_job_deque_count(&shared->deque) == 0)
        {
            return;
        }
    }
}

/* Pushes every item, popping some of them back, the way a worker does:
 * mostly it works alone at the bottom while thieves take from the top. */
static void push_and_pop_as_the_owner(receiver *self, size_t pops_per_push_in_eighths)
{
    shared_queue *const shared = self->shared;
    uint64_t state = 0x9E3779B97F4A7C15u;

    /* Not before every thief is at work, or a quick owner could finish
     * alone and the case would show nothing about stealing. */
    while (tb_platform_atomic_size_load(&shared->thieves_started) < THIEF_COUNT)
    {
    }
    for (size_t number = 1; number <= shared->item_count; number++)
    {
        void *const item = (void *)(uintptr_t)number;

        while (!tb_job_deque_push(&shared->deque, item))
        {
            /* Full: make room as a worker would, by doing some of the work. */
            void *const popped = tb_job_deque_pop(&shared->deque);

            if (popped != NULL)
            {
                receive(self, popped);
            }
        }
        /* A cheap sequence that does not repeat soon. */
        state = state * 6364136223846793005u + 1442695040888963407u;
        if ((state >> 61) < pops_per_push_in_eighths)
        {
            void *const popped = tb_job_deque_pop(&shared->deque);

            if (popped != NULL)
            {
                receive(self, popped);
            }
        }
    }
    for (;;)
    {
        void *const popped = tb_job_deque_pop(&shared->deque);

        if (popped == NULL)
        {
            break;
        }
        receive(self, popped);
    }
    tb_platform_atomic_size_store(&shared->is_done, 1);
}

typedef struct tally
{
    size_t missing_count;    /* Items that nobody received. */
    size_t duplicated_count; /* Items received more than once. */
    size_t unknown_count;    /* Things received that were never pushed. */
    size_t stolen_count;     /* Items that a thief received. */
    bool has_overflowed;
} tally;

/* Runs an owner and THIEF_COUNT thieves over a queue of `capacity`, and
 * counts what became of the items. */
static tally run_owner_and_thieves(size_t capacity, size_t item_count,
                                   size_t pops_per_push_in_eighths)
{
    tb_platform_atomic_pointer slots[MODEL_CAPACITY_MAX] = {0};
    shared_queue shared = {.item_count = item_count};
    receiver receivers[THIEF_COUNT + 1] = {0};
    tb_thread threads[THIEF_COUNT] = {0};
    tally result = {0};

    tb_job_deque_init(&shared.deque, slots, capacity);
    shared.received = tb_calloc(item_count + 1, sizeof(uint8_t));
    for (size_t index = 0; index <= THIEF_COUNT; index++)
    {
        receivers[index].shared = &shared;
        receivers[index].capacity = item_count;
        receivers[index].items = tb_calloc(item_count, sizeof(size_t));
    }
    for (size_t index = 0; index < THIEF_COUNT; index++)
    {
        tb_thread_create(&threads[index], steal_until_done, &receivers[index + 1]);
    }
    push_and_pop_as_the_owner(&receivers[0], pops_per_push_in_eighths);
    for (size_t index = 0; index < THIEF_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }

    for (size_t index = 0; index <= THIEF_COUNT; index++)
    {
        const receiver *const self = &receivers[index];

        result.has_overflowed = result.has_overflowed || self->has_overflowed;
        if (index > 0)
        {
            result.stolen_count += self->count;
        }
        for (size_t position = 0; position < self->count; position++)
        {
            const size_t number = self->items[position];

            if (number == 0 || number > item_count)
            {
                result.unknown_count++;
            }
            else if (shared.received[number] < UINT8_MAX)
            {
                shared.received[number]++;
            }
        }
    }
    for (size_t number = 1; number <= item_count; number++)
    {
        if (shared.received[number] == 0)
        {
            result.missing_count++;
        }
        else if (shared.received[number] > 1)
        {
            result.duplicated_count++;
        }
    }
    for (size_t index = 0; index <= THIEF_COUNT; index++)
    {
        tb_free(receivers[index].items);
    }
    tb_free(shared.received);
    return result;
}

static void expect_every_item_once(const tally *result)
{
    TB_TEST_EXPECT(!result->has_overflowed);
    TB_TEST_EXPECT_UINT(result->missing_count, ==, 0);
    TB_TEST_EXPECT_UINT(result->duplicated_count, ==, 0);
    TB_TEST_EXPECT_UINT(result->unknown_count, ==, 0);
}

/* Whether thieves can run while the owner does. On a machine with one
 * processor the owner may finish before any thief is given a turn. */
static bool threads_run_at_the_same_time(void)
{
    tb_system_processors processors = {0};

    tb_system_get_processors(&processors);
    return processors.logical_count >= 2;
}

TB_TEST(jobs_deque, every_item_comes_out_once_with_thieves_at_work)
{
    /* The owner pops half of what it pushes; the thieves take the rest. */
    const tally result = run_owner_and_thieves(64, SHARED_ITEM_COUNT, 4);

    expect_every_item_once(&result);
    /* Some of them were stolen, or this case showed nothing about thieves. */
    if (threads_run_at_the_same_time())
    {
        TB_TEST_EXPECT_UINT(result.stolen_count, >, 0);
    }
}

/* An owner that never pops by choice leaves everything to the thieves, and
 * one that pops after nearly every push meets them at a queue that is almost
 * always down to its last item. */
TB_TEST(jobs_deque, every_item_comes_out_once_however_the_owner_and_thieves_share)
{
    const size_t pops_in_eighths[] = {0, 1, 7, 8};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(pops_in_eighths); index++)
    {
        const tally result =
            run_owner_and_thieves(16, SHARED_ITEM_COUNT / 4, pops_in_eighths[index]);

        TB_TEST_CONTEXT("the owner pops after %zu pushes in 8", pops_in_eighths[index]);
        expect_every_item_once(&result);
    }
}

/* With one slot every item is the last one, and the owner and the thieves
 * race for each. */
TB_TEST(jobs_deque, the_last_item_goes_to_the_owner_or_to_one_thief)
{
    const tally result = run_owner_and_thieves(1, SHARED_ITEM_COUNT / 4, 8);

    expect_every_item_once(&result);
    if (threads_run_at_the_same_time())
    {
        TB_TEST_EXPECT_UINT(result.stolen_count, >, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static void init_with_three_slots(void *context)
{
    tb_platform_atomic_pointer slots[4] = {0};
    tb_job_deque deque = {0};

    (void)context;
    tb_job_deque_init(&deque, slots, 3);
}

static void init_with_no_slots(void *context)
{
    tb_platform_atomic_pointer slots[1] = {0};
    tb_job_deque deque = {0};

    (void)context;
    tb_job_deque_init(&deque, slots, 0);
}

static void init_with_null_storage(void *context)
{
    tb_job_deque deque = {0};

    (void)context;
    tb_job_deque_init(&deque, NULL, 4);
}

static void push_nothing(void *context)
{
    (void)tb_job_deque_push(context, NULL);
}

TB_TEST(jobs_deque, bad_arguments_violate_the_contract)
{
    tb_platform_atomic_pointer slots[4] = {0};
    tb_job_deque deque = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    tb_job_deque_init(&deque, slots, TB_TEST_COUNT_OF(slots));
    TB_TEST_EXPECT_TRAP(init_with_three_slots, NULL, "the capacity is a power of two");
    TB_TEST_EXPECT_TRAP(init_with_no_slots, NULL, "the capacity is a power of two");
    TB_TEST_EXPECT_TRAP(init_with_null_storage, NULL, "slots != NULL");
    TB_TEST_EXPECT_TRAP(push_nothing, &deque, "an item is not NULL");
    TB_TEST_EXPECT_UINT(tb_job_deque_count(&deque), ==, 0);
}
