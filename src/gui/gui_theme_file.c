/* Theme files: a theme's classes and values read from JSON (docs/gui.md,
 * "Theme files"), all or nothing, and read again when the file changes.
 *
 * The layout files of the GUI read JSON, report errors, and watch files
 * through the VFS the same way; once both are in, they should share one
 * reader of files and one watch list. */
#include "gui_style.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/hash.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/vfs.h"

#include <math.h>
#include <string.h>

/* Bytes an arena of a document grows by. */
#define TB_THEME_FILE_ARENA_CHUNK 16384u
/* The largest theme file read: 4 MiB. */
#define TB_THEME_FILE_MOST_BYTES ((uint64_t)4u << 20)
/* Seconds between looks at a watched file, unless chosen. */
#define TB_THEME_WATCH_INTERVAL 0.5
/* Watches a GUI first has room for. */
#define TB_THEME_FIRST_WATCHES 4u
/* The most a font's weight is. */
#define TB_THEME_MOST_WEIGHT 1000
/* What a theme file says it is. */
#define TB_THEME_FILE_FORMAT "badger-theme"
/* The largest number of units a value is: more is a mistake. */
#define TB_THEME_MOST_UNITS 1.0e6

/* What a document is being read into, and where it failed. */
typedef struct tb_theme_reader
{
    tb_theme *target;  /* whose images are named */
    tb_theme *loaded;  /* the classes read */
    const char *name;  /* the file's, for messages; never NULL */
    char path[TB_THEME_FILE_ERROR_CAPACITY];
    tb_theme_file_error *error;
} tb_theme_reader;

/* ---------------------------------------------------------------------------
 * Errors.
 * ------------------------------------------------------------------------- */

static tb_theme_file_result tb_theme_fail(tb_theme_reader *reader, tb_theme_file_result result,
                                          const char *reason)
{
    tb_theme_file_error *const error = reader->error;

    *error = (tb_theme_file_error){.result = result};
    (void)tb_fmt_buffer(error->path, sizeof(error->path), "%s", reader->path);
    if (reader->path[0] != '\0')
    {
        (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s: %s: %s", reader->name,
                            reader->path, reason);
    }
    else
    {
        (void)tb_fmt_buffer(error->message, sizeof(error->message), "%s: %s", reader->name,
                            reason);
    }
    return result;
}

/* Set where in the document the reader is: a dotted path of keys. */
static void tb_theme_at(tb_theme_reader *reader, const char *style_class, const char *key)
{
    if (style_class == NULL)
    {
        (void)tb_fmt_buffer(reader->path, sizeof(reader->path), "%s", key);
        return;
    }
    (void)tb_fmt_buffer(reader->path, sizeof(reader->path), "styles.%s%s%s", style_class,
                        key != NULL ? "." : "", key != NULL ? key : "");
}

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
        channels[channel] = (float)(high * 16 + low) / 255.0f;
    }
    *color = (tb_vec4){channels[0], channels[1], channels[2], channels[3]};
    return true;
}

