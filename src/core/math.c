/* The functions of badger/math.h that are not inline: quaternion blends and
 * conversions, and 3×3 and 4×4 matrices. The conventions are those of the
 * header: column vectors, matrices stored by column, right-handed, and clip
 * space with reversed depth from 0 to 1. */
#include "badger/math.h"

#include "badger/assert.h"
#include "math_kernels.h"

#include <math.h>

/* Above this cosine, two rotations are so close that slerp's division by the
 * sine of their angle loses precision, and a linear blend is as exact. */
#define TB_MATH_SLERP_LINEAR_COSINE 0.9995f

/* A squared length at or below this is no direction at all. */
#define TB_MATH_DEGENERATE_SQUARED 1e-12f

/* ---------------------------------------------------------------------------
 * Quaternions.
 * ------------------------------------------------------------------------- */

static tb_quat tb_quat_negate(tb_quat q)
{
    return (tb_quat){-q.x, -q.y, -q.z, -q.w};
}

/* `to`, or `−to`, whichever is the shorter way from `from`; the same rotation. */
static tb_quat tb_quat_nearer(tb_quat from, tb_quat to)
{
    if (tb_quat_dot(from, to) < 0.0f)
    {
        return tb_quat_negate(to);
    }
    return to;
}

static tb_quat tb_quat_blend(tb_quat from, tb_quat to, float from_weight, float to_weight)
{
    return (tb_quat){
        from.x * from_weight + to.x * to_weight, from.y * from_weight + to.y * to_weight,
        from.z * from_weight + to.z * to_weight, from.w * from_weight + to.w * to_weight};
}

tb_quat tb_quat_from_axis_angle(tb_vec3 axis, float angle)
{
    TB_ASSERT_MSG(tb_vec3_length_squared(axis) > 0.0f, "a rotation needs an axis");
    const tb_vec3 unit = tb_vec3_normalize(axis);
    const float half = angle * 0.5f;
    const float sine = sinf(half);

    return (tb_quat){unit.x * sine, unit.y * sine, unit.z * sine, cosf(half)};
}

tb_quat tb_quat_nlerp(tb_quat from, tb_quat to, float t)
{
    const tb_quat end = tb_quat_nearer(from, to);

    return tb_quat_normalize(tb_quat_blend(from, end, 1.0f - t, t));
}

tb_quat tb_quat_slerp(tb_quat from, tb_quat to, float t)
{
    const tb_quat end = tb_quat_nearer(from, to);
    const float cosine = tb_quat_dot(from, end);

    if (cosine > TB_MATH_SLERP_LINEAR_COSINE)
    {
        return tb_quat_normalize(tb_quat_blend(from, end, 1.0f - t, t));
    }
    const float angle = acosf(cosine);
    const float sine = sinf(angle);
    const float from_weight = sinf((1.0f - t) * angle) / sine;
    const float to_weight = sinf(t * angle) / sine;

    return tb_quat_blend(from, end, from_weight, to_weight);
}

/* Shepperd's method: the largest of the four candidates for 4|component|²
 * gives the square root, and so the most precise division, whatever the
 * rotation. */
