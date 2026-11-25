/* Scrolling: the offsets of kinds whose children scroll, a widget scrolled
 * into view, and the scrollframe kind, a frame whose grid is laid out at
 * its natural size and seen through it, with bars to scroll it. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Lines of text a notch of the wheel scrolls. */
#define TB_GUI_WHEEL_LINES 3.0f
/* No axis is being dragged. */
#define TB_GUI_NO_AXIS (-1)

/* ---------------------------------------------------------------------------
 * Offsets.
 * ------------------------------------------------------------------------- */

static bool tb_gui_scrolls(const tb_gui_node *node)
{
    return node != NULL && node->kind->viewport != NULL;
}

tb_vec2 tb_widget_scroll_offset(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return tb_gui_scrolls(node) ? node->scroll : (tb_vec2){0};
}

bool tb_widget_scroll_to(tb_widget widget, tb_vec2 offset)
{
    tb_gui_node *const node = tb_gui_node_of(widget);

    TB_ASSERT(isfinite(offset.x) && isfinite(offset.y));
    if (!tb_gui_scrolls(node))
    {
        return false;
    }
    node->scroll = (tb_vec2){fmaxf(offset.x, 0.0f), fmaxf(offset.y, 0.0f)};
    tb_gui_mark_layout(widget.gui);
    return true;
}

/* How far to move a span to bring [low, high) into [start, end): as little
 * as shows it, its low edge first when it is longer. */
static float tb_gui_bring_into(float low, float high, float start, float end)
{
    if (low < start)
    {
        return low - start;
    }
    if (high > end)
    {
        return fminf(high - end, low - start);
    }
    return 0.0f;
}

void tb_gui_see_rect(tb_gui *gui, uint32_t index, tb_canvas_rect rect)
{
    /* Scrolling an ancestor moves every widget in it as far; what is left
     * for the next ancestor out is worked out from that, without another
     * layout until the next one. */
    for (uint32_t at = tb_gui_node_at(gui, index)->parent; at != TB_GUI_NO_NODE;
         at = tb_gui_node_at(gui, at)->parent)
    {
        tb_gui_node *const owner = tb_gui_node_at(gui, at);
        if (!tb_gui_scrolls(owner) || !owner->shown)
        {
            continue;
        }
        const tb_canvas_rect clip = owner->clip;
        const tb_vec2 most = {fmaxf(owner->content.x - clip.width, 0.0f),
                              fmaxf(owner->content.y - clip.height, 0.0f)};
        const tb_vec2 wanted = {owner->scroll.x + tb_gui_bring_into(rect.x, rect.x + rect.width,
                                                                    clip.x, clip.x + clip.width),
                                owner->scroll.y + tb_gui_bring_into(rect.y, rect.y + rect.height,
                                                                    clip.y, clip.y + clip.height)};
        const tb_vec2 kept = {fminf(fmaxf(wanted.x, 0.0f), most.x),
                              fminf(fmaxf(wanted.y, 0.0f), most.y)};
        rect.x -= kept.x - owner->scroll.x;
        rect.y -= kept.y - owner->scroll.y;
        if (kept.x != owner->scroll.x || kept.y != owner->scroll.y)
        {
            owner->scroll = kept;
            tb_gui_mark_layout(gui);
        }
    }
}

bool tb_widget_see(tb_widget widget)
{
    if (tb_gui_node_of(widget) == NULL)
    {
        return false;
    }
    tb_gui_layout(widget.gui);
    const tb_gui_node *const node = tb_gui_node_of(widget);
    if (!node->shown)
    {
        return false;
    }
    tb_gui_see_rect(widget.gui, widget.index, node->rect);
    return true;
}

/* ---------------------------------------------------------------------------
 * Scroll frames.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_scrollframe
{
    float width;
    float height;
    bool xscroll;
    bool yscroll;
    bool border;
    /* As last arranged: the clip, the content, and which bars are shown. */
    tb_canvas_rect view;
    tb_vec2 content;
    tb_canvas_rect tracks[2];
    bool bars[2];
    /* The bar being dragged, and where its thumb was taken. */
    int drag_axis;
    float grab;
    int hot_axis;
} tb_gui_scrollframe;

