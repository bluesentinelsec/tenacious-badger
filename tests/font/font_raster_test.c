/* The rasterizer: coverage checked against what the outlines are, worked
 * out here by other means (the area of a contour by Green's theorem, and
 * pixel fonts' glyphs as a separate reader in Python sampled them), and
 * outlines made by hand whose coverage is known exactly. */
#include "badger/arena.h"
#include "badger/font.h"
#include "badger/font_raster.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "font_builder.h"
#include "tb_test.h"
#include "test_data.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

#define FONT_BUFFER ((size_t)65536)

static tb_font *open_font(const char *name)
{
    const tb_test_file *found = NULL;
    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        if (tb_str_equal(tb_font_files[index].name, name))
        {
            found = &tb_font_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(found->bytes, found->length, NULL, &font, NULL), ==,
                         TB_FONT_OK);
    return font;
}

/* Twice the signed area of a contour, in font units, by Green's theorem: the
 * shoelace sum of its chords, and for each curve the area between it and
 * its chord, which for a quadratic Bezier curve is two thirds of the
 * triangle of its ends and control point. */
static double contour_area(const tb_font_contour *contour)
{
    double twice = 0.0;
    double x0 = (double)contour->start.x;
    double y0 = (double)contour->start.y;
    for (size_t index = 0; index < contour->segment_count; index++)
    {
        const tb_font_segment *segment = &contour->segments[index];
        const double x1 = (double)segment->end.x;
        const double y1 = (double)segment->end.y;
        twice += x0 * y1 - x1 * y0;
        if (segment->type == TB_FONT_QUADRATIC)
        {
            const double cx = (double)segment->control.x;
            const double cy = (double)segment->control.y;
            const double triangle = (cx - x0) * (y1 - y0) - (x1 - x0) * (cy - y0);
            twice += 2.0 / 3.0 * triangle;
        }
        x0 = x1;
        y0 = y1;
    }
    return twice / 2.0;
}

static double outline_area(const tb_font_outline *outline)
{
    double area = 0.0;
    for (size_t index = 0; index < outline->contour_count; index++)
    {
        area += contour_area(&outline->contours[index]);
    }
    return fabs(area);
}

static double coverage_of(const tb_font_glyph_image *image)
{
    uint64_t sum = 0;
    for (size_t index = 0; index < (size_t)image->width * image->height; index++)
    {
        sum += image->pixels[index];
    }
    return (double)sum / 255.0;
}

/* -------------------------------------------------------------------------
 * Measures.
 * ------------------------------------------------------------------------- */

TB_TEST(font_raster, scales_the_measures_of_lines_by_the_em)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_font *px437 = open_font("Px437_IBM_VGA_8x16.ttf");
    /* Roboto has 2048 units to the em, an ascender of 1900, a descender of
     * -500, and no line gap. */
    TB_TEST_EXPECT_FLOAT(tb_font_scale(roboto, 2048.0f), ==, 1.0f);
    tb_font_line_metrics lines = {0};
    tb_font_get_line_metrics(roboto, 20.48f, &lines);
    TB_TEST_EXPECT_FLOAT_NEAR(lines.ascent, 19.0, 1e-4);
    TB_TEST_EXPECT_FLOAT_NEAR(lines.descent, -5.0, 1e-4);
    TB_TEST_EXPECT_FLOAT(lines.line_gap, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(lines.line_height, 24.0, 1e-4);
    /* The VGA font is 16 pixels to the em, 12 above the baseline and 4 below. */
    tb_font_get_line_metrics(px437, 16.0f, &lines);
    TB_TEST_EXPECT_FLOAT(lines.ascent, ==, 12.0f);
    TB_TEST_EXPECT_FLOAT(lines.descent, ==, -4.0f);
    TB_TEST_EXPECT_FLOAT(lines.line_height, ==, 16.0f);
    tb_font_close(px437);
    tb_font_close(roboto);
}

