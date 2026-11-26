/* What evdev devices' codes mean in the standard layout, fed as the
 * kernel's events, for the drivers whose devices a Linux player and the
 * Steam Deck have: xpad (and Steam's virtual pads, which are made like it),
 * hid-playstation, hid-nintendo, hid-steam, and a generic HID pad. The
 * mapping is standard C, so it is tested on every platform. */
#include "platform/linux/gamepad_evdev.h"

#include "tb_test.h"

#include <stddef.h>

static void set_bit(uint8_t *bits, uint32_t code)
{
    bits[code / 8u] = (uint8_t)(bits[code / 8u] | (1u << (code % 8u)));
}

static void add_keys(tb_evdev_device *device, const uint32_t *codes, size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        set_bit(device->keys, codes[index]);
    }
}

static void add_axis(tb_evdev_device *device, uint32_t code, int32_t minimum, int32_t maximum)
{
    set_bit(device->axes, code);
    device->minimum[code] = minimum;
    device->maximum[code] = maximum;
}

static const uint32_t common_keys[] = {
    TB_EVDEV_BTN_SOUTH, TB_EVDEV_BTN_EAST,   TB_EVDEV_BTN_NORTH,  TB_EVDEV_BTN_WEST,
    TB_EVDEV_BTN_TL,    TB_EVDEV_BTN_TR,     TB_EVDEV_BTN_SELECT, TB_EVDEV_BTN_START,
    TB_EVDEV_BTN_MODE,  TB_EVDEV_BTN_THUMBL, TB_EVDEV_BTN_THUMBR,
};

/* An Xbox pad on xpad: sticks on X, Y, RX and RY from -32768 to 32767,
 * triggers on Z and RZ from 0 to 1023, the d-pad a hat. */
static tb_evdev_device xpad(void)
{
    tb_evdev_device device = {.vendor = 0x045e, .product = 0x0b12};

    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_axis(&device, TB_EVDEV_ABS_X, -32768, 32767);
    add_axis(&device, TB_EVDEV_ABS_Y, -32768, 32767);
    add_axis(&device, TB_EVDEV_ABS_RX, -32768, 32767);
    add_axis(&device, TB_EVDEV_ABS_RY, -32768, 32767);
    add_axis(&device, TB_EVDEV_ABS_Z, 0, 1023);
    add_axis(&device, TB_EVDEV_ABS_RZ, 0, 1023);
    add_axis(&device, TB_EVDEV_ABS_HAT0X, -1, 1);
    add_axis(&device, TB_EVDEV_ABS_HAT0Y, -1, 1);
    set_bit(device.effects, TB_EVDEV_FF_RUMBLE);
    return device;
}

static void press(const tb_evdev_device *device, const tb_evdev_layout *layout, uint32_t code,
                  tb_gamepad_state *state)
{
    tb_evdev_apply(device, layout, TB_EVDEV_EV_KEY, code, 1, state);
}

static void move(const tb_evdev_device *device, const tb_evdev_layout *layout, uint32_t code,
                 int32_t value, tb_gamepad_state *state)
{
    tb_evdev_apply(device, layout, TB_EVDEV_EV_ABS, code, value, state);
}

TB_TEST(platform_gamepad_evdev, gamepads_are_told_from_other_devices)
{
    tb_evdev_device device = xpad();
    tb_evdev_device keyboard = {0};
    tb_evdev_device joystick = {0};
    const uint32_t trigger = 0x120u; /* BTN_TRIGGER, a joystick's. */

    TB_TEST_EXPECT(tb_evdev_is_gamepad(&device));
    TB_TEST_EXPECT(tb_evdev_can_rumble(&device));
    set_bit(keyboard.keys, 30u); /* KEY_A */
    TB_TEST_EXPECT(!tb_evdev_is_gamepad(&keyboard));
    add_keys(&joystick, &trigger, 1);
    add_axis(&joystick, TB_EVDEV_ABS_X, 0, 1023);
    add_axis(&joystick, TB_EVDEV_ABS_Y, 0, 1023);
    TB_TEST_EXPECT(!tb_evdev_is_gamepad(&joystick));
    TB_TEST_EXPECT(!tb_evdev_can_rumble(&joystick));
    /* A gamepad without a stick, as a touchpad's buttons. */
    device = (tb_evdev_device){0};
    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    TB_TEST_EXPECT(!tb_evdev_is_gamepad(&device));
    TB_TEST_EXPECT(!tb_evdev_has(device.keys, TB_EVDEV_KEY_COUNT, TB_EVDEV_KEY_COUNT));
}

