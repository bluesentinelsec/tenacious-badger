/* An arena replaces most individual frees in the engine, so a defect in it
 * corrupts whatever shares it. These cases pin its promises: every object is
 * usable, aligned, and separate from every other until it is released;
 * releasing keeps the memory and disturbs nothing else; a size that cannot be
 * served is refused, never wrapped; and running out of memory at any point
 * leaves the arena intact. What AddressSanitizer reports about released
 * memory is checked by arena_probe.c in a child process. */
#include "badger/arena.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "engine_test.h"
#include "tb_test.h"

#include <stdint.h>
#include <string.h>

/* The size of the engine's memory blocks is the arena's chunk size, so small
 * chunks make a few hundred bytes of objects cross many chunk boundaries. */
#define SMALL_CHUNK_SIZE ((size_t)1024)

static const size_t chunk_sizes[] = {
    TB_ARENA_MINIMUM_SIZE,      TB_ARENA_MINIMUM_SIZE + 1, 1000, SMALL_CHUNK_SIZE, 4096,
    TB_ARENA_DEFAULT_CHUNK_SIZE};

/* Writes a pattern that depends on the seed and on each byte's position, so an
 * object that was moved, truncated, or overlapped by another is detected. */
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

/* Number of bytes in a range that do not hold `value`. */
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

/* Whether a range of `size` bytes lies inside a buffer. */
static bool is_inside(const void *memory, size_t size, const void *buffer, size_t buffer_size)
{
    const uintptr_t begin = (uintptr_t)memory;
    const uintptr_t buffer_begin = (uintptr_t)buffer;

    return begin >= buffer_begin && size <= buffer_size &&
           begin - buffer_begin <= buffer_size - size;
}

/* --- a replacement allocator that records what the arena asks for ------------- */

/* Requests above this are refused without being passed on. The cases that use
 * it ask for sizes no machine has, which the C library may not survive. */
#define RECORDING_REFUSAL_LIMIT ((size_t)1 << 30)

typedef struct recording_allocator
{
    tb_memory_functions original;
    size_t malloc_calls;
    size_t last_malloc_size;
} recording_allocator;

static recording_allocator recording = {0};

static void *recording_malloc_function(size_t size)
{
    recording.malloc_calls++;
    recording.last_malloc_size = size;
    if (size > RECORDING_REFUSAL_LIMIT)
    {
        return NULL;
    }
    return recording.original.malloc_function(size);
}

static void *recording_calloc_function(size_t count, size_t size)
{
    return recording.original.calloc_function(count, size);
}

static void *recording_realloc_function(void *memory, size_t size)
{
    return recording.original.realloc_function(memory, size);
}

static void recording_free_function(void *memory)
{
    recording.original.free_function(memory);
}

/* Starts recording with nothing recorded. Nothing may be allocated when this
 * is called. Put the shared functions back with
 * tb_engine_test_install_memory_functions() before any check can end the case. */
static void install_recording_allocator(void)
{
    static const tb_memory_functions functions = {.malloc_function = recording_malloc_function,
                                                  .calloc_function = recording_calloc_function,
                                                  .realloc_function = recording_realloc_function,
                                                  .free_function = recording_free_function};

    memset(&recording, 0, sizeof(recording));
    tb_memory_get_original_functions(&recording.original);
    TB_TEST_REQUIRE(tb_memory_set_functions(&functions));
}

/* --- an application's own allocator, which owes nothing to the C library ------ */

enum
{
    POOL_SIZE = 256 * 1024,
    POOL_ALIGNMENT = 16
};

/* Hands out pieces of one static buffer and never reuses them. Any address
 * outside the buffer therefore did not come from this allocator. */
typedef struct pool_allocator
{
    size_t used;
    size_t malloc_calls;
    size_t calloc_calls;
    size_t realloc_calls;
    size_t free_calls;
} pool_allocator;

static _Alignas(POOL_ALIGNMENT) unsigned char pool_memory[POOL_SIZE];
static pool_allocator pool = {0};

static void *pool_malloc_function(size_t size)
{
    const size_t rounded_size = (size + POOL_ALIGNMENT - 1) / POOL_ALIGNMENT * POOL_ALIGNMENT;
    void *memory = NULL;

    pool.malloc_calls++;
    if (rounded_size > POOL_SIZE - pool.used)
    {
        return NULL;
    }
    memory = pool_memory + pool.used;
    pool.used += rounded_size;
    return memory;
}

/* An arena takes whole chunks and never resizes them, so it has no use for
 * these two. They count the calls and refuse. */
static void *pool_calloc_function(size_t count, size_t size)
{
    (void)count;
    (void)size;
    pool.calloc_calls++;
    return NULL;
}

static void *pool_realloc_function(void *memory, size_t size)
{
    (void)memory;
    (void)size;
    pool.realloc_calls++;
    return NULL;
}

static void pool_free_function(void *memory)
{
    (void)memory;
    pool.free_calls++;
}

/* Makes the pool the engine's allocator. Nothing may be allocated when this
 * is called. Put the shared functions back with
 * tb_engine_test_install_memory_functions() before any check can end the case. */
static void install_pool_allocator(void)
{
    static const tb_memory_functions functions = {.malloc_function = pool_malloc_function,
                                                  .calloc_function = pool_calloc_function,
                                                  .realloc_function = pool_realloc_function,
                                                  .free_function = pool_free_function};

    memset(&pool, 0, sizeof(pool));
    TB_TEST_REQUIRE(tb_memory_set_functions(&functions));
}

/* --- creating and destroying -------------------------------------------------- */

TB_TEST(core_arena, a_new_arena_is_one_block_that_is_ready_for_objects)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(chunk_sizes); index++)
    {
        const size_t chunk_size = chunk_sizes[index];
        const size_t blocks_before = tb_memory_allocation_count();
        tb_arena *const arena = tb_arena_create(chunk_size);

        TB_TEST_CONTEXT("chunk size %zu", chunk_size);
        TB_TEST_REQUIRE_POINTER(arena, !=, NULL);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before + 1);
        /* The arena's own bookkeeping takes a small part of the first chunk. */
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), <, chunk_size);
        TB_TEST_EXPECT_UINT(chunk_size - tb_arena_capacity(arena), <, TB_ARENA_MINIMUM_SIZE);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, tb_arena_capacity(arena));
        tb_arena_destroy(arena);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
    }
}

TB_TEST(core_arena, chunk_sizes_are_requested_as_documented)
{
    typedef struct chunk_size_row
    {
        size_t requested;
        size_t used;
    } chunk_size_row;
    static const chunk_size_row rows[] = {
        {0, TB_ARENA_DEFAULT_CHUNK_SIZE},
        {1, TB_ARENA_MINIMUM_SIZE},
        {TB_ARENA_MINIMUM_SIZE - 1, TB_ARENA_MINIMUM_SIZE},
        {TB_ARENA_MINIMUM_SIZE, TB_ARENA_MINIMUM_SIZE},
        {TB_ARENA_MINIMUM_SIZE + 1, TB_ARENA_MINIMUM_SIZE + 1},
        {1000, 1000},
        {TB_ARENA_DEFAULT_CHUNK_SIZE, TB_ARENA_DEFAULT_CHUNK_SIZE},
    };
    size_t first_chunk_sizes[TB_TEST_COUNT_OF(rows)] = {0};
    size_t later_chunk_sizes[TB_TEST_COUNT_OF(rows)] = {0};

    install_recording_allocator();
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        tb_arena *const arena = tb_arena_create(rows[index].requested);

        first_chunk_sizes[index] = recording.last_malloc_size;
        /* One byte more than the first chunk has left needs a second chunk. */
        (void)tb_arena_alloc(arena, tb_arena_remaining(arena) + 1, 1);
        later_chunk_sizes[index] = recording.last_malloc_size;
        tb_arena_destroy(arena);
    }
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT_UINT(recording.malloc_calls, ==, 2 * TB_TEST_COUNT_OF(rows));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu: requested %zu", index, rows[index].requested);
        TB_TEST_EXPECT_UINT(first_chunk_sizes[index], ==, rows[index].used);
        TB_TEST_EXPECT_UINT(later_chunk_sizes[index], ==, rows[index].used);
    }
}

