/* ZIP writing and reading of an archive like an export: 1,000 files of
 * 4 KiB of text under a few directories, deflated at level 6. Operations
 * are bytes of the files, so the numbers are nanoseconds per byte. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/stream.h"
#include "badger/zip.h"

#include <stddef.h>
#include <stdint.h>

#define ZIP_FILES ((size_t)1000)
#define ZIP_FILE_SIZE ((size_t)4096)

typedef struct zip_state
{
    unsigned char *data; /* One file's data; every file has it. */
    size_t files;
    const unsigned char *archive; /* Written once, for the reader. */
    size_t length;
} zip_state;

static void write_archive(const zip_state *state, tb_stream *sink)
{
    tb_zip_writer *const writer = tb_zip_writer_create(sink, NULL);
    char name[64] = {0};
    for (size_t index = 0; index < state->files; index++)
    {
        tb_zip_entry entry = {0};
        entry.name = name;
        entry.name_length = tb_fmt_buffer(name, sizeof(name), "assets/group-%02zu/file-%04zu.txt",
                                          index % 16, index);
        entry.type = TB_ZIP_FILE;
        entry.method = TB_ZIP_DEFLATED;
        entry.size = ZIP_FILE_SIZE;
        entry.modified = 1700000000;
        (void)tb_zip_write_entry(writer, &entry, 6);
        (void)tb_zip_write_data(writer, state->data, ZIP_FILE_SIZE);
    }
    (void)tb_zip_writer_finish(writer);
    tb_zip_writer_destroy(writer);
}

static void run_write(void *context)
{
    const zip_state *const state = context;
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
    const zip_state *const state = context;
    static unsigned char buffer[16384];
    tb_stream source = {0};
    tb_zip_archive *archive = NULL;
    uint64_t total = 0;
    tb_stream_open_memory(state->archive, state->length, NULL, &source);
    if (tb_zip_open(&source, NULL, NULL, &archive) == TB_ZIP_OK)
    {
        for (size_t index = 0; index < tb_zip_count(archive); index++)
        {
            tb_stream entry = {0};
            size_t count = 0;
            (void)tb_zip_open_entry(archive, index, NULL, &entry);
            do
            {
                (void)tb_stream_read(&entry, buffer, sizeof(buffer), &count);
                total += count;
            } while (count > 0);
            (void)tb_stream_close(&entry);
        }
        tb_zip_close(archive);
    }
    (void)tb_stream_close(&source);
    tb_bench_keep(total);
}

void tb_bench_zip(tb_bench *bench)
{
    zip_state state = {0};
    tb_stream archive = {0};
    tb_bench_case cases[2] = {
        {.name = "write an archive of 1,000 files",
         .variant = "tb_zip_write_data, level 6",
         .run = run_write},
        {.name = "read an archive of 1,000 files", .variant = "tb_zip_open_entry", .run = run_read},
    };
    if (!tb_bench_wants(bench, "zip"))
    {
        return;
    }
    state.files = tb_bench_size(bench, ZIP_FILES);
    state.data = tb_malloc(ZIP_FILE_SIZE);
    for (size_t index = 0; index < ZIP_FILE_SIZE; index++)
    {
        state.data[index] =
            (unsigned char)("the quick brown fox jumps over a lazy dog\n"[index % 42]);
    }
    tb_stream_open_buffer(NULL, &archive);
    write_archive(&state, &archive);
    state.archive = tb_stream_buffer_bytes(&archive, &state.length);
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)(state.files * ZIP_FILE_SIZE);
    }
    tb_bench_measure(bench, "zip", cases, 2);
    (void)tb_stream_close(&archive);
    tb_free(state.data);
}
