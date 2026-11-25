/* Combo boxes: one of a list of values, chosen from a list that drops down
 * in the GUI's popup, above every widget (tb_gui_popup_open()). The value
 * is in a text variable, by its text; in an integer variable, by its index;
 * or its own. */
#include "gui_values.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Values the list shows at once unless chosen; characters of "0" it is at
 * least wide. */
#define TB_GUI_COMBO_HEIGHT 8
#define TB_GUI_COMBO_LEAST_WIDTH 4
/* No index. */
#define TB_GUI_NO_INDEX (-1)
/* The list's scroll bar's width, in units. */
#define TB_GUI_COMBO_BAR 4.0f
/* The arrow's chevron, in its square: its width and height. */
#define TB_GUI_COMBO_CHEVRON_WIDTH 0.4f
#define TB_GUI_COMBO_CHEVRON_HEIGHT 0.12f

typedef struct tb_gui_combobox
{
    tb_gui_string values; /* one a line */
    tb_var variable;
    int32_t current;
    int32_t width;
    int32_t height;
    tb_gui_command command;
    int32_t highlight; /* in the open list, by the keys */
    int32_t top;       /* the first value the open list shows */
    bool picking;      /* a press in the open list waits for its release */
} tb_gui_combobox;

/* ---------------------------------------------------------------------------
 * Values.
 * ------------------------------------------------------------------------- */

/* How many values, one a line; a last line break ends the last. */
static int32_t tb_gui_combo_count(const tb_gui_combobox *box)
{
    const tb_text values = tb_gui_string_text(&box->values);
    int32_t count = 0;

    if (values.length == 0)
    {
        return 0;
    }
    count = 1;
    for (size_t at = 0; at < values.length; at++)
    {
        if (values.bytes[at] == '\n' && at + 1 < values.length)
        {
            count++;
        }
    }
    return count;
}

static tb_text tb_gui_combo_value(const tb_gui_combobox *box, int32_t index)
{
    const tb_text values = tb_gui_string_text(&box->values);
    size_t begin = 0;
    int32_t line = 0;

    for (size_t at = 0; at <= values.length; at++)
    {
        if (at == values.length || values.bytes[at] == '\n')
        {
            if (line == index)
            {
                return tb_text_make(values.bytes + begin, at - begin);
            }
            line++;
            begin = at + 1;
        }
    }
    return tb_text_make("", 0);
}

static int32_t tb_gui_combo_find(const tb_gui_combobox *box, tb_text text)
{
    const int32_t count = tb_gui_combo_count(box);

    for (int32_t index = 0; index < count; index++)
    {
        if (tb_text_equal(tb_gui_combo_value(box, index), text))
        {
            return index;
        }
    }
    return TB_GUI_NO_INDEX;
}

/* The index chosen, or TB_GUI_NO_INDEX. */
static int32_t tb_gui_combo_current(const tb_gui_combobox *box)
{
    int32_t index = box->current;

    if (tb_var_alive(box->variable))
    {
        index = tb_var_type(box->variable) == TB_GUI_TYPE_TEXT
                    ? tb_gui_combo_find(box, tb_var_get_text(box->variable))
                    : tb_var_get_int(box->variable);
    }
    return index >= 0 && index < tb_gui_combo_count(box) ? index : TB_GUI_NO_INDEX;
}

/* What it shows: a text variable's text, chosen or not; or the value chosen. */
static tb_text tb_gui_combo_text(const tb_gui_combobox *box)
{
    if (tb_var_alive(box->variable) && tb_var_type(box->variable) == TB_GUI_TYPE_TEXT)
    {
        return tb_var_get_text(box->variable);
    }
    return tb_gui_combo_value(box, tb_gui_combo_current(box));
}

/* Choose a value, as a program does: whether what it shows changed. */
static bool tb_gui_combo_set(tb_gui_combobox *box, int32_t index)
{
    const uint64_t version = tb_var_version(box->variable);
    const int32_t before = box->current;

    if (index < 0 || index >= tb_gui_combo_count(box))
    {
        return false;
    }
    if (!tb_var_alive(box->variable))
    {
        box->current = index;
        return before != index;
    }
    if (tb_var_type(box->variable) == TB_GUI_TYPE_TEXT)
    {
        (void)tb_gui_var_set_span(box->variable, tb_gui_combo_value(box, index));
    }
    else
    {
        (void)tb_var_set_int(box->variable, index);
    }
    return tb_var_version(box->variable) != version;
}

