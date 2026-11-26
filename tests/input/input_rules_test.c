/* Modifiers must change a key's value as Unreal's do, triggers must fire
 * on the ticks Unreal's fire, timing edges included, and an action's
 * triggers must come to one state by Unreal's rules: explicit triggers when
 * any fires, implicit ones when all do, blockers never. */
#include "badger/input.h"

#include "tb_test.h"

#include <math.h>
#include <stddef.h>

/* A device of the tests', assigned to player 0. */
#define RIG_DEVICE TB_INPUT_DEVICE_FIRST_OTHER

/* An input with one action, mapped from one key in one context. */
typedef struct rig
{
    tb_input *input;
    tb_input_player *player;
    tb_input_action action;
    tb_input_context context;
} rig;

typedef struct rig_desc
{
    tb_input_value_type type;
    tb_input_key key;
    const tb_input_modifier *modifiers;
    size_t modifier_count;
    const tb_input_trigger *triggers;
    size_t trigger_count;
    /* The action's own. */
    const tb_input_modifier *action_modifiers;
    size_t action_modifier_count;
    const tb_input_trigger *action_triggers;
    size_t action_trigger_count;
} rig_desc;

static rig rig_make(const rig_desc *desc)
{
    rig made = {0};

    made.input = tb_input_create(NULL);
    made.player = tb_input_player_at(made.input, 0);
    tb_input_assign_device(made.input, RIG_DEVICE, 0);
    made.action = tb_input_create_action(
        made.input, &(tb_input_action_desc){.name = "act",
                                            .type = desc->type,
                                            .modifiers = desc->action_modifiers,
                                            .modifier_count = desc->action_modifier_count,
                                            .triggers = desc->action_triggers,
                                            .trigger_count = desc->action_trigger_count});
    made.context = tb_input_create_context(made.input, "play");
    tb_input_context_map(made.input, made.context,
                         &(tb_input_mapping_desc){.action = made.action,
                                                  .key = desc->key,
                                                  .modifiers = desc->modifiers,
                                                  .modifier_count = desc->modifier_count,
                                                  .triggers = desc->triggers,
                                                  .trigger_count = desc->trigger_count});
    tb_input_add_context(made.player, made.context, 0);
    return made;
}

static void feed(rig *made, tb_input_key key, float x, float y)
{
    tb_input_feed_key(made->input, RIG_DEVICE, key, (tb_input_value){x, y, 0.0f});
}

/* One tick of `seconds`; the action's events in it. */
static uint32_t step(rig *made, double seconds)
{
    tb_input_tick(made->input, seconds);
    return tb_input_action_events(made->player, made->action);
}

static tb_input_value value_of(const rig *made)
{
    return tb_input_action_value(made->player, made->action);
}

/* The value a modifier makes of a stick's position, through an axis2d. */
static tb_input_value modified(const tb_input_modifier *modifier, float x, float y)
{
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS2D,
                                    .key = TB_INPUT_KEY_GAMEPAD_LEFT_STICK,
                                    .modifiers = modifier,
                                    .modifier_count = 1});
    feed(&made, TB_INPUT_KEY_GAMEPAD_LEFT_STICK, x, y);
    (void)step(&made, 0.0);
    const tb_input_value value = value_of(&made);
    tb_input_destroy(made.input);
    return value;
}

#define EXPECT_VALUE(value, ex, ey, ez)                                                            \
    do                                                                                             \
    {                                                                                              \
        const tb_input_value expect_value_ = (value);                                              \
        TB_TEST_EXPECT_FLOAT_NEAR(expect_value_.x, (ex), 1e-5);                                    \
        TB_TEST_EXPECT_FLOAT_NEAR(expect_value_.y, (ey), 1e-5);                                    \
        TB_TEST_EXPECT_FLOAT_NEAR(expect_value_.z, (ez), 1e-5);                                    \
    } while (0)

/* ---------------------------------------------------------------------------
 * Modifiers.
 * ------------------------------------------------------------------------- */

/* A radial dead zone zeroes a stick near its center and scales the rest
 * onto 0 to 1 by length, keeping its direction, as a stick's should. */
