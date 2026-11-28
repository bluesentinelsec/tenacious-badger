/** @file shader_cache.h
 * @brief What a compiled shader is known by, and the cache on disk that
 * keeps compiled shaders between runs.
 *
 * A shader compiled ahead of time by `badger-shaderc` and the same shader
 * compiled at run time by the engine give the same bytes, and share one
 * cache, as Unreal's ShaderCompileWorker and editor share the derived data
 * cache. See docs/shader-cache.md.
 *
 * @code
 * tb_shader_key key = {0};
 * if (tb_shader_key_make(&request, &key) == TB_SHADER_OK)
 * {
 *     tb_shader_key entry = {0};
 *     tb_shader_cache_key(&key, tb_dxc_version(dxc), &entry);
 *     if (tb_shader_cache_get(cache, &entry, arena, &code, &size) != TB_SHADER_CACHE_OK)
 *     {
 *         // compile, then tb_shader_cache_put(cache, &entry, code, size)
 *     }
 * }
 * @endcode
 *
 * **Two keys.** A *shader key* (tb_shader_key_make()) names what a compile
 * is asked to do: the shader's sources, preprocessed with every file they
 * include, the defines, the entry point and its stage, the target and its
 * shader model, the compile's flags, and TB_SHADER_COMPILER_REVISION. A
 * shader library (badger/shader_library.h) holds shaders by it, so that a
 * game finds its shaders without a compiler. A *cache key*
 * (tb_shader_cache_key()) adds the compiler that did the work, as it names
 * itself (tb_dxc_version(), tb_d3dcompiler_version()), so that a new
 * compiler compiles again rather than reuse what an old one wrote. Both are SHA-256 digests
 * (badger/sha256.h).
 *
 * **The cache** is a directory with one file for each cache key, in a
 * documented, versioned format. An entry is written whole, then renamed into
 * place, so a reader sees an entry whole or not at all, and any number of
 * threads and programs may share a cache. An entry that is damaged,
 * truncated, or of another version is reported as DAMAGED, which a caller
 * treats as a miss: it compiles, and its put replaces the entry. The cache
 * never trusts an entry it has not checked.
 *
 * @par Threading
 * Every function is thread-safe. A cache may be shared by any number of
 * threads, and its directory by any number of programs.
 */
#ifndef TB_SHADER_CACHE_H
#define TB_SHADER_CACHE_H

#include "badger/api.h"
#include "badger/arena.h"
#include "badger/shader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Bytes of a key: a SHA-256 digest. */
#define TB_SHADER_KEY_SIZE 32u

/** @brief Bytes of a key written in hexadecimal by tb_shader_key_format(), its NUL included. */
#define TB_SHADER_KEY_TEXT_CAPACITY 65u

/**
 * @brief The revision of what the engine's compiler writes for a target,
 * which every shader key includes.
 *
 * Raise it in a change that makes the compiler write different bytes for
 * the same sources, such as a change to the text the compiler gives DXC, so
 * that libraries and caches written before it are no longer found.
 */
#define TB_SHADER_COMPILER_REVISION 6u

/** @brief A shader key or a cache key. */
typedef struct tb_shader_key
{
    uint8_t bytes[TB_SHADER_KEY_SIZE]; /**< The digest. */
} tb_shader_key;

/**
 * @brief Make the shader key of a compile: what it would compile, whatever
 * compiler does it.
 *
 * Preprocesses the request's source as tb_shader_compile() would, with
 * `#line` markers, so that a change to any file it includes, or to where
 * its code sits in a file, changes the key, and hashes the text with the
 * defines, in order, each without a value as `1`; the entry point; the
 * stage; the target; the shader model, its default applied; the flags; and
 * TB_SHADER_COMPILER_REVISION. Neither DXC nor any other compiler is called.
 *
 * @param request Required. Its DXC is not used.
 * @param[out] key Required. Filled in on OK; zero otherwise.
 * @return OK; INVALID for a request with no entry point or stage, or a
 * source tb_shader_preprocess() calls INVALID; FAILED for a source with
 * errors, which only a compile reports. Never NO_MEMORY.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_shader_result tb_shader_key_make(const tb_shader_compile_request *request,
                                           tb_shader_key *key);

/**
 * @brief Make the cache key of a shader key compiled by a compiler.
 * @param shader Required; a shader key.
 * @param compiler Required, NUL-terminated: the compiler as it names itself,
 * such as tb_dxc_version() or tb_d3dcompiler_version() gives.
 * @param[out] key Required. Receives the cache key.
 */
