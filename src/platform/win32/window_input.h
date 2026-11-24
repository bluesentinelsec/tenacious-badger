/* Private to Win32 windowing: what it does with input that a test cannot
 * have Windows send, because Windows sends it only to the window in front. */
#ifndef TB_WIN32_WINDOW_INPUT_H
#define TB_WIN32_WINDOW_INPUT_H

#include "../shared/window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Report a Raw Input mouse's motion as relative motion of a window in
 * relative mouse mode. A mouse that reports positions, as a remote
 * desktop's does, reports the change from its last one. */
void tb_win32_raw_mouse(tb_window *window, const RAWMOUSE *mouse);

#endif
