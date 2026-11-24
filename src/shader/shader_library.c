/* Shader libraries: reading one whole and checked, finding a key in it, and
 * writing one. The format is docs/shader-cache.md's; shader_bytes.h reads
 * and writes its numbers. */
#include "shader_bytes.h"

#include "badger/arena.h"
#include "badger/checksum.h"
#include "badger/memory.h"
#include "badger/shader_cache.h"
#include "badger/shader_library.h"
#include "badger/sort.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include <string.h>

/* The header. */
static const uint8_t library_magic[4] = {'T', 'B', 'S', 'L'};
#define LIBRARY_VERSION 1u
#define HEADER_MAGIC_OFFSET ((size_t)0)
#define HEADER_VERSION_OFFSET ((size_t)4)
#define HEADER_COUNT_OFFSET ((size_t)8)
#define HEADER_COMPILER_LENGTH_OFFSET ((size_t)12)
#define HEADER_FILE_SIZE_OFFSET ((size_t)16)
#define HEADER_CHECKSUM_OFFSET ((size_t)24)
#define HEADER_RESERVED_OFFSET ((size_t)28)
#define HEADER_SIZE ((size_t)32)

/* An entry of the table. */
#define ENTRY_KEY_OFFSET ((size_t)0)
#define ENTRY_CODE_OFFSET ((size_t)32)
#define ENTRY_SIZE_OFFSET ((size_t)40)
#define ENTRY_CHECKSUM_OFFSET ((size_t)44)
#define ENTRY_SIZE ((size_t)48)

/* The compiler's name, and each code, start at a multiple of this. */
#define ALIGNMENT ((size_t)8)

struct tb_shader_library
{
    uint8_t *bytes; /* The library, from tb_aligned_alloc(). */
    size_t size;
    const uint8_t *table; /* Of `count` entries, in the order of their keys. */
    size_t count;
    char *compiler;
};

typedef struct writer_entry
{
    tb_shader_key key;
    uint8_t *code;
    size_t size;
} writer_entry;

struct tb_shader_library_writer
{
    writer_entry *entries;
    size_t count;
    size_t capacity;
};

