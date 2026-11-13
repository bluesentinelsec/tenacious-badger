/* Every module in the engine allocates through badger/memory.h, so its
 * promises are pinned here: blocks are usable and distinct, sizes behave the
 * same everywhere, exhaustion is fatal or reported exactly as documented, and
 * a replacement allocator sees every request. The default report-and-abort
 * path is checked by memory_probe.c in a child process. */
#include "badger/memory.h"

#include "engine_test.h"
#include "tb_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A type with the strictest alignment of any ordinary scalar. */
typedef union widest_scalar {
    long long integer;
    long double real;
    void *pointer;
    void (*function)(void);
} widest_scalar;

/* Writes a pattern that depends on the seed and on each byte's position, so a
 * block that was moved, truncated, or overlapped by another is detected. */
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

static bool is_aligned(const void *memory, size_t alignment)
{
    return ((uintptr_t)memory & (uintptr_t)(alignment - 1)) == 0;
}

static const size_t block_sizes[] = {0,
                                     1,
                                     2,
                                     3,
                                     7,
                                     8,
                                     9,
                                     15,
                                     16,
                                     17,
                                     31,
                                     32,
                                     33,
                                     63,
                                     64,
                                     65,
                                     255,
                                     256,
                                     257,
                                     1023,
                                     1024,
                                     1025,
                                     4095,
                                     4096,
                                     4097,
                                     65535,
                                     65536,
                                     65537,
                                     (size_t)1 << 20};

/* A replacement allocator that counts what the engine asks of it and checks
 * the guarantees the engine documents for these functions. */
typedef struct counting_allocator
{
    tb_memory_functions original;
    size_t malloc_calls;
    size_t calloc_calls;
    size_t realloc_calls;
    size_t free_calls;
    size_t broken_guarantees; /* A zero size or a NULL block reached a function. */
} counting_allocator;

static counting_allocator counting = {0};

static void *counting_malloc_function(size_t size)
{
    counting.malloc_calls++;
    if (size == 0)
    {
        counting.broken_guarantees++;
    }
    return counting.original.malloc_function(size);
}

static void *counting_calloc_function(size_t count, size_t size)
{
    counting.calloc_calls++;
    if (count == 0 || size == 0 || count > SIZE_MAX / size)
    {
        counting.broken_guarantees++;
    }
    return counting.original.calloc_function(count, size);
}

static void *counting_realloc_function(void *memory, size_t size)
{
    counting.realloc_calls++;
    if (memory == NULL || size == 0)
    {
        counting.broken_guarantees++;
    }
    return counting.original.realloc_function(memory, size);
}

static void counting_free_function(void *memory)
{
    counting.free_calls++;
    if (memory == NULL)
    {
        counting.broken_guarantees++;
    }
    counting.original.free_function(memory);
}

static const tb_memory_functions counting_functions = {.malloc_function = counting_malloc_function,
                                                       .calloc_function = counting_calloc_function,
                                                       .realloc_function =
                                                           counting_realloc_function,
                                                       .free_function = counting_free_function};

/* Starts counting from zero with the counting allocator installed. Nothing
 * may be allocated when this is called. */
static void install_counting_allocator(void)
{
    memset(&counting, 0, sizeof(counting));
    tb_memory_get_original_functions(&counting.original);
    TB_TEST_REQUIRE(tb_memory_set_functions(&counting_functions));
}

/* --- blocks ------------------------------------------------------------------ */

TB_TEST(core_memory, malloc_returns_distinct_usable_blocks)
{
    void *blocks[TB_TEST_COUNT_OF(block_sizes)] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(block_sizes); index++)
    {
        TB_TEST_CONTEXT("size %zu", block_sizes[index]);
        blocks[index] = tb_malloc(block_sizes[index]);
        TB_TEST_REQUIRE_POINTER(blocks[index], !=, NULL);
        TB_TEST_EXPECT(is_aligned(blocks[index], _Alignof(widest_scalar)));
        fill_pattern(blocks[index], block_sizes[index], index);
    }
    /* Every block still holds its own pattern, so no two of them overlap. */
    for (size_t index = 0; index < TB_TEST_COUNT_OF(block_sizes); index++)
    {
        TB_TEST_CONTEXT("size %zu", block_sizes[index]);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(blocks[index], block_sizes[index], index), ==,
                            block_sizes[index]);
        tb_free(blocks[index]);
    }
}

