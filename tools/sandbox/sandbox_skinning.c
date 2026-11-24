/* The skinning scene: a row of tentacles, each a tapered cylinder skinned to
 * a chain of joints, played by two clips made in code (a wave and a curl)
 * whose blend drifts over time, evaluated on the job system and skinned on
 * the GPU, with their bones drawn over them. */
#include "sandbox.h"

#include "badger/anim.h"
#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/jobs.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/skinning.h"

#include <math.h>
#include <string.h>

/* Tentacles in the row, the joints of each, and the length of a bone. */
#define TENTACLES 5u
#define TENTACLE_JOINTS 7u
#define BONE_LENGTH 0.42f
/* The tentacle's rings for each bone, segments around, and its radius at
 * the root and at the tip. */
#define RINGS_PER_BONE 4u
#define SEGMENTS 20u
#define ROOT_RADIUS 0.26f
#define TIP_RADIUS 0.06f
/* Keys of each joint's channel in each clip, and the clips' durations. */
#define CLIP_KEYS 17u
#define WAVE_SECONDS 2.0f
#define CURL_SECONDS 3.0f
/* Pixels a side of the checkerboard of the textured tentacle. */
#define CHECKER_SIZE 64u

typedef struct skinning_scene
{
    tb_jobs *jobs;
    tb_skeleton *skeleton;
    tb_anim_clip *wave;
    tb_anim_clip *curl;
    tb_skinned_mesh *mesh;
    tb_mesh *floor;
    tb_texture *checker;
    tb_material *checkered;
    tb_anim_layer layers[TENTACLES][2];
    tb_anim_instance instances[TENTACLES];
    tb_anim_transform local[TENTACLES][TENTACLE_JOINTS];
    tb_mat4 model[TENTACLES][TENTACLE_JOINTS];
    tb_mat4 skinning[TENTACLES][TENTACLE_JOINTS];
} skinning_scene;

/* A chain up +Y: a root at the floor, each joint a bone above its parent. */
static tb_skeleton *skinning_skeleton(void)
{
    static const char *const names[TENTACLE_JOINTS] = {"root", "j1", "j2", "j3", "j4", "j5", "tip"};
    tb_skeleton_joint joints[TENTACLE_JOINTS] = {0};

    for (uint32_t joint = 0; joint < TENTACLE_JOINTS; joint++)
    {
        joints[joint] =
            (tb_skeleton_joint){.name = names[joint],
                                .parent = joint == 0 ? TB_SKELETON_NO_PARENT : joint - 1u,
                                .rest = TB_ANIM_TRANSFORM_INIT};
        joints[joint].rest.translation = (tb_vec3){0, joint == 0 ? 0.0f : BONE_LENGTH, 0};
    }
    return tb_skeleton_create(&(tb_skeleton_desc){joints, TENTACLE_JOINTS, NULL});
}

/* A clip of every joint but the root turning about an axis, by an angle
 * that `angle(joint, phase)` gives at each of its keys, `phase` from 0 to 1
 * over the duration. */
static tb_anim_clip *skinning_clip(const char *name, float duration, tb_vec3 axis,
                                   float (*angle)(uint32_t joint, float phase))
{
    float times[CLIP_KEYS] = {0};
    float values[TENTACLE_JOINTS][CLIP_KEYS * 4u] = {0};
    tb_anim_channel channels[TENTACLE_JOINTS] = {0};

    for (uint32_t key = 0; key < CLIP_KEYS; key++)
    {
        times[key] = duration * (float)key / (float)(CLIP_KEYS - 1u);
    }
    for (uint32_t joint = 1; joint < TENTACLE_JOINTS; joint++)
    {
        for (uint32_t key = 0; key < CLIP_KEYS; key++)
        {
            const float phase = (float)key / (float)(CLIP_KEYS - 1u);
            const tb_quat turn = tb_quat_from_axis_angle(axis, angle(joint, phase));
            values[joint][key * 4u] = turn.x;
            values[joint][key * 4u + 1u] = turn.y;
            values[joint][key * 4u + 2u] = turn.z;
            values[joint][key * 4u + 3u] = turn.w;
        }
        channels[joint - 1u] = (tb_anim_channel){.joint = joint,
                                                 .path = TB_ANIM_ROTATION,
                                                 .times = times,
                                                 .values = values[joint],
                                                 .key_count = CLIP_KEYS};
    }
    return tb_anim_clip_create(&(tb_anim_clip_desc){
        .channels = channels, .channel_count = TENTACLE_JOINTS - 1u, .name = name});
}

