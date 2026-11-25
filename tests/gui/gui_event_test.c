/* The retained GUI's events, from input made by hand: the mouse entering
 * and leaving, presses held until released, clicks and double clicks,
 * clipping and hidden widgets, disabled widgets, buttons' commands, Tab's
 * focus, modal widgets, keys and the wheel given to ancestors, cursors,
 * and widgets destroyed from inside callbacks. */
#include "gui_fixture.h"
#include "tb_test.h"

/* Two boxes side by side at the top left: a at (0, 0), b at (100, 0), each
 * 100 by 50. */
typedef struct pair
{
    tb_widget a;
    tb_widget b;
} pair;

static pair make_pair(gui_fixture *fixture)
{
    const tb_widget root = tb_gui_root(fixture->gui);
    pair made = {gui_box(root, 100.0f, 50.0f), gui_box(root, 100.0f, 50.0f)};

    TB_TEST_REQUIRE(tb_grid(made.a, &(tb_grid_options){.column = 0}));
    TB_TEST_REQUIRE(tb_grid(made.b, &(tb_grid_options){.column = 1}));
    gui_update(fixture);
    return made;
}

static tb_gui_stats stats_of(const gui_fixture *fixture)
{
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture->gui, &stats);
    return stats;
}

TB_TEST(gui_event, the_mouse_enters_and_leaves_the_widget_under_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const pair boxes = make_pair(&fixture);
    gui_log log = {0};

    gui_record_all(boxes.a, &log);
    gui_record_all(boxes.b, &log);
    gui_move(&fixture, 50.0f, 25.0f);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_ENTER, boxes.a), ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, boxes.a));
    TB_TEST_EXPECT_UINT(tb_widget_state(boxes.a), ==, TB_WIDGET_HOVERED);
    TB_TEST_EXPECT(tb_gui_wants_mouse(fixture.gui));
    gui_move(&fixture, 60.0f, 25.0f);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_ENTER, boxes.a), ==, 1);

    gui_move(&fixture, 150.0f, 25.0f);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_LEAVE, boxes.a), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_ENTER, boxes.b), ==, 1);
    TB_TEST_EXPECT_UINT(log.kinds[log.count - 2], ==, TB_GUI_EVENT_LEAVE);
    TB_TEST_EXPECT_UINT(log.kinds[log.count - 1], ==, TB_GUI_EVENT_ENTER);

    /* Over nothing but the root, which leaves the mouse to the game. */
    gui_move(&fixture, 500.0f, 400.0f);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_LEAVE, boxes.b), ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, tb_gui_root(fixture.gui)));
    TB_TEST_EXPECT(!tb_gui_wants_mouse(fixture.gui));

    gui_move(&fixture, 150.0f, 25.0f);
    gui_leave(&fixture);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_LEAVE, boxes.b), ==, 2);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).hovered));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_press_holds_its_widget_until_released_and_a_click_needs_both_on_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const pair boxes = make_pair(&fixture);
    gui_log log = {0};

    gui_record_all(boxes.a, &log);
    gui_record_all(boxes.b, &log);
    gui_move(&fixture, 50.0f, 25.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, boxes.a), ==, 1);
    TB_TEST_EXPECT_UINT(tb_widget_state(boxes.a), ==, TB_WIDGET_HOVERED | TB_WIDGET_PRESSED);

    /* Dragged over b, a keeps the mouse: it leaves a, and b is not entered. */
    gui_move(&fixture, 150.0f, 25.0f);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_LEAVE, boxes.a), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_ENTER, boxes.b), ==, 0);
    TB_TEST_EXPECT_UINT(tb_widget_state(boxes.a), ==, TB_WIDGET_PRESSED);
    TB_TEST_EXPECT(tb_gui_wants_mouse(fixture.gui));
    /* Another button goes to the widget held, too. */
    gui_press(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_release(&fixture, TB_MOUSE_BUTTON_RIGHT);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, boxes.a), ==, 2);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, boxes.b), ==, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).pressed, boxes.a));

    /* Released over b: a is released, and not clicked; b is entered. */
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_RELEASE, boxes.a), ==, 2);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CLICK, boxes.a), ==, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_RELEASE, boxes.b), ==, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_ENTER, boxes.b), ==, 1);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).pressed));

    /* Pressed and released on it: a click. */
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CLICK, boxes.a), ==, 1);
    /* A release with nothing pressed goes nowhere. */
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_RELEASE, boxes.a), ==, 3);

    /* The window losing the focus lets go of what is held. */
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    const tb_window_event lost = {.type = TB_WINDOW_EVENT_FOCUS_LOST};
    tb_gui_feed_event(fixture.gui, &lost);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).pressed));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_second_press_soon_and_near_is_a_double_click)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const pair boxes = make_pair(&fixture);
    gui_log log = {0};

    gui_record_all(boxes.a, &log);
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    gui_click(&fixture, (tb_vec2){51.0f, 25.0f});
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_DOUBLE_CLICK, boxes.a), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CLICK, boxes.a), ==, 2);
    /* A third is a first again. */
    gui_click(&fixture, (tb_vec2){51.0f, 25.0f});
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_DOUBLE_CLICK, boxes.a), ==, 1);

    /* Too late. */
    tb_gui_update(fixture.gui, 1.0);
    gui_click(&fixture, (tb_vec2){51.0f, 25.0f});
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_DOUBLE_CLICK, boxes.a), ==, 1);
    /* Too far. */
    gui_click(&fixture, (tb_vec2){90.0f, 25.0f});
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_DOUBLE_CLICK, boxes.a), ==, 1);
    /* Another button. */
    gui_move(&fixture, 90.0f, 25.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_release(&fixture, TB_MOUSE_BUTTON_RIGHT);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_DOUBLE_CLICK, boxes.a), ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, the_mouse_finds_only_what_is_seen)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget frame = tb_frame(root, NULL);
    const tb_widget big = gui_box(frame, 200.0f, 200.0f);
    const tb_widget beside = gui_box(root, 700.0f, 200.0f);

    /* The frame gives back half its width, by its weight: the box in it is
     * cut off at its right edge, where the box beside begins. */
    TB_TEST_REQUIRE(tb_grid(frame, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    TB_TEST_REQUIRE(tb_grid(big, NULL));
    TB_TEST_REQUIRE(tb_grid(beside, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(frame).width, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(big).width, ==, 200.0f);

    gui_move(&fixture, 50.0f, 50.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, big));
    gui_move(&fixture, 150.0f, 50.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, beside));

    /* Hidden, a widget is not found; the frame under it is. */
    tb_widget_set_visible(big, false);
    gui_move(&fixture, 50.0f, 50.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, frame));

    /* A widget that ignores the mouse is passed through to what is under it;
     * one that passes it takes events and leaves the mouse to the game. */
    tb_widget_set_visible(big, true);
    tb_widget_set_mouse_filter(big, TB_GUI_MOUSE_IGNORE);
    gui_move(&fixture, 51.0f, 50.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, frame));
    TB_TEST_EXPECT(!tb_gui_wants_mouse(fixture.gui));
    tb_widget_set_mouse_filter(big, TB_GUI_MOUSE_PASS);
    gui_move(&fixture, 50.0f, 50.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, big));
    TB_TEST_EXPECT(!tb_gui_wants_mouse(fixture.gui));
    gui_fixture_close(&fixture);
}

