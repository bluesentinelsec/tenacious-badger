/* Reading packages: the header, footer, and index once, then entries at their offsets. */
#include "package_internal.h"

#include "archive_internal.h"
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/sort.h"
#include "entry_stream.h"

#include <string.h>

typedef struct tb_package_record
{
    size_t name; /* Offset in `names`, with a NUL after it. */
    size_t name_length;
    tb_package_codec codec;
    uint32_t crc32;
    uint64_t offset;
    uint64_t stored_size;
    uint64_t size;
} tb_package_record;

struct tb_package
{
    tb_stream source;
    tb_allocator allocator;
    bool obfuscated;
    uint8_t key[TB_CHACHA20_KEY_SIZE]; /* The package's own. */
    /* For an obfuscated package, the source as it was before obfuscation.
     * It is opened once, with the package, because opening it tells the
     * source's position; entries then only read it at offsets, which many
     * threads may do at once. */
    tb_stream restored;
    tb_package_record *records; /* In the order of their names. */
    size_t count;
    char *names;
    size_t names_size;     /* Bytes used. */
    size_t names_capacity; /* Bytes allocated: the index's size, which bounds every name. */
};

static tb_package_result tb_package_read_at(tb_package *package, uint64_t offset, void *bytes,
                                            size_t length)
{
    const tb_stream_result result =
        tb_stream_read_exact_at(&package->source, offset, bytes, length);
    if (result == TB_STREAM_END)
    {
        return TB_PACKAGE_MALFORMED;
    }
    return result == TB_STREAM_OK ? TB_PACKAGE_OK : TB_PACKAGE_INPUT_FAILED;
}

/* The header: magic, a version this reader knows, known flags, and the key's check. */
static tb_package_result tb_package_read_header(tb_package *package, const uint8_t *key)
{
    unsigned char header[TB_PACKAGE_HEADER_SIZE] = {0};
    uint8_t check[TB_PACKAGE_CHECK_SIZE] = {0};
    uint16_t flags = 0;
    tb_package_result result = tb_package_read_at(package, 0, header, sizeof(header));
    if (result != TB_PACKAGE_OK)
    {
        return result;
    }
    if (memcmp(header, TB_PACKAGE_MAGIC, TB_PACKAGE_MAGIC_SIZE) != 0)
    {
        return TB_PACKAGE_MALFORMED;
    }
    flags = tb_package_get16(header + TB_PACKAGE_HEADER_FLAGS);
    if (tb_package_get16(header + TB_PACKAGE_HEADER_VERSION) != TB_PACKAGE_VERSION ||
        (flags & ~TB_PACKAGE_KNOWN_FLAGS) != 0)
    {
        return TB_PACKAGE_UNSUPPORTED;
    }
    if ((flags & TB_PACKAGE_FLAG_OBFUSCATED) == 0)
    {
        return TB_PACKAGE_OK;
    }
    if (key == NULL)
    {
        return TB_PACKAGE_KEY_REQUIRED;
    }
    package->obfuscated = true;
    tb_package_derive_key(key, header + TB_PACKAGE_HEADER_SALT, package->key, check);
    return memcmp(check, header + TB_PACKAGE_HEADER_CHECK, sizeof(check)) == 0
               ? TB_PACKAGE_OK
               : TB_PACKAGE_WRONG_KEY;
}

/* Check every record, keeping its name; names must be in strict order, which
 * also makes them unique. */
