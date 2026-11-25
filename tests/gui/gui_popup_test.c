/* The popup layer: widgets shown in front of every other, at their natural
 * size, placed by an anchor and kept inside the GUI, unclipped by their
 * parents; closed by a press outside them, by Escape, by hand, and when
 * hidden or destroyed; keys given to the one in front; and tooltips shown
 * after the mouse rests. */
#include "gui_fixture.h"
#include "tb_test.h"

#include "badger/ui.h"

static tb_gui_stats stats_of(const gui_fixture *fixture)
{
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture->gui, &stats);
    return stats;
}

static void expect_rect(tb_canvas_rect rect, float x, float y, float width, float height)
{
    TB_TEST_EXPECT_FLOAT_NEAR(rect.x, x, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.y, y, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.width, width, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.height, height, 1e-3);
}

/* The first command a widget drew: its layer and clip. */
static bool command_of(const gui_fixture *fixture, tb_widget widget, tb_ui_command *found)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == (tb_ui_id)widget.index + 1u)
        {
            *found = commands[index];
            return true;
        }
    }
    return false;
}

/* A small frame at the top left, and a box of 100 by 80 in it, not gridded. */
typedef struct owner
{
    tb_widget frame;
    tb_widget popup;
} owner;

static owner make_owner(gui_fixture *fixture)
{
    owner made = {0};

    made.frame = gui_box(tb_gui_root(fixture->gui), 50.0f, 50.0f);
    TB_TEST_REQUIRE(tb_grid(made.frame, NULL));
    made.popup = gui_box(made.frame, 100.0f, 80.0f);
    gui_update(fixture);
    return made;
}

