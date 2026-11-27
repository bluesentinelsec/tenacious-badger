/* Damaged PNG files: every way a chunk, the header, the palette,
 * transparency or the image data can be wrong, refused with the chunk and
 * the byte at fault; images whose dimensions the data cannot fill, refused
 * before their pixels are allocated; and damage at random, read without a
 * fault under the sanitizers. */
#include "badger/bitmap.h"
#include "badger/checksum.h"
#include "badger/deflate.h"
#include "badger/rng.h"
#include "png_builder.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

static void put_u32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

/* Rows of an RGBA8 image 3 wide, of no filter. */
static size_t rgba_rows(uint32_t height, uint8_t *rows)
{
    size_t used = 0;
    for (uint32_t y = 0; y < height; y++)
    {
        rows[used++] = 0;
        for (size_t index = 0; index < 12u; index++)
        {
            rows[used++] = (uint8_t)(y * 12u + index);
        }
    }
    return used;
}

/* A valid file: RGBA8, 3 by 2. */
static void valid_rgba(png_build *file)
{
    uint8_t rows[64] = {0};
    const size_t length = rgba_rows(2, rows);

    png_build_begin(file);
    png_build_header(file, 3, 2, 8, 6, 0);
    png_build_data(file, rows, length, 0);
    png_build_end(file);
}

/* The start of a palette file of 2 bits, 4 by 2: its header and palette. */
static void palette_start(png_build *file, size_t colors)
{
    static const uint8_t palette[12] = {0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255};

    png_build_begin(file);
    png_build_header(file, 4, 2, 2, 3, 0);
    png_build_chunk(file, "PLTE", palette, colors * 3u);
}

/* Its image data: indices 0 to 3 in each row, of no filter. */
static void palette_data(png_build *file)
{
    static const uint8_t rows[] = {0, 0x1Bu, 0, 0xE4u};
    png_build_data(file, rows, sizeof(rows), 0);
}

static void expect_refused(const png_build *file, const tb_bitmap_decode_options *options,
                           tb_bitmap_result result, const char *message)
{
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};

    TB_TEST_CONTEXT("%s", message);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file->bytes, file->length, options, NULL, &bitmap, &error),
                        ==, result);
    TB_TEST_EXPECT_POINTER(bitmap, ==, NULL);
    TB_TEST_EXPECT_STRING(error.message, ==, message);
}

static void expect_malformed(const png_build *file, const char *message)
{
    expect_refused(file, NULL, TB_BITMAP_MALFORMED, message);
}

/* Change a chunk's data, and give it its CRC again. */
static void change_byte(png_build *file, size_t at, uint8_t value)
{
    file->bytes[at] = value;
    png_build_fix_crcs(file);
}

TB_TEST(media_bitmap_png, the_valid_files_are_read)
{
    png_build file = {0};
    tb_bitmap *bitmap = NULL;

    valid_rgba(&file);
    TB_TEST_REQUIRE_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                         TB_BITMAP_OK);
    TB_TEST_EXPECT_UINT(tb_bitmap_get_pixel(bitmap, 2, 1).a, ==, 23);
    tb_bitmap_destroy(bitmap);
    palette_start(&file, 4);
    palette_data(&file);
    png_build_end(&file);
    TB_TEST_REQUIRE_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                         TB_BITMAP_OK);
    TB_TEST_EXPECT_UINT(tb_bitmap_get_pixel(bitmap, 3, 0).b, ==, 255);
    TB_TEST_EXPECT_UINT(tb_bitmap_get_pixel(bitmap, 0, 1).b, ==, 255);
    tb_bitmap_destroy(bitmap);
}

