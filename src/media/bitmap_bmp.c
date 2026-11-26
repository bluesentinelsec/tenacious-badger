/* BMP files: Windows' device-independent bitmaps, read in every form that
 * tools still write, and written as 32 bits with alpha, or 24 without.
 *
 * A file is a file header of 14 bytes, an information header whose size
 * says its version, color masks or a palette, and the pixels, at an offset
 * the file header gives. Every number is little-endian. Each is checked
 * against the bytes there are before it is used: the file is untrusted. */
#include "bitmap_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <string.h>

/* Bytes of the file header: "BM", the file's size, two reserved words, and
 * the offset of the pixels. */
#define BMP_FILE_HEADER 14u
#define BMP_PIXEL_OFFSET_AT 10u
#define BMP_INFO_AT BMP_FILE_HEADER

/* Sizes of the information headers, which name their versions. */
#define BMP_CORE_HEADER 12u      /* BITMAPCOREHEADER, of OS/2 1.x and Windows 2. */
#define BMP_OS2_HEADER 64u       /* OS/2 2.x's BITMAPCOREHEADER2. */
#define BMP_OS2_SHORT_HEADER 16u /* The same, cut short, as OS/2 allows. */
#define BMP_INFO_HEADER 40u      /* BITMAPINFOHEADER. */
#define BMP_V2_HEADER 52u        /* With the color masks. */
#define BMP_V3_HEADER 56u        /* With the alpha mask too. */
#define BMP_V4_HEADER 108u       /* BITMAPV4HEADER: color spaces. */
#define BMP_V5_HEADER 124u       /* BITMAPV5HEADER: color profiles. */

/* Offsets of the fields of the information headers, from the file's start. */
#define BMP_CORE_WIDTH_AT 18u
#define BMP_CORE_HEIGHT_AT 20u
#define BMP_CORE_PLANES_AT 22u
#define BMP_CORE_BITS_AT 24u
#define BMP_WIDTH_AT 18u
#define BMP_HEIGHT_AT 22u
#define BMP_PLANES_AT 26u
#define BMP_BITS_AT 28u
#define BMP_COMPRESSION_AT 30u
#define BMP_COLORS_USED_AT 46u
#define BMP_MASKS_AT 54u

/* Compressions. */
#define BMP_RGB 0u
#define BMP_RLE8 1u
#define BMP_RLE4 2u
#define BMP_BITFIELDS 3u
#define BMP_JPEG 4u
#define BMP_PNG 5u
#define BMP_ALPHABITFIELDS 6u
#define BMP_CMYK 11u
#define BMP_CMYK_RLE8 12u
#define BMP_CMYK_RLE4 13u

/* The most colors a palette has: what 8 bits address. */
#define BMP_MOST_COLORS 256u

/* What a 32-bit file of the version 5 header that the writer writes holds:
 * its masks, its color space ("sRGB"), its rendering intent (pictures),
 * and its pixels per meter (72 per inch). */
#define BMP_RED_MASK 0x00FF0000u
#define BMP_GREEN_MASK 0x0000FF00u
#define BMP_BLUE_MASK 0x000000FFu
#define BMP_ALPHA_MASK 0xFF000000u
#define BMP_SRGB 0x73524742u
#define BMP_INTENT_IMAGES 4u
#define BMP_PIXELS_PER_METER 2835u
#define BMP_V5_CS_TYPE_AT 56u
#define BMP_V5_INTENT_AT 108u

/* The masks of 16-bit pixels without masks of their own: 5 bits a channel. */
#define BMP_RED_555 0x7C00u
#define BMP_GREEN_555 0x03E0u
#define BMP_BLUE_555 0x001Fu

/* Escapes of run-length encoding, after a count of zero. */
#define BMP_RLE_END_OF_LINE 0u
#define BMP_RLE_END_OF_BITMAP 1u
#define BMP_RLE_DELTA 2u

/* ---------------------------------------------------------------------------
 * Reading.
 * ------------------------------------------------------------------------- */

static uint32_t bmp_u16(const uint8_t *bytes, size_t at)
{
    return (uint32_t)bytes[at] | (uint32_t)bytes[at + 1u] << 8;
}

static uint32_t bmp_u32(const uint8_t *bytes, size_t at)
{
    return (uint32_t)bytes[at] | (uint32_t)bytes[at + 1u] << 8 | (uint32_t)bytes[at + 2u] << 16 |
           (uint32_t)bytes[at + 3u] << 24;
}

