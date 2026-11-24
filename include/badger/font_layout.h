/** @file font_layout.h
 * @brief Text laid out in lines: UTF-8 turned into glyphs placed in pixels
 * at a size, kerned, broken into lines at a width, and aligned; measured
 * without drawing; and the places of a caret between its characters.
 *
 * This is the third part of the `font` module, after badger/font.h, which
 * reads fonts, and badger/font_raster.h, which draws their glyphs. Layout
 * knows nothing of a renderer: the canvas (badger/canvas.h) draws what it
 * lays out, and an interface measures with it.
 *
 * @code
 * const tb_font_layout_options options = {.wrap_width = 300.0f, .align = TB_FONT_ALIGN_CENTER};
 * tb_font_layout layout = {0};
 * tb_font_layout_text(font, TB_TEXT("Hello, world"), 24.0f, &options, arena, &layout);
 * for (size_t index = 0; index < layout.glyph_count; index++)
 * {
 *     const tb_font_layout_glyph *glyph = &layout.glyphs[index];
 *     const tb_font_layout_line *line = &layout.lines[glyph->line];
 *     // draw glyph->glyph with its pen at (line->x + glyph->x, line->baseline)
 * }
 * @endcode
 *
 * **Coordinates** are pixels, from the top left of the text's box, x to the
 * right and y down. Each line's glyphs are placed from the line's left
 * edge, and each glyph's pen is on the line's baseline.
 *
 * **Characters.** Text is UTF-8. Each byte that does not begin a valid
 * character is read as U+FFFD, the replacement character. A character the
 * font has no glyph for draws the font's missing glyph, glyph 0. `\n` ends a
 * line; `\r` is ignored, so `\r\n` ends one line; a tab is as wide as four
 * spaces.
 *
 * **Lines** break at `\n`, and, given a width to wrap at, before a word
 * that would end past it: after the spaces that end the word before, which
 * stay on the line they end and are not counted in its width. A word wider
 * than the whole width breaks between any two of its characters. Every line
 * holds at least one character, so a width narrower than one character
 * gives a line to each.
 *
 * **Placement.** Each glyph advances the pen by its advance, plus the
 * kerning (tb_font_get_kerning()) between it and the glyph before it on its
 * line, at the size's scale, unrounded: drawing places glyphs at fractions
 * of a pixel through the atlas's subpixel bins. A pixel font is laid out in
 * whole pixels (#tb_font_layout_options).
 *
 * **Not done.** Shaping (ligatures, and the joining and reordering of
 * complex scripts such as Arabic or Devanagari), right-to-left and
 * bidirectional text, falling back to another font for a character this
 * one lacks, combining marks placed on their base, and colour emoji. Each
 * character is one glyph, left to right.
 *
 * @par Threading
 * Thread-safe: layout only reads its font, which is thread-safe to read,
 * and writes the layout and arena it is given, which are the caller's.
 */
#ifndef TB_FONT_LAYOUT_H
#define TB_FONT_LAYOUT_H

#include "badger/api.h"
#include "badger/arena.h"
#include "badger/font.h"
#include "badger/font_raster.h"
#include "badger/text.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief How the lines of a text sit across its box. */
typedef enum tb_font_align
{
    TB_FONT_ALIGN_LEFT,   /**< Against the left edge. */
    TB_FONT_ALIGN_CENTER, /**< In the middle. */
    TB_FONT_ALIGN_RIGHT   /**< Against the right edge. */
} tb_font_align;

/** @brief How to lay out text. Zero lays out one line per `\n`, left
 * aligned, lines as far apart as the font says, smooth. */
typedef struct tb_font_layout_options
{
    /** The widest a line may be, in pixels: longer lines are broken. 0, or
     * anything not more than 0, breaks lines only at `\n`. */
    float wrap_width;
    /** How lines sit across the box: the wrap width when there is one, or
     * else the widest line. */
    tb_font_align align;
    /** The font's line height (tb_font_get_line_metrics()) is multiplied by
     * this, from baseline to baseline: 1.5 spaces lines half as far again.
     * 0, or anything not more than 0, is 1. */
    float line_spacing;
    /** For a pixel font, the size in pixels to the em it was drawn for,
     * such as 16 for Px437 IBM VGA 8x16, or 8 for Silkscreen: the text is
     * then laid out at the whole multiple of it nearest the size asked for
     * (1 at least), every glyph on a whole pixel, and drawn aliased, so each
     * of the font's pixels is a square of whole pixels. 0 lays out smooth
     * text at the size asked for. */
    float pixel_size;
} tb_font_layout_options;

/** @brief A character placed in a layout. */
typedef struct tb_font_layout_glyph
{
    uint16_t glyph;      /**< Its glyph in the font; 0, the missing glyph, for one it lacks. */
    uint32_t code_point; /**< The character: U+FFFD for a byte that begins none. */
    size_t offset;       /**< The byte of the text where it begins. */
    size_t end;          /**< The byte after it. */
    size_t line;         /**< Its line, an index of the layout's lines. */
    /** Its pen, in pixels from its line's left edge (`x` of its line), its
     * kerning against the glyph before it included. */
    float x;
    /** How far the pen moves after it, in pixels, before the next glyph's
     * kerning: the caret after it is at `x + advance`. */
    float advance;
} tb_font_layout_glyph;

