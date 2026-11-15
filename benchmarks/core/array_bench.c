/* The dynamic array: adding elements, reading them back, and the cost of an
 * array that lives only for a moment. */
#include "tb_bench.h"

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/assert.h"

#include <stddef.h>
#include <stdint.h>

#define ARRAY_ELEMENTS ((size_t)1000000)
#define ARRAY_READ_ROUNDS ((size_t)4)
#define ARRAY_SMALL_ARRAYS ((size_t)100000)
#define ARRAY_SMALL_LENGTH ((size_t)16)
#define ARRAY_ARENA_RESET_INTERVAL ((size_t)256)

/* An element of the size of a small component. */
typedef struct array_particle
{
    float position[3];
    float velocity[3];
    float color[4];
    unsigned flags;
    unsigned owner;
    double born;
    double expires;
} array_particle;

TB_STATIC_ASSERT(sizeof(array_particle) == 64, "the wide element is meant to be 64 bytes");

typedef struct array_state
{
    size_t elements;
    size_t small_arrays;
    tb_array *array;
} array_state;

static void run_push_integers(void *context)
{
    const array_state *const state = context;
    tb_array *const array = TB_ARRAY_CREATE(NULL, int);

    for (size_t index = 0; index < state->elements; index++)
    {
        TB_ARRAY_PUSH(array, int, (int)index);
    }
    tb_bench_keep(tb_array_length(array));
    tb_array_destroy(array);
}

static void run_push_particles(void *context)
{
    const array_state *const state = context;
    tb_array *const array = TB_ARRAY_CREATE(NULL, array_particle);
    array_particle particle = {0};

    for (size_t index = 0; index < state->elements; index++)
    {
        particle.owner = (unsigned)index;
        (void)tb_array_push(array, &particle);
    }
    tb_bench_keep(tb_array_length(array));
    tb_array_destroy(array);
}

static void set_up_filled(void *context)
{
    array_state *const state = context;

    state->array = TB_ARRAY_CREATE(NULL, int);
    for (size_t index = 0; index < state->elements; index++)
    {
        TB_ARRAY_PUSH(state->array, int, (int)index);
    }
}

static void tear_down(void *context)
{
    array_state *const state = context;

    tb_array_destroy(state->array);
    state->array = NULL;
}

/* An array that keeps its storage from one use to the next, as an array
 * refilled every frame does: no growing is timed. */
static void run_refill(void *context)
{
    const array_state *const state = context;

    tb_array_clear(state->array);
    for (size_t index = 0; index < state->elements; index++)
    {
        TB_ARRAY_PUSH(state->array, int, (int)index);
    }
    tb_bench_keep(tb_array_length(state->array));
}

static void run_sum_by_index(void *context)
{
    const array_state *const state = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < ARRAY_READ_ROUNDS; round++)
    {
        for (size_t index = 0; index < tb_array_length(state->array); index++)
        {
            sum += (uint64_t)*TB_ARRAY_AT(state->array, int, index);
        }
    }
    tb_bench_keep(sum);
}

static void run_sum_by_pointer(void *context)
{
    const array_state *const state = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < ARRAY_READ_ROUNDS; round++)
    {
        const int *const elements = TB_ARRAY_DATA(state->array, int);
        const size_t length = tb_array_length(state->array);

        for (size_t index = 0; index < length; index++)
        {
            sum += (uint64_t)elements[index];
        }
    }
    tb_bench_keep(sum);
}

static void fill_small(tb_array *array)
{
    for (size_t index = 0; index < ARRAY_SMALL_LENGTH; index++)
    {
        TB_ARRAY_PUSH(array, int, (int)index);
    }
    tb_bench_keep(tb_array_length(array));
}

static void run_small_on_the_heap(void *context)
{
    const array_state *const state = context;

    for (size_t count = 0; count < state->small_arrays; count++)
    {
        tb_array *const array = TB_ARRAY_CREATE(NULL, int);

        fill_small(array);
        tb_array_destroy(array);
    }
}

static void run_small_in_an_arena(void *context)
{
    const array_state *const state = context;
    tb_arena *const arena = tb_arena_create(0);
    const tb_allocator allocator = tb_arena_allocator(arena);

    for (size_t count = 0; count < state->small_arrays; count++)
    {
        fill_small(TB_ARRAY_CREATE(&allocator, int));
        if (count % ARRAY_ARENA_RESET_INTERVAL == ARRAY_ARENA_RESET_INTERVAL - 1)
        {
            tb_arena_reset(arena);
        }
    }
    tb_arena_destroy(arena);
}

void tb_bench_array(tb_bench *bench)
{
    array_state state = {.elements = tb_bench_size(bench, ARRAY_ELEMENTS),
                         .small_arrays = tb_bench_size(bench, ARRAY_SMALL_ARRAYS)};
    const double elements = (double)state.elements;
    const double reads = elements * (double)ARRAY_READ_ROUNDS;
    const double small = (double)state.small_arrays;
    const tb_bench_case pushes[] = {
        {.name = "push an int, growing from empty",
         .variant = "badger",
         .operations = elements,
         .run = run_push_integers,
         .context = &state},
        {.name = "push an int into storage that is kept",
         .variant = "badger",
         .operations = elements,
         .setup = set_up_filled,
         .run = run_refill,
         .teardown = tear_down,
         .context = &state},
        {.name = "push a 64-byte element, growing from empty",
         .variant = "badger",
         .operations = elements,
         .run = run_push_particles,
         .context = &state},
    };
    const tb_bench_case reading[] = {
        {.name = "read an int",
         .variant = "TB_ARRAY_AT for each",
         .operations = reads,
         .setup = set_up_filled,
         .run = run_sum_by_index,
         .teardown = tear_down,
         .context = &state},
        {.name = "read an int",
         .variant = "TB_ARRAY_DATA once",
         .operations = reads,
         .setup = set_up_filled,
         .run = run_sum_by_pointer,
         .teardown = tear_down,
         .context = &state},
    };
    const tb_bench_case short_lived[] = {
        {.name = "create an array, push 16 ints, release it",
         .variant = "on the heap",
         .operations = small,
         .run = run_small_on_the_heap,
         .context = &state},
        {.name = "create an array, push 16 ints, release it",
         .variant = "in an arena",
         .operations = small,
         .run = run_small_in_an_arena,
         .context = &state},
    };

    for (size_t index = 0; index < sizeof(pushes) / sizeof(pushes[0]); index++)
    {
        tb_bench_measure(bench, "array", &pushes[index], 1);
    }
    tb_bench_measure(bench, "array", reading, sizeof(reading) / sizeof(reading[0]));
    tb_bench_measure(bench, "array", short_lived, sizeof(short_lived) / sizeof(short_lived[0]));
}
