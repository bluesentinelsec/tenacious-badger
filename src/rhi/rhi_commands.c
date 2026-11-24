/* Command lists of the RHI: their handles looked up and their contracts
 * checked before each command reaches the backend; and submissions. */
#include "rhi_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

tb_rhi_command_list *tb_rhi_command_list_begin(tb_rhi_device *device)
{
    tb_rhi_command_list *list = NULL;

    TB_ASSERT(device != NULL);
    list = tb_calloc(1, sizeof(*list));
    list->device = device;
    list->backend = device->table->list_begin(device->state);
    tb_mutex_lock(&device->lock);
    device->open_lists++;
    tb_mutex_unlock(&device->lock);
    return list;
}

static void tb_rhi_list_close(tb_rhi_command_list *list)
{
    tb_rhi_device *const device = list->device;

    tb_mutex_lock(&device->lock);
    device->open_lists--;
    tb_mutex_unlock(&device->lock);
    tb_free(list);
}

void tb_rhi_command_list_discard(tb_rhi_command_list *list)
{
    TB_ASSERT(list != NULL);
    list->device->table->list_discard(list->device->state, list->backend);
    tb_rhi_list_close(list);
}

uint64_t tb_rhi_submit(tb_rhi_device *device, tb_rhi_command_list *const *lists, size_t count)
{
    void **backends = NULL;
    uint64_t submission = 0;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && (lists != NULL || count == 0));
    backends = tb_calloc(count == 0 ? 1 : count, sizeof(*backends));
    for (size_t index = 0; index < count; index++)
    {
        TB_ASSERT_MSG(lists[index]->device == device, "a command list of another device");
        TB_ASSERT_MSG(!lists[index]->in_pass, "a command list submitted inside a pass");
        TB_ASSERT_MSG(lists[index]->markers == 0, "a command list submitted inside a marker");
        backends[index] = lists[index]->backend;
    }
    tb_mutex_lock(&device->submit_lock);
    tb_mutex_lock(&device->lock);
    device->submitted++;
    submission = device->submitted;
    tb_mutex_unlock(&device->lock);
    result = device->table->submit(device->state, (void *const *)backends, count, submission);
    tb_mutex_unlock(&device->submit_lock);
    tb_free(backends);
    for (size_t index = 0; index < count; index++)
    {
        tb_rhi_list_close(lists[index]);
    }
    tb_mutex_lock(&device->lock);
    device->lost = device->lost || result == TB_RHI_DEVICE_LOST;
    tb_rhi_collect(device, device->table->completed(device->state));
    tb_mutex_unlock(&device->lock);
    return submission;
}

uint64_t tb_rhi_completed(tb_rhi_device *device)
{
    uint64_t completed = 0;

    TB_ASSERT(device != NULL);
    tb_mutex_lock(&device->lock);
    completed = device->table->completed(device->state);
    tb_rhi_collect(device, completed);
    tb_mutex_unlock(&device->lock);
    return completed;
}

tb_rhi_result tb_rhi_wait(tb_rhi_device *device, uint64_t submission)
{
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL);
    tb_mutex_lock(&device->lock);
    TB_ASSERT_MSG(submission <= device->submitted, "waiting for a submission not made");
    result = device->lost ? TB_RHI_DEVICE_LOST : TB_RHI_OK;
    tb_mutex_unlock(&device->lock);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    result = device->table->wait(device->state, submission);
    tb_mutex_lock(&device->lock);
    device->lost = device->lost || result == TB_RHI_DEVICE_LOST;
    tb_rhi_collect(device, device->table->completed(device->state));
    tb_mutex_unlock(&device->lock);
    return result;
}

tb_rhi_result tb_rhi_wait_idle(tb_rhi_device *device)
{
    uint64_t submitted = 0;

    TB_ASSERT(device != NULL);
    tb_mutex_lock(&device->lock);
    submitted = device->submitted;
    tb_mutex_unlock(&device->lock);
    return tb_rhi_wait(device, submitted);
}

/* ---------------------------------------------------------------------------
 * Recording.
 * ------------------------------------------------------------------------- */

