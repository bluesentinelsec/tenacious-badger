/* Every module that owns memory takes a tb_allocator, so the interface is
 * pinned here from both sides. The allocators the engine supplies, its heap
 * and an arena, must hand out usable, aligned blocks and keep their contents
 * across a resize. A custom allocator must be told exactly what the header
 * promises: the true size and alignment of every block, and never a size of
 * zero for a block. Exhaustion must be fatal or reported as documented. */
#include "badger/allocator.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/memory.h"
#include "engine_test.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

/* Writes a pattern that depends on the seed and on each byte's position, so a
 * block that was truncated, or overlapped by another, is detected. */
static void fill_pattern(void *memory, size_t size, size_t seed)
{
    unsigned char *const bytes = memory;

    for (size_t index = 0; index < size; index++)
    {
        bytes[index] = (unsigned char)((seed + index * 31u) & 0xFFu);
    }
}

/* Index of the first byte that does not hold the pattern, or `size`. */
static size_t find_pattern_mismatch(const void *memory, size_t size, size_t seed)
{
    const unsigned char *const bytes = memory;

    for (size_t index = 0; index < size; index++)
    {
        if (bytes[index] != (unsigned char)((seed + index * 31u) & 0xFFu))
        {
            return index;
        }
    }
    return size;
}

static size_t count_bytes_other_than(const void *memory, size_t size, unsigned char value)
{
    const unsigned char *const bytes = memory;
    size_t count = {0};

    for (size_t index = 0; index < size; index++)
    {
        if (bytes[index] != value)
        {
            count++;
        }
    }
    return count;
}

static bool is_aligned(const void *memory, size_t alignment)
{
    return ((uintptr_t)memory & (uintptr_t)(alignment - 1)) == 0;
}

enum
{
    /* 64 KiB: beyond the page size of every supported platform. */
    LARGEST_ALIGNMENT_SHIFT = 16,
    ARENA_CHUNK_SIZE = 4096
};

static const size_t block_sizes[] = {0, 1, 3, 8, 24, 100, 4096, 5000};

/* --- the engine's heap -------------------------------------------------------- */

TB_TEST(core_allocator, null_selects_the_heap)
{
    const tb_allocator heap = tb_allocator_heap();
    const tb_allocator resolved = tb_allocator_resolve(NULL);
    const size_t blocks_before = tb_memory_allocation_count();
    unsigned char *from_null = NULL;
    unsigned char *from_heap = NULL;

    TB_TEST_EXPECT(resolved.function == heap.function);
    TB_TEST_EXPECT_POINTER(resolved.context, ==, heap.context);
    TB_TEST_REQUIRE(heap.function != NULL);

    /* A block from one can be resized and released through the other. */
    from_null = tb_allocator_alloc(NULL, 100, 8);
    from_heap = tb_allocator_alloc(&heap, 100, 8);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before + 2);
    fill_pattern(from_null, 100, 1);
    from_null = tb_allocator_realloc(&heap, from_null, 100, 300, 8);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(from_null, 100, 1), ==, 100);
    tb_allocator_free(&heap, from_null, 300, 8);
    tb_allocator_free(NULL, from_heap, 100, 8);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

TB_TEST(core_allocator, resolve_copies_the_allocator_it_is_given)
{
    tb_test_allocator tracker = {0};
    tb_allocator original = tb_test_allocator_start(&tracker);
    const tb_allocator copy = tb_allocator_resolve(&original);

    TB_TEST_EXPECT(copy.function == original.function);
    TB_TEST_EXPECT_POINTER(copy.context, ==, &tracker);
    /* The copy does not depend on the structure it was made from. */
    original.function = NULL;
    original.context = NULL;
    tb_allocator_free(&copy, tb_allocator_alloc(&copy, 16, 8), 16, 8);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_allocator, heap_blocks_are_distinct_usable_and_aligned)
{
    const size_t blocks_before = tb_memory_allocation_count();
    unsigned char *blocks[LARGEST_ALIGNMENT_SHIFT + 1][TB_TEST_COUNT_OF(block_sizes)] = {0};

    for (size_t shift = 0; shift <= LARGEST_ALIGNMENT_SHIFT; shift++)
    {
        for (size_t index = 0; index < TB_TEST_COUNT_OF(block_sizes); index++)
        {
            const size_t alignment = (size_t)1 << shift;

            TB_TEST_CONTEXT("alignment %zu, size %zu", alignment, block_sizes[index]);
            blocks[shift][index] = tb_allocator_alloc(NULL, block_sizes[index], alignment);
            TB_TEST_REQUIRE_POINTER(blocks[shift][index], !=, NULL);
            TB_TEST_EXPECT(is_aligned(blocks[shift][index], alignment));
            fill_pattern(blocks[shift][index], block_sizes[index], shift * 100 + index);
        }
    }
    /* Every block still holds its own pattern, so no two of them overlap. */
    for (size_t shift = 0; shift <= LARGEST_ALIGNMENT_SHIFT; shift++)
    {
        for (size_t index = 0; index < TB_TEST_COUNT_OF(block_sizes); index++)
        {
            TB_TEST_CONTEXT("alignment %zu, size %zu", (size_t)1 << shift, block_sizes[index]);
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(blocks[shift][index], block_sizes[index],
                                                      shift * 100 + index),
                                ==, block_sizes[index]);
            tb_allocator_free(NULL, blocks[shift][index], block_sizes[index], (size_t)1 << shift);
        }
    }
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