/* xpad reports the Xbox labels: BTN_X (0x133) is the left button and BTN_Y
 * (0x134) the top. The guide button is never a button of the layout. */
TB_TEST(platform_gamepad_evdev, xpad_buttons_are_by_label)
{
    const tb_evdev_device device = xpad();
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};
    static const struct
    {
        uint32_t code;
        tb_gamepad_button button;
    } rows[] = {
        {TB_EVDEV_BTN_SOUTH, TB_GAMEPAD_BUTTON_SOUTH},
        {TB_EVDEV_BTN_EAST, TB_GAMEPAD_BUTTON_EAST},
        {TB_EVDEV_BTN_NORTH, TB_GAMEPAD_BUTTON_WEST},
        {TB_EVDEV_BTN_WEST, TB_GAMEPAD_BUTTON_NORTH},
        {TB_EVDEV_BTN_TL, TB_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {TB_EVDEV_BTN_TR, TB_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {TB_EVDEV_BTN_SELECT, TB_GAMEPAD_BUTTON_BACK},
        {TB_EVDEV_BTN_START, TB_GAMEPAD_BUTTON_START},
        {TB_EVDEV_BTN_THUMBL, TB_GAMEPAD_BUTTON_LEFT_STICK},
        {TB_EVDEV_BTN_THUMBR, TB_GAMEPAD_BUTTON_RIGHT_STICK},
        {TB_EVDEV_BTN_TRIGGER_HAPPY5, TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1},
        {TB_EVDEV_BTN_TRIGGER_HAPPY6, TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2},
        {TB_EVDEV_BTN_TRIGGER_HAPPY7, TB_GAMEPAD_BUTTON_LEFT_PADDLE_1},
        {TB_EVDEV_BTN_TRIGGER_HAPPY8, TB_GAMEPAD_BUTTON_LEFT_PADDLE_2},
    };

    tb_evdev_layout_make(&device, &layout);
    TB_TEST_EXPECT(!layout.positional_faces);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("code 0x%x", (unsigned)rows[index].code);
        state.buttons = 0;
        press(&device, &layout, rows[index].code, &state);
        TB_TEST_EXPECT_HEX(state.buttons, ==, (uint32_t)(1u << (unsigned)rows[index].button));
        tb_evdev_apply(&device, &layout, TB_EVDEV_EV_KEY, rows[index].code, 0, &state);
        TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    }
    press(&device, &layout, TB_EVDEV_BTN_MODE, &state);
    press(&device, &layout, 167u, &state); /* KEY_RECORD: the share button. */
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    /* Synchronization and other types change nothing. */
    tb_evdev_apply(&device, &layout, TB_EVDEV_EV_SYN, TB_EVDEV_SYN_REPORT, 0, &state);
    tb_evdev_apply(&device, &layout, TB_EVDEV_EV_FF, 0, 1, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
}

/* Sticks scale to -1 to 1 with up positive, though evdev's Y is down
 * positive; triggers to 0 to 1; the hat is the d-pad. */
TB_TEST(platform_gamepad_evdev, xpad_axes_scale_to_the_standard_ranges)
{
    const tb_evdev_device device = xpad();
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};

    tb_evdev_layout_make(&device, &layout);
    move(&device, &layout, TB_EVDEV_ABS_X, 32767, &state);
    move(&device, &layout, TB_EVDEV_ABS_Y, -32768, &state);
    move(&device, &layout, TB_EVDEV_ABS_RX, -32768, &state);
    move(&device, &layout, TB_EVDEV_ABS_RY, 16384, &state);
    move(&device, &layout, TB_EVDEV_ABS_Z, 1023, &state);
    move(&device, &layout, TB_EVDEV_ABS_RZ, 0, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_X], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_X], ==, -1.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_RIGHT_Y], -0.5f, 1e-4f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 0.0f);

    move(&device, &layout, TB_EVDEV_ABS_HAT0X, -1, &state);
    move(&device, &layout, TB_EVDEV_ABS_HAT0Y, -1, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       (1u << TB_GAMEPAD_BUTTON_DPAD_LEFT) | (1u << TB_GAMEPAD_BUTTON_DPAD_UP));
    move(&device, &layout, TB_EVDEV_ABS_HAT0X, 1, &state);
    move(&device, &layout, TB_EVDEV_ABS_HAT0Y, 1, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       (1u << TB_GAMEPAD_BUTTON_DPAD_RIGHT) | (1u << TB_GAMEPAD_BUTTON_DPAD_DOWN));
    move(&device, &layout, TB_EVDEV_ABS_HAT0X, 0, &state);
    move(&device, &layout, TB_EVDEV_ABS_HAT0Y, 0, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
    /* An axis the layout does not use, and one past the kernel's. */
    move(&device, &layout, TB_EVDEV_ABS_GAS, 99, &state);
    move(&device, &layout, TB_EVDEV_ABS_COUNT, 99, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);
}

