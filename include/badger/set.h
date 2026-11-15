/** @file set.h
 * @brief An unordered set: which things are members, answered in constant time.
 *
 * A set holds members and no values. A member is a pointer, which the set
 * stores and does not own: it is whatever the caller's hash and equality
 * functions understand.
 *
 * @code
 * tb_set *const visible = tb_set_create(NULL, 0, NULL, NULL); // members are atoms
 *
 * (void)tb_set_put(visible, tb_atom_string(names, "player"));
 * if (tb_set_contains(visible, name))
 * ...
 * @endcode
 *
 * **The same three kinds of member as the keys of a table (table.h):**
 * - **Atoms, and anything else compared by address.** Pass NULL for both
 *   functions.
 * - **Integers.** Also NULL for both, with the integer cast to a pointer:
 *   `(void *)(uintptr_t)entity_id`. Every value is a valid member, zero
 *   included.
 * - **C strings compared by content.** Pass tb_set_string_hash() and
 *   tb_set_string_equal(). The set keeps the pointer, so the string must
 *   outlive its membership and must not change while it is a member.
 *
 * **Set operations make new sets.** tb_set_union() and the others leave their
 * operands alone and return a set the caller owns. The operands must have been
 * created with the same hash and equality functions.
 *
 * **Order.** Members are visited in no particular order, and the order changes
 * as the set grows. With members compared by address the order follows the
 * addresses, so it can differ from one run of the program to the next. Code
 * that must do the same thing in every run must not let the order of a set
 * reach its results: sort the members, or keep the order it needs in an array.
 *
 * **Memory.** A set gets all of its memory from the allocator it was created
 * with (allocator.h). Growing fails fast; tb_set_try_reserve() is the one
 * fallible form. A set never shrinks by itself.
 *
 * For members that are small integers from a fixed range, a bit vector (bit.h)
 * is smaller and faster.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same set. Functions that take a `const tb_set *` only read it, and may run
 * together on the same set.
 */
#ifndef TB_SET_H
#define TB_SET_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief An unordered set. Created by tb_set_create(), tb_set_copy(), or a set
 * operation; owned by the caller; released by tb_set_destroy().
 */
typedef struct tb_set tb_set;

/**
 * @brief Computes the hash of a member. Equal members must have equal hashes.
 * See hash.h for functions to build one from.
 */
typedef uint64_t tb_set_hash_function(const void *member);

/** @brief Decides whether two members are the same member. */
typedef bool tb_set_equal_function(const void *left, const void *right);

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an empty set.
 * @param allocator Where the set gets its memory, or NULL for the engine's
 * heap. It is copied.
 * @param capacity_hint Number of members the set should hold before it has to
 * grow; 0 when unknown.
 * @param hash Hash function for the members, or NULL to hash a member by its
 * address. Give both functions or neither.
 * @param equal Equality function for the members, or NULL to compare members
 * by address.
 * @return The set; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_set *tb_set_create(const tb_allocator *allocator, size_t capacity_hint,
                             tb_set_hash_function *hash, tb_set_equal_function *equal);

/**
 * @brief Create a set with the same members as another.
 * @param set Set to copy; required non-null. The copy has its hash and
 * equality functions, and the same member pointers: what they point to is
 * shared, not copied.
 * @param allocator Where the copy gets its memory, or NULL for the heap. It
 * need not be the allocator of `set`.
 * @return The copy; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_set *tb_set_copy(const tb_set *set, const tb_allocator *allocator);

/**
 * @brief Destroy a set. What its members point to is not touched.
 * @param set Set to destroy; NULL is ignored.
 */
TB_API void tb_set_destroy(tb_set *set);

/** @brief Hash function for members that are NUL-terminated strings. */
TB_API uint64_t tb_set_string_hash(const void *member);

/** @brief Equality function for members that are NUL-terminated strings. */
TB_API bool tb_set_string_equal(const void *left, const void *right);

/* -------------------------------------------------------------------------
 * Members.
 * ------------------------------------------------------------------------- */

/** @brief Number of members in the set. @param set Required non-null. */
TB_API size_t tb_set_length(const tb_set *set);

/**
 * @brief Whether something is a member of the set.
 * @param set Required non-null.
 * @param member What to look for; any pointer, NULL included.
 */
TB_API bool tb_set_contains(const tb_set *set, const void *member);

/**
 * @brief Look a member up, and get the pointer the set holds for it.
 *
 * With an equality function, the member in the set may be a different pointer
 * from the one asked about: an equal string at another address. This returns
 * the one in the set.
 *
 * @param set Required non-null.
 * @param member What to look for.
 * @param[out] stored Receives the member as the set holds it when it is found,
 * and is left untouched when it is not; NULL when only the answer is wanted.
 * @return true when the member is in the set.
 */
TB_API bool tb_set_find(const tb_set *set, const void *member, const void **stored);

/**
 * @brief Add a member.
 * @param set Required non-null.
 * @param member The member; any pointer, NULL included. When an equal member
 * is already in the set, that one is kept and this one is not stored.
 * @return true when the member was added; false when an equal member was
 * already there and the set is unchanged.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API bool tb_set_put(tb_set *set, const void *member);

/**
 * @brief Remove a member.
 * @param set Required non-null.
 * @param member The member.
 * @param[out] stored Receives the member as the set held it when it was in the
 * set, so that what it points to can be released; NULL to discard.
 * @return true when the member was in the set and is now removed.
 */
TB_API bool tb_set_remove(tb_set *set, const void *member, const void **stored);

/**
 * @brief Remove every member and keep the storage for reuse.
 * @param set Required non-null.
 */
TB_API void tb_set_clear(tb_set *set);

