/* Skeletal animation on the CPU: clips sample between their keys, wrap and
 * clamp, blend by weights that are normalized, add differences, accumulate
 * down the hierarchy, and skin a vertex where the joints take it; and many
 * instances evaluated on the job system match the same evaluated one at a
 * time. Descriptions that break a rule make nothing. */
#include "badger/anim.h"

#include "badger/jobs.h"
#include "badger/memory.h"

#include "tb_test.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TOLERANCE 1e-5f
#define ARM_JOINTS 2u

static void expect_vec3(tb_vec3 actual, tb_vec3 expected)
{
    TB_TEST_EXPECT_FLOAT_NEAR(actual.x, expected.x, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(actual.y, expected.y, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(actual.z, expected.z, TOLERANCE);
}

/* The same rotation, whichever sign each quaternion has. */
static void expect_rotation(tb_quat actual, tb_quat expected)
{
    TB_TEST_EXPECT_FLOAT_NEAR(fabsf(tb_quat_dot(actual, expected)), 1.0f, TOLERANCE);
}

static tb_quat turn_about_z(float angle)
{
    return tb_quat_from_axis_angle((tb_vec3){0, 0, 1}, angle);
}

/* An arm of two joints along +Y: a shoulder at the origin, and an elbow one
 * unit up. */
static tb_skeleton *make_arm(void)
{
    tb_skeleton_joint joints[ARM_JOINTS] = {
        {.name = "shoulder", .parent = TB_SKELETON_NO_PARENT, .rest = TB_ANIM_TRANSFORM_INIT},
        {.name = "elbow", .parent = 0, .rest = TB_ANIM_TRANSFORM_INIT}};

    joints[1].rest.translation = (tb_vec3){0, 1, 0};
    return tb_skeleton_create(&(tb_skeleton_desc){joints, ARM_JOINTS, NULL});
}

/* The elbow bends a quarter turn about Z over a second, its shoulder rises
 * from 0 to 2 over two seconds, and its scale steps at one second. */
static tb_anim_clip *make_bend(tb_anim_interpolation rise)
{
    static const float bend_times[2] = {0.0f, 1.0f};
    static const float rise_times[2] = {0.0f, 2.0f};
    static const float scale_times[2] = {0.0f, 1.0f};
    static const float rise_values[6] = {0, 0, 0, 0, 2, 0};
    static const float scale_values[6] = {1, 1, 1, 2, 2, 2};
    const tb_quat bent = turn_about_z(TB_PI * 0.5f);
    const float bend_values[8] = {0, 0, 0, 1, bent.x, bent.y, bent.z, bent.w};
    const tb_anim_channel channels[3] = {{.joint = 1,
                                          .path = TB_ANIM_ROTATION,
                                          .times = bend_times,
                                          .values = bend_values,
                                          .key_count = 2},
                                         {.joint = 0,
                                          .path = TB_ANIM_TRANSLATION,
                                          .interpolation = rise,
                                          .times = rise_times,
                                          .values = rise_values,
                                          .key_count = 2},
                                         {.joint = 1,
                                          .path = TB_ANIM_SCALE,
                                          .interpolation = TB_ANIM_STEP,
                                          .times = scale_times,
                                          .values = scale_values,
                                          .key_count = 2}};

    return tb_anim_clip_create(
        &(tb_anim_clip_desc){.channels = channels, .channel_count = 3, .name = "bend"});
}

/* Halfway between keys, a translation is halfway, a rotation has turned
 * half the angle, and a step holds its earlier key. */
TB_TEST(anim_clip, sampling_between_keys_blends_linearly_and_steps)
{
    tb_skeleton *const arm = make_arm();
    tb_anim_clip *const bend = make_bend(TB_ANIM_LINEAR);
    tb_anim_transform pose[ARM_JOINTS] = {0};

    TB_TEST_REQUIRE(arm != NULL && bend != NULL);
    TB_TEST_EXPECT_STRING(tb_anim_clip_name(bend), ==, "bend");
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_duration(bend), ==, 2.0f);
    tb_anim_clip_sample(bend, arm, 0.5f, TB_ANIM_CLAMP, pose, ARM_JOINTS);
    expect_vec3(pose[0].translation, (tb_vec3){0, 0.5f, 0});
    expect_rotation(pose[1].rotation, turn_about_z(TB_PI * 0.25f));
    expect_vec3(pose[1].scale, (tb_vec3){1, 1, 1});
    /* The elbow's rest translation stays: no channel moves it. */
    expect_vec3(pose[1].translation, (tb_vec3){0, 1, 0});
    tb_anim_clip_sample(bend, arm, 1.5f, TB_ANIM_CLAMP, pose, ARM_JOINTS);
    expect_vec3(pose[0].translation, (tb_vec3){0, 1.5f, 0});
    /* Past its last key, a channel holds it. */
    expect_rotation(pose[1].rotation, turn_about_z(TB_PI * 0.5f));
    expect_vec3(pose[1].scale, (tb_vec3){2, 2, 2});
    tb_anim_clip_destroy(bend);
    tb_skeleton_destroy(arm);
}

/* A step channel holds each key until the next, and its last after. */
TB_TEST(anim_clip, a_step_channel_holds_each_key)
{
    tb_skeleton *const arm = make_arm();
    tb_anim_clip *const bend = make_bend(TB_ANIM_STEP);
    tb_anim_transform pose[ARM_JOINTS] = {0};

    TB_TEST_REQUIRE(arm != NULL && bend != NULL);
    tb_anim_clip_sample(bend, arm, 1.9f, TB_ANIM_CLAMP, pose, ARM_JOINTS);
    expect_vec3(pose[0].translation, (tb_vec3){0, 0, 0});
    tb_anim_clip_sample(bend, arm, 2.0f, TB_ANIM_CLAMP, pose, ARM_JOINTS);
    expect_vec3(pose[0].translation, (tb_vec3){0, 2, 0});
    tb_anim_clip_destroy(bend);
    tb_skeleton_destroy(arm);
}

/* Looping wraps forward and back into [0, duration); clamping stops at
 * either end; a clip of no duration is always at 0. */
TB_TEST(anim_clip, time_loops_and_clamps)
{
    static const float times[1] = {0.0f};
    static const float values[3] = {1, 2, 3};
    const tb_anim_channel still = {
        .path = TB_ANIM_TRANSLATION, .times = times, .values = values, .key_count = 1};
    tb_anim_clip *const bend = make_bend(TB_ANIM_LINEAR);
    tb_anim_clip *const held =
        tb_anim_clip_create(&(tb_anim_clip_desc){.channels = &still, .channel_count = 1});
    tb_anim_clip *const longer = tb_anim_clip_create(
        &(tb_anim_clip_desc){.channels = &still, .channel_count = 1, .duration = 3.0f});

    TB_TEST_REQUIRE(bend != NULL && held != NULL && longer != NULL);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_anim_clip_time(bend, 2.5f, TB_ANIM_LOOP), 0.5f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_anim_clip_time(bend, 4.0f, TB_ANIM_LOOP), 0.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_anim_clip_time(bend, -0.5f, TB_ANIM_LOOP), 1.5f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_anim_clip_time(bend, 7.25f, TB_ANIM_LOOP), 1.25f, TOLERANCE);
    /* A negative time too small to tell from 0 wraps to 0, not to the end. */
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_time(bend, -1e-9f, TB_ANIM_LOOP), <, 2.0f);
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_time(bend, 2.5f, TB_ANIM_CLAMP), ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_time(bend, -1.0f, TB_ANIM_CLAMP), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_duration(held), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_time(held, 5.0f, TB_ANIM_LOOP), ==, 0.0f);
    TB_TEST_EXPECT_POINTER(tb_anim_clip_name(held), ==, NULL);
    TB_TEST_EXPECT_FLOAT(tb_anim_clip_duration(longer), ==, 3.0f);
    tb_anim_clip_destroy(longer);
    tb_anim_clip_destroy(held);
    tb_anim_clip_destroy(bend);
}