/* Choose a value from input. */
static void tb_gui_combo_choose(tb_widget widget, tb_gui_combobox *box, int32_t index)
{
    if (tb_gui_combo_set(box, index))
    {
        tb_gui_values_changed(widget, box->command);
    }
}

/* ---------------------------------------------------------------------------
 * The list.
 * ------------------------------------------------------------------------- */

static float tb_gui_combo_item_height(tb_gui *gui)
{
    return tb_gui_values_measures(gui).line_height + tb_gui_theme(gui)->frame_padding.y;
}

static int32_t tb_gui_combo_rows(const tb_gui_combobox *box)
{
    const int32_t count = tb_gui_combo_count(box);
    return count < box->height ? count : box->height;
}

/* Where the list is, below the combo box, or above when it fits only there. */
static tb_canvas_rect tb_gui_combo_list_rect(tb_widget widget, const tb_gui_combobox *box)
{
    const tb_canvas_rect rect = tb_widget_rect(widget);
    const float border = tb_gui_theme(widget.gui)->border;
    const float height =
        (float)tb_gui_combo_rows(box) * tb_gui_combo_item_height(widget.gui) + 2.0f * border;
    const float bottom = tb_gui_view_of(widget.gui).size.y;
    tb_canvas_rect list = {rect.x, rect.y + rect.height, rect.width, height};

    if (list.y + height > bottom && rect.y - height >= 0.0f)
    {
        list.y = rect.y - height;
    }
    return list;
}

/* The value under a point in the open list, or TB_GUI_NO_INDEX. */
static int32_t tb_gui_combo_item_at(tb_widget widget, const tb_gui_combobox *box, tb_vec2 point,
                                    tb_canvas_rect list)
{
    const float border = tb_gui_theme(widget.gui)->border;
    const float item = tb_gui_combo_item_height(widget.gui);
    int32_t row = 0;

    if (!tb_ui_rect_contains(list, point))
    {
        return TB_GUI_NO_INDEX;
    }
    row = (int32_t)floorf((point.y - list.y - border) / item);
    if (row < 0 || row >= tb_gui_combo_rows(box))
    {
        return TB_GUI_NO_INDEX;
    }
    return box->top + row;
}

/* Scroll the list so that a value is seen. */
static void tb_gui_combo_reveal(tb_gui_combobox *box, int32_t index)
{
    const int32_t rows = tb_gui_combo_rows(box);
    const int32_t most = tb_gui_combo_count(box) - rows;

    if (index < box->top)
    {
        box->top = index;
    }
    if (index >= box->top + rows)
    {
        box->top = index - rows + 1;
    }
    box->top = box->top > most ? most : box->top;
    box->top = box->top < 0 ? 0 : box->top;
}

