/* What the VFS's parts share: names, the index of an archive, and the
 * sources it builds in. */
#ifndef TB_VFS_INTERNAL_H
#define TB_VFS_INTERNAL_H

#include "badger/vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether a name is one the VFS takes: UTF-8, components between single
 * slashes, none empty, ".", or "..", and no '\\', ':', or NUL. The empty
 * name, the root, only when `allow_root`. */
bool tb_vfs_name_is_valid(const char *name, size_t length, bool allow_root);

/* -------------------------------------------------------------------------
 * The index of an archive: every name it has, and every directory those
 * names imply, sorted by their bytes.
 * ------------------------------------------------------------------------- */

typedef struct tb_vfs_node
{
    size_t name; /* Offset of the name in the index's `names`. */
    size_t name_length;
    tb_vfs_kind kind;
    uint64_t size;
    uint64_t locator; /* What the source needs to open it: an index, or an offset. */
    size_t order;     /* The order it was added in: a later file of one name wins. */
} tb_vfs_node;

typedef struct tb_vfs_index
{
    tb_allocator allocator;
    tb_vfs_node *nodes;
    size_t count;
    size_t capacity;
    char *names;
    size_t names_length;
    size_t names_capacity;
} tb_vfs_index;

/* Start an index; tb_vfs_index_free() releases it. */
void tb_vfs_index_init(tb_vfs_index *index, const tb_allocator *allocator);
void tb_vfs_index_free(tb_vfs_index *index);

/* Add a file or a directory. Its name must already be valid. */
void tb_vfs_index_add(tb_vfs_index *index, const char *name, size_t length, tb_vfs_kind kind,
                      uint64_t size, uint64_t locator);

/* Add the directories that names imply, sort, and merge names that repeat:
 * directories with directories, and files with files (the later wins). A
 * name that is both a file and a directory is MALFORMED. */
tb_vfs_result tb_vfs_index_finish(tb_vfs_index *index);

/* The node of a name, or NULL. The root is not a node. */
const tb_vfs_node *tb_vfs_index_find(const tb_vfs_index *index, const char *name, size_t length);

/* Query and list, for sources built on an index. */
tb_vfs_result tb_vfs_index_query(const tb_vfs_index *index, const char *name, size_t length,
                                 tb_vfs_info *info);
tb_vfs_result tb_vfs_index_list(const tb_vfs_index *index, const char *directory, size_t length,
                                tb_vfs_list_function *function, void *context);

/* -------------------------------------------------------------------------
 * The sources the VFS builds in.
 * ------------------------------------------------------------------------- */

/* A directory on disk, at an absolute path. */
tb_vfs_result tb_vfs_directory_source(const tb_allocator *allocator, const char *directory,
                                      tb_vfs_source *source);

/* An archive in `stream`, which the source takes and closes when released,
 * on failure too. `key` is for an obfuscated package, or NULL. */
tb_vfs_result tb_vfs_archive_source(const tb_allocator *allocator, tb_stream *stream,
                                    const uint8_t *key, tb_vfs_source *source);

#endif
