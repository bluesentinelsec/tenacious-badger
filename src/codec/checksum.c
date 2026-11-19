#include "badger/checksum.h"

#include "badger/assert.h"
#include "crc32_table.h"

/* Adler-32 sums modulo the largest prime below 2^16. */
#define TB_ADLER32_MODULUS UINT32_C(65521)

/* The most bytes Adler-32 can add before its sums must be reduced: the
 * largest n with 255 * n * (n + 1) / 2 + (n + 1) * (MODULUS - 1) below 2^32,
 * as zlib computes it. */
#define TB_ADLER32_RUN ((size_t)5552)

/* Bytes that slicing-by-8 combines in one step. */
#define TB_CRC32_SLICE ((size_t)8)

static uint32_t tb_crc32_load32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

uint32_t tb_crc32_update(uint32_t crc, const void *bytes, size_t length)
{
    const unsigned char *input = bytes;
    uint32_t value = ~crc;
    TB_ASSERT(bytes != NULL || length == 0);
    /* Eight bytes per step: the CRC absorbs the first four, and each of the
     * eight bytes is advanced past the bytes that follow it by its own table. */
    while (length >= TB_CRC32_SLICE)
    {
        const uint32_t low = value ^ tb_crc32_load32(input);
        const uint32_t high = tb_crc32_load32(input + 4);
        value = tb_crc32_table[7][low & 0xFFu] ^ tb_crc32_table[6][(low >> 8) & 0xFFu] ^
                tb_crc32_table[5][(low >> 16) & 0xFFu] ^ tb_crc32_table[4][low >> 24] ^
                tb_crc32_table[3][high & 0xFFu] ^ tb_crc32_table[2][(high >> 8) & 0xFFu] ^
                tb_crc32_table[1][(high >> 16) & 0xFFu] ^ tb_crc32_table[0][high >> 24];
        input += TB_CRC32_SLICE;
        length -= TB_CRC32_SLICE;
    }
    while (length > 0)
    {
        value = (value >> 8) ^ tb_crc32_table[0][(value ^ *input) & 0xFFu];
        input++;
        length--;
    }
    return ~value;
}

uint32_t tb_crc32(const void *bytes, size_t length)
{
    return tb_crc32_update(TB_CRC32_INITIAL, bytes, length);
}

uint32_t tb_adler32_update(uint32_t adler, const void *bytes, size_t length)
{
    const unsigned char *input = bytes;
    uint32_t low = adler & 0xFFFFu;
    uint32_t high = adler >> 16;
    TB_ASSERT(bytes != NULL || length == 0);
    while (length > 0)
    {
        size_t run = length;
        if (run > TB_ADLER32_RUN)
        {
            run = TB_ADLER32_RUN;
        }
        length -= run;
        while (run > 0)
        {
            low += *input;
            high += low;
            input++;
            run--;
        }
        low %= TB_ADLER32_MODULUS;
        high %= TB_ADLER32_MODULUS;
    }
    return (high << 16) | low;
}

uint32_t tb_adler32(const void *bytes, size_t length)
{
    return tb_adler32_update(TB_ADLER32_INITIAL, bytes, length);
}
