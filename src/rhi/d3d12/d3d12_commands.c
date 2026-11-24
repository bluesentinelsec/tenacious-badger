/* The D3D12 backend's command lists: allocators and lists reused once the
 * GPU has finished with them, the RHI's states turned into resource
 * barriers, passes into render-target and depth views, and submissions into
 * signals of the device's fence. */
#include "d3d12_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* The PIX event metadata that marks an ANSI string. */
#define TB_D3D12_PIX_ANSI 1u

static void tb_d3d12_list_free(tb_d3d12_list *list)
{
    if (list->targets != NULL)
    {
        (void)ID3D12DescriptorHeap_Release(list->targets);
    }
    if (list->depths != NULL)
    {
        (void)ID3D12DescriptorHeap_Release(list->depths);
    }
    if (list->list != NULL)
    {
        (void)ID3D12GraphicsCommandList_Release(list->list);
    }
    if (list->allocator != NULL)
    {
        (void)ID3D12CommandAllocator_Release(list->allocator);
    }
    tb_free(list);
}

/* A new list, or NULL if D3D12 refuses one. */
static tb_d3d12_list *tb_d3d12_list_make(tb_d3d12_device *device)
{
    const D3D12_DESCRIPTOR_HEAP_DESC targets = {
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV, TB_RHI_COLOR_TARGETS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC depths = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                               D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    tb_d3d12_list *list = tb_calloc(1, sizeof(*list));
    HRESULT result = S_OK;

    list->device = device;
    result =
        ID3D12Device_CreateCommandAllocator(device->device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            &IID_ID3D12CommandAllocator, (void **)&list->allocator);
    if (SUCCEEDED(result))
    {
        result = ID3D12Device_CreateCommandList(
            device->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, list->allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list->list);
    }
    if (SUCCEEDED(result))
    {
        result = ID3D12GraphicsCommandList_Close(list->list);
    }
    if (SUCCEEDED(result))
    {
        result = ID3D12Device_CreateDescriptorHeap(
            device->device, &targets, &IID_ID3D12DescriptorHeap, (void **)&list->targets);
    }
    if (SUCCEEDED(result))
    {
        result = ID3D12Device_CreateDescriptorHeap(
            device->device, &depths, &IID_ID3D12DescriptorHeap, (void **)&list->depths);
    }
    if (FAILED(result))
    {
        tb_d3d12_list_free(list);
        return NULL;
    }
    return list;
}

/* A list the GPU is done with, or a new one. */
static tb_d3d12_list *tb_d3d12_list_take(tb_d3d12_device *device)
{
    const UINT64 completed = ID3D12Fence_GetCompletedValue(device->fence);
    tb_d3d12_list *list = NULL;

    tb_mutex_lock(&device->lists_lock);
    for (size_t index = 0; index < device->idle_count; index++)
    {
        if (device->idle[index]->submission <= completed)
        {
            list = device->idle[index];
            device->idle[index] = device->idle[device->idle_count - 1];
            device->idle_count--;
            break;
        }
    }
    tb_mutex_unlock(&device->lists_lock);
    if (list == NULL)
    {
        list = tb_d3d12_list_make(device);
    }
    if (list == NULL)
    {
        tb_fatal("d3d12: no command list can be made; the device is out of memory or lost");
    }
    return list;
}

static void tb_d3d12_list_give(tb_d3d12_device *device, tb_d3d12_list *list)
{
    tb_mutex_lock(&device->lists_lock);
    if (device->idle_count == device->idle_capacity)
    {
        device->idle_capacity = device->idle_capacity == 0 ? 8 : device->idle_capacity * 2;
        device->idle = tb_realloc_array(device->idle, device->idle_capacity, sizeof(*device->idle));
    }
    device->idle[device->idle_count] = list;
    device->idle_count++;
    tb_mutex_unlock(&device->lists_lock);
}

void tb_d3d12_forget_lists(tb_d3d12_device *device)
{
    tb_mutex_lock(&device->lists_lock);
    for (size_t index = 0; index < device->idle_count; index++)
    {
        tb_d3d12_list *const list = device->idle[index];
        (void)ID3D12CommandAllocator_Reset(list->allocator);
        (void)ID3D12GraphicsCommandList_Reset(list->list, list->allocator, NULL);
        (void)ID3D12GraphicsCommandList_Close(list->list);
    }
    tb_mutex_unlock(&device->lists_lock);
}

void tb_d3d12_release_lists(tb_d3d12_device *device)
{
    for (size_t index = 0; index < device->idle_count; index++)
    {
        tb_d3d12_list_free(device->idle[index]);
    }
    tb_free(device->idle);
    device->idle = NULL;
    device->idle_count = 0;
    device->idle_capacity = 0;
}

void *tb_d3d12_list_begin(void *state)
{
    tb_d3d12_device *const device = state;
    tb_d3d12_list *const list = tb_d3d12_list_take(device);
    ID3D12DescriptorHeap *heaps[2] = {device->resources, device->samplers};

    (void)ID3D12CommandAllocator_Reset(list->allocator);
    (void)ID3D12GraphicsCommandList_Reset(list->list, list->allocator, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list->list, 2, heaps);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list->list, device->root);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list->list, device->root);
    return list;
}

