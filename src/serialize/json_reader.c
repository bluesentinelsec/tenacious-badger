/* The JSON reader: a state machine over the text, with a stack of the open
 * containers as deep as the limit allows. It never recurses, never copies the
 * text, and checks every string and number fully before reporting it. */
#include "badger/json.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/utf8.h"

#include <string.h>

#define TB_JSON_KNOWN_FLAGS                                                                        \
    ((unsigned)TB_JSON_ALLOW_COMMENTS | (unsigned)TB_JSON_ALLOW_TRAILING_COMMAS |                  \
     (unsigned)TB_JSON_ALLOW_DUPLICATE_KEYS)

/* What the reader expects next. */
typedef enum tb_json_expect
{
    TB_JSON_EXPECT_DOCUMENT,    /* The one value of the document. */
    TB_JSON_EXPECT_FIRST_VALUE, /* After `[`: a value or `]`. */
    TB_JSON_EXPECT_VALUE, /* After `,` in an array: a value, or the end after a trailing comma. */
    TB_JSON_EXPECT_MEMBER_VALUE, /* After `:`: a value, and nothing else. */
    TB_JSON_EXPECT_FIRST_KEY,    /* After `{`: a key or `}`. */
    TB_JSON_EXPECT_KEY,          /* After `,` in an object: a key. */
    TB_JSON_EXPECT_COLON,        /* After a key. */
    TB_JSON_EXPECT_SEPARATOR,    /* After a value in a container: `,` or its end. */
    TB_JSON_EXPECT_END,          /* After the document's value: nothing but space. */
    TB_JSON_EXPECT_NOTHING       /* The end was reported. */
} tb_json_expect;

/* What a container on the stack is. */
#define TB_JSON_IN_ARRAY ((unsigned char)'[')
#define TB_JSON_IN_OBJECT ((unsigned char)'{')

struct tb_json_reader
{
    tb_allocator allocator;
    const char *text;
    size_t length;
    size_t position;
    unsigned flags;
    size_t max_depth;
    size_t max_length;
    tb_json_expect expect;
    bool after_comma; /* The last separator was a comma, for trailing commas. */
    unsigned char *stack;
    size_t depth;
    tb_json_error error;
};

/* -------------------------------------------------------------------------
 * Failure.
 * ------------------------------------------------------------------------- */

static tb_json_result tb_json_fail(tb_json_reader *reader, tb_json_result result, size_t offset,
                                   const char *reason)
{
    size_t line = 1;
    size_t line_start = 0;
    if (offset > reader->length)
    {
        offset = reader->length;
    }
    for (size_t index = 0; index < offset; index++)
    {
        if (reader->text[index] == '\n')
        {
            line++;
            line_start = index + 1;
        }
    }
    reader->error.result = result;
    reader->error.offset = offset;
    reader->error.line = line;
    reader->error.column = offset - line_start + 1;
    reader->error.reason = reason;
    return result;
}

/* -------------------------------------------------------------------------
 * Space and comments.
 * ------------------------------------------------------------------------- */

static bool tb_json_is_space(char character)
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

/* Skip space and, when allowed, comments. False for an unterminated comment. */
static bool tb_json_skip_space(tb_json_reader *reader)
{
    const char *const text = reader->text;
    for (;;)
    {
        while (reader->position < reader->length && tb_json_is_space(text[reader->position]))
        {
            reader->position++;
        }
        if ((reader->flags & TB_JSON_ALLOW_COMMENTS) == 0 ||
            reader->position + 1 >= reader->length || text[reader->position] != '/')
        {
            return true;
        }
        if (text[reader->position + 1] == '/')
        {
            reader->position += 2;
            while (reader->position < reader->length && text[reader->position] != '\n')
            {
                reader->position++;
            }
            continue;
        }
        if (text[reader->position + 1] == '*')
        {
            const size_t start = reader->position;
            reader->position += 2;
            for (;;)
            {
                if (reader->position + 1 >= reader->length)
                {
                    (void)tb_json_fail(reader, TB_JSON_MALFORMED, start, "unterminated comment");
                    return false;
                }
                if (text[reader->position] == '*' && text[reader->position + 1] == '/')
                {
                    reader->position += 2;
                    break;
                }
                reader->position++;
            }
            continue;
        }
        return true;
    }
}

