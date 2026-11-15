#include "tb_bench.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/platform.h"
#include "badger/str.h"

#include <inttypes.h>
#include <stdlib.h>

/* Times each case is run when the command line does not say. The fastest of
 * them is reported. */
#define TB_BENCH_DEFAULT_REPEATS ((size_t)9)

/* The size cases work at in quick mode: enough to run every line of a
 * benchmark, small enough that all of them finish at once in a Debug build. */
#define TB_BENCH_QUICK_SIZE ((size_t)200)

/* Most cases that tb_bench_measure() takes in one call. */
#define TB_BENCH_GROUP_CAPACITY ((size_t)8)

/* A result is printed to this many parts of a nanosecond. */
#define TB_BENCH_PARTS_PER_NANOSECOND 100

/* tb_fmt_print() or tb_fmt_print_error(): where some output is to go. */
typedef size_t tb_bench_print(const char *format, ...);

typedef struct tb_bench_module
{
    const char *name;
    void (*function)(tb_bench *bench);
} tb_bench_module;

static const tb_bench_module tb_bench_modules[] = {
    {"array", tb_bench_array}, {"atom", tb_bench_atom},   {"bit", tb_bench_bit},
    {"fmt", tb_bench_fmt},     {"hash", tb_bench_hash},   {"memory", tb_bench_memory},
    {"ring", tb_bench_ring},   {"table", tb_bench_table}, {"text", tb_bench_text},
};

#define TB_BENCH_MODULE_COUNT (sizeof(tb_bench_modules) / sizeof(tb_bench_modules[0]))

struct tb_bench
{
    bool is_wanted[TB_BENCH_MODULE_COUNT]; /* The modules named on the command line. */
    bool runs_every_module;                /* None was named. */
    bool is_quick;                         /* Every case once, at a small size. */
    size_t repeats;                        /* Times each case is run. */
    size_t case_count;                     /* Cases measured so far. */
};

/* Sum of everything given to tb_bench_keep(). Printing it at the end is what
 * makes the results of the timed work needed. */
static uint64_t tb_bench_kept = 0;

/* ---------------------------------------------------------------------------
 * Measuring.
 * ------------------------------------------------------------------------- */

/* Nanoseconds that one run of a case took. */
static uint64_t tb_bench_time_once(const tb_bench_case *measured)
{
    uint64_t start = {0};
    uint64_t end = {0};

    if (measured->setup != NULL)
    {
        measured->setup(measured->context);
    }
    start = tb_platform_clock_nanoseconds();
    measured->run(measured->context);
    end = tb_platform_clock_nanoseconds();
    if (measured->teardown != NULL)
    {
        measured->teardown(measured->context);
    }
    return end - start;
}

/* Index of a module in tb_bench_modules, or TB_BENCH_MODULE_COUNT. */
static size_t tb_bench_module_index(const char *name)
{
    for (size_t index = 0; index < TB_BENCH_MODULE_COUNT; index++)
    {
        if (tb_str_equal(tb_bench_modules[index].name, name))
        {
            return index;
        }
    }
    return TB_BENCH_MODULE_COUNT;
}

bool tb_bench_wants(const tb_bench *bench, const char *module)
{
    size_t index = {0};

    TB_ASSERT(bench != NULL);
    TB_ASSERT(module != NULL);
    if (bench->runs_every_module)
    {
        return true;
    }
    index = tb_bench_module_index(module);
    return index < TB_BENCH_MODULE_COUNT && bench->is_wanted[index];
}

size_t tb_bench_size(const tb_bench *bench, size_t full_size)
{
    TB_ASSERT(bench != NULL);
    if (bench->is_quick && full_size > TB_BENCH_QUICK_SIZE)
    {
        return TB_BENCH_QUICK_SIZE;
    }
    return full_size;
}

/* Prints one result. The time is written from whole numbers, to two decimal
 * places, because badger/fmt.h does not convert floating point yet. */
static void tb_bench_print_result(const char *module, const tb_bench_case *measured,
                                  double nanoseconds)
{
    const uint64_t parts = (uint64_t)(nanoseconds * TB_BENCH_PARTS_PER_NANOSECOND + 0.5);

    (void)tb_fmt_print("%s\t%s\t%s\t%" PRIu64 ".%02" PRIu64 "\n", module, measured->name,
                       measured->variant, parts / TB_BENCH_PARTS_PER_NANOSECOND,
                       parts % TB_BENCH_PARTS_PER_NANOSECOND);
}

