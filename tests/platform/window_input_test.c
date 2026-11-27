/* What every backend's input becomes, through the helpers the backends
 * share: text without its control characters, in events of whole
 * characters; positions in both logical units and pixels, whichever the
 * system reads; relative motion only in relative mouse mode; and entering
 * and leaving reported once each. The helpers are called directly, as a
 * backend calls them, so no event of the desktop's own is involved. */
#include "platform/shared/window.h"

#include "badger/input.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

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
    return made;
}

static void close_window(fixture *made)
{
    tb_window_destroy(made->window);
    tb_window_system_close(made->system);
}

/* Forget what the system queued, so that the case reads only its own. */
static void drain(fixture *made)
{
    made->system->next_event = 0;
    tb_array_clear(made->system->events);
}

/* The next queued event of the window's, without reading the system. */
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

TB_TEST(platform_window_input, text_comes_in_whole_characters_without_controls)
{
    fixture made = open_window();
    tb_window_event event = {0};
    /* 40 bytes: 20 of "e" with an acute accent, which a 31-byte event cannot
     * hold, and which it must not cut in two. */
    char long_text[41] = {0};
    char joined[64] = {0};
    size_t joined_length = 0;
    size_t events = 0;

    for (size_t index = 0; index < 20; index++)
    {
        long_text[index * 2] = '\xC3';
        long_text[index * 2 + 1] = '\xA9';
    }
    drain(&made);
    tb_window_text(made.window, long_text, 40);
    while (next(&made, &event))
    {
        TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_TEXT_INPUT);
        const size_t size = tb_str_length(event.text);
        TB_TEST_EXPECT_UINT(size % 2, ==, 0);
        TB_TEST_REQUIRE_UINT(joined_length + size, <, sizeof(joined));
        memcpy(joined + joined_length, event.text, size + 1);
        joined_length += size;
        events++;
    }
    TB_TEST_EXPECT_UINT(events, ==, 2);
    TB_TEST_EXPECT_STRING(joined, ==, long_text);

    /* Control characters and bytes that are not UTF-8 are dropped; text of
     * nothing else is no event. */
    tb_window_text(made.window,
                   "a\tb\x7F\xC2\x85\xFF"
                   "c\r\n",
                   10);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_STRING(event.text, ==, "abc");
    tb_window_text(made.window, "\x1B\b", 2);
    TB_TEST_EXPECT(!next(&made, &event));
    close_window(&made);
}

TB_TEST(platform_window_input, positions_are_in_logical_units_and_pixels)
{
    fixture made = open_window();
    tb_window_event event = {0};
    const bool in_pixels = made.system->pointer_in_pixels;

    drain(&made);
    made.window->state.content_scale = 2.0;
    made.system->pointer_in_pixels = true;
    tb_window_mouse_button(made.window, TB_MOUSE_BUTTON_LEFT, true, 10.0, 20.0, TB_MODIFIER_ALT);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN);
    TB_TEST_EXPECT_FLOAT(event.x, ==, 5.0);
    TB_TEST_EXPECT_FLOAT(event.y, ==, 10.0);
    TB_TEST_EXPECT_FLOAT(event.pixel_x, ==, 10.0);
    TB_TEST_EXPECT_UINT(event.modifiers, ==, TB_MODIFIER_ALT);

    made.system->pointer_in_pixels = false;
    tb_window_mouse_wheel(made.window, 0.25, -1.0, 10.0, 20.0, 0);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_WHEEL);
    TB_TEST_EXPECT_FLOAT(event.x, ==, 10.0);
    TB_TEST_EXPECT_FLOAT(event.pixel_y, ==, 40.0);
    TB_TEST_EXPECT_FLOAT(event.delta_x, ==, 0.25);
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, -1.0);
    made.system->pointer_in_pixels = in_pixels;
    close_window(&made);
}

