/* A pool must hand out distinct, aligned objects that keep their contents
 * until freed, reuse freed slots, grow by whole blocks from its allocator and
 * give every block back, respect its object limit, and catch, in builds with
 * assertions, an object freed twice or freed into the wrong pool. */
#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/pool.h"

#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

/* An object that records who it is, so a slot shared by two live objects, or
 * one damaged by the pool's own bookkeeping, is noticed. */
typedef struct record
{
    uint64_t identity;
    uint64_t check;
    uint64_t padding;
} record;

static void stamp(record *object, uint64_t identity)
{
    object->identity = identity;
    object->check = ~identity;
    object->padding = identity * 31;
}

static bool is_intact(const record *object, uint64_t identity)
{
    return object->identity == identity && object->check == ~identity &&
           object->padding == identity * 31;
}

static tb_pool *record_pool(const tb_allocator *allocator, size_t per_block, size_t limit)
{
    const tb_pool_options options = {.object_size = sizeof(record),
                                     .alignment = _Alignof(record),
                                     .objects_per_block = per_block,
                                     .object_limit = limit,
                                     .allocator = allocator};

    return tb_pool_create(&options);
}

/* ---------------------------------------------------------------------------
 * Objects.
 * ------------------------------------------------------------------------- */

TB_TEST(core_pool, every_object_is_distinct_and_keeps_its_contents)
{
    enum
    {
        COUNT = 1000
    };
    tb_pool *const pool = record_pool(NULL, 0, 0);
    record *objects[COUNT] = {0};
    size_t damaged = 0;

    for (size_t index = 0; index < COUNT; index++)
    {
        objects[index] = tb_pool_alloc(pool);
        stamp(objects[index], index);
    }
    for (size_t index = 0; index < COUNT; index++)
    {
        if (!is_intact(objects[index], index))
        {
            damaged++;
        }
    }
    TB_TEST_EXPECT_UINT(damaged, ==, 0);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, COUNT);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(pool), >=, COUNT);
    tb_pool_destroy(pool);
}

TB_TEST(core_pool, the_most_recently_freed_slot_is_reused_first)
{
    tb_pool *const pool = record_pool(NULL, 0, 0);
    record *const first = tb_pool_alloc(pool);
    record *const second = tb_pool_alloc(pool);

    tb_pool_free(pool, first);
    tb_pool_free(pool, second);
    TB_TEST_EXPECT_POINTER(tb_pool_alloc(pool), ==, second);
    TB_TEST_EXPECT_POINTER(tb_pool_alloc(pool), ==, first);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, 2);
    tb_pool_destroy(pool);
}

TB_TEST(core_pool, freeing_null_does_nothing)
{
    tb_pool *const pool = record_pool(NULL, 0, 0);

    (void)tb_pool_alloc(pool);
    tb_pool_free(pool, NULL);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, 1);
    tb_pool_destroy(pool);
    tb_pool_destroy(NULL);
}

/* Objects of every size are aligned as asked, including sizes smaller than
 * the pointer the pool keeps in a freed slot, and alignments beyond any the
 * heap guarantees. */
TB_TEST(core_pool, every_object_is_aligned_as_asked)
{
    static const size_t sizes[] = {1, 3, 8, 24, 100};
    static const size_t alignments[] = {0, 1, 2, 16, 64, 4096};

    for (size_t size_index = 0; size_index < TB_TEST_COUNT_OF(sizes); size_index++)
    {
        for (size_t align_index = 0; align_index < TB_TEST_COUNT_OF(alignments); align_index++)
        {
            const tb_pool_options options = {.object_size = sizes[size_index],
                                             .alignment = alignments[align_index],
                                             .objects_per_block = 9};
            tb_pool *const pool = tb_pool_create(&options);
            size_t expected = alignments[align_index];
            size_t misaligned = 0;
            void *objects[40] = {0};

            if (expected == 0)
            {
                expected = _Alignof(long double) > _Alignof(long long) ? _Alignof(long double)
                                                                       : _Alignof(long long);
            }
            for (size_t index = 0; index < TB_TEST_COUNT_OF(objects); index++)
            {
                objects[index] = tb_pool_alloc(pool);
                memset(objects[index], (int)index, sizes[size_index]);
                if ((uintptr_t)objects[index] % expected != 0)
                {
                    misaligned++;
                }
            }
            /* Free every other one and take them back: reused slots too. */
            for (size_t index = 0; index < TB_TEST_COUNT_OF(objects); index += 2)
            {
                tb_pool_free(pool, objects[index]);
            }
            for (size_t index = 0; index < TB_TEST_COUNT_OF(objects); index += 2)
            {
                objects[index] = tb_pool_alloc(pool);
                if ((uintptr_t)objects[index] % expected != 0)
                {
                    misaligned++;
                }
            }
            TB_TEST_CONTEXT("size %zu, alignment %zu", sizes[size_index], alignments[align_index]);
            TB_TEST_EXPECT_UINT(misaligned, ==, 0);
            tb_pool_destroy(pool);
        }
    }
}

/* Random allocations and frees against a record of what is live: a free
 * list that loses, repeats, or damages a slot shows up as an object whose
 * contents changed or as two live objects sharing an address. */