static tb_rhi_texture_ref tb_rhi_texture_ref_of(tb_rhi_device *device, tb_rhi_texture texture)
{
    const tb_rhi_slot *const slot =
        tb_rhi_slot_of(device, TB_RHI_KIND_TEXTURE, texture.index, texture.generation);
    return (tb_rhi_texture_ref){slot->object, &slot->desc.texture};
}

static void *tb_rhi_buffer_object(tb_rhi_device *device, tb_rhi_buffer buffer, uint64_t *size)
{
    const tb_rhi_slot *const slot =
        tb_rhi_slot_of(device, TB_RHI_KIND_BUFFER, buffer.index, buffer.generation);
    if (size != NULL)
    {
        *size = slot->desc.buffer.size;
    }
    return slot->object;
}

void tb_rhi_cmd_barriers(tb_rhi_command_list *list, const tb_rhi_barrier *barriers, size_t count)
{
    enum
    {
        BATCH = 16
    };
    tb_rhi_barrier_ref refs[BATCH] = {0};
    size_t batched = 0;

    TB_ASSERT(list != NULL && (barriers != NULL || count == 0));
    TB_ASSERT_MSG(!list->in_pass, "a barrier inside a pass");
    for (size_t index = 0; index < count; index++)
    {
        const tb_rhi_barrier *const barrier = &barriers[index];
        TB_ASSERT_MSG((barrier->buffer.generation != 0) != (barrier->texture.generation != 0),
                      "a barrier names one buffer or one texture");
        refs[batched] = (tb_rhi_barrier_ref){.before = barrier->before, .after = barrier->after};
        if (barrier->buffer.generation != 0)
        {
            refs[batched].buffer = tb_rhi_buffer_object(list->device, barrier->buffer, NULL);
        }
        else
        {
            refs[batched].texture = tb_rhi_texture_ref_of(list->device, barrier->texture);
        }
        batched++;
        if (batched == BATCH || index + 1 == count)
        {
            list->device->table->cmd_barriers(list->backend, refs, batched);
            batched = 0;
        }
    }
}

void tb_rhi_cmd_barrier(tb_rhi_command_list *list, const tb_rhi_barrier *barrier)
{
    tb_rhi_cmd_barriers(list, barrier, 1);
}

/* The size of a target's level. */
static void tb_rhi_level_size(const tb_rhi_texture_desc *desc, uint32_t mip, uint32_t *width,
                              uint32_t *height)
{
    TB_ASSERT_MSG(mip < desc->mips, "a level the texture lacks");
    *width = desc->width >> mip == 0 ? 1u : desc->width >> mip;
    *height = desc->height >> mip == 0 ? 1u : desc->height >> mip;
}

void tb_rhi_cmd_begin_pass(tb_rhi_command_list *list, const tb_rhi_pass *pass)
{
    tb_rhi_pass_ref ref = {0};
    bool sized = false;

    TB_ASSERT(list != NULL && pass != NULL);
    TB_ASSERT_MSG(!list->in_pass, "a pass inside a pass");
    TB_ASSERT_MSG(pass->color_count <= TB_RHI_COLOR_TARGETS, "too many color targets");
    TB_ASSERT_MSG(pass->color_count > 0 || pass->depth.texture.generation != 0,
                  "a pass of no targets");
    ref.color_count = pass->color_count;
    for (uint32_t index = 0; index < pass->color_count; index++)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        ref.colors[index] = pass->colors[index];
        ref.color_textures[index] =
            tb_rhi_texture_ref_of(list->device, pass->colors[index].texture);
        TB_ASSERT_MSG((ref.color_textures[index].desc->usage & TB_RHI_TEXTURE_RENDER_TARGET) != 0,
                      "a color target not made to be one");
        TB_ASSERT_MSG(pass->colors[index].layer < ref.color_textures[index].desc->layers,
                      "an image the texture lacks");
        tb_rhi_level_size(ref.color_textures[index].desc, pass->colors[index].mip, &width, &height);
        TB_ASSERT_MSG(!sized || (width == ref.width && height == ref.height),
                      "targets of a pass differ in size");
        ref.width = width;
        ref.height = height;
        sized = true;
    }
    ref.depth = pass->depth;
    if (pass->depth.texture.generation != 0)
    {
        ref.depth_texture = tb_rhi_texture_ref_of(list->device, pass->depth.texture);
        TB_ASSERT_MSG((ref.depth_texture.desc->usage & TB_RHI_TEXTURE_DEPTH) != 0,
                      "a depth target not made to be one");
        TB_ASSERT_MSG(!sized || (ref.depth_texture.desc->width == ref.width &&
                                 ref.depth_texture.desc->height == ref.height),
                      "targets of a pass differ in size");
        ref.width = ref.depth_texture.desc->width;
        ref.height = ref.depth_texture.desc->height;
    }
    list->in_pass = true;
    list->device->table->cmd_begin_pass(list->backend, &ref);
}