/* Chunks that are not whole, not named, or not as their CRC says. */
TB_TEST(media_bitmap_png, damaged_chunks_are_refused)
{
    png_build file = {0};

    png_build_begin(&file);
    expect_malformed(&file, "byte 8: the file ends before its IEND chunk");
    valid_rgba(&file);
    file.length -= 12u;
    expect_malformed(&file, "byte 79: the file ends before its IEND chunk");
    file.length += 11u;
    expect_malformed(&file, "IEND chunk at byte 79: the chunk runs past the end of the file");

    valid_rgba(&file);
    file.bytes[13] = '1';
    expect_malformed(&file, "byte 13: a chunk whose name is not four letters");
    valid_rgba(&file);
    put_u32(file.bytes + 8, 0x80000000u);
    expect_malformed(&file, "IHDR chunk at byte 8: a length of 2^31 bytes or more");
    valid_rgba(&file);
    put_u32(file.bytes + 8, 1000);
    expect_malformed(&file, "IHDR chunk at byte 8: the chunk runs past the end of the file");
    valid_rgba(&file);
    file.bytes[20] ^= 1u;
    expect_malformed(&file, "IHDR chunk at byte 29: a CRC that does not match the chunk");
    valid_rgba(&file);
    file.bytes[file.length - 1u] ^= 1u;
    expect_malformed(&file, "IEND chunk at byte 87: a CRC that does not match the chunk");
}

/* An image header that is missing, of the wrong length, or of a field no
 * PNG file has; and one over the limits. */
