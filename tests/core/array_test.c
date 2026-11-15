/* The dynamic array is the collection most engine code will use, so its
 * promises are pinned here: elements are stored by value, in order, in one
 * contiguous block; every operation leaves exactly the elements it should;
 * growth is amortized and never loses an element; all memory comes from the
 * array's allocator and goes back to it; and misuse is caught. */
#include "badger/array.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/memory.h"
#include "engine_test.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

typedef struct particle
{
    double position[3];
    float lifetime;
    unsigned char flags;
} particle;

/* A type that needs more alignment than any scalar, as SIMD data does. */
typedef struct wide_vector
{
    _Alignas(64) float lanes[16];
} wide_vector;

static bool is_aligned(const void *memory, size_t alignment)
{
    return ((uintptr_t)memory & (uintptr_t)(alignment - 1)) == 0;
}

/* An int array holding 0, 1, 2, ... up to a length. */
static tb_array *create_counting_array(const tb_allocator *allocator, size_t length)
{
    tb_array *const array = TB_ARRAY_CREATE(allocator, int);

    for (size_t index = 0; index < length; index++)
    {
        TB_ARRAY_PUSH(array, int, (int)index);
    }
    return array;
}

/* Checks that an int array holds exactly the expected values, in order. */
static void expect_ints(const tb_array *array, const int *expected, size_t expected_length)
{
    TB_TEST_REQUIRE_UINT(tb_array_length(array), ==, expected_length);
    if (expected_length > 0)
    {
        TB_TEST_EXPECT_BYTES(tb_array_data(array), expected, expected_length * sizeof(int));
    }
}

/* --- creating and destroying -------------------------------------------------- */

TB_TEST(core_array, a_new_array_is_empty_and_has_no_storage)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_array *const array = TB_ARRAY_CREATE(&allocator, particle);

    TB_TEST_REQUIRE_POINTER(array, !=, NULL);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_element_size(array), ==, sizeof(particle));
    TB_TEST_EXPECT_POINTER(tb_array_data(array), ==, NULL);
    TB_TEST_EXPECT_POINTER(TB_ARRAY_DATA(array, particle), ==, NULL);
    /* Only the array itself has been allocated. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    tb_array_destroy(array);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_array, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_array_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

TB_TEST(core_array, all_memory_comes_from_the_allocator_and_goes_back_to_it)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const size_t heap_blocks = tb_memory_allocation_count();
    tb_array *const array = create_counting_array(&allocator, 1000);

    /* The array and its storage: two blocks, both the test allocator's. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 2);
    TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, array, 1));
    TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, tb_array_data(array),
                                          tb_array_capacity(array) * sizeof(int)));
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, heap_blocks + 2);
    tb_array_destroy(array);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, heap_blocks);
}

/* --- elements ----------------------------------------------------------------- */

TB_TEST(core_array, elements_are_stored_by_value_in_order_and_contiguously)
{
    enum
    {
        ELEMENT_COUNT = 1000
    };
    tb_array *const particles = TB_ARRAY_CREATE(NULL, particle);
    const particle *data = NULL;

    for (size_t index = 0; index < ELEMENT_COUNT; index++)
    {
        particle element = {0};

        element.position[0] = (double)index;
        element.position[2] = (double)index * 0.5;
        element.lifetime = (float)index;
        element.flags = (unsigned char)(index & 0xFFu);
        TB_ARRAY_PUSH(particles, particle, element);
        /* The array holds its own copy. */
        element.position[0] = -1.0;
    }
    TB_TEST_REQUIRE_UINT(tb_array_length(particles), ==, ELEMENT_COUNT);
    data = TB_ARRAY_DATA(particles, particle);
    TB_TEST_EXPECT(is_aligned(data, _Alignof(particle)));
    for (size_t index = 0; index < ELEMENT_COUNT; index++)
    {
        const particle *const element = TB_ARRAY_AT(particles, particle, index);

        TB_TEST_CONTEXT("element %zu", index);
        TB_TEST_EXPECT_POINTER(element, ==, data + index);
        TB_TEST_EXPECT_POINTER(tb_array_at(particles, index), ==, element);
        TB_TEST_EXPECT_FLOAT(element->position[0], ==, (double)index);
        TB_TEST_EXPECT_FLOAT(element->position[2], ==, (double)index * 0.5);
        TB_TEST_EXPECT_UINT(element->flags, ==, index & 0xFFu);
    }
    tb_array_destroy(particles);
}

