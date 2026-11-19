/** @file vfs.h
 * @brief The virtual file system: one namespace of game files, made of
 * mounted directories, archive files, and archives embedded in the
 * executable, through which the engine opens every asset.
 *
 * The game asks for `textures/rock.tex`; the VFS finds it in whichever mount
 * has it and returns a stream. Three kinds of mount are built in, and any
 * other is a tb_vfs_source of its own:
 * - **a directory** on disk, read live, so files changed or added after it
 *   is mounted are seen (for development and mods);
 * - **an archive file**: a package (badger/package.h), a ZIP archive, or a
 *   TAR archive, recognized by its bytes;
 * - **an archive in memory**, such as one embedded in the executable, which
 *   needs no file system at all.
 *
 * **Priorities.** Each mount has a priority. Where two mounts have a name,
 * the one of higher priority wins, and of equal priority, the one mounted
 * later: a mod mounted over the base game replaces its files. A directory
 * lists the names of every mount, each once.
 *
 * **Names** are UTF-8, case-sensitive, with `/` between components, such as
 * `textures/rock.tex`. A name is refused, as INVALID, if it has an empty,
 * `.`, or `..` component, a `\`, a `:`, or a NUL, so no name leaves its
 * mount or means something else on another system. The empty name is the
 * root, which only tb_vfs_query() and tb_vfs_list() take. An archive is not
 * mounted if any of its entries has a name the VFS would refuse.
 *
 * @par Threading
 * Mounting and destroying are externally synchronized with every other
 * call: mount during setup, then read. Once mounted, tb_vfs_open(),
 * tb_vfs_query(), and tb_vfs_list() are thread-safe, and any number of
 * threads may read at once. Each stream that tb_vfs_open() gives is the
 * caller's. tb_vfs_result_name() is thread-safe.
 */
#ifndef TB_VFS_H
#define TB_VFS_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

/** @brief What happened, or why not. */
typedef enum tb_vfs_result
{
    TB_VFS_OK,        /**< Success. */
    TB_VFS_INVALID,   /**< A name or mount point the VFS refuses, or a path that is not absolute. */
    TB_VFS_NOT_FOUND, /**< No mount has the name. */
    TB_VFS_NOT_A_FILE,      /**< The name is a directory. */
    TB_VFS_NOT_A_DIRECTORY, /**< The name is a file, or a path given to mount is not a directory. */
    TB_VFS_UNKNOWN_FORMAT,  /**< The bytes are not a package, a ZIP archive, or a TAR archive. */
    TB_VFS_UNSAFE_NAME,     /**< An archive has an entry whose name the VFS refuses. */
    TB_VFS_MALFORMED, /**< An archive is damaged, or has a name that is both a file and a directory.
                       */
    TB_VFS_UNSUPPORTED, /**< Valid, but not something the VFS mounts or reads, such as a .tar.gz. */
    TB_VFS_KEY_REQUIRED, /**< A package is obfuscated, and no key was given. */
    TB_VFS_WRONG_KEY,    /**< The key given is not the package's. */
    TB_VFS_INPUT_FAILED  /**< The system refused to read: a file is missing, locked, or unreadable.
                          */
} tb_vfs_result;

/** @brief What a name is. */
typedef enum tb_vfs_kind
{
    TB_VFS_FILE,     /**< A file, which tb_vfs_open() reads. */
    TB_VFS_DIRECTORY /**< A directory, which tb_vfs_list() lists. */
} tb_vfs_kind;

/** @brief What tb_vfs_query() and tb_vfs_list() tell of a name. */
typedef struct tb_vfs_info
{
    tb_vfs_kind kind; /**< What it is. */
    uint64_t size;    /**< Bytes of a file; 0 for a directory. */
} tb_vfs_info;

/**
 * @brief Called once for each name in a directory.
 * @param context What was given to tb_vfs_list().
 * @param name The name within the directory, one component; valid during the call.
 * @param length Bytes of `name`.
 * @param info What it is.
 */
typedef void tb_vfs_list_function(void *context, const char *name, size_t length,
                                  const tb_vfs_info *info);

/* -------------------------------------------------------------------------
 * Sources: what a mount reads from.
 * ------------------------------------------------------------------------- */

/**
 * @brief What a kind of mount does. Names given to it are relative to its
 * mount point and already checked; the empty name is its root. Every
 * function but `release` must be thread-safe.
 */
typedef struct tb_vfs_source_functions
{
    /** Open a file of the source for reading. NOT_FOUND, NOT_A_FILE, or another failure. */
    tb_vfs_result (*open_file)(void *state, const char *name, size_t length,
                               const tb_allocator *allocator, tb_stream *stream);
    /** Tell what a name is: OK, NOT_FOUND, or another failure. The root is a directory. */
    tb_vfs_result (*query)(void *state, const char *name, size_t length, tb_vfs_info *info);
    /** Call `function` for each name in a directory: OK, NOT_FOUND, NOT_A_DIRECTORY, or another
     * failure. */
    tb_vfs_result (*list)(void *state, const char *directory, size_t length,
                          tb_vfs_list_function *function, void *context);
    /** Release the state; called once, when the VFS is destroyed. */
    void (*release)(void *state);
} tb_vfs_source_functions;

/** @brief A source: what it does, and the state it does it to. */
typedef struct tb_vfs_source
{
    const tb_vfs_source_functions *functions; /**< The kind's table. */
    void *state;                              /**< Passed to every function. */
} tb_vfs_source;

/* -------------------------------------------------------------------------
 * The VFS.
 * ------------------------------------------------------------------------- */

/** @brief A virtual file system. Opaque. */
typedef struct tb_vfs tb_vfs;

