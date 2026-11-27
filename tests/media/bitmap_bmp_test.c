/* BMP files: every header, depth and compression read from files built
 * here byte by byte; exact round trips through the writer; every way a
 * file can be damaged, refused with where and why; damage at random, read
 * without a fault under the sanitizers; and loading through streams and a
 * VFS. */
#include "badger/bitmap.h"
#include "badger/rng.h"
#include "badger/str.h"
#include "badger/vfs.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

/* Room for every file a case builds. */
#define BMP_CAPACITY 4096u

/* What a file is built of. Zero-initialize, then set what differs. */
typedef struct bmp_spec
{
    uint32_t header; /* Size of the information header: 12, 40, 52, 56, 108 or 124. */
    int32_t width;
    int32_t height; /* Negative for rows from the top. */
    uint32_t bits;
    uint32_t compression;
    uint32_t colors_used;
    uint32_t masks[4];      /* In the header from version 2, or after the plain one. */
    size_t trailing_masks;  /* Masks after a plain header: 0, 3 or 4. */
    const uint8_t *palette; /* Blue, green, red, and a reserved byte unless the header is 12. */
    size_t palette_bytes;
    const uint8_t *pixels;
    size_t pixel_bytes;
} bmp_spec;

typedef struct bmp_file
{
    uint8_t bytes[BMP_CAPACITY];
    size_t length;
} bmp_file;

static void put_u16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *at, uint32_t value)
{
    put_u16(at, value & 0xFFFFu);
    put_u16(at + 2, value >> 16);
}

static void build(const bmp_spec *spec, bmp_file *file)
{
    uint8_t *const bytes = file->bytes;
    const size_t masks_at = 14u + spec->header;
    const size_t palette_at = masks_at + spec->trailing_masks * 4u;
    const size_t pixels_at = palette_at + spec->palette_bytes;

    memset(file, 0, sizeof(*file));
    TB_TEST_REQUIRE_UINT(pixels_at + spec->pixel_bytes, <=, BMP_CAPACITY);
    bytes[0] = 'B';
    bytes[1] = 'M';
    put_u32(bytes + 2, (uint32_t)(pixels_at + spec->pixel_bytes));
    put_u32(bytes + 10, (uint32_t)pixels_at);
    put_u32(bytes + 14, spec->header);
    if (spec->header == 12u)
    {
        put_u16(bytes + 18, (uint32_t)spec->width);
        put_u16(bytes + 20, (uint32_t)spec->height);
        put_u16(bytes + 22, 1);
        put_u16(bytes + 24, spec->bits);
    }
    else
    {
        put_u32(bytes + 18, (uint32_t)spec->width);
        put_u32(bytes + 22, (uint32_t)spec->height);
        put_u16(bytes + 26, 1);
        put_u16(bytes + 28, spec->bits);
        put_u32(bytes + 30, spec->compression);
        put_u32(bytes + 46, spec->colors_used);
        const size_t in_header = spec->header >= 56u ? 4u : spec->header >= 52u ? 3u : 0u;
        for (size_t index = 0; index < in_header; index++)
        {
            put_u32(bytes + 54 + index * 4u, spec->masks[index]);
        }
        for (size_t index = 0; index < spec->trailing_masks; index++)
        {
            put_u32(bytes + masks_at + index * 4u, spec->masks[index]);
        }
    }
    if (spec->palette_bytes > 0)
    {
        memcpy(bytes + palette_at, spec->palette, spec->palette_bytes);
    }
    if (spec->pixel_bytes > 0)
    {
        memcpy(bytes + pixels_at, spec->pixels, spec->pixel_bytes);
    }
    file->length = pixels_at + spec->pixel_bytes;
}

static tb_bitmap *decode_ok(const bmp_file *file)
{
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    const tb_bitmap_result result =
        tb_bitmap_decode(file->bytes, file->length, NULL, NULL, &bitmap, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_BITMAP_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    TB_TEST_EXPECT_STRING(error.message, ==, "");
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    TB_TEST_EXPECT_UINT(info.format, ==, TB_BITMAP_RGBA8);
    TB_TEST_EXPECT(!info.premultiplied);
    return bitmap;
}

/* Compare a decoded bitmap with rows of RGBA bytes from the top. */
static void expect_pixels(const tb_bitmap *bitmap, uint32_t width, uint32_t height,
                          const uint8_t *rgba)
{
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    TB_TEST_REQUIRE_UINT(info.width, ==, width);
    TB_TEST_REQUIRE_UINT(info.height, ==, height);
    TB_TEST_EXPECT_BYTES(tb_bitmap_const_pixels(bitmap), rgba, (size_t)width * height * 4u);
}

static void expect_decoded(const bmp_spec *spec, uint32_t width, uint32_t height,
                           const uint8_t *rgba)
{
    bmp_file file = {0};
    build(spec, &file);
    tb_bitmap *const bitmap = decode_ok(&file);
    expect_pixels(bitmap, width, height, rgba);
    tb_bitmap_destroy(bitmap);
}

/* Two rows of two pixels: red, green above blue, white, as RGBA from the top. */
static const uint8_t four_colors[] = {255, 0, 0,   255, 0,   255, 0,   255,
                                      0,   0, 255, 255, 255, 255, 255, 255};

/* ---------------------------------------------------------------------------
 * Depths and headers.
 * ------------------------------------------------------------------------- */

/* 24 bits, rows from the bottom, each padded to a whole word. */
TB_TEST(media_bitmap_bmp, reads_24_bits_from_the_bottom)
{
    static const uint8_t pixels[] = {255, 0, 0,   255, 255, 255, 0, 0,  /* blue, white */
                                     0,   0, 255, 0,   255, 0,   0, 0}; /* red, green */
    const bmp_spec spec = {.header = 40,
                           .width = 2,
                           .height = 2,
                           .bits = 24,
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 2, 2, four_colors);
}

TB_TEST(media_bitmap_bmp, reads_rows_from_the_top)
{
    static const uint8_t pixels[] = {0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0};
    const bmp_spec spec = {.header = 40,
                           .width = 2,
                           .height = -2,
                           .bits = 24,
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 2, 2, four_colors);
}

/* The last row may lack its padding: some writers leave it off. */
TB_TEST(media_bitmap_bmp, the_last_row_needs_no_padding)
{
    static const uint8_t pixels[] = {255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 255, 0};
    const bmp_spec spec = {.header = 40,
                           .width = 2,
                           .height = 2,
                           .bits = 24,
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 2, 2, four_colors);
}

/* 32 bits without masks: the fourth byte is alpha, unless every pixel's is
 * zero, when it held nothing and the image is opaque. */
TB_TEST(media_bitmap_bmp, reads_32_bits_with_alpha_or_without)
{
    static const uint8_t with_alpha[] = {10, 20, 30, 40, 50, 60, 70, 0};
    static const uint8_t without[] = {10, 20, 30, 0, 50, 60, 70, 0};
    static const uint8_t expected_with[] = {30, 20, 10, 40, 70, 60, 50, 0};
    static const uint8_t expected_without[] = {30, 20, 10, 255, 70, 60, 50, 255};
    bmp_spec spec = {.header = 40,
                     .width = 2,
                     .height = 1,
                     .bits = 32,
                     .pixels = with_alpha,
                     .pixel_bytes = sizeof(with_alpha)};
    expect_decoded(&spec, 2, 1, expected_with);
    spec.pixels = without;
    expect_decoded(&spec, 2, 1, expected_without);
}

/* Masks in the header of version 5, as the writer writes them, and in the
 * one of version 4, with alpha in the top byte. */
TB_TEST(media_bitmap_bmp, reads_32_bits_with_masks_in_the_header)
{
    static const uint8_t pixels[] = {10, 20, 30, 0, 50, 60, 70, 255};
    static const uint8_t expected[] = {30, 20, 10, 0, 70, 60, 50, 255};
    static const uint32_t headers[] = {56, 108, 124};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(headers); index++)
    {
        const bmp_spec spec = {.header = headers[index],
                               .width = 2,
                               .height = 1,
                               .bits = 32,
                               .compression = 3,
                               .masks = {0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u},
                               .pixels = pixels,
                               .pixel_bytes = sizeof(pixels)};
        TB_TEST_CONTEXT("header of %u bytes", headers[index]);
        expect_decoded(&spec, 2, 1, expected);
    }
}

