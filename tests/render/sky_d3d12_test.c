/* Skies on D3D12, on WARP, where Windows builds it: looking up shows the
 * zenith, down the ground, toward the sun the sun; the sky stays behind
 * what the scene drew; and a cube map shows the face a direction reaches. */
#include "badger/log.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/window.h"

#include "tb_test.h"

#include <stdlib.h>

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
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

/* A window and a renderer on it, or a skipped case where there is none. */
static void open_screen(screen *opened)
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
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            opened->system,
            &(tb_window_options){.title = "sky", .width = 160, .height = 120, .fixed_size = true},
            &opened->window, NULL),
        ==, TB_WINDOW_OK);
    opened->log = tb_log_create();
    config.minimum = TB_LOG_TRACE;
    config.write_record = hear;
    config.flush = NULL;
    TB_TEST_REQUIRE_UINT(tb_log_configure(opened->log, &config), ==, TB_LOG_OK);
    options.window = opened->window;
    options.log = opened->log;
    options.device = (tb_rhi_device_options){
        .backend = TB_RHI_BACKEND_D3D12, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        (void)tb_log_destroy(opened->log);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    tb_rhi_device_info_of(tb_renderer_device(opened->renderer), &info);
    if ((info.features & TB_RHI_FEATURE_BINDLESS) == 0)
    {
        tb_renderer_destroy(opened->renderer);
        (void)tb_log_destroy(opened->log);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no bindless resources on this device");
    }
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    (void)tb_log_destroy(opened->log);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* A camera at the origin turned from looking down -Z. */
static tb_camera camera_turned(tb_vec3 axis, float angle)
{
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    camera.orientation = tb_quat_from_axis_angle(axis, angle);
    return camera;
}

/* The middle pixel of a frame through a camera, as 0xRRGGBB, with a mesh
 * drawn first if any. */
static uint32_t middle_of(screen *opened, const tb_camera *camera, const tb_mesh *mesh)
{
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    const uint8_t *bgra = NULL;

    tb_renderer_capture_next_frame(opened->renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, camera, (tb_vec4){1, 0, 1, 1}));
    if (mesh != NULL)
    {
        tb_renderer_draw_mesh_shaded(opened->renderer, mesh,
                                     tb_mat4_translation((tb_vec3){0, 0, -3}),
                                     (tb_vec4){0, 1, 0, 1}, TB_MESH_FLAT);
    }
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

/* A procedural sky: straight up is the zenith's color, straight down the
 * ground's, and toward the sun the sun's; a mesh in front hides it. The
 * frame is written linear, so 0.2 is 51. */
TB_TEST(sky_frame, a_procedural_sky_shows_zenith_ground_and_sun_on_d3d12)
{
    screen opened = {0};
    tb_sky sky = TB_SKY_PROCEDURAL_INIT;
    const tb_vec3 sun = {0.6f, 0.3f, -1.0f};
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    tb_mesh *cube = NULL;
    uint32_t color = 0;

    open_screen(&opened);
    sky.zenith = (tb_vec3){0.2f, 0.4f, 0.8f};
    sky.horizon = (tb_vec3){0.8f, 0.8f, 0.8f};
    sky.ground = (tb_vec3){0.4f, 0.2f, 0.0f};
    sky.sun_direction = sun;
    sky.sun_color = (tb_vec3){1.0f, 1.0f, 0.0f};
    sky.sun_radius = 0.1f;
    tb_renderer_set_sky(opened.renderer, &sky);
    cube = tb_mesh_create_shape(opened.renderer, TB_MESH_CUBE, 0);

    camera = camera_turned((tb_vec3){1, 0, 0}, TB_PI * 0.5f);
    color = middle_of(&opened, &camera, NULL);
    TB_TEST_CONTEXT("up: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, 0x3366CCu, 2));
    camera = camera_turned((tb_vec3){1, 0, 0}, -TB_PI * 0.5f);
    color = middle_of(&opened, &camera, NULL);
    TB_TEST_CONTEXT("down: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, 0x663300u, 2));
    camera = tb_camera_perspective(tb_radians(60.0f));
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 0}, sun));
    color = middle_of(&opened, &camera, NULL);
    TB_TEST_CONTEXT("sun: %06X", (unsigned)color);
    TB_TEST_EXPECT(near_color(color, 0xFFFF00u, 2));
    /* Straight ahead, at the horizon, a cube three meters away is in front
     * of the sky; without it, the horizon's color, nearly: the middle pixel
     * is half a pixel below the horizon, a little toward the ground. */
    camera = tb_camera_perspective(tb_radians(60.0f));
    TB_TEST_CONTEXT("ahead");
    TB_TEST_EXPECT_HEX(middle_of(&opened, &camera, cube), ==, 0x00FF00u);
    TB_TEST_EXPECT(near_color(middle_of(&opened, &camera, NULL), 0xCCCCCCu, 16));
    /* Without a sky, the clear color. */
    tb_renderer_set_sky(opened.renderer, NULL);
    TB_TEST_EXPECT_HEX(middle_of(&opened, &camera, NULL), ==, 0xFF00FFu);
    tb_mesh_destroy(opened.renderer, cube);
    close_screen(&opened);
}

/* A cube map of six plain faces: each direction shows its face, Direct3D's
 * order (+X, -X, +Y, -Y, +Z, -Z). */
TB_TEST(sky_frame, a_cube_map_sky_shows_each_face_where_it_should_on_d3d12)
{
    enum
    {
        FACE = 4
    };
    static const uint32_t faces[6] = {0xFF0000u, 0x00FFFFu, 0x00FF00u,
                                      0xFF00FFu, 0x0000FFu, 0xFFFF00u};
    static const struct
    {
        tb_vec3 axis;
        float angle;
        int face;
    } looks[6] = {{{0, 1, 0}, -0.5f, 0}, {{0, 1, 0}, 0.5f, 1}, {{1, 0, 0}, 0.5f, 2},
                  {{1, 0, 0}, -0.5f, 3}, {{0, 1, 0}, 1.0f, 4}, {{0, 1, 0}, 0.0f, 5}};
    screen opened = {0};
    const size_t size = (size_t)FACE * FACE * 4u * 6u;
    uint8_t *pixels = NULL;
    tb_texture *cube = NULL;

    open_screen(&opened);
    pixels = tb_malloc(size);
    for (size_t texel = 0; texel < (size_t)FACE * FACE * 6u; texel++)
    {
        const uint32_t color = faces[texel / (FACE * FACE)];
        pixels[texel * 4u + 0] = (uint8_t)(color >> 16);
        pixels[texel * 4u + 1] = (uint8_t)(color >> 8);
        pixels[texel * 4u + 2] = (uint8_t)color;
        pixels[texel * 4u + 3] = UINT8_MAX;
    }
    cube = tb_texture_create_cube(opened.renderer,
                                  &(tb_texture_desc){.width = FACE, .height = FACE}, pixels, size);
    tb_renderer_set_sky(opened.renderer, &(tb_sky){.kind = TB_SKY_CUBE, .cube = cube});
    for (size_t look = 0; look < TB_TEST_COUNT_OF(looks); look++)
    {
        const tb_camera camera = camera_turned(looks[look].axis, looks[look].angle * TB_PI);
        TB_TEST_CONTEXT("face %d", looks[look].face);
        TB_TEST_EXPECT_HEX(middle_of(&opened, &camera, NULL), ==, faces[looks[look].face]);
    }
    tb_renderer_set_sky(opened.renderer, NULL);
    tb_texture_destroy(opened.renderer, cube);
    tb_free(pixels);
    close_screen(&opened);
}
