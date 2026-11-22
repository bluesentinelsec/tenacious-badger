/* The registry: families, settings, their layers, and the changes that move
 * their versions. One mutex guards everything that changes; what never
 * changes after a setting is added, its name and its range, is read without
 * it. */
#include "settings_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <float.h>
#include <string.h>

/* The layers that hold a player's choices. */
#define TB_SETTING_PLAYER_LAYERS                                                                   \
    ((1u << TB_SETTING_LAYER_PLAYER_FILE) | (1u << TB_SETTING_LAYER_RUNTIME))

/* ---------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

static bool tb_settings_name_character(char character)
{
    const bool lowercase = character >= 'a' && character <= 'z';
    const bool digit = character >= '0' && character <= '9';
    return lowercase || digit || character == '_';
}

bool tb_settings_valid_name(const char *name, size_t length, size_t words, size_t most_words)
{
    size_t found = 1;
    size_t word_length = 0;

    if (length == 0 || length > TB_SETTING_NAME_LIMIT)
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        if (name[index] == '.')
        {
            if (word_length == 0)
            {
                return false;
            }
            found++;
            word_length = 0;
        }
        else if (tb_settings_name_character(name[index]))
        {
            word_length++;
        }
        else
        {
            return false;
        }
    }
    if (word_length == 0 || found < words)
    {
        return false;
    }
    return most_words == 0 || found <= most_words;
}

/* The family of a setting's name: the index of the family named by its first
 * word, or the number of families when none is. */
static size_t tb_settings_family_of(const tb_settings *settings, const char *name)
{
    const size_t dot = tb_str_find_char(name, 0, TB_STR_END, '.');
    const size_t word = dot == TB_STR_NOT_FOUND ? tb_str_length(name) : dot;
    const size_t count = tb_array_length(settings->families);

    for (size_t index = 0; index < count; index++)
    {
        const tb_settings_family_record *const family =
            TB_ARRAY_AT(settings->families, tb_settings_family_record, index);
        if (tb_str_length(family->name) == word && memcmp(family->name, name, word) == 0)
        {
            return index;
        }
    }
    return count;
}

static const char *tb_settings_copy(tb_settings *settings, const char *string)
{
    const size_t length = tb_str_length(string);
    char *const copy = tb_arena_alloc(settings->arena, length + 1, 1);

    memcpy(copy, string, length + 1);
    return copy;
}

static const char *tb_settings_copy_optional(tb_settings *settings, const char *string)
{
    if (string == NULL)
    {
        return NULL;
    }
    return tb_settings_copy(settings, string);
}

/* ---------------------------------------------------------------------------
 * Values and layers.
 * ------------------------------------------------------------------------- */

static bool tb_setting_layer_present(const tb_setting *setting, tb_setting_layer layer)
{
    return (setting->present & (1u << layer)) != 0;
}

tb_setting_layer tb_setting_source_locked(const tb_setting *setting)
{
    tb_setting_layer layer = TB_SETTING_LAYER_RUNTIME;

    while (layer > TB_SETTING_LAYER_BUILT_IN && !tb_setting_layer_present(setting, layer))
    {
        layer--;
    }
    return layer;
}

static const tb_setting_value *tb_setting_current(const tb_setting *setting)
{
    return &setting->values[tb_setting_source_locked(setting)];
}

static bool tb_setting_values_equal(const tb_setting *setting, const tb_setting_value *left,
                                    const tb_setting_value *right)
{
    switch (setting->type)
    {
    case TB_SETTING_BOOLEAN:
        return left->boolean == right->boolean;
    case TB_SETTING_INTEGER:
        return left->integer == right->integer;
    case TB_SETTING_NUMBER:
        /* Bitwise, so that 0.0 and -0.0, which a file writes differently,
         * are different values. */
        return memcmp(&left->number, &right->number, sizeof(double)) == 0;
    case TB_SETTING_CHOICE:
        return left->choice == right->choice;
    case TB_SETTING_TEXT:
        return tb_str_equal(left->text, right->text);
    }
    return false;
}

