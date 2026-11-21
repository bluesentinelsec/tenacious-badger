/* Command lines read into a program's variables, the help written for them,
 * and the errors a wrong one gets. A user meets these messages, so the tests
 * compare them whole. */
#include "badger/assert.h"
#include "badger/cli.h"
#include "badger/str.h"
#include "badger/utf8.h"

#include "tb_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * A program to read command lines with: "tool", with options of every kind,
 * and the commands "build" and "clean".
 * ------------------------------------------------------------------------- */

typedef struct values
{
    bool verbose;
    bool quiet;
    size_t verbosity;
    const char *output;
    int64_t jobs;
    double scale;
    size_t mode;
    const char *defines[3];
    size_t define_count;
    bool output_given;
    const char *target;
    const char *extra;
    const char *files[4];
    size_t file_count;
    bool release;
} values;

static values given;

static const char *const modes[] = {"fast", "small", "debug"};

static const tb_cli_option build_options[] = {
    {.long_name = "release", .flag = &given.release, .help = "Build optimized."},
    {.short_name = 'v',
     .long_name = "verbose",
     .counter = &given.verbosity,
     .help = "Say more; give it twice to say everything."},
    {.name = "TARGET", .text = &given.target, .required = true, .help = "What to build."},
    {.name = "FILE",
     .texts = given.files,
     .capacity = 4,
     .count = &given.file_count,
     .help = "Sources to build it from."},
};

static int run_build(const tb_cli_outcome *outcome, void *context)
{
    (void)outcome;
    *(int *)context += 1;
    return 7;
}

static const tb_cli_command commands[] = {
    {.name = "build",
     .summary = "Build a target.",
     .options = build_options,
     .option_count = TB_CLI_COUNT_OF(build_options),
     .run = run_build},
    {.name = "clean", .summary = "Remove what was built."},
};

static const tb_cli_option tool_options[] = {
    {.short_name = 'v', .long_name = "verbose", .flag = &given.verbose, .help = "Say more."},
    {.short_name = 'q', .flag = &given.quiet, .help = "Say less."},
    {.short_name = 'o',
     .long_name = "output",
     .text = &given.output,
     .name = "FILE",
     .given = &given.output_given,
     .help = "Where to write the result."},
    {.short_name = 'j',
     .long_name = "jobs",
     .integer = &given.jobs,
     .minimum = 1,
     .maximum = 64,
     .name = "N",
     .help = "Run N jobs at once."},
    {.long_name = "scale", .number = &given.scale, .name = "FACTOR", .help = "Scale by FACTOR."},
    {.short_name = 'm',
     .long_name = "mode",
     .choice = &given.mode,
     .choices = modes,
     .choice_count = TB_CLI_COUNT_OF(modes),
     .help = "How to build."},
    {.short_name = 'D',
     .long_name = "define",
     .texts = given.defines,
     .capacity = 3,
     .count = &given.define_count,
     .name = "NAME",
     .help = "Define NAME."},
};

static const tb_cli_command tool = {
    .name = "tool",
    .version = "1.2.3",
    .description = "Builds things.\n\nGive it a command; each has a help of its own.",
    .epilog = "See the manual for more.",
    .options = tool_options,
    .option_count = TB_CLI_COUNT_OF(tool_options),
    .commands = commands,
    .command_count = TB_CLI_COUNT_OF(commands),
};

/* The defaults of every test: what the program starts with. */
static void reset(void)
{
    const values defaults = {.jobs = 4, .scale = 1.5, .mode = 0};
    given = defaults;
}

/* Parses "tool" and the arguments given, which end with NULL. */
static tb_cli_result parse_arguments(const tb_cli_command *program, tb_cli_outcome *outcome,
                                     const char *const *arguments)
{
    const char *line[16] = {"tool"};
    size_t count = 1;
    while (arguments[count - 1] != NULL)
    {
        TB_TEST_REQUIRE(count < TB_TEST_COUNT_OF(line));
        line[count] = arguments[count - 1];
        count++;
    }
    return tb_cli_parse(program, count, line, outcome);
}

#define PARSE(outcome, ...)                                                                        \
    parse_arguments(&tool, (outcome), (const char *const[]){__VA_ARGS__, NULL})
#define PARSE_NOTHING(outcome) parse_arguments(&tool, (outcome), (const char *const[]){NULL})

/* Collects written text. */
typedef struct collected
{
    char text[8192];
    size_t length;
} collected;

