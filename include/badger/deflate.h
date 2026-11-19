/** @file deflate.h
 * @brief DEFLATE compression and decompression (RFC 1951), in zlib
 * (RFC 1950) and gzip (RFC 1952) framing or raw.
 *
 * DEFLATE is the compression of ZIP, gzip, PNG, and zlib.
 *
 * **Decompression.** An inflater
 * decompresses one stream, fed input and output in ranges of any size: it
 * stops when it needs more input or more room, and continues where it left
 * off. For data that fits in memory, tb_inflate() does it in one call. For a
 * stream of the engine's own, tb_inflate_open_stream() decompresses another
 * stream as it is read.
 *
 * **The input is untrusted.** Archives, mods, and downloads reach the
 * decoder, so it never reads or writes out of bounds, never allocates as it
 * decodes, and reports every malformed stream as a result. Its memory is
 * fixed when it is created: a window of 32 KiB and its code tables. What
 * bounds the output is the caller: the capacity it gives tb_inflate(), or the
 * limit it gives tb_stream_copy().
 *
 * **The input is consumed exactly.** An inflater reads no byte past the end
 * of its stream, so whatever follows, such as the next member of a gzip file
 * or the next entry of an archive, is left to the caller.
 *
 * **Compression.** A deflater compresses at a level from 0 (store only) to 9
 * (smallest), fed in the same way, and tb_deflate() and
 * tb_deflate_open_stream() are its one-call and stream forms. The same input
 * gives the same compressed bytes at the same level however it is split into
 * calls.
 *
 * @par Threading
 * An inflater or a deflater is externally synchronized; different ones are
 * independent. tb_inflate(), tb_deflate(), tb_deflate_bound(), and
 * tb_deflate_result_name() are thread-safe.
 */
#ifndef TB_DEFLATE_H
#define TB_DEFLATE_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief Why decompression stopped short. */
typedef enum tb_deflate_result
{
    TB_DEFLATE_OK,         /**< Success. */
    TB_DEFLATE_INVALID,    /**< An argument is malformed, such as an unknown format. */
    TB_DEFLATE_TOO_LARGE,  /**< The output did not fit in the capacity given. */
    TB_DEFLATE_MALFORMED,  /**< The input is not a valid stream of its format. */
    TB_DEFLATE_CHECKSUM,   /**< The stream decoded, but its checksum or length does not match. */
    TB_DEFLATE_TRUNCATED,  /**< The input ended before the stream did. */
    TB_DEFLATE_UNSUPPORTED /**< A valid stream that needs what this decoder lacks: a zlib preset
                              dictionary. */
} tb_deflate_result;

/** @brief How the DEFLATE data is framed. */
typedef enum tb_deflate_format
{
    TB_DEFLATE_RAW,  /**< No framing, as in ZIP entries. */
    TB_DEFLATE_ZLIB, /**< A two-byte header and an Adler-32, as in PNG. */
    TB_DEFLATE_GZIP  /**< A gzip member: a header, and a CRC-32 and length. */
} tb_deflate_format;

/** @brief A decompressor of one stream at a time. Opaque. */
typedef struct tb_inflater tb_inflater;

/**
 * @brief Create an inflater for streams of one format.
 * @param format How the streams are framed; any other value is a contract violation.
 * @param allocator Where its fixed memory comes from, or NULL for the heap.
 * @return The inflater, ready for a stream; never NULL. Release it with tb_inflater_destroy().
 * @par Errors Running out of memory ends the process, as allocation does.
 * @par Threading Thread-safe; the new inflater is the caller's.
 */
TB_API tb_inflater *tb_inflater_create(tb_deflate_format format, const tb_allocator *allocator);

/**
 * @brief Release an inflater. @param inflater An inflater, or NULL to do nothing.
 */
TB_API void tb_inflater_destroy(tb_inflater *inflater);

/**
 * @brief Forget the current stream, its progress and any failure, and start
 * again at the beginning of a new stream of the same format.
 * @param inflater Required.
 */
TB_API void tb_inflater_reset(tb_inflater *inflater);

