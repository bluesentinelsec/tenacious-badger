/* Bitmaps in memory: making them every way, reading and changing pixels in
 * every format, clipping at every edge, blending in both conventions of
 * alpha, and that every block comes from, and goes back to, the allocator
 * a bitmap was made with. */
#include "badger/assert.h"
#include "badger/bitmap.h"
#include "tb_test.h"
#include "test_allocator.h"

#include <string.h>

static const tb_bitmap_color red = {255, 0, 0, 255};
static const tb_bitmap_color green = {0, 255, 0, 255};
static const tb_bitmap_color blue = {0, 0, 255, 255};
static const tb_bitmap_color white = {255, 255, 255, 255};
static const tb_bitmap_color clear = {0, 0, 0, 0};

static bool same_color(tb_bitmap_color a, tb_bitmap_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

#define EXPECT_COLOR(actual, expected)                                                             \
    do                                                                                             \
    {                                                                                              \
        const tb_bitmap_color expect_color_actual = (actual);                                      \
        const tb_bitmap_color expect_color_expected = (expected);                                  \
        TB_TEST_CONTEXT("(%u, %u, %u, %u), expected (%u, %u, %u, %u)",                             \
                        (unsigned)expect_color_actual.r, (unsigned)expect_color_actual.g,          \
                        (unsigned)expect_color_actual.b, (unsigned)expect_color_actual.a,          \
                        (unsigned)expect_color_expected.r, (unsigned)expect_color_expected.g,      \
                        (unsigned)expect_color_expected.b, (unsigned)expect_color_expected.a);     \
        TB_TEST_EXPECT(same_color(expect_color_actual, expect_color_expected));                    \
    } while (0)

static tb_bitmap_info info_of(const tb_bitmap *bitmap)
{
    tb_bitmap_info info = {0};
    tb_bitmap_info_of(bitmap, &info);
    return info;
}

/* A bitmap whose pixel (x, y) is (x, y, x + y, 255): every pixel differs. */
static tb_bitmap *numbered(uint32_t width, uint32_t height, tb_bitmap_format format)
{
    tb_bitmap *const bitmap = tb_bitmap_create(width, height, format, NULL);
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            tb_bitmap_set_pixel(bitmap, x, y,
                                (tb_bitmap_color){(uint8_t)x, (uint8_t)y, (uint8_t)(x + y), 255});
        }
    }
    return bitmap;
}

TB_TEST(media_bitmap, formats_have_sizes_and_names)
{
    TB_TEST_EXPECT_UINT(tb_bitmap_format_bytes(TB_BITMAP_RGBA8), ==, 4);
    TB_TEST_EXPECT_UINT(tb_bitmap_format_bytes(TB_BITMAP_BGRA8), ==, 4);
    TB_TEST_EXPECT_UINT(tb_bitmap_format_bytes(TB_BITMAP_GRAY8), ==, 1);
    TB_TEST_EXPECT_STRING(tb_bitmap_format_name(TB_BITMAP_RGBA8), ==, "RGBA8");
    TB_TEST_EXPECT_STRING(tb_bitmap_format_name(TB_BITMAP_BGRA8), ==, "BGRA8");
    TB_TEST_EXPECT_STRING(tb_bitmap_format_name(TB_BITMAP_GRAY8), ==, "GRAY8");
    TB_TEST_EXPECT_STRING(tb_bitmap_format_name(TB_BITMAP_FORMAT_COUNT), ==, "unknown format");
}

/* A new bitmap is transparent black, its rows packed: what tb_image_create() takes. */
TB_TEST(media_bitmap, a_new_bitmap_is_zero_and_packed)
{
    tb_bitmap *const bitmap = tb_bitmap_create(3, 2, TB_BITMAP_RGBA8, NULL);
    const tb_bitmap_info info = info_of(bitmap);

    TB_TEST_EXPECT_UINT(info.width, ==, 3);
    TB_TEST_EXPECT_UINT(info.height, ==, 2);
    TB_TEST_EXPECT_UINT(info.format, ==, TB_BITMAP_RGBA8);
    TB_TEST_EXPECT_UINT(info.stride, ==, 12);
    TB_TEST_EXPECT_UINT(info.size, ==, 24);
    TB_TEST_EXPECT(info.owns_pixels);
    TB_TEST_EXPECT(!info.premultiplied);
    for (size_t index = 0; index < info.size; index++)
    {
        TB_TEST_EXPECT_UINT(tb_bitmap_const_pixels(bitmap)[index], ==, 0);
    }
    TB_TEST_EXPECT_POINTER(tb_bitmap_row(bitmap, 1), ==, tb_bitmap_pixels(bitmap) + 12);
    TB_TEST_EXPECT_POINTER(tb_bitmap_const_row(bitmap, 1), ==, tb_bitmap_const_pixels(bitmap) + 12);
    tb_bitmap_destroy(bitmap);
}

TB_TEST(media_bitmap, a_bitmap_may_have_no_pixels)
{
    static const uint32_t sizes[][2] = {{0, 0}, {0, 5}, {5, 0}};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(sizes); index++)
    {
        tb_bitmap *const bitmap =
            tb_bitmap_create(sizes[index][0], sizes[index][1], TB_BITMAP_GRAY8, NULL);
        TB_TEST_CONTEXT("%u by %u", sizes[index][0], sizes[index][1]);
        TB_TEST_EXPECT_UINT(info_of(bitmap).size, ==, 0);
        TB_TEST_EXPECT_POINTER(tb_bitmap_pixels(bitmap), ==, NULL);
        tb_bitmap_fill(bitmap, white);
        tb_bitmap_flip_vertical(bitmap);
        tb_bitmap_flip_horizontal(bitmap);
        tb_bitmap *const copy = tb_bitmap_duplicate(bitmap, NULL);
        TB_TEST_EXPECT(tb_bitmap_equal(bitmap, copy));
        tb_bitmap_destroy(copy);
        tb_bitmap_destroy(bitmap);
    }
    tb_bitmap_destroy(NULL);
}

