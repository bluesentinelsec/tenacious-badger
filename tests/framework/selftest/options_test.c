/* The command line is how developers, CTest, and IDEs drive a test executable.
 * These cases pin its grammar: what is accepted, and that everything else is
 * rejected with a message that names the problem. */
#include "internal.h"

#include <string.h>

enum
{
    MAXIMUM_ARGUMENTS = 8,
    ARGUMENT_CAPACITY = 48,
    ERROR_CAPACITY = 160
};

/* A mutable argv, as `main` receives it, built from string literals. */
typedef struct command_line
{
    char storage[MAXIMUM_ARGUMENTS + 1][ARGUMENT_CAPACITY];
    char *argv[MAXIMUM_ARGUMENTS + 2];
    int argc;
} command_line;

/* Builds `line` from the program name plus `count` arguments. */
static void build_command_line(command_line *line, const char *const *arguments, size_t count)
{
    TB_TEST_REQUIRE_UINT(count, <=, MAXIMUM_ARGUMENTS);
    memset(line, 0, sizeof(*line));
    (void)snprintf(line->storage[0], ARGUMENT_CAPACITY, "%s", "test_program");
    line->argv[0] = line->storage[0];
    for (size_t index = 0; index < count; index++)
    {
        TB_TEST_REQUIRE_UINT(strlen(arguments[index]), <, ARGUMENT_CAPACITY);
        (void)snprintf(line->storage[index + 1], ARGUMENT_CAPACITY, "%s", arguments[index]);
        line->argv[index + 1] = line->storage[index + 1];
    }
    line->argc = (int)count + 1;
}

/* Parses the arguments and requires success. Release `options` afterwards. */
static void parse_valid(command_line *line, const char *const *arguments, size_t count,
                        tb_test_options *options)
{
    char error[ERROR_CAPACITY] = {0};

    build_command_line(line, arguments, count);
    TB_TEST_REQUIRE(tb_test_options_parse(line->argc, line->argv, options, error, sizeof(error)));
    TB_TEST_EXPECT_STRING(error, ==, "");
}

TB_TEST(framework_options, no_arguments_select_the_defaults)
{
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, NULL, 0, &options);
    TB_TEST_EXPECT_UINT(options.include_count, ==, 0);
    TB_TEST_EXPECT_UINT(options.exclude_count, ==, 0);
    TB_TEST_EXPECT_HEX(options.seed, ==, TB_TEST_DEFAULT_SEED);
    TB_TEST_EXPECT_UINT(options.repeat_count, ==, 1);
    TB_TEST_EXPECT(!options.seed_from_time);
    TB_TEST_EXPECT(!options.list_only);
    TB_TEST_EXPECT(!options.fail_fast);
    TB_TEST_EXPECT(!options.break_on_failure);
    TB_TEST_EXPECT(!options.shuffle);
    TB_TEST_EXPECT(!options.verbose);
    TB_TEST_EXPECT(!options.show_help);
    tb_test_options_release(&options);
}

TB_TEST(framework_options, an_absent_argv_is_an_empty_command_line)
{
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    char error[ERROR_CAPACITY] = {0};

    /* Hosted programs may be started with argc == 0. */
    TB_TEST_REQUIRE(tb_test_options_parse(0, NULL, &options, error, sizeof(error)));
    TB_TEST_EXPECT_UINT(options.include_count, ==, 0);
    tb_test_options_release(&options);
}

TB_TEST(framework_options, flags_are_recognized_individually)
{
    static const char *const arguments[] = {"--list",    "--fail-fast", "--break",
                                            "--shuffle", "--verbose",   "--help"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(arguments); index++)
    {
        TB_TEST_CONTEXT("flag %s", arguments[index]);
        parse_valid(&line, &arguments[index], 1, &options);
        TB_TEST_EXPECT(options.list_only == (index == 0));
        TB_TEST_EXPECT(options.fail_fast == (index == 1));
        TB_TEST_EXPECT(options.break_on_failure == (index == 2));
        TB_TEST_EXPECT(options.shuffle == (index == 3));
        TB_TEST_EXPECT(options.verbose == (index == 4));
        TB_TEST_EXPECT(options.show_help == (index == 5));
        tb_test_options_release(&options);
    }
}

