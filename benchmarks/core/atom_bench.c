/* Interning names: new ones, ones that are already atoms, looking for ones
 * that are not, and releasing a table. */
#include "tb_bench.h"

#include "badger/atom.h"
#include "badger/fmt.h"
#include "badger/memory.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ATOM_OPERATIONS_PER_RUN ((size_t)2000000)
#define ATOM_NAME_SIZE ((size_t)48)

typedef struct atom_names
{
    size_t count;
    size_t rounds;
    char *names;   /* `count` names to intern, each in ATOM_NAME_SIZE bytes. */
    char *copies;  /* The same names in another order, in memory of their own. */
    char *absent;  /* `count` names that are never interned. */
    size_t length; /* Every name has this many bytes. */
    tb_atom_table *table;
} atom_names;

static void write_name(char *name, size_t index)
{
    (void)tb_fmt_buffer(name, ATOM_NAME_SIZE, "textures/level_%02u/prop_%07u.dds",
                        (unsigned)(index % 37), (unsigned)((index * 7919 + 13) % 10000000));
}

static atom_names make_names(const tb_bench *bench, size_t full_count)
{
    atom_names names = {0};
    uint64_t state = 77;

    names.count = tb_bench_size(bench, full_count);
    names.rounds = tb_bench_size(bench, ATOM_OPERATIONS_PER_RUN) / names.count;
    if (names.rounds == 0)
    {
        names.rounds = 1;
    }
    names.names = tb_calloc(names.count, ATOM_NAME_SIZE);
    names.copies = tb_calloc(names.count, ATOM_NAME_SIZE);
    names.absent = tb_calloc(names.count, ATOM_NAME_SIZE);
    for (size_t index = 0; index < names.count; index++)
    {
        write_name(names.names + index * ATOM_NAME_SIZE, index);
        write_name(names.absent + index * ATOM_NAME_SIZE, index + names.count);
    }
    names.length = strlen(names.names);
    /* The copies are looked up in a shuffled order, so that a lookup compares
     * bytes it has not just written. */
    memcpy(names.copies, names.names, names.count * ATOM_NAME_SIZE);
    for (size_t index = names.count; index > 1; index--)
    {
        const size_t other = (size_t)(tb_bench_random(&state) % index);
        char held[ATOM_NAME_SIZE] = {0};

        memcpy(held, names.copies + (index - 1) * ATOM_NAME_SIZE, ATOM_NAME_SIZE);
        memcpy(names.copies + (index - 1) * ATOM_NAME_SIZE, names.copies + other * ATOM_NAME_SIZE,
               ATOM_NAME_SIZE);
        memcpy(names.copies + other * ATOM_NAME_SIZE, held, ATOM_NAME_SIZE);
    }
    return names;
}

static void set_up_empty(void *context)
{
    atom_names *const names = context;

    names->table = tb_atom_table_create(NULL);
}

static void set_up_full(void *context)
{
    atom_names *const names = context;

    names->table = tb_atom_table_create(NULL);
    for (size_t index = 0; index < names->count; index++)
    {
        (void)tb_atom_new(names->table, names->names + index * ATOM_NAME_SIZE, names->length);
    }
}

static void tear_down(void *context)
{
    atom_names *const names = context;

    tb_atom_table_destroy(names->table);
    names->table = NULL;
}

static void run_new(void *context)
{
    atom_names *const names = context;
    uint64_t sum = 0;

    /* A small table is made and filled again and again. */
    for (size_t round = 0; round < names->rounds; round++)
    {
        if (round > 0)
        {
            tb_atom_table_destroy(names->table);
            names->table = tb_atom_table_create(NULL);
        }
        for (size_t index = 0; index < names->count; index++)
        {
            sum += (uintptr_t)tb_atom_new(names->table, names->names + index * ATOM_NAME_SIZE,
                                          names->length);
        }
    }
    tb_bench_keep(sum);
}

/* The common call: a name that is already an atom, given as a C string. */
static void run_existing(void *context)
{
    atom_names *const names = context;
    uint64_t sum = 0;

    for (size_t round = 0; round < names->rounds; round++)
    {
        for (size_t index = 0; index < names->count; index++)
        {
            sum += (uintptr_t)tb_atom_string(names->table, names->copies + index * ATOM_NAME_SIZE);
        }
    }
    tb_bench_keep(sum);
}

static void run_absent(void *context)
{
    const atom_names *const names = context;
    uint64_t found = 0;

    for (size_t round = 0; round < names->rounds; round++)
    {
        for (size_t index = 0; index < names->count; index++)
        {
            if (tb_atom_find(names->table, names->absent + index * ATOM_NAME_SIZE, names->length) !=
                NULL)
            {
                found++;
            }
        }
    }
    tb_bench_keep(found);
}

static void run_destroy(void *context)
{
    atom_names *const names = context;

    tb_atom_table_destroy(names->table);
    names->table = NULL;
}

static void measure_names(tb_bench *bench, const char *label, size_t full_count)
{
    atom_names names = make_names(bench, full_count);
    const double once = (double)names.count;
    const double repeated = (double)names.count * (double)names.rounds;
    char case_names[4][64] = {{0}};
    tb_bench_case cases[4] = {
        {.operations = repeated, .setup = set_up_empty, .run = run_new, .teardown = tear_down},
        {.operations = repeated, .setup = set_up_full, .run = run_existing, .teardown = tear_down},
        {.operations = repeated, .setup = set_up_full, .run = run_absent, .teardown = tear_down},
        {.operations = once, .setup = set_up_full, .run = run_destroy},
    };
    static const char *const operation_names[4] = {
        "intern a new name", "intern a name that is an atom", "look for a name that is not an atom",
        "destroy the table (per atom)"};

    for (size_t index = 0; index < 4; index++)
    {
        (void)tb_fmt_buffer(case_names[index], sizeof(case_names[index]), "%s: %s", label,
                            operation_names[index]);
        cases[index].name = case_names[index];
        cases[index].variant = "badger";
        cases[index].context = &names;
        tb_bench_measure(bench, "atom", &cases[index], 1);
    }
    tb_free(names.names);
    tb_free(names.copies);
    tb_free(names.absent);
}

void tb_bench_atom(tb_bench *bench)
{
    if (!tb_bench_wants(bench, "atom"))
    {
        return;
    }
    measure_names(bench, "1,000 names", 1000);
    measure_names(bench, "100,000 names", 100000);
}
