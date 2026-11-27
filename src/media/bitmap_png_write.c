/* PNG files, written: 8 bits a sample in RGBA, RGB or gray, each row
 * filtered by the filter that makes it smallest by estimate, and compressed
 * with the engine's own DEFLATE. */
#include "bitmap_internal.h"
#include "bitmap_png.h"

#include "badger/assert.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/memory.h"

#include <string.h>

/* Bytes of compressed image data each IDAT chunk holds, but the last. */
#define PNG_IDAT_CAPACITY ((size_t)64 << 10)

/* The rendering intent of the sRGB chunk: perceptual, for pictures. */
#define PNG_SRGB_PERCEPTUAL 0u

typedef struct png_writer
{
    const tb_bitmap *bitmap;
    tb_stream *sink;
    tb_bitmap_error *error;
    tb_deflater *deflater;
    uint32_t color; /* The color type written. */
    size_t samples; /* Bytes of a pixel. */
    size_t row_bytes;
    uint8_t *above;   /* The row above, unfiltered, after a zero byte of filter type. */
    uint8_t *current; /* The row, likewise. */
    uint8_t *filtered[PNG_FILTER_COUNT]; /* The row under each filter, after its type. */
    uint8_t *idat;                       /* Compressed bytes not yet written. */
    size_t idat_used;
} png_writer;

static tb_bitmap_result png_refused(png_writer *writer)
{
    return tb_bitmap_fail(writer->error, TB_BITMAP_OUTPUT_FAILED, TB_BITMAP_NO_OFFSET,
                          "the stream refused the file");
}

/* Write a chunk: its length, name, data and CRC. */
static tb_bitmap_result png_write_chunk(png_writer *writer, const char *name, const uint8_t *data,
                                        size_t length)
{
    uint8_t head[PNG_CHUNK_HEAD] = {0};
    uint8_t crc_bytes[PNG_CHUNK_CRC] = {0};

    TB_ASSERT(length <= PNG_LONGEST_CHUNK);
    png_put_u32(head, (uint32_t)length);
    memcpy(head + 4, name, 4);
    uint32_t crc = tb_crc32(head + 4, 4);
    crc = tb_crc32_update(crc, data, length);
    png_put_u32(crc_bytes, crc);
    if (tb_stream_write_all(writer->sink, head, sizeof(head)) != TB_STREAM_OK ||
        (length > 0 && tb_stream_write_all(writer->sink, data, length) != TB_STREAM_OK) ||
        tb_stream_write_all(writer->sink, crc_bytes, sizeof(crc_bytes)) != TB_STREAM_OK)
    {
        return png_refused(writer);
    }
    return TB_BITMAP_OK;
}

/* Compress bytes into the IDAT buffer, writing a chunk whenever it fills. */
static tb_bitmap_result png_compress(png_writer *writer, const uint8_t *bytes, size_t length,
                                     tb_deflate_flush flush)
{
    for (;;)
    {
        size_t used = 0;
        size_t written = 0;
        (void)tb_deflater_run(writer->deflater, bytes, length, &used,
                              writer->idat + writer->idat_used,
                              PNG_IDAT_CAPACITY - writer->idat_used, &written, flush);
        bytes += used;
        length -= used;
        writer->idat_used += written;
        const bool full = writer->idat_used == PNG_IDAT_CAPACITY;
        const bool last = flush == TB_DEFLATE_FINISH && tb_deflater_finished(writer->deflater);
        if (full || (last && writer->idat_used > 0))
        {
            const tb_bitmap_result result =
                png_write_chunk(writer, "IDAT", writer->idat, writer->idat_used);
            if (result != TB_BITMAP_OK)
            {
                return result;
            }
            writer->idat_used = 0;
        }
        if (last || (flush != TB_DEFLATE_FINISH && length == 0 && !full))
        {
            return TB_BITMAP_OK;
        }
    }
}

