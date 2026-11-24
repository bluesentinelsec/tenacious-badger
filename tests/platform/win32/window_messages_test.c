/* The Win32 windowing turns the messages Windows sends a window into the
 * engine's events: keys by their scancodes, a close request that leaves the
 * window open, focus, minimizing, displays, and a new scale. Messages are
 * sent to the window directly, as Windows would send them, so that each is
 * checked without a person at the keyboard. */
#include "badger/window.h"

#include "tb_test.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
