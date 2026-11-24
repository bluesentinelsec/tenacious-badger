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
 * if (tb_shader_library_load(vfs, "Engine/Shaders/D3D12.tbsl", 26, &library) ==
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
 * @param name The file's name in the VFS, such as `Engine/Shaders/D3D12.tbsl`.
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