/* Pixels given with a stride wider than a row are copied, packed; the
 * caller's memory is not kept. */
TB_TEST(media_bitmap, pixels_are_copied_from_the_caller)
{
    uint8_t pixels[20] = {1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99, 9, 10, 11, 12, 13, 14, 15, 16};
    tb_bitmap *const bitmap = tb_bitmap_create_from(pixels, 20, 2, 2, 12, TB_BITMAP_RGBA8, NULL);

    TB_TEST_EXPECT_UINT(info_of(bitmap).stride, ==, 8);
    pixels[0] = 50;
    EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, 0, 0), ((tb_bitmap_color){1, 2, 3, 4}));
    EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, 1, 1), ((tb_bitmap_color){13, 14, 15, 16}));
    tb_bitmap_destroy(bitmap);
}

/* A wrapped bitmap changes the caller's memory, and destroying it leaves
 * that memory alone: only the bitmap's own block goes back. */
TB_TEST(media_bitmap, a_wrapped_bitmap_borrows_the_callers_pixels)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    uint8_t pixels[3 * 5] = {0};
    tb_bitmap *const bitmap =
        tb_bitmap_wrap(pixels, sizeof(pixels), 2, 3, 5, TB_BITMAP_GRAY8, &allocator);

    TB_TEST_EXPECT(!info_of(bitmap).owns_pixels);
    TB_TEST_EXPECT_UINT(info_of(bitmap).size, ==, 12);
    tb_bitmap_fill(bitmap, white);
    static const uint8_t expected[3 * 5] = {255, 255, 0,   0,   0, 255, 255, 0,
                                            0,   0,   255, 255, 0, 0,   0};
    TB_TEST_EXPECT_BYTES(pixels, expected, sizeof(expected));
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 1);
    tb_bitmap_destroy(bitmap);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");

    tb_bitmap *const empty = tb_bitmap_wrap(NULL, 0, 0, 0, 0, TB_BITMAP_RGBA8, NULL);
    TB_TEST_EXPECT_POINTER(tb_bitmap_const_pixels(empty), ==, NULL);
    tb_bitmap_destroy(empty);
}