typedef struct resize_row
{
    size_t old_size;
    size_t new_size;
} resize_row;

static const resize_row resize_rows[] = {
    {1, 2},      {8, 9},     {16, 4096}, {100, 100}, {4096, 100000},
    {100000, 7}, {4096, 16}, {9, 8},     {1, 0},     {0, 50},
};

/* Resizes blocks of two alignments, one that the heap resizes directly and
 * one beyond it, and checks that the kept bytes survive. */
static void check_resizing(const tb_allocator *allocator)
{
    static const size_t alignments[] = {8, 256};

    for (size_t alignment_index = 0; alignment_index < TB_TEST_COUNT_OF(alignments);
         alignment_index++)
    {
        for (size_t index = 0; index < TB_TEST_COUNT_OF(resize_rows); index++)
        {
            const size_t alignment = alignments[alignment_index];
            const resize_row row = resize_rows[index];
            const size_t kept_size = row.old_size < row.new_size ? row.old_size : row.new_size;
            unsigned char *block = tb_allocator_alloc(allocator, row.old_size, alignment);

            TB_TEST_CONTEXT("alignment %zu, %zu to %zu bytes", alignment, row.old_size,
                            row.new_size);
            fill_pattern(block, row.old_size, index);
            block = tb_allocator_realloc(allocator, block, row.old_size, row.new_size, alignment);
            TB_TEST_REQUIRE_POINTER(block, !=, NULL);
            TB_TEST_EXPECT(is_aligned(block, alignment));
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, kept_size, index), ==, kept_size);
            /* All of the new size is usable. */
            fill_pattern(block, row.new_size, index + 1);
            tb_allocator_free(allocator, block, row.new_size, alignment);
        }
    }
}

TB_TEST(core_allocator, resizing_a_heap_block_keeps_its_contents)
{
    const size_t blocks_before = tb_memory_allocation_count();

    check_resizing(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

TB_TEST(core_allocator, resizing_nothing_allocates)
{
    unsigned char *const block = tb_allocator_realloc(NULL, NULL, 12345, 64, 16);
    unsigned char *const fallible = tb_allocator_try_realloc(NULL, NULL, 0, 64, 16);

    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    TB_TEST_REQUIRE_POINTER(fallible, !=, NULL);
    fill_pattern(block, 64, 1);
    fill_pattern(fallible, 64, 2);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, 64, 1), ==, 64);
    tb_allocator_free(NULL, block, 64, 16);
    tb_allocator_free(NULL, fallible, 64, 16);
}

