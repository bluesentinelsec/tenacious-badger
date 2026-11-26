/* The null backend: accepts every call and draws nothing, as Unreal's null
 * RHI does. Buffers keep their bytes, so what is written to a buffer can be
 * read back; the GPU finishes each submission the moment it is made. */
#include "rhi_backend.h"

#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/thread.h"
#include "badger/window.h"

#include <string.h>

typedef struct tb_rhi_null_state
{
    tb_mutex lock; /* guards completed */
    uint64_t completed;
} tb_rhi_null_state;

typedef struct tb_rhi_null_buffer
{
    uint8_t *bytes;
} tb_rhi_null_buffer;

typedef struct tb_rhi_null_chain
{
    tb_rhi_swap_chain_images images;
    uint32_t current;
    struct tb_window *window;
} tb_rhi_null_chain;

/* Every object but a buffer is a byte of its own, so that each has an
 * address and a leak of one is found. */
static void *tb_rhi_null_object(void)
{
    return tb_calloc(1, 1);
}

static tb_rhi_result tb_rhi_null_device_create(const tb_rhi_device_options *options, void **state,
                                               tb_rhi_device_info *info, tb_rhi_error *error)
{
    (void)options;
    (void)error;
    *state = tb_calloc(1, sizeof(tb_rhi_null_state));
    (void)tb_fmt_buffer(info->adapter, sizeof(info->adapter), "%s", "null");
    info->features = TB_RHI_FEATURE_BINDLESS | TB_RHI_FEATURE_HEAP_INDICES;
    info->shader_model = 66;
    info->max_texture_size = 16384;
    return TB_RHI_OK;
}

static void tb_rhi_null_device_destroy(void *state)
{
    tb_free(state);
}

static tb_rhi_result tb_rhi_null_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                               tb_rhi_indices indices, void **object,
                                               tb_rhi_error *error)
{
    tb_rhi_null_buffer *buffer = NULL;

    (void)state;
    (void)indices;
    if (desc->size > SIZE_MAX)
    {
        return tb_rhi_fail(error, TB_RHI_OUT_OF_MEMORY, "a buffer of %llu bytes",
                           (unsigned long long)desc->size);
    }
    buffer = tb_calloc(1, sizeof(*buffer));
    buffer->bytes = tb_calloc((size_t)desc->size, 1);
    *object = buffer;
    return TB_RHI_OK;
}

static void tb_rhi_null_buffer_destroy(void *state, void *object)
{
    tb_rhi_null_buffer *const buffer = object;

    (void)state;
    tb_free(buffer->bytes);
    tb_free(buffer);
}

static void tb_rhi_null_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                                     size_t size)
{
    tb_rhi_null_buffer *const buffer = object;

    (void)state;
    memcpy(buffer->bytes + offset, data, size);
}

static void tb_rhi_null_buffer_read(void *state, void *object, uint64_t offset, void *data,
                                    size_t size)
{
    const tb_rhi_null_buffer *const buffer = object;

    (void)state;
    memcpy(data, buffer->bytes + offset, size);
}

static tb_rhi_result tb_rhi_null_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                                tb_rhi_indices indices, void **object,
                                                tb_rhi_error *error)
{
    (void)state;
    (void)desc;
    (void)indices;
    (void)error;
    *object = tb_rhi_null_object();
    return TB_RHI_OK;
}

static tb_rhi_result tb_rhi_null_sampler_create(void *state, const tb_rhi_sampler_desc *desc,
                                                tb_rhi_indices indices, void **object,
                                                tb_rhi_error *error)
{
    (void)state;
    (void)desc;
    (void)indices;
    (void)error;
    *object = tb_rhi_null_object();
    return TB_RHI_OK;
}

static tb_rhi_result tb_rhi_null_shader_create(void *state, const tb_rhi_shader_desc *desc,
                                               void **object, tb_rhi_error *error)
{
    (void)state;
    (void)desc;
    (void)error;
    *object = tb_rhi_null_object();
    return TB_RHI_OK;
}

static tb_rhi_result tb_rhi_null_graphics_pipeline_create(
    void *state, const tb_rhi_graphics_pipeline_ref *pipeline, void **object, tb_rhi_error *error)
{
    (void)state;
    (void)pipeline;
    (void)error;
    *object = tb_rhi_null_object();
    return TB_RHI_OK;
}

static tb_rhi_result tb_rhi_null_compute_pipeline_create(void *state,
                                                         const tb_rhi_compute_pipeline_desc *desc,
                                                         void *shader,
                                                         const tb_rhi_shader_desc *shader_desc,
                                                         void **object, tb_rhi_error *error)
{
    (void)state;
    (void)desc;
    (void)shader;
    (void)shader_desc;
    (void)error;
    *object = tb_rhi_null_object();
    return TB_RHI_OK;
}

/* Objects of a byte, released alike. */
static void tb_rhi_null_destroy(void *state, void *object)
{
    (void)state;
    tb_free(object);
}

