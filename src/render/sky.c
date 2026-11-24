/* The sky: one triangle over the whole view at the far plane, behind what
 * the scene drew, its color a gradient and a sun computed for each pixel's
 * direction, or a cube map sampled by it. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/sky.h"

#include <math.h>

/* What the sky shader reads, as its SkyConstants lays it out. */
typedef struct tb_sky_constants
{
    tb_vec4 forward; /* the camera's, to the middle of the view */
    tb_vec4 right;   /* from the middle to the right edge of the view */
    tb_vec4 up;      /* from the middle to the top edge of the view */
    tb_vec4 zenith;
    tb_vec4 horizon;
    tb_vec4 ground;
    tb_vec4 sun;       /* the way to the sun, and the cosine of its radius */
    tb_vec4 sun_color; /* and 1 for a sun, 0 for none */
    uint32_t cube[4];  /* 1 for a cube map, its index, its sampler's */
} tb_sky_constants;

TB_STATIC_ASSERT(sizeof(tb_sky_constants) <= 256u, "the sky's constants in 256 bytes");

/* The sky's triangle covers the view, at depth 0: the far plane, as depth
 * is reversed. Each pixel's direction is the camera's forward, plus its
 * place in the view times the edges' directions. Above the horizon the
 * color runs from the horizon's to the zenith's, quickly near the horizon
 * as a sky's does; below, to the ground's over a few degrees. The sun's
 * disc is smoothed over a pixel, with a faint glow around it. */
static const char tb_sky_source[] =
    "struct SkyConstants\n"
    "{\n"
    "    float4 Forward; float4 Right; float4 Up; float4 Zenith; float4 Horizon; float4 Ground;\n"
    "    float4 Sun; float4 SunColor; uint4 Cube;\n"
    "};\n"
    "ConstantBuffer<SkyConstants> Sky : register(b1);\n"
    "struct VertexOut { float4 Position : SV_Position; float2 View : TEXCOORD0; };\n"
    "VertexOut MainVS(uint vertex : SV_VertexID)\n"
    "{\n"
    "    const float2 corner = float2((vertex << 1) & 2, vertex & 2);\n"
    "    const float2 view = corner * float2(2, -2) + float2(-1, 1);\n"
    "    VertexOut output;\n"
    "    output.Position = float4(view, 0, 1);\n"
    "    output.View = view;\n"
    "    return output;\n"
    "}\n"
    "float3 Procedural(float3 direction)\n"
    "{\n"
    "    float3 color = lerp(Sky.Horizon.rgb, Sky.Ground.rgb, saturate(-direction.y * 12.0));\n"
    "    if (direction.y >= 0.0)\n"
    "    {\n"
    "        color = lerp(Sky.Horizon.rgb, Sky.Zenith.rgb, sqrt(direction.y));\n"
    "    }\n"
    "    const float facing = dot(direction, Sky.Sun.xyz);\n"
    "    const float edge = max(fwidth(facing), 1e-6);\n"
    "    const float disc = smoothstep(Sky.Sun.w - edge, Sky.Sun.w + edge, facing);\n"
    "    const float glow = pow(saturate(facing), 64.0) * 0.25;\n"
    "    const float sun = Sky.SunColor.w;\n"
    "    color = lerp(color, Sky.SunColor.rgb, disc * sun) + Sky.SunColor.rgb * glow * sun;\n"
    "    return color;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    const float3 direction = normalize(Sky.Forward.xyz + input.View.x * Sky.Right.xyz +\n"
    "                                       input.View.y * Sky.Up.xyz);\n"
    "    if (Sky.Cube.x != 0)\n"
    "    {\n"
    "        TextureCube<float4> cube = ResourceDescriptorHeap[Sky.Cube.y];\n"
    "        SamplerState filter = SamplerDescriptorHeap[Sky.Cube.z];\n"
    "        return float4(cube.Sample(filter, direction).rgb, 1);\n"
    "    }\n"
    "    return float4(Procedural(direction), 1);\n"
    "}\n";

static tb_vec4 tb_sky_color(tb_vec3 color)
{
    return (tb_vec4){color.x, color.y, color.z, 1.0f};
}

