/* What the kinds that edit values share: numbers written by a format, the
 * measures and frames they draw by, the events a change sends, and their
 * registration with the engine's other kinds. */
#include "gui_values.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/utf8.h"

#include <math.h>
#include <string.h>

/* What a line is when the theme has no font: its height, ascent, and
 * capitals, in ems; and a character's width. */
#define TB_GUI_FALLBACK_LINE 1.25f
#define TB_GUI_FALLBACK_ASCENT 0.95f
#define TB_GUI_FALLBACK_CAPS 0.7f
#define TB_GUI_FALLBACK_CHAR 0.55f
/* The most decimals a step's digits have, and how near a whole number a
 * step's multiple must be to count as one. */
#define TB_GUI_MOST_DIGITS 6
#define TB_GUI_DIGITS_TOLERANCE 1e-5
/* The widest and most precise a format may write. */
#define TB_GUI_MOST_FORMAT_WIDTH 32

/* ---------------------------------------------------------------------------
 * Numbers.
 * ------------------------------------------------------------------------- */

static bool tb_gui_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

/* Digits at `*at`, at most TB_GUI_MOST_FORMAT_WIDTH: false for a number
 * larger. */
static bool tb_gui_skip_number(tb_text format, size_t *at)
{
    int32_t value = 0;

    while (*at < format.length && tb_gui_is_digit(format.bytes[*at]))
    {
        value = value * 10 + (format.bytes[*at] - '0');
        if (value > TB_GUI_MOST_FORMAT_WIDTH)
        {
            return false;
        }
        (*at)++;
    }
    return true;
}

/* A conversion after its `%`, at `*at`: whether it is one of a double's. */
static bool tb_gui_skip_conversion(tb_text format, size_t *at)
{
    static const char flags[] = "-+ #0";
    static const char conversions[] = "fFeEgG";

    while (*at < format.length && memchr(flags, format.bytes[*at], sizeof(flags) - 1) != NULL)
    {
        (*at)++;
    }
    if (!tb_gui_skip_number(format, at))
    {
        return false;
    }
    if (*at < format.length && format.bytes[*at] == '.')
    {
        (*at)++;
        if (!tb_gui_skip_number(format, at))
        {
            return false;
        }
    }
    if (*at >= format.length ||
        memchr(conversions, format.bytes[*at], sizeof(conversions) - 1) == NULL)
    {
        return false;
    }
    (*at)++;
    return true;
}

bool tb_gui_format_valid(tb_text format)
{
    size_t conversions = 0;

    for (size_t at = 0; at < format.length;)
    {
        if (format.bytes[at] == '\0')
        {
            return false;
        }
        if (format.bytes[at] != '%')
        {
            at++;
            continue;
        }
        at++;
        if (at < format.length && format.bytes[at] == '%')
        {
            at++;
            continue;
        }
        if (!tb_gui_skip_conversion(format, &at))
        {
            return false;
        }
        conversions++;
    }
    return format.length == 0 || conversions == 1;
}

tb_text tb_gui_format_number(tb_text format, double value, int32_t digits, char *buffer)
{
    char pattern[TB_GUI_NUMBER_CAPACITY] = {0};
    size_t required = 0;

    /* -0 is written 0. */
    value = value == 0.0 ? 0.0 : value;
    if (format.length > 0 && format.length < sizeof(pattern))
    {
        memcpy(pattern, format.bytes, format.length);
        (void)tb_fmt_buffer(buffer, TB_GUI_NUMBER_CAPACITY, pattern, value);
        return tb_text_from_cstr(buffer);
    }
    if (digits >= 0)
    {
        (void)tb_fmt_buffer(pattern, sizeof(pattern), "%%.%df", (int)digits);
        (void)tb_fmt_buffer(buffer, TB_GUI_NUMBER_CAPACITY, pattern, value);
        return tb_text_from_cstr(buffer);
    }
    if (tb_decimal_shortest(value, buffer, TB_GUI_NUMBER_CAPACITY, &required) != TB_DECIMAL_OK)
    {
        buffer[0] = '\0';
    }
    return tb_text_from_cstr(buffer);
}

int32_t tb_gui_step_digits(double step)
{
    double scaled = fabs(step);

    if (!(scaled > 0.0) || !isfinite(scaled))
    {
        return 0;
    }
    for (int32_t digits = 0; digits < TB_GUI_MOST_DIGITS; digits++)
    {
        if (fabs(scaled - floor(scaled + 0.5)) < TB_GUI_DIGITS_TOLERANCE * fmax(scaled, 1.0))
        {
            return digits;
        }
        scaled *= 10.0;
    }
    return TB_GUI_MOST_DIGITS;
}

bool tb_gui_read_number(tb_text text, double *value)
{
    const tb_text trimmed = tb_text_trim(text, tb_text_from_cstr(" \t"));
    double read = 0.0;

    if (trimmed.length == 0 ||
        tb_decimal_parse(trimmed.bytes, trimmed.length, &read) != TB_DECIMAL_OK || !isfinite(read))
    {
        return false;
    }
    *value = read;
    return true;
}

/* ---------------------------------------------------------------------------
 * What kinds draw by.
 * ------------------------------------------------------------------------- */

