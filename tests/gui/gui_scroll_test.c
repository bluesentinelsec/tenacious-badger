/* Scrolling in the retained GUI: a scroll frame lays its grid out at its
 * natural size and shows it through itself, moved by its offset, clipped
 * to its view, and found by the mouse only there; the wheel and its bars
 * scroll it, widgets are scrolled into view, and a scroll bar alone is
 * dragged, paged, and turned. */
#include "gui_fixture.h"
#include "tb_test.h"

#include "badger/ui.h"

/* The theme's scroll bar width, which the fixture keeps. */
#define BAR 10.0f

static tb_widget scrollframe_at_top_left(gui_fixture *fixture, float width, float height)
{
    const tb_widget frame = tb_scrollframe(
        tb_gui_root(fixture->gui), &(tb_scrollframe_desc){.width = width, .height = height});
    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    return frame;
}

static tb_widget box_at(tb_widget parent, float width, float height, int32_t row)
{
    const tb_widget box = gui_box(parent, width, height);
    TB_TEST_REQUIRE(
        tb_grid(box, &(tb_grid_options){.row = row, .sticky = TB_STICKY_N | TB_STICKY_W}));
    return box;
}

static void expect_rect(tb_canvas_rect rect, float x, float y, float width, float height)
{
    TB_TEST_EXPECT_FLOAT_NEAR(rect.x, x, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.y, y, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.width, width, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.height, height, 1e-3);
}

static tb_gui_stats stats_of(const gui_fixture *fixture)
{
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture->gui, &stats);
    return stats;
}

/* The clip of the first command a widget drew. */
static bool clip_of(const gui_fixture *fixture, tb_widget widget, tb_canvas_rect *clip)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == (tb_ui_id)widget.index + 1u)
        {
            *clip = commands[index].clip;
            return true;
        }
    }
    return false;
}

static void count_wheel(const tb_gui_event *event, void *user)
{
    int *const count = user;

    (void)event;
    (*count)++;
}