static void tb_gui_combo_draw_list(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_combobox *const box = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect list = tb_gui_paint_rect(paint);
    const float item = tb_gui_combo_item_height(widget.gui);
    const int32_t rows = tb_gui_combo_rows(box);
    const int32_t count = tb_gui_combo_count(box);
    const tb_gui *const gui = widget.gui;
    int32_t lit = box->highlight;

    if (gui->mouse_inside && tb_ui_rect_contains(list, gui->mouse))
    {
        lit = tb_gui_combo_item_at(widget, box, gui->mouse, list);
    }
    tb_gui_paint_fill(paint, list, theme->frame_rounding, theme->colors[TB_UI_COLOR_POPUP]);
    for (int32_t row = 0; row < rows; row++)
    {
        const int32_t index = box->top + row;
        const tb_canvas_rect line = {list.x + theme->border,
                                     list.y + theme->border + (float)row * item,
                                     list.width - 2.0f * theme->border, item};
        const tb_canvas_rect text = {line.x + theme->frame_padding.x, line.y,
                                     line.width - 2.0f * theme->frame_padding.x, line.height};
        const tb_gui_text_block block = {.text = tb_gui_combo_value(box, index),
                                         .anchor = TB_GUI_ANCHOR_W};
        if (index == lit)
        {
            tb_gui_paint_fill(paint, line, 0.0f, theme->colors[TB_UI_COLOR_HEADER_HOT]);
        }
        else if (index == tb_gui_combo_current(box))
        {
            tb_gui_paint_fill(paint, line, 0.0f, theme->colors[TB_UI_COLOR_SELECTION]);
        }
        tb_gui_paint_text(paint, &block, text, theme->colors[TB_UI_COLOR_TEXT]);
    }
    if (count > rows)
    {
        const float track = list.height - 2.0f * theme->border;
        const tb_canvas_rect grab = {list.x + list.width - theme->border - TB_GUI_COMBO_BAR,
                                     list.y + theme->border +
                                         track * (float)box->top / (float)count,
                                     TB_GUI_COMBO_BAR, track * (float)rows / (float)count};
        tb_gui_paint_fill(paint, grab, TB_GUI_COMBO_BAR * 0.5f,
                          theme->colors[TB_UI_COLOR_SCROLLBAR_GRAB]);
    }
    tb_gui_paint_outline(paint, list, theme->frame_rounding, theme->border,
                         theme->colors[TB_UI_COLOR_BORDER]);
}

static void tb_gui_combo_open(tb_widget widget, tb_gui_combobox *box)
{
    const int32_t current = tb_gui_combo_current(box);

    if (tb_gui_combo_count(box) == 0)
    {
        return;
    }
    box->highlight = current >= 0 ? current : 0;
    tb_gui_combo_reveal(box, box->highlight);
    box->picking = false;
    tb_gui_popup_open(widget, tb_gui_combo_list_rect(widget, box), tb_gui_combo_draw_list);
}

static void tb_gui_combo_close(tb_widget widget, tb_gui_combobox *box)
{
    box->picking = false;
    if (tb_gui_popup_owned(widget.gui, widget))
    {
        tb_gui_popup_close(widget.gui);
    }
}

bool tb_combobox_open(tb_widget widget)
{
    return tb_widget_class_of(widget) == tb_combobox_class() &&
           tb_gui_popup_owned(widget.gui, widget);
}

/* ---------------------------------------------------------------------------
 * Measures and drawing.
 * ------------------------------------------------------------------------- */

static tb_gui_edges tb_gui_combo_insets(tb_widget widget, const void *data)
{
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const float across = theme->frame_padding.x + theme->border;
    const float down = theme->frame_padding.y + theme->border;

    (void)data;
    return (tb_gui_edges){across, down, across, down};
}

static tb_vec2 tb_gui_combo_measure(tb_widget widget, const void *data)
{
    const tb_gui_combobox *const box = data;
    const float line = tb_gui_values_measures(widget.gui).line_height;
    float width = tb_gui_values_char_width(widget.gui, TB_GUI_COMBO_LEAST_WIDTH);

    if (box->width > 0)
    {
        width = tb_gui_values_char_width(widget.gui, box->width);
    }
    else
    {
        const int32_t count = tb_gui_combo_count(box);
        for (int32_t index = 0; index < count; index++)
        {
            const tb_gui_text_block block = {.text = tb_gui_combo_value(box, index)};
            width = fmaxf(width, tb_gui_measure_text(widget.gui, &block).x);
        }
    }
    return (tb_vec2){width + tb_gui_theme(widget.gui)->frame_padding.x + line, line};
}

