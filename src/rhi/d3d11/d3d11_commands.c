/* The D3D11 backend's command lists: commands recorded into memory on any
 * thread, then replayed on the immediate context when they are submitted.
 * Replaying is where the RHI's model meets D3D11's: before each draw or
 * dispatch, the constants a shader reads are copied into the backend's
 * constant buffers, and each resource it takes from a descriptor heap is
 * bound to the register the shader compiler gave it, found by the index it
 * reads from those constants. */
#include "d3d11_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* Every record starts at a multiple of this. */
#define TB_D3D11_RECORD_ALIGNMENT 8u
/* Bytes of a marker's name a list keeps. */
#define TB_D3D11_MARKER_BYTES 64u

typedef enum tb_d3d11_op
{
    TB_D3D11_OP_BEGIN_PASS,
    TB_D3D11_OP_END_PASS,
    TB_D3D11_OP_VIEWPORT,
    TB_D3D11_OP_SCISSOR,
    TB_D3D11_OP_PIPELINE,
    TB_D3D11_OP_PUSH_CONSTANTS,
    TB_D3D11_OP_CONSTANT_BUFFER,
    TB_D3D11_OP_VERTEX_BUFFER,
    TB_D3D11_OP_INDEX_BUFFER,
    TB_D3D11_OP_DRAW,
    TB_D3D11_OP_DRAW_INDEXED,
    TB_D3D11_OP_DISPATCH,
    TB_D3D11_OP_COPY_BUFFER,
    TB_D3D11_OP_COPY_BUFFER_TO_TEXTURE,
    TB_D3D11_OP_COPY_TEXTURE_TO_BUFFER,
    TB_D3D11_OP_BEGIN_MARKER,
    TB_D3D11_OP_END_MARKER
} tb_d3d11_op;

typedef struct tb_d3d11_record
{
    uint32_t op;
    uint32_t size; /* of the record, its header included, at the alignment */
} tb_d3d11_record;

typedef struct tb_d3d11_pass
{
    tb_d3d11_texture *colors[TB_RHI_COLOR_TARGETS];
    uint32_t mips[TB_RHI_COLOR_TARGETS];
    uint32_t layers[TB_RHI_COLOR_TARGETS];
    bool clear[TB_RHI_COLOR_TARGETS];
    float clears[TB_RHI_COLOR_TARGETS][4];
    uint32_t color_count;
    tb_d3d11_texture *depth;
    bool depth_clear;
    float depth_value;
    uint8_t stencil_value;
    bool read_only;
    uint32_t width;
    uint32_t height;
} tb_d3d11_pass;

typedef struct tb_d3d11_viewport
{
    float values[6];
} tb_d3d11_viewport;

typedef struct tb_d3d11_bind
{
    void *object;
    uint64_t offset;
    uint64_t size;
    uint32_t slot;
    uint32_t stride;
    bool compute;
    tb_rhi_format format;
} tb_d3d11_bind;

typedef struct tb_d3d11_constants
{
    uint32_t size;
    uint8_t bytes[TB_RHI_PUSH_CONSTANT_BYTES];
} tb_d3d11_constants;

typedef struct tb_d3d11_draw
{
    uint32_t counts[4];
    int32_t vertex_offset;
} tb_d3d11_draw;

typedef struct tb_d3d11_buffer_copy
{
    tb_d3d11_buffer *dest;
    uint64_t dest_offset;
    tb_d3d11_buffer *source;
    uint64_t source_offset;
    uint64_t size;
} tb_d3d11_buffer_copy;

typedef struct tb_d3d11_texture_copy
{
    tb_d3d11_buffer *buffer;
    uint64_t offset;
    uint32_t row_pitch;
    tb_d3d11_texture *texture;
    uint32_t mip;
    uint32_t layer;
    tb_rhi_rect region;
} tb_d3d11_texture_copy;

typedef struct tb_d3d11_marker
{
    char name[TB_D3D11_MARKER_BYTES];
} tb_d3d11_marker;

/* ---------------------------------------------------------------------------
 * Lists, and recording.
 * ------------------------------------------------------------------------- */

void *tb_d3d11_list_begin(void *state)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_list *list = NULL;

    tb_mutex_lock(&device->lists_lock);
    if (device->idle_count > 0)
    {
        device->idle_count--;
        list = device->idle[device->idle_count];
    }
    tb_mutex_unlock(&device->lists_lock);
    if (list == NULL)
    {
        list = tb_calloc(1, sizeof(*list));
        list->device = device;
    }
    list->length = 0;
    return list;
}

static void tb_d3d11_list_give(tb_d3d11_device *device, tb_d3d11_list *list)
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

void tb_d3d11_list_discard(void *state, void *list)
{
    tb_d3d11_list_give(state, list);
}

void tb_d3d11_release_lists(tb_d3d11_device *device)
{
    for (size_t index = 0; index < device->idle_count; index++)
    {
        tb_free(device->idle[index]->bytes);
        tb_free(device->idle[index]);
    }
    tb_free(device->idle);
    device->idle = NULL;
    device->idle_count = 0;
    device->idle_capacity = 0;
}

/* Room for a record of a payload of `size` bytes at the end of a list, zeroed. */
static void *tb_d3d11_append(void *list, tb_d3d11_op op, size_t size)
{
    tb_d3d11_list *const d3d11_list = list;
    const size_t whole = (sizeof(tb_d3d11_record) + size + TB_D3D11_RECORD_ALIGNMENT - 1) /
                         TB_D3D11_RECORD_ALIGNMENT * TB_D3D11_RECORD_ALIGNMENT;
    tb_d3d11_record *record = NULL;

    if (d3d11_list->length + whole > d3d11_list->capacity)
    {
        size_t capacity = d3d11_list->capacity == 0 ? 4096 : d3d11_list->capacity;
        while (capacity < d3d11_list->length + whole)
        {
            capacity *= 2;
        }
        d3d11_list->bytes = tb_realloc(d3d11_list->bytes, capacity);
        d3d11_list->capacity = capacity;
    }
    record = (tb_d3d11_record *)(void *)(d3d11_list->bytes + d3d11_list->length);
    memset(record, 0, whole);
    record->op = (uint32_t)op;
    record->size = (uint32_t)whole;
    d3d11_list->length += whole;
    return record + 1;
}

