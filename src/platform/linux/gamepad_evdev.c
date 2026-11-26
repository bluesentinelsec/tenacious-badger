/* What an evdev gamepad's codes mean in the standard layout. Standard C: the
 * backend in gamepad.c reads the device, and this turns what it read into a
 * state, whichever driver made the device.
 *
 * Drivers disagree on two things, which the layout settles per device:
 *
 * - **The face buttons.** The kernel's gamepad documentation names them by
 *   place, BTN_NORTH (0x133) the top and BTN_WEST (0x134) the left, and the
 *   Sony and Nintendo drivers follow it. xpad, hid-steam and Steam's
 *   virtual pads report the Xbox labels instead, BTN_X (also 0x133) for the
 *   left button and BTN_Y (0x134) for the top. A device of Sony's or
 *   Nintendo's vendor is read by place; any other by label. Linux's driver
 *   of the Switch 2 controllers (hid-nintendo, since Linux 7.4) names each
 *   button by its label's code instead: A, on the right, is BTN_A
 *   (BTN_SOUTH), B, at the bottom, BTN_B (BTN_EAST), X, at the top, BTN_X
 *   (BTN_NORTH), and Y, on the left, BTN_Y (BTN_WEST). Their south and east
 *   are swapped back.
 * - **The right stick and the triggers.** xpad and the PlayStation drivers
 *   put the right stick on ABS_RX and ABS_RY and the triggers on ABS_Z and
 *   ABS_RZ; generic HID pads put the right stick on ABS_Z and ABS_RZ;
 *   hid-steam puts the triggers on ABS_HAT2Y and ABS_HAT2X; some drivers on
 *   ABS_BRAKE and ABS_GAS; and Nintendo's have digital triggers only,
 *   BTN_TL2 and BTN_TR2.
 *
 * The guide button (BTN_MODE) and the share button (KEY_RECORD on xpad) are
 * the system's and Steam's, and are never read. The DualSense Edge's back
 * buttons, which hid-playstation gives from Linux 7.2 as
 * BTN_TRIGGER_HAPPY3 and 4, are the upper paddles; its Fn buttons, 1 and 2,
 * switch the pad's own profiles and are not read. */
#include "gamepad_evdev.h"

#include "../shared/gamepad.h"

#include "badger/assert.h"

bool tb_evdev_has(const uint8_t *bits, size_t count, uint32_t code)
{
    if (code >= count)
    {
        return false;
    }
    return (bits[code / 8u] & (1u << (code % 8u))) != 0;
}

static bool tb_evdev_has_key(const tb_evdev_device *device, uint32_t code)
{
    return tb_evdev_has(device->keys, TB_EVDEV_KEY_COUNT, code);
}

static bool tb_evdev_has_axis(const tb_evdev_device *device, uint32_t code)
{
    return tb_evdev_has(device->axes, TB_EVDEV_ABS_COUNT, code);
}

bool tb_evdev_is_gamepad(const tb_evdev_device *device)
{
    return tb_evdev_has_key(device, TB_EVDEV_BTN_SOUTH) &&
           tb_evdev_has_axis(device, TB_EVDEV_ABS_X) && tb_evdev_has_axis(device, TB_EVDEV_ABS_Y);
}

bool tb_evdev_can_rumble(const tb_evdev_device *device)
{
    return tb_evdev_has(device->effects, TB_EVDEV_FF_COUNT, TB_EVDEV_FF_RUMBLE);
}

/* Whether a device has both axes of a pair. */
static bool tb_evdev_has_pair(const tb_evdev_device *device, uint32_t first, uint32_t second)
{
    return tb_evdev_has_axis(device, first) && tb_evdev_has_axis(device, second);
}

static void tb_evdev_set_pair(tb_evdev_layout *layout, tb_gamepad_axis first_axis, uint32_t first,
                              uint32_t second)
{
    layout->axes[first_axis] = (uint16_t)first;
    layout->axes[first_axis + 1] = (uint16_t)second;
}

/* The triggers, by what the device has: analog on one of three pairs, or
 * digital. */
