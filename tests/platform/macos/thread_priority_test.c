/* What a thread's priority becomes is each system's own. On macOS it is a
 * quality-of-service class: utility for a background thread and
 * user-interactive for a high one. The class also decides, on a machine with
 * two kinds of core, which kind the thread runs on. The thread reads its
 * class from the system itself. */
#define _DARWIN_C_SOURCE

#include "badger/thread.h"

#include "tb_test.h"

#include <pthread/qos.h>

typedef struct native_report
{
    qos_class_t quality; /* qos_class_self() of the thread. */
} native_report;

static void report_what_the_system_says(void *argument)
{
    native_report *const report = argument;

    report->quality = qos_class_self();
}

static native_report report_of_a_thread_with(const tb_thread_options *options)
{
    native_report report = {0};
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, options, report_what_the_system_says, &report);
    tb_thread_join(&thread);
    return report;
}

TB_TEST(platform_macos_thread, each_priority_is_a_quality_of_service_class)
{
    const tb_thread_options normal = {.priority = TB_THREAD_PRIORITY_NORMAL};
    const tb_thread_options background = {.priority = TB_THREAD_PRIORITY_BACKGROUND};
    const tb_thread_options high = {.priority = TB_THREAD_PRIORITY_HIGH};
    const native_report unasked = report_of_a_thread_with(NULL);
    const native_report at_normal = report_of_a_thread_with(&normal);
    const native_report at_background = report_of_a_thread_with(&background);
    const native_report at_high = report_of_a_thread_with(&high);

    /* A thread that asks for nothing is left with what the system gave it. */
    TB_TEST_EXPECT_UINT(at_normal.quality, ==, unasked.quality);
    TB_TEST_EXPECT_UINT(at_background.quality, ==, QOS_CLASS_UTILITY);
    TB_TEST_EXPECT_UINT(at_high.quality, ==, QOS_CLASS_USER_INTERACTIVE);
}
