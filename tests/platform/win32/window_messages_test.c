/* The Win32 windowing turns the messages Windows sends a window into the
 * engine's events: keys by their scancodes, a close request that leaves the
 * window open, focus, minimizing, displays, a new scale, the mouse, and the
 * characters keys type; and while Windows holds the thread in its loop for
 * a drag, a timer and each new size refresh, and the game loop runs its
 * frames from there. Messages are sent to the window directly, as Windows
 * would send them, so that each is checked without a person at the keyboard
 * or the mouse. */
#include "badger/game_loop.h"
#include "badger/memory.h"
#include "badger/platform.h"
#include "badger/settings.h"
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"

#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* The timer the window refreshes by while Windows holds the thread, as the
 * backend names it. */
#define REFRESH_TIMER 0x7462u

/* How long a case waits for Windows to fire a timer. */
#define TIMER_NANOSECONDS UINT64_C(1000000000)

/* A key message's lParam: a scancode, extended or not, repeating or not. */
static LPARAM key_parameter(uint32_t scancode, bool repeat, bool alt)
{
    uint32_t flags = scancode & 0xFFu;

    if ((scancode & 0xE000u) == 0xE000u)
    {
        flags |= KF_EXTENDED;
    }
    if (repeat)
    {
        flags |= KF_REPEAT;
    }
    if (alt)
    {
        flags |= KF_ALTDOWN;
    }
    return (LPARAM)(((uintptr_t)flags << 16) | 1u);
}

typedef struct fixture
{
    tb_window_system *system;
    tb_window *window;
    HWND handle;
} fixture;

static fixture open_window(void)
{
    fixture made = {0};
    tb_window_native native = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (tb_window_system_open(NULL, &made.system, &error) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here: %s", error.message);
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(made.system,
                         &(tb_window_options){.title = "messages", .width = 320, .height = 180},
                         &made.window, &error),
        ==, TB_WINDOW_OK);
    tb_window_native_of(made.window, &native);
    made.handle = native.window;
    return made;
}

static void close_window(fixture *made)
{
    tb_window_destroy(made->window);
    tb_window_system_close(made->system);
}

/* Drain the queue, and say whether an event of a type came; the last such
 * one is kept. */
static bool received(tb_window_system *system, tb_window_event_type type, tb_window_event *kept)
{
    tb_window_event event = {0};
    bool found = false;

    while (tb_window_system_poll(system, &event))
    {
        if (event.type == type)
        {
            found = true;
            if (kept != NULL)
            {
                *kept = event;
            }
        }
    }
    return found;
}

TB_TEST(platform_win32_window, keys_are_named_by_their_scancodes)
{
    fixture made = open_window();
    tb_window_event event = {0};
    static const struct
    {
        UINT message;
        WPARAM virtual_key;
        uint32_t scancode;
        bool repeat;
        tb_window_event_type type;
        tb_key key;
    } rows[] = {
        {WM_KEYDOWN, VK_ESCAPE, 0x01, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_ESCAPE},
        {WM_KEYDOWN, 'W', 0x11, true, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_W},
        {WM_KEYUP, 'W', 0x11, false, TB_WINDOW_EVENT_KEY_UP, TB_KEY_W},
        {WM_KEYDOWN, VK_UP, 0xE048, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_UP},
        {WM_KEYDOWN, VK_HOME, 0xE047, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_HOME},
        {WM_KEYDOWN, VK_DELETE, 0xE053, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_DELETE},
        {WM_KEYDOWN, VK_RCONTROL, 0xE01D, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_RIGHT_CONTROL},
        {WM_SYSKEYDOWN, VK_F10, 0x44, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_F10},
        {WM_SYSKEYUP, VK_F10, 0x44, false, TB_WINDOW_EVENT_KEY_UP, TB_KEY_F10},
        {WM_KEYDOWN, 0, 0x7F, false, TB_WINDOW_EVENT_KEY_DOWN, TB_KEY_UNKNOWN},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_INT(
            SendMessageW(made.handle, rows[index].message, rows[index].virtual_key,
                         key_parameter(rows[index].scancode, rows[index].repeat, false)),
            ==, 0);
        TB_TEST_REQUIRE(received(made.system, rows[index].type, &event));
        TB_TEST_EXPECT_UINT(event.key, ==, rows[index].key);
        TB_TEST_EXPECT_UINT(event.scancode, ==, rows[index].scancode);
        TB_TEST_EXPECT(event.repeat == rows[index].repeat);
        TB_TEST_EXPECT_POINTER(event.window, ==, made.window);
    }
    close_window(&made);
}