TB_TEST(media_bitmap_png, damaged_headers_are_refused)
{
    png_build file = {0};
    const size_t data = PNG_BUILD_IHDR_DATA;

    png_build_begin(&file);
    png_build_chunk(&file, "tEXt", "a", 1);
    expect_malformed(&file, "tEXt chunk at byte 8: a first chunk that is not IHDR");
    png_build_begin(&file);
    png_build_chunk(&file, "IHDR", "abcdefghijkl", 12);
    expect_malformed(&file, "IHDR chunk at byte 8: an IHDR chunk not 13 bytes long");

    static const struct
    {
        size_t at;
        uint8_t value;
        const char *message;
    } fields[] = {
        {0, 0x80, "IHDR chunk at byte 16: a width of 0, or of 2^31 or more"},
        {4, 0x80, "IHDR chunk at byte 20: a height of 0, or of 2^31 or more"},
        {8, 4, "IHDR chunk at byte 24: a bit depth the color type does not have"},
        {8, 3, "IHDR chunk at byte 24: a bit depth the color type does not have"},
        {9, 1, "IHDR chunk at byte 25: an unknown color type"},
        {9, 5, "IHDR chunk at byte 25: an unknown color type"},
        {9, 7, "IHDR chunk at byte 25: an unknown color type"},
        {10, 1, "IHDR chunk at byte 26: a compression method other than 0"},
        {11, 1, "IHDR chunk at byte 27: a filter method other than 0"},
        {12, 2, "IHDR chunk at byte 28: an unknown interlace method"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(fields); index++)
    {
        valid_rgba(&file);
        change_byte(&file, data + fields[index].at, fields[index].value);
        expect_malformed(&file, fields[index].message);
    }
    valid_rgba(&file);
    put_u32(file.bytes + data, 0);
    png_build_fix_crcs(&file);
    expect_malformed(&file, "IHDR chunk at byte 16: a width of 0, or of 2^31 or more");
    valid_rgba(&file);
    put_u32(file.bytes + data + 4u, 0);
    png_build_fix_crcs(&file);
    expect_malformed(&file, "IHDR chunk at byte 20: a height of 0, or of 2^31 or more");

    const tb_bitmap_decode_options narrow = {.max_side = 2, .name = "wide.png"};
    const tb_bitmap_decode_options few = {.max_pixels = 5};
    valid_rgba(&file);
    expect_refused(&file, &narrow, TB_BITMAP_TOO_LARGE,
                   "wide.png: IHDR chunk at byte 16: dimensions over the limits");
    expect_refused(&file, &few, TB_BITMAP_TOO_LARGE,
                   "IHDR chunk at byte 16: dimensions over the limits");
}

/* Chunks out of order, twice, or where their color type has none. */
TB_TEST(media_bitmap_png, chunks_out_of_place_are_refused)
{
    png_build file = {0};
    uint8_t rows[64] = {0};
    const size_t length = rgba_rows(2, rows);

    valid_rgba(&file);
    file.length -= 12u;
    png_build_header(&file, 3, 2, 8, 6, 0);
    expect_malformed(&file, "IHDR chunk at byte 79: a second IHDR chunk");

    valid_rgba(&file);
    file.length = png_build_find(&file, "IEND");
    png_build_chunk(&file, "IEND", "a", 1);
    expect_malformed(&file, "IEND chunk at byte 79: an IEND chunk with data");

    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_data(&file, rows, length, 7);
    const size_t second = png_build_find(&file, "IDAT") + 19u;
    file.length = second;
    png_build_chunk(&file, "tEXt", "a", 1);
    png_build_data(&file, rows, length, 0);
    expect_malformed(&file, "IDAT chunk at byte 65: IDAT chunks that are not one after another");

    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_chunk(&file, "CUST", "a", 1);
    expect_refused(&file, NULL, TB_BITMAP_UNSUPPORTED,
                   "CUST chunk at byte 33: a critical chunk the engine does not know");

    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_end(&file);
    expect_malformed(&file, "IEND chunk at byte 33: no image data before IEND");
}

TB_TEST(media_bitmap_png, damaged_palettes_are_refused)
{
    png_build file = {0};
    static const uint8_t colors[257 * 3] = {0};

    palette_start(&file, 4);
    png_build_chunk(&file, "PLTE", colors, 6);
    expect_malformed(&file, "PLTE chunk at byte 57: a second PLTE chunk");
    palette_start(&file, 4);
    palette_data(&file);
    png_build_chunk(&file, "PLTE", colors, 6);
    expect_malformed(&file, "PLTE chunk at byte 81: a PLTE chunk after the image data");
    palette_start(&file, 4);
    png_build_chunk(&file, "tRNS", colors, 1);
    png_build_chunk(&file, "PLTE", colors, 6);
    expect_malformed(&file, "PLTE chunk at byte 70: a PLTE chunk after the tRNS chunk");
    palette_start(&file, 0);
    expect_malformed(&file, "PLTE chunk at byte 33: a palette not of 1 to 256 colors of 3 bytes");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 8, 3, 0);
    png_build_chunk(&file, "PLTE", colors, 4);
    expect_malformed(&file, "PLTE chunk at byte 33: a palette not of 1 to 256 colors of 3 bytes");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 8, 3, 0);
    png_build_chunk(&file, "PLTE", colors, sizeof(colors));
    expect_malformed(&file, "PLTE chunk at byte 33: a palette not of 1 to 256 colors of 3 bytes");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 2, 3, 0);
    png_build_chunk(&file, "PLTE", colors, 15);
    expect_malformed(&file, "PLTE chunk at byte 33: more colors than the bit depth addresses");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 8, 0, 0);
    png_build_chunk(&file, "PLTE", colors, 6);
    expect_malformed(&file, "PLTE chunk at byte 33: a PLTE chunk in a gray image");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 2, 3, 0);
    palette_data(&file);
    png_build_end(&file);
    expect_malformed(&file, "IDAT chunk at byte 33: a palette image without PLTE");
    /* A palette of three colors, and a pixel of the fourth. */
    palette_start(&file, 3);
    palette_data(&file);
    png_build_end(&file);
    expect_malformed(&file, "IDAT chunk at byte 54: a pixel past the palette");
}

