/* Variables: values the GUI owns, which widgets show and set, observers
 * watch, and engine settings follow; and the clipboard of the GUI's text. */
#include "gui_values.h"

#include "badger/assert.h"
#include "badger/decimal.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* Variables a GUI first holds room for, and traces a variable. */
#define TB_GUI_FIRST_VARS 8u
#define TB_GUI_FIRST_TRACES 4u
/* Rounds of observers an update runs at most, while observers keep setting
 * variables. */
#define TB_GUI_MOST_TRACE_PASSES 8
/* Bytes of a value of a setting written as text, its terminator included,
 * besides a text setting's. */
#define TB_GUI_SETTING_TEXT 128u
/* The words a bool is read from, true then false. */
static const char *const tb_gui_true_words[] = {"true", "1", "on", "yes"};
static const char *const tb_gui_false_words[] = {"false", "0", "off", "no"};

/* An observer. One removed while observers run has its callback cleared,
 * and is dropped after. */
typedef struct tb_gui_trace
{
    tb_var_callback *callback;
    void *user;
} tb_gui_trace;

/* A value of one of a variable's types. Text is borrowed. */
typedef struct tb_gui_var_value
{
    tb_gui_type type;
    bool boolean;
    int32_t integer;
    double number;
    tb_text text;
} tb_gui_var_value;

/* A slot of a variable. */
typedef struct tb_gui_var_slot
{
    uint32_t generation;
    bool alive;
    tb_gui_type type;
    bool boolean;
    int32_t integer;
    double number;
    tb_gui_string text;      /* a text variable's */
    tb_gui_string formatted; /* another's as text, when `formatted_ok` */
    bool formatted_ok;
    tb_gui_string name;
    uint64_t version;
    uint64_t notified; /* the version its observers last ran for */
    tb_gui_trace *traces;
    size_t trace_count;
    size_t trace_capacity;
    bool traces_cleared;
    tb_setting *setting;
    uint64_t setting_version; /* the setting's, as the variable last matched it */
    bool accepted;
    bool deferred; /* the setting takes its value only when it is applied */
    bool held;     /* deferred, and holding a value its setting does not */
} tb_gui_var_slot;

struct tb_gui_values
{
    tb_gui_var_slot *slots;
    size_t count;
    size_t capacity;
    uint32_t *free_slots;
    size_t free_count;
    size_t free_capacity;
    uint32_t notifying; /* observers running */
    tb_gui_string clipboard;
};

static tb_gui_values *tb_gui_values_of(tb_gui *gui)
{
    if (gui->values == NULL)
    {
        gui->values = TB_NEW(tb_gui_values);
    }
    return gui->values;
}

static tb_gui_var_slot *tb_gui_var_slot_of(tb_var var)
{
    tb_gui_var_slot *slot = NULL;

    if (var.gui == NULL || var.gui->values == NULL || var.generation == 0 ||
        (size_t)var.index >= var.gui->values->count)
    {
        return NULL;
    }
    slot = &var.gui->values->slots[var.index];
    if (!slot->alive || slot->generation != var.generation)
    {
        return NULL;
    }
    return slot;
}

static void tb_gui_var_release(tb_gui_var_slot *slot)
{
    const uint32_t generation = slot->generation;

    tb_gui_string_free(&slot->text);
    tb_gui_string_free(&slot->formatted);
    tb_gui_string_free(&slot->name);
    tb_free(slot->traces);
    *slot = (tb_gui_var_slot){.generation = generation == UINT32_MAX ? 1u : generation + 1u};
}

void tb_gui_vars_free(tb_gui *gui)
{
    tb_gui_values *const values = gui->values;

    if (values == NULL)
    {
        return;
    }
    for (size_t index = 0; index < values->count; index++)
    {
        tb_gui_var_release(&values->slots[index]);
    }
    tb_gui_string_free(&values->clipboard);
    tb_free(values->slots);
    tb_free(values->free_slots);
    tb_free(values);
    gui->values = NULL;
}

/* ---------------------------------------------------------------------------
 * Reading text as values, and values as text.
 * ------------------------------------------------------------------------- */

static bool tb_gui_word_is(tb_text text, const char *word)
{
    const size_t length = tb_str_length(word);

    if (text.length != length)
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        char letter = text.bytes[index];
        if (letter >= 'A' && letter <= 'Z')
        {
            letter = (char)(letter - 'A' + 'a');
        }
        if (letter != word[index])
        {
            return false;
        }
    }
    return true;
}

static bool tb_gui_read_bool(tb_text text, bool *value)
{
    for (size_t index = 0; index < TB_GUI_VALUES_COUNT_OF(tb_gui_true_words); index++)
    {
        if (tb_gui_word_is(text, tb_gui_true_words[index]))
        {
            *value = true;
            return true;
        }
        if (tb_gui_word_is(text, tb_gui_false_words[index]))
        {
            *value = false;
            return true;
        }
    }
    return false;
}

