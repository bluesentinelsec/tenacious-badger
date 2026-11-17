#ifndef TB_FS_INTERNAL_H
#define TB_FS_INTERNAL_H
#include "badger/fs.h"
/* The atomic-write transaction's fallible steps. Tests substitute individual
 * failures, including short writes, without depending on a disk filling up. */
typedef struct tb_fs_write_operations
{
    tb_fs_result (*write_bytes)(tb_file *, const void *, size_t, size_t *);
    tb_fs_result (*flush)(tb_file *);
    tb_fs_result (*close_file)(tb_file *);
    tb_fs_result (*rename_file)(tb_path_view, tb_path_view);
} tb_fs_write_operations;
tb_fs_result tb_fs_replace_with_operations(tb_path_view path, const void *bytes, size_t length,
                                           const tb_fs_write_operations *operations);
#endif
