/* The input: its definitions, its players and their stacks of contexts, the
 * keys gathered between ticks, and each tick's evaluation of every action,
 * after Unreal's UEnhancedPlayerInput. */
#include "input_internal.h"

#include "badger/assert.h"
#include "badger/engine_settings.h"
#include "badger/fatal.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/size.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* The longest name of an action or a context, in bytes. */
#define TB_INPUT_MOST_NAME_LENGTH 64u

/* A key's value as it is gathered between ticks. */
typedef struct tb_input_key_state
{
    tb_input_value value;   /* A button's 1 or 0, an axis's last, a relative key's sum. */
    bool pressed;           /* A button went down since the last tick. */
    tb_input_device device; /* The device that fed it last; none for a replayed tick's. */
} tb_input_key_state;

/* A context on a player's stack, and the state of its mappings' rules. */
typedef struct tb_input_entry
{
    tb_input_context context;
    int32_t priority;
    uint64_t order;              /* When it was added: later ones see keys first. */
    size_t mapping_count;        /* Mappings its state was made for. */
    tb_input_key *keys;          /* Each mapping's key, as the player has it bound. */
    size_t *offsets;             /* Each mapping's first rule state. */
    bool *blocked;               /* Each mapping's key was down when added: ignored until up. */
    bool *active;                /* Each mapping sees its key this tick. */
    tb_input_rule_state *states; /* Each mapping's rules, then its context's rules of its action. */
} tb_input_entry;

/* An action as one player has it. */
typedef struct tb_input_action_runtime
{
    tb_input_action_status status;
    tb_input_rule_state *states; /* The action's own rules. */
    bool chorded;                /* Some rule of it waits on another action this tick. */
} tb_input_action_runtime;

/* A mapping a player bound to another key than its default. */
typedef struct tb_input_rebound
{
    tb_input_context context;
    size_t mapping;
    tb_input_key key; /* None: unbound. */
} tb_input_rebound;

typedef struct tb_input_bound
{
    uint32_t id;
    tb_input_action action;
    uint32_t events;
    tb_input_callback *callback;
    void *user;
    bool removed;
} tb_input_bound;

struct tb_input_player
{
    tb_input *input;
    size_t index;
    tb_input_entry *entries; /* By priority, highest first; of equal ones, the last added first. */
    size_t entry_count;
    size_t entry_capacity;
    uint64_t next_order;
    tb_input_key_state keys[TB_INPUT_KEY_COUNT];
    tb_input_value tick_keys[TB_INPUT_KEY_COUNT]; /* As the last tick saw them. */
    tb_input_action_runtime *actions;             /* One for each of the input's actions. */
    tb_input_bound *bindings;
    size_t binding_count;
    size_t binding_capacity;
    uint32_t next_binding;
    bool dispatching;
    tb_input_rebound *rebinds; /* In the order of contexts, then of mappings. */
    size_t rebind_count;
    size_t rebind_capacity;
    tb_input_device last_device;
};

/* A key fed, by player: what a recording holds. */
typedef struct tb_input_fed
{
    size_t player;
    tb_input_key key;
    tb_input_value value;
} tb_input_fed;

typedef struct tb_input_recorded_tick
{
    double seconds;
    tb_input_settings settings;
    size_t first; /* Its first event. */
    size_t count; /* Its events. */
} tb_input_recorded_tick;

struct tb_input_recording
{
    tb_input_recorded_tick *ticks;
    size_t tick_count;
    size_t tick_capacity;
    tb_input_fed *events;
    size_t event_count;
    size_t event_capacity;
};

typedef struct tb_input_device_entry
{
    tb_input_device device;
    size_t player;
} tb_input_device_entry;

/* What a listener waits for, and what it found. */
typedef struct tb_input_listener
{
    tb_input_capture capture;
    size_t player; /* Whose keys count; TB_INPUT_NO_PLAYER for every player's. */
    uint32_t classes;
    tb_input_value_type type;
    float threshold;
    float distance;
    const tb_input_key *cancel_keys;
    size_t cancel_key_count;
    /* By player: keys that were down or away from rest, not yet released. */
    bool held[TB_INPUT_MAX_PLAYERS][TB_INPUT_KEY_COUNT];
    float travel[TB_INPUT_MAX_PLAYERS][2]; /* The mouse's motion, X and Y, since it began. */
} tb_input_listener;

struct tb_input
{
    tb_input_action_def *actions;
    size_t action_count;
    size_t action_capacity;
    tb_input_context_def *contexts;
    size_t context_count;
    size_t context_capacity;
    tb_input_player players[TB_INPUT_MAX_PLAYERS];
    size_t player_count;
    tb_input_device_entry *devices;
    size_t device_count;
    size_t device_capacity;
    tb_input_settings settings;
    const tb_engine_settings *engine;
    tb_input_recording *recording;
    size_t recording_first; /* The recording's first event of the tick gathering. */
    double seconds;
    uint64_t ticks;
    bool mouse_known; /* The cursor's last place is known, for its motion. */
    double mouse_x;
    double mouse_y;
    tb_input_listener listener;
    bool listening_tick; /* The listener was waiting when this tick began. */
};

/* ---------------------------------------------------------------------------
 * Arrays.
 * ------------------------------------------------------------------------- */

/* Room for one more item. */
static void *tb_input_grow(void *items, size_t *capacity, size_t count, size_t size)
{
    size_t grown = 0;

    if (count < *capacity)
    {
        return items;
    }
    /* coverage-off: no array of the input holds SIZE_MAX bytes */
    if (!tb_size_grow(*capacity, count + 1, size, &grown))
    {
        tb_fatal_message("input: an array too large to grow");
    }
    /* coverage-on */
    *capacity = grown;
    return tb_realloc_array(items, grown, size);
}

/* ---------------------------------------------------------------------------
 * Values.
 * ------------------------------------------------------------------------- */

static tb_input_value tb_input_truncated(tb_input_value value, tb_input_value_type type)
{
    switch (type)
    {
    case TB_INPUT_VALUE_BOOL:
    case TB_INPUT_VALUE_AXIS1D:
        return (tb_input_value){value.x, 0.0f, 0.0f};
    case TB_INPUT_VALUE_AXIS2D:
        return (tb_input_value){value.x, value.y, 0.0f};
    case TB_INPUT_VALUE_AXIS3D:
    case TB_INPUT_VALUE_TYPE_COUNT:
        break;
    }
    return value;
}

/* A key's value as an action of a type takes it: a bool of a 2D key is its
 * length. */
static tb_input_value tb_input_as_type(tb_input_value value, tb_input_value_type type)
{
    if (type == TB_INPUT_VALUE_BOOL && (value.y != 0.0f || value.z != 0.0f))
    {
        return (tb_input_value){sqrtf(value.x * value.x + value.y * value.y + value.z * value.z),
                                0.0f, 0.0f};
    }
    return tb_input_truncated(value, type);
}

static float tb_input_highest(float kept, float candidate)
{
    return fabsf(candidate) > fabsf(kept) ? candidate : kept;
}

static tb_input_value tb_input_accumulate(tb_input_value total, tb_input_value value,
                                          tb_input_accumulation accumulation)
{
    if (accumulation == TB_INPUT_ACCUMULATE_CUMULATIVE)
    {
        return (tb_input_value){total.x + value.x, total.y + value.y, total.z + value.z};
    }
    return (tb_input_value){tb_input_highest(total.x, value.x), tb_input_highest(total.y, value.y),
                            tb_input_highest(total.z, value.z)};
}

/* ---------------------------------------------------------------------------
 * The input.
 * ------------------------------------------------------------------------- */

static void tb_input_set_device(tb_input *input, tb_input_device device, size_t player)
{
    for (size_t index = 0; index < input->device_count; index++)
    {
        if (input->devices[index].device == device)
        {
            input->devices[index].player = player;
            return;
        }
    }
    input->devices = tb_input_grow(input->devices, &input->device_capacity, input->device_count,
                                   sizeof(*input->devices));
    input->devices[input->device_count] = (tb_input_device_entry){device, player};
    input->device_count++;
}

tb_input *tb_input_create(const tb_input_options *options)
{
    const tb_input_settings defaults = TB_INPUT_SETTINGS_DEFAULT;
    tb_input *const input = tb_calloc(1, sizeof(*input));
    const size_t players = options != NULL && options->player_count > 0 ? options->player_count : 1;

    TB_ASSERT_MSG(players <= TB_INPUT_MAX_PLAYERS, "at most TB_INPUT_MAX_PLAYERS players");
    input->player_count = players;
    input->settings = defaults;
    input->listener.capture = (tb_input_capture){.player = TB_INPUT_NO_PLAYER};
    for (size_t index = 0; index < players; index++)
    {
        input->players[index].input = input;
        input->players[index].index = index;
    }
    tb_input_set_device(input, TB_INPUT_DEVICE_KEYBOARD, 0);
    tb_input_set_device(input, TB_INPUT_DEVICE_MOUSE, 0);
    return input;
}

static void tb_input_entry_free(tb_input_entry *entry)
{
    tb_free(entry->keys);
    tb_free(entry->offsets);
    tb_free(entry->blocked);
    tb_free(entry->active);
    tb_free(entry->states);
    entry->keys = NULL;
    entry->offsets = NULL;
    entry->blocked = NULL;
    entry->active = NULL;
    entry->states = NULL;
    entry->mapping_count = 0;
}

static void tb_input_player_free(tb_input_player *player, size_t action_count)
{
    for (size_t index = 0; index < player->entry_count; index++)
    {
        tb_input_entry_free(&player->entries[index]);
    }
    tb_free(player->entries);
    for (size_t index = 0; index < action_count; index++)
    {
        tb_free(player->actions[index].states);
    }
    tb_free(player->actions);
    tb_free(player->bindings);
    tb_free(player->rebinds);
}

