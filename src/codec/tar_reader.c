/* Reading TAR archives in order from a stream. */
#include "badger/tar.h"

#include "badger/assert.h"
#include "badger/str.h"
#include "tar_internal.h"

#include <stdbool.h>
#include <string.h>

/* Bytes read and thrown away at a time when data is skipped. */
#define TB_TAR_SKIP_BUFFER ((size_t)4096)

/* The highest magnitude of a negative int64_t: 2^63. */
#define TB_TAR_INT64_NEGATIVE_LIMIT ((uint64_t)INT64_MAX + 1u)

/* What kind of header a block is, by its magic. */
typedef enum tb_tar_header_format
{
    TB_TAR_FORMAT_OLD,   /* No magic: the original format, without the prefix field. */
    TB_TAR_FORMAT_USTAR, /* POSIX ustar, which pax extends. */
    TB_TAR_FORMAT_GNU    /* GNU's, whose prefix field holds other things. */
} tb_tar_header_format;

typedef enum tb_tar_reader_state
{
    TB_TAR_READER_BEFORE, /* No entry yet. */
    TB_TAR_READER_ENTRY,  /* At an entry, whose data may be read. */
    TB_TAR_READER_DONE    /* Ended or failed; `outcome` says which. */
} tb_tar_reader_state;

/* Bytes the reader owns, with room for a NUL after them. */
typedef struct tb_tar_text
{
    char *bytes;
    size_t length;
    size_t capacity;
} tb_tar_text;

struct tb_tar_reader
{
    tb_stream source;
    tb_allocator allocator;
    unsigned flags;
    size_t max_name;
    size_t max_extended;
    tb_tar_reader_state state;
    tb_tar_result outcome;          /* END or the failure, once DONE. */
    tb_stream_result source_result; /* The source's own result, after INPUT_FAILED. */
    uint64_t position;              /* Bytes taken from the source. */
    uint64_t offset;                /* See tb_tar_reader_offset(). */
    uint64_t remaining;             /* Data of the current entry not yet read. */
    uint64_t padding;               /* Bytes after the data, to the end of its block. */
    uint64_t generation;            /* Entries moved past; data streams check it. */
    /* Values an extended header gave for the next entry. */
    bool has_name;
    bool has_link;
    bool has_size;
    bool has_modified;
    uint64_t size;
    int64_t modified;
    tb_tar_text name;
    tb_tar_text link;
    tb_tar_text extended; /* The data of an extended header, while it is read. */
    unsigned char header[TB_TAR_BLOCK];
};

/* -------------------------------------------------------------------------
 * Bytes from the source.
 * ------------------------------------------------------------------------- */

static tb_tar_result tb_tar_fail(tb_tar_reader *reader, tb_tar_result result)
{
    TB_ASSERT(result != TB_TAR_OK);
    reader->state = TB_TAR_READER_DONE;
    reader->outcome = result;
    return result;
}

/* Read up to `length` bytes, as many reads as it takes; fewer only at the
 * end of the source. */
static tb_tar_result tb_tar_read_fully(tb_tar_reader *reader, void *bytes, size_t length,
                                       size_t *count)
{
    unsigned char *const output = bytes;
    *count = 0;
    while (*count < length)
    {
        size_t transferred = 0;
        const tb_stream_result result =
            tb_stream_read(&reader->source, output + *count, length - *count, &transferred);
        *count += transferred;
        reader->position += transferred;
        if (result != TB_STREAM_OK)
        {
            reader->source_result = result;
            return TB_TAR_INPUT_FAILED;
        }
        if (transferred == 0)
        {
            break;
        }
    }
    return TB_TAR_OK;
}

/* Read the next block into `header`. At the end of the source, `at_end` is
 * set; a block cut short is TRUNCATED. */
static tb_tar_result tb_tar_read_block(tb_tar_reader *reader, bool *at_end)
{
    size_t count = 0;
    const tb_tar_result result = tb_tar_read_fully(reader, reader->header, TB_TAR_BLOCK, &count);
    *at_end = false;
    if (result != TB_TAR_OK)
    {
        return result;
    }
    if (count == 0)
    {
        *at_end = true;
        return TB_TAR_OK;
    }
    return count == TB_TAR_BLOCK ? TB_TAR_OK : TB_TAR_TRUNCATED;
}