static int64_t bmp_s32(const uint8_t *bytes, size_t at)
{
    const uint32_t value = bmp_u32(bytes, at);
    return value <= INT32_MAX ? (int64_t)value : (int64_t)value - ((int64_t)1 << 32);
}

/* Where one channel is in a pixel of 16 or 32 bits. */
typedef struct bmp_channel
{
    uint32_t mask;
    uint32_t shift;
    uint64_t largest; /* The mask's value shifted down: the channel at full. */
} bmp_channel;

/* What the headers say. */
typedef struct bmp_layout
{
    uint32_t width;
    uint32_t height;
    bool top_down;
    uint32_t bits;
    size_t bits_at; /* Where the header says them, for errors. */
    uint32_t compression;
    size_t pixel_offset;
    bmp_channel channels[4]; /* Red, green, blue, alpha; an alpha of no mask is opaque. */
    /* A 32-bit file of no masks keeps alpha in its fourth byte, or keeps
     * nothing there: alpha is used unless every pixel's is zero. */
    bool guess_alpha;
    tb_bitmap_color palette[BMP_MOST_COLORS];
    size_t colors;
} bmp_layout;

typedef struct bmp_reader
{
    const uint8_t *bytes;
    size_t length;
    tb_bitmap_error *error;
} bmp_reader;

static tb_bitmap_result bmp_malformed(const bmp_reader *reader, size_t offset, const char *reason)
{
    return tb_bitmap_fail(reader->error, TB_BITMAP_MALFORMED, offset, reason);
}

/* A mask: contiguous bits within the pixel's. */
static tb_bitmap_result bmp_channel_of(const bmp_reader *reader, uint32_t mask, uint32_t bits,
                                       size_t at, bmp_channel *channel)
{
    *channel = (bmp_channel){0};
    if (mask == 0)
    {
        return TB_BITMAP_OK;
    }
    if (bits < 32u && mask >> bits != 0)
    {
        return bmp_malformed(reader, at, "a mask outside the pixel's bits");
    }
    uint32_t shift = 0;
    while ((mask >> shift & 1u) == 0)
    {
        shift++;
    }
    const uint64_t largest = (uint64_t)mask >> shift;
    if ((largest & (largest + 1u)) != 0)
    {
        return bmp_malformed(reader, at, "a mask whose bits are not together");
    }
    *channel = (bmp_channel){mask, shift, largest};
    return TB_BITMAP_OK;
}

static uint8_t bmp_channel_value(const bmp_channel *channel, uint32_t pixel)
{
    if (channel->mask == 0)
    {
        return 0;
    }
    const uint64_t value = (uint64_t)((pixel & channel->mask) >> channel->shift);
    return (uint8_t)((value * UINT8_MAX + channel->largest / 2u) / channel->largest);
}

/* The masks of 16 and 32-bit pixels: the headers', or the defaults. */
static tb_bitmap_result bmp_read_masks(const bmp_reader *reader, uint32_t header,
                                       bmp_layout *layout)
{
    uint32_t masks[4] = {0};
    const bool has_masks =
        layout->compression == BMP_BITFIELDS || layout->compression == BMP_ALPHABITFIELDS;

    if (!has_masks && layout->bits == 16u)
    {
        masks[0] = BMP_RED_555;
        masks[1] = BMP_GREEN_555;
        masks[2] = BMP_BLUE_555;
    }
    if (!has_masks && layout->bits == 32u)
    {
        masks[0] = BMP_RED_MASK;
        masks[1] = BMP_GREEN_MASK;
        masks[2] = BMP_BLUE_MASK;
        masks[3] = BMP_ALPHA_MASK;
        layout->guess_alpha = true;
    }
    if (has_masks)
    {
        /* A header of version 2 or later holds the masks; the plain one is
         * followed by them, before the pixels. Only a version 3 header or
         * later, or BI_ALPHABITFIELDS, has a mask of alpha. */
        const bool alpha = header >= BMP_V3_HEADER || layout->compression == BMP_ALPHABITFIELDS;
        const size_t count = alpha ? 4u : 3u;
        for (size_t index = 0; index < count; index++)
        {
            masks[index] = bmp_u32(reader->bytes, BMP_MASKS_AT + index * 4u);
        }
    }
    for (size_t index = 0; index < 4u; index++)
    {
        const tb_bitmap_result result =
            bmp_channel_of(reader, masks[index], layout->bits, BMP_MASKS_AT + index * 4u,
                           &layout->channels[index]);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
    }
    return TB_BITMAP_OK;
}

