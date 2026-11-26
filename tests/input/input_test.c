/* The input must give each player's actions from the keys of the devices
 * assigned to it, through its contexts by priority, with keys consumed by
 * higher contexts; evaluate once a tick so that no tap is lost; translate
 * the window's events into keys; call callbacks in a fixed order; follow
 * the engine's settings; and replay a recording to the same actions. */
#include "badger/engine_settings.h"
#include "badger/input.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

#include "tb_test.h"

#include <stddef.h>
#include <string.h>

static const tb_input_value pressed_value = {1.0f, 0.0f, 0.0f};
static const tb_input_value released_value = {0.0f, 0.0f, 0.0f};

static void press(tb_input *input, tb_input_key key)
{
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, key, pressed_value);
}

static void release(tb_input *input, tb_input_key key)
{
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, key, released_value);
}

static tb_input_action make_action(tb_input *input, const char *name, tb_input_value_type type)
{
    return tb_input_create_action(input, &(tb_input_action_desc){.name = name, .type = type});
}

static void map(tb_input *input, tb_input_context context, tb_input_action action, tb_input_key key)
{
    tb_input_context_map(input, context, &(tb_input_mapping_desc){.action = action, .key = key});
}

/* ---------------------------------------------------------------------------
 * Contexts.
 * ------------------------------------------------------------------------- */

/* A higher context sees a key first and consumes it, so a lower context's
 * mapping of the key does not fire; one that passes it through lets both
 * fire; removed, the lower one fires again. */
TB_TEST(input_contexts, higher_priority_consumes_keys_from_lower)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action interact = make_action(input, "interact", TB_INPUT_VALUE_BOOL);
    const tb_input_action select = make_action(input, "select", TB_INPUT_VALUE_BOOL);
    const tb_input_context gameplay = tb_input_create_context(input, "gameplay");
    const tb_input_context menu = tb_input_create_context(input, "menu");
    const tb_input_context overlay = tb_input_create_context(input, "overlay");

    map(input, gameplay, interact, TB_INPUT_KEY_E);
    map(input, menu, select, TB_INPUT_KEY_E);
    tb_input_add_context(player, gameplay, 0);
    tb_input_add_context(player, menu, 10);
    press(input, TB_INPUT_KEY_E);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, select));
    TB_TEST_EXPECT(!tb_input_action_triggered(player, interact));

    TB_TEST_EXPECT(tb_input_remove_context(player, menu));
    TB_TEST_EXPECT(!tb_input_remove_context(player, menu));
    TB_TEST_EXPECT(!tb_input_has_context(player, menu));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, interact));
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, select), ==, TB_INPUT_EVENT_COMPLETED);

    /* A mapping that passes its key through lets lower contexts see it. */
    tb_input_context_map(
        input, overlay,
        &(tb_input_mapping_desc){.action = select, .key = TB_INPUT_KEY_E, .pass_through = true});
    tb_input_add_context(player, overlay, 5);
    release(input, TB_INPUT_KEY_E);
    tb_input_tick(input, 0.1);
    press(input, TB_INPUT_KEY_E);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, select));
    TB_TEST_EXPECT(tb_input_action_triggered(player, interact));
    tb_input_destroy(input);
}

/* Of contexts of equal priority, the one added last sees keys first; and a
 * context added again takes its new priority. */
TB_TEST(input_contexts, equal_priorities_go_to_the_last_added)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action first_action = make_action(input, "first", TB_INPUT_VALUE_BOOL);
    const tb_input_action second_action = make_action(input, "second", TB_INPUT_VALUE_BOOL);
    const tb_input_context first = tb_input_create_context(input, "first");
    const tb_input_context second = tb_input_create_context(input, "second");

    map(input, first, first_action, TB_INPUT_KEY_Q);
    map(input, second, second_action, TB_INPUT_KEY_Q);
    tb_input_add_context(player, first, 1);
    tb_input_add_context(player, second, 1);
    press(input, TB_INPUT_KEY_Q);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, second_action));
    TB_TEST_EXPECT(!tb_input_action_triggered(player, first_action));
    tb_input_add_context(player, first, 2);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, first_action));
    TB_TEST_EXPECT(!tb_input_action_triggered(player, second_action));
    tb_input_clear_contexts(player);
    TB_TEST_EXPECT(!tb_input_has_context(player, first));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, first_action), ==, TB_INPUT_EVENT_COMPLETED);
    tb_input_destroy(input);
}

/* In one context a key may drive many actions, and many keys one action. */
TB_TEST(input_contexts, keys_and_actions_map_many_to_many)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action fire = make_action(input, "fire", TB_INPUT_VALUE_BOOL);
    const tb_input_action alert = make_action(input, "alert", TB_INPUT_VALUE_BOOL);
    const tb_input_action forward = make_action(input, "forward", TB_INPUT_VALUE_AXIS1D);
    const tb_input_context context = tb_input_create_context(input, "play");

    map(input, context, fire, TB_INPUT_KEY_F);
    map(input, context, alert, TB_INPUT_KEY_F);
    map(input, context, forward, TB_INPUT_KEY_W);
    map(input, context, forward, TB_INPUT_KEY_UP);
    TB_TEST_EXPECT_UINT(tb_input_context_mapping_count(input, context), ==, 4);
    tb_input_add_context(player, context, 0);
    press(input, TB_INPUT_KEY_F);
    press(input, TB_INPUT_KEY_UP);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, fire));
    TB_TEST_EXPECT(tb_input_action_triggered(player, alert));
    TB_TEST_EXPECT_FLOAT(tb_input_action_value(player, forward).x, ==, 1.0f);
    /* Both keys: the highest, not the sum, by default. */
    press(input, TB_INPUT_KEY_W);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_action_value(player, forward).x, ==, 1.0f);
    release(input, TB_INPUT_KEY_UP);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, forward));
    tb_input_destroy(input);
}

