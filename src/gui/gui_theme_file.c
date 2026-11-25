/* Theme files: a theme's classes and values read from JSON (docs/gui.md,
 * "Theme files"), all or nothing, with the layout files' errors: a JSON
 * path, a line and a column, and a reason (gui_file.c). Values are read and
 * written here for theme files and for the style overrides layout files
 * hold. Theme files are watched with the layout files (gui_watch.c). */
#include "gui_style.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* The deepest a theme file nests: an image's insets in a value in a class. */
#define TB_THEME_FILE_JSON_DEPTH 8u
/* The most a font's weight is. */
#define TB_THEME_MOST_WEIGHT 1000
/* The largest number of units a value is: more is a mistake. */
#define TB_THEME_MOST_UNITS 1.0e6
/* The largest value of a channel of a color written as hex. */
#define TB_THEME_CHANNEL_MAX 255.0f
/* Bytes of a color written "#rrggbbaa", and of a key written. */
#define TB_THEME_COLOR_TEXT 10u

/* ---------------------------------------------------------------------------
 * Values.
 * ------------------------------------------------------------------------- */

static int tb_theme_hex_digit(char digit)
{
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F')
    {
        return digit - 'A' + 10;
    }
    return -1;
}

/* A color written "#rrggbb" or "#rrggbbaa". */
static bool tb_theme_read_color(const tb_json_value *value, tb_vec4 *color)
{
    size_t length = 0;
    const char *const text =
        tb_json_type_of(value) == TB_JSON_STRING ? tb_json_as_text(value, &length) : NULL;
    float channels[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    if (text == NULL || (length != 7 && length != 9) || text[0] != '#')
    {
        return false;
    }
    for (size_t channel = 0; channel < (length - 1) / 2; channel++)
    {
        const int high = tb_theme_hex_digit(text[1 + channel * 2]);
        const int low = tb_theme_hex_digit(text[2 + channel * 2]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        channels[channel] = (float)(high * 16 + low) / TB_THEME_CHANNEL_MAX;
    }
    *color = (tb_vec4){channels[0], channels[1], channels[2], channels[3]};
    return true;
}

static bool tb_theme_read_number(const tb_json_value *value, float *number)
{
    double read = 0.0;

    if (tb_json_type_of(value) != TB_JSON_NUMBER || !tb_json_as_double(value, &read) ||
        !isfinite(read) || fabs(read) > TB_THEME_MOST_UNITS)
    {
        return false;
    }
    *number = (float)read;
    return true;
}

/* Edges: one number for every side, two for across and down, or four:
 * left, top, right, and bottom. */
static bool tb_theme_read_edges(const tb_json_value *value, tb_gui_edges *edges)
{
    float numbers[4] = {0};
    const size_t count = tb_json_count(value);

    if (tb_json_type_of(value) == TB_JSON_NUMBER)
    {
        if (!tb_theme_read_number(value, &numbers[0]))
        {
            return false;
        }
        *edges = (tb_gui_edges){numbers[0], numbers[0], numbers[0], numbers[0]};
        return true;
    }
    if (tb_json_type_of(value) != TB_JSON_ARRAY || (count != 2 && count != 4))
    {
        return false;
    }
    for (size_t index = 0; index < count; index++)
    {
        if (!tb_theme_read_number(tb_json_at(value, index), &numbers[index]))
        {
            return false;
        }
    }
    *edges = count == 2 ? (tb_gui_edges){numbers[0], numbers[1], numbers[0], numbers[1]}
                        : (tb_gui_edges){numbers[0], numbers[1], numbers[2], numbers[3]};
    return true;
}

static bool tb_theme_edges_unsigned(tb_gui_edges edges)
{
    return edges.left >= 0.0f && edges.top >= 0.0f && edges.right >= 0.0f && edges.bottom >= 0.0f;
}

static const char *const tb_theme_role_names[TB_FONT_ROLE_COUNT] = {
    [TB_FONT_ROLE_UI] = "ui",
    [TB_FONT_ROLE_BODY] = "body",
    [TB_FONT_ROLE_HEADING] = "heading",
    [TB_FONT_ROLE_MONOSPACE] = "monospace",
    [TB_FONT_ROLE_PIXEL] = "pixel"};

/* Every key of an object is one of a list. */
static bool tb_theme_known_keys(const tb_json_value *object, const char *const *names, size_t count)
{
    for (size_t member = 0; member < tb_json_count(object); member++)
    {
        const char *key = NULL;
        size_t length = 0;
        bool known = false;
        (void)tb_json_member_at(object, member, &key, &length);
        for (size_t index = 0; index < count; index++)
        {
            known = known || tb_str_equal(names[index], key);
        }
        if (!known)
        {
            return false;
        }
    }
    return true;
}

static const char *tb_theme_read_font(const tb_json_value *value, tb_font_spec *font)
{
    static const char *const keys[] = {"role", "weight", "italic", "size"};
    const tb_json_value *const role = tb_json_find(value, "role", 4);
    const tb_json_value *const weight = tb_json_find(value, "weight", 6);
    const tb_json_value *const italic = tb_json_find(value, "italic", 6);
    const tb_json_value *const size = tb_json_find(value, "size", 4);
    size_t length = 0;
    int64_t read_weight = TB_FONT_WEIGHT_REGULAR;

    if (!tb_theme_known_keys(value, keys, sizeof(keys) / sizeof(keys[0])))
    {
        return "a font of a key other than role, weight, italic, and size";
    }
    const char *const role_name = role != NULL && tb_json_type_of(role) == TB_JSON_STRING
                                      ? tb_json_as_text(role, &length)
                                      : NULL;
    *font = (tb_font_spec){.role = TB_FONT_ROLE_COUNT};
    for (size_t index = 0; role_name != NULL && index < TB_FONT_ROLE_COUNT; index++)
    {
        if (tb_str_equal(role_name, tb_theme_role_names[index]))
        {
            font->role = (tb_font_role)index;
        }
    }
    if (font->role == TB_FONT_ROLE_COUNT)
    {
        return "a font's role: ui, body, heading, monospace, or pixel";
    }
    /* A weight of 0 is the style's font: the GUI's own. */
    if (weight != NULL && (!tb_json_as_int64(weight, &read_weight) || read_weight < 0 ||
                           read_weight > TB_THEME_MOST_WEIGHT))
    {
        return "a font's weight: from 0 to 1000";
    }
    font->weight = (uint16_t)read_weight;
    if (italic != NULL && !tb_json_as_bool(italic, &font->italic))
    {
        return "a font's italic: true or false";
    }
    if (size != NULL && (!tb_theme_read_number(size, &font->size) || font->size < 0.0f))
    {
        return "a font's size: 0 or more units";
    }
    return NULL;
}

/* A count of texels, whole and not negative, at most `most`. */
static bool tb_theme_read_texels(const tb_json_value *value, int64_t most, uint32_t *texels)
{
    int64_t read = 0;

    if (!tb_json_as_int64(value, &read) || read < 0 || read > most)
    {
        return false;
    }
    *texels = (uint32_t)read;
    return true;
}

static bool tb_theme_read_insets(const tb_json_value *value, tb_canvas_nine_slice *slice)
{
    uint32_t texels[4] = {0};
    const size_t count = tb_json_type_of(value) == TB_JSON_ARRAY ? tb_json_count(value) : 1u;

    if (count != 1 && count != 2 && count != 4)
    {
        return false;
    }
    for (size_t index = 0; index < count; index++)
    {
        const tb_json_value *const item = count == 1 ? value : tb_json_at(value, index);
        if (!tb_theme_read_texels(item, INT32_MAX, &texels[index]))
        {
            return false;
        }
    }
    slice->left = texels[0];
    slice->top = count == 1 ? texels[0] : texels[1];
    slice->right = count == 4 ? texels[2] : texels[0];
    slice->bottom = count == 4 ? texels[3] : slice->top;
    return true;
}

static bool tb_theme_read_source(const tb_json_value *value, tb_rhi_rect *source)
{
    uint32_t numbers[4] = {0};

    if (tb_json_type_of(value) != TB_JSON_ARRAY || tb_json_count(value) != 4)
    {
        return false;
    }
    for (size_t index = 0; index < 4; index++)
    {
        if (!tb_theme_read_texels(tb_json_at(value, index), INT32_MAX, &numbers[index]))
        {
            return false;
        }
    }
    *source = (tb_rhi_rect){(int32_t)numbers[0], (int32_t)numbers[1], numbers[2], numbers[3]};
    return true;
}

static const char *tb_theme_read_image(const tb_json_value *value, tb_style_image_finder *find,
                                       void *context, tb_style_image *image)
{
    static const char *const keys[] = {"image",        "insets", "source", "scale",
                                       "whole_pixels", "hollow", "tint"};
    const tb_json_value *const path = tb_json_find(value, "image", 5);
    const tb_json_value *const insets = tb_json_find(value, "insets", 6);
    const tb_json_value *const source = tb_json_find(value, "source", 6);
    const tb_json_value *const scale = tb_json_find(value, "scale", 5);
    const tb_json_value *const whole = tb_json_find(value, "whole_pixels", 12);
    const tb_json_value *const hollow = tb_json_find(value, "hollow", 6);
    const tb_json_value *const tint = tb_json_find(value, "tint", 4);
    size_t length = 0;

    *image = (tb_style_image){.tint = {1.0f, 1.0f, 1.0f, 1.0f}};
    if (!tb_theme_known_keys(value, keys, sizeof(keys) / sizeof(keys[0])))
    {
        return "an image of a key other than image, insets, source, scale, whole_pixels, "
               "hollow, and tint";
    }
    const char *const name = path != NULL && tb_json_type_of(path) == TB_JSON_STRING
                                 ? tb_json_as_text(path, &length)
                                 : NULL;
    if (name == NULL || length == 0)
    {
        return "an image's path: a VFS path";
    }
    image->slice.image = find(context, name);
    if (image->slice.image == NULL)
    {
        return "an image no theme names by that path (tb_theme_add_image())";
    }
    if (insets != NULL && !tb_theme_read_insets(insets, &image->slice))
    {
        return "an image's insets: one, two, or four whole numbers of texels";
    }
    if (source != NULL && !tb_theme_read_source(source, &image->slice.source))
    {
        return "an image's source: [x, y, width, height] in texels";
    }
    if (scale != NULL &&
        (!tb_theme_read_number(scale, &image->slice.scale) || !(image->slice.scale > 0.0f)))
    {
        return "an image's scale: more than 0";
    }
    if ((whole != NULL && !tb_json_as_bool(whole, &image->slice.whole_pixels)) ||
        (hollow != NULL && !tb_json_as_bool(hollow, &image->slice.hollow)))
    {
        return "an image's whole_pixels and hollow: true or false";
    }
    if (tint != NULL && !tb_theme_read_color(tint, &image->tint))
    {
        return "an image's tint: a color, \"#rrggbb\" or \"#rrggbbaa\"";
    }
    return NULL;
}

/* The type a value of no standard name has, by its shape. */
static tb_style_type tb_theme_shape_of(const tb_json_value *value)
{
    switch (tb_json_type_of(value))
    {
    case TB_JSON_STRING:
        return TB_STYLE_COLOR;
    case TB_JSON_NUMBER:
        return TB_STYLE_NUMBER;
    case TB_JSON_ARRAY:
        return TB_STYLE_EDGES;
    case TB_JSON_OBJECT:
        return tb_json_find(value, "image", 5) != NULL ? TB_STYLE_IMAGE : TB_STYLE_FONT;
    case TB_JSON_NULL:
    case TB_JSON_BOOL:
        break;
    }
    return TB_STYLE_NONE;
}

/* Sizes that must not be negative. */
static bool tb_theme_unsigned_name(const char *name)
{
    return tb_str_equal(name, "border_width") || tb_str_equal(name, "radius") ||
           tb_str_equal(name, "focus_width") || tb_str_equal(name, "padding");
}

const char *tb_style_value_read(const char *name, const tb_json_value *json,
                                tb_style_image_finder *find, void *context, tb_style_value *value)
{
    const tb_style_standard standard = tb_style_standard_of(name);
    const tb_style_type type = standard != TB_STYLE_STANDARD_COUNT
                                   ? tb_style_standard_type(standard)
                                   : tb_theme_shape_of(json);

    *value = (tb_style_value){.type = type};
    switch (type)
    {
    case TB_STYLE_COLOR:
        return tb_theme_read_color(json, &value->color) ? NULL
                                                        : "a color: \"#rrggbb\" or \"#rrggbbaa\"";
    case TB_STYLE_NUMBER:
        if (!tb_theme_read_number(json, &value->number) ||
            (tb_theme_unsigned_name(name) && value->number < 0.0f))
        {
            return "a number of units";
        }
        return NULL;
    case TB_STYLE_EDGES:
        if (!tb_theme_read_edges(json, &value->edges) ||
            (tb_theme_unsigned_name(name) && !tb_theme_edges_unsigned(value->edges)))
        {
            return "edges: a number, [across, down], or [left, top, right, bottom]";
        }
        return NULL;
    case TB_STYLE_FONT:
        if (tb_json_type_of(json) != TB_JSON_OBJECT)
        {
            return "a font: an object with a role";
        }
        return tb_theme_read_font(json, &value->font);
    case TB_STYLE_IMAGE:
        if (tb_json_type_of(json) != TB_JSON_OBJECT)
        {
            return "an image: an object with an image path";
        }
        return tb_theme_read_image(json, find, context, &value->image);
    case TB_STYLE_NONE:
        break;
    }
    return "a value: a color, a number, edges, a font, or an image";
}

static const char *const tb_theme_state_names[TB_STYLE_STATE_BITS] = {
    "hovered", "pressed", "focused", "disabled", "focus_visible", "engaged", "selected"};
static const uint32_t tb_theme_state_flags[TB_STYLE_STATE_BITS] = {
    TB_WIDGET_HOVERED,       TB_WIDGET_PRESSED, TB_WIDGET_FOCUSED, TB_WIDGET_DISABLED,
    TB_WIDGET_FOCUS_VISIBLE, TB_WIDGET_ENGAGED, TB_WIDGET_SELECTED};

/* The flag a state's name of a length is, or 0. */
static uint32_t tb_theme_state_of(const char *word, size_t length)
{
    for (size_t index = 0; index < TB_STYLE_STATE_BITS; index++)
    {
        if (tb_str_length(tb_theme_state_names[index]) == length &&
            memcmp(tb_theme_state_names[index], word, length) == 0)
        {
            return tb_theme_state_flags[index];
        }
    }
    return 0;
}

bool tb_style_key_read(const char *key, size_t length, char *name, size_t capacity,
                       uint32_t *states)
{
    const char *const colon = memchr(key, ':', length);
    const size_t name_length = colon != NULL ? (size_t)(colon - key) : length;

    *states = 0;
    if (name_length == 0 || name_length >= capacity)
    {
        return false;
    }
    memcpy(name, key, name_length);
    name[name_length] = '\0';
    if (colon == NULL)
    {
        return true;
    }
    const char *at = colon + 1;
    const char *const end = key + length;
    while (at <= end)
    {
        const char *const plus = memchr(at, '+', (size_t)(end - at));
        const char *const stop = plus != NULL ? plus : end;
        const uint32_t state = tb_theme_state_of(at, (size_t)(stop - at));
        if (state == 0)
        {
            return false;
        }
        *states |= state;
        at = stop + 1;
    }
    return true;
}

void tb_style_key_write(const char *name, uint32_t states, char *buffer, size_t capacity)
{
    size_t used = 0;
    const char *joiner = ":";

    TB_ASSERT(capacity > 0);
    buffer[0] = '\0';
    tb_gui_append(buffer, capacity, &used, name);
    for (size_t index = 0; index < TB_STYLE_STATE_BITS; index++)
    {
        if ((states & tb_theme_state_flags[index]) != 0)
        {
            tb_gui_append(buffer, capacity, &used, joiner);
            tb_gui_append(buffer, capacity, &used, tb_theme_state_names[index]);
            joiner = "+";
        }
    }
}

/* ---------------------------------------------------------------------------
 * Writing values.
 * ------------------------------------------------------------------------- */

/* The first failure of a run of writes is kept. */
static void tb_theme_keep(tb_json_result *kept, tb_json_result result)
{
    if (*kept == TB_JSON_OK)
    {
        *kept = result;
    }
}

static void tb_theme_write_number(tb_json_writer *writer, float value, tb_json_result *kept)
{
    char text[TB_GUI_FLOAT_TEXT] = {0};
    const size_t length = tb_gui_float_text(value, text, sizeof(text));

    tb_theme_keep(kept, tb_json_write_number_text(writer, text, length));
}

static void tb_theme_write_key(tb_json_writer *writer, const char *key, tb_json_result *kept)
{
    tb_theme_keep(kept, tb_json_write_key(writer, key, tb_str_length(key)));
}

static uint32_t tb_theme_channel(float value)
{
    const float clamped = fminf(fmaxf(value, 0.0f), 1.0f);
    return (uint32_t)lroundf(clamped * TB_THEME_CHANNEL_MAX);
}

/* "#rrggbb", or "#rrggbbaa" when it is not opaque. */
static void tb_theme_write_color(tb_json_writer *writer, tb_vec4 color, tb_json_result *kept)
{
    char text[TB_THEME_COLOR_TEXT] = {0};
    const uint32_t alpha = tb_theme_channel(color.w);
    size_t length = 0;

    if (alpha == (uint32_t)TB_THEME_CHANNEL_MAX)
    {
        length = tb_fmt_buffer(text, sizeof(text), "#%02x%02x%02x", tb_theme_channel(color.x),
                               tb_theme_channel(color.y), tb_theme_channel(color.z));
    }
    else
    {
        length = tb_fmt_buffer(text, sizeof(text), "#%02x%02x%02x%02x", tb_theme_channel(color.x),
                               tb_theme_channel(color.y), tb_theme_channel(color.z), alpha);
    }
    tb_theme_keep(kept, tb_json_write_string(writer, text, length));
}

/* Numbers as an array, `count` of them. */
static void tb_theme_write_numbers(tb_json_writer *writer, const float *numbers, size_t count,
                                   tb_json_result *kept)
{
    tb_theme_keep(kept, tb_json_write_begin_array(writer));
    for (size_t index = 0; index < count; index++)
    {
        tb_theme_write_number(writer, numbers[index], kept);
    }
    tb_theme_keep(kept, tb_json_write_end_array(writer));
}

/* Edges, always as an array, so that an element's shape reads back as
 * edges: [across, down] when they are even, or all four. */
static void tb_theme_write_edges(tb_json_writer *writer, tb_gui_edges edges, tb_json_result *kept)
{
    const float even[2] = {edges.left, edges.top};
    const float all[4] = {edges.left, edges.top, edges.right, edges.bottom};

    if (edges.left == edges.right && edges.top == edges.bottom)
    {
        tb_theme_write_numbers(writer, even, 2, kept);
        return;
    }
    tb_theme_write_numbers(writer, all, 4, kept);
}

static void tb_theme_write_font(tb_json_writer *writer, tb_font_spec font, tb_json_result *kept)
{
    const tb_font_role role = font.role < TB_FONT_ROLE_COUNT ? font.role : TB_FONT_ROLE_UI;
    const char *const name = tb_theme_role_names[role];

    tb_theme_keep(kept, tb_json_write_begin_object(writer));
    tb_theme_write_key(writer, "role", kept);
    tb_theme_keep(kept, tb_json_write_string(writer, name, tb_str_length(name)));
    tb_theme_write_key(writer, "weight", kept);
    tb_theme_keep(kept, tb_json_write_int64(writer, font.weight));
    if (font.italic)
    {
        tb_theme_write_key(writer, "italic", kept);
        tb_theme_keep(kept, tb_json_write_bool(writer, true));
    }
    if (font.size > 0.0f)
    {
        tb_theme_write_key(writer, "size", kept);
        tb_theme_write_number(writer, font.size, kept);
    }
    tb_theme_keep(kept, tb_json_write_end_object(writer));
}

static void tb_theme_write_image(tb_json_writer *writer, const tb_style_image *image,
                                 const char *path, tb_json_result *kept)
{
    const tb_canvas_nine_slice *const slice = &image->slice;
    const tb_vec4 tint = image->tint;
    const tb_rhi_rect source = slice->source;

    tb_theme_keep(kept, tb_json_write_begin_object(writer));
    tb_theme_write_key(writer, "image", kept);
    tb_theme_keep(kept, tb_json_write_string(writer, path, tb_str_length(path)));
    tb_theme_write_key(writer, "insets", kept);
    tb_theme_keep(kept, tb_json_write_begin_array(writer));
    tb_theme_keep(kept, tb_json_write_uint64(writer, slice->left));
    tb_theme_keep(kept, tb_json_write_uint64(writer, slice->top));
    tb_theme_keep(kept, tb_json_write_uint64(writer, slice->right));
    tb_theme_keep(kept, tb_json_write_uint64(writer, slice->bottom));
    tb_theme_keep(kept, tb_json_write_end_array(writer));
    if (source.width > 0 && source.height > 0)
    {
        tb_theme_write_key(writer, "source", kept);
        tb_theme_keep(kept, tb_json_write_begin_array(writer));
        tb_theme_keep(kept, tb_json_write_int64(writer, source.x));
        tb_theme_keep(kept, tb_json_write_int64(writer, source.y));
        tb_theme_keep(kept, tb_json_write_uint64(writer, source.width));
        tb_theme_keep(kept, tb_json_write_uint64(writer, source.height));
        tb_theme_keep(kept, tb_json_write_end_array(writer));
    }
    if (slice->scale > 0.0f)
    {
        tb_theme_write_key(writer, "scale", kept);
        tb_theme_write_number(writer, slice->scale, kept);
    }
    if (slice->whole_pixels)
    {
        tb_theme_write_key(writer, "whole_pixels", kept);
        tb_theme_keep(kept, tb_json_write_bool(writer, true));
    }
    if (slice->hollow)
    {
        tb_theme_write_key(writer, "hollow", kept);
        tb_theme_keep(kept, tb_json_write_bool(writer, true));
    }
    if (tint.x != 1.0f || tint.y != 1.0f || tint.z != 1.0f || tint.w != 1.0f)
    {
        tb_theme_write_key(writer, "tint", kept);
        tb_theme_write_color(writer, tint, kept);
    }
    tb_theme_keep(kept, tb_json_write_end_object(writer));
}

tb_json_result tb_style_value_write(tb_json_writer *writer, const tb_style_value *value,
                                    const char *image_path)
{
    tb_json_result kept = TB_JSON_OK;

    switch (value->type)
    {
    case TB_STYLE_COLOR:
        tb_theme_write_color(writer, value->color, &kept);
        break;
    case TB_STYLE_NUMBER:
        tb_theme_write_number(writer, value->number, &kept);
        break;
    case TB_STYLE_EDGES:
        tb_theme_write_edges(writer, value->edges, &kept);
        break;
    case TB_STYLE_FONT:
        tb_theme_write_font(writer, value->font, &kept);
        break;
    case TB_STYLE_IMAGE:
        TB_ASSERT(image_path != NULL);
        tb_theme_write_image(writer, &value->image, image_path, &kept);
        break;
    case TB_STYLE_NONE:
        tb_theme_keep(&kept, tb_json_write_null(writer));
        break;
    }
    return kept;
}

/* ---------------------------------------------------------------------------
 * Theme files.
 * ------------------------------------------------------------------------- */

/* What a document is being read into, and where in it. */
typedef struct tb_theme_reader
{
    tb_theme *target; /* whose images are named */
    tb_theme *loaded; /* the classes read */
    const char *text;
    size_t length;
    const char *file_name; /* NULL for none */
    tb_gui_file_error *error;
    char path[TB_GUI_FILE_ERROR_CAPACITY];
    size_t path_length;
} tb_theme_reader;

static const tb_image *tb_theme_find_image(void *context, const char *path)
{
    return tb_theme_image(context, path);
}

static size_t tb_theme_path_key(tb_theme_reader *reader, const char *key)
{
    const size_t previous = reader->path_length;

    tb_gui_append(reader->path, sizeof(reader->path), &reader->path_length, ".");
    tb_gui_append(reader->path, sizeof(reader->path), &reader->path_length, key);
    return previous;
}

static void tb_theme_path_back(tb_theme_reader *reader, size_t previous)
{
    reader->path_length = previous;
    reader->path[previous] = '\0';
}

/* Fail at a value, at the reader's path, as a layout file does. */
static tb_gui_file_result tb_theme_fail(tb_theme_reader *reader, const tb_json_value *value,
                                        const char *reason, const char *detail)
{
    tb_gui_file_error *const error = reader->error;

    *error = (tb_gui_file_error){.result = TB_GUI_FILE_INVALID, .reason = reason};
    memcpy(error->path, reader->path, reader->path_length + 1);
    tb_gui_locate(reader->text, reader->length, tb_json_offset_of(value), &error->line,
                  &error->column);
    tb_gui_error_message(error, reader->file_name, detail);
    return TB_GUI_FILE_INVALID;
}

static tb_gui_file_result tb_theme_read_class(tb_theme_reader *reader, const char *style_class,
                                              const tb_json_value *values)
{
    char name[TB_GUI_FILE_ERROR_CAPACITY] = {0};

    if (tb_json_type_of(values) != TB_JSON_OBJECT)
    {
        return tb_theme_fail(reader, values, "a style class: an object of values by name", NULL);
    }
    for (size_t member = 0; member < tb_json_count(values); member++)
    {
        const char *key = NULL;
        size_t length = 0;
        uint32_t states = 0;
        tb_style_value value = {0};
        const tb_json_value *const json = tb_json_member_at(values, member, &key, &length);
        const size_t back = tb_theme_path_key(reader, key);
        const char *reason = NULL;
        if (!tb_style_key_read(key, length, name, sizeof(name), &states))
        {
            reason = "a key: a name, then \":\" and states joined by \"+\": hovered, pressed, "
                     "focused, disabled, focus_visible, engaged, selected";
        }
        else
        {
            reason = tb_style_value_read(name, json, tb_theme_find_image, reader->target, &value);
        }
        if (reason != NULL)
        {
            return tb_theme_fail(reader, json, reason, NULL);
        }
        const bool set = tb_theme_set(reader->loaded, style_class, name, states, &value);
        TB_ASSERT_MSG(set, "a value read as its name's type");
        (void)set;
        tb_theme_path_back(reader, back);
    }
    return TB_GUI_FILE_OK;
}

/* A theme's top: its format, version, name, and styles. */
static tb_gui_file_result tb_theme_read_head(tb_theme_reader *reader, const tb_json_value *root)
{
    static const char *const keys[] = {"format", "version", "name", "styles"};
    const tb_json_value *const format = tb_json_find(root, "format", 6);
    const tb_json_value *const version = tb_json_find(root, "version", 7);
    const tb_json_value *const name = tb_json_find(root, "name", 4);
    size_t length = 0;
    int64_t number = 0;

    for (size_t member = 0; member < tb_json_count(root); member++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        const tb_json_value *const value = tb_json_member_at(root, member, &key, &key_length);
        bool known = false;
        for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++)
        {
            known = known || tb_str_equal(keys[index], key);
        }
        if (!known)
        {
            (void)tb_theme_path_key(reader, key);
            return tb_theme_fail(reader, value, "unknown key", key);
        }
    }
    const char *const format_name = format != NULL && tb_json_type_of(format) == TB_JSON_STRING
                                        ? tb_json_as_text(format, &length)
                                        : NULL;
    if (format_name == NULL || !tb_str_equal(format_name, TB_THEME_FILE_FORMAT))
    {
        (void)tb_theme_path_key(reader, "format");
        return tb_theme_fail(reader, format != NULL ? format : root,
                             "a format other than \"" TB_THEME_FILE_FORMAT "\"", NULL);
    }
    if (version == NULL || !tb_json_as_int64(version, &number) || number < 1)
    {
        (void)tb_theme_path_key(reader, "version");
        return tb_theme_fail(reader, version != NULL ? version : root,
                             "a version: a whole number from 1", NULL);
    }
    if (number > TB_THEME_FILE_FORMAT_VERSION)
    {
        (void)tb_theme_path_key(reader, "version");
        return tb_theme_fail(reader, version, "a newer version than this engine reads", NULL);
    }
    if (name != NULL && tb_json_type_of(name) != TB_JSON_STRING)
    {
        (void)tb_theme_path_key(reader, "name");
        return tb_theme_fail(reader, name, "a theme's name: text", NULL);
    }
    return TB_GUI_FILE_OK;
}

static tb_gui_file_result tb_theme_read_document(tb_theme_reader *reader, const tb_json_value *root)
{
    if (tb_json_type_of(root) != TB_JSON_OBJECT)
    {
        return tb_theme_fail(reader, root, "a theme: an object", NULL);
    }
    const tb_gui_file_result head = tb_theme_read_head(reader, root);
    if (head != TB_GUI_FILE_OK)
    {
        return head;
    }
    const tb_json_value *const styles = tb_json_find(root, "styles", 6);
    if (styles == NULL || tb_json_type_of(styles) != TB_JSON_OBJECT)
    {
        (void)tb_theme_path_key(reader, "styles");
        return tb_theme_fail(reader, styles != NULL ? styles : root,
                             "a theme's styles: an object of style classes", NULL);
    }
    const size_t back = tb_theme_path_key(reader, "styles");
    for (size_t member = 0; member < tb_json_count(styles); member++)
    {
        const char *style_class = NULL;
        size_t class_length = 0;
        const tb_json_value *const values =
            tb_json_member_at(styles, member, &style_class, &class_length);
        const size_t inner = tb_theme_path_key(reader, style_class);
        const tb_gui_file_result read = tb_theme_read_class(reader, style_class, values);
        if (read != TB_GUI_FILE_OK)
        {
            return read;
        }
        tb_theme_path_back(reader, inner);
    }
    tb_theme_path_back(reader, back);
    return TB_GUI_FILE_OK;
}

tb_gui_file_result tb_theme_load(tb_theme *theme, const char *text, size_t length, const char *name,
                                 tb_gui_file_error *error)
{
    tb_gui_file_error ignored = {0};
    tb_theme_reader reader = {.target = theme,
                              .text = text,
                              .length = length,
                              .file_name = name,
                              .error = error != NULL ? error : &ignored};
    const tb_json_options options = {.max_depth = TB_THEME_FILE_JSON_DEPTH};
    tb_json_error json_error = {0};
    const tb_json_value *root = NULL;
    tb_arena *const arena = tb_arena_create(0);
    tb_gui_file_result result = TB_GUI_FILE_OK;

    TB_ASSERT(theme != NULL);
    TB_ASSERT(text != NULL || length == 0);
    *reader.error = (tb_gui_file_error){.reason = ""};
    tb_gui_append(reader.path, sizeof(reader.path), &reader.path_length, "$");
    reader.loaded = tb_theme_create();
    if (tb_json_parse(text, length, &options, arena, &root, &json_error) != TB_JSON_OK)
    {
        *reader.error = (tb_gui_file_error){.result = TB_GUI_FILE_MALFORMED,
                                            .line = json_error.line,
                                            .column = json_error.column,
                                            .reason = json_error.reason};
        tb_gui_error_message(reader.error, name, NULL);
        result = TB_GUI_FILE_MALFORMED;
        goto cleanup;
    }
    result = tb_theme_read_document(&reader, root);
    if (result == TB_GUI_FILE_OK)
    {
        tb_theme_take_classes(theme, reader.loaded);
    }
cleanup:
    tb_theme_destroy(reader.loaded);
    tb_arena_destroy(arena);
    return result;
}

tb_gui_file_result tb_theme_load_file(tb_theme *theme, struct tb_vfs *vfs, const char *path,
                                      tb_gui_file_error *error)
{
    tb_gui_file_error ignored = {0};
    tb_gui_file_error *const found = error != NULL ? error : &ignored;
    char *bytes = NULL;
    size_t length = 0;

    TB_ASSERT(theme != NULL);
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    const tb_gui_file_result read = tb_gui_read_file(vfs, path, &bytes, &length, found);
    if (read != TB_GUI_FILE_OK)
    {
        return read;
    }
    const tb_gui_file_result result = tb_theme_load(theme, bytes, length, path, found);
    tb_free(bytes);
    return result;
}