TB_TEST(input_modifiers, a_radial_dead_zone_scales_by_length)
{
    const tb_input_modifier zone = {.type = TB_INPUT_MODIFIER_DEAD_ZONE, .lower = 0.2f};

    EXPECT_VALUE(modified(&zone, 0.1f, 0.1f), 0.0f, 0.0f, 0.0f);
    EXPECT_VALUE(modified(&zone, 0.6f, 0.0f), 0.5f, 0.0f, 0.0f);
    EXPECT_VALUE(modified(&zone, 0.0f, -0.6f), 0.0f, -0.5f, 0.0f);
    EXPECT_VALUE(modified(&zone, 0.0f, 2.0f), 0.0f, 1.0f, 0.0f);
    /* A lower threshold of 0 is Unreal's 0.2. */
    const tb_input_modifier fallback = {.type = TB_INPUT_MODIFIER_DEAD_ZONE};
    EXPECT_VALUE(modified(&fallback, 0.6f, 0.0f), 0.5f, 0.0f, 0.0f);
    /* An upper threshold makes 1 sooner. */
    const tb_input_modifier upper = {
        .type = TB_INPUT_MODIFIER_DEAD_ZONE, .lower = 0.2f, .upper = 0.6f};
    EXPECT_VALUE(modified(&upper, 0.4f, 0.0f), 0.5f, 0.0f, 0.0f);
    EXPECT_VALUE(modified(&upper, 0.7f, 0.0f), 1.0f, 0.0f, 0.0f);
}

/* An axial dead zone treats each axis alone, so a stick pushed straight up
 * has no X from a small wobble. */
TB_TEST(input_modifiers, an_axial_dead_zone_treats_each_axis_alone)
{
    const tb_input_modifier zone = {
        .type = TB_INPUT_MODIFIER_DEAD_ZONE, .dead_zone = TB_INPUT_DEAD_ZONE_AXIAL, .lower = 0.2f};

    EXPECT_VALUE(modified(&zone, 0.1f, 0.6f), 0.0f, 0.5f, 0.0f);
    EXPECT_VALUE(modified(&zone, -0.6f, 1.0f), -0.5f, 1.0f, 0.0f);
}

/* An unscaled radial dead zone zeroes the center and keeps the rest as it
 * is, but no longer than the upper threshold. */
TB_TEST(input_modifiers, an_unscaled_radial_dead_zone_keeps_values_outside)
{
    const tb_input_modifier zone = {.type = TB_INPUT_MODIFIER_DEAD_ZONE,
                                    .dead_zone = TB_INPUT_DEAD_ZONE_UNSCALED_RADIAL,
                                    .lower = 0.2f,
                                    .upper = 0.8f};

    EXPECT_VALUE(modified(&zone, 0.1f, 0.0f), 0.0f, 0.0f, 0.0f);
    EXPECT_VALUE(modified(&zone, 0.5f, 0.0f), 0.5f, 0.0f, 0.0f);
    EXPECT_VALUE(modified(&zone, 0.0f, 1.0f), 0.0f, 0.8f, 0.0f);
}

/* Negation without axes negates all three; with axes, only those. */
TB_TEST(input_modifiers, negation_changes_the_sign_of_its_axes)
{
    const tb_input_modifier all = {.type = TB_INPUT_MODIFIER_NEGATE};
    const tb_input_modifier y = {.type = TB_INPUT_MODIFIER_NEGATE, .axes = TB_INPUT_AXIS_Y};
    const tb_input_modifier xz = {.type = TB_INPUT_MODIFIER_NEGATE,
                                  .axes = TB_INPUT_AXIS_X | TB_INPUT_AXIS_Z};

    EXPECT_VALUE(modified(&all, 0.5f, -0.25f), -0.5f, 0.25f, 0.0f);
    EXPECT_VALUE(modified(&y, 0.5f, -0.25f), 0.5f, 0.25f, 0.0f);
    EXPECT_VALUE(modified(&xz, 0.5f, -0.25f), -0.5f, -0.25f, 0.0f);
}

/* Each swizzle puts the axes where its name says, through an axis3d. */
TB_TEST(input_modifiers, swizzles_reorder_the_axes)
{
    static const struct
    {
        tb_input_swizzle order;
        float x, y, z;
    } cases[] = {{TB_INPUT_SWIZZLE_YXZ, 2.0f, 1.0f, 0.0f},
                 {TB_INPUT_SWIZZLE_ZYX, 0.0f, 2.0f, 1.0f},
                 {TB_INPUT_SWIZZLE_XZY, 1.0f, 0.0f, 2.0f},
                 {TB_INPUT_SWIZZLE_YZX, 2.0f, 0.0f, 1.0f},
                 {TB_INPUT_SWIZZLE_ZXY, 0.0f, 1.0f, 2.0f}};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        const tb_input_modifier swizzle = {.type = TB_INPUT_MODIFIER_SWIZZLE,
                                           .order = cases[index].order};
        rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS3D,
                                        .key = TB_INPUT_KEY_TOUCH_1_POSITION,
                                        .modifiers = &swizzle,
                                        .modifier_count = 1});
        TB_TEST_CONTEXT("order %zu", index);
        feed(&made, TB_INPUT_KEY_TOUCH_1_POSITION, 1.0f, 2.0f);
        (void)step(&made, 0.0);
        EXPECT_VALUE(value_of(&made), cases[index].x, cases[index].y, cases[index].z);
        tb_input_destroy(made.input);
    }
}

