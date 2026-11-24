/* What the renderer keeps, shared by its sources. */
#ifndef TB_RENDERER_INTERNAL_H
#define TB_RENDERER_INTERNAL_H

#include "canvas_internal.h"

#include "badger/arena.h"
#include "badger/font_catalog.h"
#include "badger/font_raster.h"
#include "badger/material.h"
#include "badger/render_graph.h"
#include "badger/renderer.h"
#include "badger/sky.h"
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

/* A glyph drawn this frame, kept so that it can be found in the atlas
 * again when the atlas clears before the frame ends. */
typedef struct tb_canvas_glyph
{
    size_t quad; /* its quad, an index of the canvas's batch */
    const tb_font *font;
    uint16_t glyph;
    tb_font_raster_options options;
    uint64_t generation; /* the atlas's, when the glyph was placed */
} tb_canvas_glyph;

/* A label at a point in the world, its text in the labels' bytes. */
typedef struct tb_renderer_label
{
    tb_vec3 position;
    tb_vec4 color;
    size_t offset;
    size_t length;
} tb_renderer_label;

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

    tb_sky sky;                   /* behind every frame, until changed */
    tb_rhi_pipeline sky_pipeline; /* none without bindless resources */

    tb_canvas_batch canvas;            /* this frame's 2D */
    tb_rhi_pipeline canvas_pipeline;   /* none without bindless resources */
    tb_rhi_sampler canvas_samplers[2]; /* linear, nearest */
    tb_image *canvas_white;            /* what solid quads sample */
    tb_canvas_stats canvas_stats;      /* of the last frame */

    tb_font_atlas *glyph_atlas;   /* the glyphs text is drawn from */
    tb_rhi_texture glyph_texture; /* the atlas's image on the GPU */
    uint32_t glyph_texture_index; /* its bindless index */
    uint32_t glyph_side;          /* its width and height */
    bool glyph_texture_written;   /* its first copy has been made */
    tb_arena *text_arena;         /* layouts, while text is drawn */
    tb_canvas_glyph *glyphs;      /* this frame's */
    size_t glyph_count;
    size_t glyph_capacity;
    size_t glyphs_hidden;      /* this frame's, the atlas could not hold */
    const tb_font *debug_font; /* labels', as chosen; NULL for the default */
    float debug_font_size;
    tb_font_catalog *default_catalog; /* holds the default debug font's bytes */
    tb_font *default_debug_font;      /* the catalog's UI font, opened when first needed */
    bool default_debug_font_tried;
    tb_renderer_label *labels; /* this frame's; guarded by lines_lock */
    size_t label_count;
    size_t label_capacity;
    char *label_bytes; /* their text */
    size_t label_bytes_used;
    size_t label_bytes_capacity;

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

/* A texture's bindless index, and whether it is a cube map. */
uint32_t tb_texture_bindless_index(const tb_texture *texture);
bool tb_texture_is_cube(const tb_texture *texture);

/* The sky's pipeline (sky.c). */
tb_rhi_result tb_renderer_sky_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_sky_destroy(tb_renderer *renderer);

/* Write the sky's constants for the frame's camera, seen at an aspect:
 * their offset in the frame's uploads, or UINT64_MAX when there is no sky
 * to draw or no room. */
uint64_t tb_renderer_sky_upload(tb_renderer *renderer, float aspect);

/* Draw the sky behind what the scene pass drew so far. */
void tb_renderer_sky_record(tb_renderer *renderer, tb_rhi_command_list *list, uint64_t constants);

/* The canvas's samplers, image and pipeline (canvas.c). */
tb_rhi_result tb_renderer_canvas_create(tb_renderer *renderer, uint32_t glyph_atlas_size,
                                        tb_rhi_error *error);
void tb_renderer_canvas_destroy(tb_renderer *renderer);

/* The glyph atlas and its texture (canvas_text.c), made with the canvas. */
tb_rhi_result tb_renderer_text_create(tb_renderer *renderer, uint32_t side, tb_rhi_error *error);
void tb_renderer_text_destroy(tb_renderer *renderer);

/* Forget the last frame's glyphs and labels. */
void tb_renderer_text_begin_frame(tb_renderer *renderer);

/* Before the frame's 2D is recorded, while it may still be drawn to: draw
 * the labels, find again the glyphs that the atlas cleared, and copy what
 * changed in the atlas to its texture. */
void tb_renderer_text_end_frame(tb_renderer *renderer);

/* Upload the frame's quads and add the pass that draws them over `image`.
 * `recording` lives until the graph is executed. */
void tb_renderer_canvas_record(tb_renderer *renderer, tb_canvas_recording *recording,
                               tb_render_graph_texture image);

#endif
