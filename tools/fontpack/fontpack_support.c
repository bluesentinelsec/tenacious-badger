/* The functions of the engine that its compressor calls, for badger-fontpack,
 * which is built from the compressor's sources without the engine library:
 * allocation from the C library's heap, and assertions that report and stop.
 * Only what src/codec/deflate.c, inflate.c, and checksum.c call is here; a
 * new call there fails the tool's link, which names what to add. */
#include "badger/allocator.h"
#include "badger/assert.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What malloc aligns every block to at least, on every platform the engine
 * builds for: two pointers. */
#define FONTPACK_MALLOC_ALIGNMENT (2 * sizeof(void *))

static void *fontpack_heap(void *context, void *memory, size_t old_size, size_t new_size,
                           size_t alignment)
{
    (void)context;
    (void)old_size;
    if (alignment > FONTPACK_MALLOC_ALIGNMENT)
    {
        fprintf(stderr, "badger-fontpack: an alignment of %zu is more than malloc gives\n",
                alignment);
        exit(EXIT_FAILURE);
    }
    if (new_size == 0)
    {
        free(memory);
        return NULL;
    }
    return realloc(memory, new_size);
}

tb_allocator tb_allocator_resolve(const tb_allocator *allocator)
{
    tb_allocator heap = {fontpack_heap, NULL};
    if (allocator != NULL)
    {
        return *allocator;
    }
    return heap;
}

void *tb_allocator_calloc(const tb_allocator *allocator, size_t count, size_t size,
                          size_t alignment)
{
    void *memory = NULL;
    if (size != 0 && count > (size_t)-1 / size)
    {
        fprintf(stderr, "badger-fontpack: an allocation's size overflows\n");
        exit(EXIT_FAILURE);
    }
    memory = allocator->function(allocator->context, NULL, 0, count * size, alignment);
    if (memory == NULL)
    {
        fprintf(stderr, "badger-fontpack: out of memory\n");
        exit(EXIT_FAILURE);
    }
    memset(memory, 0, count * size);
    return memory;
}

void tb_allocator_free_array(const tb_allocator *allocator, void *memory, size_t count, size_t size,
                             size_t alignment)
{
    if (memory != NULL)
    {
        allocator->function(allocator->context, memory, count * size, 0, alignment);
    }
}

_Noreturn void tb_assert_fail(const char *expression, const char *message, const char *file,
                              int line, const char *function)
{
    fprintf(stderr, "badger-fontpack: %s:%d: %s: assertion failed: %s%s%s\n", file, line, function,
            expression, message != NULL ? ": " : "", message != NULL ? message : "");
    abort();
}
