/* The D3D11 backend's swap chains: DXGI's flip model on a window's HWND, as
 * the D3D12 backend's are, presenting with vsync, or without it and tearing
 * where the system allows. The window module owns fullscreen, so DXGI's
 * Alt+Enter is turned off.
 *
 * The images are shown unscaled, from the window's top-left corner, so a
 * window dragged larger never stretches the last frame; a system that
 * refuses unscaled images gets stretched ones.
 *
 * In D3D11, a flip-model swap chain's first buffer is always the one to
 * draw next: DXGI turns its buffers behind it. The RHI's images are still
 * as many as the swap chain has, so that the renderer cycles through them
 * as on D3D12; each is that first buffer, and the current one is the next
 * in turn after each present. */
#include "d3d11_internal.h"

#include "badger/memory.h"
#include "badger/window.h"

typedef struct tb_d3d11_chain
{
    IDXGISwapChain1 *chain;
    tb_d3d11_texture images[TB_RHI_SWAP_CHAIN_MAX_IMAGES];
    uint32_t count;
    uint32_t current;
    tb_rhi_format format;
    bool vsync;
    bool tearing; /* made able to tear */
} tb_d3d11_chain;

/* A flip-model swap chain's buffers are UNORM; sRGB is a view of them. */
static DXGI_FORMAT tb_d3d11_chain_format(tb_rhi_format format)
{
    if (format == TB_RHI_FORMAT_B8G8R8A8_SRGB)
    {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (format == TB_RHI_FORMAT_R8G8B8A8_SRGB)
    {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return tb_d3d11_format(format);
}

static void tb_d3d11_chain_release_images(tb_d3d11_chain *chain)
{
    for (uint32_t index = 0; index < chain->count; index++)
    {
        tb_d3d11_texture_release_views(&chain->images[index]);
        if (chain->images[index].texture != NULL)
        {
            (void)ID3D11Texture2D_Release(chain->images[index].texture);
            chain->images[index].texture = NULL;
        }
    }
}

static tb_rhi_result tb_d3d11_chain_images(tb_d3d11_chain *chain, tb_rhi_swap_chain_images *images,
                                           tb_rhi_error *error)
{
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    HRESULT result = IDXGISwapChain1_GetDesc1(chain->chain, &desc);

    for (uint32_t index = 0; SUCCEEDED(result) && index < chain->count; index++)
    {
        tb_d3d11_texture *const image = &chain->images[index];
        *image = (tb_d3d11_texture){0};
        image->target_format = tb_d3d11_format(chain->format);
        image->resource_format = desc.Format;
        image->swap_chain = true;
        image->desc.kind = TB_RHI_TEXTURE_2D;
        image->desc.format = chain->format;
        image->desc.width = desc.Width;
        image->desc.height = desc.Height;
        image->desc.layers = 1;
        image->desc.mips = 1;
        image->desc.samples = 1;
        image->desc.usage = TB_RHI_TEXTURE_RENDER_TARGET;
        image->indices = (tb_rhi_indices){TB_RHI_NO_INDEX, TB_RHI_NO_INDEX, {0}, 0};
        result = IDXGISwapChain1_GetBuffer(chain->chain, 0, &IID_ID3D11Texture2D,
                                           (void **)&image->texture);
        images->objects[index] = image;
    }
    if (FAILED(result))
    {
        tb_d3d11_chain_release_images(chain);
        return tb_d3d11_fail(error, result, "the swap chain's images");
    }
    chain->current = 0;
    images->count = chain->count;
    images->width = desc.Width;
    images->height = desc.Height;
    images->format = chain->format;
    return TB_RHI_OK;
}

tb_rhi_result tb_d3d11_swap_chain_create(void *state, struct tb_window *window,
                                         const tb_rhi_swap_chain_desc *desc, void **chain,
                                         tb_rhi_swap_chain_images *images, tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    tb_window_native native = {0};
    tb_window_state window_state = {0};
    DXGI_SWAP_CHAIN_DESC1 chain_desc = {0};
    tb_d3d11_chain *made = NULL;
    HRESULT result = S_OK;
    tb_rhi_result images_made = TB_RHI_OK;

    tb_window_native_of(window, &native);
    if (native.backend != TB_WINDOW_BACKEND_WIN32)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d11: a swap chain needs a Win32 window");
    }
    tb_window_state_of(window, &window_state);
    made = tb_calloc(1, sizeof(*made));
    made->count = desc->images;
    made->format = desc->format;
    made->vsync = desc->vsync;
    made->tearing = device->tearing;
    chain_desc.Width = window_state.pixel_width;
    chain_desc.Height = window_state.pixel_height;
    chain_desc.Format = tb_d3d11_chain_format(desc->format);
    chain_desc.SampleDesc.Count = 1;
    chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain_desc.BufferCount = desc->images;
    chain_desc.Scaling = DXGI_SCALING_NONE;
    chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    chain_desc.Flags = made->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    result = IDXGIFactory2_CreateSwapChainForHwnd(device->factory, (IUnknown *)device->device,
                                                  (HWND)native.window, &chain_desc, NULL, NULL,
                                                  &made->chain);
    if (result == DXGI_ERROR_INVALID_CALL || result == E_INVALIDARG)
    {
        /* coverage-off: Windows 10 and later show unscaled images */
        chain_desc.Scaling = DXGI_SCALING_STRETCH;
        result = IDXGIFactory2_CreateSwapChainForHwnd(device->factory, (IUnknown *)device->device,
                                                      (HWND)native.window, &chain_desc, NULL, NULL,
                                                      &made->chain);
        /* coverage-on */
    }
    if (FAILED(result))
    {
        tb_free(made);
        tb_d3d11_drain_messages(device);
        return tb_d3d11_fail(error, result, "a swap chain");
    }
    (void)IDXGIFactory2_MakeWindowAssociation(device->factory, (HWND)native.window,
                                              DXGI_MWA_NO_ALT_ENTER);
    images_made = tb_d3d11_chain_images(made, images, error);
    if (images_made != TB_RHI_OK)
    {
        (void)IDXGISwapChain1_Release(made->chain);
        tb_free(made);
        return images_made;
    }
    *chain = made;
    return TB_RHI_OK;
}

/* Let go of the images on the immediate context too, as DXGI needs before
 * it resizes or releases them. */
static void tb_d3d11_chain_unbind(tb_d3d11_device *device)
{
    tb_mutex_lock(&device->context_lock);
    ID3D11DeviceContext_OMSetRenderTargets(device->context, 0, NULL, NULL);
    ID3D11DeviceContext_ClearState(device->context);
    ID3D11DeviceContext_Flush(device->context);
    tb_mutex_unlock(&device->context_lock);
}

void tb_d3d11_swap_chain_destroy(void *state, void *chain)
{
    tb_d3d11_chain *const d3d11_chain = chain;

    tb_d3d11_chain_release_images(d3d11_chain);
    tb_d3d11_chain_unbind(state);
    (void)IDXGISwapChain1_Release(d3d11_chain->chain);
    tb_free(d3d11_chain);
}

uint32_t tb_d3d11_swap_chain_current(void *state, void *chain)
{
    const tb_d3d11_chain *const d3d11_chain = chain;

    (void)state;
    return d3d11_chain->current;
}

tb_rhi_result tb_d3d11_swap_chain_present(void *state, void *chain)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_chain *const d3d11_chain = chain;
    const bool tear = !d3d11_chain->vsync && d3d11_chain->tearing;
    HRESULT result = S_OK;

    tb_mutex_lock(&device->context_lock);
    result = IDXGISwapChain1_Present(d3d11_chain->chain, d3d11_chain->vsync ? 1u : 0u,
                                     tear ? DXGI_PRESENT_ALLOW_TEARING : 0u);
    tb_mutex_unlock(&device->context_lock);
    d3d11_chain->current = (d3d11_chain->current + 1) % d3d11_chain->count;
    tb_d3d11_drain_messages(device);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        return TB_RHI_DEVICE_LOST;
    }
    return TB_RHI_OK;
}

tb_rhi_result tb_d3d11_swap_chain_resize(void *state, void *chain, uint32_t width, uint32_t height,
                                         bool vsync, tb_rhi_swap_chain_images *images,
                                         tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_chain *const d3d11_chain = chain;
    HRESULT result = S_OK;

    tb_d3d11_chain_release_images(d3d11_chain);
    tb_d3d11_chain_unbind(device);
    d3d11_chain->vsync = vsync;
    result = IDXGISwapChain1_ResizeBuffers(
        d3d11_chain->chain, d3d11_chain->count, width, height, DXGI_FORMAT_UNKNOWN,
        d3d11_chain->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    tb_d3d11_drain_messages(device);
    if (FAILED(result))
    {
        return tb_d3d11_fail(error, result, "resizing a swap chain");
    }
    return tb_d3d11_chain_images(d3d11_chain, images, error);
}