/* A clip whose channels break a rule makes nothing, so a loader of bad
 * data gets an answer instead of a clip that reads out of bounds. */
TB_TEST(anim_clip, a_description_that_breaks_a_rule_makes_nothing)
{
    static const float increasing[2] = {0.0f, 1.0f};
    static const float repeated[2] = {1.0f, 1.0f};
    static const float values[8] = {0, 0, 0, 1, 0, 0, 0, 1};
    const float not_finite[2] = {0.0f, INFINITY};
    const tb_anim_channel good = {
        .path = TB_ANIM_ROTATION, .times = increasing, .values = values, .key_count = 2};
    tb_anim_channel channels[2] = {good, good};

    TB_TEST_EXPECT_POINTER(tb_anim_clip_create(&(tb_anim_clip_desc){.channel_count = 1}), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_anim_clip_create(&(tb_anim_clip_desc){
                               .channels = &good, .channel_count = 1, .duration = -1.0f}),
                           ==, NULL);
    /* Two rotations of one joint. */
    TB_TEST_EXPECT_POINTER(
        tb_anim_clip_create(&(tb_anim_clip_desc){.channels = channels, .channel_count = 2}), ==,
        NULL);
    channels[1].times = repeated;
    TB_TEST_EXPECT_POINTER(
        tb_anim_clip_create(&(tb_anim_clip_desc){.channels = &channels[1], .channel_count = 1}), ==,
        NULL);
    channels[1].times = not_finite;
    TB_TEST_EXPECT_POINTER(
        tb_anim_clip_create(&(tb_anim_clip_desc){.channels = &channels[1], .channel_count = 1}), ==,
        NULL);
    channels[1] = good;
    channels[1].key_count = 0;
    TB_TEST_EXPECT_POINTER(
        tb_anim_clip_create(&(tb_anim_clip_desc){.channels = &channels[1], .channel_count = 1}), ==,
        NULL);
    channels[1] = good;
    channels[1].joint = TB_SKELETON_MAX_JOINTS;
    TB_TEST_EXPECT_POINTER(
        tb_anim_clip_create(&(tb_anim_clip_desc){.channels = channels, .channel_count = 2}), ==,
        NULL);
    /* A clip of no channels is a clip that moves nothing. */
    tb_anim_clip_destroy(tb_anim_clip_create(&(tb_anim_clip_desc){0}));
    tb_anim_clip_destroy(NULL);
}

