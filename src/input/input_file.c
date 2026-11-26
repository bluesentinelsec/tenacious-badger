/* Input files: actions and contexts read from JSON (docs/input.md), all or
 * nothing, with errors that give a JSON path, a line and a column, and a
 * reason, as the GUI's layout files do; and written back the same way. */
#include "input_internal.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/json.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include <float.h>
#include <math.h>
#include <string.h>

/* The deepest an input file nests: a scale in a modifier in a mapping in a
 * context. */
#define TB_INPUT_FILE_JSON_DEPTH 8u
/* The largest file read: definitions, not data. */
#define TB_INPUT_FILE_MOST_BYTES ((uint64_t)16 << 20)
/* The most significant digits of a float. */
#define TB_INPUT_FLOAT_DIGITS 9
/* Bytes of a float or a double written. */
#define TB_INPUT_NUMBER_TEXT 40u
/* Whole floats below this are written whole: 100, not 1e+02. */
#define TB_INPUT_FLOAT_WHOLE 1.0e9f
/* Bytes of an index written in a path: "[18446744073709551615]". */
#define TB_INPUT_INDEX_TEXT 24u

/* ---------------------------------------------------------------------------
 * Names of enumerations.
 * ------------------------------------------------------------------------- */

static const char *const tb_input_type_names[TB_INPUT_VALUE_TYPE_COUNT] = {"bool", "axis1d",
                                                                           "axis2d", "axis3d"};
static const char *const tb_input_accumulation_names[TB_INPUT_ACCUMULATION_COUNT] = {"highest",
                                                                                     "cumulative"};
static const char *const tb_input_modifier_names[TB_INPUT_MODIFIER_TYPE_COUNT] = {
    "dead_zone", "negate", "swizzle", "scalar", "response_curve", "smooth", "settings"};
static const char *const tb_input_dead_zone_names[TB_INPUT_DEAD_ZONE_COUNT] = {"radial", "axial",
                                                                               "unscaled_radial"};
static const char *const tb_input_swizzle_names[TB_INPUT_SWIZZLE_COUNT] = {"yxz", "zyx", "xzy",
                                                                           "yzx", "zxy"};
static const char *const tb_input_source_names[TB_INPUT_SETTINGS_SOURCE_COUNT] = {
    "mouse", "gamepad", "touch", "left_stick", "right_stick"};
static const char *const tb_input_trigger_names[TB_INPUT_TRIGGER_TYPE_COUNT] = {
    "down", "pressed", "released", "hold",         "hold_and_release",
    "tap",  "pulse",   "chorded",  "chord_blocker"};
static const char *const tb_input_axis_names[3] = {"x", "y", "z"};

/* The keys of a modifier of each type, after "type". */
static const char *const tb_input_modifier_keys[TB_INPUT_MODIFIER_TYPE_COUNT][4] = {
    [TB_INPUT_MODIFIER_DEAD_ZONE] = {"kind", "lower", "upper", NULL},
    [TB_INPUT_MODIFIER_NEGATE] = {"axes", NULL},
    [TB_INPUT_MODIFIER_SWIZZLE] = {"order", NULL},
    [TB_INPUT_MODIFIER_SCALAR] = {"scale", NULL},
    [TB_INPUT_MODIFIER_RESPONSE_CURVE] = {"exponent", NULL},
    [TB_INPUT_MODIFIER_SMOOTH] = {"samples", NULL},
    [TB_INPUT_MODIFIER_SETTINGS] = {"source", NULL}};

/* The keys of a trigger of each type, after "type". */
static const char *const tb_input_trigger_keys[TB_INPUT_TRIGGER_TYPE_COUNT][5] = {
    [TB_INPUT_TRIGGER_DOWN] = {"threshold", NULL},
    [TB_INPUT_TRIGGER_PRESSED] = {"threshold", NULL},
    [TB_INPUT_TRIGGER_RELEASED] = {"threshold", NULL},
    [TB_INPUT_TRIGGER_HOLD] = {"threshold", "seconds", "one_shot", NULL},
    [TB_INPUT_TRIGGER_HOLD_AND_RELEASE] = {"threshold", "seconds", NULL},
    [TB_INPUT_TRIGGER_TAP] = {"threshold", "seconds", NULL},
    [TB_INPUT_TRIGGER_PULSE] = {"threshold", "seconds", "on_start", "limit", NULL},
    [TB_INPUT_TRIGGER_CHORDED] = {"action", NULL},
    [TB_INPUT_TRIGGER_CHORD_BLOCKER] = {"action", NULL}};

/* ---------------------------------------------------------------------------
 * Errors.
 * ------------------------------------------------------------------------- */

static void tb_input_append(char *buffer, size_t capacity, size_t *used, const char *text)
{
    const size_t length = tb_str_length(text);
    const size_t room = capacity - 1 - *used;
    const size_t taken = length < room ? length : room;

    memcpy(buffer + *used, text, taken);
    *used += taken;
    buffer[*used] = '\0';
}

static void tb_input_error_message(tb_input_file_error *error, const char *file_name,
                                   const char *detail)
{
    char place[64] = {0};
    size_t used = 0;

    error->message[0] = '\0';
    if (file_name != NULL)
    {
        tb_input_append(error->message, sizeof(error->message), &used, file_name);
        tb_input_append(error->message, sizeof(error->message), &used, ":");
    }
    if (error->line > 0)
    {
        (void)tb_fmt_buffer(place, sizeof(place), "%zu:%zu:", error->line, error->column);
        tb_input_append(error->message, sizeof(error->message), &used, place);
    }
    if (used > 0)
    {
        tb_input_append(error->message, sizeof(error->message), &used, " ");
    }
    if (error->path[0] != '\0')
    {
        tb_input_append(error->message, sizeof(error->message), &used, error->path);
        tb_input_append(error->message, sizeof(error->message), &used, ": ");
    }
    tb_input_append(error->message, sizeof(error->message), &used, error->reason);
    if (detail != NULL)
    {
        tb_input_append(error->message, sizeof(error->message), &used, " \"");
        tb_input_append(error->message, sizeof(error->message), &used, detail);
        tb_input_append(error->message, sizeof(error->message), &used, "\"");
    }
}

