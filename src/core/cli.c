/* Command lines: reading them into a program's variables (tb_cli_parse), and
 * writing the help and errors that go with them. The parser allocates
 * nothing; it keeps what it must remember in a structure on the stack. */
#include "badger/cli.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/str.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* How many options and arguments a command may declare: the parser keeps one
 * mark for each, to know which required ones were given. */
#define TB_CLI_OPTIONS 128

/* The help's width, in characters. */
#define TB_CLI_WIDTH 80

/* The widest column of option names; a wider name puts its help on the next
 * line. */
#define TB_CLI_NAME_COLUMN 30

/* Names longer than this are not compared for a suggestion. */
#define TB_CLI_SUGGESTION_LENGTH 63

/* In UTF-8 the bytes of a character after its first all have the form
 * 10xxxxxx. */
#define TB_CLI_CONTINUATION_MASK 0xC0u
#define TB_CLI_CONTINUATION_BITS 0x80u

/* What a parse keeps while it reads. */
typedef struct tb_cli_parser
{
    tb_cli_outcome *outcome;
    size_t count;
    const char *const *arguments;
    size_t index;
    /* The next argument of the command chosen last. */
    size_t positional;
    bool options_ended;
    /* Which options and arguments of each command in the path were given. */
    bool given[TB_CLI_DEPTH][TB_CLI_OPTIONS];
} tb_cli_parser;

/* An option found by its name, and which command of the path declares it. */
typedef struct tb_cli_found
{
    const tb_cli_option *option;
    size_t depth;
    size_t index;
} tb_cli_found;

/* -------------------------------------------------------------------------
 * The declaration.
 * ------------------------------------------------------------------------- */

static bool tb_cli_is_argument(const tb_cli_option *option)
{
    return option->short_name == '\0' && option->long_name == NULL;
}

static bool tb_cli_takes_value(const tb_cli_option *option)
{
    return option->flag == NULL && option->counter == NULL;
}

static bool tb_cli_is_letter(char character)
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

static bool tb_cli_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

static size_t tb_cli_destinations(const tb_cli_option *option)
{
    size_t count = 0;
    count += option->flag != NULL ? 1u : 0u;
    count += option->counter != NULL ? 1u : 0u;
    count += option->text != NULL ? 1u : 0u;
    count += option->integer != NULL ? 1u : 0u;
    count += option->number != NULL ? 1u : 0u;
    count += option->choice != NULL ? 1u : 0u;
    count += option->texts != NULL ? 1u : 0u;
    return count;
}

static bool tb_cli_long_name_is_valid(const char *name)
{
    if (!tb_cli_is_letter(name[0]) && !tb_cli_is_digit(name[0]))
    {
        return false;
    }
    for (const char *character = name; *character != '\0'; character++)
    {
        if (!tb_cli_is_letter(*character) && !tb_cli_is_digit(*character) && *character != '-')
        {
            return false;
        }
    }
    return true;
}

/* Asserts what tb_cli_option documents of one entry. */
static void tb_cli_check_option(const tb_cli_option *option)
{
    TB_ASSERT(tb_cli_destinations(option) == 1);
    if (tb_cli_is_argument(option))
    {
        TB_ASSERT(option->name != NULL && option->name[0] != '\0');
        TB_ASSERT(tb_cli_takes_value(option));
    }
    else
    {
        TB_ASSERT(option->short_name == '\0' || tb_cli_is_letter(option->short_name));
        TB_ASSERT(option->short_name != 'h');
        TB_ASSERT(option->long_name == NULL || tb_cli_long_name_is_valid(option->long_name));
        TB_ASSERT(option->long_name == NULL || (!tb_str_equal(option->long_name, "help") &&
                                                !tb_str_equal(option->long_name, "version")));
    }
    if (option->choice != NULL)
    {
        TB_ASSERT(option->choices != NULL && option->choice_count > 0);
    }
    if (option->texts != NULL)
    {
        TB_ASSERT(option->capacity > 0 && option->count != NULL);
    }
}

/* Asserts what tb_cli_command documents of one command: its entries, no
 * name twice, and arguments in an order that can be read. */
static void tb_cli_check_command(const tb_cli_command *command)
{
    bool optional_seen = false;
    bool variadic_seen = false;
    size_t arguments = 0;

    TB_ASSERT(command->options != NULL || command->option_count == 0);
    TB_ASSERT(command->commands != NULL || command->command_count == 0);
    TB_ASSERT(command->option_count <= TB_CLI_OPTIONS);
    for (size_t index = 0; index < command->option_count; index++)
    {
        const tb_cli_option *const option = &command->options[index];
        tb_cli_check_option(option);
        for (size_t earlier = 0; earlier < index; earlier++)
        {
            const tb_cli_option *const other = &command->options[earlier];
            TB_ASSERT(option->short_name == '\0' || option->short_name != other->short_name);
            TB_ASSERT(option->long_name == NULL || other->long_name == NULL ||
                      !tb_str_equal(option->long_name, other->long_name));
        }
        if (!tb_cli_is_argument(option))
        {
            continue;
        }
        /* An argument after one that takes all the rest would never be
         * reached, and a required one after an optional one could not be
         * told from it. */
        TB_ASSERT(!variadic_seen);
        TB_ASSERT(!(optional_seen && option->required));
        optional_seen = optional_seen || !option->required;
        variadic_seen = option->texts != NULL;
        arguments++;
    }
    TB_ASSERT(arguments == 0 || command->command_count == 0);
    for (size_t index = 0; index < command->command_count; index++)
    {
        TB_ASSERT(command->commands[index].name != NULL);
        for (size_t earlier = 0; earlier < index; earlier++)
        {
            TB_ASSERT(
                !tb_str_equal(command->commands[index].name, command->commands[earlier].name));
        }
    }
}

/* -------------------------------------------------------------------------
 * Text.
 * ------------------------------------------------------------------------- */

static bool tb_cli_is_continuation(char byte)
{
    return ((unsigned char)byte & TB_CLI_CONTINUATION_MASK) == TB_CLI_CONTINUATION_BITS;
}

