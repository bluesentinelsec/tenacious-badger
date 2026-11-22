/** @file geometry.h
 * @brief Planes, boxes, spheres, rays and view frustums: the shapes that
 * culling, picking and physics queries test against one another.
 *
 * @code
 * const tb_frustum view = tb_frustum_from_matrix(tb_mat4_mul(projection, camera));
 *
 * if (tb_frustum_test_aabb(&view, bounds) != TB_CONTAINMENT_OUTSIDE)
 * {
 *     draw(object);
 * }
 * @endcode
 *
 * The conventions are those of math.h: right-handed, column vectors, and clip
 * space with reversed depth from 0 to 1. A box is aligned with the axes. A
 * ray's direction need not have length 1, and a distance along it counts in
 * lengths of its direction: tb_ray_at(ray, t) is `origin + t * direction`.
 *
 * @par Threading
 * Thread-safe: every function is pure, and reads only its arguments.
 */
#ifndef TB_GEOMETRY_H
#define TB_GEOMETRY_H

#include "badger/api.h"
#include "badger/math.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @brief A plane: the points `p` where `dot(normal, p) == distance`. `normal`
 * has length 1, and points to the plane's front.
 */
typedef struct tb_plane
{
    tb_vec3 normal; /**< Length 1, toward the front. */
    float distance; /**< How far the plane is from the origin, along `normal`. */
} tb_plane;

/** @brief A box aligned with the axes; `minimum` is at most `maximum` on every axis. */
typedef struct tb_aabb
{
    tb_vec3 minimum; /**< The corner with the smallest coordinates. */
    tb_vec3 maximum; /**< The corner with the largest coordinates. */
} tb_aabb;

/** @brief A sphere, or a ball: the points no farther than `radius` from `center`. */
typedef struct tb_sphere
{
    tb_vec3 center; /**< The center. */
    float radius;   /**< The radius, at least 0. */
} tb_sphere;

/** @brief A ray: the points `origin + t * direction` for every `t >= 0`. */
typedef struct tb_ray
{
    tb_vec3 origin;    /**< Where the ray starts. */
    tb_vec3 direction; /**< Where it goes; not 0, and of any length. */
} tb_ray;

/** @brief Where a shape is, relative to a frustum. */
typedef enum tb_containment
{
    TB_CONTAINMENT_OUTSIDE,      /**< Wholly outside. */
    TB_CONTAINMENT_INTERSECTING, /**< Partly inside, or too near an edge to tell cheaply. */
    TB_CONTAINMENT_INSIDE,       /**< Wholly inside. */
} tb_containment;

/** @brief The order of a frustum's planes. */
enum
{
    TB_FRUSTUM_LEFT,   /**< The left plane. */
    TB_FRUSTUM_RIGHT,  /**< The right plane. */
    TB_FRUSTUM_BOTTOM, /**< The bottom plane. */
    TB_FRUSTUM_TOP,    /**< The top plane. */
    TB_FRUSTUM_NEAR,   /**< The near plane. */
    TB_FRUSTUM_FAR,    /**< The far plane, which may be at infinity. */
    TB_FRUSTUM_PLANES, /**< How many planes a frustum has. */
};

/**
 * @brief What a camera sees: six planes facing inward. A far plane at
 * infinity has a normal of 0, and every point is in front of it.
 */
typedef struct tb_frustum
{
    tb_plane planes[TB_FRUSTUM_PLANES]; /**< In the order of #TB_FRUSTUM_LEFT and the rest. */
} tb_frustum;

/** @brief The plane through `point` facing `normal`, which must not be 0; it is normalized. */
TB_API tb_plane tb_plane_from_point_normal(tb_vec3 point, tb_vec3 normal);

/**
 * @brief The plane through three points, facing the side from which they turn
 * counterclockwise.
 * @return false, leaving `*plane` alone, when the points are on one line.
 */