static void tb_setting_release(tb_setting *setting, tb_setting_layer layer)
{
    if (setting->type == TB_SETTING_TEXT && tb_setting_layer_present(setting, layer))
    {
        tb_free(setting->values[layer].text);
        setting->values[layer].text = NULL;
    }
}

/* Bump the versions when the value changed. `before` is the value before the
 * change; a text value is compared before the old text is released. */
static void tb_setting_note_change(tb_setting *setting, const tb_setting_value *before)
{
    if (!tb_setting_values_equal(setting, before, tb_setting_current(setting)))
    {
        setting->version++;
        setting->owner->version++;
    }
}

void tb_setting_store_locked(tb_setting *setting, tb_setting_layer layer, tb_setting_value value)
{
    const tb_setting_layer source = tb_setting_source_locked(setting);
    tb_setting_value before = setting->values[source];
    tb_setting_value replaced = setting->values[layer];
    const bool had = tb_setting_layer_present(setting, layer);

    setting->values[layer] = value;
    setting->present |= 1u << layer;
    tb_setting_note_change(setting, &before);
    if (had && setting->type == TB_SETTING_TEXT)
    {
        tb_free(replaced.text);
    }
}

void tb_setting_clear_locked(tb_setting *setting, tb_setting_layer layer)
{
    const tb_setting_layer source = tb_setting_source_locked(setting);
    const tb_setting_value before = setting->values[source];
    tb_setting_value removed = setting->values[layer];

    if (layer == TB_SETTING_LAYER_BUILT_IN || !tb_setting_layer_present(setting, layer))
    {
        return;
    }
    setting->present &= ~(1u << layer);
    tb_setting_note_change(setting, &before);
    if (setting->type == TB_SETTING_TEXT)
    {
        tb_free(removed.text);
    }
    setting->values[layer] = (tb_setting_value){0};
}

static void tb_settings_lock(const tb_settings *settings)
{
    tb_mutex_lock(settings->mutex);
}

static void tb_settings_unlock(const tb_settings *settings)
{
    tb_mutex_unlock(settings->mutex);
}

/* ---------------------------------------------------------------------------
 * The registry.
 * ------------------------------------------------------------------------- */

tb_settings *tb_settings_create(void)
{
    tb_settings *const settings = tb_calloc(1, sizeof(tb_settings));

    settings->mutex = tb_calloc(1, sizeof(tb_mutex));
    settings->arena = tb_arena_create(0);
    settings->families = TB_ARRAY_CREATE(NULL, tb_settings_family_record);
    settings->settings = TB_ARRAY_CREATE(NULL, tb_setting *);
    settings->by_name = tb_table_create(NULL, 0, tb_table_string_hash, tb_table_string_equal);
    settings->kept = TB_ARRAY_CREATE(NULL, tb_settings_kept);
    return settings;
}

static void tb_settings_free_kept(tb_settings_kept *kept)
{
    tb_free(kept->name);
    tb_free(kept->value);
}

void tb_settings_destroy(tb_settings *settings)
{
    if (settings == NULL)
    {
        return;
    }
    for (size_t index = 0; index < tb_array_length(settings->settings); index++)
    {
        tb_setting *const setting = *TB_ARRAY_AT(settings->settings, tb_setting *, index);
        for (unsigned layer = 0; layer < TB_SETTING_LAYER_COUNT; layer++)
        {
            tb_setting_release(setting, (tb_setting_layer)layer);
        }
    }
    for (size_t index = 0; index < tb_array_length(settings->kept); index++)
    {
        tb_settings_free_kept(TB_ARRAY_AT(settings->kept, tb_settings_kept, index));
    }
    tb_array_destroy(settings->kept);
    tb_table_destroy(settings->by_name);
    tb_array_destroy(settings->settings);
    tb_array_destroy(settings->families);
    tb_arena_destroy(settings->arena);
    tb_free(settings->mutex);
    tb_free(settings);
}

