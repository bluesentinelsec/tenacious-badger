/* Navigation: the focus moved by the arrows and a gamepad's actions to the
 * nearest widget in a direction, in grids regular and not; neighbours
 * named over the geometry; containers that wrap; each kind's engagement;
 * Accept and Back; modal widgets that trap the focus and give it back;
 * scroll frames that follow it; the focus shown only after the keys; and
 * the key map rebound. */
#include "gui_fixture.h"
#include "tb_test.h"

static tb_widget focused(const gui_fixture *fixture)
{
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture->gui, &stats);
    return stats.focus;
}

/* An action fed, as a gamepad's, then handled. */
static void nav(gui_fixture *fixture, tb_gui_nav action)
{
    tb_gui_feed_navigation(fixture->gui, action);
    gui_update(fixture);
}

/* A focusable box in a cell of a grid. */
static tb_widget cell(tb_widget parent, int32_t row, int32_t column, float width, float height)
{
    const tb_widget box = gui_box_of(parent, gui_focus_box_class(), width, height);
    TB_TEST_REQUIRE(tb_grid(
        box, &(tb_grid_options){.row = row, .column = column, .padx = {4, 4}, .pady = {4, 4}}));
    return box;
}

/* A three-by-three grid of boxes of one size. */
typedef struct grid_case
{
    gui_fixture fixture;
    tb_widget form;
    tb_widget boxes[3][3];
} grid_case;

static void open_grid(grid_case *made)
{
    gui_fixture_open(&made->fixture, 1.0f);
    made->form = tb_frame(tb_gui_root(made->fixture.gui), NULL);
    TB_TEST_REQUIRE(tb_grid(made->form, NULL));
    for (int32_t row = 0; row < 3; row++)
    {
        for (int32_t column = 0; column < 3; column++)
        {
            made->boxes[row][column] = cell(made->form, row, column, 60.0f, 30.0f);
        }
    }
    gui_update(&made->fixture);
}

