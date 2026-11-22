/* The SIMD kernels of math.h must give the portable kernels' results bit for
 * bit: the engine promises the same results from the same build, and a
 * replay recorded where one form ran must play back where the other runs.
 * Both forms are built into every engine, so each build compares the form it
 * chose against the portable one, over many matrices and vectors. */
#include "badger/rng.h"
#include "badger/str.h"
#include "core/math_kernels.h"

#include "tb_test.h"

#include <string.h>

enum
{
    MATRICES = 200,
    VECTORS = 37 /* Not a multiple of any SIMD width. */
};

static tb_mat4 random_matrix(tb_rng *rng)
{
    tb_mat4 m = {0};
    float *const elements = (float *)&m;

    for (size_t index = 0; index < 16; index++)
    {
        elements[index] = tb_rng_float_between(rng, -100.0f, 100.0f);
    }
    return m;
}

TB_TEST(core_math_kernels, the_build_names_the_kernels_it_chose)
{
    TB_TEST_EXPECT(tb_str_equal(tb_math_kernel_name, "sse2") ||
                   tb_str_equal(tb_math_kernel_name, "neon") ||
                   tb_str_equal(tb_math_kernel_name, "portable"));
}

TB_TEST(core_math_kernels, transforming_vectors_matches_the_portable_kernel_bit_for_bit)
{
    tb_rng rng = {0};
    size_t differences = 0;

    tb_rng_seed(&rng, 201);
    for (size_t trial = 0; trial < MATRICES; trial++)
    {
        const tb_mat4 m = random_matrix(&rng);
        tb_vec4 vectors[VECTORS] = {0};
        tb_vec4 simd[VECTORS] = {0};
        tb_vec4 portable[VECTORS] = {0};

        for (size_t index = 0; index < VECTORS; index++)
        {
            vectors[index] = (tb_vec4){tb_rng_float_between(&rng, -1000.0f, 1000.0f),
                                       tb_rng_float_between(&rng, -1000.0f, 1000.0f),
                                       tb_rng_float_between(&rng, -1000.0f, 1000.0f),
                                       tb_rng_float_between(&rng, -2.0f, 2.0f)};
        }
        tb_math_kernel_transform_many(&m, vectors, simd, VECTORS);
        tb_math_portable_transform_many(&m, vectors, portable, VECTORS);
        if (memcmp(simd, portable, sizeof(simd)) != 0)
        {
            differences++;
        }
    }
    TB_TEST_EXPECT_UINT(differences, ==, 0);
}

TB_TEST(core_math_kernels, transforming_points_matches_the_portable_kernel_bit_for_bit)
{
    tb_rng rng = {0};
    size_t differences = 0;

    tb_rng_seed(&rng, 202);
    for (size_t trial = 0; trial < MATRICES; trial++)
    {
        const tb_mat4 m = random_matrix(&rng);
        tb_vec3 points[VECTORS] = {0};
        tb_vec3 simd[VECTORS] = {0};
        tb_vec3 portable[VECTORS] = {0};

        for (size_t index = 0; index < VECTORS; index++)
        {
            points[index] = (tb_vec3){tb_rng_float_between(&rng, -1000.0f, 1000.0f),
                                      tb_rng_float_between(&rng, -1000.0f, 1000.0f),
                                      tb_rng_float_between(&rng, -1000.0f, 1000.0f)};
        }
        tb_math_kernel_transform_points(&m, points, simd, VECTORS);
        tb_math_portable_transform_points(&m, points, portable, VECTORS);
        if (memcmp(simd, portable, sizeof(simd)) != 0)
        {
            differences++;
        }
    }
    TB_TEST_EXPECT_UINT(differences, ==, 0);
}

TB_TEST(core_math_kernels, the_kernels_transform_in_place)
{
    const tb_mat4 m = tb_mat4_from_trs((tb_vec3){1.0f, 2.0f, 3.0f},
                                       tb_quat_from_axis_angle((tb_vec3){0.0f, 1.0f, 0.0f}, 1.0f),
                                       (tb_vec3){2.0f, 2.0f, 2.0f});
    tb_vec4 vectors[3] = {
        {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {3.0f, 2.0f, 1.0f, 1.0f}};
    tb_vec4 expected_vectors[3] = {0};
    tb_vec3 points[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {3.0f, 2.0f, 1.0f}};
    tb_vec3 expected_points[3] = {0};

    tb_math_portable_transform_many(&m, vectors, expected_vectors, 3);
    tb_math_portable_transform_points(&m, points, expected_points, 3);
    tb_math_kernel_transform_many(&m, vectors, vectors, 3);
    tb_math_kernel_transform_points(&m, points, points, 3);
    TB_TEST_EXPECT_BYTES(vectors, expected_vectors, sizeof(vectors));
    TB_TEST_EXPECT_BYTES(points, expected_points, sizeof(points));
}