TB_TEST(core_memory, calloc_returns_zero_filled_arrays)
{
    static const size_t counts[] = {0, 1, 2, 3, 16, 1000};
    static const size_t sizes[] = {0, 1, 3, 8, 24, 4096};

    for (size_t count_index = 0; count_index < TB_TEST_COUNT_OF(counts); count_index++)
    {
        for (size_t size_index = 0; size_index < TB_TEST_COUNT_OF(sizes); size_index++)
        {
            const size_t total = counts[count_index] * sizes[size_index];
            unsigned char *const bytes = tb_calloc(counts[count_index], sizes[size_index]);
            size_t nonzero = {0};

            TB_TEST_CONTEXT("%zu x %zu bytes", counts[count_index], sizes[size_index]);
            TB_TEST_REQUIRE_POINTER(bytes, !=, NULL);
            for (size_t index = 0; index < total; index++)
            {
                if (bytes[index] != 0)
                {
                    nonzero++;
                }
            }
            TB_TEST_EXPECT_UINT(nonzero, ==, 0);
            fill_pattern(bytes, total, total);
            tb_free(bytes);
        }
    }
}

TB_TEST(core_memory, empty_requests_yield_unique_blocks)
{
    /* The C library may answer a zero-byte request with NULL or with a block.
     * The engine always gives a block, so callers need no special case. */
    void *blocks[] = {
        tb_malloc(0),
        tb_calloc(0, 0),
        tb_calloc(0, 8),
        tb_calloc(8, 0),
        tb_realloc(NULL, 0),
        tb_realloc_array(NULL, 0, 0),
        tb_try_malloc(0),
        tb_try_calloc(0, 0),
        tb_try_realloc(NULL, 0),
        tb_try_realloc_array(NULL, 0, 4),
    };
    void *const aligned = tb_aligned_alloc(64, 0);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(blocks); index++)
    {
        TB_TEST_CONTEXT("block %zu", index);
        TB_TEST_REQUIRE_POINTER(blocks[index], !=, NULL);
        for (size_t other = 0; other < index; other++)
        {
            TB_TEST_EXPECT_POINTER(blocks[index], !=, blocks[other]);
        }
    }
    TB_TEST_EXPECT_POINTER(aligned, !=, NULL);

    /* Resizing to zero keeps a block alive; it never frees. */
    blocks[0] = tb_realloc(blocks[0], 0);
    TB_TEST_EXPECT_POINTER(blocks[0], !=, NULL);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(blocks); index++)
    {
        tb_free(blocks[index]);
    }
    tb_aligned_free(aligned);
}

TB_TEST(core_memory, freeing_null_does_nothing)
{
    const size_t blocks_before = tb_memory_allocation_count();

    tb_free(NULL);
    tb_aligned_free(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- resizing ---------------------------------------------------------------- */

typedef struct resize_row
{
    size_t old_size;
    size_t new_size;
} resize_row;

TB_TEST(core_memory, realloc_preserves_contents_up_to_the_smaller_size)
{
    static const resize_row rows[] = {
        {1, 1},       {1, 2},         {2, 1},       {16, 17},     {17, 16},
        {64, 4096},   {4096, 64},     {4096, 4097}, {1000, 1000}, {1, 1 << 20},
        {1 << 20, 1}, {65536, 65537}, {100, 0},     {0, 100},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        size_t kept = rows[index].old_size;
        unsigned char *block = tb_malloc(rows[index].old_size);

        if (rows[index].new_size < kept)
        {
            kept = rows[index].new_size;
        }
        TB_TEST_CONTEXT("row %zu: %zu -> %zu bytes", index, rows[index].old_size,
                        rows[index].new_size);
        fill_pattern(block, rows[index].old_size, index);
        block = tb_realloc(block, rows[index].new_size);
        TB_TEST_REQUIRE_POINTER(block, !=, NULL);
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, kept, index), ==, kept);
        /* The whole new size is writable. */
        fill_pattern(block, rows[index].new_size, index + 1);
        tb_free(block);
    }
}

TB_TEST(core_memory, realloc_of_null_allocates)
{
    unsigned char *const block = tb_realloc(NULL, 48);
    uint32_t *const numbers = tb_realloc_array(NULL, 12, sizeof(uint32_t));

    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    TB_TEST_REQUIRE_POINTER(numbers, !=, NULL);
    fill_pattern(block, 48, 1);
    fill_pattern(numbers, 12 * sizeof(uint32_t), 2);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, 48, 1), ==, 48);
    tb_free(block);
    tb_free(numbers);
}

