/* The count of processors must make sense on every machine the tests run on,
 * whatever it has. The machines differ, so these cases check what must hold
 * on all of them. The counting itself is checked against machines that are
 * described to it, in the tests of each adapter that has such counting. */
#include "badger/assert.h"
#include "badger/system.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#define LIST_CAPACITY ((size_t)4096)

/* The list describes what the counts count. Where processors cannot be
 * confined, the system does not say which is of which kind, and only the
 * number of entries is compared. */
TB_TEST(platform_system, the_list_of_processors_agrees_with_the_counts)
{
    static tb_system_processor list[LIST_CAPACITY];
    tb_system_processors counts = {0};
    size_t count = 0;
    size_t next_core = 0;
    size_t fast_cores = 0;

    tb_system_get_processors(&counts);
    count = tb_system_list_processors(list, LIST_CAPACITY);
    TB_TEST_REQUIRE_UINT(count, ==, counts.logical_count);
    TB_TEST_REQUIRE_UINT(count, <=, LIST_CAPACITY);
    TB_TEST_EXPECT_UINT(tb_system_list_processors(NULL, 0), ==, count);
    for (size_t index = 0; index < count; index++)
    {
        TB_TEST_CONTEXT("processor %zu", index);
        /* A core is first reached in order, and none is skipped. */
        TB_TEST_REQUIRE_UINT(list[index].core, <=, next_core);
        if (list[index].core == next_core)
        {
            next_core++;
            if (list[index].is_performance)
            {
                fast_cores++;
            }
        }
    }
    if (!tb_thread_can_be_confined())
    {
        return;
    }
    TB_TEST_EXPECT_UINT(next_core, ==, counts.core_count);
    TB_TEST_EXPECT_UINT(fast_cores, ==, counts.performance_core_count);
}

typedef struct placed
{
    size_t seen[64]; /* Where the thread found itself, each time it looked. */
    size_t looks;
} placed;

static void look_where_it_runs(void *argument)
{
    placed *const self = argument;

    for (size_t look = 0; look < 64; look++)
    {
        self->seen[look] = tb_thread_current_processor();
        tb_thread_yield();
    }
    self->looks = 64;
}

static bool is_among(size_t place, const size_t *places, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        if (places[index] == place)
        {
            return true;
        }
    }
    return false;
}

static void expect_confined(const size_t *places, size_t count, const size_t *allowed,
                            size_t allowed_count)
{
    const tb_thread_options options = {.processors = places, .processor_count = count};
    placed self = {0};
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, &options, look_where_it_runs, &self);
    tb_thread_join(&thread);
    TB_TEST_REQUIRE_UINT(self.looks, ==, 64);
    for (size_t look = 0; look < 64; look++)
    {
        if (!is_among(self.seen[look], allowed, allowed_count))
        {
            TB_TEST_FAIL("the thread ran on processor %zu", self.seen[look]);
            return;
        }
    }
}

/* A thread confined to one processor runs on that one, for the first and the
 * last of the list; one confined to two runs on one of them; and a place
 * that is not in the list is left out. */
TB_TEST(platform_system, a_confined_thread_runs_where_it_was_put)
{
    const size_t count = tb_system_list_processors(NULL, 0);
    const size_t last = count - 1;
    const size_t first_and_last[] = {0, last};
    const size_t with_one_too_many[] = {last, count + 5};

    if (!tb_thread_can_be_confined())
    {
        TB_TEST_EXPECT_UINT(tb_thread_current_processor(), ==, SIZE_MAX);
        TB_TEST_SKIP("threads cannot be confined on this platform");
    }
    TB_TEST_CONTEXT("the first processor");
    expect_confined(&first_and_last[0], 1, &first_and_last[0], 1);
    TB_TEST_CONTEXT("the last processor");
    expect_confined(&first_and_last[1], 1, &first_and_last[1], 1);
    TB_TEST_CONTEXT("the first or the last");
    expect_confined(first_and_last, 2, first_and_last, 2);
    TB_TEST_CONTEXT("the last, and a processor that does not exist");
    expect_confined(with_one_too_many, 2, &with_one_too_many[0], 1);
}

/* The calling thread is on one of the processors of the list. */
TB_TEST(platform_system, the_calling_thread_is_on_a_listed_processor)
{
    const size_t count = tb_system_list_processors(NULL, 0);
    const size_t place = tb_thread_current_processor();

    if (!tb_thread_can_be_confined())
    {
        TB_TEST_EXPECT_UINT(place, ==, SIZE_MAX);
        return;
    }
    TB_TEST_EXPECT_UINT(place, <, count);
}

static void list_into_null(void *context)
{
    (void)context;
    (void)tb_system_list_processors(NULL, 1);
}

static void name_no_processors(void *context)
{
    const tb_thread_options options = {.processor_count = 1};
    tb_thread thread = {0};

    (void)context;
    tb_thread_create_with_options(&thread, &options, look_where_it_runs, NULL);
}

TB_TEST(platform_system, bad_arguments_to_the_list_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the platform adapter was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(list_into_null, NULL, "processors != NULL");
    TB_TEST_EXPECT_TRAP(name_no_processors, NULL, "processors are named");
}
