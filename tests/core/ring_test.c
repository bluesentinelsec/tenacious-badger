/* A ring keeps its elements in a circular buffer, so its mistakes hide where
 * the sequence wraps from the end of the storage to its start. Every operation
 * is therefore checked against a plain array, with the front of the ring
 * placed at every position of the storage, on rings that are full and rings
 * that have room. */
#include "badger/ring.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/memory.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

enum
{
    MODEL_LENGTH_LIMIT = 300
};

/* The reference: a plain array whose first element is the front. */
typedef struct ring_model
{
    int values[MODEL_LENGTH_LIMIT];
    size_t length;
} ring_model;

static void model_insert(ring_model *model, size_t index, int value)
{
    memmove(&model->values[index + 1], &model->values[index],
            (model->length - index) * sizeof(int));
    model->values[index] = value;
    model->length++;
}

static int model_remove(ring_model *model, size_t index)
{
    const int removed = model->values[index];

    model->length--;
    memmove(&model->values[index], &model->values[index + 1],
            (model->length - index) * sizeof(int));
    return removed;
}

/* Moves the front element to the back, `count` times. */
static void model_rotate_left(ring_model *model, size_t count)
{
    for (size_t step = 0; step < count; step++)
    {
        const int front = model_remove(model, 0);

        model_insert(model, model->length, front);
    }
}

/* Number of indexes at which a ring differs from a model. */
static size_t count_differences(const tb_ring *ring, const ring_model *model)
{
    size_t differences = {0};

    if (tb_ring_length(ring) != model->length)
    {
        return SIZE_MAX;
    }
    for (size_t index = 0; index < model->length; index++)
    {
        if (*TB_RING_AT(ring, int, index) != model->values[index])
        {
            differences++;
        }
    }
    return differences;
}

/* Builds a ring of ints holding 0, 1, 2, ... whose front is `head` positions
 * into storage of exactly `capacity` elements, and the model of it. */
static tb_ring *create_ring(const tb_allocator *allocator, size_t capacity, size_t head,
                            size_t length, ring_model *model)
{
    tb_ring *const ring = TB_RING_CREATE(allocator, int);

    tb_ring_reserve(ring, capacity);
    /* Passing elements through moves the front along the storage. */
    for (size_t step = 0; step < head; step++)
    {
        TB_RING_PUSH_BACK(ring, int, -1);
        tb_ring_pop_front(ring, NULL);
    }
    model->length = 0;
    for (size_t index = 0; index < length; index++)
    {
        TB_RING_PUSH_BACK(ring, int, (int)index);
        model_insert(model, index, (int)index);
    }
    return ring;
}

static bool is_aligned(const void *memory, size_t alignment)
{
    return ((uintptr_t)memory & (uintptr_t)(alignment - 1)) == 0;
}

/* --- creating and destroying -------------------------------------------------- */

TB_TEST(core_ring, a_new_ring_is_empty_and_has_no_storage)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_ring *const ring = TB_RING_CREATE(&allocator, double);

    TB_TEST_REQUIRE_POINTER(ring, !=, NULL);
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 0);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 0);
    TB_TEST_EXPECT_UINT(tb_ring_element_size(ring), ==, sizeof(double));
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    /* Rotating and clearing an empty ring are allowed and do nothing. */
    tb_ring_rotate_left(ring, 3);
    tb_ring_rotate_right(ring, 3);
    tb_ring_clear(ring);
    tb_ring_shrink_to_fit(ring);
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 0);
    tb_ring_destroy(ring);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_ring, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_ring_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- the two ends ------------------------------------------------------------- */

TB_TEST(core_ring, elements_leave_the_front_in_the_order_they_joined_the_back)
{
    tb_ring *const ring = TB_RING_CREATE(NULL, int);

    for (int value = 0; value < 1000; value++)
    {
        TB_RING_PUSH_BACK(ring, int, value);
    }
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 1000);
    for (int expected = 0; expected < 1000; expected++)
    {
        int popped = -1;

        TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, expected);
        tb_ring_pop_front(ring, &popped);
        TB_TEST_EXPECT_INT(popped, ==, expected);
    }
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 0);
    tb_ring_destroy(ring);
}

