/* The Linux adapter counts processors from the kernel's files about them.
 * The counting is plain C over text, and reaches the files through a function
 * that these cases supply, so each case describes a machine: which logical
 * processors share a core, and which cores are the fast ones. That covers
 * machines no CI host is, and files that are missing or malformed. */
#include "processors.h"

#include "tb_test.h"

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Lists of processors.
 * ------------------------------------------------------------------------- */

/* The processors of a set as a string of '1' and '0', the first `count`. */
static void describe(const tb_linux_cpu_set *set, size_t count, char *text)
{
    for (size_t cpu = 0; cpu < count; cpu++)
    {
        text[cpu] = '0';
        if (tb_linux_cpu_set_has(set, cpu))
        {
            text[cpu] = '1';
        }
    }
    text[count] = '\0';
}

#define DESCRIBED_COUNT 24

typedef struct list_case
{
    const char *text;
    const char *members; /* The first DESCRIBED_COUNT processors, '1' for a member. */
} list_case;

static const list_case list_cases[] = {
    {"0\n", "100000000000000000000000"},
    {"0", "100000000000000000000000"},
    {"3\n", "000100000000000000000000"},
    {"0-3\n", "111100000000000000000000"},
    {"5-5\n", "000001000000000000000000"},
    {"0,2\n", "101000000000000000000000"},
    {"0-1,4-5\n", "110011000000000000000000"},
    {"0-7,16-23\n", "111111110000000011111111"},
    {"1,3,5-7,10\n", "010101110010000000000000"},
    /* Out of order, and overlapping: a set all the same. */
    {"4-6,0-1\n", "110011100000000000000000"},
    {"0-3,2-5\n", "111111000000000000000000"},
    /* No processors at all. */
    {"\n", "000000000000000000000000"},
    {"", "000000000000000000000000"},
};

TB_TEST(platform_linux_processors, a_list_names_its_processors)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(list_cases); index++)
    {
        tb_linux_cpu_set set = {0};
        char members[DESCRIBED_COUNT + 1] = {0};

        TB_TEST_CONTEXT("list %zu", index);
        /* Whatever the set held is replaced. */
        tb_linux_cpu_set_add(&set, 23);
        TB_TEST_REQUIRE(tb_linux_cpu_list_parse(list_cases[index].text, &set));
        describe(&set, DESCRIBED_COUNT, members);
        TB_TEST_EXPECT_STRING(members, ==, list_cases[index].members);
        TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, DESCRIBED_COUNT));
    }
}

static const char *const malformed_lists[] = {
    "a\n",      "0-\n",
    "-3\n",     "0,\n",
    ",0\n",     "0,,1\n",
    "3-1\n",    "0-1-2\n",
    "0 1\n",    " 0\n",
    "0 \n",     "0\n\n",
    "0\n1\n",   "0x10\n",
    "1.5\n",    "0-3,x\n",
    "+1\n",     "0;1\n",
    "\n0\n",    "1024\n",
    "0-1024\n", "99999999999999999999999999\n",
};

TB_TEST(platform_linux_processors, a_malformed_list_is_refused_and_leaves_an_empty_set)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(malformed_lists); index++)
    {
        tb_linux_cpu_set set = {0};

        TB_TEST_CONTEXT("list %zu", index);
        tb_linux_cpu_set_add(&set, 7);
        TB_TEST_EXPECT(!tb_linux_cpu_list_parse(malformed_lists[index], &set));
        for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
        {
            if (tb_linux_cpu_set_has(&set, cpu))
            {
                TB_TEST_FAIL("processor %zu is in a set that must be empty", cpu);
            }
        }
    }
}

