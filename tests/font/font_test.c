/* The TrueType reader: three fonts of the font pack read against values that
 * a separate reader, written in Python's standard library, found in them;
 * and fonts made in memory for every format and case those three do not
 * use. */
#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/font.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "font_builder.h"
#include "tb_test.h"
#include "test_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

#define FONT_BUFFER ((size_t)65536)

static const tb_test_file *font_file(const char *name)
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
    return found;
}

static tb_font *open_file(const char *name)
{
    const tb_test_file *file = font_file(name);
    tb_font *font = NULL;
    tb_font_error error = {0};
    TB_TEST_REQUIRE_UINT(tb_font_open(file->bytes, file->length, NULL, &font, &error), ==,
                         TB_FONT_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    TB_TEST_EXPECT_STRING(error.table, ==, "");
    return font;
}

/* A font made in memory, and the bytes it was written to. */
typedef struct built_font
{
    uint8_t *bytes;
    size_t length;
    tb_font *font;
    tb_font_error error;
    tb_font_result result;
} built_font;

static built_font open_built(const test_font *made)
{
    built_font built = {0};
    built.bytes = tb_malloc(FONT_BUFFER);
    built.length = test_font_write(made, built.bytes, FONT_BUFFER);
    built.result = tb_font_open(built.bytes, built.length, NULL, &built.font, &built.error);
    return built;
}

static void close_built(built_font *built)
{
    tb_font_close(built->font);
    tb_free(built->bytes);
}

/* -------------------------------------------------------------------------
 * The fonts of the font pack.
 * ------------------------------------------------------------------------- */

typedef struct expected_font
{
    const char *file;
    tb_font_metrics metrics;
    const char *family;
    const char *style;
    uint16_t weight;
    bool italic;
} expected_font;

static const expected_font expected_fonts[] = {
    {"Px437_IBM_VGA_8x16.ttf",
     {1600, 289, 1200, -400, 0, 1000, 700, 0, -400, 800, 1200},
     "Px437 IBM VGA 8x16",
     "Regular",
     400,
     false},
    {"Roboto-Regular.ttf",
     {2048, 1326, 1900, -500, 0, 1456, 1082, -1510, -555, 2353, 2163},
     "Roboto",
     "Regular",
     400,
     false},
    {"Silkscreen-Regular.ttf",
     {1000, 228, 1030, -250, 0, 700, 500, -187, -250, 1125, 1000},
     "Silkscreen",
     "Regular",
     400,
     false},
};

TB_TEST(font_truetype, reads_the_metrics_and_names_of_real_fonts)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_fonts); index++)
    {
        const expected_font *expected = &expected_fonts[index];
        TB_TEST_CONTEXT("%s", expected->file);
        tb_font *font = open_file(expected->file);
        tb_font_metrics metrics = {0};
        tb_font_get_metrics(font, &metrics);
        TB_TEST_EXPECT_UINT(metrics.units_per_em, ==, expected->metrics.units_per_em);
        TB_TEST_EXPECT_UINT(metrics.glyph_count, ==, expected->metrics.glyph_count);
        TB_TEST_EXPECT_INT(metrics.ascender, ==, expected->metrics.ascender);
        TB_TEST_EXPECT_INT(metrics.descender, ==, expected->metrics.descender);
        TB_TEST_EXPECT_INT(metrics.line_gap, ==, expected->metrics.line_gap);
        TB_TEST_EXPECT_INT(metrics.cap_height, ==, expected->metrics.cap_height);
        TB_TEST_EXPECT_INT(metrics.x_height, ==, expected->metrics.x_height);
        TB_TEST_EXPECT_INT(metrics.x_min, ==, expected->metrics.x_min);
        TB_TEST_EXPECT_INT(metrics.y_min, ==, expected->metrics.y_min);
        TB_TEST_EXPECT_INT(metrics.x_max, ==, expected->metrics.x_max);
        TB_TEST_EXPECT_INT(metrics.y_max, ==, expected->metrics.y_max);
        tb_font_style style = {0};
        tb_font_get_style(font, &style);
        TB_TEST_EXPECT_STRING(style.family, ==, expected->family);
        TB_TEST_EXPECT_UINT(style.family_length, ==, tb_str_length(expected->family));
        TB_TEST_EXPECT_STRING(style.style, ==, expected->style);
        TB_TEST_EXPECT_UINT(style.style_length, ==, tb_str_length(expected->style));
        TB_TEST_EXPECT_UINT(style.weight, ==, expected->weight);
        TB_TEST_EXPECT(style.italic == expected->italic);
        tb_font_close(font);
    }
}

typedef struct expected_glyph
{
    const char *file;
    uint32_t code_point;
    uint16_t glyph;
    uint16_t advance;
    int16_t bearing;
} expected_glyph;

