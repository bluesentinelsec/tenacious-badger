/* Text layout: glyphs placed by their advances and kerning as the font
 * gives them, lines broken at spaces, inside long words and at \n, lines
 * aligned and spaced, text measured without its glyphs, carets between
 * characters, bad UTF-8 replaced, and pixel fonts on whole pixels. */
#include "badger/arena.h"
#include "badger/font.h"
#include "badger/font_layout.h"
#include "badger/font_raster.h"
#include "badger/str.h"
#include "tb_test.h"
#include "test_data.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

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

/* What a run of ASCII advances, kerned, at a size: worked out from the
 * font's own measures, as layout should. */
static float expected_width(const tb_font *font, const char *text, float size)
{
    const float scale = tb_font_scale(font, size);
    float pen = 0.0f;
    uint16_t previous = 0;

    for (size_t index = 0; text[index] != '\0'; index++)
    {
        const uint16_t glyph = tb_font_find_glyph(font, (uint8_t)text[index]);
        tb_font_glyph_metrics metrics = {0};
        tb_font_get_glyph_metrics(font, glyph, &metrics);
        if (index > 0)
        {
            pen += (float)tb_font_get_kerning(font, previous, glyph) * scale;
        }
        pen += (float)metrics.advance_width * scale;
        previous = glyph;
    }
    return pen;
}

static tb_font_text_size measure(const tb_font *font, const char *text, float size,
                                 const tb_font_layout_options *options)
{
    tb_font_text_size measured = {0};
    tb_font_measure_text(font, tb_text_from_cstr(text), size, options, &measured);
    return measured;
}

/* ---------------------------------------------------------------------------
 * Placing glyphs.
 * ------------------------------------------------------------------------- */

/* Each glyph's pen is the sum of the advances before it plus the kerning
 * between each pair, from tb_font; Roboto kerns A and V closer. */
TB_TEST(font_layout, glyphs_advance_and_kern_as_the_font_says)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    const char *const text = "AVATAR To";
    const float scale = tb_font_scale(font, 32.0f);
    const uint16_t a = tb_font_find_glyph(font, 'A');
    const uint16_t v = tb_font_find_glyph(font, 'V');

    TB_TEST_REQUIRE_INT(tb_font_get_kerning(font, a, v), <, 0);
    tb_font_layout_text(font, tb_text_from_cstr(text), 32.0f, NULL, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.glyph_count, ==, 9);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 1);
    TB_TEST_EXPECT_FLOAT(layout.pixels_per_em, ==, 32.0f);
    TB_TEST_EXPECT_UINT(layout.mode, ==, TB_FONT_RASTER_SMOOTH);
    TB_TEST_EXPECT_FLOAT(layout.glyphs[0].x, ==, 0.0f);
    for (size_t index = 1; index < layout.glyph_count; index++)
    {
        char prefix[16] = {0};
        memcpy(prefix, text, index);
        /* The pen of each glyph is the width of the text before it, plus
         * its kerning against the glyph before. */
        const float kern = (float)tb_font_get_kerning(font, layout.glyphs[index - 1].glyph,
                                                      layout.glyphs[index].glyph) *
                           scale;
        TB_TEST_EXPECT_FLOAT_NEAR(layout.glyphs[index].x,
                                  expected_width(font, prefix, 32.0f) + kern, 0.001f);
        TB_TEST_EXPECT_UINT(layout.glyphs[index].offset, ==, index);
        TB_TEST_EXPECT_UINT(layout.glyphs[index].end, ==, index + 1);
        TB_TEST_EXPECT_UINT(layout.glyphs[index].code_point, ==, (uint8_t)text[index]);
    }
    TB_TEST_EXPECT_UINT(layout.glyphs[1].glyph, ==, v);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.width, expected_width(font, text, 32.0f), 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[0].width, layout.width, 0.0f);
    /* Kerning made the pair narrower than its advances. */
    TB_TEST_EXPECT_FLOAT(layout.glyphs[1].x, <, layout.glyphs[0].advance);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* Lines are as far apart as the font's line height, times the spacing, and
 * each baseline is the ascent below its line's top. */
