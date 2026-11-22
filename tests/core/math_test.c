/* badger/math.h must follow the conventions it documents (right-handed, Y
 * up, column vectors, reversed depth from 0 to 1), because the renderer,
 * every backend's clip space, and every game's transforms rest on them. Each
 * case checks an identity or a known value, so a broken convention fails
 * here rather than as a mirrored or inside-out scene. */
#include "badger/math.h"

#include "tb_test.h"

#include <math.h>

/* Single-precision results of a few operations each. */
#define TOLERANCE 1e-5f

#define EXPECT_VEC3_NEAR(actual, expected)                                                         \
    do                                                                                             \
    {                                                                                              \
        const tb_vec3 vec3_actual = (actual);                                                      \
        const tb_vec3 vec3_expected = (expected);                                                  \
        TB_TEST_EXPECT_FLOAT_NEAR(vec3_actual.x, vec3_expected.x, TOLERANCE);                      \
        TB_TEST_EXPECT_FLOAT_NEAR(vec3_actual.y, vec3_expected.y, TOLERANCE);                      \
        TB_TEST_EXPECT_FLOAT_NEAR(vec3_actual.z, vec3_expected.z, TOLERANCE);                      \
    } while (0)

#define EXPECT_VEC4_NEAR(actual, expected)                                                         \
    do                                                                                             \
    {                                                                                              \
        const tb_vec4 vec4_actual = (actual);                                                      \
        const tb_vec4 vec4_expected = (expected);                                                  \
        TB_TEST_EXPECT_FLOAT_NEAR(vec4_actual.x, vec4_expected.x, TOLERANCE);                      \
        TB_TEST_EXPECT_FLOAT_NEAR(vec4_actual.y, vec4_expected.y, TOLERANCE);                      \
        TB_TEST_EXPECT_FLOAT_NEAR(vec4_actual.z, vec4_expected.z, TOLERANCE);                      \
        TB_TEST_EXPECT_FLOAT_NEAR(vec4_actual.w, vec4_expected.w, TOLERANCE);                      \
    } while (0)

static bool mat4_nearly_equal(tb_mat4 a, tb_mat4 b)
{
    for (size_t column = 0; column < 4; column++)
    {
        if (!tb_vec4_nearly_equal(a.columns[column], b.columns[column], TOLERANCE))
        {
            return false;
        }
    }
    return true;
}

static bool mat3_nearly_equal(tb_mat3 a, tb_mat3 b)
{
    for (size_t column = 0; column < 3; column++)
    {
        if (!tb_vec3_nearly_equal(a.columns[column], b.columns[column], TOLERANCE))
        {
            return false;
        }
    }
    return true;
}

static const tb_vec3 unit_x = {1.0f, 0.0f, 0.0f};
static const tb_vec3 unit_y = {0.0f, 1.0f, 0.0f};
static const tb_vec3 unit_z = {0.0f, 0.0f, 1.0f};

/* A transform with every part at work: a scale that differs by axis, a turn
 * about an axis that is no coordinate axis, and a move. */
static tb_mat4 sample_transform(void)
{
    const tb_quat turn = tb_quat_from_axis_angle((tb_vec3){1.0f, 2.0f, 3.0f}, 0.7f);

    return tb_mat4_from_trs((tb_vec3){4.0f, -5.0f, 6.0f}, turn, (tb_vec3){2.0f, 0.5f, 3.0f});
}

/* ---------------------------------------------------------------------------
 * Scalars.
 * ------------------------------------------------------------------------- */