TB_TEST(font_raster, sizes_that_are_not_positive_and_finite_draw_nothing_and_huge_ones_clamp)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    const float nothing[] = {0.0f, -12.0f, NAN, INFINITY, -INFINITY};
    tb_arena *arena = tb_arena_create(0);
    const uint16_t glyph = tb_font_find_glyph(roboto, 'W');
    for (size_t index = 0; index < TB_TEST_COUNT_OF(nothing); index++)
    {
        TB_TEST_CONTEXT("size %g", (double)nothing[index]);
        TB_TEST_EXPECT_FLOAT(tb_font_scale(roboto, nothing[index]), ==, 0.0f);
        tb_font_line_metrics lines = {0};
        tb_font_get_line_metrics(roboto, nothing[index], &lines);
        TB_TEST_EXPECT_FLOAT(lines.line_height, ==, 0.0f);
        const tb_font_raster_options options = {nothing[index], 0.0f, TB_FONT_RASTER_SMOOTH};
        tb_font_glyph_image image = {0};
        TB_TEST_EXPECT_UINT(tb_font_rasterize_glyph(roboto, glyph, &options, arena, &image, NULL),
                            ==, TB_FONT_OK);
        TB_TEST_EXPECT_UINT(image.width, ==, 0);
        TB_TEST_EXPECT_UINT(image.height, ==, 0);
        TB_TEST_EXPECT_POINTER(image.pixels, ==, NULL);
        TB_TEST_EXPECT_FLOAT(image.advance, ==, 0.0f);
    }
    /* A size past the limit is the limit: the same box, without drawing. */
    const tb_font_raster_options huge = {1.0e9f, 0.0f, TB_FONT_RASTER_SMOOTH};
    const tb_font_raster_options limit = {TB_FONT_RASTER_MAX_PIXELS_PER_EM, 0.0f,
                                          TB_FONT_RASTER_SMOOTH};
    tb_font_glyph_image clamped = {0};
    tb_font_glyph_image at_limit = {0};
    tb_arena_reset(arena);
    const size_t remaining = tb_arena_remaining(arena);
    TB_TEST_REQUIRE_UINT(tb_font_measure_glyph(roboto, glyph, &huge, arena, &clamped, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(tb_font_measure_glyph(roboto, glyph, &limit, arena, &at_limit, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(clamped.width, ==, at_limit.width);
    TB_TEST_EXPECT_UINT(clamped.height, ==, at_limit.height);
    TB_TEST_EXPECT_INT(clamped.left, ==, at_limit.left);
    TB_TEST_EXPECT_FLOAT(clamped.advance, ==, at_limit.advance);
    TB_TEST_EXPECT_UINT(clamped.width, >, 1000);
    TB_TEST_EXPECT_POINTER(clamped.pixels, ==, NULL);
    /* Measuring leaves the arena as it was. */
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, remaining);
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

TB_TEST(font_raster, a_blank_glyph_has_an_advance_and_no_bitmap)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    const uint16_t space = tb_font_find_glyph(roboto, ' ');
    tb_font_glyph_metrics metrics = {0};
    tb_font_get_glyph_metrics(roboto, space, &metrics);
    const tb_font_raster_options options = {32.0f, 0.5f, TB_FONT_RASTER_SMOOTH};
    tb_font_glyph_image image = {0};
    tb_font_error error = {0};
    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, space, &options, arena, &image, &error),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    TB_TEST_EXPECT_UINT(image.width, ==, 0);
    TB_TEST_EXPECT_UINT(image.height, ==, 0);
    TB_TEST_EXPECT_POINTER(image.pixels, ==, NULL);
    TB_TEST_EXPECT_FLOAT_NEAR(image.advance, metrics.advance_width * 32.0 / 2048.0, 1e-4);
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

TB_TEST(font_raster, the_bitmap_holds_the_glyph_where_its_metrics_say)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    const uint16_t glyph = tb_font_find_glyph(roboto, 'H');
    tb_font_glyph_metrics metrics = {0};
    tb_font_get_glyph_metrics(roboto, glyph, &metrics);
    const float size = 40.0f;
    const double scale = (double)size / 2048.0;
    const tb_font_raster_options options = {size, 0.0f, TB_FONT_RASTER_SMOOTH};
    tb_font_glyph_image measured = {0};
    TB_TEST_REQUIRE_UINT(tb_font_measure_glyph(roboto, glyph, &options, arena, &measured, NULL), ==,
                         TB_FONT_OK);
    tb_font_glyph_image image = {0};
    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, glyph, &options, arena, &image, NULL), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(image.width, ==, measured.width);
    TB_TEST_EXPECT_UINT(image.height, ==, measured.height);
    TB_TEST_EXPECT_INT(image.left, ==, measured.left);
    TB_TEST_EXPECT_INT(image.top, ==, measured.top);
    TB_TEST_EXPECT_FLOAT_NEAR(image.advance, metrics.advance_width * scale, 1e-4);
    TB_TEST_EXPECT_FLOAT_NEAR(image.left_side_bearing, metrics.left_side_bearing * scale, 1e-4);
    /* An H sits on the baseline, its top at the cap height of 1456 units,
     * and its left at its side bearing: the box is the next whole pixels
     * out from those. */
    TB_TEST_EXPECT_INT(image.top, ==, (int32_t)ceil(1456.0 * scale));
    TB_TEST_EXPECT_INT(image.top - (int32_t)image.height, ==, 0);
    TB_TEST_EXPECT_INT(image.left, ==, (int32_t)floor(metrics.left_side_bearing * scale));
    /* Every edge row and column of the box has some coverage: it is tight. */
    bool top_row = false;
    bool bottom_row = false;
    bool left_column = false;
    bool right_column = false;
    for (uint32_t x = 0; x < image.width; x++)
    {
        top_row = top_row || image.pixels[x] != 0;
        bottom_row = bottom_row || image.pixels[(size_t)(image.height - 1) * image.width + x] != 0;
    }
    for (uint32_t y = 0; y < image.height; y++)
    {
        left_column = left_column || image.pixels[(size_t)y * image.width] != 0;
        right_column = right_column || image.pixels[(size_t)y * image.width + image.width - 1] != 0;
    }
    TB_TEST_EXPECT(top_row && bottom_row && left_column && right_column);
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

/* -------------------------------------------------------------------------
 * Coverage of real glyphs.
 * ------------------------------------------------------------------------- */

/* The length of an outline's lines and control polygons, in font units:
 * at least the length of its contours. */
static double outline_perimeter(const tb_font_outline *outline)
{
    double length = 0.0;
    for (size_t contour_index = 0; contour_index < outline->contour_count; contour_index++)
    {
        const tb_font_contour *contour = &outline->contours[contour_index];
        tb_font_point from = contour->start;
        for (size_t index = 0; index < contour->segment_count; index++)
        {
            const tb_font_segment *segment = &contour->segments[index];
            if (segment->type == TB_FONT_QUADRATIC)
            {
                length += hypot((double)segment->control.x - (double)from.x,
                                (double)segment->control.y - (double)from.y);
                from = segment->control;
            }
            length += hypot((double)segment->end.x - (double)from.x,
                            (double)segment->end.y - (double)from.y);
            from = segment->end;
        }
    }
    return length;
}

/* Roboto draws many glyphs of contours that overlap, or of one contour that
 * crosses itself, such as the bar and the stem of a T and the loop of an e,
 * whose areas added count the overlap twice: the reference rasterizer below
 * covers those. These have no overlaps. */
#define ROBOTO_SIMPLE_GLYPHS "CGIJLNOSUcilos01"

typedef struct area_case
{
    const char *file;
    float size;
    float subpixel_x;
} area_case;

TB_TEST(font_raster, the_coverage_of_a_glyph_adds_up_to_the_area_of_its_outline)
{
    /* Summed, the coverage of every pixel is the area inside the outline.
     * It differs by the rounding of each pixel to 1/255, and by cutting
     * curves into lines that stray at most a twentieth of a pixel inward,
     * which loses at most two thirds of that along the outline's length. */
    const area_case cases[] = {
        {"Roboto-Regular.ttf", 9.0f, 0.0f},      {"Roboto-Regular.ttf", 13.0f, 0.37f},
        {"Roboto-Regular.ttf", 24.0f, 0.75f},    {"Roboto-Regular.ttf", 61.5f, 0.2f},
        {"Px437_IBM_VGA_8x16.ttf", 16.0f, 0.0f}, {"Px437_IBM_VGA_8x16.ttf", 21.3f, 0.6f},
        {"Silkscreen-Regular.ttf", 8.0f, 0.0f},  {"Silkscreen-Regular.ttf", 13.7f, 0.5f},
    };
    tb_arena *arena = tb_arena_create(0);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        tb_font *font = open_font(cases[index].file);
        const double scale = (double)tb_font_scale(font, cases[index].size);
        for (uint32_t code_point = '!'; code_point <= '~'; code_point++)
        {
            TB_TEST_CONTEXT("%s at %g: U+%04X", cases[index].file, (double)cases[index].size,
                            (unsigned)code_point);
            const uint16_t glyph = tb_font_find_glyph(font, code_point);
            const tb_arena_mark mark = tb_arena_get_mark(arena);
            tb_font_outline outline = {0};
            TB_TEST_REQUIRE_UINT(tb_font_get_outline(font, glyph, arena, &outline, NULL), ==,
                                 TB_FONT_OK);
            if (tb_str_equal(cases[index].file, "Roboto-Regular.ttf") &&
                tb_str_find_char(ROBOTO_SIMPLE_GLYPHS, 0, TB_STR_END, (char)code_point) ==
                    TB_STR_NOT_FOUND)
            {
                tb_arena_rewind(arena, mark);
                continue;
            }
            const double expected = outline_area(&outline) * scale * scale;
            const double perimeter = outline_perimeter(&outline) * scale;
            const tb_font_raster_options options = {cases[index].size, cases[index].subpixel_x,
                                                    TB_FONT_RASTER_SMOOTH};
            tb_font_glyph_image image = {0};
            TB_TEST_REQUIRE_UINT(
                tb_font_rasterize_glyph(font, glyph, &options, arena, &image, NULL), ==,
                TB_FONT_OK);
            const double pixels = (double)image.width * image.height;
            TB_TEST_EXPECT_FLOAT_NEAR(coverage_of(&image), expected,
                                      0.04 * perimeter + 0.002 * pixels + 0.01);
            tb_arena_rewind(arena, mark);
        }
        tb_font_close(font);
    }
    tb_arena_destroy(arena);
}