TB_TEST(gui_navigation, the_arrows_move_to_the_nearest_widget_in_a_grid)
{
    grid_case made = {0};
    open_grid(&made);
    gui_fixture *const fixture = &made.fixture;

    /* Nothing is focused, and nothing is marked default: the arrows leave
     * the focus alone, so a game's own keys stay its own. */
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(!tb_widget_alive(focused(fixture)));

    TB_TEST_REQUIRE(tb_widget_focus(made.boxes[1][1]));
    gui_key(fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][1]));
    gui_key(fixture, TB_KEY_DOWN, 0);
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[2][1]));
    gui_key(fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[2][0]));
    /* At the edge, nothing is that way: the focus stays. */
    gui_key(fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[2][0]));
    /* A gamepad's actions do the same. */
    nav(fixture, TB_GUI_NAV_RIGHT);
    nav(fixture, TB_GUI_NAV_RIGHT);
    nav(fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][2]));
    /* So does the function, now. */
    TB_TEST_EXPECT(tb_gui_focus_direction(fixture->gui, TB_GUI_NAV_UP));
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][2]));
    TB_TEST_EXPECT(!tb_gui_focus_direction(fixture->gui, TB_GUI_NAV_UP));
    TB_TEST_EXPECT(!tb_gui_focus_direction(fixture->gui, TB_GUI_NAV_ACCEPT));
    /* Hidden and disabled widgets are passed over. */
    tb_widget_set_enabled(made.boxes[0][1], false);
    gui_update(fixture);
    gui_key(fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][0]));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, an_irregular_layout_chooses_the_nearest_then_the_one_in_line)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget form = tb_frame(tb_gui_root(fixture.gui), NULL);
    TB_TEST_REQUIRE(tb_grid(form, NULL));
    /* A narrow box at the top right, a wide one below spanning both
     * columns, and a tall one at the left of the top row. */
    const tb_widget tall = cell(form, 0, 0, 100.0f, 80.0f);
    const tb_widget narrow = cell(form, 0, 1, 40.0f, 20.0f);
    const tb_widget wide = gui_box_of(form, gui_focus_box_class(), 200.0f, 20.0f);
    TB_TEST_REQUIRE(tb_grid(wide, &(tb_grid_options){.row = 1, .column_span = 2}));
    /* A box off to the lower right, nearer by its edge than in line. */
    const tb_widget corner = cell(form, 2, 2, 20.0f, 20.0f);
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(narrow));
    /* Below the narrow box, the wide one overlaps it: in line, and nearest. */
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), wide));
    /* Up from the wide one: the tall box and the narrow one both overlap it
     * across; the tall one's edge is nearer. */
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), tall));
    /* Right of the tall box, only the narrow one lies wholly past its edge
     * and in its rows; the corner is farther. */
    gui_key(&fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), narrow));
    /* Down from the wide one, only the corner lies past it, off to a side. */
    TB_TEST_REQUIRE(tb_widget_focus(wide));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), corner));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, a_tie_goes_to_the_one_in_line_then_the_first_in_the_tree)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget form = tb_frame(tb_gui_root(fixture.gui), NULL);
    TB_TEST_REQUIRE(tb_grid(form, NULL));
    /* Two boxes below a wide one: both overlap it, at one distance. */
    const tb_widget top = gui_box_of(form, gui_focus_box_class(), 200.0f, 20.0f);
    TB_TEST_REQUIRE(tb_grid(top, &(tb_grid_options){.column_span = 2}));
    const tb_widget left = cell(form, 1, 0, 60.0f, 20.0f);
    const tb_widget right = cell(form, 1, 1, 60.0f, 20.0f);
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(top));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), left));
    gui_key(&fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), right));
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), top));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, neighbours_named_override_the_geometry)
{
    grid_case made = {0};
    open_grid(&made);
    gui_fixture *const fixture = &made.fixture;
    tb_widget(*const boxes)[3] = made.boxes;

    /* Right from the top left goes to the bottom right; Tab from the
     * middle goes to the top left, Shift+Tab to the bottom left. */
    TB_TEST_EXPECT(tb_widget_set_focus_neighbor(boxes[0][0], TB_GUI_NAV_RIGHT, boxes[2][2]));
    TB_TEST_EXPECT(tb_widget_set_focus_neighbor(boxes[1][1], TB_GUI_NAV_NEXT, boxes[0][0]));
    TB_TEST_EXPECT(tb_widget_set_focus_neighbor(boxes[1][1], TB_GUI_NAV_PREVIOUS, boxes[2][0]));
    TB_TEST_EXPECT(
        tb_widget_equal(tb_widget_focus_neighbor(boxes[0][0], TB_GUI_NAV_RIGHT), boxes[2][2]));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_focus_neighbor(boxes[0][0], TB_GUI_NAV_LEFT)));
    /* Only the six sides are sides. */
    TB_TEST_EXPECT(!tb_widget_set_focus_neighbor(boxes[0][0], TB_GUI_NAV_ACCEPT, boxes[1][1]));
    TB_TEST_EXPECT(!tb_widget_set_focus_neighbor(boxes[0][0], TB_GUI_NAV_TAB_NEXT, boxes[1][1]));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_focus_neighbor(boxes[0][0], TB_GUI_NAV_BACK)));

    TB_TEST_REQUIRE(tb_widget_focus(boxes[0][0]));
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[2][2]));
    TB_TEST_REQUIRE(tb_widget_focus(boxes[1][1]));
    gui_key(fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[0][0]));
    TB_TEST_REQUIRE(tb_widget_focus(boxes[1][1]));
    gui_key(fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[2][0]));

    /* A neighbour hidden is passed over: the geometry decides. */
    tb_widget_set_visible(boxes[2][2], false);
    gui_update(fixture);
    TB_TEST_REQUIRE(tb_widget_focus(boxes[0][0]));
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[0][1]));
    /* A neighbour forgotten, or destroyed, is none. */
    TB_TEST_EXPECT(tb_widget_set_focus_neighbor(boxes[1][1], TB_GUI_NAV_NEXT, TB_WIDGET_NONE));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_focus_neighbor(boxes[1][1], TB_GUI_NAV_NEXT)));
    tb_widget_destroy(boxes[2][0]);
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_focus_neighbor(boxes[1][1], TB_GUI_NAV_PREVIOUS)));
    TB_TEST_EXPECT(!tb_widget_set_focus_neighbor(boxes[2][0], TB_GUI_NAV_UP, boxes[1][1]));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, neighbours_are_named_by_path_as_layout_files_name_them)
{
    grid_case made = {0};
    open_grid(&made);
    gui_fixture *const fixture = &made.fixture;
    tb_widget(*const boxes)[3] = made.boxes;
    tb_gui_value value = {0};

    tb_widget_set_name(made.form, "form");
    tb_widget_set_name(boxes[2][2], "corner");
    tb_widget_set_name(boxes[2][0], "low");
    /* A path is looked up from the widget's parent up to the root: the
     * nearest that has it. */
    const tb_gui_value corner = tb_gui_text(TB_TEXT("corner"));
    const tb_gui_value full = tb_gui_text(TB_TEXT("form/low"));
    const tb_gui_value nowhere = tb_gui_text(TB_TEXT("form/nothing"));
    const tb_gui_value none = tb_gui_text(TB_TEXT(""));
    TB_TEST_REQUIRE(tb_widget_set(boxes[0][0], "focus_right", &corner));
    TB_TEST_REQUIRE(tb_widget_set(boxes[0][0], "focus_down", &full));
    TB_TEST_EXPECT(
        tb_widget_equal(tb_widget_focus_neighbor(boxes[0][0], TB_GUI_NAV_RIGHT), boxes[2][2]));
    TB_TEST_REQUIRE(tb_widget_focus(boxes[0][0]));
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[2][2]));
    TB_TEST_REQUIRE(tb_widget_focus(boxes[0][0]));
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[2][0]));
    TB_TEST_REQUIRE(tb_widget_get(boxes[0][0], "focus_down", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("form/low")));

    /* A path to nothing is passed over: the geometry decides. */
    TB_TEST_REQUIRE(tb_widget_set(boxes[0][0], "focus_right", &nowhere));
    TB_TEST_REQUIRE(tb_widget_focus(boxes[0][0]));
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), boxes[0][1]));

    /* The last given wins: a handle over a path, read back as its path
     * from the root; a path over a handle; and none clears it. */
    TB_TEST_REQUIRE(tb_widget_set_focus_neighbor(boxes[0][0], TB_GUI_NAV_RIGHT, boxes[2][0]));
    TB_TEST_REQUIRE(tb_widget_get(boxes[0][0], "focus_right", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("form/low")));
    TB_TEST_REQUIRE(tb_widget_set(boxes[0][0], "focus_right", &corner));
    TB_TEST_EXPECT(
        tb_widget_equal(tb_widget_focus_neighbor(boxes[0][0], TB_GUI_NAV_RIGHT), boxes[2][2]));
    TB_TEST_REQUIRE(tb_widget_set(boxes[0][0], "focus_right", &none));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_focus_neighbor(boxes[0][0], TB_GUI_NAV_RIGHT)));
    /* A neighbour by handle with no path from the root reads as none. */
    TB_TEST_REQUIRE(tb_widget_set_focus_neighbor(boxes[0][0], TB_GUI_NAV_LEFT, boxes[1][1]));
    TB_TEST_REQUIRE(tb_widget_get(boxes[0][0], "focus_left", &value));
    TB_TEST_EXPECT_UINT(value.text.length, ==, 0);
    /* Each side is its own property; a widget that names none reads none. */
    static const char *const sides[] = {"focus_up",    "focus_down", "focus_left",
                                        "focus_right", "focus_next", "focus_previous"};
    for (size_t side = 0; side < TB_TEST_COUNT_OF(sides); side++)
    {
        TB_TEST_REQUIRE(tb_widget_set(boxes[1][2], sides[side], &corner));
        TB_TEST_REQUIRE(tb_widget_get(boxes[1][2], sides[side], &value));
        TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("corner")));
        TB_TEST_REQUIRE(tb_widget_get(boxes[2][1], sides[side], &value));
        TB_TEST_EXPECT_UINT(value.text.length, ==, 0);
    }
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_container_that_wraps_goes_around_its_own_widgets)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget list = tb_frame(root, NULL);
    TB_TEST_REQUIRE(tb_grid(list, NULL));
    tb_widget rows[3] = {0};
    for (int32_t row = 0; row < 3; row++)
    {
        rows[row] = cell(list, row, 0, 80.0f, 20.0f);
    }
    const tb_widget below = cell(root, 1, 0, 80.0f, 20.0f);
    gui_update(&fixture);

    /* Without wrapping, Down from the list's last goes on below it. */
    TB_TEST_REQUIRE(tb_widget_focus(rows[2]));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), below));

    /* Wrapping along its rows, it keeps the focus and goes around. */
    const tb_gui_value vertical = tb_gui_flags(TB_GUI_WRAP_VERTICAL);
    const tb_gui_value both = tb_gui_flags(TB_GUI_WRAP_VERTICAL | TB_GUI_WRAP_HORIZONTAL);
    const tb_gui_value unnamed = tb_gui_flags(4u);
    const tb_gui_value none = tb_gui_flags(0u);
    TB_TEST_REQUIRE(tb_widget_set(list, "focus_wrap", &vertical));
    TB_TEST_REQUIRE(tb_widget_focus(rows[2]));
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), rows[0]));
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), rows[2]));
    /* Inside, it moves as ever. */
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), rows[1]));
    /* Across, it does not wrap: nothing is right of it. */
    gui_key(&fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), rows[1]));

    /* The property reads as set, and refuses bits it has no names for. */
    tb_gui_value value = {0};
    TB_TEST_REQUIRE(tb_widget_get(list, "focus_wrap", &value));
    TB_TEST_EXPECT_UINT(value.flags, ==, TB_GUI_WRAP_VERTICAL);
    TB_TEST_EXPECT(!tb_widget_set(list, "focus_wrap", &unnamed));
    TB_TEST_REQUIRE(tb_widget_get(rows[0], "focus_wrap", &value));
    TB_TEST_EXPECT_UINT(value.flags, ==, 0);
    /* Wrapping across too, it goes around to itself, alone in its row. */
    TB_TEST_REQUIRE(tb_widget_set(list, "focus_wrap", &both));
    TB_TEST_EXPECT(tb_gui_focus_direction(fixture.gui, TB_GUI_NAV_RIGHT));
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), rows[1]));
    TB_TEST_REQUIRE(tb_widget_set(list, "focus_wrap", &none));
    TB_TEST_REQUIRE(tb_widget_get(list, "focus_wrap", &value));
    TB_TEST_EXPECT_UINT(value.flags, ==, 0);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, the_default_focus_starts_a_screen)
{
    grid_case made = {0};
    open_grid(&made);
    gui_fixture *const fixture = &made.fixture;
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_widget_get(made.boxes[1][2], "default_focus", &value));
    TB_TEST_EXPECT(!value.boolean);
    const tb_gui_value yes = tb_gui_bool(true);
    const tb_gui_value no = tb_gui_bool(false);
    TB_TEST_REQUIRE(tb_widget_set(made.boxes[1][2], "default_focus", &yes));
    TB_TEST_REQUIRE(tb_widget_get(made.boxes[1][2], "default_focus", &value));
    TB_TEST_EXPECT(value.boolean);
    /* With nothing focused, a direction gives the focus to the default. */
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][2]));
    TB_TEST_EXPECT(!tb_widget_engaged(made.boxes[1][2]));
    /* And so does Accept. */
    tb_gui_clear_focus(fixture->gui);
    gui_key(fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][2]));
    /* A screen's default focus, by hand; a screen without one gives it to
     * its first. */
    TB_TEST_EXPECT(tb_widget_focus_default(made.form));
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][2]));
    TB_TEST_REQUIRE(tb_widget_set(made.boxes[1][2], "default_focus", &no));
    TB_TEST_EXPECT(tb_widget_focus_default(made.form));
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][0]));
    /* A screen with nothing to focus keeps the focus where it is. */
    const tb_widget empty = tb_frame(tb_gui_root(fixture->gui), NULL);
    TB_TEST_EXPECT(!tb_widget_focus_default(empty));
    TB_TEST_EXPECT(!tb_widget_focus_default(TB_WIDGET_NONE));
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][0]));
    gui_fixture_close(fixture);
}