static void tb_input_context_free(tb_input_context_def *context)
{
    for (size_t index = 0; index < context->mapping_count; index++)
    {
        tb_input_rules_free(&context->mappings[index].rules);
    }
    for (size_t index = 0; index < context->action_rules_count; index++)
    {
        tb_input_rules_free(&context->action_rules[index].rules);
    }
    tb_free(context->mappings);
    tb_free(context->action_rules);
    tb_free(context->name);
}

void tb_input_destroy(tb_input *input)
{
    if (input == NULL)
    {
        return;
    }
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_player_free(&input->players[index], input->action_count);
    }
    for (size_t index = 0; index < input->action_count; index++)
    {
        tb_input_rules_free(&input->actions[index].rules);
        tb_free(input->actions[index].name);
    }
    for (size_t index = 0; index < input->context_count; index++)
    {
        tb_input_context_free(&input->contexts[index]);
    }
    tb_free(input->actions);
    tb_free(input->contexts);
    tb_free(input->devices);
    tb_free(input);
}

size_t tb_input_player_count(const tb_input *input)
{
    TB_ASSERT(input != NULL);
    return input->player_count;
}

tb_input_player *tb_input_player_at(tb_input *input, size_t index)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(index < input->player_count, "a player of the input");
    return &input->players[index];
}

tb_input *tb_input_player_input(const tb_input_player *player)
{
    TB_ASSERT(player != NULL);
    return player->input;
}

size_t tb_input_player_index(const tb_input_player *player)
{
    TB_ASSERT(player != NULL);
    return player->index;
}

void tb_input_assign_device(tb_input *input, tb_input_device device, size_t player)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(device != TB_INPUT_DEVICE_NONE, "a device");
    TB_ASSERT_MSG(player == TB_INPUT_NO_PLAYER || player < input->player_count,
                  "a player of the input, or none");
    tb_input_set_device(input, device, player);
}

size_t tb_input_device_player(const tb_input *input, tb_input_device device)
{
    TB_ASSERT(input != NULL);
    for (size_t index = 0; index < input->device_count; index++)
    {
        if (input->devices[index].device == device)
        {
            return input->devices[index].player;
        }
    }
    return TB_INPUT_NO_PLAYER;
}

void tb_input_set_settings(tb_input *input, const tb_input_settings *settings)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(settings != NULL);
    input->settings = *settings;
    input->engine = NULL;
}

void tb_input_follow_settings(tb_input *input, const tb_engine_settings *engine)
{
    TB_ASSERT(input != NULL);
    input->engine = engine;
}

void tb_input_get_settings(const tb_input *input, tb_input_settings *settings)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(settings != NULL);
    *settings = input->settings;
}

static void tb_input_read_engine_settings(tb_input *input)
{
    const tb_engine_settings *const engine = input->engine;

    input->settings = (tb_input_settings){
        .mouse_sensitivity = (float)tb_setting_number(engine->controls.mouse_sensitivity),
        .mouse_invert_y = tb_setting_boolean(engine->controls.mouse_invert_y),
        .mouse_smoothing = tb_setting_boolean(engine->controls.mouse_smoothing),
        .gamepad_horizontal_sensitivity =
            (float)tb_setting_number(engine->controls.gamepad_horizontal_sensitivity),
        .gamepad_vertical_sensitivity =
            (float)tb_setting_number(engine->controls.gamepad_vertical_sensitivity),
        .gamepad_invert_y = tb_setting_boolean(engine->controls.gamepad_invert_y),
        .gamepad_left_dead_zone = (float)tb_setting_number(engine->controls.gamepad_left_dead_zone),
        .gamepad_right_dead_zone =
            (float)tb_setting_number(engine->controls.gamepad_right_dead_zone),
        .touch_sensitivity = (float)tb_setting_number(engine->controls.touch_sensitivity)};
}

/* ---------------------------------------------------------------------------
 * Actions and contexts.
 * ------------------------------------------------------------------------- */

bool tb_input_name_valid(const char *name)
{
    const size_t length = tb_str_length(name);

    if (length == 0 || length > TB_INPUT_MOST_NAME_LENGTH)
    {
        return false;
    }
    for (size_t index = 0; index < length; index++)
    {
        const char character = name[index];
        const bool lower = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (!lower && !digit && character != '_')
        {
            return false;
        }
    }
    return true;
}

static bool tb_input_action_valid(const tb_input *input, tb_input_action action)
{
    return action.id >= 1 && action.id <= input->action_count;
}

static bool tb_input_context_valid(const tb_input *input, tb_input_context context)
{
    return context.id >= 1 && context.id <= input->context_count;
}

const tb_input_action_def *tb_input_action_def_of(const tb_input *input, tb_input_action action)
{
    TB_ASSERT_MSG(tb_input_action_valid(input, action), "an action of the input");
    return &input->actions[action.id - 1];
}

const tb_input_context_def *tb_input_context_def_of(const tb_input *input, tb_input_context context)
{
    TB_ASSERT_MSG(tb_input_context_valid(input, context), "a context of the input");
    return &input->contexts[context.id - 1];
}

const char *tb_input_check_action(const tb_input *input, const tb_input_action_desc *desc)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(desc != NULL);
    if (desc->name == NULL || !tb_input_name_valid(desc->name))
    {
        return "a name: lowercase letters, digits and \"_\", at most 64 bytes";
    }
    if (tb_input_find_action(input, desc->name).id != 0)
    {
        return "a name an action already has";
    }
    if ((unsigned)desc->type >= (unsigned)TB_INPUT_VALUE_TYPE_COUNT)
    {
        return "a value type other than bool, axis1d, axis2d and axis3d";
    }
    if ((unsigned)desc->accumulation >= (unsigned)TB_INPUT_ACCUMULATION_COUNT)
    {
        return "an accumulation other than highest and cumulative";
    }
    return tb_input_check_rules(input, TB_INPUT_ACTION_NONE, desc->modifiers, desc->modifier_count,
                                desc->triggers, desc->trigger_count);
}

tb_input_action tb_input_create_action(tb_input *input, const tb_input_action_desc *desc)
{
    tb_input_action_def def = {0};

    TB_ASSERT_MSG(tb_input_check_action(input, desc) == NULL, "a valid action");
    def.name = tb_str_dup(NULL, desc->name);
    def.type = desc->type;
    def.accumulation = desc->accumulation;
    tb_input_rules_copy(&def.rules, desc->modifiers, desc->modifier_count, desc->triggers,
                        desc->trigger_count);
    input->actions = tb_input_grow(input->actions, &input->action_capacity, input->action_count,
                                   sizeof(*input->actions));
    input->actions[input->action_count] = def;
    input->action_count++;
    const size_t states = tb_input_rules_state_count(&def.rules);
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_player *const player = &input->players[index];
        player->actions =
            tb_realloc_array(player->actions, input->action_count, sizeof(*player->actions));
        player->actions[input->action_count - 1] = (tb_input_action_runtime){
            .status = {.last_triggered_at = -1.0},
            .states = states > 0 ? tb_calloc(states, sizeof(tb_input_rule_state)) : NULL};
    }
    return (tb_input_action){(uint32_t)input->action_count};
}

tb_input_action tb_input_find_action(const tb_input *input, const char *name)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(name != NULL);
    for (size_t index = 0; index < input->action_count; index++)
    {
        if (tb_str_equal(input->actions[index].name, name))
        {
            return (tb_input_action){(uint32_t)(index + 1)};
        }
    }
    return TB_INPUT_ACTION_NONE;
}

size_t tb_input_action_count(const tb_input *input)
{
    TB_ASSERT(input != NULL);
    return input->action_count;
}

tb_input_action tb_input_action_at(const tb_input *input, size_t index)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(index < input->action_count, "an action of the input");
    return (tb_input_action){(uint32_t)(index + 1)};
}

const char *tb_input_action_name(const tb_input *input, tb_input_action action)
{
    TB_ASSERT(input != NULL);
    return tb_input_action_def_of(input, action)->name;
}

tb_input_value_type tb_input_action_type(const tb_input *input, tb_input_action action)
{
    TB_ASSERT(input != NULL);
    return tb_input_action_def_of(input, action)->type;
}

tb_input_context tb_input_create_context(tb_input *input, const char *name)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT_MSG(tb_input_name_valid(name), "a context's name: lowercase letters, digits, \"_\"");
    TB_ASSERT_MSG(tb_input_find_context(input, name).id == 0, "a name no context has");
    input->contexts = tb_input_grow(input->contexts, &input->context_capacity, input->context_count,
                                    sizeof(*input->contexts));
    input->contexts[input->context_count] = (tb_input_context_def){.name = tb_str_dup(NULL, name)};
    input->context_count++;
    return (tb_input_context){(uint32_t)input->context_count};
}

tb_input_context tb_input_find_context(const tb_input *input, const char *name)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(name != NULL);
    for (size_t index = 0; index < input->context_count; index++)
    {
        if (tb_str_equal(input->contexts[index].name, name))
        {
            return (tb_input_context){(uint32_t)(index + 1)};
        }
    }
    return TB_INPUT_CONTEXT_NONE;
}

size_t tb_input_context_count(const tb_input *input)
{
    TB_ASSERT(input != NULL);
    return input->context_count;
}

tb_input_context tb_input_context_at(const tb_input *input, size_t index)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(index < input->context_count, "a context of the input");
    return (tb_input_context){(uint32_t)(index + 1)};
}

const char *tb_input_context_name(const tb_input *input, tb_input_context context)
{
    TB_ASSERT(input != NULL);
    return tb_input_context_def_of(input, context)->name;
}

static void tb_input_entry_build(const tb_input_player *player, tb_input_entry *entry);