TB_TEST(platform_window_input, relative_motion_replaces_positions_in_relative_mode)
{
    fixture made = open_window();
    tb_window_event event = {0};

    drain(&made);
    tb_window_mouse_relative(made.window, 1.0, 2.0);
    TB_TEST_EXPECT(!next(&made, &event));
    made.window->state.relative_mouse = true;
    tb_window_mouse_moved(made.window, 1.0, 2.0, 0);
    TB_TEST_EXPECT(!next(&made, &event));
    tb_window_mouse_relative(made.window, 3.0, -4.0);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION);
    TB_TEST_EXPECT_FLOAT(event.delta_x, ==, 3.0);
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, -4.0);
    made.window->state.relative_mouse = false;
    tb_window_mouse_moved(made.window, 1.0, 2.0, 0);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_MOVED);
    close_window(&made);
}

TB_TEST(platform_window_input, entering_and_leaving_are_reported_once)
{
    fixture made = open_window();
    tb_window_event event = {0};

    drain(&made);
    made.window->mouse_inside = false;
    tb_window_mouse_crossed(made.window, true, 4.0, 5.0);
    tb_window_mouse_crossed(made.window, true, 4.0, 5.0);
    tb_window_mouse_crossed(made.window, false, 0.0, 0.0);
    tb_window_mouse_crossed(made.window, false, 0.0, 0.0);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_ENTERED);
    TB_TEST_REQUIRE(next(&made, &event));
    TB_TEST_EXPECT_UINT(event.type, ==, TB_WINDOW_EVENT_MOUSE_LEFT);
    TB_TEST_EXPECT(!next(&made, &event));
    close_window(&made);
}

static bool layout_label(tb_window_system *system, tb_key key, char *label, size_t capacity)
{
    const char *const letter = system->native;
    if (key != TB_KEY_A || capacity < 2)
    {
        return false;
    }
    label[0] = *letter;
    label[1] = '\0';
    return true;
}

TB_TEST(platform_window_input, layout_changes_only_the_displayed_keyboard_label)
{
    const tb_window_ops ops = {.key_label = layout_label};
    char letter = 'q';
    tb_window_system system = {.ops = &ops, .native = &letter};
    tb_input_window_prompt first = {0};
    tb_input_window_prompt second = {0};
    tb_input_key_prompt_for_window(&system, TB_INPUT_KEY_A, TB_GAMEPAD_FAMILY_GENERIC, &first);
    letter = 'a';
    tb_input_key_prompt_for_window(&system, TB_INPUT_KEY_A, TB_GAMEPAD_FAMILY_GENERIC, &second);
    TB_TEST_EXPECT_STRING(first.label, ==, "Q");
    TB_TEST_EXPECT_STRING(second.label, ==, "A");
    TB_TEST_EXPECT_STRING(first.glyph, ==, second.glyph);
    TB_TEST_EXPECT_STRING(first.glyph, ==, "keyboard/a");
    char small[1] = {'?'};
    TB_TEST_EXPECT(!tb_window_system_key_label(&system, TB_KEY_A, small, sizeof(small)));
    TB_TEST_EXPECT_STRING(small, ==, "");
    tb_input_key_prompt_for_window(&system, TB_INPUT_KEY_B, TB_GAMEPAD_FAMILY_GENERIC, &second);
    TB_TEST_EXPECT_STRING(second.label, ==, "B");
}

TB_TEST(platform_window_input, the_native_layout_names_keys_or_uses_the_portable_prompt)
{
    fixture made = open_window();
    for (tb_key key = TB_KEY_A; key < TB_KEY_COUNT; key++)
    {
        tb_input_window_prompt prompt = {0};
        tb_input_prompt portable = {0};
        const tb_input_key physical = tb_input_key_from_window(key);
        tb_input_key_prompt(physical, TB_GAMEPAD_FAMILY_GENERIC, &portable);
        tb_input_key_prompt_for_window(made.system, physical, TB_GAMEPAD_FAMILY_GENERIC, &prompt);
        TB_TEST_EXPECT(prompt.label[0] != '\0');
        TB_TEST_EXPECT_STRING(prompt.glyph, ==, portable.glyph);
        if (made.system->backend == TB_WINDOW_BACKEND_COCOA && key == TB_KEY_RIGHT_ALT)
        {
            TB_TEST_EXPECT_STRING(prompt.label, ==, "Right Option");
        }
    }
    close_window(&made);
}
