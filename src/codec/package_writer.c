/* Writing packages: the header, each entry whole, then the index and footer. */
#include "package_internal.h"

#include "archive_internal.h"
#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/random.h"
#include "badger/set.h"
#include "badger/sort.h"

#include <string.h>

/* Bytes obfuscated at a time on their way to the sink. */
#define TB_PACKAGE_WRITE_BUFFER ((size_t)4096)

typedef struct tb_package_written
{
    char *name; /* NUL after it. */
    size_t name_length;
    tb_package_codec codec;
    uint32_t crc32;
    uint64_t offset;
    uint64_t stored_size;
    uint64_t size;
} tb_package_written;

struct tb_package_writer
{
    tb_stream sink;
    tb_allocator allocator;
    bool obfuscated;
    uint8_t key[TB_CHACHA20_KEY_SIZE]; /* The package's own, derived from the game's. */
    uint64_t position;                 /* Bytes written. */
    tb_package_written *entries;
    size_t count;
    size_t capacity;
    tb_set *names;
    bool failed;
    bool finished;
};

/* Write bytes at the position: obfuscated by their offset in the package, or
 * as they are; or, with `plain`, as they are in either case (the footer). */
static tb_package_result tb_package_emit(tb_package_writer *writer, const void *bytes,
                                         size_t length, bool plain)
{
    const unsigned char *input = bytes;
    unsigned char buffer[TB_PACKAGE_WRITE_BUFFER] = {0};
    if (writer->failed)
    {
        return TB_PACKAGE_OUTPUT_FAILED;
    }
    while (length > 0)
    {
        const size_t piece = length < sizeof(buffer) ? length : sizeof(buffer);
        const void *output = input;
        if (writer->obfuscated && !plain)
        {
            tb_chacha20_xor(writer->key, tb_package_nonce, writer->position, input, buffer, piece);
            output = buffer;
        }
        if (tb_stream_write_all(&writer->sink, output, piece) != TB_STREAM_OK)
        {
            writer->failed = true;
            return TB_PACKAGE_OUTPUT_FAILED;
        }
        writer->position += piece;
        input += piece;
        length -= piece;
    }
    return TB_PACKAGE_OK;
}

static tb_package_result tb_package_align(tb_package_writer *writer)
{
    static const unsigned char zeros[TB_PACKAGE_ALIGNMENT] = {0};
    const size_t padding =
        (size_t)((TB_PACKAGE_ALIGNMENT - writer->position % TB_PACKAGE_ALIGNMENT) %
                 TB_PACKAGE_ALIGNMENT);
    return tb_package_emit(writer, zeros, padding, false);
}

tb_package_result tb_package_writer_create(tb_stream *sink, const uint8_t *key,
                                           const tb_allocator *allocator,
                                           tb_package_writer **writer)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    unsigned char header[TB_PACKAGE_HEADER_SIZE] = {0};
    tb_package_writer *created = NULL;
    tb_package_result result = TB_PACKAGE_OK;
    TB_ASSERT(sink != NULL && sink->functions != NULL);
    TB_ASSERT(writer != NULL);
    *writer = NULL;
    memcpy(header, TB_PACKAGE_MAGIC, TB_PACKAGE_MAGIC_SIZE);
    tb_package_put16(header + TB_PACKAGE_HEADER_VERSION, TB_PACKAGE_VERSION);
    if (key != NULL)
    {
        if (!tb_random_bytes(header + TB_PACKAGE_HEADER_SALT, TB_PACKAGE_SALT_SIZE))
        {
            return TB_PACKAGE_RANDOM_FAILED;
        }
        tb_package_put16(header + TB_PACKAGE_HEADER_FLAGS, TB_PACKAGE_FLAG_OBFUSCATED);
    }
    created = TB_ALLOCATOR_NEW(&resolved, tb_package_writer);
    created->sink = *sink;
    created->allocator = resolved;
    created->names = tb_set_create(&created->allocator, 0, tb_set_string_hash, tb_set_string_equal);
    if (key != NULL)
    {
        created->obfuscated = true;
        tb_package_derive_key(key, header + TB_PACKAGE_HEADER_SALT, created->key,
                              header + TB_PACKAGE_HEADER_CHECK);
    }
    /* The header is never obfuscated: it says how to read the rest. */
    result = tb_package_emit(created, header, sizeof(header), true);
    if (result != TB_PACKAGE_OK)
    {
        tb_package_writer_destroy(created);
        return result;
    }
    *writer = created;
    return TB_PACKAGE_OK;
}