TB_API void tb_shader_cache_key(const tb_shader_key *shader, const char *compiler,
                                tb_shader_key *key);

/**
 * @brief Write a key in lower-case hexadecimal, as the cache names its files.
 * @param key Required.
 * @param text Required; TB_SHADER_KEY_TEXT_CAPACITY bytes, NUL-terminated after.
 */
TB_API void tb_shader_key_format(const tb_shader_key *key, char *text);

/** @brief What a cache did. */
typedef enum tb_shader_cache_result
{
    TB_SHADER_CACHE_OK,      /**< Success: for a get, a hit. */
    TB_SHADER_CACHE_MISS,    /**< The cache has no entry for the key. */
    TB_SHADER_CACHE_DAMAGED, /**< The entry is damaged, truncated, or of another version: a miss. */
    TB_SHADER_CACHE_FAILED   /**< The directory or the entry could not be made or written. */
} tb_shader_cache_result;

/** @brief Bytes of compiled code an entry holds at most. */
#define TB_SHADER_CACHE_MAX_CODE ((size_t)64 << 20)

/** @brief A cache of compiled shaders in a directory. Opaque. */
typedef struct tb_shader_cache tb_shader_cache;

/**
 * @brief Open a cache in a directory, making the directory and its parents
 * where they are missing.
 * @param directory Required, NUL-terminated; a UTF-8 path in the system's
 * syntax, such as `build/ShaderCache`. Copied.
 * @param[out] cache Required. Receives the cache, which the caller closes
 * with tb_shader_cache_close(); NULL on failure.
 * @return OK, or FAILED when the directory cannot be made.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_shader_cache_result tb_shader_cache_open(const char *directory, tb_shader_cache **cache);

/**
 * @brief The cache every program of the engine shares by default, as
 * Unreal's editors share their local derived data cache:
 * `TenaciousBadger/ShaderCache` in the user's cache directory, such as
 * `%LOCALAPPDATA%` on Windows and `~/.cache` on Linux. Keys hold everything
 * that makes two compiles differ, so games and builds share it safely.
 * @param buffer Where to write the path, NUL-terminated; NULL only at size zero.
 * @param size Bytes of `buffer`.
 * @return Whether the system has a cache directory and the path fits.
 */
TB_API bool tb_shader_cache_default_directory(char *buffer, size_t size);

/**
 * @brief Close a cache. Its entries stay on disk.
 * @param cache The cache, or NULL, which does nothing. No other call may use it after.
 */
TB_API void tb_shader_cache_close(tb_shader_cache *cache);

/**
 * @brief Find a cache key's code.
 * @param cache Required.
 * @param key Required; a cache key.
 * @param arena Where the code is copied to, aligned to 8 bytes, which a DXIL
 * container needs. It lasts until the arena is reset or destroyed.
 * @param[out] code Required. The code on OK; NULL otherwise.
 * @param[out] size Required. Bytes of the code on OK; 0 otherwise.
 * @return OK; MISS when there is no entry, or it cannot be read; DAMAGED.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_shader_cache_result tb_shader_cache_get(tb_shader_cache *cache, const tb_shader_key *key,
                                                  tb_arena *arena, const void **code, size_t *size);

/**
 * @brief Keep a cache key's code, replacing any entry it has.
 *
 * The entry is written beside its place, then renamed into it, so a reader
 * never sees it in part. A failure leaves the cache as it was, and the
 * caller goes on without it: a cache that cannot be written is slower, not
 * wrong.
 * @param cache Required.
 * @param key Required; a cache key.
 * @param code Required; the compiled code. Borrowed for the call.
 * @param size Bytes of `code`: from 1 to TB_SHADER_CACHE_MAX_CODE.
 * @return OK, or FAILED.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_shader_cache_result tb_shader_cache_put(tb_shader_cache *cache, const tb_shader_key *key,
                                                  const void *code, size_t size);

/**
 * @brief Name a result, for a log or a message: "damaged".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_shader_cache_result_name(tb_shader_cache_result result);

#endif