/* A region is a window on its parent: filling it fills the parent's cell. */
TB_TEST(media_bitmap, a_region_borrows_its_parents_pixels)
{
    tb_bitmap *const sheet = tb_bitmap_create(4, 4, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const cell = tb_bitmap_wrap_region(sheet, (tb_bitmap_rect){2, 1, 5, 2}, NULL);
    const tb_bitmap_info cell_info = info_of(cell);

    TB_TEST_EXPECT_UINT(cell_info.width, ==, 2);
    TB_TEST_EXPECT_UINT(cell_info.height, ==, 2);
    TB_TEST_EXPECT_UINT(cell_info.stride, ==, 16);
    TB_TEST_EXPECT_UINT(cell_info.size, ==, 24);
    tb_bitmap_fill(cell, red);
    for (uint32_t y = 0; y < 4; y++)
    {
        for (uint32_t x = 0; x < 4; x++)
        {
            const bool inside = x >= 2 && y >= 1 && y <= 2;
            TB_TEST_CONTEXT("(%u, %u)", x, y);
            EXPECT_COLOR(tb_bitmap_get_pixel(sheet, x, y), inside ? red : clear);
        }
    }
    tb_bitmap *const outside = tb_bitmap_wrap_region(sheet, (tb_bitmap_rect){-9, 0, 3, 3}, NULL);
    TB_TEST_EXPECT_UINT(info_of(outside).width, ==, 0);
    TB_TEST_EXPECT_POINTER(tb_bitmap_pixels(outside), ==, NULL);
    tb_bitmap_destroy(outside);
    tb_bitmap_destroy(cell);
    tb_bitmap_destroy(sheet);
}

TB_TEST(media_bitmap, copies_of_a_bitmap_and_of_a_rectangle)
{
    tb_bitmap *const source = numbered(5, 4, TB_BITMAP_BGRA8);
    tb_bitmap *const copy = tb_bitmap_duplicate(source, NULL);
    tb_bitmap *const part = tb_bitmap_copy_rect(source, (tb_bitmap_rect){3, -1, 4, 3}, NULL);
    tb_bitmap *const none = tb_bitmap_copy_rect(source, (tb_bitmap_rect){5, 0, 2, 2}, NULL);

    TB_TEST_EXPECT(tb_bitmap_equal(source, copy));
    TB_TEST_EXPECT_UINT(info_of(part).width, ==, 2);
    TB_TEST_EXPECT_UINT(info_of(part).height, ==, 2);
    EXPECT_COLOR(tb_bitmap_get_pixel(part, 0, 0), tb_bitmap_get_pixel(source, 3, 0));
    EXPECT_COLOR(tb_bitmap_get_pixel(part, 1, 1), tb_bitmap_get_pixel(source, 4, 1));
    TB_TEST_EXPECT_UINT(info_of(none).size, ==, 0);
    tb_bitmap_destroy(none);
    tb_bitmap_destroy(part);
    tb_bitmap_destroy(copy);
    tb_bitmap_destroy(source);
}

/* Bitmaps are equal by their pixels, not by how their rows are laid out. */
TB_TEST(media_bitmap, equality_ignores_the_stride)
{
    tb_bitmap *const source = numbered(3, 3, TB_BITMAP_RGBA8);
    tb_bitmap *const wide = tb_bitmap_create(5, 3, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const region = tb_bitmap_wrap_region(wide, (tb_bitmap_rect){1, 0, 3, 3}, NULL);

    tb_bitmap_blit(region, 0, 0, source, NULL, TB_BITMAP_BLEND_NONE);
    TB_TEST_EXPECT(tb_bitmap_equal(source, region));
    tb_bitmap_set_pixel(region, 2, 2, white);
    TB_TEST_EXPECT(!tb_bitmap_equal(source, region));
    TB_TEST_EXPECT(!tb_bitmap_equal(source, wide));
    tb_bitmap *const gray = tb_bitmap_convert(source, TB_BITMAP_GRAY8, NULL);
    tb_bitmap *const gray_too = tb_bitmap_create(3, 3, TB_BITMAP_GRAY8, NULL);
    TB_TEST_EXPECT(!tb_bitmap_equal(source, gray));
    tb_bitmap_blit(gray_too, 0, 0, gray, NULL, TB_BITMAP_BLEND_NONE);
    TB_TEST_EXPECT(tb_bitmap_equal(gray, gray_too));
    tb_bitmap *const premultiplied = tb_bitmap_duplicate(source, NULL);
    tb_bitmap_set_premultiplied(premultiplied, true);
    TB_TEST_EXPECT(!tb_bitmap_equal(source, premultiplied));
    tb_bitmap_destroy(premultiplied);
    tb_bitmap_destroy(gray_too);
    tb_bitmap_destroy(gray);
    tb_bitmap_destroy(region);
    tb_bitmap_destroy(wide);
    tb_bitmap_destroy(source);
}

/* Each format stores a pixel its own way, and reads it back as the same color. */
TB_TEST(media_bitmap, pixels_are_stored_in_their_formats_order)
{
    const tb_bitmap_color color = {10, 20, 30, 40};
    tb_bitmap *const rgba = tb_bitmap_create(1, 1, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const bgra = tb_bitmap_create(1, 1, TB_BITMAP_BGRA8, NULL);
    tb_bitmap *const gray = tb_bitmap_create(1, 1, TB_BITMAP_GRAY8, NULL);
    static const uint8_t rgba_bytes[4] = {10, 20, 30, 40};
    static const uint8_t bgra_bytes[4] = {30, 20, 10, 40};

    tb_bitmap_set_pixel(rgba, 0, 0, color);
    tb_bitmap_set_pixel(bgra, 0, 0, color);
    TB_TEST_EXPECT_BYTES(tb_bitmap_const_pixels(rgba), rgba_bytes, 4);
    TB_TEST_EXPECT_BYTES(tb_bitmap_const_pixels(bgra), bgra_bytes, 4);
    EXPECT_COLOR(tb_bitmap_get_pixel(rgba, 0, 0), color);
    EXPECT_COLOR(tb_bitmap_get_pixel(bgra, 0, 0), color);
    /* Gray is the luminance of Rec. 709; white stays white, and alpha goes. */
    tb_bitmap_set_pixel(gray, 0, 0, red);
    TB_TEST_EXPECT_UINT(tb_bitmap_const_pixels(gray)[0], ==, 54);
    tb_bitmap_set_pixel(gray, 0, 0, green);
    TB_TEST_EXPECT_UINT(tb_bitmap_const_pixels(gray)[0], ==, 182);
    tb_bitmap_set_pixel(gray, 0, 0, blue);
    TB_TEST_EXPECT_UINT(tb_bitmap_const_pixels(gray)[0], ==, 19);
    tb_bitmap_set_pixel(gray, 0, 0, (tb_bitmap_color){255, 255, 255, 0});
    EXPECT_COLOR(tb_bitmap_get_pixel(gray, 0, 0), white);
    tb_bitmap_destroy(gray);
    tb_bitmap_destroy(bgra);
    tb_bitmap_destroy(rgba);
}

/* A rectangle is clipped at every edge, and one wholly outside changes nothing. */
TB_TEST(media_bitmap, rectangles_are_clipped_to_the_bitmap)
{
    static const struct
    {
        tb_bitmap_rect rect;
        bool inside;
        tb_bitmap_rect clipped;
    } rows[] = {
        {{0, 0, 4, 3}, true, {0, 0, 4, 3}},
        {{-2, -1, 4, 3}, true, {0, 0, 2, 2}},
        {{3, 2, 9, 9}, true, {3, 2, 1, 1}},
        {{4, 0, 1, 1}, false, {0, 0, 0, 0}},
        {{0, 3, 1, 1}, false, {0, 0, 0, 0}},
        {{-5, 0, 5, 1}, false, {0, 0, 0, 0}},
        {{1, 1, 0, 1}, false, {0, 0, 0, 0}},
        {{INT32_MIN, INT32_MIN, UINT32_MAX, UINT32_MAX}, true, {0, 0, 4, 3}},
        {{INT32_MAX, INT32_MAX, UINT32_MAX, UINT32_MAX}, false, {0, 0, 0, 0}},
    };
    tb_bitmap *const bitmap = tb_bitmap_create(4, 3, TB_BITMAP_GRAY8, NULL);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_bitmap_rect clipped = {1, 1, 1, 1};
        TB_TEST_CONTEXT("row %zu", row);
        TB_TEST_EXPECT(tb_bitmap_clip(bitmap, rows[row].rect, &clipped) == rows[row].inside);
        TB_TEST_EXPECT_INT(clipped.x, ==, rows[row].clipped.x);
        TB_TEST_EXPECT_INT(clipped.y, ==, rows[row].clipped.y);
        TB_TEST_EXPECT_UINT(clipped.width, ==, rows[row].clipped.width);
        TB_TEST_EXPECT_UINT(clipped.height, ==, rows[row].clipped.height);
    }
    tb_bitmap_fill_rect(bitmap, (tb_bitmap_rect){-1, 1, 3, 9}, white);
    static const uint8_t expected[12] = {0, 0, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0};
    TB_TEST_EXPECT_BYTES(tb_bitmap_const_pixels(bitmap), expected, sizeof(expected));
    tb_bitmap_fill_rect(bitmap, (tb_bitmap_rect){4, 0, 2, 2}, white);
    TB_TEST_EXPECT_BYTES(tb_bitmap_const_pixels(bitmap), expected, sizeof(expected));
    tb_bitmap_destroy(bitmap);
}

TB_TEST(media_bitmap, filling_sets_every_pixel_of_any_format)
{
    static const tb_bitmap_format formats[] = {TB_BITMAP_RGBA8, TB_BITMAP_BGRA8, TB_BITMAP_GRAY8};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(formats); index++)
    {
        tb_bitmap *const bitmap = tb_bitmap_create(3, 2, formats[index], NULL);
        tb_bitmap_fill(bitmap, white);
        TB_TEST_CONTEXT("%s", tb_bitmap_format_name(formats[index]));
        for (uint32_t y = 0; y < 2; y++)
        {
            for (uint32_t x = 0; x < 3; x++)
            {
                EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, x, y), white);
            }
        }
        tb_bitmap_destroy(bitmap);
    }
}

