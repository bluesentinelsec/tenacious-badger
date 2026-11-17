#include "deque.h"

#include "badger/assert.h"

/* Every atomic operation here is sequentially consistent, which the platform
 * interface offers and nothing weaker. The algorithm needs that strength in
 * one place, marked below; elsewhere it is more than enough.
 *
 * The owner sees `bottom` exactly, because nobody else writes it, and may see
 * a `top` that is already too low, because thieves raise it. So the owner's
 * idea of how many items there are is never too small, and it can run out of
 * room or find the queue empty a moment early, never late. */

static tb_platform_atomic_pointer *tb_job_deque_slot(tb_job_deque *deque, size_t count)
{
    /* The capacity is a power of two, so this is the count modulo the
     * capacity, and stays in step when the count wraps around. */
    return &deque->slots[count & (deque->capacity - 1)];
}

static bool tb_job_deque_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

void tb_job_deque_init_at(tb_job_deque *deque, tb_platform_atomic_pointer *slots, size_t capacity,
                          size_t first_count)
{
    TB_ASSERT(deque != NULL);
    TB_ASSERT(slots != NULL);
    TB_ASSERT_MSG(tb_job_deque_is_power_of_two(capacity), "the capacity is a power of two");
    deque->slots = slots;
    deque->capacity = capacity;
    tb_platform_atomic_size_store(&deque->top, first_count);
    tb_platform_atomic_size_store(&deque->bottom, first_count);
}

void tb_job_deque_init(tb_job_deque *deque, tb_platform_atomic_pointer *slots, size_t capacity)
{
    tb_job_deque_init_at(deque, slots, capacity, 0);
}

bool tb_job_deque_push(tb_job_deque *deque, void *item)
{
    size_t bottom = 0;
    size_t top = 0;

    TB_ASSERT(deque != NULL);
    TB_ASSERT_MSG(item != NULL, "an item is not NULL, which means that there is none");
    bottom = tb_platform_atomic_size_load(&deque->bottom);
    top = tb_platform_atomic_size_load(&deque->top);
    if (bottom - top >= deque->capacity)
    {
        return false;
    }
    /* The slot is free: the items in use are those from `top` on, and `top`
     * has only risen since it was read. A thief that reads this slot after a
     * stale look at `top` fails to claim it and drops what it read. */
    tb_platform_atomic_pointer_store(tb_job_deque_slot(deque, bottom), item);
    /* Published last: a thief that sees the new `bottom` finds the item. */
    tb_platform_atomic_size_store(&deque->bottom, bottom + 1);
    return true;
}

void *tb_job_deque_pop(tb_job_deque *deque)
{
    size_t bottom = 0;
    size_t top = 0;
    void *item = NULL;

    TB_ASSERT(deque != NULL);
    /* Claim the newest item by lowering `bottom`, and only then look at
     * `top`. This write followed by that read is where the algorithm needs
     * sequential consistency: a thief does the same two steps on the other
     * end, reading `top` then `bottom`, and one of the two must see the
     * other, or both would take the last item. */
    bottom = tb_platform_atomic_size_load(&deque->bottom) - 1;
    tb_platform_atomic_size_store(&deque->bottom, bottom);
    top = tb_platform_atomic_size_load(&deque->top);
    if (top == bottom + 1)
    {
        /* It was empty: `bottom` is now one below `top`. Put it back. */
        tb_platform_atomic_size_store(&deque->bottom, bottom + 1);
        return NULL;
    }
    item = tb_platform_atomic_pointer_load(tb_job_deque_slot(deque, bottom));
    if (top != bottom)
    {
        /* More than one item was left, so no thief can reach this one: a
         * thief takes the item at `top`, and sees the lowered `bottom`. */
        return item;
    }
    /* The last item. A thief may be claiming it at this moment, by the same
     * step that the owner now takes: raising `top` past it. One succeeds. */
    if (!tb_platform_atomic_size_compare_exchange(&deque->top, &top, top + 1))
    {
        item = NULL;
    }
    /* Empty either way, with `bottom` level with the raised `top`. */
    tb_platform_atomic_size_store(&deque->bottom, bottom + 1);
    return item;
}

void *tb_job_deque_steal(tb_job_deque *deque)
{
    size_t top = 0;
    size_t bottom = 0;
    void *item = NULL;

    TB_ASSERT(deque != NULL);
    top = tb_platform_atomic_size_load(&deque->top);
    bottom = tb_platform_atomic_size_load(&deque->bottom);
    /* Nothing when the ends are level, and nothing when the owner has
     * stepped `bottom` one below `top` on its way out of an empty queue. */
    if (bottom == top || bottom + 1 == top)
    {
        return NULL;
    }
    /* Read before it is claimed: once `top` is raised, the owner may put a
     * new item in this slot. */
    item = tb_platform_atomic_pointer_load(tb_job_deque_slot(deque, top));
    if (!tb_platform_atomic_size_compare_exchange(&deque->top, &top, top + 1))
    {
        /* Another thief, or the owner taking the last item, raised `top`
         * first. What was read may be theirs, or may have come from a slot
         * that `top` had already left; either way it is dropped. */
        return NULL;
    }
    return item;
}

size_t tb_job_deque_count(tb_job_deque *deque)
{
    size_t top = 0;
    size_t bottom = 0;

    TB_ASSERT(deque != NULL);
    top = tb_platform_atomic_size_load(&deque->top);
    bottom = tb_platform_atomic_size_load(&deque->bottom);
    /* The owner is stepping out of an empty queue. */
    if (bottom + 1 == top)
    {
        return 0;
    }
    return bottom - top;
}
