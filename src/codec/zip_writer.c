/* Writing ZIP archives in order to a stream: each entry with a data
 * descriptor, and the central directory at the finish. */
#include "badger/zip.h"

#include "archive_internal.h"
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/set.h"
#include "zip_internal.h"

#include <stdbool.h>
#include <string.h>

/* Compressed bytes gathered before they go to the sink. */
#define TB_ZIP_OUTPUT_BUFFER ((size_t)16384)

/* A size from which an entry is written with ZIP64 fields: far enough below
 * 4 GiB that DEFLATE's few bytes of growth cannot reach it. */
#define TB_ZIP_ZIP64_THRESHOLD UINT64_C(0xF0000000)

/* "Version made by" and "needed": 2.0 for DEFLATE, 4.5 for ZIP64. */
#define TB_ZIP_VERSION_DEFLATE 20u
#define TB_ZIP_VERSION_ZIP64 45u

/* Permissions for an entry that gives none. */
#define TB_ZIP_FILE_MODE 0644u
#define TB_ZIP_DIRECTORY_MODE 0755u

/* What the central directory needs of an entry. */
typedef struct tb_zip_written
{
    char *name; /* Without a directory's slash; NUL after it. */
    size_t name_length;
    tb_zip_type type;
    uint16_t method;
    uint16_t flags;
    uint16_t dos_date;
    uint16_t dos_time;
    bool has_time; /* The exact time fits the extended timestamp. */
    bool zip64;    /* The local header has ZIP64 fields. */
    int32_t time;
    uint32_t mode;
    uint32_t crc32;
    uint64_t size;
    uint64_t compressed_size;
    uint64_t local_offset;
} tb_zip_written;

struct tb_zip_writer
{
    tb_stream sink;
    tb_allocator allocator;
    uint64_t position; /* Bytes written. */
    tb_zip_written *entries;
    size_t count;
    size_t capacity;
    tb_set *names;
    tb_deflater *deflater;
    int deflater_level;
    uint64_t remaining; /* Data the current file still owes. */
    bool failed;
    bool finished;
    unsigned char output[TB_ZIP_OUTPUT_BUFFER];
};

/* -------------------------------------------------------------------------
 * Output.
 * ------------------------------------------------------------------------- */

static tb_zip_result tb_zip_emit(tb_zip_writer *writer, const void *bytes, size_t length)
{
    if (writer->failed)
    {
        return TB_ZIP_OUTPUT_FAILED;
    }
    if (tb_stream_write_all(&writer->sink, bytes, length) != TB_STREAM_OK)
    {
        writer->failed = true;
        return TB_ZIP_OUTPUT_FAILED;
    }
    writer->position += length;
    return TB_ZIP_OK;
}

/* -------------------------------------------------------------------------
 * Times.
 * ------------------------------------------------------------------------- */

/* A time in UTC as a DOS date and time, the nearest that DOS can hold. */
static void tb_zip_to_dos(int64_t modified, uint16_t *date, uint16_t *time)
{
    const int64_t clamped = modified < TB_ZIP_DOS_FIRST  ? TB_ZIP_DOS_FIRST
                            : modified > TB_ZIP_DOS_LAST ? TB_ZIP_DOS_LAST
                                                         : modified;
    const int64_t days = clamped / 86400;
    const int64_t seconds = clamped % 86400;
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    tb_zip_civil_from_days(days, &year, &month, &day);
    *date = (uint16_t)(((uint32_t)(year - 1980) << 9) | (month << 5) | day);
    *time = (uint16_t)(((uint32_t)(seconds / 3600) << 11) | ((uint32_t)(seconds / 60 % 60) << 5) |
                       (uint32_t)(seconds % 60 / 2));
}

/* -------------------------------------------------------------------------
 * Entries.
 * ------------------------------------------------------------------------- */

static bool tb_zip_entry_is_valid(const tb_zip_entry *entry, int level)
{
    if (entry->type != TB_ZIP_FILE && entry->type != TB_ZIP_DIRECTORY)
    {
        return false;
    }
    if (!tb_archive_is_relative_path(entry->name, entry->name_length) ||
        entry->name_length >= TB_ZIP_MAX_FIELD)
    {
        return false;
    }
    if (entry->method != TB_ZIP_STORED && entry->method != TB_ZIP_DEFLATED)
    {
        return false;
    }
    if (entry->type == TB_ZIP_DIRECTORY && entry->size != 0)
    {
        return false;
    }
    return entry->mode <= TB_ZIP_UNIX_MODE_MASK && level >= 0 && level <= 9;
}

