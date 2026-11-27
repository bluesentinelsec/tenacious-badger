/* PNG files: every color type and bit depth, interlaced and not, with and
 * without transparency, read from files that encoders other than the
 * engine's wrote (tools/make_png_test_data.py); the writer's files read
 * back exactly; and loading through streams. */
#include "badger/bitmap.h"
#include "badger/fmt.h"
#include "badger/rng.h"
#include "badger/str.h"
#include "png_builder.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdlib.h>
#include <string.h>

extern const tb_test_file tb_png_files[];
extern const size_t tb_png_files_count;

/* The pattern every file holds: keep in step with make_png_test_data.py. */
#define PATTERN_WIDTH 13u
#define PATTERN_HEIGHT 11u

static uint32_t pattern(uint32_t x, uint32_t y, uint32_t channel, uint32_t depth)
{
    const uint32_t value =
        (x * 4111u + y * 7919u + channel * 1237u + x * y * 31u + (x ^ y) * 263u) & 0xFFFFu;
    return value >> (16u - depth);
}

static tb_bitmap_color palette_color(uint32_t index)
{
    return (tb_bitmap_color){(uint8_t)((index * 71u + 5u) & 255u),
                             (uint8_t)((index * 113u + 40u) & 255u),
                             (uint8_t)((index * 29u + 7u) & 255u), 255};
}

/* A sample of `depth` bits at 8: spread evenly, or 16 rounded to nearest. */
static uint8_t widen(uint32_t sample, uint32_t depth)
{
    if (depth == 16u)
    {
        return (uint8_t)((sample * 255u + 32767u) / 65535u);
    }
    return (uint8_t)(sample * 255u / ((1u << depth) - 1u));
}

/* What a file of the pattern decodes to at (x, y). */
static tb_bitmap_color expected_pixel(uint32_t color, uint32_t depth, bool transparent, uint32_t x,
                                      uint32_t y)
{
    uint32_t samples[4] = {0};
    for (uint32_t channel = 0; channel < 4u; channel++)
    {
        samples[channel] = pattern(x, y, channel, depth);
    }
    switch (color)
    {
    case 0: {
        const uint8_t gray = widen(samples[0], depth);
        const bool clear = transparent && samples[0] == pattern(1, 0, 0, depth);
        return (tb_bitmap_color){gray, gray, gray, clear ? 0u : 255u};
    }
    case 2: {
        bool clear = transparent;
        for (uint32_t channel = 0; channel < 3u; channel++)
        {
            clear = clear && samples[channel] == pattern(1, 0, channel, depth);
        }
        return (tb_bitmap_color){widen(samples[0], depth), widen(samples[1], depth),
                                 widen(samples[2], depth), clear ? 0u : 255u};
    }
    case 3: {
        tb_bitmap_color entry = palette_color(samples[0]);
        if (transparent && samples[0] <= (1u << depth) / 2u)
        {
            entry.a = (uint8_t)((samples[0] * 97u + 3u) & 255u);
        }
        return entry;
    }
    case 4: {
        const uint8_t gray = widen(samples[0], depth);
        return (tb_bitmap_color){gray, gray, gray, widen(samples[1], depth)};
    }
    default:
        return (tb_bitmap_color){widen(samples[0], depth), widen(samples[1], depth),
                                 widen(samples[2], depth), widen(samples[3], depth)};
    }
}

static const tb_test_file *find_file(const char *name)
{
    size_t index = 0;
    while (index < tb_png_files_count && !tb_str_equal(tb_png_files[index].name, name))
    {
        index++;
    }
    TB_TEST_CONTEXT("%s", name);
    TB_TEST_REQUIRE_UINT(index, <, tb_png_files_count);
    return &tb_png_files[index];
}

