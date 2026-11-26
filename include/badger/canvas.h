/** @file canvas.h
 * @brief 2D drawing over a frame: quads, sprites, images and anti-aliased
 * shapes, sorted by layer, as Unreal's Canvas and Godot's CanvasItem are.
 *
 * @code
 * const tb_image_desc desc = {.width = 64, .height = 64, .filter = TB_IMAGE_NEAREST};
 * uint8_t pixels[64 * 64 * 4] = {0};
 * tb_image_fill_checkerboard(pixels, sizeof(pixels), 64, 64, 8, (tb_vec4){1, 1, 1, 1},
 *                            (tb_vec4){0.2f, 0.2f, 0.2f, 1});
 * tb_image *const checker = tb_image_create(renderer, &desc, pixels, sizeof(pixels));
 *
 * // each frame, between tb_renderer_begin_frame() and tb_renderer_end_frame()
 * const tb_canvas_view ui = tb_canvas_view_logical(renderer);
 * tb_canvas_set_view(renderer, &ui);
 * tb_canvas_fill_rounded_rect(renderer, (tb_canvas_rect){16, 16, 200, 48}, 8,
 *                             (tb_vec4){0.1f, 0.1f, 0.1f, 0.8f});
 * tb_canvas_set_layer(renderer, 1);
 * tb_canvas_draw_image(renderer, checker, (tb_vec2){24, 24}, (tb_vec4){1, 1, 1, 1});
 * @endcode
 *
 * **Where 2D is drawn.** A frame's 2D is drawn after its 3D scene and its
 * debug lines, over them, into the same image: what a game's interface and a
 * 2D game need. In the display policy's viewport render mode, what is drawn
 * in the design's view goes into the viewport's target with the 3D scene,
 * and is upscaled with it; what is drawn in a view of the frame goes over
 * the upscaled frame (#tb_canvas_target).
 *
 * **Coordinates.** The canvas's origin is the top left of the view, x to
 * the right and y down, as windows, interfaces and image editors have it.
 * Rotations are in radians, clockwise on the screen. A view
 * (#tb_canvas_view) turns the canvas's units into the frame's pixels:
 * pixels themselves, the window's logical units at its display scale (what
 * interfaces want), the app's design units over the viewport its display
 * policy gives (tb_canvas_view_viewport(), badger/viewport.h), or a game's
 * own resolution scaled to fit, by whole pixels when pixel art wants them.
 *
 * **Layers.** Each quad has the layer that was current when it was drawn
 * (tb_canvas_set_layer()); a higher layer is drawn over a lower one, and
 * within a layer, what was drawn later is drawn over what came before.
 *
 * **Colors** are RGBA from 0 to 1 with straight alpha, written to the frame
 * as they are given. The canvas blends with premultiplied alpha, so an
 * image's pixels are premultiplied when it is made, and its edges blend
 * cleanly when it is filtered.
 *
 * **Shapes** are anti-aliased over a pixel: each is a quad whose pixel
 * shader measures its distance to the shape's edge. A shape whose edges lie
 * on whole pixels has crisp edges.
 *
 * **Text** is laid out by the font module (badger/font_layout.h) at the
 * view's scale, so text in logical units is drawn at the display's own
 * pixels and stays crisp at any display scale: 16 units at 150% is laid out
 * and rasterized at 24 pixels, never drawn at 16 and scaled up. A pixel
 * font is drawn at the whole multiple of its design size nearest its size
 * in pixels (see tb_canvas_draw_text()). Its glyphs are rasterized
 * once into the renderer's glyph atlas, a single-channel texture, and each
 * is drawn as a quad of the atlas's coverage times the text's color, on a
 * whole pixel; the atlas's subpixel bins keep the pen's fractions of a
 * pixel. Glyphs changed in the atlas are copied to its texture before the
 * frame's 2D is drawn, and when the atlas fills and clears partway through
 * a frame, the frame's glyphs are found again. A font drawn with stays open
 * until the frame ends; tb_canvas_forget_font() comes before it closes.
 *
 * @par Threading
 * Stage-owned: every function is called on the thread that renders, as the
 * renderer's are, except those marked thread-safe.
 */
