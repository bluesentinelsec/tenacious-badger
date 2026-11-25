/* Spin boxes, sliders, and progress bars: values stepped by keys, arrows,
 * and the wheel, typed and taken on Enter or when the focus leaves, dragged
 * along a track, rounded to a resolution and kept within a range, in
 * variables of any type; and bars filled, or a block moved by time. */
#include "gui_fixture.h"
#include "tb_test.h"

static float number_of(tb_widget widget, const char *name)
{
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_widget_get(widget, name, &value));
    return value.number;
}

static tb_widget placed(gui_fixture *fixture, tb_widget widget, int32_t row)
{
    TB_TEST_REQUIRE(tb_grid(widget, &(tb_grid_options){.row = row, .sticky = TB_STICKY_W}));
    gui_update(fixture);
    return widget;
}

/* The text of the first text command a widget drew, or empty. */
static tb_text drawn_text(const gui_fixture *fixture, tb_widget widget)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == widget.index + 1u && commands[index].type == TB_UI_COMMAND_TEXT)
        {
            return commands[index].text;
        }
    }
    return tb_text_make("", 0);
}

/* The widest rectangle a widget filled, as a bar's fill is. */
static tb_canvas_rect widest_fill(const gui_fixture *fixture, tb_widget widget, size_t skip)
{
    size_t count = 0;
    size_t seen = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == widget.index + 1u && commands[index].type == TB_UI_COMMAND_RECT)
        {
            if (seen == skip)
            {
                return commands[index].rect;
            }
            seen++;
        }
    }
    return (tb_canvas_rect){0};
}

