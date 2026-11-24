/* The sandbox's scenes, each a small test of what the renderer can show:
 * a clear, a triangle, a spinning cube with depth, and a textured quad whose
 * texture and sampler shaders reach by bindless index. */
#include "sandbox.h"

#include "badger/fmt.h"
#include "badger/math.h"
#include "badger/memory.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * clear: a color that cycles, to see the swap chain present.
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
    /* The pass cleared to the frame's color; nothing more to draw. */
}

static void clear_destroy(sandbox *box, void *state)
{
    (void)box;
    (void)state;
}

/* ---------------------------------------------------------------------------
 * triangle: three colored corners, turning.
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

static void triangle_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    const triangle_scene *const scene = state;
    const float constants[2] = {(float)frame->seconds, frame->aspect};

    (void)box;
    tb_rhi_cmd_pipeline(frame->list, scene->pipeline);
    tb_rhi_cmd_push_constants(frame->list, constants, sizeof(constants));
    tb_rhi_cmd_draw(frame->list, 3, 1, 0, 0);
}

static void triangle_destroy(sandbox *box, void *state)
{
    triangle_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * cube: an indexed cube from a vertex buffer, turning, in perspective, with
 * reversed depth.
 * ------------------------------------------------------------------------- */

static const char cube_source[] =
    "struct DrawConstants { float4x4 Clip; };\n"
    "ConstantBuffer<DrawConstants> Draw : register(b0);\n"
    "struct VertexIn { float3 Position : POSITION; float3 Color : COLOR0; };\n"
    "struct VertexOut { float4 Position : SV_Position; float3 Color : COLOR0; };\n"
    "VertexOut MainVS(VertexIn input)\n"
    "{\n"
    "    VertexOut output;\n"
    "    output.Position = mul(Draw.Clip, float4(input.Position, 1));\n"
    "    output.Color = input.Color;\n"
    "    return output;\n"
    "}\n"
    "float4 MainPS(VertexOut input) : SV_Target { return float4(input.Color, 1); }\n";

typedef struct cube_vertex
{
    float position[3];
    float color[3];
} cube_vertex;

typedef struct cube_scene
{
    tb_rhi_pipeline pipeline;
    tb_rhi_buffer vertices;
    tb_rhi_buffer indices;
} cube_scene;