/* A DualSense on hid-playstation names the face buttons by place: BTN_NORTH
 * the triangle, BTN_WEST the square. Its sticks go from 0 to 255. */
TB_TEST(platform_gamepad_evdev, playstation_buttons_are_by_place)
{
    tb_evdev_device device = {.vendor = TB_EVDEV_VENDOR_SONY, .product = 0x0ce6};
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};
    const uint32_t digital[] = {TB_EVDEV_BTN_TL2, TB_EVDEV_BTN_TR2};

    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_keys(&device, digital, 2);
    add_axis(&device, TB_EVDEV_ABS_X, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_Y, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_RX, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_RY, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_Z, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_RZ, 0, 255);
    tb_evdev_layout_make(&device, &layout);
    TB_TEST_EXPECT(layout.positional_faces);
    TB_TEST_EXPECT(!layout.digital_triggers);
    press(&device, &layout, TB_EVDEV_BTN_NORTH, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 1u << TB_GAMEPAD_BUTTON_NORTH);
    press(&device, &layout, TB_EVDEV_BTN_WEST, &state);
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_WEST));
    /* With analog triggers, the digital ones are not buttons. */
    press(&device, &layout, TB_EVDEV_BTN_TL2, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 0.0f);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       (1u << TB_GAMEPAD_BUTTON_NORTH) | (1u << TB_GAMEPAD_BUTTON_WEST));
    move(&device, &layout, TB_EVDEV_ABS_Y, 128, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], ==, 0.0f);
}

/* A Switch Pro pad on hid-nintendo: by place, digital triggers, the d-pad
 * as buttons. */
TB_TEST(platform_gamepad_evdev, nintendo_triggers_are_digital)
{
    tb_evdev_device device = {.vendor = TB_EVDEV_VENDOR_NINTENDO, .product = 0x2009};
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};
    const uint32_t extra[] = {TB_EVDEV_BTN_TL2,       TB_EVDEV_BTN_TR2,
                              TB_EVDEV_BTN_DPAD_UP,   TB_EVDEV_BTN_DPAD_DOWN,
                              TB_EVDEV_BTN_DPAD_LEFT, TB_EVDEV_BTN_DPAD_RIGHT};

    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_keys(&device, extra, TB_TEST_COUNT_OF(extra));
    add_axis(&device, TB_EVDEV_ABS_X, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_Y, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_RX, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_RY, -32767, 32767);
    tb_evdev_layout_make(&device, &layout);
    TB_TEST_EXPECT(layout.digital_triggers);
    TB_TEST_EXPECT(!layout.hat_dpad);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, TB_EVDEV_NO_AXIS);
    press(&device, &layout, TB_EVDEV_BTN_TR2, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 1.0f);
    press(&device, &layout, TB_EVDEV_BTN_TL2, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 1.0f);
    tb_evdev_apply(&device, &layout, TB_EVDEV_EV_KEY, TB_EVDEV_BTN_TL2, 0, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 0.0f);
    press(&device, &layout, TB_EVDEV_BTN_DPAD_UP, &state);
    press(&device, &layout, TB_EVDEV_BTN_DPAD_DOWN, &state);
    press(&device, &layout, TB_EVDEV_BTN_DPAD_LEFT, &state);
    press(&device, &layout, TB_EVDEV_BTN_DPAD_RIGHT, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       (1u << TB_GAMEPAD_BUTTON_DPAD_UP) | (1u << TB_GAMEPAD_BUTTON_DPAD_DOWN) |
                           (1u << TB_GAMEPAD_BUTTON_DPAD_LEFT) |
                           (1u << TB_GAMEPAD_BUTTON_DPAD_RIGHT));
}