TB_TEST(core_allocator, releasing_null_does_nothing)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    const size_t blocks_before = tb_memory_allocation_count();

    tb_allocator_free(NULL, NULL, 16, 8);
    tb_allocator_free_array(NULL, NULL, 4, 4, 8);
    tb_allocator_free(&allocator, NULL, 16, 8);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
    TB_TEST_EXPECT_UINT(tracker.release_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

/* --- zero-filled and typed allocation ----------------------------------------- */

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

TB_TEST(core_allocator, calloc_returns_zero_filled_arrays)
{
    static const size_t counts[] = {0, 1, 3, 16, 1000};
    static const size_t sizes[] = {0, 1, 3, 8, 24};
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    for (size_t count_index = 0; count_index < TB_TEST_COUNT_OF(counts); count_index++)
    {
        for (size_t size_index = 0; size_index < TB_TEST_COUNT_OF(sizes); size_index++)
        {
            const size_t count = counts[count_index];
            const size_t size = sizes[size_index];
            unsigned char *const array = tb_allocator_calloc(&allocator, count, size, 16);
            unsigned char *const fallible = tb_allocator_try_calloc(&allocator, count, size, 16);

            TB_TEST_CONTEXT("%zu x %zu bytes", count, size);
            TB_TEST_REQUIRE_POINTER(array, !=, NULL);
            TB_TEST_REQUIRE_POINTER(fallible, !=, NULL);
            TB_TEST_EXPECT(is_aligned(array, 16));
            TB_TEST_EXPECT_UINT(count_bytes_other_than(array, count * size, 0), ==, 0);
            TB_TEST_EXPECT_UINT(count_bytes_other_than(fallible, count * size, 0), ==, 0);
            fill_pattern(array, count * size, count_index);
            tb_allocator_free_array(&allocator, array, count, size, 16);
            tb_allocator_free_array(&allocator, fallible, count, size, 16);
        }
    }
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_allocator, typed_forms_allocate_and_release_objects_of_their_type)
{
    enum
    {
        PARTICLE_COUNT = 50
    };
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    particle *const one = TB_ALLOCATOR_NEW(&allocator, particle);
    particle *const many = TB_ALLOCATOR_NEW_ARRAY(&allocator, particle, PARTICLE_COUNT);
    wide_vector *const vector = TB_ALLOCATOR_NEW(&allocator, wide_vector);
    wide_vector *const on_heap = TB_ALLOCATOR_NEW(NULL, wide_vector);

    TB_TEST_EXPECT(is_aligned(one, _Alignof(particle)));
    TB_TEST_EXPECT(is_aligned(vector, 64));
    TB_TEST_EXPECT(is_aligned(on_heap, 64));
    TB_TEST_EXPECT_UINT(count_bytes_other_than(one, sizeof(*one), 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(many, PARTICLE_COUNT * sizeof(*many), 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(vector, sizeof(*vector), 0), ==, 0);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==,
                        sizeof(particle) * (PARTICLE_COUNT + 1) + sizeof(wide_vector));

    /* The release forms name the size and alignment the allocator recorded. */
    TB_ALLOCATOR_DELETE(&allocator, particle, one);
    TB_ALLOCATOR_DELETE_ARRAY(&allocator, particle, many, PARTICLE_COUNT);
    TB_ALLOCATOR_DELETE(&allocator, wide_vector, vector);
    TB_ALLOCATOR_DELETE(NULL, wide_vector, on_heap);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

/* --- what a custom allocator is told ------------------------------------------ */

/* The one block the recording allocator hands out, aligned for every request
 * the cases make. */
static _Alignas(64) unsigned char recording_storage[256];

/* The arguments of the most recent call to the recording allocator. */
typedef struct recorded_call
{
    size_t call_count;
    void *memory;
    size_t old_size;
    size_t new_size;
    size_t alignment;
} recorded_call;

static void *recording_function(void *context, void *memory, size_t old_size, size_t new_size,
                                size_t alignment)
{
    recorded_call *const call = context;

    call->call_count++;
    call->memory = memory;
    call->old_size = old_size;
    call->new_size = new_size;
    call->alignment = alignment;
    if (new_size == 0 || new_size > sizeof(recording_storage))
    {
        return NULL;
    }
    return recording_storage;
}

TB_TEST(core_allocator, a_custom_allocator_is_told_exactly_what_is_asked_of_it)
{
    recorded_call call = {0};
    const tb_allocator allocator = {.function = recording_function, .context = &call};
    void *block = NULL;

    block = tb_allocator_alloc(&allocator, 100, 32);
    TB_TEST_EXPECT_POINTER(block, ==, recording_storage);
    TB_TEST_EXPECT_POINTER(call.memory, ==, NULL);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 0);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 100);
    TB_TEST_EXPECT_UINT(call.alignment, ==, 32);

    block = tb_allocator_realloc(&allocator, block, 100, 200, 32);
    TB_TEST_EXPECT_POINTER(call.memory, ==, recording_storage);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 100);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 200);
    TB_TEST_EXPECT_UINT(call.alignment, ==, 32);

    tb_allocator_free(&allocator, block, 200, 32);
    TB_TEST_EXPECT_POINTER(call.memory, ==, recording_storage);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 200);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 0);
    TB_TEST_EXPECT_UINT(call.alignment, ==, 32);

    block = tb_allocator_calloc(&allocator, 5, 12, 4);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 60);
    TB_TEST_EXPECT_UINT(call.alignment, ==, 4);
    tb_allocator_free_array(&allocator, block, 5, 12, 4);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 60);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 0);
    TB_TEST_EXPECT_UINT(call.call_count, ==, 5);
}

