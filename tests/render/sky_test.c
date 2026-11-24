/* Skies on the null backend, which every platform runs: a procedural sky,
 * a cube map's, and none, through perspective and orthographic cameras; a
 * cube texture has six faces and its mip chain. */
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/window.h"

#include "tb_test.h"

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on the null backend, or a skipped case where
 * there is no windowing. */
static void open_screen(screen *opened)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            opened->system,
            &(tb_window_options){.title = "sky", .width = 160, .height = 120, .fixed_size = true},
            &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &opened->renderer, &error), ==, TB_RHI_OK);
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

static void draw_frame(tb_renderer *renderer, const tb_camera *camera)
{
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, camera, (tb_vec4){0, 0, 0, 1}));
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
}

/* A cube texture is six square faces, with the mip chain of one. */
TB_TEST(sky_texture, a_cube_texture_has_six_faces_and_a_mip_chain)
{
    screen opened = {0};
    const size_t size = (size_t)8 * 8 * 4u * 6u;
    uint8_t *pixels = NULL;
    tb_texture *cube = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 0;

    open_screen(&opened);
    pixels = tb_calloc(size, 1);
    cube = tb_texture_create_cube(opened.renderer, &(tb_texture_desc){.width = 8, .height = 8},
                                  pixels, size);
    tb_texture_size(cube, &width, &height, &mips);
    TB_TEST_EXPECT_UINT(width, ==, 8);
    TB_TEST_EXPECT_UINT(height, ==, 8);
    TB_TEST_EXPECT_UINT(mips, ==, 4);
    tb_texture_destroy(opened.renderer, cube);
    tb_free(pixels);
    close_screen(&opened);
}

/* Frames draw under a procedural sky, a sunless one, a cube map's and
 * none, through a perspective camera and an orthographic one. */
TB_TEST(sky_frame, skies_draw_on_the_null_backend)
{
    screen opened = {0};
    const size_t size = (size_t)4 * 4 * 4u * 6u;
    uint8_t *pixels = NULL;
    tb_texture *cube = NULL;
    tb_sky sky = TB_SKY_PROCEDURAL_INIT;
    tb_camera perspective = tb_camera_perspective(tb_radians(70.0f));
    const tb_camera orthographic = tb_camera_orthographic(4.0f);

    open_screen(&opened);
    pixels = tb_calloc(size, 1);
    TB_TEST_REQUIRE(tb_camera_look_at(&perspective, (tb_vec3){0, 1, 4}, (tb_vec3){0, 0, 0}));
    cube = tb_texture_create_cube(opened.renderer,
                                  &(tb_texture_desc){.width = 4, .height = 4, .no_mipmaps = true},
                                  pixels, size);
    draw_frame(opened.renderer, &perspective);
    tb_renderer_set_sky(opened.renderer, &sky);
    draw_frame(opened.renderer, &perspective);
    draw_frame(opened.renderer, &orthographic);
    sky.sun_radius = 0.0f;
    tb_renderer_set_sky(opened.renderer, &sky);
    draw_frame(opened.renderer, &perspective);
    tb_renderer_set_sky(opened.renderer, &(tb_sky){.kind = TB_SKY_CUBE, .cube = cube});
    draw_frame(opened.renderer, &perspective);
    tb_renderer_set_sky(opened.renderer, &(tb_sky){0});
    draw_frame(opened.renderer, &perspective);
    tb_renderer_set_sky(opened.renderer, NULL);
    draw_frame(opened.renderer, &perspective);
    tb_texture_destroy(opened.renderer, cube);
    tb_free(pixels);
    close_screen(&opened);
}
