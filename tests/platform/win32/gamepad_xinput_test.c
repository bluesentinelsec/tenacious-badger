/* What the Windows backend makes of XInput, with XInput's functions
 * replaced by pads of the test's own: buttons and ranges, users that come
 * and go, empty users asked again only now and then, and the motors. */
#include "platform/win32/gamepad_xinput.h"

#include "tb_test.h"

/* The pads XInput reports, and what was asked of it. One test at a time
 * uses them. */
typedef struct fake_xinput
{
    bool connected[TB_XINPUT_USER_COUNT];
    XINPUT_GAMEPAD gamepads[TB_XINPUT_USER_COUNT];
    BYTE subtype;
    bool capabilities_fail;
    bool motors;
    size_t reads[TB_XINPUT_USER_COUNT];
    size_t sets;
    DWORD set_user;
    XINPUT_VIBRATION vibration;
} fake_xinput;

static fake_xinput fake = {0};

static uint64_t fake_now = 0;

static uint64_t fake_clock(void)
{
    return fake_now;
}

static DWORD WINAPI fake_get_state(DWORD user, XINPUT_STATE *state)
{
    fake.reads[user]++;
    if (!fake.connected[user])
    {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    state->Gamepad = fake.gamepads[user];
    return ERROR_SUCCESS;
}

static DWORD WINAPI fake_set_state(DWORD user, XINPUT_VIBRATION *vibration)
{
    fake.sets++;
    fake.set_user = user;
    fake.vibration = *vibration;
    return ERROR_SUCCESS;
}

static DWORD WINAPI fake_get_capabilities(DWORD user, DWORD flags,
                                          XINPUT_CAPABILITIES *capabilities)
{
    (void)user;
    (void)flags;
    if (fake.capabilities_fail)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    capabilities->SubType = fake.subtype;
    capabilities->Vibration.wLeftMotorSpeed = fake.motors ? 0xffff : 0;
    capabilities->Vibration.wRightMotorSpeed = fake.motors ? 0xffff : 0;
    return ERROR_SUCCESS;
}

static tb_gamepad_system *fake_system(void)
{
    static const tb_xinput_functions functions = {
        .get_state = fake_get_state,
        .set_state = fake_set_state,
        .get_capabilities = fake_get_capabilities,
    };

    fake = (fake_xinput){0};
    fake.subtype = XINPUT_DEVSUBTYPE_GAMEPAD;
    fake.motors = true;
    fake_now = 0;
    return tb_gamepad_system_make(&tb_xinput_ops, tb_xinput_make(&functions), fake_clock);
}

/* Every button of XInput's, by its place, and the ranges: sticks to -1 to 1
 * with up positive, as XInput's are; triggers to 0 to 1. */
TB_TEST(platform_win32_gamepad, xinput_buttons_and_ranges)
{
    static const struct
    {
        WORD bit;
        tb_gamepad_button button;
    } rows[] = {
        {XINPUT_GAMEPAD_A, TB_GAMEPAD_BUTTON_SOUTH},
        {XINPUT_GAMEPAD_B, TB_GAMEPAD_BUTTON_EAST},
        {XINPUT_GAMEPAD_X, TB_GAMEPAD_BUTTON_WEST},
        {XINPUT_GAMEPAD_Y, TB_GAMEPAD_BUTTON_NORTH},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, TB_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, TB_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {XINPUT_GAMEPAD_LEFT_THUMB, TB_GAMEPAD_BUTTON_LEFT_STICK},
        {XINPUT_GAMEPAD_RIGHT_THUMB, TB_GAMEPAD_BUTTON_RIGHT_STICK},
        {XINPUT_GAMEPAD_DPAD_UP, TB_GAMEPAD_BUTTON_DPAD_UP},
        {XINPUT_GAMEPAD_DPAD_DOWN, TB_GAMEPAD_BUTTON_DPAD_DOWN},
        {XINPUT_GAMEPAD_DPAD_LEFT, TB_GAMEPAD_BUTTON_DPAD_LEFT},
        {XINPUT_GAMEPAD_DPAD_RIGHT, TB_GAMEPAD_BUTTON_DPAD_RIGHT},
        {XINPUT_GAMEPAD_START, TB_GAMEPAD_BUTTON_START},
        {XINPUT_GAMEPAD_BACK, TB_GAMEPAD_BUTTON_BACK},
    };
    tb_gamepad_state state = {0};
    XINPUT_GAMEPAD gamepad = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("bit 0x%x", (unsigned)rows[index].bit);
        gamepad.wButtons = rows[index].bit;
        tb_xinput_state(&gamepad, &state);
        TB_TEST_EXPECT(state.connected);
        TB_TEST_EXPECT_HEX(state.buttons, ==, (uint32_t)(1u << (unsigned)rows[index].button));
    }
    /* 0x0400 is the guide button of XInputGetStateEx, which is never read;
     * were it there, it is not a button of the layout. */
    gamepad.wButtons = 0x0400;
    tb_xinput_state(&gamepad, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);

    gamepad = (XINPUT_GAMEPAD){.sThumbLX = INT16_MAX,
                               .sThumbLY = INT16_MIN,
                               .sThumbRX = 0,
                               .sThumbRY = 16384,
                               .bLeftTrigger = 255,
                               .bRightTrigger = 0};
    tb_xinput_state(&gamepad, &state);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_X], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_Y], ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_X], ==, 0.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(state.axes[TB_GAMEPAD_AXIS_RIGHT_Y], 0.5f, 1e-4f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER], ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(state.axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER], ==, 0.0f);
}

