/** @file array.h
 * @brief A dynamic array: elements of one type, stored contiguously, growing
 * as needed.
 *
 * This is the collection to reach for first. Elements are stored by value,
 * one after another, so iterating them is as fast as memory allows and an
 * element costs exactly its own size. Appending is cheap on average because
 * the capacity doubles when it runs out.
 *
 * @code
 * tb_array *const scores = TB_ARRAY_CREATE(NULL, int);
 *
 * TB_ARRAY_PUSH(scores, int, 40);
 * TB_ARRAY_PUSH(scores, int, 2);
 * for (size_t index = 0; index < tb_array_length(scores); index++)
 * {
 *     total += *TB_ARRAY_AT(scores, int, index);
 * }
 * tb_array_destroy(scores);
 * @endcode
 *
 * **Typed and untyped access.** The functions take and return `void *` and
 * copy elements by their size. The `TB_ARRAY_*` macros name the element type,
 * return typed pointers, and, in builds with assertions, check that the type
 * has the size the array was created with. Prefer the macros.
 *
 * **Pointers into the array are short-lived.** A pointer to an element, or to
 * the storage, is valid until the array next grows, shrinks its storage, or
 * is destroyed. Any function that may add an element may grow the array. Keep
 * an index, not a pointer, across such calls.
 *
 * **Elements are plain bytes.** The array copies them with `memcpy` and never
 * runs code for them. An element that owns something else must have that
 * released before the element is removed or the array destroyed.
 *
 * **Memory.** An array gets all of its memory from the allocator it was
 * created with (allocator.h). Growing fails fast, like the rest of the
 * engine; tb_array_try_reserve() is the one fallible form, for a caller that
 * must survive a capacity it cannot get.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same array. Different arrays are independent.
 */
#ifndef TB_ARRAY_H
#define TB_ARRAY_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief A dynamic array. Created by tb_array_create() or tb_array_copy();
 * owned by the caller; released by tb_array_destroy().
 */
typedef struct tb_array tb_array;

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an empty array.
 * @param allocator Where the array gets its memory, or NULL for the engine's
 * heap. It is copied.
 * @param element_size Bytes per element; at least 1 and a multiple of
 * `element_alignment`, as `sizeof` is for every type.
 * @param element_alignment Alignment of an element in bytes; a power of two.
 * @return The array; never NULL. It holds no elements and no storage yet.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_array *tb_array_create(const tb_allocator *allocator, size_t element_size,
                                 size_t element_alignment);

/**
 * @brief Create an empty array of a type: `TB_ARRAY_CREATE(NULL, vertex)`.
 * @param allocator Where the array gets its memory, or NULL for the heap.
 * @param type Element type.
 * @return The array; never NULL.
 */
#define TB_ARRAY_CREATE(allocator, type) tb_array_create((allocator), sizeof(type), _Alignof(type))

/**
 * @brief Create an array with the same elements as another.
 * @param array Array to copy; required non-null.
 * @param allocator Where the copy gets its memory, or NULL for the heap. It
 * need not be the allocator of `array`.
 * @return The copy; never NULL. Its capacity is its length.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_array *tb_array_copy(const tb_array *array, const tb_allocator *allocator);

/**
 * @brief Destroy an array and release its storage.
 * @param array Array to destroy; NULL is ignored. The pointer, and every
 * pointer into the array, is invalid afterwards.
 */
TB_API void tb_array_destroy(tb_array *array);

/* -------------------------------------------------------------------------
 * Size.
 * ------------------------------------------------------------------------- */

/** @brief Number of elements in the array. @param array Required non-null. */
TB_API size_t tb_array_length(const tb_array *array);

/**
 * @brief Number of elements the array can hold before it must grow.
 * @param array Required non-null.
 */
TB_API size_t tb_array_capacity(const tb_array *array);

/** @brief Bytes per element, as given at creation. @param array Required non-null. */
TB_API size_t tb_array_element_size(const tb_array *array);

/**
 * @brief Make room for at least `capacity` elements without changing the length.
 *
 * Reserving the final size before a run of pushes makes the run allocate once
 * and keeps pointers into the array valid during it.
 *
 * @param array Required non-null.
 * @param capacity Elements to make room for. A capacity the array already
 * has changes nothing.
 * @par Errors Exhaustion, or a capacity whose size in bytes overflows
 * `size_t`, calls the memory failure handler and ends the process.
 */
TB_API void tb_array_reserve(tb_array *array, size_t capacity);

/**
 * @brief Make room for at least `capacity` elements, or report that it cannot
 * be done.
 * @param array Required non-null.
 * @param capacity Elements to make room for.
 * @return true when the array has the capacity. false, leaving the array
 * unchanged, when the allocator cannot supply the memory or the size in
 * bytes overflows `size_t`.
 */
TB_API bool tb_array_try_reserve(tb_array *array, size_t capacity);

/**
 * @brief Change the number of elements.
 * @param array Required non-null.
 * @param length New length. Elements beyond it are removed; elements added to
 * reach it are zero-filled. The capacity never shrinks.
 * @par Errors As for tb_array_reserve().
 */
TB_API void tb_array_resize(tb_array *array, size_t length);

/**
 * @brief Remove every element and keep the storage for reuse.
 * @param array Required non-null.
 */
TB_API void tb_array_clear(tb_array *array);

