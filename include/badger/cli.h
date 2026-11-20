/** @file cli.h
 * @brief Command lines: options, arguments, subcommands, and their help.
 *
 * A program declares what its command line accepts as data: a command, with
 * tables of options and arguments whose entries point at the program's own
 * variables, and the commands below it. tb_cli_parse() reads the arguments
 * into those variables and says which command was chosen, or why it could
 * not: help or the version was asked for, or the command line is wrong.
 *
 * @code
 * static bool verbose = false;
 * static int64_t jobs = 4;
 * static const char *target = NULL;
 *
 * static const tb_cli_option build_options[] = {
 *     {.short_name = 'j', .long_name = "jobs", .integer = &jobs, .minimum = 1,
 *      .maximum = 64, .metavar = "N", .help = "Run N jobs at once."},
 *     {.name = "TARGET", .text = &target, .required = true, .help = "What to build."},
 * };
 * static const tb_cli_command commands[] = {
 *     {.name = "build", .summary = "Build the project.", .options = build_options,
 *      .option_count = TB_CLI_COUNT_OF(build_options)},
 * };
 * static const tb_cli_option options[] = {
 *     {.short_name = 'v', .long_name = "verbose", .flag = &verbose, .help = "Say more."},
 * };
 * static const tb_cli_command program = {
 *     .name = "burrow", .version = "0.1.0", .options = options,
 *     .option_count = TB_CLI_COUNT_OF(options), .commands = commands,
 *     .command_count = TB_CLI_COUNT_OF(commands)};
 *
 * int main(int count, char **arguments)
 * {
 *     tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
 *     if (tb_cli_parse(&program, (size_t)count, (const char *const *)arguments,
 *                      &outcome) != TB_CLI_OK)
 *     {
 *         return tb_cli_finish(&outcome);
 *     }
 *     ...outcome.command is &commands[0], and jobs and target are set...
 * }
 * @endcode
 *
 * **What a command line may hold.**
 * - Short options, `-v`, which group: `-vq` is `-v -q`. A short option that
 *   takes a value takes the rest of its argument, `-j4`, or the next one,
 *   `-j 4`.
 * - Long options, `--verbose`, with a value as `--jobs=4` or `--jobs 4`.
 * - Arguments, in the order the command declares them, interleaved with
 *   options as the user likes. `--` ends the options: what follows is all
 *   arguments. `-` alone is an argument, by convention standard input, and so
 *   is a negative number.
 * - A command's name, which chooses it from its parent's commands. The
 *   options of every command chosen so far are accepted after it; a
 *   command's own option hides a parent's of the same name.
 *
 * A value is the next argument whatever it holds, unless it begins with `-`
 * and is not a negative number: `--output --verbose` is an error, not an
 * output named "--verbose". `--output=--verbose` gives that name.
 *
 * **Help.** Every command accepts `-h` and `--help`, and a program with a
 * version `--version`; a declaration may not use those names. The help says
 * how to use the command, then its description, arguments, options and
 * commands, and its epilog, at 80 columns.
 *
 * **Errors** are the caller's: tb_cli_parse() reports them in the outcome
 * and never ends the process. tb_cli_finish() writes help, a version, or an
 * error as a command-line program does, and returns the status to exit
 * with.
 *
 * **A wrong declaration is a contract violation**, caught by an assertion
 * when the command is parsed: an option with no destination or two, a name
 * used twice, a required argument after an optional one, and the like. A
 * command declares at most 128 options and arguments.
 *
 * @par Threading
 * Thread-safe, when no two calls write the same variables. Declarations are
 * only read.
 */
#ifndef TB_CLI_H
#define TB_CLI_H

#include "badger/api.h"
#include "badger/fmt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The number of elements of an array, for the counts of a
 * declaration. */
#define TB_CLI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/** @brief The status a program exits with after a wrong command line, as
 * argparse and most command-line programs do. */
#define TB_CLI_EXIT_USAGE 2

/** @brief How deep commands may nest, the program included. */
#define TB_CLI_DEPTH 8

/** @brief Bytes of an outcome's message, its NUL included. */
#define TB_CLI_MESSAGE_CAPACITY 512

typedef struct tb_cli_outcome tb_cli_outcome;