TB_TEST(core_pool, random_allocation_and_freeing_keep_every_live_object_intact)
{
    enum
    {
        MOST_LIVE = 500,
        STEPS = 20000
    };
    tb_pool *const pool = record_pool(NULL, 16, 0);
    record *live[MOST_LIVE] = {0};
    uint64_t identities[MOST_LIVE] = {0};
    size_t live_count = 0;
    uint64_t next_identity = 1;
    size_t damaged = 0;

    for (size_t step = 0; step < STEPS; step++)
    {
        const bool allocate =
            live_count == 0 || (live_count < MOST_LIVE && tb_test_random_below(100) < 55);

        if (allocate)
        {
            live[live_count] = tb_pool_alloc(pool);
            identities[live_count] = next_identity;
            stamp(live[live_count], next_identity);
            next_identity++;
            live_count++;
            continue;
        }
        {
            const size_t victim = (size_t)tb_test_random_below(live_count);

            if (!is_intact(live[victim], identities[victim]))
            {
                damaged++;
            }
            tb_pool_free(pool, live[victim]);
            live_count--;
            live[victim] = live[live_count];
            identities[victim] = identities[live_count];
        }
    }
    for (size_t index = 0; index < live_count; index++)
    {
        if (!is_intact(live[index], identities[index]))
        {
            damaged++;
        }
    }
    TB_TEST_EXPECT_UINT(damaged, ==, 0);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, live_count);
    /* Slots are reused: the pool never held many more than were live at once. */
    TB_TEST_EXPECT_UINT(tb_pool_capacity(pool), <=, MOST_LIVE + 16);
    tb_pool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * Blocks and the allocator.
 * ------------------------------------------------------------------------- */

TB_TEST(core_pool, the_pool_grows_one_block_at_a_time_from_its_allocator)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_pool *const pool = record_pool(&allocator, 10, 0);

    /* The record of the pool, and no block until an object is wanted. */
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 1);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(pool), ==, 0);
    for (size_t index = 0; index < 10; index++)
    {
        (void)tb_pool_alloc(pool);
    }
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 2);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(pool), ==, 10);
    (void)tb_pool_alloc(pool);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 3);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(pool), ==, 20);
    tb_pool_destroy(pool);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_pool, the_default_block_holds_16_kib_of_objects_or_at_least_8)
{
    const tb_pool_options small = {.object_size = 32, .alignment = 8};
    const tb_pool_options large = {.object_size = 8192, .alignment = 8};
    tb_pool *const small_pool = tb_pool_create(&small);
    tb_pool *const large_pool = tb_pool_create(&large);

    (void)tb_pool_alloc(small_pool);
    (void)tb_pool_alloc(large_pool);
    /* 32-byte objects, and in builds with assertions a header of their own:
     * hundreds to a 16 KiB block either way. */
    TB_TEST_EXPECT_UINT(tb_pool_capacity(small_pool), >=, 16 * 1024 / 64);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(small_pool), <=, 16 * 1024 / 32);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(large_pool), ==, TB_POOL_MINIMUM_BLOCK_OBJECTS);
    tb_pool_destroy(small_pool);
    tb_pool_destroy(large_pool);
}

TB_TEST(core_pool, a_reset_frees_every_object_and_keeps_the_blocks)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_pool *const pool = record_pool(&allocator, 10, 0);
    size_t allocations = 0;
    size_t capacity = 0;

    for (size_t index = 0; index < 35; index++)
    {
        stamp(tb_pool_alloc(pool), index);
    }
    allocations = tracker.allocation_count;
    capacity = tb_pool_capacity(pool);
    tb_pool_reset(pool);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, 0);
    TB_TEST_EXPECT_UINT(tb_pool_capacity(pool), ==, capacity);
    /* As many objects again come from the blocks the pool already has. */
    for (size_t index = 0; index < 35; index++)
    {
        stamp(tb_pool_alloc(pool), index);
    }
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, allocations);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, 35);
    tb_pool_destroy(pool);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_pool, an_allocator_that_refuses_leaves_the_pool_usable)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_pool *const pool = record_pool(&allocator, 4, 0);
    record *kept[4] = {0};

    for (size_t index = 0; index < 4; index++)
    {
        kept[index] = tb_pool_alloc(pool);
        stamp(kept[index], index);
    }
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_POINTER(tb_pool_try_alloc(pool), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, 4);
    /* A freed slot needs no block. */
    tb_pool_free(pool, kept[0]);
    TB_TEST_EXPECT_POINTER(tb_pool_try_alloc(pool), ==, kept[0]);
    tb_test_allocator_stop_failing(&tracker);
    TB_TEST_EXPECT_POINTER(tb_pool_try_alloc(pool), !=, NULL);
    for (size_t index = 1; index < 4; index++)
    {
        TB_TEST_EXPECT(is_intact(kept[index], index));
    }
    tb_pool_destroy(pool);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* ---------------------------------------------------------------------------
 * The object limit.
 * ------------------------------------------------------------------------- */