TB_TEST(core_ring, each_end_is_also_a_stack)
{
    tb_ring *const ring = TB_RING_CREATE(NULL, int);

    for (int value = 0; value < 500; value++)
    {
        TB_RING_PUSH_FRONT(ring, int, value);
        /* The newest element at the front is always index 0. */
        TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, value);
    }
    for (int index = 0; index < 500; index++)
    {
        TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, (size_t)index), ==, 499 - index);
    }
    /* What went in at the front first comes out at the back first. */
    for (int expected = 0; expected < 250; expected++)
    {
        int popped = -1;

        tb_ring_pop_back(ring, &popped);
        TB_TEST_EXPECT_INT(popped, ==, expected);
    }
    for (int expected = 499; expected >= 250; expected--)
    {
        int popped = -1;

        tb_ring_pop_front(ring, &popped);
        TB_TEST_EXPECT_INT(popped, ==, expected);
    }
    /* A removed element need not be wanted. */
    TB_RING_PUSH_BACK(ring, int, 1);
    TB_RING_PUSH_BACK(ring, int, 2);
    tb_ring_pop_front(ring, NULL);
    tb_ring_pop_back(ring, NULL);
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 0);
    tb_ring_destroy(ring);
}

TB_TEST(core_ring, a_queue_that_stays_short_goes_round_its_storage_without_growing)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_ring *const ring = TB_RING_CREATE(&allocator, int);
    size_t requests = {0};

    tb_ring_reserve(ring, 8);
    requests = tracker.allocation_count + tracker.reallocation_count;
    for (int value = 0; value < 7; value++)
    {
        TB_RING_PUSH_BACK(ring, int, value);
    }
    /* Every push fills the storage to its last slot, and the front passes
     * the end of the storage many times over. */
    for (int value = 7; value < 10000; value++)
    {
        int popped = -1;

        TB_RING_PUSH_BACK(ring, int, value);
        TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 8);
        tb_ring_pop_front(ring, &popped);
        TB_TEST_EXPECT_INT(popped, ==, value - 7);
        TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 6), ==, value);
    }
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 8);
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==, requests);
    tb_ring_destroy(ring);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_ring, growing_keeps_the_order_wherever_the_front_is)
{
    enum
    {
        CAPACITY = 8
    };

    /* A full ring with its front at each position, grown by one more push at
     * each end. */
    for (size_t head = 0; head < CAPACITY; head++)
    {
        for (size_t end = 0; end < 2; end++)
        {
            ring_model model = {0};
            tb_ring *const ring = create_ring(NULL, CAPACITY, head, CAPACITY, &model);

            TB_TEST_CONTEXT("front at storage position %zu, pushing at end %zu", head, end);
            TB_TEST_REQUIRE_UINT(tb_ring_capacity(ring), ==, CAPACITY);
            if (end == 0)
            {
                TB_RING_PUSH_BACK(ring, int, 100);
                model_insert(&model, model.length, 100);
            }
            else
            {
                TB_RING_PUSH_FRONT(ring, int, 100);
                model_insert(&model, 0, 100);
            }
            TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 2 * CAPACITY);
            TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
            tb_ring_destroy(ring);
        }
    }
}

TB_TEST(core_ring, push_without_an_element_adds_a_zero_filled_one)
{
    tb_ring *const ring = TB_RING_CREATE(NULL, int);

    /* Dirty the storage first, so that zero is the ring's doing. */
    for (int value = 1; value <= 8; value++)
    {
        TB_RING_PUSH_BACK(ring, int, value * 0x01010101);
    }
    tb_ring_clear(ring);
    TB_TEST_EXPECT_INT(*(int *)tb_ring_push_back(ring, NULL), ==, 0);
    TB_TEST_EXPECT_INT(*(int *)tb_ring_push_front(ring, NULL), ==, 0);
    TB_TEST_EXPECT_INT(*(int *)tb_ring_insert(ring, 1, NULL), ==, 0);
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 3);
    tb_ring_destroy(ring);
}

/* --- elements by index -------------------------------------------------------- */

