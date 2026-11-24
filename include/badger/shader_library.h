/** @file shader_library.h
 * @brief Shader libraries: the compiled shaders a game ships, one library
 * for each backend, found by their shader keys.
 *
 * `badger-shaderc` compiles a game's shaders ahead of time into a library,
 * which the game loads through the VFS, as Unreal's packaged games load
 * their shader libraries, so a shipped game needs no compiler for any shader
 * its libraries hold. The format is docs/shader-cache.md's.
 *
 * @code
 * tb_shader_library *library = NULL;
 * if (tb_shader_library_load(vfs, "Shaders/d3d12.tbsl", 18, &library) ==
 *     TB_SHADER_LIBRARY_OK)
 * {
 *     const void *code = NULL;
 *     size_t size = 0;
 *     if (tb_shader_library_find(library, &key, &code, &size)) { ... }
 *     tb_shader_library_close(library);
 * }
 * @endcode
 *
 * Every entry is found by its shader key (tb_shader_key_make()), which
 * names the sources, the defines, the entry point, the stage and the
 * target, but not the compiler: a library records the compiler that wrote
 * it once, for its whole content (tb_shader_library_compiler()). A shader
 * whose sources changed after its library was written has another key, so
 * it is not found, and the engine compiles it, where it may.
 *
 * A library is checked whole when it is opened: a library that is damaged,
 * truncated, or of another version of the format is refused, never read in
 * part.
 *
 * @par Threading
 * An open library is immutable: every function that reads one is
 * thread-safe. A writer is externally synchronized.
 */
#ifndef TB_SHADER_LIBRARY_H
#define TB_SHADER_LIBRARY_H

#include "badger/api.h"
#include "badger/arena.h"
#include "badger/shader_cache.h"

#include <stdbool.h>
#include <stddef.h>

struct tb_vfs;

/** @brief What opening a library came to. */
typedef enum tb_shader_library_result
{
    TB_SHADER_LIBRARY_OK,          /**< Success. */
    TB_SHADER_LIBRARY_NOT_FOUND,   /**< The VFS has no such file. */
    TB_SHADER_LIBRARY_READ_FAILED, /**< The file exists, but could not be read, or is too large. */
    TB_SHADER_LIBRARY_DAMAGED,     /**< The bytes are not a whole library. */
    /** The bytes are a library of a version of the format this engine does
     * not read: write it again with this engine's `badger-shaderc`. */
    TB_SHADER_LIBRARY_VERSION
} tb_shader_library_result;

/** @brief Bytes of a library's file at most. */
#define TB_SHADER_LIBRARY_MAX_SIZE ((size_t)1 << 30)

/** @brief Bytes of the name of the compiler a library records at most. */
#define TB_SHADER_LIBRARY_MAX_COMPILER ((size_t)1024)

/** @brief An open library. Opaque. */
typedef struct tb_shader_library tb_shader_library;

/**
 * @brief Open a library held in memory, checking it whole.
 * @param bytes The library; NULL only at size zero. Copied: the caller may
 * release them when this returns.
 * @param size Bytes of `bytes`.
 * @param[out] library Required. Receives the library, which the caller
 * closes with tb_shader_library_close(); NULL on failure.
 * @return OK, DAMAGED or VERSION.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_shader_library_result tb_shader_library_open(const void *bytes, size_t size,
                                                       tb_shader_library **library);

/**
 * @brief Read a library from a VFS, and open it as tb_shader_library_open() does.
 * @param vfs Required. Borrowed for the call.
 * @param name The file's name in the VFS, such as `Shaders/d3d12.tbsl`.
 * @param length Bytes of `name`.
 * @param[out] library Required. As tb_shader_library_open().
 * @return OK, NOT_FOUND, READ_FAILED (for a file over
 * TB_SHADER_LIBRARY_MAX_SIZE too), DAMAGED or VERSION.
 * @par Errors Running out of memory ends the process, as allocation does.
 * @par Threading Thread-safe, as reading a VFS is.
 */
TB_API tb_shader_library_result tb_shader_library_load(struct tb_vfs *vfs, const char *name,
                                                       size_t length, tb_shader_library **library);

/**
 * @brief Close a library. Code it gave is no longer valid.
 * @param library The library, or NULL, which does nothing.
 * @par Threading Must not race any other use of the library.
 */
