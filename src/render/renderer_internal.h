/* What the renderer keeps, shared by its sources. */
#ifndef TB_RENDERER_INTERNAL_H
#define TB_RENDERER_INTERNAL_H

#include "canvas_internal.h"

#include "badger/arena.h"
#include "badger/font_catalog.h"
#include "badger/font_raster.h"
#include "badger/light.h"
#include "badger/material.h"
#include "badger/render_graph.h"
#include "badger/renderer.h"
#include "badger/sky.h"
#include "badger/thread.h"
#include "badger/viewport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_dxc;
struct tb_shader_cache;
struct tb_shader_library;

/* Frames the CPU prepares while the GPU draws the ones before. */
#define TB_RENDERER_FRAMES 2u
#define TB_RENDERER_COLOR_FORMAT TB_RHI_FORMAT_B8G8R8A8_UNORM
#define TB_RENDERER_DEPTH_FORMAT TB_RHI_FORMAT_D32_FLOAT
/* The raster lighting mode's targets: the scene's light, in linear HDR,
 * and the G-buffer's base color, octahedral normal, and surface (metallic,
 * roughness, ambient occlusion, and 1 for lit or 0 for unlit). */
#define TB_RENDERER_SCENE_FORMAT TB_RHI_FORMAT_R16G16B16A16_FLOAT
#define TB_RENDERER_BASE_COLOR_FORMAT TB_RHI_FORMAT_R8G8B8A8_SRGB
#define TB_RENDERER_NORMAL_FORMAT TB_RHI_FORMAT_R16G16_FLOAT
#define TB_RENDERER_SURFACE_FORMAT TB_RHI_FORMAT_R8G8B8A8_UNORM
#define TB_RENDERER_GBUFFER_TARGETS 3u
/* The render scale's bounds. */
#define TB_RENDERER_MIN_RENDER_SCALE 0.25f
#define TB_RENDERER_MAX_RENDER_SCALE 2.0f

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

/* A light as the lighting shader reads it, 64 bytes in the frame's
 * uploads. */
typedef struct tb_packed_light
{
    tb_vec4 position;  /* in the world; w: 1 / range squared, 0 for none */
    tb_vec4 direction; /* the way its light goes, of unit length; w: its tb_light_kind */
    tb_vec4 radiance;  /* color * intensity * pi, which a Lambert surface divides by pi */
    tb_vec4 cone;      /* a spot's cosine of its outer angle, and 1 / (cos inner - cos outer) */
} tb_packed_light;

/* A light of the scene, and the generation of the handles that name it. */
typedef struct tb_light_slot
{
    tb_light light;
    uint32_t generation; /* of 16 bits, never 0 */
    bool used;
} tb_light_slot;

typedef struct tb_renderer_draw
{
    const tb_mesh *mesh;
    const tb_material *material; /* NULL for a color alone */
    tb_mat4 transform;
    tb_vec4 color;
    tb_mesh_shading shading;
    uint64_t constants;      /* where in the frame's uploads; UINT64_MAX when they ran out */
    bool skinned;            /* its vertices are the frame's skinned vertices, not its mesh's */
    bool skinned_on_cpu;     /* skinned, and they are in the frame's uploads */
    uint64_t skinned_offset; /* bytes into the frame's skinned vertices, or its uploads */
} tb_renderer_draw;