TB_TEST(media_bitmap_png, damaged_transparency_is_refused)
{
    png_build file = {0};
    static const uint8_t alphas[8] = {0};

    palette_start(&file, 4);
    png_build_chunk(&file, "tRNS", alphas, 2);
    png_build_chunk(&file, "tRNS", alphas, 2);
    expect_malformed(&file, "tRNS chunk at byte 71: a second tRNS chunk");
    palette_start(&file, 4);
    palette_data(&file);
    png_build_chunk(&file, "tRNS", alphas, 2);
    expect_malformed(&file, "tRNS chunk at byte 81: a tRNS chunk after the image data");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 2, 3, 0);
    png_build_chunk(&file, "tRNS", alphas, 2);
    expect_malformed(&file, "tRNS chunk at byte 33: a tRNS chunk before PLTE");
    palette_start(&file, 4);
    png_build_chunk(&file, "tRNS", alphas, 5);
    expect_malformed(&file, "tRNS chunk at byte 57: more alphas than the palette has colors");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 8, 0, 0);
    png_build_chunk(&file, "tRNS", alphas, 6);
    expect_malformed(&file,
                     "tRNS chunk at byte 33: a tRNS chunk of the wrong length for its color type");
    png_build_begin(&file);
    png_build_header(&file, 4, 2, 8, 2, 0);
    png_build_chunk(&file, "tRNS", alphas, 2);
    expect_malformed(&file,
                     "tRNS chunk at byte 33: a tRNS chunk of the wrong length for its color type");

    /* An image with alpha needs none, and its tRNS is ignored. */
    uint8_t rows[64] = {0};
    const size_t length = rgba_rows(2, rows);
    tb_bitmap *bitmap = NULL;
    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_chunk(&file, "tRNS", alphas, 3);
    png_build_data(&file, rows, length, 0);
    png_build_end(&file);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_OK);
    tb_bitmap_destroy(bitmap);
}

/* Image data that is not zlib, or that holds more or fewer rows than the
 * image, or rows of a filter type there is not. */
TB_TEST(media_bitmap_png, damaged_image_data_is_refused)
{
    png_build file = {0};
    uint8_t rows[64] = {0};
    const size_t length = rgba_rows(2, rows);

    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_chunk(&file, "IDAT", "\x78\x9C\xFF", 3);
    png_build_end(&file);
    expect_malformed(&file, "IDAT chunk at byte 33: image data that is not a zlib stream");
    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_chunk(&file, "IDAT", "\x78\xBB\x00\x00\x00\x01", 6);
    png_build_end(&file);
    expect_malformed(
        &file, "IDAT chunk at byte 33: image data with a preset dictionary, which PNG forbids");

    valid_rgba(&file);
    const size_t data = png_build_find(&file, "IDAT");
    const uint32_t idat_length = (uint32_t)file.bytes[data + 3u];
    change_byte(&file, data + 8u + idat_length - 1u, file.bytes[data + 8u + idat_length - 1u] ^ 1u);
    expect_malformed(&file, "IDAT chunk at byte 33: image data whose Adler-32 does not match");

    /* The stream cut before its Adler-32: every row, and no end. */
    valid_rgba(&file);
    put_u32(file.bytes + data, idat_length - 4u);
    file.length = data + 8u + idat_length - 4u + 4u;
    png_build_fix_crcs(&file);
    png_build_end(&file);
    expect_malformed(&file, "IEND chunk at byte 75: image data whose zlib stream does not end");

    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_data(&file, rows, length - 13u, 0);
    png_build_end(&file);
    expect_malformed(&file, "IEND chunk at byte 66: image data that ends before the image");

    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_data(&file, rows, length + 1u, 0);
    png_build_end(&file);
    expect_malformed(&file, "IDAT chunk at byte 33: image data past the image's last row");

    rows[13] = 5;
    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_data(&file, rows, length, 0);
    png_build_end(&file);
    expect_malformed(&file, "IDAT chunk at byte 33: a row of an unknown filter type");

    /* Data after the zlib stream's end, in its chunk or the next, is ignored. */
    rows[13] = 0;
    tb_bitmap *bitmap = NULL;
    png_build_begin(&file);
    png_build_header(&file, 3, 2, 8, 6, 0);
    png_build_data(&file, rows, length, 0);
    png_build_chunk(&file, "IDAT", "junk", 4);
    png_build_end(&file);
    TB_TEST_EXPECT_UINT(tb_bitmap_decode(file.bytes, file.length, NULL, NULL, &bitmap, NULL), ==,
                        TB_BITMAP_OK);
    tb_bitmap_destroy(bitmap);
}

