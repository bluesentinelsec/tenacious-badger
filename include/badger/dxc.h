/** @file dxc.h
 * @brief DXC, Microsoft's HLSL compiler, loaded at run time: the one
 * compiler that writes DXIL, the bytecode D3D12 runs.
 *
 * The shader module (badger/shader.h) compiles a shader for D3D12 through
 * this interface; most callers want tb_shader_compile(), not this.
 *
 * @code
 * tb_dxc *dxc = NULL;
 * tb_dxc_error error = TB_DXC_ERROR_INIT;
 * if (tb_dxc_open(&dxc, &error) != TB_DXC_OK)
 * {
 *     report(error.message); // and fall back to a backend that needs no DXIL
 * }
 * @endcode
 *
 * **Where DXC comes from.** DXC is not part of Windows, and the engine does
 * not ship it. tb_dxc_open() loads `dxcompiler.dll`, and `dxil.dll`, which
 * signs what it compiles (D3D12 refuses a shader that is not signed), from
 * the same directory, the first of:
 * 1. the directory of the running executable, where a game or a tool puts
 *    its own copy;
 * 2. the Windows SDK's redistributable directory, `Redist\D3D\x64` (or
 *    `arm64`) under the SDK's root, which the registry names (the value
 *    `KitsRoot10` of `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots`).
 *
 * Development builds therefore compile shaders at run time on any machine
 * with the Windows SDK installed, and a shipped game, whose shaders are
 * compiled when it is built, needs no DXC at all.
 *
 * **Unavailable, never fatal.** A DXC that is not found, or a directory that
 * has one of the two libraries and not the other, makes tb_dxc_open()
 * return TB_DXC_UNAVAILABLE with a message that says where it looked and
 * what to do: a capability the machine lacks, which the renderer reports and
 * falls back from. On Linux and macOS, DXC is always unavailable: DXIL runs
 * only on D3D12, which only Windows has, so these systems lack the
 * capability rather than the work. (DXC builds for Linux too; compiling
 * DXIL there, to cook a Windows game, can come later.) A system the engine
 * does not support reports "not implemented", as every platform interface
 * does there.
 *
 * @par Threading
 * tb_dxc_open() and tb_dxc_compile() are thread-safe: each compile makes a
 * compiler of its own, so any number may run at once with one tb_dxc.
 * tb_dxc_release() is externally synchronized for the output it releases,
 * and tb_dxc_close() must not race any other use of its DXC.
 */
#ifndef TB_DXC_H
#define TB_DXC_H

#include "badger/api.h"

#include <stddef.h>

/** @brief DXC, loaded. Opened by tb_dxc_open(); owned by the caller; closed
 * by tb_dxc_close(). */
typedef struct tb_dxc tb_dxc;

/** @brief What a call of this interface came to. */
typedef enum tb_dxc_result
{
    TB_DXC_OK, /**< Success; a compile's messages may still hold warnings. */
    /** The request is malformed: a required string missing or empty. */
    TB_DXC_INVALID,
    /** DXC is not on this machine, or not usable; the message says why. */
    TB_DXC_UNAVAILABLE,
    /** DXC refused the source, and its messages say why. */
    TB_DXC_FAILED,
    /** The system, or DXC, ran out of memory. */
    TB_DXC_NO_MEMORY
} tb_dxc_result;

/** @brief Bytes of a tb_dxc_error's message, its NUL included. */
#define TB_DXC_MESSAGE_CAPACITY 512

/** @brief Why a call failed, for the caller to report. A caller that only
 * wants the result passes NULL in place of one. */
typedef struct tb_dxc_error
{
    tb_dxc_result result; /**< As the call returned. */
    /** UTF-8, NUL-terminated, and empty on success: what went wrong, and,
     * for an unavailable DXC, where it was looked for and what to do. A
     * message too long for it is cut at a character. */
    char message[TB_DXC_MESSAGE_CAPACITY];
} tb_dxc_error;

/** @brief An error with no failure in it. */
#define TB_DXC_ERROR_INIT {TB_DXC_OK, {0}}

/** @brief Compile for debugging: without optimizing (`-Od`), with the
 * debug information embedded in the output (`-Zi -Qembed_debug`), which
 * PIX reads. Without it, a compile optimizes fully (`-O3`). */
#define TB_DXC_DEBUG 1u

/** @brief What to compile. Zero-initialize, then fill in what is needed.
 *
 * Every compile is HLSL 2021 (`-HV 2021`). The source has no `#include`, as
 * the shader module's preprocessed text has none: DXC reads no file. */