TB_TEST(core_ring, get_and_put_reach_every_element_across_the_wrap)
{
    enum
    {
        CAPACITY = 8,
        LENGTH = 6
    };

    for (size_t head = 0; head < CAPACITY; head++)
    {
        ring_model model = {0};
        tb_ring *const ring = create_ring(NULL, CAPACITY, head, LENGTH, &model);

        TB_TEST_CONTEXT("front at storage position %zu", head);
        for (size_t index = 0; index < LENGTH; index++)
        {
            int value = -1;

            tb_ring_get(ring, index, &value);
            TB_TEST_EXPECT_INT(value, ==, (int)index);
            value = (int)index * 10;
            tb_ring_put(ring, index, &value);
            model.values[index] = value;
            TB_TEST_EXPECT_POINTER(tb_ring_at(ring, index), ==, TB_RING_AT(ring, int, index));
        }
        TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
        tb_ring_destroy(ring);
    }
}

TB_TEST(core_ring, insert_places_an_element_at_any_index_wherever_the_front_is)
{
    enum
    {
        CAPACITY = 9
    };

    /* Rings with room and rings that must grow, the front at each position,
     * and the new element at each index. */
    for (size_t length = 0; length <= CAPACITY; length++)
    {
        for (size_t head = 0; head < CAPACITY; head++)
        {
            for (size_t index = 0; index <= length; index++)
            {
                ring_model model = {0};
                tb_ring *const ring = create_ring(NULL, CAPACITY, head, length, &model);
                static const int inserted = -7;
                int *const slot = tb_ring_insert(ring, index, &inserted);

                model_insert(&model, index, inserted);
                if (count_differences(ring, &model) != 0 || slot != tb_ring_at(ring, index))
                {
                    TB_TEST_FAIL("inserting at %zu of %zu elements with the front at storage "
                                 "position %zu left %zu elements wrong",
                                 index, length, head, count_differences(ring, &model));
                }
                tb_ring_destroy(ring);
            }
        }
    }
}

TB_TEST(core_ring, remove_takes_out_any_element_wherever_the_front_is)
{
    enum
    {
        CAPACITY = 9
    };

    for (size_t length = 1; length <= CAPACITY; length++)
    {
        for (size_t head = 0; head < CAPACITY; head++)
        {
            for (size_t index = 0; index < length; index++)
            {
                ring_model model = {0};
                tb_ring *const ring = create_ring(NULL, CAPACITY, head, length, &model);
                int removed = -1;

                tb_ring_remove(ring, index, &removed);
                if (removed != model_remove(&model, index) || count_differences(ring, &model) != 0)
                {
                    TB_TEST_FAIL("removing %zu of %zu elements with the front at storage "
                                 "position %zu left %zu elements wrong",
                                 index, length, head, count_differences(ring, &model));
                }
                /* The removed element need not be wanted. */
                if (model.length > 0)
                {
                    tb_ring_remove(ring, 0, NULL);
                    (void)model_remove(&model, 0);
                    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
                }
                tb_ring_destroy(ring);
            }
        }
    }
}

/* --- rotating ----------------------------------------------------------------- */

TB_TEST(core_ring, rotate_moves_elements_from_one_end_to_the_other)
{
    enum
    {
        CAPACITY = 9
    };

    /* Full rings, which only move their front, and rings with room, which
     * move elements across the gap from the nearer end. */
    for (size_t length = 1; length <= CAPACITY; length++)
    {
        for (size_t head = 0; head < CAPACITY; head += 2)
        {
            for (size_t count = 0; count <= 2 * length + 1; count++)
            {
                ring_model model = {0};
                ring_model original = {0};
                tb_ring *const ring = create_ring(NULL, CAPACITY, head, length, &model);

                original = model;
                TB_TEST_CONTEXT("%zu elements, front at storage position %zu, rotating by %zu",
                                length, head, count);
                tb_ring_rotate_left(ring, count);
                model_rotate_left(&model, count % length);
                TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
                /* The element that was at index `count` is now the front. */
                TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, (int)(count % length));
                /* Rotating right by the same count puts everything back. */
                tb_ring_rotate_right(ring, count);
                TB_TEST_EXPECT_UINT(count_differences(ring, &original), ==, 0);
                /* Right alone moves the back element to the front. */
                tb_ring_rotate_right(ring, 1);
                TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, (int)(length - 1));
                TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, CAPACITY);
                tb_ring_destroy(ring);
            }
        }
    }
}