/* ---------------------------------------------------------------------------
 * Engagement.
 * ------------------------------------------------------------------------- */

/* A kind's widget between two boxes, above and below, and beside a third
 * at its right. */
typedef struct engage_case
{
    gui_fixture fixture;
    tb_widget above;
    tb_widget below;
    tb_widget beside;
} engage_case;

static void open_engage(engage_case *made)
{
    gui_fixture_open(&made->fixture, 1.0f);
    const tb_widget root = tb_gui_root(made->fixture.gui);
    made->above = cell(root, 0, 0, 60.0f, 20.0f);
    made->below = cell(root, 2, 0, 60.0f, 20.0f);
    made->beside = cell(root, 1, 1, 60.0f, 20.0f);
}

static void place(engage_case *made, tb_widget widget)
{
    TB_TEST_REQUIRE(tb_grid(widget, &(tb_grid_options){.row = 1, .column = 0}));
    gui_update(&made->fixture);
}

/* Arrive at a widget from the box above it, by navigation: unengaged. */
static void arrive(engage_case *made, tb_widget widget)
{
    TB_TEST_REQUIRE(tb_widget_focus(made->above));
    nav(&made->fixture, TB_GUI_NAV_DOWN);
    TB_TEST_REQUIRE(tb_widget_equal(focused(&made->fixture), widget));
    TB_TEST_EXPECT(!tb_widget_engaged(widget));
}