/* WASD makes a 2D vector: W swizzled into Y, S swizzled and negated, D as
 * it is, A negated; opposite keys cancel when the action adds them up. */
TB_TEST(input_contexts, wasd_makes_an_axis2d_by_swizzle_and_negate)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action move = tb_input_create_action(
        input, &(tb_input_action_desc){.name = "move",
                                       .type = TB_INPUT_VALUE_AXIS2D,
                                       .accumulation = TB_INPUT_ACCUMULATE_CUMULATIVE});
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_modifier swizzle = {.type = TB_INPUT_MODIFIER_SWIZZLE};
    const tb_input_modifier negate = {.type = TB_INPUT_MODIFIER_NEGATE};
    const tb_input_modifier swizzle_negate[] = {swizzle, negate};

    tb_input_context_map(
        input, context,
        &(tb_input_mapping_desc){
            .action = move, .key = TB_INPUT_KEY_W, .modifiers = &swizzle, .modifier_count = 1});
    tb_input_context_map(input, context,
                         &(tb_input_mapping_desc){.action = move,
                                                  .key = TB_INPUT_KEY_S,
                                                  .modifiers = swizzle_negate,
                                                  .modifier_count = 2});
    tb_input_context_map(
        input, context,
        &(tb_input_mapping_desc){
            .action = move, .key = TB_INPUT_KEY_A, .modifiers = &negate, .modifier_count = 1});
    map(input, context, move, TB_INPUT_KEY_D);
    tb_input_add_context(player, context, 0);

    press(input, TB_INPUT_KEY_W);
    press(input, TB_INPUT_KEY_D);
    tb_input_tick(input, 0.1);
    tb_input_value value = tb_input_action_value(player, move);
    TB_TEST_EXPECT_FLOAT(value.x, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(value.y, ==, 1.0f);
    release(input, TB_INPUT_KEY_W);
    release(input, TB_INPUT_KEY_D);
    press(input, TB_INPUT_KEY_S);
    press(input, TB_INPUT_KEY_A);
    tb_input_tick(input, 0.1);
    value = tb_input_action_value(player, move);
    TB_TEST_EXPECT_FLOAT(value.x, ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(value.y, ==, -1.0f);
    press(input, TB_INPUT_KEY_W);
    tb_input_tick(input, 0.1);
    value = tb_input_action_value(player, move);
    TB_TEST_EXPECT_FLOAT(value.x, ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(value.y, ==, 0.0f);
    tb_input_destroy(input);
}

/* A key held when a context is added does not act in it until released:
 * the press that opened a menu does not choose in it. */
TB_TEST(input_contexts, keys_held_when_a_context_comes_wait_for_release)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action select = make_action(input, "select", TB_INPUT_VALUE_BOOL);
    const tb_input_context menu = tb_input_create_context(input, "menu");

    map(input, menu, select, TB_INPUT_KEY_ENTER);
    press(input, TB_INPUT_KEY_ENTER);
    tb_input_tick(input, 0.1);
    tb_input_add_context(player, menu, 0);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_triggered(player, select));
    release(input, TB_INPUT_KEY_ENTER);
    tb_input_tick(input, 0.1);
    press(input, TB_INPUT_KEY_ENTER);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, select));
    tb_input_destroy(input);
}

/* A context changed while a player has it starts over: a pressed trigger
 * of a mapping added since fires on the key already held. */
TB_TEST(input_contexts, a_changed_context_starts_over)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action jump = make_action(input, "jump", TB_INPUT_VALUE_BOOL);
    const tb_input_action duck = make_action(input, "duck", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");

    map(input, context, jump, TB_INPUT_KEY_SPACE);
    tb_input_add_context(player, context, 0);
    press(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, jump));
    map(input, context, duck, TB_INPUT_KEY_C);
    press(input, TB_INPUT_KEY_C);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, duck));
    /* Space was held when the context started over, so it waits. */
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, jump), ==, TB_INPUT_EVENT_COMPLETED);
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Ticks.
 * ------------------------------------------------------------------------- */

/* A key pressed and released between two ticks is down for the next tick
 * and up for the one after: started and triggered, then completed, and
 * triggered once. */
TB_TEST(input_ticks, a_tap_between_ticks_is_not_lost)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action jump = make_action(input, "jump", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_trigger pressed = {.type = TB_INPUT_TRIGGER_PRESSED};

    tb_input_context_map(
        input, context,
        &(tb_input_mapping_desc){
            .action = jump, .key = TB_INPUT_KEY_SPACE, .triggers = &pressed, .trigger_count = 1});
    tb_input_add_context(player, context, 0);
    press(input, TB_INPUT_KEY_SPACE);
    release(input, TB_INPUT_KEY_SPACE);
    press(input, TB_INPUT_KEY_SPACE);
    release(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, jump), ==,
                        TB_INPUT_EVENT_STARTED | TB_INPUT_EVENT_TRIGGERED);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_SPACE).x, ==, 1.0f);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, jump), ==, TB_INPUT_EVENT_COMPLETED);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_SPACE).x, ==, 0.0f);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, jump), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_tick_count(input), ==, 3);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_input_seconds(input), 0.3, 1e-12);
    tb_input_destroy(input);
}

