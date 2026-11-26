/* Windows gamepads: XInput, and beside it the raw HID pads of
 * gamepad_rawhid.c, which XInput does not read, as one backend.
 *
 * XInput is loaded at run time from the system directory, xinput1_4.dll
 * or, before Windows 8, xinput9_1_0.dll; never linked.
 *
 * XInput has four users, which are the first four slots. It reads by
 * XInputGetState(), which leaves out the guide button: that is only in the
 * undocumented XInputGetStateEx(), which the engine does not call, so that
 * the button stays Game Bar's and Steam's. The share button of newer pads
 * is not in XInput at all. The left motor is the low-frequency one and the
 * right the high-frequency one; motors in the triggers need
 * Windows.Gaming.Input, which the engine does not use yet. */
#include "gamepad_xinput.h"

#include "gamepad_rawhid.h"

#include "../shared/gamepad_mux.h"

#include "badger/assert.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* The largest value of a motor's speed. */
#define TB_XINPUT_MOTOR_MAX 65535.0f

/* The most a trigger reads. */
#define TB_XINPUT_TRIGGER_MAX 255

typedef struct tb_xinput
{
    tb_library *library; /* NULL for a test's functions. */
    tb_xinput_functions functions;
    uint64_t next_probe; /* When empty slots are next asked for a pad. */
    uint64_t serial;     /* The last pad's serial. */
} tb_xinput;