/* A scalar multiplies each axis by its own factor; a response curve raises
 * each to its power, keeping the sign, and an exponent of 0 is linear. */
TB_TEST(input_modifiers, scalars_and_response_curves_shape_each_axis)
{
    const tb_input_modifier scalar = {.type = TB_INPUT_MODIFIER_SCALAR,
                                      .scale = {2.0f, -3.0f, 4.0f}};
    const tb_input_modifier curve = {.type = TB_INPUT_MODIFIER_RESPONSE_CURVE,
                                     .exponent = {2.0f, 0.0f, 1.0f}};

    EXPECT_VALUE(modified(&scalar, 0.5f, 0.25f), 1.0f, -0.75f, 0.0f);
    EXPECT_VALUE(modified(&curve, -0.5f, -0.5f), -0.25f, -0.5f, 0.0f);
}

/* Smoothing gives the mean of the last ticks' values, up to its samples. */
TB_TEST(input_modifiers, smoothing_averages_the_last_ticks)
{
    const tb_input_modifier smooth = {.type = TB_INPUT_MODIFIER_SMOOTH, .samples = 2};
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS1D,
                                    .key = TB_INPUT_KEY_MOUSE_X,
                                    .modifiers = &smooth,
                                    .modifier_count = 1});

    feed(&made, TB_INPUT_KEY_MOUSE_X, 4.0f, 0.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 4.0f, 0.0f, 0.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 2.0f, 0.0f, 0.0f);
    feed(&made, TB_INPUT_KEY_MOUSE_X, 2.0f, 0.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 1.0f, 0.0f, 0.0f);
    tb_input_destroy(made.input);

    /* Zero samples are four. */
    const tb_input_modifier fallback = {.type = TB_INPUT_MODIFIER_SMOOTH};
    made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS1D,
                                .key = TB_INPUT_KEY_MOUSE_X,
                                .modifiers = &fallback,
                                .modifier_count = 1});
    feed(&made, TB_INPUT_KEY_MOUSE_X, 8.0f, 0.0f);
    (void)step(&made, 0.0);
    for (int tick = 0; tick < 3; tick++)
    {
        (void)step(&made, 0.0);
    }
    EXPECT_VALUE(value_of(&made), 2.0f, 0.0f, 0.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 0.0f, 0.0f, 0.0f);
    tb_input_destroy(made.input);
}

/* The settings modifier applies the player's mouse settings: sensitivity
 * on both axes, invert Y, and smoothing over two ticks when it is on. */
TB_TEST(input_modifiers, mouse_settings_scale_invert_and_smooth)
{
    const tb_input_modifier mouse = {.type = TB_INPUT_MODIFIER_SETTINGS,
                                     .source = TB_INPUT_SETTINGS_MOUSE};
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS2D,
                                    .key = TB_INPUT_KEY_MOUSE_XY,
                                    .modifiers = &mouse,
                                    .modifier_count = 1});
    tb_input_settings settings = TB_INPUT_SETTINGS_DEFAULT;

    feed(&made, TB_INPUT_KEY_MOUSE_XY, 1.0f, 2.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 1.0f, 2.0f, 0.0f);
    settings.mouse_sensitivity = 2.0f;
    settings.mouse_invert_y = true;
    tb_input_set_settings(made.input, &settings);
    feed(&made, TB_INPUT_KEY_MOUSE_XY, 1.0f, 2.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 2.0f, -4.0f, 0.0f);
    settings.mouse_smoothing = true;
    tb_input_set_settings(made.input, &settings);
    feed(&made, TB_INPUT_KEY_MOUSE_XY, 1.0f, 2.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 2.0f, -4.0f, 0.0f);
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 1.0f, -2.0f, 0.0f);
    tb_input_destroy(made.input);
}

/* The gamepad's settings scale each axis by its own sensitivity and invert
 * Y; touch has one sensitivity; each stick has its own dead zone. */