TB_TEST(gui_navigation, an_entry_is_engaged_by_accept_or_typing_and_let_go_by_back)
{
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget entry = tb_entry(tb_gui_root(fixture->gui), &(tb_entry_desc){.text = "abc"});
    place(&made, entry);
    size_t caret = 0;
    size_t anchor = 0;

    /* Unengaged, its arrows move the focus on. */
    arrive(&made, entry);
    TB_TEST_EXPECT((tb_widget_state(entry) & TB_WIDGET_ENGAGED) == 0);
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.beside));
    /* Accept engages it: the arrows are its caret's. */
    arrive(&made, entry);
    gui_key(fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT(tb_widget_engaged(entry));
    TB_TEST_EXPECT((tb_widget_state(entry) & TB_WIDGET_ENGAGED) != 0);
    TB_TEST_REQUIRE(tb_widget_select(entry, 0, 0));
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), entry));
    TB_TEST_REQUIRE(tb_widget_selection(entry, &caret, &anchor));
    TB_TEST_EXPECT_UINT(caret, ==, 1);
    /* A gamepad's Left too. */
    nav(fixture, TB_GUI_NAV_LEFT);
    TB_TEST_REQUIRE(tb_widget_selection(entry, &caret, &anchor));
    TB_TEST_EXPECT_UINT(caret, ==, 0);
    /* Up and Down are never an entry's: they move on even engaged. */
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));
    /* Back lets it go, and the focus stays. */
    arrive(&made, entry);
    TB_TEST_REQUIRE(tb_widget_engage(entry, true));
    gui_key(fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), entry));
    TB_TEST_EXPECT(!tb_widget_engaged(entry));
    /* Typing engages it. */
    TB_TEST_REQUIRE(tb_widget_select(entry, 0, 0));
    gui_type(fixture, "x");
    TB_TEST_EXPECT(tb_widget_engaged(entry));
    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(entry), TB_TEXT("xabc")));
    /* Keys it uses that are not navigation engage it too. */
    TB_TEST_REQUIRE(tb_widget_engage(entry, false));
    gui_key(fixture, TB_KEY_END, 0);
    TB_TEST_EXPECT(tb_widget_engaged(entry));
    /* A click engages it, and so does Tab, from the widget before it. */
    gui_click(fixture, gui_middle(tb_widget_rect(made.beside)));
    gui_click(fixture, gui_middle(tb_widget_rect(entry)));
    TB_TEST_EXPECT(tb_widget_engaged(entry));
    TB_TEST_REQUIRE(tb_widget_focus(made.beside));
    gui_key(fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), entry));
    TB_TEST_EXPECT(tb_widget_engaged(entry));
    /* Engagement is the focused widget's alone. */
    TB_TEST_EXPECT(!tb_widget_engaged(made.above));
    TB_TEST_EXPECT(!tb_widget_engage(made.above, true));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_text_box_has_every_arrow_while_engaged)
{
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget box = tb_text_box(tb_gui_root(fixture->gui),
                                      &(tb_text_box_desc){.text = "one\ntwo", .height = 2});
    place(&made, box);
    size_t caret = 0;
    size_t anchor = 0;

    arrive(&made, box);
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));
    arrive(&made, box);
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(tb_widget_engaged(box));
    TB_TEST_REQUIRE(tb_widget_select(box, 0, 0));
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), box));
    TB_TEST_REQUIRE(tb_widget_selection(box, &caret, &anchor));
    TB_TEST_EXPECT_UINT(caret, ==, 4);
    /* Enter is its line break while engaged, not Accept. */
    gui_key(fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(box), TB_TEXT("one\n\ntwo")));
    nav(fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!tb_widget_engaged(box));
    gui_key(fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.above));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_slider_keeps_the_arrows_along_it_and_leaves_those_across)
{
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget across =
        tb_scale(tb_gui_root(fixture->gui), &(tb_scale_desc){.from = 0, .to = 10, .resolution = 1});
    place(&made, across);
    tb_gui_value value = {0};

    arrive(&made, across);
    nav(fixture, TB_GUI_NAV_RIGHT);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), across));
    TB_TEST_REQUIRE(tb_widget_get(across, "value", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 1.0f);
    gui_key(fixture, TB_KEY_LEFT, 0);
    TB_TEST_REQUIRE(tb_widget_get(across, "value", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 0.0f);
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));

    /* Upright, it keeps Up and Down; Left and Right move on. */
    tb_widget_destroy(across);
    const tb_widget upright = tb_scale(
        tb_gui_root(fixture->gui),
        &(tb_scale_desc){.from = 0, .to = 10, .resolution = 1, .orient = TB_GUI_ORIENT_VERTICAL});
    place(&made, upright);
    arrive(&made, upright);
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), upright));
    TB_TEST_REQUIRE(tb_widget_get(upright, "value", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 1.0f);
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.beside));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_spin_box_steps_only_while_engaged)
{
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget spin =
        tb_spinbox(tb_gui_root(fixture->gui), &(tb_spinbox_desc){.from = 0, .to = 10, .value = 5});
    place(&made, spin);
    tb_gui_value value = {0};

    arrive(&made, spin);
    nav(fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.above));
    arrive(&made, spin);
    nav(fixture, TB_GUI_NAV_ACCEPT);
    nav(fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), spin));
    TB_TEST_REQUIRE(tb_widget_get(spin, "value", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 6.0f);
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_list_is_entered_with_accept_and_left_with_back)
{
    static const char *const items[] = {"one", "two", "three"};
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget list =
        tb_listbox(tb_gui_root(fixture->gui), &(tb_listbox_desc){.items = items, .item_count = 3});
    place(&made, list);

    arrive(&made, list);
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));
    arrive(&made, list);
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(tb_widget_engaged(list));
    /* Accept engages it, and chooses nothing yet. */
    TB_TEST_EXPECT(!tb_listbox_selected(list, 0));
    nav(fixture, TB_GUI_NAV_DOWN);
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), list));
    TB_TEST_EXPECT_UINT(tb_listbox_active(list), ==, 2);
    nav(fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!tb_widget_engaged(list));
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), list));
    nav(fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.above));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_tree_is_entered_with_accept_and_left_with_back)
{
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget tree = tb_treeview(tb_gui_root(fixture->gui), &(tb_treeview_desc){.height = 3});
    const tb_tree_item first =
        tb_treeview_insert(tree, TB_TREE_ROOT, SIZE_MAX, &(tb_tree_item_desc){.text = "a"});
    const tb_tree_item second =
        tb_treeview_insert(tree, TB_TREE_ROOT, SIZE_MAX, &(tb_tree_item_desc){.text = "b"});
    place(&made, tree);
    TB_TEST_REQUIRE(tb_treeview_set_focus_item(tree, first));

    arrive(&made, tree);
    gui_key(fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.beside));
    arrive(&made, tree);
    gui_key(fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(tb_widget_engaged(tree));
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(tree), ==, second);
    gui_key(fixture, TB_KEY_ESCAPE, 0);
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_combo_box_opens_on_accept_and_its_list_has_the_keys)
{
    static const char *const values[] = {"low", "medium", "high"};
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget combo = tb_combobox(tb_gui_root(fixture->gui),
                                        &(tb_combobox_desc){.values = values, .value_count = 3});
    place(&made, combo);
    tb_gui_value value = {0};

    /* Closed and unengaged, its arrows move on, and choose nothing. */
    arrive(&made, combo);
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));
    TB_TEST_REQUIRE(tb_widget_get(combo, "current", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 0);
    /* Accept opens it; the list has the arrows, Accept chooses. */
    arrive(&made, combo);
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(tb_combobox_open(combo));
    nav(fixture, TB_GUI_NAV_DOWN);
    nav(fixture, TB_GUI_NAV_DOWN);
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_REQUIRE(tb_widget_get(combo, "current", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 2);
    /* Back closes it, choosing nothing. */
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(tb_combobox_open(combo));
    nav(fixture, TB_GUI_NAV_UP);
    nav(fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), combo));
    TB_TEST_REQUIRE(tb_widget_get(combo, "current", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 2);
    /* Focused by a click, Up and Down choose, as a desktop's do. */
    gui_click(fixture, gui_middle(tb_widget_rect(combo)));
    gui_click(fixture, gui_middle(tb_widget_rect(combo)));
    TB_TEST_EXPECT(!tb_combobox_open(combo));
    TB_TEST_EXPECT(tb_widget_engaged(combo));
    gui_key(fixture, TB_KEY_UP, 0);
    TB_TEST_REQUIRE(tb_widget_get(combo, "current", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 1);
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, check_and_radio_buttons_take_accept_and_a_group_lets_go_at_its_ends)
{
    engage_case made = {0};
    open_engage(&made);
    gui_fixture *const fixture = &made.fixture;
    const tb_widget group = tb_frame(tb_gui_root(fixture->gui), NULL);
    place(&made, group);
    const tb_var choice = tb_var_int(fixture->gui, 0);
    const tb_widget first = tb_radiobutton(
        group, &(tb_radiobutton_desc){.text = "A", .variable = choice, .value = "0"});
    const tb_widget second = tb_radiobutton(
        group, &(tb_radiobutton_desc){.text = "B", .variable = choice, .value = "1"});
    TB_TEST_REQUIRE(tb_grid(first, &(tb_grid_options){.row = 0}));
    TB_TEST_REQUIRE(tb_grid(second, &(tb_grid_options){.row = 1}));
    gui_update(fixture);

    arrive(&made, first);
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), second));
    TB_TEST_EXPECT_INT(tb_var_get_int(choice), ==, 1);
    /* Past the group's last, the arrow moves on. */
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.below));
    TB_TEST_EXPECT_INT(tb_var_get_int(choice), ==, 1);
    TB_TEST_REQUIRE(tb_widget_focus(first));
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(tb_var_get_int(choice), ==, 0);

    const tb_var ticked = tb_var_bool(fixture->gui, false);
    const tb_widget check = tb_checkbutton(tb_gui_root(fixture->gui),
                                           &(tb_checkbutton_desc){.text = "C", .variable = ticked});
    TB_TEST_REQUIRE(tb_grid(check, &(tb_grid_options){.row = 3}));
    gui_update(fixture);
    TB_TEST_REQUIRE(tb_widget_focus(check));
    nav(fixture, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(tb_var_get_bool(ticked));
    gui_key(fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT(!tb_var_get_bool(ticked));
    gui_fixture_close(fixture);
}

TB_TEST(gui_navigation, a_notebook_takes_its_tabs_by_the_shoulders_and_its_page_by_down)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget book = tb_notebook(tb_gui_root(fixture.gui), NULL);
    TB_TEST_REQUIRE(tb_grid(book, NULL));
    tb_widget pages[2] = {0};
    tb_widget fields[2] = {0};
    for (size_t index = 0; index < 2; index++)
    {
        pages[index] = tb_frame(book, NULL);
        TB_TEST_REQUIRE_UINT(tb_notebook_add(book, pages[index], index == 0 ? "One" : "Two"), ==,
                             index);
        fields[index] = cell(pages[index], 0, 0, 80.0f, 20.0f);
    }
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(fields[0]));
    /* The right shoulder from inside a page: the next tab, and the focus
     * on the tabs. */
    nav(&fixture, TB_GUI_NAV_TAB_NEXT);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(book), ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), book));
    /* Down from the tabs goes into the page shown; Up comes back. */
    nav(&fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), fields[1]));
    nav(&fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), book));
    /* On the tabs, Left and Right go from tab to tab. */
    nav(&fixture, TB_GUI_NAV_LEFT);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(book), ==, 0);
    /* Control+Tab and the left shoulder too. */
    gui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(book), ==, 1);
    nav(&fixture, TB_GUI_NAV_TAB_PREVIOUS);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(book), ==, 0);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Accept and Back.
 * ------------------------------------------------------------------------- */

