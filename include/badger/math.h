/** @file math.h
 * @brief Vectors, quaternions and matrices for 3D and 2D games.
 *
 * @code
 * const tb_mat4 model = tb_mat4_from_trs(position, tb_quat_from_axis_angle(up, yaw), scale);
 * tb_mat4 view = tb_mat4_identity();
 * (void)tb_mat4_look_at(eye, target, (tb_vec3){0.0f, 1.0f, 0.0f}, &view);
 * const tb_mat4 projection = tb_mat4_perspective(tb_radians(70.0f), aspect, 0.1f, INFINITY);
 * const tb_mat4 clip = tb_mat4_mul(projection, tb_mat4_mul(view, model));
 * @endcode
 *
 * **Conventions**, the same everywhere in the engine (docs/math.md):
 * - **Right-handed, Y up.** +X is right, +Y is up, and a camera looks down
 *   −Z, as in glTF, whose models then need no conversion. A positive angle
 *   turns counterclockwise when seen from the axis's positive end.
 * - **Column vectors, stored by column.** A matrix transforms a vector as
 *   `M * v`, so `tb_mat4_mul(a, b)` applies `b` first, then `a`. Its 16
 *   floats are its four columns in order, the layout shader constant buffers
 *   take by default, and the translation is the last column.
 * - **Clip space** is that of D3D, Vulkan and Metal: x and y from −1 to 1
 *   with y up, depth from 0 to 1. **Depth is reversed**: the near plane is at
 *   1 and the far plane at 0, which spreads float precision evenly over
 *   distance, and a far plane at infinity is allowed. A renderer clears depth
 *   to 0 and keeps what is greater. Backends with other conventions (OpenGL,
 *   Vulkan's y down) convert, so games never do.
 * - **Angles are radians**; tb_radians() converts degrees, which settings and
 *   menus use.
 * - **Quaternions** are `x, y, z, w`, with `w` the scalar part. A rotation is
 *   a unit quaternion, and `tb_quat_mul(a, b)` rotates by `b`, then by `a`.
 *
 * **Small operations are inline** (vectors, quaternions, scalars), so they
 * cost what the arithmetic costs. Matrix products and transforms of many
 * vectors use SIMD: SSE2 on x64 and NEON on ARM, chosen by the build, with a
 * portable path that gives the same results, bit for bit.
 *
 * **Determinism.** The engine is built without fast-math and without fused
 * multiply-add, so the same build on the same platform gives the same
 * results. The inline functions are compiled with the caller's settings: a
 * game that wants the same guarantee builds the same way. Functions that
 * use the C library's sine, cosine and arc functions may differ between
 * platforms.
 *
 * **Edge cases.** Normalizing a vector or quaternion of length zero returns
 * it unchanged, never NaN. Functions that can have no answer, such as
 * inverting a singular matrix, return false and leave their output alone.
 *
 * @par Threading
 * Thread-safe: every function is pure, and reads only its arguments.
 */
#ifndef TB_MATH_H
#define TB_MATH_H

#include "badger/api.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

/** @brief π, as a float. */
#define TB_PI 3.14159265358979323846f
/** @brief 2π, a full turn in radians. */
#define TB_TAU 6.28318530717958647692f

/** @brief A 2D vector or point. */
typedef struct tb_vec2
{
    float x; /**< Right. */
    float y; /**< Up. */
} tb_vec2;

/** @brief A 3D vector or point. */
typedef struct tb_vec3
{
    float x; /**< Right. */
    float y; /**< Up. */
    float z; /**< Toward the viewer: forward is −z. */
} tb_vec3;

/** @brief A 4D vector: a homogeneous point (w = 1), direction (w = 0), or color. */
typedef struct tb_vec4
{
    float x; /**< First component. */
    float y; /**< Second component. */
    float z; /**< Third component. */
    float w; /**< Fourth component. */
} tb_vec4;

/** @brief A quaternion; a rotation when its length is 1. tb_quat_identity() turns nothing. */
typedef struct tb_quat
{
    float x; /**< The vector part, x. */
    float y; /**< The vector part, y. */
    float z; /**< The vector part, z. */
    float w; /**< The scalar part. */
} tb_quat;

/** @brief A 3×3 matrix, stored by column: rotations, scales, and normal transforms. */
typedef struct tb_mat3
{
    tb_vec3 columns[3]; /**< The columns, left to right. */
} tb_mat3;

