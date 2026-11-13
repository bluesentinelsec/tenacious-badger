/* The only engine source that calls the C library's allocator. Every other
 * file allocates through badger/memory.h; tools/check_allocations.py enforces it. */
#include "badger/memory.h"

#include "badger/assert.h"
#include "badger/platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An aligned block stores the address of the underlying allocation in the
 * bytes just before the address handed to the caller. */
#define TB_MEMORY_ALIGNED_HEADER_SIZE sizeof(void *)

/* ---------------------------------------------------------------------------
 * The built-in memory functions. They are wrapped rather than referenced so
 * their addresses are constants whichever way the C library is linked.
 * ------------------------------------------------------------------------- */

static void *tb_memory_original_malloc(size_t size)
{
    return malloc(size);
}

static void *tb_memory_original_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static void *tb_memory_original_realloc(void *memory, size_t size)
{
    return realloc(memory, size);
}

static void tb_memory_original_free(void *memory)
{
    free(memory);
}

#define TB_MEMORY_ORIGINAL_FUNCTIONS                                                               \
    {.malloc_function = tb_memory_original_malloc,                                                 \
     .calloc_function = tb_memory_original_calloc,                                                 \
     .realloc_function = tb_memory_original_realloc,                                               \
     .free_function = tb_memory_original_free}

static tb_memory_functions tb_memory_installed = TB_MEMORY_ORIGINAL_FUNCTIONS;

/* Blocks allocated and not yet freed. */
static tb_platform_atomic_size tb_memory_live_blocks = {0};

/* Installed failure handler; a NULL handler selects the default report. */
typedef struct tb_memory_failure_binding
{
    tb_memory_failure_handler *handler;
    void *context;
} tb_memory_failure_binding;

static tb_memory_failure_binding tb_memory_failure_installed = {0};

/* ---------------------------------------------------------------------------
 * Running out of memory.
 * ------------------------------------------------------------------------- */

static _Noreturn void tb_memory_fail(size_t count, size_t size, size_t alignment)
{
    const tb_memory_failure failure = {.count = count, .size = size, .alignment = alignment};

    if (tb_memory_failure_installed.handler != NULL)
    {
        tb_memory_failure_installed.handler(&failure, tb_memory_failure_installed.context);
    }
    else
    {
        fprintf(stderr, "badger: out of memory allocating %zu x %zu bytes (alignment %zu)\n", count,
                size, alignment);
    }
    /* Reached by the default report and by a handler that returned: the caller
     * was promised memory, so there is nothing to return to. */
    (void)fflush(stderr);
    tb_platform_abort();
}

void tb_memory_set_failure_handler(tb_memory_failure_handler *handler, void *context)
{
    tb_memory_failure_installed.handler = handler;
    tb_memory_failure_installed.context = context;
}

/* ---------------------------------------------------------------------------
 * Replacing the memory functions.
 * ------------------------------------------------------------------------- */

void tb_memory_get_original_functions(tb_memory_functions *functions)
{
    TB_ASSERT(functions != NULL);
    *functions = (tb_memory_functions)TB_MEMORY_ORIGINAL_FUNCTIONS;
}

void tb_memory_get_functions(tb_memory_functions *functions)
{
    TB_ASSERT(functions != NULL);
    *functions = tb_memory_installed;
}

bool tb_memory_set_functions(const tb_memory_functions *functions)
{
    TB_ASSERT(functions != NULL);
    if (functions->malloc_function == NULL || functions->calloc_function == NULL ||
        functions->realloc_function == NULL || functions->free_function == NULL)
    {
        return false;
    }
    /* A live block would later be resized or freed by the wrong allocator. */
    if (tb_memory_allocation_count() != 0)
    {
        return false;
    }
    tb_memory_installed = *functions;
    return true;
}

size_t tb_memory_allocation_count(void)
{
    return tb_platform_atomic_size_load(&tb_memory_live_blocks);
}

/* ---------------------------------------------------------------------------
 * Allocation for callers that can recover.
 * ------------------------------------------------------------------------- */

/* Every size the memory functions see is at least 1, so their result never
 * depends on how an allocator treats empty requests. */
static size_t tb_memory_at_least_one(size_t value)
{
    if (value == 0)
    {
        return 1;
    }
    return value;
}

void *tb_try_malloc(size_t size)
{
    void *const memory = tb_memory_installed.malloc_function(tb_memory_at_least_one(size));

    if (memory != NULL)
    {
        (void)tb_platform_atomic_size_add(&tb_memory_live_blocks, 1);
    }
    return memory;
}