static void count_call(const tb_gui_event *event, void *user)
{
    int *const count = user;

    (void)event;
    (*count)++;
}

TB_TEST(gui_event, a_disabled_widget_takes_nothing_and_blocks_the_mouse)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    int clicks = 0;
    const tb_widget button =
        tb_button(frame, &(tb_button_desc){.text = "Go", .command = count_call, .user = &clicks});
    gui_log log = {0};

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    gui_record_all(button, &log);
    gui_update(&fixture);
    const tb_vec2 middle = gui_middle(tb_widget_rect(button));

    tb_widget_set_enabled(button, false);
    gui_click(&fixture, middle);
    TB_TEST_EXPECT_INT(clicks, ==, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_ENTER, button), ==, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, button), ==, 0);
    TB_TEST_EXPECT_UINT(tb_widget_state(button), ==, TB_WIDGET_DISABLED);
    TB_TEST_EXPECT(tb_gui_wants_mouse(fixture.gui));
    TB_TEST_EXPECT(!tb_widget_invoke(button));
    TB_TEST_EXPECT(!tb_widget_focus(button));

    /* A disabled ancestor disables what is in it. */
    tb_widget_set_enabled(button, true);
    tb_widget_set_enabled(frame, false);
    gui_click(&fixture, middle);
    TB_TEST_EXPECT_INT(clicks, ==, 0);
    TB_TEST_EXPECT_UINT(tb_widget_state(button), ==, TB_WIDGET_DISABLED);

    tb_widget_set_enabled(frame, true);
    gui_click(&fixture, middle);
    TB_TEST_EXPECT_INT(clicks, ==, 1);

    /* Disabled while it holds the focus, it loses it. */
    TB_TEST_REQUIRE(tb_widget_focus(button));
    tb_widget_set_enabled(button, false);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_button_calls_its_command_when_clicked_or_pressed_with_keys)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    int commands = 0;
    int bound = 0;
    const tb_widget button =
        tb_button(tb_gui_root(fixture.gui),
                  &(tb_button_desc){.text = "Apply", .command = count_call, .user = &commands});

    TB_TEST_REQUIRE(tb_grid(button, NULL));
    TB_TEST_REQUIRE(tb_bind(button, TB_GUI_EVENT_COMMAND, count_call, &bound));
    gui_update(&fixture);
    gui_click(&fixture, gui_middle(tb_widget_rect(button)));
    TB_TEST_EXPECT_INT(commands, ==, 1);
    TB_TEST_EXPECT_INT(bound, ==, 1);
    /* Clicked with another button, it does nothing. */
    gui_press(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_release(&fixture, TB_MOUSE_BUTTON_RIGHT);
    TB_TEST_EXPECT_INT(commands, ==, 1);

    /* Keys reach it only with the focus. */
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT_INT(commands, ==, 1);
    TB_TEST_EXPECT(!tb_gui_wants_keyboard(fixture.gui));
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_gui_wants_keyboard(fixture.gui));
    TB_TEST_EXPECT_UINT(tb_widget_state(button) & TB_WIDGET_FOCUSED, ==, TB_WIDGET_FOCUSED);
    gui_key(&fixture, TB_KEY_SPACE, 0);
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_INT(commands, ==, 3);
    const tb_window_event repeat = {
        .type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_SPACE, .repeat = true};
    tb_gui_feed_event(fixture.gui, &repeat);
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(commands, ==, 3);

    TB_TEST_EXPECT(tb_widget_invoke(button));
    TB_TEST_EXPECT_INT(commands, ==, 4);
    TB_TEST_EXPECT_INT(bound, ==, 4);

    /* A command set to none is a button that does nothing. */
    const tb_gui_value none = tb_gui_command_of(NULL, NULL);
    TB_TEST_REQUIRE(tb_widget_set(button, "command", &none));
    TB_TEST_EXPECT(tb_widget_invoke(button));
    TB_TEST_EXPECT_INT(commands, ==, 4);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, tab_moves_the_focus_in_the_order_of_the_tree)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget first = tb_button(root, &(tb_button_desc){.text = "1"});
    const tb_widget disabled = tb_button(root, &(tb_button_desc){.text = "2"});
    const tb_widget hidden = tb_button(root, &(tb_button_desc){.text = "3"});
    const tb_widget frame = tb_frame(root, NULL);
    const tb_widget nested = tb_button(frame, &(tb_button_desc){.text = "4"});
    const tb_widget label = tb_label(root, &(tb_label_desc){.text = "Not focusable"});
    const tb_widget last = gui_box_of(root, gui_focus_box_class(), 10.0f, 10.0f);
    gui_log log = {0};

    TB_TEST_REQUIRE(tb_grid(first, &(tb_grid_options){.row = 0}));
    TB_TEST_REQUIRE(tb_grid(disabled, &(tb_grid_options){.row = 1}));
    TB_TEST_REQUIRE(tb_grid(hidden, &(tb_grid_options){.row = 2}));
    TB_TEST_REQUIRE(tb_grid(frame, &(tb_grid_options){.row = 3}));
    TB_TEST_REQUIRE(tb_grid(nested, NULL));
    TB_TEST_REQUIRE(tb_grid(label, &(tb_grid_options){.row = 4}));
    TB_TEST_REQUIRE(tb_grid(last, &(tb_grid_options){.row = 5}));
    tb_widget_set_enabled(disabled, false);
    tb_widget_set_visible(hidden, false);
    gui_record_all(first, &log);
    gui_record_all(nested, &log);
    gui_update(&fixture);

    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, first));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_FOCUS_IN, first), ==, 1);
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, nested));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_FOCUS_OUT, first), ==, 1);
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, last));
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, first));
    gui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, last));
    gui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, nested));
    /* Tab with Control is the game's. */
    gui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, nested));

    /* Nothing focused: Shift+Tab starts at the last. */
    tb_gui_clear_focus(fixture.gui);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    gui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, last));

    /* Focus given by hand, to what takes it. */
    TB_TEST_EXPECT(!tb_widget_focus(label));
    TB_TEST_EXPECT(!tb_widget_focus(hidden));
    TB_TEST_EXPECT(tb_widget_focus(nested));
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, nested));

    /* A press on what does not take the focus on a click takes it away, and
     * the game has the keyboard again. */
    gui_click(&fixture, (tb_vec2){700.0f, 500.0f});
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    TB_TEST_EXPECT(!tb_gui_wants_keyboard(fixture.gui));

    /* Hidden while focused, it loses the focus. */
    TB_TEST_EXPECT(tb_widget_focus(first));
    tb_widget_set_visible(first, false);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, tab_with_nothing_to_focus_does_nothing)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "Hi"});

    TB_TEST_REQUIRE(tb_grid(label, NULL));
    gui_update(&fixture);
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    tb_gui_focus_next(fixture.gui, true);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, keys_and_text_go_to_the_focus_and_then_to_its_ancestors)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget typing = gui_box_of(frame, gui_typing_box_class(), 100.0f, 30.0f);
    const tb_widget button = tb_button(frame, &(tb_button_desc){.text = "B"});
    gui_log frame_log = {0};
    gui_log typing_log = {0};

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(typing, NULL));
    TB_TEST_REQUIRE(tb_grid(button, &(tb_grid_options){.row = 1}));
    gui_record_all(frame, &frame_log);
    gui_record_all(typing, &typing_log);
    gui_update(&fixture);

    /* A kind that takes the focus on a click takes it. */
    gui_click(&fixture, gui_middle(tb_widget_rect(typing)));
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, typing));
    gui_type(&fixture, "h\xC3\xA9");
    TB_TEST_EXPECT_UINT(gui_log_count(&typing_log, TB_GUI_EVENT_TEXT, typing), ==, 1);
    gui_key(&fixture, TB_KEY_A, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&typing_log, TB_GUI_EVENT_KEY, typing), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&frame_log, TB_GUI_EVENT_KEY, frame), ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(frame_log.targets[frame_log.count - 1], typing));
    TB_TEST_EXPECT_UINT(frame_log.keys[frame_log.count - 1], ==, TB_KEY_A);

    /* A key the focus uses goes no further. */
    TB_TEST_REQUIRE(tb_widget_focus(button));
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&frame_log, TB_GUI_EVENT_KEY, frame), ==, 1);
    gui_key(&fixture, TB_KEY_B, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&frame_log, TB_GUI_EVENT_KEY, frame), ==, 2);
    /* Text goes to the focus alone. */
    gui_type(&fixture, "x");
    TB_TEST_EXPECT_UINT(gui_log_count(&frame_log, TB_GUI_EVENT_TEXT, frame), ==, 0);

    /* No focus: keys and text go to no widget. */
    tb_gui_clear_focus(fixture.gui);
    gui_key(&fixture, TB_KEY_C, 0);
    gui_type(&fixture, "y");
    TB_TEST_EXPECT_UINT(gui_log_count(&frame_log, TB_GUI_EVENT_KEY, frame), ==, 2);

    /* The wheel goes to the widget under the mouse and its ancestors. */
    gui_move(&fixture, gui_middle(tb_widget_rect(typing)).x, gui_middle(tb_widget_rect(typing)).y);
    gui_wheel(&fixture, 1.0f);
    TB_TEST_EXPECT_UINT(gui_log_count(&typing_log, TB_GUI_EVENT_WHEEL, typing), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&frame_log, TB_GUI_EVENT_WHEEL, frame), ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_modal_widget_alone_takes_input)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    int behind_clicks = 0;
    int ok_clicks = 0;
    const tb_widget behind = tb_button(
        root, &(tb_button_desc){.text = "Behind", .command = count_call, .user = &behind_clicks});
    const tb_widget dialog = tb_frame(root, &(tb_frame_desc){.padding = 8, .background = true});
    const tb_widget ok = tb_button(
        dialog, &(tb_button_desc){.text = "OK", .command = count_call, .user = &ok_clicks});
    const tb_widget cancel = tb_button(dialog, &(tb_button_desc){.text = "Cancel"});
    gui_log log = {0};
    size_t count = 0;
    const int32_t layer = TB_GUI_DEFAULT_LAYER;

    TB_TEST_REQUIRE(tb_grid(behind, NULL));
    TB_TEST_REQUIRE(tb_grid(dialog, &(tb_grid_options){.row = 1}));
    TB_TEST_REQUIRE(tb_grid(ok, NULL));
    TB_TEST_REQUIRE(tb_grid(cancel, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_bind(dialog, TB_GUI_EVENT_KEY, gui_record, &log));
    gui_update(&fixture);
    const tb_vec2 behind_middle = gui_middle(tb_widget_rect(behind));
    const tb_vec2 ok_middle = gui_middle(tb_widget_rect(ok));

    TB_TEST_REQUIRE(tb_widget_focus(behind));
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    TB_TEST_EXPECT(!tb_widget_show_modal(root));
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    gui_click(&fixture, behind_middle);
    TB_TEST_EXPECT_INT(behind_clicks, ==, 0);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).hovered));
    TB_TEST_EXPECT(tb_gui_wants_mouse(fixture.gui));
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).modal, dialog));
    TB_TEST_EXPECT(!tb_widget_focus(behind));
    gui_click(&fixture, ok_middle);
    TB_TEST_EXPECT_INT(ok_clicks, ==, 1);

    /* Tab goes around the modal widget's own. */
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, ok));
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, cancel));
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, ok));
    /* With no focus, keys go to the modal widget. */
    tb_gui_clear_focus(fixture.gui);
    gui_key(&fixture, TB_KEY_A, 0);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_KEY, dialog), ==, 1);

    /* It is drawn on a layer above the others, over a shade. */
    const tb_ui_command *const commands = tb_gui_commands(fixture.gui, &count);
    bool above = false;
    for (size_t index = 0; index < count; index++)
    {
        above = above || commands[index].layer > layer;
        TB_TEST_EXPECT_INT(commands[index].layer, >=, layer);
    }
    TB_TEST_EXPECT(above);

    tb_widget_end_modal(dialog);
    gui_click(&fixture, behind_middle);
    TB_TEST_EXPECT_INT(behind_clicks, ==, 1);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).modal));

    /* Hidden, or destroyed, it is modal no longer. */
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    tb_widget_set_visible(dialog, false);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).modal));
    tb_widget_set_visible(dialog, true);
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    tb_widget_destroy(dialog);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).modal));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, the_cursor_is_the_widgets_or_its_nearest_ancestors)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget inside = gui_box(frame, 50.0f, 50.0f);
    const tb_widget own = gui_box(frame, 50.0f, 50.0f);

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(inside, NULL));
    TB_TEST_REQUIRE(tb_grid(own, &(tb_grid_options){.column = 1}));
    tb_widget_set_cursor(frame, TB_CURSOR_HAND);
    tb_widget_set_cursor(own, TB_CURSOR_TEXT);
    gui_update(&fixture);
    gui_move(&fixture, 25.0f, 25.0f);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(fixture.gui), ==, TB_CURSOR_HAND);
    gui_move(&fixture, 75.0f, 25.0f);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(fixture.gui), ==, TB_CURSOR_TEXT);
    /* Held, a widget keeps its cursor wherever the mouse goes. */
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&fixture, 400.0f, 400.0f);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(fixture.gui), ==, TB_CURSOR_TEXT);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(fixture.gui), ==, TB_CURSOR_ARROW);
    tb_widget_set_cursor(own, TB_CURSOR_COUNT);
    gui_move(&fixture, 75.0f, 25.0f);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(fixture.gui), ==, TB_CURSOR_HAND);
    gui_leave(&fixture);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(fixture.gui), ==, TB_CURSOR_ARROW);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, bindings_are_added_and_removed)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const pair boxes = make_pair(&fixture);
    int first = 0;
    int second = 0;

    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_CLICK, count_call, &first));
    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_CLICK, count_call, &second));
    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_PRESS, count_call, &second));
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    TB_TEST_EXPECT_INT(first, ==, 1);
    TB_TEST_EXPECT_INT(second, ==, 2);

    TB_TEST_EXPECT_UINT(tb_unbind(boxes.a, TB_GUI_EVENT_CLICK, count_call, &first), ==, 1);
    TB_TEST_EXPECT_UINT(tb_unbind(boxes.a, TB_GUI_EVENT_CLICK, count_call, &first), ==, 0);
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    TB_TEST_EXPECT_INT(first, ==, 1);
    TB_TEST_EXPECT_INT(second, ==, 4);
    TB_TEST_EXPECT_UINT(tb_unbind(boxes.a, TB_GUI_EVENT_PRESS, NULL, NULL), ==, 1);
    TB_TEST_EXPECT_UINT(tb_unbind(boxes.a, TB_GUI_EVENT_CLICK, NULL, NULL), ==, 1);
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    TB_TEST_EXPECT_INT(second, ==, 4);
    TB_TEST_EXPECT_UINT(tb_unbind(TB_WIDGET_NONE, TB_GUI_EVENT_CLICK, NULL, NULL), ==, 0);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Callbacks that change the tree.
 * ------------------------------------------------------------------------- */