TB_TEST(core_ring, rotating_by_a_huge_count_wraps_around)
{
    ring_model model = {0};
    tb_ring *const ring = create_ring(NULL, 16, 5, 7, &model);

    tb_ring_rotate_left(ring, SIZE_MAX);
    model_rotate_left(&model, (size_t)(SIZE_MAX % 7));
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    tb_ring_rotate_right(ring, SIZE_MAX);
    tb_ring_rotate_left(ring, (size_t)7 * 1000000);
    TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, 0);
    tb_ring_destroy(ring);
}

/* --- size and storage --------------------------------------------------------- */

TB_TEST(core_ring, capacity_doubles_so_that_adding_is_cheap)
{
    enum
    {
        ELEMENT_COUNT = 200000,
        /* The first capacity of eight, then fifteen doublings. */
        GROWTH_LIMIT = 16
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_ring *const ring = TB_RING_CREATE(&allocator, int);
    size_t capacity = {0};
    size_t growth_count = {0};

    for (int value = 0; value < ELEMENT_COUNT; value++)
    {
        /* Alternate the ends, so the ring is wrapped whenever it grows. */
        if (value % 2 == 0)
        {
            TB_RING_PUSH_BACK(ring, int, value);
        }
        else
        {
            TB_RING_PUSH_FRONT(ring, int, value);
        }
        if (tb_ring_capacity(ring) != capacity)
        {
            TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), >=, capacity * 2);
            capacity = tb_ring_capacity(ring);
            growth_count++;
        }
    }
    TB_TEST_EXPECT_UINT(growth_count, <=, GROWTH_LIMIT);
    /* Odd values run down from the front, even values up from the middle. */
    TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, ELEMENT_COUNT - 1);
    TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, ELEMENT_COUNT / 2 - 1), ==, 1);
    TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, ELEMENT_COUNT / 2), ==, 0);
    TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, ELEMENT_COUNT - 1), ==, ELEMENT_COUNT - 2);
    tb_ring_destroy(ring);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_ring, reserve_clear_and_shrink_control_the_storage)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    ring_model model = {0};
    tb_ring *const ring = create_ring(&allocator, 16, 11, 9, &model);
    size_t requests = {0};

    /* Reserving more keeps the elements; reserving less changes nothing. */
    tb_ring_reserve(ring, 100);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 100);
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    requests = tracker.allocation_count + tracker.reallocation_count;
    tb_ring_reserve(ring, 10);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 100);
    TB_TEST_EXPECT(tb_ring_try_reserve(ring, 100));
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 100);
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==, requests);

    tb_ring_shrink_to_fit(ring);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 9);
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    tb_ring_shrink_to_fit(ring);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 9);

    /* Clearing keeps the storage. Shrinking an empty ring gives it all back. */
    tb_ring_clear(ring);
    TB_TEST_EXPECT_UINT(tb_ring_length(ring), ==, 0);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 9);
    tb_ring_shrink_to_fit(ring);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 0);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    TB_RING_PUSH_FRONT(ring, int, 5);
    TB_TEST_EXPECT_INT(*TB_RING_AT(ring, int, 0), ==, 5);
    tb_ring_destroy(ring);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

typedef struct wide_vector
{
    _Alignas(64) float lanes[16];
} wide_vector;

TB_TEST(core_ring, the_storage_is_requested_at_the_alignment_of_the_elements)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_ring *const vectors = TB_RING_CREATE(&allocator, wide_vector);
    size_t aligned_block_count = {0};

    (void)tb_ring_push_back(vectors, NULL);
    /* A lucky address proves nothing, so the request itself is checked. */
    for (size_t index = 0; index < tracker.live_block_count; index++)
    {
        if (tracker.blocks[index].alignment == _Alignof(wide_vector))
        {
            aligned_block_count++;
            TB_TEST_EXPECT_UINT(tracker.blocks[index].size, ==,
                                tb_ring_capacity(vectors) * sizeof(wide_vector));
        }
    }
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 2);
    TB_TEST_EXPECT_UINT(aligned_block_count, ==, 1);
    tb_ring_destroy(vectors);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_ring, inserting_and_removing_move_only_the_shorter_side)
{
    enum
    {
        CAPACITY = 16,
        LENGTH = 10
    };

    for (size_t head = 0; head < CAPACITY; head++)
    {
        ring_model model = {0};
        tb_ring *const ring = create_ring(NULL, CAPACITY, head, LENGTH, &model);
        static const int inserted = -3;
        const int *back = TB_RING_AT(ring, int, LENGTH - 1);
        const int *front = NULL;

        TB_TEST_CONTEXT("front at storage position %zu", head);
        /* Near the front: the back element stays where it is in memory. */
        (void)tb_ring_insert(ring, 1, &inserted);
        TB_TEST_EXPECT_POINTER(TB_RING_AT(ring, int, LENGTH), ==, back);
        tb_ring_remove(ring, 1, NULL);
        TB_TEST_EXPECT_POINTER(TB_RING_AT(ring, int, LENGTH - 1), ==, back);
        /* Near the back: the front element stays where it is. */
        front = TB_RING_AT(ring, int, 0);
        (void)tb_ring_insert(ring, LENGTH - 1, &inserted);
        TB_TEST_EXPECT_POINTER(TB_RING_AT(ring, int, 0), ==, front);
        tb_ring_remove(ring, LENGTH - 1, NULL);
        TB_TEST_EXPECT_POINTER(TB_RING_AT(ring, int, 0), ==, front);
        TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
        tb_ring_destroy(ring);
    }
}