typedef struct tb_dxc_request
{
    const char *source;   /**< HLSL in UTF-8; NULL only at length zero. Borrowed for the call. */
    size_t source_length; /**< Bytes of `source`. */
    /** Required; the name DXC gives the source in its messages and its
     * debug information, such as `Engine/Private/Main.hlsl`. NUL-terminated,
     * UTF-8, and not starting with `-` or `/`, which DXC reads as an option. */
    const char *name;
    /** The profile, as `-T` takes it: `ps_6_6`, `cs_6_8`, `lib_6_6`.
     * Required, NUL-terminated. */
    const char *profile;
    /** The function to compile, as `-E` takes it, NUL-terminated; required
     * but for a library profile (`lib_*`), which compiles every entry point
     * and ignores it. */
    const char *entry_point;
    unsigned flags; /**< A combination of TB_DXC_DEBUG. */
} tb_dxc_request;

/** @brief The private part of an output, which only this interface reads. */
typedef struct tb_dxc_compiled tb_dxc_compiled;

/** @brief What a compile gave. Zero-initialize; tb_dxc_compile() fills it
 * in, and tb_dxc_release() releases it. */
typedef struct tb_dxc_output
{
    /** The DXIL container, signed, on OK: a DXBC container whose parts
     * include `DXIL`, the signatures and `HASH`. NULL otherwise. Valid until
     * tb_dxc_release(). */
    const void *code;
    size_t code_size; /**< Bytes of `code`. */
    /** DXC's errors and warnings as it writes them, UTF-8, with a NUL after
     * them: `Main.hlsl:3:5: error: ...`. "" when there are none. Valid until
     * tb_dxc_release(). */
    const char *messages;
    size_t messages_length;    /**< Bytes of `messages` before the NUL. */
    tb_dxc_compiled *compiled; /**< Private: what holds the rest. */
} tb_dxc_output;

/**
 * @brief Load DXC.
 *
 * Looks where the file comment says, and loads `dxcompiler.dll` and
 * `dxil.dll` from the first directory that has both.
 *
 * @param[out] dxc Required. Receives DXC, which the caller closes with
 * tb_dxc_close(); NULL on failure.
 * @param[out] error Why it failed, or NULL.
 * @return TB_DXC_OK or UNAVAILABLE. Running out of memory ends the process,
 * as allocation does.
 * @par Threading Thread-safe.
 */
TB_API tb_dxc_result tb_dxc_open(tb_dxc **dxc, tb_dxc_error *error);

/**
 * @brief Close DXC, which tb_dxc_open() opened.
 *
 * Every output it gave must have been released first.
 *
 * @param dxc DXC, which is no longer the caller's; or NULL, which does
 * nothing.
 * @par Threading Must not race any other use of `dxc`.
 */
TB_API void tb_dxc_close(tb_dxc *dxc);

/**
 * @brief Compile HLSL into DXIL.
 *
 * @param dxc Required, open.
 * @param request Required.
 * @param[out] output Required. Filled in on OK and FAILED, with DXC's
 * messages; zero otherwise. Release it with tb_dxc_release() in every case.
 * @param[out] error Why it failed, or NULL. On FAILED, the message is DXC's
 * first error, the first that names a place where one does.
 * @return TB_DXC_OK, INVALID, FAILED, or NO_MEMORY.
 * @par Threading Thread-safe.
 */
TB_API tb_dxc_result tb_dxc_compile(tb_dxc *dxc, const tb_dxc_request *request,
                                    tb_dxc_output *output, tb_dxc_error *error);

/**
 * @brief Release what a compile gave, and zero it.
 *
 * @param output Required; one tb_dxc_compile() filled in, or a zero one,
 * which this leaves alone.
 * @par Threading Externally synchronized for `output`.
 */
TB_API void tb_dxc_release(tb_dxc_output *output);

/** @brief Bytes of the text tb_dxc_version() gives at most, its NUL included. */
#define TB_DXC_VERSION_CAPACITY 256

/**
 * @brief Name the DXC loaded: the version of its compiler and of its
 * validator, which signs, each with the number and the hash of the commit
 * it was built from where it tells them (IDxcVersionInfo2), as the Windows
 * SDK's compiler does and its validator does not:
 * `dxcompiler 1.8 (4792, 23992152) dxil 1.8`.
 *
 * Two DXCs that name themselves alike compile a source alike, so the shader
 * cache keys on this (badger/shader_cache.h). A part DXC does not tell is
 * named `unknown`.
 *
 * @param dxc Required, open.
 * @return UTF-8, NUL-terminated, shorter than TB_DXC_VERSION_CAPACITY;
 * owned by `dxc`, and valid until tb_dxc_close().
 * @par Threading Thread-safe.
 */
TB_API const char *tb_dxc_version(const tb_dxc *dxc);

/** @brief Name a result, for a log or a message: "unavailable".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe. */
TB_API const char *tb_dxc_result_name(tb_dxc_result result);

#endif
