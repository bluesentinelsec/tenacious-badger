#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "../posix/fs.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void tb_native_fs_stat_time(const struct stat *native, tb_fs_info *info)
{
    info->modified_seconds = (int64_t)native->st_mtim.tv_sec;
    info->modified_nanoseconds = (uint32_t)native->st_mtim.tv_nsec;
}
tb_fs_result tb_native_fs_location(tb_fs_location location, char **path)
{
    const char *environment = NULL;
    const char *suffix = NULL;
    *path = NULL;
    switch (location)
    {
    case TB_FS_EXECUTABLE: {
        size_t capacity = {0};
        capacity = 256;
        while (capacity <= 1048576)
        {
            char *buffer = NULL;
            ssize_t count = {0};
            buffer = malloc(capacity);
            if (buffer == NULL)
            {
                return TB_FS_NO_MEMORY;
            }
            count = readlink("/proc/self/exe", buffer, capacity - 1);
            if (count < 0)
            {
                tb_fs_result result = {0};
                result = tb_native_fs_errno(errno);
                free(buffer);
                return result;
            }
            if ((size_t)count < capacity - 1)
            {
                size_t length = {0};
                length = (size_t)count;
                while (length > 1 && buffer[length - 1] != '/')
                {
                    length--;
                }
                buffer[length] = '\0';
                *path = buffer;
                return TB_FS_OK;
            }
            free(buffer);
            capacity *= 2;
        }
        return TB_FS_TOO_LARGE;
    }
    case TB_FS_USER_DATA:
        environment = getenv("XDG_DATA_HOME");
        suffix = "/.local/share";
        break;
    case TB_FS_USER_CONFIG:
        environment = getenv("XDG_CONFIG_HOME");
        suffix = "/.config";
        break;
    case TB_FS_USER_CACHE:
        environment = getenv("XDG_CACHE_HOME");
        suffix = "/.cache";
        break;
    case TB_FS_TEMP:
        environment = getenv("TMPDIR");
        if (environment == NULL || environment[0] != '/')
        {
            environment = "/tmp";
        }
        break;
    default:
        return TB_FS_INVALID;
    }
    if (environment != NULL && environment[0] == '/')
    {
        return tb_native_fs_copy_path((tb_path_view){environment, strlen(environment)}, path);
    }
    return tb_native_fs_home_suffix(suffix, path);
}