#ifndef TB_CANVAS_H
#define TB_CANVAS_H

#include "badger/api.h"
#include "badger/font_layout.h"
#include "badger/math.h"
#include "badger/renderer.h"
#include "badger/rhi.h"
#include "badger/viewport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief An image the canvas draws. Made by tb_image_create(); owned by the
 * caller; destroyed by tb_image_destroy() before its renderer. */
typedef struct tb_image tb_image;

/** @brief A rectangle of the canvas: its top left and its size. */
typedef struct tb_canvas_rect
{
    float x;      /**< Left. */
    float y;      /**< Top. */
    float width;  /**< Across, to the right. */
    float height; /**< Down. */
} tb_canvas_rect;

/**
 * @brief Where a view's quads are drawn: over the whole frame, or into the
 * viewport render mode's target.
 *
 * Views of the frame, which tb_canvas_view_pixels(), tb_canvas_view_logical()
 * and tb_canvas_view_fit() make, draw over the whole frame at its own
 * pixels, after the viewport's upscale: debug overlays, consoles and
 * statistics stay crisp in every render mode. tb_canvas_view_viewport()
 * makes a view of the viewport's target in the viewport render mode; in
 * the canvas mode, there is no target, and a view of it draws as a view of
 * the frame.
 */
typedef enum tb_canvas_target
{
    TB_CANVAS_TARGET_FRAME,    /**< The frame's pixels, over everything. */
    TB_CANVAS_TARGET_VIEWPORT, /**< The viewport's target's pixels, upscaled. */
    TB_CANVAS_TARGET_COUNT     /**< The number of targets. */
} tb_canvas_target;

/**
 * @brief How the canvas's units become the pixels of its target, the
 * frame's or the viewport's (#tb_canvas_target): a pixel is
 * `origin + scale * unit`. Draws outside `clip` are not seen.
 *
 * A view stretched one way more than the other, as a display policy's
 * `ignore` stretches its units, gives its pixels a unit each way in
 * `stretch`; text and the radii and widths of shapes keep `scale`, so text
 * is never squashed.
 */
typedef struct tb_canvas_view
{
    tb_vec2 origin;   /**< The pixel where the canvas's (0, 0) is drawn. */
    float scale;      /**< Pixels per unit; more than 0. */
    tb_rhi_rect clip; /**< Pixels drawn into; a zero size is the whole target. */
    /** Pixels per unit across and down, where they differ; zero is `scale`
     * both ways. */
    tb_vec2 stretch;
    /** Where it draws; zero is the frame. */
    tb_canvas_target target;
} tb_canvas_view;

/** @brief How an image is read between its pixels. */
typedef enum tb_image_filter
{
    TB_IMAGE_LINEAR, /**< Blend the nearest four: smooth when scaled or turned. */
    TB_IMAGE_NEAREST /**< The nearest: pixel art, sharp at whole scales. */
} tb_image_filter;

/** @brief An image to make. Zero-initialize, then set what differs. */
typedef struct tb_image_desc
{
    uint32_t width;         /**< Pixels; more than 0, at most the device's largest texture. */
    uint32_t height;        /**< Pixels; likewise. */
    tb_image_filter filter; /**< How it is read when drawn at another size. */
    /** The pixels given are premultiplied by their alpha already; otherwise
     * they are straight, and premultiplied when they are uploaded. */
    bool premultiplied;
    const char *name; /**< For debuggers; may be NULL; copied. */
} tb_image_desc;

/** @brief A sprite: part of an image, or a solid color, placed, scaled,
 * turned and tinted. Zero-initialize, then set what differs. */
typedef struct tb_canvas_sprite
{
    const tb_image *image; /**< The image, or NULL for a solid color. */
    /** The image's texels to draw, from its top left; a zero size is the
     * whole image. A sprite sheet's frame. */
    tb_rhi_rect source;
    tb_vec2 position; /**< Where the pivot is drawn, in the canvas's units. */
    /** Units across and down; zero is the source's size in texels (one unit
     * a texel). A negative size mirrors the sprite. */
    tb_vec2 size;
    /** The point it is placed by and turns around, as a fraction of its size:
     * (0, 0) its top left, the default; (0.5, 0.5) its middle. */
    tb_vec2 pivot;
    float rotation; /**< Radians, clockwise on the screen, around the pivot. */
    tb_vec4 tint;   /**< Multiplies the image's colors; zero alpha draws nothing. */
} tb_canvas_sprite;

