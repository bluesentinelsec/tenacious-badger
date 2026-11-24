/* The Win32 windowing turns the messages Windows sends a window into the
 * engine's events: keys by their scancodes, a close request that leaves the
 * window open, focus, minimizing, displays, a new scale, the mouse, and the
 * characters keys type. Messages are
 * sent to the window directly, as Windows would send them, so that each is
 * checked without a person at the keyboard. */
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"

#include <string.h>

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
