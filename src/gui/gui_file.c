/* Layout files: a widget tree written as JSON and built from it.
 *
 * The format knows no kind of widget. A widget is its kind's registered
 * name, its name, its properties written through its kind's property table
 * by type, its place in its parent's grid, the rows and columns of its own
 * grid, and its children. Each type of value has a codec here, which reads
 * it from JSON, writes it, and compares two; a kind added anywhere is read
 * and written with no change to this file. */
#include "gui_style.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"
#include "badger/vfs.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* Widgets nested in a file at most, the root counted. */
#define TB_GUI_FILE_MOST_DEPTH 64u
/* JSON nesting a file needs for that: an object and a list of children for
 * each widget, the file's own object, and a property's arrays and objects. */
#define TB_GUI_FILE_JSON_DEPTH (2u * TB_GUI_FILE_MOST_DEPTH + 8u)
/* Bytes of a file read at most: far more than any layout. */
#define TB_GUI_FILE_MOST_BYTES ((size_t)64 << 20)
/* Significant digits that tell every float apart. */
#define TB_GUI_FLOAT_DIGITS 9
/* Whole numbers below this, 2 to the 24th, are every one a float, and are
 * written without an exponent. */
#define TB_GUI_FLOAT_WHOLE 16777216.0f

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* The common properties of a widget's place in its parent's grid, written
 * under "grid" in this order; and those written nowhere but as the
 * widget's own keys. */
static const char *const tb_gui_placement[] = {
    "row", "column", "row_span", "column_span", "sticky", "padx", "pady", "ipadx", "ipady"};
static const char *const tb_gui_reserved[] = {"name", "gridded"};

/* The keys of a file, of a widget, of a row or column, and of a font. */
static const char *const tb_gui_file_keys[] = {"format", "version", "variables", "root"};
static const char *const tb_gui_variable_keys[] = {"name", "type", "value", "setting"};
/* The types of variables, by the name a file gives them. */
static const char *const tb_gui_variable_types[] = {"bool", "int", "float", "string"};
static const tb_gui_type tb_gui_variable_type_of[] = {TB_GUI_TYPE_BOOL, TB_GUI_TYPE_INT,
                                                      TB_GUI_TYPE_FLOAT, TB_GUI_TYPE_TEXT};

static const char *const tb_gui_widget_keys[] = {"kind", "name",    "properties", "overrides",
                                                 "grid", "columns", "rows",       "children"};
static const char *const tb_gui_track_keys[] = {"weight", "minsize", "pad", "uniform"};
static const char *const tb_gui_font_keys[] = {"role", "weight", "italic", "size"};

