/* The CPU's part of temporal anti-aliasing and the render scale: Halton's
 * sequence, the jitter it gives each frame and the projection it moves,
 * the render size of a scale, and the profile and quality a device takes. */
#include "render/renderer_internal.h"

#include "badger/fmt.h"
#include "badger/str.h"

#include "tb_test.h"

#include <math.h>

/* Halton's sequence is the radical inverse of the index in its base: the
 * index's digits mirrored about the point. */
TB_TEST(render_temporal, halton_mirrors_the_digits_of_the_index)
{
    TB_TEST_EXPECT_FLOAT(tb_halton(0, 2), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_halton(1, 2), ==, 0.5f);
    TB_TEST_EXPECT_FLOAT(tb_halton(2, 2), ==, 0.25f);
    TB_TEST_EXPECT_FLOAT(tb_halton(3, 2), ==, 0.75f);
    TB_TEST_EXPECT_FLOAT(tb_halton(6, 2), ==, 0.375f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_halton(1, 3), 1.0f / 3.0f, 1e-7f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_halton(2, 3), 2.0f / 3.0f, 1e-7f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_halton(4, 3), 4.0f / 9.0f, 1e-7f);
}

/* Eight jitters a cycle, each within half a pixel, all different, and
 * centered on the pixel: the sample positions cover it evenly. */
TB_TEST(render_temporal, the_jitter_covers_the_pixel_in_a_cycle_of_eight)
{
    tb_vec2 sum = {0};

    for (uint32_t frame = 0; frame < 8; frame++)
    {
        const tb_vec2 jitter = tb_temporal_jitter(frame);
        const tb_vec2 again = tb_temporal_jitter(frame + 8);
        TB_TEST_CONTEXT("frame %u", frame);
        TB_TEST_EXPECT(jitter.x >= -0.5f && jitter.x < 0.5f);
        TB_TEST_EXPECT(jitter.y >= -0.5f && jitter.y < 0.5f);
        TB_TEST_EXPECT_FLOAT(again.x, ==, jitter.x);
        TB_TEST_EXPECT_FLOAT(again.y, ==, jitter.y);
        for (uint32_t other = 0; other < frame; other++)
        {
            const tb_vec2 before = tb_temporal_jitter(other);
            TB_TEST_EXPECT(before.x != jitter.x || before.y != jitter.y);
        }
        sum.x += jitter.x;
        sum.y += jitter.y;
    }
    TB_TEST_EXPECT_FLOAT_NEAR(sum.x / 8.0f, 0.0f, 0.07f);
    TB_TEST_EXPECT_FLOAT_NEAR(sum.y / 8.0f, 0.0f, 0.07f);
}

/* A jittered projection moves every point by the jitter in pixels, near or
 * far, right for +x and down for +y, and leaves depth as it was. */
TB_TEST(render_temporal, a_jittered_projection_moves_points_by_the_jitter_in_pixels)
{
    const tb_camera camera = tb_camera_perspective(tb_radians(60.0f));
    const tb_mat4 projection = tb_camera_projection_of(&camera, 2.0f);
    const tb_vec2 jitter = {0.25f, -0.375f};
    const tb_mat4 jittered = tb_temporal_jittered(projection, jitter, 200, 100);
    const float depths[2] = {-0.5f, -400.0f};

    for (int index = 0; index < 2; index++)
    {
        const tb_vec4 point = {0.3f, -0.2f, depths[index], 1.0f};
        const tb_vec4 plain = tb_mat4_transform(projection, point);
        const tb_vec4 moved = tb_mat4_transform(jittered, point);
        /* From clip space to pixels: x across 200 pixels, y down 100. */
        const float dx = (moved.x / moved.w - plain.x / plain.w) * 0.5f * 200.0f;
        const float dy = -(moved.y / moved.w - plain.y / plain.w) * 0.5f * 100.0f;
        TB_TEST_CONTEXT("depth %g", (double)depths[index]);
        TB_TEST_EXPECT_FLOAT_NEAR(dx, jitter.x, 1e-3f);
        TB_TEST_EXPECT_FLOAT_NEAR(dy, jitter.y, 1e-3f);
        TB_TEST_EXPECT_FLOAT_NEAR(moved.z / moved.w, plain.z / plain.w, 1e-6f);
    }
}

/* The render size is the output's times the scale, rounded, at least a
 * pixel, and no larger than the device's largest texture. */
TB_TEST(render_temporal, the_render_size_is_the_scale_of_the_output)
{
    uint32_t width = 0;
    uint32_t height = 0;

    tb_render_size_of(1920, 1080, 0.5f, 16384, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, 960);
    TB_TEST_EXPECT_UINT(height, ==, 540);
    tb_render_size_of(1920, 1080, 2.0f / 3.0f, 16384, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, 1280);
    TB_TEST_EXPECT_UINT(height, ==, 720);
    tb_render_size_of(301, 3, 0.25f, 16384, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, 75);
    TB_TEST_EXPECT_UINT(height, ==, 1);
    tb_render_size_of(12000, 100, 2.0f, 16384, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, 16384);
    TB_TEST_EXPECT_UINT(height, ==, 200);
}

