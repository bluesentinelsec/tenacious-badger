/* Private to the engine: checks, in builds with assertions, that an object
 * whose caller must serialize its use is used by one thread at a time.
 *
 * Every function that changes such an object enters its guard first and
 * leaves it last. A thread that enters while another thread is inside fails
 * an assertion that names the object's kind, which on Windows, where there is
 * no ThreadSanitizer, is the only report two such threads get.
 *
 * Functions that read the object's contents check it as readers, which leave
 * nothing in the object: readers run together. A reader fails when it begins
 * while another thread is changing the object, and when it ends after another
 * thread began a change, which every change records by counting itself in
 * the guard's generation. A reader that leaves a function through a trap
 * therefore leaves no trace. Accessors that read one field, such as a length
 * or a capacity, take no part; neither does a pointer the object hands out,
 * once the function that returned it has left.
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
#include "badger/exclusive.h"
#include "badger/platform.h"

#include <stddef.h>

/* The thread changing an object, how many times it has entered, and how many
 * changes have begun. The type is public so that public structures, such
 * as a list, can embed it. */
typedef tb_exclusive_check tb_exclusive;

/* Zero is a guard that no thread is inside. */
#define TB_EXCLUSIVE_INIT TB_EXCLUSIVE_CHECK_INIT

/* What a failed check reports: two threads changing an object, or one
 * reading it while another changes it. */
#define TB_EXCLUSIVE_WRITE_WRITE_MESSAGE "one thread at a time changes the object"
#define TB_EXCLUSIVE_READ_WRITE_MESSAGE "no thread reads the object while another changes it"

/* Enter the guard, or fail an assertion naming `kind` when another thread is
 * inside. Use TB_EXCLUSIVE_ENTER(), which gives the caller's location. */
void tb_exclusive_enter(tb_exclusive *guard, const char *kind, const char *file, int line,
                        const char *function);
/* Leave a guard the calling thread entered. */
void tb_exclusive_leave(tb_exclusive *guard);
/* Begin reading: fail an assertion naming `kind` when another thread is
 * changing the object, or return the generation to give to
 * tb_exclusive_read_leave(). Use TB_EXCLUSIVE_READ_ENTER(). */
size_t tb_exclusive_read_enter(const tb_exclusive *guard, const char *kind, const char *file,
                               int line, const char *function);
/* End reading: fail an assertion naming `kind` when another thread began a
 * change since `generation`. Use TB_EXCLUSIVE_READ_LEAVE(). */
void tb_exclusive_read_leave(const tb_exclusive *guard, size_t generation, const char *kind,
                             const char *file, int line, const char *function);

#if TB_ASSERT_ENABLED
#define TB_EXCLUSIVE_ENTER(guard, kind)                                                            \
    tb_exclusive_enter((guard), (kind), __FILE__, __LINE__, __func__)
#define TB_EXCLUSIVE_LEAVE(guard) tb_exclusive_leave(guard)
#define TB_EXCLUSIVE_READ_ENTER(guard, kind)                                                       \
    tb_exclusive_read_enter((guard), (kind), __FILE__, __LINE__, __func__)
#define TB_EXCLUSIVE_READ_LEAVE(guard, generation, kind)                                           \
    tb_exclusive_read_leave((guard), (generation), (kind), __FILE__, __LINE__, __func__)
#else
#define TB_EXCLUSIVE_ENTER(guard, kind) ((void)(guard), (void)(kind))
#define TB_EXCLUSIVE_LEAVE(guard) ((void)(guard))
#define TB_EXCLUSIVE_READ_ENTER(guard, kind) ((void)(guard), (void)(kind), (size_t)0)
#define TB_EXCLUSIVE_READ_LEAVE(guard, generation, kind)                                           \
    ((void)(guard), (void)(generation), (void)(kind))
#endif

#endif
