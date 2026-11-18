/** @file stream.h
 * @brief One interface for a source or a sink of bytes, whatever holds them.
 *
 * A stream is a value: a table of functions and the state they work on, as an
 * allocator is. The engine's codecs, archives, and virtual file system read
 * and write through it, so they can be stacked: a file, a range of it that is
 * one entry of a package, that entry decompressed. Anyone can implement a
 * stream by filling in a tb_stream_functions table.
 * @code
 * tb_stream file = {0};
 * tb_stream entry = {0};
 * unsigned char header[16] = {0};
 * if (tb_stream_open_file(path, TB_FILE_READ, NULL, &file) != TB_FS_OK) { ... }
 * if (tb_stream_open_range(&file, entry_offset, entry_size, NULL, &entry) != TB_STREAM_OK) { ... }
 * if (tb_stream_read_exact(&entry, header, sizeof(header)) != TB_STREAM_OK) { ... }
 * tb_stream_close(&entry);
 * tb_stream_close(&file);
 * @endcode
 *
 * **A stream offers what it can do.** Each operation of the table may be
 * absent; asking a stream for one it lacks returns TB_STREAM_UNSUPPORTED. A
 * memory stream cannot be written, a decompressor cannot seek, and neither is
 * an error until someone asks.
 *
 * **Transfers can be short.** A read returns at least one byte unless it is at
 * the end or fails, and zero bytes at the end is success, as for files. A
 * write transfers at least one byte or fails. tb_stream_read_exact() and
 * tb_stream_write_all() loop for callers that want the whole length.
 *
 * **Positions are 64-bit**, from zero to INT64_MAX, so a stream is as large as
 * a file can be. A memory stream is limited by memory.
 *
 * **Every stream is closed once**, with tb_stream_close(), by whoever opened
 * it. A stream that is built on another borrows it: the other must stay open
 * until this one is closed.
 *
 * @par Threading
 * A stream is externally synchronized: the caller serializes every call that
 * names it. tb_stream_read_at() is the exception for the streams this header
 * makes from memory, files, and ranges: reads at an offset run together, from
 * any number of threads, as long as nothing writes, seeks, or closes the stream
 * meanwhile. Each other implementation says whether its reads at an offset do.
 * Different streams are independent, except that a range uses its parent.
 */
#ifndef TB_STREAM_H
#define TB_STREAM_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/file.h"

#include <stddef.h>
#include <stdint.h>

/** @brief Why a stream operation failed. */
typedef enum tb_stream_result
{
    TB_STREAM_OK,          /**< Success. */
    TB_STREAM_INVALID,     /**< An argument is malformed: an unknown origin, a negative position. */
    TB_STREAM_NO_MEMORY,   /**< Memory for the stream's data ran out. */
    TB_STREAM_TOO_LARGE,   /**< A position or a size is past what the stream can represent. */
    TB_STREAM_UNSUPPORTED, /**< The stream does not offer this operation. */
    TB_STREAM_END,         /**< The stream ended before the length that was required. */
    TB_STREAM_NO_SPACE,    /**< The sink is full: a disk, a quota, or a fixed limit. */
    TB_STREAM_MALFORMED,   /**< The bytes are not a valid encoding, as a codec reports. */
    TB_STREAM_ACCESS,      /**< The system refused the operation: permission, sharing. */
    TB_STREAM_OS_ERROR     /**< The operating system reported a failure no other result names. */
} tb_stream_result;

/** @brief Where a seek counts from. */
typedef enum tb_stream_origin
{
    TB_STREAM_FROM_START,   /**< The first byte. */
    TB_STREAM_FROM_CURRENT, /**< The current position. */
    TB_STREAM_FROM_END      /**< One past the last byte. */
} tb_stream_origin;

/**
 * @brief Read up to `capacity` bytes at the position, and move past them.
 * @param state The stream's state. @param bytes Output, NULL only at capacity zero.
 * @param capacity Bytes wanted. @param[out] transferred Bytes read, set even on failure.
 * @return Status. At the end, OK with zero bytes. Otherwise at least one byte, or a failure.
 */
typedef tb_stream_result tb_stream_read_function(void *state, void *bytes, size_t capacity,
                                                 size_t *transferred);
