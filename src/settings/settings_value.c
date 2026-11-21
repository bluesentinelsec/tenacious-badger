/* Values as text: reading them from a file, the command line or a console,
 * writing them to a file, and saying why text is not a value. */
#include "settings_internal.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/utf8.h"

#include <string.h>

/* The most bytes of a value that a problem's message quotes. */
#define TB_SETTINGS_QUOTED_VALUE ((size_t)48)

/* ---------------------------------------------------------------------------
 * Quotes.
 * ------------------------------------------------------------------------- */

/* What an escape stands for, or 0 for no escape. */
static char tb_settings_escaped(char character)
{
    switch (character)
    {
    case '"':
        return '"';
    case '\\':
        return '\\';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    default:
        return 0;
    }
}

/* What a character is written as inside quotes: its escape, or 0 when it is
 * written as itself. */
static char tb_settings_escape(char character)
{
    switch (character)
    {
    case '"':
        return '"';
    case '\\':
        return '\\';
    case '\n':
        return 'n';
    case '\r':
        return 'r';
    case '\t':
        return 't';
    default:
        return 0;
    }
}

static char *tb_settings_copy_text(const char *text, size_t length)
{
    char *const copy = tb_malloc(length + 1);

    if (length > 0)
    {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}

char *tb_settings_unquote(const char *text, size_t length, size_t *unquoted_length,
                          const char **problem)
{
    char *unquoted = NULL;
    size_t written = 0;
    size_t index = 1;

    *problem = NULL;
    if (length == 0 || text[0] != '"')
    {
        *unquoted_length = length;
        return tb_settings_copy_text(text, length);
    }
    unquoted = tb_malloc(length);
    while (index < length && text[index] != '"')
    {
        char character = text[index];
        if (character == '\\')
        {
            if (index + 1 == length || tb_settings_escaped(text[index + 1]) == 0)
            {
                *problem = "an unknown escape in quoted text";
                goto failed;
            }
            character = tb_settings_escaped(text[index + 1]);
            index++;
        }
        unquoted[written] = character;
        written++;
        index++;
    }
    if (index == length)
    {
        *problem = "quoted text without its closing quote";
        goto failed;
    }
    if (index + 1 != length)
    {
        *problem = "more after the closing quote";
        goto failed;
    }
    unquoted[written] = '\0';
    *unquoted_length = written;
    return unquoted;

failed:
    tb_free(unquoted);
    *unquoted_length = 0;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

bool tb_settings_valid_utf8(const char *text, size_t length)
{
    size_t error = 0;

    return tb_utf8_validate(text, length, &error);
}

static char tb_settings_lower(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return (char)(character - 'A' + 'a');
    }
    return character;
}

/* Whether text is a lowercase word, ignoring the case of ASCII letters. */
static bool tb_settings_is_word(const char *word, const char *text, size_t length)
{
    if (tb_str_length(word) != length)
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        if (tb_settings_lower(text[index]) != word[index])
        {
            return false;
        }
    }
    return true;
}

static tb_setting_result tb_settings_read_boolean(const char *text, size_t length, bool *value)
{
    static const char *const yes[] = {"true", "on", "yes", "1"};
    static const char *const no[] = {"false", "off", "no", "0"};

    for (size_t index = 0; index < sizeof(yes) / sizeof(yes[0]); index++)
    {
        if (tb_settings_is_word(yes[index], text, length))
        {
            *value = true;
            return TB_SETTING_OK;
        }
        if (tb_settings_is_word(no[index], text, length))
        {
            *value = false;
            return TB_SETTING_OK;
        }
    }
    return TB_SETTING_INVALID;
}

/* Decimal digits with an optional sign. A value that does not fit in 64 bits
 * is out of range, as every setting's range is narrower. */
