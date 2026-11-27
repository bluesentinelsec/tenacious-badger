/* PNG files, read: every color type and bit depth, the five filters,
 * Adam7 interlacing, and transparency.
 *
 * A file is a signature and chunks, each a big-endian length, four letters
 * of name, the data and a CRC-32 of the name and data. The image header
 * (IHDR) comes first, the palette (PLTE) and transparency (tRNS) before the
 * image data, the image data (IDAT) in chunks one after another, and the
 * end (IEND) last. The image data is one zlib stream of filtered rows, which
 * is decompressed a row at a time into two rows of memory, the row and the
 * one above, unfiltered and put in the bitmap as RGBA8: no more memory than
 * the bitmap's, whatever the file.
 *
 * The file is untrusted. Every length is checked against the bytes there
 * are, every CRC is checked, and the dimensions are checked against the
 * limits, and against how much the image data could possibly hold, before
 * any pixel is allocated. */
#include "bitmap_png.h"
#include "bitmap_internal.h"

#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"

#include <string.h>

const uint8_t tb_bitmap_png_signature[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};

/* The most PLTE colors: what 8 bits address. */
#define PNG_MOST_COLORS 256u

/* The most bytes DEFLATE can make of one: a length of 258 and a distance,
 * each a code of one bit, make 258 bytes of two bits. A stream that would
 * make an image of more bytes than this many times its own is damaged. */
#define PNG_MOST_INFLATION 1032u

/* The largest width or height PNG allows. */
#define PNG_LARGEST_SIDE 0x7FFFFFFFu

/* Offsets of the header's fields from the start of the file: the first
 * chunk's data begins after the signature and its length and name. */
#define PNG_IHDR_AT (PNG_SIGNATURE_BYTES + PNG_CHUNK_HEAD)
#define PNG_WIDTH_AT PNG_IHDR_AT
#define PNG_DEPTH_AT (PNG_IHDR_AT + 8u)
#define PNG_COLOR_AT (PNG_IHDR_AT + 9u)
#define PNG_COMPRESSION_AT (PNG_IHDR_AT + 10u)
#define PNG_FILTER_METHOD_AT (PNG_IHDR_AT + 11u)
#define PNG_INTERLACE_AT (PNG_IHDR_AT + 12u)

/* Adam7: the seven passes of an interlaced image, each a grid of pixels
 * from its first column and row, every so many columns and rows. */
#define PNG_PASSES 7u
static const uint8_t png_pass_column[PNG_PASSES] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t png_pass_row[PNG_PASSES] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t png_pass_across[PNG_PASSES] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t png_pass_down[PNG_PASSES] = {8, 8, 8, 4, 4, 2, 2};

/* One chunk: where it begins, its name, and its data. */
typedef struct png_chunk
{
    size_t at; /* Its length's first byte. */
    char name[5];
    const uint8_t *data;
    size_t data_at;
    uint32_t length;
} png_chunk;

/* What the image header says. */
typedef struct png_header
{
    uint32_t width;
    uint32_t height;
    uint32_t depth;   /* Bits of a sample: 1, 2, 4, 8 or 16. */
    uint32_t color;   /* The color type. */
    uint32_t samples; /* Samples of a pixel: 1 to 4. */
    bool interlaced;
    size_t filter_bytes; /* Bytes of a whole pixel, at least 1: what filters look back. */
} png_header;

/* One pass of the image, or the whole of one that is not interlaced. */
typedef struct png_pass
{
    uint32_t index;
    uint32_t width;   /* Pixels of a row of the pass. */
    uint32_t height;  /* Rows of the pass. */
    uint32_t row;     /* The row being decompressed. */
    size_t row_bytes; /* Bytes of a row, after its filter type. */
} png_pass;

typedef struct png_decoder
{
    const uint8_t *bytes;
    size_t length;
    tb_bitmap_error *error;
    const tb_allocator *allocator;
    png_header header;
    tb_bitmap_color palette[PNG_MOST_COLORS]; /* With the alpha of tRNS. */
    size_t colors;
    bool keyed;      /* tRNS names a color of gray or RGB that is transparent. */
    uint16_t key[3]; /* That color's samples: gray, or red, green and blue. */
    bool seen_palette;
    bool seen_transparency;
    bool seen_data;
    bool data_ended; /* A chunk other than IDAT came after the image data. */
    /* Decompression. */
    tb_inflater *inflater;
    uint8_t *rows;    /* Two rows, each a filter type and its bytes. */
    size_t rows_size; /* Bytes allocated for both. */
    uint8_t *above;   /* The row above, unfiltered; zero above a pass's first. */
    uint8_t *current; /* The row being decompressed. */
    size_t filled;    /* Bytes of the current row, its filter type included. */
    png_pass pass;
    bool rows_done;
    tb_bitmap *bitmap;
} png_decoder;

