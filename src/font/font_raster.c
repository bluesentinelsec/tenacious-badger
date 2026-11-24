/* Glyph outlines into coverage: curves cut into lines, and the lines
 * accumulated into a bitmap one band of rows at a time.
 *
 * The smooth mode is a signed-area rasterizer, the kind FreeType's "smooth"
 * renderer, font-rs, and stb_truetype's second rasterizer are. Each line of
 * the outline adds, to every pixel of each row it crosses, how much of the
 * pixel lies to its right within the row, signed by which way the line goes.
 * It adds that as differences along the row, so that the area of a pixel is
 * the sum of the row's differences up to it: a line touches only the pixels
 * it passes through, and one more. Summing a row gives each pixel's area
 * inside the outline, the winding of overlapping contours adding up; its
 * magnitude, at most 1, is the pixel's coverage. That is the non-zero rule,
 * exact for pixels that the edges of no two contours share; a glyph whose
 * contours overlap is drawn again finer (see tb_font_oversample()).
 *
 * The aliased mode counts, at each pixel's center, the windings of the lines
 * that cross the row's center to the left of it, by the same differences
 * along the row: a pixel is full when its winding is not zero. */
#include "badger/font_raster.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/font.h"
#include "badger/sort.h"
#include "font_internal.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* How far a curve's lines may stray from the curve, in pixels. */
#define TB_FONT_CURVE_TOLERANCE 0.05f

/* The most lines one curve is cut into. */
#define TB_FONT_MAX_CURVE_LINES ((size_t)64)

/* Rows accumulated at once: the scratch memory is this many rows. */
#define TB_FONT_BAND_ROWS ((uint32_t)32)

/* How far from the origin, in pixels, a glyph's bitmap may lie. */
#define TB_FONT_MAX_OFFSET 1.0e8f

/* A winding past this, in a smooth pass, means overlapping contours: more
 * than float error can add to 1 across a row. */
#define TB_FONT_OVERLAP_THRESHOLD (1.0f + 1.0f / 64.0f)

/* How many times finer, each way, glyphs of overlapping contours are drawn,
 * and how many rows of pixels are drawn finer at once. */
#define TB_FONT_OVERSAMPLE 8u
#define TB_FONT_OVERSAMPLED_ROWS 4u

/* Each line becomes at most three edges, cut where it crosses the bitmap's
 * left and right sides. */
#define TB_FONT_PIECES_PER_LINE ((size_t)3)

/* One line of an outline, in the bitmap's pixels, from top to bottom. */
typedef struct tb_font_edge
{
    float x0;
    float y0; /* The top: y0 <= y1. */
    float x1;
    float y1;
    float direction; /* +1 for a line that went down, -1 for one that went up. */
} tb_font_edge;

typedef struct tb_font_edges
{
    tb_font_edge *edges;
    size_t count;
    size_t capacity;
    float width; /* The bitmap's width, to which edges are clipped. */
} tb_font_edges;

/* -------------------------------------------------------------------------
 * Sizes.
 * ------------------------------------------------------------------------- */

float tb_font_raster_clamp_size(float pixels_per_em)
{
    if (!isfinite(pixels_per_em) || pixels_per_em <= 0.0f)
    {
        return 0.0f;
    }
    if (pixels_per_em > TB_FONT_RASTER_MAX_PIXELS_PER_EM)
    {
        return TB_FONT_RASTER_MAX_PIXELS_PER_EM;
    }
    return pixels_per_em;
}

float tb_font_raster_subpixel(float subpixel_x)
{
    if (!isfinite(subpixel_x))
    {
        return 0.0f;
    }
    const float fraction = subpixel_x - floorf(subpixel_x);
    /* A tiny negative value's fraction rounds to 1. */
    return fraction < 1.0f ? fraction : 0.0f;
}

float tb_font_scale(const tb_font *font, float pixels_per_em)
{
    TB_ASSERT(font != NULL);
    tb_font_metrics metrics = {0};
    tb_font_get_metrics(font, &metrics);
    return tb_font_raster_clamp_size(pixels_per_em) / (float)metrics.units_per_em;
}