void tb_d3d11_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count)
{
    /* D3D11 tracks every resource's state, and orders what reads after
     * what writes, itself. */
    (void)list;
    (void)barriers;
    (void)count;
}

void tb_d3d11_cmd_begin_pass(void *list, const tb_rhi_pass_ref *pass)
{
    tb_d3d11_pass *const made = tb_d3d11_append(list, TB_D3D11_OP_BEGIN_PASS, sizeof(*made));

    made->color_count = pass->color_count;
    for (uint32_t index = 0; index < pass->color_count; index++)
    {
        made->colors[index] = pass->color_textures[index].object;
        made->mips[index] = pass->colors[index].mip;
        made->layers[index] = pass->colors[index].layer;
        made->clear[index] = pass->colors[index].load == TB_RHI_LOAD_CLEAR;
        memcpy(made->clears[index], pass->colors[index].clear, sizeof(made->clears[index]));
    }
    made->depth = pass->depth_texture.object;
    made->depth_clear = pass->depth.load == TB_RHI_LOAD_CLEAR;
    made->depth_value = pass->depth.clear;
    made->stencil_value = pass->depth.stencil_clear;
    made->read_only = pass->depth.read_only;
    made->width = pass->width;
    made->height = pass->height;
}

void tb_d3d11_cmd_end_pass(void *list)
{
    (void)tb_d3d11_append(list, TB_D3D11_OP_END_PASS, 0);
}

void tb_d3d11_cmd_viewport(void *list, float x, float y, float width, float height,
                           float near_depth, float far_depth)
{
    tb_d3d11_viewport *const made = tb_d3d11_append(list, TB_D3D11_OP_VIEWPORT, sizeof(*made));
    const float values[6] = {x, y, width, height, near_depth, far_depth};

    memcpy(made->values, values, sizeof(values));
}

void tb_d3d11_cmd_scissor(void *list, tb_rhi_rect rect)
{
    tb_rhi_rect *const made = tb_d3d11_append(list, TB_D3D11_OP_SCISSOR, sizeof(*made));

    *made = rect;
}

void tb_d3d11_cmd_pipeline(void *list, void *pipeline, bool compute)
{
    tb_d3d11_bind *const made = tb_d3d11_append(list, TB_D3D11_OP_PIPELINE, sizeof(*made));

    made->object = pipeline;
    made->compute = compute;
}

void tb_d3d11_cmd_push_constants(void *list, const void *data, size_t size)
{
    tb_d3d11_constants *const made =
        tb_d3d11_append(list, TB_D3D11_OP_PUSH_CONSTANTS, sizeof(*made));

    TB_ASSERT(size <= TB_RHI_PUSH_CONSTANT_BYTES);
    made->size = (uint32_t)size;
    memcpy(made->bytes, data, size);
}

void tb_d3d11_cmd_constant_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset)
{
    tb_d3d11_bind *const made = tb_d3d11_append(list, TB_D3D11_OP_CONSTANT_BUFFER, sizeof(*made));

    made->object = buffer;
    made->offset = offset;
    made->slot = slot;
}

void tb_d3d11_cmd_vertex_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset,
                                uint64_t size, uint32_t stride)
{
    tb_d3d11_bind *const made = tb_d3d11_append(list, TB_D3D11_OP_VERTEX_BUFFER, sizeof(*made));

    made->object = buffer;
    made->offset = offset;
    made->size = size;
    made->slot = slot;
    made->stride = stride;
}

void tb_d3d11_cmd_index_buffer(void *list, void *buffer, uint64_t offset, uint64_t size,
                               tb_rhi_format format)
{
    tb_d3d11_bind *const made = tb_d3d11_append(list, TB_D3D11_OP_INDEX_BUFFER, sizeof(*made));

    made->object = buffer;
    made->offset = offset;
    made->size = size;
    made->format = format;
}

void tb_d3d11_cmd_draw(void *list, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance)
{
    tb_d3d11_draw *const made = tb_d3d11_append(list, TB_D3D11_OP_DRAW, sizeof(*made));
    const uint32_t counts[4] = {vertices, instances, first_vertex, first_instance};

    memcpy(made->counts, counts, sizeof(counts));
}

void tb_d3d11_cmd_draw_indexed(void *list, uint32_t indices, uint32_t instances,
                               uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
    tb_d3d11_draw *const made = tb_d3d11_append(list, TB_D3D11_OP_DRAW_INDEXED, sizeof(*made));
    const uint32_t counts[4] = {indices, instances, first_index, first_instance};

    memcpy(made->counts, counts, sizeof(counts));
    made->vertex_offset = vertex_offset;
}

void tb_d3d11_cmd_dispatch(void *list, uint32_t x, uint32_t y, uint32_t z)
{
    tb_d3d11_draw *const made = tb_d3d11_append(list, TB_D3D11_OP_DISPATCH, sizeof(*made));
    const uint32_t counts[4] = {x, y, z, 0};

    memcpy(made->counts, counts, sizeof(counts));
}

void tb_d3d11_cmd_copy_buffer(void *list, void *dest, uint64_t dest_offset, void *source,
                              uint64_t source_offset, uint64_t size)
{
    tb_d3d11_buffer_copy *const made =
        tb_d3d11_append(list, TB_D3D11_OP_COPY_BUFFER, sizeof(*made));

    made->dest = dest;
    made->dest_offset = dest_offset;
    made->source = source;
    made->source_offset = source_offset;
    made->size = size;
}

static void tb_d3d11_record_texture_copy(void *list, tb_d3d11_op op,
                                         const tb_rhi_texture_copy_ref *copy)
{
    tb_d3d11_texture_copy *const made = tb_d3d11_append(list, op, sizeof(*made));

    made->buffer = copy->buffer;
    made->offset = copy->offset;
    made->row_pitch = copy->row_pitch;
    made->texture = copy->texture.object;
    made->mip = copy->mip;
    made->layer = copy->layer;
    made->region = copy->region;
}