void tb_settings_add_family(tb_settings *settings, const tb_settings_family *family)
{
    tb_settings_family_record record = {0};

    TB_ASSERT(settings != NULL);
    TB_ASSERT(family != NULL && family->name != NULL);
    TB_ASSERT_MSG(tb_settings_valid_name(family->name, tb_str_length(family->name), 1, 1),
                  "a family's name is one word of lowercase letters, digits and underscores");
    tb_settings_lock(settings);
    TB_ASSERT_MSG(tb_settings_family_of(settings, family->name) ==
                      tb_array_length(settings->families),
                  "no family has the name already");
    record.name = tb_settings_copy(settings, family->name);
    record.title = tb_settings_copy(settings, family->title != NULL ? family->title : family->name);
    record.description = tb_settings_copy_optional(settings, family->description);
    record.for_players = family->for_players;
    (void)tb_array_push(settings->families, &record);
    tb_settings_unlock(settings);
}

size_t tb_settings_family_count(const tb_settings *settings)
{
    size_t count = 0;

    TB_ASSERT(settings != NULL);
    tb_settings_lock(settings);
    count = tb_array_length(settings->families);
    tb_settings_unlock(settings);
    return count;
}

tb_settings_family tb_settings_family_at(const tb_settings *settings, size_t index)
{
    tb_settings_family family = {0};

    TB_ASSERT(settings != NULL);
    tb_settings_lock(settings);
    TB_ASSERT_MSG(index < tb_array_length(settings->families), "index < family count");
    {
        const tb_settings_family_record *const record =
            TB_ARRAY_AT(settings->families, tb_settings_family_record, index);
        family = (tb_settings_family){.name = record->name,
                                      .title = record->title,
                                      .description = record->description,
                                      .for_players = record->for_players};
    }
    tb_settings_unlock(settings);
    return family;
}

/* ---------------------------------------------------------------------------
 * Adding a setting.
 * ------------------------------------------------------------------------- */

static void tb_settings_check_definition(const tb_setting_definition *definition)
{
    switch (definition->type)
    {
    case TB_SETTING_BOOLEAN:
        break;
    case TB_SETTING_INTEGER:
        TB_ASSERT_MSG(definition->integer.minimum <= definition->integer.maximum,
                      "an integer setting's minimum is at most its maximum");
        TB_ASSERT_MSG(definition->integer.default_value >= definition->integer.minimum &&
                          definition->integer.default_value <= definition->integer.maximum,
                      "an integer setting's default is within its range");
        break;
    case TB_SETTING_NUMBER:
        TB_ASSERT_MSG(definition->number.minimum >= -DBL_MAX &&
                          definition->number.maximum <= DBL_MAX &&
                          definition->number.minimum <= definition->number.maximum,
                      "a number setting's range is finite, its minimum at most its maximum");
        TB_ASSERT_MSG(definition->number.default_value >= definition->number.minimum &&
                          definition->number.default_value <= definition->number.maximum,
                      "a number setting's default is within its range");
        break;
    case TB_SETTING_CHOICE:
        TB_ASSERT_MSG(definition->choice.names != NULL && definition->choice.count > 0 &&
                          definition->choice.count <= TB_SETTING_CHOICE_LIMIT,
                      "a choice setting has 1 to TB_SETTING_CHOICE_LIMIT choices");
        TB_ASSERT_MSG(definition->choice.default_value < definition->choice.count,
                      "a choice setting's default is one of its choices");
        for (size_t index = 0; index < definition->choice.count; index++)
        {
            const char *const name = definition->choice.names[index];
            TB_ASSERT_MSG(name != NULL && tb_settings_valid_name(name, tb_str_length(name), 1, 1),
                          "a choice's name is one word of lowercase letters, digits and "
                          "underscores");
            for (size_t other = 0; other < index; other++)
            {
                TB_ASSERT_MSG(!tb_str_equal(definition->choice.names[other], name),
                              "a choice setting's choices have different names");
            }
        }
        break;
    case TB_SETTING_TEXT: {
        const char *const text =
            definition->text.default_value != NULL ? definition->text.default_value : "";
        const size_t limit =
            definition->text.limit != 0 ? definition->text.limit : TB_SETTING_DEFAULT_TEXT_LIMIT;
        TB_ASSERT_MSG(tb_settings_valid_utf8(text, tb_str_length(text)) &&
                          tb_str_length(text) <= limit,
                      "a text setting's default is valid UTF-8 within its limit");
        (void)text;
        (void)limit;
        break;
    }
    default:
        TB_ASSERT_MSG(false, "a setting's type is a tb_setting_type");
        break;
    }
}