static tb_package_result tb_package_parse_index(tb_package *package, const unsigned char *index,
                                                size_t index_size, uint64_t index_offset)
{
    size_t at = 0;
    for (size_t entry = 0; entry < package->count; entry++)
    {
        tb_package_record *const record = &package->records[entry];
        const unsigned char *bytes = index + at;
        size_t name_length = 0;
        unsigned codec = 0;
        if (index_size - at < TB_PACKAGE_RECORD_SIZE)
        {
            return TB_PACKAGE_MALFORMED;
        }
        name_length = tb_package_get16(bytes + TB_PACKAGE_RECORD_NAME_LENGTH);
        codec = bytes[TB_PACKAGE_RECORD_CODEC];
        record->crc32 = tb_package_get32(bytes + TB_PACKAGE_RECORD_CRC);
        record->offset = tb_package_get64(bytes + TB_PACKAGE_RECORD_OFFSET);
        record->stored_size = tb_package_get64(bytes + TB_PACKAGE_RECORD_STORED);
        record->size = tb_package_get64(bytes + TB_PACKAGE_RECORD_SIZE_FIELD);
        if (index_size - at - TB_PACKAGE_RECORD_SIZE < name_length ||
            (codec != TB_PACKAGE_STORED && codec != TB_PACKAGE_DEFLATED))
        {
            return TB_PACKAGE_MALFORMED;
        }
        record->codec = (tb_package_codec)codec;
        bytes += TB_PACKAGE_RECORD_SIZE;
        if (!tb_archive_is_relative_path((const char *)bytes, name_length))
        {
            return TB_PACKAGE_MALFORMED;
        }
        /* The data lies between the header and the index. */
        if (record->offset < TB_PACKAGE_HEADER_SIZE || record->offset > index_offset ||
            record->stored_size > index_offset - record->offset)
        {
            return TB_PACKAGE_MALFORMED;
        }
        if (record->codec == TB_PACKAGE_STORED
                ? record->stored_size != record->size
                : record->stored_size == 0 ||
                      (record->stored_size <=
                           (UINT64_MAX - TB_PACKAGE_RATIO_SLACK) / TB_PACKAGE_MAX_RATIO &&
                       record->size >
                           record->stored_size * TB_PACKAGE_MAX_RATIO + TB_PACKAGE_RATIO_SLACK))
        {
            return TB_PACKAGE_MALFORMED;
        }
        record->name = package->names_size;
        memcpy(package->names + record->name, bytes, name_length);
        package->names[record->name + name_length] = '\0';
        record->name_length = name_length;
        package->names_size += name_length + 1;
        if (entry > 0)
        {
            const tb_package_record *const previous = &package->records[entry - 1];
            const size_t shorter =
                previous->name_length < name_length ? previous->name_length : name_length;
            const int order = memcmp(package->names + previous->name, bytes, shorter);
            if (order > 0 || (order == 0 && previous->name_length >= name_length))
            {
                return TB_PACKAGE_MALFORMED;
            }
        }
        at += TB_PACKAGE_RECORD_SIZE + name_length;
    }
    return at == index_size ? TB_PACKAGE_OK : TB_PACKAGE_MALFORMED;
}

static int tb_package_compare_offsets(const void *left, const void *right, void *context)
{
    const tb_package_record *const first = *(tb_package_record *const *)left;
    const tb_package_record *const second = *(tb_package_record *const *)right;
    (void)context;
    if (first->offset != second->offset)
    {
        return first->offset < second->offset ? -1 : 1;
    }
    /* An empty entry may share its offset with the next; it sorts first. */
    if (first->stored_size != second->stored_size)
    {
        return first->stored_size < second->stored_size ? -1 : 1;
    }
    return 0;
}

/* No two entries' data may share a byte. */
static tb_package_result tb_package_check_overlaps(tb_package *package)
{
    tb_package_record **order = NULL;
    tb_package_result result = TB_PACKAGE_OK;
    if (package->count < 2)
    {
        return TB_PACKAGE_OK;
    }
    order = TB_ALLOCATOR_NEW_ARRAY(&package->allocator, tb_package_record *, package->count);
    for (size_t index = 0; index < package->count; index++)
    {
        order[index] = &package->records[index];
    }
    tb_sort(order, package->count, sizeof(order[0]), tb_package_compare_offsets, NULL);
    for (size_t index = 1; index < package->count; index++)
    {
        const tb_package_record *const before = order[index - 1];
        if (order[index]->offset - before->offset < before->stored_size)
        {
            result = TB_PACKAGE_MALFORMED;
            break;
        }
    }
    TB_ALLOCATOR_DELETE_ARRAY(&package->allocator, tb_package_record *, order, package->count);
    return result;
}