/* Decimal digits with an optional sign, within an int32_t. */
static bool tb_gui_read_int(tb_text text, int32_t *value)
{
    size_t at = 0;
    bool negative = false;
    int64_t magnitude = 0;

    if (text.length > 0 && (text.bytes[0] == '-' || text.bytes[0] == '+'))
    {
        negative = text.bytes[0] == '-';
        at = 1;
    }
    if (at == text.length)
    {
        return false;
    }
    for (; at < text.length; at++)
    {
        const char digit = text.bytes[at];
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        magnitude = magnitude * 10 + (digit - '0');
        if (magnitude > (int64_t)INT32_MAX + 1)
        {
            return false;
        }
    }
    if (!negative && magnitude > INT32_MAX)
    {
        return false;
    }
    *value = (int32_t)(negative ? -magnitude : magnitude);
    return true;
}

static bool tb_gui_read_float(tb_text text, double *value)
{
    double read = 0.0;

    if (tb_decimal_parse(text.bytes, text.length, &read) != TB_DECIMAL_OK || !isfinite(read))
    {
        return false;
    }
    *value = read;
    return true;
}

/* A number rounded to an int32_t: false when it is not finite or out of range. */
static bool tb_gui_round_int(double number, int32_t *value)
{
    const double rounded = floor(number + 0.5);

    if (!isfinite(rounded) || rounded < (double)INT32_MIN || rounded > (double)INT32_MAX)
    {
        return false;
    }
    *value = (int32_t)rounded;
    return true;
}

/* A value converted to a type; text is borrowed from the value's. */
static bool tb_gui_convert(const tb_gui_var_value *from, tb_gui_type type, char *scratch,
                           size_t scratch_size, tb_gui_var_value *to)
{
    size_t required = 0;

    *to = (tb_gui_var_value){.type = type};
    if (from->type == TB_GUI_TYPE_TEXT && type != TB_GUI_TYPE_TEXT)
    {
        if (type == TB_GUI_TYPE_BOOL)
        {
            return tb_gui_read_bool(from->text, &to->boolean);
        }
        if (type == TB_GUI_TYPE_INT)
        {
            return tb_gui_read_int(from->text, &to->integer);
        }
        return tb_gui_read_float(from->text, &to->number);
    }
    double number = from->number;
    if (from->type == TB_GUI_TYPE_BOOL)
    {
        number = from->boolean ? 1.0 : 0.0;
    }
    else if (from->type == TB_GUI_TYPE_INT)
    {
        number = (double)from->integer;
    }
    if (type == TB_GUI_TYPE_BOOL)
    {
        to->boolean = number != 0.0;
        return isfinite(number) != 0;
    }
    if (type == TB_GUI_TYPE_INT)
    {
        return tb_gui_round_int(number, &to->integer);
    }
    if (type == TB_GUI_TYPE_FLOAT)
    {
        to->number = number;
        return isfinite(number) != 0;
    }
    if (from->type == TB_GUI_TYPE_TEXT)
    {
        to->text = from->text;
        return true;
    }
    if (from->type == TB_GUI_TYPE_FLOAT)
    {
        if (!isfinite(number) ||
            tb_decimal_shortest(number, scratch, scratch_size, &required) != TB_DECIMAL_OK)
        {
            return false;
        }
        to->text = tb_text_from_cstr(scratch);
        return true;
    }
    (void)tb_fmt_buffer(scratch, scratch_size, "%d", (int)number);
    to->text = tb_text_from_cstr(scratch);
    return true;
}

static tb_gui_var_value tb_gui_slot_value(const tb_gui_var_slot *slot)
{
    return (tb_gui_var_value){.type = slot->type,
                              .boolean = slot->boolean,
                              .integer = slot->integer,
                              .number = slot->number,
                              .text = tb_gui_string_text(&slot->text)};
}

static bool tb_gui_values_equal(const tb_gui_var_value *a, const tb_gui_var_value *b)
{
    if (a->type == TB_GUI_TYPE_BOOL)
    {
        return a->boolean == b->boolean;
    }
    if (a->type == TB_GUI_TYPE_INT)
    {
        return a->integer == b->integer;
    }
    if (a->type == TB_GUI_TYPE_FLOAT)
    {
        return a->number == b->number;
    }
    return tb_text_equal(a->text, b->text);
}

/* ---------------------------------------------------------------------------
 * Settings.
 * ------------------------------------------------------------------------- */

static bool tb_gui_binds(tb_gui_type type, tb_setting_type setting)
{
    if (type == TB_GUI_TYPE_BOOL)
    {
        return setting == TB_SETTING_BOOLEAN;
    }
    if (type == TB_GUI_TYPE_INT)
    {
        return setting == TB_SETTING_INTEGER || setting == TB_SETTING_CHOICE;
    }
    if (type == TB_GUI_TYPE_FLOAT)
    {
        return setting == TB_SETTING_NUMBER || setting == TB_SETTING_INTEGER;
    }
    return true;
}

