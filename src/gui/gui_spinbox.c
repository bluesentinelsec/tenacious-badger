/* Spin boxes: a number typed in a field (gui_field.c), taken when Enter is
 * pressed or the focus leaves, or stepped with arrows, keys, and the wheel,
 * within a range, in a variable or its own. */
#include "gui_values.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

/* Characters of "0" its text is wide unless chosen; its range unless
 * chosen; steps Page Up and Page Down take. */
#define TB_GUI_SPINBOX_WIDTH 8
#define TB_GUI_SPINBOX_DEFAULT_TO 100.0f
#define TB_GUI_SPINBOX_PAGE 10.0
/* An arrow's chevron, in its half of the column: its width and height. */
#define TB_GUI_CHEVRON_WIDTH 0.36f
#define TB_GUI_CHEVRON_HEIGHT 0.2f

/* No arrow held. */
#define TB_GUI_NO_ARROW 0
#define TB_GUI_ARROW_UP 1
#define TB_GUI_ARROW_DOWN (-1)

typedef struct tb_gui_spinbox
{
    tb_gui_field field;
    tb_var variable;
    float from;
    float to;
    float increment;
    float value;
    tb_gui_string format;
    int32_t width;
    bool wrap;
    tb_gui_command command;
    int32_t held; /* the arrow held: TB_GUI_ARROW_UP, _DOWN, or none */
} tb_gui_spinbox;

TB_STATIC_ASSERT(offsetof(tb_gui_spinbox, field) == 0, "a field is first in its kind's data");

static double tb_gui_spinbox_value(const tb_gui_spinbox *box)
{
    return tb_var_alive(box->variable) ? tb_var_get_float(box->variable) : (double)box->value;
}

static double tb_gui_spinbox_low(const tb_gui_spinbox *box)
{
    return fmin((double)box->from, (double)box->to);
}

static double tb_gui_spinbox_high(const tb_gui_spinbox *box)
{
    return fmax((double)box->from, (double)box->to);
}

/* Its value as text, by its format. */
static tb_text tb_gui_spinbox_written(const tb_gui_spinbox *box, char *buffer)
{
    return tb_gui_format_number(tb_gui_string_text(&box->format), tb_gui_spinbox_value(box),
                                tb_gui_step_digits((double)box->increment), buffer);
}

/* Show its value, unless the text is being typed. */
static void tb_gui_spinbox_show(tb_gui_spinbox *box)
{
    char buffer[TB_GUI_NUMBER_CAPACITY] = {0};

    if (!box->field.dirty)
    {
        tb_gui_field_set_text(&box->field, tb_gui_spinbox_written(box, buffer));
    }
}

/* Set its value from input: kept within its range, then shown. */
static void tb_gui_spinbox_take(tb_widget widget, tb_gui_spinbox *box, double value)
{
    const double before = tb_gui_spinbox_value(box);
    const double clamped = fmin(fmax(value, tb_gui_spinbox_low(box)), tb_gui_spinbox_high(box));

    if (tb_var_alive(box->variable))
    {
        (void)tb_var_set_float(box->variable, clamped);
    }
    else
    {
        box->value = (float)clamped;
    }
    box->field.dirty = false;
    tb_gui_spinbox_show(box);
    if (tb_gui_spinbox_value(box) != before)
    {
        tb_gui_values_changed(widget, box->command);
    }
}

/* Take what was typed: a number, or the value it had. */
static void tb_gui_spinbox_commit(tb_widget widget, tb_gui_spinbox *box)
{
    double typed = 0.0;

    if (!box->field.dirty)
    {
        return;
    }
    if (!tb_gui_read_number(tb_gui_field_text(&box->field), &typed))
    {
        box->field.dirty = false;
        tb_gui_spinbox_show(box);
        return;
    }
    tb_gui_spinbox_take(widget, box, typed);
}