TB_TEST(core_math, degrees_and_radians_convert_both_ways)
{
    TB_TEST_EXPECT_FLOAT_NEAR(tb_radians(180.0f), TB_PI, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_degrees(TB_PI / 2.0f), 90.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(TB_TAU, 2.0f * TB_PI, TOLERANCE);
}

TB_TEST(core_math, clamp_lerp_and_smoothstep_hold_their_ends)
{
    TB_TEST_EXPECT_FLOAT(tb_clamp(-1.0f, 0.0f, 1.0f), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_clamp(2.0f, 0.0f, 1.0f), ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_clamp(0.25f, 0.0f, 1.0f), ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(tb_lerp(2.0f, 6.0f, 0.0f), ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(tb_lerp(2.0f, 6.0f, 1.0f), ==, 6.0f);
    TB_TEST_EXPECT_FLOAT(tb_lerp(2.0f, 6.0f, 0.25f), ==, 3.0f);
    TB_TEST_EXPECT_FLOAT(tb_smoothstep(1.0f, 3.0f, 0.0f), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_smoothstep(1.0f, 3.0f, 2.0f), ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(tb_smoothstep(1.0f, 3.0f, 4.0f), ==, 1.0f);
}

TB_TEST(core_math, an_angle_wraps_to_within_half_a_turn)
{
    TB_TEST_EXPECT_FLOAT_NEAR(tb_wrap_angle(TB_TAU + 0.5f), 0.5f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_wrap_angle(-TB_TAU * 3.0f - 0.5f), -0.5f, 1e-4f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_wrap_angle(0.25f), 0.25f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(fabsf(tb_wrap_angle(3.0f * TB_PI)), TB_PI, 1e-4f);
}

TB_TEST(core_math, nearly_equal_is_absolute_near_zero_and_relative_when_large)
{
    TB_TEST_EXPECT(tb_nearly_equal(1e-7f, 0.0f, 1e-6f));
    TB_TEST_EXPECT(!tb_nearly_equal(1e-5f, 0.0f, 1e-6f));
    TB_TEST_EXPECT(tb_nearly_equal(100000.0f, 100000.05f, 1e-6f));
    TB_TEST_EXPECT(!tb_nearly_equal(1.0f, 1.1f, 1e-6f));
}

/* ---------------------------------------------------------------------------
 * Vectors.
 * ------------------------------------------------------------------------- */

TB_TEST(core_math, vec2_arithmetic_works_component_by_component)
{
    const tb_vec2 a = {1.0f, 2.0f};
    const tb_vec2 b = {3.0f, -4.0f};

    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_add(a, b), (tb_vec2){4.0f, -2.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_sub(a, b), (tb_vec2){-2.0f, 6.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_scale(a, 2.0f), (tb_vec2){2.0f, 4.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_mul(a, b), (tb_vec2){3.0f, -8.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_negate(a), (tb_vec2){-1.0f, -2.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_min(a, b), (tb_vec2){1.0f, -4.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_max(a, b), (tb_vec2){3.0f, 2.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_lerp(a, b, 0.5f), (tb_vec2){2.0f, -1.0f}, 0.0f));
    TB_TEST_EXPECT_FLOAT(tb_vec2_dot(a, b), ==, -5.0f);
    TB_TEST_EXPECT_FLOAT(tb_vec2_length((tb_vec2){3.0f, 4.0f}), ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(tb_vec2_distance(a, (tb_vec2){4.0f, 6.0f}), ==, 5.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec2_length(tb_vec2_normalize(b)), 1.0f, TOLERANCE);
    TB_TEST_EXPECT(tb_vec2_nearly_equal(tb_vec2_normalize((tb_vec2){0.0f, 0.0f}),
                                        (tb_vec2){0.0f, 0.0f}, 0.0f));
}

TB_TEST(core_math, vec3_arithmetic_works_component_by_component)
{
    const tb_vec3 a = {1.0f, 2.0f, 3.0f};
    const tb_vec3 b = {-2.0f, 4.0f, 0.5f};

    EXPECT_VEC3_NEAR(tb_vec3_add(a, b), ((tb_vec3){-1.0f, 6.0f, 3.5f}));
    EXPECT_VEC3_NEAR(tb_vec3_sub(a, b), ((tb_vec3){3.0f, -2.0f, 2.5f}));
    EXPECT_VEC3_NEAR(tb_vec3_scale(a, -1.0f), tb_vec3_negate(a));
    EXPECT_VEC3_NEAR(tb_vec3_mul(a, b), ((tb_vec3){-2.0f, 8.0f, 1.5f}));
    EXPECT_VEC3_NEAR(tb_vec3_min(a, b), ((tb_vec3){-2.0f, 2.0f, 0.5f}));
    EXPECT_VEC3_NEAR(tb_vec3_max(a, b), ((tb_vec3){1.0f, 4.0f, 3.0f}));
    EXPECT_VEC3_NEAR(tb_vec3_lerp(a, b, 0.5f), ((tb_vec3){-0.5f, 3.0f, 1.75f}));
    TB_TEST_EXPECT_FLOAT(tb_vec3_dot(a, b), ==, 7.5f);
    TB_TEST_EXPECT_FLOAT(tb_vec3_length((tb_vec3){2.0f, 3.0f, 6.0f}), ==, 7.0f);
    TB_TEST_EXPECT_FLOAT(tb_vec3_distance(a, (tb_vec3){3.0f, 5.0f, 9.0f}), ==, 7.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(tb_vec3_normalize(b)), 1.0f, TOLERANCE);
    EXPECT_VEC3_NEAR(tb_vec3_normalize((tb_vec3){0.0f, 0.0f, 0.0f}), ((tb_vec3){0.0f, 0.0f, 0.0f}));
}

TB_TEST(core_math, the_cross_product_follows_the_right_hand_rule)
{
    EXPECT_VEC3_NEAR(tb_vec3_cross(unit_x, unit_y), unit_z);
    EXPECT_VEC3_NEAR(tb_vec3_cross(unit_y, unit_z), unit_x);
    EXPECT_VEC3_NEAR(tb_vec3_cross(unit_z, unit_x), unit_y);
    EXPECT_VEC3_NEAR(tb_vec3_cross(unit_y, unit_x), tb_vec3_negate(unit_z));
}

TB_TEST(core_math, vec4_arithmetic_works_component_by_component)
{
    const tb_vec4 a = {1.0f, 2.0f, 3.0f, 4.0f};
    const tb_vec4 b = {4.0f, 3.0f, -2.0f, 1.0f};

    EXPECT_VEC4_NEAR(tb_vec4_add(a, b), ((tb_vec4){5.0f, 5.0f, 1.0f, 5.0f}));
    EXPECT_VEC4_NEAR(tb_vec4_sub(a, b), ((tb_vec4){-3.0f, -1.0f, 5.0f, 3.0f}));
    EXPECT_VEC4_NEAR(tb_vec4_scale(a, -1.0f), tb_vec4_negate(a));
    EXPECT_VEC4_NEAR(tb_vec4_mul(a, b), ((tb_vec4){4.0f, 6.0f, -6.0f, 4.0f}));
    EXPECT_VEC4_NEAR(tb_vec4_min(a, b), ((tb_vec4){1.0f, 2.0f, -2.0f, 1.0f}));
    EXPECT_VEC4_NEAR(tb_vec4_max(a, b), ((tb_vec4){4.0f, 3.0f, 3.0f, 4.0f}));
    EXPECT_VEC4_NEAR(tb_vec4_lerp(a, b, 0.5f), ((tb_vec4){2.5f, 2.5f, 0.5f, 2.5f}));
    TB_TEST_EXPECT_FLOAT(tb_vec4_dot(a, b), ==, 8.0f);
    TB_TEST_EXPECT_FLOAT(tb_vec4_length((tb_vec4){1.0f, 1.0f, 1.0f, 1.0f}), ==, 2.0f);
    TB_TEST_EXPECT_FLOAT(tb_vec4_distance(a, (tb_vec4){2.0f, 3.0f, 4.0f, 5.0f}), ==, 2.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec4_length(tb_vec4_normalize(b)), 1.0f, TOLERANCE);
    EXPECT_VEC4_NEAR(tb_vec4_normalize((tb_vec4){0.0f, 0.0f, 0.0f, 0.0f}),
                     ((tb_vec4){0.0f, 0.0f, 0.0f, 0.0f}));
    EXPECT_VEC3_NEAR(tb_vec3_from_vec4(tb_vec4_from_vec3(unit_y, 1.0f)), unit_y);
    TB_TEST_EXPECT_FLOAT(tb_vec4_from_vec3(unit_y, 1.0f).w, ==, 1.0f);
}

/* ---------------------------------------------------------------------------
 * Quaternions.
 * ------------------------------------------------------------------------- */

TB_TEST(core_math, a_positive_turn_is_counterclockwise_seen_from_the_axis)
{
    /* Seen from above, +y, a quarter turn takes +x to −z: the right-hand rule. */
    const tb_quat about_y = tb_quat_from_axis_angle(unit_y, TB_PI / 2.0f);
    const tb_quat about_z = tb_quat_from_axis_angle((tb_vec3){0.0f, 0.0f, 5.0f}, TB_PI / 2.0f);

    EXPECT_VEC3_NEAR(tb_quat_rotate(about_y, unit_x), tb_vec3_negate(unit_z));
    EXPECT_VEC3_NEAR(tb_quat_rotate(about_z, unit_x), unit_y);
    EXPECT_VEC3_NEAR(tb_quat_rotate(tb_quat_identity(), unit_x), unit_x);
}

TB_TEST(core_math, a_product_of_rotations_turns_by_the_right_one_first)
{
    const tb_quat first = tb_quat_from_axis_angle(unit_x, 0.4f);
    const tb_quat second = tb_quat_from_axis_angle(unit_y, 1.1f);
    const tb_vec3 v = {1.0f, 2.0f, 3.0f};
    const tb_quat both = tb_quat_mul(second, first);

    EXPECT_VEC3_NEAR(tb_quat_rotate(both, v), tb_quat_rotate(second, tb_quat_rotate(first, v)));
    EXPECT_VEC3_NEAR(tb_quat_rotate(tb_quat_conjugate(both), tb_quat_rotate(both, v)), v);
}

TB_TEST(core_math, normalizing_a_quaternion_gives_length_one_and_leaves_zero_alone)
{
    const tb_quat unit = tb_quat_normalize((tb_quat){1.0f, 2.0f, 3.0f, 4.0f});
    const tb_quat zero = tb_quat_normalize((tb_quat){0.0f, 0.0f, 0.0f, 0.0f});

    TB_TEST_EXPECT_FLOAT_NEAR(tb_quat_dot(unit, unit), 1.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT(tb_quat_dot(zero, zero), ==, 0.0f);
}

TB_TEST(core_math, a_quaternion_and_its_negation_are_the_same_rotation)
{
    const tb_quat q = tb_quat_from_axis_angle((tb_vec3){1.0f, 1.0f, 0.0f}, 2.0f);
    const tb_quat negated = {-q.x, -q.y, -q.z, -q.w};

    TB_TEST_EXPECT(tb_quat_nearly_equal(q, negated, TOLERANCE));
    TB_TEST_EXPECT(!tb_quat_nearly_equal(q, tb_quat_identity(), TOLERANCE));
    EXPECT_VEC3_NEAR(tb_quat_rotate(q, unit_z), tb_quat_rotate(negated, unit_z));
}

TB_TEST(core_math, slerp_turns_at_a_steady_speed_between_its_ends)
{
    const tb_quat from = tb_quat_identity();
    const tb_quat to = tb_quat_from_axis_angle(unit_z, TB_PI / 2.0f);
    const float diagonal = sqrtf(0.5f);

    TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_slerp(from, to, 0.0f), from, TOLERANCE));
    TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_slerp(from, to, 1.0f), to, TOLERANCE));
    EXPECT_VEC3_NEAR(tb_quat_rotate(tb_quat_slerp(from, to, 0.5f), unit_x),
                     ((tb_vec3){diagonal, diagonal, 0.0f}));
    /* A third of the way is a third of the angle: the speed is steady. */
    EXPECT_VEC3_NEAR(tb_quat_rotate(tb_quat_slerp(from, to, 1.0f / 3.0f), unit_x),
                     ((tb_vec3){cosf(TB_PI / 6.0f), sinf(TB_PI / 6.0f), 0.0f}));
}

TB_TEST(core_math, blends_take_the_shorter_way_round)
{
    /* −q is the same rotation as q, and so the blend never leaves it. */
    const tb_quat q = tb_quat_from_axis_angle(unit_y, 0.8f);
    const tb_quat negated = {-q.x, -q.y, -q.z, -q.w};

    TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_slerp(q, negated, 0.5f), q, TOLERANCE));
    TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_nlerp(q, negated, 0.5f), q, TOLERANCE));
}

TB_TEST(core_math, nlerp_and_slerp_agree_on_nearby_rotations)
{
    const tb_quat from = tb_quat_from_axis_angle(unit_x, 0.1f);
    const tb_quat to = tb_quat_from_axis_angle(unit_x, 0.1001f);

    TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_slerp(from, to, 0.5f),
                                        tb_quat_nlerp(from, to, 0.5f), TOLERANCE));
    TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_slerp(from, to, 0.5f),
                                        tb_quat_from_axis_angle(unit_x, 0.10005f), TOLERANCE));
}

