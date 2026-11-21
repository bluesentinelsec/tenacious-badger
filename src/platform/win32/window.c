/* Windows: Win32 windows, per-monitor DPI, and fullscreen by changing the
 * display's mode.
 *
 * The window system's thread runs per-monitor aware (version 2) while it is
 * open, so sizes are pixels, monitors report their own scale, and a window
 * moved to another monitor is told to rescale. user32 and gdi32 are among
 * the libraries the toolchain links by default; shcore, for a monitor's
 * scale, is loaded through the library interface. */
#include "../shared/window.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/utf8.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

/* The message that wakes the window system's thread. */
#define TB_WIN32_WAKE (WM_APP + 1)

/* The arrow cursor, IDC_ARROW, as the wide resource name it is. */
#define TB_WIN32_ARROW MAKEINTRESOURCEW(32512)

/* The DPI at which a logical unit is a pixel. */
#define TB_WIN32_BASE_DPI 96.0

/* What GetDpiForMonitor() asks for: the scale the user chose. */
#define TB_WIN32_EFFECTIVE_DPI 0

/* GetDpiForMonitor(), from shcore. */
typedef HRESULT(WINAPI *tb_win32_dpi_for_monitor)(HMONITOR monitor, int type, UINT *x, UINT *y);

typedef struct tb_win32_system
{
    HINSTANCE instance;
    wchar_t window_class[64];
    wchar_t wake_class[64];
    HWND wake;
    DPI_AWARENESS_CONTEXT previous_awareness;
    tb_library *shcore;
    tb_win32_dpi_for_monitor dpi_for_monitor;
} tb_win32_system;

typedef struct tb_win32_window
{
    HWND handle;
    /* The monitor whose mode a fullscreen window changed, and that it gives
     * back; empty when the mode is the monitor's own. */
    wchar_t changed_device[CCHDEVICENAME];
    bool applying; /* Inside tb_win32_apply(): size messages are expected. */
} tb_win32_window;

static tb_win32_system *tb_win32_of(const tb_window_system *system)
{
    return system->native;
}

static tb_win32_window *tb_win32_window_of(const tb_window *window)
{
    return window->native;
}

/* The last error, in the system's words, for a message. */
static void tb_win32_describe(DWORD code, char *buffer, size_t capacity)
{
    wchar_t text[256] = {0};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        NULL, code, 0, text, 256, NULL);
    size_t written = 0;

    /* coverage-off: only a code the system cannot describe has no text */
    if (length == 0 ||
        !tb_utf8_from_utf16((const uint16_t *)text, length, buffer, capacity - 1, &written))
    {
        (void)tb_fmt_buffer(buffer, capacity, "error %lu", (unsigned long)code);
        return;
    }
    /* coverage-on */
    while (written > 0 && (buffer[written - 1] == '\n' || buffer[written - 1] == '\r' ||
                           buffer[written - 1] == '.'))
    {
        written--;
    }
    buffer[written] = '\0';
}

static tb_window_result tb_win32_fail(tb_window_error *error, const char *what)
{
    char reason[256] = {0};

    tb_win32_describe(GetLastError(), reason, sizeof(reason));
    return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR, "%s: %s", what, reason);
}

/* UTF-8 to a new UTF-16 string of the heap; NULL for text that is not UTF-8. */
static wchar_t *tb_win32_wide(const char *text)
{
    const size_t length = tb_str_length(text);
    size_t units = 0;
    wchar_t *wide = NULL;

    if (!tb_utf8_to_utf16(text, length, NULL, 0, &units))
    {
        return NULL;
    }
    wide = tb_calloc(units + 1, sizeof(wchar_t));
    (void)tb_utf8_to_utf16(text, length, (uint16_t *)wide, units, &units);
    wide[units] = L'\0';
    return wide;
}

