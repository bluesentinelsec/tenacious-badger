/* Burrow's room: the graybox level, as data, and the triangles of its
 * pieces.
 *
 * A room is a list of pieces: boxes, ramps and pillars, each a size and a
 * place in meters, with the surface it is drawn as. Their triangles are made
 * in the world's space with texture coordinates in meters along each face,
 * so that a texture repeats at the same size on every piece, whatever its
 * size: Unreal's world-aligned textures, which a graybox level's grid needs
 * to read as a scale.
 *
 * Threading: thread-safe; the room is constant, and making triangles
 * touches only the caller's geometry. */
#ifndef BURROW_ROOM_H
#define BURROW_ROOM_H

#include "badger/renderer.h"

#include <stddef.h>
#include <stdint.h>

/* The shapes of the pieces. Each fills the box of its size around its
 * center. */
typedef enum burrow_shape
{
    BURROW_SHAPE_BOX,    /* A box. */
    BURROW_SHAPE_RAMP,   /* A wedge rising from its front (+Z) to its full height at its back. */
    BURROW_SHAPE_PILLAR, /* An upright cylinder of BURROW_PILLAR_SIDES flat sides. */
    BURROW_SHAPE_COUNT
} burrow_shape;

/* What a piece is drawn as: each surface is the grid in a color of its
 * own. */
typedef enum burrow_surface
{
    BURROW_SURFACE_FLOOR,  /* The ground. */
    BURROW_SURFACE_WALL,   /* The room's walls. */
    BURROW_SURFACE_BLOCK,  /* Boxes, steps and ramps to climb. */
    BURROW_SURFACE_ACCENT, /* Pillars and cover, to judge distance by. */
    BURROW_SURFACE_COUNT
} burrow_surface;

/* The sides around a pillar. */
#define BURROW_PILLAR_SIDES 16u

/* The most vertices and indices a piece's triangles have: a pillar's. */
#define BURROW_PIECE_MOST_VERTICES (BURROW_PILLAR_SIDES * 6u)
#define BURROW_PIECE_MOST_INDICES (BURROW_PILLAR_SIDES * 12u)

/* A piece of the room. */
typedef struct burrow_piece
{
    burrow_shape shape;
    burrow_surface surface;
    tb_vec3 center; /* Meters. */
    tb_vec3 size;   /* Meters along each axis, before it is turned; each more than 0. */
    /* Quarter turns about +Y, counter-clockwise seen from above: a ramp of
     * 1 rises toward -X. */
    uint32_t quarter_turns;
} burrow_piece;

/* A room. */
typedef struct burrow_room
{
    const burrow_piece *pieces;
    size_t piece_count;
    tb_vec3 spawn;   /* Where the player's eye starts. */
    float spawn_yaw; /* Which way the player starts looking, as burrow_player's yaw. */
} burrow_room;

/* A piece's triangles, in the world's space, with texture coordinates in
 * meters. */
typedef struct burrow_geometry
{
    tb_mesh_vertex vertices[BURROW_PIECE_MOST_VERTICES];
    uint32_t indices[BURROW_PIECE_MOST_INDICES];
    size_t vertex_count;
    size_t index_count;
} burrow_geometry;

/* Burrow's graybox room: a floor and four walls open to the sky, with
 * boxes, steps, ramps and pillars at heights from half a meter to six. The
 * room is constant and lasts as long as the program. */
const burrow_room *burrow_room_graybox(void);

/* Make a piece's triangles: flat faces, counter-clockwise seen from
 * outside, each vertex with its face's outward normal and the coordinates
 * of its place across the face in meters. Replaces what `geometry` held. */
void burrow_piece_geometry(const burrow_piece *piece, burrow_geometry *geometry);

#endif
