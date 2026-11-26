/* A gamepad's state becomes keys of the player its device is assigned to:
 * its buttons, sticks and triggers, and the buttons the sticks and the
 * triggers make, with hysteresis; never the guide button; and dead zones
 * applied once, by the settings modifier. */
#include "badger/input.h"

#include "tb_test.h"

#include <stddef.h>

static const tb_input_device first_pad = TB_INPUT_DEVICE_GAMEPAD(0);

static tb_input *pad_input(void)
{
    tb_input *const input = tb_input_create(NULL);

    tb_input_assign_device(input, first_pad, 0);
    return input;
}

static float key_value(tb_input *input, tb_input_key key)
{
    return tb_input_key_value(tb_input_player_at(input, 0), key).x;
}

TB_TEST(input_gamepad, buttons_and_axes_become_keys)
{
    tb_input *const input = pad_input();
    tb_gamepad_state state = {.connected = true};

    for (size_t button = 0; button < TB_GAMEPAD_BUTTON_COUNT; button++)
    {
        state.buttons |= 1u << button;
    }
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = 0.25f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_Y] = -0.5f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_X] = -0.75f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_Y] = 0.125f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.5f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] = 1.0f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    for (size_t button = 0; button < TB_GAMEPAD_BUTTON_COUNT; button++)
    {
        const tb_input_key key = tb_input_key_from_gamepad_button((tb_gamepad_button)button);
        TB_TEST_CONTEXT("button %s", tb_gamepad_button_name((tb_gamepad_button)button));
        TB_TEST_EXPECT_UINT(key, !=, TB_INPUT_KEY_NONE);
        TB_TEST_EXPECT_FLOAT(key_value(input, key), ==, 1.0f);
    }
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X), ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y), ==, -0.5f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X), ==, -0.75f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_Y), ==, 0.125f);
    TB_TEST_EXPECT_FLOAT(
        tb_input_key_value(tb_input_player_at(input, 0), TB_INPUT_KEY_GAMEPAD_RIGHT_STICK).x, ==,
        -0.75f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER), ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER), ==, 1.0f);
    /* The guide button is the system's: no state of a pad gives it. */
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_GUIDE), ==, 0.0f);

    /* A pad that goes releases every button and centers every axis. */
    state.connected = false;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y), ==, 0.0f);
    tb_input_destroy(input);
}

TB_TEST(input_gamepad, gamepad_keys_name_buttons_and_axes)
{
    TB_TEST_EXPECT_UINT(tb_input_key_from_gamepad_button(TB_GAMEPAD_BUTTON_LEFT_STICK), ==,
                        TB_INPUT_KEY_GAMEPAD_LEFT_STICK_CLICK);
    TB_TEST_EXPECT_UINT(tb_input_key_from_gamepad_button(TB_GAMEPAD_BUTTON_COUNT), ==,
                        TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_UINT(tb_input_key_from_gamepad_axis(TB_GAMEPAD_AXIS_RIGHT_TRIGGER), ==,
                        TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER);
    TB_TEST_EXPECT_UINT(tb_input_key_from_gamepad_axis(TB_GAMEPAD_AXIS_COUNT), ==,
                        TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_STRING(tb_input_key_name(TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP), ==,
                          "gamepad_left_stick_up");
    TB_TEST_EXPECT_STRING(tb_input_key_name(TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON), ==,
                          "gamepad_right_trigger_button");
    TB_TEST_EXPECT_UINT(tb_input_key_type(TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT), ==,
                        TB_INPUT_VALUE_BOOL);
    TB_TEST_EXPECT_UINT(tb_input_key_type(TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON), ==,
                        TB_INPUT_VALUE_BOOL);
}

/* A device assigned to no player is ignored, and so is a pad fed again
 * with nothing changed. */
TB_TEST(input_gamepad, only_changes_of_an_assigned_pad_are_fed)
{
    tb_input *const input = pad_input();
    tb_input_recording *const recording = tb_input_recording_create();
    tb_gamepad_state state = {.connected = true, .buttons = 1u << TB_GAMEPAD_BUTTON_SOUTH};

    tb_input_feed_gamepad(input, TB_INPUT_DEVICE_GAMEPAD(1), &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH), ==, 0.0f);

    tb_input_record(input, recording);
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    tb_input_record(input, NULL);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH), ==, 1.0f);
    TB_TEST_EXPECT_UINT(tb_input_recording_tick_count(recording), ==, 2);
    tb_input_recording_destroy(recording);
    tb_input_destroy(input);
}

/* A stick's direction presses at 0.5 and releases below 0.4; a trigger's
 * button presses at 0.2 and releases below 0.1. A value that hovers
 * between does not chatter. */