TB_API void tb_shader_library_close(tb_shader_library *library);

/**
 * @brief Find a shader key's code.
 * @param library Required.
 * @param key Required; a shader key.
 * @param[out] code Required. The code, aligned to 8 bytes, valid until the
 * library is closed, when found; NULL otherwise.
 * @param[out] size Required. Bytes of the code when found; 0 otherwise.
 * @return Whether the library has the key.
 */
TB_API bool tb_shader_library_find(const tb_shader_library *library, const tb_shader_key *key,
                                   const void **code, size_t *size);

/**
 * @brief The shaders a library holds.
 * @param library Required.
 * @return Its entries.
 */
TB_API size_t tb_shader_library_count(const tb_shader_library *library);

/**
 * @brief The compiler that wrote a library, as it named itself (tb_dxc_version()).
 * @param library Required.
 * @return UTF-8, NUL-terminated; valid until the library is closed.
 */
TB_API const char *tb_shader_library_compiler(const tb_shader_library *library);

/**
 * @brief Name a result, for a log or a message: "damaged".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_shader_library_result_name(tb_shader_library_result result);

/* -------------------------------------------------------------------------
 * Finding a shader, or compiling it.
 * ------------------------------------------------------------------------- */

/** @brief Where tb_shader_compile_cached() looks for a shader, and whether
 * it may compile one. Zero-initialized, it compiles every shader. */
typedef struct tb_shader_lookup
{
    /** Searched first, by shader key; NULL for none. Borrowed for the call;
     * code found there lasts as long as the library is open. */
    const tb_shader_library *library;
    /** Searched next, by cache key, and given what is compiled; NULL for
     * none. Borrowed for the call. */
    tb_shader_cache *cache;
    /** Whether the library alone may give shaders, as `shaders.runtime_compile`
     * off asks of a shipped game: a shader it lacks is unavailable. */
    bool library_only;
} tb_shader_lookup;

/** @brief Where a shader tb_shader_compile_cached() gave came from. */
typedef enum tb_shader_origin
{
    TB_SHADER_ORIGIN_COMPILE, /**< It was compiled. */
    TB_SHADER_ORIGIN_LIBRARY, /**< The library held it. */
    TB_SHADER_ORIGIN_CACHE    /**< The cache held it. */
} tb_shader_origin;

/**
 * @brief Find a shader in a library, then in a cache, else compile it and
 * keep it in the cache: what the renderer and `badger-shaderc` both do, so
 * that both give the same bytes.
 *
 * A request for TB_SHADER_TARGET_NONE writes no code, so it is compiled
 * whatever the lookup says. Otherwise the shader key is made
 * (tb_shader_key_make()), and the library searched. The cache is searched
 * by the cache key of the request's DXC (tb_dxc_version()). A shader in
 * neither is compiled with tb_shader_compile(), unless the lookup is
 * library-only, or the request has no DXC: then it is UNAVAILABLE, and a
 * diagnostic says why. A source with errors is compiled, so that the
 * diagnostics say what they are.
 * @param request Required, as tb_shader_compile() takes it.
 * @param lookup Required.
 * @param arena Where the output lives, as for tb_shader_compile().
 * @param[out] output Required. On OK from a library or a cache, only `code`
 * and `code_size` are filled in: the reflection comes with a compile.
 * Otherwise as tb_shader_compile() fills it in.
 * @param[out] key Required. The shader key, or zero for a request that has
 * none.
 * @param[out] origin Required. Where the shader came from, on OK.
 * @return As tb_shader_compile(), or UNAVAILABLE.
 * @par Threading Thread-safe, as tb_shader_compile() is.
 */
TB_API tb_shader_result tb_shader_compile_cached(const tb_shader_compile_request *request,
                                                 const tb_shader_lookup *lookup, tb_arena *arena,
                                                 tb_shader_compiled *output, tb_shader_key *key,
                                                 tb_shader_origin *origin);

/* -------------------------------------------------------------------------
 * Manifests.
 * ------------------------------------------------------------------------- */

/** @brief Defines one shader of a manifest has at most. */
#define TB_SHADER_MANIFEST_MAX_DEFINES ((size_t)32)