TB_TEST(core_ring, elements_of_any_type_are_stored_by_value_and_aligned)
{
    tb_ring *const vectors = TB_RING_CREATE(NULL, wide_vector);
    tb_ring *const bytes = tb_ring_create(NULL, 3, 1);
    static const unsigned char triple[3] = {1, 2, 3};
    unsigned char read_back[3] = {0};
    wide_vector vector = {0};

    for (size_t index = 0; index < 20; index++)
    {
        vector.lanes[0] = (float)index;
        TB_RING_PUSH_FRONT(vectors, wide_vector, vector);
        /* The ring holds its own copy. */
        vector.lanes[0] = -1.0f;
    }
    for (size_t index = 0; index < 20; index++)
    {
        const wide_vector *const element = TB_RING_AT(vectors, wide_vector, index);

        TB_TEST_EXPECT(is_aligned(element, 64));
        TB_TEST_EXPECT_FLOAT((double)element->lanes[0], ==, (double)(19 - index));
    }
    (void)tb_ring_push_back(bytes, triple);
    (void)tb_ring_push_front(bytes, triple);
    tb_ring_get(bytes, 1, read_back);
    TB_TEST_EXPECT_BYTES(read_back, triple, sizeof(triple));
    TB_TEST_EXPECT_UINT(tb_ring_element_size(bytes), ==, 3);
    tb_ring_destroy(bytes);
    tb_ring_destroy(vectors);
}

/* --- running out of memory ---------------------------------------------------- */

static void push_back_one(void *context)
{
    TB_RING_PUSH_BACK((tb_ring *)context, int, 1);
}

static void push_front_one(void *context)
{
    TB_RING_PUSH_FRONT((tb_ring *)context, int, 1);
}

static void insert_one(void *context)
{
    (void)tb_ring_insert(context, 1, NULL);
}

static void reserve_a_thousand(void *context)
{
    tb_ring_reserve(context, 1000);
}

static void reserve_more_than_memory_can_hold(void *context)
{
    tb_ring_reserve(context, SIZE_MAX / sizeof(int) + 1);
}

static void create_a_ring(void *context)
{
    (void)TB_RING_CREATE(context, int);
}

TB_TEST(core_ring, an_exhausted_allocator_ends_growth_and_leaves_the_ring_intact)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);
    ring_model model = {0};
    tb_ring *const ring = create_ring(&allocator, 8, 5, 8, &model);
    size_t requests = {0};

    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(push_back_one, ring, "out of memory: 9 x 4 bytes");
    TB_TEST_EXPECT_TRAP(push_front_one, ring, "out of memory: 9 x 4 bytes");
    TB_TEST_EXPECT_TRAP(insert_one, ring, "out of memory: 9 x 4 bytes");
    TB_TEST_EXPECT_TRAP(reserve_a_thousand, ring, "out of memory: 1000 x 4 bytes");
    TB_TEST_EXPECT_TRAP(create_a_ring, &allocator, "out of memory");
    TB_TEST_EXPECT(!tb_ring_try_reserve(ring, 9));
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 8);
    /* Shrinking that cannot be done leaves the ring as it was. */
    tb_ring_pop_back(ring, NULL);
    (void)model_remove(&model, model.length - 1);
    tb_ring_shrink_to_fit(ring);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 8);
    tb_test_allocator_stop_failing(&tracker);

    /* A capacity that cannot be expressed is refused without asking. */
    requests = tracker.allocation_count + tracker.reallocation_count;
    TB_TEST_EXPECT(!tb_ring_try_reserve(ring, SIZE_MAX / sizeof(int) + 1));
    TB_TEST_EXPECT_TRAP(reserve_more_than_memory_can_hold, ring, "out of memory");
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==, requests);

    TB_RING_PUSH_BACK(ring, int, 7);
    TB_RING_PUSH_BACK(ring, int, 8);
    model_insert(&model, model.length, 7);
    model_insert(&model, model.length, 8);
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    tb_ring_destroy(ring);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* An allocator that supplies blocks up to a size limit and no larger. */
typedef struct limited_allocator
{
    size_t size_limit;
    size_t refusal_count;
} limited_allocator;

