/* Shader keys, and the cache of compiled shaders on disk. The format of an
 * entry is docs/shader-cache.md's; shader_bytes.h reads and writes its
 * numbers. */
#include "shader_bytes.h"
#include "shader_dxbc.h"

#include "badger/arena.h"
#include "badger/checksum.h"
#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/path.h"
#include "badger/sha256.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/str.h"

#include <string.h>

/* What every shader key and cache key starts with, so that neither can be
 * the other, or any other digest of the same bytes. */
static const char shader_key_tag[] = "badger shader key";
static const char cache_key_tag[] = "badger shader cache key";

/* An entry: its header, then its code. */
static const uint8_t entry_magic[4] = {'T', 'B', 'S', 'C'};
#define ENTRY_VERSION 1u
#define ENTRY_MAGIC_OFFSET ((size_t)0)
#define ENTRY_VERSION_OFFSET ((size_t)4)
#define ENTRY_KEY_OFFSET ((size_t)8)
#define ENTRY_SIZE_OFFSET ((size_t)40)
#define ENTRY_CHECKSUM_OFFSET ((size_t)48)
#define ENTRY_RESERVED_OFFSET ((size_t)52)
#define ENTRY_HEADER_SIZE ((size_t)56)

/* What an entry's file is named after its key. */
#define ENTRY_EXTENSION ".tbsc"

/* The alignment code is copied to: a DXIL container's words need 4. */
#define CODE_ALIGNMENT ((size_t)8)

#define STAGE_COUNT ((size_t)TB_SHADER_STAGE_CALLABLE + 1)

/* Bytes of the user's cache directory at most. */
#define TB_SHADER_CACHE_PATH_CAPACITY ((size_t)4096)

struct tb_shader_cache
{
    char *directory;
};

/* -------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

static void hash_u64(tb_sha256_state *hash, uint64_t value)
{
    uint8_t bytes[8] = {0};
    tb_shader_store_u64(bytes, value);
    tb_sha256_update(hash, bytes, sizeof(bytes));
}

/* Bytes, after their length, so that no two lists of strings hash alike. */
static void hash_bytes(tb_sha256_state *hash, const void *bytes, size_t length)
{
    hash_u64(hash, (uint64_t)length);
    tb_sha256_update(hash, bytes, length);
}

static void hash_string(tb_sha256_state *hash, const char *string)
{
    hash_bytes(hash, string, tb_str_length(string));
}

static void hash_request(tb_sha256_state *hash, const tb_shader_compile_request *request)
{
    const tb_shader_preprocess_request *const source = &request->source;
    hash_string(hash, shader_key_tag);
    hash_u64(hash, TB_SHADER_COMPILER_REVISION);
    hash_string(hash, request->entry_point);
    hash_u64(hash, (uint64_t)request->stage);
    hash_u64(hash, (uint64_t)request->target);
    hash_u64(hash, (uint64_t)tb_shader_target_model(request));
    hash_u64(hash, (uint64_t)request->flags);
    hash_u64(hash, (uint64_t)source->define_count);
    for (size_t index = 0; index < source->define_count; index++)
    {
        const char *const value = source->defines[index].value;
        hash_string(hash, source->defines[index].name);
        hash_string(hash, value == NULL ? "1" : value);
    }
}

tb_shader_result tb_shader_key_make(const tb_shader_compile_request *request, tb_shader_key *key)
{
    const tb_shader_key empty = {0};
    *key = empty;
    if (request->entry_point == NULL || request->entry_point[0] == '\0' ||
        (size_t)request->stage >= STAGE_COUNT)
    {
        return TB_SHADER_INVALID;
    }
    tb_arena *const scratch = tb_arena_create(0);
    tb_shader_preprocess_request source = request->source;
    source.flags |= TB_SHADER_LINE_MARKERS;
    tb_shader_preprocessed preprocessed = {0};
    const tb_shader_result result = tb_shader_preprocess(&source, scratch, &preprocessed);
    if (result == TB_SHADER_OK)
    {
        tb_sha256_state hash = {0};
        tb_sha256_init(&hash);
        hash_request(&hash, request);
        hash_bytes(&hash, preprocessed.text, preprocessed.length);
        tb_sha256_final(&hash, key->bytes);
    }
    tb_arena_destroy(scratch);
    /* The scratch arena grows, so only the source can fail. */
    return result;
}