/* A blit lands where it is put, clipped against both bitmaps, whichever
 * side the source rectangle or the destination runs off. */
TB_TEST(media_bitmap, a_blit_is_clipped_to_both_bitmaps)
{
    static const struct
    {
        int32_t x;
        int32_t y;
        tb_bitmap_rect source;
    } rows[] = {
        {0, 0, {0, 0, 4, 4}},          {-1, -2, {0, 0, 4, 4}},
        {2, 3, {0, 0, 4, 4}},          {1, 1, {-1, -1, 3, 3}},
        {0, 0, {2, 2, 9, 9}},          {-3, 0, {0, 0, 2, 2}},
        {5, 0, {0, 0, 4, 4}},          {0, 0, {4, 0, 2, 2}},
        {INT32_MAX, 0, {-5, 0, 9, 1}}, {INT32_MIN, INT32_MIN, {0, 0, 4, 4}},
    };
    tb_bitmap *const source = numbered(4, 4, TB_BITMAP_RGBA8);
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        tb_bitmap *const destination = tb_bitmap_create(5, 5, TB_BITMAP_RGBA8, NULL);
        tb_bitmap_blit(destination, rows[row].x, rows[row].y, source, &rows[row].source,
                       TB_BITMAP_BLEND_NONE);
        /* Every pixel of the destination is the source's pixel that a blit
         * without clipping would put there, or untouched. */
        for (int32_t y = 0; y < 5; y++)
        {
            for (int32_t x = 0; x < 5; x++)
            {
                const int64_t source_x = (int64_t)x - rows[row].x + rows[row].source.x;
                const int64_t source_y = (int64_t)y - rows[row].y + rows[row].source.y;
                const bool in_rect =
                    source_x >= rows[row].source.x && source_y >= rows[row].source.y &&
                    source_x < rows[row].source.x + (int64_t)rows[row].source.width &&
                    source_y < rows[row].source.y + (int64_t)rows[row].source.height;
                const bool in_source =
                    source_x >= 0 && source_y >= 0 && source_x < 4 && source_y < 4;
                const tb_bitmap_color expected =
                    in_rect && in_source
                        ? tb_bitmap_get_pixel(source, (uint32_t)source_x, (uint32_t)source_y)
                        : clear;
                TB_TEST_CONTEXT("row %zu at (%d, %d)", row, x, y);
                EXPECT_COLOR(tb_bitmap_get_pixel(destination, (uint32_t)x, (uint32_t)y), expected);
            }
        }
        tb_bitmap_destroy(destination);
    }
    tb_bitmap_destroy(source);
}