static tb_input_file_result tb_input_fail_plain(tb_input_file_error *error,
                                                tb_input_file_result result, const char *name,
                                                const char *reason)
{
    *error = (tb_input_file_error){.result = result, .reason = reason};
    tb_input_error_message(error, name, NULL);
    return result;
}

/* ---------------------------------------------------------------------------
 * The reader.
 * ------------------------------------------------------------------------- */

typedef struct tb_input_reader
{
    tb_input *input;
    const char *text;
    size_t length;
    const char *file_name;
    tb_input_file_error *error;
    char path[TB_INPUT_FILE_ERROR_CAPACITY];
    size_t path_length;
    /* The rules of one list read, until they are given to the input. */
    tb_input_modifier *modifiers;
    size_t modifier_count;
    tb_input_trigger *triggers;
    size_t trigger_count;
} tb_input_reader;

/* Add `.key` to the path; returns its length before, for tb_input_path_back(). */
static size_t tb_input_path_key(tb_input_reader *reader, const char *key)
{
    const size_t back = reader->path_length;
    tb_input_append(reader->path, sizeof(reader->path), &reader->path_length, ".");
    tb_input_append(reader->path, sizeof(reader->path), &reader->path_length, key);
    return back;
}

static size_t tb_input_path_index(tb_input_reader *reader, size_t index)
{
    char text[TB_INPUT_INDEX_TEXT] = {0};
    const size_t back = reader->path_length;
    (void)tb_fmt_buffer(text, sizeof(text), "[%zu]", index);
    tb_input_append(reader->path, sizeof(reader->path), &reader->path_length, text);
    return back;
}

static void tb_input_path_back(tb_input_reader *reader, size_t back)
{
    reader->path_length = back;
    reader->path[back] = '\0';
}

static tb_input_file_result tb_input_fail(tb_input_reader *reader, const tb_json_value *value,
                                          const char *reason, const char *detail)
{
    tb_input_file_error *const error = reader->error;
    const size_t offset = tb_json_offset_of(value);
    size_t line_start = 0;

    *error = (tb_input_file_error){.result = TB_INPUT_FILE_INVALID, .reason = reason, .line = 1};
    memcpy(error->path, reader->path, reader->path_length + 1);
    for (size_t index = 0; index < offset && index < reader->length; index++)
    {
        if (reader->text[index] == '\n')
        {
            error->line++;
            line_start = index + 1;
        }
    }
    error->column = offset - line_start + 1;
    tb_input_error_message(error, reader->file_name, detail);
    return TB_INPUT_FILE_INVALID;
}

/* A member, with its path, failing at it. */
static tb_input_file_result tb_input_fail_at(tb_input_reader *reader, const tb_json_value *object,
                                             const char *key, const char *reason,
                                             const char *detail)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));
    (void)tb_input_path_key(reader, key);
    return tb_input_fail(reader, value != NULL ? value : object, reason, detail);
}

/* Every key of an object is "type" or one of `names`, which ends at NULL. */
static tb_input_file_result tb_input_known_keys(tb_input_reader *reader,
                                                const tb_json_value *object,
                                                const char *const *names, size_t count)
{
    for (size_t member = 0; member < tb_json_count(object); member++)
    {
        const char *key = NULL;
        size_t length = 0;
        const tb_json_value *const value = tb_json_member_at(object, member, &key, &length);
        bool known = false;
        for (size_t index = 0; index < count && names[index] != NULL; index++)
        {
            known = known || tb_str_equal(names[index], key);
        }
        if (!known)
        {
            (void)tb_input_path_key(reader, key);
            return tb_input_fail(reader, value, "a key the format does not have", key);
        }
    }
    return TB_INPUT_FILE_OK;
}

static const char *tb_input_text_of(const tb_json_value *value)
{
    size_t length = 0;
    return value != NULL && tb_json_type_of(value) == TB_JSON_STRING
               ? tb_json_as_text(value, &length)
               : NULL;
}

/* A string member that is one of `names`: its index, or `count` when it is
 * not. A member left out is `fallback`. */
static size_t tb_input_read_name(const tb_json_value *object, const char *key,
                                 const char *const *names, size_t count, size_t fallback)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));
    const char *const text = tb_input_text_of(value);

    if (value == NULL)
    {
        return fallback;
    }
    for (size_t index = 0; text != NULL && index < count; index++)
    {
        if (tb_str_equal(names[index], text))
        {
            return index;
        }
    }
    return count;
}

/* A number member as a float; a member left out is unchanged. */
static bool tb_input_read_float(const tb_json_value *object, const char *key, float *number)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));
    double read = 0.0;

    if (value == NULL)
    {
        return true;
    }
    if (!tb_json_as_double(value, &read) || isfinite(read) == 0 || fabs(read) > (double)FLT_MAX)
    {
        return false;
    }
    *number = (float)read;
    return true;
}

static bool tb_input_read_double(const tb_json_value *object, const char *key, double *number)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));

    if (value == NULL)
    {
        return true;
    }
    return tb_json_as_double(value, number) && isfinite(*number) != 0;
}

static bool tb_input_read_bool(const tb_json_value *object, const char *key, bool *flag)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));
    return value == NULL || tb_json_as_bool(value, flag);
}

static bool tb_input_read_count(const tb_json_value *object, const char *key, uint32_t *count)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));
    uint64_t read = 0;

    if (value == NULL)
    {
        return true;
    }
    if (!tb_json_as_uint64(value, &read) || read > UINT32_MAX)
    {
        return false;
    }
    *count = (uint32_t)read;
    return true;
}

/* Three numbers, or one for all three. A member left out is refused. */
static bool tb_input_read_vector(const tb_json_value *object, const char *key,
                                 tb_input_value *vector)
{
    const tb_json_value *const value = tb_json_find(object, key, tb_str_length(key));
    float numbers[3] = {0};

    if (value == NULL)
    {
        return false;
    }
    if (tb_json_type_of(value) == TB_JSON_NUMBER)
    {
        if (!tb_input_read_float(object, key, &numbers[0]))
        {
            return false;
        }
        *vector = (tb_input_value){numbers[0], numbers[0], numbers[0]};
        return true;
    }
    if (tb_json_type_of(value) != TB_JSON_ARRAY || tb_json_count(value) != 3)
    {
        return false;
    }
    for (size_t index = 0; index < 3; index++)
    {
        double read = 0.0;
        if (!tb_json_as_double(tb_json_at(value, index), &read) || isfinite(read) == 0 ||
            fabs(read) > (double)FLT_MAX)
        {
            return false;
        }
        numbers[index] = (float)read;
    }
    *vector = (tb_input_value){numbers[0], numbers[1], numbers[2]};
    return true;
}