static tb_bitmap_result png_malformed_in(const png_decoder *decoder, const png_chunk *chunk,
                                         size_t offset, const char *reason)
{
    return tb_bitmap_fail_in(decoder->error, TB_BITMAP_MALFORMED, chunk->name, offset, reason);
}

static bool png_named(const png_chunk *chunk, const char *name)
{
    return memcmp(chunk->name, name, 4) == 0;
}

static bool png_is_letter(uint8_t byte)
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

/* ---------------------------------------------------------------------------
 * Chunks.
 * ------------------------------------------------------------------------- */

/* The chunk at `at`: its length and name, and that its data and CRC are
 * there. The CRC itself is checked by png_check_crc(). */
static tb_bitmap_result png_chunk_at(const png_decoder *decoder, size_t at, png_chunk *chunk)
{
    *chunk = (png_chunk){.at = at};
    if (decoder->length - at < PNG_CHUNK_HEAD)
    {
        return tb_bitmap_fail(decoder->error, TB_BITMAP_MALFORMED, decoder->length,
                              "the file ends before its IEND chunk");
    }
    const uint8_t *const head = decoder->bytes + at;
    for (size_t letter = 0; letter < 4u; letter++)
    {
        if (!png_is_letter(head[4u + letter]))
        {
            return tb_bitmap_fail(decoder->error, TB_BITMAP_MALFORMED, at + 4u + letter,
                                  "a chunk whose name is not four letters");
        }
        chunk->name[letter] = (char)head[4u + letter];
    }
    chunk->length = png_u32(head);
    if (chunk->length > PNG_LONGEST_CHUNK)
    {
        return png_malformed_in(decoder, chunk, at, "a length of 2^31 bytes or more");
    }
    const size_t left = decoder->length - at - PNG_CHUNK_HEAD;
    if (chunk->length > left || left - chunk->length < PNG_CHUNK_CRC)
    {
        return png_malformed_in(decoder, chunk, at, "the chunk runs past the end of the file");
    }
    chunk->data_at = at + PNG_CHUNK_HEAD;
    chunk->data = decoder->bytes + chunk->data_at;
    return TB_BITMAP_OK;
}

static tb_bitmap_result png_check_crc(const png_decoder *decoder, const png_chunk *chunk)
{
    const size_t crc_at = chunk->data_at + chunk->length;
    const uint32_t computed = tb_crc32(decoder->bytes + chunk->at + 4u, 4u + (size_t)chunk->length);

    if (computed != png_u32(decoder->bytes + crc_at))
    {
        return png_malformed_in(decoder, chunk, crc_at, "a CRC that does not match the chunk");
    }
    return TB_BITMAP_OK;
}

static size_t png_after(const png_chunk *chunk)
{
    return chunk->data_at + chunk->length + PNG_CHUNK_CRC;
}

/* ---------------------------------------------------------------------------
 * The header, the palette and transparency.
 * ------------------------------------------------------------------------- */

/* Whether a color type has a bit depth, and how many samples a pixel of it has. */
static bool png_depth_allowed(uint32_t color, uint32_t depth, uint32_t *samples)
{
    const bool byte_depths = depth == 8u || depth == 16u;

    switch (color)
    {
    case PNG_GRAY:
        *samples = 1;
        return depth == 1u || depth == 2u || depth == 4u || byte_depths;
    case PNG_RGB:
        *samples = 3;
        return byte_depths;
    case PNG_PALETTE:
        *samples = 1;
        return depth == 1u || depth == 2u || depth == 4u || depth == 8u;
    case PNG_GRAY_ALPHA:
        *samples = 2;
        return byte_depths;
    case PNG_RGBA:
        *samples = 4;
        return byte_depths;
    default:
        *samples = 0;
        return false;
    }
}

