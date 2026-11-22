/* The kernels of math.h with SSE2, which every x64 processor has. A column of
 * a matrix is one register, and a vector's components are broadcast to
 * multiply it: four columns times four components, summed in order. */
#include "math_kernels.h"

#include "badger/assert.h"

#include <xmmintrin.h>

TB_STATIC_ASSERT(sizeof(tb_vec4) == 4 * sizeof(float), "a tb_vec4 is four packed floats");
TB_STATIC_ASSERT(sizeof(tb_mat4) == 4 * sizeof(tb_vec4), "a tb_mat4 is four packed columns");

const char tb_math_kernel_name[] = "sse2";

typedef struct tb_math_sse2_columns
{
    __m128 columns[4];
} tb_math_sse2_columns;

static tb_math_sse2_columns tb_math_sse2_load(const tb_mat4 *m)
{
    const tb_math_sse2_columns loaded = {{
        _mm_loadu_ps((const float *)&m->columns[0]),
        _mm_loadu_ps((const float *)&m->columns[1]),
        _mm_loadu_ps((const float *)&m->columns[2]),
        _mm_loadu_ps((const float *)&m->columns[3]),
    }};

    return loaded;
}

/* The first three columns times x, y and z, summed. */
static __m128 tb_math_sse2_sum3(const tb_math_sse2_columns *m, float x, float y, float z)
{
    const __m128 first = _mm_mul_ps(m->columns[0], _mm_set1_ps(x));
    const __m128 second = _mm_add_ps(first, _mm_mul_ps(m->columns[1], _mm_set1_ps(y)));

    return _mm_add_ps(second, _mm_mul_ps(m->columns[2], _mm_set1_ps(z)));
}

void tb_math_kernel_transform_many(const tb_mat4 *m, const tb_vec4 *vectors, tb_vec4 *results,
                                   size_t count)
{
    const tb_math_sse2_columns columns = tb_math_sse2_load(m);

    for (size_t index = 0; index < count; index++)
    {
        const tb_vec4 vector = vectors[index];
        const __m128 three = tb_math_sse2_sum3(&columns, vector.x, vector.y, vector.z);
        const __m128 sum = _mm_add_ps(three, _mm_mul_ps(columns.columns[3], _mm_set1_ps(vector.w)));

        _mm_storeu_ps((float *)&results[index], sum);
    }
}

void tb_math_kernel_transform_points(const tb_mat4 *m, const tb_vec3 *points, tb_vec3 *results,
                                     size_t count)
{
    const tb_math_sse2_columns columns = tb_math_sse2_load(m);

    for (size_t index = 0; index < count; index++)
    {
        const tb_vec3 point = points[index];
        const __m128 three = tb_math_sse2_sum3(&columns, point.x, point.y, point.z);
        float stored[4] = {0};

        _mm_storeu_ps(stored, _mm_add_ps(three, columns.columns[3]));
        results[index] = (tb_vec3){stored[0], stored[1], stored[2]};
    }
}