TB_TEST(core_memory, repeated_resizing_never_loses_data)
{
    enum
    {
        RESIZE_COUNT = 3000,
        MAXIMUM_SIZE = 8192
    };
    size_t size = 1;
    unsigned char *block = tb_malloc(size);

    fill_pattern(block, size, 0);
    for (size_t step = 1; step <= RESIZE_COUNT; step++)
    {
        const size_t new_size = (size_t)tb_test_random_between(1, MAXIMUM_SIZE);
        size_t kept = size;

        if (new_size < kept)
        {
            kept = new_size;
        }
        TB_TEST_CONTEXT("step %zu: %zu -> %zu bytes", step, size, new_size);
        if (tb_test_random_below(2) == 0)
        {
            block = tb_realloc(block, new_size);
        }
        else
        {
            block = tb_realloc_array(block, new_size, 1);
        }
        TB_TEST_REQUIRE_UINT(find_pattern_mismatch(block, kept, step - 1), ==, kept);
        size = new_size;
        fill_pattern(block, size, step);
    }
    tb_free(block);
}

/* --- array size arithmetic ---------------------------------------------------- */

typedef struct array_row
{
    size_t count;
    size_t size;
} array_row;

/* Element counts and sizes whose product does not fit in size_t. */
static const array_row overflowing_arrays[] = {
    {SIZE_MAX, 2},        {2, SIZE_MAX},
    {SIZE_MAX, SIZE_MAX}, {SIZE_MAX / 2 + 1, 2},
    {3, SIZE_MAX / 2},    {(size_t)1 << (sizeof(size_t) * 4), (size_t)1 << (sizeof(size_t) * 4)},
    {SIZE_MAX / 7, 8},
};

TB_TEST(core_memory, overflowing_array_sizes_are_refused_before_allocating)
{
    unsigned char *existing = NULL;

    /* The C library also rejects most of these, which would hide a missing
     * check. The counting allocator proves that it is never even asked. */
    install_counting_allocator();
    existing = tb_malloc(32);
    fill_pattern(existing, 32, 9);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(overflowing_arrays); index++)
    {
        const array_row row = overflowing_arrays[index];

        void *const from_calloc = tb_try_calloc(row.count, row.size);
        void *const from_nothing = tb_try_realloc_array(NULL, row.count, row.size);
        void *const from_existing = tb_try_realloc_array(existing, row.count, row.size);

        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_POINTER(from_calloc, ==, NULL);
        TB_TEST_EXPECT_POINTER(from_nothing, ==, NULL);
        TB_TEST_EXPECT_POINTER(from_existing, ==, NULL);
        if (from_calloc != NULL || from_nothing != NULL || from_existing != NULL)
        {
            /* A wrapped size was honored. Release what came back and stop:
             * the original block may have been shrunk or moved. */
            tb_free(from_calloc);
            tb_free(from_nothing);
            if (from_existing != NULL)
            {
                existing = from_existing;
            }
            break;
        }
        /* A refused resize leaves the original block alone. */
        TB_TEST_EXPECT_UINT(find_pattern_mismatch(existing, 32, 9), ==, 32);
    }
    tb_free(existing);
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT_UINT(counting.malloc_calls, ==, 1);
    TB_TEST_EXPECT_UINT(counting.calloc_calls, ==, 0);
    TB_TEST_EXPECT_UINT(counting.realloc_calls, ==, 0);
    TB_TEST_EXPECT_UINT(counting.broken_guarantees, ==, 0);
}

static void calloc_an_overflowing_array(void *context)
{
    const array_row *const row = context;

    (void)tb_calloc(row->count, row->size);
}

static void resize_to_an_overflowing_array(void *context)
{
    const array_row *const row = context;

    (void)tb_realloc_array(NULL, row->count, row->size);
}

TB_TEST(core_memory, overflowing_array_sizes_are_fatal_for_fail_fast_functions)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(overflowing_arrays); index++)
    {
        array_row row = overflowing_arrays[index];
        char expected[96] = {0};

        TB_TEST_CONTEXT("row %zu", index);
        (void)snprintf(expected, sizeof(expected), "out of memory: %zu x %zu bytes", row.count,
                       row.size);
        TB_TEST_EXPECT_TRAP(calloc_an_overflowing_array, &row, expected);
        TB_TEST_EXPECT_TRAP(resize_to_an_overflowing_array, &row, expected);
    }
}