TB_TEST(input_modifiers, gamepad_and_touch_settings_apply)
{
    tb_input_settings settings = TB_INPUT_SETTINGS_DEFAULT;
    const tb_input_modifier gamepad = {.type = TB_INPUT_MODIFIER_SETTINGS,
                                       .source = TB_INPUT_SETTINGS_GAMEPAD};
    const tb_input_modifier touch = {.type = TB_INPUT_MODIFIER_SETTINGS,
                                     .source = TB_INPUT_SETTINGS_TOUCH};
    const tb_input_modifier left = {.type = TB_INPUT_MODIFIER_SETTINGS,
                                    .source = TB_INPUT_SETTINGS_LEFT_STICK};
    const tb_input_modifier right = {.type = TB_INPUT_MODIFIER_SETTINGS,
                                     .source = TB_INPUT_SETTINGS_RIGHT_STICK};
    const tb_input_modifier *const modifiers[] = {&gamepad, &touch, &left, &right};
    const tb_input_value expected[] = {
        {1.0f, -1.5f, 0.0f}, {2.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.4482f, 0.4482f, 0.0f}};

    settings.gamepad_horizontal_sensitivity = 2.0f;
    settings.gamepad_vertical_sensitivity = 3.0f;
    settings.gamepad_invert_y = true;
    settings.touch_sensitivity = 4.0f;
    settings.gamepad_left_dead_zone = 0.8f;
    settings.gamepad_right_dead_zone = 0.2f;
    for (size_t index = 0; index < TB_TEST_COUNT_OF(modifiers); index++)
    {
        rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS2D,
                                        .key = TB_INPUT_KEY_GAMEPAD_LEFT_STICK,
                                        .modifiers = modifiers[index],
                                        .modifier_count = 1});
        TB_TEST_CONTEXT("source %zu", index);
        tb_input_set_settings(made.input, &settings);
        feed(&made, TB_INPUT_KEY_GAMEPAD_LEFT_STICK, 0.5f, 0.5f);
        (void)step(&made, 0.0);
        const tb_input_value value = value_of(&made);
        TB_TEST_EXPECT_FLOAT_NEAR(value.x, expected[index].x, 0.01);
        TB_TEST_EXPECT_FLOAT_NEAR(value.y, expected[index].y, 0.01);
        tb_input_destroy(made.input);
    }
}

/* Modifiers apply in order: a mapping's, then its context's for the
 * action, then the action's own on the combined value. */
TB_TEST(input_modifiers, mapping_context_and_action_modifiers_apply_in_order)
{
    const tb_input_modifier scale = {.type = TB_INPUT_MODIFIER_SCALAR, .scale = {2.0f, 2.0f, 2.0f}};
    const tb_input_modifier negate = {.type = TB_INPUT_MODIFIER_NEGATE};
    const tb_input_modifier curve = {.type = TB_INPUT_MODIFIER_RESPONSE_CURVE,
                                     .exponent = {2.0f, 2.0f, 2.0f}};
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS1D,
                                    .key = TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER,
                                    .modifiers = &scale,
                                    .modifier_count = 1,
                                    .action_modifiers = &curve,
                                    .action_modifier_count = 1});
    tb_input_context_set_action_rules(made.input, made.context,
                                      &(tb_input_action_rules_desc){.action = made.action,
                                                                    .modifiers = &negate,
                                                                    .modifier_count = 1});

    feed(&made, TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER, 0.25f, 0.0f);
    (void)step(&made, 0.0);
    /* (0.25 * 2) negated is -0.5, and squared keeping its sign, -0.25. */
    EXPECT_VALUE(value_of(&made), -0.25f, 0.0f, 0.0f);
    /* Rules set again replace the context's. */
    tb_input_context_set_action_rules(made.input, made.context,
                                      &(tb_input_action_rules_desc){.action = made.action});
    (void)step(&made, 0.0);
    EXPECT_VALUE(value_of(&made), 0.25f, 0.0f, 0.0f);
    tb_input_destroy(made.input);
}

/* ---------------------------------------------------------------------------
 * Triggers.
 * ------------------------------------------------------------------------- */

static rig button_rig(const tb_input_trigger *trigger)
{
    return rig_make(&(rig_desc){.type = TB_INPUT_VALUE_BOOL,
                                .key = TB_INPUT_KEY_SPACE,
                                .triggers = trigger,
                                .trigger_count = trigger != NULL ? 1 : 0});
}

static void down(rig *made)
{
    feed(made, TB_INPUT_KEY_SPACE, 1.0f, 0.0f);
}

static void up(rig *made)
{
    feed(made, TB_INPUT_KEY_SPACE, 0.0f, 0.0f);
}

#define STARTED TB_INPUT_EVENT_STARTED
#define ONGOING TB_INPUT_EVENT_ONGOING
#define TRIGGERED TB_INPUT_EVENT_TRIGGERED
#define COMPLETED TB_INPUT_EVENT_COMPLETED
#define CANCELED TB_INPUT_EVENT_CANCELED

/* A mapping without triggers fires while its value is not zero, as with
 * Unreal's implicit Down. */
TB_TEST(input_triggers, no_trigger_fires_while_the_value_is_not_zero)
{
    rig made = button_rig(NULL);

    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    TB_TEST_EXPECT(tb_input_action_active(made.player, made.action));
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, TRIGGERED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, COMPLETED);
    TB_TEST_EXPECT(!tb_input_action_active(made.player, made.action));
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    tb_input_destroy(made.input);
}

/* Down fires every tick its value is actuated: at least its threshold,
 * Unreal's 0.5 when it gives none. */