/* Alt+F4 and the close button ask to close; the window stays, for the game
 * to decide. */
TB_TEST(platform_win32_window, closing_is_a_request_that_leaves_the_window_open)
{
    fixture made = open_window();

    (void)SendMessageW(made.handle, WM_CLOSE, 0, 0);
    TB_TEST_EXPECT(received(made.system, TB_WINDOW_EVENT_CLOSE_REQUESTED, NULL));
    TB_TEST_EXPECT(IsWindow(made.handle));
    /* Alt+F4 reaches a window as the close command. */
    (void)SendMessageW(made.handle, WM_SYSCOMMAND, SC_CLOSE, 0);
    TB_TEST_EXPECT(received(made.system, TB_WINDOW_EVENT_CLOSE_REQUESTED, NULL));
    TB_TEST_EXPECT(IsWindow(made.handle));
    /* Alt alone opens no menu. */
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_SYSCOMMAND, SC_KEYMENU, 0), ==, 0);
    close_window(&made);
}

/* Alt with a key types a character that would open a menu, and beep where
 * there is none: Alt+Enter, which switches to fullscreen, must not. The
 * window answers it itself, and the key still comes as a key. */
TB_TEST(platform_win32_window, alt_with_a_key_is_a_key_and_no_menu)
{
    fixture made = open_window();
    tb_window_event event = {0};

    (void)SendMessageW(made.handle, WM_SYSKEYDOWN, VK_RETURN, key_parameter(0x1C, false, true));
    TB_TEST_REQUIRE(received(made.system, TB_WINDOW_EVENT_KEY_DOWN, &event));
    TB_TEST_EXPECT_UINT(event.key, ==, TB_KEY_ENTER);
    TB_TEST_EXPECT_INT(
        SendMessageW(made.handle, WM_SYSCHAR, '\r', key_parameter(0x1C, false, true)), ==, 0);
    TB_TEST_EXPECT(!received(made.system, TB_WINDOW_EVENT_TEXT_INPUT, NULL));
    close_window(&made);
}

/* The engine's least window, in logical units, when its options give none. */
#define ENGINE_LEAST 64

/* The frame of a window's least client area, at the window's DPI: the
 * least size in pixels, rounded up. */
static RECT least_frame(HWND handle, LONG width, LONG height)
{
    const UINT dpi = GetDpiForWindow(handle);
    RECT frame = {0, 0, (LONG)(((UINT)width * dpi + 95u) / 96u),
                  (LONG)(((UINT)height * dpi + 95u) / 96u)};

    (void)AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongPtrW(handle, GWL_STYLE), FALSE,
                                   (DWORD)GetWindowLongPtrW(handle, GWL_EXSTYLE), dpi);
    return frame;
}

/* Windows asks how small the player may drag the window: the frame around
 * its least client area. A window that fills its display is not asked
 * about. */
