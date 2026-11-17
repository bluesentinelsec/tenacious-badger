/** @file utf8.h
 * @brief Strict Unicode scalar conversion and navigation over bounded ranges.
 *
 * No allocation, locale, normalization, or replacement characters. Embedded
 * NUL and noncharacters are valid scalars. Surrogates and values above U+10FFFF
 * are invalid. Counts are code points, not grapheme clusters or display cells.
 * All functions are thread-safe when callers synchronize access to buffers.
 * Inputs are borrowed for the call; no pointers are retained. Source and
 * destination ranges must not overlap. NULL is allowed for an empty input.
 * Required output pointers are non-null; violations assert in Debug.
 */
#ifndef TB_UTF8_H
#define TB_UTF8_H
#include "badger/api.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum number of bytes in one encoded Unicode scalar. */
#define TB_UTF8_MAX_BYTES ((size_t)4)

/**
 * @brief Decode one scalar at a byte offset, without checking earlier bytes.
 * @param bytes Input range; may be NULL only when length is zero.
 * @param length Number of input bytes.
 * @param offset Byte offset of the scalar; at or beyond length returns false.
 * @param[out] scalar Required; receives the scalar on success.
 * @param[out] next Required; receives the first byte after the scalar.
 * @return false for an invalid/truncated sequence; outputs stay unchanged.
 */
TB_API bool tb_utf8_decode(const char *bytes, size_t length, size_t offset, uint32_t *scalar,
                           size_t *next);
/** @brief Return the encoded byte count, or zero for a non-scalar value. */
TB_API size_t tb_utf8_encoded_size(uint32_t scalar);
/**
 * @brief Encode one scalar without a terminator.
 * @param scalar Unicode scalar value.
 * @param destination Output range; NULL only when capacity is zero.
 * @param capacity Available bytes.
 * @return Bytes written, or zero for invalid scalar/insufficient capacity.
 * Failure leaves the destination unchanged.
 */
TB_API size_t tb_utf8_encode(uint32_t scalar, char *destination, size_t capacity);
/**
 * @brief Validate a complete range.
 * @param bytes Input bytes; NULL only for an empty range.
 * @param length Input byte count.
 * @param[out] error Required; length on success, otherwise the start of the
 * first ill-formed sequence, including a truncated sequence.
 * @return Whether the complete range is well-formed UTF-8.
 */
TB_API bool tb_utf8_validate(const char *bytes, size_t length, size_t *error);
/**
 * @brief Count scalars in a complete range.
 * @param bytes Input bytes; NULL only when length is zero.
 * @param length Byte count.
 * @param[out] count Required; unchanged on invalid input.
 * @return Whether all input was valid.
 */
TB_API bool tb_utf8_count(const char *bytes, size_t length, size_t *count);
/**
 * @brief Find the byte offset of a zero-based scalar index, or the end index.
 * @param bytes Input bytes; NULL only when length is zero.
 * @param length Byte count.
 * @param index Number of scalars to skip; zero selects offset zero.
 * @param[out] offset Required; unchanged on failure.
 * @return false if the traversed prefix is invalid or index exceeds its count.
 * Bytes after the selected offset are not validated.
 */
TB_API bool tb_utf8_offset(const char *bytes, size_t length, size_t index, size_t *offset);
/**
 * @brief Decode the scalar immediately before a byte boundary.
 * @param bytes Input bytes; NULL only when length is zero.
 * @param length Byte count.
 * @param offset Boundary, in [1,length]; outside this range returns false.
 * @param[out] scalar Required; receives the preceding scalar.
 * @param[out] previous Required; receives its byte offset.
 * @return false if no valid scalar ends at offset; outputs stay unchanged.
 * Earlier bytes are not validated.
 */
TB_API bool tb_utf8_previous(const char *bytes, size_t length, size_t offset, uint32_t *scalar,
                             size_t *previous);
/**
 * @brief Find the nearest scalar boundary at or before an arbitrary byte offset.
 * @param bytes Input bytes; NULL only when length is zero.
 * @param length Byte count.
 * @param offset Position in [0,length].
 * @param[out] boundary Required; unchanged on invalid traversed input/offset.
 * @return Whether the prefix through the containing scalar is valid. Bytes
 * starting at a boundary equal to offset, and later bytes, are not validated.
 */
TB_API bool tb_utf8_boundary(const char *bytes, size_t length, size_t offset, size_t *boundary);
/**
 * @brief Convert a complete UTF-8 range to native-endian UTF-16 code units.
 * @param bytes Input; NULL only when length is zero.
 * @param length Byte count, including any embedded NULs.
 * @param destination Output units, or NULL with capacity zero to measure.
 * @param capacity Available units; no terminator is appended.
 * @param[out] written Required; required unit count for valid input, zero for
 * invalid input or size overflow, even when the destination is too small.
 * @return true if valid input was measured or converted. false for invalid
 * input, overflow, or a short destination. No destination writes on failure.
 */
TB_API bool tb_utf8_to_utf16(const char *bytes, size_t length, uint16_t *destination,
                             size_t capacity, size_t *written);
/**
 * @brief Convert native-endian UTF-16 to UTF-8 with no appended terminator.
 * @param units Input units; NULL only when length is zero. Unpaired surrogates
 * are rejected. BOMs are preserved, not interpreted as byte-order switches.
 * @param length Input unit count.
 * @param destination Output bytes, or NULL with capacity zero to measure.
 * @param capacity Available bytes.
 * @param[out] written Required byte count, or zero for invalid input/overflow.
 * @return Same measurement, capacity and failure contract as tb_utf8_to_utf16.
 */
TB_API bool tb_utf8_from_utf16(const uint16_t *units, size_t length, char *destination,
                               size_t capacity, size_t *written);
#endif