void tb_renderer_set_sky(tb_renderer *renderer, const tb_sky *sky)
{
    TB_ASSERT(renderer != NULL);
    renderer->sky = sky != NULL ? *sky : (tb_sky){0};
    TB_ASSERT_MSG(renderer->sky.kind != TB_SKY_CUBE ||
                      (renderer->sky.cube != NULL && tb_texture_is_cube(renderer->sky.cube)),
                  "a cube sky has a cube map");
    TB_ASSERT_MSG(renderer->sky.sun_radius >= 0.0f && renderer->sky.sun_radius < TB_PI,
                  "a sun's radius from 0 to less than a half turn");
}

uint64_t tb_renderer_sky_upload(tb_renderer *renderer, float aspect)
{
    const tb_camera *const camera = &renderer->camera;
    const tb_sky *const sky = &renderer->sky;
    const float sun_length = tb_vec3_length(sky->sun_direction);
    const bool has_sun = sun_length > 0.0f && sky->sun_radius > 0.0f;
    tb_sky_constants constants = {0};
    float half_height = 0.0f;

    if (sky->kind == TB_SKY_NONE || renderer->sky_pipeline.generation == 0)
    {
        return UINT64_MAX;
    }
    /* An orthographic camera looks one way at every pixel. */
    if (camera->projection == TB_CAMERA_PERSPECTIVE)
    {
        half_height = tanf(camera->vertical_fov * 0.5f);
    }
    constants.forward = tb_sky_color(tb_quat_rotate(camera->orientation, (tb_vec3){0, 0, -1}));
    constants.right = tb_sky_color(tb_vec3_scale(
        tb_quat_rotate(camera->orientation, (tb_vec3){1, 0, 0}), half_height * aspect));
    constants.up = tb_sky_color(
        tb_vec3_scale(tb_quat_rotate(camera->orientation, (tb_vec3){0, 1, 0}), half_height));
    constants.zenith = tb_sky_color(sky->zenith);
    constants.horizon = tb_sky_color(sky->horizon);
    constants.ground = tb_sky_color(sky->ground);
    if (has_sun)
    {
        const tb_vec3 toward = tb_vec3_scale(sky->sun_direction, 1.0f / sun_length);
        constants.sun = (tb_vec4){toward.x, toward.y, toward.z, cosf(sky->sun_radius)};
        constants.sun_color = (tb_vec4){sky->sun_color.x, sky->sun_color.y, sky->sun_color.z, 1.0f};
    }
    if (sky->kind == TB_SKY_CUBE)
    {
        constants.cube[0] = 1;
        constants.cube[1] = tb_texture_bindless_index(sky->cube);
        constants.cube[2] = tb_rhi_sampler_index(renderer->device, renderer->material_sampler);
    }
    return tb_renderer_upload(renderer, &constants, sizeof(constants), TB_RHI_CONSTANT_ALIGNMENT);
}

void tb_renderer_sky_record(tb_renderer *renderer, tb_rhi_command_list *list, uint64_t constants)
{
    if (constants == UINT64_MAX)
    {
        return;
    }
    tb_rhi_cmd_pipeline(list, renderer->sky_pipeline);
    tb_rhi_cmd_constant_buffer(list, 0, renderer->frames[renderer->slot].upload, constants);
    tb_rhi_cmd_draw(list, 3, 1, 0, 0);
}

tb_rhi_result tb_renderer_sky_create(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_graphics_pipeline_desc desc = {0};

    if ((renderer->info.features & TB_RHI_FEATURE_BINDLESS) == 0)
    {
        /* The sky's shader reaches a cube map by index; without, there is
         * no sky, and the clear color shows. */
        return TB_RHI_OK;
    }
    desc.topology = TB_RHI_TOPOLOGY_TRIANGLES;
    desc.depth_test = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER_EQUAL;
    desc.colors[0] = TB_RENDERER_SCENE_FORMAT;
    desc.color_count = 1;
    desc.depth = TB_RENDERER_DEPTH_FORMAT;
    desc.name = "sky";
    return tb_renderer_pipeline(renderer, "/Engine/Private/Sky.hlsl", tb_sky_source, &desc,
                                &renderer->sky_pipeline, error);
}

void tb_renderer_sky_destroy(tb_renderer *renderer)
{
    tb_rhi_pipeline_destroy(renderer->device, renderer->sky_pipeline);
}
