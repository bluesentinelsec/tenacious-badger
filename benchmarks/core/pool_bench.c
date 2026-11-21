/* Pools: allocating and freeing objects of one size, in a steady state where
 * a fixed number are live and each step frees one at random and allocates a
 * replacement, as entities or particles come and go. The pool is measured
 * beside the engine's heap and the C library's, which serve the same pattern
 * with general-purpose allocators. Times are per allocation and free. */
#include "tb_bench.h"

#include "badger/memory.h"
#include "badger/pool.h"

#include <stdint.h>
#include <stdlib.h>

#define POOL_OBJECT_SIZE ((size_t)48)
#define POOL_LIVE ((size_t)4096)
#define POOL_STEPS ((size_t)2000000)

typedef struct pool_state
{
    tb_pool *pool;
    void **live;
    const uint32_t *victims; /* Which live object each step replaces. */
    size_t steps;
} pool_state;

static void run_pool(void *context)
{
    pool_state *const state = context;
    uint64_t sum = 0;

    for (size_t step = 0; step < state->steps; step++)
    {
        void **const slot = &state->live[state->victims[step % POOL_LIVE]];

        tb_pool_free(state->pool, *slot);
        *slot = tb_pool_alloc(state->pool);
        sum += (uintptr_t)*slot & 0xFFu;
    }
    tb_bench_keep(sum);
}

static void run_heap(void *context)
{
    pool_state *const state = context;
    uint64_t sum = 0;

    for (size_t step = 0; step < state->steps; step++)
    {
        void **const slot = &state->live[state->victims[step % POOL_LIVE]];

        tb_free(*slot);
        *slot = tb_malloc(POOL_OBJECT_SIZE);
        sum += (uintptr_t)*slot & 0xFFu;
    }
    tb_bench_keep(sum);
}

static void run_c_library(void *context)
{
    pool_state *const state = context;
    uint64_t sum = 0;

    for (size_t step = 0; step < state->steps; step++)
    {
        void **const slot = &state->live[state->victims[step % POOL_LIVE]];

        free(*slot);
        *slot = malloc(POOL_OBJECT_SIZE);
        sum += (uintptr_t)*slot & 0xFFu;
    }
    tb_bench_keep(sum);
}

void tb_bench_pool(tb_bench *bench)
{
    const tb_pool_options options = {.object_size = POOL_OBJECT_SIZE, .alignment = 8};
    pool_state pool = {.steps = tb_bench_size(bench, POOL_STEPS)};
    pool_state heap = pool;
    pool_state c_library = pool;
    uint32_t *victims = NULL;
    uint64_t random_state = 23;
    const char *const name = "replace one of 4096 live 48-byte objects";
    tb_bench_case cases[3] = {
        {.name = name, .variant = "tb_pool", .run = run_pool, .context = &pool},
        {.name = name, .variant = "tb_malloc", .run = run_heap, .context = &heap},
        {.name = name, .variant = "malloc", .run = run_c_library, .context = &c_library},
    };

    if (!tb_bench_wants(bench, "pool"))
    {
        return;
    }
    victims = tb_malloc(POOL_LIVE * sizeof(uint32_t));
    pool.pool = tb_pool_create(&options);
    pool.live = tb_malloc(POOL_LIVE * sizeof(void *));
    heap.live = tb_malloc(POOL_LIVE * sizeof(void *));
    c_library.live = tb_malloc(POOL_LIVE * sizeof(void *));
    for (size_t index = 0; index < POOL_LIVE; index++)
    {
        victims[index] = (uint32_t)(tb_bench_random(&random_state) % POOL_LIVE);
        pool.live[index] = tb_pool_alloc(pool.pool);
        heap.live[index] = tb_malloc(POOL_OBJECT_SIZE);
        c_library.live[index] = malloc(POOL_OBJECT_SIZE);
    }
    pool.victims = victims;
    heap.victims = victims;
    c_library.victims = victims;
    for (size_t index = 0; index < 3; index++)
    {
        cases[index].operations = (double)pool.steps;
    }
    tb_bench_measure(bench, "pool", cases, 3);
    for (size_t index = 0; index < POOL_LIVE; index++)
    {
        free(c_library.live[index]);
        tb_free(heap.live[index]);
    }
    tb_free(c_library.live);
    tb_free(heap.live);
    tb_free(pool.live);
    tb_pool_destroy(pool.pool);
    tb_free(victims);
}
