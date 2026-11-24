/** @file skinning.h
 * @brief Skinned meshes: meshes whose vertices follow the joints of a
 * skeleton, skinned on the GPU by a compute shader each frame.
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
 * **How it draws.** At the end of the frame, a compute pass, `skinning`,
 * runs before the scene pass: a thread for each vertex of each skinned draw
 * reads the mesh's vertices and the draw's joint matrices by their bindless
 * indices and writes the skinned vertex, laid out as a #tb_mesh_vertex, to a
 * buffer the frame's render graph makes. The scene pass then draws each
 * skinned mesh from its part of that buffer, through the pipelines plain
 * meshes and materials draw with, so a skinned mesh is shaded and lit as any
 * mesh is.
 *
 * A device without bindless resources skins nothing, and draws no skinned
 * mesh.
 *
 * @par Threading
 * Stage-owned: called on the thread that renders, as the renderer's
 * functions are.
 */
#ifndef TB_SKINNING_H
#define TB_SKINNING_H

#include "badger/api.h"
#include "badger/material.h"
#include "badger/math.h"
#include "badger/renderer.h"

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
 * caller; destroyed by tb_skinned_mesh_destroy() before its renderer. */
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
 * matrices, or the device has no bindless resources.
 * @param draw Required; copied, its matrices with it.
 */
TB_API void tb_renderer_draw_skinned(tb_renderer *renderer, const tb_skinned_draw *draw);

#endif