/**
 * @brief Decompress as much as the input and the room for output allow.
 *
 * Call it again with the input that was not used, and more, and with fresh
 * room, until tb_inflater_finished() is true. A call that uses no input and
 * writes nothing needs what the caller did not give: more input when there
 * was room, or room when there was input.
 * @param inflater Required.
 * @param input Compressed bytes, NULL only at length zero.
 * @param input_length Bytes in `input`.
 * @param[out] input_used Required; bytes of `input` consumed, even on failure.
 * @param output Room for decompressed bytes, NULL only at capacity zero.
 * @param output_capacity Bytes of room.
 * @param[out] output_written Required; bytes written to `output`, even on failure.
 * @return OK, or why the stream cannot be decoded. A failure is kept: every
 * later call returns it, without consuming or writing, until a reset.
 */
TB_API tb_deflate_result tb_inflater_run(tb_inflater *inflater, const void *input,
                                         size_t input_length, size_t *input_used, void *output,
                                         size_t output_capacity, size_t *output_written);

/**
 * @brief Whether the whole stream, its checksum included, has been decoded
 * and every byte of it written out.
 * @param inflater Required.
 */
TB_API bool tb_inflater_finished(const tb_inflater *inflater);

/**
 * @brief Decompress one whole stream into a buffer, in one call.
 * @param format How the stream is framed.
 * @param input The whole stream, and nothing after it; NULL only at length zero.
 * @param input_length Bytes in `input`.
 * @param output Room for the decompressed bytes, NULL only at capacity zero.
 * @param capacity Bytes of room: the most the caller accepts.
 * @param[out] written Required; bytes written, even on failure.
 * @return Status. TOO_LARGE when the output does not fit; TRUNCATED when the
 * input ends first; MALFORMED when bytes follow the end of the stream.
 * @par Errors The inflater it uses is allocated on the heap, and running out
 * of memory for it ends the process.
 */
TB_API tb_deflate_result tb_inflate(tb_deflate_format format, const void *input,
                                    size_t input_length, void *output, size_t capacity,
                                    size_t *written);

/**
 * @brief Open a stream that decompresses another as it is read. It can read;
 * it cannot write, seek, report its size, or read at an offset.
 *
 * In gzip format it reads every member, one after another, as gzip does.
 * A malformed stream, a wrong checksum, or bytes after the end that do not
 * start a gzip member, are read as TB_STREAM_MALFORMED. A source that ends
 * before the stream does is read as TB_STREAM_END.
 * @param source Required open stream that can read; borrowed, and must stay
 * open until this one is closed. It is read from its position, in pieces, and
 * may be read past the end of the compressed data.
 * @param format How the data is framed.
 * @param allocator Where the stream's state is allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on success, zero on failure.
 * @return Status. UNSUPPORTED if the source cannot read.
 * @par Errors Running out of memory for the state ends the process.
 * @par Threading Thread-safe; the new stream is the caller's.
 */
TB_API tb_stream_result tb_inflate_open_stream(tb_stream *source, tb_deflate_format format,
                                               const tb_allocator *allocator, tb_stream *stream);

/* -------------------------------------------------------------------------
 * Compression.
 * ------------------------------------------------------------------------- */

/** @brief The level that balances size and speed, as zlib's default does. */
#define TB_DEFLATE_DEFAULT_LEVEL 6

/** @brief The highest level: the smallest output, the slowest. */
#define TB_DEFLATE_BEST_LEVEL 9

/** @brief What a call to tb_deflater_run() should finish. */
typedef enum tb_deflate_flush
{
    /** Compress what fits; more input follows. */
    TB_DEFLATE_NO_FLUSH,
    /** Write out everything given so far, ending on a whole byte, so a reader
     * can decompress it now; the stream continues afterwards. */
    TB_DEFLATE_SYNC_FLUSH,
    /** The input is complete: end the stream, with its checksum. */
    TB_DEFLATE_FINISH
} tb_deflate_flush;

/** @brief A compressor of one stream at a time. Opaque. */
typedef struct tb_deflater tb_deflater;

/**
 * @brief Create a deflater for streams of one format.
 * @param format How the streams are framed; any other value is a contract violation.
 * @param level From 0, which only stores, to 9; any other value is a contract violation.
 * @param allocator Where its fixed memory comes from, or NULL for the heap.
 * @return The deflater; never NULL. Release it with tb_deflater_destroy().
 * @par Errors Running out of memory ends the process, as allocation does. A
 * deflater holds about 600 KiB, whatever it compresses.
 * @par Threading Thread-safe; the new deflater is the caller's.
 */
