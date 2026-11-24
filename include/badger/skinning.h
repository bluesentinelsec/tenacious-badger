/** @file skinning.h
 * @brief Skinned meshes: meshes whose vertices follow the joints of a
 * skeleton, skinned each frame on the GPU by a compute shader, or on the CPU
 * where the GPU cannot.
 *
 * @code
 * tb_skinned_mesh *const arm = tb_skinned_mesh_create(renderer, vertices, vertex_count,
 *                                                     indices, index_count);
 *
 * // each frame: skinning matrices from the animation module (badger/anim.h)
 * tb_anim_evaluate(&instance);
 * tb_renderer_draw_skinned(renderer, &(tb_skinned_draw){.mesh = arm,
 *                                                       .transform = transform,
 *                                                       .joints = instance.skinning,
 *                                                       .joint_count = joint_count,
 *                                                       .color = {1, 1, 1, 1}});
 * @endcode
 *
 * **Vertices** carry up to four joints and their weights, as glTF's do. A
 * vertex's skinned position is its position transformed by the weighted sum
 * of its joints' matrices; its normal likewise, normalized. The weights
 * ought to sum to 1, and are used as they are.
 *
 * **Joint matrices** are skinning matrices: each joint's model matrix times
 * its inverse bind matrix (tb_anim_skinning_matrices()), from the mesh's
 * space as it was bound to where the joints have taken it. A draw copies them
 * into the frame's uploads, so they need last only for the call.
 *
 * **How it draws on the GPU.** At the end of the frame, a compute pass,
 * `skinning`, runs before the scene pass: a thread for each vertex of each
 * skinned draw reads the mesh's vertices and the draw's joint matrices by
 * their bindless indices and writes the skinned vertex, laid out as a
 * #tb_mesh_vertex, to a buffer the frame's render graph makes. The scene
 * pass then draws each skinned mesh from its part of that buffer, through
 * the pipelines plain meshes and materials draw with, so a skinned mesh is
 * shaded and lit as any mesh is.
 *
 * **On the CPU.** A device without bindless resources, or one where the
 * skinning pipeline cannot be made, skins on the CPU instead: each draw's
 * vertices are skinned by tb_skin_vertices() when the draw is made, into the
 * frame's uploads, and drawn from there by the same pipelines. The two give
 * the same picture, within the rounding of the GPU's arithmetic. A missing
 * GPU path is an unavailable capability, never a failure:
 * tb_renderer_skinning_info() says which path is in use and why, and the
 * renderer logs it when it is made. tb_renderer_force_cpu_skinning() takes
 * the CPU path where the GPU's is there, to test it and to compare the two.
 *
 * @par Threading
 * Stage-owned: called on the thread that renders, as the renderer's
 * functions are; but tb_skin_vertices() and tb_skinning_path_name() are
 * thread-safe.
 */
#ifndef TB_SKINNING_H
#define TB_SKINNING_H

#include "badger/api.h"
#include "badger/material.h"
#include "badger/math.h"
#include "badger/renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A vertex of a skinned mesh: 56 bytes. */
typedef struct tb_skinned_vertex
{
    tb_vec3 position;   /**< In the mesh's space, as it was bound. */
    tb_vec3 normal;     /**< Unit length, outward. */
    tb_vec2 uv;         /**< Texture coordinates. */
    uint16_t joints[4]; /**< The joints that move it: indices of the draw's joint matrices. */
    float weights[4];   /**< How much each moves it; summing to 1. */
} tb_skinned_vertex;

/** @brief The most vertices a skinned mesh has. */
#define TB_SKINNED_MESH_MAX_VERTICES 4194240u

/** @brief A skinned mesh. Made by tb_skinned_mesh_create(); owned by the
 * caller; destroyed by tb_skinned_mesh_destroy() before its renderer. It
 * keeps its vertices both on the GPU and in memory, for either path. */
typedef struct tb_skinned_mesh tb_skinned_mesh;

/**
 * @brief Make a skinned mesh from triangles.
 * @param vertices Required; `vertex_count` of them, from 1 to
 * #TB_SKINNED_MESH_MAX_VERTICES; copied.
 * @param indices Required; `index_count` of them, three to a triangle,
 * counter-clockwise seen from its front; copied.
 * @return The mesh; never NULL.
 */
