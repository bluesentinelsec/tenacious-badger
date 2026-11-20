/* A source over a directory on disk, read live. Names are joined to it with
 * a rooted join, and links are not followed: the file, and each directory on
 * the way to it, must be what it says, not a link to something else. */
#include "vfs_internal.h"

#include "badger/assert.h"
#include "badger/file.h"
#include "badger/path.h"
#include "badger/str.h"

#include <string.h>

/* Room for one name of a directory, grown when a name needs more. */
#define TB_VFS_NAME_BUFFER ((size_t)256)

typedef struct tb_vfs_directory
{
    tb_allocator allocator;
    char *root; /* Absolute, NUL-terminated. */
    size_t root_length;
    tb_path_style style;
} tb_vfs_directory;

static tb_vfs_result tb_vfs_from_fs(tb_fs_result result)
{
    switch (result)
    {
    case TB_FS_OK:
        return TB_VFS_OK;
    case TB_FS_NOT_FOUND:
    case TB_FS_NOT_DIRECTORY:
        return TB_VFS_NOT_FOUND;
    case TB_FS_IS_DIRECTORY:
        return TB_VFS_NOT_A_FILE;
    case TB_FS_EXISTS:
    case TB_FS_ACCESS:
    case TB_FS_NOT_EMPTY:
    case TB_FS_INVALID:
    case TB_FS_NO_SPACE:
    case TB_FS_BUFFER_TOO_SMALL:
    case TB_FS_NO_MEMORY:
    case TB_FS_TOO_LARGE:
    case TB_FS_OS_ERROR:
    case TB_FS_END:
    default:
        return TB_VFS_INPUT_FAILED;
    }
}

/* The native path of the first `length` bytes of a name: the root itself for
 * none. Freed with tb_str_free(). */
static char *tb_vfs_native(const tb_vfs_directory *directory, const char *name, size_t length)
{
    tb_path_result result = TB_PATH_OK;
    char *const path =
        tb_path_rooted_join_new(&directory->allocator, directory->style,
                                (tb_path_view){directory->root, directory->root_length},
                                (tb_path_view){name, length}, &result);
    /* The name is checked, so the join cannot be refused. */
    TB_ASSERT(path != NULL);
    return path;
}

static void tb_vfs_native_free(const tb_vfs_directory *directory, char *path)
{
    tb_str_free(&directory->allocator, path);
}

/* What the first `length` bytes of a name are on disk, without following a link. */
static tb_vfs_result tb_vfs_query_native(const tb_vfs_directory *directory, const char *name,
                                         size_t length, tb_fs_info *info)
{
    char *const path = tb_vfs_native(directory, name, length);
    const tb_fs_result result = tb_fs_query((tb_path_view){path, tb_str_length(path)}, info);
    tb_vfs_native_free(directory, path);
    return tb_vfs_from_fs(result);
}

/* What a name is, with every directory on the way to it checked to be a
 * directory and not a link. Links and other kinds are NOT_FOUND. */
static tb_vfs_result tb_vfs_resolve(const tb_vfs_directory *directory, const char *name,
                                    size_t length, tb_fs_info *info)
{
    tb_vfs_result result = TB_VFS_OK;
    for (size_t at = 0; at < length; at++)
    {
        if (name[at] != '/')
        {
            continue;
        }
        result = tb_vfs_query_native(directory, name, at, info);
        if (result != TB_VFS_OK)
        {
            return result;
        }
        if (info->kind != TB_FS_DIRECTORY)
        {
            return TB_VFS_NOT_FOUND;
        }
    }
    result = tb_vfs_query_native(directory, name, length, info);
    if (result == TB_VFS_OK && info->kind != TB_FS_REGULAR && info->kind != TB_FS_DIRECTORY)
    {
        return TB_VFS_NOT_FOUND;
    }
    return result;
}

static tb_vfs_result tb_vfs_directory_open(void *state, const char *name, size_t length,
                                           const tb_allocator *allocator, tb_stream *stream)
{
    const tb_vfs_directory *const directory = state;
    tb_fs_info info = {0};
    char *path = NULL;
    tb_fs_result opened = TB_FS_OK;
    tb_vfs_result result = tb_vfs_resolve(directory, name, length, &info);
    if (result != TB_VFS_OK)
    {
        return result;
    }
    if (info.kind != TB_FS_REGULAR)
    {
        return TB_VFS_NOT_A_FILE;
    }
    path = tb_vfs_native(directory, name, length);
    opened = tb_stream_open_file((tb_path_view){path, tb_str_length(path)}, TB_FILE_READ, allocator,
                                 stream);
    tb_vfs_native_free(directory, path);
    return tb_vfs_from_fs(opened);
}

static tb_vfs_result tb_vfs_directory_query(void *state, const char *name, size_t length,
                                            tb_vfs_info *info)
{
    const tb_vfs_directory *const directory = state;
    tb_fs_info native = {0};
    const tb_vfs_result result = tb_vfs_resolve(directory, name, length, &native);
    if (result != TB_VFS_OK)
    {
        return result;
    }
    *info = native.kind == TB_FS_DIRECTORY
                ? (tb_vfs_info){.kind = TB_VFS_DIRECTORY}
                : (tb_vfs_info){.kind = TB_VFS_FILE, .size = native.size};
    return TB_VFS_OK;
}