/* -------------------------------------------------------------------------
 * A reference rasterizer: slow and simple, and sharing nothing with the
 * engine's. It cuts curves into many more lines, and samples each pixel row
 * at many heights; at each, it sorts where the lines cross, walks them left
 * to right counting windings, and adds the lengths where the winding is not
 * zero to the pixels they span.
 * ------------------------------------------------------------------------- */

#define REFERENCE_CURVE_LINES 32
#define REFERENCE_SAMPLES_PER_ROW 16

typedef struct reference_line
{
    double x0;
    double y0;
    double x1;
    double y1;
} reference_line;

typedef struct reference_crossing
{
    double x;
    int winding;
} reference_crossing;

typedef struct reference
{
    reference_line *lines;
    size_t count;
    size_t capacity;
    double scale;
    double x; /* Where the font's origin lands in the bitmap. */
    double y;
} reference;

static void reference_add(reference *made, double x0, double y0, double x1, double y1)
{
    TB_TEST_REQUIRE_UINT(made->count, <, made->capacity);
    const reference_line line = {made->x + x0 * made->scale, made->y - y0 * made->scale,
                                 made->x + x1 * made->scale, made->y - y1 * made->scale};
    made->lines[made->count] = line;
    made->count++;
}

static void reference_lines(reference *made, const tb_font_outline *outline)
{
    for (size_t contour_index = 0; contour_index < outline->contour_count; contour_index++)
    {
        const tb_font_contour *contour = &outline->contours[contour_index];
        double x = (double)contour->start.x;
        double y = (double)contour->start.y;
        for (size_t index = 0; index < contour->segment_count; index++)
        {
            const tb_font_segment *segment = &contour->segments[index];
            const double end_x = (double)segment->end.x;
            const double end_y = (double)segment->end.y;
            if (segment->type == TB_FONT_LINE)
            {
                reference_add(made, x, y, end_x, end_y);
                x = end_x;
                y = end_y;
                continue;
            }
            double previous_x = x;
            double previous_y = y;
            for (int step = 1; step <= REFERENCE_CURVE_LINES; step++)
            {
                const double t = (double)step / REFERENCE_CURVE_LINES;
                const double u = 1.0 - t;
                const double next_x =
                    u * u * x + 2.0 * u * t * (double)segment->control.x + t * t * end_x;
                const double next_y =
                    u * u * y + 2.0 * u * t * (double)segment->control.y + t * t * end_y;
                reference_add(made, previous_x, previous_y, next_x, next_y);
                previous_x = next_x;
                previous_y = next_y;
            }
            x = end_x;
            y = end_y;
        }
    }
}

/* Add the length from `from` to `to` of one sample row to the pixels it spans. */
static void reference_span(double *row, uint32_t width, double from, double to, double weight)
{
    for (uint32_t column = 0; column < width; column++)
    {
        const double left = fmax(from, (double)column);
        const double right = fmin(to, (double)column + 1.0);
        if (right > left)
        {
            row[column] += (right - left) * weight;
        }
    }
}