tb_quat tb_quat_from_mat3(tb_mat3 rotation)
{
    const float m00 = rotation.columns[0].x;
    const float m10 = rotation.columns[0].y;
    const float m20 = rotation.columns[0].z;
    const float m01 = rotation.columns[1].x;
    const float m11 = rotation.columns[1].y;
    const float m21 = rotation.columns[1].z;
    const float m02 = rotation.columns[2].x;
    const float m12 = rotation.columns[2].y;
    const float m22 = rotation.columns[2].z;
    const float trace = m00 + m11 + m22;

    if (trace > 0.0f)
    {
        const float s = sqrtf(trace + 1.0f) * 2.0f;
        return tb_quat_normalize(
            (tb_quat){(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s});
    }
    if (m00 > m11 && m00 > m22)
    {
        const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        return tb_quat_normalize(
            (tb_quat){0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s});
    }
    if (m11 > m22)
    {
        const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        return tb_quat_normalize(
            (tb_quat){(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s});
    }
    const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
    return tb_quat_normalize(
        (tb_quat){(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s});
}

/* ---------------------------------------------------------------------------
 * 3×3 matrices.
 * ------------------------------------------------------------------------- */

static bool tb_math_invertible(float determinant)
{
    return determinant != 0.0f && isfinite(determinant);
}

tb_mat3 tb_mat3_identity(void)
{
    const tb_mat3 identity = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};

    return identity;
}

tb_vec3 tb_mat3_transform(tb_mat3 m, tb_vec3 v)
{
    const tb_vec3 first = tb_vec3_scale(m.columns[0], v.x);
    const tb_vec3 second = tb_vec3_add(first, tb_vec3_scale(m.columns[1], v.y));

    return tb_vec3_add(second, tb_vec3_scale(m.columns[2], v.z));
}

tb_mat3 tb_mat3_mul(tb_mat3 a, tb_mat3 b)
{
    const tb_mat3 product = {{tb_mat3_transform(a, b.columns[0]),
                              tb_mat3_transform(a, b.columns[1]),
                              tb_mat3_transform(a, b.columns[2])}};

    return product;
}

tb_mat3 tb_mat3_transpose(tb_mat3 m)
{
    const tb_mat3 transpose = {{{m.columns[0].x, m.columns[1].x, m.columns[2].x},
                                {m.columns[0].y, m.columns[1].y, m.columns[2].y},
                                {m.columns[0].z, m.columns[1].z, m.columns[2].z}}};

    return transpose;
}

float tb_mat3_determinant(tb_mat3 m)
{
    return tb_vec3_dot(m.columns[0], tb_vec3_cross(m.columns[1], m.columns[2]));
}

/* The rows of the inverse of `m`, times its determinant: the cross products of
 * its columns in turn. */
typedef struct tb_mat3_adjugate
{
    tb_vec3 rows[3];
    float determinant;
} tb_mat3_adjugate;

static tb_mat3_adjugate tb_mat3_adjugate_of(tb_mat3 m)
{
    const tb_mat3_adjugate adjugate = {
        .rows = {tb_vec3_cross(m.columns[1], m.columns[2]),
                 tb_vec3_cross(m.columns[2], m.columns[0]),
                 tb_vec3_cross(m.columns[0], m.columns[1])},
        .determinant = tb_mat3_determinant(m),
    };

    return adjugate;
}

bool tb_mat3_inverse(tb_mat3 m, tb_mat3 *inverse)
{
    TB_ASSERT(inverse != NULL);
    const tb_mat3_adjugate adjugate = tb_mat3_adjugate_of(m);

    if (!tb_math_invertible(adjugate.determinant))
    {
        return false;
    }
    const float scale = 1.0f / adjugate.determinant;
    const tb_mat3 rows = {{tb_vec3_scale(adjugate.rows[0], scale),
                           tb_vec3_scale(adjugate.rows[1], scale),
                           tb_vec3_scale(adjugate.rows[2], scale)}};

    *inverse = tb_mat3_transpose(rows);
    return true;
}

bool tb_mat3_normal(tb_mat4 model, tb_mat3 *normal)
{
    TB_ASSERT(normal != NULL);
    const tb_mat3_adjugate adjugate = tb_mat3_adjugate_of(tb_mat3_from_mat4(model));

    if (!tb_math_invertible(adjugate.determinant))
    {
        return false;
    }
    /* The inverse's transpose has the inverse's rows for columns. */
    const float scale = 1.0f / adjugate.determinant;
    const tb_mat3 transpose = {{tb_vec3_scale(adjugate.rows[0], scale),
                                tb_vec3_scale(adjugate.rows[1], scale),
                                tb_vec3_scale(adjugate.rows[2], scale)}};

    *normal = transpose;
    return true;
}

tb_mat3 tb_mat3_from_quat(tb_quat rotation)
{
    const float x = rotation.x;
    const float y = rotation.y;
    const float z = rotation.z;
    const float w = rotation.w;
    const tb_mat3 matrix = {{
        {1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y)},
        {2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x)},
        {2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y)},
    }};

    return matrix;
}