static void collect(void *context, const char *bytes, size_t length)
{
    collected *const into = context;
    TB_TEST_REQUIRE(into->length + length < sizeof(into->text));
    memcpy(into->text + into->length, bytes, length);
    into->length += length;
    into->text[into->length] = '\0';
}

/* -------------------------------------------------------------------------
 * Options.
 * ------------------------------------------------------------------------- */

TB_TEST(core_cli, nothing_given_leaves_every_default_and_needs_a_command)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE_NOTHING(&outcome), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "a command is required (build, clean)");
    TB_TEST_EXPECT_INT(given.jobs, ==, 4);
    TB_TEST_EXPECT(!given.verbose && given.output == NULL);
}

TB_TEST(core_cli, flags_are_set_by_short_long_and_grouped_names)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "-vq", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT(given.verbose && given.quiet);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "");

    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "--verbose", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT(given.verbose && !given.quiet);
}

TB_TEST(core_cli, a_value_is_taken_in_every_form)
{
    static const char *const forms[][2] = {
        {"-j8", NULL}, {"-j", "8"}, {"--jobs=8", NULL}, {"--jobs", "8"}};

    for (size_t form = 0; form < TB_TEST_COUNT_OF(forms); form++)
    {
        tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
        tb_cli_result result = TB_CLI_OK;
        TB_TEST_CONTEXT("form %zu", form);
        reset();
        if (forms[form][1] == NULL)
        {
            result = PARSE(&outcome, forms[form][0], "clean");
        }
        else
        {
            result = PARSE(&outcome, forms[form][0], forms[form][1], "clean");
        }
        TB_TEST_EXPECT_UINT(result, ==, TB_CLI_OK);
        TB_TEST_EXPECT_INT(given.jobs, ==, 8);
    }
}

TB_TEST(core_cli, text_is_borrowed_from_the_arguments_and_marked_given)
{
    const char *line[] = {"tool", "-qofile.txt", "clean"};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(tb_cli_parse(&tool, 3, line, &outcome), ==, TB_CLI_OK);
    TB_TEST_EXPECT(given.quiet);
    TB_TEST_EXPECT_POINTER(given.output, ==, line[1] + 3);
    TB_TEST_EXPECT(given.output_given);
}

/* An option's value may not look like another option; written with '=' it
 * may be anything. */
TB_TEST(core_cli, a_value_that_looks_like_an_option_must_be_joined_with_equals)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--output", "--verbose", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--output' needs a value (FILE)");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "-o"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--output' needs a value (FILE)");

    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "--output=--verbose", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_STRING(given.output, ==, "--verbose");
    TB_TEST_EXPECT(!given.verbose);
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "-o", "-", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_STRING(given.output, ==, "-");
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "--output=", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_STRING(given.output, ==, "");
}

TB_TEST(core_cli, integers_are_decimal_or_hexadecimal_and_within_their_range)
{
    static const struct
    {
        const char *text;
        int64_t value;
    } accepted[] = {{"1", 1}, {"+64", 64}, {"0x20", 32}, {"0X3f", 63}, {"007", 7}};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(accepted); index++)
    {
        TB_TEST_CONTEXT("%s", accepted[index].text);
        reset();
        TB_TEST_EXPECT_UINT(PARSE(&outcome, "-j", accepted[index].text, "clean"), ==, TB_CLI_OK);
        TB_TEST_EXPECT_INT(given.jobs, ==, accepted[index].value);
    }
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jobs=65", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--jobs': 65 is not between 1 and 64");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "-j", "-3", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--jobs': -3 is not between 1 and 64");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jobs=4x", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--jobs': '4x' is not an integer");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jobs=", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jobs=0x", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jobs=-", "clean"), ==, TB_CLI_ERROR);
}

