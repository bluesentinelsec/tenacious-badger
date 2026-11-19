/* Decompression of streams made by zlib (tests/codec/deflate_vectors.h):
 * text with short matches, long runs of one byte, a stored block, and text
 * coded with literals alone. Operations are output bytes, so the numbers are
 * nanoseconds per decompressed byte. */
#include "tb_bench.h"

#include "badger/deflate.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "deflate_vectors.h"

#include <stddef.h>
#include <stdint.h>

typedef struct inflate_state
{
    const deflate_vector *vector;
    unsigned char *output;
    size_t capacity;
} inflate_state;

static void run_inflate(void *context)
{
    const inflate_state *const state = context;
    size_t written = 0;
    (void)tb_inflate(state->vector->format, state->vector->data, state->vector->size, state->output,
                     state->capacity, &written);
    tb_bench_keep(written);
}

void tb_bench_inflate(tb_bench *bench)
{
    static const char *const names[] = {"zlib_text_level6", "zlib_zeros_level9",
                                        "zlib_random_stored", "raw_text_huffman_only",
                                        "raw_far_match_level9"};
    inflate_state states[5] = {0};
    tb_bench_case cases[5] = {0};
    if (!tb_bench_wants(bench, "inflate"))
    {
        return;
    }
    for (size_t index = 0; index < 5; index++)
    {
        for (size_t row = 0; row < sizeof(deflate_vectors) / sizeof(deflate_vectors[0]); row++)
        {
            if (tb_str_equal(deflate_vectors[row].name, names[index]))
            {
                states[index].vector = &deflate_vectors[row];
            }
        }
        states[index].capacity = states[index].vector->length;
        states[index].output = tb_malloc(states[index].capacity);
        cases[index].name = names[index];
        cases[index].variant = "tb_inflate";
        cases[index].run = run_inflate;
        cases[index].context = &states[index];
        cases[index].operations = (double)states[index].capacity;
    }
    tb_bench_measure(bench, "inflate", cases, 5);
    for (size_t index = 0; index < 5; index++)
    {
        tb_free(states[index].output);
    }
}
