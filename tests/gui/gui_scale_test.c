/* Units and scaling: a tool's GUI in logical pixels at the display's scale,
 * a game's designed at a reference size and stretched to the frame by
 * keeping its aspect, expanding, or whole multiples; the mouse placed in
 * units; a frame resized laid out again; and text measured at the frame's
 * own pixels. */
#include "badger/font_layout.h"

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

TB_TEST(gui_scale, a_tools_units_are_logical_pixels_at_the_displays_scale)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);

    gui_resize(&fixture, 1000, 800, 1.25f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.25f, 800.0f, 640.0f);
    tb_gui_set_scale(fixture.gui, 2.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 2.5f, 400.0f, 320.0f);
    /* A reference size without a stretch is a tool's. */
    tb_gui_set_scale(fixture.gui, 1.0f);
    tb_gui_set_stretch(fixture.gui, (tb_vec2){1920.0f, 1080.0f}, TB_GUI_STRETCH_NONE);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.25f, 800.0f, 640.0f);
    /* A stretch without a reference size likewise. */
    tb_gui_set_stretch(fixture.gui, (tb_vec2){0.0f, 0.0f}, TB_GUI_STRETCH_EXPAND);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.25f, 800.0f, 640.0f);
    /* No display scale is 1. */
    gui_resize(&fixture, 1000, 800, 0.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 1.0f, 1000.0f, 800.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, keep_aspect_fits_the_reference_and_letterboxes_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = gui_box(tb_gui_root(fixture.gui), 100.0f, 100.0f);

    TB_TEST_REQUIRE(tb_grid(box, NULL));
    tb_gui_set_stretch(fixture.gui, (tb_vec2){1920.0f, 1080.0f}, TB_GUI_STRETCH_KEEP_ASPECT);
    /* The display's scale does not change a fit: the reference fills the
     * same share of the frame at any density. */
    gui_resize(&fixture, 1000, 1000, 2.0f);
    const float fit = 1000.0f / 1920.0f;
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 219.0f, fit, 1920.0f, 1080.0f);
    gui_resize(&fixture, 1000, 1000, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 219.0f, fit, 1920.0f, 1080.0f);

    /* The mouse is placed in units, from the letterbox's corner. */
    gui_move(&fixture, 50.0f, 50.0f);
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.hovered, box));
    /* In the bar above, the mouse is over nothing of the GUI. */
    gui_move(&fixture, 50.0f, -100.0f);
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT(!tb_widget_alive(stats.hovered));
    TB_TEST_EXPECT(!tb_gui_wants_mouse(fixture.gui));

    /* A factor on top: larger units, and fewer of them. */
    tb_gui_set_scale(fixture.gui, 2.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 219.0f, 2.0f * fit, 960.0f, 540.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, expand_scales_by_the_smaller_axis_and_gives_the_rest_to_the_root)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = gui_box(tb_gui_root(fixture.gui), 10.0f, 10.0f);
    const tb_widget root = tb_gui_root(fixture.gui);

    TB_TEST_REQUIRE(tb_grid(box, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    tb_gui_set_stretch(fixture.gui, (tb_vec2){1920.0f, 1080.0f}, TB_GUI_STRETCH_EXPAND);
    gui_resize(&fixture, 2000, 1000, 1.0f);
    const float fit = 1000.0f / 1080.0f;
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, fit, 2000.0f / fit, 1080.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(box).width, 2160.0f, 1.0f / fit);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(box).height, 1080.0f, 1.0f / fit);

    /* A taller frame: the reference's width fits, and the height grows. */
    gui_resize(&fixture, 960, 1080, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 0.0f, 0.5f, 1920.0f, 2160.0f);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(box).height, 2160.0f, 1e-3f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_scale, integer_scales_by_whole_pixels_on_whole_pixels)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);

    tb_gui_set_stretch(fixture.gui, (tb_vec2){320.0f, 180.0f}, TB_GUI_STRETCH_INTEGER);
    gui_resize(&fixture, 1280, 800, 1.5f);
    expect_view(tb_gui_view_of(fixture.gui), 0.0f, 40.0f, 4.0f, 320.0f, 180.0f);
    gui_resize(&fixture, 1000, 700, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), 20.0f, 80.0f, 3.0f, 320.0f, 180.0f);
    /* Never less than a pixel a unit; what does not fit is cut off. */
    gui_resize(&fixture, 200, 100, 1.0f);
    expect_view(tb_gui_view_of(fixture.gui), -60.0f, -40.0f, 1.0f, 320.0f, 180.0f);
    /* A frame of no size yet fits nothing, and stays whole. */
    gui_resize(&fixture, 0, 0, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_gui_view_of(fixture.gui).scale, ==, 1.0f);
    gui_update(&fixture);
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