/**
 * @brief A nine-slice panel: an image cut by four insets into corners
 * drawn at their size, edges stretched along their length, and a middle
 * stretched both ways, so that a frame or a button of any size keeps its
 * borders, as Godot's `StyleBoxTexture` and Unity's sliced sprites do.
 * Zero-initialize, then set what differs.
 *
 * The borders are drawn `inset * scale` units wide, so that they keep their
 * size in units at any view's scale. With `whole_pixels`, each texel of a
 * border is drawn as a whole number of pixels, at least one, the nearest
 * below `scale` units: pixel art stays sharp and even. Borders wider than
 * the rectangle are shrunk together to fit it. Every edge of a slice is
 * placed on a whole pixel, so slices meet with neither gap nor overlap.
 *
 * An image filtered linearly blends a slice's edge texels with its
 * neighbour's; give pixel art #TB_IMAGE_NEAREST, or keep slices that differ
 * a texel apart.
 */
typedef struct tb_canvas_nine_slice
{
    const tb_image *image; /**< The image; NULL for none, which draws nothing. */
    /** The image's texels the panel is cut from; a zero size is the whole
     * image. Several panels share one image, as a sprite sheet's frames do. */
    tb_rhi_rect source;
    uint32_t left;     /**< Texels of the source in the left corners and edge. */
    uint32_t top;      /**< Texels in the top corners and edge. */
    uint32_t right;    /**< Texels in the right corners and edge. */
    uint32_t bottom;   /**< Texels in the bottom corners and edge. */
    float scale;       /**< Units a texel of a border is drawn; 0 is 1. */
    bool whole_pixels; /**< Each border texel a whole number of pixels: for pixel art. */
    bool hollow;       /**< The middle is not drawn: a frame around something. */
} tb_canvas_nine_slice;

/** @brief One slice of a nine-slice panel: the texels drawn, and where. */
typedef struct tb_canvas_slice
{
    tb_rhi_rect source;  /**< Texels of the image. */
    tb_canvas_rect rect; /**< Where they are drawn, in units. */
} tb_canvas_slice;

/** @brief The most slices a nine-slice panel is drawn in. */
#define TB_CANVAS_NINE_SLICES 9u

/** @brief What the canvas drew in the last frame that ended. */
typedef struct tb_canvas_stats
{
    size_t quads;   /**< Quads drawn. */
    size_t dropped; /**< Quads not drawn: the frame's uploads were full, or the device
                       has no bindless resources, which the canvas needs. */
    /** Draw calls they took: one for all of them, or none; in the viewport
     * render mode, one more for those of the viewport's target. */
    uint32_t draws;
    size_t glyphs; /**< Of the quads, the glyphs of text. */
} tb_canvas_stats;

/* ---------------------------------------------------------------------------
 * Views.
 * ------------------------------------------------------------------------- */

/**
 * @brief A view in pixels: one unit is one pixel of the frame.
 * @par Threading Thread-safe.
 */
TB_API tb_canvas_view tb_canvas_view_pixels(void);

/**
 * @brief A view in the window's logical units: one unit is as many pixels
 * as its display's scale (1.5 at 150%), so an interface keeps its size on
 * every display.
 *
 * The scale is the window's content scale when this is called. When the
 * window moves to a display of another scale, the window learns the new
 * scale as its events are pumped (#TB_WINDOW_EVENT_SCALE_CHANGED), and the
 * renderer the new size of its pixels at the next tb_renderer_begin_frame();
 * a view made in each frame follows it in the frame after the move.
 */
TB_API tb_canvas_view tb_canvas_view_logical(const tb_renderer *renderer);

