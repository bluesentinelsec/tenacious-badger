/* Skinned meshes: their vertices on the GPU, the frame's skinned draws with
 * their joint matrices in the frame's uploads, and the compute pass that
 * skins them into a buffer the scene pass draws from. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/memory.h"
#include "badger/skinning.h"

/* Bytes of a vertex as the skinning shader reads it, and as it writes it. */
#define TB_SKIN_SOURCE_BYTES 56u
#define TB_SKIN_OUTPUT_BYTES 32u
/* Bytes of a joint's matrix. */
#define TB_SKIN_JOINT_BYTES 64u
/* Vertices a group of the skinning shader skins. */
#define TB_SKIN_GROUP 64u
/* The most joints a draw has: as many as a vertex's 16-bit indices name. */
#define TB_SKIN_MAX_JOINTS 65536u
/* The least a frame's skinned vertices are given, so that frames of
 * different sizes share the render graph's pooled memory. */
#define TB_SKIN_MIN_BUFFER ((uint64_t)64 * 1024)

TB_STATIC_ASSERT(sizeof(tb_skinned_vertex) == TB_SKIN_SOURCE_BYTES,
                 "a skinned vertex as the shader reads it");
TB_STATIC_ASSERT(sizeof(tb_mesh_vertex) == TB_SKIN_OUTPUT_BYTES,
                 "a mesh vertex as the shader writes it");
TB_STATIC_ASSERT(sizeof(tb_mat4) == TB_SKIN_JOINT_BYTES, "a joint's matrix as the shader reads it");
TB_STATIC_ASSERT(TB_SKINNED_MESH_MAX_VERTICES == TB_SKIN_GROUP * 65535u,
                 "the vertices of one dispatch's groups");

struct tb_skinned_mesh
{
    tb_mesh mesh;         /* its indices; its vertices are the frame's skinned ones */
    tb_rhi_buffer source; /* its vertices as they were bound, read by index */
    uint32_t source_index;
    uint32_t vertex_count;
};

/* A draw's skinning, as the skinning shader's SkinConstants lays it out. */
typedef struct tb_skin_constants
{
    uint32_t source;       /* the bindless index of the mesh's vertices */
    uint32_t joints;       /* the bindless index of the frame's uploads */
    uint32_t joint_offset; /* bytes into them of the draw's joint matrices */
    uint32_t joint_count;
    uint32_t dest;        /* the storage index of the frame's skinned vertices */
    uint32_t dest_offset; /* bytes into them of the draw's */
    uint32_t vertex_count;
    uint32_t unused;
} tb_skin_constants;

/* Each thread skins a vertex: its position and normal transformed by the
 * sum of its joints' matrices, weighed, which is a sum of each column. A
 * joint past the draw's matrices takes the last, so no vertex reads another
 * draw's. Normals are transformed as directions by the same matrix, which
 * is right for rotations and uniform scales. */