static int32_t tb_gui_clamp_int(int64_t value)
{
    if (value < INT32_MIN)
    {
        return INT32_MIN;
    }
    return value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

/* A setting's value in a variable's type, its text in `buffer` (owned by
 * the caller, freed with tb_free()). */
static tb_gui_var_value tb_gui_setting_value(const tb_setting *setting, tb_gui_type type,
                                             char **buffer)
{
    const tb_setting_type kind = tb_setting_type_of(setting);
    tb_gui_var_value value = {.type = type};
    size_t capacity = TB_GUI_SETTING_TEXT;

    if (type == TB_GUI_TYPE_TEXT)
    {
        if (kind == TB_SETTING_TEXT)
        {
            capacity = tb_setting_text_limit(setting) + 1;
        }
        *buffer = tb_malloc(capacity);
        (*buffer)[0] = '\0';
        if (kind == TB_SETTING_TEXT)
        {
            (void)tb_setting_text(setting, *buffer, capacity);
        }
        else if (kind == TB_SETTING_CHOICE)
        {
            (void)tb_fmt_buffer(*buffer, capacity, "%s",
                                tb_setting_choice_name(setting, tb_setting_choice(setting)));
        }
        else
        {
            (void)tb_setting_format(setting, TB_SETTING_LAYER_COUNT, *buffer, capacity);
        }
        value.text = tb_text_from_cstr(*buffer);
        return value;
    }
    switch (kind)
    {
    case TB_SETTING_BOOLEAN:
        value.boolean = tb_setting_boolean(setting);
        break;
    case TB_SETTING_INTEGER:
        value.integer = tb_gui_clamp_int(tb_setting_integer(setting));
        value.number = (double)tb_setting_integer(setting);
        break;
    case TB_SETTING_NUMBER:
        value.number = tb_setting_number(setting);
        break;
    case TB_SETTING_CHOICE:
        value.integer = tb_gui_clamp_int((int64_t)tb_setting_choice(setting));
        break;
    case TB_SETTING_TEXT:
        break;
    }
    return value;
}

/* Give a setting a variable's value: whether it took it. */
static bool tb_gui_write_setting(tb_setting *setting, const tb_gui_var_value *value)
{
    const tb_setting_type kind = tb_setting_type_of(setting);
    tb_setting_result result = TB_SETTING_INVALID;

    if (value->type == TB_GUI_TYPE_TEXT)
    {
        char *const text = tb_malloc(value->text.length + 1);
        if (value->text.length > 0)
        {
            memcpy(text, value->text.bytes, value->text.length);
        }
        text[value->text.length] = '\0';
        if (kind == TB_SETTING_TEXT)
        {
            result = tb_setting_set_text(setting, text);
        }
        else if (kind == TB_SETTING_CHOICE)
        {
            const size_t choice = tb_setting_find_choice(setting, text);
            result = choice < tb_setting_choice_count(setting)
                         ? tb_setting_set_choice(setting, choice)
                         : TB_SETTING_OUT_OF_RANGE;
        }
        else
        {
            result = tb_setting_parse(setting, TB_SETTING_LAYER_RUNTIME, value->text.bytes,
                                      value->text.length);
        }
        tb_free(text);
        return result == TB_SETTING_OK;
    }
    switch (kind)
    {
    case TB_SETTING_BOOLEAN:
        result = tb_setting_set_boolean(setting, value->boolean);
        break;
    case TB_SETTING_INTEGER:
        result = value->type == TB_GUI_TYPE_FLOAT
                     ? tb_setting_set_integer(setting, (int64_t)floor(value->number + 0.5))
                     : tb_setting_set_integer(setting, value->integer);
        break;
    case TB_SETTING_NUMBER:
        result = tb_setting_set_number(setting, value->number);
        break;
    case TB_SETTING_CHOICE:
        result = value->integer >= 0 ? tb_setting_set_choice(setting, (size_t)value->integer)
                                     : TB_SETTING_OUT_OF_RANGE;
        break;
    case TB_SETTING_TEXT:
        break;
    }
    return result == TB_SETTING_OK;
}

/* ---------------------------------------------------------------------------
 * Storing a value.
 * ------------------------------------------------------------------------- */

/* Put a value of the slot's type in it: a change when it differs. */
static void tb_gui_store(tb_gui_var_slot *slot, const tb_gui_var_value *value)
{
    const tb_gui_var_value old = tb_gui_slot_value(slot);

    if (tb_gui_values_equal(&old, value))
    {
        return;
    }
    slot->boolean = value->boolean;
    slot->integer = value->integer;
    slot->number = value->number;
    if (slot->type == TB_GUI_TYPE_TEXT)
    {
        tb_gui_string_set(&slot->text, value->text);
    }
    slot->formatted_ok = false;
    slot->version++;
}

/* Take a bound setting's value. */
static void tb_gui_take_setting(tb_gui_var_slot *slot)
{
    char *buffer = NULL;
    const tb_gui_var_value value = tb_gui_setting_value(slot->setting, slot->type, &buffer);

    tb_gui_store(slot, &value);
    tb_free(buffer);
    slot->setting_version = tb_setting_version(slot->setting);
    slot->accepted = true;
    slot->held = false;
}

/* Whether a variable holds its setting's value, in the variable's type. */
static bool tb_gui_matches_setting(const tb_gui_var_slot *slot)
{
    char *buffer = NULL;
    const tb_gui_var_value setting = tb_gui_setting_value(slot->setting, slot->type, &buffer);
    const tb_gui_var_value held = tb_gui_slot_value(slot);
    const bool equal = tb_gui_values_equal(&held, &setting);

    tb_free(buffer);
    return equal;
}

/* Whether a setting would take a value, without giving it: the checks it
 * makes of a value set at its runtime layer, its range, its choices, and its
 * length. */
static bool tb_gui_setting_accepts(const tb_setting *setting, const tb_gui_var_value *value)
{
    const bool text = value->type == TB_GUI_TYPE_TEXT;
    int32_t integer = value->integer;
    double number = value->type == TB_GUI_TYPE_INT ? (double)value->integer : value->number;
    int64_t least = 0;
    int64_t most = 0;
    double lowest = 0.0;
    double highest = 0.0;
    bool boolean = false;

    switch (tb_setting_type_of(setting))
    {
    case TB_SETTING_BOOLEAN:
        return !text || tb_gui_read_bool(value->text, &boolean);
    case TB_SETTING_INTEGER:
        if (text && !tb_gui_read_int(value->text, &integer))
        {
            return false;
        }
        if (value->type == TB_GUI_TYPE_FLOAT && !tb_gui_round_int(value->number, &integer))
        {
            return false;
        }
        tb_setting_integer_range(setting, &least, &most);
        return integer >= least && integer <= most;
    case TB_SETTING_NUMBER:
        if (text && !tb_gui_read_float(value->text, &number))
        {
            return false;
        }
        tb_setting_number_range(setting, &lowest, &highest);
        return isfinite(number) && number >= lowest && number <= highest;
    case TB_SETTING_CHOICE: {
        if (!text)
        {
            return integer >= 0 && (size_t)integer < tb_setting_choice_count(setting);
        }
        char *const name = tb_malloc(value->text.length + 1);
        if (value->text.length > 0)
        {
            memcpy(name, value->text.bytes, value->text.length);
        }
        name[value->text.length] = '\0';
        const bool found = tb_setting_find_choice(setting, name) < tb_setting_choice_count(setting);
        tb_free(name);
        return found;
    }
    case TB_SETTING_TEXT:
        break;
    }
    return value->text.length <= tb_setting_text_limit(setting);
}

/* Hold a value in a deferred variable, for its setting to take when it is
 * applied: refused as its setting would refuse it, but for text, which is
 * kept as typed. */
static bool tb_gui_hold(tb_gui_var_slot *slot, const tb_gui_var_value *value)
{
    const bool acceptable = tb_gui_setting_accepts(slot->setting, value);

    if (!acceptable && slot->type != TB_GUI_TYPE_TEXT)
    {
        return false;
    }
    tb_gui_store(slot, value);
    slot->accepted = acceptable;
    slot->held = !tb_gui_matches_setting(slot);
    return true;
}

/* Take the escapes out of a text setting's value as a file writes it,
 * quoted, in place: what is left is the text. */
static tb_text tb_gui_unquote(char *written, size_t length)
{
    size_t kept = 0;

    if (length < 2 || written[0] != '"' || written[length - 1] != '"')
    {
        return tb_text_make(written, length);
    }
    for (size_t at = 1; at + 1 < length; at++)
    {
        char letter = written[at];
        if (letter == '\\' && at + 2 < length)
        {
            at++;
            letter = written[at];
            if (letter == 'n')
            {
                letter = '\n';
            }
            else if (letter == 'r')
            {
                letter = '\r';
            }
            else if (letter == 't')
            {
                letter = '\t';
            }
        }
        written[kept] = letter;
        kept++;
    }
    written[kept] = '\0';
    return tb_text_make(written, kept);
}

/* A setting's value without the game's own choices: its default file's,
 * or else its definition's; a choice in a variable's type, any other as
 * the text a file writes, for the caller to convert. Its text is in
 * `buffer` (the caller's, freed with tb_free()). */
static tb_gui_var_value tb_gui_setting_default(const tb_setting *setting, tb_gui_type type,
                                               char **buffer)
{
    const tb_setting_type kind = tb_setting_type_of(setting);
    const tb_setting_layer layer = tb_setting_has_layer(setting, TB_SETTING_LAYER_DEFAULT_FILE)
                                       ? TB_SETTING_LAYER_DEFAULT_FILE
                                       : TB_SETTING_LAYER_BUILT_IN;
    /* A text setting's text, each byte escaped at worst, and its quotes. */
    const size_t capacity =
        kind == TB_SETTING_TEXT ? tb_setting_text_limit(setting) * 2u + 3u : TB_GUI_SETTING_TEXT;
    tb_gui_var_value value = {.type = TB_GUI_TYPE_TEXT};
    size_t length = 0;

    *buffer = tb_calloc(capacity, 1);
    length = tb_setting_format(setting, layer, *buffer, capacity);
    if (length >= capacity)
    {
        length = 0;
        (*buffer)[0] = '\0';
    }
    if (kind == TB_SETTING_TEXT)
    {
        value.text = tb_gui_unquote(*buffer, length);
        return value;
    }
    value.text = tb_text_make(*buffer, length);
    if (kind == TB_SETTING_CHOICE && type != TB_GUI_TYPE_TEXT)
    {
        value.type = TB_GUI_TYPE_INT;
        value.integer = tb_gui_clamp_int((int64_t)tb_setting_find_choice(setting, *buffer));
    }
    return value;
}

/* Set a variable to a value of any type, through its setting. */
static bool tb_gui_var_assign(tb_var var, const tb_gui_var_value *given)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    char scratch[TB_DECIMAL_SHORTEST_SIZE] = {0};
    tb_gui_var_value value = {0};

    if (slot == NULL || !tb_gui_convert(given, slot->type, scratch, sizeof(scratch), &value))
    {
        return false;
    }
    if (slot->setting == NULL)
    {
        tb_gui_store(slot, &value);
        return true;
    }
    if (slot->deferred)
    {
        return tb_gui_hold(slot, &value);
    }
    const bool taken = tb_gui_write_setting(slot->setting, &value);
    if (slot->type == TB_GUI_TYPE_TEXT)
    {
        /* Text is kept as typed; the setting follows when it can. */
        tb_gui_store(slot, &value);
        slot->accepted = taken;
        slot->setting_version = tb_setting_version(slot->setting);
        return true;
    }
    if (!taken)
    {
        return false;
    }
    tb_gui_take_setting(slot);
    return true;
}