TB_TEST(gui_popup, a_popup_is_shown_in_front_at_its_natural_size_unclipped_by_its_parent)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const owner made = make_owner(&fixture);
    tb_ui_command command = {0};

    TB_TEST_EXPECT(!tb_widget_is_popup(made.popup));
    TB_TEST_REQUIRE(tb_widget_popup(
        made.popup, &(tb_gui_popup_options){.anchor = {10.0f, 10.0f, 40.0f, 20.0f}}));
    TB_TEST_EXPECT(tb_widget_is_popup(made.popup));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 10.0f, 30.0f, 100.0f, 80.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).popup, made.popup));
    /* Drawn whole, past its parent's 50 by 50, above the GUI's layer. */
    TB_TEST_REQUIRE(command_of(&fixture, made.popup, &command));
    expect_rect(command.clip, 10.0f, 30.0f, 100.0f, 80.0f);
    TB_TEST_EXPECT_INT(command.layer, >, TB_GUI_DEFAULT_LAYER);
    /* Found by the mouse where its parent is not. */
    gui_move(&fixture, 90.0f, 90.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, made.popup));
    TB_TEST_EXPECT(tb_gui_wants_mouse(fixture.gui));

    /* Popped up again, it moves; the root never is one. */
    TB_TEST_REQUIRE(
        tb_widget_popup(made.popup, &(tb_gui_popup_options){.anchor = {.x = 200.0f, .y = 100.0f},
                                                            .placement = TB_GUI_POPUP_AT,
                                                            .min_size = {150.0f, 0.0f}}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 200.0f, 100.0f, 150.0f, 80.0f);
    TB_TEST_EXPECT(!tb_widget_popup(tb_gui_root(fixture.gui), NULL));
    TB_TEST_EXPECT(!tb_widget_popup(TB_WIDGET_NONE, NULL));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_popup, a_popup_is_kept_inside_the_gui_flipped_beside_its_anchor)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const owner made = make_owner(&fixture);

    /* No room below: above the anchor. */
    TB_TEST_REQUIRE(tb_widget_popup(
        made.popup, &(tb_gui_popup_options){.anchor = {10.0f, 560.0f, 40.0f, 20.0f}}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 10.0f, 480.0f, 100.0f, 80.0f);
    /* No room to the right of a cascade: to its left. */
    TB_TEST_REQUIRE(tb_widget_popup(
        made.popup, &(tb_gui_popup_options){.anchor = {650.0f, 10.0f, 100.0f, 20.0f},
                                            .placement = TB_GUI_POPUP_RIGHT}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 550.0f, 10.0f, 100.0f, 80.0f);
    /* At a point near the corner: up and left of it. */
    TB_TEST_REQUIRE(
        tb_widget_popup(made.popup, &(tb_gui_popup_options){.anchor = {.x = 790.0f, .y = 590.0f},
                                                            .placement = TB_GUI_POPUP_AT}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 690.0f, 510.0f, 100.0f, 80.0f);
    /* Below an anchor at the right edge: pushed in. */
    TB_TEST_REQUIRE(tb_widget_popup(
        made.popup, &(tb_gui_popup_options){.anchor = {760.0f, 10.0f, 40.0f, 20.0f}}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 700.0f, 30.0f, 100.0f, 80.0f);
    /* Larger than the GUI: as large as it. */
    TB_TEST_REQUIRE(
        tb_widget_popup(made.popup, &(tb_gui_popup_options){.min_size = {1000.0f, 1000.0f}}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(made.popup), 0.0f, 0.0f, 800.0f, 600.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_popup, a_press_outside_closes_the_popups_and_is_used_by_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const owner made = make_owner(&fixture);
    const tb_widget front = gui_box(made.popup, 60.0f, 60.0f);
    gui_log log = {0};

    gui_record_all(made.popup, &log);
    gui_record_all(made.frame, &log);
    gui_record_all(front, &log);
    TB_TEST_REQUIRE(
        tb_widget_popup(made.popup, &(tb_gui_popup_options){.anchor = {200.0f, 0.0f, 0.0f, 0.0f}}));
    TB_TEST_REQUIRE(
        tb_widget_popup(front, &(tb_gui_popup_options){.anchor = {400.0f, 0.0f, 0.0f, 0.0f}}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(front), 400.0f, 0.0f, 60.0f, 60.0f);

    /* A press in the popup behind closes the one in front of it only, and
     * goes on to the popup. */
    gui_click(&fixture, (tb_vec2){210.0f, 10.0f});
    TB_TEST_EXPECT(!tb_widget_is_popup(front));
    TB_TEST_EXPECT(tb_widget_is_popup(made.popup));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_POPUP_CLOSE, front), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, made.popup), ==, 1);

    /* A press outside every popup closes them, and the widget under it is
     * not pressed. */
    gui_click(&fixture, (tb_vec2){10.0f, 10.0f});
    TB_TEST_EXPECT(!tb_widget_is_popup(made.popup));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_POPUP_CLOSE, made.popup), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, made.frame), ==, 0);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).popup));
    /* With none shown, presses go through again. */
    gui_click(&fixture, (tb_vec2){10.0f, 10.0f});
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_PRESS, made.frame), ==, 1);
    gui_fixture_close(&fixture);
}

static void count_event(const tb_gui_event *event, void *user)
{
    int *const count = user;

    (void)event;
    (*count)++;
}

static bool popup_uses_up(const tb_gui_event *event)
{
    return event->kind == TB_GUI_EVENT_KEY && event->key == TB_KEY_UP;
}

typedef struct key_box
{
    int keys;
} key_box;

static bool key_box_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    key_box *const box = data;

    (void)widget;
    if (popup_uses_up(event))
    {
        box->keys++;
        return true;
    }
    return false;
}

static tb_vec2 key_box_measure(tb_widget widget, const void *data)
{
    (void)widget;
    (void)data;
    return (tb_vec2){40.0f, 40.0f};
}

static const tb_widget_class key_box_kind = {.name = "key_box",
                                             .data_size = sizeof(key_box),
                                             .mouse_filter = TB_GUI_MOUSE_STOP,
                                             .measure = key_box_measure,
                                             .event = key_box_event};