/* Tell a child of a directory to `function`, if it is a name the VFS takes and
 * a file or a directory. */
static void tb_vfs_list_child(const tb_vfs_directory *directory, const char *parent,
                              size_t parent_length, const char *child, size_t child_length,
                              tb_vfs_list_function *function, void *context)
{
    char *full = NULL;
    size_t full_length = 0;
    tb_fs_info native = {0};
    tb_vfs_info info = {0};
    if (!tb_vfs_name_is_valid(child, child_length, false) ||
        memchr(child, '/', child_length) != NULL)
    {
        return;
    }
    full_length = parent_length + (parent_length > 0 ? 1u : 0u) + child_length;
    full = tb_allocator_alloc(&directory->allocator, full_length, 1);
    if (parent_length > 0)
    {
        memcpy(full, parent, parent_length);
        full[parent_length] = '/';
    }
    memcpy(full + full_length - child_length, child, child_length);
    if (tb_vfs_query_native(directory, full, full_length, &native) == TB_VFS_OK &&
        (native.kind == TB_FS_REGULAR || native.kind == TB_FS_DIRECTORY))
    {
        info = native.kind == TB_FS_DIRECTORY
                   ? (tb_vfs_info){.kind = TB_VFS_DIRECTORY}
                   : (tb_vfs_info){.kind = TB_VFS_FILE, .size = native.size};
        function(context, child, child_length, &info);
    }
    tb_allocator_free(&directory->allocator, full, full_length, 1);
}

static tb_vfs_result tb_vfs_directory_list(void *state, const char *name, size_t length,
                                           tb_vfs_list_function *function, void *context)
{
    const tb_vfs_directory *const directory = state;
    tb_fs_info info = {0};
    tb_directory iterator = {0};
    char *path = NULL;
    char *buffer = NULL;
    size_t capacity = TB_VFS_NAME_BUFFER;
    tb_fs_result opened = TB_FS_OK;
    tb_vfs_result result = tb_vfs_resolve(directory, name, length, &info);
    if (result != TB_VFS_OK)
    {
        return result;
    }
    if (info.kind != TB_FS_DIRECTORY)
    {
        return TB_VFS_NOT_A_DIRECTORY;
    }
    path = tb_vfs_native(directory, name, length);
    opened = tb_directory_open((tb_path_view){path, tb_str_length(path)}, &iterator);
    tb_vfs_native_free(directory, path);
    if (opened != TB_FS_OK)
    {
        return tb_vfs_from_fs(opened);
    }
    buffer = tb_allocator_alloc(&directory->allocator, capacity, 1);
    for (;;)
    {
        size_t required = 0;
        const tb_fs_result next = tb_directory_next(&iterator, buffer, capacity, &required);
        if (next == TB_FS_BUFFER_TOO_SMALL)
        {
            buffer = tb_allocator_realloc(&directory->allocator, buffer, capacity, required, 1);
            capacity = required;
            continue;
        }
        if (next == TB_FS_INVALID)
        {
            continue; /* A name that is not Unicode: the VFS could not name it. */
        }
        if (next != TB_FS_OK)
        {
            result = next == TB_FS_END ? TB_VFS_OK : tb_vfs_from_fs(next);
            break;
        }
        tb_vfs_list_child(directory, name, length, buffer, required - 1, function, context);
    }
    tb_allocator_free(&directory->allocator, buffer, capacity, 1);
    (void)tb_directory_close(&iterator);
    return result;
}

static void tb_vfs_directory_release(void *state)
{
    tb_vfs_directory *const directory = state;
    const tb_allocator allocator = directory->allocator;
    tb_allocator_free(&allocator, directory->root, directory->root_length + 1, 1);
    TB_ALLOCATOR_DELETE(&allocator, tb_vfs_directory, directory);
}

static const tb_vfs_source_functions tb_vfs_directory_functions = {
    .open_file = tb_vfs_directory_open,
    .query = tb_vfs_directory_query,
    .list = tb_vfs_directory_list,
    .release = tb_vfs_directory_release,
};

tb_vfs_result tb_vfs_directory_source(const tb_allocator *allocator, const char *path,
                                      tb_vfs_source *source)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    const tb_path_style style = tb_fs_native_style();
    const size_t length = tb_str_length(path);
    tb_path_parts parts = {0};
    tb_fs_info info = {0};
    tb_vfs_directory *directory = NULL;
    tb_fs_result queried = TB_FS_OK;
    if (tb_path_parse(style, (tb_path_view){path, length}, &parts) != TB_PATH_OK ||
        !parts.absolute || parts.verbatim)
    {
        return TB_VFS_INVALID;
    }
    queried = tb_fs_query((tb_path_view){path, length}, &info);
    if (queried != TB_FS_OK)
    {
        return tb_vfs_from_fs(queried);
    }
    if (info.kind != TB_FS_DIRECTORY)
    {
        return TB_VFS_NOT_A_DIRECTORY;
    }
    directory = TB_ALLOCATOR_NEW(&resolved, tb_vfs_directory);
    directory->allocator = resolved;
    directory->style = style;
    directory->root_length = length;
    directory->root = tb_allocator_alloc(&resolved, length + 1, 1);
    memcpy(directory->root, path, length + 1);
    *source = (tb_vfs_source){.functions = &tb_vfs_directory_functions, .state = directory};
    return TB_VFS_OK;
}