TB_TEST(core_array, elements_of_every_size_and_alignment_round_trip)
{
    typedef struct layout_row
    {
        size_t size;
        size_t alignment;
    } layout_row;
    static const layout_row rows[] = {
        {1, 1},  {2, 1},   {3, 1},   {7, 1},    {100, 1},     {2, 2},       {8, 8},
        {24, 8}, {16, 16}, {64, 64}, {192, 64}, {4096, 4096}, {8192, 4096},
    };
    static unsigned char element[8192] = {0};
    static unsigned char read_back[8192] = {0};

    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        const size_t size = rows[row].size;
        tb_array *const array = tb_array_create(NULL, size, rows[row].alignment);

        TB_TEST_CONTEXT("element size %zu, alignment %zu", size, rows[row].alignment);
        TB_TEST_EXPECT_UINT(tb_array_element_size(array), ==, size);
        for (size_t index = 0; index < 20; index++)
        {
            memset(element, (int)(index + 1), size);
            (void)tb_array_push(array, element);
        }
        for (size_t index = 0; index < 20; index++)
        {
            memset(element, (int)(index + 1), size);
            tb_array_get(array, index, read_back);
            TB_TEST_EXPECT_BYTES(read_back, element, size);
            /* Every element is aligned, because its size is a multiple of
             * its alignment. */
            TB_TEST_EXPECT(is_aligned(tb_array_at(array, index), rows[row].alignment));
        }
        tb_array_destroy(array);
    }
}

TB_TEST(core_array, typed_macros_store_and_return_the_element_type)
{
    tb_array *const scores = TB_ARRAY_CREATE(NULL, int);
    tb_array *const vectors = TB_ARRAY_CREATE(NULL, wide_vector);
    wide_vector vector = {0};
    int total = {0};

    TB_ARRAY_PUSH(scores, int, 40);
    TB_ARRAY_PUSH(scores, int, 2);
    /* The value is converted to the element type, as by assignment. */
    TB_ARRAY_PUSH(scores, int, 'A');
    for (size_t index = 0; index < tb_array_length(scores); index++)
    {
        total += *TB_ARRAY_AT(scores, int, index);
    }
    TB_TEST_EXPECT_INT(total, ==, 42 + 'A');
    *TB_ARRAY_AT(scores, int, 0) = 7;
    TB_TEST_EXPECT_INT(TB_ARRAY_DATA(scores, int)[0], ==, 7);

    vector.lanes[15] = 3.0f;
    TB_ARRAY_PUSH(vectors, wide_vector, vector);
    TB_ARRAY_PUSH(vectors, wide_vector, vector);
    TB_TEST_EXPECT(is_aligned(TB_ARRAY_DATA(vectors, wide_vector), 64));
    TB_TEST_EXPECT_FLOAT((double)TB_ARRAY_AT(vectors, wide_vector, 1)->lanes[15], ==, 3.0);
    tb_array_destroy(vectors);
    tb_array_destroy(scores);
}

TB_TEST(core_array, push_without_an_element_adds_a_zero_filled_one)
{
    tb_array *const array = TB_ARRAY_CREATE(NULL, particle);
    static const particle zero = {0};
    particle dirty = {0};
    particle *added = NULL;

    /* Fill the storage with non-zero bytes, then reuse it. */
    memset(&dirty, 0xFF, sizeof(dirty));
    for (size_t index = 0; index < 20; index++)
    {
        (void)tb_array_push(array, &dirty);
    }
    tb_array_clear(array);
    added = tb_array_push(array, NULL);
    TB_TEST_EXPECT_POINTER(added, ==, tb_array_at(array, 0));
    TB_TEST_EXPECT_BYTES(added, &zero, sizeof(zero));
    added = tb_array_insert(array, 0, NULL);
    TB_TEST_EXPECT_BYTES(added, &zero, sizeof(zero));
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 2);
    tb_array_destroy(array);
}

TB_TEST(core_array, get_and_put_copy_whole_elements)
{
    static const int expected[] = {99, 1, 2, 99, 4};
    tb_array *const array = create_counting_array(NULL, 5);
    int value = -1;

    tb_array_get(array, 3, &value);
    TB_TEST_EXPECT_INT(value, ==, 3);
    value = 99;
    tb_array_put(array, 3, &value);
    tb_array_put(array, 0, &value);
    value = 0;
    tb_array_get(array, 0, &value);
    TB_TEST_EXPECT_INT(value, ==, 99);
    expect_ints(array, expected, TB_TEST_COUNT_OF(expected));
    tb_array_destroy(array);
}

