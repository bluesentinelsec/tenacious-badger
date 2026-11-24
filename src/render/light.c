/* The raster lighting mode's lights and passes: the scene's lights, kept
 * by handle; the lighting pass, which lights the G-buffer with them and
 * with the sky into the scene's color; and the tonemap pass, which maps
 * that color into the swap chain's. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/str.h"

#include <math.h>

/* A light's handle: its slot plus 1 in the low bits, its generation above. */
#define TB_LIGHT_SLOT_BITS 16u
#define TB_LIGHT_SLOT_MASK 0xFFFFu
#define TB_LIGHT_GENERATION_MASK 0xFFFFu
/* Bytes of a light as the lighting shader reads it. */
#define TB_PACKED_LIGHT_BYTES 64u
/* The ambient light of a frame without a sky, unless set. */
#define TB_AMBIENT_SKY 0.25f
#define TB_AMBIENT_GROUND 0.08f
/* The lighting pass's flags. */
#define TB_LIGHTING_SPECULAR_AMBIENT 1u

TB_STATIC_ASSERT(sizeof(tb_packed_light) == TB_PACKED_LIGHT_BYTES, "a light in 64 bytes");
TB_STATIC_ASSERT(TB_RENDERER_MAX_LIGHTS <= TB_LIGHT_SLOT_MASK, "a light's slot in its handle");

/* The lighting pass's constants, as the lighting shader's
 * LightingConstants lays them out. */
typedef struct tb_lighting_constants
{
    tb_mat4 clip_to_world; /* the inverse of the frame's clip from the world */
    tb_vec4 eye;           /* the camera's position; w 1 for perspective, 0 orthographic */
    tb_vec4 forward;       /* the way the camera looks */
    tb_vec4 clear;         /* where nothing was drawn */
    tb_vec4 zenith;        /* the ambient light from straight up */
    tb_vec4 horizon;       /* from the horizon */
    tb_vec4 ground;        /* from below */
    tb_vec4 size;          /* the G-buffer's pixels, and one over each */
    uint32_t lights[4];    /* the uploads' bindless index, the lights' offset, count, flags */
    uint32_t sky[4];       /* 1 for a cube map, its index, its sampler's, its levels */
} tb_lighting_constants;

TB_STATIC_ASSERT(sizeof(tb_lighting_constants) <= 256u, "the lighting's constants in 256 bytes");

/* What the tonemap shader reads, as its ToneConstants lays it out. */
typedef struct tb_tonemap_constants
{
    uint32_t scene; /* the bindless index of the scene's color */
    float exposure; /* what the scene's color is multiplied by first */
    uint32_t unused[2];
} tb_tonemap_constants;

/* Every full-screen pass draws one triangle that covers the view. */
#define TB_FULL_SCREEN_VERTEX                                                                      \
    "struct VertexOut { float4 Position : SV_Position; };\n"                                       \
    "VertexOut MainVS(uint vertex : SV_VertexID)\n"                                                \
    "{\n"                                                                                          \
    "    const float2 corner = float2((vertex << 1) & 2, vertex & 2);\n"                           \
    "    VertexOut output;\n"                                                                      \
    "    output.Position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);\n"                \
    "    return output;\n"                                                                         \
    "}\n"

/* Unreal's default lit shading model: a Lambert diffuse, and a specular of
 * GGX's distribution, Smith's joint visibility (approximated), and
 * Schlick's Fresnel, with Unreal's remapping of roughness (a = roughness
 * squared). Each pixel's place in the world comes back from its depth
 * through the inverse of the frame's clip transform. Ambient light comes
 * from the sky: a cube map's lower levels, or the colors of a procedural
 * sky (or of the ambient light without one), blended toward the average of
 * the half they face as a surface roughens; reflections are scaled by
 * Karis's analytical fit of the environment BRDF. Point and spot lights
 * fall off by the inverse square of their distance, windowed to nothing at
 * their range; a spot's cone fades between its angles, squared, as
 * Unreal's do. The source is in two parts, each shorter than the longest
 * string literal C compilers must take; the pipeline joins them. */