TB_TEST(core_arena, destroying_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_arena_destroy(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

TB_TEST(core_arena, destroying_releases_every_chunk)
{
    const size_t blocks_before = tb_memory_allocation_count();
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);

    for (size_t index = 0; index < 100; index++)
    {
        fill_pattern(tb_arena_alloc(arena, 300 + index * 50, 8), 300 + index * 50, index);
    }
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), >, blocks_before + 30);
    tb_arena_destroy(arena);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- objects ------------------------------------------------------------------ */

TB_TEST(core_arena, many_small_objects_stay_separate_and_keep_their_contents)
{
    enum
    {
        OBJECT_COUNT = 10000,
        SIZE_CYCLE = 67
    };
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    unsigned char **const objects = TB_NEW_ARRAY(unsigned char *, OBJECT_COUNT);

    for (size_t index = 0; index < OBJECT_COUNT; index++)
    {
        const size_t size = index % SIZE_CYCLE + 1;

        objects[index] = tb_arena_alloc(arena, size, 1);
        fill_pattern(objects[index], size, index);
    }
    /* Every object still holds its own pattern, so no two of them overlap. */
    for (size_t index = 0; index < OBJECT_COUNT; index++)
    {
        const size_t size = index % SIZE_CYCLE + 1;

        TB_TEST_CONTEXT("object %zu of %zu bytes", index, size);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(objects[index], size, index), ==, size);
    }
    tb_free(objects);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, a_chunk_is_filled_to_its_last_byte_before_another_is_added)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(chunk_sizes); index++)
    {
        tb_arena *const arena = tb_arena_create(chunk_sizes[index]);
        const size_t blocks = tb_memory_allocation_count();
        const size_t first_room = tb_arena_remaining(arena);
        unsigned char *all_but_one = NULL;
        unsigned char *last_byte = NULL;
        size_t later_room = {0};

        TB_TEST_CONTEXT("chunk size %zu", chunk_sizes[index]);
        all_but_one = tb_arena_alloc(arena, first_room - 1, 1);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 1);
        last_byte = tb_arena_alloc(arena, 1, 1);
        TB_TEST_EXPECT_POINTER(last_byte, ==, all_but_one + first_room - 1);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);

        /* The first chunk is full: one more byte needs a second chunk. */
        (void)tb_arena_alloc(arena, 1, 1);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks + 1);
        later_room = tb_arena_remaining(arena) + 1;
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, first_room + later_room);

        /* The second chunk is filled exactly as well. */
        (void)tb_arena_alloc(arena, later_room - 1, 1);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks + 1);
        (void)tb_arena_alloc(arena, 1, 1);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks + 2);
        tb_arena_destroy(arena);
    }
}

TB_TEST(core_arena, an_object_one_byte_too_large_for_what_is_left_moves_to_another_chunk)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(chunk_sizes); index++)
    {
        tb_arena *const arena = tb_arena_create(chunk_sizes[index]);
        const size_t blocks = tb_memory_allocation_count();
        const size_t first_room = tb_arena_remaining(arena);
        unsigned char *exact = NULL;
        unsigned char *moved = NULL;

        TB_TEST_CONTEXT("chunk size %zu", chunk_sizes[index]);
        /* Half of the chunk is in use; the other half is the room left. */
        exact = tb_arena_alloc(arena, first_room / 2, 1);
        fill_pattern(exact, first_room / 2, 1);
        moved = tb_arena_alloc(arena, tb_arena_remaining(arena) + 1, 1);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks + 1);
        fill_pattern(moved, first_room - first_room / 2 + 1, 2);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(exact, first_room / 2, 1), ==, first_room / 2);
        tb_arena_destroy(arena);
    }
}

TB_TEST(core_arena, objects_larger_than_a_chunk_get_a_chunk_of_their_own)
{
    static const size_t sizes[] = {SMALL_CHUNK_SIZE - 1,  SMALL_CHUNK_SIZE,
                                   SMALL_CHUNK_SIZE + 1,  2 * SMALL_CHUNK_SIZE,
                                   10 * SMALL_CHUNK_SIZE, (size_t)1 << 20};
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    unsigned char *objects[TB_TEST_COUNT_OF(sizes)] = {0};
    unsigned char *small = NULL;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        const size_t blocks = tb_memory_allocation_count();
        const size_t capacity = tb_arena_capacity(arena);

        TB_TEST_CONTEXT("size %zu", sizes[index]);
        objects[index] = tb_arena_alloc(arena, sizes[index], 1);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks + 1);
        /* The chunk is exactly as large as the object: nothing is wasted. */
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, capacity + sizes[index]);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
        fill_pattern(objects[index], sizes[index], index);
    }
    /* Ordinary objects are still served afterwards, from an ordinary chunk. */
    small = tb_arena_alloc(arena, 16, 8);
    fill_pattern(small, 16, 99);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), >, 0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        TB_TEST_CONTEXT("size %zu", sizes[index]);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(objects[index], sizes[index], index), ==,
                            sizes[index]);
    }
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(small, 16, 99), ==, 16);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, every_object_has_the_alignment_it_asked_for)
{
    enum
    {
        /* 64 KiB: beyond the page size of every supported platform. */
        LARGEST_ALIGNMENT_SHIFT = 16
    };
    static const size_t sizes[] = {0, 1, 3, 8, 24, 100, 5000};
    unsigned char *objects[LARGEST_ALIGNMENT_SHIFT + 1][TB_TEST_COUNT_OF(sizes)] = {0};
    tb_arena *const arena = tb_arena_create(4096);

    for (size_t shift = 0; shift <= LARGEST_ALIGNMENT_SHIFT; shift++)
    {
        const size_t alignment = (size_t)1 << shift;

        for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
        {
            TB_TEST_CONTEXT("alignment %zu, size %zu", alignment, sizes[index]);
            /* Alternate the two entry points; both must align. */
            if (index % 2 == 0)
            {
                objects[shift][index] = tb_arena_alloc(arena, sizes[index], alignment);
            }
            else
            {
                objects[shift][index] = tb_arena_calloc(arena, sizes[index], 1, alignment);
            }
            TB_TEST_EXPECT(is_aligned(objects[shift][index], alignment));
            fill_pattern(objects[shift][index], sizes[index], shift * 100 + index);
            /* An odd position, so the next object needs padding to align. */
            (void)tb_arena_alloc(arena, 1, 1);
        }
    }
    for (size_t shift = 0; shift <= LARGEST_ALIGNMENT_SHIFT; shift++)
    {
        for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
        {
            TB_TEST_CONTEXT("alignment %zu, size %zu", (size_t)1 << shift, sizes[index]);
            TB_TEST_EXPECT_UINT(
                find_pattern_mismatch(objects[shift][index], sizes[index], shift * 100 + index), ==,
                sizes[index]);
        }
    }
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, empty_requests_yield_unique_objects)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    /* A zero size is served as one byte, so each of these can be told apart
     * from the others and written to. */
    unsigned char *const objects[] = {
        tb_arena_alloc(arena, 0, 1),         tb_arena_alloc(arena, 0, 16),
        tb_arena_calloc(arena, 0, 0, 1),     tb_arena_calloc(arena, 0, 8, 4),
        tb_arena_calloc(arena, 8, 0, 4),     tb_arena_try_alloc(arena, 0, 1),
        tb_arena_try_calloc(arena, 0, 0, 1), tb_arena_try_calloc(arena, 0, 3, 1),
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(objects); index++)
    {
        TB_TEST_CONTEXT("object %zu", index);
        TB_TEST_REQUIRE_POINTER(objects[index], !=, NULL);
        for (size_t other = 0; other < index; other++)
        {
            TB_TEST_EXPECT_POINTER(objects[index], !=, objects[other]);
        }
        objects[index][0] = (unsigned char)index;
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(objects); index++)
    {
        TB_TEST_EXPECT_UINT(objects[index][0], ==, index);
    }
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, calloc_returns_zero_filled_arrays_from_memory_that_was_used_before)
{
    static const size_t counts[] = {0, 1, 3, 16, 100};
    static const size_t sizes[] = {0, 1, 3, 8, 24};
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);

    for (size_t count_index = 0; count_index < TB_TEST_COUNT_OF(counts); count_index++)
    {
        for (size_t size_index = 0; size_index < TB_TEST_COUNT_OF(sizes); size_index++)
        {
            const size_t total = counts[count_index] * sizes[size_index];
            const size_t dirty_size = tb_arena_remaining(arena);
            unsigned char *array = NULL;

            TB_TEST_CONTEXT("%zu x %zu bytes", counts[count_index], sizes[size_index]);
            /* Leave every byte of the chunk non-zero, then hand it out again. */
            memset(tb_arena_alloc(arena, dirty_size, 1), 0xFF, dirty_size);
            tb_arena_reset(arena);
            array = tb_arena_calloc(arena, counts[count_index], sizes[size_index], 8);
            TB_TEST_EXPECT(is_aligned(array, 8));
            TB_TEST_EXPECT_UINT(count_bytes_other_than(array, total, 0), ==, 0);
            tb_arena_reset(arena);
        }
    }
    tb_arena_destroy(arena);
}

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

