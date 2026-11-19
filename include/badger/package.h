/** @file package.h
 * @brief Packages: the engine's own archive of cooked assets, compressed
 * and optionally obfuscated, read at offsets from many threads at once.
 *
 * ZIP and TAR are for exchange with other tools. A package is for shipping:
 * each entry is compressed on its own (or stored, when compressing would not
 * make it smaller), the index is found by name in O(log n), and any entry can
 * be read while others are, from as many threads as the source allows.
 *
 * **Obfuscation.** A package written with a key is obfuscated whole, its
 * index and every entry, with ChaCha20 (badger/chacha20.h). Each package
 * draws a random salt, from which its own key is derived from the game's
 * (HMAC-SHA256), so no two packages share a keystream. A package records a
 * check of its key, so a wrong key is told from a damaged package.
 * **Obfuscation is not security**: the key ships with the game, and
 * tb_add_package_key (docs/packages.md) only keeps it out of plain sight.
 *
 * **The package is untrusted.** Opening a package checks all of its index
 * before any entry is read: every entry must lie within the data, overlap no
 * other, have a name that is a relative path, and claim no more than its
 * codec can produce; and the index has a CRC-32. An entry's stream ends at
 * its size and checks its CRC-32 there.
 *
 * Layout, little-endian (see docs/packages.md):
 * - a header of 64 bytes: magic, version, flags, salt, key check;
 * - each entry's data, from an offset that is a multiple of 16;
 * - the index: a record per entry, sorted by name;
 * - a footer of 48 bytes: where the index is, its length, its CRC-32, and
 *   the count of entries.
 *
 * @par Threading
 * An open package does not change: tb_package_count(),
 * tb_package_entry_at(), and tb_package_find() are thread-safe, and
 * tb_package_open_entry() is thread-safe when the source's reads at an
 * offset are. Entry streams and writers are externally synchronized.
 * tb_package_result_name() is thread-safe.
 */
#ifndef TB_PACKAGE_H
#define TB_PACKAGE_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What happened, or why not. */
typedef enum tb_package_result
{
    TB_PACKAGE_OK,           /**< Success. */
    TB_PACKAGE_INVALID,      /**< An argument is malformed, such as a name a writer refuses. */
    TB_PACKAGE_MALFORMED,    /**< The package is damaged or not a package. */
    TB_PACKAGE_UNSUPPORTED,  /**< A newer version, unknown flags, or a source that cannot read at an
                                offset. */
    TB_PACKAGE_TOO_LARGE,    /**< More entries than the limit. */
    TB_PACKAGE_KEY_REQUIRED, /**< The package is obfuscated, and no key was given. */
    TB_PACKAGE_WRONG_KEY,    /**< The key given is not the package's. */
    TB_PACKAGE_DUPLICATE_NAME, /**< A writer was given a name it already has. */
    TB_PACKAGE_RANDOM_FAILED,  /**< The system gave no random bytes for a salt. */
    TB_PACKAGE_INPUT_FAILED,   /**< The source stream failed. */
    TB_PACKAGE_OUTPUT_FAILED   /**< The sink stream refused what it was given. */
} tb_package_result;

/** @brief Bytes of a key: the game's, as tb_add_package_key gives it. */
#define TB_PACKAGE_KEY_SIZE 32u

/** @brief How an entry is stored. */
typedef enum tb_package_codec
{
    TB_PACKAGE_STORED = 0,  /**< As it is. */
    TB_PACKAGE_DEFLATED = 1 /**< Raw DEFLATE (badger/deflate.h). */
} tb_package_codec;

/** @brief One entry of a package. */
typedef struct tb_package_entry
{
    /** Its name: a relative path of UTF-8, components between single slashes.
     * Ends in a NUL that `name_length` does not count. */
    const char *name;
    size_t name_length;     /**< Bytes of `name`. */
    tb_package_codec codec; /**< How it is stored. */
    uint64_t size;          /**< Bytes of its data. */
    uint64_t stored_size;   /**< Bytes it takes in the package. */
    uint32_t crc32;         /**< CRC-32 of its data. */
} tb_package_entry;

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/** @brief Entries a package may have when the options give no limit. */
#define TB_PACKAGE_DEFAULT_MAX_ENTRIES ((size_t)1 << 22)

/** @brief How to open. Zero-initialize for the defaults. */
typedef struct tb_package_options
{
    size_t max_entries; /**< Entries of the package; 0 for the default. */
} tb_package_options;

/** @brief An open package. Opaque. */
typedef struct tb_package tb_package;

