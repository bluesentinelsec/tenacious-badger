/* The operations of math.h and geometry.h that a frame runs thousands of
 * times: matrix products and inverses, transforms of many points (with the
 * SIMD kernel, and one call at a time beside it), rotation blends, and
 * culling boxes against a frustum. Times are per operation. */
#include "tb_bench.h"

#include "badger/geometry.h"
#include "badger/math.h"

#include <stdint.h>
#include <string.h>

#define MATH_OPERATIONS ((size_t)1000000)
#define MATH_POINTS ((size_t)4096)

typedef struct math_state
{
    size_t operations; /* Of the cases that do one thing at a time. */
    size_t rounds;     /* Of the cases that go through every point. */
    tb_mat4 transform;
    tb_frustum frustum;
    tb_vec3 points[MATH_POINTS];
    tb_vec3 results[MATH_POINTS];
} math_state;

static void keep_float(float value)
{
    uint32_t bits = 0;

    memcpy(&bits, &value, sizeof(bits));
    tb_bench_keep(bits);
}

static void run_mat4_mul(void *context)
{
    math_state *const state = context;
    tb_mat4 product = tb_mat4_identity();

    for (size_t index = 0; index < state->operations; index++)
    {
        product = tb_mat4_mul(state->transform, product);
        product.columns[3].w = 1.0f;
    }
    keep_float(product.columns[0].x);
}

static void run_mat4_inverse(void *context)
{
    math_state *const state = context;
    tb_mat4 inverse = tb_mat4_identity();
    float sum = 0.0f;

    for (size_t index = 0; index < state->operations; index++)
    {
        tb_mat4 m = state->transform;

        m.columns[3].x = (float)(index & 15u);
        if (tb_mat4_inverse(m, &inverse))
        {
            sum += inverse.columns[3].x;
        }
    }
    keep_float(sum);
}

static void run_transform_points(void *context)
{
    math_state *const state = context;

    for (size_t round = 0; round < state->rounds; round++)
    {
        tb_mat4_transform_points(state->transform, state->points, state->results, MATH_POINTS);
    }
    keep_float(state->results[MATH_POINTS - 1].x);
}

static void run_transform_point_each(void *context)
{
    math_state *const state = context;

    for (size_t round = 0; round < state->rounds; round++)
    {
        for (size_t index = 0; index < MATH_POINTS; index++)
        {
            state->results[index] = tb_mat4_transform_point(state->transform, state->points[index]);
        }
    }
    keep_float(state->results[MATH_POINTS - 1].x);
}

static void run_slerp(void *context)
{
    math_state *const state = context;
    const tb_quat from = tb_quat_from_axis_angle((tb_vec3){0.0f, 1.0f, 0.0f}, 0.2f);
    const tb_quat to = tb_quat_from_axis_angle((tb_vec3){1.0f, 1.0f, 0.0f}, 2.0f);
    float sum = 0.0f;

    for (size_t index = 0; index < state->operations; index++)
    {
        const float t = (float)(index & 1023u) / 1023.0f;
        sum += tb_quat_slerp(from, to, t).w;
    }
    keep_float(sum);
}

static void run_frustum_cull(void *context)
{
    math_state *const state = context;
    size_t visible = 0;

    for (size_t round = 0; round < state->rounds; round++)
    {
        for (size_t index = 0; index < MATH_POINTS; index++)
        {
            const tb_vec3 center = state->points[index];
            const tb_aabb box = {.minimum = tb_vec3_sub(center, (tb_vec3){1.0f, 1.0f, 1.0f}),
                                 .maximum = tb_vec3_add(center, (tb_vec3){1.0f, 1.0f, 1.0f})};

            if (tb_frustum_test_aabb(&state->frustum, box) != TB_CONTAINMENT_OUTSIDE)
            {
                visible++;
            }
        }
    }
    tb_bench_keep(visible);
}

void tb_bench_math(tb_bench *bench)
{
    static math_state state = {0};
    uint64_t random = 201;
    tb_mat4 view = tb_mat4_identity();
    const size_t operations = tb_bench_size(bench, MATH_OPERATIONS);

    state.operations = operations;
    state.rounds = operations / MATH_POINTS + 1;
    state.transform = tb_mat4_from_trs((tb_vec3){1.0f, 2.0f, 3.0f},
                                       tb_quat_from_axis_angle((tb_vec3){1.0f, 2.0f, 3.0f}, 0.5f),
                                       (tb_vec3){1.0f, 2.0f, 0.5f});
    (void)tb_mat4_look_at((tb_vec3){0.0f, 0.0f, 0.0f}, (tb_vec3){0.0f, 0.0f, -1.0f},
                          (tb_vec3){0.0f, 1.0f, 0.0f}, &view);
    state.frustum = tb_frustum_from_matrix(
        tb_mat4_mul(tb_mat4_perspective(1.2f, 16.0f / 9.0f, 0.1f, 500.0f), view));
    for (size_t index = 0; index < MATH_POINTS; index++)
    {
        const float x = (float)(tb_bench_random(&random) % 2001u) / 10.0f - 100.0f;
        const float y = (float)(tb_bench_random(&random) % 2001u) / 10.0f - 100.0f;
        const float z = (float)(tb_bench_random(&random) % 2001u) / 10.0f - 100.0f;
        state.points[index] = (tb_vec3){x, y, z};
    }

    tb_bench_case cases[6] = {
        {.name = "4x4 matrix product", .variant = "badger", .run = run_mat4_mul},
        {.name = "4x4 matrix inverse", .variant = "badger", .run = run_mat4_inverse},
        {.name = "transform a point, in batches", .variant = "badger", .run = run_transform_points},
        {.name = "transform a point, in batches",
         .variant = "one call each",
         .run = run_transform_point_each},
        {.name = "slerp two rotations", .variant = "badger", .run = run_slerp},
        {.name = "cull a box against a frustum", .variant = "badger", .run = run_frustum_cull},
    };

    for (size_t index = 0; index < 6; index++)
    {
        cases[index].operations = (double)operations;
        cases[index].context = &state;
    }
    cases[2].operations = (double)(state.rounds * MATH_POINTS);
    cases[3].operations = cases[2].operations;
    cases[5].operations = cases[2].operations;
    tb_bench_measure(bench, "math", cases, 6);
}