static void tb_win32_narrow(const wchar_t *text, char *buffer, size_t capacity)
{
    size_t written = 0;

    if (!tb_utf8_from_utf16((const uint16_t *)text, wcslen(text), buffer, capacity - 1, &written))
    {
        written = 0;
    }
    buffer[written] = '\0';
}

/* ---------------------------------------------------------------------------
 * Displays.
 * ------------------------------------------------------------------------- */

typedef struct tb_win32_monitors
{
    const tb_win32_system *native;
    tb_display displays[64];
    wchar_t devices[64][CCHDEVICENAME];
    size_t count;
} tb_win32_monitors;

static double tb_win32_monitor_scale(const tb_win32_system *native, HMONITOR monitor)
{
    UINT x = 0;
    UINT y = 0;

    if (native->dpi_for_monitor == NULL ||
        FAILED(native->dpi_for_monitor(monitor, TB_WIN32_EFFECTIVE_DPI, &x, &y)) || x == 0)
    {
        /* coverage-off: shcore is part of every Windows the engine runs on */
        return 1.0;
        /* coverage-on */
    }
    return (double)x / TB_WIN32_BASE_DPI;
}

static void tb_win32_describe_monitor(const tb_win32_system *native, HMONITOR monitor,
                                      tb_display *display, wchar_t *device)
{
    MONITORINFOEXW info = {.cbSize = sizeof(MONITORINFOEXW)};
    DEVMODEW mode = {.dmSize = sizeof(DEVMODEW)};
    DISPLAY_DEVICEW device_info = {.cb = sizeof(DISPLAY_DEVICEW)};
    HDC context = NULL;

    (void)GetMonitorInfoW(monitor, (MONITORINFO *)&info);
    memcpy(device, info.szDevice, sizeof(info.szDevice));
    display->primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    display->x = info.rcMonitor.left;
    display->y = info.rcMonitor.top;
    display->width = (uint32_t)(info.rcMonitor.right - info.rcMonitor.left);
    display->height = (uint32_t)(info.rcMonitor.bottom - info.rcMonitor.top);
    display->usable_x = info.rcWork.left;
    display->usable_y = info.rcWork.top;
    display->usable_width = (uint32_t)(info.rcWork.right - info.rcWork.left);
    display->usable_height = (uint32_t)(info.rcWork.bottom - info.rcWork.top);
    display->pixel_width = display->width;
    display->pixel_height = display->height;
    display->content_scale = tb_win32_monitor_scale(native, monitor);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode))
    {
        display->pixel_width = mode.dmPelsWidth;
        display->pixel_height = mode.dmPelsHeight;
        /* 0 and 1 mean the hardware's default rate, which is unknown. */
        display->refresh_millihertz =
            mode.dmDisplayFrequency > 1 ? (uint32_t)mode.dmDisplayFrequency * 1000u : 0u;
    }
    if (EnumDisplayDevicesW(info.szDevice, 0, &device_info, 0))
    {
        tb_win32_narrow(device_info.DeviceString, display->name, sizeof(display->name));
    }
    context = CreateDCW(L"DISPLAY", info.szDevice, NULL, NULL);
    if (context != NULL)
    {
        display->physical_width_mm = (uint32_t)GetDeviceCaps(context, HORZSIZE);
        display->physical_height_mm = (uint32_t)GetDeviceCaps(context, VERTSIZE);
        (void)DeleteDC(context);
    }
}

static BOOL CALLBACK tb_win32_add_monitor(HMONITOR monitor, HDC context, LPRECT rectangle,
                                          LPARAM parameter)
{
    tb_win32_monitors *const monitors = (tb_win32_monitors *)parameter;

    (void)context;
    (void)rectangle;
    if (monitors->count == sizeof(monitors->displays) / sizeof(monitors->displays[0]))
    {
        /* coverage-off: no machine has more monitors than this */
        return FALSE;
        /* coverage-on */
    }
    tb_win32_describe_monitor(monitors->native, monitor, &monitors->displays[monitors->count],
                              monitors->devices[monitors->count]);
    monitors->count++;
    return TRUE;
}

