/* Windows and displays, on every windowing the machine has: displays are
 * described sensibly, a window opens at a sensible size on the display it
 * asked for, changes mode while it is open, wakes its system from another
 * thread, and refuses the windowing of another platform. Each case runs once
 * for each windowing that opens, so a Linux machine with both a Wayland
 * compositor and an X server tests both. A machine without windowing, a
 * server or CI without a display, skips the cases. */
#include "badger/platform.h"
#include "badger/str.h"
#include "badger/thread.h"
#include "badger/window.h"

#include "tb_test.h"

/* How long the cases wait for the system to answer: a compositor or an X
 * server answers asynchronously. */
#define SETTLE_NANOSECONDS (UINT64_C(300) * 1000000u)
#define WAKE_NANOSECONDS (UINT64_C(5000) * 1000000u)

/* Run a case's body on each windowing that opens; skip the case when none
 * does. */
static void for_each_backend(void (*body)(tb_window_system *system))
{
    static const tb_window_backend backends[] = {TB_WINDOW_BACKEND_WIN32, TB_WINDOW_BACKEND_COCOA,
                                                 TB_WINDOW_BACKEND_WAYLAND, TB_WINDOW_BACKEND_X11};
    size_t opened = 0;

    for (size_t index = 0; index < TB_TEST_COUNT_OF(backends); index++)
    {
        tb_window_system *system = NULL;
        const tb_window_system_options options = {.backend = backends[index]};
        if (tb_window_system_open(&options, &system, NULL) != TB_WINDOW_OK)
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", tb_window_backend_name(backends[index]));
        TB_TEST_EXPECT_UINT(tb_window_system_backend(system), ==, backends[index]);
        body(system);
        tb_window_system_close(system);
        opened++;
    }
    if (opened == 0)
    {
        TB_TEST_SKIP("no windowing here");
    }
}

/* Read events for a while, so that a window's state catches up with an
 * asynchronous system; returns how many of a type came. */
static size_t settle(tb_window_system *system, tb_window_event_type counted)
{
    const uint64_t deadline = tb_platform_clock_nanoseconds() + SETTLE_NANOSECONDS;
    size_t count = 0;

    while (tb_platform_clock_nanoseconds() < deadline)
    {
        tb_window_event event = {0};
        while (tb_window_system_poll(system, &event))
        {
            count += event.type == counted ? 1u : 0u;
        }
        tb_window_system_wait(system, UINT64_C(10) * 1000000u);
    }
    return count;
}

static void displays_are_described_with_the_primary_first_on(tb_window_system *system)
{
    const size_t count = tb_window_system_display_count(system);

    TB_TEST_EXPECT_UINT(count, >=, 1);
    for (size_t index = 0; index < count; index++)
    {
        tb_display display = {0};
        tb_window_system_display(system, index, &display);
        TB_TEST_CONTEXT("display %zu, '%s'", index, display.name);
        TB_TEST_EXPECT(display.primary == (index == 0));
        TB_TEST_EXPECT_UINT(display.width, >, 0);
        TB_TEST_EXPECT_UINT(display.height, >, 0);
        TB_TEST_EXPECT_UINT(display.pixel_width, >, 0);
        TB_TEST_EXPECT_UINT(display.usable_width, <=, display.width);
        TB_TEST_EXPECT_UINT(display.usable_height, <=, display.height);
        TB_TEST_EXPECT_FLOAT(display.content_scale, >=, 0.5);
        TB_TEST_EXPECT_FLOAT(display.content_scale, <=, 8.0);
        TB_TEST_EXPECT_UINT(tb_str_length(display.name), <, TB_DISPLAY_NAME_CAPACITY);
    }
}

TB_TEST(platform_window, displays_are_described_with_the_primary_first)
{
    for_each_backend(displays_are_described_with_the_primary_first_on);
    tb_window_system_close(NULL);
}