TB_TEST(gui_popup, keys_go_to_the_popup_in_front_and_escape_closes_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget parent = gui_box(root, 50.0f, 50.0f);
    const tb_widget field = gui_box_of(root, gui_typing_box_class(), 50.0f, 50.0f);
    const tb_widget popup = tb_widget_create(parent, &key_box_kind);
    int parent_keys = 0;
    int closes = 0;

    TB_TEST_REQUIRE(tb_grid(parent, NULL));
    TB_TEST_REQUIRE(tb_grid(field, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_bind(parent, TB_GUI_EVENT_KEY, count_event, &parent_keys));
    TB_TEST_REQUIRE(tb_bind(popup, TB_GUI_EVENT_POPUP_CLOSE, count_event, &closes));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_widget_focus(field));
    TB_TEST_REQUIRE(tb_widget_popup(popup, NULL));
    gui_update(&fixture);

    /* The popup takes the keys, though a field has the focus; those it does
     * not use go to its ancestors. */
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_INT(((const key_box *)tb_widget_data(popup))->keys, ==, 1);
    TB_TEST_EXPECT_INT(parent_keys, ==, 0);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(parent_keys, ==, 1);
    /* Tab is a key like another while it is shown. */
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT_INT(parent_keys, ==, 2);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, field));

    /* Escape closes it, and goes no further. */
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT(!tb_widget_is_popup(popup));
    TB_TEST_EXPECT_INT(closes, ==, 1);
    TB_TEST_EXPECT_INT(parent_keys, ==, 2);
    /* Without the focus, the GUI takes the keyboard only while a popup is shown. */
    tb_gui_clear_focus(fixture.gui);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_gui_wants_keyboard(fixture.gui));
    TB_TEST_REQUIRE(tb_widget_popup(popup, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_gui_wants_keyboard(fixture.gui));
    tb_gui_close_popups(fixture.gui);
    /* Closed, keys go to the focus again. */
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_INT(((const key_box *)tb_widget_data(popup))->keys, ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_popup, popups_are_closed_by_hand_and_when_hidden_or_destroyed)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const owner made = make_owner(&fixture);
    const tb_widget second = gui_box(made.frame, 20.0f, 20.0f);
    const tb_widget third = gui_box(made.frame, 20.0f, 20.0f);
    int closes = 0;

    TB_TEST_REQUIRE(tb_bind(made.popup, TB_GUI_EVENT_POPUP_CLOSE, count_event, &closes));
    TB_TEST_REQUIRE(tb_bind(second, TB_GUI_EVENT_POPUP_CLOSE, count_event, &closes));
    TB_TEST_REQUIRE(tb_bind(third, TB_GUI_EVENT_POPUP_CLOSE, count_event, &closes));
    TB_TEST_REQUIRE(tb_widget_popup(made.popup, NULL));
    TB_TEST_REQUIRE(tb_widget_popup(second, NULL));
    TB_TEST_REQUIRE(tb_widget_popup(third, NULL));
    /* Closing one closes those in front of it. */
    tb_widget_close_popup(second);
    TB_TEST_EXPECT(!tb_widget_is_popup(third));
    TB_TEST_EXPECT(!tb_widget_is_popup(second));
    TB_TEST_EXPECT(tb_widget_is_popup(made.popup));
    TB_TEST_EXPECT_INT(closes, ==, 2);
    /* A widget that is not one closes nothing. */
    tb_widget_close_popup(second);
    tb_widget_close_popup(TB_WIDGET_NONE);
    TB_TEST_EXPECT_INT(closes, ==, 2);

    /* Hidden, or destroyed, it is closed without the event. */
    tb_widget_set_visible(made.popup, false);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.popup));
    tb_widget_set_visible(made.popup, true);
    TB_TEST_REQUIRE(tb_widget_popup(second, NULL));
    tb_widget_destroy(second);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).popup));
    TB_TEST_EXPECT_INT(closes, ==, 2);
    /* Closed, it is out of its parent's grid only while it was one. */
    TB_TEST_REQUIRE(tb_grid(third, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_widget_popup(third, NULL));
    gui_update(&fixture);
    tb_gui_close_popups(fixture.gui);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(third).width, ==, 20.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_popup, a_popup_is_in_front_of_a_modal_widget_and_takes_input_with_it_shown)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const owner made = make_owner(&fixture);
    const tb_widget dialog = gui_box(tb_gui_root(fixture.gui), 100.0f, 100.0f);
    const tb_widget inside = gui_box_of(made.popup, gui_focus_box_class(), 20.0f, 20.0f);
    tb_ui_command popup_command = {0};
    tb_ui_command dialog_command = {0};

    TB_TEST_REQUIRE(tb_grid(dialog, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_grid(inside, NULL));
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    TB_TEST_REQUIRE(
        tb_widget_popup(made.popup, &(tb_gui_popup_options){.anchor = {.x = 300.0f, .y = 300.0f}}));
    gui_update(&fixture);
    TB_TEST_REQUIRE(command_of(&fixture, made.popup, &popup_command));
    TB_TEST_REQUIRE(command_of(&fixture, dialog, &dialog_command));
    TB_TEST_EXPECT_INT(popup_command.layer, >, dialog_command.layer);
    gui_move(&fixture, 350.0f, 350.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, made.popup));
    /* What is in it takes the focus, though it is outside the modal widget. */
    TB_TEST_EXPECT(tb_widget_focus(inside));
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Tooltips.
 * ------------------------------------------------------------------------- */

