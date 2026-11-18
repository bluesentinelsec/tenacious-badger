/* Helpers beneath core: native adapters are also built without core. */
#ifndef TB_NATIVE_FS_H
#define TB_NATIVE_FS_H
#include "badger/file.h"

bool tb_native_fs_text_valid(const char *text, size_t length);
tb_fs_result tb_native_fs_copy_path(tb_path_view path, char **copy);
tb_fs_result tb_native_fs_output(const char *text, char *destination, size_t capacity,
                                 size_t *required);
/* Platform-specific, owned C heap string for the shared bounded wrapper. */
tb_fs_result tb_native_fs_location(tb_fs_location location, char **path);
bool tb_native_fs_separator(char byte);
/* Bound a positional transfer: INVALID for an offset above INT64_MAX, else the
 * length shortened to one native call and to the end of the int64_t range. */
tb_fs_result tb_native_fs_positional_length(uint64_t offset, size_t length, size_t *bounded);
#endif
