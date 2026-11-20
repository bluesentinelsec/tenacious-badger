#include "exclusive.h"

#include <stdint.h>

/* Its address names the calling thread: no two running threads share it. */
static _Thread_local char tb_exclusive_token = 0;

static size_t tb_exclusive_me(void)
{
    return (size_t)(uintptr_t)&tb_exclusive_token;
}

void tb_exclusive_enter(tb_exclusive *guard, const char *kind, const char *file, int line,
                        const char *function)
{
    const size_t me = tb_exclusive_me();
    size_t found = 0;
    TB_ASSERT(guard != NULL);
    if (tb_platform_atomic_size_compare_exchange(&guard->owner, &found, me))
    {
        /* A change begins: a reader that overlaps it sees the count move. */
        (void)tb_platform_atomic_size_add(&guard->generation, 1);
        guard->depth = 1;
        return;
    }
    if (found == me)
    {
        guard->depth++;
        return;
    }
    tb_assert_fail(TB_EXCLUSIVE_WRITE_WRITE_MESSAGE, kind, file, line, function);
}

void tb_exclusive_leave(tb_exclusive *guard)
{
    TB_ASSERT(guard != NULL);
    TB_ASSERT_MSG(tb_platform_atomic_size_load(&guard->owner) == tb_exclusive_me() &&
                      guard->depth > 0,
                  "only the thread inside an object leaves it");
    guard->depth--;
    if (guard->depth == 0)
    {
        tb_platform_atomic_size_store(&guard->owner, 0);
    }
}

/* The guard is changed through a pointer to a const object: the objects that
 * hold one are always made by the engine, never defined const. */
static tb_exclusive *tb_exclusive_writable(const tb_exclusive *guard)
{
    TB_ASSERT(guard != NULL);
    return (tb_exclusive *)(uintptr_t)guard;
}

size_t tb_exclusive_read_enter(const tb_exclusive *guard, const char *kind, const char *file,
                               int line, const char *function)
{
    tb_exclusive *const writable = tb_exclusive_writable(guard);
    /* The generation is taken first: a change that begins after it moves it,
     * and one that began before it still holds the owner. The thread that is
     * changing the object may read it too. */
    const size_t generation = tb_platform_atomic_size_load(&writable->generation);
    const size_t owner = tb_platform_atomic_size_load(&writable->owner);
    if (owner != 0 && owner != tb_exclusive_me())
    {
        tb_assert_fail(TB_EXCLUSIVE_READ_WRITE_MESSAGE, kind, file, line, function);
    }
    return generation;
}

void tb_exclusive_read_leave(const tb_exclusive *guard, size_t generation, const char *kind,
                             const char *file, int line, const char *function)
{
    tb_exclusive *const writable = tb_exclusive_writable(guard);
    if (tb_platform_atomic_size_load(&writable->generation) != generation)
    {
        tb_assert_fail(TB_EXCLUSIVE_READ_WRITE_MESSAGE, kind, file, line, function);
    }
}
