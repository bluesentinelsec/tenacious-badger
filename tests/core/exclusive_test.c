/* Objects whose caller serializes their use (arenas and the containers)
 * check, in builds with assertions, that one thread at a time changes them.
 *
 * Two threads are made to overlap without a race in the test itself: an
 * allocator holds one thread inside a function that grows the object, and
 * the test's thread then changes the same object. That must fail the check.
 * One thread after another, readers together, and a thread that re-enters an
 * object it is already inside must not. */
#include "badger/allocator.h"
#include "badger/arena.h"
#include "badger/array.h"
#include "badger/assert.h"
#include "badger/platform.h"
#include "badger/ring.h"
#include "badger/set.h"
#include "badger/table.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <stdint.h>

/* An allocator that, once armed, stops the next thread that allocates
 * until the test lets it go. */
typedef struct holding_allocator
{
    tb_semaphore inside;  /* Signalled when a thread is held. */
    tb_semaphore release; /* Signalled to let it go. */
    tb_platform_atomic_u32 armed;
} holding_allocator;

static void *hold_then_allocate(void *context, void *memory, size_t old_size, size_t new_size,
                                size_t alignment)
{
    holding_allocator *const holder = context;
    const tb_allocator heap = tb_allocator_resolve(NULL);
    if (new_size != 0 && tb_platform_atomic_u32_exchange(&holder->armed, 0) != 0)
    {
        tb_semaphore_signal(&holder->inside, 1);
        tb_semaphore_wait(&holder->release);
    }
    return heap.function(heap.context, memory, old_size, new_size, alignment);
}

static tb_allocator holding(holding_allocator *holder)
{
    const tb_allocator allocator = {.function = hold_then_allocate, .context = holder};
    return allocator;
}

/* One kind of object: how to make it, grow it so that it allocates, change
 * it a little, read it, and destroy it. */
typedef struct object_kind
{
    const char *name;
    void *(*create)(const tb_allocator *allocator);
    void (*grow)(void *object);
    void (*change)(void *object);
    size_t (*measure)(void *object);
    void (*destroy)(void *object);
} object_kind;

static void *create_array(const tb_allocator *allocator)
{
    return tb_array_create(allocator, sizeof(uint32_t), _Alignof(uint32_t));
}
static void grow_array(void *object)
{
    tb_array_reserve(object, tb_array_capacity(object) * 2 + 64);
}
static void change_array(void *object)
{
    const uint32_t value = 7;
    (void)tb_array_push(object, &value);
}
static size_t read_array(void *object)
{
    return tb_array_length(object);
}
static void destroy_array(void *object)
{
    tb_array_destroy(object);
}

static void *create_table(const tb_allocator *allocator)
{
    return tb_table_create(allocator, 0, NULL, NULL);
}
static void grow_table(void *object)
{
    tb_table_reserve(object, tb_table_capacity(object) * 2 + 64);
}
static void change_table(void *object)
{
    static int key = 0;
    (void)tb_table_put(object, &key, NULL);
}
static size_t read_table(void *object)
{
    return tb_table_length(object);
}
static void destroy_table(void *object)
{
    tb_table_destroy(object);
}

static void *create_set(const tb_allocator *allocator)
{
    return tb_set_create(allocator, 0, NULL, NULL);
}
static void grow_set(void *object)
{
    tb_set_reserve(object, tb_set_capacity(object) * 2 + 64);
}
static void change_set(void *object)
{
    static int member = 0;
    (void)tb_set_put(object, &member);
}
static size_t read_set(void *object)
{
    return tb_set_length(object);
}
static void destroy_set(void *object)
{
    tb_set_destroy(object);
}

static void *create_ring(const tb_allocator *allocator)
{
    return tb_ring_create(allocator, sizeof(uint32_t), _Alignof(uint32_t));
}
static void grow_ring(void *object)
{
    tb_ring_reserve(object, tb_ring_capacity(object) * 2 + 64);
}
static void change_ring(void *object)
{
    const uint32_t value = 7;
    (void)tb_ring_push_back(object, &value);
}
static size_t read_ring(void *object)
{
    return tb_ring_length(object);
}
static void destroy_ring(void *object)
{
    tb_ring_destroy(object);
}

static const object_kind kinds[] = {
    {"array", create_array, grow_array, change_array, read_array, destroy_array},
    {"table", create_table, grow_table, change_table, read_table, destroy_table},
    {"set", create_set, grow_set, change_set, read_set, destroy_set},
    {"ring", create_ring, grow_ring, change_ring, read_ring, destroy_ring},
};