TB_TEST(core_arena, typed_forms_return_zeroed_objects_aligned_for_their_type)
{
    enum
    {
        PARTICLE_COUNT = 100
    };
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    /* An odd position first, so that alignment is the arena's doing. */
    unsigned char *const odd = tb_arena_alloc(arena, 1, 1);
    particle *const one = TB_ARENA_NEW(arena, particle);
    particle *const many = TB_ARENA_NEW_ARRAY(arena, particle, PARTICLE_COUNT);
    wide_vector *const vector = TB_ARENA_NEW(arena, wide_vector);
    wide_vector *const vectors = TB_ARENA_NEW_ARRAY(arena, wide_vector, 3);

    *odd = 1;
    TB_TEST_EXPECT(is_aligned(one, _Alignof(particle)));
    TB_TEST_EXPECT(is_aligned(many, _Alignof(particle)));
    TB_TEST_EXPECT(is_aligned(vector, 64));
    TB_TEST_EXPECT(is_aligned(vectors, 64));
    TB_TEST_EXPECT_UINT(count_bytes_other_than(one, sizeof(*one), 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(many, PARTICLE_COUNT * sizeof(*many), 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(vector, sizeof(*vector), 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(vectors, 3 * sizeof(*vectors), 0), ==, 0);

    /* The objects are separate: writing all of one leaves the others zero. */
    fill_pattern(many, PARTICLE_COUNT * sizeof(*many), 7);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(one, sizeof(*one), 0), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(vector, sizeof(*vector), 0), ==, 0);
    tb_arena_destroy(arena);
}

/* --- releasing ---------------------------------------------------------------- */

/* A fixed sequence of requests that crosses several chunks and includes
 * objects larger than a chunk. Records where each object was placed. */
enum
{
    WORKLOAD_OBJECT_COUNT = 60
};

static size_t workload_object_size(size_t index)
{
    static const size_t sizes[] = {24, 100, 7, 400, 1, 3000, 64, 900};

    return sizes[index % TB_TEST_COUNT_OF(sizes)];
}

static void run_workload(tb_arena *arena, unsigned char *objects[WORKLOAD_OBJECT_COUNT])
{
    static const size_t alignments[] = {8, 1, 16, 4, 64};

    for (size_t index = 0; index < WORKLOAD_OBJECT_COUNT; index++)
    {
        const size_t size = workload_object_size(index);

        objects[index] =
            tb_arena_alloc(arena, size, alignments[index % TB_TEST_COUNT_OF(alignments)]);
        fill_pattern(objects[index], size, index);
    }
}

TB_TEST(core_arena, reset_recycles_memory_without_allocating)
{
    enum
    {
        ROUND_COUNT = 10
    };
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    unsigned char *first_round[WORKLOAD_OBJECT_COUNT] = {0};
    unsigned char *this_round[WORKLOAD_OBJECT_COUNT] = {0};
    size_t blocks = {0};
    size_t capacity = {0};

    run_workload(arena, first_round);
    blocks = tb_memory_allocation_count();
    capacity = tb_arena_capacity(arena);
    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        TB_TEST_CONTEXT("round %zu", round);
        tb_arena_reset(arena);
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, capacity);
        run_workload(arena, this_round);
        /* The same requests land in the same memory: nothing new was needed. */
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, capacity);
        TB_TEST_EXPECT_BYTES(this_round, first_round, sizeof(first_round));
    }
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, reset_returns_an_arena_to_its_first_byte)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    const size_t room = tb_arena_remaining(arena);
    unsigned char *const first = tb_arena_alloc(arena, 16, 1);
    unsigned char *objects[WORKLOAD_OBJECT_COUNT] = {0};

    run_workload(arena, objects);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, 16, 1), ==, first);
    /* Resetting an arena that is already empty changes nothing. */
    tb_arena_reset(arena);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, chunks_kept_by_a_reset_are_reused_in_any_order)
{
    enum
    {
        LARGE_SIZE = 5000,
        MEDIUM_SIZE = 900
    };
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    unsigned char *large = NULL;
    unsigned char *medium = NULL;
    size_t blocks = {0};

    /* Three chunks: the first, an ordinary one, and one made for a large
     * object. */
    (void)tb_arena_alloc(arena, tb_arena_remaining(arena), 1);
    (void)tb_arena_alloc(arena, 100, 1);
    (void)tb_arena_alloc(arena, LARGE_SIZE, 1);
    blocks = tb_memory_allocation_count();

    /* After a reset the large object comes first. It skips the ordinary chunk
     * to reuse its own, and the ordinary chunk then serves what follows. */
    tb_arena_reset(arena);
    large = tb_arena_alloc(arena, LARGE_SIZE, 1);
    fill_pattern(large, LARGE_SIZE, 1);
    medium = tb_arena_alloc(arena, MEDIUM_SIZE, 1);
    fill_pattern(medium, MEDIUM_SIZE, 2);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(large, LARGE_SIZE, 1), ==, LARGE_SIZE);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(medium, MEDIUM_SIZE, 2), ==, MEDIUM_SIZE);
    tb_arena_destroy(arena);
}

enum
{
    NESTING_DEPTH = 8,
    OBJECTS_PER_LEVEL = 12
};

/* Sizes that make each nesting level cross chunk boundaries, with one object
 * per level that is larger than a chunk. */
static size_t nested_object_size(size_t level, size_t index)
{
    if (index == OBJECTS_PER_LEVEL / 2)
    {
        return SMALL_CHUNK_SIZE + level;
    }
    return 40 + level * 13 + index * 29;
}

