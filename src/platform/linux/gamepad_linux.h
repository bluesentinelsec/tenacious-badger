/* Private to the Linux gamepad backend and its tests: the system calls it
 * makes on devices, which a test replaces with devices of its own, and a
 * backend over a directory other than /dev/input. */
#ifndef TB_LINUX_GAMEPAD_LINUX_H
#define TB_LINUX_GAMEPAD_LINUX_H

#include "../shared/gamepad.h"

#include <stddef.h>
#include <sys/types.h>

/* The calls the backend makes on a device node, each required, as the
 * system's own: open(2), close(2), read(2), write(2) and ioctl(2). */
typedef struct tb_linux_device_calls
{
    int (*open)(const char *path, int flags);
    int (*close)(int descriptor);
    ssize_t (*read)(int descriptor, void *buffer, size_t size);
    ssize_t (*write)(int descriptor, const void *buffer, size_t size);
    int (*ioctl)(int descriptor, unsigned long request, void *argument);
} tb_linux_device_calls;

/* The backend's table. */
extern const tb_gamepad_ops tb_linux_gamepad_ops;

/* A backend that reads the devices of `directory` through `calls`, and
 * watches the directory for devices that come and go. `ignored` is a list
 * as SDL_GAMECONTROLLER_IGNORE_DEVICES holds, or NULL; it is copied. For
 * tb_gamepad_system_make() with tb_linux_gamepad_ops. */
void *tb_linux_gamepads_make(const tb_linux_device_calls *calls, const char *directory,
                             const char *ignored);

#endif
