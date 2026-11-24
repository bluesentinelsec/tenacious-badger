/* Debug drawing: lines in the world for one frame, from any thread, as
 * boxes, spheres, grids, axes and arrows made of lines. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

/* Segments of a circle of a debug sphere. */
#define TB_DEBUG_CIRCLE_SEGMENTS 32u

void tb_debug_line(tb_renderer *renderer, tb_vec3 a, tb_vec3 b, tb_vec4 color, unsigned flags)
{
    tb_renderer_lines *lines = NULL;

    TB_ASSERT(renderer != NULL);
    tb_mutex_lock(&renderer->lines_lock);
    lines = &renderer->lines[(flags & TB_DEBUG_ON_TOP) != 0 ? 1 : 0];
    if (lines->count + 2 > lines->capacity)
    {
        lines->capacity = lines->capacity == 0 ? 1024 : lines->capacity * 2;
        lines->vertices =
            tb_realloc_array(lines->vertices, lines->capacity, sizeof(*lines->vertices));
    }
    lines->vertices[lines->count] = (tb_renderer_line_vertex){a, color};
    lines->vertices[lines->count + 1] = (tb_renderer_line_vertex){b, color};
    lines->count += 2;
    tb_mutex_unlock(&renderer->lines_lock);
}

void tb_debug_box(tb_renderer *renderer, tb_mat4 transform, tb_vec3 half_extents, tb_vec4 color,
                  unsigned flags)
{
    static const int edges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                     {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    tb_vec3 corners[8] = {0};

    for (int corner = 0; corner < 8; corner++)
    {
        const tb_vec3 local = {(corner & 1) != 0 ? half_extents.x : -half_extents.x,
                               (corner & 2) != 0 ? half_extents.y : -half_extents.y,
                               (corner & 4) != 0 ? half_extents.z : -half_extents.z};
        corners[corner] = tb_mat4_transform_point(transform, local);
    }
    for (int edge = 0; edge < 12; edge++)
    {
        tb_debug_line(renderer, corners[edges[edge][0]], corners[edges[edge][1]], color, flags);
    }
}

void tb_debug_sphere(tb_renderer *renderer, tb_vec3 center, float radius, tb_vec4 color,
                     unsigned flags)
{
    for (int axis = 0; axis < 3; axis++)
    {
        tb_vec3 previous = {0};
        for (uint32_t segment = 0; segment <= TB_DEBUG_CIRCLE_SEGMENTS; segment++)
        {
            const float angle = TB_TAU * (float)segment / (float)TB_DEBUG_CIRCLE_SEGMENTS;
            const float u = cosf(angle) * radius;
            const float v = sinf(angle) * radius;
            const tb_vec3 offset = axis == 0   ? (tb_vec3){0.0f, u, v}
                                   : axis == 1 ? (tb_vec3){u, 0.0f, v}
                                               : (tb_vec3){u, v, 0.0f};
            const tb_vec3 point = tb_vec3_add(center, offset);
            if (segment > 0)
            {
                tb_debug_line(renderer, previous, point, color, flags);
            }
            previous = point;
        }
    }
}

void tb_debug_grid(tb_renderer *renderer, tb_vec3 center, float size, uint32_t divisions,
                   tb_vec4 color, unsigned flags)
{
    const float half = size * 0.5f;

    TB_ASSERT_MSG(divisions > 0, "a grid of no cells");
    for (uint32_t line = 0; line <= divisions; line++)
    {
        const float at = -half + size * (float)line / (float)divisions;
        tb_debug_line(renderer, tb_vec3_add(center, (tb_vec3){at, 0.0f, -half}),
                      tb_vec3_add(center, (tb_vec3){at, 0.0f, half}), color, flags);
        tb_debug_line(renderer, tb_vec3_add(center, (tb_vec3){-half, 0.0f, at}),
                      tb_vec3_add(center, (tb_vec3){half, 0.0f, at}), color, flags);
    }
}

void tb_debug_axes(tb_renderer *renderer, tb_mat4 transform, float size, unsigned flags)
{
    const tb_vec3 origin = tb_mat4_transform_point(transform, (tb_vec3){0.0f, 0.0f, 0.0f});

    tb_debug_line(renderer, origin, tb_mat4_transform_point(transform, (tb_vec3){size, 0.0f, 0.0f}),
                  (tb_vec4){1.0f, 0.2f, 0.2f, 1.0f}, flags);
    tb_debug_line(renderer, origin, tb_mat4_transform_point(transform, (tb_vec3){0.0f, size, 0.0f}),
                  (tb_vec4){0.2f, 1.0f, 0.2f, 1.0f}, flags);
    tb_debug_line(renderer, origin, tb_mat4_transform_point(transform, (tb_vec3){0.0f, 0.0f, size}),
                  (tb_vec4){0.3f, 0.4f, 1.0f, 1.0f}, flags);
}

void tb_debug_arrow(tb_renderer *renderer, tb_vec3 from, tb_vec3 to, tb_vec4 color, unsigned flags)
{
    const tb_vec3 shaft = tb_vec3_sub(to, from);
    const float length = tb_vec3_length(shaft);
    const tb_vec3 direction = tb_vec3_normalize(shaft);
    /* Any direction across the shaft, then the one across both. */
    const tb_vec3 helper = fabsf(direction.y) < 0.9f ? (tb_vec3){0, 1, 0} : (tb_vec3){1, 0, 0};
    const tb_vec3 across = tb_vec3_normalize(tb_vec3_cross(direction, helper));
    const tb_vec3 other = tb_vec3_cross(direction, across);
    const tb_vec3 base = tb_vec3_sub(to, tb_vec3_scale(direction, length * 0.2f));
    const float width = length * 0.08f;

    tb_debug_line(renderer, from, to, color, flags);
    tb_debug_line(renderer, to, tb_vec3_add(base, tb_vec3_scale(across, width)), color, flags);
    tb_debug_line(renderer, to, tb_vec3_sub(base, tb_vec3_scale(across, width)), color, flags);
    tb_debug_line(renderer, to, tb_vec3_add(base, tb_vec3_scale(other, width)), color, flags);
    tb_debug_line(renderer, to, tb_vec3_sub(base, tb_vec3_scale(other, width)), color, flags);
}
