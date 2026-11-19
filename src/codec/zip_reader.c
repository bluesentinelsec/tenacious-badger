/* Reading ZIP archives: the central directory once, then entries at their offsets. */
#include "badger/zip.h"

#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/table.h"
#include "badger/utf8.h"
#include "zip_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Bytes of the central directory read at a time. */
#define TB_ZIP_DIRECTORY_BUFFER ((size_t)65536)

/* What a code page 437 byte from 0x80 up is in Unicode. */
static const uint16_t tb_zip_cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* One entry, as its central record has it. */
typedef struct tb_zip_record
{
    size_t name; /* Offset in `names` of the UTF-8 name, NUL after it. */
    size_t name_length;
    size_t raw; /* Offset in `names` of the name as stored, for the local header. */
    size_t raw_length;
    uint64_t local_offset;
    uint64_t limit; /* Where the next entry, or the central directory, begins. */
    uint64_t size;
    uint64_t compressed_size;
    int64_t modified;
    uint32_t crc32;
    uint32_t mode;
    uint16_t method;
    uint16_t flags;
    tb_zip_type type;
} tb_zip_record;

struct tb_zip_archive
{
    tb_stream source;
    tb_allocator allocator;
    uint64_t directory_offset;
    tb_zip_record *records;
    size_t count;
    char *names;
    size_t names_length;
    size_t names_capacity;
    tb_table *index; /* From a name to its record. */
};

/* -------------------------------------------------------------------------
 * Bytes from the source.
 * ------------------------------------------------------------------------- */

static tb_zip_result tb_zip_read_at(tb_zip_archive *archive, uint64_t offset, void *bytes,
                                    size_t length)
{
    const tb_stream_result result =
        tb_stream_read_exact_at(&archive->source, offset, bytes, length);
    if (result == TB_STREAM_END)
    {
        return TB_ZIP_MALFORMED;
    }
    return result == TB_STREAM_OK ? TB_ZIP_OK : TB_ZIP_INPUT_FAILED;
}

/* A window on the central directory, read a buffer at a time. */
typedef struct tb_zip_cursor
{
    tb_zip_archive *archive;
    uint64_t position;
    uint64_t end;
    unsigned char *buffer;
    size_t capacity;
    uint64_t buffer_start;
    size_t buffer_length;
} tb_zip_cursor;

/* Make the next `length` bytes available at `*bytes`, without moving. */
static tb_zip_result tb_zip_cursor_need(tb_zip_cursor *cursor, size_t length,
                                        const unsigned char **bytes)
{
    size_t want = 0;
    tb_zip_result result = TB_ZIP_OK;
    if (length > cursor->end - cursor->position)
    {
        return TB_ZIP_MALFORMED;
    }
    if (cursor->buffer != NULL && cursor->position >= cursor->buffer_start &&
        cursor->position - cursor->buffer_start + length <= cursor->buffer_length)
    {
        *bytes = cursor->buffer + (cursor->position - cursor->buffer_start);
        return TB_ZIP_OK;
    }
    if (length > cursor->capacity)
    {
        const size_t capacity = length > TB_ZIP_DIRECTORY_BUFFER ? length : TB_ZIP_DIRECTORY_BUFFER;
        cursor->buffer = tb_allocator_realloc(&cursor->archive->allocator, cursor->buffer,
                                              cursor->capacity, capacity, 1);
        cursor->capacity = capacity;
    }
    want = cursor->end - cursor->position < cursor->capacity
               ? (size_t)(cursor->end - cursor->position)
               : cursor->capacity;
    result = tb_zip_read_at(cursor->archive, cursor->position, cursor->buffer, want);
    if (result != TB_ZIP_OK)
    {
        cursor->buffer_length = 0;
        return result;
    }
    cursor->buffer_start = cursor->position;
    cursor->buffer_length = want;
    *bytes = cursor->buffer;
    return TB_ZIP_OK;
}

/* -------------------------------------------------------------------------
 * Names and times.
 * ------------------------------------------------------------------------- */

/* Append bytes and a NUL to `names`; return where they begin. */
static size_t tb_zip_names_append(tb_zip_archive *archive, const void *bytes, size_t length)
{
    const size_t start = archive->names_length;
    TB_ASSERT(length < SIZE_MAX - start - 1);
    if (archive->names_capacity - start < length + 1)
    {
        size_t capacity = archive->names_capacity == 0 ? 4096u : archive->names_capacity;
        while (capacity - start < length + 1)
        {
            TB_ASSERT(capacity <= SIZE_MAX / 2);
            capacity *= 2;
        }
        archive->names = tb_allocator_realloc(&archive->allocator, archive->names,
                                              archive->names_capacity, capacity, 1);
        archive->names_capacity = capacity;
    }
    if (length > 0)
    {
        memcpy(archive->names + start, bytes, length);
    }
    archive->names[start + length] = '\0';
    archive->names_length = start + length + 1;
    return start;
}

