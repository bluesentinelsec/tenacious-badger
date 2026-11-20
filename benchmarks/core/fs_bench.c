/* Whole files, beside the C library's streams: a small one, where the cost is
 * the opening and the closing, and a large one, where it is the bytes.
 *
 * Every case works in a directory of its own under the system's temporary
 * directory, which the benchmark removes when it is done. What is measured
 * is this machine's file system as much as the engine: compare the engine
 * with the C library beside it, not one machine with another.
 *
 * The writes are not alike. tb_fs_write_file() and tb_fs_replace_file() ask
 * the system to put the bytes on the disk before they return; their _cached
 * forms, and fwrite(), leave them in the system's cache. The difference in
 * the numbers is the price of that promise, most of it the disk's. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FS_PATH_CAPACITY ((size_t)32768)
#define FS_SMALL_SIZE ((size_t)4096)
#define FS_LARGE_SIZE ((size_t)4 << 20)
#define FS_SMALL_ROUNDS ((size_t)300)
#define FS_LARGE_ROUNDS ((size_t)8)

typedef struct fs_state
{
    char *root;           /* The directory the files are in. */
    char *path;           /* The file that the cases write and read. */
    unsigned char *bytes; /* What is written, and where the C library reads to. */
    size_t size;          /* Bytes in the file. */
    size_t rounds;        /* Files written or read in a run. */
} fs_state;

static tb_path_view fs_view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

static void write_the_file(void *context)
{
    const fs_state *const state = context;

    (void)tb_fs_write_file(fs_view(state->path), state->bytes, state->size);
}

static void run_write(void *context)
{
    const fs_state *const state = context;
    uint64_t failures = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        if (tb_fs_write_file(fs_view(state->path), state->bytes, state->size) != TB_FS_OK)
        {
            failures++;
        }
    }
    tb_bench_keep(failures);
}

static void run_replace(void *context)
{
    const fs_state *const state = context;
    uint64_t failures = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        if (tb_fs_replace_file(fs_view(state->path), state->bytes, state->size) != TB_FS_OK)
        {
            failures++;
        }
    }
    tb_bench_keep(failures);
}

static void run_write_cached(void *context)
{
    const fs_state *const state = context;
    uint64_t failures = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        if (tb_fs_write_file_cached(fs_view(state->path), state->bytes, state->size) != TB_FS_OK)
        {
            failures++;
        }
    }
    tb_bench_keep(failures);
}

static void run_replace_cached(void *context)
{
    const fs_state *const state = context;
    uint64_t failures = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        if (tb_fs_replace_file_cached(fs_view(state->path), state->bytes, state->size) != TB_FS_OK)
        {
            failures++;
        }
    }
    tb_bench_keep(failures);
}

static void run_read(void *context)
{
    const fs_state *const state = context;
    uint64_t total = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        tb_fs_buffer whole = {0};

        if (tb_fs_read_file(fs_view(state->path), NULL, state->size, &whole) == TB_FS_OK)
        {
            total += whole.length + whole.data[whole.length / 2];
            tb_fs_buffer_free(&whole);
        }
    }
    tb_bench_keep(total);
}

static void run_query(void *context)
{
    const fs_state *const state = context;
    uint64_t total = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        tb_fs_info info = {0};

        (void)tb_fs_query(fs_view(state->path), &info);
        total += info.size;
    }
    tb_bench_keep(total);
}

/* The same with the C library: a stream opened, written or read in one call,
 * and closed. */
static void run_write_with_streams(void *context)
{
    const fs_state *const state = context;
    uint64_t total = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        FILE *const file = fopen(state->path, "wb");

        if (file != NULL)
        {
            total += fwrite(state->bytes, 1, state->size, file);
            (void)fclose(file);
        }
    }
    tb_bench_keep(total);
}