/* Without a rectangle, all of the source is blitted, converting its format. */
TB_TEST(media_bitmap, a_blit_converts_formats)
{
    tb_bitmap *const source = numbered(3, 2, TB_BITMAP_BGRA8);
    tb_bitmap *const destination = tb_bitmap_create(3, 2, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const gray = tb_bitmap_create(3, 2, TB_BITMAP_GRAY8, NULL);

    tb_bitmap_blit(destination, 0, 0, source, NULL, TB_BITMAP_BLEND_NONE);
    tb_bitmap *const expected = tb_bitmap_convert(source, TB_BITMAP_RGBA8, NULL);
    TB_TEST_EXPECT(tb_bitmap_equal(destination, expected));
    tb_bitmap_fill(gray, white);
    tb_bitmap_blit(destination, 0, 0, gray, NULL, TB_BITMAP_BLEND_ALPHA);
    EXPECT_COLOR(tb_bitmap_get_pixel(destination, 2, 1), white);
    tb_bitmap_destroy(expected);
    tb_bitmap_destroy(gray);
    tb_bitmap_destroy(destination);
    tb_bitmap_destroy(source);
}

/* Source over destination, worked out on premultiplied colors: half
 * transparent red over opaque blue is half of each, opaque. */
TB_TEST(media_bitmap, alpha_blending_is_source_over)
{
    tb_bitmap *const source = tb_bitmap_create(1, 1, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const straight = tb_bitmap_create(1, 1, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const premultiplied = tb_bitmap_create(1, 1, TB_BITMAP_BGRA8, NULL);
    tb_bitmap *const nothing = tb_bitmap_create(1, 1, TB_BITMAP_RGBA8, NULL);

    tb_bitmap_set_pixel(source, 0, 0, (tb_bitmap_color){255, 0, 0, 128});
    tb_bitmap_fill(straight, blue);
    tb_bitmap_fill(premultiplied, blue);
    tb_bitmap_premultiply(premultiplied);
    tb_bitmap_blit(straight, 0, 0, source, NULL, TB_BITMAP_BLEND_ALPHA);
    tb_bitmap_blit(premultiplied, 0, 0, source, NULL, TB_BITMAP_BLEND_ALPHA);
    tb_bitmap_blit(nothing, 0, 0, source, NULL, TB_BITMAP_BLEND_ALPHA);
    EXPECT_COLOR(tb_bitmap_get_pixel(straight, 0, 0), ((tb_bitmap_color){128, 0, 127, 255}));
    EXPECT_COLOR(tb_bitmap_get_pixel(premultiplied, 0, 0), ((tb_bitmap_color){128, 0, 127, 255}));
    /* Over nothing, the source is itself, straight. */
    EXPECT_COLOR(tb_bitmap_get_pixel(nothing, 0, 0), ((tb_bitmap_color){255, 0, 0, 128}));
    /* A premultiplied source keeps its meaning over a straight destination. */
    tb_bitmap_premultiply(source);
    tb_bitmap_fill(straight, blue);
    tb_bitmap_blit(straight, 0, 0, source, NULL, TB_BITMAP_BLEND_ALPHA);
    EXPECT_COLOR(tb_bitmap_get_pixel(straight, 0, 0), ((tb_bitmap_color){128, 0, 127, 255}));
    /* Copying it to a straight bitmap makes it straight again. */
    tb_bitmap_blit(nothing, 0, 0, source, NULL, TB_BITMAP_BLEND_NONE);
    EXPECT_COLOR(tb_bitmap_get_pixel(nothing, 0, 0), ((tb_bitmap_color){255, 0, 0, 128}));
    tb_bitmap_destroy(nothing);
    tb_bitmap_destroy(premultiplied);
    tb_bitmap_destroy(straight);
    tb_bitmap_destroy(source);
}

/* A bitmap blitted onto itself, overlapping, moves as if through a copy. */
TB_TEST(media_bitmap, a_bitmap_blits_to_itself)
{
    tb_bitmap *const bitmap = numbered(4, 3, TB_BITMAP_RGBA8);
    tb_bitmap *const original = tb_bitmap_duplicate(bitmap, NULL);

    tb_bitmap_blit(bitmap, 1, 1, bitmap, NULL, TB_BITMAP_BLEND_NONE);
    for (uint32_t y = 1; y < 3; y++)
    {
        for (uint32_t x = 1; x < 4; x++)
        {
            EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, x, y),
                         tb_bitmap_get_pixel(original, x - 1u, y - 1u));
        }
    }
    EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, 0, 0), tb_bitmap_get_pixel(original, 0, 0));
    /* Apart, no copy is needed, and the result is the same. */
    const tb_bitmap_rect corner = {0, 0, 1, 1};
    tb_bitmap_blit(bitmap, 3, 2, bitmap, &corner, TB_BITMAP_BLEND_ALPHA);
    EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, 3, 2), tb_bitmap_get_pixel(original, 0, 0));
    tb_bitmap_destroy(original);
    tb_bitmap_destroy(bitmap);
}

TB_TEST(media_bitmap, flips_reverse_rows_and_columns)
{
    static const tb_bitmap_format formats[] = {TB_BITMAP_RGBA8, TB_BITMAP_GRAY8};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(formats); index++)
    {
        tb_bitmap *const original = numbered(5, 3, formats[index]);
        tb_bitmap *const vertical = tb_bitmap_duplicate(original, NULL);
        tb_bitmap *const horizontal = tb_bitmap_duplicate(original, NULL);
        tb_bitmap_flip_vertical(vertical);
        tb_bitmap_flip_horizontal(horizontal);
        TB_TEST_CONTEXT("%s", tb_bitmap_format_name(formats[index]));
        for (uint32_t y = 0; y < 3; y++)
        {
            for (uint32_t x = 0; x < 5; x++)
            {
                EXPECT_COLOR(tb_bitmap_get_pixel(vertical, x, y),
                             tb_bitmap_get_pixel(original, x, 2u - y));
                EXPECT_COLOR(tb_bitmap_get_pixel(horizontal, x, y),
                             tb_bitmap_get_pixel(original, 4u - x, y));
            }
        }
        tb_bitmap_flip_vertical(vertical);
        tb_bitmap_flip_horizontal(horizontal);
        TB_TEST_EXPECT(tb_bitmap_equal(vertical, original));
        TB_TEST_EXPECT(tb_bitmap_equal(horizontal, original));
        tb_bitmap_destroy(horizontal);
        tb_bitmap_destroy(vertical);
        tb_bitmap_destroy(original);
    }
}

/* Rows wider than the buffer they are swapped through flip whole. */
TB_TEST(media_bitmap, wide_rows_flip_whole)
{
    tb_bitmap *const original = numbered(200, 3, TB_BITMAP_RGBA8);
    tb_bitmap *const flipped = tb_bitmap_duplicate(original, NULL);

    tb_bitmap_flip_vertical(flipped);
    for (uint32_t x = 0; x < 200; x++)
    {
        EXPECT_COLOR(tb_bitmap_get_pixel(flipped, x, 0), tb_bitmap_get_pixel(original, x, 2));
    }
    tb_bitmap_destroy(flipped);
    tb_bitmap_destroy(original);
}

