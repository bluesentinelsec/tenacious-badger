/** @file material.h
 * @brief Textures and materials for meshes: base color, normal, roughness
 * and metallic, from data a caller gives, mipmapped on the GPU and filtered
 * anisotropically.
 *
 * @code
 * uint8_t pixels[256 * 256 * 4] = {0};
 * tb_image_fill_checkerboard(pixels, sizeof(pixels), 256, 256, 32, (tb_vec4){1, 1, 1, 1},
 *                            (tb_vec4){0.1f, 0.1f, 0.1f, 1});
 * tb_texture *const checker = tb_texture_create(
 *     renderer, &(tb_texture_desc){.width = 256, .height = 256}, pixels, sizeof(pixels));
 *
 * tb_material_desc desc = TB_MATERIAL_DESC_INIT;
 * desc.base_color = checker;
 * desc.roughness = 0.6f;
 * tb_material *const floor = tb_material_create(renderer, &desc);
 *
 * // each frame, between tb_renderer_begin_frame() and tb_renderer_end_frame()
 * tb_renderer_draw_mesh_material(renderer, plane, tb_mat4_identity(), floor);
 * @endcode
 *
 * **Textures** are RGBA, 8 bits a channel, made from pixels in memory, rows
 * from the top. What they hold decides how they are read: colors
 * (#TB_TEXTURE_COLOR) are in sRGB and read as linear, as images are
 * authored; data and normals are read as they are. Every level of a
 * texture's mip chain is made on the GPU when it is created, by a compute
 * shader that halves each level into the next, averaging colors in linear
 * light and renormalizing normals.
 *
 * **Materials** are the textures and factors of a surface, after glTF's
 * metallic-roughness model: a base color (a texture times a factor), a
 * tangent-space normal map, and roughness and metallic (green and blue of a
 * texture, times factors). Each texture is optional. A material borrows its
 * textures: they outlive it.
 *
 * **Lighting.** A material is lit, for now, as every mesh is: a light from
 * above and behind the camera with ambient light, its normal map bending
 * the light, and its roughness and metallic shaping a highlight. The raster
 * lighting mode replaces it.
 *
 * **Filtering.** Materials are sampled trilinearly, repeating, and
 * anisotropically as the renderer's options and tb_renderer_set_anisotropy()
 * say: what the `graphics.anisotropic_filtering` setting holds.
 *
 * @par Threading
 * Stage-owned: every function is called on the thread that renders, as the
 * renderer's are, except those marked thread-safe.
 */
#ifndef TB_MATERIAL_H
#define TB_MATERIAL_H

#include "badger/api.h"
#include "badger/math.h"
#include "badger/renderer.h"
#include "badger/rhi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A texture that materials and skies sample. Made by
 * tb_texture_create(), tb_texture_create_cube() or tb_texture_from_rhi(); owned by the caller;
 * destroyed by tb_texture_destroy() after the materials that use it, before its renderer. */
typedef struct tb_texture tb_texture;

/** @brief A material. Made by tb_material_create(); owned by the caller;
 * destroyed by tb_material_destroy() before its renderer. */
typedef struct tb_material tb_material;

/** @brief What a texture's pixels hold, which decides how they are read. */
typedef enum tb_texture_content
{
    /** Colors in sRGB, as images are authored: read as linear, and averaged
     * in linear light into each level. Base colors. */
    TB_TEXTURE_COLOR,
    /** Values read as they are: roughness, metallic, masks. */
    TB_TEXTURE_DATA,
    /** Tangent-space normals, each channel from -1 at 0 to 1 at 255, +Z out
     * of the surface (0.5, 0.5, 1 is flat): renormalized in each level. */
    TB_TEXTURE_NORMAL
} tb_texture_content;

/** @brief A texture to make. Zero-initialize, then set what differs. */
typedef struct tb_texture_desc
{
    uint32_t width;             /**< Texels; more than 0. */
    uint32_t height;            /**< Texels; more than 0. */
    tb_texture_content content; /**< What its pixels hold. */
    bool no_mipmaps;            /**< Keep the first level alone; else make the full chain. */
    const char *name;           /**< For debuggers; may be NULL; copied. */
} tb_texture_desc;

/**
 * @brief Make a texture from RGBA pixels, 8 bits a channel, rows from the
 * top, and make its mip chain on the GPU. The texture is ready for the
 * frames after: its uploads are submitted before them.
 * @param renderer Required.
 * @param desc Required; a size the device makes.
 * @param pixels Required; `width * height * 4` bytes; copied.
 * @param size Bytes of `pixels`.
 * @return The texture; never NULL. Running out of GPU memory is fatal, as it
 * is for meshes and images.
 */
TB_API tb_texture *tb_texture_create(tb_renderer *renderer, const tb_texture_desc *desc,
                                     const uint8_t *pixels, size_t size);