void tb_bench_measure(tb_bench *bench, const char *module, const tb_bench_case *cases, size_t count)
{
    uint64_t fastest[TB_BENCH_GROUP_CAPACITY] = {0};

    TB_ASSERT(bench != NULL);
    TB_ASSERT(module != NULL);
    TB_ASSERT(cases != NULL);
    TB_ASSERT_MSG(count >= 1 && count <= TB_BENCH_GROUP_CAPACITY,
                  "a group holds between one and TB_BENCH_GROUP_CAPACITY cases");
    if (!tb_bench_wants(bench, module))
    {
        return;
    }
    for (size_t index = 0; index < count; index++)
    {
        TB_ASSERT(cases[index].name != NULL);
        TB_ASSERT(cases[index].variant != NULL);
        TB_ASSERT(cases[index].run != NULL);
        TB_ASSERT_MSG(cases[index].operations > 0, "a case performs at least one operation");
        fastest[index] = UINT64_MAX;
    }
    /* Round after round, each case once: a slow spell of the machine then
     * costs every case one round, and none of them its fastest. */
    for (size_t round = 0; round < bench->repeats; round++)
    {
        for (size_t index = 0; index < count; index++)
        {
            const uint64_t nanoseconds = tb_bench_time_once(&cases[index]);

            if (nanoseconds < fastest[index])
            {
                fastest[index] = nanoseconds;
            }
        }
    }
    for (size_t index = 0; index < count; index++)
    {
        tb_bench_print_result(module, &cases[index],
                              (double)fastest[index] / cases[index].operations);
    }
    (void)tb_fmt_flush();
    bench->case_count += count;
}

void tb_bench_keep(uint64_t value)
{
    tb_bench_kept += value;
}

uint64_t tb_bench_random(uint64_t *state)
{
    uint64_t value = {0};

    TB_ASSERT(state != NULL);
    /* SplitMix64: a counter, and a finalizer that scatters it. */
    *state += UINT64_C(0x9E3779B97F4A7C15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31);
}

/* ---------------------------------------------------------------------------
 * The command line.
 * ------------------------------------------------------------------------- */

static void tb_bench_print_usage(tb_bench_print *print)
{
    (void)print("usage: tb_bench [--quick] [--repeats N] [module ...]\n"
                "\n"
                "Times the operations of the engine's foundation modules. Each line of\n"
                "output is a module, a case, who performed it, and the nanoseconds one\n"
                "operation took in the fastest run, separated by tabs.\n"
                "\n"
                "  --quick      run every case once at a small size, to check that it runs\n"
                "  --repeats N  run every case N times and report the fastest (default 9)\n"
                "\n"
                "modules:");
    for (size_t index = 0; index < TB_BENCH_MODULE_COUNT; index++)
    {
        (void)print(" %s", tb_bench_modules[index].name);
    }
    (void)print("\n");
}

/* Reads the count after --repeats. Returns false when it is not a number of
 * at least one. */
static bool tb_bench_parse_repeats(const char *text, size_t *repeats)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);

    if (end == text || *end != '\0' || value == 0 || value > 1000000UL)
    {
        return false;
    }
    *repeats = (size_t)value;
    return true;
}

/* Fills in a run from the command line. Returns false, having explained why
 * on stderr, when the command line is not understood. */
static bool tb_bench_parse(tb_bench *bench, int argument_count, char **arguments, bool *wants_help)
{
    bool has_repeats = false;

    bench->repeats = TB_BENCH_DEFAULT_REPEATS;
    bench->runs_every_module = true;
    for (int position = 1; position < argument_count; position++)
    {
        const char *const argument = arguments[position];
        size_t module = {0};

        if (tb_str_equal(argument, "--help"))
        {
            *wants_help = true;
            continue;
        }
        if (tb_str_equal(argument, "--quick"))
        {
            bench->is_quick = true;
            continue;
        }
        if (tb_str_equal(argument, "--repeats"))
        {
            position++;
            if (position >= argument_count ||
                !tb_bench_parse_repeats(arguments[position], &bench->repeats))
            {
                (void)tb_fmt_print_error("tb_bench: --repeats needs a number of at least 1\n");
                return false;
            }
            has_repeats = true;
            continue;
        }
        module = tb_bench_module_index(argument);
        if (module == TB_BENCH_MODULE_COUNT)
        {
            (void)tb_fmt_print_error("tb_bench: there is no option or module named %s\n", argument);
            return false;
        }
        bench->is_wanted[module] = true;
        bench->runs_every_module = false;
    }
    if (bench->is_quick && !has_repeats)
    {
        bench->repeats = 1;
    }
    return true;
}

int tb_bench_main(int argument_count, char **arguments)
{
    tb_bench bench = {0};
    bool wants_help = false;

    if (!tb_bench_parse(&bench, argument_count, arguments, &wants_help))
    {
        tb_bench_print_usage(tb_fmt_print_error);
        return EXIT_FAILURE;
    }
    if (wants_help)
    {
        tb_bench_print_usage(tb_fmt_print);
        return EXIT_SUCCESS;
    }
    for (size_t index = 0; index < TB_BENCH_MODULE_COUNT; index++)
    {
        tb_bench_modules[index].function(&bench);
    }
    (void)tb_fmt_print_error("tb_bench: %zu cases, checksum %016" PRIx64 "\n", bench.case_count,
                             tb_bench_kept);
    return EXIT_SUCCESS;
}
