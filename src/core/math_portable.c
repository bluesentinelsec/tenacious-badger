/* The kernels of math_kernels.h in portable C. Each sum is taken column by
 * column, in the order the SIMD forms take it, and every product and sum is
 * rounded on its own, so the forms agree bit for bit. */
#include "math_kernels.h"

static tb_vec3 tb_math_portable_point(const tb_mat4 *m, tb_vec3 point)
{
    const tb_vec4 first = tb_vec4_scale(m->columns[0], point.x);
    const tb_vec4 second = tb_vec4_add(first, tb_vec4_scale(m->columns[1], point.y));
    const tb_vec4 third = tb_vec4_add(second, tb_vec4_scale(m->columns[2], point.z));

    return tb_vec3_from_vec4(tb_vec4_add(third, m->columns[3]));
}

static tb_vec4 tb_math_portable_vector(const tb_mat4 *m, tb_vec4 vector)
{
    const tb_vec4 first = tb_vec4_scale(m->columns[0], vector.x);
    const tb_vec4 second = tb_vec4_add(first, tb_vec4_scale(m->columns[1], vector.y));
    const tb_vec4 third = tb_vec4_add(second, tb_vec4_scale(m->columns[2], vector.z));

    return tb_vec4_add(third, tb_vec4_scale(m->columns[3], vector.w));
}

void tb_math_portable_transform_many(const tb_mat4 *m, const tb_vec4 *vectors, tb_vec4 *results,
                                     size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        results[index] = tb_math_portable_vector(m, vectors[index]);
    }
}

void tb_math_portable_transform_points(const tb_mat4 *m, const tb_vec3 *points, tb_vec3 *results,
                                       size_t count)
{
    for (size_t index = 0; index < count; index++)
    {
        results[index] = tb_math_portable_point(m, points[index]);
    }
}