static bool tb_theme_read_number(const tb_json_value *value, float *number)
{
    double read = 0.0;

    if (!tb_json_as_double(value, &read) || !isfinite(read) || fabs(read) > TB_THEME_MOST_UNITS)
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

/* A member's key is one of a list of names. */
static bool tb_theme_known_keys(const tb_json_value *object, const char *const *names,
                                size_t count, const char **unknown)
{
    for (size_t member = 0; member < tb_json_count(object); member++)
    {
        const char *key = NULL;
        size_t length = 0;
        bool known = false;
        (void)tb_json_member_at(object, member, &key, &length);
        for (size_t index = 0; index < count; index++)
        {
            known = known || strcmp(names[index], key) == 0;
        }
        if (!known)
        {
            *unknown = key;
            return false;
        }
    }
    return true;
}

static tb_theme_file_result tb_theme_read_font(tb_theme_reader *reader,
                                               const tb_json_value *value, tb_font_spec *font)
{
    static const char *const keys[] = {"role", "weight", "italic", "size"};
    const tb_json_value *const role = tb_json_find(value, "role", 4);
    const tb_json_value *const weight = tb_json_find(value, "weight", 6);
    const tb_json_value *const italic = tb_json_find(value, "italic", 6);
    const tb_json_value *const size = tb_json_find(value, "size", 4);
    const char *unknown = NULL;
    size_t length = 0;
    int64_t read_weight = TB_FONT_WEIGHT_REGULAR;

    if (!tb_theme_known_keys(value, keys, sizeof(keys) / sizeof(keys[0]), &unknown))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a font of an unknown key");
    }
    const char *const role_name = role != NULL ? tb_json_as_text(role, &length) : NULL;
    *font = (tb_font_spec){.role = TB_FONT_ROLE_COUNT};
    for (size_t index = 0; role_name != NULL && index < TB_FONT_ROLE_COUNT; index++)
    {
        if (tb_json_type_of(role) == TB_JSON_STRING &&
            strcmp(role_name, tb_theme_role_names[index]) == 0)
        {
            font->role = (tb_font_role)index;
        }
    }
    if (font->role == TB_FONT_ROLE_COUNT)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "a font's role: ui, body, heading, monospace, or pixel");
    }
    if (weight != NULL && (!tb_json_as_int64(weight, &read_weight) || read_weight < 1 ||
                           read_weight > TB_THEME_MOST_WEIGHT))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a font's weight: from 1 to 1000");
    }
    font->weight = (uint16_t)read_weight;
    if (italic != NULL && !tb_json_as_bool(italic, &font->italic))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a font's italic: true or false");
    }
    if (size != NULL && (!tb_theme_read_number(size, &font->size) || font->size < 0.0f))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a font's size: 0 or more units");
    }
    return TB_THEME_FILE_OK;
}

/* A count of texels, whole and not negative. */
static bool tb_theme_read_texels(const tb_json_value *value, uint32_t *texels)
{
    int64_t read = 0;

    if (!tb_json_as_int64(value, &read) || read < 0 || read > (int64_t)UINT32_MAX)
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
        if (!tb_theme_read_texels(item, &texels[index]))
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
        if (!tb_theme_read_texels(tb_json_at(value, index), &numbers[index]))
        {
            return false;
        }
    }
    *source = (tb_rhi_rect){numbers[0], numbers[1], numbers[2], numbers[3]};
    return true;
}

static tb_theme_file_result tb_theme_read_image(tb_theme_reader *reader,
                                                const tb_json_value *value, tb_style_image *image)
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
    const char *unknown = NULL;
    size_t length = 0;

    *image = (tb_style_image){.tint = {1.0f, 1.0f, 1.0f, 1.0f}};
    if (!tb_theme_known_keys(value, keys, sizeof(keys) / sizeof(keys[0]), &unknown))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "an image of an unknown key");
    }
    const char *const name = tb_json_as_text(path, &length);
    if (tb_json_type_of(path) != TB_JSON_STRING || length == 0)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "an image's path: a VFS path");
    }
    image->slice.image = tb_theme_image(reader->target, name);
    if (image->slice.image == NULL)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "an image the theme has none of by that path (tb_theme_add_image())");
    }
    if (insets != NULL && !tb_theme_read_insets(insets, &image->slice))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "an image's insets: one, two, or four whole numbers of texels");
    }
    if (source != NULL && !tb_theme_read_source(source, &image->slice.source))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "an image's source: [x, y, width, height] in texels");
    }
    if (scale != NULL && (!tb_theme_read_number(scale, &image->slice.scale) ||
                          !(image->slice.scale > 0.0f)))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "an image's scale: more than 0");
    }
    if ((whole != NULL && !tb_json_as_bool(whole, &image->slice.whole_pixels)) ||
        (hollow != NULL && !tb_json_as_bool(hollow, &image->slice.hollow)))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "an image's whole_pixels and hollow: true or false");
    }
    if (tint != NULL && !tb_theme_read_color(tint, &image->tint))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "an image's tint: a color, \"#rrggbb\" or \"#rrggbbaa\"");
    }
    return TB_THEME_FILE_OK;
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
    return strcmp(name, "border_width") == 0 || strcmp(name, "radius") == 0 ||
           strcmp(name, "focus_width") == 0 || strcmp(name, "padding") == 0;
}

