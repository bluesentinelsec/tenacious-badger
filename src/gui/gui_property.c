/* Properties: the common ones every widget has, a kind's own, and reading
 * and setting either by name. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

static const char *const tb_gui_cursor_names[] = {"arrow",
                                                  "text",
                                                  "hand",
                                                  "crosshair",
                                                  "not_allowed",
                                                  "resize_horizontal",
                                                  "resize_vertical",
                                                  "resize_diagonal_down",
                                                  "resize_diagonal_up",
                                                  "resize_all",
                                                  "inherit"};
static const char *const tb_gui_filter_names[] = {"stop", "pass", "ignore"};
static const char *const tb_gui_sticky_names[] = {"n", "s", "e", "w"};
static const char *const tb_gui_anchor_names[] = {"center", "n",  "ne", "e", "se",
                                                  "s",      "sw", "w",  "nw"};
static const char *const tb_gui_wrap_names[] = {"horizontal", "vertical"};

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* ---------------------------------------------------------------------------
 * The common properties, read and set on the widget's slot.
 * ------------------------------------------------------------------------- */

static bool tb_gui_get_name(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_text(tb_gui_string_text(&tb_gui_node_of(widget)->name));
    return true;
}

static bool tb_gui_set_name(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_string_set(&tb_gui_node_of(widget)->name, value->text);
    return true;
}

static bool tb_gui_get_style(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_text(tb_gui_string_text(&tb_gui_node_of(widget)->style));
    return true;
}

static bool tb_gui_set_style(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_string_set(&tb_gui_node_of(widget)->style, value->text);
    return true;
}

static bool tb_gui_get_visible(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_bool(tb_gui_node_of(widget)->visible);
    return true;
}

static bool tb_gui_set_visible(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_node_of(widget)->visible = value->boolean;
    if (!value->boolean)
    {
        tb_widget_end_modal(widget);
        tb_gui_forget_popup(widget.gui, widget.index);
    }
    return true;
}

static bool tb_gui_get_enabled(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_bool(tb_gui_node_of(widget)->enabled);
    return true;
}

static bool tb_gui_set_enabled(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_node_of(widget)->enabled = value->boolean;
    return true;
}

static bool tb_gui_get_cursor(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_enum(tb_gui_node_of(widget)->cursor);
    return true;
}

static bool tb_gui_set_cursor(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_node_of(widget)->cursor = value->integer;
    return true;
}

static bool tb_gui_get_filter(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_enum((int32_t)tb_gui_node_of(widget)->mouse_filter);
    return true;
}

static bool tb_gui_set_filter(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_node_of(widget)->mouse_filter = (tb_gui_mouse_filter)value->integer;
    return true;
}

static bool tb_gui_get_anchor(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_enum(tb_gui_node_of(widget)->grid_anchor);
    return true;
}

static bool tb_gui_set_anchor(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    tb_gui_node_of(widget)->grid_anchor = value->integer;
    return true;
}

static bool tb_gui_get_gridded(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_bool(tb_gui_node_of(widget)->gridded);
    return true;
}

static bool tb_gui_set_gridded(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    if (!value->boolean)
    {
        tb_grid_forget(widget);
        return true;
    }
    return tb_grid(widget, &tb_gui_node_of(widget)->grid);
}

/* The grid's options of a widget, to be changed and placed again. */
static bool tb_gui_regrid(tb_widget widget, const tb_grid_options *options)
{
    return tb_grid(widget, options);
}

static bool tb_gui_get_grid_int(tb_widget widget, const char *name, tb_gui_value *value)
{
    const tb_grid_options *const grid = &tb_gui_node_of(widget)->grid;
    int32_t found = 0;

    if (tb_str_equal(name, "row"))
    {
        found = grid->row;
    }
    else if (tb_str_equal(name, "column"))
    {
        found = grid->column;
    }
    else if (tb_str_equal(name, "row_span"))
    {
        found = grid->row_span > 0 ? grid->row_span : 1;
    }
    else
    {
        found = grid->column_span > 0 ? grid->column_span : 1;
    }
    *value = tb_gui_int(found);
    return true;
}

static bool tb_gui_get_row(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_grid_int(widget, "row", value);
}

