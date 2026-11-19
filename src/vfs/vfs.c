/* The VFS: mounts in the order of their priority, and names routed to them. */
#include "vfs_internal.h"

#include "badger/assert.h"
#include "badger/str.h"
#include "badger/table.h"

#include <stdlib.h>
#include <string.h>

typedef struct tb_vfs_mount_point
{
    char *point; /* NUL after it. */
    size_t point_length;
    tb_vfs_source source;
    int priority;
    uint64_t sequence; /* Later mounts have larger numbers. */
} tb_vfs_mount_point;

struct tb_vfs
{
    tb_allocator allocator;
    tb_vfs_mount_point *mounts; /* Highest priority first; of equal priority, latest first. */
    size_t count;
    size_t capacity;
    uint64_t sequence;
};

tb_vfs *tb_vfs_create(const tb_allocator *allocator)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_vfs *const vfs = TB_ALLOCATOR_NEW(&resolved, tb_vfs);
    vfs->allocator = resolved;
    return vfs;
}

void tb_vfs_destroy(tb_vfs *vfs)
{
    tb_allocator allocator = {0};
    if (vfs == NULL)
    {
        return;
    }
    allocator = vfs->allocator;
    for (size_t index = 0; index < vfs->count; index++)
    {
        tb_vfs_mount_point *const mount = &vfs->mounts[index];
        mount->source.functions->release(mount->source.state);
        tb_allocator_free(&allocator, mount->point, mount->point_length + 1, 1);
    }
    tb_allocator_free(&allocator, vfs->mounts, vfs->capacity * sizeof(tb_vfs_mount_point),
                      _Alignof(tb_vfs_mount_point));
    TB_ALLOCATOR_DELETE(&allocator, tb_vfs, vfs);
}

tb_vfs_result tb_vfs_mount(tb_vfs *vfs, const char *point, size_t point_length,
                           tb_vfs_source source, int priority)
{
    tb_vfs_mount_point mount = {0};
    size_t at = 0;
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(source.functions != NULL && source.functions->open_file != NULL &&
              source.functions->query != NULL && source.functions->list != NULL &&
              source.functions->release != NULL);
    if (!tb_vfs_name_is_valid(point, point_length, true))
    {
        return TB_VFS_INVALID;
    }
    if (vfs->count == vfs->capacity)
    {
        const size_t capacity = vfs->capacity == 0 ? 8u : vfs->capacity * 2;
        TB_ASSERT(vfs->capacity <= SIZE_MAX / 2 / sizeof(tb_vfs_mount_point));
        vfs->mounts = tb_allocator_realloc(
            &vfs->allocator, vfs->mounts, vfs->capacity * sizeof(tb_vfs_mount_point),
            capacity * sizeof(tb_vfs_mount_point), _Alignof(tb_vfs_mount_point));
        vfs->capacity = capacity;
    }
    vfs->sequence++;
    mount.point = tb_allocator_alloc(&vfs->allocator, point_length + 1, 1);
    if (point_length > 0)
    {
        memcpy(mount.point, point, point_length);
    }
    mount.point[point_length] = '\0';
    mount.point_length = point_length;
    mount.source = source;
    mount.priority = priority;
    mount.sequence = vfs->sequence;
    /* Before the first mount of a lower priority: of equal ones, this is the latest. */
    while (at < vfs->count && vfs->mounts[at].priority > priority)
    {
        at++;
    }
    memmove(vfs->mounts + at + 1, vfs->mounts + at, (vfs->count - at) * sizeof(vfs->mounts[0]));
    vfs->mounts[at] = mount;
    vfs->count++;
    return TB_VFS_OK;
}

tb_vfs_result tb_vfs_mount_directory(tb_vfs *vfs, const char *point, size_t point_length,
                                     const char *directory, int priority)
{
    tb_vfs_source source = {0};
    tb_vfs_result result = TB_VFS_OK;
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(directory != NULL);
    if (!tb_vfs_name_is_valid(point, point_length, true))
    {
        return TB_VFS_INVALID;
    }
    result = tb_vfs_directory_source(&vfs->allocator, directory, &source);
    if (result != TB_VFS_OK)
    {
        return result;
    }
    return tb_vfs_mount(vfs, point, point_length, source, priority);
}