/* A skinned draw of the frame, which the skinning pass skins. */
typedef struct tb_renderer_skin
{
    uint32_t source;       /* the bindless index of the mesh's vertices */
    uint32_t vertex_count; /* of the mesh */
    uint64_t joints;       /* its joint matrices' offset in the frame's uploads */
    uint32_t joint_count;
    uint64_t output; /* bytes into the frame's skinned vertices */
} tb_renderer_skin;

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
    struct tb_dxc *dxc;                       /* NULL where shaders come from the library only */
    struct tb_shader_library *shader_library; /* of the device's backend; NULL for none */
    struct tb_shader_cache *shader_cache;     /* borrowed; NULL for none */
    bool shader_library_only;
    struct tb_window *window;
    struct tb_log *log;
    tb_render_graph *graph;
    uint32_t width; /* of the swap chain, in pixels */
    uint32_t height;

    /* The display policy, the player's aspect ratio, and the viewport they
     * give the swap chain. In the viewport render mode, the 3D scene and the
     * design's canvas are drawn into the viewport's target, which is
     * upscaled into its rectangle; in the canvas mode, straight into it. */
    tb_viewport_policy viewport_policy;
    float aspect_ratio;
    tb_viewport viewport;
    /* Where the 3D scene is drawn on its output, in the output's pixels:
     * the viewport's rectangle of the swap chain, or the whole target. */
    tb_rhi_rect scene_rect;
    bool viewport_mode_said;          /* that the viewport render mode needs bindless was logged */
    tb_rhi_pipeline upscale_pipeline; /* none without bindless resources */
    bool vsync;
    bool vsync_wanted;
    bool vsync_changed;

    /* The raster lighting mode, on a device with bindless resources: a
     * G-buffer lit by a full-screen pass, tone mapped into the swap chain.
     * Without, meshes are drawn lit by one light straight into it. */
    bool deferred;
    tb_rhi_pipeline mesh_pipeline;     /* without bindless resources */
    tb_rhi_pipeline line_pipeline;     /* lines tested against depth */
    tb_rhi_pipeline line_top_pipeline; /* lines on top */
    tb_rhi_pipeline lighting_pipeline; /* with bindless resources */
    tb_rhi_pipeline tonemap_pipeline;  /* with bindless resources */
    tb_light_slot *lights;             /* the scene's, by slot */
    size_t light_slots;                /* in use or freed */
    size_t light_capacity;
    size_t light_count;
    tb_vec3 ambient_sky; /* without a sky */
    tb_vec3 ambient_ground;

    /* What the frames are made with, and what they may be. */
    tb_renderer_capabilities capabilities;
    tb_renderer_quality quality_asked;
    tb_renderer_quality quality; /* in effect, within the capabilities */
    uint32_t render_width;       /* the scene's, its rectangle's times the render scale */
    uint32_t render_height;

    /* Temporal anti-aliasing (temporal.c). */
    tb_rhi_pipeline temporal_pipeline;
    tb_rhi_texture history[2];     /* at the scene's rectangle's size; one read, one written */
    tb_rhi_state history_state[2]; /* where each was left */
    uint32_t history_width;        /* of the history textures */
    uint32_t history_height;
    uint32_t history_written; /* which of `history` the next frame writes */
    bool history_valid;       /* the one not written holds the last frame */
    uint32_t jitter_frame;    /* frames jittered, for the Halton sequence */
    tb_mat4 previous_clip;    /* the last frame's clip, unjittered */
    tb_camera_projection previous_projection;
    uint32_t previous_render_width; /* the last frame's render size */
    uint32_t previous_render_height;

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

    tb_rhi_pipeline material_pipeline; /* into the G-buffer; none without bindless */
    tb_rhi_pipeline mip_pipeline;      /* none without bindless resources */
    tb_rhi_sampler material_sampler;   /* trilinear, repeating, anisotropic */
    uint32_t anisotropy;               /* of material_sampler */
    uint32_t anisotropy_wanted;        /* from the next frame on */
    tb_texture *material_white;        /* what a material without a texture samples */
    tb_texture *material_flat;         /* the normal map of a material without one */

    tb_sky sky;                   /* behind every frame, until changed */
    tb_rhi_pipeline sky_pipeline; /* none without bindless resources */

    tb_rhi_pipeline skin_pipeline; /* none where the GPU cannot skin */
    tb_renderer_skin *skins;       /* this frame's, for the skinning pass */
    size_t skin_count;
    size_t skin_capacity;
    uint64_t skinned_bytes; /* of the frame's skinned vertices */
    /* Why the GPU cannot skin, so that the CPU does; "" when it can. */
    char skin_unavailable[TB_RHI_MESSAGE_CAPACITY];
    bool skin_cpu_forced;        /* the CPU skins from the next frame on */
    bool skin_on_cpu;            /* the CPU skins this frame */
    tb_mesh_vertex *cpu_skinned; /* a draw's vertices, skinned on the CPU */
    size_t cpu_skinned_capacity;

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

