/* Sorting: tb_sort and tb_sort_stable beside the C library's qsort, on
 * 32-bit integers in random order, on records of 24 bytes in random order,
 * and on integers already in order. Each run copies the same unsorted array
 * before sorting it, in every variant alike, so the copy does not tilt the
 * comparison. Times are per element. */
#include "tb_bench.h"

#include "badger/memory.h"
#include "badger/sort.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SORT_COUNT ((size_t)100000)

typedef struct sort_record
{
    uint64_t key;
    uint64_t payload[2];
} sort_record;

typedef struct sort_state
{
    const void *source; /* The array every run starts from. */
    void *work;         /* Where each run sorts its copy. */
    size_t count;
    size_t size;
    int (*compare)(const void *left, const void *right); /* For qsort. */
    tb_sort_compare_function *compare_with_context;      /* The same, for the engine. */
} sort_state;

static int compare_u32(const void *left, const void *right)
{
    uint32_t first = 0;
    uint32_t second = 0;

    memcpy(&first, left, sizeof(first));
    memcpy(&second, right, sizeof(second));
    return (first > second) - (first < second);
}

static int compare_records(const void *left, const void *right)
{
    const sort_record *const first = left;
    const sort_record *const second = right;

    return (first->key > second->key) - (first->key < second->key);
}

/* The same comparisons in the engine's form, so that each sort makes one call
 * per comparison, as qsort does. */
static int compare_u32_with_context(const void *left, const void *right, void *context)
{
    (void)context;
    return compare_u32(left, right);
}

static int compare_records_with_context(const void *left, const void *right, void *context)
{
    (void)context;
    return compare_records(left, right);
}

static uint64_t first_and_last(const sort_state *state)
{
    const unsigned char *const bytes = state->work;
    uint64_t sum = 0;

    sum += bytes[0];
    sum += bytes[(state->count - 1) * state->size];
    return sum;
}

static void run_sort(void *context)
{
    sort_state *const state = context;

    memcpy(state->work, state->source, state->count * state->size);
    tb_sort(state->work, state->count, state->size, state->compare_with_context, NULL);
    tb_bench_keep(first_and_last(state));
}

static void run_stable(void *context)
{
    sort_state *const state = context;

    memcpy(state->work, state->source, state->count * state->size);
    tb_sort_stable(state->work, state->count, state->size, state->compare_with_context, NULL, NULL);
    tb_bench_keep(first_and_last(state));
}

static void run_qsort(void *context)
{
    sort_state *const state = context;

    memcpy(state->work, state->source, state->count * state->size);
    qsort(state->work, state->count, state->size, state->compare);
    tb_bench_keep(first_and_last(state));
}

/* Measures the three variants on one array. */
static void measure(tb_bench *bench, const char *name, sort_state *state)
{
    tb_bench_case cases[3] = {
        {.name = name, .variant = "tb_sort", .run = run_sort},
        {.name = name, .variant = "tb_sort_stable", .run = run_stable},
        {.name = name, .variant = "qsort", .run = run_qsort},
    };

    for (size_t index = 0; index < 3; index++)
    {
        cases[index].operations = (double)state->count;
        cases[index].context = state;
    }
    tb_bench_measure(bench, "sort", cases, 3);
}

void tb_bench_sort(tb_bench *bench)
{
    const size_t count = tb_bench_size(bench, SORT_COUNT);
    uint32_t *integers = NULL;
    uint32_t *in_order = NULL;
    sort_record *records = NULL;
    uint64_t random_state = 17;
    sort_state state = {.count = count};

    if (!tb_bench_wants(bench, "sort"))
    {
        return;
    }
    integers = tb_malloc(count * sizeof(uint32_t));
    in_order = tb_malloc(count * sizeof(uint32_t));
    records = tb_malloc(count * sizeof(sort_record));
    state.work = tb_malloc(count * sizeof(sort_record));
    for (size_t index = 0; index < count; index++)
    {
        integers[index] = (uint32_t)tb_bench_random(&random_state);
        in_order[index] = (uint32_t)index;
        records[index] = (sort_record){.key = tb_bench_random(&random_state),
                                       .payload = {index, ~(uint64_t)index}};
    }
    state.size = sizeof(uint32_t);
    state.compare = compare_u32;
    state.compare_with_context = compare_u32_with_context;
    state.source = integers;
    measure(bench, "sort 32-bit integers in random order", &state);
    state.source = in_order;
    measure(bench, "sort 32-bit integers already in order", &state);
    state.size = sizeof(sort_record);
    state.compare = compare_records;
    state.compare_with_context = compare_records_with_context;
    state.source = records;
    measure(bench, "sort 24-byte records in random order", &state);
    tb_free(state.work);
    tb_free(records);
    tb_free(in_order);
    tb_free(integers);
}