static const char tb_skin_source[] =
    "struct SkinConstants\n"
    "{\n"
    "    uint Source; uint Joints; uint JointOffset; uint JointCount;\n"
    "    uint Dest; uint DestOffset; uint VertexCount; uint Unused;\n"
    "};\n"
    "ConstantBuffer<SkinConstants> Skin : register(b0);\n"
    "float4 Column(ByteAddressBuffer joints, uint4 index, float4 weights, uint column)\n"
    "{\n"
    "    const uint last = Skin.JointCount - 1;\n"
    "    const uint at = Skin.JointOffset + column * 16;\n"
    "    float4 sum = joints.Load<float4>(at + min(index.x, last) * 64) * weights.x;\n"
    "    sum += joints.Load<float4>(at + min(index.y, last) * 64) * weights.y;\n"
    "    sum += joints.Load<float4>(at + min(index.z, last) * 64) * weights.z;\n"
    "    sum += joints.Load<float4>(at + min(index.w, last) * 64) * weights.w;\n"
    "    return sum;\n"
    "}\n"
    "[numthreads(64, 1, 1)]\n"
    "void MainCS(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    if (id.x >= Skin.VertexCount)\n"
    "    {\n"
    "        return;\n"
    "    }\n"
    "    ByteAddressBuffer source = ResourceDescriptorHeap[Skin.Source];\n"
    "    ByteAddressBuffer joints = ResourceDescriptorHeap[Skin.Joints];\n"
    "    RWByteAddressBuffer dest = ResourceDescriptorHeap[Skin.Dest];\n"
    "    const uint from = id.x * 56;\n"
    "    const float3 position = source.Load<float3>(from);\n"
    "    const float3 normal = source.Load<float3>(from + 12);\n"
    "    const float2 uv = source.Load<float2>(from + 24);\n"
    "    const uint2 packed = source.Load<uint2>(from + 32);\n"
    "    const float4 weights = source.Load<float4>(from + 40);\n"
    "    const uint4 index =\n"
    "        uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);\n"
    "    const float4 x = Column(joints, index, weights, 0);\n"
    "    const float4 y = Column(joints, index, weights, 1);\n"
    "    const float4 z = Column(joints, index, weights, 2);\n"
    "    const float4 w = Column(joints, index, weights, 3);\n"
    "    const float3 skinned = (x * position.x + y * position.y + z * position.z + w).xyz;\n"
    "    const float3 turned = (x * normal.x + y * normal.y + z * normal.z).xyz;\n"
    "    const float size = dot(turned, turned);\n"
    "    const uint to = Skin.DestOffset + id.x * 32;\n"
    "    dest.Store<float3>(to, skinned);\n"
    "    dest.Store<float3>(to + 12, size > 0.0 ? turned * rsqrt(size) : normal);\n"
    "    dest.Store<float2>(to + 24, uv);\n"
    "}\n";

/* ---------------------------------------------------------------------------
 * Meshes.
 * ------------------------------------------------------------------------- */

static void tb_skinned_mesh_buffer(tb_renderer *renderer, const tb_rhi_buffer_desc *desc,
                                   tb_rhi_buffer *buffer)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    if (tb_rhi_buffer_create(renderer->device, desc, buffer, &error) != TB_RHI_OK)
    {
        tb_fatal("renderer: no memory for a skinned mesh: %s", error.message);
    }
}

tb_skinned_mesh *tb_skinned_mesh_create(tb_renderer *renderer, const tb_skinned_vertex *vertices,
                                        size_t vertex_count, const uint32_t *indices,
                                        size_t index_count)
{
    const size_t vertex_bytes = vertex_count * sizeof(*vertices);
    const size_t index_bytes = index_count * sizeof(*indices);
    tb_skinned_mesh *mesh = NULL;
    tb_rhi_buffer staging = {0};
    tb_rhi_command_list *list = NULL;

    TB_ASSERT(renderer != NULL && vertices != NULL && indices != NULL);
    TB_ASSERT_MSG(vertex_count > 0 && vertex_count <= TB_SKINNED_MESH_MAX_VERTICES,
                  "a skinned mesh of from 1 to TB_SKINNED_MESH_MAX_VERTICES vertices");
    TB_ASSERT_MSG(index_count > 0 && index_count % 3 == 0 && index_count <= UINT32_MAX,
                  "a mesh of whole triangles");
    mesh = tb_calloc(1, sizeof(*mesh));
    mesh->vertex_count = (uint32_t)vertex_count;
    mesh->mesh.index_count = (uint32_t)index_count;
    tb_skinned_mesh_buffer(renderer,
                           &(tb_rhi_buffer_desc){.size = vertex_bytes,
                                                 .usage = TB_RHI_BUFFER_SHADER_READ,
                                                 .name = "skinned mesh vertices"},
                           &mesh->source);
    tb_skinned_mesh_buffer(renderer,
                           &(tb_rhi_buffer_desc){.size = index_bytes,
                                                 .usage = TB_RHI_BUFFER_INDEX,
                                                 .name = "skinned mesh indices"},
                           &mesh->mesh.indices);
    tb_skinned_mesh_buffer(
        renderer,
        &(tb_rhi_buffer_desc){.size = vertex_bytes + index_bytes, .memory = TB_RHI_MEMORY_UPLOAD},
        &staging);
    mesh->source_index = tb_rhi_buffer_index(renderer->device, mesh->source);
    tb_rhi_buffer_write(renderer->device, staging, 0, vertices, vertex_bytes);
    tb_rhi_buffer_write(renderer->device, staging, vertex_bytes, indices, index_bytes);
    list = tb_rhi_command_list_begin(renderer->device);
    tb_rhi_cmd_barriers(list,
                        (const tb_rhi_barrier[]){{.buffer = mesh->source,
                                                  .before = TB_RHI_STATE_COMMON,
                                                  .after = TB_RHI_STATE_COPY_DEST},
                                                 {.buffer = mesh->mesh.indices,
                                                  .before = TB_RHI_STATE_COMMON,
                                                  .after = TB_RHI_STATE_COPY_DEST}},
                        2);
    tb_rhi_cmd_copy_buffer(list, mesh->source, 0, staging, 0, vertex_bytes);
    tb_rhi_cmd_copy_buffer(list, mesh->mesh.indices, 0, staging, vertex_bytes, index_bytes);
    tb_rhi_cmd_barriers(list,
                        (const tb_rhi_barrier[]){{.buffer = mesh->source,
                                                  .before = TB_RHI_STATE_COPY_DEST,
                                                  .after = TB_RHI_STATE_SHADER_READ},
                                                 {.buffer = mesh->mesh.indices,
                                                  .before = TB_RHI_STATE_COPY_DEST,
                                                  .after = TB_RHI_STATE_VERTEX_INDEX}},
                        2);
    (void)tb_rhi_submit(renderer->device, &list, 1);
    /* The staging buffer goes once the GPU has copied it. */
    tb_rhi_buffer_destroy(renderer->device, staging);
    return mesh;
}