/* A context changed: every player that has it starts it over. */
static void tb_input_context_changed(tb_input *input, tb_input_context context)
{
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_player *const player = &input->players[index];
        for (size_t entry = 0; entry < player->entry_count; entry++)
        {
            if (player->entries[entry].context.id == context.id)
            {
                tb_input_entry_build(player, &player->entries[entry]);
            }
        }
    }
}

const char *tb_input_check_mapping(const tb_input *input, const tb_input_mapping_desc *desc)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(desc != NULL);
    if (!tb_input_action_valid(input, desc->action))
    {
        return "a mapping of no action";
    }
    if (desc->key <= TB_INPUT_KEY_NONE || desc->key >= TB_INPUT_KEY_COUNT)
    {
        return "a mapping of no key";
    }
    return tb_input_check_rules(input, desc->action, desc->modifiers, desc->modifier_count,
                                desc->triggers, desc->trigger_count);
}

void tb_input_context_map(tb_input *input, tb_input_context context,
                          const tb_input_mapping_desc *desc)
{
    tb_input_mapping_def mapping = {0};

    TB_ASSERT_MSG(tb_input_check_mapping(input, desc) == NULL, "a valid mapping");
    TB_ASSERT_MSG(tb_input_context_valid(input, context), "a context of the input");
    tb_input_context_def *const def = &input->contexts[context.id - 1];
    mapping.action = desc->action;
    mapping.key = desc->key;
    mapping.pass_through = desc->pass_through;
    tb_input_rules_copy(&mapping.rules, desc->modifiers, desc->modifier_count, desc->triggers,
                        desc->trigger_count);
    def->mappings = tb_input_grow(def->mappings, &def->mapping_capacity, def->mapping_count,
                                  sizeof(*def->mappings));
    def->mappings[def->mapping_count] = mapping;
    def->mapping_count++;
    tb_input_context_changed(input, context);
}

size_t tb_input_context_mapping_count(const tb_input *input, tb_input_context context)
{
    TB_ASSERT(input != NULL);
    return tb_input_context_def_of(input, context)->mapping_count;
}

const char *tb_input_check_action_rules(const tb_input *input,
                                        const tb_input_action_rules_desc *desc)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(desc != NULL);
    if (!tb_input_action_valid(input, desc->action))
    {
        return "rules of no action";
    }
    return tb_input_check_rules(input, desc->action, desc->modifiers, desc->modifier_count,
                                desc->triggers, desc->trigger_count);
}

/* A context's rules of an action, or NULL. */
static const tb_input_rules *tb_input_context_rules(const tb_input_context_def *context,
                                                    tb_input_action action)
{
    for (size_t index = 0; index < context->action_rules_count; index++)
    {
        if (context->action_rules[index].action.id == action.id)
        {
            return &context->action_rules[index].rules;
        }
    }
    return NULL;
}

void tb_input_context_set_action_rules(tb_input *input, tb_input_context context,
                                       const tb_input_action_rules_desc *desc)
{
    TB_ASSERT_MSG(tb_input_check_action_rules(input, desc) == NULL, "valid action rules");
    TB_ASSERT_MSG(tb_input_context_valid(input, context), "a context of the input");
    tb_input_context_def *const def = &input->contexts[context.id - 1];
    tb_input_rules rules = {0};
    tb_input_rules_copy(&rules, desc->modifiers, desc->modifier_count, desc->triggers,
                        desc->trigger_count);
    size_t index = 0;
    while (index < def->action_rules_count && def->action_rules[index].action.id != desc->action.id)
    {
        index++;
    }
    if (index < def->action_rules_count)
    {
        tb_input_rules_free(&def->action_rules[index].rules);
        def->action_rules[index].rules = rules;
    }
    else
    {
        def->action_rules = tb_input_grow(def->action_rules, &def->action_rules_capacity,
                                          def->action_rules_count, sizeof(*def->action_rules));
        def->action_rules[def->action_rules_count] =
            (tb_input_action_rules_def){.action = desc->action, .rules = rules};
        def->action_rules_count++;
    }
    tb_input_context_changed(input, context);
}

/* ---------------------------------------------------------------------------
 * Players' contexts.
 * ------------------------------------------------------------------------- */

static bool tb_input_button_held(const tb_input_player *player, tb_input_key key)
{
    if (key == TB_INPUT_KEY_NONE)
    {
        return false;
    }
    const tb_input_key_state *const state = &player->keys[key];
    return tb_input_key_kind_of(key) == TB_INPUT_KEY_KIND_BUTTON &&
           (state->value.x != 0.0f || state->pressed);
}

/* Make an entry's state for its context as it is now: its rules start over,
 * and its mappings of buttons held wait for them to be released. */
static void tb_input_entry_build(const tb_input_player *player, tb_input_entry *entry)
{
    const tb_input_context_def *const def = tb_input_context_def_of(player->input, entry->context);
    size_t states = 0;

    tb_input_entry_free(entry);
    entry->mapping_count = def->mapping_count;
    if (def->mapping_count == 0)
    {
        return;
    }
    entry->keys = tb_calloc(def->mapping_count, sizeof(*entry->keys));
    entry->offsets = tb_calloc(def->mapping_count, sizeof(*entry->offsets));
    entry->blocked = tb_calloc(def->mapping_count, sizeof(*entry->blocked));
    entry->active = tb_calloc(def->mapping_count, sizeof(*entry->active));
    for (size_t index = 0; index < def->mapping_count; index++)
    {
        const tb_input_mapping_def *const mapping = &def->mappings[index];
        const tb_input_rules *const extra = tb_input_context_rules(def, mapping->action);
        entry->keys[index] = tb_input_binding_key(player, entry->context, index);
        entry->offsets[index] = states;
        entry->blocked[index] = tb_input_button_held(player, entry->keys[index]);
        states += tb_input_rules_state_count(&mapping->rules);
        states += extra != NULL ? tb_input_rules_state_count(extra) : 0;
    }
    entry->states = states > 0 ? tb_calloc(states, sizeof(*entry->states)) : NULL;
}

/* Whether `left` sees keys before `right`. */
static bool tb_input_entry_before(const tb_input_entry *left, const tb_input_entry *right)
{
    if (left->priority != right->priority)
    {
        return left->priority > right->priority;
    }
    return left->order > right->order;
}

/* Move the entry at `index` to its place. */
static void tb_input_entry_place(tb_input_player *player, size_t index)
{
    const tb_input_entry moved = player->entries[index];
    size_t place = 0;

    memmove(&player->entries[index], &player->entries[index + 1],
            (player->entry_count - index - 1) * sizeof(*player->entries));
    player->entry_count--;
    while (place < player->entry_count && tb_input_entry_before(&player->entries[place], &moved))
    {
        place++;
    }
    memmove(&player->entries[place + 1], &player->entries[place],
            (player->entry_count - place) * sizeof(*player->entries));
    player->entries[place] = moved;
    player->entry_count++;
}

static size_t tb_input_entry_index(const tb_input_player *player, tb_input_context context)
{
    for (size_t index = 0; index < player->entry_count; index++)
    {
        if (player->entries[index].context.id == context.id)
        {
            return index;
        }
    }
    return player->entry_count;
}

void tb_input_add_context(tb_input_player *player, tb_input_context context, int32_t priority)
{
    TB_ASSERT(player != NULL);
    TB_ASSERT_MSG(tb_input_context_valid(player->input, context), "a context of the input");
    size_t index = tb_input_entry_index(player, context);
    if (index == player->entry_count)
    {
        player->entries = tb_input_grow(player->entries, &player->entry_capacity,
                                        player->entry_count, sizeof(*player->entries));
        player->next_order++;
        player->entries[index] = (tb_input_entry){.context = context, .order = player->next_order};
        player->entry_count++;
        tb_input_entry_build(player, &player->entries[index]);
    }
    player->entries[index].priority = priority;
    tb_input_entry_place(player, index);
}

bool tb_input_remove_context(tb_input_player *player, tb_input_context context)
{
    TB_ASSERT(player != NULL);
    const size_t index = tb_input_entry_index(player, context);
    if (index == player->entry_count)
    {
        return false;
    }
    tb_input_entry_free(&player->entries[index]);
    memmove(&player->entries[index], &player->entries[index + 1],
            (player->entry_count - index - 1) * sizeof(*player->entries));
    player->entry_count--;
    return true;
}

void tb_input_clear_contexts(tb_input_player *player)
{
    TB_ASSERT(player != NULL);
    for (size_t index = 0; index < player->entry_count; index++)
    {
        tb_input_entry_free(&player->entries[index]);
    }
    player->entry_count = 0;
}

bool tb_input_has_context(const tb_input_player *player, tb_input_context context)
{
    TB_ASSERT(player != NULL);
    return tb_input_entry_index(player, context) < player->entry_count;
}

/* ---------------------------------------------------------------------------
 * Gathering.
 * ------------------------------------------------------------------------- */

static void tb_input_apply(tb_input_player *player, tb_input_device device, tb_input_key key,
                           tb_input_value value);

/* A virtual button's state from its axis's value toward it. */
static void tb_input_apply_virtual(tb_input_player *player, tb_input_device device,
                                   tb_input_key button, float value,
                                   const tb_gamepad_threshold *threshold)
{
    const bool was_down = player->keys[button].value.x != 0.0f;
    const bool down = tb_gamepad_analog_button(value, was_down, threshold);

    if (down != was_down)
    {
        tb_input_apply(player, device, button, (tb_input_value){down ? 1.0f : 0.0f, 0.0f, 0.0f});
    }
}

/* The buttons an axis makes, from its new value: a stick's directions and a
 * trigger's button. They are made here rather than fed, so that a
 * recording, which holds the axis, makes them again. */