void tb_font_get_line_metrics(const tb_font *font, float pixels_per_em,
                              tb_font_line_metrics *metrics)
{
    TB_ASSERT(font != NULL);
    TB_ASSERT(metrics != NULL);
    tb_font_metrics font_metrics = {0};
    tb_font_get_metrics(font, &font_metrics);
    const float scale = tb_font_scale(font, pixels_per_em);
    metrics->ascent = (float)font_metrics.ascender * scale;
    metrics->descent = (float)font_metrics.descender * scale;
    metrics->line_gap = (float)font_metrics.line_gap * scale;
    metrics->line_height = metrics->ascent - metrics->descent + metrics->line_gap;
}

/* -------------------------------------------------------------------------
 * Curves into lines.
 * ------------------------------------------------------------------------- */

typedef struct tb_font_pixel_point
{
    float x;
    float y;
} tb_font_pixel_point;

static tb_font_pixel_point tb_font_place(const tb_font_raster_placement *placement,
                                         tb_font_point point)
{
    const tb_font_pixel_point placed = {placement->x + point.x * placement->scale,
                                        placement->y - point.y * placement->scale};
    return placed;
}

static bool tb_font_finite(tb_font_pixel_point point)
{
    return isfinite(point.x) && isfinite(point.y);
}

/* How many lines a curve is cut into: a quadratic curve strays from a line
 * cut into n equal steps of its parameter by at most |p0 - 2c + p1| / (4 n^2). */
static size_t tb_font_curve_lines(tb_font_pixel_point from, tb_font_pixel_point control,
                                  tb_font_pixel_point to)
{
    const float bend_x = from.x - 2.0f * control.x + to.x;
    const float bend_y = from.y - 2.0f * control.y + to.y;
    const float bend = sqrtf(bend_x * bend_x + bend_y * bend_y);
    if (!isfinite(bend))
    {
        return 0;
    }
    const float lines = ceilf(sqrtf(bend / (4.0f * TB_FONT_CURVE_TOLERANCE)));
    if (lines < 1.0f)
    {
        return 1;
    }
    if (lines > (float)TB_FONT_MAX_CURVE_LINES)
    {
        return TB_FONT_MAX_CURVE_LINES;
    }
    return (size_t)lines;
}

/* Lines an outline is cut into, at most; a count past the limit stops early. */
static size_t tb_font_count_lines(const tb_font_outline *outline,
                                  const tb_font_raster_placement *placement)
{
    size_t count = 0;
    for (size_t contour_index = 0; contour_index < outline->contour_count; contour_index++)
    {
        const tb_font_contour *contour = &outline->contours[contour_index];
        tb_font_pixel_point from = tb_font_place(placement, contour->start);
        /* The line that closes the contour. */
        count++;
        for (size_t index = 0; index < contour->segment_count; index++)
        {
            const tb_font_segment *segment = &contour->segments[index];
            const tb_font_pixel_point to = tb_font_place(placement, segment->end);
            if (segment->type == TB_FONT_QUADRATIC)
            {
                count += tb_font_curve_lines(from, tb_font_place(placement, segment->control), to);
            }
            else
            {
                count++;
            }
            from = to;
        }
        if (count > TB_FONT_RASTER_MAX_EDGES)
        {
            return count;
        }
    }
    return count;
}

/* Store an edge. A level edge covers no height and adds nothing, but is
 * kept for finding edges that cross. */
static void tb_font_store_edge(tb_font_edges *edges, float x0, float y0, float x1, float y1)
{
    TB_ASSERT(edges->count < edges->capacity);
    tb_font_edge *edge = &edges->edges[edges->count];
    edges->count++;
    const float clamped_x0 = fminf(fmaxf(x0, 0.0f), edges->width);
    const float clamped_x1 = fminf(fmaxf(x1, 0.0f), edges->width);
    if (y0 <= y1)
    {
        *edge = (tb_font_edge){clamped_x0, y0, clamped_x1, y1, 1.0f};
    }
    else
    {
        *edge = (tb_font_edge){clamped_x1, y1, clamped_x0, y0, -1.0f};
    }
}

/* Cut a line where it crosses x = boundary: the y at which it does, or NAN
 * when it does not. */