TB_TEST(core_pool, no_more_than_the_limit_are_live_at_once)
{
    tb_pool *const pool = record_pool(NULL, 0, 3);
    record *objects[3] = {0};

    for (size_t index = 0; index < 3; index++)
    {
        objects[index] = tb_pool_try_alloc(pool);
        TB_TEST_REQUIRE_POINTER(objects[index], !=, NULL);
    }
    TB_TEST_EXPECT_POINTER(tb_pool_try_alloc(pool), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_pool_count(pool), ==, 3);
    tb_pool_free(pool, objects[1]);
    TB_TEST_EXPECT_POINTER(tb_pool_try_alloc(pool), ==, objects[1]);
    tb_pool_destroy(pool);
}

static void allocate_past_the_limit(void *context)
{
    (void)tb_pool_alloc(context);
}

TB_TEST(core_pool, a_fail_fast_allocation_past_the_limit_ends_the_process)
{
    tb_pool *const pool = record_pool(NULL, 0, 1);

    (void)tb_pool_alloc(pool);
    TB_TEST_EXPECT_TRAP(allocate_past_the_limit, pool, "out of memory: 1 x 24 bytes, alignment 8");
    tb_pool_destroy(pool);
}

/* ---------------------------------------------------------------------------
 * Checks in builds with assertions.
 * ------------------------------------------------------------------------- */

TB_TEST(core_pool, a_new_object_is_filled_with_the_fill_byte)
{
    tb_pool *const pool = record_pool(NULL, 0, 0);
    unsigned char expected[sizeof(record)] = {0};
    record *object = NULL;

    if (!tb_assert_library_enabled())
    {
        tb_pool_destroy(pool);
        TB_TEST_SKIP("the engine was built without assertions");
    }
    memset(expected, TB_POOL_FILL_BYTE, sizeof(expected));
    object = tb_pool_alloc(pool);
    TB_TEST_EXPECT_BYTES(object, expected, sizeof(expected));
    /* Again for a reused slot, which held the free list's link. */
    stamp(object, 5);
    tb_pool_free(pool, object);
    object = tb_pool_alloc(pool);
    TB_TEST_EXPECT_BYTES(object, expected, sizeof(expected));
    tb_pool_destroy(pool);
}

typedef struct two_pools
{
    tb_pool *first;
    tb_pool *second;
    void *object;
} two_pools;

static void free_twice(void *context)
{
    two_pools *const pools = context;

    tb_pool_free(pools->first, pools->object);
}

static void free_into_another_pool(void *context)
{
    two_pools *const pools = context;

    tb_pool_free(pools->second, pools->object);
}

static void free_after_a_reset(void *context)
{
    two_pools *const pools = context;

    tb_pool_free(pools->first, pools->object);
}

TB_TEST(core_pool, freeing_twice_or_into_another_pool_violates_the_contract)
{
    two_pools pools = {.first = record_pool(NULL, 0, 0), .second = record_pool(NULL, 0, 0)};

    if (!tb_assert_library_enabled())
    {
        tb_pool_destroy(pools.first);
        tb_pool_destroy(pools.second);
        TB_TEST_SKIP("the engine was built without assertions");
    }
    pools.object = tb_pool_alloc(pools.first);
    TB_TEST_EXPECT_TRAP(free_into_another_pool, &pools, "the object came from this pool");
    tb_pool_free(pools.first, pools.object);
    TB_TEST_EXPECT_TRAP(free_twice, &pools, "it was not freed already");
    pools.object = tb_pool_alloc(pools.first);
    tb_pool_reset(pools.first);
    TB_TEST_EXPECT_TRAP(free_after_a_reset, &pools, "it was not freed already");
    tb_pool_destroy(pools.first);
    tb_pool_destroy(pools.second);
}

static void create_with_no_options(void *context)
{
    (void)context;
    (void)tb_pool_create(NULL);
}

static void create_with_objects_of_no_size(void *context)
{
    const tb_pool_options options = {.object_size = 0};

    (void)context;
    (void)tb_pool_create(&options);
}

static void create_with_an_alignment_of_three(void *context)
{
    const tb_pool_options options = {.object_size = 8, .alignment = 3};

    (void)context;
    (void)tb_pool_create(&options);
}

static void alloc_from_null(void *context)
{
    (void)context;
    (void)tb_pool_alloc(NULL);
}

TB_TEST(core_pool, bad_arguments_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(create_with_no_options, NULL, "options != NULL");
    TB_TEST_EXPECT_TRAP(create_with_objects_of_no_size, NULL, "object_size >= 1");
    TB_TEST_EXPECT_TRAP(create_with_an_alignment_of_three, NULL,
                        "the alignment is 0 or a power of two");
    TB_TEST_EXPECT_TRAP(alloc_from_null, NULL, "pool != NULL");
}

static void create_an_overflowing_pool(void *context)
{
    const tb_pool_options options = {.object_size = SIZE_MAX / 2, .objects_per_block = 4};

    (void)context;
    (void)tb_pool_create(&options);
}

TB_TEST(core_pool, a_block_too_large_to_describe_is_out_of_memory)
{
    TB_TEST_EXPECT_TRAP(create_an_overflowing_pool, NULL, "out of memory");
}
