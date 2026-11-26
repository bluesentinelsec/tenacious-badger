/* The raster lighting mode on D3D12, on WARP, where Windows builds it: a
 * light of intensity 1 lights a white, matte surface facing it to 1, a
 * point light at a meter as a directional light does, and nothing past
 * its range or outside its cone; the ambient light is the sky's; and a
 * scene of materials under every kind of light matches its golden image. */
#include "badger/canvas.h"
#include "badger/light.h"
#include "badger/log.h"
#include "badger/material.h"
#include "badger/sky.h"
#include "badger/window.h"

#include "display_color.h"
#include "golden.h"
#include "tb_test.h"

#include <math.h>
#include <stdlib.h>

/* The backend the case runs on: each runs on D3D12, then on D3D11. */
static tb_rhi_backend tb_test_backend = TB_RHI_BACKEND_D3D12;

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
    tb_mesh *quad;
    tb_log *log;
} screen;

/* What validation warns of fails the case. */
static bool hear(void *context, const tb_log_record *record)
{
    (void)context;
    if (record->level >= TB_LOG_WARNING)
    {
        TB_TEST_FAIL("validation: %.*s", (int)record->length, record->message);
    }
    return true;
}

/* A quad from -1 to 1 in X and Y, facing +Z. */
static tb_mesh *make_quad(tb_renderer *renderer)
{
    static const tb_mesh_vertex vertices[4] = {{{-1, 1, 0}, {0, 0, 1}, {0, 0}},
                                               {{1, 1, 0}, {0, 0, 1}, {1, 0}},
                                               {{-1, -1, 0}, {0, 0, 1}, {0, 1}},
                                               {{1, -1, 0}, {0, 0, 1}, {1, 1}}};
    static const uint32_t indices[6] = {0, 2, 1, 1, 2, 3};

    return tb_mesh_create(renderer, vertices, 4, indices, 6);
}

/* A window and a renderer on it of a profile, or a skipped case where
 * there is none. */
static void open_screen_with(screen *opened, tb_renderer_profile profile)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_rhi_device_info info = {0};
    tb_log_config config = TB_LOG_CONFIG_INIT;

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    opened->window = golden_window(opened->system, "lighting");
    opened->log = tb_log_create();
    config.minimum = TB_LOG_TRACE;
    config.write_record = hear;
    config.flush = NULL;
    TB_TEST_REQUIRE_UINT(tb_log_configure(opened->log, &config), ==, TB_LOG_OK);
    options.window = opened->window;
    options.log = opened->log;
    options.device = (tb_rhi_device_options){
        .backend = tb_test_backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    options.profile = profile;
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        (void)tb_log_destroy(opened->log);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    tb_renderer_set_quality(opened->renderer, &(tb_renderer_quality){.render_scale = 1.0f,
                                                                     .temporal_aa = false,
                                                                     .max_lights = 256,
                                                                     .reflections = true});
    tb_rhi_device_info_of(tb_renderer_device(opened->renderer), &info);
    if ((info.features & TB_RHI_FEATURE_HEAP_INDICES) == 0)
    {
        tb_renderer_destroy(opened->renderer);
        (void)tb_log_destroy(opened->log);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no bindless resources on this device, so no raster lighting mode");
    }
    opened->quad = make_quad(opened->renderer);
}

/* The full path at the output's resolution, without temporal
 * anti-aliasing, so that each pixel is the scene where the camera puts it:
 * WARP would take the low profile by itself. */
static void open_screen(screen *opened)
{
    open_screen_with(opened, TB_RENDERER_PROFILE_FULL);
}

/* Ask for temporal anti-aliasing at a render scale. */
static void ask_temporal(screen *opened, float render_scale)
{
    tb_renderer_quality quality = TB_RENDERER_QUALITY_INIT;

    quality.render_scale = render_scale;
    tb_renderer_set_quality(opened->renderer, &quality);
}