void tb_d3d12_list_discard(void *state, void *list)
{
    tb_d3d12_device *const device = state;
    tb_d3d12_list *const d3d12_list = list;

    (void)ID3D12GraphicsCommandList_Close(d3d12_list->list);
    tb_d3d12_list_give(device, d3d12_list);
}

tb_rhi_result tb_d3d12_submit(void *state, void *const *lists, size_t count, uint64_t submission)
{
    enum
    {
        BATCH = 32
    };
    tb_d3d12_device *const device = state;
    ID3D12CommandList *batch[BATCH] = {0};
    size_t batched = 0;
    HRESULT result = S_OK;

    for (size_t index = 0; index < count; index++)
    {
        tb_d3d12_list *const list = lists[index];
        HRESULT closed = ID3D12GraphicsCommandList_Close(list->list);
        result = FAILED(result) ? result : closed;
        batch[batched] = (ID3D12CommandList *)list->list;
        batched++;
        if (batched == BATCH || index + 1 == count)
        {
            ID3D12CommandQueue_ExecuteCommandLists(device->queue, (UINT)batched, batch);
            batched = 0;
        }
    }
    if (SUCCEEDED(result))
    {
        result = ID3D12CommandQueue_Signal(device->queue, device->fence, submission);
    }
    for (size_t index = 0; index < count; index++)
    {
        tb_d3d12_list *const list = lists[index];
        list->submission = submission;
        tb_d3d12_list_give(device, list);
    }
    tb_d3d12_drain_messages(device);
    if (FAILED(result) || FAILED(ID3D12Device_GetDeviceRemovedReason(device->device)))
    {
        return TB_RHI_DEVICE_LOST;
    }
    return TB_RHI_OK;
}

/* ---------------------------------------------------------------------------
 * Barriers.
 * ------------------------------------------------------------------------- */

static D3D12_RESOURCE_STATES tb_d3d12_state(tb_rhi_state state)
{
    static const D3D12_RESOURCE_STATES states[] = {
        [TB_RHI_STATE_COMMON] = D3D12_RESOURCE_STATE_COMMON,
        [TB_RHI_STATE_VERTEX_INDEX] =
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER,
        [TB_RHI_STATE_CONSTANT] = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        [TB_RHI_STATE_SHADER_READ] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        [TB_RHI_STATE_STORAGE] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        [TB_RHI_STATE_RENDER_TARGET] = D3D12_RESOURCE_STATE_RENDER_TARGET,
        [TB_RHI_STATE_DEPTH_WRITE] = D3D12_RESOURCE_STATE_DEPTH_WRITE,
        [TB_RHI_STATE_DEPTH_READ] = D3D12_RESOURCE_STATE_DEPTH_READ |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        [TB_RHI_STATE_COPY_SOURCE] = D3D12_RESOURCE_STATE_COPY_SOURCE,
        [TB_RHI_STATE_COPY_DEST] = D3D12_RESOURCE_STATE_COPY_DEST,
        [TB_RHI_STATE_INDIRECT] = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
        [TB_RHI_STATE_PRESENT] = D3D12_RESOURCE_STATE_PRESENT};
    return states[state];
}

