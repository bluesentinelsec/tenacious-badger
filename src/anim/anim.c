/* Skeletal animation on the CPU: skeletons, clips of keyframes, sampling,
 * blending, the hierarchy of a pose, and skinning matrices. */
#include "badger/anim.h"

#include "badger/assert.h"
#include "badger/jobs.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* What a joint's entry in a clip's table holds for a path no channel moves. */
#define TB_ANIM_NO_TRACK UINT32_MAX
/* Paths a joint has: translation, rotation and scale. */
#define TB_ANIM_PATHS 3u
/* Floats of a key of a rotation, and of a translation or a scale. */
#define TB_ANIM_ROTATION_FLOATS 4u
#define TB_ANIM_VECTOR_FLOATS 3u

struct tb_skeleton
{
    size_t joint_count;
    uint32_t *parents;
    char **names; /* NULL for a joint without one */
    tb_anim_transform *rest;
    tb_mat4 *inverse_bind;
};

/* One channel's keys, as the clip keeps them. */
typedef struct tb_anim_track
{
    tb_anim_path path;
    tb_anim_interpolation interpolation;
    size_t key_count;
    float *times;
    float *values; /* tb_anim_floats(path) a key */
} tb_anim_track;

struct tb_anim_clip
{
    char *name;
    float duration;
    tb_anim_track *tracks;
    size_t track_count;
    /* For each joint up to the highest a channel moves, the track of each
     * path: joint * TB_ANIM_PATHS + path. */
    uint32_t *table;
    size_t table_joints;
};

/* A blend of transforms being summed, joint by joint. */
typedef struct tb_anim_sum
{
    tb_vec3 translation;
    tb_quat rotation;
    tb_vec3 scale;
} tb_anim_sum;

/* ---------------------------------------------------------------------------
 * Transforms.
 * ------------------------------------------------------------------------- */

tb_mat4 tb_anim_transform_matrix(tb_anim_transform transform)
{
    return tb_mat4_from_trs(transform.translation, transform.rotation, transform.scale);
}

static size_t tb_anim_floats(tb_anim_path path)
{
    return path == TB_ANIM_ROTATION ? TB_ANIM_ROTATION_FLOATS : TB_ANIM_VECTOR_FLOATS;
}

static void tb_anim_sum_add(tb_anim_sum *sum, const tb_anim_transform *transform, float weight)
{
    tb_quat rotation = transform->rotation;

    /* q and −q are the same rotation; summed, they would cancel. Each is
     * brought into the hemisphere of the sum so far, which therefore only
     * grows, and is never 0 once a rotation of length 1 has been added. */
    if (tb_quat_dot(sum->rotation, rotation) < 0.0f)
    {
        rotation = (tb_quat){-rotation.x, -rotation.y, -rotation.z, -rotation.w};
    }
    sum->translation = tb_vec3_add(sum->translation, tb_vec3_scale(transform->translation, weight));
    sum->scale = tb_vec3_add(sum->scale, tb_vec3_scale(transform->scale, weight));
    sum->rotation.x += rotation.x * weight;
    sum->rotation.y += rotation.y * weight;
    sum->rotation.z += rotation.z * weight;
    sum->rotation.w += rotation.w * weight;
}

static tb_anim_transform tb_anim_sum_result(const tb_anim_sum *sum)
{
    return (tb_anim_transform){sum->translation, tb_quat_normalize(sum->rotation), sum->scale};
}

/* ---------------------------------------------------------------------------
 * Skeletons.
 * ------------------------------------------------------------------------- */

static bool tb_skeleton_desc_valid(const tb_skeleton_desc *desc)
{
    if (desc->joints == NULL || desc->joint_count == 0 ||
        desc->joint_count > TB_SKELETON_MAX_JOINTS)
    {
        return false;
    }
    for (size_t joint = 0; joint < desc->joint_count; joint++)
    {
        const uint32_t parent = desc->joints[joint].parent;
        if (parent != TB_SKELETON_NO_PARENT && parent >= joint)
        {
            return false;
        }
    }
    return true;
}