TB_TEST(core_array, insert_places_an_element_and_moves_the_rest_up)
{
    typedef struct insert_row
    {
        size_t index;
        int expected[6];
    } insert_row;
    static const insert_row rows[] = {
        {0, {-1, 0, 1, 2, 3, 4}}, {1, {0, -1, 1, 2, 3, 4}}, {3, {0, 1, 2, -1, 3, 4}},
        {4, {0, 1, 2, 3, -1, 4}}, {5, {0, 1, 2, 3, 4, -1}},
    };
    static const int inserted = -1;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        tb_array *const array = create_counting_array(NULL, 5);
        int *const slot = tb_array_insert(array, rows[index].index, &inserted);

        TB_TEST_CONTEXT("inserting at %zu", rows[index].index);
        TB_TEST_EXPECT_POINTER(slot, ==, tb_array_at(array, rows[index].index));
        expect_ints(array, rows[index].expected, 6);
        tb_array_destroy(array);
    }
}

TB_TEST(core_array, remove_takes_an_element_out_and_keeps_the_order)
{
    typedef struct remove_row
    {
        size_t index;
        int expected[4];
    } remove_row;
    static const remove_row rows[] = {
        {0, {1, 2, 3, 4}},
        {1, {0, 2, 3, 4}},
        {3, {0, 1, 2, 4}},
        {4, {0, 1, 2, 3}},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        tb_array *const array = create_counting_array(NULL, 5);
        int removed = -1;

        TB_TEST_CONTEXT("removing at %zu", rows[index].index);
        tb_array_remove(array, rows[index].index, &removed);
        TB_TEST_EXPECT_INT(removed, ==, (int)rows[index].index);
        expect_ints(array, rows[index].expected, 4);
        /* The removed element need not be wanted. */
        tb_array_remove(array, 0, NULL);
        TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 3);
        tb_array_destroy(array);
    }
}

TB_TEST(core_array, remove_unordered_fills_the_gap_with_the_last_element)
{
    typedef struct remove_row
    {
        size_t index;
        int expected[4];
    } remove_row;
    static const remove_row rows[] = {
        {0, {4, 1, 2, 3}},
        {1, {0, 4, 2, 3}},
        {3, {0, 1, 2, 4}},
        {4, {0, 1, 2, 3}},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        tb_array *const array = create_counting_array(NULL, 5);
        int removed = -1;

        TB_TEST_CONTEXT("removing at %zu", rows[index].index);
        tb_array_remove_unordered(array, rows[index].index, &removed);
        TB_TEST_EXPECT_INT(removed, ==, (int)rows[index].index);
        expect_ints(array, rows[index].expected, 4);
        tb_array_remove_unordered(array, 0, NULL);
        TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 3);
        tb_array_destroy(array);
    }
}

TB_TEST(core_array, pop_returns_the_elements_in_reverse_order)
{
    tb_array *const array = create_counting_array(NULL, 100);

    for (int expected = 99; expected >= 1; expected--)
    {
        int popped = -1;

        tb_array_pop(array, &popped);
        TB_TEST_EXPECT_INT(popped, ==, expected);
    }
    tb_array_pop(array, NULL);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 0);
    tb_array_destroy(array);
}

TB_TEST(core_array, append_adds_a_run_of_elements)
{
    static const int first[] = {1, 2, 3};
    static const int second[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    static const int expected[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                   11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    tb_array *const array = TB_ARRAY_CREATE(NULL, int);

    tb_array_append(array, NULL, 0);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 0);
    tb_array_append(array, first, TB_TEST_COUNT_OF(first));
    tb_array_append(array, second, 0);
    tb_array_append(array, second, TB_TEST_COUNT_OF(second));
    expect_ints(array, expected, TB_TEST_COUNT_OF(expected));
    tb_array_destroy(array);
}

/* --- size and storage --------------------------------------------------------- */

TB_TEST(core_array, capacity_doubles_so_that_appending_is_cheap)
{
    enum
    {
        ELEMENT_COUNT = 1000000,
        /* The first capacity of eight, then seventeen doublings, holds a
         * million elements. */
        GROWTH_LIMIT = 18
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_array *const array = TB_ARRAY_CREATE(&allocator, int);
    size_t capacity = {0};
    size_t growth_count = {0};
    long long total = {0};

    for (size_t index = 0; index < ELEMENT_COUNT; index++)
    {
        TB_ARRAY_PUSH(array, int, (int)index);
        if (tb_array_capacity(array) != capacity)
        {
            /* Each growth at least doubles the room. */
            TB_TEST_EXPECT_UINT(tb_array_capacity(array), >=, capacity * 2);
            capacity = tb_array_capacity(array);
            growth_count++;
        }
    }
    TB_TEST_EXPECT_UINT(growth_count, <=, GROWTH_LIMIT);
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==,
                        growth_count + 1);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), <, 2 * ELEMENT_COUNT + 16);
    for (size_t index = 0; index < ELEMENT_COUNT; index++)
    {
        total += *TB_ARRAY_AT(array, int, index);
    }
    TB_TEST_EXPECT_INT(total, ==, (long long)ELEMENT_COUNT * (ELEMENT_COUNT - 1) / 2);
    tb_array_destroy(array);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