void tb_d3d11_cmd_copy_buffer_to_texture(void *list, const tb_rhi_texture_copy_ref *copy)
{
    tb_d3d11_record_texture_copy(list, TB_D3D11_OP_COPY_BUFFER_TO_TEXTURE, copy);
}

void tb_d3d11_cmd_copy_texture_to_buffer(void *list, const tb_rhi_texture_copy_ref *copy)
{
    tb_d3d11_record_texture_copy(list, TB_D3D11_OP_COPY_TEXTURE_TO_BUFFER, copy);
}

void tb_d3d11_cmd_begin_marker(void *list, const char *name)
{
    tb_d3d11_marker *const made = tb_d3d11_append(list, TB_D3D11_OP_BEGIN_MARKER, sizeof(*made));
    const size_t length = tb_str_length(name);
    const size_t kept = length < TB_D3D11_MARKER_BYTES - 1 ? length : TB_D3D11_MARKER_BYTES - 1;

    memcpy(made->name, name, kept);
}

void tb_d3d11_cmd_end_marker(void *list)
{
    (void)tb_d3d11_append(list, TB_D3D11_OP_END_MARKER, 0);
}

/* ---------------------------------------------------------------------------
 * Replaying: what is bound, and binding before each draw.
 * ------------------------------------------------------------------------- */

/* The stages that bind views: the vertex, pixel and compute shaders. */
enum
{
    TB_D3D11_VERTEX,
    TB_D3D11_PIXEL,
    TB_D3D11_COMPUTE,
    TB_D3D11_STAGES
};

typedef struct tb_d3d11_replay
{
    tb_d3d11_device *device;
    ID3D11DeviceContext *context;
    uint64_t submission;
    const tb_d3d11_pipeline *pipeline;
    uint8_t push[TB_RHI_PUSH_CONSTANT_BYTES];
    uint32_t push_size;
    bool push_fresh; /* the push constants changed since b0 was filled */
    const tb_d3d11_buffer *buffers[TB_RHI_CONSTANT_BUFFER_SLOTS];
    uint64_t offsets[TB_RHI_CONSTANT_BUFFER_SLOTS];
    /* What b1 and b2 were last filled from, by buffer, offset and bytes. */
    const tb_d3d11_buffer *filled_buffers[TB_RHI_CONSTANT_BUFFER_SLOTS];
    uint64_t filled_offsets[TB_RHI_CONSTANT_BUFFER_SLOTS];
    uint32_t filled_sizes[TB_D3D11_CONSTANT_BUFFERS];
    /* Registers bound so far of each stage, which the next binding clears. */
    UINT reads[TB_D3D11_STAGES];
    UINT samplers[TB_D3D11_STAGES];
    UINT writes;
} tb_d3d11_replay;

/* The heap index a slot reads: false when the constants do not hold it. */
static bool tb_d3d11_read_index(const tb_d3d11_replay *replay, const tb_shader_dxbc_slot *slot,
                                uint32_t *index)
{
    const uint8_t *bytes = NULL;
    uint64_t available = 0;
    uint64_t at = slot->offset;

    if (slot->source == 0)
    {
        bytes = replay->push;
        available = replay->push_size;
    }
    else
    {
        const tb_d3d11_buffer *const buffer = replay->buffers[slot->source - 1];
        if (buffer == NULL || buffer->cpu == NULL)
        {
            return false;
        }
        bytes = buffer->cpu;
        available = buffer->size;
        at += replay->offsets[slot->source - 1];
    }
    if (at + sizeof(*index) > available)
    {
        return false;
    }
    memcpy(index, bytes + at, sizeof(*index));
    return true;
}

/* Fill the backend's constant buffer of a register with what the shaders
 * of the pipeline read of it. */
