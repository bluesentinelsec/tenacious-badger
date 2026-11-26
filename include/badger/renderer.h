/** @file renderer.h
 * @brief The renderer: frames drawn to a window from a camera, meshes, and
 * debug drawing, through the render graph and the RHI.
 *
 * @code
 * tb_renderer *renderer = NULL;
 * tb_rhi_error error = TB_RHI_ERROR_INIT;
 * (void)tb_renderer_create(&(tb_renderer_options){.window = window}, &renderer, &error);
 * tb_mesh *const cube = tb_mesh_create_shape(renderer, TB_MESH_CUBE, 0);
 *
 * // each frame
 * tb_camera camera = tb_camera_perspective(tb_radians(90.0f));
 * tb_camera_look_at(&camera, (tb_vec3){0, 2, 5}, (tb_vec3){0, 0, 0});
 * if (tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0.1f, 0.1f, 0.1f, 1}))
 * {
 *     tb_renderer_draw_mesh(renderer, cube, tb_mat4_identity(), (tb_vec4){1, 0.5f, 0, 1});
 *     tb_debug_grid(renderer, (tb_vec3){0, 0, 0}, 10.0f, 10, (tb_vec4){0.5f, 0.5f, 0.5f, 1}, 0);
 *     tb_renderer_end_frame(renderer);
 * }
 * @endcode
 *
 * **Frames.** A frame begins with a camera and a clear color, gathers what
 * is drawn, and ends by recording it through the render graph and showing
 * it. The swap chain follows the window's size in pixels by itself. Frames
 * the window does not show, when it is minimized, are skipped: begin says
 * so. Two frames are in flight at once, so the CPU prepares one while the
 * GPU draws the last.
 *
 * **Meshes** are triangles with positions, normals and texture coordinates,
 * made from data a caller gives or as shapes the renderer generates. A mesh
 * is drawn with a transform and a color, lit or flat, or with a material of
 * textures (badger/material.h).
 *
 * **The viewport.** The 3D scene is drawn into the viewport that the
 * display policy gives the frame (badger/viewport.h): a rectangle of the
 * swap chain, with bars of the policy's color beside it. The camera
 * projects at the viewport's aspect. Until a policy is set
 * (tb_renderer_set_viewport_policy()), the viewport is the whole frame.
 *
 * **Resolution and quality.** The scene renders at a share of the
 * viewport's resolution, the render scale, and temporal anti-aliasing
 * upsamples it to the viewport's, as Unreal's TAAU does; the canvas and the
 * debug lines are drawn at the output's. A tb_renderer_quality holds what the quality
 * settings set (tb_renderer_quality_from_settings()). On a software
 * adapter, or a device without what the full path needs, the renderer
 * takes a low profile: a lower render scale and the costly features off
 * (tb_renderer_capabilities_of()).
 *
 * **Lighting.** On a device with bindless resources, frames are drawn in
 * the raster lighting mode: meshes into a G-buffer, lit by the scene's
 * lights (badger/light.h) and its sky (badger/sky.h) with Unreal's default
 * lit model, and tone mapped from linear HDR into the swap chain by an ACES
 * filmic curve and sRGB's encoding, so that the colors given are light, not
 * pixels. Without bindless resources, meshes are drawn straight into the
 * swap chain, lit by one light.
 *
 * **Debug drawing** puts lines in the world for one frame: lines, boxes,
 * spheres, grids, axes and arrows, tested against depth or drawn over
 * everything. It may be called from any thread between a frame's begin and
 * its end.
 *
 * **Labels** (tb_debug_text()) are text at a point in the world, drawn by
 * the canvas over everything, in the debug font that
 * tb_renderer_set_debug_font() chooses, or else in the font of the UI role
 * of the engine's font catalog (badger/font_catalog.h).
 *
 * **Conventions** are the engine's (docs/math.md): right-handed, Y up, a
 * camera looking down −Z, and reversed depth.
 *
 * @par Threading
 * Stage-owned: every function is called on the thread that renders, except
 * the tb_debug_ functions, which are thread-safe.
 */
#ifndef TB_RENDERER_H
#define TB_RENDERER_H

#include "badger/api.h"
#include "badger/math.h"
#include "badger/rhi.h"
#include "badger/text.h"
#include "badger/viewport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_engine_settings;
struct tb_font;
struct tb_log;
struct tb_shader_cache;
struct tb_vfs;
struct tb_window;