void tb_d3d12_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count)
{
    enum
    {
        BATCH = 16
    };
    tb_d3d12_list *const d3d12_list = list;
    D3D12_RESOURCE_BARRIER batch[BATCH] = {0};
    UINT batched = 0;

    TB_ASSERT(count <= BATCH);
    for (size_t index = 0; index < count; index++)
    {
        const tb_rhi_barrier_ref *const barrier = &barriers[index];
        ID3D12Resource *resource = NULL;
        if (barrier->buffer != NULL)
        {
            const tb_d3d12_buffer *const buffer = barrier->buffer;
            /* Upload and readback memory never change state in D3D12. */
            resource = buffer->memory == TB_RHI_MEMORY_GPU ? buffer->resource : NULL;
        }
        else
        {
            resource = ((const tb_d3d12_texture *)barrier->texture.object)->resource;
        }
        if (resource == NULL)
        {
            continue;
        }
        batch[batched] = (D3D12_RESOURCE_BARRIER){0};
        if (barrier->before == barrier->after)
        {
            if (barrier->after != TB_RHI_STATE_STORAGE)
            {
                continue;
            }
            batch[batched].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            batch[batched].UAV.pResource = resource;
        }
        else
        {
            batch[batched].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            batch[batched].Transition.pResource = resource;
            batch[batched].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            batch[batched].Transition.StateBefore = tb_d3d12_state(barrier->before);
            batch[batched].Transition.StateAfter = tb_d3d12_state(barrier->after);
        }
        batched++;
    }
    if (batched > 0)
    {
        ID3D12GraphicsCommandList_ResourceBarrier(d3d12_list->list, batched, batch);
    }
}

/* ---------------------------------------------------------------------------
 * Passes.
 * ------------------------------------------------------------------------- */

static void tb_d3d12_target_view(tb_d3d12_list *list, const tb_rhi_texture_ref *texture,
                                 const tb_rhi_color_target *target,
                                 D3D12_CPU_DESCRIPTOR_HANDLE where)
{
    const tb_d3d12_texture *const d3d12_texture = texture->object;
    D3D12_RENDER_TARGET_VIEW_DESC view = {0};

    view.Format = d3d12_texture->target_format;
    if (texture->desc->samples > 1)
    {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
    }
    else if (texture->desc->layers > 1)
    {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = target->mip;
        view.Texture2DArray.FirstArraySlice = target->layer;
        view.Texture2DArray.ArraySize = 1;
    }
    else
    {
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = target->mip;
    }
    ID3D12Device_CreateRenderTargetView(list->device->device, d3d12_texture->resource, &view,
                                        where);
}

void tb_d3d12_cmd_begin_pass(void *list, const tb_rhi_pass_ref *pass)
{
    tb_d3d12_list *const d3d12_list = list;
    tb_d3d12_device *const device = d3d12_list->device;
    D3D12_CPU_DESCRIPTOR_HANDLE targets[TB_RHI_COLOR_TARGETS] = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE start = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE depth = {0};
    const D3D12_VIEWPORT viewport = {0.0f, 0.0f, (FLOAT)pass->width, (FLOAT)pass->height,
                                     0.0f, 1.0f};
    const D3D12_RECT scissor = {0, 0, (LONG)pass->width, (LONG)pass->height};

    (void)ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d12_list->targets, &start);
    for (uint32_t index = 0; index < pass->color_count; index++)
    {
        targets[index] = tb_d3d12_descriptor(start, device->target_increment, index);
        tb_d3d12_target_view(d3d12_list, &pass->color_textures[index], &pass->colors[index],
                             targets[index]);
        if (pass->colors[index].load == TB_RHI_LOAD_CLEAR)
        {
            ID3D12GraphicsCommandList_ClearRenderTargetView(d3d12_list->list, targets[index],
                                                            pass->colors[index].clear, 0, NULL);
        }
    }
    if (pass->depth_texture.object != NULL)
    {
        const tb_d3d12_texture *const texture = pass->depth_texture.object;
        D3D12_DEPTH_STENCIL_VIEW_DESC view = {0};
        view.Format = texture->target_format;
        view.ViewDimension = pass->depth_texture.desc->samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                                   : D3D12_DSV_DIMENSION_TEXTURE2D;
        view.Flags = pass->depth.read_only ? D3D12_DSV_FLAG_READ_ONLY_DEPTH : D3D12_DSV_FLAG_NONE;
        (void)ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(d3d12_list->depths, &depth);
        ID3D12Device_CreateDepthStencilView(device->device, texture->resource, &view, depth);
        if (pass->depth.load == TB_RHI_LOAD_CLEAR)
        {
            ID3D12GraphicsCommandList_ClearDepthStencilView(
                d3d12_list->list, depth, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                pass->depth.clear, pass->depth.stencil_clear, 0, NULL);
        }
    }
    ID3D12GraphicsCommandList_OMSetRenderTargets(
        d3d12_list->list, pass->color_count, targets, FALSE,
        pass->depth_texture.object != NULL ? &depth : NULL);
    ID3D12GraphicsCommandList_RSSetViewports(d3d12_list->list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(d3d12_list->list, 1, &scissor);
}