TB_API tb_deflater *tb_deflater_create(tb_deflate_format format, int level,
                                       const tb_allocator *allocator);

/**
 * @brief Release a deflater. @param deflater A deflater, or NULL to do nothing.
 */
TB_API void tb_deflater_destroy(tb_deflater *deflater);

/**
 * @brief Forget the current stream and start a new one, of the same format and level.
 * @param deflater Required.
 */
TB_API void tb_deflater_reset(tb_deflater *deflater);

/**
 * @brief Compress as much as the input and the room for output allow.
 *
 * Give the input in pieces of any size with TB_DEFLATE_NO_FLUSH, then call
 * with TB_DEFLATE_FINISH, with the rest of the input or none, until
 * tb_deflater_finished() is true. A call that returns having written all the
 * room it was given has more to write.
 * @param deflater Required.
 * @param input Bytes to compress, NULL only at length zero.
 * @param input_length Bytes in `input`.
 * @param[out] input_used Required; bytes of `input` taken.
 * @param output Room for compressed bytes, NULL only at capacity zero.
 * @param output_capacity Bytes of room.
 * @param[out] output_written Required; bytes written to `output`.
 * @param flush What the call should complete, once all of `input` is taken.
 * @return OK; INVALID for an unknown flush. Input given after a finish is not taken.
 */
TB_API tb_deflate_result tb_deflater_run(tb_deflater *deflater, const void *input,
                                         size_t input_length, size_t *input_used, void *output,
                                         size_t output_capacity, size_t *output_written,
                                         tb_deflate_flush flush);

/**
 * @brief Whether the stream has been finished and every byte of it written out.
 * @param deflater Required.
 */
TB_API bool tb_deflater_finished(const tb_deflater *deflater);

/**
 * @brief The most bytes tb_deflate() can write for an input of a length, in
 * any format and at any level.
 * @param input_length Bytes to compress.
 * @return A capacity that is always enough; SIZE_MAX when it cannot be represented.
 */
TB_API size_t tb_deflate_bound(size_t input_length);

/**
 * @brief Compress bytes into one whole stream, in one call.
 * @param format How the stream is framed.
 * @param level From 0 to 9.
 * @param input Bytes to compress, NULL only at length zero.
 * @param input_length Bytes in `input`.
 * @param output Room for the stream, NULL only at capacity zero.
 * @param capacity Bytes of room; tb_deflate_bound() of the input is always enough.
 * @param[out] written Required; bytes written, even on failure.
 * @return Status. TOO_LARGE when the stream does not fit; INVALID for an
 * unknown format or a level out of range.
 * @par Errors The deflater it uses is allocated on the heap, and running out
 * of memory for it ends the process.
 */
TB_API tb_deflate_result tb_deflate(tb_deflate_format format, int level, const void *input,
                                    size_t input_length, void *output, size_t capacity,
                                    size_t *written);

/**
 * @brief Open a stream that compresses what is written to it into another.
 * It can write and flush; it cannot read, seek, report its size, or read at
 * an offset.
 *
 * A flush writes out everything written so far, as TB_DEFLATE_SYNC_FLUSH
 * does, then flushes the sink. Closing the stream finishes the compressed
 * stream and writes its end to the sink; a failure to do so is the result of
 * tb_stream_close(). The sink is not closed.
 * @param sink Required open stream that can write; borrowed, and must stay
 * open until this one is closed.
 * @param format How the data is framed.
 * @param level From 0 to 9.
 * @param allocator Where the stream's state is allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on success, zero on failure.
 * @return Status. UNSUPPORTED if the sink cannot write; INVALID for an unknown
 * format or a level out of range.
 * @par Errors Running out of memory for the state ends the process.
 * @par Threading Thread-safe; the new stream is the caller's.
 */
TB_API tb_stream_result tb_deflate_open_stream(tb_stream *sink, tb_deflate_format format, int level,
                                               const tb_allocator *allocator, tb_stream *stream);

/**
 * @brief Name a result, for a log or a message: "malformed data", "checksum mismatch".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe.
 */
TB_API const char *tb_deflate_result_name(tb_deflate_result result);

#endif