/** @brief A renderer. Created by tb_renderer_create(); owned by the caller;
 * destroyed by tb_renderer_destroy() before its window. */
typedef struct tb_renderer tb_renderer;

/** @brief A mesh. Made by tb_mesh_create() or tb_mesh_create_shape(); owned
 * by the caller; destroyed by tb_mesh_destroy() before its renderer. */
typedef struct tb_mesh tb_mesh;

/** @brief How much a renderer spends on its frames: what it may draw, as
 * the profile allows. */
typedef enum tb_renderer_profile
{
    /** Full, unless the device runs on the CPU (a software adapter such as
     * WARP) or lacks the bindless resources the raster lighting mode needs:
     * then low. */
    TB_RENDERER_PROFILE_AUTOMATIC,
    TB_RENDERER_PROFILE_FULL, /**< Whatever the quality asks. */
    /** At most #TB_RENDERER_LOW_RENDER_SCALE of the output's resolution, and
     * no temporal anti-aliasing, reflections, or more than
     * #TB_RENDERER_LOW_LIGHTS lights: for GPUs that run on the CPU. */
    TB_RENDERER_PROFILE_LOW
} tb_renderer_profile;

/** @brief The render scale of the low profile at most. */
#define TB_RENDERER_LOW_RENDER_SCALE 0.5f
/** @brief The lights the low profile shades at most. */
#define TB_RENDERER_LOW_LIGHTS 16u

/** @brief How a renderer is made. Zero-initialize, then set what differs. */
typedef struct tb_renderer_options
{
    struct tb_window *window;     /**< Required; outlives the renderer. */
    tb_rhi_device_options device; /**< The device: backend, adapter, validation. */
    bool vsync;                   /**< Wait for the display's refresh. */
    /** Texels a material's sample blends along a slope at most: 1 (off), 2,
     * 4, 8 or 16, as `graphics.anisotropic_filtering` says
     * (tb_engine_settings_anisotropy()); 0 is 8, the setting's default. */
    uint32_t anisotropy;
    /** Where messages go, the RHI's validation included; NULL drops them.
     * Borrowed until the renderer is destroyed. */
    struct tb_log *log;
    /** Pixels a side of the glyph atlas, the texture the canvas draws text
     * from: from 16 to the device's largest texture; 0 for 1024. */
    uint32_t glyph_atlas_size;
    /** What it may spend: automatic, the default, takes the low profile on
     * a software adapter. Tests of the full path on WARP ask for full. */
    tb_renderer_profile profile;
    /** Where the renderer finds the shader library of its device's backend,
     * `Shaders/<backend>.tbsl` (`Shaders/d3d12.tbsl`), which `badger-shaderc`
     * writes (badger/shader_library.h). Its shaders are found first, and
     * need no compiler. NULL, or a VFS without the library, finds none
     * there. Borrowed for the call. */
    struct tb_vfs *shader_vfs;
    /** The shader cache (badger/shader_cache.h), searched for the shaders
     * the library lacks, and given those compiled; NULL for none. Borrowed
     * until the renderer is destroyed. */
    struct tb_shader_cache *shader_cache;
    /** Use the library's shaders only, compiling none at run time, as
     * `shaders.runtime_compile` off asks of a shipped game: a shader the
     * library lacks makes tb_renderer_create() fail. */
    bool shader_library_only;
} tb_renderer_options;

/** @brief What a renderer's frames are made with: the renderer's values of
 * the quality settings. Start from #TB_RENDERER_QUALITY_INIT. */
typedef struct tb_renderer_quality
{
    /** The scene's resolution over the output's, each way: from 0.25 to 2.
     * Below 1, temporal anti-aliasing upsamples it, or, without, a bilinear
     * filter. The canvas is drawn at the output's. */
    float render_scale;
    /** Temporal anti-aliasing: each frame's projection jittered within a
     * pixel, and blended with the frames before. */
    bool temporal_aa;
    /** The lights shaded a frame at most, from 1: the directional lights
     * first, then the nearest to the camera. */
    uint32_t max_lights;
    /** Smooth surfaces reflect the sky. */
    bool reflections;
} tb_renderer_quality;

/** @brief The full quality: the output's resolution, temporal
 * anti-aliasing, 256 lights, and reflections. */
#define TB_RENDERER_QUALITY_INIT                                                                   \
    {.render_scale = 1.0f, .temporal_aa = true, .max_lights = 256u, .reflections = true}

