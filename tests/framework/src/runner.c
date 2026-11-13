#include "internal.h"
#include "platform.h"

#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Capacity of each formatted message kept by the runner, including the NUL. */
#define TB_TEST_MESSAGE_CAPACITY 512

/* Value passed through longjmp; only "non-zero" matters to the receivers. */
#define TB_TEST_JUMP_TAKEN 1

typedef enum tb_test_outcome
{
    TB_TEST_OUTCOME_PASSED,
    TB_TEST_OUTCOME_FAILED,
    TB_TEST_OUTCOME_SKIPPED
} tb_test_outcome;

/* Totals over every executed case of a run. */
typedef struct tb_test_totals
{
    size_t passed;
    size_t failed;
    size_t skipped;
    size_t not_run; /* Selected, but cut off by --fail-fast. */
    uint64_t checks;
} tb_test_totals;

/* Everything the runner knows about the case in progress. It is static, not
 * automatic, so its values stay defined when a case leaves through longjmp. */
typedef struct tb_test_state
{
    const tb_test_options *options; /* Borrowed for the duration of tb_test_run. */
    const tb_test_case *current;    /* NULL whenever no case is running. */
    jmp_buf case_exit;              /* Target of a fatal failure, skip, or trap. */
    jmp_buf trap_exit;              /* Target of an expected trap. */
    bool trap_is_expected;          /* A trap check is running its body. */
    bool is_skipped;
    uint64_t check_count;
    uint64_t failure_count;
    uint64_t seed;
    tb_test_generator generator;
    char context[TB_TEST_MESSAGE_CAPACITY];
    char skip_reason[TB_TEST_MESSAGE_CAPACITY];
    char trap_message[TB_TEST_MESSAGE_CAPACITY];
} tb_test_state;

static tb_test_state tb_test_running = {0};

/* ---------------------------------------------------------------------------
 * State shared with the checks.
 * ------------------------------------------------------------------------- */

static void tb_test_require_running_case(const char *caller)
{
    if (tb_test_running.current == NULL)
    {
        tb_test_misuse("%s was used outside a running test case", caller);
    }
}

static _Noreturn void tb_test_leave_case(void)
{
    longjmp(tb_test_running.case_exit, TB_TEST_JUMP_TAKEN);
}

static void tb_test_print_failure_heading(const char *file, int line)
{
    tb_test_platform_print_location(file, line);
    printf(": error: %s.%s: ", tb_test_running.current->suite, tb_test_running.current->name);
}

/* Shared tail of every failure report. */
static void tb_test_finish_failure_report(void)
{
    if (tb_test_running.context[0] != '\0')
    {
        printf("    context: %s\n", tb_test_running.context);
    }
    tb_test_running.failure_count++;
    (void)fflush(stdout);
    if (tb_test_running.options->break_on_failure)
    {
        tb_test_platform_debug_break();
    }
}

bool tb_test_failure_begin(const tb_test_site *site, bool passed)
{
    tb_test_require_running_case(site->macro);
    tb_test_running.check_count++;
    if (passed)
    {
        return false;
    }
    tb_test_print_failure_heading(site->file, site->line);
    return true;
}

void tb_test_failure_end(const tb_test_site *site)
{
    tb_test_finish_failure_report();
    if (site->fatal)
    {
        tb_test_leave_case();
    }
}

void tb_test_fail_at(const char *file, int line, const char *format, ...)
{
    va_list arguments;

    tb_test_require_running_case("TB_TEST_FAIL");
    tb_test_running.check_count++;
    tb_test_print_failure_heading(file, line);
    fputs("TB_TEST_FAIL: ", stdout);
    va_start(arguments, format);
    (void)vprintf(format, arguments);
    va_end(arguments);
    fputc('\n', stdout);
    tb_test_finish_failure_report();
    tb_test_leave_case();
}

void tb_test_skip_at(const char *file, int line, const char *format, ...)
{
    va_list arguments;

    (void)file;
    (void)line;
    tb_test_require_running_case("TB_TEST_SKIP");
    va_start(arguments, format);
    (void)vsnprintf(tb_test_running.skip_reason, sizeof(tb_test_running.skip_reason), format,
                    arguments);
    va_end(arguments);
    tb_test_running.is_skipped = true;
    tb_test_leave_case();
}

