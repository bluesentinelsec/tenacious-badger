/** @file shader.h
 * @brief The shader compiler: shaders written once, in HLSL, for every backend.
 *
 * Shaders are written in HLSL, as Unreal's are, and compiled by the engine
 * for whichever backend runs them: ahead of time by `badger-shaderc` and at
 * run time by the engine, with the same result. See docs/shaders.md.
 *
 * This header holds the compiler's first step: reading a shader's sources.
 *
 * **Virtual shader paths.** Every shader file is named by an absolute
 * virtual path, as in Unreal: `/Engine/Private/DeferredLighting.hlsl`,
 * `/Engine/Public/Common.hlsli`, `/Game/Water.hlsl`. A `.hlsl` file is a
 * shader with entry points; a `.hlsli` file is included by others. A path
 * has a root (`/Engine`, `/Game`, `/Plugin`) and at least one more
 * component, and is checked by tb_shader_path_check().
 *
 * **Where files come from.** A compile reads its files through a loader.
 * tb_shader_vfs_loader() reads them from a VFS (badger/vfs.h), in which the
 * path `/Engine/Public/Common.hlsli` is the name `Engine/Public/Common.hlsli`:
 * the engine mounts its shaders at `Engine`, a game at `Game`, and a mod
 * mounted over a game overrides its shaders as it overrides its other files.
 * A compile can also supply files of its own in memory, such as the
 * generated `/Engine/Generated/...` files of Unreal; those win over the loader.
 *
 * **The preprocessor** behaves as DXC's does: object-like and function-like
 * macros, variadic macros, `#` and `##`, conditionals with `defined`,
 * `#include`, `#pragma once`, `#error`, `#warning` and `#line`. Other pragmas,
 * such as `#pragma pack_matrix`, are passed on. An include names a virtual
 * path, or a path relative to the including file.
 *
 * **Errors** name the virtual path, the line and the column, in the form
 * Visual Studio and other editors jump to:
 * `/Engine/Private/Foo.hlsl(12,5): error: unterminated comment`.
 *
 * @par Threading
 * tb_shader_preprocess() is thread-safe when each call has its own arena and
 * the loader is thread-safe, as the VFS loader is. Every other function is
 * thread-safe.
 */
#ifndef TB_SHADER_H
#define TB_SHADER_H

#include "badger/api.h"
#include "badger/arena.h"

#include <stdbool.h>
#include <stddef.h>

struct tb_vfs;

/** @brief Why a compile failed. */
typedef enum tb_shader_result
{
    TB_SHADER_OK,       /**< Success; the output may still hold warnings. */
    TB_SHADER_INVALID,  /**< The request is malformed, such as a main path that is not valid. */
    TB_SHADER_FAILED,   /**< The sources have errors, which the diagnostics describe. */
    TB_SHADER_NO_MEMORY /**< The caller's arena could not hold the output. */
} tb_shader_result;

/** @brief How serious a diagnostic is. */
typedef enum tb_shader_severity
{
    TB_SHADER_ERROR,  /**< The compile fails. */
    TB_SHADER_WARNING /**< The compile goes on. */
} tb_shader_severity;

/** @brief One error or warning, and where it is. */
typedef struct tb_shader_diagnostic
{
    tb_shader_severity severity; /**< How serious it is. */
    /** The virtual path of the file, or "" when the request itself is at fault. */
    const char *path;
    size_t line;         /**< Line, from 1; 0 when there is no file. */
    size_t column;       /**< Byte in the line, from 1; 0 when there is no file. */
    const char *message; /**< What is wrong, in lower case, without a final period. */
} tb_shader_diagnostic;

/** @brief A macro a compile defines before reading its main file, as `/D` does for DXC. */
typedef struct tb_shader_define
{
    const char *name;  /**< Required; an identifier. */
    const char *value; /**< Its replacement as text, or NULL for `1`. */
} tb_shader_define;

/** @brief A file a compile supplies in memory, which wins over the loader's. */
typedef struct tb_shader_file
{
    const char *path; /**< Required; a virtual path, NUL-terminated. */
    const char *text; /**< The file; NULL only at length zero. Borrowed for the call. */
    size_t length;    /**< Bytes of `text`. */
} tb_shader_file;

/** @brief What a loader found. */
typedef enum tb_shader_load_result
{
    TB_SHADER_LOAD_OK,        /**< The file was read. */
    TB_SHADER_LOAD_NOT_FOUND, /**< There is no such file. */
    TB_SHADER_LOAD_FAILED     /**< The file exists, but could not be read. */
} tb_shader_load_result;

/**
 * @brief Read a shader file whole.
 * @param context The loader's context.
 * @param path A virtual path, already checked, NUL-terminated.
 * @param length Bytes of `path`.
 * @param arena Where to put the text; it lasts as long as the compile needs it.
 * @param[out] text The file's bytes, in `arena`, on OK.
 * @param[out] text_length Bytes of `text`, on OK.
 * @return What was found.
 */