static tb_bitmap_result png_read_header(png_decoder *decoder, const png_chunk *chunk,
                                        uint32_t max_side, uint64_t max_pixels)
{
    png_header *const header = &decoder->header;
    const uint8_t *const data = chunk->data;

    if (!png_named(chunk, "IHDR"))
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a first chunk that is not IHDR");
    }
    if (chunk->length != PNG_IHDR_BYTES)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "an IHDR chunk not 13 bytes long");
    }
    header->width = png_u32(data);
    header->height = png_u32(data + 4);
    header->depth = data[8];
    header->color = data[9];
    if (header->width == 0 || header->width > PNG_LARGEST_SIDE)
    {
        return png_malformed_in(decoder, chunk, PNG_WIDTH_AT, "a width of 0, or of 2^31 or more");
    }
    if (header->height == 0 || header->height > PNG_LARGEST_SIDE)
    {
        return png_malformed_in(decoder, chunk, PNG_WIDTH_AT + 4u,
                                "a height of 0, or of 2^31 or more");
    }
    if (!png_depth_allowed(header->color, header->depth, &header->samples))
    {
        if (header->samples == 0)
        {
            return png_malformed_in(decoder, chunk, PNG_COLOR_AT, "an unknown color type");
        }
        return png_malformed_in(decoder, chunk, PNG_DEPTH_AT,
                                "a bit depth the color type does not have");
    }
    if (data[10] != 0)
    {
        return png_malformed_in(decoder, chunk, PNG_COMPRESSION_AT,
                                "a compression method other than 0");
    }
    if (data[11] != 0)
    {
        return png_malformed_in(decoder, chunk, PNG_FILTER_METHOD_AT,
                                "a filter method other than 0");
    }
    if (data[12] > 1u)
    {
        return png_malformed_in(decoder, chunk, PNG_INTERLACE_AT, "an unknown interlace method");
    }
    header->interlaced = data[12] == 1u;
    const size_t pixel_bits = (size_t)header->samples * header->depth;
    header->filter_bytes = pixel_bits < 8u ? 1u : pixel_bits / 8u;
    if (header->width > max_side || header->height > max_side ||
        (uint64_t)header->width * header->height > max_pixels)
    {
        return tb_bitmap_fail_in(decoder->error, TB_BITMAP_TOO_LARGE, chunk->name, PNG_WIDTH_AT,
                                 "dimensions over the limits");
    }
    return TB_BITMAP_OK;
}

static tb_bitmap_result png_read_palette(png_decoder *decoder, const png_chunk *chunk)
{
    const uint32_t color = decoder->header.color;

    if (decoder->seen_data)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a PLTE chunk after the image data");
    }
    if (decoder->seen_transparency)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a PLTE chunk after the tRNS chunk");
    }
    if (decoder->seen_palette)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a second PLTE chunk");
    }
    if (color == PNG_GRAY || color == PNG_GRAY_ALPHA)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a PLTE chunk in a gray image");
    }
    const size_t colors = chunk->length / 3u;
    if (chunk->length % 3u != 0 || colors == 0 || colors > PNG_MOST_COLORS)
    {
        return png_malformed_in(decoder, chunk, chunk->at,
                                "a palette not of 1 to 256 colors of 3 bytes");
    }
    decoder->seen_palette = true;
    /* The palette of an image of color is only a suggestion, for displays
     * of few colors. */
    if (color != PNG_PALETTE)
    {
        return TB_BITMAP_OK;
    }
    if (colors > ((size_t)1 << decoder->header.depth))
    {
        return png_malformed_in(decoder, chunk, chunk->at,
                                "more colors than the bit depth addresses");
    }
    for (size_t index = 0; index < colors; index++)
    {
        const uint8_t *const entry = chunk->data + index * 3u;
        decoder->palette[index] = (tb_bitmap_color){entry[0], entry[1], entry[2], UINT8_MAX};
    }
    decoder->colors = colors;
    return TB_BITMAP_OK;
}

