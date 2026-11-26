/** @file d3dcompiler.h
 * @brief D3DCompiler, the HLSL compiler that is part of Windows, loaded at
 * run time: what compiles HLSL of Shader Model 5 into the DXBC bytecode
 * D3D11 runs.
 *
 * The shader module (badger/shader.h) compiles a shader for D3D11 through
 * this interface; most callers want tb_shader_compile(), not this.
 *
 * @code
 * tb_d3dcompiler *compiler = NULL;
 * tb_d3dcompiler_error error = TB_D3DCOMPILER_ERROR_INIT;
 * if (tb_d3dcompiler_open(&compiler, &error) != TB_D3DCOMPILER_OK)
 * {
 *     report(error.message); // and fall back to a backend that needs no DXBC
 * }
 * @endcode
 *
 * **Where it comes from.** `D3DCompiler_47.dll` is part of every Windows
 * since 8.1, in the system directory, from which tb_d3dcompiler_open()
 * loads it, and only from there: the engine ships no copy, and a copy
 * beside the executable is not looked at.
 *
 * **Unavailable, never fatal.** A system directory without the library
 * makes tb_d3dcompiler_open() return TB_D3DCOMPILER_UNAVAILABLE with a
 * message that says so: a capability the machine lacks, which the renderer
 * reports and falls back from. On Linux and macOS it is always unavailable:
 * DXBC runs only on D3D11, which only Windows has, so these systems lack the
 * capability rather than the work. A system the engine does not support
 * reports "not implemented", as every platform interface does there.
 *
 * @par Threading
 * tb_d3dcompiler_open() and tb_d3dcompiler_compile() are thread-safe: any
 * number of compiles may run at once with one tb_d3dcompiler.
 * tb_d3dcompiler_release() is externally synchronized for the output it
 * releases, and tb_d3dcompiler_close() must not race any other use of its
 * compiler.
 */
#ifndef TB_D3DCOMPILER_H
#define TB_D3DCOMPILER_H

#include "badger/api.h"

#include <stddef.h>

/** @brief D3DCompiler, loaded. Opened by tb_d3dcompiler_open(); owned by the
 * caller; closed by tb_d3dcompiler_close(). */
typedef struct tb_d3dcompiler tb_d3dcompiler;

/** @brief What a call of this interface came to. */
typedef enum tb_d3dcompiler_result
{
    TB_D3DCOMPILER_OK, /**< Success; a compile's messages may still hold warnings. */
    /** The request is malformed: a required string missing or empty. */
    TB_D3DCOMPILER_INVALID,
    /** D3DCompiler is not on this machine, or not usable; the message says why. */
    TB_D3DCOMPILER_UNAVAILABLE,
    /** The compiler refused the source, and its messages say why. */
    TB_D3DCOMPILER_FAILED,
    /** The system, or the compiler, ran out of memory. */
    TB_D3DCOMPILER_NO_MEMORY
} tb_d3dcompiler_result;

/** @brief Bytes of a tb_d3dcompiler_error's message, its NUL included. */
#define TB_D3DCOMPILER_MESSAGE_CAPACITY 512

/** @brief Why a call failed, for the caller to report. A caller that only
 * wants the result passes NULL in place of one. */
typedef struct tb_d3dcompiler_error
{
    tb_d3dcompiler_result result; /**< As the call returned. */
    /** UTF-8, NUL-terminated, and empty on success: what went wrong. A
     * message too long for it is cut at a character. */
    char message[TB_D3DCOMPILER_MESSAGE_CAPACITY];
} tb_d3dcompiler_error;

/** @brief An error with no failure in it. */
#define TB_D3DCOMPILER_ERROR_INIT {TB_D3DCOMPILER_OK, {0}}

/** @brief Compile for debugging: without optimizing, with debug information
 * in the output (`/Od /Zi`). Without it, a compile optimizes fully (`/O3`). */
#define TB_D3DCOMPILER_DEBUG 1u

/** @brief What to compile. Zero-initialize, then fill in what is needed.
 *
 * The source has no `#include`, as the HLSL the shader module writes has
 * none: the compiler reads no file. */
typedef struct tb_d3dcompiler_request
{
    const char *source;   /**< HLSL in UTF-8; NULL only at length zero. Borrowed for the call. */
    size_t source_length; /**< Bytes of `source`. */
    /** Required; the name the compiler gives the source in its messages,
     * such as `Engine/Private/Main.hlsl`. NUL-terminated UTF-8. */
    const char *name;
    /** The profile: `vs_5_0`, `ps_5_0`, `cs_5_0`. Required, NUL-terminated. */
    const char *profile;
    /** The function to compile, NUL-terminated; required. */
    const char *entry_point;
    unsigned flags; /**< A combination of TB_D3DCOMPILER_DEBUG. */
} tb_d3dcompiler_request;