/* -------------------------------------------------------------------------
 * Strings and numbers.
 * ------------------------------------------------------------------------- */

static int tb_json_hex_value(char character)
{
    if (character >= '0' && character <= '9')
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

/* Four hexadecimal digits at `at`; -1 when they are not. */
static long tb_json_hex4(const char *text, size_t length, size_t at)
{
    long value = 0;
    if (at + 4 > length)
    {
        return -1;
    }
    for (size_t index = 0; index < 4; index++)
    {
        const int digit = tb_json_hex_value(text[at + index]);
        if (digit < 0)
        {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

/* Check the string that opens at `reader->position` and move past it. */
static tb_json_result tb_json_scan_string(tb_json_reader *reader, tb_json_token *token)
{
    const char *const text = reader->text;
    const size_t start = reader->position + 1;
    size_t at = start;
    bool escaped = false;
    for (;;)
    {
        unsigned char byte = 0;
        if (at >= reader->length)
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "unterminated string");
        }
        byte = (unsigned char)text[at];
        if (byte == '"')
        {
            break;
        }
        if (byte < 0x20)
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, at, "control character in a string");
        }
        if (byte == '\\')
        {
            long unit = 0;
            escaped = true;
            if (at + 1 >= reader->length)
            {
                return tb_json_fail(reader, TB_JSON_MALFORMED, at, "unterminated string");
            }
            switch (text[at + 1])
            {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                at += 2;
                continue;
            case 'u':
                break;
            default:
                return tb_json_fail(reader, TB_JSON_MALFORMED, at, "unknown escape");
            }
            unit = tb_json_hex4(text, reader->length, at + 2);
            if (unit < 0)
            {
                return tb_json_fail(reader, TB_JSON_MALFORMED, at, "bad \\u escape");
            }
            if (unit >= 0xDC00 && unit <= 0xDFFF)
            {
                return tb_json_fail(reader, TB_JSON_MALFORMED, at, "unpaired surrogate");
            }
            if (unit >= 0xD800 && unit <= 0xDBFF)
            {
                long low = -1;
                if (at + 7 < reader->length && text[at + 6] == '\\' && text[at + 7] == 'u')
                {
                    low = tb_json_hex4(text, reader->length, at + 8);
                }
                if (low < 0xDC00 || low > 0xDFFF)
                {
                    return tb_json_fail(reader, TB_JSON_MALFORMED, at, "unpaired surrogate");
                }
                at += 12;
                continue;
            }
            at += 6;
            continue;
        }
        if (byte < 0x80)
        {
            at++;
            continue;
        }
        {
            uint32_t scalar = 0;
            size_t next = 0;
            if (!tb_utf8_decode(text, reader->length, at, &scalar, &next))
            {
                return tb_json_fail(reader, TB_JSON_MALFORMED, at, "invalid UTF-8");
            }
            at = next;
        }
    }
    token->text = text + start;
    token->length = at - start;
    token->escaped = escaped;
    reader->position = at + 1;
    return TB_JSON_OK;
}

static bool tb_json_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

