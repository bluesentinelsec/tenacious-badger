/* Burrow's room must make every piece as a closed, convex solid seen from
 * outside: each triangle wound counter-clockwise toward its normal, each
 * normal of unit length and outward. Texture coordinates must be meters
 * along each face, so that the grid is the same size on every piece and up
 * a ramp's slope. The graybox room must stand on its floor inside its
 * walls, with the player starting in the open. */
#include "burrow_room.h"

#include "tb_test.h"

#include <math.h>

#define NEAR 1e-4

/* The middle of a piece's vertices: inside it, since every piece is
 * convex. */
static tb_vec3 middle_of(const burrow_geometry *geometry)
{
    tb_vec3 sum = {0};

    for (size_t index = 0; index < geometry->vertex_count; index++)
    {
        sum = tb_vec3_add(sum, geometry->vertices[index].position);
    }
    return tb_vec3_scale(sum, 1.0f / (float)geometry->vertex_count);
}

/* Every triangle faces the way its vertices' normals say, and they point
 * out of the piece; every vertex is within the piece's box. */
static void expect_solid(const burrow_piece *piece, const burrow_geometry *geometry)
{
    const tb_vec3 middle = middle_of(geometry);
    const float reach = tb_vec3_length(tb_vec3_scale(piece->size, 0.5f)) + (float)NEAR;

    TB_TEST_REQUIRE_UINT(geometry->index_count % 3u, ==, 0u);
    for (size_t index = 0; index < geometry->index_count; index += 3u)
    {
        const tb_mesh_vertex *const a = &geometry->vertices[geometry->indices[index]];
        const tb_mesh_vertex *const b = &geometry->vertices[geometry->indices[index + 1u]];
        const tb_mesh_vertex *const c = &geometry->vertices[geometry->indices[index + 2u]];
        const tb_vec3 facing = tb_vec3_cross(tb_vec3_sub(b->position, a->position),
                                             tb_vec3_sub(c->position, a->position));
        TB_TEST_CONTEXT("triangle %zu", index / 3u);
        TB_TEST_EXPECT(tb_vec3_dot(facing, a->normal) > 0.0f);
        TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(a->normal), 1.0, NEAR);
        TB_TEST_EXPECT(tb_vec3_dot(a->normal, tb_vec3_sub(a->position, middle)) > 0.0f);
    }
    for (size_t index = 0; index < geometry->vertex_count; index++)
    {
        TB_TEST_EXPECT(tb_vec3_distance(geometry->vertices[index].position, piece->center) <=
                       reach);
    }
    for (size_t index = 0; index < geometry->index_count; index++)
    {
        TB_TEST_EXPECT_UINT(geometry->indices[index], <, geometry->vertex_count);
    }
}

/* The vertex of the geometry with the greatest dot product with `toward`. */
static const tb_mesh_vertex *farthest(const burrow_geometry *geometry, tb_vec3 toward)
{
    const tb_mesh_vertex *best = &geometry->vertices[0];

    for (size_t index = 1; index < geometry->vertex_count; index++)
    {
        const tb_mesh_vertex *const vertex = &geometry->vertices[index];
        if (tb_vec3_dot(vertex->position, toward) > tb_vec3_dot(best->position, toward))
        {
            best = vertex;
        }
    }
    return best;
}

