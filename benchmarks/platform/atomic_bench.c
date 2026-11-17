/* What an atomic operation costs, alone and with eight threads working on
 * the same value: an addition, a change made by compare-and-exchange, and
 * the exchange of a pointer. */
#include "tb_bench.h"

#include "badger/platform.h"

#include <stddef.h>
#include <stdint.h>

/* Threads that work on one value at the same time. */
#define ATOMIC_CONTENDERS ((size_t)8)

#define ATOMIC_OPERATIONS_ALONE ((size_t)4000000)
#define ATOMIC_OPERATIONS_CONTENDED ((size_t)500000)

typedef struct atomic_state
{
    tb_platform_atomic_size size;
    tb_platform_atomic_u32 word;
    tb_platform_atomic_pointer pointer;
    size_t operations; /* What each thread performs. */
} atomic_state;

static void run_add(void *context)
{
    atomic_state *const state = context;
    uint64_t last = 0;

    for (size_t round = 0; round < state->operations; round++)
    {
        last = tb_platform_atomic_size_add(&state->size, 1);
    }
    tb_bench_keep(last);
}

static void run_load(void *context)
{
    atomic_state *const state = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < state->operations; round++)
    {
        sum += tb_platform_atomic_size_load(&state->size);
    }
    tb_bench_keep(sum);
}

/* How a value is changed that was computed from an earlier reading: try, and
 * compute again when another thread got there first. */
static void run_compare_exchange(void *context)
{
    atomic_state *const state = context;
    uint32_t seen = tb_platform_atomic_u32_load(&state->word);

    for (size_t round = 0; round < state->operations; round++)
    {
        while (!tb_platform_atomic_u32_compare_exchange(&state->word, &seen, seen + 1))
        {
            /* `seen` now holds what the other thread wrote. */
        }
    }
    tb_bench_keep(seen);
}

static void run_exchange(void *context)
{
    atomic_state *const state = context;
    void *held = state;

    for (size_t round = 0; round < state->operations; round++)
    {
        held = tb_platform_atomic_pointer_exchange(&state->pointer, held);
    }
    tb_bench_keep((uintptr_t)held & 0xFFu);
}

void tb_bench_atomic(tb_bench *bench)
{
    atomic_state alone = {0};
    atomic_state contended = {0};
    tb_bench_case additions[2] = {
        {.name = "add to an atomic size", .variant = "badger", .run = run_add},
        {.name = "add to an atomic size on 8 threads at once",
         .variant = "badger",
         .run = run_add,
         .threads = ATOMIC_CONTENDERS},
    };
    tb_bench_case changes[2] = {
        {.name = "change a 32-bit value by compare-and-exchange",
         .variant = "badger",
         .run = run_compare_exchange},
        {.name = "change a 32-bit value by compare-and-exchange on 8 threads at once",
         .variant = "badger",
         .run = run_compare_exchange,
         .threads = ATOMIC_CONTENDERS},
    };
    tb_bench_case others[2] = {
        {.name = "load an atomic size", .variant = "badger", .run = run_load},
        {.name = "exchange an atomic pointer", .variant = "badger", .run = run_exchange},
    };

    alone.operations = tb_bench_size(bench, ATOMIC_OPERATIONS_ALONE);
    contended.operations = tb_bench_size(bench, ATOMIC_OPERATIONS_CONTENDED);
    additions[0].operations = (double)alone.operations;
    additions[0].context = &alone;
    additions[1].operations = (double)contended.operations;
    additions[1].context = &contended;
    changes[0].operations = (double)alone.operations;
    changes[0].context = &alone;
    changes[1].operations = (double)contended.operations;
    changes[1].context = &contended;
    for (size_t index = 0; index < 2; index++)
    {
        others[index].operations = (double)alone.operations;
        others[index].context = &alone;
    }
    tb_bench_measure(bench, "atomic", additions, 2);
    tb_bench_measure(bench, "atomic", changes, 2);
    tb_bench_measure(bench, "atomic", others, 2);
}
