/* What a thread's priority becomes is each system's own. On Windows the
 * three engine priorities are the system's normal priority and the ones just
 * below and above it, and a thread's stack is as large as it asked for. The
 * thread reads both from the system itself. */
#include "badger/thread.h"

#include "tb_test.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>

typedef struct native_report
{
    int priority;      /* GetThreadPriority() of the thread. */
    size_t stack_size; /* Bytes between the limits of its stack. */
} native_report;

static void report_what_the_system_says(void *argument)
{
    native_report *const report = argument;
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;

    report->priority = GetThreadPriority(GetCurrentThread());
    GetCurrentThreadStackLimits(&low, &high);
    report->stack_size = (size_t)(high - low);
}

static native_report report_of_a_thread_with(const tb_thread_options *options)
{
    native_report report = {0};
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, options, report_what_the_system_says, &report);
    tb_thread_join(&thread);
    return report;
}

TB_TEST(platform_win32_thread, each_priority_is_the_systems_nearest)
{
    const tb_thread_options normal = {.priority = TB_THREAD_PRIORITY_NORMAL};
    const tb_thread_options background = {.priority = TB_THREAD_PRIORITY_BACKGROUND};
    const tb_thread_options high = {.priority = TB_THREAD_PRIORITY_HIGH};

    TB_TEST_EXPECT_INT(report_of_a_thread_with(NULL).priority, ==, THREAD_PRIORITY_NORMAL);
    TB_TEST_EXPECT_INT(report_of_a_thread_with(&normal).priority, ==, THREAD_PRIORITY_NORMAL);
    TB_TEST_EXPECT_INT(report_of_a_thread_with(&background).priority, ==,
                       THREAD_PRIORITY_BELOW_NORMAL);
    TB_TEST_EXPECT_INT(report_of_a_thread_with(&high).priority, ==, THREAD_PRIORITY_ABOVE_NORMAL);
}

#define KIBIBYTE ((size_t)1024)
#define MEBIBYTE (KIBIBYTE * KIBIBYTE)

/* The system rounds a stack up to its allocation granularity, 64 KiB. */
#define STACK_ROUNDING (64 * KIBIBYTE)

TB_TEST(platform_win32_thread, a_stack_is_as_large_as_was_asked_and_no_larger)
{
    const size_t sizes[] = {256 * KIBIBYTE, TB_THREAD_STACK_SIZE_DEFAULT, 4 * MEBIBYTE,
                            16 * MEBIBYTE};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        const tb_thread_options options = {.stack_size = sizes[index]};
        const native_report report = report_of_a_thread_with(&options);

        TB_TEST_CONTEXT("asked for %zu bytes", sizes[index]);
        TB_TEST_EXPECT_UINT(report.stack_size, >=, sizes[index]);
        TB_TEST_EXPECT_UINT(report.stack_size, <=, sizes[index] + STACK_ROUNDING);
    }
}