/* Append a name in code page 437 as UTF-8; return where it begins and its length. */
static size_t tb_zip_names_append_cp437(tb_zip_archive *archive, const unsigned char *bytes,
                                        size_t length, size_t *converted)
{
    /* Each byte becomes at most three. */
    char *const text = tb_allocator_alloc(&archive->allocator, length * 3 + 1, 1);
    size_t written = 0;
    size_t start = 0;
    for (size_t index = 0; index < length; index++)
    {
        const uint32_t scalar =
            bytes[index] < 0x80u ? bytes[index] : tb_zip_cp437[bytes[index] - 0x80u];
        written += tb_utf8_encode(scalar, text + written, length * 3 + 1 - written);
    }
    start = tb_zip_names_append(archive, text, written);
    tb_allocator_free(&archive->allocator, text, length * 3 + 1, 1);
    *converted = written;
    return start;
}

static bool tb_zip_is_ascii(const unsigned char *bytes, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        if (bytes[index] >= 0x80u)
        {
            return false;
        }
    }
    return true;
}

/* A DOS date and time, which have no zone, read as UTC. */
static int64_t tb_zip_dos_time(uint16_t date, uint16_t time)
{
    const unsigned month = (date >> 5) & 15u;
    const unsigned day = date & 31u;
    if (month < 1 || month > 12 || day < 1)
    {
        return TB_ZIP_DOS_FIRST;
    }
    return tb_zip_days_from_civil(1980 + (date >> 9), month, day) * 86400 +
           (int64_t)(time >> 11) * 3600 + (int64_t)((time >> 5) & 63u) * 60 +
           (int64_t)(time & 31u) * 2;
}

/* -------------------------------------------------------------------------
 * The central directory.
 * ------------------------------------------------------------------------- */

/* What the extra fields of a central record add to it. */
typedef struct tb_zip_extras
{
    bool has_time;
    int64_t time;
    const unsigned char *unicode_name; /* An Info-ZIP Unicode path whose check matched. */
    size_t unicode_length;
} tb_zip_extras;

/* Read the extra fields: ZIP64 values for the fields at their maximum, in
 * order; the extended timestamp; and the Unicode path. */
static tb_zip_result tb_zip_parse_extras(const unsigned char *extra, size_t length,
                                         const unsigned char *raw, size_t raw_length,
                                         tb_zip_record *record, uint32_t *disk,
                                         tb_zip_extras *extras)
{
    size_t at = 0;
    while (at < length)
    {
        uint16_t id = 0;
        size_t size = 0;
        const unsigned char *data = NULL;
        if (length - at < 4)
        {
            return TB_ZIP_MALFORMED;
        }
        id = tb_zip_get16(extra + at);
        size = tb_zip_get16(extra + at + 2);
        data = extra + at + 4;
        if (size > length - at - 4)
        {
            return TB_ZIP_MALFORMED;
        }
        if (id == TB_ZIP_EXTRA_ZIP64)
        {
            uint64_t *const fields[3] = {&record->size, &record->compressed_size,
                                         &record->local_offset};
            size_t used = 0;
            for (size_t field = 0; field < 3; field++)
            {
                if (*fields[field] != TB_ZIP_MAX_32)
                {
                    continue;
                }
                if (size - used < 8)
                {
                    return TB_ZIP_MALFORMED;
                }
                *fields[field] = tb_zip_get64(data + used);
                used += 8;
            }
            if (*disk == TB_ZIP_MAX_16)
            {
                if (size - used < 4)
                {
                    return TB_ZIP_MALFORMED;
                }
                *disk = tb_zip_get32(data + used);
            }
        }
        else if (id == TB_ZIP_EXTRA_TIMESTAMP && size >= 5 && (data[0] & 1u) != 0)
        {
            extras->has_time = true;
            extras->time = (int32_t)tb_zip_get32(data + 1);
        }
        else if (id == TB_ZIP_EXTRA_UNICODE_PATH && size > 5 && data[0] == 1 &&
                 tb_zip_get32(data + 1) == tb_crc32(raw, raw_length))
        {
            size_t error = 0;
            if (tb_utf8_validate((const char *)data + 5, size - 5, &error) &&
                memchr(data + 5, '\0', size - 5) == NULL)
            {
                extras->unicode_name = data + 5;
                extras->unicode_length = size - 5;
            }
        }
        at += 4 + size;
    }
    return TB_ZIP_OK;
}