static tb_setting_value tb_settings_built_in(const tb_setting_definition *definition)
{
    tb_setting_value value = {0};

    switch (definition->type)
    {
    case TB_SETTING_BOOLEAN:
        value.boolean = definition->boolean.default_value;
        break;
    case TB_SETTING_INTEGER:
        value.integer = definition->integer.default_value;
        break;
    case TB_SETTING_NUMBER:
        value.number = definition->number.default_value;
        break;
    case TB_SETTING_CHOICE:
        value.choice = definition->choice.default_value;
        break;
    case TB_SETTING_TEXT:
        value.text = tb_str_dup(
            NULL, definition->text.default_value != NULL ? definition->text.default_value : "");
        break;
    }
    return value;
}

/* Apply what files kept for the setting's name before it was added, and
 * forget it: what fits now sets its layer, and what does not is dropped. */
static void tb_settings_apply_kept(tb_settings *settings, tb_setting *setting)
{
    size_t index = 0;

    while (index < tb_array_length(settings->kept))
    {
        tb_settings_kept kept = *TB_ARRAY_AT(settings->kept, tb_settings_kept, index);
        tb_setting_value value = {0};

        if (!tb_str_equal(kept.name, setting->name))
        {
            index++;
            continue;
        }
        if (tb_setting_read_value(setting, kept.value, tb_str_length(kept.value), &value) ==
            TB_SETTING_OK)
        {
            tb_setting_store_locked(setting, kept.layer, value);
        }
        tb_settings_free_kept(&kept);
        tb_array_remove(settings->kept, index, NULL);
    }
}

tb_setting *tb_settings_add(tb_settings *settings, const tb_setting_definition *definition)
{
    tb_setting *setting = NULL;
    size_t family = 0;

    TB_ASSERT(settings != NULL);
    TB_ASSERT(definition != NULL && definition->name != NULL);
    TB_ASSERT_MSG(tb_settings_valid_name(definition->name, tb_str_length(definition->name), 2, 0),
                  "a setting's name is its family's, a dot, and more words of lowercase letters, "
                  "digits and underscores");
    tb_settings_check_definition(definition);

    tb_settings_lock(settings);
    family = tb_settings_family_of(settings, definition->name);
    TB_ASSERT_MSG(family < tb_array_length(settings->families),
                  "a setting's family is added before it");
    TB_ASSERT_MSG(tb_table_get(settings->by_name, definition->name) == NULL,
                  "no setting has the name already");

    setting = TB_ARENA_NEW(settings->arena, tb_setting);
    *setting =
        (tb_setting){.owner = settings,
                     .name = tb_settings_copy(settings, definition->name),
                     .description = tb_settings_copy_optional(settings, definition->description),
                     .type = definition->type,
                     .family = family,
                     .present = 1u << TB_SETTING_LAYER_BUILT_IN};
    switch (definition->type)
    {
    case TB_SETTING_INTEGER:
        setting->integer_minimum = definition->integer.minimum;
        setting->integer_maximum = definition->integer.maximum;
        break;
    case TB_SETTING_NUMBER:
        setting->number_minimum = definition->number.minimum;
        setting->number_maximum = definition->number.maximum;
        break;
    case TB_SETTING_CHOICE:
        setting->choice_count = definition->choice.count;
        setting->choices = TB_ARENA_NEW_ARRAY(settings->arena, const char *, setting->choice_count);
        for (size_t index = 0; index < setting->choice_count; index++)
        {
            setting->choices[index] = tb_settings_copy(settings, definition->choice.names[index]);
        }
        break;
    case TB_SETTING_TEXT:
        setting->text_limit =
            definition->text.limit != 0 ? definition->text.limit : TB_SETTING_DEFAULT_TEXT_LIMIT;
        break;
    case TB_SETTING_BOOLEAN:
        break;
    }
    setting->values[TB_SETTING_LAYER_BUILT_IN] = tb_settings_built_in(definition);
    (void)tb_table_put(settings->by_name, setting->name, setting);
    (void)tb_array_push(settings->settings, &setting);
    tb_settings_apply_kept(settings, setting);
    tb_settings_unlock(settings);
    return setting;
}