/** @brief What a renderer can draw, and why not, as its profile decides. */
typedef struct tb_renderer_capabilities
{
    tb_renderer_profile profile; /**< Full or low; never automatic. */
    /** The raster lighting mode: deferred lights, the sky's light, tone
     * mapping, and temporal anti-aliasing. Without bindless resources, a
     * mesh is lit by one light. */
    bool raster_lighting;
    float max_render_scale; /**< The render scale at most. */
    bool temporal_aa;       /**< Temporal anti-aliasing can run. */
    uint32_t max_lights;    /**< The lights shaded a frame at most. */
    bool reflections;       /**< Reflections of the sky can run. */
    /** Why the profile is low, in UTF-8, for the console and an options
     * menu; empty for full. */
    char reason[128];
} tb_renderer_capabilities;

/** @brief How a camera projects. */
typedef enum tb_camera_projection
{
    TB_CAMERA_PERSPECTIVE, /**< Things farther away are smaller: 3D games. */
    TB_CAMERA_ORTHOGRAPHIC /**< Sizes do not change with distance: 2D, isometric and CAD views. */
} tb_camera_projection;

/**
 * @brief Which of a camera's extents holds when the view's aspect is not
 * the one its field of view is given at, after Unreal's
 * `EAspectRatioAxisConstraint`.
 *
 * A camera's field of view (`vertical_fov`, or an orthographic `height`)
 * is given at a **reference aspect**: the design's, which the renderer
 * takes from the viewport (tb_viewport_design_aspect()). A view of another
 * aspect, such as a 16:10 frame showing a 16:9 design in `expand`, keeps
 * one extent of the reference view and gives the other what the aspect
 * leaves. Without a reference, every constraint keeps the vertical extent.
 */
typedef enum tb_camera_axis_constraint
{
    /** The view always holds the whole reference view: a wider view sees
     * more across (Hor+), a taller one more down (Vert+). The default, so
     * that what a game is designed to show is shown on every display. */
    TB_CAMERA_AXIS_DESIGN,
    /** The vertical extent holds at every aspect: a taller view sees less
     * across. Unreal's `MaintainYFOV`. */
    TB_CAMERA_AXIS_VERTICAL,
    /** The horizontal extent of the reference view holds at every aspect:
     * a wider view sees less down. Unreal's `MaintainXFOV`. */
    TB_CAMERA_AXIS_HORIZONTAL,
    TB_CAMERA_AXIS_COUNT /**< The number of constraints. */
} tb_camera_axis_constraint;

/** @brief A camera: where it is, which way it looks, and how it projects. */
typedef struct tb_camera
{
    tb_vec3 position;                /**< In the world. */
    tb_quat orientation;             /**< Its rotation from looking down −Z with +Y up. */
    tb_camera_projection projection; /**< Perspective or orthographic. */
    /** Perspective: radians from the bottom of the view to its top, at the
     * reference aspect. */
    float vertical_fov;
    /** Orthographic: the world's height that the view shows, at the
     * reference aspect. */
    float height;
    float near_plane; /**< Distance to the nearest drawn; more than 0 for perspective. */
    float far_plane;  /**< Distance to the farthest drawn; 0 for none (perspective). */
    /** Which extent holds at another aspect than the reference's; zero is
     * #TB_CAMERA_AXIS_DESIGN. */
    tb_camera_axis_constraint axis_constraint;
} tb_camera;

/**
 * @brief A perspective camera at the origin looking down −Z, near plane at
 * 0.1, with no far plane.
 * @param vertical_fov Radians, more than 0 and less than π; from
 * `graphics.field_of_view`, which settings give in degrees.
 * @par Threading Thread-safe.
 */
TB_API tb_camera tb_camera_perspective(float vertical_fov);

/**
 * @brief An orthographic camera at the origin looking down −Z, showing
 * `height` of the world from top to bottom, from −1000 to 1000 deep.
 * @par Threading Thread-safe.
 */
TB_API tb_camera tb_camera_orthographic(float height);

/**
 * @brief Place a camera at `eye`, looking at `target` with +Y up.
 * @param camera Required.
 * @return false, with the camera unchanged, when the two are the same point
 * or the view is straight up or down.
 * @par Threading Thread-safe.
 */
TB_API bool tb_camera_look_at(tb_camera *camera, tb_vec3 eye, tb_vec3 target);

/** @brief The camera's view matrix: the world as the camera sees it.
 * @par Threading Thread-safe. */