/* The mouse's motion adds up between ticks and is gone after one; an axis
 * keeps the value fed last. */
TB_TEST(input_ticks, relative_keys_add_up_and_axes_keep_their_last)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);

    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_X,
                      (tb_input_value){2.0f, 0.0f, 0.0f});
    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_XY,
                      (tb_input_value){3.0f, -1.0f, 0.0f});
    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X,
                      (tb_input_value){0.5f, 0.0f, 0.0f});
    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X,
                      (tb_input_value){0.25f, 0.0f, 0.0f});
    tb_input_feed_key(input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_TOUCH_2_POSITION,
                      (tb_input_value){10.0f, 20.0f, 0.0f});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_X).x, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_XY).y, ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_GAMEPAD_LEFT_STICK).x, ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_TOUCH_2_POSITION).y, ==, 20.0f);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_X).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X).x, ==,
                         0.25f);
    tb_input_destroy(input);
}

/* An action's status keeps how long it has been under way and triggered,
 * and when it last triggered. */
TB_TEST(input_ticks, statuses_keep_times)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    const tb_input_action charge = make_action(input, "charge", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD, .seconds = 0.5};
    tb_input_action_status status = {0};

    tb_input_context_map(
        input, context,
        &(tb_input_mapping_desc){
            .action = charge, .key = TB_INPUT_KEY_R, .triggers = &hold, .trigger_count = 1});
    tb_input_add_context(player, context, 0);
    tb_input_get_action_status(player, charge, &status);
    TB_TEST_EXPECT_FLOAT(status.last_triggered_at, <, 0.0);
    press(input, TB_INPUT_KEY_R);
    tb_input_tick(input, 0.25);
    tb_input_tick(input, 0.25);
    tb_input_tick(input, 0.25);
    tb_input_get_action_status(player, charge, &status);
    TB_TEST_EXPECT_UINT(status.state, ==, TB_INPUT_STATE_TRIGGERED);
    TB_TEST_EXPECT_FLOAT(status.elapsed_seconds, ==, 0.75);
    TB_TEST_EXPECT_FLOAT(status.triggered_seconds, ==, 0.5);
    TB_TEST_EXPECT_FLOAT(status.last_triggered_at, ==, 0.75);
    TB_TEST_EXPECT_FLOAT(status.value.x, ==, 1.0f);
    release(input, TB_INPUT_KEY_R);
    tb_input_tick(input, 0.25);
    tb_input_get_action_status(player, charge, &status);
    TB_TEST_EXPECT_FLOAT(status.elapsed_seconds, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(status.triggered_seconds, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(status.last_triggered_at, ==, 0.75);
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Players and devices.
 * ------------------------------------------------------------------------- */

/* Each player has its own contexts and the keys of its own devices: the
 * keyboard goes to player 0 until it is given to another, and a device of
 * no player is ignored. */
TB_TEST(input_players, devices_feed_their_players)
{
    tb_input *const input = tb_input_create(&(tb_input_options){.player_count = 2});
    tb_input_player *const one = tb_input_player_at(input, 0);
    tb_input_player *const two = tb_input_player_at(input, 1);
    const tb_input_action jump = make_action(input, "jump", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_device pad = TB_INPUT_DEVICE_FIRST_OTHER + 3;

    TB_TEST_EXPECT_UINT(tb_input_player_count(input), ==, 2);
    TB_TEST_EXPECT_UINT(tb_input_player_index(two), ==, 1);
    TB_TEST_EXPECT_UINT(tb_input_device_player(input, TB_INPUT_DEVICE_KEYBOARD), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_device_player(input, TB_INPUT_DEVICE_MOUSE), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_device_player(input, pad), ==, TB_INPUT_NO_PLAYER);
    map(input, context, jump, TB_INPUT_KEY_SPACE);
    map(input, context, jump, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH);
    tb_input_add_context(one, context, 0);
    tb_input_add_context(two, context, 0);

    tb_input_feed_key(input, pad, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, pressed_value);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_triggered(one, jump));
    TB_TEST_EXPECT(!tb_input_action_triggered(two, jump));
    tb_input_assign_device(input, pad, 1);
    tb_input_assign_device(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_NO_PLAYER);
    TB_TEST_EXPECT_UINT(tb_input_device_player(input, pad), ==, 1);
    tb_input_feed_key(input, pad, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, pressed_value);
    press(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_triggered(one, jump));
    TB_TEST_EXPECT(tb_input_action_triggered(two, jump));
    tb_input_destroy(input);
}

/* Each player has its own state of the same action. */
TB_TEST(input_players, players_have_their_own_states)
{
    tb_input *const input = tb_input_create(&(tb_input_options){.player_count = 2});
    tb_input_player *const one = tb_input_player_at(input, 0);
    tb_input_player *const two = tb_input_player_at(input, 1);
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_action jump = make_action(input, "jump", TB_INPUT_VALUE_BOOL);

    map(input, context, jump, TB_INPUT_KEY_SPACE);
    tb_input_add_context(one, context, 0);
    tb_input_add_context(two, context, 0);
    tb_input_assign_device(input, TB_INPUT_DEVICE_KEYBOARD, 1);
    press(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_state(one, jump), ==, TB_INPUT_STATE_NONE);
    TB_TEST_EXPECT_UINT(tb_input_action_state(two, jump), ==, TB_INPUT_STATE_TRIGGERED);
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Window events.
 * ------------------------------------------------------------------------- */

static bool window_event(tb_input *input, tb_window_event event)
{
    return tb_input_feed_window_event(input, &event);
}

/* Keys and buttons go down and up; a key's repeats, a key the engine does
 * not name, and text are not input. */
TB_TEST(input_window, keys_and_buttons_become_keys)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);

    TB_TEST_EXPECT(
        window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_W}));
    TB_TEST_EXPECT(!window_event(
        input,
        (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_W, .repeat = true}));
    TB_TEST_EXPECT(!window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_UNKNOWN}));
    TB_TEST_EXPECT(window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
                                                         .button = TB_MOUSE_BUTTON_X2}));
    TB_TEST_EXPECT(!window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
                                                          .button = TB_MOUSE_BUTTON_NONE}));
    TB_TEST_EXPECT(
        !window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_TEXT_INPUT, .text = "w"}));
    TB_TEST_EXPECT(!window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_RESIZED}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_W).x, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_THUMB_2).x, ==, 1.0f);
    TB_TEST_EXPECT(
        window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_KEY_UP, .key = TB_KEY_W}));
    TB_TEST_EXPECT(window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_BUTTON_UP,
                                                         .button = TB_MOUSE_BUTTON_X2}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_W).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_THUMB_2).x, ==, 0.0f);
    tb_input_destroy(input);
}