/* A row of the bitmap as the file's samples, straight. */
static void png_encode_row(const png_writer *writer, uint32_t y, uint8_t *row)
{
    const tb_bitmap *const bitmap = writer->bitmap;
    const uint8_t *const pixels = tb_bitmap_const_row(bitmap, y);

    if (bitmap->format == TB_BITMAP_RGBA8 && !bitmap->premultiplied && writer->samples == 4u)
    {
        memcpy(row, pixels, writer->row_bytes);
        return;
    }
    if (bitmap->format == TB_BITMAP_GRAY8 && writer->samples == 1u)
    {
        memcpy(row, pixels, writer->row_bytes);
        return;
    }
    for (uint32_t x = 0; x < bitmap->width; x++)
    {
        tb_bitmap_color color = tb_bitmap_get_pixel(bitmap, x, y);
        if (bitmap->premultiplied)
        {
            color = tb_bitmap_unpremultiplied_color(color);
        }
        uint8_t *const sample = row + (size_t)x * writer->samples;
        if (writer->samples == 1u)
        {
            sample[0] = tb_bitmap_luminance(color);
            continue;
        }
        sample[0] = color.r;
        sample[1] = color.g;
        sample[2] = color.b;
        if (writer->samples == 4u)
        {
            sample[3] = color.a;
        }
    }
}

/* Filter a row every way, and choose the way whose bytes, taken as signed,
 * sum to the least magnitude: the heuristic of the PNG specification and
 * of libpng, which favours rows that DEFLATE compresses well. */
static const uint8_t *png_filter_row(png_writer *writer)
{
    const uint8_t *const row = writer->current + 1;
    const uint8_t *const above = writer->above + 1;
    const size_t back = writer->samples;
    const size_t length = writer->row_bytes;
    uint64_t best_cost = UINT64_MAX;
    size_t best = 0;

    for (size_t filter = 0; filter < PNG_FILTER_COUNT; filter++)
    {
        uint8_t *const out = writer->filtered[filter] + 1;
        uint64_t cost = 0;
        writer->filtered[filter][0] = (uint8_t)filter;
        for (size_t index = 0; index < length; index++)
        {
            const uint8_t left = index >= back ? row[index - back] : 0u;
            const uint8_t corner = index >= back ? above[index - back] : 0u;
            uint8_t predicted = 0;
            switch (filter)
            {
            case PNG_FILTER_SUB:
                predicted = left;
                break;
            case PNG_FILTER_UP:
                predicted = above[index];
                break;
            case PNG_FILTER_AVERAGE:
                predicted = (uint8_t)(((uint32_t)left + above[index]) >> 1);
                break;
            case PNG_FILTER_PAETH:
                predicted = png_paeth(left, above[index], corner);
                break;
            default:
                break;
            }
            out[index] = (uint8_t)(row[index] - predicted);
            cost += out[index] < 128u ? out[index] : 256u - out[index];
        }
        if (cost < best_cost)
        {
            best_cost = cost;
            best = filter;
        }
    }
    return writer->filtered[best];
}

/* Whether every pixel is opaque, and whether every one is gray too. */
static void png_survey(const tb_bitmap *bitmap, bool *opaque, bool *gray)
{
    *opaque = true;
    *gray = true;
    if (bitmap->format == TB_BITMAP_GRAY8)
    {
        return;
    }
    for (uint32_t y = 0; y < bitmap->height && (*opaque || *gray); y++)
    {
        for (uint32_t x = 0; x < bitmap->width; x++)
        {
            const tb_bitmap_color color = tb_bitmap_get_pixel(bitmap, x, y);
            *opaque = *opaque && color.a == UINT8_MAX;
            *gray = *gray && color.r == color.g && color.g == color.b;
        }
    }
}

/* The color type for the options: AUTO is the fewest samples that hold
 * every pixel exactly. */
static uint32_t png_color_of(const tb_bitmap *bitmap, tb_bitmap_png_color color)
{
    bool opaque = false;
    bool gray = false;

    switch (color)
    {
    case TB_BITMAP_PNG_RGBA8:
        return PNG_RGBA;
    case TB_BITMAP_PNG_RGB8:
        return PNG_RGB;
    case TB_BITMAP_PNG_GRAY8:
        return PNG_GRAY;
    case TB_BITMAP_PNG_AUTO:
    default:
        png_survey(bitmap, &opaque, &gray);
        if (!opaque)
        {
            return PNG_RGBA;
        }
        return gray ? PNG_GRAY : PNG_RGB;
    }
}

