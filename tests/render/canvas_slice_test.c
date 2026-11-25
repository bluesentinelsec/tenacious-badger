/* Nine-slice panels cut into slices (tb_canvas_nine_slice_layout()): the
 * corners keep their size in units at every view's scale, every edge lands
 * on a whole pixel, the slices tile the rectangle without gaps, and pixel
 * art's borders are a whole number of pixels a texel. */
#include "badger/canvas.h"

#include "tb_test.h"

#include <math.h>

/* An image of 24 by 24 texels, cut 8 from each side. */
#define SIDE 24u
#define INSET 8u

static bool on_whole_pixel(float value, float scale)
{
    const float pixels = value * scale;
    return fabsf(pixels - roundf(pixels)) < 1e-3f;
}

/* The slices cover the rectangle, row by row, each edge on a pixel, and
 * neighbours share their edges. */
static void expect_tiled(const tb_canvas_slice *slices, size_t count, tb_canvas_rect rect,
                         float scale)
{
    float area = 0.0f;

    for (size_t index = 0; index < count; index++)
    {
        const tb_canvas_rect part = slices[index].rect;
        TB_TEST_CONTEXT("slice %zu at a scale of %.2f", index, (double)scale);
        TB_TEST_EXPECT(on_whole_pixel(part.x, scale));
        TB_TEST_EXPECT(on_whole_pixel(part.y, scale));
        TB_TEST_EXPECT(on_whole_pixel(part.x + part.width, scale));
        TB_TEST_EXPECT(on_whole_pixel(part.y + part.height, scale));
        area += part.width * part.height;
    }
    TB_TEST_EXPECT_FLOAT_NEAR(area, rect.width * rect.height, 0.05f);
}

/* At scales 1, 1.5, and 2, a panel's corners are the inset in units, its
 * slices tile it on whole pixels, and its source is cut at the insets. */
TB_TEST(canvas_slice, corners_keep_their_units_at_every_scale)
{
    static const float scales[] = {1.0f, 1.5f, 2.0f};
    const tb_canvas_nine_slice slice = {
        .image = NULL, .left = INSET, .top = INSET, .right = INSET, .bottom = INSET};
    const tb_canvas_rect rect = {10.0f, 20.0f, 100.0f, 40.0f};

    for (size_t at = 0; at < sizeof(scales) / sizeof(scales[0]); at++)
    {
        tb_canvas_slice slices[TB_CANVAS_NINE_SLICES] = {0};
        const size_t count = tb_canvas_nine_slice_layout(&slice, SIDE, SIDE, rect, scales[at], slices);
        TB_TEST_REQUIRE_UINT(count, ==, 9);
        TB_TEST_CONTEXT("scale %.2f", (double)scales[at]);
        /* The top left corner: 8 by 8 texels, drawn 8 by 8 units. */
        TB_TEST_EXPECT_UINT(slices[0].source.x, ==, 0);
        TB_TEST_EXPECT_UINT(slices[0].source.width, ==, INSET);
        TB_TEST_EXPECT_FLOAT_NEAR(slices[0].rect.width, 8.0f, 1e-4f);
        TB_TEST_EXPECT_FLOAT_NEAR(slices[0].rect.height, 8.0f, 1e-4f);
        /* The middle: the texels between the cuts, stretched over the rest. */
        TB_TEST_EXPECT_UINT(slices[4].source.x, ==, INSET);
        TB_TEST_EXPECT_UINT(slices[4].source.width, ==, SIDE - 2u * INSET);
        TB_TEST_EXPECT_FLOAT_NEAR(slices[4].rect.width, 84.0f, 1e-4f);
        TB_TEST_EXPECT_FLOAT_NEAR(slices[4].rect.height, 24.0f, 1e-4f);
        /* The bottom right corner ends at the rectangle's corner. */
        TB_TEST_EXPECT_FLOAT_NEAR(slices[8].rect.x + slices[8].rect.width, 110.0f, 1e-4f);
        TB_TEST_EXPECT_FLOAT_NEAR(slices[8].rect.y + slices[8].rect.height, 60.0f, 1e-4f);
        expect_tiled(slices, count, rect, scales[at]);
    }
}

/* A rectangle off the pixel grid has its slices' edges moved onto it, the
 * corners as near their size as whole pixels allow. */
TB_TEST(canvas_slice, edges_land_on_whole_pixels_off_the_grid)
{
    static const float scales[] = {1.0f, 1.5f, 2.0f};
    const tb_canvas_nine_slice slice = {.left = 5, .top = 3, .right = 7, .bottom = 4, .scale = 1.3f};
    const tb_canvas_rect rect = {3.3f, 7.7f, 61.1f, 29.9f};

    for (size_t at = 0; at < sizeof(scales) / sizeof(scales[0]); at++)
    {
        tb_canvas_slice slices[TB_CANVAS_NINE_SLICES] = {0};
        const float scale = scales[at];
        const size_t count = tb_canvas_nine_slice_layout(&slice, 16, 16, rect, scale, slices);
        TB_TEST_REQUIRE_UINT(count, ==, 9);
        TB_TEST_CONTEXT("scale %.2f", (double)scale);
        for (size_t index = 0; index < count; index++)
        {
            TB_TEST_EXPECT(on_whole_pixel(slices[index].rect.x, scale));
            TB_TEST_EXPECT(on_whole_pixel(slices[index].rect.x + slices[index].rect.width, scale));
        }
        /* Neighbours share an edge: no gap, no overlap. */
        TB_TEST_EXPECT_FLOAT(slices[0].rect.x + slices[0].rect.width, ==, slices[1].rect.x);
        TB_TEST_EXPECT_FLOAT(slices[1].rect.x + slices[1].rect.width, ==, slices[2].rect.x);
        TB_TEST_EXPECT_FLOAT(slices[0].rect.y + slices[0].rect.height, ==, slices[3].rect.y);
        /* The left corner is 5 texels of 1.3 units, within half a pixel. */
        TB_TEST_EXPECT_FLOAT_NEAR(slices[0].rect.width, 6.5f, 0.5f / scale + 1e-4f);
    }
}

