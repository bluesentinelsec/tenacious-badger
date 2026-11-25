/* Entries and text boxes: a line of text, and several, typed and edited in
 * a field (gui_field.c), each a kind of widget with its properties. */
#include "gui_values.h"

#include "badger/assert.h"

#include <stddef.h>

/* Characters wide, and lines high, unless chosen. */
#define TB_GUI_ENTRY_WIDTH 20
#define TB_GUI_TEXT_BOX_WIDTH 40
#define TB_GUI_TEXT_BOX_HEIGHT 6

static const char *const tb_gui_wrap_names[] = {"word", "none"};

/* An entry, and a text box: a field first, as tb_gui_field_of() finds it. */
typedef struct tb_gui_entry
{
    tb_gui_field field;
    int32_t width;
    int32_t height; /* a text box's lines */
    int32_t wrap;   /* a text box's tb_text_wrap */
    int32_t max_length;
} tb_gui_entry;

TB_STATIC_ASSERT(offsetof(tb_gui_entry, field) == 0, "a field is first in its kind's data");

/* Between a field's edges and its text, as between a button's and its. */
static tb_gui_edges tb_gui_entry_insets(tb_widget widget, const void *data)
{
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const float across = theme->frame_padding.x + theme->border;
    const float down = theme->frame_padding.y + theme->border;

    (void)data;
    return (tb_gui_edges){across, down, across, down};
}

/* Where a field's text is: inside its insets. */
static tb_canvas_rect tb_gui_entry_text_rect(tb_widget widget, tb_canvas_rect rect)
{
    const tb_gui_edges insets = tb_gui_entry_insets(widget, NULL);

    return (tb_canvas_rect){rect.x + insets.left, rect.y + insets.top,
                            rect.width - insets.left - insets.right,
                            rect.height - insets.top - insets.bottom};
}

static void tb_gui_entry_destroy(tb_widget widget, void *data)
{
    tb_gui_entry *const entry = data;

    (void)widget;
    tb_gui_field_free(&entry->field);
}

static void tb_gui_entry_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_entry *const entry = data;
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);

    tb_gui_values_draw_field(widget, paint, rect);
    tb_gui_field_draw(widget, &entry->field, paint, tb_gui_entry_text_rect(widget, rect),
                      tb_gui_field_shown_text(&entry->field));
}

static bool tb_gui_entry_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_entry *const entry = data;

    return tb_gui_field_event(widget, &entry->field, event,
                              tb_gui_entry_text_rect(widget, tb_widget_rect(widget)));
}

/* ---------------------------------------------------------------------------
 * Properties both have.
 * ------------------------------------------------------------------------- */

static bool tb_gui_entry_get_text(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_entry *const entry = data;

    (void)widget;
    *value = tb_gui_text(tb_gui_field_shown_text(&entry->field));
    return true;
}

static bool tb_gui_entry_set_text(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_entry *const entry = data;

    (void)widget;
    tb_gui_field_set_text(&entry->field, value->text);
    return true;
}

static bool tb_gui_entry_get_variable(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_entry *const entry = data;

    (void)widget;
    *value = tb_gui_variable(entry->field.variable);
    return true;
}

static bool tb_gui_entry_set_variable(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_entry *const entry = data;
    return tb_gui_field_bind(widget, &entry->field, value->variable);
}

static bool tb_gui_entry_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_entry *const entry = data;

    (void)widget;
    *value = tb_gui_int(entry->width);
    return true;
}

static bool tb_gui_entry_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_entry *const entry = data;

    (void)widget;
    if (value->integer < 1)
    {
        return false;
    }
    entry->width = value->integer;
    return true;
}

static bool tb_gui_entry_get_max(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_entry *const entry = data;

    (void)widget;
    *value = tb_gui_int(entry->max_length);
    return true;
}

static bool tb_gui_entry_set_max(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_entry *const entry = data;

    (void)widget;
    if (value->integer < 0)
    {
        return false;
    }
    entry->max_length = value->integer;
    entry->field.editor.max_characters = (size_t)value->integer;
    return true;
}

#define TB_GUI_ENTRY_TEXT                                                                          \
    {.name = "text",                                                                               \
     .type = TB_GUI_TYPE_TEXT,                                                                     \
     .get = tb_gui_entry_get_text,                                                                 \
     .set = tb_gui_entry_set_text}