/* Relative motion is the mouse's own, with Y away from the player; the
 * cursor's motion is from where it was, once known; the wheel is a button
 * each way and an axis. */
TB_TEST(input_window, the_mouse_moves_and_scrolls)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);

    TB_TEST_EXPECT(window_event(
        input, (tb_window_event){
                   .type = TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION, .delta_x = 3.0, .delta_y = 4.0}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_X).x, ==, 3.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_Y).x, ==, -4.0f);

    /* The first position is where the cursor is, not a motion. */
    TB_TEST_EXPECT(!window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_MOVED, .x = 10.0, .y = 10.0}));
    TB_TEST_EXPECT(window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_MOVED, .x = 15.0, .y = 8.0}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_XY).x, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_XY).y, ==, 2.0f);
    /* Leaving forgets the place; entering gives it again. */
    TB_TEST_EXPECT(!window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_LEFT}));
    TB_TEST_EXPECT(!window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_MOVED, .x = 50.0, .y = 50.0}));
    TB_TEST_EXPECT(!window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_LEFT}));
    TB_TEST_EXPECT(!window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_ENTERED, .x = 0.0, .y = 0.0}));
    TB_TEST_EXPECT(window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_MOVED, .x = 1.0, .y = 0.0}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_X).x, ==, 1.0f);

    TB_TEST_EXPECT(window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_WHEEL, .delta_y = 2.0}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_WHEEL_UP).x, ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_WHEEL_DOWN).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_WHEEL).x, ==, 2.0f);
    TB_TEST_EXPECT(window_event(
        input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_WHEEL, .delta_y = -1.0}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_WHEEL_UP).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_WHEEL_DOWN).x, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_WHEEL).x, ==, -1.0f);
    tb_input_destroy(input);
}

/* Losing the focus releases every key and button held, so that none sticks
 * down while another program has the keyboard. */
TB_TEST(input_window, losing_the_focus_releases_everything)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);

    TB_TEST_EXPECT(!window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_FOCUS_LOST}));
    (void)window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_A});
    (void)window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
                                                .button = TB_MOUSE_BUTTON_LEFT});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(window_event(input, (tb_window_event){.type = TB_WINDOW_EVENT_FOCUS_LOST}));
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_A).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_key_value(player, TB_INPUT_KEY_MOUSE_LEFT).x, ==, 0.0f);
    tb_input_destroy(input);
}