void tb_test_context(const char *format, ...)
{
    va_list arguments;

    tb_test_require_running_case("TB_TEST_CONTEXT");
    va_start(arguments, format);
    (void)vsnprintf(tb_test_running.context, sizeof(tb_test_running.context), format, arguments);
    va_end(arguments);
}

void tb_test_context_clear(void)
{
    tb_test_require_running_case("tb_test_context_clear");
    tb_test_running.context[0] = '\0';
}

/* ---------------------------------------------------------------------------
 * Traps.
 * ------------------------------------------------------------------------- */

void tb_test_trap(const char *file, int line, const char *message)
{
    if (tb_test_running.current == NULL)
    {
        tb_test_misuse("trap outside a running test case at %s:%d: %s", file, line, message);
    }
    (void)snprintf(tb_test_running.trap_message, sizeof(tb_test_running.trap_message), "%s",
                   message);
    if (tb_test_running.trap_is_expected)
    {
        tb_test_running.trap_is_expected = false;
        longjmp(tb_test_running.trap_exit, TB_TEST_JUMP_TAKEN);
    }
    /* "The code under test does not trap" is the check that failed here. */
    tb_test_running.check_count++;
    tb_test_print_failure_heading(file, line);
    printf("unexpected trap: %s\n", tb_test_running.trap_message);
    tb_test_finish_failure_report();
    tb_test_leave_case();
}

/* Runs the body and reports whether it trapped. Kept free of other locals:
 * objects modified between setjmp and longjmp would be indeterminate here. */
static bool tb_test_body_traps(tb_test_trap_body *body, void *context)
{
    if (setjmp(tb_test_running.trap_exit) != 0)
    {
        return true;
    }
    body(context);
    return false;
}

void tb_test_check_trap(const tb_test_site *site, tb_test_trap_body *body, void *context,
                        const char *body_text, const char *message_part)
{
    bool trapped = {0};

    tb_test_require_running_case(site->macro);
    if (tb_test_running.trap_is_expected)
    {
        tb_test_misuse("%s cannot be nested inside the body of another trap check", site->macro);
    }
    tb_test_running.trap_is_expected = true;
    tb_test_running.trap_message[0] = '\0';
    trapped = tb_test_body_traps(body, context);
    tb_test_running.trap_is_expected = false;

    if (!trapped)
    {
        if (tb_test_failure_begin(site, false))
        {
            printf("%s(%s) failed: the body returned without a trap\n", site->macro, body_text);
            tb_test_failure_end(site);
        }
        return;
    }
    if (!tb_test_failure_begin(site, strstr(tb_test_running.trap_message, message_part) != NULL))
    {
        return;
    }
    printf("%s(%s) failed: trap message ", site->macro, body_text);
    tb_test_report_string(tb_test_running.trap_message);
    fputs(" does not contain ", stdout);
    tb_test_report_string(message_part);
    fputc('\n', stdout);
    tb_test_failure_end(site);
}

/* ---------------------------------------------------------------------------
 * Random inputs.
 * ------------------------------------------------------------------------- */

uint64_t tb_test_random_seed(void)
{
    tb_test_require_running_case("tb_test_random_seed");
    return tb_test_running.seed;
}

uint64_t tb_test_random_u64(void)
{
    tb_test_require_running_case("tb_test_random_u64");
    return tb_test_generator_next(&tb_test_running.generator);
}

uint64_t tb_test_random_below(uint64_t bound)
{
    tb_test_require_running_case("tb_test_random_below");
    if (bound == 0)
    {
        tb_test_misuse("tb_test_random_below needs a bound greater than 0");
    }
    return tb_test_generator_below(&tb_test_running.generator, bound);
}