/* A wave that runs up the tentacle: each joint a little behind the one
 * below it, so that the whole sways like a whip. */
static float skinning_wave_angle(uint32_t joint, float phase)
{
    return 0.32f * sinf(TB_TAU * phase - 0.7f * (float)joint);
}

/* A curl toward the viewer and back, tighter toward the tip. */
static float skinning_curl_angle(uint32_t joint, float phase)
{
    const float rise = sinf(TB_PI * phase);
    return 0.2f * (float)joint * rise * rise;
}

/* A tapered cylinder up the chain, each ring bound to the two joints it is
 * between, by how near it is to each, and a point at its tip. */
static tb_skinned_mesh *skinning_mesh(tb_renderer *renderer)
{
    enum
    {
        RINGS = (TENTACLE_JOINTS - 1u) * RINGS_PER_BONE + 1u,
        RING_VERTICES = SEGMENTS + 1u,
        VERTEX_COUNT = RINGS * RING_VERTICES + 1u,
        INDEX_COUNT = (RINGS - 1u) * SEGMENTS * 6u + SEGMENTS * 3u
    };
    tb_skinned_vertex *const vertices = tb_calloc(VERTEX_COUNT, sizeof(*vertices));
    uint32_t *const indices = tb_calloc(INDEX_COUNT, sizeof(*indices));
    const uint32_t tip = VERTEX_COUNT - 1u;
    const float length = BONE_LENGTH * (float)(TENTACLE_JOINTS - 1u);
    size_t index_count = 0;
    tb_skinned_mesh *mesh = NULL;

    for (uint32_t ring = 0; ring < RINGS; ring++)
    {
        const uint32_t bone = ring / RINGS_PER_BONE;
        const float along = (float)(ring % RINGS_PER_BONE) / (float)RINGS_PER_BONE;
        const float y = BONE_LENGTH * (float)ring / (float)RINGS_PER_BONE;
        const float radius = tb_lerp(ROOT_RADIUS, TIP_RADIUS, y / length);
        for (uint32_t segment = 0; segment < RING_VERTICES; segment++)
        {
            const float angle = TB_TAU * (float)segment / (float)SEGMENTS;
            tb_skinned_vertex *const vertex = &vertices[ring * RING_VERTICES + segment];
            vertex->position = (tb_vec3){radius * sinf(angle), y, radius * cosf(angle)};
            vertex->normal = (tb_vec3){sinf(angle), 0, cosf(angle)};
            vertex->uv = (tb_vec2){(float)segment / (float)SEGMENTS, y / length * 4.0f};
            vertex->joints[0] = (uint16_t)bone;
            vertex->joints[1] = (uint16_t)(bone + 1u < TENTACLE_JOINTS ? bone + 1u : bone);
            vertex->weights[0] = 1.0f - along;
            vertex->weights[1] = along;
        }
    }
    vertices[tip] = (tb_skinned_vertex){.position = {0, length + TIP_RADIUS, 0},
                                        .normal = {0, 1, 0},
                                        .uv = {0.5f, 4.0f},
                                        .joints = {TENTACLE_JOINTS - 1u},
                                        .weights = {1.0f}};
    for (uint32_t ring = 0; ring + 1u < RINGS; ring++)
    {
        for (uint32_t segment = 0; segment < SEGMENTS; segment++)
        {
            const uint32_t below = ring * RING_VERTICES + segment;
            const uint32_t above = below + RING_VERTICES;
            const uint32_t quad[6] = {below, below + 1u, above + 1u, below, above + 1u, above};
            memcpy(&indices[index_count], quad, sizeof(quad));
            index_count += 6u;
        }
    }
    for (uint32_t segment = 0; segment < SEGMENTS; segment++)
    {
        const uint32_t top = (RINGS - 1u) * RING_VERTICES + segment;
        const uint32_t cap[3] = {top, top + 1u, tip};
        memcpy(&indices[index_count], cap, sizeof(cap));
        index_count += 3u;
    }
    mesh = tb_skinned_mesh_create(renderer, vertices, VERTEX_COUNT, indices, index_count);
    tb_free(indices);
    tb_free(vertices);
    return mesh;
}