static tb_theme_file_result tb_theme_read_value(tb_theme_reader *reader, const char *name,
                                                const tb_json_value *json, tb_style_value *value)
{
    const tb_style_standard standard = tb_style_standard_of(name);
    const tb_style_type type = standard != TB_STYLE_STANDARD_COUNT
                                   ? tb_style_standard_type(standard)
                                   : tb_theme_shape_of(json);

    *value = (tb_style_value){.type = type};
    switch (type)
    {
    case TB_STYLE_COLOR:
        if (!tb_theme_read_color(json, &value->color))
        {
            return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                                 "a color: \"#rrggbb\" or \"#rrggbbaa\"");
        }
        return TB_THEME_FILE_OK;
    case TB_STYLE_NUMBER:
        if (!tb_theme_read_number(json, &value->number) ||
            (tb_theme_unsigned_name(name) && value->number < 0.0f))
        {
            return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a number of units");
        }
        return TB_THEME_FILE_OK;
    case TB_STYLE_EDGES:
        if (!tb_theme_read_edges(json, &value->edges) ||
            (tb_theme_unsigned_name(name) && !tb_theme_edges_unsigned(value->edges)))
        {
            return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                                 "edges: a number, [across, down], or [left, top, right, bottom]");
        }
        return TB_THEME_FILE_OK;
    case TB_STYLE_FONT:
        if (tb_json_type_of(json) != TB_JSON_OBJECT)
        {
            return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a font: an object with a role");
        }
        return tb_theme_read_font(reader, json, &value->font);
    case TB_STYLE_IMAGE:
        if (tb_json_type_of(json) != TB_JSON_OBJECT)
        {
            return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                                 "an image: an object with an image path");
        }
        return tb_theme_read_image(reader, json, &value->image);
    case TB_STYLE_NONE:
        break;
    }
    return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                         "a value: a color, a number, edges, a font, or an image");
}

static const char *const tb_theme_state_names[TB_STYLE_STATE_BITS] = {
    "hovered", "pressed", "focused", "disabled", "selected"};
static const uint32_t tb_theme_state_flags[TB_STYLE_STATE_BITS] = {
    TB_WIDGET_HOVERED, TB_WIDGET_PRESSED, TB_WIDGET_FOCUSED, TB_WIDGET_DISABLED,
    TB_WIDGET_SELECTED};

/* A key "name" or "name:state+state", split into its name, copied into
 * `name`, and its states. */
static bool tb_theme_read_key(const char *key, size_t length, char *name, size_t capacity,
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
        const size_t word = (size_t)(stop - at);
        bool found = false;
        for (size_t index = 0; index < TB_STYLE_STATE_BITS; index++)
        {
            if (strlen(tb_theme_state_names[index]) == word &&
                memcmp(tb_theme_state_names[index], at, word) == 0)
            {
                *states |= tb_theme_state_flags[index];
                found = true;
            }
        }
        if (!found)
        {
            return false;
        }
        at = stop + 1;
    }
    return true;
}

static tb_theme_file_result tb_theme_read_class(tb_theme_reader *reader, const char *style_class,
                                                const tb_json_value *values)
{
    char name[TB_THEME_FILE_ERROR_CAPACITY] = {0};

    tb_theme_at(reader, style_class, NULL);
    if (tb_json_type_of(values) != TB_JSON_OBJECT)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "a style class: an object of values by name");
    }
    for (size_t member = 0; member < tb_json_count(values); member++)
    {
        const char *key = NULL;
        size_t length = 0;
        uint32_t states = 0;
        tb_style_value value = {0};
        const tb_json_value *const json = tb_json_member_at(values, member, &key, &length);
        tb_theme_at(reader, style_class, key);
        if (!tb_theme_read_key(key, length, name, sizeof(name), &states))
        {
            return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                                 "a key: a name, then \":\" and states joined by \"+\": "
                                 "hovered, pressed, focused, disabled, selected");
        }
        const tb_theme_file_result read = tb_theme_read_value(reader, name, json, &value);
        if (read != TB_THEME_FILE_OK)
        {
            return read;
        }
        const bool set = tb_theme_set(reader->loaded, style_class, name, states, &value);
        TB_ASSERT_MSG(set, "a value read as its name's type");
        (void)set;
    }
    return TB_THEME_FILE_OK;
}

