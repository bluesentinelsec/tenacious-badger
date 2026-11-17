/* The count of processors is of those that the process may run on. The
 * counting is tested against described machines in processors_test.c; this
 * confines the real process and asks the real system, which covers the part
 * that reads the system's set and the kernel's files. The case runs on the
 * first thread of the process, which is the one the count describes, and
 * gives it back the processors it had. */
#define _GNU_SOURCE

#include "badger/system.h"

#include "tb_test.h"

#include <sched.h>
#include <stddef.h>
#include <unistd.h>

/* Confines the first thread to `set` and returns the counts it then gets. */
static tb_system_processors count_when_confined_to(const cpu_set_t *set)
{
    tb_system_processors processors = {0};

    if (sched_setaffinity(getpid(), sizeof(*set), set) != 0)
    {
        TB_TEST_FAIL("sched_setaffinity failed");
    }
    tb_system_get_processors(&processors);
    return processors;
}

TB_TEST(platform_linux_system, a_confined_process_gets_the_counts_of_its_processors)
{
    cpu_set_t original = {0};
    cpu_set_t first = {0};
    cpu_set_t first_two = {0};
    size_t chosen_count = 0;
    tb_system_processors whole = {0};
    tb_system_processors one = {0};
    tb_system_processors two = {0};
    tb_system_processors restored = {0};

    CPU_ZERO(&original);
    CPU_ZERO(&first);
    CPU_ZERO(&first_two);
    if (sched_getaffinity(getpid(), sizeof(original), &original) != 0)
    {
        TB_TEST_SKIP("the machine has more processors than the system's fixed set holds");
    }
    /* The two lowest processors that the process may use, or the one. */
    for (size_t cpu = 0; cpu < (size_t)CPU_SETSIZE && chosen_count < 2; cpu++)
    {
        if (!CPU_ISSET(cpu, &original))
        {
            continue;
        }
        if (chosen_count == 0)
        {
            CPU_SET(cpu, &first);
        }
        CPU_SET(cpu, &first_two);
        chosen_count++;
    }
    tb_system_get_processors(&whole);

    one = count_when_confined_to(&first);
    two = count_when_confined_to(&first_two);
    restored = count_when_confined_to(&original);

    TB_TEST_EXPECT_UINT(whole.logical_count, ==, (size_t)CPU_COUNT(&original));

    TB_TEST_EXPECT_UINT(one.logical_count, ==, 1);
    TB_TEST_EXPECT_UINT(one.core_count, ==, 1);
    TB_TEST_EXPECT_UINT(one.performance_core_count, ==, 1);

    /* Two logical processors are one core or two, which the machine decides. */
    TB_TEST_EXPECT_UINT(two.logical_count, ==, chosen_count);
    TB_TEST_EXPECT_UINT(two.core_count, >=, 1);
    TB_TEST_EXPECT_UINT(two.core_count, <=, two.logical_count);
    TB_TEST_EXPECT_UINT(two.performance_core_count, >=, 1);
    TB_TEST_EXPECT_UINT(two.performance_core_count, <=, two.core_count);

    TB_TEST_EXPECT_UINT(restored.logical_count, ==, whole.logical_count);
    TB_TEST_EXPECT_UINT(restored.core_count, ==, whole.core_count);
    TB_TEST_EXPECT_UINT(restored.performance_core_count, ==, whole.performance_core_count);
}