static tb_bitmap_result png_read_transparency(png_decoder *decoder, const png_chunk *chunk)
{
    const uint32_t color = decoder->header.color;

    if (decoder->seen_data)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a tRNS chunk after the image data");
    }
    if (decoder->seen_transparency)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a second tRNS chunk");
    }
    decoder->seen_transparency = true;
    switch (color)
    {
    case PNG_PALETTE:
        if (!decoder->seen_palette)
        {
            return png_malformed_in(decoder, chunk, chunk->at, "a tRNS chunk before PLTE");
        }
        if (chunk->length > decoder->colors)
        {
            return png_malformed_in(decoder, chunk, chunk->at,
                                    "more alphas than the palette has colors");
        }
        for (size_t index = 0; index < chunk->length; index++)
        {
            decoder->palette[index].a = chunk->data[index];
        }
        return TB_BITMAP_OK;
    case PNG_GRAY:
    case PNG_RGB: {
        const size_t samples = color == PNG_GRAY ? 1u : 3u;
        if (chunk->length != samples * 2u)
        {
            return png_malformed_in(decoder, chunk, chunk->at,
                                    "a tRNS chunk of the wrong length for its color type");
        }
        for (size_t sample = 0; sample < samples; sample++)
        {
            decoder->key[sample] =
                (uint16_t)(chunk->data[sample * 2u] << 8 | chunk->data[sample * 2u + 1u]);
        }
        decoder->keyed = true;
        return TB_BITMAP_OK;
    }
    default:
        /* An image with alpha needs no tRNS; PNG forbids one, and other
         * readers ignore it, as this one does. */
        return TB_BITMAP_OK;
    }
}

/* ---------------------------------------------------------------------------
 * Rows.
 * ------------------------------------------------------------------------- */

/* The pixels across or down of a pass: those of `size` from `first`, every `step`. */
static uint32_t png_pass_extent(uint32_t size, uint32_t first, uint32_t step)
{
    return size > first ? (size - first + step - 1u) / step : 0u;
}

static size_t png_row_bytes(const png_header *header, uint32_t width)
{
    return ((size_t)width * header->samples * header->depth + 7u) / 8u;
}

/* Set up the next pass that has pixels, from `index`; false when there is none. */
static bool png_begin_pass(png_decoder *decoder, uint32_t index)
{
    const png_header *const header = &decoder->header;

    for (; index < (header->interlaced ? PNG_PASSES : 1u); index++)
    {
        png_pass pass = {.index = index, .width = header->width, .height = header->height};
        if (header->interlaced)
        {
            pass.width =
                png_pass_extent(header->width, png_pass_column[index], png_pass_across[index]);
            pass.height =
                png_pass_extent(header->height, png_pass_row[index], png_pass_down[index]);
        }
        if (pass.width == 0 || pass.height == 0)
        {
            continue;
        }
        pass.row_bytes = png_row_bytes(header, pass.width);
        decoder->pass = pass;
        memset(decoder->above, 0, pass.row_bytes + 1u);
        return true;
    }
    return false;
}

/* Undo a row's filter, given the row above, unfiltered. */
static void png_unfilter(uint8_t *row, const uint8_t *above, size_t length, size_t back,
                         uint32_t filter)
{
    switch (filter)
    {
    case PNG_FILTER_SUB:
        for (size_t index = back; index < length; index++)
        {
            row[index] = (uint8_t)(row[index] + row[index - back]);
        }
        break;
    case PNG_FILTER_UP:
        for (size_t index = 0; index < length; index++)
        {
            row[index] = (uint8_t)(row[index] + above[index]);
        }
        break;
    case PNG_FILTER_AVERAGE:
        for (size_t index = 0; index < back && index < length; index++)
        {
            row[index] = (uint8_t)(row[index] + (above[index] >> 1));
        }
        for (size_t index = back; index < length; index++)
        {
            const uint32_t sum = (uint32_t)row[index - back] + above[index];
            row[index] = (uint8_t)(row[index] + (sum >> 1));
        }
        break;
    case PNG_FILTER_PAETH:
        for (size_t index = 0; index < back && index < length; index++)
        {
            row[index] = (uint8_t)(row[index] + above[index]);
        }
        for (size_t index = back; index < length; index++)
        {
            const uint8_t predicted =
                png_paeth(row[index - back], above[index], above[index - back]);
            row[index] = (uint8_t)(row[index] + predicted);
        }
        break;
    default:
        break;
    }
}

/* A sample of 16 bits as 8, rounded to nearest. */
static uint8_t png_narrow(uint32_t sample)
{
    return (uint8_t)((sample * UINT8_MAX + 32767u) / 65535u);
}