/* -------------------------------------------------------------------------
 * All members.
 * ------------------------------------------------------------------------- */

/**
 * @brief Called by tb_set_visit() for each member.
 * @param member The member.
 * @param context The pointer given to tb_set_visit().
 */
typedef void tb_set_visitor(const void *member, void *context);

/**
 * @brief Call a function for every member, in no particular order.
 * @param set Required non-null.
 * @param visitor Required non-null. It must not add or remove members of this
 * set, which is a contract violation.
 * @param context Passed to every call of `visitor`; may be NULL.
 */
TB_API void tb_set_visit(tb_set *set, tb_set_visitor *visitor, void *context);

/**
 * @brief Step through the members one at a time, in no particular order.
 *
 * @code
 * size_t cursor = 0;
 * const void *member = NULL;
 *
 * while (tb_set_next(set, &cursor, &member))
 * {
 *     use(member);
 * }
 * @endcode
 *
 * The set must not gain or lose members between the first call and the last;
 * unlike with tb_set_visit(), that mistake is not detected.
 *
 * @param set Required non-null.
 * @param[in,out] cursor Where the walk has got to; required non-null. Set a
 * variable to 0 before the first call and pass it to every call. Its value
 * means nothing else.
 * @param[out] member Receives the next member; required non-null. Left
 * untouched when there are no more.
 * @return true when `member` was filled in; false when every member has been
 * returned.
 */
TB_API bool tb_set_next(const tb_set *set, size_t *cursor, const void **member);

/**
 * @brief Copy the members of a set into an array, in no particular order.
 * @param set Required non-null.
 * @param[out] members Receives up to `capacity` members; may be NULL only
 * when `capacity` is 0.
 * @param capacity Number of members the array can hold.
 * @return The number of members in the set, which is more than `capacity`
 * when the array was too small to hold them all.
 */
TB_API size_t tb_set_to_array(const tb_set *set, const void **members, size_t capacity);

/* -------------------------------------------------------------------------
 * Comparing sets.
 *
 * Both sets must have been created with the same hash and equality
 * functions; anything else is a contract violation.
 * ------------------------------------------------------------------------- */

/**
 * @brief Whether two sets have the same members.
 * @param left Required non-null.
 * @param right Required non-null.
 */
TB_API bool tb_set_equal(const tb_set *left, const tb_set *right);

/**
 * @brief Whether every member of `left` is also a member of `right`.
 * @param left Required non-null.
 * @param right Required non-null.
 */
TB_API bool tb_set_is_subset(const tb_set *left, const tb_set *right);

/**
 * @brief Whether `left` is a subset of `right` and `right` has a member more.
 * @param left Required non-null.
 * @param right Required non-null.
 */
TB_API bool tb_set_is_proper_subset(const tb_set *left, const tb_set *right);

/* -------------------------------------------------------------------------
 * Set operations.
 *
 * Each returns a new set, which the caller owns, with the hash and equality
 * functions of its operands. The operands are not changed, may be the same
 * set, and must have been created with the same hash and equality functions;
 * anything else is a contract violation. A NULL operand is not an empty set:
 * it is a contract violation too.
 *
 * Where equal members of both operands could be stored, the result holds the
 * pointer from `left`.
 * ------------------------------------------------------------------------- */

/**
 * @brief The members of either set.
 * @param left Required non-null.
 * @param right Required non-null.
 * @param allocator Where the result gets its memory, or NULL for the heap.
 * @return The new set; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_set *tb_set_union(const tb_set *left, const tb_set *right, const tb_allocator *allocator);

/**
 * @brief The members of both sets.
 * @param left Required non-null.
 * @param right Required non-null.
 * @param allocator Where the result gets its memory, or NULL for the heap.
 * @return The new set; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_set *tb_set_intersection(const tb_set *left, const tb_set *right,
                                   const tb_allocator *allocator);

/**
 * @brief The members of `left` that are not members of `right`.
 * @param left Required non-null.
 * @param right Required non-null.
 * @param allocator Where the result gets its memory, or NULL for the heap.
 * @return The new set; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_set *tb_set_difference(const tb_set *left, const tb_set *right,
                                 const tb_allocator *allocator);

/**
 * @brief The members of exactly one of the sets.
 * @param left Required non-null.
 * @param right Required non-null.
 * @param allocator Where the result gets its memory, or NULL for the heap.
 * @return The new set; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_set *tb_set_symmetric_difference(const tb_set *left, const tb_set *right,
                                           const tb_allocator *allocator);

/* -------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make room for at least `capacity` members without growing again.
 * @param set Required non-null.
 * @param capacity Members to make room for.
 * @par Errors Exhaustion, or a capacity whose size in bytes overflows
 * `size_t`, calls the memory failure handler and ends the process.
 */
TB_API void tb_set_reserve(tb_set *set, size_t capacity);

/**
 * @brief Make room for at least `capacity` members, or report that it cannot
 * be done.
 * @param set Required non-null.
 * @param capacity Members to make room for.
 * @return true when the set has the room. false, leaving the set unchanged,
 * when the allocator cannot supply the memory or the size in bytes overflows
 * `size_t`.
 */
TB_API bool tb_set_try_reserve(tb_set *set, size_t capacity);

/**
 * @brief Give back the storage the set is not using.
 *
 * When the allocator cannot supply the smaller block the set is left as it
 * was.
 *
 * @param set Required non-null.
 */
TB_API void tb_set_shrink_to_fit(tb_set *set);

/**
 * @brief Number of members the set can hold before it must grow.
 * @param set Required non-null.
 */
TB_API size_t tb_set_capacity(const tb_set *set);

#endif