/**
 * @brief Give back the storage the array is not using.
 *
 * The capacity becomes the length. When the allocator cannot supply the
 * smaller block the array is left as it was.
 *
 * @param array Required non-null.
 */
TB_API void tb_array_shrink_to_fit(tb_array *array);

/* -------------------------------------------------------------------------
 * Elements.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get a pointer to an element.
 * @param array Required non-null.
 * @param index Index of the element; less than the length.
 * @return Pointer to the element inside the array, valid until the array
 * next grows, shrinks its storage, or is destroyed.
 */
TB_API void *tb_array_at(const tb_array *array, size_t index);

/**
 * @brief Get a pointer to the first element of the contiguous storage.
 * @param array Required non-null.
 * @return Pointer to tb_array_length() elements, one after another, valid
 * until the array next grows, shrinks its storage, or is destroyed. NULL when
 * the array has never held an element.
 */
TB_API void *tb_array_data(const tb_array *array);

/**
 * @brief Copy an element out of the array.
 * @param array Required non-null.
 * @param index Index of the element; less than the length.
 * @param[out] element Required non-null; receives tb_array_element_size() bytes.
 */
TB_API void tb_array_get(const tb_array *array, size_t index, void *element);

/**
 * @brief Overwrite an element.
 * @param array Required non-null.
 * @param index Index of the element; less than the length.
 * @param element Required non-null; tb_array_element_size() bytes are copied
 * from it. It must not point into the array.
 */
TB_API void tb_array_put(tb_array *array, size_t index, const void *element);

/**
 * @brief Add an element at the end.
 * @param array Required non-null.
 * @param element Element to copy in, or NULL for a zero-filled element. It
 * must not point into the array, which may move to make room.
 * @return Pointer to the new element inside the array.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_array_push(tb_array *array, const void *element);

/**
 * @brief Add several elements at the end.
 * @param array Required non-null.
 * @param elements `count` elements to copy in, one after another; may be NULL
 * only when `count` is 0. They must not be in the array.
 * @param count Number of elements.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void tb_array_append(tb_array *array, const void *elements, size_t count);

/**
 * @brief Remove the last element.
 * @param array Required non-null and not empty.
 * @param[out] element Receives the removed element, or NULL to discard it.
 */
TB_API void tb_array_pop(tb_array *array, void *element);

/**
 * @brief Add an element at an index, moving the elements from there on up by one.
 * @param array Required non-null.
 * @param index Where the new element goes; at most the length, which appends.
 * @param element Element to copy in, or NULL for a zero-filled element. It
 * must not point into the array.
 * @return Pointer to the new element inside the array.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_array_insert(tb_array *array, size_t index, const void *element);

/**
 * @brief Remove an element and keep the others in order.
 *
 * The elements after it move down by one, which costs time in proportion to
 * their number. When order does not matter, tb_array_remove_unordered() is
 * constant time.
 *
 * @param array Required non-null.
 * @param index Index of the element; less than the length.
 * @param[out] element Receives the removed element, or NULL to discard it.
 */
TB_API void tb_array_remove(tb_array *array, size_t index, void *element);

/**
 * @brief Remove an element by moving the last element into its place.
 * @param array Required non-null.
 * @param index Index of the element; less than the length.
 * @param[out] element Receives the removed element, or NULL to discard it.
 */
TB_API void tb_array_remove_unordered(tb_array *array, size_t index, void *element);

/* -------------------------------------------------------------------------
 * Typed access. Each macro names the element type and, in builds with
 * assertions, checks that it has the size the array was created with.
 * ------------------------------------------------------------------------- */

/** @brief tb_array_at() with a check of the element size. Use TB_ARRAY_AT(). */
TB_API void *tb_array_at_sized(const tb_array *array, size_t index, size_t element_size);

/** @brief tb_array_data() with a check of the element size. Use TB_ARRAY_DATA(). */
TB_API void *tb_array_data_sized(const tb_array *array, size_t element_size);

/**
 * @brief Add an element at the end without initializing it, with a check of
 * the element size. Use TB_ARRAY_PUSH().
 */
TB_API void *tb_array_push_sized(tb_array *array, size_t element_size);

/**
 * @brief Pointer to an element, as its type: `TB_ARRAY_AT(scores, int, 3)`.
 * @param array Required non-null.
 * @param type The element type the array was created with.
 * @param index Index of the element; less than the length.
 * @return `type *` into the array, valid until the array next grows.
 */
#define TB_ARRAY_AT(array, type, index) ((type *)tb_array_at_sized((array), (index), sizeof(type)))

/**
 * @brief Pointer to the storage, as the element type: `TB_ARRAY_DATA(scores, int)`.
 * @param array Required non-null.
 * @param type The element type the array was created with.
 * @return `type *` to the first element, or NULL when the array has never
 * held one.
 */
#define TB_ARRAY_DATA(array, type) ((type *)tb_array_data_sized((array), sizeof(type)))

/**
 * @brief Add a value at the end: `TB_ARRAY_PUSH(scores, int, 42)`.
 * @param array Required non-null.
 * @param type The element type the array was created with.
 * @param value Value to store, converted to `type` as by assignment. It may
 * be evaluated after the array has made room, so it must not read the array
 * through a pointer taken earlier.
 */
#define TB_ARRAY_PUSH(array, type, value)                                                          \
    ((void)(*(type *)tb_array_push_sized((array), sizeof(type)) = (value)))

#endif
