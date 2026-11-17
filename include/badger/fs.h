/** @file fs.h
 * @brief Allocator-aware whole-file helpers over the native file interface.
 *
 * Thread-safe for distinct outputs and thread-safe allocators. No filesystem
 * snapshot or containment is implied. Handles and temporary objects are owned
 * only during a call. Runtime errors are returned, including allocation failure.
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
/** @brief Create missing directories, accepting directories already present.
 * @param path Borrowed UTF-8 directory. Native host syntax is used.
 * Ordinary dot components are normalized lexically before creation.
 * @return Status; partial directory creation remains on failure. Existing
 * directory symlinks/reparse points are followed as native directory aliases.
 * This is a convenience operation, not a race-free security boundary. */
TB_API tb_fs_result tb_fs_make_directories(tb_path_view path);
#endif
