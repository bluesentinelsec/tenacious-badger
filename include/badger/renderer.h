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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/** @brief How a camera projects. */
typedef enum tb_camera_projection
{
    TB_CAMERA_PERSPECTIVE, /**< Things farther away are smaller: 3D games. */
    TB_CAMERA_ORTHOGRAPHIC /**< Sizes do not change with distance: 2D, isometric and CAD views. */
} tb_camera_projection;

/** @brief A camera: where it is, which way it looks, and how it projects. */
typedef struct tb_camera
{
    tb_vec3 position;                /**< In the world. */
    tb_quat orientation;             /**< Its rotation from looking down −Z with +Y up. */
    tb_camera_projection projection; /**< Perspective or orthographic. */
    float vertical_fov; /**< Perspective: radians from the bottom of the view to its top. */
    float height;       /**< Orthographic: the world's height that the view shows. */
    float near_plane;   /**< Distance to the nearest drawn; more than 0 for perspective. */
    float far_plane;    /**< Distance to the farthest drawn; 0 for none (perspective). */
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

/** @brief The camera's projection for a view of an aspect (width over height).
 * @par Threading Thread-safe. */
TB_API tb_mat4 tb_camera_projection_of(const tb_camera *camera, float aspect);

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
 * @brief Begin a frame seen through a camera.
 * @param camera Required; copied.
 * @param clear The color behind everything.
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
