/* The renderer on D3D12, on WARP, where Windows builds it: a mesh and debug
 * lines land on the pixels they should. */
#include "badger/renderer.h"
#include "badger/window.h"

#include "display_color.h"
#include "tb_test.h"

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on it, or a skipped case where there is none. */
static void open_screen(screen *opened, tb_rhi_backend backend)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "renderer", .width = 160, .height = 120, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Draw a frame: a red flat cube in the middle, a green line on top across
 * the top of the view, and a blue line hidden behind the cube. */
static void draw_frame(tb_renderer *renderer, tb_mesh *cube)
{
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 3}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_renderer_draw_mesh_shaded(renderer, cube, tb_mat4_identity(), (tb_vec4){1, 0, 0, 1},
                                 TB_MESH_FLAT);
    tb_renderer_draw_mesh(renderer, cube, tb_mat4_translation((tb_vec3){100, 0, 0}),
                          (tb_vec4){1, 1, 1, 1});
    tb_debug_line(renderer, (tb_vec3){-5, 1.2f, 0}, (tb_vec3){5, 1.2f, 0}, (tb_vec4){0, 1, 0, 1},
                  TB_DEBUG_ON_TOP);
    tb_debug_line(renderer, (tb_vec3){-0.25f, 0, -2}, (tb_vec3){0.25f, 0, -2},
                  (tb_vec4){0, 0, 1, 1}, 0);
    tb_debug_box(renderer, tb_mat4_identity(), (tb_vec3){2, 2, 2}, (tb_vec4){1, 1, 0, 1}, 0);
    tb_debug_sphere(renderer, (tb_vec3){0, 0, 0}, 3.0f, (tb_vec4){1, 1, 1, 1}, 0);
    tb_debug_grid(renderer, (tb_vec3){0, -3, 0}, 10.0f, 4, (tb_vec4){0.5f, 0.5f, 0.5f, 1}, 0);
    tb_debug_axes(renderer, tb_mat4_translation((tb_vec3){0, 0, 50}), 1.0f, 0);
    tb_debug_arrow(renderer, (tb_vec3){0, 0, 60}, (tb_vec3){0, 1, 60}, (tb_vec4){1, 1, 1, 1}, 0);
    tb_debug_arrow(renderer, (tb_vec3){0, 0, 60}, (tb_vec3){1, 0, 60}, (tb_vec4){1, 1, 1, 1}, 0);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
}

static uint32_t pixel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y)
{
    const uint8_t *const bgra = &pixels[((size_t)y * width + x) * 4u];
    return (uint32_t)bgra[0] | (uint32_t)bgra[1] << 8 | (uint32_t)bgra[2] << 16 |
           (uint32_t)bgra[3] << 24;
}

/* On D3D12, the cube is red in the middle, as the output transform shows
 * it; the line on top is green over everything, as given, since lines are
 * drawn over the tone-mapped frame; and the corners are the clear color. */
TB_TEST(renderer_frame, a_mesh_and_lines_land_where_they_should_on_d3d12)
{
    screen opened = {0};
    tb_mesh *cube = NULL;
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    tb_vec4 clip = {0};
    uint32_t line_y = 0;

    open_screen(&opened, TB_RHI_BACKEND_D3D12);
    cube = tb_mesh_create_shape(opened.renderer, TB_MESH_CUBE, 0);
    tb_renderer_capture_next_frame(opened.renderer);
    draw_frame(opened.renderer, cube);
    pixels = tb_renderer_captured(opened.renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, width / 2, height / 2), ==,
                       0xFF000000u | display_rgb(1, 0, 0));
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, 1, height - 2), ==, 0xFF000000u);
    /* Where the line on top crosses the middle of the view. */
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 3}, (tb_vec3){0, 0, 0}));
    clip = tb_mat4_transform(
        tb_mat4_mul(tb_camera_projection_of(&camera, (float)width / (float)height),
                    tb_camera_view(&camera)),
        (tb_vec4){0, 1.2f, 0, 1});
    line_y = (uint32_t)((1.0f - clip.y / clip.w) * 0.5f * (float)height);
    TB_TEST_EXPECT_HEX(pixel_at(pixels, width, width / 2, line_y) & 0xFF00FF00u, ==, 0xFF00FF00u);
    tb_mesh_destroy(opened.renderer, cube);
    close_screen(&opened);
}