size_t tb_settings_count(const tb_settings *settings)
{
    size_t count = 0;

    TB_ASSERT(settings != NULL);
    tb_settings_lock(settings);
    count = tb_array_length(settings->settings);
    tb_settings_unlock(settings);
    return count;
}

tb_setting *tb_settings_at(const tb_settings *settings, size_t index)
{
    tb_setting *setting = NULL;

    TB_ASSERT(settings != NULL);
    tb_settings_lock(settings);
    TB_ASSERT_MSG(index < tb_array_length(settings->settings), "index < setting count");
    setting = *TB_ARRAY_AT(settings->settings, tb_setting *, index);
    tb_settings_unlock(settings);
    return setting;
}

tb_setting *tb_settings_find(const tb_settings *settings, const char *name)
{
    tb_setting *setting = NULL;

    TB_ASSERT(settings != NULL);
    TB_ASSERT(name != NULL);
    tb_settings_lock(settings);
    setting = tb_table_get(settings->by_name, name);
    tb_settings_unlock(settings);
    return setting;
}

uint64_t tb_settings_version(const tb_settings *settings)
{
    uint64_t version = 0;

    TB_ASSERT(settings != NULL);
    tb_settings_lock(settings);
    version = settings->version;
    tb_settings_unlock(settings);
    return version;
}

/* ---------------------------------------------------------------------------
 * What a setting is. None of it changes after the setting is added.
 * ------------------------------------------------------------------------- */

static void tb_setting_require_type(const tb_setting *setting, tb_setting_type type)
{
    TB_ASSERT(setting != NULL);
    TB_ASSERT_MSG(setting->type == type, "the setting is of the type it is used as");
    (void)setting;
    (void)type;
}

const char *tb_setting_name(const tb_setting *setting)
{
    TB_ASSERT(setting != NULL);
    return setting->name;
}

const char *tb_setting_description(const tb_setting *setting)
{
    TB_ASSERT(setting != NULL);
    return setting->description;
}

tb_setting_type tb_setting_type_of(const tb_setting *setting)
{
    TB_ASSERT(setting != NULL);
    return setting->type;
}

size_t tb_setting_family(const tb_setting *setting)
{
    TB_ASSERT(setting != NULL);
    return setting->family;
}

bool tb_setting_for_players(const tb_setting *setting)
{
    bool for_players = false;

    TB_ASSERT(setting != NULL);
    tb_settings_lock(setting->owner);
    for_players = TB_ARRAY_AT(setting->owner->families, tb_settings_family_record, setting->family)
                      ->for_players;
    tb_settings_unlock(setting->owner);
    return for_players;
}

void tb_setting_integer_range(const tb_setting *setting, int64_t *minimum, int64_t *maximum)
{
    tb_setting_require_type(setting, TB_SETTING_INTEGER);
    TB_ASSERT(minimum != NULL && maximum != NULL);
    *minimum = setting->integer_minimum;
    *maximum = setting->integer_maximum;
}

void tb_setting_number_range(const tb_setting *setting, double *minimum, double *maximum)
{
    tb_setting_require_type(setting, TB_SETTING_NUMBER);
    TB_ASSERT(minimum != NULL && maximum != NULL);
    *minimum = setting->number_minimum;
    *maximum = setting->number_maximum;
}

size_t tb_setting_choice_count(const tb_setting *setting)
{
    tb_setting_require_type(setting, TB_SETTING_CHOICE);
    return setting->choice_count;
}

const char *tb_setting_choice_name(const tb_setting *setting, size_t index)
{
    tb_setting_require_type(setting, TB_SETTING_CHOICE);
    TB_ASSERT_MSG(index < setting->choice_count, "index < choice count");
    return setting->choices[index];
}