static bool tb_gui_listed(const char *const *list, size_t count, const char *name)
{
    for (size_t index = 0; index < count; index++)
    {
        if (tb_str_equal(list[index], name))
        {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Errors.
 * ------------------------------------------------------------------------- */

/* Text added to a buffer, cut short at its end. */
void tb_gui_append(char *buffer, size_t capacity, size_t *used, const char *text)
{
    const size_t length = tb_str_length(text);
    size_t room = 0;
    size_t taken = 0;

    /* The terminator is always in the buffer: `used` stays below `capacity`. */
    TB_ASSERT(capacity > 0 && *used < capacity);
    room = capacity - *used - 1;
    taken = length < room ? length : room;
    memcpy(buffer + *used, text, taken);
    *used += taken;
    buffer[*used] = '\0';
}

/* The message: the file's name and place, the path, the reason, and what
 * was named. */
void tb_gui_error_message(tb_gui_file_error *error, const char *file_name, const char *detail)
{
    char place[64] = {0};
    size_t used = 0;

    error->message[0] = '\0';
    if (file_name != NULL)
    {
        tb_gui_append(error->message, sizeof(error->message), &used, file_name);
        tb_gui_append(error->message, sizeof(error->message), &used, ":");
    }
    if (error->line > 0)
    {
        (void)tb_fmt_buffer(place, sizeof(place), "%zu:%zu:", error->line, error->column);
        tb_gui_append(error->message, sizeof(error->message), &used, place);
    }
    if (used > 0)
    {
        tb_gui_append(error->message, sizeof(error->message), &used, " ");
    }
    if (error->path[0] != '\0')
    {
        tb_gui_append(error->message, sizeof(error->message), &used, error->path);
        tb_gui_append(error->message, sizeof(error->message), &used, ": ");
    }
    tb_gui_append(error->message, sizeof(error->message), &used, error->reason);
    if (detail != NULL)
    {
        tb_gui_append(error->message, sizeof(error->message), &used, " \"");
        tb_gui_append(error->message, sizeof(error->message), &used, detail);
        tb_gui_append(error->message, sizeof(error->message), &used, "\"");
    }
}

/* The line and column of an offset in a text, from 1. */
void tb_gui_locate(const char *text, size_t length, size_t offset, size_t *line, size_t *column)
{
    size_t line_start = 0;

    *line = 1;
    for (size_t index = 0; index < offset && index < length; index++)
    {
        if (text[index] == '\n')
        {
            (*line)++;
            line_start = index + 1;
        }
    }
    *column = offset - line_start + 1;
}

/* A failure with no place in a text, such as a file not found. */
static tb_gui_file_result tb_gui_fail_plain(tb_gui_file_error *error, tb_gui_file_result result,
                                            const char *file_name, const char *reason)
{
    *error = (tb_gui_file_error){.result = result, .reason = reason};
    tb_gui_error_message(error, file_name, NULL);
    return result;
}

/* ---------------------------------------------------------------------------
 * Reading: the loader, and the path to the value it reads.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_loader
{
    tb_gui *gui;
    const char *text;
    size_t length;
    const char *file_name; /* NULL for none */
    tb_gui_file_error *error;
    char path[TB_GUI_FILE_ERROR_CAPACITY];
    size_t path_length;
    tb_var *made_vars; /* variables this load made, destroyed if it fails */
    size_t made_var_count;
    size_t made_var_capacity;
} tb_gui_loader;

/* Go into a member of an object; returns where to come back to. */
static size_t tb_gui_path_key(tb_gui_loader *loader, const char *key)
{
    const size_t previous = loader->path_length;

    tb_gui_append(loader->path, sizeof(loader->path), &loader->path_length, ".");
    tb_gui_append(loader->path, sizeof(loader->path), &loader->path_length, key);
    return previous;
}

/* Go into a value of an array. */
static size_t tb_gui_path_index(tb_gui_loader *loader, size_t index)
{
    const size_t previous = loader->path_length;
    char text[32] = {0};

    (void)tb_fmt_buffer(text, sizeof(text), "[%zu]", index);
    tb_gui_append(loader->path, sizeof(loader->path), &loader->path_length, text);
    return previous;
}

static void tb_gui_path_back(tb_gui_loader *loader, size_t previous)
{
    loader->path_length = previous;
    loader->path[previous] = '\0';
}

/* Fail at a value, at the loader's path; `detail` is what was named, or NULL. */
static bool tb_gui_fail(tb_gui_loader *loader, const tb_json_value *value, const char *reason,
                        const char *detail)
{
    tb_gui_file_error *const error = loader->error;

    *error = (tb_gui_file_error){.result = TB_GUI_FILE_INVALID, .reason = reason};
    memcpy(error->path, loader->path, loader->path_length + 1);
    tb_gui_locate(loader->text, loader->length, tb_json_offset_of(value), &error->line,
                  &error->column);
    tb_gui_error_message(error, loader->file_name, detail);
    return false;
}

/* Every key of an object is one of a list; fails at the first that is not. */
static bool tb_gui_check_keys(tb_gui_loader *loader, const tb_json_value *object,
                              const char *const *keys, size_t key_count)
{
    for (size_t index = 0; index < tb_json_count(object); index++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        const tb_json_value *const member = tb_json_member_at(object, index, &key, &key_length);
        if (!tb_gui_listed(keys, key_count, key))
        {
            const size_t back = tb_gui_path_key(loader, key);
            (void)tb_gui_fail(loader, member, "unknown key", key);
            tb_gui_path_back(loader, back);
            return false;
        }
    }
    return true;
}

/* A member's value, by key; NULL when there is none. */
static const tb_json_value *tb_gui_member(const tb_json_value *object, const char *key)
{
    return tb_json_find(object, key, tb_str_length(key));
}

static bool tb_gui_read_float_json(tb_gui_loader *loader, const tb_json_value *json, float *out)
{
    double number = 0.0;

    if (tb_json_type_of(json) != TB_JSON_NUMBER || !tb_json_as_double(json, &number))
    {
        return tb_gui_fail(loader, json, "expected a number", NULL);
    }
    if (fabs(number) > (double)FLT_MAX)
    {
        return tb_gui_fail(loader, json, "a number out of range", NULL);
    }
    *out = (float)number;
    return true;
}

/* An array of `count` numbers. */
static bool tb_gui_read_floats(tb_gui_loader *loader, const tb_json_value *json, float *out,
                               size_t count)
{
    if (tb_json_type_of(json) != TB_JSON_ARRAY || tb_json_count(json) != count)
    {
        return tb_gui_fail(loader, json,
                           count == 2 ? "expected an array of two numbers"
                                      : "expected an array of four numbers",
                           NULL);
    }
    for (size_t index = 0; index < count; index++)
    {
        const size_t back = tb_gui_path_index(loader, index);
        const bool read = tb_gui_read_float_json(loader, tb_json_at(json, index), &out[index]);
        tb_gui_path_back(loader, back);
        if (!read)
        {
            return false;
        }
    }
    return true;
}

static bool tb_gui_read_string(tb_gui_loader *loader, const tb_json_value *json, tb_text *out)
{
    size_t length = 0;
    const char *text = NULL;

    if (tb_json_type_of(json) != TB_JSON_STRING)
    {
        return tb_gui_fail(loader, json, "expected a string", NULL);
    }
    text = tb_json_as_text(json, &length);
    *out = tb_text_make(text, length);
    return true;
}

/* ---------------------------------------------------------------------------
 * Writing: the saver, and floats in their fewest digits.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_saver
{
    tb_gui *gui;
    tb_json_writer *writer;
    tb_gui_file_result result;             /* the first failure; OK while there is none */
    tb_gui_file_error *error;              /* where and why the first failure was */
    char path[TB_GUI_FILE_ERROR_CAPACITY]; /* of the value being written */
    size_t path_length;
} tb_gui_saver;

/* The first failure is kept, with the path of the value being written. */
static void tb_gui_save_fail(tb_gui_saver *saver, tb_gui_file_result result, const char *reason,
                             const char *detail)
{
    if (saver->result != TB_GUI_FILE_OK)
    {
        return;
    }
    saver->result = result;
    *saver->error = (tb_gui_file_error){.result = result, .reason = reason};
    memcpy(saver->error->path, saver->path, saver->path_length + 1);
    tb_gui_error_message(saver->error, NULL, detail);
}

static void tb_gui_saver_check(tb_gui_saver *saver, tb_json_result result)
{
    if (result == TB_JSON_OUTPUT_FAILED)
    {
        tb_gui_save_fail(saver, TB_GUI_FILE_OUTPUT_FAILED, "the stream refused what was written",
                         NULL);
    }
    else if (result != TB_JSON_OK)
    {
        tb_gui_save_fail(saver, TB_GUI_FILE_INVALID, "text that is not UTF-8", NULL);
    }
}

/* Go into a member of what is written; returns where to come back to. */
static size_t tb_gui_saver_key(tb_gui_saver *saver, const char *key)
{
    const size_t previous = saver->path_length;

    tb_gui_append(saver->path, sizeof(saver->path), &saver->path_length, ".");
    tb_gui_append(saver->path, sizeof(saver->path), &saver->path_length, key);
    return previous;
}

static size_t tb_gui_saver_index(tb_gui_saver *saver, size_t index)
{
    const size_t previous = saver->path_length;
    char text[32] = {0};

    (void)tb_fmt_buffer(text, sizeof(text), "[%zu]", index);
    tb_gui_append(saver->path, sizeof(saver->path), &saver->path_length, text);
    return previous;
}

static void tb_gui_saver_back(tb_gui_saver *saver, size_t previous)
{
    saver->path_length = previous;
    saver->path[previous] = '\0';
}

static void tb_gui_write_key(tb_gui_saver *saver, const char *key)
{
    tb_gui_saver_check(saver, tb_json_write_key(saver->writer, key, tb_str_length(key)));
}

/* Text that is not UTF-8 fails the save; a null takes its place, so that
 * the document stays whole. */
static void tb_gui_write_string(tb_gui_saver *saver, tb_text text)
{
    const tb_json_result result = tb_json_write_string(saver->writer, text.bytes, text.length);

    tb_gui_saver_check(saver, result);
    if (result == TB_JSON_INVALID)
    {
        tb_gui_saver_check(saver, tb_json_write_null(saver->writer));
    }
}

/* The fewest significant digits that read back as the same float: 0.1, not
 * the double nearest the float 0.1. */
size_t tb_gui_float_text(float value, char *buffer, size_t capacity)
{
    size_t length = 0;

    /* A whole number is written whole, 100 rather than 1e+02. */
    if (value == truncf(value) && fabsf(value) < TB_GUI_FLOAT_WHOLE)
    {
        return tb_fmt_buffer(buffer, capacity, "%.0f", (double)value);
    }
    for (int digits = 1; digits <= TB_GUI_FLOAT_DIGITS; digits++)
    {
        double back = 0.0;
        length = tb_fmt_buffer(buffer, capacity, "%.*g", digits, (double)value);
        if (tb_decimal_parse(buffer, length, &back) == TB_DECIMAL_OK && (float)back == value)
        {
            break;
        }
    }
    return length;
}

static void tb_gui_write_float(tb_gui_saver *saver, float value)
{
    char text[TB_GUI_FLOAT_TEXT] = {0};
    const size_t length = tb_gui_float_text(value, text, sizeof(text));

    tb_gui_saver_check(saver, tb_json_write_number_text(saver->writer, text, length));
}

/* ---------------------------------------------------------------------------
 * Codecs: how each type of value is read, written, and compared.
 * ------------------------------------------------------------------------- */

typedef bool tb_gui_read_function(tb_gui_loader *loader, const tb_gui_property *property,
                                  const tb_json_value *json, tb_gui_value *value);
typedef void tb_gui_write_function(tb_gui_saver *saver, const tb_gui_property *property,
                                   const tb_gui_value *value);
typedef bool tb_gui_equal_function(const tb_gui_value *a, const tb_gui_value *b);

typedef struct tb_gui_codec
{
    tb_gui_read_function *read_value;
    tb_gui_write_function *write_value;
    tb_gui_equal_function *equal;
} tb_gui_codec;

static bool tb_gui_read_bool(tb_gui_loader *loader, const tb_gui_property *property,
                             const tb_json_value *json, tb_gui_value *value)
{
    bool read = false;

    (void)property;
    if (!tb_json_as_bool(json, &read))
    {
        return tb_gui_fail(loader, json, "expected true or false", NULL);
    }
    *value = tb_gui_bool(read);
    return true;
}

static void tb_gui_write_bool(tb_gui_saver *saver, const tb_gui_property *property,
                              const tb_gui_value *value)
{
    (void)property;
    tb_gui_saver_check(saver, tb_json_write_bool(saver->writer, value->boolean));
}

static bool tb_gui_equal_bool(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->boolean == b->boolean;
}

static bool tb_gui_read_int_json(tb_gui_loader *loader, const tb_json_value *json, int64_t least,
                                 int64_t most, int64_t *out)
{
    if (tb_json_type_of(json) != TB_JSON_NUMBER || !tb_json_as_int64(json, out))
    {
        return tb_gui_fail(loader, json, "expected an integer", NULL);
    }
    if (*out < least || *out > most)
    {
        return tb_gui_fail(loader, json, "an integer out of range", NULL);
    }
    return true;
}

static bool tb_gui_read_int(tb_gui_loader *loader, const tb_gui_property *property,
                            const tb_json_value *json, tb_gui_value *value)
{
    int64_t read = 0;

    (void)property;
    if (!tb_gui_read_int_json(loader, json, INT32_MIN, INT32_MAX, &read))
    {
        return false;
    }
    *value = tb_gui_int((int32_t)read);
    return true;
}

static void tb_gui_write_int(tb_gui_saver *saver, const tb_gui_property *property,
                             const tb_gui_value *value)
{
    (void)property;
    tb_gui_saver_check(saver, tb_json_write_int64(saver->writer, value->integer));
}

static bool tb_gui_equal_int(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->integer == b->integer;
}

static bool tb_gui_read_float(tb_gui_loader *loader, const tb_gui_property *property,
                              const tb_json_value *json, tb_gui_value *value)
{
    float read = 0.0f;

    (void)property;
    if (!tb_gui_read_float_json(loader, json, &read))
    {
        return false;
    }
    *value = tb_gui_float(read);
    return true;
}

static void tb_gui_write_number(tb_gui_saver *saver, const tb_gui_property *property,
                                const tb_gui_value *value)
{
    (void)property;
    tb_gui_write_float(saver, value->number);
}

static bool tb_gui_equal_float(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->number == b->number;
}

static bool tb_gui_read_vec2(tb_gui_loader *loader, const tb_gui_property *property,
                             const tb_json_value *json, tb_gui_value *value)
{
    float read[2] = {0};

    (void)property;
    if (!tb_gui_read_floats(loader, json, read, 2))
    {
        return false;
    }
    *value = tb_gui_vec2((tb_vec2){read[0], read[1]});
    return true;
}

static void tb_gui_write_vec2(tb_gui_saver *saver, const tb_gui_property *property,
                              const tb_gui_value *value)
{
    (void)property;
    tb_gui_saver_check(saver, tb_json_write_begin_array(saver->writer));
    tb_gui_write_float(saver, value->vec2.x);
    tb_gui_write_float(saver, value->vec2.y);
    tb_gui_saver_check(saver, tb_json_write_end_array(saver->writer));
}

static bool tb_gui_equal_vec2(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->vec2.x == b->vec2.x && a->vec2.y == b->vec2.y;
}

static bool tb_gui_read_color(tb_gui_loader *loader, const tb_gui_property *property,
                              const tb_json_value *json, tb_gui_value *value)
{
    float read[4] = {0};

    (void)property;
    if (!tb_gui_read_floats(loader, json, read, 4))
    {
        return false;
    }
    *value = tb_gui_color((tb_vec4){read[0], read[1], read[2], read[3]});
    return true;
}

static void tb_gui_write_color(tb_gui_saver *saver, const tb_gui_property *property,
                               const tb_gui_value *value)
{
    (void)property;
    tb_gui_saver_check(saver, tb_json_write_begin_array(saver->writer));
    tb_gui_write_float(saver, value->color.x);
    tb_gui_write_float(saver, value->color.y);
    tb_gui_write_float(saver, value->color.z);
    tb_gui_write_float(saver, value->color.w);
    tb_gui_saver_check(saver, tb_json_write_end_array(saver->writer));
}

static bool tb_gui_equal_color(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->color.x == b->color.x && a->color.y == b->color.y && a->color.z == b->color.z &&
           a->color.w == b->color.w;
}

static bool tb_gui_read_text(tb_gui_loader *loader, const tb_gui_property *property,
                             const tb_json_value *json, tb_gui_value *value)
{
    tb_text read = {0};

    (void)property;
    if (!tb_gui_read_string(loader, json, &read))
    {
        return false;
    }
    *value = tb_gui_text(read);
    return true;
}

static void tb_gui_write_text(tb_gui_saver *saver, const tb_gui_property *property,
                              const tb_gui_value *value)
{
    (void)property;
    tb_gui_write_string(saver, value->text);
}

static bool tb_gui_equal_text(const tb_gui_value *a, const tb_gui_value *b)
{
    return tb_text_equal(a->text, b->text);
}

/* The index of a name among a property's names; SIZE_MAX when it is none. */
static size_t tb_gui_name_index(const tb_gui_property *property, tb_text name)
{
    for (size_t index = 0; index < property->name_count; index++)
    {
        if (tb_text_equal(tb_text_from_cstr(property->names[index]), name))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

/* An enumeration is one of its names; one of none, an integer. */
static bool tb_gui_read_enum(tb_gui_loader *loader, const tb_gui_property *property,
                             const tb_json_value *json, tb_gui_value *value)
{
    tb_text name = {0};
    size_t index = 0;

    if (property->name_count == 0)
    {
        int64_t read = 0;
        if (!tb_gui_read_int_json(loader, json, INT32_MIN, INT32_MAX, &read))
        {
            return false;
        }
        *value = tb_gui_enum((int32_t)read);
        return true;
    }
    if (!tb_gui_read_string(loader, json, &name))
    {
        return false;
    }
    index = tb_gui_name_index(property, name);
    if (index == SIZE_MAX)
    {
        return tb_gui_fail(loader, json, "unknown name", name.bytes);
    }
    *value = tb_gui_enum((int32_t)index);
    return true;
}

static void tb_gui_write_enum(tb_gui_saver *saver, const tb_gui_property *property,
                              const tb_gui_value *value)
{
    if (value->integer >= 0 && (size_t)value->integer < property->name_count)
    {
        tb_gui_write_string(saver, tb_text_from_cstr(property->names[value->integer]));
        return;
    }
    tb_gui_saver_check(saver, tb_json_write_int64(saver->writer, value->integer));
}

/* Flags are an array of the names of the bits set, lowest first. */
static bool tb_gui_read_flags(tb_gui_loader *loader, const tb_gui_property *property,
                              const tb_json_value *json, tb_gui_value *value)
{
    uint32_t flags = 0;

    if (tb_json_type_of(json) != TB_JSON_ARRAY)
    {
        return tb_gui_fail(loader, json, "expected an array of names", NULL);
    }
    for (size_t index = 0; index < tb_json_count(json); index++)
    {
        const tb_json_value *const item = tb_json_at(json, index);
        const size_t back = tb_gui_path_index(loader, index);
        tb_text name = {0};
        size_t bit = SIZE_MAX;
        if (tb_gui_read_string(loader, item, &name))
        {
            bit = tb_gui_name_index(property, name);
            if (bit == SIZE_MAX || bit >= 32)
            {
                (void)tb_gui_fail(loader, item, "unknown name", name.bytes);
            }
        }
        tb_gui_path_back(loader, back);
        if (bit == SIZE_MAX || bit >= 32)
        {
            return false;
        }
        flags |= 1u << bit;
    }
    *value = tb_gui_flags(flags);
    return true;
}

static void tb_gui_write_flags(tb_gui_saver *saver, const tb_gui_property *property,
                               const tb_gui_value *value)
{
    tb_gui_saver_check(saver, tb_json_write_begin_array(saver->writer));
    for (size_t bit = 0; bit < property->name_count && bit < 32; bit++)
    {
        if ((value->flags & (1u << bit)) != 0)
        {
            tb_gui_write_string(saver, tb_text_from_cstr(property->names[bit]));
        }
    }
    tb_gui_saver_check(saver, tb_json_write_end_array(saver->writer));
}

static bool tb_gui_equal_flags(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->flags == b->flags;
}

/* A font is an object: its role by name, its weight, whether italic, and
 * its size, each left out when zero. */
static bool tb_gui_read_font_role(tb_gui_loader *loader, const tb_json_value *json,
                                  tb_font_role *role)
{
    tb_text name = {0};

    if (!tb_gui_read_string(loader, json, &name))
    {
        return false;
    }
    for (int index = 0; index < (int)TB_FONT_ROLE_COUNT; index++)
    {
        if (tb_text_equal(tb_text_from_cstr(tb_font_role_name((tb_font_role)index)), name))
        {
            *role = (tb_font_role)index;
            return true;
        }
    }
    return tb_gui_fail(loader, json, "unknown font role", name.bytes);
}

static bool tb_gui_read_font_member(tb_gui_loader *loader, const char *key,
                                    const tb_json_value *json, tb_font_spec *spec)
{
    int64_t weight = 0;

    if (tb_str_equal(key, "role"))
    {
        return tb_gui_read_font_role(loader, json, &spec->role);
    }
    if (tb_str_equal(key, "weight"))
    {
        if (!tb_gui_read_int_json(loader, json, 0, UINT16_MAX, &weight))
        {
            return false;
        }
        spec->weight = (uint16_t)weight;
        return true;
    }
    if (tb_str_equal(key, "italic"))
    {
        if (!tb_json_as_bool(json, &spec->italic))
        {
            return tb_gui_fail(loader, json, "expected true or false", NULL);
        }
        return true;
    }
    return tb_gui_read_float_json(loader, json, &spec->size);
}

static bool tb_gui_read_font(tb_gui_loader *loader, const tb_gui_property *property,
                             const tb_json_value *json, tb_gui_value *value)
{
    tb_font_spec spec = {0};

    (void)property;
    if (tb_json_type_of(json) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, json, "expected an object", NULL);
    }
    if (!tb_gui_check_keys(loader, json, tb_gui_font_keys, TB_GUI_COUNT_OF(tb_gui_font_keys)))
    {
        return false;
    }
    for (size_t index = 0; index < tb_json_count(json); index++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        const tb_json_value *const member = tb_json_member_at(json, index, &key, &key_length);
        const size_t back = tb_gui_path_key(loader, key);
        const bool read = tb_gui_read_font_member(loader, key, member, &spec);
        tb_gui_path_back(loader, back);
        if (!read)
        {
            return false;
        }
    }
    *value = tb_gui_font(spec);
    return true;
}

static void tb_gui_write_font(tb_gui_saver *saver, const tb_gui_property *property,
                              const tb_gui_value *value)
{
    const tb_font_spec *const spec = &value->font;

    (void)property;
    tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
    if (spec->role != TB_FONT_ROLE_UI && spec->role < TB_FONT_ROLE_COUNT)
    {
        tb_gui_write_key(saver, "role");
        tb_gui_write_string(saver, tb_text_from_cstr(tb_font_role_name(spec->role)));
    }
    if (spec->weight != 0)
    {
        tb_gui_write_key(saver, "weight");
        tb_gui_saver_check(saver, tb_json_write_int64(saver->writer, spec->weight));
    }
    if (spec->italic)
    {
        tb_gui_write_key(saver, "italic");
        tb_gui_saver_check(saver, tb_json_write_bool(saver->writer, true));
    }
    if (spec->size != 0.0f)
    {
        tb_gui_write_key(saver, "size");
        tb_gui_write_float(saver, spec->size);
    }
    tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
}

static bool tb_gui_equal_font(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->font.role == b->font.role && a->font.weight == b->font.weight &&
           a->font.italic == b->font.italic && a->font.size == b->font.size;
}

/* A value of a named type is written as its name (gui_names.c). */
static bool tb_gui_read_named(tb_gui_loader *loader, const tb_gui_property *property,
                              const tb_json_value *json, tb_gui_value *value)
{
    tb_text name = {0};

    if (!tb_gui_read_string(loader, json, &name))
    {
        return false;
    }
    if (!tb_gui_named_find(loader->gui, property->type, name, value))
    {
        return tb_gui_fail(loader, json,
                           property->type == TB_GUI_TYPE_COMMAND ? "unknown command"
                                                                 : "unknown variable",
                           name.bytes);
    }
    return true;
}

static void tb_gui_write_named(tb_gui_saver *saver, const tb_gui_property *property,
                               const tb_gui_value *value)
{
    const char *const name = tb_gui_named_name_of(saver->gui, value);

    (void)property;
    /* Only a value with a name is chosen to be written. */
    TB_ASSERT(name != NULL);
    tb_gui_write_string(saver, tb_text_from_cstr(name));
}

static bool tb_gui_equal_variable(const tb_gui_value *a, const tb_gui_value *b)
{
    return tb_var_equal(a->variable, b->variable);
}

static bool tb_gui_equal_command(const tb_gui_value *a, const tb_gui_value *b)
{
    return a->command.callback == b->command.callback && a->command.user == b->command.user;
}

/* A type with no codec here is neither read nor written. */
static const tb_gui_codec tb_gui_codecs[TB_GUI_TYPE_COUNT] = {
    [TB_GUI_TYPE_BOOL] = {tb_gui_read_bool, tb_gui_write_bool, tb_gui_equal_bool},
    [TB_GUI_TYPE_INT] = {tb_gui_read_int, tb_gui_write_int, tb_gui_equal_int},
    [TB_GUI_TYPE_FLOAT] = {tb_gui_read_float, tb_gui_write_number, tb_gui_equal_float},
    [TB_GUI_TYPE_VEC2] = {tb_gui_read_vec2, tb_gui_write_vec2, tb_gui_equal_vec2},
    [TB_GUI_TYPE_COLOR] = {tb_gui_read_color, tb_gui_write_color, tb_gui_equal_color},
    [TB_GUI_TYPE_TEXT] = {tb_gui_read_text, tb_gui_write_text, tb_gui_equal_text},
    [TB_GUI_TYPE_ENUM] = {tb_gui_read_enum, tb_gui_write_enum, tb_gui_equal_int},
    [TB_GUI_TYPE_FLAGS] = {tb_gui_read_flags, tb_gui_write_flags, tb_gui_equal_flags},
    [TB_GUI_TYPE_FONT] = {tb_gui_read_font, tb_gui_write_font, tb_gui_equal_font},
    [TB_GUI_TYPE_COMMAND] = {tb_gui_read_named, tb_gui_write_named, tb_gui_equal_command},
    [TB_GUI_TYPE_VARIABLE] = {tb_gui_read_named, tb_gui_write_named, tb_gui_equal_variable},
};

static const tb_gui_codec *tb_gui_codec_of(tb_gui_type type)
{
    if ((size_t)type >= TB_GUI_COUNT_OF(tb_gui_codecs) || tb_gui_codecs[type].read_value == NULL)
    {
        return NULL;
    }
    return &tb_gui_codecs[type];
}

/* ---------------------------------------------------------------------------
 * Loading a widget.
 * ------------------------------------------------------------------------- */

static bool tb_gui_load_widget(tb_gui_loader *loader, tb_widget parent, const tb_json_value *object,
                               size_t depth, tb_widget *made);

/* A value of a property, read by its codec and set by name. */
static bool tb_gui_load_value(tb_gui_loader *loader, tb_widget widget,
                              const tb_gui_property *property, const tb_json_value *json)
{
    const tb_gui_codec *const codec = tb_gui_codec_of(property->type);
    tb_gui_value value = {0};

    if (codec == NULL)
    {
        return tb_gui_fail(loader, json, "a property of a type files do not hold", property->name);
    }
    if ((property->flags & TB_GUI_PROPERTY_READ_ONLY) != 0)
    {
        return tb_gui_fail(loader, json, "a read-only property", property->name);
    }
    if (!codec->read_value(loader, property, json, &value))
    {
        return false;
    }
    if (!tb_widget_set(widget, property->name, &value))
    {
        return tb_gui_fail(loader, json, "a value the property refuses", property->name);
    }
    return true;
}

static bool tb_gui_load_name(tb_gui_loader *loader, tb_widget widget, const tb_json_value *json)
{
    tb_text name = {0};
    tb_gui_value value = {0};

    if (!tb_gui_read_string(loader, json, &name))
    {
        return false;
    }
    if (tb_text_find_char(name, 0, TB_TEXT_END, '/') != TB_TEXT_NOT_FOUND)
    {
        return tb_gui_fail(loader, json, "a name with a slash, which no path finds", name.bytes);
    }
    value = tb_gui_text(name);
    return tb_widget_set(widget, "name", &value);
}

static bool tb_gui_load_properties(tb_gui_loader *loader, tb_widget widget,
                                   const tb_json_value *object)
{
    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, object, "expected an object", NULL);
    }
    for (size_t index = 0; index < tb_json_count(object); index++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        const tb_json_value *const member = tb_json_member_at(object, index, &key, &key_length);
        const tb_gui_property *const property = tb_widget_find_property(widget, key);
        const size_t back = tb_gui_path_key(loader, key);
        bool loaded = false;
        if (property == NULL)
        {
            loaded = tb_gui_fail(loader, member, "unknown property", key);
        }
        else if (tb_gui_listed(tb_gui_reserved, TB_GUI_COUNT_OF(tb_gui_reserved), key) ||
                 tb_gui_listed(tb_gui_placement, TB_GUI_COUNT_OF(tb_gui_placement), key))
        {
            loaded = tb_gui_fail(loader, member, "a property written elsewhere in a widget", key);
        }
        else
        {
            loaded = tb_gui_load_value(loader, widget, property, member);
        }
        tb_gui_path_back(loader, back);
        if (!loaded)
        {
            return false;
        }
    }
    return true;
}