/* ---------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

static uint32_t tb_gui_take_var_slot(tb_gui_values *values)
{
    if (values->free_count > 0)
    {
        values->free_count--;
        return values->free_slots[values->free_count];
    }
    if (values->count == values->capacity)
    {
        values->capacity = values->capacity == 0 ? TB_GUI_FIRST_VARS : values->capacity * 2;
        values->slots = tb_realloc_array(values->slots, values->capacity, sizeof(*values->slots));
    }
    TB_ASSERT_MSG(values->count < UINT32_MAX, "too many variables");
    values->slots[values->count] = (tb_gui_var_slot){.generation = 1};
    values->count++;
    return (uint32_t)(values->count - 1);
}

tb_var tb_var_create(tb_gui *gui, const tb_gui_value *initial)
{
    tb_gui_values *values = NULL;
    tb_gui_var_slot *slot = NULL;
    uint32_t index = 0;

    TB_ASSERT(gui != NULL);
    TB_ASSERT(initial != NULL);
    if (initial->type != TB_GUI_TYPE_BOOL && initial->type != TB_GUI_TYPE_INT &&
        initial->type != TB_GUI_TYPE_FLOAT && initial->type != TB_GUI_TYPE_TEXT)
    {
        return TB_VAR_NONE;
    }
    if (initial->type == TB_GUI_TYPE_FLOAT && !isfinite(initial->number))
    {
        return TB_VAR_NONE;
    }
    values = tb_gui_values_of(gui);
    index = tb_gui_take_var_slot(values);
    slot = &values->slots[index];
    slot->alive = true;
    slot->type = initial->type;
    slot->accepted = true;
    if (initial->type == TB_GUI_TYPE_BOOL)
    {
        slot->boolean = initial->boolean;
    }
    else if (initial->type == TB_GUI_TYPE_INT)
    {
        slot->integer = initial->integer;
    }
    else if (initial->type == TB_GUI_TYPE_FLOAT)
    {
        slot->number = (double)initial->number;
    }
    else if (initial->type == TB_GUI_TYPE_TEXT)
    {
        tb_gui_string_set(&slot->text, initial->text);
    }
    /* Made, not changed: its observers have nothing to see yet. */
    slot->version = 1;
    slot->notified = 1;
    return (tb_var){gui, index, slot->generation};
}