/**
 * @brief A view that fits a game's own resolution, `width` by `height`
 * units, in the middle of a frame, as large as it fits, with the rest of the
 * frame clipped away (letterboxed).
 * @param frame_width The frame's pixels across (tb_renderer_size()).
 * @param frame_height The frame's pixels down.
 * @param width Units across to show; more than 0.
 * @param height Units down to show; more than 0.
 * @param whole_pixels Scale by a whole number of pixels per unit, at least
 * 1, and place the view on whole pixels: pixel art stays sharp and even.
 * @par Threading Thread-safe.
 */
TB_API tb_canvas_view tb_canvas_view_fit(uint32_t frame_width, uint32_t frame_height, float width,
                                         float height, bool whole_pixels);

/**
 * @brief A view in an app's design units, over the viewport its display
 * policy gives the frame (tb_renderer_viewport()): unit (0, 0) at the
 * viewport's top left, its scale the viewport's, and what is drawn clipped
 * to it, off the bars. In `ignore`, stretched as the viewport is.
 *
 * In the viewport render mode (a viewport with a target), the view is of
 * #TB_CANVAS_TARGET_VIEWPORT: unit (0, 0) at the target's top left, a pixel
 * a unit, clipped to the target, so that what is drawn in it, text
 * included, is drawn at the target's pixels and upscaled with the 3D scene.
 * @param viewport Required.
 * @par Threading Thread-safe.
 */
TB_API tb_canvas_view tb_canvas_view_viewport(const tb_viewport *viewport);

/**
 * @brief The pixel of the view's target, the frame's or the viewport's,
 * where a point of the canvas is drawn.
 * @param view Required.
 * @par Threading Thread-safe.
 */
TB_API tb_vec2 tb_canvas_view_to_pixels(const tb_canvas_view *view, tb_vec2 point);

/**
 * @brief The point of the canvas drawn at a pixel of the view's target: of
 * the frame, where the mouse points, for a view of the frame. Place the
 * mouse in a view of the viewport's target by the viewport itself
 * (tb_viewport_from_pixels()).
 * @param view Required.
 * @par Threading Thread-safe.
 */
TB_API tb_vec2 tb_canvas_view_from_pixels(const tb_canvas_view *view, tb_vec2 pixel);

/* ---------------------------------------------------------------------------
 * Drawing. Each is called between tb_renderer_begin_frame() and
 * tb_renderer_end_frame(), and draws in the frame that ends.
 * ------------------------------------------------------------------------- */

/**
 * @brief Use a view for what is drawn after, in this frame. Each frame
 * begins with tb_canvas_view_pixels().
 * @param view Required; copied. Its scale more than 0.
 */
TB_API void tb_canvas_set_view(tb_renderer *renderer, const tb_canvas_view *view);

/**
 * @brief Draw what comes after on a layer, in this frame. Each frame begins
 * on layer 0. Higher layers are drawn over lower ones.
 */
TB_API void tb_canvas_set_layer(tb_renderer *renderer, int32_t layer);

/** @brief A filled rectangle. */
TB_API void tb_canvas_fill_rect(tb_renderer *renderer, tb_canvas_rect rect, tb_vec4 color);

/** @brief A rectangle's outline, `thickness` units wide inside its edges. */
TB_API void tb_canvas_stroke_rect(tb_renderer *renderer, tb_canvas_rect rect, float thickness,
                                  tb_vec4 color);

/** @brief A filled rectangle with corners rounded by `radius` units, at most
 * half its shorter side. */
TB_API void tb_canvas_fill_rounded_rect(tb_renderer *renderer, tb_canvas_rect rect, float radius,
                                        tb_vec4 color);

/** @brief A rounded rectangle's outline, `thickness` units wide inside its
 * edges. */
TB_API void tb_canvas_stroke_rounded_rect(tb_renderer *renderer, tb_canvas_rect rect, float radius,
                                          float thickness, tb_vec4 color);

/** @brief A filled circle. */
TB_API void tb_canvas_fill_circle(tb_renderer *renderer, tb_vec2 center, float radius,
                                  tb_vec4 color);

/** @brief A circle's outline, `thickness` units wide inside its edge. */
TB_API void tb_canvas_stroke_circle(tb_renderer *renderer, tb_vec2 center, float radius,
                                    float thickness, tb_vec4 color);

/** @brief A line from `a` to `b`, `thickness` units wide, its ends square
 * at `a` and `b`. A line of no length draws nothing. */
