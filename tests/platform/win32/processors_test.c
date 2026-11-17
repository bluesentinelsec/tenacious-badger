/* The Windows adapter counts processors from the system's description of
 * each core: its efficiency class, and the logical processors it presents as
 * a group and a mask. The counting is apart from the call that asks, so
 * these cases describe machines to it: the development laptop, with its two
 * kinds of core, a process confined to part of one, and a machine of more
 * than 64 processors, which Windows divides into groups. */
#include "processors.h"

#include "tb_test.h"

#include <stddef.h>

/* Efficiency classes: a higher class is a faster core. */
#define EFFICIENT_CLASS ((uint8_t)0)
#define PERFORMANCE_CLASS ((uint8_t)1)

/* The development laptop: 8 fast cores of two logical processors each,
 * numbered 0 to 15, then 16 efficient cores of one each, numbered 16 to 31. */
#define LAPTOP_FAST_CORE_COUNT ((size_t)8)
#define LAPTOP_EFFICIENT_CORE_COUNT ((size_t)16)

static void add_the_laptop(tb_win32_processor_tally *tally)
{
    for (size_t core = 0; core < LAPTOP_FAST_CORE_COUNT; core++)
    {
        tb_win32_processors_add_core(tally, PERFORMANCE_CLASS, 0, UINT64_C(3) << (core * 2));
    }
    for (size_t core = 0; core < LAPTOP_EFFICIENT_CORE_COUNT; core++)
    {
        tb_win32_processors_add_core(tally, EFFICIENT_CLASS, 0,
                                     UINT64_C(1) << (LAPTOP_FAST_CORE_COUNT * 2 + core));
    }
}

static void expect_counts(const tb_win32_processor_tally *tally, size_t logical_count,
                          size_t core_count, size_t performance_core_count)
{
    tb_system_processors processors = {
        .logical_count = SIZE_MAX, .core_count = SIZE_MAX, .performance_core_count = SIZE_MAX};

    tb_win32_processors_finish(tally, &processors);
    TB_TEST_EXPECT_UINT(processors.logical_count, ==, logical_count);
    TB_TEST_EXPECT_UINT(processors.core_count, ==, core_count);
    TB_TEST_EXPECT_UINT(processors.performance_core_count, ==, performance_core_count);
}

TB_TEST(platform_win32_processors, a_machine_with_two_kinds_of_core_is_counted_by_kind)
{
    tb_win32_processor_tally tally = {0};

    add_the_laptop(&tally);
    expect_counts(&tally, 32, 24, 8);
}

/* The order in which the system lists the cores must not matter. */
TB_TEST(platform_win32_processors, the_fastest_kind_is_found_wherever_it_is_listed)
{
    tb_win32_processor_tally efficient_first = {0};
    tb_win32_processor_tally mixed = {0};

    tb_win32_processors_add_core(&efficient_first, EFFICIENT_CLASS, 0, UINT64_C(1) << 0);
    tb_win32_processors_add_core(&efficient_first, EFFICIENT_CLASS, 0, UINT64_C(1) << 1);
    tb_win32_processors_add_core(&efficient_first, PERFORMANCE_CLASS, 0, UINT64_C(3) << 2);
    expect_counts(&efficient_first, 4, 3, 1);

    tb_win32_processors_add_core(&mixed, PERFORMANCE_CLASS, 0, UINT64_C(3) << 0);
    tb_win32_processors_add_core(&mixed, EFFICIENT_CLASS, 0, UINT64_C(1) << 2);
    tb_win32_processors_add_core(&mixed, PERFORMANCE_CLASS, 0, UINT64_C(3) << 3);
    tb_win32_processors_add_core(&mixed, EFFICIENT_CLASS, 0, UINT64_C(1) << 5);
    expect_counts(&mixed, 6, 4, 2);
}

/* Three kinds: only the fastest of them are performance cores. */
TB_TEST(platform_win32_processors, only_the_highest_class_counts_as_performance)
{
    tb_win32_processor_tally tally = {0};

    tb_win32_processors_add_core(&tally, 1, 0, UINT64_C(1) << 0);
    tb_win32_processors_add_core(&tally, 2, 0, UINT64_C(1) << 1);
    tb_win32_processors_add_core(&tally, 0, 0, UINT64_C(1) << 2);
    tb_win32_processors_add_core(&tally, 2, 0, UINT64_C(1) << 3);
    tb_win32_processors_add_core(&tally, 1, 0, UINT64_C(1) << 4);
    expect_counts(&tally, 5, 5, 2);
}

/* Cores that are all alike are all performance cores, whatever their class
 * is called, with one logical processor each or two. */
TB_TEST(platform_win32_processors, cores_of_one_kind_are_all_performance_cores)
{
    tb_win32_processor_tally single = {0};
    tb_win32_processor_tally paired = {0};

    for (size_t core = 0; core < 6; core++)
    {
        tb_win32_processors_add_core(&single, 0, 0, UINT64_C(1) << core);
        tb_win32_processors_add_core(&paired, 5, 0, UINT64_C(3) << (core * 2));
    }
    expect_counts(&single, 6, 6, 6);
    expect_counts(&paired, 12, 6, 6);
}