static void close_screen(screen *opened)
{
    tb_mesh_destroy(opened->renderer, opened->quad);
    tb_renderer_destroy(opened->renderer);
    (void)tb_log_destroy(opened->log);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* The middle pixel of a frame of a white quad facing the camera, which
 * looks down -Z, as 0xRRGGBB. */
static uint32_t middle_of_white_quad(screen *opened)
{
    const tb_camera camera = tb_camera_orthographic(4.0f);
    const uint8_t *pixels = NULL;
    const uint8_t *bgra = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    tb_renderer_capture_next_frame(opened->renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_renderer_draw_mesh(opened->renderer, opened->quad, tb_mat4_identity(),
                          (tb_vec4){1, 1, 1, 1});
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
    pixels = tb_renderer_captured(opened->renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    bgra = &pixels[((size_t)(height / 2u) * width + width / 2u) * 4u];
    return (uint32_t)bgra[2] << 16 | (uint32_t)bgra[1] << 8 | (uint32_t)bgra[0];
}

/* Whether two 0xRRGGBB colors are the same, each channel within a
 * tolerance. */
static bool near_color(uint32_t actual, uint32_t expected, int tolerance)
{
    for (int shift = 0; shift < 24; shift += 8)
    {
        const int a = (int)((actual >> shift) & 0xFFu);
        const int e = (int)((expected >> shift) & 0xFFu);
        if (abs(a - e) > tolerance)
        {
            return false;
        }
    }
    return true;
}

/* A white, matte surface facing a light, looked at straight on, reflects
 * the light's intensity, plus the specular of a rough dielectric: GGX's
 * distribution 1/pi times Smith's visibility 1/4 times a Fresnel of 0.04
 * at normal incidence, times pi, about 0.01. */
#define FACING_LIGHT 1.01f

/* Directional, point and spot lights in the units light.h gives: each
 * lights the quad to 1 where it should, and to nothing past a point
 * light's range or outside a spot's cone. The ambient light is off. */
static void lights_light_a_white_surface_as_their_units_say_case(void)
{
    screen opened = {0};
    tb_light light = TB_LIGHT_INIT;
    tb_light_id id = {0};
    uint32_t color = 0;

    open_screen(&opened);
    tb_renderer_set_ambient(opened.renderer, (tb_vec3){0, 0, 0}, (tb_vec3){0, 0, 0});
    light.kind = TB_LIGHT_DIRECTIONAL;
    light.direction = (tb_vec3){0, 0, -1};
    id = tb_renderer_add_light(opened.renderer, &light);
    TB_TEST_REQUIRE(id.id != 0);
    color = middle_of_white_quad(&opened);
    TB_TEST_CONTEXT("directional: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, display_rgb(FACING_LIGHT, FACING_LIGHT, FACING_LIGHT), 2));

    /* A point light of intensity 4 at 2 meters: 4 over 2 squared. */
    light = (tb_light)TB_LIGHT_INIT;
    light.position = (tb_vec3){0, 0, 2};
    light.intensity = 4.0f;
    light.range = 100.0f;
    TB_TEST_REQUIRE(tb_renderer_set_light(opened.renderer, id, &light));
    color = middle_of_white_quad(&opened);
    TB_TEST_CONTEXT("point: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, display_rgb(FACING_LIGHT, FACING_LIGHT, FACING_LIGHT), 2));
    light.range = 1.5f;
    TB_TEST_REQUIRE(tb_renderer_set_light(opened.renderer, id, &light));
    TB_TEST_CONTEXT("point out of range");
    TB_TEST_EXPECT_HEX(middle_of_white_quad(&opened), ==, 0u);

    /* A spot light the same: lit on its axis, dark off its cone. */
    light.kind = TB_LIGHT_SPOT;
    light.range = 100.0f;
    light.direction = (tb_vec3){0, 0, -1};
    TB_TEST_REQUIRE(tb_renderer_set_light(opened.renderer, id, &light));
    color = middle_of_white_quad(&opened);
    TB_TEST_CONTEXT("spot: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, display_rgb(FACING_LIGHT, FACING_LIGHT, FACING_LIGHT), 2));
    light.direction = (tb_vec3){0, 1, -1};
    TB_TEST_REQUIRE(tb_renderer_set_light(opened.renderer, id, &light));
    TB_TEST_CONTEXT("spot pointing away");
    TB_TEST_EXPECT_HEX(middle_of_white_quad(&opened), ==, 0u);
    close_screen(&opened);
}

TB_TEST(lighting_frame, lights_light_a_white_surface_as_their_units_say_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    lights_light_a_white_surface_as_their_units_say_case();
}

TB_TEST(lighting_frame, lights_light_a_white_surface_as_their_units_say_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    lights_light_a_white_surface_as_their_units_say_case();
}

/* With a light of no intensity, the ambient light alone: a surface facing
 * the horizon takes half of the sky's and half of the ground's, plus a
 * little reflection of the horizon (0.4 of the environment BRDF's 0.016
 * for a rough dielectric seen straight on). */
static void a_surface_facing_the_horizon_takes_half_of_sky_and_ground_case(void)
{
    screen opened = {0};
    tb_light light = TB_LIGHT_INIT;
    const float expected = 0.4f + 0.4f * 0.016f;
    uint32_t color = 0;

    open_screen(&opened);
    light.intensity = 0.0f;
    TB_TEST_REQUIRE(tb_renderer_add_light(opened.renderer, &light).id != 0);
    tb_renderer_set_ambient(opened.renderer, (tb_vec3){0.6f, 0.6f, 0.6f},
                            (tb_vec3){0.2f, 0.2f, 0.2f});
    color = middle_of_white_quad(&opened);
    TB_TEST_CONTEXT("ambient: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, display_rgb(expected, expected, expected), 2));
    close_screen(&opened);
}

TB_TEST(lighting_frame, a_surface_facing_the_horizon_takes_half_of_sky_and_ground_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    a_surface_facing_the_horizon_takes_half_of_sky_and_ground_case();
}

TB_TEST(lighting_frame, a_surface_facing_the_horizon_takes_half_of_sky_and_ground_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    a_surface_facing_the_horizon_takes_half_of_sky_and_ground_case();
}

/* ---------------------------------------------------------------------------
 * The golden image.
 * ------------------------------------------------------------------------- */

/* The scene of the golden image: a checkered floor and three spheres (red
 * and rough, gold metal, white and glossy) under a procedural sky, lit by
 * its sun, a blue point light and an orange spot; an unlit cube; and axes
 * at the origin, partly hidden by the floor. */
typedef struct golden_scene
{
    tb_mesh *plane;
    tb_mesh *sphere;
    tb_mesh *cube;
    tb_texture *checker;
    tb_material *floor;
    tb_material *spheres[3];
} golden_scene;

static void golden_scene_create(screen *opened, golden_scene *scene)
{
    static const tb_vec4 colors[3] = {
        {0.8f, 0.1f, 0.08f, 1}, {1.0f, 0.78f, 0.34f, 1}, {1, 1, 1, 1}};
    static const float roughness[3] = {0.7f, 0.3f, 0.15f};
    static const float metallic[3] = {0.0f, 1.0f, 0.0f};
    uint8_t pixels[64 * 64 * 4] = {0};
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    tb_light light = TB_LIGHT_INIT;
    tb_sky sky = TB_SKY_PROCEDURAL_INIT;

    scene->plane = tb_mesh_create_shape(opened->renderer, TB_MESH_PLANE, 0);
    scene->sphere = tb_mesh_create_shape(opened->renderer, TB_MESH_SPHERE, 24);
    scene->cube = tb_mesh_create_shape(opened->renderer, TB_MESH_CUBE, 0);
    tb_image_fill_checkerboard(pixels, sizeof(pixels), 64, 64, 8, (tb_vec4){0.8f, 0.8f, 0.8f, 1},
                               (tb_vec4){0.35f, 0.35f, 0.35f, 1});
    scene->checker = tb_texture_create(
        opened->renderer, &(tb_texture_desc){.width = 64, .height = 64}, pixels, sizeof(pixels));
    desc.base_color = scene->checker;
    desc.roughness = 0.8f;
    desc.uv_scale = (tb_vec2){4, 4};
    scene->floor = tb_material_create(opened->renderer, &desc);
    for (int index = 0; index < 3; index++)
    {
        desc = (tb_material_desc)TB_MATERIAL_DESC_INIT;
        desc.base_color_factor = colors[index];
        desc.roughness = roughness[index];
        desc.metallic = metallic[index];
        scene->spheres[index] = tb_material_create(opened->renderer, &desc);
    }
    tb_renderer_set_sky(opened->renderer, &sky);
    light.kind = TB_LIGHT_DIRECTIONAL;
    light.direction = tb_vec3_negate(sky.sun_direction);
    light.color = sky.sun_color;
    light.intensity = 1.2f;
    TB_TEST_REQUIRE(tb_renderer_add_light(opened->renderer, &light).id != 0);
    light = (tb_light)TB_LIGHT_INIT;
    light.position = (tb_vec3){-1.6f, 0.8f, 0.9f};
    light.color = (tb_vec3){0.3f, 0.5f, 1.0f};
    light.intensity = 2.0f;
    light.range = 4.0f;
    TB_TEST_REQUIRE(tb_renderer_add_light(opened->renderer, &light).id != 0);
    light = (tb_light)TB_LIGHT_INIT;
    light.kind = TB_LIGHT_SPOT;
    light.position = (tb_vec3){2.5f, 3.0f, 1.5f};
    light.direction = tb_vec3_sub((tb_vec3){1.2f, 0.0f, 0.0f}, light.position);
    light.color = (tb_vec3){1.0f, 0.6f, 0.2f};
    light.intensity = 25.0f;
    light.inner_angle = 0.2f;
    light.outer_angle = 0.35f;
    TB_TEST_REQUIRE(tb_renderer_add_light(opened->renderer, &light).id != 0);
}

static void golden_scene_draw(screen *opened, const golden_scene *scene)
{
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 1.6f, 4.5f}, (tb_vec3){0, 0.4f, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_renderer_draw_mesh_material(opened->renderer, scene->plane,
                                   tb_mat4_scaling((tb_vec3){8, 1, 8}), scene->floor);
    for (int index = 0; index < 3; index++)
    {
        tb_renderer_draw_mesh_material(
            opened->renderer, scene->sphere,
            tb_mat4_translation((tb_vec3){(float)(index - 1) * 1.2f, 0.5f, 0}),
            scene->spheres[index]);
    }
    tb_renderer_draw_mesh_shaded(opened->renderer, scene->cube,
                                 tb_mat4_from_trs((tb_vec3){2.3f, 0.25f, -0.6f}, tb_quat_identity(),
                                                  (tb_vec3){0.5f, 0.5f, 0.5f}),
                                 (tb_vec4){0.1f, 0.9f, 0.4f, 1}, TB_MESH_FLAT);
    tb_debug_axes(opened->renderer, tb_mat4_translation((tb_vec3){0, -0.2f, 1.5f}), 0.6f, 0);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
}

static void golden_scene_destroy(screen *opened, golden_scene *scene)
{
    for (int index = 0; index < 3; index++)
    {
        tb_material_destroy(opened->renderer, scene->spheres[index]);
    }
    tb_material_destroy(opened->renderer, scene->floor);
    tb_texture_destroy(opened->renderer, scene->checker);
    tb_mesh_destroy(opened->renderer, scene->cube);
    tb_mesh_destroy(opened->renderer, scene->sphere);
    tb_mesh_destroy(opened->renderer, scene->plane);
}

/* The whole raster lighting mode at once, against an image of it taken on
 * WARP and looked at by eye: the G-buffer, every kind of light, the sky's
 * ambient light and reflections, tone mapping, the sky behind, an unlit
 * mesh, and lines over it all, hidden where the scene is nearer. */
static void a_lit_scene_matches_its_golden_image_case(void)
{
    screen opened = {0};
    golden_scene scene = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    open_screen(&opened);
    golden_scene_create(&opened, &scene);
    tb_renderer_capture_next_frame(opened.renderer);
    golden_scene_draw(&opened, &scene);
    pixels = tb_renderer_captured(opened.renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    expect_golden("lighting", pixels, width, height);
    golden_scene_destroy(&opened, &scene);
    close_screen(&opened);
}

TB_TEST(lighting_frame, a_lit_scene_matches_its_golden_image_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    a_lit_scene_matches_its_golden_image_case();
}

TB_TEST(lighting_frame, a_lit_scene_matches_its_golden_image_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    a_lit_scene_matches_its_golden_image_case();
}

/* Draw the golden scene for some frames, the camera still, and give the
 * last. */
static const uint8_t *golden_frames(screen *opened, const golden_scene *scene, int frames,
                                    uint32_t *width, uint32_t *height)
{
    const uint8_t *pixels = NULL;

    for (int frame = 0; frame < frames; frame++)
    {
        if (frame == frames - 1)
        {
            tb_renderer_capture_next_frame(opened->renderer);
        }
        golden_scene_draw(opened, scene);
    }
    pixels = tb_renderer_captured(opened->renderer, width, height);
    TB_TEST_REQUIRE(pixels != NULL);
    return pixels;
}

/* Temporal anti-aliasing, a cycle of its jitter blended: the edges smooth,
 * the rest as without it. */
static void temporal_anti_aliasing_matches_its_golden_image_case(void)
{
    screen opened = {0};
    golden_scene scene = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    open_screen(&opened);
    ask_temporal(&opened, 1.0f);
    golden_scene_create(&opened, &scene);
    pixels = golden_frames(&opened, &scene, 16, &width, &height);
    expect_golden("lighting_taa", pixels, width, height);
    golden_scene_destroy(&opened, &scene);
    close_screen(&opened);
}

TB_TEST(lighting_frame, temporal_anti_aliasing_matches_its_golden_image_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    temporal_anti_aliasing_matches_its_golden_image_case();
}

TB_TEST(lighting_frame, temporal_anti_aliasing_matches_its_golden_image_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    temporal_anti_aliasing_matches_its_golden_image_case();
}

/* At half the output's resolution, temporal anti-aliasing upsamples the
 * scene to the output's: the frame is the output's size, and the
 * accumulated jitter gives back detail a bilinear filter would not. */
static void temporal_anti_aliasing_upsamples_half_resolution_case(void)
{
    screen opened = {0};
    golden_scene scene = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;

    open_screen(&opened);
    ask_temporal(&opened, 0.5f);
    golden_scene_create(&opened, &scene);
    pixels = golden_frames(&opened, &scene, 16, &width, &height);
    tb_renderer_render_size(opened.renderer, &render_width, &render_height);
    TB_TEST_EXPECT_UINT(render_width, ==, GOLDEN_WIDTH / 2u);
    TB_TEST_EXPECT_UINT(render_height, ==, GOLDEN_HEIGHT / 2u);
    TB_TEST_EXPECT_UINT(width, ==, GOLDEN_WIDTH);
    expect_golden("lighting_taa_half", pixels, width, height);
    golden_scene_destroy(&opened, &scene);
    close_screen(&opened);
}

TB_TEST(lighting_frame, temporal_anti_aliasing_upsamples_half_resolution_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    temporal_anti_aliasing_upsamples_half_resolution_case();
}

TB_TEST(lighting_frame, temporal_anti_aliasing_upsamples_half_resolution_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    temporal_anti_aliasing_upsamples_half_resolution_case();
}

/* WARP runs on the CPU, so a renderer left to choose takes the low profile:
 * half the output's resolution, filtered up, with neither temporal
 * anti-aliasing nor reflections, and says why. The frame is still the
 * output's size. */
static void warp_takes_the_low_profile_by_itself_case(void)
{
    screen opened = {0};
    golden_scene scene = {0};
    tb_renderer_capabilities capabilities = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;

    open_screen_with(&opened, TB_RENDERER_PROFILE_AUTOMATIC);
    tb_renderer_capabilities_of(opened.renderer, &capabilities);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_LOW);
    TB_TEST_EXPECT(!capabilities.temporal_aa);
    TB_TEST_EXPECT(capabilities.reason[0] != '\0');
    ask_temporal(&opened, 1.0f);
    golden_scene_create(&opened, &scene);
    pixels = golden_frames(&opened, &scene, 1, &width, &height);
    tb_renderer_render_size(opened.renderer, &render_width, &render_height);
    TB_TEST_EXPECT_UINT(render_width, ==, GOLDEN_WIDTH / 2u);
    TB_TEST_EXPECT_UINT(width, ==, GOLDEN_WIDTH);
    TB_TEST_EXPECT_UINT(height, ==, GOLDEN_HEIGHT);
    expect_golden("lighting_low", pixels, width, height);
    golden_scene_destroy(&opened, &scene);
    close_screen(&opened);
}

TB_TEST(lighting_frame, warp_takes_the_low_profile_by_itself_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    warp_takes_the_low_profile_by_itself_case();
}

TB_TEST(lighting_frame, warp_takes_the_low_profile_by_itself_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    warp_takes_the_low_profile_by_itself_case();
}

/* Pixels of a row neither the quad's nor the background's: those an edge
 * covers in part. */
static uint32_t partial_pixels(screen *opened, int frames)
{
    const tb_camera camera = tb_camera_orthographic(4.0f);
    const tb_mat4 turned =
        tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, tb_radians(10.0f)));
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t partial = 0;

    for (int frame = 0; frame < frames; frame++)
    {
        if (frame == frames - 1)
        {
            tb_renderer_capture_next_frame(opened->renderer);
        }
        TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
        tb_renderer_draw_mesh_shaded(opened->renderer, opened->quad, turned, (tb_vec4){1, 1, 1, 1},
                                     TB_MESH_FLAT);
        TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
    }
    pixels = tb_renderer_captured(opened->renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const uint8_t green = pixels[((size_t)y * width + x) * 4u + 1u];
            partial += green > 16 && green < display_channel(1.0f) - 16 ? 1u : 0u;
        }
    }
    return partial;
}

/* The edges of a turned quad are steps of whole pixels without temporal
 * anti-aliasing; with it, after two cycles of its jitter, the pixels the
 * edges cross take shares of both sides, as many as the edges are long. */
static void temporal_anti_aliasing_smooths_edges_case(void)
{
    screen opened = {0};
    uint32_t stepped = 0;
    uint32_t smoothed = 0;

    open_screen(&opened);
    stepped = partial_pixels(&opened, 1);
    ask_temporal(&opened, 1.0f);
    smoothed = partial_pixels(&opened, 16);
    TB_TEST_CONTEXT("partly covered: %u without, %u with", stepped, smoothed);
    TB_TEST_EXPECT_UINT(stepped, ==, 0);
    TB_TEST_EXPECT_UINT(smoothed, >, GOLDEN_HEIGHT);
    close_screen(&opened);
}

TB_TEST(lighting_frame, temporal_anti_aliasing_smooths_edges_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    temporal_anti_aliasing_smooths_edges_case();
}

TB_TEST(lighting_frame, temporal_anti_aliasing_smooths_edges_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    temporal_anti_aliasing_smooths_edges_case();
}
