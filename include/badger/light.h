/** @file light.h
 * @brief Lights of a renderer's scene, for the raster lighting mode:
 * directional, point and spot lights that last from frame to frame, named
 * by handles, and the ambient light of a scene without a sky.
 *
 * @code
 * tb_light sun = TB_LIGHT_INIT;
 * sun.kind = TB_LIGHT_DIRECTIONAL;
 * sun.direction = (tb_vec3){-0.3f, -1.0f, -0.4f};
 * sun.intensity = 3.0f;
 * const tb_light_id sun_id = tb_renderer_add_light(renderer, &sun);
 *
 * tb_light lamp = TB_LIGHT_INIT; // a point light
 * lamp.position = (tb_vec3){0, 2, 0};
 * lamp.color = (tb_vec3){1.0f, 0.8f, 0.6f};
 * const tb_light_id lamp_id = tb_renderer_add_light(renderer, &lamp);
 *
 * // later: move it, and take the sun away
 * lamp.position.x += 1.0f;
 * tb_renderer_set_light(renderer, lamp_id, &lamp);
 * tb_renderer_remove_light(renderer, sun_id);
 * @endcode
 *
 * **Lasting.** A light is part of the scene from the next frame on, until
 * it is removed, as a level's lights are: a game does not add it again each
 * frame. Its handle is good until then; a handle of a removed light names
 * nothing, even when another light takes its place.
 *
 * **Units.** Colors are linear RGB. A light of intensity 1 lights a white,
 * matte surface that faces it to 1: a directional light anywhere, a point
 * or spot light at a meter. Point and spot lights fall off with the square
 * of the distance, and smoothly to nothing at their range, as Unreal's do.
 *
 * **Shading.** Lights shade every lit mesh and material through the
 * G-buffer, with Unreal's default lit model: a Lambert diffuse and a GGX
 * specular with Smith visibility and Schlick's Fresnel (docs/renderer.md).
 * A frame without lights is lit by a default light: toward the procedural
 * sky's sun when there is one, else from above and behind the camera.
 *
 * **Ambient.** The sky lights the scene too: its colors, or its cube map,
 * give each surface the light of the sky above it and the ground below,
 * and reflections. Without a sky, tb_renderer_set_ambient() sets the
 * colors of that sky and ground.
 *
 * A device without bindless resources has no raster lighting mode: its
 * meshes are lit by the default light alone.
 *
 * @par Threading
 * Stage-owned: every function is called on the thread that renders, as the
 * renderer's are.
 */
#ifndef TB_LIGHT_H
#define TB_LIGHT_H

#include "badger/api.h"
#include "badger/math.h"
#include "badger/renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief What a light is. */
typedef enum tb_light_kind
{
    TB_LIGHT_DIRECTIONAL, /**< From far away, one way everywhere: the sun. */
    TB_LIGHT_POINT,       /**< From a point, every way: a bulb. */
    TB_LIGHT_SPOT         /**< From a point, in a cone: a flashlight. */
} tb_light_kind;

/** @brief A light. Start from #TB_LIGHT_INIT, then set what differs. */
typedef struct tb_light
{
    tb_light_kind kind; /**< Which of the fields below it uses. */
    tb_vec3 color;      /**< Linear RGB. */
    /** How bright: 1 lights a white, matte surface facing it to 1, at a
     * meter for a point or spot light. 0 or more. */
    float intensity;
    tb_vec3 position; /**< Point and spot: where it is, in the world. */
    /** Directional and spot: the way its light goes, in the world; need not
     * be of unit length, but not zero. */
    tb_vec3 direction;
    /** Point and spot: meters from it where its light ends; more than 0. */
    float range;
    /** Spot: radians from its axis where its light begins to fade; 0 or
     * more, less than `outer_angle`. */
    float inner_angle;
    /** Spot: radians from its axis where its light ends; less than π/2. */
    float outer_angle;
} tb_light;

/** @brief A white point light of intensity 1 at the origin, reaching 10
 * meters; as a spot, a cone of 20 degrees fading from 15, pointing down. */
#define TB_LIGHT_INIT                                                                              \
    {.kind = TB_LIGHT_POINT,                                                                       \
     .color = {1.0f, 1.0f, 1.0f},                                                                  \
     .intensity = 1.0f,                                                                            \
     .direction = {0.0f, -1.0f, 0.0f},                                                             \
     .range = 10.0f,                                                                               \
     .inner_angle = 0.2618f,                                                                       \
     .outer_angle = 0.3491f}

/** @brief A light of a renderer's scene. Zero names none. */
typedef struct tb_light_id
{
    uint32_t id; /**< Private. */
} tb_light_id;

/** @brief The most lights a renderer's scene holds at once. */
#define TB_RENDERER_MAX_LIGHTS 1024u

/**
 * @brief Add a light to the scene, from the next frame on.
 * @param renderer Required.
 * @param light Required; valid as #tb_light says; copied.
 * @return Its handle, good until it is removed; none when the scene holds
 * #TB_RENDERER_MAX_LIGHTS already.
 */
TB_API tb_light_id tb_renderer_add_light(tb_renderer *renderer, const tb_light *light);

/**
 * @brief Change a light, from the next frame on.
 * @param light Required; copied.
 * @return false, changing nothing, when `id` names no light of the scene.
 */
TB_API bool tb_renderer_set_light(tb_renderer *renderer, tb_light_id id, const tb_light *light);

/**
 * @brief Read a light of the scene.
 * @param light Required; receives the light when there is one.
 * @return false when `id` names no light of the scene.
 */
TB_API bool tb_renderer_get_light(const tb_renderer *renderer, tb_light_id id, tb_light *light);

/** @brief Take a light out of the scene, from the next frame on. A handle
 * that names no light is ignored. */
TB_API void tb_renderer_remove_light(tb_renderer *renderer, tb_light_id id);

/** @brief How many lights the scene holds. */
TB_API size_t tb_renderer_light_count(const tb_renderer *renderer);

/**
 * @brief Set the ambient light of frames without a sky: the light of a sky
 * above every surface and of a ground below it, blended by the way the
 * surface faces. A sky's own colors take its place while there is one.
 * @param sky Linear RGB from above; 0.25 gray by default.
 * @param ground Linear RGB from below; 0.08 gray by default.
 */
TB_API void tb_renderer_set_ambient(tb_renderer *renderer, tb_vec3 sky, tb_vec3 ground);

#endif