/* The entry's name, as UTF-8, and its raw bytes, into `names`. */
static void tb_zip_store_names(tb_zip_archive *archive, tb_zip_record *record,
                               const unsigned char *raw, size_t raw_length,
                               const tb_zip_extras *extras)
{
    record->raw = tb_zip_names_append(archive, raw, raw_length);
    record->raw_length = raw_length;
    if (extras->unicode_name != NULL)
    {
        record->name = tb_zip_names_append(archive, extras->unicode_name, extras->unicode_length);
        record->name_length = extras->unicode_length;
    }
    else if ((record->flags & TB_ZIP_FLAG_UTF8) != 0 || tb_zip_is_ascii(raw, raw_length))
    {
        record->name = record->raw;
        record->name_length = raw_length;
    }
    else
    {
        record->name = tb_zip_names_append_cp437(archive, raw, raw_length, &record->name_length);
    }
}

/* What the entry is, from its name and its attributes; a directory's
 * trailing slashes are taken off its name. */
static tb_zip_result tb_zip_classify(tb_zip_archive *archive, tb_zip_record *record,
                                     uint16_t made_by, uint32_t external)
{
    char *name = archive->names + record->name;
    const uint32_t unix_mode = external >> 16;
    record->type = TB_ZIP_FILE;
    if ((made_by >> 8) == TB_ZIP_HOST_UNIX)
    {
        record->mode = unix_mode & TB_ZIP_UNIX_MODE_MASK;
        if ((unix_mode & TB_ZIP_UNIX_TYPE_MASK) == TB_ZIP_UNIX_DIRECTORY)
        {
            record->type = TB_ZIP_DIRECTORY;
        }
        else if ((unix_mode & TB_ZIP_UNIX_TYPE_MASK) == TB_ZIP_UNIX_SYMLINK)
        {
            record->type = TB_ZIP_SYMLINK;
        }
    }
    else if ((external & TB_ZIP_DOS_DIRECTORY) != 0)
    {
        record->type = TB_ZIP_DIRECTORY;
    }
    if (record->name_length > 0 && name[record->name_length - 1] == '/')
    {
        record->type = TB_ZIP_DIRECTORY;
        /* The raw name keeps its slash for the local header: the name gets its own copy. */
        if (record->name == record->raw)
        {
            record->name = tb_zip_names_append(archive, name, record->name_length);
            name = archive->names + record->name;
        }
    }
    while (record->name_length > 0 && name[record->name_length - 1] == '/')
    {
        record->name_length--;
        name[record->name_length] = '\0';
    }
    return record->name_length == 0 ? TB_ZIP_MALFORMED : TB_ZIP_OK;
}

/* Read the central record at the cursor into `record`, and move past it. */
static tb_zip_result tb_zip_read_record(tb_zip_cursor *cursor, tb_zip_record *record)
{
    tb_zip_archive *const archive = cursor->archive;
    const unsigned char *bytes = NULL;
    size_t name_length = 0;
    size_t extra_length = 0;
    size_t total = 0;
    uint32_t disk = 0;
    uint16_t made_by = 0;
    uint32_t external = 0;
    tb_zip_extras extras = {0};
    tb_zip_result result = tb_zip_cursor_need(cursor, TB_ZIP_CENTRAL_LENGTH, &bytes);
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    if (tb_zip_get32(bytes) != TB_ZIP_CENTRAL_SIGNATURE)
    {
        return TB_ZIP_MALFORMED;
    }
    name_length = tb_zip_get16(bytes + 28);
    extra_length = tb_zip_get16(bytes + 30);
    total = TB_ZIP_CENTRAL_LENGTH + name_length + extra_length + tb_zip_get16(bytes + 32);
    result = tb_zip_cursor_need(cursor, total, &bytes);
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    made_by = tb_zip_get16(bytes + 4);
    record->flags = tb_zip_get16(bytes + 8);
    record->method = tb_zip_get16(bytes + 10);
    record->modified = tb_zip_dos_time(tb_zip_get16(bytes + 14), tb_zip_get16(bytes + 12));
    record->crc32 = tb_zip_get32(bytes + 16);
    record->compressed_size = tb_zip_get32(bytes + 20);
    record->size = tb_zip_get32(bytes + 24);
    disk = tb_zip_get16(bytes + 34);
    external = tb_zip_get32(bytes + 38);
    record->local_offset = tb_zip_get32(bytes + 42);
    if (name_length == 0 || memchr(bytes + TB_ZIP_CENTRAL_LENGTH, '\0', name_length) != NULL)
    {
        return TB_ZIP_MALFORMED;
    }
    result =
        tb_zip_parse_extras(bytes + TB_ZIP_CENTRAL_LENGTH + name_length, extra_length,
                            bytes + TB_ZIP_CENTRAL_LENGTH, name_length, record, &disk, &extras);
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    if (disk != 0)
    {
        return TB_ZIP_UNSUPPORTED;
    }
    if (extras.has_time)
    {
        record->modified = extras.time;
    }
    tb_zip_store_names(archive, record, bytes + TB_ZIP_CENTRAL_LENGTH, name_length, &extras);
    cursor->position += total;
    return tb_zip_classify(archive, record, made_by, external);
}