/* A plain header with BI_BITFIELDS is followed by three masks, and no
 * alpha; BI_ALPHABITFIELDS by four. Masks may put channels anywhere. */
TB_TEST(media_bitmap_bmp, reads_masks_after_the_plain_header)
{
    static const uint8_t pixels[] = {0x11, 0x22, 0x33, 0x44};
    static const uint8_t opaque[] = {0x11, 0x22, 0x33, 255};
    static const uint8_t with_alpha[] = {0x11, 0x22, 0x33, 0x44};
    bmp_spec spec = {.header = 40,
                     .width = 1,
                     .height = 1,
                     .bits = 32,
                     .compression = 3,
                     .masks = {0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u},
                     .trailing_masks = 3,
                     .pixels = pixels,
                     .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 1, 1, opaque);
    spec.compression = 6;
    spec.trailing_masks = 4;
    expect_decoded(&spec, 1, 1, with_alpha);
    /* A channel without a mask is 0. */
    static const uint8_t no_green[] = {0x11, 0, 0x33, 255};
    spec.masks[1] = 0;
    spec.compression = 3;
    spec.trailing_masks = 3;
    expect_decoded(&spec, 1, 1, no_green);
    spec.masks[1] = 0x0000FF00u;
    /* Version 2 has the color masks and no alpha. */
    spec.header = 52;
    spec.compression = 3;
    spec.trailing_masks = 0;
    expect_decoded(&spec, 1, 1, opaque);
}

/* 16 bits: 5-5-5 without masks, 5-6-5 and 4-4-4-4 with them, each channel
 * widened to 8 bits with rounding, so that full is 255. */
TB_TEST(media_bitmap_bmp, reads_16_bits)
{
    /* 5-5-5: red full, green half (16 of 31), blue none; then white. */
    static const uint8_t pixels_555[] = {0x00, 0x7E, 0xFF, 0x7F};
    static const uint8_t expected_555[] = {255, 132, 0, 255, 255, 255, 255, 255};
    /* 5-6-5: red none, green full, blue full; then 1 of 31 red. */
    static const uint8_t pixels_565[] = {0xFF, 0x07, 0x00, 0x08};
    static const uint8_t expected_565[] = {0, 255, 255, 255, 8, 0, 0, 255};
    /* 4-4-4-4 with alpha in the top bits. */
    static const uint8_t pixels_4444[] = {0x0F, 0x8F, 0, 0};
    static const uint8_t expected_4444[] = {255, 0, 255, 136, 0, 0, 0, 0};
    bmp_spec spec = {.header = 40,
                     .width = 2,
                     .height = 1,
                     .bits = 16,
                     .pixels = pixels_555,
                     .pixel_bytes = sizeof(pixels_555)};
    expect_decoded(&spec, 2, 1, expected_555);
    spec.compression = 3;
    spec.trailing_masks = 3;
    spec.masks[0] = 0xF800u;
    spec.masks[1] = 0x07E0u;
    spec.masks[2] = 0x001Fu;
    spec.pixels = pixels_565;
    expect_decoded(&spec, 2, 1, expected_565);
    spec.header = 56;
    spec.trailing_masks = 0;
    spec.masks[0] = 0x0F00u;
    spec.masks[1] = 0x00F0u;
    spec.masks[2] = 0x000Fu;
    spec.masks[3] = 0xF000u;
    spec.pixels = pixels_4444;
    expect_decoded(&spec, 2, 1, expected_4444);
}

/* Palettes of 1, 4 and 8 bits, pixels packed from the high bits, and
 * rows of 9 pixels that cross bytes. */