static void tb_input_apply_virtuals(tb_input_player *player, tb_input_device device,
                                    tb_input_key axis, float value)
{
    tb_input_key positive = TB_INPUT_KEY_NONE;
    tb_input_key negative = TB_INPUT_KEY_NONE;
    tb_gamepad_threshold threshold = {0};

    if (!tb_input_key_virtual_buttons(axis, &positive, &negative, &threshold))
    {
        return;
    }
    tb_input_apply_virtual(player, device, positive, value, &threshold);
    if (negative != TB_INPUT_KEY_NONE)
    {
        tb_input_apply_virtual(player, device, negative, -value, &threshold);
    }
}

/* A key's value into a player's keys, from a device. */
static void tb_input_apply(tb_input_player *player, tb_input_device device, tb_input_key key,
                           tb_input_value value)
{
    tb_input_key_state *const state = &player->keys[key];
    tb_input_key x = TB_INPUT_KEY_NONE;
    tb_input_key y = TB_INPUT_KEY_NONE;

    state->device = device;
    if (!tb_input_value_is_zero(value))
    {
        player->last_device = device;
    }
    switch (tb_input_key_kind_of(key))
    {
    case TB_INPUT_KEY_KIND_BUTTON:
        state->value = (tb_input_value){value.x != 0.0f ? 1.0f : 0.0f, 0.0f, 0.0f};
        state->pressed = state->pressed || value.x != 0.0f;
        return;
    case TB_INPUT_KEY_KIND_AXIS:
        state->value = (tb_input_value){value.x, 0.0f, 0.0f};
        tb_input_apply_virtuals(player, device, key, value.x);
        return;
    case TB_INPUT_KEY_KIND_RELATIVE:
        state->value.x += value.x;
        return;
    case TB_INPUT_KEY_KIND_COMPOSITE:
        tb_input_key_components(key, &x, &y);
        tb_input_apply(player, device, x, (tb_input_value){value.x, 0.0f, 0.0f});
        tb_input_apply(player, device, y, (tb_input_value){value.y, 0.0f, 0.0f});
        return;
    case TB_INPUT_KEY_KIND_POSITION:
        state->value = (tb_input_value){value.x, value.y, 0.0f};
        return;
    }
}

static void tb_input_record_event(tb_input *input, size_t player, tb_input_key key,
                                  tb_input_value value)
{
    tb_input_recording *const recording = input->recording;

    if (recording == NULL)
    {
        return;
    }
    recording->events = tb_input_grow(recording->events, &recording->event_capacity,
                                      recording->event_count, sizeof(*recording->events));
    recording->events[recording->event_count] = (tb_input_fed){player, key, value};
    recording->event_count++;
}

void tb_input_feed_key(tb_input *input, tb_input_device device, tb_input_key key,
                       tb_input_value value)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(key > TB_INPUT_KEY_NONE && key < TB_INPUT_KEY_COUNT, "a key");
    TB_ASSERT_MSG(isfinite(value.x) && isfinite(value.y) && isfinite(value.z), "a finite value");
    const size_t player = tb_input_device_player(input, device);
    if (player == TB_INPUT_NO_PLAYER)
    {
        return;
    }
    tb_input_record_event(input, player, key, value);
    tb_input_apply(&input->players[player], device, key, value);
}

static tb_input_value tb_input_axis_value(double value)
{
    return (tb_input_value){(float)value, 0.0f, 0.0f};
}

/* Release every key and button the window gave, as losing the focus does. */
static bool tb_input_release_window_keys(tb_input *input)
{
    bool released = false;

    for (size_t key = TB_INPUT_KEY_A; tb_input_key_is_window_button((tb_input_key)key); key++)
    {
        const tb_input_device device =
            key >= TB_INPUT_KEY_MOUSE_LEFT ? TB_INPUT_DEVICE_MOUSE : TB_INPUT_DEVICE_KEYBOARD;
        const size_t player = tb_input_device_player(input, device);
        if (player == TB_INPUT_NO_PLAYER || input->players[player].keys[key].value.x == 0.0f)
        {
            continue;
        }
        tb_input_feed_key(input, device, (tb_input_key)key, (tb_input_value){0});
        released = true;
    }
    input->mouse_known = false;
    return released;
}

static bool tb_input_feed_button(tb_input *input, tb_input_device device, tb_input_key key,
                                 bool down)
{
    if (key == TB_INPUT_KEY_NONE)
    {
        return false;
    }
    tb_input_feed_key(input, device, key, tb_input_axis_value(down ? 1.0 : 0.0));
    return true;
}

static bool tb_input_feed_motion(tb_input *input, double right, double up)
{
    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_XY,
                      (tb_input_value){(float)right, (float)up, 0.0f});
    return true;
}

static bool tb_input_feed_cursor(tb_input *input, const tb_window_event *event)
{
    const bool known = input->mouse_known;
    const double right = event->x - input->mouse_x;
    const double up = input->mouse_y - event->y;

    input->mouse_known = true;
    input->mouse_x = event->x;
    input->mouse_y = event->y;
    if (!known)
    {
        return false;
    }
    return tb_input_feed_motion(input, right, up);
}

static bool tb_input_feed_wheel(tb_input *input, double notches)
{
    if (notches > 0.0)
    {
        tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL_UP,
                          tb_input_axis_value(notches));
    }
    if (notches < 0.0)
    {
        tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL_DOWN,
                          tb_input_axis_value(-notches));
    }
    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL,
                      tb_input_axis_value(notches));
    return true;
}

bool tb_input_feed_window_event(tb_input *input, const tb_window_event *event)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(event != NULL);
    switch (event->type)
    {
    case TB_WINDOW_EVENT_KEY_DOWN:
        return !event->repeat && tb_input_feed_button(input, TB_INPUT_DEVICE_KEYBOARD,
                                                      tb_input_key_from_window(event->key), true);
    case TB_WINDOW_EVENT_KEY_UP:
        return tb_input_feed_button(input, TB_INPUT_DEVICE_KEYBOARD,
                                    tb_input_key_from_window(event->key), false);
    case TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
        return tb_input_feed_button(input, TB_INPUT_DEVICE_MOUSE,
                                    tb_input_key_from_mouse_button(event->button), true);
    case TB_WINDOW_EVENT_MOUSE_BUTTON_UP:
        return tb_input_feed_button(input, TB_INPUT_DEVICE_MOUSE,
                                    tb_input_key_from_mouse_button(event->button), false);
    case TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION:
        /* The window's delta_y is toward the player; the key's Y is away. */
        return tb_input_feed_motion(input, event->delta_x, -event->delta_y);
    case TB_WINDOW_EVENT_MOUSE_MOVED:
        return tb_input_feed_cursor(input, event);
    case TB_WINDOW_EVENT_MOUSE_ENTERED:
        input->mouse_known = true;
        input->mouse_x = event->x;
        input->mouse_y = event->y;
        return false;
    case TB_WINDOW_EVENT_MOUSE_LEFT:
        input->mouse_known = false;
        return false;
    case TB_WINDOW_EVENT_MOUSE_WHEEL:
        return tb_input_feed_wheel(input, event->delta_y);
    case TB_WINDOW_EVENT_FOCUS_LOST:
        return tb_input_release_window_keys(input);
    case TB_WINDOW_EVENT_CLOSE_REQUESTED:
    case TB_WINDOW_EVENT_RESIZED:
    case TB_WINDOW_EVENT_SCALE_CHANGED:
    case TB_WINDOW_EVENT_FOCUS_GAINED:
    case TB_WINDOW_EVENT_MINIMIZED:
    case TB_WINDOW_EVENT_RESTORED:
    case TB_WINDOW_EVENT_DISPLAYS_CHANGED:
    case TB_WINDOW_EVENT_WAKE:
    case TB_WINDOW_EVENT_TEXT_INPUT:
        break;
    }
    return false;
}

/* Feed a key of a pad's when the player has another value for it. */
static void tb_input_feed_changed(tb_input *input, tb_input_device device,
                                  const tb_input_player *player, tb_input_key key, float value)
{
    if (player->keys[key].value.x == value)
    {
        return;
    }
    tb_input_feed_key(input, device, key, (tb_input_value){value, 0.0f, 0.0f});
}

void tb_input_feed_gamepad(tb_input *input, tb_input_device device, const tb_gamepad_state *state)
{
    size_t player = TB_INPUT_NO_PLAYER;

    TB_ASSERT(input != NULL);
    TB_ASSERT(state != NULL);
    TB_ASSERT_MSG(device != TB_INPUT_DEVICE_NONE, "a device");
    player = tb_input_device_player(input, device);
    if (player == TB_INPUT_NO_PLAYER)
    {
        return;
    }
    for (size_t button = 0; button < TB_GAMEPAD_BUTTON_COUNT; button++)
    {
        const bool down =
            state->connected && tb_gamepad_button_down(state, (tb_gamepad_button)button);
        tb_input_feed_changed(input, device, &input->players[player],
                              tb_input_key_from_gamepad_button((tb_gamepad_button)button),
                              down ? 1.0f : 0.0f);
    }
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
    {
        const float value = state->connected ? state->axes[axis] : 0.0f;
        tb_input_feed_changed(input, device, &input->players[player],
                              tb_input_key_from_gamepad_axis((tb_gamepad_axis)axis), value);
    }
}

/* ---------------------------------------------------------------------------
 * Evaluation.
 * ------------------------------------------------------------------------- */

/* The keys' values for this tick: a button pressed since the last is down
 * for it, and a 2D key is its X and Y keys. */
static void tb_input_take_keys(tb_input_player *player)
{
    tb_input_key x = TB_INPUT_KEY_NONE;
    tb_input_key y = TB_INPUT_KEY_NONE;

    for (size_t key = 1; key < TB_INPUT_KEY_COUNT; key++)
    {
        const tb_input_key_state *const state = &player->keys[key];
        const bool button = tb_input_key_kind_of((tb_input_key)key) == TB_INPUT_KEY_KIND_BUTTON;
        player->tick_keys[key] = state->value;
        if (button && state->pressed)
        {
            player->tick_keys[key] = (tb_input_value){1.0f, 0.0f, 0.0f};
        }
    }
    for (size_t key = 1; key < TB_INPUT_KEY_COUNT; key++)
    {
        if (tb_input_key_kind_of((tb_input_key)key) != TB_INPUT_KEY_KIND_COMPOSITE)
        {
            continue;
        }
        tb_input_key_components((tb_input_key)key, &x, &y);
        player->tick_keys[key] =
            (tb_input_value){player->tick_keys[x].x, player->tick_keys[y].x, 0.0f};
    }
}

