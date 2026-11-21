/** @file sort.h
 * @brief Sorting arrays, and finding a place in a sorted one.
 *
 * These replace the C library's qsort() and bsearch() for two reasons.
 *
 * **The comparison takes a context.** A comparison that needs more than the
 * two elements, such as the table their names are in or the direction to sort
 * by, receives it as an argument instead of through a global.
 *
 * **The order is the engine's, not the C library's.** qsort() is not stable,
 * and the C libraries of Windows, Linux, and macOS order equal elements
 * differently, so a draw list or the entities of a simulation would come out
 * in a different order on each platform. The algorithms here are the
 * engine's own: for the same input and comparison, every platform and every
 * build gives the same result, equal elements included.
 *
 * @code
 * static int by_distance(const void *left, const void *right, void *context)
 * {
 *     const float *const distances = context;
 *     const float first = distances[*(const uint32_t *)left];
 *     const float second = distances[*(const uint32_t *)right];
 *
 *     return (first > second) - (first < second);
 * }
 *
 * tb_sort(visible, visible_count, sizeof(visible[0]), by_distance, distances);
 * @endcode
 *
 * @par Threading
 * Thread-safe: no function keeps state. The array being sorted or searched
 * is externally synchronized: no other thread may change it during the call.
 */
#ifndef TB_SORT_H
#define TB_SORT_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stddef.h>

/**
 * @brief Order two elements.
 *
 * The comparison must be a consistent order: the same result for the same
 * two elements every time it is asked, the opposite sign when they are
 * swapped, and transitive. A comparison that is not, such as one that
 * compares floating-point values when one of them is NaN, leaves the array
 * in an unspecified order, though every element is still in it exactly once.
 *
 * @param left An element of the array, or the key of a search; never NULL.
 * @param right Another element of the array; never NULL.
 * @param context The pointer passed to the sorting function; may be NULL.
 * @return Negative when `left` goes before `right`, positive when it goes
 * after, and zero when they are equal.
 */
typedef int tb_sort_compare_function(const void *left, const void *right, void *context);

/**
 * @brief Sort an array in place.
 *
 * Introsort: quicksort with a median pivot, which turns to heapsort when the
 * pivots are poor and to insertion sort for short ranges. It takes
 * O(n log n) comparisons in the worst case, needs no memory beyond a little
 * stack, and never allocates.
 *
 * Elements that compare equal end in an order that is unspecified but fixed:
 * the same on every platform and in every build. Use tb_sort_stable() when
 * equal elements must keep the order they had.
 *
 * @param elements The array; may be NULL only when `count` is 0.
 * @param count Number of elements.
 * @param size Size of one element in bytes; at least 1. `count * size` must
 * not overflow.
 * @param compare The order; required non-null.
 * @param context Passed to every call of `compare`; may be NULL.
 */
TB_API void tb_sort(void *elements, size_t count, size_t size, tb_sort_compare_function *compare,
                    void *context);

/**
 * @brief Sort an array, keeping equal elements in the order they had.
 *
 * Merge sort. It takes O(n log n) comparisons in the worst case and O(n) for
 * an array that is already sorted. It needs a scratch buffer of half the
 * array, from `allocator`, and releases it before returning; an array of a
 * few elements needs none.
 *
 * Fails fast like tb_allocator_alloc(): when the allocator cannot supply the
 * buffer, the memory failure handler is called and the process ends.
 *
 * @param elements The array; may be NULL only when `count` is 0.
 * @param count Number of elements.
 * @param size Size of one element in bytes; at least 1. `count * size` must
 * not overflow.
 * @param compare The order; required non-null.
 * @param context Passed to every call of `compare`; may be NULL.
 * @param allocator Where the scratch buffer comes from; NULL for the
 * engine's heap. An arena allocator suits it well.
 */
TB_API void tb_sort_stable(void *elements, size_t count, size_t size,
                           tb_sort_compare_function *compare, void *context,
                           const tb_allocator *allocator);

/**
 * @brief Find where a key belongs in a sorted array.
 *
 * Binary search, in O(log n) comparisons. `compare` is called with an
 * element of the array as `left` and `key` as `right`, so the key may be a
 * different type from the elements, such as a name to find among records,
 * as long as the comparison knows both.
 *
 * @param elements The array, sorted by `compare`; may be NULL only when
 * `count` is 0.
 * @param count Number of elements.
 * @param size Size of one element in bytes; at least 1.
 * @param key What to look for; required non-null.
 * @param compare The order the array is sorted in; required non-null.
 * @param context Passed to every call of `compare`; may be NULL.
 * @return The index of the first element that does not go before `key`, or
 * `count` when every element does. The key is present when the result is
 * less than `count` and the element there compares equal to it; this is also
 * where to insert the key to keep the array sorted.
 */
TB_API size_t tb_sort_lower_bound(const void *elements, size_t count, size_t size, const void *key,
                                  tb_sort_compare_function *compare, void *context);

#endif
