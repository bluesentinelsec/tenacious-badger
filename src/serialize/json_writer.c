/* The JSON writer: values to a stream, through a buffer, with the structure
 * checked as it goes. Writing a document walks it with a stack of its own. */
#include "badger/json.h"

#include "badger/array.h"
#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/utf8.h"

#include <string.h>

#define TB_JSON_WRITER_BUFFER ((size_t)4096)
#define TB_JSON_INDENT_WIDTH ((size_t)2)
/* Digits of the largest 64-bit integer, a sign, and room to spare. */
#define TB_JSON_INTEGER_TEXT ((size_t)24)

/* An open container: its bracket, and how many values or members it has. */
typedef struct tb_json_level
{
    char kind;
    size_t count;
} tb_json_level;

struct tb_json_writer
{
    tb_allocator allocator;
    tb_stream sink;
    tb_json_style style;
    tb_json_result failure;
    tb_array *levels; /* tb_json_level */
    bool key_written; /* A key waits for its value. */
    bool value_done;  /* The document's one value is complete. */
    bool finished;
    size_t used;
    unsigned char buffer[TB_JSON_WRITER_BUFFER];
};

/* -------------------------------------------------------------------------
 * Output.
 * ------------------------------------------------------------------------- */

static void tb_json_writer_flush(tb_json_writer *writer)
{
    if (writer->failure == TB_JSON_OK && writer->used > 0 &&
        tb_stream_write_all(&writer->sink, writer->buffer, writer->used) != TB_STREAM_OK)
    {
        writer->failure = TB_JSON_OUTPUT_FAILED;
    }
    writer->used = 0;
}

static void tb_json_put(tb_json_writer *writer, const char *bytes, size_t length)
{
    while (length > 0 && writer->failure == TB_JSON_OK)
    {
        size_t piece = TB_JSON_WRITER_BUFFER - writer->used;
        if (piece > length)
        {
            piece = length;
        }
        memcpy(writer->buffer + writer->used, bytes, piece);
        writer->used += piece;
        bytes += piece;
        length -= piece;
        if (writer->used == TB_JSON_WRITER_BUFFER)
        {
            tb_json_writer_flush(writer);
        }
    }
}

static void tb_json_put_char(tb_json_writer *writer, char character)
{
    tb_json_put(writer, &character, 1);
}

/* In the indented style, a line break and the indentation of `depth`. */
static void tb_json_new_line(tb_json_writer *writer, size_t depth)
{
    static const char spaces[] = "                                ";
    size_t indent = depth * TB_JSON_INDENT_WIDTH;
    if (writer->style != TB_JSON_INDENTED)
    {
        return;
    }
    tb_json_put_char(writer, '\n');
    while (indent > 0)
    {
        const size_t piece = indent < sizeof(spaces) - 1 ? indent : sizeof(spaces) - 1;
        tb_json_put(writer, spaces, piece);
        indent -= piece;
    }
}

/* -------------------------------------------------------------------------
 * Structure.
 * ------------------------------------------------------------------------- */

static tb_json_level *tb_json_top(tb_json_writer *writer)
{
    const size_t depth = tb_array_length(writer->levels);
    return depth == 0 ? NULL : tb_array_at(writer->levels, depth - 1);
}

/* What comes before a value: in an array a comma and a line; in an object,
 * nothing, since its key came first. */
static void tb_json_before_value(tb_json_writer *writer)
{
    tb_json_level *const level = tb_json_top(writer);
    TB_ASSERT_MSG(!writer->finished, "the document is finished");
    if (level == NULL)
    {
        TB_ASSERT_MSG(!writer->value_done, "a document has one value at its top");
        return;
    }
    if (level->kind == '{')
    {
        TB_ASSERT_MSG(writer->key_written, "a value in an object needs a key first");
        writer->key_written = false;
        return;
    }
    if (level->count > 0)
    {
        tb_json_put_char(writer, ',');
    }
    level->count++;
    tb_json_new_line(writer, tb_array_length(writer->levels));
}

static void tb_json_after_value(tb_json_writer *writer)
{
    if (tb_array_length(writer->levels) == 0)
    {
        writer->value_done = true;
    }
}

static tb_json_result tb_json_begin(tb_json_writer *writer, char kind)
{
    tb_json_level level = {0};
    tb_json_before_value(writer);
    tb_json_put_char(writer, kind);
    level.kind = kind;
    (void)tb_array_push(writer->levels, &level);
    return writer->failure;
}