typedef struct destroyer
{
    tb_widget victim;
    int calls;
} destroyer;

static void destroy_victim(const tb_gui_event *event, void *user)
{
    destroyer *const doing = user;

    (void)event;
    doing->calls++;
    tb_widget_destroy(doing->victim);
}

static void destroy_self(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    (*calls)++;
    tb_widget_destroy(event->widget);
}

static void unbind_self(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    (*calls)++;
    (void)tb_unbind(event->widget, event->kind, unbind_self, user);
}

static void make_many(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    (*calls)++;
    for (int index = 0; index < 100; index++)
    {
        const tb_widget made = tb_label(tb_widget_parent(event->widget), NULL);
        TB_TEST_REQUIRE(tb_grid(made, &(tb_grid_options){.row = 10 + index}));
    }
}

TB_TEST(gui_event, a_callback_may_destroy_the_widget_it_was_called_for)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    int calls = 0;
    int after = 0;
    const tb_widget button =
        tb_button(tb_gui_root(fixture.gui),
                  &(tb_button_desc){.text = "Self", .command = destroy_self, .user = &calls});

    TB_TEST_REQUIRE(tb_grid(button, NULL));
    /* Bindings are called before the kind, which calls the command. */
    TB_TEST_REQUIRE(tb_bind(button, TB_GUI_EVENT_CLICK, count_call, &after));
    gui_update(&fixture);
    gui_click(&fixture, gui_middle(tb_widget_rect(button)));
    TB_TEST_EXPECT_INT(calls, ==, 1);
    TB_TEST_EXPECT(!tb_widget_alive(button));
    TB_TEST_EXPECT_INT(after, ==, 1);
    TB_TEST_EXPECT_UINT(stats_of(&fixture).widgets, ==, 1);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).hovered) ||
                   tb_widget_equal(stats_of(&fixture).hovered, tb_gui_root(fixture.gui)));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_callback_may_destroy_its_widgets_parent_and_the_events_stop)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget box = gui_box(frame, 50.0f, 50.0f);
    destroyer doing = {.victim = frame};
    int later = 0;

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(box, NULL));
    TB_TEST_REQUIRE(tb_bind(box, TB_GUI_EVENT_PRESS, destroy_victim, &doing));
    TB_TEST_REQUIRE(tb_bind(box, TB_GUI_EVENT_PRESS, count_call, &later));
    TB_TEST_REQUIRE(tb_bind(box, TB_GUI_EVENT_RELEASE, count_call, &later));
    gui_update(&fixture);
    gui_click(&fixture, (tb_vec2){25.0f, 25.0f});
    TB_TEST_EXPECT_INT(doing.calls, ==, 1);
    TB_TEST_EXPECT_INT(later, ==, 0);
    TB_TEST_EXPECT(!tb_widget_alive(frame));
    TB_TEST_EXPECT(!tb_widget_alive(box));
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).pressed));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_widget_destroyed_by_a_leave_is_never_entered)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const pair boxes = make_pair(&fixture);
    destroyer doing = {.victim = boxes.b};
    gui_log log = {0};

    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_LEAVE, destroy_victim, &doing));
    gui_record_all(boxes.b, &log);
    gui_move(&fixture, 50.0f, 25.0f);
    gui_move(&fixture, 150.0f, 25.0f);
    TB_TEST_EXPECT_INT(doing.calls, ==, 1);
    TB_TEST_EXPECT_UINT(log.count, ==, 0);
    TB_TEST_EXPECT(!tb_widget_alive(boxes.b));
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, tb_gui_root(fixture.gui)));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, a_widget_destroyed_as_the_focus_leaves_another_is_not_focused)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget first = tb_button(root, &(tb_button_desc){.text = "1"});
    const tb_widget second = tb_button(root, &(tb_button_desc){.text = "2"});
    destroyer doing = {.victim = second};

    TB_TEST_REQUIRE(tb_grid(first, NULL));
    TB_TEST_REQUIRE(tb_grid(second, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_bind(first, TB_GUI_EVENT_FOCUS_OUT, destroy_victim, &doing));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_widget_focus(first));
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT_INT(doing.calls, ==, 1);
    TB_TEST_EXPECT(!tb_widget_alive(second));
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).focus));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_event, callbacks_may_unbind_themselves_and_make_widgets)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const pair boxes = make_pair(&fixture);
    int unbinds = 0;
    int makes = 0;
    int after = 0;

    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_CLICK, unbind_self, &unbinds));
    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_CLICK, make_many, &makes));
    TB_TEST_REQUIRE(tb_bind(boxes.a, TB_GUI_EVENT_CLICK, count_call, &after));
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    TB_TEST_EXPECT_INT(unbinds, ==, 1);
    TB_TEST_EXPECT_INT(makes, ==, 1);
    TB_TEST_EXPECT_INT(after, ==, 1);
    gui_click(&fixture, (tb_vec2){50.0f, 25.0f});
    TB_TEST_EXPECT_INT(unbinds, ==, 1);
    TB_TEST_EXPECT_INT(makes, ==, 2);
    TB_TEST_EXPECT_INT(after, ==, 2);
    TB_TEST_EXPECT_UINT(stats_of(&fixture).widgets, ==, 203);
    gui_fixture_close(&fixture);
}

static void resize_again(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    (*calls)++;
    tb_widget_set_visible(event->widget, false);
}

TB_TEST(gui_event, a_resize_callback_may_change_the_layout)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = gui_box(tb_gui_root(fixture.gui), 30.0f, 30.0f);
    int calls = 0;

    TB_TEST_REQUIRE(tb_bind(box, TB_GUI_EVENT_RESIZE, resize_again, &calls));
    TB_TEST_REQUIRE(tb_grid(box, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(calls, ==, 1);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(box).width, ==, 0.0f);
    gui_fixture_close(&fixture);
}
