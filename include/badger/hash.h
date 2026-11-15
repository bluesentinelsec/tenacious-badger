/** @file hash.h
 * @brief Hash functions for the keys of hashed collections.
 *
 * A hash turns a key into a 64-bit number that is the same for equal keys and
 * well spread for different ones. The atom table and the hash table use these
 * functions, and a caller that supplies its own hash function to a table can
 * build it from them.
 *
 * The results are fixed by the algorithms named below: the same on every
 * platform, in every build, and from one run to the next. They are not
 * randomized, so do not use them where someone hostile chooses the keys.
 *
 * @par Threading
 * Thread-safe: no function keeps state.
 */
#ifndef TB_HASH_H
#define TB_HASH_H

#include "badger/api.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Hash a range of bytes with 64-bit FNV-1a.
 *
 * FNV-1a mixes one byte at a time, which suits the short identifiers that
 * most keys are: names of assets, console variables, and components.
 *
 * @param bytes The bytes; may be NULL only when `length` is 0.
 * @param length Number of bytes.
 * @return The hash. The empty range hashes to `0xCBF29CE484222325`.
 */
TB_API uint64_t tb_hash_bytes(const void *bytes, size_t length);

/**
 * @brief Hash a NUL-terminated string, not including its terminator.
 * @param string Required non-null.
 * @return The same value as tb_hash_bytes() for the bytes of the string.
 */
TB_API uint64_t tb_hash_string(const char *string);

/**
 * @brief Hash a 64-bit integer.
 *
 * Integers used as keys are often consecutive or share their low bits. This
 * spreads them over all 64 bits with the finalizer of SplitMix64, so that a
 * table can use any part of the result.
 *
 * @param value Any value.
 * @return The hash. Different values give different results.
 */
TB_API uint64_t tb_hash_u64(uint64_t value);

/**
 * @brief Hash a pointer by its address, for keys compared by identity.
 *
 * Addresses are aligned, so their low bits are always zero; this spreads them
 * as tb_hash_u64() does.
 *
 * @param pointer Any pointer, including NULL.
 */
TB_API uint64_t tb_hash_pointer(const void *pointer);

#endif