tb_mat3 tb_mat3_from_mat4(tb_mat4 m)
{
    const tb_mat3 upper = {{tb_vec3_from_vec4(m.columns[0]), tb_vec3_from_vec4(m.columns[1]),
                            tb_vec3_from_vec4(m.columns[2])}};

    return upper;
}

/* ---------------------------------------------------------------------------
 * 4×4 matrices.
 * ------------------------------------------------------------------------- */

tb_mat4 tb_mat4_identity(void)
{
    const tb_mat4 identity = {{{1.0f, 0.0f, 0.0f, 0.0f},
                               {0.0f, 1.0f, 0.0f, 0.0f},
                               {0.0f, 0.0f, 1.0f, 0.0f},
                               {0.0f, 0.0f, 0.0f, 1.0f}}};

    return identity;
}

tb_mat4 tb_mat4_mul(tb_mat4 a, tb_mat4 b)
{
    tb_mat4 product = {0};

    tb_math_kernel_transform_many(&a, b.columns, product.columns, 4);
    return product;
}

tb_vec4 tb_mat4_transform(tb_mat4 m, tb_vec4 v)
{
    tb_vec4 result = {0};

    tb_math_kernel_transform_many(&m, &v, &result, 1);
    return result;
}

tb_vec3 tb_mat4_transform_point(tb_mat4 m, tb_vec3 p)
{
    tb_vec3 result = {0};

    tb_math_kernel_transform_points(&m, &p, &result, 1);
    return result;
}

tb_vec3 tb_mat4_transform_direction(tb_mat4 m, tb_vec3 d)
{
    return tb_mat3_transform(tb_mat3_from_mat4(m), d);
}

void tb_mat4_transform_many(tb_mat4 m, const tb_vec4 *vectors, tb_vec4 *results, size_t count)
{
    TB_ASSERT(count == 0 || (vectors != NULL && results != NULL));
    tb_math_kernel_transform_many(&m, vectors, results, count);
}

void tb_mat4_transform_points(tb_mat4 m, const tb_vec3 *points, tb_vec3 *results, size_t count)
{
    TB_ASSERT(count == 0 || (points != NULL && results != NULL));
    tb_math_kernel_transform_points(&m, points, results, count);
}

tb_mat4 tb_mat4_transpose(tb_mat4 m)
{
    const tb_mat4 transpose = {{{m.columns[0].x, m.columns[1].x, m.columns[2].x, m.columns[3].x},
                                {m.columns[0].y, m.columns[1].y, m.columns[2].y, m.columns[3].y},
                                {m.columns[0].z, m.columns[1].z, m.columns[2].z, m.columns[3].z},
                                {m.columns[0].w, m.columns[1].w, m.columns[2].w, m.columns[3].w}}};

    return transpose;
}

/* The 2×2 determinants of the top two and the bottom two rows that the
 * cofactor expansion of a 4×4 matrix shares: the inverse and the determinant
 * are both built from them. `a` holds the elements by row, a[row][column]. */
typedef struct tb_mat4_minors
{
    float a[4][4];
    float top[6];
    float bottom[6];
    float determinant;
} tb_mat4_minors;