/**
 * @brief Create an empty VFS.
 * @param allocator Where the VFS and its mounts live, or NULL for the heap.
 * @return The VFS, for tb_vfs_destroy().
 * @par Errors Running out of memory ends the process.
 * @par Threading Thread-safe; the VFS is the caller's.
 */
TB_API tb_vfs *tb_vfs_create(const tb_allocator *allocator);

/**
 * @brief Destroy a VFS and release every mount. Streams it gave must be closed first.
 * @param vfs Optional; NULL does nothing.
 */
TB_API void tb_vfs_destroy(tb_vfs *vfs);

/**
 * @brief Mount a source of one's own.
 * @param vfs Required.
 * @param point Where in the namespace: a name, or "" for the root.
 * @param point_length Bytes of `point`.
 * @param source The source; on OK the VFS owns it and releases it when destroyed.
 * @param priority Higher wins; of equal priorities, the later mount wins.
 * @return OK, or INVALID for a mount point the VFS refuses, when the source
 * is still the caller's.
 */
TB_API tb_vfs_result tb_vfs_mount(tb_vfs *vfs, const char *point, size_t point_length,
                                  tb_vfs_source source, int priority);

/**
 * @brief Mount a directory on disk, read live.
 *
 * Names below it are joined to it with tb_path_rooted_join(), and links are
 * not followed: a name whose file, or any directory on the way to it, is a
 * link, is NOT_FOUND. Such a mount sees files added after it is made.
 * @param vfs Required. @param point As tb_vfs_mount(). @param point_length Bytes of `point`.
 * @param directory Required; the absolute UTF-8 path of an existing directory. Copied.
 * @param priority As tb_vfs_mount().
 * @return OK; INVALID for a mount point or a path that is not absolute;
 * NOT_FOUND or NOT_A_DIRECTORY for the path; or INPUT_FAILED.
 */
TB_API tb_vfs_result tb_vfs_mount_directory(tb_vfs *vfs, const char *point, size_t point_length,
                                            const char *directory, int priority);

/**
 * @brief Mount an archive file: a package, a ZIP archive, or a TAR archive.
 *
 * The file stays open, and is read at offsets, until the VFS is destroyed.
 * Every entry name is checked first; a ZIP or TAR entry that is a link, a
 * device, or another kind that is neither a file nor a directory is left out.
 * @param vfs Required. @param point As tb_vfs_mount(). @param point_length Bytes of `point`.
 * @param path Required; the UTF-8 path of the file.
 * @param key For an obfuscated package, its game's key of TB_PACKAGE_KEY_SIZE
 * bytes (badger/package.h); NULL otherwise. Copied.
 * @param priority As tb_vfs_mount().
 * @return OK; INVALID; NOT_FOUND or INPUT_FAILED for the file; UNKNOWN_FORMAT;
 * UNSUPPORTED for a compressed TAR; UNSAFE_NAME; MALFORMED; KEY_REQUIRED; or WRONG_KEY.
 */
TB_API tb_vfs_result tb_vfs_mount_archive_file(tb_vfs *vfs, const char *point, size_t point_length,
                                               const char *path, const uint8_t *key, int priority);

/**
 * @brief Mount an archive held in memory, such as one embedded in the
 * executable, as tb_vfs_mount_archive_file() mounts a file.
 * @param vfs Required. @param point As tb_vfs_mount(). @param point_length Bytes of `point`.
 * @param bytes The archive, borrowed: it must stay valid and unchanged until
 * the VFS is destroyed. NULL only at length zero.
 * @param length Bytes of `bytes`.
 * @param key As tb_vfs_mount_archive_file().
 * @param priority As tb_vfs_mount().
 * @return As tb_vfs_mount_archive_file(), but never NOT_FOUND.
 */
TB_API tb_vfs_result tb_vfs_mount_archive_memory(tb_vfs *vfs, const char *point,
                                                 size_t point_length, const void *bytes,
                                                 size_t length, const uint8_t *key, int priority);

/**
 * @brief Open a file for reading, from the mount of highest priority that has it.
 * @param vfs Required. @param name The file's name. @param length Bytes of `name`.
 * @param allocator Where the stream's state lives, or NULL for the heap.
 * @param[out] stream Required zero stream; open on OK.
 * @return OK; INVALID; NOT_FOUND; NOT_A_FILE; or the mount's failure to open it.
 */
TB_API tb_vfs_result tb_vfs_open(tb_vfs *vfs, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream);

/**
 * @brief Tell whether a name exists, and what it is.
 *
 * A file is told by the mount of highest priority that has it. A directory
 * exists where any mount has it, or where a mount's point lies below it.
 * @param vfs Required. @param name A name, or "" for the root. @param length Bytes of `name`.
 * @param[out] info Required; on OK, what the name is.
 * @return OK, INVALID, NOT_FOUND, or a mount's failure.
 */
TB_API tb_vfs_result tb_vfs_query(tb_vfs *vfs, const char *name, size_t length, tb_vfs_info *info);

/**
 * @brief List a directory: every name in it, from every mount, each once,
 * as the mount of highest priority tells it, in the order of their bytes.
 * @param vfs Required. @param directory A name, or "" for the root.
 * @param length Bytes of `directory`.
 * @param function Required; called once for each name.
 * @param context Passed to `function`.
 * @return OK; INVALID; NOT_FOUND if no mount has the directory;
 * NOT_A_DIRECTORY if the name is a file; or a mount's failure.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_vfs_result tb_vfs_list(tb_vfs *vfs, const char *directory, size_t length,
                                 tb_vfs_list_function *function, void *context);

/** @brief A short name of a result, such as "not found". Thread-safe. */
TB_API const char *tb_vfs_result_name(tb_vfs_result result);

#endif