static void *limited_function(void *context, void *memory, size_t old_size, size_t new_size,
                              size_t alignment)
{
    limited_allocator *const limited = context;
    const tb_allocator heap = tb_allocator_heap();

    if (new_size > limited->size_limit)
    {
        limited->refusal_count++;
        return NULL;
    }
    return heap.function(heap.context, memory, old_size, new_size, alignment);
}

TB_TEST(core_ring, growth_settles_for_what_it_needs_when_double_cannot_be_had)
{
    limited_allocator limited = {.size_limit = 100 * sizeof(int)};
    const tb_allocator allocator = {.function = limited_function, .context = &limited};
    ring_model model = {0};
    tb_ring *const ring = create_ring(&allocator, 64, 40, 64, &model);

    TB_RING_PUSH_FRONT(ring, int, -1);
    model_insert(&model, 0, -1);
    TB_TEST_EXPECT_UINT(limited.refusal_count, ==, 1);
    TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, 65);
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    tb_ring_destroy(ring);
}

/* --- random operations against a model ---------------------------------------- */

static void run_model(const tb_allocator *allocator)
{
    enum
    {
        OPERATION_COUNT = 30000
    };
    static ring_model model = {0};
    tb_ring *const ring = TB_RING_CREATE(allocator, int);
    size_t removal_count = {0};
    size_t rotation_count = {0};

    memset(&model, 0, sizeof(model));
    for (size_t operation = 0; operation < OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(100);
        const int value = (int)tb_test_random_between(-1000000, 1000000);
        int removed = {0};

        TB_TEST_CONTEXT("operation %zu (choice %u) on %zu elements", operation, (unsigned)choice,
                        model.length);
        if (model.length == MODEL_LENGTH_LIMIT || choice < 1)
        {
            tb_ring_clear(ring);
            model.length = 0;
        }
        else if (choice < 22 || model.length == 0)
        {
            TB_RING_PUSH_BACK(ring, int, value);
            model_insert(&model, model.length, value);
        }
        else if (choice < 40)
        {
            TB_RING_PUSH_FRONT(ring, int, value);
            model_insert(&model, 0, value);
        }
        else if (choice < 50)
        {
            const size_t index = (size_t)tb_test_random_below(model.length + 1);

            (void)tb_ring_insert(ring, index, &value);
            model_insert(&model, index, value);
        }
        else if (choice < 60)
        {
            tb_ring_pop_front(ring, &removed);
            TB_TEST_EXPECT_INT(removed, ==, model_remove(&model, 0));
            removal_count++;
        }
        else if (choice < 70)
        {
            tb_ring_pop_back(ring, &removed);
            TB_TEST_EXPECT_INT(removed, ==, model_remove(&model, model.length - 1));
            removal_count++;
        }
        else if (choice < 78)
        {
            const size_t index = (size_t)tb_test_random_below(model.length);

            tb_ring_remove(ring, index, &removed);
            TB_TEST_EXPECT_INT(removed, ==, model_remove(&model, index));
            removal_count++;
        }
        else if (choice < 84)
        {
            const size_t index = (size_t)tb_test_random_below(model.length);

            tb_ring_put(ring, index, &value);
            model.values[index] = value;
        }
        else if (choice < 90)
        {
            const size_t count = (size_t)tb_test_random_below(2 * model.length + 1);

            tb_ring_rotate_left(ring, count);
            model_rotate_left(&model, count % model.length);
            rotation_count++;
        }
        else if (choice < 96)
        {
            const size_t count = (size_t)tb_test_random_below(2 * model.length + 1);

            tb_ring_rotate_right(ring, count);
            model_rotate_left(&model, model.length - count % model.length);
            rotation_count++;
        }
        else if (choice < 98)
        {
            tb_ring_shrink_to_fit(ring);
            TB_TEST_EXPECT_UINT(tb_ring_capacity(ring), ==, model.length);
        }
        else
        {
            tb_ring_reserve(ring, model.length + (size_t)tb_test_random_below(64));
        }
        TB_TEST_REQUIRE_UINT(tb_ring_capacity(ring), >=, model.length);
        TB_TEST_REQUIRE_UINT(count_differences(ring, &model), ==, 0);
    }
    TB_TEST_EXPECT_UINT(removal_count, >, OPERATION_COUNT / 5);
    TB_TEST_EXPECT_UINT(rotation_count, >, OPERATION_COUNT / 20);
    tb_ring_destroy(ring);
}