TB_TEST(platform_linux_processors, the_highest_processor_number_is_one_below_the_limit)
{
    tb_linux_cpu_set set = {0};

    TB_TEST_REQUIRE(tb_linux_cpu_list_parse("0,63-64,1023\n", &set));
    TB_TEST_EXPECT(tb_linux_cpu_set_has(&set, 0));
    TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, 1));
    TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, 62));
    /* Each side of a word of the set. */
    TB_TEST_EXPECT(tb_linux_cpu_set_has(&set, 63));
    TB_TEST_EXPECT(tb_linux_cpu_set_has(&set, 64));
    TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, 65));
    TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, 1022));
    TB_TEST_EXPECT(tb_linux_cpu_set_has(&set, 1023));
    /* Beyond the set, nothing is a member, and asking is safe. */
    TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, 1024));
    TB_TEST_EXPECT(!tb_linux_cpu_set_has(&set, SIZE_MAX));

    TB_TEST_REQUIRE(tb_linux_cpu_list_parse("0-1023\n", &set));
    for (size_t cpu = 0; cpu < TB_LINUX_CPU_LIMIT; cpu++)
    {
        if (!tb_linux_cpu_set_has(&set, cpu))
        {
            TB_TEST_FAIL("processor %zu is missing from the full set", cpu);
        }
    }
}

/* ---------------------------------------------------------------------------
 * A machine, as the files that describe it.
 * ------------------------------------------------------------------------- */

typedef struct machine_file
{
    const char *path;
    const char *text;
} machine_file;

typedef struct machine
{
    const machine_file *files;
    size_t file_count;
    size_t read_count; /* Files asked for, found or not. */
} machine;

static bool is_same_text(const char *left, const char *right)
{
    size_t index = 0;

    while (left[index] != '\0' && left[index] == right[index])
    {
        index++;
    }
    return left[index] == right[index];
}

static bool read_machine_file(void *context, const char *path, char *buffer, size_t capacity)
{
    machine *const described = context;

    described->read_count++;
    for (size_t index = 0; index < described->file_count; index++)
    {
        const char *const text = described->files[index].text;
        size_t length = 0;

        if (!is_same_text(described->files[index].path, path))
        {
            continue;
        }
        while (text[length] != '\0')
        {
            if (length + 1 >= capacity)
            {
                return false;
            }
            buffer[length] = text[length];
            length++;
        }
        buffer[length] = '\0';
        return true;
    }
    return false;
}

static void expect_counts(machine *described, const char *allowed_list, size_t logical_count,
                          size_t core_count, size_t performance_core_count)
{
    tb_linux_cpu_set allowed = {0};
    tb_system_processors processors = {
        .logical_count = SIZE_MAX, .core_count = SIZE_MAX, .performance_core_count = SIZE_MAX};

    TB_TEST_REQUIRE(tb_linux_cpu_list_parse(allowed_list, &allowed));
    tb_linux_count_processors(&allowed, read_machine_file, described, &processors);
    TB_TEST_EXPECT_UINT(processors.logical_count, ==, logical_count);
    TB_TEST_EXPECT_UINT(processors.core_count, ==, core_count);
    TB_TEST_EXPECT_UINT(processors.performance_core_count, ==, performance_core_count);
}

/* Lists the processors among `allowed_list` and checks each against
 * `expected`: its number, its core, and whether it is of the fast kind. */
static void expect_list(machine *described, const char *allowed_list,
                        const tb_linux_processor *expected, size_t expected_count)
{
    tb_linux_cpu_set allowed = {0};
    tb_linux_processor list[16] = {{0}};
    size_t count = 0;

    TB_TEST_REQUIRE(tb_linux_cpu_list_parse(allowed_list, &allowed));
    count = tb_linux_list_processors(&allowed, read_machine_file, described, list, 16);
    TB_TEST_REQUIRE_UINT(count, ==, expected_count);
    for (size_t index = 0; index < count; index++)
    {
        TB_TEST_CONTEXT("place %zu", index);
        TB_TEST_EXPECT_UINT(list[index].cpu, ==, expected[index].cpu);
        TB_TEST_EXPECT_UINT(list[index].core, ==, expected[index].core);
        TB_TEST_EXPECT(list[index].is_performance == expected[index].is_performance);
    }
}

#define CPU "/sys/devices/system/cpu/cpu"
#define SIBLINGS "/topology/thread_siblings_list"
#define CAPACITY "/cpu_capacity"

/* Four cores of two logical processors each, numbered so that the second
 * of each core follows all the first ones, as on a Steam Deck. */
