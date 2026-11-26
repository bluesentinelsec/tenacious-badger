/* Rules: what modifiers do to a value, when triggers fire, and how an
 * action's triggers come to one state, after Unreal's Enhanced Input
 * (UInputModifier*, UInputTrigger* and FTriggerStateTracker). */
#include "input_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

/* Unreal's defaults, for fields left 0. */
#define TB_INPUT_DEFAULT_DEAD_ZONE_LOWER 0.2f
#define TB_INPUT_DEFAULT_DEAD_ZONE_UPPER 1.0f
#define TB_INPUT_DEFAULT_SMOOTH_SAMPLES 4u
#define TB_INPUT_DEFAULT_HOLD_SECONDS 1.0
#define TB_INPUT_DEFAULT_TAP_SECONDS 0.2
#define TB_INPUT_DEFAULT_PULSE_SECONDS 1.0
/* Mouse smoothing, when the player turns it on: the mean of two ticks. */
#define TB_INPUT_MOUSE_SMOOTH_SAMPLES 2u
/* Every axis, for a negation that names none. */
#define TB_INPUT_ALL_AXES (TB_INPUT_AXIS_X | TB_INPUT_AXIS_Y | TB_INPUT_AXIS_Z)

bool tb_input_value_is_zero(tb_input_value value)
{
    return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f;
}

