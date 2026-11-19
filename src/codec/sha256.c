/* SHA-256 as FIPS 180-4 defines it, and HMAC-SHA256 as RFC 2104 does. */
#include "badger/sha256.h"

#include "badger/assert.h"

#include <string.h>

/* The most bytes SHA-256 can hash: its length is counted in 64 bits of bits. */
#define TB_SHA256_MAX_LENGTH ((UINT64_C(1) << 61) - 1)

/* The padding bytes of HMAC's inner and outer keys. */
#define TB_HMAC_INNER_PAD 0x36u
#define TB_HMAC_OUTER_PAD 0x5Cu

/* The first 32 bits of the fractional parts of the cube roots of the first 64 primes. */
static const uint32_t tb_sha256_constants[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u,
    0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu,
    0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu,
    0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu, 0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu,
    0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
    0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u, 0x19A4C116u,
    0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u,
    0xC67178F2u,
};

/* The first 32 bits of the fractional parts of the square roots of the first 8 primes. */
static const uint32_t tb_sha256_initial[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

/* Clear memory that held a secret. A plain memset of memory no one reads
 * again may be removed by the compiler; stores through a volatile pointer
 * may not. (This is not synchronization, for which volatile is never used.) */
static void tb_sha256_wipe(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;
    while (length > 0)
    {
        *bytes = 0;
        bytes++;
        length--;
    }
}

static uint32_t tb_sha256_rotate(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32u - count));
}

static uint32_t tb_sha256_load(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 |
           (uint32_t)bytes[3];
}

static void tb_sha256_store(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value >> 24);
    bytes[1] = (unsigned char)((value >> 16) & 0xFFu);
    bytes[2] = (unsigned char)((value >> 8) & 0xFFu);
    bytes[3] = (unsigned char)(value & 0xFFu);
}