static const expected_glyph expected_glyphs[] = {
    {"Px437_IBM_VGA_8x16.ttf", 'A', 66, 800, 0},    {"Px437_IBM_VGA_8x16.ttf", 'T', 85, 800, 100},
    {"Px437_IBM_VGA_8x16.ttf", 0xE9, 166, 800, 0},  {"Px437_IBM_VGA_8x16.ttf", 0x20AC, 0, 800, 53},
    {"Roboto-Regular.ttf", 'A', 37, 1336, 29},      {"Roboto-Regular.ttf", ' ', 4, 508, 0},
    {"Roboto-Regular.ttf", 'g', 75, 1150, 97},      {"Roboto-Regular.ttf", 0xE9, 703, 1086, 93},
    {"Roboto-Regular.ttf", 0x20AC, 440, 1151, 95},  {"Roboto-Regular.ttf", 0x105, 727, 1114, 109},
    {"Roboto-Regular.ttf", 0x1F600, 0, 0, 0},       {"Silkscreen-Regular.ttf", 'A', 1, 750, 125},
    {"Silkscreen-Regular.ttf", ' ', 142, 500, 0},   {"Silkscreen-Regular.ttf", 0xC5, 6, 750, 125},
    {"Silkscreen-Regular.ttf", 0x105, 0, 625, 125},
};

TB_TEST(font_truetype, maps_characters_to_glyphs_and_their_advances)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_glyphs); index++)
    {
        const expected_glyph *expected = &expected_glyphs[index];
        TB_TEST_CONTEXT("%s U+%04X", expected->file, (unsigned)expected->code_point);
        tb_font *font = open_file(expected->file);
        const uint16_t glyph = tb_font_find_glyph(font, expected->code_point);
        TB_TEST_EXPECT_UINT(glyph, ==, expected->glyph);
        if (expected->advance != 0)
        {
            tb_font_glyph_metrics metrics = {0};
            tb_font_get_glyph_metrics(font, glyph, &metrics);
            TB_TEST_EXPECT_UINT(metrics.advance_width, ==, expected->advance);
            TB_TEST_EXPECT_INT(metrics.left_side_bearing, ==, expected->bearing);
        }
        tb_font_close(font);
    }
}

/* What a separate reader found in the whole of each font: every outline, as
 * contours of lines and curves, and a weighted sum of their points; every
 * advance and bearing, weighted by glyph; the code points of the BMP that
 * map to a glyph, and the sum of those glyphs; and the kerning between
 * every pair of printable ASCII characters. */
typedef struct font_totals
{
    const char *file;
    size_t contours;
    size_t lines;
    size_t quadratics;
    double point_sum;
    uint64_t advance_sum;
    int64_t bearing_sum;
    size_t mapped;
    uint64_t glyph_sum;
    size_t kerned_pairs;
    int64_t kerning_sum;
} font_totals;

static const font_totals expected_totals[] = {
    {"Px437_IBM_VGA_8x16.ttf", 607, 6580, 0, 51186722.0, 33524000, 2019953, 288, 41616, 0, 0},
    {"Roboto-Regular.ttf", 3635, 15020, 23501, 477431525.0, 1015103259, 72807438, 927, 536176, 432,
     -5787339},
    {"Silkscreen-Regular.ttf", 515, 3916, 0, 28249939.0, 16950125, 3104258, 226, 25737, 0, 0},
};

static void add_outline(const tb_font_outline *outline, font_totals *totals)
{
    for (size_t contour = 0; contour < outline->contour_count; contour++)
    {
        const tb_font_contour *each = &outline->contours[contour];
        totals->contours++;
        totals->point_sum += (double)each->start.x * 11.0 + (double)each->start.y * 13.0;
        for (size_t index = 0; index < each->segment_count; index++)
        {
            const tb_font_segment *segment = &each->segments[index];
            if (segment->type == TB_FONT_LINE)
            {
                totals->lines++;
                TB_TEST_EXPECT_FLOAT(segment->control.x, ==, segment->end.x);
                TB_TEST_EXPECT_FLOAT(segment->control.y, ==, segment->end.y);
            }
            else
            {
                totals->quadratics++;
            }
            totals->point_sum += (double)segment->end.x + (double)segment->end.y * 3.0 +
                                 (double)segment->control.x * 5.0 +
                                 (double)segment->control.y * 7.0;
        }
        /* Every contour closes where it began. */
        const tb_font_segment *last = &each->segments[each->segment_count - 1];
        TB_TEST_EXPECT_FLOAT(last->end.x, ==, each->start.x);
        TB_TEST_EXPECT_FLOAT(last->end.y, ==, each->start.y);
    }
}

