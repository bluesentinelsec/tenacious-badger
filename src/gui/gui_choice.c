/* Check buttons and radio buttons: a box ticked and cleared, and one of a
 * group chosen, each showing and setting a variable by its values as text,
 * as Tk's do. */
#include "gui_values.h"

#include "badger/assert.h"

#include <stddef.h>

/* An indicator's side, in lines of text; its tick's points, in sides. */
#define TB_GUI_INDICATOR_LINES 0.8f
#define TB_GUI_TICK_LEFT 0.22f
#define TB_GUI_TICK_MIDDLE_X 0.42f
#define TB_GUI_TICK_MIDDLE_Y 0.7f
#define TB_GUI_TICK_RIGHT 0.78f
#define TB_GUI_TICK_TOP 0.3f
#define TB_GUI_TICK_LEFT_Y 0.5f
/* A tick's thickness, and a radio button's dot, in sides. */
#define TB_GUI_TICK_THICKNESS 0.14f
#define TB_GUI_DOT 0.5f

/* A check button and a radio button. */
typedef struct tb_gui_choice
{
    tb_gui_string text;
    tb_var variable;
    tb_gui_string on_value; /* a radio button's value */
    tb_gui_string off_value;
    bool selected; /* without a variable */
    tb_gui_command command;
} tb_gui_choice;

/* Whether it is ticked, or chosen. */
static bool tb_gui_choice_selected(const tb_gui_choice *choice)
{
    if (tb_var_alive(choice->variable))
    {
        return tb_gui_var_holds(choice->variable, tb_gui_string_text(&choice->on_value));
    }
    return choice->selected;
}

/* Set its state as a program does: its variable, or its own. */
static void tb_gui_choice_set(tb_gui_choice *choice, bool selected, bool radio)
{
    if (!tb_var_alive(choice->variable))
    {
        choice->selected = selected;
        return;
    }
    if (radio && !selected)
    {
        /* A radio button cleared leaves its group's variable to the others. */
        return;
    }
    (void)tb_gui_var_set_span(
        choice->variable, tb_gui_string_text(selected ? &choice->on_value : &choice->off_value));
}

/* ---------------------------------------------------------------------------
 * Measures and drawing.
 * ------------------------------------------------------------------------- */

/* The elements of a check button's and a radio button's styles. Their box
 * is their indicator's. */
enum
{
    TB_GUI_CHOICE_MARK,    /* a check button's tick, a radio button's dot */
    TB_GUI_CHOICE_SPACING, /* units between the indicator and the text */
    TB_GUI_CHOICE_ELEMENTS
};

static const tb_style_element tb_gui_choice_elements[TB_GUI_CHOICE_ELEMENTS] = {
    [TB_GUI_CHOICE_MARK] = {"mark_color", TB_STYLE_COLOR},
    [TB_GUI_CHOICE_SPACING] = {"spacing", TB_STYLE_NUMBER}};

static const tb_widget_style_desc tb_gui_checkbutton_style = {
    .name = "Checkbutton",
    .states = TB_GUI_VALUES_STATES | TB_WIDGET_SELECTED,
    .elements = tb_gui_choice_elements,
    .element_count = TB_GUI_CHOICE_ELEMENTS};

static const tb_widget_style_desc tb_gui_radiobutton_style = {
    .name = "Radiobutton",
    .states = TB_GUI_VALUES_STATES | TB_WIDGET_SELECTED,
    .elements = tb_gui_choice_elements,
    .element_count = TB_GUI_CHOICE_ELEMENTS};

static float tb_gui_indicator_side(tb_widget widget)
{
    return tb_gui_values_measures(widget).line_height * TB_GUI_INDICATOR_LINES;
}

/* The gap between an indicator and its text. */
static float tb_gui_indicator_gap(tb_widget widget)
{
    return tb_style_number(tb_style_in(widget, 0), TB_GUI_CHOICE_SPACING);
}

static tb_vec2 tb_gui_choice_measure(tb_widget widget, const void *data)
{
    const tb_gui_choice *const choice = data;
    const tb_gui_text_block block = {.text = tb_gui_string_text(&choice->text)};
    const float side = tb_gui_indicator_side(widget);
    const float line = tb_gui_values_measures(widget).line_height;
    const tb_vec2 text = tb_widget_measure_text(widget, &block);
    const float gap = text.x > 0.0f ? tb_gui_indicator_gap(widget) : 0.0f;

    return (tb_vec2){side + gap + text.x, text.y > line ? text.y : line};
}

