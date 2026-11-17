/* The count of processors must make sense on every machine the tests run on,
 * whatever it has. The machines differ, so these cases check what must hold
 * on all of them. The counting itself is checked against machines that are
 * described to it, in the tests of each adapter that has such counting. */
#include "badger/assert.h"
#include "badger/system.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stddef.h>

TB_TEST(platform_system, processor_counts_are_at_least_one_and_ordered)
{
    tb_system_processors processors = {0};

    tb_system_get_processors(&processors);
    TB_TEST_CONTEXT("logical %zu, cores %zu, performance cores %zu", processors.logical_count,
                    processors.core_count, processors.performance_core_count);
    TB_TEST_EXPECT_UINT(processors.logical_count, >=, 1);
    TB_TEST_EXPECT_UINT(processors.core_count, >=, 1);
    TB_TEST_EXPECT_UINT(processors.performance_core_count, >=, 1);
    /* A core has at least one logical processor, and the fastest cores are
     * some of the cores. */
    TB_TEST_EXPECT_UINT(processors.core_count, <=, processors.logical_count);
    TB_TEST_EXPECT_UINT(processors.performance_core_count, <=, processors.core_count);
}

/* Every field is written: an answer does not depend on what was there. */
TB_TEST(platform_system, processor_counts_replace_whatever_the_structure_held)
{
    tb_system_processors from_zero = {0};
    tb_system_processors from_junk = {
        .logical_count = SIZE_MAX, .core_count = SIZE_MAX, .performance_core_count = SIZE_MAX};

    tb_system_get_processors(&from_zero);
    tb_system_get_processors(&from_junk);
    TB_TEST_EXPECT_UINT(from_junk.logical_count, ==, from_zero.logical_count);
    TB_TEST_EXPECT_UINT(from_junk.core_count, ==, from_zero.core_count);
    TB_TEST_EXPECT_UINT(from_junk.performance_core_count, ==, from_zero.performance_core_count);
}

#define THREAD_COUNT 8

typedef struct asker
{
    tb_system_processors answer;
} asker;

static void ask(void *argument)
{
    asker *const self = argument;

    tb_system_get_processors(&self->answer);
}

/* Nothing confines a thread that the engine starts, so each sees what the
 * thread of the case sees, and asking from several at once is safe. */
TB_TEST(platform_system, every_thread_gets_the_same_processor_counts)
{
    tb_system_processors expected = {0};
    asker askers[THREAD_COUNT] = {0};
    tb_thread threads[THREAD_COUNT] = {0};

    tb_system_get_processors(&expected);
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_create(&threads[index], ask, &askers[index]);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        tb_thread_join(&threads[index]);
    }
    for (size_t index = 0; index < THREAD_COUNT; index++)
    {
        TB_TEST_CONTEXT("thread %zu", index);
        TB_TEST_EXPECT_UINT(askers[index].answer.logical_count, ==, expected.logical_count);
        TB_TEST_EXPECT_UINT(askers[index].answer.core_count, ==, expected.core_count);
        TB_TEST_EXPECT_UINT(askers[index].answer.performance_core_count, ==,
                            expected.performance_core_count);
    }
}

static void get_into_null(void *context)
{
    (void)context;
    tb_system_get_processors(NULL);
}

TB_TEST(platform_system, a_null_pointer_violates_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the platform adapter was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(get_into_null, NULL, "processors != NULL");
}