/* Pixel art: with whole pixels, a texel of a border is a whole number of
 * pixels, the most that fits its scale in units, and at least one. */
TB_TEST(canvas_slice, whole_pixels_draw_each_texel_as_whole_pixels)
{
    const tb_canvas_nine_slice slice = {
        .left = 4, .top = 4, .right = 4, .bottom = 4, .scale = 2.0f, .whole_pixels = true};
    const tb_canvas_rect rect = {0.0f, 0.0f, 80.0f, 40.0f};
    tb_canvas_slice slices[TB_CANVAS_NINE_SLICES] = {0};

    /* At 1: 2 pixels a texel. */
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&slice, 12, 12, rect, 1.0f, slices), ==, 9);
    TB_TEST_EXPECT_FLOAT(slices[0].rect.width * 1.0f, ==, 8.0f);
    /* At 1.5: 3 pixels a texel, which is 2 units. */
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&slice, 12, 12, rect, 1.5f, slices), ==, 9);
    TB_TEST_EXPECT_FLOAT_NEAR(slices[0].rect.width * 1.5f, 12.0f, 1e-4f);
    /* At 1.25: 2.5 pixels a texel would blur, so 2. */
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&slice, 12, 12, rect, 1.25f, slices), ==, 9);
    TB_TEST_EXPECT_FLOAT_NEAR(slices[0].rect.width * 1.25f, 8.0f, 1e-4f);
    /* At 2: 4 pixels a texel. */
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&slice, 12, 12, rect, 2.0f, slices), ==, 9);
    TB_TEST_EXPECT_FLOAT_NEAR(slices[0].rect.width * 2.0f, 16.0f, 1e-4f);

    /* A scale under a pixel still draws a texel as one pixel. */
    const tb_canvas_nine_slice tiny = {.left = 4, .right = 4, .scale = 0.25f, .whole_pixels = true};
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&tiny, 12, 12, rect, 1.0f, slices), ==, 3);
    TB_TEST_EXPECT_FLOAT(slices[0].rect.width, ==, 4.0f);
}

/* Borders wider than the rectangle shrink together to fit it; insets
 * larger than the image are cut at it. */
TB_TEST(canvas_slice, borders_shrink_to_fit_a_small_rectangle)
{
    const tb_canvas_nine_slice slice = {.left = 8, .top = 8, .right = 8, .bottom = 8};
    tb_canvas_slice slices[TB_CANVAS_NINE_SLICES] = {0};
    const size_t count = tb_canvas_nine_slice_layout(&slice, SIDE, SIDE,
                                                     (tb_canvas_rect){0, 0, 10, 40}, 1.0f, slices);

    /* No middle column is left across: six slices. */
    TB_TEST_REQUIRE_UINT(count, ==, 6);
    TB_TEST_EXPECT_FLOAT(slices[0].rect.width, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(slices[1].rect.x, ==, 5.0f);

    const tb_canvas_nine_slice wide = {.left = 30, .right = 30};
    const size_t cut = tb_canvas_nine_slice_layout(&wide, SIDE, SIDE,
                                                   (tb_canvas_rect){0, 0, 100, 10}, 1.0f, slices);
    TB_TEST_REQUIRE_UINT(cut, ==, 1);
    TB_TEST_EXPECT_UINT(slices[0].source.width, ==, SIDE);
}

/* A hollow panel leaves its middle out; a source is cut within itself, as
 * a sprite sheet's frame; no insets is the image stretched whole. */
TB_TEST(canvas_slice, hollow_panels_and_sources)
{
    tb_canvas_slice slices[TB_CANVAS_NINE_SLICES] = {0};
    const tb_canvas_rect rect = {0, 0, 50, 50};
    const tb_canvas_nine_slice hollow = {.left = 2, .top = 2, .right = 2, .bottom = 2, .hollow = true};

    TB_TEST_EXPECT_UINT(tb_canvas_nine_slice_layout(&hollow, 8, 8, rect, 1.0f, slices), ==, 8);
    for (size_t index = 0; index < 8; index++)
    {
        TB_TEST_EXPECT(!(slices[index].rect.width == 46.0f && slices[index].rect.height == 46.0f));
    }

    const tb_canvas_nine_slice framed = {
        .source = {32, 16, 16, 16}, .left = 4, .top = 4, .right = 4, .bottom = 4};
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&framed, 64, 64, rect, 1.0f, slices), ==, 9);
    TB_TEST_EXPECT_UINT(slices[0].source.x, ==, 32);
    TB_TEST_EXPECT_UINT(slices[0].source.y, ==, 16);
    TB_TEST_EXPECT_UINT(slices[8].source.x, ==, 44);
    TB_TEST_EXPECT_UINT(slices[8].source.y + slices[8].source.height, ==, 32);

    const tb_canvas_nine_slice plain = {0};
    TB_TEST_REQUIRE_UINT(tb_canvas_nine_slice_layout(&plain, 8, 8, rect, 1.0f, slices), ==, 1);
    TB_TEST_EXPECT_UINT(slices[0].source.width, ==, 8);
    TB_TEST_EXPECT_FLOAT(slices[0].rect.width, ==, 50.0f);
}
