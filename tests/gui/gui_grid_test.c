/* The grid, after Tk's own grid tests: rows and columns sized by what
 * their widgets want, extra space by weight or placed by the anchor,
 * minimum sizes, pads, uniform groups, spans, sticky edges, padding,
 * shrinking, edges on whole pixels at any scale, and layout only when
 * something changed. Boxes of chosen sizes stand for widgets. */
#include "gui_fixture.h"
#include "tb_test.h"

#include <math.h>

/* A rectangle's four numbers, each as expected. */
static void expect_rect(tb_canvas_rect rect, float x, float y, float width, float height)
{
    TB_TEST_EXPECT_FLOAT_NEAR(rect.x, x, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.y, y, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.width, width, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.height, height, 1e-3f);
}

static tb_canvas_rect column_of(tb_widget container, size_t column)
{
    tb_canvas_rect rect = {0};
    TB_TEST_REQUIRE(tb_grid_bbox(container, column, 0, column, 0, &rect));
    return rect;
}

static void place(tb_widget widget, int32_t row, int32_t column, uint32_t sticky)
{
    TB_TEST_REQUIRE(
        tb_grid(widget, &(tb_grid_options){.row = row, .column = column, .sticky = sticky}));
}

TB_TEST(gui_grid, rows_and_columns_are_as_large_as_their_largest_widget)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget a = gui_box(frame, 30.0f, 10.0f);
    const tb_widget b = gui_box(frame, 50.0f, 20.0f);
    const tb_widget c = gui_box(frame, 20.0f, 40.0f);
    tb_canvas_rect bbox = {0};
    size_t columns = 0;
    size_t rows = 0;

    place(frame, 0, 0, 0);
    place(a, 0, 0, 0);
    place(b, 0, 1, 0);
    place(c, 1, 0, 0);
    gui_update(&fixture);

    const tb_vec2 natural = tb_widget_natural_size(frame);
    TB_TEST_EXPECT_FLOAT(natural.x, ==, 80.0f);
    TB_TEST_EXPECT_FLOAT(natural.y, ==, 60.0f);
    expect_rect(tb_widget_rect(frame), 0.0f, 0.0f, 80.0f, 60.0f);
    /* Each in the middle of its cell. */
    expect_rect(tb_widget_rect(a), 0.0f, 5.0f, 30.0f, 10.0f);
    expect_rect(tb_widget_rect(b), 30.0f, 0.0f, 50.0f, 20.0f);
    expect_rect(tb_widget_rect(c), 5.0f, 20.0f, 20.0f, 40.0f);
    tb_grid_size(frame, &columns, &rows);
    TB_TEST_EXPECT_UINT(columns, ==, 2);
    TB_TEST_EXPECT_UINT(rows, ==, 2);
    TB_TEST_REQUIRE(tb_grid_bbox(frame, 1, 1, 0, 0, &bbox));
    expect_rect(bbox, 0.0f, 0.0f, 80.0f, 60.0f);
    TB_TEST_EXPECT(!tb_grid_bbox(frame, 0, 0, 2, 0, &bbox));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, extra_space_is_shared_by_weight)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget a = gui_box(root, 100.0f, 10.0f);
    const tb_widget b = gui_box(root, 100.0f, 10.0f);

    place(a, 0, 0, TB_STICKY_EW);
    place(b, 0, 1, TB_STICKY_EW);
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 1, &(tb_grid_track){.weight = 3.0f}));
    gui_update(&fixture);

    /* 600 units to share: 150 and 450. Rows have no weight: the row is at the top. */
    expect_rect(tb_widget_rect(a), 0.0f, 0.0f, 250.0f, 10.0f);
    expect_rect(tb_widget_rect(b), 250.0f, 0.0f, 550.0f, 10.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, without_weights_the_grid_is_placed_by_its_anchor)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget box = gui_box(root, 100.0f, 50.0f);

    place(box, 0, 0, 0);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 0.0f, 0.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_CENTER);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 350.0f, 275.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_SE);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 700.0f, 550.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_N);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 350.0f, 0.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_E);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 700.0f, 275.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_SW);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 0.0f, 550.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_W);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 0.0f, 275.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_NE);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 700.0f, 0.0f, 100.0f, 50.0f);
    tb_grid_anchor(root, TB_GUI_ANCHOR_S);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(box), 350.0f, 550.0f, 100.0f, 50.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, minimum_sizes_and_pads_widen_rows_and_columns)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget first = gui_box(root, 100.0f, 10.0f);
    const tb_widget last = gui_box(root, 50.0f, 10.0f);
    size_t columns = 0;
    size_t rows = 0;

    place(first, 0, 0, 0);
    place(last, 0, 2, 0);
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.minsize = 120.0f}));
    /* An empty column takes its minimum size, and no pad: it has no widget. */
    TB_TEST_REQUIRE(
        tb_grid_column_configure(root, 1, &(tb_grid_track){.minsize = 30.0f, .pad = 10.0f}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 2, &(tb_grid_track){.pad = 10.0f}));
    /* A row configured past the widgets exists, empty. */
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 3, &(tb_grid_track){.minsize = 5.0f}));
    gui_update(&fixture);

    expect_rect(column_of(root, 0), 0.0f, 0.0f, 120.0f, 10.0f);
    expect_rect(column_of(root, 1), 120.0f, 0.0f, 30.0f, 10.0f);
    expect_rect(column_of(root, 2), 150.0f, 0.0f, 60.0f, 10.0f);
    /* Half the pad on each side. */
    expect_rect(tb_widget_rect(last), 155.0f, 0.0f, 50.0f, 10.0f);
    expect_rect(tb_widget_rect(first), 10.0f, 0.0f, 100.0f, 10.0f);
    tb_grid_size(root, &columns, &rows);
    TB_TEST_EXPECT_UINT(columns, ==, 3);
    TB_TEST_EXPECT_UINT(rows, ==, 4);
    TB_TEST_EXPECT(!tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = -1.0f}));
    TB_TEST_EXPECT(!tb_grid_row_configure(root, 0, &(tb_grid_track){.minsize = -1.0f}));
    TB_TEST_EXPECT(!tb_grid_row_configure(TB_WIDGET_NONE, 0, &(tb_grid_track){0}));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, a_uniform_group_is_sized_in_proportion_to_its_weights)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget narrow = gui_box(frame, 40.0f, 10.0f);
    const tb_widget wide = gui_box(frame, 100.0f, 10.0f);
    const tb_widget alone = gui_box(frame, 10.0f, 10.0f);

    place(frame, 0, 0, 0);
    place(narrow, 0, 0, 0);
    place(wide, 0, 1, 0);
    place(alone, 0, 2, 0);
    /* No weight counts as one: the two are as wide as each other. */
    TB_TEST_REQUIRE(tb_grid_column_configure(frame, 0, &(tb_grid_track){.uniform = "buttons"}));
    TB_TEST_REQUIRE(tb_grid_column_configure(frame, 1, &(tb_grid_track){.uniform = "buttons"}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 0).width, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 1).width, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 2).width, ==, 10.0f);

    /* Weights of 1 and 2: the narrow column is half the wide one, which
     * needs 100. Extra space keeps the proportion. */
    TB_TEST_REQUIRE(
        tb_grid_column_configure(frame, 0, &(tb_grid_track){.weight = 1.0f, .uniform = "buttons"}));
    TB_TEST_REQUIRE(
        tb_grid_column_configure(frame, 1, &(tb_grid_track){.weight = 2.0f, .uniform = "buttons"}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 0).width, ==, 50.0f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 1).width, ==, 100.0f);
    place(frame, 0, 0, TB_STICKY_NSEW);
    TB_TEST_REQUIRE(
        tb_grid_column_configure(tb_gui_root(fixture.gui), 0, &(tb_grid_track){.weight = 1.0f}));
    gui_update(&fixture);
    const float ratio = column_of(frame, 1).width / column_of(frame, 0).width;
    TB_TEST_EXPECT_FLOAT_NEAR(ratio, 2.0f, 0.01f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 2).width, ==, 10.0f);

    /* A group of one other name is its own. */
    TB_TEST_REQUIRE(tb_grid_column_configure(frame, 2, &(tb_grid_track){.uniform = "other"}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 2).width, ==, 10.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, a_widget_spanning_widens_its_tracks_by_weight_or_evenly)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget left = gui_box(frame, 50.0f, 10.0f);
    const tb_widget right = gui_box(frame, 50.0f, 10.0f);
    const tb_widget wide = gui_box(frame, 300.0f, 10.0f);

    place(frame, 0, 0, 0);
    place(left, 0, 0, 0);
    place(right, 0, 1, 0);
    TB_TEST_REQUIRE(tb_grid(
        wide, &(tb_grid_options){.row = 1, .column = 0, .column_span = 2, .sticky = TB_STICKY_EW}));
    gui_update(&fixture);
    /* No weights: the 200 missing is shared evenly. */
    TB_TEST_EXPECT_FLOAT(column_of(frame, 0).width, ==, 150.0f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 1).width, ==, 150.0f);
    expect_rect(tb_widget_rect(wide), 0.0f, 10.0f, 300.0f, 10.0f);

    TB_TEST_REQUIRE(tb_grid_column_configure(frame, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_column_configure(frame, 1, &(tb_grid_track){.weight = 3.0f}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 0).width, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 1).width, ==, 200.0f);

    /* A span that fits widens nothing, and spans are sized after the
     * widgets of one cell, narrower spans first. */
    const tb_widget three = gui_box(frame, 30.0f, 10.0f);
    TB_TEST_REQUIRE(tb_grid(three, &(tb_grid_options){.row = 2, .column = 0, .column_span = 3}));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 0).width, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(column_of(frame, 2).width, ==, 0.0f);
    const tb_widget tall = gui_box(frame, 10.0f, 50.0f);
    TB_TEST_REQUIRE(tb_grid(tall, &(tb_grid_options){.row = 0, .column = 2, .row_span = 2}));
    gui_update(&fixture);
    tb_canvas_rect rows = {0};
    TB_TEST_REQUIRE(tb_grid_bbox(frame, 0, 0, 0, 1, &rows));
    TB_TEST_EXPECT_FLOAT(rows.height, ==, 50.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, sticky_edges_place_or_stretch_a_widget_in_its_cell)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget box = gui_box(root, 40.0f, 20.0f);
    static const struct
    {
        uint32_t sticky;
        tb_canvas_rect rect;
    } cases[] = {
        {0, {30.0f, 20.0f, 40.0f, 20.0f}},
        {TB_STICKY_N, {30.0f, 0.0f, 40.0f, 20.0f}},
        {TB_STICKY_S, {30.0f, 40.0f, 40.0f, 20.0f}},
        {TB_STICKY_E, {60.0f, 20.0f, 40.0f, 20.0f}},
        {TB_STICKY_W, {0.0f, 20.0f, 40.0f, 20.0f}},
        {TB_STICKY_N | TB_STICKY_W, {0.0f, 0.0f, 40.0f, 20.0f}},
        {TB_STICKY_S | TB_STICKY_E, {60.0f, 40.0f, 40.0f, 20.0f}},
        {TB_STICKY_EW, {0.0f, 20.0f, 100.0f, 20.0f}},
        {TB_STICKY_NS, {30.0f, 0.0f, 40.0f, 60.0f}},
        {TB_STICKY_NSEW, {0.0f, 0.0f, 100.0f, 60.0f}},
    };

    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.minsize = 100.0f}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.minsize = 60.0f}));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        TB_TEST_CONTEXT("sticky %u", (unsigned)cases[index].sticky);
        place(box, 0, 0, cases[index].sticky);
        gui_update(&fixture);
        const tb_canvas_rect expected = cases[index].rect;
        expect_rect(tb_widget_rect(box), expected.x, expected.y, expected.width, expected.height);
    }
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, padding_is_outside_a_widget_and_inner_padding_inside_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget box = gui_box(root, 40.0f, 20.0f);
    const tb_widget next = gui_box(root, 10.0f, 10.0f);

    TB_TEST_REQUIRE(tb_grid(
        box, &(tb_grid_options){
                 .padx = {3.0f, 7.0f}, .pady = {2.0f, 8.0f}, .ipadx = 5.0f, .ipady = 4.0f}));
    place(next, 1, 1, TB_STICKY_N | TB_STICKY_W);
    gui_update(&fixture);
    /* The cell is 40 + 2 * 5 + 3 + 7 across and 20 + 2 * 4 + 2 + 8 down;
     * the widget, its inner padding included, sits inside the outer. */
    expect_rect(column_of(root, 0), 0.0f, 0.0f, 60.0f, 38.0f);
    expect_rect(tb_widget_rect(box), 3.0f, 2.0f, 50.0f, 28.0f);
    expect_rect(tb_widget_rect(next), 60.0f, 38.0f, 10.0f, 10.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, a_grid_too_large_gives_back_by_weight_then_is_cut_off)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget a = gui_box(root, 150.0f, 10.0f);
    const tb_widget b = gui_box(root, 150.0f, 10.0f);

    place(a, 0, 0, TB_STICKY_EW);
    place(b, 0, 1, TB_STICKY_EW);
    gui_resize(&fixture, 200, 100, 1.0f);

    /* Two of equal weight give back equally. */
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 1, &(tb_grid_track){.weight = 1.0f}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(a), 0.0f, 0.0f, 100.0f, 10.0f);
    expect_rect(tb_widget_rect(b), 100.0f, 0.0f, 100.0f, 10.0f);

    /* One of weight gives back down to its minimum size; the rest is cut off
     * at the right. */
    TB_TEST_REQUIRE(
        tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f, .minsize = 60.0f}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 1, &(tb_grid_track){0}));
    gui_update(&fixture);
    expect_rect(tb_widget_rect(a), 0.0f, 0.0f, 60.0f, 10.0f);
    expect_rect(tb_widget_rect(b), 60.0f, 0.0f, 150.0f, 10.0f);

    /* With no weight at all, nothing gives back: it starts at the anchor's
     * edge and is cut off. */
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){0}));
    tb_grid_anchor(root, TB_GUI_ANCHOR_CENTER);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(a), 0.0f, 45.0f, 150.0f, 10.0f);
    expect_rect(tb_widget_rect(b), 150.0f, 45.0f, 150.0f, 10.0f);
    gui_fixture_close(&fixture);
}

