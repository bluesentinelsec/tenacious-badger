/** @file font.h
 * @brief TrueType fonts (`.ttf`), read from bytes in memory: which glyph
 * draws a character, its outline as lines and quadratic curves, how far it
 * advances, how it kerns against its neighbour, and the font's metrics,
 * family, style, and weight.
 *
 * A font is opened once from the bytes of its file, which it borrows, and is
 * then only read. Everything below works in the font's own units, of which
 * there are `units_per_em` to the em, with y up from the baseline; scaling to
 * pixels, and rasterizing, are for the caller.
 *
 * **What is read.** The tables `head`, `hhea`, `maxp`, `hmtx`, `loca`, `glyf`,
 * and `cmap` are required. Characters map through a Unicode `cmap` subtable
 * of format 12 (all of Unicode), or else of format 4 (the Basic Multilingual
 * Plane). Outlines come from `glyf`, simple glyphs and compound glyphs of
 * other glyphs moved, scaled, or transformed, to any depth up to a limit.
 * Kerning comes from the pair adjustments (lookup type 2, both formats,
 * directly or through an extension lookup) of the `kern` feature of `GPOS`,
 * or, in a font without one, from the first horizontal format 0 subtable of
 * the older `kern` table. The optional `OS/2` table gives the weight, the
 * italic flag, and the cap and x heights, and the optional `name` table the
 * family and style.
 *
 * **What is not.** Fonts with PostScript outlines (`OTTO`, `.otf` with `CFF`)
 * and font collections (`.ttc`) are refused as unsupported. Hinting
 * instructions, variations of a variable font (its default instance is
 * read), vertical metrics, glyph substitution, and every GPOS adjustment but
 * the advance of a pair are ignored.
 *
 * **The bytes are untrusted.** Every offset, count, and length is checked
 * before it is followed. Opening checks every table that later queries rely
 * on: the table directory, the header tables, the metrics, `loca`, the
 * chosen `cmap` subtable, the kerning tables, `OS/2`, and the names it uses.
 * A glyph's outline is checked when it is read, so one damaged glyph spoils
 * only itself. Damage is a result with a reason, never a read out of bounds.
 *
 * @par Threading
 * A font is immutable once open: every function that takes a `const
 * tb_font *` is thread-safe, for as long as the font is open.
 * tb_font_open() is thread-safe; the font is the caller's. tb_font_close() is
 * externally synchronized: nothing else may use the font while it closes.
 * tb_font_get_outline() is as thread-safe as the arena it is given.
 */
#ifndef TB_FONT_H
#define TB_FONT_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What happened, or why not. */
typedef enum tb_font_result
{
    TB_FONT_OK,          /**< Success. */
    TB_FONT_MALFORMED,   /**< The bytes are damaged, or are not a TrueType font. */
    TB_FONT_TRUNCATED,   /**< A table the directory names runs past the end of the bytes. */
    TB_FONT_UNSUPPORTED, /**< A valid font this reader cannot read, such as one with CFF outlines.
                          */
    TB_FONT_TOO_LARGE    /**< A compound glyph's outline is larger than the limits. */
} tb_font_result;

/** @brief Why opening a font, or reading an outline, failed. */
typedef struct tb_font_error
{
    tb_font_result result; /**< What failed; OK when nothing has. */
    /** The tag of the table at fault, such as "glyf"; "" when the fault is
     * in no one table, or when nothing failed. Static. */
    const char *table;
    /** A short phrase in lower case, such as "contour ends are out of
     * order"; "" when nothing failed. Static. */
    const char *reason;
} tb_font_error;

/** @brief Points in all the glyphs one compound glyph draws at most: one million. */
#define TB_FONT_MAX_OUTLINE_POINTS ((size_t)1 << 20)

/** @brief How deep compound glyphs may nest: a compound of a compound is depth 2. */
#define TB_FONT_MAX_COMPOUND_DEPTH 16

/** @brief A font open for reading. Opaque. */
typedef struct tb_font tb_font;

