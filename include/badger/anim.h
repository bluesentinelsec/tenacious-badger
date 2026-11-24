/** @file anim.h
 * @brief Skeletal animation on the CPU: skeletons, clips of keyframes,
 * sampling, blending, the pose of a hierarchy, and skinning matrices.
 *
 * The animation module knows nothing of the GPU. Gameplay code samples and
 * blends poses with it, and the renderer skins meshes with the matrices it
 * gives (badger/skinning.h).
 *
 * @code
 * tb_skeleton *const skeleton = tb_skeleton_create(&(tb_skeleton_desc){joints, joint_count});
 * tb_anim_clip *const walk = tb_anim_clip_create(&walk_desc);
 * tb_anim_clip *const run = tb_anim_clip_create(&run_desc);
 *
 * // each frame, for each character
 * const tb_anim_layer layers[2] = {{walk, seconds, TB_ANIM_LOOP, 1.0f - speed},
 *                                  {run, seconds, TB_ANIM_LOOP, speed}};
 * characters[i].instance = (tb_anim_instance){.skeleton = skeleton,
 *                                             .layers = layers,
 *                                             .layer_count = 2,
 *                                             .local = characters[i].local,
 *                                             .model = characters[i].model,
 *                                             .skinning = characters[i].skinning};
 * // ...
 * tb_anim_evaluate_many(jobs, instances, character_count);
 * @endcode
 *
 * **Joints** are numbered from 0, each after its parent, so that a pose is
 * accumulated down the hierarchy in one pass. A joint's transform is local:
 * relative to its parent, or to the skeleton's space for a root. Its *model*
 * matrix is the product of its ancestors' local transforms and its own: from
 * the joint's space to the skeleton's. Its *skinning* matrix is its model
 * matrix times its inverse bind matrix: from where a vertex was bound, in the
 * mesh's space, to where the joint has taken it.
 *
 * **Clips** hold channels: the keyframes of one joint's translation, rotation
 * or scale, at increasing times in seconds. Between keys a channel blends
 * linearly (rotations by slerp) or holds the earlier key's value (step).
 * Before its first key and after its last a channel holds the nearest key. A
 * joint no channel moves keeps its rest transform. A clip's time is looped or
 * clamped to its duration.
 *
 * **Blending** weighs poses by weights that need not sum to one: they are
 * divided by their sum. Rotations are blended as quaternions, each brought
 * into the hemisphere of the sum before it, summed and normalized: the same rotation
 * whichever sign each quaternion has. An additive pose adds what a pose
 * differs from a reference pose to a base pose.
 *
 * **Conventions** are the engine's (docs/math.md): column vectors, matrices
 * stored by column, and quaternions `x, y, z, w`.
 *
 * @par Threading
 * Skeletons and clips are immutable once made: every function that reads
 * them is thread-safe, and many threads sample the same ones at once.
 * Creating and destroying one is externally synchronized with its use.
 * Functions that write poses are thread-safe for different poses.
 */
#ifndef TB_ANIM_H
#define TB_ANIM_H

#include "badger/api.h"
#include "badger/math.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_jobs;

/** @brief A transform of a joint: scaled, then rotated, then translated. */
typedef struct tb_anim_transform
{
    tb_vec3 translation; /**< Where it is. */
    tb_quat rotation;    /**< How it is turned; of length 1. */
    tb_vec3 scale;       /**< How it is scaled along its axes. */
} tb_anim_transform;

/** @brief The initializer of a transform that changes nothing. */
#define TB_ANIM_TRANSFORM_INIT {.rotation = {0.0f, 0.0f, 0.0f, 1.0f}, .scale = {1.0f, 1.0f, 1.0f}}

/** @brief A transform's matrix: `translation * rotation * scale`.
 * @par Threading Thread-safe. */
TB_API tb_mat4 tb_anim_transform_matrix(tb_anim_transform transform);

/** @brief What a root joint names as its parent. */
#define TB_SKELETON_NO_PARENT UINT32_MAX
/** @brief What tb_skeleton_find() gives for a name no joint has. */
#define TB_SKELETON_NO_JOINT UINT32_MAX
/** @brief The most joints a skeleton has: as many as a vertex's 16-bit
 * joint indices name. */
