/** @file sha256.h
 * @brief SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104).
 *
 * SHA-256 maps bytes of any length to 32 bytes that change unpredictably
 * with any change to the input. The engine uses it to turn a password into
 * a key, and HMAC-SHA256 to derive one key from another and a salt, as the
 * package format does. It also names content, for caches of cooked assets.
 *
 * Each has a one-call form and an incremental form, which gives the same
 * result for the input in pieces of any size:
 * @code
 * tb_sha256_state hash;
 * uint8_t digest[TB_SHA256_SIZE];
 * tb_sha256_init(&hash);
 * tb_sha256_update(&hash, header, header_length);
 * tb_sha256_update(&hash, body, body_length);
 * tb_sha256_final(&hash, digest);
 * @endcode
 *
 * The state of a hash of secret data holds that data. tb_sha256_final() and
 * tb_hmac_sha256_final() clear it.
 *
 * @par Threading
 * The one-call functions are thread-safe. A state is externally synchronized.
 */
#ifndef TB_SHA256_H
#define TB_SHA256_H

#include "badger/api.h"

#include <stddef.h>
#include <stdint.h>

/** @brief Bytes of a SHA-256 digest, and of an HMAC-SHA256. */
#define TB_SHA256_SIZE 32u

/** @brief Bytes of the blocks SHA-256 works in. */
#define TB_SHA256_BLOCK 64u

/** @brief The state of an incremental SHA-256. Start it with tb_sha256_init(). */
typedef struct tb_sha256_state
{
    uint32_t state[8];                     /**< The chaining value; private. */
    uint64_t length;                       /**< Bytes taken so far; private. */
    unsigned char buffer[TB_SHA256_BLOCK]; /**< Bytes of a block not yet complete; private. */
    size_t buffered;                       /**< Bytes in `buffer`; private. */
} tb_sha256_state;

/** @brief Start a hash. @param hash Required. */
TB_API void tb_sha256_init(tb_sha256_state *hash);

/**
 * @brief Add bytes to a hash.
 * @param hash Required, started and not finished.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @par Errors More than 2^61 - 1 bytes in all is a contract violation.
 */
TB_API void tb_sha256_update(tb_sha256_state *hash, const void *bytes, size_t length);

/**
 * @brief Finish a hash and clear its state, which must be started again to be used.
 * @param hash Required.
 * @param[out] digest Required; TB_SHA256_SIZE bytes.
 */
TB_API void tb_sha256_final(tb_sha256_state *hash, uint8_t digest[TB_SHA256_SIZE]);

/**
 * @brief The SHA-256 of some bytes.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @param[out] digest Required; TB_SHA256_SIZE bytes.
 */
TB_API void tb_sha256(const void *bytes, size_t length, uint8_t digest[TB_SHA256_SIZE]);

/** @brief The state of an incremental HMAC-SHA256. Start it with tb_hmac_sha256_init(). */
typedef struct tb_hmac_sha256_state
{
    tb_sha256_state inner; /**< The hash of the padded key and the message; private. */
    tb_sha256_state outer; /**< The hash of the other padded key, waiting for the inner; private. */
} tb_hmac_sha256_state;

/**
 * @brief Start an HMAC.
 * @param mac Required.
 * @param key The key, NULL only at length zero. Any length; a key longer
 * than TB_SHA256_BLOCK is hashed first, as RFC 2104 says.
 * @param key_length Bytes of `key`.
 */
TB_API void tb_hmac_sha256_init(tb_hmac_sha256_state *mac, const void *key, size_t key_length);

/**
 * @brief Add bytes to an HMAC.
 * @param mac Required, started and not finished.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 */
TB_API void tb_hmac_sha256_update(tb_hmac_sha256_state *mac, const void *bytes, size_t length);

/**
 * @brief Finish an HMAC and clear its state.
 * @param mac Required.
 * @param[out] result Required; TB_SHA256_SIZE bytes.
 */
TB_API void tb_hmac_sha256_final(tb_hmac_sha256_state *mac, uint8_t result[TB_SHA256_SIZE]);

/**
 * @brief The HMAC-SHA256 of some bytes under a key.
 * @param key The key, NULL only at length zero.
 * @param key_length Bytes of `key`.
 * @param bytes Input, NULL only at length zero.
 * @param length Bytes in `bytes`.
 * @param[out] result Required; TB_SHA256_SIZE bytes.
 */
TB_API void tb_hmac_sha256(const void *key, size_t key_length, const void *bytes, size_t length,
                           uint8_t result[TB_SHA256_SIZE]);

#endif
