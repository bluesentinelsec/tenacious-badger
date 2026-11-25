/* What the kinds that edit values share: variables read and set from text,
 * text being edited in a widget (an entry's, a text box's, a spin box's),
 * numbers written by a format, and the measures and colors they draw by. */
#ifndef TB_GUI_VALUES_H
#define TB_GUI_VALUES_H

#include "gui_internal.h"

#include "../ui/ui_editor.h"

#include "badger/font_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_GUI_VALUES_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Bytes of a number written by a format, its terminator included. */
#define TB_GUI_NUMBER_CAPACITY 64

/* ---------------------------------------------------------------------------
 * Variables, for the kinds (gui_var.c).
 * ------------------------------------------------------------------------- */

/* Set a variable from text that need not be terminated, as
 * tb_var_set_text() does. */
bool tb_gui_var_set_span(tb_var var, tb_text text);

/* Whether a variable holds the value text reads as, in its type: "1" for
 * a bool variable that is true, "2.50" for a number variable of 2.5. */
bool tb_gui_var_holds(tb_var var, tb_text text);

/* A variable property's value for a kind's own: none for a variable of
 * another GUI. */
bool tb_gui_var_of_widget(tb_widget widget, tb_var var);

/* ---------------------------------------------------------------------------
 * Numbers (gui_values.c).
 * ------------------------------------------------------------------------- */

/* Whether a format writes one number: text, then one conversion of a
 * `double` (`f`, `F`, `e`, `E`, `g`, `G`) with flags, a width, and a
 * precision, then text; `%%` writes a percent sign. "" is the shortest. */
bool tb_gui_format_valid(tb_text format);

/* Write a number by a format (checked) into a buffer of
 * TB_GUI_NUMBER_CAPACITY bytes; "" writes the fewest digits that read back
 * as it, after `digits` decimals when `digits` is not negative. */
tb_text tb_gui_format_number(tb_text format, double value, int32_t digits, char *buffer);

/* The decimals a step's digits have, for writing values that are its
 * multiples: 0.25 has 2, 10 has 0; at most 6. */
int32_t tb_gui_step_digits(double step);

/* A number read from text with spaces around it: false for none. */
bool tb_gui_read_number(tb_text text, double *value);

/* ---------------------------------------------------------------------------
 * What kinds draw by (gui_values.c).
 * ------------------------------------------------------------------------- */

/* A widget's font's measures at the view's scale, in units. */
tb_ui_font_measures tb_gui_values_measures(tb_widget widget);

/* Characters of "0" wide in a widget's font, in units. */
float tb_gui_values_char_width(tb_widget widget, int32_t characters);

/* The space between a widget's edges and its content: its style's padding
 * and border, at rest. */
tb_gui_edges tb_gui_values_insets(tb_widget widget);

/* One line of text in a paint's widget's font, with its left edge at `x`,
 * its capitals centered in a rectangle's height, clipped to `clip`. */
void tb_gui_values_paint_line(tb_gui_paint *paint, tb_text text, float x, tb_canvas_rect rect,
                              tb_canvas_rect clip, tb_vec4 color);

/* The top of a line of a widget's text whose capitals are centered in a
 * rectangle. */
float tb_gui_values_line_top(tb_widget widget, tb_canvas_rect rect);

/* The elements of the style of a kind that edits text in a field, first in
 * its elements, in this order. */
enum
{
    TB_GUI_FIELD_SELECTION,   /* selected text's background */
    TB_GUI_FIELD_PLACEHOLDER, /* the placeholder's color */
    TB_GUI_FIELD_CARET,       /* the caret's color */
    TB_GUI_FIELD_ELEMENTS
};
#define TB_GUI_FIELD_STYLE_ELEMENTS                                                                \
    [TB_GUI_FIELD_SELECTION] = {"selection", TB_STYLE_COLOR},                                      \
    [TB_GUI_FIELD_PLACEHOLDER] = {"placeholder_color", TB_STYLE_COLOR},                            \
    [TB_GUI_FIELD_CARET] = {"caret_color", TB_STYLE_COLOR}

/* The states the look of a kind follows. */
#define TB_GUI_VALUES_STATES                                                                       \
    (TB_WIDGET_HOVERED | TB_WIDGET_PRESSED | TB_WIDGET_FOCUSED | TB_WIDGET_DISABLED)

/* Whether a key event is a press of a key with none of Control, Alt, or
 * Command held. */
bool tb_gui_values_plain(const tb_gui_event *event);

/* Send a widget #TB_GUI_EVENT_CHANGED, then call a command, as a value
 * changed by input does. */
void tb_gui_values_changed(tb_widget widget, tb_gui_command command);

/* ---------------------------------------------------------------------------
 * Text being edited in a widget (gui_field.c).
 * ------------------------------------------------------------------------- */

/* A widget's text, its caret and selection, and how it is scrolled. The
 * buffer is the field's; a variable, when it has one, holds its text too. */
typedef struct tb_gui_field
{
    tb_ui_editor editor;
    tb_var variable;    /* a text variable it edits, or none */
    uint64_t synced;    /* the variable's version its text was taken at */
    tb_gui_string show; /* shown for each character in place of it */
    tb_gui_string placeholder;
    tb_gui_command validate;
    tb_vec2 scroll; /* units the text is moved left and up */
    double blink;   /* when the caret last moved */
    bool read_only;
    bool refused;  /* tb_gui_refuse_edit() was called for the edit proposed */
    bool dragging; /* the mouse selects */
    bool wraps;    /* lines break at the width */
    /* Edits are not set in a variable nor sent as changes: its kind takes
     * the text when it is done, as a spin box does on Enter. */
    bool deferred;
    bool dirty; /* edited since its kind last took the text */
} tb_gui_field;

/* Make and release a field's buffer. */
void tb_gui_field_init(tb_gui_field *field, bool multiline);
void tb_gui_field_free(tb_gui_field *field);

/* Its text, taken again from its variable when that changed elsewhere. */
void tb_gui_field_sync(tb_gui_field *field);
tb_text tb_gui_field_text(const tb_gui_field *field);

/* Set its text, as a program does: no event is sent; the variable is set. */
void tb_gui_field_set_text(tb_gui_field *field, tb_text text);

/* Edit it with a key, typed text, or the mouse: true when the event was
 * used. Changes are validated, set in its variable, and sent as
 * #TB_GUI_EVENT_CHANGED. `rect` is where its text is, in units. */
bool tb_gui_field_event(tb_widget widget, tb_gui_field *field, const tb_gui_event *event,
                        tb_canvas_rect rect);

/* Scroll it so the caret is seen in `rect`. */
void tb_gui_field_follow(tb_widget widget, tb_gui_field *field, tb_canvas_rect rect);

/* Draw text as the field's, with its selection and caret (with the focus),
 * in `rect`: its own, or what its kind shows while it is not edited. */
void tb_gui_field_draw(tb_widget widget, const tb_gui_field *field, tb_gui_paint *paint,
                       tb_canvas_rect rect, tb_text text);

/* The text a field shows: its variable's when that changed since the field
 * last took it. */
tb_text tb_gui_field_shown_text(const tb_gui_field *field);

/* Bind it to a variable: none, or a text variable. */
bool tb_gui_field_bind(tb_widget widget, tb_gui_field *field, tb_var var);

/* The field of a widget that edits text: its kind's, or NULL. */
tb_gui_field *tb_gui_field_of(tb_widget widget);

#endif