tb_var tb_var_bool(tb_gui *gui, bool value)
{
    const tb_gui_value initial = tb_gui_bool(value);
    return tb_var_create(gui, &initial);
}

tb_var tb_var_int(tb_gui *gui, int32_t value)
{
    const tb_gui_value initial = tb_gui_int(value);
    return tb_var_create(gui, &initial);
}

tb_var tb_var_float(tb_gui *gui, double value)
{
    tb_var var = TB_VAR_NONE;
    const tb_gui_value initial = tb_gui_float(0.0f);

    TB_ASSERT(isfinite(value));
    var = tb_var_create(gui, &initial);
    gui->values->slots[var.index].number = value;
    return var;
}

tb_var tb_var_string(tb_gui *gui, const char *value)
{
    TB_ASSERT(value != NULL);
    const tb_gui_value initial = tb_gui_text(tb_text_from_cstr(value));
    return tb_var_create(gui, &initial);
}

void tb_var_destroy(tb_var var)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    tb_gui_values *const values = var.gui != NULL ? var.gui->values : NULL;

    if (slot == NULL)
    {
        return;
    }
    TB_ASSERT(values != NULL);
    tb_gui_var_release(slot);
    if (values->free_count == values->free_capacity)
    {
        values->free_capacity =
            values->free_capacity == 0 ? TB_GUI_FIRST_VARS : values->free_capacity * 2;
        values->free_slots = tb_realloc_array(values->free_slots, values->free_capacity,
                                              sizeof(*values->free_slots));
    }
    values->free_slots[values->free_count] = var.index;
    values->free_count++;
}

bool tb_var_alive(tb_var var)
{
    return tb_gui_var_slot_of(var) != NULL;
}

bool tb_var_equal(tb_var a, tb_var b)
{
    return a.gui == b.gui && a.index == b.index && a.generation == b.generation;
}

tb_gui_type tb_var_type(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot != NULL ? slot->type : TB_GUI_TYPE_COUNT;
}