static void tb_gui_scrollframe_init(tb_widget widget, void *data)
{
    tb_gui_scrollframe *const frame = data;

    (void)widget;
    frame->xscroll = true;
    frame->yscroll = true;
    frame->drag_axis = TB_GUI_NO_AXIS;
    frame->hot_axis = TB_GUI_NO_AXIS;
}

static float tb_gui_bar_width(tb_widget widget)
{
    return tb_gui_theme(widget.gui)->scrollbar_width;
}

static tb_vec2 tb_gui_scrollframe_measure(tb_widget widget, const void *data)
{
    const tb_gui_scrollframe *const frame = data;
    const tb_vec2 grid = tb_grid_request(widget);
    const float bar = tb_gui_bar_width(widget);
    tb_vec2 wanted = {frame->width > 0.0f ? frame->width : grid.x,
                      frame->height > 0.0f ? frame->height : grid.y};

    /* A bar is wanted, beside the content, where the content is larger. */
    const bool down = frame->yscroll && grid.y > wanted.y;
    const bool across = frame->xscroll && grid.x > wanted.x;
    wanted.x += down ? bar : 0.0f;
    wanted.y += across ? bar : 0.0f;
    return wanted;
}

static tb_gui_edges tb_gui_scrollframe_insets(tb_widget widget, const void *data)
{
    const tb_gui_scrollframe *const frame = data;
    const float edge = frame->border ? tb_gui_theme(widget.gui)->border : 0.0f;

    return (tb_gui_edges){edge, edge, edge, edge};
}

static tb_gui_viewport tb_gui_scrollframe_viewport(tb_widget widget, void *data,
                                                   tb_canvas_rect inner)
{
    tb_gui_scrollframe *const frame = data;
    const tb_vec2 grid = tb_grid_request(widget);
    const float bar = tb_gui_bar_width(widget);
    tb_vec2 seen = {inner.width, inner.height};

    /* A bar along one axis takes room from the other, which may then need
     * its own. */
    bool down = frame->yscroll && grid.y > seen.y;
    seen.x = inner.width - (down ? bar : 0.0f);
    const bool across = frame->xscroll && grid.x > seen.x;
    seen.y = inner.height - (across ? bar : 0.0f);
    down = frame->yscroll && grid.y > seen.y;
    seen = (tb_vec2){fmaxf(inner.width - (down ? bar : 0.0f), 0.0f), fmaxf(seen.y, 0.0f)};

    frame->view = (tb_canvas_rect){inner.x, inner.y, seen.x, seen.y};
    frame->content = (tb_vec2){frame->xscroll ? fmaxf(grid.x, seen.x) : seen.x,
                               frame->yscroll ? fmaxf(grid.y, seen.y) : seen.y};
    frame->bars[TB_GUI_COLUMNS] = across;
    frame->bars[TB_GUI_ROWS] = down;
    frame->tracks[TB_GUI_COLUMNS] = (tb_canvas_rect){inner.x, inner.y + seen.y, seen.x, bar};
    frame->tracks[TB_GUI_ROWS] = (tb_canvas_rect){inner.x + seen.x, inner.y, bar, seen.y};
    return (tb_gui_viewport){.clip = frame->view, .content = frame->content};
}

static tb_gui_bar tb_gui_scrollframe_bar(tb_widget widget, const tb_gui_scrollframe *frame,
                                         int axis)
{
    const tb_vec2 offset = tb_widget_scroll_offset(widget);
    const bool vertical = axis == TB_GUI_ROWS;

    return (tb_gui_bar){.track = frame->tracks[axis],
                        .vertical = vertical,
                        .offset = vertical ? offset.y : offset.x,
                        .view = vertical ? frame->view.height : frame->view.width,
                        .content = vertical ? frame->content.y : frame->content.x};
}

static void tb_gui_scrollframe_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_scrollframe *const frame = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);

    for (int axis = TB_GUI_COLUMNS; axis <= TB_GUI_ROWS; axis++)
    {
        if (frame->bars[axis])
        {
            const tb_gui_bar bar = tb_gui_scrollframe_bar(widget, frame, axis);
            tb_gui_bar_draw(paint, &bar, frame->hot_axis == axis || frame->drag_axis == axis);
        }
    }
    if (frame->border)
    {
        tb_gui_paint_outline(paint, tb_gui_paint_rect(paint), 0.0f, theme->border,
                             theme->colors[TB_UI_COLOR_BORDER]);
    }
}

