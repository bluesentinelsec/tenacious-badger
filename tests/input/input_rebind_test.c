/* Rebinding: a player's mappings bound to other keys, or none, apart from
 * other players and from the defaults; conflicts within a context, by the
 * keys' values; the key a prompt shows for an action; and listening for the
 * next key a player presses or moves, which must never take the press that
 * started it nor let the key it captures act. */
#include "badger/input.h"

#include "tb_test.h"

#include <stddef.h>

static const tb_input_value down = {1.0f, 0.0f, 0.0f};
static const tb_input_value up = {0.0f, 0.0f, 0.0f};
static const tb_input_device pad = TB_INPUT_DEVICE_GAMEPAD(0);

/* A game of two players: "jump" on Space and the pad's south, "fire" on
 * the left mouse button, and "move" on the left stick, in "gameplay". */
typedef struct game
{
    tb_input *input;
    tb_input_player *player;
    tb_input_action jump;
    tb_input_action fire;
    tb_input_action move;
    tb_input_context gameplay;
} game;

static game make_game(void)
{
    const tb_input_options options = {.player_count = 2};
    game made = {.input = tb_input_create(&options)};

    made.player = tb_input_player_at(made.input, 0);
    made.jump = tb_input_create_action(
        made.input, &(tb_input_action_desc){.name = "jump", .type = TB_INPUT_VALUE_BOOL});
    made.fire = tb_input_create_action(
        made.input, &(tb_input_action_desc){.name = "fire", .type = TB_INPUT_VALUE_BOOL});
    made.move = tb_input_create_action(
        made.input, &(tb_input_action_desc){.name = "move", .type = TB_INPUT_VALUE_AXIS2D});
    made.gameplay = tb_input_create_context(made.input, "gameplay");
    tb_input_context_map(made.input, made.gameplay,
                         &(tb_input_mapping_desc){.action = made.jump, .key = TB_INPUT_KEY_SPACE});
    tb_input_context_map(
        made.input, made.gameplay,
        &(tb_input_mapping_desc){.action = made.jump, .key = TB_INPUT_KEY_GAMEPAD_FACE_SOUTH});
    tb_input_context_map(
        made.input, made.gameplay,
        &(tb_input_mapping_desc){.action = made.fire, .key = TB_INPUT_KEY_MOUSE_LEFT});
    tb_input_context_map(
        made.input, made.gameplay,
        &(tb_input_mapping_desc){.action = made.move, .key = TB_INPUT_KEY_GAMEPAD_LEFT_STICK});
    tb_input_add_context(made.player, made.gameplay, 0);
    tb_input_assign_device(made.input, pad, 0);
    return made;
}

static void press(tb_input *input, tb_input_device device, tb_input_key key)
{
    tb_input_feed_key(input, device, key, down);
}

static void release(tb_input *input, tb_input_device device, tb_input_key key)
{
    tb_input_feed_key(input, device, key, up);
}

TB_TEST(input_rebind, mappings_are_listed_as_defined)
{
    game g = make_game();
    tb_input_mapping_info info = {0};

    tb_input_context_mapping_at(g.input, g.gameplay, 2, &info);
    TB_TEST_EXPECT_UINT(info.action.id, ==, g.fire.id);
    TB_TEST_EXPECT_UINT(info.key, ==, TB_INPUT_KEY_MOUSE_LEFT);
    TB_TEST_EXPECT(!info.pass_through);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(g.player, g.gameplay, 2), ==, TB_INPUT_KEY_MOUSE_LEFT);
    tb_input_destroy(g.input);
}

/* A rebind moves an action to another key for one player, and back to its
 * default forgets it; the other player keeps the default. */