bool sandbox_skinning_create(sandbox *box, void **state)
{
    skinning_scene *const scene = tb_calloc(1, sizeof(*scene));
    uint8_t *const pixels = tb_malloc((size_t)CHECKER_SIZE * CHECKER_SIZE * 4u);
    tb_material_desc desc = TB_MATERIAL_DESC_INIT;

    *state = scene;
    scene->jobs = tb_jobs_create(NULL);
    scene->skeleton = skinning_skeleton();
    scene->wave = skinning_clip("wave", WAVE_SECONDS, (tb_vec3){0, 0, 1}, skinning_wave_angle);
    scene->curl = skinning_clip("curl", CURL_SECONDS, (tb_vec3){1, 0, 0.6f}, skinning_curl_angle);
    scene->mesh = skinning_mesh(box->renderer);
    scene->floor = tb_mesh_create_shape(box->renderer, TB_MESH_PLANE, 0);
    tb_image_fill_checkerboard(pixels, (size_t)CHECKER_SIZE * CHECKER_SIZE * 4u, CHECKER_SIZE,
                               CHECKER_SIZE, 8, (tb_vec4){0.95f, 0.85f, 0.4f, 1},
                               (tb_vec4){0.3f, 0.15f, 0.1f, 1});
    scene->checker = tb_texture_create(
        box->renderer,
        &(tb_texture_desc){.width = CHECKER_SIZE, .height = CHECKER_SIZE, .name = "tentacle"},
        pixels, (size_t)CHECKER_SIZE * CHECKER_SIZE * 4u);
    tb_free(pixels);
    desc.base_color = scene->checker;
    desc.roughness = 0.5f;
    scene->checkered = tb_material_create(box->renderer, &desc);
    for (uint32_t tentacle = 0; tentacle < TENTACLES; tentacle++)
    {
        scene->instances[tentacle] = (tb_anim_instance){.skeleton = scene->skeleton,
                                                        .layers = scene->layers[tentacle],
                                                        .layer_count = 2,
                                                        .local = scene->local[tentacle],
                                                        .model = scene->model[tentacle],
                                                        .skinning = scene->skinning[tentacle]};
    }
    return scene->skeleton != NULL && scene->wave != NULL && scene->curl != NULL;
}

/* Where a tentacle stands. */
static tb_mat4 skinning_place(uint32_t tentacle)
{
    const float x = ((float)tentacle - (float)(TENTACLES - 1u) * 0.5f) * 1.9f;

    return tb_mat4_translation((tb_vec3){x, 0, 0});
}

/* Each bone as a line from its joint to the next, the last to the tip, and
 * each joint as a small sphere, over everything. */