TB_TEST(input_gamepad, sticks_and_triggers_make_buttons_with_hysteresis)
{
    tb_input *const input = pad_input();
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action jump = tb_input_create_action(
        input, &(tb_input_action_desc){.name = "jump", .type = TB_INPUT_VALUE_BOOL});
    const tb_input_context gameplay = tb_input_create_context(input, "gameplay");
    const tb_input_trigger pressed = {.type = TB_INPUT_TRIGGER_PRESSED};
    tb_gamepad_state state = {.connected = true};
    size_t jumps = 0;
    static const float hover[] = {0.21f, 0.15f, 0.19f, 0.11f, 0.2f, 0.12f};

    tb_input_context_map(input, gameplay,
                         &(tb_input_mapping_desc){.action = jump,
                                                  .key = TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON,
                                                  .triggers = &pressed,
                                                  .trigger_count = 1});
    tb_input_add_context(player, gameplay, 0);

    /* The trigger squeezed, then hovering about its threshold: one jump. */
    for (size_t index = 0; index < TB_TEST_COUNT_OF(hover); index++)
    {
        state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] = hover[index];
        tb_input_feed_gamepad(input, first_pad, &state);
        tb_input_tick(input, 0.1);
        jumps += tb_input_action_triggered(player, jump) ? 1u : 0u;
    }
    TB_TEST_EXPECT_UINT(jumps, ==, 1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON), ==, 1.0f);
    state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.09f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON), ==, 0.0f);

    /* The left stick up and to the left, then back to between the
     * thresholds: up stays, left was never pressed. */
    state.axes[TB_GAMEPAD_AXIS_LEFT_Y] = 0.6f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = -0.35f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_X] = 0.9f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_Y] = -0.9f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.5f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_LEFT), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_RIGHT), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_DOWN), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON), ==, 1.0f);
    state.axes[TB_GAMEPAD_AXIS_LEFT_Y] = 0.45f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN), ==, 0.0f);
    state.axes[TB_GAMEPAD_AXIS_LEFT_Y] = -0.5f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN), ==, 1.0f);
    tb_input_destroy(input);
}

/* A press of a stick's direction in a tick that releases it before the
 * tick is seen for that tick, as a key's is; and a recording, which holds
 * the axis, makes the same buttons when replayed. */
TB_TEST(input_gamepad, virtual_buttons_replay_from_their_axes)
{
    tb_input *const input = pad_input();
    tb_input *const copy = pad_input();
    tb_input_recording *const recording = tb_input_recording_create();
    tb_gamepad_state state = {.connected = true};

    tb_input_record(input, recording);
    state.axes[TB_GAMEPAD_AXIS_RIGHT_X] = -1.0f;
    tb_input_feed_gamepad(input, first_pad, &state);
    state.axes[TB_GAMEPAD_AXIS_RIGHT_X] = 0.0f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    tb_input_record(input, NULL);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT), ==, 1.0f);

    tb_input_replay_tick(copy, recording, 0);
    TB_TEST_EXPECT_FLOAT(key_value(copy, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT), ==, 1.0f);
    tb_input_tick(copy, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(copy, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT), ==, 0.0f);
    tb_input_recording_destroy(recording);
    tb_input_destroy(copy);
    tb_input_destroy(input);
}

/* The bridge feeds values as the pad gave them: a mapping's settings
 * modifier applies the dead zone of `controls.gamepad.left_dead_zone`, once. */
TB_TEST(input_gamepad, dead_zones_are_applied_once_by_the_settings_modifier)
{
    tb_input *const input = pad_input();
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action move = tb_input_create_action(
        input, &(tb_input_action_desc){.name = "move", .type = TB_INPUT_VALUE_AXIS2D});
    const tb_input_context gameplay = tb_input_create_context(input, "gameplay");
    const tb_input_modifier dead_zone = {.type = TB_INPUT_MODIFIER_SETTINGS,
                                         .source = TB_INPUT_SETTINGS_LEFT_STICK};
    tb_input_settings settings = TB_INPUT_SETTINGS_DEFAULT;
    tb_gamepad_state state = {.connected = true};

    settings.gamepad_left_dead_zone = 0.2f;
    tb_input_set_settings(input, &settings);
    tb_input_context_map(input, gameplay,
                         &(tb_input_mapping_desc){.action = move,
                                                  .key = TB_INPUT_KEY_GAMEPAD_LEFT_STICK,
                                                  .modifiers = &dead_zone,
                                                  .modifier_count = 1});
    tb_input_add_context(player, gameplay, 0);
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = 0.15f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(key_value(input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X), ==, 0.15f);
    TB_TEST_EXPECT_FLOAT(tb_input_action_value(player, move).x, ==, 0.0f);
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = 0.6f;
    tb_input_feed_gamepad(input, first_pad, &state);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_input_action_value(player, move).x, 0.5f, 1e-5f);
    tb_input_destroy(input);
}
