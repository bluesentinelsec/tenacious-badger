/* What the PNG reader and writer share: the layout of chunks and headers,
 * and big-endian numbers. */
#ifndef TB_BITMAP_PNG_H
#define TB_BITMAP_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Bytes of the signature, and of a chunk's length, name and CRC. */
#define PNG_SIGNATURE_BYTES 8u
#define PNG_CHUNK_HEAD 8u
#define PNG_CHUNK_CRC 4u

/* The longest chunk PNG allows: lengths are below 2^31. */
#define PNG_LONGEST_CHUNK 0x7FFFFFFFu

/* The image header: width, height, bit depth, color type, compression,
 * filter method and interlacing. */
#define PNG_IHDR_BYTES 13u

/* Color types. */
#define PNG_GRAY 0u
#define PNG_RGB 2u
#define PNG_PALETTE 3u
#define PNG_GRAY_ALPHA 4u
#define PNG_RGBA 6u

/* Filter types of a row. */
#define PNG_FILTER_NONE 0u
#define PNG_FILTER_SUB 1u
#define PNG_FILTER_UP 2u
#define PNG_FILTER_AVERAGE 3u
#define PNG_FILTER_PAETH 4u
#define PNG_FILTER_COUNT 5u

static inline uint32_t png_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 |
           (uint32_t)bytes[3];
}

static inline void png_put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

/* The Paeth predictor of a byte from its neighbours to the left, above,
 * and above to the left: whichever is nearest their sum less the corner. */
static inline uint8_t png_paeth(uint8_t left, uint8_t above, uint8_t corner)
{
    const int estimate = (int)left + (int)above - (int)corner;
    const int to_left = estimate > left ? estimate - left : left - estimate;
    const int to_above = estimate > above ? estimate - above : above - estimate;
    const int to_corner = estimate > corner ? estimate - corner : corner - estimate;

    if (to_left <= to_above && to_left <= to_corner)
    {
        return left;
    }
    if (to_above <= to_corner)
    {
        return above;
    }
    return corner;
}

#endif
