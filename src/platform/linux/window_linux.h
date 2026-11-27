/* Private to Linux windowing: the two backends, and what they share. */
#ifndef TB_LINUX_WINDOW_H
#define TB_LINUX_WINDOW_H

#include "../shared/window.h"

#include <stdbool.h>
#include <stdint.h>

/* Connect to a Wayland compositor; UNAVAILABLE without one. */
tb_window_result tb_wayland_open(tb_window_system *system, tb_window_error *error);

/* Connect to an X server; UNAVAILABLE without one. */
tb_window_result tb_x11_open(tb_window_system *system, tb_window_error *error);

/* Wayland and X11 have no dialog of their own to ask a question with. */
tb_window_result tb_linux_confirm(tb_window *window, const char *title, const char *message,
                                  bool *confirmed);

/* The key a Linux input event code names: evdev's KEY_* values, which both
 * Wayland and X11 (less 8) report. */
tb_key tb_linux_key(uint32_t code);
uint32_t tb_linux_keycode(tb_key key);

/* The button a Linux input event code names: evdev's BTN_* values, which
 * Wayland reports; none for a code of no mouse button. */
tb_mouse_button tb_linux_button(uint32_t code);

/* How far an XInput2 raw motion event moved, from its valuators: the mask
 * of those it has, `mask_length` bytes, and their values, in order. A
 * valuator it lacks moved 0. */
void tb_x11_raw_motion(const unsigned char *mask, int mask_length, const double *raw_values,
                       double *delta_x, double *delta_y);

/* Read the compositor's entire NUL-terminated keymap from byte zero. The
 * caller frees the returned text and always retains ownership of fd. */
char *tb_wl_read_keymap(int fd, uint32_t size);

/* Wayland's repeating key, timed against the monotonic clock. */
typedef struct tb_wl_repeat
{
    uint64_t delay;
    uint64_t interval;
    uint64_t started;
    uint64_t duration;
    uint32_t key;
    bool active;
} tb_wl_repeat;

void tb_wl_repeat_configure(tb_wl_repeat *repeat, int32_t rate, int32_t delay, uint64_t now);
void tb_wl_repeat_key(tb_wl_repeat *repeat, uint32_t key, bool down, bool repeatable, uint64_t now);
uint64_t tb_wl_repeat_wait(const tb_wl_repeat *repeat, uint64_t now, uint64_t timeout);
bool tb_wl_repeat_due(tb_wl_repeat *repeat, uint64_t now);

/* A pipe that wakes a backend's wait, both ends non-blocking. */
typedef struct tb_linux_waker
{
    int read_end;
    int write_end;
} tb_linux_waker;

bool tb_linux_waker_open(tb_linux_waker *waker);
void tb_linux_waker_close(tb_linux_waker *waker);
/* Thread-safe. */
void tb_linux_waker_wake(tb_linux_waker *waker);
/* Empty the pipe; true when it held a wake. */
bool tb_linux_waker_drain(tb_linux_waker *waker);

/* Wait until one of two descriptors can be read, or time passes; returns
 * which could: bit 0 for the first, bit 1 for the second. */
unsigned tb_linux_wait(int first, int second, uint64_t nanoseconds);

#endif
