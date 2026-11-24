/** @file font_raster.h
 * @brief Glyphs as pixels: a font's measures scaled to a size in pixels,
 * glyph outlines rasterized into 8-bit coverage bitmaps, and an atlas that
 * caches rasterized glyphs packed into one single-channel image.
 *
 * This is the half of the `font` module that draws; badger/font.h is the
 * half that reads, and everything here takes the fonts it opens.
 *
 * **Sizes.** A size is a number of pixels to the em, `pixels_per_em`: a
 * font unit is `pixels_per_em / units_per_em` pixels. Sizes are clamped to
 * at most #TB_FONT_RASTER_MAX_PIXELS_PER_EM; a size that is not a positive
 * finite number draws nothing.
 *
 * **Bitmaps.** Pixels are bytes of coverage, from 0 (empty) to 255 (full),
 * in rows from the top. A glyph's bitmap is placed by its `left` and `top`:
 * with the pen at (x, y) on the baseline of an image whose y grows down, the
 * bitmap's top-left pixel goes at (x + left, y - top).
 *
 * **Two modes.** The smooth mode computes the exact area of each pixel that
 * the outline covers, for anti-aliased text at any size. The aliased mode
 * fills a pixel when its center is inside the outline, for pixel fonts at
 * the sizes they were drawn for, whose edges then fall on pixel edges and
 * come out crisp. Both fill by the non-zero rule.
 *
 * Signed distance fields, for text that scales in 3D, are not made yet.
 *
 * @par Threading
 * Rasterizing is thread-safe, as reading a font is, and as thread-safe as
 * the arena it is given: each thread gives its own. An atlas is externally
 * synchronized: the caller serializes every call that names the same atlas;
 * calls that only read it may run together.
 */
#ifndef TB_FONT_RASTER_H
#define TB_FONT_RASTER_H

#include "badger/allocator.h"
#include "badger/api.h"
#include "badger/arena.h"
#include "badger/font.h"

#include <stddef.h>
#include <stdint.h>

/** @brief The largest size drawn, in pixels to the em; larger sizes are clamped to it. */
#define TB_FONT_RASTER_MAX_PIXELS_PER_EM 2048.0f

/** @brief The widest or tallest bitmap drawn, in pixels. */
#define TB_FONT_RASTER_MAX_SIDE 4096u

/** @brief The most line segments one outline is drawn with, its curves cut into lines. */
#define TB_FONT_RASTER_MAX_EDGES ((size_t)1 << 20)

/* -------------------------------------------------------------------------
 * Measures in pixels.
 * ------------------------------------------------------------------------- */

/**
 * @brief How many pixels a font unit is at a size.
 * @param font Required.
 * @param pixels_per_em The size; clamped as every size is.
 * @return `pixels_per_em / units_per_em`, or 0 for a size that draws nothing.
 */
TB_API float tb_font_scale(const tb_font *font, float pixels_per_em);

/** @brief How lines of a font are spaced, in pixels at a size. */
typedef struct tb_font_line_metrics
{
    float ascent;      /**< Height above the baseline that lines leave room for. */
    float descent;     /**< Depth below the baseline that lines leave room for; negative. */
    float line_gap;    /**< Space between one line's descent and the next one's ascent. */
    float line_height; /**< From one baseline to the next: `ascent - descent + line_gap`. */
} tb_font_line_metrics;

/**
 * @brief Get the spacing of lines of a font at a size, from its `hhea` table.
 * @param font Required.
 * @param pixels_per_em The size.
 * @param[out] metrics Required; zero for a size that draws nothing.
 */
TB_API void tb_font_get_line_metrics(const tb_font *font, float pixels_per_em,
                                     tb_font_line_metrics *metrics);

/* -------------------------------------------------------------------------
 * Rasterizing.
 * ------------------------------------------------------------------------- */

/** @brief How coverage is computed. */
typedef enum tb_font_raster_mode
{
    /** The exact area of each pixel inside the outline: anti-aliased. */
    TB_FONT_RASTER_SMOOTH,
    /** 255 where the pixel's center is inside the outline, else 0: for
     * pixel fonts at their design sizes. */
    TB_FONT_RASTER_ALIASED
} tb_font_raster_mode;

