/* Check buttons and radio buttons: toggled and chosen by the mouse, Space,
 * the arrows, and invoking; their variables kept in step with every widget
 * on them; their own state without one; their CHANGED events and commands;
 * and what they draw. */
#include "gui_fixture.h"
#include "tb_test.h"

/* Times a command was called, and the last event's kind. */
typedef struct calls
{
    int count;
    tb_gui_event_kind kind;
} calls;

static void count_call(const tb_gui_event *event, void *user)
{
    calls *const record = user;

    record->count++;
    record->kind = event->kind;
}

static bool selected(tb_widget widget)
{
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_widget_get(widget, "selected", &value));
    return value.boolean;
}

static void place(gui_fixture *fixture, tb_widget widget, int32_t row)
{
    TB_TEST_REQUIRE(tb_grid(widget, &(tb_grid_options){.row = row, .sticky = TB_STICKY_W}));
    gui_update(fixture);
}

TB_TEST(gui_choice, a_check_button_toggles_its_variable_by_its_values)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_var music = tb_var_string(fixture.gui, "off");
    calls command = {0};
    gui_log log = {0};
    const tb_widget check = tb_checkbutton(root, &(tb_checkbutton_desc){.text = "Music",
                                                                        .variable = music,
                                                                        .on_value = "on",
                                                                        .off_value = "off",
                                                                        .command = count_call,
                                                                        .user = &command});
    const tb_widget twin = tb_checkbutton(
        root, &(tb_checkbutton_desc){.variable = music, .on_value = "on", .off_value = "off"});

    place(&fixture, check, 0);
    place(&fixture, twin, 1);
    gui_record_all(check, &log);
    TB_TEST_EXPECT(!selected(check));

    /* A click toggles it, sets its variable, says so, and calls its command. */
    gui_click(&fixture, gui_middle(tb_widget_rect(check)));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(music), TB_TEXT("on")));
    TB_TEST_EXPECT(selected(check));
    TB_TEST_EXPECT(selected(twin));
    TB_TEST_EXPECT_INT(command.count, ==, 1);
    TB_TEST_EXPECT_UINT(command.kind, ==, TB_GUI_EVENT_CHANGED);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, check), ==, 1);

    /* Space with the focus, and invoking, toggle it back and forth. */
    TB_TEST_REQUIRE(tb_widget_focus(check));
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(music), TB_TEXT("off")));
    TB_TEST_EXPECT(tb_widget_invoke(check));
    TB_TEST_EXPECT(selected(twin));
    TB_TEST_EXPECT_INT(command.count, ==, 3);
    /* Enter, Accept, toggles it too. */
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_INT(command.count, ==, 4);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(music), TB_TEXT("off")));

    /* Set by a program: no event, no command; its twin follows. */
    TB_TEST_EXPECT(tb_var_set_text(music, "off"));
    gui_update(&fixture);
    TB_TEST_EXPECT(!selected(check));
    const tb_gui_value on = tb_gui_bool(true);
    TB_TEST_EXPECT(tb_widget_set(twin, "selected", &on));
    TB_TEST_EXPECT(selected(check));
    TB_TEST_EXPECT_INT(command.count, ==, 4);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, check), ==, 4);

    /* A value of neither makes it cleared. */
    TB_TEST_EXPECT(tb_var_set_text(music, "loud"));
    TB_TEST_EXPECT(!selected(check));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_choice, a_check_button_without_a_variable_keeps_its_own_state)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget check = tb_checkbutton(tb_gui_root(fixture.gui), NULL);
    const tb_var flag = tb_var_bool(fixture.gui, false);
    const tb_var count = tb_var_int(fixture.gui, 0);
    tb_gui_value value = {0};

    place(&fixture, check, 0);
    TB_TEST_EXPECT(!selected(check));
    gui_click(&fixture, gui_middle(tb_widget_rect(check)));
    TB_TEST_EXPECT(selected(check));
    TB_TEST_REQUIRE(tb_widget_get(check, "on_value", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("1")));

    /* "1" and "0" fit bool and integer variables alike. */
    const tb_gui_value bound = tb_gui_variable(flag);
    TB_TEST_REQUIRE(tb_widget_set(check, "variable", &bound));
    TB_TEST_EXPECT(!selected(check));
    TB_TEST_EXPECT(tb_widget_invoke(check));
    TB_TEST_EXPECT(tb_var_get_bool(flag));
    const tb_gui_value counted = tb_gui_variable(count);
    TB_TEST_REQUIRE(tb_widget_set(check, "variable", &counted));
    TB_TEST_EXPECT(tb_widget_invoke(check));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, 1);

    /* Disabled, it takes neither clicks nor invoking. */
    tb_widget_set_enabled(check, false);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_invoke(check));
    gui_click(&fixture, gui_middle(tb_widget_rect(check)));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_choice, radio_buttons_share_a_variable_each_with_its_value)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget group = tb_frame(root, NULL);
    const tb_var level = tb_var_int(fixture.gui, 2);
    calls command = {0};
    tb_widget radios[3] = {0};
    static const char *const values[] = {"1", "2", "3"};
    gui_log log = {0};

    place(&fixture, group, 0);
    for (int32_t index = 0; index < 3; index++)
    {
        radios[index] = tb_radiobutton(group, &(tb_radiobutton_desc){.text = values[index],
                                                                     .variable = level,
                                                                     .value = values[index],
                                                                     .command = count_call,
                                                                     .user = &command});
        place(&fixture, radios[index], index);
    }
    /* A label after them is no radio button: past the last, the arrows stop. */
    const tb_widget label = tb_label(group, &(tb_label_desc){.text = "Between"});
    TB_TEST_REQUIRE(tb_grid(label, &(tb_grid_options){.row = 4}));
    gui_record_all(radios[0], &log);
    TB_TEST_EXPECT(!selected(radios[0]));
    TB_TEST_EXPECT(selected(radios[1]));

    gui_click(&fixture, gui_middle(tb_widget_rect(radios[0])));
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 1);
    TB_TEST_EXPECT(selected(radios[0]));
    TB_TEST_EXPECT(!selected(radios[1]));
    TB_TEST_EXPECT_INT(command.count, ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, radios[0]), ==, 1);

    /* Choosing the one chosen changes nothing. */
    gui_click(&fixture, gui_middle(tb_widget_rect(radios[0])));
    TB_TEST_EXPECT_INT(command.count, ==, 1);

    /* The arrows choose the next and the one before, and take the focus. */
    TB_TEST_REQUIRE(tb_widget_focus(radios[0]));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 2);
    gui_key(&fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 3);
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.focus, radios[2]));
    /* Past the last, nothing moves. */
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 3);
    gui_key(&fixture, TB_KEY_UP, 0);
    gui_key(&fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 1);
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 1);
    /* Space on the one chosen changes nothing. */
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT_INT(command.count, ==, 5);

    /* A radio button cleared by a program leaves the group's variable. */
    const tb_gui_value off = tb_gui_bool(false);
    const tb_gui_value on = tb_gui_bool(true);
    TB_TEST_EXPECT(tb_widget_set(radios[0], "selected", &off));
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 1);
    TB_TEST_EXPECT(tb_widget_set(radios[2], "selected", &on));
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 3);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_choice, a_radio_button_alone_chooses_itself)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget radio =
        tb_radiobutton(tb_gui_root(fixture.gui), &(tb_radiobutton_desc){.text = "Alone"});

    place(&fixture, radio, 0);
    TB_TEST_REQUIRE(tb_widget_focus(radio));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(!selected(radio));
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(selected(radio));
    const tb_gui_value off = tb_gui_bool(false);
    TB_TEST_EXPECT(tb_widget_set(radio, "selected", &off));
    TB_TEST_EXPECT(!selected(radio));
    gui_fixture_close(&fixture);
}

