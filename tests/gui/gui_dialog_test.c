/* Message dialogs, images, and canvases: a dialog shown modally, answered
 * by its buttons, Enter, and Escape; an image fitted to its widget each
 * way, and drawn on the null backend's canvas; and a canvas drawn by its
 * callback, clipped, taking the focus on a click. */
#include "badger/canvas.h"
#include "badger/window.h"

#include "gui_fixture.h"
#include "tb_test.h"

typedef struct answer
{
    int count;
    tb_message_result result;
} answer;

static void record_answer(tb_message_result result, void *user)
{
    answer *const answered = user;

    answered->count++;
    answered->result = result;
}

static tb_gui_stats stats_of(const gui_fixture *fixture)
{
    tb_gui_stats stats = {0};
    tb_gui_stats_of(fixture->gui, &stats);
    return stats;
}

TB_TEST(gui_dialog, a_message_box_is_modal_and_answered_by_a_button)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    answer answered = {0};
    const tb_widget behind = gui_box(tb_gui_root(fixture.gui), 100.0f, 100.0f);

    TB_TEST_REQUIRE(tb_grid(behind, NULL));
    gui_update(&fixture);
    const tb_widget dialog =
        tb_gui_message_box(fixture.gui, "Delete the level?", "The level and its lights go.",
                           TB_MESSAGE_YES_NO_CANCEL, record_answer, &answered);
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).modal, dialog));
    TB_TEST_EXPECT_POINTER(tb_widget_class_of(dialog), ==, tb_message_box_class());
    /* The first button has the focus. */
    const tb_widget focus = stats_of(&fixture).focus;
    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(focus), TB_TEXT("Yes")));
    /* The widget behind takes nothing. */
    gui_click(&fixture, (tb_vec2){5.0f, 5.0f});
    TB_TEST_EXPECT_INT(answered.count, ==, 0);

    /* That press took the focus away, as a press on no field does; Tab
     * goes around the dialog's buttons from the first. */
    gui_key(&fixture, TB_KEY_TAB, 0);
    gui_key(&fixture, TB_KEY_TAB, 0);
    const tb_widget no = stats_of(&fixture).focus;
    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(no), TB_TEXT("No")));
    gui_click(&fixture, gui_middle(tb_widget_rect(no)));
    TB_TEST_EXPECT_INT(answered.count, ==, 1);
    TB_TEST_EXPECT_INT(answered.result, ==, TB_MESSAGE_RESULT_NO);
    TB_TEST_EXPECT(!tb_widget_alive(dialog));
    TB_TEST_EXPECT(!tb_widget_alive(stats_of(&fixture).modal));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_dialog, enter_chooses_the_first_button_and_escape_the_last_way_out)
{
    static const struct
    {
        tb_message_buttons buttons;
        tb_message_result first;
        tb_message_result escape;
    } cases[] = {
        {TB_MESSAGE_OK, TB_MESSAGE_RESULT_OK, TB_MESSAGE_RESULT_OK},
        {TB_MESSAGE_OK_CANCEL, TB_MESSAGE_RESULT_OK, TB_MESSAGE_RESULT_CANCEL},
        {TB_MESSAGE_YES_NO, TB_MESSAGE_RESULT_YES, TB_MESSAGE_RESULT_NO},
        {TB_MESSAGE_YES_NO_CANCEL, TB_MESSAGE_RESULT_YES, TB_MESSAGE_RESULT_CANCEL},
        {TB_MESSAGE_RETRY_CANCEL, TB_MESSAGE_RESULT_RETRY, TB_MESSAGE_RESULT_CANCEL},
    };
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        answer answered = {0};
        (void)tb_gui_message_box(fixture.gui, NULL, "Go on?", cases[index].buttons, record_answer,
                                 &answered);
        gui_update(&fixture);
        gui_key(&fixture, TB_KEY_ENTER, 0);
        TB_TEST_EXPECT_INT(answered.count, ==, 1);
        TB_TEST_EXPECT_INT(answered.result, ==, cases[index].first);
        (void)tb_gui_message_box(fixture.gui, "", "Go on?", cases[index].buttons, record_answer,
                                 &answered);
        gui_update(&fixture);
        gui_key(&fixture, TB_KEY_ESCAPE, 0);
        TB_TEST_EXPECT_INT(answered.count, ==, 2);
        TB_TEST_EXPECT_INT(answered.result, ==, cases[index].escape);
    }
    /* Without a callback, and destroyed by hand: nothing is called. */
    const tb_widget quiet =
        tb_gui_message_box(fixture.gui, NULL, "Done.", TB_MESSAGE_OK, NULL, NULL);
    gui_update(&fixture);
    gui_key(&fixture, TB_KEY_A, 0);
    TB_TEST_EXPECT(tb_widget_alive(quiet));
    gui_key(&fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(!tb_widget_alive(quiet));
    answer untouched = {0};
    const tb_widget dropped = tb_gui_message_box(fixture.gui, NULL, "Never answered.",
                                                 TB_MESSAGE_OK, record_answer, &untouched);
    tb_widget_destroy(dropped);
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(untouched.count, ==, 0);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Images.
 * ------------------------------------------------------------------------- */

static void expect_rect(tb_canvas_rect rect, float x, float y, float width, float height)
{
    TB_TEST_EXPECT_FLOAT_NEAR(rect.x, x, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.y, y, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.width, width, 1e-3);
    TB_TEST_EXPECT_FLOAT_NEAR(rect.height, height, 1e-3);
}

TB_TEST(gui_dialog, an_image_is_fitted_each_way_and_placed_by_its_anchor)
{
    const tb_vec2 size = {100.0f, 50.0f};
    const tb_canvas_rect box = {10.0f, 20.0f, 200.0f, 200.0f};

    expect_rect(tb_gui_fit_rect(size, box, TB_IMAGE_FIT_NONE, TB_GUI_ANCHOR_CENTER), 60.0f, 95.0f,
                100.0f, 50.0f);
    expect_rect(tb_gui_fit_rect(size, box, TB_IMAGE_FIT_CONTAIN, TB_GUI_ANCHOR_N), 10.0f, 20.0f,
                200.0f, 100.0f);
    expect_rect(tb_gui_fit_rect(size, box, TB_IMAGE_FIT_COVER, TB_GUI_ANCHOR_CENTER), -90.0f, 20.0f,
                400.0f, 200.0f);
    expect_rect(tb_gui_fit_rect(size, box, TB_IMAGE_FIT_FILL, TB_GUI_ANCHOR_SE), 10.0f, 20.0f,
                200.0f, 200.0f);
    /* Scale down: its own size when it fits, contained when it does not. */
    expect_rect(tb_gui_fit_rect(size, box, TB_IMAGE_FIT_SCALE_DOWN, TB_GUI_ANCHOR_NW), 10.0f, 20.0f,
                100.0f, 50.0f);
    expect_rect(
        tb_gui_fit_rect((tb_vec2){400.0f, 100.0f}, box, TB_IMAGE_FIT_SCALE_DOWN, TB_GUI_ANCHOR_SW),
        10.0f, 170.0f, 200.0f, 50.0f);
    expect_rect(tb_gui_fit_rect((tb_vec2){0}, box, TB_IMAGE_FIT_FILL, TB_GUI_ANCHOR_SW), 10.0f,
                20.0f, 0.0f, 0.0f);
}

TB_TEST(gui_dialog, an_image_widget_wants_its_size_and_draws_without_an_image_nothing)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget image =
        tb_image_widget(tb_gui_root(fixture.gui), &(tb_image_widget_desc){.width = 64.0f});
    tb_gui_value value = {0};
    size_t count = 0;

    TB_TEST_REQUIRE(tb_grid(image, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(image).x, ==, 64.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(image).y, ==, 0.0f);
    TB_TEST_EXPECT_POINTER(tb_image_widget_image(image), ==, NULL);
    (void)tb_gui_commands(fixture.gui, &count);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_REQUIRE(tb_widget_get(image, "tint", &value));
    TB_TEST_EXPECT_FLOAT(value.color.w, ==, 1.0f);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(image, "width", &negative));
    TB_TEST_EXPECT(!tb_widget_set(image, "height", &negative));
    const tb_gui_value tall = tb_gui_float(32.0f);
    TB_TEST_REQUIRE(tb_widget_set(image, "height", &tall));
    TB_TEST_REQUIRE(tb_widget_get(image, "height", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 32.0f);
    TB_TEST_REQUIRE(tb_widget_get(image, "width", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 64.0f);
    TB_TEST_EXPECT(!tb_image_widget_set_image(tb_gui_root(fixture.gui), NULL));
    TB_TEST_EXPECT_POINTER(tb_image_widget_image(tb_gui_root(fixture.gui)), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "image"), ==, tb_image_widget_class());
    gui_fixture_close(&fixture);
}

TB_TEST(gui_dialog, an_image_is_drawn_on_the_canvas_fitted_and_clipped)
{
    gui_fixture fixture = {0};
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_canvas_stats stats = {0};
    tb_camera camera = tb_camera_perspective(1.2f);
    uint8_t pixels[4 * 4 * 2] = {0};
    size_t count = 0;

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            system,
            &(tb_window_options){.title = "gui", .width = 400, .height = 300, .fixed_size = true},
            &window, NULL),
        ==, TB_WINDOW_OK);
    options.window = window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &renderer, &error), ==, TB_RHI_OK);
    tb_image *const picture = tb_image_create(renderer, &(tb_image_desc){.width = 4, .height = 2},
                                              pixels, sizeof(pixels));
    TB_TEST_REQUIRE_POINTER(picture, !=, NULL);

    gui_fixture_open(&fixture, 1.0f);
    gui_resize(&fixture, 400, 300, 1.0f);
    const tb_widget image = tb_image_widget(
        tb_gui_root(fixture.gui),
        &(tb_image_widget_desc){
            .image = picture, .fit = TB_IMAGE_FIT_CONTAIN, .width = 100.0f, .height = 100.0f});
    TB_TEST_REQUIRE(tb_grid(image, NULL));
    gui_update(&fixture);
    const tb_ui_command *const commands = tb_gui_commands(fixture.gui, &count);
    TB_TEST_REQUIRE_UINT(count, ==, 1);
    TB_TEST_EXPECT_UINT(commands[0].type, ==, TB_UI_COMMAND_IMAGE);
    TB_TEST_EXPECT_POINTER(commands[0].image, ==, picture);
    expect_rect(commands[0].rect, 0.0f, 25.0f, 100.0f, 50.0f);
    /* Its own size, unless chosen. */
    TB_TEST_REQUIRE(tb_image_widget_set_image(image, picture));
    tb_widget_set_enabled(image, false);
    gui_update(&fixture);

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_gui_draw(fixture.gui, renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);
    TB_TEST_EXPECT_UINT(stats.quads, >=, 1);

    gui_fixture_close(&fixture);
    tb_image_destroy(renderer, picture);
    tb_renderer_destroy(renderer);
    tb_window_destroy(window);
    tb_window_system_close(system);
}

