/* A source over an archive: a package, a ZIP archive, or a TAR archive,
 * recognized by its bytes, with every name in an index made when it is
 * mounted. */
#include "vfs_internal.h"

#include "badger/assert.h"
#include "badger/package.h"
#include "badger/tar.h"
#include "badger/zip.h"

#include <string.h>

/* The first bytes of each kind. */
#define TB_VFS_PROBE_SIZE ((size_t)8)
#define TB_VFS_PACKAGE_MAGIC "TBPK\r\n\x1a\n"
#define TB_VFS_GZIP_FIRST 0x1Fu
#define TB_VFS_GZIP_SECOND 0x8Bu

/* A TAR header's data begins one block after it. */
#define TB_VFS_TAR_BLOCK UINT64_C(512)

typedef enum tb_vfs_archive_kind
{
    TB_VFS_ARCHIVE_PACKAGE,
    TB_VFS_ARCHIVE_ZIP,
    TB_VFS_ARCHIVE_TAR
} tb_vfs_archive_kind;

typedef struct tb_vfs_archive
{
    tb_allocator allocator;
    tb_stream stream; /* Owned. */
    tb_vfs_archive_kind kind;
    tb_package *package;
    tb_zip_archive *zip;
    tb_vfs_index index; /* Locators: an entry's index, or for TAR its data's offset. */
} tb_vfs_archive;

static tb_vfs_result tb_vfs_from_package(tb_package_result result)
{
    switch (result)
    {
    case TB_PACKAGE_OK:
        return TB_VFS_OK;
    case TB_PACKAGE_UNSUPPORTED:
        return TB_VFS_UNSUPPORTED;
    case TB_PACKAGE_KEY_REQUIRED:
        return TB_VFS_KEY_REQUIRED;
    case TB_PACKAGE_WRONG_KEY:
        return TB_VFS_WRONG_KEY;
    case TB_PACKAGE_INPUT_FAILED:
        return TB_VFS_INPUT_FAILED;
    case TB_PACKAGE_INVALID:
    case TB_PACKAGE_MALFORMED:
    case TB_PACKAGE_TOO_LARGE:
    case TB_PACKAGE_DUPLICATE_NAME:
    case TB_PACKAGE_RANDOM_FAILED:
    case TB_PACKAGE_OUTPUT_FAILED:
    default:
        return TB_VFS_MALFORMED;
    }
}

static tb_vfs_result tb_vfs_from_zip(tb_zip_result result)
{
    switch (result)
    {
    case TB_ZIP_OK:
        return TB_VFS_OK;
    case TB_ZIP_UNSUPPORTED:
        return TB_VFS_UNSUPPORTED;
    case TB_ZIP_INPUT_FAILED:
        return TB_VFS_INPUT_FAILED;
    case TB_ZIP_INVALID:
    case TB_ZIP_MALFORMED:
    case TB_ZIP_TOO_LARGE:
    case TB_ZIP_DUPLICATE_NAME:
    case TB_ZIP_OUTPUT_FAILED:
    default:
        return TB_VFS_MALFORMED;
    }
}

static tb_vfs_result tb_vfs_from_tar(tb_tar_result result)
{
    switch (result)
    {
    case TB_TAR_OK:
    case TB_TAR_END:
        return TB_VFS_OK;
    case TB_TAR_UNSUPPORTED:
        return TB_VFS_UNSUPPORTED;
    case TB_TAR_INPUT_FAILED:
        return TB_VFS_INPUT_FAILED;
    case TB_TAR_INVALID:
    case TB_TAR_MALFORMED:
    case TB_TAR_TRUNCATED:
    case TB_TAR_TOO_LARGE:
    case TB_TAR_OUTPUT_FAILED:
    default:
        return TB_VFS_MALFORMED;
    }
}

/* -------------------------------------------------------------------------
 * Indexing each kind.
 * ------------------------------------------------------------------------- */