TB_TEST(font_truetype, reads_every_glyph_as_a_separate_reader_does)
{
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected_totals); index++)
    {
        const font_totals *expected = &expected_totals[index];
        TB_TEST_CONTEXT("%s", expected->file);
        tb_font *font = open_file(expected->file);
        tb_arena *arena = tb_arena_create(0);
        tb_font_metrics metrics = {0};
        tb_font_get_metrics(font, &metrics);
        font_totals totals = {0};
        for (uint16_t glyph = 0; glyph < metrics.glyph_count; glyph++)
        {
            tb_font_outline outline = {0};
            tb_font_error error = {0};
            TB_TEST_REQUIRE_UINT(tb_font_get_outline(font, glyph, arena, &outline, &error), ==,
                                 TB_FONT_OK);
            add_outline(&outline, &totals);
            tb_font_glyph_metrics advance = {0};
            tb_font_get_glyph_metrics(font, glyph, &advance);
            totals.advance_sum += (uint64_t)advance.advance_width * (glyph + 1u);
            totals.bearing_sum += (int64_t)advance.left_side_bearing * (glyph + 1);
            tb_arena_reset(arena);
        }
        for (uint32_t code_point = 0; code_point <= 0xFFFF; code_point++)
        {
            const uint16_t glyph = tb_font_find_glyph(font, code_point);
            totals.mapped += glyph != 0 ? 1u : 0u;
            totals.glyph_sum += glyph;
        }
        for (uint32_t left = 0x20; left < 0x7F; left++)
        {
            for (uint32_t right = 0x20; right < 0x7F; right++)
            {
                const int32_t kerning = tb_font_get_kerning(font, tb_font_find_glyph(font, left),
                                                            tb_font_find_glyph(font, right));
                totals.kerned_pairs += kerning != 0 ? 1u : 0u;
                totals.kerning_sum += (int64_t)kerning * (int64_t)(left + 3 * right);
            }
        }
        TB_TEST_EXPECT_UINT(totals.contours, ==, expected->contours);
        TB_TEST_EXPECT_UINT(totals.lines, ==, expected->lines);
        TB_TEST_EXPECT_UINT(totals.quadratics, ==, expected->quadratics);
        TB_TEST_EXPECT_FLOAT(totals.point_sum, ==, expected->point_sum);
        TB_TEST_EXPECT_UINT(totals.advance_sum, ==, expected->advance_sum);
        TB_TEST_EXPECT_INT(totals.bearing_sum, ==, expected->bearing_sum);
        TB_TEST_EXPECT_UINT(totals.mapped, ==, expected->mapped);
        TB_TEST_EXPECT_UINT(totals.glyph_sum, ==, expected->glyph_sum);
        TB_TEST_EXPECT_UINT(totals.kerned_pairs, ==, expected->kerned_pairs);
        TB_TEST_EXPECT_INT(totals.kerning_sum, ==, expected->kerning_sum);
        tb_arena_destroy(arena);
        tb_font_close(font);
    }
}

TB_TEST(font_truetype, kerns_pairs_through_the_gpos_kern_feature)
{
    static const struct
    {
        char left;
        char right;
        int32_t kerning;
    } pairs[] = {
        {'A', 'V', -87},  {'V', 'A', -75}, {'T', 'o', -99}, {'A', 'T', -129}, {'A', 'v', -50},
        {'L', 'T', -275}, {'o', 'o', 0},   {'Y', 'o', -65}, {'P', ',', -324}, {'T', 'e', -99},
    };
    tb_font *font = open_file("Roboto-Regular.ttf");
    for (size_t index = 0; index < TB_TEST_COUNT_OF(pairs); index++)
    {
        TB_TEST_CONTEXT("%c%c", pairs[index].left, pairs[index].right);
        const uint16_t left = tb_font_find_glyph(font, (uint32_t)pairs[index].left);
        const uint16_t right = tb_font_find_glyph(font, (uint32_t)pairs[index].right);
        TB_TEST_EXPECT_INT(tb_font_get_kerning(font, left, right), ==, pairs[index].kerning);
    }
    /* Glyphs the font does not have kern with nothing. */
    TB_TEST_EXPECT_INT(tb_font_get_kerning(font, 0xFFFF, 0xFFFF), ==, 0);
    tb_font_close(font);
}