int64_t tb_test_random_between(int64_t minimum, int64_t maximum)
{
    /* Unsigned arithmetic wraps, so the span and the offset stay defined even
     * when the range covers every int64_t value. */
    const uint64_t span = (uint64_t)maximum - (uint64_t)minimum;
    uint64_t offset = {0};
    uint64_t biased = {0};

    tb_test_require_running_case("tb_test_random_between");
    if (minimum > maximum)
    {
        tb_test_misuse("tb_test_random_between needs minimum <= maximum, got %" PRId64
                       " > %" PRId64,
                       minimum, maximum);
    }
    if (span == UINT64_MAX)
    {
        offset = tb_test_generator_next(&tb_test_running.generator);
    }
    else
    {
        offset = tb_test_generator_below(&tb_test_running.generator, span + 1);
    }
    biased = (uint64_t)minimum + offset;
    /* Map the two's-complement bit pattern back without an out-of-range
     * conversion, whose result the language leaves to the implementation. */
    if (biased <= (uint64_t)INT64_MAX)
    {
        return (int64_t)biased;
    }
    return -(int64_t)(UINT64_MAX - biased) - 1;
}

void tb_test_random_bytes(void *buffer, size_t size)
{
    unsigned char *const bytes = buffer;

    tb_test_require_running_case("tb_test_random_bytes");
    if (size > 0 && buffer == NULL)
    {
        tb_test_misuse("tb_test_random_bytes needs a buffer for %zu bytes", size);
    }
    for (size_t index = 0; index < size; index++)
    {
        bytes[index] = (unsigned char)(tb_test_generator_next(&tb_test_running.generator) >> 56);
    }
}

/* ---------------------------------------------------------------------------
 * Running cases.
 * ------------------------------------------------------------------------- */

/* Suffix that makes a counted noun plural: "1 case", "2 cases". */
static const char *tb_test_plural(uint64_t count)
{
    if (count == 1)
    {
        return "";
    }
    return "s";
}

