/* The count of processors is of those that the process may run on. The
 * counting is tested against described machines in processors_test.c; this
 * confines the real process and asks the real system, which covers the part
 * that reads the system's answer. The process is given back what it had. */
#include "badger/system.h"

#include "tb_test.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>

static size_t count_bits(DWORD_PTR mask)
{
    size_t count = 0;

    while (mask != 0)
    {
        mask &= mask - 1;
        count++;
    }
    return count;
}

static DWORD_PTR lowest_bit(DWORD_PTR mask)
{
    return mask & (~mask + 1);
}

/* Confines the process to `mask` and returns the counts it then gets. */
static tb_system_processors count_when_confined_to(DWORD_PTR mask)
{
    tb_system_processors processors = {0};

    if (!SetProcessAffinityMask(GetCurrentProcess(), mask))
    {
        TB_TEST_FAIL("SetProcessAffinityMask(%#llx) failed", (unsigned long long)mask);
    }
    tb_system_get_processors(&processors);
    return processors;
}

TB_TEST(platform_win32_system, a_confined_process_gets_the_counts_of_its_processors)
{
    const HANDLE process = GetCurrentProcess();
    DWORD_PTR original = 0;
    DWORD_PTR available = 0;
    DWORD_PTR first = 0;
    DWORD_PTR first_two = 0;
    tb_system_processors whole = {0};
    tb_system_processors one = {0};
    tb_system_processors two = {0};
    tb_system_processors restored = {0};

    if (!GetProcessAffinityMask(process, &original, &available) || original == 0)
    {
        TB_TEST_SKIP("the process spans processor groups and has no one mask");
    }
    tb_system_get_processors(&whole);
    first = lowest_bit(original);
    first_two = first | lowest_bit(original & ~first);

    one = count_when_confined_to(first);
    two = count_when_confined_to(first_two);
    restored = count_when_confined_to(original);

    TB_TEST_EXPECT_UINT(whole.logical_count, ==, count_bits(original));

    TB_TEST_EXPECT_UINT(one.logical_count, ==, 1);
    TB_TEST_EXPECT_UINT(one.core_count, ==, 1);
    TB_TEST_EXPECT_UINT(one.performance_core_count, ==, 1);

    /* Two logical processors are one core or two, which the machine decides. */
    TB_TEST_EXPECT_UINT(two.logical_count, ==, count_bits(first_two));
    TB_TEST_EXPECT_UINT(two.core_count, >=, 1);
    TB_TEST_EXPECT_UINT(two.core_count, <=, two.logical_count);
    TB_TEST_EXPECT_UINT(two.performance_core_count, >=, 1);
    TB_TEST_EXPECT_UINT(two.performance_core_count, <=, two.core_count);

    TB_TEST_EXPECT_UINT(restored.logical_count, ==, whole.logical_count);
    TB_TEST_EXPECT_UINT(restored.core_count, ==, whole.core_count);
    TB_TEST_EXPECT_UINT(restored.performance_core_count, ==, whole.performance_core_count);
}
