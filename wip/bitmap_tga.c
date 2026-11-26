/* TGA files: Truevision's format, read uncompressed and run-length encoded,
 * in true color, gray and color-mapped, from either corner, and written as
 * 32 bits with alpha.
 *
 * A file is a header of 18 bytes, an image ID, a color map, and the pixels,
 * then, in version 2, an extension area and a footer that ends with a
 * signature. Every number is little-endian. Nothing at the start says that
 * a file is TGA, so the generic decoders recognize one by its footer, or by
 * a header whose every field is plausible, after every format that has a
 * signature (see tb_bitmap_is_tga()). Every size is checked against the
 * bytes there are: the file is untrusted. */
#include "bitmap_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <string.h>

/* The header's size, and its fields. */
#define TGA_HEADER 18u
#define TGA_ID_LENGTH_AT 0u
#define TGA_MAP_TYPE_AT 1u
#define TGA_IMAGE_TYPE_AT 2u
#define TGA_MAP_FIRST_AT 3u
#define TGA_MAP_LENGTH_AT 5u
#define TGA_MAP_BITS_AT 7u
#define TGA_WIDTH_AT 12u
#define TGA_HEIGHT_AT 14u
#define TGA_BITS_AT 16u
#define TGA_DESCRIPTOR_AT 17u

/* The descriptor: bits of alpha, and the corner the first pixel is in. */
#define TGA_ALPHA_BITS 0x0Fu
#define TGA_RIGHT_TO_LEFT 0x10u
#define TGA_TOP_TO_BOTTOM 0x20u

/* Image types. */
#define TGA_NONE 0u
#define TGA_MAPPED 1u
#define TGA_TRUE_COLOR 2u
#define TGA_GRAY 3u
#define TGA_RLE 8u /* Added to the three above. */

/* The footer of version 2: the offsets of the extension area and the
 * developer directory, and the signature. */
#define TGA_FOOTER 26u
#define TGA_SIGNATURE "TRUEVISION-XFILE.\0"
#define TGA_SIGNATURE_BYTES 18u
#define TGA_EXTENSION_BYTES 495u
#define TGA_ATTRIBUTES_AT 494u /* From the extension area's start. */

/* What the extension area says the alpha of 32-bit pixels is. */
#define TGA_ATTRIBUTES_NONE 0u
#define TGA_ATTRIBUTES_UNDEFINED_IGNORE 1u
#define TGA_ATTRIBUTES_UNDEFINED_KEEP 2u
#define TGA_ATTRIBUTES_ALPHA 3u
#define TGA_ATTRIBUTES_PREMULTIPLIED 4u

/* The most entries a color map has: what 16 bits address. */
#define TGA_MOST_COLORS 65536u

static uint32_t tga_u16(const uint8_t *bytes, size_t at)
{
    return (uint32_t)bytes[at] | (uint32_t)bytes[at + 1u] << 8;
}

static uint32_t tga_u32(const uint8_t *bytes, size_t at)
{
    return tga_u16(bytes, at) | tga_u16(bytes, at + 2u) << 16;
}

/* What the header says. */
typedef struct tga_layout
{
    uint32_t width;
    uint32_t height;
    uint32_t type;   /* TGA_MAPPED, TGA_TRUE_COLOR or TGA_GRAY. */
    bool compressed; /* Run-length encoded. */
    uint32_t bits;   /* Of a pixel. */
    size_t pixel_bytes;
    bool right_to_left;
    bool top_to_bottom;
    uint32_t alpha_bits;
    /* The color map: its first index, its length, and the bits of an entry. */
    uint32_t map_first;
    uint32_t map_length;
    uint32_t map_bits;
    size_t map_at;
    size_t pixels_at;
    /* 32-bit alpha: kept, dropped, or premultiplied. */
    bool keep_alpha;
    bool premultiplied;
} tga_layout;

typedef struct tga_reader
{
    const uint8_t *bytes;
    size_t length;
    tb_bitmap_error *error;
} tga_reader;