static size_t align_up(size_t value)
{
    return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/* Whether bytes are all zero, as padding is. */
static bool is_zero(const uint8_t *bytes, size_t length)
{
    for (size_t index = 0; index < length; index++)
    {
        if (bytes[index] != 0)
        {
            return false;
        }
    }
    return true;
}

/* Check the header; on OK, where the table starts, and its entries. */
static tb_shader_library_result check_header(const uint8_t *bytes, size_t size,
                                             size_t *table_offset, size_t *count)
{
    if (size < HEADER_SIZE || memcmp(bytes, library_magic, sizeof(library_magic)) != 0)
    {
        return TB_SHADER_LIBRARY_DAMAGED;
    }
    if (tb_shader_load_u32(bytes + HEADER_VERSION_OFFSET) != LIBRARY_VERSION)
    {
        return TB_SHADER_LIBRARY_VERSION;
    }
    const size_t entries = tb_shader_load_u32(bytes + HEADER_COUNT_OFFSET);
    const size_t compiler_length = tb_shader_load_u32(bytes + HEADER_COMPILER_LENGTH_OFFSET);
    if (tb_shader_load_u64(bytes + HEADER_FILE_SIZE_OFFSET) != (uint64_t)size ||
        tb_shader_load_u32(bytes + HEADER_RESERVED_OFFSET) != 0 ||
        compiler_length > TB_SHADER_LIBRARY_MAX_COMPILER)
    {
        return TB_SHADER_LIBRARY_DAMAGED;
    }
    const size_t offset = HEADER_SIZE + align_up(compiler_length);
    if (offset > size || entries > (size - offset) / ENTRY_SIZE ||
        memchr(bytes + HEADER_SIZE, '\0', compiler_length) != NULL ||
        !is_zero(bytes + HEADER_SIZE + compiler_length, offset - HEADER_SIZE - compiler_length))
    {
        return TB_SHADER_LIBRARY_DAMAGED;
    }
    const size_t table_end = offset + entries * ENTRY_SIZE;
    if (tb_crc32(bytes + HEADER_SIZE, table_end - HEADER_SIZE) !=
        tb_shader_load_u32(bytes + HEADER_CHECKSUM_OFFSET))
    {
        return TB_SHADER_LIBRARY_DAMAGED;
    }
    *table_offset = offset;
    *count = entries;
    return TB_SHADER_LIBRARY_OK;
}

/* Check every entry: keys in ascending order, each once; code at the first
 * multiple of ALIGNMENT after the table or the code before it, the padding
 * between them zero, within the file, and as its checksum says; and the
 * file ending where the last code does. So no byte of a library goes
 * unchecked. */
static bool check_entries(const uint8_t *bytes, size_t size, size_t table_offset, size_t count)
{
    size_t next_free = table_offset + count * ENTRY_SIZE;
    for (size_t index = 0; index < count; index++)
    {
        const uint8_t *const entry = bytes + table_offset + index * ENTRY_SIZE;
        const uint64_t offset = tb_shader_load_u64(entry + ENTRY_CODE_OFFSET);
        const size_t code_size = tb_shader_load_u32(entry + ENTRY_SIZE_OFFSET);
        if (index > 0 && memcmp(entry - ENTRY_SIZE, entry, TB_SHADER_KEY_SIZE) >= 0)
        {
            return false;
        }
        if (code_size == 0 || offset != (uint64_t)align_up(next_free) || offset > (uint64_t)size ||
            code_size > size - (size_t)offset ||
            !is_zero(bytes + next_free, (size_t)offset - next_free))
        {
            return false;
        }
        if (tb_crc32(bytes + offset, code_size) !=
            tb_shader_load_u32(entry + ENTRY_CHECKSUM_OFFSET))
        {
            return false;
        }
        next_free = (size_t)offset + code_size;
    }
    return next_free == size;
}

/* Open a library from bytes of tb_aligned_alloc(), which it takes. */
static tb_shader_library_result open_owned(uint8_t *bytes, size_t size, tb_shader_library **library)
{
    size_t table_offset = 0;
    size_t count = 0;
    *library = NULL;
    tb_shader_library_result result = check_header(bytes, size, &table_offset, &count);
    if (result == TB_SHADER_LIBRARY_OK && !check_entries(bytes, size, table_offset, count))
    {
        result = TB_SHADER_LIBRARY_DAMAGED;
    }
    if (result != TB_SHADER_LIBRARY_OK)
    {
        tb_aligned_free(bytes);
        return result;
    }
    const size_t compiler_length = tb_shader_load_u32(bytes + HEADER_COMPILER_LENGTH_OFFSET);
    tb_shader_library *const opened = tb_calloc(1, sizeof(*opened));
    opened->bytes = bytes;
    opened->size = size;
    opened->table = bytes + table_offset;
    opened->count = count;
    opened->compiler = tb_malloc(compiler_length + 1);
    memcpy(opened->compiler, bytes + HEADER_SIZE, compiler_length);
    opened->compiler[compiler_length] = '\0';
    *library = opened;
    return TB_SHADER_LIBRARY_OK;
}

tb_shader_library_result tb_shader_library_open(const void *bytes, size_t size,
                                                tb_shader_library **library)
{
    uint8_t *const copy = tb_aligned_alloc(ALIGNMENT, size);
    if (size > 0)
    {
        memcpy(copy, bytes, size);
    }
    return open_owned(copy, size, library);
}

tb_shader_library_result tb_shader_library_load(struct tb_vfs *vfs, const char *name, size_t length,
                                                tb_shader_library **library)
{
    tb_vfs_info info = {0};
    tb_stream stream = {0};
    *library = NULL;
    const tb_vfs_result queried = tb_vfs_query(vfs, name, length, &info);
    if (queried == TB_VFS_NOT_FOUND || (queried == TB_VFS_OK && info.kind != TB_VFS_FILE))
    {
        return TB_SHADER_LIBRARY_NOT_FOUND;
    }
    if (queried != TB_VFS_OK || info.size > (uint64_t)TB_SHADER_LIBRARY_MAX_SIZE ||
        tb_vfs_open(vfs, name, length, NULL, &stream) != TB_VFS_OK)
    {
        return TB_SHADER_LIBRARY_READ_FAILED;
    }
    const size_t size = (size_t)info.size;
    uint8_t *const bytes = tb_aligned_alloc(ALIGNMENT, size);
    const tb_stream_result read =
        size == 0 ? TB_STREAM_OK : tb_stream_read_exact(&stream, bytes, size);
    (void)tb_stream_close(&stream);
    if (read != TB_STREAM_OK)
    {
        /* coverage-off: a file the VFS told the size of reads whole */
        tb_aligned_free(bytes);
        return TB_SHADER_LIBRARY_READ_FAILED;
        /* coverage-on */
    }
    return open_owned(bytes, size, library);
}

void tb_shader_library_close(tb_shader_library *library)
{
    if (library == NULL)
    {
        return;
    }
    tb_aligned_free(library->bytes);
    tb_free(library->compiler);
    tb_free(library);
}

bool tb_shader_library_find(const tb_shader_library *library, const tb_shader_key *key,
                            const void **code, size_t *size)
{
    *code = NULL;
    *size = 0;
    size_t low = 0;
    size_t high = library->count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const uint8_t *const entry = library->table + middle * ENTRY_SIZE;
        const int order = memcmp(key->bytes, entry + ENTRY_KEY_OFFSET, TB_SHADER_KEY_SIZE);
        if (order == 0)
        {
            *code = library->bytes + tb_shader_load_u64(entry + ENTRY_CODE_OFFSET);
            *size = tb_shader_load_u32(entry + ENTRY_SIZE_OFFSET);
            return true;
        }
        if (order < 0)
        {
            high = middle;
        }
        else
        {
            low = middle + 1;
        }
    }
    return false;
}