static void tb_d3d11_fill_constants(tb_d3d11_replay *replay, uint32_t slot)
{
    tb_d3d11_device *const device = replay->device;
    const tb_d3d11_pipeline *const pipeline = replay->pipeline;
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    const uint8_t *source = NULL;
    uint64_t available = 0;
    uint32_t bytes = 0;

    for (uint32_t stage = 0; stage < pipeline->stage_count; stage++)
    {
        const uint32_t read = pipeline->stages[stage].constant_bytes[slot];
        bytes = read > bytes ? read : bytes;
    }
    if (bytes == 0)
    {
        return;
    }
    bytes = bytes < TB_D3D11_CONSTANT_BUFFER_BYTES ? bytes : TB_D3D11_CONSTANT_BUFFER_BYTES;
    if (slot == 0)
    {
        if (!replay->push_fresh && replay->filled_sizes[0] >= bytes)
        {
            return;
        }
        source = replay->push;
        available = replay->push_size;
        replay->push_fresh = false;
    }
    else
    {
        const tb_d3d11_buffer *const buffer = replay->buffers[slot - 1];
        const uint64_t offset = replay->offsets[slot - 1];
        if (buffer == NULL || buffer->cpu == NULL)
        {
            tb_d3d11_warn(device, TB_D3D11_WARN_CONSTANTS,
                          "a draw reads a constant buffer it was not given, or one in the GPU's "
                          "memory, which D3D11 reads constants of only through a copy in upload "
                          "memory; its constants are zero");
            return;
        }
        if (replay->filled_buffers[slot - 1] == buffer &&
            replay->filled_offsets[slot - 1] == offset && replay->filled_sizes[slot] >= bytes)
        {
            return;
        }
        source = buffer->cpu + offset;
        available = offset < buffer->size ? buffer->size - offset : 0;
        replay->filled_buffers[slot - 1] = buffer;
        replay->filled_offsets[slot - 1] = offset;
    }
    replay->filled_sizes[slot] = bytes;
    if (FAILED(ID3D11DeviceContext_Map(replay->context, (ID3D11Resource *)device->constants[slot],
                                       0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) ||
        mapped.pData == NULL)
    {
        /* coverage-off: a dynamic buffer maps unless the device is lost */
        return;
        /* coverage-on */
    }
    const size_t copied = available < bytes ? (size_t)available : bytes;
    memcpy(mapped.pData, source, copied);
    memset((uint8_t *)mapped.pData + copied, 0, bytes - copied);
    ID3D11DeviceContext_Unmap(replay->context, (ID3D11Resource *)device->constants[slot], 0);
}

/* What a stage's slots name, in the registers the shader compiler gave
 * them: a view of the shape the shader declares, or nothing. */
typedef struct tb_d3d11_bound
{
    ID3D11ShaderResourceView *reads[TB_D3D11_READ_REGISTERS];
    UINT read_count;
    ID3D11SamplerState *samplers[TB_D3D11_SAMPLER_REGISTERS];
    UINT sampler_count;
    ID3D11UnorderedAccessView *writes[TB_D3D11_WRITE_REGISTERS];
    UINT write_count;
} tb_d3d11_bound;

static void tb_d3d11_gather(const tb_d3d11_replay *replay, const tb_shader_dxbc_shader *shader,
                            tb_d3d11_bound *bound)
{
    tb_d3d11_device *const device = replay->device;

    for (uint32_t index = 0; index < shader->slot_count; index++)
    {
        const tb_shader_dxbc_slot *const slot = &shader->slots[index];
        const uint32_t at = slot->register_index;
        uint32_t heap_index = 0;
        if (!tb_d3d11_read_index(replay, slot, &heap_index))
        {
            tb_d3d11_warn(device, TB_D3D11_WARN_INDEX,
                          "a shader reads a heap index past the constants it was given; what it "
                          "names is not bound");
            continue;
        }
        if (slot->register_class == 's')
        {
            if (at < TB_D3D11_SAMPLER_REGISTERS)
            {
                bound->samplers[at] =
                    heap_index < device->sampler_count ? device->samplers[heap_index] : NULL;
                bound->sampler_count =
                    at + 1 > bound->sampler_count ? at + 1 : bound->sampler_count;
            }
            continue;
        }
        const tb_d3d11_view *const view =
            heap_index < device->view_count ? &device->views[heap_index] : NULL;
        const bool writable = slot->register_class == 'u';
        const bool fits = view != NULL && view->view != NULL && view->writable == writable &&
                          view->shape == slot->view;
        if (writable && at < TB_D3D11_WRITE_REGISTERS)
        {
            bound->writes[at] = fits ? (ID3D11UnorderedAccessView *)view->view : NULL;
            bound->write_count = at + 1 > bound->write_count ? at + 1 : bound->write_count;
        }
        else if (!writable && at < TB_D3D11_READ_REGISTERS)
        {
            bound->reads[at] = fits ? (ID3D11ShaderResourceView *)view->view : NULL;
            bound->read_count = at + 1 > bound->read_count ? at + 1 : bound->read_count;
        }
    }
}

static UINT tb_d3d11_larger(UINT left, UINT right)
{
    return left > right ? left : right;
}

/* Bind what a graphics stage takes, and unbind what it took before. */
static void tb_d3d11_bind_graphics_stage(tb_d3d11_replay *replay, uint32_t stage,
                                         const tb_shader_dxbc_shader *shader)
{
    tb_d3d11_bound bound = {0};
    UINT reads = 0;
    UINT samplers = 0;

    if (shader != NULL)
    {
        tb_d3d11_gather(replay, shader, &bound);
    }
    if (bound.write_count > 0)
    {
        tb_d3d11_warn(replay->device, TB_D3D11_WARN_PIXEL_WRITES,
                      "a pixel shader writes resources it takes from a heap, which the D3D11 "
                      "backend does not bind yet");
    }
    reads = tb_d3d11_larger(bound.read_count, replay->reads[stage]);
    samplers = tb_d3d11_larger(bound.sampler_count, replay->samplers[stage]);
    if (stage == TB_D3D11_VERTEX)
    {
        if (reads > 0)
        {
            ID3D11DeviceContext_VSSetShaderResources(replay->context, 0, reads, bound.reads);
        }
        if (samplers > 0)
        {
            ID3D11DeviceContext_VSSetSamplers(replay->context, 0, samplers, bound.samplers);
        }
    }
    else
    {
        if (reads > 0)
        {
            ID3D11DeviceContext_PSSetShaderResources(replay->context, 0, reads, bound.reads);
        }
        if (samplers > 0)
        {
            ID3D11DeviceContext_PSSetSamplers(replay->context, 0, samplers, bound.samplers);
        }
    }
    replay->reads[stage] = bound.read_count;
    replay->samplers[stage] = bound.sampler_count;
}

/* Unbind what the compute stage took, so that what it wrote can be read, or
 * drawn to, without D3D11 taking it away with a warning. */
static void tb_d3d11_unbind_compute(tb_d3d11_replay *replay)
{
    ID3D11ShaderResourceView *const reads[TB_D3D11_READ_REGISTERS] = {0};
    ID3D11UnorderedAccessView *const writes[TB_D3D11_WRITE_REGISTERS] = {0};

    if (replay->reads[TB_D3D11_COMPUTE] > 0)
    {
        ID3D11DeviceContext_CSSetShaderResources(replay->context, 0,
                                                 replay->reads[TB_D3D11_COMPUTE], reads);
        replay->reads[TB_D3D11_COMPUTE] = 0;
    }
    if (replay->writes > 0)
    {
        ID3D11DeviceContext_CSSetUnorderedAccessViews(replay->context, 0, replay->writes, writes,
                                                      NULL);
        replay->writes = 0;
    }
}

/* Unbind what the graphics stages read: a pass may draw to it, or a
 * dispatch write it. */
static void tb_d3d11_unbind_graphics(tb_d3d11_replay *replay)
{
    ID3D11ShaderResourceView *const reads[TB_D3D11_READ_REGISTERS] = {0};

    if (replay->reads[TB_D3D11_VERTEX] > 0)
    {
        ID3D11DeviceContext_VSSetShaderResources(replay->context, 0, replay->reads[TB_D3D11_VERTEX],
                                                 reads);
        replay->reads[TB_D3D11_VERTEX] = 0;
    }
    if (replay->reads[TB_D3D11_PIXEL] > 0)
    {
        ID3D11DeviceContext_PSSetShaderResources(replay->context, 0, replay->reads[TB_D3D11_PIXEL],
                                                 reads);
        replay->reads[TB_D3D11_PIXEL] = 0;
    }
}

static void tb_d3d11_before_draw(tb_d3d11_replay *replay)
{
    const tb_d3d11_pipeline *const pipeline = replay->pipeline;

    if (pipeline == NULL || pipeline->compute)
    {
        return;
    }
    tb_d3d11_unbind_compute(replay);
    for (uint32_t slot = 0; slot < TB_D3D11_CONSTANT_BUFFERS; slot++)
    {
        tb_d3d11_fill_constants(replay, slot);
    }
    tb_d3d11_bind_graphics_stage(replay, TB_D3D11_VERTEX, &pipeline->stages[0]);
    tb_d3d11_bind_graphics_stage(replay, TB_D3D11_PIXEL,
                                 pipeline->stage_count > 1 ? &pipeline->stages[1] : NULL);
}

static void tb_d3d11_before_dispatch(tb_d3d11_replay *replay)
{
    const tb_d3d11_pipeline *const pipeline = replay->pipeline;
    tb_d3d11_bound bound = {0};
    UINT reads = 0;
    UINT samplers = 0;
    UINT writes = 0;

    if (pipeline == NULL || !pipeline->compute)
    {
        return;
    }
    tb_d3d11_unbind_graphics(replay);
    for (uint32_t slot = 0; slot < TB_D3D11_CONSTANT_BUFFERS; slot++)
    {
        tb_d3d11_fill_constants(replay, slot);
    }
    tb_d3d11_gather(replay, &pipeline->stages[0], &bound);
    reads = tb_d3d11_larger(bound.read_count, replay->reads[TB_D3D11_COMPUTE]);
    samplers = tb_d3d11_larger(bound.sampler_count, replay->samplers[TB_D3D11_COMPUTE]);
    writes = tb_d3d11_larger(bound.write_count, replay->writes);
    /* What is written is unbound from reading first, and the other way
     * around, so that no view is bound both ways at once. */
    if (writes > 0)
    {
        ID3D11UnorderedAccessView *const none[TB_D3D11_WRITE_REGISTERS] = {0};
        ID3D11DeviceContext_CSSetUnorderedAccessViews(replay->context, 0, writes, none, NULL);
    }
    if (reads > 0)
    {
        ID3D11DeviceContext_CSSetShaderResources(replay->context, 0, reads, bound.reads);
    }
    if (samplers > 0)
    {
        ID3D11DeviceContext_CSSetSamplers(replay->context, 0, samplers, bound.samplers);
    }
    if (writes > 0)
    {
        ID3D11DeviceContext_CSSetUnorderedAccessViews(replay->context, 0, writes, bound.writes,
                                                      NULL);
    }
    replay->reads[TB_D3D11_COMPUTE] = bound.read_count;
    replay->samplers[TB_D3D11_COMPUTE] = bound.sampler_count;
    replay->writes = bound.write_count;
}

/* ---------------------------------------------------------------------------
 * Replaying each command.
 * ------------------------------------------------------------------------- */

static void tb_d3d11_replay_pass(tb_d3d11_replay *replay, const tb_d3d11_pass *pass)
{
    ID3D11RenderTargetView *targets[TB_RHI_COLOR_TARGETS] = {0};
    ID3D11DepthStencilView *depth = NULL;
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, (FLOAT)pass->width, (FLOAT)pass->height,
                                     0.0f, 1.0f};
    const D3D11_RECT scissor = {0, 0, (LONG)pass->width, (LONG)pass->height};

    tb_d3d11_unbind_graphics(replay);
    tb_d3d11_unbind_compute(replay);
    for (uint32_t index = 0; index < pass->color_count; index++)
    {
        targets[index] = tb_d3d11_target_view(replay->device, pass->colors[index],
                                              pass->mips[index], pass->layers[index]);
        if (pass->clear[index] && targets[index] != NULL)
        {
            ID3D11DeviceContext_ClearRenderTargetView(replay->context, targets[index],
                                                      pass->clears[index]);
        }
    }
    if (pass->depth != NULL)
    {
        depth = tb_d3d11_depth_view(replay->device, pass->depth, pass->read_only);
        if (pass->depth_clear && depth != NULL)
        {
            /* A read-only view cannot be cleared: its written one can. */
            ID3D11DepthStencilView *const written =
                tb_d3d11_depth_view(replay->device, pass->depth, false);
            ID3D11DeviceContext_ClearDepthStencilView(replay->context, written,
                                                      D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                                      pass->depth_value, pass->stencil_value);
        }
    }
    ID3D11DeviceContext_OMSetRenderTargets(replay->context, pass->color_count, targets, depth);
    ID3D11DeviceContext_RSSetViewports(replay->context, 1, &viewport);
    ID3D11DeviceContext_RSSetScissorRects(replay->context, 1, &scissor);
}

