/* Writing TAR archives in order to a stream. */
#include "badger/tar.h"

#include "badger/assert.h"
#include "badger/str.h"
#include "badger/utf8.h"
#include "tar_internal.h"

#include <stdbool.h>
#include <string.h>

/* The name of a pax header's own entry, as Python's tarfile writes it. */
#define TB_TAR_PAX_NAME "././@PaxHeader"

/* Permissions for an entry that gives none. */
#define TB_TAR_FILE_MODE 0644u
#define TB_TAR_DIRECTORY_MODE 0755u
#define TB_TAR_SYMLINK_MODE 0777u

/* Digits of the largest uint64_t or int64_t, with a sign. */
#define TB_TAR_DECIMAL_DIGITS 21u

struct tb_tar_writer
{
    tb_stream sink;
    tb_allocator allocator;
    uint64_t remaining; /* Data the current file still owes. */
    size_t padding;     /* Zeros after the file's data, to the end of its block. */
    bool failed;
    bool finished;
    /* The records of a pax header, while they are made. */
    char *records;
    size_t records_length;
    size_t records_capacity;
};

static const unsigned char tb_tar_zeros[TB_TAR_BLOCK] = {0};

/* -------------------------------------------------------------------------
 * Output.
 * ------------------------------------------------------------------------- */

static tb_tar_result tb_tar_emit(tb_tar_writer *writer, const void *bytes, size_t length)
{
    if (writer->failed)
    {
        return TB_TAR_OUTPUT_FAILED;
    }
    if (tb_stream_write_all(&writer->sink, bytes, length) != TB_STREAM_OK)
    {
        writer->failed = true;
        return TB_TAR_OUTPUT_FAILED;
    }
    return TB_TAR_OK;
}

static size_t tb_tar_padding_of(uint64_t size)
{
    return (size_t)((TB_TAR_BLOCK - size % TB_TAR_BLOCK) % TB_TAR_BLOCK);
}

/* -------------------------------------------------------------------------
 * What an entry may be.
 * ------------------------------------------------------------------------- */

static bool tb_tar_is_text(const char *text, size_t length)
{
    size_t error = 0;
    return text != NULL && length > 0 && memchr(text, '\0', length) == NULL &&
           tb_utf8_validate(text, length, &error);
}

/* A relative path whose components are neither empty, ".", nor "..". */
static bool tb_tar_is_relative_path(const char *name, size_t length)
{
    size_t start = 0;
    if (!tb_tar_is_text(name, length))
    {
        return false;
    }
    while (start <= length)
    {
        const char *const slash = memchr(name + start, '/', length - start);
        const size_t end = slash == NULL ? length : (size_t)(slash - name);
        const size_t component = end - start;
        const bool dots = (component == 1 && name[start] == '.') ||
                          (component == 2 && name[start] == '.' && name[start + 1] == '.');
        if (component == 0 || dots)
        {
            return false;
        }
        start = end + 1;
    }
    return true;
}

static bool tb_tar_is_link(tb_tar_type type)
{
    return type == TB_TAR_SYMLINK || type == TB_TAR_HARDLINK;
}

static bool tb_tar_entry_is_valid(const tb_tar_entry *entry)
{
    if (entry->type != TB_TAR_FILE && entry->type != TB_TAR_DIRECTORY &&
        !tb_tar_is_link(entry->type))
    {
        return false;
    }
    if (!tb_tar_is_relative_path(entry->name, entry->name_length))
    {
        return false;
    }
    if (tb_tar_is_link(entry->type) ? !tb_tar_is_text(entry->link, entry->link_length)
                                    : entry->link_length != 0)
    {
        return false;
    }
    return (entry->type == TB_TAR_FILE || entry->size == 0) && entry->mode <= TB_TAR_MODE_MASK;
}

/* -------------------------------------------------------------------------
 * Headers.
 * ------------------------------------------------------------------------- */

static void tb_tar_put_text(unsigned char *header, size_t offset, size_t width, const char *text,
                            size_t length)
{
    if (length > 0)
    {
        memcpy(header + offset, text, length < width ? length : width);
    }
}

/* A number as width - 1 octal digits and a NUL. */
static void tb_tar_put_octal(unsigned char *header, size_t offset, size_t width, uint64_t value)
{
    size_t index = width - 1;
    TB_ASSERT(width - 1 < 22 && value >> (3 * (width - 1)) == 0);
    header[offset + index] = '\0';
    while (index > 0)
    {
        index--;
        header[offset + index] = (unsigned char)('0' + (value & 7u));
        value >>= 3;
    }
}