static void reference_coverage(const reference *made, uint32_t width, uint32_t height,
                               double *coverage)
{
    reference_crossing *crossings = tb_calloc(made->count + 1, sizeof(reference_crossing));
    for (uint32_t row = 0; row < height; row++)
    {
        for (int sample = 0; sample < REFERENCE_SAMPLES_PER_ROW; sample++)
        {
            const double y = row + (sample + 0.5) / REFERENCE_SAMPLES_PER_ROW;
            size_t count = 0;
            for (size_t index = 0; index < made->count; index++)
            {
                const reference_line *line = &made->lines[index];
                const bool down = line->y0 <= y && y < line->y1;
                const bool up = line->y1 <= y && y < line->y0;
                if (!down && !up)
                {
                    continue;
                }
                const double t = (y - line->y0) / (line->y1 - line->y0);
                /* Insert in order of x. */
                const reference_crossing crossing = {line->x0 + t * (line->x1 - line->x0),
                                                     down ? 1 : -1};
                size_t at = count;
                while (at > 0 && crossings[at - 1].x > crossing.x)
                {
                    crossings[at] = crossings[at - 1];
                    at--;
                }
                crossings[at] = crossing;
                count++;
            }
            int winding = 0;
            for (size_t index = 0; index + 1 < count; index++)
            {
                winding += crossings[index].winding;
                if (winding != 0)
                {
                    reference_span(coverage + (size_t)row * width, width, crossings[index].x,
                                   crossings[index + 1].x, 1.0 / REFERENCE_SAMPLES_PER_ROW);
                }
            }
        }
    }
    tb_free(crossings);
}

TB_TEST(font_raster, the_coverage_of_each_pixel_matches_a_reference_rasterizer)
{
    /* Every printable character of Roboto, overlapping contours and all,
     * at small sizes where every pixel counts and at one larger. The
     * reference errs by up to 1/32 of a pixel at a level edge, sampling 16
     * heights a row; the engine's lines stray up to 1/20 of a pixel from its
     * curves, and where contours overlap it errs by up to 1/16 of a pixel
     * (see tb_font_oversample()). The worst found is 15 of 255. */
    const float sizes[] = {9.0f, 13.0f, 28.5f};
    const float fractions[] = {0.0f, 0.37f, 0.8f};
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    for (size_t size = 0; size < TB_TEST_COUNT_OF(sizes); size++)
    {
        const double scale = (double)tb_font_scale(roboto, sizes[size]);
        for (uint32_t code_point = '!'; code_point <= '~'; code_point++)
        {
            TB_TEST_CONTEXT("U+%04X at %g", (unsigned)code_point, (double)sizes[size]);
            const uint16_t glyph = tb_font_find_glyph(roboto, code_point);
            const tb_font_raster_options options = {sizes[size], fractions[size],
                                                    TB_FONT_RASTER_SMOOTH};
            tb_font_glyph_image image = {0};
            TB_TEST_REQUIRE_UINT(
                tb_font_rasterize_glyph(roboto, glyph, &options, arena, &image, NULL), ==,
                TB_FONT_OK);
            tb_font_outline outline = {0};
            TB_TEST_REQUIRE_UINT(tb_font_get_outline(roboto, glyph, arena, &outline, NULL), ==,
                                 TB_FONT_OK);
            reference made = {0};
            made.capacity = 0;
            for (size_t index = 0; index < outline.contour_count; index++)
            {
                made.capacity += outline.contours[index].segment_count * REFERENCE_CURVE_LINES;
            }
            made.lines = tb_calloc(made.capacity + 1, sizeof(reference_line));
            made.scale = scale;
            made.x = (double)fractions[size] - (double)image.left;
            made.y = image.top;
            reference_lines(&made, &outline);
            const size_t pixels = (size_t)image.width * image.height;
            double *expected = tb_calloc(pixels + 1, sizeof(double));
            reference_coverage(&made, image.width, image.height, expected);
            for (size_t index = 0; index < pixels; index++)
            {
                const double want = fmin(expected[index], 1.0) * 255.0;
                TB_TEST_EXPECT_FLOAT_NEAR(image.pixels[index], want, 20.0);
            }
            tb_free(expected);
            tb_free(made.lines);
            tb_arena_reset(arena);
        }
    }
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

/* Whether a column of a bitmap is full from row `first` to row `end`. */
static bool column_is_full(const tb_font_glyph_image *image, uint32_t column, uint32_t first,
                           uint32_t end)
{
    for (uint32_t row = first; row < end; row++)
    {
        if (image->pixels[(size_t)row * image->width + column] != 255)
        {
            return false;
        }
    }
    return true;
}

TB_TEST(font_raster, a_vertical_stem_drawn_on_whole_pixels_has_full_columns)
{
    /* The stems of l and I are wider than two pixels at these sizes, so
     * at least one column of the bitmap lies wholly inside them, top to
     * bottom but for the rows that the ends of the stem cut. */
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    const char letters[] = {'l', 'I'};
    const float sizes[] = {32.0f, 48.0f, 100.0f};
    for (size_t letter = 0; letter < TB_TEST_COUNT_OF(letters); letter++)
    {
        for (size_t size = 0; size < TB_TEST_COUNT_OF(sizes); size++)
        {
            TB_TEST_CONTEXT("%c at %g", letters[letter], (double)sizes[size]);
            const tb_font_raster_options options = {sizes[size], 0.0f, TB_FONT_RASTER_SMOOTH};
            tb_font_glyph_image image = {0};
            TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(
                                     roboto, tb_font_find_glyph(roboto, (uint32_t)letters[letter]),
                                     &options, arena, &image, NULL),
                                 ==, TB_FONT_OK);
            TB_TEST_REQUIRE_UINT(image.height, >, 2);
            size_t full = 0;
            for (uint32_t column = 0; column < image.width; column++)
            {
                full += column_is_full(&image, column, 1, image.height - 1) ? 1u : 0u;
            }
            TB_TEST_EXPECT_UINT(full, >=, 1);
            tb_arena_reset(arena);
        }
    }
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

/* The x of the center of a bitmap's coverage, from the pen. */
static double center_of(const tb_font_glyph_image *image)
{
    double moment = 0.0;
    double total = 0.0;
    for (uint32_t y = 0; y < image->height; y++)
    {
        for (uint32_t x = 0; x < image->width; x++)
        {
            const double coverage = image->pixels[(size_t)y * image->width + x];
            moment += coverage * ((double)image->left + x + 0.5);
            total += coverage;
        }
    }
    return moment / total;
}

TB_TEST(font_raster, a_subpixel_offset_moves_the_glyph_right_by_that_fraction)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    const uint16_t glyph = tb_font_find_glyph(roboto, 'o');
    const tb_font_raster_options whole = {20.0f, 0.0f, TB_FONT_RASTER_SMOOTH};
    tb_font_glyph_image at_whole = {0};
    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, glyph, &whole, arena, &at_whole, NULL), ==,
                         TB_FONT_OK);
    const float offsets[] = {0.25f, 0.5f, 0.75f, 3.5f, -0.25f};
    const double expected_shift[] = {0.25, 0.5, 0.75, 0.5, 0.75};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(offsets); index++)
    {
        TB_TEST_CONTEXT("offset %g", (double)offsets[index]);
        const tb_font_raster_options shifted = {20.0f, offsets[index], TB_FONT_RASTER_SMOOTH};
        tb_font_glyph_image image = {0};
        TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, glyph, &shifted, arena, &image, NULL),
                             ==, TB_FONT_OK);
        TB_TEST_EXPECT_FLOAT_NEAR(center_of(&image) - center_of(&at_whole), expected_shift[index],
                                  0.02);
        TB_TEST_EXPECT_FLOAT_NEAR(coverage_of(&image), coverage_of(&at_whole), 0.5);
    }
    /* An offset that is not finite is no offset. */
    const tb_font_raster_options not_finite = {20.0f, NAN, TB_FONT_RASTER_SMOOTH};
    tb_font_glyph_image image = {0};
    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, glyph, &not_finite, arena, &image, NULL),
                         ==, TB_FONT_OK);
    TB_TEST_EXPECT_BYTES(image.pixels, at_whole.pixels, (size_t)image.width * image.height);
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