TB_TEST(core_array, reserve_makes_room_once_and_keeps_pointers_valid)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_array *const array = TB_ARRAY_CREATE(&allocator, int);
    const int *first = NULL;
    size_t requests = {0};

    tb_array_reserve(array, 1000);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 1000);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 0);
    requests = tracker.allocation_count + tracker.reallocation_count;
    TB_ARRAY_PUSH(array, int, 0);
    first = TB_ARRAY_AT(array, int, 0);
    for (int value = 1; value < 1000; value++)
    {
        TB_ARRAY_PUSH(array, int, value);
    }
    /* No further request was made, so the storage never moved. */
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==, requests);
    TB_TEST_EXPECT_POINTER(TB_ARRAY_AT(array, int, 0), ==, first);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 1000);

    /* A capacity the array already has changes nothing. */
    tb_array_reserve(array, 10);
    TB_TEST_EXPECT(tb_array_try_reserve(array, 1000));
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 1000);
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==, requests);
    tb_array_destroy(array);
}

TB_TEST(core_array, resize_adds_zero_filled_elements_and_removes_from_the_end)
{
    tb_array *const array = TB_ARRAY_CREATE(NULL, int);
    static const int kept[] = {7, 7, 7, 0, 0, 0, 0, 0, 0, 0};
    size_t capacity = {0};

    /* Dirty the storage first, so that zeroes are the array's doing. */
    for (size_t index = 0; index < 50; index++)
    {
        TB_ARRAY_PUSH(array, int, 7);
    }
    capacity = tb_array_capacity(array);
    tb_array_resize(array, 3);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 3);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, capacity);
    tb_array_resize(array, 10);
    expect_ints(array, kept, TB_TEST_COUNT_OF(kept));
    tb_array_resize(array, 10);
    expect_ints(array, kept, TB_TEST_COUNT_OF(kept));

    /* Beyond the capacity the array grows. */
    tb_array_resize(array, 5000);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 5000);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), >=, 5000);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 2), ==, 7);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 4999), ==, 0);
    tb_array_resize(array, 0);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 0);
    tb_array_destroy(array);
}

TB_TEST(core_array, clear_removes_every_element_and_keeps_the_storage)
{
    tb_array *const array = create_counting_array(NULL, 100);
    const size_t capacity = tb_array_capacity(array);
    const void *const data = tb_array_data(array);

    tb_array_clear(array);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, capacity);
    TB_TEST_EXPECT_POINTER(tb_array_data(array), ==, data);
    tb_array_clear(array);
    TB_ARRAY_PUSH(array, int, 5);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 0), ==, 5);
    tb_array_destroy(array);
}