TB_TEST(framework_options, short_help_is_an_alias)
{
    static const char *const arguments[] = {"-h"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, arguments, TB_TEST_COUNT_OF(arguments), &options);
    TB_TEST_EXPECT(options.show_help);
    tb_test_options_release(&options);
}

TB_TEST(framework_options, patterns_and_exclusions_keep_their_order)
{
    static const char *const arguments[] = {"sum.*",     "--exclude", "*.slow_*",       "list.?",
                                            "--shuffle", "table.get", "--exclude=set.*"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, arguments, TB_TEST_COUNT_OF(arguments), &options);
    TB_TEST_REQUIRE_UINT(options.include_count, ==, 3);
    TB_TEST_EXPECT_STRING(options.include_patterns[0], ==, "sum.*");
    TB_TEST_EXPECT_STRING(options.include_patterns[1], ==, "list.?");
    TB_TEST_EXPECT_STRING(options.include_patterns[2], ==, "table.get");
    TB_TEST_REQUIRE_UINT(options.exclude_count, ==, 2);
    TB_TEST_EXPECT_STRING(options.exclude_patterns[0], ==, "*.slow_*");
    TB_TEST_EXPECT_STRING(options.exclude_patterns[1], ==, "set.*");
    TB_TEST_EXPECT(options.shuffle);
    tb_test_options_release(&options);
}

typedef struct seed_row
{
    const char *text;
    uint64_t seed;
} seed_row;

TB_TEST(framework_options, seed_accepts_decimal_and_hexadecimal)
{
    static const seed_row rows[] = {
        {"0", 0},
        {"1", 1},
        {"12345", 12345},
        {"0x0", 0},
        {"0x3039", 12345},
        {"0XfF", 255},
        {"18446744073709551615", UINT64_MAX},
        {"0xFFFFFFFFFFFFFFFF", UINT64_MAX},
        {"0x7462746573740001", TB_TEST_DEFAULT_SEED},
    };
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};
    char inline_argument[ARGUMENT_CAPACITY] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const char *const separate[] = {"--seed", rows[index].text};
        const char *const joined[] = {inline_argument};

        TB_TEST_CONTEXT("row %zu: --seed %s", index, rows[index].text);
        parse_valid(&line, separate, TB_TEST_COUNT_OF(separate), &options);
        TB_TEST_EXPECT_HEX(options.seed, ==, rows[index].seed);
        TB_TEST_EXPECT(!options.seed_from_time);
        tb_test_options_release(&options);

        (void)snprintf(inline_argument, sizeof(inline_argument), "--seed=%s", rows[index].text);
        parse_valid(&line, joined, TB_TEST_COUNT_OF(joined), &options);
        TB_TEST_EXPECT_HEX(options.seed, ==, rows[index].seed);
        tb_test_options_release(&options);
    }
}

TB_TEST(framework_options, seed_can_come_from_the_clock)
{
    static const char *const arguments[] = {"--seed", "time"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, arguments, TB_TEST_COUNT_OF(arguments), &options);
    TB_TEST_EXPECT(options.seed_from_time);
    tb_test_options_release(&options);
}

TB_TEST(framework_options, the_last_seed_wins)
{
    static const char *const arguments[] = {"--seed", "time", "--seed", "7"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, arguments, TB_TEST_COUNT_OF(arguments), &options);
    TB_TEST_EXPECT(!options.seed_from_time);
    TB_TEST_EXPECT_UINT(options.seed, ==, 7);
    tb_test_options_release(&options);
}

TB_TEST(framework_options, repeat_accepts_its_whole_range)
{
    static const char *const once[] = {"--repeat", "1"};
    static const char *const most[] = {"--repeat=1000000"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, once, TB_TEST_COUNT_OF(once), &options);
    TB_TEST_EXPECT_UINT(options.repeat_count, ==, 1);
    tb_test_options_release(&options);

    parse_valid(&line, most, TB_TEST_COUNT_OF(most), &options);
    TB_TEST_EXPECT_UINT(options.repeat_count, ==, 1000000);
    tb_test_options_release(&options);
}