static bool tb_gui_get_column(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_grid_int(widget, "column", value);
}

static bool tb_gui_get_row_span(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_grid_int(widget, "row_span", value);
}

static bool tb_gui_get_column_span(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_grid_int(widget, "column_span", value);
}

static bool tb_gui_set_row(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.row = value->integer;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_set_column(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.column = value->integer;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_set_row_span(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    if (value->integer < 1)
    {
        return false;
    }
    options.row_span = value->integer;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_set_column_span(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    if (value->integer < 1)
    {
        return false;
    }
    options.column_span = value->integer;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_get_sticky(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_flags(tb_gui_node_of(widget)->grid.sticky);
    return true;
}

static bool tb_gui_set_sticky(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.sticky = value->flags;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_get_padx(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_vec2(tb_gui_node_of(widget)->grid.padx);
    return true;
}

static bool tb_gui_set_padx(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.padx = value->vec2;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_get_pady(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_vec2(tb_gui_node_of(widget)->grid.pady);
    return true;
}

static bool tb_gui_set_pady(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.pady = value->vec2;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_get_ipadx(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_float(tb_gui_node_of(widget)->grid.ipadx);
    return true;
}

static bool tb_gui_set_ipadx(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.ipadx = value->number;
    return tb_gui_regrid(widget, &options);
}

static bool tb_gui_get_ipady(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    *value = tb_gui_float(tb_gui_node_of(widget)->grid.ipady);
    return true;
}

static bool tb_gui_set_ipady(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_grid_options options = tb_gui_node_of(widget)->grid;

    (void)data;
    options.ipady = value->number;
    return tb_gui_regrid(widget, &options);
}

#define TB_GUI_COMMON(property_name, property_type, property_flags, getter, setter)                \
    {.name = (property_name),                                                                      \
     .type = (property_type),                                                                      \
     .flags = (property_flags),                                                                    \
     .get = (getter),                                                                              \
     .set = (setter)}

#define TB_GUI_COMMON_NAMED(property_name, property_flags, getter, setter, value_names)            \
    {.name = (property_name),                                                                      \
     .type = TB_GUI_TYPE_ENUM,                                                                     \
     .flags = (property_flags),                                                                    \
     .names = (value_names),                                                                       \
     .name_count = TB_GUI_COUNT_OF(value_names),                                                   \
     .get = (getter),                                                                              \
     .set = (setter)}

static const tb_gui_property tb_gui_common[] = {
    TB_GUI_COMMON("name", TB_GUI_TYPE_TEXT, 0, tb_gui_get_name, tb_gui_set_name),
    TB_GUI_COMMON("style", TB_GUI_TYPE_TEXT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_style,
                  tb_gui_set_style),
    TB_GUI_COMMON("visible", TB_GUI_TYPE_BOOL, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_visible,
                  tb_gui_set_visible),
    TB_GUI_COMMON("enabled", TB_GUI_TYPE_BOOL, 0, tb_gui_get_enabled, tb_gui_set_enabled),
    TB_GUI_COMMON_NAMED("cursor", 0, tb_gui_get_cursor, tb_gui_set_cursor, tb_gui_cursor_names),
    TB_GUI_COMMON_NAMED("mouse_filter", 0, tb_gui_get_filter, tb_gui_set_filter,
                        tb_gui_filter_names),
    TB_GUI_COMMON("gridded", TB_GUI_TYPE_BOOL, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_gridded,
                  tb_gui_set_gridded),
    TB_GUI_COMMON("row", TB_GUI_TYPE_INT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_row, tb_gui_set_row),
    TB_GUI_COMMON("column", TB_GUI_TYPE_INT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_column,
                  tb_gui_set_column),
    TB_GUI_COMMON("row_span", TB_GUI_TYPE_INT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_row_span,
                  tb_gui_set_row_span),
    TB_GUI_COMMON("column_span", TB_GUI_TYPE_INT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_column_span,
                  tb_gui_set_column_span),
    {.name = "sticky",
     .type = TB_GUI_TYPE_FLAGS,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .names = tb_gui_sticky_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_sticky_names),
     .get = tb_gui_get_sticky,
     .set = tb_gui_set_sticky},
    TB_GUI_COMMON("padx", TB_GUI_TYPE_VEC2, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_padx,
                  tb_gui_set_padx),
    TB_GUI_COMMON("pady", TB_GUI_TYPE_VEC2, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_pady,
                  tb_gui_set_pady),
    TB_GUI_COMMON("ipadx", TB_GUI_TYPE_FLOAT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_ipadx,
                  tb_gui_set_ipadx),
    TB_GUI_COMMON("ipady", TB_GUI_TYPE_FLOAT, TB_GUI_PROPERTY_LAYOUT, tb_gui_get_ipady,
                  tb_gui_set_ipady),
    TB_GUI_COMMON_NAMED("grid_anchor", TB_GUI_PROPERTY_LAYOUT, tb_gui_get_anchor, tb_gui_set_anchor,
                        tb_gui_anchor_names),
    TB_GUI_COMMON("default_focus", TB_GUI_TYPE_BOOL, 0, tb_gui_get_default_focus,
                  tb_gui_set_default_focus),
    {.name = "focus_wrap",
     .type = TB_GUI_TYPE_FLAGS,
     .names = tb_gui_wrap_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_wrap_names),
     .get = tb_gui_get_focus_wrap,
     .set = tb_gui_set_focus_wrap},
    TB_GUI_COMMON("focus_up", TB_GUI_TYPE_TEXT, 0, tb_gui_get_focus_up, tb_gui_set_focus_up),
    TB_GUI_COMMON("focus_down", TB_GUI_TYPE_TEXT, 0, tb_gui_get_focus_down, tb_gui_set_focus_down),
    TB_GUI_COMMON("focus_left", TB_GUI_TYPE_TEXT, 0, tb_gui_get_focus_left, tb_gui_set_focus_left),
    TB_GUI_COMMON("focus_right", TB_GUI_TYPE_TEXT, 0, tb_gui_get_focus_right,
                  tb_gui_set_focus_right),
    TB_GUI_COMMON("focus_next", TB_GUI_TYPE_TEXT, 0, tb_gui_get_focus_next, tb_gui_set_focus_next),
    TB_GUI_COMMON("focus_previous", TB_GUI_TYPE_TEXT, 0, tb_gui_get_focus_previous,
                  tb_gui_set_focus_previous),
};

const tb_gui_property *tb_gui_common_properties(size_t *count)
{
    TB_ASSERT(count != NULL);
    *count = TB_GUI_COUNT_OF(tb_gui_common);
    return tb_gui_common;
}

/* ---------------------------------------------------------------------------
 * Finding, reading, and setting.
 * ------------------------------------------------------------------------- */

const tb_gui_property *tb_gui_property_find(const tb_gui_node *node, const char *name)
{
    for (size_t index = 0; index < TB_GUI_COUNT_OF(tb_gui_common); index++)
    {
        if (tb_str_equal(tb_gui_common[index].name, name))
        {
            return &tb_gui_common[index];
        }
    }
    for (size_t index = 0; index < node->kind->property_count; index++)
    {
        if (tb_str_equal(node->kind->properties[index].name, name))
        {
            return &node->kind->properties[index];
        }
    }
    return NULL;
}

size_t tb_widget_property_count(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? TB_GUI_COUNT_OF(tb_gui_common) + node->kind->property_count : 0;
}

const tb_gui_property *tb_widget_property_at(tb_widget widget, size_t index)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);

    if (node == NULL)
    {
        return NULL;
    }
    if (index < TB_GUI_COUNT_OF(tb_gui_common))
    {
        return &tb_gui_common[index];
    }
    index -= TB_GUI_COUNT_OF(tb_gui_common);
    return index < node->kind->property_count ? &node->kind->properties[index] : NULL;
}

const tb_gui_property *tb_widget_find_property(tb_widget widget, const char *name)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);

    TB_ASSERT(name != NULL);
    return node != NULL ? tb_gui_property_find(node, name) : NULL;
}

/* A value read from a kind's data at a property's offset. */
static tb_gui_value tb_gui_read_offset(const tb_gui_property *property, const void *data)
{
    const char *const at = (const char *)data + property->offset;
    tb_gui_value value = {.type = property->type};

    switch (property->type)
    {
    case TB_GUI_TYPE_BOOL:
        memcpy(&value.boolean, at, sizeof(value.boolean));
        break;
    case TB_GUI_TYPE_INT:
    case TB_GUI_TYPE_ENUM:
        memcpy(&value.integer, at, sizeof(value.integer));
        break;
    case TB_GUI_TYPE_FLAGS:
        memcpy(&value.flags, at, sizeof(value.flags));
        break;
    case TB_GUI_TYPE_FLOAT:
        memcpy(&value.number, at, sizeof(value.number));
        break;
    case TB_GUI_TYPE_VEC2:
        memcpy(&value.vec2, at, sizeof(value.vec2));
        break;
    case TB_GUI_TYPE_COLOR:
        memcpy(&value.color, at, sizeof(value.color));
        break;
    case TB_GUI_TYPE_TEXT:
        value.text = tb_gui_string_text((const tb_gui_string *)(const void *)at);
        break;
    case TB_GUI_TYPE_FONT:
        memcpy(&value.font, at, sizeof(value.font));
        break;
    case TB_GUI_TYPE_COMMAND:
        memcpy(&value.command, at, sizeof(value.command));
        break;
    case TB_GUI_TYPE_VARIABLE:
        memcpy(&value.variable, at, sizeof(value.variable));
        break;
    case TB_GUI_TYPE_COUNT:
        TB_ASSERT_MSG(false, "a property of no type");
        break;
    }
    return value;
}

/* A value written into a kind's data at a property's offset. */
static void tb_gui_write_offset(const tb_gui_property *property, void *data,
                                const tb_gui_value *value)
{
    char *const at = (char *)data + property->offset;

    switch (property->type)
    {
    case TB_GUI_TYPE_BOOL:
        memcpy(at, &value->boolean, sizeof(value->boolean));
        break;
    case TB_GUI_TYPE_INT:
    case TB_GUI_TYPE_ENUM:
        memcpy(at, &value->integer, sizeof(value->integer));
        break;
    case TB_GUI_TYPE_FLAGS:
        memcpy(at, &value->flags, sizeof(value->flags));
        break;
    case TB_GUI_TYPE_FLOAT:
        memcpy(at, &value->number, sizeof(value->number));
        break;
    case TB_GUI_TYPE_VEC2:
        memcpy(at, &value->vec2, sizeof(value->vec2));
        break;
    case TB_GUI_TYPE_COLOR:
        memcpy(at, &value->color, sizeof(value->color));
        break;
    case TB_GUI_TYPE_TEXT:
        tb_gui_string_set((tb_gui_string *)(void *)at, value->text);
        break;
    case TB_GUI_TYPE_FONT:
        memcpy(at, &value->font, sizeof(value->font));
        break;
    case TB_GUI_TYPE_COMMAND:
        memcpy(at, &value->command, sizeof(value->command));
        break;
    case TB_GUI_TYPE_VARIABLE:
        memcpy(at, &value->variable, sizeof(value->variable));
        break;
    case TB_GUI_TYPE_COUNT:
        TB_ASSERT_MSG(false, "a property of no type");
        break;
    }
}

/* Whether a value is one its property takes: of its type, a name's index,
 * bits of its names, finite numbers. */
static bool tb_gui_value_fits(const tb_gui_property *property, const tb_gui_value *value)
{
    if (value->type != property->type)
    {
        return false;
    }
    switch (property->type)
    {
    case TB_GUI_TYPE_ENUM:
        return value->integer >= 0 && (size_t)value->integer < property->name_count;
    case TB_GUI_TYPE_FLAGS:
        return property->name_count >= 32 ||
               (value->flags & ~((1u << property->name_count) - 1u)) == 0;
    case TB_GUI_TYPE_FLOAT:
        return isfinite(value->number) != 0;
    case TB_GUI_TYPE_VEC2:
        return isfinite(value->vec2.x) != 0 && isfinite(value->vec2.y) != 0;
    case TB_GUI_TYPE_TEXT:
        return value->text.bytes != NULL || value->text.length == 0;
    case TB_GUI_TYPE_VARIABLE:
        /* None, or a variable alive. */
        return value->variable.gui == NULL || tb_var_alive(value->variable);
    case TB_GUI_TYPE_BOOL:
    case TB_GUI_TYPE_INT:
    case TB_GUI_TYPE_COLOR:
    case TB_GUI_TYPE_FONT:
    case TB_GUI_TYPE_COMMAND:
    case TB_GUI_TYPE_COUNT:
        break;
    }
    return true;
}

bool tb_widget_get(tb_widget widget, const char *name, tb_gui_value *value)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    const tb_gui_property *property = NULL;

    TB_ASSERT(name != NULL);
    TB_ASSERT(value != NULL);
    if (node == NULL)
    {
        return false;
    }
    property = tb_gui_property_find(node, name);
    if (property == NULL)
    {
        return false;
    }
    if (property->get != NULL)
    {
        return property->get(widget, node->data, value);
    }
    *value = tb_gui_read_offset(property, node->data);
    return true;
}

