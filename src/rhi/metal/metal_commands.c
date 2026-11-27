#include "metal_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include <string.h>

typedef struct metal_extent
{
    unsigned long x, y, z;
} metal_extent;
typedef struct metal_color
{
    double r, g, b, a;
} metal_color;
typedef struct metal_viewport
{
    double x, y, width, height, near_depth, far_depth;
} metal_viewport;
typedef struct metal_scissor
{
    unsigned long x, y, width, height;
} metal_scissor;
typedef struct metal_list
{
    tb_metal_device *device;
    void *commands;
    void *render;
    tb_metal_pipeline *pipeline;
    unsigned char constants[TB_RHI_PUSH_CONSTANT_BYTES];
    void *buffers[TB_RHI_CONSTANT_BUFFER_SLOTS];
    uint64_t offsets[TB_RHI_CONSTANT_BUFFER_SLOTS];
    void *vertices[TB_RHI_VERTEX_BUFFERS];
    uint64_t vertex_offsets[TB_RHI_VERTEX_BUFFERS];
    void *indices;
    uint64_t index_offset;
    unsigned long index_type;
    uint32_t width, height;
} metal_list;

void *tb_metal_list_begin(void *state)
{
    tb_metal_device *const d = state;
    metal_list *const list = tb_calloc(1, sizeof(*list));
    list->device = d;
    void *const pool = d->pool_push();
    list->commands = tb_metal_get(d, tb_metal_get(d, d->queue, "commandBuffer"), "retain");
    TB_ASSERT_MSG(list->commands != NULL, "Metal could not allocate a command buffer");
    d->pool_pop(pool);
    return list;
}
void tb_metal_list_discard(void *state, void *object)
{
    metal_list *const list = object;
    if (list->render != NULL)
    {
        tb_metal_cmd_end_pass(list);
    }
    tb_metal_release(state, list->commands);
    tb_free(list);
}

tb_rhi_result tb_metal_submit(void *state, void *const *lists, size_t count, uint64_t submission)
{
    tb_metal_device *const d = state;
    void *const pool = d->pool_push();
    bool lost = false;
    for (size_t index = 0; index < count; index++)
    {
        metal_list *const list = lists[index];
        tb_metal_call(d, list->commands, "commit");
    }
    for (size_t index = 0; index < count; index++)
    {
        metal_list *const list = lists[index];
        tb_metal_call(d, list->commands, "waitUntilCompleted");
        const unsigned long status =
            ((unsigned long (*)(void *, void *))d->send)(list->commands, d->selector("status"));
        lost = lost || status != 4; /* MTLCommandBufferStatusCompleted */
        tb_metal_list_discard(d, list);
    }
    d->pool_pop(pool);
    tb_mutex_lock(&d->lock);
    d->completed = submission;
    d->lost = d->lost || lost;
    tb_condition_wake_all(&d->finished);
    tb_mutex_unlock(&d->lock);
    return lost ? TB_RHI_DEVICE_LOST : TB_RHI_OK;
}

void tb_metal_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count)
{
    /* Tracked Metal resources are ordered across encoders on this command queue. */
    (void)list;
    (void)barriers;
    (void)count;
}
void tb_metal_cmd_begin_pass(void *object, const tb_rhi_pass_ref *pass)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    void *const pool = d->pool_push();
    void *const desc =
        tb_metal_get(d, d->get_class("MTLRenderPassDescriptor"), "renderPassDescriptor");
    for (uint32_t i = 0; i < pass->color_count; i++)
    {
        const tb_rhi_color_target *const color = &pass->colors[i];
        void *const attachment = tb_metal_color(d, desc, i);
        tb_metal_set(d, attachment, "setTexture:", pass->color_textures[i].object);
        tb_metal_number(d, attachment, "setLevel:", color->mip);
        tb_metal_number(d, attachment, "setSlice:", color->layer);
        const unsigned long loads[] = {1, 2, 0}; /* Load, Clear, DontCare */
        tb_metal_number(d, attachment, "setLoadAction:", loads[color->load]);
        tb_metal_number(d, attachment, "setStoreAction:", 1); /* Store */
        const metal_color clear = {(double)color->clear[0], (double)color->clear[1],
                                   (double)color->clear[2], (double)color->clear[3]};
        TB_METAL_METHOD(d, void, metal_color)(attachment, d->selector("setClearColor:"), clear);
    }
    if (pass->depth_texture.object != NULL)
    {
        void *const attachment = tb_metal_get(d, desc, "depthAttachment");
        const unsigned long loads[] = {1, 2, 0};
        tb_metal_set(d, attachment, "setTexture:", pass->depth_texture.object);
        tb_metal_number(d, attachment, "setLoadAction:", loads[pass->depth.load]);
        tb_metal_number(d, attachment, "setStoreAction:", 1);
        TB_METAL_METHOD(d, void, double)(attachment, d->selector("setClearDepth:"),
                                         (double)pass->depth.clear);
    }
    list->width = pass->width;
    list->height = pass->height;
    void *const encoder = TB_METAL_METHOD(d, void *, void *)(
        list->commands, d->selector("renderCommandEncoderWithDescriptor:"), desc);
    TB_ASSERT_MSG(encoder != NULL, "Metal refused the render pass");
    list->render = tb_metal_get(d, encoder, "retain");
    d->pool_pop(pool);
}