/* The `index`th sample of a row: of fewer than 8 bits, packed from the
 * high bits of each byte; of 16, big-endian. */
static uint32_t png_sample(const uint8_t *row, size_t index, uint32_t depth)
{
    switch (depth)
    {
    case 8:
        return row[index];
    case 16:
        return (uint32_t)row[index * 2u] << 8 | row[index * 2u + 1u];
    default: {
        const size_t bit = index * depth;
        const uint32_t shift = 8u - depth - (uint32_t)(bit % 8u);
        return (uint32_t)(row[bit / 8u] >> shift) & ((1u << depth) - 1u);
    }
    }
}

/* A sample at full scale in 8 bits: of fewer bits, each value spread evenly
 * from 0 to 255. */
static uint8_t png_widen(uint32_t sample, uint32_t depth)
{
    switch (depth)
    {
    case 1:
        return (uint8_t)(sample * 255u);
    case 2:
        return (uint8_t)(sample * 85u);
    case 4:
        return (uint8_t)(sample * 17u);
    case 8:
        return (uint8_t)sample;
    default:
        return png_narrow(sample);
    }
}

/* Where the `index`th pixel of the pass's current row goes, and the bytes
 * from one to the next. */
static uint8_t *png_destination(const png_decoder *decoder, size_t *step)
{
    const png_pass *const pass = &decoder->pass;
    uint32_t x = 0;
    uint32_t y = pass->row;

    *step = 4u;
    if (decoder->header.interlaced)
    {
        x = png_pass_column[pass->index];
        y = png_pass_row[pass->index] + pass->row * png_pass_down[pass->index];
        *step = 4u * png_pass_across[pass->index];
    }
    return tb_bitmap_row(decoder->bitmap, y) + (size_t)x * 4u;
}

static void png_put_gray(const png_decoder *decoder, const uint8_t *row, uint8_t *first,
                         size_t step)
{
    const uint32_t depth = decoder->header.depth;

    for (uint32_t x = 0; x < decoder->pass.width; x++)
    {
        uint8_t *const out = first + (size_t)x * step;
        const uint32_t sample = png_sample(row, x, depth);
        const uint8_t gray = png_widen(sample, depth);
        const bool clear = decoder->keyed && sample == decoder->key[0];
        out[0] = gray;
        out[1] = gray;
        out[2] = gray;
        out[3] = clear ? 0u : UINT8_MAX;
    }
}

static void png_put_gray_alpha(const png_decoder *decoder, const uint8_t *row, uint8_t *first,
                               size_t step)
{
    const uint32_t depth = decoder->header.depth;

    for (uint32_t x = 0; x < decoder->pass.width; x++)
    {
        uint8_t *const out = first + (size_t)x * step;
        const uint8_t gray = png_widen(png_sample(row, (size_t)x * 2u, depth), depth);
        out[0] = gray;
        out[1] = gray;
        out[2] = gray;
        out[3] = png_widen(png_sample(row, (size_t)x * 2u + 1u, depth), depth);
    }
}

static void png_put_rgb(const png_decoder *decoder, const uint8_t *row, uint8_t *first, size_t step)
{
    const uint32_t depth = decoder->header.depth;

    for (uint32_t x = 0; x < decoder->pass.width; x++)
    {
        uint8_t *const out = first + (size_t)x * step;
        const uint32_t red = png_sample(row, (size_t)x * 3u, depth);
        const uint32_t green = png_sample(row, (size_t)x * 3u + 1u, depth);
        const uint32_t blue = png_sample(row, (size_t)x * 3u + 2u, depth);
        const bool clear = decoder->keyed && red == decoder->key[0] && green == decoder->key[1] &&
                           blue == decoder->key[2];
        out[0] = png_widen(red, depth);
        out[1] = png_widen(green, depth);
        out[2] = png_widen(blue, depth);
        out[3] = clear ? 0u : UINT8_MAX;
    }
}