/** @brief The private part of an output, which only this interface reads. */
typedef struct tb_d3dcompiler_compiled tb_d3dcompiler_compiled;

/** @brief What a compile gave. Zero-initialize; tb_d3dcompiler_compile()
 * fills it in, and tb_d3dcompiler_release() releases it. */
typedef struct tb_d3dcompiler_output
{
    /** The DXBC container on OK, which D3D11 makes a shader of as it is;
     * NULL otherwise. Valid until tb_d3dcompiler_release(). */
    const void *code;
    size_t code_size; /**< Bytes of `code`. */
    /** The compiler's errors and warnings as it writes them, UTF-8, with a
     * NUL after them: `Main.hlsl(3,5-9): error X3004: ...`. "" when there
     * are none. Valid until tb_d3dcompiler_release(). */
    const char *messages;
    size_t messages_length;            /**< Bytes of `messages` before the NUL. */
    tb_d3dcompiler_compiled *compiled; /**< Private: what holds the rest. */
} tb_d3dcompiler_output;

/**
 * @brief Load D3DCompiler from the system directory.
 *
 * @param[out] compiler Required. Receives the compiler, which the caller
 * closes with tb_d3dcompiler_close(); NULL on failure.
 * @param[out] error Why it failed, or NULL.
 * @return TB_D3DCOMPILER_OK or UNAVAILABLE. Running out of memory ends the
 * process, as allocation does.
 * @par Threading Thread-safe.
 */
TB_API tb_d3dcompiler_result tb_d3dcompiler_open(tb_d3dcompiler **compiler,
                                                 tb_d3dcompiler_error *error);

/**
 * @brief Close a compiler tb_d3dcompiler_open() opened.
 *
 * Every output it gave must have been released first.
 *
 * @param compiler The compiler, which is no longer the caller's; or NULL,
 * which does nothing.
 * @par Threading Must not race any other use of `compiler`.
 */
TB_API void tb_d3dcompiler_close(tb_d3dcompiler *compiler);

/**
 * @brief Compile HLSL into DXBC.
 *
 * @param compiler Required, open.
 * @param request Required.
 * @param[out] output Required. Filled in on OK and FAILED, with the
 * compiler's messages; zero otherwise. Release it with
 * tb_d3dcompiler_release() in every case.
 * @param[out] error Why it failed, or NULL. On FAILED, the message is the
 * compiler's first error.
 * @return TB_D3DCOMPILER_OK, INVALID, FAILED, or NO_MEMORY.
 * @par Threading Thread-safe.
 */
TB_API tb_d3dcompiler_result tb_d3dcompiler_compile(tb_d3dcompiler *compiler,
                                                    const tb_d3dcompiler_request *request,
                                                    tb_d3dcompiler_output *output,
                                                    tb_d3dcompiler_error *error);

/**
 * @brief Release what a compile gave, and zero it.
 *
 * @param output Required; one tb_d3dcompiler_compile() filled in, or a zero
 * one, which this leaves alone.
 * @par Threading Externally synchronized for `output`.
 */
TB_API void tb_d3dcompiler_release(tb_d3dcompiler_output *output);

/** @brief Bytes of the text tb_d3dcompiler_version() gives at most, its NUL included. */
#define TB_D3DCOMPILER_VERSION_CAPACITY 128

/**
 * @brief Name the compiler loaded, by its library's file version:
 * `d3dcompiler_47 10.0.26100.1`.
 *
 * Two compilers that name themselves alike compile a source alike, so the
 * shader cache keys on this (badger/shader_cache.h). A library that tells
 * no version is named `d3dcompiler_47 unknown`.
 *
 * @param compiler Required, open.
 * @return UTF-8, NUL-terminated, shorter than TB_D3DCOMPILER_VERSION_CAPACITY;
 * owned by `compiler`, and valid until tb_d3dcompiler_close().
 * @par Threading Thread-safe.
 */
TB_API const char *tb_d3dcompiler_version(const tb_d3dcompiler *compiler);

/** @brief Name a result, for a log or a message: "unavailable".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe. */
TB_API const char *tb_d3dcompiler_result_name(tb_d3dcompiler_result result);

#endif