TB_API tb_mat4 tb_camera_view(const tb_camera *camera);

/** @brief The camera's projection for a view of an aspect (width over height),
 * without a reference aspect: the vertical extent holds, whatever the
 * camera's axis constraint. tb_camera_projection_fit() with a reference.
 * @par Threading Thread-safe. */
TB_API tb_mat4 tb_camera_projection_of(const tb_camera *camera, float aspect);

/**
 * @brief The camera's projection for a view of an aspect, its field of
 * view given at a reference aspect and held by its axis constraint: what
 * the renderer draws the scene, the sky, labels and temporal
 * anti-aliasing's jitter with, the aspect tb_viewport_aspect_of() and the
 * reference tb_viewport_design_aspect() of its viewport.
 * @param camera Required.
 * @param aspect The view's, width over height; more than 0.
 * @param reference_aspect The aspect the field of view is given at; 0 for
 * none, which keeps the vertical extent.
 * @par Threading Thread-safe.
 */
TB_API tb_mat4 tb_camera_projection_fit(const tb_camera *camera, float aspect,
                                        float reference_aspect);

/**
 * @brief Half the extents of the view that tb_camera_projection_fit()
 * gives, across and down: for a perspective camera, the tangents of half
 * its horizontal and vertical fields of view, the view's half size a unit
 * ahead; for an orthographic one, half the world's width and height shown.
 * @param camera Required.
 * @param aspect As tb_camera_projection_fit().
 * @param reference_aspect As tb_camera_projection_fit().
 * @par Threading Thread-safe.
 */
TB_API tb_vec2 tb_camera_half_extent(const tb_camera *camera, float aspect,
                                     float reference_aspect);

/** @brief A constraint's name: "design", "vertical" or "horizontal";
 * "unknown" for another value. @par Threading Thread-safe. */
TB_API const char *tb_camera_axis_constraint_name(tb_camera_axis_constraint constraint);

/**
 * @brief Make a renderer: a device, a swap chain on the window, and the
 * shaders it draws with.
 * @param options Required.
 * @param renderer Required; set to the renderer, or to NULL on failure.
 * @param error May be NULL; on failure, says why.
 * @return #TB_RHI_OK, or why there is no renderer: no backend, no shader
 * compiler for it, or no swap chain.
 */
TB_API tb_rhi_result tb_renderer_create(const tb_renderer_options *options, tb_renderer **renderer,
                                        tb_rhi_error *error);

/** @brief Destroy a renderer, waiting for the GPU. NULL is ignored. */
TB_API void tb_renderer_destroy(tb_renderer *renderer);

/** @brief The renderer's device, for what draws through the RHI itself. */
TB_API tb_rhi_device *tb_renderer_device(tb_renderer *renderer);

/**
 * @brief The size of the frames in pixels: the window's, as of the last
 * tb_renderer_begin_frame() that drew, or the renderer's creation.
 * @param width Required; receives the pixels across.
 * @param height Required; receives the pixels down.
 */
TB_API void tb_renderer_size(const tb_renderer *renderer, uint32_t *width, uint32_t *height);

/**
 * @brief The render graph the renderer records its frames through: for
 * tb_render_graph_dump(), to see the last frame's passes and barriers.
 * @return The renderer's; valid until it is destroyed. Read it outside a
 * frame, on the thread that renders.
 */
TB_API const struct tb_render_graph *tb_renderer_graph(const tb_renderer *renderer);

/** @brief Wait for the display's refresh from the next frame on, or not. */
TB_API void tb_renderer_set_vsync(tb_renderer *renderer, bool vsync);

/**
 * @brief Place the 3D scene by a display policy from now on: the viewport
 * of each frame follows from it, the frame's size, the display's scale, and
 * the player's aspect ratio. A renderer begins with
 * #TB_VIEWPORT_POLICY_LOGICAL, whose viewport is the whole frame; an app
 * gives the policy its settings describe (tb_viewport_policy_from_settings()).
 *
 * In the viewport render mode, the 3D scene, its debug lines, and what the
 * canvas draws in views of the viewport's target are drawn into that
 * target, a pixel a unit, which is then upscaled into the viewport; what
 * the canvas draws in views of the frame is drawn over it, at the frame's
 * pixels. A device without bindless resources draws the canvas mode
 * instead, which the renderer logs once.
 *
 * @param policy Required; copied.
 * @param aspect_ratio The player's, width over height, from
 * tb_viewport_aspect_ratio_from_settings(); 0 for none.
 */
