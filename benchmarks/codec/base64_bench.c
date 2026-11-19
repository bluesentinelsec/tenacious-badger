/* Base64 of 1 MiB of random bytes, each way. Operations are input bytes for
 * encoding and output bytes for decoding: the same megabyte either way. */
#include "tb_bench.h"

#include "badger/base64.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>

#define BASE64_BENCH_SIZE ((size_t)1 << 20)

typedef struct base64_state
{
    unsigned char *bytes;
    size_t size;
    char *text;
    size_t text_length;
    unsigned char *decoded;
} base64_state;

static void run_encode(void *context)
{
    base64_state *const state = context;
    size_t written = 0;
    (void)tb_base64_encode(state->bytes, state->size, 0, state->text, state->text_length, &written);
    tb_bench_keep(written);
}

static void run_decode(void *context)
{
    base64_state *const state = context;
    size_t written = 0;
    (void)tb_base64_decode(state->text, state->text_length, 0, state->decoded, state->size,
                           &written, NULL);
    tb_bench_keep(written);
}

void tb_bench_base64(tb_bench *bench)
{
    base64_state state = {0};
    uint64_t random = 3;
    size_t written = 0;
    tb_bench_case cases[2] = {
        {.name = "encode 1 MiB", .variant = "tb_base64_encode", .run = run_encode},
        {.name = "decode 1 MiB", .variant = "tb_base64_decode", .run = run_decode},
    };
    if (!tb_bench_wants(bench, "base64"))
    {
        return;
    }
    state.size = tb_bench_size(bench, BASE64_BENCH_SIZE);
    state.bytes = tb_malloc(state.size);
    state.decoded = tb_malloc(state.size);
    for (size_t index = 0; index < state.size; index++)
    {
        state.bytes[index] = (unsigned char)tb_bench_random(&random);
    }
    (void)tb_base64_encoded_size(state.size, 0, &state.text_length);
    state.text = tb_malloc(state.text_length);
    (void)tb_base64_encode(state.bytes, state.size, 0, state.text, state.text_length, &written);
    for (size_t index = 0; index < 2; index++)
    {
        cases[index].context = &state;
        cases[index].operations = (double)state.size;
    }
    tb_bench_measure(bench, "base64", cases, 2);
    tb_free(state.text);
    tb_free(state.decoded);
    tb_free(state.bytes);
}
