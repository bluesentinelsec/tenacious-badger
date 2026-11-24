/* Lights as the lighting shader reads them: a directional light's way of
 * unit length and its radiance times pi, a point light's inverse range
 * squared, and a spot's cone as the cosines of its angles; and the light
 * of a frame without lights, toward the sky's sun or from above. */
#include "render/renderer_internal.h"

#include "badger/memory.h"

#include "tb_test.h"

#include <math.h>

/* A directional light's way is normalized, its kind is 0, its radiance is
 * its color times its intensity times pi (a Lambert surface divides by
 * pi), and it has no range. */
TB_TEST(render_light_pack, a_directional_light_packs_its_unit_way_and_radiance)
{
    tb_light light = TB_LIGHT_INIT;
    tb_packed_light packed = {0};

    light.kind = TB_LIGHT_DIRECTIONAL;
    light.direction = (tb_vec3){0, -3, 4};
    light.color = (tb_vec3){1.0f, 0.5f, 0.25f};
    light.intensity = 2.0f;
    packed = tb_light_pack(&light);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.direction.x, 0.0f, 1e-6f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.direction.y, -0.6f, 1e-6f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.direction.z, 0.8f, 1e-6f);
    TB_TEST_EXPECT_FLOAT(packed.direction.w, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.radiance.x, 2.0f * TB_PI, 1e-5f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.radiance.y, TB_PI, 1e-5f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.radiance.z, 0.5f * TB_PI, 1e-5f);
    TB_TEST_EXPECT_FLOAT(packed.position.w, ==, 0.0f);
}

/* A point light keeps its position, and its range as one over its square,
 * which the shader's window of distance squared needs. */
TB_TEST(render_light_pack, a_point_light_packs_its_inverse_range_squared)
{
    tb_light light = TB_LIGHT_INIT;
    tb_packed_light packed = {0};

    light.position = (tb_vec3){1, 2, 3};
    light.range = 4.0f;
    packed = tb_light_pack(&light);
    TB_TEST_EXPECT_FLOAT(packed.position.x, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(packed.position.z, ==, 3.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.position.w, 1.0f / 16.0f, 1e-7f);
    TB_TEST_EXPECT_FLOAT(packed.direction.w, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(packed.cone.x, ==, 0.0f);
}

/* A spot's cone is the cosine of its outer angle, where the light ends,
 * and one over the cosines' difference, so that the shader's fade is a
 * multiply-add: 0 at the outer angle, 1 at the inner. */
TB_TEST(render_light_pack, a_spot_light_packs_its_cone_as_cosines)
{
    tb_light light = TB_LIGHT_INIT;
    tb_packed_light packed = {0};
    float fade_inner = 0.0f;

    light.kind = TB_LIGHT_SPOT;
    light.inner_angle = 0.25f;
    light.outer_angle = 0.5f;
    packed = tb_light_pack(&light);
    TB_TEST_EXPECT_FLOAT(packed.direction.w, ==, 2.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(packed.cone.x, cosf(0.5f), 1e-6f);
    fade_inner = (cosf(0.25f) - packed.cone.x) * packed.cone.y;
    TB_TEST_EXPECT_FLOAT_NEAR(fade_inner, 1.0f, 1e-4f);
}

/* Without lights, a frame is lit toward the procedural sky's sun, in its
 * color; without a sun, from above. */
TB_TEST(render_light_pack, the_default_light_follows_the_sun_or_comes_from_above)
{
    tb_renderer *const renderer = tb_calloc(1, sizeof(*renderer));
    const tb_sky sky = TB_SKY_PROCEDURAL_INIT;
    tb_light light = {0};
    tb_vec3 toward = {0};

    renderer->camera = tb_camera_perspective(1.0f);
    renderer->sky = sky;
    light = tb_renderer_default_light(renderer);
    toward = tb_vec3_normalize(sky.sun_direction);
    TB_TEST_EXPECT_UINT(light.kind, ==, TB_LIGHT_DIRECTIONAL);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_dot(light.direction, toward), -1.0f, 1e-5f);
    TB_TEST_EXPECT_FLOAT(light.color.z, ==, sky.sun_color.z);
    renderer->sky = (tb_sky){0};
    light = tb_renderer_default_light(renderer);
    TB_TEST_EXPECT(light.direction.y < -0.5f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_vec3_length(light.direction), 1.0f, 1e-5f);
    tb_free(renderer);
}
