/** @file random.h
 * @brief Random bytes from the operating system, fit for keys and nonces.
 *
 * These come from the system's generator for cryptography: BCryptGenRandom
 * on Windows, getrandom on Linux, and arc4random_buf on macOS. They cannot
 * be seeded or repeated; for random numbers a test or a simulation must be
 * able to repeat, use a generator from badger/rng.h with a recorded seed.
 * Implementations are build-selected.
 */
#ifndef TB_RANDOM_H
#define TB_RANDOM_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Fill a buffer with random bytes from the operating system.
 * @param bytes Output, NULL only at length zero.
 * @param length Bytes to fill; any number.
 * @return true when all requested bytes were filled, including an empty request.
 * On a system generator failure, returns false and the buffer contents are unspecified.
 * @par Threading Thread-safe.
 */
TB_API bool tb_random_bytes(void *bytes, size_t length);

#endif
