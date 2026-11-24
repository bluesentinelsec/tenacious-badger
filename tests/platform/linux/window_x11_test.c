/* The X11 windowing turns what the X server sends a window into the
 * engine's events: buttons and the wheel's notches, motion, entering and
 * leaving, and keys with the text they type. The events are sent to the
 * window through the server, as XSendEvent() sends them, so that each is
 * checked without a person at the mouse; relative motion, which the server
 * makes only from a device, is made by XTest where the server has it. A
 * machine without an X server skips the cases. */
#include "badger/library.h"
#include "badger/platform.h"
#include "badger/window.h"

#include "tb_test.h"

/* How long a case waits for the server to send an event back. */
#define ANSWER_NANOSECONDS (UINT64_C(2000) * 1000000u)

/* As much of Xlib as is sent: XKeyEvent, XButtonEvent and XMotionEvent share
 * a layout but for the field after `state`; XCrossingEvent differs after
 * `y_root`. */
typedef struct x_display x_display;

typedef struct x_input_event
{
    int type;
    unsigned long serial;
    int send_event;
    x_display *display;
    unsigned long window;
    unsigned long root;
    unsigned long subwindow;
    unsigned long time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int detail; /* keycode or button; is_hint for motion */
    int same_screen;
} x_input_event;

typedef struct x_crossing_event
{
    int type;
    unsigned long serial;
    int send_event;
    x_display *display;
    unsigned long window;
    unsigned long root;
    unsigned long subwindow;
    unsigned long time;
    int x;
    int y;
    int x_root;
    int y_root;
    int mode;
    int detail;
    int same_screen;
    int focus;
    unsigned int state;
} x_crossing_event;

typedef struct x_focus_event
{
    int type;
    unsigned long serial;
    int send_event;
    x_display *display;
    unsigned long window;
    int mode;
    int detail;
} x_focus_event;

typedef union x_event {
    int type;
    x_input_event input;
    x_crossing_event crossing;
    x_focus_event focus;
    long pad[24];
} x_event;

enum
{
    KEY_PRESS = 2,
    KEY_RELEASE = 3,
    BUTTON_PRESS = 4,
    BUTTON_RELEASE = 5,
    MOTION_NOTIFY = 6,
    ENTER_NOTIFY = 7,
    LEAVE_NOTIFY = 8,
    FOCUS_IN = 9,
    FOCUS_OUT = 10
};

#define SHIFT_MASK (1u << 0)
#define CONTROL_MASK (1u << 2)
#define MOD1_MASK (1u << 3)
#define MOD4_MASK (1u << 6)
/* The keycode of A, evdev's 30 and 8 more, in every keymap X uses. */
#define KEYCODE_A 38u

typedef struct fixture
{
    tb_window_system *system;
    tb_window *window;
    x_display *display;
    unsigned long handle;
    tb_library *xlib;
    int (*send_event)(x_display *display, unsigned long window, int propagate, long mask,
                      x_event *event);
    int (*flush)(x_display *display);
} fixture;

static fixture open_window(void)
{
    fixture made = {0};
    tb_window_native native = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;
    tb_library_function send_event = NULL;
    tb_library_function flush = NULL;

    if (tb_window_system_open(&(tb_window_system_options){.backend = TB_WINDOW_BACKEND_X11},
                              &made.system, &error) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no X server here: %s", error.message);
    }
    TB_TEST_REQUIRE_UINT(tb_window_create(made.system,
                                          &(tb_window_options){.width = 320, .height = 180},
                                          &made.window, &error),
                         ==, TB_WINDOW_OK);
    tb_window_native_of(made.window, &native);
    made.display = native.system;
    made.handle = (unsigned long)native.x11_window;
    TB_TEST_REQUIRE_UINT(tb_library_open("libX11.so.6", TB_LIBRARY_SEARCH_SYSTEM, &made.xlib, NULL),
                         ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_find_function(made.xlib, "XSendEvent", &send_event, NULL), ==,
                         TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_find_function(made.xlib, "XFlush", &flush, NULL), ==,
                         TB_LIBRARY_OK);
    made.send_event = (int (*)(x_display *, unsigned long, int, long, x_event *))send_event;
    made.flush = (int (*)(x_display *))flush;
    return made;
}

static void close_window(fixture *made)
{
    tb_window_destroy(made->window);
    tb_window_system_close(made->system);
    tb_library_close(made->xlib);
}

static void send(fixture *made, x_event *event)
{
    event->input.display = made->display;
    event->input.window = made->handle;
    event->input.same_screen = 1;
    TB_TEST_EXPECT_INT(made->send_event(made->display, made->handle, 0, 0, event), !=, 0);
    (void)made->flush(made->display);
}

static void send_input(fixture *made, int type, unsigned int detail, int x, int y,
                       unsigned int state)
{
    x_event event = {0};

    event.input = (x_input_event){.type = type, .x = x, .y = y, .state = state, .detail = detail};
    send(made, &event);
}

/* Wait for an event of a type at a position in pixels, reading the
 * window's events until it comes. */