static const char tb_lighting_brdf_source[] =
    "#define PI 3.14159265\n"
    "struct GBufferIndices { uint BaseColor; uint Normal; uint Surface; uint Depth; };\n"
    "ConstantBuffer<GBufferIndices> GBuffer : register(b0);\n"
    "struct LightingConstants\n"
    "{\n"
    "    float4x4 ClipToWorld; float4 Eye; float4 Forward; float4 Clear;\n"
    "    float4 Zenith; float4 Horizon; float4 Ground; float4 Size; uint4 Lights; uint4 Sky;\n"
    "};\n"
    "ConstantBuffer<LightingConstants> Lighting : register(b1);\n" TB_FULL_SCREEN_VERTEX
    "float3 DecodeNormal(float2 encoded)\n"
    "{\n"
    "    float3 normal = float3(encoded.x, encoded.y, 1.0 - abs(encoded.x) - abs(encoded.y));\n"
    "    const float fold = saturate(-normal.z);\n"
    "    normal.x += normal.x >= 0.0 ? -fold : fold;\n"
    "    normal.y += normal.y >= 0.0 ? -fold : fold;\n"
    "    return normalize(normal);\n"
    "}\n"
    "float Pow5(float x)\n"
    "{\n"
    "    const float x2 = x * x;\n"
    "    return x2 * x2 * x;\n"
    "}\n"
    "float DistributionGGX(float a2, float n_dot_h)\n"
    "{\n"
    "    const float d = (n_dot_h * a2 - n_dot_h) * n_dot_h + 1.0;\n"
    "    return a2 / (PI * d * d);\n"
    "}\n"
    "float VisibilitySmithJoint(float a2, float n_dot_v, float n_dot_l)\n"
    "{\n"
    "    const float a = sqrt(a2);\n"
    "    const float smith_v = n_dot_l * (n_dot_v * (1.0 - a) + a);\n"
    "    const float smith_l = n_dot_v * (n_dot_l * (1.0 - a) + a);\n"
    "    return 0.5 / (smith_v + smith_l);\n"
    "}\n"
    "float3 FresnelSchlick(float3 specular, float v_dot_h)\n"
    "{\n"
    "    const float fc = Pow5(1.0 - v_dot_h);\n"
    "    return saturate(50.0 * specular.g) * fc + (1.0 - fc) * specular;\n"
    "}\n"
    "float3 EnvironmentBRDF(float3 specular, float roughness, float n_dot_v)\n"
    "{\n"
    "    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);\n"
    "    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);\n"
    "    const float4 r = roughness * c0 + c1;\n"
    "    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;\n"
    "    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;\n"
    "    return specular * ab.x + saturate(50.0 * specular.g) * ab.y;\n"
    "}\n"
    "float3 SkyLight(float3 direction, float blur)\n"
    "{\n"
    "    if (Lighting.Sky.x != 0)\n"
    "    {\n"
    "        TextureCube<float4> cube = ResourceDescriptorHeap[Lighting.Sky.y];\n"
    "        SamplerState filter = SamplerDescriptorHeap[Lighting.Sky.z];\n"
    "        const float level = blur * (float)(Lighting.Sky.w - 1);\n"
    "        return cube.SampleLevel(filter, direction, level).rgb;\n"
    "    }\n"
    "    const float up = direction.y;\n"
    "    float3 sharp = lerp(Lighting.Horizon.rgb, Lighting.Ground.rgb, saturate(-up * 12.0));\n"
    "    if (up >= 0.0)\n"
    "    {\n"
    "        sharp = lerp(Lighting.Horizon.rgb, Lighting.Zenith.rgb, sqrt(up));\n"
    "    }\n"
    "    const float3 above = 0.5 * (Lighting.Horizon.rgb + Lighting.Zenith.rgb);\n"
    "    const float3 soft = lerp(Lighting.Ground.rgb, above, saturate(0.5 + 0.5 * up));\n"
    "    return lerp(sharp, soft, blur);\n"
    "}\n"
    "float3 Shade(float3 diffuse, float3 specular, float a2, float3 normal, float3 view,\n"
    "             float3 to_light, float n_dot_v)\n"
    "{\n"
    "    const float n_dot_l = saturate(dot(normal, to_light));\n"
    "    if (n_dot_l <= 0.0)\n"
    "    {\n"
    "        return float3(0, 0, 0);\n"
    "    }\n"
    "    const float3 halfway = normalize(view + to_light);\n"
    "    const float n_dot_h = saturate(dot(normal, halfway));\n"
    "    const float v_dot_h = saturate(dot(view, halfway));\n"
    "    const float3 lobe = DistributionGGX(a2, n_dot_h) *\n"
    "                        VisibilitySmithJoint(a2, n_dot_v, n_dot_l) *\n"
    "                        FresnelSchlick(specular, v_dot_h);\n"
    "    return (diffuse / PI + lobe) * n_dot_l;\n"
    "}\n";

