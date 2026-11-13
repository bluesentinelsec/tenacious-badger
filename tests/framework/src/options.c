#include "internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Far beyond any useful soak run, and representable in every size_t. */
#define TB_TEST_MAXIMUM_REPEAT_COUNT UINT64_C(1000000)

_Static_assert(ULLONG_MAX == UINT64_MAX, "strtoull must cover exactly the uint64_t range");

/* Accept "--name" (value in the next argument) and "--name=value". */
static bool tb_test_option_is(const char *argument, const char *name, const char **inline_value)
{
    const size_t name_length = strlen(name);

    *inline_value = NULL;
    if (strncmp(argument, name, name_length) != 0)
    {
        return false;
    }
    if (argument[name_length] == '\0')
    {
        return true;
    }
    if (argument[name_length] == '=')
    {
        *inline_value = argument + name_length + 1;
        return true;
    }
    return false;
}

/* Decimal or 0x-prefixed hexadecimal, with nothing before or after the digits.
 * strtoull alone would also accept signs and leading spaces. */
static bool tb_test_parse_unsigned(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed = {0};

    if (text[0] < '0' || text[0] > '9')
    {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || *end != '\0')
    {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool tb_test_fail_parse(char *error, size_t error_size, const char *problem,
                               const char *subject)
{
    (void)snprintf(error, error_size, "%s: %s", problem, subject);
    return false;
}

static bool tb_test_parse_seed(const char *value, tb_test_options *options, char *error,
                               size_t error_size)
{
    if (strcmp(value, "time") == 0)
    {
        options->seed_from_time = true;
        return true;
    }
    if (!tb_test_parse_unsigned(value, &options->seed))
    {
        return tb_test_fail_parse(error, error_size,
                                  "--seed expects an unsigned number or \"time\"", value);
    }
    options->seed_from_time = false;
    return true;
}

static bool tb_test_parse_repeat(const char *value, tb_test_options *options, char *error,
                                 size_t error_size)
{
    uint64_t count = {0};

    if (!tb_test_parse_unsigned(value, &count) || count == 0 ||
        count > TB_TEST_MAXIMUM_REPEAT_COUNT)
    {
        return tb_test_fail_parse(error, error_size, "--repeat expects a count from 1 to 1000000",
                                  value);
    }
    options->repeat_count = (size_t)count;
    return true;
}

static bool tb_test_parse_exclude(const char *value, tb_test_options *options, char *error,
                                  size_t error_size)
{
    if (value[0] == '\0')
    {
        return tb_test_fail_parse(error, error_size, "--exclude expects a pattern", "(empty)");
    }
    options->exclude_patterns[options->exclude_count] = value;
    options->exclude_count++;
    return true;
}

/* Options that take no value. Returns the flag to set, or NULL. */
static bool *tb_test_find_flag(const char *argument, tb_test_options *options)
{
    if (strcmp(argument, "--list") == 0)
    {
        return &options->list_only;
    }
    if (strcmp(argument, "--fail-fast") == 0)
    {
        return &options->fail_fast;
    }
    if (strcmp(argument, "--break") == 0)
    {
        return &options->break_on_failure;
    }
    if (strcmp(argument, "--shuffle") == 0)
    {
        return &options->shuffle;
    }
    if (strcmp(argument, "--verbose") == 0)
    {
        return &options->verbose;
    }
    if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0)
    {
        return &options->show_help;
    }
    return NULL;
}

typedef bool tb_test_value_parser(const char *value, tb_test_options *options, char *error,
                                  size_t error_size);

/* Options that take a value. Returns the parser, or NULL. */
static tb_test_value_parser *tb_test_find_value_parser(const char *argument,
                                                       const char **inline_value)
{
    if (tb_test_option_is(argument, "--seed", inline_value))
    {
        return tb_test_parse_seed;
    }
    if (tb_test_option_is(argument, "--repeat", inline_value))
    {
        return tb_test_parse_repeat;
    }
    if (tb_test_option_is(argument, "--exclude", inline_value))
    {
        return tb_test_parse_exclude;
    }
    return NULL;
}

bool tb_test_options_parse(int argc, char **argv, tb_test_options *options, char *error,
                           size_t error_size)
{
    size_t argument_count = {0};

    error[0] = '\0';
    if (argc > 1)
    {
        argument_count = (size_t)argc - 1;
    }
    /* Every argument is at most one pattern, so argv bounds both lists. */
    options->include_patterns = tb_test_allocate_zeroed(argument_count, sizeof(const char *));
    options->exclude_patterns = tb_test_allocate_zeroed(argument_count, sizeof(const char *));

    for (size_t index = 1; index <= argument_count; index++)
    {
        const char *const argument = argv[index];
        const char *value = NULL;
        bool *flag = NULL;
        tb_test_value_parser *parse_value = NULL;

        if (argument[0] != '-')
        {
            if (argument[0] == '\0')
            {
                return tb_test_fail_parse(error, error_size, "empty pattern", "(empty)");
            }
            options->include_patterns[options->include_count] = argument;
            options->include_count++;
            continue;
        }
        flag = tb_test_find_flag(argument, options);
        if (flag != NULL)
        {
            *flag = true;
            continue;
        }
        parse_value = tb_test_find_value_parser(argument, &value);
        if (parse_value == NULL)
        {
            return tb_test_fail_parse(error, error_size, "unknown option", argument);
        }
        if (value == NULL)
        {
            if (index == argument_count)
            {
                return tb_test_fail_parse(error, error_size, "missing value for option", argument);
            }
            index++;
            value = argv[index];
        }
        if (!parse_value(value, options, error, error_size))
        {
            return false;
        }
    }
    return true;
}

void tb_test_options_release(tb_test_options *options)
{
    tb_test_release(options->include_patterns);
    tb_test_release(options->exclude_patterns);
    *options = (tb_test_options)TB_TEST_OPTIONS_INIT;
}

void tb_test_options_print_usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [options] [pattern...]\n", program);
    fputs("\n"
          "Runs the test cases whose \"suite.name\" matches any pattern; all cases when no\n"
          "pattern is given. In a pattern, '*' matches any run of characters and '?' one.\n"
          "Cases run in the order of the patterns, and in source order within a pattern.\n"
          "\n"
          "  --exclude PATTERN  Leave out matching cases. May be repeated.\n"
          "  --list             Print the selected cases with their locations and exit.\n"
          "  --fail-fast        Stop the run at the first failed case.\n"
          "  --break            Break into the debugger at a failed check. Run under a\n"
          "                     debugger; without one the break ends the process.\n"
          "  --seed VALUE       Seed for tb_test_random_* and --shuffle: a decimal or 0x\n"
          "                     number, or \"time\" for a fresh seed. Default: fixed.\n"
          "  --shuffle          Run the cases in seeded random order.\n"
          "  --repeat COUNT     Run the selection COUNT times. Each round adds one to the\n"
          "                     seed, so random inputs differ between rounds.\n"
          "  --verbose          Report every case, not only failures and skips.\n"
          "  -h, --help         Print this text and exit.\n"
          "\n"
          "Exit status: 0 passed, 1 failed, 2 bad command line or no case selected,\n"
          "77 every selected case was skipped.\n",
          stream);
}