static void tb_gui_combo_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_combobox *const box = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const uint32_t state = tb_widget_state(widget);
    const bool disabled = (state & TB_WIDGET_DISABLED) != 0;
    const bool open = tb_gui_popup_owned(widget.gui, widget);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_gui_edges insets = tb_gui_combo_insets(widget, data);
    const float line = tb_gui_values_measures(widget.gui).line_height;
    const tb_canvas_rect arrow = {rect.x + rect.width - insets.right - line,
                                  rect.y + (rect.height - line) * 0.5f, line, line};
    const tb_canvas_rect text = {rect.x + insets.left, rect.y,
                                 arrow.x - rect.x - insets.left - theme->frame_padding.x,
                                 rect.height};
    const tb_gui_text_block block = {.text = tb_gui_combo_text(box), .anchor = TB_GUI_ANCHOR_W};
    const tb_vec4 mark = theme->colors[disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT];
    tb_ui_color fill = TB_UI_COLOR_BUTTON;

    if (open || (state & TB_WIDGET_PRESSED) != 0)
    {
        fill = TB_UI_COLOR_BUTTON_ACTIVE;
    }
    else if ((state & TB_WIDGET_HOVERED) != 0)
    {
        fill = TB_UI_COLOR_BUTTON_HOT;
    }
    tb_gui_paint_fill(paint, rect, theme->frame_rounding, theme->colors[fill]);
    tb_gui_paint_outline(paint, rect, theme->frame_rounding, theme->border,
                         theme->colors[TB_UI_COLOR_BORDER]);
    if ((state & TB_WIDGET_FOCUSED) != 0)
    {
        tb_gui_values_draw_focus(widget, paint, rect);
    }
    tb_gui_paint_text(paint, &block, text, mark);
    const float half_width = arrow.width * TB_GUI_COMBO_CHEVRON_WIDTH * 0.5f;
    const float half_height = arrow.height * TB_GUI_COMBO_CHEVRON_HEIGHT;
    const float middle_x = arrow.x + arrow.width * 0.5f;
    const float middle_y = arrow.y + arrow.height * 0.5f;
    const float tip = open ? middle_y - half_height : middle_y + half_height;
    const float base = open ? middle_y + half_height : middle_y - half_height;
    tb_gui_paint_line(paint, (tb_vec2){middle_x - half_width, base}, (tb_vec2){middle_x, tip},
                      theme->border * 1.5f, mark);
    tb_gui_paint_line(paint, (tb_vec2){middle_x, tip}, (tb_vec2){middle_x + half_width, base},
                      theme->border * 1.5f, mark);
}

