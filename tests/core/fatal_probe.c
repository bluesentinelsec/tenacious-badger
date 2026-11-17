/* Meets a fatal condition with the engine's default handling, which writes
 * the report to stderr and ends the process. ExpectFatalAbort.cmake runs
 * these cases as child processes and checks how they end; they are not
 * registered with CTest directly. */
#include "badger/fatal.h"

#include "badger/fmt.h"
#include "tb_test.h"

/* Not static, so that a platform which names only what a program keeps the
 * names of can name it. */
void fatal_probe_meet_a_condition(int value);

void fatal_probe_meet_a_condition(int value)
{
    tb_fatal("probe: the value was %d\n", value);
}

/* Called through a pointer that the compiler must read each time, so that it
 * cannot see that the call does not return, and keeps the line after it:
 * the line that must never run. */
static void (*volatile meet_a_condition)(int value) = fatal_probe_meet_a_condition;

TB_TEST(fatal_probe, the_default_writes_the_report_and_ends_the_process)
{
    (void)tb_fmt_print("before-fatal\n");
    (void)tb_fmt_flush();
    meet_a_condition(42);
    (void)tb_fmt_print("after-fatal\n");
}

static void add_a_line_and_return(const tb_fatal_report *report, void *context)
{
    (void)context;
    (void)tb_fmt_print_error("handler-saw: %s\n", report->message);
    tb_fatal_write(report);
}

/* A handler that adds to the default report and then returns, which it must
 * not: the process ends all the same. */
TB_TEST(fatal_probe, a_handler_that_returns_does_not_keep_the_process_alive)
{
    tb_fatal_set_handler(add_a_line_and_return, NULL);
    (void)tb_fmt_print("before-fatal\n");
    (void)tb_fmt_flush();
    meet_a_condition(7);
    (void)tb_fmt_print("after-fatal\n");
}
