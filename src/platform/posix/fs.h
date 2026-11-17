#ifndef TB_POSIX_FS_H
#define TB_POSIX_FS_H
#include "../shared/fs.h"
#include <sys/stat.h>
void tb_native_fs_stat_time(const struct stat *native, tb_fs_info *info);
tb_fs_result tb_native_fs_errno(int code);
tb_fs_result tb_native_fs_home(char **path);
tb_fs_result tb_native_fs_home_suffix(const char *suffix, char **path);
#endif
