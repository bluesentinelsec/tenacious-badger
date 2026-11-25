/** @file viewport.h
 * @brief The display policy: how an app's design resolution is placed on a
 * frame of any size and shape, after Godot's stretch settings, and the
 * viewport it gives.
 *
 * @code
 * tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
 * tb_viewport_policy_from_settings(engine, &policy);
 * const tb_viewport viewport = tb_viewport_compute(&policy, &(tb_viewport_frame){
 *     .width = 2560, .height = 1600, .display_scale = 1.5f,
 *     .aspect_ratio = tb_viewport_aspect_ratio_from_settings(engine)});
 * // viewport: 2560 by 1440 pixels from (0, 80), 1.333 pixels a unit, 1920 by 1080 units
 * const tb_vec2 mouse = tb_viewport_from_pixels(&viewport, (tb_vec2){1280, 800});
 * // mouse: (960, 540), the middle of the design
 * @endcode
 *
 * **The policy** is one per app, set by the `display.design_*` and
 * `display.stretch_*` settings, which an app gives in its default file:
 *
 * - The **design size**, in units: the resolution the app is made for. With
 *   none (0 by 0), a unit is a logical pixel at the display's scale, and the
 *   viewport is the whole frame, as a tool wants.
 * - The **aspect** (#tb_viewport_aspect): what happens when the frame's
 *   shape is not the design's. `keep` letterboxes or pillarboxes;
 *   `keep_width` and `keep_height` keep one axis and show more of the other;
 *   `expand` shows more of either; `ignore` stretches.
 * - The **scale mode**: fractional, or whole pixels a unit for pixel art.
 * - The **render mode**: the canvas draws at the frame's own pixels, so
 *   text stays crisp; the viewport mode renders into a target of a pixel a
 *   unit, then upscales it into the viewport, as a pixel-art game wants.
 * - A **scale**, a factor on top: 2 draws every unit twice as large, and
 *   shows half as many.
 * - The **bar color**, of what the viewport does not cover.
 *
 * **The viewport** (#tb_viewport) is what the policy gives a frame: a
 * rectangle of whole pixels, the pixels a unit each way, and the units it
 * shows. It is the single source of truth for everything placed on the
 * frame: the 3D camera's aspect and render target, the canvas's design view
 * (tb_canvas_view_viewport()), the GUI (tb_gui_set_viewport()), and the
 * mouse. The renderer computes it each frame (tb_renderer_viewport()).
 *
 * **The player's aspect ratio**, the `display.aspect_ratio` setting,
 * narrows the frame first: when it is not `auto`, the policy works within
 * the largest centered rectangle of that shape.
 *
 * See docs/display.md.
 *
 * @par Threading
 * Thread-safe: every function is pure, but those that read settings, which
 * are thread-safe as settings are.
 */
#ifndef TB_VIEWPORT_H
#define TB_VIEWPORT_H

#include "badger/api.h"
#include "badger/math.h"

#include <stdbool.h>
#include <stdint.h>

struct tb_engine_settings;

/** @brief What the viewport does when the frame's shape is not the design's,
 * as Godot's `stretch/aspect` does. */
typedef enum tb_viewport_aspect
{
    /** The design fits whole, as large as it fits, centered: the rest of the
     * frame is bars, above and below or at the sides. */
    TB_VIEWPORT_ASPECT_KEEP,
    /** The design's width is kept: a taller frame shows more height, and a
     * wider one is pillarboxed. */
    TB_VIEWPORT_ASPECT_KEEP_WIDTH,
    /** The design's height is kept: a wider frame shows more width, and a
     * taller one is letterboxed. */
    TB_VIEWPORT_ASPECT_KEEP_HEIGHT,
    /** The design fits by the smaller axis, and the frame's extra is shown:
     * the viewport is the whole frame. */
    TB_VIEWPORT_ASPECT_EXPAND,
    /** The design is stretched to the whole frame, each axis its own scale. */
    TB_VIEWPORT_ASPECT_IGNORE,
    TB_VIEWPORT_ASPECT_COUNT /**< The number of aspects. */
} tb_viewport_aspect;

/** @brief How the pixels a unit are chosen. */
typedef enum tb_viewport_scale_mode
{
    TB_VIEWPORT_SCALE_FRACTIONAL, /**< As large as the frame allows. */
    /** A whole number of pixels a unit, at least 1, on whole pixels: pixel art
     * stays sharp and even. A frame too small for the design at 1 shows
     * the part of it from its top left that fits. */
    TB_VIEWPORT_SCALE_INTEGER,
    TB_VIEWPORT_SCALE_COUNT /**< The number of scale modes. */
} tb_viewport_scale_mode;