/* The indicator's square: at the left, in the middle of the height. */
static tb_canvas_rect tb_gui_indicator_rect(tb_widget widget, tb_canvas_rect rect)
{
    const float side = tb_gui_indicator_side(widget);

    return (tb_canvas_rect){rect.x, rect.y + (rect.height - side) * 0.5f, side, side};
}

static void tb_gui_choice_draw_text(tb_widget widget, const tb_gui_choice *choice,
                                    tb_gui_paint *paint, tb_canvas_rect rect, const tb_style *style)
{
    const float offset = tb_gui_indicator_side(widget) + tb_gui_indicator_gap(widget);
    const tb_gui_text_block block = {.text = tb_gui_string_text(&choice->text),
                                     .anchor = TB_GUI_ANCHOR_W};
    const tb_canvas_rect text = {rect.x + offset, rect.y, rect.width - offset, rect.height};

    tb_gui_paint_text(paint, &block, text, style->text_color);
}

/* A choice's style in its state, chosen or not. */
static const tb_style *tb_gui_choice_style(tb_widget widget, const tb_gui_choice *choice)
{
    const uint32_t selected = tb_gui_choice_selected(choice) ? TB_WIDGET_SELECTED : 0u;
    return tb_style_in(widget, tb_widget_state(widget) | selected);
}

static void tb_gui_checkbutton_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_choice *const choice = data;
    const tb_style *const style = tb_gui_choice_style(widget, choice);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_canvas_rect box = tb_gui_indicator_rect(widget, rect);

    tb_gui_paint_box(paint, box, style);
    if (tb_gui_choice_selected(choice))
    {
        const float side = box.width;
        const tb_vec2 left = {box.x + side * TB_GUI_TICK_LEFT, box.y + side * TB_GUI_TICK_LEFT_Y};
        const tb_vec2 middle = {box.x + side * TB_GUI_TICK_MIDDLE_X,
                                box.y + side * TB_GUI_TICK_MIDDLE_Y};
        const tb_vec2 right = {box.x + side * TB_GUI_TICK_RIGHT, box.y + side * TB_GUI_TICK_TOP};
        const tb_vec4 mark = tb_style_color(style, TB_GUI_CHOICE_MARK);
        tb_gui_paint_line(paint, left, middle, side * TB_GUI_TICK_THICKNESS, mark);
        tb_gui_paint_line(paint, middle, right, side * TB_GUI_TICK_THICKNESS, mark);
    }
    tb_gui_choice_draw_text(widget, choice, paint, rect, style);
}

static void tb_gui_radiobutton_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_choice *const choice = data;
    const tb_style *const style = tb_gui_choice_style(widget, choice);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_canvas_rect circle = tb_gui_indicator_rect(widget, rect);
    tb_style round = *style;

    /* A radio button's indicator is a circle, whatever its corners. */
    round.radius = circle.width * 0.5f;
    tb_gui_paint_box(paint, circle, &round);
    if (tb_gui_choice_selected(choice))
    {
        const float dot = circle.width * TB_GUI_DOT;
        const tb_canvas_rect inner = {circle.x + (circle.width - dot) * 0.5f,
                                      circle.y + (circle.height - dot) * 0.5f, dot, dot};
        tb_gui_paint_fill(paint, inner, dot * 0.5f, tb_style_color(style, TB_GUI_CHOICE_MARK));
    }
    tb_gui_choice_draw_text(widget, choice, paint, rect, style);
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

/* The radio button before or after one among its parent's children of its
 * variable; none when there is none. */