/* The footer, then the index it points at, restored and checked. */
static tb_package_result tb_package_read_index(tb_package *package, uint64_t size,
                                               size_t max_entries)
{
    unsigned char footer[TB_PACKAGE_FOOTER_SIZE] = {0};
    uint64_t index_offset = 0;
    uint64_t index_size = 0;
    uint64_t count = 0;
    unsigned char *index = NULL;
    tb_package_result result =
        tb_package_read_at(package, size - TB_PACKAGE_FOOTER_SIZE, footer, sizeof(footer));
    if (result != TB_PACKAGE_OK)
    {
        return result;
    }
    index_offset = tb_package_get64(footer + TB_PACKAGE_FOOTER_INDEX_OFFSET);
    index_size = tb_package_get64(footer + TB_PACKAGE_FOOTER_INDEX_SIZE);
    count = tb_package_get64(footer + TB_PACKAGE_FOOTER_COUNT);
    if (memcmp(footer + TB_PACKAGE_FOOTER_MAGIC, TB_PACKAGE_END_MAGIC, TB_PACKAGE_MAGIC_SIZE) !=
            0 ||
        index_offset < TB_PACKAGE_HEADER_SIZE || index_offset > size - TB_PACKAGE_FOOTER_SIZE ||
        index_size != size - TB_PACKAGE_FOOTER_SIZE - index_offset ||
        count > index_size / TB_PACKAGE_RECORD_SIZE)
    {
        return TB_PACKAGE_MALFORMED;
    }
    if (count > max_entries)
    {
        return TB_PACKAGE_TOO_LARGE;
    }
    package->count = (size_t)count;
    package->records = TB_ALLOCATOR_NEW_ARRAY(&package->allocator, tb_package_record,
                                              package->count == 0 ? 1 : package->count);
    /* Every name and its NUL fit in the index's own size. */
    package->names_capacity = (size_t)index_size + 1;
    package->names = tb_allocator_alloc(&package->allocator, package->names_capacity, 1);
    index = tb_allocator_alloc(&package->allocator, (size_t)index_size + 1, 1);
    result = tb_package_read_at(package, index_offset, index, (size_t)index_size);
    if (result == TB_PACKAGE_OK && package->obfuscated)
    {
        tb_chacha20_xor(package->key, tb_package_nonce, index_offset, index, index,
                        (size_t)index_size);
    }
    if (result == TB_PACKAGE_OK && tb_crc32(index, (size_t)index_size) !=
                                       tb_package_get32(footer + TB_PACKAGE_FOOTER_INDEX_CRC))
    {
        result = TB_PACKAGE_MALFORMED;
    }
    if (result == TB_PACKAGE_OK)
    {
        result = tb_package_parse_index(package, index, (size_t)index_size, index_offset);
    }
    tb_allocator_free(&package->allocator, index, (size_t)index_size + 1, 1);
    return result;
}

tb_package_result tb_package_open(tb_stream *source, const uint8_t *key,
                                  const tb_package_options *options, const tb_allocator *allocator,
                                  tb_package **package)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_package *opened = NULL;
    uint64_t size = 0;
    tb_stream_result sized = TB_STREAM_OK;
    tb_package_result result = TB_PACKAGE_OK;
    TB_ASSERT(source != NULL && source->functions != NULL);
    TB_ASSERT(package != NULL);
    *package = NULL;
    if (source->functions->read_bytes_at == NULL)
    {
        return TB_PACKAGE_UNSUPPORTED;
    }
    sized = tb_stream_size(source, &size);
    if (sized != TB_STREAM_OK)
    {
        return sized == TB_STREAM_UNSUPPORTED ? TB_PACKAGE_UNSUPPORTED : TB_PACKAGE_INPUT_FAILED;
    }
    /* The data and the index may lie at offsets a range stream can reach. */
    if (size < TB_PACKAGE_HEADER_SIZE + TB_PACKAGE_FOOTER_SIZE || size > (uint64_t)INT64_MAX)
    {
        return TB_PACKAGE_MALFORMED;
    }
    opened = TB_ALLOCATOR_NEW(&resolved, tb_package);
    opened->source = *source;
    opened->allocator = resolved;
    result = tb_package_read_header(opened, key);
    if (result == TB_PACKAGE_OK)
    {
        const size_t max_entries = options == NULL || options->max_entries == 0
                                       ? TB_PACKAGE_DEFAULT_MAX_ENTRIES
                                       : options->max_entries;
        result = tb_package_read_index(opened, size, max_entries);
    }
    if (result == TB_PACKAGE_OK)
    {
        result = tb_package_check_overlaps(opened);
    }
    if (result == TB_PACKAGE_OK && opened->obfuscated)
    {
        const tb_stream_result restored = tb_chacha20_open_stream(
            &opened->source, opened->key, tb_package_nonce, &resolved, &opened->restored);
        if (restored != TB_STREAM_OK)
        {
            result = TB_PACKAGE_INPUT_FAILED;
        }
    }
    if (result != TB_PACKAGE_OK)
    {
        tb_package_close(opened);
        return result;
    }
    *package = opened;
    return TB_PACKAGE_OK;
}