/* Weights are divided by their sum, so 1 and 3 blend a quarter and three
 * quarters; weights of sum 0 give the first pose. */
TB_TEST(anim_blend, weights_are_normalized)
{
    tb_anim_transform a[1] = {TB_ANIM_TRANSFORM_INIT};
    tb_anim_transform b[1] = {TB_ANIM_TRANSFORM_INIT};
    const tb_anim_transform *const poses[2] = {a, b};
    tb_anim_transform result[1] = {0};

    b[0].translation = (tb_vec3){4, 0, 0};
    b[0].scale = (tb_vec3){3, 3, 3};
    b[0].rotation = turn_about_z(TB_PI * 0.5f);
    tb_anim_blend(poses, (const float[2]){1.0f, 3.0f}, 2, result, 1);
    expect_vec3(result[0].translation, (tb_vec3){3, 0, 0});
    expect_vec3(result[0].scale, (tb_vec3){2.5f, 2.5f, 2.5f});
    /* The normalized blend of rotations lies between them. */
    TB_TEST_EXPECT_FLOAT_NEAR(tb_quat_dot(result[0].rotation, result[0].rotation), 1.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT(tb_quat_dot(result[0].rotation, b[0].rotation), >,
                         tb_quat_dot(result[0].rotation, a[0].rotation));
    tb_anim_blend(poses, (const float[2]){0.0f, 0.0f}, 2, result, 1);
    expect_vec3(result[0].translation, (tb_vec3){0, 0, 0});
    /* Equal weights of the same pose are that pose. */
    tb_anim_blend(poses, (const float[2]){0.0f, 2.0f}, 2, result, 1);
    expect_vec3(result[0].translation, (tb_vec3){4, 0, 0});
    expect_rotation(result[0].rotation, b[0].rotation);
}

/* q and −q are one rotation: blended, they give it back, not a quaternion
 * of length 0. */
TB_TEST(anim_blend, opposite_signs_of_one_rotation_blend_to_it)
{
    const tb_quat turned = turn_about_z(1.0f);
    tb_anim_transform a[1] = {TB_ANIM_TRANSFORM_INIT};
    tb_anim_transform b[1] = {TB_ANIM_TRANSFORM_INIT};
    const tb_anim_transform *const poses[2] = {a, b};
    tb_anim_transform result[1] = {0};

    a[0].rotation = turned;
    b[0].rotation = (tb_quat){-turned.x, -turned.y, -turned.z, -turned.w};
    tb_anim_blend(poses, (const float[2]){0.5f, 0.5f}, 2, result, 1);
    expect_rotation(result[0].rotation, turned);
    /* The result may be the first pose itself. */
    tb_anim_blend(poses, (const float[2]){0.5f, 0.5f}, 2, a, 1);
    expect_rotation(a[0].rotation, turned);
}

/* An additive pose adds its difference from the reference: none at weight
 * 0, all of it at 1, half of a turn at a half. */
TB_TEST(anim_blend, an_additive_pose_adds_its_difference)
{
    tb_anim_transform base[1] = {TB_ANIM_TRANSFORM_INIT};
    tb_anim_transform reference[1] = {TB_ANIM_TRANSFORM_INIT};
    tb_anim_transform additive[1] = {TB_ANIM_TRANSFORM_INIT};
    tb_anim_transform result[1] = {0};

    base[0].translation = (tb_vec3){1, 0, 0};
    base[0].rotation = turn_about_z(0.5f);
    base[0].scale = (tb_vec3){2, 2, 2};
    reference[0].translation = (tb_vec3){0, 1, 0};
    reference[0].scale = (tb_vec3){2, 2, 2};
    additive[0].translation = (tb_vec3){0, 3, 0};
    additive[0].rotation = turn_about_z(1.0f);
    additive[0].scale = (tb_vec3){4, 4, 4};
    tb_anim_add(base, additive, reference, 0.0f, result, 1);
    expect_vec3(result[0].translation, base[0].translation);
    expect_rotation(result[0].rotation, base[0].rotation);
    expect_vec3(result[0].scale, base[0].scale);
    tb_anim_add(base, additive, reference, 1.0f, result, 1);
    expect_vec3(result[0].translation, (tb_vec3){1, 2, 0});
    expect_rotation(result[0].rotation, turn_about_z(1.5f));
    expect_vec3(result[0].scale, (tb_vec3){4, 4, 4});
    tb_anim_add(base, additive, reference, 0.5f, base, 1);
    expect_rotation(base[0].rotation, turn_about_z(1.0f));
}

/* A joint's model matrix is its parent's times its own: the elbow, one up
 * from a shoulder turned a quarter about Z, is one to the left. At rest the
 * skinning matrices are the identity: the mesh is where it was bound. */
TB_TEST(anim_hierarchy, model_matrices_accumulate_down_the_hierarchy)
{
    tb_skeleton *const arm = make_arm();
    tb_anim_transform pose[ARM_JOINTS] = {TB_ANIM_TRANSFORM_INIT, TB_ANIM_TRANSFORM_INIT};
    tb_mat4 model[ARM_JOINTS] = {0};
    tb_mat4 skinning[ARM_JOINTS] = {0};

    TB_TEST_REQUIRE(arm != NULL);
    tb_anim_model_pose(arm, tb_skeleton_rest_pose(arm), model, ARM_JOINTS);
    tb_anim_skinning_matrices(arm, model, skinning, ARM_JOINTS);
    for (size_t joint = 0; joint < ARM_JOINTS; joint++)
    {
        expect_vec3(tb_mat4_transform_point(skinning[joint], (tb_vec3){0.3f, 0.7f, -0.2f}),
                    (tb_vec3){0.3f, 0.7f, -0.2f});
    }
    pose[0].rotation = turn_about_z(TB_PI * 0.5f);
    pose[1].translation = (tb_vec3){0, 1, 0};
    tb_anim_model_pose(arm, pose, model, ARM_JOINTS);
    expect_vec3(tb_mat4_transform_point(model[1], (tb_vec3){0, 0, 0}), (tb_vec3){-1, 0, 0});
    expect_vec3(tb_mat4_transform_point(model[1], (tb_vec3){0, 1, 0}), (tb_vec3){-2, 0, 0});
    tb_skeleton_destroy(arm);
}

/* A vertex skinned on the CPU lands where its joints take it: bound to the
 * elbow alone, the tip of the arm turns with it; weighed half and half
 * between shoulder and elbow, it lands halfway. A joint past the matrices
 * takes the last. */
TB_TEST(anim_skinning, a_vertex_lands_where_its_joints_take_it)
{
    tb_skeleton *const arm = make_arm();
    tb_anim_transform pose[ARM_JOINTS] = {TB_ANIM_TRANSFORM_INIT, TB_ANIM_TRANSFORM_INIT};
    tb_mat4 model[ARM_JOINTS] = {0};
    tb_mat4 skinning[ARM_JOINTS] = {0};
    const tb_vec3 tip = {0, 2, 0};

    TB_TEST_REQUIRE(arm != NULL);
    pose[1].translation = (tb_vec3){0, 1, 0};
    pose[1].rotation = turn_about_z(-TB_PI * 0.5f);
    tb_anim_model_pose(arm, pose, model, ARM_JOINTS);
    tb_anim_skinning_matrices(arm, model, skinning, ARM_JOINTS);
    expect_vec3(tb_mat4_transform_point(tb_anim_skin_matrix(skinning, ARM_JOINTS,
                                                            (const uint16_t[4]){1, 0, 0, 0},
                                                            (const float[4]){1, 0, 0, 0}),
                                        tip),
                (tb_vec3){1, 1, 0});
    expect_vec3(tb_mat4_transform_point(tb_anim_skin_matrix(skinning, ARM_JOINTS,
                                                            (const uint16_t[4]){0, 1, 0, 0},
                                                            (const float[4]){0.5f, 0.5f, 0, 0}),
                                        tip),
                (tb_vec3){0.5f, 1.5f, 0});
    expect_vec3(tb_mat4_transform_point(tb_anim_skin_matrix(skinning, ARM_JOINTS,
                                                            (const uint16_t[4]){9, 0, 0, 0},
                                                            (const float[4]){1, 0, 0, 0}),
                                        tip),
                (tb_vec3){1, 1, 0});
    tb_skeleton_destroy(arm);
}

/* An instance's layers blend as tb_anim_blend() blends their samples, and
 * its skinning matrices are its model matrices times the inverse binds. */
TB_TEST(anim_instance, evaluating_blends_layers_and_skins)
{
    tb_skeleton *const arm = make_arm();
    tb_anim_clip *const bend = make_bend(TB_ANIM_LINEAR);
    tb_anim_transform first[ARM_JOINTS] = {0};
    tb_anim_transform second[ARM_JOINTS] = {0};
    tb_anim_transform expected[ARM_JOINTS] = {0};
    const tb_anim_transform *const poses[2] = {first, second};
    tb_anim_transform local[ARM_JOINTS] = {0};
    tb_mat4 model[ARM_JOINTS] = {0};
    tb_mat4 skinning[ARM_JOINTS] = {0};
    const tb_anim_layer layers[3] = {{bend, 0.25f, TB_ANIM_LOOP, 1.0f},
                                     {bend, 1.0f, TB_ANIM_LOOP, 2.0f},
                                     {bend, 1.5f, TB_ANIM_LOOP, 0.0f}};
    tb_anim_instance instance = {arm, layers, 3, local, model, skinning};

    TB_TEST_REQUIRE(arm != NULL && bend != NULL);
    tb_anim_clip_sample(bend, arm, 0.25f, TB_ANIM_LOOP, first, ARM_JOINTS);
    tb_anim_clip_sample(bend, arm, 1.0f, TB_ANIM_LOOP, second, ARM_JOINTS);
    tb_anim_blend(poses, (const float[2]){1.0f, 2.0f}, 2, expected, ARM_JOINTS);
    tb_anim_evaluate(&instance);
    for (size_t joint = 0; joint < ARM_JOINTS; joint++)
    {
        expect_vec3(local[joint].translation, expected[joint].translation);
        expect_rotation(local[joint].rotation, expected[joint].rotation);
        expect_vec3(local[joint].scale, expected[joint].scale);
    }
    expect_vec3(tb_mat4_transform_point(skinning[1], (tb_vec3){0, 1, 0}),
                tb_mat4_transform_point(model[1], (tb_vec3){0, 0, 0}));
    /* No layers, or no weight, is the rest pose. */
    instance.layer_count = 0;
    instance.skinning = NULL;
    tb_anim_evaluate(&instance);
    expect_vec3(local[1].translation, (tb_vec3){0, 1, 0});
    instance.layers = &layers[2];
    instance.layer_count = 1;
    tb_anim_evaluate(&instance);
    expect_rotation(local[1].rotation, tb_quat_identity());
    tb_anim_clip_destroy(bend);
    tb_skeleton_destroy(arm);
}

#define CROWD 64u

/* Many instances evaluated on the job system give what each gives
 * evaluated alone, bit for bit: each is the same arithmetic on one thread. */
TB_TEST(anim_instance, many_instances_on_the_job_system_match_one_at_a_time)
{
    tb_skeleton *const arm = make_arm();
    tb_anim_clip *const bend = make_bend(TB_ANIM_LINEAR);
    tb_jobs *const jobs = tb_jobs_create(&(tb_jobs_options){.worker_count = 3});
    tb_anim_layer *const layers = tb_calloc(CROWD, sizeof(*layers));
    tb_anim_instance *const instances = tb_calloc(CROWD, sizeof(*instances));
    tb_anim_transform *const local = tb_calloc(CROWD * ARM_JOINTS, sizeof(*local));
    tb_mat4 *const model = tb_calloc(CROWD * ARM_JOINTS, sizeof(*model));
    tb_mat4 *const skinning = tb_calloc(CROWD * ARM_JOINTS, sizeof(*skinning));
    tb_anim_transform alone_local[ARM_JOINTS] = {0};
    tb_mat4 alone_model[ARM_JOINTS] = {0};
    tb_mat4 alone_skinning[ARM_JOINTS] = {0};
    size_t different = 0;

    TB_TEST_REQUIRE(arm != NULL && bend != NULL);
    for (size_t index = 0; index < CROWD; index++)
    {
        layers[index] = (tb_anim_layer){bend, (float)index * 0.1f, TB_ANIM_LOOP, 1.0f};
        instances[index] = (tb_anim_instance){arm,
                                              &layers[index],
                                              1,
                                              &local[index * ARM_JOINTS],
                                              &model[index * ARM_JOINTS],
                                              &skinning[index * ARM_JOINTS]};
    }
    tb_anim_evaluate_many(jobs, instances, CROWD);
    for (size_t index = 0; index < CROWD; index++)
    {
        const tb_anim_instance alone = {arm,         &layers[index], 1,
                                        alone_local, alone_model,    alone_skinning};
        tb_anim_evaluate(&alone);
        different +=
            memcmp(alone_skinning, &skinning[index * ARM_JOINTS], sizeof(alone_skinning)) != 0 ? 1u
                                                                                               : 0u;
    }
    TB_TEST_EXPECT_UINT(different, ==, 0);
    /* Without a job system, on the calling thread. */
    tb_anim_evaluate_many(NULL, instances, 1);
    tb_anim_evaluate_many(jobs, NULL, 0);
    tb_free(skinning);
    tb_free(model);
    tb_free(local);
    tb_free(instances);
    tb_free(layers);
    tb_jobs_destroy(jobs);
    tb_anim_clip_destroy(bend);
    tb_skeleton_destroy(arm);
}

/* A skeleton keeps its joints' names and parents, and inverse bind
 * matrices it is given. */
TB_TEST(anim_skeleton, a_skeleton_keeps_its_joints)
{
    tb_skeleton_joint joints[3] = {
        {.name = "root", .parent = TB_SKELETON_NO_PARENT, .rest = TB_ANIM_TRANSFORM_INIT},
        {.parent = 0, .rest = TB_ANIM_TRANSFORM_INIT},
        {.name = "tip", .parent = 1, .rest = TB_ANIM_TRANSFORM_INIT}};
    tb_mat4 inverse_bind[3] = {0};
    tb_skeleton *skeleton = NULL;

    for (size_t joint = 0; joint < 3; joint++)
    {
        inverse_bind[joint] = tb_mat4_translation((tb_vec3){0, -(float)joint, 0});
    }
    skeleton = tb_skeleton_create(&(tb_skeleton_desc){joints, 3, inverse_bind});
    TB_TEST_REQUIRE(skeleton != NULL);
    TB_TEST_EXPECT_UINT(tb_skeleton_joint_count(skeleton), ==, 3);
    TB_TEST_EXPECT_UINT(tb_skeleton_parent(skeleton, 0), ==, TB_SKELETON_NO_PARENT);
    TB_TEST_EXPECT_UINT(tb_skeleton_parent(skeleton, 2), ==, 1);
    TB_TEST_EXPECT_STRING(tb_skeleton_joint_name(skeleton, 0), ==, "root");
    TB_TEST_EXPECT_POINTER(tb_skeleton_joint_name(skeleton, 1), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_skeleton_find(skeleton, "tip"), ==, 2);
    TB_TEST_EXPECT_UINT(tb_skeleton_find(skeleton, "elbow"), ==, TB_SKELETON_NO_JOINT);
    TB_TEST_EXPECT_FLOAT(tb_skeleton_inverse_bind(skeleton)[2].columns[3].y, ==, -2.0f);
    tb_skeleton_destroy(skeleton);
    tb_skeleton_destroy(NULL);
}

/* A skeleton that breaks a rule makes nothing: no joints, a parent after its
 * child, or a rest pose of scale 0, which has no inverse bind. */
TB_TEST(anim_skeleton, a_description_that_breaks_a_rule_makes_nothing)
{
    tb_skeleton_joint joints[2] = {
        {.parent = 1, .rest = TB_ANIM_TRANSFORM_INIT},
        {.parent = TB_SKELETON_NO_PARENT, .rest = TB_ANIM_TRANSFORM_INIT}};

    TB_TEST_EXPECT_POINTER(tb_skeleton_create(&(tb_skeleton_desc){joints, 0, NULL}), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_skeleton_create(&(tb_skeleton_desc){NULL, 2, NULL}), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_skeleton_create(&(tb_skeleton_desc){joints, 2, NULL}), ==, NULL);
    joints[0].parent = TB_SKELETON_NO_PARENT;
    joints[1].parent = 0;
    joints[1].rest.scale = (tb_vec3){0, 1, 1};
    TB_TEST_EXPECT_POINTER(tb_skeleton_create(&(tb_skeleton_desc){joints, 2, NULL}), ==, NULL);
}