/* The commands a widget drew of a type. */
static size_t drawn(const gui_fixture *fixture, tb_widget widget, tb_ui_command_type type)
{
    size_t count = 0;
    size_t found = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        found += commands[index].id == widget.index + 1u && commands[index].type == type ? 1 : 0;
    }
    return found;
}

TB_TEST(gui_choice, they_draw_their_state_and_measure_their_text)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget check = tb_checkbutton(root, &(tb_checkbutton_desc){.text = "Subtitles"});
    const tb_widget radio = tb_radiobutton(root, &(tb_radiobutton_desc){.text = "Subtitles"});
    const tb_widget bare = tb_checkbutton(root, NULL);

    place(&fixture, check, 0);
    place(&fixture, radio, 1);
    place(&fixture, bare, 2);
    /* Its indicator and its text, side by side, a line high. */
    const tb_vec2 size = tb_widget_natural_size(check);
    const tb_vec2 alone = tb_widget_natural_size(bare);
    TB_TEST_EXPECT_FLOAT(size.y, ==, alone.y);
    TB_TEST_EXPECT_FLOAT(size.x, >, alone.x + 40.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(radio).x, ==, size.x);

    const size_t lines = drawn(&fixture, check, TB_UI_COMMAND_LINE);
    TB_TEST_EXPECT_UINT(lines, ==, 0);
    TB_TEST_EXPECT_UINT(drawn(&fixture, check, TB_UI_COMMAND_TEXT), ==, 1);
    TB_TEST_EXPECT(tb_widget_invoke(check));
    TB_TEST_EXPECT(tb_widget_invoke(radio));
    TB_TEST_REQUIRE(tb_widget_focus(check));
    /* The focus ring is drawn while the focus is shown. */
    tb_gui_set_focus_visible(fixture.gui, true);
    gui_update(&fixture);
    /* Ticked: a tick of two lines; chosen: a dot. */
    TB_TEST_EXPECT_UINT(drawn(&fixture, check, TB_UI_COMMAND_LINE), ==, 2);
    TB_TEST_EXPECT_UINT(drawn(&fixture, radio, TB_UI_COMMAND_RECT), ==, 2);
    TB_TEST_EXPECT_UINT(drawn(&fixture, check, TB_UI_COMMAND_OUTLINE), ==, 2);
    tb_widget_set_enabled(radio, false);
    TB_TEST_REQUIRE(tb_widget_focus(radio) == false);
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(drawn(&fixture, radio, TB_UI_COMMAND_RECT), ==, 2);
    gui_fixture_close(&fixture);
}
