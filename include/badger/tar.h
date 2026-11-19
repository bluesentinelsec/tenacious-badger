/** @file tar.h
 * @brief TAR archives, read and written in order through streams: POSIX
 * ustar and pax, and GNU's long names.
 *
 * TAR is the archive of Linux exports, and it streams: neither reading nor
 * writing ever seeks, so an archive can come through a decompression stream
 * (`.tar.gz`, with badger/deflate.h) or go to one.
 *
 * **Reading.** A reader takes the archive's entries in order. Each call to
 * tb_tar_reader_next() describes one entry; its data is read with
 * tb_tar_reader_read(), or through a stream from tb_tar_reader_open_data(),
 * and whatever of it is not read is skipped by the next call. It reads:
 * - ustar headers, with names split between the prefix and name fields;
 * - pax extended headers, for paths and link targets of any length, sizes of
 *   8 GiB and more, and times out of the octal range; a pax global header
 *   is checked and its records are ignored;
 * - GNU long names and link targets, and GNU's base-256 numbers;
 * - the older headers without a magic, by their checksum.
 *
 * **The archive is untrusted.** Every header's checksum is checked, numeric
 * fields must be octal or base-256, extended headers have limits, and every
 * damaged archive is a result, never undefined behavior. Names and link
 * targets are reported as the archive has them, bytes and all: they may climb
 * out of a directory (`../`), be absolute, or not be UTF-8. A caller that
 * makes files of them joins them with tb_path_rooted_join(), which refuses
 * all of those. Links are reported and never followed.
 *
 * **Writing.** A writer writes entries in order, each a header and then
 * exactly its data, and finishes with the two zero blocks that end an
 * archive. Names that fit the ustar fields are written there, split at a
 * slash if they must be; longer names, link targets, sizes, and times go in
 * a pax header, so any reader of pax reads them all. The writer writes the
 * permissions and time it is given, owner and group 0 with no names, and
 * nothing else of the host: the same entries make the same bytes.
 *
 * @par Threading
 * A reader or a writer is externally synchronized; different ones are
 * independent. tb_tar_result_name() is thread-safe.
 */
#ifndef TB_TAR_H
#define TB_TAR_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

/** @brief What happened, or why not. */
typedef enum tb_tar_result
{
    TB_TAR_OK,           /**< Success. */
    TB_TAR_END,          /**< Not a failure: the archive has no more entries. */
    TB_TAR_INVALID,      /**< An argument is malformed, such as an entry a writer refuses. */
    TB_TAR_MALFORMED,    /**< The archive is damaged or not a TAR archive. */
    TB_TAR_TRUNCATED,    /**< The source ended inside the archive, or before its end blocks. */
    TB_TAR_TOO_LARGE,    /**< A name or an extended header is longer than the reader's limit. */
    TB_TAR_UNSUPPORTED,  /**< A valid entry this reader cannot read: sparse or multi-volume. */
    TB_TAR_INPUT_FAILED, /**< The source stream failed. */
    TB_TAR_OUTPUT_FAILED /**< The sink stream refused what it was given. */
} tb_tar_result;

/** @brief What an entry is. */
typedef enum tb_tar_type
{
    TB_TAR_FILE,      /**< A regular file; its data follows. */
    TB_TAR_DIRECTORY, /**< A directory. */
    TB_TAR_SYMLINK,   /**< A symbolic link to `link`. */
    TB_TAR_HARDLINK,  /**< Another name for the earlier entry `link`. */
    TB_TAR_OTHER      /**< Anything else, such as a device or a FIFO; see `flag`. */
} tb_tar_type;