static bool tb_gui_load_grid(tb_gui_loader *loader, tb_widget widget, const tb_json_value *object)
{
    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, object, "expected an object", NULL);
    }
    if (!tb_gui_check_keys(loader, object, tb_gui_placement, TB_GUI_COUNT_OF(tb_gui_placement)))
    {
        return false;
    }
    if (!tb_grid(widget, NULL))
    {
        return tb_gui_fail(loader, object, "a widget the grid refuses", NULL);
    }
    for (size_t index = 0; index < tb_json_count(object); index++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        const tb_json_value *const member = tb_json_member_at(object, index, &key, &key_length);
        const size_t back = tb_gui_path_key(loader, key);
        const bool loaded =
            tb_gui_load_value(loader, widget, tb_widget_find_property(widget, key), member);
        tb_gui_path_back(loader, back);
        if (!loaded)
        {
            return false;
        }
    }
    return true;
}

static bool tb_gui_load_track_member(tb_gui_loader *loader, const char *key,
                                     const tb_json_value *json, tb_grid_track *track,
                                     tb_text *uniform)
{
    if (tb_str_equal(key, "weight"))
    {
        return tb_gui_read_float_json(loader, json, &track->weight);
    }
    if (tb_str_equal(key, "minsize"))
    {
        return tb_gui_read_float_json(loader, json, &track->minsize);
    }
    if (tb_str_equal(key, "pad"))
    {
        return tb_gui_read_float_json(loader, json, &track->pad);
    }
    return tb_gui_read_string(loader, json, uniform);
}

