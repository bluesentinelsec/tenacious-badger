/* The kernels of math.h on a processor with no SIMD form of its own. */
#include "math_kernels.h"

const char tb_math_kernel_name[] = "portable";

void tb_math_kernel_transform_many(const tb_mat4 *m, const tb_vec4 *vectors, tb_vec4 *results,
                                   size_t count)
{
    tb_math_portable_transform_many(m, vectors, results, count);
}

void tb_math_kernel_transform_points(const tb_mat4 *m, const tb_vec3 *points, tb_vec3 *results,
                                     size_t count)
{
    tb_math_portable_transform_points(m, points, results, count);
}