TB_TEST(platform_win32_processors, a_confined_process_counts_only_what_it_may_use)
{
    /* The first logical processor of the first two fast cores, and the first
     * four efficient cores. */
    tb_win32_processor_tally some_of_each = {
        .affinity = {.is_limited = true, .group = 0, .mask = UINT64_C(0x000F0005)}};
    /* Efficient cores alone are the fastest that the process has. */
    tb_win32_processor_tally efficient_only = {
        .affinity = {.is_limited = true, .group = 0, .mask = UINT64_C(0x00030000)}};
    /* Both logical processors of one fast core. */
    tb_win32_processor_tally one_core = {
        .affinity = {.is_limited = true, .group = 0, .mask = UINT64_C(0x0000000C)}};
    /* One logical processor. */
    tb_win32_processor_tally one_processor = {
        .affinity = {.is_limited = true, .group = 0, .mask = UINT64_C(0x80000000)}};

    add_the_laptop(&some_of_each);
    expect_counts(&some_of_each, 6, 6, 2);
    add_the_laptop(&efficient_only);
    expect_counts(&efficient_only, 2, 2, 2);
    add_the_laptop(&one_core);
    expect_counts(&one_core, 2, 1, 1);
    add_the_laptop(&one_processor);
    expect_counts(&one_processor, 1, 1, 1);
}

/* A mask says nothing about a group it was not given for. */
TB_TEST(platform_win32_processors, a_confined_process_has_nothing_in_another_group)
{
    tb_win32_processor_tally tally = {
        .affinity = {.is_limited = true, .group = 1, .mask = UINT64_C(0xF)}};

    for (size_t core = 0; core < 4; core++)
    {
        tb_win32_processors_add_core(&tally, 0, 0, UINT64_C(1) << core);
    }
    tb_win32_processors_add_core(&tally, 0, 1, UINT64_C(1) << 0);
    tb_win32_processors_add_core(&tally, 0, 1, UINT64_C(1) << 1);
    tb_win32_processors_add_core(&tally, 0, 1, UINT64_C(1) << 7);
    expect_counts(&tally, 2, 2, 2);
}

/* A process that is not confined runs in every group. A full group has 64
 * logical processors, which fills the mask. */
TB_TEST(platform_win32_processors, an_unconfined_process_counts_every_group)
{
    tb_win32_processor_tally tally = {0};

    for (uint16_t group = 0; group < 3; group++)
    {
        for (size_t core = 0; core < 32; core++)
        {
            tb_win32_processors_add_core(&tally, 0, group, UINT64_C(3) << (core * 2));
        }
    }
    expect_counts(&tally, 192, 96, 96);
}

/* No running process has no processor, but the counts are never zero. */
TB_TEST(platform_win32_processors, no_allowed_processor_still_counts_one)
{
    tb_win32_processor_tally nothing_added = {0};
    tb_win32_processor_tally nothing_allowed = {
        .affinity = {.is_limited = true, .group = 0, .mask = 0}};

    expect_counts(&nothing_added, 1, 1, 1);
    add_the_laptop(&nothing_allowed);
    expect_counts(&nothing_allowed, 1, 1, 1);
}

/* The same tally that counts, lists: each allowed processor in the order the
 * system describes the cores, its place in its group, its core among the
 * allowed cores, and its class. */
TB_TEST(platform_win32_processors, the_processors_are_listed_core_by_core)
{
    tb_win32_processor list[40] = {{0}};
    tb_win32_processor_tally tally = {.list = list, .list_capacity = 40};

    add_the_laptop(&tally);
    TB_TEST_REQUIRE_UINT(tally.logical_count, ==, 32);
    for (size_t place = 0; place < 32; place++)
    {
        size_t core = place / 2;
        uint8_t efficiency_class = PERFORMANCE_CLASS;

        TB_TEST_CONTEXT("processor %zu", place);
        if (place >= 16)
        {
            core = place - 8;
            efficiency_class = EFFICIENT_CLASS;
        }
        TB_TEST_EXPECT_UINT(list[place].group, ==, 0);
        TB_TEST_EXPECT_UINT(list[place].number, ==, place);
        TB_TEST_EXPECT_UINT(list[place].core, ==, core);
        TB_TEST_EXPECT_UINT(list[place].efficiency_class, ==, efficiency_class);
    }
}

/* Confined to the second processor of each of two fast cores and to two
 * efficient cores, in group 1 of a machine of two groups: four processors,
 * on four cores, numbered from zero. Nothing is written past the room. */
TB_TEST(platform_win32_processors, a_confined_process_lists_only_what_it_may_use)
{
    tb_win32_processor list[3] = {{0}};
    tb_win32_processor_tally tally = {
        .affinity = {.is_limited = true, .group = 1, .mask = UINT64_C(0x3000A)},
        .list = list,
        .list_capacity = 3};

    add_the_laptop(&tally);
    for (size_t core = 0; core < LAPTOP_FAST_CORE_COUNT; core++)
    {
        tb_win32_processors_add_core(&tally, PERFORMANCE_CLASS, 1, UINT64_C(3) << (core * 2));
    }
    for (size_t core = 0; core < LAPTOP_EFFICIENT_CORE_COUNT; core++)
    {
        tb_win32_processors_add_core(&tally, EFFICIENT_CLASS, 1,
                                     UINT64_C(1) << (LAPTOP_FAST_CORE_COUNT * 2 + core));
    }
    TB_TEST_REQUIRE_UINT(tally.logical_count, ==, 4);
    TB_TEST_EXPECT_UINT(list[0].group, ==, 1);
    TB_TEST_EXPECT_UINT(list[0].number, ==, 1);
    TB_TEST_EXPECT_UINT(list[0].core, ==, 0);
    TB_TEST_EXPECT_UINT(list[1].number, ==, 3);
    TB_TEST_EXPECT_UINT(list[1].core, ==, 1);
    TB_TEST_EXPECT_UINT(list[2].number, ==, 16);
    TB_TEST_EXPECT_UINT(list[2].core, ==, 2);
    TB_TEST_EXPECT_UINT(list[2].efficiency_class, ==, EFFICIENT_CLASS);
}
