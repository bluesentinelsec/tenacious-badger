/* The renderer: a device and swap chain on a window, the shaders it draws
 * with, frames in flight, and each frame's meshes and debug lines recorded
 * through the render graph: into a G-buffer, lit, and tone mapped, in the
 * raster lighting mode; or, on a device without bindless resources, lit by
 * one light straight into the swap chain. */
#include "renderer_internal.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/dxc.h"
#include "badger/fatal.h"
#include "badger/fmt.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/shader_library.h"
#include "badger/str.h"
#include "badger/window.h"

#include <stdarg.h>
#include <string.h>

/* Bytes each frame in flight may write for its draws' constants and lines. */
#define TB_RENDERER_UPLOAD_BYTES ((uint64_t)16 * 1024 * 1024)
/* Bytes of one draw's constants, at the alignment constant buffers need. */
#define TB_RENDERER_DRAW_BYTES 256u

/* What a line pipeline reads, as the line shaders' LineConstants lay it
 * out: the frame's clip transform and, with bindless resources, how to test
 * against the scene's depth. */
typedef struct tb_renderer_line_constants
{
    tb_mat4 clip;
    uint32_t depth; /* the scene depth's bindless index */
    uint32_t test;  /* 1 to test against it */
    float scale[2]; /* from the output's pixels to the depth's */
} tb_renderer_line_constants;

static tb_rhi_result tb_renderer_fail(tb_rhi_error *error, tb_rhi_result result, const char *format,
                                      ...)
{
    va_list arguments;

    if (error != NULL)
    {
        error->result = result;
        va_start(arguments, format);
        (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
        va_end(arguments);
    }
    return result;
}

/* ---------------------------------------------------------------------------
 * Shaders and pipelines.
 * ------------------------------------------------------------------------- */

static tb_shader_stage tb_renderer_stage(tb_rhi_stage stage)
{
    switch (stage)
    {
    case TB_RHI_STAGE_VERTEX:
        return TB_SHADER_STAGE_VERTEX;
    case TB_RHI_STAGE_PIXEL:
        return TB_SHADER_STAGE_PIXEL;
    case TB_RHI_STAGE_COMPUTE:
        break;
    }
    return TB_SHADER_STAGE_COMPUTE;
}

/* A shader of the renderer's own, for the device's backend: DXIL for
 * D3D12, from the library, the cache, or DXC; for the null backend, which
 * runs nothing, the checked source itself. */
tb_rhi_result tb_renderer_shader(tb_renderer *renderer, const char *path, const char *source,
                                 const char *entry, tb_rhi_stage stage, tb_rhi_shader *shader,
                                 tb_rhi_error *error)
{
    const tb_shader_loader engine = tb_shader_engine_loader();
    tb_shader_file file = {path, source, 0};
    tb_shader_compile_request request = {0};
    tb_shader_compiled compiled = {0};
    tb_shader_lookup lookup = {0};
    tb_shader_key key = {0};
    tb_shader_origin origin = TB_SHADER_ORIGIN_COMPILE;
    tb_arena *const arena = tb_arena_create(0);
    tb_rhi_shader_desc desc = {.stage = stage, .entry = entry, .name = path};
    tb_rhi_result result = TB_RHI_OK;

    request.source.path = path;
    request.source.loader = &engine;
    if (source != NULL)
    {
        file.length = tb_str_length(source);
        request.source.files = &file;
        request.source.file_count = 1;
    }
    request.entry_point = entry;
    request.stage = tb_renderer_stage(stage);
    request.target = renderer->info.backend == TB_RHI_BACKEND_D3D12 ? TB_SHADER_TARGET_DXIL
                                                                    : TB_SHADER_TARGET_NONE;
    request.dxc = renderer->dxc;
    lookup.library = renderer->shader_library;
    lookup.cache = renderer->shader_cache;
    lookup.library_only = renderer->shader_library_only;
    if (tb_shader_compile_cached(&request, &lookup, arena, &compiled, &key, &origin) !=
        TB_SHADER_OK)
    {
        char line[TB_RHI_MESSAGE_CAPACITY] = {0};
        if (compiled.diagnostic_count > 0)
        {
            (void)tb_shader_diagnostic_format(&compiled.diagnostics[0], line, sizeof(line));
        }
        result =
            tb_renderer_fail(error, TB_RHI_SYSTEM_ERROR, "renderer: %s %s: %s", path, entry, line);
        goto done;
    }
    desc.code = compiled.code;
    desc.size = compiled.code_size;
    if (compiled.code == NULL)
    {
        /* The null backend keeps the source, which it runs no more than code. */
        desc.code = source != NULL ? source : path;
        desc.size = tb_str_length(desc.code);
    }
    result = tb_rhi_shader_create(renderer->device, &desc, shader, error);
done:
    tb_arena_destroy(arena);
    return result;
}

tb_rhi_result tb_renderer_pipeline(tb_renderer *renderer, const char *path, const char *source,
                                   tb_rhi_graphics_pipeline_desc *desc, tb_rhi_pipeline *pipeline,
                                   tb_rhi_error *error)
{
    tb_rhi_result result = tb_renderer_shader(renderer, path, source, "MainVS", TB_RHI_STAGE_VERTEX,
                                              &desc->vertex, error);

    if (result == TB_RHI_OK)
    {
        result = tb_renderer_shader(renderer, path, source, "MainPS", TB_RHI_STAGE_PIXEL,
                                    &desc->pixel, error);
    }
    if (result == TB_RHI_OK)
    {
        result = tb_rhi_graphics_pipeline_create(renderer->device, desc, pipeline, error);
    }
    tb_rhi_shader_destroy(renderer->device, desc->vertex);
    tb_rhi_shader_destroy(renderer->device, desc->pixel);
    return result;
}

static const tb_rhi_vertex_attribute tb_renderer_line_attributes[2] = {
    {.semantic = "POSITION", .format = TB_RHI_FORMAT_R32G32B32_FLOAT},
    {.semantic = "COLOR", .format = TB_RHI_FORMAT_R32G32B32A32_FLOAT, .offset = 12}};

/* coverage-off: only a device without bindless resources draws this way, and no
 * backend that tests run on lacks them */
/* The pipelines of a device without bindless resources: meshes lit by one
 * light, and lines tested by depth or on top, all into the swap chain. */
static tb_rhi_result tb_renderer_forward_pipelines(tb_renderer *renderer, tb_rhi_error *error)
{
    const tb_rhi_vertex_attribute mesh_attributes[3] = {
        {.semantic = "POSITION", .format = TB_RHI_FORMAT_R32G32B32_FLOAT},
        {.semantic = "NORMAL", .format = TB_RHI_FORMAT_R32G32B32_FLOAT, .offset = 12},
        {.semantic = "TEXCOORD", .format = TB_RHI_FORMAT_R32G32_FLOAT, .offset = 24}};
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_result result = TB_RHI_OK;

    desc.attributes = mesh_attributes;
    desc.attribute_count = 3;
    desc.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = sizeof(tb_mesh_vertex)};
    desc.buffer_count = 1;
    desc.cull = TB_RHI_CULL_BACK;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER;
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.color_count = 1;
    desc.depth = TB_RENDERER_DEPTH_FORMAT;
    desc.name = "mesh";
    result = tb_renderer_pipeline(renderer, "/Engine/Private/Mesh.hlsl", NULL, &desc,
                                  &renderer->mesh_pipeline, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    desc = (tb_rhi_graphics_pipeline_desc){0};
    desc.attributes = tb_renderer_line_attributes;
    desc.attribute_count = 2;
    desc.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = sizeof(tb_renderer_line_vertex)};
    desc.buffer_count = 1;
    desc.topology = TB_RHI_TOPOLOGY_LINES;
    desc.depth_test = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER_EQUAL;
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.color_count = 1;
    desc.depth = TB_RENDERER_DEPTH_FORMAT;
    desc.name = "debug lines";
    result = tb_renderer_pipeline(renderer, "/Engine/Private/DebugLines.hlsl", NULL, &desc,
                                  &renderer->line_pipeline, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    desc.depth_test = false;
    desc.depth = TB_RHI_FORMAT_UNKNOWN;
    desc.name = "debug lines on top";
    return tb_renderer_pipeline(renderer, "/Engine/Private/DebugLines.hlsl", NULL, &desc,
                                &renderer->line_top_pipeline, error);
}

/* coverage-on */

/* The line pipeline of the raster lighting mode: lines over the output,
 * tested in the shader against the scene's depth or not. */
static tb_rhi_result tb_renderer_output_line_pipeline(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_graphics_pipeline_desc desc = {0};

    desc.attributes = tb_renderer_line_attributes;
    desc.attribute_count = 2;
    desc.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = sizeof(tb_renderer_line_vertex)};
    desc.buffer_count = 1;
    desc.topology = TB_RHI_TOPOLOGY_LINES;
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.color_count = 1;
    desc.name = "debug lines";
    return tb_renderer_pipeline(renderer, "/Engine/Private/DebugLinesOutput.hlsl", NULL, &desc,
                                &renderer->line_pipeline, error);
}