TB_TEST(input_rebind, a_rebind_moves_a_mapping_for_one_player)
{
    game g = make_game();
    tb_input_player *const other = tb_input_player_at(g.input, 1);

    tb_input_rebind(g.player, g.gameplay, 0, TB_INPUT_KEY_F);
    TB_TEST_EXPECT(tb_input_is_rebound(g.player, g.gameplay, 0));
    TB_TEST_EXPECT(!tb_input_is_rebound(other, g.gameplay, 0));
    TB_TEST_EXPECT_UINT(tb_input_binding_key(g.player, g.gameplay, 0), ==, TB_INPUT_KEY_F);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(other, g.gameplay, 0), ==, TB_INPUT_KEY_SPACE);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(g.player), ==, 1);

    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.jump));
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_F);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.jump));
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_F);

    /* Rebinding to the default is no rebind. */
    tb_input_rebind(g.player, g.gameplay, 0, TB_INPUT_KEY_SPACE);
    TB_TEST_EXPECT(!tb_input_is_rebound(g.player, g.gameplay, 0));
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(g.player), ==, 0);
    tb_input_destroy(g.input);
}

/* Unbound, a mapping sees no key and consumes none: a lower context sees
 * the key again. */
TB_TEST(input_rebind, an_unbound_mapping_sees_and_consumes_nothing)
{
    game g = make_game();
    const tb_input_context menu = tb_input_create_context(g.input, "menu");

    tb_input_context_map(g.input, menu,
                         &(tb_input_mapping_desc){.action = g.fire, .key = TB_INPUT_KEY_SPACE});
    tb_input_add_context(g.player, menu, 1);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.fire));
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.jump));

    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    tb_input_rebind(g.player, menu, 0, TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(g.player, menu, 0), ==, TB_INPUT_KEY_NONE);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.fire));
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.jump));
    tb_input_destroy(g.input);
}

/* A key held as it is bound acts only once released and pressed again, so
 * that the press that chose it does not jump. */
TB_TEST(input_rebind, a_key_held_when_bound_waits_for_its_release)
{
    game g = make_game();

    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_G);
    tb_input_tick(g.input, 0.1);
    tb_input_rebind(g.player, g.gameplay, 0, TB_INPUT_KEY_G);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.jump));
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_G);
    tb_input_tick(g.input, 0.1);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_G);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.jump));
    tb_input_destroy(g.input);
}

TB_TEST(input_rebind, resets_restore_the_defaults)
{
    game g = make_game();

    tb_input_rebind(g.player, g.gameplay, 2, TB_INPUT_KEY_MOUSE_RIGHT);
    tb_input_rebind(g.player, g.gameplay, 0, TB_INPUT_KEY_E);
    tb_input_rebind(g.player, g.gameplay, 1, TB_INPUT_KEY_GAMEPAD_FACE_EAST);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(g.player), ==, 3);
    tb_input_reset_binding(g.player, g.gameplay, 0);
    tb_input_reset_binding(g.player, g.gameplay, 0);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(g.player, g.gameplay, 0), ==, TB_INPUT_KEY_SPACE);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(g.player, g.gameplay, 2), ==,
                        TB_INPUT_KEY_MOUSE_RIGHT);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(g.player), ==, 2);
    tb_input_reset_bindings(g.player);
    tb_input_reset_bindings(g.player);
    TB_TEST_EXPECT_UINT(tb_input_rebind_count(g.player), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_binding_key(g.player, g.gameplay, 1), ==,
                        TB_INPUT_KEY_GAMEPAD_FACE_SOUTH);
    tb_input_destroy(g.input);
}

/* Conflicts are mappings bound to the same key, or to keys that share a
 * value: the left stick with its X and with its directions as buttons. */
