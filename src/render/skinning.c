/* Skinned meshes: their vertices on the GPU and in memory, the frame's
 * skinned draws with their joint matrices in the frame's uploads, and the
 * compute pass that skins them into a buffer the scene pass draws from; or,
 * where the GPU cannot skin, their vertices skinned on the CPU into the
 * frame's uploads. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/log.h"
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
/* The alignment of a draw's vertices skinned on the CPU, in the uploads. */
#define TB_SKIN_CPU_ALIGNMENT 16u

TB_STATIC_ASSERT(sizeof(tb_skinned_vertex) == TB_SKIN_SOURCE_BYTES,
                 "a skinned vertex as the shader reads it");
TB_STATIC_ASSERT(sizeof(tb_mesh_vertex) == TB_SKIN_OUTPUT_BYTES,
                 "a mesh vertex as the shader writes it");
TB_STATIC_ASSERT(sizeof(tb_mat4) == TB_SKIN_JOINT_BYTES, "a joint's matrix as the shader reads it");
TB_STATIC_ASSERT(TB_SKINNED_MESH_MAX_VERTICES == TB_SKIN_GROUP * 65535u,
                 "the vertices of one dispatch's groups");

struct tb_skinned_mesh
{
    tb_mesh mesh; /* its indices; its vertices are the frame's skinned ones */
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

/* ---------------------------------------------------------------------------
 * Meshes.
 * ------------------------------------------------------------------------- */

tb_skinned_mesh *tb_skinned_mesh_create(tb_renderer *renderer, const tb_skinned_vertex *vertices,
                                        size_t vertex_count, const uint32_t *indices,
                                        size_t index_count)
{
    TB_ASSERT(renderer != NULL && vertices != NULL && indices != NULL);
    TB_ASSERT_MSG(vertex_count > 0 && vertex_count <= TB_SKINNED_MESH_MAX_VERTICES,
                  "a skinned mesh of from 1 to TB_SKINNED_MESH_MAX_VERTICES vertices");
    TB_ASSERT_MSG(index_count > 0 && index_count % 3 == 0 && index_count <= UINT32_MAX,
                  "a mesh of whole triangles");
    tb_skinned_mesh *const mesh = tb_calloc(1, sizeof(*mesh));
    mesh->vertex_count = (uint32_t)vertex_count;
    tb_renderer_mesh_init(renderer, &mesh->mesh, vertices, vertex_count * sizeof(*vertices),
                          indices, index_count, true);
    mesh->source_index = tb_rhi_buffer_index(renderer->device, mesh->mesh.vertices);
    return mesh;
}

void tb_skinned_mesh_destroy(tb_renderer *renderer, tb_skinned_mesh *mesh)
{
    TB_ASSERT(renderer != NULL);
    if (mesh == NULL)
    {
        return;
    }
    tb_renderer_mesh_release(renderer, &mesh->mesh);
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

/* Give a draw's mesh to the skinning pass, its joints to the frame's
 * uploads: where in the frame's skinned vertices the pass writes it, or
 * UINT64_MAX when the uploads have no room for its joints. */
static uint64_t tb_renderer_skin_on_gpu(tb_renderer *renderer, const tb_skinned_draw *draw)
{
    const tb_skinned_mesh *const mesh = draw->mesh;
    tb_renderer_skin skin = {0};

    skin.joints =
        tb_renderer_upload(renderer, draw->joints, draw->joint_count * sizeof(tb_mat4), 16);
    if (skin.joints == UINT64_MAX)
    {
        return UINT64_MAX;
    }
    skin.source = mesh->source_index;
    skin.vertex_count = mesh->vertex_count;
    skin.joint_count = (uint32_t)draw->joint_count;
    skin.output = renderer->skinned_bytes;
    renderer->skinned_bytes += (uint64_t)mesh->vertex_count * TB_SKIN_OUTPUT_BYTES;
    tb_renderer_add_skin(renderer, &skin);
    return skin.output;
}

/* Skin a draw's vertices on the CPU into the frame's uploads: where they
 * are, or UINT64_MAX when the uploads have no room for them. */
static uint64_t tb_renderer_skin_on_cpu(tb_renderer *renderer, const tb_skinned_draw *draw)
{
    const tb_skinned_mesh *const mesh = draw->mesh;
    const size_t vertex_count = mesh->vertex_count;
    const size_t bytes = vertex_count * sizeof(tb_mesh_vertex);

    if (bytes > tb_renderer_upload_room(renderer, TB_SKIN_CPU_ALIGNMENT))
    {
        return UINT64_MAX;
    }
    if (vertex_count > renderer->cpu_skinned_capacity)
    {
        renderer->cpu_skinned =
            tb_realloc_array(renderer->cpu_skinned, vertex_count, sizeof(*renderer->cpu_skinned));
        renderer->cpu_skinned_capacity = vertex_count;
    }
    tb_skin_vertices(mesh->mesh.cpu_vertices, vertex_count, draw->joints, draw->joint_count,
                     renderer->cpu_skinned);
    return tb_renderer_upload(renderer, renderer->cpu_skinned, bytes, TB_SKIN_CPU_ALIGNMENT);
}

void tb_renderer_draw_skinned(tb_renderer *renderer, const tb_skinned_draw *draw)
{
    const tb_skinned_mesh *mesh = NULL;
    tb_renderer_draw added = {0};
    uint64_t offset = 0;

    TB_ASSERT(renderer != NULL && draw != NULL && draw->mesh != NULL && draw->joints != NULL);
    TB_ASSERT_MSG(draw->joint_count > 0 && draw->joint_count <= TB_SKIN_MAX_JOINTS,
                  "a skinned draw of from 1 to 65536 joints");
    TB_ASSERT_MSG(renderer->in_frame, "a skinned mesh drawn outside a frame");
    mesh = draw->mesh;
    offset = renderer->skin_on_cpu ? tb_renderer_skin_on_cpu(renderer, draw)
                                   : tb_renderer_skin_on_gpu(renderer, draw);
    if (offset == UINT64_MAX)
    {
        return; /* the frame's uploads ran out */
    }
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
    added.skinned_on_cpu = renderer->skin_on_cpu;
    added.skinned_offset = offset;
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

/* Say where the renderer skins, and why. */
static void tb_renderer_skinning_log(const tb_renderer *renderer)
{
    tb_skinning_info info = {0};

    if (renderer->log == NULL)
    {
        return;
    }
    tb_renderer_skinning_info(renderer, &info);
    (void)tb_log_printf(renderer->log, TB_LOG_INFO, "renderer", "skinning on the %s: %s",
                        tb_skinning_path_name(info.path), info.reason);
}

/* The skinning pipeline, or why the GPU cannot skin. */
static void tb_renderer_skinning_pipeline(tb_renderer *renderer)
{
    tb_rhi_shader shader = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_rhi_result result = TB_RHI_OK;

    if ((renderer->info.features & TB_RHI_FEATURE_HEAP_INDICES) == 0)
    {
        /* The skinning shader reaches vertices and joints by index. */
        (void)tb_fmt_buffer(renderer->skin_unavailable, sizeof(renderer->skin_unavailable), "%s",
                            "the device has no bindless resources");
        return;
    }
    result = tb_renderer_shader(renderer, "/Engine/Private/Skinning.hlsl", NULL, "MainCS",
                                TB_RHI_STAGE_COMPUTE, &shader, &error);
    if (result == TB_RHI_OK)
    {
        result = tb_rhi_compute_pipeline_create(
            renderer->device,
            &(tb_rhi_compute_pipeline_desc){.compute = shader, .name = "skinning"},
            &renderer->skin_pipeline, &error);
    }
    tb_rhi_shader_destroy(renderer->device, shader);
    /* coverage-off: the shader is the renderer's own, so only a broken backend refuses it */
    if (result != TB_RHI_OK)
    {
        (void)tb_fmt_buffer(renderer->skin_unavailable, sizeof(renderer->skin_unavailable),
                            "the skinning pipeline could not be made: %s", error.message);
    }
    /* coverage-on */
}

tb_rhi_result tb_renderer_skinning_create(tb_renderer *renderer, tb_rhi_error *error)
{
    (void)error;
    tb_renderer_skinning_pipeline(renderer);
    renderer->skin_on_cpu = renderer->skin_unavailable[0] != '\0';
    tb_renderer_skinning_log(renderer);
    return TB_RHI_OK;
}

void tb_renderer_skinning_destroy(tb_renderer *renderer)
{
    tb_rhi_pipeline_destroy(renderer->device, renderer->skin_pipeline);
    tb_free(renderer->skins);
    renderer->skins = NULL;
    tb_free(renderer->cpu_skinned);
    renderer->cpu_skinned = NULL;
}

/* Whether the CPU skins the next frame. */
static bool tb_renderer_skins_on_cpu(const tb_renderer *renderer)
{
    return renderer->skin_cpu_forced || renderer->skin_unavailable[0] != '\0';
}

void tb_renderer_skinning_begin_frame(tb_renderer *renderer)
{
    const bool on_cpu = tb_renderer_skins_on_cpu(renderer);

    renderer->skin_count = 0;
    renderer->skinned_bytes = 0;
    if (on_cpu != renderer->skin_on_cpu)
    {
        renderer->skin_on_cpu = on_cpu;
        tb_renderer_skinning_log(renderer);
    }
}

/* ---------------------------------------------------------------------------
 * Which path skins.
 * ------------------------------------------------------------------------- */

void tb_renderer_skinning_info(const tb_renderer *renderer, tb_skinning_info *info)
{
    bool on_cpu = false;

    TB_ASSERT(renderer != NULL && info != NULL);
    on_cpu = renderer->in_frame ? renderer->skin_on_cpu : tb_renderer_skins_on_cpu(renderer);
    info->path = on_cpu ? TB_SKINNING_CPU : TB_SKINNING_GPU;
    info->reason = "a compute pass";
    if (renderer->skin_unavailable[0] != '\0')
    {
        info->reason = renderer->skin_unavailable;
    }
    else if (on_cpu)
    {
        info->reason = "forced";
    }
}

void tb_renderer_force_cpu_skinning(tb_renderer *renderer, bool force)
{
    TB_ASSERT(renderer != NULL);
    renderer->skin_cpu_forced = force;
}
