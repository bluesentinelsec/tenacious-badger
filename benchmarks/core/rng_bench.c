/* The generator: raw 64-bit outputs, bounded integers on the 32-bit and the
 * 64-bit path, doubles, floats in a range, and a jump. Times are per number. */
#include "tb_bench.h"

#include "badger/rng.h"

#include <stdint.h>
#include <string.h>

#define RNG_DRAWS ((size_t)10000000)
#define RNG_JUMPS ((size_t)10000)

typedef struct rng_state
{
    tb_rng rng;
    size_t draws;
} rng_state;

static void run_next(void *context)
{
    rng_state *const state = context;
    uint64_t sum = 0;

    for (size_t draw = 0; draw < state->draws; draw++)
    {
        sum += tb_rng_next(&state->rng);
    }
    tb_bench_keep(sum);
}

static void run_below_small(void *context)
{
    rng_state *const state = context;
    uint64_t sum = 0;

    for (size_t draw = 0; draw < state->draws; draw++)
    {
        sum += tb_rng_below(&state->rng, 1000);
    }
    tb_bench_keep(sum);
}

static void run_below_large(void *context)
{
    rng_state *const state = context;
    uint64_t sum = 0;

    for (size_t draw = 0; draw < state->draws; draw++)
    {
        sum += tb_rng_below(&state->rng, (UINT64_C(1) << 40) + 7);
    }
    tb_bench_keep(sum);
}

static void run_double(void *context)
{
    rng_state *const state = context;
    double sum = 0;
    uint64_t bits = 0;

    for (size_t draw = 0; draw < state->draws; draw++)
    {
        sum += tb_rng_double(&state->rng);
    }
    memcpy(&bits, &sum, sizeof(bits));
    tb_bench_keep(bits);
}

static void run_float_between(void *context)
{
    rng_state *const state = context;
    float sum = 0;
    uint32_t bits = 0;

    for (size_t draw = 0; draw < state->draws; draw++)
    {
        sum += tb_rng_float_between(&state->rng, -1.0f, 1.0f);
    }
    memcpy(&bits, &sum, sizeof(bits));
    tb_bench_keep(bits);
}

static void run_jump(void *context)
{
    rng_state *const state = context;

    for (size_t jump = 0; jump < state->draws; jump++)
    {
        tb_rng_jump(&state->rng);
    }
    tb_bench_keep(state->rng.state[0]);
}

void tb_bench_rng(tb_bench *bench)
{
    rng_state draws = {.draws = tb_bench_size(bench, RNG_DRAWS)};
    rng_state jumps = {.draws = tb_bench_size(bench, RNG_JUMPS)};
    tb_bench_case cases[5] = {
        {.name = "64 random bits", .variant = "badger", .run = run_next},
        {.name = "an integer below 1000", .variant = "badger", .run = run_below_small},
        {.name = "an integer below 2^40 + 7", .variant = "badger", .run = run_below_large},
        {.name = "a double in [0, 1)", .variant = "badger", .run = run_double},
        {.name = "a float in [-1, 1]", .variant = "badger", .run = run_float_between},
    };
    tb_bench_case jump = {.name = "jump 2^128 numbers ahead", .variant = "badger", .run = run_jump};

    tb_rng_seed(&draws.rng, 1);
    tb_rng_seed(&jumps.rng, 1);
    for (size_t index = 0; index < 5; index++)
    {
        cases[index].operations = (double)draws.draws;
        cases[index].context = &draws;
    }
    jump.operations = (double)jumps.draws;
    jump.context = &jumps;
    tb_bench_measure(bench, "rng", cases, 5);
    tb_bench_measure(bench, "rng", &jump, 1);
}