static const char tb_lighting_pass_source[] =
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    const int3 texel = int3(input.Position.xy, 0);\n"
    "    Texture2D<float> depth_map = ResourceDescriptorHeap[GBuffer.Depth];\n"
    "    const float depth = depth_map.Load(texel);\n"
    "    if (depth <= 0.0)\n"
    "    {\n"
    "        return float4(Lighting.Clear.rgb, 1);\n"
    "    }\n"
    "    Texture2D<float4> base_map = ResourceDescriptorHeap[GBuffer.BaseColor];\n"
    "    Texture2D<float2> normal_map = ResourceDescriptorHeap[GBuffer.Normal];\n"
    "    Texture2D<float4> surface_map = ResourceDescriptorHeap[GBuffer.Surface];\n"
    "    const float3 base = base_map.Load(texel).rgb;\n"
    "    const float4 surface = surface_map.Load(texel);\n"
    "    if (surface.a < 0.5)\n"
    "    {\n"
    "        return float4(base, 1);\n"
    "    }\n"
    "    const float3 normal = DecodeNormal(normal_map.Load(texel));\n"
    "    const float2 ndc = float2(input.Position.x * Lighting.Size.z * 2.0 - 1.0,\n"
    "                              1.0 - input.Position.y * Lighting.Size.w * 2.0);\n"
    "    const float4 place = mul(Lighting.ClipToWorld, float4(ndc, depth, 1));\n"
    "    const float3 world = place.xyz / place.w;\n"
    "    float3 view = -Lighting.Forward.xyz;\n"
    "    if (Lighting.Eye.w != 0.0)\n"
    "    {\n"
    "        view = normalize(Lighting.Eye.xyz - world);\n"
    "    }\n"
    "    const float n_dot_v = saturate(abs(dot(normal, view)) + 1e-5);\n"
    "    const float metallic = surface.r;\n"
    "    const float roughness = max(surface.g, 0.02);\n"
    "    const float a = roughness * roughness;\n"
    "    const float a2 = a * a;\n"
    "    const float3 diffuse = base * (1.0 - metallic);\n"
    "    const float3 specular = lerp(float3(0.04, 0.04, 0.04), base, metallic);\n"
    "    float3 color = float3(0, 0, 0);\n"
    "    ByteAddressBuffer lights = ResourceDescriptorHeap[Lighting.Lights.x];\n"
    "    for (uint index = 0; index < Lighting.Lights.z; index++)\n"
    "    {\n"
    "        const uint at = Lighting.Lights.y + index * 64;\n"
    "        const float4 position = lights.Load<float4>(at);\n"
    "        const float4 direction = lights.Load<float4>(at + 16);\n"
    "        const float4 radiance = lights.Load<float4>(at + 32);\n"
    "        const float4 cone = lights.Load<float4>(at + 48);\n"
    "        float3 to_light = -direction.xyz;\n"
    "        float attenuation = 1.0;\n"
    "        if (direction.w > 0.5)\n"
    "        {\n"
    "            const float3 offset = position.xyz - world;\n"
    "            const float distance2 = max(dot(offset, offset), 1e-4);\n"
    "            const float reach = distance2 * position.w;\n"
    "            const float window = saturate(1.0 - reach * reach);\n"
    "            to_light = offset * rsqrt(distance2);\n"
    "            attenuation = window * window / distance2;\n"
    "            if (direction.w > 1.5)\n"
    "            {\n"
    "                const float spot = saturate((dot(-to_light, direction.xyz) - cone.x) * "
    "cone.y);\n"
    "                attenuation *= spot * spot;\n"
    "            }\n"
    "        }\n"
    "        color += Shade(diffuse, specular, a2, normal, view, to_light, n_dot_v) *\n"
    "                 radiance.rgb * attenuation;\n"
    "    }\n"
    "    const float occlusion = surface.b;\n"
    "    color += SkyLight(normal, 1.0) * diffuse * occlusion;\n"
    "    if ((Lighting.Lights.w & 1) != 0)\n"
    "    {\n"
    "        const float3 reflected = reflect(-view, normal);\n"
    "        color += SkyLight(reflected, roughness) * EnvironmentBRDF(specular, roughness, "
    "n_dot_v) * occlusion;\n"
    "    }\n"
    "    return float4(color, 1);\n"
    "}\n";