/* Without a range, every int64_t is accepted, and nothing beyond. */
TB_TEST(core_cli, an_integer_without_a_range_takes_all_of_int64)
{
    int64_t value = 0;
    const tb_cli_option options[] = {{.name = "N", .integer = &value}};
    const tb_cli_command program = {.name = "count", .options = options, .option_count = 1};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;

    TB_TEST_EXPECT_UINT(
        parse_arguments(&program, &outcome, (const char *const[]){"-9223372036854775808", NULL}),
        ==, TB_CLI_OK);
    TB_TEST_EXPECT_INT(value, ==, INT64_MIN);
    TB_TEST_EXPECT_UINT(
        parse_arguments(&program, &outcome, (const char *const[]){"9223372036854775807", NULL}), ==,
        TB_CLI_OK);
    TB_TEST_EXPECT_INT(value, ==, INT64_MAX);
    TB_TEST_EXPECT_UINT(
        parse_arguments(&program, &outcome, (const char *const[]){"0x7fffffffffffffff", NULL}), ==,
        TB_CLI_OK);
    TB_TEST_EXPECT_INT(value, ==, INT64_MAX);
    TB_TEST_EXPECT_UINT(
        parse_arguments(&program, &outcome, (const char *const[]){"9223372036854775808", NULL}), ==,
        TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==,
                          "argument N: '9223372036854775808' is not an integer");
    TB_TEST_EXPECT_UINT(
        parse_arguments(&program, &outcome, (const char *const[]){"-9223372036854775809", NULL}),
        ==, TB_CLI_ERROR);
}

TB_TEST(core_cli, numbers_are_finite_decimals)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "--scale", "-0.25", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_FLOAT(given.scale, ==, -0.25);
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "--scale=2e3", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_FLOAT(given.scale, ==, 2000.0);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--scale=inf", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--scale': 'inf' is not a finite number");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--scale=1e999", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--scale=half", "clean"), ==, TB_CLI_ERROR);
}

TB_TEST(core_cli, a_choice_is_one_of_its_names_or_a_suggestion_is_made)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "-m", "debug", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_UINT(given.mode, ==, 2);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--mode=smal", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==,
                          "option '--mode': invalid choice 'smal'; did you mean 'small'?");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--mode=quick", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==,
                          "option '--mode': invalid choice 'quick' (choose from fast, small, "
                          "debug)");
    /* A choice is matched whole and in its case. */
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--mode=Fast", "clean"), ==, TB_CLI_ERROR);
}

TB_TEST(core_cli, a_repeated_option_collects_up_to_its_capacity)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "-DA", "--define", "B", "-D", "C", "clean"), ==,
                         TB_CLI_OK);
    TB_TEST_REQUIRE_UINT(given.define_count, ==, 3);
    TB_TEST_EXPECT_STRING(given.defines[0], ==, "A");
    TB_TEST_EXPECT_STRING(given.defines[1], ==, "B");
    TB_TEST_EXPECT_STRING(given.defines[2], ==, "C");

    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "-DA", "-DB", "-DC", "-DD", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--define': given more than 3 times");
}

TB_TEST(core_cli, a_repeated_single_option_keeps_the_last_value)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "-j2", "-j3", "clean"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_INT(given.jobs, ==, 3);
}

TB_TEST(core_cli, an_unknown_option_gets_a_suggestion_when_one_is_near)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jbos=4", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '--jbos'; did you mean '--jobs'?");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--verbos"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==,
                          "unknown option '--verbos'; did you mean '--verbose'?");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--hlep"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '--hlep'; did you mean '--help'?");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--colour"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '--colour'");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "-x"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '-x'");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "-v\xC3\xA9"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '-v\xC3\xA9'");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "---jobs"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '---jobs'; did you mean '--jobs'?");
}

TB_TEST(core_cli, an_option_without_a_value_refuses_one)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--verbose=yes", "clean"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--verbose' takes no value");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--help=yes"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "option '--help' takes no value");
}

/* -------------------------------------------------------------------------
 * Arguments and commands.
 * ------------------------------------------------------------------------- */

TB_TEST(core_cli, a_command_is_chosen_and_takes_its_arguments_in_order)
{
    const char *line[] = {"tool", "-v", "build", "game", "a.c", "--release", "b.c"};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(tb_cli_parse(&tool, TB_TEST_COUNT_OF(line), line, &outcome), ==,
                         TB_CLI_OK);
    TB_TEST_EXPECT_UINT(outcome.depth, ==, 2);
    TB_TEST_EXPECT_POINTER(outcome.path[0], ==, &tool);
    TB_TEST_EXPECT_POINTER(outcome.command, ==, &commands[0]);
    TB_TEST_EXPECT(given.verbose && given.release);
    TB_TEST_EXPECT_STRING(given.target, ==, "game");
    TB_TEST_REQUIRE_UINT(given.file_count, ==, 2);
    TB_TEST_EXPECT_POINTER(given.files[0], ==, line[4]);
    TB_TEST_EXPECT_POINTER(given.files[1], ==, line[6]);
}

/* A command's own option hides the program's of the same name; the
 * program's other options are still accepted after the command. */