TB_TEST(core_allocator, a_custom_allocator_never_sees_a_block_of_size_zero)
{
    recorded_call call = {0};
    const tb_allocator allocator = {.function = recording_function, .context = &call};
    void *block = NULL;

    /* A size of zero means "release" to an allocator, so an empty request is
     * served as one byte, and stays one byte when it is resized or released. */
    block = tb_allocator_alloc(&allocator, 0, 1);
    TB_TEST_EXPECT_POINTER(block, !=, NULL);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 1);

    block = tb_allocator_realloc(&allocator, block, 0, 0, 1);
    TB_TEST_EXPECT_POINTER(block, !=, NULL);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 1);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 1);

    tb_allocator_free(&allocator, block, 0, 1);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 1);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 0);

    block = tb_allocator_calloc(&allocator, 0, 0, 1);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 1);
    block = tb_allocator_calloc(&allocator, 0, 24, 8);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 24);
    tb_allocator_free_array(&allocator, block, 0, 24, 8);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 24);

    /* Nothing to resize: the old size is ignored and reported as none. */
    block = tb_allocator_realloc(&allocator, NULL, 77, 10, 2);
    TB_TEST_EXPECT_POINTER(call.memory, ==, NULL);
    TB_TEST_EXPECT_UINT(call.old_size, ==, 0);
    TB_TEST_EXPECT_UINT(call.new_size, ==, 10);
}

/* --- running out of memory ---------------------------------------------------- */

TB_TEST(core_allocator, fallible_forms_report_exhaustion_and_keep_the_block)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    unsigned char *const block = tb_allocator_alloc(&allocator, 64, 8);

    fill_pattern(block, 64, 3);
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_alloc(&allocator, 16, 8), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_calloc(&allocator, 4, 4, 8), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_realloc(&allocator, block, 64, 128, 8), ==, NULL);
    TB_TEST_EXPECT_UINT(tracker.refusal_count, ==, 3);
    /* The block that could not be resized is untouched and still owned. */
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, 64, 3), ==, 64);
    tb_test_allocator_stop_failing(&tracker);

    tb_allocator_free(&allocator, block, 64, 8);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_allocator, an_array_that_overflows_is_refused_before_the_allocator_is_asked)
{
    recorded_call call = {0};
    const tb_allocator allocator = {.function = recording_function, .context = &call};

    TB_TEST_EXPECT_POINTER(tb_allocator_try_calloc(&allocator, SIZE_MAX, 2, 8), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_calloc(&allocator, 2, SIZE_MAX, 8), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_calloc(&allocator, SIZE_MAX / 2 + 1, 2, 8), ==, NULL);
    TB_TEST_EXPECT_UINT(call.call_count, ==, 0);
}

typedef struct allocation_request
{
    const tb_allocator *allocator;
    void *memory;
} allocation_request;

static void allocate_64_bytes(void *context)
{
    const allocation_request *const request = context;

    (void)tb_allocator_alloc(request->allocator, 64, 32);
}

static void allocate_four_zeroed_elements(void *context)
{
    const allocation_request *const request = context;

    (void)tb_allocator_calloc(request->allocator, 4, 16, 8);
}

static void allocate_an_overflowing_array(void *context)
{
    const allocation_request *const request = context;

    (void)tb_allocator_calloc(request->allocator, SIZE_MAX, 2, 8);
}

static void grow_to_128_bytes(void *context)
{
    const allocation_request *const request = context;

    (void)tb_allocator_realloc(request->allocator, request->memory, 64, 128, 8);
}

