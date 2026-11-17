/* What a thread's priority becomes is each system's own. On Linux it is the
 * thread's niceness: 10 for a background thread, 0 for a normal one, and -5
 * for a high one where the process is allowed to raise a priority, which
 * most are not. The thread reads its niceness from the system itself. */
#define _GNU_SOURCE

#include "badger/thread.h"

#include "tb_test.h"

#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct native_report
{
    int niceness;  /* getpriority() of the thread. */
    bool was_read; /* The system answered. */
} native_report;

static void report_what_the_system_says(void *argument)
{
    native_report *const report = argument;
    const id_t thread = (id_t)syscall(SYS_gettid);

    /* -1 is a niceness, so errno is what tells a failure. */
    errno = 0;
    report->niceness = getpriority(PRIO_PROCESS, thread);
    report->was_read = errno == 0;
}

static native_report report_of_a_thread_with(const tb_thread_options *options)
{
    native_report report = {0};
    tb_thread thread = {0};

    tb_thread_create_with_options(&thread, options, report_what_the_system_says, &report);
    tb_thread_join(&thread);
    return report;
}

TB_TEST(platform_linux_thread, each_priority_is_a_niceness)
{
    const tb_thread_options normal = {.priority = TB_THREAD_PRIORITY_NORMAL};
    const tb_thread_options background = {.priority = TB_THREAD_PRIORITY_BACKGROUND};
    const tb_thread_options high = {.priority = TB_THREAD_PRIORITY_HIGH};
    const native_report own = {.niceness = getpriority(PRIO_PROCESS, 0)};
    const native_report unasked = report_of_a_thread_with(NULL);
    const native_report at_normal = report_of_a_thread_with(&normal);
    const native_report at_background = report_of_a_thread_with(&background);
    const native_report at_high = report_of_a_thread_with(&high);

    TB_TEST_REQUIRE(unasked.was_read && at_normal.was_read);
    TB_TEST_REQUIRE(at_background.was_read && at_high.was_read);
    /* A thread that asks for nothing keeps what the thread of the case has. */
    TB_TEST_EXPECT_INT(unasked.niceness, ==, own.niceness);
    TB_TEST_EXPECT_INT(at_normal.niceness, ==, own.niceness);
    TB_TEST_EXPECT_INT(at_background.niceness, ==, 10);
    /* Raised where the process may raise it, and left alone where not. */
    if (at_high.niceness != own.niceness)
    {
        TB_TEST_EXPECT_INT(at_high.niceness, ==, -5);
    }
    /* The priority was the thread's alone: the thread of the case kept its own. */
    TB_TEST_EXPECT_INT(getpriority(PRIO_PROCESS, 0), ==, own.niceness);
}
