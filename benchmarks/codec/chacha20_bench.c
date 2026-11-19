/* ChaCha20 over 1 MiB, in one call and through a stream over memory: what
 * obfuscation adds to reading a package. */
#include "tb_bench.h"

#include "badger/chacha20.h"
#include "badger/memory.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

#define CHACHA20_SIZE ((size_t)1 << 20)

typedef struct chacha20_bench_state
{
    unsigned char *input;
    unsigned char *output;
    size_t size;
} chacha20_bench_state;

static const uint8_t bench_key[TB_CHACHA20_KEY_SIZE] = {7};
static const uint8_t bench_nonce[TB_CHACHA20_NONCE_SIZE] = {9};

static void run_xor(void *context)
{
    const chacha20_bench_state *const state = context;
    tb_chacha20_xor(bench_key, bench_nonce, 0, state->input, state->output, state->size);
    tb_bench_keep(state->output[0]);
}

static void run_stream(void *context)
{
    const chacha20_bench_state *const state = context;
    tb_stream source = {0};
    tb_stream stream = {0};
    tb_stream_open_memory(state->input, state->size, NULL, &source);
    (void)tb_chacha20_open_stream(&source, bench_key, bench_nonce, NULL, &stream);
    (void)tb_stream_read_exact(&stream, state->output, state->size);
    (void)tb_stream_close(&stream);
    (void)tb_stream_close(&source);
    tb_bench_keep(state->output[0]);
}

void tb_bench_chacha20(tb_bench *bench)
{
    chacha20_bench_state state = {0};
    uint64_t random = 1;
    tb_bench_case cases[2] = {
        {.name = "ChaCha20 over 1 MiB", .variant = "tb_chacha20_xor", .run = run_xor},
        {.name = "ChaCha20 over 1 MiB", .variant = "a stream over memory", .run = run_stream},
    };
    if (!tb_bench_wants(bench, "chacha20"))
    {
        return;
    }
    state.size = tb_bench_size(bench, CHACHA20_SIZE);
    state.input = tb_malloc(state.size);
    state.output = tb_malloc(state.size);
    for (size_t index = 0; index < state.size; index++)
    {
        state.input[index] = (unsigned char)tb_bench_random(&random);
    }
    /* Operations are bytes, so the numbers are nanoseconds per byte. */
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)state.size;
    }
    tb_bench_measure(bench, "chacha20", cases, 2);
    tb_free(state.output);
    tb_free(state.input);
}
