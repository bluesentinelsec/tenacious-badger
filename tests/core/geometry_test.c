/* badger/geometry.h decides what is drawn and what a shot hits: a cull that
 * drops a visible object, or a ray that passes through a wall, is a visible
 * bug. Each case checks a shape against points and shapes whose answer is
 * known, including the edges: touching, starting inside, and parallel. */
#include "badger/geometry.h"

#include "tb_test.h"

#include <math.h>

#define TOLERANCE 1e-5f

static const tb_aabb unit_box = {.minimum = {-1.0f, -1.0f, -1.0f}, .maximum = {1.0f, 1.0f, 1.0f}};

/* A camera at the origin looking down −z: 90° each way, from 1 to 100. */
static tb_frustum camera_frustum(float far_plane)
{
    return tb_frustum_from_matrix(tb_mat4_perspective(tb_radians(90.0f), 1.0f, 1.0f, far_plane));
}

/* ---------------------------------------------------------------------------
 * Planes.
 * ------------------------------------------------------------------------- */

TB_TEST(core_geometry, a_plane_measures_distance_in_front_and_behind)
{
    const tb_plane floor =
        tb_plane_from_point_normal((tb_vec3){0.0f, 2.0f, 0.0f}, (tb_vec3){0.0f, 5.0f, 0.0f});

    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(floor.normal), 1.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(floor.distance, 2.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_plane_signed_distance(floor, (tb_vec3){7.0f, 5.0f, 1.0f}), 3.0f,
                              TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_plane_signed_distance(floor, (tb_vec3){0.0f, 0.0f, 0.0f}), -2.0f,
                              TOLERANCE);
}