/* What a record claims must be possible before anything is read for it. */
static tb_zip_result tb_zip_check_record(const tb_zip_archive *archive, const tb_zip_record *record)
{
    const bool encrypted = (record->flags & TB_ZIP_FLAG_ENCRYPTED) != 0;
    if (record->local_offset >= archive->directory_offset ||
        archive->directory_offset - record->local_offset < TB_ZIP_LOCAL_LENGTH)
    {
        return TB_ZIP_MALFORMED;
    }
    if (encrypted)
    {
        return TB_ZIP_OK;
    }
    if (record->method == TB_ZIP_STORED && record->size != record->compressed_size)
    {
        return TB_ZIP_MALFORMED;
    }
    if (record->method == TB_ZIP_DEFLATED &&
        record->compressed_size <= (UINT64_MAX - TB_ZIP_RATIO_SLACK) / TB_ZIP_MAX_RATIO &&
        record->size > record->compressed_size * TB_ZIP_MAX_RATIO + TB_ZIP_RATIO_SLACK)
    {
        return TB_ZIP_MALFORMED;
    }
    return TB_ZIP_OK;
}

static int tb_zip_compare_offsets(const void *left, const void *right)
{
    const tb_zip_record *const first = *(tb_zip_record *const *)left;
    const tb_zip_record *const second = *(tb_zip_record *const *)right;
    if (first->local_offset != second->local_offset)
    {
        return first->local_offset < second->local_offset ? -1 : 1;
    }
    return 0;
}

/* Each entry's data must end before the next entry begins, and the last
 * before the central directory: no two may share bytes. */
static tb_zip_result tb_zip_check_overlaps(tb_zip_archive *archive)
{
    tb_zip_record **order = NULL;
    tb_zip_result result = TB_ZIP_OK;
    if (archive->count == 0)
    {
        return TB_ZIP_OK;
    }
    order = TB_ALLOCATOR_NEW_ARRAY(&archive->allocator, tb_zip_record *, archive->count);
    for (size_t index = 0; index < archive->count; index++)
    {
        order[index] = &archive->records[index];
    }
    qsort(order, archive->count, sizeof(order[0]), tb_zip_compare_offsets);
    for (size_t index = 0; index < archive->count; index++)
    {
        tb_zip_record *const record = order[index];
        const uint64_t limit =
            index + 1 < archive->count ? order[index + 1]->local_offset : archive->directory_offset;
        const uint64_t room = limit - record->local_offset;
        record->limit = limit;
        if (room < TB_ZIP_LOCAL_LENGTH || room - TB_ZIP_LOCAL_LENGTH < record->raw_length ||
            room - TB_ZIP_LOCAL_LENGTH - record->raw_length < record->compressed_size)
        {
            result = TB_ZIP_MALFORMED;
            break;
        }
    }
    TB_ALLOCATOR_DELETE_ARRAY(&archive->allocator, tb_zip_record *, order, archive->count);
    return result;
}

static tb_zip_result tb_zip_build_index(tb_zip_archive *archive, bool allow_duplicates)
{
    archive->index = tb_table_create(&archive->allocator, archive->count, tb_table_string_hash,
                                     tb_table_string_equal);
    for (size_t index = 0; index < archive->count; index++)
    {
        tb_zip_record *const record = &archive->records[index];
        const char *const name = archive->names + record->name;
        if (tb_table_put(archive->index, name, record) != NULL && !allow_duplicates)
        {
            return TB_ZIP_DUPLICATE_NAME;
        }
    }
    return TB_ZIP_OK;
}