typedef struct invalid_row
{
    const char *arguments[3];
    size_t count;
    const char *error_part; /* Text the error message must contain. */
} invalid_row;

TB_TEST(framework_options, invalid_command_lines_are_rejected_with_a_reason)
{
    static const invalid_row rows[] = {
        /* Unknown options, including near misses of real ones. */
        {{"--bogus"}, 1, "unknown option: --bogus"},
        {{"-x"}, 1, "unknown option: -x"},
        {{"-"}, 1, "unknown option: -"},
        {{"--"}, 1, "unknown option: --"},
        {{"--seedling", "1"}, 2, "unknown option: --seedling"},
        {{"--list=yes"}, 1, "unknown option: --list=yes"},
        {{"--Verbose"}, 1, "unknown option: --Verbose"},
        /* Options that need a value and have none. */
        {{"--seed"}, 1, "missing value for option: --seed"},
        {{"--repeat"}, 1, "missing value for option: --repeat"},
        {{"sum.*", "--exclude"}, 2, "missing value for option: --exclude"},
        /* Seeds that are not plain unsigned numbers. */
        {{"--seed", ""}, 2, "--seed expects"},
        {{"--seed="}, 1, "--seed expects"},
        {{"--seed", "-1"}, 2, "--seed expects"},
        {{"--seed", "+1"}, 2, "--seed expects"},
        {{"--seed", " 1"}, 2, "--seed expects"},
        {{"--seed", "1 "}, 2, "--seed expects"},
        {{"--seed", "12x"}, 2, "--seed expects"},
        {{"--seed", "0x"}, 2, "--seed expects"},
        {{"--seed", "abc"}, 2, "--seed expects"},
        {{"--seed", "Time"}, 2, "--seed expects"},
        {{"--seed", "18446744073709551616"}, 2, "--seed expects"},
        {{"--seed", "0x10000000000000000"}, 2, "--seed expects"},
        /* Repeat counts outside 1..1000000. */
        {{"--repeat", "0"}, 2, "--repeat expects a count from 1 to 1000000: 0"},
        {{"--repeat", "1000001"}, 2, "--repeat expects"},
        {{"--repeat", "two"}, 2, "--repeat expects"},
        {{"--repeat", "-3"}, 2, "--repeat expects"},
        {{"--repeat=18446744073709551616"}, 1, "--repeat expects"},
        /* Empty patterns would match nothing and hide a quoting mistake. */
        {{""}, 1, "empty pattern"},
        {{"--exclude", ""}, 2, "--exclude expects a pattern"},
        {{"--exclude="}, 1, "--exclude expects a pattern"},
        /* A valid prefix does not excuse a bad argument after it. */
        {{"sum.*", "--verbose", "--nope"}, 3, "unknown option: --nope"},
    };
    command_line line = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        tb_test_options options = TB_TEST_OPTIONS_INIT;
        char error[ERROR_CAPACITY] = {0};

        TB_TEST_CONTEXT("row %zu: first argument \"%s\"", index, rows[index].arguments[0]);
        build_command_line(&line, rows[index].arguments, rows[index].count);
        TB_TEST_EXPECT(
            !tb_test_options_parse(line.argc, line.argv, &options, error, sizeof(error)));
        TB_TEST_EXPECT(strstr(error, rows[index].error_part) != NULL);
        tb_test_options_release(&options);
    }
}

TB_TEST(framework_options, release_returns_to_the_defaults)
{
    static const char *const arguments[] = {"--seed", "9", "--repeat", "4", "--verbose", "a.*"};
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    command_line line = {0};

    parse_valid(&line, arguments, TB_TEST_COUNT_OF(arguments), &options);
    tb_test_options_release(&options);
    TB_TEST_EXPECT_POINTER(options.include_patterns, ==, NULL);
    TB_TEST_EXPECT_POINTER(options.exclude_patterns, ==, NULL);
    TB_TEST_EXPECT_UINT(options.include_count, ==, 0);
    TB_TEST_EXPECT_HEX(options.seed, ==, TB_TEST_DEFAULT_SEED);
    TB_TEST_EXPECT_UINT(options.repeat_count, ==, 1);
    TB_TEST_EXPECT(!options.verbose);
}