TB_TEST(core_arena, rewind_releases_only_what_was_allocated_after_the_mark)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    const size_t room = tb_arena_remaining(arena);
    tb_arena_mark marks[NESTING_DEPTH] = {0};
    unsigned char *objects[NESTING_DEPTH][OBJECTS_PER_LEVEL] = {0};
    size_t blocks = {0};

    for (size_t level = 0; level < NESTING_DEPTH; level++)
    {
        marks[level] = tb_arena_get_mark(arena);
        for (size_t index = 0; index < OBJECTS_PER_LEVEL; index++)
        {
            const size_t size = nested_object_size(level, index);

            objects[level][index] = tb_arena_alloc(arena, size, 8);
            fill_pattern(objects[level][index], size, level * 100 + index);
        }
    }
    blocks = tb_memory_allocation_count();

    for (size_t remaining_levels = NESTING_DEPTH; remaining_levels > 0; remaining_levels--)
    {
        const size_t level = remaining_levels - 1;
        unsigned char *const first_of_level = objects[level][0];

        tb_arena_rewind(arena, marks[level]);
        /* Everything allocated before the mark is untouched. */
        for (size_t outer = 0; outer < level; outer++)
        {
            for (size_t index = 0; index < OBJECTS_PER_LEVEL; index++)
            {
                const size_t size = nested_object_size(outer, index);

                TB_TEST_CONTEXT("after rewinding level %zu: level %zu, object %zu", level, outer,
                                index);
                TB_TEST_EXPECT_UINT(
                    find_pattern_mismatch(objects[outer][index], size, outer * 100 + index), ==,
                    size);
            }
        }
        /* The released memory is the next to be handed out, and the mark can
         * be rewound to again. */
        TB_TEST_CONTEXT("after rewinding level %zu", level);
        TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, nested_object_size(level, 0), 8), ==,
                               first_of_level);
        tb_arena_rewind(arena, marks[level]);
    }
    /* The first mark was taken in an empty arena. */
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, rewinding_to_the_current_position_changes_nothing)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    unsigned char *const object = tb_arena_alloc(arena, 100, 1);
    size_t room = {0};

    fill_pattern(object, 100, 5);
    room = tb_arena_remaining(arena);
    tb_arena_rewind(arena, tb_arena_get_mark(arena));
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(object, 100, 5), ==, 100);
    tb_arena_destroy(arena);
}

/* --- random operations against a model ---------------------------------------- */

enum
{
    MODEL_OBJECT_LIMIT = 256,
    MODEL_MARK_LIMIT = 16,
    MODEL_OPERATION_COUNT = 40000,
    MODEL_FIXED_BUFFER_SIZE = 32 * 1024
};

typedef struct model_object
{
    unsigned char *memory;
    size_t size;
    size_t alignment;
    size_t seed;
} model_object;

typedef struct model_mark
{
    tb_arena_mark mark;
    size_t object_count; /* Objects that were live when the mark was taken. */
} model_mark;

/* What the arena must currently hold: every live object with the pattern it
 * was given, and the marks that can still be rewound to. */
typedef struct arena_model
{
    tb_arena *arena;
    bool is_fixed;
    size_t typical_size; /* Sizes are drawn around this. */
    model_object objects[MODEL_OBJECT_LIMIT];
    size_t object_count;
    model_mark marks[MODEL_MARK_LIMIT];
    size_t mark_count;
    size_t next_seed;
    size_t allocation_count;
    size_t rewind_count;
    size_t reset_count;
} arena_model;

static arena_model model = {0};

static void model_verify(void)
{
    for (size_t index = 0; index < model.object_count; index++)
    {
        const model_object *const object = &model.objects[index];

        TB_TEST_EXPECT(is_aligned(object->memory, object->alignment));
        if (find_pattern_mismatch(object->memory, object->size, object->seed) != object->size)
        {
            TB_TEST_FAIL("object %zu of %zu (%zu bytes, alignment %zu) lost its contents at "
                         "byte %zu",
                         index, model.object_count, object->size, object->alignment,
                         find_pattern_mismatch(object->memory, object->size, object->seed));
        }
    }
}

static void model_reset(void)
{
    tb_arena_reset(model.arena);
    model.object_count = 0;
    model.mark_count = 0;
    model.reset_count++;
}

static void model_rewind(void)
{
    const size_t mark_index = (size_t)tb_test_random_below(model.mark_count);

    tb_arena_rewind(model.arena, model.marks[mark_index].mark);
    model.object_count = model.marks[mark_index].object_count;
    /* The mark that was rewound to stays valid; later ones are released. */
    model.mark_count = mark_index + 1;
    model.rewind_count++;
}

static void model_take_mark(void)
{
    model.marks[model.mark_count].mark = tb_arena_get_mark(model.arena);
    model.marks[model.mark_count].object_count = model.object_count;
    model.mark_count++;
}

/* Mostly small objects, some near the size of a chunk, a few larger. */
static size_t model_random_size(void)
{
    switch (tb_test_random_below(10))
    {
    case 0:
        return 0;
    case 1:
        return model.typical_size + (size_t)tb_test_random_below(2 * model.typical_size);
    case 2:
    case 3:
        return (size_t)tb_test_random_below(model.typical_size);
    default:
        return (size_t)tb_test_random_below(64) + 1;
    }
}

/* Mostly the alignments of ordinary types, sometimes up to a page. */
static size_t model_random_alignment(void)
{
    if (tb_test_random_below(10) == 0)
    {
        return (size_t)1 << tb_test_random_below(13);
    }
    return (size_t)1 << tb_test_random_below(7);
}

static void model_allocate(void)
{
    model_object *const object = &model.objects[model.object_count];
    const bool wants_zeroes = tb_test_random_below(4) == 0;

    object->size = model_random_size();
    object->alignment = model_random_alignment();
    object->seed = model.next_seed;
    model.next_seed++;
    if (wants_zeroes)
    {
        object->memory = tb_arena_try_calloc(model.arena, object->size, 1, object->alignment);
    }
    else
    {
        object->memory = tb_arena_try_alloc(model.arena, object->size, object->alignment);
    }
    if (object->memory == NULL)
    {
        /* Only a fixed arena can be full. Its objects must have survived the
         * refusal; make room and carry on. */
        TB_TEST_REQUIRE(model.is_fixed);
        model_verify();
        model_reset();
        return;
    }
    if (wants_zeroes)
    {
        TB_TEST_EXPECT_UINT(count_bytes_other_than(object->memory, object->size, 0), ==, 0);
    }
    fill_pattern(object->memory, object->size, object->seed);
    model.object_count++;
    model.allocation_count++;
}

/* Runs random operations on an arena and checks after each release, and at the
 * end, that every object the model says is live still has its contents. */
static void run_model(tb_arena *arena, bool is_fixed, size_t typical_size)
{
    const size_t blocks_before = tb_memory_allocation_count();
    size_t capacity = tb_arena_capacity(arena);

    memset(&model, 0, sizeof(model));
    model.arena = arena;
    model.is_fixed = is_fixed;
    model.typical_size = typical_size;
    for (size_t operation = 0; operation < MODEL_OPERATION_COUNT; operation++)
    {
        const uint64_t choice = tb_test_random_below(100);

        TB_TEST_CONTEXT("operation %zu, with %zu objects and %zu marks", operation,
                        model.object_count, model.mark_count);
        if (model.object_count == MODEL_OBJECT_LIMIT)
        {
            model_verify();
            model_reset();
        }
        else if (choice < 3)
        {
            model_verify();
            model_reset();
        }
        else if (choice < 13 && model.mark_count > 0)
        {
            model_rewind();
            model_verify();
        }
        else if (choice < 23 && model.mark_count < MODEL_MARK_LIMIT)
        {
            model_take_mark();
        }
        else
        {
            model_allocate();
        }
        /* An arena never gives memory back, and never claims more room than
         * it holds. */
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), >=, capacity);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), <=, tb_arena_capacity(arena));
        capacity = tb_arena_capacity(arena);
    }
    model_verify();
    if (is_fixed)
    {
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
    }
    /* The operations are only a fair test if each kind happened often. */
    TB_TEST_CONTEXT("%zu allocations, %zu rewinds, %zu resets", model.allocation_count,
                    model.rewind_count, model.reset_count);
    TB_TEST_EXPECT_UINT(model.allocation_count, >, MODEL_OPERATION_COUNT / 2);
    TB_TEST_EXPECT_UINT(model.rewind_count, >, MODEL_OPERATION_COUNT / 50);
    TB_TEST_EXPECT_UINT(model.reset_count, >, MODEL_OPERATION_COUNT / 100);
}