/** @brief A line of a layout. */
typedef struct tb_font_layout_line
{
    size_t first_glyph; /**< Its first glyph, an index of the layout's glyphs. */
    size_t glyph_count; /**< Its glyphs, the spaces that end it included. */
    size_t begin;       /**< The byte of the text where it begins. */
    /** The byte where it ends: before the `\n` that ends it, or after the
     * spaces where it wraps. */
    size_t end;
    float x;        /**< Its left edge, in pixels from the box's, as it is aligned. */
    float top;      /**< Its top, in pixels from the box's: a line height a line. */
    float baseline; /**< Its baseline, in pixels from the box's top. */
    float width;    /**< Its width in pixels, the spaces that end it not counted. */
} tb_font_layout_line;

/** @brief Text laid out: its glyphs, its lines, and its size. */
typedef struct tb_font_layout
{
    tb_font_layout_glyph *glyphs; /**< In the order of the text; from the arena. */
    size_t glyph_count;           /**< Characters placed: every one but `\n` and `\r`. */
    tb_font_layout_line *lines;   /**< From the top; from the arena. */
    size_t line_count;            /**< At least 1: empty text is one empty line. */
    /** The widest line's width, in pixels: with `height`, the size of the
     * text's box of lines. */
    float width;
    float height;      /**< The lines' height, in pixels: `line_count * line_height`. */
    float line_height; /**< From one baseline to the next, in pixels. */
    float ascent;      /**< From a line's top down to its baseline, in pixels. */
    /** The size it was laid out at, in pixels to the em: the size asked
     * for, or for a pixel font its whole multiple of the font's size. */
    float pixels_per_em;
    /** How to draw its glyphs: aliased for a pixel font, else smooth. */
    tb_font_raster_mode mode;
} tb_font_layout;

/** @brief The size of text laid out, without its glyphs. */
typedef struct tb_font_text_size
{
    float width;       /**< The widest line, in pixels. */
    float height;      /**< The lines' height, in pixels. */
    size_t line_count; /**< Lines; at least 1. */
} tb_font_text_size;

/** @brief Where a caret goes, between two characters of a layout. */
typedef struct tb_font_caret
{
    /** The byte offset it stands at: the offset asked for, moved back to the
     * start of the character it was inside, and to the end of the text if
     * it was past it. */
    size_t offset;
    size_t line;  /**< Its line. */
    float x;      /**< In pixels from the box's left edge, alignment included. */
    float top;    /**< The top of its line, in pixels from the box's top. */
    float bottom; /**< The bottom of its line: `top` plus a line height. */
} tb_font_caret;

/**
 * @brief Lay out text: place each of its characters, break it into lines,
 * and align them.
 * @param font Required.
 * @param text UTF-8; any bytes. Borrowed for the call.
 * @param pixels_per_em The size, in pixels to the em; a size that is not a
 * positive finite number places every glyph at 0 with no advance.
 * @param options Optional; NULL is every default.
 * @param arena Required; the layout's glyphs and lines come from it, and
 * stay valid until it is reset or rewound.
 * @param[out] layout Required; the layout.
 * @par Errors Running out of memory in the arena ends the process.
 */
TB_API void tb_font_layout_text(const tb_font *font, tb_text text, float pixels_per_em,
                                const tb_font_layout_options *options, tb_arena *arena,
                                tb_font_layout *layout);

/**
 * @brief Measure text as tb_font_layout_text() would lay it out, without
 * keeping its glyphs: what an interface sizes a box by. Allocates nothing.
 * @param font Required.
 * @param text UTF-8.
 * @param pixels_per_em The size.
 * @param options Optional; NULL is every default.
 * @param[out] size Required; the widest line, the lines' height, and how
 * many there are, as tb_font_layout_text() gives them.
 */
TB_API void tb_font_measure_text(const tb_font *font, tb_text text, float pixels_per_em,
                                 const tb_font_layout_options *options, tb_font_text_size *size);

/**
 * @brief Find where a caret goes at a byte offset of the text laid out: the
 * boundary before the character that begins there.
 *
 * An offset where a line wraps is at the start of the line after; the
 * offset of a `\n` is at the end of the line it ends.
 *
 * @param layout Required; as tb_font_layout_text() made it.
 * @param offset A byte offset of the text; any value.
 * @param[out] caret Required.
 */
TB_API void tb_font_layout_caret(const tb_font_layout *layout, size_t offset, tb_font_caret *caret);

/**
 * @brief Find the caret nearest a point: where a click in text puts it.
 *
 * The line is the one the point is across from, the first or the last for
 * a point above or below them all, and the caret the boundary between
 * characters nearest the point's x on that line.
 *
 * @param layout Required; as tb_font_layout_text() made it.
 * @param x Pixels from the box's left edge.
 * @param y Pixels from the box's top.
 * @param[out] caret Required.
 */
TB_API void tb_font_layout_hit(const tb_font_layout *layout, float x, float y,
                               tb_font_caret *caret);

#endif
