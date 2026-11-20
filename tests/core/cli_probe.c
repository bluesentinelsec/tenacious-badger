/* tb_cli_finish() writes to the standard streams, which a case of tb_tests
 * cannot read. Each case here finishes one outcome and checks the status it
 * returns; tests/ExpectCommandLine.cmake runs them in a child process and
 * checks what reached standard output and standard error. */
#include "badger/cli.h"

#include "tb_test.h"

#include <stdlib.h>

static bool verbose = false;

static const tb_cli_option options[] = {
    {.short_name = 'v', .long_name = "verbose", .flag = &verbose, .help = "Say more."},
};

static const tb_cli_option deploy_options[] = {
    {.long_name = "dry-run", .flag = &verbose, .help = "Only say what would be done."},
};

static const tb_cli_command commands[] = {
    {.name = "deploy",
     .summary = "Deploy it.",
     .options = deploy_options,
     .option_count = TB_CLI_COUNT_OF(deploy_options)},
};

static const tb_cli_command program = {
    .name = "probe",
    .version = "9.8.7",
    .options = options,
    .option_count = TB_CLI_COUNT_OF(options),
    .commands = commands,
    .command_count = TB_CLI_COUNT_OF(commands),
};

static int finish(size_t count, const char *const *arguments, tb_cli_result expected)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    TB_TEST_REQUIRE_UINT(tb_cli_parse(&program, count, arguments, &outcome), ==, expected);
    return tb_cli_finish(&outcome);
}

TB_TEST(cli_probe, help_goes_to_standard_output)
{
    const char *line[] = {"probe", "--help"};
    TB_TEST_EXPECT_INT(finish(2, line, TB_CLI_HELP), ==, EXIT_SUCCESS);
}

TB_TEST(cli_probe, version_goes_to_standard_output)
{
    const char *line[] = {"probe", "deploy", "--version"};
    TB_TEST_EXPECT_INT(finish(3, line, TB_CLI_VERSION), ==, EXIT_SUCCESS);
}

TB_TEST(cli_probe, an_error_goes_to_standard_error_with_the_usage)
{
    const char *line[] = {"probe", "deploy", "--dry-rn"};
    TB_TEST_EXPECT_INT(finish(3, line, TB_CLI_ERROR), ==, TB_CLI_EXIT_USAGE);
}
