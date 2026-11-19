/** @file tb_bench.h
 * @brief The in-house benchmark harness: times cases and prints one line each.
 *
 * A benchmark is a function that does a known number of operations. The
 * harness runs it several times and reports the fastest run as nanoseconds per
 * operation, which is the run least disturbed by the rest of the machine.
 *
 * @code
 * static void push_integers(void *context)
 * {
 *     tb_array *const array = context;
 *
 *     for (size_t index = 0; index < PUSHES; index++)
 *     {
 *         TB_ARRAY_PUSH(array, int, (int)index);
 *     }
 * }
 *
 * void tb_bench_array(tb_bench *bench)
 * {
 *     const tb_bench_case cases[] = {
 *         {.name = "push", .variant = "badger", .operations = PUSHES, .run = push_integers},
 *     };
 *
 *     tb_bench_measure(bench, "array", cases, 1);
 * }
 * @endcode
 *
 * **Cases measured together are compared fairly.** tb_bench_measure() runs
 * its cases in turn, round after round, so a change in the speed of the
 * machine falls on all of them alike. Put the cases that will be compared
 * with each other, an engine function and the C library's, in one call.
 *
 * **A case can run on several threads at once.** Set `threads`, and the
 * harness starts that many, holds them until all are ready, lets them go
 * together, and times until the last has finished. `operations` is then what
 * one thread performs, so the result is the time one thread took for one of
 * its operations while the others were at work: the same as the one-thread
 * time where threads do not get in each other's way, and more where they do.
 *
 * **Quick mode keeps the benchmarks alive.** With `--quick` every case runs
 * once at a small size. CTest runs that, so a benchmark that stops compiling
 * or crashes fails the tests on every platform. No test depends on a timing.
 */
#ifndef TB_BENCH_H
#define TB_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The state of one run of the benchmark executable. */
typedef struct tb_bench tb_bench;

/**
 * @brief A step of a case: its setup, the timed run, or its teardown.
 * @param context The pointer given in the case; may be NULL.
 */
typedef void tb_bench_function(void *context);

/** @brief One thing to time. */
typedef struct tb_bench_case
{
    const char *name;            /**< What is done: "insert 1000 keys". Required. */
    const char *variant;         /**< Who does it: "badger", "snprintf". Required. */
    double operations;           /**< Operations one run performs; more than zero. */
    tb_bench_function *setup;    /**< Runs before every timed run; may be NULL. */
    tb_bench_function *run;      /**< The timed run. Required. */
    tb_bench_function *teardown; /**< Runs after every timed run; may be NULL. */
    void *context;               /**< Passed to the three functions; may be NULL. */
    /** Threads that each perform `run` at the same time, up to
     * TB_BENCH_THREADS_MAX; 0 or 1 for the calling thread alone. With more
     * than one, `operations` counts what one of them performs, `run` is
     * given the same `context` on every thread, and `setup` and `teardown`
     * still run once, on the calling thread. */
    size_t threads;
} tb_bench_case;

/** @brief Most threads that a case can ask for. */
#define TB_BENCH_THREADS_MAX ((size_t)64)

/**
 * @brief Which of a case's threads is calling: 0 for the first, up to one
 * less than the case's `threads`. Always 0 in a case that runs on the calling
 * thread alone. For a `run` function whose threads have different parts.
 */
size_t tb_bench_thread_number(void);

/**
 * @brief Time a group of cases and print a line for each.
 *
 * Each line is `module`, the case's name, its variant, and the nanoseconds
 * per operation of its fastest run, separated by tabs. Nothing is run or
 * printed when the module was not asked for on the command line.
 *
 * @param bench The run; required non-null.
 * @param module Name of the module the cases belong to; required non-null.
 * @param cases The cases; required non-null. They run in turn in each round.
 * @param count Number of cases; at least one.
 */
void tb_bench_measure(tb_bench *bench, const char *module, const tb_bench_case *cases,
                      size_t count);

/**
 * @brief Whether a module was asked for, to skip preparing data nobody times.
 * @param bench The run; required non-null.
 * @param module Name of the module; required non-null.
 * @return true when tb_bench_measure() would run cases of this module.
 */
bool tb_bench_wants(const tb_bench *bench, const char *module);

/**
 * @brief The size a case should work at: the full size, or a small one in
 * quick mode.
 * @param bench The run; required non-null.
 * @param full_size Size of the real measurement.
 * @return `full_size`, or at most a few hundred in quick mode.
 */