static tb_bitmap *decode_file(const char *name)
{
    const tb_test_file *const file = find_file(name);
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    const tb_bitmap_decode_options options = {.name = name};

    const tb_bitmap_result result =
        tb_bitmap_decode(file->bytes, file->length, &options, NULL, &bitmap, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_BITMAP_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    TB_TEST_EXPECT_UINT(info.format, ==, TB_BITMAP_RGBA8);
    TB_TEST_EXPECT(!info.premultiplied);
    TB_TEST_EXPECT(info.owns_pixels);
    return bitmap;
}

static void expect_pattern(const char *name, uint32_t color, uint32_t depth, bool transparent)
{
    tb_bitmap *const bitmap = decode_file(name);
    tb_bitmap_info info = {0};

    tb_bitmap_info_of(bitmap, &info);
    TB_TEST_CONTEXT("%s", name);
    TB_TEST_REQUIRE_UINT(info.width, ==, PATTERN_WIDTH);
    TB_TEST_REQUIRE_UINT(info.height, ==, PATTERN_HEIGHT);
    for (uint32_t y = 0; y < PATTERN_HEIGHT; y++)
    {
        for (uint32_t x = 0; x < PATTERN_WIDTH; x++)
        {
            const tb_bitmap_color expected = expected_pixel(color, depth, transparent, x, y);
            const tb_bitmap_color actual = tb_bitmap_get_pixel(bitmap, x, y);
            TB_TEST_CONTEXT("%s at (%u, %u)", name, x, y);
            TB_TEST_REQUIRE_BYTES(&actual, &expected, sizeof(expected));
        }
    }
    tb_bitmap_destroy(bitmap);
}

/* The Paeth predictor, as the specification writes it. */
static int paeth(int left, int above, int corner)
{
    const int estimate = left + above - corner;
    const int to_left = abs(estimate - left);
    const int to_above = abs(estimate - above);
    const int to_corner = abs(estimate - corner);
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

typedef struct variant
{
    uint32_t color;
    uint32_t depth;
} variant;

static const variant variants[] = {{0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1},
                                   {3, 2}, {3, 4}, {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16}};

/* Every color type at every bit depth PNG has, not interlaced and
 * interlaced, each row filtered by a different filter. */
TB_TEST(media_bitmap_png, reads_every_color_type_and_depth)
{
    char name[64] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(variants); index++)
    {
        for (uint32_t interlace = 0; interlace < 2u; interlace++)
        {
            (void)tb_fmt_buffer(name, sizeof(name), "py-c%u-d%u-i%u.png", variants[index].color,
                                variants[index].depth, interlace);
            expect_pattern(name, variants[index].color, variants[index].depth, false);
        }
    }
}

/* tRNS: a gray or RGB color that is transparent, compared at the file's
 * own depth, and alphas for the first entries of a palette. */
TB_TEST(media_bitmap_png, reads_transparency)
{
    char name[64] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(variants); index++)
    {
        const uint32_t color = variants[index].color;
        if (color == 4u || color == 6u)
        {
            continue;
        }
        (void)tb_fmt_buffer(name, sizeof(name), "py-c%u-d%u-trns.png", color,
                            variants[index].depth);
        expect_pattern(name, color, variants[index].depth, true);
    }
}

/* Interlaced images too small for some passes, which have no rows, read
 * as the same image not interlaced. */
TB_TEST(media_bitmap_png, reads_interlaced_images_with_empty_passes)
{
    static const char *const names[] = {"py-tiny-1x1-i1.png", "py-tiny-2x3-i1.png",
                                        "py-tiny-3x1-i1.png", "py-tiny-1x5-i1.png",
                                        "py-tiny-5x5-i1.png"};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        tb_bitmap *const bitmap = decode_file(names[index]);
        tb_bitmap_info info = {0};
        tb_bitmap_info_of(bitmap, &info);
        for (uint32_t y = 0; y < info.height; y++)
        {
            for (uint32_t x = 0; x < info.width; x++)
            {
                const tb_bitmap_color expected = expected_pixel(6, 8, false, x, y);
                const tb_bitmap_color actual = tb_bitmap_get_pixel(bitmap, x, y);
                TB_TEST_CONTEXT("%s at (%u, %u)", names[index], x, y);
                TB_TEST_EXPECT_BYTES(&actual, &expected, sizeof(expected));
            }
        }
        tb_bitmap_destroy(bitmap);
    }
}

/* Ancillary chunks of every kind are skipped, gamma and color spaces among
 * them; IDAT chunks of one byte, and of none, join up; stored blocks need
 * no compression; and an animated PNG is read as its default image. */
TB_TEST(media_bitmap_png, skips_what_it_does_not_need)
{
    expect_pattern("py-chunks.png", 6, 8, false);
    expect_pattern("py-apng.png", 6, 8, false);
    expect_pattern("py-level0.png", 2, 8, false);
}