/* --- aligned blocks ----------------------------------------------------------- */

TB_TEST(core_memory, aligned_blocks_honor_every_power_of_two)
{
    static const size_t sizes[] = {0, 1, 7, 64, 1000, 4097};
    enum
    {
        LARGEST_ALIGNMENT_SHIFT = 16
    };

    for (size_t shift = 0; shift <= LARGEST_ALIGNMENT_SHIFT; shift++)
    {
        const size_t alignment = (size_t)1 << shift;

        for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
        {
            void *const first = tb_aligned_alloc(alignment, sizes[index]);
            void *const second = tb_try_aligned_alloc(alignment, sizes[index]);

            TB_TEST_CONTEXT("alignment %zu, size %zu", alignment, sizes[index]);
            TB_TEST_REQUIRE_POINTER(first, !=, NULL);
            TB_TEST_REQUIRE_POINTER(second, !=, NULL);
            TB_TEST_EXPECT(is_aligned(first, alignment));
            TB_TEST_EXPECT(is_aligned(second, alignment));
            fill_pattern(first, sizes[index], 1);
            fill_pattern(second, sizes[index], 2);
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(first, sizes[index], 1), ==, sizes[index]);
            TB_TEST_EXPECT_UINT(find_pattern_mismatch(second, sizes[index], 2), ==, sizes[index]);
            tb_aligned_free(first);
            tb_aligned_free(second);
        }
    }
}

TB_TEST(core_memory, aligned_sizes_too_large_to_pad_are_refused_before_allocating)
{
    static const array_row rows[] = {
        /* `count` is the alignment here. */
        {16, SIZE_MAX},
        {16, SIZE_MAX - 16},
        {4096, SIZE_MAX - 4096},
        {(size_t)1 << (sizeof(size_t) * 8 - 1), (size_t)1 << (sizeof(size_t) * 8 - 1)},
        {(size_t)1 << (sizeof(size_t) * 8 - 1), SIZE_MAX},
    };

    /* Padding these sizes would wrap around to a small request, which an
     * allocator would happily satisfy with a block that is far too short. */
    install_counting_allocator();
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_POINTER(tb_try_aligned_alloc(rows[index].count, rows[index].size), ==, NULL);
    }
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT_UINT(counting.malloc_calls, ==, 0);
}

static void allocate_with_alignment(void *context)
{
    const size_t *const alignment = context;

    tb_aligned_free(tb_aligned_alloc(*alignment, 64));
}

TB_TEST(core_memory, alignment_that_is_not_a_power_of_two_violates_the_contract)
{
    static const size_t invalid_alignments[] = {0, 3, 5, 6, 12, 24, 100, SIZE_MAX};

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(invalid_alignments); index++)
    {
        size_t alignment = invalid_alignments[index];

        TB_TEST_CONTEXT("alignment %zu", alignment);
        TB_TEST_EXPECT_TRAP(allocate_with_alignment, &alignment,
                            "alignment must be a power of two");
    }
}

/* --- accounting --------------------------------------------------------------- */

TB_TEST(core_memory, allocation_count_follows_every_block)
{
    const size_t start = tb_memory_allocation_count();
    void *first = tb_malloc(10);
    void *second = NULL;
    void *third = NULL;
    void *fourth = NULL;

    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 1);
    second = tb_calloc(4, 4);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 2);
    third = tb_aligned_alloc(128, 10);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 3);
    fourth = tb_realloc(NULL, 10);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 4);

    /* Resizing changes a block, not the number of blocks. */
    first = tb_realloc(first, 5000);
    second = tb_realloc_array(second, 1, 1);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 4);

    /* Refused requests and null frees do not count. */
    TB_TEST_EXPECT_POINTER(tb_try_calloc(SIZE_MAX, 2), ==, NULL);
    tb_free(NULL);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 4);

    tb_free(first);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 3);
    tb_free(second);
    tb_aligned_free(third);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start + 1);
    tb_free(fourth);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, start);
}

/* --- exhaustion ---------------------------------------------------------------- */