void tb_shader_cache_key(const tb_shader_key *shader, const char *compiler, tb_shader_key *key)
{
    tb_sha256_state hash = {0};
    tb_sha256_init(&hash);
    hash_string(&hash, cache_key_tag);
    hash_bytes(&hash, shader->bytes, sizeof(shader->bytes));
    hash_string(&hash, compiler);
    tb_sha256_final(&hash, key->bytes);
}

void tb_shader_key_format(const tb_shader_key *key, char *text)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < TB_SHADER_KEY_SIZE; index++)
    {
        text[index * 2] = digits[key->bytes[index] >> 4];
        text[index * 2 + 1] = digits[key->bytes[index] & 0x0Fu];
    }
    text[TB_SHADER_KEY_SIZE * 2] = '\0';
}

/* -------------------------------------------------------------------------
 * The cache.
 * ------------------------------------------------------------------------- */

static tb_path_view view_of(const char *text)
{
    const tb_path_view view = {text, tb_str_length(text)};
    return view;
}

tb_shader_cache_result tb_shader_cache_open(const char *directory, tb_shader_cache **cache)
{
    *cache = NULL;
    if (directory[0] == '\0' || tb_fs_make_directories(view_of(directory)) != TB_FS_OK)
    {
        return TB_SHADER_CACHE_FAILED;
    }
    tb_shader_cache *const opened = tb_calloc(1, sizeof(*opened));
    const size_t length = tb_str_length(directory);
    opened->directory = tb_malloc(length + 1);
    memcpy(opened->directory, directory, length + 1);
    *cache = opened;
    return TB_SHADER_CACHE_OK;
}

bool tb_shader_cache_default_directory(char *buffer, size_t size)
{
    char base[TB_SHADER_CACHE_PATH_CAPACITY] = {0};
    size_t required = 0;
    if (tb_fs_get_location(TB_FS_USER_CACHE, base, sizeof(base), &required) != TB_FS_OK)
    {
        /* coverage-off: every system the engine runs on has a cache directory */
        return false;
        /* coverage-on */
    }
    return tb_path_rooted_join(tb_fs_native_style(), view_of(base),
                               view_of("TenaciousBadger/ShaderCache"), buffer, size,
                               &required) == TB_PATH_OK;
}

void tb_shader_cache_close(tb_shader_cache *cache)
{
    if (cache == NULL)
    {
        return;
    }
    tb_free(cache->directory);
    tb_free(cache);
}

/* The path of a key's entry, for tb_str_free(NULL, ...). */
static char *entry_path(const tb_shader_cache *cache, const tb_shader_key *key)
{
    char name[TB_SHADER_KEY_TEXT_CAPACITY + sizeof(ENTRY_EXTENSION)] = {0};
    tb_shader_key_format(key, name);
    memcpy(name + TB_SHADER_KEY_SIZE * 2, ENTRY_EXTENSION, sizeof(ENTRY_EXTENSION));
    tb_path_result result = TB_PATH_OK;
    char *const path = tb_path_join_new(NULL, tb_fs_native_style(), view_of(cache->directory),
                                        view_of(name), &result);
    return path;
}

/* Whether an entry's bytes are whole, of this version, and for this key;
 * its code is then after its header. */
static bool entry_is_whole(const unsigned char *bytes, size_t length, const tb_shader_key *key)
{
    if (length < ENTRY_HEADER_SIZE ||
        memcmp(bytes + ENTRY_MAGIC_OFFSET, entry_magic, sizeof(entry_magic)) != 0 ||
        tb_shader_load_u32(bytes + ENTRY_VERSION_OFFSET) != ENTRY_VERSION ||
        memcmp(bytes + ENTRY_KEY_OFFSET, key->bytes, TB_SHADER_KEY_SIZE) != 0 ||
        tb_shader_load_u32(bytes + ENTRY_RESERVED_OFFSET) != 0)
    {
        return false;
    }
    const uint64_t size = tb_shader_load_u64(bytes + ENTRY_SIZE_OFFSET);
    const size_t code_length = length - ENTRY_HEADER_SIZE;
    return size != 0 && size == (uint64_t)code_length &&
           tb_crc32(bytes + ENTRY_HEADER_SIZE, code_length) ==
               tb_shader_load_u32(bytes + ENTRY_CHECKSUM_OFFSET);
}

