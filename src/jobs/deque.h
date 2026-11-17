/* Private to the jobs module: the queue a worker thread keeps its work in.
 *
 * One thread owns the queue. It adds items at the bottom and takes them from
 * the bottom, newest first, and touches nothing that another thread writes
 * unless the queue is nearly empty. Any other thread may steal from the top,
 * oldest first. Nothing locks: a worker pays for other threads only when one
 * of them is out of work and comes to steal.
 *
 * This is the deque of Chase and Lev ("Dynamic Circular Work-Stealing
 * Deque", 2005), with a fixed capacity.
 *
 *     top                      bottom
 *      v                         v
 *      [ oldest | ... | newest ]
 *      ^ thieves steal here      ^ the owner pushes and pops here
 *
 * `top` and `bottom` count items since the queue was made, and an item's
 * place in the storage is its count modulo the capacity. All arithmetic on
 * them is unsigned and may wrap around.
 *
 * Threading: tb_job_deque_push() and tb_job_deque_pop() are for the owning
 * thread alone. tb_job_deque_steal() and tb_job_deque_count() are
 * thread-safe. */
#ifndef TB_JOB_DEQUE_H
#define TB_JOB_DEQUE_H

#include "badger/platform.h"

#include <stdbool.h>
#include <stddef.h>

/* Zero-initialize it, then give it storage with tb_job_deque_init(). */
typedef struct tb_job_deque
{
    /* Count of the oldest item. It only rises: a thief raises it to take
     * that item, and so does the owner to take the last one. */
    tb_platform_atomic_size top;
    /* Count one past the newest item. Only the owner writes it. */
    tb_platform_atomic_size bottom;
    /* The storage, which the caller owns. */
    tb_platform_atomic_pointer *slots;
    /* Slots in the storage; a power of two. */
    size_t capacity;
} tb_job_deque;

/* Makes an empty queue over `slots`, which the caller owns and keeps alive,
 * unmoved, for as long as the queue is used. `capacity` is the number of
 * slots, a power of two and at least 1. Both pointers are required non-null.
 * Call it before the queue is shared with any thread. */
void tb_job_deque_init(tb_job_deque *deque, tb_platform_atomic_pointer *slots, size_t capacity);

/* As tb_job_deque_init(), with the counts of the two ends starting at
 * `first_count` in place of zero. Any count is as good as another; this is
 * for tests, which start near the largest count to see the arithmetic wrap. */
void tb_job_deque_init_at(tb_job_deque *deque, tb_platform_atomic_pointer *slots, size_t capacity,
                          size_t first_count);

/* Adds `item`, which is not NULL, as the newest. Returns false when the
 * queue is full, in which case nothing changed. For the owning thread. */
bool tb_job_deque_push(tb_job_deque *deque, void *item);

/* Takes the newest item, or returns NULL when there is none, which includes
 * a last item that a thief took first. For the owning thread. */
void *tb_job_deque_pop(tb_job_deque *deque);

/* Takes the oldest item, or returns NULL when the queue was seen empty. Each
 * item is returned once, to the owner or to one thief. Thread-safe. */
void *tb_job_deque_steal(tb_job_deque *deque);

/* How many items the queue holds. From any thread but the owner it is a
 * moment's reading that may already be wrong; it never exceeds the capacity.
 * Thread-safe. */
size_t tb_job_deque_count(tb_job_deque *deque);

#endif
