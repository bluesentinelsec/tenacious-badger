#include "metal_internal.h"

#include "badger/fmt.h"
#include "badger/memory.h"

#include <string.h>

void tb_metal_release(void *state, void *object)
{
    tb_metal_call(state, object, "release");
}
void tb_metal_device_destroy(void *state)
{
    tb_metal_device *const d = state;
    if (d->send != NULL)
    {
        tb_metal_heaps_destroy(d);
        tb_metal_release(d, d->queue);
        tb_metal_release(d, d->gpu);
    }
    tb_library_close(d->metal);
    tb_library_close(d->objc);
    tb_free(d);
}

tb_rhi_result tb_metal_device_create(const tb_rhi_device_options *options, void **state,
                                     tb_rhi_device_info *info, tb_rhi_error *error)
{
    if (options->adapter == TB_RHI_ADAPTER_SOFTWARE)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Metal requires a hardware GPU");
    }
    tb_metal_device *const d = tb_calloc(1, sizeof(*d));
    tb_library_function functions[5] = {0};
    const tb_library_binding runtime[] = {{"objc_getClass", &functions[0], true},
                                          {"sel_registerName", &functions[1], true},
                                          {"objc_msgSend", &functions[2], true},
                                          {"objc_autoreleasePoolPush", &functions[3], true},
                                          {"objc_autoreleasePoolPop", &functions[4], true}};
    tb_library_error load = TB_LIBRARY_ERROR_INIT;
    tb_library_function create = NULL;
    tb_rhi_result result = TB_RHI_OK;
    if (tb_library_open("libobjc.A.dylib", TB_LIBRARY_SEARCH_SYSTEM, &d->objc, &load) !=
            TB_LIBRARY_OK ||
        tb_library_bind(d->objc, runtime, 5, &load) != TB_LIBRARY_OK ||
        tb_library_open("Metal.framework/Metal", TB_LIBRARY_SEARCH_SYSTEM, &d->metal, &load) !=
            TB_LIBRARY_OK ||
        tb_library_find_function(d->metal, "MTLCreateSystemDefaultDevice", &create, &load) !=
            TB_LIBRARY_OK)
    {
        result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Metal: %s", load.message);
        goto failed;
    }
    memcpy(&d->get_class, &functions[0], sizeof(d->get_class));
    memcpy(&d->selector, &functions[1], sizeof(d->selector));
    d->send = functions[2];
    memcpy(&d->pool_push, &functions[3], sizeof(d->pool_push));
    memcpy(&d->pool_pop, &functions[4], sizeof(d->pool_pop));
    void *const pool = d->pool_push();
    d->gpu = ((void *(*)(void))create)();
    d->queue = tb_metal_get(d, d->gpu, "newCommandQueue");
    if (d->queue == NULL)
    {
        result = tb_rhi_fail(error, d->gpu == NULL ? TB_RHI_UNAVAILABLE : TB_RHI_SYSTEM_ERROR,
                             d->gpu == NULL ? "Metal: no system default GPU"
                                            : "Metal: could not create the command queue");
        d->pool_pop(pool);
        goto failed;
    }
    /* On macOS 13 and later, Tier 2 argument buffers support direct GPU
     * addresses. Tier 1 devices bind draw-uniform indices to stage slots. */
    d->argument_buffers = ((unsigned long (*)(void *, void *))d->send)(
                              d->gpu, d->selector("argumentBuffersSupport")) == 1ul;
    if (info->bindless_resources > TB_MSL_RESOURCE_COUNT ||
        info->bindless_samplers > TB_MSL_SAMPLER_COUNT)
    {
        result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                             "Metal argument-buffer capacities are %u resources and %u samplers",
                             TB_MSL_RESOURCE_COUNT, TB_MSL_SAMPLER_COUNT);
        d->pool_pop(pool);
        goto failed;
    }
    if (!tb_metal_heaps_create(d))
    {
        result =
            tb_rhi_fail(error, TB_RHI_OUT_OF_MEMORY, "Metal could not allocate resource tables");
        d->pool_pop(pool);
        goto failed;
    }
    info->features = TB_RHI_FEATURE_DEVICE_RECREATE | TB_RHI_FEATURE_HEAP_INDICES |
                     (d->argument_buffers ? TB_RHI_FEATURE_BINDLESS : 0u);
    const char *const name = tb_metal_get(d, tb_metal_get(d, d->gpu, "name"), "UTF8String");
    info->max_texture_size = 8192; /* Conservative 2D limit across macOS Metal families. */

    (void)tb_fmt_buffer(info->adapter, sizeof(info->adapter), "%s", name);
    d->pool_pop(pool);
    *state = d;
    return TB_RHI_OK;
failed:
    tb_metal_device_destroy(d);
    return result;
}

uint64_t tb_metal_completed(void *state)
{
    tb_metal_device *const d = state;
    tb_mutex_lock(&d->lock);
    const uint64_t completed = d->completed;
    tb_mutex_unlock(&d->lock);
    return completed;
}

tb_rhi_result tb_metal_wait(void *state, uint64_t submission)
{
    tb_metal_device *const d = state;
    tb_mutex_lock(&d->lock);
    while (d->completed < submission && !d->lost)
    {
        tb_condition_wait(&d->finished, &d->lock);
    }
    const bool lost = d->lost;
    tb_mutex_unlock(&d->lock);
    return lost ? TB_RHI_DEVICE_LOST : TB_RHI_OK;
}