static tb_theme_file_result tb_theme_read_document(tb_theme_reader *reader,
                                                   const tb_json_value *root)
{
    static const char *const keys[] = {"format", "version", "name", "styles"};
    const tb_json_value *const format = tb_json_find(root, "format", 6);
    const tb_json_value *const version = tb_json_find(root, "version", 7);
    const tb_json_value *const name = tb_json_find(root, "name", 4);
    const tb_json_value *const styles = tb_json_find(root, "styles", 6);
    const char *unknown = NULL;
    size_t length = 0;
    int64_t number = 0;

    if (tb_json_type_of(root) != TB_JSON_OBJECT)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a theme: an object");
    }
    tb_theme_at(reader, NULL, "format");
    const char *const format_name = format != NULL ? tb_json_as_text(format, &length) : NULL;
    if (format_name == NULL || tb_json_type_of(format) != TB_JSON_STRING ||
        strcmp(format_name, TB_THEME_FILE_FORMAT) != 0)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "a theme's format: \"" TB_THEME_FILE_FORMAT "\"");
    }
    tb_theme_at(reader, NULL, "version");
    if (version == NULL || !tb_json_as_int64(version, &number))
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a theme's version: a whole number");
    }
    if (number != TB_THEME_FILE_FORMAT_VERSION)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_VERSION, "a version this engine does not read");
    }
    tb_theme_at(reader, NULL, "");
    if (!tb_theme_known_keys(root, keys, sizeof(keys) / sizeof(keys[0]), &unknown))
    {
        tb_theme_at(reader, NULL, unknown);
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "a key a theme has none of: format, version, name, or styles");
    }
    tb_theme_at(reader, NULL, "name");
    if (name != NULL && tb_json_type_of(name) != TB_JSON_STRING)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID, "a theme's name: text");
    }
    tb_theme_at(reader, NULL, "styles");
    if (styles == NULL || tb_json_type_of(styles) != TB_JSON_OBJECT)
    {
        return tb_theme_fail(reader, TB_THEME_FILE_INVALID,
                             "a theme's styles: an object of style classes");
    }
    for (size_t member = 0; member < tb_json_count(styles); member++)
    {
        const char *style_class = NULL;
        size_t class_length = 0;
        const tb_json_value *const values =
            tb_json_member_at(styles, member, &style_class, &class_length);
        const tb_theme_file_result read = tb_theme_read_class(reader, style_class, values);
        if (read != TB_THEME_FILE_OK)
        {
            return read;
        }
    }
    return TB_THEME_FILE_OK;
}

tb_theme_file_result tb_theme_load(tb_theme *theme, const char *text, size_t length,
                                   const char *name, tb_theme_file_error *error)
{
    tb_theme_file_error ignored = {0};
    tb_theme_reader reader = {.target = theme,
                              .name = name != NULL ? name : "theme",
                              .error = error != NULL ? error : &ignored};
    const tb_json_options options = {.flags = TB_JSON_ALLOW_COMMENTS |
                                              TB_JSON_ALLOW_TRAILING_COMMAS};
    tb_json_error json_error = {0};
    const tb_json_value *root = NULL;
    tb_arena *const arena = tb_arena_create(TB_THEME_FILE_ARENA_CHUNK);
    tb_theme_file_result result = TB_THEME_FILE_OK;

    TB_ASSERT(theme != NULL);
    TB_ASSERT(text != NULL || length == 0);
    *reader.error = (tb_theme_file_error){0};
    reader.loaded = tb_theme_create();
    if (tb_json_parse(text, length, &options, arena, &root, &json_error) != TB_JSON_OK)
    {
        result = tb_theme_fail(&reader, TB_THEME_FILE_SYNTAX, json_error.reason);
        reader.error->line = json_error.line;
        reader.error->column = json_error.column;
        (void)tb_fmt_buffer(reader.error->message, sizeof(reader.error->message),
                            "%s:%zu:%zu: %s", reader.name, json_error.line, json_error.column,
                            json_error.reason);
        goto cleanup;
    }
    result = tb_theme_read_document(&reader, root);
    if (result == TB_THEME_FILE_OK)
    {
        tb_theme_take_classes(theme, reader.loaded);
    }
cleanup:
    tb_theme_destroy(reader.loaded);
    tb_arena_destroy(arena);
    return result;
}