/** @brief How what is drawn in units reaches the frame. */
typedef enum tb_viewport_render_mode
{
    /** At the frame's own pixels, through the viewport's transform: text
     * and edges stay crisp at any scale. */
    TB_VIEWPORT_RENDER_CANVAS,
    /** Into a target of a pixel a unit, the units the viewport shows, then
     * upscaled into the viewport: nearest-neighbour at whole pixels a unit,
     * linear otherwise. Text is rasterized at the target's pixels, for a
     * retro look. */
    TB_VIEWPORT_RENDER_VIEWPORT,
    TB_VIEWPORT_RENDER_COUNT /**< The number of render modes. */
} tb_viewport_render_mode;

/** @brief An app's display policy. Start from #TB_VIEWPORT_POLICY_INIT, or
 * from the settings (tb_viewport_policy_from_settings()). */
typedef struct tb_viewport_policy
{
    /** The design size, in units. 0 by 0 is none: a unit is a logical pixel
     * at the display's scale. A size of one axis alone is none too. */
    uint32_t design_width;
    uint32_t design_height;              /**< See `design_width`. */
    tb_viewport_aspect aspect;           /**< With a design size. */
    tb_viewport_scale_mode scale_mode;   /**< Fractional or whole pixels. */
    tb_viewport_render_mode render_mode; /**< Canvas or viewport. */
    /** A factor on top: 2 draws every unit twice as large. 0 is 1. */
    float scale;
    /** The color of the bars, as the frame shows it (sRGB), and opaque. */
    tb_vec4 bar_color;
} tb_viewport_policy;

/** @brief The engine's default policy, the settings' built-in values: a
 * design of 1920 by 1080, kept whole, at fractional scales, on the canvas,
 * with black bars. */
#define TB_VIEWPORT_POLICY_INIT                                                                    \
    {.design_width = 1920u,                                                                        \
     .design_height = 1080u,                                                                       \
     .aspect = TB_VIEWPORT_ASPECT_KEEP,                                                            \
     .scale_mode = TB_VIEWPORT_SCALE_FRACTIONAL,                                                   \
     .render_mode = TB_VIEWPORT_RENDER_CANVAS,                                                     \
     .scale = 1.0f,                                                                                \
     .bar_color = {0.0f, 0.0f, 0.0f, 1.0f}}

/** @brief A policy without a design size: units are logical pixels, and the
 * viewport is the whole frame, as a tool's. */
#define TB_VIEWPORT_POLICY_LOGICAL                                                                 \
    {.aspect = TB_VIEWPORT_ASPECT_KEEP,                                                            \
     .scale_mode = TB_VIEWPORT_SCALE_FRACTIONAL,                                                   \
     .render_mode = TB_VIEWPORT_RENDER_CANVAS,                                                     \
     .scale = 1.0f,                                                                                \
     .bar_color = {0.0f, 0.0f, 0.0f, 1.0f}}

/** @brief The frame a viewport is computed for. Zero-initialize, then set
 * what differs. */
typedef struct tb_viewport_frame
{
    uint32_t width;  /**< Pixels across. */
    uint32_t height; /**< Pixels down. */
    /** The display's scale, 1.5 at 150%, which a logical pixel is without a
     * design size; 0 is 1. */
    float display_scale;
    /** The player's aspect ratio, width over height, which narrows the frame
     * first to the largest centered rectangle of that shape; 0 for none
     * (`auto`). tb_viewport_aspect_ratio_from_settings() gives it. */
    float aspect_ratio;
} tb_viewport_frame;

/**
 * @brief Where a policy places its units on a frame: a rectangle of whole
 * pixels, and a scale from units to pixels.
 *
 * Unit (0, 0) is drawn at the rectangle's top left, and a unit is `scale`
 * pixels: a pixel is `(x, y) + scale * unit`. The scale is the same both
 * ways but in `ignore`. `size` is the units the rectangle shows: the
 * design size in `keep`, more along an axis that grows, and as many as the
 * frame holds without a design size.
 *
 * In the viewport render mode, what is drawn in units is drawn first into
 * a **target** of `target_width` by `target_height` pixels, a pixel a unit
 * each way from its top left, then upscaled into the rectangle: the
 * target's units `(0, 0)` to `size` cover the rectangle exactly. The
 * mouse is still placed by the rectangle and `scale`.
 */
typedef struct tb_viewport
{
    uint32_t x;      /**< The rectangle's left, in the frame's pixels. */
    uint32_t y;      /**< Its top. */
    uint32_t width;  /**< Its pixels across; at least 1 for a frame of some size. */
    uint32_t height; /**< Its pixels down. */
    tb_vec2 scale;   /**< Pixels a unit, across and down; more than 0. */
    tb_vec2 size;    /**< The units shown, across and down. */
    /** In the viewport render mode, the target's pixels across: the units
     * shown, rounded up, from 1 to #TB_VIEWPORT_MAX_TARGET. 0 in the canvas
     * mode, and for a frame of no size. */
    uint32_t target_width;
    uint32_t target_height; /**< The target's pixels down; see `target_width`. */
} tb_viewport;

