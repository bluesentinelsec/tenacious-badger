/* Names the VFS takes, and the sorted index of an archive's names. */
#include "vfs_internal.h"

#include "badger/assert.h"
#include "badger/utf8.h"

#include <stdlib.h>
#include <string.h>

bool tb_vfs_name_is_valid(const char *name, size_t length, bool allow_root)
{
    size_t error = 0;
    size_t start = 0;
    if (length == 0)
    {
        return allow_root;
    }
    if (name == NULL || memchr(name, '\0', length) != NULL || memchr(name, '\\', length) != NULL ||
        memchr(name, ':', length) != NULL || !tb_utf8_validate(name, length, &error))
    {
        return false;
    }
    while (start <= length)
    {
        const char *const slash = memchr(name + start, '/', length - start);
        const size_t end = slash == NULL ? length : (size_t)(slash - name);
        const size_t component = end - start;
        const bool dots = (component == 1 && name[start] == '.') ||
                          (component == 2 && name[start] == '.' && name[start + 1] == '.');
        if (component == 0 || dots)
        {
            return false;
        }
        start = end + 1;
    }
    return true;
}

void tb_vfs_index_init(tb_vfs_index *index, const tb_allocator *allocator)
{
    TB_ASSERT(index != NULL);
    *index = (tb_vfs_index){.allocator = tb_allocator_resolve(allocator)};
}

void tb_vfs_index_free(tb_vfs_index *index)
{
    TB_ASSERT(index != NULL);
    tb_allocator_free(&index->allocator, index->nodes, index->capacity * sizeof(tb_vfs_node),
                      _Alignof(tb_vfs_node));
    tb_allocator_free(&index->allocator, index->names, index->names_capacity, 1);
    *index = (tb_vfs_index){0};
}

static tb_vfs_node *tb_vfs_index_push(tb_vfs_index *index)
{
    if (index->count == index->capacity)
    {
        const size_t capacity = index->capacity == 0 ? 64u : index->capacity * 2;
        TB_ASSERT(index->capacity <= SIZE_MAX / 2 / sizeof(tb_vfs_node));
        index->nodes = tb_allocator_realloc(&index->allocator, index->nodes,
                                            index->capacity * sizeof(tb_vfs_node),
                                            capacity * sizeof(tb_vfs_node), _Alignof(tb_vfs_node));
        index->capacity = capacity;
    }
    index->count++;
    return &index->nodes[index->count - 1];
}

void tb_vfs_index_add(tb_vfs_index *index, const char *name, size_t length, tb_vfs_kind kind,
                      uint64_t size, uint64_t locator)
{
    tb_vfs_node *node = NULL;
    TB_ASSERT(index != NULL);
    TB_ASSERT(tb_vfs_name_is_valid(name, length, false));
    if (index->names_capacity - index->names_length < length)
    {
        size_t capacity = index->names_capacity == 0 ? 4096u : index->names_capacity;
        while (capacity - index->names_length < length)
        {
            TB_ASSERT(capacity <= SIZE_MAX / 2);
            capacity *= 2;
        }
        index->names = tb_allocator_realloc(&index->allocator, index->names, index->names_capacity,
                                            capacity, 1);
        index->names_capacity = capacity;
    }
    memcpy(index->names + index->names_length, name, length);
    node = tb_vfs_index_push(index);
    *node = (tb_vfs_node){
        .name = index->names_length,
        .name_length = length,
        .kind = kind,
        .size = kind == TB_VFS_FILE ? size : 0u,
        .locator = locator,
        .order = index->count,
    };
    index->names_length += length;
}

/* Bytes compared, the shorter first where one begins the other. */
static int tb_vfs_compare_bytes(const char *left, size_t left_length, const char *right,
                                size_t right_length)
{
    const size_t shorter = left_length < right_length ? left_length : right_length;
    const int order = shorter == 0 ? 0 : memcmp(left, right, shorter);
    if (order != 0)
    {
        return order;
    }
    return left_length < right_length ? -1 : (left_length > right_length ? 1 : 0);
}

/* qsort has no context, so the names it compares are found through this. */
static _Thread_local const char *tb_vfs_sorting_names;

static int tb_vfs_compare_nodes(const void *left, const void *right)
{
    const tb_vfs_node *const first = left;
    const tb_vfs_node *const second = right;
    const int order =
        tb_vfs_compare_bytes(tb_vfs_sorting_names + first->name, first->name_length,
                             tb_vfs_sorting_names + second->name, second->name_length);
    if (order != 0)
    {
        return order;
    }
    /* Of one name, the earlier first, so the later file is kept. */
    return first->order < second->order ? -1 : (first->order > second->order ? 1 : 0);
}