uint64_t tb_var_version(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot != NULL ? slot->version : 0;
}

/* ---------------------------------------------------------------------------
 * Reading and setting.
 * ------------------------------------------------------------------------- */

bool tb_var_get(tb_var var, tb_gui_value *value)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);

    TB_ASSERT(value != NULL);
    if (slot == NULL)
    {
        return false;
    }
    if (slot->type == TB_GUI_TYPE_BOOL)
    {
        *value = tb_gui_bool(slot->boolean);
    }
    else if (slot->type == TB_GUI_TYPE_INT)
    {
        *value = tb_gui_int(slot->integer);
    }
    else if (slot->type == TB_GUI_TYPE_FLOAT)
    {
        *value = tb_gui_float((float)slot->number);
    }
    else
    {
        *value = tb_gui_text(tb_gui_string_text(&slot->text));
    }
    return true;
}

bool tb_var_set(tb_var var, const tb_gui_value *value)
{
    TB_ASSERT(value != NULL);
    if (value->type == TB_GUI_TYPE_BOOL)
    {
        return tb_var_set_bool(var, value->boolean);
    }
    if (value->type == TB_GUI_TYPE_INT)
    {
        return tb_var_set_int(var, value->integer);
    }
    if (value->type == TB_GUI_TYPE_FLOAT)
    {
        return tb_var_set_float(var, (double)value->number);
    }
    if (value->type == TB_GUI_TYPE_TEXT)
    {
        return (value->text.bytes != NULL || value->text.length == 0) &&
               tb_gui_var_set_span(var, value->text);
    }
    return false;
}

bool tb_var_set_bool(tb_var var, bool value)
{
    const tb_gui_var_value given = {.type = TB_GUI_TYPE_BOOL, .boolean = value};
    return tb_gui_var_assign(var, &given);
}

bool tb_var_set_int(tb_var var, int32_t value)
{
    const tb_gui_var_value given = {.type = TB_GUI_TYPE_INT, .integer = value};
    return tb_gui_var_assign(var, &given);
}

bool tb_var_set_float(tb_var var, double value)
{
    const tb_gui_var_value given = {.type = TB_GUI_TYPE_FLOAT, .number = value};
    return isfinite(value) && tb_gui_var_assign(var, &given);
}

bool tb_gui_var_set_span(tb_var var, tb_text text)
{
    const tb_gui_var_value given = {.type = TB_GUI_TYPE_TEXT, .text = text};
    return tb_gui_var_assign(var, &given);
}

bool tb_var_set_text(tb_var var, const char *text)
{
    TB_ASSERT(text != NULL);
    return tb_gui_var_set_span(var, tb_text_from_cstr(text));
}

bool tb_var_get_bool(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    const tb_gui_var_value value = slot != NULL ? tb_gui_slot_value(slot) : (tb_gui_var_value){0};
    tb_gui_var_value read = {0};

    if (slot == NULL || !tb_gui_convert(&value, TB_GUI_TYPE_BOOL, NULL, 0, &read))
    {
        return false;
    }
    return read.boolean;
}

int32_t tb_var_get_int(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    const tb_gui_var_value value = slot != NULL ? tb_gui_slot_value(slot) : (tb_gui_var_value){0};
    tb_gui_var_value read = {0};
    int32_t rounded = 0;

    if (slot == NULL)
    {
        return 0;
    }
    if (slot->type == TB_GUI_TYPE_TEXT)
    {
        /* Text of a number reads as its nearest integer, as a spin box's. */
        if (!tb_gui_convert(&value, TB_GUI_TYPE_FLOAT, NULL, 0, &read) ||
            !tb_gui_round_int(read.number, &rounded))
        {
            return 0;
        }
        return rounded;
    }
    return tb_gui_convert(&value, TB_GUI_TYPE_INT, NULL, 0, &read) ? read.integer : 0;
}

double tb_var_get_float(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    const tb_gui_var_value value = slot != NULL ? tb_gui_slot_value(slot) : (tb_gui_var_value){0};
    tb_gui_var_value read = {0};

    if (slot == NULL || !tb_gui_convert(&value, TB_GUI_TYPE_FLOAT, NULL, 0, &read))
    {
        return 0.0;
    }
    return read.number;
}

tb_text tb_var_get_text(tb_var var)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    char scratch[TB_DECIMAL_SHORTEST_SIZE] = {0};
    tb_gui_var_value read = {0};

    if (slot == NULL)
    {
        return tb_text_make("", 0);
    }
    if (slot->type == TB_GUI_TYPE_TEXT)
    {
        return tb_gui_string_text(&slot->text);
    }
    if (!slot->formatted_ok)
    {
        const tb_gui_var_value value = tb_gui_slot_value(slot);
        if (!tb_gui_convert(&value, TB_GUI_TYPE_TEXT, scratch, sizeof(scratch), &read))
        {
            read.text = tb_text_make("", 0);
        }
        tb_gui_string_set(&slot->formatted, read.text);
        slot->formatted_ok = true;
    }
    return tb_gui_string_text(&slot->formatted);
}