/**
 * @brief Write up to `length` bytes at the position, and move past them.
 * @param state The stream's state. @param bytes Input, NULL only at length zero.
 * @param length Bytes offered. @param[out] transferred Bytes written, set even on failure.
 * @return Status. At least one byte when `length` is not zero, or a failure.
 */
typedef tb_stream_result tb_stream_write_function(void *state, const void *bytes, size_t length,
                                                  size_t *transferred);
/**
 * @brief Move the position.
 * @param state The stream's state. @param offset Signed displacement from `origin`.
 * @param origin Where `offset` counts from; any other value is INVALID.
 * @param[out] position The new position on success; unchanged on failure.
 * @return Status. A resulting position below zero is INVALID; one past INT64_MAX is TOO_LARGE.
 */
typedef tb_stream_result tb_stream_seek_function(void *state, int64_t offset,
                                                 tb_stream_origin origin, uint64_t *position);
/**
 * @brief Report the size in bytes. @param state The stream's state.
 * @param[out] size The size on success; unchanged on failure. @return Status.
 */
typedef tb_stream_result tb_stream_size_function(void *state, uint64_t *size);
/**
 * @brief Read up to `capacity` bytes at `offset`, without moving the position.
 * @param state The stream's state. @param offset Position of the first byte.
 * @param bytes Output, NULL only at capacity zero. @param capacity Bytes wanted.
 * @param[out] transferred Bytes read, set even on failure.
 * @return Status, with the rules of tb_stream_read_function.
 */
typedef tb_stream_result tb_stream_read_at_function(void *state, uint64_t offset, void *bytes,
                                                    size_t capacity, size_t *transferred);
/**
 * @brief Pass written bytes on to where they are kept. @param state The stream's state.
 * @return Status.
 */
typedef tb_stream_result tb_stream_flush_function(void *state);
/**
 * @brief Release the stream's state and everything it owns. Called exactly once.
 * @param state The stream's state; not used again afterwards.
 * @return Status of releasing what the stream owns, such as closing a file.
 */
typedef tb_stream_result tb_stream_release_function(void *state);

/**
 * @brief What a kind of stream can do. One constant table serves every stream
 * of the kind. Any member except `release` may be NULL for an operation the kind
 * does not offer.
 */
typedef struct tb_stream_functions
{
    tb_stream_read_function *read_bytes;       /**< Read at the position, or NULL. */
    tb_stream_write_function *write_bytes;     /**< Write at the position, or NULL. */
    tb_stream_seek_function *seek;             /**< Move the position, or NULL. */
    tb_stream_size_function *size;             /**< Report the size, or NULL. */
    tb_stream_read_at_function *read_bytes_at; /**< Read at an offset, or NULL. */
    tb_stream_flush_function *flush;           /**< Flush written bytes, or NULL. */
    tb_stream_release_function *release;       /**< Release the state; required. */
} tb_stream_functions;

/**
 * @brief A stream: what it can do, and the state it does it to.
 * Zero is a closed stream. A stream has one owner; a copy of the value is the
 * same stream, not a new one, and must not be closed separately.
 */
typedef struct tb_stream
{
    const tb_stream_functions *functions; /**< The kind's table; NULL when closed. */
    void *state;                          /**< Passed to every function of the table. */
} tb_stream;

/* -------------------------------------------------------------------------
 * Operations on any stream.
 * ------------------------------------------------------------------------- */

/**
 * @brief Read up to `capacity` bytes, and move past them.
 * @param stream Required open stream.
 * @param bytes Output, NULL only at capacity zero.
 * @param capacity Bytes wanted.
 * @param[out] transferred Required; bytes read, even on failure.
 * @return Status. OK with zero bytes is the end. UNSUPPORTED if the stream cannot read.
 */
TB_API tb_stream_result tb_stream_read(tb_stream *stream, void *bytes, size_t capacity,
                                       size_t *transferred);

/**
 * @brief Write up to `length` bytes, and move past them.
 * @param stream Required open stream.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes offered.
 * @param[out] transferred Required; bytes written, even on failure.
 * @return Status. UNSUPPORTED if the stream cannot write.
 */