/* Start gathering for the next tick: presses are spent, and relative keys
 * start from zero. */
static void tb_input_spend_keys(tb_input_player *player)
{
    for (size_t key = 1; key < TB_INPUT_KEY_COUNT; key++)
    {
        player->keys[key].pressed = false;
        if (tb_input_key_kind_of((tb_input_key)key) == TB_INPUT_KEY_KIND_RELATIVE)
        {
            player->keys[key].value = (tb_input_value){0};
        }
    }
}

/* Which mappings see their keys this tick: those whose keys no context
 * above consumed, and that do not wait for a key held when they came. */
static void tb_input_choose_mappings(tb_input_player *player, bool listened)
{
    bool consumed[TB_INPUT_KEY_COUNT] = {0};

    for (size_t index = 0; index < player->input->action_count; index++)
    {
        player->actions[index].chorded =
            tb_input_rules_chorded(&player->input->actions[index].rules);
    }
    for (size_t index = 0; index < player->entry_count; index++)
    {
        tb_input_entry *const entry = &player->entries[index];
        const tb_input_context_def *const def =
            tb_input_context_def_of(player->input, entry->context);
        for (size_t mapping = 0; mapping < def->mapping_count; mapping++)
        {
            const tb_input_mapping_def *const map = &def->mappings[mapping];
            const tb_input_rules *const extra = tb_input_context_rules(def, map->action);
            const tb_input_key key = entry->keys[mapping];
            if (key == TB_INPUT_KEY_NONE)
            {
                entry->active[mapping] = false;
                continue;
            }
            if (entry->blocked[mapping] && tb_input_value_is_zero(player->tick_keys[key]))
            {
                entry->blocked[mapping] = false;
            }
            entry->active[mapping] = !listened && !consumed[key] && !entry->blocked[mapping];
            if (entry->active[mapping] && (tb_input_rules_chorded(&map->rules) ||
                                           (extra != NULL && tb_input_rules_chorded(extra))))
            {
                player->actions[map->action.id - 1].chorded = true;
            }
        }
        for (size_t mapping = 0; mapping < def->mapping_count; mapping++)
        {
            if (!def->mappings[mapping].pass_through && entry->keys[mapping] != TB_INPUT_KEY_NONE)
            {
                consumed[entry->keys[mapping]] = true;
            }
        }
    }
}

static tb_input_value tb_input_apply_modifiers(const tb_input_player *player,
                                               const tb_input_rules *rules,
                                               tb_input_rule_state *states, tb_input_value value)
{
    for (size_t index = 0; index < rules->modifier_count; index++)
    {
        value = tb_input_modify(&rules->modifiers[index], &states[index], value,
                                &player->input->settings);
    }
    return value;
}

static void tb_input_apply_triggers(const tb_input_player *player, const tb_input_rules *rules,
                                    tb_input_rule_state *states, tb_input_value value,
                                    double seconds, tb_input_tracker *tracker)
{
    for (size_t index = 0; index < rules->trigger_count; index++)
    {
        const tb_input_trigger *const trigger = &rules->triggers[index];
        const tb_input_trigger_state chord =
            trigger->chord.id != 0 ? player->actions[trigger->chord.id - 1].status.state
                                   : TB_INPUT_STATE_NONE;
        const tb_input_trigger_state state = tb_input_trigger_update(
            trigger, &states[rules->modifier_count + index], value, seconds, chord);
        tb_input_tracker_add(tracker, trigger->type, state);
    }
}

/* What the mappings of an action come to in a tick. */
typedef struct tb_input_mapped
{
    tb_input_value value;         /* Their values, combined. */
    tb_input_trigger_state state; /* The highest of their states. */
    bool applied;                 /* Some mapping had triggers. */
} tb_input_mapped;

static void tb_input_evaluate_mapping(const tb_input_player *player, const tb_input_entry *entry,
                                      const tb_input_context_def *context, size_t index,
                                      double seconds, tb_input_mapped *mapped)
{
    const tb_input_mapping_def *const mapping = &context->mappings[index];
    const tb_input_action_def *const action =
        tb_input_action_def_of(player->input, mapping->action);
    const tb_input_rules *const extra = tb_input_context_rules(context, mapping->action);
    tb_input_rule_state *const states = &entry->states[entry->offsets[index]];
    tb_input_rule_state *const extra_states = &states[tb_input_rules_state_count(&mapping->rules)];
    tb_input_tracker tracker = TB_INPUT_TRACKER_INIT;

    tb_input_value value = tb_input_as_type(player->tick_keys[entry->keys[index]], action->type);
    value = tb_input_apply_modifiers(player, &mapping->rules, states, value);
    if (extra != NULL)
    {
        value = tb_input_apply_modifiers(player, extra, extra_states, value);
    }
    value = tb_input_truncated(value, action->type);
    tb_input_apply_triggers(player, &mapping->rules, states, value, seconds, &tracker);
    if (extra != NULL)
    {
        tb_input_apply_triggers(player, extra, extra_states, value, seconds, &tracker);
    }
    const tb_input_trigger_state state = tb_input_tracker_state(&tracker, value);
    mapped->applied = mapped->applied || tracker.count > 0;
    mapped->state = state > mapped->state ? state : mapped->state;
    mapped->value = tb_input_accumulate(mapped->value, value, action->accumulation);
}

/* The events of a move from one state to another, as Unreal's
 * ETriggerEventInternal. */
static uint32_t tb_input_transition(tb_input_trigger_state from, tb_input_trigger_state to)
{
    static const uint32_t events[3][3] = {
        [TB_INPUT_STATE_NONE] = {[TB_INPUT_STATE_ONGOING] = TB_INPUT_EVENT_STARTED,
                                 [TB_INPUT_STATE_TRIGGERED] =
                                     TB_INPUT_EVENT_STARTED | TB_INPUT_EVENT_TRIGGERED},
        [TB_INPUT_STATE_ONGOING] = {[TB_INPUT_STATE_NONE] = TB_INPUT_EVENT_CANCELED,
                                    [TB_INPUT_STATE_ONGOING] = TB_INPUT_EVENT_ONGOING,
                                    [TB_INPUT_STATE_TRIGGERED] = TB_INPUT_EVENT_TRIGGERED},
        [TB_INPUT_STATE_TRIGGERED] = {[TB_INPUT_STATE_NONE] = TB_INPUT_EVENT_COMPLETED,
                                      [TB_INPUT_STATE_ONGOING] = TB_INPUT_EVENT_ONGOING,
                                      [TB_INPUT_STATE_TRIGGERED] = TB_INPUT_EVENT_TRIGGERED}};
    return events[from][to];
}

static void tb_input_settle(tb_input_player *player, tb_input_action_runtime *runtime,
                            const tb_input_action_def *def, tb_input_trigger_state state,
                            tb_input_value value, double seconds)
{
    tb_input_action_status *const status = &runtime->status;

    status->events = tb_input_transition(status->state, state);
    status->state = state;
    status->value = state != TB_INPUT_STATE_NONE ? value : (tb_input_value){0};
    if (def->type == TB_INPUT_VALUE_BOOL)
    {
        status->value.x = status->value.x != 0.0f ? 1.0f : 0.0f;
    }
    status->elapsed_seconds =
        state != TB_INPUT_STATE_NONE ? status->elapsed_seconds + seconds : 0.0;
    status->triggered_seconds =
        state == TB_INPUT_STATE_TRIGGERED ? status->triggered_seconds + seconds : 0.0;
    if (state == TB_INPUT_STATE_TRIGGERED)
    {
        status->last_triggered_at = player->input->seconds;
    }
}

static void tb_input_evaluate_action(tb_input_player *player, size_t action, double seconds)
{
    const tb_input_action_def *const def = &player->input->actions[action];
    tb_input_action_runtime *const runtime = &player->actions[action];
    tb_input_mapped mapped = {0};
    tb_input_tracker tracker = TB_INPUT_TRACKER_INIT;

    for (size_t index = 0; index < player->entry_count; index++)
    {
        const tb_input_entry *const entry = &player->entries[index];
        const tb_input_context_def *const context =
            tb_input_context_def_of(player->input, entry->context);
        for (size_t mapping = 0; mapping < context->mapping_count; mapping++)
        {
            if (entry->active[mapping] && context->mappings[mapping].action.id == action + 1)
            {
                tb_input_evaluate_mapping(player, entry, context, mapping, seconds, &mapped);
            }
        }
    }
    tb_input_value value =
        tb_input_apply_modifiers(player, &def->rules, runtime->states, mapped.value);
    value = tb_input_truncated(value, def->type);
    tb_input_apply_triggers(player, &def->rules, runtime->states, value, seconds, &tracker);
    /* An action without triggers of its own follows its mappings'. */
    tb_input_trigger_state state = def->rules.trigger_count == 0 && mapped.applied
                                       ? TB_INPUT_STATE_TRIGGERED
                                       : tb_input_tracker_state(&tracker, value);
    /* Both the mappings' triggers and the action's must fire. */
    if (mapped.applied && mapped.state < state)
    {
        state = mapped.state;
    }
    tb_input_settle(player, runtime, def, state, value, seconds);
}

static void tb_input_compact_bindings(tb_input_player *player)
{
    size_t kept = 0;

    for (size_t index = 0; index < player->binding_count; index++)
    {
        if (!player->bindings[index].removed)
        {
            player->bindings[kept] = player->bindings[index];
            kept++;
        }
    }
    player->binding_count = kept;
}