static tb_vfs_result tb_vfs_mount_stream(tb_vfs *vfs, const char *point, size_t point_length,
                                         tb_stream *stream, const uint8_t *key, int priority)
{
    tb_vfs_source source = {0};
    const tb_vfs_result result = tb_vfs_archive_source(&vfs->allocator, stream, key, &source);
    if (result != TB_VFS_OK)
    {
        return result;
    }
    return tb_vfs_mount(vfs, point, point_length, source, priority);
}

tb_vfs_result tb_vfs_mount_archive_file(tb_vfs *vfs, const char *point, size_t point_length,
                                        const char *path, const uint8_t *key, int priority)
{
    tb_stream stream = {0};
    tb_fs_result opened = TB_FS_OK;
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    if (!tb_vfs_name_is_valid(point, point_length, true))
    {
        return TB_VFS_INVALID;
    }
    opened = tb_stream_open_file((tb_path_view){path, tb_str_length(path)}, TB_FILE_READ,
                                 &vfs->allocator, &stream);
    if (opened != TB_FS_OK)
    {
        return opened == TB_FS_NOT_FOUND ? TB_VFS_NOT_FOUND : TB_VFS_INPUT_FAILED;
    }
    return tb_vfs_mount_stream(vfs, point, point_length, &stream, key, priority);
}

tb_vfs_result tb_vfs_mount_archive_memory(tb_vfs *vfs, const char *point, size_t point_length,
                                          const void *bytes, size_t length, const uint8_t *key,
                                          int priority)
{
    tb_stream stream = {0};
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    if (!tb_vfs_name_is_valid(point, point_length, true))
    {
        return TB_VFS_INVALID;
    }
    tb_stream_open_memory(bytes, length, &vfs->allocator, &stream);
    return tb_vfs_mount_stream(vfs, point, point_length, &stream, key, priority);
}

/* -------------------------------------------------------------------------
 * Routing names to mounts.
 * ------------------------------------------------------------------------- */

typedef enum tb_vfs_relation
{
    TB_VFS_UNRELATED, /* The name is outside the mount. */
    TB_VFS_INSIDE,    /* The name is the mount's point, or below it. */
    TB_VFS_ABOVE      /* The mount's point is below the name, which is a directory. */
} tb_vfs_relation;

/* How a name relates to a mount; `inner` and `inner_length` are the name
 * within the mount when it is inside. */
static tb_vfs_relation tb_vfs_relate(const tb_vfs_mount_point *mount, const char *name,
                                     size_t length, const char **inner, size_t *inner_length)
{
    const size_t point = mount->point_length;
    if (point == 0)
    {
        *inner = name;
        *inner_length = length;
        return TB_VFS_INSIDE;
    }
    if (length >= point && memcmp(name, mount->point, point) == 0 &&
        (length == point || name[point] == '/'))
    {
        *inner = length == point ? name + length : name + point + 1;
        *inner_length = length == point ? 0u : length - point - 1;
        return TB_VFS_INSIDE;
    }
    if (length < point &&
        (length == 0 || (memcmp(name, mount->point, length) == 0 && mount->point[length] == '/')))
    {
        return TB_VFS_ABOVE;
    }
    return TB_VFS_UNRELATED;
}

tb_vfs_result tb_vfs_open(tb_vfs *vfs, const char *name, size_t length,
                          const tb_allocator *allocator, tb_stream *stream)
{
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    if (!tb_vfs_name_is_valid(name, length, false))
    {
        return TB_VFS_INVALID;
    }
    for (size_t index = 0; index < vfs->count; index++)
    {
        const tb_vfs_mount_point *const mount = &vfs->mounts[index];
        const char *inner = NULL;
        size_t inner_length = 0;
        const tb_vfs_relation relation = tb_vfs_relate(mount, name, length, &inner, &inner_length);
        tb_vfs_result result = TB_VFS_NOT_FOUND;
        if (relation == TB_VFS_ABOVE)
        {
            return TB_VFS_NOT_A_FILE;
        }
        if (relation == TB_VFS_UNRELATED)
        {
            continue;
        }
        result = inner_length == 0
                     ? TB_VFS_NOT_A_FILE
                     : mount->source.functions->open_file(mount->source.state, inner, inner_length,
                                                          allocator, stream);
        if (result != TB_VFS_NOT_FOUND)
        {
            return result;
        }
    }
    return TB_VFS_NOT_FOUND;
}