/* Fill in the fields every header shares, and its checksum. */
static void tb_tar_seal_header(unsigned char *header)
{
    uint32_t unsigned_sum = 0;
    int32_t signed_sum = 0;
    tb_tar_put_octal(header, TB_TAR_UID, TB_TAR_ID_WIDTH, 0);
    tb_tar_put_octal(header, TB_TAR_GID, TB_TAR_ID_WIDTH, 0);
    tb_tar_put_octal(header, TB_TAR_DEVMAJOR, TB_TAR_DEV_WIDTH, 0);
    tb_tar_put_octal(header, TB_TAR_DEVMINOR, TB_TAR_DEV_WIDTH, 0);
    memcpy(header + TB_TAR_MAGIC, "ustar\0", TB_TAR_MAGIC_WIDTH);
    memcpy(header + TB_TAR_VERSION, "00", TB_TAR_VERSION_WIDTH);
    tb_tar_header_sums(header, &unsigned_sum, &signed_sum);
    /* Six digits, a NUL, and a space, as POSIX writes it. */
    tb_tar_put_octal(header, TB_TAR_CHECKSUM, TB_TAR_CHECKSUM_WIDTH - 1u, unsigned_sum);
    header[TB_TAR_CHECKSUM + TB_TAR_CHECKSUM_WIDTH - 1u] = ' ';
}

/* Where to split a name too long for the name field between the prefix and
 * name fields: at a slash with at most 155 bytes before it and 1 to 100
 * after it. Returns the slash's index, or 0 for none. */
static size_t tb_tar_split(const char *name, size_t length)
{
    const size_t first = length > TB_TAR_NAME_WIDTH + 1u ? length - TB_TAR_NAME_WIDTH - 1u : 1u;
    for (size_t index = first; index <= TB_TAR_PREFIX_WIDTH && index + 1 < length; index++)
    {
        if (name[index] == '/')
        {
            return index;
        }
    }
    return 0;
}

static size_t tb_tar_decimal(uint64_t value, char *digits)
{
    char reversed[TB_TAR_DECIMAL_DIGITS] = {0};
    size_t count = 0;
    do
    {
        reversed[count] = (char)('0' + value % 10u);
        count++;
        value /= 10u;
    } while (value > 0);
    for (size_t index = 0; index < count; index++)
    {
        digits[index] = reversed[count - 1 - index];
    }
    return count;
}

static void tb_tar_records_append(tb_tar_writer *writer, const char *bytes, size_t length)
{
    if (length > writer->records_capacity - writer->records_length)
    {
        size_t capacity = writer->records_capacity == 0 ? TB_TAR_BLOCK : writer->records_capacity;
        while (capacity - writer->records_length < length)
        {
            TB_ASSERT(capacity <= SIZE_MAX / 2);
            capacity *= 2;
        }
        writer->records = tb_allocator_realloc(&writer->allocator, writer->records,
                                               writer->records_capacity, capacity, 1);
        writer->records_capacity = capacity;
    }
    memcpy(writer->records + writer->records_length, bytes, length);
    writer->records_length += length;
}

/* Add the pax record "<length> <key>=<value>\n", whose length counts itself. */
static void tb_tar_add_record(tb_tar_writer *writer, const char *key, const char *value,
                              size_t value_length)
{
    const size_t key_length = tb_str_length(key);
    char digits[TB_TAR_DECIMAL_DIGITS] = {0};
    size_t base = 0;
    size_t total = 0;
    size_t count = 0;
    TB_ASSERT(value_length < SIZE_MAX / 2);
    base = key_length + value_length + 3u; /* The space, the '=', and the newline. */
    total = base + tb_tar_decimal(base, digits);
    /* Counting its own digits can add a digit. */
    count = tb_tar_decimal(total, digits);
    if (base + count != total)
    {
        total = base + count;
        count = tb_tar_decimal(total, digits);
    }
    TB_ASSERT(base + count == total);
    tb_tar_records_append(writer, digits, count);
    tb_tar_records_append(writer, " ", 1);
    tb_tar_records_append(writer, key, key_length);
    tb_tar_records_append(writer, "=", 1);
    tb_tar_records_append(writer, value, value_length);
    tb_tar_records_append(writer, "\n", 1);
}

static void tb_tar_add_number_record(tb_tar_writer *writer, const char *key, bool negative,
                                     uint64_t magnitude)
{
    char text[TB_TAR_DECIMAL_DIGITS + 1u] = {0};
    size_t length = 0;
    if (negative)
    {
        text[0] = '-';
        length = 1;
    }
    length += tb_tar_decimal(magnitude, text + length);
    tb_tar_add_record(writer, key, text, length);
}