static tb_bitmap_result tga_malformed(const tga_reader *reader, size_t offset, const char *reason)
{
    return tb_bitmap_fail(reader->error, TB_BITMAP_MALFORMED, offset, reason);
}

/* Whether the file ends with a version 2 footer. */
static bool tga_has_footer(const uint8_t *bytes, size_t length)
{
    return length >= TGA_HEADER + TGA_FOOTER &&
           memcmp(bytes + length - TGA_SIGNATURE_BYTES, TGA_SIGNATURE, TGA_SIGNATURE_BYTES) == 0;
}

/* Whether a header's fields are those of a TGA file: a known image type, a
 * color map exactly when the type is color-mapped, and bits that the type has. */
static bool tga_header_plausible(const uint8_t *bytes)
{
    const uint32_t map_type = bytes[TGA_MAP_TYPE_AT];
    const uint32_t image_type = bytes[TGA_IMAGE_TYPE_AT];
    const uint32_t type = image_type & ~TGA_RLE;
    const uint32_t bits = bytes[TGA_BITS_AT];
    const uint32_t map_bits = bytes[TGA_MAP_BITS_AT];

    if (image_type > (TGA_GRAY | TGA_RLE) || type == TGA_NONE || type > TGA_GRAY ||
        (image_type & 0xF4u) != 0)
    {
        return false;
    }
    if (map_type > 1u || (type == TGA_MAPPED) != (map_type == 1u))
    {
        return false;
    }
    if (map_type == 1u && map_bits != 15u && map_bits != 16u && map_bits != 24u && map_bits != 32u)
    {
        return false;
    }
    if (tga_u16(bytes, TGA_WIDTH_AT) == 0 || tga_u16(bytes, TGA_HEIGHT_AT) == 0 ||
        (bytes[TGA_DESCRIPTOR_AT] & 0xC0u) != 0)
    {
        return false;
    }
    switch (type)
    {
    case TGA_MAPPED:
        return bits == 8u || bits == 16u;
    case TGA_TRUE_COLOR:
        return bits == 15u || bits == 16u || bits == 24u || bits == 32u;
    default:
        return bits == 8u || bits == 16u;
    }
}

bool tb_bitmap_is_tga(const uint8_t *bytes, size_t length)
{
    if (length < TGA_HEADER)
    {
        return false;
    }
    return tga_has_footer(bytes, length) || tga_header_plausible(bytes);
}

/* The alpha of 32-bit pixels, as the extension area's attributes type says:
 * without one, the descriptor's bits of alpha. */
static tb_bitmap_result tga_read_attributes(const tga_reader *reader, tga_layout *layout)
{
    layout->keep_alpha = layout->alpha_bits > 0;
    if (!tga_has_footer(reader->bytes, reader->length))
    {
        return TB_BITMAP_OK;
    }
    const size_t footer_at = reader->length - TGA_FOOTER;
    const size_t extension_at = tga_u32(reader->bytes, footer_at);
    if (extension_at == 0)
    {
        return TB_BITMAP_OK;
    }
    if (extension_at > footer_at || footer_at - extension_at < TGA_EXTENSION_BYTES)
    {
        return tga_malformed(reader, footer_at, "an extension area that ends past the footer");
    }
    if (tga_u16(reader->bytes, extension_at) != TGA_EXTENSION_BYTES)
    {
        return tga_malformed(reader, extension_at, "an extension area of the wrong size");
    }
    switch (reader->bytes[extension_at + TGA_ATTRIBUTES_AT])
    {
    case TGA_ATTRIBUTES_NONE:
    case TGA_ATTRIBUTES_UNDEFINED_IGNORE:
        layout->keep_alpha = false;
        break;
    case TGA_ATTRIBUTES_ALPHA:
        layout->keep_alpha = true;
        break;
    case TGA_ATTRIBUTES_PREMULTIPLIED:
        layout->keep_alpha = true;
        layout->premultiplied = true;
        break;
    default:
        /* Alpha that is undefined but should be kept, and values of no
         * meaning: the descriptor decides. */
        break;
    }
    return TB_BITMAP_OK;
}

