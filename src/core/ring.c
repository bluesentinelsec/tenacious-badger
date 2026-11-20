/* The elements live in a circular buffer. `head` is the storage position of
 * the front element, and the element at index i is `i` positions after it,
 * wrapping from the end of the storage to its start:
 *
 *   storage:  [ 3 | 4 | . | . | . | 0 | 1 | 2 ]      length 5, capacity 8
 *                                   ^ head
 *
 * Adding at the front moves the head back, and removing there moves it on, so
 * neither end ever shifts the elements between them. */
#include "badger/ring.h"

#include "badger/assert.h"
#include "badger/size.h"
#include "exclusive.h"
#include "memory_internal.h"

#include <string.h>

/* The first storage a ring allocates holds this many elements. */
#define TB_RING_FIRST_CAPACITY ((size_t)8)

struct tb_ring
{
    tb_allocator allocator;   /* Supplies the structure and the storage. */
    unsigned char *data;      /* The storage; NULL while the capacity is 0. */
    size_t capacity;          /* Elements the storage holds. */
    size_t length;            /* Elements in use. */
    size_t head;              /* Storage position of the front element. */
    size_t element_size;      /* Bytes per element; at least 1. */
    size_t element_alignment; /* Alignment of the storage. */
    tb_exclusive exclusive;   /* One thread at a time changes it. */
};

static bool tb_ring_is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* ---------------------------------------------------------------------------
 * Positions.
 * ------------------------------------------------------------------------- */

/* The storage position `distance` places after `position`, wrapping around.
 * Both are at most the capacity, and the sum is never formed, so it cannot
 * overflow. */
static size_t tb_ring_position_after(const tb_ring *ring, size_t position, size_t distance)
{
    const size_t places_to_the_end = ring->capacity - position;

    if (distance >= places_to_the_end)
    {
        return distance - places_to_the_end;
    }
    return position + distance;
}

/* Address of the element at an index, which may be the length when the ring
 * has room for one more. */
static unsigned char *tb_ring_slot(const tb_ring *ring, size_t index)
{
    return ring->data + tb_ring_position_after(ring, ring->head, index) * ring->element_size;
}

/* Moves the head on by one: index 1 becomes index 0. */
static void tb_ring_advance_head(tb_ring *ring)
{
    ring->head = tb_ring_position_after(ring, ring->head, 1);
}

/* Moves the head back by one: the position before the front becomes index 0. */
static void tb_ring_retreat_head(tb_ring *ring)
{
    ring->head = tb_ring_position_after(ring, ring->head, ring->capacity - 1);
}

/* Copies the element at one index over the element at another. */
static void tb_ring_move_element(tb_ring *ring, size_t to, size_t from)
{
    memcpy(tb_ring_slot(ring, to), tb_ring_slot(ring, from), ring->element_size);
}

/* Moves the `count` elements that start at index `from` so that they start
 * at index `to`. The two runs may overlap. Each run is at most two stretches
 * of storage, so the move is a few block copies whatever the count. */
static void tb_ring_move_run(tb_ring *ring, size_t to, size_t from, size_t count)
{
    const size_t size = ring->element_size;

    if (to < from)
    {
        /* Toward the front: first elements first, so that none is
         * overwritten before it has moved. */
        while (count > 0)
        {
            const size_t from_position = tb_ring_position_after(ring, ring->head, from);
            const size_t to_position = tb_ring_position_after(ring, ring->head, to);
            size_t stretch = count;

            if (stretch > ring->capacity - from_position)
            {
                stretch = ring->capacity - from_position;
            }
            if (stretch > ring->capacity - to_position)
            {
                stretch = ring->capacity - to_position;
            }
            memmove(ring->data + to_position * size, ring->data + from_position * size,
                    stretch * size);
            from += stretch;
            to += stretch;
            count -= stretch;
        }
        return;
    }
    /* Toward the back: last elements first. */
    while (count > 0)
    {
        /* One past the storage position of the last element of each run. */
        const size_t from_end = tb_ring_position_after(ring, ring->head, from + count - 1) + 1;
        const size_t to_end = tb_ring_position_after(ring, ring->head, to + count - 1) + 1;
        size_t stretch = count;

        if (stretch > from_end)
        {
            stretch = from_end;
        }
        if (stretch > to_end)
        {
            stretch = to_end;
        }
        memmove(ring->data + (to_end - stretch) * size, ring->data + (from_end - stretch) * size,
                stretch * size);
        count -= stretch;
    }
}