/* The output transform: exposure, then Narkowicz's fit of the ACES filmic
 * curve to each channel, with the 0.6 he scales its input by to match
 * ACES's reference (a middle gray of 0.18 shows as about 0.41 in sRGB),
 * then sRGB's encoding, since the swap chain is UNORM and the display reads
 * it as sRGB. */
static const char tb_tonemap_source[] =
    "struct ToneConstants { uint Scene; float Exposure; uint2 Unused; };\n"
    "ConstantBuffer<ToneConstants> Tone : register(b0);\n" TB_FULL_SCREEN_VERTEX
    "float3 FilmicACES(float3 color)\n"
    "{\n"
    "    const float3 x = color * 0.6;\n"
    "    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));\n"
    "}\n"
    "float3 ToSRGB(float3 c)\n"
    "{\n"
    "    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    Texture2D<float4> scene = ResourceDescriptorHeap[Tone.Scene];\n"
    "    const float3 color = max(scene.Load(int3(input.Position.xy, 0)).rgb, 0.0);\n"
    "    return float4(ToSRGB(FilmicACES(color * Tone.Exposure)), 1);\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * Lights.
 * ------------------------------------------------------------------------- */

static tb_vec4 tb_light_vec4(tb_vec3 v, float w)
{
    return (tb_vec4){v.x, v.y, v.z, w};
}

/* The way a light goes, of unit length; straight down for none. */
static tb_vec3 tb_light_direction(tb_vec3 direction)
{
    const float length = tb_vec3_length(direction);
    return length > 0.0f ? tb_vec3_scale(direction, 1.0f / length) : (tb_vec3){0.0f, -1.0f, 0.0f};
}

tb_packed_light tb_light_pack(const tb_light *light)
{
    const tb_vec3 radiance = tb_vec3_scale(light->color, light->intensity * TB_PI);
    tb_packed_light packed = {0};

    packed.direction =
        tb_light_vec4(tb_light_direction(light->direction), (float)(uint32_t)light->kind);
    packed.radiance = tb_light_vec4(radiance, 0.0f);
    if (light->kind == TB_LIGHT_DIRECTIONAL)
    {
        return packed;
    }
    packed.position = tb_light_vec4(light->position, 1.0f / (light->range * light->range));
    if (light->kind == TB_LIGHT_SPOT)
    {
        const float outer = cosf(light->outer_angle);
        const float inner = cosf(light->inner_angle);
        packed.cone = (tb_vec4){outer, 1.0f / fmaxf(inner - outer, 1e-4f), 0.0f, 0.0f};
    }
    return packed;
}