TB_TEST(input_triggers, down_fires_at_its_threshold)
{
    const tb_input_trigger fallback = {.type = TB_INPUT_TRIGGER_DOWN};
    const tb_input_trigger low = {.type = TB_INPUT_TRIGGER_DOWN, .threshold = 0.25f};
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS1D,
                                    .key = TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER,
                                    .triggers = &fallback,
                                    .trigger_count = 1});

    feed(&made, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.49f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    feed(&made, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.5f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    EXPECT_VALUE(value_of(&made), 0.5f, 0.0f, 0.0f);
    tb_input_destroy(made.input);

    made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_AXIS1D,
                                .key = TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER,
                                .triggers = &low,
                                .trigger_count = 1});
    feed(&made, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.3f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    feed(&made, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.2f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, COMPLETED);
    /* An action in state none has no value, though its key has one. */
    EXPECT_VALUE(value_of(&made), 0.0f, 0.0f, 0.0f);
    tb_input_destroy(made.input);
}

/* Pressed fires once, on the tick the key goes down, and not while held. */
TB_TEST(input_triggers, pressed_fires_once_per_press)
{
    const tb_input_trigger pressed = {.type = TB_INPUT_TRIGGER_PRESSED};
    rig made = button_rig(&pressed);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, COMPLETED);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    tb_input_destroy(made.input);
}

/* Released is ongoing while held, and fires on the tick the key comes up. */
TB_TEST(input_triggers, released_fires_when_the_key_comes_up)
{
    const tb_input_trigger released = {.type = TB_INPUT_TRIGGER_RELEASED};
    rig made = button_rig(&released);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, ONGOING);
    TB_TEST_EXPECT_UINT(tb_input_action_state(made.player, made.action), ==,
                        TB_INPUT_STATE_ONGOING);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, COMPLETED);
    tb_input_destroy(made.input);
}

/* Hold is ongoing until held for its seconds, exactly at them included,
 * then fires every tick; released early, it is canceled. */
TB_TEST(input_triggers, hold_fires_once_held_long_enough)
{
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD, .seconds = 0.5};
    rig made = button_rig(&hold);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, COMPLETED);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, CANCELED);
    tb_input_destroy(made.input);
}

/* A one-shot hold fires once per hold, then completes while still held. */
TB_TEST(input_triggers, a_one_shot_hold_fires_once_per_hold)
{
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD, .seconds = 0.5, .one_shot = true};
    rig made = button_rig(&hold);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, COMPLETED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, 0);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, 0);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    tb_input_destroy(made.input);
}

/* A hold of 0 seconds is Unreal's one second. */
TB_TEST(input_triggers, a_hold_of_no_seconds_is_one_second)
{
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD};
    rig made = button_rig(&hold);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.5), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, ONGOING);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    tb_input_destroy(made.input);
}

/* Hold-and-release fires on release after its seconds or more, and is
 * canceled by a release before them. */
TB_TEST(input_triggers, hold_and_release_fires_on_a_long_release)
{
    const tb_input_trigger trigger = {.type = TB_INPUT_TRIGGER_HOLD_AND_RELEASE, .seconds = 0.5};
    rig made = button_rig(&trigger);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, CANCELED);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, ONGOING);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, COMPLETED);
    tb_input_destroy(made.input);
}

/* Tap fires when released within its seconds, exactly at them included,
 * and is canceled the tick a hold goes past them. */
TB_TEST(input_triggers, tap_fires_on_a_quick_release)
{
    const tb_input_trigger tap = {.type = TB_INPUT_TRIGGER_TAP, .seconds = 0.5};
    rig made = button_rig(&tap);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, ONGOING);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, COMPLETED);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, ONGOING);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, CANCELED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, 0);
    tb_input_destroy(made.input);
}

/* A tap of 0 seconds is Unreal's 0.2. */
TB_TEST(input_triggers, a_tap_of_no_seconds_is_a_fifth_of_a_second)
{
    const tb_input_trigger tap = {.type = TB_INPUT_TRIGGER_TAP};
    rig made = button_rig(&tap);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, 0);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, 0);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.125), ==, STARTED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.125), ==, TRIGGERED);
    tb_input_destroy(made.input);
}

/* Pulse fires every interval of a hold, on its first tick too when asked,
 * and no more than its limit. */
TB_TEST(input_triggers, pulse_fires_at_each_interval)
{
    const tb_input_trigger pulse = {.type = TB_INPUT_TRIGGER_PULSE, .seconds = 0.5};
    const tb_input_trigger limited = {
        .type = TB_INPUT_TRIGGER_PULSE, .seconds = 0.5, .pulse_on_start = true, .limit = 2};
    rig made = button_rig(&pulse);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, ONGOING);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    /* A long tick past several intervals pulses once. */
    TB_TEST_EXPECT_UINT(step(&made, 1.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, ONGOING);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, CANCELED);
    tb_input_destroy(made.input);

    made = button_rig(&limited);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED | TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, COMPLETED);
    TB_TEST_EXPECT_UINT(step(&made, 0.5), ==, 0);
    up(&made);
    (void)step(&made, 0.25);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED | TRIGGERED);
    tb_input_destroy(made.input);
}