void tb_rhi_cmd_end_pass(tb_rhi_command_list *list)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(list->in_pass, "ending a pass not begun");
    list->in_pass = false;
    list->device->table->cmd_end_pass(list->backend);
}

void tb_rhi_cmd_viewport(tb_rhi_command_list *list, float x, float y, float width, float height,
                         float near_depth, float far_depth)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(list->in_pass, "a viewport outside a pass");
    list->device->table->cmd_viewport(list->backend, x, y, width, height, near_depth, far_depth);
}

void tb_rhi_cmd_scissor(tb_rhi_command_list *list, tb_rhi_rect rect)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(list->in_pass, "a scissor outside a pass");
    list->device->table->cmd_scissor(list->backend, rect);
}

void tb_rhi_cmd_pipeline(tb_rhi_command_list *list, tb_rhi_pipeline pipeline)
{
    const tb_rhi_slot *slot = NULL;

    TB_ASSERT(list != NULL);
    slot = tb_rhi_slot_of(list->device, TB_RHI_KIND_PIPELINE, pipeline.index, pipeline.generation);
    list->compute = slot->compute;
    list->has_pipeline = true;
    for (uint32_t index = 0; index < TB_RHI_VERTEX_BUFFERS; index++)
    {
        if (list->layouts[index].stride != slot->layouts[index].stride &&
            list->vertex[index].object != NULL)
        {
            list->vertex_dirty |= 1u << index;
        }
        list->layouts[index] = slot->layouts[index];
    }
    list->device->table->cmd_pipeline(list->backend, slot->object, slot->compute);
}

void tb_rhi_cmd_push_constants(tb_rhi_command_list *list, const void *data, size_t size)
{
    TB_ASSERT(list != NULL && data != NULL);
    TB_ASSERT_MSG(size % 4 == 0 && size <= TB_RHI_PUSH_CONSTANT_BYTES,
                  "push constants are whole words, at most TB_RHI_PUSH_CONSTANT_BYTES");
    list->device->table->cmd_push_constants(list->backend, data, size);
}

void tb_rhi_cmd_constant_buffer(tb_rhi_command_list *list, uint32_t slot, tb_rhi_buffer buffer,
                                uint64_t offset)
{
    const tb_rhi_slot *found = NULL;

    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(slot < TB_RHI_CONSTANT_BUFFER_SLOTS, "a constant buffer slot out of range");
    TB_ASSERT_MSG(offset % TB_RHI_CONSTANT_ALIGNMENT == 0, "a constant buffer's offset misaligned");
    found = tb_rhi_slot_of(list->device, TB_RHI_KIND_BUFFER, buffer.index, buffer.generation);
    TB_ASSERT_MSG((found->desc.buffer.usage & TB_RHI_BUFFER_CONSTANT) != 0,
                  "a buffer not made for constants");
    TB_ASSERT_MSG(offset < found->desc.buffer.size, "a constant buffer's offset past its end");
    list->device->table->cmd_constant_buffer(list->backend, slot, found->object, offset);
}

