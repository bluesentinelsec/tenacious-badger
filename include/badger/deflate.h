/** @file deflate.h
 * @brief DEFLATE decompression (RFC 1951), in zlib (RFC 1950) and gzip
 * (RFC 1952) framing or raw.
 *
 * DEFLATE is the compression of ZIP, gzip, PNG, and zlib. An inflater
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
 * @par Threading
 * An inflater is externally synchronized; different inflaters are
 * independent. tb_inflate() and tb_deflate_result_name() are thread-safe.
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

/**
 * @brief Name a result, for a log or a message: "malformed data", "checksum mismatch".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 * @par Threading Thread-safe.
 */
TB_API const char *tb_deflate_result_name(tb_deflate_result result);

#endif