TB_TEST(core_arena, random_operations_keep_every_object_intact_with_tiny_chunks)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_MINIMUM_SIZE);

    run_model(arena, false, TB_ARENA_MINIMUM_SIZE);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, random_operations_keep_every_object_intact_with_small_chunks)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);

    run_model(arena, false, SMALL_CHUNK_SIZE);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, random_operations_keep_every_object_intact_with_default_chunks)
{
    tb_arena *const arena = tb_arena_create(TB_ARENA_DEFAULT_CHUNK_SIZE);

    run_model(arena, false, SMALL_CHUNK_SIZE);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, random_operations_keep_every_object_intact_in_a_fixed_arena)
{
    static unsigned char buffer[MODEL_FIXED_BUFFER_SIZE];
    tb_arena *const arena = tb_arena_create_fixed(buffer, sizeof(buffer));

    run_model(arena, true, SMALL_CHUNK_SIZE);
    tb_arena_destroy(arena);
}

/* --- fixed arenas ------------------------------------------------------------- */

enum
{
    FIXED_BUFFER_SIZE = 4096,
    FIXED_GUARD_SIZE = 16,
    FIXED_OBJECT_SIZE = 100,
    FIXED_OBJECT_LIMIT = FIXED_BUFFER_SIZE / FIXED_OBJECT_SIZE
};

TB_TEST(core_arena, a_fixed_arena_serves_objects_from_its_buffer_and_never_allocates)
{
    static unsigned char buffer[FIXED_GUARD_SIZE + FIXED_BUFFER_SIZE + FIXED_GUARD_SIZE];

    /* The buffer may start at any address, so every misalignment is tried.
     * Guard bytes on both sides show that the arena stays inside it. */
    for (size_t offset = 0; offset < FIXED_GUARD_SIZE; offset++)
    {
        unsigned char *const start = buffer + FIXED_GUARD_SIZE + offset;
        const size_t size = FIXED_BUFFER_SIZE - offset;
        const size_t blocks = tb_memory_allocation_count();
        unsigned char *objects[FIXED_OBJECT_LIMIT] = {0};
        size_t object_count = {0};
        tb_arena *arena = NULL;
        size_t capacity = {0};

        TB_TEST_CONTEXT("buffer offset %zu", offset);
        memset(buffer, 0xA5, sizeof(buffer));
        arena = tb_arena_create_fixed(start, size);
        TB_TEST_REQUIRE(is_inside(arena, 1, start, size));
        /* The arena keeps pointers in its own structure, which the buffer's
         * address says nothing about. */
        TB_TEST_EXPECT(is_aligned(arena, _Alignof(void *)));
        capacity = tb_arena_capacity(arena);
        TB_TEST_EXPECT_UINT(capacity, <, size);
        TB_TEST_EXPECT_UINT(size - capacity, <, TB_ARENA_MINIMUM_SIZE);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, capacity);

        while (object_count < FIXED_OBJECT_LIMIT)
        {
            unsigned char *const object = tb_arena_try_alloc(arena, FIXED_OBJECT_SIZE, 8);

            if (object == NULL)
            {
                break;
            }
            TB_TEST_EXPECT(is_inside(object, FIXED_OBJECT_SIZE, start, size));
            TB_TEST_EXPECT(is_aligned(object, 8));
            fill_pattern(object, FIXED_OBJECT_SIZE, object_count);
            objects[object_count] = object;
            object_count++;
        }
        /* Nearly all of the buffer was usable. */
        TB_TEST_EXPECT_UINT(object_count, >=,
                            (capacity - FIXED_OBJECT_SIZE) / (FIXED_OBJECT_SIZE + 8));
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), <, FIXED_OBJECT_SIZE + 8);

        /* What is left can be taken to the last byte, and then it is full. */
        if (tb_arena_remaining(arena) > 0)
        {
            const size_t left = tb_arena_remaining(arena);
            unsigned char *const last = tb_arena_try_alloc(arena, left, 1);

            TB_TEST_REQUIRE_POINTER(last, !=, NULL);
            TB_TEST_EXPECT(is_inside(last, left, start, size));
            memset(last, 0x11, left);
        }
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
        TB_TEST_EXPECT_POINTER(tb_arena_try_alloc(arena, 1, 1), ==, NULL);
        TB_TEST_EXPECT_POINTER(tb_arena_try_alloc(arena, 0, 1), ==, NULL);
        TB_TEST_EXPECT_POINTER(tb_arena_try_calloc(arena, 1, 1, 1), ==, NULL);
        for (size_t index = 0; index < object_count; index++)
        {
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(objects[index], FIXED_OBJECT_SIZE, index), ==,
                                FIXED_OBJECT_SIZE);
        }

        /* A reset makes all of it available again. */
        tb_arena_reset(arena);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, capacity);
        TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, FIXED_OBJECT_SIZE, 8), ==, objects[0]);
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, capacity);

        tb_arena_destroy(arena);
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(buffer, FIXED_GUARD_SIZE + offset, 0xA5), ==, 0);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(buffer + FIXED_GUARD_SIZE + FIXED_BUFFER_SIZE,
                                                   FIXED_GUARD_SIZE, 0xA5),
                            ==, 0);
    }
}

TB_TEST(core_arena, a_fixed_arena_of_the_minimum_size_has_room_for_objects)
{
    static unsigned char buffer[TB_ARENA_MINIMUM_SIZE + FIXED_GUARD_SIZE];

    for (size_t offset = 0; offset < FIXED_GUARD_SIZE; offset++)
    {
        tb_arena *const arena = tb_arena_create_fixed(buffer + offset, TB_ARENA_MINIMUM_SIZE);
        const size_t capacity = tb_arena_capacity(arena);
        unsigned char *object = NULL;

        TB_TEST_CONTEXT("buffer offset %zu", offset);
        /* At least half of the smallest buffer is left for objects. */
        TB_TEST_EXPECT_UINT(capacity, >=, TB_ARENA_MINIMUM_SIZE / 2);
        object = tb_arena_alloc(arena, capacity, 1);
        TB_TEST_EXPECT(is_inside(object, capacity, buffer + offset, TB_ARENA_MINIMUM_SIZE));
        fill_pattern(object, capacity, offset);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(object, capacity, offset), ==, capacity);
        tb_arena_destroy(arena);
    }
}

TB_TEST(core_arena, a_buffer_can_be_used_normally_after_its_arena_is_destroyed)
{
    static unsigned char buffer[FIXED_BUFFER_SIZE];
    tb_arena *arena = tb_arena_create_fixed(buffer, sizeof(buffer));

    (void)tb_arena_alloc(arena, 1000, 8);
    tb_arena_destroy(arena);
    /* Every byte is the caller's again, including what the arena never
     * handed out and what held its bookkeeping. */
    fill_pattern(buffer, sizeof(buffer), 3);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(buffer, sizeof(buffer), 3), ==, sizeof(buffer));

    /* And it can hold another arena. */
    arena = tb_arena_create_fixed(buffer, sizeof(buffer));
    fill_pattern(tb_arena_alloc(arena, 1000, 8), 1000, 4);
    tb_arena_destroy(arena);
}

/* --- sizes that cannot be served ---------------------------------------------- */

typedef struct request_row
{
    size_t count;
    size_t size;
    size_t alignment;
} request_row;

/* Requests whose size in bytes, once a chunk's bookkeeping and alignment
 * padding are added, does not fit in size_t. */
static const request_row unrepresentable_requests[] = {
    {1, SIZE_MAX, 1},
    {1, SIZE_MAX, 8},
    {1, SIZE_MAX - 1, 1},
    {1, SIZE_MAX - 7, 1},
    {1, SIZE_MAX - 4096, 8192},
    {1, SIZE_MAX / 2 + 1, (SIZE_MAX / 2) + 1},
    {SIZE_MAX, 2, 1},
    {2, SIZE_MAX, 1},
    {SIZE_MAX / 2 + 1, 2, 8},
    {SIZE_MAX, SIZE_MAX, 16},
    {(size_t)1 << (sizeof(size_t) * 4), (size_t)1 << (sizeof(size_t) * 4), 1},
};

