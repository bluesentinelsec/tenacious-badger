/* Streams over memory, beside memcpy: what the interface costs for each call
 * it makes through its table, at a size where the call is most of the work
 * and at one where the bytes are. Files are left to the fs benchmarks, so
 * the numbers here are the engine's and not the disk's. */
#include "tb_bench.h"

#include "badger/memory.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STREAM_SOURCE_SIZE ((size_t)1 << 20)
#define STREAM_SMALL_PIECE ((size_t)64)
#define STREAM_LARGE_PIECE ((size_t)4096)
#define STREAM_RANGE_READS ((size_t)100000)

typedef struct stream_state
{
    unsigned char *source; /* The bytes that are read. */
    unsigned char *output; /* Where the reads go: a piece of the largest size. */
    size_t size;           /* Bytes in the source that a run reads. */
    size_t piece;          /* Bytes each call reads or writes. */
} stream_state;

static void run_stream_read(void *context)
{
    const stream_state *const state = context;
    tb_stream stream = {0};
    uint64_t total = 0;
    tb_stream_open_memory(state->source, state->size, NULL, &stream);
    for (;;)
    {
        size_t count = 0;
        if (tb_stream_read(&stream, state->output, state->piece, &count) != TB_STREAM_OK ||
            count == 0)
        {
            break;
        }
        total += state->output[count - 1];
    }
    (void)tb_stream_close(&stream);
    tb_bench_keep(total);
}

static void run_memcpy(void *context)
{
    const stream_state *const state = context;
    uint64_t total = 0;
    for (size_t offset = 0; offset < state->size; offset += state->piece)
    {
        memcpy(state->output, state->source + offset, state->piece);
        total += state->output[state->piece - 1];
    }
    tb_bench_keep(total);
}

static void run_buffer_write(void *context)
{
    const stream_state *const state = context;
    tb_stream stream = {0};
    size_t length = 0;
    tb_stream_open_buffer(NULL, &stream);
    for (size_t offset = 0; offset < state->size; offset += state->piece)
    {
        (void)tb_stream_write_all(&stream, state->source + offset, state->piece);
    }
    (void)tb_stream_buffer_bytes(&stream, &length);
    (void)tb_stream_close(&stream);
    tb_bench_keep(length);
}

static void run_range_read_at(void *context)
{
    const stream_state *const state = context;
    tb_stream parent = {0};
    tb_stream range = {0};
    uint64_t random = 1;
    uint64_t total = 0;
    tb_stream_open_memory(state->source, state->size, NULL, &parent);
    (void)tb_stream_open_range(&parent, state->size / 4, state->size / 2, NULL, &range);
    for (size_t read = 0; read < STREAM_RANGE_READS; read++)
    {
        const uint64_t offset = tb_bench_random(&random) % (state->size / 2);
        size_t count = 0;
        (void)tb_stream_read_at(&range, offset, state->output, STREAM_SMALL_PIECE, &count);
        total += count;
    }
    (void)tb_stream_close(&range);
    (void)tb_stream_close(&parent);
    tb_bench_keep(total);
}

static void measure_piece(tb_bench *bench, stream_state *state, size_t piece, const char *reading,
                          const char *writing)
{
    tb_bench_case reads[] = {
        {.name = reading, .variant = "tb_stream_read, memory", .run = run_stream_read},
        {.name = reading, .variant = "memcpy", .run = run_memcpy},
    };
    tb_bench_case writes[] = {
        {.name = writing, .variant = "tb_stream_write_all, buffer", .run = run_buffer_write},
    };
    state->piece = piece;
    for (size_t index = 0; index < 2; index++)
    {
        reads[index].context = state;
        reads[index].operations = (double)(state->size / piece);
    }
    writes[0].context = state;
    writes[0].operations = (double)(state->size / piece);
    tb_bench_measure(bench, "stream", reads, 2);
    tb_bench_measure(bench, "stream", writes, 1);
}

void tb_bench_stream(tb_bench *bench)
{
    stream_state state = {0};
    tb_bench_case range = {.name = "read 64 bytes at a random offset of a range",
                           .variant = "tb_stream_read_at",
                           .run = run_range_read_at};
    if (!tb_bench_wants(bench, "stream"))
    {
        return;
    }
    state.size = tb_bench_size(bench, STREAM_SOURCE_SIZE);
    /* Whole pieces only, and at least one of the largest. */
    if (state.size < STREAM_LARGE_PIECE)
    {
        state.size = STREAM_LARGE_PIECE;
    }
    state.size -= state.size % STREAM_LARGE_PIECE;
    state.source = tb_malloc(state.size);
    state.output = tb_malloc(STREAM_LARGE_PIECE);
    for (size_t index = 0; index < state.size; index++)
    {
        state.source[index] = (unsigned char)(index * 131u);
    }
    measure_piece(bench, &state, STREAM_SMALL_PIECE, "read 1 MiB in pieces of 64 bytes",
                  "write 1 MiB in pieces of 64 bytes");
    measure_piece(bench, &state, STREAM_LARGE_PIECE, "read 1 MiB in pieces of 4 KiB",
                  "write 1 MiB in pieces of 4 KiB");
    state.piece = STREAM_SMALL_PIECE;
    range.context = &state;
    range.operations = (double)STREAM_RANGE_READS;
    tb_bench_measure(bench, "stream", &range, 1);
    tb_free(state.output);
    tb_free(state.source);
}