TB_TEST(core_cli, a_commands_options_hide_the_programs_and_the_rest_are_accepted)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "build", "-vv", "-q", "-j2", "game"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_UINT(given.verbosity, ==, 2);
    TB_TEST_EXPECT(!given.verbose && given.quiet);
    TB_TEST_EXPECT_INT(given.jobs, ==, 2);
}

TB_TEST(core_cli, after_two_dashes_everything_is_an_argument)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "build", "--", "--release", "-", "-v"), ==, TB_CLI_OK);
    TB_TEST_EXPECT(!given.release);
    TB_TEST_EXPECT_STRING(given.target, ==, "--release");
    TB_TEST_REQUIRE_UINT(given.file_count, ==, 2);
    TB_TEST_EXPECT_STRING(given.files[0], ==, "-");
    TB_TEST_EXPECT_STRING(given.files[1], ==, "-v");
}

TB_TEST(core_cli, a_negative_number_is_an_argument)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "build", "-5", "-.5", "-1e3"), ==, TB_CLI_OK);
    TB_TEST_EXPECT_STRING(given.target, ==, "-5");
    TB_TEST_REQUIRE_UINT(given.file_count, ==, 2);
}

TB_TEST(core_cli, what_is_required_and_missing_is_named_all_at_once)
{
    const char *name = NULL;
    const char *file = NULL;
    const tb_cli_option options[] = {
        {.long_name = "name", .text = &name, .required = true},
        {.name = "FILE", .text = &file, .required = true},
    };
    const tb_cli_command program = {.name = "need", .options = options, .option_count = 2};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;

    TB_TEST_EXPECT_UINT(parse_arguments(&program, &outcome, (const char *const[]){NULL}), ==,
                        TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "the following are required: --name, FILE");

    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "build"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "the following are required: TARGET");
}

TB_TEST(core_cli, an_argument_too_many_is_unexpected)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "clean", "now"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unexpected argument 'now'");
}

TB_TEST(core_cli, an_unknown_command_gets_a_suggestion_when_one_is_near)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "biuld"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown command 'biuld'; did you mean 'build'?");
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "deploy"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown command 'deploy'");
}

TB_TEST(core_cli, a_program_may_run_without_a_command_when_it_says_so)
{
    tb_cli_command program = tool;
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    program.command_optional = true;
    reset();
    TB_TEST_EXPECT_UINT(parse_arguments(&program, &outcome, (const char *const[]){"-v", NULL}), ==,
                        TB_CLI_OK);
    TB_TEST_EXPECT_UINT(outcome.depth, ==, 1);
    TB_TEST_EXPECT_POINTER(outcome.command, ==, &program);
}

/* -------------------------------------------------------------------------
 * Help, version, and the program's name.
 * ------------------------------------------------------------------------- */

TB_TEST(core_cli, help_is_asked_for_at_any_point)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "-h"), ==, TB_CLI_HELP);
    TB_TEST_EXPECT_UINT(outcome.depth, ==, 1);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "build", "-vh"), ==, TB_CLI_HELP);
    TB_TEST_EXPECT_POINTER(outcome.command, ==, &commands[0]);
    /* Reading stops at the first thing that ends it: an error before the
     * help, or the help before an error. */
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--jobs=900", "--help"), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--help", "--jobs=900"), ==, TB_CLI_HELP);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "");
}

TB_TEST(core_cli, version_is_offered_only_by_a_program_with_one)
{
    const tb_cli_command program = {.name = "plain", .command_optional = true};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "--version"), ==, TB_CLI_VERSION);
    TB_TEST_EXPECT_UINT(PARSE(&outcome, "build", "--version"), ==, TB_CLI_VERSION);
    TB_TEST_EXPECT_UINT(
        parse_arguments(&program, &outcome, (const char *const[]){"--version", NULL}), ==,
        TB_CLI_ERROR);
    TB_TEST_EXPECT_STRING(outcome.message, ==, "unknown option '--version'");
}

TB_TEST(core_cli, a_program_without_a_name_takes_it_from_its_first_argument)
{
    static const struct
    {
        const char *first;
        const char *name;
    } cases[] = {
        {"C:\\Games\\burrow.EXE", "burrow"},
        {"/usr/local/bin/burrow", "burrow"},
        {"./burrow.exe.old", "burrow.exe.old"},
        {".exe", "program"},
        {"", "program"},
        {"bin/", "program"},
    };
    const tb_cli_command program = {.command_optional = true};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        const char *line[] = {cases[index].first};
        tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
        TB_TEST_CONTEXT("%s", cases[index].first);
        TB_TEST_REQUIRE_UINT(tb_cli_parse(&program, 1, line, &outcome), ==, TB_CLI_OK);
        TB_TEST_EXPECT_STRING(outcome.program, ==, cases[index].name);
    }
}