/* -------------------------------------------------------------------------
 * The end records.
 * ------------------------------------------------------------------------- */

typedef struct tb_zip_end
{
    uint64_t offset; /* Of the end record, or of the ZIP64 end record when there is one. */
    uint64_t entries;
    uint64_t directory_size;
    uint64_t directory_offset;
} tb_zip_end;

/* Find the end record: the last whose comment runs exactly to the end. */
static tb_zip_result tb_zip_find_end(tb_zip_archive *archive, uint64_t size, tb_zip_end *end)
{
    const size_t tail = size < TB_ZIP_END_LENGTH + TB_ZIP_MAX_FIELD
                            ? (size_t)size
                            : TB_ZIP_END_LENGTH + TB_ZIP_MAX_FIELD;
    unsigned char *const bytes = tb_allocator_alloc(&archive->allocator, tail, 1);
    tb_zip_result result = TB_ZIP_MALFORMED;
    if (tail >= TB_ZIP_END_LENGTH)
    {
        result = tb_zip_read_at(archive, size - tail, bytes, tail);
    }
    if (result == TB_ZIP_OK)
    {
        result = TB_ZIP_MALFORMED;
        for (size_t at = tail - TB_ZIP_END_LENGTH + 1; at > 0; at--)
        {
            const unsigned char *const record = bytes + at - 1;
            if (tb_zip_get32(record) == TB_ZIP_END_SIGNATURE &&
                at - 1 + TB_ZIP_END_LENGTH + tb_zip_get16(record + 20) == tail)
            {
                const bool one_disk = tb_zip_get16(record + 4) == 0 &&
                                      tb_zip_get16(record + 6) == 0 &&
                                      tb_zip_get16(record + 8) == tb_zip_get16(record + 10);
                end->offset = size - tail + (at - 1);
                end->entries = tb_zip_get16(record + 10);
                end->directory_size = tb_zip_get32(record + 12);
                end->directory_offset = tb_zip_get32(record + 16);
                result = one_disk ? TB_ZIP_OK : TB_ZIP_UNSUPPORTED;
                break;
            }
        }
    }
    tb_allocator_free(&archive->allocator, bytes, tail, 1);
    return result;
}

/* A ZIP64 locator just before the end record points at the ZIP64 end
 * record, whose values replace the end record's. */
static tb_zip_result tb_zip_read_end64(tb_zip_archive *archive, tb_zip_end *end)
{
    unsigned char locator[TB_ZIP_LOCATOR_LENGTH] = {0};
    unsigned char record[TB_ZIP_END64_LENGTH] = {0};
    uint64_t offset = 0;
    tb_zip_result result = TB_ZIP_OK;
    if (end->offset < TB_ZIP_LOCATOR_LENGTH)
    {
        return TB_ZIP_OK;
    }
    result = tb_zip_read_at(archive, end->offset - TB_ZIP_LOCATOR_LENGTH, locator, sizeof(locator));
    if (result != TB_ZIP_OK || tb_zip_get32(locator) != TB_ZIP_LOCATOR_SIGNATURE)
    {
        return result;
    }
    if (tb_zip_get32(locator + 4) != 0 || tb_zip_get32(locator + 16) != 1)
    {
        return TB_ZIP_UNSUPPORTED;
    }
    offset = tb_zip_get64(locator + 8);
    if (offset > end->offset - TB_ZIP_LOCATOR_LENGTH ||
        end->offset - TB_ZIP_LOCATOR_LENGTH - offset < TB_ZIP_END64_LENGTH)
    {
        return TB_ZIP_MALFORMED;
    }
    result = tb_zip_read_at(archive, offset, record, sizeof(record));
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    if (tb_zip_get32(record) != TB_ZIP_END64_SIGNATURE)
    {
        return TB_ZIP_MALFORMED;
    }
    if (tb_zip_get32(record + 16) != 0 || tb_zip_get32(record + 20) != 0 ||
        tb_zip_get64(record + 24) != tb_zip_get64(record + 32))
    {
        return TB_ZIP_UNSUPPORTED;
    }
    end->offset = offset;
    end->entries = tb_zip_get64(record + 32);
    end->directory_size = tb_zip_get64(record + 40);
    end->directory_offset = tb_zip_get64(record + 48);
    return TB_ZIP_OK;
}

/* -------------------------------------------------------------------------
 * Opening and closing.
 * ------------------------------------------------------------------------- */

