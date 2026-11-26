/* Skinning on D3D12, on WARP, where Windows builds it: a cylinder of two
 * bones, bent at its middle by the compute pass, is drawn where the same
 * vertices skinned on the CPU project, in a color and with a material; and
 * the renderer forced to skin on the CPU draws the same pixels as the GPU's
 * pass, within a tolerance. */
#include "badger/anim.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/skinning.h"
#include "badger/window.h"

#include "display_color.h"
#include "tb_test.h"

#include <math.h>
#include <string.h>

/* The backend the case runs on: each runs on D3D12, then on D3D11. */
static tb_rhi_backend tb_test_backend = TB_RHI_BACKEND_D3D12;

/* Rings along the cylinder, and segments around it. */
#define RINGS 9u
#define SEGMENTS 16u
#define RADIUS 0.15f
#define HEIGHT 2.0f

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on D3D12, or a skipped case where there is none. */
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
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "skinning", .width = 160, .height = 120, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = tb_test_backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    /* The full path at the output's resolution, without temporal
     * anti-aliasing, so that each pixel is the scene where the camera puts
     * it: WARP would take the low profile by itself. */
    options.profile = TB_RENDERER_PROFILE_FULL;
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    tb_renderer_set_quality(opened->renderer, &(tb_renderer_quality){.render_scale = 1.0f,
                                                                     .temporal_aa = false,
                                                                     .max_lights = 256,
                                                                     .reflections = true});
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* A point on the cylinder's side, at a height and an angle around it from
 * +Z toward +X. */
static tb_vec3 side_point(float y, float angle)
{
    return (tb_vec3){RADIUS * sinf(angle), y, RADIUS * cosf(angle)};
}

/* A capless cylinder up +Y from 0 to HEIGHT, bound below its middle to
 * joint 0 and above to joint 1, its middle ring to both by half. */
static tb_skinned_mesh *make_cylinder(tb_renderer *renderer)
{
    tb_skinned_vertex vertices[RINGS * (SEGMENTS + 1u)] = {0};
    uint32_t indices[(RINGS - 1u) * SEGMENTS * 6u] = {0};
    size_t index_count = 0;

    for (uint32_t ring = 0; ring < RINGS; ring++)
    {
        const float y = HEIGHT * (float)ring / (float)(RINGS - 1u);
        for (uint32_t segment = 0; segment <= SEGMENTS; segment++)
        {
            const float angle = TB_TAU * (float)segment / (float)SEGMENTS;
            tb_skinned_vertex *const vertex = &vertices[ring * (SEGMENTS + 1u) + segment];
            vertex->position = side_point(y, angle);
            vertex->normal = (tb_vec3){sinf(angle), 0, cosf(angle)};
            vertex->joints[0] = 0;
            vertex->joints[1] = 1;
            vertex->weights[0] = 0.5f;
            if (y != 1.0f)
            {
                vertex->weights[0] = y < 1.0f ? 1.0f : 0.0f;
            }
            vertex->weights[1] = 1.0f - vertex->weights[0];
        }
    }
    for (uint32_t ring = 0; ring + 1u < RINGS; ring++)
    {
        for (uint32_t segment = 0; segment < SEGMENTS; segment++)
        {
            const uint32_t below = ring * (SEGMENTS + 1u) + segment;
            const uint32_t above = below + SEGMENTS + 1u;
            const uint32_t quad[6] = {below, below + 1u, above + 1u, below, above + 1u, above};
            for (uint32_t corner = 0; corner < 6; corner++)
            {
                indices[index_count] = quad[corner];
                index_count++;
            }
        }
    }
    return tb_skinned_mesh_create(renderer, vertices, TB_TEST_COUNT_OF(vertices), indices,
                                  index_count);
}

static uint32_t pixel_at(const uint8_t *pixels, uint32_t width, uint32_t x, uint32_t y)
{
    const uint8_t *const bgra = &pixels[((size_t)y * width + x) * 4u];
    return (uint32_t)bgra[0] | (uint32_t)bgra[1] << 8 | (uint32_t)bgra[2] << 16 |
           (uint32_t)bgra[3] << 24;
}

/* Where a point in the world is drawn, in pixels from the top left. */
static void project(const tb_camera *camera, uint32_t width, uint32_t height, tb_vec3 point,
                    uint32_t *x, uint32_t *y)
{
    const tb_mat4 clip_from_world = tb_mat4_mul(
        tb_camera_projection_of(camera, (float)width / (float)height), tb_camera_view(camera));
    const tb_vec4 clip =
        tb_mat4_transform(clip_from_world, (tb_vec4){point.x, point.y, point.z, 1});

    *x = (uint32_t)((clip.x / clip.w * 0.5f + 0.5f) * (float)width);
    *y = (uint32_t)((0.5f - clip.y / clip.w * 0.5f) * (float)height);
}

