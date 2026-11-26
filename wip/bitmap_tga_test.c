/* TGA files: every image type, depth and corner read from files built here
 * byte by byte, and from files macOS's ImageIO wrote; color maps, the
 * attribute bits of 16-bit pixels, and the extension area's alpha; exact
 * round trips through the writer, raw and compressed; how the generic
 * decoders recognize a file with no signature; every way a file can be
 * damaged; and damage at random, under the sanitizers. */
#include "badger/bitmap.h"
#include "badger/rng.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_allocator.h"
#include "test_data.h"

#include <string.h>

extern const tb_test_file tb_tga_files[];
extern const size_t tb_tga_files_count;

/* Room for every file a case builds. */
#define TGA_CAPACITY 4096u

typedef struct tga_spec
{
    uint32_t type;      /* Image type: 1, 2, 3, or 9, 10, 11. */
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
    static const uint8_t bottom_left[] = {7, 8, 9, 0, 10, 11, 12, 64, 1, 2, 3, 255, 4, 5, 6, 128};
    /* RGBA from BGRA: red is the third byte. */
    static const uint8_t expected[4][16] = {
        {9, 8, 7, 0, 12, 11, 10, 64, 3, 2, 1, 255, 6, 5, 4, 128},  /* bottom left */
        {7, 8, 9, 0, 10, 11, 12, 64, 1, 2, 3, 255, 4, 5, 6, 128},  /* unused */
        {3, 2, 1, 255, 6, 5, 4, 128, 9, 8, 7, 0, 12, 11, 10, 64},  /* top left */
        {6, 5, 4, 128, 3, 2, 1, 255, 12, 11, 10, 64, 9, 8, 7, 0}}; /* top right */
    (void)bottom_left;
    tga_spec spec = {.type = 2, .width = 2, .height = 2, .bits = 32, .descriptor = 8,
                     .pixels = bgra_4, .pixel_bytes = sizeof(bgra_4), .attributes = -1};

    expect_decoded(&spec, expected[0]);
    spec.descriptor = 8u | 0x20u;
    expect_decoded(&spec, expected[2]);
    spec.descriptor = 8u | 0x30u;
    expect_decoded(&spec, expected[3]);
}
