/* TGA pixel layouts, metadata, packet boundaries and malformed files. */
#include "badger/bitmap.h"
#include "badger/rng.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

/* Room for every file a case builds. */
#define TGA_CAPACITY 4096u

typedef struct tga_spec
{
    uint32_t type; /* Image type: 1, 2, 3, or 9, 10, 11. */
    uint32_t width;
    uint32_t height;
    uint32_t bits;
    uint32_t descriptor;
    const uint8_t *id;
    size_t id_bytes;
    uint32_t map_first;
    uint32_t map_length;
    uint32_t map_bits;
    const uint8_t *map;
    size_t map_bytes;
    const uint8_t *pixels;
    size_t pixel_bytes;
    int attributes; /* The extension area's attributes type; -1 for no footer, -2 for a
                       footer and no extension area. */
} tga_spec;

typedef struct tga_file
{
    uint8_t bytes[TGA_CAPACITY];
    size_t length;
} tga_file;

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

static void append(tga_file *file, const void *bytes, size_t length)
{
    TB_TEST_REQUIRE_UINT(file->length + length, <=, TGA_CAPACITY);
    if (length > 0)
    {
        memcpy(file->bytes + file->length, bytes, length);
    }
    file->length += length;
}

static void build(const tga_spec *spec, tga_file *file)
{
    uint8_t header[18] = {0};

    memset(file, 0, sizeof(*file));
    header[0] = (uint8_t)spec->id_bytes;
    header[1] = spec->map_length > 0 ? 1u : 0u;
    header[2] = (uint8_t)spec->type;
    put_u16(header + 3, spec->map_first);
    put_u16(header + 5, spec->map_length);
    header[7] = (uint8_t)spec->map_bits;
    put_u16(header + 12, spec->width);
    put_u16(header + 14, spec->height);
    header[16] = (uint8_t)spec->bits;
    header[17] = (uint8_t)spec->descriptor;
    append(file, header, sizeof(header));
    append(file, spec->id, spec->id_bytes);
    append(file, spec->map, spec->map_bytes);
    append(file, spec->pixels, spec->pixel_bytes);
    if (spec->attributes == -1)
    {
        return;
    }
    uint8_t footer[26] = {0};
    if (spec->attributes >= 0)
    {
        uint8_t extension[495] = {0};
        put_u16(extension, 495);
        extension[494] = (uint8_t)spec->attributes;
        put_u32(footer, (uint32_t)file->length);
        append(file, extension, sizeof(extension));
    }
    memcpy(footer + 8, "TRUEVISION-XFILE.", 18);
    append(file, footer, sizeof(footer));
}

static tb_bitmap *decode_ok(const tga_file *file)
{
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    const tb_bitmap_result result =
        tb_bitmap_decode(file->bytes, file->length, NULL, NULL, &bitmap, &error);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_BITMAP_OK);
    return bitmap;
}

/* Compare a decoded bitmap with rows of RGBA bytes from the top. */
static void expect_pixels(const tb_bitmap *bitmap, uint32_t width, uint32_t height,
                          const uint8_t *expected)
{
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    TB_TEST_REQUIRE_UINT(info.width, ==, width);
    TB_TEST_REQUIRE_UINT(info.height, ==, height);
    TB_TEST_EXPECT_UINT(info.format, ==, TB_BITMAP_RGBA8);
    TB_TEST_EXPECT(!info.premultiplied);
    for (uint32_t y = 0; y < height; y++)
    {
        TB_TEST_CONTEXT("row %u", y);
        TB_TEST_EXPECT_BYTES(tb_bitmap_const_row(bitmap, y), expected + (size_t)y * width * 4u,
                             (size_t)width * 4u);
    }
}

static void expect_decoded(const tga_spec *spec, const uint8_t *expected)
{
    tga_file file = {0};
    build(spec, &file);
    tb_bitmap *const bitmap = decode_ok(&file);
    expect_pixels(bitmap, spec->width, spec->height, expected);
    tb_bitmap_destroy(bitmap);
}

/* Four pixels, BGRA, in the file's order. */
static const uint8_t bgra_4[] = {1, 2, 3, 255, 4, 5, 6, 128, 7, 8, 9, 0, 10, 11, 12, 64};

/* The corner bits: rows from the bottom by default, or the top; columns
 * from the left, or the right. */
