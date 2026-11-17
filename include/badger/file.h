/** @file file.h
 * @brief Native synchronous binary files and directories.
 *
 * Paths are bounded UTF-8 without embedded NUL; NULL is valid only at length
 * zero, which is an invalid filename. Native path rules apply. No process
 * working directory is changed. Relative paths require callers to keep it
 * stable. Calls on distinct handles are thread-safe; a handle and its cursor
 * are externally synchronized. Close must not race any use. Handles are
 * move-only values: copying does not duplicate ownership. Zero means closed.
 * Native resource exhaustion is returned as TB_FS_NO_MEMORY.
 */
#ifndef TB_FILE_H
#define TB_FILE_H
#include "badger/api.h"
#include "badger/path.h"
#include <stdint.h>

/** @brief Portable filesystem failure categories. */
typedef enum tb_fs_result
{
    TB_FS_OK,            /**< Success. */
    TB_FS_NOT_FOUND,     /**< A name or parent does not exist. */
    TB_FS_EXISTS,        /**< Exclusive creation found an existing name. */
    TB_FS_ACCESS,        /**< Permission denied, read-only filesystem, or sharing violation. */
    TB_FS_NOT_DIRECTORY, /**< A required directory is a different kind. */
    TB_FS_IS_DIRECTORY,  /**< A regular file was required. */
    TB_FS_NOT_EMPTY,     /**< Directory removal found children. */
    TB_FS_INVALID,       /**< Malformed UTF-8, path, flags, or offset. */
    TB_FS_NO_SPACE,      /**< Disk full, quota exhausted, or output buffer too small. */
    TB_FS_NO_MEMORY,     /**< Native resource allocation failed. */
    TB_FS_TOO_LARGE,     /**< Path, size, or offset exceeds a native limit. */
    TB_FS_IO,            /**< Other native I/O failure. */
    TB_FS_END            /**< Enumeration exhausted; not a read error. */
} tb_fs_result;
/** @brief Owned native file token. Initialize to zero; do not inspect value. */
typedef struct tb_file
{
    uintptr_t value; /**< Private token; zero is closed. */
} tb_file;
/** @brief Owned directory iterator. Initialize to zero; do not copy while open. */
typedef struct tb_directory
{
    void *state; /**< Private iterator state. */
} tb_directory;
/** @brief File opening flags. Read/write may be combined; create requires write. */
typedef enum tb_file_flags
{
    TB_FILE_READ = 1,      /**< Read access. */
    TB_FILE_WRITE = 2,     /**< Write access. */
    TB_FILE_CREATE = 4,    /**< Create if missing. */
    TB_FILE_EXCLUSIVE = 8, /**< With CREATE, fail if name exists. */
    TB_FILE_TRUNCATE = 16  /**< With WRITE, truncate after opening. */
} tb_file_flags;
/** @brief Seek origin; negative resulting positions are errors. */
typedef enum tb_file_origin
{
    TB_FILE_START,   /**< Beginning. */
    TB_FILE_CURRENT, /**< Cursor. */
    TB_FILE_END      /**< Current end. */
} tb_file_origin;
/** @brief Filesystem object kind; queries do not follow symlinks/reparse points. */
typedef enum tb_fs_kind
{
    TB_FS_REGULAR,   /**< Regular file. */
    TB_FS_DIRECTORY, /**< Directory. */
    TB_FS_LINK,      /**< Link or reparse point. */
    TB_FS_OTHER      /**< Another native kind. */
} tb_fs_kind;
/** @brief Metadata snapshot; later operations may observe changes. */
typedef struct tb_fs_info
{
    tb_fs_kind kind;               /**< Object kind. */
    uint64_t size;                 /**< Bytes for regular files, unspecified for other kinds. */
    int64_t modified_seconds;      /**< UTC seconds since Unix epoch. */
    uint32_t modified_nanoseconds; /**< Fractional seconds, in [0, 1000000000). */
} tb_fs_info;
/** @brief A per-user base directory or the directory containing this executable. */
typedef enum tb_fs_location
{
    TB_FS_EXECUTABLE,  /**< Executable directory. */
    TB_FS_USER_DATA,   /**< Persistent application data base. */
    TB_FS_USER_CONFIG, /**< Configuration base. */
    TB_FS_USER_CACHE,  /**< Disposable cache base. */
    TB_FS_TEMP         /**< Temporary directory base. */
} tb_fs_location;

/** @brief Open a regular file; output must be zero and stays zero on failure.
 * @param path Borrowed UTF-8 filename.
 * @param flags Bitwise TB_FILE_* combination; no implicit locking or append.
 * @param[out] file Required owned output. Close with tb_file_close.
 * @return Status. Windows shares read, write and delete; POSIX uses no advisory
 * locks. Handles are non-inheritable. Newly created POSIX files use 0600.
 * Existing links are followed by open; use trusted paths for sensitive I/O.
 */
TB_API tb_fs_result tb_file_open(tb_path_view path, unsigned flags, tb_file *file);
/** @brief Read up to capacity bytes; EOF succeeds with zero bytes.
 * @param file Required open readable handle; cursor advances by bytes read.
 * @param bytes Output buffer, NULL only at capacity zero.
 * @param capacity Maximum bytes to read.
 * @param[out] transferred Required; always receives bytes completed, even on error.
 * @return Status; short reads are allowed. Buffer prefix is valid on error.
 */