static tb_zip_result tb_zip_read_directory(tb_zip_archive *archive, const tb_zip_end *end,
                                           size_t max_entries)
{
    tb_zip_cursor cursor = {0};
    tb_zip_result result = TB_ZIP_OK;
    if (end->directory_offset > end->offset ||
        end->directory_size > end->offset - end->directory_offset)
    {
        return TB_ZIP_MALFORMED;
    }
    if (end->entries > max_entries)
    {
        return TB_ZIP_TOO_LARGE;
    }
    /* Every record takes at least its fixed part. */
    if (end->entries > end->directory_size / TB_ZIP_CENTRAL_LENGTH)
    {
        return TB_ZIP_MALFORMED;
    }
    archive->directory_offset = end->directory_offset;
    archive->count = (size_t)end->entries;
    archive->records = TB_ALLOCATOR_NEW_ARRAY(&archive->allocator, tb_zip_record,
                                              archive->count == 0 ? 1 : archive->count);
    cursor.archive = archive;
    cursor.position = end->directory_offset;
    cursor.end = end->directory_offset + end->directory_size;
    for (size_t index = 0; index < archive->count && result == TB_ZIP_OK; index++)
    {
        result = tb_zip_read_record(&cursor, &archive->records[index]);
        if (result == TB_ZIP_OK)
        {
            result = tb_zip_check_record(archive, &archive->records[index]);
        }
    }
    if (result == TB_ZIP_OK && cursor.position != cursor.end)
    {
        result = TB_ZIP_MALFORMED;
    }
    tb_allocator_free(&archive->allocator, cursor.buffer, cursor.capacity, 1);
    return result;
}

tb_zip_result tb_zip_open(tb_stream *source, const tb_zip_options *options,
                          const tb_allocator *allocator, tb_zip_archive **archive)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_zip_archive *opened = NULL;
    tb_zip_end end = {0};
    uint64_t size = 0;
    tb_stream_result sized = TB_STREAM_OK;
    tb_zip_result result = TB_ZIP_OK;
    TB_ASSERT(source != NULL && source->functions != NULL);
    TB_ASSERT(archive != NULL);
    TB_ASSERT_MSG(options == NULL ||
                      (options->flags & ~(unsigned)TB_ZIP_ALLOW_DUPLICATE_NAMES) == 0,
                  "unknown ZIP reading flags");
    *archive = NULL;
    if (source->functions->read_bytes_at == NULL)
    {
        return TB_ZIP_UNSUPPORTED;
    }
    sized = tb_stream_size(source, &size);
    if (sized != TB_STREAM_OK)
    {
        return sized == TB_STREAM_UNSUPPORTED ? TB_ZIP_UNSUPPORTED : TB_ZIP_INPUT_FAILED;
    }
    opened = TB_ALLOCATOR_NEW(&resolved, tb_zip_archive);
    opened->source = *source;
    opened->allocator = resolved;
    result = tb_zip_find_end(opened, size, &end);
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_read_end64(opened, &end);
    }
    if (result == TB_ZIP_OK)
    {
        const size_t max_entries = options == NULL || options->max_entries == 0
                                       ? TB_ZIP_DEFAULT_MAX_ENTRIES
                                       : options->max_entries;
        result = tb_zip_read_directory(opened, &end, max_entries);
    }
    if (result == TB_ZIP_OK)
    {
        const bool allow = options != NULL && (options->flags & TB_ZIP_ALLOW_DUPLICATE_NAMES) != 0;
        result = tb_zip_build_index(opened, allow);
    }
    if (result == TB_ZIP_OK)
    {
        result = tb_zip_check_overlaps(opened);
    }
    if (result != TB_ZIP_OK)
    {
        tb_zip_close(opened);
        return result;
    }
    *archive = opened;
    return TB_ZIP_OK;
}

void tb_zip_close(tb_zip_archive *archive)
{
    tb_allocator allocator = {0};
    if (archive == NULL)
    {
        return;
    }
    allocator = archive->allocator;
    tb_table_destroy(archive->index);
    tb_allocator_free(&allocator, archive->names, archive->names_capacity, 1);
    if (archive->records != NULL)
    {
        TB_ALLOCATOR_DELETE_ARRAY(&allocator, tb_zip_record, archive->records,
                                  archive->count == 0 ? 1 : archive->count);
    }
    TB_ALLOCATOR_DELETE(&allocator, tb_zip_archive, archive);
}

size_t tb_zip_count(const tb_zip_archive *archive)
{
    TB_ASSERT(archive != NULL);
    return archive->count;
}

