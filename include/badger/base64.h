/** @file base64.h
 * @brief Base64 (RFC 4648): bytes as text, and text back to bytes.
 *
 * Binary data travels as Base64 in JSON, configuration files, and the data
 * URIs of glTF. Two alphabets are supported, each with or without padding:
 * the standard one (`+` and `/`) and the URL-safe one (`-` and `_`).
 *
 * **Decoding is strict.** Text is untrusted, so a character outside the
 * alphabet, padding in the wrong place or of the wrong amount, and non-zero
 * bits in the last character are all rejected, with the offset of the first
 * offending character. Each byte string therefore has exactly one encoding
 * that decodes. ASCII whitespace is skipped only when asked for, for text
 * wrapped into lines.
 *
 * **Nothing is written on failure.** A buffer that is too small is reported
 * with the size it needs, and a decode checks all of the text before it
 * writes a byte.
 *
 * @par Threading
 * Thread-safe: the functions keep no state. The allocating forms are as
 * thread-safe as the allocator they are given.
 */
#ifndef TB_BASE64_H
#define TB_BASE64_H

#include "badger/allocator.h"
#include "badger/api.h"

#include <stddef.h>

/** @brief Why an encode or a decode failed. */
typedef enum tb_base64_result
{
    TB_BASE64_OK,      /**< Success. */
    TB_BASE64_INVALID, /**< The text is not Base64 of the chosen form, or the flags are unknown. */
    TB_BASE64_TOO_LARGE,       /**< A size cannot be represented in a size_t. */
    TB_BASE64_BUFFER_TOO_SMALL /**< The output does not fit; the size it needs is reported. */
} tb_base64_result;

/** @brief Choices of form, combined with `|`. Zero is the standard, padded form. */
typedef enum tb_base64_flags
{
    TB_BASE64_URL_SAFE = 1,       /**< `-` and `_` in place of `+` and `/` (RFC 4648 section 5). */
    TB_BASE64_NO_PADDING = 2,     /**< No `=`: encoding writes none, decoding accepts none. */
    TB_BASE64_SKIP_WHITESPACE = 4 /**< Decoding skips space, tab, and line breaks anywhere. */
} tb_base64_flags;

/**
 * @brief The exact length of the text for `length` bytes.
 * @param length Bytes to encode.
 * @param flags TB_BASE64_URL_SAFE and TB_BASE64_NO_PADDING have meaning here.
 * @param[out] size Required; the length on success, unchanged on failure.
 * @return OK, TOO_LARGE, or INVALID for unknown flags.
 */
TB_API tb_base64_result tb_base64_encoded_size(size_t length, unsigned flags, size_t *size);

/**
 * @brief The most bytes `text_length` characters can decode to: enough room
 * for tb_base64_decode() whatever the text is.
 * @param text_length Characters of text.
 * @return The size; never more than three quarters of the text, rounded up.
 */
TB_API size_t tb_base64_decoded_size_bound(size_t text_length);

/**
 * @brief Encode bytes as text. No NUL is written.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @param flags A combination of tb_base64_flags.
 * @param text Output, NULL only at capacity zero.
 * @param capacity Characters of room.
 * @param[out] written Required; characters written on success, the length
 * needed on BUFFER_TOO_SMALL, zero otherwise.
 * @return OK, BUFFER_TOO_SMALL, TOO_LARGE, or INVALID for unknown flags.
 */
TB_API tb_base64_result tb_base64_encode(const void *bytes, size_t length, unsigned flags,
                                         char *text, size_t capacity, size_t *written);

/**
 * @brief Decode text into bytes.
 * @param text Input, NULL only at length zero. Need not end with a NUL.
 * @param length Characters of `text`.
 * @param flags A combination of tb_base64_flags.
 * @param bytes Output, NULL only at capacity zero.
 * @param capacity Bytes of room; tb_base64_decoded_size_bound() is always enough.
 * @param[out] written Required; bytes written on success, the length needed
 * on BUFFER_TOO_SMALL, zero otherwise.
 * @param[out] error_offset Optional; on INVALID, the offset of the first
 * character that is wrong, or `length` when the text ends too soon.
 * @return OK, BUFFER_TOO_SMALL, or INVALID. Nothing is written unless OK.
 */
TB_API tb_base64_result tb_base64_decode(const char *text, size_t length, unsigned flags,
                                         void *bytes, size_t capacity, size_t *written,
                                         size_t *error_offset);

/**
 * @brief Encode bytes as text in memory of its own, with a NUL after it.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @param flags A combination of tb_base64_flags.
 * @param allocator Where the text is allocated, or NULL for the heap.
 * @param[out] text Required; the text on success, NULL otherwise. Release it
 * with tb_allocator_free(allocator, text, text_length + 1, 1).
 * @param[out] text_length Required; characters before the NUL; zero on failure.
 * @return OK, TOO_LARGE, or INVALID for unknown flags.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_base64_result tb_base64_encode_alloc(const void *bytes, size_t length, unsigned flags,
                                               const tb_allocator *allocator, char **text,
                                               size_t *text_length);

/**
 * @brief Decode text into bytes in memory of their own.
 * @param text Input, NULL only at length zero.
 * @param length Characters of `text`.
 * @param flags A combination of tb_base64_flags.
 * @param allocator Where the bytes are allocated, or NULL for the heap.
 * @param[out] bytes Required; the bytes on success, NULL otherwise. Release
 * them with tb_allocator_free(allocator, bytes, byte_length + 1, 1); a NUL
 * follows them for convenience.
 * @param[out] byte_length Required; bytes decoded; zero on failure.
 * @param[out] error_offset Optional; as for tb_base64_decode().
 * @return OK or INVALID.
 * @par Errors Running out of memory ends the process, as allocation does.
 */
TB_API tb_base64_result tb_base64_decode_alloc(const char *text, size_t length, unsigned flags,
                                               const tb_allocator *allocator, unsigned char **bytes,
                                               size_t *byte_length, size_t *error_offset);

/**
 * @brief Name a result, for a log or a message: "invalid argument", "buffer too small".
 * @param result Any value; one that is no result is named "unknown result".
 * @return A string with static storage duration; never NULL.
 */
TB_API const char *tb_base64_result_name(tb_base64_result result);

#endif