/* Keys have names, types, and the window's keys and buttons as keys. */
TB_TEST(input_window, keys_have_names_and_types)
{
    for (int key = 1; key < TB_INPUT_KEY_COUNT; key++)
    {
        const char *const name = tb_input_key_name((tb_input_key)key);
        TB_TEST_CONTEXT("key %d", key);
        TB_TEST_REQUIRE(name[0] != '\0');
        TB_TEST_EXPECT_INT(tb_input_find_key(name, tb_str_length(name)), ==, key);
    }
    TB_TEST_EXPECT_STRING(tb_input_key_name(TB_INPUT_KEY_NONE), ==, "");
    TB_TEST_EXPECT_STRING(tb_input_key_name(TB_INPUT_KEY_COUNT), ==, "");
    TB_TEST_EXPECT_INT(tb_input_find_key("spcae", 5), ==, TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_INT(tb_input_find_key(NULL, 0), ==, TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_INT(tb_input_key_type(TB_INPUT_KEY_SPACE), ==, TB_INPUT_VALUE_BOOL);
    TB_TEST_EXPECT_INT(tb_input_key_type(TB_INPUT_KEY_MOUSE_X), ==, TB_INPUT_VALUE_AXIS1D);
    TB_TEST_EXPECT_INT(tb_input_key_type(TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER), ==,
                       TB_INPUT_VALUE_AXIS1D);
    TB_TEST_EXPECT_INT(tb_input_key_type(TB_INPUT_KEY_GAMEPAD_RIGHT_STICK), ==,
                       TB_INPUT_VALUE_AXIS2D);
    TB_TEST_EXPECT_INT(tb_input_key_type(TB_INPUT_KEY_TOUCH_10_POSITION), ==,
                       TB_INPUT_VALUE_AXIS2D);
    TB_TEST_EXPECT_INT(tb_input_key_from_window(TB_KEY_A), ==, TB_INPUT_KEY_A);
    TB_TEST_EXPECT_INT(tb_input_key_from_window(TB_KEY_DELETE), ==, TB_INPUT_KEY_DELETE);
    TB_TEST_EXPECT_INT(tb_input_key_from_window(TB_KEY_COUNT), ==, TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_INT(tb_input_key_from_mouse_button(TB_MOUSE_BUTTON_LEFT), ==,
                       TB_INPUT_KEY_MOUSE_LEFT);
    TB_TEST_EXPECT_INT(tb_input_key_from_mouse_button(TB_MOUSE_BUTTON_RIGHT), ==,
                       TB_INPUT_KEY_MOUSE_RIGHT);
    TB_TEST_EXPECT_INT(tb_input_key_from_mouse_button(TB_MOUSE_BUTTON_MIDDLE), ==,
                       TB_INPUT_KEY_MOUSE_MIDDLE);
    TB_TEST_EXPECT_INT(tb_input_key_from_mouse_button(TB_MOUSE_BUTTON_X1), ==,
                       TB_INPUT_KEY_MOUSE_THUMB_1);
    TB_TEST_EXPECT_INT(tb_input_key_from_mouse_button(TB_MOUSE_BUTTON_COUNT), ==,
                       TB_INPUT_KEY_NONE);
}

/* ---------------------------------------------------------------------------
 * Callbacks.
 * ------------------------------------------------------------------------- */

#define MOST_CALLS 16

/* What callbacks were called with. */
typedef struct calls
{
    size_t count;
    tb_input_event events[MOST_CALLS];
    char names[MOST_CALLS];
    tb_input_player *player;
    tb_input_binding unbind;  /* Unbound by the next call, when set. */
    tb_input_action bind;     /* Bound by the next call, when set. */
    tb_input_context context; /* Removed by the next call, when set. */
} calls;

typedef struct call_site
{
    calls *calls;
    char name;
} call_site;

static void record_call(void *user, const tb_input_action_event *event)
{
    const call_site *const site = user;
    calls *const made = site->calls;

    if (made->count < MOST_CALLS)
    {
        made->events[made->count] = event->event;
        made->names[made->count] = site->name;
        made->count++;
    }
    if (made->unbind.id != 0)
    {
        tb_input_unbind(made->player, made->unbind);
        made->unbind = (tb_input_binding){0};
    }
    if (made->bind.id != 0)
    {
        (void)tb_input_bind(made->player, made->bind, TB_INPUT_EVENT_ALL, record_call, user);
        made->bind = TB_INPUT_ACTION_NONE;
    }
    if (made->context.id != 0)
    {
        (void)tb_input_remove_context(made->player, made->context);
        made->context = TB_INPUT_CONTEXT_NONE;
    }
}

/* Callbacks are called after the tick's evaluation, in the order bound,
 * once for each of their events in the order started, ongoing, triggered,
 * completed, canceled. */
TB_TEST(input_callbacks, callbacks_are_called_in_order)
{
    tb_input *const input = tb_input_create(NULL);
    calls made = {.player = tb_input_player_at(input, 0)};
    call_site first = {&made, 'a'};
    call_site second = {&made, 'b'};
    const tb_input_action jump = make_action(input, "jump", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");

    map(input, context, jump, TB_INPUT_KEY_SPACE);
    tb_input_add_context(made.player, context, 0);
    (void)tb_input_bind(made.player, jump, TB_INPUT_EVENT_ALL, record_call, &first);
    (void)tb_input_bind(made.player, jump, TB_INPUT_EVENT_COMPLETED | TB_INPUT_EVENT_STARTED,
                        record_call, &second);
    press(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    release(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    TB_TEST_REQUIRE_UINT(made.count, ==, 5);
    TB_TEST_EXPECT_UINT(made.events[0], ==, TB_INPUT_EVENT_STARTED);
    TB_TEST_EXPECT_INT(made.names[0], ==, 'a');
    TB_TEST_EXPECT_UINT(made.events[1], ==, TB_INPUT_EVENT_TRIGGERED);
    TB_TEST_EXPECT_INT(made.names[1], ==, 'a');
    TB_TEST_EXPECT_UINT(made.events[2], ==, TB_INPUT_EVENT_STARTED);
    TB_TEST_EXPECT_INT(made.names[2], ==, 'b');
    TB_TEST_EXPECT_UINT(made.events[3], ==, TB_INPUT_EVENT_COMPLETED);
    TB_TEST_EXPECT_INT(made.names[3], ==, 'a');
    TB_TEST_EXPECT_UINT(made.events[4], ==, TB_INPUT_EVENT_COMPLETED);
    TB_TEST_EXPECT_INT(made.names[4], ==, 'b');
    tb_input_destroy(input);
}

/* A callback may unbind, at once, and bind, from the next tick; and remove
 * a context, which ends its actions at the next tick. */
TB_TEST(input_callbacks, callbacks_may_bind_unbind_and_change_contexts)
{
    tb_input *const input = tb_input_create(NULL);
    calls made = {.player = tb_input_player_at(input, 0)};
    call_site first = {&made, 'a'};
    call_site second = {&made, 'b'};
    const tb_input_action jump = make_action(input, "jump", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");

    map(input, context, jump, TB_INPUT_KEY_SPACE);
    tb_input_add_context(made.player, context, 0);
    (void)tb_input_bind(made.player, jump, TB_INPUT_EVENT_TRIGGERED, record_call, &first);
    made.unbind = tb_input_bind(made.player, jump, TB_INPUT_EVENT_TRIGGERED, record_call, &second);
    made.bind = jump;
    press(input, TB_INPUT_KEY_SPACE);
    tb_input_tick(input, 0.1);
    /* "a" unbound "b" before it was called, and bound another "a". */
    TB_TEST_REQUIRE_UINT(made.count, ==, 1);
    made.context = context;
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(made.count, ==, 3);
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_events(made.player, jump), ==, TB_INPUT_EVENT_COMPLETED);
    /* The second "a" is bound to every event, so it saw the completion. */
    TB_TEST_EXPECT_UINT(made.count, ==, 4);
    TB_TEST_EXPECT_UINT(made.events[3], ==, TB_INPUT_EVENT_COMPLETED);
    tb_input_unbind(made.player, (tb_input_binding){0});
    tb_input_unbind(made.player, (tb_input_binding){1});
    tb_input_unbind(made.player, (tb_input_binding){1});
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Settings.
 * ------------------------------------------------------------------------- */

/* Following the engine's settings, a change in an options menu applies at
 * the next tick; setting values directly stops following. */
TB_TEST(input_settings, the_engine_settings_apply_at_the_next_tick)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_input *const input = tb_input_create(NULL);
    tb_input_settings read = {0};
    const tb_input_settings defaults = TB_INPUT_SETTINGS_DEFAULT;

    tb_engine_settings_add(settings, NULL, &engine);
    tb_input_get_settings(input, &read);
    TB_TEST_EXPECT(memcmp(&read, &defaults, sizeof(read)) == 0);
    tb_input_follow_settings(input, &engine);
    TB_TEST_REQUIRE_UINT(tb_setting_set_number(engine.controls.mouse_sensitivity, 2.5), ==,
                         TB_SETTING_OK);
    TB_TEST_REQUIRE_UINT(tb_setting_set_boolean(engine.controls.gamepad_invert_y, true), ==,
                         TB_SETTING_OK);
    TB_TEST_REQUIRE_UINT(tb_setting_set_number(engine.controls.gamepad_left_dead_zone, 0.3), ==,
                         TB_SETTING_OK);
    tb_input_get_settings(input, &read);
    TB_TEST_EXPECT_FLOAT(read.mouse_sensitivity, ==, 1.0f);
    tb_input_tick(input, 0.1);
    tb_input_get_settings(input, &read);
    TB_TEST_EXPECT_FLOAT(read.mouse_sensitivity, ==, 2.5f);
    TB_TEST_EXPECT(read.gamepad_invert_y);
    TB_TEST_EXPECT_FLOAT_NEAR(read.gamepad_left_dead_zone, 0.3f, 1e-6);
    TB_TEST_EXPECT_FLOAT(read.touch_sensitivity, ==, 1.0f);
    tb_input_set_settings(input, &defaults);
    tb_input_tick(input, 0.1);
    tb_input_get_settings(input, &read);
    TB_TEST_EXPECT_FLOAT(read.mouse_sensitivity, ==, 1.0f);
    tb_input_follow_settings(input, &engine);
    tb_input_follow_settings(input, NULL);
    tb_input_tick(input, 0.1);
    tb_input_get_settings(input, &read);
    TB_TEST_EXPECT_FLOAT(read.mouse_sensitivity, ==, 1.0f);
    tb_input_destroy(input);
    tb_settings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 * Definitions.
 * ------------------------------------------------------------------------- */

/* Actions and contexts are found by name and by place, in the order made. */
TB_TEST(input_definitions, actions_and_contexts_are_found)
{
    tb_input *const input = tb_input_create(NULL);
    const tb_input_action look = make_action(input, "look", TB_INPUT_VALUE_AXIS2D);
    const tb_input_context context = tb_input_create_context(input, "play");

    TB_TEST_EXPECT_UINT(tb_input_action_count(input), ==, 1);
    TB_TEST_EXPECT_UINT(tb_input_action_at(input, 0).id, ==, look.id);
    TB_TEST_EXPECT_UINT(tb_input_find_action(input, "look").id, ==, look.id);
    TB_TEST_EXPECT_UINT(tb_input_find_action(input, "jump").id, ==, 0);
    TB_TEST_EXPECT_STRING(tb_input_action_name(input, look), ==, "look");
    TB_TEST_EXPECT_INT(tb_input_action_type(input, look), ==, TB_INPUT_VALUE_AXIS2D);
    TB_TEST_EXPECT_UINT(tb_input_context_count(input), ==, 1);
    TB_TEST_EXPECT_UINT(tb_input_context_at(input, 0).id, ==, context.id);
    TB_TEST_EXPECT_UINT(tb_input_find_context(input, "play").id, ==, context.id);
    TB_TEST_EXPECT_UINT(tb_input_find_context(input, "menu").id, ==, 0);
    TB_TEST_EXPECT_STRING(tb_input_context_name(input, context), ==, "play");
    TB_TEST_EXPECT_UINT(tb_input_context_mapping_count(input, context), ==, 0);
    tb_input_destroy(NULL);
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Recording and replaying.
 * ------------------------------------------------------------------------- */

/* Definitions with something of every kind: a hold, a tap, a pulse, a
 * chord, smoothed and scaled look, cumulative movement. */
static void define(tb_input *input)
{
    const tb_input_action move = tb_input_create_action(
        input, &(tb_input_action_desc){.name = "move",
                                       .type = TB_INPUT_VALUE_AXIS2D,
                                       .accumulation = TB_INPUT_ACCUMULATE_CUMULATIVE});
    const tb_input_action look = make_action(input, "look", TB_INPUT_VALUE_AXIS2D);
    const tb_input_action fire = make_action(input, "fire", TB_INPUT_VALUE_BOOL);
    const tb_input_action charge = make_action(input, "charge", TB_INPUT_VALUE_BOOL);
    const tb_input_action dodge = make_action(input, "dodge", TB_INPUT_VALUE_BOOL);
    const tb_input_action sprint = make_action(input, "sprint", TB_INPUT_VALUE_BOOL);
    const tb_input_context context = tb_input_create_context(input, "play");
    const tb_input_modifier swizzle = {.type = TB_INPUT_MODIFIER_SWIZZLE};
    const tb_input_modifier look_modifiers[] = {
        {.type = TB_INPUT_MODIFIER_SETTINGS, .source = TB_INPUT_SETTINGS_MOUSE},
        {.type = TB_INPUT_MODIFIER_SMOOTH, .samples = 3}};
    const tb_input_trigger pulse = {.type = TB_INPUT_TRIGGER_PULSE, .seconds = 0.1};
    const tb_input_trigger hold = {.type = TB_INPUT_TRIGGER_HOLD, .seconds = 0.2};
    const tb_input_trigger dodge_triggers[] = {{.type = TB_INPUT_TRIGGER_TAP, .seconds = 0.15},
                                               {.type = TB_INPUT_TRIGGER_CHORDED, .chord = sprint}};

    tb_input_context_map(
        input, context,
        &(tb_input_mapping_desc){
            .action = move, .key = TB_INPUT_KEY_W, .modifiers = &swizzle, .modifier_count = 1});
    map(input, context, move, TB_INPUT_KEY_D);
    tb_input_context_map(input, context,
                         &(tb_input_mapping_desc){.action = look,
                                                  .key = TB_INPUT_KEY_MOUSE_XY,
                                                  .modifiers = look_modifiers,
                                                  .modifier_count = 2});
    tb_input_context_map(input, context,
                         &(tb_input_mapping_desc){.action = fire,
                                                  .key = TB_INPUT_KEY_MOUSE_LEFT,
                                                  .triggers = &pulse,
                                                  .trigger_count = 1});
    tb_input_context_map(input, context,
                         &(tb_input_mapping_desc){.action = charge,
                                                  .key = TB_INPUT_KEY_MOUSE_RIGHT,
                                                  .triggers = &hold,
                                                  .trigger_count = 1});
    tb_input_context_map(input, context,
                         &(tb_input_mapping_desc){.action = dodge,
                                                  .key = TB_INPUT_KEY_SPACE,
                                                  .triggers = dodge_triggers,
                                                  .trigger_count = 2});
    map(input, context, sprint, TB_INPUT_KEY_LEFT_SHIFT);
    tb_input_add_context(tb_input_player_at(input, 0), context, 0);
}

/* Every action's status after each tick, as one long trace. */
#define TRACE_TICKS 40u
#define TRACE_ACTIONS 6u

typedef struct trace
{
    tb_input_action_status statuses[TRACE_TICKS][TRACE_ACTIONS];
    size_t calls;
} trace;

/* Every field equal, exactly: a status has padding, so not memcmp. */
static bool same_status(const tb_input_action_status *left, const tb_input_action_status *right)
{
    return left->value.x == right->value.x && left->value.y == right->value.y &&
           left->value.z == right->value.z && left->state == right->state &&
           left->events == right->events && left->elapsed_seconds == right->elapsed_seconds &&
           left->triggered_seconds == right->triggered_seconds &&
           left->last_triggered_at == right->last_triggered_at;
}

static void count_call(void *user, const tb_input_action_event *event)
{
    trace *const traced = user;
    (void)event;
    traced->calls++;
}

static void take_trace(tb_input *input, trace *traced, size_t tick)
{
    for (size_t action = 0; action < TRACE_ACTIONS; action++)
    {
        tb_input_get_action_status(tb_input_player_at(input, 0), tb_input_action_at(input, action),
                                   &traced->statuses[tick][action]);
    }
}

static void bind_all(tb_input *input, trace *traced)
{
    for (size_t action = 0; action < TRACE_ACTIONS; action++)
    {
        (void)tb_input_bind(tb_input_player_at(input, 0), tb_input_action_at(input, action),
                            TB_INPUT_EVENT_ALL, count_call, traced);
    }
}

/* Input of a few seconds of play, as a window gives it, on ticks of
 * varying length: taps, holds, chords, the mouse, and a lost focus. */
static void play(tb_input *input, size_t tick)
{
    static const tb_key keys[] = {TB_KEY_W, TB_KEY_D, TB_KEY_SPACE, TB_KEY_LEFT_SHIFT};
    const tb_key key = keys[(tick / 2) % 4];
    const bool even = tick % 2 == 0;
    tb_window_event event = {.type = even ? TB_WINDOW_EVENT_KEY_DOWN : TB_WINDOW_EVENT_KEY_UP,
                             .key = key};

    (void)tb_input_feed_window_event(input, &event);
    if (tick % 3 == 0)
    {
        event = (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_SPACE};
        (void)tb_input_feed_window_event(input, &event);
        event.type = TB_WINDOW_EVENT_KEY_UP;
        (void)tb_input_feed_window_event(input, &event);
    }
    event = (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION,
                              .delta_x = (double)(tick % 5) - 2.0,
                              .delta_y = (double)(tick % 7) * 0.5};
    (void)tb_input_feed_window_event(input, &event);
    event = (tb_window_event){
        .type = tick % 8 < 5 ? TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN : TB_WINDOW_EVENT_MOUSE_BUTTON_UP,
        .button = tick % 16 < 8 ? TB_MOUSE_BUTTON_LEFT : TB_MOUSE_BUTTON_RIGHT};
    (void)tb_input_feed_window_event(input, &event);
    if (tick == 30)
    {
        event = (tb_window_event){.type = TB_WINDOW_EVENT_FOCUS_LOST};
        (void)tb_input_feed_window_event(input, &event);
    }
}

static double tick_length(size_t tick)
{
    return tick % 4 == 3 ? 0.125 : 0.0625;
}

/* A recording replayed into an input of the same definitions gives the
 * same statuses and callbacks on every tick, settings included, though the
 * settings changed while it was recorded and differ when it is replayed. */
TB_TEST(input_recording, a_replay_gives_the_same_actions)
{
    tb_input *const recorded = tb_input_create(NULL);
    tb_input *const replayed = tb_input_create(NULL);
    tb_input_recording *const recording = tb_input_recording_create();
    trace *const first = tb_calloc(1, sizeof(trace));
    trace *const second = tb_calloc(1, sizeof(trace));
    tb_input_settings settings = TB_INPUT_SETTINGS_DEFAULT;

    define(recorded);
    define(replayed);
    bind_all(recorded, first);
    bind_all(replayed, second);
    /* Input before recording is not recorded. */
    tb_input_feed_key(recorded, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_Q, pressed_value);
    tb_input_tick(recorded, 0.1);
    tb_input_tick(replayed, 0.1);
    tb_input_record(recorded, recording);
    for (size_t tick = 0; tick < TRACE_TICKS; tick++)
    {
        if (tick == 20)
        {
            settings.mouse_sensitivity = 3.0f;
            settings.mouse_smoothing = true;
            tb_input_set_settings(recorded, &settings);
        }
        play(recorded, tick);
        tb_input_tick(recorded, tick_length(tick));
        take_trace(recorded, first, tick);
    }
    tb_input_record(recorded, NULL);
    TB_TEST_REQUIRE_UINT(tb_input_recording_tick_count(recording), ==, TRACE_TICKS);
    for (size_t tick = 0; tick < TRACE_TICKS; tick++)
    {
        tb_input_replay_tick(replayed, recording, tick);
        take_trace(replayed, second, tick);
    }
    TB_TEST_EXPECT(first->calls > TRACE_TICKS);
    TB_TEST_EXPECT_UINT(second->calls, ==, first->calls);
    for (size_t tick = 0; tick < TRACE_TICKS; tick++)
    {
        for (size_t action = 0; action < TRACE_ACTIONS; action++)
        {
            TB_TEST_CONTEXT("tick %zu, action %zu", tick, action);
            TB_TEST_EXPECT(
                same_status(&first->statuses[tick][action], &second->statuses[tick][action]));
        }
    }
    TB_TEST_EXPECT_FLOAT(tb_input_seconds(replayed), ==, tb_input_seconds(recorded));

    /* Something of every action happened. */
    for (size_t action = 0; action < TRACE_ACTIONS; action++)
    {
        bool triggered = false;
        for (size_t tick = 0; tick < TRACE_TICKS; tick++)
        {
            triggered =
                triggered || (first->statuses[tick][action].events & TB_INPUT_EVENT_TRIGGERED) != 0;
        }
        TB_TEST_CONTEXT("action %zu", action);
        TB_TEST_EXPECT(triggered);
    }
    tb_free(first);
    tb_free(second);
    tb_input_recording_destroy(recording);
    tb_input_recording_destroy(NULL);
    tb_input_destroy(recorded);
    tb_input_destroy(replayed);
}
