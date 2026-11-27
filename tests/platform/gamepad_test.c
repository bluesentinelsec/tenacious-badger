/* What the gamepad interface does the same on every platform: names, a
 * stick or a trigger turned into buttons with hysteresis, and a system
 * opened on the machine's own gamepads, which CI has none of. */
#include "badger/assert.h"
#include "badger/gamepad.h"
#include "badger/str.h"

#include "tb_test.h"

#include <stddef.h>

TB_TEST(platform_gamepad, names_results_buttons_and_axes)
{
    TB_TEST_EXPECT_STRING(tb_gamepad_result_name(TB_GAMEPAD_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_gamepad_result_name(TB_GAMEPAD_UNAVAILABLE), ==, "unavailable");
    TB_TEST_EXPECT_STRING(tb_gamepad_result_name(TB_GAMEPAD_SYSTEM_ERROR), ==, "system error");
    TB_TEST_EXPECT_STRING(tb_gamepad_result_name((tb_gamepad_result)99), ==, "unknown result");
    TB_TEST_EXPECT_STRING(tb_gamepad_button_name(TB_GAMEPAD_BUTTON_SOUTH), ==, "south");
    TB_TEST_EXPECT_STRING(tb_gamepad_button_name(TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2), ==,
                          "right_paddle_2");
    TB_TEST_EXPECT_STRING(tb_gamepad_button_name(TB_GAMEPAD_BUTTON_COUNT), ==, "");
    TB_TEST_EXPECT_STRING(tb_gamepad_axis_name(TB_GAMEPAD_AXIS_LEFT_X), ==, "left_x");
    TB_TEST_EXPECT_STRING(tb_gamepad_axis_name(TB_GAMEPAD_AXIS_RIGHT_TRIGGER), ==, "right_trigger");
    TB_TEST_EXPECT_STRING(tb_gamepad_axis_name(TB_GAMEPAD_AXIS_COUNT), ==, "");
    for (size_t button = 0; button < TB_GAMEPAD_BUTTON_COUNT; button++)
    {
        TB_TEST_CONTEXT("button %zu", button);
        TB_TEST_EXPECT_UINT(tb_str_length(tb_gamepad_button_name((tb_gamepad_button)button)), >, 0);
    }
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
    {
        TB_TEST_CONTEXT("axis %zu", axis);
        TB_TEST_EXPECT_UINT(tb_str_length(tb_gamepad_axis_name((tb_gamepad_axis)axis)), >, 0);
    }
}

/* A pad's family by its USB IDs: by maker, and the Steam Deck by its own
 * product, not by Valve's other pads, which stand for any pad. */
TB_TEST(platform_gamepad, families_by_name_and_by_usb_ids)
{
    TB_TEST_EXPECT_STRING(tb_gamepad_family_name(TB_GAMEPAD_FAMILY_GENERIC), ==, "generic");
    TB_TEST_EXPECT_STRING(tb_gamepad_family_name(TB_GAMEPAD_FAMILY_STEAM_DECK), ==, "steam_deck");
    TB_TEST_EXPECT_STRING(tb_gamepad_family_name(TB_GAMEPAD_FAMILY_COUNT), ==, "");
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0x045e, 0x0b12), ==, TB_GAMEPAD_FAMILY_XBOX);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0x054c, 0x0df2), ==,
                       TB_GAMEPAD_FAMILY_PLAYSTATION);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0x057e, 0x2069), ==, TB_GAMEPAD_FAMILY_NINTENDO);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0x28de, 0x1205), ==,
                       TB_GAMEPAD_FAMILY_STEAM_DECK);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0x28de, 0x11ff), ==, TB_GAMEPAD_FAMILY_GENERIC);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0x2dc8, 0x3106), ==, TB_GAMEPAD_FAMILY_GENERIC);
    TB_TEST_EXPECT_INT(tb_gamepad_family_from_usb(0, 0), ==, TB_GAMEPAD_FAMILY_GENERIC);
}

TB_TEST(platform_gamepad, a_button_is_down_by_its_bit)
{
    const tb_gamepad_state state = {.connected = true,
                                    .buttons = (1u << TB_GAMEPAD_BUTTON_EAST) |
                                               (1u << TB_GAMEPAD_BUTTON_BACK)};

    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_EAST));
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_BACK));
    TB_TEST_EXPECT(!tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_SOUTH));
}

/* A value at the press threshold presses; one that falls back between the
 * thresholds stays down, so it does not chatter; one below the release
 * releases; and one that climbs back between them stays up. */
TB_TEST(platform_gamepad, analog_buttons_press_and_release_with_hysteresis)
{
    const tb_gamepad_threshold threshold = {0.5f, 0.4f};

    TB_TEST_EXPECT(!tb_gamepad_analog_button(0.49f, false, &threshold));
    TB_TEST_EXPECT(tb_gamepad_analog_button(0.5f, false, &threshold));
    TB_TEST_EXPECT(tb_gamepad_analog_button(0.45f, true, &threshold));
    TB_TEST_EXPECT(tb_gamepad_analog_button(0.4f, true, &threshold));
    TB_TEST_EXPECT(!tb_gamepad_analog_button(0.39f, true, &threshold));
    TB_TEST_EXPECT(!tb_gamepad_analog_button(0.45f, false, &threshold));
    TB_TEST_EXPECT(!tb_gamepad_analog_button(-1.0f, true, &threshold));
    TB_TEST_EXPECT(tb_gamepad_analog_button(1.0f, false, &threshold));
}

