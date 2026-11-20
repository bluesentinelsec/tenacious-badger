/* Crashes on purpose, after tb_crash_catch(), so that the report of each can
 * be checked. ExpectCrashReport.cmake runs these cases as child processes
 * and checks how they end; they are not registered with CTest directly. */
#include "badger/crash.h"

#include "badger/fatal.h"
#include "badger/fmt.h"
#include "badger/log.h"
#include "badger/thread.h"
#include "tb_test.h"

#include <signal.h>
#include <stdlib.h>

/* Read each time, so that the compiler cannot see that it is null. */
static int *volatile crash_probe_nowhere = NULL;

/* Not static, so that a platform which names only what a program keeps the
 * names of can name them. */
void crash_probe_write_through_null(void);
void crash_probe_abort(void);

void crash_probe_write_through_null(void)
{
    *crash_probe_nowhere = 1;
}

void crash_probe_abort(void)
{
    abort();
}

/* Called through pointers that the compiler must read each time, so that it
 * keeps the line after the call: the line that must never run. */
static void (*volatile write_through_null)(void) = crash_probe_write_through_null;
static void (*volatile abort_now)(void) = crash_probe_abort;

static void begin(void)
{
    tb_crash_catch();
    /* This program may have a C library of its own, beside the engine's. */
    (void)signal(SIGABRT, tb_crash_report_abort);
    (void)tb_fmt_print("before-crash\n");
    (void)tb_fmt_flush();
}

/* As begin(), and asks for crash dumps in the system's temporary directory,
 * saying whether the platform writes them. */
static void begin_with_dumps(void)
{
    tb_crash_catch();
    (void)signal(SIGABRT, tb_crash_report_abort);
    if (tb_crash_write_dumps(NULL))
    {
        (void)tb_fmt_print("dumps: yes\n");
    }
    else
    {
        (void)tb_fmt_print("dumps: no\n");
    }
    (void)tb_fmt_print("before-crash\n");
    (void)tb_fmt_flush();
}

TB_TEST(crash_probe, a_null_pointer_is_reported_with_its_stack)
{
    begin();
    write_through_null();
    (void)tb_fmt_print("after-crash\n");
}

TB_TEST(crash_probe, abort_is_reported_with_its_stack)
{
    begin();
    abort_now();
    (void)tb_fmt_print("after-crash\n");
}

static void crash_on_a_thread(void *context)
{
    (void)context;
    write_through_null();
}

TB_TEST(crash_probe, a_crash_on_another_thread_is_reported)
{
    tb_thread thread = {0};

    begin();
    tb_thread_create(&thread, crash_on_a_thread, NULL);
    tb_thread_join(&thread);
    (void)tb_fmt_print("after-crash\n");
}

static bool crash_in_the_sink(void *context, const tb_log_record *record)
{
    (void)context;
    (void)record;
    write_through_null();
    return true;
}

/* A log writes to its sink holding the log's lock: the report must not
 * need it, nor anything else the crashed thread holds. */
TB_TEST(crash_probe, a_crash_holding_the_engines_lock_is_reported)
{
    tb_log *const log = tb_log_create();
    const tb_log_config config = {TB_LOG_INFO, NULL, crash_in_the_sink, NULL, NULL};

    TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    begin();
    (void)tb_log_printf(log, TB_LOG_INFO, "probe", "crash now");
    (void)tb_fmt_print("after-crash\n");
}

/* A fatal condition of the engine's own is reported once, as itself, and
 * not again as the crash that ending the process might look like. */
TB_TEST(crash_probe, a_fatal_condition_is_reported_once)
{
    begin();
    tb_fatal("probe: fatal after catching crashes");
}

TB_TEST(crash_probe, a_crash_is_dumped_where_the_platform_has_dumps)
{
    begin_with_dumps();
    write_through_null();
    (void)tb_fmt_print("after-crash\n");
}

TB_TEST(crash_probe, abort_is_dumped_where_the_platform_has_dumps)
{
    begin_with_dumps();
    abort_now();
    (void)tb_fmt_print("after-crash\n");
}