/* Its list follows it when it is laid out again. */
static void tb_gui_combo_arrange(tb_widget widget, void *data, tb_canvas_rect rect)
{
    tb_gui_combobox *const box = data;

    (void)rect;
    if (tb_gui_popup_owned(widget.gui, widget))
    {
        tb_gui_popup_open(widget, tb_gui_combo_list_rect(widget, box), tb_gui_combo_draw_list);
    }
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

static bool tb_gui_combo_key_open(tb_widget widget, tb_gui_combobox *box, tb_key key)
{
    const int32_t count = tb_gui_combo_count(box);
    const int32_t rows = tb_gui_combo_rows(box);
    int32_t highlight = box->highlight;

    if (key == TB_KEY_ESCAPE)
    {
        tb_gui_combo_close(widget, box);
        return true;
    }
    if (key == TB_KEY_ENTER || key == TB_KEY_SPACE)
    {
        tb_gui_combo_close(widget, box);
        tb_gui_combo_choose(widget, box, box->highlight);
        return true;
    }
    if (key == TB_KEY_UP || key == TB_KEY_DOWN)
    {
        highlight += key == TB_KEY_UP ? -1 : 1;
    }
    else if (key == TB_KEY_PAGE_UP || key == TB_KEY_PAGE_DOWN)
    {
        highlight += key == TB_KEY_PAGE_UP ? -rows : rows;
    }
    else if (key == TB_KEY_HOME || key == TB_KEY_END)
    {
        highlight = key == TB_KEY_HOME ? 0 : count - 1;
    }
    else
    {
        return false;
    }
    highlight = highlight >= count ? count - 1 : highlight;
    box->highlight = highlight < 0 ? 0 : highlight;
    tb_gui_combo_reveal(box, box->highlight);
    return true;
}

static bool tb_gui_combo_key(tb_widget widget, tb_gui_combobox *box, const tb_gui_event *event)
{
    const tb_key key = event->key;
    const bool alt = (event->modifiers & TB_MODIFIER_ALT) != 0;
    const int32_t current = tb_gui_combo_current(box);

    if (tb_gui_popup_owned(widget.gui, widget))
    {
        if (alt && key == TB_KEY_UP)
        {
            tb_gui_combo_close(widget, box);
            return true;
        }
        return tb_gui_values_plain(event) && tb_gui_combo_key_open(widget, box, key);
    }
    if ((alt && key == TB_KEY_DOWN) || (key == TB_KEY_SPACE && tb_gui_values_plain(event)))
    {
        tb_gui_combo_open(widget, box);
        return true;
    }
    if (!tb_gui_values_plain(event))
    {
        return false;
    }
    if (key == TB_KEY_UP || key == TB_KEY_DOWN)
    {
        tb_gui_combo_choose(widget, box, current + (key == TB_KEY_UP ? -1 : 1));
        return true;
    }
    if (key == TB_KEY_HOME || key == TB_KEY_END)
    {
        tb_gui_combo_choose(widget, box, key == TB_KEY_HOME ? 0 : tb_gui_combo_count(box) - 1);
        return true;
    }
    return false;
}

static bool tb_gui_combo_wheel(tb_widget widget, tb_gui_combobox *box, const tb_gui_event *event)
{
    const int32_t notch = event->wheel.y > 0.0f ? -1 : 1;

    if (event->wheel.y == 0.0f)
    {
        return false;
    }
    if (tb_gui_popup_owned(widget.gui, widget))
    {
        box->top += notch;
        tb_gui_combo_reveal(box, box->top);
        return true;
    }
    if ((tb_widget_state(widget) & TB_WIDGET_FOCUSED) == 0)
    {
        return false;
    }
    tb_gui_combo_choose(widget, box, tb_gui_combo_current(box) + notch);
    return true;
}

static bool tb_gui_combo_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_combobox *const box = data;
    const bool open = tb_gui_popup_owned(widget.gui, widget);
    const tb_canvas_rect list = tb_gui_combo_list_rect(widget, box);

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS:
        if (event->button != TB_MOUSE_BUTTON_LEFT)
        {
            return true;
        }
        if (open && tb_ui_rect_contains(list, event->position))
        {
            box->picking = true;
        }
        else if (open)
        {
            tb_gui_combo_close(widget, box);
        }
        else
        {
            tb_gui_combo_open(widget, box);
        }
        return true;
    case TB_GUI_EVENT_RELEASE:
        if (box->picking && event->button == TB_MOUSE_BUTTON_LEFT)
        {
            const int32_t index = tb_gui_combo_item_at(widget, box, event->position, list);
            box->picking = false;
            if (index != TB_GUI_NO_INDEX)
            {
                tb_gui_combo_close(widget, box);
                tb_gui_combo_choose(widget, box, index);
            }
        }
        return true;
    case TB_GUI_EVENT_KEY:
        return tb_gui_combo_key(widget, box, event);
    case TB_GUI_EVENT_WHEEL:
        return tb_gui_combo_wheel(widget, box, event);
    case TB_GUI_EVENT_FOCUS_OUT:
        tb_gui_combo_close(widget, box);
        return false;
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
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Properties.
 * ------------------------------------------------------------------------- */

static bool tb_gui_combo_get_current(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    *value = tb_gui_int(tb_gui_combo_current(data));
    return true;
}

static bool tb_gui_combo_set_current(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_combobox *const box = data;

    (void)widget;
    if (value->integer == TB_GUI_NO_INDEX && !tb_var_alive(box->variable))
    {
        box->current = TB_GUI_NO_INDEX;
        return true;
    }
    if (value->integer < 0 || value->integer >= tb_gui_combo_count(box))
    {
        return false;
    }
    (void)tb_gui_combo_set(box, value->integer);
    return true;
}

static bool tb_gui_combo_get_variable(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_combobox *const box = data;

    (void)widget;
    *value = tb_gui_variable(box->variable);
    return true;
}

static bool tb_gui_combo_set_variable(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_combobox *const box = data;
    const tb_gui_type type = tb_var_type(value->variable);

    if (!tb_gui_var_of_widget(widget, value->variable) ||
        (value->variable.gui != NULL && type != TB_GUI_TYPE_TEXT && type != TB_GUI_TYPE_INT))
    {
        return false;
    }
    box->variable = value->variable;
    return true;
}

static bool tb_gui_combo_get_size(const tb_gui_combobox *box, bool height, tb_gui_value *value)
{
    *value = tb_gui_int(height ? box->height : box->width);
    return true;
}

static bool tb_gui_combo_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    return tb_gui_combo_get_size(data, false, value);
}

