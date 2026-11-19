/** @file zip.h
 * @brief ZIP archives: opened from a stream that reads at offsets, entries
 * found by index or name and read as streams, and written in order to any
 * stream, stored or deflated, with ZIP64 when it is needed.
 *
 * ZIP is the interchange format of tools and of content people make. The
 * engine's own packages are another format.
 *
 * **Reading.** tb_zip_open() reads an archive's central directory once, and
 * checks all of it. The archive then lists its entries, finds one by name,
 * and opens any entry as a stream that decompresses it and checks its CRC-32.
 * Entries are read at their offsets, never in order, so the source must be
 * able to report its size and read at an offset (a file or memory stream can),
 * and several entries can be read at once, from as many threads.
 *
 * **The archive is untrusted.** Every offset and size is checked against the
 * archive before it is used:
 * - Entries may not overlap one another or the central directory.
 * - A local header must agree with its central record.
 * - A deflated entry may not claim more than DEFLATE can expand to.
 * - An entry's stream ends at its stated size, and is MALFORMED if the data
 *   holds more or less, or a CRC-32 that differs.
 * - Entry counts are limited, and duplicate names are refused unless allowed.
 *
 * Names are reported as the archive has them, converted to UTF-8 if the
 * archive marks them as code page 437. They may climb out of a directory or
 * be absolute; a caller that makes files of them joins them with
 * tb_path_rooted_join(), which refuses those.
 *
 * Not read, and reported UNSUPPORTED when an entry is opened: encrypted
 * entries and methods other than stored and deflated. Archives that span
 * several volumes are UNSUPPORTED to open.
 *
 * **Writing.** A writer writes each entry as a local header, its data, and a
 * data descriptor with the CRC-32 and sizes, then the central directory when
 * it finishes, so it never seeks. Names are flagged as UTF-8; Unix
 * permissions and the time in UTC are kept, and nothing of the host is
 * written: the same entries make the same bytes.
 *
 * @par Threading
 * An open archive does not change: tb_zip_count(), tb_zip_entry_at(), and
 * tb_zip_find() are thread-safe, and tb_zip_open_entry() is thread-safe when
 * the source's reads at an offset are (those of files and memory are). Each
 * entry stream, and each writer, is externally synchronized.
 * tb_zip_result_name() is thread-safe.
 */
#ifndef TB_ZIP_H
#define TB_ZIP_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What happened, or why not. */
typedef enum tb_zip_result
{
    TB_ZIP_OK,             /**< Success. */
    TB_ZIP_INVALID,        /**< An argument is malformed, such as an entry a writer refuses. */
    TB_ZIP_MALFORMED,      /**< The archive is damaged, inconsistent, or not a ZIP archive. */
    TB_ZIP_TOO_LARGE,      /**< The archive has more entries than the limit. */
    TB_ZIP_UNSUPPORTED,    /**< Valid, but needs what this reader lacks; see the file comment. */
    TB_ZIP_DUPLICATE_NAME, /**< Two entries have one name, which is refused unless allowed. */
    TB_ZIP_INPUT_FAILED,   /**< The source stream failed. */
    TB_ZIP_OUTPUT_FAILED   /**< The sink stream refused what it was given. */
} tb_zip_result;

/** @brief What an entry is. */
typedef enum tb_zip_type
{
    TB_ZIP_FILE,      /**< A file. */
    TB_ZIP_DIRECTORY, /**< A directory: its name ended in `/`, or its Unix mode says so. */
    TB_ZIP_SYMLINK    /**< A symbolic link, by its Unix mode; its data is the target. */
} tb_zip_type;

/** @brief How an entry's data is compressed. */
typedef enum tb_zip_method
{
    TB_ZIP_STORED = 0,  /**< Not compressed. */
    TB_ZIP_DEFLATED = 8 /**< DEFLATE (badger/deflate.h). */
} tb_zip_method;