/* Step by a count of increments, going around past an end when it wraps. */
static void tb_gui_spinbox_step(tb_widget widget, tb_gui_spinbox *box, double steps)
{
    const double low = tb_gui_spinbox_low(box);
    const double high = tb_gui_spinbox_high(box);
    double value = 0.0;

    tb_gui_spinbox_commit(widget, box);
    value = tb_gui_spinbox_value(box) + steps * (double)box->increment;
    if (box->wrap && value > high)
    {
        value = low;
    }
    else if (box->wrap && value < low)
    {
        value = high;
    }
    tb_gui_spinbox_take(widget, box, value);
}

/* ---------------------------------------------------------------------------
 * Measures and drawing.
 * ------------------------------------------------------------------------- */

static float tb_gui_spinbox_arrow_width(tb_gui *gui)
{
    return tb_gui_values_measures(gui).line_height;
}

static tb_gui_edges tb_gui_spinbox_insets(tb_widget widget, const void *data)
{
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const float across = theme->frame_padding.x + theme->border;
    const float down = theme->frame_padding.y + theme->border;

    (void)data;
    return (tb_gui_edges){across, down, across, down};
}

static tb_vec2 tb_gui_spinbox_measure(tb_widget widget, const void *data)
{
    const tb_gui_spinbox *const box = data;

    return (tb_vec2){tb_gui_values_char_width(widget.gui, box->width) +
                         tb_gui_spinbox_arrow_width(widget.gui),
                     tb_gui_values_measures(widget.gui).line_height};
}

/* Its arrows' column, at the right edge. */
static tb_canvas_rect tb_gui_spinbox_arrows(tb_widget widget, tb_canvas_rect rect)
{
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const float width = tb_gui_spinbox_arrow_width(widget.gui) + theme->frame_padding.x;

    return (tb_canvas_rect){rect.x + rect.width - width, rect.y, width, rect.height};
}

static tb_canvas_rect tb_gui_spinbox_text_rect(tb_widget widget, tb_canvas_rect rect)
{
    const tb_gui_edges insets = tb_gui_spinbox_insets(widget, NULL);
    const float arrows = tb_gui_spinbox_arrow_width(widget.gui);

    return (tb_canvas_rect){rect.x + insets.left, rect.y + insets.top,
                            rect.width - insets.left - insets.right - arrows,
                            rect.height - insets.top - insets.bottom};
}

/* A chevron in a rectangle, pointing up or down. */
static void tb_gui_chevron(tb_gui_paint *paint, tb_canvas_rect rect, bool up, tb_vec4 color,
                           float thickness)
{
    const float half_width = rect.width * TB_GUI_CHEVRON_WIDTH * 0.5f;
    const float half_height = rect.height * TB_GUI_CHEVRON_HEIGHT;
    const float middle_x = rect.x + rect.width * 0.5f;
    const float middle_y = rect.y + rect.height * 0.5f;
    const float tip = up ? middle_y - half_height : middle_y + half_height;
    const float base = up ? middle_y + half_height : middle_y - half_height;

    tb_gui_paint_line(paint, (tb_vec2){middle_x - half_width, base}, (tb_vec2){middle_x, tip},
                      thickness, color);
    tb_gui_paint_line(paint, (tb_vec2){middle_x, tip}, (tb_vec2){middle_x + half_width, base},
                      thickness, color);
}