/** @brief A 4×4 matrix, stored by column: transforms, views and projections. */
typedef struct tb_mat4
{
    tb_vec4 columns[4]; /**< The columns, left to right; the last holds the translation. */
} tb_mat4;

/* ---------------------------------------------------------------------------
 * Scalars.
 * ------------------------------------------------------------------------- */

/** @brief Degrees in radians. */
static inline float tb_radians(float degrees)
{
    return degrees * (TB_PI / 180.0f);
}

/** @brief Radians in degrees. */
static inline float tb_degrees(float radians)
{
    return radians * (180.0f / TB_PI);
}

/** @brief `value` limited to `[minimum, maximum]`; requires `minimum <= maximum`. */
static inline float tb_clamp(float value, float minimum, float maximum)
{
    if (value < minimum)
    {
        return minimum;
    }
    if (value > maximum)
    {
        return maximum;
    }
    return value;
}

/** @brief The point a fraction `t` of the way from `from` to `to`: `from` at 0, `to` at 1. */
static inline float tb_lerp(float from, float to, float t)
{
    return from + (to - from) * t;
}

/**
 * @brief A smooth step from 0, at or below `edge0`, to 1, at or above `edge1`,
 * with zero slope at both ends. Requires `edge0 != edge1`.
 */
static inline float tb_smoothstep(float edge0, float edge1, float value)
{
    const float t = tb_clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);

    return t * t * (3.0f - 2.0f * t);
}

/** @brief The same angle, from −π to π: a heading that has turned many times. */
static inline float tb_wrap_angle(float radians)
{
    return remainderf(radians, TB_TAU);
}

/**
 * @brief Whether two floats differ by at most `tolerance`, scaled by the larger
 * magnitude when that is above 1: an absolute tolerance near zero, and a
 * relative one for large values.
 */
static inline bool tb_nearly_equal(float a, float b, float tolerance)
{
    const float magnitude = fmaxf(1.0f, fmaxf(fabsf(a), fabsf(b)));

    return fabsf(a - b) <= tolerance * magnitude;
}

/* ---------------------------------------------------------------------------
 * 2D vectors.
 * ------------------------------------------------------------------------- */

/** @brief `a + b`. */
static inline tb_vec2 tb_vec2_add(tb_vec2 a, tb_vec2 b)
{
    return (tb_vec2){a.x + b.x, a.y + b.y};
}

/** @brief `a − b`. */
static inline tb_vec2 tb_vec2_sub(tb_vec2 a, tb_vec2 b)
{
    return (tb_vec2){a.x - b.x, a.y - b.y};
}

/** @brief Each component of `a` times `scale`. */
static inline tb_vec2 tb_vec2_scale(tb_vec2 a, float scale)
{
    return (tb_vec2){a.x * scale, a.y * scale};
}

/** @brief Each component of `a` times the same of `b`. */
static inline tb_vec2 tb_vec2_mul(tb_vec2 a, tb_vec2 b)
{
    return (tb_vec2){a.x * b.x, a.y * b.y};
}

/** @brief `−a`. */
static inline tb_vec2 tb_vec2_negate(tb_vec2 a)
{
    return (tb_vec2){-a.x, -a.y};
}

/** @brief The dot product. */
static inline float tb_vec2_dot(tb_vec2 a, tb_vec2 b)
{
    return a.x * b.x + a.y * b.y;
}

/** @brief The squared length, which orders lengths without a square root. */
static inline float tb_vec2_length_squared(tb_vec2 a)
{
    return tb_vec2_dot(a, a);
}

/** @brief The length. */
static inline float tb_vec2_length(tb_vec2 a)
{
    return sqrtf(tb_vec2_length_squared(a));
}

/** @brief The distance between two points. */
static inline float tb_vec2_distance(tb_vec2 a, tb_vec2 b)
{
    return tb_vec2_length(tb_vec2_sub(a, b));
}

/** @brief `a` with length 1; `a` itself when its length is 0. */
static inline tb_vec2 tb_vec2_normalize(tb_vec2 a)
{
    const float length = tb_vec2_length(a);

    if (length == 0.0f)
    {
        return a;
    }
    return tb_vec2_scale(a, 1.0f / length);
}