/** @brief One shader a manifest lists. Its strings live in the reader's arena. */
typedef struct tb_shader_manifest_entry
{
    const char *path;                /**< Its file's virtual path. */
    tb_shader_stage stage;           /**< The stage it runs in. */
    const char *entry_point;         /**< The function the stage runs. */
    const tb_shader_define *defines; /**< Its defines, in order. */
    size_t define_count;             /**< Entries of `defines`. */
    size_t line;                     /**< Its line in the manifest, from 1. */
} tb_shader_manifest_entry;

/** @brief A manifest as tb_shader_manifest_read() read it. */
typedef struct tb_shader_manifest
{
    const tb_shader_manifest_entry *entries; /**< In the order of their lines. */
    size_t entry_count;                      /**< Entries of `entries`. */
    const tb_shader_diagnostic *diagnostics; /**< Its errors, by line and column. */
    size_t diagnostic_count;                 /**< Entries of `diagnostics`. */
} tb_shader_manifest;

/**
 * @brief Read a manifest: the shaders `badger-shaderc` compiles into a
 * library, one to a line.
 *
 * A line lists a shader's virtual path, its stage as `[shader("...")]`
 * names it, its entry point, and its defines, `NAME` or `NAME=VALUE`,
 * separated by spaces or tabs. A `#` begins a comment, to the end of the
 * line, and a line with nothing else is ignored:
 * @code
 * # path                          stage   entry point  defines
 * /Engine/Private/Sky.hlsl        vertex  MainVS
 * /Engine/Private/Blur.hlsl       compute MainCS       RADIUS=4 HORIZONTAL
 * @endcode
 * @param text The manifest, UTF-8; NULL only at length zero. Borrowed.
 * @param length Bytes of `text`.
 * @param name What its diagnostics name it, such as its file's path. Copied.
 * @param arena Where the manifest lives, until the arena is reset or destroyed.
 * @param[out] manifest Required. Filled in on OK; on FAILED, its
 * diagnostics say what is wrong, and its entries are the lines that read.
 * @return OK or FAILED.
 * @par Errors Running out of memory ends the process, as allocation does.
 * @par Threading Thread-safe when each call has its own arena.
 */
TB_API tb_shader_result tb_shader_manifest_read(const char *text, size_t length, const char *name,
                                                tb_arena *arena, tb_shader_manifest *manifest);

/* -------------------------------------------------------------------------
 * Writing a library.
 * ------------------------------------------------------------------------- */

/** @brief A library being written. Opaque. */
typedef struct tb_shader_library_writer tb_shader_library_writer;

/**
 * @brief Begin a library.
 * @return The writer, for tb_shader_library_writer_destroy().
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_shader_library_writer *tb_shader_library_writer_create(void);

/**
 * @brief Release a writer, and what it holds.
 * @param writer The writer, or NULL, which does nothing.
 */
TB_API void tb_shader_library_writer_destroy(tb_shader_library_writer *writer);

/**
 * @brief Add a shader to a library.
 *
 * A key added twice with the same code is kept once: two entries of a
 * manifest may compile alike.
 * @param writer Required.
 * @param key Required; the shader key.
 * @param code Required; the compiled code. Copied.
 * @param size Bytes of `code`: from 1 to TB_SHADER_CACHE_MAX_CODE.
 * @return true; false, adding nothing, for a size out of range, or a key
 * added before with other code.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API bool tb_shader_library_writer_add(tb_shader_library_writer *writer, const tb_shader_key *key,
                                         const void *code, size_t size);

/**
 * @brief Write the library, its entries in the order of their keys.
 *
 * The same entries and compiler always give the same bytes, in whatever
 * order the entries were added.
 * @param writer Required. It may be added to and written again after.
 * @param compiler Required, NUL-terminated: what compiled the library's
 * shaders, as it names itself (tb_dxc_version()); at most
 * TB_SHADER_LIBRARY_MAX_COMPILER bytes.
 * @param arena Where the bytes are written, aligned to 8 bytes.
 * @param[out] bytes Required. The library, on true.
 * @param[out] size Required. Bytes of the library, on true.
 * @return true; false for a compiler's name too long, or a library over
 * TB_SHADER_LIBRARY_MAX_SIZE.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API bool tb_shader_library_writer_finish(tb_shader_library_writer *writer, const char *compiler,
                                            tb_arena *arena, const void **bytes, size_t *size);

#endif