#define TB_GUI_ENTRY_VARIABLE                                                                      \
    {.name = "variable",                                                                           \
     .type = TB_GUI_TYPE_VARIABLE,                                                                 \
     .get = tb_gui_entry_get_variable,                                                             \
     .set = tb_gui_entry_set_variable}
#define TB_GUI_ENTRY_WIDTH_PROPERTY                                                                \
    {.name = "width",                                                                              \
     .type = TB_GUI_TYPE_INT,                                                                      \
     .flags = TB_GUI_PROPERTY_LAYOUT,                                                              \
     .get = tb_gui_entry_get_width,                                                                \
     .set = tb_gui_entry_set_width}
#define TB_GUI_ENTRY_MAX                                                                           \
    {.name = "max_length",                                                                         \
     .type = TB_GUI_TYPE_INT,                                                                      \
     .get = tb_gui_entry_get_max,                                                                  \
     .set = tb_gui_entry_set_max}
#define TB_GUI_ENTRY_READ_ONLY                                                                     \
    {.name = "read_only",                                                                          \
     .type = TB_GUI_TYPE_BOOL,                                                                     \
     .offset = offsetof(tb_gui_entry, field.read_only)}

/* ---------------------------------------------------------------------------
 * Entries.
 * ------------------------------------------------------------------------- */

static void tb_gui_entry_init(tb_widget widget, void *data)
{
    tb_gui_entry *const entry = data;

    tb_gui_field_init(&entry->field, false);
    entry->width = TB_GUI_ENTRY_WIDTH;
    tb_widget_set_cursor(widget, TB_CURSOR_TEXT);
}

static tb_vec2 tb_gui_entry_measure(tb_widget widget, const void *data)
{
    const tb_gui_entry *const entry = data;

    return (tb_vec2){tb_gui_values_char_width(widget.gui, entry->width),
                     tb_gui_values_measures(widget.gui).line_height};
}

static const tb_gui_property tb_gui_entry_properties[] = {
    TB_GUI_ENTRY_TEXT,
    TB_GUI_ENTRY_VARIABLE,
    {.name = "placeholder",
     .type = TB_GUI_TYPE_TEXT,
     .offset = offsetof(tb_gui_entry, field.placeholder)},
    {.name = "show", .type = TB_GUI_TYPE_TEXT, .offset = offsetof(tb_gui_entry, field.show)},
    TB_GUI_ENTRY_WIDTH_PROPERTY,
    TB_GUI_ENTRY_MAX,
    TB_GUI_ENTRY_READ_ONLY,
    {.name = "validate",
     .type = TB_GUI_TYPE_COMMAND,
     .offset = offsetof(tb_gui_entry, field.validate)},
};

static const tb_widget_class tb_gui_entry_kind = {
    .name = "entry",
    .data_size = sizeof(tb_gui_entry),
    .properties = tb_gui_entry_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_entry_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK | TB_WIDGET_CLASS_ENGAGES,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_entry_init,
    .destroy = tb_gui_entry_destroy,
    .measure = tb_gui_entry_measure,
    .insets = tb_gui_entry_insets,
    .draw = tb_gui_entry_draw,
    .event = tb_gui_entry_event};

const tb_widget_class *tb_entry_class(void)
{
    return &tb_gui_entry_kind;
}

/* Set a property from a desc's text, when it has some. */
static void tb_gui_set_text_if(tb_widget widget, const char *name, const char *text)
{
    if (text != NULL)
    {
        const tb_gui_value value = tb_gui_text(tb_text_from_cstr(text));
        (void)tb_widget_set(widget, name, &value);
    }
}

tb_widget tb_entry(tb_widget parent, const tb_entry_desc *desc)
{
    const tb_entry_desc chosen = desc != NULL ? *desc : (tb_entry_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_entry_kind);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);
    const tb_gui_value max_length = tb_gui_int(chosen.max_length);
    const tb_gui_value read_only = tb_gui_bool(chosen.read_only);
    const tb_gui_value validate = tb_gui_command_of(chosen.validate, chosen.user);

    if (chosen.width > 0)
    {
        const tb_gui_value width = tb_gui_int(chosen.width);
        (void)tb_widget_set(widget, "width", &width);
    }
    tb_gui_set_text_if(widget, "text", chosen.text);
    (void)tb_widget_set(widget, "variable", &variable);
    tb_gui_set_text_if(widget, "placeholder", chosen.placeholder);
    tb_gui_set_text_if(widget, "show", chosen.show);
    (void)tb_widget_set(widget, "max_length", &max_length);
    (void)tb_widget_set(widget, "read_only", &read_only);
    (void)tb_widget_set(widget, "validate", &validate);
    return widget;
}