void tb_package_writer_destroy(tb_package_writer *writer)
{
    tb_allocator allocator = {0};
    if (writer == NULL)
    {
        return;
    }
    allocator = writer->allocator;
    tb_set_destroy(writer->names);
    for (size_t index = 0; index < writer->count; index++)
    {
        tb_allocator_free(&allocator, writer->entries[index].name,
                          writer->entries[index].name_length + 1, 1);
    }
    tb_allocator_free(&allocator, writer->entries, writer->capacity * sizeof(tb_package_written),
                      _Alignof(tb_package_written));
    tb_package_wipe(writer->key, sizeof(writer->key));
    TB_ALLOCATOR_DELETE(&allocator, tb_package_writer, writer);
}

/* Keep an entry for the index, with a copy of its name; NULL if the name is taken. */
static tb_package_written *tb_package_remember(tb_package_writer *writer, const char *name,
                                               size_t name_length)
{
    tb_package_written *written = NULL;
    char *copy = tb_allocator_alloc(&writer->allocator, name_length + 1, 1);
    memcpy(copy, name, name_length);
    copy[name_length] = '\0';
    if (!tb_set_put(writer->names, copy))
    {
        tb_allocator_free(&writer->allocator, copy, name_length + 1, 1);
        return NULL;
    }
    if (writer->count == writer->capacity)
    {
        const size_t capacity = writer->capacity == 0 ? 64u : writer->capacity * 2;
        TB_ASSERT(writer->capacity <= SIZE_MAX / 2 / sizeof(tb_package_written));
        writer->entries = tb_allocator_realloc(
            &writer->allocator, writer->entries, writer->capacity * sizeof(tb_package_written),
            capacity * sizeof(tb_package_written), _Alignof(tb_package_written));
        writer->capacity = capacity;
    }
    written = &writer->entries[writer->count];
    *written = (tb_package_written){.name = copy, .name_length = name_length};
    writer->count++;
    return written;
}

tb_package_result tb_package_add(tb_package_writer *writer, const char *name, size_t name_length,
                                 const void *bytes, size_t size, int level)
{
    tb_package_written *written = NULL;
    unsigned char *compressed = NULL;
    size_t bound = 0;
    size_t compressed_size = 0;
    const void *stored = bytes;
    tb_package_result result = TB_PACKAGE_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT(bytes != NULL || size == 0);
    TB_ASSERT_MSG(!writer->finished, "the package is finished");
    if (!tb_archive_is_relative_path(name, name_length) || name_length > TB_PACKAGE_MAX_NAME ||
        level < 0 || level > 9)
    {
        return TB_PACKAGE_INVALID;
    }
    written = tb_package_remember(writer, name, name_length);
    if (written == NULL)
    {
        return TB_PACKAGE_DUPLICATE_NAME;
    }
    written->size = size;
    written->crc32 = tb_crc32(bytes, size);
    written->codec = TB_PACKAGE_STORED;
    written->stored_size = size;
    if (level > 0 && size > 0)
    {
        bound = tb_deflate_bound(size);
        compressed = tb_allocator_alloc(&writer->allocator, bound, 1);
        if (tb_deflate(TB_DEFLATE_RAW, level, bytes, size, compressed, bound, &compressed_size) ==
                TB_DEFLATE_OK &&
            compressed_size < size)
        {
            written->codec = TB_PACKAGE_DEFLATED;
            written->stored_size = compressed_size;
            stored = compressed;
        }
    }
    result = tb_package_align(writer);
    written->offset = writer->position;
    if (result == TB_PACKAGE_OK)
    {
        result = tb_package_emit(writer, stored, (size_t)written->stored_size, false);
    }
    tb_allocator_free(&writer->allocator, compressed, bound, 1);
    return result;
}

