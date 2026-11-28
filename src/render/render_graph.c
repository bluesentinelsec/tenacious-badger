/* The render graph: passes culled when nothing uses their work, transient
 * resources given pooled memory for the passes between their first and last
 * use, and barriers placed from the states passes declare. */
#include "renderer_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stdarg.h>
#include <string.h>

/* Bytes of a resource's or a pass's name, its NUL included. */
#define TB_RG_NAME_CAPACITY 64u
/* Frames a pooled resource may go unused before it is released. */
#define TB_RG_POOL_FRAMES 3u
/* Accesses of one pass: its own, its color targets and its depth target. */
#define TB_RG_PASS_ACCESSES (TB_RENDER_GRAPH_ACCESSES + TB_RHI_COLOR_TARGETS + 1u)
#define TB_RG_NONE UINT32_MAX

typedef enum tb_rg_kind
{
    TB_RG_TEXTURE,
    TB_RG_BUFFER
} tb_rg_kind;

/* A resource of the frame. */
typedef struct tb_rg_resource
{
    tb_rg_kind kind;
    bool imported;
    tb_rhi_texture texture;
    tb_rhi_buffer buffer;
    tb_rhi_texture_desc texture_desc; /* of a transient texture */
    tb_rhi_buffer_desc buffer_desc;   /* of a transient buffer */
    tb_rhi_state state;               /* while recording */
    tb_rhi_state final_state;         /* of an imported resource */
    uint32_t first;                   /* the first and last kept passes that use it */
    uint32_t last;
    uint32_t pooled; /* its memory, for a transient one; TB_RG_NONE before */
    bool needed;     /* while culling: a kept pass reads it */
    char name[TB_RG_NAME_CAPACITY];
} tb_rg_resource;

/* One use of a resource by a pass, its targets included. */
typedef struct tb_rg_use
{
    uint32_t resource;
    tb_rhi_state state;
    bool writes;
    bool overwrites; /* a target cleared, or drawn over whole: earlier work does not matter */
} tb_rg_use;

typedef struct tb_rg_pass
{
    tb_render_graph_pass desc;
    char name[TB_RG_NAME_CAPACITY];
    tb_rg_use uses[TB_RG_PASS_ACCESSES];
    uint32_t use_count;
    bool kept;
} tb_rg_pass;

/* Memory a transient resource may be given, kept from frame to frame. */
typedef struct tb_rg_pooled
{
    tb_rg_kind kind;
    tb_rhi_texture texture;
    tb_rhi_buffer buffer;
    tb_rhi_texture_desc texture_desc;
    tb_rhi_buffer_desc buffer_desc;
    tb_rhi_state state;
    bool taken;          /* by a resource between its first and last use */
    uint64_t last_frame; /* the frame that last used it */
} tb_rg_pooled;

struct tb_render_graph
{
    tb_rhi_device *device;
    tb_rg_resource *resources;
    uint32_t resource_count;
    uint32_t resource_capacity;
    tb_rg_pass *passes;
    uint32_t pass_count;
    uint32_t pass_capacity;
    tb_rg_pooled *pool;
    uint32_t pool_count;
    uint32_t pool_capacity;
    uint64_t frame;
    bool executing;
    char *dump;
    size_t dump_length;
    size_t dump_capacity;
};

/* ---------------------------------------------------------------------------
 * The dump.
 * ------------------------------------------------------------------------- */