TB_API tb_stream_result tb_stream_write(tb_stream *stream, const void *bytes, size_t length,
                                        size_t *transferred);

/**
 * @brief Move the position. Moving past the end is allowed, and changes nothing
 * until a write there.
 * @param stream Required open stream.
 * @param offset Signed displacement from `origin`.
 * @param origin Where `offset` counts from.
 * @param[out] position Optional; the new position on success, unchanged on failure.
 * @return Status. UNSUPPORTED if the stream cannot seek.
 */
TB_API tb_stream_result tb_stream_seek(tb_stream *stream, int64_t offset, tb_stream_origin origin,
                                       uint64_t *position);

/**
 * @brief Report the position.
 * @param stream Required open stream.
 * @param[out] position Required; unchanged on failure.
 * @return Status. UNSUPPORTED if the stream cannot seek.
 */
TB_API tb_stream_result tb_stream_tell(tb_stream *stream, uint64_t *position);

/**
 * @brief Report the size in bytes.
 * @param stream Required open stream.
 * @param[out] size Required; unchanged on failure.
 * @return Status. UNSUPPORTED if the stream does not know its size.
 */
TB_API tb_stream_result tb_stream_size(tb_stream *stream, uint64_t *size);

/**
 * @brief Read up to `capacity` bytes at `offset`, without moving the position.
 * @param stream Required open stream.
 * @param offset Position of the first byte. At or past the end, OK with zero bytes.
 * @param bytes Output, NULL only at capacity zero.
 * @param capacity Bytes wanted.
 * @param[out] transferred Required; bytes read, even on failure.
 * @return Status. UNSUPPORTED if the stream cannot read at an offset.
 * @par Threading
 * Thread-safe with other reads at an offset for the streams of this header;
 * see the file comment.
 */
TB_API tb_stream_result tb_stream_read_at(tb_stream *stream, uint64_t offset, void *bytes,
                                          size_t capacity, size_t *transferred);

/**
 * @brief Pass written bytes on to where they are kept: a file's storage, or
 * the stream below a codec.
 * @param stream Required open stream.
 * @return Status. A stream that keeps nothing back succeeds.
 */
TB_API tb_stream_result tb_stream_flush(tb_stream *stream);

/**
 * @brief Close a stream and release its state, then zero it.
 * @param stream Required; a zero stream is accepted and closes nothing.
 * @return Status of releasing what the stream owns. The stream is closed even
 * on failure, and must not be closed again.
 */
TB_API tb_stream_result tb_stream_close(tb_stream *stream);

/**
 * @brief Read exactly `length` bytes, through as many reads as it takes.
 * @param stream Required open stream.
 * @param bytes Output, NULL only at length zero.
 * @param length Bytes required.
 * @return Status. END if the stream ends first. After a failure the bytes read
 * so far are in the output, and the position is past them.
 */
TB_API tb_stream_result tb_stream_read_exact(tb_stream *stream, void *bytes, size_t length);

/**
 * @brief Read exactly `length` bytes at `offset`, without moving the position.
 * @param stream Required open stream.
 * @param offset Position of the first byte.
 * @param bytes Output, NULL only at length zero.
 * @param length Bytes required.
 * @return Status. END if the stream ends first; TOO_LARGE if the range passes UINT64_MAX.
 * @par Threading
 * As tb_stream_read_at().
 */
TB_API tb_stream_result tb_stream_read_exact_at(tb_stream *stream, uint64_t offset, void *bytes,
                                                size_t length);

/**
 * @brief Write all `length` bytes, through as many writes as it takes.
 * @param stream Required open stream.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes to write.
 * @return Status. After a failure, an unknown prefix has been written.
 */
TB_API tb_stream_result tb_stream_write_all(tb_stream *stream, const void *bytes, size_t length);

/**
 * @brief Copy from one stream to another until the source ends, through a
 * buffer of the caller's.
 * @param source Required open stream that can read.
 * @param destination Required open stream that can write; not the source.
 * @param buffer Required scratch space.
 * @param capacity Bytes in `buffer`; not zero.
 * @param limit Most bytes to copy. A source with more fails with TOO_LARGE
 * after `limit` bytes, which stops a stream built to expand without bound.
 * Pass UINT64_MAX for no limit.
 * @param[out] copied Optional; bytes written to the destination, even on failure.
 * @return Status of the first read or write that failed, or TOO_LARGE.
 */
