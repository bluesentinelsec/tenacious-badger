/* Private to the Windows gamepad backend and its tests: the XInput
 * functions it calls, which a test replaces with its own pads. */
#ifndef TB_WIN32_GAMEPAD_XINPUT_H
#define TB_WIN32_GAMEPAD_XINPUT_H

#include "../shared/gamepad.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <xinput.h>

/* The users XInput has: its slots. */
#define TB_XINPUT_USER_COUNT 4u

/* How often slots without a pad are asked whether one came. XInput is slow
 * to answer for an empty slot, so the backend does not ask every frame. */
#define TB_XINPUT_PROBE_NANOSECONDS 500000000u

typedef DWORD(WINAPI *tb_xinput_get_state)(DWORD user, XINPUT_STATE *state);
typedef DWORD(WINAPI *tb_xinput_set_state)(DWORD user, XINPUT_VIBRATION *vibration);
typedef DWORD(WINAPI *tb_xinput_get_capabilities)(DWORD user, DWORD flags,
                                                  XINPUT_CAPABILITIES *capabilities);

/* The functions the backend calls: XInputGetState, XInputSetState and
 * XInputGetCapabilities, each required. */
typedef struct tb_xinput_functions
{
    tb_xinput_get_state get_state;
    tb_xinput_set_state set_state;
    tb_xinput_get_capabilities get_capabilities;
} tb_xinput_functions;

/* The backend's table. */
extern const tb_gamepad_ops tb_xinput_ops;

/* A backend over functions of a test's own, for tb_gamepad_system_make()
 * with tb_xinput_ops; it owns no library. */
void *tb_xinput_make(const tb_xinput_functions *functions);

/* A pad's state as XInput reports it, in the standard ranges. */
void tb_xinput_state(const XINPUT_GAMEPAD *gamepad, tb_gamepad_state *state);

#endif