static void tb_input_dispatch(tb_input_player *player)
{
    static const tb_input_event order[] = {TB_INPUT_EVENT_STARTED, TB_INPUT_EVENT_ONGOING,
                                           TB_INPUT_EVENT_TRIGGERED, TB_INPUT_EVENT_COMPLETED,
                                           TB_INPUT_EVENT_CANCELED};
    const size_t count = player->binding_count;

    player->dispatching = true;
    for (size_t index = 0; index < count; index++)
    {
        for (size_t event = 0; event < sizeof(order) / sizeof(order[0]); event++)
        {
            /* Read again for each call: a callback may unbind it, or bind
             * others, which may move the array. */
            const tb_input_bound bound = player->bindings[index];
            const tb_input_action_status *const status =
                &player->actions[bound.action.id - 1].status;
            if (bound.removed)
            {
                break;
            }
            if ((bound.events & status->events & (uint32_t)order[event]) == 0)
            {
                continue;
            }
            const tb_input_action_event happened = {
                .player = player, .action = bound.action, .event = order[event], .status = *status};
            bound.callback(bound.user, &happened);
        }
    }
    player->dispatching = false;
    tb_input_compact_bindings(player);
}

/* ---------------------------------------------------------------------------
 * Listening.
 * ------------------------------------------------------------------------- */

/* A key back below this part of a listener's threshold is at rest. */
#define TB_INPUT_LISTEN_REST 0.5f

static const tb_input_key tb_input_default_cancel_keys[] = {TB_INPUT_KEY_ESCAPE,
                                                            TB_INPUT_KEY_GAMEPAD_START};

/* Whether a player's keys count for the listener this tick. */
static bool tb_input_listens_to(const tb_input *input, size_t player)
{
    const tb_input_listener *const listener = &input->listener;
    return input->listening_tick &&
           (listener->player == TB_INPUT_NO_PLAYER || listener->player == player);
}

/* Whether a key is away from rest: a button down, an axis past half the
 * threshold. The mouse's motion and the wheel are always at rest, since
 * they start from zero each tick. */
static bool tb_input_listen_away(const tb_input_listener *listener, tb_input_key key,
                                 tb_input_value value)
{
    switch (tb_input_key_kind_of(key))
    {
    case TB_INPUT_KEY_KIND_BUTTON:
        return value.x != 0.0f;
    case TB_INPUT_KEY_KIND_AXIS:
        return fabsf(value.x) >= listener->threshold * TB_INPUT_LISTEN_REST;
    case TB_INPUT_KEY_KIND_RELATIVE:
    case TB_INPUT_KEY_KIND_COMPOSITE:
    case TB_INPUT_KEY_KIND_POSITION:
        break;
    }
    return false;
}

/* Whether a key was pressed or moved far enough this tick to be captured. */
static bool tb_input_listen_moved(const tb_input_listener *listener, size_t player,
                                  tb_input_key key, tb_input_value value)
{
    switch (tb_input_key_kind_of(key))
    {
    case TB_INPUT_KEY_KIND_BUTTON:
        return value.x != 0.0f;
    case TB_INPUT_KEY_KIND_AXIS:
        return fabsf(value.x) >= listener->threshold;
    case TB_INPUT_KEY_KIND_RELATIVE:
        if (key == TB_INPUT_KEY_MOUSE_X || key == TB_INPUT_KEY_MOUSE_Y)
        {
            return listener->travel[player][key == TB_INPUT_KEY_MOUSE_Y ? 1 : 0] >=
                   listener->distance;
        }
        return value.x > 0.0f || (key == TB_INPUT_KEY_MOUSE_WHEEL && value.x != 0.0f);
    case TB_INPUT_KEY_KIND_COMPOSITE:
    case TB_INPUT_KEY_KIND_POSITION:
        break;
    }
    return false;
}

static bool tb_input_is_cancel_key(const tb_input_listener *listener, tb_input_key key)
{
    for (size_t index = 0; index < listener->cancel_key_count; index++)
    {
        if (listener->cancel_keys[index] == key)
        {
            return true;
        }
    }
    return false;
}

/* The key a listener captures for a key moved: the key itself, its 2D key
 * for a 2D listener, or none when it does not take it. */
static tb_input_key tb_input_listen_take(const tb_input_listener *listener, tb_input_key key)
{
    const bool made = tb_input_key_source(key) != TB_INPUT_KEY_NONE;
    const tb_input_key_kind kind = tb_input_key_kind_of(key);

    if ((tb_input_key_class_of(key) & listener->classes) == 0)
    {
        return TB_INPUT_KEY_NONE;
    }
    if (listener->type == TB_INPUT_VALUE_BOOL)
    {
        /* A button, or a button made from an axis; not the axis itself. */
        return made || kind == TB_INPUT_KEY_KIND_BUTTON ? key : TB_INPUT_KEY_NONE;
    }
    if (made)
    {
        /* An axis listener takes the axis, not the buttons made of it. */
        return TB_INPUT_KEY_NONE;
    }
    if (listener->type == TB_INPUT_VALUE_AXIS1D)
    {
        return key;
    }
    if (key >= TB_INPUT_KEY_TOUCH_1 && key <= TB_INPUT_KEY_TOUCH_10)
    {
        return (tb_input_key)(TB_INPUT_KEY_TOUCH_1_POSITION + (key - TB_INPUT_KEY_TOUCH_1));
    }
    const tb_input_key composite = tb_input_key_composite_of(key);
    return composite != TB_INPUT_KEY_NONE ? composite : key;
}

/* End listening with a key of a player's. */
static void tb_input_listen_end(tb_input_player *player, tb_input_listen_state state,
                                tb_input_key key, tb_input_device device)
{
    tb_input_listener *const listener = &player->input->listener;

    listener->capture =
        (tb_input_capture){.state = state, .key = key, .device = device, .player = player->index};
}

/* Look at a player's keys for the listener: forget the keys held when it
 * began once they are at rest, and capture or cancel on a key moved. */
static void tb_input_listen_look(tb_input_player *player)
{
    tb_input_listener *const listener = &player->input->listener;
    bool *const held = listener->held[player->index];
    tb_input_key moved[TB_INPUT_KEY_COUNT] = {0};
    size_t moved_count = 0;

    listener->travel[player->index][0] += fabsf(player->tick_keys[TB_INPUT_KEY_MOUSE_X].x);
    listener->travel[player->index][1] += fabsf(player->tick_keys[TB_INPUT_KEY_MOUSE_Y].x);
    for (size_t index = 1; index < TB_INPUT_KEY_COUNT; index++)
    {
        const tb_input_key key = (tb_input_key)index;
        const tb_input_value value = player->tick_keys[key];
        if (held[key])
        {
            held[key] = tb_input_listen_away(listener, key, value);
            continue;
        }
        if (tb_input_listen_moved(listener, player->index, key, value))
        {
            moved[moved_count] = key;
            moved_count++;
        }
    }
    for (size_t index = 0; index < moved_count; index++)
    {
        if (tb_input_is_cancel_key(listener, moved[index]))
        {
            tb_input_listen_end(player, TB_INPUT_LISTEN_CANCELED, TB_INPUT_KEY_NONE,
                                player->keys[moved[index]].device);
            return;
        }
    }
    for (size_t index = 0; index < moved_count; index++)
    {
        const tb_input_key taken = tb_input_listen_take(listener, moved[index]);
        if (taken != TB_INPUT_KEY_NONE)
        {
            tb_input_listen_end(player, TB_INPUT_LISTEN_CAPTURED, taken,
                                player->keys[moved[index]].device);
            return;
        }
    }
}

/* Every mapping of a player's whose key is a button held waits for it to be
 * released: after listening, so that the key captured does not act. */
static void tb_input_block_held(tb_input_player *player)
{
    for (size_t index = 0; index < player->entry_count; index++)
    {
        tb_input_entry *const entry = &player->entries[index];
        for (size_t mapping = 0; mapping < entry->mapping_count; mapping++)
        {
            entry->blocked[mapping] =
                entry->blocked[mapping] || tb_input_button_held(player, entry->keys[mapping]);
        }
    }
}

/* The listener's look at a player's keys this tick; whether the player is
 * listened to, so that its actions see no keys. */
static bool tb_input_listen_tick(tb_input_player *player)
{
    tb_input *const input = player->input;

    if (!tb_input_listens_to(input, player->index))
    {
        return false;
    }
    if (input->listener.capture.state == TB_INPUT_LISTEN_WAITING)
    {
        tb_input_listen_look(player);
    }
    return true;
}

static void tb_input_player_tick(tb_input_player *player, double seconds)
{
    tb_input_take_keys(player);
    const bool listened = tb_input_listen_tick(player);
    tb_input_choose_mappings(player, listened);
    /* Actions that wait on others go after them, so that a chord sees the
     * other action's state of this tick. */
    for (size_t pass = 0; pass < 2; pass++)
    {
        for (size_t action = 0; action < player->input->action_count; action++)
        {
            if (player->actions[action].chorded == (pass == 1))
            {
                tb_input_evaluate_action(player, action, seconds);
            }
        }
    }
    tb_input_spend_keys(player);
}

static void tb_input_tick_with(tb_input *input, double seconds)
{
    input->ticks++;
    input->seconds += seconds;
    input->listening_tick = input->listener.capture.state == TB_INPUT_LISTEN_WAITING;
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_player_tick(&input->players[index], seconds);
    }
    /* Listening ended: keys held wait to be released before they act. */
    for (size_t index = 0; index < input->player_count; index++)
    {
        if (tb_input_listens_to(input, index) &&
            input->listener.capture.state != TB_INPUT_LISTEN_WAITING)
        {
            tb_input_block_held(&input->players[index]);
        }
    }
    input->listening_tick = false;
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_dispatch(&input->players[index]);
    }
}