static void tb_win32_read_displays(tb_window_system *system)
{
    tb_win32_monitors *const monitors = tb_calloc(1, sizeof(tb_win32_monitors));

    monitors->native = tb_win32_of(system);
    (void)EnumDisplayMonitors(NULL, NULL, tb_win32_add_monitor, (LPARAM)monitors);
    tb_window_set_displays(system, monitors->displays, monitors->count);
    tb_free(monitors);
}

/* The device name of the display a window system lists at an index, for
 * changing its mode. */
static void tb_win32_display_device(const tb_window_system *system, size_t index, wchar_t *device)
{
    tb_display wanted = {0};
    const POINT corner = {0, 0};
    HMONITOR monitor = NULL;
    MONITORINFOEXW info = {.cbSize = sizeof(MONITORINFOEXW)};

    tb_window_system_display(system, index, &wanted);
    monitor = MonitorFromPoint(
        (POINT){wanted.x + (LONG)wanted.width / 2, wanted.y + (LONG)wanted.height / 2},
        MONITOR_DEFAULTTOPRIMARY);
    if (monitor == NULL)
    {
        /* coverage-off: MONITOR_DEFAULTTOPRIMARY always finds one */
        monitor = MonitorFromPoint(corner, MONITOR_DEFAULTTOPRIMARY);
        /* coverage-on */
    }
    (void)GetMonitorInfoW(monitor, (MONITORINFO *)&info);
    memcpy(device, info.szDevice, sizeof(info.szDevice));
}

/* ---------------------------------------------------------------------------
 * A window's size and place.
 * ------------------------------------------------------------------------- */

static double tb_win32_window_scale(HWND handle)
{
    const UINT dpi = GetDpiForWindow(handle);

    return dpi == 0 ? 1.0 : (double)dpi / TB_WIN32_BASE_DPI;
}

/* Read the window's client area and scale into its state, with events. */
static void tb_win32_read_size(tb_window *window)
{
    const HWND handle = tb_win32_window_of(window)->handle;
    RECT client = {0};
    RECT frame = {0};

    (void)GetClientRect(handle, &client);
    (void)GetWindowRect(handle, &frame);
    if (client.right > 0 && client.bottom > 0)
    {
        tb_window_resized(window, (uint32_t)client.right, (uint32_t)client.bottom,
                          tb_win32_window_scale(handle));
    }
    window->state.display = tb_window_display_at(window->system, frame.left, frame.top,
                                                 (uint32_t)(frame.right - frame.left),
                                                 (uint32_t)(frame.bottom - frame.top));
}

static DWORD tb_win32_windowed_style(const tb_window_options *options)
{
    if (options->fixed_size)
    {
        return WS_OVERLAPPEDWINDOW & ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    return WS_OVERLAPPEDWINDOW;
}

/* Give back the mode a fullscreen window set on its monitor. */
static void tb_win32_restore_mode(tb_win32_window *native)
{
    if (native->changed_device[0] == L'\0')
    {
        return;
    }
    (void)ChangeDisplaySettingsExW(native->changed_device, NULL, NULL, 0, NULL);
    native->changed_device[0] = L'\0';
}

/* Set the fullscreen mode the options ask for on a monitor; false when the
 * monitor has no such mode, which leaves the window borderless. */
static bool tb_win32_set_mode(tb_window *window, const wchar_t *device)
{
    tb_win32_window *const native = tb_win32_window_of(window);
    const tb_window_options *const options = &window->options;
    DEVMODEW mode = {.dmSize = sizeof(DEVMODEW)};

    if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode))
    {
        /* coverage-off: the current settings of a listed monitor are always readable */
        return false;
        /* coverage-on */
    }
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (options->fullscreen_width != 0 && options->fullscreen_height != 0)
    {
        mode.dmPelsWidth = options->fullscreen_width;
        mode.dmPelsHeight = options->fullscreen_height;
    }
    if (options->refresh_rate != 0)
    {
        mode.dmDisplayFrequency = options->refresh_rate;
        mode.dmFields |= DM_DISPLAYFREQUENCY;
    }
    if (ChangeDisplaySettingsExW(device, &mode, NULL, CDS_FULLSCREEN, NULL) !=
        DISP_CHANGE_SUCCESSFUL)
    {
        return false;
    }
    memcpy(native->changed_device, device, sizeof(native->changed_device));
    return true;
}

