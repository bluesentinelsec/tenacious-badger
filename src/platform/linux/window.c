/* Linux windowing: Wayland where a compositor runs, else X11, each loaded at
 * run time; and what the two share, the keys and the wake. */
#define _GNU_SOURCE
#include "window_linux.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

tb_window_result tb_native_window_open(tb_window_system *system, tb_window_backend requested,
                                       tb_window_error *error)
{
    tb_window_result result = TB_WINDOW_OK;

    switch (requested)
    {
    case TB_WINDOW_BACKEND_WAYLAND:
        return tb_wayland_open(system, error);
    case TB_WINDOW_BACKEND_X11:
        return tb_x11_open(system, error);
    case TB_WINDOW_BACKEND_AUTOMATIC:
        result = tb_wayland_open(system, error);
        if (result == TB_WINDOW_UNAVAILABLE)
        {
            result = tb_x11_open(system, error);
        }
        return result;
    case TB_WINDOW_BACKEND_WIN32:
    case TB_WINDOW_BACKEND_COCOA:
        break;
    }
    return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s windowing is not on Linux",
                          tb_window_backend_name(requested));
}

/* ---------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

typedef struct tb_linux_key_row
{
    uint16_t code;
    tb_key key;
} tb_linux_key_row;

/* linux/input-event-codes.h. */
static const tb_linux_key_row tb_linux_keys[] = {
    {1, TB_KEY_ESCAPE},
    {2, TB_KEY_1},
    {3, TB_KEY_2},
    {4, TB_KEY_3},
    {5, TB_KEY_4},
    {6, TB_KEY_5},
    {7, TB_KEY_6},
    {8, TB_KEY_7},
    {9, TB_KEY_8},
    {10, TB_KEY_9},
    {11, TB_KEY_0},
    {14, TB_KEY_BACKSPACE},
    {15, TB_KEY_TAB},
    {16, TB_KEY_Q},
    {17, TB_KEY_W},
    {18, TB_KEY_E},
    {19, TB_KEY_R},
    {20, TB_KEY_T},
    {21, TB_KEY_Y},
    {22, TB_KEY_U},
    {23, TB_KEY_I},
    {24, TB_KEY_O},
    {25, TB_KEY_P},
    {28, TB_KEY_ENTER},
    {29, TB_KEY_LEFT_CONTROL},
    {30, TB_KEY_A},
    {31, TB_KEY_S},
    {32, TB_KEY_D},
    {33, TB_KEY_F},
    {34, TB_KEY_G},
    {35, TB_KEY_H},
    {36, TB_KEY_J},
    {37, TB_KEY_K},
    {38, TB_KEY_L},
    {41, TB_KEY_GRAVE},
    {42, TB_KEY_LEFT_SHIFT},
    {44, TB_KEY_Z},
    {45, TB_KEY_X},
    {46, TB_KEY_C},
    {47, TB_KEY_V},
    {48, TB_KEY_B},
    {49, TB_KEY_N},
    {50, TB_KEY_M},
    {54, TB_KEY_RIGHT_SHIFT},
    {56, TB_KEY_LEFT_ALT},
    {57, TB_KEY_SPACE},
    {59, TB_KEY_F1},
    {60, TB_KEY_F2},
    {61, TB_KEY_F3},
    {62, TB_KEY_F4},
    {63, TB_KEY_F5},
    {64, TB_KEY_F6},
    {65, TB_KEY_F7},
    {66, TB_KEY_F8},
    {67, TB_KEY_F9},
    {68, TB_KEY_F10},
    {87, TB_KEY_F11},
    {88, TB_KEY_F12},
    {96, TB_KEY_ENTER},
    {97, TB_KEY_RIGHT_CONTROL},
    {100, TB_KEY_RIGHT_ALT},
    {103, TB_KEY_UP},
    {105, TB_KEY_LEFT},
    {106, TB_KEY_RIGHT},
    {108, TB_KEY_DOWN},
};

tb_key tb_linux_key(uint32_t code)
{
    for (size_t index = 0; index < sizeof(tb_linux_keys) / sizeof(tb_linux_keys[0]); index++)
    {
        if (tb_linux_keys[index].code == code)
        {
            return tb_linux_keys[index].key;
        }
    }
    return TB_KEY_UNKNOWN;
}

/* ---------------------------------------------------------------------------
 * Waking and waiting.
 * ------------------------------------------------------------------------- */

bool tb_linux_waker_open(tb_linux_waker *waker)
{
    int ends[2] = {-1, -1};

    if (pipe2(ends, O_NONBLOCK | O_CLOEXEC) != 0)
    {
        /* coverage-off: only a process out of descriptors has no pipe */
        return false;
        /* coverage-on */
    }
    waker->read_end = ends[0];
    waker->write_end = ends[1];
    return true;
}

void tb_linux_waker_close(tb_linux_waker *waker)
{
    (void)close(waker->read_end);
    (void)close(waker->write_end);
    waker->read_end = -1;
    waker->write_end = -1;
}

void tb_linux_waker_wake(tb_linux_waker *waker)
{
    const char byte = 1;

    /* A full pipe already holds a wake. */
    (void)write(waker->write_end, &byte, 1);
}

bool tb_linux_waker_drain(tb_linux_waker *waker)
{
    char bytes[64] = {0};
    bool woken = false;

    while (read(waker->read_end, bytes, sizeof(bytes)) > 0)
    {
        woken = true;
    }
    return woken;
}

unsigned tb_linux_wait(int first, int second, uint64_t nanoseconds)
{
    struct pollfd descriptors[2] = {{.fd = first, .events = POLLIN},
                                    {.fd = second, .events = POLLIN}};
    const uint64_t milliseconds = (nanoseconds + 999999u) / 1000000u;
    const int timeout = milliseconds > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)milliseconds;
    unsigned ready = 0;

    if (poll(descriptors, 2, timeout) <= 0)
    {
        return 0;
    }
    ready |= (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 ? 1u : 0u;
    ready |= (descriptors[1].revents & POLLIN) != 0 ? 2u : 0u;
    return ready;
}