TB_TEST(font_layout, lines_are_spaced_by_the_font_line_height)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_line_metrics metrics = {0};
    tb_font_layout layout = {0};
    const tb_font_layout_options spaced = {.line_spacing = 1.5f};

    tb_font_get_line_metrics(font, 20.0f, &metrics);
    tb_font_layout_text(font, TB_TEXT("one\ntwo\nthree"), 20.0f, NULL, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 3);
    TB_TEST_EXPECT_FLOAT(layout.line_height, ==, metrics.line_height);
    TB_TEST_EXPECT_FLOAT(layout.ascent, ==, metrics.ascent);
    TB_TEST_EXPECT_FLOAT(layout.lines[2].top, ==, 2.0f * metrics.line_height);
    TB_TEST_EXPECT_FLOAT(layout.lines[1].baseline, ==, metrics.line_height + metrics.ascent);
    TB_TEST_EXPECT_FLOAT(layout.height, ==, 3.0f * metrics.line_height);

    tb_font_layout_text(font, TB_TEXT("one\ntwo"), 20.0f, &spaced, arena, &layout);
    TB_TEST_EXPECT_FLOAT(layout.line_height, ==, metrics.line_height * 1.5f);
    TB_TEST_EXPECT_FLOAT(layout.height, ==, 2.0f * metrics.line_height * 1.5f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* ---------------------------------------------------------------------------
 * Breaking lines.
 * ------------------------------------------------------------------------- */

/* A line breaks before the word that would end past the width, after the
 * spaces before it, which stay on the line they end and are not counted in
 * its width. A word that fits exactly stays. */
TB_TEST(font_layout, lines_break_at_spaces_before_a_word_that_does_not_fit)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    tb_font_layout_options options = {.wrap_width = expected_width(font, "aaa  bbb", 16.0f)};

    tb_font_layout_text(font, TB_TEXT("aaa  bbb ccc"), 16.0f, &options, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 2);
    TB_TEST_EXPECT_UINT(layout.lines[0].begin, ==, 0);
    TB_TEST_EXPECT_UINT(layout.lines[0].end, ==, 9);
    TB_TEST_EXPECT_UINT(layout.lines[0].glyph_count, ==, 9);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[0].width, expected_width(font, "aaa  bbb", 16.0f),
                              0.001f);
    TB_TEST_EXPECT_UINT(layout.lines[1].begin, ==, 9);
    TB_TEST_EXPECT_UINT(layout.lines[1].end, ==, 12);
    TB_TEST_EXPECT_UINT(layout.lines[1].first_glyph, ==, 9);
    TB_TEST_EXPECT_UINT(layout.lines[1].glyph_count, ==, 3);
    TB_TEST_EXPECT_FLOAT(layout.glyphs[9].x, ==, 0.0f);
    TB_TEST_EXPECT_UINT(layout.glyphs[9].line, ==, 1);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[1].width, expected_width(font, "ccc", 16.0f), 0.001f);

    /* A little narrower, and "bbb" goes down too. */
    options.wrap_width -= 1.0f;
    tb_font_layout_text(font, TB_TEXT("aaa  bbb ccc"), 16.0f, &options, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 2);
    TB_TEST_EXPECT_UINT(layout.lines[0].end, ==, 5);
    TB_TEST_EXPECT_UINT(layout.lines[1].begin, ==, 5);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[1].width, expected_width(font, "bbb ccc", 16.0f),
                              0.001f);

    /* Spaces past the width never break a line, nor begin one. */
    options.wrap_width = expected_width(font, "aaa", 16.0f);
    tb_font_layout_text(font, TB_TEXT("aaa     "), 16.0f, &options, arena, &layout);
    TB_TEST_EXPECT_UINT(layout.line_count, ==, 1);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.width, expected_width(font, "aaa", 16.0f), 0.001f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* A word wider than the width breaks between any two characters, and a
 * width narrower than a character still puts one on each line. */
TB_TEST(font_layout, long_words_break_anywhere)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    tb_font_layout_options options = {.wrap_width = expected_width(font, "nnnn", 16.0f) + 0.5f};

    tb_font_layout_text(font, TB_TEXT("nnnnnnnnnn"), 16.0f, &options, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 3);
    TB_TEST_EXPECT_UINT(layout.lines[0].end, ==, 4);
    TB_TEST_EXPECT_UINT(layout.lines[1].begin, ==, 4);
    TB_TEST_EXPECT_UINT(layout.lines[1].end, ==, 8);
    TB_TEST_EXPECT_UINT(layout.lines[2].glyph_count, ==, 2);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[1].width, expected_width(font, "nnnn", 16.0f), 0.001f);

    /* After words that fit, a long word starts its own line, then breaks. */
    tb_font_layout_text(font, TB_TEXT("nn nnnnnnnn"), 16.0f, &options, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 3);
    TB_TEST_EXPECT_UINT(layout.lines[0].end, ==, 3);
    TB_TEST_EXPECT_UINT(layout.lines[1].end, ==, 7);

    options.wrap_width = 1.0f;
    tb_font_layout_text(font, TB_TEXT("xyz"), 16.0f, &options, arena, &layout);
    TB_TEST_EXPECT_UINT(layout.line_count, ==, 3);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* \n ends a line, even an empty one; \r is ignored, so \r\n ends one; a
 * text that ends with \n has an empty last line, where a caret can go. */