/* Write a pax header holding the records made, and its data. */
static tb_tar_result tb_tar_write_pax(tb_tar_writer *writer, uint64_t modified)
{
    unsigned char header[TB_TAR_BLOCK] = {0};
    tb_tar_result result = TB_TAR_OK;
    tb_tar_put_text(header, TB_TAR_NAME, TB_TAR_NAME_WIDTH, TB_TAR_PAX_NAME,
                    tb_str_length(TB_TAR_PAX_NAME));
    tb_tar_put_octal(header, TB_TAR_MODE, TB_TAR_MODE_WIDTH, TB_TAR_FILE_MODE);
    tb_tar_put_octal(header, TB_TAR_SIZE, TB_TAR_SIZE_WIDTH, writer->records_length);
    tb_tar_put_octal(header, TB_TAR_MTIME, TB_TAR_MTIME_WIDTH, modified);
    header[TB_TAR_TYPE] = 'x';
    tb_tar_seal_header(header);
    result = tb_tar_emit(writer, header, sizeof(header));
    if (result == TB_TAR_OK)
    {
        result = tb_tar_emit(writer, writer->records, writer->records_length);
    }
    if (result == TB_TAR_OK)
    {
        result = tb_tar_emit(writer, tb_tar_zeros, tb_tar_padding_of(writer->records_length));
    }
    return result;
}

static char tb_tar_flag_of(tb_tar_type type)
{
    TB_ASSERT(type != TB_TAR_OTHER);
    switch (type)
    {
    case TB_TAR_FILE:
        return '0';
    case TB_TAR_DIRECTORY:
        return '5';
    case TB_TAR_SYMLINK:
        return '2';
    case TB_TAR_HARDLINK:
        return '1';
    case TB_TAR_OTHER:
    default:
        return '0';
    }
}

static uint32_t tb_tar_mode_of(const tb_tar_entry *entry)
{
    if (entry->mode != 0)
    {
        return entry->mode;
    }
    switch (entry->type)
    {
    case TB_TAR_DIRECTORY:
        return TB_TAR_DIRECTORY_MODE;
    case TB_TAR_SYMLINK:
        return TB_TAR_SYMLINK_MODE;
    case TB_TAR_FILE:
    case TB_TAR_HARDLINK:
    case TB_TAR_OTHER:
    default:
        return TB_TAR_FILE_MODE;
    }
}

/* Write the header of a valid entry, after a pax header for whatever the
 * ustar fields cannot hold. `path` is the name, with a slash after a
 * directory's. */
static tb_tar_result tb_tar_write_header(tb_tar_writer *writer, const tb_tar_entry *entry,
                                         const char *path, size_t length)
{
    unsigned char header[TB_TAR_BLOCK] = {0};
    const bool time_fits = entry->modified >= 0 && (uint64_t)entry->modified <= TB_TAR_OCTAL_11_MAX;
    const uint64_t modified = time_fits ? (uint64_t)entry->modified : 0u;
    const bool size_fits = entry->size <= TB_TAR_OCTAL_11_MAX;
    size_t split = 0;
    writer->records_length = 0;
    if (length > TB_TAR_NAME_WIDTH)
    {
        split = tb_tar_split(path, length);
        if (split == 0)
        {
            tb_tar_add_record(writer, "path", path, length);
        }
    }
    if (entry->link_length > TB_TAR_LINK_WIDTH)
    {
        tb_tar_add_record(writer, "linkpath", entry->link, entry->link_length);
    }
    if (!size_fits)
    {
        tb_tar_add_number_record(writer, "size", false, entry->size);
    }
    if (!time_fits)
    {
        const bool negative = entry->modified < 0;
        /* The magnitude of a negative time, without overflow at INT64_MIN. */
        const uint64_t magnitude =
            negative ? (uint64_t)(-(entry->modified + 1)) + 1u : (uint64_t)entry->modified;
        tb_tar_add_number_record(writer, "mtime", negative, magnitude);
    }
    if (writer->records_length > 0)
    {
        const tb_tar_result result = tb_tar_write_pax(writer, modified);
        if (result != TB_TAR_OK)
        {
            return result;
        }
    }
    if (split > 0)
    {
        tb_tar_put_text(header, TB_TAR_PREFIX, TB_TAR_PREFIX_WIDTH, path, split);
        tb_tar_put_text(header, TB_TAR_NAME, TB_TAR_NAME_WIDTH, path + split + 1,
                        length - split - 1);
    }
    else
    {
        /* A name in the pax header leaves its start here, for readers without pax. */
        tb_tar_put_text(header, TB_TAR_NAME, TB_TAR_NAME_WIDTH, path, length);
    }
    tb_tar_put_octal(header, TB_TAR_MODE, TB_TAR_MODE_WIDTH, tb_tar_mode_of(entry));
    tb_tar_put_octal(header, TB_TAR_SIZE, TB_TAR_SIZE_WIDTH, size_fits ? entry->size : 0u);
    tb_tar_put_octal(header, TB_TAR_MTIME, TB_TAR_MTIME_WIDTH, modified);
    header[TB_TAR_TYPE] = (unsigned char)tb_tar_flag_of(entry->type);
    tb_tar_put_text(header, TB_TAR_LINK, TB_TAR_LINK_WIDTH, entry->link, entry->link_length);
    tb_tar_seal_header(header);
    return tb_tar_emit(writer, header, sizeof(header));
}

