/* The shapes of badger/geometry.h. */
#include "badger/geometry.h"

#include "badger/assert.h"

#include <float.h>
#include <math.h>

/* A squared length at or below this is no direction at all. */
#define TB_GEOMETRY_DEGENERATE_SQUARED 1e-12f

/* ---------------------------------------------------------------------------
 * Planes.
 * ------------------------------------------------------------------------- */

tb_plane tb_plane_from_point_normal(tb_vec3 point, tb_vec3 normal)
{
    TB_ASSERT_MSG(tb_vec3_length_squared(normal) > 0.0f, "a plane needs a normal");
    const tb_vec3 unit = tb_vec3_normalize(normal);
    const tb_plane plane = {.normal = unit, .distance = tb_vec3_dot(unit, point)};

    return plane;
}

bool tb_plane_from_points(tb_vec3 a, tb_vec3 b, tb_vec3 c, tb_plane *plane)
{
    TB_ASSERT(plane != NULL);
    const tb_vec3 normal = tb_vec3_cross(tb_vec3_sub(b, a), tb_vec3_sub(c, a));

    if (tb_vec3_length_squared(normal) <= TB_GEOMETRY_DEGENERATE_SQUARED)
    {
        return false;
    }
    *plane = tb_plane_from_point_normal(a, normal);
    return true;
}

float tb_plane_signed_distance(tb_plane plane, tb_vec3 point)
{
    return tb_vec3_dot(plane.normal, point) - plane.distance;
}

/* ---------------------------------------------------------------------------
 * Boxes.
 * ------------------------------------------------------------------------- */

tb_aabb tb_aabb_from_points(const tb_vec3 *points, size_t count)
{
    TB_ASSERT_MSG(points != NULL && count > 0, "a box holds at least one point");
    tb_aabb box = {.minimum = points[0], .maximum = points[0]};

    for (size_t index = 1; index < count; index++)
    {
        box.minimum = tb_vec3_min(box.minimum, points[index]);
        box.maximum = tb_vec3_max(box.maximum, points[index]);
    }
    return box;
}

tb_aabb tb_aabb_union(tb_aabb a, tb_aabb b)
{
    const tb_aabb box = {.minimum = tb_vec3_min(a.minimum, b.minimum),
                         .maximum = tb_vec3_max(a.maximum, b.maximum)};

    return box;
}

bool tb_aabb_contains_point(tb_aabb box, tb_vec3 point)
{
    return point.x >= box.minimum.x && point.x <= box.maximum.x && point.y >= box.minimum.y &&
           point.y <= box.maximum.y && point.z >= box.minimum.z && point.z <= box.maximum.z;
}

bool tb_aabb_overlaps(tb_aabb a, tb_aabb b)
{
    return a.minimum.x <= b.maximum.x && b.minimum.x <= a.maximum.x && a.minimum.y <= b.maximum.y &&
           b.minimum.y <= a.maximum.y && a.minimum.z <= b.maximum.z && b.minimum.z <= a.maximum.z;
}

tb_vec3 tb_aabb_center(tb_aabb box)
{
    return tb_vec3_scale(tb_vec3_add(box.minimum, box.maximum), 0.5f);
}

tb_vec3 tb_aabb_extents(tb_aabb box)
{
    return tb_vec3_scale(tb_vec3_sub(box.maximum, box.minimum), 0.5f);
}

static tb_vec3 tb_vec3_abs(tb_vec3 a)
{
    return (tb_vec3){fabsf(a.x), fabsf(a.y), fabsf(a.z)};
}

/* Arvo's method: the center moves as a point, and each new extent is the sum
 * of the old extents, weighted by how much each axis turns into it. */
tb_aabb tb_aabb_transform(tb_aabb box, tb_mat4 m)
{
    const tb_vec3 center = tb_mat4_transform_point(m, tb_aabb_center(box));
    const tb_vec3 extents = tb_aabb_extents(box);
    const tb_vec3 x = tb_vec3_scale(tb_vec3_abs(tb_vec3_from_vec4(m.columns[0])), extents.x);
    const tb_vec3 y = tb_vec3_scale(tb_vec3_abs(tb_vec3_from_vec4(m.columns[1])), extents.y);
    const tb_vec3 z = tb_vec3_scale(tb_vec3_abs(tb_vec3_from_vec4(m.columns[2])), extents.z);
    const tb_vec3 reach = tb_vec3_add(tb_vec3_add(x, y), z);
    const tb_aabb moved = {.minimum = tb_vec3_sub(center, reach),
                           .maximum = tb_vec3_add(center, reach)};

    return moved;
}

