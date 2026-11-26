/* Units and scaling: a GUI fills the viewport the display policy gives a
 * frame (badger/viewport.h): a tool's in logical pixels at the display's
 * scale, a game's in its design units, kept whole, expanded, or by whole
 * pixels; the GUI's own factor on top; the mouse placed in units, and
 * reaching nothing over the bars; a frame resized laid out again; and text
 * measured at the frame's own pixels. */
#include "badger/font_layout.h"
#include "badger/viewport.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <math.h>

static void expect_view(tb_gui_view view, float x, float y, float scale, float width, float height)
{
    TB_TEST_EXPECT_FLOAT_NEAR(view.origin.x, x, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(view.origin.y, y, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(view.scale, scale, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(view.size.x, width, 1e-2f);
    TB_TEST_EXPECT_FLOAT_NEAR(view.size.y, height, 1e-2f);
}

/* Give the fixture's GUI the viewport a policy gives a frame, as an app
 * gives it the renderer's. */
static void place(gui_fixture *fixture, const tb_viewport_policy *policy, uint32_t width,
                  uint32_t height, float display_scale)
{
    const tb_viewport viewport = tb_viewport_compute(
        policy,
        &(tb_viewport_frame){.width = width, .height = height, .display_scale = display_scale});

    fixture->scale = display_scale;
    tb_gui_set_viewport(fixture->gui, &viewport);
}

static tb_viewport_policy design(uint32_t width, uint32_t height, tb_viewport_aspect aspect,
                                 tb_viewport_scale_mode mode)
{
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;

    policy.design_width = width;
    policy.design_height = height;
    policy.aspect = aspect;
    policy.scale_mode = mode;
    return policy;
}

TB_TEST(gui_scale, a_tools_units_are_logical_pixels_at_the_displays_scale)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);

    gui_resize(&fixture, 1000, 800, 1.25f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.25f, 800.0f, 640.0f);
    tb_gui_set_scale(fixture.gui, 2.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 2.5f, 400.0f, 320.0f);
    /* The policy without a design size is a tool's, whatever its aspect. */
    tb_gui_set_scale(fixture.gui, 1.0f);
    tb_viewport_policy tool = design(0, 0, TB_VIEWPORT_ASPECT_EXPAND, TB_VIEWPORT_SCALE_FRACTIONAL);
    place(&fixture, &tool, 1000, 800, 1.25f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.25f, 800.0f, 640.0f);
    /* A design size of one axis alone is none. */
    tool.design_width = 1920;
    place(&fixture, &tool, 1000, 800, 1.25f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.25f, 800.0f, 640.0f);
    /* No display scale is 1. */
    gui_resize(&fixture, 1000, 800, 0.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.0f, 1000.0f, 800.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, keep_fits_the_design_and_the_bars_reach_no_widget)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = gui_box(tb_gui_root(fixture.gui), 100.0f, 100.0f);
    const tb_viewport_policy keep =
        design(1920, 1080, TB_VIEWPORT_ASPECT_KEEP, TB_VIEWPORT_SCALE_FRACTIONAL);

    TB_TEST_REQUIRE(tb_grid(box, NULL));
    /* The display's scale does not change a fit: the design fills the
     * same share of the frame at any density. */
    place(&fixture, &keep, 1000, 1000, 2.0f);
    const float fit = 1000.0f / 1920.0f;
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 219.0f, fit, 1920.0f, 1080.0f);
    place(&fixture, &keep, 1000, 1000, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 219.0f, fit, 1920.0f, 1080.0f);
    TB_TEST_EXPECT_UINT(tb_gui_viewport_of(fixture.gui).height, ==, 562u);

    /* The mouse is placed in units, from the viewport's corner. */
    gui_move(&fixture, 50.0f, 50.0f);
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.hovered, box));
    /* On the bar above, the mouse is over nothing of the GUI, the root
     * included. */
    gui_move(&fixture, 50.0f, -100.0f);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(!tb_widget_alive(stats.hovered));
    TB_TEST_EXPECT(!tb_gui_wants_mouse(fixture.gui));

    /* The GUI's own factor on top: larger units, and fewer of them. */
    tb_gui_set_scale(fixture.gui, 2.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 219.0f, 2.0f * fit, 960.0f, 540.0f);
    gui_fixture_close(&fixture);
}