static tb_bitmap_result png_write_header(png_writer *writer)
{
    static const uint8_t srgb[1] = {PNG_SRGB_PERCEPTUAL};
    uint8_t header[PNG_IHDR_BYTES] = {0};

    png_put_u32(header, writer->bitmap->width);
    png_put_u32(header + 4, writer->bitmap->height);
    header[8] = 8u;
    header[9] = (uint8_t)writer->color;
    if (tb_stream_write_all(writer->sink, tb_bitmap_png_signature,
                            sizeof(tb_bitmap_png_signature)) != TB_STREAM_OK)
    {
        return png_refused(writer);
    }
    const tb_bitmap_result result = png_write_chunk(writer, "IHDR", header, sizeof(header));
    if (result != TB_BITMAP_OK)
    {
        return result;
    }
    /* The pixels of a bitmap are sRGB, and saying so keeps viewers that
     * manage color from guessing. */
    return png_write_chunk(writer, "sRGB", srgb, sizeof(srgb));
}

static tb_bitmap_result png_write_rows(png_writer *writer)
{
    for (uint32_t y = 0; y < writer->bitmap->height; y++)
    {
        png_encode_row(writer, y, writer->current + 1);
        const uint8_t *const filtered = png_filter_row(writer);
        const tb_bitmap_result result =
            png_compress(writer, filtered, writer->row_bytes + 1u, TB_DEFLATE_NO_FLUSH);
        if (result != TB_BITMAP_OK)
        {
            return result;
        }
        uint8_t *const swap = writer->above;
        writer->above = writer->current;
        writer->current = swap;
    }
    return png_compress(writer, NULL, 0, TB_DEFLATE_FINISH);
}

/* The level of the options: 0 is the default. */
static int png_level_of(const tb_bitmap_png_options *options)
{
    if (options == NULL || options->level == 0)
    {
        return TB_DEFLATE_DEFAULT_LEVEL;
    }
    return options->level;
}

tb_bitmap_result tb_bitmap_write_png(const tb_bitmap *bitmap, const tb_bitmap_png_options *options,
                                     tb_stream *sink, tb_bitmap_error *error)
{
    tb_bitmap_error ignored = {0};
    png_writer writer = {.bitmap = bitmap, .sink = sink, .error = error != NULL ? error : &ignored};
    uint8_t *rows = NULL;
    size_t rows_size = 0;
    tb_bitmap_result result = TB_BITMAP_OK;
    const int level = png_level_of(options);

    TB_ASSERT(bitmap != NULL);
    TB_ASSERT(sink != NULL);
    *writer.error = (tb_bitmap_error){.reason = ""};
    if (bitmap->width == 0 || bitmap->height == 0)
    {
        result = tb_bitmap_fail(writer.error, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "a bitmap of no pixels, which no PNG file holds");
        goto done;
    }
    if (level < 1 || level > TB_DEFLATE_BEST_LEVEL ||
        (options != NULL && (uint32_t)options->color > (uint32_t)TB_BITMAP_PNG_GRAY8))
    {
        result = tb_bitmap_fail(writer.error, TB_BITMAP_INVALID, TB_BITMAP_NO_OFFSET,
                                "options of no PNG file");
        goto done;
    }
    writer.color = png_color_of(bitmap, options != NULL ? options->color : TB_BITMAP_PNG_AUTO);
    writer.samples = writer.color == PNG_RGBA ? 4u : writer.color == PNG_RGB ? 3u : 1u;
    writer.row_bytes = (size_t)bitmap->width * writer.samples;
    /* Two rows to filter from, five to filter into, and the IDAT buffer. */
    const size_t row = writer.row_bytes + 1u;
    rows_size = row * (2u + PNG_FILTER_COUNT) + PNG_IDAT_CAPACITY;
    rows = tb_calloc(rows_size, 1u);
    writer.above = rows;
    writer.current = rows + row;
    for (size_t filter = 0; filter < PNG_FILTER_COUNT; filter++)
    {
        writer.filtered[filter] = rows + row * (2u + filter);
    }
    writer.idat = rows + row * (2u + PNG_FILTER_COUNT);
    writer.deflater = tb_deflater_create(TB_DEFLATE_ZLIB, level, NULL);
    result = png_write_header(&writer);
    if (result == TB_BITMAP_OK)
    {
        result = png_write_rows(&writer);
    }
    if (result == TB_BITMAP_OK)
    {
        result = png_write_chunk(&writer, "IEND", NULL, 0);
    }
done:
    tb_deflater_destroy(writer.deflater);
    tb_free(rows);
    tb_bitmap_describe(writer.error, NULL);
    return result;
}