tb_vfs_result tb_vfs_query(tb_vfs *vfs, const char *name, size_t length, tb_vfs_info *info)
{
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(info != NULL);
    if (!tb_vfs_name_is_valid(name, length, true))
    {
        return TB_VFS_INVALID;
    }
    /* The root is always there, mounts or none. */
    if (length == 0)
    {
        *info = (tb_vfs_info){.kind = TB_VFS_DIRECTORY};
        return TB_VFS_OK;
    }
    for (size_t index = 0; index < vfs->count; index++)
    {
        const tb_vfs_mount_point *const mount = &vfs->mounts[index];
        const char *inner = NULL;
        size_t inner_length = 0;
        const tb_vfs_relation relation = tb_vfs_relate(mount, name, length, &inner, &inner_length);
        tb_vfs_result result = TB_VFS_NOT_FOUND;
        if (relation == TB_VFS_ABOVE)
        {
            *info = (tb_vfs_info){.kind = TB_VFS_DIRECTORY};
            return TB_VFS_OK;
        }
        if (relation == TB_VFS_UNRELATED)
        {
            continue;
        }
        result = mount->source.functions->query(mount->source.state, inner, inner_length, info);
        if (result != TB_VFS_NOT_FOUND)
        {
            return result;
        }
    }
    return TB_VFS_NOT_FOUND;
}

/* -------------------------------------------------------------------------
 * Listing: names from every mount, each once, as the first to tell it says.
 * ------------------------------------------------------------------------- */

typedef struct tb_vfs_listed
{
    char *name; /* NUL after it; the table's key. */
    size_t length;
    tb_vfs_info info;
} tb_vfs_listed;

typedef struct tb_vfs_listing
{
    tb_vfs *vfs;
    tb_table *seen; /* From a name to its tb_vfs_listed. */
} tb_vfs_listing;

static void tb_vfs_collect(void *context, const char *name, size_t length, const tb_vfs_info *info)
{
    tb_vfs_listing *const listing = context;
    const tb_allocator *const allocator = &listing->vfs->allocator;
    tb_vfs_listed *listed = NULL;
    char *key = tb_allocator_alloc(allocator, length + 1, 1);
    memcpy(key, name, length);
    key[length] = '\0';
    if (tb_table_contains(listing->seen, key))
    {
        tb_allocator_free(allocator, key, length + 1, 1);
        return;
    }
    listed = TB_ALLOCATOR_NEW(allocator, tb_vfs_listed);
    *listed = (tb_vfs_listed){.name = key, .length = length, .info = *info};
    (void)tb_table_put(listing->seen, key, listed);
}

static int tb_vfs_compare_listed(const void *left, const void *right)
{
    const tb_vfs_listed *const first = ((const tb_table_entry *)left)->value;
    const tb_vfs_listed *const second = ((const tb_table_entry *)right)->value;
    const size_t shorter = first->length < second->length ? first->length : second->length;
    const int order = memcmp(first->name, second->name, shorter);
    if (order != 0)
    {
        return order;
    }
    return first->length < second->length ? -1 : (first->length > second->length ? 1 : 0);
}

/* Tell the names collected, in order, and free them. */
static void tb_vfs_deliver(tb_vfs_listing *listing, tb_vfs_list_function *function, void *context)
{
    const tb_allocator *const allocator = &listing->vfs->allocator;
    const size_t count = tb_table_length(listing->seen);
    tb_table_entry *const entries =
        TB_ALLOCATOR_NEW_ARRAY(allocator, tb_table_entry, count == 0 ? 1 : count);
    (void)tb_table_to_array(listing->seen, entries, count);
    if (count > 1)
    {
        qsort(entries, count, sizeof(entries[0]), tb_vfs_compare_listed);
    }
    for (size_t index = 0; index < count; index++)
    {
        tb_vfs_listed *const listed = entries[index].value;
        if (function != NULL)
        {
            function(context, listed->name, listed->length, &listed->info);
        }
        tb_allocator_free(allocator, listed->name, listed->length + 1, 1);
        TB_ALLOCATOR_DELETE(allocator, tb_vfs_listed, listed);
    }
    TB_ALLOCATOR_DELETE_ARRAY(allocator, tb_table_entry, entries, count == 0 ? 1 : count);
}