/* Files of Windows' encoders, WIC and GDI+, as they write them: with sRGB,
 * gAMA and pHYs chunks, and filters and compression of their own. */
TB_TEST(media_bitmap_png, reads_what_other_encoders_write)
{
    expect_pattern("wic-bgra32.png", 6, 8, false);
    expect_pattern("wic-bgra32-i1.png", 6, 8, false);
    expect_pattern("wic-bgr24.png", 2, 8, false);
    expect_pattern("wic-gray8.png", 0, 8, false);
    expect_pattern("wic-gray16.png", 0, 16, false);
    expect_pattern("wic-rgba64.png", 6, 16, false);
    expect_pattern("gdiplus-argb.png", 6, 8, false);
    /* WIC's palette holds the pattern's indices' colors. */
    tb_bitmap *const bitmap = decode_file("wic-indexed8.png");
    for (uint32_t y = 0; y < PATTERN_HEIGHT; y++)
    {
        for (uint32_t x = 0; x < PATTERN_WIDTH; x++)
        {
            const tb_bitmap_color expected = palette_color(pattern(x, y, 0, 8));
            const tb_bitmap_color actual = tb_bitmap_get_pixel(bitmap, x, y);
            TB_TEST_CONTEXT("at (%u, %u)", x, y);
            TB_TEST_EXPECT_BYTES(&actual, &expected, sizeof(expected));
        }
    }
    tb_bitmap_destroy(bitmap);
}

/* Rows of every filter type over every pixel width, built here: each
 * filter's arithmetic wraps modulo 256, and looks back a whole pixel. */
TB_TEST(media_bitmap_png, every_filter_at_every_pixel_width)
{
    static const struct
    {
        uint32_t color;
        uint32_t depth;
        size_t bytes; /* Of a pixel. */
    } layouts[] = {{0, 8, 1}, {4, 8, 2}, {2, 8, 3}, {6, 8, 4}, {2, 16, 6}, {6, 16, 8}};
    tb_rng rng = {0};
    tb_rng_seed(&rng, 0x9E1u);

    for (size_t layout = 0; layout < TB_TEST_COUNT_OF(layouts); layout++)
    {
        enum
        {
            width = 7,
            height = 10
        };
        const size_t row_bytes = width * layouts[layout].bytes;
        uint8_t raw[height][width * 8] = {{0}};
        uint8_t filtered[height * (width * 8 + 1)] = {0};
        size_t used = 0;
        for (size_t y = 0; y < height; y++)
        {
            const uint32_t filter = (uint32_t)(y % 5u);
            for (size_t index = 0; index < row_bytes; index++)
            {
                raw[y][index] = (uint8_t)tb_rng_next_u32(&rng);
            }
            filtered[used++] = (uint8_t)filter;
            for (size_t index = 0; index < row_bytes; index++)
            {
                const size_t back = layouts[layout].bytes;
                const int left = index >= back ? raw[y][index - back] : 0;
                const int above = y > 0 ? raw[y - 1][index] : 0;
                const int corner = (y > 0 && index >= back) ? raw[y - 1][index - back] : 0;
                int predicted = 0;
                if (filter == 1)
                {
                    predicted = left;
                }
                else if (filter == 2)
                {
                    predicted = above;
                }
                else if (filter == 3)
                {
                    predicted = (left + above) / 2;
                }
                else if (filter == 4)
                {
                    predicted = paeth(left, above, corner);
                }
                filtered[used++] = (uint8_t)(raw[y][index] - predicted);
            }
        }
        png_build file = {0};
        png_build_begin(&file);
        png_build_header(&file, width, height, layouts[layout].depth, layouts[layout].color, 0);
        png_build_data(&file, filtered, used, 0);
        png_build_end(&file);
        tb_bitmap *bitmap = NULL;
        TB_TEST_REQUIRE_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL),
                             ==, TB_BITMAP_OK);
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                const uint8_t *const pixel = raw[y] + (size_t)x * layouts[layout].bytes;
                const tb_bitmap_color actual = tb_bitmap_get_pixel(bitmap, x, y);
                TB_TEST_CONTEXT("layout %zu at (%u, %u)", layout, x, y);
                if (layouts[layout].depth == 8u && layouts[layout].color == 6u)
                {
                    TB_TEST_EXPECT_BYTES(&actual, pixel, 4);
                }
                else if (layouts[layout].depth == 8u && layouts[layout].color == 2u)
                {
                    TB_TEST_EXPECT_BYTES(&actual, pixel, 3);
                }
                else if (layouts[layout].color == 0u)
                {
                    TB_TEST_EXPECT_UINT(actual.g, ==, pixel[0]);
                }
                else if (layouts[layout].color == 4u)
                {
                    TB_TEST_EXPECT_UINT(actual.r, ==, pixel[0]);
                    TB_TEST_EXPECT_UINT(actual.a, ==, pixel[1]);
                }
                else
                {
                    /* 16 bits: the high byte, rounded by the low. */
                    const uint32_t red = (uint32_t)pixel[0] << 8 | pixel[1];
                    TB_TEST_EXPECT_UINT(actual.r, ==, widen(red, 16));
                }
            }
        }
        tb_bitmap_destroy(bitmap);
    }
}