static tb_rhi_result metal_replacement(void *original, const tb_rhi_device_info *info,
                                       void **replacement, tb_rhi_error *error)
{
    tb_metal_device *const old = original;
    tb_rhi_device_info fresh_info = *info;
    void *state = NULL;
    const tb_rhi_result opened =
        tb_metal_device_create(&(tb_rhi_device_options){0}, &state, &fresh_info, error);
    if (opened != TB_RHI_OK)
    {
        return opened;
    }
    tb_metal_device *const fresh = state;
    /* Metal resources and the layer belong to this exact MTLDevice. */
    if (fresh->gpu != old->gpu)
    {
        tb_metal_device_destroy(fresh);
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "The original Metal adapter is no longer the default device");
    }
    fresh->completed = old->completed;
    *replacement = fresh;
    return TB_RHI_OK;
}

static tb_rhi_result metal_device_copy(void *state, const tb_rhi_device_copy *copies, size_t count,
                                       tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    void *const pool = d->pool_push();
    void *const commands = tb_metal_get(d, d->queue, "commandBuffer");
    void *const encoder = tb_metal_get(d, commands, "blitCommandEncoder");
    tb_rhi_result result = TB_RHI_OK;
    if (encoder == NULL)
    {
        result = tb_rhi_fail(error, TB_RHI_OUT_OF_MEMORY,
                             "Could not create a Metal resource copy encoder");
        goto done;
    }
    for (size_t i = 0; i < count; i++)
    {
        const tb_rhi_device_copy *const copy = &copies[i];
        if (copy->bytes)
        {
            TB_METAL_METHOD(d, void, void *, unsigned long, void *, unsigned long, unsigned long)(
                encoder,
                d->selector("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
                copy->source, 0ul, copy->destination, 0ul, copy->bytes);
        }
        else
        {
            TB_METAL_METHOD(d, void, void *, void *)(encoder,
                                                     d->selector("copyFromTexture:toTexture:"),
                                                     copy->source, copy->destination);
        }
    }
    tb_metal_call(d, encoder, "endEncoding");
    tb_metal_call(d, commands, "commit");
    tb_metal_call(d, commands, "waitUntilCompleted");
    if (((unsigned long (*)(void *, void *))d->send)(commands, d->selector("status")) != 4ul)
    {
        const char *const why = tb_metal_get(
            d, tb_metal_get(d, tb_metal_get(d, commands, "error"), "localizedDescription"),
            "UTF8String");
        result = tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Metal resource copy failed: %s",
                             why ? why : "no diagnostic");
    }
done:
    d->pool_pop(pool);
    return result;
}

const tb_rhi_backend_table tb_rhi_metal_backend = {
    .backend = TB_RHI_BACKEND_METAL,
    .device_create = tb_metal_device_create,
    .device_destroy = tb_metal_device_destroy,
    .device_replacement = metal_replacement,
    .device_copy = metal_device_copy,
    .buffer_create = tb_metal_buffer_create,
    .buffer_destroy = tb_metal_resource_destroy,
    .buffer_write = tb_metal_buffer_write,
    .buffer_read = tb_metal_buffer_read,
    .texture_create = tb_metal_texture_create,
    .texture_destroy = tb_metal_resource_destroy,
    .sampler_create = tb_metal_sampler_create,
    .sampler_destroy = tb_metal_sampler_destroy,
    .compute_pipeline_create = tb_metal_compute_pipeline_create,
    .cmd_index_buffer = tb_metal_cmd_index_buffer,
    .cmd_vertex_buffer = tb_metal_cmd_vertex_buffer,
    .cmd_draw_indexed = tb_metal_cmd_draw_indexed,
    .cmd_dispatch = tb_metal_cmd_dispatch,
    .shader_create = tb_metal_shader_create,
    .shader_destroy = tb_metal_release,
    .graphics_pipeline_create = tb_metal_graphics_pipeline_create,
    .pipeline_destroy = tb_metal_pipeline_destroy,
    .swap_chain_create = tb_metal_swap_chain_create,
    .swap_chain_destroy = tb_metal_swap_chain_destroy,
    .swap_chain_current = tb_metal_swap_chain_current,
    .swap_chain_present = tb_metal_swap_chain_present,
    .swap_chain_resize = tb_metal_swap_chain_resize,
    .list_begin = tb_metal_list_begin,
    .list_discard = tb_metal_list_discard,
    .submit = tb_metal_submit,
    .completed = tb_metal_completed,
    .wait = tb_metal_wait,
    .cmd_barriers = tb_metal_cmd_barriers,
    .cmd_begin_pass = tb_metal_cmd_begin_pass,
    .cmd_end_pass = tb_metal_cmd_end_pass,
    .cmd_viewport = tb_metal_cmd_viewport,
    .cmd_scissor = tb_metal_cmd_scissor,
    .cmd_pipeline = tb_metal_cmd_pipeline,
    .cmd_push_constants = tb_metal_cmd_push_constants,
    .cmd_constant_buffer = tb_metal_cmd_constant_buffer,
    .cmd_draw = tb_metal_cmd_draw,
    .cmd_copy_buffer = tb_metal_cmd_copy_buffer,
    .cmd_copy_texture_to_buffer = tb_metal_cmd_copy_texture_to_buffer,
    .cmd_copy_buffer_to_texture = tb_metal_cmd_copy_buffer_to_texture,
    .cmd_begin_marker = tb_metal_cmd_begin_marker,
    .cmd_end_marker = tb_metal_cmd_end_marker};