/**
 * @brief Open a font from the bytes of a `.ttf` file, and check its tables.
 * @param bytes Required; borrowed, and must stay valid and unchanged until
 * the font is closed. Need not be aligned.
 * @param length Bytes of the file.
 * @param allocator Where the font lives, or NULL for the heap.
 * @param[out] font Required; the font on OK, for tb_font_close(); NULL otherwise.
 * @param[out] error Optional; why opening failed, or OK and "" on success.
 * @return OK, or why not: MALFORMED for damage or bytes that are not a font,
 * TRUNCATED for a table cut off by the end of the bytes, UNSUPPORTED for a
 * font with PostScript outlines, a collection, or a font with no Unicode
 * character map this reader can read.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_font_result tb_font_open(const void *bytes, size_t length, const tb_allocator *allocator,
                                   tb_font **font, tb_font_error *error);

/** @brief Close a font and release its memory. @param font Optional; NULL does nothing. */
TB_API void tb_font_close(tb_font *font);

/** @brief The measures of a whole font, in font units. */
typedef struct tb_font_metrics
{
    uint16_t units_per_em; /**< Font units to the em: from 16 to 16384, often 1000 or 2048. */
    uint16_t glyph_count;  /**< Glyphs in the font; a glyph index is less than this. */
    int16_t ascender;      /**< Height above the baseline that lines leave room for (`hhea`). */
    int16_t descender;     /**< Depth below the baseline that lines leave room for; negative. */
    int16_t line_gap;      /**< Space between one line's descender and the next one's ascender. */
    /** Height of flat capital letters, such as H, above the baseline; 0
     * when the font does not say (an `OS/2` table before version 2, or none). */
    int16_t cap_height;
    int16_t x_height; /**< Height of flat lower-case letters, such as x; 0 when the font does not
                         say. */
    int16_t x_min;    /**< Left edge of the box around every glyph (`head`). */
    int16_t y_min;    /**< Bottom edge of the box around every glyph. */
    int16_t x_max;    /**< Right edge of the box around every glyph. */
    int16_t y_max;    /**< Top edge of the box around every glyph. */
} tb_font_metrics;

/**
 * @brief Get the measures of a font.
 * @param font Required. @param[out] metrics Required.
 */
TB_API void tb_font_get_metrics(const tb_font *font, tb_font_metrics *metrics);

/** @brief Weights of `OS/2`'s usWeightClass, from 100 for the thinnest to 900 for the blackest. */
enum
{
    TB_FONT_WEIGHT_THIN = 100,        /**< Thin, or hairline. */
    TB_FONT_WEIGHT_EXTRA_LIGHT = 200, /**< Extra light, or ultra light. */
    TB_FONT_WEIGHT_LIGHT = 300,       /**< Light. */
    TB_FONT_WEIGHT_REGULAR = 400,     /**< Regular, or normal. */
    TB_FONT_WEIGHT_MEDIUM = 500,      /**< Medium. */
    TB_FONT_WEIGHT_SEMI_BOLD = 600,   /**< Semibold, or demibold. */
    TB_FONT_WEIGHT_BOLD = 700,        /**< Bold. */
    TB_FONT_WEIGHT_EXTRA_BOLD = 800,  /**< Extra bold, or ultra bold. */
    TB_FONT_WEIGHT_BLACK = 900        /**< Black, or heavy. */
};

/** @brief What a font is called, and its style. */
typedef struct tb_font_style
{
    /** The family, such as "Roboto", in UTF-8 and ending in a NUL that
     * `family_length` does not count: the typographic family where the font
     * names one, else its family. "" when the font has no `name` table or no
     * family in it. The font's; valid until it is closed. */
    const char *family;
    size_t family_length; /**< Bytes of `family`. */
    /** The style within the family, such as "SemiBold Italic", as `family`
     * is: the typographic subfamily, else the subfamily. */
    const char *style;
    size_t style_length; /**< Bytes of `style`. */
    /** The weight, such as #TB_FONT_WEIGHT_REGULAR, from `OS/2`; without
     * one, BOLD or REGULAR as `head` marks the font bold or not. */
    uint16_t weight;
    /** Whether the font is italic or oblique, as `OS/2` or `head` marks it. */
    bool italic;
} tb_font_style;

/**
 * @brief Get the family, style, and weight of a font.
 * @param font Required. @param[out] style Required; its text is the font's.
 */
TB_API void tb_font_get_style(const tb_font *font, tb_font_style *style);

/**
 * @brief Find the glyph that draws a character.
 * @param font Required.
 * @param code_point A Unicode code point; any value is allowed.
 * @return The glyph's index, or 0, the font's "missing glyph", when the font
 * has none for the code point.
 */
TB_API uint16_t tb_font_find_glyph(const tb_font *font, uint32_t code_point);

