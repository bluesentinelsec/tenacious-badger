/* What the renderer keeps, shared by its sources. */
#ifndef TB_RENDERER_INTERNAL_H
#define TB_RENDERER_INTERNAL_H

#include "canvas_internal.h"

#include "badger/material.h"
#include "badger/render_graph.h"
#include "badger/renderer.h"
#include "badger/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_dxc;

/* Frames the CPU prepares while the GPU draws the ones before. */
#define TB_RENDERER_FRAMES 2u
#define TB_RENDERER_COLOR_FORMAT TB_RHI_FORMAT_B8G8R8A8_UNORM
#define TB_RENDERER_DEPTH_FORMAT TB_RHI_FORMAT_D32_FLOAT

struct tb_mesh
{
    tb_rhi_buffer vertices;
    tb_rhi_buffer indices;
    uint32_t index_count;
};

/* A draw's constants, as the mesh shader's DrawConstants lays them out. */
typedef struct tb_renderer_draw_constants
{
    tb_mat4 clip;
    tb_mat4 model;
    tb_vec4 color;
    tb_vec4 light; /* the light's direction, and 1 to light the mesh or 0 */
} tb_renderer_draw_constants;

/* The light every lit draw is lit by this frame. */
typedef struct tb_renderer_lighting
{
    tb_vec3 direction; /* the way the light goes */
    tb_vec3 eye;       /* the camera's position */
} tb_renderer_lighting;

typedef struct tb_renderer_draw
{
    const tb_mesh *mesh;
    const tb_material *material; /* NULL for a color alone */
    tb_mat4 transform;
    tb_vec4 color;
    tb_mesh_shading shading;
    uint64_t constants; /* where in the frame's uploads; UINT64_MAX when they ran out */
} tb_renderer_draw;

typedef struct tb_renderer_custom
{
    tb_renderer_draw_function *draw;
    void *context;
} tb_renderer_custom;

typedef struct tb_renderer_line_vertex
{
    tb_vec3 position;
    tb_vec4 color;
} tb_renderer_line_vertex;

/* Lines of one layer: tested against depth, or on top. */
typedef struct tb_renderer_lines
{
    tb_renderer_line_vertex *vertices;
    size_t count;
    size_t capacity;
} tb_renderer_lines;

/* What one frame in flight writes for the GPU. */
typedef struct tb_renderer_frame
{
    tb_rhi_buffer upload;
    uint64_t used;
    uint64_t submission; /* the last that read it */
} tb_renderer_frame;

struct tb_renderer
{
    tb_rhi_device *device;
    tb_rhi_device_info info;
    tb_rhi_swap_chain *chain;
    struct tb_dxc *dxc;
    struct tb_window *window;
    struct tb_log *log;
    tb_render_graph *graph;
    uint32_t width; /* of the swap chain, in pixels */
    uint32_t height;
    bool vsync;
    bool vsync_wanted;
    bool vsync_changed;

    tb_rhi_pipeline mesh_pipeline;
    tb_rhi_pipeline line_pipeline;
    tb_rhi_pipeline line_top_pipeline;

    tb_renderer_frame frames[TB_RENDERER_FRAMES];
    uint32_t slot;
    bool in_frame;
    tb_camera camera;
    tb_vec4 clear;
    tb_renderer_draw *draws;
    size_t draw_count;
    size_t draw_capacity;
    tb_renderer_custom *customs;
    size_t custom_count;
    size_t custom_capacity;
    tb_mutex lines_lock; /* guards lines, which any thread may add to */
    tb_renderer_lines lines[2];

    tb_rhi_pipeline material_pipeline; /* none without bindless resources */
    tb_rhi_pipeline mip_pipeline;      /* none without bindless resources */
    tb_rhi_sampler material_sampler;   /* trilinear, repeating, anisotropic */
    uint32_t anisotropy;               /* of material_sampler */
    uint32_t anisotropy_wanted;        /* from the next frame on */
    tb_texture *material_white;        /* what a material without a texture samples */
    tb_texture *material_flat;         /* the normal map of a material without one */

    tb_canvas_batch canvas;            /* this frame's 2D */
    tb_rhi_pipeline canvas_pipeline;   /* none without bindless resources */
    tb_rhi_sampler canvas_samplers[2]; /* linear, nearest */
    tb_image *canvas_white;            /* what solid quads sample */
    tb_canvas_stats canvas_stats;      /* of the last frame */

    bool capture_next;
    tb_rhi_buffer capture;
    uint8_t *captured;
    uint32_t captured_width;
    uint32_t captured_height;
};

/* The canvas's draw in a frame being recorded. */
typedef struct tb_canvas_recording
{
    tb_renderer *renderer;
    uint64_t offset; /* of its quads in the frame's uploads */
    uint32_t count;
} tb_canvas_recording;

/* Make a pipeline of a shader of the renderer's own, with entry points
 * MainVS and MainPS, compiled for the device's backend. */
tb_rhi_result tb_renderer_pipeline(tb_renderer *renderer, const char *path, const char *source,
                                   tb_rhi_graphics_pipeline_desc *desc, tb_rhi_pipeline *pipeline,
                                   tb_rhi_error *error);

/* Write bytes to this frame's uploads at an alignment; their offset, or
 * UINT64_MAX when they are full. */
uint64_t tb_renderer_upload(tb_renderer *renderer, const void *data, size_t size,
                            uint64_t alignment);

/* Bytes this frame's uploads have left at an alignment. */
uint64_t tb_renderer_upload_room(const tb_renderer *renderer, uint64_t alignment);

/* Add a draw of a mesh to the frame. */
void tb_renderer_add_draw(tb_renderer *renderer, const tb_renderer_draw *draw);

/* Compile a shader of the renderer's own for the device's backend. */
tb_rhi_result tb_renderer_shader(tb_renderer *renderer, const char *path, const char *source,
                                 const char *entry, tb_rhi_stage stage, tb_rhi_shader *shader,
                                 tb_rhi_error *error);

/* The materials' sampler, textures and pipelines (material.c). */
tb_rhi_result tb_renderer_materials_create(tb_renderer *renderer, uint32_t anisotropy,
                                           tb_rhi_error *error);
void tb_renderer_materials_destroy(tb_renderer *renderer);

/* Take the anisotropy asked for, at the start of a frame. */
void tb_renderer_materials_begin_frame(tb_renderer *renderer);

/* Write a material draw's constants to the frame's uploads: their offset, or
 * UINT64_MAX when the uploads are full. */
uint64_t tb_renderer_material_upload(tb_renderer *renderer, const tb_renderer_draw *draw,
                                     tb_mat4 clip, const tb_renderer_lighting *lighting);

/* The canvas's samplers, image and pipeline (canvas.c). */
tb_rhi_result tb_renderer_canvas_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_canvas_destroy(tb_renderer *renderer);

/* Upload the frame's quads and add the pass that draws them over `image`.
 * `recording` lives until the graph is executed. */
void tb_renderer_canvas_record(tb_renderer *renderer, tb_canvas_recording *recording,
                               tb_render_graph_texture image);

#endif
