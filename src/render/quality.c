/* What the renderer's frames are made with: the profile a device allows,
 * the quality asked for within it, the render size that follows, and the
 * quality the engine's settings describe. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/engine_settings.h"
#include "badger/fmt.h"

#include <math.h>

/* The choices of the settings the quality reads, in the order
 * engine_settings.c declares them. */
enum
{
    TB_QUALITY_UPSCALER_MODE_NATIVE,
    TB_QUALITY_UPSCALER_MODE_QUALITY,
    TB_QUALITY_UPSCALER_MODE_BALANCED,
    TB_QUALITY_UPSCALER_MODE_PERFORMANCE,
    TB_QUALITY_UPSCALER_MODE_ULTRA_PERFORMANCE,
    TB_QUALITY_UPSCALER_MODE_CUSTOM
};
enum
{
    TB_QUALITY_ANTI_ALIASING_OFF,
    TB_QUALITY_ANTI_ALIASING_FXAA,
    TB_QUALITY_ANTI_ALIASING_TAA
};

/* The render scale of each upscaler mode before custom, as the vendors
 * name them: DLSS's and FSR's quality, balanced, performance and ultra
 * performance. */
static const float tb_quality_mode_scales[] = {1.0f, 2.0f / 3.0f, 0.58f, 0.5f, 1.0f / 3.0f};
/* The lights of each level of `graphics.light_quality`, low to ultra. */
#define TB_QUALITY_LEVELS 4u
static const uint32_t tb_quality_lights[TB_QUALITY_LEVELS] = {16u, 64u, 256u,
                                                              TB_RENDERER_MAX_LIGHTS};

tb_renderer_capabilities tb_renderer_capabilities_for(const tb_rhi_device_info *info,
                                                      tb_renderer_profile profile)
{
    tb_renderer_capabilities capabilities = {0};
    const bool bindless = (info->features & TB_RHI_FEATURE_BINDLESS) != 0;

    capabilities.profile = TB_RENDERER_PROFILE_FULL;
    capabilities.raster_lighting = bindless;
    capabilities.max_render_scale = TB_RENDERER_MAX_RENDER_SCALE;
    capabilities.temporal_aa = true;
    capabilities.max_lights = TB_RENDERER_MAX_LIGHTS;
    capabilities.reflections = true;
    if (!bindless)
    {
        /* Without the raster lighting mode, meshes are drawn straight into
         * the swap chain, at its size, lit by one light. */
        capabilities.profile = TB_RENDERER_PROFILE_LOW;
        capabilities.max_render_scale = 1.0f;
        capabilities.temporal_aa = false;
        capabilities.max_lights = 1;
        capabilities.reflections = false;
        (void)tb_fmt_buffer(capabilities.reason, sizeof(capabilities.reason),
                            "%s has no bindless resources, which the raster lighting mode needs",
                            info->adapter);
        return capabilities;
    }
    if (profile == TB_RENDERER_PROFILE_FULL ||
        (profile == TB_RENDERER_PROFILE_AUTOMATIC && !info->software))
    {
        return capabilities;
    }
    capabilities.profile = TB_RENDERER_PROFILE_LOW;
    capabilities.max_render_scale = TB_RENDERER_LOW_RENDER_SCALE;
    capabilities.temporal_aa = false;
    capabilities.max_lights = TB_RENDERER_LOW_LIGHTS;
    capabilities.reflections = false;
    if (profile == TB_RENDERER_PROFILE_LOW)
    {
        (void)tb_fmt_buffer(capabilities.reason, sizeof(capabilities.reason),
                            "the low profile was asked for");
        return capabilities;
    }
    (void)tb_fmt_buffer(capabilities.reason, sizeof(capabilities.reason), "%s runs on the CPU",
                        info->adapter);
    return capabilities;
}

tb_renderer_quality tb_renderer_quality_within(const tb_renderer_quality *asked,
                                               const tb_renderer_capabilities *capabilities)
{
    tb_renderer_quality quality = *asked;

    quality.render_scale = fminf(quality.render_scale, capabilities->max_render_scale);
    quality.temporal_aa = quality.temporal_aa && capabilities->temporal_aa;
    quality.max_lights = quality.max_lights < capabilities->max_lights ? quality.max_lights
                                                                       : capabilities->max_lights;
    quality.reflections = quality.reflections && capabilities->reflections;
    return quality;
}