/* The palette of 1, 4 and 8-bit pixels: as many colors as the header says,
 * or as the bits address, but no more than fit before the pixels, which
 * begin within the file. */
static tb_bitmap_result bmp_read_palette(const bmp_reader *reader, uint32_t header,
                                         uint32_t colors_used, bmp_layout *layout)
{
    const size_t entry = header == BMP_CORE_HEADER ? 3u : 4u;
    const size_t start = BMP_INFO_AT + header;
    size_t colors = colors_used != 0 ? colors_used : (size_t)1 << layout->bits;

    if (colors_used > BMP_MOST_COLORS)
    {
        return bmp_malformed(reader, BMP_COLORS_USED_AT, "a palette of more than 256 colors");
    }
    if ((layout->pixel_offset - start) / entry < colors)
    {
        colors = (layout->pixel_offset - start) / entry;
    }
    if (colors == 0)
    {
        return bmp_malformed(reader, start, "no room for the palette");
    }
    for (size_t index = 0; index < colors; index++)
    {
        const uint8_t *const color = reader->bytes + start + index * entry;
        layout->palette[index] = (tb_bitmap_color){color[2], color[1], color[0], UINT8_MAX};
    }
    layout->colors = colors;
    return TB_BITMAP_OK;
}

/* The compression goes with the bits of a pixel and the order of rows. */
static tb_bitmap_result bmp_check_compression(const bmp_reader *reader, const bmp_layout *layout)
{
    const uint32_t compression = layout->compression;
    const uint32_t bits = layout->bits;

    switch (compression)
    {
    case BMP_RGB:
        if (bits == 1u || bits == 4u || bits == 8u || bits == 16u || bits == 24u || bits == 32u)
        {
            return TB_BITMAP_OK;
        }
        return bmp_malformed(reader, layout->bits_at, "bits per pixel no BMP file has");
    case BMP_RLE8:
    case BMP_RLE4:
        if (bits != (compression == BMP_RLE8 ? 8u : 4u))
        {
            return bmp_malformed(reader, BMP_COMPRESSION_AT,
                                 "a compression the bits per pixel cannot have");
        }
        if (layout->top_down)
        {
            return bmp_malformed(reader, BMP_HEIGHT_AT, "compressed rows stored from the top");
        }
        return TB_BITMAP_OK;
    case BMP_BITFIELDS:
    case BMP_ALPHABITFIELDS:
        if (bits == 16u || bits == 32u)
        {
            return TB_BITMAP_OK;
        }
        return bmp_malformed(reader, BMP_COMPRESSION_AT,
                             "a compression the bits per pixel cannot have");
    case BMP_JPEG:
        return tb_bitmap_fail(reader->error, TB_BITMAP_UNSUPPORTED, BMP_COMPRESSION_AT,
                              "a BMP file holding a JPEG image");
    case BMP_PNG:
        return tb_bitmap_fail(reader->error, TB_BITMAP_UNSUPPORTED, BMP_COMPRESSION_AT,
                              "a BMP file holding a PNG image");
    case BMP_CMYK:
    case BMP_CMYK_RLE8:
    case BMP_CMYK_RLE4:
        return tb_bitmap_fail(reader->error, TB_BITMAP_UNSUPPORTED, BMP_COMPRESSION_AT,
                              "CMYK pixels");
    default:
        return bmp_malformed(reader, BMP_COMPRESSION_AT, "an unknown compression");
    }
}

