/** @file checksum.h
 * @brief CRC-32 and Adler-32: the checksums of ZIP, gzip, PNG, and zlib.
 *
 * Both detect accidental damage, such as a truncated download or a flipped
 * bit on a disk. Neither resists deliberate tampering: anyone who changes the
 * data can compute the checksum again. Use them to check integrity, never
 * authenticity.
 *
 * Each has a one-call form and an incremental form. The incremental form
 * starts from the initial value and is given the previous result with each
 * piece; pieces of any size give the same result as one call over the whole.
 * @code
 * uint32_t crc = TB_CRC32_INITIAL;
 * crc = tb_crc32_update(crc, header, header_length);
 * crc = tb_crc32_update(crc, body, body_length);
 * // crc == tb_crc32(whole, header_length + body_length)
 * @endcode
 *
 * @par Threading
 * Thread-safe: the functions keep no state between calls.
 */
#ifndef TB_CHECKSUM_H
#define TB_CHECKSUM_H

#include "badger/api.h"

#include <stddef.h>
#include <stdint.h>

/** @brief The CRC-32 of no bytes, and where an incremental CRC-32 starts. */
#define TB_CRC32_INITIAL UINT32_C(0)

/** @brief The Adler-32 of no bytes, and where an incremental Adler-32 starts. */
#define TB_ADLER32_INITIAL UINT32_C(1)

/**
 * @brief The CRC-32 of some bytes, as ZIP, gzip, and PNG compute it: the
 * reflected polynomial 0xEDB88320, with the value inverted before and after.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @return The checksum. `tb_crc32("123456789", 9)` is 0xCBF43926.
 */
TB_API uint32_t tb_crc32(const void *bytes, size_t length);

/**
 * @brief Continue a CRC-32 over more bytes.
 * @param crc TB_CRC32_INITIAL, or the result over the bytes before these.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @return The CRC-32 of the earlier bytes followed by these.
 */
TB_API uint32_t tb_crc32_update(uint32_t crc, const void *bytes, size_t length);

/**
 * @brief The Adler-32 of some bytes, as zlib computes it (RFC 1950).
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @return The checksum. `tb_adler32("Wikipedia", 9)` is 0x11E60398.
 */
TB_API uint32_t tb_adler32(const void *bytes, size_t length);

/**
 * @brief Continue an Adler-32 over more bytes.
 * @param adler TB_ADLER32_INITIAL, or the result over the bytes before these.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @return The Adler-32 of the earlier bytes followed by these.
 */
TB_API uint32_t tb_adler32_update(uint32_t adler, const void *bytes, size_t length);

#endif