TB_API void tb_renderer_set_viewport_policy(tb_renderer *renderer, const tb_viewport_policy *policy,
                                            float aspect_ratio);

/** @brief The display policy in use (tb_renderer_set_viewport_policy()).
 * @param policy Required; receives it. */
TB_API void tb_renderer_viewport_policy(const tb_renderer *renderer, tb_viewport_policy *policy);

/**
 * @brief The viewport: where the 3D scene is drawn, and where an app's units
 * are, for the canvas (tb_canvas_view_viewport()), the GUI
 * (tb_gui_set_viewport()) and the mouse. Of the frame's size as of the last
 * tb_renderer_begin_frame() that drew, or the renderer's creation, and the
 * policy set since. Its target is of the size the device makes, and none
 * where the device draws the viewport render mode as the canvas mode.
 */
TB_API tb_viewport tb_renderer_viewport(const tb_renderer *renderer);

/**
 * @brief Ask for a quality from the next frame on. The renderer takes what
 * its profile allows of it (tb_renderer_quality_of()); a change of the
 * render scale or of temporal anti-aliasing starts its history again.
 * @param quality Required; copied. A render scale from 0.25 to 2, and at
 * least one light.
 */
TB_API void tb_renderer_set_quality(tb_renderer *renderer, const tb_renderer_quality *quality);

/**
 * @brief The quality the renderer draws with: what was asked, within what
 * its profile allows.
 * @param quality Required; receives it.
 */
TB_API void tb_renderer_quality_of(const tb_renderer *renderer, tb_renderer_quality *quality);

/**
 * @brief What the renderer can draw: its profile, what the profile allows,
 * and why it is low, which the renderer also logs when it is made.
 * @param capabilities Required; receives them.
 */
TB_API void tb_renderer_capabilities_of(const tb_renderer *renderer,
                                        tb_renderer_capabilities *capabilities);

/**
 * @brief The size the scene renders at, in pixels: the viewport's times the
 * render scale in effect, at least 1. As of the last tb_renderer_begin_frame()
 * that drew, or the renderer's creation.
 * @param width Required; receives the pixels across.
 * @param height Required; receives the pixels down.
 */
TB_API void tb_renderer_render_size(const tb_renderer *renderer, uint32_t *width, uint32_t *height);

/**
 * @brief Say that the next frame does not follow from the last: a cut to
 * another camera, or a teleport. Temporal anti-aliasing then forgets its
 * history, which would otherwise ghost. A change of the viewport's size, of
 * the render scale or of the camera's projection forgets it by itself.
 */
TB_API void tb_renderer_camera_cut(tb_renderer *renderer);

/**
 * @brief The quality the engine's settings describe, as Unreal's
 * scalability groups set renderer values: the render scale from
 * `graphics.upscaler_mode` (native 1, quality 2/3, balanced 0.58,
 * performance 1/2, ultra_performance 1/3, custom `graphics.render_scale`);
 * temporal anti-aliasing when it upsamples or `graphics.anti_aliasing` is
 * taa; lights from `graphics.light_quality` (16, 64, 256, 1024); and
 * reflections unless `graphics.reflection_quality` is low.
 * @param engine Required; the engine's settings.
 * @param quality Required; receives the quality.
 * @par Threading Thread-safe, as reading settings is.
 */
TB_API void tb_renderer_quality_from_settings(const struct tb_engine_settings *engine,
                                              tb_renderer_quality *quality);

/**
 * @brief Begin a frame seen through a camera.
 * @param camera Required; copied.
 * @param clear The color behind everything in the viewport; the bars beside
 * it are the policy's color.
 * @return true to draw the frame and end it; false when the window shows
 * nothing, as when it is minimized: then draw nothing and do not end it.
 */
TB_API bool tb_renderer_begin_frame(tb_renderer *renderer, const tb_camera *camera, tb_vec4 clear);

/**
 * @brief End the frame: record what was drawn, and show it.
 * @return #TB_RHI_OK, or #TB_RHI_DEVICE_LOST.
 */
TB_API tb_rhi_result tb_renderer_end_frame(tb_renderer *renderer);

/**
 * @brief Save the next frame's pixels when it ends, for tests and
 * screenshots: tb_renderer_captured() gives them after.
 */
TB_API void tb_renderer_capture_next_frame(tb_renderer *renderer);