static void tb_input_record_tick(tb_input *input, double seconds)
{
    tb_input_recording *const recording = input->recording;

    if (recording == NULL)
    {
        return;
    }
    recording->ticks = tb_input_grow(recording->ticks, &recording->tick_capacity,
                                     recording->tick_count, sizeof(*recording->ticks));
    recording->ticks[recording->tick_count] =
        (tb_input_recorded_tick){.seconds = seconds,
                                 .settings = input->settings,
                                 .first = input->recording_first,
                                 .count = recording->event_count - input->recording_first};
    recording->tick_count++;
    input->recording_first = recording->event_count;
}

void tb_input_tick(tb_input *input, double seconds)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(isfinite(seconds) && seconds >= 0.0, "a tick's length: finite, not negative");
    if (input->engine != NULL)
    {
        tb_input_read_engine_settings(input);
    }
    tb_input_record_tick(input, seconds);
    tb_input_tick_with(input, seconds);
}

uint64_t tb_input_tick_count(const tb_input *input)
{
    TB_ASSERT(input != NULL);
    return input->ticks;
}

double tb_input_seconds(const tb_input *input)
{
    TB_ASSERT(input != NULL);
    return input->seconds;
}

tb_input_value tb_input_key_value(const tb_input_player *player, tb_input_key key)
{
    TB_ASSERT(player != NULL);
    TB_ASSERT_MSG(key > TB_INPUT_KEY_NONE && key < TB_INPUT_KEY_COUNT, "a key");
    return player->tick_keys[key];
}

/* ---------------------------------------------------------------------------
 * Polling and callbacks.
 * ------------------------------------------------------------------------- */

static const tb_input_action_status *tb_input_status_of(const tb_input_player *player,
                                                        tb_input_action action)
{
    TB_ASSERT(player != NULL);
    TB_ASSERT_MSG(tb_input_action_valid(player->input, action), "an action of the input");
    return &player->actions[action.id - 1].status;
}

void tb_input_get_action_status(const tb_input_player *player, tb_input_action action,
                                tb_input_action_status *status)
{
    TB_ASSERT(status != NULL);
    *status = *tb_input_status_of(player, action);
}

tb_input_value tb_input_action_value(const tb_input_player *player, tb_input_action action)
{
    return tb_input_status_of(player, action)->value;
}

bool tb_input_action_active(const tb_input_player *player, tb_input_action action)
{
    return !tb_input_value_is_zero(tb_input_status_of(player, action)->value);
}

bool tb_input_action_triggered(const tb_input_player *player, tb_input_action action)
{
    return (tb_input_status_of(player, action)->events & TB_INPUT_EVENT_TRIGGERED) != 0;
}

uint32_t tb_input_action_events(const tb_input_player *player, tb_input_action action)
{
    return tb_input_status_of(player, action)->events;
}

tb_input_trigger_state tb_input_action_state(const tb_input_player *player, tb_input_action action)
{
    return tb_input_status_of(player, action)->state;
}

tb_input_binding tb_input_bind(tb_input_player *player, tb_input_action action, uint32_t events,
                               tb_input_callback *callback, void *user)
{
    TB_ASSERT(player != NULL);
    TB_ASSERT_MSG(tb_input_action_valid(player->input, action), "an action of the input");
    TB_ASSERT_MSG(events != 0 && (events & ~(uint32_t)TB_INPUT_EVENT_ALL) == 0, "events to bind");
    TB_ASSERT(callback != NULL);
    player->bindings = tb_input_grow(player->bindings, &player->binding_capacity,
                                     player->binding_count, sizeof(*player->bindings));
    player->next_binding++;
    player->bindings[player->binding_count] = (tb_input_bound){.id = player->next_binding,
                                                               .action = action,
                                                               .events = events,
                                                               .callback = callback,
                                                               .user = user};
    player->binding_count++;
    return (tb_input_binding){player->next_binding};
}

void tb_input_unbind(tb_input_player *player, tb_input_binding binding)
{
    TB_ASSERT(player != NULL);
    for (size_t index = 0; index < player->binding_count; index++)
    {
        if (player->bindings[index].id == binding.id && binding.id != 0)
        {
            player->bindings[index].removed = true;
        }
    }
    if (!player->dispatching)
    {
        tb_input_compact_bindings(player);
    }
}

/* ---------------------------------------------------------------------------
 * Recording and replaying.
 * ------------------------------------------------------------------------- */

tb_input_recording *tb_input_recording_create(void)
{
    return tb_calloc(1, sizeof(tb_input_recording));
}

void tb_input_recording_destroy(tb_input_recording *recording)
{
    if (recording == NULL)
    {
        return;
    }
    tb_free(recording->ticks);
    tb_free(recording->events);
    tb_free(recording);
}

size_t tb_input_recording_tick_count(const tb_input_recording *recording)
{
    TB_ASSERT(recording != NULL);
    return recording->tick_count;
}

void tb_input_record(tb_input *input, tb_input_recording *recording)
{
    TB_ASSERT(input != NULL);
    input->recording = recording;
    input->recording_first = recording != NULL ? recording->event_count : 0;
}

void tb_input_replay_tick(tb_input *input, const tb_input_recording *recording, size_t tick)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(recording != NULL);
    TB_ASSERT_MSG(input->recording == NULL, "an input that is not recording");
    TB_ASSERT_MSG(tick < recording->tick_count, "a tick of the recording");
    const tb_input_recorded_tick *const recorded = &recording->ticks[tick];
    for (size_t index = 0; index < recorded->count; index++)
    {
        const tb_input_fed *const fed = &recording->events[recorded->first + index];
        TB_ASSERT_MSG(fed->player < input->player_count, "a player the recording's input had");
        tb_input_apply(&input->players[fed->player], TB_INPUT_DEVICE_NONE, fed->key, fed->value);
    }
    input->settings = recorded->settings;
    tb_input_tick_with(input, recorded->seconds);
}

/* ---------------------------------------------------------------------------
 * For the file reader.
 * ------------------------------------------------------------------------- */

void tb_input_replace_action_rules(tb_input *input, tb_input_action action,
                                   const tb_input_modifier *modifiers, size_t modifier_count,
                                   const tb_input_trigger *triggers, size_t trigger_count)
{
    TB_ASSERT_MSG(tb_input_check_rules(input, action, modifiers, modifier_count, triggers,
                                       trigger_count) == NULL,
                  "valid rules");
    tb_input_action_def *const def = &input->actions[action.id - 1];
    tb_input_rules_free(&def->rules);
    tb_input_rules_copy(&def->rules, modifiers, modifier_count, triggers, trigger_count);
    const size_t states = tb_input_rules_state_count(&def->rules);
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_action_runtime *const runtime = &input->players[index].actions[action.id - 1];
        tb_free(runtime->states);
        runtime->states = states > 0 ? tb_calloc(states, sizeof(tb_input_rule_state)) : NULL;
    }
}

void tb_input_rollback(tb_input *input, size_t action_count, size_t context_count)
{
    TB_ASSERT(action_count <= input->action_count && context_count <= input->context_count);
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_player *const player = &input->players[index];
        for (size_t action = action_count; action < input->action_count; action++)
        {
            tb_free(player->actions[action].states);
        }
    }
    for (size_t index = action_count; index < input->action_count; index++)
    {
        tb_input_rules_free(&input->actions[index].rules);
        tb_free(input->actions[index].name);
    }
    for (size_t index = context_count; index < input->context_count; index++)
    {
        tb_input_context_free(&input->contexts[index]);
    }
    input->action_count = action_count;
    input->context_count = context_count;
    for (size_t index = 0; index < input->player_count; index++)
    {
        tb_input_player *const player = &input->players[index];
        size_t kept = 0;
        for (size_t rebind = 0; rebind < player->rebind_count; rebind++)
        {
            if (player->rebinds[rebind].context.id <= context_count)
            {
                player->rebinds[kept] = player->rebinds[rebind];
                kept++;
            }
        }
        player->rebind_count = kept;
    }
}

/* ---------------------------------------------------------------------------
 * Rebinding.
 * ------------------------------------------------------------------------- */

void tb_input_context_mapping_at(const tb_input *input, tb_input_context context, size_t mapping,
                                 tb_input_mapping_info *info)
{
    TB_ASSERT(input != NULL);
    TB_ASSERT(info != NULL);
    const tb_input_context_def *const def = tb_input_context_def_of(input, context);
    TB_ASSERT_MSG(mapping < def->mapping_count, "a mapping of the context");
    *info = (tb_input_mapping_info){.action = def->mappings[mapping].action,
                                    .key = def->mappings[mapping].key,
                                    .pass_through = def->mappings[mapping].pass_through};
}

static const tb_input_mapping_def *tb_input_mapping_of(const tb_input *input,
                                                       tb_input_context context, size_t mapping)
{
    const tb_input_context_def *const def = tb_input_context_def_of(input, context);
    TB_ASSERT_MSG(mapping < def->mapping_count, "a mapping of the context");
    return &def->mappings[mapping];
}

/* Where a player's rebind of a mapping is, or would go: rebinds are kept
 * in the order of contexts, then of mappings. */
static size_t tb_input_rebind_place(const tb_input_player *player, tb_input_context context,
                                    size_t mapping, bool *found)
{
    size_t index = 0;

    while (index < player->rebind_count)
    {
        const tb_input_rebound *const rebind = &player->rebinds[index];
        if (rebind->context.id > context.id ||
            (rebind->context.id == context.id && rebind->mapping >= mapping))
        {
            break;
        }
        index++;
    }
    *found = index < player->rebind_count && player->rebinds[index].context.id == context.id &&
             player->rebinds[index].mapping == mapping;
    return index;
}