TB_TEST(gui_scroll, a_scrollframe_lays_its_grid_out_at_its_natural_size_and_scrolls_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = scrollframe_at_top_left(&fixture, 200.0f, 100.0f);
    const tb_widget box = box_at(frame, 150.0f, 400.0f, 0);

    gui_update(&fixture);
    /* It wants its own size, and a bar beside it, for the grid is taller:
     * the grid does not widen it. */
    expect_rect(tb_widget_rect(frame), 0.0f, 0.0f, 200.0f + BAR, 100.0f);
    expect_rect(tb_widget_rect(box), 0.0f, 0.0f, 150.0f, 400.0f);
    TB_TEST_EXPECT_FLOAT(tb_grid_request(frame).x, ==, 150.0f);
    TB_TEST_EXPECT_FLOAT(tb_grid_request(frame).y, ==, 400.0f);
    TB_TEST_EXPECT_FLOAT(tb_grid_request(TB_WIDGET_NONE).y, ==, 0.0f);

    /* Scrolled, its children move up, and the offset is kept within the
     * content: 400 tall seen through 100. */
    TB_TEST_EXPECT(tb_widget_scroll_to(frame, (tb_vec2){0.0f, 50.0f}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 0.0f, -50.0f, 150.0f, 400.0f);
    TB_TEST_REQUIRE(tb_widget_scroll_to(frame, (tb_vec2){500.0f, 1000.0f}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 300.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).x, ==, 0.0f);
    TB_TEST_REQUIRE(tb_widget_scroll_to(frame, (tb_vec2){-5.0f, -5.0f}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 0.0f);

    /* Kinds that do not scroll have no offset, and refuse one. */
    TB_TEST_EXPECT(!tb_widget_scroll_to(box, (tb_vec2){0.0f, 1.0f}));
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(box).y, ==, 0.0f);
    TB_TEST_EXPECT(!tb_widget_scroll_to(TB_WIDGET_NONE, (tb_vec2){0.0f, 1.0f}));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, children_are_clipped_to_the_view_and_found_by_the_mouse_only_there)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = scrollframe_at_top_left(&fixture, 200.0f, 100.0f);
    const tb_widget box = box_at(frame, 150.0f, 400.0f, 0);
    tb_canvas_rect clip = {0};

    TB_TEST_REQUIRE(tb_widget_scroll_to(frame, (tb_vec2){0.0f, 50.0f}));
    gui_update(&fixture);
    /* Its drawing is clipped to the view, not to its own rectangle. */
    TB_TEST_REQUIRE(clip_of(&fixture, box, &clip));
    expect_rect(clip, 0.0f, 0.0f, 150.0f, 100.0f);

    /* Inside the view, the box is under the mouse, its local point moved
     * by the offset; below the view, where the box goes on unseen, the
     * root is. */
    gui_move(&fixture, 50.0f, 25.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, box));
    gui_move(&fixture, 50.0f, 150.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, tb_gui_root(fixture.gui)));
    /* Over the bar, the scroll frame itself is. */
    gui_move(&fixture, 205.0f, 50.0f);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).hovered, frame));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, the_wheel_scrolls_and_at_the_end_goes_on_to_the_ancestors)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = scrollframe_at_top_left(&fixture, 200.0f, 100.0f);
    (void)box_at(frame, 150.0f, 400.0f, 0);
    int root_wheels = 0;

    TB_TEST_REQUIRE(
        tb_bind(tb_gui_root(fixture.gui), TB_GUI_EVENT_WHEEL, count_wheel, &root_wheels));
    gui_move(&fixture, 50.0f, 50.0f);
    gui_wheel(&fixture, -1.0f);
    const float step = tb_widget_scroll_offset(frame).y;
    TB_TEST_EXPECT_FLOAT(step, >, 0.0f);
    TB_TEST_EXPECT_INT(root_wheels, ==, 0);
    gui_wheel(&fixture, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 0.0f);
    /* At the top already, up goes on to the root. */
    gui_wheel(&fixture, 1.0f);
    TB_TEST_EXPECT_INT(root_wheels, ==, 1);
    for (int notch = 0; notch < 20; notch++)
    {
        gui_wheel(&fixture, -1.0f);
    }
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 300.0f);
    TB_TEST_EXPECT_INT(root_wheels, >, 1);

    /* Shift turns the wheel across, which this grid does not need. */
    const tb_window_event shifted = {.type = TB_WINDOW_EVENT_MOUSE_WHEEL,
                                     .pixel_x = 50.0,
                                     .pixel_y = 50.0,
                                     .delta_y = -1.0,
                                     .modifiers = TB_MODIFIER_SHIFT};
    const int before = root_wheels;
    tb_gui_feed_event(fixture.gui, &shifted);
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(root_wheels, ==, before + 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, its_bars_are_dragged_and_paged)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = scrollframe_at_top_left(&fixture, 200.0f, 100.0f);
    (void)box_at(frame, 500.0f, 400.0f, 0);
    tb_gui_value offset = {0};

    gui_update(&fixture);
    /* Both bars: each takes room from the other's view. */
    expect_rect(tb_widget_rect(frame), 0.0f, 0.0f, 200.0f + BAR, 100.0f + BAR);

    /* The vertical thumb is a quarter of its track, 100 seen of 400; dragged
     * half its travel of 75, it scrolls half of 300. */
    gui_move(&fixture, 205.0f, 5.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&fixture, 205.0f, 5.0f + 37.5f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_scroll_offset(frame).y, 150.0f, 1e-3);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_REQUIRE(tb_widget_get(frame, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.vec2.y, 150.0f, 1e-3);

    /* A press on the track below the thumb pages a view down; above, up. */
    gui_click(&fixture, (tb_vec2){205.0f, 95.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_scroll_offset(frame).y, 250.0f, 1e-3);
    gui_click(&fixture, (tb_vec2){205.0f, 2.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_scroll_offset(frame).y, 150.0f, 1e-3);

    /* The horizontal bar, below: paged to the right. */
    gui_click(&fixture, (tb_vec2){190.0f, 105.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_scroll_offset(frame).x, 200.0f, 1e-3);
    /* A press elsewhere in the frame, or with another button, is not the bars'. */
    gui_move(&fixture, 205.0f, 105.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_release(&fixture, TB_MOUSE_BUTTON_RIGHT);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_scroll_offset(frame).x, 200.0f, 1e-3);
    gui_leave(&fixture);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, see_scrolls_as_little_as_shows_a_widget_and_the_focus_is_seen)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = scrollframe_at_top_left(&fixture, 200.0f, 100.0f);
    tb_widget rows[8] = {0};

    for (int32_t row = 0; row < 8; row++)
    {
        rows[row] = gui_box_of(frame, gui_focus_box_class(), 100.0f, 50.0f);
        TB_TEST_REQUIRE(tb_grid(rows[row], &(tb_grid_options){.row = row}));
    }
    /* Before any update: laid out first. Row 6 spans 300 to 350: its
     * bottom is brought to the view's. */
    TB_TEST_EXPECT(tb_widget_see(rows[6]));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 250.0f);
    /* Shown already: nothing moves. */
    TB_TEST_EXPECT(tb_widget_see(rows[5]));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 250.0f);
    TB_TEST_EXPECT(tb_widget_see(rows[0]));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 0.0f);

    /* The focus given to a widget scrolls it into view. */
    TB_TEST_REQUIRE(tb_widget_focus(rows[7]));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 300.0f);

    /* A stale handle, and a widget not shown, are not seen. */
    tb_grid_forget(rows[3]);
    TB_TEST_EXPECT(!tb_widget_see(rows[3]));
    tb_widget_destroy(rows[3]);
    TB_TEST_EXPECT(!tb_widget_see(rows[3]));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, nested_scrollframes_each_scroll_their_share)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget outer = scrollframe_at_top_left(&fixture, 300.0f, 200.0f);
    const tb_widget spacer = box_at(outer, 100.0f, 300.0f, 0);
    const tb_widget inner = tb_scrollframe(outer, &(tb_scrollframe_desc){.height = 100.0f});
    TB_TEST_REQUIRE(
        tb_grid(inner, &(tb_grid_options){.row = 1, .sticky = TB_STICKY_N | TB_STICKY_W}));
    (void)box_at(inner, 100.0f, 200.0f, 0);
    const tb_widget deep = box_at(inner, 100.0f, 50.0f, 1);

    (void)spacer;
    /* The deep box is 200 down its frame, which is 300 down the outer:
     * the inner frame scrolls 150, the outer 200. */
    TB_TEST_EXPECT(tb_widget_see(deep));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(inner).y, ==, 150.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(outer).y, ==, 200.0f);
    const tb_canvas_rect rect = tb_widget_rect(deep);
    TB_TEST_EXPECT_FLOAT(rect.y, >=, 0.0f);
    TB_TEST_EXPECT_FLOAT(rect.y + rect.height, <=, 200.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, a_scrollframe_s_properties_are_read_and_set_by_name)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_scrollframe(
        tb_gui_root(fixture.gui), &(tb_scrollframe_desc){.no_yscroll = true, .border = true});
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    (void)box_at(frame, 400.0f, 300.0f, 0);
    const tb_gui_value width = tb_gui_float(120.0f);
    const tb_gui_value height = tb_gui_float(80.0f);
    TB_TEST_REQUIRE(tb_widget_set(frame, "width", &width));
    TB_TEST_REQUIRE(tb_widget_set(frame, "height", &height));
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(frame, "width", &negative));
    TB_TEST_EXPECT(!tb_widget_set(frame, "height", &negative));
    TB_TEST_REQUIRE(tb_widget_get(frame, "width", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 120.0f);
    TB_TEST_REQUIRE(tb_widget_get(frame, "height", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 80.0f);
    TB_TEST_REQUIRE(tb_widget_get(frame, "yscroll", &value));
    TB_TEST_EXPECT(!value.boolean);
    TB_TEST_REQUIRE(tb_widget_get(frame, "xscroll", &value));
    TB_TEST_EXPECT(value.boolean);
    gui_update(&fixture);
    /* It does not scroll down: its grid is squeezed into its height, and
     * only the bar across is shown, inside its border. */
    expect_rect(tb_widget_rect(frame), 0.0f, 0.0f, 122.0f, 80.0f + BAR + 2.0f);
    const tb_gui_value offset = tb_gui_vec2((tb_vec2){50.0f, 50.0f});
    TB_TEST_REQUIRE(tb_widget_set(frame, "offset", &offset));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).x, ==, 50.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 0.0f);
    gui_move(&fixture, 50.0f, 40.0f);
    gui_wheel(&fixture, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, ==, 0.0f);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "scrollframe"), ==,
                           tb_scrollframe_class());
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * A scroll bar alone.
 * ------------------------------------------------------------------------- */