/* The width of UTF-8 text in characters: its bytes that begin one. */
static size_t tb_cli_width(const char *text, size_t length)
{
    size_t width = 0;
    for (size_t index = 0; index < length; index++)
    {
        width += tb_cli_is_continuation(text[index]) ? 0u : 1u;
    }
    return width;
}

/* Cuts text that tb_fmt_vbuffer() cut at its capacity back to the last
 * whole character. */
static void tb_cli_cut_at_character(char *text, size_t length, size_t capacity)
{
    size_t end = length;
    if (length < capacity)
    {
        return;
    }
    end = capacity - 1;
    while (end > 0 && tb_cli_is_continuation(text[end]))
    {
        end--;
    }
    text[end] = '\0';
}

/* The distance between two names: insertions, deletions, substitutions,
 * and swaps of neighbors, so that "jbos" is one from "jobs". */
static size_t tb_cli_distance(const char *left, size_t left_length, const char *right,
                              size_t right_length)
{
    size_t rows[3][TB_CLI_SUGGESTION_LENGTH + 2] = {{0}};
    size_t *before = rows[0];
    size_t *previous = rows[1];
    size_t *current = rows[2];

    for (size_t column = 0; column <= right_length; column++)
    {
        previous[column] = column;
    }
    for (size_t row = 1; row <= left_length; row++)
    {
        size_t *const oldest = before;
        current[0] = row;
        for (size_t column = 1; column <= right_length; column++)
        {
            const size_t cost = left[row - 1] == right[column - 1] ? 0u : 1u;
            size_t best = previous[column - 1] + cost;
            if (previous[column] + 1 < best)
            {
                best = previous[column] + 1;
            }
            if (current[column - 1] + 1 < best)
            {
                best = current[column - 1] + 1;
            }
            if (row > 1 && column > 1 && left[row - 1] == right[column - 2] &&
                left[row - 2] == right[column - 1] && before[column - 2] + 1 < best)
            {
                best = before[column - 2] + 1;
            }
            current[column] = best;
        }
        before = previous;
        previous = current;
        current = oldest;
    }
    return previous[right_length];
}

/* Keeps `candidate` as the suggestion for `typed` when it is the nearest
 * yet, and near enough to be what was meant. */
typedef struct tb_cli_suggestion
{
    const char *typed;
    size_t typed_length;
    const char *best;
    size_t distance;
} tb_cli_suggestion;

static void tb_cli_consider(tb_cli_suggestion *suggestion, const char *candidate)
{
    const size_t length = tb_str_length(candidate);
    size_t distance = 0;
    size_t limit = 0;

    if (suggestion->typed_length > TB_CLI_SUGGESTION_LENGTH || length > TB_CLI_SUGGESTION_LENGTH)
    {
        return;
    }
    distance = tb_cli_distance(suggestion->typed, suggestion->typed_length, candidate, length);
    /* One edit in a short name, more in a longer one; never most of it. */
    limit = length / 3;
    if (limit < 1)
    {
        limit = 1;
    }
    if (distance <= limit && distance < length &&
        (suggestion->best == NULL || distance < suggestion->distance))
    {
        suggestion->best = candidate;
        suggestion->distance = distance;
    }
}

/* -------------------------------------------------------------------------
 * Failures.
 * ------------------------------------------------------------------------- */

/* Records a usage error, formatted, and returns TB_CLI_ERROR. */
static tb_cli_result tb_cli_fail(tb_cli_parser *parser, const char *format, ...)
{
    tb_cli_outcome *const outcome = parser->outcome;
    va_list arguments;
    size_t length = 0;

    va_start(arguments, format);
    length = tb_fmt_vbuffer(outcome->message, sizeof(outcome->message), format, arguments);
    va_end(arguments);
    tb_cli_cut_at_character(outcome->message, length, sizeof(outcome->message));
    outcome->result = TB_CLI_ERROR;
    return TB_CLI_ERROR;
}

/* Appends formatted text to the message, cut at a character. */
static void tb_cli_append(tb_cli_parser *parser, const char *format, ...)
{
    tb_cli_outcome *const outcome = parser->outcome;
    const size_t used = tb_str_length(outcome->message);
    va_list arguments;
    size_t length = 0;

    va_start(arguments, format);
    length =
        tb_fmt_vbuffer(outcome->message + used, sizeof(outcome->message) - used, format, arguments);
    va_end(arguments);
    tb_cli_cut_at_character(outcome->message + used, length, sizeof(outcome->message) - used);
}

/* How an option is named in a message: as the user may have written it. */
static void tb_cli_label(const tb_cli_option *option, char *label, size_t capacity)
{
    if (tb_cli_is_argument(option))
    {
        (void)tb_fmt_buffer(label, capacity, "argument %s", option->name);
    }
    else if (option->long_name != NULL)
    {
        (void)tb_fmt_buffer(label, capacity, "option '--%s'", option->long_name);
    }
    else
    {
        (void)tb_fmt_buffer(label, capacity, "option '-%c'", option->short_name);
    }
}

/* -------------------------------------------------------------------------
 * Values.
 * ------------------------------------------------------------------------- */

/* Whether an argument that begins with '-' is a negative number, and so not
 * an option: "-5", "-0.5", "-.5". */
static bool tb_cli_is_negative_number(const char *argument)
{
    if (argument[0] != '-')
    {
        return false;
    }
    if (tb_cli_is_digit(argument[1]))
    {
        return true;
    }
    return argument[1] == '.' && tb_cli_is_digit(argument[2]);
}