tb_vfs_result tb_vfs_index_finish(tb_vfs_index *index)
{
    size_t added = 0;
    size_t kept = 0;
    TB_ASSERT(index != NULL);
    added = index->count;
    /* Each name's parents, as directories: prefixes of the name, which share its bytes. */
    for (size_t entry = 0; entry < added; entry++)
    {
        const size_t name = index->nodes[entry].name;
        const size_t length = index->nodes[entry].name_length;
        for (size_t at = 0; at < length; at++)
        {
            if (index->names[name + at] == '/')
            {
                tb_vfs_node *const parent = tb_vfs_index_push(index);
                *parent = (tb_vfs_node){.name = name,
                                        .name_length = at,
                                        .kind = TB_VFS_DIRECTORY,
                                        .order = index->count};
            }
        }
    }
    if (index->count > 1)
    {
        tb_vfs_sorting_names = index->names;
        qsort(index->nodes, index->count, sizeof(index->nodes[0]), tb_vfs_compare_nodes);
        tb_vfs_sorting_names = NULL;
    }
    for (size_t entry = 0; entry < index->count; entry++)
    {
        const tb_vfs_node *const node = &index->nodes[entry];
        tb_vfs_node *const last = kept == 0 ? NULL : &index->nodes[kept - 1];
        if (last != NULL && tb_vfs_compare_bytes(index->names + last->name, last->name_length,
                                                 index->names + node->name, node->name_length) == 0)
        {
            if (last->kind != node->kind)
            {
                return TB_VFS_MALFORMED;
            }
            /* The same directory again changes nothing; a file again replaces it. */
            *last = *node;
            continue;
        }
        index->nodes[kept] = *node;
        kept++;
    }
    index->count = kept;
    return TB_VFS_OK;
}

const tb_vfs_node *tb_vfs_index_find(const tb_vfs_index *index, const char *name, size_t length)
{
    size_t low = 0;
    size_t high = index->count;
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const tb_vfs_node *const node = &index->nodes[middle];
        const int order =
            tb_vfs_compare_bytes(index->names + node->name, node->name_length, name, length);
        if (order == 0)
        {
            return node;
        }
        if (order < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return NULL;
}

tb_vfs_result tb_vfs_index_query(const tb_vfs_index *index, const char *name, size_t length,
                                 tb_vfs_info *info)
{
    const tb_vfs_node *node = NULL;
    if (length == 0)
    {
        *info = (tb_vfs_info){.kind = TB_VFS_DIRECTORY};
        return TB_VFS_OK;
    }
    node = tb_vfs_index_find(index, name, length);
    if (node == NULL)
    {
        return TB_VFS_NOT_FOUND;
    }
    *info = (tb_vfs_info){.kind = node->kind, .size = node->size};
    return TB_VFS_OK;
}

/* A name compared with `directory` followed by a slash: below 0 if the name
 * comes before every name in the directory, 0 if it is in the directory or
 * after. */
static int tb_vfs_compare_to_prefix(const char *name, size_t name_length, const char *directory,
                                    size_t length)
{
    const size_t shorter = name_length < length ? name_length : length;
    const int order = shorter == 0 ? 0 : memcmp(name, directory, shorter);
    if (order != 0)
    {
        return order;
    }
    if (name_length <= length)
    {
        return -1;
    }
    return name[length] < '/' ? -1 : 0;
}

tb_vfs_result tb_vfs_index_list(const tb_vfs_index *index, const char *directory, size_t length,
                                tb_vfs_list_function *function, void *context)
{
    size_t low = 0;
    size_t high = index->count;
    const size_t prefix = length == 0 ? 0u : length + 1;
    if (length > 0)
    {
        const tb_vfs_node *const node = tb_vfs_index_find(index, directory, length);
        if (node == NULL)
        {
            return TB_VFS_NOT_FOUND;
        }
        if (node->kind != TB_VFS_DIRECTORY)
        {
            return TB_VFS_NOT_A_DIRECTORY;
        }
    }
    /* The first name at or after "directory/": its descendants follow it in order. */
    while (length > 0 && low < high)
    {
        const size_t middle = low + (high - low) / 2;
        const tb_vfs_node *const node = &index->nodes[middle];
        if (tb_vfs_compare_to_prefix(index->names + node->name, node->name_length, directory,
                                     length) < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    for (size_t entry = low; entry < index->count; entry++)
    {
        const tb_vfs_node *const node = &index->nodes[entry];
        const char *const name = index->names + node->name;
        tb_vfs_info info = {.kind = node->kind, .size = node->size};
        if (length > 0 && (node->name_length <= length || memcmp(name, directory, length) != 0 ||
                           name[length] != '/'))
        {
            break;
        }
        /* A child has no slash after the directory's; deeper names are skipped. */
        if (memchr(name + prefix, '/', node->name_length - prefix) != NULL)
        {
            continue;
        }
        function(context, name + prefix, node->name_length - prefix, &info);
    }
    return TB_VFS_OK;
}