TB_API tb_fs_result tb_file_read(tb_file *file, void *bytes, size_t capacity, size_t *transferred);
/** @brief Write up to length bytes; caller must handle short writes.
 * @param file Required open writable handle; cursor advances by completed bytes.
 * @param bytes Borrowed input, NULL only at length zero.
 * @param length Input bytes.
 * @param[out] transferred Required; completed prefix length even on error.
 * @return Status. Data is not durable until a successful flush.
 */
TB_API tb_fs_result tb_file_write(tb_file *file, const void *bytes, size_t length,
                                  size_t *transferred);
/** @brief Seek a regular file; seeking beyond EOF does not extend it.
 * @param file Required open handle.
 * @param offset Signed byte displacement.
 * @param origin Starting point.
 * @param[out] position Required resulting offset; unchanged on failure.
 * @return Status; negative positions and values above INT64_MAX are rejected.
 */
TB_API tb_fs_result tb_file_seek(tb_file *file, int64_t offset, tb_file_origin origin,
                                 uint64_t *position);
/** @brief Read the cursor. Required output is unchanged on failure.
 * @param file Required open handle. @param[out] position Required cursor output.
 * @return Status. */
TB_API tb_fs_result tb_file_tell(tb_file *file, uint64_t *position);
/** @brief Query byte size without changing the cursor.
 * @param file Required open handle. @param[out] size Required; unchanged on failure.
 * @return Status. */
TB_API tb_fs_result tb_file_size(tb_file *file, uint64_t *size);
/** @brief Flush file data and metadata to the OS storage contract.
 * @param file Required open handle. @return Status; hardware may have weaker guarantees. */
TB_API tb_fs_result tb_file_flush(tb_file *file);
/** @brief Close and zero a handle even when close reports an error.
 * @param file Required; a zero handle is accepted. @return Status; never retry a failed close. */
TB_API tb_fs_result tb_file_close(tb_file *file);
/** @brief Query a path without following its final symlink/reparse point.
 * @param path Borrowed UTF-8 name. @param[out] info Required; unchanged on error.
 * @return NOT_FOUND means absent; ACCESS does not imply absence. */
TB_API tb_fs_result tb_fs_query(tb_path_view path, tb_fs_info *info);
/** @brief Create one directory, with POSIX mode 0700. Parents must exist.
 * @param path Borrowed UTF-8 name. @return EXISTS for any existing name. */
TB_API tb_fs_result tb_fs_make_directory(tb_path_view path);
/** @brief Remove one file/link, or one empty directory; never recurse.
 * @param path Borrowed UTF-8 name. @return Status. */
TB_API tb_fs_result tb_fs_remove(tb_path_view path);
/** @brief Atomically rename within one filesystem, replacing an existing file.
 * @param source Existing UTF-8 name. @param destination New UTF-8 name.
 * @return Status; cross-device moves fail. Directory replacement is not portable.
 * Atomic visibility is distinct from durability after power loss. */
TB_API tb_fs_result tb_fs_rename(tb_path_view source, tb_path_view destination);
/** @brief Begin directory enumeration; output must be zero.
 * @param path UTF-8 directory. @param[out] directory Required owned iterator.
 * @return Status; order is unspecified and concurrent mutations are unspecified.
 * Iterator does not follow entries. Close with tb_directory_close. */
TB_API tb_fs_result tb_directory_open(tb_path_view path, tb_directory *directory);
/** @brief Read the next name, excluding dot and dot-dot.
 * @param directory Required open iterator.
 * @param name Output buffer or NULL with zero capacity to measure.
 * @param capacity Bytes available including NUL.
 * @param[out] required Required bytes including NUL, zero on END or other errors.
 * @return OK consumes entry, except measurement retains it. NO_SPACE retains
 * entry and leaves buffer untouched. Invalid native Unicode returns INVALID
 * and consumes that entry so enumeration can continue. */
TB_API tb_fs_result tb_directory_next(tb_directory *directory, char *name, size_t capacity,
                                      size_t *required);
/** @brief Close and zero an iterator; zero is accepted.
 * @param directory Required owned iterator. @return Status; consumed even on error. */
TB_API tb_fs_result tb_directory_close(tb_directory *directory);
/** @brief Get an absolute UTF-8 base path without creating it.
 * @param location Location to query. @param destination Output or NULL to measure.
 * @param capacity Bytes including NUL. @param[out] required Required bytes
 * including NUL on OK/NO_SPACE, zero otherwise. Output unchanged on error.
 * @return Status. Environment-dependent locations require stable environment.
 * Append an application identifier with tb_path_rooted_join before use. */
TB_API tb_fs_result tb_fs_get_location(tb_fs_location location, char *destination, size_t capacity,
                                       size_t *required);
/** @brief Create a unique empty file in a supplied existing directory.
 * @param directory Existing directory; NULL/zero selects system temp directory.
 * @param path Output path; must be non-NULL.
 * @param capacity Bytes available including NUL; at least directory bytes+64
 * (or 32768 for the system default) is recommended. No object created on NO_SPACE.
 * @param[out] file Required zero handle, owned on success. Remove path separately.
 * @return Status. File is exclusively created; temporary is a lifetime convention. */
TB_API tb_fs_result tb_fs_temp_file(tb_path_view directory, char *path, size_t capacity,
                                    tb_file *file);
/** @brief Create a unique empty directory, removed explicitly by its owner.
 * @param directory Existing parent; NULL/zero selects system temp directory.
 * @param path Required output path. @param capacity Bytes including NUL, as temp_file.
 * @return Status; no object created on NO_SPACE. */
TB_API tb_fs_result tb_fs_temp_directory(tb_path_view directory, char *path, size_t capacity);
#endif
