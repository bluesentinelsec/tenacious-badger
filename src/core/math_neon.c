/* The kernels of math.h with NEON, which every 64-bit ARM processor has. A
 * column of a matrix is one register, multiplied by each of a vector's
 * components and summed in order. Products and sums are separate
 * instructions, never fused, so the results match the portable form. */
#include "math_kernels.h"

#include "badger/assert.h"

#include <arm_neon.h>

TB_STATIC_ASSERT(sizeof(tb_vec4) == 4 * sizeof(float), "a tb_vec4 is four packed floats");
TB_STATIC_ASSERT(sizeof(tb_mat4) == 4 * sizeof(tb_vec4), "a tb_mat4 is four packed columns");

const char tb_math_kernel_name[] = "neon";

typedef struct tb_math_neon_columns
{
    float32x4_t columns[4];
} tb_math_neon_columns;

static tb_math_neon_columns tb_math_neon_load(const tb_mat4 *m)
{
    const tb_math_neon_columns loaded = {{
        vld1q_f32((const float *)&m->columns[0]),
        vld1q_f32((const float *)&m->columns[1]),
        vld1q_f32((const float *)&m->columns[2]),
        vld1q_f32((const float *)&m->columns[3]),
    }};

    return loaded;
}

/* The first three columns times x, y and z, summed. */
static float32x4_t tb_math_neon_sum3(const tb_math_neon_columns *m, float x, float y, float z)
{
    const float32x4_t first = vmulq_n_f32(m->columns[0], x);
    const float32x4_t second = vaddq_f32(first, vmulq_n_f32(m->columns[1], y));

    return vaddq_f32(second, vmulq_n_f32(m->columns[2], z));
}

void tb_math_kernel_transform_many(const tb_mat4 *m, const tb_vec4 *vectors, tb_vec4 *results,
                                   size_t count)
{
    const tb_math_neon_columns columns = tb_math_neon_load(m);

    for (size_t index = 0; index < count; index++)
    {
        const tb_vec4 vector = vectors[index];
        const float32x4_t three = tb_math_neon_sum3(&columns, vector.x, vector.y, vector.z);
        const float32x4_t sum = vaddq_f32(three, vmulq_n_f32(columns.columns[3], vector.w));

        vst1q_f32((float *)&results[index], sum);
    }
}

void tb_math_kernel_transform_points(const tb_mat4 *m, const tb_vec3 *points, tb_vec3 *results,
                                     size_t count)
{
    const tb_math_neon_columns columns = tb_math_neon_load(m);

    for (size_t index = 0; index < count; index++)
    {
        const tb_vec3 point = points[index];
        const float32x4_t three = tb_math_neon_sum3(&columns, point.x, point.y, point.z);
        float stored[4] = {0};

        vst1q_f32(stored, vaddq_f32(three, columns.columns[3]));
        results[index] = (tb_vec3){stored[0], stored[1], stored[2]};
    }
}