static tb_vfs_result tb_vfs_index_package(tb_vfs_archive *archive, const uint8_t *key)
{
    const tb_vfs_result opened = tb_vfs_from_package(
        tb_package_open(&archive->stream, key, NULL, &archive->allocator, &archive->package));
    if (opened != TB_VFS_OK)
    {
        return opened;
    }
    for (size_t index = 0; index < tb_package_count(archive->package); index++)
    {
        tb_package_entry entry = {0};
        tb_package_entry_at(archive->package, index, &entry);
        if (!tb_vfs_name_is_valid(entry.name, entry.name_length, false))
        {
            return TB_VFS_UNSAFE_NAME;
        }
        tb_vfs_index_add(&archive->index, entry.name, entry.name_length, TB_VFS_FILE, entry.size,
                         index);
    }
    return TB_VFS_OK;
}

static tb_vfs_result tb_vfs_index_zip(tb_vfs_archive *archive)
{
    const tb_vfs_result opened =
        tb_vfs_from_zip(tb_zip_open(&archive->stream, NULL, &archive->allocator, &archive->zip));
    if (opened != TB_VFS_OK)
    {
        return opened;
    }
    for (size_t index = 0; index < tb_zip_count(archive->zip); index++)
    {
        tb_zip_entry entry = {0};
        tb_zip_entry_at(archive->zip, index, &entry);
        if (!tb_vfs_name_is_valid(entry.name, entry.name_length, false))
        {
            return TB_VFS_UNSAFE_NAME;
        }
        /* Links are neither files nor directories here, and are not followed. */
        if (entry.type == TB_ZIP_SYMLINK)
        {
            continue;
        }
        tb_vfs_index_add(&archive->index, entry.name, entry.name_length,
                         entry.type == TB_ZIP_DIRECTORY ? TB_VFS_DIRECTORY : TB_VFS_FILE,
                         entry.size, index);
    }
    return TB_VFS_OK;
}

static tb_vfs_result tb_vfs_index_tar(tb_vfs_archive *archive)
{
    tb_tar_reader *const reader = tb_tar_reader_create(&archive->stream, NULL, &archive->allocator);
    tb_vfs_result result = TB_VFS_OK;
    bool first = true;
    for (;;)
    {
        tb_tar_entry entry = {0};
        const tb_tar_result next = tb_tar_reader_next(reader, &entry);
        if (next != TB_TAR_OK)
        {
            /* Bytes that are not even one header are another format. */
            result = first && next == TB_TAR_MALFORMED && tb_tar_reader_offset(reader) == 0
                         ? TB_VFS_UNKNOWN_FORMAT
                         : tb_vfs_from_tar(next);
            break;
        }
        first = false;
        if (!tb_vfs_name_is_valid(entry.name, entry.name_length, false))
        {
            result = TB_VFS_UNSAFE_NAME;
            break;
        }
        if (entry.type == TB_TAR_FILE)
        {
            /* The reader's offset is of the entry's own header; its data follows it. */
            tb_vfs_index_add(&archive->index, entry.name, entry.name_length, TB_VFS_FILE,
                             entry.size, tb_tar_reader_offset(reader) + TB_VFS_TAR_BLOCK);
        }
        else if (entry.type == TB_TAR_DIRECTORY)
        {
            tb_vfs_index_add(&archive->index, entry.name, entry.name_length, TB_VFS_DIRECTORY, 0,
                             0);
        }
    }
    tb_tar_reader_destroy(reader);
    return result;
}

/* Which kind the bytes are, by their start. */
static tb_vfs_result tb_vfs_probe(tb_vfs_archive *archive, tb_vfs_archive_kind *kind)
{
    unsigned char start[TB_VFS_PROBE_SIZE] = {0};
    uint64_t size = 0;
    if (tb_stream_size(&archive->stream, &size) != TB_STREAM_OK ||
        archive->stream.functions->read_bytes_at == NULL)
    {
        return TB_VFS_UNSUPPORTED;
    }
    if (size < TB_VFS_PROBE_SIZE)
    {
        return TB_VFS_UNKNOWN_FORMAT;
    }
    if (tb_stream_read_exact_at(&archive->stream, 0, start, sizeof(start)) != TB_STREAM_OK)
    {
        return TB_VFS_INPUT_FAILED;
    }
    if (memcmp(start, TB_VFS_PACKAGE_MAGIC, TB_VFS_PROBE_SIZE) == 0)
    {
        *kind = TB_VFS_ARCHIVE_PACKAGE;
    }
    else if (start[0] == 'P' && start[1] == 'K')
    {
        *kind = TB_VFS_ARCHIVE_ZIP;
    }
    else if (start[0] == TB_VFS_GZIP_FIRST && start[1] == TB_VFS_GZIP_SECOND)
    {
        /* A compressed TAR cannot be read at offsets: decompress it first. */
        return TB_VFS_UNSUPPORTED;
    }
    else
    {
        *kind = TB_VFS_ARCHIVE_TAR;
    }
    return TB_VFS_OK;
}