static void skinning_draw_bones(sandbox *box, const tb_mat4 *model, tb_mat4 place)
{
    static const tb_vec4 bone_color = {1.0f, 0.95f, 0.3f, 1.0f};

    for (uint32_t joint = 0; joint < TENTACLE_JOINTS; joint++)
    {
        const tb_mat4 world = tb_mat4_mul(place, model[joint]);
        const tb_vec3 at = tb_mat4_transform_point(world, (tb_vec3){0, 0, 0});
        const tb_vec3 next =
            joint + 1u < TENTACLE_JOINTS
                ? tb_mat4_transform_point(tb_mat4_mul(place, model[joint + 1u]), (tb_vec3){0, 0, 0})
                : tb_mat4_transform_point(world, (tb_vec3){0, BONE_LENGTH * 0.6f, 0});
        tb_debug_line(box->renderer, at, next, bone_color, TB_DEBUG_ON_TOP);
        tb_debug_sphere(box->renderer, at, 0.035f, bone_color, TB_DEBUG_ON_TOP);
    }
}

void sandbox_skinning_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    static const tb_vec4 colors[TENTACLES] = {{0.85f, 0.35f, 0.40f, 1},
                                              {0.35f, 0.70f, 0.85f, 1},
                                              {1, 1, 1, 1},
                                              {0.55f, 0.80f, 0.40f, 1},
                                              {0.80f, 0.50f, 0.90f, 1}};
    skinning_scene *const scene = state;
    const float seconds = (float)frame->seconds;

    for (uint32_t tentacle = 0; tentacle < TENTACLES; tentacle++)
    {
        /* Each drifts between the clips at its own phase along the row. */
        const float curl = 0.5f + 0.5f * sinf(0.9f * seconds - 0.9f * (float)tentacle);
        const float offset = 0.37f * (float)tentacle;
        scene->layers[tentacle][0] =
            (tb_anim_layer){scene->wave, seconds + offset, TB_ANIM_LOOP, 1.0f - curl};
        scene->layers[tentacle][1] =
            (tb_anim_layer){scene->curl, seconds + offset, TB_ANIM_LOOP, curl};
    }
    tb_anim_evaluate_many(scene->jobs, scene->instances, TENTACLES);
    tb_renderer_draw_mesh(
        box->renderer, scene->floor,
        tb_mat4_from_trs((tb_vec3){0, -0.01f, 0}, tb_quat_identity(), (tb_vec3){24, 1, 24}),
        (tb_vec4){0.22f, 0.23f, 0.25f, 1});
    for (uint32_t tentacle = 0; tentacle < TENTACLES; tentacle++)
    {
        const tb_mat4 place = skinning_place(tentacle);
        const float curl = scene->layers[tentacle][1].weight;
        char label[32] = {0};
        const size_t length = tb_fmt_buffer(label, sizeof(label), "curl %d%%",

                                            (int)lroundf(curl * 100.0f));
        tb_renderer_draw_skinned(
            box->renderer,
            &(tb_skinned_draw){.mesh = scene->mesh,
                               .transform = place,
                               .joints = scene->skinning[tentacle],
                               .joint_count = TENTACLE_JOINTS,
                               .material = tentacle == TENTACLES / 2u ? scene->checkered : NULL,
                               .color = colors[tentacle]});
        skinning_draw_bones(box, scene->model[tentacle], place);
        tb_debug_text(box->renderer, tb_mat4_transform_point(place, (tb_vec3){0, -0.2f, 0.6f}),
                      tb_text_make(label, length < sizeof(label) ? length : sizeof(label) - 1u),
                      (tb_vec4){0.9f, 0.9f, 0.9f, 1});
    }
    tb_debug_grid(box->renderer, (tb_vec3){0, 0, 0}, 24.0f, 24, (tb_vec4){0.35f, 0.36f, 0.4f, 1},
                  0);
}

void sandbox_skinning_destroy(sandbox *box, void *state)
{
    skinning_scene *const scene = state;

    tb_material_destroy(box->renderer, scene->checkered);
    tb_texture_destroy(box->renderer, scene->checker);
    tb_mesh_destroy(box->renderer, scene->floor);
    tb_skinned_mesh_destroy(box->renderer, scene->mesh);
    tb_anim_clip_destroy(scene->curl);
    tb_anim_clip_destroy(scene->wave);
    tb_skeleton_destroy(scene->skeleton);
    tb_jobs_destroy(scene->jobs);
    tb_free(scene);
}