TB_TEST(input_rebind, conflicts_are_keys_that_share_a_value)
{
    game g = make_game();
    size_t conflicts[4] = {0};

    TB_TEST_EXPECT_UINT(
        tb_input_find_conflicts(g.player, g.gameplay, 2, TB_INPUT_KEY_SPACE, conflicts, 4), ==, 1);
    TB_TEST_EXPECT_UINT(conflicts[0], ==, 0);
    /* The mapping rebound is left out. */
    TB_TEST_EXPECT_UINT(
        tb_input_find_conflicts(g.player, g.gameplay, 0, TB_INPUT_KEY_SPACE, conflicts, 4), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_GAMEPAD_LEFT_STICK_X, conflicts, 4),
                        ==, 1);
    TB_TEST_EXPECT_UINT(conflicts[0], ==, 3);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP, NULL, 0),
                        ==, 1);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_GAMEPAD_RIGHT_STICK, NULL, 0),
                        ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_NONE, NULL, 0),
                        ==, 0);

    /* As the player has them bound: two on F, and more than room for. */
    tb_input_rebind(g.player, g.gameplay, 0, TB_INPUT_KEY_F);
    tb_input_rebind(g.player, g.gameplay, 2, TB_INPUT_KEY_F);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_F, conflicts, 1),
                        ==, 2);
    TB_TEST_EXPECT_UINT(conflicts[0], ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_SPACE, NULL, 0),
                        ==, 0);

    /* The wheel and its buttons share a value too. */
    tb_input_rebind(g.player, g.gameplay, 2, TB_INPUT_KEY_MOUSE_WHEEL_UP);
    TB_TEST_EXPECT_UINT(tb_input_find_conflicts(g.player, g.gameplay, TB_INPUT_NO_MAPPING,
                                                TB_INPUT_KEY_MOUSE_WHEEL, NULL, 0),
                        ==, 1);
    tb_input_destroy(g.input);
}

/* A prompt's key for an action: the first of the classes asked for, as the
 * player has it bound. */
TB_TEST(input_rebind, a_prompt_finds_an_actions_key_of_a_class)
{
    game g = make_game();

    TB_TEST_EXPECT_UINT(tb_input_find_binding(g.player, g.gameplay, g.jump, 0), ==,
                        TB_INPUT_KEY_SPACE);
    TB_TEST_EXPECT_UINT(tb_input_find_binding(g.player, g.gameplay, g.jump, TB_INPUT_CLASS_GAMEPAD),
                        ==, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH);
    TB_TEST_EXPECT_UINT(tb_input_find_binding(g.player, g.gameplay, g.fire, TB_INPUT_CLASS_GAMEPAD),
                        ==, TB_INPUT_KEY_NONE);
    tb_input_rebind(g.player, g.gameplay, 1, TB_INPUT_KEY_GAMEPAD_FACE_NORTH);
    TB_TEST_EXPECT_UINT(tb_input_find_binding(g.player, g.gameplay, g.jump, TB_INPUT_CLASS_GAMEPAD),
                        ==, TB_INPUT_KEY_GAMEPAD_FACE_NORTH);
    tb_input_destroy(g.input);
}

TB_TEST(input_rebind, keys_have_classes)
{
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_NONE), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_COUNT), ==, 0);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_DELETE), ==, TB_INPUT_CLASS_KEYBOARD);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_MOUSE_THUMB_2), ==,
                        TB_INPUT_CLASS_MOUSE_BUTTONS);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_MOUSE_WHEEL_UP), ==,
                        TB_INPUT_CLASS_MOUSE_AXES);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_MOUSE_XY), ==,
                        TB_INPUT_CLASS_MOUSE_AXES);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_GAMEPAD_GUIDE), ==,
                        TB_INPUT_CLASS_GAMEPAD_BUTTONS);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER), ==,
                        TB_INPUT_CLASS_GAMEPAD_AXES);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_LEFT), ==,
                        TB_INPUT_CLASS_GAMEPAD_AXES);
    TB_TEST_EXPECT_UINT(tb_input_key_class_of(TB_INPUT_KEY_TOUCH_10_POSITION), ==,
                        TB_INPUT_CLASS_TOUCH);
}