static tb_widget tb_gui_radio_neighbour(tb_widget widget, const tb_gui_choice *choice, bool after)
{
    tb_widget before = TB_WIDGET_NONE;
    bool passed = false;

    for (tb_widget child = tb_widget_first_child(tb_widget_parent(widget)); tb_widget_alive(child);
         child = tb_widget_next_sibling(child))
    {
        if (tb_widget_equal(child, widget))
        {
            if (!after)
            {
                return before;
            }
            passed = true;
            continue;
        }
        const tb_gui_choice *const other = tb_widget_data(child);
        const bool same = tb_widget_class_of(child) == tb_radiobutton_class() &&
                          tb_var_equal(other->variable, choice->variable) &&
                          (tb_widget_state(child) & TB_WIDGET_DISABLED) == 0;
        if (!same)
        {
            continue;
        }
        if (passed)
        {
            return child;
        }
        before = child;
    }
    return TB_WIDGET_NONE;
}

/* Move a radio button group's choice with an arrow. */
static bool tb_gui_radio_arrow(tb_widget widget, const tb_gui_choice *choice, tb_key key)
{
    const bool after = key == TB_KEY_DOWN || key == TB_KEY_RIGHT;
    tb_widget next = TB_WIDGET_NONE;

    if (!tb_var_alive(choice->variable))
    {
        return false;
    }
    next = tb_gui_radio_neighbour(widget, choice, after);
    if (!tb_widget_alive(next))
    {
        return false;
    }
    (void)tb_widget_focus(next);
    (void)tb_widget_invoke(next);
    return true;
}

static bool tb_gui_choice_event(tb_widget widget, tb_gui_choice *choice, const tb_gui_event *event,
                                bool radio)
{
    switch (event->kind)
    {
    case TB_GUI_EVENT_CLICK:
        if (event->button == TB_MOUSE_BUTTON_LEFT)
        {
            (void)tb_widget_invoke(widget);
        }
        return true;
    case TB_GUI_EVENT_KEY:
        if ((event->key == TB_KEY_SPACE || event->key == TB_KEY_ENTER) && !event->repeat &&
            tb_gui_values_plain(event))
        {
            (void)tb_widget_invoke(widget);
            return true;
        }
        if (radio && tb_gui_values_plain(event) &&
            (event->key == TB_KEY_UP || event->key == TB_KEY_DOWN || event->key == TB_KEY_LEFT ||
             event->key == TB_KEY_RIGHT))
        {
            return tb_gui_radio_arrow(widget, choice, event->key);
        }
        return false;
    case TB_GUI_EVENT_COMMAND: {
        const bool was = tb_gui_choice_selected(choice);
        if (radio && was)
        {
            return true;
        }
        tb_gui_choice_set(choice, radio ? true : !was, radio);
        if (tb_gui_choice_selected(choice) != was)
        {
            tb_gui_values_changed(widget, choice->command);
        }
        return true;
    }
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
    case TB_GUI_EVENT_BACK:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static bool tb_gui_checkbutton_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    return tb_gui_choice_event(widget, data, event, false);
}

static bool tb_gui_radiobutton_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    return tb_gui_choice_event(widget, data, event, true);
}

/* ---------------------------------------------------------------------------
 * Properties.
 * ------------------------------------------------------------------------- */

static bool tb_gui_choice_get_variable(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_choice *const choice = data;

    (void)widget;
    *value = tb_gui_variable(choice->variable);
    return true;
}

static bool tb_gui_choice_set_variable(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_choice *const choice = data;

    if (!tb_gui_var_of_widget(widget, value->variable))
    {
        return false;
    }
    choice->variable = value->variable;
    return true;
}

static bool tb_gui_choice_get_selected(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    *value = tb_gui_bool(tb_gui_choice_selected(data));
    return true;
}

static bool tb_gui_checkbutton_set_selected(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)widget;
    tb_gui_choice_set(data, value->boolean, false);
    return true;
}

static bool tb_gui_radiobutton_set_selected(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)widget;
    tb_gui_choice_set(data, value->boolean, true);
    return true;
}

#define TB_GUI_CHOICE_TEXT                                                                         \
    {.name = "text",                                                                               \
     .type = TB_GUI_TYPE_TEXT,                                                                     \
     .offset = offsetof(tb_gui_choice, text),                                                      \
     .flags = TB_GUI_PROPERTY_LAYOUT}
#define TB_GUI_CHOICE_VARIABLE                                                                     \
    {.name = "variable",                                                                           \
     .type = TB_GUI_TYPE_VARIABLE,                                                                 \
     .get = tb_gui_choice_get_variable,                                                            \
     .set = tb_gui_choice_set_variable}