static tb_setting_result tb_settings_read_integer(const char *text, size_t length, int64_t *value)
{
    const bool negative = length > 0 && text[0] == '-';
    const size_t first = length > 0 && (text[0] == '-' || text[0] == '+') ? 1u : 0u;
    /* The magnitude of INT64_MIN, the largest a negative value may have. */
    const uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t magnitude = 0;
    bool too_large = false;

    if (first == length)
    {
        return TB_SETTING_INVALID;
    }
    for (size_t index = first; index < length; index++)
    {
        const char character = text[index];
        uint64_t digit = 0;
        if (character < '0' || character > '9')
        {
            return TB_SETTING_INVALID;
        }
        digit = (uint64_t)(character - '0');
        if (magnitude > (limit - digit) / 10u)
        {
            too_large = true;
        }
        else
        {
            magnitude = magnitude * 10u + digit;
        }
    }
    if (too_large)
    {
        return TB_SETTING_OUT_OF_RANGE;
    }
    if (negative)
    {
        /* -(magnitude - 1) - 1 reaches INT64_MIN without overflowing. */
        *value = magnitude == 0 ? 0 : -(int64_t)(magnitude - 1u) - 1;
    }
    else
    {
        *value = (int64_t)magnitude;
    }
    return TB_SETTING_OK;
}

static tb_setting_result tb_settings_read_text(const tb_setting *setting, const char *text,
                                               size_t length, tb_setting_value *value)
{
    if (memchr(text, '\0', length) != NULL || !tb_settings_valid_utf8(text, length))
    {
        return TB_SETTING_INVALID;
    }
    if (length > setting->text_limit)
    {
        return TB_SETTING_OUT_OF_RANGE;
    }
    value->text = tb_settings_copy_text(text, length);
    return TB_SETTING_OK;
}

size_t tb_setting_choice_index(const tb_setting *setting, const char *text, size_t length)
{
    for (size_t index = 0; index < setting->choice_count; index++)
    {
        if (tb_settings_is_word(setting->choices[index], text, length))
        {
            return index;
        }
    }
    return setting->choice_count;
}

tb_setting_result tb_setting_read_value(const tb_setting *setting, const char *text, size_t length,
                                        tb_setting_value *value)
{
    tb_setting_result result = TB_SETTING_OK;

    *value = (tb_setting_value){0};
    switch (setting->type)
    {
    case TB_SETTING_BOOLEAN:
        return tb_settings_read_boolean(text, length, &value->boolean);
    case TB_SETTING_INTEGER:
        result = tb_settings_read_integer(text, length, &value->integer);
        if (result == TB_SETTING_OK && (value->integer < setting->integer_minimum ||
                                        value->integer > setting->integer_maximum))
        {
            result = TB_SETTING_OUT_OF_RANGE;
        }
        return result;
    case TB_SETTING_NUMBER:
        if (tb_decimal_parse(text, length, &value->number) != TB_DECIMAL_OK)
        {
            return TB_SETTING_INVALID;
        }
        /* Written so that NaN, which compares false with everything, fails. */
        if (!(value->number >= setting->number_minimum && value->number <= setting->number_maximum))
        {
            return TB_SETTING_OUT_OF_RANGE;
        }
        return TB_SETTING_OK;
    case TB_SETTING_CHOICE:
        value->choice = tb_setting_choice_index(setting, text, length);
        return value->choice < setting->choice_count ? TB_SETTING_OK : TB_SETTING_OUT_OF_RANGE;
    case TB_SETTING_TEXT:
        return tb_settings_read_text(setting, text, length, value);
    }
    return TB_SETTING_INVALID;
}

/* ---------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

/* Text written in quotes, with escapes. */
static size_t tb_settings_write_quoted(const char *text, char *buffer, size_t capacity)
{
    const size_t length = tb_str_length(text);
    size_t needed = 2;
    size_t written = 0;

    for (size_t index = 0; index < length; index++)
    {
        needed += tb_settings_escape(text[index]) != 0 ? 2u : 1u;
    }
    if (needed >= capacity)
    {
        return needed;
    }
    buffer[written] = '"';
    written++;
    for (size_t index = 0; index < length; index++)
    {
        const char escape = tb_settings_escape(text[index]);
        if (escape != 0)
        {
            buffer[written] = '\\';
            written++;
            buffer[written] = escape;
        }
        else
        {
            buffer[written] = text[index];
        }
        written++;
    }
    buffer[written] = '"';
    buffer[written + 1] = '\0';
    return needed;
}