TB_TEST(font_layout, newlines_end_lines)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};

    tb_font_layout_text(font, TB_TEXT("a\n\nb\r\nc\n"), 16.0f, NULL, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 5);
    TB_TEST_EXPECT_UINT(layout.glyph_count, ==, 3);
    TB_TEST_EXPECT_UINT(layout.lines[0].end, ==, 1);
    TB_TEST_EXPECT_UINT(layout.lines[1].begin, ==, 2);
    TB_TEST_EXPECT_UINT(layout.lines[1].glyph_count, ==, 0);
    TB_TEST_EXPECT_UINT(layout.lines[2].begin, ==, 3);
    TB_TEST_EXPECT_UINT(layout.lines[2].end, ==, 5);
    TB_TEST_EXPECT_UINT(layout.lines[2].glyph_count, ==, 1);
    TB_TEST_EXPECT_UINT(layout.lines[3].begin, ==, 6);
    TB_TEST_EXPECT_UINT(layout.lines[4].begin, ==, 8);
    TB_TEST_EXPECT_UINT(layout.lines[4].glyph_count, ==, 0);
    TB_TEST_EXPECT_FLOAT(layout.lines[4].width, ==, 0.0f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* ---------------------------------------------------------------------------
 * Alignment, measuring, and edge cases.
 * ------------------------------------------------------------------------- */

/* Lines sit across the wrap width, or across the widest line without one. */
TB_TEST(font_layout, lines_align_across_their_box)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    const float wide = expected_width(font, "wide line", 16.0f);
    const float narrow = expected_width(font, "ok", 16.0f);
    tb_font_layout_options options = {.align = TB_FONT_ALIGN_CENTER};

    tb_font_layout_text(font, TB_TEXT("wide line\nok"), 16.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT(layout.lines[0].x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[1].x, (wide - narrow) * 0.5f, 0.001f);
    options.align = TB_FONT_ALIGN_RIGHT;
    tb_font_layout_text(font, TB_TEXT("wide line\nok"), 16.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[1].x, wide - narrow, 0.001f);
    options.wrap_width = 200.0f;
    tb_font_layout_text(font, TB_TEXT("wide line\nok"), 16.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[0].x, 200.0f - wide, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(layout.lines[1].x, 200.0f - narrow, 0.001f);
    options.align = TB_FONT_ALIGN_LEFT;
    tb_font_layout_text(font, TB_TEXT("wide line\nok"), 16.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT(layout.lines[1].x, ==, 0.0f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* Measuring gives the box of the layout, without allocating it. */
TB_TEST(font_layout, measuring_matches_the_layout)
{
    static const char *const texts[] = {"", "Hello", "Hello, world\nand more text to wrap",
                                        "   leading spaces", "x\n"};
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    const tb_font_layout_options options = {.wrap_width = 90.0f, .align = TB_FONT_ALIGN_CENTER};

    for (size_t index = 0; index < sizeof(texts) / sizeof(texts[0]); index++)
    {
        tb_font_layout layout = {0};
        const tb_font_text_size measured = measure(font, texts[index], 18.0f, &options);
        tb_font_layout_text(font, tb_text_from_cstr(texts[index]), 18.0f, &options, arena, &layout);
        TB_TEST_EXPECT_FLOAT(measured.width, ==, layout.width);
        TB_TEST_EXPECT_FLOAT(measured.height, ==, layout.height);
        TB_TEST_EXPECT_UINT(measured.line_count, ==, layout.line_count);
    }
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* Empty text is one empty line, a line high; a size that draws nothing
 * places nothing. */
TB_TEST(font_layout, empty_text_and_empty_sizes)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    tb_font_caret caret = {0};
    tb_font_text_size measured = {0};

    tb_font_layout_text(font, (tb_text){0}, 16.0f, NULL, arena, &layout);
    TB_TEST_EXPECT_UINT(layout.glyph_count, ==, 0);
    TB_TEST_EXPECT_UINT(layout.line_count, ==, 1);
    TB_TEST_EXPECT_FLOAT(layout.width, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(layout.height, ==, layout.line_height);
    TB_TEST_EXPECT_FLOAT(layout.height, >, 0.0f);
    tb_font_layout_caret(&layout, 0, &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 0);
    TB_TEST_EXPECT_FLOAT(caret.x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(caret.bottom, ==, layout.line_height);

    tb_font_layout_text(font, TB_TEXT("abc"), 0.0f, NULL, arena, &layout);
    TB_TEST_EXPECT_UINT(layout.glyph_count, ==, 3);
    TB_TEST_EXPECT_FLOAT(layout.width, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(layout.height, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(layout.pixels_per_em, ==, 0.0f);
    measured = measure(font, "abc", NAN, NULL);
    TB_TEST_EXPECT_FLOAT(measured.width, ==, 0.0f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* A byte that begins no character is U+FFFD, the font's glyph for it, and
 * one byte; the characters around it are read as they are. */
TB_TEST(font_layout, bad_utf8_is_replaced)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};

    tb_font_layout_text(font, TB_TEXT("a\xFF\xC3\xA9\xE2\x82z"), 16.0f, NULL, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.glyph_count, ==, 6);
    TB_TEST_EXPECT_UINT(layout.glyphs[0].code_point, ==, 'a');
    TB_TEST_EXPECT_UINT(layout.glyphs[1].code_point, ==, 0xFFFD);
    TB_TEST_EXPECT_UINT(layout.glyphs[1].glyph, ==, tb_font_find_glyph(font, 0xFFFD));
    TB_TEST_EXPECT_UINT(layout.glyphs[1].end, ==, 2);
    TB_TEST_EXPECT_UINT(layout.glyphs[2].code_point, ==, 0xE9);
    TB_TEST_EXPECT_UINT(layout.glyphs[2].end, ==, 4);
    /* A sequence cut short: each of its bytes is replaced. */
    TB_TEST_EXPECT_UINT(layout.glyphs[3].code_point, ==, 0xFFFD);
    TB_TEST_EXPECT_UINT(layout.glyphs[4].code_point, ==, 0xFFFD);
    TB_TEST_EXPECT_UINT(layout.glyphs[5].code_point, ==, 'z');
    /* A character the font lacks draws the missing glyph. */
    tb_font_layout_text(font, TB_TEXT("\xF0\x9F\x98\x80"), 16.0f, NULL, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.glyph_count, ==, 1);
    TB_TEST_EXPECT_UINT(layout.glyphs[0].glyph, ==, 0);
    TB_TEST_EXPECT_UINT(layout.glyphs[0].code_point, ==, 0x1F600);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* A tab is as wide as four spaces, and kerns with nothing. */
TB_TEST(font_layout, a_tab_is_four_spaces)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");

    TB_TEST_EXPECT_FLOAT_NEAR(measure(font, "a\tb", 16.0f, NULL).width,
                              measure(font, "a    b", 16.0f, NULL).width, 0.01f);
    tb_font_close(font);
}

/* ---------------------------------------------------------------------------
 * Carets.
 * ------------------------------------------------------------------------- */

/* A caret stands before the character at its offset, at the start of one
 * it is inside, at the end of the text past it, at the start of the line
 * after a wrap, and at the end of the line a \n ends. */
TB_TEST(font_layout, carets_stand_between_characters)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    tb_font_caret caret = {0};
    const tb_font_layout_options options = {.wrap_width = expected_width(font, "ab", 16.0f) + 0.5f,
                                            .align = TB_FONT_ALIGN_RIGHT};

    /* "ab \xC3\xA9d\nx": a, b, space, é (two bytes), d, newline, x. */
    tb_font_layout_text(font,
                        TB_TEXT("ab \xC3\xA9"
                                "d\nx"),
                        16.0f, &options, arena, &layout);
    TB_TEST_REQUIRE_UINT(layout.line_count, ==, 3);
    TB_TEST_REQUIRE_UINT(layout.lines[1].begin, ==, 3);

    tb_font_layout_caret(&layout, 1, &caret);
    TB_TEST_EXPECT_UINT(caret.line, ==, 0);
    TB_TEST_EXPECT_FLOAT(caret.x, ==, layout.lines[0].x + layout.glyphs[1].x);
    /* The wrap: at the start of the next line. */
    tb_font_layout_caret(&layout, 3, &caret);
    TB_TEST_EXPECT_UINT(caret.line, ==, 1);
    TB_TEST_EXPECT_FLOAT(caret.x, ==, layout.lines[1].x);
    TB_TEST_EXPECT_FLOAT(caret.top, ==, layout.line_height);
    /* Inside é: at its start. */
    tb_font_layout_caret(&layout, 4, &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 3);
    tb_font_layout_caret(&layout, 5, &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 5);
    TB_TEST_EXPECT_FLOAT(caret.x, ==, layout.lines[1].x + layout.glyphs[4].x);
    /* The \n: the end of the line it ends. */
    tb_font_layout_caret(&layout, 6, &caret);
    TB_TEST_EXPECT_UINT(caret.line, ==, 1);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 6);
    TB_TEST_EXPECT_FLOAT_NEAR(
        caret.x, layout.lines[1].x + layout.glyphs[4].x + layout.glyphs[4].advance, 0.0f);
    /* Past the end: the end. */
    tb_font_layout_caret(&layout, 100, &caret);
    TB_TEST_EXPECT_UINT(caret.line, ==, 2);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 8);
    TB_TEST_EXPECT_FLOAT(caret.bottom, ==, 3.0f * layout.line_height);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* A point finds the caret nearest it: on its line, by which half of a
 * character it is over; above or below every line, the first or last. */
TB_TEST(font_layout, a_point_finds_the_nearest_caret)
{
    tb_font *const font = open_font("Roboto-Regular.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    tb_font_caret caret = {0};

    tb_font_layout_text(font, TB_TEXT("abc\nde"), 20.0f, NULL, arena, &layout);
    tb_font_layout_hit(&layout, layout.glyphs[1].x + layout.glyphs[1].advance * 0.4f, 1.0f, &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 1);
    tb_font_layout_hit(&layout, layout.glyphs[1].x + layout.glyphs[1].advance * 0.6f, -50.0f,
                       &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 2);
    TB_TEST_EXPECT_UINT(caret.line, ==, 0);
    tb_font_layout_hit(&layout, -10.0f, layout.line_height * 1.5f, &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 4);
    TB_TEST_EXPECT_UINT(caret.line, ==, 1);
    tb_font_layout_hit(&layout, 1000.0f, 1000.0f, &caret);
    TB_TEST_EXPECT_UINT(caret.offset, ==, 6);
    TB_TEST_EXPECT_FLOAT(caret.x, ==, layout.lines[1].width);
    tb_arena_destroy(arena);
    tb_font_close(font);
}

/* ---------------------------------------------------------------------------
 * Pixel fonts.
 * ------------------------------------------------------------------------- */

/* A pixel font is laid out at the whole multiple of its size nearest the
 * size asked for, at least 1, aliased, every pen and line on whole pixels. */
TB_TEST(font_layout, pixel_fonts_lay_out_on_whole_pixels_at_whole_multiples)
{
    tb_font *const font = open_font("Px437_IBM_VGA_8x16.ttf");
    tb_arena *const arena = tb_arena_create(0);
    tb_font_layout layout = {0};
    const tb_font_layout_options options = {
        .pixel_size = 16.0f, .align = TB_FONT_ALIGN_CENTER, .wrap_width = 101.0f};

    tb_font_layout_text(font, TB_TEXT("VGA text"), 20.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT(layout.pixels_per_em, ==, 16.0f);
    TB_TEST_EXPECT_UINT(layout.mode, ==, TB_FONT_RASTER_ALIASED);
    /* Each cell of the VGA font is 8 pixels across at 16 to the em. */
    TB_TEST_EXPECT_FLOAT(layout.glyphs[3].x, ==, 24.0f);
    TB_TEST_EXPECT_FLOAT(layout.width, ==, 64.0f);
    TB_TEST_EXPECT_FLOAT(layout.lines[0].x, ==, roundf((101.0f - 64.0f) * 0.5f));
    TB_TEST_EXPECT_FLOAT(layout.ascent, ==, roundf(layout.ascent));
    TB_TEST_EXPECT_FLOAT(layout.line_height, ==, roundf(layout.line_height));

    tb_font_layout_text(font, TB_TEXT("VGA"), 24.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT(layout.pixels_per_em, ==, 32.0f);
    TB_TEST_EXPECT_FLOAT(layout.glyphs[1].x, ==, 16.0f);
    tb_font_layout_text(font, TB_TEXT("VGA"), 3.0f, &options, arena, &layout);
    TB_TEST_EXPECT_FLOAT(layout.pixels_per_em, ==, 16.0f);
    tb_arena_destroy(arena);
    tb_font_close(font);
}