TB_TEST(font_truetype, places_the_parts_of_a_compound_glyph)
{
    /* Roboto's é is its e, and its acute accent moved into place. */
    tb_font *font = open_file("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    tb_font_outline outline = {0};
    TB_TEST_REQUIRE_UINT(tb_font_get_outline(font, 703, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 2);
    TB_TEST_EXPECT_FLOAT(outline.contours[0].start.x, ==, 590.0f);
    TB_TEST_EXPECT_FLOAT(outline.contours[0].start.y, ==, -20.0f);
    TB_TEST_REQUIRE_UINT(outline.contours[0].segment_count, ==, 36);
    TB_TEST_EXPECT_UINT(outline.contours[0].segments[0].type, ==, TB_FONT_QUADRATIC);
    TB_TEST_EXPECT_FLOAT(outline.contours[0].segments[0].control.x, ==, 477.0f);
    TB_TEST_EXPECT_FLOAT(outline.contours[0].segments[0].end.x, ==, 385.5f);
    TB_TEST_EXPECT_FLOAT(outline.contours[0].segments[0].end.y, ==, 18.5f);
    TB_TEST_EXPECT_FLOAT(outline.contours[1].start.x, ==, 463.0f);
    TB_TEST_EXPECT_FLOAT(outline.contours[1].start.y, ==, 1242.0f);
    TB_TEST_REQUIRE_UINT(outline.contours[1].segment_count, ==, 4);
    TB_TEST_EXPECT_UINT(outline.contours[1].segments[0].type, ==, TB_FONT_LINE);
    TB_TEST_EXPECT_FLOAT(outline.contours[1].segments[0].end.x, ==, 657.0f);
    TB_TEST_EXPECT_FLOAT(outline.contours[1].segments[0].end.y, ==, 1536.0f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

TB_TEST(font_truetype, a_blank_glyph_has_no_contours)
{
    tb_font *font = open_file("Roboto-Regular.ttf");
    tb_arena *arena = tb_arena_create(0);
    tb_font_outline outline = {0};
    TB_TEST_REQUIRE_UINT(
        tb_font_get_outline(font, tb_font_find_glyph(font, ' '), arena, &outline, NULL), ==,
        TB_FONT_OK);
    TB_TEST_EXPECT_UINT(outline.contour_count, ==, 0);
    TB_TEST_EXPECT_POINTER(outline.contours, ==, NULL);
    TB_TEST_EXPECT_INT(outline.x_max, ==, 0);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* -------------------------------------------------------------------------
 * Fonts made in memory.
 * ------------------------------------------------------------------------- */

static void expect_point(tb_font_point point, float x, float y)
{
    TB_TEST_EXPECT_FLOAT_NEAR(point.x, x, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(point.y, y, 0.001f);
}

/* The end of each segment of a contour, and whether it is a curve. */
typedef struct expected_segment
{
    bool curve;
    float control_x;
    float control_y;
    float x;
    float y;
} expected_segment;

static void expect_contour(const tb_font_contour *contour, float start_x, float start_y,
                           const expected_segment *segments, size_t count)
{
    expect_point(contour->start, start_x, start_y);
    TB_TEST_REQUIRE_UINT(contour->segment_count, ==, count);
    for (size_t index = 0; index < count; index++)
    {
        TB_TEST_CONTEXT("segment %zu", index);
        const tb_font_segment *segment = &contour->segments[index];
        TB_TEST_EXPECT_UINT(segment->type, ==,
                            segments[index].curve ? TB_FONT_QUADRATIC : TB_FONT_LINE);
        expect_point(segment->end, segments[index].x, segments[index].y);
        if (segments[index].curve)
        {
            expect_point(segment->control, segments[index].control_x, segments[index].control_y);
        }
    }
}

TB_TEST(font_truetype, reads_a_small_font_made_in_memory)
{
    test_font *made = test_font_create();
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_metrics metrics = {0};
    tb_font_get_metrics(built.font, &metrics);
    TB_TEST_EXPECT_UINT(metrics.units_per_em, ==, 1000);
    TB_TEST_EXPECT_UINT(metrics.glyph_count, ==, 3);
    TB_TEST_EXPECT_INT(metrics.ascender, ==, 800);
    TB_TEST_EXPECT_INT(metrics.descender, ==, -200);
    TB_TEST_EXPECT_INT(metrics.line_gap, ==, 90);
    TB_TEST_EXPECT_INT(metrics.cap_height, ==, 700);
    TB_TEST_EXPECT_INT(metrics.x_height, ==, 500);
    tb_font_style style = {0};
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_STRING(style.family, ==, "Test");
    TB_TEST_EXPECT_STRING(style.style, ==, "Regular");
    TB_TEST_EXPECT_UINT(style.weight, ==, TB_FONT_WEIGHT_REGULAR);
    TB_TEST_EXPECT(!style.italic);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'A'), ==, 1);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'B'), ==, 2);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'C'), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x10000), ==, 0);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 2), ==, 0);

    tb_arena *arena = tb_arena_create(0);
    tb_font_outline outline = {0};
    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 1, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 1);
    const expected_segment square[] = {
        {false, 0, 0, 0, 100}, {false, 0, 0, 100, 100}, {false, 0, 0, 100, 0}, {false, 0, 0, 0, 0}};
    expect_contour(&outline.contours[0], 0, 0, square, 4);
    TB_TEST_EXPECT_INT(outline.x_max, ==, 100);
    TB_TEST_EXPECT_INT(outline.y_max, ==, 100);

    /* Only off-curve points: each curve ends halfway to the next. */
    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 2, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 1);
    const expected_segment diamond[] = {{true, 50, 0, 25, 25},
                                        {true, 0, 50, 25, 75},
                                        {true, 50, 100, 75, 75},
                                        {true, 100, 50, 75, 25}};
    expect_contour(&outline.contours[0], 75, 25, diamond, 4);
    tb_arena_destroy(arena);
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, resolves_off_curve_points_into_curves)
{
    test_font *made = test_font_create();
    test_glyph glyphs[3] = {0};
    /* Two off-curve points in a row imply an on-curve point between them;
     * a contour that begins off the curve begins at its first on-curve point. */
    const test_point points[] = {{0, 0, true},    {0, 100, false}, {100, 100, false},
                                 {100, 0, true},  {200, 0, false}, {200, 100, true},
                                 {300, 100, true}};
    const uint16_t ends[] = {3, 6};
    test_simple_glyph(&glyphs[1], points, TB_TEST_COUNT_OF(points), ends, 2);
    test_font_set_glyphs(made, glyphs, 3);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_arena *arena = tb_arena_create(0);
    tb_font_outline outline = {0};
    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 1, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 2);
    const expected_segment first[] = {
        {true, 0, 100, 50, 100}, {true, 100, 100, 100, 0}, {false, 0, 0, 0, 0}};
    expect_contour(&outline.contours[0], 0, 0, first, 3);
    const expected_segment second[] = {{false, 0, 0, 300, 100}, {true, 200, 0, 200, 100}};
    expect_contour(&outline.contours[1], 200, 100, second, 2);
    /* A blank glyph has no box. */
    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 0, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(outline.contour_count, ==, 0);
    tb_arena_destroy(arena);
    close_built(&built);
    test_font_destroy(made);
}

