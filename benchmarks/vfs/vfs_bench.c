/* Opening files through the VFS: a package of 1,000 entries under nine mods
 * of 100 entries each, all in memory, so the numbers are the cost of
 * finding a name across ten mounts and opening it, per file. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/package.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include <stddef.h>
#include <stdint.h>

#define VFS_BASE_ENTRIES ((size_t)1000)
#define VFS_MODS ((size_t)9)
#define VFS_MOD_ENTRIES ((size_t)100)

typedef struct vfs_bench_state
{
    tb_vfs *vfs;
    size_t files;
} vfs_bench_state;

static void write_package(tb_stream *sink, size_t first, size_t count)
{
    tb_package_writer *writer = NULL;
    char name[64] = {0};
    (void)tb_package_writer_create(sink, NULL, NULL, &writer);
    for (size_t index = first; index < first + count; index++)
    {
        const size_t length = tb_fmt_buffer(name, sizeof(name), "assets/group-%02zu/file-%04zu.txt",
                                            index % 16, index);
        (void)tb_package_add(writer, name, length, name, length, 0);
    }
    (void)tb_package_writer_finish(writer);
    tb_package_writer_destroy(writer);
}

static void run_open(void *context)
{
    const vfs_bench_state *const state = context;
    char name[64] = {0};
    uint64_t total = 0;
    for (size_t index = 0; index < state->files; index++)
    {
        tb_stream stream = {0};
        const size_t length = tb_fmt_buffer(name, sizeof(name), "assets/group-%02zu/file-%04zu.txt",
                                            index % 16, index);
        if (tb_vfs_open(state->vfs, name, length, NULL, &stream) == TB_VFS_OK)
        {
            uint8_t byte = 0;
            size_t count = 0;
            (void)tb_stream_read(&stream, &byte, 1, &count);
            total += count;
            (void)tb_stream_close(&stream);
        }
    }
    tb_bench_keep(total);
}

void tb_bench_vfs(tb_bench *bench)
{
    vfs_bench_state state = {0};
    tb_stream buffers[1 + VFS_MODS] = {0};
    tb_bench_case open = {
        .name = "open a file across ten mounts", .variant = "tb_vfs_open", .run = run_open};
    if (!tb_bench_wants(bench, "vfs"))
    {
        return;
    }
    state.files = tb_bench_size(bench, VFS_BASE_ENTRIES);
    state.vfs = tb_vfs_create(NULL);
    for (size_t mount = 0; mount <= VFS_MODS; mount++)
    {
        const unsigned char *bytes = NULL;
        size_t length = 0;
        tb_stream_open_buffer(NULL, &buffers[mount]);
        /* The base has every file; each mod replaces a hundred of them. */
        write_package(&buffers[mount], mount == 0 ? 0 : (mount - 1) * VFS_MOD_ENTRIES,
                      mount == 0 ? state.files : VFS_MOD_ENTRIES);
        bytes = tb_stream_buffer_bytes(&buffers[mount], &length);
        (void)tb_vfs_mount_archive_memory(state.vfs, "", 0, bytes, length, NULL, (int)mount);
    }
    open.context = &state;
    open.operations = (double)state.files;
    tb_bench_measure(bench, "vfs", &open, 1);
    tb_vfs_destroy(state.vfs);
    for (size_t mount = 0; mount <= VFS_MODS; mount++)
    {
        (void)tb_stream_close(&buffers[mount]);
    }
}
