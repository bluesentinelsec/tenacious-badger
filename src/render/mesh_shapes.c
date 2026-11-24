/* The shapes the renderer generates: a cube, a plane, a sphere, a cylinder,
 * a cone, a torus and a capsule, each in a unit box around the origin. Each
 * shape is built from grids of vertices; triangles are then turned to face
 * the way their vertices' normals say, so that every shape winds
 * counter-clockwise seen from outside without each generator minding it. */
#include "badger/renderer.h"

#include "badger/assert.h"
#include "badger/memory.h"

#define TB_SHAPE_DEFAULT_DETAIL 32u
#define TB_SHAPE_LEAST_DETAIL 3u

typedef struct tb_shape
{
    tb_mesh_vertex *vertices;
    size_t vertex_count;
    size_t vertex_capacity;
    uint32_t *indices;
    size_t index_count;
    size_t index_capacity;
} tb_shape;

static uint32_t tb_shape_vertex(tb_shape *shape, tb_vec3 position, tb_vec3 normal, tb_vec2 uv)
{
    if (shape->vertex_count == shape->vertex_capacity)
    {
        shape->vertex_capacity = shape->vertex_capacity == 0 ? 64 : shape->vertex_capacity * 2;
        shape->vertices =
            tb_realloc_array(shape->vertices, shape->vertex_capacity, sizeof(*shape->vertices));
    }
    shape->vertices[shape->vertex_count] =
        (tb_mesh_vertex){position, tb_vec3_normalize(normal), uv};
    shape->vertex_count++;
    return (uint32_t)(shape->vertex_count - 1);
}

static void tb_shape_triangle(tb_shape *shape, uint32_t a, uint32_t b, uint32_t c)
{
    if (shape->index_count + 3 > shape->index_capacity)
    {
        shape->index_capacity = shape->index_capacity == 0 ? 192 : shape->index_capacity * 2;
        shape->indices =
            tb_realloc_array(shape->indices, shape->index_capacity, sizeof(*shape->indices));
    }
    shape->indices[shape->index_count] = a;
    shape->indices[shape->index_count + 1] = b;
    shape->indices[shape->index_count + 2] = c;
    shape->index_count += 3;
}

/* Join a grid of `rows` by `columns` vertices, made row after row from
 * `first`, into quads of two triangles. */
static void tb_shape_grid(tb_shape *shape, uint32_t first, uint32_t rows, uint32_t columns)
{
    for (uint32_t row = 0; row + 1 < rows; row++)
    {
        for (uint32_t column = 0; column + 1 < columns; column++)
        {
            const uint32_t a = first + row * columns + column;
            const uint32_t b = a + 1;
            const uint32_t c = a + columns;
            const uint32_t d = c + 1;
            tb_shape_triangle(shape, a, c, b);
            tb_shape_triangle(shape, b, c, d);
        }
    }
}

/* A disc of `segments` around Y at height `y`, facing up or down. */
static void tb_shape_cap(tb_shape *shape, float y, float radius, uint32_t segments, bool up)
{
    const tb_vec3 normal = {0.0f, up ? 1.0f : -1.0f, 0.0f};
    const uint32_t center =
        tb_shape_vertex(shape, (tb_vec3){0.0f, y, 0.0f}, normal, (tb_vec2){0.5f, 0.5f});

    for (uint32_t segment = 0; segment <= segments; segment++)
    {
        const float angle = TB_TAU * (float)segment / (float)segments;
        const float x = cosf(angle);
        const float z = sinf(angle);
        (void)tb_shape_vertex(shape, (tb_vec3){x * radius, y, z * radius}, normal,
                              (tb_vec2){0.5f + x * 0.5f, 0.5f + z * 0.5f});
    }
    for (uint32_t segment = 0; segment < segments; segment++)
    {
        tb_shape_triangle(shape, center, center + 1 + segment, center + 2 + segment);
    }
}