/* A user that comes is found at the next probe, keeps its slot, and is read
 * every poll; one that goes is gone at once. Empty users are asked only
 * every half second. */
TB_TEST(platform_win32_gamepad, xinput_users_come_and_go)
{
    tb_gamepad_system *const system = fake_system();
    tb_gamepad_event event = {0};
    tb_gamepad_state state = {0};
    tb_gamepad_info info = {0};

    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(fake.reads[1], ==, 1);
    fake.connected[1] = true;
    fake.gamepads[1].wButtons = XINPUT_GAMEPAD_A;
    fake_now = TB_XINPUT_PROBE_NANOSECONDS / 2u;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(fake.reads[1], ==, 1);
    TB_TEST_EXPECT(!tb_gamepad_system_next_event(system, &event));

    fake_now = TB_XINPUT_PROBE_NANOSECONDS;
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_CONNECTED);
    TB_TEST_EXPECT_UINT(event.slot, ==, 1);
    tb_gamepad_get_state(system, 1, &state);
    tb_gamepad_get_info(system, 1, &info);
    TB_TEST_EXPECT(tb_gamepad_button_down(&state, TB_GAMEPAD_BUTTON_SOUTH));
    TB_TEST_EXPECT_STRING(info.name, ==, "Xbox controller");
    TB_TEST_EXPECT_HEX(info.capabilities, ==, TB_GAMEPAD_CAPABILITY_RUMBLE);

    /* Read every poll while it is there, without waiting for a probe. */
    fake.gamepads[1].wButtons = 0;
    fake_now += 1u;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_state(system, 1, &state);
    TB_TEST_EXPECT_HEX(state.buttons, ==, 0);

    fake.connected[1] = false;
    fake_now += 1u;
    tb_gamepad_system_poll(system);
    TB_TEST_REQUIRE(tb_gamepad_system_next_event(system, &event));
    TB_TEST_EXPECT_INT(event.type, ==, TB_GAMEPAD_EVENT_DISCONNECTED);
    tb_gamepad_system_close(system);
}

/* XInput names only a kind: a wheel or a guitar is an XInput controller.
 * A pad without motors, or whose capabilities cannot be read, is
 * described as well as it can be. */
TB_TEST(platform_win32_gamepad, xinput_names_and_motors)
{
    tb_gamepad_system *const system = fake_system();
    tb_gamepad_info info = {0};

    fake.subtype = XINPUT_DEVSUBTYPE_WHEEL;
    fake.motors = false;
    fake.connected[0] = true;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_info(system, 0, &info);
    TB_TEST_EXPECT_STRING(info.name, ==, "XInput controller");
    TB_TEST_EXPECT_HEX(info.capabilities, ==, 0);
    TB_TEST_EXPECT(!tb_gamepad_rumble(system, 0, &(tb_gamepad_rumble_desc){1.0f, 1.0f, 0.1}));

    fake.capabilities_fail = true;
    fake.connected[2] = true;
    fake_now = TB_XINPUT_PROBE_NANOSECONDS;
    tb_gamepad_system_poll(system);
    tb_gamepad_get_info(system, 2, &info);
    TB_TEST_EXPECT_STRING(info.name, ==, "XInput controller");
    TB_TEST_EXPECT_HEX(info.capabilities, ==, TB_GAMEPAD_CAPABILITY_RUMBLE);
    tb_gamepad_system_close(system);
}

/* The left motor is the low-frequency one; XInput's motors run until told
 * otherwise, so the front's stop at the end of the time reaches them. */
TB_TEST(platform_win32_gamepad, xinput_motors)
{
    tb_gamepad_system *const system = fake_system();

    fake.connected[3] = true;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT(tb_gamepad_rumble(system, 3, &(tb_gamepad_rumble_desc){1.0f, 0.5f, 0.1}));
    TB_TEST_EXPECT_UINT(fake.sets, ==, 1);
    TB_TEST_EXPECT_UINT(fake.set_user, ==, 3);
    TB_TEST_EXPECT_UINT(fake.vibration.wLeftMotorSpeed, ==, 65535);
    TB_TEST_EXPECT_UINT(fake.vibration.wRightMotorSpeed, ==, 32767);
    fake_now += 200000000u;
    tb_gamepad_system_poll(system);
    TB_TEST_EXPECT_UINT(fake.sets, ==, 2);
    TB_TEST_EXPECT_UINT(fake.vibration.wLeftMotorSpeed, ==, 0);
    TB_TEST_EXPECT_UINT(fake.vibration.wRightMotorSpeed, ==, 0);
    tb_gamepad_system_close(system);
}
