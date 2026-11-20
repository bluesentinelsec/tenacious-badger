/** @file fs.h
 * @brief Allocator-aware whole-file helpers over the native file interface.
 *
 * No filesystem snapshot or containment is implied. Handles and temporary
 * objects are owned only during a call. Runtime errors are returned, including
 * allocation failure.
 *
 * @par Threading
 * Thread-safe, when each call has an output of its own and the allocator it
 * is given is thread-safe. Two calls that write the same path at the same
 * time each succeed or fail as a whole, in an order the file system chooses.
 */
#ifndef TB_FS_H
#define TB_FS_H
#include "badger/allocator.h"
#include "badger/file.h"

/** @brief Owned binary data with a convenience NUL after its last byte.
 * Initialize to zero, do not copy ownership, and release with tb_fs_buffer_free.
 */
typedef struct tb_fs_buffer
{
    unsigned char *data;    /**< Owned bytes; NULL only when uninitialized or freed. */
    size_t length;          /**< Payload bytes, excluding the convenience NUL. */
    size_t capacity;        /**< Private allocation size, including NUL; do not modify. */
    tb_allocator allocator; /**< Private allocator copy; its context must outlive data. */
} tb_fs_buffer;
/** @brief Read until EOF, bounded by a caller's resource limit.
 * @param path Borrowed UTF-8 filename.
 * @param allocator Result allocator, or NULL for the engine heap.
 * @param maximum Maximum payload bytes; zero accepts only an empty file.
 * @param[out] output Required zero-initialized buffer; unchanged on error.
 * @return Status. TOO_LARGE means the limit or representation was exceeded.
 * A concurrently modified file is not a snapshot. Empty files still own a
 * one-byte NUL buffer. Allocation failures return NO_MEMORY after closing.
 */
TB_API tb_fs_result tb_fs_read_file(tb_path_view path, const tb_allocator *allocator,
                                    size_t maximum, tb_fs_buffer *output);
/** @brief Release and zero an owned buffer; already-zero buffers are accepted.
 * @param buffer Required, exclusively owned buffer from tb_fs_read_file. */
TB_API void tb_fs_buffer_free(tb_fs_buffer *buffer);
/** @brief Create/truncate and write a complete binary file, then flush and close.
 * @param path Borrowed filename. @param bytes Input, NULL only at length zero.
 * @param length Payload bytes. @return Status; failure may leave a partial file.
 * Parents must exist. Use tb_fs_replace_file when the previous file must survive. */
TB_API tb_fs_result tb_fs_write_file(tb_path_view path, const void *bytes, size_t length);
/** @brief Write a file as tb_fs_write_file() does, but leave the bytes in the
 * system's cache, for the system to put on the disk later.
 *
 * A program that reads the file after this returns, this one or another,
 * reads the bytes written. A loss of power before the system writes them may
 * lose them. That suits a cache, a log, or a tool writing many files that it
 * can write again; a save game or a setting the player chose wants
 * tb_fs_write_file() or tb_fs_replace_file(). Writing 4 KiB takes about a
 * tenth of the time.
 * @param path Borrowed filename. @param bytes Input, NULL only at length zero.
 * @param length Payload bytes. @return As tb_fs_write_file(). */
TB_API tb_fs_result tb_fs_write_file_cached(tb_path_view path, const void *bytes, size_t length);
/** @brief Replace a file after a complete write, flush, and close in its directory.
 * @param path Borrowed filename; must have a non-dot final component.
 * @param bytes Input, NULL only at length zero. @param length Payload bytes.
 * @return Status. Until rename succeeds the old name remains unchanged.
 * New files use native creation permissions, not old permissions/metadata.
 * Readers see the old or new file. No power-loss durability for the directory
 * entry is promised. On failure cleanup is attempted; permission changes may
 * prevent removal of a temporary file named tb-*. No directory is created.
 */
TB_API tb_fs_result tb_fs_replace_file(tb_path_view path, const void *bytes, size_t length);
/** @brief Replace a file as tb_fs_replace_file() does, but leave the new
 * bytes in the system's cache, for the system to put on the disk later.
 *
 * If the program ends, however it ends, readers see the old file or the new
 * one, whole. If the power is lost before the system writes the bytes, the
 * new file may be found empty or partly written: use tb_fs_replace_file()
 * for anything that must survive that.
 * @param path Borrowed filename; must have a non-dot final component.
 * @param bytes Input, NULL only at length zero. @param length Payload bytes.
 * @return As tb_fs_replace_file(). */
TB_API tb_fs_result tb_fs_replace_file_cached(tb_path_view path, const void *bytes, size_t length);
/** @brief Create missing directories, accepting directories already present.
 * @param path Borrowed UTF-8 directory. Native host syntax is used.
 * Ordinary dot components are normalized lexically before creation.
 * @return Status; partial directory creation remains on failure. Existing
 * directory symlinks/reparse points are followed as native directory aliases.
 * This is a convenience operation, not a race-free security boundary. */
TB_API tb_fs_result tb_fs_make_directories(tb_path_view path);
/** @brief Name a result, for a log or a message: "not found", "access denied".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe. */
TB_API const char *tb_fs_result_name(tb_fs_result result);
#endif