TB_TEST(media_bitmap_tga, reads_32_bits_from_every_corner)
{
    /* RGBA from BGRA: red is the third byte. */
    static const uint8_t expected[4][16] = {
        {9, 8, 7, 0, 12, 11, 10, 64, 3, 2, 1, 255, 6, 5, 4, 128},  /* bottom left */
        {12, 11, 10, 64, 9, 8, 7, 0, 6, 5, 4, 128, 3, 2, 1, 255},  /* bottom right */
        {3, 2, 1, 255, 6, 5, 4, 128, 9, 8, 7, 0, 12, 11, 10, 64},  /* top left */
        {6, 5, 4, 128, 3, 2, 1, 255, 12, 11, 10, 64, 9, 8, 7, 0}}; /* top right */
    tga_spec spec = {.type = 2,
                     .width = 2,
                     .height = 2,
                     .bits = 32,
                     .descriptor = 8,
                     .pixels = bgra_4,
                     .pixel_bytes = sizeof(bgra_4),
                     .attributes = -1};

    expect_decoded(&spec, expected[0]);
    spec.descriptor = 8u | 0x10u;
    expect_decoded(&spec, expected[1]);
    spec.descriptor = 8u | 0x20u;
    expect_decoded(&spec, expected[2]);
    spec.descriptor = 8u | 0x30u;
    expect_decoded(&spec, expected[3]);
}
TB_TEST(media_bitmap_tga, reads_color_maps_and_packet_types)
{
    const uint8_t indices[] = {0x81, 7, 1, 8, 7};
    static const uint8_t expected[] = {255, 0,   0, 255, 255, 0, 0, 255,
                                       0,   255, 0, 255, 255, 0, 0, 255};
    static const uint8_t map16[] = {0, 0xFC, 0xE0, 0x83};
    static const uint8_t map24[] = {0, 0, 255, 0, 255, 0};
    static const uint8_t map32[] = {0, 0, 255, 255, 0, 255, 0, 255};
    tga_spec spec = {.type = 9,
                     .width = 4,
                     .height = 1,
                     .bits = 8,
                     .descriptor = 1,
                     .map_first = 7,
                     .map_length = 2,
                     .map_bits = 15,
                     .map = map16,
                     .map_bytes = sizeof(map16),
                     .pixels = indices,
                     .pixel_bytes = sizeof(indices),
                     .attributes = -1};
    expect_decoded(&spec, expected);
    spec.map_bits = 16;
    expect_decoded(&spec, expected);
    spec.map_bits = 24;
    spec.map = map24;
    spec.map_bytes = sizeof(map24);
    expect_decoded(&spec, expected);
    spec.map_bits = 32;
    spec.map = map32;
    spec.map_bytes = sizeof(map32);
    expect_decoded(&spec, expected);
}

TB_TEST(media_bitmap_tga, reads_gray_and_packed_color)
{
    static const uint8_t gray[] = {71, 29};
    static const uint8_t gray8[] = {71, 71, 71, 255};
    static const uint8_t gray16[] = {71, 71, 71, 29};
    tga_spec spec = {.type = 3,
                     .width = 1,
                     .height = 1,
                     .bits = 8,
                     .pixels = gray,
                     .pixel_bytes = 1,
                     .attributes = -1};
    expect_decoded(&spec, gray8);
    spec.bits = 16;
    spec.descriptor = 8;
    spec.pixel_bytes = 2;
    expect_decoded(&spec, gray16);
    static const uint8_t green[] = {0xE0, 0x03};
    static const uint8_t transparent[] = {0, 255, 0, 0};
    static const uint8_t opaque[] = {0, 255, 0, 255};
    spec.type = 2;
    spec.pixels = green;
    spec.descriptor = 1;
    expect_decoded(&spec, transparent);
    spec.bits = 15;
    expect_decoded(&spec, opaque);
}

TB_TEST(media_bitmap_tga, uses_declared_alpha_before_guessing)
{
    static const uint8_t pixel[] = {0, 0, 32, 64};
    static const uint8_t straight[] = {32, 0, 0, 64};
    static const uint8_t opaque[] = {32, 0, 0, 255};
    static const uint8_t unpremultiplied[] = {128, 0, 0, 64};
    tga_spec spec = {.type = 2,
                     .width = 1,
                     .height = 1,
                     .bits = 32,
                     .pixels = pixel,
                     .pixel_bytes = 4,
                     .attributes = -2};
    expect_decoded(&spec, straight);
    spec.attributes = 4;
    expect_decoded(&spec, unpremultiplied);
    spec.attributes = 1;
    spec.descriptor = 8;
    expect_decoded(&spec, opaque);
    static const uint8_t zero_alpha[] = {0, 0, 32, 0};
    spec.pixels = zero_alpha;
    spec.attributes = -1;
    spec.descriptor = 0;
    expect_decoded(&spec, opaque);
}