TB_TEST(platform_win32_window, the_least_size_limits_dragging)
{
    fixture made = open_window();
    MINMAXINFO limits = {0};
    RECT frame = least_frame(made.handle, ENGINE_LEAST, ENGINE_LEAST);

    (void)SendMessageW(made.handle, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
    TB_TEST_EXPECT_INT(limits.ptMinTrackSize.x, ==, frame.right - frame.left);
    TB_TEST_EXPECT_INT(limits.ptMinTrackSize.y, ==, frame.bottom - frame.top);

    TB_TEST_REQUIRE_UINT(
        tb_window_configure(
            made.window,
            &(tb_window_options){.width = 640, .height = 360, .min_width = 400, .min_height = 300},
            NULL),
        ==, TB_WINDOW_OK);
    frame = least_frame(made.handle, 400, 300);
    limits = (MINMAXINFO){0};
    (void)SendMessageW(made.handle, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
    TB_TEST_EXPECT_INT(limits.ptMinTrackSize.x, ==, frame.right - frame.left);
    TB_TEST_EXPECT_INT(limits.ptMinTrackSize.y, ==, frame.bottom - frame.top);

    TB_TEST_REQUIRE_UINT(tb_window_configure(made.window,
                                             &(tb_window_options){.mode = TB_WINDOW_MODE_BORDERLESS,
                                                                  .min_width = 400,
                                                                  .min_height = 300},
                                             NULL),
                         ==, TB_WINDOW_OK);
    limits = (MINMAXINFO){0};
    (void)SendMessageW(made.handle, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
    TB_TEST_EXPECT_INT(limits.ptMinTrackSize.x, !=, frame.right - frame.left);
    close_window(&made);
}

/* Dragging the border of a window with an aspect, the frame Windows
 * proposes is changed to keep the aspect; without one it is left alone. */
TB_TEST(platform_win32_window, dragging_keeps_the_aspect)
{
    fixture made = open_window();
    RECT proposed = {0};
    RECT client = {0};
    RECT border = least_frame(made.handle, 0, 0);
    const LONG border_width = border.right - border.left;
    const LONG border_height = border.bottom - border.top;

    proposed = (RECT){100, 100, 100 + 1000 + border_width, 100 + 300 + border_height};
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_SIZING, WMSZ_RIGHT, (LPARAM)&proposed), ==,
                       FALSE);
    TB_TEST_EXPECT_INT(proposed.bottom, ==, 100 + 300 + border_height);

    TB_TEST_REQUIRE_UINT(tb_window_configure(made.window,
                                             &(tb_window_options){.width = 640,
                                                                  .height = 360,
                                                                  .aspect_width = 16,
                                                                  .aspect_height = 9},
                                             NULL),
                         ==, TB_WINDOW_OK);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_SIZING, WMSZ_RIGHT, (LPARAM)&proposed), ==,
                       TRUE);
    client = (RECT){proposed.left, proposed.top, proposed.right - border_width,
                    proposed.bottom - border_height};
    TB_TEST_EXPECT_INT(proposed.left, ==, 100);
    TB_TEST_EXPECT_INT(proposed.top, ==, 100);
    TB_TEST_EXPECT_INT(client.right - client.left, ==, 1000);
    TB_TEST_EXPECT_INT(client.bottom - client.top, ==, 563);

    /* Dragging the top-left corner keeps the bottom-right one: 1050 wide
     * and 583 high follows the width. */
    proposed = (RECT){50, 80, 100 + 1000 + border_width, 100 + 563 + border_height};
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_SIZING, WMSZ_TOPLEFT, (LPARAM)&proposed), ==,
                       TRUE);
    TB_TEST_EXPECT_INT(proposed.right, ==, 100 + 1000 + border_width);
    TB_TEST_EXPECT_INT(proposed.bottom, ==, 100 + 563 + border_height);
    TB_TEST_EXPECT_INT(proposed.right - proposed.left - border_width, ==, 1050);
    TB_TEST_EXPECT_INT(proposed.bottom - proposed.top - border_height, ==, 591);
    close_window(&made);
}

/* Moving to a monitor of another scale, Windows suggests a new frame: a
 * windowed window takes it. */
TB_TEST(platform_win32_window, a_windowed_window_takes_the_frame_a_new_scale_suggests)
{
    fixture made = open_window();
    tb_window_event event = {0};
    RECT frame = {0};
    RECT suggested = {0};
    const UINT dpi = GetDpiForWindow(made.handle);

    (void)received(made.system, TB_WINDOW_EVENT_RESIZED, NULL);
    (void)GetWindowRect(made.handle, &frame);
    suggested = (RECT){frame.left, frame.top, frame.right + 40, frame.bottom + 20};
    TB_TEST_EXPECT_INT(
        SendMessageW(made.handle, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&suggested), ==, 0);
    TB_TEST_REQUIRE(received(made.system, TB_WINDOW_EVENT_RESIZED, &event));
    TB_TEST_EXPECT_UINT(event.state.pixel_width, ==, (uint32_t)(320.0 * dpi / 96.0 + 0.5) + 40u);
    close_window(&made);
}

/* A fullscreen window gives the display back while the player is in another
 * program, and takes it again on return. A machine whose display refuses a
 * mode change gets a borderless window, which has nothing to give back. */