static tb_tar_result tb_tar_skip(tb_tar_reader *reader, uint64_t length)
{
    unsigned char discard[TB_TAR_SKIP_BUFFER];
    while (length > 0)
    {
        const size_t want = length < sizeof(discard) ? (size_t)length : sizeof(discard);
        size_t count = 0;
        const tb_tar_result result = tb_tar_read_fully(reader, discard, want, &count);
        if (result != TB_TAR_OK)
        {
            return result;
        }
        if (count < want)
        {
            return TB_TAR_TRUNCATED;
        }
        length -= count;
    }
    return TB_TAR_OK;
}

/* Make room for `length` bytes and a NUL. Lengths are bounded by the
 * reader's limits, far below SIZE_MAX. */
static void tb_tar_text_reserve(tb_tar_reader *reader, tb_tar_text *text, size_t length)
{
    TB_ASSERT(length < SIZE_MAX);
    if (length + 1 > text->capacity)
    {
        text->bytes =
            tb_allocator_realloc(&reader->allocator, text->bytes, text->capacity, length + 1, 1);
        text->capacity = length + 1;
    }
}

static void tb_tar_text_set(tb_tar_reader *reader, tb_tar_text *text, const void *bytes,
                            size_t length)
{
    tb_tar_text_reserve(reader, text, length);
    if (length > 0)
    {
        memcpy(text->bytes, bytes, length);
    }
    text->bytes[length] = '\0';
    text->length = length;
}

static void tb_tar_text_free(tb_tar_reader *reader, tb_tar_text *text)
{
    tb_allocator_free(&reader->allocator, text->bytes, text->capacity, 1);
    *text = (tb_tar_text){0};
}

/* -------------------------------------------------------------------------
 * Fields of a header.
 * ------------------------------------------------------------------------- */

/* The bytes of a text field: up to its first NUL, or all of it. */
static size_t tb_tar_field_length(const unsigned char *field, size_t width)
{
    const unsigned char *const end = memchr(field, '\0', width);
    return end == NULL ? width : (size_t)(end - field);
}

static bool tb_tar_is_zero_block(const unsigned char *block)
{
    for (size_t index = 0; index < TB_TAR_BLOCK; index++)
    {
        if (block[index] != 0)
        {
            return false;
        }
    }
    return true;
}

/* GNU's base-256: the field is a two's complement number, big-endian, whose
 * first bit marks the form. */
static bool tb_tar_base256(const unsigned char *field, size_t width, bool *negative,
                           uint64_t *magnitude)
{
    const bool below_zero = (field[0] & 0x40u) != 0;
    /* A negative number is read as its complement, whose magnitude is one less. */
    const unsigned flip = below_zero ? 0xFFu : 0x00u;
    uint64_t value = (uint64_t)((field[0] ^ flip) & 0x7Fu);
    for (size_t index = 1; index < width; index++)
    {
        if (value > (UINT64_MAX >> 8))
        {
            return false;
        }
        value = (value << 8) | (uint64_t)((field[index] ^ flip) & 0xFFu);
    }
    if (below_zero)
    {
        if (value >= UINT64_MAX)
        {
            return false;
        }
        value++;
    }
    *negative = below_zero;
    *magnitude = value;
    return true;
}

/* A numeric field: octal digits, perhaps after spaces and before spaces or
 * NULs, or a field of only those, which is zero; or GNU's base-256. */
static bool tb_tar_number(const unsigned char *field, size_t width, bool *negative,
                          uint64_t *magnitude)
{
    size_t index = 0;
    uint64_t value = 0;
    if ((field[0] & 0x80u) != 0)
    {
        return tb_tar_base256(field, width, negative, magnitude);
    }
    while (index < width && field[index] == ' ')
    {
        index++;
    }
    while (index < width && field[index] >= '0' && field[index] <= '7')
    {
        if (value > (UINT64_MAX >> 3))
        {
            return false;
        }
        value = (value << 3) | (uint64_t)(field[index] - '0');
        index++;
    }
    while (index < width && (field[index] == ' ' || field[index] == '\0'))
    {
        index++;
    }
    *negative = false;
    *magnitude = value;
    return index == width;
}

