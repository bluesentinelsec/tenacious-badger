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
    TB_ASSERT(!compute);
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
void tb_metal_cmd_draw(void *object, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance)
{
    metal_list *const list = object;
    tb_metal_device *const d = list->device;
    tb_metal_set(d, list->render, "setRenderPipelineState:", list->pipeline->state);
    const char *const bytes[] = {"setVertexBytes:length:atIndex:",
                                 "setFragmentBytes:length:atIndex:"};
    const char *const buffers[] = {"setVertexBuffer:offset:atIndex:",
                                   "setFragmentBuffer:offset:atIndex:"};
    for (size_t stage = 0; stage < 2; stage++)
    {
        TB_METAL_METHOD(d, void, const void *, unsigned long, unsigned long)(
            list->render, d->selector(bytes[stage]), list->constants, sizeof(list->constants), 0ul);
        for (size_t slot = 0; slot < TB_RHI_CONSTANT_BUFFER_SLOTS; slot++)
        {
            TB_METAL_METHOD(d, void, void *, unsigned long,
                            unsigned long)(list->render, d->selector(buffers[stage]),
                                           list->buffers[slot], list->offsets[slot], slot + 1);
        }
    }
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
