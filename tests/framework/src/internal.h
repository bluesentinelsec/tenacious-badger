/** @file internal.h
 * @brief Interfaces shared by the test framework's own translation units.
 *
 * Nothing here is visible to test code. Each section names the source file
 * that implements it.
 */
#ifndef TB_TEST_INTERNAL_H
#define TB_TEST_INTERNAL_H

#include "tb_test.h"

#include <stdio.h>

/* --- support.c ----------------------------------------------------------- */

/**
 * @brief Allocate a zeroed array or terminate; never returns NULL.
 * @return Memory owned by the caller; release it with tb_test_release().
 */
void *tb_test_allocate_zeroed(size_t count, size_t element_size);

/** @brief Release memory from tb_test_allocate_zeroed(); NULL is ignored. */
void tb_test_release(void *memory);

/** @brief Report a misuse of the framework to stderr and abort. */
TB_TEST_PRINTF_FORMAT(1, 2) _Noreturn void tb_test_misuse(const char *format, ...);

/* --- pattern.c ----------------------------------------------------------- */

/**
 * @brief Match a whole name against a glob pattern.
 * @param pattern `*` matches any run of characters, including none; `?`
 * matches exactly one character; anything else matches itself.
 * @param text Name to test.
 * @return true when the pattern matches all of `text`.
 */
bool tb_test_pattern_matches(const char *pattern, const char *text);

/* --- random.c ------------------------------------------------------------ */

/** @brief Deterministic 64-bit generator; identical output on every platform. */
typedef struct tb_test_generator
{
    uint64_t state;
} tb_test_generator;

/** @brief Zero state for a generator about to be seeded. */
#define TB_TEST_GENERATOR_INIT {0}

/** @brief Start a sequence. Equal seeds give equal sequences. */
void tb_test_generator_seed(tb_test_generator *generator, uint64_t seed);

/** @brief Next 64 random bits. */
uint64_t tb_test_generator_next(tb_test_generator *generator);

/** @brief Uniform value in `[0, bound)`; `bound` must be greater than 0. */
uint64_t tb_test_generator_below(tb_test_generator *generator, uint64_t bound);

/**
 * @brief Derive the seed of one case from the run seed and the case name, so
 * a case draws the same values regardless of selection and order.
 */
uint64_t tb_test_generator_seed_for_case(uint64_t run_seed, const char *suite, const char *name);

/* --- options.c ----------------------------------------------------------- */

/** @brief Run seed used when the command line does not choose one. */
#define TB_TEST_DEFAULT_SEED UINT64_C(0x7462746573740001)

/** @brief Parsed command line. Pattern strings are borrowed from argv. */
typedef struct tb_test_options
{
    const char **include_patterns; /**< Owned array; an empty list selects every case. */
    size_t include_count;
    const char **exclude_patterns; /**< Owned array; a match removes a case. */
    size_t exclude_count;
    uint64_t seed;         /**< Base seed of the run. */
    size_t repeat_count;   /**< Rounds to run the selection; at least 1. */
    bool seed_from_time;   /**< Replace `seed` with a time-derived value. */
    bool list_only;        /**< Print the selection instead of running it. */
    bool fail_fast;        /**< Stop the run at the first failed case. */
    bool break_on_failure; /**< Break into the debugger at each failed check. */
    bool shuffle;          /**< Run each round in seeded random order. */
    bool verbose;          /**< Report every case, not only failures and skips. */
    bool show_help;        /**< Print the usage text instead of running. */
} tb_test_options;

/** @brief Defaults for a command line with no arguments. */
#define TB_TEST_OPTIONS_INIT {.seed = TB_TEST_DEFAULT_SEED, .repeat_count = 1}

/**
 * @brief Parse a command line.
 * @param argc Argument count; values below 1 mean "no arguments".
 * @param argv Argument vector; pattern strings stay borrowed from it.
 * @param[out] options Initialized with TB_TEST_OPTIONS_INIT. Always release
 * it with tb_test_options_release(), on success and on failure.
 * @param[out] error Receives a one-line explanation on failure.
 * @param error_size Capacity of `error`; must be greater than 0.
 * @return true on success; false when the command line is not valid.
 */
bool tb_test_options_parse(int argc, char **argv, tb_test_options *options, char *error,
                           size_t error_size);

/** @brief Free the arrays owned by parsed options and reset them. */
void tb_test_options_release(tb_test_options *options);

/** @brief Print the command-line reference. */
void tb_test_options_print_usage(FILE *stream, const char *program);

/* --- report.c ------------------------------------------------------------ */

/** @brief Print a string in double quotes with escapes, or `(null)`. */
void tb_test_report_string(const char *text);

/** @brief Index of the first byte at which two strings differ. */
size_t tb_test_report_string_difference(const char *left, const char *right);

/**
 * @brief Print the bytes around one offset in hexadecimal, marking that byte.
 * @param bytes Range to print from; required non-null.
 * @param size Number of bytes in the range.
 * @param focus Offset of the byte to mark; must be less than `size`.
 */
void tb_test_report_bytes(const unsigned char *bytes, size_t size, size_t focus);

/* --- runner.c ------------------------------------------------------------ */

/**
 * @brief Count one check and open its failure report when it failed.
 * @return false when the check passed and there is nothing to report. true
 * after printing the location and case name; the caller then prints the
 * explanation to stdout, ends its line, and calls tb_test_failure_end().
 */
bool tb_test_failure_begin(const tb_test_site *site, bool passed);

/**
 * @brief Close a failure report: print the context, then honor `--break`.
 *
 * Does not return for a fatal site; the case ends there.
 */
void tb_test_failure_end(const tb_test_site *site);

#endif
