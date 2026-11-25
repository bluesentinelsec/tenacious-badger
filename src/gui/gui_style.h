/* Themes as their sources share them: the classes and values a theme
 * holds, the names of the standard values, and the images a theme owns.
 * The GUI's side of styles is in gui_internal.h. */
#ifndef TB_GUI_STYLE_H
#define TB_GUI_STYLE_H

#include "gui_internal.h"

#include "badger/json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The standard values of a style, in the order of their names. */
typedef enum tb_style_standard
{
    TB_STYLE_STANDARD_BACKGROUND,
    TB_STYLE_STANDARD_BACKGROUND_IMAGE,
    TB_STYLE_STANDARD_BORDER_COLOR,
    TB_STYLE_STANDARD_BORDER_WIDTH,
    TB_STYLE_STANDARD_RADIUS,
    TB_STYLE_STANDARD_PADDING,
    TB_STYLE_STANDARD_FONT,
    TB_STYLE_STANDARD_TEXT_COLOR,
    TB_STYLE_STANDARD_FOCUS_COLOR,
    TB_STYLE_STANDARD_FOCUS_WIDTH,
    TB_STYLE_STANDARD_COUNT
} tb_style_standard;

/* A standard value's name and type. */
const char *tb_style_standard_name(tb_style_standard standard);
tb_style_type tb_style_standard_type(tb_style_standard standard);
/* The standard value of a name; TB_STYLE_STANDARD_COUNT for an element's. */
tb_style_standard tb_style_standard_of(const char *name);

/* The root class, where values every kind shares are kept. */
#define TB_STYLE_ROOT_CLASS "."

/* A style class of a theme: its values, each for a set of states. */
typedef struct tb_theme_class
{
    char *name; /* owned */
    tb_gui_style_entry *entries;
    size_t count;
    size_t capacity;
} tb_theme_class;

/* An image named by a path, for theme files. */
typedef struct tb_theme_image_name
{
    char *path; /* owned */
    const tb_image *image;
} tb_theme_image_name;

struct tb_theme
{
    tb_theme_class *classes;
    size_t class_count;
    size_t class_capacity;
    tb_theme_image_name *images;
    size_t image_count;
    size_t image_capacity;
    /* Images the theme made, destroyed with it on its renderer. */
    tb_renderer *renderer;
    tb_image **owned;
    size_t owned_count;
    size_t owned_capacity;
    uint64_t generation;
};

/* A class of a theme by its name, or NULL. */
const tb_theme_class *tb_theme_find_class(const tb_theme *theme, const char *name);
/* Move a theme's classes into another, in place of its own, which are
 * freed; the first is left empty. */
void tb_theme_take_classes(tb_theme *into, tb_theme *from);
/* Make an image the theme owns, on its renderer. */
const tb_image *tb_theme_make_image(tb_theme *theme, tb_renderer *renderer,
                                    const tb_image_desc *desc, const uint8_t *pixels, size_t size);
/* Whether a value may be set by a name: of a standard value's type, and of
 * some type; a font given no size takes its role's. */
bool tb_style_value_check(const char *name, const tb_style_value *value, tb_style_value *checked);

/* gui_theme_file.c: values read and written as theme files hold them, and
 * as layout files hold a widget's overrides. An image is found by the path
 * it is named by; NULL when nothing names it. */
typedef const tb_image *tb_style_image_finder(void *context, const char *path);
/* A key, "name" or "name:state+state": its name, copied into `name`, and
 * its states. False for a key of no name, a name too long, or a state of
 * no such name. */
bool tb_style_key_read(const char *key, size_t length, char *name, size_t capacity,
                       uint32_t *states);
/* A key of a name and states, its states in a fixed order. */
void tb_style_key_write(const char *name, uint32_t states, char *buffer, size_t capacity);
/* A value of a name: of its standard type, or of the type its shape says.
 * Returns NULL, or why it is refused. */
const char *tb_style_value_read(const char *name, const tb_json_value *json,
                                tb_style_image_finder *find, void *context, tb_style_value *value);
/* A value, as it reads back; an image by its path, which is required. */
tb_json_result tb_style_value_write(tb_json_writer *writer, const tb_style_value *value,
                                    const char *image_path);

/* gui_style.c: an image by the path one of a widget's themes names it by,
 * nearest first; and the path a widget's themes name an image by. NULL for
 * none. */
const tb_image *tb_gui_style_image_named(tb_widget widget, const char *path);
const char *tb_gui_style_image_path(tb_widget widget, const tb_image *image);

/* The tool theme's values, of a palette, into a theme (gui_theme_tool.c). */
void tb_theme_fill_tool(tb_theme *theme, const tb_ui_theme *palette);

#endif
