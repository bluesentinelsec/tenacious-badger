/* Private to the Windows adapter: what else uses the debugging library,
 * DbgHelp, that stack.c loads and serializes. */
#ifndef TB_WIN32_DEBUG_LIBRARY_H
#define TB_WIN32_DEBUG_LIBRARY_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdbool.h>

/* Loads the library, if it has not been, and reports whether it can write a
 * dump. Called before a crash, where loading is safe. */
bool tb_win32_debug_can_write_dump(void);

/* Writes a minidump of the process to an open file: the threads, their
 * stacks, and the memory they point at. `pointers` describes the exception
 * that thread `thread_id` raised, or is NULL where there was none, as for
 * abort(). Called from a thread other than the one that crashed, since a
 * thread cannot dump itself reliably. Returns whether the dump was written.
 *
 * A crashed thread may hold the library's lock, and waits while the dump is
 * written, so the lock is waited for only for a moment, and the dump is
 * written without it after that. */
bool tb_win32_debug_write_dump(HANDLE file, DWORD thread_id, EXCEPTION_POINTERS *pointers);

#endif