/* -------------------------------------------------------------------------
 * Pixel fonts, aliased.
 * ------------------------------------------------------------------------- */

typedef struct pixel_case
{
    const char *file;
    float size;
    uint32_t code_point;
    int32_t ascent; /* Rows of the cell above the baseline. */
    uint32_t height;
    /* Whether every edge of the outline lies on a pixel edge, so that the
     * smooth mode draws the same pixels. Silkscreen's A has a counter whose
     * top is 5 units short of one. */
    bool on_pixel_edges;
    const char *rows[16];
} pixel_case;

/* The cells that tools/font_pixel_goldens.py printed, sampling each pixel's
 * center in the outlines it read itself:
 *
 *     python tools/font_pixel_goldens.py tests/data/fonts/Px437_IBM_VGA_8x16.ttf 16 8 16 12 Ag
 *     python tools/font_pixel_goldens.py tests/data/fonts/Silkscreen-Regular.ttf 8 8 8 7 Ag
 */
static const pixel_case pixel_cases[] = {
    {"Px437_IBM_VGA_8x16.ttf",
     16.0f,
     'A',
     12,
     16,
     true,
     {"........", "........", "...#....", "..###...", ".##.##..", "##...##.", "##...##.",
      "#######.", "##...##.", "##...##.", "##...##.", "##...##.", "........", "........",
      "........", "........"}},
    {"Px437_IBM_VGA_8x16.ttf",
     16.0f,
     'g',
     12,
     16,
     true,
     {"........", "........", "........", "........", "........", ".###.##.", "##..##..",
      "##..##..", "##..##..", "##..##..", "##..##..", ".#####..", "....##..", "##..##..",
      ".####...", "........"}},
    {"Silkscreen-Regular.ttf",
     8.0f,
     'A',
     7,
     8,
     false,
     {"........", "........", "..##....", ".#..#...", ".####...", ".#..#...", ".#..#...",
      "........"}},
    {"Silkscreen-Regular.ttf",
     8.0f,
     'g',
     7,
     8,
     false,
     {"........", "........", "..###...", ".#......", ".#.##...", ".#..#...", "..##....",
      "........"}},
};

/* Put a bitmap in an 8-pixel-wide cell, as '#' for full and '.' for empty,
 * and '?' for anything between. */
static void draw_cell(const tb_font_glyph_image *image, int32_t ascent, uint32_t height,
                      char cell[16][9])
{
    for (uint32_t row = 0; row < height; row++)
    {
        memset(cell[row], '.', 8);
        cell[row][8] = '\0';
    }
    for (uint32_t y = 0; y < image->height; y++)
    {
        for (uint32_t x = 0; x < image->width; x++)
        {
            const int32_t row = ascent - image->top + (int32_t)y;
            const int32_t column = image->left + (int32_t)x;
            const uint8_t coverage = image->pixels[(size_t)y * image->width + x];
            if (row < 0 || row >= (int32_t)height || column < 0 || column >= 8)
            {
                TB_TEST_EXPECT_UINT(coverage, ==, 0);
                continue;
            }
            cell[row][column] = coverage == 255 ? '#' : coverage == 0 ? '.' : '?';
        }
    }
}

