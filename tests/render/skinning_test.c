/* Skinned meshes on the null backend, which every platform runs: frames
 * draw them in colors and with materials, beside plain meshes, skinned by
 * the GPU's pass or forced onto the CPU; a frame that skins more than its
 * first buffer holds, or whose joints or vertices fill its uploads, still
 * ends. And the CPU's skinning arithmetic, which needs no renderer. */
#include "badger/anim.h"
#include "badger/log.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/skinning.h"
#include "badger/window.h"

#include "tb_test.h"

#include <math.h>
#include <string.h>

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on the null backend, logging to `log` (NULL for
 * none), or a skipped case where there is no windowing. */
static void open_screen_logged(screen *opened, tb_log *log)
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
    options.log = log;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &opened->renderer, &error), ==, TB_RHI_OK);
}

static void open_screen(screen *opened)
{
    open_screen_logged(opened, NULL);
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* A strip of `rows` quads up +Y, each row bound to joint 0 below y = 1 and
 * to joint 1 above. */
static tb_skinned_mesh *make_strip(tb_renderer *renderer, uint32_t rows)
{
    const size_t vertex_count = ((size_t)rows + 1u) * 2u;
    const size_t index_count = (size_t)rows * 6u;
    tb_skinned_vertex *const vertices = tb_calloc(vertex_count, sizeof(*vertices));
    uint32_t *const indices = tb_calloc(index_count, sizeof(*indices));
    tb_skinned_mesh *mesh = NULL;

    for (uint32_t row = 0; row <= rows; row++)
    {
        const float y = 2.0f * (float)row / (float)rows;
        for (uint32_t side = 0; side < 2; side++)
        {
            tb_skinned_vertex *const vertex = &vertices[row * 2u + side];
            vertex->position = (tb_vec3){side == 0 ? -0.1f : 0.1f, y, 0};
            vertex->normal = (tb_vec3){0, 0, 1};
            vertex->uv = (tb_vec2){(float)side, y * 0.5f};
            vertex->joints[0] = y < 1.0f ? 0 : 1;
            vertex->weights[0] = 1.0f;
        }
    }
    for (uint32_t row = 0; row < rows; row++)
    {
        const uint32_t at = row * 2u;
        const uint32_t quad[6] = {at, at + 1u, at + 3u, at, at + 3u, at + 2u};
        for (uint32_t corner = 0; corner < 6; corner++)
        {
            indices[row * 6u + corner] = quad[corner];
        }
    }
    mesh = tb_skinned_mesh_create(renderer, vertices, vertex_count, indices, index_count);
    tb_free(indices);
    tb_free(vertices);
    return mesh;
}

static void begin(tb_renderer *renderer)
{
    tb_camera camera = tb_camera_perspective(tb_radians(60.0f));

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 1, 4}, (tb_vec3){0, 1, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
}

/* Skinned meshes draw in a color, lit or flat, and with a material, beside a
 * plain mesh, frame after frame; a frame that skins nothing skips the pass. */
TB_TEST(skinning_frame, skinned_meshes_draw_on_the_null_backend)
{
    screen opened = {0};
    tb_skinned_mesh *strip = NULL;
    tb_mesh *cube = NULL;
    tb_material *material = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    const tb_mat4 joints[2] = {tb_mat4_identity(),
                               tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, 0.5f))};

    open_screen(&opened);
    strip = make_strip(opened.renderer, 8);
    cube = tb_mesh_create_shape(opened.renderer, TB_MESH_CUBE, 0);
    material = tb_material_create(opened.renderer, &desc);
    for (int frame = 0; frame < 3; frame++)
    {
        begin(opened.renderer);
        tb_renderer_draw_mesh(opened.renderer, cube, tb_mat4_identity(), (tb_vec4){1, 1, 1, 1});
        tb_renderer_draw_skinned(opened.renderer,
                                 &(tb_skinned_draw){.mesh = strip,
                                                    .transform = tb_mat4_identity(),
                                                    .joints = joints,
                                                    .joint_count = 2,
                                                    .color = {1, 0, 0, 1}});
        tb_renderer_draw_skinned(opened.renderer,
                                 &(tb_skinned_draw){.mesh = strip,
                                                    .transform = tb_mat4_identity(),
                                                    .joints = joints,
                                                    .joint_count = 1,
                                                    .color = {0, 1, 0, 1},
                                                    .shading = TB_MESH_FLAT});
        tb_renderer_draw_skinned(opened.renderer,
                                 &(tb_skinned_draw){.mesh = strip,
                                                    .transform = tb_mat4_identity(),
                                                    .joints = joints,
                                                    .joint_count = 2,
                                                    .material = material});
        TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    }
    begin(opened.renderer);
    tb_renderer_draw_mesh(opened.renderer, cube, tb_mat4_identity(), (tb_vec4){1, 1, 1, 1});
    TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    tb_material_destroy(opened.renderer, material);
    tb_mesh_destroy(opened.renderer, cube);
    tb_skinned_mesh_destroy(opened.renderer, strip);
    tb_skinned_mesh_destroy(opened.renderer, NULL);
    close_screen(&opened);
}

