/* The canvas follows a window to a display of another scale: in the frame
 * after the window's content scale changes, the logical view, the frame's
 * size, and text measured in the view are at the new scale. The window's
 * scale is changed as a platform adapter changes it when the window moves
 * to another monitor, through the helper the adapters share, so no event of
 * the desktop's own is involved. On the null backend. */
#include "platform/shared/window.h"

#include "badger/canvas.h"
#include "badger/font.h"
#include "badger/font_layout.h"
#include "badger/str.h"
#include "badger/window.h"

#include "tb_test.h"
#include "test_data.h"

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

/* The window's size, in logical units. */
#define SCALE_WINDOW_WIDTH 160u
#define SCALE_WINDOW_HEIGHT 120u

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
    tb_font *font;
} screen;

static tb_font *open_roboto(void)
{
    const tb_test_file *found = NULL;
    tb_font *font = NULL;

    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        if (tb_str_equal(tb_font_files[index].name, "Roboto-Regular.ttf"))
        {
            found = &tb_font_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    TB_TEST_REQUIRE_UINT(tb_font_open(found->bytes, found->length, NULL, &font, NULL), ==,
                         TB_FONT_OK);
    return font;
}

/* A window, a null renderer on it, and Roboto; or a skipped case where there
 * is no window. */
static void open_screen(screen *opened)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(tb_window_create(opened->system,
                                          &(tb_window_options){.title = "scale",
                                                               .width = SCALE_WINDOW_WIDTH,
                                                               .height = SCALE_WINDOW_HEIGHT,
                                                               .fixed_size = true},
                                          &opened->window, NULL),
                         ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &opened->renderer, &error), ==, TB_RHI_OK);
    opened->font = open_roboto();
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_font_close(opened->font);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Move the window to a display of a scale, as an adapter reports it: its
 * logical size kept, its pixels at the new scale. */
static void move_to_scale(screen *opened, double scale)
{
    tb_window_resized(opened->window, (uint32_t)(SCALE_WINDOW_WIDTH * scale + 0.5),
                      (uint32_t)(SCALE_WINDOW_HEIGHT * scale + 0.5), scale);
}

static void begin(screen *opened)
{
    tb_camera camera = tb_camera_perspective(1.2f);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
}

/* What a frame sees of the scale: the logical view's, and the text's
 * measure in it, in pixels. */
static void expect_frame_at_scale(screen *opened, float scale)
{
    const tb_text text = TB_TEXT("Moving monitors");
    tb_canvas_view view = {0};
    tb_vec2 measured = {0};
    tb_font_text_size pixels = {0};
    uint32_t width = 0;
    uint32_t height = 0;

    TB_TEST_CONTEXT("at %.0f%%", (double)(scale * 100.0f));
    begin(opened);
    view = tb_canvas_view_logical(opened->renderer);
    TB_TEST_EXPECT_FLOAT(view.scale, ==, scale);
    tb_renderer_size(opened->renderer, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, (uint32_t)((float)SCALE_WINDOW_WIDTH * scale + 0.5f));
    TB_TEST_EXPECT_UINT(height, ==, (uint32_t)((float)SCALE_WINDOW_HEIGHT * scale + 0.5f));
    tb_canvas_set_view(opened->renderer, &view);
    measured = tb_canvas_measure_text(opened->renderer, opened->font, text, 16.0f, NULL);
    tb_font_measure_text(opened->font, text, 16.0f * scale, NULL, &pixels);
    TB_TEST_EXPECT_FLOAT_NEAR(measured.x * scale, pixels.width, 0.001f);
    TB_TEST_EXPECT_FLOAT_NEAR(measured.y * scale, pixels.height, 0.001f);
    tb_canvas_draw_text(opened->renderer, opened->font, text, 16.0f, (tb_vec2){4, 4},
                        (tb_vec4){1, 1, 1, 1}, NULL);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
}

/* A window moved from a display at 100% to one at 150%, then to one at
 * 200%, and back: each frame after a move is at the new scale, with
 * nothing for the game to do but draw in the logical view. */
TB_TEST(canvas_scale, the_frame_after_a_change_of_scale_is_at_the_new_scale)
{
    screen opened = {0};

    open_screen(&opened);
    move_to_scale(&opened, 1.0);
    expect_frame_at_scale(&opened, 1.0f);
    move_to_scale(&opened, 1.5);
    expect_frame_at_scale(&opened, 1.5f);
    move_to_scale(&opened, 2.0);
    expect_frame_at_scale(&opened, 2.0f);
    move_to_scale(&opened, 1.0);
    expect_frame_at_scale(&opened, 1.0f);
    close_screen(&opened);
}