/* Scroll along an axis to an offset; returns whether it moved. */
static bool tb_gui_scrollframe_move(tb_widget widget, const tb_gui_scrollframe *frame, int axis,
                                    float offset)
{
    const tb_gui_bar bar = tb_gui_scrollframe_bar(widget, frame, axis);
    const float kept = tb_gui_bar_clamp(&bar, offset);
    tb_vec2 scroll = tb_widget_scroll_offset(widget);

    if (kept == bar.offset)
    {
        return false;
    }
    if (axis == TB_GUI_ROWS)
    {
        scroll.y = kept;
    }
    else
    {
        scroll.x = kept;
    }
    return tb_widget_scroll_to(widget, scroll);
}

/* The axis whose bar is at a point, or none. */
static int tb_gui_scrollframe_bar_at(tb_widget widget, const tb_gui_scrollframe *frame,
                                     tb_vec2 point, tb_gui_bar_part *part)
{
    for (int axis = TB_GUI_COLUMNS; axis <= TB_GUI_ROWS; axis++)
    {
        const tb_gui_bar bar = tb_gui_scrollframe_bar(widget, frame, axis);
        *part = frame->bars[axis] ? tb_gui_bar_part_at(&bar, point) : TB_GUI_BAR_NONE;
        if (*part != TB_GUI_BAR_NONE)
        {
            return axis;
        }
    }
    return TB_GUI_NO_AXIS;
}

static bool tb_gui_scrollframe_press(tb_widget widget, tb_gui_scrollframe *frame,
                                     const tb_gui_event *event)
{
    tb_gui_bar_part part = TB_GUI_BAR_NONE;
    const int axis = tb_gui_scrollframe_bar_at(widget, frame, event->position, &part);

    if (event->button != TB_MOUSE_BUTTON_LEFT || axis == TB_GUI_NO_AXIS)
    {
        return false;
    }
    const tb_gui_bar bar = tb_gui_scrollframe_bar(widget, frame, axis);
    if (part == TB_GUI_BAR_THUMB)
    {
        frame->drag_axis = axis;
        frame->grab = tb_gui_bar_grab(&bar, event->position);
        return true;
    }
    (void)tb_gui_scrollframe_move(widget, frame, axis, tb_gui_bar_page(&bar, part));
    return true;
}

static bool tb_gui_scrollframe_wheel(tb_widget widget, const tb_gui_scrollframe *frame,
                                     const tb_gui_event *event)
{
    const float step = tb_gui_line_height(widget.gui) * TB_GUI_WHEEL_LINES;
    const bool sideways = (event->modifiers & TB_MODIFIER_SHIFT) != 0 || event->wheel.x != 0.0f;
    const float notches = event->wheel.x != 0.0f ? event->wheel.x : -event->wheel.y;
    const int axis = sideways ? TB_GUI_COLUMNS : TB_GUI_ROWS;
    const tb_gui_bar bar = tb_gui_scrollframe_bar(widget, frame, axis);

    if (!(axis == TB_GUI_ROWS ? frame->yscroll : frame->xscroll))
    {
        return false;
    }
    /* At its end, the wheel goes on to an ancestor that scrolls. */
    return tb_gui_scrollframe_move(widget, frame, axis, bar.offset + notches * step);
}