bool tb_widget_set(tb_widget widget, const char *name, const tb_gui_value *value)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    const tb_gui_property *property = NULL;
    bool done = true;

    TB_ASSERT(name != NULL);
    TB_ASSERT(value != NULL);
    if (node == NULL)
    {
        return false;
    }
    property = tb_gui_property_find(node, name);
    if (property == NULL || (property->flags & TB_GUI_PROPERTY_READ_ONLY) != 0 ||
        !tb_gui_value_fits(property, value))
    {
        return false;
    }
    if (property->set != NULL)
    {
        done = property->set(widget, node->data, value);
    }
    else
    {
        tb_gui_write_offset(property, node->data, value);
    }
    if (done && (property->flags & TB_GUI_PROPERTY_LAYOUT) != 0)
    {
        tb_gui_mark_layout(widget.gui);
    }
    return done;
}

/* ---------------------------------------------------------------------------
 * Shorthand.
 * ------------------------------------------------------------------------- */

bool tb_widget_set_text(tb_widget widget, const char *text)
{
    const tb_gui_value value = tb_gui_text(tb_text_from_cstr(text));

    TB_ASSERT(text != NULL);
    return tb_widget_set(widget, "text", &value);
}

static tb_text tb_gui_text_of(tb_widget widget, const char *name)
{
    tb_gui_value value = {0};

    if (!tb_widget_get(widget, name, &value) || value.type != TB_GUI_TYPE_TEXT)
    {
        return tb_text_make("", 0);
    }
    return value.text;
}

