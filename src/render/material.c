/* Textures and materials: pixels uploaded and their mip chains made by a
 * compute shader, the sampler that filters them as the settings say, and
 * the pipeline that draws a mesh into the G-buffer with a material's
 * textures, reached by their bindless indices. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/log.h"
#include "badger/memory.h"

#include <string.h>

/* The anisotropy a renderer filters with unless its options say. */
#define TB_MATERIAL_DEFAULT_ANISOTROPY 8u
/* The most texels a sample blends along a slope. */
#define TB_MATERIAL_MAX_ANISOTROPY 16u
/* Texels a side of the group of threads that makes a level. */
#define TB_MIP_GROUP 8u

struct tb_texture
{
    tb_renderer_source source;
    uint8_t *pixels; /* packed base level, all faces */
    tb_texture_content content;
    tb_rhi_texture texture;
    bool owned; /* made here, not borrowed from a caller */
    bool cube;  /* six faces, sampled as a TextureCube */
    uint32_t width;
    uint32_t height;
    uint32_t mips;
    uint32_t index; /* the texture's bindless index */
};

struct tb_material
{
    uint32_t base_color; /* bindless indices of its textures */
    uint32_t normal;
    uint32_t metallic_roughness;
    tb_vec4 base_color_factor;
    float normal_scale;
    float roughness;
    float metallic;
    tb_vec2 uv_scale;
    bool unlit;
};

/* A draw's constants, as the material shader's MaterialConstants lays
 * them out. */
typedef struct tb_material_constants
{
    tb_mat4 clip;
    tb_mat4 model;
    tb_vec4 color;        /* the base color factor */
    tb_vec4 surface;      /* roughness, metallic, normal scale, and 1 for lit or 0 for unlit */
    tb_vec4 uv;           /* the scale of the texture coordinates */
    uint32_t textures[4]; /* base color, normal, metallic-roughness, sampler */
} tb_material_constants;

TB_STATIC_ASSERT(sizeof(tb_material_constants) <= 256u, "a draw's constants in 256 bytes");

/* A level's making, as the mip shader's MipConstants lays it out. */
typedef struct tb_mip_constants
{
    uint32_t source; /* the storage index of the level before */
    uint32_t dest;   /* the storage index of the level made */
    uint32_t width;  /* of the level made */
    uint32_t height;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t content; /* a tb_texture_content */
    uint32_t array;   /* 1 when the texture has layers, and is written as an array */
} tb_mip_constants;

/* ---------------------------------------------------------------------------
 * Textures.
 * ------------------------------------------------------------------------- */

static bool tb_texture_can_make_mips(const tb_renderer *renderer)
{
    return renderer->mip_pipeline.generation != 0;
}

/* Make each level after the first from the one before. The texture is in
 * TB_RHI_STATE_STORAGE, and stays there. */
static void tb_texture_record_mips(tb_renderer *renderer, tb_rhi_command_list *list,
                                   const tb_texture *texture, tb_texture_content content,
                                   uint32_t layers)
{
    tb_rhi_cmd_pipeline(list, renderer->mip_pipeline);
    for (uint32_t mip = 1; mip < texture->mips; mip++)
    {
        const tb_mip_constants constants = {
            .source = tb_rhi_texture_storage_mip_index(renderer->device, texture->texture, mip - 1),
            .dest = tb_rhi_texture_storage_mip_index(renderer->device, texture->texture, mip),
            .width = tb_rhi_mip_size(texture->width, mip),
            .height = tb_rhi_mip_size(texture->height, mip),
            .source_width = tb_rhi_mip_size(texture->width, mip - 1),
            .source_height = tb_rhi_mip_size(texture->height, mip - 1),
            .content = (uint32_t)content,
            .array = layers > 1 ? 1u : 0u};
        tb_rhi_cmd_push_constants(list, &constants, sizeof(constants));
        tb_rhi_cmd_dispatch(list, (constants.width + TB_MIP_GROUP - 1u) / TB_MIP_GROUP,
                            (constants.height + TB_MIP_GROUP - 1u) / TB_MIP_GROUP, layers);
        /* The next level reads what this one wrote. */
        tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture->texture,
                                                   .before = TB_RHI_STATE_STORAGE,
                                                   .after = TB_RHI_STATE_STORAGE});
    }
}

/* Copy each layer's pixels into the texture's first level, make the levels
 * after, and leave it to be sampled. The copy is submitted at once, before
 * any frame that draws it. */
