/* Private to the macOS gamepad backend and its tests. */
#ifndef TB_MACOS_GAMEPAD_MACOS_H
#define TB_MACOS_GAMEPAD_MACOS_H

#include "../shared/gamepad.h"

/* Put a controller in the lowest free slot, as one the framework reports
 * is: for tests, with a GCController that +controllerWithExtendedGamepad
 * made, whose elements a test sets. The backend keeps it, retained, until
 * the backend is closed; returns its slot, or TB_GAMEPAD_SLOT_COUNT when
 * every slot is taken. */
size_t tb_macos_gamepads_attach(void *native, void *controller);

#endif