size_t tb_bench_size(const tb_bench *bench, size_t full_size);

/**
 * @brief Use a result, so that the compiler cannot remove the work that
 * produced it. The values are summed and the sum is printed at the end.
 * @param value Any result of the timed work.
 * @par Threading Thread-safe; the threads of a case may each call it.
 */
void tb_bench_keep(uint64_t value);

/**
 * @brief The next number of a fixed pseudo-random sequence, the same in every
 * run and on every platform, for data that should not be orderly.
 * @param state The sequence; required non-null. Start it at any value.
 */
uint64_t tb_bench_random(uint64_t *state);

/**
 * @brief Run every benchmark. Used by the entry point.
 * @param argument_count Number of command-line arguments, as main() gets it.
 * @param arguments The arguments: `--quick`, `--repeats N`, `--help`, and the
 * names of the modules to run; none runs them all.
 * @return The exit status of the process.
 */
int tb_bench_main(int argument_count, char **arguments);

/* -------------------------------------------------------------------------
 * The benchmarks of each module. tb_bench_main() calls them all; add a new
 * one here, in tb_bench.c, and in CMakeLists.txt.
 * ------------------------------------------------------------------------- */

/** @brief Benchmarks of badger/ap.h. */
void tb_bench_ap(tb_bench *bench);
/** @brief Benchmarks of badger/array.h. */
void tb_bench_array(tb_bench *bench);
/** @brief Benchmarks of badger/atom.h. */
void tb_bench_atom(tb_bench *bench);
/** @brief Benchmarks of the atomic values of badger/platform.h. */
void tb_bench_atomic(tb_bench *bench);
/** @brief Benchmarks of badger/bit.h. */
void tb_bench_bit(tb_bench *bench);
/** @brief Benchmarks of badger/decimal.h and the floating-point conversions of fmt.h. */
void tb_bench_decimal(tb_bench *bench);
/** @brief Benchmarks of badger/fmt.h. */
void tb_bench_fmt(tb_bench *bench);
/** @brief Benchmarks of badger/hash.h. */
void tb_bench_hash(tb_bench *bench);
/** @brief Benchmarks of the scheduler of badger/jobs.h. */
void tb_bench_jobs(tb_bench *bench);
/** @brief Benchmarks of badger/memory.h, badger/arena.h, and badger/allocator.h. */
void tb_bench_memory(tb_bench *bench);
/** @brief Benchmarks of badger/ring.h. */
void tb_bench_ring(tb_bench *bench);
/** @brief Benchmarks of badger/table.h. */
void tb_bench_table(tb_bench *bench);
/** @brief Benchmarks of the searches of badger/str.h and badger/text.h. */
void tb_bench_text(tb_bench *bench);
/** @brief Benchmarks of the mutex and the condition variable of badger/thread.h. */
void tb_bench_thread(tb_bench *bench);
/** @brief Benchmarks of the clock, the sleep, and the wait of badger/platform.h. */
void tb_bench_time(tb_bench *bench);
/** @brief Benchmarks of whole files in badger/fs.h, beside the C library's streams. */
void tb_bench_fs(tb_bench *bench);
/** @brief Benchmarks of badger/log.h. */
void tb_bench_log(tb_bench *bench);
/** @brief Benchmarks of the checking functions of badger/memory_debug.h. */
void tb_bench_memory_debug(tb_bench *bench);
/** @brief Benchmarks of badger/path.h. */
void tb_bench_path(tb_bench *bench);
/** @brief Benchmarks of badger/utf8.h. */
void tb_bench_utf8(tb_bench *bench);
/** @brief Benchmarks of badger/virtual_memory.h and of arenas that reserve. */
void tb_bench_vm(tb_bench *bench);
/** @brief Benchmarks of streams over memory in badger/stream.h, beside memcpy. */
void tb_bench_stream(tb_bench *bench);
/** @brief Benchmarks of CRC-32 and Adler-32 in badger/checksum.h. */
void tb_bench_checksum(tb_bench *bench);
/** @brief Benchmarks of DEFLATE compression in badger/deflate.h. */
void tb_bench_deflate(tb_bench *bench);
/** @brief Benchmarks of DEFLATE decompression in badger/deflate.h. */
void tb_bench_inflate(tb_bench *bench);
/** @brief Benchmarks of reading and writing JSON with badger/json.h. */
void tb_bench_json(tb_bench *bench);
/** @brief Benchmarks of badger/xp.h. */
void tb_bench_xp(tb_bench *bench);

#endif
