/** @file chacha20.h
 * @brief The ChaCha20 stream cipher (RFC 8439), from any byte of its keystream.
 *
 * ChaCha20 turns a 256-bit key and a 96-bit nonce into a keystream, which is
 * combined with data by XOR: the same call obfuscates and restores. Each
 * 64-byte block of the keystream is computed from its own index, so a
 * position anywhere in the data costs no more to reach than the start. That
 * is what lets many threads read one obfuscated package at offsets of their
 * own.
 *
 * The engine uses it to obfuscate packages, which deters casual extraction.
 * Used alone it hides data, but does not detect changes to it, and the key
 * a game ships with can be recovered by anyone who runs the game under a
 * debugger. **Obfuscation is not security.**
 *
 * A key must never be used twice with the same nonce: the XOR of the two
 * results would be the XOR of the two inputs. Take a fresh random nonce
 * (badger/random.h) for each thing obfuscated under one key.
 *
 * @par Threading
 * tb_chacha20_block() and tb_chacha20_xor() are thread-safe. A stream from
 * tb_chacha20_open_stream() is externally synchronized, except that reads at
 * an offset are as thread-safe as the inner stream's. Opening one is
 * externally synchronized with the inner stream: open it once, then read it
 * at offsets from many threads.
 */
#ifndef TB_CHACHA20_H
#define TB_CHACHA20_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/stream.h"

#include <stddef.h>
#include <stdint.h>

/** @brief Bytes of a key. */
#define TB_CHACHA20_KEY_SIZE 32u

/** @brief Bytes of a nonce. */
#define TB_CHACHA20_NONCE_SIZE 12u

/** @brief Bytes of a keystream block. */
#define TB_CHACHA20_BLOCK 64u

/** @brief The longest keystream of one key and nonce: 2^32 blocks, 256 GiB. */
#define TB_CHACHA20_MAX_LENGTH (UINT64_C(1) << 38)

/**
 * @brief One block of keystream, as RFC 8439 section 2.3 defines it.
 * @param key Required; TB_CHACHA20_KEY_SIZE bytes.
 * @param counter The block's index.
 * @param nonce Required; TB_CHACHA20_NONCE_SIZE bytes.
 * @param[out] output Required; TB_CHACHA20_BLOCK bytes.
 */
TB_API void tb_chacha20_block(const uint8_t key[TB_CHACHA20_KEY_SIZE], uint32_t counter,
                              const uint8_t nonce[TB_CHACHA20_NONCE_SIZE],
                              uint8_t output[TB_CHACHA20_BLOCK]);

/**
 * @brief XOR bytes with the keystream from a byte offset in it: obfuscate or restore.
 *
 * The keystream starts at block 0, so offset 0 matches RFC 8439's
 * encryption with an initial counter of 0, and offset 64 one of 1.
 * @param key Required; TB_CHACHA20_KEY_SIZE bytes.
 * @param nonce Required; TB_CHACHA20_NONCE_SIZE bytes.
 * @param offset The position in the keystream of the first byte.
 * @param input Bytes to combine, NULL only at length zero.
 * @param[out] output Where the result goes, NULL only at length zero. It may
 * be `input` itself; otherwise the two must not overlap.
 * @param length Bytes to combine.
 * @par Errors Running past TB_CHACHA20_MAX_LENGTH is a contract violation.
 */
TB_API void tb_chacha20_xor(const uint8_t key[TB_CHACHA20_KEY_SIZE],
                            const uint8_t nonce[TB_CHACHA20_NONCE_SIZE], uint64_t offset,
                            const void *input, void *output, size_t length);

/**
 * @brief Open a stream that XORs another stream's bytes with a keystream as
 * they pass, in either direction.
 *
 * Byte p of the inner stream meets byte p of the keystream: reads at an
 * offset, and reads and writes after a seek, line up by themselves. The
 * stream offers what the inner stream offers, and nothing more. It starts
 * at the inner stream's position, or 0 if the inner stream cannot tell it.
 * Closing it clears its copy of the key and leaves the inner stream open.
 * @param inner Required open stream; borrowed, and must stay open until this
 * one is closed.
 * @param key Required; TB_CHACHA20_KEY_SIZE bytes, copied.
 * @param nonce Required; TB_CHACHA20_NONCE_SIZE bytes, copied.
 * @param allocator Where the stream's state is allocated, or NULL for the heap.
 * @param[out] stream Required zero stream; open on OK.
 * @return OK, or the inner stream's failure to tell its position.
 * @par Errors Running out of memory ends the process. Positions past
 * TB_CHACHA20_MAX_LENGTH are read and written as TB_STREAM_TOO_LARGE.
 * @par Threading Externally synchronized with every other use of `inner`,
 * since telling its position may move it; the new stream is the caller's.
 */
TB_API tb_stream_result tb_chacha20_open_stream(tb_stream *inner,
                                                const uint8_t key[TB_CHACHA20_KEY_SIZE],
                                                const uint8_t nonce[TB_CHACHA20_NONCE_SIZE],
                                                const tb_allocator *allocator, tb_stream *stream);

#endif