TB_TEST(core_geometry, a_plane_through_points_faces_where_they_turn_counterclockwise)
{
    tb_plane plane = {0};

    /* Counterclockwise seen from +z. */
    TB_TEST_REQUIRE(tb_plane_from_points((tb_vec3){0.0f, 0.0f, 1.0f}, (tb_vec3){1.0f, 0.0f, 1.0f},
                                         (tb_vec3){0.0f, 1.0f, 1.0f}, &plane));
    TB_TEST_EXPECT_FLOAT_NEAR(plane.normal.z, 1.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(plane.distance, 1.0f, TOLERANCE);
}

TB_TEST(core_geometry, points_on_a_line_make_no_plane)
{
    tb_plane plane = {.distance = 42.0f};

    TB_TEST_EXPECT(!tb_plane_from_points((tb_vec3){0.0f, 0.0f, 0.0f}, (tb_vec3){1.0f, 1.0f, 1.0f},
                                         (tb_vec3){2.0f, 2.0f, 2.0f}, &plane));
    TB_TEST_EXPECT_FLOAT(plane.distance, ==, 42.0f);
}

/* ---------------------------------------------------------------------------
 * Boxes and spheres.
 * ------------------------------------------------------------------------- */

TB_TEST(core_geometry, a_box_of_points_holds_every_point)
{
    const tb_vec3 points[] = {{1.0f, -2.0f, 3.0f}, {-4.0f, 5.0f, 0.0f}, {2.0f, 0.0f, -6.0f}};
    const tb_aabb box = tb_aabb_from_points(points, 3);

    TB_TEST_EXPECT(tb_vec3_nearly_equal(box.minimum, (tb_vec3){-4.0f, -2.0f, -6.0f}, 0.0f));
    TB_TEST_EXPECT(tb_vec3_nearly_equal(box.maximum, (tb_vec3){2.0f, 5.0f, 3.0f}, 0.0f));
    for (size_t index = 0; index < 3; index++)
    {
        TB_TEST_EXPECT(tb_aabb_contains_point(box, points[index]));
    }
    TB_TEST_EXPECT(!tb_aabb_contains_point(box, (tb_vec3){0.0f, 6.0f, 0.0f}));
    TB_TEST_EXPECT(tb_vec3_nearly_equal(tb_aabb_center(box), (tb_vec3){-1.0f, 1.5f, -1.5f}, 0.0f));
    TB_TEST_EXPECT(tb_vec3_nearly_equal(tb_aabb_extents(box), (tb_vec3){3.0f, 3.5f, 4.5f}, 0.0f));
}

TB_TEST(core_geometry, boxes_overlap_when_they_touch_and_union_into_one)
{
    const tb_aabb right = {.minimum = {1.0f, -1.0f, -1.0f}, .maximum = {3.0f, 1.0f, 1.0f}};
    const tb_aabb far_away = {.minimum = {5.0f, 5.0f, 5.0f}, .maximum = {6.0f, 6.0f, 6.0f}};
    const tb_aabb both = tb_aabb_union(unit_box, far_away);

    TB_TEST_EXPECT(tb_aabb_overlaps(unit_box, right));
    TB_TEST_EXPECT(!tb_aabb_overlaps(unit_box, far_away));
    TB_TEST_EXPECT(tb_vec3_nearly_equal(both.minimum, unit_box.minimum, 0.0f));
    TB_TEST_EXPECT(tb_vec3_nearly_equal(both.maximum, far_away.maximum, 0.0f));
}

TB_TEST(core_geometry, a_turned_box_is_bounded_by_a_box_that_holds_its_corners)
{
    /* An eighth of a turn about y: the corners reach √2 along x and z. */
    const tb_quat turn = tb_quat_from_axis_angle((tb_vec3){0.0f, 1.0f, 0.0f}, TB_PI / 4.0f);
    const tb_mat4 m =
        tb_mat4_from_trs((tb_vec3){10.0f, 0.0f, 0.0f}, turn, (tb_vec3){1.0f, 2.0f, 1.0f});
    const tb_aabb moved = tb_aabb_transform(unit_box, m);
    const float reach = sqrtf(2.0f);

    TB_TEST_EXPECT(
        tb_vec3_nearly_equal(moved.minimum, (tb_vec3){10.0f - reach, -2.0f, -reach}, TOLERANCE));
    TB_TEST_EXPECT(
        tb_vec3_nearly_equal(moved.maximum, (tb_vec3){10.0f + reach, 2.0f, reach}, TOLERANCE));
}

TB_TEST(core_geometry, spheres_hold_points_and_overlap_spheres_and_boxes)
{
    const tb_sphere ball = {.center = {0.0f, 0.0f, 0.0f}, .radius = 2.0f};
    const tb_sphere touching = {.center = {4.0f, 0.0f, 0.0f}, .radius = 2.0f};
    const tb_sphere apart = {.center = {5.0f, 0.0f, 0.0f}, .radius = 2.0f};
    const tb_aabb box = {.minimum = {3.0f, 3.0f, 3.0f}, .maximum = {4.0f, 4.0f, 4.0f}};
    const tb_sphere near_corner = {.center = {2.0f, 2.0f, 2.0f}, .radius = 1.8f};

    TB_TEST_EXPECT(tb_sphere_contains_point(ball, (tb_vec3){0.0f, 2.0f, 0.0f}));
    TB_TEST_EXPECT(!tb_sphere_contains_point(ball, (tb_vec3){1.5f, 1.5f, 0.0f}));
    TB_TEST_EXPECT(tb_sphere_overlaps(ball, touching));
    TB_TEST_EXPECT(!tb_sphere_overlaps(ball, apart));
    /* The corner (3, 3, 3) is √3 ≈ 1.73 away. */
    TB_TEST_EXPECT(tb_sphere_overlaps_aabb(near_corner, box));
    TB_TEST_EXPECT(!tb_sphere_overlaps_aabb(ball, box));
}

/* ---------------------------------------------------------------------------
 * Rays.
 * ------------------------------------------------------------------------- */

TB_TEST(core_geometry, a_ray_meets_a_plane_ahead_from_either_side)
{
    const tb_plane floor =
        tb_plane_from_point_normal((tb_vec3){0.0f, 0.0f, 0.0f}, (tb_vec3){0.0f, 1.0f, 0.0f});
    const tb_ray down = {.origin = {1.0f, 4.0f, 0.0f}, .direction = {0.0f, -2.0f, 0.0f}};
    const tb_ray up = {.origin = {1.0f, -4.0f, 0.0f}, .direction = {0.0f, 1.0f, 0.0f}};
    const tb_ray away = {.origin = {1.0f, 4.0f, 0.0f}, .direction = {0.0f, 1.0f, 0.0f}};
    const tb_ray level = {.origin = {1.0f, 4.0f, 0.0f}, .direction = {1.0f, 0.0f, 0.0f}};
    float t = -1.0f;

    TB_TEST_REQUIRE(tb_ray_intersect_plane(down, floor, &t));
    TB_TEST_EXPECT_FLOAT_NEAR(t, 2.0f, TOLERANCE);
    TB_TEST_EXPECT(
        tb_vec3_nearly_equal(tb_ray_at(down, t), (tb_vec3){1.0f, 0.0f, 0.0f}, TOLERANCE));
    TB_TEST_REQUIRE(tb_ray_intersect_plane(up, floor, &t));
    TB_TEST_EXPECT_FLOAT_NEAR(t, 4.0f, TOLERANCE);
    t = -1.0f;
    TB_TEST_EXPECT(!tb_ray_intersect_plane(away, floor, &t));
    TB_TEST_EXPECT(!tb_ray_intersect_plane(level, floor, &t));
    TB_TEST_EXPECT_FLOAT(t, ==, -1.0f);
}

TB_TEST(core_geometry, a_ray_enters_a_box_on_its_nearest_face)
{
    const tb_ray along_x = {.origin = {-5.0f, 0.5f, 0.0f}, .direction = {1.0f, 0.0f, 0.0f}};
    const tb_ray diagonal = {.origin = {-3.0f, -3.0f, -3.0f}, .direction = {1.0f, 1.0f, 1.0f}};
    const tb_ray inside = {.origin = {0.0f, 0.0f, 0.0f}, .direction = {0.0f, 0.0f, -1.0f}};
    const tb_ray beside = {.origin = {-5.0f, 2.0f, 0.0f}, .direction = {1.0f, 0.0f, 0.0f}};
    const tb_ray behind = {.origin = {5.0f, 0.0f, 0.0f}, .direction = {1.0f, 0.0f, 0.0f}};
    const tb_ray past = {.origin = {-5.0f, 0.0f, 0.0f}, .direction = {1.0f, 1.0f, 0.0f}};
    float t = -1.0f;

    TB_TEST_REQUIRE(tb_ray_intersect_aabb(along_x, unit_box, &t));
    TB_TEST_EXPECT_FLOAT_NEAR(t, 4.0f, TOLERANCE);
    TB_TEST_REQUIRE(tb_ray_intersect_aabb(diagonal, unit_box, &t));
    TB_TEST_EXPECT_FLOAT_NEAR(t, 2.0f, TOLERANCE);
    TB_TEST_REQUIRE(tb_ray_intersect_aabb(inside, unit_box, &t));
    TB_TEST_EXPECT_FLOAT(t, ==, 0.0f);
    t = -1.0f;
    TB_TEST_EXPECT(!tb_ray_intersect_aabb(beside, unit_box, &t));
    TB_TEST_EXPECT(!tb_ray_intersect_aabb(behind, unit_box, &t));
    TB_TEST_EXPECT(!tb_ray_intersect_aabb(past, unit_box, &t));
    TB_TEST_EXPECT_FLOAT(t, ==, -1.0f);
}

TB_TEST(core_geometry, a_ray_enters_a_sphere_where_it_first_meets_it)
{
    const tb_sphere ball = {.center = {0.0f, 0.0f, -10.0f}, .radius = 2.0f};
    const tb_ray ahead = {.origin = {0.0f, 0.0f, 0.0f}, .direction = {0.0f, 0.0f, -2.0f}};
    const tb_ray inside = {.origin = {0.0f, 0.0f, -10.0f}, .direction = {1.0f, 0.0f, 0.0f}};
    const tb_ray miss = {.origin = {0.0f, 3.0f, 0.0f}, .direction = {0.0f, 0.0f, -1.0f}};
    const tb_ray away = {.origin = {0.0f, 0.0f, 0.0f}, .direction = {0.0f, 0.0f, 1.0f}};
    const tb_ray still = {.origin = {0.0f, 0.0f, 0.0f}, .direction = {0.0f, 0.0f, 0.0f}};
    float t = -1.0f;

    TB_TEST_REQUIRE(tb_ray_intersect_sphere(ahead, ball, &t));
    TB_TEST_EXPECT_FLOAT_NEAR(t, 4.0f, TOLERANCE);
    TB_TEST_REQUIRE(tb_ray_intersect_sphere(inside, ball, &t));
    TB_TEST_EXPECT_FLOAT(t, ==, 0.0f);
    t = -1.0f;
    TB_TEST_EXPECT(!tb_ray_intersect_sphere(miss, ball, &t));
    TB_TEST_EXPECT(!tb_ray_intersect_sphere(away, ball, &t));
    TB_TEST_EXPECT(!tb_ray_intersect_sphere(still, ball, &t));
    TB_TEST_EXPECT_FLOAT(t, ==, -1.0f);
}

/* ---------------------------------------------------------------------------
 * Frustums.
 * ------------------------------------------------------------------------- */

TB_TEST(core_geometry, a_frustum_holds_what_the_camera_sees)
{
    const tb_frustum view = camera_frustum(100.0f);

    TB_TEST_EXPECT(tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, -10.0f}));
    /* 90° wide: at distance 10, the edge is 10 to each side. */
    TB_TEST_EXPECT(tb_frustum_contains_point(&view, (tb_vec3){9.9f, 0.0f, -10.0f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){10.1f, 0.0f, -10.0f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){0.0f, 10.1f, -10.0f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, 10.0f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, -0.5f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, -101.0f}));
    TB_TEST_EXPECT_FLOAT_NEAR(view.planes[TB_FRUSTUM_NEAR].normal.z, -1.0f, TOLERANCE);
    TB_TEST_EXPECT_FLOAT_NEAR(view.planes[TB_FRUSTUM_FAR].normal.z, 1.0f, TOLERANCE);
}