static bool tb_tar_unsigned_field(const unsigned char *field, size_t width, uint64_t *value)
{
    bool negative = false;
    return tb_tar_number(field, width, &negative, value) && !negative;
}

static bool tb_tar_to_int64(bool negative, uint64_t magnitude, int64_t *value)
{
    if (negative)
    {
        if (magnitude > TB_TAR_INT64_NEGATIVE_LIMIT)
        {
            return false;
        }
        /* -(magnitude - 1) - 1 stays in range at 2^63. */
        *value = magnitude == 0 ? 0 : -(int64_t)(magnitude - 1u) - 1;
        return true;
    }
    if (magnitude > (uint64_t)INT64_MAX)
    {
        return false;
    }
    *value = (int64_t)magnitude;
    return true;
}

/* Check the header's checksum and tell its format by its magic. */
static tb_tar_result tb_tar_check_header(const unsigned char *header, tb_tar_header_format *format)
{
    uint64_t stored = 0;
    uint32_t unsigned_sum = 0;
    int32_t signed_sum = 0;
    if (!tb_tar_unsigned_field(header + TB_TAR_CHECKSUM, TB_TAR_CHECKSUM_WIDTH, &stored))
    {
        return TB_TAR_MALFORMED;
    }
    tb_tar_header_sums(header, &unsigned_sum, &signed_sum);
    if (stored != unsigned_sum && (signed_sum < 0 || stored != (uint64_t)signed_sum))
    {
        return TB_TAR_MALFORMED;
    }
    if (memcmp(header + TB_TAR_MAGIC, "ustar\0", TB_TAR_MAGIC_WIDTH) == 0)
    {
        *format = TB_TAR_FORMAT_USTAR;
    }
    else if (memcmp(header + TB_TAR_MAGIC, "ustar  \0", TB_TAR_MAGIC_WIDTH + 2) == 0)
    {
        *format = TB_TAR_FORMAT_GNU;
    }
    else
    {
        *format = TB_TAR_FORMAT_OLD;
    }
    return TB_TAR_OK;
}

/* -------------------------------------------------------------------------
 * Extended headers.
 * ------------------------------------------------------------------------- */

/* Read an extended header's data into `extended`, and skip to its block's end. */
static tb_tar_result tb_tar_read_extended(tb_tar_reader *reader, uint64_t size)
{
    size_t count = 0;
    tb_tar_result result = TB_TAR_OK;
    if (size > reader->max_extended)
    {
        return TB_TAR_TOO_LARGE;
    }
    tb_tar_text_reserve(reader, &reader->extended, (size_t)size);
    result = tb_tar_read_fully(reader, reader->extended.bytes, (size_t)size, &count);
    if (result != TB_TAR_OK)
    {
        return result;
    }
    if (count < (size_t)size)
    {
        return TB_TAR_TRUNCATED;
    }
    reader->extended.bytes[count] = '\0';
    reader->extended.length = count;
    return tb_tar_skip(reader, (TB_TAR_BLOCK - size % TB_TAR_BLOCK) % TB_TAR_BLOCK);
}

static bool tb_tar_key_is(const char *key, size_t length, const char *expected)
{
    return length == tb_str_length(expected) && memcmp(key, expected, length) == 0;
}

static bool tb_tar_has_prefix(const char *key, size_t length, const char *prefix)
{
    const size_t prefix_length = tb_str_length(prefix);
    return length >= prefix_length && memcmp(key, prefix, prefix_length) == 0;
}

/* A pax decimal: digits only, which fit 64 bits. */
static bool tb_tar_pax_unsigned(const char *text, size_t length, uint64_t *value)
{
    uint64_t total = 0;
    if (length == 0)
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        const unsigned digit = (unsigned)(unsigned char)text[index] - (unsigned)'0';
        if (digit > 9u || total > (UINT64_MAX - digit) / 10u)
        {
            return false;
        }
        total = total * 10u + digit;
    }
    *value = total;
    return true;
}