#define MANY_JOINTS 65536u
#define MANY_DRAWS 80u
/* Rows of a strip whose vertices, skinned on the CPU, take 256 KiB of the
 * frame's uploads, so that MANY_DRAWS of them outgrow its 16 MiB. */
#define LARGE_STRIP_ROWS 4096u

/* A frame of many skinned draws, whose vertices outgrow the least buffer
 * and whose joints outgrow the frame's uploads, still ends: the draws past
 * the uploads are left out. Forced onto the CPU, whose skinned vertices go
 * into the uploads too, a frame that outgrows them likewise ends. */
TB_TEST(skinning_frame, a_frame_that_skins_a_lot_still_ends)
{
    screen opened = {0};
    tb_skinned_mesh *strip = NULL;
    tb_mat4 *joints = NULL;

    /* A case without windowing ends here, before anything is allocated. */
    open_screen(&opened);
    joints = tb_calloc(MANY_JOINTS, sizeof(*joints));
    for (size_t joint = 0; joint < MANY_JOINTS; joint++)
    {
        joints[joint] = tb_mat4_identity();
    }
    for (int path = 0; path < 2; path++)
    {
        tb_renderer_force_cpu_skinning(opened.renderer, path == 1);
        strip = make_strip(opened.renderer, path == 1 ? LARGE_STRIP_ROWS : 512u);
        begin(opened.renderer);
        for (uint32_t draw = 0; draw < MANY_DRAWS; draw++)
        {
            tb_renderer_draw_skinned(
                opened.renderer,
                &(tb_skinned_draw){.mesh = strip,
                                   .transform = tb_mat4_translation((tb_vec3){(float)draw, 0, 0}),
                                   .joints = joints,
                                   .joint_count = draw % 16u == 0 ? MANY_JOINTS : 2u,
                                   .color = {1, 1, 1, 1}});
        }
        TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
        tb_skinned_mesh_destroy(opened.renderer, strip);
    }
    tb_free(joints);
    close_screen(&opened);
}

/* Records the renderer's messages about where it skins. */
typedef struct skinning_messages
{
    size_t count;
    char last[128];
} skinning_messages;

static bool keep_skinning_message(void *context, const tb_log_record *record)
{
    static const char prefix[] = "skinning on the ";
    skinning_messages *const messages = context;
    const size_t prefix_length = sizeof(prefix) - 1u;

    if (record->length >= prefix_length && memcmp(record->message, prefix, prefix_length) == 0)
    {
        const size_t kept =
            record->length < sizeof(messages->last) ? record->length : sizeof(messages->last) - 1u;
        memcpy(messages->last, record->message, kept);
        messages->last[kept] = '\0';
        messages->count++;
    }
    return true;
}

static void draw_strip(tb_renderer *renderer, const tb_skinned_mesh *strip, const tb_mat4 *joints,
                       const tb_material *material)
{
    tb_renderer_draw_skinned(renderer, &(tb_skinned_draw){.mesh = strip,
                                                          .transform = tb_mat4_identity(),
                                                          .joints = joints,
                                                          .joint_count = 2,
                                                          .material = material,
                                                          .color = {1, 0, 0, 1}});
}

/* The null backend has bindless resources, so the GPU skins, and the
 * renderer says so once, when it is made. Forcing the CPU takes effect at
 * the next frame, not inside the one being drawn, and is logged once; skinned
 * draws in a color and with a material draw on either path; and unforcing
 * goes back to the GPU. */
