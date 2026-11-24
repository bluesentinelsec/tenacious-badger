/* The raster lighting mode on D3D12, on WARP, where Windows builds it: a
 * light of intensity 1 lights a white, matte surface facing it to 1, a
 * point light at a meter as a directional light does, and nothing past
 * its range or outside its cone; the ambient light is the sky's; and a
 * scene of materials under every kind of light matches its golden image. */
#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/light.h"
#include "badger/log.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/str.h"
#include "badger/window.h"

#include "display_color.h"
#include "tb_test.h"
#include "test_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const tb_test_file tb_golden_files[];
extern const size_t tb_golden_files_count;

/* The frame of the golden images, in pixels: a size that whole logical
 * sizes reach at display scales of 100%, 125%, 150%, 200% and 250%. */
#define GOLDEN_WIDTH 240u
#define GOLDEN_HEIGHT 180u
/* How far a pixel's channel may stray from its golden image's before it
 * counts as different, and how many pixels in a thousand may differ:
 * WARP's results move a little between versions of Windows. */
#define GOLDEN_CHANNEL_TOLERANCE 16
#define GOLDEN_DIFFERENT_PER_MILLE 10u
/* Bytes of the headers of the BMP files the golden images are. */
#define BMP_FILE_HEADER 14u
#define BMP_INFO_HEADER 40u

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

/* A window of the golden images' size in pixels, whatever the display's
 * scale, or a skipped case at a scale that cannot make it. */
static void open_window(screen *opened)
{
    tb_window_state state = {0};
    tb_window_options options = {
        .title = "lighting", .width = GOLDEN_WIDTH, .height = GOLDEN_HEIGHT, .fixed_size = true};

    TB_TEST_REQUIRE_UINT(tb_window_create(opened->system, &options, &opened->window, NULL), ==,
                         TB_WINDOW_OK);
    tb_window_state_of(opened->window, &state);
    if (state.pixel_width == GOLDEN_WIDTH && state.pixel_height == GOLDEN_HEIGHT)
    {
        return;
    }
    tb_window_destroy(opened->window);
    options.width = (uint32_t)lround(GOLDEN_WIDTH / state.content_scale);
    options.height = (uint32_t)lround(GOLDEN_HEIGHT / state.content_scale);
    TB_TEST_REQUIRE_UINT(tb_window_create(opened->system, &options, &opened->window, NULL), ==,
                         TB_WINDOW_OK);
    tb_window_state_of(opened->window, &state);
    if (state.pixel_width != GOLDEN_WIDTH || state.pixel_height != GOLDEN_HEIGHT)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no window of %ux%u pixels at a display scale of %.2f", GOLDEN_WIDTH,
                     GOLDEN_HEIGHT, state.content_scale);
    }
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
    open_window(opened);
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
        TB_TEST_SKIP("no bindless resources on this device, so no raster lighting mode");
    }
    opened->quad = make_quad(opened->renderer);
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
TB_TEST(lighting_frame, lights_light_a_white_surface_as_their_units_say_on_d3d12)
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

/* With a light of no intensity, the ambient light alone: a surface facing
 * the horizon takes half of the sky's and half of the ground's, plus a
 * little reflection of the horizon (0.4 of the environment BRDF's 0.016
 * for a rough dielectric seen straight on). */
TB_TEST(lighting_frame, a_surface_facing_the_horizon_takes_half_of_sky_and_ground_on_d3d12)
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

/* ---------------------------------------------------------------------------
 * The golden image.
 * ------------------------------------------------------------------------- */

static void write_u16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *at, uint32_t value)
{
    write_u16(at, value & 0xFFFFu);
    write_u16(at + 2, value >> 16);
}

static uint32_t read_u32(const uint8_t *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

/* Write a frame as a 32-bit BMP, rows from the top, so that it can become
 * a golden image: `<name>.actual.bmp` in the working directory. */
static void write_actual(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    const size_t pixel_bytes = (size_t)width * height * 4u;
    const size_t size = BMP_FILE_HEADER + BMP_INFO_HEADER + pixel_bytes;
    uint8_t *const file = tb_calloc(1, size);
    char path[128] = {0};

    file[0] = 'B';
    file[1] = 'M';
    write_u32(&file[2], (uint32_t)size);
    write_u32(&file[10], BMP_FILE_HEADER + BMP_INFO_HEADER);
    write_u32(&file[14], BMP_INFO_HEADER);
    write_u32(&file[18], width);
    write_u32(&file[22], (uint32_t)-(int32_t)height); /* negative: rows from the top */
    write_u16(&file[26], 1);
    write_u16(&file[28], 32);
    write_u32(&file[34], (uint32_t)pixel_bytes);
    memcpy(&file[BMP_FILE_HEADER + BMP_INFO_HEADER], pixels, pixel_bytes);
    (void)tb_fmt_buffer(path, sizeof(path), "%s.actual.bmp", name);
    (void)tb_fs_write_file((tb_path_view){path, tb_str_length(path)}, file, size);
    tb_free(file);
}

/* The pixels of a golden image written as write_actual() writes, or NULL
 * when there is none of that name and size. */
static const uint8_t *golden_pixels(const char *name, uint32_t width, uint32_t height)
{
    char file_name[128] = {0};

    (void)tb_fmt_buffer(file_name, sizeof(file_name), "%s.bmp", name);
    for (size_t index = 0; index < tb_golden_files_count; index++)
    {
        const tb_test_file *const file = &tb_golden_files[index];
        if (!tb_str_equal(file->name, file_name))
        {
            continue;
        }
        if (file->length != BMP_FILE_HEADER + BMP_INFO_HEADER + (size_t)width * height * 4u ||
            read_u32(&file->bytes[18]) != width ||
            read_u32(&file->bytes[22]) != (uint32_t)-(int32_t)height)
        {
            return NULL;
        }
        return &file->bytes[BMP_FILE_HEADER + BMP_INFO_HEADER];
    }
    return NULL;
}

/* Compare a frame with its golden image, within the tolerances; on a
 * difference, or without one, write the frame beside the tests, to look at
 * or to take as the golden image (tests/data/golden). */
static void expect_golden(const char *name, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    const uint8_t *const golden = golden_pixels(name, width, height);
    size_t different = 0;

    if (golden == NULL)
    {
        write_actual(name, pixels, width, height);
        TB_TEST_FAIL("no golden image %s.bmp of %ux%u; this frame is in %s.actual.bmp", name, width,
                     height, name);
        return;
    }
    for (size_t pixel = 0; pixel < (size_t)width * height; pixel++)
    {
        for (size_t channel = 0; channel < 3; channel++)
        {
            const int actual = pixels[pixel * 4u + channel];
            const int expected = golden[pixel * 4u + channel];
            if (abs(actual - expected) > GOLDEN_CHANNEL_TOLERANCE)
            {
                different++;
                break;
            }
        }
    }
    TB_TEST_CONTEXT("%s: %zu pixels differ", name, different);
    if (different * 1000u > (size_t)width * height * GOLDEN_DIFFERENT_PER_MILLE)
    {
        write_actual(name, pixels, width, height);
        TB_TEST_FAIL("%s differs from its golden image in %zu pixels; this frame is in "
                     "%s.actual.bmp",
                     name, different, name);
    }
}

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
TB_TEST(lighting_frame, a_lit_scene_matches_its_golden_image_on_d3d12)
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
