/* Skinned meshes on the null backend, which every platform runs: frames
 * draw them in colors and with materials, beside plain meshes; a frame that
 * skins more than its first buffer holds, or whose joints fill its uploads,
 * still ends. */
#include "badger/anim.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/skinning.h"
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
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "skinning", .width = 160, .height = 120, .fixed_size = true},
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

/* A frame of many skinned draws, whose vertices outgrow the least buffer
 * and whose joints outgrow the frame's uploads, still ends: the draws past
 * the uploads are left out. */
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
    strip = make_strip(opened.renderer, 512);
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
    tb_free(joints);
    close_screen(&opened);
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