static bool wait_for(fixture *made, tb_window_event_type type, double pixel_x,
                     tb_window_event *found)
{
    const uint64_t deadline = tb_platform_clock_nanoseconds() + ANSWER_NANOSECONDS;

    while (tb_platform_clock_nanoseconds() < deadline)
    {
        tb_window_event event = {0};
        while (tb_window_system_poll(made->system, &event))
        {
            if (event.type == type && event.pixel_x == pixel_x)
            {
                *found = event;
                return true;
            }
        }
        tb_window_system_wait(made->system, UINT64_C(10) * 1000000u);
    }
    return false;
}

TB_TEST(platform_x11_window, buttons_and_the_wheel_are_events)
{
    fixture made = open_window();
    tb_window_event event = {0};
    static const struct
    {
        int type;
        unsigned int button;
        tb_window_event_type expected;
        tb_mouse_button which;
        double notches_x;
        double notches_y;
    } rows[] = {
        {BUTTON_PRESS, 1, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, TB_MOUSE_BUTTON_LEFT, 0.0, 0.0},
        {BUTTON_RELEASE, 1, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, TB_MOUSE_BUTTON_LEFT, 0.0, 0.0},
        {BUTTON_PRESS, 2, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, TB_MOUSE_BUTTON_MIDDLE, 0.0, 0.0},
        {BUTTON_PRESS, 3, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, TB_MOUSE_BUTTON_RIGHT, 0.0, 0.0},
        {BUTTON_PRESS, 8, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, TB_MOUSE_BUTTON_X1, 0.0, 0.0},
        {BUTTON_PRESS, 9, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, TB_MOUSE_BUTTON_X2, 0.0, 0.0},
        {BUTTON_PRESS, 4, TB_WINDOW_EVENT_MOUSE_WHEEL, TB_MOUSE_BUTTON_NONE, 0.0, 1.0},
        {BUTTON_PRESS, 5, TB_WINDOW_EVENT_MOUSE_WHEEL, TB_MOUSE_BUTTON_NONE, 0.0, -1.0},
        {BUTTON_PRESS, 6, TB_WINDOW_EVENT_MOUSE_WHEEL, TB_MOUSE_BUTTON_NONE, -1.0, 0.0},
        {BUTTON_PRESS, 7, TB_WINDOW_EVENT_MOUSE_WHEEL, TB_MOUSE_BUTTON_NONE, 1.0, 0.0},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(rows); index++)
    {
        const int x = 10 + (int)index;
        TB_TEST_CONTEXT("row %zu", index);
        /* The wheel's release and a button of no name are passed over. */
        send_input(&made, BUTTON_RELEASE, 4, x, 20, 0);
        send_input(&made, BUTTON_PRESS, 12, x, 20, 0);
        send_input(&made, rows[index].type, rows[index].button, x, 20,
                   SHIFT_MASK | CONTROL_MASK | MOD1_MASK | MOD4_MASK);
        TB_TEST_REQUIRE(wait_for(&made, rows[index].expected, (double)x, &event));
        TB_TEST_EXPECT_UINT(event.button, ==, rows[index].which);
        TB_TEST_EXPECT_FLOAT(event.delta_x, ==, rows[index].notches_x);
        TB_TEST_EXPECT_FLOAT(event.delta_y, ==, rows[index].notches_y);
        TB_TEST_EXPECT_FLOAT(event.pixel_y, ==, 20.0);
        TB_TEST_EXPECT_UINT(event.modifiers, ==,
                            TB_MODIFIER_SHIFT | TB_MODIFIER_CONTROL | TB_MODIFIER_ALT |
                                TB_MODIFIER_SUPER);
    }
    close_window(&made);
}

TB_TEST(platform_x11_window, the_mouse_moves_enters_and_leaves)
{
    fixture made = open_window();
    tb_window_event event = {0};
    x_event crossing = {0};

    /* The window may be under the pointer already: it leaves first. */
    crossing.crossing = (x_crossing_event){.type = LEAVE_NOTIFY};
    send(&made, &crossing);
    crossing.crossing = (x_crossing_event){.type = ENTER_NOTIFY, .x = 5, .y = 6};
    send(&made, &crossing);
    TB_TEST_EXPECT(wait_for(&made, TB_WINDOW_EVENT_MOUSE_ENTERED, 5.0, &event));
    send_input(&made, MOTION_NOTIFY, 0, 30, 40, SHIFT_MASK);
    TB_TEST_REQUIRE(wait_for(&made, TB_WINDOW_EVENT_MOUSE_MOVED, 30.0, &event));
    TB_TEST_EXPECT_FLOAT(event.pixel_y, ==, 40.0);
    TB_TEST_EXPECT_UINT(event.modifiers, ==, TB_MODIFIER_SHIFT);
    crossing.crossing = (x_crossing_event){.type = LEAVE_NOTIFY};
    send(&made, &crossing);
    TB_TEST_EXPECT(wait_for(&made, TB_WINDOW_EVENT_MOUSE_LEFT, 0.0, &event));
    close_window(&made);
}