/** @brief How to draw a glyph. Zero draws nothing: give a size. */
typedef struct tb_font_raster_options
{
    float pixels_per_em; /**< The size. */
    /** How far right of a whole pixel the pen is, from 0 to 1: the glyph is
     * drawn shifted by it, so that text can be placed at fractions of a
     * pixel. Only its fraction counts; a value that is not finite is 0. */
    float subpixel_x;
    tb_font_raster_mode mode; /**< Smooth or aliased. */
} tb_font_raster_options;

/** @brief A glyph drawn, or measured, at a size. */
typedef struct tb_font_glyph_image
{
    /** `width * height` bytes of coverage, rows from the top, `width` bytes
     * to a row; from the arena the glyph was drawn with. NULL when the glyph
     * was only measured, or is blank. */
    uint8_t *pixels;
    uint32_t width;  /**< Pixels across; 0 for a blank glyph, such as a space. */
    uint32_t height; /**< Pixels down; 0 for a blank glyph. */
    int32_t left;    /**< From the pen right to the bitmap's left edge; 0 for a blank glyph. */
    int32_t top;     /**< From the baseline up to the bitmap's top edge; 0 for a blank glyph. */
    float advance;   /**< How far the pen moves right after the glyph, unrounded. */
    float left_side_bearing; /**< From the pen to the left of the glyph's box, unrounded. */
} tb_font_glyph_image;

/**
 * @brief Measure a glyph at a size, without drawing it: the size and place
 * of the bitmap tb_font_rasterize_glyph() would draw, and its advance.
 *
 * The bitmap is the smallest box of whole pixels around every point of the
 * outline, its curves' control points included, shifted by the subpixel
 * offset.
 *
 * @param font Required.
 * @param glyph Less than the font's glyph count.
 * @param options Required.
 * @param arena Required; used while the outline is read, and left as it was.
 * @param[out] image Required; `pixels` is NULL. Zero on failure.
 * @param[out] error Optional; why it failed, or OK and "" on success.
 * @return OK; a failure of tb_font_get_outline(); or TOO_LARGE for a bitmap
 * wider or taller than #TB_FONT_RASTER_MAX_SIDE.
 * @par Errors Running out of memory in the arena ends the process.
 */
TB_API tb_font_result tb_font_measure_glyph(const tb_font *font, uint16_t glyph,
                                            const tb_font_raster_options *options, tb_arena *arena,
                                            tb_font_glyph_image *image, tb_font_error *error);

/**
 * @brief Draw a glyph at a size into a new bitmap of coverage.
 * @param font Required.
 * @param glyph Less than the font's glyph count.
 * @param options Required.
 * @param arena Required. The bitmap, and the outline read on the way, come
 * from it and stay valid until it is reset or rewound; the memory used
 * while drawing is released before returning, and a failure leaves the
 * arena as it was.
 * @param[out] image Required; as tb_font_measure_glyph() measures it, with
 * its pixels. Zero on failure.
 * @param[out] error Optional; why it failed, or OK and "" on success.
 * @return As tb_font_measure_glyph(); also TOO_LARGE for an outline of more
 * than #TB_FONT_RASTER_MAX_EDGES lines once its curves are cut into lines.
 * @par Errors Running out of memory in the arena ends the process.
 */
TB_API tb_font_result tb_font_rasterize_glyph(const tb_font *font, uint16_t glyph,
                                              const tb_font_raster_options *options,
                                              tb_arena *arena, tb_font_glyph_image *image,
                                              tb_font_error *error);

/** @brief Where an outline lands in a bitmap. */
typedef struct tb_font_raster_placement
{
    float scale; /**< Pixels to a font unit. */
    float x;     /**< Column, in pixels from the bitmap's left edge, of the outline's x = 0. */
    float y;     /**< Row, in pixels from the bitmap's top edge, of the outline's y = 0. */
} tb_font_raster_placement;

/** @brief A bitmap of coverage to draw into, which the caller owns. */
typedef struct tb_font_raster_target
{
    uint8_t *pixels; /**< `stride * height` bytes; required when width and height are not 0. */
    uint32_t width;  /**< Pixels across; at most #TB_FONT_RASTER_MAX_SIDE. */
    uint32_t height; /**< Pixels down; at most #TB_FONT_RASTER_MAX_SIDE. */
    size_t stride;   /**< Bytes from one row to the next; at least `width`. */
} tb_font_raster_target;