TB_TEST(core_allocator, an_exhausted_allocator_ends_a_fail_fast_allocation)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    allocation_request request = {.allocator = &allocator};

    request.memory = tb_allocator_alloc(&allocator, 64, 8);
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_TRAP(allocate_64_bytes, &request, "out of memory: 1 x 64 bytes, alignment 32");
    TB_TEST_EXPECT_TRAP(allocate_four_zeroed_elements, &request,
                        "out of memory: 4 x 16 bytes, alignment 8");
    TB_TEST_EXPECT_TRAP(grow_to_128_bytes, &request, "out of memory: 1 x 128 bytes, alignment 8");
    tb_test_allocator_stop_failing(&tracker);
    TB_TEST_EXPECT_TRAP(allocate_an_overflowing_array, &request, "out of memory");

    tb_allocator_free(&allocator, request.memory, 64, 8);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_allocator, an_exhausted_heap_ends_a_fail_fast_allocation)
{
    allocation_request request = {.allocator = NULL};

    tb_engine_test_fail_allocations_after(0);
    TB_TEST_EXPECT_TRAP(allocate_64_bytes, &request, "out of memory: 1 x 64 bytes, alignment 32");
    TB_TEST_EXPECT_POINTER(tb_allocator_try_alloc(NULL, 64, 8), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_alloc(NULL, 64, 4096), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_calloc(NULL, 4, 16, 8), ==, NULL);
    tb_engine_test_stop_failing_allocations();
}

TB_TEST(core_allocator, a_heap_block_survives_a_resize_that_fails)
{
    static const size_t alignments[] = {8, 256};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(alignments); index++)
    {
        unsigned char *const block = tb_allocator_alloc(NULL, 64, alignments[index]);

        TB_TEST_CONTEXT("alignment %zu", alignments[index]);
        fill_pattern(block, 64, index);
        tb_engine_test_fail_allocations_after(0);
        TB_TEST_EXPECT_POINTER(tb_allocator_try_realloc(NULL, block, 64, 4096, alignments[index]),
                               ==, NULL);
        tb_engine_test_stop_failing_allocations();
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, 64, index), ==, 64);
        tb_allocator_free(NULL, block, 64, alignments[index]);
    }
}

/* --- an arena as an allocator ------------------------------------------------- */

TB_TEST(core_allocator, an_arena_allocator_takes_its_memory_from_the_arena)
{
    tb_arena *const arena = tb_arena_create(ARENA_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);
    const size_t blocks = tb_memory_allocation_count();
    const size_t room = tb_arena_remaining(arena);
    unsigned char *const first = tb_allocator_alloc(&allocator, 100, 1);
    unsigned char *const second = tb_allocator_calloc(&allocator, 10, 10, 1);

    TB_TEST_EXPECT_POINTER(second, ==, first + 100);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room - 200);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(second, 100, 0), ==, 0);

    /* Releasing gives nothing back: the arena does that all at once. */
    TB_TEST_EXPECT_POINTER(allocator.function(allocator.context, second, 100, 0, 1), ==, NULL);
    tb_allocator_free(&allocator, second, 100, 1);
    tb_allocator_free(&allocator, first, 100, 1);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room - 200);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    tb_arena_destroy(arena);
}

TB_TEST(core_allocator, an_arena_allocator_aligns_and_resizes_like_any_other)
{
    tb_arena *const arena = tb_arena_create(ARENA_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);

    for (size_t shift = 0; shift <= LARGEST_ALIGNMENT_SHIFT; shift++)
    {
        const size_t alignment = (size_t)1 << shift;
        unsigned char *const block = tb_allocator_alloc(&allocator, 24, alignment);

        TB_TEST_CONTEXT("alignment %zu", alignment);
        TB_TEST_EXPECT(is_aligned(block, alignment));
        fill_pattern(block, 24, shift);
        /* An odd position, so the next block needs padding to align. */
        (void)tb_allocator_alloc(&allocator, 1, 1);
        TB_TEST_EXPECT(
            is_aligned(tb_allocator_realloc(&allocator, block, 24, 5000, alignment), alignment));
    }
    check_resizing(&allocator);
    tb_arena_destroy(arena);
}

TB_TEST(core_allocator, an_arena_allocator_grows_the_most_recent_block_in_place)
{
    tb_arena *const arena = tb_arena_create(ARENA_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);
    const size_t blocks = tb_memory_allocation_count();
    unsigned char *const earlier = tb_allocator_alloc(&allocator, 50, 1);
    unsigned char *array = tb_allocator_alloc(&allocator, 8, 8);
    unsigned char *const start = array;
    size_t size = 8;
    size_t room = {0};

    fill_pattern(earlier, 50, 1);
    fill_pattern(array, size, 2);
    /* An array that doubles while it is the last block never moves and uses
     * exactly its own size, until the chunk is full. */
    while (size * 2 <= tb_arena_remaining(arena) + size)
    {
        room = tb_arena_remaining(arena);
        array = tb_allocator_realloc(&allocator, array, size, size * 2, 8);
        TB_TEST_CONTEXT("growing %zu bytes to %zu", size, size * 2);
        TB_TEST_REQUIRE_POINTER(array, ==, start);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room - size);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(array, size, 2), ==, size);
        size *= 2;
        fill_pattern(array, size, 2);
    }
    TB_TEST_EXPECT_UINT(size, >=, ARENA_CHUNK_SIZE / 4);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);

    /* The chunk cannot hold the next size, so the block moves to another. */
    array = tb_allocator_realloc(&allocator, array, size, size * 2, 8);
    TB_TEST_EXPECT_POINTER(array, !=, start);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks + 1);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(array, size, 2), ==, size);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(earlier, 50, 1), ==, 50);
    tb_arena_destroy(arena);
}