TB_TEST(platform_win32_window, fullscreen_gives_the_display_back_while_away)
{
    fixture made = open_window();
    tb_window_state state = {0};

    TB_TEST_REQUIRE_UINT(
        tb_window_configure(made.window, &(tb_window_options){.mode = TB_WINDOW_MODE_FULLSCREEN},
                            NULL),
        ==, TB_WINDOW_OK);
    tb_window_state_of(made.window, &state);
    if (state.mode != TB_WINDOW_MODE_FULLSCREEN)
    {
        close_window(&made);
        TB_TEST_SKIP("this display refuses a change of mode");
    }
    (void)SendMessageW(made.handle, WM_ACTIVATEAPP, FALSE, 0);
    TB_TEST_EXPECT(IsIconic(made.handle));
    (void)SendMessageW(made.handle, WM_ACTIVATEAPP, TRUE, 0);
    tb_window_state_of(made.window, &state);
    TB_TEST_EXPECT_UINT(state.mode, ==, TB_WINDOW_MODE_FULLSCREEN);
    /* A borderless window is not affected. */
    TB_TEST_REQUIRE_UINT(
        tb_window_configure(made.window, &(tb_window_options){.mode = TB_WINDOW_MODE_BORDERLESS},
                            NULL),
        ==, TB_WINDOW_OK);
    (void)SendMessageW(made.handle, WM_ACTIVATEAPP, FALSE, 0);
    TB_TEST_EXPECT(!IsIconic(made.handle));
    close_window(&made);
}

/* Drain the queue, and say whether an event of a type came at a position in
 * pixels; the real mouse may send events of its own meanwhile, which are
 * passed over. */
static bool received_at(tb_window_system *system, tb_window_event_type type, double pixel_x,
                        tb_window_event *kept)
{
    tb_window_event event = {0};
    bool found = false;

    while (tb_window_system_poll(system, &event))
    {
        if (event.type == type && event.pixel_x == pixel_x)
        {
            found = true;
            *kept = event;
        }
    }
    return found;
}

/* A mouse message's lParam: a position in the client area. */
static LPARAM position(int x, int y)
{
    return MAKELPARAM((WORD)(short)x, (WORD)(short)y);
}

/* Buttons are reported with where the mouse was, in pixels and in logical
 * units, and the modifiers the message carries. */
TB_TEST(platform_win32_window, mouse_buttons_are_events_with_a_position)
{
    fixture made = open_window();
    tb_window_event event = {0};
    tb_window_state state = {0};
    static const struct
    {
        UINT message;
        WPARAM word;
        LRESULT answer;
        tb_window_event_type type;
        tb_mouse_button button;
        uint32_t modifiers;
    } rows[] = {
        {WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, 0, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
         TB_MOUSE_BUTTON_LEFT, TB_MODIFIER_SHIFT},
        {WM_RBUTTONDOWN, MK_RBUTTON | MK_CONTROL, 0, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
         TB_MOUSE_BUTTON_RIGHT, TB_MODIFIER_CONTROL},
        {WM_RBUTTONUP, 0, 0, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, TB_MOUSE_BUTTON_RIGHT, 0},
        {WM_LBUTTONUP, 0, 0, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, TB_MOUSE_BUTTON_LEFT, 0},
        {WM_MBUTTONDOWN, MK_MBUTTON, 0, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, TB_MOUSE_BUTTON_MIDDLE,
         0},
        {WM_MBUTTONUP, 0, 0, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, TB_MOUSE_BUTTON_MIDDLE, 0},
        {WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1), TRUE, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
         TB_MOUSE_BUTTON_X1, 0},
        {WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1), TRUE, TB_WINDOW_EVENT_MOUSE_BUTTON_UP,
         TB_MOUSE_BUTTON_X1, 0},
        {WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2), TRUE, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
         TB_MOUSE_BUTTON_X2, 0},
        {WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2), TRUE, TB_WINDOW_EVENT_MOUSE_BUTTON_UP,
         TB_MOUSE_BUTTON_X2, 0},
    };

    tb_window_state_of(made.window, &state);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        TB_TEST_CONTEXT("row %zu", index);
        TB_TEST_EXPECT_INT(
            SendMessageW(made.handle, rows[index].message, rows[index].word, position(12, 30)), ==,
            rows[index].answer);
        TB_TEST_REQUIRE(received(made.system, rows[index].type, &event));
        TB_TEST_EXPECT_UINT(event.button, ==, rows[index].button);
        TB_TEST_EXPECT_STRING(tb_mouse_button_name(event.button), !=, "none");
        TB_TEST_EXPECT_UINT(event.modifiers & (TB_MODIFIER_SHIFT | TB_MODIFIER_CONTROL), ==,
                            rows[index].modifiers);
        TB_TEST_EXPECT_FLOAT(event.pixel_x, ==, 12.0);
        TB_TEST_EXPECT_FLOAT(event.pixel_y, ==, 30.0);
        TB_TEST_EXPECT_FLOAT_NEAR(event.x, 12.0 / state.content_scale, 1e-9);
        TB_TEST_EXPECT_POINTER(event.window, ==, made.window);
    }
    /* Another window taking the mouse ends a drag. */
    (void)SendMessageW(made.handle, WM_LBUTTONDOWN, MK_LBUTTON, position(1, 1));
    (void)SendMessageW(made.handle, WM_CAPTURECHANGED, 0, 0);
    (void)SendMessageW(made.handle, WM_LBUTTONUP, 0, position(1, 1));
    TB_TEST_EXPECT(received(made.system, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, NULL));
    close_window(&made);
}