TB_TEST(media_bitmap_bmp, reads_palettes_of_1_4_and_8_bits)
{
    static const uint8_t palette[] = {0, 0, 0, 0, 255, 255, 255, 0, 255, 0, 0, 0};
    /* 1 bit: 101010101, padded to a word. */
    static const uint8_t pixels_1[] = {0xAA, 0x80, 0, 0};
    /* 4 bits: 0 1 2 0 1 2 0 1 2. */
    static const uint8_t pixels_4[] = {0x01, 0x20, 0x12, 0x01, 0x20, 0, 0, 0};
    /* 8 bits: 2 1 0 2 1 0 2 1 0. */
    static const uint8_t pixels_8[] = {2, 1, 0, 2, 1, 0, 2, 1, 0, 0, 0, 0};
    static const uint8_t black[4] = {0, 0, 0, 255};
    static const uint8_t white[4] = {255, 255, 255, 255};
    static const uint8_t blue[4] = {0, 0, 255, 255};
    const uint8_t *const of_1[] = {white, black};
    const uint8_t *const of_4[] = {black, white, blue};
    const uint8_t *const of_8[] = {blue, white, black};
    uint8_t expected[9 * 4] = {0};
    bmp_spec spec = {.header = 40,
                     .width = 9,
                     .height = 1,
                     .bits = 1,
                     .palette = palette,
                     .palette_bytes = 8,
                     .pixels = pixels_1,
                     .pixel_bytes = sizeof(pixels_1)};

    for (size_t x = 0; x < 9; x++)
    {
        memcpy(expected + x * 4, of_1[x % 2], 4);
    }
    expect_decoded(&spec, 9, 1, expected);
    spec.bits = 4;
    spec.colors_used = 3;
    spec.palette_bytes = sizeof(palette);
    spec.pixels = pixels_4;
    spec.pixel_bytes = sizeof(pixels_4);
    for (size_t x = 0; x < 9; x++)
    {
        memcpy(expected + x * 4, of_4[x % 3], 4);
    }
    expect_decoded(&spec, 9, 1, expected);
    spec.bits = 8;
    spec.pixels = pixels_8;
    spec.pixel_bytes = sizeof(pixels_8);
    for (size_t x = 0; x < 9; x++)
    {
        memcpy(expected + x * 4, of_8[x % 3], 4);
    }
    expect_decoded(&spec, 9, 1, expected);
}

/* A palette that the header does not count holds what fits before the pixels. */
TB_TEST(media_bitmap_bmp, a_palette_holds_what_fits_before_the_pixels)
{
    static const uint8_t palette[] = {0, 0, 255, 0, 0, 255, 0, 0};
    static const uint8_t pixels[] = {0, 1, 0, 0};
    static const uint8_t expected[] = {255, 0, 0, 255, 0, 255, 0, 255};
    const bmp_spec spec = {.header = 40,
                           .width = 2,
                           .height = 1,
                           .bits = 8,
                           .palette = palette,
                           .palette_bytes = sizeof(palette),
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 2, 1, expected);
}

/* The core header of OS/2 1.x: 16-bit dimensions and a palette of three bytes a color. */
TB_TEST(media_bitmap_bmp, reads_the_core_header)
{
    static const uint8_t palette[] = {0, 0, 255, 0, 255, 0};
    static const uint8_t pixels_8[] = {1, 0, 0, 0, 0, 1, 0, 0};
    static const uint8_t pixels_24[] = {255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0};
    static const uint8_t expected_8[] = {255, 0,   0, 255, 0,   255, 0, 255,
                                         0,   255, 0, 255, 255, 0,   0, 255};
    bmp_spec spec = {.header = 12,
                     .width = 2,
                     .height = 2,
                     .bits = 8,
                     .palette = palette,
                     .palette_bytes = sizeof(palette),
                     .pixels = pixels_8,
                     .pixel_bytes = sizeof(pixels_8)};
    expect_decoded(&spec, 2, 2, expected_8);
    spec.bits = 24;
    spec.palette_bytes = 0;
    spec.pixels = pixels_24;
    spec.pixel_bytes = sizeof(pixels_24);
    expect_decoded(&spec, 2, 2, four_colors);
}

/* RLE8: runs, a stretch padded to a word, the end of a line, a move, and
 * the end of the image. Pixels no run reaches are transparent black. */
TB_TEST(media_bitmap_bmp, reads_rle8)
{
    static const uint8_t palette[] = {0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 0};
    static const uint8_t pixels[] = {
        3, 2,             /* bottom row: three red */
        0, 0,             /* end of line */
        0, 3, 1, 2, 1, 0, /* a stretch of three: white, red, white, padded */
        0, 1,             /* end of the image */
    };
    static const uint8_t expected[] = {
        255, 255, 255, 255, 255, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 0,
        255, 0,   0,   255, 255, 0, 0, 255, 255, 0,   0,   255, 0, 0, 0, 0,
    };
    const bmp_spec spec = {.header = 40,
                           .width = 4,
                           .height = 2,
                           .bits = 8,
                           .compression = 1,
                           .colors_used = 3,
                           .palette = palette,
                           .palette_bytes = sizeof(palette),
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 4, 2, expected);
}

/* A move skips pixels, and a file may end between commands without saying so. */
TB_TEST(media_bitmap_bmp, reads_rle8_moves_and_an_unmarked_end)
{
    static const uint8_t palette[] = {255, 0, 0, 0};
    static const uint8_t pixels[] = {0, 2, 1, 1, 1, 0};
    static const uint8_t expected[] = {0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0};
    const bmp_spec spec = {.header = 40,
                           .width = 2,
                           .height = 2,
                           .bits = 8,
                           .compression = 1,
                           .palette = palette,
                           .palette_bytes = sizeof(palette),
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 2, 2, expected);
}

