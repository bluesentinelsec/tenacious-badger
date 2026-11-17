#include "badger/assert.h"
#include "badger/size.h"
#include "badger/str.h"
#include "fs_internal.h"
#include <string.h>

void tb_fs_buffer_free(tb_fs_buffer *buffer)
{
    TB_ASSERT(buffer != NULL);
    if (buffer->data != NULL)
    {
        tb_allocator_free(&buffer->allocator, buffer->data, buffer->capacity, 1);
    }
    *buffer = (tb_fs_buffer){0};
}
tb_fs_result tb_fs_read_file(tb_path_view path, const tb_allocator *allocator, size_t maximum,
                             tb_fs_buffer *output)
{
    tb_file file = {0};
    tb_fs_buffer buffer = {0};
    tb_fs_result result = {0};
    uint64_t size = {0};
    TB_ASSERT(output != NULL && output->data == NULL && output->length == 0 &&
              output->capacity == 0);
    buffer.allocator = tb_allocator_resolve(allocator);
    if (maximum == SIZE_MAX)
    {
        maximum--;
    }
    result = tb_file_open(path, TB_FILE_READ, &file);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    result = tb_file_size(&file, &size);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    if (size > maximum)
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    buffer.capacity = (size_t)size + 1;
    buffer.data = tb_allocator_try_alloc(&buffer.allocator, buffer.capacity, 1);
    if (buffer.data == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    for (;;)
    {
        size_t count = {0};
        if (buffer.length == buffer.capacity - 1)
        {
            unsigned char extra = {0};
            size_t grown = {0};
            unsigned char *resized = NULL;
            result = tb_file_read(&file, &extra, 1, &count);
            if (result != TB_FS_OK || count == 0)
            {
                break;
            }
            if (buffer.length == maximum)
            {
                result = TB_FS_TOO_LARGE;
                break;
            }
            grown = buffer.capacity > (maximum + 1) / 2 ? maximum + 1 : buffer.capacity * 2;
            resized =
                tb_allocator_try_realloc(&buffer.allocator, buffer.data, buffer.capacity, grown, 1);
            if (resized == NULL)
            {
                result = TB_FS_NO_MEMORY;
                break;
            }
            buffer.data = resized;
            buffer.capacity = grown;
            buffer.data[buffer.length] = extra;
            buffer.length++;
        }
        else
        {
            result = tb_file_read(&file, buffer.data + buffer.length,
                                  buffer.capacity - 1 - buffer.length, &count);
            buffer.length += count;
            if (result != TB_FS_OK || count == 0)
            {
                break;
            }
        }
    }
cleanup: {
    tb_fs_result closed = {0};
    closed = tb_file_close(&file);
    if (result == TB_FS_OK)
    {
        result = closed;
    }
}
    if (result == TB_FS_OK)
    {
        buffer.data[buffer.length] = 0;
        *output = buffer;
    }
    else
    {
        tb_fs_buffer_free(&buffer);
    }
    return result;
}
static tb_fs_result tb_fs_write_all(tb_file *file, const void *bytes, size_t length,
                                    const tb_fs_write_operations *operations)
{
    size_t written = {0};
    while (written < length)
    {
        size_t count = {0};
        tb_fs_result result = {0};
        result = operations->write_bytes(file, (const unsigned char *)bytes + written,
                                         length - written, &count);
        if (result != TB_FS_OK)
        {
            return result;
        }
        if (count == 0 || count > length - written)
        {
            return TB_FS_OS_ERROR;
        }
        written += count;
    }
    return operations->flush(file);
}
static const tb_fs_write_operations tb_fs_native_writes = {tb_file_write, tb_file_flush,
                                                           tb_file_close, tb_fs_rename};

tb_fs_result tb_fs_write_file(tb_path_view path, const void *bytes, size_t length)
{
    tb_file file = {0};
    tb_fs_result result = {0};
    tb_fs_result closed = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    result = tb_file_open(path, TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_TRUNCATE, &file);
    if (result != TB_FS_OK)
    {
        return result;
    }
    result = tb_fs_write_all(&file, bytes, length, &tb_fs_native_writes);
    closed = tb_file_close(&file);
    return result == TB_FS_OK ? closed : result;
}

tb_fs_result tb_fs_replace_with_operations(tb_path_view path, const void *bytes, size_t length,
                                           const tb_fs_write_operations *operations)
{
    tb_file file = {0};
    tb_path_style style = {0};
    tb_path_parts parts = {0};
    char *parent = NULL;
    char *temporary = NULL;
    size_t capacity = {0};
    size_t parent_size = {0};
    size_t parent_length = {0};
    bool append_dot = {0};
    bool created = {0};
    tb_fs_result result = {0};
    TB_ASSERT(bytes != NULL || length == 0);
    style = tb_fs_native_style();
    if (tb_path_parse(style, path, &parts) != TB_PATH_OK || parts.name.length == 0 ||
        (parts.name.length == 1 && parts.name.data[0] == '.') ||
        (parts.name.length == 2 && memcmp(parts.name.data, "..", 2) == 0))
    {
        return TB_FS_INVALID;
    }
    /* Preserve OS resolution through symlink-containing parents. Lexically
     * normalizing a/../ can change the target directory before the rename. */
    parent_length = parts.directory.length;
    append_dot = parent_length == 0 ||
                 (style == TB_PATH_WINDOWS && parts.directory.data[parent_length - 1] == ':');
    if (!tb_size_add(parent_length, append_dot ? 2 : 1, &parent_size))
    {
        return TB_FS_TOO_LARGE;
    }
    parent = tb_allocator_try_alloc(NULL, parent_size, 1);
    if (parent == NULL)
    {
        return TB_FS_NO_MEMORY;
    }
    if (parent_length != 0)
    {
        memcpy(parent, parts.directory.data, parent_length);
    }
    if (append_dot)
    {
        parent[parent_length] = '.';
        parent_length++;
    }
    parent[parent_length] = '\0';
    if (!tb_size_add(parent_length, 64, &capacity))
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    temporary = tb_allocator_try_alloc(NULL, capacity, 1);
    if (temporary == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    result =
        tb_fs_temp_file((tb_path_view){parent, tb_str_length(parent)}, temporary, capacity, &file);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    created = true;
    result = tb_fs_write_all(&file, bytes, length, operations);
    {
        tb_fs_result closed = {0};
        closed = operations->close_file(&file);
        if (result == TB_FS_OK)
        {
            result = closed;
        }
    }
    if (result == TB_FS_OK)
    {
        result = operations->rename_file((tb_path_view){temporary, tb_str_length(temporary)}, path);
    }
    if (result == TB_FS_OK)
    {
        created = false;
    }
cleanup:
    if (file.value != 0)
    {
        (void)tb_file_close(&file);
    }
    if (created)
    {
        (void)tb_fs_remove((tb_path_view){temporary, tb_str_length(temporary)});
    }
    tb_allocator_free(NULL, temporary, capacity, 1);
    tb_allocator_free(NULL, parent, parent_size, 1);
    return result;
}
tb_fs_result tb_fs_replace_file(tb_path_view path, const void *bytes, size_t length)
{
    return tb_fs_replace_with_operations(path, bytes, length, &tb_fs_native_writes);
}
tb_fs_result tb_fs_make_directories(tb_path_view path)
{
    char *normalized = NULL;
    tb_path_style style = {0};
    tb_path_parts parts = {0};
    tb_path_result path_result = {0};
    tb_fs_result result = {0};
    size_t length = {0};
    style = tb_fs_native_style();
    if (path.length == 0)
    {
        return TB_FS_INVALID;
    }
    normalized = tb_path_normalize_new(NULL, style, path, &path_result);
    if (normalized == NULL)
    {
        return TB_FS_INVALID;
    }
    length = tb_str_length(normalized);
    (void)tb_path_parse(style, (tb_path_view){normalized, length}, &parts);
    if (parts.verbatim)
    {
        result = TB_FS_INVALID;
        goto cleanup;
    }
    for (size_t end = {0}; end <= length; end++)
    {
        tb_fs_info info = {0};
        if (end == 0 || end < parts.root.length || (end != length && normalized[end] != '/'))
        {
            continue;
        }
        result = tb_fs_make_directory((tb_path_view){normalized, end});
        if (result == TB_FS_EXISTS || end == parts.root.length)
        {
            result = tb_fs_query((tb_path_view){normalized, end}, &info);
            if (result == TB_FS_OK && info.kind == TB_FS_LINK)
            {
                tb_directory directory = {0};
                /* Native directory aliases (for example macOS /var) are part
                 * of normal path resolution. Opening verifies the link target
                 * is a directory without pretending to provide containment. */
                result = tb_directory_open((tb_path_view){normalized, end}, &directory);
                if (result == TB_FS_OK)
                {
                    result = tb_directory_close(&directory);
                }
            }
            else if (result == TB_FS_OK && info.kind != TB_FS_DIRECTORY)
            {
                result = TB_FS_NOT_DIRECTORY;
            }
        }
        if (result != TB_FS_OK)
        {
            goto cleanup;
        }
    }
cleanup:
    tb_str_free(NULL, normalized);
    return result;
}

const char *tb_fs_result_name(tb_fs_result result)
{
    switch (result)
    {
    case TB_FS_OK:
        return "ok";
    case TB_FS_NOT_FOUND:
        return "not found";
    case TB_FS_EXISTS:
        return "already exists";
    case TB_FS_ACCESS:
        return "access denied";
    case TB_FS_NOT_DIRECTORY:
        return "not a directory";
    case TB_FS_IS_DIRECTORY:
        return "is a directory";
    case TB_FS_NOT_EMPTY:
        return "directory not empty";
    case TB_FS_INVALID:
        return "invalid argument";
    case TB_FS_NO_SPACE:
        return "no space left";
    case TB_FS_BUFFER_TOO_SMALL:
        return "buffer too small";
    case TB_FS_NO_MEMORY:
        return "out of memory";
    case TB_FS_TOO_LARGE:
        return "too large";
    case TB_FS_OS_ERROR:
        return "operating system error";
    case TB_FS_END:
        return "end";
    }
    return "unknown result";
}
