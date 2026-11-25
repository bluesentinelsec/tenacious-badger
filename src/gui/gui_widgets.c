/* The engine's kinds of widgets: frames, labels, and buttons, each a class
 * of functions and properties as a game's own kinds are. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

static const char *const tb_gui_justify_names[] = {"left", "center", "right"};
static const char *const tb_gui_anchor_names[] = {"center", "n",  "ne", "e", "se",
                                                  "s",      "sw", "w",  "nw"};

static tb_vec4 tb_gui_theme_color(tb_widget widget, tb_ui_color color)
{
    return tb_gui_theme(widget.gui)->colors[color];
}

static bool tb_gui_disabled(tb_widget widget)
{
    return (tb_widget_state(widget) & TB_WIDGET_DISABLED) != 0;
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_frame
{
    float padding;
    bool background;
    bool border;
} tb_gui_frame;

static bool tb_gui_frame_set_padding(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_frame *const frame = data;

    (void)widget;
    if (value->number < 0.0f)
    {
        return false;
    }
    frame->padding = value->number;
    return true;
}

static bool tb_gui_frame_get_padding(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_frame *const frame = data;

    (void)widget;
    *value = tb_gui_float(frame->padding);
    return true;
}

/* A frame that is filled stops the mouse; one that only lays others out
 * leaves it to the game. */
static bool tb_gui_frame_set_background(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_frame *const frame = data;

    frame->background = value->boolean;
    tb_widget_set_mouse_filter(widget, value->boolean ? TB_GUI_MOUSE_STOP : TB_GUI_MOUSE_PASS);
    return true;
}

static bool tb_gui_frame_get_background(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_frame *const frame = data;

    (void)widget;
    *value = tb_gui_bool(frame->background);
    return true;
}

static tb_gui_edges tb_gui_frame_insets(tb_widget widget, const void *data)
{
    const tb_gui_frame *const frame = data;
    const float edge = frame->padding + (frame->border ? tb_gui_theme(widget.gui)->border : 0.0f);

    return (tb_gui_edges){edge, edge, edge, edge};
}

static void tb_gui_frame_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_frame *const frame = data;
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);

    if (frame->background)
    {
        tb_gui_paint_fill(paint, rect, 0.0f, tb_gui_theme_color(widget, TB_UI_COLOR_WINDOW));
    }
    if (frame->border)
    {
        tb_gui_paint_outline(paint, rect, 0.0f, tb_gui_theme(widget.gui)->border,
                             tb_gui_theme_color(widget, TB_UI_COLOR_BORDER));
    }
}

static const tb_gui_property tb_gui_frame_properties[] = {
    {.name = "padding",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_frame_get_padding,
     .set = tb_gui_frame_set_padding},
    {.name = "background",
     .type = TB_GUI_TYPE_BOOL,
     .get = tb_gui_frame_get_background,
     .set = tb_gui_frame_set_background},
    {.name = "border",
     .type = TB_GUI_TYPE_BOOL,
     .offset = offsetof(tb_gui_frame, border),
     .flags = TB_GUI_PROPERTY_LAYOUT},
};

static const tb_widget_class tb_gui_frame_kind = {.name = "frame",
                                                  .data_size = sizeof(tb_gui_frame),
                                                  .properties = tb_gui_frame_properties,
                                                  .property_count =
                                                      TB_GUI_COUNT_OF(tb_gui_frame_properties),
                                                  .mouse_filter = TB_GUI_MOUSE_PASS,
                                                  .insets = tb_gui_frame_insets,
                                                  .draw = tb_gui_frame_draw};

const tb_widget_class *tb_frame_class(void)
{
    return &tb_gui_frame_kind;
}

tb_widget tb_frame(tb_widget parent, const tb_frame_desc *desc)
{
    const tb_frame_desc chosen = desc != NULL ? *desc : (tb_frame_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_frame_kind);
    const tb_gui_value padding = tb_gui_float(chosen.padding);
    const tb_gui_value background = tb_gui_bool(chosen.background);
    const tb_gui_value border = tb_gui_bool(chosen.border);

    (void)tb_widget_set(widget, "padding", &padding);
    (void)tb_widget_set(widget, "background", &background);
    (void)tb_widget_set(widget, "border", &border);
    return widget;
}