void tb_skinned_mesh_destroy(tb_renderer *renderer, tb_skinned_mesh *mesh)
{
    TB_ASSERT(renderer != NULL);
    if (mesh == NULL)
    {
        return;
    }
    tb_rhi_buffer_destroy(renderer->device, mesh->source);
    tb_rhi_buffer_destroy(renderer->device, mesh->mesh.indices);
    tb_free(mesh);
}

/* ---------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------- */

static void tb_renderer_add_skin(tb_renderer *renderer, const tb_renderer_skin *skin)
{
    if (renderer->skin_count == renderer->skin_capacity)
    {
        renderer->skin_capacity = renderer->skin_capacity == 0 ? 64 : renderer->skin_capacity * 2;
        renderer->skins =
            tb_realloc_array(renderer->skins, renderer->skin_capacity, sizeof(*renderer->skins));
    }
    renderer->skins[renderer->skin_count] = *skin;
    renderer->skin_count++;
}

void tb_renderer_draw_skinned(tb_renderer *renderer, const tb_skinned_draw *draw)
{
    const tb_skinned_mesh *mesh = NULL;
    tb_renderer_skin skin = {0};
    tb_renderer_draw added = {0};

    TB_ASSERT(renderer != NULL && draw != NULL && draw->mesh != NULL && draw->joints != NULL);
    TB_ASSERT_MSG(draw->joint_count > 0 && draw->joint_count <= TB_SKIN_MAX_JOINTS,
                  "a skinned draw of from 1 to 65536 joints");
    TB_ASSERT_MSG(renderer->in_frame, "a skinned mesh drawn outside a frame");
    mesh = draw->mesh;
    if (renderer->skin_pipeline.generation == 0)
    {
        return;
    }
    skin.joints =
        tb_renderer_upload(renderer, draw->joints, draw->joint_count * sizeof(tb_mat4), 16);
    if (skin.joints == UINT64_MAX)
    {
        return; /* the frame's uploads ran out */
    }
    skin.source = mesh->source_index;
    skin.vertex_count = mesh->vertex_count;
    skin.joint_count = (uint32_t)draw->joint_count;
    skin.output = renderer->skinned_bytes;
    renderer->skinned_bytes += (uint64_t)mesh->vertex_count * TB_SKIN_OUTPUT_BYTES;
    tb_renderer_add_skin(renderer, &skin);
    if (draw->material != NULL)
    {
        added = tb_renderer_material_draw(&mesh->mesh, draw->transform, draw->material);
    }
    else
    {
        added = (tb_renderer_draw){.mesh = &mesh->mesh,
                                   .transform = draw->transform,
                                   .color = draw->color,
                                   .shading = draw->shading};
    }
    added.skinned = true;
    added.skinned_offset = skin.output;
    tb_renderer_add_draw(renderer, &added);
}

/* ---------------------------------------------------------------------------
 * The skinning pass.
 * ------------------------------------------------------------------------- */