/* A negation's axes: an array of "x", "y" and "z". */
static bool tb_input_read_axes(const tb_json_value *object, uint32_t *axes)
{
    const tb_json_value *const value = tb_json_find(object, "axes", 4);

    if (value == NULL)
    {
        return true;
    }
    if (tb_json_type_of(value) != TB_JSON_ARRAY || tb_json_count(value) == 0)
    {
        return false;
    }
    for (size_t index = 0; index < tb_json_count(value); index++)
    {
        const char *const text = tb_input_text_of(tb_json_at(value, index));
        size_t axis = 0;
        while (axis < 3 && (text == NULL || !tb_str_equal(text, tb_input_axis_names[axis])))
        {
            axis++;
        }
        if (axis == 3)
        {
            return false;
        }
        *axes |= 1u << axis;
    }
    return true;
}

static tb_input_file_result tb_input_read_modifier_fields(tb_input_reader *reader,
                                                          const tb_json_value *object,
                                                          tb_input_modifier *modifier)
{
    size_t index = 0;

    switch (modifier->type)
    {
    case TB_INPUT_MODIFIER_DEAD_ZONE:
        index = tb_input_read_name(object, "kind", tb_input_dead_zone_names,
                                   TB_INPUT_DEAD_ZONE_COUNT, 0);
        if (index == TB_INPUT_DEAD_ZONE_COUNT)
        {
            return tb_input_fail_at(reader, object, "kind",
                                    "a dead zone's kind: radial, axial or unscaled_radial", NULL);
        }
        modifier->dead_zone = (tb_input_dead_zone)index;
        if (!tb_input_read_float(object, "lower", &modifier->lower))
        {
            return tb_input_fail_at(reader, object, "lower", "a number", NULL);
        }
        if (!tb_input_read_float(object, "upper", &modifier->upper))
        {
            return tb_input_fail_at(reader, object, "upper", "a number", NULL);
        }
        return TB_INPUT_FILE_OK;
    case TB_INPUT_MODIFIER_NEGATE:
        return tb_input_read_axes(object, &modifier->axes)
                   ? TB_INPUT_FILE_OK
                   : tb_input_fail_at(reader, object, "axes",
                                      "axes: an array of \"x\", \"y\" and \"z\"", NULL);
    case TB_INPUT_MODIFIER_SWIZZLE:
        index =
            tb_input_read_name(object, "order", tb_input_swizzle_names, TB_INPUT_SWIZZLE_COUNT, 0);
        if (index == TB_INPUT_SWIZZLE_COUNT)
        {
            return tb_input_fail_at(reader, object, "order", "an order: yxz, zyx, xzy, yzx or zxy",
                                    NULL);
        }
        modifier->order = (tb_input_swizzle)index;
        return TB_INPUT_FILE_OK;
    case TB_INPUT_MODIFIER_SCALAR:
        return tb_input_read_vector(object, "scale", &modifier->scale)
                   ? TB_INPUT_FILE_OK
                   : tb_input_fail_at(reader, object, "scale",
                                      "a scale: a number, or an array of three", NULL);
    case TB_INPUT_MODIFIER_RESPONSE_CURVE:
        return tb_input_read_vector(object, "exponent", &modifier->exponent)
                   ? TB_INPUT_FILE_OK
                   : tb_input_fail_at(reader, object, "exponent",
                                      "an exponent: a number, or an array of three", NULL);
    case TB_INPUT_MODIFIER_SMOOTH:
        return tb_input_read_count(object, "samples", &modifier->samples)
                   ? TB_INPUT_FILE_OK
                   : tb_input_fail_at(reader, object, "samples", "a whole number", NULL);
    case TB_INPUT_MODIFIER_SETTINGS:
        index = tb_input_read_name(object, "source", tb_input_source_names,
                                   TB_INPUT_SETTINGS_SOURCE_COUNT, TB_INPUT_SETTINGS_SOURCE_COUNT);
        if (index == TB_INPUT_SETTINGS_SOURCE_COUNT)
        {
            return tb_input_fail_at(reader, object, "source",
                                    "a source: mouse, gamepad, touch, left_stick or right_stick",
                                    NULL);
        }
        modifier->source = (tb_input_settings_source)index;
        return TB_INPUT_FILE_OK;
    case TB_INPUT_MODIFIER_TYPE_COUNT:
        break;
    }
    /* coverage-off: the type was read from the table of names */
    return TB_INPUT_FILE_INVALID;
    /* coverage-on */
}

static tb_input_file_result tb_input_read_modifier(tb_input_reader *reader,
                                                   const tb_json_value *object,
                                                   tb_input_modifier *modifier)
{
    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, object, "a modifier: an object", NULL);
    }
    const size_t type =
        tb_input_read_name(object, "type", tb_input_modifier_names, TB_INPUT_MODIFIER_TYPE_COUNT,
                           TB_INPUT_MODIFIER_TYPE_COUNT);
    if (type == TB_INPUT_MODIFIER_TYPE_COUNT)
    {
        return tb_input_fail_at(reader, object, "type",
                                "a modifier's type: dead_zone, negate, swizzle, scalar, "
                                "response_curve, smooth or settings",
                                tb_input_text_of(tb_json_find(object, "type", 4)));
    }
    modifier->type = (tb_input_modifier_type)type;
    const char *const *const keys = tb_input_modifier_keys[type];
    const char *const names[5] = {"type", keys[0], keys[1], keys[2], keys[3]};
    tb_input_file_result result = tb_input_known_keys(reader, object, names, 5);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_modifier_fields(reader, object, modifier);
    }
    return result;
}