TB_TEST(core_arena, sizes_that_cannot_be_expressed_are_refused_before_allocating)
{
    static unsigned char buffer[FIXED_BUFFER_SIZE];
    tb_arena *arenas[2] = {0};
    size_t refused_count = {0};
    size_t unchanged_count = {0};
    size_t malloc_calls_before = {0};

    /* Adding a chunk's bookkeeping to these sizes would wrap around to a
     * small request, which an allocator would satisfy with a block that is
     * far too short. The recording allocator proves it is never asked. */
    install_recording_allocator();
    arenas[0] = tb_arena_create(SMALL_CHUNK_SIZE);
    arenas[1] = tb_arena_create_fixed(buffer, sizeof(buffer));
    malloc_calls_before = recording.malloc_calls;
    for (size_t arena_index = 0; arena_index < TB_TEST_COUNT_OF(arenas); arena_index++)
    {
        tb_arena *const arena = arenas[arena_index];
        const size_t capacity = tb_arena_capacity(arena);
        const size_t room = tb_arena_remaining(arena);

        for (size_t index = 0; index < TB_TEST_COUNT_OF(unrepresentable_requests); index++)
        {
            const request_row row = unrepresentable_requests[index];
            void *memory = NULL;

            if (row.count == 1)
            {
                memory = tb_arena_try_alloc(arena, row.size, row.alignment);
            }
            else
            {
                memory = tb_arena_try_calloc(arena, row.count, row.size, row.alignment);
            }
            if (memory == NULL)
            {
                refused_count++;
            }
            if (tb_arena_capacity(arena) == capacity && tb_arena_remaining(arena) == room)
            {
                unchanged_count++;
            }
        }
    }
    tb_arena_destroy(arenas[0]);
    tb_arena_destroy(arenas[1]);
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT_UINT(refused_count, ==, 2 * TB_TEST_COUNT_OF(unrepresentable_requests));
    TB_TEST_EXPECT_UINT(unchanged_count, ==, 2 * TB_TEST_COUNT_OF(unrepresentable_requests));
    TB_TEST_EXPECT_UINT(recording.malloc_calls, ==, malloc_calls_before);
}

TB_TEST(core_arena, the_largest_expressible_size_reaches_the_allocator_intact)
{
    tb_arena *arena = NULL;
    size_t room = {0};
    size_t bookkeeping = {0};
    void *memory = NULL;
    size_t largest_request = {0};
    size_t malloc_calls = {0};

    install_recording_allocator();
    arena = tb_arena_create(SMALL_CHUNK_SIZE);
    /* An object one byte larger than a chunk holds gets a chunk of exactly
     * its size plus the chunk's bookkeeping, which this measures. */
    room = tb_arena_remaining(arena);
    (void)tb_arena_alloc(arena, SMALL_CHUNK_SIZE, 1);
    bookkeeping = recording.last_malloc_size - SMALL_CHUNK_SIZE;
    malloc_calls = recording.malloc_calls;

    /* The largest object whose chunk can still be expressed. The allocator is
     * asked for all of it, and its refusal is passed on. */
    memory = tb_arena_try_alloc(arena, SIZE_MAX - bookkeeping, 1);
    largest_request = recording.last_malloc_size;
    tb_arena_destroy(arena);
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT_UINT(room, <, SMALL_CHUNK_SIZE);
    TB_TEST_EXPECT_UINT(bookkeeping, <, TB_ARENA_MINIMUM_SIZE);
    TB_TEST_EXPECT_POINTER(memory, ==, NULL);
    TB_TEST_EXPECT_UINT(recording.malloc_calls, ==, malloc_calls + 1);
    TB_TEST_EXPECT_UINT(largest_request, ==, SIZE_MAX);
}

/* --- an application's allocator ----------------------------------------------- */

TB_TEST(core_arena, an_application_allocator_supplies_all_of_an_arenas_memory)
{
    static unsigned char buffer[FIXED_BUFFER_SIZE];
    unsigned char *objects[WORKLOAD_OBJECT_COUNT] = {0};
    tb_arena *arena = NULL;
    tb_arena *fixed = NULL;
    bool is_arena_in_pool = false;
    size_t objects_outside_pool = {0};
    size_t damaged_objects = {0};
    size_t chunks = {0};
    size_t chunks_after_reuse = {0};
    size_t chunks_after_fixed_arena = {0};
    size_t frees_before_destroy = {0};

    /* An application that installs its own allocator expects the engine to
     * take nothing from anywhere else. The pool is one static buffer, so an
     * address outside it would have come from the C library. */
    install_pool_allocator();
    arena = tb_arena_create(SMALL_CHUNK_SIZE);
    is_arena_in_pool = is_inside(arena, 1, pool_memory, sizeof(pool_memory));
    run_workload(arena, objects);
    objects[0] = tb_arena_alloc(arena, 24, 4096);
    for (size_t index = 0; index < WORKLOAD_OBJECT_COUNT; index++)
    {
        if (!is_inside(objects[index], 1, pool_memory, sizeof(pool_memory)))
        {
            objects_outside_pool++;
        }
    }
    chunks = pool.malloc_calls;

    /* Memory kept across a reset is reused without asking the allocator. */
    tb_arena_reset(arena);
    run_workload(arena, objects);
    for (size_t index = 0; index < WORKLOAD_OBJECT_COUNT; index++)
    {
        const size_t size = workload_object_size(index);

        if (find_pattern_mismatch(objects[index], size, index) != size)
        {
            damaged_objects++;
        }
    }
    chunks_after_reuse = pool.malloc_calls;

    /* An arena in a caller's buffer asks the allocator for nothing. */
    fixed = tb_arena_create_fixed(buffer, sizeof(buffer));
    (void)tb_arena_alloc(fixed, 1000, 8);
    tb_arena_destroy(fixed);
    chunks_after_fixed_arena = pool.malloc_calls;

    /* Every chunk goes back to the allocator it came from. */
    frees_before_destroy = pool.free_calls;
    tb_arena_destroy(arena);
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT(is_arena_in_pool);
    TB_TEST_EXPECT_UINT(objects_outside_pool, ==, 0);
    TB_TEST_EXPECT_UINT(damaged_objects, ==, 0);
    TB_TEST_EXPECT_UINT(chunks, >, 10);
    TB_TEST_EXPECT_UINT(chunks_after_reuse, ==, chunks);
    TB_TEST_EXPECT_UINT(chunks_after_fixed_arena, ==, chunks);
    TB_TEST_EXPECT_UINT(frees_before_destroy, ==, 0);
    TB_TEST_EXPECT_UINT(pool.free_calls, ==, chunks);
    TB_TEST_EXPECT_UINT(pool.calloc_calls, ==, 0);
    TB_TEST_EXPECT_UINT(pool.realloc_calls, ==, 0);
}

/* --- running out of memory ---------------------------------------------------- */

enum
{
    EXHAUSTION_OBJECT_COUNT = 24,
    EXHAUSTION_STEP_LIMIT = 64
};

/* Allocates objects that need a new chunk every step or two, some of them
 * larger than a chunk, with the functions that report failure. Returns false
 * at the first refusal, after checking that it damaged nothing. */
static bool allocate_until_refused(tb_arena *arena)
{
    static const size_t sizes[] = {100, 300, 700, 50, 2000, 1};
    unsigned char *objects[EXHAUSTION_OBJECT_COUNT] = {0};

    for (size_t index = 0; index < EXHAUSTION_OBJECT_COUNT; index++)
    {
        const size_t size = sizes[index % TB_TEST_COUNT_OF(sizes)];
        const size_t capacity = tb_arena_capacity(arena);
        const size_t room = tb_arena_remaining(arena);

        if (index % 2 == 0)
        {
            objects[index] = tb_arena_try_alloc(arena, size, 16);
        }
        else
        {
            objects[index] = tb_arena_try_calloc(arena, size, 1, 16);
        }
        if (objects[index] != NULL)
        {
            fill_pattern(objects[index], size, index);
            continue;
        }
        /* Refused: the arena is exactly as it was, and what it still has room
         * for can be allocated without asking for memory. */
        TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, capacity);
        TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
        for (size_t earlier = 0; earlier < index; earlier++)
        {
            const size_t earlier_size = sizes[earlier % TB_TEST_COUNT_OF(sizes)];

            TB_TEST_EXPECT_UINT(find_pattern_mismatch(objects[earlier], earlier_size, earlier), ==,
                                earlier_size);
        }
        if (room > 0)
        {
            TB_TEST_EXPECT_POINTER(tb_arena_try_alloc(arena, room, 1), !=, NULL);
        }
        return false;
    }
    return true;
}