/* Fold one block into the chaining value. */
static void tb_sha256_block(uint32_t state[8], const unsigned char *block)
{
    uint32_t schedule[64] = {0};
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (size_t index = 0; index < 16; index++)
    {
        schedule[index] = tb_sha256_load(block + 4 * index);
    }
    for (size_t index = 16; index < 64; index++)
    {
        const uint32_t early = schedule[index - 15];
        const uint32_t late = schedule[index - 2];
        const uint32_t sigma0 =
            tb_sha256_rotate(early, 7) ^ tb_sha256_rotate(early, 18) ^ (early >> 3);
        const uint32_t sigma1 =
            tb_sha256_rotate(late, 17) ^ tb_sha256_rotate(late, 19) ^ (late >> 10);
        schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
    }
    for (size_t index = 0; index < 64; index++)
    {
        const uint32_t sum1 =
            tb_sha256_rotate(e, 6) ^ tb_sha256_rotate(e, 11) ^ tb_sha256_rotate(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t first = h + sum1 + choose + tb_sha256_constants[index] + schedule[index];
        const uint32_t sum0 =
            tb_sha256_rotate(a, 2) ^ tb_sha256_rotate(a, 13) ^ tb_sha256_rotate(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void tb_sha256_init(tb_sha256_state *hash)
{
    TB_ASSERT(hash != NULL);
    *hash = (tb_sha256_state){0};
    memcpy(hash->state, tb_sha256_initial, sizeof(hash->state));
}

void tb_sha256_update(tb_sha256_state *hash, const void *bytes, size_t length)
{
    const unsigned char *input = bytes;
    TB_ASSERT(hash != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    TB_ASSERT_MSG((uint64_t)length <= TB_SHA256_MAX_LENGTH - hash->length,
                  "more than SHA-256 can hash");
    hash->length += length;
    if (hash->buffered > 0)
    {
        const size_t room = TB_SHA256_BLOCK - hash->buffered;
        const size_t piece = length < room ? length : room;
        memcpy(hash->buffer + hash->buffered, input, piece);
        hash->buffered += piece;
        input += piece;
        length -= piece;
        if (hash->buffered < TB_SHA256_BLOCK)
        {
            return;
        }
        tb_sha256_block(hash->state, hash->buffer);
        hash->buffered = 0;
    }
    while (length >= TB_SHA256_BLOCK)
    {
        tb_sha256_block(hash->state, input);
        input += TB_SHA256_BLOCK;
        length -= TB_SHA256_BLOCK;
    }
    if (length > 0)
    {
        memcpy(hash->buffer, input, length);
        hash->buffered = length;
    }
}

void tb_sha256_final(tb_sha256_state *hash, uint8_t digest[TB_SHA256_SIZE])
{
    uint64_t bits = 0;
    TB_ASSERT(hash != NULL);
    TB_ASSERT(digest != NULL);
    bits = hash->length * 8u;
    /* A one bit, zeros to 8 bytes short of a block, then the length in bits. */
    hash->buffer[hash->buffered] = 0x80u;
    hash->buffered++;
    if (hash->buffered > TB_SHA256_BLOCK - 8u)
    {
        memset(hash->buffer + hash->buffered, 0, TB_SHA256_BLOCK - hash->buffered);
        tb_sha256_block(hash->state, hash->buffer);
        hash->buffered = 0;
    }
    memset(hash->buffer + hash->buffered, 0, TB_SHA256_BLOCK - 8u - hash->buffered);
    tb_sha256_store(hash->buffer + TB_SHA256_BLOCK - 8u, (uint32_t)(bits >> 32));
    tb_sha256_store(hash->buffer + TB_SHA256_BLOCK - 4u, (uint32_t)(bits & 0xFFFFFFFFu));
    tb_sha256_block(hash->state, hash->buffer);
    for (size_t index = 0; index < 8; index++)
    {
        tb_sha256_store(digest + 4 * index, hash->state[index]);
    }
    tb_sha256_wipe(hash, sizeof(*hash));
}

void tb_sha256(const void *bytes, size_t length, uint8_t digest[TB_SHA256_SIZE])
{
    tb_sha256_state hash = {0};
    tb_sha256_init(&hash);
    tb_sha256_update(&hash, bytes, length);
    tb_sha256_final(&hash, digest);
}

void tb_hmac_sha256_init(tb_hmac_sha256_state *mac, const void *key, size_t key_length)
{
    unsigned char block[TB_SHA256_BLOCK] = {0};
    unsigned char pad[TB_SHA256_BLOCK] = {0};
    TB_ASSERT(mac != NULL);
    TB_ASSERT(key != NULL || key_length == 0);
    if (key_length > TB_SHA256_BLOCK)
    {
        tb_sha256(key, key_length, block);
    }
    else if (key_length > 0)
    {
        memcpy(block, key, key_length);
    }
    for (size_t index = 0; index < TB_SHA256_BLOCK; index++)
    {
        pad[index] = (unsigned char)(block[index] ^ TB_HMAC_INNER_PAD);
    }
    tb_sha256_init(&mac->inner);
    tb_sha256_update(&mac->inner, pad, sizeof(pad));
    for (size_t index = 0; index < TB_SHA256_BLOCK; index++)
    {
        pad[index] = (unsigned char)(block[index] ^ TB_HMAC_OUTER_PAD);
    }
    tb_sha256_init(&mac->outer);
    tb_sha256_update(&mac->outer, pad, sizeof(pad));
    /* The key and what was made of it are secrets. */
    tb_sha256_wipe(block, sizeof(block));
    tb_sha256_wipe(pad, sizeof(pad));
}

void tb_hmac_sha256_update(tb_hmac_sha256_state *mac, const void *bytes, size_t length)
{
    TB_ASSERT(mac != NULL);
    tb_sha256_update(&mac->inner, bytes, length);
}

void tb_hmac_sha256_final(tb_hmac_sha256_state *mac, uint8_t result[TB_SHA256_SIZE])
{
    uint8_t inner[TB_SHA256_SIZE] = {0};
    TB_ASSERT(mac != NULL);
    TB_ASSERT(result != NULL);
    tb_sha256_final(&mac->inner, inner);
    tb_sha256_update(&mac->outer, inner, sizeof(inner));
    tb_sha256_final(&mac->outer, result);
    tb_sha256_wipe(inner, sizeof(inner));
}

void tb_hmac_sha256(const void *key, size_t key_length, const void *bytes, size_t length,
                    uint8_t result[TB_SHA256_SIZE])
{
    tb_hmac_sha256_state mac = {0};
    tb_hmac_sha256_init(&mac, key, key_length);
    tb_hmac_sha256_update(&mac, bytes, length);
    tb_hmac_sha256_final(&mac, result);
}