TB_TEST(core_allocator, an_arena_allocator_moves_a_block_that_is_not_the_most_recent)
{
    tb_arena *const arena = tb_arena_create(ARENA_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);
    unsigned char *const first = tb_allocator_alloc(&allocator, 40, 8);
    unsigned char *const second = tb_allocator_alloc(&allocator, 40, 8);
    unsigned char *grown = NULL;
    size_t room = {0};

    fill_pattern(first, 40, 1);
    fill_pattern(second, 40, 2);
    grown = tb_allocator_realloc(&allocator, first, 40, 400, 8);
    TB_TEST_EXPECT_POINTER(grown, !=, first);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(grown, 40, 1), ==, 40);
    fill_pattern(grown, 400, 3);
    /* The block that stayed is untouched by its neighbor's move. */
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(second, 40, 2), ==, 40);

    /* A block that shrinks stays where it is and gives nothing back. */
    room = tb_arena_remaining(arena);
    TB_TEST_EXPECT_POINTER(tb_allocator_realloc(&allocator, grown, 400, 10, 8), ==, grown);
    TB_TEST_EXPECT_POINTER(tb_allocator_realloc(&allocator, second, 40, 40, 8), ==, second);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(grown, 10, 3), ==, 10);
    tb_arena_destroy(arena);
}

TB_TEST(core_allocator, a_full_fixed_arena_refuses_through_its_allocator)
{
    static unsigned char buffer[ARENA_CHUNK_SIZE];
    tb_arena *const arena = tb_arena_create_fixed(buffer, sizeof(buffer));
    const tb_allocator allocator = tb_arena_allocator(arena);
    allocation_request request = {.allocator = &allocator};
    unsigned char *const kept = tb_allocator_alloc(&allocator, 64, 8);
    unsigned char *filler = NULL;

    fill_pattern(kept, 64, 4);
    filler = tb_allocator_alloc(&allocator, tb_arena_remaining(arena), 1);
    TB_TEST_EXPECT_POINTER(filler, !=, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_alloc(&allocator, 1, 1), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_calloc(&allocator, 4, 4, 4), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_allocator_try_realloc(&allocator, kept, 64, 128, 8), ==, NULL);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(kept, 64, 4), ==, 64);
    request.memory = kept;
    TB_TEST_EXPECT_TRAP(allocate_64_bytes, &request, "out of memory: 1 x 64 bytes, alignment 32");
    TB_TEST_EXPECT_TRAP(grow_to_128_bytes, &request, "out of memory: 1 x 128 bytes, alignment 8");
    tb_arena_destroy(arena);
}

TB_TEST(core_allocator, memory_grown_in_place_holds_the_fill_byte_in_checked_builds)
{
    tb_arena *arena = NULL;
    tb_allocator allocator = {0};
    unsigned char *block = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions, and the fill follows them");
    }
    arena = tb_arena_create(ARENA_CHUNK_SIZE);
    allocator = tb_arena_allocator(arena);
    block = tb_allocator_alloc(&allocator, 16, 1);
    memset(block, 0, 16);
    block = tb_allocator_realloc(&allocator, block, 16, 200, 1);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(block, 16, 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(block + 16, 184, TB_ARENA_FILL_BYTE), ==, 0);
    tb_arena_destroy(arena);
}

/* --- random operations against a model ---------------------------------------- */

enum
{
    MODEL_BLOCK_LIMIT = 64,
    MODEL_OPERATION_COUNT = 20000
};

typedef struct model_block
{
    unsigned char *memory;
    size_t size;
    size_t alignment;
    size_t seed;
} model_block;

/* Mostly small blocks, some of a few kilobytes. */
static size_t random_size(void)
{
    if (tb_test_random_below(8) == 0)
    {
        return (size_t)tb_test_random_below(6000);
    }
    return (size_t)tb_test_random_below(96);
}