/* Component flags. */
enum
{
    WORDS = 0x0001,
    XY = 0x0002,
    SCALE = 0x0008,
    MORE = 0x0020,
    XY_SCALE = 0x0040,
    TWO_BY_TWO = 0x0080,
    SCALED_OFFSET = 0x0800
};

static void component(test_glyph *glyph, uint16_t flags, uint16_t index)
{
    test_glyph_u16(glyph, flags);
    test_glyph_u16(glyph, index);
}

TB_TEST(font_truetype, transforms_and_places_components)
{
    test_font *made = test_font_create();
    test_glyph glyphs[9] = {0};
    const test_point square[] = {{0, 0, true}, {0, 100, true}, {100, 100, true}, {100, 0, true}};
    const uint16_t square_ends[] = {3};
    test_simple_glyph(&glyphs[1], square, 4, square_ends, 1);
    /* 2: the square moved by 16-bit offsets, then by 8-bit offsets and halved. */
    test_compound_header(&glyphs[2]);
    component(&glyphs[2], WORDS | XY | MORE, 1);
    test_glyph_u16(&glyphs[2], 1000);
    test_glyph_u16(&glyphs[2], (uint16_t)-20);
    component(&glyphs[2], XY | SCALE, 1);
    test_glyph_u8(&glyphs[2], (uint8_t)-5);
    test_glyph_u8(&glyphs[2], 7);
    test_glyph_u16(&glyphs[2], 0x2000); /* 0.5 */
    /* 3: the square scaled 1.5 across and 0.25 up. */
    test_compound_header(&glyphs[3]);
    component(&glyphs[3], XY | XY_SCALE, 1);
    test_glyph_u8(&glyphs[3], 0);
    test_glyph_u8(&glyphs[3], 0);
    test_glyph_u16(&glyphs[3], 0x6000); /* 1.5 */
    test_glyph_u16(&glyphs[3], 0x1000); /* 0.25 */
    /* 4: the square turned a quarter turn, its offset turned with it. */
    test_compound_header(&glyphs[4]);
    component(&glyphs[4], XY | TWO_BY_TWO | SCALED_OFFSET, 1);
    test_glyph_u8(&glyphs[4], 10);
    test_glyph_u8(&glyphs[4], 0);
    test_glyph_u16(&glyphs[4], 0);
    test_glyph_u16(&glyphs[4], 0x4000);
    test_glyph_u16(&glyphs[4], 0xC000);
    test_glyph_u16(&glyphs[4], 0);
    /* 5: two squares, the second placed so that its point 0 lands on the
     * first's point 2, by 8-bit and then 16-bit point numbers. */
    test_compound_header(&glyphs[5]);
    component(&glyphs[5], XY | MORE, 1);
    test_glyph_u8(&glyphs[5], 0);
    test_glyph_u8(&glyphs[5], 0);
    component(&glyphs[5], MORE, 1);
    test_glyph_u8(&glyphs[5], 2);
    test_glyph_u8(&glyphs[5], 0);
    component(&glyphs[5], WORDS, 1);
    test_glyph_u16(&glyphs[5], 6);
    test_glyph_u16(&glyphs[5], 0);
    /* 6: a compound of the compound 2, moved. */
    test_compound_header(&glyphs[6]);
    component(&glyphs[6], XY, 2);
    test_glyph_u8(&glyphs[6], 1);
    test_glyph_u8(&glyphs[6], 1);
    /* 7: the square, its offset not scaled: the default. */
    test_compound_header(&glyphs[7]);
    component(&glyphs[7], XY | SCALE, 1);
    test_glyph_u8(&glyphs[7], 10);
    test_glyph_u8(&glyphs[7], 10);
    test_glyph_u16(&glyphs[7], 0x2000);
    test_font_set_glyphs(made, glyphs, 8);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_arena *arena = tb_arena_create(0);
    tb_font_outline outline = {0};

    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 2, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 2);
    const expected_segment moved[] = {{false, 0, 0, 1000, 80},
                                      {false, 0, 0, 1100, 80},
                                      {false, 0, 0, 1100, -20},
                                      {false, 0, 0, 1000, -20}};
    expect_contour(&outline.contours[0], 1000, -20, moved, 4);
    const expected_segment halved[] = {
        {false, 0, 0, -5, 57}, {false, 0, 0, 45, 57}, {false, 0, 0, 45, 7}, {false, 0, 0, -5, 7}};
    expect_contour(&outline.contours[1], -5, 7, halved, 4);

    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 3, arena, &outline, NULL), ==, TB_FONT_OK);
    const expected_segment stretched[] = {
        {false, 0, 0, 0, 25}, {false, 0, 0, 150, 25}, {false, 0, 0, 150, 0}, {false, 0, 0, 0, 0}};
    expect_contour(&outline.contours[0], 0, 0, stretched, 4);

    /* x' = 0 x + -1 y, y' = 1 x + 0 y; the offset (10, 0) turns to (0, 10). */
    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 4, arena, &outline, NULL), ==, TB_FONT_OK);
    const expected_segment turned[] = {{false, 0, 0, -100, 10},
                                       {false, 0, 0, -100, 110},
                                       {false, 0, 0, 0, 110},
                                       {false, 0, 0, 0, 10}};
    expect_contour(&outline.contours[0], 0, 10, turned, 4);

    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 5, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 3);
    expect_point(outline.contours[0].start, 0, 0);
    expect_point(outline.contours[1].start, 100, 100);
    expect_point(outline.contours[2].start, 200, 200);

    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 6, arena, &outline, NULL), ==, TB_FONT_OK);
    TB_TEST_REQUIRE_UINT(outline.contour_count, ==, 2);
    expect_point(outline.contours[0].start, 1001, -19);
    expect_point(outline.contours[1].start, -4, 8);

    TB_TEST_REQUIRE_UINT(tb_font_get_outline(built.font, 7, arena, &outline, NULL), ==, TB_FONT_OK);
    expect_point(outline.contours[0].start, 10, 10);
    expect_point(outline.contours[0].segments[1].end, 60, 60);
    tb_arena_destroy(arena);
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, glyphs_past_the_last_advance_share_it)
{
    test_font *made = test_font_create();
    /* One full metric for glyph 0, then bearings 7 and 9 for glyphs 1 and 2. */
    test_table_set16(test_font_table(made, "hhea"), 34, 1);
    test_table *hmtx = test_font_table(made, "hmtx");
    hmtx->length = 0;
    test_table_u16(hmtx, 600);
    test_table_u16(hmtx, 3);
    test_table_u16(hmtx, 7);
    test_table_u16(hmtx, 9);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    const int16_t bearings[] = {3, 7, 9};
    for (uint16_t glyph = 0; glyph < 3; glyph++)
    {
        tb_font_glyph_metrics metrics = {0};
        tb_font_get_glyph_metrics(built.font, glyph, &metrics);
        TB_TEST_EXPECT_UINT(metrics.advance_width, ==, 600);
        TB_TEST_EXPECT_INT(metrics.left_side_bearing, ==, bearings[glyph]);
    }
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, prefers_the_full_unicode_map_and_follows_glyph_arrays)
{
    test_font *made = test_font_create();
    test_table *cmap = test_font_table(made, "cmap");
    test_cmap_two_subtables(cmap);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x1F600), ==, 2);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x20000), ==, 1);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x20001), ==, 2);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x1F5FF), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x20002), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x30000), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'a'), ==, 0);
    close_built(&built);

    /* Without the format 12 subtable, the format 4 one maps through its glyph array. */
    test_table_set16(cmap, 2, 2);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'a'), ==, 1);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'b'), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'c'), ==, 2);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 'd'), ==, 0);
    TB_TEST_EXPECT_UINT(tb_font_find_glyph(built.font, 0x1F600), ==, 0);
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, kerns_pairs_through_the_kern_table)
{
    test_font *made = test_font_create();
    test_kern_table(test_font_table(made, "kern"), 1);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 2), ==, -40);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 2, 1), ==, 25);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 1), ==, 0);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 2, 2), ==, 0);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 0, 0), ==, 0);
    close_built(&built);

    /* Apple's version 1 kern table is not read. */
    test_table_set16(test_font_table(made, "kern"), 0, 1);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 2), ==, 0);
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, sums_the_kerning_of_every_gpos_kern_lookup)
{
    test_font *made = test_font_create();
    test_gpos_layout layout = {0};
    test_gpos_table(test_font_table(made, "GPOS"), &layout);
    /* A kern table, which GPOS's kern feature overrides. */
    test_kern_table(test_font_table(made, "kern"), 0);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 2), ==, -35);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 0), ==, 77);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 1), ==, 0);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 2, 1), ==, 0);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 0, 2), ==, 500);
    close_built(&built);

    /* Without a kern feature, the kern table kerns. */
    test_table_set32(test_font_table(made, "GPOS"), 18, 0x6B65726F);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_INT(tb_font_get_kerning(built.font, 1, 2), ==, -40);
    close_built(&built);
    test_font_destroy(made);
}