TB_TEST(core_array, shrink_to_fit_gives_back_the_unused_storage)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_array *const array = create_counting_array(&allocator, 100);
    static const int head[] = {0, 1, 2, 3, 4};

    TB_TEST_EXPECT_UINT(tb_array_capacity(array), >, 100);
    tb_array_shrink_to_fit(array);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 100);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count - 100 * sizeof(int), <, 100);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 99), ==, 99);
    /* Already as small as it can be. */
    tb_array_shrink_to_fit(array);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 100);

    tb_array_resize(array, 5);
    tb_array_shrink_to_fit(array);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 5);
    expect_ints(array, head, TB_TEST_COUNT_OF(head));

    /* An empty array gives back all of its storage. */
    tb_array_clear(array);
    tb_array_shrink_to_fit(array);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 0);
    TB_TEST_EXPECT_POINTER(tb_array_data(array), ==, NULL);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    TB_ARRAY_PUSH(array, int, 1);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 1);
    tb_array_destroy(array);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_array, copy_makes_an_independent_array_in_any_allocator)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_array *const original = create_counting_array(NULL, 100);
    tb_array *const copy = tb_array_copy(original, &allocator);
    tb_array *const empty = TB_ARRAY_CREATE(NULL, particle);
    tb_array *const empty_copy = tb_array_copy(empty, &allocator);
    tb_array *const few = create_counting_array(NULL, 3);
    tb_array *const few_copy = tb_array_copy(few, &allocator);

    TB_TEST_EXPECT_UINT(tb_array_length(copy), ==, 100);
    TB_TEST_EXPECT_UINT(tb_array_capacity(copy), ==, 100);
    TB_TEST_EXPECT_UINT(tb_array_element_size(copy), ==, sizeof(int));
    TB_TEST_EXPECT_BYTES(tb_array_data(copy), tb_array_data(original), 100 * sizeof(int));
    TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, tb_array_data(copy), 100 * sizeof(int)));

    /* Changing one does not change the other. */
    *TB_ARRAY_AT(copy, int, 0) = -5;
    tb_array_pop(original, NULL);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(original, int, 0), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_length(copy), ==, 100);

    TB_TEST_EXPECT_UINT(tb_array_length(empty_copy), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_capacity(empty_copy), ==, 0);
    TB_TEST_EXPECT_UINT(tb_array_element_size(empty_copy), ==, sizeof(particle));
    /* A copy is exactly as large as its elements, however few. */
    TB_TEST_EXPECT_UINT(tb_array_length(few_copy), ==, 3);
    TB_TEST_EXPECT_UINT(tb_array_capacity(few_copy), ==, 3);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(few_copy, int, 2), ==, 2);
    tb_array_destroy(few_copy);
    tb_array_destroy(few);
    tb_array_destroy(empty_copy);
    tb_array_destroy(empty);
    tb_array_destroy(copy);
    tb_array_destroy(original);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- an array in an arena ----------------------------------------------------- */

TB_TEST(core_array, an_array_in_an_arena_grows_in_place_and_needs_no_destroy)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);
    const size_t blocks = tb_memory_allocation_count();
    tb_array *const array = TB_ARRAY_CREATE(&allocator, int);
    const int *first = NULL;

    TB_ARRAY_PUSH(array, int, 0);
    first = TB_ARRAY_AT(array, int, 0);
    for (int value = 1; value < 5000; value++)
    {
        TB_ARRAY_PUSH(array, int, value);
    }
    /* The array is the most recent block of the arena, so every growth
     * extended it where it was. */
    TB_TEST_EXPECT_POINTER(TB_ARRAY_AT(array, int, 0), ==, first);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 4999), ==, 4999);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
    /* Destroying the arena releases the array with everything else in it. */
    tb_arena_destroy(arena);
}

/* --- running out of memory ---------------------------------------------------- */

static void push_one(void *context)
{
    TB_ARRAY_PUSH((tb_array *)context, int, 1);
}

static void reserve_a_thousand(void *context)
{
    tb_array_reserve(context, 1000);
}

static void reserve_more_than_memory_can_hold(void *context)
{
    tb_array_reserve(context, SIZE_MAX / sizeof(int) + 1);
}

static void create_an_array(void *context)
{
    (void)TB_ARRAY_CREATE(context, int);
}