/** @brief The point a fraction `t` of the way from `from` to `to`. */
static inline tb_vec2 tb_vec2_lerp(tb_vec2 from, tb_vec2 to, float t)
{
    return (tb_vec2){tb_lerp(from.x, to.x, t), tb_lerp(from.y, to.y, t)};
}

/** @brief The smaller of each component. */
static inline tb_vec2 tb_vec2_min(tb_vec2 a, tb_vec2 b)
{
    return (tb_vec2){fminf(a.x, b.x), fminf(a.y, b.y)};
}

/** @brief The larger of each component. */
static inline tb_vec2 tb_vec2_max(tb_vec2 a, tb_vec2 b)
{
    return (tb_vec2){fmaxf(a.x, b.x), fmaxf(a.y, b.y)};
}

/** @brief Whether every component is tb_nearly_equal(). */
static inline bool tb_vec2_nearly_equal(tb_vec2 a, tb_vec2 b, float tolerance)
{
    return tb_nearly_equal(a.x, b.x, tolerance) && tb_nearly_equal(a.y, b.y, tolerance);
}

/* ---------------------------------------------------------------------------
 * 3D vectors.
 * ------------------------------------------------------------------------- */

/** @brief `a + b`. */
static inline tb_vec3 tb_vec3_add(tb_vec3 a, tb_vec3 b)
{
    return (tb_vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

/** @brief `a − b`. */
static inline tb_vec3 tb_vec3_sub(tb_vec3 a, tb_vec3 b)
{
    return (tb_vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

/** @brief Each component of `a` times `scale`. */
static inline tb_vec3 tb_vec3_scale(tb_vec3 a, float scale)
{
    return (tb_vec3){a.x * scale, a.y * scale, a.z * scale};
}

/** @brief Each component of `a` times the same of `b`. */
static inline tb_vec3 tb_vec3_mul(tb_vec3 a, tb_vec3 b)
{
    return (tb_vec3){a.x * b.x, a.y * b.y, a.z * b.z};
}

/** @brief `−a`. */
static inline tb_vec3 tb_vec3_negate(tb_vec3 a)
{
    return (tb_vec3){-a.x, -a.y, -a.z};
}

/** @brief The dot product. */
static inline float tb_vec3_dot(tb_vec3 a, tb_vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/** @brief The cross product, by the right-hand rule: x × y = z. */
static inline tb_vec3 tb_vec3_cross(tb_vec3 a, tb_vec3 b)
{
    return (tb_vec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/** @brief The squared length, which orders lengths without a square root. */
static inline float tb_vec3_length_squared(tb_vec3 a)
{
    return tb_vec3_dot(a, a);
}

/** @brief The length. */
static inline float tb_vec3_length(tb_vec3 a)
{
    return sqrtf(tb_vec3_length_squared(a));
}

/** @brief The distance between two points. */
static inline float tb_vec3_distance(tb_vec3 a, tb_vec3 b)
{
    return tb_vec3_length(tb_vec3_sub(a, b));
}

/** @brief `a` with length 1; `a` itself when its length is 0. */
static inline tb_vec3 tb_vec3_normalize(tb_vec3 a)
{
    const float length = tb_vec3_length(a);

    if (length == 0.0f)
    {
        return a;
    }
    return tb_vec3_scale(a, 1.0f / length);
}

/** @brief The point a fraction `t` of the way from `from` to `to`. */
static inline tb_vec3 tb_vec3_lerp(tb_vec3 from, tb_vec3 to, float t)
{
    return (tb_vec3){tb_lerp(from.x, to.x, t), tb_lerp(from.y, to.y, t), tb_lerp(from.z, to.z, t)};
}

/** @brief The smaller of each component. */
static inline tb_vec3 tb_vec3_min(tb_vec3 a, tb_vec3 b)
{
    return (tb_vec3){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)};
}

/** @brief The larger of each component. */
static inline tb_vec3 tb_vec3_max(tb_vec3 a, tb_vec3 b)
{
    return (tb_vec3){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)};
}

/** @brief Whether every component is tb_nearly_equal(). */
static inline bool tb_vec3_nearly_equal(tb_vec3 a, tb_vec3 b, float tolerance)
{
    return tb_nearly_equal(a.x, b.x, tolerance) && tb_nearly_equal(a.y, b.y, tolerance) &&
           tb_nearly_equal(a.z, b.z, tolerance);
}

/* ---------------------------------------------------------------------------
 * 4D vectors.
 * ------------------------------------------------------------------------- */

/** @brief `a` with a fourth component: 1 for a point, 0 for a direction. */
static inline tb_vec4 tb_vec4_from_vec3(tb_vec3 a, float w)
{
    return (tb_vec4){a.x, a.y, a.z, w};
}

/** @brief The first three components of `a`. */
static inline tb_vec3 tb_vec3_from_vec4(tb_vec4 a)
{
    return (tb_vec3){a.x, a.y, a.z};
}

/** @brief `a + b`. */
static inline tb_vec4 tb_vec4_add(tb_vec4 a, tb_vec4 b)
{
    return (tb_vec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

/** @brief `a − b`. */
static inline tb_vec4 tb_vec4_sub(tb_vec4 a, tb_vec4 b)
{
    return (tb_vec4){a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

/** @brief Each component of `a` times `scale`. */
static inline tb_vec4 tb_vec4_scale(tb_vec4 a, float scale)
{
    return (tb_vec4){a.x * scale, a.y * scale, a.z * scale, a.w * scale};
}

/** @brief Each component of `a` times the same of `b`. */
static inline tb_vec4 tb_vec4_mul(tb_vec4 a, tb_vec4 b)
{
    return (tb_vec4){a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

/** @brief `−a`. */
static inline tb_vec4 tb_vec4_negate(tb_vec4 a)
{
    return (tb_vec4){-a.x, -a.y, -a.z, -a.w};
}

/** @brief The dot product. */
static inline float tb_vec4_dot(tb_vec4 a, tb_vec4 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/** @brief The squared length, which orders lengths without a square root. */
static inline float tb_vec4_length_squared(tb_vec4 a)
{
    return tb_vec4_dot(a, a);
}

/** @brief The length. */
static inline float tb_vec4_length(tb_vec4 a)
{
    return sqrtf(tb_vec4_length_squared(a));
}

/** @brief The distance between two points. */
static inline float tb_vec4_distance(tb_vec4 a, tb_vec4 b)
{
    return tb_vec4_length(tb_vec4_sub(a, b));
}

/** @brief `a` with length 1; `a` itself when its length is 0. */
static inline tb_vec4 tb_vec4_normalize(tb_vec4 a)
{
    const float length = tb_vec4_length(a);

    if (length == 0.0f)
    {
        return a;
    }
    return tb_vec4_scale(a, 1.0f / length);
}

/** @brief The point a fraction `t` of the way from `from` to `to`. */
static inline tb_vec4 tb_vec4_lerp(tb_vec4 from, tb_vec4 to, float t)
{
    return (tb_vec4){tb_lerp(from.x, to.x, t), tb_lerp(from.y, to.y, t), tb_lerp(from.z, to.z, t),
                     tb_lerp(from.w, to.w, t)};
}

/** @brief The smaller of each component. */
static inline tb_vec4 tb_vec4_min(tb_vec4 a, tb_vec4 b)
{
    return (tb_vec4){fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z), fminf(a.w, b.w)};
}

/** @brief The larger of each component. */
static inline tb_vec4 tb_vec4_max(tb_vec4 a, tb_vec4 b)
{
    return (tb_vec4){fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z), fmaxf(a.w, b.w)};
}

/** @brief Whether every component is tb_nearly_equal(). */
static inline bool tb_vec4_nearly_equal(tb_vec4 a, tb_vec4 b, float tolerance)
{
    return tb_nearly_equal(a.x, b.x, tolerance) && tb_nearly_equal(a.y, b.y, tolerance) &&
           tb_nearly_equal(a.z, b.z, tolerance) && tb_nearly_equal(a.w, b.w, tolerance);
}

/* ---------------------------------------------------------------------------
 * Quaternions.
 * ------------------------------------------------------------------------- */

/** @brief The rotation that turns nothing. */
static inline tb_quat tb_quat_identity(void)
{
    return (tb_quat){0.0f, 0.0f, 0.0f, 1.0f};
}

/** @brief The rotation by `b`, then by `a`: the Hamilton product `a * b`. */
static inline tb_quat tb_quat_mul(tb_quat a, tb_quat b)
{
    return (tb_quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

/** @brief The conjugate, which is the inverse rotation of a unit quaternion. */
static inline tb_quat tb_quat_conjugate(tb_quat a)
{
    return (tb_quat){-a.x, -a.y, -a.z, a.w};
}

/** @brief The dot product: the cosine of half the angle between two rotations. */
static inline float tb_quat_dot(tb_quat a, tb_quat b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

/** @brief `a` with length 1; `a` itself when its length is 0. */
static inline tb_quat tb_quat_normalize(tb_quat a)
{
    const float length = sqrtf(tb_quat_dot(a, a));

    if (length == 0.0f)
    {
        return a;
    }
    return (tb_quat){a.x / length, a.y / length, a.z / length, a.w / length};
}

/** @brief `v` turned by the unit quaternion `q`. */
static inline tb_vec3 tb_quat_rotate(tb_quat q, tb_vec3 v)
{
    const tb_vec3 axis = {q.x, q.y, q.z};
    const tb_vec3 twice = tb_vec3_scale(tb_vec3_cross(axis, v), 2.0f);
    const tb_vec3 turned = tb_vec3_add(v, tb_vec3_scale(twice, q.w));

    return tb_vec3_add(turned, tb_vec3_cross(axis, twice));
}

/**
 * @brief Whether two unit quaternions are nearly the same rotation. `q` and
 * `−q` are the same rotation, so either matches.
 */
static inline bool tb_quat_nearly_equal(tb_quat a, tb_quat b, float tolerance)
{
    return fabsf(tb_quat_dot(a, b)) >= 1.0f - tolerance;
}

/**
 * @brief The rotation by `angle` radians about `axis`, counterclockwise when
 * seen from the axis's end. `axis` need not have length 1, but must not be 0.
 */
TB_API tb_quat tb_quat_from_axis_angle(tb_vec3 axis, float angle);

/**
 * @brief The rotation a fraction `t` of the way from `from` to `to`, at a
 * steady angular speed and along the shorter way round. Both are unit
 * quaternions; the result is one.
 */
TB_API tb_quat tb_quat_slerp(tb_quat from, tb_quat to, float t);

/**
 * @brief A cheaper blend than tb_quat_slerp(), the normalized linear blend
 * along the shorter way round: the same path at an uneven speed, which is
 * as good for small steps such as blending animation.
 */
TB_API tb_quat tb_quat_nlerp(tb_quat from, tb_quat to, float t);

/** @brief The rotation of a rotation matrix: one with orthonormal columns and determinant 1. */
TB_API tb_quat tb_quat_from_mat3(tb_mat3 rotation);

/* ---------------------------------------------------------------------------
 * 3×3 matrices.
 * ------------------------------------------------------------------------- */

/** @brief The matrix that changes nothing. */
TB_API tb_mat3 tb_mat3_identity(void);

/** @brief `a * b`: the transform by `b`, then by `a`. */
TB_API tb_mat3 tb_mat3_mul(tb_mat3 a, tb_mat3 b);

/** @brief `m * v`. */
TB_API tb_vec3 tb_mat3_transform(tb_mat3 m, tb_vec3 v);

/** @brief The transpose, which is the inverse of a rotation. */
TB_API tb_mat3 tb_mat3_transpose(tb_mat3 m);

/** @brief The determinant. */
TB_API float tb_mat3_determinant(tb_mat3 m);

/**
 * @brief The inverse of `m`, in `*inverse`.
 * @return false, leaving `*inverse` alone, when `m` is singular: its
 *         determinant is 0 or not finite.
 */
TB_API bool tb_mat3_inverse(tb_mat3 m, tb_mat3 *inverse);

/** @brief The rotation matrix of a unit quaternion. */
TB_API tb_mat3 tb_mat3_from_quat(tb_quat rotation);

/** @brief The upper-left 3×3 part of `m`: its rotation and scale, without translation. */
TB_API tb_mat3 tb_mat3_from_mat4(tb_mat4 m);

/**
 * @brief The matrix that transforms normals for the transform `model`: the
 * inverse transpose of its upper-left 3×3 part, which keeps normals
 * perpendicular to surfaces under scales that differ by axis.
 * @return false, leaving `*normal` alone, when that part is singular.
 */
TB_API bool tb_mat3_normal(tb_mat4 model, tb_mat3 *normal);

/* ---------------------------------------------------------------------------
 * 4×4 matrices.
 * ------------------------------------------------------------------------- */

/** @brief The matrix that changes nothing. */
TB_API tb_mat4 tb_mat4_identity(void);

/** @brief `a * b`: the transform by `b`, then by `a`. */
TB_API tb_mat4 tb_mat4_mul(tb_mat4 a, tb_mat4 b);

/** @brief `m * v`. */
TB_API tb_vec4 tb_mat4_transform(tb_mat4 m, tb_vec4 v);

/** @brief The point `p` moved by `m`: `m * (p, 1)` without dividing by w. */
TB_API tb_vec3 tb_mat4_transform_point(tb_mat4 m, tb_vec3 p);

/** @brief The direction `d` turned and scaled by `m`, without its translation: `m * (d, 0)`. */
TB_API tb_vec3 tb_mat4_transform_direction(tb_mat4 m, tb_vec3 d);

/**
 * @brief `m * vectors[i]` for each of `count` vectors, into `results`, with
 * SIMD. `results` may be `vectors`, and both may be NULL when `count` is 0.
 */
TB_API void tb_mat4_transform_many(tb_mat4 m, const tb_vec4 *vectors, tb_vec4 *results,
                                   size_t count);

/**
 * @brief tb_mat4_transform_point() for each of `count` points, into
 * `results`, with SIMD. `results` may be `points`, and both may be NULL when
 * `count` is 0.
 */
TB_API void tb_mat4_transform_points(tb_mat4 m, const tb_vec3 *points, tb_vec3 *results,
                                     size_t count);

/** @brief The transpose. */
TB_API tb_mat4 tb_mat4_transpose(tb_mat4 m);

/** @brief The determinant. */
TB_API float tb_mat4_determinant(tb_mat4 m);

/**
 * @brief The inverse of `m`, in `*inverse`.
 * @return false, leaving `*inverse` alone, when `m` is singular: its
 *         determinant is 0 or not finite.
 */
TB_API bool tb_mat4_inverse(tb_mat4 m, tb_mat4 *inverse);

/** @brief The transform that moves points by `offset`. */
TB_API tb_mat4 tb_mat4_translation(tb_vec3 offset);

/** @brief The transform that scales each axis by the same of `scale`. */
TB_API tb_mat4 tb_mat4_scaling(tb_vec3 scale);

/** @brief The transform that turns by the unit quaternion `rotation`. */
TB_API tb_mat4 tb_mat4_rotation(tb_quat rotation);

/**
 * @brief The transform of an object: scaled by `scale`, turned by `rotation`,
 * then moved to `translation`. The same as translation * rotation * scaling,
 * built directly.
 */
TB_API tb_mat4 tb_mat4_from_trs(tb_vec3 translation, tb_quat rotation, tb_vec3 scale);

/**
 * @brief A camera's view matrix: the camera at `eye` looking at `target`,
 * with `up` above it. The view puts the camera at the origin looking down −z,
 * with +y up.
 * @return false, leaving `*view` alone, when `eye` and `target` are the same
 *         point or `up` points along the line of sight.
 */
TB_API bool tb_mat4_look_at(tb_vec3 eye, tb_vec3 target, tb_vec3 up, tb_mat4 *view);

/**
 * @brief A perspective projection, with reversed depth: the near plane maps
 * to depth 1 and the far plane to depth 0.
 * @param vertical_fov The angle from the bottom of the view to its top, in
 *        radians, more than 0 and less than π.
 * @param aspect The view's width over its height, more than 0.
 * @param near_plane The distance to the near plane, more than 0.
 * @param far_plane The distance to the far plane, more than `near_plane`, or
 *        `INFINITY` for no far plane: nothing is too far to draw, and
 *        reversed depth keeps precision anyway.
 */
TB_API tb_mat4 tb_mat4_perspective(float vertical_fov, float aspect, float near_plane,
                                   float far_plane);

/**
 * @brief An orthographic projection, with reversed depth, for 2D games, user
 * interfaces, shadow maps, and isometric views: the box from `left` to
 * `right`, `bottom` to `top`, and the distances `near_plane` to `far_plane`
 * in front of the view fills clip space, the near plane at depth 1 and the
 * far plane at depth 0. Each pair must differ; `near_plane` may be negative.
 */
TB_API tb_mat4 tb_mat4_orthographic(float left, float right, float bottom, float top,
                                    float near_plane, float far_plane);

#endif