bool tb_gui_var_holds(tb_var var, tb_text text)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    const tb_gui_var_value given = {.type = TB_GUI_TYPE_TEXT, .text = text};
    tb_gui_var_value read = {0};

    if (slot == NULL || !tb_gui_convert(&given, slot->type, NULL, 0, &read))
    {
        return false;
    }
    const tb_gui_var_value value = tb_gui_slot_value(slot);
    return tb_gui_values_equal(&value, &read);
}

bool tb_gui_var_of_widget(tb_widget widget, tb_var var)
{
    return var.gui == NULL || var.gui == widget.gui;
}

/* ---------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

void tb_var_set_name(tb_var var, const char *name)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);

    TB_ASSERT(name != NULL);
    if (slot != NULL)
    {
        tb_gui_string_set(&slot->name, tb_text_from_cstr(name));
    }
}

tb_text tb_var_name(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot != NULL ? tb_gui_string_text(&slot->name) : tb_text_make("", 0);
}

tb_var tb_gui_find_var(tb_gui *gui, const char *name)
{
    const tb_text wanted = tb_text_from_cstr(name);

    TB_ASSERT(gui != NULL);
    TB_ASSERT(name != NULL);
    if (gui->values == NULL || wanted.length == 0)
    {
        return TB_VAR_NONE;
    }
    for (size_t index = 0; index < gui->values->count; index++)
    {
        const tb_gui_var_slot *const slot = &gui->values->slots[index];
        if (slot->alive && tb_text_equal(tb_gui_string_text(&slot->name), wanted))
        {
            return (tb_var){gui, (uint32_t)index, slot->generation};
        }
    }
    return TB_VAR_NONE;
}

/* ---------------------------------------------------------------------------
 * Settings, bound.
 * ------------------------------------------------------------------------- */

bool tb_var_bind_setting(tb_var var, tb_setting *setting)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);

    if (slot == NULL)
    {
        return false;
    }
    if (setting == NULL)
    {
        slot->setting = NULL;
        slot->accepted = true;
        slot->deferred = false;
        slot->held = false;
        return true;
    }
    if (!tb_gui_binds(slot->type, tb_setting_type_of(setting)))
    {
        return false;
    }
    slot->setting = setting;
    slot->deferred = false;
    tb_gui_take_setting(slot);
    return true;
}

bool tb_var_bind_setting_deferred(tb_var var, tb_setting *setting)
{
    tb_gui_var_slot *slot = NULL;

    TB_ASSERT(setting != NULL);
    if (!tb_var_bind_setting(var, setting))
    {
        return false;
    }
    slot = tb_gui_var_slot_of(var);
    slot->deferred = true;
    return true;
}

bool tb_var_deferred(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot != NULL && slot->deferred;
}

bool tb_var_pending(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot != NULL && slot->held;
}

bool tb_var_apply(tb_var var)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);

    if (slot == NULL)
    {
        return false;
    }
    if (!slot->held)
    {
        return true;
    }
    const tb_gui_var_value value = tb_gui_slot_value(slot);
    if (!tb_gui_write_setting(slot->setting, &value))
    {
        return false;
    }
    tb_gui_take_setting(slot);
    return true;
}

bool tb_var_revert(tb_var var)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);

    if (slot == NULL || slot->setting == NULL)
    {
        return false;
    }
    tb_gui_take_setting(slot);
    return true;
}

bool tb_var_restore_default(tb_var var)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    char *buffer = NULL;
    char scratch[TB_DECIMAL_SHORTEST_SIZE] = {0};
    tb_gui_var_value value = {0};
    bool restored = false;

    if (slot == NULL || slot->setting == NULL)
    {
        return false;
    }
    const tb_gui_var_value written = tb_gui_setting_default(slot->setting, slot->type, &buffer);
    if (!tb_gui_convert(&written, slot->type, scratch, sizeof(scratch), &value))
    {
        restored = false;
    }
    else if (slot->deferred)
    {
        restored = tb_gui_hold(slot, &value);
    }
    else
    {
        restored = tb_gui_var_assign(var, &value);
    }
    tb_free(buffer);
    return restored;
}

/* Each deferred variable of a GUI, alive, in the order made. */
static size_t tb_gui_deferred_count(const tb_gui *gui)
{
    return gui->values != NULL ? gui->values->count : 0;
}

static tb_var tb_gui_deferred_at(tb_gui *gui, size_t index)
{
    const tb_gui_var_slot *const slot = &gui->values->slots[index];

    if (!slot->alive || !slot->deferred)
    {
        return TB_VAR_NONE;
    }
    return (tb_var){gui, (uint32_t)index, slot->generation};
}

bool tb_gui_apply_settings(tb_gui *gui)
{
    bool applied = true;

    TB_ASSERT(gui != NULL);
    for (size_t index = 0; index < tb_gui_deferred_count(gui); index++)
    {
        const tb_var var = tb_gui_deferred_at(gui, index);
        if (tb_var_alive(var) && !tb_var_apply(var))
        {
            applied = false;
        }
    }
    return applied;
}

void tb_gui_revert_settings(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    for (size_t index = 0; index < tb_gui_deferred_count(gui); index++)
    {
        const tb_var var = tb_gui_deferred_at(gui, index);
        if (tb_var_alive(var))
        {
            (void)tb_var_revert(var);
        }
    }
}