size_t tb_shader_library_count(const tb_shader_library *library)
{
    return library->count;
}

const char *tb_shader_library_compiler(const tb_shader_library *library)
{
    return library->compiler;
}

const char *tb_shader_library_result_name(tb_shader_library_result result)
{
    switch (result)
    {
    case TB_SHADER_LIBRARY_OK:
        return "ok";
    case TB_SHADER_LIBRARY_NOT_FOUND:
        return "not found";
    case TB_SHADER_LIBRARY_READ_FAILED:
        return "read failed";
    case TB_SHADER_LIBRARY_DAMAGED:
        return "damaged";
    case TB_SHADER_LIBRARY_VERSION:
        return "another version";
    }
    return "unknown result";
}

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

tb_shader_library_writer *tb_shader_library_writer_create(void)
{
    return tb_calloc(1, sizeof(tb_shader_library_writer));
}

void tb_shader_library_writer_destroy(tb_shader_library_writer *writer)
{
    if (writer == NULL)
    {
        return;
    }
    for (size_t index = 0; index < writer->count; index++)
    {
        tb_free(writer->entries[index].code);
    }
    tb_free(writer->entries);
    tb_free(writer);
}

bool tb_shader_library_writer_add(tb_shader_library_writer *writer, const tb_shader_key *key,
                                  const void *code, size_t size)
{
    if (size == 0 || size > TB_SHADER_CACHE_MAX_CODE)
    {
        return false;
    }
    for (size_t index = 0; index < writer->count; index++)
    {
        const writer_entry *const entry = &writer->entries[index];
        if (memcmp(entry->key.bytes, key->bytes, TB_SHADER_KEY_SIZE) == 0)
        {
            return entry->size == size && memcmp(entry->code, code, size) == 0;
        }
    }
    if (writer->count == writer->capacity)
    {
        writer->capacity = writer->capacity == 0 ? 16 : writer->capacity * 2;
        writer->entries =
            tb_realloc_array(writer->entries, writer->capacity, sizeof(writer->entries[0]));
    }
    writer_entry *const entry = &writer->entries[writer->count];
    entry->key = *key;
    entry->code = tb_malloc(size);
    memcpy(entry->code, code, size);
    entry->size = size;
    writer->count++;
    return true;
}

