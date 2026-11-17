/** @file fatal.h
 * @brief The one way the engine ends the process when it cannot go on.
 *
 * A failed assertion, exhausted memory, a system call that the engine cannot
 * do without: each is a condition that no caller can handle, and each ends
 * the process. All of them end it here, so that every one is reported in the
 * same shape, with the call stack that led to it, and can be sent to the same
 * place by one handler.
 *
 * @code
 * if (!load_shaders(&shaders))
 * {
 *     tb_fatal("render: the built-in shaders could not be loaded (%s)", reason);
 * }
 * @endcode
 *
 * By default that writes to stderr and ends the process:
 *
 * @code
 * render: the built-in shaders could not be loaded (file not found)
 *     #0 tb_fatal (badger.dll+0x1a40)
 *     #1 start_renderer (burrow.exe+0x2f1c) renderer.c:212
 *     #2 main (burrow.exe+0x1108) main.c:40
 * @endcode
 *
 * Use it for what a correct program in a working system never meets. A
 * failure that a caller can do something about is returned to the caller,
 * and a caller's own mistake is a failed TB_ASSERT().
 *
 * **Other handlers come first.** A failed assertion goes to the handler of
 * `badger/assert.h`, exhausted memory to the handler of `badger/memory.h`,
 * and a memory fault to the handler of `badger/memory_debug.h`, when one is
 * installed. Each of those reports arrives here only when it has no handler
 * of its own.
 */
#ifndef TB_FATAL_H
#define TB_FATAL_H

#include "badger/api.h"

#include <stddef.h>

/** @brief Bytes of a fatal message that are kept, its terminator included. */
#define TB_FATAL_MESSAGE_CAPACITY 512

/** @brief The most frames of the call stack that a report carries. */
#define TB_FATAL_FRAMES_MAX 48

/** @brief A condition that ends the process. */
typedef struct tb_fatal_report
{
    /** What went wrong: UTF-8, terminated, one line with no newline at its
     * end. Never NULL. */
    const char *message;
    /** The call stack that led to it, as tb_stack_capture() gives it: the
     * innermost frame first. */
    void *const *frames;
    /** How many frames there are; 0 when the stack could not be captured. */
    size_t frame_count;
} tb_fatal_report;

/**
 * @brief Receives a fatal condition in place of the default report.
 * @param report Borrowed, valid for the duration of the call.
 * @param context The pointer registered with tb_fatal_set_handler().
 * @par Errors A handler must not return: the caller cannot go on. If it
 * returns, the process ends. It may leave by a non-local jump, as a test
 * does.
 */
typedef void tb_fatal_handler(const tb_fatal_report *report, void *context);

/**
 * @brief Replace what happens when the engine meets a fatal condition.
 *
 * The default writes the report to stderr with tb_fatal_write() and ends the
 * process with tb_platform_abort(). A handler that only wants to add
 * something, such as a line in a log file, does that and then calls
 * tb_fatal_write() itself.
 *
 * @param handler Handler to install, or NULL to restore the default.
 * @param context Passed to every handler call; may be NULL. The caller keeps
 * ownership and must keep it valid while the handler is installed.
 * @par Threading Stage-owned: call during single-threaded startup.
 */
TB_API void tb_fatal_set_handler(tb_fatal_handler *handler, void *context);

/**
 * @brief Report a condition that the program cannot go on from, and end the
 * process.
 *
 * @param format A format of `badger/fmt.h`, and its arguments. The message is
 * cut off at TB_FATAL_MESSAGE_CAPACITY - 1 bytes; a newline at its end is
 * dropped. Required non-null.
 * @par Ownership Nothing is allocated by the engine: a process that is out of
 * memory can still say so.
 * @par Threading Thread-safe. Two threads that fail at once each report.
 */
TB_API _Noreturn void tb_fatal(const char *format, ...);

/**
 * @brief Report a fatal condition whose message is already a string.
 *
 * As tb_fatal(), for a caller that has no arguments to format, and for the
 * platform adapters, which report a system call that failed this way.
 *
 * @param message Required non-null; copied.
 * @par Threading Thread-safe.
 */
TB_API _Noreturn void tb_fatal_message(const char *message);

/**
 * @brief Write a report to stderr, as the default does.
 *
 * The message on a line, then a line for each frame of the stack: its
 * number, its function where that is known, its module and the offset within
 * it, and its source file and line where those are known.
 *
 * A fatal condition that is met while a report is being written ends the
 * process at once, without a second report.
 *
 * @param report Required non-null.
 * @par Threading Thread-safe; two reports written at once may interleave.
 */
TB_API void tb_fatal_write(const tb_fatal_report *report);

#endif