/* A row or column: its weight, minimum size, pad, and uniform group. */
static bool tb_gui_load_track(tb_gui_loader *loader, tb_widget widget, int axis, size_t index,
                              const tb_json_value *object)
{
    tb_grid_track track = {0};
    tb_text uniform = {0};
    bool configured = false;

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, object, "expected an object", NULL);
    }
    if (!tb_gui_check_keys(loader, object, tb_gui_track_keys, TB_GUI_COUNT_OF(tb_gui_track_keys)))
    {
        return false;
    }
    for (size_t member_index = 0; member_index < tb_json_count(object); member_index++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        const tb_json_value *const member =
            tb_json_member_at(object, member_index, &key, &key_length);
        const size_t back = tb_gui_path_key(loader, key);
        const bool read = tb_gui_load_track_member(loader, key, member, &track, &uniform);
        tb_gui_path_back(loader, back);
        if (!read)
        {
            return false;
        }
    }
    /* The document's strings end with a NUL. */
    track.uniform = uniform.bytes;
    configured = axis == TB_GUI_ROWS ? tb_grid_row_configure(widget, index, &track)
                                     : tb_grid_column_configure(widget, index, &track);
    if (!configured)
    {
        return tb_gui_fail(loader, object, "a row or column the grid refuses", NULL);
    }
    return true;
}

static bool tb_gui_load_tracks(tb_gui_loader *loader, tb_widget widget, int axis,
                               const tb_json_value *array)
{
    if (tb_json_type_of(array) != TB_JSON_ARRAY)
    {
        return tb_gui_fail(loader, array, "expected an array", NULL);
    }
    for (size_t index = 0; index < tb_json_count(array); index++)
    {
        const size_t back = tb_gui_path_index(loader, index);
        const bool loaded =
            tb_gui_load_track(loader, widget, axis, index, tb_json_at(array, index));
        tb_gui_path_back(loader, back);
        if (!loaded)
        {
            return false;
        }
    }
    return true;
}