/* RLE4: a run alternates its byte's two nibbles; an odd stretch is padded. */
TB_TEST(media_bitmap_bmp, reads_rle4)
{
    static const uint8_t palette[] = {0, 0, 0, 0, 255, 255, 255, 0};
    static const uint8_t pixels[] = {
        5, 0x10,             /* white black white black white */
        0, 0,                /* end of line */
        0, 3,    0x01, 0x10, /* a stretch of three: black white white, padded */
        0, 1,
    };
    static const uint8_t white[4] = {255, 255, 255, 255};
    static const uint8_t black[4] = {0, 0, 0, 255};
    static const uint8_t clear[4] = {0, 0, 0, 0};
    const uint8_t *const top[] = {black, white, white, clear, clear};
    const uint8_t *const bottom[] = {white, black, white, black, white};
    uint8_t expected[2 * 5 * 4] = {0};
    for (size_t x = 0; x < 5; x++)
    {
        memcpy(expected + x * 4, top[x], 4);
        memcpy(expected + 20 + x * 4, bottom[x], 4);
    }
    const bmp_spec spec = {.header = 40,
                           .width = 5,
                           .height = 2,
                           .bits = 4,
                           .compression = 2,
                           .colors_used = 2,
                           .palette = palette,
                           .palette_bytes = sizeof(palette),
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    expect_decoded(&spec, 5, 2, expected);
}

/* ---------------------------------------------------------------------------
 * Writing.
 * ------------------------------------------------------------------------- */

static void write_to_buffer(const tb_bitmap *bitmap, tb_bitmap_bmp_depth depth, tb_stream *sink)
{
    tb_bitmap_error error = {0};
    tb_stream_open_buffer(NULL, sink);
    TB_TEST_REQUIRE_UINT(tb_bitmap_write_bmp(bitmap, depth, sink, &error), ==, TB_BITMAP_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
}

static tb_bitmap *read_back(tb_stream *sink)
{
    size_t length = 0;
    const unsigned char *const bytes = tb_stream_buffer_bytes(sink, &length);
    tb_bitmap *bitmap = NULL;
    TB_TEST_REQUIRE_UINT(tb_bitmap_decode(bytes, length, NULL, NULL, &bitmap, NULL), ==,
                         TB_BITMAP_OK);
    return bitmap;
}

/* Random pixels of every width up to 9, alpha and all, read back exactly. */
TB_TEST(media_bitmap_bmp, 32_bits_round_trip_exactly)
{
    for (uint32_t width = 1; width <= 9; width++)
    {
        tb_bitmap *const original = tb_bitmap_create(width, 3, TB_BITMAP_RGBA8, NULL);
        tb_stream sink = {0};
        tb_test_random_bytes(tb_bitmap_pixels(original), (size_t)width * 3u * 4u);
        write_to_buffer(original, TB_BITMAP_BMP_32, &sink);
        size_t length = 0;
        (void)tb_stream_buffer_bytes(&sink, &length);
        TB_TEST_EXPECT_UINT(length, ==, 138u + (size_t)width * 3u * 4u);
        tb_bitmap *const copy = read_back(&sink);
        TB_TEST_CONTEXT("width %u", width);
        TB_TEST_EXPECT(tb_bitmap_equal(original, copy));
        tb_bitmap_destroy(copy);
        (void)tb_stream_close(&sink);
        tb_bitmap_destroy(original);
    }
}

/* 24 bits keep the colors, padded rows and all, and drop alpha. */
TB_TEST(media_bitmap_bmp, 24_bits_round_trip_without_alpha)
{
    for (uint32_t width = 1; width <= 5; width++)
    {
        tb_bitmap *const original = tb_bitmap_create(width, 2, TB_BITMAP_RGBA8, NULL);
        tb_stream sink = {0};
        tb_test_random_bytes(tb_bitmap_pixels(original), (size_t)width * 2u * 4u);
        write_to_buffer(original, TB_BITMAP_BMP_24, &sink);
        tb_bitmap *const copy = read_back(&sink);
        TB_TEST_CONTEXT("width %u", width);
        for (uint32_t y = 0; y < 2; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                tb_bitmap_color expected = tb_bitmap_get_pixel(original, x, y);
                const tb_bitmap_color actual = tb_bitmap_get_pixel(copy, x, y);
                expected.a = 255;
                TB_TEST_EXPECT_BYTES(&actual, &expected, sizeof(actual));
            }
        }
        tb_bitmap_destroy(copy);
        (void)tb_stream_close(&sink);
        tb_bitmap_destroy(original);
    }
}

/* What BMP files hold is straight: a premultiplied bitmap is written as its
 * straight colors, and gray as gray in each channel. */
TB_TEST(media_bitmap_bmp, writing_makes_pixels_straight)
{
    tb_bitmap *const premultiplied = tb_bitmap_create(2, 1, TB_BITMAP_BGRA8, NULL);
    tb_bitmap *const gray = tb_bitmap_create(1, 1, TB_BITMAP_GRAY8, NULL);
    tb_stream sink = {0};

    tb_bitmap_set_pixel(premultiplied, 0, 0, (tb_bitmap_color){255, 0, 0, 255});
    tb_bitmap_set_pixel(premultiplied, 1, 0, (tb_bitmap_color){255, 128, 0, 128});
    tb_bitmap_premultiply(premultiplied);
    write_to_buffer(premultiplied, TB_BITMAP_BMP_32, &sink);
    tb_bitmap *const copy = read_back(&sink);
    static const uint8_t expected[] = {255, 0, 0, 255, 255, 128, 0, 128};
    expect_pixels(copy, 2, 1, expected);
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&sink);

    tb_bitmap_fill(gray, (tb_bitmap_color){100, 100, 100, 255});
    write_to_buffer(gray, TB_BITMAP_BMP_24, &sink);
    tb_bitmap *const gray_copy = read_back(&sink);
    static const uint8_t expected_gray[] = {100, 100, 100, 255};
    expect_pixels(gray_copy, 1, 1, expected_gray);
    tb_bitmap_destroy(gray_copy);
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(gray);
    tb_bitmap_destroy(premultiplied);
}

/* The headers the writer writes: version 5 with masks and sRGB, or the plain one. */
TB_TEST(media_bitmap_bmp, the_writers_headers)
{
    tb_bitmap *const bitmap = tb_bitmap_create(3, 2, TB_BITMAP_RGBA8, NULL);
    tb_stream sink = {0};
    size_t length = 0;

    write_to_buffer(bitmap, TB_BITMAP_BMP_32, &sink);
    const unsigned char *bytes = tb_stream_buffer_bytes(&sink, &length);
    static const uint8_t v5[] = {'B', 'M', 162, 0, 0, 0, 0, 0, 0, 0, 138, 0, 0, 0, 124, 0, 0,  0,
                                 3,   0,   0,   0, 2, 0, 0, 0, 1, 0, 32,  0, 3, 0, 0,   0, 24, 0};
    TB_TEST_EXPECT_BYTES(bytes, v5, sizeof(v5));
    static const uint8_t masks[] = {0, 0, 255, 0, 0, 255, 0,   0,   255, 0,
                                    0, 0, 0,   0, 0, 255, 'B', 'G', 'R', 's'};
    TB_TEST_EXPECT_BYTES(bytes + 54, masks, sizeof(masks));
    (void)tb_stream_close(&sink);
    write_to_buffer(bitmap, TB_BITMAP_BMP_24, &sink);
    bytes = tb_stream_buffer_bytes(&sink, &length);
    TB_TEST_EXPECT_UINT(length, ==, 54u + 2u * 12u);
    static const uint8_t plain[] = {'B', 'M', 78, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0,  0,
                                    3,   0,   0,  0, 2, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0,  0, 24, 0};
    TB_TEST_EXPECT_BYTES(bytes, plain, sizeof(plain));
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(bitmap);
}

