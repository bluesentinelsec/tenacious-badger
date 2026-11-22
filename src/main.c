#include "badger/cli.h"
#include "badger/core.h"
#include "badger/crash.h"
#include "badger/fmt.h"
#include "badger/process.h"
#include "badger/version.h"

#include "burrow_game.h"
#include "burrow_settings.h"

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>

/* The most --set options one command line may give. */
#define BURROW_MOST_VALUES 64

/* Whether this build is a developer build, which the build system says:
 * every configuration but Release and MinSizeRel. */
#ifndef BURROW_DEVELOPER
#define BURROW_DEVELOPER 1
#endif

/* The command line's choices, read by tb_cli_run() before a command runs. */
static const char *const burrow_modes[] = {"developer", "release"};
static size_t burrow_mode = BURROW_DEVELOPER ? 0u : 1u;
static bool burrow_reset = false;
static bool burrow_headless = false;
static const char *burrow_settings_file = NULL;
static const char *burrow_values[BURROW_MOST_VALUES] = {0};
static size_t burrow_value_count = 0;

static int burrow_load(burrow_settings *loaded)
{
    const burrow_settings_request request = {.developer = burrow_mode == 0,
                                             .reset = burrow_reset,
                                             .player_file = burrow_settings_file,
                                             .values = burrow_values,
                                             .value_count = burrow_value_count};

    return burrow_settings_load(&request, loaded);
}

static int burrow_finish(void)
{
    /* Formatting reports no failure; the stream does, once it is flushed. */
    if (!tb_fmt_flush())
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* What burrow does without a command: play. */
static int burrow_run(const tb_cli_outcome *outcome, void *context)
{
    burrow_settings loaded = {0};
    int status = EXIT_SUCCESS;

    (void)outcome;
    (void)context;
    status = burrow_load(&loaded);
    if (status != EXIT_SUCCESS)
    {
        return status;
    }
    status = burrow_play(&loaded, burrow_headless);
    burrow_settings_release(&loaded);
    return status;
}

/* `burrow check`: the bootstrap's check that it runs against the engine, with
 * its settings loaded, which a packaged export runs to prove it starts. */
static int burrow_run_check(const tb_cli_outcome *outcome, void *context)
{
    burrow_settings loaded = {0};
    int32_t sum = {0};
    int status = EXIT_SUCCESS;

    (void)outcome;
    (void)context;
    status = burrow_load(&loaded);
    if (status != EXIT_SUCCESS)
    {
        return status;
    }
    burrow_settings_release(&loaded);
    /* coverage-off: 2 + 3 cannot overflow; the result of a fallible function is checked all the
     * same */
    if (!tb_sum_i32(2, 3, &sum))
    {
        (void)tb_fmt_print_error("burrow: bootstrap sum failed\n");
        return EXIT_FAILURE;
    }
    /* coverage-on */
    (void)tb_fmt_print("Tenacious Badger %s: 2 + 3 = %" PRId32 "\n", TB_VERSION_STRING, sum);
    return burrow_finish();
}

/* `burrow settings`: every setting, its value, and where the value came from. */
static int burrow_run_settings(const tb_cli_outcome *outcome, void *context)
{
    burrow_settings loaded = {0};
    int status = EXIT_SUCCESS;

    (void)outcome;
    (void)context;
    status = burrow_load(&loaded);
    if (status != EXIT_SUCCESS)
    {
        return status;
    }
    burrow_settings_list(&loaded);
    burrow_settings_release(&loaded);
    return burrow_finish();
}

static const tb_cli_option burrow_options[] = {
    {.long_name = "set",
     .name = "NAME=VALUE",
     .texts = burrow_values,
     .capacity = BURROW_MOST_VALUES,
     .count = &burrow_value_count,
     .help = "Give a setting a value for this session, over the settings files; may be given "
             "more than once. `burrow settings` lists them."},
    {.long_name = "reset-settings",
     .flag = &burrow_reset,
     .help = "Start from the default settings, setting the player's settings file aside as "
             "settings.cfg.bak, for when its settings leave the game unusable."},
    {.long_name = "settings-file",
     .name = "FILE",
     .text = &burrow_settings_file,
     .help = "Use this settings file in place of the player's usual one."},
    {.long_name = "headless",
     .flag = &burrow_headless,
     .help = "Play without a window, as a server or a test does; --set "
             "developer.frame_count=N stops it after N frames."},
    {.long_name = "mode",
     .choice = &burrow_mode,
     .choices = burrow_modes,
     .choice_count = TB_CLI_COUNT_OF(burrow_modes),
     .help = "Behave as a developer build (start in a window, quit on Escape) or a release build "
             "(start fullscreen, ask before quitting); the default is the build's own."},
};

static const tb_cli_command burrow_commands[] = {
    {.name = "check",
     .summary = "Check that Burrow starts against its engine, and say so.",
     .run = burrow_run_check},
    {.name = "settings",
     .summary = "List every setting, its value, and where the value comes from.",
     .run = burrow_run_settings},
};

static const tb_cli_command burrow = {
    .name = "burrow",
    .version = TB_VERSION_STRING,
    .description = "The showcase game of the Tenacious Badger engine: a graybox first-person "
                   "shooter, under construction.",
    .options = burrow_options,
    .option_count = TB_CLI_COUNT_OF(burrow_options),
    .commands = burrow_commands,
    .command_count = TB_CLI_COUNT_OF(burrow_commands),
    .command_optional = true,
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