/* Keep what the central directory needs; the name is copied. */
static tb_zip_written *tb_zip_remember(tb_zip_writer *writer, const tb_zip_entry *entry)
{
    tb_zip_written *written = NULL;
    if (writer->count == writer->capacity)
    {
        const size_t capacity = writer->capacity == 0 ? 64u : writer->capacity * 2;
        TB_ASSERT(writer->capacity <= SIZE_MAX / 2 / sizeof(tb_zip_written));
        writer->entries = tb_allocator_realloc(
            &writer->allocator, writer->entries, writer->capacity * sizeof(tb_zip_written),
            capacity * sizeof(tb_zip_written), _Alignof(tb_zip_written));
        writer->capacity = capacity;
    }
    written = &writer->entries[writer->count];
    *written = (tb_zip_written){0};
    written->name = tb_allocator_alloc(&writer->allocator, entry->name_length + 1, 1);
    memcpy(written->name, entry->name, entry->name_length);
    written->name[entry->name_length] = '\0';
    written->name_length = entry->name_length;
    writer->count++;
    return written;
}

/* Write a local header, with a slash after a directory's name. */
static tb_zip_result tb_zip_write_local(tb_zip_writer *writer, const tb_zip_written *written,
                                        bool descriptor)
{
    unsigned char header[TB_ZIP_LOCAL_LENGTH] = {0};
    unsigned char extra[9 + 20] = {0};
    size_t extra_length = 0;
    const bool directory = written->type == TB_ZIP_DIRECTORY;
    const uint32_t unknown = written->zip64 ? (uint32_t)TB_ZIP_MAX_32 : 0u;
    tb_zip_result result = TB_ZIP_OK;
    if (written->has_time)
    {
        tb_zip_put16(extra, TB_ZIP_EXTRA_TIMESTAMP);
        tb_zip_put16(extra + 2, 5);
        extra[4] = 1;
        tb_zip_put32(extra + 5, (uint32_t)written->time);
        extra_length = 9;
    }
    if (written->zip64)
    {
        /* The sizes come in the data descriptor; these are placeholders. */
        tb_zip_put16(extra + extra_length, TB_ZIP_EXTRA_ZIP64);
        tb_zip_put16(extra + extra_length + 2, 16);
        extra_length += 20;
    }
    tb_zip_put32(header, TB_ZIP_LOCAL_SIGNATURE);
    tb_zip_put16(header + 4, written->zip64 ? TB_ZIP_VERSION_ZIP64 : TB_ZIP_VERSION_DEFLATE);
    tb_zip_put16(header + 6, written->flags);
    tb_zip_put16(header + 8, written->method);
    tb_zip_put16(header + 10, written->dos_time);
    tb_zip_put16(header + 12, written->dos_date);
    tb_zip_put32(header + 14, descriptor ? 0u : written->crc32);
    tb_zip_put32(header + 18, descriptor ? unknown : written->compressed_size);
    tb_zip_put32(header + 22, descriptor ? unknown : written->size);
    tb_zip_put16(header + 26, (uint32_t)(written->name_length + (directory ? 1u : 0u)));
    tb_zip_put16(header + 28, (uint32_t)extra_length);
    result = tb_zip_emit(writer, header, sizeof(header));
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_emit(writer, written->name, written->name_length);
    }
    if (result == TB_ZIP_OK && directory)
    {
        result = tb_zip_emit(writer, "/", 1);
    }
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_emit(writer, extra, extra_length);
    }
    return result;
}

/* Prepare the deflater for the current entry, at its level. */
static void tb_zip_prepare_deflater(tb_zip_writer *writer, int level)
{
    if (writer->deflater != NULL && writer->deflater_level == level)
    {
        tb_deflater_reset(writer->deflater);
        return;
    }
    tb_deflater_destroy(writer->deflater);
    writer->deflater = tb_deflater_create(TB_DEFLATE_RAW, level, &writer->allocator);
    writer->deflater_level = level;
}