/* The skinning pass of a frame being recorded. */
typedef struct tb_skinning_recording
{
    tb_renderer *renderer;
    tb_render_graph_buffer vertices; /* what it writes; none when nothing is skinned */
} tb_skinning_recording;

/* The canvas's draw into one target in a frame being recorded. */
typedef struct tb_canvas_draw
{
    tb_renderer *renderer;
    uint64_t offset; /* of its quads in the frame's uploads */
    uint32_t count;
    /* Its quads, in the canvas's batch, which holds them until the next
     * frame begins: what splits them into runs of one image. */
    const tb_canvas_quad *quads;
    uint32_t width; /* of the target, in pixels */
    uint32_t height;
} tb_canvas_draw;

/* The canvas's draws in a frame being recorded, by tb_canvas_target. */
typedef struct tb_canvas_recording
{
    tb_canvas_draw targets[TB_CANVAS_TARGET_COUNT];
} tb_canvas_recording;

/* Make a pipeline of a shader of the renderer's own, with entry points
 * MainVS and MainPS, for the device's backend, as tb_renderer_shader()
 * finds or compiles each. */
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

/* A shader of the renderer's own, for the device's backend: found in the
 * shader library, else in the shader cache, else compiled where the renderer
 * may. `source` is its text, or NULL for the engine's own file at `path`
 * (tb_shader_engine_loader()), which the build compiles ahead of time when
 * shaders/Engine/Shaders.manifest lists it. */
tb_rhi_result tb_renderer_shader(tb_renderer *renderer, const char *path, const char *source,
                                 const char *entry, tb_rhi_stage stage, tb_rhi_shader *shader,
                                 tb_rhi_error *error);

/* The materials' sampler, textures and pipelines (material.c). */
tb_rhi_result tb_renderer_materials_create(tb_renderer *renderer, uint32_t anisotropy,
                                           tb_rhi_error *error);
void tb_renderer_materials_destroy(tb_renderer *renderer);

/* Take the anisotropy asked for, at the start of a frame. */
void tb_renderer_materials_begin_frame(tb_renderer *renderer);

/* A draw of a mesh with a material, as tb_renderer_draw_mesh_material()
 * adds it. */
tb_renderer_draw tb_renderer_material_draw(const tb_mesh *mesh, tb_mat4 transform,
                                           const tb_material *material);

/* Write a draw's constants for the G-buffer to the frame's uploads, of its
 * material or, for a draw of a color, of a plain material of that color:
 * their offset, or UINT64_MAX when the uploads are full. */
uint64_t tb_renderer_material_upload(tb_renderer *renderer, const tb_renderer_draw *draw,
                                     tb_mat4 clip);

/* A texture's bindless index, and whether it is a cube map. */
uint32_t tb_texture_bindless_index(const tb_texture *texture);
bool tb_texture_is_cube(const tb_texture *texture);

/* The sky's pipeline (sky.c). */
tb_rhi_result tb_renderer_sky_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_sky_destroy(tb_renderer *renderer);

/* Write the sky's constants for the frame's camera, seen through the
 * viewport: their offset in the frame's uploads, or UINT64_MAX when there
 * is no sky to draw or no room. */
uint64_t tb_renderer_sky_upload(tb_renderer *renderer);

