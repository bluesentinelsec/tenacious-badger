/** @file process.h
 * @brief The running process: its command-line arguments, as UTF-8, and
 * where its standard output and standard error go.
 *
 * Text in the engine is UTF-8. On Windows the C runtime gives `main` its
 * arguments in the system's ANSI code page, where a name outside that code
 * page arrives as question marks; tb_process_get_arguments() reads them
 * again from the system, as UTF-16, and converts them. Linux and macOS give
 * `main` the bytes the program was started with, which are passed on as
 * they are: a file name there need not be UTF-8. Implementations are
 * build-selected.
 *
 * @code
 * int main(int count, char **values)
 * {
 *     tb_arguments arguments = TB_ARGUMENTS_INIT;
 *     if (!tb_process_get_arguments(count, values, &arguments))
 *     {
 *         return EXIT_FAILURE;
 *     }
 *     ...arguments.values[0] to arguments.values[arguments.count - 1]...
 *     tb_process_release_arguments(&arguments);
 * }
 * @endcode
 *
 * @par Threading
 * Thread-safe; each call fills its own arguments.
 */
#ifndef TB_PROCESS_H
#define TB_PROCESS_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief The arguments of the process. Zero-initialize with
 * #TB_ARGUMENTS_INIT; release with tb_process_release_arguments(). */
typedef struct tb_arguments
{
    size_t count; /**< Number of `values`; at least one, the program. */
    /** The arguments, the program first, each terminated; valid until the
     * arguments are released. */
    const char *const *values;
    void *storage; /**< Private: memory the values live in, or NULL. */
} tb_arguments;

/** @brief Arguments before tb_process_get_arguments(). */
#define TB_ARGUMENTS_INIT {0, NULL, NULL}

/**
 * @brief Get the process's command-line arguments as UTF-8.
 *
 * @param count `main`'s first parameter. Linux and macOS use it and
 * `values`; Windows reads the command line from the system instead, and
 * splits it with CommandLineToArgvW, whose rules for quotes and
 * backslashes are the C runtime's in all but rare cases.
 * @param values `main`'s second parameter; required. On Linux and macOS
 * the arguments returned are these, and stay valid as long as they do.
 * @param[out] arguments Required, as #TB_ARGUMENTS_INIT; filled on success,
 * left so on failure.
 * @return Whether the arguments were got. false only when the system is out
 * of memory or cannot give the command line.
 * @par Threading Thread-safe.
 */
TB_API bool tb_process_get_arguments(int count, char **values, tb_arguments *arguments);

/**
 * @brief Release what tb_process_get_arguments() made, and zero the
 * arguments.
 * @param arguments Required; zero arguments are accepted.
 * @par Threading Must not race any use of the arguments.
 */
TB_API void tb_process_release_arguments(tb_arguments *arguments);

/**
 * @brief Send the process's standard output and standard error to a file,
 * for the rest of its life.
 *
 * Programs write their log to standard error and their results to standard
 * output, and where nothing reads them they are lost: a game started from a
 * launcher, or run under Proton, has neither. This makes or empties the file
 * at `path` and makes both streams write to it, in the order they are
 * written: the C runtime's `stdout` and `stderr`, which tb_fmt_print(),
 * tb_fmt_print_error() and the default logger write to, and the system's own
 * (file descriptors 1 and 2, or the standard handles on Windows), which a
 * crash report writes to. Neither stream is buffered from
 * then on, so the file holds every line written before a crash.
 *
 * @param path The file, as UTF-8; required. Its directory must exist. The
 * file is shared for reading, so another program can follow it as it grows.
 * @return Whether the streams now write to the file. On false the file could
 * not be made, or the streams could not be moved to it, and both still write
 * where they did.
 * @par Threading
 * Call it once, early in `main`, before the program writes to either stream
 * and before it starts other threads: the C standard lets a stream's
 * buffering change only before it is first used.
 */
TB_API bool tb_process_write_output_to(const char *path);

#endif