/**
 * @brief The pixels the last captured frame ended with: BGRA, 8 bits each,
 * rows from the top, `width * 4` bytes apart.
 * @param width Required; receives the width in pixels, 0 if none was captured.
 * @param height Required; receives the height in pixels.
 * @return The pixels, owned by the renderer, good until the next capture or
 * its destruction; NULL if none was captured.
 */
TB_API const uint8_t *tb_renderer_captured(const tb_renderer *renderer, uint32_t *width,
                                           uint32_t *height);

/** @brief A vertex of a mesh. */
typedef struct tb_mesh_vertex
{
    tb_vec3 position; /**< In the mesh's space. */
    tb_vec3 normal;   /**< Unit length, outward. */
    tb_vec2 uv;       /**< Texture coordinates. */
} tb_mesh_vertex;

/** @brief Shapes the renderer generates. Each fits in a unit box centered
 * on the origin, its normals outward, its triangles counter-clockwise seen
 * from outside. */
typedef enum tb_mesh_shape
{
    TB_MESH_CUBE,     /**< A cube of side 1, each face with its own normals. */
    TB_MESH_PLANE,    /**< A square of side 1 in XZ, facing +Y. */
    TB_MESH_SPHERE,   /**< A sphere of diameter 1. */
    TB_MESH_CYLINDER, /**< A capped cylinder of diameter 1 along Y. */
    TB_MESH_CONE,     /**< A capped cone of diameter 1 along Y, its tip up. */
    TB_MESH_TORUS,    /**< A torus of diameter 1 around Y, its tube 0.25 across. */
    TB_MESH_CAPSULE   /**< A capsule of diameter 0.5 and height 1 along Y. */
} tb_mesh_shape;

/**
 * @brief Make a mesh from triangles.
 * @param vertices Required; `vertex_count` of them; copied.
 * @param indices Required; `index_count` of them, three to a triangle,
 * counter-clockwise seen from its front; copied.
 * @return The mesh; never NULL.
 */
TB_API tb_mesh *tb_mesh_create(tb_renderer *renderer, const tb_mesh_vertex *vertices,
                               size_t vertex_count, const uint32_t *indices, size_t index_count);

/**
 * @brief Make a mesh of a shape.
 * @param detail Segments around a round shape; 0 is 32. A cube and a plane
 * ignore it.
 * @return The mesh; never NULL.
 */
TB_API tb_mesh *tb_mesh_create_shape(tb_renderer *renderer, tb_mesh_shape shape, uint32_t detail);

/**
 * @brief Generate a shape's triangles without making a mesh, for a caller
 * that keeps its own.
 * @param vertices Receives `*vertex_count` vertices from tb_malloc(); the
 * caller frees them with tb_free().
 * @param indices Receives `*index_count` indices from tb_malloc(); likewise.
 * @par Threading Thread-safe.
 */
TB_API void tb_mesh_generate(tb_mesh_shape shape, uint32_t detail, tb_mesh_vertex **vertices,
                             size_t *vertex_count, uint32_t **indices, size_t *index_count);

/** @brief Destroy a mesh. NULL is ignored. */
TB_API void tb_mesh_destroy(tb_renderer *renderer, tb_mesh *mesh);

/** @brief How a mesh is shaded. */
typedef enum tb_mesh_shading
{
    TB_MESH_LIT, /**< Lit by the scene's lights and its ambient light. */
    TB_MESH_FLAT /**< Its color alone. */
} tb_mesh_shading;

/**
 * @brief Draw a mesh this frame.
 * @param mesh Required.
 * @param transform From the mesh's space to the world's.
 * @param color Linear RGB and alpha; alpha below 1 is not blended yet.
 */
TB_API void tb_renderer_draw_mesh(tb_renderer *renderer, const tb_mesh *mesh, tb_mat4 transform,
                                  tb_vec4 color);

/** @brief Draw a mesh this frame, shaded as asked. As tb_renderer_draw_mesh(). */
TB_API void tb_renderer_draw_mesh_shaded(tb_renderer *renderer, const tb_mesh *mesh,
                                         tb_mat4 transform, tb_vec4 color, tb_mesh_shading shading);

/** @brief What a custom draw is given, inside the frame's pass of what is
 * drawn as it is after lighting, or, without bindless resources, its scene
 * pass. */
