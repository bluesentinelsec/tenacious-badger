/* Exhausts memory with the engine's default failure handler, which reports to
 * stderr and ends the process. The case first restores that handler, because
 * the test entry point replaces it. ExpectOutOfMemoryAbort.cmake runs this case as
 * a child process and checks how it ends; it is not registered with CTest
 * directly. */
#include "badger/memory.h"

#include "tb_test.h"

#include <stdio.h>

/* An allocator with nothing left to give. */
static void *refuse_malloc_request(size_t size)
{
    (void)size;
    return NULL;
}

static void *refuse_calloc_request(size_t count, size_t size)
{
    (void)count;
    (void)size;
    return NULL;
}

static void *refuse_realloc_request(void *memory, size_t size)
{
    (void)memory;
    (void)size;
    return NULL;
}

static void ignore_free_request(void *memory)
{
    (void)memory;
}

TB_TEST(memory_probe, exhausted_allocation_reports_and_aborts)
{
    const tb_memory_functions exhausted = {.malloc_function = refuse_malloc_request,
                                           .calloc_function = refuse_calloc_request,
                                           .realloc_function = refuse_realloc_request,
                                           .free_function = ignore_free_request};

    tb_memory_set_failure_handler(NULL, NULL);
    TB_TEST_REQUIRE(tb_memory_set_functions(&exhausted));
    /* The fallible form reports failure and carries on. */
    TB_TEST_REQUIRE_POINTER(tb_try_malloc(64), ==, NULL);
    puts("before-fail-fast-allocation");
    (void)fflush(stdout);
    (void)tb_calloc(3, 64);
    puts("after-fail-fast-allocation");
}