tb_vfs_result tb_vfs_list(tb_vfs *vfs, const char *directory, size_t length,
                          tb_vfs_list_function *function, void *context)
{
    tb_vfs_listing listing = {0};
    tb_vfs_result result = TB_VFS_OK;
    bool found = length == 0; /* The root is always there. */
    bool first_answer = true;
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(function != NULL);
    if (!tb_vfs_name_is_valid(directory, length, true))
    {
        return TB_VFS_INVALID;
    }
    listing.vfs = vfs;
    listing.seen = tb_table_create(&vfs->allocator, 0, tb_table_string_hash, tb_table_string_equal);
    for (size_t index = 0; index < vfs->count && result == TB_VFS_OK; index++)
    {
        const tb_vfs_mount_point *const mount = &vfs->mounts[index];
        const char *inner = NULL;
        size_t inner_length = 0;
        const tb_vfs_relation relation =
            tb_vfs_relate(mount, directory, length, &inner, &inner_length);
        if (relation == TB_VFS_ABOVE)
        {
            /* The next component of the mount's point is a directory here. */
            const char *const below = mount->point + (length == 0 ? 0u : length + 1);
            const char *const slash =
                memchr(below, '/', mount->point_length - (size_t)(below - mount->point));
            const size_t child = slash == NULL
                                     ? mount->point_length - (size_t)(below - mount->point)
                                     : (size_t)(slash - below);
            const tb_vfs_info info = {.kind = TB_VFS_DIRECTORY};
            tb_vfs_collect(&listing, below, child, &info);
            found = true;
            first_answer = false;
        }
        else if (relation == TB_VFS_INSIDE)
        {
            const tb_vfs_result listed = mount->source.functions->list(
                mount->source.state, inner, inner_length, tb_vfs_collect, &listing);
            if (listed == TB_VFS_OK)
            {
                found = true;
            }
            else if (listed == TB_VFS_NOT_A_DIRECTORY && first_answer)
            {
                /* The mount that tells the name first tells what it is: a file. */
                result = TB_VFS_NOT_A_DIRECTORY;
            }
            else if (listed != TB_VFS_NOT_FOUND && listed != TB_VFS_NOT_A_DIRECTORY)
            {
                result = listed;
            }
            if (listed != TB_VFS_NOT_FOUND)
            {
                first_answer = false;
            }
        }
    }
    if (result == TB_VFS_OK && !found)
    {
        result = TB_VFS_NOT_FOUND;
    }
    tb_vfs_deliver(&listing, result == TB_VFS_OK ? function : NULL, context);
    tb_table_destroy(listing.seen);
    return result;
}

const char *tb_vfs_result_name(tb_vfs_result result)
{
    switch (result)
    {
    case TB_VFS_OK:
        return "ok";
    case TB_VFS_INVALID:
        return "invalid";
    case TB_VFS_NOT_FOUND:
        return "not found";
    case TB_VFS_NOT_A_FILE:
        return "not a file";
    case TB_VFS_NOT_A_DIRECTORY:
        return "not a directory";
    case TB_VFS_UNKNOWN_FORMAT:
        return "unknown format";
    case TB_VFS_UNSAFE_NAME:
        return "unsafe name";
    case TB_VFS_MALFORMED:
        return "malformed";
    case TB_VFS_UNSUPPORTED:
        return "unsupported";
    case TB_VFS_KEY_REQUIRED:
        return "key required";
    case TB_VFS_WRONG_KEY:
        return "wrong key";
    case TB_VFS_INPUT_FAILED:
        return "input failed";
    default:
        return "unknown result";
    }
}