TB_TEST(core_ring, random_operations_agree_with_a_plain_array)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    run_model(&allocator);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_ring, random_operations_agree_with_a_plain_array_in_an_arena)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);

    run_model(&allocator);
    tb_arena_destroy(arena);
}

/* --- contract violations ------------------------------------------------------ */

static void length_of_null(void *context)
{
    (void)context;
    (void)tb_ring_length(NULL);
}

static void capacity_of_null(void *context)
{
    (void)context;
    (void)tb_ring_capacity(NULL);
}

static void element_size_of_null(void *context)
{
    (void)context;
    (void)tb_ring_element_size(NULL);
}

static void at_of_null(void *context)
{
    (void)context;
    (void)tb_ring_at(NULL, 0);
}

static void push_back_to_null(void *context)
{
    (void)tb_ring_push_back(NULL, context);
}

static void push_front_to_null(void *context)
{
    (void)tb_ring_push_front(NULL, context);
}

static void pop_back_from_null(void *context)
{
    (void)context;
    tb_ring_pop_back(NULL, NULL);
}

static void pop_front_from_null(void *context)
{
    (void)context;
    tb_ring_pop_front(NULL, NULL);
}

static void insert_into_null(void *context)
{
    (void)tb_ring_insert(NULL, 0, context);
}

static void remove_from_null(void *context)
{
    (void)context;
    tb_ring_remove(NULL, 0, NULL);
}

static void rotate_left_null(void *context)
{
    (void)context;
    tb_ring_rotate_left(NULL, 1);
}

static void rotate_right_null(void *context)
{
    (void)context;
    tb_ring_rotate_right(NULL, 1);
}

static void clear_null(void *context)
{
    (void)context;
    tb_ring_clear(NULL);
}

static void reserve_null(void *context)
{
    (void)context;
    tb_ring_reserve(NULL, 1);
}

static void try_reserve_null(void *context)
{
    (void)context;
    (void)tb_ring_try_reserve(NULL, 1);
}

static void shrink_null(void *context)
{
    (void)context;
    tb_ring_shrink_to_fit(NULL);
}

TB_TEST(core_ring, a_null_ring_violates_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        length_of_null,    capacity_of_null,   element_size_of_null, at_of_null,
        push_back_to_null, push_front_to_null, pop_back_from_null,   pop_front_from_null,
        insert_into_null,  remove_from_null,   rotate_left_null,     rotate_right_null,
        clear_null,        reserve_null,       try_reserve_null,     shrink_null,
    };
    int element = 1;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], &element, "ring != NULL");
    }
}

typedef struct element_request
{
    tb_ring *ring;
    size_t index;
    int element;
} element_request;

static void at_index(void *context)
{
    const element_request *const request = context;

    (void)tb_ring_at(request->ring, request->index);
}

static void typed_at_index(void *context)
{
    const element_request *const request = context;

    (void)TB_RING_AT(request->ring, int, request->index);
}

static void get_index(void *context)
{
    element_request *const request = context;

    tb_ring_get(request->ring, request->index, &request->element);
}

static void put_index(void *context)
{
    const element_request *const request = context;

    tb_ring_put(request->ring, request->index, &request->element);
}

static void remove_index(void *context)
{
    const element_request *const request = context;

    tb_ring_remove(request->ring, request->index, NULL);
}

