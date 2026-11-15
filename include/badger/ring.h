/** @file ring.h
 * @brief A ring: a sequence that grows and shrinks at both ends.
 *
 * A ring is the collection for queues: events waiting to be handled, commands
 * waiting to run, the last N samples of a measurement. Adding and removing at
 * either end takes constant time and moves nothing, because the elements live
 * in a circular buffer whose start travels around it.
 *
 * @code
 * tb_ring *const pending = TB_RING_CREATE(NULL, event);
 *
 * TB_RING_PUSH_BACK(pending, event, incoming);
 * while (tb_ring_length(pending) > 0)
 * {
 *     event next = {0};
 *
 *     tb_ring_pop_front(pending, &next);
 *     handle(&next);
 * }
 * @endcode
 *
 * **Elements are numbered from the front.** Index 0 is the front element and
 * `tb_ring_length() - 1` is the back one, wherever they are in the buffer.
 *
 * **The elements are not contiguous.** They are stored by value, but the
 * sequence may wrap from the end of the buffer to its start, so there is no
 * pointer to all of them. Use the indexes, or a dynamic array (array.h) when
 * the elements must be one block.
 *
 * **Pointers into the ring are short-lived.** A pointer to an element is valid
 * until the next call that adds, removes, or rotates elements, or destroys the
 * ring.
 *
 * **Elements are plain bytes**, copied with `memcpy`; the ring never runs code
 * for them.
 *
 * **Memory.** A ring gets all of its memory from the allocator it was created
 * with (allocator.h). Growing fails fast; tb_ring_try_reserve() is the one
 * fallible form.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same ring. A ring is not a queue between threads.
 */
#ifndef TB_RING_H
#define TB_RING_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief A ring. Created by tb_ring_create(); owned by the caller; released
 * by tb_ring_destroy().
 */
typedef struct tb_ring tb_ring;

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an empty ring.
 * @param allocator Where the ring gets its memory, or NULL for the engine's
 * heap. It is copied.
 * @param element_size Bytes per element; at least 1 and a multiple of
 * `element_alignment`, as `sizeof` is for every type.
 * @param element_alignment Alignment of an element in bytes; a power of two.
 * @return The ring; never NULL. It holds no elements and no storage yet.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_ring *tb_ring_create(const tb_allocator *allocator, size_t element_size,
                               size_t element_alignment);

/**
 * @brief Create an empty ring of a type: `TB_RING_CREATE(NULL, event)`.
 * @param allocator Where the ring gets its memory, or NULL for the heap.
 * @param type Element type.
 * @return The ring; never NULL.
 */
#define TB_RING_CREATE(allocator, type) tb_ring_create((allocator), sizeof(type), _Alignof(type))

/**
 * @brief Destroy a ring and release its storage.
 * @param ring Ring to destroy; NULL is ignored.
 */
TB_API void tb_ring_destroy(tb_ring *ring);

/* -------------------------------------------------------------------------
 * Size.
 * ------------------------------------------------------------------------- */

/** @brief Number of elements in the ring. @param ring Required non-null. */
TB_API size_t tb_ring_length(const tb_ring *ring);

/**
 * @brief Number of elements the ring can hold before it must grow.
 * @param ring Required non-null.
 */
TB_API size_t tb_ring_capacity(const tb_ring *ring);

/** @brief Bytes per element, as given at creation. @param ring Required non-null. */
TB_API size_t tb_ring_element_size(const tb_ring *ring);

/**
 * @brief Make room for at least `capacity` elements without changing the length.
 *
 * A queue that reserves its largest expected length never allocates while it
 * runs.
 *
 * @param ring Required non-null.
 * @param capacity Elements to make room for. A capacity the ring already has
 * changes nothing.
 * @par Errors Exhaustion, or a capacity whose size in bytes overflows
 * `size_t`, calls the memory failure handler and ends the process.
 */
TB_API void tb_ring_reserve(tb_ring *ring, size_t capacity);

/**
 * @brief Make room for at least `capacity` elements, or report that it cannot
 * be done.
 * @param ring Required non-null.
 * @param capacity Elements to make room for.
 * @return true when the ring has the capacity. false, leaving the ring
 * unchanged, when the allocator cannot supply the memory or the size in bytes
 * overflows `size_t`.
 */
TB_API bool tb_ring_try_reserve(tb_ring *ring, size_t capacity);

/**
 * @brief Give back the storage the ring is not using.
 *
 * The capacity becomes the length. When the allocator cannot supply the
 * smaller block the ring is left as it was.
 *
 * @param ring Required non-null.
 */
TB_API void tb_ring_shrink_to_fit(tb_ring *ring);

/**
 * @brief Remove every element and keep the storage for reuse.
 * @param ring Required non-null.
 */
TB_API void tb_ring_clear(tb_ring *ring);

/* -------------------------------------------------------------------------
 * The two ends.
 * ------------------------------------------------------------------------- */

/**
 * @brief Add an element at the back, after the last one.
 * @param ring Required non-null.
 * @param element Element to copy in, or NULL for a zero-filled element. It
 * must not point into the ring.
 * @return Pointer to the new element inside the ring.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_ring_push_back(tb_ring *ring, const void *element);

/**
 * @brief Add an element at the front, before the first one.
 *
 * The new element becomes index 0 and every other index rises by one. Nothing
 * is moved.
 *
 * @param ring Required non-null.
 * @param element Element to copy in, or NULL for a zero-filled element. It
 * must not point into the ring.
 * @return Pointer to the new element inside the ring.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_ring_push_front(tb_ring *ring, const void *element);

/**
 * @brief Remove the back element.
 * @param ring Required non-null and not empty.
 * @param[out] element Receives the removed element, or NULL to discard it.
 */