/** @brief One entry of an archive. */
typedef struct tb_zip_entry
{
    /** Its path, with `/` between components and without a trailing `/`.
     * Not empty. An archive's ends in a NUL that `name_length` does not
     * count, and holds no other NUL. */
    const char *name;
    size_t name_length; /**< Bytes of `name`. */
    tb_zip_type type;   /**< What the entry is. */
    uint16_t method;    /**< How it is compressed: a tb_zip_method, or another the archive names. */
    bool encrypted;     /**< Its data is encrypted, and cannot be read here. */
    uint64_t size;      /**< Bytes of its data. */
    uint64_t compressed_size; /**< Bytes it takes in the archive. Read only. */
    uint32_t crc32;           /**< CRC-32 of its data. Read only. */
    uint32_t mode;            /**< Unix permissions, 07777 at most; 0 when the archive has none. */
    /** Time of last change, in seconds since 1970 in UTC: from the extended
     * timestamp when there is one, or else the DOS time read as UTC. */
    int64_t modified;
} tb_zip_entry;

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/** @brief Relaxations for reading. Off by default. */
typedef enum tb_zip_flags
{
    /** Open archives with duplicate names; tb_zip_find() finds the last. */
    TB_ZIP_ALLOW_DUPLICATE_NAMES = 1
} tb_zip_flags;

/** @brief Entries an archive may have when the options give no limit. */
#define TB_ZIP_DEFAULT_MAX_ENTRIES ((size_t)1 << 20)

/** @brief How to open. Zero-initialize for the defaults. */
typedef struct tb_zip_options
{
    unsigned flags;     /**< A combination of tb_zip_flags. */
    size_t max_entries; /**< Entries of the archive; 0 for the default. */
} tb_zip_options;

/** @brief An open archive. Opaque. */
typedef struct tb_zip_archive tb_zip_archive;

/**
 * @brief Open an archive: read and check its central directory.
 *
 * The end record is found at the end of the source, after a comment of any
 * length up to 65,535 bytes that runs to the end. Data before the archive or
 * after its end record, as self-extracting archives have, is MALFORMED.
 * @param source Required open stream that can report its size and read at an
 * offset; borrowed, and must stay open, and not change, until the archive is
 * closed.
 * @param options Optional; NULL for the defaults.
 * @param allocator Where the archive lives, or NULL for the heap.
 * @param[out] archive Required; the archive on OK, for tb_zip_close(); NULL otherwise.
 * @return OK; UNSUPPORTED if the source cannot report its size or read at an
 * offset, or the archive spans volumes; MALFORMED, TOO_LARGE,
 * DUPLICATE_NAME, or INPUT_FAILED.
 * @par Errors Running out of memory ends the process. Unknown flags are a
 * contract violation.
 * @par Threading Thread-safe; the archive is the caller's.
 */
TB_API tb_zip_result tb_zip_open(tb_stream *source, const tb_zip_options *options,
                                 const tb_allocator *allocator, tb_zip_archive **archive);

/** @brief Close an archive. Its entry streams must be closed first. @param archive Optional. */
TB_API void tb_zip_close(tb_zip_archive *archive);

/** @brief The number of entries. @param archive Required. */
TB_API size_t tb_zip_count(const tb_zip_archive *archive);

/**
 * @brief Describe an entry.
 * @param archive Required.
 * @param index Less than tb_zip_count(); entries are in the central directory's order.
 * @param[out] entry Required. Its name is the archive's, valid until it is closed.
 */
TB_API void tb_zip_entry_at(const tb_zip_archive *archive, size_t index, tb_zip_entry *entry);

/**
 * @brief Find an entry by its name, as tb_zip_entry_at() reports it: bytes
 * compared exactly, without a trailing `/`.
 * @param archive Required.
 * @param name The name, NULL only at length zero.
 * @param length Bytes of `name`.
 * @param[out] index Required; the entry's index when found.
 * @return Whether it was found.
 */
TB_API bool tb_zip_find(const tb_zip_archive *archive, const char *name, size_t length,
                        size_t *index);