static const machine_file four_paired_cores[] = {
    {CPU "0" SIBLINGS, "0,4\n"}, {CPU "1" SIBLINGS, "1,5\n"}, {CPU "2" SIBLINGS, "2,6\n"},
    {CPU "3" SIBLINGS, "3,7\n"}, {CPU "4" SIBLINGS, "0,4\n"}, {CPU "5" SIBLINGS, "1,5\n"},
    {CPU "6" SIBLINGS, "2,6\n"}, {CPU "7" SIBLINGS, "3,7\n"},
};

TB_TEST(platform_linux_processors, logical_processors_that_share_a_core_are_one_core)
{
    machine described = {.files = four_paired_cores,
                         .file_count = TB_TEST_COUNT_OF(four_paired_cores)};

    TB_TEST_CONTEXT("every processor allowed");
    expect_counts(&described, "0-7\n", 8, 4, 4);
}

TB_TEST(platform_linux_processors, a_confined_process_counts_only_what_it_may_use)
{
    machine described = {.files = four_paired_cores,
                         .file_count = TB_TEST_COUNT_OF(four_paired_cores)};

    TB_TEST_CONTEXT("the first processor of every core");
    expect_counts(&described, "0-3\n", 4, 4, 4);
    TB_TEST_CONTEXT("the second processor of every core");
    expect_counts(&described, "4-7\n", 4, 4, 4);
    TB_TEST_CONTEXT("both processors of two cores");
    expect_counts(&described, "1-2,5-6\n", 4, 2, 2);
    TB_TEST_CONTEXT("one processor of one core and both of another");
    expect_counts(&described, "4,3,7\n", 3, 2, 2);
    TB_TEST_CONTEXT("one processor");
    expect_counts(&described, "6\n", 1, 1, 1);
}

/* A laptop with two kinds of core, like the development machine: 4 fast
 * cores of two logical processors, numbered in pairs, then 4 efficient cores
 * of one. The kernel lists the logical processors of the fast kind. */
static const machine_file two_kinds_of_core[] = {
    {CPU "0" SIBLINGS, "0-1\n"},
    {CPU "1" SIBLINGS, "0-1\n"},
    {CPU "2" SIBLINGS, "2-3\n"},
    {CPU "3" SIBLINGS, "2-3\n"},
    {CPU "4" SIBLINGS, "4-5\n"},
    {CPU "5" SIBLINGS, "4-5\n"},
    {CPU "6" SIBLINGS, "6-7\n"},
    {CPU "7" SIBLINGS, "6-7\n"},
    {CPU "8" SIBLINGS, "8\n"},
    {CPU "9" SIBLINGS, "9\n"},
    {CPU "10" SIBLINGS, "10\n"},
    {CPU "11" SIBLINGS, "11\n"},
    {"/sys/devices/cpu_core/cpus", "0-7\n"},
};

TB_TEST(platform_linux_processors, listed_fast_cores_are_the_performance_cores)
{
    machine described = {.files = two_kinds_of_core,
                         .file_count = TB_TEST_COUNT_OF(two_kinds_of_core)};

    TB_TEST_CONTEXT("every processor allowed");
    expect_counts(&described, "0-11\n", 12, 8, 4);
    TB_TEST_CONTEXT("the second processor of two fast cores, and two efficient cores");
    expect_counts(&described, "1,3,8-9\n", 4, 4, 2);
    /* The efficient cores are then the fastest that the process has. */
    TB_TEST_CONTEXT("efficient cores alone");
    expect_counts(&described, "8-11\n", 4, 4, 4);
    TB_TEST_CONTEXT("fast cores alone");
    expect_counts(&described, "0-7\n", 8, 4, 4);
}

/* An Arm machine with three kinds of core, which the kernel tells apart by
 * capacity: four small, three large, one largest. Each is one processor. */
static const machine_file three_capacities[] = {
    {CPU "0" SIBLINGS, "0\n"},    {CPU "1" SIBLINGS, "1\n"},   {CPU "2" SIBLINGS, "2\n"},
    {CPU "3" SIBLINGS, "3\n"},    {CPU "4" SIBLINGS, "4\n"},   {CPU "5" SIBLINGS, "5\n"},
    {CPU "6" SIBLINGS, "6\n"},    {CPU "7" SIBLINGS, "7\n"},   {CPU "0" CAPACITY, "282\n"},
    {CPU "1" CAPACITY, "282\n"},  {CPU "2" CAPACITY, "282\n"}, {CPU "3" CAPACITY, "282\n"},
    {CPU "4" CAPACITY, "756\n"},  {CPU "5" CAPACITY, "756\n"}, {CPU "6" CAPACITY, "756\n"},
    {CPU "7" CAPACITY, "1024\n"},
};

