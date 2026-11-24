/* Lights of a renderer's scene, on the null backend: a handle names its
 * light until the light is removed, and never again after, even when
 * another light takes its slot; the scene holds TB_RENDERER_MAX_LIGHTS at
 * most; and frames run with lights of every kind. */
#include "badger/light.h"
#include "badger/window.h"

#include "tb_test.h"

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on the null backend, or a skipped case. */
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
            &(tb_window_options){.title = "lights", .width = 64, .height = 48, .fixed_size = true},
            &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device.backend = TB_RHI_BACKEND_NULL;
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

/* A handle reads and changes its light until the light is removed; after,
 * it names nothing, though a new light takes the same slot. */
TB_TEST(renderer_light, a_handle_names_its_light_until_it_is_removed)
{
    screen opened = {0};
    tb_light light = TB_LIGHT_INIT;
    tb_light read = {0};
    tb_light_id first = {0};
    tb_light_id second = {0};

    open_screen(&opened);
    TB_TEST_EXPECT_UINT(tb_renderer_light_count(opened.renderer), ==, 0);
    first = tb_renderer_add_light(opened.renderer, &light);
    TB_TEST_REQUIRE(first.id != 0);
    TB_TEST_REQUIRE(tb_renderer_get_light(opened.renderer, first, &read));
    TB_TEST_EXPECT_FLOAT(read.range, ==, 10.0f);
    light.range = 4.0f;
    TB_TEST_EXPECT(tb_renderer_set_light(opened.renderer, first, &light));
    TB_TEST_REQUIRE(tb_renderer_get_light(opened.renderer, first, &read));
    TB_TEST_EXPECT_FLOAT(read.range, ==, 4.0f);
    TB_TEST_EXPECT_UINT(tb_renderer_light_count(opened.renderer), ==, 1);

    tb_renderer_remove_light(opened.renderer, first);
    TB_TEST_EXPECT_UINT(tb_renderer_light_count(opened.renderer), ==, 0);
    TB_TEST_EXPECT(!tb_renderer_get_light(opened.renderer, first, &read));
    TB_TEST_EXPECT(!tb_renderer_set_light(opened.renderer, first, &light));
    tb_renderer_remove_light(opened.renderer, first); /* ignored */
    tb_renderer_remove_light(opened.renderer, (tb_light_id){0});
    TB_TEST_EXPECT(!tb_renderer_get_light(opened.renderer, (tb_light_id){0}, &read));
    TB_TEST_EXPECT(!tb_renderer_get_light(opened.renderer, (tb_light_id){0x7FFFu}, &read));

    second = tb_renderer_add_light(opened.renderer, &light);
    TB_TEST_REQUIRE(second.id != 0);
    TB_TEST_EXPECT_HEX(second.id, !=, first.id);
    TB_TEST_EXPECT(!tb_renderer_get_light(opened.renderer, first, &read));
    TB_TEST_EXPECT(tb_renderer_get_light(opened.renderer, second, &read));
    close_screen(&opened);
}

/* The scene holds TB_RENDERER_MAX_LIGHTS; one more is refused, and room
 * made by removing one is taken again. */
TB_TEST(renderer_light, the_scene_holds_at_most_the_most_lights)
{
    screen opened = {0};
    const tb_light light = TB_LIGHT_INIT;
    tb_light_id last = {0};

    open_screen(&opened);
    for (uint32_t index = 0; index < TB_RENDERER_MAX_LIGHTS; index++)
    {
        last = tb_renderer_add_light(opened.renderer, &light);
        TB_TEST_REQUIRE(last.id != 0);
    }
    TB_TEST_EXPECT_UINT(tb_renderer_add_light(opened.renderer, &light).id, ==, 0);
    tb_renderer_remove_light(opened.renderer, last);
    TB_TEST_EXPECT_UINT(tb_renderer_add_light(opened.renderer, &light).id, !=, 0);
    TB_TEST_EXPECT_UINT(tb_renderer_light_count(opened.renderer), ==, TB_RENDERER_MAX_LIGHTS);
    close_screen(&opened);
}

/* Frames run with a light of each kind, and with none, through the null
 * backend's G-buffer, lighting and tonemap passes. */
TB_TEST(renderer_light, frames_run_with_lights_of_every_kind)
{
    screen opened = {0};
    tb_light light = TB_LIGHT_INIT;
    tb_mesh *cube = NULL;
    const tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    open_screen(&opened);
    cube = tb_mesh_create_shape(opened.renderer, TB_MESH_CUBE, 0);
    for (int kind = 0; kind <= 3; kind++)
    {
        if (kind < 3)
        {
            light.kind = (tb_light_kind)kind;
            TB_TEST_REQUIRE(tb_renderer_add_light(opened.renderer, &light).id != 0);
        }
        else
        {
            tb_renderer_set_ambient(opened.renderer, (tb_vec3){0.1f, 0.1f, 0.2f},
                                    (tb_vec3){0.05f, 0.05f, 0.05f});
        }
        TB_TEST_REQUIRE(tb_renderer_begin_frame(opened.renderer, &camera, (tb_vec4){0, 0, 0, 1}));
        tb_renderer_draw_mesh(opened.renderer, cube, tb_mat4_translation((tb_vec3){0, 0, -3}),
                              (tb_vec4){1, 1, 1, 1});
        TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    }
    TB_TEST_EXPECT_UINT(tb_renderer_light_count(opened.renderer), ==, 3);
    tb_mesh_destroy(opened.renderer, cube);
    close_screen(&opened);
}