void tb_d3d12_cmd_end_pass(void *list)
{
    (void)list;
}

void tb_d3d12_cmd_viewport(void *list, float x, float y, float width, float height,
                           float near_depth, float far_depth)
{
    tb_d3d12_list *const d3d12_list = list;
    const D3D12_VIEWPORT viewport = {x, y, width, height, near_depth, far_depth};

    ID3D12GraphicsCommandList_RSSetViewports(d3d12_list->list, 1, &viewport);
}

void tb_d3d12_cmd_scissor(void *list, tb_rhi_rect rect)
{
    tb_d3d12_list *const d3d12_list = list;
    const D3D12_RECT scissor = {rect.x, rect.y, (LONG)(rect.x + (LONG)rect.width),
                                (LONG)(rect.y + (LONG)rect.height)};

    ID3D12GraphicsCommandList_RSSetScissorRects(d3d12_list->list, 1, &scissor);
}

/* ---------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------- */

void tb_d3d12_cmd_pipeline(void *list, void *pipeline, bool compute)
{
    tb_d3d12_list *const d3d12_list = list;
    const tb_d3d12_pipeline *const d3d12_pipeline = pipeline;

    ID3D12GraphicsCommandList_SetPipelineState(d3d12_list->list, d3d12_pipeline->state);
    if (!compute)
    {
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(d3d12_list->list,
                                                         d3d12_pipeline->topology);
    }
}

/* Push constants and constant buffers reach both kinds of pipeline: the
 * root signature is the same. */
void tb_d3d12_cmd_push_constants(void *list, const void *data, size_t size)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(
        d3d12_list->list, TB_D3D12_ROOT_CONSTANTS, (UINT)(size / 4), data, 0);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(
        d3d12_list->list, TB_D3D12_ROOT_CONSTANTS, (UINT)(size / 4), data, 0);
}

void tb_d3d12_cmd_constant_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset)
{
    tb_d3d12_list *const d3d12_list = list;
    const tb_d3d12_buffer *const d3d12_buffer = buffer;
    const D3D12_GPU_VIRTUAL_ADDRESS address =
        ID3D12Resource_GetGPUVirtualAddress(d3d12_buffer->resource) + offset;

    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
        d3d12_list->list, TB_D3D12_ROOT_CONSTANT_BUFFER + slot, address);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(
        d3d12_list->list, TB_D3D12_ROOT_CONSTANT_BUFFER + slot, address);
}

void tb_d3d12_cmd_vertex_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset,
                                uint64_t size, uint32_t stride)
{
    tb_d3d12_list *const d3d12_list = list;
    const tb_d3d12_buffer *const d3d12_buffer = buffer;
    const D3D12_VERTEX_BUFFER_VIEW view = {
        ID3D12Resource_GetGPUVirtualAddress(d3d12_buffer->resource) + offset, (UINT)size, stride};

    ID3D12GraphicsCommandList_IASetVertexBuffers(d3d12_list->list, slot, 1, &view);
}

void tb_d3d12_cmd_index_buffer(void *list, void *buffer, uint64_t offset, uint64_t size,
                               tb_rhi_format format)
{
    tb_d3d12_list *const d3d12_list = list;
    const tb_d3d12_buffer *const d3d12_buffer = buffer;
    const D3D12_INDEX_BUFFER_VIEW view = {
        ID3D12Resource_GetGPUVirtualAddress(d3d12_buffer->resource) + offset, (UINT)size,
        tb_d3d12_format(format)};

    ID3D12GraphicsCommandList_IASetIndexBuffer(d3d12_list->list, &view);
}

