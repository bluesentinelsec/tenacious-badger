#include "badger/cli.h"
#include "badger/core.h"
#include "badger/crash.h"
#include "badger/fmt.h"
#include "badger/process.h"
#include "badger/version.h"

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>

/* What burrow does without a command: the bootstrap's check that it runs
 * against the engine. */
static int burrow_run(const tb_cli_outcome *outcome, void *context)
{
    int32_t sum = {0};
    (void)outcome;
    (void)context;
    /* coverage-off: 2 + 3 cannot overflow; the result of a fallible function is checked all the
     * same */
    if (!tb_sum_i32(2, 3, &sum))
    {
        (void)tb_fmt_print_error("burrow: bootstrap sum failed\n");
        return EXIT_FAILURE;
    }
    /* coverage-on */
    (void)tb_fmt_print("Tenacious Badger %s: 2 + 3 = %" PRId32 "\n", TB_VERSION_STRING, sum);
    /* Formatting reports no failure; the stream does, once it is flushed. */
    if (!tb_fmt_flush())
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static const tb_cli_command burrow = {
    .name = "burrow",
    .version = TB_VERSION_STRING,
    .description = "The showcase game of the Tenacious Badger engine: a graybox first-person "
                   "shooter, under construction.",
    .run = burrow_run,
};

int main(int count, char **values)
{
    tb_arguments arguments = TB_ARGUMENTS_INIT;
    int status = EXIT_FAILURE;

    /* A crash is reported with its stack, wherever it happens. Burrow has a C
     * library of its own beside the engine's, so abort() is caught here too. */
    tb_crash_catch();
    (void)signal(SIGABRT, tb_crash_report_abort);
    /* Where the platform writes crash dumps, a crash also leaves one in the
     * temporary directory, for a debugger; elsewhere the system's core files
     * serve. */
    (void)tb_crash_write_dumps(NULL);
    /* The arguments as UTF-8, which on Windows main does not receive. */
    /* coverage-off: only a system out of memory cannot give them */
    if (!tb_process_get_arguments(count, values, &arguments))
    {
        (void)tb_fmt_print_error("burrow: the command line could not be read\n");
        return EXIT_FAILURE;
    }
    /* coverage-on */
    status = tb_cli_run(&burrow, arguments.count, arguments.values, NULL);
    tb_process_release_arguments(&arguments);
    return status;
}
