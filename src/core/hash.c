/* Bytes are hashed with XXH64, the 64-bit function of xxHash, with a seed of
 * zero. It takes the input in stripes of 32 bytes, each stripe as four 64-bit
 * words that go into four accumulators which do not wait for one another, and
 * then folds in whatever is left, eight, four, and one byte at a time.
 *
 * XXH64 defines its words as least significant byte first. They are copied
 * out of the input as the machine stores them, which is that order on every
 * platform the engine supports, and costs one instruction where putting them
 * together byte by byte costs eight. On a machine of the other byte order the
 * published results in the tests fail, so a port to one cannot hash
 * differently without anyone noticing. */
#include "badger/hash.h"

#include "badger/assert.h"

#include <string.h>

/* The five primes of XXH64, from its specification. */
#define TB_HASH_PRIME_1 UINT64_C(0x9E3779B185EBCA87)
#define TB_HASH_PRIME_2 UINT64_C(0xC2B2AE3D27D4EB4F)
#define TB_HASH_PRIME_3 UINT64_C(0x165667B19E3779F9)
#define TB_HASH_PRIME_4 UINT64_C(0x85EBCA77C2B2AE63)
#define TB_HASH_PRIME_5 UINT64_C(0x27D4EB2F165667C5)

/* Bytes that the four accumulators take in one step. */
#define TB_HASH_STRIPE_SIZE ((size_t)32)

static uint64_t tb_hash_rotate_left(uint64_t value, unsigned count)
{
    return (value << count) | (value >> (64 - count));
}

/* The next four bytes as a number. Copied, not read in place: the input has
 * no alignment. */
static uint64_t tb_hash_read_32(const unsigned char *input)
{
    uint32_t word = 0;

    memcpy(&word, input, sizeof(word));
    return word;
}

/* The next eight bytes as a number. */
static uint64_t tb_hash_read_64(const unsigned char *input)
{
    uint64_t word = 0;

    memcpy(&word, input, sizeof(word));
    return word;
}

/* One accumulator takes in one word. */
static uint64_t tb_hash_round(uint64_t accumulator, uint64_t word)
{
    accumulator += word * TB_HASH_PRIME_2;
    accumulator = tb_hash_rotate_left(accumulator, 31);
    return accumulator * TB_HASH_PRIME_1;
}

/* The hash takes in one accumulator, when the stripes are done. */
static uint64_t tb_hash_merge(uint64_t hash, uint64_t accumulator)
{
    hash ^= tb_hash_round(0, accumulator);
    return hash * TB_HASH_PRIME_1 + TB_HASH_PRIME_4;
}

/* The last step: every bit of the hash comes to depend on every other. */
static uint64_t tb_hash_avalanche(uint64_t hash)
{
    hash ^= hash >> 33;
    hash *= TB_HASH_PRIME_2;
    hash ^= hash >> 29;
    hash *= TB_HASH_PRIME_3;
    hash ^= hash >> 32;
    return hash;
}

/* Hashes the whole stripes of an input that has at least one, and moves the
 * cursor past them. */
static uint64_t tb_hash_stripes(const unsigned char **cursor, size_t length)
{
    const unsigned char *input = *cursor;
    uint64_t first = TB_HASH_PRIME_1 + TB_HASH_PRIME_2;
    uint64_t second = TB_HASH_PRIME_2;
    uint64_t third = 0;
    uint64_t fourth = (uint64_t)0 - TB_HASH_PRIME_1;
    uint64_t hash = {0};

    for (size_t left = length; left >= TB_HASH_STRIPE_SIZE; left -= TB_HASH_STRIPE_SIZE)
    {
        first = tb_hash_round(first, tb_hash_read_64(input));
        second = tb_hash_round(second, tb_hash_read_64(input + 8));
        third = tb_hash_round(third, tb_hash_read_64(input + 16));
        fourth = tb_hash_round(fourth, tb_hash_read_64(input + 24));
        input += TB_HASH_STRIPE_SIZE;
    }
    hash = tb_hash_rotate_left(first, 1) + tb_hash_rotate_left(second, 7) +
           tb_hash_rotate_left(third, 12) + tb_hash_rotate_left(fourth, 18);
    hash = tb_hash_merge(hash, first);
    hash = tb_hash_merge(hash, second);
    hash = tb_hash_merge(hash, third);
    hash = tb_hash_merge(hash, fourth);
    *cursor = input;
    return hash;
}

uint64_t tb_hash_bytes(const void *bytes, size_t length)
{
    const unsigned char *input = bytes;
    /* Bytes after the whole stripes: fewer than a stripe. */
    size_t left = length % TB_HASH_STRIPE_SIZE;
    uint64_t hash = TB_HASH_PRIME_5;

    TB_ASSERT(bytes != NULL || length == 0);
    if (length >= TB_HASH_STRIPE_SIZE)
    {
        hash = tb_hash_stripes(&input, length);
    }
    hash += (uint64_t)length;
    for (; left >= 8; left -= 8)
    {
        hash ^= tb_hash_round(0, tb_hash_read_64(input));
        hash = tb_hash_rotate_left(hash, 27) * TB_HASH_PRIME_1 + TB_HASH_PRIME_4;
        input += 8;
    }
    if (left >= 4)
    {
        hash ^= tb_hash_read_32(input) * TB_HASH_PRIME_1;
        hash = tb_hash_rotate_left(hash, 23) * TB_HASH_PRIME_2 + TB_HASH_PRIME_3;
        input += 4;
        left -= 4;
    }
    for (; left > 0; left--)
    {
        hash ^= (uint64_t)*input * TB_HASH_PRIME_5;
        hash = tb_hash_rotate_left(hash, 11) * TB_HASH_PRIME_1;
        input++;
    }
    return tb_hash_avalanche(hash);
}

uint64_t tb_hash_string(const char *string)
{
    TB_ASSERT(string != NULL);
    return tb_hash_bytes(string, strlen(string));
}

uint64_t tb_hash_u64(uint64_t value)
{
    /* The finalizer of SplitMix64: each step is invertible, so no two inputs
     * share an output. */
    value ^= value >> 30;
    value *= UINT64_C(0xBF58476D1CE4E5B9);
    value ^= value >> 27;
    value *= UINT64_C(0x94D049BB133111EB);
    value ^= value >> 31;
    return value;
}

uint64_t tb_hash_pointer(const void *pointer)
{
    return tb_hash_u64((uint64_t)(uintptr_t)pointer);
}