TB_TEST(media_bitmap_bmp, writing_fails_cleanly)
{
    tb_bitmap *const empty = tb_bitmap_create(0, 4, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const bitmap = tb_bitmap_create(2, 2, TB_BITMAP_RGBA8, NULL);
    tb_bitmap_error error = {0};
    tb_stream sink = {0};
    tb_stream read_only = {0};
    static const uint8_t nothing[1] = {0};

    tb_stream_open_buffer(NULL, &sink);
    TB_TEST_EXPECT_UINT(tb_bitmap_write_bmp(empty, TB_BITMAP_BMP_32, &sink, &error), ==,
                        TB_BITMAP_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==, "a bitmap of no pixels, which no BMP file holds");
    TB_TEST_EXPECT_UINT(tb_bitmap_write_bmp(empty, TB_BITMAP_BMP_24, &sink, NULL), ==,
                        TB_BITMAP_INVALID);
    tb_stream_open_memory(nothing, sizeof(nothing), NULL, &read_only);
    TB_TEST_EXPECT_UINT(tb_bitmap_write_bmp(bitmap, TB_BITMAP_BMP_32, &read_only, &error), ==,
                        TB_BITMAP_OUTPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the stream refused the headers");
    TB_TEST_EXPECT_STRING(tb_bitmap_result_name(error.result), ==, "output failed");
    (void)tb_stream_close(&read_only);
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(bitmap);
    tb_bitmap_destroy(empty);
}

/* A stream that takes `room` bytes, then refuses. */
typedef struct short_sink
{
    size_t room;
} short_sink;

static tb_stream_result short_sink_write(void *state, const void *bytes, size_t length,
                                         size_t *transferred)
{
    short_sink *const sink = state;
    (void)bytes;
    *transferred = 0;
    if (length > sink->room)
    {
        return TB_STREAM_NO_SPACE;
    }
    sink->room -= length;
    *transferred = length;
    return TB_STREAM_OK;
}

static tb_stream_result short_sink_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

TB_TEST(media_bitmap_bmp, a_sink_that_fills_fails_the_pixels)
{
    static const tb_stream_functions functions = {.write_bytes = short_sink_write,
                                                  .release = short_sink_release};
    short_sink state = {.room = 138};
    tb_stream sink = {.functions = &functions, .state = &state};
    tb_bitmap *const bitmap = tb_bitmap_create(2, 2, TB_BITMAP_RGBA8, NULL);
    tb_bitmap_error error = {0};

    TB_TEST_EXPECT_UINT(tb_bitmap_write_bmp(bitmap, TB_BITMAP_BMP_32, &sink, &error), ==,
                        TB_BITMAP_OUTPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the stream refused the pixels");
    tb_bitmap_destroy(bitmap);
}

/* ---------------------------------------------------------------------------
 * Damaged files.
 * ------------------------------------------------------------------------- */

/* A valid file to damage: 8 bits, a palette of two, 3 by 2. */
static void valid_palettized(bmp_file *file)
{
    static const uint8_t palette[] = {0, 0, 0, 0, 255, 255, 255, 0};
    static const uint8_t pixels[] = {0, 1, 0, 0, 1, 0, 1, 0};
    const bmp_spec spec = {.header = 40,
                           .width = 3,
                           .height = 2,
                           .bits = 8,
                           .colors_used = 2,
                           .palette = palette,
                           .palette_bytes = sizeof(palette),
                           .pixels = pixels,
                           .pixel_bytes = sizeof(pixels)};
    build(&spec, file);
}

static void expect_refused(const bmp_file *file, const tb_bitmap_decode_options *options,
                           tb_bitmap_result result, size_t offset, const char *reason)
{
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file->bytes, file->length, options, NULL, &bitmap, &error),
                        ==, result);
    TB_TEST_EXPECT_POINTER(bitmap, ==, NULL);
    TB_TEST_EXPECT_UINT(error.result, ==, result);
    TB_TEST_EXPECT_UINT(error.offset, ==, offset);
    TB_TEST_EXPECT_STRING(error.reason, ==, reason);
}

/* Each damage to a header is refused, at the byte at fault. */
TB_TEST(media_bitmap_bmp, damaged_headers_are_refused)
{
    static const struct
    {
        size_t at;      /* Where the damage is written. */
        uint32_t value; /* A 32-bit value written there; see `width`. */
        size_t width;   /* Bytes of the value: 2 or 4. */
        tb_bitmap_result result;
        size_t offset;
        const char *reason;
    } rows[] = {
        {14, 41, 4, TB_BITMAP_MALFORMED, 14, "a header of an unknown size"},
        {14, 64, 4, TB_BITMAP_UNSUPPORTED, 14, "an OS/2 2.x header"},
        {14, 16, 4, TB_BITMAP_UNSUPPORTED, 14, "an OS/2 2.x header"},
        {14, 124, 4, TB_BITMAP_MALFORMED, 68, "the header ends early"},
        {26, 2, 2, TB_BITMAP_MALFORMED, 26, "planes other than 1"},
        {18, 0, 4, TB_BITMAP_MALFORMED, 18, "a width of no pixels"},
        {18, 0xFFFFFFFFu, 4, TB_BITMAP_MALFORMED, 18, "a width of no pixels"},
        {22, 0, 4, TB_BITMAP_MALFORMED, 22, "a height of no pixels"},
        {22, 0x80000000u, 4, TB_BITMAP_MALFORMED, 22, "a height of no pixels"},
        {28, 7, 2, TB_BITMAP_MALFORMED, 28, "bits per pixel no BMP file has"},
        {30, 4, 4, TB_BITMAP_UNSUPPORTED, 30, "a BMP file holding a JPEG image"},
        {30, 5, 4, TB_BITMAP_UNSUPPORTED, 30, "a BMP file holding a PNG image"},
        {30, 11, 4, TB_BITMAP_UNSUPPORTED, 30, "CMYK pixels"},
        {30, 12, 4, TB_BITMAP_UNSUPPORTED, 30, "CMYK pixels"},
        {30, 13, 4, TB_BITMAP_UNSUPPORTED, 30, "CMYK pixels"},
        {30, 99, 4, TB_BITMAP_MALFORMED, 30, "an unknown compression"},
        {30, 2, 4, TB_BITMAP_MALFORMED, 30, "a compression the bits per pixel cannot have"},
        {30, 3, 4, TB_BITMAP_MALFORMED, 30, "a compression the bits per pixel cannot have"},
        {18, 20000, 4, TB_BITMAP_TOO_LARGE, 18, "dimensions over the limits"},
        {22, 10000, 4, TB_BITMAP_TOO_LARGE, 18, "dimensions over the limits"},
        {10, 20, 4, TB_BITMAP_MALFORMED, 10, "the pixels begin inside the headers"},
        {10, 5000, 4, TB_BITMAP_MALFORMED, 10, "the pixels begin past the end"},
        {46, 257, 4, TB_BITMAP_MALFORMED, 46, "a palette of more than 256 colors"},
        {10, 54, 4, TB_BITMAP_MALFORMED, 54, "no room for the palette"},
        {18, 5, 4, TB_BITMAP_MALFORMED, 70, "the pixels end early"},
    };
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        bmp_file file = {0};
        valid_palettized(&file);
        if (rows[row].width == 2)
        {
            put_u16(file.bytes + rows[row].at, rows[row].value);
        }
        else
        {
            put_u32(file.bytes + rows[row].at, rows[row].value);
        }
        if (rows[row].at == 22 && rows[row].value == 10000)
        {
            /* A tall image of 3 by 10000 is within the sides, not the pixels. */
            const tb_bitmap_decode_options options = {.max_pixels = 3 * 10000 - 1};
            TB_TEST_CONTEXT("row %zu", row);
            expect_refused(&file, &options, rows[row].result, rows[row].offset, rows[row].reason);
            continue;
        }
        if (rows[row].at == 14 && rows[row].value == 124)
        {
            file.length = 68;
        }
        TB_TEST_CONTEXT("row %zu", row);
        expect_refused(&file, NULL, rows[row].result, rows[row].offset, rows[row].reason);
    }
}

