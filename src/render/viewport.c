/* The display policy's arithmetic: the frame narrowed to the player's
 * aspect ratio, then the design size placed in it, axis by axis. Every
 * function here is pure. */
#include "badger/viewport.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

/* Less than this over a whole number of units is that number: what float
 * arithmetic leaves over 320 / 0.5 and the like. */
#define TB_VIEWPORT_TARGET_SLACK 0.001

/* How each axis of the design is placed in the frame. A kept axis shows the
 * design's extent, centered, with bars beside it where the frame is
 * larger; an open axis fills the frame, and shows as many units as fit. */
typedef enum tb_viewport_axis
{
    TB_VIEWPORT_AXIS_KEPT,
    TB_VIEWPORT_AXIS_OPEN
} tb_viewport_axis;

/* One axis of a frame, or of the viewport made in it. */
typedef struct tb_viewport_span
{
    uint32_t start;  /* in the frame's pixels */
    uint32_t length; /* pixels */
    float scale;     /* pixels a unit */
    float units;     /* shown */
} tb_viewport_span;

static const char *const tb_viewport_aspect_names[TB_VIEWPORT_ASPECT_COUNT] = {
    "keep", "keep_width", "keep_height", "expand", "ignore"};
static const char *const tb_viewport_scale_mode_names[TB_VIEWPORT_SCALE_COUNT] = {"fractional",
                                                                                  "integer"};
static const char *const tb_viewport_render_mode_names[TB_VIEWPORT_RENDER_COUNT] = {"canvas",
                                                                                    "viewport"};

/* A positive, finite factor, or 1. */
static float tb_viewport_positive(float value)
{
    return value > 0.0f && isfinite(value) ? value : 1.0f;
}

/* Whole pixels from a length that is at least 0 and no more than a frame's. */
static uint32_t tb_viewport_whole(double pixels)
{
    return (uint32_t)floor(pixels + 0.5);
}

/* The frame as the player's aspect ratio narrows it: the largest centered
 * rectangle of that shape, on whole pixels. */
static void tb_viewport_narrow(const tb_viewport_frame *frame, tb_viewport_span *across,
                               tb_viewport_span *down)
{
    const double ratio = (double)frame->aspect_ratio;

    *across = (tb_viewport_span){.length = frame->width};
    *down = (tb_viewport_span){.length = frame->height};
    if (!(ratio > 0.0) || !isfinite(ratio) || frame->width == 0 || frame->height == 0)
    {
        return;
    }
    const double wide = (double)frame->height * ratio;
    if (wide < (double)frame->width)
    {
        uint32_t length = tb_viewport_whole(wide);
        length = length < 1u ? 1u : length;
        across->start = (frame->width - length) / 2u;
        across->length = length;
        return;
    }
    uint32_t length = tb_viewport_whole((double)frame->width / ratio);
    length = length < 1u ? 1u : length;
    length = length > frame->height ? frame->height : length;
    down->start = (frame->height - length) / 2u;
    down->length = length;
}

/* An axis of the viewport in an axis of the frame: kept at the design's
 * extent and centered, or open to the whole frame. `base` is the pixels a
 * unit of the design before the policy's factor. */
static tb_viewport_span tb_viewport_place(tb_viewport_span frame, uint32_t design, float base,
                                          float factor, tb_viewport_axis axis)
{
    tb_viewport_span span = {.start = frame.start, .length = frame.length};

    span.scale = base * factor;
    if (axis == TB_VIEWPORT_AXIS_KEPT)
    {
        const double extent = (double)design * (double)base;
        const uint32_t length = tb_viewport_whole(extent);
        if (length <= frame.length)
        {
            /* The design fits: centered on whole pixels, showing it whole. */
            span.length = length < 1u ? 1u : length;
            span.start = frame.start + (frame.length - span.length) / 2u;
            span.units = (float)design / factor;
            return span;
        }
        /* Whole pixels a unit larger than the frame: what fits, from the
         * design's top left. */
    }
    span.units = (float)frame.length / span.scale;
    return span;
}