static void tb_light_check(const tb_light *light)
{
    TB_ASSERT(light != NULL);
    TB_ASSERT_MSG(light->kind <= TB_LIGHT_SPOT, "a light of a kind there is");
    TB_ASSERT_MSG(light->intensity >= 0.0f, "an intensity of 0 or more");
    TB_ASSERT_MSG(light->kind == TB_LIGHT_POINT || tb_vec3_length(light->direction) > 0.0f,
                  "a direction that is not zero");
    TB_ASSERT_MSG(light->kind == TB_LIGHT_DIRECTIONAL || light->range > 0.0f,
                  "a range of more than 0");
    TB_ASSERT_MSG(light->kind != TB_LIGHT_SPOT ||
                      (light->inner_angle >= 0.0f && light->inner_angle < light->outer_angle &&
                       light->outer_angle < TB_PI * 0.5f),
                  "a cone of angles from 0 to less than a quarter turn, inner below outer");
}

static uint32_t tb_light_handle(size_t slot, uint32_t generation)
{
    return generation << TB_LIGHT_SLOT_BITS | (uint32_t)(slot + 1u);
}

/* The slot a handle names, or SIZE_MAX when it names no light. */
static size_t tb_light_find(const tb_renderer *renderer, tb_light_id id)
{
    const size_t slot = (size_t)(id.id & TB_LIGHT_SLOT_MASK);

    if (slot == 0 || slot > renderer->light_slots)
    {
        return SIZE_MAX;
    }
    if (!renderer->lights[slot - 1].used ||
        renderer->lights[slot - 1].generation != id.id >> TB_LIGHT_SLOT_BITS)
    {
        return SIZE_MAX;
    }
    return slot - 1;
}

tb_light_id tb_renderer_add_light(tb_renderer *renderer, const tb_light *light)
{
    size_t slot = 0;
    tb_light_slot *made = NULL;

    TB_ASSERT(renderer != NULL);
    tb_light_check(light);
    if (renderer->light_count == TB_RENDERER_MAX_LIGHTS)
    {
        return (tb_light_id){0};
    }
    while (slot < renderer->light_slots && renderer->lights[slot].used)
    {
        slot++;
    }
    if (slot == renderer->light_slots)
    {
        if (renderer->light_slots == renderer->light_capacity)
        {
            renderer->light_capacity =
                renderer->light_capacity == 0 ? 16 : renderer->light_capacity * 2;
            renderer->lights = tb_realloc_array(renderer->lights, renderer->light_capacity,
                                                sizeof(*renderer->lights));
        }
        renderer->lights[slot] = (tb_light_slot){0};
        renderer->light_slots++;
    }
    made = &renderer->lights[slot];
    /* A slot's generation moves on each time it is taken, skipping 0, so a
     * handle of a light removed from it names nothing. */
    made->generation = (made->generation + 1u) & TB_LIGHT_GENERATION_MASK;
    if (made->generation == 0)
    {
        made->generation = 1;
    }
    made->light = *light;
    made->used = true;
    renderer->light_count++;
    return (tb_light_id){tb_light_handle(slot, made->generation)};
}

bool tb_renderer_set_light(tb_renderer *renderer, tb_light_id id, const tb_light *light)
{
    size_t slot = 0;

    TB_ASSERT(renderer != NULL);
    tb_light_check(light);
    slot = tb_light_find(renderer, id);
    if (slot == SIZE_MAX)
    {
        return false;
    }
    renderer->lights[slot].light = *light;
    return true;
}

bool tb_renderer_get_light(const tb_renderer *renderer, tb_light_id id, tb_light *light)
{
    size_t slot = 0;

    TB_ASSERT(renderer != NULL && light != NULL);
    slot = tb_light_find(renderer, id);
    if (slot == SIZE_MAX)
    {
        return false;
    }
    *light = renderer->lights[slot].light;
    return true;
}

void tb_renderer_remove_light(tb_renderer *renderer, tb_light_id id)
{
    size_t slot = 0;

    TB_ASSERT(renderer != NULL);
    slot = tb_light_find(renderer, id);
    if (slot == SIZE_MAX)
    {
        return;
    }
    renderer->lights[slot].used = false;
    renderer->light_count--;
}

