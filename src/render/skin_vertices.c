/* Skinning on the CPU: the arithmetic of the skinning shader, a vertex at a
 * time, with nothing of the renderer's, so that any thread may skin any
 * range of vertices. */
#include "badger/anim.h"
#include "badger/assert.h"
#include "badger/skinning.h"

#include <math.h>

/* One vertex, as the shader skins it: the position as a point and the
 * normal as a direction, by the one weighted matrix. */
static tb_mesh_vertex tb_skin_vertex(const tb_skinned_vertex *vertex, const tb_mat4 *joints,
                                     size_t joint_count)
{
    const tb_mat4 matrix =
        tb_anim_skin_matrix(joints, joint_count, vertex->joints, vertex->weights);
    const tb_vec3 position = vertex->position;
    const tb_vec3 normal = vertex->normal;
    const tb_vec4 skinned =
        tb_mat4_transform(matrix, (tb_vec4){position.x, position.y, position.z, 1.0f});
    const tb_vec4 turned = tb_mat4_transform(matrix, (tb_vec4){normal.x, normal.y, normal.z, 0.0f});
    const float size = turned.x * turned.x + turned.y * turned.y + turned.z * turned.z;
    tb_mesh_vertex result = {0};

    result.position = (tb_vec3){skinned.x, skinned.y, skinned.z};
    result.normal = normal;
    if (size > 0.0f)
    {
        const float scale = 1.0f / sqrtf(size);
        result.normal = (tb_vec3){turned.x * scale, turned.y * scale, turned.z * scale};
    }
    result.uv = vertex->uv;
    return result;
}

void tb_skin_vertices(const tb_skinned_vertex *vertices, size_t vertex_count, const tb_mat4 *joints,
                      size_t joint_count, tb_mesh_vertex *skinned)
{
    TB_ASSERT(vertices != NULL && joints != NULL && skinned != NULL);
    TB_ASSERT_MSG(joint_count > 0, "skinning by at least one joint");
    for (size_t index = 0; index < vertex_count; index++)
    {
        skinned[index] = tb_skin_vertex(&vertices[index], joints, joint_count);
    }
}

const char *tb_skinning_path_name(tb_skinning_path path)
{
    switch (path)
    {
    case TB_SKINNING_GPU:
        return "GPU";
    case TB_SKINNING_CPU:
        return "CPU";
    }
    return "unknown";
}