static void tb_d3d11_replay_pipeline(tb_d3d11_replay *replay, const tb_d3d11_bind *bind)
{
    ID3D11DeviceContext *const context = replay->context;
    const tb_d3d11_pipeline *const pipeline = bind->object;
    ID3D11Buffer *const *const constants = replay->device->constants;

    replay->pipeline = pipeline;
    /* Another pipeline may read more of the constants. */
    memset(replay->filled_sizes, 0, sizeof(replay->filled_sizes));
    if (pipeline->compute)
    {
        ID3D11DeviceContext_CSSetShader(context, pipeline->compute_shader, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(context, 0, TB_D3D11_CONSTANT_BUFFERS, constants);
        return;
    }
    ID3D11DeviceContext_IASetInputLayout(context, pipeline->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, pipeline->topology);
    ID3D11DeviceContext_VSSetShader(context, pipeline->vertex, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, pipeline->pixel, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(context, 0, TB_D3D11_CONSTANT_BUFFERS, constants);
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, TB_D3D11_CONSTANT_BUFFERS, constants);
    ID3D11DeviceContext_RSSetState(context, pipeline->rasterizer);
    ID3D11DeviceContext_OMSetDepthStencilState(context, pipeline->depth, 0);
    ID3D11DeviceContext_OMSetBlendState(context, pipeline->blend, NULL, 0xFFFFFFFFu);
}

static void tb_d3d11_push_readback(tb_d3d11_device *device, const tb_d3d11_readback *readback)
{
    if (device->readback_count == device->readback_capacity)
    {
        device->readback_capacity =
            device->readback_capacity == 0 ? 8 : device->readback_capacity * 2;
        device->readbacks = tb_realloc_array(device->readbacks, device->readback_capacity,
                                             sizeof(*device->readbacks));
    }
    device->readbacks[device->readback_count] = *readback;
    device->readback_count++;
}

static void tb_d3d11_replay_copy_buffer(tb_d3d11_replay *replay, const tb_d3d11_buffer_copy *copy)
{
    tb_d3d11_device *const device = replay->device;
    const D3D11_BOX box = {
        (UINT)copy->source_offset, 0, 0, (UINT)(copy->source_offset + copy->size), 1, 1};

    if (copy->dest->memory == TB_RHI_MEMORY_READBACK)
    {
        if (copy->source->buffer == NULL)
        {
            /* Upload memory read only as constants is the CPU's alone. */
            memcpy(copy->dest->cpu + copy->dest_offset, copy->source->cpu + copy->source_offset,
                   (size_t)copy->size);
            return;
        }
        D3D11_BUFFER_DESC staging = {0};
        tb_d3d11_readback readback = {0};
        staging.ByteWidth = (UINT)copy->size;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateBuffer(device->device, &staging, NULL,
                                             (ID3D11Buffer **)&readback.staging)))
        {
            /* coverage-off: a staging buffer is made unless memory runs out */
            return;
            /* coverage-on */
        }
        ID3D11DeviceContext_CopySubresourceRegion(replay->context, readback.staging, 0, 0, 0, 0,
                                                  (ID3D11Resource *)copy->source->buffer, 0, &box);
        readback.buffer = copy->dest;
        readback.offset = copy->dest_offset;
        readback.size = copy->size;
        readback.submission = replay->submission;
        tb_d3d11_push_readback(device, &readback);
        return;
    }
    if (copy->dest->buffer == NULL || copy->dest->memory == TB_RHI_MEMORY_UPLOAD)
    {
        tb_d3d11_warn(device, TB_D3D11_WARN_COPY,
                      "a copy into upload memory, which the GPU does not write");
        return;
    }
    if (copy->source->cpu != NULL && copy->source->memory == TB_RHI_MEMORY_UPLOAD)
    {
        const D3D11_BOX dest = {
            (UINT)copy->dest_offset, 0, 0, (UINT)(copy->dest_offset + copy->size), 1, 1};
        ID3D11DeviceContext_UpdateSubresource(replay->context, (ID3D11Resource *)copy->dest->buffer,
                                              0, &dest, copy->source->cpu + copy->source_offset, 0,
                                              0);
    }
    else if (copy->source->buffer != NULL)
    {
        ID3D11DeviceContext_CopySubresourceRegion(
            replay->context, (ID3D11Resource *)copy->dest->buffer, 0, (UINT)copy->dest_offset, 0, 0,
            (ID3D11Resource *)copy->source->buffer, 0, &box);
    }
    /* A constant buffer in the GPU's memory keeps a CPU copy of what is
     * copied into it, which draws read their constants from. */
    if (copy->dest->cpu != NULL && copy->source->cpu != NULL)
    {
        memcpy(copy->dest->cpu + copy->dest_offset, copy->source->cpu + copy->source_offset,
               (size_t)copy->size);
    }
}