static void count(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    (void)event;
    (*calls)++;
}

static void count_and_accept(const tb_gui_event *event, void *user)
{
    count(event, user);
    tb_gui_accept_event(event);
}

TB_TEST(gui_navigation, accept_invokes_and_back_bubbles_up_to_be_bound)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget menu = tb_frame(tb_gui_root(fixture.gui), NULL);
    TB_TEST_REQUIRE(tb_grid(menu, NULL));
    int pressed = 0;
    int backs = 0;
    int root_backs = 0;
    const tb_widget button =
        tb_button(menu, &(tb_button_desc){.text = "Play", .command = count, .user = &pressed});
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_bind(menu, TB_GUI_EVENT_BACK, count, &backs));
    TB_TEST_REQUIRE(tb_bind(tb_gui_root(fixture.gui), TB_GUI_EVENT_BACK, count, &root_backs));

    TB_TEST_REQUIRE(tb_widget_focus(button));
    nav(&fixture, TB_GUI_NAV_ACCEPT);
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_INT(pressed, ==, 2);
    /* Back that nothing uses is sent up the tree from the focus. */
    nav(&fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT_INT(backs, ==, 1);
    TB_TEST_EXPECT_INT(root_backs, ==, 1);
    /* A binding that accepts it stops it. */
    TB_TEST_EXPECT_UINT(tb_unbind(menu, TB_GUI_EVENT_BACK, count, &backs), ==, 1);
    TB_TEST_REQUIRE(tb_bind(menu, TB_GUI_EVENT_BACK, count_and_accept, &backs));
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT_INT(backs, ==, 2);
    TB_TEST_EXPECT_INT(root_backs, ==, 1);
    /* With nothing focused, it goes to the root. */
    tb_gui_clear_focus(fixture.gui);
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT_INT(root_backs, ==, 2);
    /* A key accepted by a binding is not taken as navigation. */
    TB_TEST_REQUIRE(tb_widget_focus(button));
    TB_TEST_REQUIRE(tb_bind(button, TB_GUI_EVENT_KEY, count_and_accept, &backs));
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_INT(pressed, ==, 2);
    TB_TEST_EXPECT_INT(backs, ==, 3);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, back_closes_the_popup_in_front_then_the_modal_widget)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget opener = cell(root, 0, 0, 60.0f, 20.0f);
    const tb_widget dialog = tb_frame(root, &(tb_frame_desc){.background = true});
    TB_TEST_REQUIRE(tb_grid(dialog, &(tb_grid_options){.row = 1}));
    const tb_widget inside = cell(dialog, 0, 0, 60.0f, 20.0f);
    const tb_widget popup = gui_box(inside, 40.0f, 40.0f);
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(opener));
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    TB_TEST_REQUIRE(tb_widget_focus(inside));
    TB_TEST_REQUIRE(
        tb_widget_popup(popup, &(tb_gui_popup_options){.anchor = tb_widget_rect(inside)}));
    gui_update(&fixture);
    nav(&fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!tb_widget_is_popup(popup));
    TB_TEST_EXPECT(tb_widget_visible(dialog));
    nav(&fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!tb_widget_visible(dialog));
    /* The focus goes back to where it was before the modal widget. */
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), opener));
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Modal widgets and popups.
 * ------------------------------------------------------------------------- */

