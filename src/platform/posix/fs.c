#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "fs.h"
#include "badger/assert.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct tb_native_directory
{
    DIR *handle;
    struct dirent *pending;
} tb_native_directory;

tb_fs_result tb_native_fs_errno(int code)
{
    switch (code)
    {
    case ENOENT:
        return TB_FS_NOT_FOUND;
    case EEXIST:
        return TB_FS_EXISTS;
    case EBADF:
    case EACCES:
    case EPERM:
    case EROFS:
    case EBUSY:
        return TB_FS_ACCESS;
    case ENOTDIR:
        return TB_FS_NOT_DIRECTORY;
    case EISDIR:
        return TB_FS_IS_DIRECTORY;
    case ENOTEMPTY:
        return TB_FS_NOT_EMPTY;
    case EINVAL:
    case ELOOP:
        return TB_FS_INVALID;
    case ENOSPC:
    case EDQUOT:
        return TB_FS_NO_SPACE;
    case ENOMEM:
    case EMFILE:
    case ENFILE:
        return TB_FS_NO_MEMORY;
    case ENAMETOOLONG:
    case EFBIG:
    case EOVERFLOW:
        return TB_FS_TOO_LARGE;
    default:
        return TB_FS_IO;
    }
}
static int tb_file_descriptor(const tb_file *file)
{
    TB_ASSERT(file != NULL);
    TB_ASSERT(file->value > 0 && file->value - 1 <= INT_MAX);
    return (int)(file->value - 1);
}
tb_fs_result tb_file_open(tb_path_view path, unsigned flags, tb_file *file)
{
    char *name = NULL;
    int mode = {0};
    int descriptor = {0};
    struct stat metadata = {0};
    tb_fs_result result = {0};
    TB_ASSERT(file != NULL);
    TB_ASSERT(file->value == 0);
    if ((flags & ~(unsigned)(TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE |
                             TB_FILE_TRUNCATE)) != 0 ||
        (flags & (TB_FILE_READ | TB_FILE_WRITE)) == 0 ||
        ((flags & (TB_FILE_CREATE | TB_FILE_TRUNCATE)) != 0 && (flags & TB_FILE_WRITE) == 0) ||
        ((flags & TB_FILE_EXCLUSIVE) != 0 && (flags & TB_FILE_CREATE) == 0))
    {
        return TB_FS_INVALID;
    }
    result = tb_native_fs_copy_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    mode = O_RDONLY;
    if ((flags & TB_FILE_WRITE) != 0)
    {
        mode = (flags & TB_FILE_READ) != 0 ? O_RDWR : O_WRONLY;
    }
    mode |= O_CLOEXEC | O_NONBLOCK;
    if ((flags & TB_FILE_CREATE) != 0)
    {
        mode |= O_CREAT;
    }
    if ((flags & TB_FILE_EXCLUSIVE) != 0)
    {
        mode |= O_EXCL;
    }
    /* Check regular-file kind before truncating so a directory or device is
     * never modified as a side effect of rejecting it. */
    do
    {
        descriptor = open(name, mode, 0600);
    } while (descriptor < 0 && errno == EINTR);
    result = descriptor < 0 ? tb_native_fs_errno(errno) : TB_FS_OK;
    free(name);
    if (descriptor < 0)
    {
        return result;
    }
    if (fstat(descriptor, &metadata) != 0)
    {
        result = tb_native_fs_errno(errno);
        goto failed;
    }
    if (!S_ISREG(metadata.st_mode))
    {
        result = S_ISDIR(metadata.st_mode) ? TB_FS_IS_DIRECTORY : TB_FS_INVALID;
        goto failed;
    }
    if ((flags & TB_FILE_TRUNCATE) != 0)
    {
        int truncated = {0};
        do
        {
            truncated = ftruncate(descriptor, 0);
        } while (truncated != 0 && errno == EINTR);
        if (truncated != 0)
        {
            result = tb_native_fs_errno(errno);
            goto failed;
        }
    }
    file->value = (uintptr_t)descriptor + 1;
    return TB_FS_OK;
failed:
    (void)close(descriptor);
    return result;
}
tb_fs_result tb_file_read(tb_file *file, void *bytes, size_t capacity, size_t *transferred)
{
    ssize_t count = {0};
    int descriptor = {0};
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || capacity == 0);
    *transferred = 0;
    descriptor = tb_file_descriptor(file);
    if (capacity == 0)
    {
        return TB_FS_OK;
    }
    if (capacity > INT_MAX)
    {
        capacity = INT_MAX;
    }
    do
    {
        count = read(descriptor, bytes, capacity);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
    {
        return tb_native_fs_errno(errno);
    }
    *transferred = (size_t)count;
    return TB_FS_OK;
}
tb_fs_result tb_file_write(tb_file *file, const void *bytes, size_t length, size_t *transferred)
{
    ssize_t count = {0};
    int descriptor = {0};
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    *transferred = 0;
    descriptor = tb_file_descriptor(file);
    if (length == 0)
    {
        return TB_FS_OK;
    }
    if (length > INT_MAX)
    {
        length = INT_MAX;
    }
    do
    {
        count = write(descriptor, bytes, length);
    } while (count < 0 && errno == EINTR);
    if (count < 0)
    {
        return tb_native_fs_errno(errno);
    }
    *transferred = (size_t)count;
    return TB_FS_OK;
}
tb_fs_result tb_file_seek(tb_file *file, int64_t offset, tb_file_origin origin, uint64_t *position)
{
    int native_origin = {0};
    off_t answer = {0};
    TB_ASSERT(position != NULL);
    TB_STATIC_ASSERT(sizeof(off_t) >= sizeof(int64_t), "64-bit file offsets required");
    switch (origin)
    {
    case TB_FILE_START:
        native_origin = SEEK_SET;
        break;
    case TB_FILE_CURRENT:
        native_origin = SEEK_CUR;
        break;
    case TB_FILE_END:
        native_origin = SEEK_END;
        break;
    default:
        return TB_FS_INVALID;
    }
    answer = lseek(tb_file_descriptor(file), (off_t)offset, native_origin);
    if (answer < 0)
    {
        return tb_native_fs_errno(errno);
    }
    *position = (uint64_t)answer;
    return TB_FS_OK;
}
tb_fs_result tb_file_tell(tb_file *file, uint64_t *position)
{
    return tb_file_seek(file, 0, TB_FILE_CURRENT, position);
}
tb_fs_result tb_file_size(tb_file *file, uint64_t *size)
{
    struct stat metadata = {0};
    TB_ASSERT(size != NULL);
    if (fstat(tb_file_descriptor(file), &metadata) != 0)
    {
        return tb_native_fs_errno(errno);
    }
    if (metadata.st_size < 0)
    {
        return TB_FS_TOO_LARGE;
    }
    *size = (uint64_t)metadata.st_size;
    return TB_FS_OK;
}
tb_fs_result tb_file_flush(tb_file *file)
{
    int result = {0};
    int descriptor = {0};
    descriptor = tb_file_descriptor(file);
    do
    {
        result = fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result == 0 ? TB_FS_OK : tb_native_fs_errno(errno);
}
tb_fs_result tb_file_close(tb_file *file)
{
    int descriptor = {0};
    TB_ASSERT(file != NULL);
    if (file->value == 0)
    {
        return TB_FS_OK;
    }
    descriptor = tb_file_descriptor(file);
    file->value = 0;
    return close(descriptor) == 0 ? TB_FS_OK : tb_native_fs_errno(errno);
}
tb_fs_result tb_fs_query(tb_path_view path, tb_fs_info *info)
{
    char *name = NULL;
    struct stat metadata = {0};
    tb_fs_info found = {0};
    tb_fs_result result = {0};
    TB_ASSERT(info != NULL);
    result = tb_native_fs_copy_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    if (lstat(name, &metadata) != 0)
    {
        result = tb_native_fs_errno(errno);
        goto cleanup;
    }
    found.kind = TB_FS_OTHER;
    if (S_ISREG(metadata.st_mode))
    {
        found.kind = TB_FS_REGULAR;
    }
    else if (S_ISDIR(metadata.st_mode))
    {
        found.kind = TB_FS_DIRECTORY;
    }
    else if (S_ISLNK(metadata.st_mode))
    {
        found.kind = TB_FS_LINK;
    }
    if (metadata.st_size < 0)
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    found.size = (uint64_t)metadata.st_size;
    tb_native_fs_stat_time(&metadata, &found);
    *info = found;
cleanup:
    free(name);
    return result;
}
tb_fs_result tb_fs_make_directory(tb_path_view path)
{
    char *name = NULL;
    tb_fs_result result = {0};
    result = tb_native_fs_copy_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    if (mkdir(name, 0700) != 0)
    {
        result = tb_native_fs_errno(errno);
    }
    free(name);
    return result;
}
tb_fs_result tb_fs_remove(tb_path_view path)
{
    char *name = NULL;
    struct stat metadata = {0};
    tb_fs_result result = {0};
    int removed = {0};
    result = tb_native_fs_copy_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    if (lstat(name, &metadata) != 0)
    {
        result = tb_native_fs_errno(errno);
        goto cleanup;
    }
    removed = S_ISDIR(metadata.st_mode) ? rmdir(name) : unlink(name);
    if (removed != 0)
    {
        result = tb_native_fs_errno(errno);
    }
cleanup:
    free(name);
    return result;
}
tb_fs_result tb_fs_rename(tb_path_view source, tb_path_view destination)
{
    char *from = NULL;
    char *to = NULL;
    tb_fs_result result = {0};
    result = tb_native_fs_copy_path(source, &from);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    result = tb_native_fs_copy_path(destination, &to);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    if (rename(from, to) != 0)
    {
        result = tb_native_fs_errno(errno);
    }
cleanup:
    free(from);
    free(to);
    return result;
}
tb_fs_result tb_directory_open(tb_path_view path, tb_directory *directory)
{
    char *name = NULL;
    tb_native_directory *state = NULL;
    tb_fs_result result = {0};
    TB_ASSERT(directory != NULL);
    TB_ASSERT(directory->state == NULL);
    result = tb_native_fs_copy_path(path, &name);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    state = malloc(sizeof(*state));
    if (state == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    *state = (tb_native_directory){0};
    state->handle = opendir(name);
    if (state->handle == NULL)
    {
        result = tb_native_fs_errno(errno);
        goto cleanup;
    }
    directory->state = state;
    state = NULL;
cleanup:
    free(state);
    free(name);
    return result;
}
tb_fs_result tb_directory_next(tb_directory *directory, char *name, size_t capacity,
                               size_t *required)
{
    tb_native_directory *state = NULL;
    tb_fs_result result = {0};
    TB_ASSERT(directory != NULL && directory->state != NULL);
    TB_ASSERT(required != NULL);
    TB_ASSERT(name != NULL || capacity == 0);
    *required = 0;
    state = directory->state;
    while (state->pending == NULL)
    {
        errno = 0;
        state->pending = readdir(state->handle);
        if (state->pending == NULL)
        {
            return errno == 0 ? TB_FS_END : tb_native_fs_errno(errno);
        }
        if (strcmp(state->pending->d_name, ".") == 0 || strcmp(state->pending->d_name, "..") == 0)
        {
            state->pending = NULL;
        }
    }
    result = tb_native_fs_output(state->pending->d_name, name, capacity, required);
    if (result == TB_FS_INVALID || (result == TB_FS_OK && name != NULL))
    {
        state->pending = NULL;
    }
    return result;
}
tb_fs_result tb_directory_close(tb_directory *directory)
{
    tb_native_directory *state = NULL;
    tb_fs_result result = {0};
    TB_ASSERT(directory != NULL);
    state = directory->state;
    directory->state = NULL;
    if (state == NULL)
    {
        return TB_FS_OK;
    }
    if (closedir(state->handle) != 0)
    {
        result = tb_native_fs_errno(errno);
    }
    free(state);
    return result;
}

tb_fs_result tb_native_fs_home(char **path)
{
    const char *environment = NULL;
    char *buffer = NULL;
    size_t capacity = {0};
    tb_fs_result result = {0};
    environment = getenv("HOME");
    if (environment != NULL && environment[0] == '/')
    {
        return tb_native_fs_copy_path((tb_path_view){environment, strlen(environment)}, path);
    }
    capacity = 4096;
    for (;;)
    {
        struct passwd record = {0};
        struct passwd *found = NULL;
        int code = {0};
        buffer = malloc(capacity);
        if (buffer == NULL)
        {
            return TB_FS_NO_MEMORY;
        }
        code = getpwuid_r(getuid(), &record, buffer, capacity, &found);
        if (code == ERANGE && capacity < 1048576)
        {
            free(buffer);
            buffer = NULL;
            capacity *= 2;
            continue;
        }
        if (code != 0)
        {
            result = tb_native_fs_errno(code);
        }
        else if (found == NULL || found->pw_dir == NULL || found->pw_dir[0] != '/')
        {
            result = TB_FS_NOT_FOUND;
        }
        else
        {
            result =
                tb_native_fs_copy_path((tb_path_view){found->pw_dir, strlen(found->pw_dir)}, path);
        }
        free(buffer);
        return result;
    }
}
tb_fs_result tb_native_fs_home_suffix(const char *suffix, char **path)
{
    char *home = NULL;
    size_t length = {0};
    size_t extra = {0};
    tb_fs_result result = {0};
    result = tb_native_fs_home(&home);
    if (result != TB_FS_OK)
    {
        return result;
    }
    length = strlen(home);
    extra = strlen(suffix);
    if (length >= SIZE_MAX - extra)
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    *path = malloc(length + extra + 1);
    if (*path == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    memcpy(*path, home, length);
    memcpy(*path + length, suffix, extra + 1);
cleanup:
    free(home);
    return result;
}

tb_path_style tb_fs_native_style(void)
{
    return TB_PATH_POSIX;
}
bool tb_native_fs_separator(char byte)
{
    return byte == '/';
}