static tb_bitmap_result tga_read_layout(const tga_reader *reader, uint32_t max_side,
                                        uint64_t max_pixels, tga_layout *layout)
{
    const uint8_t *const bytes = reader->bytes;

    if (reader->length < TGA_HEADER)
    {
        return tga_malformed(reader, reader->length, "the header ends early");
    }
    if (!tga_header_plausible(bytes))
    {
        return tga_malformed(reader, TGA_IMAGE_TYPE_AT,
                             "an image type, color map or bits that do not go together");
    }
    const uint32_t image_type = bytes[TGA_IMAGE_TYPE_AT];
    const uint32_t descriptor = bytes[TGA_DESCRIPTOR_AT];
    *layout = (tga_layout){.width = tga_u16(bytes, TGA_WIDTH_AT),
                           .height = tga_u16(bytes, TGA_HEIGHT_AT),
                           .type = image_type & ~TGA_RLE,
                           .compressed = (image_type & TGA_RLE) != 0,
                           .bits = bytes[TGA_BITS_AT],
                           .right_to_left = (descriptor & TGA_RIGHT_TO_LEFT) != 0,
                           .top_to_bottom = (descriptor & TGA_TOP_TO_BOTTOM) != 0,
                           .alpha_bits = descriptor & TGA_ALPHA_BITS,
                           .map_first = tga_u16(bytes, TGA_MAP_FIRST_AT),
                           .map_length = tga_u16(bytes, TGA_MAP_LENGTH_AT),
                           .map_bits = bytes[TGA_MAP_BITS_AT]};
    layout->pixel_bytes = (layout->bits + 7u) / 8u;
    if (layout->width > max_side || layout->height > max_side ||
        (uint64_t)layout->width * layout->height > max_pixels)
    {
        return tb_bitmap_fail(reader->error, TB_BITMAP_TOO_LARGE, TGA_WIDTH_AT,
                              "dimensions over the limits");
    }
    layout->map_at = TGA_HEADER + (size_t)bytes[TGA_ID_LENGTH_AT];
    const size_t map_bytes =
        bytes[TGA_MAP_TYPE_AT] == 1u ? (size_t)layout->map_length * ((layout->map_bits + 7u) / 8u)
                                     : 0u;
    layout->pixels_at = layout->map_at + map_bytes;
    if (layout->pixels_at > reader->length)
    {
        return tga_malformed(reader, layout->map_at > reader->length ? TGA_ID_LENGTH_AT
                                                                      : TGA_MAP_LENGTH_AT,
                             "the image ID or color map ends past the end of the file");
    }
    if (layout->type == TGA_MAPPED && layout->map_length == 0)
    {
        return tga_malformed(reader, TGA_MAP_LENGTH_AT, "a color map of no entries");
    }
    return tga_read_attributes(reader, layout);
}

/* A color of 15 or 16 bits, 5-5-5 from the high bits, with an attribute bit
 * on top that is alpha when the file says there is one. */
static tb_bitmap_color tga_color_16(uint32_t value, bool alpha)
{
    const uint32_t red = value >> 10 & 0x1Fu;
    const uint32_t green = value >> 5 & 0x1Fu;
    const uint32_t blue = value & 0x1Fu;
    const uint8_t opacity = (alpha && (value & 0x8000u) == 0) ? 0u : UINT8_MAX;

    return (tb_bitmap_color){(uint8_t)((red * 255u + 15u) / 31u), (uint8_t)((green * 255u + 15u) / 31u),
                             (uint8_t)((blue * 255u + 15u) / 31u), opacity};
}

/* A color of `bits` bits, blue first, as both pixels and color map entries
 * store it. */
static tb_bitmap_color tga_color(const uint8_t *bytes, uint32_t bits, bool alpha)
{
    switch (bits)
    {
    case 15:
    case 16:
        return tga_color_16(tga_u16(bytes, 0), bits == 16u && alpha);
    case 24:
        return (tb_bitmap_color){bytes[2], bytes[1], bytes[0], UINT8_MAX};
    default:
        return (tb_bitmap_color){bytes[2], bytes[1], bytes[0], alpha ? bytes[3] : UINT8_MAX};
    }
}