void tb_metal_cmd_end_pass(void *object)
{
    metal_list *const list = object;
    tb_metal_call(list->device, list->render, "endEncoding");
    tb_metal_release(list->device, list->render);
    list->render = NULL;
}
void tb_metal_cmd_viewport(void *object, float x, float y, float width, float height,
                           float near_depth, float far_depth)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    const metal_viewport viewport = {(double)x,      (double)y,          (double)width,
                                     (double)height, (double)near_depth, (double)far_depth};
    TB_METAL_METHOD(d, void, metal_viewport)(list->render, d->selector("setViewport:"), viewport);
}
static unsigned long clamp_edge(int64_t edge, uint32_t limit)
{
    return edge<0 ? 0ul : edge>(int64_t) limit ? limit : (unsigned long)edge;
}
void tb_metal_cmd_scissor(void *object, tb_rhi_rect rect)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    const unsigned long x = clamp_edge(rect.x, list->width);
    const unsigned long y = clamp_edge(rect.y, list->height);
    const metal_scissor scissor = {x, y, clamp_edge((int64_t)rect.x + rect.width, list->width) - x,
                                   clamp_edge((int64_t)rect.y + rect.height, list->height) - y};
    TB_METAL_METHOD(d, void, metal_scissor)(list->render, d->selector("setScissorRect:"), scissor);
}
void tb_metal_cmd_pipeline(void *object, void *pipeline, bool compute)
{
    metal_list *const list = object;
    (void)compute;
    list->pipeline = pipeline;
}
void tb_metal_cmd_push_constants(void *object, const void *data, size_t size)
{
    metal_list *const list = object;
    memcpy(list->constants, data, size);
}
void tb_metal_cmd_constant_buffer(void *object, uint32_t slot, void *buffer, uint64_t offset)
{
    metal_list *const list = object;
    list->buffers[slot] = buffer;
    list->offsets[slot] = offset;
}
void tb_metal_cmd_index_buffer(void *object, void *buffer, uint64_t offset, uint64_t size,
                               tb_rhi_format format)
{
    metal_list *const list = object;
    (void)size;
    list->indices = buffer;
    list->index_offset = offset;
    list->index_type = format == TB_RHI_FORMAT_R16_UINT ? 0ul : 1ul;
}
void tb_metal_cmd_vertex_buffer(void *object, uint32_t slot, void *buffer, uint64_t offset,
                                uint64_t size, uint32_t stride)
{
    metal_list *const list = object;
    (void)size;
    (void)stride;
    list->vertices[slot] = buffer;
    list->vertex_offsets[slot] = offset;
}
static uint32_t uniform_index(metal_list *list, const tb_msl_slot *slot)
{
    const uint8_t *bytes = list->constants;
    uint64_t size = sizeof(list->constants);
    if (slot->buffer != 0)
    {
        const size_t index = slot->buffer - 1;
        tb_metal_device *const d = list->device;
        void *const buffer = list->buffers[index];
        const uint64_t length =
            ((unsigned long (*)(void *, void *))d->send)(buffer, d->selector("length"));
        if (list->offsets[index] > length)
        {
            return TB_RHI_NO_INDEX;
        }
        bytes = tb_metal_get(d, buffer, "contents");
        if (bytes == NULL)
        {
            return TB_RHI_NO_INDEX;
        }
        bytes += list->offsets[index];
        size = length - list->offsets[index];
    }
    if (slot->offset > size || size - slot->offset < sizeof(uint32_t))
    {
        return TB_RHI_NO_INDEX;
    }
    uint32_t value = 0;
    memcpy(&value, bytes + slot->offset, sizeof(value));
    return value;
}
static void bind_stage(metal_list *list, void *encoder, tb_rhi_stage stage)
{
    static const char *const bytes[] = {
        "setVertexBytes:length:atIndex:", "setFragmentBytes:length:atIndex:",
        "setBytes:length:atIndex:"};
    static const char *const buffers[] = {
        "setVertexBuffer:offset:atIndex:", "setFragmentBuffer:offset:atIndex:",
        "setBuffer:offset:atIndex:"};
    static const char *const textures[] = {
        "setVertexTexture:atIndex:", "setFragmentTexture:atIndex:", "setTexture:atIndex:"};
    static const char *const samplers[] = {
        "setVertexSamplerState:atIndex:", "setFragmentSamplerState:atIndex:",
        "setSamplerState:atIndex:"};
    tb_metal_device *const d = list->device;
    void *const set_buffer = d->selector(buffers[stage]);
    TB_METAL_METHOD(d, void, const void *, unsigned long, unsigned long)(
        encoder, d->selector(bytes[stage]), list->constants, sizeof(list->constants), 0ul);
    for (size_t i = 0; i < TB_RHI_CONSTANT_BUFFER_SLOTS; i++)
    {
        TB_METAL_METHOD(d, void, void *, unsigned long, unsigned long)(
            encoder, set_buffer, list->buffers[i], list->offsets[i], i + 1);
    }
    if (!d->argument_buffers)
    {
        const tb_metal_bindings *const bindings = &list->pipeline->stages[stage];
        tb_mutex_lock(&d->lock);
        for (uint32_t i = 0; i < bindings->count; i++)
        {
            const tb_msl_slot *const slot = &bindings->slots[i];
            void *const resource = tb_metal_resource_at(d, slot->heap, uniform_index(list, slot));
            if (slot->heap == TB_MSL_BUFFERS)
            {
                TB_METAL_METHOD(d, void, void *, unsigned long,
                                unsigned long)(encoder, set_buffer, resource, 0ul, slot->slot);
            }
            else
            {
                void *const method =
                    d->selector(slot->heap == TB_MSL_SAMPLERS ? samplers[stage] : textures[stage]);
                TB_METAL_METHOD(d, void, void *, unsigned long)(encoder, method, resource,
                                                                slot->slot);
            }
        }
        tb_mutex_unlock(&d->lock);
        return;
    }
    for (size_t i = 0; i < TB_MSL_HEAP_COUNT; i++)
    {
        TB_METAL_METHOD(d, void, void *, unsigned long, unsigned long)(
            encoder, set_buffer, d->heaps[i], 0ul, TB_MSL_FIRST_HEAP_SLOT + i);
    }
}
static void declare_residency(tb_metal_device *d, void *encoder, bool compute)
{
    if (!d->argument_buffers)
    {
        return;
    }
    tb_mutex_lock(&d->lock);
    for (size_t i = 0; i < tb_array_length(d->residents); i++)
    {
        const tb_metal_resident *const entry = TB_ARRAY_AT(d->residents, tb_metal_resident, i);
        if (entry->heap != TB_MSL_SAMPLERS)
        {
            if (compute)
            {
                TB_METAL_METHOD(d, void, void *, unsigned long)(
                    encoder, d->selector("useResource:usage:"), entry->resource, entry->usage);
            }
            else
            {
                TB_METAL_METHOD(d, void, void *, unsigned long,
                                unsigned long)(encoder, d->selector("useResource:usage:stages:"),
                                               entry->resource, entry->usage, 3ul);
            }
        }
    }
    tb_mutex_unlock(&d->lock);
}