/* The dimensions, bits and compression of the information header. */
static tb_bitmap_result bmp_read_header(const bmp_reader *reader, uint32_t header,
                                        bmp_layout *layout, uint32_t *colors_used)
{
    const uint8_t *const bytes = reader->bytes;
    int64_t width = 0;
    int64_t height = 0;
    uint32_t planes = 0;
    size_t planes_at = BMP_PLANES_AT;

    if (header == BMP_CORE_HEADER)
    {
        width = bmp_u16(bytes, BMP_CORE_WIDTH_AT);
        height = bmp_u16(bytes, BMP_CORE_HEIGHT_AT);
        planes = bmp_u16(bytes, BMP_CORE_PLANES_AT);
        planes_at = BMP_CORE_PLANES_AT;
        layout->bits = bmp_u16(bytes, BMP_CORE_BITS_AT);
        layout->bits_at = BMP_CORE_BITS_AT;
        layout->compression = BMP_RGB;
    }
    else
    {
        width = bmp_s32(bytes, BMP_WIDTH_AT);
        height = bmp_s32(bytes, BMP_HEIGHT_AT);
        planes = bmp_u16(bytes, BMP_PLANES_AT);
        layout->bits = bmp_u16(bytes, BMP_BITS_AT);
        layout->bits_at = BMP_BITS_AT;
        layout->compression = bmp_u32(bytes, BMP_COMPRESSION_AT);
        *colors_used = bmp_u32(bytes, BMP_COLORS_USED_AT);
    }
    if (planes != 1u)
    {
        return bmp_malformed(reader, planes_at, "planes other than 1");
    }
    if (width <= 0)
    {
        return bmp_malformed(reader, BMP_WIDTH_AT, "a width of no pixels");
    }
    if (height == 0 || height == INT32_MIN)
    {
        return bmp_malformed(reader, header == BMP_CORE_HEADER ? BMP_CORE_HEIGHT_AT : BMP_HEIGHT_AT,
                             "a height of no pixels");
    }
    layout->top_down = height < 0;
    layout->width = (uint32_t)width;
    layout->height = (uint32_t)(height < 0 ? -height : height);
    return bmp_check_compression(reader, layout);
}

/* The headers, as far as the pixels. */
static tb_bitmap_result bmp_read_layout(const bmp_reader *reader, uint32_t max_side,
                                        uint64_t max_pixels, bmp_layout *layout)
{
    uint32_t colors_used = 0;
    tb_bitmap_result result = TB_BITMAP_OK;

    if (reader->length < BMP_FILE_HEADER + 4u)
    {
        return bmp_malformed(reader, reader->length, "the file header ends early");
    }
    const uint32_t header = bmp_u32(reader->bytes, BMP_INFO_AT);
    if (header == BMP_OS2_HEADER || header == BMP_OS2_SHORT_HEADER)
    {
        return tb_bitmap_fail(reader->error, TB_BITMAP_UNSUPPORTED, BMP_INFO_AT,
                              "an OS/2 2.x header");
    }
    if (header != BMP_CORE_HEADER && header != BMP_INFO_HEADER && header != BMP_V2_HEADER &&
        header != BMP_V3_HEADER && header != BMP_V4_HEADER && header != BMP_V5_HEADER)
    {
        return bmp_malformed(reader, BMP_INFO_AT, "a header of an unknown size");
    }
    if (BMP_INFO_AT + (size_t)header > reader->length)
    {
        return bmp_malformed(reader, reader->length, "the header ends early");
    }
    result = bmp_read_header(reader, header, layout, &colors_used);
    if (result != TB_BITMAP_OK)
    {
        return result;
    }
    if (layout->width > max_side || layout->height > max_side ||
        (uint64_t)layout->width * layout->height > max_pixels)
    {
        return tb_bitmap_fail(reader->error, TB_BITMAP_TOO_LARGE, BMP_WIDTH_AT,
                              "dimensions over the limits");
    }
    /* The pixels follow the headers, the masks that follow the plain one,
     * and any palette. */
    const bool trailing_masks =
        header == BMP_INFO_HEADER &&
        (layout->compression == BMP_BITFIELDS || layout->compression == BMP_ALPHABITFIELDS);
    const size_t mask_bytes = !trailing_masks                             ? 0u
                              : layout->compression == BMP_ALPHABITFIELDS ? 16u
                                                                          : 12u;
    layout->pixel_offset = bmp_u32(reader->bytes, BMP_PIXEL_OFFSET_AT);
    if (layout->pixel_offset < BMP_INFO_AT + header + mask_bytes)
    {
        return bmp_malformed(reader, BMP_PIXEL_OFFSET_AT, "the pixels begin inside the headers");
    }
    if (layout->pixel_offset > reader->length)
    {
        return bmp_malformed(reader, BMP_PIXEL_OFFSET_AT, "the pixels begin past the end");
    }
    if (layout->bits <= 8u)
    {
        return bmp_read_palette(reader, header, colors_used, layout);
    }
    return bmp_read_masks(reader, header, layout);
}