static double tb_test_now_milliseconds(void)
{
    struct timespec now = {0};

    if (timespec_get(&now, TIME_UTC) != TIME_UTC)
    {
        return 0.0;
    }
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static uint64_t tb_test_seed_from_time(void)
{
    struct timespec now = {0};
    tb_test_generator mixer = TB_TEST_GENERATOR_INIT;

    (void)timespec_get(&now, TIME_UTC);
    tb_test_generator_seed(&mixer, (uint64_t)now.tv_sec ^ ((uint64_t)now.tv_nsec << 32));
    return tb_test_generator_next(&mixer);
}

/* Calls the case body. Returns normally whether the body returned or left
 * through tb_test_leave_case(). Kept free of other locals; see
 * tb_test_body_traps(). */
static void tb_test_call_case_body(const tb_test_case *test)
{
    if (setjmp(tb_test_running.case_exit) != 0)
    {
        return;
    }
    test->function();
}

static tb_test_outcome tb_test_run_case(const tb_test_case *test, uint64_t run_seed)
{
    const bool verbose = tb_test_running.options->verbose;
    tb_test_outcome outcome = TB_TEST_OUTCOME_PASSED;
    double started = {0};
    double elapsed = {0};

    tb_test_running.is_skipped = false;
    tb_test_running.trap_is_expected = false;
    tb_test_running.check_count = 0;
    tb_test_running.failure_count = 0;
    tb_test_running.context[0] = '\0';
    tb_test_running.skip_reason[0] = '\0';
    tb_test_running.seed = run_seed;
    tb_test_generator_seed(&tb_test_running.generator,
                           tb_test_generator_seed_for_case(run_seed, test->suite, test->name));

    if (verbose)
    {
        printf("[ RUN     ] %s.%s\n", test->suite, test->name);
        (void)fflush(stdout);
    }
    started = tb_test_now_milliseconds();
    tb_test_running.current = test;
    tb_test_call_case_body(test);
    tb_test_running.current = NULL;
    elapsed = tb_test_now_milliseconds() - started;

    if (tb_test_running.failure_count > 0)
    {
        outcome = TB_TEST_OUTCOME_FAILED;
        printf("[ FAILED  ] %s.%s: %" PRIu64 " of %" PRIu64 " checks failed; rerun with: "
               "%s.%s --seed 0x%" PRIX64 "\n",
               test->suite, test->name, tb_test_running.failure_count, tb_test_running.check_count,
               test->suite, test->name, run_seed);
    }
    else if (tb_test_running.is_skipped)
    {
        outcome = TB_TEST_OUTCOME_SKIPPED;
        printf("[ SKIPPED ] %s.%s: %s\n", test->suite, test->name, tb_test_running.skip_reason);
    }
    else if (verbose)
    {
        printf("[ PASSED  ] %s.%s (%" PRIu64 " check%s, %.2f ms)\n", test->suite, test->name,
               tb_test_running.check_count, tb_test_plural(tb_test_running.check_count), elapsed);
    }
    (void)fflush(stdout);
    return outcome;
}

static void tb_test_count_outcome(tb_test_totals *totals, tb_test_outcome outcome)
{
    totals->checks += tb_test_running.check_count;
    switch (outcome)
    {
    case TB_TEST_OUTCOME_PASSED:
        totals->passed++;
        return;
    case TB_TEST_OUTCOME_FAILED:
        totals->failed++;
        return;
    case TB_TEST_OUTCOME_SKIPPED:
        totals->skipped++;
        return;
    default:
        tb_test_misuse("unknown test outcome %d", (int)outcome);
    }
}

static void tb_test_full_name(const tb_test_case *test, char *name, size_t name_size)
{
    (void)snprintf(name, name_size, "%s.%s", test->suite, test->name);
}

static bool tb_test_matches_any(const char *const *patterns, size_t pattern_count, const char *name)
{
    for (size_t index = 0; index < pattern_count; index++)
    {
        if (tb_test_pattern_matches(patterns[index], name))
        {
            return true;
        }
    }
    return false;
}

static bool tb_test_is_selected(const tb_test_case *test, const tb_test_options *options)
{
    char name[TB_TEST_MESSAGE_CAPACITY] = {0};

    tb_test_full_name(test, name, sizeof(name));
    if (tb_test_matches_any(options->exclude_patterns, options->exclude_count, name))
    {
        return false;
    }
    if (options->include_count == 0)
    {
        return true;
    }
    return tb_test_matches_any(options->include_patterns, options->include_count, name);
}

/* Returns an owned array of the selected cases in registry order. */
static const tb_test_case **tb_test_select(const tb_test_case *const *cases, size_t case_count,
                                           const tb_test_options *options, size_t *selected_count)
{
    const tb_test_case **selected = tb_test_allocate_zeroed(case_count, sizeof(*selected));

    *selected_count = 0;
    for (size_t index = 0; index < case_count; index++)
    {
        if (tb_test_is_selected(cases[index], options))
        {
            selected[*selected_count] = cases[index];
            (*selected_count)++;
        }
    }
    return selected;
}

/* Fisher-Yates: every order is equally likely for a given seed. */
static void tb_test_shuffle(const tb_test_case **cases, size_t case_count, uint64_t seed)
{
    tb_test_generator generator = TB_TEST_GENERATOR_INIT;

    tb_test_generator_seed(&generator, seed);
    for (size_t remaining = case_count; remaining > 1; remaining--)
    {
        const size_t last = remaining - 1;
        const size_t chosen = (size_t)tb_test_generator_below(&generator, remaining);
        const tb_test_case *const swapped = cases[last];

        cases[last] = cases[chosen];
        cases[chosen] = swapped;
    }
}

static void tb_test_list(const tb_test_case *const *cases, size_t case_count)
{
    for (size_t index = 0; index < case_count; index++)
    {
        printf("%s.%s\t", cases[index]->suite, cases[index]->name);
        tb_test_platform_print_location(cases[index]->file, cases[index]->line);
        fputc('\n', stdout);
    }
}

/* Runs one round over the selection. Returns false when --fail-fast stopped it. */
static bool tb_test_run_round(const tb_test_case **order, size_t case_count, uint64_t round_seed,
                              tb_test_totals *totals)
{
    if (tb_test_running.options->shuffle)
    {
        tb_test_shuffle(order, case_count, round_seed);
    }
    for (size_t index = 0; index < case_count; index++)
    {
        const tb_test_outcome outcome = tb_test_run_case(order[index], round_seed);

        tb_test_count_outcome(totals, outcome);
        if (outcome == TB_TEST_OUTCOME_FAILED && tb_test_running.options->fail_fast)
        {
            totals->not_run += case_count - index - 1;
            return false;
        }
    }
    return true;
}

static void tb_test_print_summary(const tb_test_totals *totals, uint64_t seed, double elapsed)
{
    const size_t executed = totals->passed + totals->failed + totals->skipped;

    if (totals->not_run > 0)
    {
        printf("[ STOPPED ] --fail-fast: %zu selected case%s did not run\n", totals->not_run,
               tb_test_plural(totals->not_run));
    }
    printf("[ SUMMARY ] %zu case%s: %zu passed, %zu failed, %zu skipped; %" PRIu64
           " check%s; %.2f ms; seed 0x%" PRIX64 "\n",
           executed, tb_test_plural(executed), totals->passed, totals->failed, totals->skipped,
           totals->checks, tb_test_plural(totals->checks), elapsed, seed);
    (void)fflush(stdout);
}

static int tb_test_exit_status(const tb_test_totals *totals)
{
    if (totals->failed > 0)
    {
        return TB_TEST_EXIT_FAILED;
    }
    if (totals->passed == 0 && totals->skipped > 0)
    {
        return TB_TEST_EXIT_SKIPPED;
    }
    return TB_TEST_EXIT_PASSED;
}

static int tb_test_execute(const tb_test_case **selected, size_t selected_count,
                           const tb_test_options *options)
{
    tb_test_totals totals = {0};
    uint64_t seed = {0};
    double started = {0};

    seed = options->seed;
    if (options->seed_from_time)
    {
        seed = tb_test_seed_from_time();
    }
    /* A debugger session wants the native crash behavior; unattended runs must
     * never wait on a dialog. */
    if (!options->break_on_failure)
    {
        tb_test_platform_disable_error_dialogs();
    }

    tb_test_running.options = options;
    started = tb_test_now_milliseconds();
    for (size_t round = 0; round < options->repeat_count; round++)
    {
        /* Unsigned wraparound is the intended behavior for a seed. */
        if (!tb_test_run_round(selected, selected_count, seed + round, &totals))
        {
            break;
        }
    }
    tb_test_print_summary(&totals, seed, tb_test_now_milliseconds() - started);
    tb_test_running.options = NULL;
    return tb_test_exit_status(&totals);
}

int tb_test_run(int argc, char **argv, const tb_test_case *const *cases, size_t case_count)
{
    tb_test_options options = TB_TEST_OPTIONS_INIT;
    char error[TB_TEST_MESSAGE_CAPACITY] = {0};
    const char *program = "tb_test";
    const tb_test_case **selected = NULL;
    size_t selected_count = {0};
    int status = TB_TEST_EXIT_USAGE;

    if (argc > 0 && argv[0] != NULL)
    {
        program = argv[0];
    }
    if (!tb_test_options_parse(argc, argv, &options, error, sizeof(error)))
    {
        fprintf(stderr, "%s: %s\n\n", program, error);
        tb_test_options_print_usage(stderr, program);
        goto cleanup;
    }
    if (options.show_help)
    {
        tb_test_options_print_usage(stdout, program);
        status = TB_TEST_EXIT_PASSED;
        goto cleanup;
    }
    selected = tb_test_select(cases, case_count, &options, &selected_count);
    if (selected_count == 0)
    {
        fprintf(stderr, "%s: no test case matches the given patterns\n", program);
        goto cleanup;
    }
    if (options.list_only)
    {
        tb_test_list(selected, selected_count);
        status = TB_TEST_EXIT_PASSED;
        goto cleanup;
    }
    status = tb_test_execute(selected, selected_count, &options);

cleanup:
    free(selected);
    tb_test_options_release(&options);
    return status;
}