tb_zip_result tb_zip_write_entry(tb_zip_writer *writer, const tb_zip_entry *entry, int level)
{
    tb_zip_written *written = NULL;
    bool descriptor = false;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(entry != NULL);
    TB_ASSERT_MSG(!writer->finished, "the ZIP archive is finished");
    TB_ASSERT_MSG(writer->remaining == 0, "the last entry's data is not complete");
    if (!tb_zip_entry_is_valid(entry, level))
    {
        return TB_ZIP_INVALID;
    }
    written = tb_zip_remember(writer, entry);
    if (!tb_set_put(writer->names, written->name))
    {
        tb_allocator_free(&writer->allocator, written->name, written->name_length + 1, 1);
        writer->count--;
        return TB_ZIP_INVALID;
    }
    written->type = entry->type;
    written->size = entry->size;
    written->mode = entry->mode != 0                  ? entry->mode
                    : entry->type == TB_ZIP_DIRECTORY ? TB_ZIP_DIRECTORY_MODE
                                                      : TB_ZIP_FILE_MODE;
    written->local_offset = writer->position;
    written->has_time = entry->modified >= INT32_MIN && entry->modified <= INT32_MAX;
    written->time = written->has_time ? (int32_t)entry->modified : 0;
    tb_zip_to_dos(entry->modified, &written->dos_date, &written->dos_time);
    /* Only data needs a descriptor: an entry without any is complete in its header. */
    descriptor = entry->size > 0;
    written->method = descriptor ? entry->method : (uint16_t)TB_ZIP_STORED;
    written->zip64 = entry->size >= TB_ZIP_ZIP64_THRESHOLD;
    written->flags = (uint16_t)(TB_ZIP_FLAG_UTF8 | (descriptor ? TB_ZIP_FLAG_DESCRIPTOR : 0u));
    /* Begun even when the sink fails, so the caller's data goes where it
     * would have, and is refused in turn. */
    writer->remaining = entry->size;
    if (written->method == TB_ZIP_DEFLATED)
    {
        tb_zip_prepare_deflater(writer, level);
    }
    return tb_zip_write_local(writer, written, descriptor);
}

/* Compress input, or finish the stream, sending what comes out to the sink. */
static tb_zip_result tb_zip_deflate(tb_zip_writer *writer, tb_zip_written *written,
                                    const unsigned char *bytes, size_t length, bool finish)
{
    tb_zip_result result = TB_ZIP_OK;
    for (;;)
    {
        size_t used = 0;
        size_t produced = 0;
        const tb_deflate_result deflated = tb_deflater_run(
            writer->deflater, bytes, length, &used, writer->output, sizeof(writer->output),
            &produced, finish ? TB_DEFLATE_FINISH : TB_DEFLATE_NO_FLUSH);
        TB_ASSERT(deflated == TB_DEFLATE_OK);
        bytes += used;
        length -= used;
        written->compressed_size += produced;
        result = tb_zip_emit(writer, writer->output, produced);
        if (result != TB_ZIP_OK)
        {
            return result;
        }
        if (finish ? tb_deflater_finished(writer->deflater)
                   : length == 0 && produced < sizeof(writer->output))
        {
            return TB_ZIP_OK;
        }
    }
}

/* The CRC-32 and sizes, after the data. */
static tb_zip_result tb_zip_write_descriptor(tb_zip_writer *writer, const tb_zip_written *written)
{
    unsigned char descriptor[24] = {0};
    size_t length = 0;
    tb_zip_put32(descriptor, TB_ZIP_DESCRIPTOR_SIGNATURE);
    tb_zip_put32(descriptor + 4, written->crc32);
    if (written->zip64)
    {
        tb_zip_put64(descriptor + 8, written->compressed_size);
        tb_zip_put64(descriptor + 16, written->size);
        length = 24;
    }
    else
    {
        tb_zip_put32(descriptor + 8, written->compressed_size);
        tb_zip_put32(descriptor + 12, written->size);
        length = 16;
    }
    return tb_zip_emit(writer, descriptor, length);
}

tb_zip_result tb_zip_write_data(tb_zip_writer *writer, const void *bytes, size_t length)
{
    tb_zip_written *written = NULL;
    tb_zip_result result = TB_ZIP_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT_MSG(!writer->finished, "the ZIP archive is finished");
    TB_ASSERT_MSG(length <= writer->remaining, "more data than the entry's size");
    if (length == 0)
    {
        return writer->failed ? TB_ZIP_OUTPUT_FAILED : TB_ZIP_OK;
    }
    written = &writer->entries[writer->count - 1];
    writer->remaining -= length;
    written->crc32 = tb_crc32_update(written->crc32, bytes, length);
    if (written->method == TB_ZIP_DEFLATED)
    {
        result = tb_zip_deflate(writer, written, bytes, length, false);
    }
    else
    {
        written->compressed_size += length;
        result = tb_zip_emit(writer, bytes, length);
    }
    if (writer->remaining > 0)
    {
        return result;
    }
    if (result == TB_ZIP_OK && written->method == TB_ZIP_DEFLATED)
    {
        result = tb_zip_deflate(writer, written, NULL, 0, true);
    }
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_write_descriptor(writer, written);
    }
    return result;
}