tb_skeleton *tb_skeleton_create(const tb_skeleton_desc *desc)
{
    tb_skeleton *skeleton = NULL;
    tb_mat4 *model = NULL;

    TB_ASSERT(desc != NULL);
    if (!tb_skeleton_desc_valid(desc))
    {
        return NULL;
    }
    skeleton = tb_calloc(1, sizeof(*skeleton));
    skeleton->joint_count = desc->joint_count;
    skeleton->parents = tb_calloc(desc->joint_count, sizeof(*skeleton->parents));
    skeleton->names = tb_calloc(desc->joint_count, sizeof(*skeleton->names));
    skeleton->rest = tb_calloc(desc->joint_count, sizeof(*skeleton->rest));
    skeleton->inverse_bind = tb_calloc(desc->joint_count, sizeof(*skeleton->inverse_bind));
    for (size_t joint = 0; joint < desc->joint_count; joint++)
    {
        const tb_skeleton_joint *const given = &desc->joints[joint];
        skeleton->parents[joint] = given->parent;
        skeleton->names[joint] = given->name != NULL ? tb_str_dup(NULL, given->name) : NULL;
        skeleton->rest[joint] = given->rest;
    }
    if (desc->inverse_bind != NULL)
    {
        memcpy(skeleton->inverse_bind, desc->inverse_bind,
               desc->joint_count * sizeof(*skeleton->inverse_bind));
        return skeleton;
    }
    model = tb_calloc(desc->joint_count, sizeof(*model));
    tb_anim_model_pose(skeleton, skeleton->rest, model, desc->joint_count);
    for (size_t joint = 0; joint < desc->joint_count; joint++)
    {
        if (!tb_mat4_inverse(model[joint], &skeleton->inverse_bind[joint]))
        {
            tb_skeleton_destroy(skeleton);
            skeleton = NULL;
            goto done;
        }
    }
done:
    tb_free(model);
    return skeleton;
}

void tb_skeleton_destroy(tb_skeleton *skeleton)
{
    if (skeleton == NULL)
    {
        return;
    }
    for (size_t joint = 0; joint < skeleton->joint_count; joint++)
    {
        tb_free(skeleton->names[joint]);
    }
    tb_free(skeleton->names);
    tb_free(skeleton->parents);
    tb_free(skeleton->rest);
    tb_free(skeleton->inverse_bind);
    tb_free(skeleton);
}

size_t tb_skeleton_joint_count(const tb_skeleton *skeleton)
{
    TB_ASSERT(skeleton != NULL);
    return skeleton->joint_count;
}

uint32_t tb_skeleton_parent(const tb_skeleton *skeleton, size_t joint)
{
    TB_ASSERT(skeleton != NULL && joint < skeleton->joint_count);
    return skeleton->parents[joint];
}

const char *tb_skeleton_joint_name(const tb_skeleton *skeleton, size_t joint)
{
    TB_ASSERT(skeleton != NULL && joint < skeleton->joint_count);
    return skeleton->names[joint];
}

uint32_t tb_skeleton_find(const tb_skeleton *skeleton, const char *name)
{
    TB_ASSERT(skeleton != NULL && name != NULL);
    for (size_t joint = 0; joint < skeleton->joint_count; joint++)
    {
        if (skeleton->names[joint] != NULL && tb_str_equal(skeleton->names[joint], name))
        {
            return (uint32_t)joint;
        }
    }
    return TB_SKELETON_NO_JOINT;
}

const tb_anim_transform *tb_skeleton_rest_pose(const tb_skeleton *skeleton)
{
    TB_ASSERT(skeleton != NULL);
    return skeleton->rest;
}

const tb_mat4 *tb_skeleton_inverse_bind(const tb_skeleton *skeleton)
{
    TB_ASSERT(skeleton != NULL);
    return skeleton->inverse_bind;
}

/* ---------------------------------------------------------------------------
 * Clips.
 * ------------------------------------------------------------------------- */

static bool tb_anim_channel_valid(const tb_anim_channel *channel)
{
    if (channel->times == NULL || channel->values == NULL || channel->key_count == 0 ||
        channel->joint >= TB_SKELETON_MAX_JOINTS || (unsigned)channel->path > TB_ANIM_SCALE ||
        (unsigned)channel->interpolation > TB_ANIM_STEP)
    {
        return false;
    }
    for (size_t key = 0; key < channel->key_count; key++)
    {
        if (!isfinite(channel->times[key]))
        {
            return false;
        }
        if (key > 0 && !(channel->times[key] > channel->times[key - 1]))
        {
            return false;
        }
    }
    return true;
}