/* Premultiplying rounds as the canvas does, (c * a + 127) / 255, for every
 * channel and alpha, so pixels premultiplied here or by the canvas match. */
TB_TEST(media_bitmap, premultiplying_rounds_as_the_canvas_does)
{
    tb_bitmap *const bitmap = tb_bitmap_create(256, 256, TB_BITMAP_RGBA8, NULL);
    for (uint32_t alpha = 0; alpha < 256; alpha++)
    {
        for (uint32_t channel = 0; channel < 256; channel++)
        {
            tb_bitmap_set_pixel(
                bitmap, channel, alpha,
                (tb_bitmap_color){(uint8_t)channel, (uint8_t)(255u - channel), 0, (uint8_t)alpha});
        }
    }
    tb_bitmap_premultiply(bitmap);
    TB_TEST_EXPECT(info_of(bitmap).premultiplied);
    size_t wrong = 0;
    for (uint32_t alpha = 0; alpha < 256; alpha++)
    {
        for (uint32_t channel = 0; channel < 256; channel++)
        {
            const tb_bitmap_color color = tb_bitmap_get_pixel(bitmap, channel, alpha);
            wrong += color.r != (channel * alpha + 127u) / 255u;
            wrong += color.g != ((255u - channel) * alpha + 127u) / 255u;
            wrong += color.a != alpha;
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0);
    /* Again does nothing; undoing it brings opaque pixels back exactly, and
     * transparent ones to transparent black. */
    tb_bitmap_premultiply(bitmap);
    tb_bitmap_unpremultiply(bitmap);
    TB_TEST_EXPECT(!info_of(bitmap).premultiplied);
    for (uint32_t channel = 0; channel < 256; channel++)
    {
        EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, channel, 255),
                     ((tb_bitmap_color){(uint8_t)channel, (uint8_t)(255u - channel), 0, 255}));
        EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, channel, 0), clear);
    }
    tb_bitmap_unpremultiply(bitmap);
    tb_bitmap_destroy(bitmap);
}

/* Unpremultiplying is the division nearest to undoing it, and never over 255. */
TB_TEST(media_bitmap, unpremultiplying_divides_by_alpha)
{
    tb_bitmap *const bitmap = tb_bitmap_create(2, 1, TB_BITMAP_BGRA8, NULL);
    tb_bitmap_set_premultiplied(bitmap, true);
    tb_bitmap_set_pixel(bitmap, 0, 0, (tb_bitmap_color){64, 32, 128, 128});
    tb_bitmap_set_pixel(bitmap, 1, 0, (tb_bitmap_color){200, 0, 0, 100});
    tb_bitmap_unpremultiply(bitmap);
    EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, 0, 0), ((tb_bitmap_color){128, 64, 255, 128}));
    EXPECT_COLOR(tb_bitmap_get_pixel(bitmap, 1, 0), ((tb_bitmap_color){255, 0, 0, 100}));
    tb_bitmap_destroy(bitmap);

    tb_bitmap *const gray = tb_bitmap_create(1, 1, TB_BITMAP_GRAY8, NULL);
    tb_bitmap_premultiply(gray);
    TB_TEST_EXPECT(!info_of(gray).premultiplied);
    tb_bitmap_destroy(gray);
}

/* The color key of a classic sheet becomes transparent black; nothing else changes. */
TB_TEST(media_bitmap, a_color_key_becomes_transparent)
{
    static const tb_bitmap_format formats[] = {TB_BITMAP_RGBA8, TB_BITMAP_BGRA8};
    const tb_bitmap_color magenta = {255, 0, 255, 255};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(formats); index++)
    {
        tb_bitmap *const sheet = tb_bitmap_create(3, 2, formats[index], NULL);
        tb_bitmap_fill(sheet, magenta);
        tb_bitmap_set_pixel(sheet, 1, 0, red);
        tb_bitmap_set_pixel(sheet, 2, 1, (tb_bitmap_color){255, 0, 255, 7});
        TB_TEST_CONTEXT("%s", tb_bitmap_format_name(formats[index]));
        TB_TEST_EXPECT_UINT(tb_bitmap_color_key(sheet, (tb_bitmap_color){255, 0, 255, 0}), ==, 5);
        EXPECT_COLOR(tb_bitmap_get_pixel(sheet, 0, 0), clear);
        EXPECT_COLOR(tb_bitmap_get_pixel(sheet, 1, 0), red);
        EXPECT_COLOR(tb_bitmap_get_pixel(sheet, 2, 1), clear);
        TB_TEST_EXPECT_UINT(tb_bitmap_color_key(sheet, magenta), ==, 0);
        tb_bitmap_destroy(sheet);
    }
}

/* Converting changes how a pixel is stored, not what it is; colors to gray
 * keep their luminance; gray to colors is opaque. */