/* Copies an element into a slot, or zero-fills the slot when there is none. */
static void tb_ring_fill_slot(const tb_ring *ring, unsigned char *slot, const void *element)
{
    if (element == NULL)
    {
        memset(slot, 0, ring->element_size);
        return;
    }
    memcpy(slot, element, ring->element_size);
}

/* Copies an element out of a slot when the caller wants it. */
static void tb_ring_read_slot(const tb_ring *ring, const unsigned char *slot, void *element)
{
    if (element != NULL)
    {
        memcpy(element, slot, ring->element_size);
    }
}

/* ---------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */

/* Moves the elements to new storage of exactly `capacity` elements, which is
 * at least the length and more than zero, with the front at its start.
 * Returns false, changing nothing, when the size overflows or the allocator
 * cannot supply the storage. */
static bool tb_ring_set_capacity(tb_ring *ring, size_t capacity)
{
    unsigned char *data = NULL;
    size_t size = {0};
    /* Elements from the head to the end of the old storage; the rest wrap. */
    size_t unwrapped_count = ring->capacity - ring->head;

    if (!tb_size_multiply(capacity, ring->element_size, &size))
    {
        return false;
    }
    data = tb_allocator_try_alloc(&ring->allocator, size, ring->element_alignment);
    if (data == NULL)
    {
        return false;
    }
    if (unwrapped_count > ring->length)
    {
        unwrapped_count = ring->length;
    }
    if (ring->length > 0)
    {
        memcpy(data, ring->data + ring->head * ring->element_size,
               unwrapped_count * ring->element_size);
        memcpy(data + unwrapped_count * ring->element_size, ring->data,
               (ring->length - unwrapped_count) * ring->element_size);
    }
    tb_allocator_free(&ring->allocator, ring->data, ring->capacity * ring->element_size,
                      ring->element_alignment);
    ring->data = data;
    ring->capacity = capacity;
    ring->head = 0;
    return true;
}

/* What each function that changes the object does once inside its guard. */
static bool tb_ring_try_reserve_inside(tb_ring *ring, size_t capacity);
static void tb_ring_reserve_inside(tb_ring *ring, size_t capacity);
static void tb_ring_shrink_to_fit_inside(tb_ring *ring);
static void tb_ring_clear_inside(tb_ring *ring);
static void *tb_ring_push_back_inside(tb_ring *ring, const void *element);
static void *tb_ring_push_front_inside(tb_ring *ring, const void *element);
static void tb_ring_pop_back_inside(tb_ring *ring, void *element);
static void tb_ring_pop_front_inside(tb_ring *ring, void *element);
static void tb_ring_put_inside(tb_ring *ring, size_t index, const void *element);
static void *tb_ring_insert_inside(tb_ring *ring, size_t index, const void *element);
static void tb_ring_remove_inside(tb_ring *ring, size_t index, void *element);
static void tb_ring_rotate_left_inside(tb_ring *ring, size_t count);
static void tb_ring_rotate_right_inside(tb_ring *ring, size_t count);
static void *tb_ring_push_back_sized_inside(tb_ring *ring, size_t element_size);
static void *tb_ring_push_front_sized_inside(tb_ring *ring, size_t element_size);

static bool tb_ring_try_reserve_inside(tb_ring *ring, size_t capacity)
{
    TB_ASSERT(ring != NULL);
    if (capacity <= ring->capacity)
    {
        return true;
    }
    return tb_ring_set_capacity(ring, capacity);
}