void *tb_try_calloc(size_t count, size_t size)
{
    void *memory = NULL;

    count = tb_memory_at_least_one(count);
    size = tb_memory_at_least_one(size);
    if (count > SIZE_MAX / size)
    {
        return NULL;
    }
    memory = tb_memory_installed.calloc_function(count, size);
    if (memory != NULL)
    {
        (void)tb_platform_atomic_size_add(&tb_memory_live_blocks, 1);
    }
    return memory;
}

void *tb_try_realloc(void *memory, size_t size)
{
    if (memory == NULL)
    {
        return tb_try_malloc(size);
    }
    /* A resize neither adds nor removes a block, whether or not it succeeds. */
    return tb_memory_installed.realloc_function(memory, tb_memory_at_least_one(size));
}

void *tb_try_realloc_array(void *memory, size_t count, size_t size)
{
    count = tb_memory_at_least_one(count);
    size = tb_memory_at_least_one(size);
    if (count > SIZE_MAX / size)
    {
        return NULL;
    }
    return tb_try_realloc(memory, count * size);
}

void tb_free(void *memory)
{
    if (memory == NULL)
    {
        return;
    }
    tb_memory_installed.free_function(memory);
    (void)tb_platform_atomic_size_subtract(&tb_memory_live_blocks, 1);
}

/* ---------------------------------------------------------------------------
 * Allocation that fails fast.
 * ------------------------------------------------------------------------- */

void *tb_malloc(size_t size)
{
    void *const memory = tb_try_malloc(size);

    if (memory == NULL)
    {
        tb_memory_fail(1, size, 0);
    }
    return memory;
}

void *tb_calloc(size_t count, size_t size)
{
    void *const memory = tb_try_calloc(count, size);

    if (memory == NULL)
    {
        tb_memory_fail(count, size, 0);
    }
    return memory;
}

void *tb_realloc(void *memory, size_t size)
{
    void *const resized = tb_try_realloc(memory, size);

    if (resized == NULL)
    {
        tb_memory_fail(1, size, 0);
    }
    return resized;
}

void *tb_realloc_array(void *memory, size_t count, size_t size)
{
    void *const resized = tb_try_realloc_array(memory, count, size);

    if (resized == NULL)
    {
        tb_memory_fail(count, size, 0);
    }
    return resized;
}

/* ---------------------------------------------------------------------------
 * Aligned allocation, built on the memory functions so that a replacement
 * allocator needs nothing beyond the four of them.
 * ------------------------------------------------------------------------- */

static bool tb_memory_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

void *tb_try_aligned_alloc(size_t alignment, size_t size)
{
    unsigned char *underlying = NULL;
    unsigned char *aligned = NULL;
    uintptr_t first_usable = {0};
    size_t padding = {0};

    TB_ASSERT_MSG(tb_memory_is_power_of_two(alignment), "alignment must be a power of two");
    size = tb_memory_at_least_one(size);
    /* Room for the header, plus enough slack to reach the next aligned address
     * from wherever the underlying block happens to start. */
    if (alignment > SIZE_MAX - TB_MEMORY_ALIGNED_HEADER_SIZE ||
        size > SIZE_MAX - TB_MEMORY_ALIGNED_HEADER_SIZE - alignment)
    {
        return NULL;
    }
    underlying = tb_try_malloc(size + TB_MEMORY_ALIGNED_HEADER_SIZE + alignment);
    if (underlying == NULL)
    {
        return NULL;
    }
    first_usable = (uintptr_t)underlying + TB_MEMORY_ALIGNED_HEADER_SIZE;
    padding = (size_t)(first_usable & (uintptr_t)(alignment - 1));
    if (padding != 0)
    {
        padding = alignment - padding;
    }
    aligned = underlying + TB_MEMORY_ALIGNED_HEADER_SIZE + padding;
    /* Copied, not assigned through a pointer: the header itself has no
     * alignment guarantee when the requested alignment is small. */
    memcpy(aligned - TB_MEMORY_ALIGNED_HEADER_SIZE, &underlying, TB_MEMORY_ALIGNED_HEADER_SIZE);
    return aligned;
}

void *tb_aligned_alloc(size_t alignment, size_t size)
{
    void *const memory = tb_try_aligned_alloc(alignment, size);

    if (memory == NULL)
    {
        tb_memory_fail(1, size, alignment);
    }
    return memory;
}

void tb_aligned_free(void *memory)
{
    unsigned char *underlying = NULL;

    if (memory == NULL)
    {
        return;
    }
    memcpy(&underlying, (unsigned char *)memory - TB_MEMORY_ALIGNED_HEADER_SIZE,
           TB_MEMORY_ALIGNED_HEADER_SIZE);
    tb_free(underlying);
}