typedef struct tb_renderer_draw_context
{
    tb_rhi_device *device;     /**< The renderer's device. */
    tb_rhi_command_list *list; /**< To record into, inside the pass. */
    tb_mat4 view;              /**< The frame's camera's view. */
    tb_mat4 projection;        /**< Its projection, for the frame's aspect. */
    tb_mat4 clip;              /**< `projection * view`. */
    uint32_t width;            /**< The frame's size, in pixels. */
    uint32_t height;           /**< See `width`. */
    void *context;             /**< The caller's. */
} tb_renderer_draw_context;

/** @brief Records a custom draw. */
typedef void tb_renderer_draw_function(const tb_renderer_draw_context *draw);

/**
 * @brief Record a draw of the caller's own in this frame, after the meshes
 * are lit and before the sky and the debug lines: what draws through the
 * RHI itself with pipelines of tb_renderer_target_formats(). Its colors
 * are light, in linear HDR, which the output transform maps as the
 * scene's; it tests and writes the scene's depth.
 * @param draw Required; called once, while the frame ends.
 * @param context Given to `draw`.
 */
TB_API void tb_renderer_draw_custom(tb_renderer *renderer, tb_renderer_draw_function *draw,
                                    void *context);

/**
 * @brief The formats of the targets custom draws draw into, which their
 * pipelines use: the scene's light (#TB_RHI_FORMAT_R16G16B16A16_FLOAT) in
 * the raster lighting mode, else the swap chain's; and depth, which is
 * reversed: clear to 0, keep what is greater.
 * @param color Required; receives the color target's format.
 * @param depth Required; receives the depth target's format.
 */
TB_API void tb_renderer_target_formats(const tb_renderer *renderer, tb_rhi_format *color,
                                       tb_rhi_format *depth);

/** @brief Debug drawing over everything, not hidden by what is nearer. */
#define TB_DEBUG_ON_TOP 1u

/** @brief A line from `a` to `b` this frame.
 * @param flags 0, or #TB_DEBUG_ON_TOP.
 * @par Threading Thread-safe. */
TB_API void tb_debug_line(tb_renderer *renderer, tb_vec3 a, tb_vec3 b, tb_vec4 color,
                          unsigned flags);

/** @brief The edges of a box, `half_extents` from its center along each
 * axis, turned by `transform` (the identity for one aligned to the world).
 * @par Threading Thread-safe. */
TB_API void tb_debug_box(tb_renderer *renderer, tb_mat4 transform, tb_vec3 half_extents,
                         tb_vec4 color, unsigned flags);

/** @brief A sphere, as three circles around its axes.
 * @par Threading Thread-safe. */
TB_API void tb_debug_sphere(tb_renderer *renderer, tb_vec3 center, float radius, tb_vec4 color,
                            unsigned flags);

/** @brief A square grid in XZ around `center`, `size` across, of
 * `divisions` cells a side.
 * @par Threading Thread-safe. */
TB_API void tb_debug_grid(tb_renderer *renderer, tb_vec3 center, float size, uint32_t divisions,
                          tb_vec4 color, unsigned flags);

/** @brief The axes of a transform, `size` long: X red, Y green, Z blue.
 * @par Threading Thread-safe. */
TB_API void tb_debug_axes(tb_renderer *renderer, tb_mat4 transform, float size, unsigned flags);

/** @brief An arrow from `from` to `to`, its head a fifth of its length.
 * @par Threading Thread-safe. */
TB_API void tb_debug_arrow(tb_renderer *renderer, tb_vec3 from, tb_vec3 to, tb_vec4 color,
                           unsigned flags);

/**
 * @brief Choose the font labels are drawn in (tb_debug_text()). Until one
 * is chosen, labels are drawn at 14 units in the font of the UI role of the
 * engine's font catalog, opened the first time a label is drawn; in a
 * build that carries no fonts, they are not drawn.
 * @param font Open while the renderer draws with it; NULL for the default.
 * Call tb_canvas_forget_font() before closing it.
 * @param size Its size in the window's logical units to the em; more than 0.
 */
TB_API void tb_renderer_set_debug_font(tb_renderer *renderer, const struct tb_font *font,
                                       float size);

/** @brief A label at a point in the world this frame: text centered on
 * where the point is drawn, over everything, in the debug font. A point
 * behind the camera is not drawn.
 * @param text UTF-8; copied.
 * @par Threading Thread-safe. */
TB_API void tb_debug_text(tb_renderer *renderer, tb_vec3 position, tb_text text, tb_vec4 color);

#endif
