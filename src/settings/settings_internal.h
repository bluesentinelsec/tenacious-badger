/* What the parts of the settings module share: the registry's layout, and
 * reading and writing values as text. */
#ifndef TB_SETTINGS_INTERNAL_H
#define TB_SETTINGS_INTERNAL_H

#include "badger/arena.h"
#include "badger/array.h"
#include "badger/settings.h"
#include "badger/table.h"
#include "badger/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One layer's value of a setting. Text is the heap's, owned by the setting. */
typedef union tb_setting_value {
    bool boolean;
    int64_t integer;
    double number;
    size_t choice;
    char *text;
} tb_setting_value;

struct tb_setting
{
    tb_settings *owner;
    const char *name;        /* The registry's arena. */
    const char *description; /* The registry's arena; NULL for none. */
    tb_setting_type type;
    size_t family;
    int64_t integer_minimum;
    int64_t integer_maximum;
    double number_minimum;
    double number_maximum;
    const char **choices; /* The registry's arena. */
    size_t choice_count;
    size_t text_limit;
    unsigned present; /* Bit n set when layer n has a value. */
    tb_setting_value values[TB_SETTING_LAYER_COUNT];
    uint64_t version;
};

/* A family, with its strings in the registry's arena. */
typedef struct tb_settings_family_record
{
    const char *name;
    const char *title;
    const char *description;
    bool for_players;
} tb_settings_family_record;

/* A loaded value for a name that no setting has: kept to apply if the
 * setting is added, and to write back to the player's file. Both strings are
 * the heap's. The value is unquoted. */
typedef struct tb_settings_kept
{
    tb_setting_layer layer;
    char *name;
    char *value;
} tb_settings_kept;

struct tb_settings
{
    tb_mutex *mutex;    /* Its own allocation, so that readers of a const registry lock it. */
    tb_arena *arena;    /* Names, descriptions, choices, and the settings. */
    tb_array *families; /* tb_settings_family_record, in the order added. */
    tb_array *settings; /* tb_setting *, in the order added. */
    tb_table *by_name;  /* Name to tb_setting *. */
    tb_array *kept;     /* tb_settings_kept. */
    uint64_t version;
};

/* Whether a name is words of [a-z0-9_] joined by dots: at least `words`,
 * at most `most_words` (0 for any number), within TB_SETTING_NAME_LIMIT. */
bool tb_settings_valid_name(const char *name, size_t length, size_t words, size_t most_words);

/* Whether bytes are valid UTF-8. */
bool tb_settings_valid_utf8(const char *text, size_t length);

/* Read a value of a setting's type from unquoted text, checking its range.
 * Text values are new heap strings, the caller's. */
tb_setting_result tb_setting_read_value(const tb_setting *setting, const char *text, size_t length,
                                        tb_setting_value *value);

/* The index of the choice that text names, ignoring the case of ASCII
 * letters; the choice count when none does. */
size_t tb_setting_choice_index(const tb_setting *setting, const char *text, size_t length);

/* Describe why text is not a value of a setting, for a problem's message. */
void tb_setting_describe_failure(const tb_setting *setting, tb_setting_result result,
                                 const char *text, size_t length, char *buffer, size_t capacity);

/* Write a value as a file writes it; returns its length, and writes it with a
 * NUL only when it is less than the capacity, as tb_setting_format() does. */
size_t tb_setting_write_value(const tb_setting *setting, const tb_setting_value *value,
                              char *buffer, size_t capacity);

/* Remove the quotes and escapes from a value, when it is quoted. The result
 * is a new heap string, the caller's, of `*unquoted_length` bytes; NULL when
 * the quoting is wrong, with what is wrong in `problem`. */
char *tb_settings_unquote(const char *text, size_t length, size_t *unquoted_length,
                          const char **problem);

/* Set a layer's value with the registry locked, taking ownership of text,
 * and update the versions when the setting's value changes. */
void tb_setting_store_locked(tb_setting *setting, tb_setting_layer layer, tb_setting_value value);

/* Remove a layer's value with the registry locked, updating the versions. */
void tb_setting_clear_locked(tb_setting *setting, tb_setting_layer layer);

/* The layer that gives a setting its value. */
tb_setting_layer tb_setting_source_locked(const tb_setting *setting);

#endif