static void tb_gui_spinbox_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_spinbox *const box = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_canvas_rect arrows = tb_gui_spinbox_arrows(widget, rect);
    const float half = arrows.height * 0.5f;
    const tb_canvas_rect up = {arrows.x, arrows.y, arrows.width, half};
    const tb_canvas_rect down = {arrows.x, arrows.y + half, arrows.width, half};
    const bool disabled = (tb_widget_state(widget) & TB_WIDGET_DISABLED) != 0;
    const tb_vec4 mark = theme->colors[disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT];
    char buffer[TB_GUI_NUMBER_CAPACITY] = {0};

    tb_gui_values_draw_field(widget, paint, rect);
    if (box->held != TB_GUI_NO_ARROW)
    {
        tb_gui_paint_fill(paint, box->held == TB_GUI_ARROW_UP ? up : down, 0.0f,
                          theme->colors[TB_UI_COLOR_BUTTON_ACTIVE]);
    }
    tb_gui_paint_line(paint, (tb_vec2){arrows.x, arrows.y},
                      (tb_vec2){arrows.x, arrows.y + arrows.height}, theme->border,
                      theme->colors[TB_UI_COLOR_BORDER]);
    tb_gui_chevron(paint, up, true, mark, theme->border * 1.5f);
    tb_gui_chevron(paint, down, false, mark, theme->border * 1.5f);
    tb_gui_field_draw(widget, &box->field, paint, tb_gui_spinbox_text_rect(widget, rect),
                      box->field.dirty ? tb_gui_field_text(&box->field)
                                       : tb_gui_spinbox_written(box, buffer));
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

static bool tb_gui_spinbox_key(tb_widget widget, tb_gui_spinbox *box, const tb_gui_event *event)
{
    const tb_key key = event->key;

    if (!tb_gui_values_plain(event) && key != TB_KEY_ENTER)
    {
        return false;
    }
    if (key == TB_KEY_UP || key == TB_KEY_DOWN)
    {
        tb_gui_spinbox_step(widget, box, key == TB_KEY_UP ? 1.0 : -1.0);
        return true;
    }
    if (key == TB_KEY_PAGE_UP || key == TB_KEY_PAGE_DOWN)
    {
        tb_gui_spinbox_step(widget, box,
                            key == TB_KEY_PAGE_UP ? TB_GUI_SPINBOX_PAGE : -TB_GUI_SPINBOX_PAGE);
        return true;
    }
    if (key == TB_KEY_ENTER)
    {
        /* Taken, and left to a dialog's default button too. */
        tb_gui_spinbox_commit(widget, box);
        return false;
    }
    if (key == TB_KEY_ESCAPE && box->field.dirty)
    {
        box->field.dirty = false;
        tb_gui_spinbox_show(box);
        return true;
    }
    return false;
}

static bool tb_gui_spinbox_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_spinbox *const box = data;
    const tb_canvas_rect rect = tb_widget_rect(widget);
    const tb_canvas_rect arrows = tb_gui_spinbox_arrows(widget, rect);
    const tb_canvas_rect text = tb_gui_spinbox_text_rect(widget, rect);

    if (event->kind != TB_GUI_EVENT_VALIDATE)
    {
        tb_gui_spinbox_show(box);
    }
    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS:
        if (event->button == TB_MOUSE_BUTTON_LEFT && tb_ui_rect_contains(arrows, event->position))
        {
            const bool up = event->position.y < arrows.y + arrows.height * 0.5f;
            box->held = up ? TB_GUI_ARROW_UP : TB_GUI_ARROW_DOWN;
            tb_gui_spinbox_step(widget, box, up ? 1.0 : -1.0);
            return true;
        }
        break;
    case TB_GUI_EVENT_RELEASE:
        box->held = TB_GUI_NO_ARROW;
        break;
    case TB_GUI_EVENT_KEY:
        if (tb_gui_spinbox_key(widget, box, event))
        {
            return true;
        }
        break;
    case TB_GUI_EVENT_WHEEL:
        if (event->wheel.y != 0.0f)
        {
            tb_gui_spinbox_step(widget, box, event->wheel.y > 0.0f ? 1.0 : -1.0);
            return true;
        }
        return false;
    case TB_GUI_EVENT_FOCUS_OUT:
        tb_gui_spinbox_commit(widget, box);
        break;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_LEAVE:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_MOTION:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    if (!tb_widget_alive(widget))
    {
        return true;
    }
    return tb_gui_field_event(widget, &box->field, event, text);
}