static bool tb_gui_combo_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    return tb_gui_combo_get_size(data, true, value);
}

static bool tb_gui_combo_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_combobox *const box = data;

    (void)widget;
    if (value->integer < 0)
    {
        return false;
    }
    box->width = value->integer;
    return true;
}

static bool tb_gui_combo_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_combobox *const box = data;

    (void)widget;
    if (value->integer < 1)
    {
        return false;
    }
    box->height = value->integer;
    return true;
}

static void tb_gui_combo_init(tb_widget widget, void *data)
{
    tb_gui_combobox *const box = data;

    (void)widget;
    box->current = TB_GUI_NO_INDEX;
    box->height = TB_GUI_COMBO_HEIGHT;
}

static const tb_gui_property tb_gui_combo_properties[] = {
    {.name = "values",
     .type = TB_GUI_TYPE_TEXT,
     .offset = offsetof(tb_gui_combobox, values),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "current",
     .type = TB_GUI_TYPE_INT,
     .get = tb_gui_combo_get_current,
     .set = tb_gui_combo_set_current},
    {.name = "variable",
     .type = TB_GUI_TYPE_VARIABLE,
     .get = tb_gui_combo_get_variable,
     .set = tb_gui_combo_set_variable},
    {.name = "width",
     .type = TB_GUI_TYPE_INT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_combo_get_width,
     .set = tb_gui_combo_set_width},
    {.name = "height",
     .type = TB_GUI_TYPE_INT,
     .get = tb_gui_combo_get_height,
     .set = tb_gui_combo_set_height},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_combobox, command)},
};

static const tb_widget_class tb_gui_combo_kind = {
    .name = "combobox",
    .data_size = sizeof(tb_gui_combobox),
    .properties = tb_gui_combo_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_combo_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_combo_init,
    .measure = tb_gui_combo_measure,
    .insets = tb_gui_combo_insets,
    .arrange = tb_gui_combo_arrange,
    .draw = tb_gui_combo_draw,
    .event = tb_gui_combo_event};

const tb_widget_class *tb_combobox_class(void)
{
    return &tb_gui_combo_kind;
}

/* Values one a line, joined; tb_free() it. */
static char *tb_gui_combo_join(const char *const *values, size_t count)
{
    size_t length = 0;
    size_t at = 0;
    char *joined = NULL;

    for (size_t index = 0; index < count; index++)
    {
        const size_t size = tb_str_length(values[index]);
        TB_ASSERT_MSG(size < SIZE_MAX - length - 2, "too many values");
        length += size + 1;
    }
    joined = tb_malloc(length + 1);
    for (size_t index = 0; index < count; index++)
    {
        const size_t size = tb_str_length(values[index]);
        if (index > 0)
        {
            joined[at] = '\n';
            at++;
        }
        memcpy(joined + at, values[index], size);
        at += size;
    }
    joined[at] = '\0';
    return joined;
}

tb_widget tb_combobox(tb_widget parent, const tb_combobox_desc *desc)
{
    const tb_combobox_desc chosen = desc != NULL ? *desc : (tb_combobox_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_combo_kind);
    char *const joined =
        tb_gui_combo_join(chosen.values, chosen.values != NULL ? chosen.value_count : 0);
    const tb_gui_value values = tb_gui_text(tb_text_from_cstr(joined));
    const tb_gui_value width = tb_gui_int(chosen.width);
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);
    const tb_gui_value current = tb_gui_int(chosen.current);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);

    (void)tb_widget_set(widget, "values", &values);
    tb_free(joined);
    (void)tb_widget_set(widget, "width", &width);
    if (chosen.height > 0)
    {
        const tb_gui_value height = tb_gui_int(chosen.height);
        (void)tb_widget_set(widget, "height", &height);
    }
    (void)tb_widget_set(widget, "command", &command);
    if (tb_var_alive(chosen.variable))
    {
        (void)tb_widget_set(widget, "variable", &variable);
    }
    else
    {
        (void)tb_widget_set(widget, "current", &current);
    }
    return widget;
}