/* ---------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

/* A file's bytes through a VFS, a terminator after them; tb_free() them. */
static tb_theme_file_result tb_theme_read_file(struct tb_vfs *vfs, const char *path,
                                               char **bytes, size_t *length,
                                               tb_theme_file_error *error)
{
    tb_theme_reader reader = {.name = path, .error = error};
    tb_stream stream = {0};
    uint64_t size = 0;
    tb_theme_file_result result = TB_THEME_FILE_OK;
    const tb_vfs_result opened = tb_vfs_open(vfs, path, tb_str_length(path), NULL, &stream);

    *bytes = NULL;
    *length = 0;
    if (opened == TB_VFS_NOT_FOUND || opened == TB_VFS_NOT_A_FILE || opened == TB_VFS_INVALID)
    {
        return tb_theme_fail(&reader, TB_THEME_FILE_NOT_FOUND, "no such file");
    }
    if (opened != TB_VFS_OK)
    {
        return tb_theme_fail(&reader, TB_THEME_FILE_INPUT_FAILED, tb_vfs_result_name(opened));
    }
    if (tb_stream_size(&stream, &size) != TB_STREAM_OK || size > TB_THEME_FILE_MOST_BYTES)
    {
        result = tb_theme_fail(&reader, TB_THEME_FILE_INPUT_FAILED, "a file too large to read");
        goto cleanup;
    }
    *length = (size_t)size;
    *bytes = tb_malloc(*length + 1);
    (*bytes)[*length] = '\0';
    if (tb_stream_read_exact(&stream, *bytes, *length) != TB_STREAM_OK)
    {
        tb_free(*bytes);
        *bytes = NULL;
        *length = 0;
        result = tb_theme_fail(&reader, TB_THEME_FILE_INPUT_FAILED, "the file could not be read");
    }
cleanup:
    (void)tb_stream_close(&stream);
    return result;
}

tb_theme_file_result tb_theme_load_file(tb_theme *theme, struct tb_vfs *vfs, const char *path,
                                        tb_theme_file_error *error)
{
    tb_theme_file_error ignored = {0};
    tb_theme_file_error *const found = error != NULL ? error : &ignored;
    char *bytes = NULL;
    size_t length = 0;

    TB_ASSERT(theme != NULL);
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    const tb_theme_file_result read = tb_theme_read_file(vfs, path, &bytes, &length, found);
    if (read != TB_THEME_FILE_OK)
    {
        return read;
    }
    const tb_theme_file_result result = tb_theme_load(theme, bytes, length, path, found);
    tb_free(bytes);
    return result;
}

/* ---------------------------------------------------------------------------
 * Watching.
 * ------------------------------------------------------------------------- */

struct tb_gui_theme_watch
{
    tb_theme *theme;
    struct tb_vfs *vfs;
    char *path; /* owned */
    double interval;
    double next;   /* the GUI's time of the next look */
    bool read;     /* the file was read, and `hash` is of its bytes */
    uint64_t hash; /* of the bytes last loaded or tried */
    tb_theme_reload_callback *callback;
    void *user;
};

static size_t tb_gui_theme_watch_index(const tb_gui *gui, const tb_theme *theme)
{
    for (size_t index = 0; index < gui->theme_watch_count; index++)
    {
        if (gui->theme_watches[index].theme == theme)
        {
            return index;
        }
    }
    return SIZE_MAX;
}