TB_TEST(core_math, a_rotation_survives_conversion_to_a_matrix_and_back)
{
    /* Half turns about each axis take each branch of the conversion. */
    const tb_quat rotations[] = {
        tb_quat_identity(),
        tb_quat_from_axis_angle(unit_x, TB_PI),
        tb_quat_from_axis_angle(unit_y, TB_PI),
        tb_quat_from_axis_angle(unit_z, TB_PI),
        tb_quat_from_axis_angle((tb_vec3){1.0f, -2.0f, 0.5f}, 2.5f),
        tb_quat_from_axis_angle((tb_vec3){-3.0f, 1.0f, 1.0f}, 0.3f),
    };

    for (size_t index = 0; index < sizeof(rotations) / sizeof(rotations[0]); index++)
    {
        const tb_quat q = rotations[index];
        const tb_mat3 matrix = tb_mat3_from_quat(q);

        TB_TEST_EXPECT(tb_quat_nearly_equal(tb_quat_from_mat3(matrix), q, TOLERANCE));
        EXPECT_VEC3_NEAR(tb_mat3_transform(matrix, (tb_vec3){1.0f, 2.0f, 3.0f}),
                         tb_quat_rotate(q, (tb_vec3){1.0f, 2.0f, 3.0f}));
    }
}