static int tb_cli_digit_value(char character)
{
    if (tb_cli_is_digit(character))
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

/* An integer: an optional sign, then decimal digits, or "0x" and
 * hexadecimal ones. False for anything else, or a value beyond int64_t. */
static bool tb_cli_read_integer(const char *text, int64_t *value)
{
    bool negative = false;
    uint64_t magnitude = 0;
    uint64_t base = 10;
    uint64_t limit = (uint64_t)INT64_MAX;
    const char *digit = text;

    if (*digit == '+' || *digit == '-')
    {
        negative = *digit == '-';
        digit++;
    }
    if (digit[0] == '0' && (digit[1] == 'x' || digit[1] == 'X'))
    {
        base = 16;
        digit += 2;
    }
    if (*digit == '\0')
    {
        return false;
    }
    if (negative)
    {
        limit = (uint64_t)INT64_MAX + 1u;
    }
    for (; *digit != '\0'; digit++)
    {
        const int number = tb_cli_digit_value(*digit);
        if (number < 0 || (uint64_t)number >= base)
        {
            return false;
        }
        if (magnitude > (limit - (uint64_t)number) / base)
        {
            return false;
        }
        magnitude = magnitude * base + (uint64_t)number;
    }
    if (negative && magnitude == (uint64_t)INT64_MAX + 1u)
    {
        *value = INT64_MIN;
    }
    else if (negative)
    {
        *value = -(int64_t)magnitude;
    }
    else
    {
        *value = (int64_t)magnitude;
    }
    return true;
}

static bool tb_cli_has_range(const tb_cli_option *option)
{
    return option->minimum < option->maximum;
}

/* Finds a choice by its exact name. */
static bool tb_cli_find_choice(const tb_cli_option *option, const char *text, size_t *index)
{
    for (size_t choice = 0; choice < option->choice_count; choice++)
    {
        if (tb_str_equal(option->choices[choice], text))
        {
            *index = choice;
            return true;
        }
    }
    return false;
}

static tb_cli_result tb_cli_wrong_choice(tb_cli_parser *parser, const tb_cli_option *option,
                                         const char *label, const char *value)
{
    tb_cli_suggestion suggestion = {0};

    suggestion.typed = value;
    suggestion.typed_length = tb_str_length(value);
    for (size_t choice = 0; choice < option->choice_count; choice++)
    {
        tb_cli_consider(&suggestion, option->choices[choice]);
    }
    (void)tb_cli_fail(parser, "%s: invalid choice '%s'", label, value);
    if (suggestion.best != NULL)
    {
        tb_cli_append(parser, "; did you mean '%s'?", suggestion.best);
        return TB_CLI_ERROR;
    }
    tb_cli_append(parser, " (choose from ");
    for (size_t choice = 0; choice < option->choice_count; choice++)
    {
        tb_cli_append(parser, "%s%s", choice > 0 ? ", " : "", option->choices[choice]);
    }
    tb_cli_append(parser, ")");
    return TB_CLI_ERROR;
}

/* Writes a value into its option's destination, and marks it given. */
static tb_cli_result tb_cli_apply(tb_cli_parser *parser, const tb_cli_found *found,
                                  const char *value)
{
    const tb_cli_option *const option = found->option;
    char label[96] = {0};

    tb_cli_label(option, label, sizeof(label));
    if (option->flag != NULL)
    {
        *option->flag = true;
    }
    else if (option->counter != NULL)
    {
        if (*option->counter < SIZE_MAX)
        {
            (*option->counter)++;
        }
    }
    else if (option->text != NULL)
    {
        *option->text = value;
    }
    else if (option->integer != NULL)
    {
        int64_t integer = 0;
        if (!tb_cli_read_integer(value, &integer))
        {
            return tb_cli_fail(parser, "%s: '%s' is not an integer", label, value);
        }
        if (tb_cli_has_range(option) && (integer < option->minimum || integer > option->maximum))
        {
            return tb_cli_fail(parser, "%s: %lld is not between %lld and %lld", label,
                               (long long)integer, (long long)option->minimum,
                               (long long)option->maximum);
        }
        *option->integer = integer;
    }
    else if (option->number != NULL)
    {
        double number = 0.0;
        if (tb_decimal_parse(value, tb_str_length(value), &number) != TB_DECIMAL_OK ||
            !isfinite(number))
        {
            return tb_cli_fail(parser, "%s: '%s' is not a finite number", label, value);
        }
        *option->number = number;
    }
    else if (option->choice != NULL)
    {
        size_t index = 0;
        if (!tb_cli_find_choice(option, value, &index))
        {
            return tb_cli_wrong_choice(parser, option, label, value);
        }
        *option->choice = index;
    }
    else
    {
        if (*option->count >= option->capacity)
        {
            return tb_cli_fail(parser, "%s: given more than %zu times", label, option->capacity);
        }
        option->texts[*option->count] = value;
        (*option->count)++;
    }
    if (option->given != NULL)
    {
        *option->given = true;
    }
    parser->given[found->depth][found->index] = true;
    return TB_CLI_OK;
}

/* -------------------------------------------------------------------------
 * Reading the command line.
 * ------------------------------------------------------------------------- */

static const tb_cli_command *tb_cli_current(const tb_cli_parser *parser)
{
    return parser->outcome->path[parser->outcome->depth - 1];
}

/* Finds an option of the commands chosen so far, the last chosen first, by
 * its long name (of `length` bytes) or its short one. */
static bool tb_cli_find(const tb_cli_parser *parser, const char *long_name, size_t length,
                        char short_name, tb_cli_found *found)
{
    for (size_t depth = parser->outcome->depth; depth > 0; depth--)
    {
        const tb_cli_command *const command = parser->outcome->path[depth - 1];
        for (size_t index = 0; index < command->option_count; index++)
        {
            const tb_cli_option *const option = &command->options[index];
            const bool by_long = long_name != NULL && option->long_name != NULL &&
                                 tb_str_length(option->long_name) == length &&
                                 memcmp(option->long_name, long_name, length) == 0;
            const bool by_short = short_name != '\0' && option->short_name == short_name;
            if (by_long || by_short)
            {
                found->option = option;
                found->depth = depth - 1;
                found->index = index;
                return true;
            }
        }
    }
    return false;
}

/* Whether the next argument can be an option's value: there is one, and it
 * is not an option. */
static bool tb_cli_next_is_value(const tb_cli_parser *parser)
{
    const char *next = NULL;
    if (parser->index >= parser->count)
    {
        return false;
    }
    next = parser->arguments[parser->index];
    return next[0] != '-' || next[1] == '\0' || tb_cli_is_negative_number(next);
}

static tb_cli_result tb_cli_needs_value(tb_cli_parser *parser, const tb_cli_option *option)
{
    char label[96] = {0};
    tb_cli_label(option, label, sizeof(label));
    return tb_cli_fail(parser, "%s needs a value (%s)", label,
                       option->name != NULL ? option->name : "VALUE");
}

static tb_cli_result tb_cli_unknown_option(tb_cli_parser *parser, const char *name, size_t length)
{
    tb_cli_suggestion suggestion = {0};
    char typed[TB_CLI_SUGGESTION_LENGTH + 1] = {0};

    if (length <= TB_CLI_SUGGESTION_LENGTH)
    {
        memcpy(typed, name, length);
        suggestion.typed = typed;
        suggestion.typed_length = length;
        for (size_t depth = 0; depth < parser->outcome->depth; depth++)
        {
            const tb_cli_command *const command = parser->outcome->path[depth];
            for (size_t index = 0; index < command->option_count; index++)
            {
                if (command->options[index].long_name != NULL)
                {
                    tb_cli_consider(&suggestion, command->options[index].long_name);
                }
            }
        }
        tb_cli_consider(&suggestion, "help");
        if (parser->outcome->path[0]->version != NULL)
        {
            tb_cli_consider(&suggestion, "version");
        }
    }
    (void)tb_cli_fail(parser, "unknown option '--%.*s'", (int)length, name);
    if (suggestion.best != NULL)
    {
        tb_cli_append(parser, "; did you mean '--%s'?", suggestion.best);
    }
    return TB_CLI_ERROR;
}

/* "--name" or "--name=value". */
static tb_cli_result tb_cli_read_long(tb_cli_parser *parser, const char *argument)
{
    const char *const name = argument + 2;
    const size_t equals = tb_str_find_char(name, 0, TB_STR_END, '=');
    const size_t length = equals == TB_STR_NOT_FOUND ? tb_str_length(name) : equals;
    const char *value = NULL;
    tb_cli_found found = {0};

    const bool help = length == 4 && memcmp(name, "help", 4) == 0;
    const bool version =
        length == 7 && memcmp(name, "version", 7) == 0 && parser->outcome->path[0]->version != NULL;

    if ((help || version) && equals != TB_STR_NOT_FOUND)
    {
        return tb_cli_fail(parser, "option '--%.*s' takes no value", (int)length, name);
    }
    if (help)
    {
        parser->outcome->result = TB_CLI_HELP;
        return TB_CLI_HELP;
    }
    if (version)
    {
        parser->outcome->result = TB_CLI_VERSION;
        return TB_CLI_VERSION;
    }
    if (!tb_cli_find(parser, name, length, '\0', &found))
    {
        return tb_cli_unknown_option(parser, name, length);
    }
    TB_ASSERT(found.option != NULL);
    if (!tb_cli_takes_value(found.option))
    {
        if (equals != TB_STR_NOT_FOUND)
        {
            return tb_cli_fail(parser, "option '--%s' takes no value", found.option->long_name);
        }
        return tb_cli_apply(parser, &found, NULL);
    }
    if (equals != TB_STR_NOT_FOUND)
    {
        value = name + equals + 1;
    }
    else if (tb_cli_next_is_value(parser))
    {
        value = parser->arguments[parser->index];
        parser->index++;
    }
    else
    {
        return tb_cli_needs_value(parser, found.option);
    }
    return tb_cli_apply(parser, &found, value);
}

/* "-v", "-vq", "-j4", "-j 4". */
static tb_cli_result tb_cli_read_short(tb_cli_parser *parser, const char *argument)
{
    for (size_t index = 1; argument[index] != '\0'; index++)
    {
        const char letter = argument[index];
        tb_cli_found found = {0};
        tb_cli_result result = TB_CLI_OK;

        if (letter == 'h')
        {
            parser->outcome->result = TB_CLI_HELP;
            return TB_CLI_HELP;
        }
        if (!tb_cli_is_letter(letter) || !tb_cli_find(parser, NULL, 0, letter, &found))
        {
            /* A byte that is not a letter may begin a character of several;
             * the whole argument names it. */
            if (!tb_cli_is_letter(letter))
            {
                return tb_cli_fail(parser, "unknown option '%s'", argument);
            }
            return tb_cli_fail(parser, "unknown option '-%c'", letter);
        }
        if (!tb_cli_takes_value(found.option))
        {
            result = tb_cli_apply(parser, &found, NULL);
            if (result != TB_CLI_OK)
            {
                return result;
            }
            continue;
        }
        if (argument[index + 1] != '\0')
        {
            return tb_cli_apply(parser, &found, argument + index + 1);
        }
        if (!tb_cli_next_is_value(parser))
        {
            return tb_cli_needs_value(parser, found.option);
        }
        parser->index++;
        return tb_cli_apply(parser, &found, parser->arguments[parser->index - 1]);
    }
    return TB_CLI_OK;
}

static tb_cli_result tb_cli_unknown_command(tb_cli_parser *parser, const char *word)
{
    const tb_cli_command *const command = tb_cli_current(parser);
    tb_cli_suggestion suggestion = {0};

    suggestion.typed = word;
    suggestion.typed_length = tb_str_length(word);
    for (size_t index = 0; index < command->command_count; index++)
    {
        tb_cli_consider(&suggestion, command->commands[index].name);
    }
    (void)tb_cli_fail(parser, "unknown command '%s'", word);
    if (suggestion.best != NULL)
    {
        tb_cli_append(parser, "; did you mean '%s'?", suggestion.best);
    }
    return TB_CLI_ERROR;
}

/* The `position`-th argument entry of a command, or NULL. */
static const tb_cli_option *tb_cli_argument_at(const tb_cli_command *command, size_t position,
                                               size_t *index)
{
    size_t seen = 0;
    for (size_t entry = 0; entry < command->option_count; entry++)
    {
        if (!tb_cli_is_argument(&command->options[entry]))
        {
            continue;
        }
        if (seen == position)
        {
            *index = entry;
            return &command->options[entry];
        }
        seen++;
    }
    return NULL;
}

/* A word that is no option: a command's name, or an argument. */
static tb_cli_result tb_cli_read_word(tb_cli_parser *parser, const char *word)
{
    tb_cli_outcome *const outcome = parser->outcome;
    const tb_cli_command *const command = tb_cli_current(parser);
    tb_cli_found found = {0};

    if (command->command_count > 0)
    {
        for (size_t index = 0; index < command->command_count; index++)
        {
            const tb_cli_command *const chosen = &command->commands[index];
            if (!tb_str_equal(chosen->name, word))
            {
                continue;
            }
            TB_ASSERT(outcome->depth < TB_CLI_DEPTH);
            tb_cli_check_command(chosen);
            outcome->path[outcome->depth] = chosen;
            outcome->depth++;
            outcome->command = chosen;
            parser->positional = 0;
            return TB_CLI_OK;
        }
        return tb_cli_unknown_command(parser, word);
    }
    found.option = tb_cli_argument_at(command, parser->positional, &found.index);
    if (found.option == NULL)
    {
        return tb_cli_fail(parser, "unexpected argument '%s'", word);
    }
    found.depth = outcome->depth - 1;
    /* An argument that takes all the rest stays the next one. */
    if (found.option->texts == NULL)
    {
        parser->positional++;
    }
    return tb_cli_apply(parser, &found, word);
}

/* Names what is required and was not given, by every command chosen. */
static tb_cli_result tb_cli_check_required(tb_cli_parser *parser)
{
    tb_cli_outcome *const outcome = parser->outcome;
    const tb_cli_command *const command = tb_cli_current(parser);
    size_t missing = 0;

    if (command->command_count > 0 && !command->command_optional)
    {
        (void)tb_cli_fail(parser, "a command is required (");
        for (size_t index = 0; index < command->command_count; index++)
        {
            tb_cli_append(parser, "%s%s", index > 0 ? ", " : "", command->commands[index].name);
        }
        tb_cli_append(parser, ")");
        return TB_CLI_ERROR;
    }
    for (size_t depth = 0; depth < outcome->depth; depth++)
    {
        const tb_cli_command *const level = outcome->path[depth];
        for (size_t index = 0; index < level->option_count; index++)
        {
            const tb_cli_option *const option = &level->options[index];
            if (!option->required || parser->given[depth][index])
            {
                continue;
            }
            if (missing == 0)
            {
                (void)tb_cli_fail(parser, "the following are required: ");
            }
            else
            {
                tb_cli_append(parser, ", ");
            }
            if (tb_cli_is_argument(option))
            {
                tb_cli_append(parser, "%s", option->name);
            }
            else if (option->long_name != NULL)
            {
                tb_cli_append(parser, "--%s", option->long_name);
            }
            else
            {
                tb_cli_append(parser, "-%c", option->short_name);
            }
            missing++;
        }
    }
    if (missing > 0)
    {
        return TB_CLI_ERROR;
    }
    return TB_CLI_OK;
}

/* The program's name from its first argument: without a directory, or an
 * ".exe" in any case. */
static void tb_cli_program_name(const tb_cli_command *program, const char *first,
                                tb_cli_outcome *outcome)
{
    const char *name = program->name;
    size_t length = 0;
    size_t written = 0;

    if (name == NULL)
    {
        const size_t slash = tb_str_rfind_any(first, 0, TB_STR_END, "/\\");
        name = slash == TB_STR_NOT_FOUND ? first : first + slash + 1;
    }
    length = tb_str_length(name);
    if (program->name == NULL && length >= 4 && name[length - 4] == '.' &&
        (name[length - 3] == 'e' || name[length - 3] == 'E') &&
        (name[length - 2] == 'x' || name[length - 2] == 'X') &&
        (name[length - 1] == 'e' || name[length - 1] == 'E'))
    {
        length -= 4;
    }
    if (length == 0)
    {
        name = "program";
        length = tb_str_length(name);
    }
    written = tb_fmt_buffer(outcome->program, sizeof(outcome->program), "%.*s", (int)length, name);
    tb_cli_cut_at_character(outcome->program, written, sizeof(outcome->program));
}

tb_cli_result tb_cli_parse(const tb_cli_command *program, size_t count,
                           const char *const *arguments, tb_cli_outcome *outcome)
{
    tb_cli_parser parser = {0};
    tb_cli_result result = TB_CLI_OK;

    TB_ASSERT(program != NULL);
    TB_ASSERT(arguments != NULL && count >= 1);
    TB_ASSERT(outcome != NULL);
    memset(outcome, 0, sizeof(*outcome));
    outcome->path[0] = program;
    outcome->depth = 1;
    outcome->command = program;
    tb_cli_program_name(program, arguments[0], outcome);
    tb_cli_check_command(program);
    parser.outcome = outcome;
    parser.count = count;
    parser.arguments = arguments;
    parser.index = 1;
    while (result == TB_CLI_OK && parser.index < count)
    {
        const char *const argument = arguments[parser.index];
        TB_ASSERT(argument != NULL);
        parser.index++;
        if (parser.options_ended || argument[0] != '-' || argument[1] == '\0' ||
            tb_cli_is_negative_number(argument))
        {
            result = tb_cli_read_word(&parser, argument);
        }
        else if (argument[1] == '-' && argument[2] == '\0')
        {
            parser.options_ended = true;
        }
        else if (argument[1] == '-')
        {
            result = tb_cli_read_long(&parser, argument);
        }
        else
        {
            result = tb_cli_read_short(&parser, argument);
        }
    }
    if (result == TB_CLI_OK)
    {
        result = tb_cli_check_required(&parser);
    }
    if (result == TB_CLI_OK)
    {
        outcome->message[0] = '\0';
    }
    outcome->result = result;
    return result;
}

/* -------------------------------------------------------------------------
 * Writing help.
 * ------------------------------------------------------------------------- */

typedef struct tb_cli_writer
{
    tb_fmt_sink *sink;
    void *context;
    size_t column;
} tb_cli_writer;

static void tb_cli_put(tb_cli_writer *writer, const char *bytes, size_t length)
{
    if (length == 0)
    {
        return;
    }
    writer->sink(writer->context, bytes, length);
    for (size_t index = 0; index < length; index++)
    {
        if (bytes[index] == '\n')
        {
            writer->column = 0;
        }
        else if (!tb_cli_is_continuation(bytes[index]))
        {
            writer->column++;
        }
    }
}

static void tb_cli_puts(tb_cli_writer *writer, const char *text)
{
    tb_cli_put(writer, text, tb_str_length(text));
}

static void tb_cli_spaces(tb_cli_writer *writer, size_t count)
{
    static const char spaces[] = "                                ";
    while (count > 0)
    {
        const size_t piece = count < sizeof(spaces) - 1 ? count : sizeof(spaces) - 1;
        tb_cli_put(writer, spaces, piece);
        count -= piece;
    }
}

/* Writes one word where the line has room for it, or on the next line at
 * `indent`. The first word after `indent` is never moved: a word wider than
 * the line stands on a line of its own. */
static void tb_cli_word(tb_cli_writer *writer, const char *word, size_t length, size_t indent)
{
    const size_t width = tb_cli_width(word, length);
    if (writer->column > indent)
    {
        if (writer->column + 1 + width > TB_CLI_WIDTH)
        {
            tb_cli_puts(writer, "\n");
            tb_cli_spaces(writer, indent);
        }
        else
        {
            tb_cli_puts(writer, " ");
        }
    }
    else if (writer->column < indent)
    {
        tb_cli_spaces(writer, indent - writer->column);
    }
    tb_cli_put(writer, word, length);
}

/* Writes text of `length` bytes as words, wrapped at the help's width, with
 * lines after the first at `indent`. Spaces and line breaks in the text
 * both only separate words. */
static void tb_cli_wrap(tb_cli_writer *writer, const char *text, size_t length, size_t indent)
{
    size_t index = 0;
    while (index < length)
    {
        size_t end = index;
        if (text[index] == ' ' || text[index] == '\n')
        {
            index++;
            continue;
        }
        while (end < length && text[end] != ' ' && text[end] != '\n')
        {
            end++;
        }
        tb_cli_word(writer, text + index, end - index, indent);
        index = end;
    }
}

/* Writes paragraphs, each ended by a blank line in the text, wrapped, with
 * a blank line after each. */
static void tb_cli_paragraphs(tb_cli_writer *writer, const char *text)
{
    const size_t length = tb_str_length(text);
    size_t begin = 0;
    while (begin < length)
    {
        size_t end = tb_str_find(text, begin, length, "\n\n");
        if (end == TB_STR_NOT_FOUND)
        {
            end = length;
        }
        if (tb_cli_width(text + begin, end - begin) > 0)
        {
            tb_cli_wrap(writer, text + begin, end - begin, 0);
            tb_cli_puts(writer, "\n\n");
        }
        begin = end + 2;
    }
}

/* The name of an option's value in the help: its name, or its choices, or
 * "VALUE". */
static void tb_cli_value_name(const tb_cli_option *option, char *text, size_t capacity)
{
    size_t used = 0;
    if (option->name != NULL || option->choice == NULL)
    {
        used = tb_fmt_buffer(text, capacity, "%s", option->name != NULL ? option->name : "VALUE");
        tb_cli_cut_at_character(text, used, capacity);
        return;
    }
    used = tb_fmt_buffer(text, capacity, "{");
    for (size_t choice = 0; choice < option->choice_count && used < capacity; choice++)
    {
        used += tb_fmt_buffer(text + used, capacity - used, "%s%s", choice > 0 ? "," : "",
                              option->choices[choice]);
    }
    if (used < capacity)
    {
        used += tb_fmt_buffer(text + used, capacity - used, "}");
    }
    tb_cli_cut_at_character(text, used, capacity);
}

/* What the help names an entry by: "-j, --jobs N", "    --release", "FILE". */
static void tb_cli_entry_name(const tb_cli_option *option, char *text, size_t capacity)
{
    char value[160] = {0};
    size_t used = 0;

    if (tb_cli_is_argument(option))
    {
        used = tb_fmt_buffer(text, capacity, "%s", option->name);
        tb_cli_cut_at_character(text, used, capacity);
        return;
    }
    if (tb_cli_takes_value(option))
    {
        tb_cli_value_name(option, value, sizeof(value));
    }
    if (option->short_name != '\0' && option->long_name != NULL)
    {
        used = tb_fmt_buffer(text, capacity, "-%c, --%s", option->short_name, option->long_name);
    }
    else if (option->short_name != '\0')
    {
        used = tb_fmt_buffer(text, capacity, "-%c", option->short_name);
    }
    else
    {
        used = tb_fmt_buffer(text, capacity, "    --%s", option->long_name);
    }
    if (value[0] != '\0' && used < capacity)
    {
        used += tb_fmt_buffer(text + used, capacity - used, " %s", value);
    }
    tb_cli_cut_at_character(text, used, capacity);
}

/* What the help adds after an entry's own words: its range, its choices,
 * whether it repeats, and its default. */
static void tb_cli_entry_notes(const tb_cli_option *option, char *text, size_t capacity)
{
    size_t used = 0;
    const char *separator = "";

    text[0] = '\0';
    if (option->integer != NULL && tb_cli_has_range(option))
    {
        used += tb_fmt_buffer(text + used, capacity - used, "%s%lld to %lld", separator,
                              (long long)option->minimum, (long long)option->maximum);
        separator = "; ";
    }
    if (option->choice != NULL && option->name != NULL && used < capacity)
    {
        used += tb_fmt_buffer(text + used, capacity - used, "%sone of:", separator);
        for (size_t choice = 0; choice < option->choice_count && used < capacity; choice++)
        {
            used += tb_fmt_buffer(text + used, capacity - used, "%s %s", choice > 0 ? "," : "",
                                  option->choices[choice]);
        }
        separator = "; ";
    }
    if (option->texts != NULL && !tb_cli_is_argument(option) && used < capacity)
    {
        used += tb_fmt_buffer(text + used, capacity - used, "%sup to %zu times", separator,
                              option->capacity);
        separator = "; ";
    }
    if (!option->required && used < capacity)
    {
        if (option->text != NULL && *option->text != NULL)
        {
            used += tb_fmt_buffer(text + used, capacity - used, "%sdefault: %s", separator,
                                  *option->text);
        }
        else if (option->integer != NULL)
        {
            used += tb_fmt_buffer(text + used, capacity - used, "%sdefault: %lld", separator,
                                  (long long)*option->integer);
        }
        else if (option->number != NULL)
        {
            char number[TB_DECIMAL_SHORTEST_SIZE] = {0};
            size_t number_length = 0;
            (void)tb_decimal_shortest(*option->number, number, sizeof(number), &number_length);
            used += tb_fmt_buffer(text + used, capacity - used, "%sdefault: %s", separator, number);
        }
        else if (option->choice != NULL && *option->choice < option->choice_count)
        {
            used += tb_fmt_buffer(text + used, capacity - used, "%sdefault: %s", separator,
                                  option->choices[*option->choice]);
        }
    }
    if (used >= capacity)
    {
        tb_cli_cut_at_character(text, used, capacity);
    }
}

/* One row of a list: its name in the first column, and its help wrapped in
 * the second. */
static void tb_cli_row(tb_cli_writer *writer, const char *name, const char *help, const char *notes,
                       size_t column)
{
    tb_cli_puts(writer, "  ");
    tb_cli_puts(writer, name);
    if (writer->column + 2 > column)
    {
        tb_cli_puts(writer, "\n");
    }
    if (help != NULL)
    {
        tb_cli_wrap(writer, help, tb_str_length(help), column);
    }
    if (notes != NULL && notes[0] != '\0')
    {
        char bracketed[TB_CLI_MESSAGE_CAPACITY] = {0};
        const size_t used = tb_fmt_buffer(bracketed, sizeof(bracketed), "(%s)", notes);
        tb_cli_cut_at_character(bracketed, used, sizeof(bracketed));
        tb_cli_wrap(writer, bracketed, tb_str_length(bracketed), column);
    }
    tb_cli_puts(writer, "\n");
}

/* The column where the help of a command's rows begins. */
static size_t tb_cli_column(const tb_cli_outcome *outcome)
{
    const tb_cli_command *const command = outcome->command;
    char name[256] = {0};
    size_t widest = tb_str_length("-h, --help");

    if (outcome->depth == 1 && command->version != NULL)
    {
        widest = tb_str_length("    --version") > widest ? tb_str_length("    --version") : widest;
    }
    for (size_t depth = 0; depth < outcome->depth; depth++)
    {
        const tb_cli_command *const level = outcome->path[depth];
        for (size_t index = 0; index < level->option_count; index++)
        {
            size_t width = 0;
            tb_cli_entry_name(&level->options[index], name, sizeof(name));
            width = tb_cli_width(name, tb_str_length(name));
            widest = width > widest ? width : widest;
        }
    }
    for (size_t index = 0; index < command->command_count; index++)
    {
        const char *const command_name = command->commands[index].name;
        const size_t width = tb_cli_width(command_name, tb_str_length(command_name));
        widest = width > widest ? width : widest;
    }
    /* Two spaces before a name and two after it. */
    if (widest + 4 > TB_CLI_NAME_COLUMN)
    {
        return TB_CLI_NAME_COLUMN;
    }
    return widest + 4;
}

/* Writes the options of one command of the path. */
static void tb_cli_option_rows(tb_cli_writer *writer, const tb_cli_command *command, size_t column)
{
    char name[256] = {0};
    char notes[TB_CLI_MESSAGE_CAPACITY] = {0};
    for (size_t index = 0; index < command->option_count; index++)
    {
        const tb_cli_option *const option = &command->options[index];
        if (tb_cli_is_argument(option))
        {
            continue;
        }
        tb_cli_entry_name(option, name, sizeof(name));
        tb_cli_entry_notes(option, notes, sizeof(notes));
        tb_cli_row(writer, name, option->help, notes, column);
    }
}

static bool tb_cli_has_options(const tb_cli_command *command)
{
    for (size_t index = 0; index < command->option_count; index++)
    {
        if (!tb_cli_is_argument(&command->options[index]))
        {
            return true;
        }
    }
    return false;
}

/* The words of a usage line after "usage:", each written by tb_cli_word. */
static void tb_cli_usage_words(tb_cli_writer *writer, const tb_cli_outcome *outcome, size_t indent)
{
    const tb_cli_command *const command = outcome->command;
    char word[256] = {0};

    tb_cli_word(writer, outcome->program, tb_str_length(outcome->program), indent);
    for (size_t depth = 1; depth < outcome->depth; depth++)
    {
        const char *const name = outcome->path[depth]->name;
        tb_cli_word(writer, name, tb_str_length(name), indent);
    }
    tb_cli_word(writer, "[options]", 9, indent);
    for (size_t depth = 0; depth < outcome->depth; depth++)
    {
        const tb_cli_command *const level = outcome->path[depth];
        for (size_t index = 0; index < level->option_count; index++)
        {
            const tb_cli_option *const option = &level->options[index];
            char value[160] = {0};
            size_t used = 0;
            if (tb_cli_is_argument(option) || !option->required)
            {
                continue;
            }
            if (tb_cli_takes_value(option))
            {
                tb_cli_value_name(option, value, sizeof(value));
            }
            if (option->long_name != NULL)
            {
                used = tb_fmt_buffer(word, sizeof(word), "--%s%s%s", option->long_name,
                                     value[0] != '\0' ? " " : "", value);
            }
            else
            {
                used = tb_fmt_buffer(word, sizeof(word), "-%c%s%s", option->short_name,
                                     value[0] != '\0' ? " " : "", value);
            }
            tb_cli_cut_at_character(word, used, sizeof(word));
            tb_cli_word(writer, word, tb_str_length(word), indent);
        }
    }
    for (size_t index = 0; index < command->option_count; index++)
    {
        const tb_cli_option *const option = &command->options[index];
        const char *const dots = option->texts != NULL ? "..." : "";
        size_t used = 0;
        if (!tb_cli_is_argument(option))
        {
            continue;
        }
        if (option->required)
        {
            used = tb_fmt_buffer(word, sizeof(word), "%s%s", option->name, dots);
        }
        else
        {
            used = tb_fmt_buffer(word, sizeof(word), "[%s%s]", option->name, dots);
        }
        tb_cli_cut_at_character(word, used, sizeof(word));
        tb_cli_word(writer, word, tb_str_length(word), indent);
    }
    if (command->command_count > 0)
    {
        if (command->command_optional)
        {
            tb_cli_word(writer, "[<command> ...]", 15, indent);
        }
        else
        {
            tb_cli_word(writer, "<command> ...", 13, indent);
        }
    }
}

static tb_cli_writer tb_cli_start(tb_fmt_sink *sink, void *context)
{
    tb_cli_writer writer = {0};
    TB_ASSERT(sink != NULL);
    writer.sink = sink;
    writer.context = context;
    return writer;
}

static void tb_cli_check_outcome(const tb_cli_outcome *outcome)
{
    TB_ASSERT(outcome != NULL);
    TB_ASSERT(outcome->depth >= 1 && outcome->depth <= TB_CLI_DEPTH);
    TB_ASSERT(outcome->command == outcome->path[outcome->depth - 1]);
}

void tb_cli_write_usage(const tb_cli_outcome *outcome, tb_fmt_sink *sink, void *context)
{
    tb_cli_writer writer = tb_cli_start(sink, context);
    tb_cli_check_outcome(outcome);
    tb_cli_puts(&writer, "usage:");
    /* Lines after the first begin under the program's name. */
    tb_cli_usage_words(&writer, outcome, tb_str_length("usage: "));
    tb_cli_puts(&writer, "\n");
}

void tb_cli_write_help(const tb_cli_outcome *outcome, tb_fmt_sink *sink, void *context)
{
    tb_cli_writer writer = tb_cli_start(sink, context);
    const tb_cli_command *command = NULL;
    char name[256] = {0};
    char notes[TB_CLI_MESSAGE_CAPACITY] = {0};
    size_t column = 0;
    bool arguments = false;

    tb_cli_check_outcome(outcome);
    command = outcome->command;
    column = tb_cli_column(outcome);
    tb_cli_write_usage(outcome, sink, context);
    tb_cli_puts(&writer, "\n");
    if (command->description != NULL)
    {
        tb_cli_paragraphs(&writer, command->description);
    }
    else if (command->summary != NULL)
    {
        tb_cli_paragraphs(&writer, command->summary);
    }
    for (size_t index = 0; index < command->option_count; index++)
    {
        const tb_cli_option *const option = &command->options[index];
        if (!tb_cli_is_argument(option))
        {
            continue;
        }
        if (!arguments)
        {
            tb_cli_puts(&writer, "arguments:\n");
            arguments = true;
        }
        tb_cli_entry_name(option, name, sizeof(name));
        tb_cli_entry_notes(option, notes, sizeof(notes));
        tb_cli_row(&writer, name, option->help, notes, column);
    }
    if (arguments)
    {
        tb_cli_puts(&writer, "\n");
    }
    tb_cli_puts(&writer, "options:\n");
    tb_cli_row(&writer, "-h, --help", "Show this help and exit.", NULL, column);
    if (outcome->depth == 1 && command->version != NULL)
    {
        tb_cli_row(&writer, "    --version", "Show the version and exit.", NULL, column);
    }
    tb_cli_option_rows(&writer, command, column);
    tb_cli_puts(&writer, "\n");
    /* The options of the commands above, which are accepted here too. */
    for (size_t depth = outcome->depth - 1; depth > 0; depth--)
    {
        const tb_cli_command *const level = outcome->path[depth - 1];
        if (!tb_cli_has_options(level))
        {
            continue;
        }
        tb_cli_puts(&writer, "options of ");
        tb_cli_puts(&writer, outcome->program);
        for (size_t above = 1; above < depth; above++)
        {
            tb_cli_puts(&writer, " ");
            tb_cli_puts(&writer, outcome->path[above]->name);
        }
        tb_cli_puts(&writer, ":\n");
        tb_cli_option_rows(&writer, level, column);
        tb_cli_puts(&writer, "\n");
    }
    if (command->command_count > 0)
    {
        tb_cli_puts(&writer, "commands:\n");
        for (size_t index = 0; index < command->command_count; index++)
        {
            tb_cli_row(&writer, command->commands[index].name, command->commands[index].summary,
                       NULL, column);
        }
        tb_cli_puts(&writer, "\n");
    }
    if (command->epilog != NULL)
    {
        tb_cli_paragraphs(&writer, command->epilog);
    }
}

/* -------------------------------------------------------------------------
 * Finishing.
 * ------------------------------------------------------------------------- */

static void tb_cli_to_output(void *context, const char *bytes, size_t length)
{
    (void)context;
    (void)tb_fmt_print("%.*s", (int)length, bytes);
}

static void tb_cli_to_error(void *context, const char *bytes, size_t length)
{
    (void)context;
    (void)tb_fmt_print_error("%.*s", (int)length, bytes);
}

int tb_cli_finish(const tb_cli_outcome *outcome)
{
    tb_cli_check_outcome(outcome);
    switch (outcome->result)
    {
    case TB_CLI_OK:
        return EXIT_SUCCESS;
    case TB_CLI_HELP:
        tb_cli_write_help(outcome, tb_cli_to_output, NULL);
        return tb_fmt_flush() ? EXIT_SUCCESS : EXIT_FAILURE;
    case TB_CLI_VERSION:
        (void)tb_fmt_print("%s %s\n", outcome->program, outcome->path[0]->version);
        return tb_fmt_flush() ? EXIT_SUCCESS : EXIT_FAILURE;
    case TB_CLI_ERROR:
        break;
    }
    tb_cli_write_usage(outcome, tb_cli_to_error, NULL);
    (void)tb_fmt_print_error("%s", outcome->program);
    for (size_t depth = 1; depth < outcome->depth; depth++)
    {
        (void)tb_fmt_print_error(" %s", outcome->path[depth]->name);
    }
    (void)tb_fmt_print_error(": error: %s\n", outcome->message);
    return tb_fmt_flush_error() ? TB_CLI_EXIT_USAGE : EXIT_FAILURE;
}

int tb_cli_run(const tb_cli_command *program, size_t count, const char *const *arguments,
               void *context)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;
    if (tb_cli_parse(program, count, arguments, &outcome) != TB_CLI_OK)
    {
        return tb_cli_finish(&outcome);
    }
    TB_ASSERT(outcome.command != NULL && outcome.command->run != NULL);
    return outcome.command->run(&outcome, context);
}

const char *tb_cli_result_name(tb_cli_result result)
{
    switch (result)
    {
    case TB_CLI_OK:
        return "ok";
    case TB_CLI_HELP:
        return "help asked for";
    case TB_CLI_VERSION:
        return "version asked for";
    case TB_CLI_ERROR:
        return "wrong command line";
    }
    return "unknown result";
}
