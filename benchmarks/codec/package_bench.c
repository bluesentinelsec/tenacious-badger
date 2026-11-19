/* Package reading of 1,000 entries of 4 KiB of text, plain and obfuscated:
 * what obfuscation adds to loading assets. Operations are bytes of the
 * entries, so the numbers are nanoseconds per byte. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/package.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

#define PACKAGE_ENTRIES ((size_t)1000)
#define PACKAGE_ENTRY_SIZE ((size_t)4096)

static const uint8_t bench_key[TB_PACKAGE_KEY_SIZE] = {42};

typedef struct package_bench_state
{
    const unsigned char *plain;
    size_t plain_length;
    const unsigned char *obfuscated;
    size_t obfuscated_length;
    size_t entries;
} package_bench_state;

static void write_package(tb_stream *sink, const uint8_t *key, size_t entries,
                          const unsigned char *data)
{
    tb_package_writer *writer = NULL;
    char name[64] = {0};
    (void)tb_package_writer_create(sink, key, NULL, &writer);
    for (size_t index = 0; index < entries; index++)
    {
        const size_t length = tb_fmt_buffer(name, sizeof(name), "assets/group-%02zu/file-%04zu.txt",
                                            index % 16, index);
        (void)tb_package_add(writer, name, length, data, PACKAGE_ENTRY_SIZE, 6);
    }
    (void)tb_package_writer_finish(writer);
    tb_package_writer_destroy(writer);
}

static void read_package(const unsigned char *bytes, size_t length, const uint8_t *key)
{
    static unsigned char buffer[PACKAGE_ENTRY_SIZE];
    tb_stream source = {0};
    tb_package *package = NULL;
    uint64_t total = 0;
    tb_stream_open_memory(bytes, length, NULL, &source);
    if (tb_package_open(&source, key, NULL, NULL, &package) == TB_PACKAGE_OK)
    {
        for (size_t index = 0; index < tb_package_count(package); index++)
        {
            tb_stream entry = {0};
            size_t count = 0;
            tb_package_open_entry(package, index, NULL, &entry);
            do
            {
                (void)tb_stream_read(&entry, buffer, sizeof(buffer), &count);
                total += count;
            } while (count > 0);
            (void)tb_stream_close(&entry);
        }
        tb_package_close(package);
    }
    (void)tb_stream_close(&source);
    tb_bench_keep(total);
}

static void run_plain(void *context)
{
    const package_bench_state *const state = context;
    read_package(state->plain, state->plain_length, NULL);
}

static void run_obfuscated(void *context)
{
    const package_bench_state *const state = context;
    read_package(state->obfuscated, state->obfuscated_length, bench_key);
}

void tb_bench_package(tb_bench *bench)
{
    package_bench_state state = {0};
    tb_stream plain = {0};
    tb_stream obfuscated = {0};
    unsigned char *data = NULL;
    tb_bench_case cases[2] = {
        {.name = "read 1,000 entries of 4 KiB", .variant = "plain", .run = run_plain},
        {.name = "read 1,000 entries of 4 KiB", .variant = "obfuscated", .run = run_obfuscated},
    };
    if (!tb_bench_wants(bench, "package"))
    {
        return;
    }
    state.entries = tb_bench_size(bench, PACKAGE_ENTRIES);
    data = tb_malloc(PACKAGE_ENTRY_SIZE);
    for (size_t index = 0; index < PACKAGE_ENTRY_SIZE; index++)
    {
        data[index] = (unsigned char)("a quick brown fox jumps over the lazy dog\n"[index % 42]);
    }
    tb_stream_open_buffer(NULL, &plain);
    tb_stream_open_buffer(NULL, &obfuscated);
    write_package(&plain, NULL, state.entries, data);
    write_package(&obfuscated, bench_key, state.entries, data);
    state.plain = tb_stream_buffer_bytes(&plain, &state.plain_length);
    state.obfuscated = tb_stream_buffer_bytes(&obfuscated, &state.obfuscated_length);
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)(state.entries * PACKAGE_ENTRY_SIZE);
    }
    tb_bench_measure(bench, "package", cases, 2);
    (void)tb_stream_close(&obfuscated);
    (void)tb_stream_close(&plain);
    tb_free(data);
}