/** @brief One entry of an archive. */
typedef struct tb_tar_entry
{
    /** Its path, with `/` between components and without a trailing `/`,
     * even for a directory. Not empty. A reader's entry ends in a NUL that
     * `name_length` does not count, and holds no other NUL. */
    const char *name;
    size_t name_length; /**< Bytes of `name`. */
    /** For a link, its target; otherwise empty. A reader's ends in a NUL. */
    const char *link;
    size_t link_length; /**< Bytes of `link`. */
    tb_tar_type type;   /**< What the entry is. */
    char flag;          /**< The header's type flag as it is, such as '0' or '5'. Read only. */
    uint64_t size;      /**< Bytes of data that follow the header. */
    uint32_t mode;      /**< Permissions and the set-id and sticky bits: 07777 at most. */
    int64_t modified;   /**< Time of last change, in seconds since 1970 in UTC. */
} tb_tar_entry;

/* -------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

/** @brief Relaxations for reading. Both are off by default. */
typedef enum tb_tar_flags
{
    /** An archive that ends at a header without its two zero blocks, as some
     * writers leave one, ends there. Without this, that is TRUNCATED: a
     * source cut short at an entry's edge looks just the same. */
    TB_TAR_ALLOW_MISSING_END = 1
} tb_tar_flags;

/** @brief Longest name or link target a reader takes when the options give none: 64 KiB. */
#define TB_TAR_DEFAULT_MAX_NAME ((size_t)64 << 10)

/** @brief Longest extended header a reader takes when the options give none: 1 MiB. */
#define TB_TAR_DEFAULT_MAX_EXTENDED ((size_t)1 << 20)

/** @brief How to read. Zero-initialize for the defaults. */
typedef struct tb_tar_options
{
    unsigned flags;      /**< A combination of tb_tar_flags. */
    size_t max_name;     /**< Bytes of a name or a link target; 0 for the default. */
    size_t max_extended; /**< Bytes of one pax or GNU extended header; 0 for the default. */
} tb_tar_options;

/** @brief A reader of one archive. Opaque. */
typedef struct tb_tar_reader tb_tar_reader;

/**
 * @brief Create a reader of an archive.
 * @param source Required open stream that can read, from the archive's first
 * byte; borrowed, and must stay open until the reader is destroyed. Only the
 * archive's bytes are read from it, up to the end of its second zero block.
 * @param options Optional; NULL for the defaults.
 * @param allocator Where the reader lives, or NULL for the heap.
 * @return The reader, for tb_tar_reader_destroy().
 * @par Errors Running out of memory, here or for names and extended headers
 * later, ends the process. Unknown flags are a contract violation.
 * @par Threading Thread-safe; the reader is the caller's.
 */
TB_API tb_tar_reader *tb_tar_reader_create(tb_stream *source, const tb_tar_options *options,
                                           const tb_allocator *allocator);

/** @brief Destroy a reader. @param reader Optional; NULL does nothing. */
TB_API void tb_tar_reader_destroy(tb_tar_reader *reader);

/**
 * @brief Move to the next entry, skipping whatever of the current entry's data was not read.
 * @param reader Required.
 * @param[out] entry Required; the entry on OK, zero otherwise. Its name and
 * link are the reader's, valid until the next call to this or until the
 * reader is destroyed.
 * @return OK; END after the last entry; or why not. END and every failure
 * stay: each later call returns them again.
 */
TB_API tb_tar_result tb_tar_reader_next(tb_tar_reader *reader, tb_tar_entry *entry);

/**
 * @brief Read the current entry's data.
 * @param reader Required, at an entry.
 * @param bytes Output, NULL only at capacity zero.
 * @param capacity Bytes wanted.
 * @param[out] transferred Required; bytes read, even on failure. Zero with OK
 * is the end of the data.
 * @return OK, or why not: TRUNCATED if the source ends first, INPUT_FAILED if
 * it fails. A failure stays, as for tb_tar_reader_next().
 * @par Errors Reading before the first entry or after END is a contract violation.
 */
TB_API tb_tar_result tb_tar_reader_read(tb_tar_reader *reader, void *bytes, size_t capacity,
                                        size_t *transferred);