static tb_input_file_result tb_input_read_trigger_fields(tb_input_reader *reader,
                                                         const tb_json_value *object,
                                                         tb_input_trigger *trigger)
{
    const char *chord = NULL;

    if (!tb_input_read_float(object, "threshold", &trigger->threshold))
    {
        return tb_input_fail_at(reader, object, "threshold", "a number", NULL);
    }
    if (!tb_input_read_double(object, "seconds", &trigger->seconds))
    {
        return tb_input_fail_at(reader, object, "seconds", "a number", NULL);
    }
    if (!tb_input_read_bool(object, "one_shot", &trigger->one_shot))
    {
        return tb_input_fail_at(reader, object, "one_shot", "true or false", NULL);
    }
    if (!tb_input_read_bool(object, "on_start", &trigger->pulse_on_start))
    {
        return tb_input_fail_at(reader, object, "on_start", "true or false", NULL);
    }
    if (!tb_input_read_count(object, "limit", &trigger->limit))
    {
        return tb_input_fail_at(reader, object, "limit", "a whole number", NULL);
    }
    if (trigger->type != TB_INPUT_TRIGGER_CHORDED &&
        trigger->type != TB_INPUT_TRIGGER_CHORD_BLOCKER)
    {
        return TB_INPUT_FILE_OK;
    }
    chord = tb_input_text_of(tb_json_find(object, "action", 6));
    if (chord == NULL)
    {
        return tb_input_fail_at(reader, object, "action", "a chord's action: its name", NULL);
    }
    trigger->chord = tb_input_find_action(reader->input, chord);
    if (trigger->chord.id == 0)
    {
        return tb_input_fail_at(reader, object, "action", "an unknown action", chord);
    }
    return TB_INPUT_FILE_OK;
}

static tb_input_file_result tb_input_read_trigger(tb_input_reader *reader,
                                                  const tb_json_value *object,
                                                  tb_input_trigger *trigger)
{
    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, object, "a trigger: an object", NULL);
    }
    const size_t type =
        tb_input_read_name(object, "type", tb_input_trigger_names, TB_INPUT_TRIGGER_TYPE_COUNT,
                           TB_INPUT_TRIGGER_TYPE_COUNT);
    if (type == TB_INPUT_TRIGGER_TYPE_COUNT)
    {
        return tb_input_fail_at(reader, object, "type",
                                "a trigger's type: down, pressed, released, hold, "
                                "hold_and_release, tap, pulse, chorded or chord_blocker",
                                tb_input_text_of(tb_json_find(object, "type", 4)));
    }
    trigger->type = (tb_input_trigger_type)type;
    const char *const *const keys = tb_input_trigger_keys[type];
    const char *const names[6] = {"type", keys[0], keys[1], keys[2], keys[3], keys[4]};
    tb_input_file_result result = tb_input_known_keys(reader, object, names, 6);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_trigger_fields(reader, object, trigger);
    }
    return result;
}

/* An optional array member. */
static tb_input_file_result tb_input_array_member(tb_input_reader *reader,
                                                  const tb_json_value *object, const char *key,
                                                  const tb_json_value **array)
{
    *array = tb_json_find(object, key, tb_str_length(key));
    if (*array != NULL && tb_json_type_of(*array) != TB_JSON_ARRAY)
    {
        return tb_input_fail_at(reader, object, key, "an array", NULL);
    }
    return TB_INPUT_FILE_OK;
}

/* An object's "modifiers" and "triggers", into the reader's lists, checked
 * as rules of `self`. */
static tb_input_file_result tb_input_read_rules(tb_input_reader *reader,
                                                const tb_json_value *object, tb_input_action self)
{
    const tb_json_value *modifiers = NULL;
    const tb_json_value *triggers = NULL;
    tb_input_file_result result = tb_input_array_member(reader, object, "modifiers", &modifiers);

    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_array_member(reader, object, "triggers", &triggers);
    }
    reader->modifier_count = modifiers != NULL ? tb_json_count(modifiers) : 0;
    reader->trigger_count = triggers != NULL ? tb_json_count(triggers) : 0;
    reader->modifiers =
        tb_realloc_array(reader->modifiers, reader->modifier_count + 1, sizeof(*reader->modifiers));
    reader->triggers =
        tb_realloc_array(reader->triggers, reader->trigger_count + 1, sizeof(*reader->triggers));
    for (size_t index = 0; result == TB_INPUT_FILE_OK && index < reader->modifier_count; index++)
    {
        const tb_json_value *const item = tb_json_at(modifiers, index);
        const size_t outer = tb_input_path_key(reader, "modifiers");
        (void)tb_input_path_index(reader, index);
        reader->modifiers[index] = (tb_input_modifier){0};
        result = tb_input_read_modifier(reader, item, &reader->modifiers[index]);
        const char *const reason =
            result == TB_INPUT_FILE_OK
                ? tb_input_check_rules(reader->input, self, &reader->modifiers[index], 1, NULL, 0)
                : NULL;
        if (reason != NULL)
        {
            result = tb_input_fail(reader, item, reason, NULL);
        }
        if (result == TB_INPUT_FILE_OK)
        {
            tb_input_path_back(reader, outer);
        }
    }
    for (size_t index = 0; result == TB_INPUT_FILE_OK && index < reader->trigger_count; index++)
    {
        const tb_json_value *const item = tb_json_at(triggers, index);
        const size_t outer = tb_input_path_key(reader, "triggers");
        (void)tb_input_path_index(reader, index);
        reader->triggers[index] = (tb_input_trigger){0};
        result = tb_input_read_trigger(reader, item, &reader->triggers[index]);
        const char *const reason =
            result == TB_INPUT_FILE_OK
                ? tb_input_check_rules(reader->input, self, NULL, 0, &reader->triggers[index], 1)
                : NULL;
        if (reason != NULL)
        {
            result = tb_input_fail(reader, item, reason, NULL);
        }
        if (result == TB_INPUT_FILE_OK)
        {
            tb_input_path_back(reader, outer);
        }
    }
    return result;
}

/* An object member that names one of the input's actions. */
static tb_input_file_result tb_input_read_action_name(tb_input_reader *reader,
                                                      const tb_json_value *object,
                                                      tb_input_action *action)
{
    const char *const name = tb_input_text_of(tb_json_find(object, "action", 6));

    if (name == NULL)
    {
        return tb_input_fail_at(reader, object, "action", "an action: its name", NULL);
    }
    *action = tb_input_find_action(reader->input, name);
    if (action->id == 0)
    {
        return tb_input_fail_at(reader, object, "action", "an unknown action", name);
    }
    return TB_INPUT_FILE_OK;
}

/* An action's name, type and accumulation; its rules come after every
 * action is made, so that a chord may name an action after it. */
