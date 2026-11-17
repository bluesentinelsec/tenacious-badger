#include "fs.h"
#include "badger/assert.h"
#include "badger/platform.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native paths must reject non-Unicode byte names even on POSIX. This boundary
 * check is independent of core, which is layered above the adapters. */
bool tb_native_fs_text_valid(const char *text, size_t length)
{
    size_t index = {0};
    TB_ASSERT(text != NULL || length == 0);
    while (index < length)
    {
        unsigned char first = {0};
        size_t width = {0};
        first = (unsigned char)text[index];
        if (first == 0)
        {
            return false;
        }
        if (first < 0x80)
        {
            index++;
            continue;
        }
        if (first >= 0xC2 && first <= 0xDF)
        {
            width = 2;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            width = 3;
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            width = 4;
        }
        else
        {
            return false;
        }
        if (width > length - index)
        {
            return false;
        }
        for (size_t extra = {0}; extra < width - 1; extra++)
        {
            unsigned char byte = {0};
            byte = (unsigned char)text[index + extra + 1];
            if (byte < 0x80 || byte > 0xBF)
            {
                return false;
            }
        }
        if ((first == 0xE0 && (unsigned char)text[index + 1] < 0xA0) ||
            (first == 0xED && (unsigned char)text[index + 1] >= 0xA0) ||
            (first == 0xF0 && (unsigned char)text[index + 1] < 0x90) ||
            (first == 0xF4 && (unsigned char)text[index + 1] >= 0x90))
        {
            return false;
        }
        index += width;
    }
    return true;
}

tb_fs_result tb_native_fs_copy_path(tb_path_view path, char **copy)
{
    TB_ASSERT(copy != NULL);
    *copy = NULL;
    if (path.length == SIZE_MAX)
    {
        return TB_FS_TOO_LARGE;
    }
    if (path.length == 0 || !tb_native_fs_text_valid(path.data, path.length))
    {
        return TB_FS_INVALID;
    }
    *copy = malloc(path.length + 1);
    if (*copy == NULL)
    {
        return TB_FS_NO_MEMORY;
    }
    memcpy(*copy, path.data, path.length);
    (*copy)[path.length] = '\0';
    return TB_FS_OK;
}

tb_fs_result tb_native_fs_output(const char *text, char *destination, size_t capacity,
                                 size_t *required)
{
    size_t length = {0};
    TB_ASSERT(required != NULL);
    TB_ASSERT(destination != NULL || capacity == 0);
    *required = 0;
    length = strlen(text);
    if (!tb_native_fs_text_valid(text, length))
    {
        return TB_FS_INVALID;
    }
    *required = length + 1;
    if (destination == NULL)
    {
        return TB_FS_OK;
    }
    if (capacity < *required)
    {
        return TB_FS_BUFFER_TOO_SMALL;
    }
    memcpy(destination, text, *required);
    return TB_FS_OK;
}

tb_fs_result tb_fs_get_location(tb_fs_location location, char *destination, size_t capacity,
                                size_t *required)
{
    char *path = NULL;
    tb_fs_result result = {0};
    TB_ASSERT(required != NULL);
    TB_ASSERT(destination != NULL || capacity == 0);
    *required = 0;
    result = tb_native_fs_location(location, &path);
    if (result == TB_FS_OK)
    {
        result = tb_native_fs_output(path, destination, capacity, required);
    }
    free(path);
    return result;
}

static tb_fs_result tb_fs_temp(tb_path_view directory, char *path, size_t capacity, tb_file *file)
{
    static tb_platform_atomic_size counter = {0};
    char *parent = NULL;
    char *candidate = NULL;
    size_t length = {0};
    size_t needed = {0};
    tb_fs_result result = {0};
    TB_ASSERT(path != NULL);
    TB_ASSERT(directory.data != NULL || directory.length == 0);
    if (directory.length == 0)
    {
        result = tb_native_fs_location(TB_FS_TEMP, &parent);
    }
    else
    {
        result = tb_native_fs_copy_path(directory, &parent);
    }
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    length = strlen(parent);
    if (length > SIZE_MAX - 64)
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    needed = length + 64;
    candidate = malloc(needed);
    if (candidate == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    for (size_t attempt = {0}; attempt < 128; attempt++)
    {
        uint64_t stamp = {0};
        size_t serial = {0};
        int written = {0};
        stamp = tb_platform_clock_nanoseconds();
        serial = tb_platform_atomic_size_add(&counter, 1);
        written = snprintf(candidate, needed, "%s%stb-%016" PRIx64 "-%016zx", parent,
                           length > 0 && tb_native_fs_separator(parent[length - 1]) ? "" : "/",
                           stamp, serial);
        if (written < 0 || (size_t)written >= needed)
        {
            result = TB_FS_TOO_LARGE;
            goto cleanup;
        }
        if ((size_t)written >= capacity)
        {
            result = TB_FS_BUFFER_TOO_SMALL;
            goto cleanup;
        }
        if (file != NULL)
        {
            result = tb_file_open((tb_path_view){candidate, (size_t)written},
                                  TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE,
                                  file);
        }
        else
        {
            result = tb_fs_make_directory((tb_path_view){candidate, (size_t)written});
        }
        if (result == TB_FS_EXISTS)
        {
            continue;
        }
        if (result == TB_FS_OK)
        {
            memcpy(path, candidate, (size_t)written + 1);
        }
        goto cleanup;
    }
    result = TB_FS_EXISTS;
cleanup:
    free(candidate);
    free(parent);
    return result;
}

tb_fs_result tb_fs_temp_file(tb_path_view directory, char *path, size_t capacity, tb_file *file)
{
    TB_ASSERT(file != NULL);
    TB_ASSERT(file->value == 0);
    return tb_fs_temp(directory, path, capacity, file);
}
tb_fs_result tb_fs_temp_directory(tb_path_view directory, char *path, size_t capacity)
{
    return tb_fs_temp(directory, path, capacity, NULL);
}