/* Without a design size: logical pixels at the display's scale, over the
 * whole frame. */
static tb_viewport_span tb_viewport_logical(tb_viewport_span frame, float pixels, bool whole)
{
    tb_viewport_span span = frame;

    span.scale = whole ? fmaxf(floorf(pixels), 1.0f) : pixels;
    span.units = (float)frame.length / span.scale;
    return span;
}

static tb_viewport tb_viewport_of(tb_viewport_span across, tb_viewport_span down)
{
    return (tb_viewport){.x = across.start,
                         .y = down.start,
                         .width = across.length,
                         .height = down.length,
                         .scale = {across.scale, down.scale},
                         .size = {across.units, down.units}};
}

/* The pixels of a side of the viewport render mode's target: the units
 * shown, rounded up, but for what float arithmetic leaves over a whole
 * number. */
static uint32_t tb_viewport_target_side(float units)
{
    const double pixels = ceil((double)units - TB_VIEWPORT_TARGET_SLACK);

    if (!(pixels >= 1.0))
    {
        return 1u;
    }
    return pixels < (double)TB_VIEWPORT_MAX_TARGET ? (uint32_t)pixels : TB_VIEWPORT_MAX_TARGET;
}

/* The viewport of a policy on a frame, without its target. */
static tb_viewport tb_viewport_placed(const tb_viewport_policy *policy,
                                      const tb_viewport_frame *frame)
{
    tb_viewport_span across = {0};
    tb_viewport_span down = {0};
    tb_viewport_axis axis_x = TB_VIEWPORT_AXIS_KEPT;
    tb_viewport_axis axis_y = TB_VIEWPORT_AXIS_KEPT;
    const bool whole = policy != NULL && policy->scale_mode == TB_VIEWPORT_SCALE_INTEGER;

    TB_ASSERT(policy != NULL && frame != NULL);
    const float factor = tb_viewport_positive(policy->scale);
    tb_viewport_narrow(frame, &across, &down);
    if (across.length == 0 || down.length == 0)
    {
        return (tb_viewport){.scale = {1.0f, 1.0f}};
    }
    if (policy->design_width == 0 || policy->design_height == 0)
    {
        const float pixels = tb_viewport_positive(frame->display_scale) * factor;
        return tb_viewport_of(tb_viewport_logical(across, pixels, whole),
                              tb_viewport_logical(down, pixels, whole));
    }
    const float fit_x = (float)across.length / (float)policy->design_width;
    const float fit_y = (float)down.length / (float)policy->design_height;
    float base_x = fminf(fit_x, fit_y);
    float base_y = base_x;
    switch (policy->aspect)
    {
    case TB_VIEWPORT_ASPECT_KEEP_WIDTH:
        axis_y = TB_VIEWPORT_AXIS_OPEN;
        break;
    case TB_VIEWPORT_ASPECT_KEEP_HEIGHT:
        axis_x = TB_VIEWPORT_AXIS_OPEN;
        break;
    case TB_VIEWPORT_ASPECT_EXPAND:
        axis_x = TB_VIEWPORT_AXIS_OPEN;
        axis_y = TB_VIEWPORT_AXIS_OPEN;
        break;
    case TB_VIEWPORT_ASPECT_IGNORE:
        base_x = fit_x;
        base_y = fit_y;
        break;
    case TB_VIEWPORT_ASPECT_KEEP:
    case TB_VIEWPORT_ASPECT_COUNT:
        break;
    }
    if (whole)
    {
        base_x = fmaxf(floorf(base_x), 1.0f);
        base_y = fmaxf(floorf(base_y), 1.0f);
    }
    return tb_viewport_of(tb_viewport_place(across, policy->design_width, base_x, factor, axis_x),
                          tb_viewport_place(down, policy->design_height, base_y, factor, axis_y));
}

