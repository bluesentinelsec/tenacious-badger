/* Compression of 1 MiB at the fastest, the default, and the best level: text
 * from a small vocabulary, which compresses about six times, random bytes,
 * which do not compress, and zeros. Operations are input bytes, so the
 * numbers are nanoseconds per byte compressed. */
#include "tb_bench.h"

#include "badger/deflate.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEFLATE_BENCH_SIZE ((size_t)1 << 20)

typedef struct deflate_state
{
    const unsigned char *input;
    size_t size;
    unsigned char *output;
    size_t capacity;
    int level;
} deflate_state;

static void run_deflate(void *context)
{
    const deflate_state *const state = context;
    size_t written = 0;
    (void)tb_deflate(TB_DEFLATE_RAW, state->level, state->input, state->size, state->output,
                     state->capacity, &written);
    tb_bench_keep(written);
}

static void fill_words(unsigned char *output, size_t length)
{
    static const char *const words[] = {"the ",   "engine ", "badger ", "renders ", "a ",
                                        "frame ", "of ",     "light ",  "while ",   "jobs ",
                                        "steal ", "work. ",  "\n"};
    uint64_t random = 1;
    size_t done = 0;
    while (done < length)
    {
        const char *word = words[tb_bench_random(&random) % (sizeof(words) / sizeof(words[0]))];
        while (*word != '\0' && done < length)
        {
            output[done] = (unsigned char)*word;
            done++;
            word++;
        }
    }
}

void tb_bench_deflate(tb_bench *bench)
{
    static const int levels[3] = {1, TB_DEFLATE_DEFAULT_LEVEL, TB_DEFLATE_BEST_LEVEL};
    static const char *const variants[3] = {"tb_deflate, level 1", "tb_deflate, level 6",
                                            "tb_deflate, level 9"};
    static const char *const names[3] = {"compress 1 MiB of text", "compress 1 MiB of random bytes",
                                         "compress 1 MiB of zeros"};
    unsigned char *inputs[3] = {NULL};
    deflate_state states[3][3] = {{{0}}};
    tb_bench_case cases[3][3] = {{{0}}};
    size_t size = 0;
    size_t capacity = 0;
    unsigned char *output = NULL;
    uint64_t random = 7;
    if (!tb_bench_wants(bench, "deflate"))
    {
        return;
    }
    size = tb_bench_size(bench, DEFLATE_BENCH_SIZE);
    capacity = tb_deflate_bound(size);
    output = tb_malloc(capacity);
    for (size_t kind = 0; kind < 3; kind++)
    {
        inputs[kind] = tb_malloc(size);
    }
    fill_words(inputs[0], size);
    for (size_t index = 0; index < size; index++)
    {
        inputs[1][index] = (unsigned char)tb_bench_random(&random);
    }
    memset(inputs[2], 0, size);
    for (size_t kind = 0; kind < 3; kind++)
    {
        for (size_t level = 0; level < 3; level++)
        {
            states[kind][level] =
                (deflate_state){inputs[kind], size, output, capacity, levels[level]};
            cases[kind][level].name = names[kind];
            cases[kind][level].variant = variants[level];
            cases[kind][level].run = run_deflate;
            cases[kind][level].context = &states[kind][level];
            cases[kind][level].operations = (double)size;
        }
        tb_bench_measure(bench, "deflate", cases[kind], 3);
    }
    for (size_t kind = 0; kind < 3; kind++)
    {
        tb_free(inputs[kind]);
    }
    tb_free(output);
}