TB_TEST(core_cli, the_programs_help_lists_its_options_commands_and_defaults)
{
    static const char expected[] =
        "usage: tool [options] <command> ...\n"
        "\n"
        "Builds things.\n"
        "\n"
        "Give it a command; each has a help of its own.\n"
        "\n"
        "options:\n"
        "  -h, --help                  Show this help and exit.\n"
        "      --version               Show the version and exit.\n"
        "  -v, --verbose               Say more.\n"
        "  -q                          Say less.\n"
        "  -o, --output FILE           Where to write the result.\n"
        "  -j, --jobs N                Run N jobs at once. (1 to 64; default: 4)\n"
        "      --scale FACTOR          Scale by FACTOR. (default: 1.5)\n"
        "  -m, --mode {fast,small,debug}\n"
        "                              How to build. (default: fast)\n"
        "  -D, --define NAME           Define NAME. (up to 3 times)\n"
        "\n"
        "commands:\n"
        "  build                       Build a target.\n"
        "  clean                       Remove what was built.\n"
        "\n"
        "See the manual for more.\n"
        "\n";
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    collected help = {0};

    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "--help"), ==, TB_CLI_HELP);
    tb_cli_write_help(&outcome, collect, &help);
    TB_TEST_EXPECT_STRING(help.text, ==, expected);
}

TB_TEST(core_cli, a_commands_help_lists_the_options_of_the_program_too)
{
    static const char expected[] =
        "usage: tool build [options] TARGET [FILE...]\n"
        "\n"
        "Build a target.\n"
        "\n"
        "arguments:\n"
        "  TARGET                      What to build.\n"
        "  FILE                        Sources to build it from.\n"
        "\n"
        "options:\n"
        "  -h, --help                  Show this help and exit.\n"
        "      --release               Build optimized.\n"
        "  -v, --verbose               Say more; give it twice to say everything.\n"
        "\n"
        "options of tool:\n"
        "  -v, --verbose               Say more.\n"
        "  -q                          Say less.\n"
        "  -o, --output FILE           Where to write the result.\n"
        "  -j, --jobs N                Run N jobs at once. (1 to 64; default: 4)\n"
        "      --scale FACTOR          Scale by FACTOR. (default: 1.5)\n"
        "  -m, --mode {fast,small,debug}\n"
        "                              How to build. (default: fast)\n"
        "  -D, --define NAME           Define NAME. (up to 3 times)\n"
        "\n";
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    collected help = {0};

    reset();
    TB_TEST_REQUIRE_UINT(PARSE(&outcome, "build", "--help"), ==, TB_CLI_HELP);
    tb_cli_write_help(&outcome, collect, &help);
    TB_TEST_EXPECT_STRING(help.text, ==, expected);
}

/* Help wraps at 80 columns, under its own column, and a word too long for a
 * line stands alone on one. */
TB_TEST(core_cli, long_help_wraps_under_its_column)
{
    const char *long_word = NULL;
    const tb_cli_option options[] = {
        {.long_name = "word",
         .text = &long_word,
         .help = "This sentence is long enough that it cannot fit on one line of the help, and "
                 "goes on to a second one, then "
                 "an_extraordinarily_long_word_that_fits_on_no_line_of_the_help_at_all_anywhere."},
    };
    const tb_cli_command program = {.name = "wrap", .options = options, .option_count = 1};
    static const char expected[] =
        "usage: wrap [options]\n"
        "\n"
        "options:\n"
        "  -h, --help        Show this help and exit.\n"
        "      --word VALUE  This sentence is long enough that it cannot fit on one line\n"
        "                    of the help, and goes on to a second one, then\n"
        "                    "
        "an_extraordinarily_long_word_that_fits_on_no_line_of_the_help_at_all_anywhere.\n"
        "\n";
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    collected help = {0};

    TB_TEST_REQUIRE_UINT(parse_arguments(&program, &outcome, (const char *const[]){"-h", NULL}), ==,
                         TB_CLI_HELP);
    tb_cli_write_help(&outcome, collect, &help);
    TB_TEST_EXPECT_STRING(help.text, ==, expected);
}