TB_TEST(platform_linux_processors, cores_of_the_highest_capacity_are_the_performance_cores)
{
    machine described = {.files = three_capacities,
                         .file_count = TB_TEST_COUNT_OF(three_capacities)};

    TB_TEST_CONTEXT("every processor allowed");
    expect_counts(&described, "0-7\n", 8, 8, 1);
    TB_TEST_CONTEXT("without the largest core");
    expect_counts(&described, "0-6\n", 7, 7, 3);
    TB_TEST_CONTEXT("small cores alone");
    expect_counts(&described, "0-3\n", 4, 4, 4);
    TB_TEST_CONTEXT("one small and one large");
    expect_counts(&described, "2,5\n", 2, 2, 1);
}

/* A virtual machine, or a container without /sys: the kernel says nothing. */
TB_TEST(platform_linux_processors, a_system_that_says_nothing_has_cores_of_one_kind)
{
    machine described = {0};

    expect_counts(&described, "0-3\n", 4, 4, 4);
    /* It was asked: about every processor's siblings, then about the kinds. */
    TB_TEST_EXPECT_UINT(described.read_count, >=, 4);
    TB_TEST_CONTEXT("processors far apart");
    expect_counts(&described, "0,512,1023\n", 3, 3, 3);
}

/* What cannot be understood is treated as what is not there. */
static const machine_file broken_files[] = {
    /* Not a list. */
    {CPU "0" SIBLINGS, "zero and one\n"},
    /* A list that leaves out the processor it belongs to, and would make it
     * share the core of another. */
    {CPU "1" SIBLINGS, "0\n"},
    {CPU "2" SIBLINGS, "2-3\n"},
    {CPU "3" SIBLINGS, "2-3\n"},
    /* Not a list either, so the capacities are asked for. */
    {"/sys/devices/cpu_core/cpus", "all of them\n"},
    /* One capacity is not a number, so none is believed. */
    {CPU "0" CAPACITY, "1024\n"},
    {CPU "1" CAPACITY, "high\n"},
    {CPU "2" CAPACITY, "512\n"},
};

TB_TEST(platform_linux_processors, files_that_cannot_be_understood_are_ignored)
{
    machine described = {.files = broken_files, .file_count = TB_TEST_COUNT_OF(broken_files)};

    /* Processors 0 and 1 are each a core; 2 and 3 share one. */
    expect_counts(&described, "0-3\n", 4, 3, 3);
}

/* A capacity that is missing for one core leaves the kinds unknown. */
static const machine_file one_capacity_missing[] = {
    {CPU "0" CAPACITY, "512\n"},
    {CPU "1" CAPACITY, "1024\n"},
};

TB_TEST(platform_linux_processors, a_missing_capacity_leaves_cores_of_one_kind)
{
    machine described = {.files = one_capacity_missing,
                         .file_count = TB_TEST_COUNT_OF(one_capacity_missing)};

    TB_TEST_CONTEXT("both capacities known");
    expect_counts(&described, "0-1\n", 2, 2, 1);
    TB_TEST_CONTEXT("the third unknown");
    expect_counts(&described, "0-2\n", 3, 3, 3);
}

/* A list of fast processors that names none of the allowed ones, and a
 * machine where the list wins over capacities that disagree with it. */
static const machine_file list_and_capacities[] = {
    {"/sys/devices/cpu_core/cpus", "0\n"}, {CPU "0" CAPACITY, "100\n"}, {CPU "1" CAPACITY, "100\n"},
    {CPU "2" CAPACITY, "900\n"},           {CPU "3" CAPACITY, "900\n"},
};

