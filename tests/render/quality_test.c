/* The renderer's quality: the values the engine's settings set, as
 * Unreal's scalability groups do; and on the null backend, the quality a
 * renderer takes within its profile, and the render size that follows. */
#include "badger/engine_settings.h"
#include "badger/renderer.h"
#include "badger/settings.h"
#include "badger/window.h"

#include "tb_test.h"

/* The choices of the settings, as docs/settings.md lists them. */
enum
{
    UPSCALER_NATIVE = 0,
    UPSCALER_PERFORMANCE = 3,
    UPSCALER_CUSTOM = 5,
    ANTI_ALIASING_OFF = 0,
    QUALITY_LOW = 0,
    QUALITY_ULTRA = 3
};

/* The defaults: the upscaler's quality mode (two thirds of the output),
 * upsampled by temporal anti-aliasing; high lights; reflections. */
TB_TEST(renderer_quality, the_default_settings_upscale_from_two_thirds)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_renderer_quality quality = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    tb_renderer_quality_from_settings(&engine, &quality);
    TB_TEST_EXPECT_FLOAT_NEAR(quality.render_scale, 2.0f / 3.0f, 1e-6f);
    TB_TEST_EXPECT(quality.temporal_aa);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, 256);
    TB_TEST_EXPECT(quality.reflections);
    tb_settings_destroy(settings);
}

/* Native renders at the output's resolution, and anti-aliasing off then
 * turns temporal anti-aliasing off; upscaling turns it on whatever
 * anti-aliasing says, as the engine's upscaler is temporal; custom takes
 * graphics.render_scale; the light and reflection groups set lights and
 * reflections. */
TB_TEST(renderer_quality, settings_set_the_render_scale_lights_and_reflections)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_renderer_quality quality = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.graphics.upscaler_mode, UPSCALER_NATIVE), ==,
                         TB_SETTING_OK);
    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.graphics.anti_aliasing, ANTI_ALIASING_OFF),
                         ==, TB_SETTING_OK);
    tb_renderer_quality_from_settings(&engine, &quality);
    TB_TEST_EXPECT_FLOAT(quality.render_scale, ==, 1.0f);
    TB_TEST_EXPECT(!quality.temporal_aa);

    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.graphics.upscaler_mode, UPSCALER_PERFORMANCE),
                         ==, TB_SETTING_OK);
    tb_renderer_quality_from_settings(&engine, &quality);
    TB_TEST_EXPECT_FLOAT(quality.render_scale, ==, 0.5f);
    TB_TEST_EXPECT(quality.temporal_aa);

    TB_TEST_REQUIRE_UINT(tb_setting_set_choice(engine.graphics.upscaler_mode, UPSCALER_CUSTOM), ==,
                         TB_SETTING_OK);
    TB_TEST_REQUIRE_UINT(tb_setting_set_number(engine.graphics.render_scale, 0.8), ==,
                         TB_SETTING_OK);
    tb_engine_settings_set_quality(&engine, TB_GRAPHICS_QUALITY_LOW);
    tb_renderer_quality_from_settings(&engine, &quality);
    TB_TEST_EXPECT_FLOAT_NEAR(quality.render_scale, 0.8f, 1e-6f);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, 16);
    TB_TEST_EXPECT(!quality.reflections);
    TB_TEST_REQUIRE_UINT(
        tb_setting_set_choice(engine.graphics.quality[TB_QUALITY_GROUP_LIGHTS], QUALITY_ULTRA), ==,
        TB_SETTING_OK);
    tb_renderer_quality_from_settings(&engine, &quality);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, 1024);
    TB_TEST_EXPECT_UINT(QUALITY_LOW, ==, (unsigned)TB_GRAPHICS_QUALITY_LOW);
    tb_settings_destroy(settings);
}

typedef struct screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} screen;

/* A window and a renderer on the null backend, or a skipped case. */
static void open_screen(screen *opened, tb_renderer_profile profile)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};

    *opened = (screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(opened->system,
                         &(tb_window_options){
                             .title = "quality", .width = 200, .height = 100, .fixed_size = true},
                         &opened->window, NULL),
        ==, TB_WINDOW_OK);
    options.window = opened->window;
    options.device.backend = TB_RHI_BACKEND_NULL;
    options.profile = profile;
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
}

static void close_screen(screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

static void draw_frame(screen *opened)
{
    const tb_camera camera = tb_camera_perspective(1.0f);

    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
}

/* The null backend has what the full profile needs: the quality asked for
 * is the quality drawn with, and the scene renders at its scale of the
 * output from the next frame, through the temporal pass. */
TB_TEST(renderer_quality, the_full_profile_takes_the_quality_asked_for)
{
    screen opened = {0};
    tb_renderer_capabilities capabilities = {0};
    tb_renderer_quality quality = TB_RENDERER_QUALITY_INIT;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t output_width = 0;
    uint32_t output_height = 0;

    open_screen(&opened, TB_RENDERER_PROFILE_AUTOMATIC);
    tb_renderer_capabilities_of(opened.renderer, &capabilities);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_FULL);
    TB_TEST_EXPECT(capabilities.raster_lighting);
    TB_TEST_EXPECT(capabilities.reason[0] == '\0');
    quality.render_scale = 0.5f;
    quality.max_lights = 8;
    tb_renderer_set_quality(opened.renderer, &quality);
    quality = (tb_renderer_quality){0};
    tb_renderer_quality_of(opened.renderer, &quality);
    TB_TEST_EXPECT_FLOAT(quality.render_scale, ==, 0.5f);
    TB_TEST_EXPECT(quality.temporal_aa);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, 8);
    for (int frame = 0; frame < 3; frame++)
    {
        draw_frame(&opened);
    }
    tb_renderer_camera_cut(opened.renderer);
    draw_frame(&opened);
    tb_renderer_size(opened.renderer, &output_width, &output_height);
    tb_renderer_render_size(opened.renderer, &width, &height);
    TB_TEST_EXPECT_UINT(width, ==, (output_width + 1) / 2);
    TB_TEST_EXPECT_UINT(height, ==, (output_height + 1) / 2);
    close_screen(&opened);
}

/* The low profile, asked for, caps the quality: half the output's
 * resolution, no temporal anti-aliasing, 16 lights, no reflections; and
 * says why. */
TB_TEST(renderer_quality, the_low_profile_caps_the_quality)
{
    screen opened = {0};
    tb_renderer_capabilities capabilities = {0};
    tb_renderer_quality quality = TB_RENDERER_QUALITY_INIT;

    open_screen(&opened, TB_RENDERER_PROFILE_LOW);
    tb_renderer_capabilities_of(opened.renderer, &capabilities);
    TB_TEST_EXPECT_UINT(capabilities.profile, ==, TB_RENDERER_PROFILE_LOW);
    TB_TEST_EXPECT(capabilities.reason[0] != '\0');
    tb_renderer_set_quality(opened.renderer, &quality);
    tb_renderer_quality_of(opened.renderer, &quality);
    TB_TEST_EXPECT_FLOAT(quality.render_scale, ==, TB_RENDERER_LOW_RENDER_SCALE);
    TB_TEST_EXPECT(!quality.temporal_aa);
    TB_TEST_EXPECT_UINT(quality.max_lights, ==, TB_RENDERER_LOW_LIGHTS);
    TB_TEST_EXPECT(!quality.reflections);
    draw_frame(&opened);
    close_screen(&opened);
}