TB_TEST(input_rebind, the_last_device_is_the_one_that_gave_a_key)
{
    game g = make_game();
    tb_gamepad_state state = {.connected = true};

    TB_TEST_EXPECT_UINT(tb_input_player_last_device(g.player), ==, TB_INPUT_DEVICE_NONE);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_W);
    TB_TEST_EXPECT_UINT(tb_input_player_last_device(g.player), ==, TB_INPUT_DEVICE_KEYBOARD);
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = 0.5f;
    tb_input_feed_gamepad(g.input, pad, &state);
    TB_TEST_EXPECT_UINT(tb_input_player_last_device(g.player), ==, pad);
    /* A release is not a use. */
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_W);
    TB_TEST_EXPECT_UINT(tb_input_player_last_device(g.player), ==, pad);
    tb_input_destroy(g.input);
}

/* ---------------------------------------------------------------------------
 * Listening.
 * ------------------------------------------------------------------------- */

static tb_input_capture capture_of(const tb_input *input)
{
    tb_input_capture capture = {0};
    (void)tb_input_listen_result(input, &capture);
    return capture;
}

/* The next key pressed is captured, with its device and player; the
 * listener stops, and is idle again once stopped. */
TB_TEST(input_rebind, listening_captures_the_next_key)
{
    game g = make_game();

    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_IDLE);
    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_WAITING);
    press(g.input, pad, TB_INPUT_KEY_GAMEPAD_FACE_WEST);
    tb_input_tick(g.input, 0.1);
    const tb_input_capture captured = capture_of(g.input);
    TB_TEST_EXPECT_UINT(captured.state, ==, TB_INPUT_LISTEN_CAPTURED);
    TB_TEST_EXPECT_UINT(captured.key, ==, TB_INPUT_KEY_GAMEPAD_FACE_WEST);
    TB_TEST_EXPECT_UINT(captured.device, ==, pad);
    TB_TEST_EXPECT_UINT(captured.player, ==, 0);
    /* Later keys change nothing. */
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_Q);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_GAMEPAD_FACE_WEST);
    tb_input_stop_listening(g.input);
    TB_TEST_EXPECT_UINT(capture_of(g.input).state, ==, TB_INPUT_LISTEN_IDLE);
    TB_TEST_EXPECT_UINT(capture_of(g.input).player, ==, TB_INPUT_NO_PLAYER);
    tb_input_destroy(g.input);
}

/* The press that started listening, still held, is not captured; pressed
 * again after its release, it is. */
TB_TEST(input_rebind, a_key_held_when_listening_begins_is_not_captured)
{
    game g = make_game();

    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ENTER);
    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    tb_input_tick(g.input, 0.1);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_WAITING);
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ENTER);
    tb_input_tick(g.input, 0.1);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ENTER);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_ENTER);
    tb_input_destroy(g.input);
}

/* Escape and the pad's start cancel by default, ahead of any other key of
 * the tick; a listener's own cancel keys replace them, and none may be
 * given. */
TB_TEST(input_rebind, cancel_keys_cancel)
{
    game g = make_game();
    static const tb_input_key backspace = TB_INPUT_KEY_BACKSPACE;

    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_A);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ESCAPE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).state, ==, TB_INPUT_LISTEN_CANCELED);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_NONE);
    TB_TEST_EXPECT_UINT(capture_of(g.input).device, ==, TB_INPUT_DEVICE_KEYBOARD);
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_A);
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ESCAPE);
    tb_input_tick(g.input, 0.1);

    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    press(g.input, pad, TB_INPUT_KEY_GAMEPAD_START);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).state, ==, TB_INPUT_LISTEN_CANCELED);
    release(g.input, pad, TB_INPUT_KEY_GAMEPAD_START);
    tb_input_tick(g.input, 0.1);

    tb_input_listen(g.input,
                    &(tb_input_listen_desc){.cancel_keys = &backspace, .cancel_key_count = 1});
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ESCAPE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_ESCAPE);
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ESCAPE);
    tb_input_tick(g.input, 0.1);

    tb_input_listen(g.input, &(tb_input_listen_desc){.cancel_keys = &backspace});
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_BACKSPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_BACKSPACE);
    tb_input_destroy(g.input);
}