TB_TEST(platform_linux_processors, the_list_of_fast_cores_is_believed_before_capacities)
{
    machine described = {.files = list_and_capacities,
                         .file_count = TB_TEST_COUNT_OF(list_and_capacities)};

    /* One core by the list, where the capacities would say two. */
    TB_TEST_CONTEXT("every processor allowed");
    expect_counts(&described, "0-3\n", 4, 4, 1);
    TB_TEST_CONTEXT("none of the listed ones allowed");
    expect_counts(&described, "2-3\n", 2, 2, 2);
}

/* A capacity is a number and nothing else. */
static const machine_file capacity_with_a_unit[] = {
    {CPU "0" CAPACITY, "1024\n"},
    {CPU "1" CAPACITY, "512 units\n"},
};

TB_TEST(platform_linux_processors, a_capacity_followed_by_text_is_not_believed)
{
    machine described = {.files = capacity_with_a_unit,
                         .file_count = TB_TEST_COUNT_OF(capacity_with_a_unit)};

    expect_counts(&described, "0-1\n", 2, 2, 2);
}

/* The file of a processor is found by its number, of however many digits.
 * Each pair here shares a core, which only its own file says. The numbers
 * are chosen so that a digit dropped, or the digits in another order, names
 * a file that is not there. */
static const machine_file numbers_of_several_digits[] = {
    {CPU "7" SIBLINGS, "7,10\n"},       {CPU "10" SIBLINGS, "7,10\n"},
    {CPU "45" SIBLINGS, "45,130\n"},    {CPU "130" SIBLINGS, "45,130\n"},
    {CPU "902" SIBLINGS, "902,1023\n"}, {CPU "1023" SIBLINGS, "902,1023\n"},
};

TB_TEST(platform_linux_processors, a_processor_of_several_digits_has_its_own_file)
{
    machine described = {.files = numbers_of_several_digits,
                         .file_count = TB_TEST_COUNT_OF(numbers_of_several_digits)};

    expect_counts(&described, "7,10,45,130,902,1023\n", 6, 3, 3);
}

/* The two processors of a core have the core's number, though they are four
 * apart in the system's numbering, and the cores are numbered in the order
 * the list reaches them. With only a part allowed, the numbers close up. */
TB_TEST(platform_linux_processors, the_processors_are_listed_with_their_cores)
{
    machine described = {.files = four_paired_cores,
                         .file_count = TB_TEST_COUNT_OF(four_paired_cores)};
    static const tb_linux_processor all[] = {
        {0, 0, true}, {1, 1, true}, {2, 2, true}, {3, 3, true},
        {4, 0, true}, {5, 1, true}, {6, 2, true}, {7, 3, true},
    };
    static const tb_linux_processor some[] = {{2, 0, true}, {5, 1, true}, {6, 0, true}};

    TB_TEST_CONTEXT("every processor allowed");
    expect_list(&described, "0-7\n", all, TB_TEST_COUNT_OF(all));
    TB_TEST_CONTEXT("three processors on two cores");
    expect_list(&described, "2,5-6\n", some, TB_TEST_COUNT_OF(some));
}

TB_TEST(platform_linux_processors, the_list_says_which_processors_are_of_the_fast_kind)
{
    machine capacities = {.files = three_capacities,
                          .file_count = TB_TEST_COUNT_OF(three_capacities)};
    static const tb_linux_processor without_the_largest[] = {
        {3, 0, false}, {4, 1, true}, {6, 2, true}};
    tb_linux_processor list[2] = {{0}};
    tb_linux_cpu_set allowed = {0};

    expect_list(&capacities, "3-4,6\n", without_the_largest, TB_TEST_COUNT_OF(without_the_largest));
    /* Room for fewer than there are: the first are written, all are counted. */
    TB_TEST_REQUIRE(tb_linux_cpu_list_parse("0-7\n", &allowed));
    TB_TEST_EXPECT_UINT(tb_linux_list_processors(&allowed, read_machine_file, &capacities, list, 2),
                        ==, 8);
    TB_TEST_EXPECT_UINT(list[1].cpu, ==, 1);
    TB_TEST_EXPECT_UINT(tb_linux_list_processors(&allowed, read_machine_file, &capacities, NULL, 0),
                        ==, 8);
}
