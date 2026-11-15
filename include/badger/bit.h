/** @file bit.h
 * @brief A bit vector: a fixed number of bits with set operations.
 *
 * A bit vector answers "which of these N things" in one bit each: which
 * components an entity has, which objects are visible, which slots of a pool
 * are taken. Whole vectors combine with the set operations (union,
 * intersection, difference), 64 bits at a time.
 *
 * @code
 * tb_bit *const taken = tb_bit_create(NULL, slot_count);
 * const size_t slot = tb_bit_find_clear(taken, 0);
 *
 * if (slot != TB_BIT_NOT_FOUND)
 * {
 *     (void)tb_bit_put(taken, slot, true);
 * }
 * @endcode
 *
 * **Bits are numbered from 0.** A range of bits is given as `begin` and `end`
 * and holds the bits from `begin` up to, but not including, `end`.
 *
 * **The length is fixed** when a vector is created. Operations on two vectors
 * require them to have the same length.
 *
 * **Memory.** A vector gets its memory from the allocator it was created
 * with (allocator.h), in one block, and never allocates again.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same vector. Functions that only read may run together.
 */
#ifndef TB_BIT_H
#define TB_BIT_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A bit vector. Created by tb_bit_create() or tb_bit_copy(); owned by
 * the caller; released by tb_bit_destroy().
 */
typedef struct tb_bit tb_bit;

/** @brief Returned by the search functions when no bit matches. */
#define TB_BIT_NOT_FOUND SIZE_MAX

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create a bit vector with every bit clear.
 * @param allocator Where the vector gets its memory, or NULL for the engine's
 * heap.
 * @param length Number of bits; may be 0.
 * @return The vector; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_bit *tb_bit_create(const tb_allocator *allocator, size_t length);

/**
 * @brief Create a bit vector with the same length and bits as another.
 * @param bits Vector to copy; required non-null.
 * @param allocator Where the copy gets its memory, or NULL for the heap.
 * @return The copy; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_bit *tb_bit_copy(const tb_bit *bits, const tb_allocator *allocator);

/**
 * @brief Destroy a bit vector.
 * @param bits Vector to destroy; NULL is ignored.
 */
TB_API void tb_bit_destroy(tb_bit *bits);

/* -------------------------------------------------------------------------
 * Single bits.
 * ------------------------------------------------------------------------- */

/** @brief Number of bits in the vector. @param bits Required non-null. */
TB_API size_t tb_bit_length(const tb_bit *bits);

/** @brief Number of bits that are set. @param bits Required non-null. */
TB_API size_t tb_bit_count(const tb_bit *bits);

/**
 * @brief Read one bit.
 * @param bits Required non-null.
 * @param index Number of the bit; less than the length.
 * @return true when the bit is set.
 */
TB_API bool tb_bit_get(const tb_bit *bits, size_t index);

/**
 * @brief Write one bit.
 * @param bits Required non-null.
 * @param index Number of the bit; less than the length.
 * @param value true to set the bit, false to clear it.
 * @return The value the bit had before.
 */
TB_API bool tb_bit_put(tb_bit *bits, size_t index, bool value);

/* -------------------------------------------------------------------------
 * Ranges of bits: from `begin` up to, but not including, `end`.
 * ------------------------------------------------------------------------- */

/**
 * @brief Set every bit in a range.
 * @param bits Required non-null.
 * @param begin First bit of the range; at most `end`.
 * @param end One past the last bit of the range; at most the length. An
 * empty range changes nothing.
 */
TB_API void tb_bit_set(tb_bit *bits, size_t begin, size_t end);

/** @brief Clear every bit in a range. Parameters as for tb_bit_set(). */
TB_API void tb_bit_clear(tb_bit *bits, size_t begin, size_t end);

/** @brief Invert every bit in a range. Parameters as for tb_bit_set(). */
TB_API void tb_bit_flip(tb_bit *bits, size_t begin, size_t end);

/* -------------------------------------------------------------------------
 * Searching.
 * ------------------------------------------------------------------------- */

/**
 * @brief Find the first set bit at or after a position.
 *
 * To visit every set bit, use tb_bit_next_set(), which keeps its place from
 * one bit to the next.
 *
 * @param bits Required non-null.
 * @param from Where to start looking; at most the length.
 * @return The number of the bit, or #TB_BIT_NOT_FOUND when no bit from there
 * on is set.
 */
TB_API size_t tb_bit_find_set(const tb_bit *bits, size_t from);

/**
 * @brief Find the first clear bit at or after a position.
 * @param bits Required non-null.
 * @param from Where to start looking; at most the length.
 * @return The number of the bit, or #TB_BIT_NOT_FOUND when every bit from
 * there on is set.
 */
