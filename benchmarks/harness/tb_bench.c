#include "tb_bench.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"

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
    {"array", tb_bench_array},
    {"atom", tb_bench_atom},
    {"atomic", tb_bench_atomic},
    {"bit", tb_bench_bit},
    {"cli", tb_bench_cli},
    {"fmt", tb_bench_fmt},
    {"frame_memory", tb_bench_frame_memory},
    {"fs", tb_bench_fs},
    {"hash", tb_bench_hash},
    {"jobs", tb_bench_jobs},
    {"json", tb_bench_json},
    {"log", tb_bench_log},
    {"math", tb_bench_math},
    {"memory", tb_bench_memory},
    {"memory_debug", tb_bench_memory_debug},
    {"path", tb_bench_path},
    {"pool", tb_bench_pool},
    {"ring", tb_bench_ring},
    {"rng", tb_bench_rng},
    {"sort", tb_bench_sort},
    {"table", tb_bench_table},
    {"text", tb_bench_text},
    {"thread", tb_bench_thread},
    {"time", tb_bench_time},
    {"utf8", tb_bench_utf8},
    {"vm", tb_bench_vm},
    {"xp", tb_bench_xp},
    {"ap", tb_bench_ap},
    {"decimal", tb_bench_decimal},
    {"stream", tb_bench_stream},
    {"chacha20", tb_bench_chacha20},
    {"checksum", tb_bench_checksum},
    {"base64", tb_bench_base64},
    {"inflate", tb_bench_inflate},
    {"deflate", tb_bench_deflate},
    {"sha256", tb_bench_sha256},
    {"tar", tb_bench_tar},
    {"zip", tb_bench_zip},
    {"package", tb_bench_package},
    {"vfs", tb_bench_vfs},
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
 * makes the results of the timed work needed. The threads of a case add to
 * it at the same time. */
static tb_platform_atomic_size tb_bench_kept = {0};

/* Which of a case's threads the calling thread is. */
static _Thread_local size_t tb_bench_thread_index = 0;

/* ---------------------------------------------------------------------------
 * Measuring.
 * ------------------------------------------------------------------------- */

/* The threads of a case that runs on several. They gather at a gate, so
 * that starting them is not timed and none runs before the others can. */
typedef struct tb_bench_team
{
    const tb_bench_case *measured;
    tb_mutex lock;         /* Guards the fields below. */
    tb_condition changed;  /* Woken when one of them changes. */
    size_t ready_count;    /* Threads waiting at the gate. */
    size_t finished_count; /* Threads whose run has ended. */
    bool is_open;          /* The threads may run. */
} tb_bench_team;

typedef struct tb_bench_member
{
    tb_bench_team *team;
    size_t number; /* 0 for the first thread of the team. */
    tb_thread thread;
} tb_bench_member;

static void tb_bench_run_member(void *argument)
{
    const tb_bench_member *const member = argument;
    tb_bench_team *const team = member->team;

    tb_bench_thread_index = member->number;
    tb_mutex_lock(&team->lock);
    team->ready_count++;
    tb_condition_wake_all(&team->changed);
    while (!team->is_open)
    {
        tb_condition_wait(&team->changed, &team->lock);
    }
    tb_mutex_unlock(&team->lock);

    team->measured->run(team->measured->context);

    tb_mutex_lock(&team->lock);
    team->finished_count++;
    tb_condition_wake_all(&team->changed);
    tb_mutex_unlock(&team->lock);
}

/* Nanoseconds from the moment the threads of a case are let go until the
 * last of them has finished its run. */
static uint64_t tb_bench_time_team(const tb_bench_case *measured)
{
    const tb_thread_options options = {.name = "tb_bench",
                                       .stack_size = TB_THREAD_STACK_SIZE_DEFAULT};
    tb_bench_team team = {.measured = measured};
    tb_bench_member members[TB_BENCH_THREADS_MAX] = {0};
    uint64_t start = {0};
    uint64_t end = {0};

    for (size_t index = 0; index < measured->threads; index++)
    {
        members[index].team = &team;
        members[index].number = index;
        tb_thread_create_with_options(&members[index].thread, &options, tb_bench_run_member,
                                      &members[index]);
    }
    tb_mutex_lock(&team.lock);
    while (team.ready_count < measured->threads)
    {
        tb_condition_wait(&team.changed, &team.lock);
    }
    start = tb_platform_clock_nanoseconds();
    team.is_open = true;
    tb_condition_wake_all(&team.changed);
    while (team.finished_count < measured->threads)
    {
        tb_condition_wait(&team.changed, &team.lock);
    }
    end = tb_platform_clock_nanoseconds();
    tb_mutex_unlock(&team.lock);
    for (size_t index = 0; index < measured->threads; index++)
    {
        tb_thread_join(&members[index].thread);
    }
    return end - start;
}

/* Nanoseconds that one run of a case took. */
static uint64_t tb_bench_time_once(const tb_bench_case *measured)
{
    uint64_t nanoseconds = {0};

    if (measured->setup != NULL)
    {
        measured->setup(measured->context);
    }
    if (measured->threads > 1)
    {
        nanoseconds = tb_bench_time_team(measured);
    }
    else
    {
        const uint64_t start = tb_platform_clock_nanoseconds();

        measured->run(measured->context);
        nanoseconds = tb_platform_clock_nanoseconds() - start;
    }
    if (measured->teardown != NULL)
    {
        measured->teardown(measured->context);
    }
    return nanoseconds;
}

size_t tb_bench_thread_number(void)
{
    return tb_bench_thread_index;
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
        TB_ASSERT_MSG(cases[index].threads <= TB_BENCH_THREADS_MAX,
                      "a case runs on at most TB_BENCH_THREADS_MAX threads");
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
    /* The sum only has to depend on every value; where a size is narrower
     * than the value, the low bits do. */
    (void)tb_platform_atomic_size_add(&tb_bench_kept, (size_t)value);
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
                             (uint64_t)tb_platform_atomic_size_load(&tb_bench_kept));
    return EXIT_SUCCESS;
}