static tb_input_file_result tb_input_read_action(tb_input_reader *reader,
                                                 const tb_json_value *object)
{
    static const char *const keys[] = {"name", "type", "accumulation", "modifiers", "triggers"};
    tb_input_action_desc desc = {0};

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, object, "an action: an object", NULL);
    }
    tb_input_file_result result = tb_input_known_keys(reader, object, keys, 5);
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    desc.name = tb_input_text_of(tb_json_find(object, "name", 4));
    if (desc.name == NULL || !tb_input_name_valid(desc.name))
    {
        return tb_input_fail_at(reader, object, "name",
                                "a name: lowercase letters, digits and \"_\", at most 64 bytes",
                                desc.name);
    }
    if (tb_input_find_action(reader->input, desc.name).id != 0)
    {
        return tb_input_fail_at(reader, object, "name", "a name an action already has", desc.name);
    }
    const size_t type = tb_input_read_name(object, "type", tb_input_type_names,
                                           TB_INPUT_VALUE_TYPE_COUNT, TB_INPUT_VALUE_TYPE_COUNT);
    if (type == TB_INPUT_VALUE_TYPE_COUNT)
    {
        return tb_input_fail_at(reader, object, "type",
                                "a value type: bool, axis1d, axis2d or axis3d", NULL);
    }
    const size_t accumulation =
        tb_input_read_name(object, "accumulation", tb_input_accumulation_names,
                           TB_INPUT_ACCUMULATION_COUNT, TB_INPUT_ACCUMULATE_HIGHEST);
    if (accumulation == TB_INPUT_ACCUMULATION_COUNT)
    {
        return tb_input_fail_at(reader, object, "accumulation",
                                "an accumulation: highest or cumulative", NULL);
    }
    desc.type = (tb_input_value_type)type;
    desc.accumulation = (tb_input_accumulation)accumulation;
    (void)tb_input_create_action(reader->input, &desc);
    return TB_INPUT_FILE_OK;
}

static tb_input_file_result tb_input_read_mapping(tb_input_reader *reader,
                                                  const tb_json_value *object,
                                                  tb_input_context context)
{
    static const char *const keys[] = {"action", "key", "modifiers", "triggers", "pass_through"};
    tb_input_mapping_desc desc = {0};

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, object, "a mapping: an object", NULL);
    }
    tb_input_file_result result = tb_input_known_keys(reader, object, keys, 5);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_action_name(reader, object, &desc.action);
    }
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    const char *const key = tb_input_text_of(tb_json_find(object, "key", 3));
    desc.key = key != NULL ? tb_input_find_key(key, tb_str_length(key)) : TB_INPUT_KEY_NONE;
    if (desc.key == TB_INPUT_KEY_NONE)
    {
        return tb_input_fail_at(reader, object, "key", "an unknown key", key);
    }
    if (!tb_input_read_bool(object, "pass_through", &desc.pass_through))
    {
        return tb_input_fail_at(reader, object, "pass_through", "true or false", NULL);
    }
    result = tb_input_read_rules(reader, object, desc.action);
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    desc.modifiers = reader->modifiers;
    desc.modifier_count = reader->modifier_count;
    desc.triggers = reader->triggers;
    desc.trigger_count = reader->trigger_count;
    tb_input_context_map(reader->input, context, &desc);
    return TB_INPUT_FILE_OK;
}

static tb_input_file_result tb_input_read_context_action(tb_input_reader *reader,
                                                         const tb_json_value *object,
                                                         tb_input_context context)
{
    static const char *const keys[] = {"action", "modifiers", "triggers"};
    tb_input_action_rules_desc desc = {0};

    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, object, "an action's rules: an object", NULL);
    }
    tb_input_file_result result = tb_input_known_keys(reader, object, keys, 3);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_action_name(reader, object, &desc.action);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_rules(reader, object, desc.action);
    }
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    desc.modifiers = reader->modifiers;
    desc.modifier_count = reader->modifier_count;
    desc.triggers = reader->triggers;
    desc.trigger_count = reader->trigger_count;
    tb_input_context_set_action_rules(reader->input, context, &desc);
    return TB_INPUT_FILE_OK;
}

/* Each item of an optional array member, read by `read_item`. */
typedef tb_input_file_result tb_input_item_reader(tb_input_reader *reader,
                                                  const tb_json_value *item,
                                                  tb_input_context context);

static tb_input_file_result tb_input_read_items(tb_input_reader *reader,
                                                const tb_json_value *object, const char *key,
                                                tb_input_item_reader *read_item,
                                                tb_input_context context)
{
    const tb_json_value *items = NULL;
    tb_input_file_result result = tb_input_array_member(reader, object, key, &items);

    for (size_t index = 0;
         result == TB_INPUT_FILE_OK && items != NULL && index < tb_json_count(items); index++)
    {
        const size_t back = tb_input_path_key(reader, key);
        (void)tb_input_path_index(reader, index);
        result = read_item(reader, tb_json_at(items, index), context);
        if (result == TB_INPUT_FILE_OK)
        {
            tb_input_path_back(reader, back);
        }
    }
    return result;
}

static tb_input_file_result tb_input_read_context(tb_input_reader *reader,
                                                  const tb_json_value *object,
                                                  tb_input_context unused)
{
    static const char *const keys[] = {"name", "mappings", "actions"};

    (void)unused;
    if (tb_json_type_of(object) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, object, "a context: an object", NULL);
    }
    tb_input_file_result result = tb_input_known_keys(reader, object, keys, 3);
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    const char *const name = tb_input_text_of(tb_json_find(object, "name", 4));
    if (name == NULL || !tb_input_name_valid(name))
    {
        return tb_input_fail_at(reader, object, "name",
                                "a name: lowercase letters, digits and \"_\", at most 64 bytes",
                                name);
    }
    if (tb_input_find_context(reader->input, name).id != 0)
    {
        return tb_input_fail_at(reader, object, "name", "a name a context already has", name);
    }
    const tb_input_context context = tb_input_create_context(reader->input, name);
    /* The context's own rules first, so that its mappings are made once. */
    result = tb_input_read_items(reader, object, "actions", tb_input_read_context_action, context);
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_items(reader, object, "mappings", tb_input_read_mapping, context);
    }
    return result;
}

static tb_input_file_result tb_input_read_action_item(tb_input_reader *reader,
                                                      const tb_json_value *object,
                                                      tb_input_context unused)
{
    (void)unused;
    return tb_input_read_action(reader, object);
}