/* A name table of the records given, each with its bytes. */
typedef struct name_record
{
    uint16_t platform;
    uint16_t encoding;
    uint16_t language;
    uint16_t id;
    const char *bytes;
    size_t length;
} name_record;

static void name_table(test_table *name, const name_record *records, size_t count)
{
    name->length = 0;
    test_table_u16(name, 0);
    test_table_u16(name, (uint16_t)count);
    test_table_u16(name, (uint16_t)(6 + count * 12));
    size_t storage = 0;
    for (size_t index = 0; index < count; index++)
    {
        test_table_u16(name, records[index].platform);
        test_table_u16(name, records[index].encoding);
        test_table_u16(name, records[index].language);
        test_table_u16(name, records[index].id);
        test_table_u16(name, (uint16_t)records[index].length);
        test_table_u16(name, (uint16_t)storage);
        storage += records[index].length;
    }
    for (size_t index = 0; index < count; index++)
    {
        for (size_t byte = 0; byte < records[index].length; byte++)
        {
            test_table_u8(name, (uint8_t)records[index].bytes[byte]);
        }
    }
}

TB_TEST(font_truetype, decodes_names_and_prefers_typographic_ones)
{
    test_font *made = test_font_create();
    const name_record records[] = {
        /* Mac Roman: 0x8E is e with an acute accent. */
        {1, 0, 0, 1, "Caf\x8E", 4},
        /* A family in French, then one in US English. */
        {3, 1, 0x040C, 1, "\0F\0r", 4},
        {3, 1, 0x0409, 1, "\0W\0i\0n", 6},
        /* A subfamily whose encoding is not read, and one in Mac Roman. */
        {3, 2, 0x0409, 2, "\0X", 2},
        {1, 0, 0, 2, "Bold", 4},
        /* A typographic family in Unicode, with a character past the BMP. */
        {0, 3, 0, 16, "\0T\xD8\x3D\xDE\x00", 6},
        /* A typographic subfamily: an unpaired high surrogate, a lone low one, an odd byte. */
        {3, 10, 0x0409, 17, "\xD8\x00\0A\xDC\x00\0B\0", 9},
    };
    name_table(test_font_table(made, "name"), records, TB_TEST_COUNT_OF(records));
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_style style = {0};
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_STRING(style.family, ==, "T\xF0\x9F\x98\x80");
    TB_TEST_EXPECT_UINT(style.family_length, ==, 5);
    TB_TEST_EXPECT_STRING(style.style, ==,
                          "\xEF\xBF\xBD"
                          "A"
                          "\xEF\xBF\xBD"
                          "B");
    close_built(&built);

    /* Without typographic names: Windows English, then Mac Roman. */
    name_table(test_font_table(made, "name"), records, 5);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_STRING(style.family, ==, "Win");
    TB_TEST_EXPECT_STRING(style.style, ==, "Bold");
    close_built(&built);

    name_table(test_font_table(made, "name"), records, 1);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_STRING(style.family, ==, "Caf\xC3\xA9");
    TB_TEST_EXPECT_STRING(style.style, ==, "");
    TB_TEST_EXPECT_UINT(style.style_length, ==, 0);
    close_built(&built);

    /* No name table: no names. */
    test_font_remove(made, "name");
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_STRING(style.family, ==, "");
    TB_TEST_EXPECT_UINT(style.family_length, ==, 0);
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, weight_and_italic_come_from_os2_or_head)
{
    test_font *made = test_font_create();
    test_table *os2 = test_font_table(made, "OS/2");
    /* Italic in OS/2; a weight of 7 means 700. */
    test_table_set16(os2, 4, 7);
    test_table_set16(os2, 62, 0x0001);
    built_font built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_style style = {0};
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_UINT(style.weight, ==, 700);
    TB_TEST_EXPECT(style.italic);
    close_built(&built);

    /* Version 1 has no cap or x height; a weight of 0 leaves head's. */
    test_table_set16(os2, 0, 1);
    test_table_set16(os2, 4, 0);
    test_table_set16(os2, 62, 0x0200);
    test_table_set16(test_font_table(made, "head"), 44, 0x0001);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_UINT(style.weight, ==, 700);
    TB_TEST_EXPECT(style.italic);
    tb_font_metrics metrics = {0};
    tb_font_get_metrics(built.font, &metrics);
    TB_TEST_EXPECT_INT(metrics.cap_height, ==, 0);
    TB_TEST_EXPECT_INT(metrics.x_height, ==, 0);
    close_built(&built);

    /* Without OS/2: head's bold and italic flags. */
    test_font_remove(made, "OS/2");
    test_table_set16(test_font_table(made, "head"), 44, 0x0002);
    built = open_built(made);
    TB_TEST_REQUIRE_UINT(built.result, ==, TB_FONT_OK);
    tb_font_get_style(built.font, &style);
    TB_TEST_EXPECT_UINT(style.weight, ==, 400);
    TB_TEST_EXPECT(style.italic);
    close_built(&built);
    test_font_destroy(made);
}