/* Columns of equal weight across a width no scale divides evenly: each edge
 * is on a whole pixel, and neighbors meet with no gap and no overlap. */
static void expect_whole_pixel_edges(float scale)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, scale);
    const tb_widget root = tb_gui_root(fixture.gui);
    tb_widget boxes[7] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(boxes); index++)
    {
        boxes[index] = gui_box(root, 10.0f, 10.0f);
        TB_TEST_REQUIRE(tb_grid(boxes[index], &(tb_grid_options){.column = (int32_t)index,
                                                                 .sticky = TB_STICKY_NSEW,
                                                                 .padx = {0.3f, 0.0f}}));
        TB_TEST_REQUIRE(tb_grid_column_configure(root, index, &(tb_grid_track){.weight = 1.0f}));
    }
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    gui_update(&fixture);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(boxes); index++)
    {
        const tb_canvas_rect cell = column_of(root, index);
        const tb_canvas_rect rect = tb_widget_rect(boxes[index]);
        const float left = cell.x * scale;
        const float right = (cell.x + cell.width) * scale;
        TB_TEST_CONTEXT("scale %.2f, column %u", (double)scale, (unsigned)index);
        TB_TEST_EXPECT_FLOAT_NEAR(left, roundf(left), 1e-3f);
        TB_TEST_EXPECT_FLOAT_NEAR(right, roundf(right), 1e-3f);
        TB_TEST_EXPECT_FLOAT_NEAR(rect.x * scale, roundf(rect.x * scale), 1e-3f);
        TB_TEST_EXPECT_FLOAT_NEAR((rect.x + rect.width) * scale,
                                  roundf((rect.x + rect.width) * scale), 1e-3f);
        if (index + 1 < TB_TEST_COUNT_OF(boxes))
        {
            TB_TEST_EXPECT_FLOAT_NEAR(cell.x + cell.width, column_of(root, index + 1).x, 1e-4f);
            TB_TEST_EXPECT_FLOAT(rect.x + rect.width, <=, tb_widget_rect(boxes[index + 1]).x);
        }
        TB_TEST_EXPECT_FLOAT_NEAR(rect.x + rect.width, cell.x + cell.width, 1e-4f);
        /* Rounding never strays a pixel from where the edge would be. */
        TB_TEST_EXPECT_FLOAT_NEAR(cell.x, (float)index * GUI_WIDTH / 7.0f, 1.0f / scale);
    }
    const tb_canvas_rect last = column_of(root, TB_TEST_COUNT_OF(boxes) - 1);
    TB_TEST_EXPECT_FLOAT_NEAR(last.x + last.width, GUI_WIDTH, 1e-3f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, edges_land_on_whole_pixels_with_no_gaps_at_any_scale)
{
    expect_whole_pixel_edges(1.0f);
    expect_whole_pixel_edges(1.25f);
    expect_whole_pixel_edges(1.5f);
    expect_whole_pixel_edges(1.75f);
    expect_whole_pixel_edges(2.0f);
}

