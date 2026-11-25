/* Themes as their sources share them: the classes and values a theme
 * holds, the names of the standard values, and the images a theme owns.
 * The GUI's side of styles is in gui_internal.h. */
#ifndef TB_GUI_STYLE_H
#define TB_GUI_STYLE_H

#include "gui_internal.h"

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
                                    const tb_image_desc *desc, const uint8_t *pixels,
                                    size_t size);
/* Whether a value may be set by a name: of a standard value's type, and of
 * some type; a font given no size takes its role's. */
bool tb_style_value_check(const char *name, const tb_style_value *value,
                          tb_style_value *checked);

/* The tool theme's values, of a palette, into a theme (gui_theme_tool.c). */
void tb_theme_fill_tool(tb_theme *theme, const tb_ui_theme *palette);

#endif