/* Dimensions within the limits, of 64 Mi pixels, whose data is a few
 * bytes: DEFLATE makes no more than 1032 bytes of one, so the file is
 * refused before 256 MiB of pixels are allocated. */
TB_TEST(media_bitmap_png, a_decompression_bomb_allocates_nothing)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    png_build file = {0};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};
    static const uint8_t zeros[4096] = {0};

    png_build_begin(&file);
    png_build_header(&file, 8192, 8192, 8, 6, 0);
    png_build_data(&file, zeros, sizeof(zeros), 0);
    png_build_end(&file);
    TB_TEST_EXPECT_UINT(
        tb_bitmap_decode(file.bytes, file.length, NULL, &allocator, &bitmap, &error), ==,
        TB_BITMAP_MALFORMED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "too little image data for the image's dimensions");
    TB_TEST_EXPECT_UINT(tracker.allocation_count, ==, 0);
}

/* An allocator that refuses the pixels, or the rows, is NO_MEMORY, and
 * nothing leaks. */
TB_TEST(media_bitmap_png, no_memory_for_the_pixels)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    png_build file = {0};
    tb_bitmap *bitmap = NULL;
    tb_bitmap_error error = {0};

    valid_rgba(&file);
    /* The pixels are the first block a decode allocates, and the rows the third. */
    for (size_t successes = 0; successes <= 2u; successes += 2u)
    {
        tb_test_allocator_fail_after(&tracker, successes);
        TB_TEST_EXPECT_UINT(
            tb_bitmap_decode(file.bytes, file.length, NULL, &allocator, &bitmap, &error), ==,
            TB_BITMAP_NO_MEMORY);
        tb_test_allocator_stop_failing(&tracker);
        TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    }
    TB_TEST_EXPECT_STRING(error.message, ==, "no memory for the rows");
    TB_TEST_EXPECT_UINT(
        tb_bitmap_decode(file.bytes, file.length, NULL, &allocator, &bitmap, &error), ==,
        TB_BITMAP_OK);
    tb_bitmap_destroy(bitmap);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

/* Samples of a pixel of each color type. */
static uint32_t samples_of(uint32_t color)
{
    static const uint32_t samples[7] = {1, 0, 3, 1, 2, 0, 4};
    return samples[color];
}

/* Random filtered rows of an image 7 by 6, of every pass when interlaced. */
static size_t random_rows(tb_rng *rng, uint32_t color, uint32_t depth, bool interlaced,
                          uint8_t *rows)
{
    static const uint32_t first_column[7] = {0, 4, 0, 2, 0, 1, 0};
    static const uint32_t first_row[7] = {0, 0, 4, 0, 2, 0, 1};
    static const uint32_t across[7] = {8, 8, 4, 4, 2, 2, 1};
    static const uint32_t down[7] = {8, 8, 8, 4, 4, 2, 2};
    const uint32_t width = 7;
    const uint32_t height = 6;
    size_t used = 0;

    for (size_t pass = 0; pass < (interlaced ? 7u : 1u); pass++)
    {
        uint32_t pass_width = width;
        uint32_t pass_height = height;
        if (interlaced)
        {
            pass_width = (width - first_column[pass] + across[pass] - 1u) / across[pass];
            pass_height = (height - first_row[pass] + down[pass] - 1u) / down[pass];
        }
        const size_t row_bytes = ((size_t)pass_width * samples_of(color) * depth + 7u) / 8u;
        for (uint32_t y = 0; y < pass_height; y++)
        {
            rows[used++] = (uint8_t)(y % 5u);
            for (size_t byte = 0; byte < row_bytes; byte++)
            {
                const uint8_t value = (uint8_t)tb_rng_next_u32(rng);
                /* Indices of a palette of 2 bits stay below its 3 colors
                 * when unfiltered: the filter of a palette row is none. */
                rows[used++] = color == 3u ? (uint8_t)(value & 0x55u) : value;
            }
            if (color == 3u)
            {
                rows[used - row_bytes - 1u] = 0;
            }
        }
    }
    return used;
}