static void tb_shape_cube(tb_shape *shape)
{
    static const tb_vec3 normals[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

    for (int face = 0; face < 6; face++)
    {
        const tb_vec3 normal = normals[face];
        const tb_vec3 up = fabsf(normal.y) > 0.5f ? (tb_vec3){0, 0, 1} : (tb_vec3){0, 1, 0};
        const tb_vec3 side = tb_vec3_cross(up, normal);
        const uint32_t first = (uint32_t)shape->vertex_count;
        for (int corner = 0; corner < 4; corner++)
        {
            const tb_vec3 point =
                tb_vec3_add(normal, tb_vec3_add(tb_vec3_scale(side, corners[corner][0]),
                                                tb_vec3_scale(up, corners[corner][1])));
            (void)tb_shape_vertex(
                shape, tb_vec3_scale(point, 0.5f), normal,
                (tb_vec2){(corners[corner][0] + 1.0f) * 0.5f, (1.0f - corners[corner][1]) * 0.5f});
        }
        tb_shape_triangle(shape, first, first + 1, first + 2);
        tb_shape_triangle(shape, first, first + 2, first + 3);
    }
}

static void tb_shape_plane(tb_shape *shape)
{
    const tb_vec3 up = {0.0f, 1.0f, 0.0f};
    const uint32_t first = tb_shape_vertex(shape, (tb_vec3){-0.5f, 0, -0.5f}, up, (tb_vec2){0, 0});

    (void)tb_shape_vertex(shape, (tb_vec3){0.5f, 0, -0.5f}, up, (tb_vec2){1, 0});
    (void)tb_shape_vertex(shape, (tb_vec3){-0.5f, 0, 0.5f}, up, (tb_vec2){0, 1});
    (void)tb_shape_vertex(shape, (tb_vec3){0.5f, 0, 0.5f}, up, (tb_vec2){1, 1});
    tb_shape_grid(shape, first, 2, 2);
}

/* Rings of a sphere of `radius`, from the top pole to the bottom, the upper
 * half raised and the lower lowered by `stretch`: a sphere at 0, a capsule
 * above. */
static void tb_shape_rings(tb_shape *shape, float radius, float stretch, uint32_t segments)
{
    const uint32_t rings = segments / 2u;
    const uint32_t first = (uint32_t)shape->vertex_count;
    uint32_t rows = 0;

    for (uint32_t ring = 0; ring <= rings; ring++)
    {
        /* A capsule's equator is made twice: once for each half. */
        const uint32_t copies = stretch > 0.0f && ring * 2u == rings ? 2u : 1u;
        for (uint32_t copy = 0; copy < copies; copy++)
        {
            const float polar = TB_PI * (float)ring / (float)rings;
            const bool upper = ring * 2u < rings || (ring * 2u == rings && copy == 0);
            const float offset = upper ? stretch : -stretch;
            for (uint32_t segment = 0; segment <= segments; segment++)
            {
                const float angle = TB_TAU * (float)segment / (float)segments;
                const tb_vec3 normal = {sinf(polar) * cosf(angle), cosf(polar),
                                        sinf(polar) * sinf(angle)};
                const tb_vec3 position =
                    tb_vec3_add(tb_vec3_scale(normal, radius), (tb_vec3){0.0f, offset, 0.0f});
                (void)tb_shape_vertex(
                    shape, position, normal,
                    (tb_vec2){(float)segment / (float)segments, (float)ring / (float)rings});
            }
            rows++;
        }
    }
    tb_shape_grid(shape, first, rows, segments + 1);
}

/* The side of a cylinder or cone from `bottom` radius at y −0.5 to `top`
 * radius at y 0.5. */
static void tb_shape_side(tb_shape *shape, float bottom, float top, uint32_t segments)
{
    const uint32_t first = (uint32_t)shape->vertex_count;
    /* The slope of the side tilts its normals up as it narrows. */
    const float rise = bottom - top;

    for (uint32_t row = 0; row < 2; row++)
    {
        const float radius = row == 0 ? top : bottom;
        const float y = row == 0 ? 0.5f : -0.5f;
        for (uint32_t segment = 0; segment <= segments; segment++)
        {
            const float angle = TB_TAU * (float)segment / (float)segments;
            const tb_vec3 normal = {cosf(angle), rise, sinf(angle)};
            (void)tb_shape_vertex(shape, (tb_vec3){cosf(angle) * radius, y, sinf(angle) * radius},
                                  normal, (tb_vec2){(float)segment / (float)segments, (float)row});
        }
    }
    tb_shape_grid(shape, first, 2, segments + 1);
}

static void tb_shape_torus(tb_shape *shape, uint32_t segments)
{
    const float major = 0.375f;
    const float minor = 0.125f;
    const uint32_t sides =
        segments / 2u < TB_SHAPE_LEAST_DETAIL ? TB_SHAPE_LEAST_DETAIL : segments / 2u;
    const uint32_t first = (uint32_t)shape->vertex_count;

    for (uint32_t ring = 0; ring <= segments; ring++)
    {
        const float around = TB_TAU * (float)ring / (float)segments;
        const tb_vec3 center = {cosf(around) * major, 0.0f, sinf(around) * major};
        for (uint32_t side = 0; side <= sides; side++)
        {
            const float tube = TB_TAU * (float)side / (float)sides;
            const tb_vec3 normal = {cosf(around) * cosf(tube), sinf(tube),
                                    sinf(around) * cosf(tube)};
            (void)tb_shape_vertex(
                shape, tb_vec3_add(center, tb_vec3_scale(normal, minor)), normal,
                (tb_vec2){(float)ring / (float)segments, (float)side / (float)sides});
        }
    }
    tb_shape_grid(shape, first, segments + 1, sides + 1);
}

/* Turn each triangle to face the way its vertices' normals point. */
static void tb_shape_face_outward(tb_shape *shape)
{
    for (size_t index = 0; index < shape->index_count; index += 3)
    {
        const tb_mesh_vertex *const a = &shape->vertices[shape->indices[index]];
        const tb_mesh_vertex *const b = &shape->vertices[shape->indices[index + 1]];
        const tb_mesh_vertex *const c = &shape->vertices[shape->indices[index + 2]];
        const tb_vec3 face = tb_vec3_cross(tb_vec3_sub(b->position, a->position),
                                           tb_vec3_sub(c->position, a->position));
        const tb_vec3 normal = tb_vec3_add(a->normal, tb_vec3_add(b->normal, c->normal));
        if (tb_vec3_dot(face, normal) < 0.0f)
        {
            const uint32_t swap = shape->indices[index + 1];
            shape->indices[index + 1] = shape->indices[index + 2];
            shape->indices[index + 2] = swap;
        }
    }
}

void tb_mesh_generate(tb_mesh_shape shape, uint32_t detail, tb_mesh_vertex **vertices,
                      size_t *vertex_count, uint32_t **indices, size_t *index_count)
{
    tb_shape made = {0};
    uint32_t segments = detail == 0 ? TB_SHAPE_DEFAULT_DETAIL : detail;

    TB_ASSERT(vertices != NULL && vertex_count != NULL && indices != NULL && index_count != NULL);
    segments = segments < TB_SHAPE_LEAST_DETAIL ? TB_SHAPE_LEAST_DETAIL : segments;
    segments += segments % 2u; /* rings are half the segments, and a capsule splits them */
    switch (shape)
    {
    case TB_MESH_CUBE:
        tb_shape_cube(&made);
        break;
    case TB_MESH_PLANE:
        tb_shape_plane(&made);
        break;
    case TB_MESH_SPHERE:
        tb_shape_rings(&made, 0.5f, 0.0f, segments);
        break;
    case TB_MESH_CYLINDER:
        tb_shape_side(&made, 0.5f, 0.5f, segments);
        tb_shape_cap(&made, 0.5f, 0.5f, segments, true);
        tb_shape_cap(&made, -0.5f, 0.5f, segments, false);
        break;
    case TB_MESH_CONE:
        tb_shape_side(&made, 0.5f, 0.0f, segments);
        tb_shape_cap(&made, -0.5f, 0.5f, segments, false);
        break;
    case TB_MESH_TORUS:
        tb_shape_torus(&made, segments);
        break;
    case TB_MESH_CAPSULE:
        tb_shape_rings(&made, 0.25f, 0.25f, segments);
        break;
    }
    TB_ASSERT_MSG(made.index_count > 0, "not a shape");
    tb_shape_face_outward(&made);
    *vertices = made.vertices;
    *vertex_count = made.vertex_count;
    *indices = made.indices;
    *index_count = made.index_count;
}