static void tb_renderer_record_skinning(const tb_render_graph_pass_context *pass)
{
    const tb_skinning_recording *const recording = pass->context;
    tb_renderer *const renderer = recording->renderer;
    const tb_rhi_buffer vertices = tb_render_graph_rhi_buffer(pass->graph, recording->vertices);
    const uint32_t dest = tb_rhi_buffer_storage_index(renderer->device, vertices);
    const uint32_t uploads =
        tb_rhi_buffer_index(renderer->device, renderer->frames[renderer->slot].upload);

    tb_rhi_cmd_pipeline(pass->list, renderer->skin_pipeline);
    for (size_t index = 0; index < renderer->skin_count; index++)
    {
        const tb_renderer_skin *const skin = &renderer->skins[index];
        const tb_skin_constants constants = {.source = skin->source,
                                             .joints = uploads,
                                             .joint_offset = (uint32_t)skin->joints,
                                             .joint_count = skin->joint_count,
                                             .dest = dest,
                                             .dest_offset = (uint32_t)skin->output,
                                             .vertex_count = skin->vertex_count};
        tb_rhi_cmd_push_constants(pass->list, &constants, sizeof(constants));
        tb_rhi_cmd_dispatch(pass->list, (skin->vertex_count + TB_SKIN_GROUP - 1u) / TB_SKIN_GROUP,
                            1, 1);
    }
}

/* Bytes of the frame's skinned vertices, rounded up to a power of two, so
 * that frames that skin about as much reuse one buffer. */
static uint64_t tb_skinning_buffer_size(uint64_t bytes)
{
    uint64_t size = TB_SKIN_MIN_BUFFER;

    while (size < bytes)
    {
        size *= 2;
    }
    return size;
}

void tb_renderer_skinning_record(tb_renderer *renderer, tb_skinning_recording *recording)
{
    tb_render_graph_pass pass = {0};

    recording->renderer = renderer;
    recording->vertices = (tb_render_graph_buffer){0};
    if (renderer->skin_count == 0)
    {
        return;
    }
    TB_ASSERT_MSG(renderer->skinned_bytes <= UINT32_MAX, "a frame's skinned vertices in 4 GiB");
    recording->vertices = tb_render_graph_create_buffer(
        renderer->graph,
        &(tb_rhi_buffer_desc){.size = tb_skinning_buffer_size(renderer->skinned_bytes),
                              .usage = TB_RHI_BUFFER_STORAGE | TB_RHI_BUFFER_VERTEX},
        "skinned vertices");
    pass = (tb_render_graph_pass){
        .name = "skinning", .execute = tb_renderer_record_skinning, .context = recording};
    pass.accesses[0] =
        (tb_render_graph_access){.buffer = recording->vertices, .state = TB_RHI_STATE_STORAGE};
    pass.access_count = 1;
    tb_render_graph_add_pass(renderer->graph, &pass);
}

/* ---------------------------------------------------------------------------
 * The renderer's part.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_renderer_skinning_create(tb_renderer *renderer, tb_rhi_error *error)
{
    tb_rhi_shader shader = {0};
    tb_rhi_result result = TB_RHI_OK;

    if ((renderer->info.features & TB_RHI_FEATURE_BINDLESS) == 0)
    {
        /* The skinning shader reaches vertices and joints by index; without,
         * skinned meshes are not drawn. */
        return TB_RHI_OK;
    }
    result = tb_renderer_shader(renderer, "/Engine/Private/Skinning.hlsl", tb_skin_source, "MainCS",
                                TB_RHI_STAGE_COMPUTE, &shader, error);
    if (result == TB_RHI_OK)
    {
        result = tb_rhi_compute_pipeline_create(
            renderer->device,
            &(tb_rhi_compute_pipeline_desc){.compute = shader, .name = "skinning"},
            &renderer->skin_pipeline, error);
    }
    tb_rhi_shader_destroy(renderer->device, shader);
    return result;
}

void tb_renderer_skinning_destroy(tb_renderer *renderer)
{
    tb_rhi_pipeline_destroy(renderer->device, renderer->skin_pipeline);
    tb_free(renderer->skins);
    renderer->skins = NULL;
}

void tb_renderer_skinning_begin_frame(tb_renderer *renderer)
{
    renderer->skin_count = 0;
    renderer->skinned_bytes = 0;
}
