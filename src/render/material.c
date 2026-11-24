/* Textures and materials: pixels uploaded and their mip chains made by a
 * compute shader, the sampler that filters them as the settings say, and
 * the pipeline that draws a mesh with a material's textures, reached by
 * their bindless indices. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
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
    tb_rhi_texture texture;
    bool owned; /* made here, not borrowed from a caller */
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

/* A material draw's constants, as the material shader's MaterialConstants
 * lays them out. */
typedef struct tb_material_constants
{
    tb_mat4 clip;
    tb_mat4 model;
    tb_vec4 color;        /* the base color factor */
    tb_vec4 light;        /* the way the light goes, and 1 to light the surface or 0 */
    tb_vec4 eye;          /* the camera's position */
    tb_vec4 surface;      /* roughness, metallic, normal scale */
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

/* The tangent frame of each pixel comes from the screen-space derivatives
 * of its position and texture coordinates (Schueler, "Normal Mapping Without
 * Precomputed Tangents", 2013), so a mesh needs no tangents: any caller's
 * vertices work, mirrored coordinates included, for a few instructions a
 * pixel. Its sign follows the screen's handedness, since D3D's y runs down
 * where the paper's runs up. A normal map's +Y is up in the image, toward smaller v, as glTF's
 * is. Shade() is the lighting of a surface: the renderer's one light with
 * ambient, and a highlight that roughness and metallic shape, until the
 * raster lighting mode replaces it. */
static const char tb_material_source[] =
    "struct MaterialConstants\n"
    "{\n"
    "    float4x4 Clip; float4x4 Model; float4 Color; float4 Light; float4 Eye;\n"
    "    float4 Surface; float4 UV; uint4 Textures;\n"
    "};\n"
    "ConstantBuffer<MaterialConstants> Draw : register(b1);\n"
    "struct VertexIn { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; "
    "};\n"
    "struct VertexOut\n"
    "{\n"
    "    float4 Position : SV_Position;\n"
    "    float3 World : TEXCOORD1;\n"
    "    float3 Normal : NORMAL;\n"
    "    float2 UV : TEXCOORD0;\n"
    "};\n"
    "VertexOut MainVS(VertexIn input)\n"
    "{\n"
    "    VertexOut output;\n"
    "    output.Position = mul(Draw.Clip, float4(input.Position, 1));\n"
    "    output.World = mul(Draw.Model, float4(input.Position, 1)).xyz;\n"
    "    output.Normal = mul(Draw.Model, float4(input.Normal, 0)).xyz;\n"
    "    output.UV = input.UV * Draw.UV.xy;\n"
    "    return output;\n"
    "}\n"
    "float3 BendNormal(float3 normal, float3 position, float2 uv, float3 bent)\n"
    "{\n"
    "    const float3 dp1 = ddx(position);\n"
    "    const float3 dp2 = ddy(position);\n"
    "    const float2 duv1 = ddx(uv);\n"
    "    const float2 duv2 = ddy(uv);\n"
    "    const float3 dp2perp = cross(dp2, normal);\n"
    "    const float3 dp1perp = cross(normal, dp1);\n"
    "    const float3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;\n"
    "    const float3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;\n"
    "    const float largest = max(dot(tangent, tangent), dot(bitangent, bitangent));\n"
    "    const float handed = dot(cross(dp1, dp2), normal) < 0.0 ? -1.0 : 1.0;\n"
    "    const float scale = rsqrt(max(largest, 1e-20)) * handed;\n"
    "    const float3 bent_normal =\n"
    "        tangent * (bent.x * scale) - bitangent * (bent.y * scale) + normal * bent.z;\n"
    "    return normalize(bent_normal);\n"
    "}\n"
    "float3 Shade(float3 base, float3 normal, float3 view, float roughness, float metallic)\n"
    "{\n"
    "    const float3 to_light = -Draw.Light.xyz;\n"
    "    const float facing = saturate(dot(normal, to_light));\n"
    "    const float3 diffuse = base * (1.0 - metallic) * (0.25 + 0.75 * facing);\n"
    "    const float3 specular = lerp(float3(0.04, 0.04, 0.04), base, metallic);\n"
    "    const float3 halfway = normalize(to_light + view);\n"
    "    const float shininess = exp2(10.0 * (1.0 - roughness) + 1.0);\n"
    "    const float highlight =\n"
    "        pow(saturate(dot(normal, halfway)), shininess) * (shininess + 2.0) / 8.0 * facing;\n"
    "    return diffuse + specular * (0.25 * metallic + highlight);\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    Texture2D<float4> base_map = ResourceDescriptorHeap[Draw.Textures.x];\n"
    "    Texture2D<float4> normal_map = ResourceDescriptorHeap[Draw.Textures.y];\n"
    "    Texture2D<float4> surface_map = ResourceDescriptorHeap[Draw.Textures.z];\n"
    "    SamplerState filter = SamplerDescriptorHeap[Draw.Textures.w];\n"
    "    const float4 base = base_map.Sample(filter, input.UV) * Draw.Color;\n"
    "    float3 bent = normal_map.Sample(filter, input.UV).xyz * 2.0 - 1.0;\n"
    "    bent.xy *= Draw.Surface.z;\n"
    "    const float3 normal =\n"
    "        BendNormal(normalize(input.Normal), input.World, input.UV, bent);\n"
    "    const float4 surface = surface_map.Sample(filter, input.UV);\n"
    "    const float roughness = saturate(surface.g * Draw.Surface.x);\n"
    "    const float metallic = saturate(surface.b * Draw.Surface.y);\n"
    "    const float3 view = normalize(Draw.Eye.xyz - input.World);\n"
    "    const float3 lit = Shade(base.rgb, normal, view, roughness, metallic);\n"
    "    return float4(lerp(base.rgb, lit, Draw.Light.w), base.a);\n"
    "}\n";

/* Each thread makes a texel of a level from the 2x2 texels of the level
 * before, clamped at its edges: colors averaged in linear light and written
 * back in sRGB, normals averaged and renormalized. Both levels are written
 * through their storage views, so the texture stays in one state. */
static const char tb_mip_source[] =
    "struct MipConstants\n"
    "{\n"
    "    uint Source; uint Dest; uint Width; uint Height;\n"
    "    uint SourceWidth; uint SourceHeight; uint Content; uint Array;\n"
    "};\n"
    "ConstantBuffer<MipConstants> Mip : register(b0);\n"
    "float3 ToLinear(float3 c)\n"
    "{\n"
    "    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));\n"
    "}\n"
    "float3 ToSRGB(float3 c)\n"
    "{\n"
    "    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);\n"
    "}\n"
    "float4 Decode(float4 texel)\n"
    "{\n"
    "    if (Mip.Content == 0)\n"
    "    {\n"
    "        return float4(ToLinear(texel.rgb), texel.a);\n"
    "    }\n"
    "    if (Mip.Content == 2)\n"
    "    {\n"
    "        return float4(texel.rgb * 2.0 - 1.0, texel.a);\n"
    "    }\n"
    "    return texel;\n"
    "}\n"
    "float4 Encode(float4 value)\n"
    "{\n"
    "    if (Mip.Content == 0)\n"
    "    {\n"
    "        return float4(ToSRGB(saturate(value.rgb)), value.a);\n"
    "    }\n"
    "    if (Mip.Content == 2)\n"
    "    {\n"
    "        const float size = length(value.rgb);\n"
    "        const float3 normal = size > 0.0 ? value.rgb / size : float3(0, 0, 1);\n"
    "        return float4(normal * 0.5 + 0.5, value.a);\n"
    "    }\n"
    "    return value;\n"
    "}\n"
    "float4 Load(uint2 at, uint layer)\n"
    "{\n"
    "    if (Mip.Array != 0)\n"
    "    {\n"
    "        RWTexture2DArray<float4> layers = ResourceDescriptorHeap[Mip.Source];\n"
    "        return layers[uint3(at, layer)];\n"
    "    }\n"
    "    RWTexture2D<float4> level = ResourceDescriptorHeap[Mip.Source];\n"
    "    return level[at];\n"
    "}\n"
    "void Store(uint2 at, uint layer, float4 value)\n"
    "{\n"
    "    if (Mip.Array != 0)\n"
    "    {\n"
    "        RWTexture2DArray<float4> layers = ResourceDescriptorHeap[Mip.Dest];\n"
    "        layers[uint3(at, layer)] = value;\n"
    "        return;\n"
    "    }\n"
    "    RWTexture2D<float4> level = ResourceDescriptorHeap[Mip.Dest];\n"
    "    level[at] = value;\n"
    "}\n"
    "[numthreads(8, 8, 1)]\n"
    "void MainCS(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    if (id.x >= Mip.Width || id.y >= Mip.Height)\n"
    "    {\n"
    "        return;\n"
    "    }\n"
    "    const uint2 last = uint2(Mip.SourceWidth - 1, Mip.SourceHeight - 1);\n"
    "    const uint2 at = id.xy * 2;\n"
    "    float4 sum = Decode(Load(min(at, last), id.z));\n"
    "    sum += Decode(Load(min(at + uint2(1, 0), last), id.z));\n"
    "    sum += Decode(Load(min(at + uint2(0, 1), last), id.z));\n"
    "    sum += Decode(Load(min(at + uint2(1, 1), last), id.z));\n"
    "    Store(id.xy, id.z, Encode(sum * 0.25));\n"
    "}\n";

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
static void tb_texture_upload(tb_renderer *renderer, const tb_texture *texture,
                              tb_texture_content content, uint32_t layers, const uint8_t *pixels)
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
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    for (uint32_t layer = 0; layer < layers; layer++)
    {
        for (uint32_t y = 0; y < texture->height; y++)
        {
            memcpy(&staged[layer_bytes * layer + pitch * y],
                   &pixels[row * ((size_t)texture->height * layer + y)], row);
        }
    }
    if (tb_rhi_buffer_create(renderer->device,
                             &(tb_rhi_buffer_desc){.size = (uint64_t)layer_bytes * layers,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "texture pixels"},
                             &staging, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for a texture's pixels: %s", error.message);
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
    tb_texture_upload(renderer, texture, desc->content, layers, pixels);
    return texture;
}

tb_texture *tb_texture_create(tb_renderer *renderer, const tb_texture_desc *desc,
                              const uint8_t *pixels, size_t size)
{
    return tb_texture_make(renderer, desc, TB_RHI_TEXTURE_2D, 1, pixels, size);
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
    if (texture->owned)
    {
        tb_rhi_texture_destroy(renderer->device, texture->texture);
    }
    tb_free(texture);
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

void tb_renderer_draw_mesh_material(tb_renderer *renderer, const tb_mesh *mesh, tb_mat4 transform,
                                    const tb_material *material)
{
    TB_ASSERT(renderer != NULL && mesh != NULL && material != NULL);
    tb_renderer_add_draw(
        renderer, &(tb_renderer_draw){.mesh = mesh,
                                      .material = material,
                                      .transform = transform,
                                      .color = material->base_color_factor,
                                      .shading = material->unlit ? TB_MESH_FLAT : TB_MESH_LIT});
}

uint64_t tb_renderer_material_upload(tb_renderer *renderer, const tb_renderer_draw *draw,
                                     tb_mat4 clip, const tb_renderer_lighting *lighting)
{
    const tb_material *const material = draw->material;
    tb_material_constants constants = {0};

    constants.clip = clip;
    constants.model = draw->transform;
    constants.color = material->base_color_factor;
    constants.light = (tb_vec4){lighting->direction.x, lighting->direction.y, lighting->direction.z,
                                material->unlit ? 0.0f : 1.0f};
    constants.eye = (tb_vec4){lighting->eye.x, lighting->eye.y, lighting->eye.z, 1.0f};
    constants.surface =
        (tb_vec4){material->roughness, material->metallic, material->normal_scale, 0.0f};
    constants.uv = (tb_vec4){material->uv_scale.x, material->uv_scale.y, 0.0f, 0.0f};
    constants.textures[0] = material->base_color;
    constants.textures[1] = material->normal;
    constants.textures[2] = material->metallic_roughness;
    constants.textures[3] = tb_rhi_sampler_index(renderer->device, renderer->material_sampler);
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
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.color_count = 1;
    desc.depth = TB_RENDERER_DEPTH_FORMAT;
    desc.name = "material";
    result = tb_renderer_pipeline(renderer, "/Engine/Private/Material.hlsl", tb_material_source,
                                  &desc, &renderer->material_pipeline, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    result = tb_renderer_shader(renderer, "/Engine/Private/GenerateMips.hlsl", tb_mip_source,
                                "MainCS", TB_RHI_STAGE_COMPUTE, &mip, error);
    if (result == TB_RHI_OK)
    {
        result = tb_rhi_compute_pipeline_create(
            renderer->device,
            &(tb_rhi_compute_pipeline_desc){.compute = mip, .name = "generate mips"},
            &renderer->mip_pipeline, error);
    }
    tb_rhi_shader_destroy(renderer->device, mip);
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
    if ((renderer->info.features & TB_RHI_FEATURE_BINDLESS) != 0)
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
