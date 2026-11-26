/* The viewport render mode's upscale: the viewport's target, into which the
 * 3D scene and the design's canvas were drawn at a pixel a unit, drawn into
 * the viewport's rectangle of the swap chain, nearest-neighbour at whole
 * pixels a unit and linear otherwise, with the bars cleared around it. */
#include "renderer_internal.h"

#include "badger/assert.h"

#include <math.h>
#include <string.h>

/* What the upscale shader reads, as its UpscaleConstants lays it out. */
typedef struct tb_upscale_constants
{
    uint32_t source;  /* the target's bindless index */
    uint32_t sampler; /* its sampler's */
    float origin[2];  /* the viewport's top left in the swap chain's pixels */
    float inverse[2]; /* one over the viewport's width and height */
    float extent[2];  /* the units shown, as a fraction of the target's pixels */
    float low[2];     /* the least texture coordinates read: half a texel in */
    float high[2];    /* the most: half a texel inside the units shown */
} tb_upscale_constants;

TB_STATIC_ASSERT(sizeof(tb_upscale_constants) <= TB_RHI_PUSH_CONSTANT_BYTES,
                 "the upscale's constants as push constants");

/* A whole number of pixels a unit, at least 1. */
static bool tb_upscale_is_whole(float value)
{
    return value >= 1.0f && floorf(value) == value;
}

bool tb_upscale_is_nearest(const tb_viewport_policy *policy, const tb_viewport *viewport)
{
    TB_ASSERT(policy != NULL && viewport != NULL);
    return policy->scale_mode == TB_VIEWPORT_SCALE_INTEGER &&
           tb_upscale_is_whole(viewport->scale.x) && tb_upscale_is_whole(viewport->scale.y);
}

/* The texture coordinates of an axis of the target: the units shown, of
 * `pixels` of the target, and the least and most read. */
static void tb_upscale_axis(float units, uint32_t pixels, float *extent, float *low, float *high)
{
    const float side = (float)pixels;
    const float shown = fminf(units, side);

    *extent = units / side;
    *low = 0.5f / side;
    *high = fmaxf(shown - 0.5f, 0.5f) / side;
}

static void tb_upscale_execute(const tb_render_graph_pass_context *pass)
{
    const tb_upscale_recording *const recording = pass->context;
    tb_renderer *const renderer = recording->renderer;
    const tb_viewport *const viewport = &renderer->viewport;
    const bool nearest = tb_upscale_is_nearest(&renderer->viewport_policy, viewport);
    tb_upscale_constants constants = {0};

    constants.source = tb_rhi_texture_index(
        renderer->device, tb_render_graph_rhi_texture(pass->graph, recording->target));
    constants.sampler =
        tb_rhi_sampler_index(renderer->device, renderer->canvas_samplers[nearest ? 1 : 0]);
    constants.origin[0] = (float)viewport->x;
    constants.origin[1] = (float)viewport->y;
    constants.inverse[0] = 1.0f / (float)viewport->width;
    constants.inverse[1] = 1.0f / (float)viewport->height;
    tb_upscale_axis(viewport->size.x, viewport->target_width, &constants.extent[0],
                    &constants.low[0], &constants.high[0]);
    tb_upscale_axis(viewport->size.y, viewport->target_height, &constants.extent[1],
                    &constants.low[1], &constants.high[1]);
    tb_rhi_cmd_viewport(pass->list, (float)viewport->x, (float)viewport->y, (float)viewport->width,
                        (float)viewport->height, 0.0f, 1.0f);
    tb_rhi_cmd_scissor(pass->list, (tb_rhi_rect){(int32_t)viewport->x, (int32_t)viewport->y,
                                                 viewport->width, viewport->height});
    tb_rhi_cmd_pipeline(pass->list, renderer->upscale_pipeline);
    tb_rhi_cmd_push_constants(pass->list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(pass->list, 3, 1, 0, 0);
}

void tb_renderer_upscale_record(tb_renderer *renderer, tb_upscale_recording *recording,
                                tb_render_graph_texture target, tb_render_graph_texture image)
{
    tb_render_graph_pass pass = {
        .name = "viewport upscale", .execute = tb_upscale_execute, .context = recording};

    *recording = (tb_upscale_recording){.renderer = renderer, .target = target};
    pass.accesses[0] =
        (tb_render_graph_access){.texture = target, .state = TB_RHI_STATE_SHADER_READ};
    pass.access_count = 1;
    /* The bars around the viewport are cleared to the policy's color. */
    pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_CLEAR};
    memcpy(pass.colors[0].clear, &renderer->viewport_policy.bar_color,
           sizeof(pass.colors[0].clear));
    pass.color_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
}

tb_rhi_result tb_renderer_upscale_create(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_graphics_pipeline_desc desc = {0};

    if (!renderer->deferred)
    {
        return TB_RHI_OK;
    }
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.color_count = 1;
    desc.name = "viewport upscale";
    return tb_renderer_pipeline(renderer, "/Engine/Private/Upscale.hlsl", NULL, &desc,
                                &renderer->upscale_pipeline, error);
}

void tb_renderer_upscale_destroy(tb_renderer *renderer)
{
    tb_rhi_pipeline_destroy(renderer->device, renderer->upscale_pipeline);
}