/* The cylinder's skinning matrices with its upper bone bent a quarter turn
 * clockwise about Z, so that its upper half points along +X. */
static void bend(tb_mat4 skinning[2])
{
    tb_skeleton_joint bones[2] = {{.parent = TB_SKELETON_NO_PARENT, .rest = TB_ANIM_TRANSFORM_INIT},
                                  {.parent = 0, .rest = TB_ANIM_TRANSFORM_INIT}};
    tb_skeleton *skeleton = NULL;
    tb_anim_transform pose[2] = {TB_ANIM_TRANSFORM_INIT, TB_ANIM_TRANSFORM_INIT};
    tb_mat4 model[2] = {0};

    bones[1].rest.translation = (tb_vec3){0, 1, 0};
    skeleton = tb_skeleton_create(&(tb_skeleton_desc){bones, 2, NULL});
    TB_TEST_REQUIRE(skeleton != NULL);
    pose[1].translation = (tb_vec3){0, 1, 0};
    pose[1].rotation = tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, -TB_PI * 0.5f);
    tb_anim_model_pose(skeleton, pose, model, 2);
    tb_anim_skinning_matrices(skeleton, model, skinning, 2);
    tb_skeleton_destroy(skeleton);
}

/* On D3D12, the bent cylinder's front near its tip is drawn where the CPU
 * skins it to, and nothing is drawn where it would be unbent; a material
 * draws it the same. */
static void a_bent_cylinder_lands_where_the_cpu_skins_it_case(void)
{
    static const uint16_t upper[4] = {0, 1, 0, 0};
    static const float upper_weights[4] = {0, 1, 0, 0};
    screen opened = {0};
    tb_skinned_mesh *cylinder = NULL;
    tb_material *green = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    tb_mat4 skinning[2] = {0};
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    const tb_vec3 near_tip = side_point(1.9f, 0.0f);
    tb_vec3 skinned = {0};
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    open_screen(&opened);
    cylinder = make_cylinder(opened.renderer);
    desc.base_color_factor = (tb_vec4){0, 1, 0, 1};
    desc.unlit = true;
    green = tb_material_create(opened.renderer, &desc);
    bend(skinning);
    skinned =
        tb_mat4_transform_point(tb_anim_skin_matrix(skinning, 2, upper, upper_weights), near_tip);
    TB_TEST_EXPECT_FLOAT_NEAR(skinned.x, 0.9f, 1e-5f);
    TB_TEST_EXPECT_FLOAT_NEAR(skinned.y, 1.0f, 1e-5f);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0.5f, 1, 4}, (tb_vec3){0.5f, 1, 0}));
    for (int pass = 0; pass < 2; pass++)
    {
        const bool material = pass == 1;
        tb_renderer_capture_next_frame(opened.renderer);
        TB_TEST_REQUIRE(tb_renderer_begin_frame(opened.renderer, &camera, (tb_vec4){0, 0, 0, 1}));
        tb_renderer_draw_skinned(opened.renderer,
                                 &(tb_skinned_draw){.mesh = cylinder,
                                                    .transform = tb_mat4_identity(),
                                                    .joints = skinning,
                                                    .joint_count = 2,
                                                    .material = material ? green : NULL,
                                                    .color = {1, 0, 0, 1},
                                                    .shading = TB_MESH_FLAT});
        TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
        pixels = tb_renderer_captured(opened.renderer, &width, &height);
        TB_TEST_REQUIRE(pixels != NULL);
        project(&camera, width, height, skinned, &x, &y);
        TB_TEST_CONTEXT("tip at (%u, %u), %s", x, y, material ? "material" : "color");
        TB_TEST_EXPECT_HEX(pixel_at(pixels, width, x, y), ==,
                           0xFF000000u | (material ? display_rgb(0, 1, 0) : display_rgb(1, 0, 0)));
        project(&camera, width, height, near_tip, &x, &y);
        TB_TEST_EXPECT_HEX(pixel_at(pixels, width, x, y), ==, 0xFF000000u);
    }
    tb_material_destroy(opened.renderer, green);
    tb_skinned_mesh_destroy(opened.renderer, cylinder);
    close_screen(&opened);
}

TB_TEST(skinning_frame, a_bent_cylinder_lands_where_the_cpu_skins_it_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    a_bent_cylinder_lands_where_the_cpu_skins_it_case();
}

TB_TEST(skinning_frame, a_bent_cylinder_lands_where_the_cpu_skins_it_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    a_bent_cylinder_lands_where_the_cpu_skins_it_case();
}

/* A channel's difference past which two pictures differ at a pixel: the
 * GPU's arithmetic rounds otherwise than the CPU's. */
#define CHANNEL_TOLERANCE 4
/* Pixels in a thousand that may differ, along the edges, where a vertex
 * that moved by a rounding error takes a pixel's center in or out. */