size_t tb_renderer_light_count(const tb_renderer *renderer)
{
    TB_ASSERT(renderer != NULL);
    return renderer->light_count;
}

void tb_renderer_set_ambient(tb_renderer *renderer, tb_vec3 sky, tb_vec3 ground)
{
    TB_ASSERT(renderer != NULL);
    renderer->ambient_sky = sky;
    renderer->ambient_ground = ground;
}

tb_light tb_renderer_default_light(const tb_renderer *renderer)
{
    const tb_sky *const sky = &renderer->sky;
    tb_light light = TB_LIGHT_INIT;
    tb_vec3 forward = {0};

    light.kind = TB_LIGHT_DIRECTIONAL;
    if (sky->kind == TB_SKY_PROCEDURAL && tb_vec3_length(sky->sun_direction) > 0.0f &&
        sky->sun_radius > 0.0f)
    {
        light.direction = tb_vec3_normalize(tb_vec3_negate(sky->sun_direction));
        light.color = sky->sun_color;
        return light;
    }
    /* From above and a little behind the camera, as meshes were lit before
     * the lighting mode. */
    forward = tb_quat_rotate(renderer->camera.orientation, (tb_vec3){0, 0, -1});
    light.direction =
        tb_vec3_normalize(tb_vec3_add((tb_vec3){0.3f, -1.0f, 0.2f}, tb_vec3_scale(forward, 0.5f)));
    return light;
}

/* Write the frame's lights to its uploads, or its default light when it
 * has none: their offset, or UINT64_MAX when there is no room. */
static uint64_t tb_lights_upload(tb_renderer *renderer, uint32_t *count)
{
    /* One after another, so that the shader finds the lights at one offset. */
    const uint64_t needed = TB_PACKED_LIGHT_BYTES * (uint64_t)(renderer->light_count + 1u);
    uint64_t offset = UINT64_MAX;

    *count = 0;
    if (tb_renderer_upload_room(renderer, TB_PACKED_LIGHT_BYTES) < needed)
    {
        return UINT64_MAX;
    }
    if (renderer->light_count == 0)
    {
        const tb_light light = tb_renderer_default_light(renderer);
        const tb_packed_light packed = tb_light_pack(&light);
        *count = 1;
        return tb_renderer_upload(renderer, &packed, sizeof(packed), TB_PACKED_LIGHT_BYTES);
    }
    for (size_t slot = 0; slot < renderer->light_slots; slot++)
    {
        tb_packed_light packed = {0};
        uint64_t at = 0;
        if (!renderer->lights[slot].used)
        {
            continue;
        }
        packed = tb_light_pack(&renderer->lights[slot].light);
        at = tb_renderer_upload(renderer, &packed, sizeof(packed), TB_PACKED_LIGHT_BYTES);
        offset = *count == 0 ? at : offset;
        (*count)++;
    }
    return offset;
}

/* The sky's part of the lighting's constants: the ambient light's colors,
 * and its cube map. */
static void tb_lighting_sky(const tb_renderer *renderer, tb_lighting_constants *constants)
{
    const tb_sky *const sky = &renderer->sky;

    constants->zenith = tb_light_vec4(renderer->ambient_sky, 1.0f);
    constants->horizon = tb_light_vec4(renderer->ambient_sky, 1.0f);
    constants->ground = tb_light_vec4(renderer->ambient_ground, 1.0f);
    if (sky->kind == TB_SKY_PROCEDURAL)
    {
        constants->zenith = tb_light_vec4(sky->zenith, 1.0f);
        constants->horizon = tb_light_vec4(sky->horizon, 1.0f);
        constants->ground = tb_light_vec4(sky->ground, 1.0f);
    }
    if (sky->kind == TB_SKY_CUBE)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mips = 0;
        tb_texture_size(sky->cube, &width, &height, &mips);
        constants->sky[0] = 1;
        constants->sky[1] = tb_texture_bindless_index(sky->cube);
        constants->sky[2] = tb_rhi_sampler_index(renderer->device, renderer->material_sampler);
        constants->sky[3] = mips;
    }
}