/* A level of a layer of a texture, as D3D11 numbers them. */
static UINT tb_d3d11_subresource(const tb_d3d11_texture *texture, uint32_t mip, uint32_t layer)
{
    return mip + layer * texture->desc.mips;
}

static void tb_d3d11_replay_upload_texture(tb_d3d11_replay *replay,
                                           const tb_d3d11_texture_copy *copy)
{
    const tb_d3d11_texture *const texture = copy->texture;
    const D3D11_BOX box = {(UINT)copy->region.x,
                           (UINT)copy->region.y,
                           0,
                           (UINT)copy->region.x + copy->region.width,
                           (UINT)copy->region.y + copy->region.height,
                           1};

    if (copy->buffer->cpu == NULL || copy->buffer->memory != TB_RHI_MEMORY_UPLOAD ||
        tb_rhi_format_is_depth(texture->desc.format))
    {
        tb_d3d11_warn(replay->device, TB_D3D11_WARN_COPY,
                      "a copy into a texture from other than upload memory, or into depth, which "
                      "D3D11 does not make");
        return;
    }
    ID3D11DeviceContext_UpdateSubresource(replay->context, (ID3D11Resource *)texture->texture,
                                          tb_d3d11_subresource(texture, copy->mip, copy->layer),
                                          &box, copy->buffer->cpu + copy->offset, copy->row_pitch,
                                          0);
}