/* ---------------------------------------------------------------------------
 * Text boxes.
 * ------------------------------------------------------------------------- */

static void tb_gui_text_box_init(tb_widget widget, void *data)
{
    tb_gui_entry *const box = data;

    tb_gui_field_init(&box->field, true);
    box->width = TB_GUI_TEXT_BOX_WIDTH;
    box->height = TB_GUI_TEXT_BOX_HEIGHT;
    tb_widget_set_cursor(widget, TB_CURSOR_TEXT);
}

static tb_vec2 tb_gui_text_box_measure(tb_widget widget, const void *data)
{
    const tb_gui_entry *const box = data;

    return (tb_vec2){tb_gui_values_char_width(widget.gui, box->width),
                     tb_gui_values_measures(widget.gui).line_height * (float)box->height};
}

static bool tb_gui_text_box_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_entry *const box = data;

    (void)widget;
    *value = tb_gui_int(box->height);
    return true;
}

static bool tb_gui_text_box_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_entry *const box = data;

    (void)widget;
    if (value->integer < 1)
    {
        return false;
    }
    box->height = value->integer;
    return true;
}

static bool tb_gui_text_box_get_wrap(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_entry *const box = data;

    (void)widget;
    *value = tb_gui_enum(box->wrap);
    return true;
}

static bool tb_gui_text_box_set_wrap(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_entry *const box = data;

    (void)widget;
    box->wrap = value->integer;
    box->field.wraps = value->integer == TB_TEXT_WRAP_WORD;
    box->field.scroll.x = 0.0f;
    return true;
}

static const tb_gui_property tb_gui_text_box_properties[] = {
    TB_GUI_ENTRY_TEXT,
    TB_GUI_ENTRY_VARIABLE,
    TB_GUI_ENTRY_WIDTH_PROPERTY,
    {.name = "height",
     .type = TB_GUI_TYPE_INT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_text_box_get_height,
     .set = tb_gui_text_box_set_height},
    {.name = "wrap",
     .type = TB_GUI_TYPE_ENUM,
     .names = tb_gui_wrap_names,
     .name_count = TB_GUI_VALUES_COUNT_OF(tb_gui_wrap_names),
     .get = tb_gui_text_box_get_wrap,
     .set = tb_gui_text_box_set_wrap},
    TB_GUI_ENTRY_MAX,
    TB_GUI_ENTRY_READ_ONLY,
};

static const tb_widget_class tb_gui_text_box_kind = {
    .name = "text",
    .data_size = sizeof(tb_gui_entry),
    .properties = tb_gui_text_box_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_text_box_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK | TB_WIDGET_CLASS_ENGAGES,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_text_box_init,
    .destroy = tb_gui_entry_destroy,
    .measure = tb_gui_text_box_measure,
    .insets = tb_gui_entry_insets,
    .draw = tb_gui_entry_draw,
    .event = tb_gui_entry_event};

const tb_widget_class *tb_text_box_class(void)
{
    return &tb_gui_text_box_kind;
}

tb_widget tb_text_box(tb_widget parent, const tb_text_box_desc *desc)
{
    const tb_text_box_desc chosen = desc != NULL ? *desc : (tb_text_box_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_text_box_kind);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);
    const tb_gui_value wrap = tb_gui_enum((int32_t)chosen.wrap);
    const tb_gui_value max_length = tb_gui_int(chosen.max_length);
    const tb_gui_value read_only = tb_gui_bool(chosen.read_only);

    if (chosen.width > 0)
    {
        const tb_gui_value width = tb_gui_int(chosen.width);
        (void)tb_widget_set(widget, "width", &width);
    }
    if (chosen.height > 0)
    {
        const tb_gui_value height = tb_gui_int(chosen.height);
        (void)tb_widget_set(widget, "height", &height);
    }
    tb_gui_set_text_if(widget, "text", chosen.text);
    (void)tb_widget_set(widget, "variable", &variable);
    (void)tb_widget_set(widget, "wrap", &wrap);
    (void)tb_widget_set(widget, "max_length", &max_length);
    (void)tb_widget_set(widget, "read_only", &read_only);
    return widget;
}
