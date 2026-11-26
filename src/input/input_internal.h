/* The input module's own declarations: keys' kinds, the rules (modifiers
 * and triggers) and their state, and the input's definitions, which the
 * file reader and writer share with the evaluation. */
#ifndef TB_INPUT_INTERNAL_H
#define TB_INPUT_INTERNAL_H

#include "badger/input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Keys (input_keys.c).
 * ------------------------------------------------------------------------- */

/* How a key's value is gathered between ticks. */
typedef enum tb_input_key_kind
{
    TB_INPUT_KEY_KIND_BUTTON,    /* Down or up; a press within a tick is seen for one. */
    TB_INPUT_KEY_KIND_AXIS,      /* One axis; the value fed last. */
    TB_INPUT_KEY_KIND_RELATIVE,  /* One axis; the values fed in a tick, added. */
    TB_INPUT_KEY_KIND_COMPOSITE, /* Two axes, each another key's. */
    TB_INPUT_KEY_KIND_POSITION   /* Two axes; the value fed last. */
} tb_input_key_kind;

tb_input_key_kind tb_input_key_kind_of(tb_input_key key);

/* The keys of a composite key's X and Y. */
void tb_input_key_components(tb_input_key key, tb_input_key *x, tb_input_key *y);

/* Whether a key is the keyboard's or a mouse button: what losing the focus
 * releases. */
bool tb_input_key_is_window_button(tb_input_key key);

/* The buttons an axis makes, as its value is fed: a stick's two directions
 * along it, or a trigger's button, with `*negative` none; and the threshold
 * they press at. False for an axis that makes none. */
bool tb_input_key_virtual_buttons(tb_input_key axis, tb_input_key *positive, tb_input_key *negative,
                                  tb_gamepad_threshold *threshold);

/* The axis a button is made from: a stick's X or Y for its directions, a
 * trigger for its button, the wheel for its buttons; none for any other. */
tb_input_key tb_input_key_source(tb_input_key key);

/* The 2D key an axis is one of: a stick's, or the mouse's motion; none for
 * any other. */
tb_input_key tb_input_key_composite_of(tb_input_key axis);

/* ---------------------------------------------------------------------------
 * Rules: modifiers and triggers (input_rules.c).
 * ------------------------------------------------------------------------- */

/* Modifiers and triggers, owned copies. */
typedef struct tb_input_rules
{
    tb_input_modifier *modifiers;
    size_t modifier_count;
    tb_input_trigger *triggers;
    size_t trigger_count;
} tb_input_rules;

/* What a modifier or a trigger keeps from tick to tick, for one player. */
typedef struct tb_input_rule_state
{
    double held;           /* Triggers: how long the value has been actuated. */
    double next_pulse;     /* Pulse: when the next pulse is due, in `held`. */
    uint32_t pulses;       /* Pulse: pulses in this hold. */
    bool fired;            /* Hold: it triggered in this hold. */
    bool last_actuated;    /* Triggers: the value was actuated at the last tick. */
    uint32_t sample_count; /* Smoothing: samples held. */
    uint32_t sample_next;  /* Smoothing: where the next goes. */
    tb_input_value samples[TB_INPUT_MOST_SMOOTH_SAMPLES]; /* Smoothing. */
} tb_input_rule_state;

/* Why rules are broken, or NULL. A chord must name one of `input`'s
 * actions other than `self`. */
const char *tb_input_check_rules(const tb_input *input, tb_input_action self,
                                 const tb_input_modifier *modifiers, size_t modifier_count,
                                 const tb_input_trigger *triggers, size_t trigger_count);

void tb_input_rules_copy(tb_input_rules *rules, const tb_input_modifier *modifiers,
                         size_t modifier_count, const tb_input_trigger *triggers,
                         size_t trigger_count);
void tb_input_rules_free(tb_input_rules *rules);

/* States the rules need: one a modifier, one a trigger. */
size_t tb_input_rules_state_count(const tb_input_rules *rules);

/* Whether any trigger of the rules is a chord or a chord blocker. */
bool tb_input_rules_chorded(const tb_input_rules *rules);