static void tb_rhi_null_images(tb_rhi_null_chain *chain, uint32_t width, uint32_t height)
{
    tb_window_state window = {0};

    tb_window_state_of(chain->window, &window);
    chain->images.width = width == 0 ? window.pixel_width : width;
    chain->images.height = height == 0 ? window.pixel_height : height;
    chain->images.width = chain->images.width == 0 ? 1u : chain->images.width;
    chain->images.height = chain->images.height == 0 ? 1u : chain->images.height;
    for (uint32_t index = 0; index < chain->images.count; index++)
    {
        chain->images.objects[index] = tb_rhi_null_object();
    }
    chain->current = 0;
}

static void tb_rhi_null_release_images(tb_rhi_null_chain *chain)
{
    for (uint32_t index = 0; index < chain->images.count; index++)
    {
        tb_free(chain->images.objects[index]);
        chain->images.objects[index] = NULL;
    }
}

static tb_rhi_result tb_rhi_null_swap_chain_create(void *state, struct tb_window *window,
                                                   const tb_rhi_swap_chain_desc *desc, void **chain,
                                                   tb_rhi_swap_chain_images *images,
                                                   tb_rhi_error *error)
{
    tb_rhi_null_chain *made = NULL;

    (void)state;
    (void)error;
    made = tb_calloc(1, sizeof(*made));
    made->window = window;
    made->images.count = desc->images;
    made->images.format = desc->format;
    tb_rhi_null_images(made, 0, 0);
    *images = made->images;
    *chain = made;
    return TB_RHI_OK;
}

static void tb_rhi_null_swap_chain_destroy(void *state, void *chain)
{
    (void)state;
    tb_rhi_null_release_images(chain);
    tb_free(chain);
}

static uint32_t tb_rhi_null_swap_chain_current(void *state, void *chain)
{
    const tb_rhi_null_chain *const null_chain = chain;

    (void)state;
    return null_chain->current;
}

static tb_rhi_result tb_rhi_null_swap_chain_present(void *state, void *chain)
{
    tb_rhi_null_chain *const null_chain = chain;

    (void)state;
    null_chain->current = (null_chain->current + 1) % null_chain->images.count;
    return TB_RHI_OK;
}

static tb_rhi_result tb_rhi_null_swap_chain_resize(void *state, void *chain, uint32_t width,
                                                   uint32_t height, bool vsync,
                                                   tb_rhi_swap_chain_images *images,
                                                   tb_rhi_error *error)
{
    tb_rhi_null_chain *const null_chain = chain;

    (void)state;
    (void)vsync;
    (void)error;
    tb_rhi_null_release_images(null_chain);
    tb_rhi_null_images(null_chain, width, height);
    *images = null_chain->images;
    return TB_RHI_OK;
}

static void *tb_rhi_null_list_begin(void *state)
{
    (void)state;
    return tb_rhi_null_object();
}

static void tb_rhi_null_list_discard(void *state, void *list)
{
    (void)state;
    tb_free(list);
}

static tb_rhi_result tb_rhi_null_submit(void *state, void *const *lists, size_t count,
                                        uint64_t submission)
{
    tb_rhi_null_state *const null_state = state;

    for (size_t index = 0; index < count; index++)
    {
        tb_free(lists[index]);
    }
    tb_mutex_lock(&null_state->lock);
    null_state->completed = submission;
    tb_mutex_unlock(&null_state->lock);
    return TB_RHI_OK;
}

static uint64_t tb_rhi_null_completed(void *state)
{
    tb_rhi_null_state *const null_state = state;
    uint64_t completed = 0;

    tb_mutex_lock(&null_state->lock);
    completed = null_state->completed;
    tb_mutex_unlock(&null_state->lock);
    return completed;
}

static tb_rhi_result tb_rhi_null_wait(void *state, uint64_t submission)
{
    (void)state;
    (void)submission;
    return TB_RHI_OK;
}

static void tb_rhi_null_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count)
{
    (void)list;
    (void)barriers;
    (void)count;
}

static void tb_rhi_null_cmd_begin_pass(void *list, const tb_rhi_pass_ref *pass)
{
    (void)list;
    (void)pass;
}

static void tb_rhi_null_cmd_list(void *list)
{
    (void)list;
}

static void tb_rhi_null_cmd_viewport(void *list, float x, float y, float width, float height,
                                     float near_depth, float far_depth)
{
    (void)list;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)near_depth;
    (void)far_depth;
}

static void tb_rhi_null_cmd_scissor(void *list, tb_rhi_rect rect)
{
    (void)list;
    (void)rect;
}

static void tb_rhi_null_cmd_pipeline(void *list, void *pipeline, bool compute)
{
    (void)list;
    (void)pipeline;
    (void)compute;
}

static void tb_rhi_null_cmd_push_constants(void *list, const void *data, size_t size)
{
    (void)list;
    (void)data;
    (void)size;
}

static void tb_rhi_null_cmd_constant_buffer(void *list, uint32_t slot, void *buffer,
                                            uint64_t offset)
{
    (void)list;
    (void)slot;
    (void)buffer;
    (void)offset;
}