/* The rules of the actions the file made, now that every action exists. */
static tb_input_file_result tb_input_read_action_rules(tb_input_reader *reader,
                                                       const tb_json_value *actions, size_t first)
{
    for (size_t index = 0; actions != NULL && index < tb_json_count(actions); index++)
    {
        const tb_input_action action = tb_input_action_at(reader->input, first + index);
        const size_t back = tb_input_path_key(reader, "actions");
        (void)tb_input_path_index(reader, index);
        const tb_input_file_result result =
            tb_input_read_rules(reader, tb_json_at(actions, index), action);
        if (result != TB_INPUT_FILE_OK)
        {
            return result;
        }
        tb_input_path_back(reader, back);
        tb_input_replace_action_rules(reader->input, action, reader->modifiers,
                                      reader->modifier_count, reader->triggers,
                                      reader->trigger_count);
    }
    return TB_INPUT_FILE_OK;
}

/* The file's top: its format and version. */
static tb_input_file_result tb_input_read_head(tb_input_reader *reader, const tb_json_value *root)
{
    static const char *const keys[] = {"format", "version", "actions", "contexts"};
    const tb_json_value *const version = tb_json_find(root, "version", 7);
    int64_t number = 0;

    if (tb_json_type_of(root) != TB_JSON_OBJECT)
    {
        return tb_input_fail(reader, root, "input definitions: an object", NULL);
    }
    const tb_input_file_result result = tb_input_known_keys(reader, root, keys, 4);
    if (result != TB_INPUT_FILE_OK)
    {
        return result;
    }
    const char *const format = tb_input_text_of(tb_json_find(root, "format", 6));
    if (format == NULL || !tb_str_equal(format, TB_INPUT_FILE_FORMAT))
    {
        return tb_input_fail_at(reader, root, "format",
                                "a format other than \"" TB_INPUT_FILE_FORMAT "\"", NULL);
    }
    if (version == NULL || !tb_json_as_int64(version, &number) || number < 1)
    {
        return tb_input_fail_at(reader, root, "version", "a version: a whole number from 1", NULL);
    }
    if (number > TB_INPUT_FILE_VERSION)
    {
        return tb_input_fail_at(reader, root, "version", "a newer version than this engine reads",
                                NULL);
    }
    return TB_INPUT_FILE_OK;
}

static tb_input_file_result tb_input_read_document(tb_input_reader *reader,
                                                   const tb_json_value *root)
{
    const size_t first = tb_input_action_count(reader->input);
    const tb_json_value *actions = NULL;
    tb_input_file_result result = tb_input_read_head(reader, root);

    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_array_member(reader, root, "actions", &actions);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_items(reader, root, "actions", tb_input_read_action_item,
                                     TB_INPUT_CONTEXT_NONE);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_action_rules(reader, actions, first);
    }
    if (result == TB_INPUT_FILE_OK)
    {
        result = tb_input_read_items(reader, root, "contexts", tb_input_read_context,
                                     TB_INPUT_CONTEXT_NONE);
    }
    return result;
}

tb_input_file_result tb_input_load(tb_input *input, const char *text, size_t length,
                                   const char *name, tb_input_file_error *error)
{
    tb_input_file_error ignored = {0};
    tb_input_reader reader = {.input = input,
                              .text = text,
                              .length = length,
                              .file_name = name,
                              .error = error != NULL ? error : &ignored,
                              .path = "$",
                              .path_length = 1};
    const tb_json_options options = {.max_depth = TB_INPUT_FILE_JSON_DEPTH};
    tb_json_error json_error = {0};
    const tb_json_value *root = NULL;
    tb_arena *const arena = tb_arena_create(0);
    const size_t action_count = tb_input_action_count(input);
    const size_t context_count = tb_input_context_count(input);
    tb_input_file_result result = TB_INPUT_FILE_OK;

    TB_ASSERT(input != NULL);
    TB_ASSERT(text != NULL || length == 0);
    *reader.error = (tb_input_file_error){.reason = ""};
    if (tb_json_parse(text, length, &options, arena, &root, &json_error) != TB_JSON_OK)
    {
        *reader.error = (tb_input_file_error){.result = TB_INPUT_FILE_MALFORMED,
                                              .line = json_error.line,
                                              .column = json_error.column,
                                              .reason = json_error.reason};
        tb_input_error_message(reader.error, name, NULL);
        result = TB_INPUT_FILE_MALFORMED;
        goto cleanup;
    }
    result = tb_input_read_document(&reader, root);
    if (result != TB_INPUT_FILE_OK)
    {
        tb_input_rollback(input, action_count, context_count);
    }
cleanup:
    tb_free(reader.modifiers);
    tb_free(reader.triggers);
    tb_arena_destroy(arena);
    return result;
}

tb_input_file_result tb_input_load_file(tb_input *input, tb_vfs *vfs, const char *path,
                                        tb_input_file_error *error)
{
    tb_input_file_error ignored = {0};
    tb_input_file_error *const found = error != NULL ? error : &ignored;
    tb_stream stream = {0};
    uint64_t size = 0;
    char *bytes = NULL;
    tb_input_file_result result = TB_INPUT_FILE_OK;

    TB_ASSERT(input != NULL);
    TB_ASSERT(vfs != NULL);
    TB_ASSERT(path != NULL);
    const tb_vfs_result opened = tb_vfs_open(vfs, path, tb_str_length(path), NULL, &stream);
    if (opened == TB_VFS_NOT_FOUND || opened == TB_VFS_NOT_A_FILE || opened == TB_VFS_INVALID)
    {
        return tb_input_fail_plain(found, TB_INPUT_FILE_NOT_FOUND, path, "no such file");
    }
    if (opened != TB_VFS_OK)
    {
        return tb_input_fail_plain(found, TB_INPUT_FILE_INPUT_FAILED, path,
                                   tb_vfs_result_name(opened));
    }
    if (tb_stream_size(&stream, &size) != TB_STREAM_OK || size > TB_INPUT_FILE_MOST_BYTES)
    {
        result = tb_input_fail_plain(found, TB_INPUT_FILE_INPUT_FAILED, path,
                                     "a file too large to read");
        goto cleanup;
    }
    bytes = tb_malloc((size_t)size + 1);
    /* coverage-off: only a failing disk stops a read of a file of known size */
    if (tb_stream_read_exact(&stream, bytes, (size_t)size) != TB_STREAM_OK)
    {
        result = tb_input_fail_plain(found, TB_INPUT_FILE_INPUT_FAILED, path,
                                     "the file could not be read");
        goto cleanup;
    }
    /* coverage-on */
    result = tb_input_load(input, bytes, (size_t)size, path, found);
cleanup:
    tb_free(bytes);
    (void)tb_stream_close(&stream);
    return result;
}