static void tb_evdev_choose_triggers(const tb_evdev_device *device, bool right_stick_on_z,
                                     tb_evdev_layout *layout)
{
    if (!right_stick_on_z && tb_evdev_has_pair(device, TB_EVDEV_ABS_Z, TB_EVDEV_ABS_RZ))
    {
        tb_evdev_set_pair(layout, TB_GAMEPAD_AXIS_LEFT_TRIGGER, TB_EVDEV_ABS_Z, TB_EVDEV_ABS_RZ);
        return;
    }
    if (tb_evdev_has_pair(device, TB_EVDEV_ABS_HAT2Y, TB_EVDEV_ABS_HAT2X))
    {
        tb_evdev_set_pair(layout, TB_GAMEPAD_AXIS_LEFT_TRIGGER, TB_EVDEV_ABS_HAT2Y,
                          TB_EVDEV_ABS_HAT2X);
        return;
    }
    if (tb_evdev_has_pair(device, TB_EVDEV_ABS_BRAKE, TB_EVDEV_ABS_GAS))
    {
        tb_evdev_set_pair(layout, TB_GAMEPAD_AXIS_LEFT_TRIGGER, TB_EVDEV_ABS_BRAKE,
                          TB_EVDEV_ABS_GAS);
        return;
    }
    layout->digital_triggers =
        tb_evdev_has_key(device, TB_EVDEV_BTN_TL2) || tb_evdev_has_key(device, TB_EVDEV_BTN_TR2);
}

/* Whether a device is one of the Switch 2 controllers. */
static bool tb_evdev_is_switch2(const tb_evdev_device *device)
{
    if (device->vendor != TB_EVDEV_VENDOR_NINTENDO)
    {
        return false;
    }
    switch (device->product)
    {
    case TB_EVDEV_SWITCH2_JOYCON_RIGHT:
    case TB_EVDEV_SWITCH2_JOYCON_LEFT:
    case TB_EVDEV_SWITCH2_PRO:
    case TB_EVDEV_SWITCH2_GAMECUBE:
        return true;
    default:
        return false;
    }
}

void tb_evdev_layout_make(const tb_evdev_device *device, tb_evdev_layout *layout)
{
    bool right_stick_on_z = false;

    *layout = (tb_evdev_layout){0};
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
    {
        layout->axes[axis] = TB_EVDEV_NO_AXIS;
    }
    tb_evdev_set_pair(layout, TB_GAMEPAD_AXIS_LEFT_X, TB_EVDEV_ABS_X, TB_EVDEV_ABS_Y);
    if (tb_evdev_has_pair(device, TB_EVDEV_ABS_RX, TB_EVDEV_ABS_RY))
    {
        tb_evdev_set_pair(layout, TB_GAMEPAD_AXIS_RIGHT_X, TB_EVDEV_ABS_RX, TB_EVDEV_ABS_RY);
    }
    else if (tb_evdev_has_pair(device, TB_EVDEV_ABS_Z, TB_EVDEV_ABS_RZ))
    {
        tb_evdev_set_pair(layout, TB_GAMEPAD_AXIS_RIGHT_X, TB_EVDEV_ABS_Z, TB_EVDEV_ABS_RZ);
        right_stick_on_z = true;
    }
    tb_evdev_choose_triggers(device, right_stick_on_z, layout);
    layout->positional_faces =
        device->vendor == TB_EVDEV_VENDOR_SONY || device->vendor == TB_EVDEV_VENDOR_NINTENDO;
    layout->swapped_south_east = tb_evdev_is_switch2(device);
    layout->edge_paddles =
        device->vendor == TB_EVDEV_VENDOR_SONY && device->product == TB_EVDEV_DUALSENSE_EDGE;
    layout->hat_dpad = tb_evdev_has_pair(device, TB_EVDEV_ABS_HAT0X, TB_EVDEV_ABS_HAT0Y);
    layout->happy_dpad = !layout->hat_dpad && !tb_evdev_has_key(device, TB_EVDEV_BTN_DPAD_UP) &&
                         tb_evdev_has_key(device, TB_EVDEV_BTN_TRIGGER_HAPPY1) &&
                         tb_evdev_has_key(device, TB_EVDEV_BTN_TRIGGER_HAPPY4);
}

