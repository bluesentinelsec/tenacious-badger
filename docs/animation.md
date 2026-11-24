# Animation

`badger/anim.h` is skeletal animation on the CPU: skeletons, clips of
keyframes, sampling, blending, the pose of a hierarchy, and the skinning
matrices a skinned mesh is drawn with. It knows nothing of the GPU, so
gameplay code samples and blends poses with it (to place a weapon in a hand,
or to test what an animation reaches), and [the renderer](renderer.md#skinned-meshes)
skins meshes with the matrices it gives.

```c
tb_skeleton *const skeleton = tb_skeleton_create(&(tb_skeleton_desc){joints, joint_count});
tb_anim_clip *const walk = tb_anim_clip_create(&walk_desc);
tb_anim_clip *const run = tb_anim_clip_create(&run_desc);

/* each frame */
for (size_t i = 0; i < character_count; i++)
{
    characters[i].layers[0] = (tb_anim_layer){walk, seconds, TB_ANIM_LOOP, 1.0f - speed[i]};
    characters[i].layers[1] = (tb_anim_layer){run, seconds, TB_ANIM_LOOP, speed[i]};
    instances[i] = (tb_anim_instance){.skeleton = skeleton,
                                      .layers = characters[i].layers,
                                      .layer_count = 2,
                                      .local = characters[i].local,
                                      .model = characters[i].model,
                                      .skinning = characters[i].skinning};
}
tb_anim_evaluate_many(jobs, instances, character_count);
for (size_t i = 0; i < character_count; i++)
{
    tb_renderer_draw_skinned(renderer, &(tb_skinned_draw){.mesh = body,
                                                          .transform = characters[i].transform,
                                                          .joints = characters[i].skinning,
                                                          .joint_count = joint_count,
                                                          .material = skin});
}
```

Data comes from the caller: the module reads no files. A glTF loader, when
it comes, fills the same descriptions.

## Skeletons

A skeleton is joints, each with a parent and a rest transform (translation,
rotation, scale), numbered so that **every parent comes before its
children**. That order lets a pose be accumulated down the hierarchy in one
pass, and is what glTF importers and Unreal's reference skeleton keep too.
Names are optional; `tb_skeleton_find()` looks one up.

Each joint has an **inverse bind matrix**: the inverse of its model matrix
when the mesh was bound to it. A description may give them, as glTF does,
or leave them to be computed from the rest pose.

`tb_skeleton_create()` returns NULL for a description that breaks a rule (no
joints, more than 65536, a parent after its child, or a rest pose of scale 0
when it must be inverted), so a loader of bad data gets an answer, not a
skeleton that reads out of bounds.

## Clips

A clip holds **channels**: one joint's translation, rotation or scale, as
keys at increasing times in seconds. Between keys a channel blends
linearly, rotations by slerp, or holds the earlier key (`TB_ANIM_STEP`), as
glTF's `LINEAR` and `STEP` do; cubic splines are not done yet. Before its
first key and after its last, a channel holds the nearest. A joint no channel
moves keeps its rest transform, so a clip of an arm plays on a whole body.

A clip's **duration** is its last key's time unless the description sets
one. `TB_ANIM_LOOP` wraps a time into `[0, duration)`, backward too;
`TB_ANIM_CLAMP` holds the ends. Clips name joints by index, and play on any
skeleton that has them; retargeting between skeletons is not done.

## Blending

`tb_anim_blend()` weighs poses joint by joint. Weights are divided by their
sum, so `1, 3` is a quarter and three quarters; weights of sum 0 give the
first pose. Translations and scales are summed. **Rotations** are summed as
quaternions, each first brought into the hemisphere of the first weighted
pose's (`q` and `−q` are one rotation, and summed they would cancel), then
normalized: the normalized linear blend, which for poses of one character is
as good as slerp, works for any number of poses, and does not depend on
their order.

`tb_anim_add()` is **additive** blending: it adds what a pose differs from a
reference pose (its translation's difference, the rotation from the
reference's to its own, its scale's ratio) to a base pose, by a weight. A
breathing or recoil clip made relative to an idle pose layers over a walk.

## Hierarchy and skinning

`tb_anim_model_pose()` turns a local pose into each joint's **model
matrix**, its ancestors' transforms then its own, from the joint's space to
the skeleton's. `tb_anim_skinning_matrices()` multiplies each by its inverse
bind matrix: the **skinning matrix**, which takes a vertex from where it was
bound to where the joint has taken it. A vertex's skinned position is the
sum of its joints' skinning matrices, weighed, times its position;
`tb_anim_skin_matrix()` gives that sum on the CPU, as the GPU's skinning
computes it.

## Many instances on the job system

An **instance** is a skeleton, its layers (clips at times, wrapped, by
weights), and the arrays it writes: its local pose, its model matrices, and
optionally its skinning matrices. `tb_anim_evaluate()` does one on the
calling thread, joint by joint: each layer sampled at the joint, blended,
then accumulated onto its parent, with no memory of its own.
`tb_anim_evaluate_many()` evaluates many with `tb_jobs_for()`, on every
thread of [the job system](jobs.md), and returns when all are done. Each
instance is the same arithmetic on one thread, so the results are the same,
bit for bit, as evaluating them one at a time.

Skeletons and clips are immutable once made, so any number of threads
sample them at once.

## Not yet

- Cubic-spline keys, and compressed keys.
- Root motion, events and curves in clips.
- Retargeting a clip between skeletons, and masks that blend part of a
  skeleton.
- Inverse kinematics.
- Morph targets.