static void run_read_with_streams(void *context)
{
    const fs_state *const state = context;
    uint64_t total = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        FILE *const file = fopen(state->path, "rb");

        if (file != NULL)
        {
            total += fread(state->bytes, 1, state->size, file);
            total += state->bytes[state->size / 2];
            (void)fclose(file);
        }
    }
    tb_bench_keep(total);
}

static void measure_size(tb_bench *bench, fs_state *state, const char *label, size_t size,
                         size_t rounds)
{
    char names[2][96] = {{0}};
    tb_bench_case writing[] = {
        {.variant = "tb_fs_write_file", .run = run_write},
        {.variant = "tb_fs_replace_file", .run = run_replace},
        {.variant = "tb_fs_write_file_cached", .run = run_write_cached},
        {.variant = "tb_fs_replace_file_cached", .run = run_replace_cached},
        {.variant = "fopen, fwrite, fclose", .run = run_write_with_streams},
    };
    tb_bench_case reading[] = {
        {.variant = "tb_fs_read_file", .setup = write_the_file, .run = run_read},
        {.variant = "fopen, fread, fclose", .setup = write_the_file, .run = run_read_with_streams},
    };

    state->size = size;
    state->rounds = rounds;
    /* One of each is enough to see that it runs. */
    if (tb_bench_size(bench, FS_SMALL_ROUNDS * 1000) < FS_SMALL_ROUNDS * 1000)
    {
        state->rounds = 1;
    }
    (void)tb_fmt_buffer(names[0], sizeof(names[0]), "write a file of %s", label);
    (void)tb_fmt_buffer(names[1], sizeof(names[1]), "read a file of %s", label);
    for (size_t index = 0; index < sizeof(writing) / sizeof(writing[0]); index++)
    {
        writing[index].name = names[0];
        writing[index].context = state;
        writing[index].operations = (double)state->rounds;
    }
    for (size_t index = 0; index < 2; index++)
    {
        reading[index].name = names[1];
        reading[index].context = state;
        reading[index].operations = (double)state->rounds;
    }
    tb_bench_measure(bench, "fs", writing, sizeof(writing) / sizeof(writing[0]));
    tb_bench_measure(bench, "fs", reading, 2);
}

void tb_bench_fs(tb_bench *bench)
{
    fs_state state = {0};
    tb_bench_case query = {.name = "ask what a name is and how large",
                           .variant = "tb_fs_query",
                           .setup = write_the_file,
                           .run = run_query};
    size_t required = 0;

    if (!tb_bench_wants(bench, "fs"))
    {
        return;
    }
    state.root = tb_calloc(FS_PATH_CAPACITY, 1);
    state.path = tb_calloc(FS_PATH_CAPACITY, 1);
    state.bytes = tb_malloc(FS_LARGE_SIZE);
    memset(state.bytes, 0x5A, FS_LARGE_SIZE);
    if (tb_fs_temp_directory((tb_path_view){0}, state.root, FS_PATH_CAPACITY) == TB_FS_OK &&
        tb_path_join(tb_fs_native_style(), fs_view(state.root), fs_view("bench.bin"), state.path,
                     FS_PATH_CAPACITY, &required) == TB_PATH_OK)
    {
        measure_size(bench, &state, "4 KiB", FS_SMALL_SIZE, FS_SMALL_ROUNDS);
        measure_size(bench, &state, "4 MiB", FS_LARGE_SIZE, FS_LARGE_ROUNDS);
        state.size = FS_SMALL_SIZE;
        state.rounds = tb_bench_size(bench, FS_SMALL_ROUNDS * 10);
        (void)tb_fs_write_file(fs_view(state.path), state.bytes, state.size);
        query.context = &state;
        query.operations = (double)state.rounds;
        tb_bench_measure(bench, "fs", &query, 1);
        (void)tb_fs_remove(fs_view(state.path));
        (void)tb_fs_remove(fs_view(state.root));
    }
    tb_free(state.root);
    tb_free(state.path);
    tb_free(state.bytes);
}
