/* Private to the platform adapters and their tests: one backend over
 * several, as Windows reads XInput's pads and raw HID pads side by side. */
#ifndef TB_PLATFORM_SHARED_GAMEPAD_MUX_H
#define TB_PLATFORM_SHARED_GAMEPAD_MUX_H

#include "gamepad.h"

#include <stddef.h>

/* The most backends one mux reads. */
#define TB_GAMEPAD_MUX_CAPACITY 4u

/* A backend and its state, as a mux is given them. */
typedef struct tb_gamepad_mux_child
{
    const tb_gamepad_ops *ops;
    void *native;
} tb_gamepad_mux_child;

/* The mux's table. Each child polls into slots of its own; a pad that
 * comes takes the lowest slot of the system that is free, whichever child
 * has it, and keeps it until it goes. When every slot is taken a pad waits
 * for one. Rumble goes to the child that has the slot's pad; closing
 * closes every child. */
extern const tb_gamepad_ops tb_gamepad_mux_ops;

/* A mux over `count` children, from 1 to #TB_GAMEPAD_MUX_CAPACITY, which
 * it owns from now on. For tb_gamepad_system_make() with
 * tb_gamepad_mux_ops. */
void *tb_gamepad_mux_make(const tb_gamepad_mux_child *children, size_t count);

#endif