/* ---------------------------------------------------------------------------
 * 3×3 matrices.
 * ------------------------------------------------------------------------- */

TB_TEST(core_math, mat3_products_inverses_and_transposes_obey_their_identities)
{
    const tb_mat3 m = tb_mat3_from_mat4(sample_transform());
    const tb_mat3 identity = tb_mat3_identity();
    tb_mat3 inverse = identity;

    TB_TEST_EXPECT(mat3_nearly_equal(tb_mat3_mul(identity, m), m));
    TB_TEST_REQUIRE(tb_mat3_inverse(m, &inverse));
    TB_TEST_EXPECT(mat3_nearly_equal(tb_mat3_mul(inverse, m), identity));
    TB_TEST_EXPECT(mat3_nearly_equal(tb_mat3_mul(m, inverse), identity));
    TB_TEST_EXPECT(mat3_nearly_equal(tb_mat3_transpose(tb_mat3_transpose(m)), m));
    /* The scale's determinant: 2 × 0.5 × 3, which a rotation leaves alone. */
    TB_TEST_EXPECT_FLOAT_NEAR(tb_mat3_determinant(m), 3.0f, TOLERANCE);
    /* A rotation's transpose is its inverse. */
    const tb_mat3 turn = tb_mat3_from_quat(tb_quat_from_axis_angle(unit_y, 1.0f));
    TB_TEST_EXPECT(mat3_nearly_equal(tb_mat3_mul(tb_mat3_transpose(turn), turn), identity));
}