/* Draw the sky behind what the scene pass drew so far. */
void tb_renderer_sky_record(tb_renderer *renderer, tb_rhi_command_list *list, uint64_t constants);

/* Pack a light as the lighting shader reads it (light.c). */
tb_packed_light tb_light_pack(const tb_light *light);

/* The light of a frame without lights: toward the sun of a procedural sky
 * with one, else from above and a little behind the camera. */
tb_light tb_renderer_default_light(const tb_renderer *renderer);

/* The lighting mode's pipelines (light.c). */
tb_rhi_result tb_renderer_lighting_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_lighting_destroy(tb_renderer *renderer);

/* What the lighting pass reads, by bindless index. */
typedef struct tb_renderer_gbuffer_indices
{
    uint32_t base_color;
    uint32_t normal;
    uint32_t surface;
    uint32_t depth;
} tb_renderer_gbuffer_indices;

/* Write the lighting pass's constants, and the frame's lights (at most the
 * quality's, the directional first, then the nearest), to its uploads, for
 * the frame's camera seen through `clip`: their offset, or UINT64_MAX when
 * there is no room. */
uint64_t tb_renderer_lighting_upload(tb_renderer *renderer, tb_mat4 clip);

/* Light the G-buffer into the scene's color, inside the lighting pass. */
void tb_renderer_lighting_record(tb_renderer *renderer, tb_rhi_command_list *list,
                                 uint64_t constants, const tb_renderer_gbuffer_indices *gbuffer);

/* Tone map the scene's color, by its bindless index, into the scene's
 * rectangle of the output, inside the tonemap pass, filtered bilinearly
 * when its size is not the rectangle's. */
void tb_renderer_tonemap_record(tb_renderer *renderer, tb_rhi_command_list *list, uint32_t scene);

/* The i-th point of Halton's sequence of a base: the radical inverse of i,
 * from 0 to below 1 (temporal.c). */
float tb_halton(uint32_t index, uint32_t base);

/* A frame's sub-pixel jitter, from -0.5 to 0.5 of a pixel each way, x
 * right and y down: Halton's sequence of bases 2 and 3 from its second
 * point, eight points a cycle. */
tb_vec2 tb_temporal_jitter(uint32_t frame);

/* A projection moved by a jitter in pixels of a view of a size. */
tb_mat4 tb_temporal_jittered(tb_mat4 projection, tb_vec2 jitter, uint32_t width, uint32_t height);

/* The scene's size at a render scale of an output's: rounded, at least 1,
 * and at most `largest`. */
void tb_render_size_of(uint32_t width, uint32_t height, float scale, uint32_t largest,
                       uint32_t *render_width, uint32_t *render_height);

/* The capabilities of a device, as a profile asks (quality.c). */
tb_renderer_capabilities tb_renderer_capabilities_for(const tb_rhi_device_info *info,
                                                      tb_renderer_profile profile);

/* A quality within capabilities. */
tb_renderer_quality tb_renderer_quality_within(const tb_renderer_quality *asked,
                                               const tb_renderer_capabilities *capabilities);

/* Take the quality asked for and the scene's rectangle's size at the start
 * of a frame: the render size, and whether the history still holds. */
void tb_renderer_quality_begin_frame(tb_renderer *renderer);

/* The viewport of the swap chain's size, as the policy gives it, and the
 * scene's rectangle of its output. Without bindless resources, the
 * viewport render mode is drawn as the canvas mode: the viewport has no
 * target. */
void tb_renderer_update_viewport(tb_renderer *renderer);

/* The frame's camera's projection, unjittered, through the viewport: at
 * its aspect, the field of view given at the design's. Everything that
 * projects the scene (the scene itself, the sky, labels, temporal
 * anti-aliasing) projects through this one, so that none disagrees. */
tb_mat4 tb_renderer_projection(const tb_renderer *renderer);

/* Draw only into the scene's rectangle of its output, in a pass that draws
 * the 3D scene into it. */