/* ---------------------------------------------------------------------------
 * Labels.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_label
{
    tb_gui_string text;
    float wrap_length;
    int32_t justify;
    int32_t anchor;
    tb_font_spec font;
} tb_gui_label;

static tb_gui_text_block tb_gui_label_block(const tb_gui_label *label)
{
    return (tb_gui_text_block){.text = tb_gui_string_text(&label->text),
                               .font = label->font,
                               .wrap_length = label->wrap_length,
                               .justify = (tb_font_align)label->justify,
                               .anchor = (tb_gui_anchor)label->anchor};
}

tb_vec2 tb_gui_measure_block(tb_widget widget, const tb_gui_text_block *block)
{
    tb_gui *const gui = widget.gui;
    const tb_gui_resolved_font resolved = tb_gui_resolve_font(gui, block->font);
    tb_ui_font_measures measures = {0};

    if (block->text.length > 0)
    {
        return tb_gui_measure_text(gui, block);
    }
    if (resolved.font == NULL)
    {
        return (tb_vec2){0};
    }
    tb_ui_font_measures_of(resolved.font, resolved.size, gui->view.scale, &measures);
    return (tb_vec2){0.0f, measures.line_height};
}

float tb_gui_line_height(tb_gui *gui)
{
    const tb_gui_resolved_font resolved = tb_gui_resolve_font(gui, (tb_font_spec){0});
    tb_ui_font_measures measures = {0};

    if (resolved.font == NULL)
    {
        return resolved.size;
    }
    tb_ui_font_measures_of(resolved.font, resolved.size, gui->view.scale, &measures);
    return measures.line_height;
}

static tb_vec2 tb_gui_label_measure(tb_widget widget, const void *data)
{
    const tb_gui_text_block block = tb_gui_label_block(data);
    return tb_gui_measure_block(widget, &block);
}

static void tb_gui_label_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_text_block block = tb_gui_label_block(data);
    const tb_ui_color color = tb_gui_disabled(widget) ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT;

    tb_gui_paint_text(paint, &block, tb_gui_paint_rect(paint), tb_gui_theme_color(widget, color));
}

static bool tb_gui_label_set_wrap(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_label *const label = data;

    (void)widget;
    if (value->number < 0.0f)
    {
        return false;
    }
    label->wrap_length = value->number;
    return true;
}

static bool tb_gui_label_get_wrap(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_label *const label = data;

    (void)widget;
    *value = tb_gui_float(label->wrap_length);
    return true;
}

static const tb_gui_property tb_gui_label_properties[] = {
    {.name = "text",
     .type = TB_GUI_TYPE_TEXT,
     .offset = offsetof(tb_gui_label, text),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "wrap_length",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_label_get_wrap,
     .set = tb_gui_label_set_wrap},
    {.name = "justify",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_label, justify),
     .names = tb_gui_justify_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_justify_names)},
    {.name = "anchor",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_label, anchor),
     .names = tb_gui_anchor_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_anchor_names)},
    {.name = "font",
     .type = TB_GUI_TYPE_FONT,
     .offset = offsetof(tb_gui_label, font),
     .flags = TB_GUI_PROPERTY_LAYOUT},
};

static const tb_widget_class tb_gui_label_kind = {.name = "label",
                                                  .data_size = sizeof(tb_gui_label),
                                                  .properties = tb_gui_label_properties,
                                                  .property_count =
                                                      TB_GUI_COUNT_OF(tb_gui_label_properties),
                                                  .mouse_filter = TB_GUI_MOUSE_PASS,
                                                  .measure = tb_gui_label_measure,
                                                  .draw = tb_gui_label_draw};

const tb_widget_class *tb_label_class(void)
{
    return &tb_gui_label_kind;
}

tb_widget tb_label(tb_widget parent, const tb_label_desc *desc)
{
    const tb_label_desc chosen = desc != NULL ? *desc : (tb_label_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_label_kind);
    const tb_gui_value values[] = {
        tb_gui_text(tb_text_from_cstr(chosen.text != NULL ? chosen.text : "")),
        tb_gui_float(chosen.wrap_length), tb_gui_enum((int32_t)chosen.justify),
        tb_gui_enum((int32_t)chosen.anchor), tb_gui_font(chosen.font)};
    static const char *const names[] = {"text", "wrap_length", "justify", "anchor", "font"};

    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    return widget;
}

/* ---------------------------------------------------------------------------
 * Buttons.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_button
{
    tb_gui_string text;
    tb_gui_command command;
    bool is_default;
} tb_gui_button;

static tb_gui_text_block tb_gui_button_block(const tb_gui_button *button)
{
    return (tb_gui_text_block){.text = tb_gui_string_text(&button->text),
                               .anchor = TB_GUI_ANCHOR_CENTER};
}

static tb_vec2 tb_gui_button_measure(tb_widget widget, const void *data)
{
    const tb_gui_text_block block = tb_gui_button_block(data);
    return tb_gui_measure_block(widget, &block);
}

static tb_gui_edges tb_gui_button_insets(tb_widget widget, const void *data)
{
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const float across = theme->frame_padding.x + theme->border;
    const float down = theme->frame_padding.y + theme->border;

    (void)data;
    return (tb_gui_edges){across, down, across, down};
}

static tb_ui_color tb_gui_button_fill(uint32_t state)
{
    const bool hovered = (state & TB_WIDGET_HOVERED) != 0;
    const bool pressed = (state & TB_WIDGET_PRESSED) != 0;

    if (pressed && hovered)
    {
        return TB_UI_COLOR_BUTTON_ACTIVE;
    }
    if (hovered || pressed)
    {
        return TB_UI_COLOR_BUTTON_HOT;
    }
    return TB_UI_COLOR_BUTTON;
}

static void tb_gui_button_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_button *const button = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const uint32_t state = tb_widget_state(widget);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_gui_text_block block = tb_gui_button_block(button);
    const bool disabled = (state & TB_WIDGET_DISABLED) != 0;
    const tb_ui_color text = disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT;

    tb_gui_paint_fill(paint, rect, theme->frame_rounding, theme->colors[tb_gui_button_fill(state)]);
    tb_gui_paint_outline(paint, rect, theme->frame_rounding, theme->border,
                         theme->colors[TB_UI_COLOR_BORDER]);
    if ((state & TB_WIDGET_FOCUSED) != 0)
    {
        tb_gui_paint_outline(paint, rect, theme->frame_rounding, 2.0f * theme->border,
                             theme->colors[TB_UI_COLOR_ACCENT]);
    }
    else if (button->is_default && !disabled)
    {
        tb_gui_paint_outline(paint, rect, theme->frame_rounding, theme->border,
                             theme->colors[TB_UI_COLOR_ACCENT]);
    }
    tb_gui_paint_text(paint, &block, rect, theme->colors[text]);
}

static bool tb_gui_button_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    const tb_gui_button *const button = data;

    switch (event->kind)
    {
    case TB_GUI_EVENT_CLICK:
        if (event->button == TB_MOUSE_BUTTON_LEFT)
        {
            (void)tb_widget_invoke(widget);
        }
        return true;
    case TB_GUI_EVENT_KEY:
        if ((event->key == TB_KEY_SPACE || event->key == TB_KEY_ENTER) && !event->repeat)
        {
            (void)tb_widget_invoke(widget);
            return true;
        }
        return false;
    case TB_GUI_EVENT_COMMAND:
        if (button->command.callback != NULL)
        {
            button->command.callback(event, button->command.user);
        }
        return true;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_LEAVE:
    case TB_GUI_EVENT_PRESS:
    case TB_GUI_EVENT_RELEASE:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_WHEEL:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_MOTION:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static const tb_gui_property tb_gui_button_properties[] = {
    {.name = "text",
     .type = TB_GUI_TYPE_TEXT,
     .offset = offsetof(tb_gui_button, text),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_button, command)},
    {.name = "default", .type = TB_GUI_TYPE_BOOL, .offset = offsetof(tb_gui_button, is_default)},
};

static const tb_widget_class tb_gui_button_kind = {.name = "button",
                                                   .data_size = sizeof(tb_gui_button),
                                                   .properties = tb_gui_button_properties,
                                                   .property_count =
                                                       TB_GUI_COUNT_OF(tb_gui_button_properties),
                                                   .flags = TB_WIDGET_CLASS_FOCUSABLE,
                                                   .mouse_filter = TB_GUI_MOUSE_STOP,
                                                   .measure = tb_gui_button_measure,
                                                   .insets = tb_gui_button_insets,
                                                   .draw = tb_gui_button_draw,
                                                   .event = tb_gui_button_event};

const tb_widget_class *tb_button_class(void)
{
    return &tb_gui_button_kind;
}

tb_widget tb_button(tb_widget parent, const tb_button_desc *desc)
{
    const tb_button_desc chosen = desc != NULL ? *desc : (tb_button_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_button_kind);
    const tb_gui_value text =
        tb_gui_text(tb_text_from_cstr(chosen.text != NULL ? chosen.text : ""));
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);
    const tb_gui_value is_default = tb_gui_bool(chosen.is_default);

    (void)tb_widget_set(widget, "text", &text);
    (void)tb_widget_set(widget, "command", &command);
    (void)tb_widget_set(widget, "default", &is_default);
    return widget;
}

void tb_gui_register_builtins(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_frame_kind);
    (void)tb_gui_register_class(gui, &tb_gui_label_kind);
    (void)tb_gui_register_class(gui, &tb_gui_button_kind);
    tb_gui_register_scroll_kinds(gui);
    tb_gui_register_listbox(gui);
    tb_gui_register_menu_kinds(gui);
    tb_gui_register_tooltip(gui);
    tb_gui_register_treeview(gui);
    tb_gui_register_notebook(gui);
    tb_gui_register_panedwindow(gui);
    tb_gui_register_message_box(gui);
    tb_gui_register_image(gui);
    tb_gui_register_canvas(gui);
}