TB_API void tb_ring_pop_back(tb_ring *ring, void *element);

/**
 * @brief Remove the front element. Every other index falls by one.
 * @param ring Required non-null and not empty.
 * @param[out] element Receives the removed element, or NULL to discard it.
 */
TB_API void tb_ring_pop_front(tb_ring *ring, void *element);

/* -------------------------------------------------------------------------
 * Elements by index.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get a pointer to an element.
 * @param ring Required non-null.
 * @param index Index of the element, counted from the front; less than the
 * length.
 * @return Pointer to the element inside the ring, valid until the next call
 * that adds, removes, or rotates elements.
 */
TB_API void *tb_ring_at(const tb_ring *ring, size_t index);

/**
 * @brief Copy an element out of the ring.
 * @param ring Required non-null.
 * @param index Index of the element; less than the length.
 * @param[out] element Required non-null; receives tb_ring_element_size() bytes.
 */
TB_API void tb_ring_get(const tb_ring *ring, size_t index, void *element);

/**
 * @brief Overwrite an element.
 * @param ring Required non-null.
 * @param index Index of the element; less than the length.
 * @param element Required non-null; tb_ring_element_size() bytes are copied
 * from it. It must not point into the ring.
 */
TB_API void tb_ring_put(tb_ring *ring, size_t index, const void *element);

/**
 * @brief Add an element at an index.
 *
 * The elements on the shorter side of the index move by one to make room, so
 * the cost is in proportion to the distance from the nearer end.
 *
 * @param ring Required non-null.
 * @param index Where the new element goes; at most the length. 0 is the same
 * as tb_ring_push_front() and the length the same as tb_ring_push_back().
 * @param element Element to copy in, or NULL for a zero-filled element. It
 * must not point into the ring.
 * @return Pointer to the new element inside the ring.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_ring_insert(tb_ring *ring, size_t index, const void *element);

/**
 * @brief Remove an element and keep the others in order.
 *
 * The elements on the shorter side of the index move by one to close the gap.
 *
 * @param ring Required non-null.
 * @param index Index of the element; less than the length.
 * @param[out] element Receives the removed element, or NULL to discard it.
 */
TB_API void tb_ring_remove(tb_ring *ring, size_t index, void *element);

/* -------------------------------------------------------------------------
 * Rotating.
 * ------------------------------------------------------------------------- */

/**
 * @brief Rotate the elements toward the front.
 *
 * Each step moves the front element to the back, so the element that was at
 * index `count` becomes index 0. A ring of clients rotated left by one gives
 * the next client its turn.
 *
 * @param ring Required non-null. An empty ring is unchanged.
 * @param count Number of steps; any value. Rotating by the length changes
 * nothing, and larger counts wrap around.
 */
TB_API void tb_ring_rotate_left(tb_ring *ring, size_t count);

/**
 * @brief Rotate the elements toward the back.
 *
 * Each step moves the back element to the front, so the element that was at
 * index 0 becomes index `count`. It undoes tb_ring_rotate_left() by the same
 * count.
 *
 * @param ring Required non-null. An empty ring is unchanged.
 * @param count Number of steps; any value.
 */
TB_API void tb_ring_rotate_right(tb_ring *ring, size_t count);

/* -------------------------------------------------------------------------
 * Typed access. Each macro names the element type and, in builds with
 * assertions, checks that it has the size the ring was created with.
 * ------------------------------------------------------------------------- */

/** @brief tb_ring_at() with a check of the element size. Use TB_RING_AT(). */
TB_API void *tb_ring_at_sized(const tb_ring *ring, size_t index, size_t element_size);

/**
 * @brief Add an element at the back without initializing it, with a check of
 * the element size. Use TB_RING_PUSH_BACK().
 */
TB_API void *tb_ring_push_back_sized(tb_ring *ring, size_t element_size);

/**
 * @brief Add an element at the front without initializing it, with a check of
 * the element size. Use TB_RING_PUSH_FRONT().
 */
TB_API void *tb_ring_push_front_sized(tb_ring *ring, size_t element_size);

/**
 * @brief Pointer to an element, as its type: `TB_RING_AT(pending, event, 0)`.
 * @param ring Required non-null.
 * @param type The element type the ring was created with.
 * @param index Index of the element; less than the length.
 * @return `type *` into the ring.
 */
#define TB_RING_AT(ring, type, index) ((type *)tb_ring_at_sized((ring), (index), sizeof(type)))

/**
 * @brief Add a value at the back: `TB_RING_PUSH_BACK(pending, event, incoming)`.
 * @param ring Required non-null.
 * @param type The element type the ring was created with.
 * @param value Value to store, converted to `type` as by assignment. It must
 * not read the ring through a pointer taken earlier.
 */
#define TB_RING_PUSH_BACK(ring, type, value)                                                       \
    ((void)(*(type *)tb_ring_push_back_sized((ring), sizeof(type)) = (value)))

/**
 * @brief Add a value at the front: `TB_RING_PUSH_FRONT(pending, event, urgent)`.
 * @param ring Required non-null.
 * @param type The element type the ring was created with.
 * @param value Value to store, converted to `type` as by assignment. It must
 * not read the ring through a pointer taken earlier.
 */
#define TB_RING_PUSH_FRONT(ring, type, value)                                                      \
    ((void)(*(type *)tb_ring_push_front_sized((ring), sizeof(type)) = (value)))

#endif