TB_TEST(core_array, an_exhausted_allocator_ends_growth_and_leaves_the_array_intact)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    static const int expected[] = {0, 1, 2, 3, 4, 5, 6, 7};
    tb_allocator failing = allocator;
    tb_array *const array = create_counting_array(&allocator, 8);

    TB_TEST_REQUIRE_UINT(tb_array_capacity(array), ==, 8);
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(push_one, array, "out of memory: 9 x 4 bytes");
    TB_TEST_EXPECT_TRAP(reserve_a_thousand, array, "out of memory: 1000 x 4 bytes");
    TB_TEST_EXPECT_TRAP(create_an_array, &failing, "out of memory");
    TB_TEST_EXPECT(!tb_array_try_reserve(array, 9));
    expect_ints(array, expected, TB_TEST_COUNT_OF(expected));
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 8);
    /* Shrinking that cannot be done leaves the array as it was. */
    tb_array_pop(array, NULL);
    tb_array_shrink_to_fit(array);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 8);
    tb_test_allocator_stop_failing(&tracker);

    TB_ARRAY_PUSH(array, int, 7);
    TB_ARRAY_PUSH(array, int, 8);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 9);
    tb_array_destroy(array);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_array, a_capacity_that_cannot_be_expressed_is_refused)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_array *const array = create_counting_array(&allocator, 3);
    const size_t requests = tracker.allocation_count + tracker.reallocation_count;

    TB_TEST_EXPECT(!tb_array_try_reserve(array, SIZE_MAX / sizeof(int) + 1));
    TB_TEST_EXPECT(!tb_array_try_reserve(array, SIZE_MAX));
    TB_TEST_EXPECT_TRAP(reserve_more_than_memory_can_hold, array, "out of memory");
    /* The allocator was never asked for a size that wrapped around. */
    TB_TEST_EXPECT_UINT(tracker.allocation_count + tracker.reallocation_count, ==, requests);
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 3);
    tb_array_destroy(array);
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

TB_TEST(core_array, growth_settles_for_what_it_needs_when_double_cannot_be_had)
{
    limited_allocator limited = {.size_limit = 100 * sizeof(int)};
    const tb_allocator allocator = {.function = limited_function, .context = &limited};
    tb_array *const array = TB_ARRAY_CREATE(&allocator, int);

    tb_array_reserve(array, 64);
    for (int value = 0; value < 64; value++)
    {
        TB_ARRAY_PUSH(array, int, value);
    }
    /* Double the capacity is more than the allocator will give. One more
     * element is not, and that is enough to carry on. */
    TB_ARRAY_PUSH(array, int, 64);
    TB_TEST_EXPECT_UINT(limited.refusal_count, ==, 1);
    TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, 65);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 64), ==, 64);
    TB_TEST_EXPECT_INT(*TB_ARRAY_AT(array, int, 0), ==, 0);
    tb_array_destroy(array);
}

/* --- random operations against a model ---------------------------------------- */

enum
{
    MODEL_LENGTH_LIMIT = 300,
    MODEL_OPERATION_COUNT = 30000
};

/* The reference: a plain C array that is always large enough. */
typedef struct array_model
{
    int values[MODEL_LENGTH_LIMIT];
    size_t length;
} array_model;

static void model_insert(array_model *model, size_t index, int value)
{
    memmove(&model->values[index + 1], &model->values[index],
            (model->length - index) * sizeof(int));
    model->values[index] = value;
    model->length++;
}

static int model_remove(array_model *model, size_t index)
{
    const int removed = model->values[index];

    model->length--;
    memmove(&model->values[index], &model->values[index + 1],
            (model->length - index) * sizeof(int));
    return removed;
}

static void run_model(const tb_allocator *allocator)
{
    static array_model model = {0};
    tb_array *array = TB_ARRAY_CREATE(allocator, int);
    size_t removal_count = {0};

    memset(&model, 0, sizeof(model));
    for (size_t operation = 0; operation < MODEL_OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(100);
        const int value = (int)tb_test_random_between(-1000000, 1000000);
        int removed = {0};

        TB_TEST_CONTEXT("operation %zu (choice %u) on %zu elements", operation, (unsigned)choice,
                        model.length);
        if (model.length == MODEL_LENGTH_LIMIT || choice < 2)
        {
            const size_t length = (size_t)tb_test_random_below(model.length + 1);

            tb_array_resize(array, length);
            model.length = length;
        }
        else if (choice < 40 || model.length == 0)
        {
            TB_ARRAY_PUSH(array, int, value);
            model_insert(&model, model.length, value);
        }
        else if (choice < 55)
        {
            const size_t index = (size_t)tb_test_random_below(model.length + 1);

            (void)tb_array_insert(array, index, &value);
            model_insert(&model, index, value);
        }
        else if (choice < 68)
        {
            const size_t index = (size_t)tb_test_random_below(model.length);

            tb_array_remove(array, index, &removed);
            TB_TEST_EXPECT_INT(removed, ==, model_remove(&model, index));
            removal_count++;
        }
        else if (choice < 78)
        {
            const size_t index = (size_t)tb_test_random_below(model.length);

            tb_array_remove_unordered(array, index, &removed);
            TB_TEST_EXPECT_INT(removed, ==, model.values[index]);
            model.length--;
            model.values[index] = model.values[model.length];
            removal_count++;
        }
        else if (choice < 86)
        {
            tb_array_pop(array, &removed);
            TB_TEST_EXPECT_INT(removed, ==, model_remove(&model, model.length - 1));
            removal_count++;
        }
        else if (choice < 92)
        {
            const size_t index = (size_t)tb_test_random_below(model.length);

            tb_array_put(array, index, &value);
            model.values[index] = value;
        }
        else if (choice < 95)
        {
            tb_array_shrink_to_fit(array);
            TB_TEST_EXPECT_UINT(tb_array_capacity(array), ==, model.length);
        }
        else if (choice < 98)
        {
            tb_array_reserve(array, model.length + (size_t)tb_test_random_below(64));
        }
        else
        {
            tb_array *const copy = tb_array_copy(array, allocator);

            tb_array_destroy(array);
            array = copy;
        }
        TB_TEST_REQUIRE_UINT(tb_array_length(array), ==, model.length);
        TB_TEST_REQUIRE_UINT(tb_array_capacity(array), >=, model.length);
        if (model.length > 0)
        {
            TB_TEST_REQUIRE_BYTES(tb_array_data(array), model.values, model.length * sizeof(int));
        }
    }
    TB_TEST_EXPECT_UINT(removal_count, >, MODEL_OPERATION_COUNT / 5);
    tb_array_destroy(array);
}