TB_TEST(core_memory, fallible_functions_return_null_when_memory_is_exhausted)
{
    const size_t blocks_before = tb_memory_allocation_count();
    unsigned char *const existing = tb_malloc(40);

    fill_pattern(existing, 40, 3);
    tb_engine_test_fail_allocations_after(0);
    TB_TEST_EXPECT_POINTER(tb_try_malloc(64), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_calloc(4, 16), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_realloc(NULL, 64), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_realloc_array(NULL, 4, 16), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_aligned_alloc(32, 64), ==, NULL);
    /* A failed resize leaves the block as it was, still owned by the caller. */
    TB_TEST_EXPECT_POINTER(tb_try_realloc(existing, 4000), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_realloc_array(existing, 100, 40), ==, NULL);
    tb_engine_test_stop_failing_allocations();

    TB_TEST_EXPECT_UINT(find_pattern_mismatch(existing, 40, 3), ==, 40);
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before + 1);
    tb_free(existing);
}

static void exhaust_malloc(void *context)
{
    (void)context;
    tb_engine_test_fail_allocations_after(0);
    (void)tb_malloc(64);
}

static void exhaust_calloc(void *context)
{
    (void)context;
    tb_engine_test_fail_allocations_after(0);
    (void)tb_calloc(3, 8);
}

static void exhaust_realloc(void *context)
{
    tb_engine_test_fail_allocations_after(0);
    (void)tb_realloc(context, 128);
}

static void exhaust_realloc_array(void *context)
{
    tb_engine_test_fail_allocations_after(0);
    (void)tb_realloc_array(context, 5, 16);
}

static void exhaust_aligned_alloc(void *context)
{
    (void)context;
    tb_engine_test_fail_allocations_after(0);
    (void)tb_aligned_alloc(32, 64);
}

static void exhaust_new(void *context)
{
    (void)context;
    tb_engine_test_fail_allocations_after(0);
    (void)TB_NEW(widest_scalar);
}

TB_TEST(core_memory, fail_fast_functions_are_fatal_when_memory_is_exhausted)
{
    unsigned char *const existing = tb_malloc(24);
    char expected_for_new[96] = {0};

    fill_pattern(existing, 24, 5);
    TB_TEST_EXPECT_TRAP(exhaust_malloc, NULL, "out of memory: 1 x 64 bytes, alignment 0");
    TB_TEST_EXPECT_TRAP(exhaust_calloc, NULL, "out of memory: 3 x 8 bytes, alignment 0");
    TB_TEST_EXPECT_TRAP(exhaust_realloc, existing, "out of memory: 1 x 128 bytes, alignment 0");
    TB_TEST_EXPECT_TRAP(exhaust_realloc_array, existing,
                        "out of memory: 5 x 16 bytes, alignment 0");
    TB_TEST_EXPECT_TRAP(exhaust_aligned_alloc, NULL, "out of memory: 1 x 64 bytes, alignment 32");
    (void)snprintf(expected_for_new, sizeof(expected_for_new), "out of memory: 1 x %zu bytes",
                   sizeof(widest_scalar));
    TB_TEST_EXPECT_TRAP(exhaust_new, NULL, expected_for_new);
    tb_engine_test_stop_failing_allocations();

    /* The block that could not be resized is intact and still ours to free. */
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(existing, 24, 5), ==, 24);
    tb_free(existing);
}

typedef struct recorded_failure
{
    int calls;
    tb_memory_failure failure;
} recorded_failure;

static void record_failure_then_trap(const tb_memory_failure *failure, void *context)
{
    recorded_failure *const record = context;

    record->calls++;
    record->failure = *failure;
    tb_test_trap(__FILE__, __LINE__, "recorded by the replacement handler");
}

TB_TEST(core_memory, failure_handler_receives_the_request_and_its_context)
{
    recorded_failure record = {0};

    tb_memory_set_failure_handler(record_failure_then_trap, &record);
    TB_TEST_EXPECT_TRAP(exhaust_calloc, NULL, "recorded by the replacement handler");
    TB_TEST_EXPECT_TRAP(exhaust_aligned_alloc, NULL, "recorded by the replacement handler");
    /* Put the shared handler back before any check can end this case. */
    tb_engine_test_install_memory_failure_handler();
    tb_engine_test_stop_failing_allocations();

    TB_TEST_EXPECT_INT(record.calls, ==, 2);
    TB_TEST_EXPECT_UINT(record.failure.count, ==, 1);
    TB_TEST_EXPECT_UINT(record.failure.size, ==, 64);
    TB_TEST_EXPECT_UINT(record.failure.alignment, ==, 32);
}