static void count_moves(const tb_gui_event *event, void *user)
{
    int *const count = user;

    (void)event;
    (*count)++;
}

TB_TEST(gui_scroll, a_scrollbar_alone_is_dragged_paged_and_turned_and_calls_its_command)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    int moves = 0;
    const tb_widget bar =
        tb_scrollbar(tb_gui_root(fixture.gui),
                     &(tb_scrollbar_desc){
                         .orient = TB_GUI_ORIENT_VERTICAL, .command = count_moves, .user = &moves});
    float first = 0.0f;
    float last = 0.0f;

    TB_TEST_REQUIRE(tb_grid(bar, &(tb_grid_options){.sticky = TB_STICKY_NS}));
    TB_TEST_REQUIRE(
        tb_grid_row_configure(tb_gui_root(fixture.gui), 0, &(tb_grid_track){.minsize = 100.0f}));
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT(first, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(last, ==, 1.0f);
    TB_TEST_REQUIRE(tb_scrollbar_set(bar, 0.0f, 0.25f));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(bar), 0.0f, 0.0f, BAR, 100.0f);

    /* Its thumb is a quarter of 100; dragged half its travel of 75, it
     * shows from 0.375. */
    gui_move(&fixture, 5.0f, 5.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&fixture, 5.0f, 5.0f + 37.5f);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(first, 0.375, 1e-4);
    TB_TEST_EXPECT_FLOAT_NEAR(last, 0.625, 1e-4);
    TB_TEST_EXPECT_INT(moves, ==, 1);

    /* Paged below the thumb, and above. */
    gui_click(&fixture, (tb_vec2){5.0f, 95.0f});
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(first, 0.625, 1e-4);
    gui_click(&fixture, (tb_vec2){5.0f, 2.0f});
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(first, 0.375, 1e-4);
    TB_TEST_EXPECT_INT(moves, ==, 3);

    /* Turned by the wheel a tenth a notch, and kept from its ends. */
    gui_move(&fixture, 5.0f, 50.0f);
    gui_wheel(&fixture, -1.0f);
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(first, 0.475, 1e-4);
    for (int notch = 0; notch < 10; notch++)
    {
        gui_wheel(&fixture, -1.0f);
    }
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(last, 1.0, 1e-4);

    /* Set by hand within 0 and 1, in order, without the command. */
    const int before = moves;
    TB_TEST_EXPECT(!tb_scrollbar_set(bar, 0.5f, 0.25f));
    TB_TEST_EXPECT(!tb_scrollbar_set(bar, -0.5f, 0.25f));
    TB_TEST_EXPECT(!tb_scrollbar_set(bar, 0.5f, 1.5f));
    TB_TEST_EXPECT(!tb_scrollbar_set(tb_gui_root(fixture.gui), 0.0f, 1.0f));
    TB_TEST_EXPECT(!tb_scrollbar_get(tb_gui_root(fixture.gui), &first, &last));
    TB_TEST_EXPECT(tb_scrollbar_set(bar, 0.0f, 1.0f));
    TB_TEST_EXPECT_INT(moves, ==, before);
    const tb_gui_value out = tb_gui_float(2.0f);
    TB_TEST_EXPECT(!tb_widget_set(bar, "first", &out));
    TB_TEST_EXPECT(!tb_widget_set(bar, "last", &out));
    const tb_gui_value half = tb_gui_float(0.5f);
    TB_TEST_EXPECT(tb_widget_set(bar, "last", &half));
    tb_gui_value value = {0};
    TB_TEST_REQUIRE(tb_widget_get(bar, "last", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 0.5f);
    TB_TEST_REQUIRE(tb_widget_get(bar, "first", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 0.0f);
    gui_leave(&fixture);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scroll, a_horizontal_scrollbar_runs_across)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget bar = tb_scrollbar(tb_gui_root(fixture.gui),
                                       &(tb_scrollbar_desc){.orient = TB_GUI_ORIENT_HORIZONTAL});
    float first = 0.0f;
    float last = 0.0f;

    TB_TEST_REQUIRE(tb_grid(bar, NULL));
    TB_TEST_REQUIRE(tb_scrollbar_set(bar, 0.0f, 0.5f));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(bar), 0.0f, 0.0f, 4.0f * BAR, BAR);
    gui_click(&fixture, (tb_vec2){35.0f, 5.0f});
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(first, 0.5, 1e-4);
    /* Its wheel is the wheel across. */
    tb_window_event across = gui_mouse_event(&fixture, TB_WINDOW_EVENT_MOUSE_WHEEL, 5.0f, 5.0f);
    across.delta_x = -1.0;
    tb_gui_feed_event(fixture.gui, &across);
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_scrollbar_get(bar, &first, &last));
    TB_TEST_EXPECT_FLOAT_NEAR(first, 0.4, 1e-4);
    gui_fixture_close(&fixture);
}