/* An input with a "shift" action and a "jump" action of space that is
 * chorded with it, or blocked by it. */
static rig chord_rig(tb_input_trigger_type type, tb_input_action *shift)
{
    rig made = {0};

    made.input = tb_input_create(NULL);
    made.player = tb_input_player_at(made.input, 0);
    tb_input_assign_device(made.input, RIG_DEVICE, 0);
    made.context = tb_input_create_context(made.input, "play");
    /* Made first, so that the chord names an action made before it would
     * be evaluated were it not deferred. */
    made.action = tb_input_create_action(
        made.input, &(tb_input_action_desc){.name = "jump", .type = TB_INPUT_VALUE_BOOL});
    *shift = tb_input_create_action(
        made.input, &(tb_input_action_desc){.name = "shift", .type = TB_INPUT_VALUE_BOOL});
    const tb_input_trigger chord = {.type = type, .chord = *shift};
    tb_input_context_map(made.input, made.context,
                         &(tb_input_mapping_desc){.action = made.action,
                                                  .key = TB_INPUT_KEY_SPACE,
                                                  .triggers = &chord,
                                                  .trigger_count = 1});
    tb_input_context_map(
        made.input, made.context,
        &(tb_input_mapping_desc){.action = *shift, .key = TB_INPUT_KEY_LEFT_SHIFT});
    tb_input_add_context(made.player, made.context, 0);
    return made;
}

/* A chorded action fires only while its chord action does, and is seen in
 * the same tick that both go down, whatever order they were made in. */
TB_TEST(input_triggers, chorded_fires_only_with_its_chord)
{
    tb_input_action shift = TB_INPUT_ACTION_NONE;
    rig made = chord_rig(TB_INPUT_TRIGGER_CHORDED, &shift);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    up(&made);
    (void)step(&made, 0.1);
    feed(&made, TB_INPUT_KEY_LEFT_SHIFT, 1.0f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, 0);
    TB_TEST_EXPECT(tb_input_action_triggered(made.player, shift));
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    feed(&made, TB_INPUT_KEY_LEFT_SHIFT, 0.0f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, COMPLETED);
    tb_input_destroy(made.input);

    made = chord_rig(TB_INPUT_TRIGGER_CHORDED, &shift);
    feed(&made, TB_INPUT_KEY_LEFT_SHIFT, 1.0f, 0.0f);
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    tb_input_destroy(made.input);
}

/* A chord blocker stops its action while the chord action fires: plain
 * space jumps, shift and space do not. */
TB_TEST(input_triggers, a_chord_blocker_blocks_with_its_chord)
{
    tb_input_action shift = TB_INPUT_ACTION_NONE;
    rig made = chord_rig(TB_INPUT_TRIGGER_CHORD_BLOCKER, &shift);

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    feed(&made, TB_INPUT_KEY_LEFT_SHIFT, 1.0f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, COMPLETED);
    feed(&made, TB_INPUT_KEY_LEFT_SHIFT, 0.0f, 0.0f);
    TB_TEST_EXPECT_UINT(step(&made, 0.1), ==, STARTED | TRIGGERED);
    tb_input_destroy(made.input);
}

/* With an explicit and an implicit trigger, both must fire: a press of
 * space while shift is held, not shift pressed while space is held. */
TB_TEST(input_triggers, explicit_and_implicit_triggers_both_must_fire)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_action shift =
        tb_input_create_action(input, &(tb_input_action_desc){.name = "shift"});
    const tb_input_action jump =
        tb_input_create_action(input, &(tb_input_action_desc){.name = "jump"});
    const tb_input_trigger triggers[] = {{.type = TB_INPUT_TRIGGER_PRESSED},
                                         {.type = TB_INPUT_TRIGGER_CHORDED, .chord = shift}};

    tb_input_context_map(
        input, context,
        &(tb_input_mapping_desc){
            .action = jump, .key = TB_INPUT_KEY_SPACE, .triggers = triggers, .trigger_count = 2});
    tb_input_context_map(input, context,
                         &(tb_input_mapping_desc){.action = shift, .key = TB_INPUT_KEY_LEFT_SHIFT});
    tb_input_add_context(player, context, 0);
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE,
                      (tb_input_value){1.0f, 0.0f, 0.0f});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_triggered(player, jump));
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_LEFT_SHIFT,
                      (tb_input_value){1.0f, 0.0f, 0.0f});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_triggered(player, jump));
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE, (tb_input_value){0});
    tb_input_tick(input, 0.1);
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE,
                      (tb_input_value){1.0f, 0.0f, 0.0f});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, jump));
    tb_input_destroy(input);
}

