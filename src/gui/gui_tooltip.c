/* Tooltips: text a widget is given, shown as a passive popup below the
 * mouse once the mouse has rested on the widget for a while, and hidden
 * when it moves to another widget, a button is pressed, or a key goes
 * down. A tooltip is a child of the widget it is for, never in its grid. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Units below the mouse a tooltip is shown, clear of the pointer. */
#define TB_GUI_TOOLTIP_DROP 20.0f
/* Ems a tooltip's text wraps at. */
#define TB_GUI_TOOLTIP_WRAP_EMS 24.0f

typedef struct tb_gui_tooltip
{
    tb_gui_string text;
    float delay;
} tb_gui_tooltip;

static const tb_widget_style_desc tb_gui_tooltip_style = {.name = "Tooltip"};

static tb_gui_text_block tb_gui_tooltip_block(tb_widget widget, const tb_gui_tooltip *tooltip)
{
    const float em = tb_gui_widget_font(widget, (tb_font_spec){0}).size;

    return (tb_gui_text_block){.text = tb_gui_string_text(&tooltip->text),
                               .wrap_length = em * TB_GUI_TOOLTIP_WRAP_EMS,
                               .anchor = TB_GUI_ANCHOR_NW};
}

static tb_vec2 tb_gui_tooltip_measure(tb_widget widget, const void *data)
{
    const tb_gui_text_block block = tb_gui_tooltip_block(widget, data);
    return tb_gui_measure_block(widget, &block);
}

static tb_gui_edges tb_gui_tooltip_insets(tb_widget widget, const void *data)
{
    const tb_style *const style = tb_style_in(widget, 0);
    const float border = style->border_width;

    (void)data;
    return (tb_gui_edges){style->padding.left + border, style->padding.top + border,
                          style->padding.right + border, style->padding.bottom + border};
}

static void tb_gui_tooltip_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_text_block block = tb_gui_tooltip_block(widget, data);
    const tb_style *const style = tb_style_of(widget);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_gui_edges insets = tb_gui_tooltip_insets(widget, data);
    const tb_canvas_rect inner = {rect.x + insets.left, rect.y + insets.top,
                                  rect.width - insets.left - insets.right,
                                  rect.height - insets.top - insets.bottom};

    tb_gui_paint_box(paint, rect, style);
    tb_gui_paint_text(paint, &block, inner, style->text_color);
}

static bool tb_gui_tooltip_set_delay(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_tooltip *const tooltip = data;

    (void)widget;
    if (value->number < 0.0f)
    {
        return false;
    }
    tooltip->delay = value->number;
    return true;
}

static bool tb_gui_tooltip_get_delay(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_tooltip *const tooltip = data;

    (void)widget;
    *value = tb_gui_float(tooltip->delay);
    return true;
}

static const tb_gui_property tb_gui_tooltip_properties[] = {
    {.name = "text",
     .type = TB_GUI_TYPE_TEXT,
     .offset = offsetof(tb_gui_tooltip, text),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "delay",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_tooltip_get_delay,
     .set = tb_gui_tooltip_set_delay},
};

static const tb_widget_class tb_gui_tooltip_kind = {.name = "tooltip",
                                                    .data_size = sizeof(tb_gui_tooltip),
                                                    .properties = tb_gui_tooltip_properties,
                                                    .property_count =
                                                        TB_GUI_COUNT_OF(tb_gui_tooltip_properties),
                                                    .mouse_filter = TB_GUI_MOUSE_IGNORE,
                                                    .measure = tb_gui_tooltip_measure,
                                                    .insets = tb_gui_tooltip_insets,
                                                    .draw = tb_gui_tooltip_draw,
                                                    .style = &tb_gui_tooltip_style};

const tb_widget_class *tb_tooltip_class(void)
{
    return &tb_gui_tooltip_kind;
}

tb_widget tb_tooltip(tb_widget widget, const tb_tooltip_desc *desc)
{
    const tb_tooltip_desc chosen = desc != NULL ? *desc : (tb_tooltip_desc){0};
    const tb_widget tooltip = tb_widget_create(widget, &tb_gui_tooltip_kind);
    const tb_gui_value text =
        tb_gui_text(tb_text_from_cstr(chosen.text != NULL ? chosen.text : ""));
    const tb_gui_value delay = tb_gui_float(chosen.delay);

    (void)tb_widget_set(tooltip, "text", &text);
    (void)tb_widget_set(tooltip, "delay", &delay);
    return tooltip;
}

/* The tooltip of the widget under the mouse, or of its nearest ancestor
 * that has one with text; none when there is none. */
static tb_widget tb_gui_tooltip_for(tb_gui *gui, tb_widget hovered)
{
    for (uint32_t at = hovered.index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        for (uint32_t child = tb_gui_node_at(gui, at)->first_child; child != TB_GUI_NO_NODE;
             child = tb_gui_node_at(gui, child)->next)
        {
            const tb_gui_node *const node = tb_gui_node_at(gui, child);
            const tb_gui_tooltip *const tooltip = node->data;
            if (node->kind == &tb_gui_tooltip_kind && node->visible && tooltip->text.length > 0)
            {
                return tb_gui_handle(gui, child);
            }
        }
    }
    return TB_WIDGET_NONE;
}

void tb_gui_update_tooltips(tb_gui *gui)
{
    const bool resting = gui->mouse_inside && !gui->tooltip_spent &&
                         tb_widget_alive(gui->hovered) && !tb_widget_alive(gui->pressed);
    const tb_widget found = resting ? tb_gui_tooltip_for(gui, gui->hovered) : TB_WIDGET_NONE;

    if (tb_widget_alive(gui->tooltip) && !tb_widget_equal(found, gui->tooltip))
    {
        const tb_widget shown = gui->tooltip;
        gui->tooltip = TB_WIDGET_NONE;
        tb_widget_close_popup(shown);
    }
    if (!tb_widget_alive(found) || tb_widget_alive(gui->tooltip))
    {
        return;
    }
    const tb_gui_tooltip *const tooltip = tb_widget_data(found);
    const double delay =
        (double)(tooltip->delay > 0.0f ? tooltip->delay : gui->theme.tooltip_delay);
    if (gui->time - gui->hover_time < delay)
    {
        return;
    }
    gui->tooltip = found;
    (void)tb_widget_popup(
        found, &(tb_gui_popup_options){
                   .anchor = {.x = gui->mouse.x, .y = gui->mouse.y + TB_GUI_TOOLTIP_DROP},
                   .placement = TB_GUI_POPUP_AT,
                   .passive = true});
}

void tb_gui_register_tooltip(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_tooltip_kind);
}