TB_TEST(skinning_frame, the_cpu_path_is_reported_logged_and_forced)
{
    screen opened = {0};
    tb_log *const log = tb_log_create();
    skinning_messages messages = {0};
    tb_log_config config = {TB_LOG_INFO, "renderer", keep_skinning_message, NULL, &messages};
    tb_skinned_mesh *strip = NULL;
    tb_material *material = NULL;
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;
    tb_skinning_info info = {0};
    const tb_mat4 joints[2] = {tb_mat4_identity(),
                               tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, 0.5f))};

    TB_TEST_REQUIRE_UINT(tb_log_configure(log, &config), ==, TB_LOG_OK);
    open_screen_logged(&opened, log);
    TB_TEST_EXPECT_UINT(messages.count, ==, 1);
    TB_TEST_EXPECT_STRING(messages.last, ==, "skinning on the GPU: a compute pass");
    tb_renderer_skinning_info(opened.renderer, &info);
    TB_TEST_EXPECT_UINT(info.path, ==, TB_SKINNING_GPU);
    TB_TEST_EXPECT_STRING(info.reason, ==, "a compute pass");
    strip = make_strip(opened.renderer, 8);
    material = tb_material_create(opened.renderer, &desc);

    begin(opened.renderer);
    tb_renderer_force_cpu_skinning(opened.renderer, true);
    tb_renderer_skinning_info(opened.renderer, &info);
    TB_TEST_EXPECT_UINT(info.path, ==, TB_SKINNING_GPU);
    draw_strip(opened.renderer, strip, joints, NULL);
    TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    tb_renderer_skinning_info(opened.renderer, &info);
    TB_TEST_EXPECT_UINT(info.path, ==, TB_SKINNING_CPU);
    TB_TEST_EXPECT_STRING(info.reason, ==, "forced");
    TB_TEST_EXPECT_UINT(messages.count, ==, 1);

    for (int frame = 0; frame < 3; frame++)
    {
        begin(opened.renderer);
        tb_renderer_skinning_info(opened.renderer, &info);
        TB_TEST_EXPECT_UINT(info.path, ==, TB_SKINNING_CPU);
        draw_strip(opened.renderer, strip, joints, NULL);
        draw_strip(opened.renderer, strip, joints, material);
        TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    }
    TB_TEST_EXPECT_UINT(messages.count, ==, 2);
    TB_TEST_EXPECT_STRING(messages.last, ==, "skinning on the CPU: forced");

    tb_renderer_force_cpu_skinning(opened.renderer, false);
    begin(opened.renderer);
    tb_renderer_skinning_info(opened.renderer, &info);
    TB_TEST_EXPECT_UINT(info.path, ==, TB_SKINNING_GPU);
    draw_strip(opened.renderer, strip, joints, material);
    TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    TB_TEST_EXPECT_UINT(messages.count, ==, 3);
    TB_TEST_EXPECT_STRING(messages.last, ==, "skinning on the GPU: a compute pass");

    tb_material_destroy(opened.renderer, material);
    tb_skinned_mesh_destroy(opened.renderer, strip);
    close_screen(&opened);
    (void)tb_log_destroy(log);
}

/* What the animation module makes, the renderer draws: an arm's skinning
 * matrices from a sampled clip. */
TB_TEST(skinning_frame, an_animated_arm_draws)
{
    static const float times[2] = {0.0f, 1.0f};
    screen opened = {0};
    tb_skeleton_joint bones[2] = {
        {.name = "shoulder", .parent = TB_SKELETON_NO_PARENT, .rest = TB_ANIM_TRANSFORM_INIT},
        {.name = "elbow", .parent = 0, .rest = TB_ANIM_TRANSFORM_INIT}};
    const tb_quat bent = tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, 1.0f);
    const float values[8] = {0, 0, 0, 1, bent.x, bent.y, bent.z, bent.w};
    const tb_anim_channel bend = {
        .joint = 1, .path = TB_ANIM_ROTATION, .times = times, .values = values, .key_count = 2};
    tb_skeleton *skeleton = NULL;
    tb_anim_clip *clip = NULL;
    tb_skinned_mesh *strip = NULL;
    tb_anim_transform local[2] = {0};
    tb_mat4 model[2] = {0};
    tb_mat4 skinning[2] = {0};

    open_screen(&opened);
    bones[1].rest.translation = (tb_vec3){0, 1, 0};
    skeleton = tb_skeleton_create(&(tb_skeleton_desc){bones, 2, NULL});
    clip = tb_anim_clip_create(&(tb_anim_clip_desc){.channels = &bend, .channel_count = 1});
    TB_TEST_REQUIRE(skeleton != NULL && clip != NULL);
    strip = make_strip(opened.renderer, 4);
    for (int frame = 0; frame < 2; frame++)
    {
        const tb_anim_layer layer = {clip, (float)frame * 0.5f, TB_ANIM_LOOP, 1.0f};
        const tb_anim_instance instance = {skeleton, &layer, 1, local, model, skinning};
        tb_anim_evaluate(&instance);
        begin(opened.renderer);
        tb_renderer_draw_skinned(opened.renderer,
                                 &(tb_skinned_draw){.mesh = strip,
                                                    .transform = tb_mat4_identity(),
                                                    .joints = skinning,
                                                    .joint_count = 2,
                                                    .color = {1, 1, 1, 1}});
        TB_TEST_EXPECT_UINT(tb_renderer_end_frame(opened.renderer), ==, TB_RHI_OK);
    }
    tb_skinned_mesh_destroy(opened.renderer, strip);
    tb_anim_clip_destroy(clip);
    tb_skeleton_destroy(skeleton);
    close_screen(&opened);
}