size_t tb_setting_find_choice(const tb_setting *setting, const char *name)
{
    tb_setting_require_type(setting, TB_SETTING_CHOICE);
    TB_ASSERT(name != NULL);
    return tb_setting_choice_index(setting, name, tb_str_length(name));
}

size_t tb_setting_text_limit(const tb_setting *setting)
{
    tb_setting_require_type(setting, TB_SETTING_TEXT);
    return setting->text_limit;
}

/* ---------------------------------------------------------------------------
 * Reading a value.
 * ------------------------------------------------------------------------- */

bool tb_setting_boolean(const tb_setting *setting)
{
    bool value = false;

    tb_setting_require_type(setting, TB_SETTING_BOOLEAN);
    tb_settings_lock(setting->owner);
    value = tb_setting_current(setting)->boolean;
    tb_settings_unlock(setting->owner);
    return value;
}

int64_t tb_setting_integer(const tb_setting *setting)
{
    int64_t value = 0;

    tb_setting_require_type(setting, TB_SETTING_INTEGER);
    tb_settings_lock(setting->owner);
    value = tb_setting_current(setting)->integer;
    tb_settings_unlock(setting->owner);
    return value;
}

double tb_setting_number(const tb_setting *setting)
{
    double value = 0.0;

    tb_setting_require_type(setting, TB_SETTING_NUMBER);
    tb_settings_lock(setting->owner);
    value = tb_setting_current(setting)->number;
    tb_settings_unlock(setting->owner);
    return value;
}

size_t tb_setting_choice(const tb_setting *setting)
{
    size_t value = 0;

    tb_setting_require_type(setting, TB_SETTING_CHOICE);
    tb_settings_lock(setting->owner);
    value = tb_setting_current(setting)->choice;
    tb_settings_unlock(setting->owner);
    return value;
}

size_t tb_setting_text(const tb_setting *setting, char *buffer, size_t capacity)
{
    size_t length = 0;

    tb_setting_require_type(setting, TB_SETTING_TEXT);
    TB_ASSERT(buffer != NULL || capacity == 0);
    tb_settings_lock(setting->owner);
    {
        const char *const text = tb_setting_current(setting)->text;
        length = tb_str_length(text);
        if (length < capacity)
        {
            memcpy(buffer, text, length + 1);
        }
    }
    tb_settings_unlock(setting->owner);
    return length;
}

size_t tb_setting_format(const tb_setting *setting, tb_setting_layer layer, char *buffer,
                         size_t capacity)
{
    size_t length = 0;

    TB_ASSERT(setting != NULL);
    TB_ASSERT(layer <= TB_SETTING_LAYER_COUNT);
    TB_ASSERT(buffer != NULL || capacity == 0);
    tb_settings_lock(setting->owner);
    if (layer == TB_SETTING_LAYER_COUNT)
    {
        length = tb_setting_write_value(setting, tb_setting_current(setting), buffer, capacity);
    }
    else if (tb_setting_layer_present(setting, layer))
    {
        length = tb_setting_write_value(setting, &setting->values[layer], buffer, capacity);
    }
    else if (capacity > 0)
    {
        buffer[0] = '\0';
    }
    tb_settings_unlock(setting->owner);
    return length;
}

tb_setting_layer tb_setting_source(const tb_setting *setting)
{
    tb_setting_layer layer = TB_SETTING_LAYER_BUILT_IN;

    TB_ASSERT(setting != NULL);
    tb_settings_lock(setting->owner);
    layer = tb_setting_source_locked(setting);
    tb_settings_unlock(setting->owner);
    return layer;
}

bool tb_setting_has_layer(const tb_setting *setting, tb_setting_layer layer)
{
    bool present = false;

    TB_ASSERT(setting != NULL);
    TB_ASSERT(layer < TB_SETTING_LAYER_COUNT);
    tb_settings_lock(setting->owner);
    present = tb_setting_layer_present(setting, layer);
    tb_settings_unlock(setting->owner);
    return present;
}

