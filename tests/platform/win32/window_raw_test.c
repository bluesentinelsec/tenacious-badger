/* Raw Input reports how far the mouse moved, or, from a remote desktop or a
 * virtual machine, where it is. Windows sends it only to the window in
 * front, which a test running beside others cannot count on, so the mouse's
 * reports are given to the window's handling directly. */
#include "platform/win32/window_input.h"

#include "tb_test.h"

typedef struct fixture
{
    tb_window_system *system;
    tb_window *window;
} fixture;

static fixture open_window(void)
{
    fixture made = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (tb_window_system_open(NULL, &made.system, &error) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here: %s", error.message);
    }
    TB_TEST_REQUIRE_UINT(tb_window_create(made.system,
                                          &(tb_window_options){.width = 320, .height = 180},
                                          &made.window, &error),
                         ==, TB_WINDOW_OK);
    TB_TEST_REQUIRE_UINT(tb_window_set_relative_mouse(made.window, true, &error), ==, TB_WINDOW_OK);
    made.system->next_event = 0;
    tb_array_clear(made.system->events);
    return made;
}

static void close_window(fixture *made)
{
    tb_window_destroy(made->window);
    tb_window_system_close(made->system);
}

/* The next queued event, without reading the system's messages. */
static bool next(fixture *made, tb_window_event *event)
{
    if (made->system->next_event == tb_array_length(made->system->events))
    {
        return false;
    }
    *event = *TB_ARRAY_AT(made->system->events, tb_window_event, made->system->next_event);
    made->system->next_event++;
    return true;
}

TB_TEST(platform_win32_raw_mouse, motion_is_reported_as_it_comes)
{
    fixture made = open_window();
    tb_window_event event = {0};
    const RAWMOUSE moved = {.usFlags = MOUSE_MOVE_RELATIVE, .lLastX = 5, .lLastY = -3};
    const RAWMOUSE still = {.usFlags = MOUSE_MOVE_RELATIVE};

    tb_win32_raw_mouse(made.window, &moved);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION);
    TB_TEST_EXPECT_FLOAT(event.delta_x, ==, 5.0);
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, -3.0);
    /* A report of buttons alone moves nothing. */
    tb_win32_raw_mouse(made.window, &still);
    TB_TEST_EXPECT(!next(&made, &event));
    close_window(&made);
}

TB_TEST(platform_win32_raw_mouse, a_mouse_that_reports_positions_moves_by_their_change)
{
    fixture made = open_window();
    tb_window_event event = {0};
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const RAWMOUSE first = {.usFlags = MOUSE_MOVE_ABSOLUTE, .lLastX = 1000, .lLastY = 2000};
    const RAWMOUSE second = {
        .usFlags = MOUSE_MOVE_ABSOLUTE, .lLastX = 1000 + 65535 / 4, .lLastY = 2000};
    const RAWMOUSE whole = {
        .usFlags = MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP, .lLastX = 1000, .lLastY = 2000};

    /* The first position is where the mouse is; it has not moved yet. */
    tb_win32_raw_mouse(made.window, &first);
    TB_TEST_EXPECT(!next(&made, &event));
    tb_win32_raw_mouse(made.window, &second);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_FLOAT_NEAR(event.delta_x, (double)width / 4.0, 1.0);
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, 0.0);
    /* Over the whole of several monitors. */
    tb_win32_raw_mouse(made.window, &whole);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_FLOAT(event.delta_x, <, 0.0);
    close_window(&made);
}
