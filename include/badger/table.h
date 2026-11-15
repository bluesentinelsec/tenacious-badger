/** @file table.h
 * @brief A hash table: values found by key in constant time on average.
 *
 * A table maps keys to values. Both are pointers, which the table stores and
 * does not own: a key is whatever the caller's hash and equality functions
 * understand, and a value is whatever the caller wants to find again.
 *
 * @code
 * tb_table *const variables = tb_table_create(NULL, 0, NULL, NULL); // keys are atoms
 *
 * (void)tb_table_put(variables, tb_atom_string(names, "r_fov"), &fov);
 * variable = tb_table_get(variables, tb_atom_string(names, typed_name));
 * @endcode
 *
 * **Three kinds of key cover most uses:**
 * - **Atoms, and any other key compared by address.** Pass NULL for both
 *   functions. Two keys are equal when they are the same pointer.
 * - **Integers.** Also NULL for both functions, with the integer cast to a
 *   pointer: `(void *)(uintptr_t)entity_id`. Every value is a valid key, zero
 *   included.
 * - **C strings compared by content.** Pass tb_table_string_hash() and
 *   tb_table_string_equal(). The table keeps the pointer, so the string must
 *   outlive its entry and must not change while it is a key.
 *
 * **The table borrows.** It never copies or releases what its keys and values
 * point to. Release those yourself, by visiting the table before it is
 * destroyed, or keep them in an arena with the table.
 *
 * **Order.** Entries are visited in no particular order, and the order changes
 * as the table grows. With keys compared by address the order follows the
 * addresses, so it can differ from one run of the program to the next. Code
 * that must do the same thing in every run, a simulation that is replayed for
 * one, must not let the order of a table reach its results: sort the entries,
 * or keep the order it needs in an array.
 *
 * **Memory.** A table gets all of its memory from the allocator it was created
 * with (allocator.h). Growing fails fast; tb_table_try_reserve() is the one
 * fallible form. A table never shrinks by itself: removing entries leaves room
 * for as many again, and tb_table_shrink_to_fit() gives the room back.
 *
 * @par Threading
 * Externally synchronized: the caller serializes every call that names the
 * same table. Functions that only read may run together.
 */
#ifndef TB_TABLE_H
#define TB_TABLE_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A hash table. Created by tb_table_create(); owned by the caller;
 * released by tb_table_destroy().
 */
typedef struct tb_table tb_table;

/**
 * @brief Computes the hash of a key. Equal keys must have equal hashes. See
 * hash.h for functions to build one from.
 */
typedef uint64_t tb_table_hash_function(const void *key);

/** @brief Decides whether two keys are the same key. */
typedef bool tb_table_equal_function(const void *left, const void *right);

/** @brief One entry of a table, as tb_table_to_array() copies it out. */
typedef struct tb_table_entry
{
    const void *key; /**< The key, as it was given to tb_table_put(). */
    void *value;     /**< The value stored with it. */
} tb_table_entry;

/* -------------------------------------------------------------------------
 * Creating and destroying.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create an empty table.
 * @param allocator Where the table gets its memory, or NULL for the engine's
 * heap. It is copied.
 * @param capacity_hint Number of entries the table should hold before it has
 * to grow; 0 when unknown.
 * @param hash Hash function for the keys, or NULL to hash a key by its
 * address. Give both functions or neither.
 * @param equal Equality function for the keys, or NULL to compare keys by
 * address.
 * @return The table; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_table *tb_table_create(const tb_allocator *allocator, size_t capacity_hint,
                                 tb_table_hash_function *hash, tb_table_equal_function *equal);

/**
 * @brief Create a table with the same entries as another.
 * @param table Table to copy; required non-null. The copy has its hash and
 * equality functions, and the same key and value pointers: what they point to
 * is shared, not copied.
 * @param allocator Where the copy gets its memory, or NULL for the heap. It
 * need not be the allocator of `table`.
 * @return The copy; never NULL. It has room for its entries and no more than
 * that needs.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_table *tb_table_copy(const tb_table *table, const tb_allocator *allocator);

/**
 * @brief Destroy a table. What its keys and values point to is not touched.
 * @param table Table to destroy; NULL is ignored.
 */
TB_API void tb_table_destroy(tb_table *table);

/** @brief Hash function for keys that are NUL-terminated strings. */
TB_API uint64_t tb_table_string_hash(const void *key);

/** @brief Equality function for keys that are NUL-terminated strings. */
TB_API bool tb_table_string_equal(const void *left, const void *right);

/* -------------------------------------------------------------------------
 * Entries.
 * ------------------------------------------------------------------------- */

/** @brief Number of entries in the table. @param table Required non-null. */
TB_API size_t tb_table_length(const tb_table *table);