static tb_rhi_result tb_renderer_pipelines(tb_renderer *renderer, tb_rhi_error *error)
{
    if (renderer->deferred)
    {
        return tb_renderer_output_line_pipeline(renderer, error);
    }
    return tb_renderer_forward_pipelines(renderer, error);
}

/* ---------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

/* Say what a low profile leaves out, and why: never fatal, as a capability
 * that is not there is not. */
static void tb_renderer_report_profile(const tb_renderer *renderer)
{
    const tb_renderer_capabilities *const capabilities = &renderer->capabilities;

    if (renderer->log == NULL || capabilities->profile != TB_RENDERER_PROFILE_LOW)
    {
        return;
    }
    (void)tb_log_printf(renderer->log, TB_LOG_INFO, "renderer",
                        "low profile, as %s: render scale at most %.2f, %s temporal "
                        "anti-aliasing, %u lights at most, %s reflections",
                        capabilities->reason, (double)capabilities->max_render_scale,
                        capabilities->temporal_aa ? "with" : "no", capabilities->max_lights,
                        capabilities->reflections ? "with" : "no");
}

/* Where the device's shaders come from: its backend's library, the cache,
 * and DXC for D3D12. Without DXC, D3D12 draws only with what the library
 * holds; with no library either, it cannot draw, which is said at once. */
static tb_rhi_result tb_renderer_shader_sources(tb_renderer *renderer,
                                                const tb_renderer_options *options,
                                                tb_rhi_error *error)
{
    char name[64] = {0};
    tb_dxc_error dxc_error = TB_DXC_ERROR_INIT;

    renderer->shader_cache = options->shader_cache;
    renderer->shader_library_only = options->shader_library_only;
    if (renderer->info.backend != TB_RHI_BACKEND_D3D12)
    {
        return TB_RHI_OK;
    }
    if (options->shader_vfs != NULL)
    {
        const size_t length = tb_fmt_buffer(name, sizeof(name), "Shaders/%s.tbsl",
                                            tb_rhi_backend_name(renderer->info.backend));
        const tb_shader_library_result loaded =
            tb_shader_library_load(options->shader_vfs, name, length, &renderer->shader_library);
        if (loaded != TB_SHADER_LIBRARY_OK && loaded != TB_SHADER_LIBRARY_NOT_FOUND &&
            renderer->log != NULL)
        {
            (void)tb_log_printf(renderer->log, TB_LOG_WARNING, "renderer",
                                "the shader library %s is %s; compiling its shaders instead", name,
                                tb_shader_library_result_name(loaded));
        }
    }
    if (renderer->shader_library_only)
    {
        return TB_RHI_OK;
    }
    if (tb_dxc_open(&renderer->dxc, &dxc_error) != TB_DXC_OK && renderer->shader_library == NULL)
    {
        return tb_renderer_fail(error, TB_RHI_UNAVAILABLE,
                                "renderer: no shader library, and no shader compiler: %s",
                                dxc_error.message);
    }
    return TB_RHI_OK;
}