static void tb_zip_describe(const tb_zip_archive *archive, const tb_zip_record *record,
                            tb_zip_entry *entry)
{
    *entry = (tb_zip_entry){
        .name = archive->names + record->name,
        .name_length = record->name_length,
        .type = record->type,
        .method = record->method,
        .encrypted = (record->flags & TB_ZIP_FLAG_ENCRYPTED) != 0,
        .size = record->size,
        .compressed_size = record->compressed_size,
        .crc32 = record->crc32,
        .mode = record->mode,
        .modified = record->modified,
    };
}

void tb_zip_entry_at(const tb_zip_archive *archive, size_t index, tb_zip_entry *entry)
{
    TB_ASSERT(archive != NULL);
    TB_ASSERT(entry != NULL);
    TB_ASSERT_MSG(index < archive->count, "no entry at that index");
    tb_zip_describe(archive, &archive->records[index], entry);
}

bool tb_zip_find(const tb_zip_archive *archive, const char *name, size_t length, size_t *index)
{
    char stack[256] = {0};
    char *key = stack;
    const tb_zip_record *record = NULL;
    TB_ASSERT(archive != NULL);
    TB_ASSERT(name != NULL || length == 0);
    TB_ASSERT(index != NULL);
    /* The index is keyed by NUL-terminated names, which hold no NUL. */
    if (length == 0 || memchr(name, '\0', length) != NULL)
    {
        return false;
    }
    if (length >= sizeof(stack))
    {
        TB_ASSERT(length < SIZE_MAX);
        key = tb_allocator_alloc(&archive->allocator, length + 1, 1);
    }
    memcpy(key, name, length);
    key[length] = '\0';
    record = tb_table_get(archive->index, key);
    if (key != stack)
    {
        tb_allocator_free(&archive->allocator, key, length + 1, 1);
    }
    if (record == NULL)
    {
        return false;
    }
    *index = (size_t)(record - archive->records);
    return true;
}

/* -------------------------------------------------------------------------
 * Entry streams.
 * ------------------------------------------------------------------------- */

typedef struct tb_zip_entry_stream
{
    tb_stream range;    /* The compressed bytes. */
    tb_stream inflated; /* Over the range, for a deflated entry; closed otherwise. */
    tb_stream *data;    /* Whichever of the two gives the entry's bytes. */
    tb_allocator allocator;
    uint64_t remaining;
    uint32_t crc32;
    uint32_t expected_crc32;
    bool failed;
    bool verified;
} tb_zip_entry_stream;

/* At the end of the size: nothing may follow, and the CRC-32 must match. */
static bool tb_zip_entry_verify(tb_zip_entry_stream *stream)
{
    unsigned char extra = 0;
    size_t count = 0;
    const tb_stream_result result = tb_stream_read(stream->data, &extra, 1, &count);
    stream->verified = true;
    return result == TB_STREAM_OK && count == 0 && stream->crc32 == stream->expected_crc32;
}

static tb_stream_result tb_zip_entry_read(void *state, void *bytes, size_t capacity,
                                          size_t *transferred)
{
    tb_zip_entry_stream *const stream = state;
    size_t want = capacity;
    tb_stream_result result = TB_STREAM_OK;
    *transferred = 0;
    if (stream->failed)
    {
        return TB_STREAM_MALFORMED;
    }
    if (stream->remaining < want)
    {
        want = (size_t)stream->remaining;
    }
    if (want > 0)
    {
        result = tb_stream_read(stream->data, bytes, want, transferred);
        stream->crc32 = tb_crc32_update(stream->crc32, bytes, *transferred);
        stream->remaining -= *transferred;
        if (result == TB_STREAM_END || result == TB_STREAM_MALFORMED ||
            (result == TB_STREAM_OK && *transferred == 0))
        {
            stream->failed = true;
            return TB_STREAM_MALFORMED;
        }
        if (result != TB_STREAM_OK)
        {
            return result;
        }
    }
    if (stream->remaining == 0 && !stream->verified && !tb_zip_entry_verify(stream))
    {
        stream->failed = true;
        return TB_STREAM_MALFORMED;
    }
    return TB_STREAM_OK;
}

static tb_stream_result tb_zip_entry_release(void *state)
{
    tb_zip_entry_stream *const stream = state;
    const tb_allocator allocator = stream->allocator;
    if (stream->inflated.functions != NULL)
    {
        (void)tb_stream_close(&stream->inflated);
    }
    (void)tb_stream_close(&stream->range);
    TB_ALLOCATOR_DELETE(&allocator, tb_zip_entry_stream, stream);
    return TB_STREAM_OK;
}

