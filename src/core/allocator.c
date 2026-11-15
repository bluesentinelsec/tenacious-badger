#include "badger/allocator.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/size.h"
#include "memory_internal.h"

#include <stdbool.h>
#include <string.h>

/* A type with the strictest alignment of any ordinary scalar. Every block from
 * tb_malloc() has at least this alignment. */
typedef union tb_allocator_scalar {
    long long integer;
    long double real;
    void *pointer;
    void (*function)(void);
} tb_allocator_scalar;

#define TB_ALLOCATOR_HEAP_ALIGNMENT _Alignof(tb_allocator_scalar)

static bool tb_allocator_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* Every size an allocator function sees for a block is at least 1, so that a
 * size of 0 can only mean "release". */
static size_t tb_allocator_at_least_one(size_t value)
{
    if (value == 0)
    {
        return 1;
    }
    return value;
}

/* ---------------------------------------------------------------------------
 * The engine's allocation functions as an allocator.
 * ------------------------------------------------------------------------- */

/* Blocks beyond the alignment of tb_malloc() come from tb_aligned_alloc(),
 * which cannot resize, so they are resized by moving them. */
static void *tb_allocator_heap_resize_aligned(void *memory, size_t old_size, size_t new_size,
                                              size_t alignment)
{
    void *const resized = tb_try_aligned_alloc(alignment, new_size);
    size_t kept_size = old_size;

    if (resized == NULL)
    {
        return NULL;
    }
    if (memory == NULL)
    {
        return resized;
    }
    if (new_size < kept_size)
    {
        kept_size = new_size;
    }
    memcpy(resized, memory, kept_size);
    tb_aligned_free(memory);
    return resized;
}

static void *tb_allocator_heap_function(void *context, void *memory, size_t old_size,
                                        size_t new_size, size_t alignment)
{
    const bool is_aligned_block = alignment > TB_ALLOCATOR_HEAP_ALIGNMENT;

    (void)context;
    if (new_size == 0 && is_aligned_block)
    {
        tb_aligned_free(memory);
        return NULL;
    }
    if (new_size == 0)
    {
        tb_free(memory);
        return NULL;
    }
    if (is_aligned_block)
    {
        return tb_allocator_heap_resize_aligned(memory, old_size, new_size, alignment);
    }
    return tb_try_realloc(memory, new_size);
}

tb_allocator tb_allocator_heap(void)
{
    const tb_allocator heap = {.function = tb_allocator_heap_function, .context = NULL};

    return heap;
}

tb_allocator tb_allocator_resolve(const tb_allocator *allocator)
{
    if (allocator == NULL)
    {
        return tb_allocator_heap();
    }
    TB_ASSERT_MSG(allocator->function != NULL, "an allocator needs a function");
    return *allocator;
}

/* ---------------------------------------------------------------------------
 * Allocation for callers that can recover.
 * ------------------------------------------------------------------------- */

void *tb_allocator_try_realloc(const tb_allocator *allocator, void *memory, size_t old_size,
                               size_t new_size, size_t alignment)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);

    TB_ASSERT_MSG(tb_allocator_is_power_of_two(alignment), "alignment must be a power of two");
    new_size = tb_allocator_at_least_one(new_size);
    old_size = tb_allocator_at_least_one(old_size);
    if (memory == NULL)
    {
        old_size = 0;
    }
    return resolved.function(resolved.context, memory, old_size, new_size, alignment);
}

void *tb_allocator_try_alloc(const tb_allocator *allocator, size_t size, size_t alignment)
{
    return tb_allocator_try_realloc(allocator, NULL, 0, size, alignment);
}

void *tb_allocator_try_calloc(const tb_allocator *allocator, size_t count, size_t size,
                              size_t alignment)
{
    void *memory = NULL;
    size_t total_size = {0};

    TB_ASSERT_MSG(tb_allocator_is_power_of_two(alignment), "alignment must be a power of two");
    count = tb_allocator_at_least_one(count);
    size = tb_allocator_at_least_one(size);
    if (!tb_size_multiply(count, size, &total_size))
    {
        return NULL;
    }
    memory = tb_allocator_try_alloc(allocator, total_size, alignment);
    if (memory == NULL)
    {
        return NULL;
    }
    memset(memory, 0, total_size);
    return memory;
}

/* ---------------------------------------------------------------------------
 * Allocation that fails fast.
 * ------------------------------------------------------------------------- */

void *tb_allocator_alloc(const tb_allocator *allocator, size_t size, size_t alignment)
{
    void *const memory = tb_allocator_try_alloc(allocator, size, alignment);

    if (memory == NULL)
    {
        tb_memory_fail(1, size, alignment);
    }
    return memory;
}

void *tb_allocator_calloc(const tb_allocator *allocator, size_t count, size_t size,
                          size_t alignment)
{
    void *const memory = tb_allocator_try_calloc(allocator, count, size, alignment);

    if (memory == NULL)
    {
        tb_memory_fail(count, size, alignment);
    }
    return memory;
}

void *tb_allocator_realloc(const tb_allocator *allocator, void *memory, size_t old_size,
                           size_t new_size, size_t alignment)
{
    void *const resized =
        tb_allocator_try_realloc(allocator, memory, old_size, new_size, alignment);

    if (resized == NULL)
    {
        tb_memory_fail(1, new_size, alignment);
    }
    return resized;
}

/* ---------------------------------------------------------------------------
 * Releasing.
 * ------------------------------------------------------------------------- */

void tb_allocator_free(const tb_allocator *allocator, void *memory, size_t size, size_t alignment)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);

    TB_ASSERT_MSG(tb_allocator_is_power_of_two(alignment), "alignment must be a power of two");
    if (memory == NULL)
    {
        return;
    }
    (void)resolved.function(resolved.context, memory, tb_allocator_at_least_one(size), 0,
                            alignment);
}

void tb_allocator_free_array(const tb_allocator *allocator, void *memory, size_t count, size_t size,
                             size_t alignment)
{
    size_t total_size = {0};
    const bool is_size_valid = tb_size_multiply(tb_allocator_at_least_one(count),
                                                tb_allocator_at_least_one(size), &total_size);

    TB_ASSERT_MSG(is_size_valid, "count and size must be those the array was allocated with");
    tb_allocator_free(allocator, memory, total_size, alignment);
}