TB_TEST(core_math, a_singular_matrix_has_no_inverse)
{
    const tb_mat3 flat = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}};
    tb_mat3 untouched = tb_mat3_identity();

    TB_TEST_EXPECT(!tb_mat3_inverse(flat, &untouched));
    TB_TEST_EXPECT(mat3_nearly_equal(untouched, tb_mat3_identity()));
    TB_TEST_EXPECT(!tb_mat3_normal(tb_mat4_scaling((tb_vec3){1.0f, 0.0f, 1.0f}), &untouched));
}

TB_TEST(core_math, the_normal_matrix_keeps_normals_perpendicular_to_scaled_surfaces)
{
    /* A slope's normal, under a scale that stretches x: the surface's
     * direction and its transformed normal must stay perpendicular. */
    const tb_mat4 stretch = tb_mat4_scaling((tb_vec3){4.0f, 1.0f, 1.0f});
    const tb_vec3 along_surface = {1.0f, 1.0f, 0.0f};
    const tb_vec3 normal = {-1.0f, 1.0f, 0.0f};
    tb_mat3 normals = tb_mat3_identity();

    TB_TEST_REQUIRE(tb_mat3_normal(stretch, &normals));
    const tb_vec3 moved_surface = tb_mat4_transform_direction(stretch, along_surface);
    const tb_vec3 moved_normal = tb_mat3_transform(normals, normal);

    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_dot(moved_surface, moved_normal), 0.0f, TOLERANCE);
}