/* An image of every color type, interlaced and not, to damage. */
static void damage_subjects(png_build *files, size_t *count)
{
    static const uint32_t layouts[][3] = {{0, 1, 0}, {0, 16, 1}, {2, 8, 0}, {3, 2, 1},
                                          {4, 8, 1}, {6, 16, 0}, {6, 8, 1}};
    static const uint8_t palette[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    static const uint8_t alphas[] = {0, 128};
    tb_rng rng = {0};
    tb_rng_seed(&rng, 0x5EEDu);

    *count = TB_TEST_COUNT_OF(layouts);
    for (size_t index = 0; index < *count; index++)
    {
        const uint32_t color = layouts[index][0];
        const uint32_t depth = layouts[index][1];
        uint8_t rows[2048] = {0};
        const size_t used = random_rows(&rng, color, depth, layouts[index][2] == 1u, rows);
        png_build *const file = &files[index];
        png_build_begin(file);
        png_build_header(file, 7, 6, depth, color, layouts[index][2]);
        if (color == 3u)
        {
            png_build_chunk(file, "PLTE", palette, sizeof(palette));
            png_build_chunk(file, "tRNS", alphas, sizeof(alphas));
        }
        if (color == 0u)
        {
            png_build_chunk(file, "tRNS", "\0\1", 2);
        }
        png_build_chunk(file, "gAMA", "\0\0\xB1\x8F", 4);
        png_build_data(file, rows, used, 23);
        png_build_end(file);
    }
}

/* Every cut of the files, and random damage to them whose CRCs are made
 * right again, so that it reaches the header, the palette and the image
 * data, is decoded or refused: never read past, never crashed, never
 * leaked. The sanitizer build is what proves it. */
TB_TEST(media_bitmap_png, random_damage_never_faults)
{
    static png_build files[8];
    size_t count = 0;
    const tb_bitmap_decode_options options = {.max_side = 64};
    tb_rng rng = {0};
    size_t decoded = 0;

    damage_subjects(files, &count);
    tb_rng_seed(&rng, 0xD06u);
    for (size_t index = 0; index < count; index++)
    {
        const png_build *const original = &files[index];
        tb_bitmap *bitmap = NULL;
        TB_TEST_CONTEXT("file %zu", index);
        TB_TEST_REQUIRE_UINT(
            tb_bitmap_decode(original->bytes, original->length, NULL, NULL, &bitmap, NULL), ==,
            TB_BITMAP_OK);
        tb_bitmap_destroy(bitmap);
        for (size_t length = 0; length < original->length; length++)
        {
            bitmap = NULL;
            (void)tb_bitmap_decode(original->bytes, length, &options, NULL, &bitmap, NULL);
            tb_bitmap_destroy(bitmap);
        }
        for (size_t trial = 0; trial < 3000; trial++)
        {
            png_build damaged = *original;
            const uint64_t changes = 1u + tb_rng_below(&rng, 3);
            for (uint64_t change = 0; change < changes; change++)
            {
                damaged.bytes[8u + tb_rng_below(&rng, damaged.length - 8u)] =
                    (uint8_t)tb_rng_next_u32(&rng);
            }
            if (trial % 8u != 0)
            {
                png_build_fix_crcs(&damaged);
            }
            bitmap = NULL;
            const tb_bitmap_result result =
                tb_bitmap_decode(damaged.bytes, damaged.length, &options, NULL, &bitmap, NULL);
            TB_TEST_EXPECT((result == TB_BITMAP_OK) == (bitmap != NULL));
            decoded += result == TB_BITMAP_OK ? 1u : 0u;
            tb_bitmap_destroy(bitmap);
        }
    }
    /* Damage to pixels' bytes alone decodes: some trials reach the end. */
    TB_TEST_EXPECT_UINT(decoded, >, 0);
}