/**
 * @brief Store a value under a key, replacing any value already there.
 * @param table Required non-null.
 * @param key The key; any pointer, NULL included. When an equal key is
 * already in the table, that key is kept and this one is not stored.
 * @param value The value; any pointer, NULL included.
 * @return The value this replaced, or NULL when the key was new. Use
 * tb_table_contains() first to tell a new key from one that held NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API void *tb_table_put(tb_table *table, const void *key, void *value);

/**
 * @brief Get the value stored under a key.
 * @param table Required non-null.
 * @param key The key.
 * @return The value, or NULL when the key is not in the table. When NULL is
 * also stored as a value, use tb_table_find() to tell the two apart.
 */
TB_API void *tb_table_get(const tb_table *table, const void *key);

/**
 * @brief Look a key up, and report whether it is there.
 * @param table Required non-null.
 * @param key The key.
 * @param[out] entry Receives the key as it is stored and its value when the
 * key is found, and is left untouched when it is not; NULL when only the
 * answer is wanted.
 * @return true when the key is in the table.
 */
TB_API bool tb_table_find(const tb_table *table, const void *key, tb_table_entry *entry);

/** @brief Whether a key is in the table. @param table Required non-null. */
TB_API bool tb_table_contains(const tb_table *table, const void *key);

/**
 * @brief Remove a key and its value.
 * @param table Required non-null.
 * @param key The key.
 * @param[out] entry Receives the stored key and its value when the key was in
 * the table, so that what they point to can be released; NULL to discard.
 * @return true when the key was in the table and is now removed.
 */
TB_API bool tb_table_remove(tb_table *table, const void *key, tb_table_entry *entry);

/**
 * @brief Remove every entry and keep the storage for reuse.
 * @param table Required non-null.
 */
TB_API void tb_table_clear(tb_table *table);

/* -------------------------------------------------------------------------
 * All entries.
 * ------------------------------------------------------------------------- */

/**
 * @brief Called by tb_table_visit() for each entry.
 * @param key The key of the entry.
 * @param value Where its value is stored. The visitor may read it and may
 * store another value through it.
 * @param context The pointer given to tb_table_visit().
 */
typedef void tb_table_visitor(const void *key, void **value, void *context);

/**
 * @brief Call a function for every entry, in no particular order.
 * @param table Required non-null.
 * @param visitor Required non-null. It may change the value of the entry it
 * is given, through the pointer it receives. It must not call a function that
 * changes this table, tb_table_put() included, which is a contract violation.
 * @param context Passed to every call of `visitor`; may be NULL.
 */
TB_API void tb_table_visit(tb_table *table, tb_table_visitor *visitor, void *context);

/**
 * @brief Copy the entries of a table into an array, in no particular order.
 * @param table Required non-null.
 * @param[out] entries Receives up to `capacity` entries; may be NULL only
 * when `capacity` is 0.
 * @param capacity Number of entries the array can hold.
 * @return The number of entries in the table, which is more than `capacity`
 * when the array was too small to hold them all.
 */
TB_API size_t tb_table_to_array(const tb_table *table, tb_table_entry *entries, size_t capacity);

/**
 * @brief Step through the entries one at a time, in no particular order.
 *
 * @code
 * size_t cursor = 0;
 * tb_table_entry entry = {0};
 *
 * while (tb_table_next(table, &cursor, &entry))
 * {
 *     use(entry.key, entry.value);
 * }
 * @endcode
 *
 * The table must not gain or lose entries between the first call and the
 * last; unlike with tb_table_visit(), that mistake is not detected.
 *
 * @param table Required non-null.
 * @param[in,out] cursor Where the walk has got to; required non-null. Set a
 * variable to 0 before the first call and pass it to every call. Its value
 * means nothing else.
 * @param[out] entry Receives the next entry; required non-null. Left
 * untouched when there are no more.
 * @return true when `entry` was filled in; false when every entry has been
 * returned.
 */
TB_API bool tb_table_next(const tb_table *table, size_t *cursor, tb_table_entry *entry);

/* -------------------------------------------------------------------------
 * Storage.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make room for at least `capacity` entries without growing again.
 * @param table Required non-null.
 * @param capacity Entries to make room for.
 * @par Errors Exhaustion, or a capacity whose size in bytes overflows
 * `size_t`, calls the memory failure handler and ends the process.
 */
TB_API void tb_table_reserve(tb_table *table, size_t capacity);

/**
 * @brief Make room for at least `capacity` entries, or report that it cannot
 * be done.
 * @param table Required non-null.
 * @param capacity Entries to make room for.
 * @return true when the table has the room. false, leaving the table
 * unchanged, when the allocator cannot supply the memory or the size in bytes
 * overflows `size_t`.
 */
TB_API bool tb_table_try_reserve(tb_table *table, size_t capacity);

/**
 * @brief Give back the storage the table is not using.
 *
 * The table keeps the fewest slots that hold its entries, and none when it is
 * empty. When the allocator cannot supply the smaller block the table is left
 * as it was.
 *
 * @param table Required non-null.
 */
TB_API void tb_table_shrink_to_fit(tb_table *table);

/**
 * @brief Number of entries the table can hold before it must grow.
 * @param table Required non-null.
 */
TB_API size_t tb_table_capacity(const tb_table *table);

#endif