/* Allocates, resizes, and releases random blocks and checks after every step
 * that the block it touched, and at the end every block, kept its contents. */
static void run_model(const tb_allocator *allocator)
{
    model_block blocks[MODEL_BLOCK_LIMIT] = {0};
    size_t block_count = {0};
    size_t resize_count = {0};

    for (size_t operation = 0; operation < MODEL_OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(10);
        model_block *block = NULL;

        TB_TEST_CONTEXT("operation %zu, with %zu blocks", operation, block_count);
        if (block_count == 0 || (choice < 4 && block_count < MODEL_BLOCK_LIMIT))
        {
            block = &blocks[block_count];
            block->size = random_size();
            block->alignment = (size_t)1 << tb_test_random_below(9);
            block->seed = operation;
            block->memory = tb_allocator_alloc(allocator, block->size, block->alignment);
            block_count++;
        }
        else if (choice < 7)
        {
            const size_t new_size = random_size();
            size_t kept_size = {0};

            block = &blocks[tb_test_random_below(block_count)];
            kept_size = block->size < new_size ? block->size : new_size;
            block->memory = tb_allocator_realloc(allocator, block->memory, block->size, new_size,
                                                 block->alignment);
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(block->memory, kept_size, block->seed), ==,
                                kept_size);
            block->size = new_size;
            block->seed = operation;
            resize_count++;
        }
        else
        {
            block = &blocks[tb_test_random_below(block_count)];
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(block->memory, block->size, block->seed), ==,
                                block->size);
            tb_allocator_free(allocator, block->memory, block->size, block->alignment);
            block_count--;
            *block = blocks[block_count];
            continue;
        }
        TB_TEST_REQUIRE_POINTER(block->memory, !=, NULL);
        TB_TEST_EXPECT(is_aligned(block->memory, block->alignment));
        fill_pattern(block->memory, block->size, block->seed);
    }
    for (size_t index = 0; index < block_count; index++)
    {
        TB_TEST_CONTEXT("block %zu of %zu at the end", index, block_count);
        TB_TEST_EXPECT_UINT(
            find_pattern_mismatch(blocks[index].memory, blocks[index].size, blocks[index].seed), ==,
            blocks[index].size);
        tb_allocator_free(allocator, blocks[index].memory, blocks[index].size,
                          blocks[index].alignment);
    }
    TB_TEST_EXPECT_UINT(resize_count, >, MODEL_OPERATION_COUNT / 5);
}

TB_TEST(core_allocator, random_operations_keep_every_heap_block_intact)
{
    const size_t blocks_before = tb_memory_allocation_count();

    run_model(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

TB_TEST(core_allocator, random_operations_keep_every_arena_block_intact)
{
    tb_arena *const arena = tb_arena_create(ARENA_CHUNK_SIZE);
    const tb_allocator allocator = tb_arena_allocator(arena);

    run_model(&allocator);
    tb_arena_destroy(arena);
}

TB_TEST(core_allocator, random_operations_follow_the_contract_of_a_custom_allocator)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);

    run_model(&allocator);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 0);
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, tracker.release_count);
}

/* --- the test allocator itself ------------------------------------------------ */

