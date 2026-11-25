/* Scroll bars: the geometry and drawing the kinds that scroll share, a
 * track with a thumb as long as the part of the content seen, and the
 * scrollbar kind, a bar alone that a game ties to what it scrolls, Tk's
 * `scrollbar`. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Notches of the wheel a bar alone scrolls a tenth of its content by. */
#define TB_GUI_SCROLLBAR_WHEEL_STEP 0.1f
/* A bar alone is this many times as long as it is wide, unless stretched. */
#define TB_GUI_SCROLLBAR_LENGTH 4.0f

/* ---------------------------------------------------------------------------
 * Bars.
 * ------------------------------------------------------------------------- */

static float tb_gui_bar_start(const tb_gui_bar *bar)
{
    return bar->vertical ? bar->track.y : bar->track.x;
}

static float tb_gui_bar_length(const tb_gui_bar *bar)
{
    return bar->vertical ? bar->track.height : bar->track.width;
}

static float tb_gui_bar_thickness(const tb_gui_bar *bar)
{
    return bar->vertical ? bar->track.width : bar->track.height;
}

static float tb_gui_bar_along(const tb_gui_bar *bar, tb_vec2 point)
{
    return bar->vertical ? point.y : point.x;
}

float tb_gui_bar_range(const tb_gui_bar *bar)
{
    return fmaxf(bar->content - bar->view, 0.0f);
}

float tb_gui_bar_clamp(const tb_gui_bar *bar, float offset)
{
    return fminf(fmaxf(offset, 0.0f), tb_gui_bar_range(bar));
}

static float tb_gui_bar_thumb_length(const tb_gui_bar *bar)
{
    const float length = tb_gui_bar_length(bar);
    const float shown = bar->content > 0.0f ? fminf(bar->view / bar->content, 1.0f) : 1.0f;

    return fminf(fmaxf(length * shown, tb_gui_bar_thickness(bar)), length);
}

tb_canvas_rect tb_gui_bar_thumb(const tb_gui_bar *bar)
{
    const float length = tb_gui_bar_thumb_length(bar);
    const float range = tb_gui_bar_range(bar);
    const float travel = tb_gui_bar_length(bar) - length;
    const float along = range > 0.0f ? travel * tb_gui_bar_clamp(bar, bar->offset) / range : 0.0f;
    tb_canvas_rect thumb = bar->track;

    if (bar->vertical)
    {
        thumb.y += along;
        thumb.height = length;
    }
    else
    {
        thumb.x += along;
        thumb.width = length;
    }
    return thumb;
}

tb_gui_bar_part tb_gui_bar_part_at(const tb_gui_bar *bar, tb_vec2 point)
{
    const tb_canvas_rect thumb = tb_gui_bar_thumb(bar);
    const float along = tb_gui_bar_along(bar, point);
    const float thumb_start = bar->vertical ? thumb.y : thumb.x;
    const float thumb_length = bar->vertical ? thumb.height : thumb.width;

    if (!tb_ui_rect_contains(bar->track, point))
    {
        return TB_GUI_BAR_NONE;
    }
    if (along < thumb_start)
    {
        return TB_GUI_BAR_BEFORE;
    }
    if (along >= thumb_start + thumb_length)
    {
        return TB_GUI_BAR_AFTER;
    }
    return TB_GUI_BAR_THUMB;
}

float tb_gui_bar_grab(const tb_gui_bar *bar, tb_vec2 point)
{
    const tb_canvas_rect thumb = tb_gui_bar_thumb(bar);
    return tb_gui_bar_along(bar, point) - (bar->vertical ? thumb.y : thumb.x);
}

float tb_gui_bar_drag(const tb_gui_bar *bar, tb_vec2 point, float grab)
{
    const float travel = tb_gui_bar_length(bar) - tb_gui_bar_thumb_length(bar);
    const float moved = tb_gui_bar_along(bar, point) - grab - tb_gui_bar_start(bar);

    if (!(travel > 0.0f))
    {
        return 0.0f;
    }
    return tb_gui_bar_clamp(bar, moved / travel * tb_gui_bar_range(bar));
}

float tb_gui_bar_page(const tb_gui_bar *bar, tb_gui_bar_part part)
{
    switch (part)
    {
    case TB_GUI_BAR_BEFORE:
        return tb_gui_bar_clamp(bar, bar->offset - bar->view);
    case TB_GUI_BAR_AFTER:
        return tb_gui_bar_clamp(bar, bar->offset + bar->view);
    case TB_GUI_BAR_NONE:
    case TB_GUI_BAR_THUMB:
        break;
    }
    return bar->offset;
}