static bool tb_gui_load_children(tb_gui_loader *loader, tb_widget widget,
                                 const tb_json_value *array, size_t depth)
{
    if (tb_json_type_of(array) != TB_JSON_ARRAY)
    {
        return tb_gui_fail(loader, array, "expected an array", NULL);
    }
    for (size_t index = 0; index < tb_json_count(array); index++)
    {
        tb_widget child = TB_WIDGET_NONE;
        const size_t back = tb_gui_path_index(loader, index);
        const bool loaded =
            tb_gui_load_widget(loader, widget, tb_json_at(array, index), depth + 1, &child);
        tb_gui_path_back(loader, back);
        if (!loaded)
        {
            return false;
        }
    }
    return true;
}

/* One of a widget's optional members, if it is there, by its loader. */
/* An image of a widget's style overrides, by the path a theme of it
 * names it by. */
static const tb_image *tb_gui_override_image(void *context, const char *path)
{
    return tb_gui_style_image_named(*(const tb_widget *)context, path);
}

/* A widget's style overrides: values by "name" or "name:state+state", as a
 * theme file's (gui_theme_file.c). */
static bool tb_gui_load_overrides(tb_gui_loader *loader, tb_widget widget,
                                  const tb_json_value *object)
{
    char name[TB_GUI_FILE_ERROR_CAPACITY] = {0};

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, object, "expected an object", NULL);
    }
    for (size_t index = 0; index < tb_json_count(object); index++)
    {
        const char *key = NULL;
        size_t key_length = 0;
        uint32_t states = 0;
        tb_style_value value = {0};
        const tb_json_value *const member = tb_json_member_at(object, index, &key, &key_length);
        const size_t back = tb_gui_path_key(loader, key);
        const char *reason = NULL;
        if (!tb_style_key_read(key, key_length, name, sizeof(name), &states))
        {
            reason = "a style key: a name, then \":\" and states joined by \"+\"";
        }
        else
        {
            reason = tb_style_value_read(name, member, tb_gui_override_image, &widget, &value);
        }
        if (reason == NULL && !tb_widget_override_style(widget, name, states, &value))
        {
            reason = "a value the style refuses";
        }
        if (reason != NULL)
        {
            return tb_gui_fail(loader, member, reason, NULL);
        }
        tb_gui_path_back(loader, back);
    }
    return true;
}

static bool tb_gui_load_part(tb_gui_loader *loader, tb_widget widget, const tb_json_value *object,
                             const char *key, size_t depth)
{
    const tb_json_value *const member = tb_gui_member(object, key);
    size_t back = 0;
    bool loaded = false;

    if (member == NULL)
    {
        return true;
    }
    back = tb_gui_path_key(loader, key);
    if (tb_str_equal(key, "name"))
    {
        loaded = tb_gui_load_name(loader, widget, member);
    }
    else if (tb_str_equal(key, "properties"))
    {
        loaded = tb_gui_load_properties(loader, widget, member);
    }
    else if (tb_str_equal(key, "overrides"))
    {
        loaded = tb_gui_load_overrides(loader, widget, member);
    }
    else if (tb_str_equal(key, "grid"))
    {
        loaded = tb_gui_load_grid(loader, widget, member);
    }
    else if (tb_str_equal(key, "columns"))
    {
        loaded = tb_gui_load_tracks(loader, widget, TB_GUI_COLUMNS, member);
    }
    else if (tb_str_equal(key, "rows"))
    {
        loaded = tb_gui_load_tracks(loader, widget, TB_GUI_ROWS, member);
    }
    else
    {
        loaded = tb_gui_load_children(loader, widget, member, depth);
    }
    tb_gui_path_back(loader, back);
    return loaded;
}

/* A widget's kind, by its registered name; NULL when it fails. */
static const tb_widget_class *tb_gui_load_kind(tb_gui_loader *loader, const tb_json_value *json)
{
    const size_t back = tb_gui_path_key(loader, "kind");
    const tb_widget_class *kind = NULL;
    tb_text name = {0};

    if (tb_gui_read_string(loader, json, &name))
    {
        kind = tb_gui_find_class(loader->gui, name.bytes);
        if (kind == NULL)
        {
            (void)tb_gui_fail(loader, json, "unknown kind", name.bytes);
        }
    }
    tb_gui_path_back(loader, back);
    return kind;
}

/* A widget and what is in it, made under `parent`; `made` is the widget,
 * from the moment it is made, for the caller to destroy on failure. */
static bool tb_gui_load_widget(tb_gui_loader *loader, tb_widget parent, const tb_json_value *object,
                               size_t depth, tb_widget *made)
{
    static const char *const parts[] = {"name",    "properties", "overrides", "grid",
                                        "columns", "rows",       "children"};
    const tb_json_value *kind_value = NULL;
    const tb_widget_class *kind = NULL;

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, object, "expected an object", NULL);
    }
    if (depth > TB_GUI_FILE_MOST_DEPTH)
    {
        return tb_gui_fail(loader, object, "widgets nested too deep", NULL);
    }
    if (!tb_gui_check_keys(loader, object, tb_gui_widget_keys, TB_GUI_COUNT_OF(tb_gui_widget_keys)))
    {
        return false;
    }
    kind_value = tb_gui_member(object, "kind");
    if (kind_value == NULL)
    {
        return tb_gui_fail(loader, object, "a widget with no kind", NULL);
    }
    kind = tb_gui_load_kind(loader, kind_value);
    if (kind == NULL)
    {
        return false;
    }
    *made = tb_widget_create(parent, kind);
    for (size_t index = 0; index < TB_GUI_COUNT_OF(parts); index++)
    {
        if (!tb_gui_load_part(loader, *made, object, parts[index], depth))
        {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Variables a file declares.
 * ------------------------------------------------------------------------- */

/* A declared variable's first value, of its type; the type's zero when the
 * file gives none. */
static bool tb_gui_read_variable_value(tb_gui_loader *loader, tb_gui_type type,
                                       const tb_json_value *json, tb_gui_value *value)
{
    int64_t integer = 0;
    double number = 0.0;

    if (type == TB_GUI_TYPE_BOOL)
    {
        return tb_gui_read_bool(loader, NULL, json, value);
    }
    if (type == TB_GUI_TYPE_INT)
    {
        if (!tb_gui_read_int_json(loader, json, INT32_MIN, INT32_MAX, &integer))
        {
            return false;
        }
        *value = tb_gui_int((int32_t)integer);
        return true;
    }
    if (type == TB_GUI_TYPE_TEXT)
    {
        return tb_gui_read_text(loader, NULL, json, value);
    }
    if (tb_json_type_of(json) != TB_JSON_NUMBER || !tb_json_as_double(json, &number) ||
        fabs(number) > (double)FLT_MAX)
    {
        return tb_gui_fail(loader, json, "expected a number", NULL);
    }
    *value = tb_gui_float((float)number);
    return true;
}

/* A declared variable's type, by its name. */
static bool tb_gui_read_variable_type(tb_gui_loader *loader, const tb_json_value *json,
                                      tb_gui_type *type)
{
    tb_text name = {0};

    if (!tb_gui_read_string(loader, json, &name))
    {
        return false;
    }
    for (size_t index = 0; index < TB_GUI_COUNT_OF(tb_gui_variable_types); index++)
    {
        if (tb_text_equal(tb_text_from_cstr(tb_gui_variable_types[index]), name))
        {
            *type = tb_gui_variable_type_of[index];
            return true;
        }
    }
    return tb_gui_fail(loader, json, "unknown variable type", name.bytes);
}

static void tb_gui_remember_var(tb_gui_loader *loader, tb_var var)
{
    if (loader->made_var_count == loader->made_var_capacity)
    {
        loader->made_var_capacity =
            loader->made_var_capacity == 0 ? 4u : loader->made_var_capacity * 2;
        loader->made_vars = tb_realloc_array(loader->made_vars, loader->made_var_capacity,
                                             sizeof(*loader->made_vars));
    }
    loader->made_vars[loader->made_var_count] = var;
    loader->made_var_count++;
}

/* Bind a variable made to the setting a declaration names. */
static bool tb_gui_bind_declared(tb_gui_loader *loader, tb_var var, const tb_json_value *json)
{
    tb_text name = {0};
    tb_setting *setting = NULL;

    if (!tb_gui_read_string(loader, json, &name))
    {
        return false;
    }
    if (loader->gui->settings == NULL)
    {
        return tb_gui_fail(loader, json, "a setting, and no settings given the GUI", name.bytes);
    }
    setting = tb_settings_find(loader->gui->settings, name.bytes);
    if (setting == NULL)
    {
        return tb_gui_fail(loader, json, "unknown setting", name.bytes);
    }
    if (!tb_var_bind_setting(var, setting))
    {
        return tb_gui_fail(loader, json, "a setting the variable's type does not bind to",
                           name.bytes);
    }
    return true;
}

/* One declaration: a variable of the name is made, unless the GUI has one,
 * which is used as it is when it is of the type. */
static bool tb_gui_load_variable(tb_gui_loader *loader, const tb_json_value *object)
{
    const tb_json_value *const name_json = tb_gui_member(object, "name");
    const tb_json_value *const type_json = tb_gui_member(object, "type");
    const tb_json_value *const value_json = tb_gui_member(object, "value");
    const tb_json_value *const setting_json = tb_gui_member(object, "setting");
    tb_gui_type type = TB_GUI_TYPE_COUNT;
    tb_gui_value value = {0};
    tb_text name = {0};
    tb_var var = TB_VAR_NONE;
    size_t back = 0;
    bool read = false;

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, object, "expected an object", NULL);
    }
    if (!tb_gui_check_keys(loader, object, tb_gui_variable_keys,
                           TB_GUI_COUNT_OF(tb_gui_variable_keys)))
    {
        return false;
    }
    if (name_json == NULL || type_json == NULL)
    {
        return tb_gui_fail(loader, object, "a variable without its name and type", NULL);
    }
    back = tb_gui_path_key(loader, "name");
    read = tb_gui_read_string(loader, name_json, &name);
    if (read && name.length == 0)
    {
        read = tb_gui_fail(loader, name_json, "a variable with an empty name", NULL);
    }
    tb_gui_path_back(loader, back);
    if (!read)
    {
        return false;
    }
    back = tb_gui_path_key(loader, "type");
    read = tb_gui_read_variable_type(loader, type_json, &type);
    tb_gui_path_back(loader, back);
    if (!read)
    {
        return false;
    }
    var = tb_gui_find_var(loader->gui, name.bytes);
    if (tb_var_alive(var))
    {
        if (tb_var_type(var) == type)
        {
            return true;
        }
        back = tb_gui_path_key(loader, "type");
        (void)tb_gui_fail(loader, type_json, "a variable of another type by this name", name.bytes);
        tb_gui_path_back(loader, back);
        return false;
    }
    value = (tb_gui_value){.type = type};
    if (type == TB_GUI_TYPE_TEXT)
    {
        value.text = tb_text_make("", 0);
    }
    if (value_json != NULL)
    {
        back = tb_gui_path_key(loader, "value");
        read = tb_gui_read_variable_value(loader, type, value_json, &value);
        tb_gui_path_back(loader, back);
        if (!read)
        {
            return false;
        }
    }
    var = tb_var_create(loader->gui, &value);
    tb_var_set_name(var, name.bytes);
    tb_gui_remember_var(loader, var);
    if (setting_json == NULL)
    {
        return true;
    }
    back = tb_gui_path_key(loader, "setting");
    read = tb_gui_bind_declared(loader, var, setting_json);
    tb_gui_path_back(loader, back);
    return read;
}