void tb_rhi_cmd_vertex_buffer(tb_rhi_command_list *list, uint32_t slot, tb_rhi_buffer buffer,
                              uint64_t offset)
{
    const tb_rhi_slot *found = NULL;

    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(slot < TB_RHI_VERTEX_BUFFERS, "a vertex buffer slot out of range");
    found = tb_rhi_slot_of(list->device, TB_RHI_KIND_BUFFER, buffer.index, buffer.generation);
    TB_ASSERT_MSG((found->desc.buffer.usage & TB_RHI_BUFFER_VERTEX) != 0,
                  "a buffer not made for vertices");
    TB_ASSERT_MSG(offset <= found->desc.buffer.size, "a vertex buffer's offset past its end");
    list->vertex[slot].object = found->object;
    list->vertex[slot].offset = offset;
    list->vertex[slot].size = found->desc.buffer.size - offset;
    list->vertex_dirty |= 1u << slot;
}

void tb_rhi_cmd_index_buffer(tb_rhi_command_list *list, tb_rhi_buffer buffer, uint64_t offset,
                             tb_rhi_format format)
{
    const tb_rhi_slot *found = NULL;

    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(format == TB_RHI_FORMAT_R16_UINT || format == TB_RHI_FORMAT_R32_UINT,
                  "indices of 16 or 32 bits");
    found = tb_rhi_slot_of(list->device, TB_RHI_KIND_BUFFER, buffer.index, buffer.generation);
    TB_ASSERT_MSG((found->desc.buffer.usage & TB_RHI_BUFFER_INDEX) != 0,
                  "a buffer not made for indices");
    TB_ASSERT_MSG(offset <= found->desc.buffer.size, "an index buffer's offset past its end");
    list->device->table->cmd_index_buffer(list->backend, found->object, offset,
                                          found->desc.buffer.size - offset, format);
}

/* Give the backend the vertex buffers bound since the last draw, with the
 * pipeline's strides. */
static void tb_rhi_ready_to_draw(tb_rhi_command_list *list)
{
    TB_ASSERT_MSG(list->in_pass, "a draw outside a pass");
    TB_ASSERT_MSG(list->has_pipeline && !list->compute, "a draw without a graphics pipeline");
    for (uint32_t index = 0; index < TB_RHI_VERTEX_BUFFERS; index++)
    {
        if ((list->vertex_dirty & (1u << index)) != 0)
        {
            list->device->table->cmd_vertex_buffer(
                list->backend, index, list->vertex[index].object, list->vertex[index].offset,
                list->vertex[index].size, list->layouts[index].stride);
        }
    }
    list->vertex_dirty = 0;
}

void tb_rhi_cmd_draw(tb_rhi_command_list *list, uint32_t vertices, uint32_t instances,
                     uint32_t first_vertex, uint32_t first_instance)
{
    TB_ASSERT(list != NULL);
    tb_rhi_ready_to_draw(list);
    list->device->table->cmd_draw(list->backend, vertices, instances, first_vertex, first_instance);
}

void tb_rhi_cmd_draw_indexed(tb_rhi_command_list *list, uint32_t indices, uint32_t instances,
                             uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    TB_ASSERT(list != NULL);
    tb_rhi_ready_to_draw(list);
    list->device->table->cmd_draw_indexed(list->backend, indices, instances, first_index,
                                          vertex_offset, first_instance);
}

void tb_rhi_cmd_dispatch(tb_rhi_command_list *list, uint32_t x, uint32_t y, uint32_t z)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(!list->in_pass, "a dispatch inside a pass");
    TB_ASSERT_MSG(list->has_pipeline && list->compute, "a dispatch without a compute pipeline");
    list->device->table->cmd_dispatch(list->backend, x, y, z);
}

void tb_rhi_cmd_copy_buffer(tb_rhi_command_list *list, tb_rhi_buffer dest, uint64_t dest_offset,
                            tb_rhi_buffer source, uint64_t source_offset, uint64_t size)
{
    uint64_t dest_size = 0;
    uint64_t source_size = 0;
    void *dest_object = NULL;
    void *source_object = NULL;

    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(!list->in_pass, "a copy inside a pass");
    dest_object = tb_rhi_buffer_object(list->device, dest, &dest_size);
    source_object = tb_rhi_buffer_object(list->device, source, &source_size);
    TB_ASSERT_MSG(dest_offset <= dest_size && size <= dest_size - dest_offset,
                  "a copy past the destination's end");
    TB_ASSERT_MSG(source_offset <= source_size && size <= source_size - source_offset,
                  "a copy past the source's end");
    list->device->table->cmd_copy_buffer(list->backend, dest_object, dest_offset, source_object,
                                         source_offset, size);
}