bool tb_ring_try_reserve(tb_ring *ring, size_t capacity)
{
    bool result = false;
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    result = tb_ring_try_reserve_inside(ring, capacity);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
    return result;
}

static void tb_ring_reserve_inside(tb_ring *ring, size_t capacity)
{
    TB_ASSERT(ring != NULL);
    if (!tb_ring_try_reserve_inside(ring, capacity))
    {
        tb_memory_fail(capacity, ring->element_size, ring->element_alignment);
    }
}

void tb_ring_reserve(tb_ring *ring, size_t capacity)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_reserve_inside(ring, capacity);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

/* Makes room for one more element, growing by doubling. */
static void tb_ring_make_room(tb_ring *ring)
{
    size_t required = {0};
    size_t capacity = {0};

    if (ring->length < ring->capacity)
    {
        return;
    }
    if (!tb_size_add(ring->length, 1, &required))
    {
        tb_memory_fail(1, ring->element_size, ring->element_alignment);
    }
    if (!tb_size_grow(ring->capacity, required, ring->element_size, &capacity))
    {
        tb_memory_fail(required, ring->element_size, ring->element_alignment);
    }
    if (capacity < TB_RING_FIRST_CAPACITY)
    {
        capacity = TB_RING_FIRST_CAPACITY;
    }
    /* The doubled capacity is a preference. When it cannot be had, the ring
     * settles for exactly what it needs. */
    if (tb_ring_set_capacity(ring, capacity))
    {
        return;
    }
    tb_ring_reserve(ring, required);
}

static void tb_ring_shrink_to_fit_inside(tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    if (ring->length == ring->capacity)
    {
        return;
    }
    if (ring->length == 0)
    {
        tb_allocator_free(&ring->allocator, ring->data, ring->capacity * ring->element_size,
                          ring->element_alignment);
        ring->data = NULL;
        ring->capacity = 0;
        ring->head = 0;
        return;
    }
    (void)tb_ring_set_capacity(ring, ring->length);
}

