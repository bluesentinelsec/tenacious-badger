/* The sandbox's scenes: the renderer's shapes under the fly camera; and,
 * drawn through the RHI itself inside the renderer's scene pass, a turning
 * triangle and a texture reached by its bindless index; and a clear. */
#include "sandbox.h"

#include "badger/fmt.h"
#include "badger/math.h"
#include "badger/memory.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * clear: nothing but the clear color, to see the swap chain present.
 * ------------------------------------------------------------------------- */

static bool clear_create(sandbox *box, void **state)
{
    (void)box;
    *state = NULL;
    return true;
}

static void clear_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    (void)box;
    (void)state;
    (void)frame;
}

static void clear_destroy(sandbox *box, void *state)
{
    (void)box;
    (void)state;
}

/* ---------------------------------------------------------------------------
 * shapes: every shape the renderer generates, lit, turning, on a grid.
 * ------------------------------------------------------------------------- */

#define SHAPE_COUNT 7

typedef struct shapes_scene
{
    tb_mesh *meshes[SHAPE_COUNT];
    tb_mesh *floor;
} shapes_scene;

static bool shapes_create(sandbox *box, void **state)
{
    shapes_scene *const scene = tb_calloc(1, sizeof(*scene));

    *state = scene;
    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        scene->meshes[shape] = tb_mesh_create_shape(box->renderer, (tb_mesh_shape)shape, 0);
    }
    scene->floor = tb_mesh_create_shape(box->renderer, TB_MESH_PLANE, 0);
    return true;
}

static void shapes_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    static const tb_vec4 colors[SHAPE_COUNT] = {{0.90f, 0.35f, 0.30f, 1}, {0.85f, 0.85f, 0.85f, 1},
                                                {0.30f, 0.70f, 0.95f, 1}, {0.95f, 0.75f, 0.25f, 1},
                                                {0.55f, 0.85f, 0.40f, 1}, {0.80f, 0.45f, 0.90f, 1},
                                                {0.95f, 0.55f, 0.20f, 1}};
    const shapes_scene *const scene = state;
    const float turn = (float)frame->seconds * 0.6f;

    tb_renderer_draw_mesh(
        box->renderer, scene->floor,
        tb_mat4_from_trs((tb_vec3){0, -0.01f, 0}, tb_quat_identity(), (tb_vec3){24, 1, 24}),
        (tb_vec4){0.22f, 0.23f, 0.25f, 1});
    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        const float x = ((float)shape - (float)(SHAPE_COUNT - 1) * 0.5f) * 1.6f;
        const tb_quat spin = tb_quat_mul(tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, turn),
                                         tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, 0.4f));
        const tb_mat4 transform =
            tb_mat4_from_trs((tb_vec3){x, 0.75f, 0}, spin, (tb_vec3){1, 1, 1});
        tb_renderer_draw_mesh(box->renderer, scene->meshes[shape], transform, colors[shape]);
        tb_debug_box(box->renderer, transform, (tb_vec3){0.5f, 0.5f, 0.5f},
                     (tb_vec4){1, 1, 1, 0.25f}, 0);
    }
    tb_debug_grid(box->renderer, (tb_vec3){0, 0, 0}, 24.0f, 24, (tb_vec4){0.35f, 0.36f, 0.4f, 1},
                  0);
    tb_debug_axes(box->renderer, tb_mat4_identity(), 1.0f, TB_DEBUG_ON_TOP);
    tb_debug_arrow(box->renderer, (tb_vec3){0, 2.5f, 0}, (tb_vec3){0, 1.6f, 0},
                   (tb_vec4){1, 1, 0.3f, 1}, 0);
}