/* A widget that fills the root reaches the viewport's edge, and a press
 * just past it, on the bar, reaches nothing: not even a widget as large as
 * the root. */
TB_TEST(gui_scale, a_press_on_the_bars_reaches_no_widget)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget button = tb_button(root, &(tb_button_desc){.text = "Fill"});
    const tb_viewport_policy keep =
        design(400, 300, TB_VIEWPORT_ASPECT_KEEP, TB_VIEWPORT_SCALE_FRACTIONAL);
    tb_gui_stats stats = {0};

    TB_TEST_REQUIRE(tb_grid(button, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    /* Pillarboxed: 400 by 300 in the middle of 800 by 300. */
    place(&fixture, &keep, 800, 300, 1.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(tb_gui_viewport_of(fixture.gui).x, ==, 200u);
    gui_move(&fixture, 399.0f, 150.0f);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.hovered, button));
    gui_move(&fixture, 401.0f, 150.0f);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(!tb_widget_alive(stats.hovered));
    gui_move(&fixture, -1.0f, 150.0f);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(!tb_widget_alive(stats.hovered));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, expand_scales_by_the_smaller_axis_and_gives_the_rest_to_the_root)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = gui_box(tb_gui_root(fixture.gui), 10.0f, 10.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_viewport_policy expand =
        design(1920, 1080, TB_VIEWPORT_ASPECT_EXPAND, TB_VIEWPORT_SCALE_FRACTIONAL);

    TB_TEST_REQUIRE(tb_grid(box, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    place(&fixture, &expand, 2000, 1000, 1.0f);
    const float fit = 1000.0f / 1080.0f;
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, fit, 2000.0f / fit, 1080.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(box).width, 2160.0f, 1.0f / fit);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(box).height, 1080.0f, 1.0f / fit);

    /* A taller frame: the design's width fits, and the height grows. */
    place(&fixture, &expand, 960, 1080, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 0.5f, 1920.0f, 2160.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(box).height, 2160.0f, 1e-3f);
    gui_fixture_close(&fixture);
}

/* In `expand`, the default, the root spans the units the frame shows, not
 * only the design's: a grid of three by three with weighted outer tracks
 * pushes its corner widgets to the frame's real edges, and keeps its
 * middle one in the design's middle, on 16:10, 4:3 and 21:9 frames alike. */