static void note_answer(tb_message_result result, void *user)
{
    tb_message_result *const answer = user;
    *answer = result;
}

TB_TEST(gui_navigation, a_modal_widget_traps_the_focus_and_gives_it_back)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget opener = cell(root, 0, 0, 60.0f, 20.0f);
    const tb_widget outside = cell(root, 2, 0, 60.0f, 20.0f);
    const tb_widget dialog = tb_frame(root, &(tb_frame_desc){.background = true});
    TB_TEST_REQUIRE(tb_grid(dialog, &(tb_grid_options){.row = 1}));
    const tb_widget first = cell(dialog, 0, 0, 60.0f, 20.0f);
    const tb_widget second = cell(dialog, 1, 0, 60.0f, 20.0f);
    const tb_gui_value yes = tb_gui_bool(true);
    TB_TEST_REQUIRE(tb_widget_set(second, "default_focus", &yes));
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(opener));
    /* Shown, it gives its default the focus. */
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), second));
    /* The arrows stay inside it: the widget below it is not found. */
    nav(&fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), second));
    nav(&fixture, TB_GUI_NAV_UP);
    nav(&fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), first));
    tb_widget_end_modal(dialog);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), opener));
    /* Ending one that is not modal changes nothing. */
    tb_widget_end_modal(dialog);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), opener));

    /* Destroyed with the focus in it, it gives the focus back too. */
    TB_TEST_REQUIRE(tb_widget_focus(outside));
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    gui_update(&fixture);
    tb_widget_destroy(dialog);
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), outside));

    /* A message box answers Back as its Escape, and gives the focus back. */
    tb_message_result answer = TB_MESSAGE_RESULT_OK;
    TB_TEST_REQUIRE(tb_widget_focus(opener));
    (void)tb_gui_message_box(fixture.gui, "Quit", "Quit the game?", TB_MESSAGE_YES_NO, note_answer,
                             &answer);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_widget_equal(focused(&fixture), opener));
    nav(&fixture, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT_INT(answer, ==, TB_MESSAGE_RESULT_NO);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), opener));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, a_popup_closed_with_the_focus_in_it_gives_it_back)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget opener = cell(root, 0, 0, 60.0f, 20.0f);
    const tb_widget popup = tb_frame(opener, &(tb_frame_desc){.background = true});
    const tb_widget field = gui_box_of(popup, gui_typing_box_class(), 50.0f, 20.0f);
    TB_TEST_REQUIRE(tb_grid(field, NULL));
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(opener));
    TB_TEST_REQUIRE(
        tb_widget_popup(popup, &(tb_gui_popup_options){.anchor = tb_widget_rect(opener)}));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_widget_focus(field));
    tb_widget_close_popup(popup);
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), opener));
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Scrolling, the focus shown, the key map, and the order of actions.
 * ------------------------------------------------------------------------- */