tb_text tb_widget_text(tb_widget widget)
{
    return tb_gui_text_of(widget, "text");
}

void tb_widget_set_enabled(tb_widget widget, bool enabled)
{
    const tb_gui_value value = tb_gui_bool(enabled);
    (void)tb_widget_set(widget, "enabled", &value);
}

bool tb_widget_enabled(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL && node->enabled;
}

void tb_widget_set_visible(tb_widget widget, bool visible)
{
    const tb_gui_value value = tb_gui_bool(visible);
    (void)tb_widget_set(widget, "visible", &value);
}

bool tb_widget_visible(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL && node->visible;
}

void tb_widget_set_style(tb_widget widget, const char *style)
{
    TB_ASSERT(style != NULL);
    const tb_gui_value value = tb_gui_text(tb_text_from_cstr(style));
    (void)tb_widget_set(widget, "style", &value);
}

tb_text tb_widget_style(tb_widget widget)
{
    return tb_gui_text_of(widget, "style");
}

void tb_widget_set_name(tb_widget widget, const char *name)
{
    TB_ASSERT(name != NULL);
    const tb_gui_value value = tb_gui_text(tb_text_from_cstr(name));
    (void)tb_widget_set(widget, "name", &value);
}

tb_text tb_widget_name(tb_widget widget)
{
    return tb_gui_text_of(widget, "name");
}

void tb_widget_set_cursor(tb_widget widget, tb_cursor cursor)
{
    const tb_gui_value value = tb_gui_enum((int32_t)cursor);
    (void)tb_widget_set(widget, "cursor", &value);
}

void tb_widget_set_mouse_filter(tb_widget widget, tb_gui_mouse_filter filter)
{
    const tb_gui_value value = tb_gui_enum((int32_t)filter);
    (void)tb_widget_set(widget, "mouse_filter", &value);
}