static void shapes_destroy(sandbox *box, void *state)
{
    shapes_scene *const scene = state;

    for (int shape = 0; shape < SHAPE_COUNT; shape++)
    {
        tb_mesh_destroy(box->renderer, scene->meshes[shape]);
    }
    tb_mesh_destroy(box->renderer, scene->floor);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * triangle: three colored corners, turning, drawn through the RHI itself.
 * ------------------------------------------------------------------------- */

static const char triangle_source[] =
    "struct DrawConstants { float Angle; float Aspect; };\n"
    "ConstantBuffer<DrawConstants> Draw : register(b0);\n"
    "struct VertexOut { float4 Position : SV_Position; float4 Color : COLOR0; };\n"
    "VertexOut MainVS(uint id : SV_VertexID)\n"
    "{\n"
    "    const float turn = Draw.Angle + id * 2.0943951;\n"
    "    VertexOut output;\n"
    "    output.Position = float4(sin(turn) * 0.8 / Draw.Aspect, cos(turn) * 0.8, 0.5, 1);\n"
    "    output.Color = float4(id == 0, id == 1, id == 2, 1);\n"
    "    return output;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target { return input.Color; }\n";

typedef struct triangle_scene
{
    tb_rhi_pipeline pipeline;
    float angle;
} triangle_scene;

/* A pipeline from one source's MainVS and MainPS, the shaders released. */
static bool make_pipeline(sandbox *box, const char *path, const char *source,
                          tb_rhi_graphics_pipeline_desc *desc, tb_rhi_pipeline *pipeline)
{
    bool made = sandbox_shader(box, path, source, "MainVS", TB_RHI_STAGE_VERTEX, &desc->vertex) &&
                sandbox_shader(box, path, source, "MainPS", TB_RHI_STAGE_PIXEL, &desc->pixel);
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    if (made && tb_rhi_graphics_pipeline_create(box->device, desc, pipeline, &error) != TB_RHI_OK)
    {
        made = false;
        (void)tb_fmt_print_error("badger-sandbox: %s: %s\n", path, error.message);
    }
    tb_rhi_shader_destroy(box->device, desc->vertex);
    tb_rhi_shader_destroy(box->device, desc->pixel);
    return made;
}

static bool triangle_create(sandbox *box, void **state)
{
    triangle_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_rhi_graphics_pipeline_desc desc = sandbox_pipeline_desc(box);

    *state = scene;
    desc.name = "triangle";
    return make_pipeline(box, "/Sandbox/Triangle.hlsl", triangle_source, &desc, &scene->pipeline);
}

static void triangle_record(const tb_renderer_draw_context *draw)
{
    const triangle_scene *const scene = draw->context;
    const float constants[2] = {scene->angle, (float)draw->width / (float)draw->height};

    tb_rhi_cmd_pipeline(draw->list, scene->pipeline);
    tb_rhi_cmd_push_constants(draw->list, constants, sizeof(constants));
    tb_rhi_cmd_draw(draw->list, 3, 1, 0, 0);
}

static void triangle_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    triangle_scene *const scene = state;

    scene->angle = (float)frame->seconds;
    tb_renderer_draw_custom(box->renderer, triangle_record, scene);
}

static void triangle_destroy(sandbox *box, void *state)
{
    triangle_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * textured: a checkerboard made in memory, on a turning quad, reached by the
 * texture's and the sampler's bindless indices.
 * ------------------------------------------------------------------------- */

static const char textured_source[] =
    "struct DrawConstants { uint Texture; uint Sampler; float Angle; float Aspect; };\n"
    "ConstantBuffer<DrawConstants> Draw : register(b0);\n"
    "struct VertexOut { float4 Position : SV_Position; float2 UV : TEXCOORD0; };\n"
    "VertexOut MainVS(uint id : SV_VertexID)\n"
    "{\n"
    "    const float2 corner = float2(id & 1, id >> 1);\n"
    "    const float2 centered = (corner - 0.5) * 1.4;\n"
    "    const float s = sin(Draw.Angle);\n"
    "    const float c = cos(Draw.Angle);\n"
    "    VertexOut output;\n"
    "    output.Position = float4((centered.x * c - centered.y * s) / Draw.Aspect,\n"
    "                             centered.x * s + centered.y * c, 0.5, 1);\n"
    "    output.UV = corner * 4;\n"
    "    return output;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target\n"
    "{\n"
    "    Texture2D<float4> image = ResourceDescriptorHeap[Draw.Texture];\n"
    "    SamplerState sampler = SamplerDescriptorHeap[Draw.Sampler];\n"
    "    return image.Sample(sampler, input.UV);\n"
    "}\n";

#define CHECKER_SIZE 64u
#define CHECKER_PITCH (CHECKER_SIZE * 4u) /* a multiple of 256, as copies need */

typedef struct textured_scene
{
    tb_rhi_pipeline pipeline;
    tb_rhi_texture texture;
    tb_rhi_sampler sampler;
    uint32_t texture_index;
    uint32_t sampler_index;
    float angle;
} textured_scene;

/* Fill the texture through a staging buffer, once, before any frame uses it. */
static bool textured_upload(sandbox *box, textured_scene *scene)
{
    uint8_t *texels = tb_calloc(CHECKER_SIZE * CHECKER_PITCH, 1);
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;

    for (uint32_t y = 0; y < CHECKER_SIZE; y++)
    {
        for (uint32_t x = 0; x < CHECKER_SIZE; x++)
        {
            const bool light = ((x / 8u) + (y / 8u)) % 2u == 0;
            uint8_t *const texel = &texels[y * CHECKER_PITCH + x * 4u];
            texel[0] = light ? 230 : (uint8_t)(40 + x * 2);
            texel[1] = light ? 230 : 50;
            texel[2] = light ? 230 : (uint8_t)(40 + y * 2);
            texel[3] = 255;
        }
    }
    if (tb_rhi_buffer_create(box->device,
                             &(tb_rhi_buffer_desc){.size = CHECKER_SIZE * CHECKER_PITCH,
                                                   .memory = TB_RHI_MEMORY_UPLOAD},
                             &staging, NULL) != TB_RHI_OK)
    {
        tb_free(texels);
        return false;
    }
    tb_rhi_buffer_write(box->device, staging, 0, texels, CHECKER_SIZE * CHECKER_PITCH);
    tb_free(texels);
    list = tb_rhi_command_list_begin(box->device);
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = scene->texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(list, &(tb_rhi_texture_copy){.buffer = staging,
                                                                   .row_pitch = CHECKER_PITCH,
                                                                   .texture = scene->texture});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = scene->texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_SHADER_READ});
    (void)tb_rhi_submit(box->device, &list, 1);
    tb_rhi_buffer_destroy(box->device, staging);
    return true;
}

