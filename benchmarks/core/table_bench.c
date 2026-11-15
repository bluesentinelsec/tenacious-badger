/* The hash table at a small and a large size, with keys compared by address
 * and with string keys compared by content. */
#include "tb_bench.h"

#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* About this many operations are timed in one run, whatever the table size:
 * a small table is filled, or searched, that many more times. */
#define TABLE_OPERATIONS_PER_RUN ((size_t)2000000)

/* Bytes of the objects whose addresses are the keys, and of each name. */
#define TABLE_OBJECT_SIZE ((size_t)48)

typedef struct table_keys
{
    size_t count;  /* Entries in the table. */
    size_t rounds; /* Times each run repeats its pass over the keys. */
    void **keys;   /* The keys, in the order they are added. */
    void **probes; /* The same keys in another order. */
    void **absent; /* As many keys that are never in the table. */
    tb_table_hash_function *hash;
    tb_table_equal_function *equal;
    char *storage; /* What the keys point into. */
} table_keys;

typedef struct table_case
{
    const table_keys *keys;
    bool reserves;   /* Create the table with room for every key. */
    tb_table *table; /* The table of the run in progress. */
} table_case;

/* ---------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

static void shuffle(void **items, size_t count, uint64_t *state)
{
    for (size_t index = count; index > 1; index--)
    {
        const size_t other = (size_t)(tb_bench_random(state) % index);
        void *const held = items[index - 1];

        items[index - 1] = items[other];
        items[other] = held;
    }
}

/* Makes `count` keys that are in the table and `count` that are not. The keys
 * are addresses of objects, as the keys of most engine tables are, or names
 * as an asset registry has them. */
static table_keys make_keys(const tb_bench *bench, size_t full_count, bool are_strings)
{
    table_keys keys = {0};
    uint64_t state = 42;

    keys.count = tb_bench_size(bench, full_count);
    keys.rounds = tb_bench_size(bench, TABLE_OPERATIONS_PER_RUN) / keys.count;
    if (keys.rounds == 0)
    {
        keys.rounds = 1;
    }
    keys.keys = tb_calloc(keys.count, sizeof(*keys.keys));
    keys.probes = tb_calloc(keys.count, sizeof(*keys.probes));
    keys.absent = tb_calloc(keys.count, sizeof(*keys.absent));
    keys.storage = tb_calloc(keys.count * 2, TABLE_OBJECT_SIZE);
    for (size_t index = 0; index < keys.count * 2; index++)
    {
        char *const object = keys.storage + index * TABLE_OBJECT_SIZE;

        if (are_strings)
        {
            /* Multiplying by a number with no factor in common with the
             * modulus gives every index a different name. */
            (void)tb_fmt_buffer(object, TABLE_OBJECT_SIZE, "textures/level_%02u/prop_%07u.dds",
                                (unsigned)(index % 37), (unsigned)((index * 7919 + 13) % 10000000));
        }
        if (index < keys.count)
        {
            keys.keys[index] = object;
        }
        else
        {
            keys.absent[index - keys.count] = object;
        }
    }
    if (are_strings)
    {
        keys.hash = tb_table_string_hash;
        keys.equal = tb_table_string_equal;
    }
    shuffle(keys.keys, keys.count, &state);
    shuffle(keys.absent, keys.count, &state);
    for (size_t index = 0; index < keys.count; index++)
    {
        keys.probes[index] = keys.keys[index];
    }
    shuffle(keys.probes, keys.count, &state);
    return keys;
}

static void free_keys(table_keys *keys)
{
    tb_free(keys->keys);
    tb_free(keys->probes);
    tb_free(keys->absent);
    tb_free(keys->storage);
}

/* ---------------------------------------------------------------------------
 * Cases.
 * ------------------------------------------------------------------------- */

static tb_table *create_table(const table_case *measured)
{
    const table_keys *const keys = measured->keys;
    size_t capacity_hint = 0;

    if (measured->reserves)
    {
        capacity_hint = keys->count;
    }
    return tb_table_create(NULL, capacity_hint, keys->hash, keys->equal);
}

static void set_up_empty(void *context)
{
    table_case *const measured = context;

    measured->table = create_table(measured);
}

static void set_up_full(void *context)
{
    table_case *const measured = context;
    const table_keys *const keys = measured->keys;

    measured->table = create_table(measured);
    for (size_t index = 0; index < keys->count; index++)
    {
        (void)tb_table_put(measured->table, keys->keys[index], keys->keys[index]);
    }
}

static void tear_down(void *context)
{
    table_case *const measured = context;

    tb_table_destroy(measured->table);
    measured->table = NULL;
}