/* The Steam Deck on hid-steam: triggers on HAT2Y and HAT2X, and its four
 * back buttons. */
TB_TEST(platform_gamepad_evdev, steam_deck_triggers_and_back_buttons)
{
    tb_evdev_device device = {.vendor = 0x28de, .product = 0x1205};
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};
    const uint32_t grips[] = {TB_EVDEV_BTN_GRIPL, TB_EVDEV_BTN_GRIPR, TB_EVDEV_BTN_GRIPL2,
                              TB_EVDEV_BTN_GRIPR2};

    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_keys(&device, grips, TB_TEST_COUNT_OF(grips));
    add_axis(&device, TB_EVDEV_ABS_X, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_Y, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_RX, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_RY, -32767, 32767);
    add_axis(&device, TB_EVDEV_ABS_HAT2Y, 0, 32767);
    add_axis(&device, TB_EVDEV_ABS_HAT2X, 0, 32767);
    tb_evdev_layout_make(&device, &layout);
    move(&device, &layout, TB_EVDEV_ABS_HAT2Y, 32767, &state);
    move(&device, &layout, TB_EVDEV_ABS_HAT2X, 0, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 0.0f);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(grips); index++)
    {
        press(&device, &layout, grips[index], &state);
    }
    TB_TEST_EXPECT_HEX(
        state.buttons, ==,
        (1u << TB_GAMEPAD_BUTTON_LEFT_PADDLE_1) | (1u << TB_GAMEPAD_BUTTON_LEFT_PADDLE_2) |
            (1u << TB_GAMEPAD_BUTTON_RIGHT_PADDLE_1) | (1u << TB_GAMEPAD_BUTTON_RIGHT_PADDLE_2));
}

/* A generic HID pad: the right stick on Z and RZ, the triggers on BRAKE and
 * GAS. */
TB_TEST(platform_gamepad_evdev, generic_pads_put_the_right_stick_on_z)
{
    tb_evdev_device device = {.vendor = 0x0079, .product = 0x0006};
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};

    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_axis(&device, TB_EVDEV_ABS_X, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_Y, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_Z, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_RZ, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_BRAKE, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_GAS, 0, 255);
    tb_evdev_layout_make(&device, &layout);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_RIGHT_X], ==, TB_EVDEV_ABS_Z);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_RIGHT_Y], ==, TB_EVDEV_ABS_RZ);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, TB_EVDEV_ABS_BRAKE);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, TB_EVDEV_ABS_GAS);
    move(&device, &layout, TB_EVDEV_ABS_RZ, 0, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_Y], ==, 1.0f);

    /* With neither, no right stick and no triggers. */
    device = (tb_evdev_device){0};
    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_axis(&device, TB_EVDEV_ABS_X, 0, 255);
    add_axis(&device, TB_EVDEV_ABS_Y, 0, 255);
    tb_evdev_layout_make(&device, &layout);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_RIGHT_X], ==, TB_EVDEV_NO_AXIS);
    TB_TEST_EXPECT_UINT(layout.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, TB_EVDEV_NO_AXIS);
    TB_TEST_EXPECT(!layout.digital_triggers);
}