TB_API void tb_canvas_line(tb_renderer *renderer, tb_vec2 a, tb_vec2 b, float thickness,
                           tb_vec4 color);

/**
 * @brief An image at its size, one unit a texel, its top left at `position`.
 * @param image Required.
 * @param tint Multiplies its colors: (1, 1, 1, 1) draws it as it is.
 */
TB_API void tb_canvas_draw_image(tb_renderer *renderer, const tb_image *image, tb_vec2 position,
                                 tb_vec4 tint);

/** @brief A sprite.
 * @param sprite Required; copied. Its image, if any, alive until the frame ends. */
TB_API void tb_canvas_draw_sprite(tb_renderer *renderer, const tb_canvas_sprite *sprite);

/**
 * @brief Cut a nine-slice panel into the slices that draw it over a
 * rectangle at a view's scale: what tb_canvas_draw_nine_slice() draws, for
 * interfaces that build their own commands, and for tests. Slices of no
 * size, and the middle of a hollow panel, are left out.
 * @param slice Required. Its image is not read: its size is given.
 * @param image_width The image's pixels across (tb_image_size()).
 * @param image_height Its pixels down.
 * @param rect Where the panel is drawn, in units.
 * @param pixels_per_unit The view's scale, more than 0: the slices' edges
 * are placed on its whole pixels.
 * @param[out] slices Required; room for #TB_CANVAS_NINE_SLICES.
 * @return How many slices were written, row by row from the top left.
 * @par Threading Thread-safe.
 */
TB_API size_t tb_canvas_nine_slice_layout(const tb_canvas_nine_slice *slice, uint32_t image_width,
                                          uint32_t image_height, tb_canvas_rect rect,
                                          float pixels_per_unit, tb_canvas_slice *slices);

/**
 * @brief A nine-slice panel over a rectangle, at the current view's scale.
 * @param slice Required; copied. Its image, if any, alive until the frame
 * ends; none draws nothing.
 * @param tint Multiplies its colors.
 */
TB_API void tb_canvas_draw_nine_slice(tb_renderer *renderer, const tb_canvas_nine_slice *slice,
                                      tb_canvas_rect rect, tb_vec4 tint);

/**
 * @brief Text, laid out by tb_font_layout_text() and drawn with its box's
 * top left at `position`.
 *
 * The text is laid out at `size` times the view's scale in pixels, and
 * each glyph is drawn on a whole pixel, its baseline on a whole row.
 *
 * @param font Required; open until the frame ends.
 * @param text UTF-8; copied as it is laid out.
 * @param size The size in the canvas's units to the em: 16 in a logical
 * view is 16 pixels at 100% and 24 at 150%. A size that is not a positive
 * finite number draws nothing.
 * @param position The top left of the text's box of lines, in the canvas's
 * units: its first baseline is the font's ascent below it.
 * @param color Straight alpha; zero alpha draws nothing.
 * @param options Optional; NULL is every default. `wrap_width` is in the
 * canvas's units. `pixel_size` is a pixel font's design size in pixels
 * (#tb_font_info::pixel_size): the size in pixels, `size` times the view's
 * scale, is then rounded to the nearest whole multiple of it, halves up,
 * and at least once, and every glyph is drawn aliased on a whole pixel. The
 * pixel role at 16 units is 16 pixels at 100% and 125% (20 rounds down),
 * and 32 at 150% (24 rounds up) and 200%.
 */
TB_API void tb_canvas_draw_text(tb_renderer *renderer, const tb_font *font, tb_text text,
                                float size, tb_vec2 position, tb_vec4 color,
                                const tb_font_layout_options *options);

/**
 * @brief Measure text as tb_canvas_draw_text() would draw it in the current
 * view, without drawing it.
 * @param font Required.
 * @param text UTF-8.
 * @param size As tb_canvas_draw_text() takes it.
 * @param options Optional; as tb_canvas_draw_text() takes them.
 * @return The widest line and the lines' height, in the canvas's units.
 */
TB_API tb_vec2 tb_canvas_measure_text(const tb_renderer *renderer, const tb_font *font,
                                      tb_text text, float size,
                                      const tb_font_layout_options *options);

