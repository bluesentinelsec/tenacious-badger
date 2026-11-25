/* Nine-slice panels cut into the slices that draw them: see
 * tb_canvas_nine_slice in canvas.h. Pure arithmetic, so that interfaces
 * that build their own draw commands cut panels as the canvas does. */
#include "badger/assert.h"
#include "badger/canvas.h"

#include <math.h>

/* How far below a whole number a border's pixels may fall and still count
 * as it, against rounding in `scale * pixels_per_unit`. */
#define TB_CANVAS_SLICE_EPSILON 1e-4f

static float tb_canvas_slice_snap(float value, float pixels_per_unit)
{
    return roundf(value * pixels_per_unit) / pixels_per_unit;
}

/* Units a texel of a border is drawn. */
static float tb_canvas_slice_texel(const tb_canvas_nine_slice *slice, float pixels_per_unit)
{
    const float scale = slice->scale > 0.0f ? slice->scale : 1.0f;

    if (!slice->whole_pixels)
    {
        return scale;
    }
    float pixels = floorf(scale * pixels_per_unit + TB_CANVAS_SLICE_EPSILON);
    if (pixels < 1.0f)
    {
        pixels = 1.0f;
    }
    return pixels / pixels_per_unit;
}

/* One axis of a panel: the texels its four cuts are at, and the units its
 * four edges are drawn at. */
typedef struct tb_canvas_slice_axis
{
    uint32_t cuts[4];
    float edges[4];
} tb_canvas_slice_axis;

static tb_canvas_slice_axis tb_canvas_slice_cut(uint32_t start, uint32_t length, uint32_t before,
                                                uint32_t after, float position, float size,
                                                float texel, float pixels_per_unit)
{
    tb_canvas_slice_axis axis = {0};
    const uint32_t first = before < length ? before : length;
    const uint32_t last = after < length - first ? after : length - first;
    float near = (float)first * texel;
    float far = (float)last * texel;

    /* Borders wider than the panel are shrunk together to fit it. */
    if (near + far > size && near + far > 0.0f)
    {
        const float fit = size > 0.0f ? size / (near + far) : 0.0f;
        near *= fit;
        far *= fit;
    }
    axis.cuts[0] = start;
    axis.cuts[1] = start + first;
    axis.cuts[2] = start + length - last;
    axis.cuts[3] = start + length;
    axis.edges[0] = tb_canvas_slice_snap(position, pixels_per_unit);
    axis.edges[3] = tb_canvas_slice_snap(position + size, pixels_per_unit);
    axis.edges[1] = fminf(tb_canvas_slice_snap(position + near, pixels_per_unit), axis.edges[3]);
    axis.edges[2] =
        fmaxf(tb_canvas_slice_snap(position + size - far, pixels_per_unit), axis.edges[1]);
    return axis;
}

size_t tb_canvas_nine_slice_layout(const tb_canvas_nine_slice *slice, uint32_t image_width,
                                   uint32_t image_height, tb_canvas_rect rect,
                                   float pixels_per_unit, tb_canvas_slice *slices)
{
    tb_rhi_rect source = {0};
    size_t count = 0;

    TB_ASSERT(slice != NULL);
    TB_ASSERT(slices != NULL);
    TB_ASSERT(pixels_per_unit > 0.0f);
    source = slice->source;
    if (source.width == 0 || source.height == 0)
    {
        source = (tb_rhi_rect){0, 0, image_width, image_height};
    }
    const float texel = tb_canvas_slice_texel(slice, pixels_per_unit);
    const tb_canvas_slice_axis across =
        tb_canvas_slice_cut(source.x, source.width, slice->left, slice->right, rect.x, rect.width,
                            texel, pixels_per_unit);
    const tb_canvas_slice_axis down =
        tb_canvas_slice_cut(source.y, source.height, slice->top, slice->bottom, rect.y,
                            rect.height, texel, pixels_per_unit);

    for (size_t row = 0; row < 3; row++)
    {
        for (size_t column = 0; column < 3; column++)
        {
            const tb_canvas_slice part = {
                .source = {across.cuts[column], down.cuts[row],
                           across.cuts[column + 1] - across.cuts[column],
                           down.cuts[row + 1] - down.cuts[row]},
                .rect = {across.edges[column], down.edges[row],
                         across.edges[column + 1] - across.edges[column],
                         down.edges[row + 1] - down.edges[row]}};
            const bool middle = row == 1 && column == 1;
            if ((middle && slice->hollow) || part.source.width == 0 || part.source.height == 0 ||
                !(part.rect.width > 0.0f) || !(part.rect.height > 0.0f))
            {
                continue;
            }
            slices[count] = part;
            count++;
        }
    }
    return count;
}