/* A value through one modifier. */
tb_input_value tb_input_modify(const tb_input_modifier *modifier, tb_input_rule_state *state,
                               tb_input_value value, const tb_input_settings *settings);

/* A trigger's state for a tick of `seconds` with a value. `chord` is the
 * state of the trigger's chord action, for chords. */
tb_input_trigger_state tb_input_trigger_update(const tb_input_trigger *trigger,
                                               tb_input_rule_state *state, tb_input_value value,
                                               double seconds, tb_input_trigger_state chord);

/* Unreal's trigger state tracker: explicit triggers fire when any does,
 * implicit ones when all do, and blockers stop both. */
typedef struct tb_input_tracker
{
    bool found_explicit;
    bool any_explicit_triggered;
    bool all_implicit_triggered;
    bool any_ongoing;
    bool blocked;
    size_t count;
} tb_input_tracker;

#define TB_INPUT_TRACKER_INIT {false, false, true, false, false, 0}

void tb_input_tracker_add(tb_input_tracker *tracker, tb_input_trigger_type type,
                          tb_input_trigger_state state);

/* The state the triggers added come to, for a value; a tracker of no
 * triggers is triggered while the value is not zero. */
tb_input_trigger_state tb_input_tracker_state(const tb_input_tracker *tracker,
                                              tb_input_value value);

bool tb_input_value_is_zero(tb_input_value value);

/* ---------------------------------------------------------------------------
 * Definitions (input.c).
 * ------------------------------------------------------------------------- */

typedef struct tb_input_action_def
{
    char *name;
    tb_input_value_type type;
    tb_input_accumulation accumulation;
    tb_input_rules rules;
} tb_input_action_def;

typedef struct tb_input_mapping_def
{
    tb_input_action action;
    tb_input_key key;
    tb_input_rules rules;
    bool pass_through;
} tb_input_mapping_def;

typedef struct tb_input_action_rules_def
{
    tb_input_action action;
    tb_input_rules rules;
} tb_input_action_rules_def;

typedef struct tb_input_context_def
{
    char *name;
    tb_input_mapping_def *mappings;
    size_t mapping_count;
    size_t mapping_capacity;
    tb_input_action_rules_def *action_rules;
    size_t action_rules_count;
    size_t action_rules_capacity;
} tb_input_context_def;

/* The definitions of an input, for the file writer. */
const tb_input_action_def *tb_input_action_def_of(const tb_input *input, tb_input_action action);
const tb_input_context_def *tb_input_context_def_of(const tb_input *input,
                                                    tb_input_context context);

/* The input a player is of. */
tb_input *tb_input_player_input(const tb_input_player *player);

/* Whether a name is one an action or a context may have. */
bool tb_input_name_valid(const char *name);

/* Give an action other rules: for actions read from a file, whose chords
 * may name actions after them. */
void tb_input_replace_action_rules(tb_input *input, tb_input_action action,
                                   const tb_input_modifier *modifiers, size_t modifier_count,
                                   const tb_input_trigger *triggers, size_t trigger_count);

/* Forget the actions and contexts made after there were these many: a file
 * that failed. No player may have such a context. */
void tb_input_rollback(tb_input *input, size_t action_count, size_t context_count);

/* ---------------------------------------------------------------------------
 * Files (input_file.c), for every file the module reads.
 * ------------------------------------------------------------------------- */

/* Fill in an error's message from its line, column, path and reason, with
 * `detail` quoted after them when not NULL. */
void tb_input_error_message(tb_input_file_error *error, const char *file_name, const char *detail);

/* Fail with a result and a reason of no place in the text; returns `result`. */
tb_input_file_result tb_input_fail_plain(tb_input_file_error *error, tb_input_file_result result,
                                         const char *name, const char *reason);

/* Read a file through a VFS into `*text`, which the caller frees: OK,
 * NOT_FOUND or INPUT_FAILED, with the error filled in. */
tb_input_file_result tb_input_read_vfs_file(struct tb_vfs *vfs, const char *path,
                                            tb_input_file_error *error, char **text,
                                            size_t *length);

#endif