/* A key press types the text its keymap gives, through the input method. */
TB_TEST(platform_x11_window, keys_type_text)
{
    fixture made = open_window();
    tb_window_event event = {0};
    const uint64_t deadline = tb_platform_clock_nanoseconds() + ANSWER_NANOSECONDS;
    bool typed = false;
    bool pressed = false;

    send_input(&made, KEY_PRESS, KEYCODE_A, 0, 0, SHIFT_MASK);
    send_input(&made, KEY_RELEASE, KEYCODE_A, 0, 0, SHIFT_MASK);
    while (!typed && tb_platform_clock_nanoseconds() < deadline)
    {
        while (tb_window_system_poll(made.system, &event))
        {
            if (event.type == TB_WINDOW_EVENT_KEY_DOWN && event.key == TB_KEY_A)
            {
                pressed = true;
                TB_TEST_EXPECT_UINT(event.modifiers, ==, TB_MODIFIER_SHIFT);
            }
            if (event.type == TB_WINDOW_EVENT_TEXT_INPUT)
            {
                typed = true;
                TB_TEST_EXPECT_STRING(event.text, ==, "A");
            }
        }
        tb_window_system_wait(made.system, UINT64_C(10) * 1000000u);
    }
    TB_TEST_EXPECT(pressed);
    TB_TEST_EXPECT(typed);
    close_window(&made);
}

/* Cursor shapes are the cursor font's, and a hidden cursor is a blank one. */
TB_TEST(platform_x11_window, the_cursor_takes_each_shape)
{
    fixture made = open_window();

    for (int cursor = 0; cursor < TB_CURSOR_COUNT; cursor++)
    {
        tb_window_set_cursor(made.window, (tb_cursor)cursor);
    }
    tb_window_show_cursor(made.window, false);
    tb_window_show_cursor(made.window, true);
    tb_window_set_cursor(made.window, TB_CURSOR_ARROW);
    close_window(&made);
}

/* Relative mouse mode holds the mouse while the window has the keyboard,
 * and lets it go while it does not. */
TB_TEST(platform_x11_window, relative_mouse_mode_follows_the_keyboard)
{
    fixture made = open_window();
    tb_window_event event = {0};
    x_event focus = {0};

    TB_TEST_REQUIRE_UINT(tb_window_set_relative_mouse(made.window, true, NULL), ==, TB_WINDOW_OK);
    /* Without a window manager, nothing gives the window the keyboard
     * first. */
    focus.focus = (x_focus_event){.type = FOCUS_IN};
    send(&made, &focus);
    TB_TEST_EXPECT(wait_for(&made, TB_WINDOW_EVENT_FOCUS_GAINED, 0.0, &event));
    focus.focus = (x_focus_event){.type = FOCUS_OUT};
    send(&made, &focus);
    TB_TEST_EXPECT(wait_for(&made, TB_WINDOW_EVENT_FOCUS_LOST, 0.0, &event));
    focus.focus = (x_focus_event){.type = FOCUS_IN};
    send(&made, &focus);
    TB_TEST_EXPECT(wait_for(&made, TB_WINDOW_EVENT_FOCUS_GAINED, 0.0, &event));
    TB_TEST_EXPECT_UINT(tb_window_set_relative_mouse(made.window, false, NULL), ==, TB_WINDOW_OK);
    close_window(&made);
}

/* Relative mouse mode reports XInput2's raw motion, which XTest makes where
 * the server has it. */
TB_TEST(platform_x11_window, relative_mouse_mode_reports_raw_motion)
{
    fixture made = open_window();
    tb_window_event event = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;
    tb_library *xtest = NULL;
    tb_library_function fake_motion = NULL;

    TB_TEST_REQUIRE_UINT(tb_window_set_relative_mouse(made.window, true, &error), ==, TB_WINDOW_OK);
    if (tb_library_open("libXtst.so.6", TB_LIBRARY_SEARCH_SYSTEM, &xtest, NULL) == TB_LIBRARY_OK &&
        tb_library_find_function(xtest, "XTestFakeRelativeMotionEvent", &fake_motion, NULL) ==
            TB_LIBRARY_OK)
    {
        bool moved = false;
        const uint64_t deadline = tb_platform_clock_nanoseconds() + ANSWER_NANOSECONDS;
        (void)((int (*)(x_display *, int, int, unsigned long))fake_motion)(made.display, 7, 3, 0);
        (void)made.flush(made.display);
        while (!moved && tb_platform_clock_nanoseconds() < deadline)
        {
            while (tb_window_system_poll(made.system, &event))
            {
                moved = moved || event.type == TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION;
            }
            tb_window_system_wait(made.system, UINT64_C(10) * 1000000u);
        }
        TB_TEST_EXPECT(moved);
    }
    TB_TEST_EXPECT_UINT(tb_window_set_relative_mouse(made.window, false, NULL), ==, TB_WINDOW_OK);
    close_window(&made);
    /* XTest hooks the closing of the display it was used on, so it stays
     * loaded until the display is closed. */
    tb_library_close(xtest);
}