/* ---------------------------------------------------------------------------
 * Spheres.
 * ------------------------------------------------------------------------- */

bool tb_sphere_contains_point(tb_sphere sphere, tb_vec3 point)
{
    return tb_vec3_length_squared(tb_vec3_sub(point, sphere.center)) <=
           sphere.radius * sphere.radius;
}

bool tb_sphere_overlaps(tb_sphere a, tb_sphere b)
{
    const float reach = a.radius + b.radius;

    return tb_vec3_length_squared(tb_vec3_sub(a.center, b.center)) <= reach * reach;
}

bool tb_sphere_overlaps_aabb(tb_sphere sphere, tb_aabb box)
{
    const tb_vec3 nearest = tb_vec3_min(tb_vec3_max(sphere.center, box.minimum), box.maximum);

    return tb_sphere_contains_point(sphere, nearest);
}

/* ---------------------------------------------------------------------------
 * Rays.
 * ------------------------------------------------------------------------- */

tb_vec3 tb_ray_at(tb_ray ray, float t)
{
    return tb_vec3_add(ray.origin, tb_vec3_scale(ray.direction, t));
}

bool tb_ray_intersect_plane(tb_ray ray, tb_plane plane, float *t)
{
    TB_ASSERT(t != NULL);
    const float approach = tb_vec3_dot(plane.normal, ray.direction);

    if (approach == 0.0f)
    {
        return false;
    }
    const float along = -tb_plane_signed_distance(plane, ray.origin) / approach;

    if (along < 0.0f)
    {
        return false;
    }
    *t = along;
    return true;
}

/* The part of the ray between two parallel planes of a box, narrowed into
 * `[*enter, *leave]`. A ray parallel to the planes is between them
 * everywhere or nowhere. */
static bool tb_ray_clip_slab(float origin, float direction, float minimum, float maximum,
                             float *enter, float *leave)
{
    if (direction == 0.0f)
    {
        return origin >= minimum && origin <= maximum;
    }
    const float to_minimum = (minimum - origin) / direction;
    const float to_maximum = (maximum - origin) / direction;

    *enter = fmaxf(*enter, fminf(to_minimum, to_maximum));
    *leave = fminf(*leave, fmaxf(to_minimum, to_maximum));
    return *enter <= *leave;
}

bool tb_ray_intersect_aabb(tb_ray ray, tb_aabb box, float *t)
{
    TB_ASSERT(t != NULL);
    float enter = 0.0f;
    float leave = INFINITY;

    if (!tb_ray_clip_slab(ray.origin.x, ray.direction.x, box.minimum.x, box.maximum.x, &enter,
                          &leave) ||
        !tb_ray_clip_slab(ray.origin.y, ray.direction.y, box.minimum.y, box.maximum.y, &enter,
                          &leave) ||
        !tb_ray_clip_slab(ray.origin.z, ray.direction.z, box.minimum.z, box.maximum.z, &enter,
                          &leave))
    {
        return false;
    }
    *t = enter;
    return true;
}

bool tb_ray_intersect_sphere(tb_ray ray, tb_sphere sphere, float *t)
{
    TB_ASSERT(t != NULL);
    /* |origin + t direction − center|² = radius², a quadratic in t. */
    const tb_vec3 offset = tb_vec3_sub(ray.origin, sphere.center);
    const float a = tb_vec3_dot(ray.direction, ray.direction);
    const float half_b = tb_vec3_dot(offset, ray.direction);
    const float c = tb_vec3_dot(offset, offset) - sphere.radius * sphere.radius;
    const float discriminant = half_b * half_b - a * c;

    if (a == 0.0f || discriminant < 0.0f)
    {
        return false;
    }
    const float root = sqrtf(discriminant);
    const float leave = (-half_b + root) / a;

    if (leave < 0.0f)
    {
        return false;
    }
    *t = fmaxf((-half_b - root) / a, 0.0f);
    return true;
}

/* ---------------------------------------------------------------------------
 * Frustums.
 * ------------------------------------------------------------------------- */