/**
 * @brief Draw any outline into a bitmap the caller gives, replacing its
 * pixels: the rasterizer under tb_font_rasterize_glyph(), for outlines made
 * by hand.
 *
 * A point at (x, y) in font units lands at column `placement.x + x * scale`
 * and row `placement.y - y * scale`. What falls outside the bitmap is
 * clipped. Contours need not be closed; one that is not is closed by a line
 * back to its start. Points that are not finite are skipped.
 *
 * @param outline Required; its box is not used.
 * @param placement Required.
 * @param mode Smooth or aliased.
 * @param target Required.
 * @param scratch Required; memory used while drawing, released before returning.
 * @return OK, or TOO_LARGE for an outline of more than
 * #TB_FONT_RASTER_MAX_EDGES lines, which leaves the pixels as they were.
 * @par Errors Running out of memory in the arena ends the process.
 */
TB_API tb_font_result tb_font_rasterize_outline(const tb_font_outline *outline,
                                                const tb_font_raster_placement *placement,
                                                tb_font_raster_mode mode,
                                                const tb_font_raster_target *target,
                                                tb_arena *scratch);

/* -------------------------------------------------------------------------
 * The atlas.
 * ------------------------------------------------------------------------- */

/** @brief Subpixel positions an atlas keeps at most for each glyph and size. */
#define TB_FONT_ATLAS_MAX_SUBPIXEL_BINS 8u

/** @brief Rectangles an atlas lists as changed before it merges them into one. */
#define TB_FONT_ATLAS_MAX_DIRTY_RECTS 64u

/** @brief The default width and height of an atlas, in pixels. */
#define TB_FONT_ATLAS_DEFAULT_SIDE 1024u

/** @brief How to make an atlas. Zero is a valid choice of every field. */
typedef struct tb_font_atlas_options
{
    /** Pixels across, from 16 to #TB_FONT_RASTER_MAX_SIDE; 0 for #TB_FONT_ATLAS_DEFAULT_SIDE. */
    uint32_t width;
    /** Pixels down, from 16 to #TB_FONT_RASTER_MAX_SIDE; 0 for #TB_FONT_ATLAS_DEFAULT_SIDE. */
    uint32_t height;
    /** Empty pixels kept around every glyph, so that sampling a glyph
     * bilinearly never reads its neighbour: at most 8; 0 for 1. */
    uint32_t padding;
    /** Positions of the pen between whole pixels that a glyph is drawn at,
     * from 1 to #TB_FONT_ATLAS_MAX_SUBPIXEL_BINS; 0 for 1, which draws
     * every glyph at a whole pixel. 4 is the usual choice for smooth text. */
    uint32_t subpixel_bins;
} tb_font_atlas_options;

/** @brief A rectangle of an atlas's image, in pixels. */
typedef struct tb_font_atlas_rect
{
    uint32_t x;      /**< Left column. */
    uint32_t y;      /**< Top row. */
    uint32_t width;  /**< Columns. */
    uint32_t height; /**< Rows. */
} tb_font_atlas_rect;

/** @brief An atlas's image, to copy to a texture. */
typedef struct tb_font_atlas_image
{
    const uint8_t *pixels; /**< `stride * height` bytes of coverage; the atlas's. */
    uint32_t width;        /**< Pixels across. */
    uint32_t height;       /**< Pixels down. */
    size_t stride;         /**< Bytes from one row to the next. */
} tb_font_atlas_image;

/** @brief Where a glyph is in an atlas, and how to place it. */
typedef struct tb_font_atlas_glyph
{
    /** The glyph's pixels in the atlas's image; zero in size for a blank
     * glyph, which has nothing to draw. Padding lies around it. */
    tb_font_atlas_rect rect;
    /** From the whole pixel of the pen right to the bitmap's left edge: one
     * more than the bitmap's own when the pen's fraction rounds up to the
     * next whole pixel. */
    int32_t left;
    int32_t top;   /**< From the baseline up to the bitmap's top edge. */
    float advance; /**< How far the pen moves right after the glyph, unrounded. */
} tb_font_atlas_glyph;

