/** @file library.h
 * @brief Shared libraries loaded at run time, and the functions and data
 * they export.
 *
 * The engine reaches the system's graphics, windowing, and audio libraries
 * (Direct3D, Vulkan, OpenGL, Metal, X11, Wayland, and others) and the vendor
 * libraries (DLSS, FSR, XeSS, Reflex) through this interface rather than by
 * linking them. A program then starts on a machine that lacks one, and the
 * engine chooses what to do without it.
 *
 * Every load says where it looks: in the system's libraries, beside the
 * executable, or at one exact path. None of them uses the system's default
 * search order, so a library of the same name planted in the working
 * directory is never loaded in place of the one meant. Implementations are
 * build-selected: LoadLibraryExW on Windows, dlopen on Linux and macOS.
 *
 * Every failure comes back to the caller as a result, with the system's own
 * words in a message; nothing here ends the process. A missing library is
 * an unavailable capability, which the caller reports and falls back from.
 *
 * @par Threading
 * Opening a library and finding its symbols are thread-safe. Closing a
 * library must not race any other use of it.
 */
#ifndef TB_LIBRARY_H
#define TB_LIBRARY_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief A loaded shared library. Opaque; opened by tb_library_open() and
 * owned by the caller until tb_library_close(). */
typedef struct tb_library tb_library;

/** @brief A function found in a library, of no particular type.
 *
 * Convert it to the function's real type before calling it. Standard C
 * converts one function pointer type to another, but not an object pointer
 * to a function pointer, which is why functions and data are found by
 * separate calls. */
typedef void (*tb_library_function)(void);

/** @brief Where tb_library_open() looks for a library. */
typedef enum tb_library_search
{
    /** The system's own libraries. The name is a file name, with no
     * directory:
     * - **Windows:** the system directory (System32), as `d3d12.dll`.
     * - **Linux:** the dynamic linker's search, as `libX11.so.6`. Name a
     *   library by its major version: the unversioned `libX11.so` is a file
     *   of the development packages, which a player's system may lack, and
     *   may be a version the caller was not written for. The search includes
     *   `LD_LIBRARY_PATH` and the run path of the engine library, as the
     *   dynamic linker's does.
     * - **macOS:** `/usr/lib`, as `libobjc.A.dylib`, or for a name of the
     *   form `Name.framework/Name`, `/System/Library/Frameworks`, as
     *   `Metal.framework/Metal`. */
    TB_LIBRARY_SEARCH_SYSTEM,
    /** The directory of the running executable, for libraries that ship with
     * the program. The name is a file name, with no directory. On Windows,
     * the libraries it depends on are found beside it or in the system
     * directory. */
    TB_LIBRARY_SEARCH_APPLICATION,
    /** Exactly the absolute path given, in the system's syntax; a relative
     * path is refused. On Windows, the libraries it depends on are found
     * beside it or in the system directory. */
    TB_LIBRARY_SEARCH_PATH
} tb_library_search;

/** @brief What a call of this interface came to. */
typedef enum tb_library_result
{
    TB_LIBRARY_OK, /**< Success. */
    /** The name cannot be used with its search: empty, not valid UTF-8, a
     * directory where a file name is required, or a relative path for
     * TB_LIBRARY_SEARCH_PATH. Nothing was looked for. */
    TB_LIBRARY_INVALID,
    /** The library could not be loaded: it is not where the search looks, or
     * it is there but unusable, built for another architecture, needing a
     * library that is missing, or needing a symbol that no loaded library
     * has. The message says which, in the system's words. */
    TB_LIBRARY_UNAVAILABLE,
    /** The library does not export a symbol of that name. */
    TB_LIBRARY_SYMBOL_MISSING,
    /** The system ran out of memory, or of another resource. */
    TB_LIBRARY_NO_MEMORY
} tb_library_result;

/** @brief Bytes of a tb_library_error's message, its NUL included. */
#define TB_LIBRARY_MESSAGE_CAPACITY 512

/** @brief Why a call failed, for the caller to report.
 *
 * A caller that only wants the result passes NULL in place of one. */