/* Each joint's entry of each path, before any channel is entered. */
static void tb_anim_clip_clear_table(tb_anim_clip *clip)
{
    clip->table = tb_calloc(clip->table_joints * TB_ANIM_PATHS, sizeof(*clip->table));
    for (size_t entry = 0; entry < clip->table_joints * TB_ANIM_PATHS; entry++)
    {
        clip->table[entry] = TB_ANIM_NO_TRACK;
    }
}

static void tb_anim_track_copy(tb_anim_track *track, const tb_anim_channel *channel)
{
    const size_t floats = tb_anim_floats(channel->path);

    track->path = channel->path;
    track->interpolation = channel->interpolation;
    track->key_count = channel->key_count;
    track->times = tb_calloc(channel->key_count, sizeof(*track->times));
    track->values = tb_calloc(channel->key_count, floats * sizeof(*track->values));
    memcpy(track->times, channel->times, channel->key_count * sizeof(*track->times));
    memcpy(track->values, channel->values, channel->key_count * floats * sizeof(*track->values));
    if (channel->path != TB_ANIM_ROTATION)
    {
        return;
    }
    /* Rotations are kept of length 1, which slerp needs. */
    for (size_t key = 0; key < channel->key_count; key++)
    {
        float *const value = &track->values[key * TB_ANIM_ROTATION_FLOATS];
        const tb_quat rotation =
            tb_quat_normalize((tb_quat){value[0], value[1], value[2], value[3]});
        value[0] = rotation.x;
        value[1] = rotation.y;
        value[2] = rotation.z;
        value[3] = rotation.w;
    }
}

tb_anim_clip *tb_anim_clip_create(const tb_anim_clip_desc *desc)
{
    tb_anim_clip *clip = NULL;
    float last_key = 0.0f;

    TB_ASSERT(desc != NULL);
    if ((desc->channels == NULL && desc->channel_count > 0) || !(desc->duration >= 0.0f) ||
        !isfinite(desc->duration))
    {
        return NULL;
    }
    clip = tb_calloc(1, sizeof(*clip));
    clip->name = desc->name != NULL ? tb_str_dup(NULL, desc->name) : NULL;
    clip->tracks = tb_calloc(desc->channel_count, sizeof(*clip->tracks));
    for (size_t index = 0; index < desc->channel_count; index++)
    {
        const tb_anim_channel *const channel = &desc->channels[index];
        if (!tb_anim_channel_valid(channel))
        {
            goto invalid;
        }
        tb_anim_track_copy(&clip->tracks[index], channel);
        clip->track_count++;
        if (channel->joint + (size_t)1 > clip->table_joints)
        {
            clip->table_joints = channel->joint + (size_t)1;
        }
        last_key = fmaxf(last_key, channel->times[channel->key_count - 1]);
    }
    tb_anim_clip_clear_table(clip);
    for (size_t index = 0; index < desc->channel_count; index++)
    {
        const tb_anim_channel *const channel = &desc->channels[index];
        uint32_t *const entry =
            &clip->table[(size_t)channel->joint * TB_ANIM_PATHS + (size_t)channel->path];
        if (*entry != TB_ANIM_NO_TRACK)
        {
            goto invalid;
        }
        *entry = (uint32_t)index;
    }
    clip->duration = desc->duration > 0.0f ? desc->duration : fmaxf(last_key, 0.0f);
    return clip;

invalid:
    tb_anim_clip_destroy(clip);
    return NULL;
}

void tb_anim_clip_destroy(tb_anim_clip *clip)
{
    if (clip == NULL)
    {
        return;
    }
    for (size_t track = 0; track < clip->track_count; track++)
    {
        tb_free(clip->tracks[track].times);
        tb_free(clip->tracks[track].values);
    }
    tb_free(clip->tracks);
    tb_free(clip->table);
    tb_free(clip->name);
    tb_free(clip);
}

float tb_anim_clip_duration(const tb_anim_clip *clip)
{
    TB_ASSERT(clip != NULL);
    return clip->duration;
}