/**
 * @brief Make a cube map from six square faces of RGBA pixels, one after
 * another in Direct3D's order: +X, -X, +Y, -Y, +Z, -Z. Each face is laid
 * out as Direct3D lays it, rows from the top, seen from the cube's middle:
 * the texel at (u, v), each from -1 at the face's left or top edge to 1 at
 * its right or bottom, is the direction (1, -v, -u) on +X, (-1, -v, u) on
 * -X, (u, 1, v) on +Y, (u, -1, -v) on -Y, (u, -v, 1) on +Z and (-u, -v, -1)
 * on -Z. Its mip chain is made on the GPU, as a texture's is: skies
 * (badger/sky.h), and reflections later.
 * @param renderer Required.
 * @param desc Required; `width` equal to `height`.
 * @param pixels Required; `width * height * 4 * 6` bytes; copied.
 * @param size Bytes of `pixels`.
 * @return The texture; never NULL.
 */
TB_API tb_texture *tb_texture_create_cube(tb_renderer *renderer, const tb_texture_desc *desc,
                                          const uint8_t *pixels, size_t size);

/**
 * @brief Make a texture of one a caller made through the RHI, for what the
 * renderer does not make itself: render targets, textures filled by the
 * caller's own shaders. The texture is borrowed: the caller keeps it alive
 * while the texture and its materials are, and destroys it after.
 * @param renderer Required.
 * @param texture Of the renderer's device; #TB_RHI_TEXTURE_SAMPLED, in
 * #TB_RHI_STATE_SHADER_READ whenever a frame that samples it is drawn. Its
 * format says whether it holds sRGB.
 * @return The texture; never NULL.
 */
TB_API tb_texture *tb_texture_from_rhi(tb_renderer *renderer, tb_rhi_texture texture);

/** @brief Destroy a texture, after the materials that use it and outside a
 * frame that draws them. A texture of tb_texture_from_rhi() leaves the
 * RHI's texture to its caller. NULL is ignored. */
TB_API void tb_texture_destroy(tb_renderer *renderer, tb_texture *texture);

/**
 * @brief A texture's size, and its levels.
 * @param texture Required.
 * @param width Required; receives the texels across.
 * @param height Required; receives the texels down.
 * @param mips May be NULL; receives the levels of its mip chain.
 */
TB_API void tb_texture_size(const tb_texture *texture, uint32_t *width, uint32_t *height,
                            uint32_t *mips);

/** @brief A material to make. Start from #TB_MATERIAL_DESC_INIT, whose
 * factors are those of a plain white, rough surface, then set what differs. */
typedef struct tb_material_desc
{
    /** Its color: a texture of #TB_TEXTURE_COLOR, or NULL for white. */
    const tb_texture *base_color;
    /** Multiplies the base color: linear RGB, and alpha (not blended yet). */
    tb_vec4 base_color_factor;
    /** Its tangent-space normals: a texture of #TB_TEXTURE_NORMAL, or NULL
     * for none. The tangents follow the mesh's texture coordinates, worked
     * out for each pixel. */
    const tb_texture *normal;
    /** How strongly the normal map bends the surface: 0 flat, 1 as made. */
    float normal_scale;
    /** Roughness in green and metallic in blue, as glTF has them: a texture
     * of #TB_TEXTURE_DATA, or NULL for white. */
    const tb_texture *metallic_roughness;
    float roughness;  /**< Multiplies the texture's roughness: 0 a mirror, 1 matte. */
    float metallic;   /**< Multiplies the texture's metallic: 0 dielectric, 1 metal. */
    tb_vec2 uv_scale; /**< Multiplies the mesh's texture coordinates: tiling. */
    /** Draw its base color alone, unlit, as #TB_MESH_FLAT draws a color:
     * screens, signs, and tests that read exact texels. */
    bool unlit;
    const char *name; /**< For debuggers; may be NULL. */
} tb_material_desc;

/** @brief A material description of a plain white, rough, non-metal
 * surface, with no textures. */
#define TB_MATERIAL_DESC_INIT                                                                      \
    {.base_color_factor = {1.0f, 1.0f, 1.0f, 1.0f},                                                \
     .normal_scale = 1.0f,                                                                         \
     .roughness = 1.0f,                                                                            \
     .uv_scale = {1.0f, 1.0f}}

/**
 * @brief Make a material.
 * @param renderer Required.
 * @param desc Required; its textures, if any, of this renderer, outliving
 * the material.
 * @return The material; never NULL.
 */
TB_API tb_material *tb_material_create(tb_renderer *renderer, const tb_material_desc *desc);

/** @brief Destroy a material, outside a frame that draws it. NULL is
 * ignored. */
TB_API void tb_material_destroy(tb_renderer *renderer, tb_material *material);

/**
 * @brief Draw a mesh with a material this frame. A device without
 * bindless resources draws it in the base color factor alone.
 * @param mesh Required.
 * @param transform From the mesh's space to the world's.
 * @param material Required; alive until the frame ends.
 */
TB_API void tb_renderer_draw_mesh_material(tb_renderer *renderer, const tb_mesh *mesh,
                                           tb_mat4 transform, const tb_material *material);

/**
 * @brief Filter materials anisotropically from the next frame on, as the
 * `graphics.anisotropic_filtering` setting says
 * (tb_engine_settings_anisotropy()).
 * @param anisotropy Texels a sample blends along a slope at most: 1 (off),
 * 2, 4, 8 or 16.
 */
TB_API void tb_renderer_set_anisotropy(tb_renderer *renderer, uint32_t anisotropy);

#endif