/* ---------------------------------------------------------------------------
 * 4×4 matrices.
 * ------------------------------------------------------------------------- */

TB_TEST(core_math, the_translation_is_the_last_column)
{
    const tb_mat4 move = tb_mat4_translation((tb_vec3){1.0f, 2.0f, 3.0f});

    EXPECT_VEC4_NEAR(move.columns[3], ((tb_vec4){1.0f, 2.0f, 3.0f, 1.0f}));
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(move, unit_x), ((tb_vec3){2.0f, 2.0f, 3.0f}));
    EXPECT_VEC3_NEAR(tb_mat4_transform_direction(move, unit_x), unit_x);
    EXPECT_VEC4_NEAR(tb_mat4_transform(move, (tb_vec4){0.0f, 0.0f, 0.0f, 1.0f}),
                     ((tb_vec4){1.0f, 2.0f, 3.0f, 1.0f}));
}

TB_TEST(core_math, a_product_applies_the_right_matrix_first)
{
    const tb_mat4 move = tb_mat4_translation(unit_x);
    const tb_mat4 grow = tb_mat4_scaling((tb_vec3){2.0f, 2.0f, 2.0f});
    const tb_vec3 origin = {0.0f, 0.0f, 0.0f};

    /* Moved, then grown: (2, 0, 0). Grown, then moved: (1, 0, 0). */
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(tb_mat4_mul(grow, move), origin),
                     ((tb_vec3){2.0f, 0.0f, 0.0f}));
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(tb_mat4_mul(move, grow), origin), unit_x);
    TB_TEST_EXPECT(mat4_nearly_equal(tb_mat4_mul(tb_mat4_identity(), move), move));
}

TB_TEST(core_math, an_object_transform_scales_then_turns_then_moves)
{
    const tb_vec3 position = {4.0f, -5.0f, 6.0f};
    const tb_quat turn = tb_quat_from_axis_angle((tb_vec3){1.0f, 2.0f, 3.0f}, 0.7f);
    const tb_vec3 size = {2.0f, 0.5f, 3.0f};
    const tb_mat4 product = tb_mat4_mul(tb_mat4_translation(position),
                                        tb_mat4_mul(tb_mat4_rotation(turn), tb_mat4_scaling(size)));

    TB_TEST_EXPECT(mat4_nearly_equal(tb_mat4_from_trs(position, turn, size), product));
}