static void prepare_draw(metal_list *list)
{
    tb_metal_device *const d = list->device;
    tb_metal_pipeline *const p = list->pipeline;
    tb_metal_set(d, list->render, "setRenderPipelineState:", p->state);
    tb_metal_set(d, list->render, "setDepthStencilState:", p->depth);
    tb_metal_number(d, list->render, "setCullMode:", p->cull);
    tb_metal_number(d, list->render, "setFrontFacingWinding:", p->winding);
    tb_metal_number(d, list->render, "setTriangleFillMode:", p->wireframe ? 1ul : 0ul);
    TB_METAL_METHOD(d, void, float, float, float)(
        list->render, d->selector("setDepthBias:slopeScale:clamp:"), p->bias, p->slope, 0.0f);
    bind_stage(list, list->render, TB_RHI_STAGE_VERTEX);
    bind_stage(list, list->render, TB_RHI_STAGE_PIXEL);
    for (size_t i = 0; i < TB_RHI_VERTEX_BUFFERS; i++)
    {
        if (list->vertices[i] != NULL)
        {
            TB_METAL_METHOD(d, void, void *, unsigned long, unsigned long)(
                list->render, d->selector("setVertexBuffer:offset:atIndex:"), list->vertices[i],
                list->vertex_offsets[i], TB_MSL_FIRST_VERTEX_SLOT + i);
        }
    }
    declare_residency(d, list->render, false);
}
void tb_metal_cmd_dispatch(void *object, uint32_t x, uint32_t y, uint32_t z)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    void *const pool = d->pool_push();
    void *const encoder = tb_metal_get(d, list->commands, "computeCommandEncoder");
    tb_metal_set(d, encoder, "setComputePipelineState:", list->pipeline->state);
    bind_stage(list, encoder, TB_RHI_STAGE_COMPUTE);
    declare_residency(d, encoder, true);
    const metal_extent groups = {x, y, z};
    const unsigned long *const group = list->pipeline->group;
    const metal_extent threads = {group[0], group[1], group[2]};
    TB_METAL_METHOD(d, void, metal_extent, metal_extent)(
        encoder, d->selector("dispatchThreadgroups:threadsPerThreadgroup:"), groups, threads);
    tb_metal_call(d, encoder, "endEncoding");
    d->pool_pop(pool);
}
void tb_metal_cmd_draw_indexed(void *object, uint32_t indices, uint32_t instances, uint32_t first,
                               int32_t vertex, uint32_t instance)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    prepare_draw(list);
    const unsigned long offset =
        list->index_offset + (unsigned long)first * (list->index_type == 0 ? 2ul : 4ul);
    TB_METAL_METHOD(d, void, unsigned long, unsigned long, unsigned long, void *, unsigned long,
                    unsigned long, long, unsigned long)(
        list->render,
        d->selector("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"
                    "instanceCount:baseVertex:baseInstance:"),
        list->pipeline->primitive, indices, list->index_type, list->indices, offset, instances,
        vertex, instance);
}
void tb_metal_cmd_draw(void *object, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    prepare_draw(list);
    TB_METAL_METHOD(d, void, unsigned long, unsigned long, unsigned long, unsigned long,
                    unsigned long)(
        list->render,
        d->selector("drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:"),
        list->pipeline->primitive, first_vertex, vertices, instances, first_instance);
}

