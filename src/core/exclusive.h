/* Private to the engine: checks, in builds with assertions, that an object
 * whose caller must serialize its use is used by one thread at a time.
 *
 * Every function that changes such an object enters its guard first and
 * leaves it last. A thread that enters while another thread is inside fails
 * an assertion that names the object's kind, which on Windows, where there is
 * no ThreadSanitizer, is the only report two such threads get. Functions that
 * only read take no part, so readers still run together.
 *
 * A thread may enter a guard it is already inside: a function that changes
 * an array may call another that does. That also covers a test that leaves
 * a function through a trap: the guard stays with the test's own thread, so
 * the object can still be used, and destroyed, by that thread.
 *
 * In builds without assertions the guard does nothing, and costs nothing but
 * its room in the object, which every build keeps so that the engine's own
 * translation units agree on the object's layout. */
#ifndef TB_CORE_EXCLUSIVE_H
#define TB_CORE_EXCLUSIVE_H

#include "badger/assert.h"
#include "badger/platform.h"

#include <stddef.h>

/* The thread inside an object, and how many times it has entered. */
typedef struct tb_exclusive
{
    tb_platform_atomic_size owner; /* The thread's token, or 0 when none is inside. */
    size_t depth;                  /* Written only by the owner. */
} tb_exclusive;

/* Zero is a guard that no thread is inside. */
#define TB_EXCLUSIVE_INIT {{0}, 0}

/* Enter the guard, or fail an assertion naming `kind` when another thread is
 * inside. Use TB_EXCLUSIVE_ENTER(), which gives the caller's location. */
void tb_exclusive_enter(tb_exclusive *guard, const char *kind, const char *file, int line,
                        const char *function);
/* Leave a guard the calling thread entered. */
void tb_exclusive_leave(tb_exclusive *guard);

#if TB_ASSERT_ENABLED
#define TB_EXCLUSIVE_ENTER(guard, kind)                                                            \
    tb_exclusive_enter((guard), (kind), __FILE__, __LINE__, __func__)
#define TB_EXCLUSIVE_LEAVE(guard) tb_exclusive_leave(guard)
#else
#define TB_EXCLUSIVE_ENTER(guard, kind) ((void)(guard), (void)(kind))
#define TB_EXCLUSIVE_LEAVE(guard) ((void)(guard))
#endif

#endif