const char *tb_anim_clip_name(const tb_anim_clip *clip)
{
    TB_ASSERT(clip != NULL);
    return clip->name;
}

float tb_anim_clip_time(const tb_anim_clip *clip, float time, tb_anim_wrap wrap)
{
    float wrapped = 0.0f;

    TB_ASSERT(clip != NULL);
    if (clip->duration <= 0.0f)
    {
        return 0.0f;
    }
    if (wrap == TB_ANIM_CLAMP)
    {
        return tb_clamp(time, 0.0f, clip->duration);
    }
    wrapped = fmodf(time, clip->duration);
    if (wrapped < 0.0f)
    {
        wrapped += clip->duration;
    }
    /* A tiny negative time wraps to the duration itself, by rounding. */
    return wrapped < clip->duration ? wrapped : 0.0f;
}

/* The last key at or before a time, which is after the first key and
 * before the last. */
static size_t tb_anim_key_before(const tb_anim_track *track, float time)
{
    size_t low = 0;
    size_t high = track->key_count - 1;

    while (high - low > 1)
    {
        const size_t middle = low + (high - low) / 2;
        if (track->times[middle] <= time)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

/* A track's value at a time: four floats for a rotation, three otherwise. */
static void tb_anim_track_sample(const tb_anim_track *track, float time, float value[4])
{
    const size_t floats = tb_anim_floats(track->path);
    const size_t last = track->key_count - 1;
    size_t key = 0;
    float t = 0.0f;
    const float *from = NULL;
    const float *to = NULL;

    if (time <= track->times[0] || last == 0)
    {
        memcpy(value, track->values, floats * sizeof(float));
        return;
    }
    if (time >= track->times[last])
    {
        memcpy(value, &track->values[last * floats], floats * sizeof(float));
        return;
    }
    key = tb_anim_key_before(track, time);
    from = &track->values[key * floats];
    if (track->interpolation == TB_ANIM_STEP)
    {
        memcpy(value, from, floats * sizeof(float));
        return;
    }
    to = &track->values[(key + 1) * floats];
    t = (time - track->times[key]) / (track->times[key + 1] - track->times[key]);
    if (track->path == TB_ANIM_ROTATION)
    {
        const tb_quat blended = tb_quat_slerp((tb_quat){from[0], from[1], from[2], from[3]},
                                              (tb_quat){to[0], to[1], to[2], to[3]}, t);
        value[0] = blended.x;
        value[1] = blended.y;
        value[2] = blended.z;
        value[3] = blended.w;
        return;
    }
    for (size_t index = 0; index < floats; index++)
    {
        value[index] = tb_lerp(from[index], to[index], t);
    }
}

/* A joint's transform at a time within the clip: its rest transform, with
 * each path a channel moves sampled. */
static tb_anim_transform tb_anim_clip_joint(const tb_anim_clip *clip, size_t joint, float time,
                                            tb_anim_transform rest)
{
    tb_anim_transform result = rest;
    float value[4] = {0};

    if (joint >= clip->table_joints)
    {
        return result;
    }
    for (uint32_t path = 0; path < TB_ANIM_PATHS; path++)
    {
        const uint32_t track = clip->table[joint * TB_ANIM_PATHS + path];
        if (track == TB_ANIM_NO_TRACK)
        {
            continue;
        }
        tb_anim_track_sample(&clip->tracks[track], time, value);
        if (path == TB_ANIM_TRANSLATION)
        {
            result.translation = (tb_vec3){value[0], value[1], value[2]};
        }
        else if (path == TB_ANIM_ROTATION)
        {
            result.rotation = (tb_quat){value[0], value[1], value[2], value[3]};
        }
        else
        {
            result.scale = (tb_vec3){value[0], value[1], value[2]};
        }
    }
    return result;
}

void tb_anim_clip_sample(const tb_anim_clip *clip, const tb_skeleton *skeleton, float time,
                         tb_anim_wrap wrap, tb_anim_transform *pose, size_t pose_count)
{
    const float within = tb_anim_clip_time(clip, time, wrap);

    TB_ASSERT(skeleton != NULL && pose != NULL);
    TB_ASSERT_MSG(pose_count == skeleton->joint_count, "a pose of each joint of the skeleton");
    for (size_t joint = 0; joint < pose_count; joint++)
    {
        pose[joint] = tb_anim_clip_joint(clip, joint, within, skeleton->rest[joint]);
    }
}

/* ---------------------------------------------------------------------------
 * Blending.
 * ------------------------------------------------------------------------- */

static float tb_anim_weight_sum(const float *weights, size_t count)
{
    float sum = 0.0f;

    for (size_t index = 0; index < count; index++)
    {
        TB_ASSERT_MSG(weights[index] >= 0.0f, "weights are not negative");
        sum += weights[index];
    }
    return sum;
}

void tb_anim_blend(const tb_anim_transform *const *poses, const float *weights, size_t pose_count,
                   tb_anim_transform *result, size_t joint_count)
{
    float total = 0.0f;

    TB_ASSERT(poses != NULL && weights != NULL && result != NULL && pose_count > 0);
    total = tb_anim_weight_sum(weights, pose_count);
    for (size_t joint = 0; joint < joint_count; joint++)
    {
        tb_anim_sum blend = {0};
        if (total <= 0.0f)
        {
            result[joint] = poses[0][joint];
            continue;
        }
        for (size_t pose = 0; pose < pose_count; pose++)
        {
            if (weights[pose] > 0.0f)
            {
                tb_anim_sum_add(&blend, &poses[pose][joint], weights[pose] / total);
            }
        }
        result[joint] = tb_anim_sum_result(&blend);
    }
}

void tb_anim_add(const tb_anim_transform *base, const tb_anim_transform *additive,
                 const tb_anim_transform *reference, float weight, tb_anim_transform *result,
                 size_t joint_count)
{
    TB_ASSERT(base != NULL && additive != NULL && reference != NULL && result != NULL);
    for (size_t joint = 0; joint < joint_count; joint++)
    {
        const tb_anim_transform from = reference[joint];
        const tb_anim_transform to = additive[joint];
        const tb_anim_transform onto = base[joint];
        const tb_quat turn = tb_quat_mul(tb_quat_conjugate(from.rotation), to.rotation);
        const tb_vec3 ratio = {to.scale.x / from.scale.x, to.scale.y / from.scale.y,
                               to.scale.z / from.scale.z};
        tb_anim_transform added = TB_ANIM_TRANSFORM_INIT;

        TB_ASSERT_MSG(from.scale.x != 0.0f && from.scale.y != 0.0f && from.scale.z != 0.0f,
                      "a reference pose of no scale of 0");
        added.translation = tb_vec3_add(
            onto.translation, tb_vec3_scale(tb_vec3_sub(to.translation, from.translation), weight));
        added.rotation = tb_quat_normalize(
            tb_quat_mul(onto.rotation, tb_quat_slerp(tb_quat_identity(), turn, weight)));
        added.scale = tb_vec3_mul(onto.scale, tb_vec3_lerp((tb_vec3){1, 1, 1}, ratio, weight));
        result[joint] = added;
    }
}

/* ---------------------------------------------------------------------------
 * The hierarchy and skinning.
 * ------------------------------------------------------------------------- */

/* A joint's model matrix, from its parent's, which is made already. */
static tb_mat4 tb_anim_joint_model(const tb_skeleton *skeleton, const tb_mat4 *model, size_t joint,
                                   tb_anim_transform local)
{
    const uint32_t parent = skeleton->parents[joint];
    const tb_mat4 own = tb_anim_transform_matrix(local);

    if (parent == TB_SKELETON_NO_PARENT)
    {
        return own;
    }
    return tb_mat4_mul(model[parent], own);
}

void tb_anim_model_pose(const tb_skeleton *skeleton, const tb_anim_transform *local, tb_mat4 *model,
                        size_t joint_count)
{
    TB_ASSERT(skeleton != NULL && local != NULL && model != NULL);
    TB_ASSERT_MSG(joint_count == skeleton->joint_count, "a matrix for each joint of the skeleton");
    for (size_t joint = 0; joint < joint_count; joint++)
    {
        model[joint] = tb_anim_joint_model(skeleton, model, joint, local[joint]);
    }
}

void tb_anim_skinning_matrices(const tb_skeleton *skeleton, const tb_mat4 *model, tb_mat4 *skinning,
                               size_t joint_count)
{
    TB_ASSERT(skeleton != NULL && model != NULL && skinning != NULL);
    TB_ASSERT_MSG(joint_count == skeleton->joint_count, "a matrix for each joint of the skeleton");
    for (size_t joint = 0; joint < joint_count; joint++)
    {
        skinning[joint] = tb_mat4_mul(model[joint], skeleton->inverse_bind[joint]);
    }
}

tb_mat4 tb_anim_skin_matrix(const tb_mat4 *skinning, size_t matrix_count, const uint16_t joints[4],
                            const float weights[4])
{
    tb_mat4 result = {0};

    TB_ASSERT(skinning != NULL && matrix_count > 0 && joints != NULL && weights != NULL);
    for (size_t influence = 0; influence < 4; influence++)
    {
        const size_t joint =
            joints[influence] < matrix_count ? joints[influence] : matrix_count - 1;
        for (size_t column = 0; column < 4; column++)
        {
            result.columns[column] =
                tb_vec4_add(result.columns[column],
                            tb_vec4_scale(skinning[joint].columns[column], weights[influence]));
        }
    }
    return result;
}

/* ---------------------------------------------------------------------------
 * Evaluating instances.
 * ------------------------------------------------------------------------- */

/* A joint's local transform: the instance's layers sampled and blended. */
static tb_anim_transform tb_anim_instance_joint(const tb_anim_instance *instance, size_t joint,
                                                float total)
{
    const tb_anim_transform rest = instance->skeleton->rest[joint];
    tb_anim_sum blend = {0};

    if (total <= 0.0f)
    {
        return rest;
    }
    for (size_t index = 0; index < instance->layer_count; index++)
    {
        const tb_anim_layer *const layer = &instance->layers[index];
        tb_anim_transform sampled = rest;
        if (layer->weight <= 0.0f)
        {
            continue;
        }
        sampled = tb_anim_clip_joint(
            layer->clip, joint, tb_anim_clip_time(layer->clip, layer->time, layer->wrap), rest);
        tb_anim_sum_add(&blend, &sampled, layer->weight / total);
    }
    return tb_anim_sum_result(&blend);
}

void tb_anim_evaluate(const tb_anim_instance *instance)
{
    const tb_skeleton *skeleton = NULL;
    float total = 0.0f;

    TB_ASSERT(instance != NULL && instance->skeleton != NULL && instance->local != NULL &&
              instance->model != NULL);
    TB_ASSERT(instance->layers != NULL || instance->layer_count == 0);
    skeleton = instance->skeleton;
    for (size_t index = 0; index < instance->layer_count; index++)
    {
        TB_ASSERT(instance->layers[index].clip != NULL);
        TB_ASSERT_MSG(instance->layers[index].weight >= 0.0f, "weights are not negative");
        total += instance->layers[index].weight;
    }
    for (size_t joint = 0; joint < skeleton->joint_count; joint++)
    {
        instance->local[joint] = tb_anim_instance_joint(instance, joint, total);
        instance->model[joint] =
            tb_anim_joint_model(skeleton, instance->model, joint, instance->local[joint]);
    }
    if (instance->skinning != NULL)
    {
        tb_anim_skinning_matrices(skeleton, instance->model, instance->skinning,
                                  skeleton->joint_count);
    }
}

/* The instances a parallel loop evaluates. */
typedef struct tb_anim_batch
{
    const tb_anim_instance *instances;
} tb_anim_batch;

static void tb_anim_evaluate_range(void *argument, size_t first, size_t count)
{
    const tb_anim_batch *const batch = argument;

    for (size_t index = first; index < first + count; index++)
    {
        tb_anim_evaluate(&batch->instances[index]);
    }
}

void tb_anim_evaluate_many(tb_jobs *jobs, const tb_anim_instance *instances, size_t count)
{
    tb_anim_batch batch = {instances};

    TB_ASSERT(instances != NULL || count == 0);
    if (jobs == NULL)
    {
        tb_anim_evaluate_range(&batch, 0, count);
        return;
    }
    tb_jobs_for(jobs, count, 0, tb_anim_evaluate_range, &batch);
}