TB_TEST(core_cli, a_long_usage_line_wraps_under_the_programs_name)
{
    const char *texts[4] = {0};
    const tb_cli_option options[] = {
        {.long_name = "first-required-option", .text = &texts[0], .required = true},
        {.long_name = "second-required-option", .text = &texts[1], .required = true},
        {.name = "SOURCE_DIRECTORY", .text = &texts[2], .required = true},
        {.name = "DESTINATION_DIRECTORY", .text = &texts[3]},
    };
    const tb_cli_command program = {.name = "copy", .options = options, .option_count = 4};
    static const char expected[] =
        "usage: copy [options] --first-required-option VALUE\n"
        "       --second-required-option VALUE SOURCE_DIRECTORY [DESTINATION_DIRECTORY]\n";
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    collected usage = {0};

    TB_TEST_REQUIRE_UINT(parse_arguments(&program, &outcome, (const char *const[]){"-h", NULL}), ==,
                         TB_CLI_HELP);
    tb_cli_write_usage(&outcome, collect, &usage);
    TB_TEST_EXPECT_STRING(usage.text, ==, expected);
}

/* A message holds what the user typed, which may be long; it is cut at a
 * character, never inside one. */
TB_TEST(core_cli, a_long_message_is_cut_at_a_character)
{
    char argument[1200] = {0};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    size_t error = 0;

    for (size_t index = 0; index + 2 < sizeof(argument); index += 2)
    {
        argument[index] = '\xC3';
        argument[index + 1] = '\xA9';
    }
    reset();
    TB_TEST_EXPECT_UINT(PARSE(&outcome, argument), ==, TB_CLI_ERROR);
    TB_TEST_EXPECT_UINT(tb_str_length(outcome.message), <, TB_CLI_MESSAGE_CAPACITY);
    TB_TEST_EXPECT_UINT(tb_str_length(outcome.message), >, TB_CLI_MESSAGE_CAPACITY - 4);
    TB_TEST_EXPECT(tb_utf8_validate(outcome.message, tb_str_length(outcome.message), &error));
}

/* -------------------------------------------------------------------------
 * Running, finishing, and wrong declarations.
 * ------------------------------------------------------------------------- */

TB_TEST(core_cli, run_calls_the_chosen_commands_function)
{
    const char *line[] = {"tool", "build", "game"};
    int calls = 0;
    reset();
    TB_TEST_EXPECT_INT(tb_cli_run(&tool, TB_TEST_COUNT_OF(line), line, &calls), ==, 7);
    TB_TEST_EXPECT_INT(calls, ==, 1);
}

TB_TEST(core_cli, finishing_a_successful_parse_writes_nothing)
{
    const char *line[] = {"tool", "clean"};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    reset();
    TB_TEST_REQUIRE_UINT(tb_cli_parse(&tool, 2, line, &outcome), ==, TB_CLI_OK);
    TB_TEST_EXPECT_INT(tb_cli_finish(&outcome), ==, EXIT_SUCCESS);
}

static void parse_declaration(void *context)
{
    const tb_cli_command *const program = context;
    const char *line[] = {"wrong"};
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    (void)tb_cli_parse(program, 1, line, &outcome);
}

TB_TEST(core_cli, a_wrong_declaration_violates_the_contract)
{
    static bool flag = false;
    static const char *text = NULL;
    static const tb_cli_option two_destinations[] = {
        {.long_name = "both", .flag = &flag, .text = &text}};
    static const tb_cli_option help_taken[] = {{.short_name = 'h', .flag = &flag}};
    static const tb_cli_option twice[] = {{.long_name = "same", .flag = &flag},
                                          {.long_name = "same", .text = &text}};
    static const tb_cli_option required_after_optional[] = {
        {.name = "FIRST", .text = &text}, {.name = "SECOND", .text = &text, .required = true}};
    static const tb_cli_option flag_argument[] = {{.name = "FLAG", .flag = &flag}};
    static tb_cli_command programs[] = {
        {.name = "a", .options = two_destinations, .option_count = 1},
        {.name = "b", .options = help_taken, .option_count = 1},
        {.name = "c", .options = twice, .option_count = 2},
        {.name = "d", .options = required_after_optional, .option_count = 2},
        {.name = "e", .options = flag_argument, .option_count = 1},
    };

    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(programs); index++)
    {
        TB_TEST_CONTEXT("program %s", programs[index].name);
        TB_TEST_EXPECT_TRAP(parse_declaration, &programs[index], "assertion failed");
    }
}
