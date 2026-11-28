#include "badger/memory.h"
#include "badger/window.h"
#include "metal_internal.h"

/* CAMetalLayer acquires a drawable only when the compositor can display it.
 * Stable RHI image handles name private render targets. Present acquires the
 * drawable late and copies the completed image, so an occluded window cannot
 * block command recording or invalidate a handle that a caller is using. */
typedef struct metal_chain
{
    tb_window *window;
    void *view;
    void *layer;
    void *previous_layer;
    bool wanted_layer;
    tb_library *quartz;
    tb_rhi_swap_chain_images ring;
    uint32_t current;
} metal_chain;

typedef struct metal_size
{
    double width, height;
} metal_size;

static void set_bool(tb_metal_device *d, void *receiver, const char *selector, bool value)
{
    TB_METAL_METHOD(d, void, bool)(receiver, d->selector(selector), value);
}

static void release_images(tb_metal_device *d, tb_rhi_swap_chain_images *images)
{
    for (uint32_t i = 0; i < images->count; i++)
    {
        tb_metal_release(d, images->objects[i]);
        images->objects[i] = NULL;
    }
}

tb_rhi_result tb_metal_swap_chain_resize(void *state, void *object, uint32_t width, uint32_t height,
                                         bool vsync, tb_rhi_swap_chain_images *images,
                                         tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    metal_chain *const chain = object;
    tb_window_state window = {0};
    tb_window_state_of(chain->window, &window);
    tb_rhi_swap_chain_images next = {.count = chain->ring.count,
                                     .format = chain->ring.format,
                                     .width = width == 0 ? window.pixel_width : width,
                                     .height = height == 0 ? window.pixel_height : height};
    if (next.width == 0 || next.height == 0)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Metal window has an empty drawable area");
    }
    const tb_rhi_texture_desc desc = {.format = next.format,
                                      .width = next.width,
                                      .height = next.height,
                                      .mips = 1,
                                      .layers = 1,
                                      .samples = 1,
                                      .usage = TB_RHI_TEXTURE_RENDER_TARGET};
    for (uint32_t i = 0; i < next.count; i++)
    {
        const tb_rhi_result result = tb_metal_texture_create(
            d, &desc, (tb_rhi_indices){.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX},
            &next.objects[i], error);
        if (result != TB_RHI_OK)
        {
            release_images(d, &next);
            return result;
        }
    }
    release_images(d, &chain->ring);
    chain->ring = next;
    chain->current = 0;
    const metal_size size = {next.width, next.height};
    TB_METAL_METHOD(d, void, double)(chain->layer, d->selector("setContentsScale:"),
                                     window.content_scale);
    TB_METAL_METHOD(d, void, metal_size)(chain->layer, d->selector("setDrawableSize:"), size);
    set_bool(d, chain->layer, "setDisplaySyncEnabled:", vsync);
    *images = next;
    return TB_RHI_OK;
}

void tb_metal_swap_chain_destroy(void *state, void *object)
{
    tb_metal_device *const d = state;
    metal_chain *const chain = object;
    void *const pool = d->pool_push();
    if (chain->layer && tb_metal_get(d, chain->view, "layer") == chain->layer)
    {
        tb_metal_set(d, chain->view, "setLayer:", chain->previous_layer);
        set_bool(d, chain->view, "setWantsLayer:", chain->wanted_layer);
    }
    release_images(d, &chain->ring);
    tb_metal_release(d, chain->previous_layer);
    tb_metal_release(d, chain->layer);
    d->pool_pop(pool);
    tb_library_close(chain->quartz);
    tb_free(chain);
}

tb_rhi_result tb_metal_swap_chain_create(void *state, tb_window *window,
                                         const tb_rhi_swap_chain_desc *desc, void **object,
                                         tb_rhi_swap_chain_images *images, tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    tb_window_native native = {0};
    tb_window_native_of(window, &native);
    if (native.backend != TB_WINDOW_BACKEND_COCOA ||
        (desc->format != TB_RHI_FORMAT_B8G8R8A8_UNORM &&
         desc->format != TB_RHI_FORMAT_B8G8R8A8_SRGB))
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal presentation requires a Cocoa view and a BGRA8 target");
    }
    metal_chain *const chain = tb_calloc(1, sizeof(*chain));
    void *const pool = d->pool_push();
    tb_library_error load = TB_LIBRARY_ERROR_INIT;
    tb_rhi_result result = TB_RHI_OK;
    if (tb_library_open("QuartzCore.framework/QuartzCore", TB_LIBRARY_SEARCH_SYSTEM, &chain->quartz,
                        &load) != TB_LIBRARY_OK)
    {
        result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Metal presentation: %s", load.message);
        goto failed;
    }
    chain->window = window;
    chain->view = native.system;
    chain->ring.count = desc->images;
    chain->ring.format = desc->format;
    chain->layer = tb_metal_get(d, d->get_class("CAMetalLayer"), "new");
    if (chain->layer == NULL)
    {
        result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Metal presentation requires CAMetalLayer");
        goto failed;
    }
    chain->wanted_layer =
        ((bool (*)(void *, void *))d->send)(chain->view, d->selector("wantsLayer"));
    chain->previous_layer = tb_metal_get(d, tb_metal_get(d, chain->view, "layer"), "retain");
    tb_metal_set(d, chain->layer, "setDevice:", d->gpu);
    tb_metal_number(d, chain->layer,
                    "setPixelFormat:", desc->format == TB_RHI_FORMAT_B8G8R8A8_UNORM ? 80ul : 81ul);
    set_bool(d, chain->layer, "setFramebufferOnly:", false);
    tb_metal_set(d, chain->view, "setLayer:", chain->layer);
    set_bool(d, chain->view, "setWantsLayer:", true);
    result = tb_metal_swap_chain_resize(d, chain, 0, 0, desc->vsync, images, error);
    if (result != TB_RHI_OK)
    {
        goto failed;
    }
    *object = chain;
    d->pool_pop(pool);
    return TB_RHI_OK;
failed:
    tb_metal_swap_chain_destroy(d, chain);
    d->pool_pop(pool);
    return result;
}

uint32_t tb_metal_swap_chain_current(void *state, void *object)
{
    (void)state;
    const metal_chain *const chain = object;
    return chain->current;
}

tb_rhi_result tb_metal_swap_chain_present(void *state, void *object)
{
    tb_metal_device *const d = state;
    metal_chain *const chain = object;
    void *const pool = d->pool_push();
    void *const drawable = tb_metal_get(d, chain->layer, "nextDrawable");
    tb_rhi_result result = TB_RHI_OK;
    if (drawable != NULL)
    {
        void *const commands = tb_metal_get(d, d->queue, "commandBuffer");
        void *const encoder = tb_metal_get(d, commands, "blitCommandEncoder");
        void *const texture = tb_metal_get(d, drawable, "texture");
        TB_METAL_METHOD(d, void, void *, void *)(encoder, d->selector("copyFromTexture:toTexture:"),
                                                 chain->ring.objects[chain->current], texture);
        tb_metal_call(d, encoder, "endEncoding");
        tb_metal_set(d, commands, "presentDrawable:", drawable);
        tb_metal_call(d, commands, "commit");
        tb_metal_call(d, commands, "waitUntilCompleted");
        const unsigned long status =
            ((unsigned long (*)(void *, void *))d->send)(commands, d->selector("status"));
        result = status == 4 ? TB_RHI_OK : TB_RHI_DEVICE_LOST;
    }
    chain->current = (chain->current + 1) % chain->ring.count;
    d->pool_pop(pool);
    return result;
}