static float tb_font_crossing(float x0, float y0, float x1, float y1, float boundary)
{
    const bool before = x0 < boundary;
    const bool after = x1 < boundary;
    if (before == after)
    {
        return NAN;
    }
    const float t = (boundary - x0) / (x1 - x0);
    return y0 + t * (y1 - y0);
}

/* Store a line cut at the bitmap's right side. Past it, the line is moved
 * onto the side, where it covers nothing the bitmap holds. */
static void tb_font_store_right(tb_font_edges *edges, float x0, float y0, float x1, float y1)
{
    const float y = tb_font_crossing(x0, y0, x1, y1, edges->width);
    if (isnan(y))
    {
        tb_font_store_edge(edges, x0, y0, x1, y1);
        return;
    }
    tb_font_store_edge(edges, x0, y0, edges->width, y);
    tb_font_store_edge(edges, edges->width, y, x1, y1);
}

/* Store a line cut at the bitmap's left side, and then its right. Left of
 * the bitmap, the line is moved onto its left side, where it covers the
 * whole of every pixel of the rows it crosses, as it would have. */
static void tb_font_store_line(tb_font_edges *edges, tb_font_pixel_point from,
                               tb_font_pixel_point to)
{
    if (!tb_font_finite(from) || !tb_font_finite(to))
    {
        return;
    }
    const float y = tb_font_crossing(from.x, from.y, to.x, to.y, 0.0f);
    if (isnan(y))
    {
        tb_font_store_right(edges, from.x, from.y, to.x, to.y);
        return;
    }
    tb_font_store_right(edges, from.x, from.y, 0.0f, y);
    tb_font_store_right(edges, 0.0f, y, to.x, to.y);
}

static void tb_font_store_curve(tb_font_edges *edges, tb_font_pixel_point from,
                                tb_font_pixel_point control, tb_font_pixel_point to)
{
    if (!tb_font_finite(from) || !tb_font_finite(control) || !tb_font_finite(to))
    {
        return;
    }
    const size_t lines = tb_font_curve_lines(from, control, to);
    tb_font_pixel_point previous = from;
    for (size_t step = 1; step <= lines; step++)
    {
        const float t = (float)step / (float)lines;
        const float u = 1.0f - t;
        tb_font_pixel_point point = to;
        if (step < lines)
        {
            point.x = u * u * from.x + 2.0f * u * t * control.x + t * t * to.x;
            point.y = u * u * from.y + 2.0f * u * t * control.y + t * t * to.y;
        }
        tb_font_store_line(edges, previous, point);
        previous = point;
    }
}

static void tb_font_store_outline(tb_font_edges *edges, const tb_font_outline *outline,
                                  const tb_font_raster_placement *placement)
{
    for (size_t contour_index = 0; contour_index < outline->contour_count; contour_index++)
    {
        const tb_font_contour *contour = &outline->contours[contour_index];
        const tb_font_pixel_point start = tb_font_place(placement, contour->start);
        tb_font_pixel_point from = start;
        for (size_t index = 0; index < contour->segment_count; index++)
        {
            const tb_font_segment *segment = &contour->segments[index];
            const tb_font_pixel_point to = tb_font_place(placement, segment->end);
            if (segment->type == TB_FONT_QUADRATIC)
            {
                tb_font_store_curve(edges, from, tb_font_place(placement, segment->control), to);
            }
            else
            {
                tb_font_store_line(edges, from, to);
            }
            from = to;
        }
        /* A closed contour's closing line is level and empty. */
        tb_font_store_line(edges, from, start);
    }
}

/* -------------------------------------------------------------------------
 * Accumulating.
 * ------------------------------------------------------------------------- */

/* Add the area that a line, crossing one row from x = `from` to x = `to`
 * and covering `height` of it (signed by its direction), puts to its right
 * in each pixel of the row, as differences: the pixel at x gets the sum of
 * `line[0..x]`. `line` holds the bitmap's width plus 2. */