tb_viewport tb_viewport_compute(const tb_viewport_policy *policy, const tb_viewport_frame *frame)
{
    tb_viewport viewport = {0};

    TB_ASSERT(policy != NULL && frame != NULL);
    viewport = tb_viewport_placed(policy, frame);
    if (policy->render_mode == TB_VIEWPORT_RENDER_VIEWPORT && viewport.width > 0 &&
        viewport.height > 0)
    {
        viewport.target_width = tb_viewport_target_side(viewport.size.x);
        viewport.target_height = tb_viewport_target_side(viewport.size.y);
    }
    return viewport;
}

tb_vec2 tb_viewport_to_pixels(const tb_viewport *viewport, tb_vec2 units)
{
    TB_ASSERT(viewport != NULL);
    return (tb_vec2){(float)viewport->x + units.x * viewport->scale.x,
                     (float)viewport->y + units.y * viewport->scale.y};
}

tb_vec2 tb_viewport_from_pixels(const tb_viewport *viewport, tb_vec2 pixel)
{
    TB_ASSERT(viewport != NULL);
    TB_ASSERT(viewport->scale.x > 0.0f && viewport->scale.y > 0.0f);
    return (tb_vec2){(pixel.x - (float)viewport->x) / viewport->scale.x,
                     (pixel.y - (float)viewport->y) / viewport->scale.y};
}

bool tb_viewport_contains(const tb_viewport *viewport, tb_vec2 pixel)
{
    TB_ASSERT(viewport != NULL);
    const float left = (float)viewport->x;
    const float top = (float)viewport->y;
    return pixel.x >= left && pixel.y >= top && pixel.x < left + (float)viewport->width &&
           pixel.y < top + (float)viewport->height;
}

float tb_viewport_aspect_of(const tb_viewport *viewport)
{
    TB_ASSERT(viewport != NULL);
    if (!(viewport->size.x > 0.0f) || !(viewport->size.y > 0.0f))
    {
        return 1.0f;
    }
    return viewport->size.x / viewport->size.y;
}

float tb_viewport_uniform_scale(const tb_viewport *viewport)
{
    TB_ASSERT(viewport != NULL);
    return fminf(viewport->scale.x, viewport->scale.y);
}

/* The value of a hexadecimal digit, or -1. */
static int tb_viewport_hex(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F')
    {
        return digit - 'A' + 10;
    }
    return -1;
}

bool tb_viewport_parse_color(const char *text, tb_vec4 *color)
{
    enum
    {
        TB_VIEWPORT_COLOR_DIGITS = 6
    };
    float channels[3] = {0};

    TB_ASSERT(text != NULL && color != NULL);
    if (text[0] != '#')
    {
        return false;
    }
    for (size_t index = 0; index < TB_VIEWPORT_COLOR_DIGITS; index++)
    {
        const int digit = tb_viewport_hex(text[index + 1u]);
        if (digit < 0)
        {
            return false;
        }
        channels[index / 2u] = channels[index / 2u] * 16.0f + (float)digit;
    }
    if (text[TB_VIEWPORT_COLOR_DIGITS + 1] != '\0')
    {
        return false;
    }
    *color = (tb_vec4){channels[0] / 255.0f, channels[1] / 255.0f, channels[2] / 255.0f, 1.0f};
    return true;
}

const char *tb_viewport_aspect_name(tb_viewport_aspect aspect)
{
    return (size_t)aspect < TB_VIEWPORT_ASPECT_COUNT ? tb_viewport_aspect_names[aspect] : "unknown";
}

const char *tb_viewport_scale_mode_name(tb_viewport_scale_mode mode)
{
    return (size_t)mode < TB_VIEWPORT_SCALE_COUNT ? tb_viewport_scale_mode_names[mode] : "unknown";
}

const char *tb_viewport_render_mode_name(tb_viewport_render_mode mode)
{
    return (size_t)mode < TB_VIEWPORT_RENDER_COUNT ? tb_viewport_render_mode_names[mode]
                                                   : "unknown";
}