TB_TEST(font_truetype, the_error_is_optional_and_cleared_on_success)
{
    test_font *made = test_font_create();
    uint8_t *bytes = tb_malloc(FONT_BUFFER);
    const size_t length = test_font_write(made, bytes, FONT_BUFFER);
    tb_font *font = NULL;
    TB_TEST_REQUIRE_UINT(tb_font_open(bytes, length, NULL, &font, NULL), ==, TB_FONT_OK);
    tb_arena *arena = tb_arena_create(0);
    tb_font_outline outline = {0};
    tb_font_error error = {TB_FONT_MALFORMED, "x", "x"};
    TB_TEST_EXPECT_UINT(tb_font_get_outline(font, 1, arena, &outline, &error), ==, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_FONT_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    tb_arena_destroy(arena);
    tb_font_close(font);
    tb_font_close(NULL);
    tb_free(bytes);
    test_font_destroy(made);
}

TB_TEST(font_truetype, every_result_has_a_name)
{
    TB_TEST_EXPECT_STRING(tb_font_result_name(TB_FONT_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_font_result_name(TB_FONT_MALFORMED), ==, "malformed");
    TB_TEST_EXPECT_STRING(tb_font_result_name(TB_FONT_TRUNCATED), ==, "truncated");
    TB_TEST_EXPECT_STRING(tb_font_result_name(TB_FONT_UNSUPPORTED), ==, "unsupported");
    TB_TEST_EXPECT_STRING(tb_font_result_name(TB_FONT_TOO_LARGE), ==, "too large");
    TB_TEST_EXPECT_STRING(tb_font_result_name((tb_font_result)99), ==, "unknown result");
}

static tb_font *contract_font;
static tb_arena *contract_arena;

static void font_contract(void *context)
{
    const size_t choice = *(const size_t *)context;
    tb_font_glyph_metrics metrics = {0};
    tb_font_outline outline = {0};
    tb_font *font = NULL;
    switch (choice)
    {
    case 0:
        tb_font_get_glyph_metrics(contract_font, 228, &metrics);
        break;
    case 1:
        (void)tb_font_get_outline(contract_font, 228, contract_arena, &outline, NULL);
        break;
    default:
        (void)tb_font_open(NULL, 0, NULL, &font, NULL);
        break;
    }
}

TB_TEST(font_truetype, contract_violations_are_assertions)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("assertions disabled");
    }
    contract_font = open_file("Silkscreen-Regular.ttf");
    contract_arena = tb_arena_create(0);
    for (size_t choice = 0; choice < 3; choice++)
    {
        TB_TEST_CONTEXT("case %zu", choice);
        TB_TEST_EXPECT_TRAP(font_contract, &choice, "assertion failed");
    }
    tb_arena_destroy(contract_arena);
    tb_font_close(contract_font);
}