TB_TEST(gui_grid, the_widgets_are_laid_out_only_when_something_changed)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "A"});
    tb_gui_stats before = {0};
    tb_gui_stats after = {0};

    place(label, 0, 0, 0);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &before);
    gui_update(&fixture);
    gui_resize(&fixture, (uint32_t)GUI_WIDTH, (uint32_t)GUI_HEIGHT, 1.0f);
    gui_move(&fixture, 10.0f, 10.0f);
    tb_gui_stats_of(fixture.gui, &after);
    TB_TEST_EXPECT_UINT(after.layouts, ==, before.layouts);

    const float narrow = tb_widget_natural_size(label).x;
    TB_TEST_REQUIRE(tb_widget_set_text(label, "A longer text"));
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &after);
    TB_TEST_EXPECT_UINT(after.layouts, ==, before.layouts + 1);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(label).x, >, narrow);

    /* A property that changes no size, such as how lines are justified,
     * does not lay out. */
    const tb_gui_value justify = tb_gui_enum(TB_FONT_ALIGN_CENTER);
    TB_TEST_REQUIRE(tb_widget_set(label, "justify", &justify));
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &before);
    TB_TEST_EXPECT_UINT(before.layouts, ==, after.layouts);
    gui_resize(&fixture, 400, 300, 1.0f);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &after);
    TB_TEST_EXPECT_UINT(after.layouts, ==, before.layouts + 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, hidden_and_forgotten_widgets_take_no_space)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget a = gui_box(root, 100.0f, 10.0f);
    const tb_widget b = gui_box(root, 100.0f, 10.0f);
    const tb_widget inner = gui_box(a, 5.0f, 5.0f);
    tb_grid_options options = {0};

    place(a, 0, 0, 0);
    place(b, 0, 1, 0);
    place(inner, 0, 0, 0);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(b).x, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(inner).width, ==, 5.0f);

    tb_widget_set_visible(a, false);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(b).x, ==, 0.0f);
    expect_rect(tb_widget_rect(a), 0.0f, 0.0f, 0.0f, 0.0f);
    /* What is in a hidden widget is hidden with it. */
    expect_rect(tb_widget_rect(inner), 0.0f, 0.0f, 0.0f, 0.0f);

    tb_widget_set_visible(a, true);
    tb_grid_forget(b);
    gui_update(&fixture);
    TB_TEST_EXPECT(!tb_grid_info(b, &options));
    expect_rect(tb_widget_rect(b), 0.0f, 0.0f, 0.0f, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(a).width, ==, 100.0f);
    /* A widget never placed is never shown. */
    const tb_widget unplaced = gui_box(root, 10.0f, 10.0f);
    gui_update(&fixture);
    expect_rect(tb_widget_rect(unplaced), 0.0f, 0.0f, 0.0f, 0.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, grids_nest_inside_padded_frames)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget outer =
        tb_frame(tb_gui_root(fixture.gui), &(tb_frame_desc){.padding = 10.0f, .border = true});
    const tb_widget inner = tb_frame(outer, &(tb_frame_desc){.padding = 5.0f});
    const tb_widget box = gui_box(inner, 50.0f, 30.0f);
    const float border = tb_gui_theme(fixture.gui)->border;

    TB_TEST_REQUIRE(tb_grid(outer, &(tb_grid_options){.padx = {20.0f, 0.0f}}));
    place(inner, 0, 0, 0);
    place(box, 0, 0, 0);
    gui_update(&fixture);
    const float edge = 10.0f + border;
    expect_rect(tb_widget_rect(outer), 20.0f, 0.0f, 60.0f + 2.0f * edge, 40.0f + 2.0f * edge);
    expect_rect(tb_widget_rect(inner), 20.0f + edge, edge, 60.0f, 40.0f);
    expect_rect(tb_widget_rect(box), 25.0f + edge, 5.0f + edge, 50.0f, 30.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, places_the_grid_does_not_take_are_refused)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget box = gui_box(root, 10.0f, 10.0f);
    tb_grid_options options = {0};

    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.row = -1}));
    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.column = -3}));
    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.sticky = 1u << 4}));
    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.padx = {-1.0f, 0.0f}}));
    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.pady = {0.0f, -1.0f}}));
    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.ipadx = -1.0f}));
    TB_TEST_EXPECT(!tb_grid(box, &(tb_grid_options){.row = INT32_MAX, .row_span = 2}));
    TB_TEST_EXPECT(!tb_grid_info(box, &options));
    TB_TEST_EXPECT(tb_grid(box, NULL));
    TB_TEST_REQUIRE(tb_grid_info(box, &options));
    TB_TEST_EXPECT_INT(options.row_span, ==, 1);
    TB_TEST_EXPECT_INT(options.column_span, ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_grid, a_widget_resized_by_a_layout_is_told)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget box = gui_box(root, 10.0f, 10.0f);
    gui_log log = {0};

    TB_TEST_REQUIRE(tb_bind(box, TB_GUI_EVENT_RESIZE, gui_record, &log));
    place(box, 0, 0, TB_STICKY_NSEW);
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_RESIZE, box), ==, 1);
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_RESIZE, box), ==, 1);
    gui_resize(&fixture, 400, 300, 1.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_RESIZE, box), ==, 2);
    expect_rect(tb_widget_rect(box), 0.0f, 0.0f, 400.0f, 300.0f);
    gui_fixture_close(&fixture);
}