/* A pax time: seconds, perhaps negative, perhaps with a fraction, which is
 * rounded down to whole seconds. */
static bool tb_tar_pax_time(const char *text, size_t length, int64_t *value)
{
    const bool negative = length > 0 && text[0] == '-';
    const size_t start = negative ? 1u : 0u;
    size_t point = start;
    bool fraction = false;
    uint64_t seconds = 0;
    while (point < length && text[point] != '.')
    {
        point++;
    }
    for (size_t index = point + 1; index < length; index++)
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return false;
        }
        fraction = fraction || text[index] != '0';
    }
    if (!tb_tar_pax_unsigned(text + start, point - start, &seconds))
    {
        return false;
    }
    /* Rounding down makes a negative time with a fraction one second earlier. */
    if (negative && fraction)
    {
        if (seconds == UINT64_MAX)
        {
            return false;
        }
        seconds++;
    }
    return tb_tar_to_int64(negative, seconds, value);
}

/* Take one pax record's value, unless the header is global, whose records
 * are checked and not kept. */
static tb_tar_result tb_tar_pax_record(tb_tar_reader *reader, const char *key, size_t key_length,
                                       const char *value, size_t value_length, bool global)
{
    const bool is_path = tb_tar_key_is(key, key_length, "path");
    const bool is_link = tb_tar_key_is(key, key_length, "linkpath");
    if (tb_tar_has_prefix(key, key_length, "GNU.sparse."))
    {
        return TB_TAR_UNSUPPORTED;
    }
    if (is_path || is_link)
    {
        if (memchr(value, '\0', value_length) != NULL)
        {
            return TB_TAR_MALFORMED;
        }
        if (value_length > reader->max_name)
        {
            return TB_TAR_TOO_LARGE;
        }
        if (global)
        {
            return TB_TAR_OK;
        }
        /* An empty value takes back what an earlier record gave. */
        if (is_path)
        {
            tb_tar_text_set(reader, &reader->name, value, value_length);
            reader->has_name = value_length > 0;
        }
        else
        {
            tb_tar_text_set(reader, &reader->link, value, value_length);
            reader->has_link = value_length > 0;
        }
        return TB_TAR_OK;
    }
    if (tb_tar_key_is(key, key_length, "size"))
    {
        uint64_t size = 0;
        if (!tb_tar_pax_unsigned(value, value_length, &size))
        {
            return TB_TAR_MALFORMED;
        }
        if (!global)
        {
            reader->size = size;
            reader->has_size = true;
        }
        return TB_TAR_OK;
    }
    if (tb_tar_key_is(key, key_length, "mtime"))
    {
        int64_t modified = 0;
        if (!tb_tar_pax_time(value, value_length, &modified))
        {
            return TB_TAR_MALFORMED;
        }
        if (!global)
        {
            reader->modified = modified;
            reader->has_modified = true;
        }
        return TB_TAR_OK;
    }
    /* Owners, other times, character sets, and vendor records mean nothing here. */
    return TB_TAR_OK;
}

/* The records of a pax header, each "<length> <key>=<value>\n", where the
 * length counts the whole record. */