/* A viewport of another size starts the history again, even where the
 * render size rounds to the same: the history is the viewport's size, and
 * a frame of another shape does not follow from it. */
TB_TEST(render_temporal, a_viewport_of_another_size_starts_the_history_again)
{
    tb_renderer renderer = {0};
    const tb_rhi_device_info info = {.features = TB_RHI_FEATURE_BINDLESS};

    renderer.deferred = true;
    renderer.info.max_texture_size = 16384;
    renderer.capabilities = tb_renderer_capabilities_for(&info, TB_RENDERER_PROFILE_FULL);
    renderer.quality_asked = (tb_renderer_quality)TB_RENDERER_QUALITY_INIT;
    renderer.quality_asked.render_scale = 0.5f;
    renderer.quality.temporal_aa = true;
    renderer.viewport = (tb_viewport){.width = 101, .height = 100, .scale = {1, 1}};
    renderer.history_width = 101;
    renderer.history_height = 100;
    renderer.previous_render_width = 51;
    renderer.previous_render_height = 50;
    renderer.history_valid = true;
    tb_renderer_quality_begin_frame(&renderer);
    TB_TEST_EXPECT(renderer.history_valid);
    TB_TEST_EXPECT_UINT(renderer.render_width, ==, 51u);

    /* One pixel wider: the same render size, but another viewport. */
    renderer.viewport.width = 102;
    tb_renderer_quality_begin_frame(&renderer);
    TB_TEST_EXPECT_UINT(renderer.render_width, ==, 51u);
    TB_TEST_EXPECT(!renderer.history_valid);
}

/* A software adapter takes the low profile by itself, unless the full one
 * is asked for; a GPU takes the full one; a device without bindless
 * resources has no raster lighting mode, and is low whatever is asked. */
TB_TEST(render_temporal, the_profile_follows_the_device)
{
    tb_rhi_device_info info = {.features = TB_RHI_FEATURE_BINDLESS, .software = true};
    tb_renderer_capabilities capabilities = {0};

    (void)tb_fmt_buffer(info.adapter, sizeof(info.adapter), "Microsoft Basic Render Driver");
    capabilities = tb_renderer_capabilities_for(&info, TB_RENDERER_PROFILE_AUTOMATIC);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_LOW);
    TB_TEST_EXPECT(capabilities.raster_lighting);
    TB_TEST_EXPECT(!capabilities.temporal_aa);
    TB_TEST_EXPECT(!capabilities.reflections);
    TB_TEST_EXPECT_FLOAT(capabilities.max_render_scale, ==, TB_RENDERER_LOW_RENDER_SCALE);
    TB_TEST_EXPECT_UINT(capabilities.max_lights, ==, TB_RENDERER_LOW_LIGHTS);
    TB_TEST_EXPECT(tb_str_find(capabilities.reason, 0, TB_STR_END, "Basic Render Driver") !=
                   TB_STR_NOT_FOUND);
    capabilities = tb_renderer_capabilities_for(&info, TB_RENDERER_PROFILE_FULL);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_FULL);
    TB_TEST_EXPECT(capabilities.temporal_aa);
    TB_TEST_EXPECT(capabilities.reason[0] == '\0');

    info.software = false;
    capabilities = tb_renderer_capabilities_for(&info, TB_RENDERER_PROFILE_AUTOMATIC);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_FULL);
    capabilities = tb_renderer_capabilities_for(&info, TB_RENDERER_PROFILE_LOW);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_LOW);

    info.features = 0;
    capabilities = tb_renderer_capabilities_for(&info, TB_RENDERER_PROFILE_FULL);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_LOW);
    TB_TEST_EXPECT(!capabilities.raster_lighting);
    TB_TEST_EXPECT_FLOAT(capabilities.max_render_scale, ==, 1.0f);
}

/* A quality within capabilities keeps what they allow and caps the rest. */
TB_TEST(render_temporal, a_quality_is_capped_by_the_capabilities)
{
    const tb_renderer_quality asked = {
        .render_scale = 0.75f, .temporal_aa = true, .max_lights = 100, .reflections = true};
    tb_renderer_capabilities capabilities = {
        .max_render_scale = 0.5f, .temporal_aa = false, .max_lights = 16, .reflections = true};
    tb_renderer_quality quality = tb_renderer_quality_within(&asked, &capabilities);

    TB_TEST_EXPECT_FLOAT(quality.render_scale, ==, 0.5f);
    TB_TEST_EXPECT(!quality.temporal_aa);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, 16);
    TB_TEST_EXPECT(quality.reflections);
    capabilities.max_render_scale = 2.0f;
    capabilities.max_lights = 1024;
    quality = tb_renderer_quality_within(&asked, &capabilities);
    TB_TEST_EXPECT_FLOAT(quality.render_scale, ==, 0.75f);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, 100);
}