/* The first motion over the window is its entering; leaving is reported
 * once. */
TB_TEST(platform_win32_window, the_mouse_moves_enters_and_leaves)
{
    fixture made = open_window();
    tb_window_event event = {0};
    size_t entered = 0;
    size_t left = 0;
    bool moved = false;

    /* Leaving is sent once the mouse is seen elsewhere: at once, unless the
     * real mouse is over the window, so the message is sent too. */
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_MOUSEMOVE, MK_SHIFT, position(33, 44)), ==, 0);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_MOUSELEAVE, 0, 0), ==, 0);
    (void)SendMessageW(made.handle, WM_MOUSELEAVE, 0, 0);
    while (tb_window_system_poll(made.system, &event))
    {
        if (event.type == TB_WINDOW_EVENT_MOUSE_MOVED && event.pixel_x == 33.0)
        {
            moved = true;
            TB_TEST_EXPECT_FLOAT(event.pixel_y, ==, 44.0);
            TB_TEST_EXPECT_UINT(event.modifiers & TB_MODIFIER_SHIFT, ==, TB_MODIFIER_SHIFT);
        }
        entered += event.type == TB_WINDOW_EVENT_MOUSE_ENTERED ? 1u : 0u;
        left += event.type == TB_WINDOW_EVENT_MOUSE_LEFT ? 1u : 0u;
    }
    TB_TEST_EXPECT(moved);
    TB_TEST_EXPECT_UINT(entered, >=, 1);
    TB_TEST_EXPECT_UINT(left, >=, 1);
    close_window(&made);
}

/* The wheel's turn is in notches, a fraction of one from a finer wheel, at
 * the place on the screen its message gives. */
TB_TEST(platform_win32_window, the_wheel_turns_in_notches)
{
    fixture made = open_window();
    tb_window_event event = {0};
    POINT point = {7, 9};

    (void)ClientToScreen(made.handle, &point);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_MOUSEWHEEL,
                                    MAKEWPARAM(MK_CONTROL, (WORD)(short)60),
                                    MAKELPARAM((WORD)(short)point.x, (WORD)(short)point.y)),
                       ==, 0);
    TB_TEST_REQUIRE(received_at(made.system, TB_WINDOW_EVENT_MOUSE_WHEEL, 7.0, &event));
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, 0.5);
    TB_TEST_EXPECT_FLOAT(event.delta_x, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(event.pixel_y, ==, 9.0);
    TB_TEST_EXPECT_UINT(event.modifiers & TB_MODIFIER_CONTROL, ==, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_MOUSEHWHEEL, MAKEWPARAM(0, (WORD)(short)-240),
                                    MAKELPARAM((WORD)(short)point.x, (WORD)(short)point.y)),
                       ==, 0);
    TB_TEST_REQUIRE(received_at(made.system, TB_WINDOW_EVENT_MOUSE_WHEEL, 7.0, &event));
    TB_TEST_EXPECT_FLOAT(event.delta_x, ==, -2.0);
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, 0.0);
    close_window(&made);
}