/** @brief The most pixels a side of the viewport render mode's target: the
 * largest texture every backend makes. A side of more units than this, of
 * an absurd design or scale, is not drawn whole. */
#define TB_VIEWPORT_MAX_TARGET 16384u

/**
 * @brief The viewport a policy gives a frame.
 *
 * Pure: the same policy and frame give the same viewport on every thread
 * and every platform. A frame of no size gives a viewport of no size, at 1
 * pixel a unit.
 *
 * @param policy Required.
 * @param frame Required.
 * @return The viewport.
 * @par Threading Thread-safe.
 */
TB_API tb_viewport tb_viewport_compute(const tb_viewport_policy *policy,
                                       const tb_viewport_frame *frame);

/**
 * @brief The pixel of the frame where a point in units is drawn.
 * @param viewport Required.
 * @par Threading Thread-safe.
 */
TB_API tb_vec2 tb_viewport_to_pixels(const tb_viewport *viewport, tb_vec2 units);

/**
 * @brief The point in units drawn at a pixel of the frame: where the mouse
 * points. A pixel outside the viewport gives a point outside its units.
 * @param viewport Required.
 * @par Threading Thread-safe.
 */
TB_API tb_vec2 tb_viewport_from_pixels(const tb_viewport *viewport, tb_vec2 pixel);

/**
 * @brief Whether a pixel of the frame is inside the viewport's rectangle,
 * not on its bars: what a click must be to reach the app.
 * @param viewport Required.
 * @param pixel In the frame's pixels, such as a mouse event's `pixel_x` and
 * `pixel_y`.
 * @par Threading Thread-safe.
 */
TB_API bool tb_viewport_contains(const tb_viewport *viewport, tb_vec2 pixel);

/**
 * @brief The viewport's aspect, width over height of what it shows: what the
 * 3D camera projects with. The rectangle's own in every aspect but `ignore`,
 * where it is the design's, stretched to the rectangle.
 * @param viewport Required.
 * @return More than 0; 1 for a viewport of no size.
 * @par Threading Thread-safe.
 */
TB_API float tb_viewport_aspect_of(const tb_viewport *viewport);

/**
 * @brief The pixels a unit of what must not stretch, such as text: the
 * smaller of the two scales, which are the same but in `ignore`.
 * @param viewport Required.
 * @par Threading Thread-safe.
 */
TB_API float tb_viewport_uniform_scale(const tb_viewport *viewport);

/**
 * @brief The policy the engine's settings describe: `display.design_width`,
 * `display.design_height`, `display.stretch_aspect`,
 * `display.stretch_scale_mode`, `display.stretch_render_mode`,
 * `display.stretch_scale` and `display.bar_color`, as
 * tb_renderer_quality_from_settings() reads the quality.
 * @param engine Required; the engine's settings.
 * @param[out] policy Required; receives the policy. A bar color that is not
 * `#rrggbb` is black.
 * @par Threading Thread-safe, as reading settings is.
 */
TB_API void tb_viewport_policy_from_settings(const struct tb_engine_settings *engine,
                                             tb_viewport_policy *policy);

/**
 * @brief The player's aspect ratio, from `display.aspect_ratio`, as
 * #tb_viewport_frame takes it: 0 for `auto`; else 16/9, 16/10, 64/27 for
 * `21_9` (2560 by 1080), 32/9, or 4/3.
 * @param engine Required; the engine's settings.
 * @par Threading Thread-safe, as reading settings is.
 */
TB_API float tb_viewport_aspect_ratio_from_settings(const struct tb_engine_settings *engine);

/**
 * @brief A color written `#rrggbb`, as `display.bar_color` holds it.
 * @param text Required; NUL-terminated.
 * @param[out] color Required; receives the color, opaque, from 0 to 1, on
 * success; unchanged otherwise.
 * @return Whether the text is such a color.
 * @par Threading Thread-safe.
 */
TB_API bool tb_viewport_parse_color(const char *text, tb_vec4 *color);

/** @brief An aspect's name as `display.stretch_aspect` spells it: "keep",
 * "keep_width", "keep_height", "expand" or "ignore"; "unknown" for another
 * value. @par Threading Thread-safe. */
TB_API const char *tb_viewport_aspect_name(tb_viewport_aspect aspect);

/** @brief A scale mode's name: "fractional" or "integer"; "unknown" for
 * another value. @par Threading Thread-safe. */
TB_API const char *tb_viewport_scale_mode_name(tb_viewport_scale_mode mode);

/** @brief A render mode's name: "canvas" or "viewport"; "unknown" for
 * another value. @par Threading Thread-safe. */
TB_API const char *tb_viewport_render_mode_name(tb_viewport_render_mode mode);

#endif