static void press_at_zero(void *context)
{
    const tb_gamepad_threshold threshold = {0.0f, 0.0f};

    (void)context;
    (void)tb_gamepad_analog_button(0.5f, false, &threshold);
}

static void release_above_press(void *context)
{
    const tb_gamepad_threshold threshold = {0.5f, 0.6f};

    (void)context;
    (void)tb_gamepad_analog_button(0.5f, false, &threshold);
}

TB_TEST(platform_gamepad, analog_buttons_refuse_broken_thresholds)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine library was built without assertions");
    }
    TB_TEST_EXPECT_TRAP(press_at_zero, NULL, "press");
    TB_TEST_EXPECT_TRAP(release_above_press, NULL, "release");
}

/* The defaults: a stick's direction at 0.5 and released below 0.4, a
 * trigger at 0.2 and released below 0.1; each direction from one axis, so a
 * diagonal presses two; nothing from a disconnected pad. */
TB_TEST(platform_gamepad, virtual_buttons_from_sticks_and_triggers)
{
    tb_gamepad_state state = {.connected = true};
    uint32_t down = 0;

    state.axes[TB_GAMEPAD_AXIS_LEFT_Y] = 0.6f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = -0.6f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_X] = 0.7f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_Y] = -0.8f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.2f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.15f;
    down = tb_gamepad_virtual_buttons(&state, 0, NULL);
    TB_TEST_EXPECT_HEX(
        down, ==,
        (1u << TB_GAMEPAD_VIRTUAL_LEFT_STICK_UP) | (1u << TB_GAMEPAD_VIRTUAL_LEFT_STICK_LEFT) |
            (1u << TB_GAMEPAD_VIRTUAL_RIGHT_STICK_RIGHT) |
            (1u << TB_GAMEPAD_VIRTUAL_RIGHT_STICK_DOWN) | (1u << TB_GAMEPAD_VIRTUAL_RIGHT_TRIGGER));

    /* Between the thresholds: what was down stays down, what was up stays
     * up. */
    state.axes[TB_GAMEPAD_AXIS_LEFT_Y] = 0.45f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = -0.3f;
    state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.12f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.12f;
    down = tb_gamepad_virtual_buttons(&state, down, NULL);
    TB_TEST_EXPECT(down & (1u << TB_GAMEPAD_VIRTUAL_LEFT_STICK_UP));
    TB_TEST_EXPECT(!(down & (1u << TB_GAMEPAD_VIRTUAL_LEFT_STICK_LEFT)));
    TB_TEST_EXPECT(down & (1u << TB_GAMEPAD_VIRTUAL_RIGHT_TRIGGER));
    TB_TEST_EXPECT(!(down & (1u << TB_GAMEPAD_VIRTUAL_LEFT_TRIGGER)));

    state.connected = false;
    TB_TEST_EXPECT_HEX(tb_gamepad_virtual_buttons(&state, down, NULL), ==, 0);
}

TB_TEST(platform_gamepad, virtual_buttons_take_thresholds_of_their_own)
{
    const tb_gamepad_virtual_thresholds thresholds = {{0.9f, 0.8f}, {0.05f, 0.0f}};
    tb_gamepad_state state = {.connected = true};

    state.axes[TB_GAMEPAD_AXIS_LEFT_X] = 0.85f;
    state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] = 0.05f;
    TB_TEST_EXPECT_HEX(tb_gamepad_virtual_buttons(&state, 0, &thresholds), ==,
                       1u << TB_GAMEPAD_VIRTUAL_LEFT_TRIGGER);
}

/* The machine's own gamepads: open, a poll, every slot read, and closed.
 * Where the platform's library is missing the system is unavailable, which
 * is a capability and not a failure. */
TB_TEST(platform_gamepad, a_system_opens_polls_and_closes)
{
    tb_gamepad_system *system = NULL;
    tb_gamepad_error error = TB_GAMEPAD_ERROR_INIT;
    tb_gamepad_event event = {0};
    const tb_gamepad_result result = tb_gamepad_system_open(&system, &error);

    if (result == TB_GAMEPAD_UNAVAILABLE)
    {
        TB_TEST_SKIP("no gamepads here: %s", error.message);
    }
    TB_TEST_REQUIRE_INT(result, ==, TB_GAMEPAD_OK);
    TB_TEST_REQUIRE(system != NULL);
    tb_gamepad_system_set_vibration(system, 0.0f);
    tb_gamepad_system_poll(system);
    while (tb_gamepad_system_next_event(system, &event))
    {
        TB_TEST_EXPECT_UINT(event.slot, <, TB_GAMEPAD_SLOT_COUNT);
    }
    for (size_t slot = 0; slot < TB_GAMEPAD_SLOT_COUNT; slot++)
    {
        tb_gamepad_state state = {0};
        tb_gamepad_info info = {0};
        tb_gamepad_get_state(system, slot, &state);
        tb_gamepad_get_info(system, slot, &info);
        TB_TEST_CONTEXT("slot %zu", slot);
        TB_TEST_EXPECT(state.connected == (info.name[0] != '\0'));
        for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
        {
            TB_TEST_EXPECT_FLOAT(state.axes[axis], >=, -1.0f);
            TB_TEST_EXPECT_FLOAT(state.axes[axis], <=, 1.0f);
        }
    }
    tb_gamepad_system_set_focus(system, false);
    tb_gamepad_system_set_focus(system, true);
    tb_gamepad_system_close(system);
    tb_gamepad_system_close(NULL);
}
