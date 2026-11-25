/* Combo boxes, and what every kind that edits values shares: a list opened
 * by the mouse and the keys over every widget, values picked from it, text
 * and integer variables; each kind's properties read and set by name; and
 * their sizes at the scales a GUI is drawn at. */
#include "badger/str.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <math.h>
#include <string.h>

static const char *const modes[] = {"Windowed", "Borderless", "Fullscreen"};

static tb_widget placed_combo(gui_fixture *fixture, const tb_combobox_desc *desc)
{
    const tb_widget combo = tb_combobox(tb_gui_root(fixture->gui), desc);

    TB_TEST_REQUIRE(tb_grid(combo, &(tb_grid_options){.sticky = TB_STICKY_W}));
    gui_update(fixture);
    return combo;
}

static int32_t current_of(tb_widget combo)
{
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_widget_get(combo, "current", &value));
    return value.integer;
}

/* The first command a widget drew above the GUI's layer: its open list's
 * background, as large as the list. NULL when it drew none. */
static const tb_ui_command *list_of(const gui_fixture *fixture, tb_widget widget)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == widget.index + 1u && commands[index].layer > TB_GUI_DEFAULT_LAYER)
        {
            return &commands[index];
        }
    }
    return NULL;
}

TB_TEST(gui_combobox, the_mouse_opens_its_list_and_picks_a_value)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var mode = tb_var_string(fixture.gui, "Borderless");
    const tb_widget combo = placed_combo(
        &fixture, &(tb_combobox_desc){.values = modes, .value_count = 3, .variable = mode});
    /* A button under where the list drops: the list is above it. */
    const tb_widget under = tb_button(tb_gui_root(fixture.gui), &(tb_button_desc){.text = "Under"});
    gui_log log = {0};

    TB_TEST_REQUIRE(tb_grid(under, &(tb_grid_options){.row = 1, .sticky = TB_STICKY_W}));
    gui_update(&fixture);
    gui_record_all(combo, &log);
    TB_TEST_EXPECT_INT(current_of(combo), ==, 1);
    const tb_canvas_rect rect = tb_widget_rect(combo);

    gui_click(&fixture, gui_middle(rect));
    TB_TEST_EXPECT(tb_combobox_open(combo));
    TB_TEST_EXPECT((tb_widget_state(combo) & TB_WIDGET_FOCUSED) != 0);
    const tb_ui_command *const list = list_of(&fixture, combo);
    TB_TEST_REQUIRE_POINTER(list, !=, NULL);
    const tb_canvas_rect drop = list->rect;
    TB_TEST_EXPECT_FLOAT(drop.y, >=, rect.y + rect.height - 1.0f);

    /* Over the list, the mouse finds the combo box, not the button under
     * it: its last value is picked. */
    gui_move(&fixture, drop.x + 4.0f, drop.y + drop.height - 3.0f);
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.hovered, combo));
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(mode), TB_TEXT("Fullscreen")));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, combo), ==, 1);

    /* A press outside the list closes it, and goes on. */
    gui_click(&fixture, gui_middle(rect));
    TB_TEST_EXPECT(tb_combobox_open(combo));
    gui_click(&fixture, (tb_vec2){700.0f, 500.0f});
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    /* So does a second press on the box. */
    gui_click(&fixture, gui_middle(rect));
    gui_click(&fixture, gui_middle(rect));
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, combo), ==, 1);

    /* A value that is none of the list's is shown, and chosen by no index. */
    TB_TEST_EXPECT(tb_var_set_text(mode, "Other"));
    TB_TEST_EXPECT_INT(current_of(combo), ==, -1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_combobox, the_keys_open_move_through_and_choose)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var index = tb_var_int(fixture.gui, 0);
    const tb_widget combo = placed_combo(
        &fixture,
        &(tb_combobox_desc){.values = modes, .value_count = 3, .variable = index, .height = 2});
    const tb_widget other = tb_entry(tb_gui_root(fixture.gui), NULL);

    TB_TEST_REQUIRE(tb_grid(other, &(tb_grid_options){.row = 1}));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_widget_focus(combo));
    /* Closed, Up and Down choose; Home and End too. */
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 1);
    gui_key(&fixture, TB_KEY_END, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 2);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 2);
    gui_key(&fixture, TB_KEY_HOME, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 0);

    /* Open, they move the highlight; Enter chooses it. */
    gui_key(&fixture, TB_KEY_DOWN, TB_MODIFIER_ALT);
    TB_TEST_EXPECT(tb_combobox_open(combo));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 0);
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 2);

    /* Escape closes it unchosen, and is not left to its ancestors. */
    gui_log root_log = {0};
    TB_TEST_REQUIRE(tb_bind(tb_gui_root(fixture.gui), TB_GUI_EVENT_KEY, gui_record, &root_log));
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(tb_combobox_open(combo));
    gui_key(&fixture, TB_KEY_PAGE_UP, 0);
    gui_key(&fixture, TB_KEY_HOME, 0);
    gui_key(&fixture, TB_KEY_END, 0);
    gui_key(&fixture, TB_KEY_PAGE_DOWN, 0);
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 2);
    TB_TEST_EXPECT_UINT(root_log.count, ==, 0);
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT_UINT(root_log.count, ==, 1);

    /* Alt+Up closes it; losing the focus closes it. */
    gui_key(&fixture, TB_KEY_SPACE, 0);
    gui_key(&fixture, TB_KEY_UP, TB_MODIFIER_ALT);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    gui_key(&fixture, TB_KEY_SPACE, 0);
    gui_key(&fixture, TB_KEY_TAB, 0);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_combobox_open(combo));

    /* The wheel scrolls the open list, and chooses with the focus. */
    const tb_canvas_rect rect = tb_widget_rect(combo);
    TB_TEST_REQUIRE(tb_widget_focus(combo));
    gui_move(&fixture, rect.x + 4.0f, rect.y + 4.0f);
    gui_wheel(&fixture, 1.0f);
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 1);
    gui_key(&fixture, TB_KEY_SPACE, 0);
    gui_move(&fixture, rect.x + 4.0f, rect.y + rect.height + 4.0f);
    gui_wheel(&fixture, -1.0f);
    TB_TEST_EXPECT(tb_combobox_open(combo));
    TB_TEST_EXPECT_INT(tb_var_get_int(index), ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_combobox, its_own_value_and_its_values_as_lines)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget combo =
        placed_combo(&fixture, &(tb_combobox_desc){.values = modes, .value_count = 3});
    const tb_widget empty = tb_combobox(tb_gui_root(fixture.gui), NULL);
    tb_gui_value value = {0};

    TB_TEST_EXPECT_INT(current_of(combo), ==, 0);
    TB_TEST_REQUIRE(tb_widget_get(combo, "values", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("Windowed\nBorderless\nFullscreen")));
    const tb_gui_value two = tb_gui_int(2);
    const tb_gui_value three = tb_gui_int(3);
    const tb_gui_value none = tb_gui_int(-1);
    TB_TEST_EXPECT(tb_widget_set(combo, "current", &two));
    TB_TEST_EXPECT_INT(current_of(combo), ==, 2);
    TB_TEST_EXPECT(!tb_widget_set(combo, "current", &three));
    TB_TEST_EXPECT(tb_widget_set(combo, "current", &none));
    TB_TEST_EXPECT_INT(current_of(combo), ==, -1);

    /* New values; a last line break ends the last. */
    const tb_gui_value lines = tb_gui_text(TB_TEXT("Low\nHigh\n"));
    TB_TEST_REQUIRE(tb_widget_set(combo, "values", &lines));
    TB_TEST_EXPECT(tb_widget_set(combo, "current", &two) == false);
    TB_TEST_EXPECT(
        tb_widget_set(combo, "current", &(tb_gui_value){TB_GUI_TYPE_INT, {.integer = 1}}));

    /* Only text and integer variables are a combo box's. */
    const tb_gui_value flag = tb_gui_variable(tb_var_bool(fixture.gui, true));
    TB_TEST_EXPECT(!tb_widget_set(combo, "variable", &flag));
    const tb_gui_value zero = tb_gui_int(0);
    TB_TEST_EXPECT(!tb_widget_set(combo, "height", &zero));
    TB_TEST_EXPECT(!tb_widget_set(combo, "width", &none));

    /* With no values, it opens nothing. */
    TB_TEST_REQUIRE(tb_grid(empty, &(tb_grid_options){.row = 1}));
    gui_update(&fixture);
    gui_click(&fixture, gui_middle(tb_widget_rect(empty)));
    TB_TEST_EXPECT(!tb_combobox_open(empty));
    TB_TEST_EXPECT(!tb_combobox_open(tb_gui_root(fixture.gui)));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_combobox, its_list_opens_above_it_at_the_bottom_and_follows_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget combo =
        tb_combobox(root, &(tb_combobox_desc){.values = modes, .value_count = 3});

    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid(combo, &(tb_grid_options){.sticky = TB_STICKY_S}));
    gui_update(&fixture);
    const tb_canvas_rect rect = tb_widget_rect(combo);
    gui_click(&fixture, gui_middle(rect));
    TB_TEST_REQUIRE(tb_combobox_open(combo));
    /* The list is above it: a press just above it picks. */
    gui_move(&fixture, rect.x + 4.0f, rect.y - 4.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_INT(current_of(combo), ==, 2);

    /* Hidden, its list closes. */
    gui_click(&fixture, gui_middle(rect));
    TB_TEST_REQUIRE(tb_combobox_open(combo));
    gui_resize(&fixture, 640, 480, 1.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_combobox_open(combo));
    tb_widget_set_visible(combo, false);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * What every kind that edits values shares.
 * ------------------------------------------------------------------------- */

/* One of each kind that edits values. */
static void make_each(tb_widget parent, tb_widget *widgets)
{
    widgets[0] = tb_checkbutton(parent, &(tb_checkbutton_desc){.text = "Check"});
    widgets[1] = tb_radiobutton(parent, &(tb_radiobutton_desc){.text = "Radio"});
    widgets[2] = tb_entry(parent, NULL);
    widgets[3] = tb_text_box(parent, NULL);
    widgets[4] = tb_spinbox(parent, NULL);
    widgets[5] = tb_scale(parent, &(tb_scale_desc){.show_value = true});
    widgets[6] = tb_progressbar(parent, NULL);
    widgets[7] = tb_combobox(parent, &(tb_combobox_desc){.values = modes, .value_count = 3});
}

#define EACH_KIND 8

TB_TEST(gui_combobox, every_kind_reads_and_sets_its_properties_by_name)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_widget widgets[EACH_KIND] = {0};
    static const char *const names[EACH_KIND] = {"checkbutton", "radiobutton", "entry",
                                                 "text",        "spinbox",     "scale",
                                                 "progressbar", "combobox"};

    make_each(tb_gui_root(fixture.gui), widgets);
    for (size_t kind = 0; kind < EACH_KIND; kind++)
    {
        const tb_widget widget = widgets[kind];
        TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, names[kind]), ==,
                               tb_widget_class_of(widget));
        TB_TEST_EXPECT_POINTER(tb_widget_find_property(widget, "variable"), !=, NULL);
        /* Each property set to what it reads is taken. */
        for (size_t index = 0; index < tb_widget_property_count(widget); index++)
        {
            const tb_gui_property *const property = tb_widget_property_at(widget, index);
            tb_gui_value value = {0};
            if ((property->flags & TB_GUI_PROPERTY_READ_ONLY) != 0 ||
                tb_str_equal(property->name, "gridded"))
            {
                continue;
            }
            TB_TEST_REQUIRE(tb_widget_get(widget, property->name, &value));
            TB_TEST_EXPECT_UINT(value.type, ==, property->type);
            if (value.type == TB_GUI_TYPE_TEXT)
            {
                /* Text read is borrowed: copy it before it is set again. */
                char copy[256] = {0};
                TB_TEST_REQUIRE_UINT(value.text.length, <, sizeof(copy));
                memcpy(copy, value.text.bytes, value.text.length);
                value.text = tb_text_make(copy, value.text.length);
                TB_TEST_EXPECT(tb_widget_set(widget, property->name, &value));
                continue;
            }
            TB_TEST_EXPECT(tb_widget_set(widget, property->name, &value));
        }
        /* A value of another type is refused. */
        const tb_gui_value pair = tb_gui_vec2((tb_vec2){0});
        TB_TEST_EXPECT(!tb_widget_set(widget, "variable", &pair));
    }
    gui_fixture_close(&fixture);
}

