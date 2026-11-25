/* Limits on a window's size: its minimum, the room on its display, and the
 * aspect it keeps while the player drags its border. Arithmetic alone, the
 * same for every backend, which each applies in the system's own way. */
#include "window.h"

#include "badger/assert.h"

static uint32_t tb_window_larger(uint32_t value, uint32_t other)
{
    return value > other ? value : other;
}

static uint32_t tb_window_smaller(uint32_t value, uint32_t other)
{
    return value < other ? value : other;
}

/* `value` times `numerator` over `denominator`, rounded, at least 1 and at
 * most UINT32_MAX. */
static uint32_t tb_window_ratio(uint32_t value, uint32_t numerator, uint32_t denominator)
{
    const uint64_t scaled =
        ((uint64_t)value * numerator + denominator / 2u) / (uint64_t)denominator;

    if (scaled > UINT32_MAX)
    {
        return UINT32_MAX;
    }
    return scaled < 1u ? 1u : (uint32_t)scaled;
}

void tb_window_least_size(const tb_window_options *options, uint32_t *width, uint32_t *height)
{
    *width = tb_window_larger(options->min_width, TB_WINDOW_SMALLEST);
    *height = tb_window_larger(options->min_height, TB_WINDOW_SMALLEST);
}

bool tb_window_wants_aspect(const tb_window_options *options)
{
    return options->mode == TB_WINDOW_MODE_WINDOWED && !options->fixed_size &&
           options->aspect_width != 0 && options->aspect_height != 0;
}

void tb_window_fit_size(const tb_window_options *options, uint32_t room_width, uint32_t room_height,
                        uint32_t *width, uint32_t *height)
{
    uint32_t least_width = 0;
    uint32_t least_height = 0;
    uint32_t fitted_width = 0;
    uint32_t fitted_height = 0;

    TB_ASSERT(room_width > 0 && room_height > 0);
    tb_window_least_size(options, &least_width, &least_height);
    least_width = tb_window_smaller(least_width, room_width);
    least_height = tb_window_smaller(least_height, room_height);
    fitted_width = tb_window_larger(tb_window_smaller(*width, room_width), least_width);
    fitted_height = tb_window_larger(tb_window_smaller(*height, room_height), least_height);
    if (tb_window_wants_aspect(options))
    {
        const uint32_t aspect_width = options->aspect_width;
        const uint32_t aspect_height = options->aspect_height;
        /* The width decides, unless the room or the least size says
         * otherwise. */
        fitted_height = tb_window_ratio(fitted_width, aspect_height, aspect_width);
        if (fitted_height < least_height)
        {
            fitted_height = least_height;
            fitted_width = tb_window_ratio(fitted_height, aspect_width, aspect_height);
        }
        if (fitted_width > room_width)
        {
            fitted_width = room_width;
            fitted_height = tb_window_ratio(fitted_width, aspect_height, aspect_width);
        }
        if (fitted_height > room_height)
        {
            fitted_height = room_height;
            fitted_width = tb_window_ratio(fitted_height, aspect_width, aspect_height);
        }
    }
    *width = fitted_width;
    *height = fitted_height;
}

static bool tb_window_edge_is_left(tb_window_edge edge)
{
    return edge == TB_WINDOW_EDGE_LEFT || edge == TB_WINDOW_EDGE_TOP_LEFT ||
           edge == TB_WINDOW_EDGE_BOTTOM_LEFT;
}

static bool tb_window_edge_is_top(tb_window_edge edge)
{
    return edge == TB_WINDOW_EDGE_TOP || edge == TB_WINDOW_EDGE_TOP_LEFT ||
           edge == TB_WINDOW_EDGE_TOP_RIGHT;
}

/* The length of a span of a frame, without its border, at least 1. */
static uint32_t tb_window_client_span(int32_t start, int32_t end, uint32_t border)
{
    const int64_t span = (int64_t)end - (int64_t)start - (int64_t)border;

    if (span < 1)
    {
        return 1u;
    }
    return span > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)span;
}

/* An edge `length` from another, as far as an int32_t reaches. */
static int32_t tb_window_offset(int32_t from, int64_t length)
{
    const int64_t edge = (int64_t)from + length;

    if (edge > INT32_MAX)
    {
        return INT32_MAX;
    }
    return edge < INT32_MIN ? INT32_MIN : (int32_t)edge;
}

void tb_window_drag_to_aspect(tb_window_rect *frame, tb_window_edge edge, uint32_t border_width,
                              uint32_t border_height, uint32_t aspect_width, uint32_t aspect_height,
                              uint32_t least_width, uint32_t least_height)
{
    uint32_t width = tb_window_client_span(frame->left, frame->right, border_width);
    uint32_t height = tb_window_client_span(frame->top, frame->bottom, border_height);
    int64_t outer_width = 0;
    int64_t outer_height = 0;

    TB_ASSERT(aspect_width != 0 && aspect_height != 0);
    switch (edge)
    {
    case TB_WINDOW_EDGE_LEFT:
    case TB_WINDOW_EDGE_RIGHT:
        height = tb_window_ratio(width, aspect_height, aspect_width);
        break;
    case TB_WINDOW_EDGE_TOP:
    case TB_WINDOW_EDGE_BOTTOM:
        width = tb_window_ratio(height, aspect_width, aspect_height);
        break;
    case TB_WINDOW_EDGE_TOP_LEFT:
    case TB_WINDOW_EDGE_TOP_RIGHT:
    case TB_WINDOW_EDGE_BOTTOM_LEFT:
    case TB_WINDOW_EDGE_BOTTOM_RIGHT: {
        const uint32_t height_by_width = tb_window_ratio(width, aspect_height, aspect_width);
        if (height_by_width >= height)
        {
            height = height_by_width;
        }
        else
        {
            width = tb_window_ratio(height, aspect_width, aspect_height);
        }
        break;
    }
    }
    if (width < least_width)
    {
        width = least_width;
        height = tb_window_ratio(width, aspect_height, aspect_width);
    }
    if (height < least_height)
    {
        height = least_height;
        width = tb_window_ratio(height, aspect_width, aspect_height);
    }
    outer_width = (int64_t)width + border_width;
    outer_height = (int64_t)height + border_height;
    if (tb_window_edge_is_left(edge))
    {
        frame->left = tb_window_offset(frame->right, -outer_width);
    }
    else
    {
        frame->right = tb_window_offset(frame->left, outer_width);
    }
    if (tb_window_edge_is_top(edge))
    {
        frame->top = tb_window_offset(frame->bottom, -outer_height);
    }
    else
    {
        frame->bottom = tb_window_offset(frame->top, outer_height);
    }
}
