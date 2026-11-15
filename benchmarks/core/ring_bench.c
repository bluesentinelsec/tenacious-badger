/* The ring: a queue at a steady length, elements added and removed in the
 * middle, and elements read by index. */
#include "tb_bench.h"

#include "badger/ring.h"

#include <stddef.h>
#include <stdint.h>

#define RING_QUEUE_LENGTH ((size_t)1000)
#define RING_QUEUE_STEPS ((size_t)1000000)
#define RING_WIDE_LENGTH ((size_t)4096)
#define RING_MIDDLE_STEPS ((size_t)20000)
#define RING_READ_ROUNDS ((size_t)250)

typedef struct ring_state
{
    size_t length; /* Elements the ring holds when a run starts. */
    size_t steps;  /* Steps the run takes. */
    tb_ring *ring;
} ring_state;

static void set_up(void *context)
{
    ring_state *const state = context;

    state->ring = TB_RING_CREATE(NULL, int);
    for (size_t index = 0; index < state->length; index++)
    {
        TB_RING_PUSH_BACK(state->ring, int, (int)index);
    }
}

static void tear_down(void *context)
{
    ring_state *const state = context;

    tb_ring_destroy(state->ring);
    state->ring = NULL;
}

static void run_queue(void *context)
{
    const ring_state *const state = context;
    uint64_t sum = 0;

    for (size_t step = 0; step < state->steps; step++)
    {
        int front = 0;

        TB_RING_PUSH_BACK(state->ring, int, (int)step);
        tb_ring_pop_front(state->ring, &front);
        sum += (uint64_t)front;
    }
    tb_bench_keep(sum);
}

/* Inserts and removes within the middle half of the ring, where either end is
 * at least a quarter of the ring away. */
static void run_middle(void *context)
{
    const ring_state *const state = context;
    const size_t quarter = state->length / 4;
    const size_t span = state->length / 2;
    const int value = 7;

    for (size_t step = 0; step < state->steps; step++)
    {
        (void)tb_ring_insert(state->ring, quarter + step % span, &value);
        tb_ring_remove(state->ring, quarter + (step * 7) % span, NULL);
    }
    tb_bench_keep(tb_ring_length(state->ring));
}

static void run_sum_by_index(void *context)
{
    const ring_state *const state = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < state->steps; round++)
    {
        for (size_t index = 0; index < tb_ring_length(state->ring); index++)
        {
            sum += (uint64_t)*TB_RING_AT(state->ring, int, index);
        }
    }
    tb_bench_keep(sum);
}

void tb_bench_ring(tb_bench *bench)
{
    ring_state queue = {.length = tb_bench_size(bench, RING_QUEUE_LENGTH),
                        .steps = tb_bench_size(bench, RING_QUEUE_STEPS)};
    ring_state middle = {.length = tb_bench_size(bench, RING_WIDE_LENGTH),
                         .steps = tb_bench_size(bench, RING_MIDDLE_STEPS)};
    ring_state reading = {.length = tb_bench_size(bench, RING_WIDE_LENGTH),
                          .steps = tb_bench_size(bench, RING_READ_ROUNDS)};
    const tb_bench_case cases[] = {
        {.name = "push at the back and pop at the front",
         .variant = "badger",
         .operations = (double)queue.steps,
         .setup = set_up,
         .run = run_queue,
         .teardown = tear_down,
         .context = &queue},
        {.name = "insert or remove in the middle of 4,096 ints",
         .variant = "badger",
         .operations = (double)middle.steps * 2,
         .setup = set_up,
         .run = run_middle,
         .teardown = tear_down,
         .context = &middle},
        {.name = "read an int by index",
         .variant = "badger",
         .operations = (double)reading.steps * (double)reading.length,
         .setup = set_up,
         .run = run_sum_by_index,
         .teardown = tear_down,
         .context = &reading},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        tb_bench_measure(bench, "ring", &cases[index], 1);
    }
}