TB_TEST(gui_combobox, every_kind_measures_alike_at_each_scale_on_whole_pixels)
{
    static const float scales[] = {1.0f, 1.5f, 2.0f};
    tb_vec2 sizes[3][EACH_KIND] = {0};

    for (size_t scale = 0; scale < 3; scale++)
    {
        gui_fixture fixture = {0};
        tb_widget widgets[EACH_KIND] = {0};
        gui_fixture_open(&fixture, scales[scale]);
        const tb_widget form = tb_frame(tb_gui_root(fixture.gui), &(tb_frame_desc){.padding = 3});
        TB_TEST_REQUIRE(tb_grid(form, NULL));
        make_each(form, widgets);
        for (size_t kind = 0; kind < EACH_KIND; kind++)
        {
            TB_TEST_REQUIRE(tb_grid(widgets[kind], &(tb_grid_options){.row = (int32_t)kind,
                                                                      .sticky = TB_STICKY_W,
                                                                      .pady = {2.0f, 2.0f}}));
        }
        gui_update(&fixture);
        for (size_t kind = 0; kind < EACH_KIND; kind++)
        {
            const tb_canvas_rect rect = tb_widget_rect(widgets[kind]);
            const float pixels = scales[scale];
            sizes[scale][kind] = tb_widget_natural_size(widgets[kind]);
            /* Something to see, a line high or more, on whole pixels. */
            TB_TEST_EXPECT_FLOAT(rect.width, >, 10.0f);
            TB_TEST_EXPECT_FLOAT(rect.height, >=, 8.0f);
            TB_TEST_EXPECT_FLOAT_NEAR(rect.x * pixels, roundf(rect.x * pixels), 1e-3f);
            TB_TEST_EXPECT_FLOAT_NEAR(rect.y * pixels, roundf(rect.y * pixels), 1e-3f);
        }
        gui_fixture_close(&fixture);
    }
    /* In units, every kind is near one size at every scale: text is
     * measured at the display's pixels, so a little apart. */
    for (size_t kind = 0; kind < EACH_KIND; kind++)
    {
        for (size_t scale = 1; scale < 3; scale++)
        {
            TB_TEST_EXPECT_FLOAT_NEAR(sizes[scale][kind].x, sizes[0][kind].x,
                                      sizes[0][kind].x * 0.08f + 1.0f);
            TB_TEST_EXPECT_FLOAT_NEAR(sizes[scale][kind].y, sizes[0][kind].y,
                                      sizes[0][kind].y * 0.08f + 1.0f);
        }
    }
}
