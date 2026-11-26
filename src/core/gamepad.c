/* What the gamepad interface does the same on every platform, and so needs
 * nothing of one: names, and buttons made from analog values. */
#include "badger/gamepad.h"

#include "badger/assert.h"

#include <math.h>

const char *tb_gamepad_result_name(tb_gamepad_result result)
{
    switch (result)
    {
    case TB_GAMEPAD_OK:
        return "ok";
    case TB_GAMEPAD_UNAVAILABLE:
        return "unavailable";
    case TB_GAMEPAD_SYSTEM_ERROR:
        return "system error";
    }
    return "unknown result";
}

static const char *const tb_gamepad_button_names[TB_GAMEPAD_BUTTON_COUNT] = {
    [TB_GAMEPAD_BUTTON_SOUTH] = "south",
    [TB_GAMEPAD_BUTTON_EAST] = "east",
    [TB_GAMEPAD_BUTTON_WEST] = "west",
    [TB_GAMEPAD_BUTTON_NORTH] = "north",
    [TB_GAMEPAD_BUTTON_LEFT_SHOULDER] = "left_shoulder",
    [TB_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "right_shoulder",
    [TB_GAMEPAD_BUTTON_LEFT_STICK] = "left_stick",
    [TB_GAMEPAD_BUTTON_RIGHT_STICK] = "right_stick",
    [TB_GAMEPAD_BUTTON_DPAD_UP] = "dpad_up",
    [TB_GAMEPAD_BUTTON_DPAD_DOWN] = "dpad_down",
    [TB_GAMEPAD_BUTTON_DPAD_LEFT] = "dpad_left",
    [TB_GAMEPAD_BUTTON_DPAD_RIGHT] = "dpad_right",
    [TB_GAMEPAD_BUTTON_START] = "start",
    [TB_GAMEPAD_BUTTON_BACK] = "back",
    [TB_GAMEPAD_BUTTON_LEFT_PADDLE_1] = "left_paddle_1",
    [TB_GAMEPAD_BUTTON_LEFT_PADDLE_2] = "left_paddle_2",
    [TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1] = "right_paddle_1",
    [TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2] = "right_paddle_2",
};

static const char *const tb_gamepad_axis_names[TB_GAMEPAD_AXIS_COUNT] = {
    [TB_GAMEPAD_AXIS_LEFT_X] = "left_x",
    [TB_GAMEPAD_AXIS_LEFT_Y] = "left_y",
    [TB_GAMEPAD_AXIS_RIGHT_X] = "right_x",
    [TB_GAMEPAD_AXIS_RIGHT_Y] = "right_y",
    [TB_GAMEPAD_AXIS_LEFT_TRIGGER] = "left_trigger",
    [TB_GAMEPAD_AXIS_RIGHT_TRIGGER] = "right_trigger",
};

TB_STATIC_ASSERT(TB_GAMEPAD_BUTTON_COUNT <= 32, "every button has a bit of a state's buttons");
TB_STATIC_ASSERT(TB_GAMEPAD_VIRTUAL_COUNT <= 32, "every virtual button has a bit");

const char *tb_gamepad_button_name(tb_gamepad_button button)
{
    if ((unsigned)button >= (unsigned)TB_GAMEPAD_BUTTON_COUNT)
    {
        return "";
    }
    return tb_gamepad_button_names[button];
}

const char *tb_gamepad_axis_name(tb_gamepad_axis axis)
{
    if ((unsigned)axis >= (unsigned)TB_GAMEPAD_AXIS_COUNT)
    {
        return "";
    }
    return tb_gamepad_axis_names[axis];
}

bool tb_gamepad_button_down(const tb_gamepad_state *state, tb_gamepad_button button)
{
    TB_ASSERT(state != NULL);
    TB_ASSERT_MSG((unsigned)button < (unsigned)TB_GAMEPAD_BUTTON_COUNT, "a button");
    return (state->buttons & (1u << (unsigned)button)) != 0;
}

bool tb_gamepad_analog_button(float value, bool was_down, const tb_gamepad_threshold *threshold)
{
    TB_ASSERT(threshold != NULL);
    TB_ASSERT_MSG(threshold->press > 0.0f && threshold->press <= 1.0f,
                  "a press above 0, at most 1");
    TB_ASSERT_MSG(threshold->release >= 0.0f && threshold->release <= threshold->press,
                  "a release from 0 to the press");
    TB_ASSERT_MSG(isfinite(value), "a finite value");
    if (value >= threshold->press)
    {
        return true;
    }
    if (value < threshold->release)
    {
        return false;
    }
    return was_down;
}

/* One virtual button: its bit in `previous`, and whether it is down now. */
static uint32_t tb_gamepad_virtual_bit(uint32_t previous, tb_gamepad_virtual_button button,
                                       float value, const tb_gamepad_threshold *threshold)
{
    const uint32_t bit = 1u << (unsigned)button;
    const bool was_down = (previous & bit) != 0;

    if (tb_gamepad_analog_button(value, was_down, threshold))
    {
        return bit;
    }
    return 0;
}

uint32_t tb_gamepad_virtual_buttons(const tb_gamepad_state *state, uint32_t previous,
                                    const tb_gamepad_virtual_thresholds *thresholds)
{
    static const tb_gamepad_virtual_thresholds defaults = TB_GAMEPAD_VIRTUAL_THRESHOLDS_DEFAULT;
    const tb_gamepad_virtual_thresholds *const chosen = thresholds != NULL ? thresholds : &defaults;
    const tb_gamepad_threshold *const stick = &chosen->stick;
    const tb_gamepad_threshold *const trigger = &chosen->trigger;
    const float *axes = NULL;
    uint32_t down = 0;

    TB_ASSERT(state != NULL);
    if (!state->connected)
    {
        return 0;
    }
    axes = state->axes;
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_LEFT_STICK_UP,
                                   axes[TB_GAMEPAD_AXIS_LEFT_Y], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_LEFT_STICK_DOWN,
                                   -axes[TB_GAMEPAD_AXIS_LEFT_Y], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_LEFT_STICK_LEFT,
                                   -axes[TB_GAMEPAD_AXIS_LEFT_X], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_LEFT_STICK_RIGHT,
                                   axes[TB_GAMEPAD_AXIS_LEFT_X], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_RIGHT_STICK_UP,
                                   axes[TB_GAMEPAD_AXIS_RIGHT_Y], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_RIGHT_STICK_DOWN,
                                   -axes[TB_GAMEPAD_AXIS_RIGHT_Y], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_RIGHT_STICK_LEFT,
                                   -axes[TB_GAMEPAD_AXIS_RIGHT_X], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_RIGHT_STICK_RIGHT,
                                   axes[TB_GAMEPAD_AXIS_RIGHT_X], stick);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_LEFT_TRIGGER,
                                   axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], trigger);
    down |= tb_gamepad_virtual_bit(previous, TB_GAMEPAD_VIRTUAL_RIGHT_TRIGGER,
                                   axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], trigger);
    return down;
}