/* ---------------------------------------------------------------------------
 * Properties.
 * ------------------------------------------------------------------------- */

static void tb_gui_spinbox_init(tb_widget widget, void *data)
{
    tb_gui_spinbox *const box = data;

    tb_gui_field_init(&box->field, false);
    box->field.deferred = true;
    box->width = TB_GUI_SPINBOX_WIDTH;
    box->to = TB_GUI_SPINBOX_DEFAULT_TO;
    box->increment = 1.0f;
    tb_widget_set_cursor(widget, TB_CURSOR_TEXT);
    tb_gui_spinbox_show(box);
}

static void tb_gui_spinbox_destroy(tb_widget widget, void *data)
{
    tb_gui_spinbox *const box = data;

    (void)widget;
    tb_gui_field_free(&box->field);
}

static bool tb_gui_spinbox_get_number(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_float((float)tb_gui_spinbox_value(box));
    return true;
}

static bool tb_gui_spinbox_set_value(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_spinbox *const box = data;
    const double clamped =
        fmin(fmax((double)value->number, tb_gui_spinbox_low(box)), tb_gui_spinbox_high(box));

    (void)widget;
    if (tb_var_alive(box->variable) && !tb_var_set_float(box->variable, clamped))
    {
        return false;
    }
    box->value = (float)clamped;
    box->field.dirty = false;
    tb_gui_spinbox_show(box);
    return true;
}

static bool tb_gui_spinbox_get_variable(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_variable(box->variable);
    return true;
}

static bool tb_gui_spinbox_set_variable(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_spinbox *const box = data;

    if (!tb_gui_var_of_widget(widget, value->variable))
    {
        return false;
    }
    box->variable = value->variable;
    box->field.dirty = false;
    tb_gui_spinbox_show(box);
    return true;
}

static bool tb_gui_spinbox_set_bound(tb_widget widget, void *data, const tb_gui_value *value,
                                     bool to)
{
    tb_gui_spinbox *const box = data;

    (void)widget;
    if (to)
    {
        box->to = value->number;
    }
    else
    {
        box->from = value->number;
    }
    tb_gui_spinbox_show(box);
    return true;
}

static bool tb_gui_spinbox_set_from(tb_widget widget, void *data, const tb_gui_value *value)
{
    return tb_gui_spinbox_set_bound(widget, data, value, false);
}

static bool tb_gui_spinbox_set_to(tb_widget widget, void *data, const tb_gui_value *value)
{
    return tb_gui_spinbox_set_bound(widget, data, value, true);
}

static bool tb_gui_spinbox_get_from(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_float(box->from);
    return true;
}

static bool tb_gui_spinbox_get_to(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_float(box->to);
    return true;
}

static bool tb_gui_spinbox_get_increment(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_float(box->increment);
    return true;
}

static bool tb_gui_spinbox_set_increment(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_spinbox *const box = data;

    (void)widget;
    if (!(value->number > 0.0f))
    {
        return false;
    }
    box->increment = value->number;
    tb_gui_spinbox_show(box);
    return true;
}

static bool tb_gui_spinbox_get_format(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_text(tb_gui_string_text(&box->format));
    return true;
}

static bool tb_gui_spinbox_set_format(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_spinbox *const box = data;

    (void)widget;
    if (!tb_gui_format_valid(value->text) || value->text.length >= TB_GUI_NUMBER_CAPACITY)
    {
        return false;
    }
    tb_gui_string_set(&box->format, value->text);
    tb_gui_spinbox_show(box);
    return true;
}

static bool tb_gui_spinbox_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_spinbox *const box = data;

    (void)widget;
    *value = tb_gui_int(box->width);
    return true;
}

static bool tb_gui_spinbox_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_spinbox *const box = data;

    (void)widget;
    if (value->integer < 1)
    {
        return false;
    }
    box->width = value->integer;
    return true;
}