/* ---------------------------------------------------------------------------
 * Canvases.
 * ------------------------------------------------------------------------- */

typedef struct drawing
{
    int calls;
    tb_canvas_rect rect;
    tb_canvas_rect clip;
} drawing;

static void draw_cross(tb_widget widget, tb_gui_paint *paint, void *user)
{
    drawing *const drawn = user;
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);

    (void)widget;
    drawn->calls++;
    drawn->rect = rect;
    drawn->clip = tb_gui_paint_clip(paint);
    tb_gui_paint_line(paint, (tb_vec2){rect.x, rect.y},
                      (tb_vec2){rect.x + rect.width, rect.y + rect.height}, 1.0f,
                      (tb_vec4){1.0f, 0.0f, 0.0f, 1.0f});
}

TB_TEST(gui_dialog, a_canvas_is_drawn_by_its_callback_clipped_and_takes_the_focus)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    drawing drawn = {0};
    const tb_widget frame = tb_scrollframe(
        tb_gui_root(fixture.gui), &(tb_scrollframe_desc){.width = 100.0f, .height = 60.0f});
    const tb_widget canvas = tb_canvas_widget(frame, &(tb_canvas_widget_desc){.width = 300.0f,
                                                                              .height = 200.0f,
                                                                              .background = true,
                                                                              .draw = draw_cross,
                                                                              .user = &drawn});
    tb_gui_value value = {0};
    size_t count = 0;

    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(canvas, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(drawn.calls, ==, 1);
    TB_TEST_EXPECT_FLOAT(drawn.rect.width, ==, 300.0f);
    /* Seen through the frame: its clip is the frame's view. */
    TB_TEST_EXPECT_FLOAT(drawn.clip.width, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(drawn.clip.height, ==, 60.0f);
    (void)tb_gui_commands(fixture.gui, &count);
    TB_TEST_EXPECT_UINT(count, >=, 2);

    gui_click(&fixture, (tb_vec2){20.0f, 20.0f});
    TB_TEST_EXPECT(tb_widget_equal(stats_of(&fixture).focus, canvas));
    TB_TEST_EXPECT(tb_canvas_widget_set_draw(canvas, NULL, NULL));
    TB_TEST_EXPECT(!tb_canvas_widget_set_draw(frame, NULL, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(drawn.calls, ==, 4);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(canvas, "width", &negative));
    TB_TEST_EXPECT(!tb_widget_set(canvas, "height", &negative));
    TB_TEST_REQUIRE(tb_widget_get(canvas, "width", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 300.0f);
    TB_TEST_REQUIRE(tb_widget_get(canvas, "height", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 200.0f);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "canvas"), ==, tb_canvas_widget_class());
    gui_fixture_close(&fixture);
}