static tb_tar_result tb_tar_parse_pax(tb_tar_reader *reader, bool global)
{
    const char *const data = reader->extended.bytes;
    const size_t length = reader->extended.length;
    size_t at = 0;
    while (at < length)
    {
        size_t index = at;
        size_t record = 0;
        const char *equals = NULL;
        const char *key = NULL;
        size_t end = 0;
        tb_tar_result result = TB_TAR_OK;
        while (index < length && data[index] >= '0' && data[index] <= '9')
        {
            const size_t digit = (size_t)(data[index] - '0');
            if (record > (SIZE_MAX - digit) / 10u)
            {
                return TB_TAR_MALFORMED;
            }
            record = record * 10u + digit;
            index++;
        }
        if (index == at || index >= length || data[index] != ' ' || record > length - at ||
            record < index - at + 1u)
        {
            return TB_TAR_MALFORMED;
        }
        end = at + record;
        key = data + index + 1;
        if (end == 0 || data[end - 1] != '\n' || key >= data + end - 1)
        {
            return TB_TAR_MALFORMED;
        }
        equals = memchr(key, '=', (size_t)(data + end - 1 - key));
        if (equals == NULL || equals == key)
        {
            return TB_TAR_MALFORMED;
        }
        result = tb_tar_pax_record(reader, key, (size_t)(equals - key), equals + 1,
                                   (size_t)(data + end - 1 - (equals + 1)), global);
        if (result != TB_TAR_OK)
        {
            return result;
        }
        at = end;
    }
    return TB_TAR_OK;
}

/* A GNU long name or link target: the data, up to its first NUL. */
static tb_tar_result tb_tar_gnu_long(tb_tar_reader *reader, tb_tar_text *text, bool *has)
{
    const size_t length =
        tb_tar_field_length((const unsigned char *)reader->extended.bytes, reader->extended.length);
    if (length == 0)
    {
        return TB_TAR_MALFORMED;
    }
    if (length > reader->max_name)
    {
        return TB_TAR_TOO_LARGE;
    }
    tb_tar_text_set(reader, text, reader->extended.bytes, length);
    *has = true;
    return TB_TAR_OK;
}

/* Read an extended header, of a type flag 'x', 'g', 'L', or 'K', and keep
 * what it gives the next entry. */
static tb_tar_result tb_tar_extended_header(tb_tar_reader *reader, char flag, uint64_t size)
{
    const tb_tar_result result = tb_tar_read_extended(reader, size);
    if (result != TB_TAR_OK)
    {
        return result;
    }
    switch (flag)
    {
    case 'x':
        return tb_tar_parse_pax(reader, false);
    case 'g':
        return tb_tar_parse_pax(reader, true);
    case 'L':
        return tb_tar_gnu_long(reader, &reader->name, &reader->has_name);
    default:
        TB_ASSERT(flag == 'K');
        return tb_tar_gnu_long(reader, &reader->link, &reader->has_link);
    }
}

/* -------------------------------------------------------------------------
 * Entries.
 * ------------------------------------------------------------------------- */

static tb_tar_type tb_tar_type_of(char flag)
{
    switch (flag)
    {
    case '0':
    case '\0':
    case '7': /* Contiguous, which is a plain file anywhere that has no such thing. */
        return TB_TAR_FILE;
    case '1':
        return TB_TAR_HARDLINK;
    case '2':
        return TB_TAR_SYMLINK;
    case '5':
        return TB_TAR_DIRECTORY;
    default:
        return TB_TAR_OTHER;
    }
}

/* The entry's name from the header's prefix and name fields. */
static void tb_tar_header_name(tb_tar_reader *reader, tb_tar_header_format format)
{
    const unsigned char *const header = reader->header;
    const size_t name_length = tb_tar_field_length(header + TB_TAR_NAME, TB_TAR_NAME_WIDTH);
    const size_t prefix_length =
        format == TB_TAR_FORMAT_USTAR
            ? tb_tar_field_length(header + TB_TAR_PREFIX, TB_TAR_PREFIX_WIDTH)
            : 0u;
    char joined[TB_TAR_PREFIX_WIDTH + 1u + TB_TAR_NAME_WIDTH] = {0};
    size_t length = 0;
    if (prefix_length > 0)
    {
        memcpy(joined, header + TB_TAR_PREFIX, prefix_length);
        length = prefix_length;
        joined[length] = '/';
        length++;
    }
    memcpy(joined + length, header + TB_TAR_NAME, name_length);
    length += name_length;
    tb_tar_text_set(reader, &reader->name, joined, length);
}

