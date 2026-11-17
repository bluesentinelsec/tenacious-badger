#define _DARWIN_C_SOURCE
#define _FILE_OFFSET_BITS 64
#include "../posix/fs.h"
#include <errno.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void tb_native_fs_stat_time(const struct stat *native, tb_fs_info *info)
{
    info->modified_seconds = (int64_t)native->st_mtimespec.tv_sec;
    info->modified_nanoseconds = (uint32_t)native->st_mtimespec.tv_nsec;
}
tb_fs_result tb_native_fs_location(tb_fs_location location, char **path)
{
    const char *environment = NULL;
    *path = NULL;
    switch (location)
    {
    case TB_FS_EXECUTABLE: {
        uint32_t needed = {0};
        char *name = NULL;
        char *absolute = NULL;
        size_t length = {0};
        (void)_NSGetExecutablePath(NULL, &needed);
        if (needed == 0)
        {
            return TB_FS_OS_ERROR;
        }
        name = malloc(needed);
        if (name == NULL)
        {
            return TB_FS_NO_MEMORY;
        }
        if (_NSGetExecutablePath(name, &needed) != 0)
        {
            free(name);
            return TB_FS_TOO_LARGE;
        }
        absolute = realpath(name, NULL);
        free(name);
        if (absolute == NULL)
        {
            return tb_native_fs_errno(errno);
        }
        length = strlen(absolute);
        while (length > 1 && absolute[length - 1] != '/')
        {
            length--;
        }
        absolute[length] = '\0';
        *path = absolute;
        return TB_FS_OK;
    }
    case TB_FS_USER_DATA:
        return tb_native_fs_home_suffix("/Library/Application Support", path);
    case TB_FS_USER_CONFIG:
        return tb_native_fs_home_suffix("/Library/Preferences", path);
    case TB_FS_USER_CACHE:
        return tb_native_fs_home_suffix("/Library/Caches", path);
    case TB_FS_TEMP:
        environment = getenv("TMPDIR");
        if (environment == NULL || environment[0] != '/')
        {
            environment = "/tmp";
        }
        return tb_native_fs_copy_path((tb_path_view){environment, strlen(environment)}, path);
    default:
        return TB_FS_INVALID;
    }
}