/* Check the number at `reader->position` against RFC 8259's grammar. */
static tb_json_result tb_json_scan_number(tb_json_reader *reader, tb_json_token *token)
{
    const char *const text = reader->text;
    const size_t start = reader->position;
    size_t at = start;
    if (at < reader->length && text[at] == '-')
    {
        at++;
    }
    if (at >= reader->length || !tb_json_is_digit(text[at]))
    {
        return tb_json_fail(reader, TB_JSON_MALFORMED, at, "digit expected");
    }
    if (text[at] == '0')
    {
        at++;
        if (at < reader->length && tb_json_is_digit(text[at]))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, start, "leading zero");
        }
    }
    else
    {
        while (at < reader->length && tb_json_is_digit(text[at]))
        {
            at++;
        }
    }
    if (at < reader->length && text[at] == '.')
    {
        at++;
        if (at >= reader->length || !tb_json_is_digit(text[at]))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, at, "digit expected after '.'");
        }
        while (at < reader->length && tb_json_is_digit(text[at]))
        {
            at++;
        }
    }
    if (at < reader->length && (text[at] == 'e' || text[at] == 'E'))
    {
        at++;
        if (at < reader->length && (text[at] == '+' || text[at] == '-'))
        {
            at++;
        }
        if (at >= reader->length || !tb_json_is_digit(text[at]))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, at, "digit expected in exponent");
        }
        while (at < reader->length && tb_json_is_digit(text[at]))
        {
            at++;
        }
    }
    token->text = text + start;
    token->length = at - start;
    token->escaped = false;
    reader->position = at;
    return TB_JSON_OK;
}

/* -------------------------------------------------------------------------
 * Values and structure.
 * ------------------------------------------------------------------------- */

/* After a value: the end of the document, or the separator of its container. */
static void tb_json_after_value(tb_json_reader *reader)
{
    reader->expect = reader->depth == 0 ? TB_JSON_EXPECT_END : TB_JSON_EXPECT_SEPARATOR;
}

static bool tb_json_matches(const tb_json_reader *reader, const char *word, size_t length)
{
    return reader->length - reader->position >= length &&
           memcmp(reader->text + reader->position, word, length) == 0;
}

static tb_json_result tb_json_push(tb_json_reader *reader, unsigned char kind)
{
    if (reader->depth == reader->max_depth)
    {
        return tb_json_fail(reader, TB_JSON_TOO_DEEP, reader->position, "nested too deeply");
    }
    reader->stack[reader->depth] = kind;
    reader->depth++;
    reader->position++;
    return TB_JSON_OK;
}

static tb_json_result tb_json_read_value(tb_json_reader *reader, tb_json_token *token)
{
    const char character = reader->text[reader->position];
    tb_json_result result = TB_JSON_OK;
    token->text = NULL;
    token->length = 0;
    token->escaped = false;
    switch (character)
    {
    case '{':
        result = tb_json_push(reader, TB_JSON_IN_OBJECT);
        token->event = TB_JSON_EVENT_BEGIN_OBJECT;
        reader->expect = TB_JSON_EXPECT_FIRST_KEY;
        return result;
    case '[':
        result = tb_json_push(reader, TB_JSON_IN_ARRAY);
        token->event = TB_JSON_EVENT_BEGIN_ARRAY;
        reader->expect = TB_JSON_EXPECT_FIRST_VALUE;
        return result;
    case '"':
        token->event = TB_JSON_EVENT_STRING;
        result = tb_json_scan_string(reader, token);
        break;
    case 'n':
        if (!tb_json_matches(reader, "null", 4))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "unknown word");
        }
        token->event = TB_JSON_EVENT_NULL;
        reader->position += 4;
        break;
    case 't':
        if (!tb_json_matches(reader, "true", 4))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "unknown word");
        }
        token->event = TB_JSON_EVENT_TRUE;
        reader->position += 4;
        break;
    case 'f':
        if (!tb_json_matches(reader, "false", 5))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "unknown word");
        }
        token->event = TB_JSON_EVENT_FALSE;
        reader->position += 5;
        break;
    default:
        if (character != '-' && !tb_json_is_digit(character))
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "value expected");
        }
        token->event = TB_JSON_EVENT_NUMBER;
        result = tb_json_scan_number(reader, token);
        break;
    }
    if (result == TB_JSON_OK)
    {
        tb_json_after_value(reader);
    }
    return result;
}