TB_API tb_stream_result tb_stream_copy(tb_stream *source, tb_stream *destination, void *buffer,
                                       size_t capacity, uint64_t limit, uint64_t *copied);

/**
 * @brief Name a result, for a log or a message: "unexpected end", "not supported".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe.
 */
TB_API const char *tb_stream_result_name(tb_stream_result result);

/* -------------------------------------------------------------------------
 * Streams over memory, files, and other streams.
 * ------------------------------------------------------------------------- */

/**
 * @brief Open a stream that reads borrowed bytes. It can read, seek, report its
 * size, and read at an offset; it cannot write.
 * @param bytes Borrowed; must stay valid and unchanged until the stream is
 * closed. NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @param allocator Where the stream's state is allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on return.
 * @par Errors Running out of memory for the state ends the process, as allocation does.
 * @par Threading Thread-safe; the new stream is the caller's.
 */
TB_API void tb_stream_open_memory(const void *bytes, size_t length, const tb_allocator *allocator,
                                  tb_stream *stream);

/**
 * @brief Open a stream that writes into memory it owns, growing as it goes, and
 * reads back what was written. It can read, write, seek, report its size, read
 * at an offset, and flush. Writing past the end after a seek fills the gap with
 * zeros.
 * @param allocator Where the state and the bytes are allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on return.
 * @par Errors Running out of memory for the state ends the process. A write
 * that needs more memory than there is returns NO_MEMORY and writes nothing.
 * @par Threading Thread-safe; the new stream is the caller's.
 */
TB_API void tb_stream_open_buffer(const tb_allocator *allocator, tb_stream *stream);

/**
 * @brief Borrow the bytes of a stream from tb_stream_open_buffer().
 * @param stream Required open buffer stream; any other kind of stream is a
 * contract violation.
 * @param[out] length Required; the size of the stream.
 * @return The bytes, or NULL when there are none. Valid until the next write
 * to the stream or its close; the caller does not free them.
 */
TB_API const unsigned char *tb_stream_buffer_bytes(const tb_stream *stream, size_t *length);

/**
 * @brief Open a file as a stream that owns its handle. It can do what the flags
 * allow: read and read at an offset with TB_FILE_READ, write and flush with
 * TB_FILE_WRITE, and always seek and report its size.
 *
 * The stream keeps a position of its own and reads and writes with
 * tb_file_read_at() and tb_file_write_at(), so its reads at an offset are
 * thread-safe and do not disturb the position.
 * @param path Borrowed UTF-8 filename.
 * @param flags As tb_file_open().
 * @param allocator Where the stream's state is allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on success, zero on failure.
 * @return The result of opening the file, as tb_file_open() reports it.
 * @par Threading Thread-safe; the new stream is the caller's.
 */
TB_API tb_fs_result tb_stream_open_file(tb_path_view path, unsigned flags,
                                        const tb_allocator *allocator, tb_stream *stream);

/**
 * @brief Open a stream over a window of another, for one entry inside an
 * archive. It can read, seek, report its size, and read at an offset, all
 * through the parent's reads at an offset; it cannot write.
 *
 * The window is not checked against the parent's size: where the parent ends
 * first, so does the range.
 * @param parent Required open stream that can read at an offset; borrowed, and
 * must stay open until the range is closed. Its position is not used.
 * @param offset Position in the parent of the range's first byte.
 * @param length Bytes in the range.
 * @param allocator Where the range's state is allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on success, zero on failure.
 * @return Status. UNSUPPORTED if the parent cannot read at an offset; TOO_LARGE
 * if the window ends past INT64_MAX.
 * @par Threading
 * Thread-safe; the new stream is the caller's. Reads at an offset from the
 * range are as thread-safe as those of its parent.
 */
TB_API tb_stream_result tb_stream_open_range(tb_stream *parent, uint64_t offset, uint64_t length,
                                             const tb_allocator *allocator, tb_stream *stream);

#endif