TB_TEST(font_raster, pixel_fonts_at_their_design_sizes_match_their_pixels)
{
    tb_arena *arena = tb_arena_create(0);
    const tb_font_raster_mode modes[] = {TB_FONT_RASTER_ALIASED, TB_FONT_RASTER_SMOOTH};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(pixel_cases); index++)
    {
        const pixel_case *expected = &pixel_cases[index];
        tb_font *font = open_font(expected->file);
        for (size_t mode = 0; mode < TB_TEST_COUNT_OF(modes); mode++)
        {
            if (modes[mode] == TB_FONT_RASTER_SMOOTH && !expected->on_pixel_edges)
            {
                continue;
            }
            TB_TEST_CONTEXT("%s %c, %s", expected->file, (char)expected->code_point,
                            modes[mode] == TB_FONT_RASTER_ALIASED ? "aliased" : "smooth");
            const tb_font_raster_options options = {expected->size, 0.0f, modes[mode]};
            tb_font_glyph_image image = {0};
            TB_TEST_REQUIRE_UINT(
                tb_font_rasterize_glyph(font, tb_font_find_glyph(font, expected->code_point),
                                        &options, arena, &image, NULL),
                ==, TB_FONT_OK);
            char cell[16][9] = {{0}};
            draw_cell(&image, expected->ascent, expected->height, cell);
            for (uint32_t row = 0; row < expected->height; row++)
            {
                TB_TEST_CONTEXT("row %u", (unsigned)row);
                TB_TEST_EXPECT_STRING(cell[row], ==, expected->rows[row]);
            }
            tb_arena_reset(arena);
        }
        tb_font_close(font);
    }
    tb_arena_destroy(arena);
}

TB_TEST(font_raster, the_aliased_mode_draws_only_full_and_empty_pixels)
{
    tb_font *roboto = open_font("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    for (uint32_t code_point = '!'; code_point <= '~'; code_point++)
    {
        TB_TEST_CONTEXT("U+%04X", (unsigned)code_point);
        const tb_font_raster_options options = {17.0f, 0.3f, TB_FONT_RASTER_ALIASED};
        tb_font_glyph_image image = {0};
        TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(roboto, tb_font_find_glyph(roboto, code_point),
                                                     &options, arena, &image, NULL),
                             ==, TB_FONT_OK);
        size_t between = 0;
        size_t full = 0;
        for (size_t index = 0; index < (size_t)image.width * image.height; index++)
        {
            between += image.pixels[index] != 0 && image.pixels[index] != 255 ? 1u : 0u;
            full += image.pixels[index] == 255 ? 1u : 0u;
        }
        TB_TEST_EXPECT_UINT(between, ==, 0);
        TB_TEST_EXPECT_UINT(full, >, 0);
        tb_arena_reset(arena);
    }
    tb_arena_destroy(arena);
    tb_font_close(roboto);
}

/* -------------------------------------------------------------------------
 * Outlines made by hand.
 * ------------------------------------------------------------------------- */

/* A contour of straight lines through `points`, closed back to the first.
 * The points are written as a bitmap's are, y down; the contour has them y
 * up, as a font's are, so that draw_by_hand() puts them back. */
typedef struct hand_contour
{
    tb_font_segment segments[16];
    tb_font_contour contour;
} hand_contour;

static void make_polygon(hand_contour *made, const float (*points)[2], size_t count)
{
    TB_TEST_REQUIRE_UINT(count, <=, 17);
    made->contour.start = (tb_font_point){points[0][0], -points[0][1]};
    for (size_t index = 1; index < count; index++)
    {
        const tb_font_point end = {points[index][0], -points[index][1]};
        made->segments[index - 1] = (tb_font_segment){TB_FONT_LINE, end, end};
    }
    made->segments[count - 1] =
        (tb_font_segment){TB_FONT_LINE, made->contour.start, made->contour.start};
    made->contour.segments = made->segments;
    made->contour.segment_count = count;
}

/* Draw contours at a scale of 1 with y = 0 at the top of a bitmap of
 * `width` by `height`: a point at (x, -y) lands at column x, row y. */
static tb_font_result draw_by_hand(const tb_font_contour *contours, size_t count,
                                   tb_font_raster_mode mode, uint8_t *pixels, uint32_t width,
                                   uint32_t height)
{
    const tb_font_outline outline = {contours, count, 0, 0, 0, 0};
    const tb_font_raster_placement placement = {1.0f, 0.0f, 0.0f};
    const tb_font_raster_target target = {pixels, width, height, width};
    tb_arena *arena = tb_arena_create(0);
    const size_t remaining = tb_arena_remaining(arena);
    const tb_font_result result =
        tb_font_rasterize_outline(&outline, &placement, mode, &target, arena);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, remaining);
    tb_arena_destroy(arena);
    return result;
}

TB_TEST(font_raster, the_coverage_of_a_pixel_is_the_area_of_it_inside_the_outline)
{
    /* A triangle with a right angle at the bottom left, 4 by 4, and a
     * rectangle whose sides cut pixels in half. */
    const float triangle[][2] = {{0, 0}, {4, 4}, {0, 4}};
    hand_contour made = {0};
    make_polygon(&made, triangle, 3);
    uint8_t pixels[4 * 4] = {0};
    TB_TEST_REQUIRE_UINT(draw_by_hand(&made.contour, 1, TB_FONT_RASTER_SMOOTH, pixels, 4, 4), ==,
                         TB_FONT_OK);
    /* Below the diagonal full, on it half, above it empty. */
    for (uint32_t y = 0; y < 4; y++)
    {
        for (uint32_t x = 0; x < 4; x++)
        {
            TB_TEST_CONTEXT("pixel %u, %u", (unsigned)x, (unsigned)y);
            const uint8_t expected = x < y ? 255 : x == y ? 128 : 0;
            TB_TEST_EXPECT_UINT(pixels[y * 4 + x], ==, expected);
        }
    }
    const float rectangle[][2] = {{0.5f, 0.25f}, {2.5f, 0.25f}, {2.5f, 1.0f}, {0.5f, 1.0f}};
    make_polygon(&made, rectangle, 4);
    uint8_t row[3] = {0};
    TB_TEST_REQUIRE_UINT(draw_by_hand(&made.contour, 1, TB_FONT_RASTER_SMOOTH, row, 3, 1), ==,
                         TB_FONT_OK);
    /* Three quarters of the row's height; half, all, and half of the width. */
    TB_TEST_EXPECT_UINT(row[0], ==, 96);
    TB_TEST_EXPECT_UINT(row[1], ==, 191);
    TB_TEST_EXPECT_UINT(row[2], ==, 96);
    /* A shallow line across many pixels: a triangle 8 wide and 1 tall,
     * whose area in each pixel grows as (x + 0.5) / 8. */
    const float wedge[][2] = {{0, 1}, {8, 0}, {8, 1}};
    make_polygon(&made, wedge, 3);
    uint8_t wide[8] = {0};
    TB_TEST_REQUIRE_UINT(draw_by_hand(&made.contour, 1, TB_FONT_RASTER_SMOOTH, wide, 8, 1), ==,
                         TB_FONT_OK);
    for (uint32_t x = 0; x < 8; x++)
    {
        TB_TEST_CONTEXT("pixel %u", (unsigned)x);
        TB_TEST_EXPECT_UINT(wide[x], ==, (uint32_t)((x + 0.5) / 8.0 * 255.0 + 0.5));
    }
}