/* `]` or `}`, which must close the innermost container. */
static tb_json_result tb_json_close(tb_json_reader *reader, tb_json_token *token)
{
    const char character = reader->text[reader->position];
    const unsigned char kind = reader->stack[reader->depth - 1];
    if ((character == ']' && kind != TB_JSON_IN_ARRAY) ||
        (character == '}' && kind != TB_JSON_IN_OBJECT))
    {
        return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "mismatched bracket");
    }
    if (reader->after_comma && (reader->flags & TB_JSON_ALLOW_TRAILING_COMMAS) == 0)
    {
        return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "trailing comma");
    }
    token->event = character == ']' ? TB_JSON_EVENT_END_ARRAY : TB_JSON_EVENT_END_OBJECT;
    token->text = NULL;
    token->length = 0;
    token->escaped = false;
    reader->depth--;
    reader->position++;
    reader->after_comma = false;
    tb_json_after_value(reader);
    return TB_JSON_OK;
}

static tb_json_result tb_json_read_key(tb_json_reader *reader, tb_json_token *token)
{
    tb_json_result result = TB_JSON_OK;
    if (reader->text[reader->position] != '"')
    {
        return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "key expected");
    }
    token->event = TB_JSON_EVENT_KEY;
    result = tb_json_scan_string(reader, token);
    if (result == TB_JSON_OK)
    {
        reader->expect = TB_JSON_EXPECT_COLON;
        reader->after_comma = false;
    }
    return result;
}

/* One step: returns OK with an event in `token`, or a failure; or, for `,`
 * and `:`, OK with `*produced` false and nothing reported. */
static tb_json_result tb_json_step(tb_json_reader *reader, tb_json_token *token, bool *produced)
{
    char character = 0;
    *produced = true;
    if (!tb_json_skip_space(reader))
    {
        return reader->error.result;
    }
    token->offset = reader->position;
    if (reader->position >= reader->length)
    {
        if (reader->expect == TB_JSON_EXPECT_END)
        {
            reader->expect = TB_JSON_EXPECT_NOTHING;
            token->event = TB_JSON_EVENT_END;
            token->text = NULL;
            token->length = 0;
            token->escaped = false;
            return TB_JSON_OK;
        }
        return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "unexpected end");
    }
    character = reader->text[reader->position];
    switch (reader->expect)
    {
    case TB_JSON_EXPECT_DOCUMENT:
    case TB_JSON_EXPECT_MEMBER_VALUE:
    case TB_JSON_EXPECT_VALUE:
        if ((character == ']' || character == '}') && reader->expect == TB_JSON_EXPECT_VALUE)
        {
            return tb_json_close(reader, token);
        }
        reader->after_comma = false;
        return tb_json_read_value(reader, token);
    case TB_JSON_EXPECT_FIRST_VALUE:
        if (character == ']')
        {
            return tb_json_close(reader, token);
        }
        return tb_json_read_value(reader, token);
    case TB_JSON_EXPECT_FIRST_KEY:
        if (character == '}')
        {
            return tb_json_close(reader, token);
        }
        return tb_json_read_key(reader, token);
    case TB_JSON_EXPECT_KEY:
        if (character == '}')
        {
            return tb_json_close(reader, token);
        }
        return tb_json_read_key(reader, token);
    case TB_JSON_EXPECT_COLON:
        if (character != ':')
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "':' expected");
        }
        reader->position++;
        reader->expect = TB_JSON_EXPECT_MEMBER_VALUE;
        *produced = false;
        return TB_JSON_OK;
    case TB_JSON_EXPECT_SEPARATOR:
        if (character == ']' || character == '}')
        {
            return tb_json_close(reader, token);
        }
        if (character != ',')
        {
            return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position,
                                "',' or end of container expected");
        }
        reader->position++;
        reader->after_comma = true;
        reader->expect = reader->stack[reader->depth - 1] == TB_JSON_IN_ARRAY ? TB_JSON_EXPECT_VALUE
                                                                              : TB_JSON_EXPECT_KEY;
        *produced = false;
        return TB_JSON_OK;
    case TB_JSON_EXPECT_END:
        return tb_json_fail(reader, TB_JSON_MALFORMED, reader->position, "text after the document");
    case TB_JSON_EXPECT_NOTHING:
    default:
        token->event = TB_JSON_EVENT_END;
        return TB_JSON_OK;
    }
}

/* -------------------------------------------------------------------------
 * The interface.
 * ------------------------------------------------------------------------- */