typedef struct tga_decoder
{
    const tga_reader *reader;
    const tga_layout *layout;
    tb_bitmap *bitmap;
    size_t pixel; /* Pixels put so far, in the file's order. */
} tga_decoder;

/* The color of a pixel's bytes. */
static tb_bitmap_result tga_pixel_color(const tga_decoder *decoder, const uint8_t *bytes,
                                        size_t at, tb_bitmap_color *color)
{
    const tga_layout *const layout = decoder->layout;

    switch (layout->type)
    {
    case TGA_MAPPED: {
        const uint32_t index = layout->bits == 8u ? bytes[0] : tga_u16(bytes, 0);
        if (index < layout->map_first || index - layout->map_first >= layout->map_length)
        {
            return tga_malformed(decoder->reader, at, "a pixel past the color map");
        }
        const size_t entry_bytes = (layout->map_bits + 7u) / 8u;
        const uint8_t *const entry = decoder->reader->bytes + layout->map_at +
                                     (size_t)(index - layout->map_first) * entry_bytes;
        *color = tga_color(entry, layout->map_bits, layout->keep_alpha || layout->map_bits == 32u);
        return TB_BITMAP_OK;
    }
    case TGA_GRAY: {
        const uint8_t alpha = layout->bits == 16u ? bytes[1] : UINT8_MAX;
        *color = (tb_bitmap_color){bytes[0], bytes[0], bytes[0], alpha};
        return TB_BITMAP_OK;
    }
    default:
        *color = tga_color(bytes, layout->bits, layout->keep_alpha);
        return TB_BITMAP_OK;
    }
}

/* Put the next pixel in the file's order where its corner says it goes. */
static tb_bitmap_result tga_put(tga_decoder *decoder, const uint8_t *bytes, size_t at)
{
    const tga_layout *const layout = decoder->layout;
    tb_bitmap_color color = {0};
    const tb_bitmap_result result = tga_pixel_color(decoder, bytes, at, &color);

    if (result != TB_BITMAP_OK)
    {
        return result;
    }
    const uint32_t column = (uint32_t)(decoder->pixel % layout->width);
    const uint32_t row = (uint32_t)(decoder->pixel / layout->width);
    const uint32_t x = layout->right_to_left ? layout->width - 1u - column : column;
    const uint32_t y = layout->top_to_bottom ? row : layout->height - 1u - row;
    uint8_t *const out = tb_bitmap_row(decoder->bitmap, y) + (size_t)x * 4u;
    out[0] = color.r;
    out[1] = color.g;
    out[2] = color.b;
    out[3] = color.a;
    decoder->pixel++;
    return TB_BITMAP_OK;
}

static tb_bitmap_result tga_decode_raw(tga_decoder *decoder, size_t pixels)
{
    const tga_reader *const reader = decoder->reader;
    const size_t bytes = decoder->layout->pixel_bytes;
    size_t at = decoder->layout->pixels_at;

    if ((reader->length - at) / bytes < pixels)
    {
        return tga_malformed(reader, reader->length, "the pixels end early");
    }
    for (size_t index = 0; index < pixels; index++)
    {
        const tb_bitmap_result result = tga_put(decoder, reader->bytes + at, at);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
        at += bytes;
    }
    return TB_BITMAP_OK;
}

/* Run-length encoding: packets of a count byte, its high bit set for a run
 * of one pixel repeated and clear for that many pixels as they are. A packet
 * may run from one row to the next, but not past the last. */