static const struct
{
    WORD bit;
    tb_gamepad_button button;
} tb_xinput_buttons[] = {
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

void tb_xinput_state(const XINPUT_GAMEPAD *gamepad, tb_gamepad_state *state)
{
    *state = (tb_gamepad_state){.connected = true};
    for (size_t index = 0; index < sizeof(tb_xinput_buttons) / sizeof(tb_xinput_buttons[0]);
         index++)
    {
        if ((gamepad->wButtons & tb_xinput_buttons[index].bit) != 0)
        {
            state->buttons |= 1u << (unsigned)tb_xinput_buttons[index].button;
        }
    }
    /* XInput's sticks are up positive already. */
    state->axes[TB_GAMEPAD_AXIS_LEFT_X] =
        tb_gamepad_stick_value(gamepad->sThumbLX, INT16_MIN, INT16_MAX, false);
    state->axes[TB_GAMEPAD_AXIS_LEFT_Y] =
        tb_gamepad_stick_value(gamepad->sThumbLY, INT16_MIN, INT16_MAX, false);
    state->axes[TB_GAMEPAD_AXIS_RIGHT_X] =
        tb_gamepad_stick_value(gamepad->sThumbRX, INT16_MIN, INT16_MAX, false);
    state->axes[TB_GAMEPAD_AXIS_RIGHT_Y] =
        tb_gamepad_stick_value(gamepad->sThumbRY, INT16_MIN, INT16_MAX, false);
    state->axes[TB_GAMEPAD_AXIS_LEFT_TRIGGER] =
        tb_gamepad_trigger_value(gamepad->bLeftTrigger, 0, TB_XINPUT_TRIGGER_MAX);
    state->axes[TB_GAMEPAD_AXIS_RIGHT_TRIGGER] =
        tb_gamepad_trigger_value(gamepad->bRightTrigger, 0, TB_XINPUT_TRIGGER_MAX);
}

/* What a pad that came is: XInput gives no name, only a kind, and whether
 * it has motors. */
static void tb_xinput_info(const tb_xinput *xinput, DWORD user, tb_gamepad_info *info)
{
    XINPUT_CAPABILITIES capabilities = {0};
    const char *name = "XInput controller";

    *info = (tb_gamepad_info){.capabilities = TB_GAMEPAD_CAPABILITY_RUMBLE};
    if (xinput->functions.get_capabilities(user, 0, &capabilities) == ERROR_SUCCESS)
    {
        if (capabilities.SubType == XINPUT_DEVSUBTYPE_GAMEPAD)
        {
            name = "Xbox controller";
        }
        if (capabilities.Vibration.wLeftMotorSpeed == 0 &&
            capabilities.Vibration.wRightMotorSpeed == 0)
        {
            info->capabilities = 0;
        }
    }
    tb_gamepad_copy_name(info->name, name, tb_str_length(name));
}

static void tb_xinput_read(tb_xinput *xinput, DWORD user, bool probe, tb_gamepad_pad *pad)
{
    XINPUT_STATE state = {0};

    if (pad->serial == 0 && !probe)
    {
        return;
    }
    if (xinput->functions.get_state(user, &state) != ERROR_SUCCESS)
    {
        *pad = (tb_gamepad_pad){0};
        return;
    }
    if (pad->serial == 0)
    {
        xinput->serial++;
        pad->serial = xinput->serial;
        tb_xinput_info(xinput, user, &pad->info);
    }
    tb_xinput_state(&state.Gamepad, &pad->state);
}

static void tb_xinput_poll(void *native, uint64_t now, tb_gamepad_pad *pads)
{
    tb_xinput *const xinput = native;
    const bool probe = now >= xinput->next_probe;

    if (probe)
    {
        xinput->next_probe = now + TB_XINPUT_PROBE_NANOSECONDS;
    }
    for (DWORD user = 0; user < TB_XINPUT_USER_COUNT; user++)
    {
        tb_xinput_read(xinput, user, probe, &pads[user]);
    }
}

static void tb_xinput_rumble(void *native, size_t slot, float low_frequency, float high_frequency,
                             double seconds)
{
    tb_xinput *const xinput = native;
    XINPUT_VIBRATION vibration = {.wLeftMotorSpeed = (WORD)(low_frequency * TB_XINPUT_MOTOR_MAX),
                                  .wRightMotorSpeed = (WORD)(high_frequency * TB_XINPUT_MOTOR_MAX)};

    /* XInput's motors run until told otherwise; the front stops them. */
    (void)seconds;
    TB_ASSERT(slot < TB_XINPUT_USER_COUNT);
    (void)xinput->functions.set_state((DWORD)slot, &vibration);
}

static void tb_xinput_close(void *native)
{
    tb_xinput *const xinput = native;

    tb_library_close(xinput->library);
    tb_free(xinput);
}

const tb_gamepad_ops tb_xinput_ops = {
    .close = tb_xinput_close,
    .poll = tb_xinput_poll,
    .rumble = tb_xinput_rumble,
};

void *tb_xinput_make(const tb_xinput_functions *functions)
{
    tb_xinput *const xinput = tb_calloc(1, sizeof(*xinput));

    TB_ASSERT(functions != NULL);
    TB_ASSERT(functions->get_state != NULL && functions->set_state != NULL &&
              functions->get_capabilities != NULL);
    xinput->functions = *functions;
    return xinput;
}

/* XInput's library: the newest that Windows has. */
static tb_library_result tb_xinput_load(tb_library **library, tb_library_error *error)
{
    if (tb_library_open("xinput1_4.dll", TB_LIBRARY_SEARCH_SYSTEM, library, error) == TB_LIBRARY_OK)
    {
        return TB_LIBRARY_OK;
    }
    /* coverage-off: every Windows since 8 has xinput1_4.dll */
    return tb_library_open("xinput9_1_0.dll", TB_LIBRARY_SEARCH_SYSTEM, library, error);
    /* coverage-on */
}

/* Open XInput over the system's library. */
static tb_gamepad_result tb_xinput_open(void **native, tb_gamepad_error *error)
{
    tb_library *library = NULL;
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    tb_library_function functions[3] = {0};
    const tb_library_binding bindings[3] = {
        {.name = "XInputGetState", .function = &functions[0], .required = true},
        {.name = "XInputSetState", .function = &functions[1], .required = true},
        {.name = "XInputGetCapabilities", .function = &functions[2], .required = true},
    };
    tb_xinput_functions xinput = {0};
    tb_xinput *made = NULL;

    if (tb_xinput_load(&library, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every Windows since Vista has XInput */
        return tb_gamepad_fail(error, TB_GAMEPAD_UNAVAILABLE, "no XInput: %s",
                               library_error.message);
        /* coverage-on */
    }
    if (tb_library_bind(library, bindings, 3, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every XInput exports the three */
        tb_library_close(library);
        return tb_gamepad_fail(error, TB_GAMEPAD_UNAVAILABLE, "no XInput: %s",
                               library_error.message);
        /* coverage-on */
    }
    xinput.get_state = (tb_xinput_get_state)functions[0];
    xinput.set_state = (tb_xinput_set_state)functions[1];
    xinput.get_capabilities = (tb_xinput_get_capabilities)functions[2];
    made = tb_xinput_make(&xinput);
    made->library = library;
    *native = made;
    return TB_GAMEPAD_OK;
}

tb_gamepad_result tb_native_gamepad_open(const tb_gamepad_ops **ops, void **native,
                                         tb_gamepad_error *error)
{
    tb_gamepad_mux_child children[2] = {0};
    size_t count = 0;
    tb_gamepad_error xinput_error = TB_GAMEPAD_ERROR_INIT;
    tb_gamepad_error hid_error = TB_GAMEPAD_ERROR_INIT;
    void *backend = NULL;

    if (tb_xinput_open(&backend, &xinput_error) == TB_GAMEPAD_OK)
    {
        children[count] = (tb_gamepad_mux_child){&tb_xinput_ops, backend};
        count++;
    }
    if (tb_hid_gamepads_open(&backend, &hid_error) == TB_GAMEPAD_OK)
    {
        children[count] = (tb_gamepad_mux_child){&tb_hid_gamepad_ops, backend};
        count++;
    }
    if (count == 0)
    {
        /* coverage-off: every Windows since Vista has XInput */
        return tb_gamepad_fail(error, TB_GAMEPAD_UNAVAILABLE, "%s; %s", xinput_error.message,
                               hid_error.message);
        /* coverage-on */
    }
    *ops = &tb_gamepad_mux_ops;
    *native = tb_gamepad_mux_make(children, count);
    return TB_GAMEPAD_OK;
}