static const tb_stream_functions tb_zip_entry_functions = {
    .read_bytes = tb_zip_entry_read,
    .release = tb_zip_entry_release,
};

/* Read the local header and find where the entry's data begins. */
static tb_zip_result tb_zip_locate_data(tb_zip_archive *archive, const tb_zip_record *record,
                                        uint64_t *data_offset)
{
    unsigned char header[TB_ZIP_LOCAL_LENGTH] = {0};
    char stack[256] = {0};
    char *name = stack;
    uint64_t room = 0;
    size_t name_length = 0;
    size_t extra_length = 0;
    uint16_t flags = 0;
    tb_zip_result result = tb_zip_read_at(archive, record->local_offset, header, sizeof(header));
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    name_length = tb_zip_get16(header + 26);
    extra_length = tb_zip_get16(header + 28);
    flags = tb_zip_get16(header + 6);
    if (tb_zip_get32(header) != TB_ZIP_LOCAL_SIGNATURE ||
        tb_zip_get16(header + 8) != record->method || name_length != record->raw_length)
    {
        return TB_ZIP_MALFORMED;
    }
    /* Without a data descriptor, the local values must be the central ones. */
    if ((flags & TB_ZIP_FLAG_DESCRIPTOR) == 0 &&
        (tb_zip_get32(header + 14) != record->crc32 ||
         (tb_zip_get32(header + 18) != TB_ZIP_MAX_32 &&
          tb_zip_get32(header + 18) != record->compressed_size) ||
         (tb_zip_get32(header + 22) != TB_ZIP_MAX_32 && tb_zip_get32(header + 22) != record->size)))
    {
        return TB_ZIP_MALFORMED;
    }
    if (name_length > sizeof(stack))
    {
        name = tb_allocator_alloc(&archive->allocator, name_length, 1);
    }
    result = tb_zip_read_at(archive, record->local_offset + TB_ZIP_LOCAL_LENGTH, name, name_length);
    if (result == TB_ZIP_OK && memcmp(name, archive->names + record->raw, name_length) != 0)
    {
        result = TB_ZIP_MALFORMED;
    }
    if (name != stack)
    {
        tb_allocator_free(&archive->allocator, name, name_length, 1);
    }
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    room = record->limit - record->local_offset - TB_ZIP_LOCAL_LENGTH - name_length;
    if (extra_length > room || room - extra_length < record->compressed_size)
    {
        return TB_ZIP_MALFORMED;
    }
    *data_offset = record->local_offset + TB_ZIP_LOCAL_LENGTH + name_length + extra_length;
    return TB_ZIP_OK;
}

tb_zip_result tb_zip_open_entry(tb_zip_archive *archive, size_t index,
                                const tb_allocator *allocator, tb_stream *stream)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    const uint16_t encryption =
        TB_ZIP_FLAG_ENCRYPTED | TB_ZIP_FLAG_STRONG_ENCRYPTION | TB_ZIP_FLAG_MASKED_HEADERS;
    const tb_zip_record *record = NULL;
    tb_zip_entry_stream *state = NULL;
    uint64_t data_offset = 0;
    tb_zip_result result = TB_ZIP_OK;
    TB_ASSERT(archive != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    TB_ASSERT_MSG(index < archive->count, "no entry at that index");
    record = &archive->records[index];
    if ((record->flags & encryption) != 0 ||
        (record->method != TB_ZIP_STORED && record->method != TB_ZIP_DEFLATED))
    {
        return TB_ZIP_UNSUPPORTED;
    }
    result = tb_zip_locate_data(archive, record, &data_offset);
    if (result != TB_ZIP_OK)
    {
        return result;
    }
    state = TB_ALLOCATOR_NEW(&resolved, tb_zip_entry_stream);
    state->allocator = resolved;
    state->remaining = record->size;
    state->expected_crc32 = record->crc32;
    if (tb_stream_open_range(&archive->source, data_offset, record->compressed_size, &resolved,
                             &state->range) != TB_STREAM_OK)
    {
        TB_ALLOCATOR_DELETE(&resolved, tb_zip_entry_stream, state);
        return TB_ZIP_MALFORMED;
    }
    state->data = &state->range;
    if (record->method == TB_ZIP_DEFLATED)
    {
        const tb_stream_result opened =
            tb_inflate_open_stream(&state->range, TB_DEFLATE_RAW, &resolved, &state->inflated);
        TB_ASSERT(opened == TB_STREAM_OK);
        state->data = &state->inflated;
    }
    stream->functions = &tb_zip_entry_functions;
    stream->state = state;
    return TB_ZIP_OK;
}
