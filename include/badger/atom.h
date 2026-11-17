/** @file atom.h
 * @brief Atoms: strings stored once, so that equal strings are the same pointer.
 *
 * Interning a string returns its atom: a pointer to the one copy of those
 * bytes that the table keeps. Two atoms from the same table are equal exactly
 * when their pointers are equal, so comparing names costs one comparison
 * however long they are, and an atom is a ready-made key for a hash table.
 * Names that are compared far more often than they are created are what atoms
 * are for: console variables and commands, assets, components.
 *
 * @code
 * tb_atom_table *const names = tb_atom_table_create(NULL);
 * const char *const fov = tb_atom_string(names, "r_fov");
 *
 * if (tb_atom_string(names, typed_name) == fov) // one pointer comparison
 * ...
 * @endcode
 *
 * **An atom is a `const char *`.** It points to the bytes followed by a
 * terminator, so an atom without zero bytes in it can be used wherever a C
 * string can. An atom may hold any bytes, zero bytes included, and
 * tb_atom_length() gives its true length in constant time.
 *
 * **Atoms live as long as their table.** They are never released one by one.
 * Destroying the table invalidates all of them.
 *
 * **Atoms of different tables are unrelated.** Equal strings interned in two
 * tables are two different pointers. Most programs keep one table for each
 * kind of name, or one for everything.
 *
 * **Memory.** A table gets all of its memory from the allocator it was created
 * with (allocator.h). Running out fails fast, as everywhere in the engine.
 * Atoms are not allocated one by one: they share blocks, the first of about a
 * kilobyte and each later one twice the size of the last, up to 64 KiB, so a
 * table with a few names takes little and a table with a million makes few
 * allocations. An atom much longer than the others gets a block of its own.
 *
 * **A shared table can be used from every thread at once.** A table from
 * tb_atom_table_create_shared() synchronizes itself, so that modules and
 * worker threads can intern into one table of names:
 *
 * @code
 * tb_atom_table *const names = tb_atom_table_create_shared(NULL);
 *
 * // On any thread, at any time:
 * const char *const name = tb_atom_string(names, "r_fov");
 * @endcode
 *
 * Equal bytes give the same pointer whichever threads intern them, and
 * however many do so at the same moment. Finding an atom that exists takes
 * no lock, so threads that look up names that are already there do not wait
 * for one another; only a name that is new takes the table's lock while it
 * is added. A shared table keeps the arrays that it outgrows until it is
 * destroyed, which costs it about twice the memory for slots of a table that
 * is not shared.
 *
 * @par Threading
 * A table from tb_atom_table_create() is externally synchronized: the caller
 * serializes every call that names it. A table from
 * tb_atom_table_create_shared() is thread-safe, except that creating and
 * destroying it are for one thread, with no other thread using the table.
 * Using atoms that already exist (comparing them, reading their bytes,
 * tb_atom_length(), tb_atom_hash()) needs no synchronization in either.
 */
#ifndef TB_ATOM_H
#define TB_ATOM_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief A table of atoms. Created by tb_atom_table_create(); owned by the
 * caller; released by tb_atom_table_destroy().
 */
typedef struct tb_atom_table tb_atom_table;

/**
 * @brief Create an empty table.
 * @param allocator Where the table and its atoms get their memory, or NULL
 * for the engine's heap. It is copied.
 * @return The table; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_atom_table *tb_atom_table_create(const tb_allocator *allocator);

/**
 * @brief Create an empty table that any number of threads may use at once.
 *
 * Every function that takes a table works on it, with the same results.
 *
 * @param allocator Where the table and its atoms get their memory, or NULL
 * for the engine's heap. It is copied. The table calls it only while it
 * holds its own lock, so an allocator that nothing else uses need not be
 * thread-safe.
 * @return The table; never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 * @par Threading Thread-safe; tables are independent of one another.
 */
TB_API tb_atom_table *tb_atom_table_create_shared(const tb_allocator *allocator);

/**
 * @brief Destroy a table and every atom in it.
 * @param table Table to destroy; NULL is ignored. Every atom it returned is
 * invalid afterwards.
 */
TB_API void tb_atom_table_destroy(tb_atom_table *table);

/** @brief Number of distinct atoms in a table. @param table Required non-null. */
TB_API size_t tb_atom_table_count(const tb_atom_table *table);

/**
 * @brief Intern a range of bytes.
 * @param table Required non-null.
 * @param bytes The bytes, which are copied when they are new to the table;
 * may be NULL only when `length` is 0.
 * @param length Number of bytes. They may include zero bytes.
 * @return The atom for those bytes: the same pointer every time the same
 * bytes are interned in this table. Never NULL.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API const char *tb_atom_new(tb_atom_table *table, const char *bytes, size_t length);

/**
 * @brief Intern a NUL-terminated string.
 * @param table Required non-null.
 * @param string Required non-null.
 * @return The atom, as from tb_atom_new() with the length of the string.
 */
TB_API const char *tb_atom_string(tb_atom_table *table, const char *string);

/**
 * @brief Intern the decimal form of an integer, such as `"-42"`.
 * @param table Required non-null.
 * @param value Any value.
 * @return The atom, the same as tb_atom_string() gives for the digits.
 */
TB_API const char *tb_atom_int(tb_atom_table *table, long long value);

/**
 * @brief Look for an atom without creating it.
 * @param table Required non-null.
 * @param bytes The bytes to look for; may be NULL only when `length` is 0.
 * @param length Number of bytes.
 * @return The atom for those bytes, or NULL when they were never interned in
 * this table. The table is not changed.
 */
TB_API const char *tb_atom_find(const tb_atom_table *table, const char *bytes, size_t length);

/**
 * @brief Number of bytes in an atom, not counting its terminator.
 * @param atom An atom from tb_atom_new(), tb_atom_string(), tb_atom_int(), or
 * tb_atom_find(); required non-null. Any other pointer is a contract
 * violation that cannot be detected.
 * @return The length it was interned with, in constant time.
 */
TB_API size_t tb_atom_length(const char *atom);

/**
 * @brief The hash of an atom's bytes, as tb_hash_bytes() (hash.h) computes it.
 * @param atom An atom, as for tb_atom_length().
 * @return The hash, in constant time: it was computed when the atom was made.
 */
TB_API uint64_t tb_atom_hash(const char *atom);

#endif