/* An action's own triggers work on the combined value: a hold of the
 * action itself, with mappings that have none. */
TB_TEST(input_triggers, an_action_can_have_triggers_of_its_own)
{
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD, .seconds = 0.5};
    const tb_input_trigger released = {.type = TB_INPUT_TRIGGER_RELEASED};
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_BOOL,
                                    .key = TB_INPUT_KEY_SPACE,
                                    .action_triggers = &hold,
                                    .action_trigger_count = 1});

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    tb_input_destroy(made.input);

    made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_BOOL,
                                .key = TB_INPUT_KEY_SPACE,
                                .action_triggers = &released,
                                .action_trigger_count = 1});
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    tb_input_destroy(made.input);
}

/* With triggers on both a mapping and its action, the action fires only
 * when both do: a pressed mapping and a held action never meet. */
TB_TEST(input_triggers, mapping_and_action_triggers_must_both_fire)
{
    const tb_input_trigger down_trigger = {.type = TB_INPUT_TRIGGER_DOWN};
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD, .seconds = 0.5};
    rig made = rig_make(&(rig_desc){.type = TB_INPUT_VALUE_BOOL,
                                    .key = TB_INPUT_KEY_SPACE,
                                    .triggers = &down_trigger,
                                    .trigger_count = 1,
                                    .action_triggers = &hold,
                                    .action_trigger_count = 1});

    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, COMPLETED);
    tb_input_destroy(made.input);
}

/* A context's triggers for an action apply to each of its mappings of it:
 * a menu where the select key must be tapped. */
TB_TEST(input_triggers, a_context_adds_triggers_to_its_mappings)
{
    const tb_input_trigger tap = {.type = TB_INPUT_TRIGGER_TAP, .seconds = 0.5};
    rig made = button_rig(NULL);

    tb_input_context_set_action_rules(
        made.input, made.context,
        &(tb_input_action_rules_desc){.action = made.action, .triggers = &tap, .trigger_count = 1});
    down(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, STARTED);
    up(&made);
    TB_TEST_EXPECT_UINT(step(&made, 0.25), ==, TRIGGERED);
    tb_input_destroy(made.input);
}

/* ---------------------------------------------------------------------------
 * Checks.
 * ------------------------------------------------------------------------- */

/* Every broken modifier and trigger is refused with its own reason, before
 * it can be made. */
TB_TEST(input_checks, broken_rules_are_refused_with_a_reason)
{
    static const struct
    {
        tb_input_modifier modifier;
        const char *reason;
    } modifiers[] = {
        {{.type = TB_INPUT_MODIFIER_TYPE_COUNT}, "a modifier of an unknown type"},
        {{.type = TB_INPUT_MODIFIER_DEAD_ZONE, .dead_zone = TB_INPUT_DEAD_ZONE_COUNT},
         "a dead zone of an unknown kind"},
        {{.type = TB_INPUT_MODIFIER_DEAD_ZONE, .lower = 1.0f},
         "a dead zone's lower threshold: from 0 to below 1"},
        {{.type = TB_INPUT_MODIFIER_DEAD_ZONE, .upper = 1.5f},
         "a dead zone's upper threshold: from 0 to 1"},
        {{.type = TB_INPUT_MODIFIER_DEAD_ZONE, .lower = 0.5f, .upper = 0.4f},
         "a dead zone whose upper threshold is not above its lower"},
        {{.type = TB_INPUT_MODIFIER_NEGATE, .axes = 8u},
         "a negation of axes other than x, y and z"},
        {{.type = TB_INPUT_MODIFIER_SWIZZLE, .order = TB_INPUT_SWIZZLE_COUNT},
         "a swizzle of an unknown order"},
        {{.type = TB_INPUT_MODIFIER_SCALAR, .scale = {1.0f, INFINITY, 1.0f}},
         "a scale: finite numbers"},
        {{.type = TB_INPUT_MODIFIER_RESPONSE_CURVE, .exponent = {1.0f, -1.0f, 1.0f}},
         "an exponent: finite numbers, not negative"},
        {{.type = TB_INPUT_MODIFIER_SMOOTH, .samples = 9}, "a smoothing's samples: from 1 to 8"},
        {{.type = TB_INPUT_MODIFIER_SETTINGS, .source = TB_INPUT_SETTINGS_SOURCE_COUNT},
         "settings of an unknown source"}};
    tb_input *const input = tb_input_create(NULL);
    const tb_input_action other =
        tb_input_create_action(input, &(tb_input_action_desc){.name = "other"});

    for (size_t index = 0; index < TB_TEST_COUNT_OF(modifiers); index++)
    {
        TB_TEST_CONTEXT("modifier %zu", index);
        TB_TEST_EXPECT_STRING(
            tb_input_check_action(input,
                                  &(tb_input_action_desc){.name = "a",
                                                          .modifiers = &modifiers[index].modifier,
                                                          .modifier_count = 1}),
            ==, modifiers[index].reason);
    }
    const struct
    {
        tb_input_trigger trigger;
        const char *reason;
    } triggers[] = {
        {{.type = TB_INPUT_TRIGGER_TYPE_COUNT}, "a trigger of an unknown type"},
        {{.type = TB_INPUT_TRIGGER_DOWN, .threshold = -1.0f},
         "a threshold: a finite number, not negative"},
        {{.type = TB_INPUT_TRIGGER_HOLD, .seconds = -1.0},
         "seconds: a finite number, not negative"},
        {{.type = TB_INPUT_TRIGGER_CHORDED}, "a chord of no action"},
        {{.type = TB_INPUT_TRIGGER_CHORD_BLOCKER, .chord = {7}}, "a chord of no action"},
        {{.type = TB_INPUT_TRIGGER_CHORDED, .chord = other}, "a chord of its own action"}};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(triggers); index++)
    {
        TB_TEST_CONTEXT("trigger %zu", index);
        TB_TEST_EXPECT_STRING(
            tb_input_check_mapping(input,
                                   &(tb_input_mapping_desc){.action = other,
                                                            .key = TB_INPUT_KEY_A,
                                                            .triggers = &triggers[index].trigger,
                                                            .trigger_count = 1}),
            ==, triggers[index].reason);
    }
    TB_TEST_EXPECT_STRING(
        tb_input_check_mapping(
            input,
            &(tb_input_mapping_desc){.action = other, .key = TB_INPUT_KEY_A, .trigger_count = 1}),
        ==, "a count of rules without the rules");
    tb_input_destroy(input);
}