/* A pixel of a palette, or the error of an index past it. */
static tb_bitmap_result bmp_palette_color(const bmp_reader *reader, const bmp_layout *layout,
                                          uint32_t index, size_t at, tb_bitmap_color *color)
{
    if (index >= layout->colors)
    {
        return bmp_malformed(reader, at, "a pixel's index past the palette");
    }
    *color = layout->palette[index];
    return TB_BITMAP_OK;
}

static tb_bitmap_color bmp_masked_color(const bmp_layout *layout, uint32_t pixel)
{
    const bmp_channel *const channels = layout->channels;
    const uint8_t alpha =
        channels[3].mask != 0 ? bmp_channel_value(&channels[3], pixel) : (uint8_t)UINT8_MAX;
    return (tb_bitmap_color){bmp_channel_value(&channels[0], pixel),
                             bmp_channel_value(&channels[1], pixel),
                             bmp_channel_value(&channels[2], pixel), alpha};
}

static void bmp_put(tb_bitmap *bitmap, uint32_t x, uint32_t y, tb_bitmap_color color)
{
    uint8_t *const pixel = tb_bitmap_row(bitmap, y) + (size_t)x * 4u;
    pixel[0] = color.r;
    pixel[1] = color.g;
    pixel[2] = color.b;
    pixel[3] = color.a;
}

/* One row of pixels that are not compressed. */
static tb_bitmap_result bmp_decode_row(const bmp_reader *reader, const bmp_layout *layout,
                                       size_t at, tb_bitmap *bitmap, uint32_t y)
{
    const uint8_t *const row = reader->bytes + at;
    tb_bitmap_result result = TB_BITMAP_OK;

    for (uint32_t x = 0; x < layout->width && result == TB_BITMAP_OK; x++)
    {
        tb_bitmap_color color = {0};
        const size_t bit = (size_t)x * layout->bits;
        const size_t byte = bit / 8u;
        switch (layout->bits)
        {
        case 1u:
        case 4u:
        case 8u: {
            const uint32_t shift = 8u - layout->bits - (uint32_t)(bit % 8u);
            const uint32_t index = (uint32_t)(row[byte] >> shift) & ((1u << layout->bits) - 1u);
            result = bmp_palette_color(reader, layout, index, at + byte, &color);
            break;
        }
        case 16u:
            color = bmp_masked_color(layout, bmp_u16(row, byte));
            break;
        case 24u:
            color = (tb_bitmap_color){row[byte + 2u], row[byte + 1u], row[byte], UINT8_MAX};
            break;
        default:
            color = bmp_masked_color(layout, bmp_u32(row, byte));
            break;
        }
        bmp_put(bitmap, x, y, color);
    }
    return result;
}

/* Every pixel's alpha is zero: the fourth byte held nothing. */
static void bmp_make_opaque(tb_bitmap *bitmap)
{
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        uint8_t *const row = tb_bitmap_row(bitmap, y);
        for (uint32_t x = 0; x < bitmap->width; x++)
        {
            row[(size_t)x * 4u + 3u] = UINT8_MAX;
        }
    }
}

static bool bmp_has_alpha(const tb_bitmap *bitmap)
{
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        const uint8_t *const row = tb_bitmap_const_row(bitmap, y);
        for (uint32_t x = 0; x < bitmap->width; x++)
        {
            if (row[(size_t)x * 4u + 3u] != 0)
            {
                return true;
            }
        }
    }
    return false;
}

/* Bytes from a row of pixels that are not compressed to the next: rows are
 * padded to whole 32-bit words. */
static uint64_t bmp_stride(const bmp_layout *layout)
{
    return ((uint64_t)layout->width * layout->bits + 31u) / 32u * 4u;
}

/* Every row of pixels that are not compressed is in the file; the last may
 * lack its padding, as some writers leave it off. */
static tb_bitmap_result bmp_check_rows(const bmp_reader *reader, const bmp_layout *layout)
{
    const uint64_t last_row = ((uint64_t)layout->width * layout->bits + 7u) / 8u;
    const uint64_t needed = bmp_stride(layout) * (layout->height - 1u) + last_row;

    if (needed > reader->length - layout->pixel_offset)
    {
        return bmp_malformed(reader, reader->length, "the pixels end early");
    }
    return TB_BITMAP_OK;
}