/* The button of a d-pad given as xpad's buttons. */
static tb_gamepad_button tb_evdev_happy_dpad(uint32_t code)
{
    switch (code)
    {
    case TB_EVDEV_BTN_TRIGGER_HAPPY1:
        return TB_GAMEPAD_BUTTON_DPAD_LEFT;
    case TB_EVDEV_BTN_TRIGGER_HAPPY2:
        return TB_GAMEPAD_BUTTON_DPAD_RIGHT;
    case TB_EVDEV_BTN_TRIGGER_HAPPY3:
        return TB_GAMEPAD_BUTTON_DPAD_UP;
    default:
        return TB_GAMEPAD_BUTTON_DPAD_DOWN;
    }
}

/* The button of a key, or TB_GAMEPAD_BUTTON_COUNT for none. */
static tb_gamepad_button tb_evdev_button(const tb_evdev_layout *layout, uint32_t code)
{
    if (layout->happy_dpad && code >= TB_EVDEV_BTN_TRIGGER_HAPPY1 &&
        code <= TB_EVDEV_BTN_TRIGGER_HAPPY4)
    {
        return tb_evdev_happy_dpad(code);
    }
    if (layout->edge_paddles && code == TB_EVDEV_BTN_TRIGGER_HAPPY3)
    {
        return TB_GAMEPAD_BUTTON_LEFT_PADDLE_1;
    }
    if (layout->edge_paddles && code == TB_EVDEV_BTN_TRIGGER_HAPPY4)
    {
        return TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1;
    }
    switch (code)
    {
    case TB_EVDEV_BTN_SOUTH:
        return layout->swapped_south_east ? TB_GAMEPAD_BUTTON_EAST : TB_GAMEPAD_BUTTON_SOUTH;
    case TB_EVDEV_BTN_EAST:
        return layout->swapped_south_east ? TB_GAMEPAD_BUTTON_SOUTH : TB_GAMEPAD_BUTTON_EAST;
    case TB_EVDEV_BTN_NORTH:
        return layout->positional_faces ? TB_GAMEPAD_BUTTON_NORTH : TB_GAMEPAD_BUTTON_WEST;
    case TB_EVDEV_BTN_WEST:
        return layout->positional_faces ? TB_GAMEPAD_BUTTON_WEST : TB_GAMEPAD_BUTTON_NORTH;
    case TB_EVDEV_BTN_TL:
        return TB_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case TB_EVDEV_BTN_TR:
        return TB_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case TB_EVDEV_BTN_SELECT:
        return TB_GAMEPAD_BUTTON_BACK;
    case TB_EVDEV_BTN_START:
        return TB_GAMEPAD_BUTTON_START;
    case TB_EVDEV_BTN_THUMBL:
        return TB_GAMEPAD_BUTTON_LEFT_STICK;
    case TB_EVDEV_BTN_THUMBR:
        return TB_GAMEPAD_BUTTON_RIGHT_STICK;
    case TB_EVDEV_BTN_DPAD_UP:
        return TB_GAMEPAD_BUTTON_DPAD_UP;
    case TB_EVDEV_BTN_DPAD_DOWN:
        return TB_GAMEPAD_BUTTON_DPAD_DOWN;
    case TB_EVDEV_BTN_DPAD_LEFT:
        return TB_GAMEPAD_BUTTON_DPAD_LEFT;
    case TB_EVDEV_BTN_DPAD_RIGHT:
        return TB_GAMEPAD_BUTTON_DPAD_RIGHT;
    case TB_EVDEV_BTN_GRIPL:
    case TB_EVDEV_BTN_TRIGGER_HAPPY7:
        return TB_GAMEPAD_BUTTON_LEFT_PADDLE_1;
    case TB_EVDEV_BTN_GRIPL2:
    case TB_EVDEV_BTN_TRIGGER_HAPPY8:
        return TB_GAMEPAD_BUTTON_LEFT_PADDLE_2;
    case TB_EVDEV_BTN_GRIPR:
    case TB_EVDEV_BTN_TRIGGER_HAPPY5:
        return TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1;
    case TB_EVDEV_BTN_GRIPR2:
    case TB_EVDEV_BTN_TRIGGER_HAPPY6:
        return TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2;
    default:
        return TB_GAMEPAD_BUTTON_COUNT;
    }
}

static void tb_evdev_set_button(tb_gamepad_state *state, tb_gamepad_button button, bool down)
{
    const uint32_t bit = 1u << (unsigned)button;

    if (down)
    {
        state->buttons |= bit;
    }
    else
    {
        state->buttons &= ~bit;
    }
}