/* Drain the queue, joining the text of its text events. */
static void received_text(tb_window_system *system, char *text, size_t capacity)
{
    tb_window_event event = {0};
    size_t length = 0;

    text[0] = '\0';
    while (tb_window_system_poll(system, &event))
    {
        const size_t size = tb_str_length(event.text);
        if (event.type != TB_WINDOW_EVENT_TEXT_INPUT || length + size >= capacity)
        {
            continue;
        }
        memcpy(text + length, event.text, size + 1);
        length += size;
    }
}

/* Characters arrive as UTF-8, a character outside the Basic Multilingual
 * Plane in two halves that become one; control characters are keys, not
 * text. */
TB_TEST(platform_win32_window, characters_are_text_in_utf8)
{
    fixture made = open_window();
    char text[64] = {0};

    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_CHAR, 'a', 1), ==, 0);
    (void)SendMessageW(made.handle, WM_CHAR, 0x00E9, 1);
    (void)SendMessageW(made.handle, WM_CHAR, 0xD83D, 1);
    (void)SendMessageW(made.handle, WM_CHAR, 0xDE00, 1);
    /* Backspace and a second half without a first are not text. */
    (void)SendMessageW(made.handle, WM_CHAR, 0x08, 1);
    (void)SendMessageW(made.handle, WM_CHAR, 0xDE00, 1);
    received_text(made.system, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "a\xC3\xA9\xF0\x9F\x98\x80");
    close_window(&made);
}

/* The cursor's shape is set when Windows asks for it over the client area,
 * and none is set when it is hidden. */
TB_TEST(platform_win32_window, the_cursor_takes_its_shape_over_the_client_area)
{
    fixture made = open_window();
    const LPARAM over_client = MAKELPARAM(HTCLIENT, WM_MOUSEMOVE);

    for (int cursor = 0; cursor < TB_CURSOR_COUNT; cursor++)
    {
        tb_window_set_cursor(made.window, (tb_cursor)cursor);
    }
    tb_window_set_cursor(made.window, TB_CURSOR_TEXT);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_SETCURSOR, (WPARAM)made.handle, over_client),
                       ==, TRUE);
    TB_TEST_EXPECT_POINTER(GetCursor(), ==, LoadCursorW(NULL, MAKEINTRESOURCEW(32513)));
    tb_window_show_cursor(made.window, false);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_SETCURSOR, (WPARAM)made.handle, over_client),
                       ==, TRUE);
    TB_TEST_EXPECT_POINTER(GetCursor(), ==, NULL);
    tb_window_show_cursor(made.window, true);
    /* Over the frame, the frame's own cursor. */
    (void)SendMessageW(made.handle, WM_SETCURSOR, (WPARAM)made.handle,
                       MAKELPARAM(HTCAPTION, WM_MOUSEMOVE));
    /* A change while the mouse is over the window shows at once. */
    (void)SendMessageW(made.handle, WM_MOUSEMOVE, 0, position(3, 3));
    tb_window_set_cursor(made.window, TB_CURSOR_HAND);
    TB_TEST_EXPECT_POINTER(GetCursor(), ==, LoadCursorW(NULL, MAKEINTRESOURCEW(32649)));
    close_window(&made);
}

/* In relative mouse mode the cursor is hidden and positions are not
 * reported; turning it off brings them back. */