#define DIFFERING_PER_MILLE 5u

/* Draw the bent cylinder lit, in a color beside the same with a lit
 * material, and keep the frame's pixels. */
static uint8_t *draw_bent(screen *opened, const tb_skinned_mesh *cylinder,
                          const tb_material *material, const tb_mat4 skinning[2],
                          const tb_camera *camera, uint32_t *width, uint32_t *height)
{
    const uint8_t *pixels = NULL;
    uint8_t *kept = NULL;

    tb_renderer_capture_next_frame(opened->renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, camera, (tb_vec4){0, 0, 0, 1}));
    tb_renderer_draw_skinned(opened->renderer, &(tb_skinned_draw){.mesh = cylinder,
                                                                  .transform = tb_mat4_identity(),
                                                                  .joints = skinning,
                                                                  .joint_count = 2,
                                                                  .color = {1, 0.5f, 0.25f, 1}});
    tb_renderer_draw_skinned(
        opened->renderer, &(tb_skinned_draw){.mesh = cylinder,
                                             .transform = tb_mat4_translation((tb_vec3){-1, 0, 0}),
                                             .joints = skinning,
                                             .joint_count = 2,
                                             .material = material});
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
    pixels = tb_renderer_captured(opened->renderer, width, height);
    TB_TEST_REQUIRE(pixels != NULL);
    kept = tb_calloc((size_t)*width * *height, 4u);
    memcpy(kept, pixels, (size_t)*width * *height * 4u);
    return kept;
}

/* The same bent cylinders, lit, in a color and with a material, skinned by
 * the GPU's pass and then by the CPU, draw the same pixels but for a few
 * along their edges; and enough of them that the comparison means
 * something. */
static void the_cpu_path_draws_what_the_gpu_path_draws_case(void)
{
    screen opened = {0};
    tb_skinned_mesh *cylinder = NULL;
    tb_material *material = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    tb_mat4 skinning[2] = {0};
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    tb_skinning_info info = {0};
    uint8_t *on_gpu = NULL;
    uint8_t *on_cpu = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t cpu_width = 0;
    uint32_t cpu_height = 0;
    size_t drawn = 0;
    size_t differing = 0;

    open_screen(&opened);
    tb_renderer_skinning_info(opened.renderer, &info);
    TB_TEST_REQUIRE_UINT(info.path, ==, TB_SKINNING_GPU);
    cylinder = make_cylinder(opened.renderer);
    desc.base_color_factor = (tb_vec4){0.25f, 0.5f, 1, 1};
    material = tb_material_create(opened.renderer, &desc);
    bend(skinning);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 1, 4}, (tb_vec3){0, 1, 0}));
    on_gpu = draw_bent(&opened, cylinder, material, skinning, &camera, &width, &height);
    tb_renderer_force_cpu_skinning(opened.renderer, true);
    on_cpu = draw_bent(&opened, cylinder, material, skinning, &camera, &cpu_width, &cpu_height);
    tb_renderer_skinning_info(opened.renderer, &info);
    TB_TEST_EXPECT_UINT(info.path, ==, TB_SKINNING_CPU);
    TB_TEST_REQUIRE_UINT(cpu_width, ==, width);
    TB_TEST_REQUIRE_UINT(cpu_height, ==, height);
    for (size_t pixel = 0; pixel < (size_t)width * height; pixel++)
    {
        const uint8_t *const gpu = &on_gpu[pixel * 4u];
        const uint8_t *const cpu = &on_cpu[pixel * 4u];
        bool differs = false;
        for (size_t channel = 0; channel < 3; channel++)
        {
            const int difference = (int)gpu[channel] - (int)cpu[channel];
            differs = differs || difference > CHANNEL_TOLERANCE || difference < -CHANNEL_TOLERANCE;
        }
        drawn += gpu[0] != 0 || gpu[1] != 0 || gpu[2] != 0 ? 1u : 0u;
        differing += differs ? 1u : 0u;
    }
    TB_TEST_CONTEXT("%zu of %zu drawn pixels differ", differing, drawn);
    TB_TEST_EXPECT_UINT(drawn, >, (size_t)width * height / 50u);
    TB_TEST_EXPECT_UINT(differing * 1000u, <=, (size_t)width * height * DIFFERING_PER_MILLE);
    tb_free(on_cpu);
    tb_free(on_gpu);
    tb_material_destroy(opened.renderer, material);
    tb_skinned_mesh_destroy(opened.renderer, cylinder);
    close_screen(&opened);
}

TB_TEST(skinning_frame, the_cpu_path_draws_what_the_gpu_path_draws_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    the_cpu_path_draws_what_the_gpu_path_draws_case();
}

TB_TEST(skinning_frame, the_cpu_path_draws_what_the_gpu_path_draws_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    the_cpu_path_draws_what_the_gpu_path_draws_case();
}