static tb_rhi_result tb_texture_upload(tb_renderer *renderer, const tb_texture *texture,
                                       tb_texture_content content, uint32_t layers,
                                       const uint8_t *pixels, tb_rhi_error *error)
{
    const size_t row = (size_t)texture->width * 4u;
    const size_t pitch = (row + TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u) &
                         ~(size_t)(TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT - 1u);
    const size_t layer_bytes =
        (pitch * texture->height + TB_RHI_TEXTURE_COPY_OFFSET_ALIGNMENT - 1u) &
        ~(size_t)(TB_RHI_TEXTURE_COPY_OFFSET_ALIGNMENT - 1u);
    const bool mips = texture->mips > 1;
    uint8_t *staged = tb_calloc(layers, layer_bytes);
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;

    for (uint32_t layer = 0; layer < layers; layer++)
    {
        for (uint32_t y = 0; y < texture->height; y++)
        {
            memcpy(&staged[layer_bytes * layer + pitch * y],
                   &pixels[row * ((size_t)texture->height * layer + y)], row);
        }
    }
    const tb_rhi_result result =
        tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = (uint64_t)layer_bytes * layers,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "texture pixels"},
                             &staging, error);
    if (result != TB_RHI_OK)
    {
        tb_free(staged);
        return result;
    }
    tb_rhi_buffer_write(renderer->device, staging, 0, staged, layer_bytes * layers);
    tb_free(staged);
    list = tb_rhi_command_list_begin(renderer->device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture->texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    for (uint32_t layer = 0; layer < layers; layer++)
    {
        tb_rhi_cmd_copy_buffer_to_texture(list,
                                          &(tb_rhi_texture_copy){.buffer = staging,
                                                                 .offset = layer_bytes * layer,
                                                                 .row_pitch = (uint32_t)pitch,
                                                                 .texture = texture->texture,
                                                                 .layer = layer});
    }
    tb_rhi_cmd_barrier(
        list, &(tb_rhi_barrier){.texture = texture->texture,
                                .before = TB_RHI_STATE_COPY_DEST,
                                .after = mips ? TB_RHI_STATE_STORAGE : TB_RHI_STATE_SHADER_READ});
    if (mips)
    {
        tb_texture_record_mips(renderer, list, texture, content, layers);
        tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = texture->texture,
                                                   .before = TB_RHI_STATE_STORAGE,
                                                   .after = TB_RHI_STATE_SHADER_READ});
    }
    (void)tb_rhi_submit(renderer->device, &list, 1);
    /* The staging buffer goes once the GPU has copied it. */
    tb_rhi_buffer_destroy(renderer->device, staging);
    return TB_RHI_OK;
}

static tb_rhi_result tb_texture_restore(tb_renderer *renderer, tb_renderer_source *source,
                                        tb_rhi_error *error)
{
    const tb_texture *const texture = TB_LIST_ENTRY(source, tb_texture, source);
    return tb_texture_upload(renderer, texture, texture->content, texture->cube ? 6u : 1u,
                             texture->pixels, error);
}

/* Make a texture of `layers` square or flat images, as a 2D texture or a
 * cube, from pixels laid out one layer after another. */