static tb_bitmap_result tga_decode_rle(tga_decoder *decoder, size_t pixels)
{
    const tga_reader *const reader = decoder->reader;
    const size_t bytes = decoder->layout->pixel_bytes;
    size_t at = decoder->layout->pixels_at;

    while (decoder->pixel < pixels)
    {
        if (at >= reader->length)
        {
            return tga_malformed(reader, reader->length, "the compressed pixels end early");
        }
        const size_t packet = at;
        const uint32_t header = reader->bytes[at];
        const bool run = (header & 0x80u) != 0;
        const size_t count = (header & 0x7Fu) + 1u;
        at++;
        if (count > pixels - decoder->pixel)
        {
            return tga_malformed(reader, packet, "a packet past the end of the image");
        }
        const size_t needed = run ? bytes : bytes * count;
        if (needed > reader->length - at)
        {
            return tga_malformed(reader, reader->length, "the compressed pixels end early");
        }
        for (size_t index = 0; index < count; index++)
        {
            const size_t from = run ? at : at + index * bytes;
            const tb_bitmap_result result = tga_put(decoder, reader->bytes + from, from);
            if (result != TB_BITMAP_OK)
            {
                return result;
            }
        }
        at += needed;
    }
    return TB_BITMAP_OK;
}

tb_bitmap_result tb_bitmap_decode_tga(const uint8_t *bytes, size_t length, uint32_t max_side,
                                      uint64_t max_pixels, const tb_allocator *allocator,
                                      tb_bitmap **bitmap, tb_bitmap_error *error)
{
    const tga_reader reader = {bytes, length, error};
    tga_layout layout = {0};
    tb_bitmap_result result = tga_read_layout(&reader, max_side, max_pixels, &layout);

    if (result != TB_BITMAP_OK)
    {
        return result;
    }
    const size_t pixels = (size_t)layout.width * layout.height;
    /* A raw file's pixels are all there before they are allocated; a
     * compressed one's packets of 128 pixels need at least two bytes. */
    if (!layout.compressed && (length - layout.pixels_at) / layout.pixel_bytes < pixels)
    {
        return tga_malformed(&reader, length, "the pixels end early");
    }
    if (layout.compressed && (length - layout.pixels_at) / (1u + layout.pixel_bytes) < pixels / 128u)
    {
        return tga_malformed(&reader, length, "the compressed pixels end early");
    }
    tga_decoder decoder = {.reader = &reader, .layout = &layout};
    decoder.bitmap = tb_bitmap_try_create(layout.width, layout.height, TB_BITMAP_RGBA8, allocator);
    if (decoder.bitmap == NULL)
    {
        return tb_bitmap_fail(error, TB_BITMAP_NO_MEMORY, TB_BITMAP_NO_OFFSET,
                              "no memory for the pixels");
    }
    result = layout.compressed ? tga_decode_rle(&decoder, pixels) : tga_decode_raw(&decoder, pixels);
    if (result != TB_BITMAP_OK)
    {
        tb_bitmap_destroy(decoder.bitmap);
        return result;
    }
    if (layout.premultiplied)
    {
        tb_bitmap_set_premultiplied(decoder.bitmap, true);
        tb_bitmap_unpremultiply(decoder.bitmap);
    }
    *bitmap = decoder.bitmap;
    return TB_BITMAP_OK;
}

/* ---------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

static void tga_put_u16(uint8_t *bytes, size_t at, uint32_t value)
{
    bytes[at] = (uint8_t)value;
    bytes[at + 1u] = (uint8_t)(value >> 8);
}

static void tga_put_u32(uint8_t *bytes, size_t at, uint32_t value)
{
    tga_put_u16(bytes, at, value & 0xFFFFu);
    tga_put_u16(bytes, at + 2u, value >> 16);
}

/* A row of the file: BGRA, straight. */
static void tga_encode_row(const tb_bitmap *bitmap, uint32_t y, uint8_t *row)
{
    for (uint32_t x = 0; x < bitmap->width; x++)
    {
        tb_bitmap_color color = tb_bitmap_get_pixel(bitmap, x, y);
        if (bitmap->premultiplied)
        {
            color = tb_bitmap_unpremultiplied_color(color);
        }
        uint8_t *const pixel = row + (size_t)x * 4u;
        pixel[0] = color.b;
        pixel[1] = color.g;
        pixel[2] = color.r;
        pixel[3] = color.a;
    }
}

/* Run-length encode a row: runs of two or more equal pixels as run packets,
 * the rest as raw packets, none longer than 128 pixels or past the row.
 * `out` has room for a byte more each pixel; returns the bytes written. */