static bool textured_create(sandbox *box, void **state)
{
    textured_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_rhi_graphics_pipeline_desc desc = sandbox_pipeline_desc(box);

    *state = scene;
    if (tb_rhi_texture_create(box->device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                     .width = CHECKER_SIZE,
                                                     .height = CHECKER_SIZE,
                                                     .usage = TB_RHI_TEXTURE_SAMPLED,
                                                     .name = "checkerboard"},
                              &scene->texture, NULL) != TB_RHI_OK ||
        tb_rhi_sampler_create(box->device,
                              &(tb_rhi_sampler_desc){.min_filter = TB_RHI_FILTER_NEAREST,
                                                     .mag_filter = TB_RHI_FILTER_NEAREST},
                              &scene->sampler, NULL) != TB_RHI_OK ||
        !textured_upload(box, scene))
    {
        return false;
    }
    scene->texture_index = tb_rhi_texture_index(box->device, scene->texture);
    scene->sampler_index = tb_rhi_sampler_index(box->device, scene->sampler);
    desc.topology = TB_RHI_TOPOLOGY_TRIANGLE_STRIP;
    desc.name = "textured";
    return make_pipeline(box, "/Sandbox/Textured.hlsl", textured_source, &desc, &scene->pipeline);
}

static void textured_record(const tb_renderer_draw_context *draw)
{
    const textured_scene *const scene = draw->context;
    const struct
    {
        uint32_t texture;
        uint32_t sampler;
        float angle;
        float aspect;
    } constants = {scene->texture_index, scene->sampler_index, scene->angle,
                   (float)draw->width / (float)draw->height};

    tb_rhi_cmd_pipeline(draw->list, scene->pipeline);
    tb_rhi_cmd_push_constants(draw->list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(draw->list, 4, 1, 0, 0);
}

static void textured_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    textured_scene *const scene = state;

    scene->angle = (float)frame->seconds * 0.5f;
    tb_renderer_draw_custom(box->renderer, textured_record, scene);
}

static void textured_destroy(sandbox *box, void *state)
{
    textured_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_rhi_texture_destroy(box->device, scene->texture);
    tb_rhi_sampler_destroy(box->device, scene->sampler);
    tb_free(scene);
}

const sandbox_scene sandbox_scenes[] = {
    {"shapes",
     "every shape the renderer makes, lit, under the fly camera",
     {0.10f, 0.11f, 0.13f, 1},
     shapes_create,
     shapes_draw,
     shapes_destroy},
    {"triangle",
     "a triangle of three colored corners, turning, drawn through the RHI",
     {0.08f, 0.09f, 0.11f, 1},
     triangle_create,
     triangle_draw,
     triangle_destroy},
    {"textured",
     "a checkerboard made in memory, sampled by bindless index",
     {0.08f, 0.09f, 0.11f, 1},
     textured_create,
     textured_draw,
     textured_destroy},
    {"clear",
     "the swap chain cleared to a plain color",
     {0.20f, 0.35f, 0.55f, 1},
     clear_create,
     clear_draw,
     clear_destroy},
};

const size_t sandbox_scene_count = sizeof(sandbox_scenes) / sizeof(sandbox_scenes[0]);