static void tb_font_accumulate_span(float *line, float from, float to, float height)
{
    const float left = fminf(from, to);
    const float right = fmaxf(from, to);
    const float left_floor = floorf(left);
    const float right_ceil = ceilf(right);
    const size_t left_cell = (size_t)left_floor;
    const size_t right_cell = (size_t)right_ceil;
    if (right_cell <= left_cell + 1)
    {
        /* Within one pixel: the area right of the line there is a trapezoid
         * whose mean width is one less the line's mean position. */
        const float middle = 0.5f * (from + to) - left_floor;
        line[left_cell] += height * (1.0f - middle);
        line[left_cell + 1] += height * middle;
        return;
    }
    /* Across pixels: the line covers `per_pixel` of the row in each whole
     * pixel it crosses. In its first pixel the area right of it is a
     * triangle; in its last, all but a triangle left of it. */
    const float per_pixel = 1.0f / (right - left);
    const float first_fraction = 1.0f - (left - left_floor);
    const float first_area = 0.5f * per_pixel * first_fraction * first_fraction;
    const float last_fraction = right - (right_ceil - 1.0f);
    const float last_area = 0.5f * per_pixel * last_fraction * last_fraction;
    line[left_cell] += height * first_area;
    if (right_cell == left_cell + 2)
    {
        line[left_cell + 1] += height * (1.0f - first_area - last_area);
    }
    else
    {
        /* The second pixel: the first's share in full, and half of its own. */
        const float second_area = per_pixel * (first_fraction + 0.5f);
        line[left_cell + 1] += height * (second_area - first_area);
        for (size_t cell = left_cell + 2; cell < right_cell - 1; cell++)
        {
            line[cell] += height * per_pixel;
        }
        const float before_last = second_area + (float)(right_cell - left_cell - 3) * per_pixel;
        line[right_cell - 1] += height * (1.0f - before_last - last_area);
    }
    line[right_cell] += height * last_area;
}

static void tb_font_accumulate_smooth(float *band, size_t stride, const tb_font_edge *edge,
                                      uint32_t band_top, uint32_t band_rows, float width)
{
    const float top = fmaxf(edge->y0, (float)band_top);
    const float bottom = fminf(edge->y1, (float)(band_top + band_rows));
    if (bottom <= top)
    {
        return;
    }
    const float slope = (edge->x1 - edge->x0) / (edge->y1 - edge->y0);
    const uint32_t first_row = (uint32_t)floorf(top);
    const uint32_t end_row = (uint32_t)ceilf(bottom);
    for (uint32_t row = first_row; row < end_row; row++)
    {
        const float row_top = fmaxf(top, (float)row);
        const float row_bottom = fminf(bottom, (float)row + 1.0f);
        if (row_bottom <= row_top)
        {
            continue;
        }
        const float from = fminf(fmaxf(edge->x0 + (row_top - edge->y0) * slope, 0.0f), width);
        const float to = fminf(fmaxf(edge->x0 + (row_bottom - edge->y0) * slope, 0.0f), width);
        float *line = band + (size_t)(row - band_top) * stride;
        tb_font_accumulate_span(line, from, to, (row_bottom - row_top) * edge->direction);
    }
}

/* A line crosses the center of each row from the one whose center is at or
 * below its top to the one before the first whose center is at or below its
 * bottom; it adds its direction at the first pixel whose center lies right
 * of where it crosses. */
static void tb_font_accumulate_aliased(float *band, size_t stride, const tb_font_edge *edge,
                                       uint32_t band_top, uint32_t band_rows, float width)
{
    const float first = fmaxf(ceilf(edge->y0 - 0.5f), (float)band_top);
    const float end = fminf(ceilf(edge->y1 - 0.5f), (float)(band_top + band_rows));
    if (end <= first)
    {
        return;
    }
    const float slope = (edge->x1 - edge->x0) / (edge->y1 - edge->y0);
    for (uint32_t row = (uint32_t)first; row < (uint32_t)end; row++)
    {
        const float x = edge->x0 + ((float)row + 0.5f - edge->y0) * slope;
        const float column = fminf(fmaxf(floorf(x - 0.5f) + 1.0f, 0.0f), width);
        band[(size_t)(row - band_top) * stride + (size_t)column] += edge->direction;
    }
}

/* -------------------------------------------------------------------------
 * Overlaps.
 * ------------------------------------------------------------------------- */