static void png_put_rgba(const png_decoder *decoder, const uint8_t *row, uint8_t *first,
                         size_t step)
{
    const uint32_t width = decoder->pass.width;

    if (decoder->header.depth == 8u && step == 4u)
    {
        memcpy(first, row, (size_t)width * 4u);
        return;
    }
    for (uint32_t x = 0; x < width; x++)
    {
        uint8_t *const out = first + (size_t)x * step;
        for (size_t channel = 0; channel < 4u; channel++)
        {
            out[channel] =
                png_widen(png_sample(row, (size_t)x * 4u + channel, decoder->header.depth),
                          decoder->header.depth);
        }
    }
}

static tb_bitmap_result png_put_palette(const png_decoder *decoder, const png_chunk *chunk,
                                        const uint8_t *row, uint8_t *first, size_t step)
{
    const uint32_t depth = decoder->header.depth;

    for (uint32_t x = 0; x < decoder->pass.width; x++)
    {
        uint8_t *const out = first + (size_t)x * step;
        const uint32_t index = png_sample(row, x, depth);
        if (index >= decoder->colors)
        {
            return png_malformed_in(decoder, chunk, chunk->at, "a pixel past the palette");
        }
        const tb_bitmap_color color = decoder->palette[index];
        out[0] = color.r;
        out[1] = color.g;
        out[2] = color.b;
        out[3] = color.a;
    }
    return TB_BITMAP_OK;
}

/* Unfilter the current row, put its pixels in the bitmap, and move to the
 * next row, of this pass or the next. */
static tb_bitmap_result png_finish_row(png_decoder *decoder, const png_chunk *chunk)
{
    const uint32_t filter = decoder->current[0];
    uint8_t *const row = decoder->current + 1;
    size_t step = 0;

    if (filter >= PNG_FILTER_COUNT)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a row of an unknown filter type");
    }
    png_unfilter(row, decoder->above + 1, decoder->pass.row_bytes, decoder->header.filter_bytes,
                 filter);
    uint8_t *const out = png_destination(decoder, &step);
    switch (decoder->header.color)
    {
    case PNG_GRAY:
        png_put_gray(decoder, row, out, step);
        break;
    case PNG_GRAY_ALPHA:
        png_put_gray_alpha(decoder, row, out, step);
        break;
    case PNG_RGB:
        png_put_rgb(decoder, row, out, step);
        break;
    case PNG_RGBA:
        png_put_rgba(decoder, row, out, step);
        break;
    default: {
        const tb_bitmap_result result = png_put_palette(decoder, chunk, row, out, step);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
        break;
    }
    }
    uint8_t *const swap = decoder->above;
    decoder->above = decoder->current;
    decoder->current = swap;
    decoder->filled = 0;
    decoder->pass.row++;
    if (decoder->pass.row == decoder->pass.height &&
        !png_begin_pass(decoder, decoder->pass.index + 1u))
    {
        decoder->rows_done = true;
    }
    return TB_BITMAP_OK;
}

/* ---------------------------------------------------------------------------
 * The image data.
 * ------------------------------------------------------------------------- */

/* Bytes of the image data before decompression: a row's filter type and
 * bytes, for every row of every pass. */
static uint64_t png_filtered_bytes(const png_header *header)
{
    uint64_t total = 0;

    for (uint32_t index = 0; index < (header->interlaced ? PNG_PASSES : 1u); index++)
    {
        uint32_t width = header->width;
        uint32_t height = header->height;
        if (header->interlaced)
        {
            width = png_pass_extent(header->width, png_pass_column[index], png_pass_across[index]);
            height = png_pass_extent(header->height, png_pass_row[index], png_pass_down[index]);
        }
        if (width > 0 && height > 0)
        {
            total += ((uint64_t)png_row_bytes(header, width) + 1u) * height;
        }
    }
    return total;
}

/* The bytes of every IDAT chunk from `at`, without checking CRCs: what the
 * image data could decompress to at most bounds the image. */
static uint64_t png_compressed_bytes(const png_decoder *decoder, size_t at)
{
    tb_bitmap_error ignored = {0};
    /* Damage is found, and reported, when the chunks are read. */
    png_decoder probe = *decoder;
    uint64_t total = 0;

    probe.error = &ignored;
    for (;;)
    {
        png_chunk chunk = {0};
        if (png_chunk_at(&probe, at, &chunk) != TB_BITMAP_OK || !png_named(&chunk, "IDAT"))
        {
            return total;
        }
        total += chunk.length;
        at = png_after(&chunk);
    }
}