void tb_gui_bar_draw(tb_gui_paint *paint, const tb_gui_bar *bar, bool hot, const tb_style *style)
{
    const float inset = tb_gui_style_number_named(style, "scrollbar_inset");
    const tb_canvas_rect thumb = tb_ui_rect_inset(tb_gui_bar_thumb(bar), inset, inset);
    const char *const grab = hot ? "scrollbar_thumb_hot" : "scrollbar_thumb";

    tb_gui_paint_fill(paint, bar->track, 0.0f, tb_gui_style_color_named(style, "scrollbar_trough"));
    tb_gui_paint_fill(paint, thumb, fminf(thumb.width, thumb.height) * 0.5f,
                      tb_gui_style_color_named(style, grab));
}

float tb_gui_bar_width(tb_widget widget)
{
    return tb_gui_style_number_named(tb_style_in(widget, 0), "scrollbar_width");
}

/* A scroll bar's style: its bar's elements. */
static const tb_style_element tb_gui_scrollbar_elements[TB_GUI_BAR_ELEMENTS] = {
    TB_GUI_BAR_STYLE_ELEMENTS};

static const tb_widget_style_desc tb_gui_scrollbar_style = {.name = "Scrollbar",
                                                            .states = TB_WIDGET_DISABLED,
                                                            .elements = tb_gui_scrollbar_elements,
                                                            .element_count = TB_GUI_BAR_ELEMENTS};

/* ---------------------------------------------------------------------------
 * The scrollbar kind.
 * ------------------------------------------------------------------------- */

static const char *const tb_gui_orient_names[] = {"horizontal", "vertical"};

typedef struct tb_gui_scrollbar
{
    int32_t orient;
    float first;
    float last;
    tb_gui_command command;
    bool dragging;
    bool hot;
    float grab;
} tb_gui_scrollbar;

static void tb_gui_scrollbar_init(tb_widget widget, void *data)
{
    tb_gui_scrollbar *const scrollbar = data;

    (void)widget;
    scrollbar->last = 1.0f;
}

/* The bar of a scrollbar alone: a content of 1, of which first to last is seen. */
static tb_gui_bar tb_gui_scrollbar_bar(tb_widget widget, const tb_gui_scrollbar *scrollbar)
{
    return (tb_gui_bar){.track = tb_widget_rect(widget),
                        .vertical = scrollbar->orient == TB_GUI_ORIENT_VERTICAL,
                        .offset = scrollbar->first,
                        .view = scrollbar->last - scrollbar->first,
                        .content = 1.0f};
}

static tb_vec2 tb_gui_scrollbar_measure(tb_widget widget, const void *data)
{
    const tb_gui_scrollbar *const scrollbar = data;
    const float width = tb_gui_bar_width(widget);

    if (scrollbar->orient == TB_GUI_ORIENT_VERTICAL)
    {
        return (tb_vec2){width, width * TB_GUI_SCROLLBAR_LENGTH};
    }
    return (tb_vec2){width * TB_GUI_SCROLLBAR_LENGTH, width};
}

static void tb_gui_scrollbar_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_scrollbar *const scrollbar = data;
    const tb_gui_bar bar = tb_gui_scrollbar_bar(widget, scrollbar);

    tb_gui_bar_draw(paint, &bar, scrollbar->hot || scrollbar->dragging, tb_style_of(widget));
}

/* Move the view to a first fraction, its length kept, and call the command. */
static void tb_gui_scrollbar_move(tb_widget widget, tb_gui_scrollbar *scrollbar, float first)
{
    const float length = scrollbar->last - scrollbar->first;
    const float kept = fminf(fmaxf(first, 0.0f), 1.0f - length);

    if (kept == scrollbar->first)
    {
        return;
    }
    scrollbar->first = kept;
    scrollbar->last = kept + length;
    (void)tb_widget_invoke(widget);
}

