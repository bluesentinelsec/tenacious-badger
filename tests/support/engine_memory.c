#include "engine_test.h"

#include "badger/memory.h"
#include "tb_test.h"

#include <stdio.h>

/* Capacity of the trap message built from a failed allocation. */
#define TB_ENGINE_TEST_MESSAGE_CAPACITY 160

/* The engine's built-in memory functions, which do the real work. */
static tb_memory_functions tb_engine_test_original_memory = {0};

/* Failure injection: while enabled, this many attempts still succeed. */
static bool tb_engine_test_is_failing_allocations = {0};
static size_t tb_engine_test_successes_left = {0};

/* Blocks allocated when the running case began. */
static size_t tb_engine_test_blocks_before_case = {0};

static bool tb_engine_test_attempt_must_fail(void)
{
    if (!tb_engine_test_is_failing_allocations)
    {
        return false;
    }
    if (tb_engine_test_successes_left > 0)
    {
        tb_engine_test_successes_left--;
        return false;
    }
    return true;
}

static void *tb_engine_test_malloc(size_t size)
{
    if (tb_engine_test_attempt_must_fail())
    {
        return NULL;
    }
    return tb_engine_test_original_memory.malloc_function(size);
}

static void *tb_engine_test_calloc(size_t count, size_t size)
{
    if (tb_engine_test_attempt_must_fail())
    {
        return NULL;
    }
    return tb_engine_test_original_memory.calloc_function(count, size);
}

static void *tb_engine_test_realloc(void *memory, size_t size)
{
    if (tb_engine_test_attempt_must_fail())
    {
        return NULL;
    }
    return tb_engine_test_original_memory.realloc_function(memory, size);
}

static void tb_engine_test_free(void *memory)
{
    tb_engine_test_original_memory.free_function(memory);
}

void tb_engine_test_install_memory_functions(void)
{
    const tb_memory_functions functions = {.malloc_function = tb_engine_test_malloc,
                                           .calloc_function = tb_engine_test_calloc,
                                           .realloc_function = tb_engine_test_realloc,
                                           .free_function = tb_engine_test_free};

    tb_memory_get_original_functions(&tb_engine_test_original_memory);
    if (!tb_memory_set_functions(&functions))
    {
        /* Fails the calling case, or ends the process when no case is running. */
        tb_test_trap(__FILE__, __LINE__,
                     "the test memory functions cannot be installed while blocks are allocated");
    }
}

/* Turns an exhausted fail-fast allocation into a test framework trap. It
 * leaves the engine by a non-local jump, as tb_test_trap() documents. */
static void tb_engine_test_report_memory_failure(const tb_memory_failure *failure, void *context)
{
    char message[TB_ENGINE_TEST_MESSAGE_CAPACITY] = {0};

    (void)context;
    (void)snprintf(message, sizeof(message), "out of memory: %zu x %zu bytes, alignment %zu",
                   failure->count, failure->size, failure->alignment);
    tb_test_trap(__FILE__, __LINE__, message);
}

void tb_engine_test_install_memory_failure_handler(void)
{
    tb_memory_set_failure_handler(tb_engine_test_report_memory_failure, NULL);
}

void tb_engine_test_fail_allocations_after(size_t successes)
{
    tb_engine_test_is_failing_allocations = true;
    tb_engine_test_successes_left = successes;
}

void tb_engine_test_stop_failing_allocations(void)
{
    tb_engine_test_is_failing_allocations = false;
    tb_engine_test_successes_left = 0;
}

void tb_engine_test_before_case(void *context)
{
    (void)context;
    tb_engine_test_stop_failing_allocations();
    tb_engine_test_blocks_before_case = tb_memory_allocation_count();
}

/* Suffix that makes a counted noun plural: "1 block", "2 blocks". */
static const char *tb_engine_test_plural(size_t count)
{
    if (count == 1)
    {
        return "";
    }
    return "s";
}

void tb_engine_test_after_case(void *context)
{
    const tb_test_case *const test = tb_test_current_case();
    const size_t blocks_now = tb_memory_allocation_count();

    (void)context;
    tb_engine_test_stop_failing_allocations();
    if (tb_test_case_has_failed())
    {
        return;
    }
    if (blocks_now > tb_engine_test_blocks_before_case)
    {
        const size_t leaked = blocks_now - tb_engine_test_blocks_before_case;

        tb_test_fail_at(test->file, test->line,
                        "memory leak: %zu block%s allocated by this case and never freed", leaked,
                        tb_engine_test_plural(leaked));
    }
    if (blocks_now < tb_engine_test_blocks_before_case)
    {
        const size_t extra = tb_engine_test_blocks_before_case - blocks_now;

        tb_test_fail_at(test->file, test->line,
                        "this case freed %zu block%s that it did not allocate", extra,
                        tb_engine_test_plural(extra));
    }
}