bool tb_gui_watch_theme(tb_gui *gui, tb_theme *theme, struct tb_vfs *vfs, const char *path,
                        const tb_theme_watch_options *options)
{
    static const tb_theme_watch_options defaults = {0};
    const tb_theme_watch_options *const chosen = options != NULL ? options : &defaults;
    tb_theme_file_error ignored = {0};
    char *bytes = NULL;
    size_t length = 0;

    TB_ASSERT(gui != NULL);
    TB_ASSERT(theme != NULL);
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    if (tb_gui_theme_watch_index(gui, theme) != SIZE_MAX)
    {
        return false;
    }
    tb_gui_theme_watch watch = {
        .theme = theme,
        .vfs = vfs,
        .path = tb_str_dup(NULL, path),
        .interval = chosen->interval > 0.0 ? chosen->interval : TB_THEME_WATCH_INTERVAL,
        .callback = chosen->callback,
        .user = chosen->user};
    watch.next = gui->time + watch.interval;
    /* The file as it is now is the one loaded: only a change loads it again. */
    if (tb_theme_read_file(vfs, path, &bytes, &length, &ignored) == TB_THEME_FILE_OK)
    {
        watch.read = true;
        watch.hash = tb_hash_bytes(bytes, length);
    }
    tb_free(bytes);
    if (gui->theme_watch_count == gui->theme_watch_capacity)
    {
        gui->theme_watch_capacity = gui->theme_watch_capacity == 0
                                        ? TB_THEME_FIRST_WATCHES
                                        : gui->theme_watch_capacity * 2u;
        gui->theme_watches = tb_realloc_array(gui->theme_watches, gui->theme_watch_capacity,
                                              sizeof(*gui->theme_watches));
    }
    gui->theme_watches[gui->theme_watch_count] = watch;
    gui->theme_watch_count++;
    return true;
}

bool tb_gui_unwatch_theme(tb_gui *gui, tb_theme *theme)
{
    TB_ASSERT(gui != NULL);
    const size_t index = tb_gui_theme_watch_index(gui, theme);
    if (index == SIZE_MAX)
    {
        return false;
    }
    tb_str_free(NULL, gui->theme_watches[index].path);
    memmove(&gui->theme_watches[index], &gui->theme_watches[index + 1],
            (gui->theme_watch_count - index - 1) * sizeof(*gui->theme_watches));
    gui->theme_watch_count--;
    return true;
}

/* Look at a watch's file: load it when its bytes changed. Returns the
 * callback to call, and its error, which the caller calls once the watch
 * list is left alone. */
static bool tb_gui_theme_watch_look(tb_gui_theme_watch *watch, tb_theme_file_error *error)
{
    char *bytes = NULL;
    size_t length = 0;
    tb_theme_file_error ignored = {0};

    /* A file that cannot be read now, as while an editor replaces it, is
     * looked at again later. */
    if (tb_theme_read_file(watch->vfs, watch->path, &bytes, &length, &ignored) !=
        TB_THEME_FILE_OK)
    {
        return false;
    }
    const uint64_t hash = tb_hash_bytes(bytes, length);
    if (watch->read && hash == watch->hash)
    {
        tb_free(bytes);
        return false;
    }
    watch->read = true;
    watch->hash = hash;
    (void)tb_theme_load(watch->theme, bytes, length, watch->path, error);
    tb_free(bytes);
    return true;
}

void tb_gui_theme_watches_update(tb_gui *gui)
{
    for (size_t index = 0; index < gui->theme_watch_count; index++)
    {
        tb_gui_theme_watch *const watch = &gui->theme_watches[index];
        tb_theme_file_error error = {0};
        if (gui->time < watch->next)
        {
            continue;
        }
        watch->next = gui->time + watch->interval;
        if (!tb_gui_theme_watch_look(watch, &error) || watch->callback == NULL)
        {
            continue;
        }
        /* The callback may watch and unwatch: what it is called with is
         * copied first, and a list it changed is looked at again next time. */
        tb_theme_reload_callback *const callback = watch->callback;
        tb_theme *const theme = watch->theme;
        void *const user = watch->user;
        const size_t count = gui->theme_watch_count;
        callback(theme, error.result == TB_THEME_FILE_OK ? NULL : &error, user);
        if (gui->theme_watch_count != count)
        {
            return;
        }
    }
}

void tb_gui_theme_watches_free(tb_gui *gui)
{
    for (size_t index = 0; index < gui->theme_watch_count; index++)
    {
        tb_str_free(NULL, gui->theme_watches[index].path);
    }
    tb_free(gui->theme_watches);
    gui->theme_watches = NULL;
    gui->theme_watch_count = 0;
    gui->theme_watch_capacity = 0;
}