static void insert_index(void *context)
{
    const element_request *const request = context;

    (void)tb_ring_insert(request->ring, request->index, &request->element);
}

TB_TEST(core_ring, an_index_outside_the_ring_violates_the_contract)
{
    static const size_t lengths[] = {0, 1, 5};
    static const char *const expected = "index must be less than the length";

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t row = 0; row < TB_TEST_COUNT_OF(lengths); row++)
    {
        ring_model model = {0};
        element_request request = {.ring = create_ring(NULL, 8, 6, lengths[row], &model),
                                   .index = lengths[row]};

        TB_TEST_CONTEXT("length %zu", lengths[row]);
        TB_TEST_EXPECT_TRAP(at_index, &request, expected);
        TB_TEST_EXPECT_TRAP(typed_at_index, &request, expected);
        TB_TEST_EXPECT_TRAP(get_index, &request, expected);
        TB_TEST_EXPECT_TRAP(put_index, &request, expected);
        TB_TEST_EXPECT_TRAP(remove_index, &request, expected);
        request.index = SIZE_MAX;
        TB_TEST_EXPECT_TRAP(at_index, &request, expected);
        /* Inserting at the length appends; one past it is outside. */
        request.index = lengths[row] + 1;
        TB_TEST_EXPECT_TRAP(insert_index, &request, "index must be at most the length");
        TB_TEST_EXPECT_UINT(count_differences(request.ring, &model), ==, 0);
        tb_ring_destroy(request.ring);
    }
}

static void pop_back_from(void *context)
{
    tb_ring_pop_back(context, NULL);
}

static void pop_front_from(void *context)
{
    tb_ring_pop_front(context, NULL);
}

static void get_into_null(void *context)
{
    tb_ring_get(context, 0, NULL);
}

static void put_from_null(void *context)
{
    tb_ring_put(context, 0, NULL);
}

static void at_as_another_type(void *context)
{
    (void)TB_RING_AT((tb_ring *)context, char, 0);
}

static void push_back_another_type(void *context)
{
    TB_RING_PUSH_BACK((tb_ring *)context, short, 1);
}

static void push_front_another_type(void *context)
{
    TB_RING_PUSH_FRONT((tb_ring *)context, double, 1.0);
}

static void create_with_no_element_size(void *context)
{
    (void)context;
    (void)tb_ring_create(NULL, 0, 1);
}

static void create_with_a_bad_alignment(void *context)
{
    (void)context;
    (void)tb_ring_create(NULL, 12, 12);
}

static void create_with_a_size_that_does_not_fit_its_alignment(void *context)
{
    (void)context;
    (void)tb_ring_create(NULL, 12, 8);
}

TB_TEST(core_ring, misuse_of_elements_violates_the_contract)
{
    ring_model model = {0};
    tb_ring *ring = NULL;
    tb_ring *empty = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    ring = create_ring(NULL, 8, 3, 3, &model);
    empty = TB_RING_CREATE(NULL, int);
    TB_TEST_EXPECT_TRAP(pop_back_from, empty, "the ring must not be empty");
    TB_TEST_EXPECT_TRAP(pop_front_from, empty, "the ring must not be empty");
    TB_TEST_EXPECT_TRAP(get_into_null, ring, "element != NULL");
    TB_TEST_EXPECT_TRAP(put_from_null, ring, "element != NULL");
    TB_TEST_EXPECT_TRAP(at_as_another_type, ring, "the ring holds elements of another size");
    TB_TEST_EXPECT_TRAP(push_back_another_type, ring, "the ring holds elements of another size");
    TB_TEST_EXPECT_TRAP(push_front_another_type, ring, "the ring holds elements of another size");
    TB_TEST_EXPECT_TRAP(create_with_no_element_size, NULL, "an element has at least one byte");
    TB_TEST_EXPECT_TRAP(create_with_a_bad_alignment, NULL,
                        "element alignment must be a power of two");
    TB_TEST_EXPECT_TRAP(create_with_a_size_that_does_not_fit_its_alignment, NULL,
                        "element size must be a multiple of the element alignment");
    TB_TEST_EXPECT_UINT(count_differences(ring, &model), ==, 0);
    tb_ring_destroy(empty);
    tb_ring_destroy(ring);
}