uint64_t tb_setting_version(const tb_setting *setting)
{
    uint64_t version = 0;

    TB_ASSERT(setting != NULL);
    tb_settings_lock(setting->owner);
    version = setting->version;
    tb_settings_unlock(setting->owner);
    return version;
}

/* ---------------------------------------------------------------------------
 * Changing a value.
 * ------------------------------------------------------------------------- */

static void tb_setting_store(tb_setting *setting, tb_setting_layer layer, tb_setting_value value)
{
    tb_settings_lock(setting->owner);
    tb_setting_store_locked(setting, layer, value);
    tb_settings_unlock(setting->owner);
}

tb_setting_result tb_setting_set_boolean(tb_setting *setting, bool value)
{
    tb_setting_require_type(setting, TB_SETTING_BOOLEAN);
    tb_setting_store(setting, TB_SETTING_LAYER_RUNTIME, (tb_setting_value){.boolean = value});
    return TB_SETTING_OK;
}

tb_setting_result tb_setting_set_integer(tb_setting *setting, int64_t value)
{
    tb_setting_require_type(setting, TB_SETTING_INTEGER);
    if (value < setting->integer_minimum || value > setting->integer_maximum)
    {
        return TB_SETTING_OUT_OF_RANGE;
    }
    tb_setting_store(setting, TB_SETTING_LAYER_RUNTIME, (tb_setting_value){.integer = value});
    return TB_SETTING_OK;
}

tb_setting_result tb_setting_set_number(tb_setting *setting, double value)
{
    tb_setting_require_type(setting, TB_SETTING_NUMBER);
    /* Written so that NaN, which compares false with everything, fails. */
    if (!(value >= setting->number_minimum && value <= setting->number_maximum))
    {
        return TB_SETTING_OUT_OF_RANGE;
    }
    tb_setting_store(setting, TB_SETTING_LAYER_RUNTIME, (tb_setting_value){.number = value});
    return TB_SETTING_OK;
}

tb_setting_result tb_setting_set_choice(tb_setting *setting, size_t index)
{
    tb_setting_require_type(setting, TB_SETTING_CHOICE);
    if (index >= setting->choice_count)
    {
        return TB_SETTING_OUT_OF_RANGE;
    }
    tb_setting_store(setting, TB_SETTING_LAYER_RUNTIME, (tb_setting_value){.choice = index});
    return TB_SETTING_OK;
}

tb_setting_result tb_setting_set_text(tb_setting *setting, const char *text)
{
    tb_setting_value value = {0};
    tb_setting_result result = TB_SETTING_OK;

    tb_setting_require_type(setting, TB_SETTING_TEXT);
    TB_ASSERT(text != NULL);
    result = tb_setting_read_value(setting, text, tb_str_length(text), &value);
    if (result == TB_SETTING_OK)
    {
        tb_setting_store(setting, TB_SETTING_LAYER_RUNTIME, value);
    }
    return result;
}

tb_setting_result tb_setting_parse(tb_setting *setting, tb_setting_layer layer, const char *text,
                                   size_t length)
{
    tb_setting_value value = {0};
    tb_setting_result result = TB_SETTING_OK;
    const char *problem = NULL;
    size_t unquoted_length = 0;
    char *unquoted = NULL;

    TB_ASSERT(setting != NULL);
    TB_ASSERT_MSG(layer > TB_SETTING_LAYER_BUILT_IN && layer < TB_SETTING_LAYER_COUNT,
                  "the layer is one a value may be set at");
    TB_ASSERT(text != NULL || length == 0);
    unquoted = tb_settings_unquote(text != NULL ? text : "", length, &unquoted_length, &problem);
    if (unquoted == NULL)
    {
        return TB_SETTING_INVALID;
    }
    result = tb_setting_read_value(setting, unquoted, unquoted_length, &value);
    tb_free(unquoted);
    if (result == TB_SETTING_OK)
    {
        tb_setting_store(setting, layer, value);
    }
    return result;
}