static const char *tb_rg_state_name(tb_rhi_state state)
{
    static const char *const names[] = {
        "common",      "vertex_index", "constant",    "shader_read", "storage",  "render_target",
        "depth_write", "depth_read",   "copy_source", "copy_dest",   "indirect", "present"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

static void tb_rg_write(tb_render_graph *graph, const char *format, ...)
{
    va_list arguments;
    va_list again;
    size_t needed = 0;

    va_start(arguments, format);
    va_copy(again, arguments);
    needed = tb_fmt_vbuffer(NULL, 0, format, arguments);
    va_end(arguments);
    if (graph->dump_length + needed + 1 > graph->dump_capacity)
    {
        graph->dump_capacity = (graph->dump_length + needed + 1) * 2;
        graph->dump = tb_realloc(graph->dump, graph->dump_capacity);
    }
    (void)tb_fmt_vbuffer(graph->dump + graph->dump_length, needed + 1, format, again);
    va_end(again);
    graph->dump_length += needed;
}

size_t tb_render_graph_dump(const tb_render_graph *graph, char *buffer, size_t capacity)
{
    TB_ASSERT(graph != NULL && (buffer != NULL || capacity == 0));
    if (capacity > 0)
    {
        (void)tb_fmt_buffer(buffer, capacity, "%s", graph->dump == NULL ? "" : graph->dump);
    }
    return graph->dump_length;
}

/* ---------------------------------------------------------------------------
 * Graphs.
 * ------------------------------------------------------------------------- */

tb_render_graph *tb_render_graph_create(tb_rhi_device *device)
{
    tb_render_graph *graph = NULL;

    TB_ASSERT(device != NULL);
    graph = tb_calloc(1, sizeof(*graph));
    graph->device = device;
    return graph;
}

static void tb_rg_release_pooled(tb_render_graph *graph, const tb_rg_pooled *pooled)
{
    if (pooled->kind == TB_RG_TEXTURE)
    {
        tb_rhi_texture_destroy(graph->device, pooled->texture);
    }
    else
    {
        tb_rhi_buffer_destroy(graph->device, pooled->buffer);
    }
}

void tb_render_graph_discard_pool(tb_render_graph *graph)
{
    for (uint32_t index = 0; index < graph->pool_count; index++)
    {
        tb_rg_release_pooled(graph, &graph->pool[index]);
    }
    graph->pool_count = 0;
}

void tb_render_graph_destroy(tb_render_graph *graph)
{
    if (graph == NULL)
    {
        return;
    }
    tb_render_graph_discard_pool(graph);
    tb_free(graph->pool);
    tb_free(graph->resources);
    tb_free(graph->passes);
    tb_free(graph->dump);
    tb_free(graph);
}

/* Release memory no frame has used for a while. */
static void tb_rg_trim_pool(tb_render_graph *graph)
{
    uint32_t kept = 0;

    for (uint32_t index = 0; index < graph->pool_count; index++)
    {
        if (graph->frame - graph->pool[index].last_frame > TB_RG_POOL_FRAMES)
        {
            tb_rg_release_pooled(graph, &graph->pool[index]);
            continue;
        }
        graph->pool[kept] = graph->pool[index];
        kept++;
    }
    graph->pool_count = kept;
}

void tb_render_graph_begin(tb_render_graph *graph)
{
    TB_ASSERT(graph != NULL);
    TB_ASSERT_MSG(!graph->executing, "a graph begun while it executes");
    graph->frame++;
    graph->resource_count = 0;
    graph->pass_count = 0;
    tb_rg_trim_pool(graph);
}

size_t tb_render_graph_pooled(const tb_render_graph *graph)
{
    TB_ASSERT(graph != NULL);
    return graph->pool_count;
}

/* ---------------------------------------------------------------------------
 * Resources.
 * ------------------------------------------------------------------------- */

static uint32_t tb_rg_add_resource(tb_render_graph *graph, tb_rg_kind kind, const char *name)
{
    tb_rg_resource *resource = NULL;

    TB_ASSERT(name != NULL);
    if (graph->resource_count == graph->resource_capacity)
    {
        graph->resource_capacity =
            graph->resource_capacity == 0 ? 32 : graph->resource_capacity * 2;
        graph->resources =
            tb_realloc_array(graph->resources, graph->resource_capacity, sizeof(*graph->resources));
    }
    resource = &graph->resources[graph->resource_count];
    *resource = (tb_rg_resource){
        .kind = kind, .first = TB_RG_NONE, .last = TB_RG_NONE, .pooled = TB_RG_NONE};
    (void)tb_fmt_buffer(resource->name, sizeof(resource->name), "%s", name);
    graph->resource_count++;
    return graph->resource_count; /* ids start at 1, so that zero names none */
}

static tb_rg_resource *tb_rg_resource_of(const tb_render_graph *graph, uint32_t id, tb_rg_kind kind)
{
    TB_ASSERT_MSG(id != 0 && id <= graph->resource_count,
                  "a graph handle of another frame, or none");
    TB_ASSERT_MSG(graph->resources[id - 1].kind == kind, "a graph handle of the wrong kind");
    return &graph->resources[id - 1];
}

tb_render_graph_texture tb_render_graph_create_texture(tb_render_graph *graph,
                                                       const tb_rhi_texture_desc *desc,
                                                       const char *name)
{
    tb_render_graph_texture texture = {0};

    TB_ASSERT(graph != NULL && desc != NULL);
    texture.id = tb_rg_add_resource(graph, TB_RG_TEXTURE, name);
    graph->resources[texture.id - 1].texture_desc = *desc;
    graph->resources[texture.id - 1].texture_desc.name = NULL;
    return texture;
}

tb_render_graph_buffer tb_render_graph_create_buffer(tb_render_graph *graph,
                                                     const tb_rhi_buffer_desc *desc,
                                                     const char *name)
{
    tb_render_graph_buffer buffer = {0};

    TB_ASSERT(graph != NULL && desc != NULL);
    TB_ASSERT_MSG(desc->memory == TB_RHI_MEMORY_GPU, "a transient buffer is the GPU's");
    buffer.id = tb_rg_add_resource(graph, TB_RG_BUFFER, name);
    graph->resources[buffer.id - 1].buffer_desc = *desc;
    graph->resources[buffer.id - 1].buffer_desc.name = NULL;
    return buffer;
}

tb_render_graph_texture tb_render_graph_import_texture(tb_render_graph *graph,
                                                       tb_rhi_texture texture, tb_rhi_state state,
                                                       tb_rhi_state final_state, const char *name)
{
    tb_render_graph_texture imported = {0};
    tb_rg_resource *resource = NULL;

    TB_ASSERT(graph != NULL && texture.generation != 0);
    imported.id = tb_rg_add_resource(graph, TB_RG_TEXTURE, name);
    resource = &graph->resources[imported.id - 1];
    resource->imported = true;
    resource->texture = texture;
    resource->state = state;
    resource->final_state = final_state;
    tb_rhi_texture_desc_of(graph->device, texture, &resource->texture_desc);
    return imported;
}

tb_render_graph_buffer tb_render_graph_import_buffer(tb_render_graph *graph, tb_rhi_buffer buffer,
                                                     tb_rhi_state state, tb_rhi_state final_state,
                                                     const char *name)
{
    tb_render_graph_buffer imported = {0};
    tb_rg_resource *resource = NULL;

    TB_ASSERT(graph != NULL && buffer.generation != 0);
    imported.id = tb_rg_add_resource(graph, TB_RG_BUFFER, name);
    resource = &graph->resources[imported.id - 1];
    resource->imported = true;
    resource->buffer = buffer;
    resource->state = state;
    resource->final_state = final_state;
    return imported;
}

tb_rhi_texture tb_render_graph_rhi_texture(const tb_render_graph *graph,
                                           tb_render_graph_texture texture)
{
    TB_ASSERT(graph != NULL);
    TB_ASSERT_MSG(graph->executing, "a graph's texture asked for outside its execution");
    return tb_rg_resource_of(graph, texture.id, TB_RG_TEXTURE)->texture;
}

tb_rhi_buffer tb_render_graph_rhi_buffer(const tb_render_graph *graph,
                                         tb_render_graph_buffer buffer)
{
    TB_ASSERT(graph != NULL);
    TB_ASSERT_MSG(graph->executing, "a graph's buffer asked for outside its execution");
    return tb_rg_resource_of(graph, buffer.id, TB_RG_BUFFER)->buffer;
}

/* ---------------------------------------------------------------------------
 * Passes.
 * ------------------------------------------------------------------------- */

static bool tb_rg_state_writes(tb_rhi_state state)
{
    return state == TB_RHI_STATE_STORAGE || state == TB_RHI_STATE_RENDER_TARGET ||
           state == TB_RHI_STATE_DEPTH_WRITE || state == TB_RHI_STATE_COPY_DEST;
}

static void tb_rg_add_use(tb_render_graph *graph, tb_rg_pass *pass, uint32_t resource,
                          tb_rhi_state state, bool overwrites)
{
    for (uint32_t index = 0; index < pass->use_count; index++)
    {
        TB_ASSERT_MSG(pass->uses[index].resource != resource || pass->uses[index].state == state,
                      "a pass uses one resource in two states");
    }
    (void)graph;
    pass->uses[pass->use_count] = (tb_rg_use){.resource = resource,
                                              .state = state,
                                              .writes = tb_rg_state_writes(state),
                                              .overwrites = overwrites};
    pass->use_count++;
}

void tb_render_graph_add_pass(tb_render_graph *graph, const tb_render_graph_pass *desc)
{
    tb_rg_pass *pass = NULL;

    TB_ASSERT(graph != NULL && desc != NULL && desc->name != NULL && desc->execute != NULL);
    TB_ASSERT_MSG(desc->access_count <= TB_RENDER_GRAPH_ACCESSES, "too many accesses");
    TB_ASSERT_MSG(desc->color_count <= TB_RHI_COLOR_TARGETS, "too many color targets");
    if (graph->pass_count == graph->pass_capacity)
    {
        graph->pass_capacity = graph->pass_capacity == 0 ? 16 : graph->pass_capacity * 2;
        graph->passes =
            tb_realloc_array(graph->passes, graph->pass_capacity, sizeof(*graph->passes));
    }
    pass = &graph->passes[graph->pass_count];
    *pass = (tb_rg_pass){.desc = *desc};
    (void)tb_fmt_buffer(pass->name, sizeof(pass->name), "%s", desc->name);
    pass->desc.name = pass->name;
    for (uint32_t index = 0; index < desc->access_count; index++)
    {
        const tb_render_graph_access *const access = &desc->accesses[index];
        TB_ASSERT_MSG((access->texture.id != 0) != (access->buffer.id != 0),
                      "an access names one texture or one buffer");
        if (access->texture.id != 0)
        {
            (void)tb_rg_resource_of(graph, access->texture.id, TB_RG_TEXTURE);
        }
        else
        {
            (void)tb_rg_resource_of(graph, access->buffer.id, TB_RG_BUFFER);
        }
        tb_rg_add_use(graph, pass, access->texture.id != 0 ? access->texture.id : access->buffer.id,
                      access->state, false);
    }
    for (uint32_t index = 0; index < desc->color_count; index++)
    {
        (void)tb_rg_resource_of(graph, desc->colors[index].texture.id, TB_RG_TEXTURE);
        tb_rg_add_use(graph, pass, desc->colors[index].texture.id, TB_RHI_STATE_RENDER_TARGET,
                      desc->colors[index].load != TB_RHI_LOAD_KEEP);
    }
    if (desc->depth.texture.id != 0)
    {
        (void)tb_rg_resource_of(graph, desc->depth.texture.id, TB_RG_TEXTURE);
        tb_rg_add_use(graph, pass, desc->depth.texture.id,
                      desc->depth.read_only ? TB_RHI_STATE_DEPTH_READ : TB_RHI_STATE_DEPTH_WRITE,
                      !desc->depth.read_only && desc->depth.load != TB_RHI_LOAD_KEEP);
    }
    graph->pass_count++;
}

/* ---------------------------------------------------------------------------
 * Culling and lifetimes.
 * ------------------------------------------------------------------------- */

/* A pass is kept when it never culls, writes an imported resource, or
 * writes what a kept pass after it reads. Walking backward, what a kept pass
 * reads becomes needed; what it overwrites is no longer needed of earlier
 * passes. */
static void tb_rg_cull(tb_render_graph *graph)
{
    for (uint32_t index = 0; index < graph->resource_count; index++)
    {
        graph->resources[index].needed = false;
    }
    for (uint32_t index = graph->pass_count; index-- > 0;)
    {
        tb_rg_pass *const pass = &graph->passes[index];
        pass->kept = pass->desc.never_cull;
        for (uint32_t use = 0; use < pass->use_count && !pass->kept; use++)
        {
            const tb_rg_resource *const resource = &graph->resources[pass->uses[use].resource - 1];
            pass->kept = pass->uses[use].writes && (resource->imported || resource->needed);
        }
        if (!pass->kept)
        {
            continue;
        }
        for (uint32_t use = 0; use < pass->use_count; use++)
        {
            tb_rg_resource *const resource = &graph->resources[pass->uses[use].resource - 1];
            resource->needed = !pass->uses[use].overwrites;
        }
    }
}

static void tb_rg_lifetimes(tb_render_graph *graph)
{
    for (uint32_t index = 0; index < graph->pass_count; index++)
    {
        const tb_rg_pass *const pass = &graph->passes[index];
        if (!pass->kept)
        {
            continue;
        }
        for (uint32_t use = 0; use < pass->use_count; use++)
        {
            tb_rg_resource *const resource = &graph->resources[pass->uses[use].resource - 1];
            resource->first = resource->first == TB_RG_NONE ? index : resource->first;
            resource->last = index;
        }
    }
}

/* The usage a transient texture needs for every state its passes use it in. */
static uint32_t tb_rg_texture_usage(const tb_render_graph *graph, uint32_t id)
{
    uint32_t usage = graph->resources[id - 1].texture_desc.usage;

    for (uint32_t index = 0; index < graph->pass_count; index++)
    {
        const tb_rg_pass *const pass = &graph->passes[index];
        for (uint32_t use = 0; pass->kept && use < pass->use_count; use++)
        {
            if (pass->uses[use].resource != id)
            {
                continue;
            }
            switch (pass->uses[use].state)
            {
            case TB_RHI_STATE_SHADER_READ:
                usage |= TB_RHI_TEXTURE_SAMPLED;
                break;
            case TB_RHI_STATE_STORAGE:
                usage |= TB_RHI_TEXTURE_STORAGE;
                break;
            case TB_RHI_STATE_RENDER_TARGET:
                usage |= TB_RHI_TEXTURE_RENDER_TARGET;
                break;
            case TB_RHI_STATE_DEPTH_WRITE:
            case TB_RHI_STATE_DEPTH_READ:
                usage |= TB_RHI_TEXTURE_DEPTH;
                break;
            case TB_RHI_STATE_COMMON:
            case TB_RHI_STATE_VERTEX_INDEX:
            case TB_RHI_STATE_CONSTANT:
            case TB_RHI_STATE_COPY_SOURCE:
            case TB_RHI_STATE_COPY_DEST:
            case TB_RHI_STATE_INDIRECT:
            case TB_RHI_STATE_PRESENT:
                break;
            }
        }
    }
    return usage;
}

/* ---------------------------------------------------------------------------
 * Memory.
 * ------------------------------------------------------------------------- */

static bool tb_rg_same_texture(const tb_rhi_texture_desc *a, const tb_rhi_texture_desc *b)
{
    return a->kind == b->kind && a->format == b->format && a->width == b->width &&
           a->height == b->height && a->layers == b->layers && a->mips == b->mips &&
           a->samples == b->samples && a->usage == b->usage;
}

static bool tb_rg_same_buffer(const tb_rhi_buffer_desc *a, const tb_rhi_buffer_desc *b)
{
    return a->size == b->size && a->usage == b->usage;
}

/* Memory for a transient resource at its first use: pooled memory of the
 * same description that is free, or new memory. */
static void tb_rg_acquire(tb_render_graph *graph, tb_rg_resource *resource)
{
    tb_rg_pooled *pooled = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    for (uint32_t index = 0; index < graph->pool_count && pooled == NULL; index++)
    {
        tb_rg_pooled *const candidate = &graph->pool[index];
        const bool same =
            candidate->kind == resource->kind &&
            (resource->kind == TB_RG_TEXTURE
                 ? tb_rg_same_texture(&candidate->texture_desc, &resource->texture_desc)
                 : tb_rg_same_buffer(&candidate->buffer_desc, &resource->buffer_desc));
        if (!candidate->taken && same)
        {
            pooled = candidate;
            resource->pooled = index;
        }
    }
    if (pooled == NULL)
    {
        if (graph->pool_count == graph->pool_capacity)
        {
            graph->pool_capacity = graph->pool_capacity == 0 ? 16 : graph->pool_capacity * 2;
            graph->pool = tb_realloc_array(graph->pool, graph->pool_capacity, sizeof(*graph->pool));
        }
        pooled = &graph->pool[graph->pool_count];
        *pooled = (tb_rg_pooled){.kind = resource->kind,
                                 .texture_desc = resource->texture_desc,
                                 .buffer_desc = resource->buffer_desc,
                                 .state = TB_RHI_STATE_COMMON};
        resource->pooled = graph->pool_count;
        graph->pool_count++;
        if (resource->kind == TB_RG_TEXTURE)
        {
            tb_rhi_texture_desc desc = resource->texture_desc;
            desc.name = resource->name;
            if (tb_rhi_texture_create(graph->device, &desc, &pooled->texture, &error) != TB_RHI_OK)
            {
                tb_fatal("render graph: no memory for %s: %s", resource->name, error.message);
            }
        }
        else
        {
            tb_rhi_buffer_desc desc = resource->buffer_desc;
            desc.name = resource->name;
            if (tb_rhi_buffer_create(graph->device, &desc, &pooled->buffer, &error) != TB_RHI_OK)
            {
                tb_fatal("render graph: no memory for %s: %s", resource->name, error.message);
            }
        }
    }
    pooled->taken = true;
    pooled->last_frame = graph->frame;
    resource->texture = pooled->texture;
    resource->buffer = pooled->buffer;
    resource->state = pooled->state;
    tb_rg_write(graph, "  memory %u for %s\n", resource->pooled, resource->name);
}

/* ---------------------------------------------------------------------------
 * Recording.
 * ------------------------------------------------------------------------- */

static tb_rhi_barrier tb_rg_barrier(const tb_rg_resource *resource, tb_rhi_state after)
{
    tb_rhi_barrier barrier = {.before = resource->state, .after = after};

    if (resource->kind == TB_RG_TEXTURE)
    {
        barrier.texture = resource->texture;
    }
    else
    {
        barrier.buffer = resource->buffer;
    }
    return barrier;
}

/* Barriers into the states a pass uses its resources in. Writes by shaders
 * one after the other need a barrier between them too. */
static void tb_rg_transition(tb_render_graph *graph, const tb_rg_pass *pass,
                             tb_rhi_command_list *list)
{
    tb_rhi_barrier barriers[TB_RG_PASS_ACCESSES] = {0};
    size_t count = 0;

    for (uint32_t use = 0; use < pass->use_count; use++)
    {
        tb_rg_resource *const resource = &graph->resources[pass->uses[use].resource - 1];
        const tb_rhi_state state = pass->uses[use].state;
        if (resource->state == state && state != TB_RHI_STATE_STORAGE)
        {
            continue;
        }
        barriers[count] = tb_rg_barrier(resource, state);
        count++;
        tb_rg_write(graph, "  barrier %s %s -> %s\n", resource->name,
                    tb_rg_state_name(resource->state), tb_rg_state_name(state));
        resource->state = state;
    }
    if (count > 0)
    {
        tb_rhi_cmd_barriers(list, barriers, count);
    }
}

static void tb_rg_record(tb_render_graph *graph, const tb_rg_pass *pass, tb_rhi_command_list *list)
{
    const tb_render_graph_pass *const desc = &pass->desc;
    const tb_render_graph_pass_context context = {graph, list, desc->context};
    const bool raster = desc->color_count > 0 || desc->depth.texture.id != 0;
    tb_rhi_pass targets = {.color_count = desc->color_count};

    tb_rhi_cmd_begin_marker(list, pass->name);
    tb_rg_transition(graph, pass, list);
    for (uint32_t index = 0; index < desc->color_count; index++)
    {
        const tb_render_graph_color *const color = &desc->colors[index];
        targets.colors[index] =
            (tb_rhi_color_target){.texture = graph->resources[color->texture.id - 1].texture,
                                  .mip = color->mip,
                                  .layer = color->layer,
                                  .load = color->load};
        memcpy(targets.colors[index].clear, color->clear, sizeof(color->clear));
    }
    if (desc->depth.texture.id != 0)
    {
        targets.depth =
            (tb_rhi_depth_target){.texture = graph->resources[desc->depth.texture.id - 1].texture,
                                  .load = desc->depth.load,
                                  .clear = desc->depth.clear,
                                  .read_only = desc->depth.read_only};
    }
    if (raster)
    {
        tb_rhi_cmd_begin_pass(list, &targets);
    }
    desc->execute(&context);
    if (raster)
    {
        tb_rhi_cmd_end_pass(list);
    }
    tb_rhi_cmd_end_marker(list);
}

/* Give back the memory of transient resources this pass used last. */
static void tb_rg_release_after(tb_render_graph *graph, uint32_t pass)
{
    for (uint32_t index = 0; index < graph->resource_count; index++)
    {
        tb_rg_resource *const resource = &graph->resources[index];
        if (!resource->imported && resource->last == pass)
        {
            graph->pool[resource->pooled].taken = false;
            graph->pool[resource->pooled].state = resource->state;
        }
    }
}

uint64_t tb_render_graph_execute(tb_render_graph *graph)
{
    tb_rhi_command_list *list = NULL;
    tb_rhi_barrier finals[64] = {0};
    size_t final_count = 0;
    uint64_t submission = 0;

    TB_ASSERT(graph != NULL);
    graph->dump_length = 0;
    tb_rg_write(graph, "");
    tb_rg_cull(graph);
    tb_rg_lifetimes(graph);
    graph->executing = true;
    list = tb_rhi_command_list_begin(graph->device);
    for (uint32_t index = 0; index < graph->pass_count; index++)
    {
        const tb_rg_pass *const pass = &graph->passes[index];
        if (!pass->kept)
        {
            tb_rg_write(graph, "culled %s\n", pass->name);
            continue;
        }
        tb_rg_write(graph, "pass %s\n", pass->name);
        for (uint32_t use = 0; use < pass->use_count; use++)
        {
            tb_rg_resource *const resource = &graph->resources[pass->uses[use].resource - 1];
            if (!resource->imported && resource->first == index && resource->pooled == TB_RG_NONE)
            {
                if (resource->kind == TB_RG_TEXTURE)
                {
                    resource->texture_desc.usage =
                        tb_rg_texture_usage(graph, pass->uses[use].resource);
                }
                tb_rg_acquire(graph, resource);
            }
        }
        tb_rg_record(graph, pass, list);
        tb_rg_release_after(graph, index);
    }
    for (uint32_t index = 0; index < graph->resource_count; index++)
    {
        tb_rg_resource *const resource = &graph->resources[index];
        if (resource->imported && resource->state != resource->final_state)
        {
            TB_ASSERT_MSG(final_count < sizeof(finals) / sizeof(finals[0]), "too many imports");
            finals[final_count] = tb_rg_barrier(resource, resource->final_state);
            final_count++;
            tb_rg_write(graph, "final %s %s -> %s\n", resource->name,
                        tb_rg_state_name(resource->state), tb_rg_state_name(resource->final_state));
            resource->state = resource->final_state;
        }
    }
    if (final_count > 0)
    {
        tb_rhi_cmd_barriers(list, finals, final_count);
    }
    submission = tb_rhi_submit(graph->device, &list, 1);
    graph->executing = false;
    return submission;
}