/**
 * @brief An option, or a positional argument, and where its value goes.
 *
 * An entry with a short or long name is an option; one with neither is an
 * argument, named by `name`. Exactly one destination is set, and it says
 * what the entry is:
 *
 * | Destination | Value | Written |
 * | --- | --- | --- |
 * | `flag` | none | `true` |
 * | `counter` | none | one more each time it is given |
 * | `text` | any text | the argument itself, borrowed from the arguments |
 * | `integer` | decimal, or `0x` and hexadecimal, with a sign | within `minimum` to `maximum` |
 * | `number` | a finite decimal, as tb_decimal_parse() reads it | the value |
 * | `choice` | one of `choices` | its index in `choices` |
 * | `texts` | any text, each time it is given | appended, up to `capacity`, with `*count` |
 *
 * A destination keeps its value when the entry is not given: that is its
 * default, which the help shows for text, integers, numbers and choices.
 * Flags and counters only ever grow, so start them at false and 0.
 *
 * Only an argument with `texts` takes more than one argument: it takes all
 * that are left, and must be the command's last argument.
 */
typedef struct tb_cli_option
{
    char short_name;       /**< A letter, for `-x`; 0 for none. */
    const char *long_name; /**< Letters, digits and `-`, for `--name`; NULL for none. */
    /** An argument's name in the help, such as "FILE"; or an option's value
     * there, as in `--output FILE`. NULL gives an option's value the name
     * "VALUE", or the choices. */
    const char *name;
    const char *help; /**< One or more sentences; NULL for none. */
    /** Whether the command line is wrong without it. An argument with
     * `texts` that is required takes at least one. */
    bool required;

    bool *flag;         /**< Destination of a flag. */
    size_t *counter;    /**< Destination of a counter. */
    const char **text;  /**< Destination of a text. */
    int64_t *integer;   /**< Destination of an integer. */
    double *number;     /**< Destination of a number. */
    size_t *choice;     /**< Destination of a choice: the index of the one given. */
    const char **texts; /**< Destination of repeated texts: an array of `capacity`. */

    int64_t minimum; /**< The least integer accepted, when `minimum < maximum`. */
    int64_t maximum; /**< The greatest integer accepted, when `minimum < maximum`. */
    /** The choices, which `choice_count` counts. */
    const char *const *choices;
    size_t choice_count; /**< Number of `choices`; at least one. */
    size_t capacity;     /**< Elements of `texts`; at least one. */
    size_t *count;       /**< Receives how many of `texts` were given; required with them. */
    /** Set to true when the entry is given; NULL when nobody asks. */
    bool *given;
} tb_cli_option;

/**
 * @brief What a command does once its command line is read, for
 * tb_cli_run(). Its return value is the status the program exits with.
 * @param outcome The parse, whose `command` is the one that runs.
 * @param context What was given to tb_cli_run().
 */
typedef int tb_cli_run_function(const tb_cli_outcome *outcome, void *context);

/** @brief A program, or a command of one, with what it accepts. */
typedef struct tb_cli_command
{
    /** A command's name, which chooses it. A program's name, for its help
     * and errors; NULL takes it from the first argument, without a
     * directory or ".exe". */
    const char *name;
    /** One line, for the list of its parent's commands; NULL for none. */
    const char *summary;
    /** Paragraphs for its help, after the usage, each ended by a blank line
     * ("\n\n"); NULL for none. Lines within a paragraph are rewrapped. */
    const char *description;
    /** Paragraphs at the end of its help, as the description; NULL for none. */
    const char *epilog;
    /** A program's version, which adds `--version`; NULL for none. Only the
     * program's is used. */
    const char *version;
    const tb_cli_option *options; /**< Its options and arguments; NULL at count zero. */
    size_t option_count;          /**< Number of `options`. */
    /** Its commands. A command has commands or arguments, not both. */
    const struct tb_cli_command *commands;
    size_t command_count; /**< Number of `commands`. */
    /** Whether it is complete without one of its commands; otherwise a
     * command line that names none is wrong. */
    bool command_optional;
    /** What tb_cli_run() calls when this is the command chosen; NULL for
     * none. */
    tb_cli_run_function *run;
} tb_cli_command;