TB_TEST(font_raster, contours_fill_by_the_non_zero_rule)
{
    /* Two squares wound the same way overlap in a square that winds twice:
     * full, not over-full and not empty, as the even-odd rule would have
     * it. A third, wound the other way inside the first, cuts a hole. */
    const float outer[][2] = {{0, 0}, {6, 0}, {6, 6}, {0, 6}};
    const float overlapping[][2] = {{3, 3}, {9, 3}, {9, 9}, {3, 9}};
    const float hole[][2] = {{1, 1}, {1, 2}, {2, 2}, {2, 1}};
    hand_contour made[3] = {0};
    make_polygon(&made[0], outer, 4);
    make_polygon(&made[1], overlapping, 4);
    make_polygon(&made[2], hole, 4);
    const tb_font_contour contours[] = {made[0].contour, made[1].contour, made[2].contour};
    const tb_font_raster_mode modes[] = {TB_FONT_RASTER_SMOOTH, TB_FONT_RASTER_ALIASED};
    for (size_t mode = 0; mode < TB_TEST_COUNT_OF(modes); mode++)
    {
        TB_TEST_CONTEXT("mode %u", (unsigned)mode);
        uint8_t pixels[10 * 10] = {0};
        TB_TEST_REQUIRE_UINT(draw_by_hand(contours, 3, modes[mode], pixels, 10, 10), ==,
                             TB_FONT_OK);
        TB_TEST_EXPECT_UINT(pixels[4 * 10 + 4], ==, 255); /* Both squares. */
        TB_TEST_EXPECT_UINT(pixels[0 * 10 + 0], ==, 255); /* The first. */
        TB_TEST_EXPECT_UINT(pixels[8 * 10 + 8], ==, 255); /* The second. */
        TB_TEST_EXPECT_UINT(pixels[1 * 10 + 1], ==, 0);   /* The hole. */
        TB_TEST_EXPECT_UINT(pixels[8 * 10 + 1], ==, 0);   /* Neither. */
        TB_TEST_EXPECT_UINT(pixels[9 * 10 + 9], ==, 0);
    }
}

