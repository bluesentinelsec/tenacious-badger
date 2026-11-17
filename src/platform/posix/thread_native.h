/* Private to the Linux and macOS adapters. POSIX threads are the same on
 * both, but naming a thread, giving it a priority, and reading its name back
 * are not part of POSIX, and each system does them its own way.
 * src/platform/posix/thread.c is shared, and each system supplies these
 * functions in its own thread_native.c. All of them act on the calling
 * thread. */
#ifndef TB_POSIX_THREAD_NATIVE_H
#define TB_POSIX_THREAD_NATIVE_H

#include "badger/thread.h"

#include <stddef.h>

/* Gives the calling thread a name, which is UTF-8, NUL-terminated, and not
 * empty. A system that keeps fewer bytes keeps the first whole characters.
 * A system that refuses leaves the thread unnamed; it is never an error. */
void tb_posix_thread_apply_name(const char *name);

/* Moves the calling thread to a priority other than the normal one, as near
 * to it as the system lets this process go. It is never an error. */
void tb_posix_thread_apply_priority(tb_thread_priority priority);

/* Writes the calling thread's name as the system has it into `buffer`, of
 * `capacity` bytes, at least one, and terminates it. Returns the bytes
 * written, without the terminator; zero when the system has no name. */
size_t tb_posix_thread_read_name(char *buffer, size_t capacity);

#endif