/* Compressed rows go from the bottom; compression and depth go together. */
TB_TEST(media_bitmap_bmp, compressions_that_cannot_be)
{
    bmp_file file = {0};
    valid_palettized(&file);
    put_u32(file.bytes + 30, 1);
    put_u32(file.bytes + 22, (uint32_t)-2);
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 22, "compressed rows stored from the top");
    put_u32(file.bytes + 22, 2);
    put_u16(file.bytes + 28, 24);
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 30,
                   "a compression the bits per pixel cannot have");
}

TB_TEST(media_bitmap_bmp, a_file_too_short_for_its_headers)
{
    bmp_file file = {0};
    valid_palettized(&file);
    file.length = 17;
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 17, "the file header ends early");
    file.length = 40;
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 40, "the header ends early");
}

TB_TEST(media_bitmap_bmp, a_pixel_past_its_palette)
{
    bmp_file file = {0};
    valid_palettized(&file);
    file.bytes[62 + 4] = 2;
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 66, "a pixel's index past the palette");
}

/* Masks that are out of the pixel's bits, scattered, or past the end. */
TB_TEST(media_bitmap_bmp, damaged_masks_are_refused)
{
    static const uint8_t pixels[] = {0, 0, 0, 0};
    bmp_spec spec = {.header = 40,
                     .width = 2,
                     .height = 1,
                     .bits = 16,
                     .compression = 3,
                     .masks = {0x10000u, 0x00F0u, 0x000Fu},
                     .trailing_masks = 3,
                     .pixels = pixels,
                     .pixel_bytes = sizeof(pixels)};
    bmp_file file = {0};

    build(&spec, &file);
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 54, "a mask outside the pixel's bits");
    spec.masks[0] = 0x0F00u;
    spec.masks[1] = 0x00A0u;
    build(&spec, &file);
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 58, "a mask whose bits are not together");
    spec.masks[1] = 0x00F0u;
    build(&spec, &file);
    file.length = 60;
    put_u32(file.bytes + 10, 60);
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 10, "the pixels begin inside the headers");
    spec.header = 52;
    spec.trailing_masks = 0;
    build(&spec, &file);
    file.length = 60;
    expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 60, "the header ends early");
}

/* Run-length damage: runs and moves past the edge, commands cut short,
 * and an index past the palette. */
TB_TEST(media_bitmap_bmp, damaged_runs_are_refused)
{
    static const uint8_t palette[] = {0, 0, 0, 0, 255, 255, 255, 0};
    static const struct
    {
        uint8_t pixels[8];
        size_t length;
        size_t offset; /* From the pixels' start. */
        const char *reason;
    } rows[] = {
        {{5, 1}, 2, 0, "a run past the edge of the image"},
        {{0, 2, 9, 0}, 4, 0, "a move past the edge of the image"},
        {{0, 2, 0, 9}, 4, 0, "a move past the edge of the image"},
        {{0, 0, 0, 0, 0, 0, 1, 1}, 8, 6, "a run past the edge of the image"},
        {{2, 1, 0}, 3, 3, "the compressed pixels end early"},
        {{0, 2, 1}, 3, 3, "the compressed pixels end early"},
        {{0, 4, 1, 1}, 4, 4, "the compressed pixels end early"},
        {{1, 2}, 2, 0, "a pixel's index past the palette"},
        {{0, 3, 1, 5, 1, 0}, 6, 3, "a pixel's index past the palette"},
    };
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        const bmp_spec spec = {.header = 40,
                               .width = 4,
                               .height = 2,
                               .bits = 8,
                               .compression = 1,
                               .colors_used = 2,
                               .palette = palette,
                               .palette_bytes = sizeof(palette),
                               .pixels = rows[row].pixels,
                               .pixel_bytes = rows[row].length};
        bmp_file file = {0};
        build(&spec, &file);
        TB_TEST_CONTEXT("row %zu", row);
        expect_refused(&file, NULL, TB_BITMAP_MALFORMED, 62 + rows[row].offset, rows[row].reason);
    }
}

/* Bytes of no format the engine reads, and options it cannot honor. */
TB_TEST(media_bitmap_bmp, unknown_formats_and_bad_options)
{
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a', 1, 0};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    const tb_bitmap_decode_options options = {.name = "logo.gif"};
    const tb_bitmap_decode_options too_wide = {.max_side = TB_BITMAP_MAX_DIMENSION + 1u};

    TB_TEST_EXPECT_UINT(tb_bitmap_decode(gif, sizeof(gif), &options, NULL, &bitmap, &error), ==,
                        TB_BITMAP_UNKNOWN_FORMAT);
    TB_TEST_EXPECT_STRING(error.message, ==, "logo.gif: bytes of no image format the engine reads");
    TB_TEST_EXPECT_UINT(error.offset, ==, 0);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(NULL, 0, NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_UNKNOWN_FORMAT);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(gif, sizeof(gif), &too_wide, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_INVALID);
    TB_TEST_EXPECT_POINTER(bitmap, ==, NULL);
}