tb_ui_font_measures tb_gui_values_measures(tb_widget widget)
{
    const tb_gui_resolved_font resolved = tb_gui_widget_font(widget, (tb_font_spec){0});
    tb_ui_font_measures measures = {0};

    if (resolved.font == NULL)
    {
        measures.line_height = resolved.size * TB_GUI_FALLBACK_LINE;
        measures.ascent = resolved.size * TB_GUI_FALLBACK_ASCENT;
        measures.cap_height = resolved.size * TB_GUI_FALLBACK_CAPS;
        return measures;
    }
    tb_ui_font_measures_of(resolved.font, resolved.size, widget.gui->view.scale, &measures);
    return measures;
}

float tb_gui_values_char_width(tb_widget widget, int32_t characters)
{
    const tb_gui_text_block block = {.text = tb_text_from_cstr("0")};
    float width = tb_widget_measure_text(widget, &block).x;

    if (!(width > 0.0f))
    {
        width = tb_gui_widget_font(widget, (tb_font_spec){0}).size * TB_GUI_FALLBACK_CHAR;
    }
    return width * (float)characters;
}

tb_gui_edges tb_gui_values_insets(tb_widget widget)
{
    const tb_style *const style = tb_style_in(widget, 0);
    const float border = style->border_width;

    return (tb_gui_edges){style->padding.left + border, style->padding.top + border,
                          style->padding.right + border, style->padding.bottom + border};
}

float tb_gui_values_line_top(tb_widget widget, tb_canvas_rect rect)
{
    const tb_ui_font_measures measures = tb_gui_values_measures(widget);

    /* Capitals are centered: they are what the eye lines up. */
    return rect.y + rect.height * 0.5f + measures.cap_height * 0.5f - measures.ascent;
}

void tb_gui_values_paint_line(tb_gui_paint *paint, tb_text text, float x, tb_canvas_rect rect,
                              tb_canvas_rect clip, tb_vec4 color)
{
    tb_gui *const gui = paint->gui;
    const tb_gui_resolved_font resolved = tb_gui_widget_font(paint->widget, (tb_font_spec){0});
    const tb_ui_text_run run = {.font = resolved.font, .size = resolved.size};

    if (resolved.font == NULL || text.length == 0)
    {
        return;
    }
    tb_ui_paint_text(&gui->paint, paint->layer, tb_ui_rect_intersect(paint->clip, clip), &run, text,
                     (tb_vec2){x, tb_gui_values_line_top(paint->widget, rect)}, color,
                     (tb_ui_id)paint->widget.index + 1u);
}

bool tb_gui_values_plain(const tb_gui_event *event)
{
    const uint32_t chords = TB_MODIFIER_CONTROL | TB_MODIFIER_ALT | TB_MODIFIER_SUPER;
    return (event->modifiers & chords) == 0;
}

void tb_gui_values_changed(tb_widget widget, tb_gui_command command)
{
    tb_gui_event event = tb_gui_event_for(widget, TB_GUI_EVENT_CHANGED);

    (void)tb_gui_send(widget, &event);
    if (tb_widget_alive(widget) && command.callback != NULL)
    {
        command.callback(&event, command.user);
    }
}

/* ---------------------------------------------------------------------------
 * Text being edited, found by its widget.
 * ------------------------------------------------------------------------- */

tb_gui_field *tb_gui_field_of(tb_widget widget)
{
    const tb_widget_class *const kind = tb_widget_class_of(widget);

    /* Each kind that edits text has its field first in its data. */
    if (kind == tb_entry_class() || kind == tb_text_box_class() || kind == tb_spinbox_class())
    {
        return tb_widget_data(widget);
    }
    return NULL;
}

void tb_gui_refuse_edit(tb_widget widget)
{
    tb_gui_field *const field = tb_gui_field_of(widget);

    if (field != NULL)
    {
        field->refused = true;
    }
}

bool tb_widget_selection(tb_widget widget, size_t *caret, size_t *anchor)
{
    tb_gui_field *const field = tb_gui_field_of(widget);

    TB_ASSERT(caret != NULL);
    TB_ASSERT(anchor != NULL);
    if (field == NULL)
    {
        return false;
    }
    tb_gui_field_sync(field);
    *caret = field->editor.caret;
    *anchor = field->editor.anchor;
    return true;
}

bool tb_widget_select(tb_widget widget, size_t caret, size_t anchor)
{
    tb_gui_field *const field = tb_gui_field_of(widget);
    size_t boundary = 0;

    if (field == NULL)
    {
        return false;
    }
    tb_gui_field_sync(field);
    tb_ui_editor *const editor = &field->editor;
    caret = caret < editor->length ? caret : editor->length;
    anchor = anchor < editor->length ? anchor : editor->length;
    if (tb_utf8_boundary(editor->buffer, editor->length, caret, &boundary))
    {
        caret = boundary;
    }
    if (tb_utf8_boundary(editor->buffer, editor->length, anchor, &boundary))
    {
        anchor = boundary;
    }
    editor->caret = caret;
    editor->anchor = anchor;
    field->blink = widget.gui->time;
    return true;
}

/* ---------------------------------------------------------------------------
 * Registration.
 * ------------------------------------------------------------------------- */

void tb_gui_register_value_kinds(tb_gui *gui)
{
    const tb_widget_class *const kinds[] = {tb_checkbutton_class(), tb_radiobutton_class(),
                                            tb_entry_class(),       tb_text_box_class(),
                                            tb_spinbox_class(),     tb_scale_class(),
                                            tb_progressbar_class(), tb_combobox_class()};

    for (size_t index = 0; index < TB_GUI_VALUES_COUNT_OF(kinds); index++)
    {
        (void)tb_gui_register_class(gui, kinds[index]);
    }
}