/* Allocate the bitmap, the rows and the inflater, at the first IDAT chunk. */
static tb_bitmap_result png_begin_data(png_decoder *decoder, const png_chunk *chunk)
{
    const png_header *const header = &decoder->header;

    if (header->color == PNG_PALETTE && !decoder->seen_palette)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a palette image without PLTE");
    }
    if (png_compressed_bytes(decoder, chunk->at) * PNG_MOST_INFLATION < png_filtered_bytes(header))
    {
        return png_malformed_in(decoder, chunk, chunk->at,
                                "too little image data for the image's dimensions");
    }
    decoder->bitmap =
        tb_bitmap_try_create(header->width, header->height, TB_BITMAP_RGBA8, decoder->allocator);
    if (decoder->bitmap == NULL)
    {
        return tb_bitmap_fail(decoder->error, TB_BITMAP_NO_MEMORY, TB_BITMAP_NO_OFFSET,
                              "no memory for the pixels");
    }
    const size_t row = png_row_bytes(header, header->width) + 1u;
    decoder->rows_size = row * 2u;
    decoder->rows = tb_allocator_try_alloc(decoder->allocator, decoder->rows_size, 16u);
    if (decoder->rows == NULL)
    {
        return tb_bitmap_fail(decoder->error, TB_BITMAP_NO_MEMORY, TB_BITMAP_NO_OFFSET,
                              "no memory for the rows");
    }
    decoder->above = decoder->rows;
    decoder->current = decoder->rows + row;
    decoder->inflater = tb_inflater_create(TB_DEFLATE_ZLIB, decoder->allocator);
    (void)png_begin_pass(decoder, 0);
    return TB_BITMAP_OK;
}

static tb_bitmap_result png_inflate_failed(const png_decoder *decoder, const png_chunk *chunk,
                                           tb_deflate_result result)
{
    switch (result)
    {
    case TB_DEFLATE_CHECKSUM:
        return png_malformed_in(decoder, chunk, chunk->at,
                                "image data whose Adler-32 does not match");
    case TB_DEFLATE_UNSUPPORTED:
        return png_malformed_in(decoder, chunk, chunk->at,
                                "image data with a preset dictionary, which PNG forbids");
    case TB_DEFLATE_OK:
    case TB_DEFLATE_INVALID:
    case TB_DEFLATE_TOO_LARGE:
    case TB_DEFLATE_MALFORMED:
    case TB_DEFLATE_TRUNCATED:
    default:
        return png_malformed_in(decoder, chunk, chunk->at, "image data that is not a zlib stream");
    }
}

/* Decompress an IDAT chunk's data into rows. Data after the zlib stream's
 * end is ignored, as other readers ignore it; data for pixels past the
 * last row is damage. */
static tb_bitmap_result png_read_data(png_decoder *decoder, const png_chunk *chunk)
{
    const uint8_t *input = chunk->data;
    size_t left = chunk->length;

    TB_ASSERT(decoder->current != NULL && decoder->above != NULL);
    while (!tb_inflater_finished(decoder->inflater))
    {
        uint8_t spare = 0;
        uint8_t *output = &spare;
        size_t room = sizeof(spare);
        if (!decoder->rows_done)
        {
            output = decoder->current + decoder->filled;
            room = decoder->pass.row_bytes + 1u - decoder->filled;
        }
        size_t used = 0;
        size_t written = 0;
        const tb_deflate_result inflated =
            tb_inflater_run(decoder->inflater, input, left, &used, output, room, &written);
        input += used;
        left -= used;
        if (inflated != TB_DEFLATE_OK)
        {
            return png_inflate_failed(decoder, chunk, inflated);
        }
        if (decoder->rows_done && written > 0)
        {
            return png_malformed_in(decoder, chunk, chunk->at,
                                    "image data past the image's last row");
        }
        if (used == 0 && written == 0)
        {
            break; /* The stream goes on in the next chunk. */
        }
        decoder->filled += written;
        if (!decoder->rows_done && decoder->filled == decoder->pass.row_bytes + 1u)
        {
            const tb_bitmap_result result = png_finish_row(decoder, chunk);
            if (result != TB_BITMAP_OK)
            {
                return result;
            }
        }
    }
    return TB_BITMAP_OK;
}

/* ---------------------------------------------------------------------------
 * The file.
 * ------------------------------------------------------------------------- */