/* -------------------------------------------------------------------------
 * The writer.
 * ------------------------------------------------------------------------- */

tb_tar_writer *tb_tar_writer_create(tb_stream *sink, const tb_allocator *allocator)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_tar_writer *writer = NULL;
    TB_ASSERT(sink != NULL && sink->functions != NULL);
    writer = TB_ALLOCATOR_NEW(&resolved, tb_tar_writer);
    writer->sink = *sink;
    writer->allocator = resolved;
    return writer;
}

void tb_tar_writer_destroy(tb_tar_writer *writer)
{
    tb_allocator allocator = {0};
    if (writer == NULL)
    {
        return;
    }
    allocator = writer->allocator;
    tb_allocator_free(&allocator, writer->records, writer->records_capacity, 1);
    TB_ALLOCATOR_DELETE(&allocator, tb_tar_writer, writer);
}

tb_tar_result tb_tar_write_entry(tb_tar_writer *writer, const tb_tar_entry *entry)
{
    char *path = NULL;
    size_t length = 0;
    tb_tar_result result = TB_TAR_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(entry != NULL);
    TB_ASSERT_MSG(!writer->finished, "the TAR archive is finished");
    TB_ASSERT_MSG(writer->remaining == 0, "the last entry's data is not complete");
    if (!tb_tar_entry_is_valid(entry))
    {
        return TB_TAR_INVALID;
    }
    /* Begun even when the sink fails, so the caller's data goes where it
     * would have, and is refused in turn. */
    writer->remaining = entry->size;
    writer->padding = tb_tar_padding_of(entry->size);
    if (writer->failed)
    {
        return TB_TAR_OUTPUT_FAILED;
    }
    /* A directory's name is written with a slash after it. */
    TB_ASSERT(entry->name_length < SIZE_MAX - 1);
    length = entry->name_length + (entry->type == TB_TAR_DIRECTORY ? 1u : 0u);
    path = tb_allocator_alloc(&writer->allocator, length, 1);
    memcpy(path, entry->name, entry->name_length);
    if (entry->type == TB_TAR_DIRECTORY)
    {
        path[length - 1] = '/';
    }
    result = tb_tar_write_header(writer, entry, path, length);
    tb_allocator_free(&writer->allocator, path, length, 1);
    return result;
}

tb_tar_result tb_tar_write_data(tb_tar_writer *writer, const void *bytes, size_t length)
{
    tb_tar_result result = TB_TAR_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT_MSG(!writer->finished, "the TAR archive is finished");
    TB_ASSERT_MSG(length <= writer->remaining, "more data than the entry's size");
    writer->remaining -= length;
    result = tb_tar_emit(writer, bytes, length);
    if (result == TB_TAR_OK && writer->remaining == 0 && writer->padding > 0)
    {
        result = tb_tar_emit(writer, tb_tar_zeros, writer->padding);
        writer->padding = 0;
    }
    return result;
}

tb_tar_result tb_tar_writer_finish(tb_tar_writer *writer)
{
    tb_tar_result result = TB_TAR_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT_MSG(!writer->finished, "the TAR archive is finished");
    TB_ASSERT_MSG(writer->remaining == 0, "the last entry's data is not complete");
    writer->finished = true;
    /* Two zero blocks end the archive. */
    result = tb_tar_emit(writer, tb_tar_zeros, TB_TAR_BLOCK);
    if (result == TB_TAR_OK)
    {
        result = tb_tar_emit(writer, tb_tar_zeros, TB_TAR_BLOCK);
    }
    if (result == TB_TAR_OK && tb_stream_flush(&writer->sink) != TB_STREAM_OK)
    {
        writer->failed = true;
        result = TB_TAR_OUTPUT_FAILED;
    }
    return result;
}