TB_TEST(core_arena, exhaustion_at_any_step_is_reported_and_leaves_the_arena_usable)
{
    bool completed = false;
    size_t refusal_count = {0};

    /* Each round lets one more allocation succeed than the round before, so
     * every allocation the arena makes fails once. The leak check that runs
     * after the case proves that each early exit released everything. */
    for (size_t successes = 0; successes < EXHAUSTION_STEP_LIMIT && !completed; successes++)
    {
        tb_arena *arena = NULL;

        TB_TEST_CONTEXT("failing after %zu allocations", successes);
        tb_engine_test_fail_allocations_after(successes);
        arena = tb_arena_try_create(TB_ARENA_MINIMUM_SIZE * 2);
        if (arena == NULL)
        {
            TB_TEST_EXPECT_UINT(successes, ==, 0);
            refusal_count++;
            continue;
        }
        completed = allocate_until_refused(arena);
        if (!completed)
        {
            refusal_count++;
        }
        /* With memory available again the same arena carries on. */
        tb_engine_test_stop_failing_allocations();
        fill_pattern(tb_arena_alloc(arena, 3000, 64), 3000, successes);
        tb_arena_destroy(arena);
    }
    TB_TEST_EXPECT(completed);
    /* More than half of the objects needed a chunk of their own. */
    TB_TEST_EXPECT_UINT(refusal_count, >, EXHAUSTION_OBJECT_COUNT / 2);
}

static void allocate_64_bytes(void *context)
{
    (void)tb_arena_alloc(context, 64, 8);
}

static void allocate_four_zeroed_elements(void *context)
{
    (void)tb_arena_calloc(context, 4, 16, 8);
}

static void allocate_an_overflowing_array(void *context)
{
    (void)tb_arena_calloc(context, SIZE_MAX, 2, 1);
}

static void allocate_more_than_a_chunk(void *context)
{
    (void)tb_arena_alloc(context, 5000, 32);
}

static void create_an_arena(void *context)
{
    (void)context;
    (void)tb_arena_create(SMALL_CHUNK_SIZE);
}

static void create_an_arena_of_the_default_size(void *context)
{
    (void)context;
    (void)tb_arena_create(0);
}

TB_TEST(core_arena, a_full_fixed_arena_ends_a_fail_fast_allocation)
{
    static unsigned char buffer[FIXED_BUFFER_SIZE];
    tb_arena *const arena = tb_arena_create_fixed(buffer, sizeof(buffer));

    (void)tb_arena_alloc(arena, tb_arena_remaining(arena), 1);
    TB_TEST_EXPECT_TRAP(allocate_64_bytes, arena, "out of memory: 1 x 64 bytes, alignment 8");
    TB_TEST_EXPECT_TRAP(allocate_four_zeroed_elements, arena,
                        "out of memory: 4 x 16 bytes, alignment 8");
    /* The failed requests did not disturb the arena. */
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, 0);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, 64, 8), !=, NULL);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, an_exhausted_allocator_ends_a_fail_fast_allocation)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    const size_t capacity = tb_arena_capacity(arena);

    tb_engine_test_fail_allocations_after(0);
    TB_TEST_EXPECT_TRAP(allocate_more_than_a_chunk, arena,
                        "out of memory: 1 x 5000 bytes, alignment 32");
    TB_TEST_EXPECT_TRAP(create_an_arena, NULL, "out of memory: 1 x 1024 bytes, alignment 0");
    TB_TEST_EXPECT_TRAP(create_an_arena_of_the_default_size, NULL,
                        "out of memory: 1 x 65536 bytes, alignment 0");
    /* An object that fits needs nothing from the allocator. */
    TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, 64, 8), !=, NULL);
    tb_engine_test_stop_failing_allocations();

    TB_TEST_EXPECT_UINT(tb_arena_capacity(arena), ==, capacity);
    TB_TEST_EXPECT_POINTER(tb_arena_alloc(arena, 5000, 32), !=, NULL);
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, an_array_that_overflows_ends_a_fail_fast_allocation)
{
    tb_arena *const arena = tb_arena_create(SMALL_CHUNK_SIZE);
    const size_t room = tb_arena_remaining(arena);

    TB_TEST_EXPECT_TRAP(allocate_an_overflowing_array, arena, "out of memory");
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, room);
    tb_arena_destroy(arena);
}

/* --- finding stale pointers --------------------------------------------------- */

TB_TEST(core_arena, objects_are_handed_out_holding_the_fill_byte_in_checked_builds)
{
    static unsigned char buffer[FIXED_BUFFER_SIZE];
    tb_arena *arenas[2] = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions, and the fill follows them");
    }
    arenas[0] = tb_arena_create(SMALL_CHUNK_SIZE);
    arenas[1] = tb_arena_create_fixed(buffer, sizeof(buffer));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(arenas); index++)
    {
        tb_arena *const arena = arenas[index];
        unsigned char *object = NULL;
        tb_arena_mark mark = {0};

        TB_TEST_CONTEXT("arena %zu", index);
        /* New memory. */
        object = tb_arena_alloc(arena, 200, 1);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(object, 200, TB_ARENA_FILL_BYTE), ==, 0);
        memset(object, 0, 200);

        /* Memory released by a rewind. */
        mark = tb_arena_get_mark(arena);
        object = tb_arena_alloc(arena, 300, 8);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(object, 300, TB_ARENA_FILL_BYTE), ==, 0);
        memset(object, 0, 300);
        tb_arena_rewind(arena, mark);
        object = tb_arena_alloc(arena, 300, 8);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(object, 300, TB_ARENA_FILL_BYTE), ==, 0);
        memset(object, 0, 300);

        /* Memory released by a reset. */
        tb_arena_reset(arena);
        object = tb_arena_alloc(arena, 500, 1);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(object, 500, TB_ARENA_FILL_BYTE), ==, 0);
    }
    /* A chunk added later, and one kept across a reset. */
    for (size_t round = 0; round < 2; round++)
    {
        unsigned char *const large = tb_arena_alloc(arenas[0], 5000, 1);

        TB_TEST_CONTEXT("round %zu", round);
        TB_TEST_EXPECT_UINT(count_bytes_other_than(large, 5000, TB_ARENA_FILL_BYTE), ==, 0);
        memset(large, 0, 5000);
        tb_arena_reset(arenas[0]);
    }
    tb_arena_destroy(arenas[0]);
    tb_arena_destroy(arenas[1]);
}

TB_TEST(core_arena, every_chunk_is_refilled_when_a_release_spans_several)
{
    enum
    {
        OBJECT_COUNT = 12,
        OBJECT_SIZE = 600,
        ROUND_COUNT = 3
    };
    tb_arena *arena = NULL;
    tb_arena_mark mark = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions, and the fill follows them");
    }
    /* Each object needs a chunk of its own, so a release crosses many chunks:
     * the one the mark is in, the one being filled, and those in between.
     * Every round overwrites all of them and releases them a different way. */
    arena = tb_arena_create(SMALL_CHUNK_SIZE);
    (void)tb_arena_alloc(arena, 10, 1);
    mark = tb_arena_get_mark(arena);
    for (size_t round = 0; round < ROUND_COUNT; round++)
    {
        for (size_t index = 0; index < OBJECT_COUNT; index++)
        {
            unsigned char *const object = tb_arena_alloc(arena, OBJECT_SIZE, 1);

            TB_TEST_CONTEXT("round %zu, object %zu", round, index);
            TB_TEST_EXPECT_UINT(count_bytes_other_than(object, OBJECT_SIZE, TB_ARENA_FILL_BYTE), ==,
                                0);
            memset(object, 0, OBJECT_SIZE);
        }
        if (round % 2 == 0)
        {
            tb_arena_rewind(arena, mark);
        }
        else
        {
            tb_arena_reset(arena);
            (void)tb_arena_alloc(arena, 10, 1);
            mark = tb_arena_get_mark(arena);
        }
    }
    tb_arena_destroy(arena);
}

