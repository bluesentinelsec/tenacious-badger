/* Burrow's world grid: squares from the canvas's checkerboard, then the
 * lines over them. */
#include "burrow_grid.h"

#include "badger/assert.h"
#include "badger/canvas.h"

#include <stdbool.h>

/* Texels across a meter's line and a quarter's, half on each side of where
 * it falls. */
#define BURROW_GRID_METER_LINE 4u
#define BURROW_GRID_QUARTER_LINE 2u

/* The shades, in sRGB: the two squares, a meter's line, a quarter's. */
#define BURROW_GRID_LIGHT 0.70f
#define BURROW_GRID_DARK 0.64f
#define BURROW_GRID_METER_SHADE 64u
#define BURROW_GRID_QUARTER_SHADE 128u

/* Whether a texel's column or row lies on a line every `spacing` texels,
 * `width` texels across. */
static bool burrow_on_line(uint32_t texel, uint32_t spacing, uint32_t width)
{
    const uint32_t offset = (texel + width / 2u) % spacing;

    return offset < width;
}

/* The shade of a line over a texel, or 0 for none: a meter's line over a
 * quarter's where they cross. */
static uint8_t burrow_line_shade(uint32_t x, uint32_t y)
{
    if (burrow_on_line(x, BURROW_GRID_METER, BURROW_GRID_METER_LINE) ||
        burrow_on_line(y, BURROW_GRID_METER, BURROW_GRID_METER_LINE))
    {
        return BURROW_GRID_METER_SHADE;
    }
    if (burrow_on_line(x, BURROW_GRID_QUARTER, BURROW_GRID_QUARTER_LINE) ||
        burrow_on_line(y, BURROW_GRID_QUARTER, BURROW_GRID_QUARTER_LINE))
    {
        return BURROW_GRID_QUARTER_SHADE;
    }
    return 0u;
}

void burrow_grid_fill(uint8_t *pixels, size_t size)
{
    const tb_vec4 light = {BURROW_GRID_LIGHT, BURROW_GRID_LIGHT, BURROW_GRID_LIGHT, 1.0f};
    const tb_vec4 dark = {BURROW_GRID_DARK, BURROW_GRID_DARK, BURROW_GRID_DARK, 1.0f};

    TB_ASSERT(size >= BURROW_GRID_BYTES);
    tb_image_fill_checkerboard(pixels, size, BURROW_GRID_SIZE, BURROW_GRID_SIZE, BURROW_GRID_METER,
                               light, dark);
    for (uint32_t y = 0; y < BURROW_GRID_SIZE; y++)
    {
        for (uint32_t x = 0; x < BURROW_GRID_SIZE; x++)
        {
            const uint8_t shade = burrow_line_shade(x, y);
            uint8_t *const texel = &pixels[((size_t)y * BURROW_GRID_SIZE + x) * 4u];
            if (shade == 0u)
            {
                continue;
            }
            texel[0] = shade;
            texel[1] = shade;
            texel[2] = shade;
        }
    }
}
