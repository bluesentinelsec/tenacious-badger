/* The interface drawn on a renderer's canvas, on the null backend: every
 * kind of command becomes quads, text becomes glyphs, and what is clipped
 * away entirely is not drawn. */
#include "badger/canvas.h"
#include "badger/window.h"

#include "tb_test.h"
#include "ui_fixture.h"

static void draw_scene(ui_fixture *fixture)
{
    static const char *const items[] = {"One", "Two"};

    if (tb_ui_begin_window(fixture->ui, "Drawn", (tb_canvas_rect){10, 10, 300, 220}, 0))
    {
        tb_ui_text(fixture->ui, "Text");
        (void)tb_ui_button(fixture->ui, "Button");
        (void)tb_ui_checkbox(fixture->ui, "Checked", &fixture->flag);
        (void)tb_ui_radio(fixture->ui, "Radio", &fixture->choice, 0);
        (void)tb_ui_combo(fixture->ui, "Combo", &fixture->selected, items, 2);
        (void)tb_ui_header(fixture->ui, "Header", true);
        for (int line = 0; line < 20; line++)
        {
            tb_ui_textf(fixture->ui, "Line %d", line);
        }
        tb_ui_end_window(fixture->ui);
    }
}

TB_TEST(ui_draw, commands_become_quads_on_the_canvas)
{
    ui_fixture fixture = {0};
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_canvas_stats stats = {0};
    size_t count = 0;
    size_t texts = 0;
    tb_camera camera = tb_camera_perspective(1.2f);

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            system,
            &(tb_window_options){.title = "ui", .width = 400, .height = 300, .fixed_size = true},
            &window, NULL),
        ==, TB_WINDOW_OK);
    options.window = window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &renderer, &error), ==, TB_RHI_OK);

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.flag = true;
    fixture.scene = draw_scene;
    ui_frame(&fixture);
    const tb_ui_command *const commands = tb_ui_commands(fixture.ui, &count);
    for (size_t index = 0; index < count; index++)
    {
        texts += commands[index].type == TB_UI_COMMAND_TEXT ? 1 : 0;
    }
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_ui_draw(fixture.ui, renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);
    TB_TEST_EXPECT_UINT(stats.glyphs, >, texts);
    TB_TEST_EXPECT_UINT(stats.quads, >, count - texts);

    /* Commands clipped to what is wholly outside the frame draw nothing:
     * the contents of a window moved past its right edge. */
    const size_t quads = stats.quads;
    ui_move(&fixture, 50.0f, 20.0f);
    ui_frame(&fixture);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, 50.0f + 600.0f, 20.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_ui_draw(fixture.ui, renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.quads, <, quads / 2);

    ui_fixture_close(&fixture);
    tb_renderer_destroy(renderer);
    tb_window_destroy(window);
    tb_window_system_close(system);
}