static bool tb_gui_load_variables(tb_gui_loader *loader, const tb_json_value *array)
{
    if (tb_json_type_of(array) != TB_JSON_ARRAY)
    {
        return tb_gui_fail(loader, array, "expected an array", NULL);
    }
    for (size_t index = 0; index < tb_json_count(array); index++)
    {
        const size_t back = tb_gui_path_index(loader, index);
        const bool loaded = tb_gui_load_variable(loader, tb_json_at(array, index));
        tb_gui_path_back(loader, back);
        if (!loaded)
        {
            return false;
        }
    }
    return true;
}

/* The file's own object: its format, its version, and its root. */
static bool tb_gui_load_document(tb_gui_loader *loader, tb_widget parent,
                                 const tb_json_value *document, tb_widget *made)
{
    const tb_json_value *format = NULL;
    const tb_json_value *version = NULL;
    const tb_json_value *root = NULL;
    tb_text format_name = {0};
    int64_t version_number = 0;
    size_t back = 0;
    bool loaded = false;

    if (tb_json_type_of(document) != TB_JSON_OBJECT)
    {
        return tb_gui_fail(loader, document, "expected an object", NULL);
    }
    if (!tb_gui_check_keys(loader, document, tb_gui_file_keys, TB_GUI_COUNT_OF(tb_gui_file_keys)))
    {
        return false;
    }
    format = tb_gui_member(document, "format");
    version = tb_gui_member(document, "version");
    root = tb_gui_member(document, "root");
    if (format == NULL || version == NULL || root == NULL)
    {
        return tb_gui_fail(loader, document, "a file without its format, version, and root", NULL);
    }
    back = tb_gui_path_key(loader, "format");
    loaded = tb_gui_read_string(loader, format, &format_name);
    if (loaded && !tb_text_equal(format_name, TB_TEXT(TB_GUI_FILE_FORMAT)))
    {
        loaded =
            tb_gui_fail(loader, format, "not a layout file: another format", format_name.bytes);
    }
    tb_gui_path_back(loader, back);
    if (!loaded)
    {
        return false;
    }
    back = tb_gui_path_key(loader, "version");
    loaded = tb_gui_read_int_json(loader, version, 1, INT32_MAX, &version_number);
    if (loaded && version_number > TB_GUI_FILE_VERSION)
    {
        loaded = tb_gui_fail(loader, version, "a newer version than this engine reads", NULL);
    }
    tb_gui_path_back(loader, back);
    if (!loaded)
    {
        return false;
    }
    if (tb_gui_member(document, "variables") != NULL)
    {
        back = tb_gui_path_key(loader, "variables");
        loaded = tb_gui_load_variables(loader, tb_gui_member(document, "variables"));
        tb_gui_path_back(loader, back);
        if (!loaded)
        {
            return false;
        }
    }
    back = tb_gui_path_key(loader, "root");
    loaded = tb_gui_load_widget(loader, parent, root, 1, made);
    tb_gui_path_back(loader, back);
    return loaded;
}

tb_gui_file_result tb_gui_load(tb_widget parent, const char *text, size_t length, const char *name,
                               tb_widget *widget, tb_gui_file_error *error)
{
    const tb_json_options options = {.max_depth = TB_GUI_FILE_JSON_DEPTH};
    tb_gui_file_error ignored = {0};
    tb_gui_loader loader = {0};
    tb_json_error json_error = {0};
    const tb_json_value *document = NULL;
    tb_widget made = TB_WIDGET_NONE;
    tb_arena *arena = NULL;
    tb_gui_file_result result = TB_GUI_FILE_OK;

    TB_ASSERT(text != NULL || length == 0);
    TB_ASSERT(widget != NULL);
    *widget = TB_WIDGET_NONE;
    loader.error = error != NULL ? error : &ignored;
    *loader.error = (tb_gui_file_error){.reason = ""};
    if (!tb_widget_alive(parent))
    {
        return tb_gui_fail_plain(loader.error, TB_GUI_FILE_INVALID, name,
                                 "the parent is not a widget alive");
    }
    loader.gui = parent.gui;
    loader.text = text;
    loader.length = length;
    loader.file_name = name;
    tb_gui_append(loader.path, sizeof(loader.path), &loader.path_length, "$");
    arena = tb_arena_create(0);
    if (tb_json_parse(text, length, &options, arena, &document, &json_error) != TB_JSON_OK)
    {
        *loader.error = (tb_gui_file_error){.result = TB_GUI_FILE_MALFORMED,
                                            .line = json_error.line,
                                            .column = json_error.column,
                                            .reason = json_error.reason};
        tb_gui_error_message(loader.error, name, NULL);
        result = TB_GUI_FILE_MALFORMED;
        goto cleanup;
    }
    if (!tb_gui_load_document(&loader, parent, document, &made))
    {
        /* All or nothing: what was built goes, its children with it, and
         * the variables it made. */
        tb_widget_destroy(made);
        for (size_t index = 0; index < loader.made_var_count; index++)
        {
            tb_var_destroy(loader.made_vars[index]);
        }
        result = TB_GUI_FILE_INVALID;
        goto cleanup;
    }
    *widget = made;
cleanup:
    tb_free(loader.made_vars);
    tb_arena_destroy(arena);
    return result;
}

tb_gui_file_result tb_gui_read_file(struct tb_vfs *vfs, const char *path, char **bytes,
                                    size_t *length, tb_gui_file_error *error)
{
    tb_stream stream = {0};
    uint64_t size = 0;
    tb_vfs_result opened = TB_VFS_OK;
    tb_gui_file_result result = TB_GUI_FILE_OK;

    *bytes = NULL;
    *length = 0;
    opened = tb_vfs_open(vfs, path, tb_str_length(path), NULL, &stream);
    if (opened == TB_VFS_NOT_FOUND || opened == TB_VFS_NOT_A_FILE || opened == TB_VFS_INVALID)
    {
        return tb_gui_fail_plain(error, TB_GUI_FILE_NOT_FOUND, path, "no such file");
    }
    if (opened != TB_VFS_OK)
    {
        return tb_gui_fail_plain(error, TB_GUI_FILE_INPUT_FAILED, path, tb_vfs_result_name(opened));
    }
    if (tb_stream_size(&stream, &size) != TB_STREAM_OK || size > TB_GUI_FILE_MOST_BYTES)
    {
        result = tb_gui_fail_plain(error, TB_GUI_FILE_INPUT_FAILED, path,
                                   "a file too large to read, or of no size");
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
        result =
            tb_gui_fail_plain(error, TB_GUI_FILE_INPUT_FAILED, path, "the file could not be read");
        goto cleanup;
    }
cleanup:
    (void)tb_stream_close(&stream);
    return result;
}