TB_TEST(core_array, random_operations_agree_with_a_plain_array)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    run_model(&allocator);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_array, random_operations_agree_with_a_plain_array_in_an_arena)
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
    (void)tb_array_length(NULL);
}

static void capacity_of_null(void *context)
{
    (void)context;
    (void)tb_array_capacity(NULL);
}

static void element_size_of_null(void *context)
{
    (void)context;
    (void)tb_array_element_size(NULL);
}

static void data_of_null(void *context)
{
    (void)context;
    (void)tb_array_data(NULL);
}

static void at_of_null(void *context)
{
    (void)context;
    (void)tb_array_at(NULL, 0);
}

static void push_to_null(void *context)
{
    (void)tb_array_push(NULL, context);
}

static void append_to_null(void *context)
{
    tb_array_append(NULL, context, 1);
}

static void insert_into_null(void *context)
{
    (void)tb_array_insert(NULL, 0, context);
}

static void pop_from_null(void *context)
{
    (void)context;
    tb_array_pop(NULL, NULL);
}

static void remove_from_null(void *context)
{
    (void)context;
    tb_array_remove(NULL, 0, NULL);
}

static void remove_unordered_from_null(void *context)
{
    (void)context;
    tb_array_remove_unordered(NULL, 0, NULL);
}

static void resize_null(void *context)
{
    (void)context;
    tb_array_resize(NULL, 1);
}

static void clear_null(void *context)
{
    (void)context;
    tb_array_clear(NULL);
}

static void reserve_null(void *context)
{
    (void)context;
    tb_array_reserve(NULL, 1);
}

static void try_reserve_null(void *context)
{
    (void)context;
    (void)tb_array_try_reserve(NULL, 1);
}

static void shrink_null(void *context)
{
    (void)context;
    tb_array_shrink_to_fit(NULL);
}

static void copy_null(void *context)
{
    (void)context;
    (void)tb_array_copy(NULL, NULL);
}

TB_TEST(core_array, a_null_array_violates_the_contract)
{
    static tb_test_trap_body *const bodies[] = {
        length_of_null, capacity_of_null, element_size_of_null, data_of_null,
        at_of_null,     push_to_null,     append_to_null,       insert_into_null,
        pop_from_null,  remove_from_null, resize_null,          remove_unordered_from_null,
        clear_null,     reserve_null,     try_reserve_null,     shrink_null,
        copy_null,
    };
    int element = 1;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(bodies); index++)
    {
        TB_TEST_CONTEXT("function %zu", index);
        TB_TEST_EXPECT_TRAP(bodies[index], &element, "array != NULL");
    }
}

typedef struct element_request
{
    tb_array *array;
    size_t index;
    int element;
} element_request;

static void at_index(void *context)
{
    const element_request *const request = context;

    (void)tb_array_at(request->array, request->index);
}

static void get_index(void *context)
{
    element_request *const request = context;

    tb_array_get(request->array, request->index, &request->element);
}

static void put_index(void *context)
{
    const element_request *const request = context;

    tb_array_put(request->array, request->index, &request->element);
}

static void remove_index(void *context)
{
    const element_request *const request = context;

    tb_array_remove(request->array, request->index, NULL);
}