static bool tb_gui_scrollframe_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_scrollframe *const frame = data;
    tb_gui_bar_part part = TB_GUI_BAR_NONE;

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS:
        return tb_gui_scrollframe_press(widget, frame, event);
    case TB_GUI_EVENT_MOTION:
        if (frame->drag_axis != TB_GUI_NO_AXIS)
        {
            const tb_gui_bar bar = tb_gui_scrollframe_bar(widget, frame, frame->drag_axis);
            (void)tb_gui_scrollframe_move(widget, frame, frame->drag_axis,
                                          tb_gui_bar_drag(&bar, event->position, frame->grab));
            return true;
        }
        frame->hot_axis = tb_gui_scrollframe_bar_at(widget, frame, event->position, &part);
        frame->hot_axis = part == TB_GUI_BAR_THUMB ? frame->hot_axis : TB_GUI_NO_AXIS;
        return false;
    case TB_GUI_EVENT_RELEASE:
        frame->drag_axis = TB_GUI_NO_AXIS;
        return false;
    case TB_GUI_EVENT_LEAVE:
        frame->hot_axis = TB_GUI_NO_AXIS;
        return false;
    case TB_GUI_EVENT_WHEEL:
        return tb_gui_scrollframe_wheel(widget, frame, event);
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_KEY:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static bool tb_gui_scrollframe_set_size(float *slot, const tb_gui_value *value)
{
    if (value->number < 0.0f)
    {
        return false;
    }
    *slot = value->number;
    return true;
}

static bool tb_gui_scrollframe_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scrollframe *const frame = data;

    (void)widget;
    return tb_gui_scrollframe_set_size(&frame->width, value);
}

static bool tb_gui_scrollframe_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_scrollframe *const frame = data;

    (void)widget;
    *value = tb_gui_float(frame->width);
    return true;
}

static bool tb_gui_scrollframe_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scrollframe *const frame = data;

    (void)widget;
    return tb_gui_scrollframe_set_size(&frame->height, value);
}

static bool tb_gui_scrollframe_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_scrollframe *const frame = data;

    (void)widget;
    *value = tb_gui_float(frame->height);
    return true;
}

static bool tb_gui_scrollframe_set_offset(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_widget_scroll_to(widget, value->vec2);
}

static bool tb_gui_scrollframe_get_offset(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_vec2(tb_widget_scroll_offset(widget));
    return true;
}

static const tb_gui_property tb_gui_scrollframe_properties[] = {
    {.name = "width",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_scrollframe_get_width,
     .set = tb_gui_scrollframe_set_width},
    {.name = "height",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_scrollframe_get_height,
     .set = tb_gui_scrollframe_set_height},
    {.name = "xscroll",
     .type = TB_GUI_TYPE_BOOL,
     .offset = offsetof(tb_gui_scrollframe, xscroll),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "yscroll",
     .type = TB_GUI_TYPE_BOOL,
     .offset = offsetof(tb_gui_scrollframe, yscroll),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "border",
     .type = TB_GUI_TYPE_BOOL,
     .offset = offsetof(tb_gui_scrollframe, border),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "offset",
     .type = TB_GUI_TYPE_VEC2,
     .get = tb_gui_scrollframe_get_offset,
     .set = tb_gui_scrollframe_set_offset},
};

static const tb_widget_class tb_gui_scrollframe_kind = {
    .name = "scrollframe",
    .data_size = sizeof(tb_gui_scrollframe),
    .properties = tb_gui_scrollframe_properties,
    .property_count = TB_GUI_COUNT_OF(tb_gui_scrollframe_properties),
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_scrollframe_init,
    .measure = tb_gui_scrollframe_measure,
    .insets = tb_gui_scrollframe_insets,
    .draw = tb_gui_scrollframe_draw,
    .event = tb_gui_scrollframe_event,
    .viewport = tb_gui_scrollframe_viewport};

const tb_widget_class *tb_scrollframe_class(void)
{
    return &tb_gui_scrollframe_kind;
}

tb_widget tb_scrollframe(tb_widget parent, const tb_scrollframe_desc *desc)
{
    const tb_scrollframe_desc chosen = desc != NULL ? *desc : (tb_scrollframe_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_scrollframe_kind);
    const tb_gui_value values[] = {tb_gui_float(chosen.width), tb_gui_float(chosen.height),
                                   tb_gui_bool(!chosen.no_xscroll), tb_gui_bool(!chosen.no_yscroll),
                                   tb_gui_bool(chosen.border)};
    static const char *const names[] = {"width", "height", "xscroll", "yscroll", "border"};

    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    return widget;
}

void tb_gui_register_scroll_kinds(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_scrollframe_kind);
    (void)tb_gui_register_class(gui, tb_scrollbar_class());
}