static void tb_d3d11_replay_read_texture(tb_d3d11_replay *replay, const tb_d3d11_texture_copy *copy)
{
    tb_d3d11_device *const device = replay->device;
    const tb_d3d11_texture *const texture = copy->texture;
    const uint32_t texel = tb_rhi_format_size(texture->desc.format);
    /* D3D11 copies a depth texture's level whole, or not at all. */
    const bool whole = tb_rhi_format_is_depth(texture->desc.format);
    D3D11_TEXTURE2D_DESC staging = {0};
    tb_d3d11_readback readback = {0};
    const D3D11_BOX box = {(UINT)copy->region.x,
                           (UINT)copy->region.y,
                           0,
                           (UINT)copy->region.x + copy->region.width,
                           (UINT)copy->region.y + copy->region.height,
                           1};

    if (copy->buffer->memory != TB_RHI_MEMORY_READBACK)
    {
        tb_d3d11_warn(device, TB_D3D11_WARN_COPY,
                      "a copy from a texture into other than readback memory, which D3D11 does "
                      "not make");
        return;
    }
    staging.Width = whole ? tb_rhi_mip_size(texture->desc.width, copy->mip) : copy->region.width;
    staging.Height = whole ? tb_rhi_mip_size(texture->desc.height, copy->mip) : copy->region.height;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Format = texture->resource_format;
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(device->device, &staging, NULL,
                                            (ID3D11Texture2D **)&readback.staging)))
    {
        /* coverage-off: a staging texture is made unless memory runs out */
        return;
        /* coverage-on */
    }
    ID3D11DeviceContext_CopySubresourceRegion(
        replay->context, readback.staging, 0, 0, 0, 0, (ID3D11Resource *)texture->texture,
        tb_d3d11_subresource(texture, copy->mip, copy->layer), whole ? NULL : &box);
    readback.buffer = copy->buffer;
    readback.offset = copy->offset;
    readback.texture = true;
    readback.row_pitch = copy->row_pitch;
    readback.row_bytes = copy->region.width * texel;
    readback.rows = copy->region.height;
    readback.x_bytes = whole ? (uint32_t)copy->region.x * texel : 0u;
    readback.y = whole ? (uint32_t)copy->region.y : 0u;
    readback.submission = replay->submission;
    tb_d3d11_push_readback(device, &readback);
}

static void tb_d3d11_replay_marker(tb_d3d11_replay *replay, const tb_d3d11_marker *marker)
{
    wchar_t wide[TB_D3D11_MARKER_BYTES] = {0};

    if (replay->device->annotation == NULL)
    {
        return;
    }
    /* Debuggers show the name; ASCII is all a name needs to be read there. */
    for (size_t index = 0; index < TB_D3D11_MARKER_BYTES - 1 && marker->name[index] != '\0';
         index++)
    {
        wide[index] = (wchar_t)(unsigned char)marker->name[index];
    }
    (void)ID3DUserDefinedAnnotation_BeginEvent(replay->device->annotation, wide);
}

static void tb_d3d11_replay_buffer(tb_d3d11_replay *replay, tb_d3d11_op op,
                                   const tb_d3d11_bind *bind)
{
    const tb_d3d11_buffer *const buffer = bind->object;

    if (op == TB_D3D11_OP_CONSTANT_BUFFER)
    {
        replay->buffers[bind->slot] = buffer;
        replay->offsets[bind->slot] = bind->offset;
    }
    else if (op == TB_D3D11_OP_VERTEX_BUFFER)
    {
        const UINT stride = bind->stride;
        const UINT offset = (UINT)bind->offset;
        ID3D11DeviceContext_IASetVertexBuffers(replay->context, bind->slot, 1, &buffer->buffer,
                                               &stride, &offset);
    }
    else
    {
        ID3D11DeviceContext_IASetIndexBuffer(replay->context, buffer->buffer,
                                             tb_d3d11_format(bind->format), (UINT)bind->offset);
    }
}

static void tb_d3d11_replay_command(tb_d3d11_replay *replay, tb_d3d11_op op, const void *payload)
{
    ID3D11DeviceContext *const context = replay->context;

    switch (op)
    {
    case TB_D3D11_OP_BEGIN_PASS:
        tb_d3d11_replay_pass(replay, payload);
        return;
    case TB_D3D11_OP_END_PASS:
        ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
        return;
    case TB_D3D11_OP_VIEWPORT: {
        const float *const values = ((const tb_d3d11_viewport *)payload)->values;
        const D3D11_VIEWPORT viewport = {values[0], values[1], values[2],
                                         values[3], values[4], values[5]};
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        return;
    }
    case TB_D3D11_OP_SCISSOR: {
        const tb_rhi_rect *const rect = payload;
        const D3D11_RECT scissor = {rect->x, rect->y, (LONG)(rect->x + (LONG)rect->width),
                                    (LONG)(rect->y + (LONG)rect->height)};
        ID3D11DeviceContext_RSSetScissorRects(context, 1, &scissor);
        return;
    }
    case TB_D3D11_OP_PIPELINE:
        tb_d3d11_replay_pipeline(replay, payload);
        return;
    case TB_D3D11_OP_PUSH_CONSTANTS: {
        const tb_d3d11_constants *const constants = payload;
        memcpy(replay->push, constants->bytes, constants->size);
        replay->push_size = constants->size;
        replay->push_fresh = true;
        return;
    }
    case TB_D3D11_OP_CONSTANT_BUFFER:
    case TB_D3D11_OP_VERTEX_BUFFER:
    case TB_D3D11_OP_INDEX_BUFFER:
        tb_d3d11_replay_buffer(replay, op, payload);
        return;
    case TB_D3D11_OP_DRAW: {
        const uint32_t *const counts = ((const tb_d3d11_draw *)payload)->counts;
        tb_d3d11_before_draw(replay);
        ID3D11DeviceContext_DrawInstanced(context, counts[0], counts[1], counts[2], counts[3]);
        return;
    }
    case TB_D3D11_OP_DRAW_INDEXED: {
        const tb_d3d11_draw *const draw = payload;
        tb_d3d11_before_draw(replay);
        ID3D11DeviceContext_DrawIndexedInstanced(context, draw->counts[0], draw->counts[1],
                                                 draw->counts[2], draw->vertex_offset,
                                                 draw->counts[3]);
        return;
    }
    case TB_D3D11_OP_DISPATCH: {
        const uint32_t *const counts = ((const tb_d3d11_draw *)payload)->counts;
        tb_d3d11_before_dispatch(replay);
        ID3D11DeviceContext_Dispatch(context, counts[0], counts[1], counts[2]);
        return;
    }
    case TB_D3D11_OP_COPY_BUFFER:
        tb_d3d11_replay_copy_buffer(replay, payload);
        return;
    case TB_D3D11_OP_COPY_BUFFER_TO_TEXTURE:
        tb_d3d11_replay_upload_texture(replay, payload);
        return;
    case TB_D3D11_OP_COPY_TEXTURE_TO_BUFFER:
        tb_d3d11_replay_read_texture(replay, payload);
        return;
    case TB_D3D11_OP_BEGIN_MARKER:
        tb_d3d11_replay_marker(replay, payload);
        return;
    case TB_D3D11_OP_END_MARKER:
        if (replay->device->annotation != NULL)
        {
            (void)ID3DUserDefinedAnnotation_EndEvent(replay->device->annotation);
        }
        return;
    }
}