static float tb_input_length_squared(tb_input_value value)
{
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

static tb_input_value tb_input_scaled(tb_input_value value, float factor)
{
    return (tb_input_value){value.x * factor, value.y * factor, value.z * factor};
}

/* ---------------------------------------------------------------------------
 * Checks and copies.
 * ------------------------------------------------------------------------- */

static bool tb_input_finite(float value)
{
    return isfinite(value) != 0;
}

static bool tb_input_value_finite(tb_input_value value)
{
    return tb_input_finite(value.x) && tb_input_finite(value.y) && tb_input_finite(value.z);
}

static float tb_input_lower(const tb_input_modifier *modifier)
{
    return modifier->lower > 0.0f ? modifier->lower : TB_INPUT_DEFAULT_DEAD_ZONE_LOWER;
}

static float tb_input_upper(const tb_input_modifier *modifier)
{
    return modifier->upper > 0.0f ? modifier->upper : TB_INPUT_DEFAULT_DEAD_ZONE_UPPER;
}

static const char *tb_input_check_dead_zone(const tb_input_modifier *modifier)
{
    if ((unsigned)modifier->dead_zone >= (unsigned)TB_INPUT_DEAD_ZONE_COUNT)
    {
        return "a dead zone of an unknown kind";
    }
    if (!tb_input_finite(modifier->lower) || modifier->lower < 0.0f || modifier->lower >= 1.0f)
    {
        return "a dead zone's lower threshold: from 0 to below 1";
    }
    if (!tb_input_finite(modifier->upper) || modifier->upper < 0.0f || modifier->upper > 1.0f)
    {
        return "a dead zone's upper threshold: from 0 to 1";
    }
    if (tb_input_upper(modifier) <= tb_input_lower(modifier))
    {
        return "a dead zone whose upper threshold is not above its lower";
    }
    return NULL;
}

static const char *tb_input_check_modifier(const tb_input_modifier *modifier)
{
    switch (modifier->type)
    {
    case TB_INPUT_MODIFIER_DEAD_ZONE:
        return tb_input_check_dead_zone(modifier);
    case TB_INPUT_MODIFIER_NEGATE:
        return (modifier->axes & ~(uint32_t)TB_INPUT_ALL_AXES) == 0
                   ? NULL
                   : "a negation of axes other than x, y and z";
    case TB_INPUT_MODIFIER_SWIZZLE:
        return (unsigned)modifier->order < (unsigned)TB_INPUT_SWIZZLE_COUNT
                   ? NULL
                   : "a swizzle of an unknown order";
    case TB_INPUT_MODIFIER_SCALAR:
        return tb_input_value_finite(modifier->scale) ? NULL : "a scale: finite numbers";
    case TB_INPUT_MODIFIER_RESPONSE_CURVE:
        return tb_input_value_finite(modifier->exponent) && modifier->exponent.x >= 0.0f &&
                       modifier->exponent.y >= 0.0f && modifier->exponent.z >= 0.0f
                   ? NULL
                   : "an exponent: finite numbers, not negative";
    case TB_INPUT_MODIFIER_SMOOTH:
        return modifier->samples <= TB_INPUT_MOST_SMOOTH_SAMPLES
                   ? NULL
                   : "a smoothing's samples: from 1 to 8";
    case TB_INPUT_MODIFIER_SETTINGS:
        return (unsigned)modifier->source < (unsigned)TB_INPUT_SETTINGS_SOURCE_COUNT
                   ? NULL
                   : "settings of an unknown source";
    case TB_INPUT_MODIFIER_TYPE_COUNT:
        break;
    }
    return "a modifier of an unknown type";
}

static bool tb_input_trigger_has_chord(tb_input_trigger_type type)
{
    return type == TB_INPUT_TRIGGER_CHORDED || type == TB_INPUT_TRIGGER_CHORD_BLOCKER;
}

static const char *tb_input_check_trigger(const tb_input *input, tb_input_action self,
                                          const tb_input_trigger *trigger)
{
    if ((unsigned)trigger->type >= (unsigned)TB_INPUT_TRIGGER_TYPE_COUNT)
    {
        return "a trigger of an unknown type";
    }
    if (!tb_input_finite(trigger->threshold) || trigger->threshold < 0.0f)
    {
        return "a threshold: a finite number, not negative";
    }
    if (isfinite(trigger->seconds) == 0 || trigger->seconds < 0.0)
    {
        return "seconds: a finite number, not negative";
    }
    if (!tb_input_trigger_has_chord(trigger->type))
    {
        return NULL;
    }
    if (trigger->chord.id == 0 || trigger->chord.id > tb_input_action_count(input))
    {
        return "a chord of no action";
    }
    if (trigger->chord.id == self.id)
    {
        return "a chord of its own action";
    }
    return NULL;
}

const char *tb_input_check_rules(const tb_input *input, tb_input_action self,
                                 const tb_input_modifier *modifiers, size_t modifier_count,
                                 const tb_input_trigger *triggers, size_t trigger_count)
{
    if ((modifiers == NULL && modifier_count > 0) || (triggers == NULL && trigger_count > 0))
    {
        return "a count of rules without the rules";
    }
    for (size_t index = 0; index < modifier_count; index++)
    {
        const char *const reason = tb_input_check_modifier(&modifiers[index]);
        if (reason != NULL)
        {
            return reason;
        }
    }
    for (size_t index = 0; index < trigger_count; index++)
    {
        const char *const reason = tb_input_check_trigger(input, self, &triggers[index]);
        if (reason != NULL)
        {
            return reason;
        }
    }
    return NULL;
}

void tb_input_rules_copy(tb_input_rules *rules, const tb_input_modifier *modifiers,
                         size_t modifier_count, const tb_input_trigger *triggers,
                         size_t trigger_count)
{
    *rules = (tb_input_rules){0};
    if (modifier_count > 0)
    {
        rules->modifiers = tb_realloc_array(NULL, modifier_count, sizeof(*modifiers));
        memcpy(rules->modifiers, modifiers, modifier_count * sizeof(*modifiers));
        rules->modifier_count = modifier_count;
    }
    if (trigger_count > 0)
    {
        rules->triggers = tb_realloc_array(NULL, trigger_count, sizeof(*triggers));
        memcpy(rules->triggers, triggers, trigger_count * sizeof(*triggers));
        rules->trigger_count = trigger_count;
    }
}

void tb_input_rules_free(tb_input_rules *rules)
{
    tb_free(rules->modifiers);
    tb_free(rules->triggers);
    *rules = (tb_input_rules){0};
}

size_t tb_input_rules_state_count(const tb_input_rules *rules)
{
    return rules->modifier_count + rules->trigger_count;
}

bool tb_input_rules_chorded(const tb_input_rules *rules)
{
    for (size_t index = 0; index < rules->trigger_count; index++)
    {
        if (tb_input_trigger_has_chord(rules->triggers[index].type))
        {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Modifiers.
 * ------------------------------------------------------------------------- */

/* A length past `lower`, as a part of the way to `upper`, from 0 to 1. */
static float tb_input_dead_zone_part(float length, float lower, float upper)
{
    const float part = (length - lower) / (upper - lower);

    if (part <= 0.0f)
    {
        return 0.0f;
    }
    return part < 1.0f ? part : 1.0f;
}

static float tb_input_axial(float value, float lower, float upper)
{
    const float part = tb_input_dead_zone_part(fabsf(value), lower, upper);
    return value < 0.0f ? -part : part;
}

static tb_input_value tb_input_dead_zone_value(tb_input_value value, tb_input_dead_zone kind,
                                               float lower, float upper)
{
    const float length = sqrtf(tb_input_length_squared(value));

    if (kind == TB_INPUT_DEAD_ZONE_AXIAL)
    {
        return (tb_input_value){tb_input_axial(value.x, lower, upper),
                                tb_input_axial(value.y, lower, upper),
                                tb_input_axial(value.z, lower, upper)};
    }
    if (length < lower || length == 0.0f)
    {
        return (tb_input_value){0};
    }
    if (kind == TB_INPUT_DEAD_ZONE_UNSCALED_RADIAL)
    {
        return length > upper ? tb_input_scaled(value, upper / length) : value;
    }
    return tb_input_scaled(value, tb_input_dead_zone_part(length, lower, upper) / length);
}

static tb_input_value tb_input_negate(tb_input_value value, uint32_t axes)
{
    const uint32_t which = axes == 0 ? (uint32_t)TB_INPUT_ALL_AXES : axes;

    if ((which & TB_INPUT_AXIS_X) != 0)
    {
        value.x = -value.x;
    }
    if ((which & TB_INPUT_AXIS_Y) != 0)
    {
        value.y = -value.y;
    }
    if ((which & TB_INPUT_AXIS_Z) != 0)
    {
        value.z = -value.z;
    }
    return value;
}

static tb_input_value tb_input_swizzle_value(tb_input_value value, tb_input_swizzle order)
{
    switch (order)
    {
    case TB_INPUT_SWIZZLE_YXZ:
        return (tb_input_value){value.y, value.x, value.z};
    case TB_INPUT_SWIZZLE_ZYX:
        return (tb_input_value){value.z, value.y, value.x};
    case TB_INPUT_SWIZZLE_XZY:
        return (tb_input_value){value.x, value.z, value.y};
    case TB_INPUT_SWIZZLE_YZX:
        return (tb_input_value){value.y, value.z, value.x};
    case TB_INPUT_SWIZZLE_ZXY:
        return (tb_input_value){value.z, value.x, value.y};
    case TB_INPUT_SWIZZLE_COUNT:
        break;
    }
    /* coverage-off: an order is checked when its modifier is made */
    return value;
    /* coverage-on */
}

static float tb_input_curve(float value, float exponent)
{
    const float power = exponent > 0.0f ? exponent : 1.0f;
    const float curved = powf(fabsf(value), power);
    return value < 0.0f ? -curved : curved;
}

/* The mean of the last `count` values given, this one included. */
static tb_input_value tb_input_smooth(tb_input_rule_state *state, tb_input_value value,
                                      uint32_t count)
{
    tb_input_value sum = {0};

    TB_ASSERT(count >= 1 && count <= TB_INPUT_MOST_SMOOTH_SAMPLES);
    state->samples[state->sample_next % count] = value;
    state->sample_next = (state->sample_next + 1) % count;
    if (state->sample_count < count)
    {
        state->sample_count++;
    }
    for (uint32_t index = 0; index < state->sample_count; index++)
    {
        sum.x += state->samples[index].x;
        sum.y += state->samples[index].y;
        sum.z += state->samples[index].z;
    }
    return tb_input_scaled(sum, 1.0f / (float)state->sample_count);
}

static tb_input_value tb_input_settings_value(tb_input_settings_source source,
                                              tb_input_rule_state *state, tb_input_value value,
                                              const tb_input_settings *settings)
{
    switch (source)
    {
    case TB_INPUT_SETTINGS_MOUSE:
        value.x *= settings->mouse_sensitivity;
        value.y *=
            settings->mouse_invert_y ? -settings->mouse_sensitivity : settings->mouse_sensitivity;
        if (!settings->mouse_smoothing)
        {
            state->sample_count = 0;
            state->sample_next = 0;
            return value;
        }
        return tb_input_smooth(state, value, TB_INPUT_MOUSE_SMOOTH_SAMPLES);
    case TB_INPUT_SETTINGS_GAMEPAD:
        value.x *= settings->gamepad_horizontal_sensitivity;
        value.y *= settings->gamepad_invert_y ? -settings->gamepad_vertical_sensitivity
                                              : settings->gamepad_vertical_sensitivity;
        return value;
    case TB_INPUT_SETTINGS_TOUCH:
        value.x *= settings->touch_sensitivity;
        value.y *= settings->touch_sensitivity;
        return value;
    case TB_INPUT_SETTINGS_LEFT_STICK:
        return tb_input_dead_zone_value(value, TB_INPUT_DEAD_ZONE_RADIAL,
                                        settings->gamepad_left_dead_zone, 1.0f);
    case TB_INPUT_SETTINGS_RIGHT_STICK:
        return tb_input_dead_zone_value(value, TB_INPUT_DEAD_ZONE_RADIAL,
                                        settings->gamepad_right_dead_zone, 1.0f);
    case TB_INPUT_SETTINGS_SOURCE_COUNT:
        break;
    }
    /* coverage-off: a source is checked when its modifier is made */
    return value;
    /* coverage-on */
}

tb_input_value tb_input_modify(const tb_input_modifier *modifier, tb_input_rule_state *state,
                               tb_input_value value, const tb_input_settings *settings)
{
    switch (modifier->type)
    {
    case TB_INPUT_MODIFIER_DEAD_ZONE:
        return tb_input_dead_zone_value(value, modifier->dead_zone, tb_input_lower(modifier),
                                        tb_input_upper(modifier));
    case TB_INPUT_MODIFIER_NEGATE:
        return tb_input_negate(value, modifier->axes);
    case TB_INPUT_MODIFIER_SWIZZLE:
        return tb_input_swizzle_value(value, modifier->order);
    case TB_INPUT_MODIFIER_SCALAR:
        return (tb_input_value){value.x * modifier->scale.x, value.y * modifier->scale.y,
                                value.z * modifier->scale.z};
    case TB_INPUT_MODIFIER_RESPONSE_CURVE:
        return (tb_input_value){tb_input_curve(value.x, modifier->exponent.x),
                                tb_input_curve(value.y, modifier->exponent.y),
                                tb_input_curve(value.z, modifier->exponent.z)};
    case TB_INPUT_MODIFIER_SMOOTH:
        return tb_input_smooth(state, value,
                               modifier->samples > 0 ? modifier->samples
                                                     : TB_INPUT_DEFAULT_SMOOTH_SAMPLES);
    case TB_INPUT_MODIFIER_SETTINGS:
        return tb_input_settings_value(modifier->source, state, value, settings);
    case TB_INPUT_MODIFIER_TYPE_COUNT:
        break;
    }
    /* coverage-off: a type is checked when its modifier is made */
    return value;
    /* coverage-on */
}

/* ---------------------------------------------------------------------------
 * Triggers.
 * ------------------------------------------------------------------------- */

static double tb_input_trigger_seconds(const tb_input_trigger *trigger, double fallback)
{
    return trigger->seconds > 0.0 ? trigger->seconds : fallback;
}

static tb_input_trigger_state tb_input_hold(const tb_input_trigger *trigger,
                                            tb_input_rule_state *state, bool actuated,
                                            double seconds)
{
    if (!actuated)
    {
        state->held = 0.0;
        state->fired = false;
        return TB_INPUT_STATE_NONE;
    }
    state->held += seconds;
    if (state->held < tb_input_trigger_seconds(trigger, TB_INPUT_DEFAULT_HOLD_SECONDS))
    {
        return TB_INPUT_STATE_ONGOING;
    }
    const bool first = !state->fired;
    state->fired = true;
    return first || !trigger->one_shot ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
}

static tb_input_trigger_state tb_input_hold_and_release(const tb_input_trigger *trigger,
                                                        tb_input_rule_state *state, bool actuated,
                                                        double seconds)
{
    if (actuated)
    {
        state->held += seconds;
        return TB_INPUT_STATE_ONGOING;
    }
    const bool long_enough =
        state->last_actuated &&
        state->held >= tb_input_trigger_seconds(trigger, TB_INPUT_DEFAULT_HOLD_SECONDS);
    state->held = 0.0;
    return long_enough ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
}

static tb_input_trigger_state tb_input_tap(const tb_input_trigger *trigger,
                                           tb_input_rule_state *state, bool actuated,
                                           double seconds)
{
    const double longest = tb_input_trigger_seconds(trigger, TB_INPUT_DEFAULT_TAP_SECONDS);

    if (actuated)
    {
        state->held += seconds;
        return state->held <= longest ? TB_INPUT_STATE_ONGOING : TB_INPUT_STATE_NONE;
    }
    const bool tapped = state->last_actuated && state->held <= longest;
    state->held = 0.0;
    return tapped ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
}

static tb_input_trigger_state tb_input_pulse(const tb_input_trigger *trigger,
                                             tb_input_rule_state *state, bool actuated,
                                             double seconds)
{
    const double interval = tb_input_trigger_seconds(trigger, TB_INPUT_DEFAULT_PULSE_SECONDS);
    bool pulse = false;

    if (!actuated)
    {
        state->held = 0.0;
        state->pulses = 0;
        return TB_INPUT_STATE_NONE;
    }
    if (!state->last_actuated)
    {
        state->held = 0.0;
        state->pulses = 0;
        state->next_pulse = interval;
        pulse = trigger->pulse_on_start;
    }
    state->held += seconds;
    if (state->held >= state->next_pulse)
    {
        pulse = true;
        while (state->next_pulse <= state->held)
        {
            state->next_pulse += interval;
        }
    }
    const bool spent = trigger->limit > 0 && state->pulses >= trigger->limit;
    if (spent)
    {
        return TB_INPUT_STATE_NONE;
    }
    if (pulse)
    {
        state->pulses++;
        return TB_INPUT_STATE_TRIGGERED;
    }
    return TB_INPUT_STATE_ONGOING;
}

tb_input_trigger_state tb_input_trigger_update(const tb_input_trigger *trigger,
                                               tb_input_rule_state *state, tb_input_value value,
                                               double seconds, tb_input_trigger_state chord)
{
    const float threshold =
        trigger->threshold > 0.0f ? trigger->threshold : TB_INPUT_DEFAULT_THRESHOLD;
    const bool actuated = tb_input_length_squared(value) >= threshold * threshold;
    tb_input_trigger_state result = TB_INPUT_STATE_NONE;

    switch (trigger->type)
    {
    case TB_INPUT_TRIGGER_DOWN:
        result = actuated ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
        break;
    case TB_INPUT_TRIGGER_PRESSED:
        result = actuated && !state->last_actuated ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
        break;
    case TB_INPUT_TRIGGER_RELEASED:
        if (actuated)
        {
            result = TB_INPUT_STATE_ONGOING;
        }
        else
        {
            result = state->last_actuated ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
        }
        break;
    case TB_INPUT_TRIGGER_HOLD:
        result = tb_input_hold(trigger, state, actuated, seconds);
        break;
    case TB_INPUT_TRIGGER_HOLD_AND_RELEASE:
        result = tb_input_hold_and_release(trigger, state, actuated, seconds);
        break;
    case TB_INPUT_TRIGGER_TAP:
        result = tb_input_tap(trigger, state, actuated, seconds);
        break;
    case TB_INPUT_TRIGGER_PULSE:
        result = tb_input_pulse(trigger, state, actuated, seconds);
        break;
    case TB_INPUT_TRIGGER_CHORDED:
    case TB_INPUT_TRIGGER_CHORD_BLOCKER:
        result = chord == TB_INPUT_STATE_TRIGGERED ? TB_INPUT_STATE_TRIGGERED : TB_INPUT_STATE_NONE;
        break;
    case TB_INPUT_TRIGGER_TYPE_COUNT:
        /* coverage-off: a type is checked when its trigger is made */
        TB_ASSERT_MSG(false, "a trigger of a known type");
        break;
        /* coverage-on */
    }
    state->last_actuated = actuated;
    return result;
}

/* ---------------------------------------------------------------------------
 * The tracker.
 * ------------------------------------------------------------------------- */

void tb_input_tracker_add(tb_input_tracker *tracker, tb_input_trigger_type type,
                          tb_input_trigger_state state)
{
    tracker->count++;
    if (type == TB_INPUT_TRIGGER_CHORD_BLOCKER)
    {
        tracker->blocked = tracker->blocked || state == TB_INPUT_STATE_TRIGGERED;
        return;
    }
    if (state == TB_INPUT_STATE_ONGOING)
    {
        tracker->any_ongoing = true;
    }
    if (type == TB_INPUT_TRIGGER_CHORDED)
    {
        tracker->all_implicit_triggered =
            tracker->all_implicit_triggered && state == TB_INPUT_STATE_TRIGGERED;
        return;
    }
    tracker->found_explicit = true;
    tracker->any_explicit_triggered =
        tracker->any_explicit_triggered || state == TB_INPUT_STATE_TRIGGERED;
}

tb_input_trigger_state tb_input_tracker_state(const tb_input_tracker *tracker, tb_input_value value)
{
    if (tracker->blocked)
    {
        return TB_INPUT_STATE_NONE;
    }
    /* Without explicit triggers, the value itself is the explicit part: a
     * chord alone fires only while its own key is down. */
    const bool explicit_part =
        tracker->found_explicit ? tracker->any_explicit_triggered : !tb_input_value_is_zero(value);
    if (explicit_part && tracker->all_implicit_triggered)
    {
        return TB_INPUT_STATE_TRIGGERED;
    }
    return tracker->any_ongoing ? TB_INPUT_STATE_ONGOING : TB_INPUT_STATE_NONE;
}