/* A word, as tb_fmt_buffer() would, but without measuring twice. */
static size_t tb_settings_write_word(const char *word, char *buffer, size_t capacity)
{
    const size_t length = tb_str_length(word);

    if (length < capacity)
    {
        memcpy(buffer, word, length + 1);
    }
    return length;
}

size_t tb_setting_write_value(const tb_setting *setting, const tb_setting_value *value,
                              char *buffer, size_t capacity)
{
    switch (setting->type)
    {
    case TB_SETTING_BOOLEAN:
        return tb_settings_write_word(value->boolean ? "true" : "false", buffer, capacity);
    case TB_SETTING_INTEGER:
        return tb_fmt_buffer(buffer, capacity, "%lld", (long long)value->integer);
    case TB_SETTING_NUMBER: {
        char number[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;
        (void)tb_decimal_shortest(value->number, number, sizeof(number), &required);
        return tb_settings_write_word(number, buffer, capacity);
    }
    case TB_SETTING_CHOICE:
        return tb_settings_write_word(setting->choices[value->choice], buffer, capacity);
    case TB_SETTING_TEXT:
        return tb_settings_write_quoted(value->text, buffer, capacity);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Problems.
 * ------------------------------------------------------------------------- */

void tb_setting_describe_failure(const tb_setting *setting, tb_setting_result result,
                                 const char *text, size_t length, char *buffer, size_t capacity)
{
    const int shown = (int)(length < TB_SETTINGS_QUOTED_VALUE ? length : TB_SETTINGS_QUOTED_VALUE);
    const char *const more = length > TB_SETTINGS_QUOTED_VALUE ? "..." : "";

    if (result == TB_SETTING_INVALID)
    {
        static const char *const expected[] = {
            [TB_SETTING_BOOLEAN] = "true or false", [TB_SETTING_INTEGER] = "an integer",
            [TB_SETTING_NUMBER] = "a number",       [TB_SETTING_CHOICE] = "a choice",
            [TB_SETTING_TEXT] = "valid UTF-8 text",
        };
        (void)tb_fmt_buffer(buffer, capacity, "'%s': '%.*s%s' is not %s", setting->name, shown,
                            text, more, expected[setting->type]);
        return;
    }
    switch (setting->type)
    {
    case TB_SETTING_INTEGER:
        (void)tb_fmt_buffer(buffer, capacity, "'%s': '%.*s%s' is not from %lld to %lld",
                            setting->name, shown, text, more, (long long)setting->integer_minimum,
                            (long long)setting->integer_maximum);
        return;
    case TB_SETTING_NUMBER: {
        char minimum[TB_DECIMAL_SHORTEST_SIZE] = {0};
        char maximum[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;
        (void)tb_decimal_shortest(setting->number_minimum, minimum, sizeof(minimum), &required);
        (void)tb_decimal_shortest(setting->number_maximum, maximum, sizeof(maximum), &required);
        (void)tb_fmt_buffer(buffer, capacity, "'%s': '%.*s%s' is not from %s to %s", setting->name,
                            shown, text, more, minimum, maximum);
        return;
    }
    case TB_SETTING_CHOICE:
        (void)tb_fmt_buffer(buffer, capacity, "'%s' has no choice '%.*s%s'", setting->name, shown,
                            text, more);
        return;
    case TB_SETTING_TEXT:
        (void)tb_fmt_buffer(buffer, capacity, "'%s': the text is longer than %zu bytes",
                            setting->name, setting->text_limit);
        return;
    case TB_SETTING_BOOLEAN:
        break;
    }
    (void)tb_fmt_buffer(buffer, capacity, "'%s': '%.*s%s' does not fit", setting->name, shown, text,
                        more);
}