TB_TEST(core_math, mat4_inverses_and_transposes_obey_their_identities)
{
    const tb_mat4 m = sample_transform();
    tb_mat4 inverse = tb_mat4_identity();

    TB_TEST_REQUIRE(tb_mat4_inverse(m, &inverse));
    TB_TEST_EXPECT(mat4_nearly_equal(tb_mat4_mul(inverse, m), tb_mat4_identity()));
    TB_TEST_EXPECT(mat4_nearly_equal(tb_mat4_mul(m, inverse), tb_mat4_identity()));
    TB_TEST_EXPECT(mat4_nearly_equal(tb_mat4_transpose(tb_mat4_transpose(m)), m));
    TB_TEST_EXPECT_FLOAT_NEAR(tb_mat4_determinant(m), 3.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_mat4_determinant(tb_mat4_transpose(m)), 3.0f, TOLERANCE);

    /* A projection is no affine transform, and inverts too. */
    const tb_mat4 projection = tb_mat4_perspective(1.0f, 1.5f, 0.5f, 100.0f);
    TB_TEST_REQUIRE(tb_mat4_inverse(projection, &inverse));
    TB_TEST_EXPECT(mat4_nearly_equal(tb_mat4_mul(inverse, projection), tb_mat4_identity()));
}

TB_TEST(core_math, a_singular_mat4_has_no_inverse)
{
    tb_mat4 untouched = tb_mat4_identity();
    tb_mat4 broken = tb_mat4_identity();

    broken.columns[0].x = INFINITY;
    TB_TEST_EXPECT(!tb_mat4_inverse(tb_mat4_scaling((tb_vec3){1.0f, 1.0f, 0.0f}), &untouched));
    TB_TEST_EXPECT(!tb_mat4_inverse(broken, &untouched));
    TB_TEST_EXPECT(mat4_nearly_equal(untouched, tb_mat4_identity()));
}

TB_TEST(core_math, transforming_many_gives_what_transforming_one_at_a_time_gives)
{
    const tb_mat4 m = sample_transform();
    tb_vec4 vectors[5] = {{1.0f, 0.0f, 0.0f, 0.0f},
                          {0.0f, 1.0f, 0.0f, 1.0f},
                          {1.0f, 2.0f, 3.0f, 1.0f},
                          {-4.0f, 0.5f, 2.0f, 0.0f},
                          {7.0f, -1.0f, 0.0f, 2.0f}};
    tb_vec3 points[5] = {{0.0f, 0.0f, 0.0f},
                         {1.0f, 2.0f, 3.0f},
                         {-1.0f, -2.0f, -3.0f},
                         {9.0f, 0.0f, 0.5f},
                         {0.0f, 4.0f, -4.0f}};
    tb_vec4 expected_vectors[5] = {0};
    tb_vec3 expected_points[5] = {0};

    for (size_t index = 0; index < 5; index++)
    {
        expected_vectors[index] = tb_mat4_transform(m, vectors[index]);
        expected_points[index] = tb_mat4_transform_point(m, points[index]);
    }
    /* In place, as the header allows. */
    tb_mat4_transform_many(m, vectors, vectors, 5);
    tb_mat4_transform_points(m, points, points, 5);
    for (size_t index = 0; index < 5; index++)
    {
        EXPECT_VEC4_NEAR(vectors[index], expected_vectors[index]);
        EXPECT_VEC3_NEAR(points[index], expected_points[index]);
    }
    tb_mat4_transform_many(m, NULL, NULL, 0);
    tb_mat4_transform_points(m, NULL, NULL, 0);
}

TB_TEST(core_math, a_view_puts_the_camera_at_the_origin_looking_down_minus_z)
{
    const tb_vec3 eye = {3.0f, 4.0f, 5.0f};
    const tb_vec3 target = {3.0f, 4.0f, -5.0f};
    tb_mat4 view = tb_mat4_identity();

    TB_TEST_REQUIRE(tb_mat4_look_at(eye, target, unit_y, &view));
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(view, eye), ((tb_vec3){0.0f, 0.0f, 0.0f}));
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(view, target), ((tb_vec3){0.0f, 0.0f, -10.0f}));
    /* Above the camera stays above, and its right stays right. */
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(view, tb_vec3_add(eye, unit_y)), unit_y);
    EXPECT_VEC3_NEAR(tb_mat4_transform_point(view, tb_vec3_add(eye, unit_x)), unit_x);

    /* Looking along +x, ahead is −z in the view and the world's +z is right. */
    TB_TEST_REQUIRE(tb_mat4_look_at(eye, tb_vec3_add(eye, unit_x), unit_y, &view));
    EXPECT_VEC3_NEAR(tb_mat4_transform_direction(view, unit_x), tb_vec3_negate(unit_z));
    EXPECT_VEC3_NEAR(tb_mat4_transform_direction(view, unit_z), unit_x);
}