static bool tb_gui_scrollbar_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_scrollbar *const scrollbar = data;
    const tb_gui_bar bar = tb_gui_scrollbar_bar(widget, scrollbar);

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS: {
        const tb_gui_bar_part part = tb_gui_bar_part_at(&bar, event->position);
        if (event->button != TB_MOUSE_BUTTON_LEFT || part == TB_GUI_BAR_NONE)
        {
            return false;
        }
        if (part == TB_GUI_BAR_THUMB)
        {
            scrollbar->dragging = true;
            scrollbar->grab = tb_gui_bar_grab(&bar, event->position);
            return true;
        }
        tb_gui_scrollbar_move(widget, scrollbar, tb_gui_bar_page(&bar, part));
        return true;
    }
    case TB_GUI_EVENT_MOTION:
        scrollbar->hot = tb_gui_bar_part_at(&bar, event->position) == TB_GUI_BAR_THUMB;
        if (scrollbar->dragging)
        {
            tb_gui_scrollbar_move(widget, scrollbar,
                                  tb_gui_bar_drag(&bar, event->position, scrollbar->grab));
        }
        return true;
    case TB_GUI_EVENT_RELEASE:
        scrollbar->dragging = false;
        return true;
    case TB_GUI_EVENT_LEAVE:
        scrollbar->hot = false;
        return false;
    case TB_GUI_EVENT_WHEEL: {
        const float step =
            scrollbar->orient == TB_GUI_ORIENT_VERTICAL ? -event->wheel.y : event->wheel.x;
        const float before = scrollbar->first;
        tb_gui_scrollbar_move(widget, scrollbar,
                              scrollbar->first + step * TB_GUI_SCROLLBAR_WHEEL_STEP);
        return scrollbar->first != before;
    }
    case TB_GUI_EVENT_COMMAND:
        if (scrollbar->command.callback != NULL)
        {
            scrollbar->command.callback(event, scrollbar->command.user);
        }
        return true;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_KEY:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static bool tb_gui_scrollbar_set_fraction(float *slot, const tb_gui_value *value)
{
    if (value->number < 0.0f || value->number > 1.0f)
    {
        return false;
    }
    *slot = value->number;
    return true;
}

static bool tb_gui_scrollbar_set_first(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scrollbar *const scrollbar = data;

    (void)widget;
    return tb_gui_scrollbar_set_fraction(&scrollbar->first, value);
}

static bool tb_gui_scrollbar_get_first(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_scrollbar *const scrollbar = data;

    (void)widget;
    *value = tb_gui_float(scrollbar->first);
    return true;
}

static bool tb_gui_scrollbar_set_last(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scrollbar *const scrollbar = data;

    (void)widget;
    return tb_gui_scrollbar_set_fraction(&scrollbar->last, value);
}

static bool tb_gui_scrollbar_get_last(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_scrollbar *const scrollbar = data;

    (void)widget;
    *value = tb_gui_float(scrollbar->last);
    return true;
}

static const tb_gui_property tb_gui_scrollbar_properties[] = {
    {.name = "orient",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_scrollbar, orient),
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .names = tb_gui_orient_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_orient_names)},
    {.name = "first",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_scrollbar_get_first,
     .set = tb_gui_scrollbar_set_first},
    {.name = "last",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_scrollbar_get_last,
     .set = tb_gui_scrollbar_set_last},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_scrollbar, command)},
};

static const tb_widget_class tb_gui_scrollbar_kind = {
    .name = "scrollbar",
    .data_size = sizeof(tb_gui_scrollbar),
    .properties = tb_gui_scrollbar_properties,
    .property_count = TB_GUI_COUNT_OF(tb_gui_scrollbar_properties),
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_scrollbar_init,
    .measure = tb_gui_scrollbar_measure,
    .draw = tb_gui_scrollbar_draw,
    .event = tb_gui_scrollbar_event,
    .style = &tb_gui_scrollbar_style};

const tb_widget_class *tb_scrollbar_class(void)
{
    return &tb_gui_scrollbar_kind;
}

tb_widget tb_scrollbar(tb_widget parent, const tb_scrollbar_desc *desc)
{
    const tb_scrollbar_desc chosen = desc != NULL ? *desc : (tb_scrollbar_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_scrollbar_kind);
    const tb_gui_value orient = tb_gui_enum((int32_t)chosen.orient);
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);

    (void)tb_widget_set(widget, "orient", &orient);
    (void)tb_widget_set(widget, "command", &command);
    return widget;
}

bool tb_scrollbar_set(tb_widget scrollbar, float first, float last)
{
    tb_gui_scrollbar *const data =
        tb_widget_class_of(scrollbar) == &tb_gui_scrollbar_kind ? tb_widget_data(scrollbar) : NULL;

    if (data == NULL || !(first >= 0.0f) || !(last <= 1.0f) || !(first <= last))
    {
        return false;
    }
    data->first = first;
    data->last = last;
    return true;
}

bool tb_scrollbar_get(tb_widget scrollbar, float *first, float *last)
{
    const tb_gui_scrollbar *const data =
        tb_widget_class_of(scrollbar) == &tb_gui_scrollbar_kind ? tb_widget_data(scrollbar) : NULL;

    TB_ASSERT(first != NULL);
    TB_ASSERT(last != NULL);
    if (data == NULL)
    {
        return false;
    }
    *first = data->first;
    *last = data->last;
    return true;
}