/* The plane of the points that `coefficients · (x, y, z, 1) >= 0`, normalized.
 * One with no normal, the far plane at infinity, has every point in front. */
static tb_plane tb_frustum_plane(tb_vec4 coefficients)
{
    const tb_vec3 normal = tb_vec3_from_vec4(coefficients);
    const float length = tb_vec3_length(normal);

    if (length * length <= TB_GEOMETRY_DEGENERATE_SQUARED)
    {
        const tb_plane everywhere = {.normal = {0.0f, 0.0f, 0.0f}, .distance = -FLT_MAX};
        return everywhere;
    }
    const tb_plane plane = {.normal = tb_vec3_scale(normal, 1.0f / length),
                            .distance = -coefficients.w / length};

    return plane;
}

/* A point is in clip space when −w <= x <= w, −w <= y <= w, and 0 <= z <= w:
 * each of the six is a sum or difference of the matrix's rows. */
tb_frustum tb_frustum_from_matrix(tb_mat4 view_projection)
{
    const tb_mat4 rows = tb_mat4_transpose(view_projection);
    const tb_vec4 x = rows.columns[0];
    const tb_vec4 y = rows.columns[1];
    const tb_vec4 z = rows.columns[2];
    const tb_vec4 w = rows.columns[3];
    tb_frustum frustum = {0};

    frustum.planes[TB_FRUSTUM_LEFT] = tb_frustum_plane(tb_vec4_add(w, x));
    frustum.planes[TB_FRUSTUM_RIGHT] = tb_frustum_plane(tb_vec4_sub(w, x));
    frustum.planes[TB_FRUSTUM_BOTTOM] = tb_frustum_plane(tb_vec4_add(w, y));
    frustum.planes[TB_FRUSTUM_TOP] = tb_frustum_plane(tb_vec4_sub(w, y));
    /* Depth is reversed: the near plane is at depth w, the far plane at 0. */
    frustum.planes[TB_FRUSTUM_NEAR] = tb_frustum_plane(tb_vec4_sub(w, z));
    frustum.planes[TB_FRUSTUM_FAR] = tb_frustum_plane(z);
    return frustum;
}

bool tb_frustum_contains_point(const tb_frustum *frustum, tb_vec3 point)
{
    TB_ASSERT(frustum != NULL);
    for (size_t index = 0; index < TB_FRUSTUM_PLANES; index++)
    {
        if (tb_plane_signed_distance(frustum->planes[index], point) < 0.0f)
        {
            return false;
        }
    }
    return true;
}

/* Where a shape that reaches `reach[i]` from `center` toward each plane's
 * back is: outside when wholly behind any plane, inside when wholly in front
 * of all. */
static tb_containment tb_frustum_classify(const tb_frustum *frustum, tb_vec3 center,
                                          const float reach[TB_FRUSTUM_PLANES])
{
    tb_containment containment = TB_CONTAINMENT_INSIDE;

    for (size_t index = 0; index < TB_FRUSTUM_PLANES; index++)
    {
        const float distance = tb_plane_signed_distance(frustum->planes[index], center);

        if (distance < -reach[index])
        {
            return TB_CONTAINMENT_OUTSIDE;
        }
        if (distance < reach[index])
        {
            containment = TB_CONTAINMENT_INTERSECTING;
        }
    }
    return containment;
}

tb_containment tb_frustum_test_sphere(const tb_frustum *frustum, tb_sphere sphere)
{
    TB_ASSERT(frustum != NULL);
    float reach[TB_FRUSTUM_PLANES] = {0};

    for (size_t index = 0; index < TB_FRUSTUM_PLANES; index++)
    {
        reach[index] = sphere.radius;
    }
    return tb_frustum_classify(frustum, sphere.center, reach);
}

tb_containment tb_frustum_test_aabb(const tb_frustum *frustum, tb_aabb box)
{
    TB_ASSERT(frustum != NULL);
    const tb_vec3 extents = tb_aabb_extents(box);
    float reach[TB_FRUSTUM_PLANES] = {0};

    /* How far the box reaches along each plane's normal. */
    for (size_t index = 0; index < TB_FRUSTUM_PLANES; index++)
    {
        reach[index] = tb_vec3_dot(extents, tb_vec3_abs(frustum->planes[index].normal));
    }
    return tb_frustum_classify(frustum, tb_aabb_center(box), reach);
}
