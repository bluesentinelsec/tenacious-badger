/** @file sky.h
 * @brief The sky behind a renderer's scene: a procedural sky of a gradient
 * and a sun, computed in a shader with no texture, or a cube map a caller
 * fills.
 *
 * @code
 * tb_sky sky = TB_SKY_PROCEDURAL_INIT;
 * sky.sun_direction = (tb_vec3){0.4f, 0.5f, -1.0f};
 * tb_renderer_set_sky(renderer, &sky); // from the next frame on, until changed
 * @endcode
 *
 * **Where it is drawn.** The sky is drawn in the scene pass after the
 * meshes and custom draws, before the debug lines, on the pixels nothing
 * covered: a triangle over the whole view at the far plane (depth 0, as
 * depth is reversed), tested against the depth the scene wrote and writing
 * none. Pixels the scene covered fail the test before they are shaded, so
 * the sky costs what it shows, and the frame's clear color is behind
 * nothing. Without a sky the clear color shows, as before.
 *
 * **Directions** are the world's (Y up). A cube map's texel for a direction
 * is Direct3D's: the face of the direction's largest component, +X, -X, +Y,
 * -Y, +Z, -Z, each face laid out as Direct3D lays it (tb_texture_create_cube()).
 *
 * **Colors** are linear RGB, written to the frame as every draw's are.
 *
 * A device without bindless resources draws no sky: the clear color shows.
 *
 * @par Threading
 * Stage-owned: called on the thread that renders, as the renderer's
 * functions are.
 */
#ifndef TB_SKY_H
#define TB_SKY_H

#include "badger/api.h"
#include "badger/material.h"
#include "badger/math.h"
#include "badger/renderer.h"

/** @brief What a sky is made of. */
typedef enum tb_sky_kind
{
    TB_SKY_NONE,       /**< No sky: the frame's clear color. */
    TB_SKY_PROCEDURAL, /**< A gradient from the horizon to the zenith, a ground, and a sun. */
    TB_SKY_CUBE        /**< A cube map. */
} tb_sky_kind;

/** @brief A sky. Start from #TB_SKY_PROCEDURAL_INIT, or zero for none. */
typedef struct tb_sky
{
    tb_sky_kind kind; /**< Which of the fields below it uses. */
    /** Procedural: the color straight up. */
    tb_vec3 zenith;
    /** Procedural: the color at the horizon, from above and below. */
    tb_vec3 horizon;
    /** Procedural: the color below the horizon, which it reaches a little
     * below it. */
    tb_vec3 ground;
    /** Procedural: the way toward the sun, from anywhere in the world; need
     * not be of unit length. Zero for no sun. */
    tb_vec3 sun_direction;
    tb_vec3 sun_color; /**< Procedural: the color of the sun's disc. */
    /** Procedural: the sun's angular radius, in radians: 0.0047 is the real
     * sun's; a game's is usually larger. 0 for no sun. */
    float sun_radius;
    /** Cube: a texture of tb_texture_create_cube(), alive while it is the
     * sky. */
    const tb_texture *cube;
} tb_sky;

/** @brief A clear day: blue overhead, pale at the horizon, a dusty
 * ground, and a sun high to the south-west. */
#define TB_SKY_PROCEDURAL_INIT                                                                     \
    {.kind = TB_SKY_PROCEDURAL,                                                                    \
     .zenith = {0.10f, 0.25f, 0.60f},                                                              \
     .horizon = {0.60f, 0.70f, 0.80f},                                                             \
     .ground = {0.25f, 0.22f, 0.20f},                                                              \
     .sun_direction = {0.4f, 0.6f, 0.7f},                                                          \
     .sun_color = {1.0f, 0.95f, 0.85f},                                                            \
     .sun_radius = 0.03f}

/**
 * @brief Set the sky drawn behind every frame from the next on, until it is
 * set again. The sky is kept, not the description: a cube map is borrowed
 * until another sky replaces it.
 * @param renderer Required.
 * @param sky The sky, copied; NULL, or of #TB_SKY_NONE, for none.
 */
TB_API void tb_renderer_set_sky(tb_renderer *renderer, const tb_sky *sky);

#endif