TB_API tb_skinned_mesh *tb_skinned_mesh_create(tb_renderer *renderer,
                                               const tb_skinned_vertex *vertices,
                                               size_t vertex_count, const uint32_t *indices,
                                               size_t index_count);

/** @brief Destroy a skinned mesh, outside a frame that draws it. NULL is
 * ignored. */
TB_API void tb_skinned_mesh_destroy(tb_renderer *renderer, tb_skinned_mesh *mesh);

/** @brief A draw of a skinned mesh. Zero-initialize, then fill in. */
typedef struct tb_skinned_draw
{
    const tb_skinned_mesh *mesh; /**< Required. */
    tb_mat4 transform;           /**< From the mesh's space to the world's. */
    /** Required: the skinning matrices of its joints, `joint_count` of
     * them; copied. A vertex's joint past them takes the last. */
    const tb_mat4 *joints;
    size_t joint_count; /**< From 1 to 65536. */
    /** Its material, alive until the frame ends; NULL to draw it in `color`. */
    const tb_material *material;
    tb_vec4 color;           /**< Without a material: linear RGB and alpha. */
    tb_mesh_shading shading; /**< Without a material: lit or flat. */
} tb_skinned_draw;

/**
 * @brief Draw a skinned mesh this frame, posed by its joints' matrices.
 * Nothing is drawn when the frame's uploads have no room left for its
 * matrices, or, skinning on the CPU, for its skinned vertices (32 bytes
 * each).
 * @param draw Required; copied, its matrices with it.
 */
TB_API void tb_renderer_draw_skinned(tb_renderer *renderer, const tb_skinned_draw *draw);

/** @brief Where a renderer skins its skinned meshes. */
typedef enum tb_skinning_path
{
    TB_SKINNING_GPU, /**< By the `skinning` compute pass, before the scene pass. */
    TB_SKINNING_CPU  /**< By tb_skin_vertices(), as each draw is made, into the frame's uploads. */
} tb_skinning_path;

/** @brief How a renderer skins, from tb_renderer_skinning_info(). */
typedef struct tb_skinning_info
{
    tb_skinning_path path; /**< Where. */
    /** Why, as a phrase for a log or an options screen: "a compute pass",
     * "forced", "the device has no bindless resources", or why the skinning
     * pipeline could not be made. Owned by the renderer; valid until it is
     * destroyed. */
    const char *reason;
} tb_skinning_info;

/**
 * @brief Where the renderer skins this frame, or the next when called
 * outside a frame, and why: a capability query. The GPU path wants bindless
 * resources and the skinning pipeline; without them, the CPU skins.
 * @param info Required; receives the path and the reason.
 */
TB_API void tb_renderer_skinning_info(const tb_renderer *renderer, tb_skinning_info *info);

/**
 * @brief Skin on the CPU even where the GPU could, from the next frame on;
 * or, given false, on the GPU again where it can. The renderer logs each
 * change of path. For tests, and to compare the two; off when a renderer is
 * made.
 */
TB_API void tb_renderer_force_cpu_skinning(tb_renderer *renderer, bool force);

/** @brief Name a skinning path, for a log or an options screen: "GPU" or
 * "CPU"; "unknown" for any other value.
 * @par Threading Thread-safe. */
TB_API const char *tb_skinning_path_name(tb_skinning_path path);

/**
 * @brief Skin vertices on the CPU, as the GPU's skinning pass does: each
 * vertex's position and normal transformed by its joints' matrices summed by
 * its weights (tb_anim_skin_matrix()), the normal normalized, its texture
 * coordinates kept. The weights are used as they are, so weights that do not
 * sum to 1 scale the vertex as the GPU's do; a joint index past the matrices
 * takes the last; a normal that the sum turns to nothing is kept as it was
 * bound. The vertices carry no tangents: materials make theirs from the
 * skinned normals and texture coordinates.
 *
 * It touches only its arguments, so a caller may split a mesh's vertices
 * into ranges and skin each on its own thread.
 * @param vertices Required; `vertex_count` of them, as they were bound.
 * @param joints Required; `joint_count` skinning matrices, at least 1.
 * @param skinned Required; receives `vertex_count` vertices; does not
 * overlap `vertices`.
 * @par Threading Thread-safe.
 */
TB_API void tb_skin_vertices(const tb_skinned_vertex *vertices, size_t vertex_count,
                             const tb_mat4 *joints, size_t joint_count, tb_mesh_vertex *skinned);

#endif