TB_TEST(gui_scale, expand_pins_corners_to_the_frames_edges_and_centers_the_middle)
{
    static const uint32_t frames[][2] = {{2560, 1600}, {1024, 768}, {2560, 1080}, {1920, 1080}};
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget top_left = gui_box(root, 100.0f, 50.0f);
    const tb_widget middle = gui_box(root, 200.0f, 100.0f);
    const tb_widget bottom_right = gui_box(root, 100.0f, 50.0f);
    const tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;

    TB_TEST_REQUIRE(policy.aspect == TB_VIEWPORT_ASPECT_EXPAND);
    TB_TEST_REQUIRE(tb_grid(
        top_left, &(tb_grid_options){.row = 0, .column = 0, .sticky = TB_STICKY_N | TB_STICKY_W}));
    TB_TEST_REQUIRE(tb_grid(middle, &(tb_grid_options){.row = 1, .column = 1}));
    TB_TEST_REQUIRE(
        tb_grid(bottom_right,
                &(tb_grid_options){.row = 2, .column = 2, .sticky = TB_STICKY_S | TB_STICKY_E}));
    for (size_t track = 0; track < 3u; track += 2u)
    {
        TB_TEST_REQUIRE(tb_grid_column_configure(root, track, &(tb_grid_track){.weight = 1.0f}));
        TB_TEST_REQUIRE(tb_grid_row_configure(root, track, &(tb_grid_track){.weight = 1.0f}));
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(frames); index++)
    {
        TB_TEST_CONTEXT("frame %u by %u", frames[index][0], frames[index][1]);
        const tb_viewport viewport = tb_viewport_compute(
            &policy, &(tb_viewport_frame){.width = frames[index][0], .height = frames[index][1]});
        place(&fixture, &policy, frames[index][0], frames[index][1], 1.0f);
        gui_update(&fixture);
        const tb_gui_view view = tb_gui_view_of(fixture.gui);
        TB_TEST_EXPECT_FLOAT_NEAR(view.size.x, viewport.size.x, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(view.size.y, viewport.size.y, 1e-2f);
        const tb_canvas_rect first = tb_widget_rect(top_left);
        const tb_canvas_rect last = tb_widget_rect(bottom_right);
        const tb_canvas_rect center = tb_widget_rect(middle);
        TB_TEST_EXPECT_FLOAT_NEAR(first.x, 0.0f, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(first.y, 0.0f, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(last.x + last.width, viewport.size.x, 1e-2f);
        TB_TEST_EXPECT_FLOAT_NEAR(last.y + last.height, viewport.size.y, 1e-2f);
        const tb_vec2 design_middle = tb_viewport_design_anchor(&viewport, (tb_vec2){0.5f, 0.5f});
        TB_TEST_EXPECT_FLOAT_NEAR(center.x + center.width * 0.5f, design_middle.x, 1.0f);
        TB_TEST_EXPECT_FLOAT_NEAR(center.y + center.height * 0.5f, design_middle.y, 1.0f);
        /* The frame's corners are the root's. */
        const tb_vec2 corner =
            tb_viewport_to_pixels(&viewport, (tb_vec2){last.x + last.width, last.y + last.height});
        TB_TEST_EXPECT_FLOAT_NEAR(corner.x, (float)frames[index][0], 0.5f);
        TB_TEST_EXPECT_FLOAT_NEAR(corner.y, (float)frames[index][1], 0.5f);
    }
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, integer_scales_by_whole_pixels_on_whole_pixels)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_viewport_policy pixels =
        design(320, 180, TB_VIEWPORT_ASPECT_KEEP, TB_VIEWPORT_SCALE_INTEGER);

    place(&fixture, &pixels, 1280, 800, 1.5f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 40.0f, 4.0f, 320.0f, 180.0f);
    place(&fixture, &pixels, 1000, 700, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 20.0f, 80.0f, 3.0f, 320.0f, 180.0f);
    /* Never less than a pixel a unit; what fits of the design is shown,
     * from its top left. */
    place(&fixture, &pixels, 200, 100, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.0f, 200.0f, 100.0f);
    /* A frame of no size yet fits nothing, and stays whole. */
    place(&fixture, &pixels, 0, 0, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gui_view_of(fixture.gui).scale, ==, 1.0f);
    gui_update(&fixture);
    gui_fixture_close(&fixture);
}

/* Stretched by `ignore`, the GUI keeps one scale both ways, the smaller,
 * so that its text is not squashed, and its root fills the viewport with
 * more units along the axis stretched more. */
TB_TEST(gui_scale, ignore_keeps_text_whole_and_fills_the_viewport)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_viewport_policy ignore =
        design(400, 300, TB_VIEWPORT_ASPECT_IGNORE, TB_VIEWPORT_SCALE_FRACTIONAL);

    place(&fixture, &ignore, 800, 300, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.0f, 800.0f, 300.0f);
    TB_TEST_EXPECT_FLOAT(tb_gui_viewport_of(fixture.gui).scale.x, ==, 2.0f);
    gui_fixture_close(&fixture);
}

/* In the viewport render mode, the GUI is laid out, measured and drawn in
 * the viewport's target, a pixel a unit times its own scale, and upscaled
 * with the 3D scene; the mouse is still placed by the viewport on the
 * frame, and reaches nothing over the bars. */
TB_TEST(gui_scale, the_viewport_render_mode_lays_out_at_the_targets_pixels)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = gui_box(tb_gui_root(fixture.gui), 20.0f, 10.0f);
    tb_viewport_policy pixels =
        design(320, 180, TB_VIEWPORT_ASPECT_KEEP, TB_VIEWPORT_SCALE_INTEGER);
    tb_gui_stats stats = {0};

    TB_TEST_REQUIRE(tb_grid(box, NULL));
    pixels.render_mode = TB_VIEWPORT_RENDER_VIEWPORT;
    /* 1280 by 800: 4 pixels a unit, the viewport 40 pixels down. */
    place(&fixture, &pixels, 1280, 800, 1.0f);
    tb_gui_view view = tb_gui_view_of(fixture.gui);
    expect_view(view, 0.0f, 0.0f, 1.0f, 320.0f, 180.0f);
    TB_TEST_EXPECT_INT(view.target, ==, TB_CANVAS_TARGET_VIEWPORT);
    gui_update(&fixture);
    /* The box at the root's top left, units (0, 0) to (20, 10): the frame's
     * pixels (0, 40) to (80, 80). */
    tb_window_event event = {.type = TB_WINDOW_EVENT_MOUSE_MOVED, .pixel_x = 60.0, .pixel_y = 70.0};
    tb_gui_feed_event(fixture.gui, &event);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.hovered, box));
    event.pixel_x = 100.0;
    tb_gui_feed_event(fixture.gui, &event);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(!tb_widget_equal(stats.hovered, box));
    /* On the bar above it, nothing. */
    event.pixel_x = 60.0;
    event.pixel_y = 20.0;
    tb_gui_feed_event(fixture.gui, &event);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(!tb_widget_alive(stats.hovered));

    /* The GUI's own scale makes its units larger in the target: twice as
     * large, half as many, and the mouse placed alike. */
    tb_gui_set_scale(fixture.gui, 2.0f);
    view = tb_gui_view_of(fixture.gui);
    expect_view(view, 0.0f, 0.0f, 2.0f, 160.0f, 90.0f);
    event.pixel_x = 140.0; /* 35 of the viewport's units, 17.5 of the GUI's */
    event.pixel_y = 70.0;
    tb_gui_feed_event(fixture.gui, &event);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.hovered, box));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, text_is_measured_at_the_frames_own_pixels)
{
    static const char text[] = "Field of view";
    const float scales[] = {1.0f, 1.25f, 1.5f, 2.0f};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(scales); index++)
    {
        gui_fixture fixture = {0};
        tb_font_text_size size = {0};
        const float scale = scales[index];
        TB_TEST_CONTEXT("scale %.2f", (double)scale);
        gui_fixture_open(&fixture, scale);
        const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = text});
        TB_TEST_REQUIRE(tb_grid(label, NULL));
        gui_update(&fixture);
        tb_font_measure_text(fixture.font, tb_text_from_cstr(text), GUI_FONT_SIZE * scale, NULL,
                             &size);
        TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_natural_size(label).x, size.width / scale, 1e-3f);
        TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_natural_size(label).y, size.height / scale, 1e-3f);
        /* Its edges on whole pixels. */
        const tb_canvas_rect rect = tb_widget_rect(label);
        TB_TEST_EXPECT_FLOAT_NEAR((rect.x + rect.width) * scale,
                                  roundf((rect.x + rect.width) * scale), 1e-3f);
        gui_fixture_close(&fixture);
    }
}

TB_TEST(gui_scale, a_new_scale_lays_out_again)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "Scaled"});
    tb_gui_stats before = {0};
    tb_gui_stats after = {0};

    TB_TEST_REQUIRE(tb_grid(label, NULL));
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &before);
    gui_resize(&fixture, 1200, 900, 1.5f);
    gui_update(&fixture);
    tb_gui_stats_of(fixture.gui, &after);
    TB_TEST_EXPECT_UINT(after.layouts, ==, before.layouts + 1);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.5f, 800.0f, 600.0f);
    gui_fixture_close(&fixture);
}