/**
 * @brief Open a stream of an entry's data, decompressed.
 *
 * The stream ends after the entry's size. Data that is not a valid stream of
 * its method, that ends early or runs long, or whose CRC-32 differs, is read as
 * TB_STREAM_MALFORMED, at the latest when the end is reached.
 * @param archive Required; must outlive the stream.
 * @param index Less than tb_zip_count().
 * @param allocator Where the stream's state lives, or NULL for the heap.
 * @param[out] stream Required zero stream; open on OK, zero otherwise.
 * @return OK; UNSUPPORTED for an encrypted entry or another method; MALFORMED
 * if its local header is damaged, disagrees with the central directory, or
 * puts its data where it may not be; or INPUT_FAILED.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_zip_result tb_zip_open_entry(tb_zip_archive *archive, size_t index,
                                       const tb_allocator *allocator, tb_stream *stream);

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

/** @brief A writer of one archive. Opaque. */
typedef struct tb_zip_writer tb_zip_writer;

/**
 * @brief Create a writer of an archive.
 * @param sink Required open stream that can write, at the archive's first
 * byte; borrowed, and must stay open until the writer is destroyed.
 * @param allocator Where the writer lives, or NULL for the heap.
 * @return The writer, for tb_zip_writer_destroy().
 * @par Errors Running out of memory ends the process.
 * @par Threading Thread-safe; the writer is the caller's.
 */
TB_API tb_zip_writer *tb_zip_writer_create(tb_stream *sink, const tb_allocator *allocator);

/** @brief Destroy a writer, finished or not. @param writer Optional. */
TB_API void tb_zip_writer_destroy(tb_zip_writer *writer);

/**
 * @brief Begin an entry. A file's `size` bytes of data follow, through
 * tb_zip_write_data(); a directory has none.
 * @param writer Required, with the data of any earlier entry complete.
 * @param entry Required:
 * - `name`: a relative path of UTF-8, components between single slashes, no
 *   `.` or `..` component, no NUL, and no `/` at either end; not the name of
 *   an earlier entry. A directory's is written with a `/` after it.
 * - `type`: TB_ZIP_FILE or TB_ZIP_DIRECTORY.
 * - `method`: TB_ZIP_STORED or TB_ZIP_DEFLATED; a directory is stored.
 * - `size`: for a file; 0 for a directory.
 * - `mode`: 07777 at most, or 0 for 0644 for a file and 0755 for a directory.
 * - `modified`: any time; DOS times hold 1980 to 2107, and others are written
 *   as the nearest of those, with the exact time beside them while it fits 32 bits.
 * - `encrypted`, `compressed_size`, and `crc32` are ignored.
 * @param level For a deflated file, from 1 (fastest) to 9 (smallest), as in badger/deflate.h.
 * @return OK; INVALID, with nothing written, for an entry that breaks those
 * rules; or OUTPUT_FAILED, which still begins the entry: its data is owed as
 * usual, and refused in turn.
 * @par Errors Beginning an entry before the last one's data is complete, or
 * after the finish, is a contract violation.
 */
TB_API tb_zip_result tb_zip_write_entry(tb_zip_writer *writer, const tb_zip_entry *entry,
                                        int level);

/**
 * @brief Write some of the current file's data.
 * @param writer Required, at a file.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes to write; no more than the file has left.
 * @return OK, or OUTPUT_FAILED. A failure stays: every later call returns it.
 * @par Errors More data than the entry's size is a contract violation.
 */
TB_API tb_zip_result tb_zip_write_data(tb_zip_writer *writer, const void *bytes, size_t length);

/**
 * @brief Finish the archive: write its central directory and end records, and
 * flush the sink. Nothing may be written after.
 * @param writer Required, with the last entry's data complete.
 * @return OK, or OUTPUT_FAILED, also for a failure earlier.
 * @par Errors Finishing with data still owed, or twice, is a contract violation.
 */
TB_API tb_zip_result tb_zip_writer_finish(tb_zip_writer *writer);

/** @brief A short name of a result, such as "malformed". Thread-safe. */
TB_API const char *tb_zip_result_name(tb_zip_result result);

#endif