/**
 * @brief A cache of rasterized glyphs, packed into one image. Created by
 * tb_font_atlas_create(); owned by the caller; released by
 * tb_font_atlas_destroy().
 *
 * A glyph is drawn into the atlas the first time it is asked for, and found
 * there after. Glyphs are kept by font, glyph, size, mode, and subpixel bin.
 * When a glyph does not fit, the atlas is cleared, every glyph in it
 * forgotten, and its generation counts up; glyphs are then drawn again as
 * they are asked for. A caller that has placed glyphs since the generation
 * changed must ask for them again, because the rectangles it holds may now
 * hold other glyphs.
 */
typedef struct tb_font_atlas tb_font_atlas;

/**
 * @brief Create an empty atlas, its image cleared to 0.
 * @param options Optional; NULL for every default.
 * @param allocator Where the atlas lives, or NULL for the heap. It is copied.
 * @return The atlas; never NULL. Its whole image is listed as changed, and
 * its generation is 0.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_font_atlas *tb_font_atlas_create(const tb_font_atlas_options *options,
                                           const tb_allocator *allocator);

/** @brief Release an atlas. @param atlas Optional; NULL does nothing. */
TB_API void tb_font_atlas_destroy(tb_font_atlas *atlas);

/**
 * @brief Find a glyph in the atlas, or draw it there.
 *
 * The atlas keeps a font's glyphs by the font's address: call
 * tb_font_atlas_reset() before closing a font whose glyphs it holds.
 *
 * @param atlas Required.
 * @param font Required; open until the atlas is reset or destroyed.
 * @param glyph Less than the font's glyph count.
 * @param options Required. Its `subpixel_x` is rounded to the nearest of the
 * atlas's subpixel bins.
 * @param[out] placed Required; where the glyph is. Zero on failure.
 * @param[out] error Optional; why it failed, or OK and "" on success.
 * @return OK; a failure of tb_font_rasterize_glyph(), which the atlas
 * remembers nothing of; or TOO_LARGE for a glyph that, padded, is larger
 * than the whole atlas, which leaves the atlas as it was.
 * @par Errors Running out of memory ends the process.
 */
TB_API tb_font_result tb_font_atlas_get_glyph(tb_font_atlas *atlas, const tb_font *font,
                                              uint16_t glyph, const tb_font_raster_options *options,
                                              tb_font_atlas_glyph *placed, tb_font_error *error);

/**
 * @brief Forget every glyph, clear the image to 0, and count the generation up.
 * @param atlas Required. Afterwards its whole image is listed as changed.
 */
TB_API void tb_font_atlas_reset(tb_font_atlas *atlas);

/**
 * @brief How many times the atlas has been cleared, by tb_font_atlas_reset()
 * or because a glyph did not fit.
 * @param atlas Required.
 * @return The generation, from 0.
 */
TB_API uint64_t tb_font_atlas_generation(const tb_font_atlas *atlas);

/**
 * @brief How many glyphs the atlas holds, blank ones included.
 * @param atlas Required.
 */
TB_API size_t tb_font_atlas_glyph_count(const tb_font_atlas *atlas);

/**
 * @brief Get the atlas's image.
 * @param atlas Required.
 * @param[out] image Required; its pixels are the atlas's, valid until it is
 * destroyed, and change as glyphs are drawn.
 */
TB_API void tb_font_atlas_get_image(const tb_font_atlas *atlas, tb_font_atlas_image *image);

/**
 * @brief List the rectangles of the image that changed since the list was
 * last cleared: what to copy to a texture.
 *
 * The rectangles do not overlap one another unless the list has been
 * merged: past #TB_FONT_ATLAS_MAX_DIRTY_RECTS rectangles, the atlas replaces
 * them with the one rectangle around them all.
 *
 * @param atlas Required.
 * @param[out] count Required; how many rectangles there are.
 * @return The rectangles, the atlas's; valid until the next call that
 * changes the atlas. NULL when the count is 0.
 */
TB_API const tb_font_atlas_rect *tb_font_atlas_get_dirty(const tb_font_atlas *atlas, size_t *count);

/**
 * @brief Empty the list of changed rectangles, once they are copied.
 * @param atlas Required.
 */
TB_API void tb_font_atlas_clear_dirty(tb_font_atlas *atlas);

#endif