TB_TEST(platform_win32_window, relative_mouse_mode_hides_positions)
{
    fixture made = open_window();
    tb_window_state state = {0};
    tb_window_event event = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    TB_TEST_REQUIRE_UINT(tb_window_set_relative_mouse(made.window, true, &error), ==, TB_WINDOW_OK);
    TB_TEST_EXPECT_UINT(tb_window_set_relative_mouse(made.window, true, NULL), ==, TB_WINDOW_OK);
    tb_window_state_of(made.window, &state);
    TB_TEST_EXPECT(state.relative_mouse);
    (void)SendMessageW(made.handle, WM_MOUSEMOVE, 0, position(21, 22));
    TB_TEST_EXPECT(!received_at(made.system, TB_WINDOW_EVENT_MOUSE_MOVED, 21.0, &event));
    /* The cursor is held while the window has the keyboard, and let go
     * while it does not. */
    (void)SendMessageW(made.handle, WM_KILLFOCUS, 0, 0);
    (void)SendMessageW(made.handle, WM_SETFOCUS, 0, 0);
    (void)SendMessageW(made.handle, WM_MOVE, 0, 0);
    (void)SendMessageW(made.handle, WM_SIZE, SIZE_RESTORED, MAKELPARAM(320, 180));
    (void)SendMessageW(made.handle, WM_SETCURSOR, (WPARAM)made.handle,
                       MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    TB_TEST_EXPECT_POINTER(GetCursor(), ==, NULL);
    /* Raw Input that cannot be read is passed over. */
    (void)SendMessageW(made.handle, WM_INPUT, RIM_INPUT, 0);
    TB_TEST_EXPECT_UINT(tb_window_set_relative_mouse(made.window, false, NULL), ==, TB_WINDOW_OK);
    tb_window_state_of(made.window, &state);
    TB_TEST_EXPECT(!state.relative_mouse);
    (void)SendMessageW(made.handle, WM_MOUSEMOVE, 0, position(21, 22));
    TB_TEST_EXPECT(received_at(made.system, TB_WINDOW_EVENT_MOUSE_MOVED, 21.0, &event));
    /* A window destroyed in relative mouse mode lets the cursor go. */
    TB_TEST_REQUIRE_UINT(tb_window_set_relative_mouse(made.window, true, NULL), ==, TB_WINDOW_OK);
    close_window(&made);
}

/* A title is UTF-8, converted at the boundary. */
TB_TEST(platform_win32_window, titles_are_utf8)
{
    fixture made = open_window();
    wchar_t title[32] = {0};

    TB_TEST_REQUIRE_UINT(
        tb_window_configure(
            made.window, &(tb_window_options){.title = "caf\xC3\xA9", .width = 320, .height = 180},
            NULL),
        ==, TB_WINDOW_OK);
    (void)GetWindowTextW(made.handle, title, 32);
    TB_TEST_EXPECT(title[0] == L'c' && title[3] == 0x00E9 && title[4] == 0);
    /* Text that is not UTF-8 leaves no title. */
    TB_TEST_REQUIRE_UINT(
        tb_window_configure(made.window,
                            &(tb_window_options){.title = "\xC3\x28", .width = 320, .height = 180},
                            NULL),
        ==, TB_WINDOW_OK);
    (void)GetWindowTextW(made.handle, title, 32);
    TB_TEST_EXPECT(title[0] == 0);
    close_window(&made);
}

/* ---------------------------------------------------------------------------
 * Dragging: Windows holds the thread in a loop of its own.
 * ------------------------------------------------------------------------- */

static void count_refresh(void *context)
{
    size_t *const refreshes = context;
    (*refreshes)++;
}

/* Read the system's messages until a refresh comes, or a second passes. */
static bool refreshed_by_the_timer(fixture *made, const size_t *refreshes)
{
    const size_t before = *refreshes;
    const uint64_t give_up = tb_platform_clock_nanoseconds() + TIMER_NANOSECONDS;
    tb_window_event event = {0};

    while (*refreshes == before && tb_platform_clock_nanoseconds() < give_up)
    {
        tb_window_system_wait(made->system, 10000000u);
        while (tb_window_system_poll(made->system, &event))
        {
        }
    }
    return *refreshes > before;
}

/* From WM_ENTERSIZEMOVE to WM_EXITSIZEMOVE, and through the system menu's
 * loop, the refresh timer runs and each new size refreshes; outside them
 * neither does. */
TB_TEST(platform_win32_window, a_drag_refreshes_from_inside_windows_loop)
{
    fixture made = open_window();
    size_t refreshes = 0;
    RECT frame = {0};

    tb_window_system_set_refresh(made.system, count_refresh, &refreshes);
    (void)SendMessageW(made.handle, WM_TIMER, REFRESH_TIMER, 0);
    (void)GetWindowRect(made.handle, &frame);
    (void)SetWindowPos(made.handle, NULL, 0, 0, frame.right - frame.left + 10,
                       frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    TB_TEST_EXPECT_UINT(refreshes, ==, 0);

    (void)SendMessageW(made.handle, WM_ENTERSIZEMOVE, 0, 0);
    TB_TEST_EXPECT_INT(SendMessageW(made.handle, WM_TIMER, REFRESH_TIMER, 0), ==, 0);
    TB_TEST_EXPECT_UINT(refreshes, ==, 1);
    (void)SetWindowPos(made.handle, NULL, 0, 0, frame.right - frame.left + 30,
                       frame.bottom - frame.top + 20, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    TB_TEST_EXPECT_UINT(refreshes, ==, 2);
    /* Another program's timer is not the window's. */
    (void)SendMessageW(made.handle, WM_TIMER, REFRESH_TIMER + 1u, 0);
    TB_TEST_EXPECT_UINT(refreshes, ==, 2);
    TB_TEST_EXPECT(refreshed_by_the_timer(&made, &refreshes));
    (void)SendMessageW(made.handle, WM_EXITSIZEMOVE, 0, 0);
    refreshes = 0;
    (void)SendMessageW(made.handle, WM_TIMER, REFRESH_TIMER, 0);
    TB_TEST_EXPECT_UINT(refreshes, ==, 0);

    (void)SendMessageW(made.handle, WM_ENTERMENULOOP, TRUE, 0);
    TB_TEST_EXPECT(refreshed_by_the_timer(&made, &refreshes));
    (void)SendMessageW(made.handle, WM_EXITMENULOOP, TRUE, 0);
    close_window(&made);
}

/* What the game loop's frames saw. */
typedef struct frames_seen
{
    tb_window *window;
    size_t frames;
    uint32_t frame_width;   /* The window's width at the last frame. */
    uint32_t resized_width; /* The last resize given to the game before it. */
    uint32_t resized_before_frame;
} frames_seen;

static void seen_frame(void *context, const tb_game_time *time)
{
    frames_seen *const seen = context;
    tb_window_state state = {0};

    (void)time;
    tb_window_state_of(seen->window, &state);
    seen->frames++;
    seen->frame_width = state.pixel_width;
    seen->resized_before_frame = seen->resized_width;
}

static bool seen_event(void *context, const tb_window_event *event)
{
    frames_seen *const seen = context;

    if (event->type == TB_WINDOW_EVENT_RESIZED)
    {
        seen->resized_width = event->state.pixel_width;
    }
    return false;
}

/* While the player drags the window, the game loop runs frames from inside
 * Windows' loop: at each tick of its timer, and at each new size, with the
 * resize given to the game before the frame. */
TB_TEST(platform_win32_window, the_game_loop_runs_frames_during_a_drag)
{
    fixture made = open_window();
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings *const engine = tb_calloc(1, sizeof(tb_engine_settings));
    frames_seen seen = {.window = made.window};
    tb_game_loop *loop = NULL;
    RECT frame = {0};
    RECT client = {0};

    tb_engine_settings_add(settings, &(tb_engine_settings_options){.developer = true}, engine);
    loop = tb_game_loop_create(&(tb_game_loop_options){
        .system = made.system,
        .window = made.window,
        .settings = engine,
        .callbacks = {.frame = seen_frame, .event = seen_event, .context = &seen}});
    TB_TEST_REQUIRE(tb_game_loop_step(loop));
    TB_TEST_REQUIRE_UINT(seen.frames, ==, 1);

    (void)SendMessageW(made.handle, WM_ENTERSIZEMOVE, 0, 0);
    (void)SendMessageW(made.handle, WM_TIMER, REFRESH_TIMER, 0);
    TB_TEST_EXPECT_UINT(seen.frames, ==, 2);
    (void)GetWindowRect(made.handle, &frame);
    (void)SetWindowPos(made.handle, NULL, 0, 0, frame.right - frame.left + 48,
                       frame.bottom - frame.top + 24, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    (void)GetClientRect(made.handle, &client);
    TB_TEST_EXPECT_UINT(seen.frames, ==, 3);
    TB_TEST_EXPECT_UINT(seen.frame_width, ==, (uint32_t)client.right);
    TB_TEST_EXPECT_UINT(seen.resized_before_frame, ==, (uint32_t)client.right);
    (void)SendMessageW(made.handle, WM_EXITSIZEMOVE, 0, 0);
    (void)SendMessageW(made.handle, WM_TIMER, REFRESH_TIMER, 0);
    TB_TEST_EXPECT_UINT(seen.frames, ==, 3);

    tb_game_loop_destroy(loop);
    tb_settings_destroy(settings);
    tb_free(engine);
    close_window(&made);
}