TB_TEST(gui_range, a_spin_box_steps_within_its_range)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var limit = tb_var_int(fixture.gui, 60);
    gui_log log = {0};
    const tb_widget spin = placed(
        &fixture,
        tb_spinbox(tb_gui_root(fixture.gui),
                   &(tb_spinbox_desc){.variable = limit, .from = 30, .to = 240, .increment = 30}),
        0);

    gui_record_all(spin, &log);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("60")));
    TB_TEST_REQUIRE(tb_widget_focus(spin));
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 90);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, spin), ==, 1);
    gui_key(&fixture, TB_KEY_PAGE_UP, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 240);
    gui_key(&fixture, TB_KEY_PAGE_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 30);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 30);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, spin), ==, 3);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("30")));

    /* Its arrows: the upper half up, the lower half down. */
    const tb_canvas_rect rect = tb_widget_rect(spin);
    const float arrows = rect.x + rect.width - 4.0f;
    gui_click(&fixture, (tb_vec2){arrows, rect.y + 3.0f});
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 60);
    gui_click(&fixture, (tb_vec2){arrows, rect.y + rect.height - 3.0f});
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 30);

    /* The wheel steps it too. */
    gui_move(&fixture, rect.x + 4.0f, rect.y + 4.0f);
    gui_wheel(&fixture, 1.0f);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 60);

    /* Wrapping goes around past an end. */
    const tb_gui_value wrap = tb_gui_bool(true);
    TB_TEST_REQUIRE(tb_widget_set(spin, "wrap", &wrap));
    TB_TEST_EXPECT(tb_var_set_int(limit, 240));
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 30);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 240);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_range, a_spin_box_takes_typed_text_on_enter_or_when_the_focus_leaves)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget spin = placed(
        &fixture,
        tb_spinbox(
            tb_gui_root(fixture.gui),
            &(tb_spinbox_desc){
                .from = 0.0f, .to = 1.0f, .increment = 0.05f, .value = 0.5f, .format = "%.2f"}),
        0);
    const tb_widget other = placed(&fixture, tb_entry(tb_gui_root(fixture.gui), NULL), 1);
    gui_log log = {0};

    gui_record_all(spin, &log);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("0.50")));
    /* Tab selects its text; typing replaces it; nothing is taken yet. */
    gui_key(&fixture, TB_KEY_TAB, 0);
    gui_type(&fixture, "0.8");
    TB_TEST_EXPECT_FLOAT(number_of(spin, "value"), ==, 0.5f);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("0.8")));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, spin), ==, 0);
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_FLOAT(number_of(spin, "value"), ==, 0.8f);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("0.80")));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, spin), ==, 1);

    /* Out of range is kept within it, when the focus leaves. */
    gui_key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    gui_type(&fixture, "7");
    TB_TEST_REQUIRE(tb_widget_focus(other));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(number_of(spin, "value"), ==, 1.0f);

    /* Text that is no number gives back the value it had. */
    TB_TEST_REQUIRE(tb_widget_focus(spin));
    gui_key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    gui_type(&fixture, "abc");
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_FLOAT(number_of(spin, "value"), ==, 1.0f);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("1.00")));
    /* Escape gives it back too. */
    gui_type(&fixture, "9");
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("1.00")));

    /* Its properties: a format must write one number; an increment is more than 0. */
    const tb_gui_value bad = tb_gui_text(TB_TEXT("%d"));
    const tb_gui_value two = tb_gui_text(TB_TEXT("%f %f"));
    const tb_gui_value percent = tb_gui_text(TB_TEXT("%.0f%%"));
    const tb_gui_value zero = tb_gui_float(0.0f);
    TB_TEST_EXPECT(!tb_widget_set(spin, "format", &bad));
    TB_TEST_EXPECT(!tb_widget_set(spin, "format", &two));
    TB_TEST_EXPECT(tb_widget_set(spin, "format", &percent));
    TB_TEST_EXPECT(!tb_widget_set(spin, "increment", &zero));
    const tb_gui_value most = tb_gui_float(0.75f);
    TB_TEST_EXPECT(tb_widget_set(spin, "value", &most));
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, spin), TB_TEXT("1%")));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_range, a_slider_follows_the_mouse_and_the_keys)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var volume = tb_var_float(fixture.gui, 0.5);
    gui_log log = {0};
    const tb_widget scale =
        placed(&fixture,
               tb_scale(tb_gui_root(fixture.gui), &(tb_scale_desc){.variable = volume,
                                                                   .from = 0.0f,
                                                                   .to = 1.0f,
                                                                   .resolution = 0.1f,
                                                                   .length = 200.0f}),
               0);
    const tb_canvas_rect rect = tb_widget_rect(scale);
    const float middle = rect.y + rect.height * 0.5f;

    gui_record_all(scale, &log);
    TB_TEST_EXPECT_FLOAT(rect.width, ==, 200.0f);
    /* A press at the left end moves it there and gives it the focus. */
    gui_move(&fixture, rect.x, middle);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.0);
    TB_TEST_EXPECT((tb_widget_state(scale) & TB_WIDGET_FOCUSED) != 0);
    /* Dragged past the right end, it stops at 1. */
    gui_move(&fixture, rect.x + rect.width + 50.0f, middle);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 1.0);
    /* Rounded to its resolution on the way. */
    gui_move(&fixture, rect.x + rect.width * 0.52f, middle);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_var_get_float(volume), 0.5, 1e-6);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&fixture, rect.x, middle);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_var_get_float(volume), 0.5, 1e-6);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, scale), ==, 3);

    /* Left and Up toward `from`, Right and Down toward `to`, a page, the ends. */
    gui_key(&fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_var_get_float(volume), 0.6, 1e-6);
    gui_key(&fixture, TB_KEY_UP, 0);
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_var_get_float(volume), 0.4, 1e-6);
    gui_key(&fixture, TB_KEY_PAGE_DOWN, 0);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_var_get_float(volume), 0.5, 1e-6);
    gui_key(&fixture, TB_KEY_END, 0);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 1.0);
    gui_key(&fixture, TB_KEY_HOME, 0);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.0);
    /* Keys it does not use go on to its ancestors. */
    gui_log root_log = {0};
    TB_TEST_REQUIRE(tb_bind(tb_gui_root(fixture.gui), TB_GUI_EVENT_KEY, gui_record, &root_log));
    gui_key(&fixture, TB_KEY_ENTER, 0);
    gui_key(&fixture, TB_KEY_RIGHT, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(root_log.count, ==, 2);

    /* Set by a program: rounded, no event. */
    const tb_gui_value set = tb_gui_float(0.33f);
    TB_TEST_REQUIRE(tb_widget_set(scale, "value", &set));
    TB_TEST_EXPECT_FLOAT_NEAR(tb_var_get_float(volume), 0.3, 1e-6);
    TB_TEST_EXPECT_FLOAT_NEAR(number_of(scale, "value"), 0.3f, 1e-6f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_range, a_slider_runs_down_and_shows_its_value)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var level = tb_var_int(fixture.gui, 3);
    const tb_widget scale =
        placed(&fixture,
               tb_scale(tb_gui_root(fixture.gui), &(tb_scale_desc){.variable = level,
                                                                   .from = 0.0f,
                                                                   .to = 10.0f,
                                                                   .resolution = 1.0f,
                                                                   .orient = TB_GUI_ORIENT_VERTICAL,
                                                                   .show_value = true}),
               0);
    const tb_canvas_rect rect = tb_widget_rect(scale);

    TB_TEST_EXPECT_FLOAT(rect.height, ==, 160.0f);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, scale), TB_TEXT("3")));
    /* `from` at the top, `to` at the bottom. */
    gui_move(&fixture, rect.x + 4.0f, rect.y + rect.height - 1.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_INT(tb_var_get_int(level), ==, 10);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, scale), TB_TEXT("10")));
    const tb_gui_value format = tb_gui_text(TB_TEXT("%.1f dB"));
    TB_TEST_REQUIRE(tb_widget_set(scale, "format", &format));
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_text_equal(drawn_text(&fixture, scale), TB_TEXT("10.0 dB")));
    /* Wider with its value written beside it. */
    const tb_gui_value hidden = tb_gui_bool(false);
    const float wide = tb_widget_natural_size(scale).x;
    TB_TEST_REQUIRE(tb_widget_set(scale, "show_value", &hidden));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(scale).x, <, wide);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(scale, "resolution", &negative));
    TB_TEST_EXPECT(!tb_widget_set(scale, "length", &negative));
    TB_TEST_EXPECT(!tb_widget_set(scale, "big_increment", &negative));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_range, a_progress_bar_fills_by_its_value_or_moves_a_block)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var done = tb_var_float(fixture.gui, 25.0);
    const tb_widget bar = placed(
        &fixture,
        tb_progressbar(tb_gui_root(fixture.gui), &(tb_progressbar_desc){.variable = done}), 0);
    const tb_canvas_rect rect = tb_widget_rect(bar);

    TB_TEST_EXPECT_FLOAT(rect.width, ==, 160.0f);
    /* The trough, then the fill: a quarter of it. */
    TB_TEST_EXPECT_FLOAT_NEAR(widest_fill(&fixture, bar, 1).width, 40.0f, 1.0f);
    TB_TEST_EXPECT(tb_var_set_float(done, 200.0));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(widest_fill(&fixture, bar, 1).width, 160.0f, 1.0f);
    /* It takes no input. */
    gui_move(&fixture, rect.x + 4.0f, rect.y + 2.0f);
    TB_TEST_EXPECT(!tb_gui_wants_mouse(fixture.gui));

    /* Indeterminate, a block moves to and fro with time. */
    const tb_gui_value moving = tb_gui_enum(TB_PROGRESS_INDETERMINATE);
    TB_TEST_REQUIRE(tb_widget_set(bar, "mode", &moving));
    tb_gui_update(fixture.gui, 0.25);
    const tb_canvas_rect first = widest_fill(&fixture, bar, 1);
    tb_gui_update(fixture.gui, 0.25);
    const tb_canvas_rect second = widest_fill(&fixture, bar, 1);
    TB_TEST_EXPECT_FLOAT_NEAR(first.width, 40.0f, 1.0f);
    TB_TEST_EXPECT_FLOAT(second.x, !=, first.x);

    /* Down, its own value, and its maximum. */
    const tb_gui_value down = tb_gui_enum(TB_GUI_ORIENT_VERTICAL);
    const tb_gui_value unbound = tb_gui_variable(TB_VAR_NONE);
    const tb_gui_value value = tb_gui_float(5.0f);
    const tb_gui_value maximum = tb_gui_float(10.0f);
    const tb_gui_value zero = tb_gui_float(0.0f);
    const tb_gui_value fixed = tb_gui_enum(TB_PROGRESS_DETERMINATE);
    TB_TEST_REQUIRE(tb_widget_set(bar, "orient", &down));
    TB_TEST_REQUIRE(tb_widget_set(bar, "variable", &unbound));
    TB_TEST_REQUIRE(tb_widget_set(bar, "value", &value));
    TB_TEST_REQUIRE(tb_widget_set(bar, "maximum", &maximum));
    TB_TEST_REQUIRE(tb_widget_set(bar, "mode", &fixed));
    TB_TEST_EXPECT(!tb_widget_set(bar, "maximum", &zero));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(bar).height, ==, 160.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(widest_fill(&fixture, bar, 1).height, 80.0f, 1.0f);
    TB_TEST_EXPECT_FLOAT(number_of(bar, "value"), ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(number_of(bar, "maximum"), ==, 10.0f);
    TB_TEST_EXPECT_FLOAT(number_of(bar, "length"), ==, 160.0f);
    gui_fixture_close(&fixture);
}