/** @brief How a glyph sits on the line, in font units. */
typedef struct tb_font_glyph_metrics
{
    uint16_t advance_width;    /**< How far the pen moves right after the glyph. */
    int16_t left_side_bearing; /**< From the pen to the left edge of the glyph's box. */
} tb_font_glyph_metrics;

/**
 * @brief Get how far a glyph advances, and where it begins.
 * @param font Required.
 * @param glyph Less than the font's glyph count.
 * @param[out] metrics Required.
 */
TB_API void tb_font_get_glyph_metrics(const tb_font *font, uint16_t glyph,
                                      tb_font_glyph_metrics *metrics);

/**
 * @brief Get the kerning between two glyphs drawn side by side.
 * @param font Required.
 * @param left The glyph drawn first; any index.
 * @param right The glyph drawn after it; any index.
 * @return Font units to add to the left glyph's advance: negative to draw
 * the pair closer. 0 when the font has no kerning for the pair. Kerning from
 * several GPOS lookups is summed.
 */
TB_API int32_t tb_font_get_kerning(const tb_font *font, uint16_t left, uint16_t right);

/** @brief A point of an outline, in font units, y up. */
typedef struct tb_font_point
{
    float x; /**< Right of the pen. */
    float y; /**< Above the baseline. */
} tb_font_point;

/** @brief What a segment of a contour is. */
typedef enum tb_font_segment_type
{
    TB_FONT_LINE,     /**< A straight line to `end`; `control` is `end` too. */
    TB_FONT_QUADRATIC /**< A quadratic Bézier curve to `end`, pulled toward `control`. */
} tb_font_segment_type;

/** @brief One segment of a contour, from where the previous one ended. */
typedef struct tb_font_segment
{
    tb_font_segment_type type; /**< Line or curve. */
    tb_font_point control;     /**< The curve's control point. */
    tb_font_point end;         /**< Where the segment ends. */
} tb_font_segment;

/**
 * @brief One closed contour: from `start`, through each segment in turn,
 * the last of which ends at `start` again.
 *
 * TrueType winds outer contours clockwise and holes counter-clockwise, and
 * the outline is filled by the non-zero rule.
 */
typedef struct tb_font_contour
{
    tb_font_point start;             /**< Where the contour begins and ends. */
    const tb_font_segment *segments; /**< Its segments, in order; at least one. */
    size_t segment_count;            /**< Number of `segments`. */
} tb_font_contour;

/** @brief The outline of one glyph. */
typedef struct tb_font_outline
{
    const tb_font_contour *contours; /**< Its contours; NULL when there are none. */
    size_t contour_count; /**< Number of `contours`: 0 for a blank glyph such as a space. */
    int16_t x_min;        /**< Left edge of the glyph's box, as the font records it. */
    int16_t y_min;        /**< Bottom edge of the glyph's box. */
    int16_t x_max;        /**< Right edge of the glyph's box. */
    int16_t y_max;        /**< Top edge of the glyph's box. */
} tb_font_outline;

/**
 * @brief Read a glyph's outline: its contours, with the curves of TrueType's
 * on- and off-curve points made into explicit segments, and the glyphs of a
 * compound glyph placed and transformed into one outline.
 * @param font Required.
 * @param glyph Less than the font's glyph count.
 * @param arena Required; the outline's memory comes from it, and stays
 * valid until the arena is reset or destroyed. The memory used while
 * reading is released before returning, and a failure leaves the arena as
 * it was.
 * @param[out] outline Required; the outline on OK, zero otherwise.
 * @param[out] error Optional; why reading failed, or OK and "" on success.
 * @return OK; MALFORMED for a damaged glyph, or for compound glyphs nested
 * deeper than #TB_FONT_MAX_COMPOUND_DEPTH, as a cycle of them is; TOO_LARGE
 * for an outline of more than #TB_FONT_MAX_OUTLINE_POINTS points, or of
 * compound glyphs that place more than a million components in all.
 * @par Errors Running out of memory in the arena ends the process.
 */
TB_API tb_font_result tb_font_get_outline(const tb_font *font, uint16_t glyph, tb_arena *arena,
                                          tb_font_outline *outline, tb_font_error *error);

/** @brief A short name of a result, such as "malformed". Thread-safe. */
TB_API const char *tb_font_result_name(tb_font_result result);

#endif