/* A copy between a buffer and a texture, looked up and checked. */
static tb_rhi_texture_copy_ref tb_rhi_copy_ref_of(tb_rhi_command_list *list,
                                                  const tb_rhi_texture_copy *copy)
{
    tb_rhi_texture_copy_ref ref = {0};
    uint64_t buffer_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t texel = 0;

    TB_ASSERT(copy != NULL);
    TB_ASSERT_MSG(!list->in_pass, "a copy inside a pass");
    TB_ASSERT_MSG(copy->offset % TB_RHI_TEXTURE_COPY_OFFSET_ALIGNMENT == 0,
                  "a texture copy's offset misaligned");
    TB_ASSERT_MSG(copy->row_pitch % TB_RHI_TEXTURE_COPY_PITCH_ALIGNMENT == 0,
                  "a texture copy's row pitch misaligned");
    ref.buffer = tb_rhi_buffer_object(list->device, copy->buffer, &buffer_size);
    ref.texture = tb_rhi_texture_ref_of(list->device, copy->texture);
    ref.offset = copy->offset;
    ref.row_pitch = copy->row_pitch;
    ref.mip = copy->mip;
    ref.layer = copy->layer;
    ref.region = copy->region;
    TB_ASSERT_MSG(copy->layer < ref.texture.desc->layers, "an image the texture lacks");
    tb_rhi_level_size(ref.texture.desc, copy->mip, &width, &height);
    if (ref.region.width == 0 || ref.region.height == 0)
    {
        ref.region = (tb_rhi_rect){0, 0, width, height};
    }
    TB_ASSERT_MSG(ref.region.x >= 0 && ref.region.y >= 0 &&
                      (uint64_t)ref.region.x + ref.region.width <= width &&
                      (uint64_t)ref.region.y + ref.region.height <= height,
                  "a copy past the texture's edge");
    texel = tb_rhi_format_size(ref.texture.desc->format);
    TB_ASSERT_MSG((uint64_t)ref.region.width * texel <= copy->row_pitch,
                  "a row pitch shorter than a row");
    TB_ASSERT_MSG(copy->offset <= buffer_size &&
                      (uint64_t)copy->row_pitch * (ref.region.height - 1) +
                              (uint64_t)ref.region.width * texel <=
                          buffer_size - copy->offset,
                  "a copy past the buffer's end");
    return ref;
}

void tb_rhi_cmd_copy_buffer_to_texture(tb_rhi_command_list *list, const tb_rhi_texture_copy *copy)
{
    tb_rhi_texture_copy_ref ref = {0};

    TB_ASSERT(list != NULL);
    ref = tb_rhi_copy_ref_of(list, copy);
    list->device->table->cmd_copy_buffer_to_texture(list->backend, &ref);
}

void tb_rhi_cmd_copy_texture_to_buffer(tb_rhi_command_list *list, const tb_rhi_texture_copy *copy)
{
    tb_rhi_texture_copy_ref ref = {0};

    TB_ASSERT(list != NULL);
    ref = tb_rhi_copy_ref_of(list, copy);
    list->device->table->cmd_copy_texture_to_buffer(list->backend, &ref);
}

void tb_rhi_cmd_begin_marker(tb_rhi_command_list *list, const char *name)
{
    TB_ASSERT(list != NULL && name != NULL);
    list->markers++;
    list->device->table->cmd_begin_marker(list->backend, name);
}

void tb_rhi_cmd_end_marker(tb_rhi_command_list *list)
{
    TB_ASSERT(list != NULL);
    TB_ASSERT_MSG(list->markers > 0, "ending a marker not begun");
    list->markers--;
    list->device->table->cmd_end_marker(list->backend);
}