void tb_package_close(tb_package *package)
{
    tb_allocator allocator = {0};
    if (package == NULL)
    {
        return;
    }
    allocator = package->allocator;
    if (package->records != NULL)
    {
        TB_ALLOCATOR_DELETE_ARRAY(&allocator, tb_package_record, package->records,
                                  package->count == 0 ? 1 : package->count);
    }
    if (package->names != NULL)
    {
        tb_allocator_free(&allocator, package->names, package->names_capacity, 1);
    }
    if (package->restored.functions != NULL)
    {
        (void)tb_stream_close(&package->restored);
    }
    tb_package_wipe(package->key, sizeof(package->key));
    TB_ALLOCATOR_DELETE(&allocator, tb_package, package);
}

size_t tb_package_count(const tb_package *package)
{
    TB_ASSERT(package != NULL);
    return package->count;
}

void tb_package_entry_at(const tb_package *package, size_t index, tb_package_entry *entry)
{
    const tb_package_record *record = NULL;
    TB_ASSERT(package != NULL);
    TB_ASSERT(entry != NULL);
    TB_ASSERT_MSG(index < package->count, "no entry at that index");
    record = &package->records[index];
    *entry = (tb_package_entry){
        .name = package->names + record->name,
        .name_length = record->name_length,
        .codec = record->codec,
        .size = record->size,
        .stored_size = record->stored_size,
        .crc32 = record->crc32,
    };
}

bool tb_package_find(const tb_package *package, const char *name, size_t length, size_t *index)
{
    size_t low = 0;
    size_t high = 0;
    TB_ASSERT(package != NULL);
    TB_ASSERT(name != NULL || length == 0);
    TB_ASSERT(index != NULL);
    /* No entry has an empty name. */
    if (name == NULL || length == 0)
    {
        return false;
    }
    high = package->count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const tb_package_record *const record = &package->records[middle];
        const size_t shorter = record->name_length < length ? record->name_length : length;
        int order = memcmp(package->names + record->name, name, shorter);
        if (order == 0)
        {
            order = record->name_length < length ? -1 : (record->name_length > length);
        }
        if (order == 0)
        {
            *index = middle;
            return true;
        }
        if (order < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return false;
}

void tb_package_open_entry(tb_package *package, size_t index, const tb_allocator *allocator,
                           tb_stream *stream)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    const tb_package_record *record = NULL;
    tb_entry_layers layers = {0};
    tb_stream_result opened = TB_STREAM_OK;
    TB_ASSERT(package != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    TB_ASSERT_MSG(index < package->count, "no entry at that index");
    record = &package->records[index];
    /* Obfuscated bytes are restored by their offset in the package, so the
     * range is taken from the restored source, never the other way around. */
    opened = tb_stream_open_range(package->obfuscated ? &package->restored : &package->source,
                                  record->offset, record->stored_size, &resolved,
                                  &layers.layers[layers.count]);
    TB_ASSERT(opened == TB_STREAM_OK);
    layers.count++;
    if (record->codec == TB_PACKAGE_DEFLATED)
    {
        opened = tb_inflate_open_stream(&layers.layers[layers.count - 1], TB_DEFLATE_RAW, &resolved,
                                        &layers.layers[layers.count]);
        TB_ASSERT(opened == TB_STREAM_OK);
        layers.count++;
    }
    tb_entry_stream_open(&layers, record->size, record->crc32, &resolved, stream);
}
