#include "badger/gamepad.h"

#include "not_implemented.h"

/* Standard C has no gamepads: how a system finds a controller and reads it
 * is each system's own. */

tb_gamepad_result tb_gamepad_system_open(tb_gamepad_system **system, tb_gamepad_error *error)
{
    (void)system;
    (void)error;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_system_open");
}

void tb_gamepad_system_close(tb_gamepad_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_system_close");
}

void tb_gamepad_system_poll(tb_gamepad_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_system_poll");
}

bool tb_gamepad_system_next_event(tb_gamepad_system *system, tb_gamepad_event *event)
{
    (void)system;
    (void)event;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_system_next_event");
}

void tb_gamepad_system_set_vibration(tb_gamepad_system *system, float scale)
{
    (void)system;
    (void)scale;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_system_set_vibration");
}

void tb_gamepad_system_set_focus(tb_gamepad_system *system, bool focused)
{
    (void)system;
    (void)focused;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_system_set_focus");
}

void tb_gamepad_get_state(const tb_gamepad_system *system, size_t slot, tb_gamepad_state *state)
{
    (void)system;
    (void)slot;
    (void)state;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_get_state");
}

void tb_gamepad_get_info(const tb_gamepad_system *system, size_t slot, tb_gamepad_info *info)
{
    (void)system;
    (void)slot;
    (void)info;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_get_info");
}

bool tb_gamepad_rumble(tb_gamepad_system *system, size_t slot, const tb_gamepad_rumble_desc *rumble)
{
    (void)system;
    (void)slot;
    (void)rumble;
    tb_default_not_implemented("platform.gamepad", "tb_gamepad_rumble");
}
