#include "badger/assert.h"
#include "stream_internal.h"

#include <stdbool.h>

/* A file that the stream owns. The stream keeps its own position and uses
 * only the positional calls, so reads at an offset never disturb it and run
 * together from many threads. */
typedef struct tb_stream_file
{
    tb_file file;
    uint64_t position;
    tb_allocator allocator;
} tb_stream_file;

static tb_stream_result tb_stream_from_fs(tb_fs_result result)
{
    switch (result)
    {
    case TB_FS_OK:
        return TB_STREAM_OK;
    case TB_FS_INVALID:
        return TB_STREAM_INVALID;
    case TB_FS_NO_MEMORY:
        return TB_STREAM_NO_MEMORY;
    case TB_FS_TOO_LARGE:
        return TB_STREAM_TOO_LARGE;
    case TB_FS_NO_SPACE:
        return TB_STREAM_NO_SPACE;
    case TB_FS_ACCESS:
        return TB_STREAM_ACCESS;
    default:
        return TB_STREAM_OS_ERROR;
    }
}

static tb_stream_result tb_stream_file_read_at(void *state, uint64_t offset, void *bytes,
                                               size_t capacity, size_t *transferred)
{
    tb_stream_file *const stream = state;
    return tb_stream_from_fs(tb_file_read_at(&stream->file, offset, bytes, capacity, transferred));
}

static tb_stream_result tb_stream_file_read(void *state, void *bytes, size_t capacity,
                                            size_t *transferred)
{
    tb_stream_file *const stream = state;
    const tb_stream_result result =
        tb_stream_file_read_at(state, stream->position, bytes, capacity, transferred);
    stream->position += *transferred;
    return result;
}

static tb_stream_result tb_stream_file_write(void *state, const void *bytes, size_t length,
                                             size_t *transferred)
{
    tb_stream_file *const stream = state;
    const tb_fs_result result =
        tb_file_write_at(&stream->file, stream->position, bytes, length, transferred);
    stream->position += *transferred;
    return tb_stream_from_fs(result);
}

static tb_stream_result tb_stream_file_size(void *state, uint64_t *size)
{
    tb_stream_file *const stream = state;
    return tb_stream_from_fs(tb_file_size(&stream->file, size));
}

static tb_stream_result tb_stream_file_seek(void *state, int64_t offset, tb_stream_origin origin,
                                            uint64_t *position)
{
    tb_stream_file *const stream = state;
    uint64_t size = {0};
    tb_stream_result result = {0};
    /* Only a seek from the end needs the size, which costs a system call. */
    if (origin == TB_STREAM_FROM_END)
    {
        result = tb_stream_file_size(state, &size);
        if (result != TB_STREAM_OK)
        {
            return result;
        }
    }
    result = tb_stream_resolve_seek(stream->position, size, offset, origin, &stream->position);
    *position = stream->position;
    return result;
}

static tb_stream_result tb_stream_file_flush(void *state)
{
    tb_stream_file *const stream = state;
    return tb_stream_from_fs(tb_file_flush(&stream->file));
}

static tb_stream_result tb_stream_file_close(void *state)
{
    tb_stream_file *const stream = state;
    const tb_allocator allocator = stream->allocator;
    const tb_fs_result result = tb_file_close(&stream->file);
    TB_ALLOCATOR_DELETE(&allocator, tb_stream_file, stream);
    return tb_stream_from_fs(result);
}

/* What a file stream offers follows from the flags it was opened with. */
static const tb_stream_functions tb_stream_file_read_functions = {
    .read_bytes = tb_stream_file_read,
    .seek = tb_stream_file_seek,
    .size = tb_stream_file_size,
    .read_bytes_at = tb_stream_file_read_at,
    .release = tb_stream_file_close,
};

static const tb_stream_functions tb_stream_file_write_functions = {
    .write_bytes = tb_stream_file_write,
    .seek = tb_stream_file_seek,
    .size = tb_stream_file_size,
    .flush = tb_stream_file_flush,
    .release = tb_stream_file_close,
};

static const tb_stream_functions tb_stream_file_read_write_functions = {
    .read_bytes = tb_stream_file_read,
    .write_bytes = tb_stream_file_write,
    .seek = tb_stream_file_seek,
    .size = tb_stream_file_size,
    .read_bytes_at = tb_stream_file_read_at,
    .flush = tb_stream_file_flush,
    .release = tb_stream_file_close,
};

static const tb_stream_functions *tb_stream_file_functions(unsigned flags)
{
    const bool reads = (flags & TB_FILE_READ) != 0;
    const bool writes = (flags & TB_FILE_WRITE) != 0;
    if (reads && writes)
    {
        return &tb_stream_file_read_write_functions;
    }
    if (writes)
    {
        return &tb_stream_file_write_functions;
    }
    return &tb_stream_file_read_functions;
}

tb_fs_result tb_stream_open_file(tb_path_view path, unsigned flags, const tb_allocator *allocator,
                                 tb_stream *stream)
{
    tb_allocator resolved = {0};
    tb_stream_file *state = NULL;
    tb_file file = {0};
    tb_fs_result result = {0};
    TB_ASSERT(stream != NULL);
    TB_ASSERT_MSG(stream->functions == NULL, "the output stream is already open");
    result = tb_file_open(path, flags, &file);
    if (result != TB_FS_OK)
    {
        return result;
    }
    resolved = tb_allocator_resolve(allocator);
    state = TB_ALLOCATOR_NEW(&resolved, tb_stream_file);
    *state = (tb_stream_file){0};
    state->file = file;
    state->allocator = resolved;
    stream->functions = tb_stream_file_functions(flags);
    stream->state = state;
    return TB_FS_OK;
}