TB_TEST(core_memory, fallible_functions_never_call_the_failure_handler)
{
    recorded_failure record = {0};

    tb_memory_set_failure_handler(record_failure_then_trap, &record);
    tb_engine_test_fail_allocations_after(0);
    TB_TEST_EXPECT_POINTER(tb_try_malloc(8), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_calloc(SIZE_MAX, SIZE_MAX), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_try_aligned_alloc(8, 8), ==, NULL);
    tb_engine_test_stop_failing_allocations();
    tb_engine_test_install_memory_failure_handler();
    TB_TEST_EXPECT_INT(record.calls, ==, 0);
}

/* A typical operation that needs several blocks and can report failure. It is
 * the shape later modules use for recoverable allocation: every exit releases
 * what was acquired. */
static bool build_three_buffers(void)
{
    unsigned char *first = NULL;
    unsigned char *second = NULL;
    unsigned char *third = NULL;
    unsigned char *resized = NULL;
    bool succeeded = false;

    first = tb_try_malloc(100);
    if (first == NULL)
    {
        goto cleanup;
    }
    second = tb_try_calloc(10, 10);
    if (second == NULL)
    {
        goto cleanup;
    }
    third = tb_try_aligned_alloc(64, 100);
    if (third == NULL)
    {
        goto cleanup;
    }
    /* A failed resize leaves its block allocated, so the result must not
     * overwrite the only pointer to that block. */
    resized = tb_try_realloc(second, 200);
    if (resized == NULL)
    {
        goto cleanup;
    }
    second = resized;
    succeeded = true;

cleanup:
    tb_free(first);
    tb_free(second);
    tb_aligned_free(third);
    return succeeded;
}

TB_TEST(core_memory, exhaustion_at_any_step_can_be_survived_without_leaking)
{
    enum
    {
        ALLOCATION_STEPS = 4
    };
    const size_t blocks_before = tb_memory_allocation_count();

    /* Fail the first attempt, then the second, and so on. The last round
     * fails nothing. This case's leak check covers every early exit. */
    for (size_t successes = 0; successes <= ALLOCATION_STEPS; successes++)
    {
        TB_TEST_CONTEXT("%zu attempts succeed before memory runs out", successes);
        tb_engine_test_fail_allocations_after(successes);
        TB_TEST_EXPECT(build_three_buffers() == (successes == ALLOCATION_STEPS));
        tb_engine_test_stop_failing_allocations();
        TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
    }
}

/* --- typed allocation ---------------------------------------------------------- */

typedef struct sample_object
{
    void *pointer;
    double real;
    int integer;
    char text[13];
} sample_object;

TB_TEST(core_memory, new_macros_return_zero_filled_typed_objects)
{
    enum
    {
        ELEMENT_COUNT = 37
    };
    static const sample_object zero_object = {0};
    sample_object *const object = TB_NEW(sample_object);
    sample_object *const array = TB_NEW_ARRAY(sample_object, ELEMENT_COUNT);

    TB_TEST_REQUIRE_POINTER(object, !=, NULL);
    TB_TEST_REQUIRE_POINTER(array, !=, NULL);
    TB_TEST_EXPECT_POINTER(object->pointer, ==, NULL);
    TB_TEST_EXPECT_FLOAT(object->real, ==, 0.0);
    TB_TEST_EXPECT_INT(object->integer, ==, 0);
    TB_TEST_EXPECT_STRING(object->text, ==, "");
    for (size_t index = 0; index < ELEMENT_COUNT; index++)
    {
        TB_TEST_CONTEXT("element %zu", index);
        TB_TEST_EXPECT_POINTER(array[index].pointer, ==, zero_object.pointer);
        TB_TEST_EXPECT_INT(array[index].integer, ==, 0);
        array[index].integer = (int)index;
    }
    tb_free(object);
    tb_free(array);
}

/* --- replacing the memory functions --------------------------------------------- */