/* Describe the entry whose header is in `header`. */
static tb_tar_result tb_tar_take_entry(tb_tar_reader *reader, tb_tar_header_format format,
                                       uint64_t header_size, tb_tar_entry *entry)
{
    const unsigned char *const header = reader->header;
    const char flag = (char)header[TB_TAR_TYPE];
    tb_tar_type type = tb_tar_type_of(flag);
    uint64_t mode = 0;
    bool negative = false;
    uint64_t magnitude = 0;
    int64_t modified = 0;
    const uint64_t size = reader->has_size ? reader->size : header_size;
    if (!tb_tar_unsigned_field(header + TB_TAR_MODE, TB_TAR_MODE_WIDTH, &mode) ||
        !tb_tar_number(header + TB_TAR_MTIME, TB_TAR_MTIME_WIDTH, &negative, &magnitude) ||
        !tb_tar_to_int64(negative, magnitude, &modified))
    {
        return TB_TAR_MALFORMED;
    }
    if (size > UINT64_MAX - TB_TAR_BLOCK)
    {
        return TB_TAR_MALFORMED;
    }
    if (!reader->has_name)
    {
        tb_tar_header_name(reader, format);
    }
    /* A trailing slash marks a directory in the oldest archives; it is not part of a name. */
    if (reader->name.length > 0 && reader->name.bytes[reader->name.length - 1] == '/' &&
        type == TB_TAR_FILE)
    {
        type = TB_TAR_DIRECTORY;
    }
    while (reader->name.length > 0 && reader->name.bytes[reader->name.length - 1] == '/')
    {
        reader->name.length--;
        reader->name.bytes[reader->name.length] = '\0';
    }
    if (type == TB_TAR_HARDLINK || type == TB_TAR_SYMLINK)
    {
        if (!reader->has_link)
        {
            tb_tar_text_set(reader, &reader->link, header + TB_TAR_LINK,
                            tb_tar_field_length(header + TB_TAR_LINK, TB_TAR_LINK_WIDTH));
        }
    }
    else
    {
        tb_tar_text_set(reader, &reader->link, NULL, 0);
    }
    if (reader->name.length == 0 ||
        ((type == TB_TAR_HARDLINK || type == TB_TAR_SYMLINK) && reader->link.length == 0))
    {
        return TB_TAR_MALFORMED;
    }
    if (reader->name.length > reader->max_name || reader->link.length > reader->max_name)
    {
        return TB_TAR_TOO_LARGE;
    }
    reader->remaining = size;
    reader->padding = (TB_TAR_BLOCK - size % TB_TAR_BLOCK) % TB_TAR_BLOCK;
    reader->state = TB_TAR_READER_ENTRY;
    entry->name = reader->name.bytes;
    entry->name_length = reader->name.length;
    entry->link = reader->link.bytes;
    entry->link_length = reader->link.length;
    entry->type = type;
    entry->flag = flag;
    entry->size = size;
    entry->mode = (uint32_t)(mode & TB_TAR_MODE_MASK);
    entry->modified = reader->has_modified ? reader->modified : modified;
    return TB_TAR_OK;
}

/* After a zero block: a second one ends the archive, and so does the end of
 * the source, as some writers leave only one. */
static tb_tar_result tb_tar_read_end(tb_tar_reader *reader)
{
    bool at_end = false;
    const tb_tar_result result = tb_tar_read_block(reader, &at_end);
    if (result != TB_TAR_OK)
    {
        return result;
    }
    if (!at_end && !tb_tar_is_zero_block(reader->header))
    {
        return TB_TAR_MALFORMED;
    }
    return TB_TAR_END;
}