#define TB_SKELETON_MAX_JOINTS 65536u

/** @brief A skeleton: joints, their hierarchy, and their bind pose. Made by
 * tb_skeleton_create(); owned by the caller; destroyed by
 * tb_skeleton_destroy(). */
typedef struct tb_skeleton tb_skeleton;

/** @brief A joint of a skeleton to make. */
typedef struct tb_skeleton_joint
{
    /** For tb_skeleton_find() and debuggers; may be NULL; copied. */
    const char *name;
    /** The index of its parent, below its own; or #TB_SKELETON_NO_PARENT. */
    uint32_t parent;
    /** Its local transform at rest: what sampling gives a joint that no
     * channel of a clip moves, and the bind pose when the description has no
     * inverse bind matrices. */
    tb_anim_transform rest;
} tb_skeleton_joint;

/** @brief A skeleton to make. Zero-initialize, then fill in. */
typedef struct tb_skeleton_desc
{
    const tb_skeleton_joint *joints; /**< Required; `joint_count` of them; copied. */
    size_t joint_count;              /**< From 1 to #TB_SKELETON_MAX_JOINTS. */
    /** The inverse of each joint's model matrix when the mesh was bound to
     * it, as glTF gives them; `joint_count` of them, copied. NULL computes
     * them from the rest pose. */
    const tb_mat4 *inverse_bind;
} tb_skeleton_desc;

/**
 * @brief Make a skeleton.
 * @param desc Required.
 * @return The skeleton; NULL when the description breaks a rule: no joints
 * or too many, a parent that is not before its child, or a rest pose with no
 * inverse (a scale of 0) when it gives no inverse bind matrices.
 * @par Threading Thread-safe.
 */
TB_API tb_skeleton *tb_skeleton_create(const tb_skeleton_desc *desc);

/** @brief Destroy a skeleton, when nothing uses it. NULL is ignored.
 * @par Threading Externally synchronized with every use of the skeleton. */
TB_API void tb_skeleton_destroy(tb_skeleton *skeleton);

/** @brief How many joints a skeleton has.
 * @par Threading Thread-safe. */
TB_API size_t tb_skeleton_joint_count(const tb_skeleton *skeleton);

/** @brief A joint's parent, or #TB_SKELETON_NO_PARENT for a root.
 * @param joint Below the joint count.
 * @par Threading Thread-safe. */
TB_API uint32_t tb_skeleton_parent(const tb_skeleton *skeleton, size_t joint);

/** @brief A joint's name, or NULL when it has none; owned by the skeleton.
 * @param joint Below the joint count.
 * @par Threading Thread-safe. */
TB_API const char *tb_skeleton_joint_name(const tb_skeleton *skeleton, size_t joint);

/** @brief The first joint of a name, or #TB_SKELETON_NO_JOINT.
 * @param name Required.
 * @par Threading Thread-safe. */
TB_API uint32_t tb_skeleton_find(const tb_skeleton *skeleton, const char *name);

/** @brief The rest pose: a local transform for each joint, owned by the
 * skeleton.
 * @par Threading Thread-safe. */
TB_API const tb_anim_transform *tb_skeleton_rest_pose(const tb_skeleton *skeleton);

/** @brief The inverse bind matrices, one for each joint, owned by the
 * skeleton.
 * @par Threading Thread-safe. */
TB_API const tb_mat4 *tb_skeleton_inverse_bind(const tb_skeleton *skeleton);

/** @brief What a channel moves. */
typedef enum tb_anim_path
{
    TB_ANIM_TRANSLATION, /**< The joint's translation: three floats a key. */
    TB_ANIM_ROTATION,    /**< Its rotation: four floats a key, `x, y, z, w`, of length 1. */
    TB_ANIM_SCALE        /**< Its scale: three floats a key. */
} tb_anim_path;

/** @brief How a channel goes from a key to the next. */
typedef enum tb_anim_interpolation
{
    TB_ANIM_LINEAR, /**< Blends linearly; rotations by slerp. */
    TB_ANIM_STEP    /**< Holds a key's value until the next. */
} tb_anim_interpolation;