static bool cube_create(sandbox *box, void **state)
{
    /* Each face its own four corners, so that each has one color. */
    static const float faces[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                      {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    static const float colors[6][3] = {{0.9f, 0.3f, 0.3f}, {0.3f, 0.9f, 0.3f}, {0.3f, 0.3f, 0.9f},
                                       {0.9f, 0.9f, 0.3f}, {0.9f, 0.3f, 0.9f}, {0.3f, 0.9f, 0.9f}};
    const tb_rhi_vertex_attribute attributes[2] = {
        {.semantic = "POSITION", .format = TB_RHI_FORMAT_R32G32B32_FLOAT},
        {.semantic = "COLOR", .format = TB_RHI_FORMAT_R32G32B32_FLOAT, .offset = 12}};
    cube_vertex vertices[24] = {0};
    uint16_t indices[36] = {0};
    cube_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_rhi_graphics_pipeline_desc desc = sandbox_pipeline_desc(box);

    *state = scene;
    for (int face = 0; face < 6; face++)
    {
        const tb_vec3 normal = {faces[face][0], faces[face][1], faces[face][2]};
        const tb_vec3 up = fabsf(normal.y) > 0.5f ? (tb_vec3){0, 0, 1} : (tb_vec3){0, 1, 0};
        const tb_vec3 side = tb_vec3_cross(up, normal);
        static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int corner = 0; corner < 4; corner++)
        {
            cube_vertex *const vertex = &vertices[face * 4 + corner];
            const tb_vec3 point =
                tb_vec3_add(normal, tb_vec3_add(tb_vec3_scale(side, corners[corner][0]),
                                                tb_vec3_scale(up, corners[corner][1])));
            vertex->position[0] = point.x * 0.5f;
            vertex->position[1] = point.y * 0.5f;
            vertex->position[2] = point.z * 0.5f;
            memcpy(vertex->color, colors[face], sizeof(vertex->color));
        }
        indices[face * 6 + 0] = (uint16_t)(face * 4 + 0);
        indices[face * 6 + 1] = (uint16_t)(face * 4 + 1);
        indices[face * 6 + 2] = (uint16_t)(face * 4 + 2);
        indices[face * 6 + 3] = (uint16_t)(face * 4 + 0);
        indices[face * 6 + 4] = (uint16_t)(face * 4 + 2);
        indices[face * 6 + 5] = (uint16_t)(face * 4 + 3);
    }
    /* Upload memory is read by the GPU where it lies: fine for a cube. */
    if (tb_rhi_buffer_create(box->device,
                             &(tb_rhi_buffer_desc){.size = sizeof(vertices),
                                                   .usage = TB_RHI_BUFFER_VERTEX,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "cube vertices"},
                             &scene->vertices, NULL) != TB_RHI_OK ||
        tb_rhi_buffer_create(box->device,
                             &(tb_rhi_buffer_desc){.size = sizeof(indices),
                                                   .usage = TB_RHI_BUFFER_INDEX,
                                                   .memory = TB_RHI_MEMORY_UPLOAD,
                                                   .name = "cube indices"},
                             &scene->indices, NULL) != TB_RHI_OK)
    {
        return false;
    }
    tb_rhi_buffer_write(box->device, scene->vertices, 0, vertices, sizeof(vertices));
    tb_rhi_buffer_write(box->device, scene->indices, 0, indices, sizeof(indices));
    desc.attributes = attributes;
    desc.attribute_count = 2;
    desc.buffers[0] = (tb_rhi_vertex_buffer_layout){.stride = sizeof(cube_vertex)};
    desc.buffer_count = 1;
    desc.cull = TB_RHI_CULL_BACK;
    desc.name = "cube";
    return make_pipeline(box, "/Sandbox/Cube.hlsl", cube_source, &desc, &scene->pipeline);
}

static void cube_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    const cube_scene *const scene = state;
    const float seconds = (float)frame->seconds;
    const tb_quat spin = tb_quat_mul(tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, seconds),
                                     tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, seconds * 0.6f));
    const tb_mat4 model = tb_mat4_rotation(spin);
    tb_mat4 view = tb_mat4_identity();
    const tb_mat4 projection =
        tb_mat4_perspective(tb_radians(60.0f), frame->aspect, 0.1f, INFINITY);
    tb_mat4 clip = {0};

    (void)box;
    (void)tb_mat4_look_at((tb_vec3){0, 0, 3}, (tb_vec3){0, 0, 0}, (tb_vec3){0, 1, 0}, &view);
    clip = tb_mat4_mul(projection, tb_mat4_mul(view, model));
    tb_rhi_cmd_pipeline(frame->list, scene->pipeline);
    tb_rhi_cmd_vertex_buffer(frame->list, 0, scene->vertices, 0);
    tb_rhi_cmd_index_buffer(frame->list, scene->indices, 0, TB_RHI_FORMAT_R16_UINT);
    tb_rhi_cmd_push_constants(frame->list, &clip, sizeof(clip));
    tb_rhi_cmd_draw_indexed(frame->list, 36, 1, 0, 0, 0);
}

static void cube_destroy(sandbox *box, void *state)
{
    cube_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_rhi_buffer_destroy(box->device, scene->vertices);
    tb_rhi_buffer_destroy(box->device, scene->indices);
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
    tb_rhi_buffer staging;
    bool uploaded;
} textured_scene;