tb_shader_cache_result tb_shader_cache_get(tb_shader_cache *cache, const tb_shader_key *key,
                                           tb_arena *arena, const void **code, size_t *size)
{
    char *const path = entry_path(cache, key);
    tb_fs_buffer buffer = {0};
    tb_shader_cache_result result = TB_SHADER_CACHE_OK;
    *code = NULL;
    *size = 0;
    if (path == NULL)
    {
        /* coverage-off: a directory the system made takes a file's name */
        return TB_SHADER_CACHE_MISS;
        /* coverage-on */
    }
    const tb_fs_result read =
        tb_fs_read_file(view_of(path), NULL, ENTRY_HEADER_SIZE + TB_SHADER_CACHE_MAX_CODE, &buffer);
    if (read == TB_FS_TOO_LARGE)
    {
        result = TB_SHADER_CACHE_DAMAGED;
        goto cleanup;
    }
    if (read != TB_FS_OK)
    {
        result = TB_SHADER_CACHE_MISS;
        goto cleanup;
    }
    if (!entry_is_whole(buffer.data, buffer.length, key))
    {
        result = TB_SHADER_CACHE_DAMAGED;
        goto cleanup;
    }
    const size_t code_length = buffer.length - ENTRY_HEADER_SIZE;
    void *const copy = tb_arena_alloc(arena, code_length, CODE_ALIGNMENT);
    memcpy(copy, buffer.data + ENTRY_HEADER_SIZE, code_length);
    *code = copy;
    *size = code_length;
cleanup:
    tb_fs_buffer_free(&buffer);
    tb_str_free(NULL, path);
    return result;
}

tb_shader_cache_result tb_shader_cache_put(tb_shader_cache *cache, const tb_shader_key *key,
                                           const void *code, size_t size)
{
    if (size == 0 || size > TB_SHADER_CACHE_MAX_CODE)
    {
        return TB_SHADER_CACHE_FAILED;
    }
    const size_t length = ENTRY_HEADER_SIZE + size;
    unsigned char *const bytes = tb_calloc(1, length);
    memcpy(bytes + ENTRY_MAGIC_OFFSET, entry_magic, sizeof(entry_magic));
    tb_shader_store_u32(bytes + ENTRY_VERSION_OFFSET, ENTRY_VERSION);
    memcpy(bytes + ENTRY_KEY_OFFSET, key->bytes, TB_SHADER_KEY_SIZE);
    tb_shader_store_u64(bytes + ENTRY_SIZE_OFFSET, (uint64_t)size);
    tb_shader_store_u32(bytes + ENTRY_CHECKSUM_OFFSET, tb_crc32(code, size));
    memcpy(bytes + ENTRY_HEADER_SIZE, code, size);
    char *const path = entry_path(cache, key);
    const tb_fs_result written =
        path == NULL ? TB_FS_INVALID : tb_fs_replace_file_cached(view_of(path), bytes, length);
    tb_str_free(NULL, path);
    tb_free(bytes);
    return written == TB_FS_OK ? TB_SHADER_CACHE_OK : TB_SHADER_CACHE_FAILED;
}

const char *tb_shader_cache_result_name(tb_shader_cache_result result)
{
    switch (result)
    {
    case TB_SHADER_CACHE_OK:
        return "ok";
    case TB_SHADER_CACHE_MISS:
        return "miss";
    case TB_SHADER_CACHE_DAMAGED:
        return "damaged";
    case TB_SHADER_CACHE_FAILED:
        return "failed";
    }
    return "unknown result";
}