/** @brief What a time outside a clip's duration becomes. */
typedef enum tb_anim_wrap
{
    TB_ANIM_LOOP, /**< Wraps around: the clip repeats, forward and back. */
    TB_ANIM_CLAMP /**< Stops at its start or its end. */
} tb_anim_wrap;

/** @brief A channel of a clip to make: one joint's keys of one path. */
typedef struct tb_anim_channel
{
    uint32_t joint;                      /**< Which; a joint the clip's skeletons have. */
    tb_anim_path path;                   /**< What it moves. */
    tb_anim_interpolation interpolation; /**< Between keys. */
    const float *times;                  /**< Seconds; `key_count` of them, increasing; copied. */
    const float *values;                 /**< 3 or 4 floats a key, as `path` says; copied. */
    size_t key_count;                    /**< At least 1. */
} tb_anim_channel;

/** @brief A clip to make. Zero-initialize, then fill in. */
typedef struct tb_anim_clip_desc
{
    const tb_anim_channel *channels; /**< `channel_count` of them; copied. */
    size_t channel_count;            /**< At most one channel of each path for each joint. */
    /** Seconds from its start to its end, more than 0; 0 for its last key's
     * time. */
    float duration;
    const char *name; /**< May be NULL; copied. */
} tb_anim_clip_desc;

/** @brief A clip: keyframes of joints over time. Made by
 * tb_anim_clip_create(); owned by the caller; destroyed by
 * tb_anim_clip_destroy(). It names joints by index, and plays on any
 * skeleton that has them. */
typedef struct tb_anim_clip tb_anim_clip;

/**
 * @brief Make a clip.
 * @param desc Required.
 * @return The clip; NULL when the description breaks a rule: a channel with
 * no keys or no values, times that do not increase or are not finite, a
 * joint of #TB_SKELETON_MAX_JOINTS or more, two channels of one path of a
 * joint, or a negative duration.
 * @par Threading Thread-safe.
 */
TB_API tb_anim_clip *tb_anim_clip_create(const tb_anim_clip_desc *desc);

/** @brief Destroy a clip, when nothing uses it. NULL is ignored.
 * @par Threading Externally synchronized with every use of the clip. */
TB_API void tb_anim_clip_destroy(tb_anim_clip *clip);

/** @brief A clip's duration in seconds; 0 for a clip whose keys are all at 0.
 * @par Threading Thread-safe. */
TB_API float tb_anim_clip_duration(const tb_anim_clip *clip);

/** @brief A clip's name, or NULL; owned by the clip.
 * @par Threading Thread-safe. */
TB_API const char *tb_anim_clip_name(const tb_anim_clip *clip);

/**
 * @brief The time within a clip that a time comes to: from 0 to its
 * duration, looped or clamped. Looping, the duration itself comes to 0.
 * @param time Seconds; any finite value, negative too.
 * @par Threading Thread-safe.
 */
TB_API float tb_anim_clip_time(const tb_anim_clip *clip, float time, tb_anim_wrap wrap);

/**
 * @brief Sample a clip into a local pose: each joint's transform at a time,
 * or its rest transform where no channel moves it.
 * @param skeleton Required.
 * @param time Seconds, wrapped as tb_anim_clip_time() says.
 * @param pose Receives `pose_count` transforms.
 * @param pose_count The skeleton's joint count.
 * @par Threading Thread-safe.
 */
TB_API void tb_anim_clip_sample(const tb_anim_clip *clip, const tb_skeleton *skeleton, float time,
                                tb_anim_wrap wrap, tb_anim_transform *pose, size_t pose_count);

/**
 * @brief Blend poses by weight, joint by joint.
 * @param poses Required; `pose_count` poses, each of `joint_count` transforms.
 * @param weights Required; `pose_count` of them, none negative. They are
 * divided by their sum; when it is 0, the first pose is the result.
 * @param pose_count At least 1.
 * @param result Receives `joint_count` transforms; may be one of `poses`.
 * @par Threading Thread-safe.
 */