/* The message names the file and the byte, as a log wants it. */
TB_TEST(media_bitmap_bmp, errors_say_where_and_why)
{
    bmp_file file = {0};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    const tb_bitmap_decode_options options = {.name = "hero.bmp"};

    valid_palettized(&file);
    put_u16(file.bytes + 26, 3);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, &options, NULL, &bitmap, &error),
                        ==, TB_BITMAP_MALFORMED);
    TB_TEST_EXPECT_STRING(error.message, ==, "hero.bmp: byte 26: planes other than 1");
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_MALFORMED);
    TB_TEST_EXPECT_STRING(error.message, ==, "byte 26: planes other than 1");
}

TB_TEST(media_bitmap_bmp, results_have_names)
{
    static const struct
    {
        tb_bitmap_result result;
        const char *name;
    } rows[] = {
        {TB_BITMAP_OK, "ok"},
        {TB_BITMAP_INVALID, "invalid argument"},
        {TB_BITMAP_NO_MEMORY, "out of memory"},
        {TB_BITMAP_TOO_LARGE, "too large"},
        {TB_BITMAP_UNKNOWN_FORMAT, "unknown format"},
        {TB_BITMAP_UNSUPPORTED, "unsupported"},
        {TB_BITMAP_MALFORMED, "malformed"},
        {TB_BITMAP_NOT_FOUND, "not found"},
        {TB_BITMAP_INPUT_FAILED, "input failed"},
        {TB_BITMAP_OUTPUT_FAILED, "output failed"},
        {(tb_bitmap_result)99, "unknown result"},
    };
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_EXPECT_STRING(tb_bitmap_result_name(rows[row].result), ==, rows[row].name);
    }
}

/* An allocator that refuses the pixels is NO_MEMORY, and nothing leaks. */
TB_TEST(media_bitmap_bmp, no_memory_for_the_pixels)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    bmp_file file = {0};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};

    valid_palettized(&file);
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_UINT(
        tb_bitmap_decode(file.bytes, file.length, NULL, &allocator, &bitmap, &error), ==,
        TB_BITMAP_NO_MEMORY);
    TB_TEST_EXPECT_STRING(error.message, ==, "no memory for the pixels");
    tb_test_allocator_stop_failing(&tracker);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    /* A failure after the pixels are made gives them back. */
    put_u16(file.bytes + 62 + 4, 7);
    TB_TEST_EXPECT_UINT(
        tb_bitmap_decode(file.bytes, file.length, NULL, &allocator, &bitmap, &error), ==,
        TB_BITMAP_MALFORMED);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

/* Every cut of valid files, and random damage to them, is decoded or
 * refused: never read past, never crashed, never leaked. The sanitizer
 * build is what proves it. */