static void rest(gui_fixture *fixture, double seconds)
{
    tb_gui_update(fixture->gui, seconds);
}

TB_TEST(gui_popup, a_tooltip_shows_after_the_mouse_rests_and_hides_when_it_moves_on)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget button = tb_button(root, &(tb_button_desc){.text = "Save"});
    const tb_widget other = gui_box(root, 100.0f, 40.0f);
    const tb_widget tip = tb_tooltip(button, &(tb_tooltip_desc){.text = "Save the level"});

    TB_TEST_REQUIRE(tb_grid(button, NULL));
    TB_TEST_REQUIRE(tb_grid(other, &(tb_grid_options){.column = 1}));
    gui_update(&fixture);
    const tb_vec2 middle = gui_middle(tb_widget_rect(button));

    gui_move(&fixture, middle.x, middle.y);
    rest(&fixture, 0.1);
    TB_TEST_EXPECT(!tb_widget_is_popup(tip));
    rest(&fixture, 1.0);
    TB_TEST_EXPECT(tb_widget_is_popup(tip));
    rest(&fixture, 0.0);
    /* Below the mouse, and passive: the button is still under the mouse. */
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(tip).y, >, middle.y);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(tip).width, >, 0.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, button));
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).popup, tip));

    /* A press hides it, and it stays hidden on the same widget. */
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT(!tb_widget_is_popup(tip));
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    rest(&fixture, 2.0);
    TB_TEST_EXPECT(!tb_widget_is_popup(tip));

    /* Moving to another widget, and back, shows it again after the delay. */
    const tb_vec2 away = gui_middle(tb_widget_rect(other));
    gui_move(&fixture, away.x, away.y);
    gui_move(&fixture, middle.x, middle.y);
    rest(&fixture, 1.0);
    TB_TEST_EXPECT(tb_widget_is_popup(tip));
    gui_move(&fixture, away.x, away.y);
    TB_TEST_EXPECT(!tb_widget_is_popup(tip));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_popup, a_tooltip_has_its_own_delay_and_a_key_hides_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), &(tb_frame_desc){.padding = 10});
    const tb_widget inner = gui_box(frame, 40.0f, 40.0f);
    const tb_widget tip = tb_tooltip(frame, &(tb_tooltip_desc){.text = "A frame", .delay = 2.0f});
    const tb_widget empty = tb_tooltip(inner, NULL);
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(inner, NULL));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_widget_get(tip, "delay", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 2.0f);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(tip, "delay", &negative));

    /* The box has an empty tooltip: its frame's shows, after its own delay. */
    const tb_vec2 middle = gui_middle(tb_widget_rect(inner));
    gui_move(&fixture, middle.x, middle.y);
    rest(&fixture, 1.0);
    TB_TEST_EXPECT(!tb_widget_is_popup(tip));
    rest(&fixture, 1.5);
    TB_TEST_EXPECT(tb_widget_is_popup(tip));
    TB_TEST_EXPECT(!tb_widget_is_popup(empty));
    gui_key(&fixture, TB_KEY_A, 0);
    TB_TEST_EXPECT(!tb_widget_is_popup(tip));
    /* Destroyed while shown, it is forgotten. */
    gui_move(&fixture, middle.x + 1.0f, middle.y);
    gui_move(&fixture, 700.0f, 500.0f);
    gui_move(&fixture, middle.x, middle.y);
    rest(&fixture, 3.0);
    TB_TEST_REQUIRE(tb_widget_is_popup(tip));
    tb_widget_destroy(tip);
    rest(&fixture, 0.0);
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).popup));
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "tooltip"), ==, tb_tooltip_class());
    gui_fixture_close(&fixture);
}