/* Read headers until one describes an entry. */
static tb_tar_result tb_tar_next_header(tb_tar_reader *reader, tb_tar_entry *entry)
{
    bool extended = false; /* An extended header was read for this entry. */
    for (;;)
    {
        bool at_end = false;
        uint64_t size = 0;
        tb_tar_header_format format = TB_TAR_FORMAT_OLD;
        tb_tar_result result = TB_TAR_OK;
        reader->offset = reader->position;
        result = tb_tar_read_block(reader, &at_end);
        if (result != TB_TAR_OK)
        {
            return result;
        }
        if (at_end)
        {
            const bool allowed = (reader->flags & TB_TAR_ALLOW_MISSING_END) != 0;
            return allowed && !extended ? TB_TAR_END : TB_TAR_TRUNCATED;
        }
        if (tb_tar_is_zero_block(reader->header))
        {
            return extended ? TB_TAR_MALFORMED : tb_tar_read_end(reader);
        }
        result = tb_tar_check_header(reader->header, &format);
        if (result != TB_TAR_OK)
        {
            return result;
        }
        if (!tb_tar_unsigned_field(reader->header + TB_TAR_SIZE, TB_TAR_SIZE_WIDTH, &size))
        {
            return TB_TAR_MALFORMED;
        }
        switch ((char)reader->header[TB_TAR_TYPE])
        {
        case 'x':
        case 'g':
        case 'L':
        case 'K':
            result = tb_tar_extended_header(reader, (char)reader->header[TB_TAR_TYPE], size);
            if (result != TB_TAR_OK)
            {
                return result;
            }
            /* A global header belongs to no entry. */
            extended = extended || reader->header[TB_TAR_TYPE] != 'g';
            break;
        case 'S': /* A GNU sparse file. */
        case 'M': /* The rest of a file from the volume before. */
            return TB_TAR_UNSUPPORTED;
        default:
            return tb_tar_take_entry(reader, format, size, entry);
        }
    }
}

tb_tar_result tb_tar_reader_next(tb_tar_reader *reader, tb_tar_entry *entry)
{
    tb_tar_result result = TB_TAR_OK;
    TB_ASSERT(reader != NULL);
    TB_ASSERT(entry != NULL);
    *entry = (tb_tar_entry){0};
    if (reader->state == TB_TAR_READER_DONE)
    {
        return reader->outcome;
    }
    if (reader->state == TB_TAR_READER_ENTRY)
    {
        reader->offset = reader->position;
        result = tb_tar_skip(reader, reader->remaining + reader->padding);
        if (result != TB_TAR_OK)
        {
            return tb_tar_fail(reader, result);
        }
        reader->remaining = 0;
        reader->padding = 0;
    }
    reader->generation++;
    reader->state = TB_TAR_READER_BEFORE;
    reader->has_name = false;
    reader->has_link = false;
    reader->has_size = false;
    reader->has_modified = false;
    result = tb_tar_next_header(reader, entry);
    if (result != TB_TAR_OK)
    {
        *entry = (tb_tar_entry){0};
        return tb_tar_fail(reader, result);
    }
    return TB_TAR_OK;
}

tb_tar_result tb_tar_reader_read(tb_tar_reader *reader, void *bytes, size_t capacity,
                                 size_t *transferred)
{
    size_t want = capacity;
    tb_stream_result read = TB_STREAM_OK;
    TB_ASSERT(reader != NULL);
    TB_ASSERT(bytes != NULL || capacity == 0);
    TB_ASSERT(transferred != NULL);
    *transferred = 0;
    TB_ASSERT_MSG(reader->state != TB_TAR_READER_BEFORE,
                  "no entry to read: call tb_tar_reader_next");
    if (reader->state == TB_TAR_READER_DONE)
    {
        TB_ASSERT_MSG(reader->outcome != TB_TAR_END, "no entry to read: the archive has ended");
        return reader->outcome;
    }
    if (reader->remaining < want)
    {
        want = (size_t)reader->remaining;
    }
    if (want == 0)
    {
        return TB_TAR_OK;
    }
    read = tb_stream_read(&reader->source, bytes, want, transferred);
    reader->position += *transferred;
    reader->remaining -= *transferred;
    if (read != TB_STREAM_OK)
    {
        reader->source_result = read;
        reader->offset = reader->position;
        return tb_tar_fail(reader, TB_TAR_INPUT_FAILED);
    }
    if (*transferred == 0)
    {
        reader->offset = reader->position;
        return tb_tar_fail(reader, TB_TAR_TRUNCATED);
    }
    return TB_TAR_OK;
}

uint64_t tb_tar_reader_offset(const tb_tar_reader *reader)
{
    TB_ASSERT(reader != NULL);
    return reader->offset;
}