TB_TEST(gui_navigation, a_scroll_frame_scrolls_the_focus_into_view)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_scrollframe(
        tb_gui_root(fixture.gui), &(tb_scrollframe_desc){.height = 100.0f, .no_xscroll = true});
    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    tb_widget rows[10] = {0};
    for (int32_t row = 0; row < 10; row++)
    {
        rows[row] = cell(frame, row, 0, 80.0f, 22.0f);
    }
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_widget_focus(rows[0]));
    for (size_t step = 0; step < 9; step++)
    {
        nav(&fixture, TB_GUI_NAV_DOWN);
    }
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), rows[9]));
    TB_TEST_EXPECT(tb_widget_scroll_offset(frame).y > 0.0f);
    const tb_canvas_rect seen = tb_widget_rect(frame);
    const tb_canvas_rect last = tb_widget_rect(rows[9]);
    TB_TEST_EXPECT(last.y + last.height <= seen.y + seen.height + 0.5f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, the_focus_is_shown_after_the_keys_and_hidden_after_the_mouse)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget first = cell(root, 0, 0, 60.0f, 20.0f);
    const tb_widget second = cell(root, 1, 0, 60.0f, 20.0f);
    const tb_widget field = gui_box_of(root, gui_typing_box_class(), 60.0f, 20.0f);
    TB_TEST_REQUIRE(tb_grid(field, &(tb_grid_options){.row = 2}));
    gui_update(&fixture);

    TB_TEST_EXPECT(!tb_gui_focus_visible(fixture.gui));
    TB_TEST_REQUIRE(tb_widget_focus(first));
    TB_TEST_EXPECT((tb_widget_state(first) & TB_WIDGET_FOCUSED) != 0);
    TB_TEST_EXPECT((tb_widget_state(first) & TB_WIDGET_FOCUS_VISIBLE) == 0);
    gui_key(&fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_gui_focus_visible(fixture.gui));
    TB_TEST_EXPECT((tb_widget_state(second) & TB_WIDGET_FOCUS_VISIBLE) != 0);
    TB_TEST_EXPECT((tb_widget_state(first) & TB_WIDGET_FOCUS_VISIBLE) == 0);
    /* A click hides it, on the widget it focuses too. */
    gui_click(&fixture, gui_middle(tb_widget_rect(field)));
    TB_TEST_EXPECT(tb_widget_equal(focused(&fixture), field));
    TB_TEST_EXPECT(!tb_gui_focus_visible(fixture.gui));
    TB_TEST_EXPECT((tb_widget_state(field) & TB_WIDGET_FOCUS_VISIBLE) == 0);
    /* Tab and a gamepad show it; a key not mapped does not. */
    gui_key(&fixture, TB_KEY_A, 0);
    TB_TEST_EXPECT(!tb_gui_focus_visible(fixture.gui));
    gui_key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT(tb_gui_focus_visible(fixture.gui));
    tb_gui_set_focus_visible(fixture.gui, false);
    TB_TEST_EXPECT(!tb_gui_focus_visible(fixture.gui));
    nav(&fixture, TB_GUI_NAV_UP);
    TB_TEST_EXPECT(tb_gui_focus_visible(fixture.gui));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, the_focus_ring_is_drawn_only_while_shown)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget button = tb_button(tb_gui_root(fixture.gui), &(tb_button_desc){.text = "Go"});
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    gui_update(&fixture);
    size_t unfocused = 0;
    size_t hidden = 0;
    size_t shown = 0;

    (void)tb_gui_commands(fixture.gui, &unfocused);
    TB_TEST_REQUIRE(tb_widget_focus(button));
    gui_update(&fixture);
    (void)tb_gui_commands(fixture.gui, &hidden);
    tb_gui_set_focus_visible(fixture.gui, true);
    gui_update(&fixture);
    (void)tb_gui_commands(fixture.gui, &shown);
    TB_TEST_EXPECT_UINT(hidden, ==, unfocused);
    TB_TEST_EXPECT_UINT(shown, >, hidden);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_navigation, keys_are_rebound_to_actions_as_data)
{
    grid_case made = {0};
    open_grid(&made);
    gui_fixture *const fixture = &made.fixture;
    tb_gui *const gui = fixture->gui;

    TB_TEST_EXPECT_INT(tb_gui_key_action(gui, TB_KEY_UP, 0), ==, TB_GUI_NAV_UP);
    TB_TEST_EXPECT_INT(tb_gui_key_action(gui, TB_KEY_ESCAPE, 0), ==, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT_INT(tb_gui_key_action(gui, TB_KEY_TAB, TB_MODIFIER_CONTROL), ==,
                       TB_GUI_NAV_TAB_NEXT);
    /* A key with other modifiers held is another key. */
    TB_TEST_EXPECT_INT(tb_gui_key_action(gui, TB_KEY_UP, TB_MODIFIER_SHIFT), ==, TB_GUI_NAV_NONE);

    /* W, A, S, D move; the arrows no longer do. */
    TB_TEST_REQUIRE(tb_gui_map_key(gui, TB_KEY_S, 0, TB_GUI_NAV_DOWN));
    TB_TEST_REQUIRE(tb_gui_map_key(gui, TB_KEY_D, 0, TB_GUI_NAV_RIGHT));
    TB_TEST_REQUIRE(tb_gui_map_key(gui, TB_KEY_DOWN, 0, TB_GUI_NAV_NONE));
    TB_TEST_REQUIRE(tb_gui_map_key(gui, TB_KEY_S, 0, TB_GUI_NAV_DOWN));
    TB_TEST_REQUIRE(tb_gui_map_key(gui, TB_KEY_Q, 0, TB_GUI_NAV_NONE));
    TB_TEST_REQUIRE(tb_widget_focus(made.boxes[0][0]));
    gui_key(fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][0]));
    gui_key(fixture, TB_KEY_S, 0);
    gui_key(fixture, TB_KEY_D, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][1]));
    /* Out of range is refused. */
    TB_TEST_EXPECT(!tb_gui_map_key(gui, TB_KEY_COUNT, 0, TB_GUI_NAV_UP));
    TB_TEST_EXPECT(!tb_gui_map_key(gui, TB_KEY_UNKNOWN, 0, TB_GUI_NAV_UP));
    TB_TEST_EXPECT(!tb_gui_map_key(gui, TB_KEY_S, 0, TB_GUI_NAV_COUNT));
    TB_TEST_EXPECT(!tb_gui_map_key(gui, TB_KEY_S, 1u << 7, TB_GUI_NAV_UP));

    /* Cleared, no key navigates; actions fed still do. */
    tb_gui_clear_key_map(gui);
    TB_TEST_EXPECT_INT(tb_gui_key_action(gui, TB_KEY_S, 0), ==, TB_GUI_NAV_NONE);
    gui_key(fixture, TB_KEY_S, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][1]));
    nav(fixture, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[2][1]));
    /* Reset, the arrows are back. */
    tb_gui_reset_key_map(gui);
    gui_key(fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][1]));
    /* Actions out of range are ignored. */
    tb_gui_feed_navigation(gui, TB_GUI_NAV_NONE);
    tb_gui_feed_navigation(gui, TB_GUI_NAV_COUNT);
    gui_update(fixture);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][1]));
    gui_fixture_close(fixture);
}