TB_TEST(core_arena, a_released_object_reads_as_the_fill_byte_in_checked_builds)
{
#if defined(TB_SANITIZE_ADDRESS)
    TB_TEST_SKIP("AddressSanitizer ends the process at such a read; arena_probe.c checks that");
#else
    tb_arena *arena = NULL;
    const unsigned char *stale_after_rewind = NULL;
    const unsigned char *stale_after_reset = NULL;
    tb_arena_mark mark = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions, and the fill follows them");
    }
    /* The arena still owns the memory, so reading it is defined; the point is
     * that a stale pointer sees a recognizable value, not plausible data. */
    arena = tb_arena_create(SMALL_CHUNK_SIZE);
    stale_after_reset = memset(tb_arena_alloc(arena, 100, 1), 0x42, 100);
    mark = tb_arena_get_mark(arena);
    stale_after_rewind = memset(tb_arena_alloc(arena, 100, 1), 0x42, 100);
    tb_arena_rewind(arena, mark);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(stale_after_rewind, 100, TB_ARENA_FILL_BYTE), ==, 0);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(stale_after_reset, 100, 0x42), ==, 0);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(count_bytes_other_than(stale_after_reset, 100, TB_ARENA_FILL_BYTE), ==, 0);
    tb_arena_destroy(arena);
#endif
}

/* --- contract violations ------------------------------------------------------ */

static void alloc_from_null(void *context)
{
    (void)context;
    (void)tb_arena_alloc(NULL, 8, 8);
}

static void try_alloc_from_null(void *context)
{
    (void)context;
    (void)tb_arena_try_alloc(NULL, 8, 8);
}

static void calloc_from_null(void *context)
{
    (void)context;
    (void)tb_arena_calloc(NULL, 1, 8, 8);
}

static void try_calloc_from_null(void *context)
{
    (void)context;
    (void)tb_arena_try_calloc(NULL, 1, 8, 8);
}

static void reset_null(void *context)
{
    (void)context;
    tb_arena_reset(NULL);
}

static void mark_null(void *context)
{
    (void)context;
    (void)tb_arena_get_mark(NULL);
}

static void rewind_null(void *context)
{
    const tb_arena_mark mark = {0};

    (void)context;
    tb_arena_rewind(NULL, mark);
}

static void capacity_of_null(void *context)
{
    (void)context;
    (void)tb_arena_capacity(NULL);
}

static void remaining_of_null(void *context)
{
    (void)context;
    (void)tb_arena_remaining(NULL);
}

TB_TEST(core_arena, a_null_arena_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(alloc_from_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(try_alloc_from_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(calloc_from_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(try_calloc_from_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(reset_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(mark_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(rewind_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(capacity_of_null, NULL, "arena != NULL");
    TB_TEST_EXPECT_TRAP(remaining_of_null, NULL, "arena != NULL");
}

typedef struct alignment_request
{
    tb_arena *arena;
    size_t alignment;
} alignment_request;

static void alloc_with_alignment(void *context)
{
    const alignment_request *const request = context;

    (void)tb_arena_alloc(request->arena, 64, request->alignment);
}

static void calloc_with_alignment(void *context)
{
    const alignment_request *const request = context;

    (void)tb_arena_calloc(request->arena, 4, 16, request->alignment);
}

static void calloc_an_overflowing_array_with_alignment(void *context)
{
    const alignment_request *const request = context;

    (void)tb_arena_try_calloc(request->arena, SIZE_MAX, 2, request->alignment);
}

TB_TEST(core_arena, an_alignment_that_is_not_a_power_of_two_violates_the_contract)
{
    static const size_t alignments[] = {0, 3, 5, 6, 12, 24, 100, SIZE_MAX};
    tb_arena *arena = NULL;

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    arena = tb_arena_create(SMALL_CHUNK_SIZE);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(alignments); index++)
    {
        alignment_request request = {.arena = arena, .alignment = alignments[index]};

        TB_TEST_CONTEXT("alignment %zu", alignments[index]);
        TB_TEST_EXPECT_TRAP(alloc_with_alignment, &request, "alignment must be a power of two");
        TB_TEST_EXPECT_TRAP(calloc_with_alignment, &request, "alignment must be a power of two");
        /* Checked even when the size is refused first. */
        TB_TEST_EXPECT_TRAP(calloc_an_overflowing_array_with_alignment, &request,
                            "alignment must be a power of two");
    }
    tb_arena_destroy(arena);
}

static void create_fixed_without_a_buffer(void *context)
{
    (void)context;
    (void)tb_arena_create_fixed(NULL, FIXED_BUFFER_SIZE);
}

static void create_fixed_in_a_buffer_that_is_too_small(void *context)
{
    (void)tb_arena_create_fixed(context, TB_ARENA_MINIMUM_SIZE - 1);
}

TB_TEST(core_arena, a_missing_or_undersized_buffer_violates_the_contract)
{
    static unsigned char buffer[TB_ARENA_MINIMUM_SIZE];

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(create_fixed_without_a_buffer, NULL, "buffer != NULL");
    TB_TEST_EXPECT_TRAP(create_fixed_in_a_buffer_that_is_too_small, buffer,
                        "TB_ARENA_MINIMUM_SIZE");
}

typedef struct rewind_request
{
    tb_arena *arena;
    tb_arena_mark mark;
} rewind_request;

static void rewind_to_mark(void *context)
{
    const rewind_request *const request = context;

    tb_arena_rewind(request->arena, request->mark);
}

TB_TEST(core_arena, rewinding_to_a_released_or_foreign_mark_violates_the_contract)
{
    static const char *const expected = "the mark must come from this arena";
    tb_arena *arena = NULL;
    tb_arena *other = NULL;
    rewind_request request = {0};
    tb_arena_mark outer = {0};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    arena = tb_arena_create(SMALL_CHUNK_SIZE);
    other = tb_arena_create(SMALL_CHUNK_SIZE);
    request.arena = arena;

    /* A mark that was never taken. */
    TB_TEST_EXPECT_TRAP(rewind_to_mark, &request, expected);

    /* A mark taken from another arena. */
    (void)tb_arena_alloc(other, 100, 1);
    request.mark = tb_arena_get_mark(other);
    TB_TEST_EXPECT_TRAP(rewind_to_mark, &request, expected);

    /* An inner mark, after rewinding to the outer one. */
    outer = tb_arena_get_mark(arena);
    (void)tb_arena_alloc(arena, 100, 1);
    request.mark = tb_arena_get_mark(arena);
    tb_arena_rewind(arena, outer);
    TB_TEST_EXPECT_TRAP(rewind_to_mark, &request, expected);

    /* A mark in a later chunk, after rewinding out of that chunk. */
    (void)tb_arena_alloc(arena, 5000, 1);
    request.mark = tb_arena_get_mark(arena);
    tb_arena_rewind(arena, outer);
    TB_TEST_EXPECT_TRAP(rewind_to_mark, &request, expected);

    /* A mark taken before a reset. */
    (void)tb_arena_alloc(arena, 100, 1);
    request.mark = tb_arena_get_mark(arena);
    tb_arena_reset(arena);
    TB_TEST_EXPECT_TRAP(rewind_to_mark, &request, expected);

    /* The arena is undamaged by the refusals. */
    fill_pattern(tb_arena_alloc(arena, 100, 1), 100, 1);
    tb_arena_destroy(other);
    tb_arena_destroy(arena);
}