/* ---------------------------------------------------------------------------
 * The CPU's skinning arithmetic.
 * ------------------------------------------------------------------------- */

#define NEAR 1e-5f

static void expect_vec3_near(tb_vec3 actual, tb_vec3 expected)
{
    TB_TEST_EXPECT_FLOAT_NEAR(actual.x, expected.x, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(actual.y, expected.y, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(actual.z, expected.z, NEAR);
}

/* A vertex bound wholly to one joint. */
static tb_skinned_vertex bound_to(uint16_t joint, tb_vec3 position, tb_vec3 normal)
{
    tb_skinned_vertex vertex = {0};

    vertex.position = position;
    vertex.normal = normal;
    vertex.uv = (tb_vec2){0.25f, 0.75f};
    vertex.joints[0] = joint;
    vertex.weights[0] = 1.0f;
    return vertex;
}

/* Identity matrices leave a vertex as it was bound, its texture coordinates
 * with it. */
TB_TEST(skin_vertices, identity_joints_leave_vertices_as_bound)
{
    const tb_mat4 joints[1] = {tb_mat4_identity()};
    const tb_skinned_vertex vertex = bound_to(0, (tb_vec3){1, 2, 3}, (tb_vec3){0, 1, 0});
    tb_mesh_vertex skinned = {0};

    tb_skin_vertices(&vertex, 1, joints, 1, &skinned);
    expect_vec3_near(skinned.position, (tb_vec3){1, 2, 3});
    expect_vec3_near(skinned.normal, (tb_vec3){0, 1, 0});
    TB_TEST_EXPECT_FLOAT_NEAR(skinned.uv.x, 0.25f, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(skinned.uv.y, 0.75f, NEAR);
}

/* One joint turns and moves its vertex: the position as a point, the normal
 * as a direction, which the translation does not move. */
TB_TEST(skin_vertices, one_joint_turns_and_moves_a_vertex)
{
    const tb_mat4 joints[2] = {
        tb_mat4_identity(),
        tb_mat4_mul(tb_mat4_translation((tb_vec3){10, 0, 0}),
                    tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, TB_PI * 0.5f)))};
    const tb_skinned_vertex vertex = bound_to(1, (tb_vec3){1, 0, 0}, (tb_vec3){1, 0, 0});
    tb_mesh_vertex skinned = {0};

    tb_skin_vertices(&vertex, 1, joints, 2, &skinned);
    expect_vec3_near(skinned.position, (tb_vec3){10, 1, 0});
    expect_vec3_near(skinned.normal, (tb_vec3){0, 1, 0});
}

/* Two joints by half each: the position halfway between where each takes
 * it, and the normal halfway between, normalized. */
TB_TEST(skin_vertices, two_joints_blend_by_their_weights)
{
    const tb_mat4 joints[2] = {
        tb_mat4_translation((tb_vec3){2, 0, 0}),
        tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, TB_PI * 0.5f))};
    tb_skinned_vertex vertex = bound_to(0, (tb_vec3){1, 0, 0}, (tb_vec3){1, 0, 0});
    tb_mesh_vertex skinned = {0};
    const float half_root = sqrtf(0.5f);

    vertex.joints[1] = 1;
    vertex.weights[0] = 0.5f;
    vertex.weights[1] = 0.5f;
    tb_skin_vertices(&vertex, 1, joints, 2, &skinned);
    /* (3, 0, 0) by the first, (0, 1, 0) by the second. */
    expect_vec3_near(skinned.position, (tb_vec3){1.5f, 0.5f, 0});
    expect_vec3_near(skinned.normal, (tb_vec3){half_root, half_root, 0});
}

/* Weights are used as they are, as the GPU's shader uses them: weights
 * summing to a half pull a vertex halfway to the origin, and to two push it
 * twice as far, while its normal stays a unit. */