TB_API void tb_anim_blend(const tb_anim_transform *const *poses, const float *weights,
                          size_t pose_count, tb_anim_transform *result, size_t joint_count);

/**
 * @brief Add what a pose differs from a reference pose to a base pose, by a
 * weight: translations add their difference, rotations turn by the rotation
 * from the reference's to the pose's, and scales multiply by their ratio.
 * Weight 0 is the base, and 1 the whole difference.
 * @param base Required; `joint_count` transforms.
 * @param additive Required; likewise.
 * @param reference Required; likewise; no scale of 0.
 * @param weight From 0 to 1.
 * @param result Receives `joint_count` transforms; may be `base`.
 * @par Threading Thread-safe.
 */
TB_API void tb_anim_add(const tb_anim_transform *base, const tb_anim_transform *additive,
                        const tb_anim_transform *reference, float weight, tb_anim_transform *result,
                        size_t joint_count);

/**
 * @brief The model matrix of each joint: its local transform after its
 * ancestors'.
 * @param local Required; the skeleton's joint count of local transforms.
 * @param model Receives `joint_count` matrices.
 * @param joint_count The skeleton's.
 * @par Threading Thread-safe.
 */
TB_API void tb_anim_model_pose(const tb_skeleton *skeleton, const tb_anim_transform *local,
                               tb_mat4 *model, size_t joint_count);

/**
 * @brief The skinning matrix of each joint: its model matrix times its
 * inverse bind matrix. What a skinned mesh is drawn with.
 * @param model Required; `joint_count` model matrices.
 * @param skinning Receives `joint_count` matrices.
 * @param joint_count The skeleton's.
 * @par Threading Thread-safe.
 */
TB_API void tb_anim_skinning_matrices(const tb_skeleton *skeleton, const tb_mat4 *model,
                                      tb_mat4 *skinning, size_t joint_count);

/**
 * @brief The matrix that skins one vertex: its joints' skinning matrices,
 * weighed by its weights, as the GPU's skinning blends them. A joint index
 * past the matrices is the last matrix.
 * @param skinning Required; `matrix_count` matrices, at least 1.
 * @param joints The vertex's four joints.
 * @param weights Their weights, which ought to sum to 1; used as they are.
 * @par Threading Thread-safe.
 */
TB_API tb_mat4 tb_anim_skin_matrix(const tb_mat4 *skinning, size_t matrix_count,
                                   const uint16_t joints[4], const float weights[4]);

/** @brief A clip played in a blend: at a time, wrapped, by a weight. */
typedef struct tb_anim_layer
{
    const tb_anim_clip *clip; /**< Required. */
    float time;               /**< Seconds. */
    tb_anim_wrap wrap;        /**< What a time outside the clip becomes. */
    float weight;             /**< Not negative; divided by the layers' sum. */
} tb_anim_layer;

/** @brief A skeleton's pose to evaluate: its layers sampled and blended, the
 * hierarchy accumulated, and its skinning matrices made. */
typedef struct tb_anim_instance
{
    const tb_skeleton *skeleton; /**< Required. */
    const tb_anim_layer *layers; /**< `layer_count` of them. */
    size_t layer_count;          /**< 0, or weights of sum 0, gives the rest pose. */
    tb_anim_transform *local;    /**< Required; receives the joint count of transforms. */
    tb_mat4 *model;              /**< Required; receives the joint count of matrices. */
    tb_mat4 *skinning;           /**< Receives the joint count of matrices; may be NULL. */
} tb_anim_instance;

/**
 * @brief Evaluate one instance on the calling thread.
 * @param instance Required.
 * @par Threading Thread-safe for instances that write different poses.
 */
TB_API void tb_anim_evaluate(const tb_anim_instance *instance);

/**
 * @brief Evaluate many instances, on every thread of a job system, and
 * return when all are done.
 * @param jobs The job system; NULL evaluates them on the calling thread.
 * @param instances Required when `count` is more than 0; no two write the
 * same pose.
 * @par Threading Thread-safe; may be called from inside a job.
 */
TB_API void tb_anim_evaluate_many(struct tb_jobs *jobs, const tb_anim_instance *instances,
                                  size_t count);

#endif