/* Actions, mappings and rules of contexts are refused for names, types,
 * actions and keys that are not. */
TB_TEST(input_checks, broken_definitions_are_refused_with_a_reason)
{
    tb_input *const input = tb_input_create(NULL);
    const tb_input_action jump =
        tb_input_create_action(input, &(tb_input_action_desc){.name = "jump"});

    TB_TEST_EXPECT_POINTER(tb_input_check_action(input, &(tb_input_action_desc){.name = "run_2"}),
                           ==, NULL);
    TB_TEST_EXPECT_STRING(tb_input_check_action(input, &(tb_input_action_desc){0}), ==,
                          "a name: lowercase letters, digits and \"_\", at most 64 bytes");
    TB_TEST_EXPECT_STRING(tb_input_check_action(input, &(tb_input_action_desc){.name = "Jump"}), ==,
                          "a name: lowercase letters, digits and \"_\", at most 64 bytes");
    TB_TEST_EXPECT_STRING(tb_input_check_action(input, &(tb_input_action_desc){.name = ""}), ==,
                          "a name: lowercase letters, digits and \"_\", at most 64 bytes");
    TB_TEST_EXPECT_STRING(
        tb_input_check_action(
            input,
            &(tb_input_action_desc){
                .name = "a_name_of_sixty_five_bytes_which_is_one_more_than_names_may_have_"}),
        ==, "a name: lowercase letters, digits and \"_\", at most 64 bytes");
    TB_TEST_EXPECT_STRING(tb_input_check_action(input, &(tb_input_action_desc){.name = "jump"}), ==,
                          "a name an action already has");
    TB_TEST_EXPECT_STRING(
        tb_input_check_action(
            input, &(tb_input_action_desc){.name = "a", .type = TB_INPUT_VALUE_TYPE_COUNT}),
        ==, "a value type other than bool, axis1d, axis2d and axis3d");
    TB_TEST_EXPECT_STRING(
        tb_input_check_action(
            input,
            &(tb_input_action_desc){.name = "a", .accumulation = TB_INPUT_ACCUMULATION_COUNT}),
        ==, "an accumulation other than highest and cumulative");
    TB_TEST_EXPECT_STRING(
        tb_input_check_mapping(input, &(tb_input_mapping_desc){.key = TB_INPUT_KEY_A}), ==,
        "a mapping of no action");
    TB_TEST_EXPECT_STRING(tb_input_check_mapping(input, &(tb_input_mapping_desc){.action = jump}),
                          ==, "a mapping of no key");
    TB_TEST_EXPECT_STRING(
        tb_input_check_mapping(input,
                               &(tb_input_mapping_desc){.action = jump, .key = TB_INPUT_KEY_COUNT}),
        ==, "a mapping of no key");
    TB_TEST_EXPECT_STRING(tb_input_check_action_rules(input, &(tb_input_action_rules_desc){0}), ==,
                          "rules of no action");
    TB_TEST_EXPECT_POINTER(
        tb_input_check_action_rules(input, &(tb_input_action_rules_desc){.action = jump}), ==,
        NULL);
    tb_input_destroy(input);
}
