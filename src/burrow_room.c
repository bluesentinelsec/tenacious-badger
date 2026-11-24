/* Burrow's room: its pieces, and their triangles. */
#include "burrow_room.h"

#include "badger/assert.h"

#include <math.h>

/* How far a face's normal may be from straight up or down for its texture
 * to lie along X and Z, as a floor's does. */
#define BURROW_LEVEL_FACE 0.999f

/* The most corners of a face: a pillar's cap. */
#define BURROW_MOST_CORNERS BURROW_PILLAR_SIDES

/* The room, 24 meters square inside walls 6 meters high, open to the sky.
 * The player starts at its south side looking north, across cover and
 * pillars toward a ramp that climbs to a platform; steps rise along the
 * west, and boxes of one and two meters stand in the north-west corner. */
static const burrow_piece burrow_graybox_pieces[] = {
    /* The floor and the walls. */
    {BURROW_SHAPE_BOX, BURROW_SURFACE_FLOOR, {0.0f, -0.25f, 0.0f}, {24.0f, 0.5f, 24.0f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_WALL, {0.0f, 3.0f, -12.25f}, {25.0f, 6.0f, 0.5f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_WALL, {0.0f, 3.0f, 12.25f}, {25.0f, 6.0f, 0.5f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_WALL, {-12.25f, 3.0f, 0.0f}, {0.5f, 6.0f, 24.0f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_WALL, {12.25f, 3.0f, 0.0f}, {0.5f, 6.0f, 24.0f}, 0},
    /* Boxes of one and two meters. */
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {-9.0f, 0.5f, -9.0f}, {1.0f, 1.0f, 1.0f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {-6.5f, 1.0f, -9.0f}, {2.0f, 2.0f, 2.0f}, 0},
    /* A platform three meters up, and the ramp that climbs to it from the
     * south. */
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {6.0f, 1.5f, -8.0f}, {6.0f, 3.0f, 4.0f}, 0},
    {BURROW_SHAPE_RAMP, BURROW_SURFACE_BLOCK, {6.0f, 1.5f, -3.0f}, {3.0f, 3.0f, 6.0f}, 0},
    /* Steps of half a meter along the west wall. */
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {-11.0f, 0.25f, 6.0f}, {2.0f, 0.5f, 1.0f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {-11.0f, 0.5f, 5.0f}, {2.0f, 1.0f, 1.0f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {-11.0f, 0.75f, 4.0f}, {2.0f, 1.5f, 1.0f}, 0},
    {BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {-11.0f, 1.0f, 3.0f}, {2.0f, 2.0f, 1.0f}, 0},
    /* A low ramp rising toward the west, a meter high. */
    {BURROW_SHAPE_RAMP, BURROW_SURFACE_BLOCK, {-5.0f, 0.5f, 4.0f}, {2.0f, 1.0f, 4.0f}, 1},
    /* Cover a meter high, across the middle. */
    {BURROW_SHAPE_BOX, BURROW_SURFACE_ACCENT, {0.0f, 0.5f, 0.0f}, {4.0f, 1.0f, 0.5f}, 0},
    /* Pillars: two of four meters, one to the top of the walls, and a thin
     * one of two. */
    {BURROW_SHAPE_PILLAR, BURROW_SURFACE_ACCENT, {-3.0f, 2.0f, -4.0f}, {1.0f, 4.0f, 1.0f}, 0},
    {BURROW_SHAPE_PILLAR, BURROW_SURFACE_ACCENT, {2.0f, 2.0f, 4.0f}, {1.0f, 4.0f, 1.0f}, 0},
    {BURROW_SHAPE_PILLAR, BURROW_SURFACE_ACCENT, {9.0f, 3.0f, 8.0f}, {1.5f, 6.0f, 1.5f}, 0},
    {BURROW_SHAPE_PILLAR, BURROW_SURFACE_ACCENT, {-7.0f, 1.0f, -3.0f}, {0.5f, 2.0f, 0.5f}, 0},
};

static const burrow_room burrow_graybox = {
    .pieces = burrow_graybox_pieces,
    .piece_count = sizeof(burrow_graybox_pieces) / sizeof(burrow_graybox_pieces[0]),
    .spawn = {0.0f, 1.7f, 9.0f},
    .spawn_yaw = 0.0f,
};

const burrow_room *burrow_room_graybox(void)
{
    return &burrow_graybox;
}

/* A point of a piece's unit box, -0.5 to 0.5 on each axis, in the world. */
static tb_vec3 burrow_place(const burrow_piece *piece, tb_vec3 unit)
{
    const tb_vec3 scaled = tb_vec3_mul(unit, piece->size);
    tb_vec3 turned = scaled;

    /* A quarter turn counter-clockwise seen from above takes -Z to -X. */
    for (uint32_t turn = 0; turn < piece->quarter_turns % 4u; turn++)
    {
        turned = (tb_vec3){turned.z, turned.y, -turned.x};
    }
    return tb_vec3_add(piece->center, turned);
}

/* A point's texture coordinates on a face: meters along the face, across
 * and down it. A level face lies along X and Z; any other along its level
 * edge and up its slope, so a ramp's grid keeps its size. */
static tb_vec2 burrow_face_uv(tb_vec3 normal, tb_vec3 point)
{
    tb_vec3 across = {1.0f, 0.0f, 0.0f};
    tb_vec3 down = {0.0f, 0.0f, 1.0f};

    if (fabsf(normal.y) < BURROW_LEVEL_FACE)
    {
        across = tb_vec3_normalize(tb_vec3_cross((tb_vec3){0.0f, 1.0f, 0.0f}, normal));
        down = tb_vec3_negate(tb_vec3_cross(normal, across));
    }
    return (tb_vec2){tb_vec3_dot(point, across), tb_vec3_dot(point, down)};
}

/* Add a flat, convex face of the piece: `count` corners of its unit box,
 * counter-clockwise seen from outside. */
static void burrow_add_face(burrow_geometry *geometry, const burrow_piece *piece,
                            const tb_vec3 *corners, size_t count)
{
    tb_vec3 placed[BURROW_MOST_CORNERS] = {0};
    tb_vec3 normal = {0};
    const size_t first = geometry->vertex_count;

    TB_ASSERT(count >= 3u && count <= BURROW_MOST_CORNERS);
    TB_ASSERT(first + count <= BURROW_PIECE_MOST_VERTICES);
    TB_ASSERT(geometry->index_count + (count - 2u) * 3u <= BURROW_PIECE_MOST_INDICES);
    for (size_t index = 0; index < count; index++)
    {
        placed[index] = burrow_place(piece, corners[index]);
    }
    normal = tb_vec3_normalize(
        tb_vec3_cross(tb_vec3_sub(placed[1], placed[0]), tb_vec3_sub(placed[2], placed[0])));
    for (size_t index = 0; index < count; index++)
    {
        geometry->vertices[first + index] = (tb_mesh_vertex){
            .position = placed[index],
            .normal = normal,
            .uv = burrow_face_uv(normal, placed[index]),
        };
    }
    /* A fan from the first corner. */
    for (size_t index = 1; index + 1 < count; index++)
    {
        geometry->indices[geometry->index_count++] = (uint32_t)first;
        geometry->indices[geometry->index_count++] = (uint32_t)(first + index);
        geometry->indices[geometry->index_count++] = (uint32_t)(first + index + 1u);
    }
    geometry->vertex_count = first + count;
}

/* A face of the unit box: its middle `n` / 2, and its edges `u` and `v`,
 * whose cross product is `n`, outward. */
static void burrow_add_box_face(burrow_geometry *geometry, const burrow_piece *piece, tb_vec3 n,
                                tb_vec3 u, tb_vec3 v)
{
    const tb_vec3 middle = tb_vec3_scale(n, 0.5f);
    const tb_vec3 half_u = tb_vec3_scale(u, 0.5f);
    const tb_vec3 half_v = tb_vec3_scale(v, 0.5f);
    const tb_vec3 corners[4] = {
        tb_vec3_sub(tb_vec3_sub(middle, half_u), half_v),
        tb_vec3_sub(tb_vec3_add(middle, half_u), half_v),
        tb_vec3_add(tb_vec3_add(middle, half_u), half_v),
        tb_vec3_add(tb_vec3_sub(middle, half_u), half_v),
    };

    burrow_add_face(geometry, piece, corners, 4u);
}

static void burrow_box_geometry(const burrow_piece *piece, burrow_geometry *geometry)
{
    burrow_add_box_face(geometry, piece, (tb_vec3){1, 0, 0}, (tb_vec3){0, 0, -1},
                        (tb_vec3){0, 1, 0});
    burrow_add_box_face(geometry, piece, (tb_vec3){-1, 0, 0}, (tb_vec3){0, 0, 1},
                        (tb_vec3){0, 1, 0});
    burrow_add_box_face(geometry, piece, (tb_vec3){0, 1, 0}, (tb_vec3){1, 0, 0},
                        (tb_vec3){0, 0, -1});
    burrow_add_box_face(geometry, piece, (tb_vec3){0, -1, 0}, (tb_vec3){1, 0, 0},
                        (tb_vec3){0, 0, 1});
    burrow_add_box_face(geometry, piece, (tb_vec3){0, 0, 1}, (tb_vec3){1, 0, 0},
                        (tb_vec3){0, 1, 0});
    burrow_add_box_face(geometry, piece, (tb_vec3){0, 0, -1}, (tb_vec3){-1, 0, 0},
                        (tb_vec3){0, 1, 0});
}

static void burrow_ramp_geometry(const burrow_piece *piece, burrow_geometry *geometry)
{
    const tb_vec3 slope[4] = {
        {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}};
    const tb_vec3 east[3] = {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}};
    const tb_vec3 west[3] = {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}};

    burrow_add_face(geometry, piece, slope, 4u);
    burrow_add_face(geometry, piece, east, 3u);
    burrow_add_face(geometry, piece, west, 3u);
    burrow_add_box_face(geometry, piece, (tb_vec3){0, -1, 0}, (tb_vec3){1, 0, 0},
                        (tb_vec3){0, 0, 1});
    burrow_add_box_face(geometry, piece, (tb_vec3){0, 0, -1}, (tb_vec3){-1, 0, 0},
                        (tb_vec3){0, 1, 0});
}

/* A corner of a pillar's unit cylinder at its height, `side` of the way
 * around counter-clockwise seen from above, from +X. */
static tb_vec3 burrow_pillar_corner(uint32_t side, float height)
{
    const float angle = TB_TAU * (float)(side % BURROW_PILLAR_SIDES) / (float)BURROW_PILLAR_SIDES;

    return (tb_vec3){0.5f * cosf(angle), height, -0.5f * sinf(angle)};
}

static void burrow_pillar_geometry(const burrow_piece *piece, burrow_geometry *geometry)
{
    tb_vec3 top[BURROW_PILLAR_SIDES] = {0};
    tb_vec3 bottom[BURROW_PILLAR_SIDES] = {0};

    for (uint32_t side = 0; side < BURROW_PILLAR_SIDES; side++)
    {
        const tb_vec3 quad[4] = {
            burrow_pillar_corner(side, -0.5f), burrow_pillar_corner(side + 1u, -0.5f),
            burrow_pillar_corner(side + 1u, 0.5f), burrow_pillar_corner(side, 0.5f)};
        burrow_add_face(geometry, piece, quad, 4u);
        /* The top runs counter-clockwise seen from above, the bottom the
         * other way, so that each is counter-clockwise from outside. */
        top[side] = burrow_pillar_corner(side, 0.5f);
        bottom[side] = burrow_pillar_corner(BURROW_PILLAR_SIDES - side, -0.5f);
    }
    burrow_add_face(geometry, piece, top, BURROW_PILLAR_SIDES);
    burrow_add_face(geometry, piece, bottom, BURROW_PILLAR_SIDES);
}

void burrow_piece_geometry(const burrow_piece *piece, burrow_geometry *geometry)
{
    geometry->vertex_count = 0;
    geometry->index_count = 0;
    switch (piece->shape)
    {
    case BURROW_SHAPE_BOX:
        burrow_box_geometry(piece, geometry);
        break;
    case BURROW_SHAPE_RAMP:
        burrow_ramp_geometry(piece, geometry);
        break;
    case BURROW_SHAPE_PILLAR:
        burrow_pillar_geometry(piece, geometry);
        break;
    /* coverage-off: no piece is of the count, which only ends the list of shapes */
    case BURROW_SHAPE_COUNT:
        TB_ASSERT_MSG(false, "a piece of no shape");
        break;
        /* coverage-on */
    }
}