static void tb_gui_spinbox_release_format(tb_widget widget, void *data)
{
    tb_gui_spinbox *const box = data;

    tb_gui_string_free(&box->format);
    tb_gui_spinbox_destroy(widget, data);
}

static const tb_gui_property tb_gui_spinbox_properties[] = {
    {.name = "value",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_spinbox_get_number,
     .set = tb_gui_spinbox_set_value},
    {.name = "variable",
     .type = TB_GUI_TYPE_VARIABLE,
     .get = tb_gui_spinbox_get_variable,
     .set = tb_gui_spinbox_set_variable},
    {.name = "from",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_spinbox_get_from,
     .set = tb_gui_spinbox_set_from},
    {.name = "to",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_spinbox_get_to,
     .set = tb_gui_spinbox_set_to},
    {.name = "increment",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_spinbox_get_increment,
     .set = tb_gui_spinbox_set_increment},
    {.name = "format",
     .type = TB_GUI_TYPE_TEXT,
     .get = tb_gui_spinbox_get_format,
     .set = tb_gui_spinbox_set_format},
    {.name = "width",
     .type = TB_GUI_TYPE_INT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_spinbox_get_width,
     .set = tb_gui_spinbox_set_width},
    {.name = "wrap", .type = TB_GUI_TYPE_BOOL, .offset = offsetof(tb_gui_spinbox, wrap)},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_spinbox, command)},
    {.name = "validate",
     .type = TB_GUI_TYPE_COMMAND,
     .offset = offsetof(tb_gui_spinbox, field.validate)},
};

static const tb_widget_class tb_gui_spinbox_kind = {
    .name = "spinbox",
    .data_size = sizeof(tb_gui_spinbox),
    .properties = tb_gui_spinbox_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_spinbox_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_spinbox_init,
    .destroy = tb_gui_spinbox_release_format,
    .measure = tb_gui_spinbox_measure,
    .insets = tb_gui_spinbox_insets,
    .draw = tb_gui_spinbox_draw,
    .event = tb_gui_spinbox_event};

const tb_widget_class *tb_spinbox_class(void)
{
    return &tb_gui_spinbox_kind;
}

tb_widget tb_spinbox(tb_widget parent, const tb_spinbox_desc *desc)
{
    const tb_spinbox_desc chosen = desc != NULL ? *desc : (tb_spinbox_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_spinbox_kind);
    const bool default_range = chosen.from == 0.0f && chosen.to == 0.0f;
    const tb_gui_value from = tb_gui_float(chosen.from);
    const tb_gui_value to = tb_gui_float(default_range ? TB_GUI_SPINBOX_DEFAULT_TO : chosen.to);
    const tb_gui_value increment = tb_gui_float(chosen.increment > 0.0f ? chosen.increment : 1.0f);
    const tb_gui_value format =
        tb_gui_text(tb_text_from_cstr(chosen.format != NULL ? chosen.format : ""));
    const tb_gui_value wrap = tb_gui_bool(chosen.wrap);
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);
    const tb_gui_value validate = tb_gui_command_of(chosen.validate, chosen.user);
    const tb_gui_value value = tb_gui_float(chosen.value);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);

    (void)tb_widget_set(widget, "from", &from);
    (void)tb_widget_set(widget, "to", &to);
    (void)tb_widget_set(widget, "increment", &increment);
    (void)tb_widget_set(widget, "format", &format);
    if (chosen.width > 0)
    {
        const tb_gui_value width = tb_gui_int(chosen.width);
        (void)tb_widget_set(widget, "width", &width);
    }
    (void)tb_widget_set(widget, "wrap", &wrap);
    (void)tb_widget_set(widget, "command", &command);
    (void)tb_widget_set(widget, "validate", &validate);
    if (tb_var_alive(chosen.variable))
    {
        (void)tb_widget_set(widget, "variable", &variable);
    }
    else
    {
        (void)tb_widget_set(widget, "value", &value);
    }
    return widget;
}