TB_TEST(burrow_room, a_box_is_six_faces_wound_outward)
{
    static burrow_geometry geometry = {0};
    const burrow_piece box = {
        BURROW_SHAPE_BOX, BURROW_SURFACE_BLOCK, {2.0f, 1.0f, -3.0f}, {4.0f, 2.0f, 1.0f}, 0};

    burrow_piece_geometry(&box, &geometry);
    TB_TEST_EXPECT_UINT(geometry.vertex_count, ==, 24u);
    TB_TEST_EXPECT_UINT(geometry.index_count, ==, 36u);
    expect_solid(&box, &geometry);
    /* Its corners are its size around its center. */
    const tb_mesh_vertex *const corner = farthest(&geometry, (tb_vec3){1.0f, 1.0f, 1.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(corner->position.x, 4.0, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(corner->position.y, 2.0, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(corner->position.z, -2.5, NEAR);
}

TB_TEST(burrow_room, texture_coordinates_are_meters_in_the_world)
{
    static burrow_geometry geometry = {0};
    const burrow_piece floor = {
        BURROW_SHAPE_BOX, BURROW_SURFACE_FLOOR, {1.0f, -0.25f, 2.0f}, {10.0f, 0.5f, 6.0f}, 0};

    burrow_piece_geometry(&floor, &geometry);
    for (size_t index = 0; index < geometry.vertex_count; index++)
    {
        const tb_mesh_vertex *const vertex = &geometry.vertices[index];
        TB_TEST_CONTEXT("vertex %zu", index);
        if (vertex->normal.y > 0.5f)
        {
            /* The top lies along X and Z, so a grid lines up from piece to
             * piece. */
            TB_TEST_EXPECT_FLOAT_NEAR(vertex->uv.x, vertex->position.x, NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(vertex->uv.y, vertex->position.z, NEAR);
        }
        else if (fabsf(vertex->normal.z) > 0.5f)
        {
            /* A side facing along Z runs along X, and down as Y falls. */
            TB_TEST_EXPECT_FLOAT_NEAR(fabsf(vertex->uv.x), fabsf(vertex->position.x), NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(vertex->uv.y, -vertex->position.y, NEAR);
        }
        else if (fabsf(vertex->normal.x) > 0.5f)
        {
            TB_TEST_EXPECT_FLOAT_NEAR(fabsf(vertex->uv.x), fabsf(vertex->position.z), NEAR);
            TB_TEST_EXPECT_FLOAT_NEAR(vertex->uv.y, -vertex->position.y, NEAR);
        }
    }
}

TB_TEST(burrow_room, a_ramp_rises_to_its_back_and_its_grid_follows_the_slope)
{
    static burrow_geometry geometry = {0};
    const burrow_piece ramp = {
        BURROW_SHAPE_RAMP, BURROW_SURFACE_BLOCK, {0.0f, 1.5f, 0.0f}, {3.0f, 3.0f, 6.0f}, 0};
    const tb_vec3 slope_normal = tb_vec3_normalize((tb_vec3){0.0f, 6.0f, 3.0f});
    float lowest = INFINITY;
    float highest = -INFINITY;

    burrow_piece_geometry(&ramp, &geometry);
    /* The slope, the back and the bottom are quads; the sides triangles. */
    TB_TEST_EXPECT_UINT(geometry.vertex_count, ==, 18u);
    TB_TEST_EXPECT_UINT(geometry.index_count, ==, 24u);
    expect_solid(&ramp, &geometry);
    /* The highest point is at the back, -Z. */
    const tb_mesh_vertex *const top = farthest(&geometry, (tb_vec3){0.0f, 1.0f, -0.001f});
    TB_TEST_EXPECT_FLOAT_NEAR(top->position.y, 3.0, NEAR);
    TB_TEST_EXPECT_FLOAT_NEAR(top->position.z, -3.0, NEAR);
    for (size_t index = 0; index < geometry.vertex_count; index++)
    {
        const tb_mesh_vertex *const vertex = &geometry.vertices[index];
        if (tb_vec3_dot(vertex->normal, slope_normal) < 1.0f - (float)NEAR)
        {
            continue;
        }
        lowest = fminf(lowest, vertex->uv.y);
        highest = fmaxf(highest, vertex->uv.y);
    }
    /* Up the slope is its length, not its depth. */
    TB_TEST_EXPECT_FLOAT_NEAR(highest - lowest, sqrtf(3.0f * 3.0f + 6.0f * 6.0f), NEAR);
}

TB_TEST(burrow_room, a_quarter_turn_turns_a_ramp_to_rise_toward_negative_x)
{
    static burrow_geometry geometry = {0};
    const burrow_piece ramp = {
        BURROW_SHAPE_RAMP, BURROW_SURFACE_BLOCK, {5.0f, 0.5f, 4.0f}, {2.0f, 1.0f, 4.0f}, 1};

    burrow_piece_geometry(&ramp, &geometry);
    expect_solid(&ramp, &geometry);
    const tb_mesh_vertex *const top = farthest(&geometry, (tb_vec3){-0.001f, 1.0f, 0.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(top->position.y, 1.0, NEAR);
    /* Its depth of 4 now lies along X. */
    TB_TEST_EXPECT_FLOAT_NEAR(top->position.x, 3.0, NEAR);
    const tb_mesh_vertex *const side = farthest(&geometry, (tb_vec3){0.0f, 0.0f, 1.0f});
    TB_TEST_EXPECT_FLOAT_NEAR(side->position.z, 5.0, NEAR);
}

TB_TEST(burrow_room, a_pillar_is_a_closed_prism_of_its_sides)
{
    static burrow_geometry geometry = {0};
    const burrow_piece pillar = {
        BURROW_SHAPE_PILLAR, BURROW_SURFACE_ACCENT, {-3.0f, 2.0f, 1.0f}, {1.0f, 4.0f, 1.0f}, 0};

    burrow_piece_geometry(&pillar, &geometry);
    /* A quad for each side and a cap at each end. */
    TB_TEST_EXPECT_UINT(geometry.vertex_count, ==, BURROW_PILLAR_SIDES * 6u);
    TB_TEST_EXPECT_UINT(geometry.index_count, ==,
                        BURROW_PILLAR_SIDES * 6u + (BURROW_PILLAR_SIDES - 2u) * 6u);
    expect_solid(&pillar, &geometry);
    for (size_t index = 0; index < geometry.vertex_count; index++)
    {
        const tb_vec3 position = geometry.vertices[index].position;
        const float x = position.x - pillar.center.x;
        const float z = position.z - pillar.center.z;
        TB_TEST_EXPECT_FLOAT_NEAR(sqrtf(x * x + z * z), 0.5, NEAR);
        TB_TEST_EXPECT(position.y >= -(float)NEAR && position.y <= 4.0f + (float)NEAR);
    }
}

TB_TEST(burrow_room, the_graybox_room_stands_on_its_floor_inside_its_walls)
{
    static burrow_geometry geometry = {0};
    const burrow_room *const room = burrow_room_graybox();
    const float inside = 12.0f;

    TB_TEST_REQUIRE(room->piece_count > 5u);
    for (size_t index = 0; index < room->piece_count; index++)
    {
        const burrow_piece *const piece = &room->pieces[index];
        TB_TEST_CONTEXT("piece %zu", index);
        burrow_piece_geometry(piece, &geometry);
        expect_solid(piece, &geometry);
        /* Nothing sinks below the floor's top but the floor. */
        if (piece->surface != BURROW_SURFACE_FLOOR)
        {
            TB_TEST_EXPECT_FLOAT_NEAR(piece->center.y - piece->size.y * 0.5f, 0.0, NEAR);
        }
        /* Nothing but the walls reaches past them. */
        if (piece->surface != BURROW_SURFACE_WALL)
        {
            for (size_t vertex = 0; vertex < geometry.vertex_count; vertex++)
            {
                const tb_vec3 position = geometry.vertices[vertex].position;
                TB_TEST_EXPECT(fabsf(position.x) <= inside + (float)NEAR);
                TB_TEST_EXPECT(fabsf(position.z) <= inside + (float)NEAR);
            }
        }
    }
}

TB_TEST(burrow_room, the_player_starts_in_the_open_at_eye_height)
{
    const burrow_room *const room = burrow_room_graybox();
    const tb_vec3 spawn = room->spawn;

    TB_TEST_EXPECT(spawn.y > 1.0f && spawn.y < 2.0f);
    for (size_t index = 0; index < room->piece_count; index++)
    {
        const burrow_piece *const piece = &room->pieces[index];
        /* The box around a piece: a quarter turn swaps its width and depth. */
        const bool turned = piece->quarter_turns % 2u == 1u;
        const float half_x = (turned ? piece->size.z : piece->size.x) * 0.5f;
        const float half_y = piece->size.y * 0.5f;
        const float half_z = (turned ? piece->size.x : piece->size.z) * 0.5f;
        const bool within = fabsf(spawn.x - piece->center.x) < half_x &&
                            fabsf(spawn.y - piece->center.y) < half_y &&
                            fabsf(spawn.z - piece->center.z) < half_z;
        TB_TEST_CONTEXT("piece %zu", index);
        TB_TEST_EXPECT(!within);
    }
}