/* ---------------------------------------------------------------------------
 * The writer.
 * ------------------------------------------------------------------------- */

typedef struct tb_input_writer
{
    const tb_input *input;
    tb_json_writer *json;
    bool failed;
} tb_input_writer;

static void tb_input_check(tb_input_writer *writer, tb_json_result result)
{
    writer->failed = writer->failed || result != TB_JSON_OK;
}

static void tb_input_write_key(tb_input_writer *writer, const char *key)
{
    tb_input_check(writer, tb_json_write_key(writer->json, key, tb_str_length(key)));
}

static void tb_input_write_string(tb_input_writer *writer, const char *key, const char *text)
{
    tb_input_write_key(writer, key);
    tb_input_check(writer, tb_json_write_string(writer->json, text, tb_str_length(text)));
}

/* The fewest significant digits that read back as the same float: 0.1, not
 * the double nearest the float 0.1. */
static void tb_input_write_float(tb_input_writer *writer, float value)
{
    char text[TB_INPUT_NUMBER_TEXT] = {0};
    size_t length = 0;

    if (value == truncf(value) && fabsf(value) < TB_INPUT_FLOAT_WHOLE)
    {
        length = tb_fmt_buffer(text, sizeof(text), "%.0f", (double)value);
    }
    for (int digits = 1; length == 0 && digits <= TB_INPUT_FLOAT_DIGITS; digits++)
    {
        double back = 0.0;
        const size_t written = tb_fmt_buffer(text, sizeof(text), "%.*g", digits, (double)value);
        if (tb_decimal_parse(text, written, &back) == TB_DECIMAL_OK && (float)back == value)
        {
            length = written;
        }
    }
    tb_input_check(writer, tb_json_write_number_text(writer->json, text, length));
}

static void tb_input_write_float_member(tb_input_writer *writer, const char *key, float value)
{
    if (value != 0.0f)
    {
        tb_input_write_key(writer, key);
        tb_input_write_float(writer, value);
    }
}

static void tb_input_write_vector(tb_input_writer *writer, const char *key, tb_input_value value)
{
    tb_input_write_key(writer, key);
    if (value.x == value.y && value.y == value.z)
    {
        tb_input_write_float(writer, value.x);
        return;
    }
    tb_input_check(writer, tb_json_write_begin_array(writer->json));
    tb_input_write_float(writer, value.x);
    tb_input_write_float(writer, value.y);
    tb_input_write_float(writer, value.z);
    tb_input_check(writer, tb_json_write_end_array(writer->json));
}

static void tb_input_write_flag(tb_input_writer *writer, const char *key, bool flag)
{
    if (flag)
    {
        tb_input_write_key(writer, key);
        tb_input_check(writer, tb_json_write_bool(writer->json, true));
    }
}

static void tb_input_write_count(tb_input_writer *writer, const char *key, uint32_t count)
{
    if (count != 0)
    {
        tb_input_write_key(writer, key);
        tb_input_check(writer, tb_json_write_uint64(writer->json, count));
    }
}

static void tb_input_write_axes(tb_input_writer *writer, uint32_t axes)
{
    if (axes == 0)
    {
        return;
    }
    tb_input_write_key(writer, "axes");
    tb_input_check(writer, tb_json_write_begin_array(writer->json));
    for (size_t axis = 0; axis < 3; axis++)
    {
        if ((axes & (1u << axis)) != 0)
        {
            tb_input_check(writer,
                           tb_json_write_string(writer->json, tb_input_axis_names[axis], 1));
        }
    }
    tb_input_check(writer, tb_json_write_end_array(writer->json));
}

static void tb_input_write_modifier(tb_input_writer *writer, const tb_input_modifier *modifier)
{
    tb_input_check(writer, tb_json_write_begin_object(writer->json));
    tb_input_write_string(writer, "type", tb_input_modifier_names[modifier->type]);
    switch (modifier->type)
    {
    case TB_INPUT_MODIFIER_DEAD_ZONE:
        tb_input_write_string(writer, "kind", tb_input_dead_zone_names[modifier->dead_zone]);
        tb_input_write_float_member(writer, "lower", modifier->lower);
        tb_input_write_float_member(writer, "upper", modifier->upper);
        break;
    case TB_INPUT_MODIFIER_NEGATE:
        tb_input_write_axes(writer, modifier->axes);
        break;
    case TB_INPUT_MODIFIER_SWIZZLE:
        tb_input_write_string(writer, "order", tb_input_swizzle_names[modifier->order]);
        break;
    case TB_INPUT_MODIFIER_SCALAR:
        tb_input_write_vector(writer, "scale", modifier->scale);
        break;
    case TB_INPUT_MODIFIER_RESPONSE_CURVE:
        tb_input_write_vector(writer, "exponent", modifier->exponent);
        break;
    case TB_INPUT_MODIFIER_SMOOTH:
        tb_input_write_count(writer, "samples", modifier->samples);
        break;
    case TB_INPUT_MODIFIER_SETTINGS:
        tb_input_write_string(writer, "source", tb_input_source_names[modifier->source]);
        break;
    case TB_INPUT_MODIFIER_TYPE_COUNT:
        break;
    }
    tb_input_check(writer, tb_json_write_end_object(writer->json));
}

