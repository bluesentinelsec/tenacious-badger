/* TAR reading and writing of an archive like an export: 1,000 files of
 * 4 KiB under a few directories. Operations are bytes of archive, so the
 * numbers are nanoseconds per byte. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/stream.h"
#include "badger/tar.h"

#include <stddef.h>
#include <stdint.h>

#define TAR_FILES ((size_t)1000)
#define TAR_FILE_SIZE ((size_t)4096)

typedef struct tar_state
{
    unsigned char *data; /* One file's data; every file has it. */
    size_t files;
    const unsigned char *archive; /* Written once, for the reader. */
    size_t length;
} tar_state;

static void write_archive(const tar_state *state, tb_stream *sink)
{
    tb_tar_writer *const writer = tb_tar_writer_create(sink, NULL);
    char name[64] = {0};
    for (size_t index = 0; index < state->files; index++)
    {
        tb_tar_entry entry = {0};
        entry.name = name;
        entry.name_length = tb_fmt_buffer(name, sizeof(name), "assets/group-%02zu/file-%04zu.bin",
                                          index % 16, index);
        entry.link = "";
        entry.type = TB_TAR_FILE;
        entry.size = TAR_FILE_SIZE;
        entry.modified = 1700000000;
        (void)tb_tar_write_entry(writer, &entry);
        (void)tb_tar_write_data(writer, state->data, TAR_FILE_SIZE);
    }
    (void)tb_tar_writer_finish(writer);
    tb_tar_writer_destroy(writer);
}

static void run_write(void *context)
{
    const tar_state *const state = context;
    tb_stream sink = {0};
    size_t length = 0;
    tb_stream_open_buffer(NULL, &sink);
    write_archive(state, &sink);
    (void)tb_stream_buffer_bytes(&sink, &length);
    (void)tb_stream_close(&sink);
    tb_bench_keep(length);
}

static void run_read(void *context)
{
    const tar_state *const state = context;
    static unsigned char buffer[16384];
    tb_stream source = {0};
    tb_tar_reader *reader = NULL;
    tb_tar_entry entry = {0};
    uint64_t total = 0;
    tb_stream_open_memory(state->archive, state->length, NULL, &source);
    reader = tb_tar_reader_create(&source, NULL, NULL);
    while (tb_tar_reader_next(reader, &entry) == TB_TAR_OK)
    {
        size_t count = 0;
        do
        {
            (void)tb_tar_reader_read(reader, buffer, sizeof(buffer), &count);
            total += count;
        } while (count > 0);
    }
    tb_tar_reader_destroy(reader);
    (void)tb_stream_close(&source);
    tb_bench_keep(total);
}

void tb_bench_tar(tb_bench *bench)
{
    tar_state state = {0};
    tb_stream archive = {0};
    uint64_t random = 1;
    tb_bench_case cases[2] = {
        {.name = "write an archive of 1,000 files",
         .variant = "tb_tar_write_data",
         .run = run_write},
        {.name = "read an archive of 1,000 files",
         .variant = "tb_tar_reader_read",
         .run = run_read},
    };
    if (!tb_bench_wants(bench, "tar"))
    {
        return;
    }
    state.files = tb_bench_size(bench, TAR_FILES);
    state.data = tb_malloc(TAR_FILE_SIZE);
    for (size_t index = 0; index < TAR_FILE_SIZE; index++)
    {
        state.data[index] = (unsigned char)tb_bench_random(&random);
    }
    tb_stream_open_buffer(NULL, &archive);
    write_archive(&state, &archive);
    state.archive = tb_stream_buffer_bytes(&archive, &state.length);
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)state.length;
    }
    tb_bench_measure(bench, "tar", cases, 2);
    (void)tb_stream_close(&archive);
    tb_free(state.data);
}