/* Only the classes accepted, and only the player listened to, are
 * captured. */
TB_TEST(input_rebind, listening_takes_only_the_classes_and_player_asked_for)
{
    game g = make_game();
    tb_input_player *const other = tb_input_player_at(g.input, 1);
    const tb_input_device second_pad = TB_INPUT_DEVICE_GAMEPAD(1);

    tb_input_assign_device(g.input, second_pad, 1);
    tb_input_listen(g.input, &(tb_input_listen_desc){.player = other,
                                                     .classes = TB_INPUT_CLASS_GAMEPAD_BUTTONS});
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_A);
    press(g.input, pad, TB_INPUT_KEY_GAMEPAD_FACE_SOUTH);
    press(g.input, second_pad, TB_INPUT_KEY_GAMEPAD_DPAD_UP);
    tb_input_tick(g.input, 0.1);
    /* The first player was not listened to: its jump acts. */
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.jump));
    const tb_input_capture captured = capture_of(g.input);
    TB_TEST_EXPECT_UINT(captured.key, ==, TB_INPUT_KEY_GAMEPAD_DPAD_UP);
    TB_TEST_EXPECT_UINT(captured.player, ==, 1);
    TB_TEST_EXPECT_UINT(captured.device, ==, second_pad);
    tb_input_destroy(g.input);
}

/* While listening, the player's actions have nothing; after, the key
 * captured acts only once pressed again. */
TB_TEST(input_rebind, the_key_captured_does_not_act)
{
    game g = make_game();

    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.jump));
    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.jump));
    TB_TEST_EXPECT_UINT(tb_input_action_events(g.player, g.jump), ==, TB_INPUT_EVENT_COMPLETED);
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_SPACE);
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.jump));
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(!tb_input_action_active(g.player, g.jump));
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT(tb_input_action_active(g.player, g.jump));
    tb_input_destroy(g.input);
}

static void set_axis(tb_input *input, tb_input_key key, float value)
{
    tb_input_feed_key(input, pad, key, (tb_input_value){value, 0.0f, 0.0f});
}

/* A bool listener takes a stick pushed as its direction, and a trigger as
 * its button; an axis listener takes the axis, past its threshold; a 2D
 * listener takes the stick. */
TB_TEST(input_rebind, sticks_and_triggers_are_captured_as_the_listener_needs)
{
    game g = make_game();

    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y, -0.8f);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_DOWN);
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_LEFT_STICK_Y, 0.0f);

    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.3f);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER_BUTTON);
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.0f);

    tb_input_listen(g.input,
                    &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS1D, .threshold = 0.7f});
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.6f);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_WAITING);
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER, 0.8f);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_GAMEPAD_RIGHT_TRIGGER);

    /* The trigger is still away from rest: it counts once back. */
    tb_input_listen(g.input, &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS2D});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_WAITING);
    set_axis(g.input, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_X, 0.9f);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_GAMEPAD_RIGHT_STICK);
    tb_input_destroy(g.input);
}

/* The mouse is captured once it has moved far enough, as X or Y alone or
 * both; the wheel as its buttons or its axis. */