TB_TEST(core_allocator, the_test_allocator_notices_each_way_of_misusing_it)
{
    tb_test_allocator tracker = {0};
    tb_allocator allocator = tb_test_allocator_start(&tracker);
    unsigned char outsider[16] = {0};
    unsigned char *block = NULL;

    /* Each misuse is made through the raw function, because the functions of
     * allocator.h cannot be made to do it. The later cases of this project
     * trust the test allocator to object, so each objection is checked. */
    block = allocator.function(allocator.context, NULL, 0, 32, 8);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, block, 32));
    TB_TEST_EXPECT(tb_test_allocator_owns(&tracker, block + 31, 1));
    TB_TEST_EXPECT(!tb_test_allocator_owns(&tracker, block, 33));
    TB_TEST_EXPECT(!tb_test_allocator_owns(&tracker, block + 32, 1));
    TB_TEST_EXPECT(!tb_test_allocator_owns(&tracker, outsider, 1));
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");

    (void)allocator.function(allocator.context, outsider, 16, 0, 8);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 1);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==,
                          "a block was resized or released that this allocator does not own");
    (void)allocator.function(allocator.context, block, 32, 0, 8);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);

    allocator = tb_test_allocator_start(&tracker);
    block = allocator.function(allocator.context, NULL, 0, 32, 8);
    (void)allocator.function(allocator.context, NULL, 0, 0, 8);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==,
                          "a request without a block was not a plain allocation");
    (void)allocator.function(allocator.context, NULL, 0, 8, 3);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 2);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);

    /* A wrong size or alignment is recorded, and the block is still released. */
    tracker.violation_count = 0;
    (void)allocator.function(allocator.context, block, 31, 0, 16);
    TB_TEST_EXPECT_UINT(tracker.violation_count, ==, 2);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(core_allocator, the_test_allocator_moves_every_block_it_resizes)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    unsigned char *const block = tb_allocator_alloc(&allocator, 64, 8);
    unsigned char *smaller = NULL;

    fill_pattern(block, 64, 9);
    smaller = tb_allocator_realloc(&allocator, block, 64, 16, 8);
    TB_TEST_EXPECT_POINTER(smaller, !=, block);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(smaller, 16, 9), ==, 16);
    TB_TEST_EXPECT_UINT(tracker.reallocation_count, ==, 1);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 16);
    TB_TEST_EXPECT_UINT(tracker.peak_byte_count, ==, 64);
    tb_allocator_free(&allocator, smaller, 16, 8);
    TB_TEST_EXPECT_UINT(tracker.live_byte_count, ==, 0);
}

/* --- contract violations ------------------------------------------------------ */

typedef struct alignment_request
{
    size_t alignment;
    unsigned char storage[64];
} alignment_request;

static void alloc_with_alignment(void *context)
{
    const alignment_request *const request = context;

    (void)tb_allocator_alloc(NULL, 16, request->alignment);
}

static void calloc_with_alignment(void *context)
{
    const alignment_request *const request = context;

    (void)tb_allocator_calloc(NULL, 4, 4, request->alignment);
}

static void calloc_an_overflowing_array_with_alignment(void *context)
{
    const alignment_request *const request = context;

    (void)tb_allocator_try_calloc(NULL, SIZE_MAX, 2, request->alignment);
}

static void realloc_with_alignment(void *context)
{
    alignment_request *const request = context;

    (void)tb_allocator_realloc(NULL, request->storage, 16, 32, request->alignment);
}

static void free_with_alignment(void *context)
{
    alignment_request *const request = context;

    tb_allocator_free(NULL, request->storage, 16, request->alignment);
}

TB_TEST(core_allocator, an_alignment_that_is_not_a_power_of_two_violates_the_contract)
{
    static const size_t alignments[] = {0, 3, 6, 24, 100, SIZE_MAX};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(alignments); index++)
    {
        alignment_request request = {.alignment = alignments[index]};

        TB_TEST_CONTEXT("alignment %zu", alignments[index]);
        TB_TEST_EXPECT_TRAP(alloc_with_alignment, &request, "alignment must be a power of two");
        TB_TEST_EXPECT_TRAP(calloc_with_alignment, &request, "alignment must be a power of two");
        TB_TEST_EXPECT_TRAP(calloc_an_overflowing_array_with_alignment, &request,
                            "alignment must be a power of two");
        TB_TEST_EXPECT_TRAP(realloc_with_alignment, &request, "alignment must be a power of two");
        TB_TEST_EXPECT_TRAP(free_with_alignment, &request, "alignment must be a power of two");
    }
}

static void allocate_without_a_function(void *context)
{
    (void)tb_allocator_alloc(context, 16, 8);
}

static void resolve_without_a_function(void *context)
{
    (void)tb_allocator_resolve(context);
}

static void free_an_array_that_cannot_exist(void *context)
{
    tb_allocator_free_array(NULL, context, SIZE_MAX, 2, 8);
}

static void make_an_allocator_of_no_arena(void *context)
{
    (void)context;
    (void)tb_arena_allocator(NULL);
}

TB_TEST(core_allocator, an_allocator_without_a_function_violates_the_contract)
{
    tb_allocator empty = {0};
    unsigned char storage[16] = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(allocate_without_a_function, &empty, "an allocator needs a function");
    TB_TEST_EXPECT_TRAP(resolve_without_a_function, &empty, "an allocator needs a function");
    TB_TEST_EXPECT_TRAP(free_an_array_that_cannot_exist, storage, "count and size must be those");
    TB_TEST_EXPECT_TRAP(make_an_allocator_of_no_arena, NULL, "arena != NULL");
}