static int compare_entries(const void *left, const void *right, void *context)
{
    const writer_entry *const a = left;
    const writer_entry *const b = right;
    (void)context;
    return memcmp(a->key.bytes, b->key.bytes, TB_SHADER_KEY_SIZE);
}

/* The library's size, from the end of its table on; false when it is over
 * TB_SHADER_LIBRARY_MAX_SIZE. */
static bool measure(const tb_shader_library_writer *writer, size_t table_end, size_t *size)
{
    size_t end = table_end;
    for (size_t index = 0; index < writer->count; index++)
    {
        end = align_up(end);
        if (writer->entries[index].size > TB_SHADER_LIBRARY_MAX_SIZE - end)
        {
            return false;
        }
        end += writer->entries[index].size;
    }
    *size = end;
    return true;
}

bool tb_shader_library_writer_finish(tb_shader_library_writer *writer, const char *compiler,
                                     tb_arena *arena, const void **bytes, size_t *size)
{
    const size_t compiler_length = tb_str_length(compiler);
    const size_t table_offset = HEADER_SIZE + align_up(compiler_length);
    size_t total = 0;
    if (compiler_length > TB_SHADER_LIBRARY_MAX_COMPILER ||
        writer->count > (TB_SHADER_LIBRARY_MAX_SIZE - table_offset) / ENTRY_SIZE)
    {
        return false;
    }
    tb_sort(writer->entries, writer->count, sizeof(writer->entries[0]), compare_entries, NULL);
    const size_t table_end = table_offset + writer->count * ENTRY_SIZE;
    if (!measure(writer, table_end, &total))
    {
        return false;
    }
    uint8_t *const out = tb_arena_calloc(arena, 1, total, ALIGNMENT);
    memcpy(out + HEADER_MAGIC_OFFSET, library_magic, sizeof(library_magic));
    tb_shader_store_u32(out + HEADER_VERSION_OFFSET, LIBRARY_VERSION);
    tb_shader_store_u32(out + HEADER_COUNT_OFFSET, (uint32_t)writer->count);
    tb_shader_store_u32(out + HEADER_COMPILER_LENGTH_OFFSET, (uint32_t)compiler_length);
    tb_shader_store_u64(out + HEADER_FILE_SIZE_OFFSET, (uint64_t)total);
    memcpy(out + HEADER_SIZE, compiler, compiler_length);
    size_t offset = table_end;
    for (size_t index = 0; index < writer->count; index++)
    {
        const writer_entry *const source = &writer->entries[index];
        uint8_t *const entry = out + table_offset + index * ENTRY_SIZE;
        offset = align_up(offset);
        memcpy(entry + ENTRY_KEY_OFFSET, source->key.bytes, TB_SHADER_KEY_SIZE);
        tb_shader_store_u64(entry + ENTRY_CODE_OFFSET, (uint64_t)offset);
        tb_shader_store_u32(entry + ENTRY_SIZE_OFFSET, (uint32_t)source->size);
        tb_shader_store_u32(entry + ENTRY_CHECKSUM_OFFSET, tb_crc32(source->code, source->size));
        memcpy(out + offset, source->code, source->size);
        offset += source->size;
    }
    tb_shader_store_u32(out + HEADER_CHECKSUM_OFFSET,
                        tb_crc32(out + HEADER_SIZE, table_end - HEADER_SIZE));
    *bytes = out;
    *size = total;
    return true;
}