void tb_d3d12_cmd_draw(void *list, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_DrawInstanced(d3d12_list->list, vertices, instances, first_vertex,
                                            first_instance);
}

void tb_d3d12_cmd_draw_indexed(void *list, uint32_t indices, uint32_t instances,
                               uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_DrawIndexedInstanced(d3d12_list->list, indices, instances,
                                                   first_index, vertex_offset, first_instance);
}

void tb_d3d12_cmd_dispatch(void *list, uint32_t x, uint32_t y, uint32_t z)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_Dispatch(d3d12_list->list, x, y, z);
}

/* ---------------------------------------------------------------------------
 * Copies and markers.
 * ------------------------------------------------------------------------- */

void tb_d3d12_cmd_copy_buffer(void *list, void *dest, uint64_t dest_offset, void *source,
                              uint64_t source_offset, uint64_t size)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_CopyBufferRegion(
        d3d12_list->list, ((const tb_d3d12_buffer *)dest)->resource, dest_offset,
        ((const tb_d3d12_buffer *)source)->resource, source_offset, size);
}

/* The two ends of a copy between a buffer and a texture's image. */
static void tb_d3d12_copy_locations(const tb_rhi_texture_copy_ref *copy,
                                    D3D12_TEXTURE_COPY_LOCATION *buffer,
                                    D3D12_TEXTURE_COPY_LOCATION *texture)
{
    const tb_d3d12_texture *const d3d12_texture = copy->texture.object;

    buffer->pResource = ((const tb_d3d12_buffer *)copy->buffer)->resource;
    buffer->Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buffer->PlacedFootprint.Offset = copy->offset;
    buffer->PlacedFootprint.Footprint.Format = tb_d3d12_format(copy->texture.desc->format);
    buffer->PlacedFootprint.Footprint.Width = copy->region.width;
    buffer->PlacedFootprint.Footprint.Height = copy->region.height;
    buffer->PlacedFootprint.Footprint.Depth = 1;
    buffer->PlacedFootprint.Footprint.RowPitch = copy->row_pitch;
    texture->pResource = d3d12_texture->resource;
    texture->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture->SubresourceIndex = copy->mip + copy->layer * copy->texture.desc->mips;
}

void tb_d3d12_cmd_copy_buffer_to_texture(void *list, const tb_rhi_texture_copy_ref *copy)
{
    tb_d3d12_list *const d3d12_list = list;
    D3D12_TEXTURE_COPY_LOCATION buffer = {0};
    D3D12_TEXTURE_COPY_LOCATION texture = {0};

    tb_d3d12_copy_locations(copy, &buffer, &texture);
    ID3D12GraphicsCommandList_CopyTextureRegion(d3d12_list->list, &texture, (UINT)copy->region.x,
                                                (UINT)copy->region.y, 0, &buffer, NULL);
}

void tb_d3d12_cmd_copy_texture_to_buffer(void *list, const tb_rhi_texture_copy_ref *copy)
{
    tb_d3d12_list *const d3d12_list = list;
    D3D12_TEXTURE_COPY_LOCATION buffer = {0};
    D3D12_TEXTURE_COPY_LOCATION texture = {0};
    const D3D12_BOX box = {(UINT)copy->region.x,
                           (UINT)copy->region.y,
                           0,
                           (UINT)copy->region.x + copy->region.width,
                           (UINT)copy->region.y + copy->region.height,
                           1};

    tb_d3d12_copy_locations(copy, &buffer, &texture);
    ID3D12GraphicsCommandList_CopyTextureRegion(d3d12_list->list, &buffer, 0, 0, 0, &texture, &box);
}

void tb_d3d12_cmd_begin_marker(void *list, const char *name)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_BeginEvent(d3d12_list->list, TB_D3D12_PIX_ANSI, name,
                                         (UINT)(tb_str_length(name) + 1));
}

void tb_d3d12_cmd_end_marker(void *list)
{
    tb_d3d12_list *const d3d12_list = list;

    ID3D12GraphicsCommandList_EndEvent(d3d12_list->list);
}