TB_TEST(media_bitmap, converting_keeps_what_pixels_are)
{
    tb_bitmap *const rgba = numbered(4, 3, TB_BITMAP_RGBA8);
    tb_bitmap_set_pixel(rgba, 0, 0, (tb_bitmap_color){1, 2, 3, 4});
    tb_bitmap_premultiply(rgba);
    tb_bitmap *const bgra = tb_bitmap_convert(rgba, TB_BITMAP_BGRA8, NULL);
    tb_bitmap *const back = tb_bitmap_convert(bgra, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const same = tb_bitmap_convert(rgba, TB_BITMAP_RGBA8, NULL);
    tb_bitmap *const gray = tb_bitmap_convert(rgba, TB_BITMAP_GRAY8, NULL);
    tb_bitmap *const colored = tb_bitmap_convert(gray, TB_BITMAP_RGBA8, NULL);

    TB_TEST_EXPECT(info_of(bgra).premultiplied);
    TB_TEST_EXPECT(tb_bitmap_equal(rgba, back));
    TB_TEST_EXPECT(tb_bitmap_equal(rgba, same));
    TB_TEST_EXPECT(!info_of(gray).premultiplied);
    for (uint32_t y = 0; y < 3; y++)
    {
        for (uint32_t x = 0; x < 4; x++)
        {
            const tb_bitmap_color pixel = tb_bitmap_get_pixel(rgba, x, y);
            const uint8_t luminance =
                (uint8_t)((54u * pixel.r + 183u * pixel.g + 19u * pixel.b + 128u) >> 8);
            EXPECT_COLOR(tb_bitmap_get_pixel(bgra, x, y), pixel);
            EXPECT_COLOR(tb_bitmap_get_pixel(colored, x, y),
                         ((tb_bitmap_color){luminance, luminance, luminance, 255}));
        }
    }
    tb_bitmap_destroy(colored);
    tb_bitmap_destroy(gray);
    tb_bitmap_destroy(same);
    tb_bitmap_destroy(back);
    tb_bitmap_destroy(bgra);
    tb_bitmap_destroy(rgba);
}

/* Nearest scaling by whole factors repeats or picks pixels exactly. */
TB_TEST(media_bitmap, nearest_scaling_repeats_and_picks_pixels)
{
    tb_bitmap *const source = numbered(3, 2, TB_BITMAP_RGBA8);
    tb_bitmap *const larger = tb_bitmap_scale(source, 6, 6, TB_BITMAP_NEAREST, NULL);
    tb_bitmap *const smaller = tb_bitmap_scale(larger, 3, 2, TB_BITMAP_NEAREST, NULL);

    for (uint32_t y = 0; y < 6; y++)
    {
        for (uint32_t x = 0; x < 6; x++)
        {
            EXPECT_COLOR(tb_bitmap_get_pixel(larger, x, y),
                         tb_bitmap_get_pixel(source, x / 2u, y / 3u));
        }
    }
    TB_TEST_EXPECT(tb_bitmap_equal(source, smaller));
    tb_bitmap_destroy(smaller);
    tb_bitmap_destroy(larger);
    tb_bitmap_destroy(source);
}

/* Bilinear scaling keeps a flat color flat and a bitmap at its own size as
 * it is; between pixels it blends; and a transparent pixel's color does not
 * bleed into an opaque neighbour's. */
TB_TEST(media_bitmap, bilinear_scaling_blends_by_alpha)
{
    tb_bitmap *const flat = tb_bitmap_create(3, 3, TB_BITMAP_BGRA8, NULL);
    tb_bitmap_fill(flat, (tb_bitmap_color){10, 200, 30, 255});
    tb_bitmap *const flat_scaled = tb_bitmap_scale(flat, 7, 5, TB_BITMAP_BILINEAR, NULL);
    for (uint32_t y = 0; y < 5; y++)
    {
        for (uint32_t x = 0; x < 7; x++)
        {
            EXPECT_COLOR(tb_bitmap_get_pixel(flat_scaled, x, y),
                         ((tb_bitmap_color){10, 200, 30, 255}));
        }
    }
    tb_bitmap *const numbers = numbered(4, 4, TB_BITMAP_RGBA8);
    tb_bitmap *const same = tb_bitmap_scale(numbers, 4, 4, TB_BITMAP_BILINEAR, NULL);
    TB_TEST_EXPECT(tb_bitmap_equal(numbers, same));

    /* Opaque red beside transparent green: the middle is red, half covered. */
    tb_bitmap *const pair = tb_bitmap_create(2, 1, TB_BITMAP_RGBA8, NULL);
    tb_bitmap_set_pixel(pair, 0, 0, red);
    tb_bitmap_set_pixel(pair, 1, 0, (tb_bitmap_color){0, 255, 0, 0});
    tb_bitmap *const wide = tb_bitmap_scale(pair, 4, 1, TB_BITMAP_BILINEAR, NULL);
    EXPECT_COLOR(tb_bitmap_get_pixel(wide, 0, 0), red);
    EXPECT_COLOR(tb_bitmap_get_pixel(wide, 1, 0), ((tb_bitmap_color){255, 0, 0, 191}));
    EXPECT_COLOR(tb_bitmap_get_pixel(wide, 2, 0), ((tb_bitmap_color){255, 0, 0, 64}));
    EXPECT_COLOR(tb_bitmap_get_pixel(wide, 3, 0), clear);
    tb_bitmap_premultiply(pair);
    tb_bitmap *const wide_premultiplied = tb_bitmap_scale(pair, 4, 1, TB_BITMAP_BILINEAR, NULL);
    TB_TEST_EXPECT(info_of(wide_premultiplied).premultiplied);
    EXPECT_COLOR(tb_bitmap_get_pixel(wide_premultiplied, 1, 0),
                 ((tb_bitmap_color){191, 0, 0, 191}));
    tb_bitmap *const gray = tb_bitmap_convert(numbers, TB_BITMAP_GRAY8, NULL);
    tb_bitmap *const gray_scaled = tb_bitmap_scale(gray, 1, 1, TB_BITMAP_BILINEAR, NULL);
    TB_TEST_EXPECT_UINT(info_of(gray_scaled).format, ==, TB_BITMAP_GRAY8);
    tb_bitmap_destroy(gray_scaled);
    tb_bitmap_destroy(gray);
    tb_bitmap_destroy(wide_premultiplied);
    tb_bitmap_destroy(wide);
    tb_bitmap_destroy(pair);
    tb_bitmap_destroy(same);
    tb_bitmap_destroy(numbers);
    tb_bitmap_destroy(flat_scaled);
    tb_bitmap_destroy(flat);
}

/* Every bitmap a function makes lives in the allocator it was given, and
 * goes back to it. */
TB_TEST(media_bitmap, every_block_comes_from_the_allocator_given)
{
    tb_test_allocator tracker = {0};
    const tb_allocator allocator = tb_test_allocator_start(&tracker);
    tb_bitmap *const source = tb_bitmap_create(8, 8, TB_BITMAP_RGBA8, &allocator);
    tb_bitmap *made[7] = {0};

    tb_bitmap_fill(source, white);
    made[0] = tb_bitmap_duplicate(source, &allocator);
    made[1] = tb_bitmap_copy_rect(source, (tb_bitmap_rect){1, 1, 3, 3}, &allocator);
    made[2] = tb_bitmap_convert(source, TB_BITMAP_GRAY8, &allocator);
    made[3] = tb_bitmap_scale(source, 3, 3, TB_BITMAP_BILINEAR, &allocator);
    made[4] = tb_bitmap_wrap_region(source, (tb_bitmap_rect){0, 0, 2, 2}, &allocator);
    made[5] = tb_bitmap_create_from(tb_bitmap_const_pixels(source), 256, 8, 8, 32, TB_BITMAP_RGBA8,
                                    &allocator);
    made[6] = tb_bitmap_create(0, 0, TB_BITMAP_GRAY8, &allocator);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 14);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(made); index++)
    {
        tb_bitmap_destroy(made[index]);
    }
    tb_bitmap_destroy(source);
    TB_TEST_EXPECT_UINT(tracker.live_block_count, ==, 0);
    TB_TEST_EXPECT_STRING(tracker.first_violation, ==, "none");
}

