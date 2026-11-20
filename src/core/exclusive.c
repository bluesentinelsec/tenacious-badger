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
        guard->depth = 1;
        return;
    }
    if (found == me)
    {
        guard->depth++;
        return;
    }
    tb_assert_fail("one thread at a time uses the object", kind, file, line, function);
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
