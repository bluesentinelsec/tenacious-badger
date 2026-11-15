/* The bit vector: counting, visiting the set bits of a sparse and of a dense
 * vector, single bits at scattered places, and combining two vectors. */
#include "tb_bench.h"

#include "badger/bit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIT_LENGTH ((size_t)1 << 20)
#define BIT_WORD_WIDTH ((size_t)64)
#define BIT_COUNT_ROUNDS ((size_t)100)
#define BIT_SPARSE_ROUNDS ((size_t)100)
#define BIT_DENSE_ROUNDS ((size_t)4)
#define BIT_COMBINE_ROUNDS ((size_t)200)
#define BIT_SINGLE_READS ((size_t)2000000)

typedef struct bit_state
{
    size_t length;
    tb_bit *sparse; /* About one bit in a hundred is set. */
    tb_bit *dense;  /* About half the bits are set. */
    size_t reads;
} bit_state;

static void run_count(void *context)
{
    const bit_state *const state = context;

    for (size_t round = 0; round < BIT_COUNT_ROUNDS; round++)
    {
        tb_bench_keep(tb_bit_count(state->dense));
    }
}

/* Visits every set bit by asking for the next one after the last. */
static uint64_t walk_set_bits(const tb_bit *bits, size_t rounds)
{
    uint64_t sum = 0;

    for (size_t round = 0; round < rounds; round++)
    {
        size_t index = tb_bit_find_set(bits, 0);

        while (index != TB_BIT_NOT_FOUND)
        {
            sum += index;
            index = tb_bit_find_set(bits, index + 1);
        }
    }
    return sum;
}

static void run_walk_sparse(void *context)
{
    const bit_state *const state = context;

    tb_bench_keep(walk_set_bits(state->sparse, BIT_SPARSE_ROUNDS));
}

static void run_walk_dense(void *context)
{
    const bit_state *const state = context;

    tb_bench_keep(walk_set_bits(state->dense, BIT_DENSE_ROUNDS));
}

static void run_single_bits(void *context)
{
    const bit_state *const state = context;
    uint64_t sequence = 11;
    uint64_t set = 0;

    for (size_t read = 0; read < state->reads; read++)
    {
        if (tb_bit_get(state->dense, (size_t)(tb_bench_random(&sequence) % state->length)))
        {
            set++;
        }
    }
    tb_bench_keep(set);
}

static void run_union(void *context)
{
    const bit_state *const state = context;

    for (size_t round = 0; round < BIT_COMBINE_ROUNDS; round++)
    {
        tb_bit_union(state->dense, state->sparse);
    }
    tb_bench_keep(tb_bit_get(state->dense, 5) ? 1u : 0u);
}

void tb_bench_bit(tb_bench *bench)
{
    bit_state state = {0};
    uint64_t sequence = 3;
    double words = 0;
    tb_bench_case cases[5] = {
        {.name = "count the set bits (per 64 bits)", .run = run_count},
        {.name = "visit the set bits, 1 in 100 set (per bit visited)", .run = run_walk_sparse},
        {.name = "visit the set bits, 1 in 2 set (per bit visited)", .run = run_walk_dense},
        {.name = "read one bit at a scattered index", .run = run_single_bits},
        {.name = "union of two vectors (per 64 bits)", .run = run_union},
    };

    if (!tb_bench_wants(bench, "bit"))
    {
        return;
    }
    state.length = tb_bench_size(bench, BIT_LENGTH);
    state.reads = tb_bench_size(bench, BIT_SINGLE_READS);
    state.sparse = tb_bit_create(NULL, state.length);
    state.dense = tb_bit_create(NULL, state.length);
    for (size_t index = 0; index < state.length; index++)
    {
        const uint64_t roll = tb_bench_random(&sequence);

        if (roll % 100 == 0)
        {
            (void)tb_bit_put(state.sparse, index, true);
        }
        if ((roll >> 32) % 2 == 0)
        {
            (void)tb_bit_put(state.dense, index, true);
        }
    }
    /* Each vector has a set bit to visit however short it is. */
    (void)tb_bit_put(state.sparse, 0, true);
    (void)tb_bit_put(state.dense, 0, true);
    words = (double)state.length / (double)BIT_WORD_WIDTH;
    cases[0].operations = words * (double)BIT_COUNT_ROUNDS;
    cases[1].operations = (double)tb_bit_count(state.sparse) * (double)BIT_SPARSE_ROUNDS;
    cases[2].operations = (double)tb_bit_count(state.dense) * (double)BIT_DENSE_ROUNDS;
    cases[3].operations = (double)state.reads;
    cases[4].operations = words * (double)BIT_COMBINE_ROUNDS;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        cases[index].variant = "badger";
        cases[index].context = &state;
        tb_bench_measure(bench, "bit", &cases[index], 1);
    }
    tb_bit_destroy(state.sparse);
    tb_bit_destroy(state.dense);
}