static tb_bitmap_result png_read_chunk(png_decoder *decoder, const png_chunk *chunk)
{
    const bool is_data = png_named(chunk, "IDAT");

    if (decoder->seen_data && !is_data)
    {
        decoder->data_ended = true;
    }
    if (is_data)
    {
        if (decoder->data_ended)
        {
            return png_malformed_in(decoder, chunk, chunk->at,
                                    "IDAT chunks that are not one after another");
        }
        if (!decoder->seen_data)
        {
            decoder->seen_data = true;
            const tb_bitmap_result begun = png_begin_data(decoder, chunk);
            if (begun != TB_BITMAP_OK)
            {
                return begun;
            }
        }
        return png_read_data(decoder, chunk);
    }
    if (png_named(chunk, "IHDR"))
    {
        return png_malformed_in(decoder, chunk, chunk->at, "a second IHDR chunk");
    }
    if (png_named(chunk, "PLTE"))
    {
        return png_read_palette(decoder, chunk);
    }
    if (png_named(chunk, "tRNS"))
    {
        return png_read_transparency(decoder, chunk);
    }
    /* A chunk whose name begins with a capital is critical: an image cannot
     * be shown without understanding it. Every other one is ancillary, and
     * skipped: color spaces and gamma (gAMA, sRGB, iCCP, cHRM), text, times,
     * and the frames of an animated PNG (acTL, fcTL, fdAT). */
    if (chunk->name[0] >= 'A' && chunk->name[0] <= 'Z')
    {
        return tb_bitmap_fail_in(decoder->error, TB_BITMAP_UNSUPPORTED, chunk->name, chunk->at,
                                 "a critical chunk the engine does not know");
    }
    return TB_BITMAP_OK;
}

/* The end: every row decoded, and the zlib stream ended. */
static tb_bitmap_result png_check_end(png_decoder *decoder, const png_chunk *chunk)
{
    if (chunk->length != 0)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "an IEND chunk with data");
    }
    if (!decoder->seen_data)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "no image data before IEND");
    }
    if (!decoder->rows_done)
    {
        return png_malformed_in(decoder, chunk, chunk->at, "image data that ends before the image");
    }
    if (!tb_inflater_finished(decoder->inflater))
    {
        return png_malformed_in(decoder, chunk, chunk->at,
                                "image data whose zlib stream does not end");
    }
    return TB_BITMAP_OK;
}

static tb_bitmap_result png_read_chunks(png_decoder *decoder, uint32_t max_side,
                                        uint64_t max_pixels)
{
    png_chunk chunk = {0};
    tb_bitmap_result result = png_chunk_at(decoder, PNG_SIGNATURE_BYTES, &chunk);

    if (result == TB_BITMAP_OK)
    {
        result = png_check_crc(decoder, &chunk);
    }
    if (result == TB_BITMAP_OK)
    {
        result = png_read_header(decoder, &chunk, max_side, max_pixels);
    }
    while (result == TB_BITMAP_OK)
    {
        result = png_chunk_at(decoder, png_after(&chunk), &chunk);
        if (result == TB_BITMAP_OK)
        {
            result = png_check_crc(decoder, &chunk);
        }
        if (result != TB_BITMAP_OK)
        {
            break;
        }
        if (png_named(&chunk, "IEND"))
        {
            return png_check_end(decoder, &chunk);
        }
        result = png_read_chunk(decoder, &chunk);
    }
    return result;
}

tb_bitmap_result tb_bitmap_decode_png(const uint8_t *bytes, size_t length, uint32_t max_side,
                                      uint64_t max_pixels, const tb_allocator *allocator,
                                      tb_bitmap **bitmap, tb_bitmap_error *error)
{
    png_decoder decoder = {
        .bytes = bytes, .length = length, .error = error, .allocator = allocator};
    const tb_bitmap_result result = png_read_chunks(&decoder, max_side, max_pixels);

    tb_inflater_destroy(decoder.inflater);
    if (decoder.rows != NULL)
    {
        tb_allocator_free(allocator, decoder.rows, decoder.rows_size, 16u);
    }
    if (result != TB_BITMAP_OK)
    {
        tb_bitmap_destroy(decoder.bitmap);
        return result;
    }
    *bitmap = decoder.bitmap;
    return TB_BITMAP_OK;
}