/* ---------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

static void write_to_buffer(const tb_bitmap *bitmap, const tb_bitmap_png_options *options,
                            tb_stream *sink)
{
    tb_bitmap_error error = {0};
    tb_stream_open_buffer(NULL, sink);
    TB_TEST_REQUIRE_UINT(tb_bitmap_write_png(bitmap, options, sink, &error), ==, TB_BITMAP_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
}

static tb_bitmap *read_back(tb_stream *sink, uint32_t *color)
{
    size_t length = 0;
    const unsigned char *const bytes = tb_stream_buffer_bytes(sink, &length);
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    TB_TEST_REQUIRE_UINT(length, >, 26u);
    *color = bytes[25];
    const tb_bitmap_result result = tb_bitmap_decode(bytes, length, NULL, NULL, &bitmap, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_BITMAP_OK);
    return bitmap;
}

/* Random pixels, alpha and all, of every width to 17 and two heights, read
 * back exactly at every level. */
TB_TEST(media_bitmap_png, rgba_round_trips_exactly)
{
    for (uint32_t width = 1; width <= 17; width++)
    {
        for (int level = 0; level <= 9; level += 3)
        {
            tb_bitmap *const original = tb_bitmap_create(width, 5, TB_BITMAP_RGBA8, NULL);
            tb_test_random_bytes(tb_bitmap_pixels(original), (size_t)width * 5u * 4u);
            const tb_bitmap_png_options options = {.level = level == 0 ? 1 : level};
            tb_stream sink = {0};
            uint32_t color = 0;
            write_to_buffer(original, &options, &sink);
            tb_bitmap *const copy = read_back(&sink, &color);
            TB_TEST_CONTEXT("width %u, level %d", width, options.level);
            TB_TEST_EXPECT_UINT(color, ==, 6);
            TB_TEST_EXPECT(tb_bitmap_equal(original, copy));
            tb_bitmap_destroy(copy);
            tb_bitmap_destroy(original);
            (void)tb_stream_close(&sink);
        }
    }
}

/* AUTO writes the fewest samples that keep every pixel: gray for opaque
 * gray, RGB for opaque color, RGBA otherwise, and gray for a gray bitmap. */
TB_TEST(media_bitmap_png, auto_chooses_the_fewest_samples)
{
    tb_bitmap *const bitmap = tb_bitmap_create(9, 4, TB_BITMAP_RGBA8, NULL);
    tb_stream sink = {0};
    uint32_t color = 0;

    for (uint32_t y = 0; y < 4u; y++)
    {
        for (uint32_t x = 0; x < 9u; x++)
        {
            const uint8_t value = (uint8_t)(x * 29u + y * 61u);
            tb_bitmap_set_pixel(bitmap, x, y, (tb_bitmap_color){value, value, value, 255});
        }
    }
    write_to_buffer(bitmap, NULL, &sink);
    tb_bitmap *copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 0);
    TB_TEST_EXPECT(tb_bitmap_equal(bitmap, copy));
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&sink);

    tb_bitmap_set_pixel(bitmap, 3, 2, (tb_bitmap_color){1, 2, 3, 255});
    write_to_buffer(bitmap, NULL, &sink);
    copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 2);
    TB_TEST_EXPECT(tb_bitmap_equal(bitmap, copy));
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&sink);

    tb_bitmap_set_pixel(bitmap, 8, 3, (tb_bitmap_color){1, 1, 1, 254});
    write_to_buffer(bitmap, NULL, &sink);
    copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 6);
    TB_TEST_EXPECT(tb_bitmap_equal(bitmap, copy));
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&sink);

    tb_bitmap *const gray = tb_bitmap_convert(bitmap, TB_BITMAP_GRAY8, NULL);
    write_to_buffer(gray, NULL, &sink);
    copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 0);
    tb_bitmap *const expected = tb_bitmap_convert(gray, TB_BITMAP_RGBA8, NULL);
    TB_TEST_EXPECT(tb_bitmap_equal(expected, copy));
    tb_bitmap_destroy(expected);
    tb_bitmap_destroy(copy);
    tb_bitmap_destroy(gray);
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(bitmap);
}