static void tb_d3d11_replay_list(tb_d3d11_replay *replay, const tb_d3d11_list *list)
{
    for (size_t at = 0; at < list->length;)
    {
        const tb_d3d11_record *const record = (const tb_d3d11_record *)(void *)(list->bytes + at);
        tb_d3d11_replay_command(replay, (tb_d3d11_op)record->op, record + 1);
        at += record->size;
    }
}

/* ---------------------------------------------------------------------------
 * Submitting, and reading back.
 * ------------------------------------------------------------------------- */

/* Bring the GPU's copy of each upload buffer up to date where the CPU wrote. */
static void tb_d3d11_flush_uploads(tb_d3d11_device *device)
{
    for (size_t index = 0; index < device->upload_count; index++)
    {
        tb_d3d11_buffer *const buffer = device->uploads[index];
        if (buffer->dirty_end <= buffer->dirty_begin)
        {
            continue;
        }
        const D3D11_BOX box = {(UINT)buffer->dirty_begin, 0, 0, (UINT)buffer->dirty_end, 1, 1};
        ID3D11DeviceContext_UpdateSubresource(device->context, (ID3D11Resource *)buffer->buffer, 0,
                                              &box, buffer->cpu + buffer->dirty_begin, 0, 0);
        buffer->dirty_begin = 0;
        buffer->dirty_end = 0;
    }
}

static void tb_d3d11_signal(tb_d3d11_device *device, uint64_t submission)
{
    const D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *query = NULL;

    if (FAILED(ID3D11Device_CreateQuery(device->device, &desc, &query)))
    {
        /* coverage-off: an event query is made unless the device is lost */
        return;
        /* coverage-on */
    }
    ID3D11DeviceContext_End(device->context, (ID3D11Asynchronous *)query);
    if (device->pending_count == device->pending_capacity)
    {
        device->pending_capacity = device->pending_capacity == 0 ? 8 : device->pending_capacity * 2;
        device->pending =
            tb_realloc_array(device->pending, device->pending_capacity, sizeof(*device->pending));
    }
    device->pending[device->pending_count] = (tb_d3d11_pending){query, submission};
    device->pending_count++;
}

tb_rhi_result tb_d3d11_submit(void *state, void *const *lists, size_t count, uint64_t submission)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_replay replay = {0};

    tb_mutex_lock(&device->context_lock);
    tb_d3d11_flush_uploads(device);
    for (size_t index = 0; index < count; index++)
    {
        /* Each list starts with nothing of the one before it. */
        replay = (tb_d3d11_replay){0};
        replay.device = device;
        replay.context = device->context;
        replay.submission = submission;
        tb_d3d11_replay_list(&replay, lists[index]);
        tb_d3d11_unbind_graphics(&replay);
        tb_d3d11_unbind_compute(&replay);
        ID3D11DeviceContext_OMSetRenderTargets(device->context, 0, NULL, NULL);
    }
    tb_d3d11_signal(device, submission);
    ID3D11DeviceContext_Flush(device->context);
    tb_mutex_unlock(&device->context_lock);
    for (size_t index = 0; index < count; index++)
    {
        tb_d3d11_list_give(device, lists[index]);
    }
    tb_d3d11_drain_messages(device);
    if (FAILED(ID3D11Device_GetDeviceRemovedReason(device->device)))
    {
        return TB_RHI_DEVICE_LOST;
    }
    return TB_RHI_OK;
}

static void tb_d3d11_read_staging(tb_d3d11_device *device, const tb_d3d11_readback *readback)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {0};

    if (FAILED(ID3D11DeviceContext_Map(device->context, readback->staging, 0, D3D11_MAP_READ, 0,
                                       &mapped)) ||
        mapped.pData == NULL)
    {
        /* coverage-off: a staging resource maps unless the device is lost */
        return;
        /* coverage-on */
    }
    if (!readback->texture)
    {
        memcpy(readback->buffer->cpu + readback->offset, mapped.pData, (size_t)readback->size);
    }
    for (uint32_t row = 0; readback->texture && row < readback->rows; row++)
    {
        const uint8_t *const from = (const uint8_t *)mapped.pData +
                                    (size_t)(row + readback->y) * mapped.RowPitch +
                                    readback->x_bytes;
        memcpy(readback->buffer->cpu + readback->offset + (uint64_t)row * readback->row_pitch, from,
               readback->row_bytes);
    }
    ID3D11DeviceContext_Unmap(device->context, readback->staging, 0);
}

void tb_d3d11_resolve_readbacks(tb_d3d11_device *device, const tb_d3d11_buffer *buffer)
{
    const uint64_t completed = tb_d3d11_poll(device);
    size_t kept = 0;

    for (size_t index = 0; index < device->readback_count; index++)
    {
        tb_d3d11_readback *const readback = &device->readbacks[index];
        if (readback->buffer == buffer && readback->submission <= completed)
        {
            tb_d3d11_read_staging(device, readback);
            (void)ID3D11Resource_Release(readback->staging);
            continue;
        }
        device->readbacks[kept] = *readback;
        kept++;
    }
    device->readback_count = kept;
}

void tb_d3d11_drop_readbacks(tb_d3d11_device *device, const tb_d3d11_buffer *buffer)
{
    size_t kept = 0;

    for (size_t index = 0; index < device->readback_count; index++)
    {
        tb_d3d11_readback *const readback = &device->readbacks[index];
        if (readback->buffer == buffer)
        {
            (void)ID3D11Resource_Release(readback->staging);
            continue;
        }
        device->readbacks[kept] = *readback;
        kept++;
    }
    device->readback_count = kept;
}