void tb_metal_cmd_copy_buffer(void *object, void *dest, uint64_t dest_offset, void *source,
                              uint64_t source_offset, uint64_t size)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    void *const pool = d->pool_push();
    void *const encoder = tb_metal_get(d, list->commands, "blitCommandEncoder");
    TB_METAL_METHOD(d, void, void *, unsigned long, void *, unsigned long, unsigned long)(
        encoder, d->selector("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
        source, source_offset, dest, dest_offset, size);
    tb_metal_call(d, encoder, "endEncoding");
    d->pool_pop(pool);
}

static void texture_copy(void *object, const tb_rhi_texture_copy_ref *copy, bool upload)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    void *const pool = d->pool_push();
    void *const encoder = tb_metal_get(d, list->commands, "blitCommandEncoder");
    const metal_extent origin = {(unsigned long)copy->region.x, (unsigned long)copy->region.y, 0};
    const metal_extent extent = {copy->region.width, copy->region.height, 1};
    if (upload)
    {
        TB_METAL_METHOD(d, void, void *, unsigned long, unsigned long, unsigned long, metal_extent,
                        void *, unsigned long, unsigned long, metal_extent)(
            encoder,
            d->selector(
                "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:"
                "toTexture:destinationSlice:destinationLevel:destinationOrigin:"),
            copy->buffer, copy->offset, copy->row_pitch, 0ul, extent, copy->texture.object,
            copy->layer, copy->mip, origin);
    }
    else
    {
        TB_METAL_METHOD(d, void, void *, unsigned long, unsigned long, metal_extent, metal_extent,
                        void *, unsigned long, unsigned long, unsigned long)(
            encoder,
            d->selector("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:"
                        "destinationOffset:destinationBytesPerRow:destinationBytesPerImage:"),
            copy->texture.object, copy->layer, copy->mip, origin, extent, copy->buffer,
            copy->offset, copy->row_pitch, 0ul);
    }
    tb_metal_call(d, encoder, "endEncoding");
    d->pool_pop(pool);
}
void tb_metal_cmd_copy_buffer_to_texture(void *list, const tb_rhi_texture_copy_ref *copy)
{
    texture_copy(list, copy, true);
}
void tb_metal_cmd_copy_texture_to_buffer(void *list, const tb_rhi_texture_copy_ref *copy)
{
    texture_copy(list, copy, false);
}
void tb_metal_cmd_begin_marker(void *object, const char *name)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    void *const pool = d->pool_push();
    void *const string = TB_METAL_METHOD(d, void *, const char *)(
        d->get_class("NSString"), d->selector("stringWithUTF8String:"), name);
    tb_metal_set(d, list->commands, "pushDebugGroup:", string);
    d->pool_pop(pool);
}
void tb_metal_cmd_end_marker(void *object)
{
    metal_list *const list = object;
    tb_metal_call(list->device, list->commands, "popDebugGroup");
}