/* -------------------------------------------------------------------------
 * The central directory.
 * ------------------------------------------------------------------------- */

static tb_zip_result tb_zip_write_central(tb_zip_writer *writer, const tb_zip_written *written)
{
    unsigned char header[TB_ZIP_CENTRAL_LENGTH] = {0};
    unsigned char extra[9 + 28] = {0};
    size_t extra_length = 0;
    size_t zip64_length = 0;
    const bool directory = written->type == TB_ZIP_DIRECTORY;
    const bool large_size = written->size >= TB_ZIP_MAX_32;
    const bool large_compressed = written->compressed_size >= TB_ZIP_MAX_32;
    const bool large_offset = written->local_offset >= TB_ZIP_MAX_32;
    const bool zip64 = written->zip64 || large_size || large_compressed || large_offset;
    const uint32_t type_bits = directory ? TB_ZIP_UNIX_DIRECTORY : TB_ZIP_UNIX_FILE;
    tb_zip_result result = TB_ZIP_OK;
    if (written->has_time)
    {
        tb_zip_put16(extra, TB_ZIP_EXTRA_TIMESTAMP);
        tb_zip_put16(extra + 2, 5);
        extra[4] = 1;
        tb_zip_put32(extra + 5, (uint32_t)written->time);
        extra_length = 9;
    }
    if (large_size || large_compressed || large_offset)
    {
        unsigned char *const field = extra + extra_length;
        zip64_length = 4;
        if (large_size)
        {
            tb_zip_put64(field + zip64_length, written->size);
            zip64_length += 8;
        }
        if (large_compressed)
        {
            tb_zip_put64(field + zip64_length, written->compressed_size);
            zip64_length += 8;
        }
        if (large_offset)
        {
            tb_zip_put64(field + zip64_length, written->local_offset);
            zip64_length += 8;
        }
        tb_zip_put16(field, TB_ZIP_EXTRA_ZIP64);
        tb_zip_put16(field + 2, (uint32_t)(zip64_length - 4));
        extra_length += zip64_length;
    }
    tb_zip_put32(header, TB_ZIP_CENTRAL_SIGNATURE);
    tb_zip_put16(header + 4,
                 TB_ZIP_HOST_UNIX << 8 | (zip64 ? TB_ZIP_VERSION_ZIP64 : TB_ZIP_VERSION_DEFLATE));
    tb_zip_put16(header + 6, zip64 ? TB_ZIP_VERSION_ZIP64 : TB_ZIP_VERSION_DEFLATE);
    tb_zip_put16(header + 8, written->flags);
    tb_zip_put16(header + 10, written->method);
    tb_zip_put16(header + 12, written->dos_time);
    tb_zip_put16(header + 14, written->dos_date);
    tb_zip_put32(header + 16, written->crc32);
    tb_zip_put32(header + 20, large_compressed ? TB_ZIP_MAX_32 : written->compressed_size);
    tb_zip_put32(header + 24, large_size ? TB_ZIP_MAX_32 : written->size);
    tb_zip_put16(header + 28, (uint32_t)(written->name_length + (directory ? 1u : 0u)));
    tb_zip_put16(header + 30, (uint32_t)extra_length);
    tb_zip_put32(header + 38, (uint64_t)((type_bits | written->mode) << 16 |
                                         (directory ? TB_ZIP_DOS_DIRECTORY : 0u)));
    tb_zip_put32(header + 42, large_offset ? TB_ZIP_MAX_32 : written->local_offset);
    result = tb_zip_emit(writer, header, sizeof(header));
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_emit(writer, written->name, written->name_length);
    }
    if (result == TB_ZIP_OK && directory)
    {
        result = tb_zip_emit(writer, "/", 1);
    }
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_emit(writer, extra, extra_length);
    }
    return result;
}