static bool textured_create(sandbox *box, void **state)
{
    textured_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_rhi_graphics_pipeline_desc desc = sandbox_pipeline_desc(box);
    uint8_t *texels = tb_calloc(CHECKER_SIZE * CHECKER_PITCH, 1);
    bool made = false;

    *state = scene;
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
    made = tb_rhi_texture_create(box->device,
                                 &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_R8G8B8A8_UNORM,
                                                        .width = CHECKER_SIZE,
                                                        .height = CHECKER_SIZE,
                                                        .usage = TB_RHI_TEXTURE_SAMPLED,
                                                        .name = "checkerboard"},
                                 &scene->texture, NULL) == TB_RHI_OK &&
           tb_rhi_sampler_create(box->device,
                                 &(tb_rhi_sampler_desc){.min_filter = TB_RHI_FILTER_NEAREST,
                                                        .mag_filter = TB_RHI_FILTER_NEAREST},
                                 &scene->sampler, NULL) == TB_RHI_OK &&
           tb_rhi_buffer_create(box->device,
                                &(tb_rhi_buffer_desc){.size = CHECKER_SIZE * CHECKER_PITCH,
                                                      .memory = TB_RHI_MEMORY_UPLOAD},
                                &scene->staging, NULL) == TB_RHI_OK;
    if (made)
    {
        tb_rhi_buffer_write(box->device, scene->staging, 0, texels, CHECKER_SIZE * CHECKER_PITCH);
        desc.topology = TB_RHI_TOPOLOGY_TRIANGLE_STRIP;
        desc.name = "textured";
        made =
            make_pipeline(box, "/Sandbox/Textured.hlsl", textured_source, &desc, &scene->pipeline);
    }
    tb_free(texels);
    return made;
}

/* The texture is filled by the first frame's commands, before its pass. */
static void textured_prepare(sandbox *box, void *state, tb_rhi_command_list *list)
{
    textured_scene *const scene = state;

    (void)box;
    if (scene->uploaded)
    {
        return;
    }
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = scene->texture,
                                               .before = TB_RHI_STATE_COMMON,
                                               .after = TB_RHI_STATE_COPY_DEST});
    tb_rhi_cmd_copy_buffer_to_texture(list, &(tb_rhi_texture_copy){.buffer = scene->staging,
                                                                   .row_pitch = CHECKER_PITCH,
                                                                   .texture = scene->texture});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = scene->texture,
                                               .before = TB_RHI_STATE_COPY_DEST,
                                               .after = TB_RHI_STATE_SHADER_READ});
    scene->uploaded = true;
}

static void textured_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    const textured_scene *const scene = state;
    struct
    {
        uint32_t texture;
        uint32_t sampler;
        float angle;
        float aspect;
    } constants = {tb_rhi_texture_index(box->device, scene->texture),
                   tb_rhi_sampler_index(box->device, scene->sampler), (float)frame->seconds * 0.5f,
                   frame->aspect};

    tb_rhi_cmd_pipeline(frame->list, scene->pipeline);
    tb_rhi_cmd_push_constants(frame->list, &constants, sizeof(constants));
    tb_rhi_cmd_draw(frame->list, 4, 1, 0, 0);
}

static void textured_destroy(sandbox *box, void *state)
{
    textured_scene *const scene = state;

    tb_rhi_pipeline_destroy(box->device, scene->pipeline);
    tb_rhi_texture_destroy(box->device, scene->texture);
    tb_rhi_sampler_destroy(box->device, scene->sampler);
    tb_rhi_buffer_destroy(box->device, scene->staging);
    tb_free(scene);
}

const sandbox_scene sandbox_scenes[] = {
    {"clear", "the swap chain cleared to a color that cycles", clear_create, NULL, clear_draw,
     clear_destroy},
    {"triangle", "a triangle of three colored corners, turning", triangle_create, NULL,
     triangle_draw, triangle_destroy},
    {"cube", "a cube from a vertex buffer, turning in perspective, with reversed depth",
     cube_create, NULL, cube_draw, cube_destroy},
    {"textured", "a checkerboard made in memory, sampled by bindless index", textured_create,
     textured_prepare, textured_draw, textured_destroy},
};

const size_t sandbox_scene_count = sizeof(sandbox_scenes) / sizeof(sandbox_scenes[0]);