/**
 * @brief Forget the glyphs of a font, before it closes: the glyph atlas
 * keeps glyphs by their font's address, which a font opened later may
 * reuse. Clears the whole atlas, whose glyphs are then drawn again as text
 * asks for them.
 * @param font The font to forget; NULL forgets every font.
 * @par Threading Outside a frame, on the thread that renders.
 */
TB_API void tb_canvas_forget_font(tb_renderer *renderer, const tb_font *font);

/** @brief What the canvas drew in the last frame that ended.
 * @param stats Required; filled. */
TB_API void tb_canvas_stats_of(const tb_renderer *renderer, tb_canvas_stats *stats);

/* ---------------------------------------------------------------------------
 * Images.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make an image from RGBA pixels, 8 bits a channel, rows from the
 * top.
 * @param desc Required.
 * @param pixels `width * height * 4` bytes, copied; NULL for a transparent
 * image to fill with tb_image_update().
 * @param size Bytes of `pixels`; 0 with NULL.
 * @return The image; never NULL. Running out of GPU memory is fatal, as it
 * is for a mesh.
 */
TB_API tb_image *tb_image_create(tb_renderer *renderer, const tb_image_desc *desc,
                                 const uint8_t *pixels, size_t size);

/**
 * @brief Replace a region of an image's pixels. The frames after, and the
 * frame being drawn, see the new pixels.
 * @param image Required.
 * @param region Pixels to replace, inside the image; a zero size is all of
 * them.
 * @param pixels Required; `region.width * region.height * 4` bytes of RGBA,
 * rows from the top, straight or premultiplied as the image was made;
 * copied.
 * @param size Bytes of `pixels`.
 */
TB_API void tb_image_update(tb_renderer *renderer, tb_image *image, tb_rhi_rect region,
                            const uint8_t *pixels, size_t size);

/** @brief Destroy an image, outside a frame or in one that has not drawn
 * it. NULL is ignored. */
TB_API void tb_image_destroy(tb_renderer *renderer, tb_image *image);

/** @brief An image's size in pixels.
 * @param image Required.
 * @param width Required; receives its width.
 * @param height Required; receives its height.
 * @par Threading Thread-safe. */
TB_API void tb_image_size(const tb_image *image, uint32_t *width, uint32_t *height);

/**
 * @brief Fill RGBA pixels with a checkerboard of `cell`-pixel squares, `a`
 * at the top left.
 * @param pixels Required; `size` bytes, at least `width * height * 4`.
 * @param cell Pixels a side of a square; more than 0.
 * @par Threading Thread-safe.
 */
TB_API void tb_image_fill_checkerboard(uint8_t *pixels, size_t size, uint32_t width,
                                       uint32_t height, uint32_t cell, tb_vec4 a, tb_vec4 b);

/** @brief The direction of a gradient. */
typedef enum tb_image_gradient
{
    TB_IMAGE_GRADIENT_HORIZONTAL, /**< From the left edge to the right. */
    TB_IMAGE_GRADIENT_VERTICAL,   /**< From the top edge to the bottom. */
    TB_IMAGE_GRADIENT_RADIAL /**< From the middle to the nearest edge, and on to the corners. */
} tb_image_gradient;

/**
 * @brief Fill RGBA pixels with a gradient from `from` to `to`, each pixel
 * the color at its middle.
 * @param pixels Required; `size` bytes, at least `width * height * 4`.
 * @par Threading Thread-safe.
 */
TB_API void tb_image_fill_gradient(uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
                                   tb_image_gradient direction, tb_vec4 from, tb_vec4 to);

/**
 * @brief Fill RGBA pixels with smooth gray value noise, opaque: a random
 * value every `cell` pixels, blended smoothly between. The same seed gives
 * the same pixels on every platform.
 * @param pixels Required; `size` bytes, at least `width * height * 4`.
 * @param cell Pixels between random values; more than 0.
 * @par Threading Thread-safe.
 */
TB_API void tb_image_fill_noise(uint8_t *pixels, size_t size, uint32_t width, uint32_t height,
                                uint32_t cell, uint64_t seed);

#endif