static tb_mat4_minors tb_mat4_minors_of(tb_mat4 m)
{
    tb_mat4_minors minors = {0};

    for (size_t column = 0; column < 4; column++)
    {
        minors.a[0][column] = m.columns[column].x;
        minors.a[1][column] = m.columns[column].y;
        minors.a[2][column] = m.columns[column].z;
        minors.a[3][column] = m.columns[column].w;
    }
    float (*const a)[4] = minors.a;

    minors.top[0] = a[0][0] * a[1][1] - a[1][0] * a[0][1];
    minors.top[1] = a[0][0] * a[1][2] - a[1][0] * a[0][2];
    minors.top[2] = a[0][0] * a[1][3] - a[1][0] * a[0][3];
    minors.top[3] = a[0][1] * a[1][2] - a[1][1] * a[0][2];
    minors.top[4] = a[0][1] * a[1][3] - a[1][1] * a[0][3];
    minors.top[5] = a[0][2] * a[1][3] - a[1][2] * a[0][3];
    minors.bottom[0] = a[2][0] * a[3][1] - a[3][0] * a[2][1];
    minors.bottom[1] = a[2][0] * a[3][2] - a[3][0] * a[2][2];
    minors.bottom[2] = a[2][0] * a[3][3] - a[3][0] * a[2][3];
    minors.bottom[3] = a[2][1] * a[3][2] - a[3][1] * a[2][2];
    minors.bottom[4] = a[2][1] * a[3][3] - a[3][1] * a[2][3];
    minors.bottom[5] = a[2][2] * a[3][3] - a[3][2] * a[2][3];

    const float *const s = minors.top;
    const float *const c = minors.bottom;

    minors.determinant =
        s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
    return minors;
}

float tb_mat4_determinant(tb_mat4 m)
{
    return tb_mat4_minors_of(m).determinant;
}

bool tb_mat4_inverse(tb_mat4 m, tb_mat4 *inverse)
{
    TB_ASSERT(inverse != NULL);
    const tb_mat4_minors minors = tb_mat4_minors_of(m);

    if (!tb_math_invertible(minors.determinant))
    {
        return false;
    }
    const float (*const a)[4] = minors.a;
    const float *const s = minors.top;
    const float *const c = minors.bottom;
    const float scale = 1.0f / minors.determinant;
    /* Each column of the inverse, as rows 0 to 3. */
    const tb_mat4 result = {{
        {(a[1][1] * c[5] - a[1][2] * c[4] + a[1][3] * c[3]) * scale,
         (-a[1][0] * c[5] + a[1][2] * c[2] - a[1][3] * c[1]) * scale,
         (a[1][0] * c[4] - a[1][1] * c[2] + a[1][3] * c[0]) * scale,
         (-a[1][0] * c[3] + a[1][1] * c[1] - a[1][2] * c[0]) * scale},
        {(-a[0][1] * c[5] + a[0][2] * c[4] - a[0][3] * c[3]) * scale,
         (a[0][0] * c[5] - a[0][2] * c[2] + a[0][3] * c[1]) * scale,
         (-a[0][0] * c[4] + a[0][1] * c[2] - a[0][3] * c[0]) * scale,
         (a[0][0] * c[3] - a[0][1] * c[1] + a[0][2] * c[0]) * scale},
        {(a[3][1] * s[5] - a[3][2] * s[4] + a[3][3] * s[3]) * scale,
         (-a[3][0] * s[5] + a[3][2] * s[2] - a[3][3] * s[1]) * scale,
         (a[3][0] * s[4] - a[3][1] * s[2] + a[3][3] * s[0]) * scale,
         (-a[3][0] * s[3] + a[3][1] * s[1] - a[3][2] * s[0]) * scale},
        {(-a[2][1] * s[5] + a[2][2] * s[4] - a[2][3] * s[3]) * scale,
         (a[2][0] * s[5] - a[2][2] * s[2] + a[2][3] * s[1]) * scale,
         (-a[2][0] * s[4] + a[2][1] * s[2] - a[2][3] * s[0]) * scale,
         (a[2][0] * s[3] - a[2][1] * s[1] + a[2][2] * s[0]) * scale},
    }};

    *inverse = result;
    return true;
}

tb_mat4 tb_mat4_translation(tb_vec3 offset)
{
    tb_mat4 translation = tb_mat4_identity();

    translation.columns[3] = tb_vec4_from_vec3(offset, 1.0f);
    return translation;
}

tb_mat4 tb_mat4_scaling(tb_vec3 scale)
{
    tb_mat4 scaling = tb_mat4_identity();

    scaling.columns[0].x = scale.x;
    scaling.columns[1].y = scale.y;
    scaling.columns[2].z = scale.z;
    return scaling;
}

tb_mat4 tb_mat4_rotation(tb_quat rotation)
{
    return tb_mat4_from_trs((tb_vec3){0.0f, 0.0f, 0.0f}, rotation, (tb_vec3){1.0f, 1.0f, 1.0f});
}