#define TB_GUI_CHOICE_COMMAND                                                                      \
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_choice, command)}

static const tb_gui_property tb_gui_checkbutton_properties[] = {
    TB_GUI_CHOICE_TEXT,
    TB_GUI_CHOICE_VARIABLE,
    {.name = "on_value", .type = TB_GUI_TYPE_TEXT, .offset = offsetof(tb_gui_choice, on_value)},
    {.name = "off_value", .type = TB_GUI_TYPE_TEXT, .offset = offsetof(tb_gui_choice, off_value)},
    {.name = "selected",
     .type = TB_GUI_TYPE_BOOL,
     .get = tb_gui_choice_get_selected,
     .set = tb_gui_checkbutton_set_selected},
    TB_GUI_CHOICE_COMMAND,
};

static const tb_gui_property tb_gui_radiobutton_properties[] = {
    TB_GUI_CHOICE_TEXT,
    TB_GUI_CHOICE_VARIABLE,
    {.name = "value", .type = TB_GUI_TYPE_TEXT, .offset = offsetof(tb_gui_choice, on_value)},
    {.name = "selected",
     .type = TB_GUI_TYPE_BOOL,
     .get = tb_gui_choice_get_selected,
     .set = tb_gui_radiobutton_set_selected},
    TB_GUI_CHOICE_COMMAND,
};

static void tb_gui_checkbutton_init(tb_widget widget, void *data)
{
    tb_gui_choice *const choice = data;

    (void)widget;
    tb_gui_string_set(&choice->on_value, tb_text_from_cstr("1"));
    tb_gui_string_set(&choice->off_value, tb_text_from_cstr("0"));
}

static const tb_widget_class tb_gui_checkbutton_kind = {
    .name = "checkbutton",
    .data_size = sizeof(tb_gui_choice),
    .properties = tb_gui_checkbutton_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_checkbutton_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_checkbutton_init,
    .measure = tb_gui_choice_measure,
    .draw = tb_gui_checkbutton_draw,
    .event = tb_gui_checkbutton_event,
    .style = &tb_gui_checkbutton_style};

static const tb_widget_class tb_gui_radiobutton_kind = {
    .name = "radiobutton",
    .data_size = sizeof(tb_gui_choice),
    .properties = tb_gui_radiobutton_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_radiobutton_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .measure = tb_gui_choice_measure,
    .draw = tb_gui_radiobutton_draw,
    .event = tb_gui_radiobutton_event,
    .style = &tb_gui_radiobutton_style};

const tb_widget_class *tb_checkbutton_class(void)
{
    return &tb_gui_checkbutton_kind;
}

const tb_widget_class *tb_radiobutton_class(void)
{
    return &tb_gui_radiobutton_kind;
}

static void tb_gui_choice_set_text(tb_widget widget, const char *name, const char *text)
{
    const tb_gui_value value = tb_gui_text(tb_text_from_cstr(text));
    (void)tb_widget_set(widget, name, &value);
}

tb_widget tb_checkbutton(tb_widget parent, const tb_checkbutton_desc *desc)
{
    const tb_checkbutton_desc chosen = desc != NULL ? *desc : (tb_checkbutton_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_checkbutton_kind);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);

    tb_gui_choice_set_text(widget, "text", chosen.text != NULL ? chosen.text : "");
    tb_gui_choice_set_text(widget, "on_value", chosen.on_value != NULL ? chosen.on_value : "1");
    tb_gui_choice_set_text(widget, "off_value", chosen.off_value != NULL ? chosen.off_value : "0");
    (void)tb_widget_set(widget, "variable", &variable);
    (void)tb_widget_set(widget, "command", &command);
    return widget;
}

tb_widget tb_radiobutton(tb_widget parent, const tb_radiobutton_desc *desc)
{
    const tb_radiobutton_desc chosen = desc != NULL ? *desc : (tb_radiobutton_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_radiobutton_kind);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);

    tb_gui_choice_set_text(widget, "text", chosen.text != NULL ? chosen.text : "");
    tb_gui_choice_set_text(widget, "value", chosen.value != NULL ? chosen.value : "");
    (void)tb_widget_set(widget, "variable", &variable);
    (void)tb_widget_set(widget, "command", &command);
    return widget;
}
