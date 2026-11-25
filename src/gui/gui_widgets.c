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

/* The states a kind's look follows. */
#define TB_GUI_DRAWN_STATES                                                                        \
    (TB_WIDGET_HOVERED | TB_WIDGET_PRESSED | TB_WIDGET_FOCUSED | TB_WIDGET_DISABLED)

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

/* A frame's padding, and its style's padding while it draws its
 * background, and its border's width while it draws its border: a frame
 * that only lays others out takes no room of its style's. */
static tb_gui_edges tb_gui_frame_insets(tb_widget widget, const void *data)
{
    const tb_gui_frame *const frame = data;
    const tb_style *const style = tb_style_in(widget, 0);
    const float border = frame->border ? style->border_width : 0.0f;
    const tb_gui_edges padding = frame->background ? style->padding : (tb_gui_edges){0};
    const float edge = frame->padding + border;

    return (tb_gui_edges){edge + padding.left, edge + padding.top, edge + padding.right,
                          edge + padding.bottom};
}

static void tb_gui_frame_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_frame *const frame = data;
    const tb_style *const style = tb_style_of(widget);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);

    if (frame->background && style->image.slice.image != NULL)
    {
        tb_gui_paint_nine_slice(paint, &style->image.slice, rect, style->image.tint);
    }
    else if (frame->background)
    {
        tb_gui_paint_fill(paint, rect, style->radius, style->background);
    }
    if (frame->border)
    {
        tb_gui_paint_outline(paint, rect, style->radius, style->border_width,
                             style->border_color);
    }
}

/* A frame's style's elements: the root's shade is what modal widgets are
 * shown over. */
static const tb_style_element tb_gui_frame_elements[] = {{"shade", TB_STYLE_COLOR}};

static const tb_widget_style_desc tb_gui_frame_style = {
    .name = "Frame",
    .states = TB_WIDGET_DISABLED,
    .elements = tb_gui_frame_elements,
    .element_count = TB_GUI_COUNT_OF(tb_gui_frame_elements)};

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
                                                  .draw = tb_gui_frame_draw,
                                                  .style = &tb_gui_frame_style};

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
    const tb_gui_resolved_font resolved = tb_gui_widget_font(widget, block->font);
    tb_ui_font_measures measures = {0};

    if (block->text.length > 0)
    {
        return tb_widget_measure_text(widget, block);
    }
    if (resolved.font == NULL)
    {
        return (tb_vec2){0};
    }
    tb_ui_font_measures_of(resolved.font, resolved.size, gui->view.scale, &measures);
    return (tb_vec2){0.0f, measures.line_height};
}

float tb_gui_line_height(tb_widget widget)
{
    const tb_gui_resolved_font resolved = tb_gui_widget_font(widget, (tb_font_spec){0});
    tb_ui_font_measures measures = {0};

    if (resolved.font == NULL)
    {
        return resolved.size;
    }
    tb_ui_font_measures_of(resolved.font, resolved.size, widget.gui->view.scale, &measures);
    return measures.line_height;
}

static tb_vec2 tb_gui_label_measure(tb_widget widget, const void *data)
{
    const tb_gui_text_block block = tb_gui_label_block(data);
    return tb_gui_measure_block(widget, &block);
}

/* A label's style's padding, inside its box. */
static tb_gui_edges tb_gui_label_insets(tb_widget widget, const void *data)
{
    const tb_style *const style = tb_style_in(widget, 0);
    const float border = style->border_width;

    (void)data;
    return (tb_gui_edges){style->padding.left + border, style->padding.top + border,
                          style->padding.right + border, style->padding.bottom + border};
}

static void tb_gui_label_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_text_block block = tb_gui_label_block(data);
    const tb_style *const style = tb_style_of(widget);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_gui_edges insets = tb_gui_label_insets(widget, data);

    tb_gui_paint_box(paint, rect, style);
    tb_gui_paint_text(paint, &block,
                      (tb_canvas_rect){rect.x + insets.left, rect.y + insets.top,
                                       rect.width - insets.left - insets.right,
                                       rect.height - insets.top - insets.bottom},
                      style->text_color);
}

static const tb_widget_style_desc tb_gui_label_style = {.name = "Label",
                                                        .states = TB_WIDGET_DISABLED};

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
                                                  .insets = tb_gui_label_insets,
                                                  .draw = tb_gui_label_draw,
                                                  .style = &tb_gui_label_style};

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

/* A button's style's elements. */
enum
{
    TB_GUI_BUTTON_DEFAULT_COLOR, /* the ring of a dialog's default button */
    TB_GUI_BUTTON_ELEMENTS
};

static const tb_style_element tb_gui_button_elements[TB_GUI_BUTTON_ELEMENTS] = {
    [TB_GUI_BUTTON_DEFAULT_COLOR] = {"default_color", TB_STYLE_COLOR}};

static const tb_widget_style_desc tb_gui_button_style = {.name = "Button",
                                                         .states = TB_GUI_DRAWN_STATES,
                                                         .elements = tb_gui_button_elements,
                                                         .element_count = TB_GUI_BUTTON_ELEMENTS};

static tb_gui_edges tb_gui_button_insets(tb_widget widget, const void *data)
{
    const tb_style *const style = tb_style_in(widget, 0);
    const float border = style->border_width;

    (void)data;
    return (tb_gui_edges){style->padding.left + border, style->padding.top + border,
                          style->padding.right + border, style->padding.bottom + border};
}

static void tb_gui_button_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_button *const button = data;
    const uint32_t state = tb_widget_state(widget);
    const tb_style *const style = tb_style_in(widget, state);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_gui_text_block block = tb_gui_button_block(button);
    const bool plain = (state & (TB_WIDGET_FOCUSED | TB_WIDGET_DISABLED)) == 0;

    tb_gui_paint_box(paint, rect, style);
    /* A dialog's default button is ringed thinly, but for the focus's ring. */
    if (button->is_default && plain)
    {
        tb_gui_paint_outline(paint, rect, style->radius, style->border_width,
                             tb_style_color(style, TB_GUI_BUTTON_DEFAULT_COLOR));
    }
    tb_gui_paint_text(paint, &block, rect, style->text_color);
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
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_MOTION:
    case TB_GUI_EVENT_VALIDATE:
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
                                                   .event = tb_gui_button_event,
                                                   .style = &tb_gui_button_style};

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
    tb_gui_register_value_kinds(gui);
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