TB_TEST(core_memory, replacement_functions_serve_every_request)
{
    tb_memory_functions installed = {0};
    void *block = NULL;

    install_counting_allocator();
    tb_memory_get_functions(&installed);
    TB_TEST_EXPECT(installed.malloc_function == counting_malloc_function);
    TB_TEST_EXPECT(installed.calloc_function == counting_calloc_function);
    TB_TEST_EXPECT(installed.realloc_function == counting_realloc_function);
    TB_TEST_EXPECT(installed.free_function == counting_free_function);

    block = tb_malloc(10);
    block = tb_realloc(block, 20);
    block = tb_realloc_array(block, 3, 10);
    tb_free(block);
    tb_free(tb_calloc(2, 2));
    tb_free(tb_try_malloc(1));
    tb_free(tb_try_calloc(1, 1));
    tb_free(TB_NEW(sample_object));
    /* An aligned block is carved out of one ordinary allocation. */
    tb_aligned_free(tb_aligned_alloc(256, 10));
    /* Resizing nothing allocates; freeing nothing does not reach the allocator. */
    tb_free(tb_realloc(NULL, 10));
    tb_free(NULL);
    /* Empty requests arrive as one-byte requests. */
    tb_free(tb_malloc(0));
    tb_free(tb_calloc(0, 0));
    tb_free(tb_realloc(tb_malloc(4), 0));

    /* Put the shared functions back before any check can end this case. */
    tb_engine_test_install_memory_functions();

    TB_TEST_EXPECT_UINT(counting.malloc_calls, ==, 6);
    TB_TEST_EXPECT_UINT(counting.calloc_calls, ==, 4);
    TB_TEST_EXPECT_UINT(counting.realloc_calls, ==, 3);
    TB_TEST_EXPECT_UINT(counting.free_calls, ==, 10);
    TB_TEST_EXPECT_UINT(counting.broken_guarantees, ==, 0);
}

TB_TEST(core_memory, replacement_is_refused_while_blocks_are_allocated)
{
    tb_memory_functions before = {0};
    tb_memory_functions after = {0};
    void *const block = tb_malloc(1);
    void *const aligned = tb_aligned_alloc(16, 1);

    tb_memory_get_original_functions(&counting.original);
    tb_memory_get_functions(&before);
    /* The new allocator could not resize or free these blocks. */
    TB_TEST_EXPECT(!tb_memory_set_functions(&counting_functions));
    tb_free(block);
    TB_TEST_EXPECT(!tb_memory_set_functions(&counting_functions));
    tb_aligned_free(aligned);

    tb_memory_get_functions(&after);
    TB_TEST_EXPECT(after.malloc_function == before.malloc_function);
    TB_TEST_EXPECT(after.free_function == before.free_function);

    /* With nothing allocated the same request succeeds. */
    TB_TEST_EXPECT(tb_memory_set_functions(&counting_functions));
    tb_engine_test_install_memory_functions();
}

TB_TEST(core_memory, incomplete_replacement_functions_are_refused)
{
    tb_memory_functions before = {0};
    tb_memory_functions after = {0};

    tb_memory_get_functions(&before);
    for (size_t missing = 0; missing < 4; missing++)
    {
        tb_memory_functions incomplete = counting_functions;

        TB_TEST_CONTEXT("member %zu is NULL", missing);
        if (missing == 0)
        {
            incomplete.malloc_function = NULL;
        }
        if (missing == 1)
        {
            incomplete.calloc_function = NULL;
        }
        if (missing == 2)
        {
            incomplete.realloc_function = NULL;
        }
        if (missing == 3)
        {
            incomplete.free_function = NULL;
        }
        TB_TEST_EXPECT(!tb_memory_set_functions(&incomplete));
    }
    tb_memory_get_functions(&after);
    TB_TEST_EXPECT(after.malloc_function == before.malloc_function);
    TB_TEST_EXPECT(after.calloc_function == before.calloc_function);
    TB_TEST_EXPECT(after.realloc_function == before.realloc_function);
    TB_TEST_EXPECT(after.free_function == before.free_function);
}