TB_TEST(input_rebind, the_mouse_is_captured_by_how_far_it_moves)
{
    game g = make_game();

    tb_input_listen(g.input,
                    &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS1D, .distance = 10.0f});
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_XY,
                      (tb_input_value){2.0f, 6.0f, 0.0f});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_WAITING);
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_XY,
                      (tb_input_value){2.0f, -6.0f, 0.0f});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_MOUSE_Y);
    TB_TEST_EXPECT_UINT(capture_of(g.input).device, ==, TB_INPUT_DEVICE_MOUSE);

    tb_input_listen(g.input, &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS2D});
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_XY,
                      (tb_input_value){40.0f, 0.0f, 0.0f});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_MOUSE_XY);

    /* A bool listener does not take motion; it takes the wheel's buttons. */
    tb_input_listen(g.input, &(tb_input_listen_desc){0});
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_XY,
                      (tb_input_value){400.0f, 0.0f, 0.0f});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_listen_result(g.input, NULL), ==, TB_INPUT_LISTEN_WAITING);
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL_DOWN, down);
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL,
                      (tb_input_value){-1.0f, 0.0f, 0.0f});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_MOUSE_WHEEL_DOWN);

    tb_input_listen(g.input, &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS1D});
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL_DOWN, down);
    tb_input_feed_key(g.input, TB_INPUT_DEVICE_MOUSE, TB_INPUT_KEY_MOUSE_WHEEL,
                      (tb_input_value){-1.0f, 0.0f, 0.0f});
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_MOUSE_WHEEL);
    tb_input_destroy(g.input);
}

/* A touch is captured as its button, or as its position for a 2D
 * listener; a button stays a button for an axis listener. */
TB_TEST(input_rebind, touches_and_buttons_for_axes)
{
    game g = make_game();
    const tb_input_device screen = TB_INPUT_DEVICE_FIRST_OTHER + 40;

    tb_input_assign_device(g.input, screen, 0);
    tb_input_listen(g.input, &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS2D,
                                                     .classes = TB_INPUT_CLASS_TOUCH});
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_W);
    tb_input_feed_key(g.input, screen, TB_INPUT_KEY_TOUCH_3, down);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_TOUCH_3_POSITION);
    TB_TEST_EXPECT_UINT(capture_of(g.input).device, ==, screen);
    tb_input_feed_key(g.input, screen, TB_INPUT_KEY_TOUCH_3, up);
    release(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_W);
    tb_input_tick(g.input, 0.1);

    tb_input_listen(g.input, &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS1D});
    press(g.input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_W);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_W);

    tb_input_listen(g.input, &(tb_input_listen_desc){.type = TB_INPUT_VALUE_AXIS1D});
    tb_input_feed_key(g.input, screen, TB_INPUT_KEY_TOUCH_1, down);
    tb_input_tick(g.input, 0.1);
    TB_TEST_EXPECT_UINT(capture_of(g.input).key, ==, TB_INPUT_KEY_TOUCH_1);
    tb_input_destroy(g.input);
}

/* ---------------------------------------------------------------------------
 * Prompts.
 * ------------------------------------------------------------------------- */

static void expect_prompt(tb_input_key key, tb_gamepad_family family, const char *label,
                          const char *glyph)
{
    tb_input_prompt prompt = {0};

    tb_input_key_prompt(key, family, &prompt);
    TB_TEST_CONTEXT("%s for %s", tb_input_key_name(key), tb_gamepad_family_name(family));
    TB_TEST_EXPECT_STRING(prompt.label, ==, label);
    TB_TEST_EXPECT_STRING(prompt.glyph, ==, glyph);
}

/* A gamepad's key is shown by the label its family prints at the place:
 * Nintendo's A is on the right, so south is B. */
