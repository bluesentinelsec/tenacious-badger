/* Conversions between double and text: the shortest text, reading it back,
 * and the formatter's %.17g and %f, on values drawn from every exponent and
 * on values of the size a game's data holds. */
#include "tb_bench.h"

#include "badger/decimal.h"
#include "badger/fmt.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DECIMAL_VALUE_COUNT ((size_t)4096)

typedef struct decimal_state
{
    size_t count;
    double any[DECIMAL_VALUE_COUNT];     /* Bits drawn evenly: every exponent. */
    double typical[DECIMAL_VALUE_COUNT]; /* Between 0.001 and 100000. */
    char texts[DECIMAL_VALUE_COUNT][TB_DECIMAL_SHORTEST_SIZE];
    size_t lengths[DECIMAL_VALUE_COUNT];
} decimal_state;

static uint64_t write_shortest(const double *values, size_t count)
{
    uint64_t sum = 0;

    for (size_t index = 0; index < count; index++)
    {
        char text[TB_DECIMAL_SHORTEST_SIZE] = {0};
        size_t required = 0;

        (void)tb_decimal_shortest(values[index], text, sizeof(text), &required);
        sum += required;
    }
    return sum;
}

static void run_shortest_any(void *context)
{
    const decimal_state *const state = context;

    tb_bench_keep(write_shortest(state->any, state->count));
}

static void run_shortest_typical(void *context)
{
    const decimal_state *const state = context;

    tb_bench_keep(write_shortest(state->typical, state->count));
}

static void run_parse(void *context)
{
    const decimal_state *const state = context;
    uint64_t sum = 0;

    for (size_t index = 0; index < state->count; index++)
    {
        double value = 0.0;
        uint64_t bits = 0;

        (void)tb_decimal_parse(state->texts[index], state->lengths[index], &value);
        memcpy(&bits, &value, sizeof(bits));
        sum += bits;
    }
    tb_bench_keep(sum);
}

static uint64_t format_all(const double *values, size_t count, const char *format)
{
    uint64_t sum = 0;

    for (size_t index = 0; index < count; index++)
    {
        char text[512] = {0};

        sum += tb_fmt_buffer(text, sizeof(text), format, values[index]);
    }
    return sum;
}

static void run_seventeen_digits(void *context)
{
    const decimal_state *const state = context;

    tb_bench_keep(format_all(state->any, state->count, "%.17g"));
}

static void run_fixed(void *context)
{
    const decimal_state *const state = context;

    tb_bench_keep(format_all(state->typical, state->count, "%.3f"));
}

void tb_bench_decimal(tb_bench *bench)
{
    static decimal_state state = {0};
    uint64_t sequence = 13;
    tb_bench_case cases[5] = {
        {.name = "shortest text, any exponent", .run = run_shortest_any},
        {.name = "shortest text, 0.001 to 100000", .run = run_shortest_typical},
        {.name = "read shortest text, any exponent", .run = run_parse},
        {.name = "%.17g, any exponent", .run = run_seventeen_digits},
        {.name = "%.3f, 0.001 to 100000", .run = run_fixed},
    };

    if (!tb_bench_wants(bench, "decimal"))
    {
        return;
    }
    state.count = tb_bench_size(bench, DECIMAL_VALUE_COUNT);
    for (size_t index = 0; index < state.count; index++)
    {
        uint64_t bits = 0;

        /* Finite values only: an exponent field of all ones is redrawn. */
        do
        {
            bits = tb_bench_random(&sequence);
        } while ((bits & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000));
        memcpy(&state.any[index], &bits, sizeof(bits));
        state.typical[index] = 0.001 + (double)(tb_bench_random(&sequence) % 100000000u) / 1000.0;
        (void)tb_decimal_shortest(state.any[index], state.texts[index], sizeof(state.texts[index]),
                                  &state.lengths[index]);
        state.lengths[index]--;
    }
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        cases[index].variant = "badger";
        cases[index].operations = (double)state.count;
        cases[index].context = &state;
        tb_bench_measure(bench, "decimal", &cases[index], 1);
    }
}