TB_TEST(media_bitmap_tga, round_trips_runs_at_the_packet_limit)
{
    tb_bitmap *const bitmap = tb_bitmap_create(260, 2, TB_BITMAP_RGBA8, NULL);
    for (uint32_t y = 0; y < 2; y++)
    {
        for (uint32_t x = 0; x < 260; x++)
        {
            const uint8_t value = x < 129u ? 42u : (uint8_t)x;
            tb_bitmap_set_pixel(bitmap, x, y, (tb_bitmap_color){value, 11, (uint8_t)y, value});
        }
    }
    for (size_t mode = 0; mode < 2; mode++)
    {
        tb_stream sink = {0};
        tb_stream_open_buffer(NULL, &sink);
        TB_TEST_REQUIRE_UINT(tb_bitmap_write_tga(bitmap, mode != 0, &sink, NULL), ==, TB_BITMAP_OK);
        size_t length = 0;
        const uint8_t *const bytes = tb_stream_buffer_bytes(&sink, &length);
        TB_TEST_EXPECT_UINT(bytes[2], ==, mode == 0 ? 2u : 10u);
        tb_bitmap *decoded = NULL;
        TB_TEST_REQUIRE_UINT(tb_bitmap_decode(bytes, length, NULL, NULL, &decoded, NULL), ==,
                             TB_BITMAP_OK);
        for (uint32_t y = 0; y < 2; y++)
        {
            TB_TEST_EXPECT_BYTES(tb_bitmap_const_row(decoded, y), tb_bitmap_const_row(bitmap, y),
                                 260u * 4u);
        }
        tb_bitmap_destroy(decoded);
        (void)tb_stream_close(&sink);
    }
    tb_bitmap_destroy(bitmap);
}

TB_TEST(media_bitmap_tga, refuses_truncation_bad_packets_and_metadata_overlap)
{
    const uint8_t pixel[] = {1, 2, 3};
    tga_spec spec = {.type = 2,
                     .width = 2,
                     .height = 1,
                     .bits = 24,
                     .pixels = pixel,
                     .pixel_bytes = 3,
                     .attributes = -2};
    tga_file file = {0};
    tb_bitmap *bitmap = NULL;
    build(&spec, &file);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_MALFORMED);
    TB_TEST_EXPECT(bitmap == NULL);
    spec.width = 1;
    spec.attributes = 3;
    build(&spec, &file);
    put_u32(file.bytes + file.length - 26u, 1);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_MALFORMED);
    const uint8_t packet[] = {0x82, 33};
    spec = (tga_spec){.type = 11,
                      .width = 2,
                      .height = 1,
                      .bits = 8,
                      .pixels = packet,
                      .pixel_bytes = sizeof(packet),
                      .attributes = -1};
    build(&spec, &file);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_MALFORMED);
    const tb_bitmap_decode_options limits = {.max_pixels = 1};
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, &limits, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_TOO_LARGE);
    file.bytes[18] = 0x81;
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_test_allocator_fail_after(&tracker, 0);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, &allocator, &bitmap, NULL),
                        ==, TB_BITMAP_NO_MEMORY);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
}

TB_TEST(media_bitmap_tga, damaged_files_respect_decode_limits)
{
    tga_file original = {0};
    const tga_spec spec = {.type = 2,
                           .width = 2,
                           .height = 2,
                           .bits = 32,
                           .descriptor = 8,
                           .pixels = bgra_4,
                           .pixel_bytes = sizeof(bgra_4),
                           .attributes = 3};
    build(&spec, &original);
    tb_rng rng = {0};
    tb_rng_seed(&rng, 358);
    const tb_bitmap_decode_options options = {.max_pixels = 64};
    for (size_t trial = 0; trial < 500u; trial++)
    {
        tga_file file = original;
        const size_t at = (size_t)tb_rng_below(&rng, file.length);
        file.bytes[at] ^= (uint8_t)tb_rng_next_u32(&rng);
        tb_bitmap *bitmap = NULL;
        const tb_bitmap_result result =
            tb_bitmap_decode(file.bytes, file.length, &options, NULL, &bitmap, NULL);
        TB_TEST_EXPECT((result == TB_BITMAP_OK) == (bitmap != NULL));
        tb_bitmap_destroy(bitmap);
    }
}
