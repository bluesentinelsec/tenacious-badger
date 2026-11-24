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

/* The button a Linux input event code names: evdev's BTN_* values, which
 * Wayland reports; none for a code of no mouse button. */
tb_mouse_button tb_linux_button(uint32_t code);

/* How far an XInput2 raw motion event moved, from its valuators: the mask
 * of those it has, `mask_length` bytes, and their values, in order. A
 * valuator it lacks moved 0. */
void tb_x11_raw_motion(const unsigned char *mask, int mask_length, const double *raw_values,
                       double *delta_x, double *delta_y);

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
