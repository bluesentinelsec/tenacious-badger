/* Temporal anti-aliasing, and the render scale it upsamples from: each
 * frame's projection is jittered by a fraction of a pixel along Halton's
 * sequence, and a full-screen pass at the viewport's size blends the frame
 * with the last, reprojected by the camera's motion and clamped to the
 * frame's neighbourhood, into a history that is the next frame's last. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"

#include <math.h>

/* The jitter's positions before they repeat, as Unreal's default. */
#define TB_TEMPORAL_SAMPLES 8u
/* How much of each frame's own color the history takes. */
#define TB_TEMPORAL_CURRENT_WEIGHT 0.1f
#define TB_TEMPORAL_HISTORY_FORMAT TB_RHI_FORMAT_R16G16B16A16_FLOAT

/* What the temporal pass reads, as its TemporalConstants lays it out. */
typedef struct tb_temporal_constants
{
    tb_mat4 clip_to_previous; /* this frame's unjittered clip to the last frame's */
    tb_vec4 jitter;           /* in the render's pixels, x right and y down */
    tb_vec4 render_size;      /* the render's pixels, and one over each */
    tb_vec4 output_size;      /* the output's pixels, and one over each */
    /* the current frame's weight, 1 when there is a history, and the
     * output's pixels in one of the render's */
    tb_vec4 blend;
} tb_temporal_constants;

TB_STATIC_ASSERT(sizeof(tb_temporal_constants) <= 256u, "the temporal constants in 256 bytes");

/* What the temporal pass reads by bindless index, as push constants. */
typedef struct tb_temporal_indices
{
    uint32_t scene;
    uint32_t depth;
    uint32_t history;
    uint32_t sampler;
} tb_temporal_indices;

/* ---------------------------------------------------------------------------
 * The jitter.
 * ------------------------------------------------------------------------- */

float tb_halton(uint32_t index, uint32_t base)
{
    float fraction = 1.0f;
    float result = 0.0f;

    TB_ASSERT(base >= 2);
    while (index > 0)
    {
        fraction /= (float)base;
        result += fraction * (float)(index % base);
        index /= base;
    }
    return result;
}

tb_vec2 tb_temporal_jitter(uint32_t frame)
{
    /* From the sequence's second point: its first is 0, a pixel's corner. */
    const uint32_t index = frame % TB_TEMPORAL_SAMPLES + 1u;

    return (tb_vec2){tb_halton(index, 2) - 0.5f, tb_halton(index, 3) - 0.5f};
}

tb_mat4 tb_temporal_jittered(tb_mat4 projection, tb_vec2 jitter, uint32_t width, uint32_t height)
{
    /* Clip space spans 2 across the view, and its y runs up where pixels
     * run down. Adding an amount times w to x moves the projected point by
     * that amount whatever its depth, which a translation of clip space
     * after the projection does. */
    const tb_vec3 offset = {2.0f * jitter.x / (float)width, -2.0f * jitter.y / (float)height, 0.0f};

    return tb_mat4_mul(tb_mat4_translation(offset), projection);
}

/* ---------------------------------------------------------------------------
 * The history.
 * ------------------------------------------------------------------------- */

void tb_renderer_camera_cut(tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    renderer->history_valid = false;
}

static void tb_temporal_destroy_history(tb_renderer *renderer)
{
    for (int index = 0; index < 2; index++)
    {
        tb_rhi_texture_destroy(renderer->device, renderer->history[index]);
        renderer->history[index] = (tb_rhi_texture){0};
    }
    renderer->history_width = 0;
    renderer->history_height = 0;
}

/* Make the history textures at the viewport's size, when they are not: a
 * viewport of another size starts the history again. */
static void tb_temporal_size_history(tb_renderer *renderer)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    const uint32_t width = renderer->viewport.width;
    const uint32_t height = renderer->viewport.height;

    if (renderer->history_width == width && renderer->history_height == height)
    {
        return;
    }
    /* The frames before read the old ones, which go once the GPU is done. */
    tb_temporal_destroy_history(renderer);
    for (int index = 0; index < 2; index++)
    {
        if (tb_rhi_texture_create(renderer->device,
                                  &(tb_rhi_texture_desc){.format = TB_TEMPORAL_HISTORY_FORMAT,
                                                         .width = width,
                                                         .height = height,
                                                         .usage = TB_RHI_TEXTURE_SAMPLED |
                                                                  TB_RHI_TEXTURE_RENDER_TARGET,
                                                         .name = "temporal history"},
                                  &renderer->history[index], &error) != TB_RHI_OK)
        {
            tb_fatal("renderer: no memory for the temporal history: %s", error.message);
        }
        renderer->history_state[index] = TB_RHI_STATE_COMMON;
    }
    renderer->history_width = width;
    renderer->history_height = height;
    renderer->history_valid = false;
}