static tb_texture *tb_texture_make(tb_renderer *renderer, const tb_texture_desc *desc,
                                   tb_rhi_texture_kind kind, uint32_t layers, const uint8_t *pixels,
                                   size_t size)
{
    tb_texture *texture = NULL;
    tb_rhi_texture_desc made = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    TB_ASSERT(renderer != NULL && desc != NULL && pixels != NULL);
    TB_ASSERT_MSG(desc->width > 0 && desc->height > 0 &&
                      desc->width <= renderer->info.max_texture_size &&
                      desc->height <= renderer->info.max_texture_size,
                  "a texture of a size the device makes");
    TB_ASSERT_MSG((size_t)desc->width * desc->height <= SIZE_MAX / 4u / layers &&
                      size == (size_t)desc->width * desc->height * 4u * layers,
                  "width * height RGBA pixels for each layer");
    texture = tb_calloc(1, sizeof(*texture));
    texture->owned = true;
    texture->cube = kind == TB_RHI_TEXTURE_CUBE;
    texture->width = desc->width;
    texture->height = desc->height;
    texture->mips = desc->no_mipmaps || !tb_texture_can_make_mips(renderer)
                        ? 1u
                        : tb_rhi_mip_count(desc->width, desc->height);
    made.kind = kind;
    made.format = desc->content == TB_TEXTURE_COLOR ? TB_RHI_FORMAT_R8G8B8A8_SRGB
                                                    : TB_RHI_FORMAT_R8G8B8A8_UNORM;
    made.width = desc->width;
    made.height = desc->height;
    made.layers = layers;
    made.mips = texture->mips;
    made.usage = TB_RHI_TEXTURE_SAMPLED | (texture->mips > 1 ? TB_RHI_TEXTURE_STORAGE : 0u);
    made.name = desc->name != NULL ? desc->name : "texture";
    if (tb_rhi_texture_create(renderer->device, &made, &texture->texture, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for a texture: %s", error.message);
    }
    texture->index = tb_rhi_texture_index(renderer->device, texture->texture);
    if (tb_texture_upload(renderer, texture, desc->content, layers, pixels, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for a texture's pixels: %s", error.message);
    }
    if ((renderer->info.features & TB_RHI_FEATURE_DEVICE_RECOVERY) != 0)
    {
        texture->pixels = tb_malloc(size);
        memcpy(texture->pixels, pixels, size);
        texture->content = desc->content;
        texture->source.upload = tb_texture_restore;
        tb_list_push_back(&renderer->sources, &texture->source.link);
    }
    return texture;
}

tb_texture *tb_texture_create(tb_renderer *renderer, const tb_texture_desc *desc,
                              const uint8_t *pixels, size_t size)
{
    return tb_texture_make(renderer, desc, TB_RHI_TEXTURE_2D, 1, pixels, size);
}

tb_texture *tb_texture_create_cube(tb_renderer *renderer, const tb_texture_desc *desc,
                                   const uint8_t *pixels, size_t size)
{
    TB_ASSERT(desc != NULL);
    TB_ASSERT_MSG(desc->width == desc->height, "a cube's faces are square");
    return tb_texture_make(renderer, desc, TB_RHI_TEXTURE_CUBE, 6, pixels, size);
}

tb_texture *tb_texture_from_rhi(tb_renderer *renderer, tb_rhi_texture texture)
{
    tb_rhi_texture_desc desc = {0};
    tb_texture *made = NULL;

    TB_ASSERT(renderer != NULL);
    tb_rhi_texture_desc_of(renderer->device, texture, &desc);
    TB_ASSERT_MSG((desc.usage & TB_RHI_TEXTURE_SAMPLED) != 0, "a texture shaders sample");
    made = tb_calloc(1, sizeof(*made));
    made->texture = texture;
    made->width = desc.width;
    made->height = desc.height;
    made->mips = desc.mips;
    made->cube = desc.kind == TB_RHI_TEXTURE_CUBE;
    made->index = tb_rhi_texture_index(renderer->device, texture);
    return made;
}

void tb_texture_destroy(tb_renderer *renderer, tb_texture *texture)
{
    TB_ASSERT(renderer != NULL);
    if (texture == NULL)
    {
        return;
    }
    if (tb_list_link_is_linked(&texture->source.link))
    {
        tb_list_remove(&texture->source.link);
        tb_free(texture->pixels);
    }
    if (texture->owned)
    {
        tb_rhi_texture_destroy(renderer->device, texture->texture);
    }
    tb_free(texture);
}

uint32_t tb_texture_bindless_index(const tb_texture *texture)
{
    return texture->index;
}

bool tb_texture_is_cube(const tb_texture *texture)
{
    return texture->cube;
}

void tb_texture_size(const tb_texture *texture, uint32_t *width, uint32_t *height, uint32_t *mips)
{
    TB_ASSERT(texture != NULL && width != NULL && height != NULL);
    *width = texture->width;
    *height = texture->height;
    if (mips != NULL)
    {
        *mips = texture->mips;
    }
}

/* ---------------------------------------------------------------------------
 * Materials.
 * ------------------------------------------------------------------------- */

static uint32_t tb_material_index(const tb_texture *texture, const tb_texture *otherwise)
{
    return texture != NULL ? texture->index : otherwise->index;
}

tb_material *tb_material_create(tb_renderer *renderer, const tb_material_desc *desc)
{
    tb_material *material = NULL;

    TB_ASSERT(renderer != NULL && desc != NULL);
    material = tb_calloc(1, sizeof(*material));
    material->base_color = tb_material_index(desc->base_color, renderer->material_white);
    material->normal = tb_material_index(desc->normal, renderer->material_flat);
    material->metallic_roughness =
        tb_material_index(desc->metallic_roughness, renderer->material_white);
    material->base_color_factor = desc->base_color_factor;
    material->normal_scale = desc->normal_scale;
    material->roughness = desc->roughness;
    material->metallic = desc->metallic;
    material->uv_scale = desc->uv_scale;
    material->unlit = desc->unlit;
    return material;
}

void tb_material_destroy(tb_renderer *renderer, tb_material *material)
{
    TB_ASSERT(renderer != NULL);
    tb_free(material);
}

tb_renderer_draw tb_renderer_material_draw(const tb_mesh *mesh, tb_mat4 transform,
                                           const tb_material *material)
{
    return (tb_renderer_draw){.mesh = mesh,
                              .material = material,
                              .transform = transform,
                              .color = material->base_color_factor,
                              .shading = material->unlit ? TB_MESH_FLAT : TB_MESH_LIT};
}

void tb_renderer_draw_mesh_material(tb_renderer *renderer, const tb_mesh *mesh, tb_mat4 transform,
                                    const tb_material *material)
{
    tb_renderer_draw draw = {0};

    TB_ASSERT(renderer != NULL && mesh != NULL && material != NULL);
    draw = tb_renderer_material_draw(mesh, transform, material);
    tb_renderer_add_draw(renderer, &draw);
}

uint64_t tb_renderer_material_upload(tb_renderer *renderer, const tb_renderer_draw *draw,
                                     tb_mat4 clip)
{
    const tb_material *const material = draw->material;
    const float lit = draw->shading == TB_MESH_LIT ? 1.0f : 0.0f;
    tb_material_constants constants = {0};

    constants.clip = clip;
    constants.model = draw->transform;
    constants.color = draw->color;
    constants.textures[3] = tb_rhi_sampler_index(renderer->device, renderer->material_sampler);
    if (material == NULL)
    {
        /* A color is drawn as a material of it with no textures would be:
         * rough, and not metal. */
        constants.surface = (tb_vec4){1.0f, 0.0f, 1.0f, lit};
        constants.uv = (tb_vec4){1.0f, 1.0f, 0.0f, 0.0f};
        constants.textures[0] = renderer->material_white->index;
        constants.textures[1] = renderer->material_flat->index;
        constants.textures[2] = renderer->material_white->index;
        return tb_renderer_upload(renderer, &constants, sizeof(constants),
                                  TB_RHI_CONSTANT_ALIGNMENT);
    }
    constants.surface =
        (tb_vec4){material->roughness, material->metallic, material->normal_scale, lit};
    constants.uv = (tb_vec4){material->uv_scale.x, material->uv_scale.y, 0.0f, 0.0f};
    constants.textures[0] = material->base_color;
    constants.textures[1] = material->normal;
    constants.textures[2] = material->metallic_roughness;
    return tb_renderer_upload(renderer, &constants, sizeof(constants), TB_RHI_CONSTANT_ALIGNMENT);
}

/* ---------------------------------------------------------------------------
 * The renderer's part.
 * ------------------------------------------------------------------------- */

static uint32_t tb_material_anisotropy(uint32_t asked)
{
    if (asked == 0)
    {
        return TB_MATERIAL_DEFAULT_ANISOTROPY;
    }
    return asked < TB_MATERIAL_MAX_ANISOTROPY ? asked : TB_MATERIAL_MAX_ANISOTROPY;
}

static tb_rhi_result tb_material_sampler_create(tb_renderer *renderer, uint32_t anisotropy,
                                                tb_rhi_error *error)
{
    tb_rhi_result result = tb_rhi_sampler_create(
        renderer->device, &(tb_rhi_sampler_desc){.anisotropy = anisotropy, .name = "materials"},
        &renderer->material_sampler, error);

    renderer->anisotropy = anisotropy;
    renderer->anisotropy_wanted = anisotropy;
    return result;
}

static tb_rhi_result tb_material_pipelines(tb_renderer *renderer, tb_rhi_error *error)
{
    const tb_rhi_vertex_attribute attributes[3] = {
        {.semantic = "POSITION", .format = TB_RHI_FORMAT_R32G32B32_FLOAT},
        {.semantic = "NORMAL", .format = TB_RHI_FORMAT_R32G32B32_FLOAT, .offset = 12},
        {.semantic = "TEXCOORD", .format = TB_RHI_FORMAT_R32G32_FLOAT, .offset = 24}};
    tb_rhi_graphics_pipeline_desc desc = {0};
    tb_rhi_shader mip = {0};
    tb_rhi_result result = TB_RHI_OK;

    desc.attributes = attributes;
    desc.attribute_count = 3;
    desc.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = sizeof(tb_mesh_vertex)};
    desc.buffer_count = 1;
    desc.cull = TB_RHI_CULL_BACK;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER;
    desc.colors[0] = TB_RENDERER_BASE_COLOR_FORMAT;
    desc.colors[1] = TB_RENDERER_NORMAL_FORMAT;
    desc.colors[2] = TB_RENDERER_SURFACE_FORMAT;
    desc.color_count = TB_RENDERER_GBUFFER_TARGETS;
    desc.depth = TB_RENDERER_DEPTH_FORMAT;
    desc.name = "material";
    result = tb_renderer_pipeline(renderer, "/Engine/Private/Material.hlsl", NULL, &desc,
                                  &renderer->material_pipeline, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    result = tb_renderer_shader(renderer, "/Engine/Private/GenerateMips.hlsl", NULL, "MainCS",
                                TB_RHI_STAGE_COMPUTE, &mip, error);
    if (result == TB_RHI_OK)
    {
        result = tb_rhi_compute_pipeline_create(
            renderer->device,
            &(tb_rhi_compute_pipeline_desc){.compute = mip, .name = "generate mips"},
            &renderer->mip_pipeline, error);
    }
    tb_rhi_shader_destroy(renderer->device, mip);
    if (result == TB_RHI_UNAVAILABLE)
    {
        /* coverage-off: WARP and the GPUs the tests run on make mips */
        /* A device that cannot make mips on the GPU draws textures of one
         * level, as it says. */
        if (renderer->log != NULL)
        {
            (void)tb_log_printf(renderer->log, TB_LOG_WARNING, "renderer",
                                "textures have one level: %s", error->message);
        }
        return TB_RHI_OK;
        /* coverage-on */
    }
    return result;
}

tb_rhi_result tb_renderer_materials_create(tb_renderer *renderer, uint32_t anisotropy,
                                           tb_rhi_error *error)
{
    static const uint8_t white[4] = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX};
    /* A normal straight out of the surface. */
    static const uint8_t flat[4] = {128, 128, UINT8_MAX, UINT8_MAX};
    tb_rhi_result result =
        tb_material_sampler_create(renderer, tb_material_anisotropy(anisotropy), error);

    if (result != TB_RHI_OK)
    {
        return result;
    }
    if ((renderer->info.features & TB_RHI_FEATURE_HEAP_INDICES) != 0)
    {
        /* Materials reach their textures by index; without, a material
         * draws in its base color factor alone. */
        result = tb_material_pipelines(renderer, error);
        if (result != TB_RHI_OK)
        {
            return result;
        }
    }
    renderer->material_white = tb_texture_create(
        renderer,
        &(tb_texture_desc){
            .width = 1, .height = 1, .content = TB_TEXTURE_DATA, .name = "material white"},
        white, sizeof(white));
    renderer->material_flat = tb_texture_create(
        renderer,
        &(tb_texture_desc){
            .width = 1, .height = 1, .content = TB_TEXTURE_NORMAL, .name = "material flat"},
        flat, sizeof(flat));
    return TB_RHI_OK;
}

void tb_renderer_materials_destroy(tb_renderer *renderer)
{
    tb_texture_destroy(renderer, renderer->material_white);
    tb_texture_destroy(renderer, renderer->material_flat);
    renderer->material_white = NULL;
    renderer->material_flat = NULL;
    tb_rhi_pipeline_destroy(renderer->device, renderer->material_pipeline);
    tb_rhi_pipeline_destroy(renderer->device, renderer->mip_pipeline);
    tb_rhi_sampler_destroy(renderer->device, renderer->material_sampler);
}

void tb_renderer_set_anisotropy(tb_renderer *renderer, uint32_t anisotropy)
{
    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(anisotropy >= 1 && anisotropy <= TB_MATERIAL_MAX_ANISOTROPY,
                  "an anisotropy from 1 to 16");
    renderer->anisotropy_wanted = anisotropy;
}

void tb_renderer_materials_begin_frame(tb_renderer *renderer)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    if (renderer->anisotropy_wanted == renderer->anisotropy)
    {
        return;
    }
    /* The frames before were submitted with the old sampler, which goes
     * once the GPU has finished them. */
    tb_rhi_sampler_destroy(renderer->device, renderer->material_sampler);
    if (tb_material_sampler_create(renderer, renderer->anisotropy_wanted, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no sampler for materials: %s", error.message);
    }
}