tb_json_reader *tb_json_reader_create(const char *text, size_t length,
                                      const tb_json_options *options, const tb_allocator *allocator)
{
    tb_allocator resolved = {0};
    tb_json_reader *reader = NULL;
    TB_ASSERT(text != NULL || length == 0);
    resolved = tb_allocator_resolve(allocator);
    reader = TB_ALLOCATOR_NEW(&resolved, tb_json_reader);
    reader->allocator = resolved;
    reader->text = text;
    reader->length = length;
    reader->max_depth = TB_JSON_DEFAULT_MAX_DEPTH;
    reader->max_length = TB_JSON_DEFAULT_MAX_LENGTH;
    reader->expect = TB_JSON_EXPECT_DOCUMENT;
    reader->error.reason = "";
    if (options != NULL)
    {
        reader->flags = options->flags;
        if (options->max_depth != 0)
        {
            reader->max_depth = options->max_depth;
        }
        if (options->max_length != 0)
        {
            reader->max_length = options->max_length;
        }
    }
    if ((reader->flags & ~TB_JSON_KNOWN_FLAGS) != 0)
    {
        (void)tb_json_fail(reader, TB_JSON_INVALID, 0, "unknown flags");
        return reader;
    }
    if (length > reader->max_length)
    {
        (void)tb_json_fail(reader, TB_JSON_TOO_LARGE, 0, "text longer than the limit");
        return reader;
    }
    /* The stack need never be deeper than the text has brackets. */
    if (reader->max_depth > length)
    {
        reader->max_depth = length;
    }
    reader->stack = tb_allocator_alloc(&resolved, reader->max_depth + 1, 1);
    return reader;
}

void tb_json_reader_destroy(tb_json_reader *reader)
{
    tb_allocator allocator = {0};
    if (reader == NULL)
    {
        return;
    }
    allocator = reader->allocator;
    if (reader->stack != NULL)
    {
        tb_allocator_free(&allocator, reader->stack, reader->max_depth + 1, 1);
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_json_reader, reader);
}

tb_json_result tb_json_reader_next(tb_json_reader *reader, tb_json_token *token)
{
    tb_json_token found = {0};
    TB_ASSERT(reader != NULL);
    TB_ASSERT(token != NULL);
    if (reader->error.result != TB_JSON_OK)
    {
        return reader->error.result;
    }
    for (;;)
    {
        bool produced = false;
        const tb_json_result result = tb_json_step(reader, &found, &produced);
        if (result != TB_JSON_OK)
        {
            return result;
        }
        if (produced)
        {
            *token = found;
            return TB_JSON_OK;
        }
    }
}

void tb_json_reader_error(const tb_json_reader *reader, tb_json_error *error)
{
    TB_ASSERT(reader != NULL);
    TB_ASSERT(error != NULL);
    *error = reader->error;
}

/* Append a scalar as UTF-8. */
static size_t tb_json_put_scalar(uint32_t scalar, char *output)
{
    const size_t written = tb_utf8_encode(scalar, output, TB_UTF8_MAX_BYTES);
    TB_ASSERT(written > 0);
    return written;
}

size_t tb_json_unescape(const char *text, size_t length, char *output)
{
    size_t in = 0;
    size_t out = 0;
    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(output != NULL || length == 0);
    while (in < length)
    {
        long unit = 0;
        if (text[in] != '\\')
        {
            output[out] = text[in];
            out++;
            in++;
            continue;
        }
        TB_ASSERT_MSG(in + 1 < length, "the text is not a string from the reader");
        switch (text[in + 1])
        {
        case 'b':
            output[out] = '\b';
            break;
        case 'f':
            output[out] = '\f';
            break;
        case 'n':
            output[out] = '\n';
            break;
        case 'r':
            output[out] = '\r';
            break;
        case 't':
            output[out] = '\t';
            break;
        case 'u':
            unit = tb_json_hex4(text, length, in + 2);
            TB_ASSERT_MSG(unit >= 0, "the text is not a string from the reader");
            if (unit >= 0xD800 && unit <= 0xDBFF)
            {
                const long low = tb_json_hex4(text, length, in + 8);
                const uint32_t scalar =
                    0x10000u + (((uint32_t)unit - 0xD800u) << 10) + ((uint32_t)low - 0xDC00u);
                TB_ASSERT_MSG(low >= 0xDC00 && low <= 0xDFFF,
                              "the text is not a string from the reader");
                out += tb_json_put_scalar(scalar, output + out);
                in += 12;
            }
            else
            {
                out += tb_json_put_scalar((uint32_t)unit, output + out);
                in += 6;
            }
            continue;
        default:
            /* `"`, `\`, and `/` stand for themselves. */
            output[out] = text[in + 1];
            break;
        }
        out++;
        in += 2;
    }
    return out;
}