uint64_t tb_renderer_temporal_upload(tb_renderer *renderer, tb_vec2 jitter, tb_mat4 clip)
{
    tb_temporal_constants constants = {0};
    tb_mat4 to_world = {0};

    if (!renderer->quality.temporal_aa)
    {
        return UINT64_MAX;
    }
    tb_temporal_size_history(renderer);
    if (renderer->history_valid && tb_mat4_inverse(clip, &to_world))
    {
        constants.clip_to_previous = tb_mat4_mul(renderer->previous_clip, to_world);
        constants.blend.y = 1.0f;
    }
    constants.jitter = (tb_vec4){jitter.x, jitter.y, 0.0f, 0.0f};
    constants.render_size =
        (tb_vec4){(float)renderer->render_width, (float)renderer->render_height,
                  1.0f / (float)renderer->render_width, 1.0f / (float)renderer->render_height};
    constants.output_size =
        (tb_vec4){(float)renderer->viewport.width, (float)renderer->viewport.height,
                  1.0f / (float)renderer->viewport.width, 1.0f / (float)renderer->viewport.height};
    constants.blend.x = TB_TEMPORAL_CURRENT_WEIGHT;
    constants.blend.z = (float)renderer->viewport.width / (float)renderer->render_width;
    return tb_renderer_upload(renderer, &constants, sizeof(constants), TB_RHI_CONSTANT_ALIGNMENT);
}

static uint32_t tb_temporal_index(const tb_render_graph_pass_context *pass,
                                  tb_render_graph_texture texture)
{
    const tb_temporal_recording *const recording = pass->context;

    return tb_rhi_texture_index(recording->renderer->device,
                                tb_render_graph_rhi_texture(pass->graph, texture));
}

static void tb_temporal_execute(const tb_render_graph_pass_context *pass)
{
    const tb_temporal_recording *const recording = pass->context;
    tb_renderer *const renderer = recording->renderer;
    const tb_temporal_indices indices = {
        .scene = tb_temporal_index(pass, recording->scene),
        .depth = tb_temporal_index(pass, recording->depth),
        .history = tb_temporal_index(pass, recording->history),
        .sampler = tb_rhi_sampler_index(renderer->device, renderer->canvas_samplers[0])};

    tb_rhi_cmd_pipeline(pass->list, renderer->temporal_pipeline);
    tb_rhi_cmd_push_constants(pass->list, &indices, sizeof(indices));
    tb_rhi_cmd_constant_buffer(pass->list, 0, renderer->frames[renderer->slot].upload,
                               recording->constants);
    tb_rhi_cmd_draw(pass->list, 3, 1, 0, 0);
}

void tb_renderer_temporal_record(tb_renderer *renderer, tb_temporal_recording *recording)
{
    const uint32_t written = renderer->history_written;
    const uint32_t read = 1u - written;
    tb_render_graph_pass pass = {
        .name = "temporal anti-aliasing", .execute = tb_temporal_execute, .context = recording};

    recording->renderer = renderer;
    recording->history = tb_render_graph_import_texture(renderer->graph, renderer->history[read],
                                                        renderer->history_state[read],
                                                        TB_RHI_STATE_SHADER_READ, "history");
    recording->output = tb_render_graph_import_texture(renderer->graph, renderer->history[written],
                                                       renderer->history_state[written],
                                                       TB_RHI_STATE_SHADER_READ, "history written");
    pass.accesses[0] =
        (tb_render_graph_access){.texture = recording->scene, .state = TB_RHI_STATE_SHADER_READ};
    pass.accesses[1] =
        (tb_render_graph_access){.texture = recording->depth, .state = TB_RHI_STATE_SHADER_READ};
    pass.accesses[2] =
        (tb_render_graph_access){.texture = recording->history, .state = TB_RHI_STATE_SHADER_READ};
    pass.access_count = 3;
    pass.colors[0] = (tb_render_graph_color){.texture = recording->output, .load = TB_RHI_LOAD_ANY};
    pass.color_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
}

void tb_renderer_temporal_end_frame(tb_renderer *renderer, tb_mat4 clip)
{
    renderer->previous_clip = clip;
    renderer->previous_projection = renderer->camera.projection;
    renderer->previous_render_width = renderer->render_width;
    renderer->previous_render_height = renderer->render_height;
    if (!renderer->quality.temporal_aa || renderer->history[0].generation == 0)
    {
        return;
    }
    /* Both histories were imported and left to be read. */
    renderer->history_state[0] = TB_RHI_STATE_SHADER_READ;
    renderer->history_state[1] = TB_RHI_STATE_SHADER_READ;
    renderer->history_written = 1u - renderer->history_written;
    renderer->history_valid = true;
}

/* ---------------------------------------------------------------------------
 * The renderer's part.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_renderer_temporal_create(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_graphics_pipeline_desc desc = {0};

    if (!renderer->deferred)
    {
        return TB_RHI_OK;
    }
    desc.colors[0] = TB_TEMPORAL_HISTORY_FORMAT;
    desc.color_count = 1;
    desc.name = "temporal anti-aliasing";
    return tb_renderer_pipeline(renderer, "/Engine/Private/TemporalAA.hlsl", NULL, &desc,
                                &renderer->temporal_pipeline, error);
}

void tb_renderer_temporal_destroy(tb_renderer *renderer)
{
    tb_temporal_destroy_history(renderer);
    tb_rhi_pipeline_destroy(renderer->device, renderer->temporal_pipeline);
}