tb_rhi_result tb_renderer_create(const tb_renderer_options *options, tb_renderer **made,
                                 tb_rhi_error *error)
{
    tb_renderer *renderer = NULL;
    tb_rhi_device_options device = {0};
    tb_rhi_texture_desc image = {0};
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(options != NULL && options->window != NULL && made != NULL);
    *made = NULL;
    renderer = tb_calloc(1, sizeof(*renderer));
    renderer->window = options->window;
    renderer->log = options->log;
    renderer->vsync = options->vsync;
    device = options->device;
    device.log = device.log != NULL ? device.log : options->log;
    result = tb_rhi_device_create(&device, &renderer->device, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    tb_rhi_device_info_of(renderer->device, &renderer->info);
    renderer->deferred = (renderer->info.features & TB_RHI_FEATURE_BINDLESS) != 0;
    renderer->capabilities = tb_renderer_capabilities_for(&renderer->info, options->profile);
    renderer->quality_asked = (tb_renderer_quality)TB_RENDERER_QUALITY_INIT;
    tb_renderer_report_profile(renderer);
    result = tb_renderer_shader_sources(renderer, options, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    result = tb_rhi_swap_chain_create(
        renderer->device, renderer->window,
        &(tb_rhi_swap_chain_desc){.format = TB_RENDERER_COLOR_FORMAT, .vsync = renderer->vsync},
        &renderer->chain, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    tb_rhi_texture_desc_of(renderer->device, tb_rhi_swap_chain_current(renderer->chain), &image);
    renderer->width = image.width;
    renderer->height = image.height;
    result = tb_renderer_pipelines(renderer, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    for (uint32_t slot = 0; slot < TB_RENDERER_FRAMES; slot++)
    {
        result = tb_rhi_buffer_create(renderer->device,
                                      &(tb_rhi_buffer_desc){.size = TB_RENDERER_UPLOAD_BYTES,
                                                            .usage = TB_RHI_BUFFER_CONSTANT |
                                                                     TB_RHI_BUFFER_VERTEX |
                                                                     TB_RHI_BUFFER_SHADER_READ,
                                                            .memory = TB_RHI_MEMORY_UPLOAD,
                                                            .name = "frame uploads"},
                                      &renderer->frames[slot].upload, error);
        if (result != TB_RHI_OK)
        {
            goto failed;
        }
    }
    result = tb_renderer_canvas_create(renderer, options->glyph_atlas_size, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    result = tb_renderer_materials_create(renderer, options->anisotropy, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    result = tb_renderer_sky_create(renderer, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    result = tb_renderer_skinning_create(renderer, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    result = tb_renderer_lighting_create(renderer, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    result = tb_renderer_temporal_create(renderer, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    renderer->quality =
        tb_renderer_quality_within(&renderer->quality_asked, &renderer->capabilities);
    tb_render_size_of(renderer->width, renderer->height, renderer->quality.render_scale,
                      renderer->info.max_texture_size, &renderer->render_width,
                      &renderer->render_height);
    renderer->graph = tb_render_graph_create(renderer->device);
    *made = renderer;
    return TB_RHI_OK;

failed:
    tb_renderer_destroy(renderer);
    return result;
}

void tb_renderer_destroy(tb_renderer *renderer)
{
    if (renderer == NULL)
    {
        return;
    }
    if (renderer->device != NULL)
    {
        (void)tb_rhi_wait_idle(renderer->device);
        tb_render_graph_destroy(renderer->graph);
        tb_renderer_temporal_destroy(renderer);
        tb_renderer_lighting_destroy(renderer);
        tb_renderer_skinning_destroy(renderer);
        tb_renderer_sky_destroy(renderer);
        tb_renderer_materials_destroy(renderer);
        tb_renderer_canvas_destroy(renderer);
        for (uint32_t slot = 0; slot < TB_RENDERER_FRAMES; slot++)
        {
            tb_rhi_buffer_destroy(renderer->device, renderer->frames[slot].upload);
        }
        tb_rhi_buffer_destroy(renderer->device, renderer->capture);
        tb_rhi_pipeline_destroy(renderer->device, renderer->mesh_pipeline);
        tb_rhi_pipeline_destroy(renderer->device, renderer->line_pipeline);
        tb_rhi_pipeline_destroy(renderer->device, renderer->line_top_pipeline);
    }
    tb_rhi_swap_chain_destroy(renderer->chain);
    tb_rhi_device_destroy(renderer->device);
    tb_dxc_close(renderer->dxc);
    tb_shader_library_close(renderer->shader_library);
    tb_free(renderer->draws);
    tb_free(renderer->customs);
    tb_free(renderer->lines[0].vertices);
    tb_free(renderer->lines[1].vertices);
    tb_free(renderer->captured);
    tb_free(renderer->lights);
    tb_free(renderer);
}

tb_rhi_device *tb_renderer_device(tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    return renderer->device;
}

const tb_render_graph *tb_renderer_graph(const tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    return renderer->graph;
}

void tb_renderer_size(const tb_renderer *renderer, uint32_t *width, uint32_t *height)
{
    TB_ASSERT(renderer != NULL && width != NULL && height != NULL);
    *width = renderer->width;
    *height = renderer->height;
}

void tb_renderer_set_vsync(tb_renderer *renderer, bool vsync)
{
    TB_ASSERT(renderer != NULL);
    renderer->vsync_wanted = vsync;
    renderer->vsync_changed = vsync != renderer->vsync;
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

/* Follow the window's size, and vsync as asked. */
static bool tb_renderer_follow_window(tb_renderer *renderer)
{
    tb_window_state state = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    tb_window_state_of(renderer->window, &state);
    if (state.minimized || state.pixel_width == 0 || state.pixel_height == 0)
    {
        return false;
    }
    if (state.pixel_width == renderer->width && state.pixel_height == renderer->height &&
        !renderer->vsync_changed)
    {
        return true;
    }
    renderer->vsync = renderer->vsync_changed ? renderer->vsync_wanted : renderer->vsync;
    renderer->vsync_changed = false;
    if (tb_rhi_swap_chain_resize(renderer->chain, state.pixel_width, state.pixel_height,
                                 renderer->vsync, &error) != TB_RHI_OK)
    {
        if (renderer->log != NULL)
        {
            (void)tb_log_printf(renderer->log, TB_LOG_ERROR, "renderer", "%s", error.message);
        }
        return false;
    }
    renderer->width = state.pixel_width;
    renderer->height = state.pixel_height;
    return true;
}

bool tb_renderer_begin_frame(tb_renderer *renderer, const tb_camera *camera, tb_vec4 clear)
{
    tb_renderer_frame *frame = NULL;

    TB_ASSERT(renderer != NULL && camera != NULL);
    TB_ASSERT_MSG(!renderer->in_frame, "a frame begun inside a frame");
    if (!tb_renderer_follow_window(renderer))
    {
        return false;
    }
    renderer->slot = (renderer->slot + 1) % TB_RENDERER_FRAMES;
    frame = &renderer->frames[renderer->slot];
    /* This slot's uploads were read by the frame two before; wait for it. */
    (void)tb_rhi_wait(renderer->device, frame->submission);
    frame->used = 0;
    tb_renderer_materials_begin_frame(renderer);
    tb_renderer_skinning_begin_frame(renderer);
    renderer->camera = *camera;
    renderer->clear = clear;
    tb_renderer_quality_begin_frame(renderer);
    renderer->draw_count = 0;
    renderer->custom_count = 0;
    tb_canvas_batch_begin(&renderer->canvas, renderer->width, renderer->height);
    tb_renderer_text_begin_frame(renderer);
    tb_mutex_lock(&renderer->lines_lock);
    renderer->lines[0].count = 0;
    renderer->lines[1].count = 0;
    tb_mutex_unlock(&renderer->lines_lock);
    renderer->in_frame = true;
    return true;
}

uint64_t tb_renderer_upload_room(const tb_renderer *renderer, uint64_t alignment)
{
    const tb_renderer_frame *const frame = &renderer->frames[renderer->slot];
    const uint64_t offset = (frame->used + alignment - 1) / alignment * alignment;

    return offset >= TB_RENDERER_UPLOAD_BYTES ? 0 : TB_RENDERER_UPLOAD_BYTES - offset;
}

uint64_t tb_renderer_upload(tb_renderer *renderer, const void *data, size_t size,
                            uint64_t alignment)
{
    tb_renderer_frame *const frame = &renderer->frames[renderer->slot];
    const uint64_t offset = (frame->used + alignment - 1) / alignment * alignment;

    if (offset > TB_RENDERER_UPLOAD_BYTES || size > TB_RENDERER_UPLOAD_BYTES - offset)
    {
        return UINT64_MAX;
    }
    tb_rhi_buffer_write(renderer->device, frame->upload, offset, data, size);
    frame->used = offset + size;
    return offset;
}

void tb_renderer_draw_mesh(tb_renderer *renderer, const tb_mesh *mesh, tb_mat4 transform,
                           tb_vec4 color)
{
    tb_renderer_draw_mesh_shaded(renderer, mesh, transform, color, TB_MESH_LIT);
}

void tb_renderer_add_draw(tb_renderer *renderer, const tb_renderer_draw *draw)
{
    TB_ASSERT(renderer != NULL && draw != NULL && draw->mesh != NULL);
    TB_ASSERT_MSG(renderer->in_frame, "a mesh drawn outside a frame");
    if (renderer->draw_count == renderer->draw_capacity)
    {
        renderer->draw_capacity = renderer->draw_capacity == 0 ? 256 : renderer->draw_capacity * 2;
        renderer->draws =
            tb_realloc_array(renderer->draws, renderer->draw_capacity, sizeof(*renderer->draws));
    }
    renderer->draws[renderer->draw_count] = *draw;
    renderer->draw_count++;
}

void tb_renderer_draw_mesh_shaded(tb_renderer *renderer, const tb_mesh *mesh, tb_mat4 transform,
                                  tb_vec4 color, tb_mesh_shading shading)
{
    TB_ASSERT(renderer != NULL && mesh != NULL);
    tb_renderer_add_draw(
        renderer, &(tb_renderer_draw){
                      .mesh = mesh, .transform = transform, .color = color, .shading = shading});
}

void tb_renderer_draw_custom(tb_renderer *renderer, tb_renderer_draw_function *draw, void *context)
{
    TB_ASSERT(renderer != NULL && draw != NULL);
    TB_ASSERT_MSG(renderer->in_frame, "a custom draw outside a frame");
    if (renderer->custom_count == renderer->custom_capacity)
    {
        renderer->custom_capacity =
            renderer->custom_capacity == 0 ? 16 : renderer->custom_capacity * 2;
        renderer->customs = tb_realloc_array(renderer->customs, renderer->custom_capacity,
                                             sizeof(*renderer->customs));
    }
    renderer->customs[renderer->custom_count] = (tb_renderer_custom){draw, context};
    renderer->custom_count++;
}

void tb_renderer_target_formats(const tb_renderer *renderer, tb_rhi_format *color,
                                tb_rhi_format *depth)
{
    TB_ASSERT(renderer != NULL && color != NULL && depth != NULL);
    *color = renderer->deferred ? TB_RENDERER_SCENE_FORMAT : TB_RENDERER_COLOR_FORMAT;
    *depth = TB_RENDERER_DEPTH_FORMAT;
}

/* The constants of the frame's draws, and its lines, into its uploads, and
 * the graph's resources its passes share. */
typedef struct tb_renderer_recording
{
    tb_renderer *renderer;
    tb_mat4 view;
    tb_mat4 projection;      /* jittered, as the scene is drawn */
    tb_mat4 clip;            /* `projection * view` */
    tb_mat4 unjittered_clip; /* what the lines and temporal anti-aliasing see through */
    uint64_t sky;            /* the sky's constants in the frame's uploads; UINT64_MAX for none */
    tb_temporal_recording temporal; /* its constants are UINT64_MAX without it */
    uint64_t lighting;              /* the lighting pass's; UINT64_MAX when they did not fit */
    uint64_t lines_offset[2];
    tb_render_graph_buffer skinned; /* the frame's skinned vertices; none when none */
    tb_render_graph_texture gbuffer[TB_RENDERER_GBUFFER_TARGETS];
    tb_render_graph_texture depth;
    tb_render_graph_texture scene; /* the scene's light, before tone mapping */
} tb_renderer_recording;

/* coverage-off: only a device without bindless resources draws this way, and no
 * backend that tests run on lacks them */
/* A draw's constants for the pipeline of a device without bindless
 * resources: its color, lit by the frame's default light or flat. */
static uint64_t tb_renderer_forward_upload(tb_renderer *renderer, const tb_renderer_draw *draw,
                                           tb_mat4 clip, tb_vec3 light)
{
    tb_renderer_draw_constants constants = {0};

    constants.clip = clip;
    constants.model = draw->transform;
    constants.color = draw->color;
    constants.light =
        (tb_vec4){light.x, light.y, light.z, draw->shading == TB_MESH_LIT ? 1.0f : 0.0f};
    return tb_renderer_upload(renderer, &constants, sizeof(constants), TB_RENDERER_DRAW_BYTES);
}

/* coverage-on */

static void tb_renderer_upload_frame(tb_renderer *renderer, tb_renderer_recording *recording)
{
    const float aspect = (float)renderer->width / (float)renderer->height;
    const tb_mat4 projection = tb_camera_projection_of(&renderer->camera, aspect);
    const tb_light light = tb_renderer_default_light(renderer);
    tb_vec2 jitter = {0};

    if (renderer->quality.temporal_aa)
    {
        jitter = tb_temporal_jitter(renderer->jitter_frame);
        renderer->jitter_frame++;
    }
    recording->renderer = renderer;
    recording->view = tb_camera_view(&renderer->camera);
    recording->unjittered_clip = tb_mat4_mul(projection, recording->view);
    recording->projection =
        tb_temporal_jittered(projection, jitter, renderer->render_width, renderer->render_height);
    recording->clip = tb_mat4_mul(recording->projection, recording->view);
    recording->lighting = UINT64_MAX;
    recording->temporal.constants =
        renderer->deferred
            ? tb_renderer_temporal_upload(renderer, jitter, recording->unjittered_clip)
            : UINT64_MAX;
    for (size_t index = 0; index < renderer->draw_count; index++)
    {
        tb_renderer_draw *const draw = &renderer->draws[index];
        const tb_mat4 clip = tb_mat4_mul(recording->clip, draw->transform);
        if (renderer->deferred)
        {
            draw->constants = tb_renderer_material_upload(renderer, draw, clip);
            continue;
        }
        draw->constants = tb_renderer_forward_upload(renderer, draw, clip, light.direction);
    }
    recording->sky = tb_renderer_sky_upload(renderer, aspect);
    if (renderer->deferred)
    {
        recording->lighting = tb_renderer_lighting_upload(renderer, recording->clip);
    }
    tb_mutex_lock(&renderer->lines_lock);
    for (int layer = 0; layer < 2; layer++)
    {
        recording->lines_offset[layer] =
            renderer->lines[layer].count == 0
                ? UINT64_MAX
                : tb_renderer_upload(renderer, renderer->lines[layer].vertices,
                                     renderer->lines[layer].count * sizeof(tb_renderer_line_vertex),
                                     16);
    }
    tb_mutex_unlock(&renderer->lines_lock);
}

/* Draw a layer of lines: 0 tested against depth, 1 on top. In the raster
 * lighting mode, depth is the scene's, read by the shader. */
static void tb_renderer_record_lines(tb_renderer_recording *recording,
                                     const tb_render_graph_pass_context *pass, int layer)
{
    tb_renderer *const renderer = recording->renderer;
    const tb_renderer_frame *const frame = &renderer->frames[renderer->slot];
    tb_renderer_line_constants constants = {
        .clip = recording->unjittered_clip,
        .scale = {(float)renderer->render_width / (float)renderer->width,
                  (float)renderer->render_height / (float)renderer->height}};
    tb_rhi_pipeline pipeline = renderer->line_pipeline;

    if (recording->lines_offset[layer] == UINT64_MAX)
    {
        return;
    }
    if (renderer->deferred)
    {
        constants.test = layer == 0 ? 1u : 0u;
        constants.depth = tb_rhi_texture_index(
            renderer->device, tb_render_graph_rhi_texture(pass->graph, recording->depth));
    }
    else if (layer == 1)
    {
        pipeline = renderer->line_top_pipeline;
    }
    tb_rhi_cmd_pipeline(pass->list, pipeline);
    tb_rhi_cmd_push_constants(pass->list, &constants, sizeof(constants));
    tb_rhi_cmd_vertex_buffer(pass->list, 0, frame->upload, recording->lines_offset[layer]);
    tb_rhi_cmd_draw(pass->list, (uint32_t)renderer->lines[layer].count, 1, 0, 0);
}

/* Draw the frame's meshes with a pipeline, skipping those whose constants
 * did not fit in the frame's uploads. */
static void tb_renderer_record_draws(tb_renderer_recording *recording,
                                     const tb_render_graph_pass_context *pass,
                                     tb_rhi_pipeline pipeline)
{
    tb_renderer *const renderer = recording->renderer;
    const tb_renderer_frame *const frame = &renderer->frames[renderer->slot];
    const tb_rhi_buffer skinned = recording->skinned.id != 0
                                      ? tb_render_graph_rhi_buffer(pass->graph, recording->skinned)
                                      : (tb_rhi_buffer){0};

    tb_rhi_cmd_pipeline(pass->list, pipeline);
    for (size_t index = 0; index < renderer->draw_count; index++)
    {
        const tb_renderer_draw *const draw = &renderer->draws[index];
        if (draw->constants == UINT64_MAX)
        {
            continue; /* the frame's uploads ran out */
        }
        tb_rhi_cmd_constant_buffer(pass->list, 0, frame->upload, draw->constants);
        if (draw->skinned_on_cpu)
        {
            tb_rhi_cmd_vertex_buffer(pass->list, 0, frame->upload, draw->skinned_offset);
        }
        else if (draw->skinned)
        {
            tb_rhi_cmd_vertex_buffer(pass->list, 0, skinned, draw->skinned_offset);
        }
        else
        {
            tb_rhi_cmd_vertex_buffer(pass->list, 0, draw->mesh->vertices, 0);
        }
        tb_rhi_cmd_index_buffer(pass->list, draw->mesh->indices, 0, TB_RHI_FORMAT_R32_UINT);
        tb_rhi_cmd_draw_indexed(pass->list, draw->mesh->index_count, 1, 0, 0, 0);
    }
}

static void tb_renderer_record_customs(tb_renderer_recording *recording,
                                       const tb_render_graph_pass_context *pass)
{
    tb_renderer *const renderer = recording->renderer;

    for (size_t index = 0; index < renderer->custom_count; index++)
    {
        const tb_renderer_draw_context context = {.device = renderer->device,
                                                  .list = pass->list,
                                                  .view = recording->view,
                                                  .projection = recording->projection,
                                                  .clip = recording->clip,
                                                  .width = renderer->render_width,
                                                  .height = renderer->render_height,
                                                  .context = renderer->customs[index].context};
        renderer->customs[index].draw(&context);
    }
}

/* coverage-off: only a device without bindless resources draws this way, and no
 * backend that tests run on lacks them */
/* Without bindless resources: the meshes, the custom draws, and the lines
 * tested against depth, into the swap chain. */
static void tb_renderer_record_scene(const tb_render_graph_pass_context *pass)
{
    tb_renderer_recording *const recording = pass->context;

    tb_renderer_record_draws(recording, pass, recording->renderer->mesh_pipeline);
    tb_renderer_record_customs(recording, pass);
    tb_renderer_record_lines(recording, pass, 0);
}

static void tb_renderer_record_overlay(const tb_render_graph_pass_context *pass)
{
    tb_renderer_record_lines(pass->context, pass, 1);
}

/* coverage-on */

static void tb_renderer_record_gbuffer(const tb_render_graph_pass_context *pass)
{
    tb_renderer_recording *const recording = pass->context;

    tb_renderer_record_draws(recording, pass, recording->renderer->material_pipeline);
}

static uint32_t tb_renderer_graph_index(const tb_render_graph_pass_context *pass,
                                        tb_render_graph_texture texture)
{
    const tb_renderer_recording *const recording = pass->context;

    return tb_rhi_texture_index(recording->renderer->device,
                                tb_render_graph_rhi_texture(pass->graph, texture));
}

static void tb_renderer_record_lighting(const tb_render_graph_pass_context *pass)
{
    tb_renderer_recording *const recording = pass->context;
    const tb_renderer_gbuffer_indices indices = {
        .base_color = tb_renderer_graph_index(pass, recording->gbuffer[0]),
        .normal = tb_renderer_graph_index(pass, recording->gbuffer[1]),
        .surface = tb_renderer_graph_index(pass, recording->gbuffer[2]),
        .depth = tb_renderer_graph_index(pass, recording->depth)};

    tb_renderer_lighting_record(recording->renderer, pass->list, recording->lighting, &indices);
}

/* After lighting, what is drawn into the scene's light as it is: the
 * custom draws, then the sky behind everything, so that it shades only the
 * pixels left. */
static void tb_renderer_record_forward(const tb_render_graph_pass_context *pass)
{
    tb_renderer_recording *const recording = pass->context;

    tb_renderer_record_customs(recording, pass);
    tb_renderer_sky_record(recording->renderer, pass->list, recording->sky);
}

static void tb_renderer_record_tonemap(const tb_render_graph_pass_context *pass)
{
    tb_renderer_recording *const recording = pass->context;

    const tb_render_graph_texture source =
        recording->temporal.constants != UINT64_MAX ? recording->temporal.output : recording->scene;

    tb_renderer_tonemap_record(recording->renderer, pass->list,
                               tb_renderer_graph_index(pass, source));
}

/* Both layers of lines over the tone-mapped output. */
static void tb_renderer_record_output_lines(const tb_render_graph_pass_context *pass)
{
    tb_renderer_record_lines(pass->context, pass, 0);
    tb_renderer_record_lines(pass->context, pass, 1);
}

static tb_render_graph_texture tb_renderer_target(tb_renderer *renderer, tb_rhi_format format,
                                                  const char *name)
{
    return tb_render_graph_create_texture(renderer->graph,
                                          &(tb_rhi_texture_desc){.format = format,
                                                                 .width = renderer->render_width,
                                                                 .height = renderer->render_height},
                                          name);
}

/* coverage-off: only a device without bindless resources draws this way, and no
 * backend that tests run on lacks them */
/* Without bindless resources: the scene pass straight into the swap
 * chain's image, and the lines on top. */
static void tb_renderer_add_forward_passes(tb_renderer *renderer, tb_renderer_recording *recording,
                                           tb_render_graph_texture image)
{
    tb_render_graph_pass pass = {
        .name = "scene", .execute = tb_renderer_record_scene, .context = recording};

    if (recording->skinned.id != 0)
    {
        pass.accesses[0] = (tb_render_graph_access){.buffer = recording->skinned,
                                                    .state = TB_RHI_STATE_VERTEX_INDEX};
        pass.access_count = 1;
    }
    pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_CLEAR};
    memcpy(pass.colors[0].clear, &renderer->clear, sizeof(pass.colors[0].clear));
    pass.color_count = 1;
    recording->depth = tb_renderer_target(renderer, TB_RENDERER_DEPTH_FORMAT, "depth");
    pass.depth = (tb_render_graph_depth){.texture = recording->depth, .load = TB_RHI_LOAD_CLEAR};
    tb_render_graph_add_pass(renderer->graph, &pass);
    if (recording->lines_offset[1] != UINT64_MAX)
    {
        pass = (tb_render_graph_pass){
            .name = "debug on top", .execute = tb_renderer_record_overlay, .context = recording};
        pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_KEEP};
        pass.color_count = 1;
        tb_render_graph_add_pass(renderer->graph, &pass);
    }
}

/* coverage-on */

/* The G-buffer: each mesh's surface, and depth. */
static void tb_renderer_add_gbuffer_pass(tb_renderer *renderer, tb_renderer_recording *recording)
{
    static const tb_rhi_format formats[TB_RENDERER_GBUFFER_TARGETS] = {
        TB_RENDERER_BASE_COLOR_FORMAT, TB_RENDERER_NORMAL_FORMAT, TB_RENDERER_SURFACE_FORMAT};
    static const char *const names[TB_RENDERER_GBUFFER_TARGETS] = {"base color", "normal",
                                                                   "surface"};
    tb_render_graph_pass pass = {
        .name = "gbuffer", .execute = tb_renderer_record_gbuffer, .context = recording};

    if (recording->skinned.id != 0)
    {
        pass.accesses[0] = (tb_render_graph_access){.buffer = recording->skinned,
                                                    .state = TB_RHI_STATE_VERTEX_INDEX};
        pass.access_count = 1;
    }
    for (uint32_t target = 0; target < TB_RENDERER_GBUFFER_TARGETS; target++)
    {
        recording->gbuffer[target] = tb_renderer_target(renderer, formats[target], names[target]);
        /* The lighting pass reads the G-buffer only where depth says a
         * mesh was drawn, so what was there before need not be cleared. */
        pass.colors[target] =
            (tb_render_graph_color){.texture = recording->gbuffer[target], .load = TB_RHI_LOAD_ANY};
    }
    pass.color_count = TB_RENDERER_GBUFFER_TARGETS;
    recording->depth = tb_renderer_target(renderer, TB_RENDERER_DEPTH_FORMAT, "depth");
    pass.depth = (tb_render_graph_depth){.texture = recording->depth, .load = TB_RHI_LOAD_CLEAR};
    tb_render_graph_add_pass(renderer->graph, &pass);
}

/* The raster lighting mode: the G-buffer, lit into the scene's light, the
 * custom draws and the sky over it, tone mapped into the swap chain's
 * image, and the lines over that. */
static void tb_renderer_add_deferred_passes(tb_renderer *renderer, tb_renderer_recording *recording,
                                            tb_render_graph_texture image)
{
    tb_render_graph_pass pass = {0};

    tb_renderer_add_gbuffer_pass(renderer, recording);
    recording->scene = tb_renderer_target(renderer, TB_RENDERER_SCENE_FORMAT, "scene color");
    pass = (tb_render_graph_pass){
        .name = "lighting", .execute = tb_renderer_record_lighting, .context = recording};
    for (uint32_t target = 0; target < TB_RENDERER_GBUFFER_TARGETS; target++)
    {
        pass.accesses[target] = (tb_render_graph_access){.texture = recording->gbuffer[target],
                                                         .state = TB_RHI_STATE_SHADER_READ};
    }
    pass.accesses[TB_RENDERER_GBUFFER_TARGETS] =
        (tb_render_graph_access){.texture = recording->depth, .state = TB_RHI_STATE_SHADER_READ};
    pass.access_count = TB_RENDERER_GBUFFER_TARGETS + 1u;
    pass.colors[0] = (tb_render_graph_color){.texture = recording->scene, .load = TB_RHI_LOAD_ANY};
    if (recording->lighting == UINT64_MAX)
    {
        /* The frame's uploads had no room for the lighting: the clear color. */
        pass.colors[0].load = TB_RHI_LOAD_CLEAR;
        memcpy(pass.colors[0].clear, &renderer->clear, sizeof(pass.colors[0].clear));
    }
    pass.color_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
    if (renderer->custom_count > 0 || recording->sky != UINT64_MAX)
    {
        pass = (tb_render_graph_pass){
            .name = "forward", .execute = tb_renderer_record_forward, .context = recording};
        pass.colors[0] =
            (tb_render_graph_color){.texture = recording->scene, .load = TB_RHI_LOAD_KEEP};
        pass.color_count = 1;
        pass.depth = (tb_render_graph_depth){.texture = recording->depth, .load = TB_RHI_LOAD_KEEP};
        tb_render_graph_add_pass(renderer->graph, &pass);
    }
    if (recording->temporal.constants != UINT64_MAX)
    {
        recording->temporal.scene = recording->scene;
        recording->temporal.depth = recording->depth;
        tb_renderer_temporal_record(renderer, &recording->temporal);
    }
    pass = (tb_render_graph_pass){
        .name = "tonemap", .execute = tb_renderer_record_tonemap, .context = recording};
    pass.accesses[0] = (tb_render_graph_access){
        .texture = recording->temporal.constants != UINT64_MAX ? recording->temporal.output
                                                               : recording->scene,
        .state = TB_RHI_STATE_SHADER_READ};
    pass.access_count = 1;
    pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_ANY};
    pass.color_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
    if (recording->lines_offset[0] != UINT64_MAX || recording->lines_offset[1] != UINT64_MAX)
    {
        pass = (tb_render_graph_pass){.name = "debug lines",
                                      .execute = tb_renderer_record_output_lines,
                                      .context = recording};
        pass.accesses[0] = (tb_render_graph_access){.texture = recording->depth,
                                                    .state = TB_RHI_STATE_SHADER_READ};
        pass.access_count = 1;
        pass.colors[0] = (tb_render_graph_color){.texture = image, .load = TB_RHI_LOAD_KEEP};
        pass.color_count = 1;
        tb_render_graph_add_pass(renderer->graph, &pass);
    }
}

typedef struct tb_renderer_capturing
{
    tb_renderer *renderer;
    tb_render_graph_texture image;
    uint32_t pitch;
} tb_renderer_capturing;

static void tb_renderer_record_capture(const tb_render_graph_pass_context *pass)
{
    const tb_renderer_capturing *const capturing = pass->context;

    tb_rhi_cmd_copy_texture_to_buffer(
        pass->list, &(tb_rhi_texture_copy){
                        .buffer = capturing->renderer->capture,
                        .row_pitch = capturing->pitch,
                        .texture = tb_render_graph_rhi_texture(pass->graph, capturing->image)});
}

/* Copy the captured frame out of its readback buffer, rows packed. */
static void tb_renderer_keep_capture(tb_renderer *renderer, uint64_t submission, uint32_t pitch)
{
    const size_t row = (size_t)renderer->width * 4u;
    uint8_t *padded = NULL;

    if (tb_rhi_wait(renderer->device, submission) != TB_RHI_OK)
    {
        return;
    }
    padded = tb_malloc((size_t)pitch * renderer->height);
    tb_rhi_buffer_read(renderer->device, renderer->capture, 0, padded,
                       (size_t)pitch * renderer->height);
    tb_free(renderer->captured);
    renderer->captured = tb_malloc(row * renderer->height);
    for (uint32_t y = 0; y < renderer->height; y++)
    {
        memcpy(&renderer->captured[row * y], &padded[(size_t)pitch * y], row);
    }
    renderer->captured_width = renderer->width;
    renderer->captured_height = renderer->height;
    tb_free(padded);
    tb_rhi_buffer_destroy(renderer->device, renderer->capture);
    renderer->capture = (tb_rhi_buffer){0};
}

tb_rhi_result tb_renderer_end_frame(tb_renderer *renderer)
{
    tb_renderer_recording recording = {0};
    tb_renderer_capturing capturing = {0};
    tb_skinning_recording skinning = {0};
    tb_canvas_recording canvas = {0};
    tb_render_graph_texture image = {0};
    tb_render_graph_pass pass = {0};
    uint64_t submission = 0;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(renderer->in_frame, "a frame ended that was not begun");
    tb_renderer_text_end_frame(renderer);
    renderer->in_frame = false;
    tb_renderer_upload_frame(renderer, &recording);

    tb_render_graph_begin(renderer->graph);
    image =
        tb_render_graph_import_texture(renderer->graph, tb_rhi_swap_chain_current(renderer->chain),
                                       TB_RHI_STATE_PRESENT, TB_RHI_STATE_PRESENT, "back buffer");
    tb_renderer_skinning_record(renderer, &skinning);
    recording.skinned = skinning.vertices;
    if (renderer->deferred)
    {
        tb_renderer_add_deferred_passes(renderer, &recording, image);
    }
    else
    {
        tb_renderer_add_forward_passes(renderer, &recording, image);
    }
    tb_renderer_canvas_record(renderer, &canvas, image);
    if (renderer->capture_next)
    {
        capturing = (tb_renderer_capturing){renderer, image, (renderer->width * 4u + 255u) & ~255u};
        if (tb_rhi_buffer_create(
                renderer->device,
                &(tb_rhi_buffer_desc){.size = (uint64_t)capturing.pitch * renderer->height,
                                      .memory = TB_RHI_MEMORY_READBACK,
                                      .name = "capture"},
                &renderer->capture, NULL) == TB_RHI_OK)
        {
            pass = (tb_render_graph_pass){.name = "capture",
                                          .execute = tb_renderer_record_capture,
                                          .context = &capturing,
                                          .never_cull = true};
            pass.accesses[0] =
                (tb_render_graph_access){.texture = image, .state = TB_RHI_STATE_COPY_SOURCE};
            pass.access_count = 1;
            tb_render_graph_add_pass(renderer->graph, &pass);
        }
    }
    submission = tb_render_graph_execute(renderer->graph);
    renderer->frames[renderer->slot].submission = submission;
    tb_renderer_temporal_end_frame(renderer, recording.unjittered_clip);
    if (renderer->capture.generation != 0)
    {
        tb_renderer_keep_capture(renderer, submission, capturing.pitch);
    }
    renderer->capture_next = false;
    result = tb_rhi_swap_chain_present(renderer->chain);
    return result;
}

void tb_renderer_capture_next_frame(tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    renderer->capture_next = true;
}

const uint8_t *tb_renderer_captured(const tb_renderer *renderer, uint32_t *width, uint32_t *height)
{
    TB_ASSERT(renderer != NULL && width != NULL && height != NULL);
    *width = renderer->captured_width;
    *height = renderer->captured_height;
    return renderer->captured;
}

/* ---------------------------------------------------------------------------
 * Meshes.
 * ------------------------------------------------------------------------- */

tb_mesh *tb_mesh_create(tb_renderer *renderer, const tb_mesh_vertex *vertices, size_t vertex_count,
                        const uint32_t *indices, size_t index_count)
{
    const size_t vertex_bytes = vertex_count * sizeof(*vertices);
    const size_t index_bytes = index_count * sizeof(*indices);
    tb_mesh *mesh = NULL;
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    TB_ASSERT(renderer != NULL && vertices != NULL && indices != NULL);
    TB_ASSERT_MSG(vertex_count > 0 && index_count > 0 && index_count % 3 == 0 &&
                      index_count <= UINT32_MAX && vertex_count <= SIZE_MAX / sizeof(*vertices),
                  "a mesh of whole triangles");
    mesh = tb_calloc(1, sizeof(*mesh));
    mesh->index_count = (uint32_t)index_count;
    if (tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = vertex_bytes,
                                                   .usage = TB_RHI_BUFFER_VERTEX,
                                                   .name = "mesh vertices"},
                             &mesh->vertices, &error) != TB_RHI_OK ||
        tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = index_bytes,
                                                   .usage = TB_RHI_BUFFER_INDEX,
                                                   .name = "mesh indices"},
                             &mesh->indices, &error) != TB_RHI_OK ||
        tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = vertex_bytes + index_bytes,
                                                   .memory = TB_RHI_MEMORY_UPLOAD},
                             &staging, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for a mesh: %s", error.message);
    }
    tb_rhi_buffer_write(renderer->device, staging, 0, vertices, vertex_bytes);
    tb_rhi_buffer_write(renderer->device, staging, vertex_bytes, indices, index_bytes);
    list = tb_rhi_command_list_begin(renderer->device);
    tb_rhi_cmd_barriers(list,
                        (const tb_rhi_barrier[]){{.buffer = mesh->vertices,
                                                  .before = TB_RHI_STATE_COMMON,
                                                  .after = TB_RHI_STATE_COPY_DEST},
                                                 {.buffer = mesh->indices,
                                                  .before = TB_RHI_STATE_COMMON,
                                                  .after = TB_RHI_STATE_COPY_DEST}},
                        2);
    tb_rhi_cmd_copy_buffer(list, mesh->vertices, 0, staging, 0, vertex_bytes);
    tb_rhi_cmd_copy_buffer(list, mesh->indices, 0, staging, vertex_bytes, index_bytes);
    tb_rhi_cmd_barriers(list,
                        (const tb_rhi_barrier[]){{.buffer = mesh->vertices,
                                                  .before = TB_RHI_STATE_COPY_DEST,
                                                  .after = TB_RHI_STATE_VERTEX_INDEX},
                                                 {.buffer = mesh->indices,
                                                  .before = TB_RHI_STATE_COPY_DEST,
                                                  .after = TB_RHI_STATE_VERTEX_INDEX}},
                        2);
    (void)tb_rhi_submit(renderer->device, &list, 1);
    /* The staging buffer goes once the GPU has copied it. */
    tb_rhi_buffer_destroy(renderer->device, staging);
    return mesh;
}

tb_mesh *tb_mesh_create_shape(tb_renderer *renderer, tb_mesh_shape shape, uint32_t detail)
{
    tb_mesh_vertex *vertices = NULL;
    uint32_t *indices = NULL;
    size_t vertex_count = 0;
    size_t index_count = 0;
    tb_mesh *mesh = NULL;

    tb_mesh_generate(shape, detail, &vertices, &vertex_count, &indices, &index_count);
    mesh = tb_mesh_create(renderer, vertices, vertex_count, indices, index_count);
    tb_free(vertices);
    tb_free(indices);
    return mesh;
}

void tb_mesh_destroy(tb_renderer *renderer, tb_mesh *mesh)
{
    TB_ASSERT(renderer != NULL);
    if (mesh == NULL)
    {
        return;
    }
    tb_rhi_buffer_destroy(renderer->device, mesh->vertices);
    tb_rhi_buffer_destroy(renderer->device, mesh->indices);
    tb_free(mesh);
}
