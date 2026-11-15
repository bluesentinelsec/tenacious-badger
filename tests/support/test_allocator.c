#include "test_allocator.h"

#include <stdint.h>
#include <string.h>

static void tb_test_allocator_report(tb_test_allocator *tracker, const char *violation)
{
    if (tracker->violation_count == 0)
    {
        tracker->first_violation = violation;
    }
    tracker->violation_count++;
}

static bool tb_test_allocator_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* The record of a live block, or NULL when the address is not one. */
static tb_test_allocator_block *tb_test_allocator_find(tb_test_allocator *tracker,
                                                       const void *memory)
{
    for (size_t index = 0; index < tracker->live_block_count; index++)
    {
        if (tracker->blocks[index].memory == memory)
        {
            return &tracker->blocks[index];
        }
    }
    return NULL;
}

static void tb_test_allocator_forget(tb_test_allocator *tracker, tb_test_allocator_block *block)
{
    tracker->live_byte_count -= block->size;
    tracker->live_block_count--;
    *block = tracker->blocks[tracker->live_block_count];
}

static bool tb_test_allocator_must_refuse(tb_test_allocator *tracker)
{
    if (!tracker->is_failing)
    {
        return false;
    }
    if (tracker->successes_left > 0)
    {
        tracker->successes_left--;
        return false;
    }
    tracker->refusal_count++;
    return true;
}

/* Checks a resize or release against the record of the block. Returns the
 * record, or NULL when the block is unknown. */
static tb_test_allocator_block *tb_test_allocator_check_block(tb_test_allocator *tracker,
                                                              const void *memory, size_t old_size,
                                                              size_t alignment)
{
    tb_test_allocator_block *const block = tb_test_allocator_find(tracker, memory);

    if (block == NULL)
    {
        tb_test_allocator_report(
            tracker, "a block was resized or released that this allocator does not own");
        return NULL;
    }
    if (block->size != old_size)
    {
        tb_test_allocator_report(tracker,
                                 "a block was resized or released with a size it was not given");
    }
    if (block->alignment != alignment)
    {
        tb_test_allocator_report(
            tracker, "a block was resized or released with an alignment it was not given");
    }
    return block;
}

static void *tb_test_allocator_function(void *context, void *memory, size_t old_size,
                                        size_t new_size, size_t alignment)
{
    tb_test_allocator *const tracker = context;
    const tb_allocator heap = tb_allocator_heap();
    tb_test_allocator_block *block = NULL;
    void *replacement = NULL;
    size_t kept_size = {0};

    if (!tb_test_allocator_is_power_of_two(alignment))
    {
        tb_test_allocator_report(tracker, "an alignment was not a power of two");
        return NULL;
    }
    if (memory == NULL && (new_size == 0 || old_size != 0))
    {
        tb_test_allocator_report(tracker, "a request without a block was not a plain allocation");
        return NULL;
    }
    if (memory != NULL)
    {
        block = tb_test_allocator_check_block(tracker, memory, old_size, alignment);
        if (block == NULL)
        {
            return NULL;
        }
    }
    /* The record, not the caller, says how the block was really allocated. */
    if (new_size == 0)
    {
        tracker->release_count++;
        tb_allocator_free(&heap, memory, block->size, block->alignment);
        tb_test_allocator_forget(tracker, block);
        return NULL;
    }
    if (tb_test_allocator_must_refuse(tracker))
    {
        return NULL;
    }
    if (block == NULL && tracker->live_block_count == TB_TEST_ALLOCATOR_BLOCK_LIMIT)
    {
        tb_test_allocator_report(tracker, "more blocks are live than a test allocator can track");
        return NULL;
    }
    /* A resized block always moves, so that a stale pointer into the old one
     * is an invalid access and not a lucky one. */
    replacement = tb_allocator_try_alloc(&heap, new_size, alignment);
    if (replacement == NULL)
    {
        return NULL;
    }
    if (block == NULL)
    {
        tracker->allocation_count++;
        block = &tracker->blocks[tracker->live_block_count];
        tracker->live_block_count++;
    }
    else
    {
        tracker->reallocation_count++;
        kept_size = block->size;
        if (new_size < kept_size)
        {
            kept_size = new_size;
        }
        memcpy(replacement, memory, kept_size);
        tracker->live_byte_count -= block->size;
        tb_allocator_free(&heap, memory, block->size, block->alignment);
    }
    block->memory = replacement;
    block->size = new_size;
    block->alignment = alignment;
    tracker->live_byte_count += new_size;
    if (tracker->live_byte_count > tracker->peak_byte_count)
    {
        tracker->peak_byte_count = tracker->live_byte_count;
    }
    return replacement;
}

tb_allocator tb_test_allocator_start(tb_test_allocator *tracker)
{
    const tb_allocator allocator = {.function = tb_test_allocator_function, .context = tracker};

    memset(tracker, 0, sizeof(*tracker));
    tracker->first_violation = "none";
    return allocator;
}

void tb_test_allocator_fail_after(tb_test_allocator *tracker, size_t successes)
{
    tracker->is_failing = true;
    tracker->successes_left = successes;
}

void tb_test_allocator_stop_failing(tb_test_allocator *tracker)
{
    tracker->is_failing = false;
    tracker->successes_left = 0;
}

bool tb_test_allocator_owns(const tb_test_allocator *tracker, const void *memory, size_t size)
{
    const uintptr_t begin = (uintptr_t)memory;

    for (size_t index = 0; index < tracker->live_block_count; index++)
    {
        const tb_test_allocator_block *const block = &tracker->blocks[index];
        const uintptr_t block_begin = (uintptr_t)block->memory;

        if (begin >= block_begin && size <= block->size &&
            begin - block_begin <= block->size - size)
        {
            return true;
        }
    }
    return false;
}