TB_TEST(input_rebind, prompts_show_each_familys_labels)
{
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, TB_GAMEPAD_FAMILY_XBOX, "A", "xbox/a");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, TB_GAMEPAD_FAMILY_PLAYSTATION, "Cross",
                  "playstation/cross");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, TB_GAMEPAD_FAMILY_NINTENDO, "B", "nintendo/b");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_EAST, TB_GAMEPAD_FAMILY_NINTENDO, "A", "nintendo/a");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_NORTH, TB_GAMEPAD_FAMILY_NINTENDO, "X", "nintendo/x");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, TB_GAMEPAD_FAMILY_STEAM_DECK, "A",
                  "steam_deck/a");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_FACE_SOUTH, TB_GAMEPAD_FAMILY_GENERIC, "A", "generic/south");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER, TB_GAMEPAD_FAMILY_NINTENDO, "ZL",
                  "nintendo/zl");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_LEFT_TRIGGER_BUTTON, TB_GAMEPAD_FAMILY_PLAYSTATION, "L2",
                  "playstation/l2");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_BACK, TB_GAMEPAD_FAMILY_PLAYSTATION, "Create",
                  "playstation/create");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_START, TB_GAMEPAD_FAMILY_NINTENDO, "+", "nintendo/plus");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_LEFT_PADDLE_2, TB_GAMEPAD_FAMILY_STEAM_DECK, "L5",
                  "steam_deck/l5");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_RIGHT_PADDLE_1, TB_GAMEPAD_FAMILY_XBOX, "P1", "xbox/p1");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_GUIDE, TB_GAMEPAD_FAMILY_STEAM_DECK, "Steam",
                  "steam_deck/steam");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_LEFT_STICK_UP, TB_GAMEPAD_FAMILY_XBOX, "Left Stick Up",
                  "xbox/left_stick_up");
    expect_prompt(TB_INPUT_KEY_GAMEPAD_RIGHT_STICK_CLICK, TB_GAMEPAD_FAMILY_PLAYSTATION, "R3",
                  "playstation/r3");
    /* A value that is no family is generic. */
    expect_prompt(TB_INPUT_KEY_GAMEPAD_DPAD_UP, TB_GAMEPAD_FAMILY_COUNT, "D-pad Up",
                  "generic/dpad_up");
}

/* The keyboard's, the mouse's and touches are the same for every family;
 * no key has an empty prompt. */
TB_TEST(input_rebind, prompts_of_keyboards_mice_and_touches)
{
    expect_prompt(TB_INPUT_KEY_SPACE, TB_GAMEPAD_FAMILY_XBOX, "Space", "keyboard/space");
    expect_prompt(TB_INPUT_KEY_LEFT_SHIFT, TB_GAMEPAD_FAMILY_NINTENDO, "Left Shift",
                  "keyboard/left_shift");
    expect_prompt(TB_INPUT_KEY_7, TB_GAMEPAD_FAMILY_GENERIC, "7", "keyboard/7");
    expect_prompt(TB_INPUT_KEY_MOUSE_LEFT, TB_GAMEPAD_FAMILY_GENERIC, "Left Mouse Button",
                  "mouse/left");
    expect_prompt(TB_INPUT_KEY_MOUSE_XY, TB_GAMEPAD_FAMILY_GENERIC, "Mouse", "mouse/xy");
    expect_prompt(TB_INPUT_KEY_TOUCH_2, TB_GAMEPAD_FAMILY_GENERIC, "Touch 2", "touch/2");
    expect_prompt(TB_INPUT_KEY_TOUCH_10_POSITION, TB_GAMEPAD_FAMILY_GENERIC, "Touch 10 Position",
                  "touch/10_position");
    expect_prompt(TB_INPUT_KEY_NONE, TB_GAMEPAD_FAMILY_GENERIC, "", "");
    expect_prompt(TB_INPUT_KEY_COUNT, TB_GAMEPAD_FAMILY_GENERIC, "", "");
    for (size_t family = 0; family < TB_GAMEPAD_FAMILY_COUNT; family++)
    {
        for (size_t key = 1; key < TB_INPUT_KEY_COUNT; key++)
        {
            tb_input_prompt prompt = {0};
            tb_input_key_prompt((tb_input_key)key, (tb_gamepad_family)family, &prompt);
            TB_TEST_CONTEXT("%s for %s", tb_input_key_name((tb_input_key)key),
                            tb_gamepad_family_name((tb_gamepad_family)family));
            TB_TEST_REQUIRE(prompt.label != NULL && prompt.glyph != NULL);
            TB_TEST_EXPECT(prompt.label[0] != '\0');
            TB_TEST_EXPECT(prompt.glyph[0] != '\0');
        }
    }
}