void tb_ring_shrink_to_fit(tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_shrink_to_fit_inside(ring);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

static void tb_ring_clear_inside(tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    ring->length = 0;
    ring->head = 0;
}

void tb_ring_clear(tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_clear_inside(ring);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

/* ---------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

tb_ring *tb_ring_create(const tb_allocator *allocator, size_t element_size,
                        size_t element_alignment)
{
    tb_ring *ring = NULL;

    TB_ASSERT_MSG(element_size > 0, "an element has at least one byte");
    TB_ASSERT_MSG(tb_ring_is_power_of_two(element_alignment),
                  "element alignment must be a power of two");
    TB_ASSERT_MSG(element_size % element_alignment == 0,
                  "element size must be a multiple of the element alignment");
    ring = TB_ALLOCATOR_NEW(allocator, tb_ring);
    ring->allocator = tb_allocator_resolve(allocator);
    ring->element_size = element_size;
    ring->element_alignment = element_alignment;
    return ring;
}

void tb_ring_destroy(tb_ring *ring)
{
    tb_allocator allocator = {0};

    if (ring == NULL)
    {
        return;
    }
    /* No thread may be inside a ring that is going away. */
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    allocator = ring->allocator;
    tb_allocator_free(&allocator, ring->data, ring->capacity * ring->element_size,
                      ring->element_alignment);
    TB_ALLOCATOR_DELETE(&allocator, tb_ring, ring);
}

/* ---------------------------------------------------------------------------
 * Size.
 * ------------------------------------------------------------------------- */

size_t tb_ring_length(const tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    return ring->length;
}

size_t tb_ring_capacity(const tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    return ring->capacity;
}

size_t tb_ring_element_size(const tb_ring *ring)
{
    TB_ASSERT(ring != NULL);
    return ring->element_size;
}

/* ---------------------------------------------------------------------------
 * The two ends.
 * ------------------------------------------------------------------------- */

/* Adds an uninitialized element at the back and returns its slot. */
static unsigned char *tb_ring_add_back(tb_ring *ring)
{
    tb_ring_make_room(ring);
    ring->length++;
    return tb_ring_slot(ring, ring->length - 1);
}

/* Adds an uninitialized element at the front and returns its slot. */
static unsigned char *tb_ring_add_front(tb_ring *ring)
{
    tb_ring_make_room(ring);
    tb_ring_retreat_head(ring);
    ring->length++;
    return tb_ring_slot(ring, 0);
}

static void *tb_ring_push_back_inside(tb_ring *ring, const void *element)
{
    unsigned char *slot = NULL;

    TB_ASSERT(ring != NULL);
    slot = tb_ring_add_back(ring);
    tb_ring_fill_slot(ring, slot, element);
    return slot;
}

void *tb_ring_push_back(tb_ring *ring, const void *element)
{
    void *result = NULL;
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    result = tb_ring_push_back_inside(ring, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
    return result;
}

static void *tb_ring_push_front_inside(tb_ring *ring, const void *element)
{
    unsigned char *slot = NULL;

    TB_ASSERT(ring != NULL);
    slot = tb_ring_add_front(ring);
    tb_ring_fill_slot(ring, slot, element);
    return slot;
}

void *tb_ring_push_front(tb_ring *ring, const void *element)
{
    void *result = NULL;
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    result = tb_ring_push_front_inside(ring, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
    return result;
}

static void tb_ring_pop_back_inside(tb_ring *ring, void *element)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(ring->length > 0, "the ring must not be empty");
    tb_ring_read_slot(ring, tb_ring_slot(ring, ring->length - 1), element);
    ring->length--;
}

void tb_ring_pop_back(tb_ring *ring, void *element)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_pop_back_inside(ring, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

static void tb_ring_pop_front_inside(tb_ring *ring, void *element)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(ring->length > 0, "the ring must not be empty");
    tb_ring_read_slot(ring, tb_ring_slot(ring, 0), element);
    tb_ring_advance_head(ring);
    ring->length--;
}

void tb_ring_pop_front(tb_ring *ring, void *element)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_pop_front_inside(ring, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

/* ---------------------------------------------------------------------------
 * Elements by index.
 * ------------------------------------------------------------------------- */

void *tb_ring_at(const tb_ring *ring, size_t index)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(index < ring->length, "index must be less than the length");
    return tb_ring_slot(ring, index);
}

void tb_ring_get(const tb_ring *ring, size_t index, void *element)
{
    TB_ASSERT(element != NULL);
    memcpy(element, tb_ring_at(ring, index), ring->element_size);
}

static void tb_ring_put_inside(tb_ring *ring, size_t index, const void *element)
{
    TB_ASSERT(element != NULL);
    memcpy(tb_ring_at(ring, index), element, ring->element_size);
}

void tb_ring_put(tb_ring *ring, size_t index, const void *element)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_put_inside(ring, index, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

static void *tb_ring_insert_inside(tb_ring *ring, size_t index, const void *element)
{
    unsigned char *slot = NULL;

    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(index <= ring->length, "index must be at most the length");
    if (index < ring->length - index)
    {
        /* Nearer the front: open a slot there and move the elements before
         * the index down into it. */
        (void)tb_ring_add_front(ring);
        tb_ring_move_run(ring, 0, 1, index);
    }
    else
    {
        /* Nearer the back: open a slot there and move the elements from the
         * index on up into it. */
        (void)tb_ring_add_back(ring);
        tb_ring_move_run(ring, index + 1, index, ring->length - 1 - index);
    }
    slot = tb_ring_slot(ring, index);
    tb_ring_fill_slot(ring, slot, element);
    return slot;
}

void *tb_ring_insert(tb_ring *ring, size_t index, const void *element)
{
    void *result = NULL;
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    result = tb_ring_insert_inside(ring, index, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
    return result;
}

static void tb_ring_remove_inside(tb_ring *ring, size_t index, void *element)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(index < ring->length, "index must be less than the length");
    tb_ring_read_slot(ring, tb_ring_slot(ring, index), element);
    if (index < ring->length - 1 - index)
    {
        /* Nearer the front: move the elements before the index up over it,
         * then drop the front slot. */
        tb_ring_move_run(ring, 1, 0, index);
        tb_ring_advance_head(ring);
    }
    else
    {
        /* Nearer the back: move the elements after the index down over it. */
        tb_ring_move_run(ring, index, index + 1, ring->length - 1 - index);
    }
    ring->length--;
}

void tb_ring_remove(tb_ring *ring, size_t index, void *element)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_remove_inside(ring, index, element);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

/* ---------------------------------------------------------------------------
 * Rotating.
 * ------------------------------------------------------------------------- */

static void tb_ring_rotate_left_inside(tb_ring *ring, size_t count)
{
    size_t steps_left = {0};
    size_t steps_right = {0};

    TB_ASSERT(ring != NULL);
    if (ring->length == 0)
    {
        return;
    }
    steps_left = count % ring->length;
    steps_right = ring->length - steps_left;
    if (steps_left == 0)
    {
        return;
    }
    /* A full ring has no gap between its back and its front, so rotating it
     * is only a matter of where the front is. */
    if (ring->length == ring->capacity)
    {
        ring->head = tb_ring_position_after(ring, ring->head, steps_left);
        return;
    }
    /* Otherwise elements cross the gap one at a time, from whichever end
     * needs fewer of them to move. The slot after the back is free. */
    if (steps_left <= steps_right)
    {
        for (size_t step = 0; step < steps_left; step++)
        {
            tb_ring_move_element(ring, ring->length, 0);
            tb_ring_advance_head(ring);
        }
        return;
    }
    for (size_t step = 0; step < steps_right; step++)
    {
        tb_ring_retreat_head(ring);
        tb_ring_move_element(ring, 0, ring->length);
    }
}

void tb_ring_rotate_left(tb_ring *ring, size_t count)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_rotate_left_inside(ring, count);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

static void tb_ring_rotate_right_inside(tb_ring *ring, size_t count)
{
    TB_ASSERT(ring != NULL);
    if (ring->length == 0)
    {
        return;
    }
    tb_ring_rotate_left_inside(ring, ring->length - count % ring->length);
}

void tb_ring_rotate_right(tb_ring *ring, size_t count)
{
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    tb_ring_rotate_right_inside(ring, count);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
}

/* ---------------------------------------------------------------------------
 * Typed access.
 * ------------------------------------------------------------------------- */

void *tb_ring_at_sized(const tb_ring *ring, size_t index, size_t element_size)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(element_size == ring->element_size, "the ring holds elements of another size");
    return tb_ring_at(ring, index);
}

static void *tb_ring_push_back_sized_inside(tb_ring *ring, size_t element_size)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(element_size == ring->element_size, "the ring holds elements of another size");
    return tb_ring_add_back(ring);
}

void *tb_ring_push_back_sized(tb_ring *ring, size_t element_size)
{
    void *result = NULL;
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    result = tb_ring_push_back_sized_inside(ring, element_size);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
    return result;
}

static void *tb_ring_push_front_sized_inside(tb_ring *ring, size_t element_size)
{
    TB_ASSERT(ring != NULL);
    TB_ASSERT_MSG(element_size == ring->element_size, "the ring holds elements of another size");
    return tb_ring_add_front(ring);
}

void *tb_ring_push_front_sized(tb_ring *ring, size_t element_size)
{
    void *result = NULL;
    TB_ASSERT(ring != NULL);
    TB_EXCLUSIVE_ENTER(&ring->exclusive, "a ring buffer");
    result = tb_ring_push_front_sized_inside(ring, element_size);
    TB_EXCLUSIVE_LEAVE(&ring->exclusive);
    return result;
}