/* Each color type written on request: RGB drops alpha, gray takes the
 * luminance tb_bitmap_convert() takes, and a premultiplied or BGRA bitmap
 * is written straight, in RGBA order. */
TB_TEST(media_bitmap_png, writes_each_color_type_on_request)
{
    tb_bitmap *const bitmap = tb_bitmap_create(6, 3, TB_BITMAP_BGRA8, NULL);
    tb_stream sink = {0};
    uint32_t color = 0;

    for (uint32_t y = 0; y < 3u; y++)
    {
        for (uint32_t x = 0; x < 6u; x++)
        {
            tb_bitmap_set_pixel(bitmap, x, y,
                                (tb_bitmap_color){(uint8_t)(x * 40u), (uint8_t)(y * 90u),
                                                  (uint8_t)(x * y * 13u),
                                                  (uint8_t)(255u - x * 20u)});
        }
    }
    const tb_bitmap_png_options rgb = {.color = TB_BITMAP_PNG_RGB8};
    write_to_buffer(bitmap, &rgb, &sink);
    tb_bitmap *copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 2);
    for (uint32_t x = 0; x < 6u; x++)
    {
        tb_bitmap_color expected = tb_bitmap_get_pixel(bitmap, x, 2);
        expected.a = 255;
        const tb_bitmap_color actual = tb_bitmap_get_pixel(copy, x, 2);
        TB_TEST_EXPECT_BYTES(&actual, &expected, sizeof(expected));
    }
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&sink);

    const tb_bitmap_png_options gray = {.color = TB_BITMAP_PNG_GRAY8};
    write_to_buffer(bitmap, &gray, &sink);
    copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 0);
    tb_bitmap *const luminance = tb_bitmap_convert(bitmap, TB_BITMAP_GRAY8, NULL);
    for (uint32_t x = 0; x < 6u; x++)
    {
        TB_TEST_EXPECT_UINT(tb_bitmap_get_pixel(copy, x, 1).r, ==,
                            tb_bitmap_get_pixel(luminance, x, 1).r);
    }
    tb_bitmap_destroy(luminance);
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&sink);

    tb_bitmap *const straight = tb_bitmap_convert(bitmap, TB_BITMAP_RGBA8, NULL);
    tb_bitmap_premultiply(bitmap);
    const tb_bitmap_png_options rgba = {.color = TB_BITMAP_PNG_RGBA8};
    write_to_buffer(bitmap, &rgba, &sink);
    copy = read_back(&sink, &color);
    TB_TEST_EXPECT_UINT(color, ==, 6);
    tb_bitmap_premultiply(copy);
    tb_bitmap_premultiply(straight);
    TB_TEST_EXPECT(tb_bitmap_equal(straight, copy));
    tb_bitmap_destroy(copy);
    tb_bitmap_destroy(straight);
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(bitmap);
}

/* An image larger than one IDAT chunk of the writer, of noise that does
 * not compress, reads back exactly. */
TB_TEST(media_bitmap_png, a_large_image_spans_chunks)
{
    tb_bitmap *const original = tb_bitmap_create(300, 200, TB_BITMAP_RGBA8, NULL);
    tb_stream sink = {0};
    uint32_t color = 0;
    size_t length = 0;

    tb_test_random_bytes(tb_bitmap_pixels(original), 300u * 200u * 4u);
    write_to_buffer(original, NULL, &sink);
    (void)tb_stream_buffer_bytes(&sink, &length);
    TB_TEST_EXPECT_UINT(length, >, 240000u);
    tb_bitmap *const copy = read_back(&sink, &color);
    TB_TEST_EXPECT(tb_bitmap_equal(original, copy));
    tb_bitmap_destroy(copy);
    tb_bitmap_destroy(original);
    (void)tb_stream_close(&sink);
}