TB_TEST(media_bitmap_bmp, random_damage_never_faults)
{
    static const uint8_t palette[] = {0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 255, 0};
    static const uint8_t rle[] = {3, 2, 0, 0, 0, 3, 1, 2, 1, 0, 1, 0, 0, 1};
    static const uint8_t rows_8[] = {0, 1, 2, 0, 2, 1, 0, 0};
    static const uint8_t rows_32[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const bmp_spec specs[] = {
        {.header = 40,
         .width = 4,
         .height = 2,
         .bits = 8,
         .compression = 1,
         .colors_used = 3,
         .palette = palette,
         .palette_bytes = sizeof(palette),
         .pixels = rle,
         .pixel_bytes = sizeof(rle)},
        {.header = 40,
         .width = 8,
         .height = 2,
         .bits = 4,
         .compression = 2,
         .colors_used = 3,
         .palette = palette,
         .palette_bytes = sizeof(palette),
         .pixels = rle,
         .pixel_bytes = sizeof(rle)},
        {.header = 12,
         .width = 3,
         .height = 2,
         .bits = 8,
         .palette = palette,
         .palette_bytes = 9,
         .pixels = rows_8,
         .pixel_bytes = sizeof(rows_8)},
        {.header = 124,
         .width = 2,
         .height = -2,
         .bits = 32,
         .compression = 3,
         .masks = {0xFF0000u, 0xFF00u, 0xFFu, 0xFF000000u},
         .pixels = rows_32,
         .pixel_bytes = sizeof(rows_32)},
        {.header = 40,
         .width = 4,
         .height = 2,
         .bits = 16,
         .compression = 6,
         .masks = {0xF00u, 0xF0u, 0xFu, 0xF000u},
         .trailing_masks = 4,
         .pixels = rows_32,
         .pixel_bytes = sizeof(rows_32)},
        {.header = 40,
         .width = 16,
         .height = 2,
         .bits = 1,
         .palette = palette,
         .palette_bytes = 8,
         .pixels = rows_8,
         .pixel_bytes = sizeof(rows_8)},
    };
    /* Small limits, so that damaged dimensions cost little. */
    const tb_bitmap_decode_options options = {.max_side = 64};
    tb_rng rng = {0};
    tb_rng_seed(&rng, 0xB17A9u);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(specs); index++)
    {
        bmp_file original = {0};
        build(&specs[index], &original);
        tb_bitmap *bitmap = NULL;
        TB_TEST_CONTEXT("file %zu", index);
        TB_TEST_REQUIRE_UINT(
            tb_bitmap_decode(original.bytes, original.length, NULL, NULL, &bitmap, NULL), ==,
            TB_BITMAP_OK);
        tb_bitmap_destroy(bitmap);
        for (size_t length = 0; length < original.length; length++)
        {
            bitmap = NULL;
            (void)tb_bitmap_decode(original.bytes, length, &options, NULL, &bitmap, NULL);
            tb_bitmap_destroy(bitmap);
        }
        for (size_t trial = 0; trial < 2000; trial++)
        {
            bmp_file damaged = original;
            const uint64_t changes = 1u + tb_rng_below(&rng, 4);
            for (uint64_t change = 0; change < changes; change++)
            {
                damaged.bytes[tb_rng_below(&rng, damaged.length)] = (uint8_t)tb_rng_next_u32(&rng);
            }
            bitmap = NULL;
            const tb_bitmap_result result =
                tb_bitmap_decode(damaged.bytes, damaged.length, &options, NULL, &bitmap, NULL);
            TB_TEST_EXPECT((result == TB_BITMAP_OK) == (bitmap != NULL));
            tb_bitmap_destroy(bitmap);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Streams and the VFS.
 * ------------------------------------------------------------------------- */

TB_TEST(media_bitmap_bmp, reads_from_a_stream)
{
    bmp_file file = {0};
    tb_stream stream = {0};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    const tb_bitmap_decode_options small = {.max_bytes = 20, .name = "small.bmp"};
    const tb_bitmap_decode_options too_wide = {.max_side = TB_BITMAP_MAX_DIMENSION + 1u};

    valid_palettized(&file);
    tb_stream_open_memory(file.bytes, file.length, NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_bitmap_read(&stream, NULL, NULL, &bitmap, &error), ==, TB_BITMAP_OK);
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    TB_TEST_EXPECT_UINT(info.width, ==, 3);
    tb_bitmap_destroy(bitmap);
    TB_TEST_REQUIRE_UINT(tb_stream_seek(&stream, 0, TB_STREAM_FROM_START, NULL), ==, TB_STREAM_OK);
    TB_TEST_EXPECT_UINT(tb_bitmap_read(&stream, &small, NULL, &bitmap, &error), ==,
                        TB_BITMAP_TOO_LARGE);
    TB_TEST_EXPECT_STRING(error.message, ==, "small.bmp: a file longer than the limit");
    TB_TEST_EXPECT_UINT(tb_bitmap_read(&stream, &too_wide, NULL, &bitmap, &error), ==,
                        TB_BITMAP_INVALID);
    (void)tb_stream_close(&stream);
}

/* A file of exactly the limit is read; streams longer than a first buffer
 * grow it. */
TB_TEST(media_bitmap_bmp, reads_a_long_stream)
{
    tb_bitmap *const original = tb_bitmap_create(200, 100, TB_BITMAP_RGBA8, NULL);
    tb_stream sink = {0};
    size_t length = 0;
    tb_stream stream = {0};
    tb_bitmap *copy = NULL;

    tb_test_random_bytes(tb_bitmap_pixels(original), 200u * 100u * 4u);
    write_to_buffer(original, TB_BITMAP_BMP_32, &sink);
    const unsigned char *const bytes = tb_stream_buffer_bytes(&sink, &length);
    const tb_bitmap_decode_options exact = {.max_bytes = length};
    tb_stream_open_memory(bytes, length, NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_bitmap_read(&stream, &exact, NULL, &copy, NULL), ==, TB_BITMAP_OK);
    TB_TEST_EXPECT(tb_bitmap_equal(original, copy));
    tb_bitmap_destroy(copy);
    (void)tb_stream_close(&stream);
    (void)tb_stream_close(&sink);
    tb_bitmap_destroy(original);
}

static tb_stream_result broken_read(void *state, void *bytes, size_t capacity, size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)capacity;
    *transferred = 0;
    return TB_STREAM_OS_ERROR;
}

TB_TEST(media_bitmap_bmp, a_stream_that_fails_to_read)
{
    static const tb_stream_functions functions = {.read_bytes = broken_read,
                                                  .release = short_sink_release};
    tb_stream stream = {.functions = &functions};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};

    TB_TEST_EXPECT_UINT(tb_bitmap_read(&stream, NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_INPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.reason, ==, tb_stream_result_name(TB_STREAM_OS_ERROR));
}

/* A source of the VFS with one good image, one damaged, and one it cannot read. */
static tb_vfs_result images_open(void *state, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream)
{
    const bmp_file *const files = state;
    if (tb_str_equal(name, "sprites/good.bmp") && length == 16)
    {
        tb_stream_open_memory(files[0].bytes, files[0].length, allocator, stream);
        return TB_VFS_OK;
    }
    if (tb_str_equal(name, "sprites/bad.bmp") && length == 15)
    {
        tb_stream_open_memory(files[1].bytes, files[1].length, allocator, stream);
        return TB_VFS_OK;
    }
    if (tb_str_equal(name, "locked.bmp"))
    {
        return TB_VFS_INPUT_FAILED;
    }
    return TB_VFS_NOT_FOUND;
}

static tb_vfs_result images_query(void *state, const char *name, size_t length, tb_vfs_info *info)
{
    (void)state;
    (void)name;
    (void)length;
    (void)info;
    return TB_VFS_NOT_FOUND;
}

static tb_vfs_result images_list(void *state, const char *directory, size_t length,
                                 tb_vfs_list_function *function, void *context)
{
    (void)state;
    (void)directory;
    (void)length;
    (void)function;
    (void)context;
    return TB_VFS_NOT_FOUND;
}

static void images_release(void *state)
{
    (void)state;
}

TB_TEST(media_bitmap_bmp, loads_through_a_vfs)
{
    static const tb_vfs_source_functions functions = {images_open, images_query, images_list,
                                                      images_release};
    bmp_file files[2] = {0};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};

    valid_palettized(&files[0]);
    valid_palettized(&files[1]);
    put_u16(files[1].bytes + 26, 4);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount(vfs, "", 0, (tb_vfs_source){&functions, files}, 0), ==,
                         TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(tb_bitmap_load(vfs, "sprites/good.bmp", NULL, NULL, &bitmap, &error), ==,
                         TB_BITMAP_OK);
    TB_TEST_EXPECT_STRING(error.message, ==, "");
    tb_bitmap_destroy(bitmap);
    TB_TEST_EXPECT_UINT(tb_bitmap_load(vfs, "sprites/bad.bmp", NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_MALFORMED);
    TB_TEST_EXPECT_STRING(error.message, ==, "sprites/bad.bmp: byte 26: planes other than 1");
    TB_TEST_EXPECT_UINT(tb_bitmap_load(vfs, "sprites/none.bmp", NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_NOT_FOUND);
    TB_TEST_EXPECT_STRING(error.message, ==, "sprites/none.bmp: no such file");
    TB_TEST_EXPECT_UINT(tb_bitmap_load(vfs, "../escape.bmp", NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_bitmap_load(vfs, "locked.bmp", NULL, NULL, &bitmap, &error), ==,
                        TB_BITMAP_INPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.message, ==, "locked.bmp: input failed");
    const tb_bitmap_decode_options too_wide = {.max_side = TB_BITMAP_MAX_DIMENSION + 1u};
    TB_TEST_EXPECT_UINT(tb_bitmap_load(vfs, "sprites/good.bmp", &too_wide, NULL, &bitmap, &error),
                        ==, TB_BITMAP_INVALID);
    TB_TEST_EXPECT_POINTER(bitmap, ==, NULL);
    tb_vfs_destroy(vfs);
}