static tb_bitmap_result bmp_decode_rows(const bmp_reader *reader, const bmp_layout *layout,
                                        tb_bitmap *bitmap)
{
    const uint64_t stride = bmp_stride(layout);

    for (uint32_t row = 0; row < layout->height; row++)
    {
        const uint32_t y = layout->top_down ? row : layout->height - 1u - row;
        const size_t at = layout->pixel_offset + (size_t)(stride * row);
        const tb_bitmap_result result = bmp_decode_row(reader, layout, at, bitmap, y);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
    }
    if (layout->guess_alpha && !bmp_has_alpha(bitmap))
    {
        bmp_make_opaque(bitmap);
    }
    return TB_BITMAP_OK;
}

/* Where run-length decoding is: the next byte, and the pixel, counting rows
 * from the bottom as the file does. */
typedef struct bmp_rle
{
    size_t at;
    uint32_t x;
    uint32_t y;
} bmp_rle;

/* Put `count` indices of a run, or of an absolute stretch, at the cursor. */
static tb_bitmap_result bmp_rle_put(const bmp_reader *reader, const bmp_layout *layout,
                                    bmp_rle *cursor, uint32_t index, size_t at, tb_bitmap *bitmap)
{
    tb_bitmap_color color = {0};

    if (cursor->y >= layout->height || cursor->x >= layout->width)
    {
        return bmp_malformed(reader, at, "a run past the edge of the image");
    }
    const tb_bitmap_result result = bmp_palette_color(reader, layout, index, at, &color);
    if (result != TB_BITMAP_OK)
    {
        return result;
    }
    bmp_put(bitmap, cursor->x, layout->height - 1u - cursor->y, color);
    cursor->x++;
    return TB_BITMAP_OK;
}

/* The index of the `n`th pixel of a run or stretch: one byte each at 8 bits,
 * and at 4, two in a byte, the high nibble first. */
static uint32_t bmp_rle_index(uint32_t bits, uint8_t byte, uint32_t n)
{
    if (bits == 8u)
    {
        return byte;
    }
    return (n % 2u == 0) ? (uint32_t)(byte >> 4) : (uint32_t)(byte & 0x0Fu);
}

/* An absolute stretch of `count` pixels, padded to a whole 16-bit word. */
static tb_bitmap_result bmp_rle_absolute(const bmp_reader *reader, const bmp_layout *layout,
                                         bmp_rle *cursor, uint32_t count, tb_bitmap *bitmap)
{
    const size_t bytes = layout->bits == 8u ? count : (count + 1u) / 2u;
    const size_t padded = bytes + bytes % 2u;

    if (padded > reader->length - cursor->at)
    {
        return bmp_malformed(reader, reader->length, "the compressed pixels end early");
    }
    for (uint32_t n = 0; n < count; n++)
    {
        const size_t at = cursor->at + (layout->bits == 8u ? n : n / 2u);
        const uint32_t index = bmp_rle_index(layout->bits, reader->bytes[at], n);
        const tb_bitmap_result result = bmp_rle_put(reader, layout, cursor, index, at, bitmap);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
    }
    cursor->at += padded;
    return TB_BITMAP_OK;
}

/* RLE8 and RLE4. Pixels no run reaches stay transparent black. A file may
 * end between commands without saying so. */
static tb_bitmap_result bmp_decode_rle(const bmp_reader *reader, const bmp_layout *layout,
                                       tb_bitmap *bitmap)
{
    bmp_rle cursor = {.at = layout->pixel_offset};
    tb_bitmap_result result = TB_BITMAP_OK;

    while (result == TB_BITMAP_OK && cursor.at < reader->length)
    {
        const size_t command = cursor.at;
        if (reader->length - command < 2u)
        {
            return bmp_malformed(reader, reader->length, "the compressed pixels end early");
        }
        const uint32_t count = reader->bytes[command];
        const uint8_t value = reader->bytes[command + 1u];
        cursor.at += 2u;
        if (count > 0)
        {
            for (uint32_t n = 0; n < count && result == TB_BITMAP_OK; n++)
            {
                result = bmp_rle_put(reader, layout, &cursor, bmp_rle_index(layout->bits, value, n),
                                     command, bitmap);
            }
            continue;
        }
        switch (value)
        {
        case BMP_RLE_END_OF_LINE:
            cursor.x = 0;
            cursor.y++;
            break;
        case BMP_RLE_END_OF_BITMAP:
            return TB_BITMAP_OK;
        case BMP_RLE_DELTA:
            if (reader->length - cursor.at < 2u)
            {
                return bmp_malformed(reader, reader->length, "the compressed pixels end early");
            }
            cursor.x += reader->bytes[cursor.at];
            cursor.y += reader->bytes[cursor.at + 1u];
            cursor.at += 2u;
            if (cursor.x > layout->width || cursor.y > layout->height)
            {
                return bmp_malformed(reader, command, "a move past the edge of the image");
            }
            break;
        default:
            result = bmp_rle_absolute(reader, layout, &cursor, value, bitmap);
            break;
        }
    }
    return result;
}