/**
 * @brief Open a package: read and check its header, footer, and index.
 * @param source Required open stream that can report its size and read at
 * an offset; borrowed, and must stay open, and not change, until the package
 * is closed.
 * @param key TB_PACKAGE_KEY_SIZE bytes, or NULL for a package that is not
 * obfuscated. Copied. A key given for a package that is not obfuscated is
 * not used.
 * @param options Optional; NULL for the defaults.
 * @param allocator Where the package lives, or NULL for the heap.
 * @param[out] package Required; the package on OK, for tb_package_close(); NULL otherwise.
 * @return OK; KEY_REQUIRED, WRONG_KEY, UNSUPPORTED, MALFORMED, TOO_LARGE, or INPUT_FAILED.
 * @par Errors Running out of memory ends the process.
 * @par Threading Thread-safe; the package is the caller's.
 */
TB_API tb_package_result tb_package_open(tb_stream *source, const uint8_t *key,
                                         const tb_package_options *options,
                                         const tb_allocator *allocator, tb_package **package);

/** @brief Close a package and clear its key. Close its entry streams first. @param package
 * Optional. */
TB_API void tb_package_close(tb_package *package);

/** @brief The number of entries. @param package Required. */
TB_API size_t tb_package_count(const tb_package *package);

/**
 * @brief Describe an entry. Entries are in the order of their names.
 * @param package Required. @param index Less than tb_package_count().
 * @param[out] entry Required. Its name is the package's, valid until it is closed.
 */
TB_API void tb_package_entry_at(const tb_package *package, size_t index, tb_package_entry *entry);

/**
 * @brief Find an entry by its name, compared byte for byte.
 * @param package Required. @param name The name, NULL only at length zero.
 * @param length Bytes of `name`. @param[out] index Required; the entry's index when found.
 * @return Whether it was found.
 */
TB_API bool tb_package_find(const tb_package *package, const char *name, size_t length,
                            size_t *index);

/**
 * @brief Open a stream of an entry's data, restored and decompressed. It ends
 * after the entry's size; data that does not decode, runs short or long, or
 * whose CRC-32 differs, is read as TB_STREAM_MALFORMED, at the latest at the end.
 * @param package Required; must outlive the stream.
 * @param index Less than tb_package_count().
 * @param allocator Where the stream's state lives, or NULL for the heap.
 * @param[out] stream Required zero stream; open on return.
 * @par Errors Running out of memory ends the process.
 */
TB_API void tb_package_open_entry(tb_package *package, size_t index, const tb_allocator *allocator,
                                  tb_stream *stream);

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

/** @brief A writer of one package. Opaque. */
typedef struct tb_package_writer tb_package_writer;

/**
 * @brief Create a writer, and write the package's header.
 * @param sink Required open stream that can write, at the package's first
 * byte; borrowed, and must stay open until the writer is destroyed.
 * @param key TB_PACKAGE_KEY_SIZE bytes to obfuscate with, or NULL for a
 * package that is not obfuscated. Copied.
 * @param allocator Where the writer lives, or NULL for the heap.
 * @param[out] writer Required; the writer on OK, for tb_package_writer_destroy(); NULL otherwise.
 * @return OK; RANDOM_FAILED if a key was given and the system gave no salt;
 * or OUTPUT_FAILED.
 * @par Errors Running out of memory ends the process.
 * @par Threading Thread-safe; the writer is the caller's.
 */
TB_API tb_package_result tb_package_writer_create(tb_stream *sink, const uint8_t *key,
                                                  const tb_allocator *allocator,
                                                  tb_package_writer **writer);

/** @brief Destroy a writer, finished or not, and clear its key. @param writer Optional. */
TB_API void tb_package_writer_destroy(tb_package_writer *writer);

/**
 * @brief Add an entry, whole.
 *
 * At a level from 1 to 9 the data is deflated, and stored instead if that
 * would not make it smaller; at level 0 it is stored.
 * @param writer Required, not finished.
 * @param name A relative path of UTF-8: components between single slashes,
 * no `.` or `..` component, no NUL, no `/` at either end, and at most 65,535 bytes.
 * @param name_length Bytes of `name`.
 * @param bytes The data, NULL only at size zero.
 * @param size Bytes of the data.
 * @param level From 0 to 9.
 * @return OK; INVALID, with nothing written, for a name or level the writer
 * refuses; DUPLICATE_NAME; or OUTPUT_FAILED, which stays.
 * @par Errors Running out of memory ends the process. Adding after the finish
 * is a contract violation.
 */
TB_API tb_package_result tb_package_add(tb_package_writer *writer, const char *name,
                                        size_t name_length, const void *bytes, size_t size,
                                        int level);

/**
 * @brief Finish the package: write its index and footer, and flush the sink.
 * @param writer Required, not finished.
 * @return OK, or OUTPUT_FAILED, also for a failure earlier.
 */
TB_API tb_package_result tb_package_writer_finish(tb_package_writer *writer);

/** @brief A short name of a result, such as "wrong key". Thread-safe. */
TB_API const char *tb_package_result_name(tb_package_result result);

#endif
