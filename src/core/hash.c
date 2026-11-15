#include "badger/hash.h"

#include "badger/assert.h"

#include <string.h>

/* The parameters of 64-bit FNV-1a, from its specification. */
#define TB_HASH_FNV_OFFSET_BASIS UINT64_C(0xCBF29CE484222325)
#define TB_HASH_FNV_PRIME UINT64_C(0x00000100000001B3)

uint64_t tb_hash_bytes(const void *bytes, size_t length)
{
    const unsigned char *const input = bytes;
    uint64_t hash = TB_HASH_FNV_OFFSET_BASIS;

    TB_ASSERT(bytes != NULL || length == 0);
    for (size_t index = 0; index < length; index++)
    {
        hash ^= input[index];
        hash *= TB_HASH_FNV_PRIME;
    }
    return hash;
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