/* A key binding that feeds an action, which waits for the next update. */
static void feed_down(const tb_gui_event *event, void *user)
{
    (void)user;
    tb_gui_feed_navigation(event->widget.gui, TB_GUI_NAV_DOWN);
}

TB_TEST(gui_navigation, actions_are_taken_in_their_order_among_the_events)
{
    grid_case made = {0};
    open_grid(&made);
    gui_fixture *const fixture = &made.fixture;
    tb_gui *const gui = fixture->gui;
    const tb_window_event right = {.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_RIGHT};

    TB_TEST_REQUIRE(tb_widget_focus(made.boxes[0][0]));
    /* Down, then Right, then Down: the bottom row's middle... */
    tb_gui_feed_navigation(gui, TB_GUI_NAV_DOWN);
    tb_gui_feed_event(gui, &right);
    tb_gui_feed_navigation(gui, TB_GUI_NAV_DOWN);
    gui_update(fixture);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[2][1]));
    /* ... and an action fed by a callback waits for the next update. */
    TB_TEST_REQUIRE(tb_widget_focus(made.boxes[0][0]));
    TB_TEST_REQUIRE(tb_bind(made.boxes[0][1], TB_GUI_EVENT_FOCUS_IN, feed_down, NULL));
    tb_gui_feed_event(gui, &right);
    gui_update(fixture);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[0][1]));
    gui_update(fixture);
    TB_TEST_EXPECT(tb_widget_equal(focused(fixture), made.boxes[1][1]));
    gui_fixture_close(fixture);
}