static tb_json_result tb_json_end(tb_json_writer *writer, char kind)
{
    tb_json_level level = {0};
    tb_json_level *const top = tb_json_top(writer);
    TB_ASSERT_MSG(top != NULL && top->kind == kind, "an end must match its beginning");
    TB_ASSERT_MSG(!writer->key_written, "a key needs a value before the object ends");
    tb_array_pop(writer->levels, &level);
    if (level.count > 0)
    {
        tb_json_new_line(writer, tb_array_length(writer->levels));
    }
    tb_json_put_char(writer, kind == '[' ? ']' : '}');
    tb_json_after_value(writer);
    return writer->failure;
}

/* -------------------------------------------------------------------------
 * Values.
 * ------------------------------------------------------------------------- */

/* Write a string with the escapes JSON requires, after checking it is UTF-8. */
static tb_json_result tb_json_put_string(tb_json_writer *writer, const char *text, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t error = 0;
    size_t run_start = 0;
    if (!tb_utf8_validate(text, length, &error))
    {
        return TB_JSON_INVALID;
    }
    tb_json_put_char(writer, '"');
    for (size_t index = 0; index < length; index++)
    {
        const unsigned char byte = (unsigned char)text[index];
        char escape[6] = {'\\', 0, 0, 0, 0, 0};
        size_t escape_length = 2;
        switch (byte)
        {
        case '"':
            escape[1] = '"';
            break;
        case '\\':
            escape[1] = '\\';
            break;
        case '\b':
            escape[1] = 'b';
            break;
        case '\f':
            escape[1] = 'f';
            break;
        case '\n':
            escape[1] = 'n';
            break;
        case '\r':
            escape[1] = 'r';
            break;
        case '\t':
            escape[1] = 't';
            break;
        default:
            if (byte >= 0x20)
            {
                continue;
            }
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = hex[byte >> 4];
            escape[5] = hex[byte & 0x0Fu];
            escape_length = 6;
            break;
        }
        tb_json_put(writer, text + run_start, index - run_start);
        tb_json_put(writer, escape, escape_length);
        run_start = index + 1;
    }
    tb_json_put(writer, text + run_start, length - run_start);
    tb_json_put_char(writer, '"');
    return TB_JSON_OK;
}

static bool tb_json_digit(char character)
{
    return character >= '0' && character <= '9';
}

/* Whether text is exactly one number of RFC 8259's grammar. */
static bool tb_json_is_number(const char *text, size_t length)
{
    size_t at = 0;
    if (at < length && text[at] == '-')
    {
        at++;
    }
    if (at >= length || !tb_json_digit(text[at]))
    {
        return false;
    }
    if (text[at] == '0')
    {
        /* A zero stands alone: no digit may follow it. */
        at++;
        if (at < length && tb_json_digit(text[at]))
        {
            return false;
        }
    }
    else
    {
        while (at < length && tb_json_digit(text[at]))
        {
            at++;
        }
    }
    if (at < length && text[at] == '.')
    {
        at++;
        if (at >= length || !tb_json_digit(text[at]))
        {
            return false;
        }
        while (at < length && tb_json_digit(text[at]))
        {
            at++;
        }
    }
    if (at < length && (text[at] == 'e' || text[at] == 'E'))
    {
        at++;
        if (at < length && (text[at] == '+' || text[at] == '-'))
        {
            at++;
        }
        if (at >= length || !tb_json_digit(text[at]))
        {
            return false;
        }
        while (at < length && tb_json_digit(text[at]))
        {
            at++;
        }
    }
    return at == length;
}