uint64_t tb_renderer_lighting_upload(tb_renderer *renderer, tb_mat4 clip)
{
    const tb_camera *const camera = &renderer->camera;
    tb_lighting_constants constants = {0};
    uint32_t count = 0;
    const uint64_t lights = tb_lights_upload(renderer, &count);

    if (lights == UINT64_MAX || !tb_mat4_inverse(clip, &constants.clip_to_world))
    {
        return UINT64_MAX;
    }
    constants.eye =
        tb_light_vec4(camera->position, camera->projection == TB_CAMERA_PERSPECTIVE ? 1.0f : 0.0f);
    constants.forward =
        tb_light_vec4(tb_quat_rotate(camera->orientation, (tb_vec3){0, 0, -1}), 0.0f);
    constants.clear = renderer->clear;
    constants.size = (tb_vec4){(float)renderer->width, (float)renderer->height,
                               1.0f / (float)renderer->width, 1.0f / (float)renderer->height};
    constants.lights[0] =
        tb_rhi_buffer_index(renderer->device, renderer->frames[renderer->slot].upload);
    constants.lights[1] = (uint32_t)lights;
    constants.lights[2] = count;
    constants.lights[3] = TB_LIGHTING_SPECULAR_AMBIENT;
    tb_lighting_sky(renderer, &constants);
    return tb_renderer_upload(renderer, &constants, sizeof(constants), TB_RHI_CONSTANT_ALIGNMENT);
}

void tb_renderer_lighting_record(tb_renderer *renderer, tb_rhi_command_list *list,
                                 uint64_t constants, const tb_renderer_gbuffer_indices *gbuffer)
{
    if (constants == UINT64_MAX)
    {
        return;
    }
    tb_rhi_cmd_pipeline(list, renderer->lighting_pipeline);
    tb_rhi_cmd_push_constants(list, gbuffer, sizeof(*gbuffer));
    tb_rhi_cmd_constant_buffer(list, 0, renderer->frames[renderer->slot].upload, constants);
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
}

void tb_renderer_tonemap_record(tb_renderer *renderer, tb_rhi_command_list *list, uint32_t scene)
{
    const tb_tonemap_constants constants = {.scene = scene, .exposure = 1.0f};

    tb_rhi_cmd_pipeline(list, renderer->tonemap_pipeline);
    tb_rhi_cmd_push_constants(list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
}

/* ---------------------------------------------------------------------------
 * The renderer's part.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_renderer_lighting_create(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_graphics_pipeline_desc desc = {0};
    char *source = NULL;
    tb_rhi_result result = TB_RHI_OK;

    renderer->ambient_sky = (tb_vec3){TB_AMBIENT_SKY, TB_AMBIENT_SKY, TB_AMBIENT_SKY};
    renderer->ambient_ground = (tb_vec3){TB_AMBIENT_GROUND, TB_AMBIENT_GROUND, TB_AMBIENT_GROUND};
    if (!renderer->deferred)
    {
        return TB_RHI_OK;
    }
    desc.colors[0] = TB_RENDERER_SCENE_FORMAT;
    desc.color_count = 1;
    desc.name = "lighting";
    source = tb_str_cat(NULL, tb_lighting_brdf_source, tb_lighting_pass_source);
    result = tb_renderer_pipeline(renderer, "/Engine/Private/DeferredLighting.hlsl", source, &desc,
                                  &renderer->lighting_pipeline, error);
    tb_str_free(NULL, source);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    desc.colors[0] = TB_RENDERER_COLOR_FORMAT;
    desc.name = "tonemap";
    return tb_renderer_pipeline(renderer, "/Engine/Private/Tonemap.hlsl", tb_tonemap_source, &desc,
                                &renderer->tonemap_pipeline, error);
}

void tb_renderer_lighting_destroy(tb_renderer *renderer)
{
    tb_rhi_pipeline_destroy(renderer->device, renderer->lighting_pipeline);
    tb_rhi_pipeline_destroy(renderer->device, renderer->tonemap_pipeline);
}