/** @brief What tb_cli_parse() came to. */
typedef enum tb_cli_result
{
    TB_CLI_OK,      /**< The command line is right, and every destination is set. */
    TB_CLI_HELP,    /**< `-h` or `--help` was given; write the help and exit. */
    TB_CLI_VERSION, /**< `--version` was given; write the version and exit. */
    TB_CLI_ERROR    /**< The command line is wrong; the outcome says how. */
} tb_cli_result;

/**
 * @brief What a parse chose, or why it stopped.
 *
 * The pointers are borrowed: from the declaration, which outlives the
 * outcome, and from the arguments. The outcome is plain data, and may be
 * copied.
 */
struct tb_cli_outcome
{
    tb_cli_result result; /**< As tb_cli_parse() returned. */
    /** The commands chosen, from the program down: `path[depth - 1]` is the
     * command the help or the error is about, and on success the one that
     * runs. */
    const tb_cli_command *path[TB_CLI_DEPTH];
    size_t depth;                  /**< Commands in `path`; at least one after a parse. */
    const tb_cli_command *command; /**< `path[depth - 1]`, for convenience. */
    /** The program's name, as its help and errors give it: from the
     * declaration, or from the first argument. */
    char program[64];
    /** For TB_CLI_ERROR, what is wrong, in UTF-8, without the program's name:
     * "unknown option '--jbos'; did you mean '--jobs'?". Empty otherwise. */
    char message[TB_CLI_MESSAGE_CAPACITY];
};

/** @brief An outcome before a parse. */
#define TB_CLI_OUTCOME_INIT {TB_CLI_OK, {0}, 0, NULL, {0}, {0}}

/**
 * @brief Read a command line into the variables that a program declares.
 *
 * On TB_CLI_OK every destination of the commands chosen holds what the
 * command line gave, or what it held before. On any other result some may
 * have been written, and the command line should not be acted on.
 *
 * @param program The program's command; required. It and everything it
 * points at must outlive the outcome.
 * @param count Number of arguments, as `main` receives it; at least one.
 * @param arguments The arguments, the program's own name first; required.
 * Texts written to destinations point into them.
 * @param[out] outcome Required; filled in whatever the result.
 * @return What the parse came to.
 */
TB_API tb_cli_result tb_cli_parse(const tb_cli_command *program, size_t count,
                                  const char *const *arguments, tb_cli_outcome *outcome);

/**
 * @brief Write the help of the command an outcome is about.
 * @param outcome A parse's outcome, or one whose path and program a caller
 * filled in; required.
 * @param sink Receives the text; required.
 * @param context Passed to `sink`.
 */
TB_API void tb_cli_write_help(const tb_cli_outcome *outcome, tb_fmt_sink *sink, void *context);

/**
 * @brief Write the usage line of the command an outcome is about:
 * "usage: burrow build [options] TARGET\n".
 * @param outcome As for tb_cli_write_help().
 * @param sink Receives the text; required.
 * @param context Passed to `sink`.
 */
TB_API void tb_cli_write_usage(const tb_cli_outcome *outcome, tb_fmt_sink *sink, void *context);

/**
 * @brief Write what a parse that did not succeed calls for, to the standard
 * streams: the help or the version to standard output, or the usage and the
 * error to standard error, "burrow build: error: ...".
 * @param outcome A parse's outcome; required.
 * @return The status to exit with: `EXIT_SUCCESS` after help or the
 * version, #TB_CLI_EXIT_USAGE after an error, `EXIT_FAILURE` when the output
 * could not be written; and for TB_CLI_OK, `EXIT_SUCCESS` with nothing
 * written.
 */
TB_API int tb_cli_finish(const tb_cli_outcome *outcome);

/**
 * @brief Parse a command line and run the command it chooses.
 *
 * tb_cli_parse(), then the chosen command's `run`. A command line that does
 * not succeed goes to tb_cli_finish().
 *
 * @param program As for tb_cli_parse().
 * @param count As for tb_cli_parse().
 * @param arguments As for tb_cli_parse().
 * @param context Passed to `run`.
 * @return The status to exit with: what `run` returned, or what
 * tb_cli_finish() did. A chosen command without `run` is a contract
 * violation.
 */
TB_API int tb_cli_run(const tb_cli_command *program, size_t count, const char *const *arguments,
                      void *context);

/** @brief Name a result, for a log: "help asked for".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL. */
TB_API const char *tb_cli_result_name(tb_cli_result result);

#endif