/* The digits of an unsigned integer, written backwards from the end of `text`. */
static size_t tb_json_unsigned_text(uint64_t value, char text[TB_JSON_INTEGER_TEXT], size_t end)
{
    size_t start = end;
    do
    {
        start--;
        text[start] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    return start;
}

/* -------------------------------------------------------------------------
 * The interface.
 * ------------------------------------------------------------------------- */

tb_json_writer *tb_json_writer_create(tb_stream *sink, tb_json_style style,
                                      const tb_allocator *allocator)
{
    tb_allocator resolved = {0};
    tb_json_writer *writer = NULL;
    TB_ASSERT(sink != NULL && sink->functions != NULL);
    TB_ASSERT_MSG(sink->functions->write_bytes != NULL, "the sink cannot be written");
    TB_ASSERT_MSG(style == TB_JSON_COMPACT || style == TB_JSON_INDENTED, "unknown style");
    resolved = tb_allocator_resolve(allocator);
    writer = TB_ALLOCATOR_NEW(&resolved, tb_json_writer);
    writer->allocator = resolved;
    writer->sink = *sink;
    writer->style = style;
    writer->levels = tb_array_create(&resolved, sizeof(tb_json_level), _Alignof(tb_json_level));
    return writer;
}

void tb_json_writer_destroy(tb_json_writer *writer)
{
    tb_allocator allocator = {0};
    if (writer == NULL)
    {
        return;
    }
    allocator = writer->allocator;
    tb_array_destroy(writer->levels);
    TB_ALLOCATOR_DELETE(&allocator, tb_json_writer, writer);
}

static tb_json_result tb_json_write_word(tb_json_writer *writer, const char *word, size_t length)
{
    TB_ASSERT(writer != NULL);
    tb_json_before_value(writer);
    tb_json_put(writer, word, length);
    tb_json_after_value(writer);
    return writer->failure;
}

tb_json_result tb_json_write_null(tb_json_writer *writer)
{
    return tb_json_write_word(writer, "null", 4);
}

tb_json_result tb_json_write_bool(tb_json_writer *writer, bool value)
{
    return value ? tb_json_write_word(writer, "true", 4) : tb_json_write_word(writer, "false", 5);
}

tb_json_result tb_json_write_uint64(tb_json_writer *writer, uint64_t value)
{
    char text[TB_JSON_INTEGER_TEXT] = {0};
    const size_t start = tb_json_unsigned_text(value, text, sizeof(text));
    return tb_json_write_word(writer, text + start, sizeof(text) - start);
}

tb_json_result tb_json_write_int64(tb_json_writer *writer, int64_t value)
{
    char text[TB_JSON_INTEGER_TEXT] = {0};
    /* The magnitude of INT64_MIN does not fit an int64_t; it fits a uint64_t. */
    const uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    size_t start = tb_json_unsigned_text(magnitude, text, sizeof(text));
    if (value < 0)
    {
        start--;
        text[start] = '-';
    }
    return tb_json_write_word(writer, text + start, sizeof(text) - start);
}

tb_json_result tb_json_write_double(tb_json_writer *writer, double value)
{
    char text[TB_DECIMAL_SHORTEST_SIZE] = {0};
    size_t required = 0;
    TB_ASSERT(writer != NULL);
    /* Infinity and NaN have no JSON spelling. */
    if (value - value != 0.0)
    {
        return writer->failure != TB_JSON_OK ? writer->failure : TB_JSON_INVALID;
    }
    if (tb_decimal_shortest(value, text, sizeof(text), &required) != TB_DECIMAL_OK)
    {
        return TB_JSON_INVALID;
    }
    TB_ASSERT_MSG(tb_json_is_number(text, required - 1), "the shortest text is not JSON");
    return tb_json_write_word(writer, text, required - 1);
}

tb_json_result tb_json_write_number_text(tb_json_writer *writer, const char *text, size_t length)
{
    TB_ASSERT(writer != NULL);
    TB_ASSERT(text != NULL || length == 0);
    if (!tb_json_is_number(text, length))
    {
        return writer->failure != TB_JSON_OK ? writer->failure : TB_JSON_INVALID;
    }
    return tb_json_write_word(writer, text, length);
}

tb_json_result tb_json_write_string(tb_json_writer *writer, const char *text, size_t length)
{
    tb_json_result result = TB_JSON_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(text != NULL || length == 0);
    if (!tb_utf8_validate(text, length, &(size_t){0}))
    {
        return writer->failure != TB_JSON_OK ? writer->failure : TB_JSON_INVALID;
    }
    tb_json_before_value(writer);
    result = tb_json_put_string(writer, text, length);
    TB_ASSERT(result == TB_JSON_OK);
    tb_json_after_value(writer);
    return writer->failure;
}

tb_json_result tb_json_write_key(tb_json_writer *writer, const char *text, size_t length)
{
    tb_json_level *level = NULL;
    tb_json_result result = TB_JSON_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(text != NULL || length == 0);
    level = tb_json_top(writer);
    TB_ASSERT_MSG(level != NULL && level->kind == '{', "a key belongs in an object");
    TB_ASSERT_MSG(!writer->key_written, "a key needs a value before the next key");
    if (!tb_utf8_validate(text, length, &(size_t){0}))
    {
        return writer->failure != TB_JSON_OK ? writer->failure : TB_JSON_INVALID;
    }
    if (level->count > 0)
    {
        tb_json_put_char(writer, ',');
    }
    level->count++;
    tb_json_new_line(writer, tb_array_length(writer->levels));
    result = tb_json_put_string(writer, text, length);
    TB_ASSERT(result == TB_JSON_OK);
    if (writer->style == TB_JSON_INDENTED)
    {
        tb_json_put(writer, ": ", 2);
    }
    else
    {
        tb_json_put_char(writer, ':');
    }
    writer->key_written = true;
    return writer->failure;
}

tb_json_result tb_json_write_begin_array(tb_json_writer *writer)
{
    TB_ASSERT(writer != NULL);
    return tb_json_begin(writer, '[');
}

tb_json_result tb_json_write_end_array(tb_json_writer *writer)
{
    TB_ASSERT(writer != NULL);
    return tb_json_end(writer, '[');
}

tb_json_result tb_json_write_begin_object(tb_json_writer *writer)
{
    TB_ASSERT(writer != NULL);
    return tb_json_begin(writer, '{');
}

tb_json_result tb_json_write_end_object(tb_json_writer *writer)
{
    TB_ASSERT(writer != NULL);
    return tb_json_end(writer, '{');
}

/* A value of a document that is not a container. */
static tb_json_result tb_json_write_scalar(tb_json_writer *writer, const tb_json_value *value)
{
    const char *text = NULL;
    size_t length = 0;
    bool flag = false;
    switch (tb_json_type_of(value))
    {
    case TB_JSON_NULL:
        return tb_json_write_null(writer);
    case TB_JSON_BOOL:
        (void)tb_json_as_bool(value, &flag);
        return tb_json_write_bool(writer, flag);
    case TB_JSON_NUMBER:
        text = tb_json_as_text(value, &length);
        return tb_json_write_number_text(writer, text, length);
    case TB_JSON_STRING:
        text = tb_json_as_text(value, &length);
        return tb_json_write_string(writer, text, length);
    case TB_JSON_ARRAY:
    case TB_JSON_OBJECT:
    default:
        return TB_JSON_INVALID;
    }
}

/* A container being written, and the position of its next value. */
typedef struct tb_json_walk
{
    const tb_json_value *value;
    size_t next;
} tb_json_walk;

static bool tb_json_is_container(const tb_json_value *value)
{
    const tb_json_type type = tb_json_type_of(value);
    return type == TB_JSON_ARRAY || type == TB_JSON_OBJECT;
}

static tb_json_result tb_json_open(tb_json_writer *writer, tb_array *walks,
                                   const tb_json_value *value)
{
    tb_json_walk walk = {0};
    if (!tb_json_is_container(value))
    {
        return tb_json_write_scalar(writer, value);
    }
    walk.value = value;
    (void)tb_array_push(walks, &walk);
    return tb_json_type_of(value) == TB_JSON_ARRAY ? tb_json_write_begin_array(writer)
                                                   : tb_json_write_begin_object(writer);
}

tb_json_result tb_json_write_value(tb_json_writer *writer, const tb_json_value *value)
{
    tb_array *walks = NULL;
    tb_json_result result = TB_JSON_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(value != NULL);
    walks = tb_array_create(&writer->allocator, sizeof(tb_json_walk), _Alignof(tb_json_walk));
    result = tb_json_open(writer, walks, value);
    while (result == TB_JSON_OK && tb_array_length(walks) > 0)
    {
        tb_json_walk *const top = tb_array_at(walks, tb_array_length(walks) - 1);
        const tb_json_value *const container = top->value;
        const bool is_array = tb_json_type_of(container) == TB_JSON_ARRAY;
        const tb_json_value *child = NULL;
        if (top->next == tb_json_count(container))
        {
            tb_array_pop(walks, NULL);
            result = is_array ? tb_json_write_end_array(writer) : tb_json_write_end_object(writer);
            continue;
        }
        if (is_array)
        {
            child = tb_json_at(container, top->next);
        }
        else
        {
            const char *key = NULL;
            size_t key_length = 0;
            child = tb_json_member_at(container, top->next, &key, &key_length);
            result = tb_json_write_key(writer, key, key_length);
        }
        top->next++;
        if (result == TB_JSON_OK)
        {
            result = tb_json_open(writer, walks, child);
        }
    }
    tb_array_destroy(walks);
    return result;
}

tb_json_result tb_json_writer_finish(tb_json_writer *writer)
{
    TB_ASSERT(writer != NULL);
    TB_ASSERT_MSG(!writer->finished, "the document is finished");
    TB_ASSERT_MSG(writer->value_done, "the document is not complete");
    if (writer->style == TB_JSON_INDENTED)
    {
        tb_json_put_char(writer, '\n');
    }
    tb_json_writer_flush(writer);
    writer->finished = true;
    return writer->failure;
}