static int tb_font_compare_tops(const void *left, const void *right, void *context)
{
    (void)context;
    const tb_font_edge *a = left;
    const tb_font_edge *b = right;
    if (a->y0 < b->y0)
    {
        return -1;
    }
    return a->y0 > b->y0 ? 1 : 0;
}

/* Which side of the line through a and b the point c is: the sign of the
 * cross product. */
static float tb_font_side(float ax, float ay, float bx, float by, float cx, float cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* Whether two edges cross each other at a point inside both; edges that
 * only touch, as a contour's consecutive edges do, do not. */
static bool tb_font_edges_cross(const tb_font_edge *a, const tb_font_edge *b)
{
    const float c_side = tb_font_side(a->x0, a->y0, a->x1, a->y1, b->x0, b->y0);
    const float d_side = tb_font_side(a->x0, a->y0, a->x1, a->y1, b->x1, b->y1);
    const float a_side = tb_font_side(b->x0, b->y0, b->x1, b->y1, a->x0, a->y0);
    const float b_side = tb_font_side(b->x0, b->y0, b->x1, b->y1, a->x1, a->y1);
    const bool apart_c_d = (c_side < 0.0f && d_side > 0.0f) || (c_side > 0.0f && d_side < 0.0f);
    const bool apart_a_b = (a_side < 0.0f && b_side > 0.0f) || (a_side > 0.0f && b_side < 0.0f);
    return apart_c_d && apart_a_b;
}

/* Whether any two edges cross: then contours, or one contour with itself,
 * overlap. Sorts the edges by their tops, which the accumulation does not
 * mind, and compares each only with the edges that start above its bottom. */
static bool tb_font_any_edges_cross(tb_font_edges *edges)
{
    tb_sort(edges->edges, edges->count, sizeof(tb_font_edge), tb_font_compare_tops, NULL);
    for (size_t index = 0; index < edges->count; index++)
    {
        const tb_font_edge *edge = &edges->edges[index];
        for (size_t other = index + 1; other < edges->count && edges->edges[other].y0 <= edge->y1;
             other++)
        {
            if (tb_font_edges_cross(edge, &edges->edges[other]))
            {
                return true;
            }
        }
    }
    return false;
}

/* Sum a row's differences into coverage. Returns whether any pixel winds
 * more than once: contours wound the same way overlap there. */
static bool tb_font_resolve_row(const float *line, uint8_t *pixels, uint32_t width,
                                tb_font_raster_mode mode)
{
    float sum = 0.0f;
    float most = 0.0f;
    for (uint32_t x = 0; x < width; x++)
    {
        sum += line[x];
        const float coverage = fabsf(sum);
        most = fmaxf(most, coverage);
        if (mode == TB_FONT_RASTER_ALIASED)
        {
            /* Windings are whole numbers: any but 0 is inside. */
            pixels[x] = coverage > 0.5f ? 255 : 0;
            continue;
        }
        pixels[x] = coverage >= 1.0f ? 255 : (uint8_t)(coverage * 255.0f + 0.5f);
    }
    return most > TB_FONT_OVERLAP_THRESHOLD;
}

/* One pass of the rasterizer over a bitmap of any width: the public
 * function's limits are the caller's to keep. In the smooth mode, sets
 * `overlap` when edges cross or a pixel winds more than once; an `overlap`
 * already set skips looking for crossings. */
static tb_font_result tb_font_raster_pass(const tb_font_outline *outline,
                                          const tb_font_raster_placement *placement,
                                          tb_font_raster_mode mode,
                                          const tb_font_raster_target *target, tb_arena *scratch,
                                          bool *overlap)
{
    const size_t lines = tb_font_count_lines(outline, placement);
    if (lines > TB_FONT_RASTER_MAX_EDGES)
    {
        return TB_FONT_TOO_LARGE;
    }
    const tb_arena_mark mark = tb_arena_get_mark(scratch);
    tb_font_edges edges = {0};
    edges.capacity = lines * TB_FONT_PIECES_PER_LINE;
    edges.edges =
        tb_arena_alloc(scratch, edges.capacity * sizeof(tb_font_edge), _Alignof(tb_font_edge));
    edges.width = (float)target->width;
    tb_font_store_outline(&edges, outline, placement);
    if (mode == TB_FONT_RASTER_SMOOTH && !*overlap && tb_font_any_edges_cross(&edges))
    {
        *overlap = true;
    }

    const size_t stride = (size_t)target->width + 2;
    float *band =
        tb_arena_alloc(scratch, stride * TB_FONT_BAND_ROWS * sizeof(float), _Alignof(float));
    for (uint32_t band_top = 0; band_top < target->height; band_top += TB_FONT_BAND_ROWS)
    {
        const uint32_t rows = target->height - band_top < TB_FONT_BAND_ROWS
                                  ? target->height - band_top
                                  : TB_FONT_BAND_ROWS;
        memset(band, 0, stride * rows * sizeof(float));
        for (size_t index = 0; index < edges.count; index++)
        {
            if (mode == TB_FONT_RASTER_ALIASED)
            {
                tb_font_accumulate_aliased(band, stride, &edges.edges[index], band_top, rows,
                                           edges.width);
            }
            else
            {
                tb_font_accumulate_smooth(band, stride, &edges.edges[index], band_top, rows,
                                          edges.width);
            }
        }
        for (uint32_t row = 0; row < rows; row++)
        {
            uint8_t *pixels = target->pixels + (size_t)(band_top + row) * target->stride;
            if (tb_font_resolve_row(band + (size_t)row * stride, pixels, target->width, mode))
            {
                *overlap = true;
            }
        }
    }
    tb_arena_rewind(scratch, mark);
    return TB_FONT_OK;
}

/* Draw again at TB_FONT_OVERSAMPLE times the size each way, and average
 * each block of subpixels into a pixel, a band of rows at a time.
 *
 * Summing signed areas is the non-zero rule only where one contour covers a
 * pixel: where the edges of two contours wound the same way both cross a
 * pixel, it adds their areas, though they cover some of the pixel twice, and
 * the pixel comes out too dark. Overlapping contours are common (Roboto
 * draws a T as its bar and its stem), and drawing their glyphs finer keeps
 * the error to the subpixels that both edges cross, as FreeType does for the
 * glyphs that a font marks as overlapping. */
static void tb_font_oversample(const tb_font_outline *outline,
                               const tb_font_raster_placement *placement,
                               const tb_font_raster_target *target, tb_arena *scratch)
{
    const uint32_t factor = TB_FONT_OVERSAMPLE;
    const tb_arena_mark mark = tb_arena_get_mark(scratch);
    const uint32_t fine_width = target->width * factor;
    uint8_t *fine =
        tb_arena_alloc(scratch, (size_t)fine_width * factor * TB_FONT_OVERSAMPLED_ROWS, 1);
    for (uint32_t top = 0; top < target->height; top += TB_FONT_OVERSAMPLED_ROWS)
    {
        const uint32_t rows = target->height - top < TB_FONT_OVERSAMPLED_ROWS
                                  ? target->height - top
                                  : TB_FONT_OVERSAMPLED_ROWS;
        const tb_font_raster_placement finer = {placement->scale * (float)factor,
                                                placement->x * (float)factor,
                                                (placement->y - (float)top) * (float)factor};
        const tb_font_raster_target band = {fine, fine_width, rows * factor, fine_width};
        bool overlap = true;
        if (tb_font_raster_pass(outline, &finer, TB_FONT_RASTER_SMOOTH, &band, scratch, &overlap) !=
            TB_FONT_OK)
        {
            /* Too many lines at the finer size: keep the coarser drawing. */
            break;
        }
        for (uint32_t row = 0; row < rows; row++)
        {
            uint8_t *pixels = target->pixels + (size_t)(top + row) * target->stride;
            for (uint32_t x = 0; x < target->width; x++)
            {
                uint32_t sum = 0;
                for (uint32_t y = 0; y < factor; y++)
                {
                    const uint8_t *block =
                        fine + (size_t)(row * factor + y) * fine_width + (size_t)x * factor;
                    for (uint32_t sub = 0; sub < factor; sub++)
                    {
                        sum += block[sub];
                    }
                }
                const uint32_t samples = factor * factor;
                pixels[x] = (uint8_t)((sum + samples / 2) / samples);
            }
        }
    }
    tb_arena_rewind(scratch, mark);
}

tb_font_result tb_font_rasterize_outline(const tb_font_outline *outline,
                                         const tb_font_raster_placement *placement,
                                         tb_font_raster_mode mode,
                                         const tb_font_raster_target *target, tb_arena *scratch)
{
    TB_ASSERT(outline != NULL);
    TB_ASSERT(placement != NULL);
    TB_ASSERT(target != NULL);
    TB_ASSERT(scratch != NULL);
    TB_ASSERT(mode == TB_FONT_RASTER_SMOOTH || mode == TB_FONT_RASTER_ALIASED);
    TB_ASSERT(target->width <= TB_FONT_RASTER_MAX_SIDE);
    TB_ASSERT(target->height <= TB_FONT_RASTER_MAX_SIDE);
    TB_ASSERT(target->stride >= target->width);
    if (target->width == 0 || target->height == 0)
    {
        return TB_FONT_OK;
    }
    TB_ASSERT(target->pixels != NULL);
    bool overlap = false;
    const tb_font_result result =
        tb_font_raster_pass(outline, placement, mode, target, scratch, &overlap);
    if (result == TB_FONT_OK && overlap && mode == TB_FONT_RASTER_SMOOTH)
    {
        tb_font_oversample(outline, placement, target, scratch);
    }
    return result;
}

/* -------------------------------------------------------------------------
 * Glyphs.
 * ------------------------------------------------------------------------- */

typedef struct tb_font_bounds
{
    float left;
    float top;
    float right;
    float bottom;
    bool any;
} tb_font_bounds;

static void tb_font_bound(tb_font_bounds *bounds, tb_font_point point, float scale, float shift)
{
    const float x = point.x * scale + shift;
    const float y = point.y * scale;
    if (!isfinite(x) || !isfinite(y))
    {
        return;
    }
    if (!bounds->any)
    {
        *bounds = (tb_font_bounds){x, y, x, y, true};
        return;
    }
    bounds->left = fminf(bounds->left, x);
    bounds->right = fmaxf(bounds->right, x);
    bounds->top = fmaxf(bounds->top, y);
    bounds->bottom = fminf(bounds->bottom, y);
}

/* The box of whole pixels around every point of an outline, y up. */
static tb_font_bounds tb_font_outline_bounds(const tb_font_outline *outline, float scale,
                                             float shift)
{
    tb_font_bounds bounds = {0};
    for (size_t contour_index = 0; contour_index < outline->contour_count; contour_index++)
    {
        const tb_font_contour *contour = &outline->contours[contour_index];
        tb_font_bound(&bounds, contour->start, scale, shift);
        for (size_t index = 0; index < contour->segment_count; index++)
        {
            tb_font_bound(&bounds, contour->segments[index].control, scale, shift);
            tb_font_bound(&bounds, contour->segments[index].end, scale, shift);
        }
    }
    bounds.left = floorf(bounds.left);
    bounds.bottom = floorf(bounds.bottom);
    bounds.right = ceilf(bounds.right);
    bounds.top = ceilf(bounds.top);
    return bounds;
}

/* Measure an outline that has been read: the bitmap's box, and the advance. */
static tb_font_result tb_font_measure_outline(const tb_font *font, uint16_t glyph,
                                              const tb_font_outline *outline,
                                              const tb_font_raster_options *options,
                                              tb_font_glyph_image *image, tb_font_error *error)
{
    const float scale = tb_font_scale(font, options->pixels_per_em);
    tb_font_glyph_metrics metrics = {0};
    tb_font_get_glyph_metrics(font, glyph, &metrics);
    *image = (tb_font_glyph_image){0};
    image->advance = (float)metrics.advance_width * scale;
    image->left_side_bearing = (float)metrics.left_side_bearing * scale;
    if (scale == 0.0f)
    {
        return TB_FONT_OK;
    }
    const float shift = tb_font_raster_subpixel(options->subpixel_x);
    const tb_font_bounds bounds = tb_font_outline_bounds(outline, scale, shift);
    if (!bounds.any)
    {
        return TB_FONT_OK;
    }
    const float limit = TB_FONT_MAX_OFFSET;
    if (fabsf(bounds.left) > limit || fabsf(bounds.right) > limit || fabsf(bounds.top) > limit ||
        fabsf(bounds.bottom) > limit)
    {
        return tb_font_fail(error, TB_FONT_TOO_LARGE, "", "the glyph lies too far from its origin");
    }
    const float width = bounds.right - bounds.left;
    const float height = bounds.top - bounds.bottom;
    if (width > (float)TB_FONT_RASTER_MAX_SIDE || height > (float)TB_FONT_RASTER_MAX_SIDE)
    {
        return tb_font_fail(error, TB_FONT_TOO_LARGE, "",
                            "the glyph's bitmap is larger than the limit");
    }
    if (width == 0.0f || height == 0.0f)
    {
        /* A glyph of no area, such as a single line, draws nothing. */
        return TB_FONT_OK;
    }
    image->width = (uint32_t)width;
    image->height = (uint32_t)height;
    image->left = (int32_t)bounds.left;
    image->top = (int32_t)bounds.top;
    return TB_FONT_OK;
}

/* Draw a measured outline into new pixels from the arena. */
static tb_font_result tb_font_draw_outline(const tb_font *font, const tb_font_outline *outline,
                                           const tb_font_raster_options *options, tb_arena *arena,
                                           tb_font_glyph_image *image, tb_font_error *error)
{
    if (image->width == 0)
    {
        return TB_FONT_OK;
    }
    const size_t size = (size_t)image->width * image->height;
    image->pixels = tb_arena_alloc(arena, size, 1);
    const tb_font_raster_placement placement = {
        tb_font_scale(font, options->pixels_per_em),
        tb_font_raster_subpixel(options->subpixel_x) - (float)image->left, (float)image->top};
    const tb_font_raster_target target = {image->pixels, image->width, image->height, image->width};
    const tb_font_result result =
        tb_font_rasterize_outline(outline, &placement, options->mode, &target, arena);
    if (result != TB_FONT_OK)
    {
        return tb_font_fail(error, result, "",
                            "the outline has more lines than the limit once its curves are cut");
    }
    return TB_FONT_OK;
}

static tb_font_result tb_font_measure_or_draw(const tb_font *font, uint16_t glyph,
                                              const tb_font_raster_options *options,
                                              tb_arena *arena, bool draw,
                                              tb_font_glyph_image *image, tb_font_error *error)
{
    TB_ASSERT(font != NULL);
    TB_ASSERT(options != NULL);
    TB_ASSERT(arena != NULL);
    TB_ASSERT(image != NULL);
    TB_ASSERT(options->mode == TB_FONT_RASTER_SMOOTH || options->mode == TB_FONT_RASTER_ALIASED);
    *image = (tb_font_glyph_image){0};
    tb_font_error local_error = {TB_FONT_OK, "", ""};
    const tb_arena_mark mark = tb_arena_get_mark(arena);
    tb_font_outline outline = {0};
    tb_font_result result = tb_font_get_outline(font, glyph, arena, &outline, &local_error);
    if (result != TB_FONT_OK)
    {
        goto done;
    }
    result = tb_font_measure_outline(font, glyph, &outline, options, image, &local_error);
    if (result != TB_FONT_OK || !draw)
    {
        goto done;
    }
    result = tb_font_draw_outline(font, &outline, options, arena, image, &local_error);

done:
    if (result != TB_FONT_OK || !draw)
    {
        tb_arena_rewind(arena, mark);
    }
    if (result != TB_FONT_OK)
    {
        *image = (tb_font_glyph_image){0};
    }
    if (error != NULL)
    {
        *error = local_error;
    }
    return result;
}

tb_font_result tb_font_measure_glyph(const tb_font *font, uint16_t glyph,
                                     const tb_font_raster_options *options, tb_arena *arena,
                                     tb_font_glyph_image *image, tb_font_error *error)
{
    return tb_font_measure_or_draw(font, glyph, options, arena, false, image, error);
}

tb_font_result tb_font_rasterize_glyph(const tb_font *font, uint16_t glyph,
                                       const tb_font_raster_options *options, tb_arena *arena,
                                       tb_font_glyph_image *image, tb_font_error *error)
{
    return tb_font_measure_or_draw(font, glyph, options, arena, true, image, error);
}