tb_bitmap_result tb_bitmap_decode_bmp(const uint8_t *bytes, size_t length, uint32_t max_side,
                                      uint64_t max_pixels, const tb_allocator *allocator,
                                      tb_bitmap **bitmap, tb_bitmap_error *error)
{
    const bmp_reader reader = {bytes, length, error};
    bmp_layout layout = {0};
    tb_bitmap *decoded = NULL;
    const tb_bitmap_result checked = bmp_read_layout(&reader, max_side, max_pixels, &layout);
    const bool compressed = layout.compression == BMP_RLE8 || layout.compression == BMP_RLE4;
    tb_bitmap_result result = TB_BITMAP_OK;

    if (checked != TB_BITMAP_OK)
    {
        return checked;
    }
    if (!compressed)
    {
        result = bmp_check_rows(&reader, &layout);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
    }
    decoded = tb_bitmap_try_create(layout.width, layout.height, TB_BITMAP_RGBA8, allocator);
    if (decoded == NULL)
    {
        return tb_bitmap_fail(error, TB_BITMAP_NO_MEMORY, TB_BITMAP_NO_OFFSET,
                              "no memory for the pixels");
    }
    if (compressed)
    {
        result = bmp_decode_rle(&reader, &layout, decoded);
    }
    else
    {
        result = bmp_decode_rows(&reader, &layout, decoded);
    }
    if (result != TB_BITMAP_OK)
    {
        tb_bitmap_destroy(decoded);
        return result;
    }
    *bitmap = decoded;
    return TB_BITMAP_OK;
}

/* ---------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

static void bmp_put_u16(uint8_t *bytes, size_t at, uint32_t value)
{
    bytes[at] = (uint8_t)value;
    bytes[at + 1u] = (uint8_t)(value >> 8);
}

static void bmp_put_u32(uint8_t *bytes, size_t at, uint32_t value)
{
    bytes[at] = (uint8_t)value;
    bytes[at + 1u] = (uint8_t)(value >> 8);
    bytes[at + 2u] = (uint8_t)(value >> 16);
    bytes[at + 3u] = (uint8_t)(value >> 24);
}

/* The headers of a file of `stride`-byte rows; `header` is their size. */
static void bmp_write_headers(uint8_t *headers, const tb_bitmap *bitmap, tb_bitmap_bmp_depth depth,
                              uint32_t stride, uint32_t file_size)
{
    const bool alpha = depth == TB_BITMAP_BMP_32;
    const uint32_t info = alpha ? BMP_V5_HEADER : BMP_INFO_HEADER;

    headers[0] = 'B';
    headers[1] = 'M';
    bmp_put_u32(headers, 2u, file_size);
    bmp_put_u32(headers, BMP_PIXEL_OFFSET_AT, BMP_FILE_HEADER + info);
    bmp_put_u32(headers, BMP_INFO_AT, info);
    bmp_put_u32(headers, BMP_WIDTH_AT, bitmap->width);
    bmp_put_u32(headers, BMP_HEIGHT_AT, bitmap->height);
    bmp_put_u16(headers, BMP_PLANES_AT, 1u);
    bmp_put_u16(headers, BMP_BITS_AT, alpha ? 32u : 24u);
    bmp_put_u32(headers, BMP_COMPRESSION_AT, alpha ? BMP_BITFIELDS : BMP_RGB);
    bmp_put_u32(headers, BMP_COMPRESSION_AT + 4u, stride * bitmap->height);
    bmp_put_u32(headers, BMP_COMPRESSION_AT + 8u, BMP_PIXELS_PER_METER);
    bmp_put_u32(headers, BMP_COMPRESSION_AT + 12u, BMP_PIXELS_PER_METER);
    if (!alpha)
    {
        return;
    }
    bmp_put_u32(headers, BMP_MASKS_AT, BMP_RED_MASK);
    bmp_put_u32(headers, BMP_MASKS_AT + 4u, BMP_GREEN_MASK);
    bmp_put_u32(headers, BMP_MASKS_AT + 8u, BMP_BLUE_MASK);
    bmp_put_u32(headers, BMP_MASKS_AT + 12u, BMP_ALPHA_MASK);
    bmp_put_u32(headers, BMP_INFO_AT + BMP_V5_CS_TYPE_AT, BMP_SRGB);
    bmp_put_u32(headers, BMP_INFO_AT + BMP_V5_INTENT_AT, BMP_INTENT_IMAGES);
}