tb_tar_reader *tb_tar_reader_create(tb_stream *source, const tb_tar_options *options,
                                    const tb_allocator *allocator)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_tar_reader *reader = NULL;
    TB_ASSERT(source != NULL && source->functions != NULL);
    TB_ASSERT_MSG(options == NULL || (options->flags & ~(unsigned)TB_TAR_ALLOW_MISSING_END) == 0,
                  "unknown TAR reading flags");
    reader = TB_ALLOCATOR_NEW(&resolved, tb_tar_reader);
    reader->source = *source;
    reader->allocator = resolved;
    reader->flags = options == NULL ? 0u : options->flags;
    reader->max_name =
        options == NULL || options->max_name == 0 ? TB_TAR_DEFAULT_MAX_NAME : options->max_name;
    reader->max_extended = options == NULL || options->max_extended == 0
                               ? TB_TAR_DEFAULT_MAX_EXTENDED
                               : options->max_extended;
    reader->state = TB_TAR_READER_BEFORE;
    return reader;
}

void tb_tar_reader_destroy(tb_tar_reader *reader)
{
    tb_allocator allocator = {0};
    if (reader == NULL)
    {
        return;
    }
    allocator = reader->allocator;
    tb_tar_text_free(reader, &reader->name);
    tb_tar_text_free(reader, &reader->link);
    tb_tar_text_free(reader, &reader->extended);
    TB_ALLOCATOR_DELETE(&allocator, tb_tar_reader, reader);
}

/* -------------------------------------------------------------------------
 * A stream of an entry's data.
 * ------------------------------------------------------------------------- */

typedef struct tb_tar_data_stream
{
    tb_tar_reader *reader;
    uint64_t generation; /* The reader's, at the entry the stream reads. */
    tb_allocator allocator;
} tb_tar_data_stream;

static tb_stream_result tb_tar_data_stream_read(void *state, void *bytes, size_t capacity,
                                                size_t *transferred)
{
    tb_tar_data_stream *const stream = state;
    tb_tar_reader *const reader = stream->reader;
    tb_tar_result result = TB_TAR_OK;
    *transferred = 0;
    if (reader->generation != stream->generation ||
        (reader->state == TB_TAR_READER_DONE && reader->outcome == TB_TAR_END))
    {
        return TB_STREAM_INVALID;
    }
    result = tb_tar_reader_read(reader, bytes, capacity, transferred);
    switch (result)
    {
    case TB_TAR_OK:
        return TB_STREAM_OK;
    case TB_TAR_TRUNCATED:
        return TB_STREAM_END;
    case TB_TAR_INPUT_FAILED:
        return reader->source_result;
    case TB_TAR_END:
    case TB_TAR_INVALID:
    case TB_TAR_MALFORMED:
    case TB_TAR_TOO_LARGE:
    case TB_TAR_UNSUPPORTED:
    case TB_TAR_OUTPUT_FAILED:
    default:
        return TB_STREAM_MALFORMED;
    }
}

static tb_stream_result tb_tar_data_stream_release(void *state)
{
    tb_tar_data_stream *const stream = state;
    const tb_allocator allocator = stream->allocator;
    TB_ALLOCATOR_DELETE(&allocator, tb_tar_data_stream, stream);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_tar_data_stream_functions = {
    .read_bytes = tb_tar_data_stream_read,
    .release = tb_tar_data_stream_release,
};

void tb_tar_reader_open_data(tb_tar_reader *reader, tb_stream *stream)
{
    tb_tar_data_stream *state = NULL;
    TB_ASSERT(reader != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    TB_ASSERT_MSG(reader->state != TB_TAR_READER_BEFORE &&
                      !(reader->state == TB_TAR_READER_DONE && reader->outcome == TB_TAR_END),
                  "no entry to read: call tb_tar_reader_next");
    state = TB_ALLOCATOR_NEW(&reader->allocator, tb_tar_data_stream);
    state->reader = reader;
    state->generation = reader->generation;
    state->allocator = reader->allocator;
    stream->functions = &tb_tar_data_stream_functions;
    stream->state = state;
}
