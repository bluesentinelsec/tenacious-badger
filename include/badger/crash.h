/** @file crash.h
 * @brief Report the crashes that the system raises, with the call stack, as
 * the engine reports its own fatal conditions.
 *
 * A null pointer followed, an illegal instruction, a division by zero, or a
 * call to abort() ends a process with whatever the system says, which is
 * often nothing. After tb_crash_catch(), each is reported through
 * `badger/fatal.h`: to its handler if one is installed, and otherwise to
 * stderr, as a line that says what happened and where, and the stack of the
 * thread that crashed:
 *
 * @code
 * crash: access violation writing 0x0000000000000000, at 0x00007FF6A2C41F3A
 *     #0 load_level (burrow.exe+0x1f3a) level.c:88
 *     #1 main (burrow.exe+0x1108) main.c:40
 * @endcode
 *
 * Then the process ends, as after tb_fatal().
 *
 * **What a report can do.** A crashed process may have a damaged heap, and
 * the crashed thread may hold any lock. The report allocates nothing, takes
 * no lock of the C library's, and writes with the system's own call. Naming
 * the functions of the stack asks the system's debugging support, which on
 * Windows the engine serializes with a lock: a thread that crashed holding
 * it reports its stack without names. A fatal handler of the program's own
 * runs under the same conditions, and should do as little.
 *
 * **What is caught.**
 * - Windows: every exception that no handler of the program's handles, such
 *   as an access violation, and abort().
 * - Linux and macOS: SIGSEGV, SIGBUS, SIGILL, SIGFPE, and SIGABRT. The thread
 *   that calls tb_crash_catch() gets a stack of its own for the report, so
 *   that running out of stack is reported too; on other threads it is not.
 *
 * A process under a debugger stops at the crash, as it would without this.
 *
 * **abort() and the C library.** abort() is caught by a handler of SIGABRT,
 * which belongs to one copy of the C library. Where the engine is a dynamic
 * library with a C library of its own, as with MSVC's static runtime, the
 * program's calls of abort() reach the program's copy, so the program
 * installs the handler there too:
 *
 * @code
 * tb_crash_catch();
 * (void)signal(SIGABRT, tb_crash_report_abort);
 * @endcode
 *
 * Where the two share one C library the second line changes nothing.
 *
 * @par Threading
 * Stage-owned: call tb_crash_catch() once, during single-threaded startup.
 */
#ifndef TB_CRASH_H
#define TB_CRASH_H

#include "badger/api.h"

#include <stdbool.h>

/**
 * @brief Report every crash the system raises from now on, then end the
 * process. Calling it again changes nothing.
 * @par Errors A system that refuses the handler ends the process through
 * tb_fatal().
 * @par Threading Stage-owned: call during single-threaded startup, from the
 * main thread.
 */
TB_API void tb_crash_catch(void);

/**
 * @brief Report abort() as a crash, and end the process: a handler of
 * SIGABRT, for a program to install with signal() where its C library is
 * not the engine's. tb_crash_catch() installs it for the engine's own.
 * @param signal_number The signal, as signal() gives it.
 * @par Threading Thread-safe; it runs on the thread that called abort().
 */
TB_API void tb_crash_report_abort(int signal_number);

/**
 * @brief Also write a crash dump when a crash is reported, where the
 * platform has them: a file a debugger opens to show every thread of the
 * process, its stack, and the memory it pointed at, as they were.
 *
 * The dump is named after the program and the process, as in
 * `burrow-1234.dmp`, and the report's first line ends with where it is:
 *
 * @code
 * crash: access violation writing 0x0000000000000000, at 0x00007FF6A2C41F3A; dump:
 * C:\Temp\burrow-1234.dmp
 * @endcode
 *
 * Only crashes that tb_crash_catch() reports are dumped, and only the first
 * of them: the process ends after it.
 *
 * - **Windows:** a minidump (`MiniDumpWriteDump`), written by a thread that
 *   the engine starts here, since a thread cannot reliably dump itself.
 *   Visual Studio and WinDbg open it beside the program's symbols.
 * - **Linux and macOS:** no dump of the engine's: the system writes a core
 *   file where its settings allow one (`ulimit -c`, and on Linux
 *   `/proc/sys/kernel/core_pattern`). The function returns false.
 *
 * @param directory UTF-8 path of an existing directory to write dumps in,
 * or NULL for the system's temporary directory. Copied; the caller keeps
 * ownership.
 * @return true when crashes will be dumped there. false, changing nothing,
 * where the platform has no dumps of the engine's, or where `directory` is
 * not an existing directory, is not UTF-8, or makes too long a path.
 * @par Errors A system that refuses the thread that writes dumps ends the
 * process through tb_fatal().
 * @par Threading Stage-owned: call during single-threaded startup. Calling
 * it again changes where later dumps go.
 */
TB_API bool tb_crash_write_dumps(const char *directory);

#endif