static void tb_evdev_apply_key(const tb_evdev_layout *layout, uint32_t code, bool down,
                               tb_gamepad_state *state)
{
    tb_gamepad_button button = TB_GAMEPAD_BUTTON_COUNT;

    if (layout->digital_triggers && (code == TB_EVDEV_BTN_TL2 || code == TB_EVDEV_BTN_TR2))
    {
        const tb_gamepad_axis axis =
            code == TB_EVDEV_BTN_TL2 ? TB_GAMEPAD_AXIS_LEFT_TRIGGER : TB_GAMEPAD_AXIS_RIGHT_TRIGGER;
        state->axes[axis] = down ? 1.0f : 0.0f;
        return;
    }
    button = tb_evdev_button(layout, code);
    if (button == TB_GAMEPAD_BUTTON_COUNT)
    {
        return;
    }
    tb_evdev_set_button(state, button, down);
}

static void tb_evdev_apply_hat(uint32_t code, int32_t value, tb_gamepad_state *state)
{
    if (code == TB_EVDEV_ABS_HAT0X)
    {
        tb_evdev_set_button(state, TB_GAMEPAD_BUTTON_DPAD_LEFT, value < 0);
        tb_evdev_set_button(state, TB_GAMEPAD_BUTTON_DPAD_RIGHT, value > 0);
        return;
    }
    /* The hat's Y is down positive, as the sticks' are. */
    tb_evdev_set_button(state, TB_GAMEPAD_BUTTON_DPAD_UP, value < 0);
    tb_evdev_set_button(state, TB_GAMEPAD_BUTTON_DPAD_DOWN, value > 0);
}

static void tb_evdev_apply_axis(const tb_evdev_device *device, const tb_evdev_layout *layout,
                                uint32_t code, int32_t value, tb_gamepad_state *state)
{
    if (layout->hat_dpad && (code == TB_EVDEV_ABS_HAT0X || code == TB_EVDEV_ABS_HAT0Y))
    {
        tb_evdev_apply_hat(code, value, state);
        return;
    }
    if (code >= TB_EVDEV_ABS_COUNT)
    {
        return;
    }
    for (size_t axis = 0; axis < TB_GAMEPAD_AXIS_COUNT; axis++)
    {
        const int32_t minimum = device->minimum[code];
        const int32_t maximum = device->maximum[code];
        if (layout->axes[axis] != code)
        {
            continue;
        }
        if (axis >= TB_GAMEPAD_AXIS_LEFT_TRIGGER)
        {
            state->axes[axis] = tb_gamepad_trigger_value(value, minimum, maximum);
            continue;
        }
        /* A stick's Y is down positive in evdev, and up positive here. */
        state->axes[axis] = tb_gamepad_stick_value(value, minimum, maximum,
                                                   axis == TB_GAMEPAD_AXIS_LEFT_Y ||
                                                       axis == TB_GAMEPAD_AXIS_RIGHT_Y);
    }
}

void tb_evdev_apply(const tb_evdev_device *device, const tb_evdev_layout *layout, uint32_t type,
                    uint32_t code, int32_t value, tb_gamepad_state *state)
{
    TB_ASSERT(device != NULL && layout != NULL && state != NULL);
    if (type == TB_EVDEV_EV_KEY)
    {
        tb_evdev_apply_key(layout, code, value != 0, state);
        return;
    }
    if (type == TB_EVDEV_EV_ABS)
    {
        tb_evdev_apply_axis(device, layout, code, value, state);
    }
}

void tb_evdev_resync(const tb_evdev_device *device, const tb_evdev_layout *layout,
                     const uint8_t *keys_down, const int32_t *values, tb_gamepad_state *state)
{
    *state = (tb_gamepad_state){.connected = true};
    for (uint32_t code = 0; code < TB_EVDEV_KEY_COUNT; code++)
    {
        if (tb_evdev_has_key(device, code))
        {
            tb_evdev_apply_key(layout, code, tb_evdev_has(keys_down, TB_EVDEV_KEY_COUNT, code),
                               state);
        }
    }
    for (uint32_t code = 0; code < TB_EVDEV_ABS_COUNT; code++)
    {
        if (tb_evdev_has_axis(device, code))
        {
            tb_evdev_apply_axis(device, layout, code, values[code], state);
        }
    }
}