void tb_render_size_of(uint32_t width, uint32_t height, float scale, uint32_t largest,
                       uint32_t *render_width, uint32_t *render_height)
{
    const float across = roundf((float)width * scale);
    const float down = roundf((float)height * scale);

    *render_width = across < 1.0f ? 1u : (uint32_t)fminf(across, (float)largest);
    *render_height = down < 1.0f ? 1u : (uint32_t)fminf(down, (float)largest);
}

static void tb_quality_check(const tb_renderer_quality *quality)
{
    TB_ASSERT(quality != NULL);
    TB_ASSERT_MSG(quality->render_scale >= TB_RENDERER_MIN_RENDER_SCALE &&
                      quality->render_scale <= TB_RENDERER_MAX_RENDER_SCALE,
                  "a render scale from 0.25 to 2");
    TB_ASSERT_MSG(quality->max_lights >= 1, "at least one light");
}

void tb_renderer_set_quality(tb_renderer *renderer, const tb_renderer_quality *quality)
{
    TB_ASSERT(renderer != NULL);
    tb_quality_check(quality);
    renderer->quality_asked = *quality;
}

void tb_renderer_quality_of(const tb_renderer *renderer, tb_renderer_quality *quality)
{
    TB_ASSERT(renderer != NULL && quality != NULL);
    *quality = tb_renderer_quality_within(&renderer->quality_asked, &renderer->capabilities);
}

void tb_renderer_capabilities_of(const tb_renderer *renderer,
                                 tb_renderer_capabilities *capabilities)
{
    TB_ASSERT(renderer != NULL && capabilities != NULL);
    *capabilities = renderer->capabilities;
}

void tb_renderer_render_size(const tb_renderer *renderer, uint32_t *width, uint32_t *height)
{
    TB_ASSERT(renderer != NULL && width != NULL && height != NULL);
    *width = renderer->render_width;
    *height = renderer->render_height;
}

void tb_renderer_quality_begin_frame(tb_renderer *renderer)
{
    const bool was_temporal = renderer->quality.temporal_aa;

    renderer->quality =
        tb_renderer_quality_within(&renderer->quality_asked, &renderer->capabilities);
    tb_render_size_of(renderer->width, renderer->height, renderer->quality.render_scale,
                      renderer->info.max_texture_size, &renderer->render_width,
                      &renderer->render_height);
    if (!renderer->deferred)
    {
        renderer->render_width = renderer->width;
        renderer->render_height = renderer->height;
    }
    /* A history of another size, of frames drawn another way, or seen
     * through another kind of projection does not follow from this frame. */
    if (!was_temporal || renderer->render_width != renderer->previous_render_width ||
        renderer->render_height != renderer->previous_render_height ||
        renderer->camera.projection != renderer->previous_projection)
    {
        renderer->history_valid = false;
    }
}

void tb_renderer_quality_from_settings(const tb_engine_settings *engine,
                                       tb_renderer_quality *quality)
{
    size_t mode = 0;
    size_t lights = 0;
    size_t reflections = 0;
    bool taa = false;

    TB_ASSERT(engine != NULL && quality != NULL);
    mode = tb_setting_choice(engine->graphics.upscaler_mode);
    lights = tb_setting_choice(engine->graphics.quality[TB_QUALITY_GROUP_LIGHTS]);
    reflections = tb_setting_choice(engine->graphics.quality[TB_QUALITY_GROUP_REFLECTIONS]);
    taa = tb_setting_choice(engine->graphics.anti_aliasing) == TB_QUALITY_ANTI_ALIASING_TAA;
    *quality = (tb_renderer_quality)TB_RENDERER_QUALITY_INIT;
    quality->render_scale = mode < TB_QUALITY_UPSCALER_MODE_CUSTOM
                                ? tb_quality_mode_scales[mode]
                                : (float)tb_setting_number(engine->graphics.render_scale);
    /* The engine's own upscaler is its temporal anti-aliasing, as Unreal's
     * TAAU is; `graphics.anti_aliasing` says how edges are smoothed when
     * nothing upscales. Vendors' upscalers are not plugins yet: the
     * engine's stands in for them. */
    quality->temporal_aa = quality->render_scale < 1.0f || taa;
    quality->max_lights =
        lights < TB_QUALITY_LEVELS ? tb_quality_lights[lights] : TB_RENDERER_MAX_LIGHTS;
    quality->reflections = reflections != (size_t)TB_GRAPHICS_QUALITY_LOW;
}