/* The end records: ZIP64's when a count, size, or offset needs them, then the classic one. */
static tb_zip_result tb_zip_write_end(tb_zip_writer *writer, uint64_t directory_offset,
                                      uint64_t directory_size)
{
    unsigned char end64[TB_ZIP_END64_LENGTH + TB_ZIP_LOCATOR_LENGTH] = {0};
    unsigned char end[TB_ZIP_END_LENGTH] = {0};
    const uint64_t entries = writer->count;
    const bool zip64 = entries >= TB_ZIP_MAX_16 || directory_offset >= TB_ZIP_MAX_32 ||
                       directory_size >= TB_ZIP_MAX_32;
    tb_zip_result result = TB_ZIP_OK;
    if (zip64)
    {
        unsigned char *const locator = end64 + TB_ZIP_END64_LENGTH;
        tb_zip_put32(end64, TB_ZIP_END64_SIGNATURE);
        tb_zip_put64(end64 + 4, TB_ZIP_END64_LENGTH - 12);
        tb_zip_put16(end64 + 12, TB_ZIP_HOST_UNIX << 8 | TB_ZIP_VERSION_ZIP64);
        tb_zip_put16(end64 + 14, TB_ZIP_VERSION_ZIP64);
        tb_zip_put64(end64 + 24, entries);
        tb_zip_put64(end64 + 32, entries);
        tb_zip_put64(end64 + 40, directory_size);
        tb_zip_put64(end64 + 48, directory_offset);
        tb_zip_put32(locator, TB_ZIP_LOCATOR_SIGNATURE);
        tb_zip_put64(locator + 8, writer->position);
        tb_zip_put32(locator + 16, 1);
        result = tb_zip_emit(writer, end64, sizeof(end64));
    }
    tb_zip_put32(end, TB_ZIP_END_SIGNATURE);
    tb_zip_put16(end + 8, entries >= TB_ZIP_MAX_16 ? TB_ZIP_MAX_16 : (uint32_t)entries);
    tb_zip_put16(end + 10, entries >= TB_ZIP_MAX_16 ? TB_ZIP_MAX_16 : (uint32_t)entries);
    tb_zip_put32(end + 12, directory_size >= TB_ZIP_MAX_32 ? TB_ZIP_MAX_32 : directory_size);
    tb_zip_put32(end + 16, directory_offset >= TB_ZIP_MAX_32 ? TB_ZIP_MAX_32 : directory_offset);
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_emit(writer, end, sizeof(end));
    }
    return result;
}

tb_zip_result tb_zip_writer_finish(tb_zip_writer *writer)
{
    uint64_t directory_offset = 0;
    tb_zip_result result = TB_ZIP_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT_MSG(!writer->finished, "the ZIP archive is finished");
    TB_ASSERT_MSG(writer->remaining == 0, "the last entry's data is not complete");
    writer->finished = true;
    directory_offset = writer->position;
    for (size_t index = 0; index < writer->count && result == TB_ZIP_OK; index++)
    {
        result = tb_zip_write_central(writer, &writer->entries[index]);
    }
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_write_end(writer, directory_offset, writer->position - directory_offset);
    }
    if (result == TB_ZIP_OK && tb_stream_flush(&writer->sink) != TB_STREAM_OK)
    {
        writer->failed = true;
        result = TB_ZIP_OUTPUT_FAILED;
    }
    return writer->failed ? TB_ZIP_OUTPUT_FAILED : result;
}

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

tb_zip_writer *tb_zip_writer_create(tb_stream *sink, const tb_allocator *allocator)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_zip_writer *writer = NULL;
    TB_ASSERT(sink != NULL && sink->functions != NULL);
    writer = TB_ALLOCATOR_NEW(&resolved, tb_zip_writer);
    writer->sink = *sink;
    writer->allocator = resolved;
    writer->names = tb_set_create(&writer->allocator, 0, tb_set_string_hash, tb_set_string_equal);
    return writer;
}

void tb_zip_writer_destroy(tb_zip_writer *writer)
{
    tb_allocator allocator = {0};
    if (writer == NULL)
    {
        return;
    }
    allocator = writer->allocator;
    tb_set_destroy(writer->names);
    tb_deflater_destroy(writer->deflater);
    for (size_t index = 0; index < writer->count; index++)
    {
        tb_allocator_free(&allocator, writer->entries[index].name,
                          writer->entries[index].name_length + 1, 1);
    }
    tb_allocator_free(&allocator, writer->entries, writer->capacity * sizeof(tb_zip_written),
                      _Alignof(tb_zip_written));
    TB_ALLOCATOR_DELETE(&allocator, tb_zip_writer, writer);
}