tb_mat4 tb_mat4_from_trs(tb_vec3 translation, tb_quat rotation, tb_vec3 scale)
{
    const tb_mat3 turn = tb_mat3_from_quat(rotation);
    const tb_mat4 transform = {{
        tb_vec4_from_vec3(tb_vec3_scale(turn.columns[0], scale.x), 0.0f),
        tb_vec4_from_vec3(tb_vec3_scale(turn.columns[1], scale.y), 0.0f),
        tb_vec4_from_vec3(tb_vec3_scale(turn.columns[2], scale.z), 0.0f),
        tb_vec4_from_vec3(translation, 1.0f),
    }};

    return transform;
}

bool tb_mat4_look_at(tb_vec3 eye, tb_vec3 target, tb_vec3 up, tb_mat4 *view)
{
    TB_ASSERT(view != NULL);
    const tb_vec3 line_of_sight = tb_vec3_sub(target, eye);
    const tb_vec3 across = tb_vec3_cross(line_of_sight, up);

    if (tb_vec3_length_squared(line_of_sight) <= TB_MATH_DEGENERATE_SQUARED ||
        tb_vec3_length_squared(across) <= TB_MATH_DEGENERATE_SQUARED)
    {
        return false;
    }
    const tb_vec3 forward = tb_vec3_normalize(line_of_sight);
    const tb_vec3 right = tb_vec3_normalize(across);
    const tb_vec3 above = tb_vec3_cross(right, forward);
    /* The camera's axes are the view's rows: right is +x, above is +y, and
     * forward is −z. */
    const tb_mat4 result = {{
        {right.x, above.x, -forward.x, 0.0f},
        {right.y, above.y, -forward.y, 0.0f},
        {right.z, above.z, -forward.z, 0.0f},
        {-tb_vec3_dot(right, eye), -tb_vec3_dot(above, eye), tb_vec3_dot(forward, eye), 1.0f},
    }};

    *view = result;
    return true;
}

tb_mat4 tb_mat4_perspective(float vertical_fov, float aspect, float near_plane, float far_plane)
{
    TB_ASSERT_MSG(vertical_fov > 0.0f && vertical_fov < TB_PI, "the field of view is (0, π)");
    TB_ASSERT_MSG(aspect > 0.0f, "the aspect ratio is positive");
    TB_ASSERT_MSG(near_plane > 0.0f && far_plane > near_plane,
                  "the planes are 0 < near < far, and far may be infinite");
    const float focal = 1.0f / tanf(vertical_fov * 0.5f);
    /* Depth is (depth_scale * z + depth_offset) / −z: 1 at −near and 0 at −far. */
    float depth_scale = 0.0f;
    float depth_offset = near_plane;

    if (!isinf(far_plane))
    {
        depth_scale = near_plane / (far_plane - near_plane);
        depth_offset = far_plane * depth_scale;
    }
    const tb_mat4 projection = {{
        {focal / aspect, 0.0f, 0.0f, 0.0f},
        {0.0f, focal, 0.0f, 0.0f},
        {0.0f, 0.0f, depth_scale, -1.0f},
        {0.0f, 0.0f, depth_offset, 0.0f},
    }};

    return projection;
}

tb_mat4 tb_mat4_orthographic(float left, float right, float bottom, float top, float near_plane,
                             float far_plane)
{
    TB_ASSERT_MSG(left != right && bottom != top && near_plane != far_plane,
                  "an orthographic box has a size on every axis");
    const float width = right - left;
    const float height = top - bottom;
    const float depth = far_plane - near_plane;
    /* Depth is z / depth + far / depth: 1 at −near and 0 at −far. */
    const tb_mat4 projection = {{
        {2.0f / width, 0.0f, 0.0f, 0.0f},
        {0.0f, 2.0f / height, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f / depth, 0.0f},
        {-(right + left) / width, -(top + bottom) / height, far_plane / depth, 1.0f},
    }};

    return projection;
}