typedef tb_shader_load_result tb_shader_load_function(void *context, const char *path,
                                                      size_t length, tb_arena *arena,
                                                      const char **text, size_t *text_length);

/** @brief Where a compile reads its files from. */
typedef struct tb_shader_loader
{
    tb_shader_load_function *load; /**< Required. */
    void *context;                 /**< Passed to `load`. */
} tb_shader_loader;

/** @brief The preprocessed text marks each change of file and each jump in lines with `#line`. */
#define TB_SHADER_LINE_MARKERS 1u

/** @brief What to preprocess. Zero-initialize, then fill in what is needed. */
typedef struct tb_shader_preprocess_request
{
    const char *path;                /**< Required; the main file's virtual path, NUL-terminated. */
    const tb_shader_define *defines; /**< Macros defined first; NULL only at count zero. */
    size_t define_count;             /**< Entries of `defines`. */
    const tb_shader_file *files;     /**< Files in memory; NULL only at count zero. */
    size_t file_count;               /**< Entries of `files`. */
    /** Where every other file comes from, or NULL when `files` holds them all. Borrowed. */
    const tb_shader_loader *loader;
    unsigned flags; /**< A combination of TB_SHADER_LINE_MARKERS. */
    /** Tokens the preprocessor may make, from files and macros alike, which
     * bounds what a source can expand to; 0 for TB_SHADER_MAX_TOKENS. */
    size_t max_tokens;
} tb_shader_preprocess_request;

/** @brief A preprocessed shader. Everything in it lives in the caller's arena. */
typedef struct tb_shader_preprocessed
{
    const char *text; /**< The preprocessed text, with a NUL after it; "" on failure. */
    size_t length;    /**< Bytes of `text` before the NUL. */
    /** Every file the compile read, main file first, each once, in the order it was first read:
     * what the shader cache keys on, and what hot reload watches. */
    const char *const *dependencies;
    size_t dependency_count;                 /**< Entries of `dependencies`. */
    const tb_shader_diagnostic *diagnostics; /**< Errors and warnings, in the order found. */
    size_t diagnostic_count;                 /**< Entries of `diagnostics`. */
} tb_shader_preprocessed;

/** @brief Files included inside one another, the main file counting as one. */
#define TB_SHADER_MAX_INCLUDE_DEPTH ((size_t)64)

/** @brief Bytes of one file. */
#define TB_SHADER_MAX_FILE_SIZE ((size_t)16 << 20)

/** @brief Tokens the preprocessor makes for one shader when the request gives no limit. */
#define TB_SHADER_MAX_TOKENS ((size_t)1 << 20)

/** @brief Errors reported before a compile stops looking for more. */
#define TB_SHADER_MAX_ERRORS ((size_t)64)

/**
 * @brief Check a virtual shader path.
 *
 * A valid path starts with `/`, has a root and at least one more component,
 * and ends in `.hlsl` or `.hlsli`. No component is empty, `.` or `..`, and
 * no byte is a `\`, a `:`, a control character or a space, so a path means
 * the same thing on every system.
 * @param path NULL only at length zero. @param length Bytes of `path`.
 * @return Whether the path is valid.
 */
TB_API bool tb_shader_path_check(const char *path, size_t length);

/**
 * @brief Preprocess a shader: read its main file and everything it includes,
 * and apply its directives and macros.
 * @param request Required.
 * @param arena Where the output lives, until the arena is reset or destroyed.
 * @param[out] output Required. On OK and FAILED, filled in: FAILED has the
 * errors. On INVALID, the diagnostics say why. On NO_MEMORY, zero.
 * @return OK, INVALID, FAILED or NO_MEMORY.
 * @par Errors Running out of memory for the compile's own work ends the
 * process, as allocation does. Only the output's arena reports NO_MEMORY.
 */
TB_API tb_shader_result tb_shader_preprocess(const tb_shader_preprocess_request *request,
                                             tb_arena *arena, tb_shader_preprocessed *output);

/**
 * @brief A loader that reads shader files from a VFS, the path `/Engine/X.hlsli`
 * as the name `Engine/X.hlsli`.
 * @param vfs Required; borrowed, and must outlive every compile that uses the loader.
 * @return The loader. A file over TB_SHADER_MAX_FILE_SIZE is FAILED.
 */
TB_API tb_shader_loader tb_shader_vfs_loader(struct tb_vfs *vfs);

/**
 * @brief Write a diagnostic as editors read it: `path(line,column): error: message`,
 * or `error: message` when it has no file.
 * @param diagnostic Required.
 * @param buffer Where to write; NULL only at size zero. Always NUL-terminated when size is not
 * zero.
 * @param size Bytes of `buffer`.
 * @return Bytes the whole text needs, without the NUL, as tb_fmt_buffer() returns.
 */
TB_API size_t tb_shader_diagnostic_format(const tb_shader_diagnostic *diagnostic, char *buffer,
                                          size_t size);

/**
 * @brief Name a result, for a log or a message: "failed", "no memory".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_shader_result_name(tb_shader_result result);

#endif