/* Cover a monitor with the window: borderless, or fullscreen once its mode
 * is set. */
static void tb_win32_cover(HWND handle, const wchar_t *device, HWND after)
{
    DEVMODEW mode = {.dmSize = sizeof(DEVMODEW)};

    (void)EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode);
    (void)SetWindowLongPtrW(handle, GWL_STYLE, (LONG_PTR)(WS_POPUP | WS_VISIBLE));
    (void)SetWindowPos(handle, after, mode.dmPosition.x, mode.dmPosition.y, (int)mode.dmPelsWidth,
                       (int)mode.dmPelsHeight, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void tb_win32_place_windowed(tb_window *window)
{
    const HWND handle = tb_win32_window_of(window)->handle;
    tb_window_placement placement = {0};
    tb_display display = {0};
    const DWORD style = tb_win32_windowed_style(&window->options);
    RECT frame = {0};
    UINT dpi = 0;

    tb_window_place(window, true, &placement);
    tb_window_system_display(window->system, placement.display, &display);
    dpi = (UINT)(display.content_scale * TB_WIN32_BASE_DPI + 0.5);
    frame = (RECT){placement.x, placement.y, placement.x + (LONG)placement.unit_width,
                   placement.y + (LONG)placement.unit_height};
    (void)AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    (void)SetWindowLongPtrW(handle, GWL_STYLE, (LONG_PTR)(style | WS_VISIBLE));
    (void)SetWindowPos(handle, HWND_NOTOPMOST, frame.left, frame.top, frame.right - frame.left,
                       frame.bottom - frame.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

/* Put the window in the mode, on the display, at the size its options
 * say. */
static void tb_win32_apply(tb_window *window)
{
    tb_win32_window *const native = tb_win32_window_of(window);
    tb_window_placement placement = {0};
    wchar_t device[CCHDEVICENAME] = {0};
    tb_window_mode mode = window->options.mode;

    native->applying = true;
    tb_win32_restore_mode(native);
    /* A window minimized while away is shown again in its mode. */
    if (IsIconic(native->handle))
    {
        (void)ShowWindow(native->handle, SW_RESTORE);
    }
    tb_window_place(window, true, &placement);
    tb_win32_display_device(window->system, placement.display, device);
    if (mode == TB_WINDOW_MODE_FULLSCREEN && !tb_win32_set_mode(window, device))
    {
        mode = TB_WINDOW_MODE_BORDERLESS;
    }
    switch (mode)
    {
    case TB_WINDOW_MODE_WINDOWED:
        tb_win32_place_windowed(window);
        break;
    case TB_WINDOW_MODE_BORDERLESS:
        tb_win32_cover(native->handle, device, HWND_TOP);
        break;
    case TB_WINDOW_MODE_FULLSCREEN:
        tb_win32_cover(native->handle, device, HWND_TOPMOST);
        break;
    }
    window->state.mode = mode;
    native->applying = false;
    tb_win32_read_size(window);
}

/* ---------------------------------------------------------------------------
 * Messages.
 * ------------------------------------------------------------------------- */

/* Set-1 scancodes, with 0xE000 for the extended ones, and the keys they
 * are. */
typedef struct tb_win32_key
{
    uint16_t scancode;
    tb_key key;
} tb_win32_key;

static const tb_win32_key tb_win32_keys[] = {
    {0x01, TB_KEY_ESCAPE},
    {0x02, TB_KEY_1},
    {0x03, TB_KEY_2},
    {0x04, TB_KEY_3},
    {0x05, TB_KEY_4},
    {0x06, TB_KEY_5},
    {0x07, TB_KEY_6},
    {0x08, TB_KEY_7},
    {0x09, TB_KEY_8},
    {0x0A, TB_KEY_9},
    {0x0B, TB_KEY_0},
    {0x0E, TB_KEY_BACKSPACE},
    {0x0F, TB_KEY_TAB},
    {0x10, TB_KEY_Q},
    {0x11, TB_KEY_W},
    {0x12, TB_KEY_E},
    {0x13, TB_KEY_R},
    {0x14, TB_KEY_T},
    {0x15, TB_KEY_Y},
    {0x16, TB_KEY_U},
    {0x17, TB_KEY_I},
    {0x18, TB_KEY_O},
    {0x19, TB_KEY_P},
    {0x1C, TB_KEY_ENTER},
    {0x1D, TB_KEY_LEFT_CONTROL},
    {0x1E, TB_KEY_A},
    {0x1F, TB_KEY_S},
    {0x20, TB_KEY_D},
    {0x21, TB_KEY_F},
    {0x22, TB_KEY_G},
    {0x23, TB_KEY_H},
    {0x24, TB_KEY_J},
    {0x25, TB_KEY_K},
    {0x26, TB_KEY_L},
    {0x29, TB_KEY_GRAVE},
    {0x2A, TB_KEY_LEFT_SHIFT},
    {0x2C, TB_KEY_Z},
    {0x2D, TB_KEY_X},
    {0x2E, TB_KEY_C},
    {0x2F, TB_KEY_V},
    {0x30, TB_KEY_B},
    {0x31, TB_KEY_N},
    {0x32, TB_KEY_M},
    {0x36, TB_KEY_RIGHT_SHIFT},
    {0x38, TB_KEY_LEFT_ALT},
    {0x39, TB_KEY_SPACE},
    {0x3B, TB_KEY_F1},
    {0x3C, TB_KEY_F2},
    {0x3D, TB_KEY_F3},
    {0x3E, TB_KEY_F4},
    {0x3F, TB_KEY_F5},
    {0x40, TB_KEY_F6},
    {0x41, TB_KEY_F7},
    {0x42, TB_KEY_F8},
    {0x43, TB_KEY_F9},
    {0x44, TB_KEY_F10},
    {0x57, TB_KEY_F11},
    {0x58, TB_KEY_F12},
    {0xE01C, TB_KEY_ENTER},
    {0xE01D, TB_KEY_RIGHT_CONTROL},
    {0xE038, TB_KEY_RIGHT_ALT},
    {0xE048, TB_KEY_UP},
    {0xE04B, TB_KEY_LEFT},
    {0xE04D, TB_KEY_RIGHT},
    {0xE050, TB_KEY_DOWN},
};

static tb_key tb_win32_key_of(uint32_t scancode)
{
    for (size_t index = 0; index < sizeof(tb_win32_keys) / sizeof(tb_win32_keys[0]); index++)
    {
        if (tb_win32_keys[index].scancode == scancode)
        {
            return tb_win32_keys[index].key;
        }
    }
    return TB_KEY_UNKNOWN;
}

static void tb_win32_on_key(tb_window *window, LPARAM parameter, bool down)
{
    const uint32_t flags = (uint32_t)((uintptr_t)parameter >> 16);
    const uint32_t scancode = (flags & 0xFFu) | ((flags & KF_EXTENDED) != 0 ? 0xE000u : 0u);
    const bool repeat = (flags & KF_REPEAT) != 0;

    tb_window_key(window, tb_win32_key_of(scancode), scancode, down, repeat);
}

/* The active app changed: a fullscreen window gives the display back while
 * the player is elsewhere, and takes it again on return. */
static void tb_win32_on_activate_app(tb_window *window, bool active)
{
    tb_win32_window *const native = tb_win32_window_of(window);

    if (window->state.mode != TB_WINDOW_MODE_FULLSCREEN)
    {
        return;
    }
    if (active)
    {
        tb_win32_apply(window);
        return;
    }
    tb_win32_restore_mode(native);
    (void)ShowWindow(native->handle, SW_MINIMIZE);
}

static bool tb_win32_on_message(tb_window *window, UINT message, WPARAM word, LPARAM parameter,
                                LRESULT *answer)
{
    tb_win32_window *const native = tb_win32_window_of(window);

    switch (message)
    {
    case WM_CLOSE: {
        const tb_window_event event = {
            .type = TB_WINDOW_EVENT_CLOSE_REQUESTED, .window = window, .state = window->state};
        tb_window_push(window->system, &event);
        *answer = 0;
        return true;
    }
    case WM_SIZE:
        tb_window_minimized(window, word == SIZE_MINIMIZED);
        if (word != SIZE_MINIMIZED)
        {
            tb_win32_read_size(window);
        }
        return false;
    case WM_SETFOCUS:
        tb_window_focused(window, true);
        return false;
    case WM_KILLFOCUS:
        tb_window_focused(window, false);
        return false;
    case WM_ACTIVATEAPP:
        tb_win32_on_activate_app(window, word != FALSE);
        return false;
    case WM_DPICHANGED:
        /* A windowed window takes the size Windows suggests for the new
         * scale; one that covers its monitor keeps covering it. */
        if (window->state.mode == TB_WINDOW_MODE_WINDOWED && !native->applying)
        {
            const RECT *const suggested = (const RECT *)parameter;
            (void)SetWindowPos(native->handle, NULL, suggested->left, suggested->top,
                               suggested->right - suggested->left,
                               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        tb_win32_read_size(window);
        *answer = 0;
        return true;
    case WM_DISPLAYCHANGE:
        tb_win32_read_displays(window->system);
        tb_window_displays_changed(window->system);
        return false;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        tb_win32_on_key(window, parameter, true);
        /* Alt+F4 closes, as everywhere; F10 and Alt alone would open a menu
         * the window does not have. */
        if (message == WM_SYSKEYDOWN && word == VK_F4)
        {
            return false;
        }
        *answer = 0;
        return true;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        tb_win32_on_key(window, parameter, false);
        *answer = 0;
        return true;
    case WM_SYSCOMMAND:
        if ((word & 0xFFF0u) == SC_KEYMENU)
        {
            *answer = 0;
            return true;
        }
        return false;
    case WM_ERASEBKGND: {
        RECT client = {0};
        (void)GetClientRect(native->handle, &client);
        (void)FillRect((HDC)word, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        *answer = 1;
        return true;
    }
    default:
        return false;
    }
}

static LRESULT CALLBACK tb_win32_window_procedure(HWND handle, UINT message, WPARAM word,
                                                  LPARAM parameter)
{
    tb_window *window = (tb_window *)GetWindowLongPtrW(handle, GWLP_USERDATA);
    LRESULT answer = 0;

    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW *const creation = (const CREATESTRUCTW *)parameter;
        window = creation->lpCreateParams;
        (void)SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)window);
        tb_win32_window_of(window)->handle = handle;
    }
    if (window != NULL && tb_win32_on_message(window, message, word, parameter, &answer))
    {
        return answer;
    }
    return DefWindowProcW(handle, message, word, parameter);
}

static LRESULT CALLBACK tb_win32_wake_procedure(HWND handle, UINT message, WPARAM word,
                                                LPARAM parameter)
{
    tb_window_system *const system = (tb_window_system *)GetWindowLongPtrW(handle, GWLP_USERDATA);

    if (message == TB_WIN32_WAKE && system != NULL)
    {
        tb_window_woken(system);
        return 0;
    }
    return DefWindowProcW(handle, message, word, parameter);
}

/* ---------------------------------------------------------------------------
 * The backend's table.
 * ------------------------------------------------------------------------- */

static void tb_win32_close(tb_window_system *system)
{
    tb_win32_system *const native = tb_win32_of(system);

    (void)DestroyWindow(native->wake);
    (void)UnregisterClassW(native->window_class, native->instance);
    (void)UnregisterClassW(native->wake_class, native->instance);
    (void)SetThreadDpiAwarenessContext(native->previous_awareness);
    tb_library_close(native->shcore);
    tb_free(native);
}

static void tb_win32_pump(tb_window_system *system, uint64_t nanoseconds)
{
    MSG message = {0};
    bool any = false;

    (void)system;

    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        (void)DispatchMessageW(&message);
        any = true;
    }
    if (any || nanoseconds == 0)
    {
        return;
    }
    {
        /* Milliseconds, rounded up so that a short wait still waits. */
        const uint64_t milliseconds = (nanoseconds + 999999u) / 1000000u;
        const DWORD timeout = milliseconds >= INFINITE ? INFINITE - 1u : (DWORD)milliseconds;
        (void)MsgWaitForMultipleObjectsEx(0, NULL, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
    {
        (void)DispatchMessageW(&message);
    }
}

static void tb_win32_wake(tb_window_system *system)
{
    (void)PostMessageW(tb_win32_of(system)->wake, TB_WIN32_WAKE, 0, 0);
}

static tb_window_result tb_win32_create(tb_window *window, tb_window_error *error)
{
    tb_win32_system *const system = tb_win32_of(window->system);
    tb_win32_window *const native = tb_calloc(1, sizeof(tb_win32_window));
    wchar_t *const title = tb_win32_wide(window->options.title);
    HWND handle = NULL;

    window->native = native;
    handle =
        CreateWindowExW(0, system->window_class, title != NULL ? title : L"",
                        tb_win32_windowed_style(&window->options), CW_USEDEFAULT, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, system->instance, window);
    tb_free(title);
    if (handle == NULL)
    {
        /* coverage-off: only a system out of window handles refuses one */
        const tb_window_result result = tb_win32_fail(error, "the window could not be created");
        tb_free(native);
        window->native = NULL;
        return result;
        /* coverage-on */
    }
    tb_win32_apply(window);
    (void)SetForegroundWindow(handle);
    tb_window_focused(window, GetForegroundWindow() == handle);
    return TB_WINDOW_OK;
}

static void tb_win32_destroy(tb_window *window)
{
    tb_win32_window *const native = tb_win32_window_of(window);

    tb_win32_restore_mode(native);
    (void)SetWindowLongPtrW(native->handle, GWLP_USERDATA, 0);
    (void)DestroyWindow(native->handle);
    tb_free(native);
}

static tb_window_result tb_win32_configure(tb_window *window, const tb_window_options *previous,
                                           tb_window_error *error)
{
    tb_win32_window *const native = tb_win32_window_of(window);
    const tb_window_options *const options = &window->options;
    wchar_t *const title = tb_win32_wide(options->title);

    (void)error;
    (void)SetWindowTextW(native->handle, title != NULL ? title : L"");
    tb_free(title);
    /* A windowed window keeps the place the player moved it to, unless its
     * mode, display, or size changes. */
    if (options->mode != previous->mode || options->display != previous->display ||
        options->width != previous->width || options->height != previous->height ||
        options->fixed_size != previous->fixed_size ||
        options->fullscreen_width != previous->fullscreen_width ||
        options->fullscreen_height != previous->fullscreen_height ||
        options->refresh_rate != previous->refresh_rate)
    {
        tb_win32_apply(window);
    }
    return TB_WINDOW_OK;
}

static void tb_win32_native(const tb_window *window, tb_window_native *native)
{
    native->system = tb_win32_of(window->system)->instance;
    native->window = tb_win32_window_of(window)->handle;
}

static const tb_window_ops tb_win32_ops = {
    .close = tb_win32_close,
    .pump = tb_win32_pump,
    .wake = tb_win32_wake,
    .create = tb_win32_create,
    .destroy = tb_win32_destroy,
    .configure = tb_win32_configure,
    .native = tb_win32_native,
};

static bool tb_win32_register(tb_win32_system *native, const wchar_t *name, WNDPROC procedure)
{
    const WNDCLASSEXW window_class = {.cbSize = sizeof(WNDCLASSEXW),
                                      .style = CS_OWNDC,
                                      .lpfnWndProc = procedure,
                                      .hInstance = native->instance,
                                      .hCursor = LoadCursorW(NULL, TB_WIN32_ARROW),
                                      .lpszClassName = name};

    return RegisterClassExW(&window_class) != 0;
}

/* A window class's name, of the window system's own, so that two systems,
 * or two copies of the engine, never share a class. */
static void tb_win32_class_name(const char *prefix, const tb_window_system *system, wchar_t *name)
{
    char narrow[64] = {0};
    size_t units = 0;
    const size_t length =
        tb_fmt_buffer(narrow, sizeof(narrow), "%s%p", prefix, (const void *)system);

    (void)tb_utf8_to_utf16(narrow, length, (uint16_t *)name, 63, &units);
    name[units] = 0;
}

static void tb_win32_load_shcore(tb_win32_system *native)
{
    tb_library_function function = NULL;

    if (tb_library_open("shcore.dll", TB_LIBRARY_SEARCH_SYSTEM, &native->shcore, NULL) !=
        TB_LIBRARY_OK)
    {
        /* coverage-off: shcore is part of every Windows the engine runs on */
        return;
        /* coverage-on */
    }
    if (tb_library_find_function(native->shcore, "GetDpiForMonitor", &function, NULL) ==
        TB_LIBRARY_OK)
    {
        native->dpi_for_monitor = (tb_win32_dpi_for_monitor)function;
    }
}

tb_window_result tb_native_window_open(tb_window_system *system, tb_window_backend requested,
                                       tb_window_error *error)
{
    tb_win32_system *native = NULL;

    if (requested != TB_WINDOW_BACKEND_AUTOMATIC && requested != TB_WINDOW_BACKEND_WIN32)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s windowing is not on Windows",
                              tb_window_backend_name(requested));
    }
    native = tb_calloc(1, sizeof(tb_win32_system));
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)(void *)tb_win32_window_procedure, &native->instance);
    tb_win32_class_name("TenaciousBadgerWindow", system, native->window_class);
    tb_win32_class_name("TenaciousBadgerWake", system, native->wake_class);
    native->previous_awareness =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    tb_win32_load_shcore(native);
    if (!tb_win32_register(native, native->window_class, tb_win32_window_procedure) ||
        !tb_win32_register(native, native->wake_class, tb_win32_wake_procedure))
    {
        /* coverage-off: a class of a name of its own is always registered */
        const tb_window_result result = tb_win32_fail(error, "the window class was refused");
        (void)UnregisterClassW(native->window_class, native->instance);
        (void)SetThreadDpiAwarenessContext(native->previous_awareness);
        tb_library_close(native->shcore);
        tb_free(native);
        return result;
        /* coverage-on */
    }
    native->wake = CreateWindowExW(0, native->wake_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                   native->instance, NULL);
    (void)SetWindowLongPtrW(native->wake, GWLP_USERDATA, (LONG_PTR)system);
    system->backend = TB_WINDOW_BACKEND_WIN32;
    system->ops = &tb_win32_ops;
    system->native = native;
    tb_win32_read_displays(system);
    return TB_WINDOW_OK;
}
