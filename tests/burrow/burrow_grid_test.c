/* Burrow's world grid must show meters and quarters as lines over squares
 * of a meter that alternate in shade, and repeat without a seam: a line at
 * the texture's edge is split between its two sides, so that the texture
 * tiles into lines of one width. */
#include "burrow_grid.h"

#include "badger/memory.h"

#include "tb_test.h"

static const uint8_t *texel_at(const uint8_t *pixels, uint32_t x, uint32_t y)
{
    return &pixels[((size_t)y * BURROW_GRID_SIZE + x) * 4u];
}

static uint8_t shade_at(const uint8_t *pixels, uint32_t x, uint32_t y)
{
    return texel_at(pixels, x, y)[0];
}

static uint8_t *make_grid(void)
{
    uint8_t *const pixels = tb_malloc(BURROW_GRID_BYTES);

    burrow_grid_fill(pixels, BURROW_GRID_BYTES);
    return pixels;
}

TB_TEST(burrow_grid, every_texel_is_an_opaque_gray)
{
    uint8_t *const pixels = make_grid();
    size_t wrong = 0;

    for (uint32_t y = 0; y < BURROW_GRID_SIZE; y++)
    {
        for (uint32_t x = 0; x < BURROW_GRID_SIZE; x++)
        {
            const uint8_t *const texel = texel_at(pixels, x, y);
            if (texel[0] != texel[1] || texel[1] != texel[2] || texel[3] != 255u)
            {
                wrong++;
            }
        }
    }
    TB_TEST_EXPECT_UINT(wrong, ==, 0u);
    tb_free(pixels);
}

TB_TEST(burrow_grid, meters_are_dark_lines_and_quarters_fainter)
{
    uint8_t *const pixels = make_grid();
    const uint8_t square = shade_at(pixels, 40u, 40u);
    const uint8_t meter = shade_at(pixels, BURROW_GRID_METER, 40u);
    const uint8_t quarter = shade_at(pixels, BURROW_GRID_QUARTER, 40u);

    TB_TEST_EXPECT_UINT(meter, <, quarter);
    TB_TEST_EXPECT_UINT(quarter, <, square);
    /* Lines run both ways; a meter's crosses a quarter's. */
    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, BURROW_GRID_METER), ==, meter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, BURROW_GRID_QUARTER), ==, quarter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, BURROW_GRID_METER, BURROW_GRID_QUARTER), ==, meter);
    /* A meter's line is four texels across, half each side of the meter. */
    TB_TEST_EXPECT_UINT(shade_at(pixels, BURROW_GRID_METER - 2u, 40u), ==, meter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, BURROW_GRID_METER + 1u, 40u), ==, meter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, BURROW_GRID_METER - 3u, 40u), ==, square);
    TB_TEST_EXPECT_UINT(shade_at(pixels, BURROW_GRID_METER + 2u, 40u), !=, meter);
    tb_free(pixels);
}

TB_TEST(burrow_grid, squares_of_a_meter_alternate)
{
    uint8_t *const pixels = make_grid();
    const uint32_t next = BURROW_GRID_METER + 40u;

    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, 40u), !=, shade_at(pixels, next, 40u));
    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, 40u), !=, shade_at(pixels, 40u, next));
    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, 40u), ==, shade_at(pixels, next, next));
    tb_free(pixels);
}

TB_TEST(burrow_grid, it_repeats_without_a_seam)
{
    uint8_t *const pixels = make_grid();
    const uint8_t meter = shade_at(pixels, BURROW_GRID_METER, 40u);
    const uint32_t last = BURROW_GRID_SIZE - 1u;

    /* Two texels of the line on each side of the edge, and no more. */
    for (uint32_t y = 0; y < BURROW_GRID_SIZE; y++)
    {
        TB_TEST_CONTEXT("row %u", y);
        TB_TEST_EXPECT_UINT(shade_at(pixels, 0u, y), ==, meter);
        TB_TEST_EXPECT_UINT(shade_at(pixels, 1u, y), ==, meter);
        TB_TEST_EXPECT_UINT(shade_at(pixels, last - 1u, y), ==, meter);
        TB_TEST_EXPECT_UINT(shade_at(pixels, last, y), ==, meter);
    }
    TB_TEST_EXPECT_UINT(shade_at(pixels, 2u, 40u), !=, meter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, last - 2u, 40u), !=, meter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, last), ==, meter);
    TB_TEST_EXPECT_UINT(shade_at(pixels, 40u, 0u), ==, meter);
    tb_free(pixels);
}