static void tb_input_write_trigger(tb_input_writer *writer, const tb_input_trigger *trigger)
{
    tb_input_check(writer, tb_json_write_begin_object(writer->json));
    tb_input_write_string(writer, "type", tb_input_trigger_names[trigger->type]);
    if (trigger->type == TB_INPUT_TRIGGER_CHORDED ||
        trigger->type == TB_INPUT_TRIGGER_CHORD_BLOCKER)
    {
        tb_input_write_string(writer, "action",
                              tb_input_action_def_of(writer->input, trigger->chord)->name);
        tb_input_check(writer, tb_json_write_end_object(writer->json));
        return;
    }
    tb_input_write_float_member(writer, "threshold", trigger->threshold);
    const bool timed = trigger->type >= TB_INPUT_TRIGGER_HOLD;
    if (timed && trigger->seconds != 0.0)
    {
        tb_input_write_key(writer, "seconds");
        tb_input_check(writer, tb_json_write_double(writer->json, trigger->seconds));
    }
    if (trigger->type == TB_INPUT_TRIGGER_HOLD)
    {
        tb_input_write_flag(writer, "one_shot", trigger->one_shot);
    }
    if (trigger->type == TB_INPUT_TRIGGER_PULSE)
    {
        tb_input_write_flag(writer, "on_start", trigger->pulse_on_start);
        tb_input_write_count(writer, "limit", trigger->limit);
    }
    tb_input_check(writer, tb_json_write_end_object(writer->json));
}

static void tb_input_write_rules(tb_input_writer *writer, const tb_input_rules *rules)
{
    if (rules->modifier_count > 0)
    {
        tb_input_write_key(writer, "modifiers");
        tb_input_check(writer, tb_json_write_begin_array(writer->json));
        for (size_t index = 0; index < rules->modifier_count; index++)
        {
            tb_input_write_modifier(writer, &rules->modifiers[index]);
        }
        tb_input_check(writer, tb_json_write_end_array(writer->json));
    }
    if (rules->trigger_count > 0)
    {
        tb_input_write_key(writer, "triggers");
        tb_input_check(writer, tb_json_write_begin_array(writer->json));
        for (size_t index = 0; index < rules->trigger_count; index++)
        {
            tb_input_write_trigger(writer, &rules->triggers[index]);
        }
        tb_input_check(writer, tb_json_write_end_array(writer->json));
    }
}

static void tb_input_write_action(tb_input_writer *writer, const tb_input_action_def *action)
{
    tb_input_check(writer, tb_json_write_begin_object(writer->json));
    tb_input_write_string(writer, "name", action->name);
    tb_input_write_string(writer, "type", tb_input_type_names[action->type]);
    if (action->accumulation != TB_INPUT_ACCUMULATE_HIGHEST)
    {
        tb_input_write_string(writer, "accumulation",
                              tb_input_accumulation_names[action->accumulation]);
    }
    tb_input_write_rules(writer, &action->rules);
    tb_input_check(writer, tb_json_write_end_object(writer->json));
}

static void tb_input_write_context(tb_input_writer *writer, const tb_input_context_def *context)
{
    tb_input_check(writer, tb_json_write_begin_object(writer->json));
    tb_input_write_string(writer, "name", context->name);
    if (context->action_rules_count > 0)
    {
        tb_input_write_key(writer, "actions");
        tb_input_check(writer, tb_json_write_begin_array(writer->json));
        for (size_t index = 0; index < context->action_rules_count; index++)
        {
            const tb_input_action_rules_def *const rules = &context->action_rules[index];
            tb_input_check(writer, tb_json_write_begin_object(writer->json));
            tb_input_write_string(writer, "action",
                                  tb_input_action_def_of(writer->input, rules->action)->name);
            tb_input_write_rules(writer, &rules->rules);
            tb_input_check(writer, tb_json_write_end_object(writer->json));
        }
        tb_input_check(writer, tb_json_write_end_array(writer->json));
    }
    tb_input_write_key(writer, "mappings");
    tb_input_check(writer, tb_json_write_begin_array(writer->json));
    for (size_t index = 0; index < context->mapping_count; index++)
    {
        const tb_input_mapping_def *const mapping = &context->mappings[index];
        tb_input_check(writer, tb_json_write_begin_object(writer->json));
        tb_input_write_string(writer, "action",
                              tb_input_action_def_of(writer->input, mapping->action)->name);
        tb_input_write_string(writer, "key", tb_input_key_name(mapping->key));
        tb_input_write_rules(writer, &mapping->rules);
        tb_input_write_flag(writer, "pass_through", mapping->pass_through);
        tb_input_check(writer, tb_json_write_end_object(writer->json));
    }
    tb_input_check(writer, tb_json_write_end_array(writer->json));
    tb_input_check(writer, tb_json_write_end_object(writer->json));
}

tb_input_file_result tb_input_save(const tb_input *input, tb_stream *sink,
                                   tb_input_file_error *error)
{
    tb_input_file_error ignored = {0};
    tb_input_file_error *const found = error != NULL ? error : &ignored;
    tb_input_writer writer = {.input = input};

    TB_ASSERT(input != NULL);
    TB_ASSERT(sink != NULL);
    *found = (tb_input_file_error){.reason = ""};
    writer.json = tb_json_writer_create(sink, TB_JSON_INDENTED, NULL);
    tb_input_check(&writer, tb_json_write_begin_object(writer.json));
    tb_input_write_string(&writer, "format", TB_INPUT_FILE_FORMAT);
    tb_input_write_key(&writer, "version");
    tb_input_check(&writer, tb_json_write_uint64(writer.json, TB_INPUT_FILE_VERSION));
    tb_input_write_key(&writer, "actions");
    tb_input_check(&writer, tb_json_write_begin_array(writer.json));
    for (size_t index = 0; index < tb_input_action_count(input); index++)
    {
        tb_input_write_action(&writer,
                              tb_input_action_def_of(input, tb_input_action_at(input, index)));
    }
    tb_input_check(&writer, tb_json_write_end_array(writer.json));
    tb_input_write_key(&writer, "contexts");
    tb_input_check(&writer, tb_json_write_begin_array(writer.json));
    for (size_t index = 0; index < tb_input_context_count(input); index++)
    {
        tb_input_write_context(&writer,
                               tb_input_context_def_of(input, tb_input_context_at(input, index)));
    }
    tb_input_check(&writer, tb_json_write_end_array(writer.json));
    tb_input_check(&writer, tb_json_write_end_object(writer.json));
    tb_input_check(&writer, tb_json_writer_finish(writer.json));
    tb_json_writer_destroy(writer.json);
    if (writer.failed)
    {
        return tb_input_fail_plain(found, TB_INPUT_FILE_OUTPUT_FAILED, NULL,
                                   "the stream refused what was written");
    }
    return TB_INPUT_FILE_OK;
}