static int tb_package_compare_names(const void *left, const void *right, void *context)
{
    const tb_package_written *const first = left;
    const tb_package_written *const second = right;
    const size_t shorter =
        first->name_length < second->name_length ? first->name_length : second->name_length;
    const int order = memcmp(first->name, second->name, shorter);
    (void)context;
    if (order != 0)
    {
        return order;
    }
    return first->name_length < second->name_length ? -1
                                                    : (first->name_length > second->name_length);
}

tb_package_result tb_package_writer_finish(tb_package_writer *writer)
{
    unsigned char footer[TB_PACKAGE_FOOTER_SIZE] = {0};
    unsigned char *index = NULL;
    size_t index_size = 0;
    size_t at = 0;
    uint64_t index_offset = 0;
    tb_package_result result = TB_PACKAGE_OK;
    TB_ASSERT(writer != NULL);
    TB_ASSERT_MSG(!writer->finished, "the package is finished");
    writer->finished = true;
    if (writer->count > 0)
    {
        tb_sort(writer->entries, writer->count, sizeof(writer->entries[0]),
                tb_package_compare_names, NULL);
    }
    for (size_t entry = 0; entry < writer->count; entry++)
    {
        index_size += TB_PACKAGE_RECORD_SIZE + writer->entries[entry].name_length;
    }
    index = tb_allocator_alloc(&writer->allocator, index_size == 0 ? 1 : index_size, 1);
    for (size_t entry = 0; entry < writer->count; entry++)
    {
        const tb_package_written *const written = &writer->entries[entry];
        unsigned char *const record = index + at;
        memset(record, 0, TB_PACKAGE_RECORD_SIZE);
        tb_package_put16(record + TB_PACKAGE_RECORD_NAME_LENGTH, (uint32_t)written->name_length);
        record[TB_PACKAGE_RECORD_CODEC] = (unsigned char)written->codec;
        tb_package_put32(record + TB_PACKAGE_RECORD_CRC, written->crc32);
        tb_package_put64(record + TB_PACKAGE_RECORD_OFFSET, written->offset);
        tb_package_put64(record + TB_PACKAGE_RECORD_STORED, written->stored_size);
        tb_package_put64(record + TB_PACKAGE_RECORD_SIZE_FIELD, written->size);
        memcpy(record + TB_PACKAGE_RECORD_SIZE, written->name, written->name_length);
        at += TB_PACKAGE_RECORD_SIZE + written->name_length;
    }
    index_offset = writer->position;
    tb_package_put64(footer + TB_PACKAGE_FOOTER_INDEX_OFFSET, index_offset);
    tb_package_put64(footer + TB_PACKAGE_FOOTER_INDEX_SIZE, index_size);
    tb_package_put64(footer + TB_PACKAGE_FOOTER_COUNT, writer->count);
    tb_package_put32(footer + TB_PACKAGE_FOOTER_INDEX_CRC, tb_crc32(index, index_size));
    memcpy(footer + TB_PACKAGE_FOOTER_MAGIC, TB_PACKAGE_END_MAGIC, TB_PACKAGE_MAGIC_SIZE);
    result = tb_package_emit(writer, index, index_size, false);
    tb_allocator_free(&writer->allocator, index, index_size == 0 ? 1 : index_size, 1);
    if (result == TB_PACKAGE_OK)
    {
        result = tb_package_emit(writer, footer, sizeof(footer), true);
    }
    if (result == TB_PACKAGE_OK && tb_stream_flush(&writer->sink) != TB_STREAM_OK)
    {
        writer->failed = true;
    }
    return writer->failed ? TB_PACKAGE_OUTPUT_FAILED : TB_PACKAGE_OK;
}