tb_gui_file_result tb_gui_load_file(tb_widget parent, struct tb_vfs *vfs, const char *path,
                                    tb_widget *widget, tb_gui_file_error *error)
{
    tb_gui_file_error ignored = {0};
    tb_gui_file_error *const found = error != NULL ? error : &ignored;
    char *bytes = NULL;
    size_t length = 0;
    tb_gui_file_result result = TB_GUI_FILE_OK;

    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    TB_ASSERT(widget != NULL);
    *widget = TB_WIDGET_NONE;
    result = tb_gui_read_file(vfs, path, &bytes, &length, found);
    if (result != TB_GUI_FILE_OK)
    {
        return result;
    }
    result = tb_gui_load(parent, bytes, length, path, widget, found);
    tb_free(bytes);
    return result;
}

/* ---------------------------------------------------------------------------
 * Saving a widget.
 * ------------------------------------------------------------------------- */

/* Whether a property is one the file's "properties" hold, and its value can
 * be written: neither read-only, nor of a type with no codec, nor a named
 * value with no name. */
static bool tb_gui_writable(const tb_gui_property *property)
{
    return (property->flags & TB_GUI_PROPERTY_READ_ONLY) == 0 &&
           !tb_gui_listed(tb_gui_reserved, TB_GUI_COUNT_OF(tb_gui_reserved), property->name) &&
           !tb_gui_listed(tb_gui_placement, TB_GUI_COUNT_OF(tb_gui_placement), property->name);
}

/* Which properties come in which turn of the order a file writes them. */
typedef enum tb_gui_turn
{
    TB_GUI_TURN_NAMED,  /* the kind's references to named objects: variables, commands */
    TB_GUI_TURN_KIND,   /* the kind's other properties */
    TB_GUI_TURN_COMMON, /* the common properties */
    TB_GUI_TURN_COUNT
} tb_gui_turn;

static tb_gui_turn tb_gui_turn_of(const tb_gui_property *property, size_t index, size_t common)
{
    if (index < common)
    {
        return TB_GUI_TURN_COMMON;
    }
    return tb_gui_type_is_named(property->type) ? TB_GUI_TURN_NAMED : TB_GUI_TURN_KIND;
}

/* Choose a property when its value differs from the scratch widget's, and
 * set it on the scratch widget, as a load would. */
static bool tb_gui_differs(tb_gui_saver *saver, tb_widget widget, tb_widget scratch,
                           const tb_gui_property *property)
{
    const tb_gui_codec *const codec = tb_gui_codec_of(property->type);
    tb_gui_value value = {0};
    tb_gui_value fresh = {0};
    size_t back = 0;

    if (!tb_gui_writable(property) || !tb_widget_get(widget, property->name, &value) ||
        !tb_widget_get(scratch, property->name, &fresh))
    {
        return false;
    }
    back = tb_gui_saver_key(saver, property->name);
    if (codec == NULL)
    {
        /* Whatever it holds would be lost. */
        tb_gui_save_fail(saver, TB_GUI_FILE_INVALID,
                         "a property of a type layout files cannot hold yet (issue #337)",
                         property->name);
        tb_gui_saver_back(saver, back);
        return false;
    }
    if (fresh.type == value.type && codec->equal(&value, &fresh))
    {
        tb_gui_saver_back(saver, back);
        return false;
    }
    if (tb_gui_type_is_named(property->type) && tb_gui_named_name_of(saver->gui, &value) == NULL)
    {
        tb_gui_save_fail(
            saver, TB_GUI_FILE_INVALID,
            property->type == TB_GUI_TYPE_COMMAND
                ? "a command with no name, which a file cannot refer to"
                : "a variable with no name that finds it, which a file cannot refer to",
            property->name);
        tb_gui_saver_back(saver, back);
        return false;
    }
    tb_gui_saver_back(saver, back);
    (void)tb_widget_set(scratch, property->name, &value);
    return true;
}

/* The properties to write, in the order a load sets them: the kind's
 * references to variables and commands, which may give other properties
 * their values, then its other properties, then the common ones. A
 * property is chosen when its value differs from a widget of the kind made
 * new, with the properties chosen before it set: a property a setter
 * before it changed is written only when the widget holds another value. */
static size_t tb_gui_choose(tb_gui_saver *saver, tb_widget widget, tb_widget scratch,
                            const tb_gui_property **chosen)
{
    const size_t total = tb_widget_property_count(widget);
    size_t common = 0;
    size_t count = 0;

    (void)tb_gui_common_properties(&common);
    for (int turn = 0; turn < (int)TB_GUI_TURN_COUNT; turn++)
    {
        for (size_t index = 0; index < total; index++)
        {
            const tb_gui_property *const property = tb_widget_property_at(widget, index);
            if ((int)tb_gui_turn_of(property, index, common) != turn ||
                !tb_gui_differs(saver, widget, scratch, property))
            {
                continue;
            }
            chosen[count] = property;
            count++;
        }
    }
    return count;
}

static void tb_gui_save_properties(tb_gui_saver *saver, tb_widget widget, tb_widget scratch)
{
    const size_t total = tb_widget_property_count(widget);
    const tb_gui_property **const chosen = tb_calloc(total, sizeof(*chosen));
    const size_t count = tb_gui_choose(saver, widget, scratch, chosen);

    if (count > 0)
    {
        tb_gui_write_key(saver, "properties");
        tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
        for (size_t index = 0; index < count; index++)
        {
            tb_gui_value value = {0};
            (void)tb_widget_get(widget, chosen[index]->name, &value);
            tb_gui_write_key(saver, chosen[index]->name);
            const tb_gui_codec *const codec = tb_gui_codec_of(chosen[index]->type);
            TB_ASSERT(codec != NULL);
            codec->write_value(saver, chosen[index], &value);
        }
        tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
    }
    tb_free((void *)chosen);
}

/* Its place in its parent's grid, each option left out when it is a new
 * widget's. */
static void tb_gui_save_grid(tb_gui_saver *saver, tb_widget widget, tb_widget scratch)
{
    if (!tb_gui_node_of(widget)->gridded)
    {
        return;
    }
    tb_gui_write_key(saver, "grid");
    tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
    for (size_t index = 0; index < TB_GUI_COUNT_OF(tb_gui_placement); index++)
    {
        const tb_gui_property *const property =
            tb_widget_find_property(widget, tb_gui_placement[index]);
        const tb_gui_codec *codec = NULL;
        tb_gui_value value = {0};
        tb_gui_value fresh = {0};
        /* Every widget has the common placement properties, of types with codecs. */
        TB_ASSERT(property != NULL);
        codec = tb_gui_codec_of(property->type);
        TB_ASSERT(codec != NULL);
        (void)tb_widget_get(widget, property->name, &value);
        (void)tb_widget_get(scratch, property->name, &fresh);
        if (codec->equal(&value, &fresh))
        {
            continue;
        }
        tb_gui_write_key(saver, property->name);
        codec->write_value(saver, property, &value);
    }
    tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
}

static void tb_gui_save_tracks(tb_gui_saver *saver, const tb_gui_axis *axis, const char *key)
{
    if (axis->config_count == 0)
    {
        return;
    }
    tb_gui_write_key(saver, key);
    tb_gui_saver_check(saver, tb_json_write_begin_array(saver->writer));
    for (size_t index = 0; index < axis->config_count; index++)
    {
        const tb_gui_track_config *const config = &axis->configs[index];
        tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
        if (config->weight != 0.0f)
        {
            tb_gui_write_key(saver, "weight");
            tb_gui_write_float(saver, config->weight);
        }
        if (config->minsize != 0.0f)
        {
            tb_gui_write_key(saver, "minsize");
            tb_gui_write_float(saver, config->minsize);
        }
        if (config->pad != 0.0f)
        {
            tb_gui_write_key(saver, "pad");
            tb_gui_write_float(saver, config->pad);
        }
        if (config->uniform != NULL)
        {
            tb_gui_write_key(saver, "uniform");
            tb_gui_write_string(saver, tb_text_from_cstr(config->uniform));
        }
        tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
    }
    tb_gui_saver_check(saver, tb_json_write_end_array(saver->writer));
}

/* A widget's style overrides, in the order they were set; an image by the
 * path a theme of the widget names it by. */
static void tb_gui_save_overrides(tb_gui_saver *saver, tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    char key[TB_GUI_FILE_ERROR_CAPACITY] = {0};

    if (node->override_count == 0)
    {
        return;
    }
    const size_t back = tb_gui_saver_key(saver, "overrides");
    tb_gui_write_key(saver, "overrides");
    tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
    for (size_t index = 0; index < node->override_count; index++)
    {
        const tb_gui_style_entry *const entry = &node->overrides[index];
        const char *path = NULL;
        tb_style_key_write(entry->name, entry->states, key, sizeof(key));
        const size_t item = tb_gui_saver_key(saver, key);
        tb_gui_write_key(saver, key);
        if (entry->value.type == TB_STYLE_IMAGE)
        {
            path = tb_gui_style_image_path(widget, entry->value.image.slice.image);
        }
        if (entry->value.type == TB_STYLE_IMAGE && path == NULL)
        {
            /* A file names images by path: one no theme names fails the save. */
            tb_gui_save_fail(saver, TB_GUI_FILE_INVALID, "an image no theme names by a path", NULL);
            tb_gui_saver_check(saver, tb_json_write_null(saver->writer));
        }
        else
        {
            tb_gui_saver_check(saver, tb_style_value_write(saver->writer, &entry->value, path));
        }
        tb_gui_saver_back(saver, item);
    }
    tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
    tb_gui_saver_back(saver, back);
}