TB_TEST(core_geometry, a_frustum_with_no_far_plane_holds_everything_far_ahead)
{
    const tb_frustum view = camera_frustum(INFINITY);

    TB_TEST_EXPECT(tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, -1e6f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, -0.5f}));
    TB_TEST_EXPECT_FLOAT(tb_vec3_length(view.planes[TB_FRUSTUM_FAR].normal), ==, 0.0f);
    TB_TEST_EXPECT_INT(tb_frustum_test_sphere(&view, (tb_sphere){{0.0f, 0.0f, -1e6f}, 1.0f}), ==,
                       TB_CONTAINMENT_INSIDE);
}

TB_TEST(core_geometry, a_frustum_sorts_shapes_into_inside_outside_and_across)
{
    const tb_frustum view = camera_frustum(100.0f);
    const tb_sphere ahead = {.center = {0.0f, 0.0f, -20.0f}, .radius = 1.0f};
    const tb_sphere across_the_edge = {.center = {10.0f, 0.0f, -10.0f}, .radius = 1.0f};
    const tb_sphere behind = {.center = {0.0f, 0.0f, 10.0f}, .radius = 1.0f};
    const tb_aabb box_ahead = {.minimum = {-1.0f, -1.0f, -21.0f}, .maximum = {1.0f, 1.0f, -19.0f}};
    const tb_aabb box_across = {.minimum = {-1.0f, -1.0f, -2.0f}, .maximum = {1.0f, 1.0f, 0.0f}};
    const tb_aabb box_behind = {.minimum = {-1.0f, -1.0f, 5.0f}, .maximum = {1.0f, 1.0f, 6.0f}};

    TB_TEST_EXPECT_INT(tb_frustum_test_sphere(&view, ahead), ==, TB_CONTAINMENT_INSIDE);
    TB_TEST_EXPECT_INT(tb_frustum_test_sphere(&view, across_the_edge), ==,
                       TB_CONTAINMENT_INTERSECTING);
    TB_TEST_EXPECT_INT(tb_frustum_test_sphere(&view, behind), ==, TB_CONTAINMENT_OUTSIDE);
    TB_TEST_EXPECT_INT(tb_frustum_test_aabb(&view, box_ahead), ==, TB_CONTAINMENT_INSIDE);
    TB_TEST_EXPECT_INT(tb_frustum_test_aabb(&view, box_across), ==, TB_CONTAINMENT_INTERSECTING);
    TB_TEST_EXPECT_INT(tb_frustum_test_aabb(&view, box_behind), ==, TB_CONTAINMENT_OUTSIDE);
}

TB_TEST(core_geometry, a_frustum_follows_the_camera_through_its_view_matrix)
{
    /* The camera at (0, 0, 50) looking toward +z sees what is beyond it. */
    tb_mat4 camera = tb_mat4_identity();

    TB_TEST_REQUIRE(tb_mat4_look_at((tb_vec3){0.0f, 0.0f, 50.0f}, (tb_vec3){0.0f, 0.0f, 60.0f},
                                    (tb_vec3){0.0f, 1.0f, 0.0f}, &camera));
    const tb_mat4 projection = tb_mat4_perspective(tb_radians(90.0f), 1.0f, 1.0f, 100.0f);
    const tb_frustum view = tb_frustum_from_matrix(tb_mat4_mul(projection, camera));

    TB_TEST_EXPECT(tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, 70.0f}));
    TB_TEST_EXPECT(!tb_frustum_contains_point(&view, (tb_vec3){0.0f, 0.0f, 40.0f}));
}