static void remove_unordered_index(void *context)
{
    const element_request *const request = context;

    tb_array_remove_unordered(request->array, request->index, NULL);
}

static void insert_index(void *context)
{
    const element_request *const request = context;

    (void)tb_array_insert(request->array, request->index, &request->element);
}

static void typed_at_index(void *context)
{
    const element_request *const request = context;

    (void)TB_ARRAY_AT(request->array, int, request->index);
}

TB_TEST(core_array, an_index_outside_the_array_violates_the_contract)
{
    static const size_t lengths[] = {0, 1, 5};
    static const char *const expected = "index must be less than the length";

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(lengths); index++)
    {
        element_request request = {.array = create_counting_array(NULL, lengths[index]),
                                   .index = lengths[index]};

        TB_TEST_CONTEXT("length %zu", lengths[index]);
        TB_TEST_EXPECT_TRAP(at_index, &request, expected);
        TB_TEST_EXPECT_TRAP(typed_at_index, &request, expected);
        TB_TEST_EXPECT_TRAP(get_index, &request, expected);
        TB_TEST_EXPECT_TRAP(put_index, &request, expected);
        TB_TEST_EXPECT_TRAP(remove_index, &request, expected);
        TB_TEST_EXPECT_TRAP(remove_unordered_index, &request, expected);
        request.index = SIZE_MAX;
        TB_TEST_EXPECT_TRAP(at_index, &request, expected);
        /* Inserting at the length appends; one past it is outside. */
        request.index = lengths[index] + 1;
        TB_TEST_EXPECT_TRAP(insert_index, &request, "index must be at most the length");
        TB_TEST_EXPECT_UINT(tb_array_length(request.array), ==, lengths[index]);
        tb_array_destroy(request.array);
    }
}

static void pop_from(void *context)
{
    tb_array_pop(context, NULL);
}

static void get_into_null(void *context)
{
    tb_array_get(context, 0, NULL);
}

static void put_from_null(void *context)
{
    tb_array_put(context, 0, NULL);
}

static void append_null_elements(void *context)
{
    tb_array_append(context, NULL, 3);
}

static void at_as_another_type(void *context)
{
    (void)TB_ARRAY_AT((tb_array *)context, char, 0);
}

static void data_as_another_type(void *context)
{
    (void)TB_ARRAY_DATA((tb_array *)context, double);
}

static void push_another_type(void *context)
{
    TB_ARRAY_PUSH((tb_array *)context, short, 1);
}

static void create_with_no_element_size(void *context)
{
    (void)context;
    (void)tb_array_create(NULL, 0, 1);
}

static void create_with_a_bad_alignment(void *context)
{
    (void)context;
    (void)tb_array_create(NULL, 12, 12);
}

static void create_with_a_size_that_does_not_fit_its_alignment(void *context)
{
    (void)context;
    (void)tb_array_create(NULL, 12, 8);
}

TB_TEST(core_array, misuse_of_elements_violates_the_contract)
{
    tb_array *array = NULL;
    tb_array *empty = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    array = create_counting_array(NULL, 3);
    empty = TB_ARRAY_CREATE(NULL, int);
    TB_TEST_EXPECT_TRAP(pop_from, empty, "the array must not be empty");
    TB_TEST_EXPECT_TRAP(get_into_null, array, "element != NULL");
    TB_TEST_EXPECT_TRAP(put_from_null, array, "element != NULL");
    TB_TEST_EXPECT_TRAP(append_null_elements, array, "elements != NULL || count == 0");
    TB_TEST_EXPECT_TRAP(at_as_another_type, array, "the array holds elements of another size");
    TB_TEST_EXPECT_TRAP(data_as_another_type, array, "the array holds elements of another size");
    TB_TEST_EXPECT_TRAP(push_another_type, array, "the array holds elements of another size");
    TB_TEST_EXPECT_TRAP(create_with_no_element_size, NULL, "an element has at least one byte");
    TB_TEST_EXPECT_TRAP(create_with_a_bad_alignment, NULL,
                        "element alignment must be a power of two");
    TB_TEST_EXPECT_TRAP(create_with_a_size_that_does_not_fit_its_alignment, NULL,
                        "element size must be a multiple of the element alignment");
    TB_TEST_EXPECT_UINT(tb_array_length(array), ==, 3);
    tb_array_destroy(empty);
    tb_array_destroy(array);
}