/* A machine has its own platform's windowing and no other's. */
TB_TEST(platform_window, another_platforms_windowing_is_unavailable)
{
    static const tb_window_backend backends[] = {TB_WINDOW_BACKEND_WIN32, TB_WINDOW_BACKEND_COCOA,
                                                 TB_WINDOW_BACKEND_WAYLAND, TB_WINDOW_BACKEND_X11};
    tb_window_system *system = NULL;
    tb_window_backend own = TB_WINDOW_BACKEND_AUTOMATIC;
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (tb_window_system_open(NULL, &system, &error) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here: %s", error.message);
    }
    own = tb_window_system_backend(system);
    tb_window_system_close(system);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(backends); index++)
    {
        const bool linux_pair =
            (own == TB_WINDOW_BACKEND_WAYLAND || own == TB_WINDOW_BACKEND_X11) &&
            (backends[index] == TB_WINDOW_BACKEND_WAYLAND ||
             backends[index] == TB_WINDOW_BACKEND_X11);
        tb_window_system *other = NULL;
        if (backends[index] == own || linux_pair)
        {
            continue;
        }
        TB_TEST_CONTEXT("%s", tb_window_backend_name(backends[index]));
        error = (tb_window_error)TB_WINDOW_ERROR_INIT;
        TB_TEST_EXPECT_UINT(
            tb_window_system_open(&(tb_window_system_options){.backend = backends[index]}, &other,
                                  &error),
            ==, TB_WINDOW_UNAVAILABLE);
        TB_TEST_EXPECT_POINTER(other, ==, NULL);
        TB_TEST_EXPECT_UINT(error.result, ==, TB_WINDOW_UNAVAILABLE);
        TB_TEST_EXPECT(error.message[0] != '\0');
    }
}

/* A window given no size is the default size, or smaller to fit, and is
 * centered on its display where the system lets a program place windows. */
static void a_new_window_has_the_default_size_and_a_title_on(tb_window_system *system)
{
    tb_window *window = NULL;
    tb_window_state state = {0};
    tb_window_native native = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    TB_TEST_REQUIRE_UINT(
        tb_window_create(system, &(tb_window_options){.title = "Tenacious Badger \xE2\x9C\x93"},
                         &window, &error),
        ==, TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.mode, ==, TB_WINDOW_MODE_WINDOWED);
    TB_TEST_EXPECT_UINT(state.display, ==, 0);
    TB_TEST_EXPECT_UINT(state.width, <=, 1280);
    TB_TEST_EXPECT_UINT(state.height, <=, 720);
    TB_TEST_EXPECT_UINT(state.width, >=, 64);
    /* 16:9, give or take the rounding of a scale. */
    TB_TEST_EXPECT_UINT(state.width * 9u / 16u + 2u, >=, state.height);
    TB_TEST_EXPECT_UINT(state.width * 9u / 16u, <=, state.height + 2u);
    TB_TEST_EXPECT_UINT(state.pixel_width, ==,
                        (uint32_t)((double)state.width * state.content_scale + 0.5));
    tb_window_native_of(window, &native);
    TB_TEST_EXPECT_UINT(native.backend, ==, tb_window_system_backend(system));
    TB_TEST_EXPECT(native.window != NULL || native.x11_window != 0);
    /* Wayland and X11 have no dialog of their own; the others' would wait
     * for a person, so they are not asked here. */
    if (native.backend == TB_WINDOW_BACKEND_WAYLAND || native.backend == TB_WINDOW_BACKEND_X11)
    {
        bool confirmed = true;
        TB_TEST_EXPECT_UINT(tb_window_confirm(window, "Quit?", "Really?", &confirmed), ==,
                            TB_WINDOW_UNAVAILABLE);
        TB_TEST_EXPECT(confirmed);
    }
    tb_window_destroy(window);
    tb_window_destroy(NULL);
}

TB_TEST(platform_window, a_new_window_has_the_default_size_and_a_title)
{
    for_each_backend(a_new_window_has_the_default_size_and_a_title_on);
}