TB_TEST(core_memory, original_functions_are_a_working_allocator)
{
    tb_memory_functions original = {0};
    tb_memory_functions installed = {0};
    const size_t blocks_before = tb_memory_allocation_count();
    unsigned char *block = NULL;
    unsigned char *zeroed = NULL;

    tb_memory_get_original_functions(&original);
    tb_memory_get_functions(&installed);
    /* The test executable has replaced them, so they must differ. */
    TB_TEST_EXPECT(original.malloc_function != installed.malloc_function);

    block = original.malloc_function(64);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    fill_pattern(block, 64, 4);
    block = original.realloc_function(block, 4096);
    TB_TEST_REQUIRE_POINTER(block, !=, NULL);
    TB_TEST_EXPECT_UINT(find_pattern_mismatch(block, 64, 4), ==, 64);
    zeroed = original.calloc_function(8, 8);
    TB_TEST_REQUIRE_POINTER(zeroed, !=, NULL);
    TB_TEST_EXPECT_UINT(zeroed[63], ==, 0);
    original.free_function(block);
    original.free_function(zeroed);
    /* Blocks taken straight from the allocator are not the engine's to count. */
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}

/* --- everything together --------------------------------------------------------- */

typedef struct tracked_block
{
    unsigned char *memory; /* NULL while the slot is empty. */
    size_t size;
    size_t seed;
    bool is_aligned;
} tracked_block;

/* Sizes are mostly small, as in real programs, with occasional large ones. */
static size_t random_block_size(void)
{
    if (tb_test_random_below(8) == 0)
    {
        return (size_t)tb_test_random_below(70000);
    }
    return (size_t)tb_test_random_below(96);
}

static void allocate_random_block(tracked_block *block, size_t seed)
{
    const size_t size = random_block_size();

    block->size = size;
    block->seed = seed;
    block->is_aligned = false;
    switch (tb_test_random_below(5))
    {
    case 0:
        block->memory = tb_malloc(size);
        break;
    case 1:
        block->memory = tb_calloc(size, 1);
        break;
    case 2:
        block->memory = tb_realloc(NULL, size);
        break;
    case 3:
        block->memory = tb_try_malloc(size);
        break;
    default:
        block->memory = tb_aligned_alloc((size_t)1 << tb_test_random_below(10), size);
        block->is_aligned = true;
        break;
    }
    fill_pattern(block->memory, size, seed);
}

static void release_block(tracked_block *block)
{
    if (block->is_aligned)
    {
        tb_aligned_free(block->memory);
    }
    else
    {
        tb_free(block->memory);
    }
    block->memory = NULL;
}

TB_TEST(core_memory, random_operations_keep_every_block_intact)
{
    enum
    {
        SLOT_COUNT = 96,
        OPERATION_COUNT = 40000
    };
    tracked_block slots[SLOT_COUNT] = {0};
    const size_t blocks_before = tb_memory_allocation_count();
    size_t live = {0};

    for (size_t operation = 0; operation < OPERATION_COUNT; operation++)
    {
        tracked_block *const block = &slots[tb_test_random_below(SLOT_COUNT)];

        TB_TEST_CONTEXT("operation %zu", operation);
        if (block->memory == NULL)
        {
            allocate_random_block(block, operation);
            TB_TEST_REQUIRE_POINTER(block->memory, !=, NULL);
            live++;
        }
        else
        {
            /* Nothing that happened to other blocks may have touched this one. */
            TB_TEST_REQUIRE_UINT(find_pattern_mismatch(block->memory, block->size, block->seed), ==,
                                 block->size);
            if (!block->is_aligned && tb_test_random_below(2) == 0)
            {
                const size_t new_size = random_block_size();
                size_t kept = block->size;

                if (new_size < kept)
                {
                    kept = new_size;
                }
                block->memory = tb_realloc(block->memory, new_size);
                TB_TEST_REQUIRE_UINT(find_pattern_mismatch(block->memory, kept, block->seed), ==,
                                     kept);
                block->size = new_size;
                block->seed = operation;
                fill_pattern(block->memory, new_size, operation);
            }
            else
            {
                release_block(block);
                live--;
            }
        }
        TB_TEST_REQUIRE_UINT(tb_memory_allocation_count(), ==, blocks_before + live);
    }

    for (size_t index = 0; index < SLOT_COUNT; index++)
    {
        if (slots[index].memory != NULL)
        {
            TB_TEST_CONTEXT("slot %zu at the end", index);
            TB_TEST_EXPECT_UINT(
                find_pattern_mismatch(slots[index].memory, slots[index].size, slots[index].seed),
                ==, slots[index].size);
            release_block(&slots[index]);
        }
    }
    TB_TEST_EXPECT_UINT(tb_memory_allocation_count(), ==, blocks_before);
}