tb_input_key tb_input_binding_key(const tb_input_player *player, tb_input_context context,
                                  size_t mapping)
{
    bool found = false;

    TB_ASSERT(player != NULL);
    const tb_input_mapping_def *const def = tb_input_mapping_of(player->input, context, mapping);
    const size_t index = tb_input_rebind_place(player, context, mapping, &found);
    return found ? player->rebinds[index].key : def->key;
}

bool tb_input_is_rebound(const tb_input_player *player, tb_input_context context, size_t mapping)
{
    bool found = false;

    TB_ASSERT(player != NULL);
    (void)tb_input_mapping_of(player->input, context, mapping);
    (void)tb_input_rebind_place(player, context, mapping, &found);
    return found;
}

/* A player's contexts with a mapping start over, with its key as bound. */
static void tb_input_rebuild_context(tb_input_player *player, tb_input_context context)
{
    for (size_t entry = 0; entry < player->entry_count; entry++)
    {
        if (player->entries[entry].context.id == context.id)
        {
            tb_input_entry_build(player, &player->entries[entry]);
        }
    }
}

void tb_input_rebind(tb_input_player *player, tb_input_context context, size_t mapping,
                     tb_input_key key)
{
    bool found = false;

    TB_ASSERT(player != NULL);
    TB_ASSERT_MSG((unsigned)key < (unsigned)TB_INPUT_KEY_COUNT, "a key, or none");
    const tb_input_mapping_def *const def = tb_input_mapping_of(player->input, context, mapping);
    const size_t index = tb_input_rebind_place(player, context, mapping, &found);
    if (key == def->key)
    {
        tb_input_reset_binding(player, context, mapping);
        return;
    }
    if (!found)
    {
        player->rebinds = tb_input_grow(player->rebinds, &player->rebind_capacity,
                                        player->rebind_count, sizeof(*player->rebinds));
        memmove(&player->rebinds[index + 1], &player->rebinds[index],
                (player->rebind_count - index) * sizeof(*player->rebinds));
        player->rebind_count++;
    }
    player->rebinds[index] = (tb_input_rebound){.context = context, .mapping = mapping, .key = key};
    tb_input_rebuild_context(player, context);
}

void tb_input_reset_binding(tb_input_player *player, tb_input_context context, size_t mapping)
{
    bool found = false;

    TB_ASSERT(player != NULL);
    (void)tb_input_mapping_of(player->input, context, mapping);
    const size_t index = tb_input_rebind_place(player, context, mapping, &found);
    if (!found)
    {
        return;
    }
    memmove(&player->rebinds[index], &player->rebinds[index + 1],
            (player->rebind_count - index - 1) * sizeof(*player->rebinds));
    player->rebind_count--;
    tb_input_rebuild_context(player, context);
}

void tb_input_reset_bindings(tb_input_player *player)
{
    TB_ASSERT(player != NULL);
    if (player->rebind_count == 0)
    {
        return;
    }
    player->rebind_count = 0;
    for (size_t entry = 0; entry < player->entry_count; entry++)
    {
        tb_input_entry_build(player, &player->entries[entry]);
    }
}

size_t tb_input_rebind_count(const tb_input_player *player)
{
    TB_ASSERT(player != NULL);
    return player->rebind_count;
}

/* The values a key's value is made of: its own, its X's and Y's for a 2D
 * key, and the axis's for a button made from one. */
static size_t tb_input_key_roots(tb_input_key key, tb_input_key roots[2])
{
    const tb_input_key source = tb_input_key_source(key);

    if (source != TB_INPUT_KEY_NONE)
    {
        roots[0] = source;
        return 1;
    }
    if (tb_input_key_kind_of(key) == TB_INPUT_KEY_KIND_COMPOSITE)
    {
        tb_input_key_components(key, &roots[0], &roots[1]);
        return 2;
    }
    roots[0] = key;
    return 1;
}

static bool tb_input_keys_collide(tb_input_key left, tb_input_key right)
{
    tb_input_key left_roots[2] = {TB_INPUT_KEY_NONE, TB_INPUT_KEY_NONE};
    tb_input_key right_roots[2] = {TB_INPUT_KEY_NONE, TB_INPUT_KEY_NONE};

    if (left == TB_INPUT_KEY_NONE || right == TB_INPUT_KEY_NONE)
    {
        return false;
    }
    const size_t left_count = tb_input_key_roots(left, left_roots);
    const size_t right_count = tb_input_key_roots(right, right_roots);
    for (size_t index = 0; index < left_count; index++)
    {
        for (size_t other = 0; other < right_count; other++)
        {
            if (left_roots[index] == right_roots[other])
            {
                return true;
            }
        }
    }
    return false;
}

size_t tb_input_find_conflicts(const tb_input_player *player, tb_input_context context,
                               size_t mapping, tb_input_key key, size_t *conflicts, size_t capacity)
{
    size_t count = 0;

    TB_ASSERT(player != NULL);
    TB_ASSERT(conflicts != NULL || capacity == 0);
    TB_ASSERT_MSG((unsigned)key < (unsigned)TB_INPUT_KEY_COUNT, "a key, or none");
    const tb_input_context_def *const def = tb_input_context_def_of(player->input, context);
    TB_ASSERT_MSG(mapping == TB_INPUT_NO_MAPPING || mapping < def->mapping_count,
                  "a mapping of the context, or none");
    for (size_t index = 0; index < def->mapping_count; index++)
    {
        if (index == mapping ||
            !tb_input_keys_collide(key, tb_input_binding_key(player, context, index)))
        {
            continue;
        }
        if (count < capacity)
        {
            conflicts[count] = index;
        }
        count++;
    }
    return count;
}

tb_input_key tb_input_find_binding(const tb_input_player *player, tb_input_context context,
                                   tb_input_action action, uint32_t classes)
{
    const uint32_t wanted = classes != 0 ? classes : TB_INPUT_CLASS_ALL;

    TB_ASSERT(player != NULL);
    TB_ASSERT_MSG(tb_input_action_valid(player->input, action), "an action of the input");
    const tb_input_context_def *const def = tb_input_context_def_of(player->input, context);
    for (size_t index = 0; index < def->mapping_count; index++)
    {
        const tb_input_key key = tb_input_binding_key(player, context, index);
        if (def->mappings[index].action.id == action.id &&
            (tb_input_key_class_of(key) & wanted) != 0)
        {
            return key;
        }
    }
    return TB_INPUT_KEY_NONE;
}

tb_input_device tb_input_player_last_device(const tb_input_player *player)
{
    TB_ASSERT(player != NULL);
    return player->last_device;
}

/* ---------------------------------------------------------------------------
 * Listening, the API.
 * ------------------------------------------------------------------------- */

static bool tb_input_listen_desc_valid(const tb_input *input, const tb_input_listen_desc *desc)
{
    const bool player_valid = desc->player == NULL || desc->player->input == input;
    const bool cancel_valid = desc->cancel_keys != NULL || desc->cancel_key_count == 0;
    bool keys_valid = true;

    for (size_t index = 0; cancel_valid && index < desc->cancel_key_count; index++)
    {
        keys_valid = keys_valid && desc->cancel_keys[index] > TB_INPUT_KEY_NONE &&
                     desc->cancel_keys[index] < TB_INPUT_KEY_COUNT;
    }
    return player_valid && cancel_valid && keys_valid &&
           (desc->classes & ~(uint32_t)TB_INPUT_CLASS_ALL) == 0 &&
           (unsigned)desc->type < (unsigned)TB_INPUT_VALUE_TYPE_COUNT && desc->threshold >= 0.0f &&
           desc->threshold <= 1.0f && isfinite(desc->distance) && desc->distance >= 0.0f;
}

void tb_input_listen(tb_input *input, const tb_input_listen_desc *desc)
{
    static const size_t default_cancel_count =
        sizeof(tb_input_default_cancel_keys) / sizeof(tb_input_default_cancel_keys[0]);

    TB_ASSERT(input != NULL);
    TB_ASSERT(desc != NULL);
    TB_ASSERT_MSG(tb_input_listen_desc_valid(input, desc), "a valid listener");
    tb_input_listener *const listener = &input->listener;
    *listener = (tb_input_listener){
        .capture = {.state = TB_INPUT_LISTEN_WAITING, .player = TB_INPUT_NO_PLAYER},
        .player = desc->player != NULL ? desc->player->index : TB_INPUT_NO_PLAYER,
        .classes = desc->classes != 0 ? desc->classes : TB_INPUT_CLASS_ALL,
        .type = desc->type,
        .threshold = desc->threshold > 0.0f ? desc->threshold : TB_INPUT_DEFAULT_THRESHOLD,
        .distance = desc->distance > 0.0f ? desc->distance : TB_INPUT_LISTEN_DISTANCE,
        .cancel_keys = desc->cancel_keys != NULL ? desc->cancel_keys : tb_input_default_cancel_keys,
        .cancel_key_count =
            desc->cancel_keys != NULL ? desc->cancel_key_count : default_cancel_count};
    /* What is down or away from rest now counts once it has been released. */
    for (size_t player = 0; player < input->player_count; player++)
    {
        const tb_input_player *const held = &input->players[player];
        for (size_t key = 1; key < TB_INPUT_KEY_COUNT; key++)
        {
            const tb_input_key_state *const state = &held->keys[key];
            listener->held[player][key] =
                state->pressed || tb_input_listen_away(listener, (tb_input_key)key, state->value);
        }
    }
}

void tb_input_stop_listening(tb_input *input)
{
    TB_ASSERT(input != NULL);
    input->listener.capture = (tb_input_capture){.player = TB_INPUT_NO_PLAYER};
}

tb_input_listen_state tb_input_listen_result(const tb_input *input, tb_input_capture *capture)
{
    TB_ASSERT(input != NULL);
    if (capture != NULL)
    {
        *capture = input->listener.capture;
    }
    return input->listener.capture.state;
}