static void a_window_changes_mode_while_it_is_open_on(tb_window_system *system)
{
    tb_window *window = NULL;
    tb_window_state state = {0};
    tb_display display = {0};
    tb_window_options options = {.title = "modes", .width = 640, .height = 360};

    tb_window_system_display(system, 0, &display);
    TB_TEST_REQUIRE_UINT(tb_window_create(system, &options, &window, NULL), ==, TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.width, ==, 640);
    TB_TEST_EXPECT_UINT(state.height, ==, 360);

    /* Borderless covers the display. */
    options.mode = TB_WINDOW_MODE_BORDERLESS;
    TB_TEST_REQUIRE_UINT(tb_window_configure(window, &options, NULL), ==, TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.mode, ==, TB_WINDOW_MODE_BORDERLESS);
    TB_TEST_EXPECT_UINT(state.pixel_width, ==, display.pixel_width);
    TB_TEST_EXPECT_UINT(state.pixel_height, ==, display.pixel_height);

    /* Fullscreen is fullscreen where the platform has it, else borderless. */
    options.mode = TB_WINDOW_MODE_FULLSCREEN;
    TB_TEST_REQUIRE_UINT(tb_window_configure(window, &options, NULL), ==, TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT(state.mode == TB_WINDOW_MODE_FULLSCREEN ||
                   state.mode == TB_WINDOW_MODE_BORDERLESS);

    /* And back to a window of a new size, with a new title. */
    options = (tb_window_options){.title = "back", .width = 800, .height = 450, .fixed_size = true};
    TB_TEST_REQUIRE_UINT(tb_window_configure(window, &options, NULL), ==, TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.mode, ==, TB_WINDOW_MODE_WINDOWED);
    TB_TEST_EXPECT_UINT(state.width, ==, 800);
    TB_TEST_EXPECT_UINT(state.height, ==, 450);
    tb_window_destroy(window);
}

TB_TEST(platform_window, a_window_changes_mode_while_it_is_open)
{
    for_each_backend(a_window_changes_mode_while_it_is_open_on);
}

static void a_display_that_does_not_exist_is_the_primary_on(tb_window_system *system)
{
    tb_window *window = NULL;
    tb_window_state state = {0};

    TB_TEST_REQUIRE_UINT(
        tb_window_create(system, &(tb_window_options){.display = 1000}, &window, NULL), ==,
        TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.display, ==, 0);
    tb_window_destroy(window);
}

TB_TEST(platform_window, a_display_that_does_not_exist_is_the_primary)
{
    for_each_backend(a_display_that_does_not_exist_is_the_primary_on);
}

/* A window much larger than its display is clamped to the display's usable
 * area. */
static void a_window_larger_than_its_display_is_clamped_on(tb_window_system *system)
{
    tb_window *window = NULL;
    tb_window_state state = {0};
    tb_display display = {0};

    tb_window_system_display(system, 0, &display);
    TB_TEST_REQUIRE_UINT(tb_window_create(system,
                                          &(tb_window_options){.width = 100000, .height = 100000},
                                          &window, NULL),
                         ==, TB_WINDOW_OK);
    (void)settle(system, TB_WINDOW_EVENT_RESIZED);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT_UINT(state.pixel_width, <=, display.pixel_width);
    TB_TEST_EXPECT_UINT(state.pixel_height, <=, display.pixel_height);
    tb_window_destroy(window);
}

TB_TEST(platform_window, a_window_larger_than_its_display_is_clamped)
{
    for_each_backend(a_window_larger_than_its_display_is_clamped_on);
}

static void wake_soon(void *argument)
{
    tb_platform_sleep(UINT64_C(20) * 1000000u);
    tb_window_system_wake(argument);
}

static void another_thread_wakes_a_waiting_system_on(tb_window_system *system)
{
    tb_thread thread = {0};
    const uint64_t start = tb_platform_clock_nanoseconds();
    bool woken = false;

    tb_thread_create(&thread, wake_soon, system);
    while (!woken && tb_platform_clock_nanoseconds() - start < WAKE_NANOSECONDS)
    {
        tb_window_event event = {0};
        tb_window_system_wait(system, WAKE_NANOSECONDS);
        while (tb_window_system_poll(system, &event))
        {
            woken = woken || event.type == TB_WINDOW_EVENT_WAKE;
        }
    }
    tb_thread_join(&thread);
    TB_TEST_EXPECT(woken);
    /* It came well before the wait's limit. */
    TB_TEST_EXPECT_UINT(tb_platform_clock_nanoseconds() - start, <, WAKE_NANOSECONDS);
}

TB_TEST(platform_window, another_thread_wakes_a_waiting_system)
{
    for_each_backend(another_thread_wakes_a_waiting_system_on);
}

/* Every windowing has the cursor's shapes and hides it; relative mouse mode
 * is on where the system can do it, and unavailable where it cannot, as
 * under a Wayland compositor without a mouse. */
static void the_mouse_is_set_up_on(tb_window_system *system)
{
    tb_window *window = NULL;
    tb_window_state state = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;
    tb_window_result result = TB_WINDOW_OK;

    TB_TEST_REQUIRE_UINT(tb_window_create(system, NULL, &window, NULL), ==, TB_WINDOW_OK);
    for (int cursor = 0; cursor < TB_CURSOR_COUNT; cursor++)
    {
        tb_window_set_cursor(window, (tb_cursor)cursor);
    }
    tb_window_show_cursor(window, false);
    tb_window_show_cursor(window, true);
    result = tb_window_set_relative_mouse(window, true, &error);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT(result == TB_WINDOW_OK || result == TB_WINDOW_UNAVAILABLE);
    TB_TEST_EXPECT(state.relative_mouse == (result == TB_WINDOW_OK));
    TB_TEST_EXPECT(result == TB_WINDOW_OK || error.message[0] != '\0');
    TB_TEST_EXPECT_UINT(tb_window_set_relative_mouse(window, false, NULL), ==, TB_WINDOW_OK);
    tb_window_state_of(window, &state);
    TB_TEST_EXPECT(!state.relative_mouse);
    tb_window_destroy(window);
}

TB_TEST(platform_window, the_mouse_is_set_up)
{
    for_each_backend(the_mouse_is_set_up_on);
}

TB_TEST(platform_window, names_say_what_they_name)
{
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_LEFT), ==, "left");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_RIGHT), ==, "right");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_MIDDLE), ==, "middle");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_X1), ==, "x1");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_X2), ==, "x2");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_NONE), ==, "none");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name(TB_MOUSE_BUTTON_COUNT), ==, "none");
    TB_TEST_EXPECT_STRING(tb_mouse_button_name((tb_mouse_button)99), ==, "none");
    TB_TEST_EXPECT_STRING(tb_window_backend_name(TB_WINDOW_BACKEND_WAYLAND), ==, "wayland");
    TB_TEST_EXPECT_STRING(tb_window_backend_name(TB_WINDOW_BACKEND_X11), ==, "x11");
    TB_TEST_EXPECT_STRING(tb_window_backend_name(TB_WINDOW_BACKEND_WIN32), ==, "win32");
    TB_TEST_EXPECT_STRING(tb_window_backend_name(TB_WINDOW_BACKEND_COCOA), ==, "cocoa");
    TB_TEST_EXPECT_STRING(tb_window_backend_name(TB_WINDOW_BACKEND_AUTOMATIC), ==, "automatic");
    TB_TEST_EXPECT_STRING(tb_window_backend_name((tb_window_backend)99), ==, "unknown backend");
    TB_TEST_EXPECT_STRING(tb_window_result_name(TB_WINDOW_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_window_result_name(TB_WINDOW_UNAVAILABLE), ==, "unavailable");
    TB_TEST_EXPECT_STRING(tb_window_result_name(TB_WINDOW_SYSTEM_ERROR), ==, "system error");
    TB_TEST_EXPECT_STRING(tb_window_result_name((tb_window_result)99), ==, "unknown result");
    TB_TEST_EXPECT_STRING(tb_key_name(TB_KEY_ESCAPE), ==, "escape");
    TB_TEST_EXPECT_STRING(tb_key_name(TB_KEY_LEFT_SHIFT), ==, "left_shift");
    TB_TEST_EXPECT_STRING(tb_key_name(TB_KEY_F12), ==, "f12");
    TB_TEST_EXPECT_STRING(tb_key_name(TB_KEY_COUNT), ==, "unknown");
    for (int key = 0; key < TB_KEY_COUNT; key++)
    {
        TB_TEST_CONTEXT("key %d", key);
        TB_TEST_EXPECT(tb_key_name((tb_key)key) != NULL);
    }
}
