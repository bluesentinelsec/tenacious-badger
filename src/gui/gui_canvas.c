/* The canvas kind: a widget whose content the caller draws, each update,
 * through a callback given the paint functions, clipped to the widget; its
 * input comes by bindings, after Tk's `canvas` drawn by hand. It takes the
 * focus on a click, so that a viewport has its keys. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

typedef struct tb_gui_canvas
{
    float width;
    float height;
    bool background;
    tb_canvas_widget_draw *draw;
    void *user;
} tb_gui_canvas;

static tb_gui_canvas *tb_gui_canvas_of(tb_widget widget);

static tb_vec2 tb_gui_canvas_measure(tb_widget widget, const void *data)
{
    const tb_gui_canvas *const canvas = data;

    (void)widget;
    return (tb_vec2){canvas->width, canvas->height};
}

/* A canvas's style: its box, drawn when it has a background. */
static const tb_widget_style_desc tb_gui_canvas_style = {
    .name = "Canvas", .states = TB_WIDGET_FOCUSED | TB_WIDGET_DISABLED};

static void tb_gui_canvas_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_canvas *const canvas = data;

    if (canvas->background)
    {
        tb_gui_paint_box(paint, tb_gui_paint_rect(paint), tb_style_of(widget));
    }
    if (canvas->draw != NULL)
    {
        canvas->draw(widget, paint, canvas->user);
    }
}

static bool tb_gui_canvas_set_size(float *slot, const tb_gui_value *value)
{
    if (value->number < 0.0f)
    {
        return false;
    }
    *slot = value->number;
    return true;
}

static bool tb_gui_canvas_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_canvas *const canvas = data;

    (void)widget;
    return tb_gui_canvas_set_size(&canvas->width, value);
}

static bool tb_gui_canvas_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_canvas *const canvas = data;

    (void)widget;
    *value = tb_gui_float(canvas->width);
    return true;
}

static bool tb_gui_canvas_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_canvas *const canvas = data;

    (void)widget;
    return tb_gui_canvas_set_size(&canvas->height, value);
}

static bool tb_gui_canvas_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_canvas *const canvas = data;

    (void)widget;
    *value = tb_gui_float(canvas->height);
    return true;
}

static const tb_gui_property tb_gui_canvas_properties[] = {
    {.name = "width",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_canvas_get_width,
     .set = tb_gui_canvas_set_width},
    {.name = "height",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_canvas_get_height,
     .set = tb_gui_canvas_set_height},
    {.name = "background", .type = TB_GUI_TYPE_BOOL, .offset = offsetof(tb_gui_canvas, background)},
};

static const tb_widget_class tb_gui_canvas_kind = {.name = "canvas",
                                                   .data_size = sizeof(tb_gui_canvas),
                                                   .properties = tb_gui_canvas_properties,
                                                   .property_count =
                                                       TB_GUI_COUNT_OF(tb_gui_canvas_properties),
                                                   .flags = TB_WIDGET_CLASS_FOCUS_ON_CLICK,
                                                   .mouse_filter = TB_GUI_MOUSE_STOP,
                                                   .measure = tb_gui_canvas_measure,
                                                   .draw = tb_gui_canvas_draw,
                                                   .style = &tb_gui_canvas_style};

static tb_gui_canvas *tb_gui_canvas_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_canvas_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_canvas_widget_class(void)
{
    return &tb_gui_canvas_kind;
}

tb_widget tb_canvas_widget(tb_widget parent, const tb_canvas_widget_desc *desc)
{
    const tb_canvas_widget_desc chosen = desc != NULL ? *desc : (tb_canvas_widget_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_canvas_kind);
    const tb_gui_value values[] = {tb_gui_float(chosen.width), tb_gui_float(chosen.height),
                                   tb_gui_bool(chosen.background)};
    static const char *const names[] = {"width", "height", "background"};

    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    (void)tb_canvas_widget_set_draw(widget, chosen.draw, chosen.user);
    return widget;
}

bool tb_canvas_widget_set_draw(tb_widget widget, tb_canvas_widget_draw *draw, void *user)
{
    tb_gui_canvas *const canvas = tb_gui_canvas_of(widget);

    if (canvas == NULL)
    {
        return false;
    }
    canvas->draw = draw;
    canvas->user = user;
    return true;
}

void tb_gui_register_canvas(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_canvas_kind);
}