/* ---------------------------------------------------------------------------
 * Contract violations.
 * ------------------------------------------------------------------------- */

/* What a trap's body is given: a bitmap of 2 by 1 in RGBA8, and one in gray. */
typedef struct trap_bitmaps
{
    tb_bitmap *rgba;
    tb_bitmap *gray;
} trap_bitmaps;

static void create_too_wide(void *context)
{
    (void)context;
    (void)tb_bitmap_create(TB_BITMAP_MAX_DIMENSION + 1u, 1, TB_BITMAP_RGBA8, NULL);
}

static void read_outside(void *context)
{
    const trap_bitmaps *const bitmaps = context;
    (void)tb_bitmap_get_pixel(bitmaps->rgba, 2, 0);
}

static void write_outside(void *context)
{
    const trap_bitmaps *const bitmaps = context;
    tb_bitmap_set_pixel(bitmaps->rgba, 0, 1, white);
}

static void row_outside(void *context)
{
    const trap_bitmaps *const bitmaps = context;
    (void)tb_bitmap_row(bitmaps->rgba, 1);
}

static void premultiply_gray(void *context)
{
    const trap_bitmaps *const bitmaps = context;
    tb_bitmap_set_premultiplied(bitmaps->gray, true);
}

static void key_gray(void *context)
{
    const trap_bitmaps *const bitmaps = context;
    (void)tb_bitmap_color_key(bitmaps->gray, white);
}

static void wrap_short(void *context)
{
    (void)context;
    uint8_t pixels[7] = {0};
    (void)tb_bitmap_wrap(pixels, sizeof(pixels), 2, 1, 8, TB_BITMAP_RGBA8, NULL);
}

static void narrow_stride(void *context)
{
    (void)context;
    uint8_t pixels[16] = {0};
    (void)tb_bitmap_create_from(pixels, sizeof(pixels), 2, 2, 4, TB_BITMAP_RGBA8, NULL);
}

static void scale_to_nothing(void *context)
{
    const trap_bitmaps *const bitmaps = context;
    (void)tb_bitmap_scale(bitmaps->rgba, 0, 1, TB_BITMAP_NEAREST, NULL);
}

TB_TEST(media_bitmap, contract_violations_fail_assertions)
{
    static const struct
    {
        tb_test_trap_body *body;
        const char *message;
    } rows[] = {
        {create_too_wide, "at most TB_BITMAP_MAX_DIMENSION"},
        {read_outside, "a pixel of the bitmap"},
        {write_outside, "a pixel of the bitmap"},
        {row_outside, "a row of the bitmap"},
        {premultiply_gray, "a gray bitmap has no alpha"},
        {key_gray, "a color key needs a bitmap with alpha"},
        {wrap_short, "the pixels hold every row"},
        {narrow_stride, "a stride holds a row's pixels"},
        {scale_to_nothing, "a scaled bitmap has pixels"},
    };
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    trap_bitmaps bitmaps = {tb_bitmap_create(2, 1, TB_BITMAP_RGBA8, NULL),
                            tb_bitmap_create(2, 1, TB_BITMAP_GRAY8, NULL)};
    for (size_t row = 0; row < TB_TEST_COUNT_OF(rows); row++)
    {
        TB_TEST_CONTEXT("row %zu", row);
        TB_TEST_EXPECT_TRAP(rows[row].body, &bitmaps, rows[row].message);
    }
    tb_bitmap_destroy(bitmaps.gray);
    tb_bitmap_destroy(bitmaps.rgba);
}