/**
 * @brief Open a stream of the current entry's data, for code that reads streams.
 *
 * The stream reads what tb_tar_reader_read() reads, and ends with the data.
 * After the reader moves to another entry, reading it is TB_STREAM_INVALID.
 * A truncated archive is TB_STREAM_END, a failed source is the source's own
 * result, and any other failure of the reader is TB_STREAM_MALFORMED.
 * @param reader Required, at an entry; borrowed, and must outlive the stream.
 * @param[out] stream Required zero stream; open on return.
 * @par Errors Running out of memory for the stream ends the process. Opening
 * before the first entry or after END is a contract violation.
 * @par Threading The stream is externally synchronized together with its reader.
 */
TB_API void tb_tar_reader_open_data(tb_tar_reader *reader, tb_stream *stream);

/**
 * @brief Where the reader is in the archive: after a failure, the offset of
 * the header or data where it failed; otherwise, of the current entry's header.
 * @param reader Required.
 */
TB_API uint64_t tb_tar_reader_offset(const tb_tar_reader *reader);

/* -------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

/** @brief A writer of one archive. Opaque. */
typedef struct tb_tar_writer tb_tar_writer;

/**
 * @brief Create a writer of an archive.
 * @param sink Required open stream that can write; borrowed, and must stay
 * open until the writer is destroyed.
 * @param allocator Where the writer lives, or NULL for the heap.
 * @return The writer, for tb_tar_writer_destroy().
 * @par Errors Running out of memory ends the process.
 * @par Threading Thread-safe; the writer is the caller's.
 */
TB_API tb_tar_writer *tb_tar_writer_create(tb_stream *sink, const tb_allocator *allocator);

/**
 * @brief Destroy a writer, finished or not; an unfinished archive is left
 * as far as it was written. @param writer Optional; NULL does nothing.
 */
TB_API void tb_tar_writer_destroy(tb_tar_writer *writer);

/**
 * @brief Begin an entry: write its header. A file's `size` bytes of data
 * follow, through tb_tar_write_data(); other entries have none.
 * @param writer Required, with the data of any earlier entry complete.
 * @param entry Required:
 * - `name`: a relative path of UTF-8, components between single slashes, no
 *   `.` or `..` component, no NUL, and no `/` at either end. A directory's
 *   is written with a `/` after it.
 * - `link`: the target, for a link: UTF-8 without NUL, not empty; for any
 *   other type, empty.
 * - `type`: any but TB_TAR_OTHER. `flag` is ignored.
 * - `size`: for a file; 0 for any other type.
 * - `mode`: 07777 at most, or 0 for the usual: 0644 for a file or a hard
 *   link, 0755 for a directory, 0777 for a symbolic link.
 * - `modified`: any time.
 * @return OK; INVALID, with nothing written, for an entry that breaks those
 * rules; or OUTPUT_FAILED, which still begins the entry: its data is owed as
 * usual, and refused in turn.
 * @par Errors Beginning an entry before the last one's data is complete, or
 * after the finish, is a contract violation.
 */
TB_API tb_tar_result tb_tar_write_entry(tb_tar_writer *writer, const tb_tar_entry *entry);

/**
 * @brief Write some of the current file's data.
 * @param writer Required, at a file.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes to write; no more than the file has left.
 * @return OK, or OUTPUT_FAILED. A failure stays: every later call returns it.
 * @par Errors More data than the entry's size is a contract violation.
 */
TB_API tb_tar_result tb_tar_write_data(tb_tar_writer *writer, const void *bytes, size_t length);

/**
 * @brief Finish the archive: complete the last entry's block, write the two
 * zero blocks, and flush the sink. Nothing may be written after.
 * @param writer Required, with the last entry's data complete.
 * @return OK, or OUTPUT_FAILED, also for a failure earlier.
 * @par Errors Finishing with data still owed, or twice, is a contract violation.
 */
TB_API tb_tar_result tb_tar_writer_finish(tb_tar_writer *writer);

/** @brief A short name of a result, such as "malformed". Thread-safe. */
TB_API const char *tb_tar_result_name(tb_tar_result result);

#endif