/* Fills a table from empty; a small table is made and filled again and again. */
static void run_insert(void *context)
{
    table_case *const measured = context;
    const table_keys *const keys = measured->keys;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        if (round > 0)
        {
            tb_table_destroy(measured->table);
            measured->table = create_table(measured);
        }
        for (size_t index = 0; index < keys->count; index++)
        {
            (void)tb_table_put(measured->table, keys->keys[index], keys->keys[index]);
        }
    }
    tb_bench_keep(tb_table_length(measured->table));
}

static void run_hit(void *context)
{
    const table_case *const measured = context;
    const table_keys *const keys = measured->keys;
    uint64_t sum = 0;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        for (size_t index = 0; index < keys->count; index++)
        {
            sum += (uintptr_t)tb_table_get(measured->table, keys->probes[index]);
        }
    }
    tb_bench_keep(sum);
}

static void run_miss(void *context)
{
    const table_case *const measured = context;
    const table_keys *const keys = measured->keys;
    uint64_t found = 0;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        for (size_t index = 0; index < keys->count; index++)
        {
            if (tb_table_contains(measured->table, keys->absent[index]))
            {
                found++;
            }
        }
    }
    tb_bench_keep(found);
}

/* A table at a steady size: each step removes one key and adds another. The
 * keys that are present and the keys that are absent change places from one
 * round to the next. */
static void run_churn(void *context)
{
    table_case *const measured = context;
    const table_keys *const keys = measured->keys;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        void **leaving = keys->probes;
        void **arriving = keys->absent;

        if (round % 2 == 1)
        {
            leaving = keys->absent;
            arriving = keys->probes;
        }
        for (size_t index = 0; index < keys->count; index++)
        {
            (void)tb_table_remove(measured->table, leaving[index], NULL);
            (void)tb_table_put(measured->table, arriving[index], arriving[index]);
        }
    }
    tb_bench_keep(tb_table_length(measured->table));
}

static void run_iterate(void *context)
{
    const table_case *const measured = context;
    const table_keys *const keys = measured->keys;
    uint64_t sum = 0;

    for (size_t round = 0; round < keys->rounds; round++)
    {
        size_t cursor = 0;
        tb_table_entry entry = {0};

        while (tb_table_next(measured->table, &cursor, &entry))
        {
            sum += (uintptr_t)entry.value;
        }
    }
    tb_bench_keep(sum);
}

/* Measures every operation on one set of keys. `label` names the keys in
 * each case: "1000 pointers". */
static void measure_keys(tb_bench *bench, const char *label, size_t full_count, bool are_strings)
{
    table_keys keys = make_keys(bench, full_count, are_strings);
    table_case plain = {.keys = &keys};
    table_case reserved = {.keys = &keys, .reserves = true};
    const double operations = (double)keys.count * (double)keys.rounds;
    char names[6][64] = {{0}};
    tb_bench_case cases[6] = {
        {.setup = set_up_empty, .run = run_insert, .teardown = tear_down, .context = &plain},
        {.setup = set_up_empty, .run = run_insert, .teardown = tear_down, .context = &reserved},
        {.setup = set_up_full, .run = run_hit, .teardown = tear_down, .context = &plain},
        {.setup = set_up_full, .run = run_miss, .teardown = tear_down, .context = &plain},
        {.setup = set_up_full, .run = run_churn, .teardown = tear_down, .context = &plain},
        {.setup = set_up_full, .run = run_iterate, .teardown = tear_down, .context = &plain},
    };
    static const char *const operation_names[6] = {"insert",
                                                   "insert, room reserved",
                                                   "find a key that is present",
                                                   "find a key that is absent",
                                                   "remove one key and add another",
                                                   "visit every entry"};

    for (size_t index = 0; index < 6; index++)
    {
        (void)tb_fmt_buffer(names[index], sizeof(names[index]), "%s: %s", label,
                            operation_names[index]);
        cases[index].name = names[index];
        cases[index].variant = "badger";
        cases[index].operations = operations;
        /* Each is measured alone: they are different operations, not rivals. */
        tb_bench_measure(bench, "table", &cases[index], 1);
    }
    free_keys(&keys);
}

void tb_bench_table(tb_bench *bench)
{
    if (!tb_bench_wants(bench, "table"))
    {
        return;
    }
    measure_keys(bench, "1,000 pointers", 1000, false);
    measure_keys(bench, "1,000,000 pointers", 1000000, false);
    measure_keys(bench, "1,000 strings", 1000, true);
    measure_keys(bench, "100,000 strings", 100000, true);
}