tb_setting_result tb_settings_set(tb_settings *settings, tb_setting_layer layer, const char *name,
                                  const char *value)
{
    tb_setting *setting = NULL;

    TB_ASSERT(settings != NULL);
    TB_ASSERT(name != NULL && value != NULL);
    setting = tb_settings_find(settings, name);
    if (setting == NULL)
    {
        return TB_SETTING_UNKNOWN;
    }
    return tb_setting_parse(setting, layer, value, tb_str_length(value));
}

void tb_setting_clear(tb_setting *setting, tb_setting_layer layer)
{
    TB_ASSERT(setting != NULL);
    TB_ASSERT_MSG(layer > TB_SETTING_LAYER_BUILT_IN && layer < TB_SETTING_LAYER_COUNT,
                  "the layer is one a value may be set at");
    tb_settings_lock(setting->owner);
    tb_setting_clear_locked(setting, layer);
    tb_settings_unlock(setting->owner);
}

static void tb_setting_restore_locked(tb_setting *setting)
{
    tb_setting_clear_locked(setting, TB_SETTING_LAYER_RUNTIME);
    tb_setting_clear_locked(setting, TB_SETTING_LAYER_PLAYER_FILE);
}

void tb_setting_restore_default(tb_setting *setting)
{
    TB_ASSERT(setting != NULL);
    tb_settings_lock(setting->owner);
    tb_setting_restore_locked(setting);
    tb_settings_unlock(setting->owner);
}

void tb_settings_restore_family(tb_settings *settings, const char *family)
{
    size_t index = 0;

    TB_ASSERT(settings != NULL);
    TB_ASSERT(family != NULL);
    tb_settings_lock(settings);
    for (index = 0; index < tb_array_length(settings->families); index++)
    {
        if (tb_str_equal(TB_ARRAY_AT(settings->families, tb_settings_family_record, index)->name,
                         family))
        {
            break;
        }
    }
    TB_ASSERT_MSG(index < tb_array_length(settings->families), "a family has the name");
    for (size_t setting = 0; setting < tb_array_length(settings->settings); setting++)
    {
        tb_setting *const entry = *TB_ARRAY_AT(settings->settings, tb_setting *, setting);
        if (entry->family == index)
        {
            tb_setting_restore_locked(entry);
        }
    }
    tb_settings_unlock(settings);
}

void tb_settings_restore_all(tb_settings *settings)
{
    size_t index = 0;

    TB_ASSERT(settings != NULL);
    tb_settings_lock(settings);
    for (size_t setting = 0; setting < tb_array_length(settings->settings); setting++)
    {
        tb_setting_restore_locked(*TB_ARRAY_AT(settings->settings, tb_setting *, setting));
    }
    while (index < tb_array_length(settings->kept))
    {
        tb_settings_kept kept = *TB_ARRAY_AT(settings->kept, tb_settings_kept, index);
        if ((TB_SETTING_PLAYER_LAYERS & (1u << kept.layer)) == 0)
        {
            index++;
            continue;
        }
        tb_settings_free_kept(&kept);
        tb_array_remove(settings->kept, index, NULL);
    }
    tb_settings_unlock(settings);
}

const char *tb_setting_result_name(tb_setting_result result)
{
    switch (result)
    {
    case TB_SETTING_OK:
        return "ok";
    case TB_SETTING_UNKNOWN:
        return "unknown setting";
    case TB_SETTING_INVALID:
        return "invalid value";
    case TB_SETTING_OUT_OF_RANGE:
        return "out of range";
    }
    return "unknown result";
}

const char *tb_setting_layer_name(tb_setting_layer layer)
{
    switch (layer)
    {
    case TB_SETTING_LAYER_BUILT_IN:
        return "built-in";
    case TB_SETTING_LAYER_DEFAULT_FILE:
        return "default file";
    case TB_SETTING_LAYER_PLAYER_FILE:
        return "player file";
    case TB_SETTING_LAYER_COMMAND_LINE:
        return "command line";
    case TB_SETTING_LAYER_RUNTIME:
        return "runtime";
    case TB_SETTING_LAYER_COUNT:
        break;
    }
    return "unknown layer";
}