/* What the writer refuses, and a sink that refuses it. */
TB_TEST(media_bitmap_png, writing_fails_cleanly)
{
    tb_bitmap *const empty = tb_bitmap_create(0, 4, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const bitmap = tb_bitmap_create(4, 4, TB_BITMAP_RGBA8, NULL);
    tb_stream sink = {0};
    tb_bitmap_error error = {0};
    const tb_bitmap_png_options level = {.level = 10};
    const tb_bitmap_png_options color = {.color = (tb_bitmap_png_color)7};

    tb_stream_open_buffer(NULL, &sink);
    TB_TEST_EXPECT_UINT(tb_bitmap_write_png(empty, NULL, &sink, &error), ==, TB_BITMAP_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==, "a bitmap of no pixels, which no PNG file holds");
    TB_TEST_EXPECT_UINT(tb_bitmap_write_png(bitmap, &level, &sink, &error), ==, TB_BITMAP_INVALID);
    TB_TEST_EXPECT_UINT(tb_bitmap_write_png(bitmap, &color, &sink, NULL), ==, TB_BITMAP_INVALID);
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(empty);
    tb_bitmap_destroy(bitmap);
}

/* A sink that takes `limit` bytes, then refuses. */
typedef struct short_sink
{
    size_t taken;
    size_t limit;
} short_sink;

static tb_stream_result short_sink_write(void *state, const void *bytes, size_t length,
                                         size_t *transferred)
{
    short_sink *const sink = state;
    (void)bytes;
    *transferred = 0;
    if (sink->taken + length > sink->limit)
    {
        return TB_STREAM_NO_SPACE;
    }
    sink->taken += length;
    *transferred = length;
    return TB_STREAM_OK;
}

static tb_stream_result short_sink_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

/* A sink that fills anywhere, in the signature, a chunk's head, data or
 * CRC, fails the write. */
TB_TEST(media_bitmap_png, a_sink_that_fills_fails_the_write)
{
    static const tb_stream_functions functions = {.write_bytes = short_sink_write,
                                                  .release = short_sink_release};
    tb_bitmap *const bitmap = tb_bitmap_create(40, 40, TB_BITMAP_RGBA8, NULL);
    tb_stream full = {0};
    tb_stream_open_buffer(NULL, &full);
    tb_test_random_bytes(tb_bitmap_pixels(bitmap), 40u * 40u * 4u);
    TB_TEST_REQUIRE_UINT(tb_bitmap_write_png(bitmap, NULL, &full, NULL), ==, TB_BITMAP_OK);
    size_t length = 0;
    (void)tb_stream_buffer_bytes(&full, &length);
    (void)tb_stream_close(&full);

    for (size_t limit = 0; limit < length; limit += limit < 64u ? 1u : 997u)
    {
        short_sink state = {.limit = limit};
        tb_stream sink = {.functions = &functions, .state = &state};
        tb_bitmap_error error = {0};
        TB_TEST_CONTEXT("limit %zu", limit);
        TB_TEST_EXPECT_UINT(tb_bitmap_write_png(bitmap, NULL, &sink, &error), ==,
                            TB_BITMAP_OUTPUT_FAILED);
        TB_TEST_EXPECT_STRING(error.message, ==, "the stream refused the file");
    }
    tb_bitmap_destroy(bitmap);
}

/* ---------------------------------------------------------------------------
 * Streams.
 * ------------------------------------------------------------------------- */

/* tb_bitmap_read() and tb_bitmap_load() find a PNG file by its signature,
 * as tb_bitmap_decode() does. */
TB_TEST(media_bitmap_png, reads_from_a_stream)
{
    const tb_test_file *const file = find_file("py-c6-d8-i1.png");
    tb_stream stream = {0};
    tb_bitmap *bitmap = NULL;

    tb_stream_open_memory(file->bytes, file->length, NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_bitmap_read(&stream, NULL, NULL, &bitmap, NULL), ==, TB_BITMAP_OK);
    tb_bitmap *const decoded = decode_file("py-c6-d8-i1.png");
    TB_TEST_EXPECT(tb_bitmap_equal(bitmap, decoded));
    tb_bitmap_destroy(decoded);
    tb_bitmap_destroy(bitmap);
    (void)tb_stream_close(&stream);
}