typedef struct grower
{
    const object_kind *kind;
    void *object;
} grower;

static void grow_on_a_thread(void *context)
{
    const grower *const work = context;
    work->kind->grow(work->object);
}

static void change_from_the_test(void *context)
{
    const grower *const work = context;
    work->kind->change(work->object);
}

TB_TEST(core_exclusive, two_threads_changing_one_container_fail_the_check)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(kinds); index++)
    {
        holding_allocator holder = {0};
        const tb_allocator allocator = holding(&holder);
        grower work = {&kinds[index], NULL};
        tb_thread thread = {0};
        TB_TEST_CONTEXT("%s", kinds[index].name);
        work.object = kinds[index].create(&allocator);
        tb_platform_atomic_u32_store(&holder.armed, 1);
        tb_thread_create(&thread, grow_on_a_thread, &work);
        /* The other thread is now inside the object, growing it. */
        tb_semaphore_wait(&holder.inside);
        TB_TEST_EXPECT_TRAP(change_from_the_test, &work, "one thread at a time");
        tb_semaphore_signal(&holder.release, 1);
        tb_thread_join(&thread);
        /* The thread has left, so the object is the test's to change. */
        change_from_the_test(&work);
        TB_TEST_EXPECT_UINT(kinds[index].measure(work.object), ==, 1);
        kinds[index].destroy(work.object);
    }
}

/* Many threads, each changing the object in turn under a lock of the
 * caller's, which is how the contract is met. */
typedef struct taking_turns
{
    const object_kind *kind;
    void *object;
    tb_mutex lock;
} taking_turns;

static void change_in_turn(void *context)
{
    taking_turns *const turns = context;
    for (size_t round = 0; round < 200; round++)
    {
        tb_mutex_lock(&turns->lock);
        turns->kind->change(turns->object);
        tb_mutex_unlock(&turns->lock);
    }
}

TB_TEST(core_exclusive, threads_that_take_turns_pass_the_check)
{
    enum
    {
        THREADS = 4
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(kinds); index++)
    {
        taking_turns turns = {&kinds[index], NULL, {{0}}};
        tb_thread threads[THREADS] = {{0}};
        TB_TEST_CONTEXT("%s", kinds[index].name);
        turns.object = kinds[index].create(NULL);
        for (size_t thread = 0; thread < THREADS; thread++)
        {
            tb_thread_create(&threads[thread], change_in_turn, &turns);
        }
        for (size_t thread = 0; thread < THREADS; thread++)
        {
            tb_thread_join(&threads[thread]);
        }
        TB_TEST_EXPECT_UINT(kinds[index].measure(turns.object), >=, 1);
        kinds[index].destroy(turns.object);
    }
}

/* Reading changes nothing, so readers do not take the object. */
typedef struct reading
{
    const object_kind *kind;
    void *object;
    size_t total;
} reading;

static void read_many_times(void *context)
{
    reading *const reader = context;
    for (size_t round = 0; round < 2000; round++)
    {
        reader->total += reader->kind->measure(reader->object);
    }
}

TB_TEST(core_exclusive, readers_together_pass_the_check)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(kinds); index++)
    {
        void *const object = kinds[index].create(NULL);
        reading readers[4] = {{0}};
        tb_thread threads[4] = {{0}};
        TB_TEST_CONTEXT("%s", kinds[index].name);
        kinds[index].change(object);
        for (size_t thread = 0; thread < TB_TEST_COUNT_OF(threads); thread++)
        {
            readers[thread] = (reading){&kinds[index], object, 0};
            tb_thread_create(&threads[thread], read_many_times, &readers[thread]);
        }
        for (size_t thread = 0; thread < TB_TEST_COUNT_OF(threads); thread++)
        {
            tb_thread_join(&threads[thread]);
            TB_TEST_EXPECT_UINT(readers[thread].total, ==, 2000);
        }
        kinds[index].destroy(object);
    }
}

/* An arena as an allocator is entered by every container that allocates
 * from it, while the container is inside itself: the guards of two objects,
 * each with one thread. */
TB_TEST(core_exclusive, a_container_in_an_arena_passes_the_check)
{
    tb_arena *const arena = tb_arena_create(0);
    const tb_allocator allocator = tb_arena_allocator(arena);
    tb_array *const array = tb_array_create(&allocator, sizeof(uint32_t), _Alignof(uint32_t));
    for (uint32_t value = 0; value < 1000; value++)
    {
        (void)tb_array_push(array, &value);
    }
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 1000);
    tb_array_destroy(array);
    tb_arena_destroy(arena);
}