static void tb_rhi_null_cmd_vertex_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset,
                                          uint64_t size, uint32_t stride)
{
    (void)list;
    (void)slot;
    (void)buffer;
    (void)offset;
    (void)size;
    (void)stride;
}

static void tb_rhi_null_cmd_index_buffer(void *list, void *buffer, uint64_t offset, uint64_t size,
                                         tb_rhi_format format)
{
    (void)list;
    (void)buffer;
    (void)offset;
    (void)size;
    (void)format;
}

static void tb_rhi_null_cmd_draw(void *list, uint32_t vertices, uint32_t instances,
                                 uint32_t first_vertex, uint32_t first_instance)
{
    (void)list;
    (void)vertices;
    (void)instances;
    (void)first_vertex;
    (void)first_instance;
}

static void tb_rhi_null_cmd_draw_indexed(void *list, uint32_t indices, uint32_t instances,
                                         uint32_t first_index, int32_t vertex_offset,
                                         uint32_t first_instance)
{
    (void)list;
    (void)indices;
    (void)instances;
    (void)first_index;
    (void)vertex_offset;
    (void)first_instance;
}

static void tb_rhi_null_cmd_dispatch(void *list, uint32_t x, uint32_t y, uint32_t z)
{
    (void)list;
    (void)x;
    (void)y;
    (void)z;
}

static void tb_rhi_null_cmd_copy_buffer(void *list, void *dest, uint64_t dest_offset, void *source,
                                        uint64_t source_offset, uint64_t size)
{
    (void)list;
    (void)dest;
    (void)dest_offset;
    (void)source;
    (void)source_offset;
    (void)size;
}

static void tb_rhi_null_cmd_copy_texture(void *list, const tb_rhi_texture_copy_ref *copy)
{
    (void)list;
    (void)copy;
}

static void tb_rhi_null_cmd_begin_marker(void *list, const char *name)
{
    (void)list;
    (void)name;
}

const tb_rhi_backend_table tb_rhi_null_backend = {
    .backend = TB_RHI_BACKEND_NULL,
    .device_create = tb_rhi_null_device_create,
    .device_destroy = tb_rhi_null_device_destroy,
    .buffer_create = tb_rhi_null_buffer_create,
    .buffer_destroy = tb_rhi_null_buffer_destroy,
    .buffer_write = tb_rhi_null_buffer_write,
    .buffer_read = tb_rhi_null_buffer_read,
    .texture_create = tb_rhi_null_texture_create,
    .texture_destroy = tb_rhi_null_destroy,
    .sampler_create = tb_rhi_null_sampler_create,
    .sampler_destroy = tb_rhi_null_destroy,
    .shader_create = tb_rhi_null_shader_create,
    .shader_destroy = tb_rhi_null_destroy,
    .graphics_pipeline_create = tb_rhi_null_graphics_pipeline_create,
    .compute_pipeline_create = tb_rhi_null_compute_pipeline_create,
    .pipeline_destroy = tb_rhi_null_destroy,
    .swap_chain_create = tb_rhi_null_swap_chain_create,
    .swap_chain_destroy = tb_rhi_null_swap_chain_destroy,
    .swap_chain_current = tb_rhi_null_swap_chain_current,
    .swap_chain_present = tb_rhi_null_swap_chain_present,
    .swap_chain_resize = tb_rhi_null_swap_chain_resize,
    .list_begin = tb_rhi_null_list_begin,
    .list_discard = tb_rhi_null_list_discard,
    .submit = tb_rhi_null_submit,
    .completed = tb_rhi_null_completed,
    .wait = tb_rhi_null_wait,
    .cmd_barriers = tb_rhi_null_cmd_barriers,
    .cmd_begin_pass = tb_rhi_null_cmd_begin_pass,
    .cmd_end_pass = tb_rhi_null_cmd_list,
    .cmd_viewport = tb_rhi_null_cmd_viewport,
    .cmd_scissor = tb_rhi_null_cmd_scissor,
    .cmd_pipeline = tb_rhi_null_cmd_pipeline,
    .cmd_push_constants = tb_rhi_null_cmd_push_constants,
    .cmd_constant_buffer = tb_rhi_null_cmd_constant_buffer,
    .cmd_vertex_buffer = tb_rhi_null_cmd_vertex_buffer,
    .cmd_index_buffer = tb_rhi_null_cmd_index_buffer,
    .cmd_draw = tb_rhi_null_cmd_draw,
    .cmd_draw_indexed = tb_rhi_null_cmd_draw_indexed,
    .cmd_dispatch = tb_rhi_null_cmd_dispatch,
    .cmd_copy_buffer = tb_rhi_null_cmd_copy_buffer,
    .cmd_copy_buffer_to_texture = tb_rhi_null_cmd_copy_texture,
    .cmd_copy_texture_to_buffer = tb_rhi_null_cmd_copy_texture,
    .cmd_begin_marker = tb_rhi_null_cmd_begin_marker,
    .cmd_end_marker = tb_rhi_null_cmd_list,
};