static size_t tga_compress_row(const uint8_t *row, uint32_t width, uint8_t *out)
{
    size_t written = 0;
    uint32_t x = 0;

    while (x < width)
    {
        uint32_t same = 1;
        while (x + same < width && same < 128u &&
               memcmp(row + (size_t)(x + same) * 4u, row + (size_t)x * 4u, 4) == 0)
        {
            same++;
        }
        if (same >= 2u)
        {
            out[written++] = (uint8_t)(0x80u | (same - 1u));
            memcpy(out + written, row + (size_t)x * 4u, 4);
            written += 4u;
            x += same;
            continue;
        }
        /* A raw packet runs until two equal pixels begin a run. */
        uint32_t count = 1;
        while (x + count < width && count < 128u &&
               !(x + count + 1u < width &&
                 memcmp(row + (size_t)(x + count) * 4u, row + (size_t)(x + count + 1u) * 4u, 4) ==
                     0))
        {
            count++;
        }
        out[written++] = (uint8_t)(count - 1u);
        memcpy(out + written, row + (size_t)x * 4u, (size_t)count * 4u);
        written += (size_t)count * 4u;
        x += count;
    }
    return written;
}

tb_bitmap_result tb_bitmap_write_tga(const tb_bitmap *bitmap, bool compress, tb_stream *sink,
                                     tb_bitmap_error *error)
{
    tb_bitmap_error ignored = {0};
    tb_bitmap_error *const found = error != NULL ? error : &ignored;
    uint8_t header[TGA_HEADER] = {0};
    uint8_t footer[TGA_FOOTER] = {0};
    uint8_t *row = NULL;
    uint8_t *packed = NULL;
    tb_bitmap_result result = TB_BITMAP_OK;

    TB_ASSERT(bitmap != NULL);
    TB_ASSERT(sink != NULL);
    *found = (tb_bitmap_error){.reason = ""};
    if (bitmap->width == 0 || bitmap->height == 0 || bitmap->width > UINT16_MAX ||
        bitmap->height > UINT16_MAX)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "a bitmap of no pixels, or of more than 65535 a side");
        goto done;
    }
    header[TGA_IMAGE_TYPE_AT] = (uint8_t)(TGA_TRUE_COLOR | (compress ? TGA_RLE : 0u));
    tga_put_u16(header, TGA_WIDTH_AT, bitmap->width);
    tga_put_u16(header, TGA_HEIGHT_AT, bitmap->height);
    header[TGA_BITS_AT] = 32u;
    header[TGA_DESCRIPTOR_AT] = (uint8_t)(8u | TGA_TOP_TO_BOTTOM);
    if (tb_stream_write_all(sink, header, sizeof(header)) != TB_STREAM_OK)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_OUTPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                "the stream refused the file");
        goto done;
    }
    const size_t row_bytes = (size_t)bitmap->width * 4u;
    row = tb_malloc(row_bytes);
    packed = tb_malloc(row_bytes + bitmap->width);
    for (uint32_t y = 0; y < bitmap->height; y++)
    {
        tga_encode_row(bitmap, y, row);
        const uint8_t *const out = compress ? packed : row;
        const size_t length = compress ? tga_compress_row(row, bitmap->width, packed) : row_bytes;
        if (tb_stream_write_all(sink, out, length) != TB_STREAM_OK)
        {
            result = tb_bitmap_fail(found, TB_BITMAP_OUTPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                    "the stream refused the file");
            goto done;
        }
    }
    /* The footer of version 2, with no extension area: the descriptor's 8
     * bits of alpha say that alpha is alpha. */
    tga_put_u32(footer, 0, 0);
    tga_put_u32(footer, 4, 0);
    memcpy(footer + 8, TGA_SIGNATURE, TGA_SIGNATURE_BYTES);
    if (tb_stream_write_all(sink, footer, sizeof(footer)) != TB_STREAM_OK)
    {
        result = tb_bitmap_fail(found, TB_BITMAP_OUTPUT_FAILED, TB_BITMAP_NO_OFFSET,
                                "the stream refused the file");
    }
done:
    tb_free(packed);
    tb_free(row);
    tb_bitmap_describe(found, NULL);
    return result;
}