TB_API bool tb_plane_from_points(tb_vec3 a, tb_vec3 b, tb_vec3 c, tb_plane *plane);

/** @brief How far `point` is in front of `plane`; negative behind it. */
TB_API float tb_plane_signed_distance(tb_plane plane, tb_vec3 point);

/** @brief The smallest box that holds `count` points, at least one. */
TB_API tb_aabb tb_aabb_from_points(const tb_vec3 *points, size_t count);

/** @brief The smallest box that holds both boxes. */
TB_API tb_aabb tb_aabb_union(tb_aabb a, tb_aabb b);

/** @brief Whether `point` is in the box or on its surface. */
TB_API bool tb_aabb_contains_point(tb_aabb box, tb_vec3 point);

/** @brief Whether two boxes share a point, touching included. */
TB_API bool tb_aabb_overlaps(tb_aabb a, tb_aabb b);

/** @brief The box's center. */
TB_API tb_vec3 tb_aabb_center(tb_aabb box);

/** @brief Half the box's size on each axis. */
TB_API tb_vec3 tb_aabb_extents(tb_aabb box);

/**
 * @brief The smallest box aligned with the axes that holds `box` after the
 * affine transform `m`: an object's bounds in the world, from its bounds in
 * its own space.
 */
TB_API tb_aabb tb_aabb_transform(tb_aabb box, tb_mat4 m);

/** @brief Whether `point` is in the sphere or on its surface. */
TB_API bool tb_sphere_contains_point(tb_sphere sphere, tb_vec3 point);

/** @brief Whether two spheres share a point, touching included. */
TB_API bool tb_sphere_overlaps(tb_sphere a, tb_sphere b);

/** @brief Whether a sphere and a box share a point, touching included. */
TB_API bool tb_sphere_overlaps_aabb(tb_sphere sphere, tb_aabb box);

/** @brief The point `origin + t * direction`. */
TB_API tb_vec3 tb_ray_at(tb_ray ray, float t);

/**
 * @brief Where the ray first meets `plane`, from either side.
 * @param[out] t How far along the ray, when it meets the plane.
 * @return false, leaving `*t` alone, when the ray is parallel to the plane or
 *         points away from it.
 */
TB_API bool tb_ray_intersect_plane(tb_ray ray, tb_plane plane, float *t);

/**
 * @brief Where the ray enters `box`.
 * @param[out] t How far along the ray, when it meets the box; 0 when the ray
 *         starts inside.
 * @return false, leaving `*t` alone, when the ray misses the box.
 */
TB_API bool tb_ray_intersect_aabb(tb_ray ray, tb_aabb box, float *t);

/**
 * @brief Where the ray enters `sphere`.
 * @param[out] t How far along the ray, when it meets the sphere; 0 when the
 *         ray starts inside.
 * @return false, leaving `*t` alone, when the ray misses the sphere.
 */
TB_API bool tb_ray_intersect_sphere(tb_ray ray, tb_sphere sphere, float *t);

/**
 * @brief The frustum of a camera, from the product of its projection and view
 * matrices (math.h's conventions), in the world's coordinates. With a model
 * matrix multiplied in too, the frustum is in that model's coordinates.
 */
TB_API tb_frustum tb_frustum_from_matrix(tb_mat4 view_projection);

/** @brief Whether `point` is in the frustum or on its surface. */
TB_API bool tb_frustum_contains_point(const tb_frustum *frustum, tb_vec3 point);

/** @brief Where `sphere` is, relative to the frustum. */
TB_API tb_containment tb_frustum_test_sphere(const tb_frustum *frustum, tb_sphere sphere);

/**
 * @brief Where `box` is, relative to the frustum. A large box beside a corner
 * of the frustum may be called intersecting when it is outside: a cull keeps
 * it, which is safe.
 */
TB_API tb_containment tb_frustum_test_aabb(const tb_frustum *frustum, tb_aabb box);

#endif