TB_TEST(font_raster, what_falls_outside_the_bitmap_is_clipped)
{
    /* A square from x = -5 to 2 covers the first two columns whole; one
     * wholly left of the bitmap, one right of it, and one below it cover
     * nothing. */
    const float across_left[][2] = {{-5, 0}, {2, 0}, {2, 2}, {-5, 2}};
    const float left_of[][2] = {{-9, 0}, {-7, 0}, {-7, 4}, {-9, 4}};
    const float right_of[][2] = {{5, 0}, {9, 0}, {9, 4}, {5, 4}};
    const float below[][2] = {{0, 5}, {4, 5}, {4, 9}, {0, 9}};
    hand_contour made[4] = {0};
    make_polygon(&made[0], across_left, 4);
    make_polygon(&made[1], left_of, 4);
    make_polygon(&made[2], right_of, 4);
    make_polygon(&made[3], below, 4);
    const tb_font_contour contours[] = {made[0].contour, made[1].contour, made[2].contour,
                                        made[3].contour};
    const uint8_t expected[4 * 4] = {255, 255, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const tb_font_raster_mode modes[] = {TB_FONT_RASTER_SMOOTH, TB_FONT_RASTER_ALIASED};
    for (size_t mode = 0; mode < TB_TEST_COUNT_OF(modes); mode++)
    {
        TB_TEST_CONTEXT("mode %u", (unsigned)mode);
        uint8_t pixels[4 * 4] = {0};
        TB_TEST_REQUIRE_UINT(draw_by_hand(contours, 4, modes[mode], pixels, 4, 4), ==, TB_FONT_OK);
        TB_TEST_EXPECT_BYTES(pixels, expected, sizeof(expected));
    }
}

TB_TEST(font_raster, degenerate_outlines_draw_nothing_or_what_they_cover)
{
    /* A contour of one point, a contour that doubles back on itself, a
     * level line, and a contour with points that are not finite. */
    const float point[][2] = {{2, 2}};
    const float back_and_forth[][2] = {{0, 0}, {3, 3}};
    const float level[][2] = {{0, 2}, {4, 2}};
    hand_contour made[3] = {0};
    make_polygon(&made[0], point, 1);
    make_polygon(&made[1], back_and_forth, 2);
    make_polygon(&made[2], level, 2);
    hand_contour broken = {0};
    const float square[][2] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    make_polygon(&broken, square, 4);
    broken.segments[1].end.x = NAN;
    broken.segments[2].control.y = INFINITY;
    broken.segments[2].type = TB_FONT_QUADRATIC;
    const tb_font_contour contours[] = {made[0].contour, made[1].contour, made[2].contour};
    uint8_t pixels[4 * 4] = {0};
    memset(pixels, 7, sizeof(pixels));
    TB_TEST_REQUIRE_UINT(draw_by_hand(contours, 3, TB_FONT_RASTER_SMOOTH, pixels, 4, 4), ==,
                         TB_FONT_OK);
    const uint8_t empty[4 * 4] = {0};
    TB_TEST_EXPECT_BYTES(pixels, empty, sizeof(empty));
    /* Skipping what is not finite leaves lines that do not close: what they
     * draw is still coverage, from 0 to 255, and nothing is written past
     * the bitmap. */
    uint8_t guarded[4 * 4 + 4] = {0};
    memset(guarded, 7, sizeof(guarded));
    TB_TEST_REQUIRE_UINT(draw_by_hand(&broken.contour, 1, TB_FONT_RASTER_SMOOTH, guarded, 4, 4), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(guarded[16], ==, 7);
    TB_TEST_EXPECT_UINT(guarded[19], ==, 7);
    /* An empty outline, and an empty bitmap. */
    TB_TEST_EXPECT_UINT(draw_by_hand(NULL, 0, TB_FONT_RASTER_SMOOTH, pixels, 4, 4), ==, TB_FONT_OK);
    TB_TEST_EXPECT_BYTES(pixels, empty, sizeof(empty));
    TB_TEST_EXPECT_UINT(draw_by_hand(contours, 3, TB_FONT_RASTER_SMOOTH, NULL, 0, 0), ==,
                        TB_FONT_OK);
}

TB_TEST(font_raster, an_outline_of_too_many_lines_is_too_large_and_draws_nothing)
{
    /* Each curve bends so far that it is cut into the most lines a curve
     * is; one more curve than the limit allows of those is too many. */
    const size_t curves = TB_FONT_RASTER_MAX_EDGES / 64 + 1;
    tb_font_segment *segments = tb_calloc(curves, sizeof(tb_font_segment));
    for (size_t index = 0; index < curves; index++)
    {
        segments[index].type = TB_FONT_QUADRATIC;
        segments[index].control = (tb_font_point){1.0e6f, 1.0e6f};
        segments[index].end = (tb_font_point){0.0f, 0.0f};
    }
    const tb_font_contour contour = {{0.0f, 0.0f}, segments, curves};
    uint8_t pixels[4 * 4] = {0};
    memset(pixels, 7, sizeof(pixels));
    TB_TEST_EXPECT_UINT(draw_by_hand(&contour, 1, TB_FONT_RASTER_SMOOTH, pixels, 4, 4), ==,
                        TB_FONT_TOO_LARGE);
    TB_TEST_EXPECT_UINT(pixels[0], ==, 7);
    TB_TEST_EXPECT_UINT(pixels[15], ==, 7);
    tb_free(segments);
}

TB_TEST(font_raster, a_glyph_larger_than_the_limit_is_too_large)
{
    /* A font of 1000 units to the em whose glyph 1 spans 32000 units: at
     * 2000 pixels to the em that is 64000 pixels. */
    test_font *made = test_font_create();
    test_glyph glyphs[3] = {0};
    const test_point points[] = {
        {-16000, -16000, true}, {-16000, 16000, true}, {16000, 16000, true}, {16000, -16000, true}};
    const uint16_t ends[] = {3};
    test_simple_glyph(&glyphs[1], points, 4, ends, 1);
    /* Glyph 2: contour ends out of order, which the reader refuses. */
    const uint16_t bad_ends[] = {3, 1};
    test_simple_glyph(&glyphs[2], points, 4, bad_ends, 2);
    test_font_set_glyphs(made, glyphs, 3);
    uint8_t *bytes = tb_malloc(FONT_BUFFER);
    const size_t length = test_font_write(made, bytes, FONT_BUFFER);
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(bytes, length, NULL, &font, NULL), ==, TB_FONT_OK);
    tb_arena *arena = tb_arena_create(0);
    const size_t remaining = tb_arena_remaining(arena);
    const tb_font_raster_options huge = {2000.0f, 0.0f, TB_FONT_RASTER_SMOOTH};
    tb_font_glyph_image image = {0};
    tb_font_error error = {0};
    TB_TEST_EXPECT_UINT(tb_font_rasterize_glyph(font, 1, &huge, arena, &image, &error), ==,
                        TB_FONT_TOO_LARGE);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the glyph's bitmap is larger than the limit");
    TB_TEST_EXPECT_UINT(image.width, ==, 0);
    TB_TEST_EXPECT_POINTER(image.pixels, ==, NULL);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, remaining);
    /* At a small size it draws: a square, all full. */
    const tb_font_raster_options small = {0.5f, 0.0f, TB_FONT_RASTER_SMOOTH};
    TB_TEST_REQUIRE_UINT(tb_font_rasterize_glyph(font, 1, &small, arena, &image, &error), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_UINT(image.width, ==, 16);
    TB_TEST_EXPECT_UINT(image.height, ==, 16);
    TB_TEST_EXPECT_UINT(image.pixels[0], ==, 255);
    TB_TEST_EXPECT_UINT(image.pixels[255], ==, 255);
    /* A damaged glyph fails as reading its outline does. */
    tb_arena_reset(arena);
    TB_TEST_EXPECT_UINT(tb_font_rasterize_glyph(font, 2, &small, arena, &image, &error), ==,
                        TB_FONT_MALFORMED);
    TB_TEST_EXPECT_STRING(error.table, ==, "glyf");
    TB_TEST_EXPECT_UINT(tb_font_measure_glyph(font, 2, &small, arena, &image, NULL), ==,
                        TB_FONT_MALFORMED);
    TB_TEST_EXPECT_UINT(tb_arena_remaining(arena), ==, remaining);
    tb_arena_destroy(arena);
    tb_font_close(font);
    tb_free(bytes);
    test_font_destroy(made);
}