void tb_renderer_cmd_viewport(const tb_renderer *renderer, tb_rhi_command_list *list);

/* The temporal pass's pipeline and sampler (temporal.c). */
tb_rhi_result tb_renderer_temporal_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_temporal_destroy(tb_renderer *renderer);

/* A frame's temporal anti-aliasing, as it is recorded. */
typedef struct tb_temporal_recording
{
    tb_renderer *renderer;
    uint64_t constants;            /* in the frame's uploads; UINT64_MAX for none */
    tb_render_graph_texture scene; /* what it reads */
    tb_render_graph_texture depth;
    tb_render_graph_texture history; /* the last frame's */
    tb_render_graph_texture output;  /* what it writes, the next frame's history */
} tb_temporal_recording;

/* Write the temporal pass's constants for a frame whose jitter and
 * unjittered clip are these: their offset in its uploads, or UINT64_MAX. */
uint64_t tb_renderer_temporal_upload(tb_renderer *renderer, tb_vec2 jitter, tb_mat4 clip);

/* Add the temporal pass, which reads `recording->scene` and `->depth`, to
 * the frame's graph: `recording->output` is what it writes, at the
 * output's size. `recording` lives until the graph is executed. */
void tb_renderer_temporal_record(tb_renderer *renderer, tb_temporal_recording *recording);

/* After the frame's graph is executed: the history written is the next
 * frame's to read. */
void tb_renderer_temporal_end_frame(tb_renderer *renderer, tb_mat4 clip);

/* The skinning pipeline (skinning.c), or, where it cannot be made, why the
 * CPU skins instead; logged. A GPU that cannot skin is a capability
 * missing, so this returns TB_RHI_OK. */
tb_rhi_result tb_renderer_skinning_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_skinning_destroy(tb_renderer *renderer);

/* Forget the last frame's skinned draws. */
void tb_renderer_skinning_begin_frame(tb_renderer *renderer);

/* Add the pass that skins the frame's skinned draws, before the scene
 * pass, which reads `recording->vertices`. `recording` lives until the
 * graph is executed. */
void tb_renderer_skinning_record(tb_renderer *renderer, tb_skinning_recording *recording);

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

/* Upload the frame's quads, in order, each target's together. */
void tb_renderer_canvas_upload(tb_renderer *renderer, tb_canvas_recording *recording);

/* Add the pass that draws the quads of a target over `image`, of `width`
 * by `height` pixels, when it has any. `recording` lives until the graph is
 * executed. */
void tb_renderer_canvas_record(tb_renderer *renderer, tb_canvas_recording *recording,
                               tb_canvas_target target, tb_render_graph_texture image,
                               uint32_t width, uint32_t height);

/* The viewport render mode's upscale (upscale.c): its pipeline, made with
 * the raster lighting mode's. */
tb_rhi_result tb_renderer_upscale_create(tb_renderer *renderer, tb_rhi_error *error);
void tb_renderer_upscale_destroy(tb_renderer *renderer);

/* The upscale of a frame being recorded. */
typedef struct tb_upscale_recording
{
    tb_renderer *renderer;
    tb_render_graph_texture target; /* what it reads */
} tb_upscale_recording;

/* Add the pass that clears `image` to the bar color and upscales the
 * viewport's target, `target`, into its rectangle of it: nearest-neighbour
 * at whole pixels a unit, linear otherwise. `recording` lives until the
 * graph is executed. */
void tb_renderer_upscale_record(tb_renderer *renderer, tb_upscale_recording *recording,
                                tb_render_graph_texture target, tb_render_graph_texture image);

/* Whether the upscale reads its target nearest-neighbour: in the integer
 * scale mode, at whole pixels a unit both ways, which a policy's scale that
 * is not whole can undo. Linear otherwise. */
bool tb_upscale_is_nearest(const tb_viewport_policy *policy, const tb_viewport *viewport);

#endif