void tb_gui_restore_default_settings(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    for (size_t index = 0; index < tb_gui_deferred_count(gui); index++)
    {
        const tb_var var = tb_gui_deferred_at(gui, index);
        if (tb_var_alive(var))
        {
            (void)tb_var_restore_default(var);
        }
    }
}

bool tb_gui_settings_pending(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    for (size_t index = 0; index < tb_gui_deferred_count(gui); index++)
    {
        if (tb_var_pending(tb_gui_deferred_at(gui, index)))
        {
            return true;
        }
    }
    return false;
}

tb_setting *tb_var_setting(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot != NULL ? slot->setting : NULL;
}

bool tb_var_setting_accepts(tb_var var)
{
    const tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    return slot == NULL || slot->accepted;
}

/* ---------------------------------------------------------------------------
 * Observers.
 * ------------------------------------------------------------------------- */

bool tb_var_trace(tb_var var, tb_var_callback *callback, void *user)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);

    TB_ASSERT(callback != NULL);
    if (slot == NULL)
    {
        return false;
    }
    if (slot->trace_count == slot->trace_capacity)
    {
        slot->trace_capacity =
            slot->trace_capacity == 0 ? TB_GUI_FIRST_TRACES : slot->trace_capacity * 2;
        slot->traces = tb_realloc_array(slot->traces, slot->trace_capacity, sizeof(*slot->traces));
    }
    slot->traces[slot->trace_count] = (tb_gui_trace){callback, user};
    slot->trace_count++;
    return true;
}

static void tb_gui_compact_traces(tb_gui_var_slot *slot)
{
    size_t kept = 0;

    for (size_t index = 0; index < slot->trace_count; index++)
    {
        if (slot->traces[index].callback != NULL)
        {
            slot->traces[kept] = slot->traces[index];
            kept++;
        }
    }
    slot->trace_count = kept;
    slot->traces_cleared = false;
}

size_t tb_var_untrace(tb_var var, tb_var_callback *callback, void *user)
{
    tb_gui_var_slot *const slot = tb_gui_var_slot_of(var);
    size_t removed = 0;

    if (slot == NULL)
    {
        return 0;
    }
    for (size_t index = 0; index < slot->trace_count; index++)
    {
        tb_gui_trace *const trace = &slot->traces[index];
        const bool matches =
            callback == NULL || (trace->callback == callback && trace->user == user);
        if (trace->callback != NULL && matches)
        {
            trace->callback = NULL;
            removed++;
        }
    }
    if (removed > 0)
    {
        slot->traces_cleared = true;
        if (var.gui->values->notifying == 0)
        {
            tb_gui_compact_traces(slot);
        }
    }
    return removed;
}

/* Run a variable's observers: whether any variable was to be seen. */
static void tb_gui_notify(tb_gui *gui, uint32_t index)
{
    tb_gui_var_slot *slot = &gui->values->slots[index];
    const tb_var var = {gui, index, slot->generation};
    const size_t count = slot->trace_count;

    slot->notified = slot->version;
    for (size_t at = 0; at < count; at++)
    {
        /* Observers may make variables, which moves the slots. */
        slot = tb_gui_var_slot_of(var);
        if (slot == NULL || at >= slot->trace_count)
        {
            return;
        }
        const tb_gui_trace trace = slot->traces[at];
        if (trace.callback != NULL)
        {
            trace.callback(var, trace.user);
        }
    }
}

void tb_gui_vars_update(tb_gui *gui)
{
    tb_gui_values *const values = gui->values;
    bool pending = true;

    if (values == NULL)
    {
        return;
    }
    for (size_t index = 0; index < values->count; index++)
    {
        tb_gui_var_slot *const slot = &values->slots[index];
        if (!slot->alive || slot->setting == NULL ||
            tb_setting_version(slot->setting) == slot->setting_version)
        {
            continue;
        }
        if (slot->held)
        {
            /* A deferred variable keeps what it holds, which may now be
             * the setting's own. */
            slot->setting_version = tb_setting_version(slot->setting);
            slot->held = !tb_gui_matches_setting(slot);
            continue;
        }
        tb_gui_take_setting(slot);
    }
    values->notifying++;
    for (int pass = 0; pass < TB_GUI_MOST_TRACE_PASSES && pending; pass++)
    {
        pending = false;
        for (size_t index = 0; index < values->count; index++)
        {
            const tb_gui_var_slot *const slot = &values->slots[index];
            if (slot->alive && slot->version != slot->notified)
            {
                tb_gui_notify(gui, (uint32_t)index);
                pending = true;
            }
        }
    }
    values->notifying--;
    for (size_t index = 0; index < values->count; index++)
    {
        if (values->slots[index].traces_cleared)
        {
            tb_gui_compact_traces(&values->slots[index]);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The clipboard.
 * ------------------------------------------------------------------------- */

tb_text tb_gui_clipboard(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    if (gui->values == NULL)
    {
        return tb_text_make("", 0);
    }
    return tb_gui_string_text(&gui->values->clipboard);
}

void tb_gui_set_clipboard(tb_gui *gui, tb_text text)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(text.bytes != NULL || text.length == 0);
    tb_gui_string_set(&tb_gui_values_of(gui)->clipboard, text);
}