/* The digits of an integer number, without its sign; false for a fraction or exponent. */
static bool tb_json_integer_digits(const char *text, size_t length, bool *negative,
                                   uint64_t *magnitude, bool *overflow)
{
    size_t at = 0;
    uint64_t value = 0;
    *negative = false;
    *overflow = false;
    if (at < length && text[at] == '-')
    {
        *negative = true;
        at++;
    }
    if (at == length)
    {
        return false;
    }
    for (; at < length; at++)
    {
        const char character = text[at];
        unsigned digit = 0;
        if (!tb_json_is_digit(character))
        {
            return false;
        }
        digit = (unsigned)(character - '0');
        if (value > (UINT64_MAX - digit) / 10)
        {
            *overflow = true;
        }
        else
        {
            value = value * 10 + digit;
        }
    }
    *magnitude = value;
    return true;
}

bool tb_json_number_int64(const char *text, size_t length, int64_t *value)
{
    bool negative = false;
    bool overflow = false;
    uint64_t magnitude = 0;
    TB_ASSERT(text != NULL && value != NULL);
    if (!tb_json_integer_digits(text, length, &negative, &magnitude, &overflow) || overflow)
    {
        return false;
    }
    if (negative)
    {
        if (magnitude > (uint64_t)INT64_MAX + 1)
        {
            return false;
        }
        /* -(magnitude - 1) - 1 reaches INT64_MIN without overflowing. */
        *value = magnitude == 0 ? 0 : -(int64_t)(magnitude - 1) - 1;
        return true;
    }
    if (magnitude > (uint64_t)INT64_MAX)
    {
        return false;
    }
    *value = (int64_t)magnitude;
    return true;
}

bool tb_json_number_uint64(const char *text, size_t length, uint64_t *value)
{
    bool negative = false;
    bool overflow = false;
    uint64_t magnitude = 0;
    TB_ASSERT(text != NULL && value != NULL);
    if (!tb_json_integer_digits(text, length, &negative, &magnitude, &overflow) || overflow)
    {
        return false;
    }
    /* "-0" is zero, and zero is unsigned. */
    if (negative && magnitude != 0)
    {
        return false;
    }
    *value = magnitude;
    return true;
}

bool tb_json_number_double(const char *text, size_t length, double *value)
{
    double parsed = 0.0;
    TB_ASSERT(text != NULL && value != NULL);
    if (tb_decimal_parse(text, length, &parsed) != TB_DECIMAL_OK)
    {
        return false;
    }
    /* Infinity is not a number JSON can mean. */
    if (parsed - parsed != 0.0)
    {
        return false;
    }
    *value = parsed;
    return true;
}

const char *tb_json_result_name(tb_json_result result)
{
    switch (result)
    {
    case TB_JSON_OK:
        return "ok";
    case TB_JSON_INVALID:
        return "invalid argument";
    case TB_JSON_NO_MEMORY:
        return "out of memory";
    case TB_JSON_TOO_LARGE:
        return "too large";
    case TB_JSON_MALFORMED:
        return "malformed data";
    case TB_JSON_TOO_DEEP:
        return "too deep";
    case TB_JSON_DUPLICATE_KEY:
        return "duplicate key";
    case TB_JSON_OUTPUT_FAILED:
        return "output failed";
    default:
        return "unknown result";
    }
}