/* A row of the file: BGRA or BGR, straight, padded with zeros. */
static void bmp_encode_row(const tb_bitmap *bitmap, uint32_t y, tb_bitmap_bmp_depth depth,
                           uint8_t *row)
{
    const size_t bytes = depth == TB_BITMAP_BMP_32 ? 4u : 3u;

    for (uint32_t x = 0; x < bitmap->width; x++)
    {
        tb_bitmap_color color = tb_bitmap_get_pixel(bitmap, x, y);
        if (bitmap->premultiplied)
        {
            color = tb_bitmap_unpremultiplied_color(color);
        }
        uint8_t *const pixel = row + (size_t)x * bytes;
        pixel[0] = color.b;
        pixel[1] = color.g;
        pixel[2] = color.r;
        if (bytes == 4u)
        {
            pixel[3] = color.a;
        }
    }
}

tb_bitmap_result tb_bitmap_write_bmp(const tb_bitmap *bitmap, tb_bitmap_bmp_depth depth,
                                     tb_stream *sink, tb_bitmap_error *error)
{
    tb_bitmap_error ignored = {0};
    tb_bitmap_error *const found = error != NULL ? error : &ignored;
    uint8_t headers[BMP_FILE_HEADER + BMP_V5_HEADER] = {0};
    uint8_t *row = NULL;
    tb_bitmap_result result = TB_BITMAP_OK;

    TB_ASSERT(bitmap != NULL);
    TB_ASSERT(sink != NULL);
    TB_ASSERT_MSG(depth == TB_BITMAP_BMP_32 || depth == TB_BITMAP_BMP_24, "a BMP depth");
    *found = (tb_bitmap_error){.reason = ""};
    if (bitmap->width == 0 || bitmap->height == 0)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "a bitmap of no pixels, which no BMP file holds");
        goto done;
    }
    const uint64_t pixel_bits = depth == TB_BITMAP_BMP_32 ? 32u : 24u;
    const uint64_t stride = ((uint64_t)bitmap->width * pixel_bits + 31u) / 32u * 4u;
    const uint64_t header =
        BMP_FILE_HEADER + (depth == TB_BITMAP_BMP_32 ? BMP_V5_HEADER : BMP_INFO_HEADER);
    const uint64_t file_size = header + stride * bitmap->height;
    /* coverage-off: only a bitmap of more than 4 GiB is, which a test cannot afford */
    if (file_size > UINT32_MAX)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_TOO_LARGE, TB_BITMAP_NO_OFFSET,
                                "a bitmap too large for a BMP file");
        goto done;
    }
    /* coverage-on */
    bmp_write_headers(headers, bitmap, depth, (uint32_t)stride, (uint32_t)file_size);
    if (tb_stream_write_all(sink, headers, (size_t)header) != TB_STREAM_OK)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_OUTPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                "the stream refused the headers");
        goto done;
    }
    row = tb_calloc((size_t)stride, 1u);
    for (uint32_t written = 0; written < bitmap->height; written++)
    {
        bmp_encode_row(bitmap, bitmap->height - 1u - written, depth, row);
        if (tb_stream_write_all(sink, row, (size_t)stride) != TB_STREAM_OK)
        {
            result = tb_bitmap_fail(found, TB_BITMAP_OUTPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                    "the stream refused the pixels");
            goto done;
        }
    }
done:
    tb_free(row);
    tb_bitmap_describe(found, NULL);
    return result;
}