/* xpad's pads that give the d-pad as buttons give it on the first four
 * BTN_TRIGGER_HAPPY codes: left, right, up, down. */
TB_TEST(platform_gamepad_evdev, xpad_dpad_as_buttons)
{
    tb_evdev_device device = {.vendor = 0x045e, .product = 0x028e};
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.connected = true};
    const uint32_t happy[] = {TB_EVDEV_BTN_TRIGGER_HAPPY1, TB_EVDEV_BTN_TRIGGER_HAPPY2,
                              TB_EVDEV_BTN_TRIGGER_HAPPY3, TB_EVDEV_BTN_TRIGGER_HAPPY4};
    const tb_gamepad_button buttons[] = {TB_GAMEPAD_BUTTON_DPAD_LEFT, TB_GAMEPAD_BUTTON_DPAD_RIGHT,
                                         TB_GAMEPAD_BUTTON_DPAD_UP, TB_GAMEPAD_BUTTON_DPAD_DOWN};

    add_keys(&device, common_keys, TB_TEST_COUNT_OF(common_keys));
    add_keys(&device, happy, TB_TEST_COUNT_OF(happy));
    add_axis(&device, TB_EVDEV_ABS_X, -32768, 32767);
    add_axis(&device, TB_EVDEV_ABS_Y, -32768, 32767);
    tb_evdev_layout_make(&device, &layout);
    TB_TEST_EXPECT(layout.happy_dpad);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(happy); index++)
    {
        TB_TEST_CONTEXT("code 0x%x", (unsigned)happy[index]);
        state.buttons = 0;
        press(&device, &layout, happy[index], &state);
        TB_TEST_EXPECT_HEX(state.buttons, ==, (uint32_t)(1u << (unsigned)buttons[index]));
    }
}

/* After the device is opened, or its events were dropped, its whole state
 * comes from the keys down and the axes' values. */
TB_TEST(platform_gamepad_evdev, a_resync_reads_the_whole_state)
{
    const tb_evdev_device device = xpad();
    tb_evdev_layout layout = {0};
    tb_gamepad_state state = {.buttons = 0xffffu};
    uint8_t keys_down[TB_EVDEV_KEY_COUNT / 8u] = {0};
    int32_t values[TB_EVDEV_ABS_COUNT] = {0};

    tb_evdev_layout_make(&device, &layout);
    set_bit(keys_down, TB_EVDEV_BTN_EAST);
    set_bit(keys_down, TB_EVDEV_BTN_MODE);
    values[TB_EVDEV_ABS_X] = -32768;
    values[TB_EVDEV_ABS_RZ] = 1023;
    values[TB_EVDEV_ABS_HAT0Y] = 1;
    tb_evdev_resync(&device, &layout, keys_down, values, &state);
    TB_TEST_EXPECT(state.connected);
    TB_TEST_EXPECT_HEX(state.buttons, ==,
                       (1u << TB_GAMEPAD_BUTTON_EAST) | (1u << TB_GAMEPAD_BUTTON_DPAD_DOWN));
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_X], ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 1.0f);
}

/* Steam's list names pads by vendor and product, in hex, between commas. */
TB_TEST(platform_gamepad_evdev, steam_names_the_pads_to_leave_alone)
{
    const char *const list = "0x28DE/0x1205, 0x045e/0x0b12,junk,0x1/,/0x2,054c/0ce6";

    TB_TEST_EXPECT(tb_evdev_ignored(list, 0x28de, 0x1205));
    TB_TEST_EXPECT(tb_evdev_ignored(list, 0x045e, 0x0b12));
    TB_TEST_EXPECT(tb_evdev_ignored(list, 0x054c, 0x0ce6));
    TB_TEST_EXPECT(!tb_evdev_ignored(list, 0x28de, 0x11ff));
    TB_TEST_EXPECT(!tb_evdev_ignored(list, 0x0001, 0x0000));
    TB_TEST_EXPECT(!tb_evdev_ignored(NULL, 0x28de, 0x1205));
    TB_TEST_EXPECT(!tb_evdev_ignored("", 0x28de, 0x1205));
}