typedef struct tb_library_error
{
    tb_library_result result; /**< As the call returned. */
    /** UTF-8, NUL-terminated, and empty on success. It names the library or
     * symbol and says what went wrong, in the system's words where it gave
     * any: "libX11.so.6: not loaded from the system's libraries:
     * libX11.so.6: cannot open shared object file: No such file or
     * directory". A message too long for it is cut at a character. */
    char message[TB_LIBRARY_MESSAGE_CAPACITY];
} tb_library_error;

/** @brief An error with no failure in it. */
#define TB_LIBRARY_ERROR_INIT {TB_LIBRARY_OK, {0}}

/** @brief A symbol for tb_library_bind() to find, and where to put it. */
typedef struct tb_library_binding
{
    const char *name;              /**< The exported name; required. */
    tb_library_function *function; /**< Receives the function; required. */
    /** Whether the binding fails without it. A symbol that is not required
     * and is missing gets NULL, for a caller to check before calling it:
     * a function that a newer version of the library added, for one. */
    bool required;
} tb_library_binding;

/**
 * @brief Load a shared library.
 *
 * A library that is already loaded is not loaded twice: the system counts
 * the opens, and each must be closed.
 *
 * @param name The library: a UTF-8 file name, or for TB_LIBRARY_SEARCH_PATH
 * an absolute path. Required, and only borrowed.
 * @param search Where to look; see tb_library_search.
 * @param[out] library Required. Receives the library, which the caller
 * closes with tb_library_close(); NULL on failure.
 * @param[out] error Why it failed, or NULL.
 * @return TB_LIBRARY_OK, INVALID, UNAVAILABLE, or NO_MEMORY.
 * @par Threading Thread-safe.
 */
TB_API tb_library_result tb_library_open(const char *name, tb_library_search search,
                                         tb_library **library, tb_library_error *error);

/**
 * @brief Close a library that tb_library_open() opened.
 *
 * Every function and datum found in it may stop being valid, once the
 * system unloads the library: when the last of its opens is closed, if the
 * system unloads libraries at all.
 *
 * @param library The library, which is no longer the caller's; or NULL,
 * which does nothing.
 * @par Threading Must not race any other use of the library.
 */
TB_API void tb_library_close(tb_library *library);

/**
 * @brief Find a function that a library exports.
 *
 * @param library Required, open.
 * @param name The exported name; required, and only borrowed.
 * @param[out] function Required. Receives the function, valid until the
 * library is closed; NULL on failure.
 * @param[out] error Why it failed, or NULL.
 * @return TB_LIBRARY_OK or SYMBOL_MISSING. A symbol whose address is zero
 * is missing.
 * @par Threading Thread-safe.
 */
TB_API tb_library_result tb_library_find_function(const tb_library *library, const char *name,
                                                  tb_library_function *function,
                                                  tb_library_error *error);

/**
 * @brief Find a variable or other datum that a library exports.
 *
 * @param library Required, open.
 * @param name The exported name; required, and only borrowed.
 * @param[out] data Required. Receives the datum's address, valid until the
 * library is closed; NULL on failure.
 * @param[out] error Why it failed, or NULL.
 * @return TB_LIBRARY_OK or SYMBOL_MISSING. A symbol whose address is zero
 * is missing.
 * @par Threading Thread-safe.
 */
TB_API tb_library_result tb_library_find_data(const tb_library *library, const char *name,
                                              void **data, tb_library_error *error);

/**
 * @brief Find a table of functions at once.
 *
 * This is how a caller loads an interface of a library: Wayland's client
 * functions, say, into a table of function pointers. Every binding is
 * looked for, so the message of a failure names every required function
 * that is missing, not only the first: "libwayland-client.so.0: missing
 * wl_proxy_marshal_flags, wl_proxy_get_version".
 *
 * @param library Required, open.
 * @param bindings The symbols, and where each goes; NULL only at count zero.
 * @param count Number of bindings.
 * @param[out] error Why it failed, or NULL.
 * @return TB_LIBRARY_OK when every required function was found, else
 * SYMBOL_MISSING. Either way, every binding's function is set: to the
 * function, or to NULL where it is missing.
 * @par Threading Thread-safe, when no two calls write the same function.
 */
TB_API tb_library_result tb_library_bind(const tb_library *library,
                                         const tb_library_binding *bindings, size_t count,
                                         tb_library_error *error);

/** @brief Name a result, for a log or a message: "symbol missing".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe. */
TB_API const char *tb_library_result_name(tb_library_result result);

#endif
