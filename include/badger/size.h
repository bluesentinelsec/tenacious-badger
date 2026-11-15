/** @file size.h
 * @brief Arithmetic on object sizes and element counts that reports overflow.
 *
 * A size computed from counts and lengths the program does not control, such
 * as `count * sizeof(element)` or `header + payload`, can pass `SIZE_MAX` and
 * wrap to a small number. Allocating or indexing with the wrapped value
 * corrupts memory. These functions compute the same values and refuse instead
 * of wrapping. Use them wherever a size is computed rather than written down.
 *
 * Every function follows one contract: it returns true and stores the exact
 * result, or it returns false and leaves the output untouched.
 *
 * @par Threading
 * Thread-safe for independent outputs; callers synchronize concurrent access
 * to the same output.
 */
#ifndef TB_SIZE_H
#define TB_SIZE_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Add two sizes after checking for overflow.
 * @param left First operand; any value is valid.
 * @param right Second operand; any value is valid.
 * @param[out] result Required non-null caller-owned output. It may be the
 * address of a variable that was also passed as an operand.
 * @return true on success; false when `left + right` exceeds `SIZE_MAX`,
 * leaving `*result` unchanged.
 */
TB_API bool tb_size_add(size_t left, size_t right, size_t *result);

/**
 * @brief Multiply two sizes after checking for overflow.
 * @param left First operand; any value is valid.
 * @param right Second operand; any value is valid.
 * @param[out] result Required non-null caller-owned output. It may be the
 * address of a variable that was also passed as an operand.
 * @return true on success; false when `left * right` exceeds `SIZE_MAX`,
 * leaving `*result` unchanged.
 */
TB_API bool tb_size_multiply(size_t left, size_t right, size_t *result);

/**
 * @brief Choose the capacity of a buffer that has to hold more elements.
 *
 * A buffer that grows only to the size it needs is reallocated on every
 * append. This grows it by doubling, so that appending one element at a time
 * costs a constant amount of copying on average:
 *
 * @code
 * if (!tb_size_grow(capacity, length + 1, sizeof(*items), &capacity))
 * {
 *     goto cleanup;
 * }
 * items = tb_realloc_array(items, capacity, sizeof(*items));
 * @endcode
 *
 * @param capacity Elements the buffer holds now; 0 for a buffer that does not
 * exist yet. `capacity * element_size` must not exceed `SIZE_MAX`, which is
 * true of every buffer that exists.
 * @param required Elements the buffer must hold.
 * @param element_size Bytes per element; 0 is served as 1.
 * @param[out] result Required non-null caller-owned output. It may be the
 * address of a variable that was also passed as an operand. Receives:
 * - `capacity`, when that already holds `required` elements;
 * - otherwise the larger of `required` and twice `capacity`, limited to the
 *   largest capacity whose size in bytes fits in `size_t`.
 * The received capacity multiplied by `element_size` never overflows.
 * @return true on success; false when `required * element_size` exceeds
 * `SIZE_MAX`, leaving `*result` unchanged.
 */
TB_API bool tb_size_grow(size_t capacity, size_t required, size_t element_size, size_t *result);

#endif