TB_API size_t tb_bit_find_clear(const tb_bit *bits, size_t from);

/**
 * @brief Where a walk over the set bits of a vector has got to.
 *
 * Start one as `{0}` and pass it to every call of tb_bit_next_set(). Its
 * members belong to that function.
 */
typedef struct tb_bit_cursor
{
    size_t next_word;   /**< Private: the word the walk takes next. */
    uint64_t remaining; /**< Private: set bits of the current word not yet returned. */
} tb_bit_cursor;

/**
 * @brief Step through the set bits of a vector, from the lowest to the highest.
 *
 * @code
 * tb_bit_cursor cursor = {0};
 * size_t index = 0;
 *
 * while (tb_bit_next_set(visible, &cursor, &index))
 * {
 *     draw(objects[index]);
 * }
 * @endcode
 *
 * This is the way to visit every set bit. It passes over 64 clear bits at a
 * step and does not look for its place again between two bits, as a loop of
 * tb_bit_find_set() calls must.
 *
 * The vector must not change between the first call and the last; unlike a
 * contract violation elsewhere, that mistake is not detected. To change bits
 * while walking them, use tb_bit_find_set().
 *
 * @param bits Required non-null.
 * @param[in,out] cursor Where the walk has got to; required non-null. Start
 * it as `{0}` and pass it to every call.
 * @param[out] index Receives the number of the next set bit; required
 * non-null. Left untouched when there are no more.
 * @return true when `index` was filled in; false when every set bit has been
 * returned.
 */
TB_API bool tb_bit_next_set(const tb_bit *bits, tb_bit_cursor *cursor, size_t *index);

/**
 * @brief Called by tb_bit_visit() for each bit.
 * @param index Number of the bit.
 * @param value Whether the bit is set, read just before the call.
 * @param context The pointer given to tb_bit_visit().
 */
typedef void tb_bit_visitor(size_t index, bool value, void *context);

/**
 * @brief Call a function for every bit, from the first to the last.
 * @param bits Required non-null. The visitor may change bits of the vector;
 * a bit it has not reached yet is reported with its new value.
 * @param visitor Required non-null.
 * @param context Passed to every call of `visitor`; may be NULL.
 */
TB_API void tb_bit_visit(const tb_bit *bits, tb_bit_visitor *visitor, void *context);

/* -------------------------------------------------------------------------
 * Comparing two vectors of the same length.
 * ------------------------------------------------------------------------- */

/**
 * @brief Whether two vectors have the same bits set.
 * @param left Required non-null.
 * @param right Required non-null, with the length of `left`.
 */
TB_API bool tb_bit_equal(const tb_bit *left, const tb_bit *right);

/**
 * @brief Whether every bit set in `left` is also set in `right`.
 * @param left Required non-null.
 * @param right Required non-null, with the length of `left`.
 */
TB_API bool tb_bit_is_subset(const tb_bit *left, const tb_bit *right);

/**
 * @brief Whether `left` is a subset of `right` and `right` has a bit more.
 * @param left Required non-null.
 * @param right Required non-null, with the length of `left`.
 */
TB_API bool tb_bit_is_proper_subset(const tb_bit *left, const tb_bit *right);

/* -------------------------------------------------------------------------
 * Combining two vectors of the same length. Each changes `bits` in place and
 * allocates nothing. To keep the original, combine into a copy.
 * ------------------------------------------------------------------------- */

/**
 * @brief Union: also set every bit that is set in `other`.
 * @param bits Required non-null; receives the result.
 * @param other Required non-null, with the length of `bits`. It may be `bits`.
 */
TB_API void tb_bit_union(tb_bit *bits, const tb_bit *other);

/**
 * @brief Intersection: clear every bit that is not set in `other`.
 * @param bits Required non-null; receives the result.
 * @param other Required non-null, with the length of `bits`. It may be `bits`.
 */
TB_API void tb_bit_intersection(tb_bit *bits, const tb_bit *other);

/**
 * @brief Difference: clear every bit that is set in `other`.
 * @param bits Required non-null; receives the result.
 * @param other Required non-null, with the length of `bits`. It may be
 * `bits`, which clears every bit.
 */
TB_API void tb_bit_difference(tb_bit *bits, const tb_bit *other);

/**
 * @brief Symmetric difference: invert every bit that is set in `other`,
 * leaving the bits set in exactly one of the two.
 * @param bits Required non-null; receives the result.
 * @param other Required non-null, with the length of `bits`. It may be
 * `bits`, which clears every bit.
 */
TB_API void tb_bit_symmetric_difference(tb_bit *bits, const tb_bit *other);

#endif