/* -------------------------------------------------------------------------
 * The source.
 * ------------------------------------------------------------------------- */

static tb_vfs_result tb_vfs_archive_open(void *state, const char *name, size_t length,
                                         const tb_allocator *allocator, tb_stream *stream)
{
    tb_vfs_archive *const archive = state;
    const tb_vfs_node *const node = tb_vfs_index_find(&archive->index, name, length);
    if (length == 0 || (node != NULL && node->kind == TB_VFS_DIRECTORY))
    {
        return TB_VFS_NOT_A_FILE;
    }
    if (node == NULL)
    {
        return TB_VFS_NOT_FOUND;
    }
    switch (archive->kind)
    {
    case TB_VFS_ARCHIVE_PACKAGE:
        tb_package_open_entry(archive->package, (size_t)node->locator, allocator, stream);
        return TB_VFS_OK;
    case TB_VFS_ARCHIVE_ZIP:
        return tb_vfs_from_zip(
            tb_zip_open_entry(archive->zip, (size_t)node->locator, allocator, stream));
    case TB_VFS_ARCHIVE_TAR:
    default:
        return tb_stream_open_range(&archive->stream, node->locator, node->size, allocator,
                                    stream) == TB_STREAM_OK
                   ? TB_VFS_OK
                   : TB_VFS_MALFORMED;
    }
}

static tb_vfs_result tb_vfs_archive_query(void *state, const char *name, size_t length,
                                          tb_vfs_info *info)
{
    const tb_vfs_archive *const archive = state;
    return tb_vfs_index_query(&archive->index, name, length, info);
}

static tb_vfs_result tb_vfs_archive_list(void *state, const char *directory, size_t length,
                                         tb_vfs_list_function *function, void *context)
{
    const tb_vfs_archive *const archive = state;
    return tb_vfs_index_list(&archive->index, directory, length, function, context);
}

static void tb_vfs_archive_release(void *state)
{
    tb_vfs_archive *const archive = state;
    const tb_allocator allocator = archive->allocator;
    tb_package_close(archive->package);
    tb_zip_close(archive->zip);
    tb_vfs_index_free(&archive->index);
    (void)tb_stream_close(&archive->stream);
    TB_ALLOCATOR_DELETE(&allocator, tb_vfs_archive, archive);
}

static const tb_vfs_source_functions tb_vfs_archive_functions = {
    .open_file = tb_vfs_archive_open,
    .query = tb_vfs_archive_query,
    .list = tb_vfs_archive_list,
    .release = tb_vfs_archive_release,
};

tb_vfs_result tb_vfs_archive_source(const tb_allocator *allocator, tb_stream *stream,
                                    const uint8_t *key, tb_vfs_source *source)
{
    const tb_allocator resolved = tb_allocator_resolve(allocator);
    tb_vfs_archive *const archive = TB_ALLOCATOR_NEW(&resolved, tb_vfs_archive);
    tb_vfs_result result = TB_VFS_OK;
    archive->allocator = resolved;
    archive->stream = *stream;
    *stream = (tb_stream){0};
    tb_vfs_index_init(&archive->index, &resolved);
    result = tb_vfs_probe(archive, &archive->kind);
    if (result == TB_VFS_OK)
    {
        switch (archive->kind)
        {
        case TB_VFS_ARCHIVE_PACKAGE:
            result = tb_vfs_index_package(archive, key);
            break;
        case TB_VFS_ARCHIVE_ZIP:
            result = tb_vfs_index_zip(archive);
            break;
        case TB_VFS_ARCHIVE_TAR:
        default:
            result = tb_vfs_index_tar(archive);
            break;
        }
    }
    if (result == TB_VFS_OK)
    {
        result = tb_vfs_index_finish(&archive->index);
    }
    if (result != TB_VFS_OK)
    {
        tb_vfs_archive_release(archive);
        return result;
    }
    *source = (tb_vfs_source){.functions = &tb_vfs_archive_functions, .state = archive};
    return TB_VFS_OK;
}