TB_TEST(skin_vertices, weights_that_do_not_sum_to_one_scale_the_vertex)
{
    const tb_mat4 joints[1] = {tb_mat4_identity()};
    tb_skinned_vertex vertices[2] = {bound_to(0, (tb_vec3){2, 4, 6}, (tb_vec3){0, 0, 1}),
                                     bound_to(0, (tb_vec3){2, 4, 6}, (tb_vec3){0, 0, 1})};
    tb_mesh_vertex skinned[2] = {0};

    vertices[0].weights[0] = 0.5f;
    vertices[1].weights[0] = 1.5f;
    vertices[1].weights[1] = 0.5f;
    tb_skin_vertices(vertices, 2, joints, 1, skinned);
    expect_vec3_near(skinned[0].position, (tb_vec3){1, 2, 3});
    expect_vec3_near(skinned[0].normal, (tb_vec3){0, 0, 1});
    expect_vec3_near(skinned[1].position, (tb_vec3){4, 8, 12});
    expect_vec3_near(skinned[1].normal, (tb_vec3){0, 0, 1});
}

/* A joint index past the draw's matrices takes the last, as on the GPU, so
 * no vertex reads past them. */
TB_TEST(skin_vertices, a_joint_past_the_matrices_takes_the_last)
{
    const tb_mat4 joints[2] = {tb_mat4_identity(), tb_mat4_translation((tb_vec3){0, 5, 0})};
    const tb_skinned_vertex vertex = bound_to(9, (tb_vec3){1, 0, 0}, (tb_vec3){0, 1, 0});
    tb_mesh_vertex skinned = {0};

    tb_skin_vertices(&vertex, 1, joints, 2, &skinned);
    expect_vec3_near(skinned.position, (tb_vec3){1, 5, 0});
}

/* Weights of nothing take every position to the origin, and keep the normal
 * as it was bound rather than dividing by zero. */
TB_TEST(skin_vertices, a_normal_summed_to_nothing_is_kept)
{
    const tb_mat4 joints[1] = {tb_mat4_identity()};
    tb_skinned_vertex vertex = bound_to(0, (tb_vec3){1, 2, 3}, (tb_vec3){0, 1, 0});
    tb_mesh_vertex skinned = {0};

    vertex.weights[0] = 0.0f;
    tb_skin_vertices(&vertex, 1, joints, 1, &skinned);
    expect_vec3_near(skinned.position, (tb_vec3){0, 0, 0});
    expect_vec3_near(skinned.normal, (tb_vec3){0, 1, 0});
}

/* A vertex is skinned by the matrix tb_anim_skin_matrix() blends, which the
 * animation module documents as the GPU's; and skinning a mesh in two
 * ranges, as threads would, gives what skinning it at once does. */
TB_TEST(skin_vertices, ranges_skin_as_the_whole_and_as_the_blended_matrix)
{
    const tb_mat4 joints[3] = {
        tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, 0.3f)),
        tb_mat4_translation((tb_vec3){0, 1, -2}),
        tb_mat4_mul(tb_mat4_translation((tb_vec3){1, 0, 0}),
                    tb_mat4_rotation(tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, 1.1f)))};
    tb_skinned_vertex vertices[5] = {0};
    tb_mesh_vertex whole[5] = {0};
    tb_mesh_vertex split[5] = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(vertices); index++)
    {
        const float at = (float)index;
        vertices[index] = bound_to(0, (tb_vec3){at, 1.0f - at, 0.5f * at}, (tb_vec3){0, 0, 1});
        vertices[index].joints[1] = 1;
        vertices[index].joints[2] = 2;
        vertices[index].weights[0] = 0.2f;
        vertices[index].weights[1] = 0.3f;
        vertices[index].weights[2] = 0.5f;
    }
    tb_skin_vertices(vertices, 5, joints, 3, whole);
    tb_skin_vertices(vertices, 2, joints, 3, split);
    tb_skin_vertices(&vertices[2], 3, joints, 3, &split[2]);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(vertices); index++)
    {
        const tb_mat4 matrix =
            tb_anim_skin_matrix(joints, 3, vertices[index].joints, vertices[index].weights);
        TB_TEST_CONTEXT("vertex %zu", index);
        expect_vec3_near(whole[index].position,
                         tb_mat4_transform_point(matrix, vertices[index].position));
        expect_vec3_near(split[index].position, whole[index].position);
        expect_vec3_near(split[index].normal, whole[index].normal);
    }
}

TB_TEST(skin_vertices, paths_have_names)
{
    TB_TEST_EXPECT_STRING(tb_skinning_path_name(TB_SKINNING_GPU), ==, "GPU");
    TB_TEST_EXPECT_STRING(tb_skinning_path_name(TB_SKINNING_CPU), ==, "CPU");
    TB_TEST_EXPECT_STRING(tb_skinning_path_name((tb_skinning_path)99), ==, "unknown");
}