static void tb_gui_save_widget(tb_gui_saver *saver, tb_widget widget, size_t depth)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    const tb_text name = tb_gui_string_text(&node->name);
    tb_widget scratch = TB_WIDGET_NONE;

    const char *const unsaved =
        node->kind->unsaved != NULL ? node->kind->unsaved(widget, node->data) : NULL;

    if (depth > TB_GUI_FILE_MOST_DEPTH)
    {
        /* Deeper than a file is read: a null keeps the document whole. */
        tb_gui_save_fail(saver, TB_GUI_FILE_INVALID, "widgets nested too deep", NULL);
        tb_gui_saver_check(saver, tb_json_write_null(saver->writer));
        return;
    }
    if (unsaved != NULL)
    {
        /* What a file cannot hold fails the save, never silently lost. */
        tb_gui_save_fail(saver, TB_GUI_FILE_INVALID,
                         "content layout files cannot hold yet (issue #337)", unsaved);
        tb_gui_saver_check(saver, tb_json_write_null(saver->writer));
        return;
    }
    tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
    tb_gui_write_key(saver, "kind");
    tb_gui_write_string(saver, tb_text_from_cstr(node->kind->name));
    if (name.length > 0)
    {
        tb_gui_write_key(saver, "name");
        tb_gui_write_string(saver, name);
    }
    /* A widget of the kind made new holds the defaults to compare against. */
    scratch = tb_widget_create(tb_gui_root(widget.gui), node->kind);
    {
        const size_t back = tb_gui_saver_key(saver, "properties");
        tb_gui_save_properties(saver, widget, scratch);
        tb_gui_saver_back(saver, back);
    }
    tb_gui_save_overrides(saver, widget);
    tb_gui_save_grid(saver, widget, scratch);
    tb_widget_destroy(scratch);
    tb_gui_save_tracks(saver, &tb_gui_node_of(widget)->axes[TB_GUI_COLUMNS], "columns");
    tb_gui_save_tracks(saver, &tb_gui_node_of(widget)->axes[TB_GUI_ROWS], "rows");
    if (tb_widget_child_count(widget) > 0)
    {
        size_t index = 0;
        const size_t back = tb_gui_saver_key(saver, "children");
        tb_gui_write_key(saver, "children");
        tb_gui_saver_check(saver, tb_json_write_begin_array(saver->writer));
        for (tb_widget child = tb_widget_first_child(widget); tb_widget_alive(child);
             child = tb_widget_next_sibling(child))
        {
            const size_t item = tb_gui_saver_index(saver, index);
            tb_gui_save_widget(saver, child, depth + 1);
            tb_gui_saver_back(saver, item);
            index++;
        }
        tb_gui_saver_check(saver, tb_json_write_end_array(saver->writer));
        tb_gui_saver_back(saver, back);
    }
    tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
}

/* The variables a widget's tree names, each once, in the order of the tree. */
static void tb_gui_collect_vars(tb_gui_saver *saver, tb_widget widget, size_t depth, tb_var **vars,
                                size_t *count, size_t *capacity)
{
    if (depth > TB_GUI_FILE_MOST_DEPTH)
    {
        return;
    }
    for (size_t index = 0; index < tb_widget_property_count(widget); index++)
    {
        const tb_gui_property *const property = tb_widget_property_at(widget, index);
        tb_gui_value value = {0};
        bool seen = false;
        if (property->type != TB_GUI_TYPE_VARIABLE ||
            !tb_widget_get(widget, property->name, &value) ||
            tb_gui_named_name_of(saver->gui, &value) == NULL)
        {
            continue;
        }
        for (size_t found = 0; found < *count && !seen; found++)
        {
            seen = tb_var_equal((*vars)[found], value.variable);
        }
        if (seen)
        {
            continue;
        }
        if (*count == *capacity)
        {
            *capacity = *capacity == 0 ? 4u : *capacity * 2;
            *vars = tb_realloc_array(*vars, *capacity, sizeof(**vars));
        }
        (*vars)[*count] = value.variable;
        (*count)++;
    }
    for (tb_widget child = tb_widget_first_child(widget); tb_widget_alive(child);
         child = tb_widget_next_sibling(child))
    {
        tb_gui_collect_vars(saver, child, depth + 1, vars, count, capacity);
    }
}

/* A variable's value, left out when it is its type's zero or a setting
 * gives it. */
static void tb_gui_save_variable_value(tb_gui_saver *saver, tb_var var)
{
    const tb_gui_type type = tb_var_type(var);
    const double number = type == TB_GUI_TYPE_FLOAT ? tb_var_get_float(var) : 0.0;

    if (type == TB_GUI_TYPE_BOOL && tb_var_get_bool(var))
    {
        tb_gui_write_key(saver, "value");
        tb_gui_saver_check(saver, tb_json_write_bool(saver->writer, true));
    }
    else if (type == TB_GUI_TYPE_INT && tb_var_get_int(var) != 0)
    {
        tb_gui_write_key(saver, "value");
        tb_gui_saver_check(saver, tb_json_write_int64(saver->writer, tb_var_get_int(var)));
    }
    else if (type == TB_GUI_TYPE_FLOAT && number != 0.0)
    {
        tb_gui_write_key(saver, "value");
        tb_gui_write_float(saver, (float)number);
    }
    else if (type == TB_GUI_TYPE_TEXT && tb_var_get_text(var).length > 0)
    {
        tb_gui_write_key(saver, "value");
        tb_gui_write_string(saver, tb_var_get_text(var));
    }
}

static void tb_gui_save_variables(tb_gui_saver *saver, tb_widget widget)
{
    tb_var *vars = NULL;
    size_t count = 0;
    size_t capacity = 0;

    tb_gui_collect_vars(saver, widget, 1, &vars, &count, &capacity);
    if (count == 0)
    {
        return;
    }
    tb_gui_write_key(saver, "variables");
    tb_gui_saver_check(saver, tb_json_write_begin_array(saver->writer));
    for (size_t index = 0; index < count; index++)
    {
        const tb_gui_type type = tb_var_type(vars[index]);
        const tb_setting *const setting = tb_var_setting(vars[index]);
        tb_gui_saver_check(saver, tb_json_write_begin_object(saver->writer));
        tb_gui_write_key(saver, "name");
        tb_gui_write_string(saver, tb_var_name(vars[index]));
        tb_gui_write_key(saver, "type");
        for (size_t named = 0; named < TB_GUI_COUNT_OF(tb_gui_variable_type_of); named++)
        {
            if (tb_gui_variable_type_of[named] == type)
            {
                tb_gui_write_string(saver, tb_text_from_cstr(tb_gui_variable_types[named]));
            }
        }
        if (setting == NULL)
        {
            tb_gui_save_variable_value(saver, vars[index]);
        }
        else
        {
            tb_gui_write_key(saver, "setting");
            tb_gui_write_string(saver, tb_text_from_cstr(tb_setting_name(setting)));
        }
        tb_gui_saver_check(saver, tb_json_write_end_object(saver->writer));
    }
    tb_gui_saver_check(saver, tb_json_write_end_array(saver->writer));
    tb_free(vars);
}

tb_gui_file_result tb_gui_save(tb_widget widget, tb_stream *sink, tb_gui_file_error *error)
{
    tb_gui_file_error ignored = {0};
    tb_gui_saver saver = {0};
    tb_json_result finished = TB_JSON_OK;
    size_t back = 0;

    TB_ASSERT(sink != NULL);
    saver.error = error != NULL ? error : &ignored;
    *saver.error = (tb_gui_file_error){.reason = ""};
    if (!tb_widget_alive(widget))
    {
        return tb_gui_fail_plain(saver.error, TB_GUI_FILE_INVALID, NULL,
                                 "the widget is not a widget alive");
    }
    saver.gui = widget.gui;
    tb_gui_append(saver.path, sizeof(saver.path), &saver.path_length, "$");
    saver.writer = tb_json_writer_create(sink, TB_JSON_INDENTED, NULL);
    tb_gui_saver_check(&saver, tb_json_write_begin_object(saver.writer));
    tb_gui_write_key(&saver, "format");
    tb_gui_write_string(&saver, TB_TEXT(TB_GUI_FILE_FORMAT));
    tb_gui_write_key(&saver, "version");
    tb_gui_saver_check(&saver, tb_json_write_int64(saver.writer, TB_GUI_FILE_VERSION));
    tb_gui_save_variables(&saver, widget);
    tb_gui_write_key(&saver, "root");
    back = tb_gui_saver_key(&saver, "root");
    tb_gui_save_widget(&saver, widget, 1);
    tb_gui_saver_back(&saver, back);
    tb_gui_saver_check(&saver, tb_json_write_end_object(saver.writer));
    finished = tb_json_writer_finish(saver.writer);
    tb_gui_saver_check(&saver, finished);
    tb_json_writer_destroy(saver.writer);
    return saver.result;
}