TB_TEST(core_math, a_view_needs_a_line_of_sight_and_an_up_off_it)
{
    tb_mat4 view = tb_mat4_identity();

    TB_TEST_EXPECT(!tb_mat4_look_at(unit_x, unit_x, unit_y, &view));
    TB_TEST_EXPECT(!tb_mat4_look_at(unit_x, tb_vec3_add(unit_x, unit_y), unit_y, &view));
    TB_TEST_EXPECT(mat4_nearly_equal(view, tb_mat4_identity()));
}

/* Clip coordinates of a point in view space, divided by w. */
static tb_vec3 project(tb_mat4 projection, tb_vec3 point)
{
    const tb_vec4 clip = tb_mat4_transform(projection, tb_vec4_from_vec3(point, 1.0f));

    return tb_vec3_scale(tb_vec3_from_vec4(clip), 1.0f / clip.w);
}

TB_TEST(core_math, perspective_depth_is_reversed_from_one_at_near_to_zero_at_far)
{
    const float fov = tb_radians(90.0f);
    const tb_mat4 projection = tb_mat4_perspective(fov, 2.0f, 0.5f, 100.0f);

    EXPECT_VEC3_NEAR(project(projection, (tb_vec3){0.0f, 0.0f, -0.5f}),
                     ((tb_vec3){0.0f, 0.0f, 1.0f}));
    TB_TEST_EXPECT_FLOAT_NEAR(project(projection, (tb_vec3){0.0f, 0.0f, -100.0f}).z, 0.0f,
                              TOLERANCE);
    /* A 90° view reaches as high as it is far; twice as wide as high. */
    EXPECT_VEC3_NEAR(project(projection, (tb_vec3){2.0f, 1.0f, -1.0f}),
                     ((tb_vec3){1.0f, 1.0f, project(projection, (tb_vec3){0, 0, -1.0f}).z}));
    /* Y is up in clip space. */
    TB_TEST_EXPECT_FLOAT(project(projection, (tb_vec3){0.0f, 1.0f, -10.0f}).y, >, 0.0f);
}

TB_TEST(core_math, an_infinite_perspective_has_depth_falling_toward_zero)
{
    const tb_mat4 projection = tb_mat4_perspective(1.0f, 1.0f, 0.1f, INFINITY);

    TB_TEST_EXPECT_FLOAT_NEAR(project(projection, (tb_vec3){0.0f, 0.0f, -0.1f}).z, 1.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(project(projection, (tb_vec3){0.0f, 0.0f, -10.0f}).z, 0.01f,
                              TOLERANCE);
    TB_TEST_EXPECT_FLOAT(project(projection, (tb_vec3){0.0f, 0.0f, -1e9f}).z, >, 0.0f);
}

TB_TEST(core_math, an_orthographic_box_fills_clip_space)
{
    const tb_mat4 projection = tb_mat4_orthographic(-4.0f, 4.0f, -2.0f, 2.0f, 1.0f, 11.0f);
    const tb_mat4 screen = tb_mat4_orthographic(0.0f, 1280.0f, 0.0f, 720.0f, -1.0f, 1.0f);

    EXPECT_VEC3_NEAR(project(projection, (tb_vec3){-4.0f, -2.0f, -1.0f}),
                     ((tb_vec3){-1.0f, -1.0f, 1.0f}));
    EXPECT_VEC3_NEAR(project(projection, (tb_vec3){4.0f, 2.0f, -11.0f}),
                     ((tb_vec3){1.0f, 1.0f, 0.0f}));
    /* A 2D view in pixels: its center is the middle of clip space. */
    EXPECT_VEC3_NEAR(project(screen, (tb_vec3){640.0f, 360.0f, 0.0f}),
                     ((tb_vec3){0.0f, 0.0f, 0.5f}));
}
