/* The D3D12 backend's swap chains: DXGI's flip model on a window's HWND,
 * presenting with vsync, or without it and tearing where the system allows,
 * as variable-refresh displays want. The window module owns fullscreen, so
 * DXGI's Alt+Enter is turned off. */
#include "d3d12_internal.h"

#include "badger/memory.h"
#include "badger/window.h"

typedef struct tb_d3d12_chain
{
    IDXGISwapChain3 *chain;
    tb_d3d12_texture images[TB_RHI_SWAP_CHAIN_MAX_IMAGES];
    uint32_t count;
    tb_rhi_format format;
    bool vsync;
    bool tearing; /* made able to tear */
} tb_d3d12_chain;

/* A flip-model swap chain's buffers are UNORM; sRGB is a view of them. */
static DXGI_FORMAT tb_d3d12_chain_format(tb_rhi_format format)
{
    switch (format)
    {
    case TB_RHI_FORMAT_B8G8R8A8_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TB_RHI_FORMAT_R8G8B8A8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return tb_d3d12_format(format);
    }
}

static void tb_d3d12_chain_release_images(tb_d3d12_chain *chain)
{
    for (uint32_t index = 0; index < chain->count; index++)
    {
        if (chain->images[index].resource != NULL)
        {
            (void)ID3D12Resource_Release(chain->images[index].resource);
            chain->images[index].resource = NULL;
        }
    }
}

static tb_rhi_result tb_d3d12_chain_images(tb_d3d12_chain *chain, tb_rhi_swap_chain_images *images,
                                           tb_rhi_error *error)
{
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    HRESULT result = IDXGISwapChain3_GetDesc1(chain->chain, &desc);

    for (uint32_t index = 0; SUCCEEDED(result) && index < chain->count; index++)
    {
        chain->images[index] =
            (tb_d3d12_texture){.target_format = tb_d3d12_format(chain->format), .swap_chain = true};
        result = IDXGISwapChain3_GetBuffer(chain->chain, index, &IID_ID3D12Resource,
                                           (void **)&chain->images[index].resource);
        images->objects[index] = &chain->images[index];
    }
    if (FAILED(result))
    {
        tb_d3d12_chain_release_images(chain);
        return tb_d3d12_fail(error, result, "the swap chain's images");
    }
    images->count = chain->count;
    images->width = desc.Width;
    images->height = desc.Height;
    images->format = chain->format;
    return TB_RHI_OK;
}

tb_rhi_result tb_d3d12_swap_chain_create(void *state, struct tb_window *window,
                                         const tb_rhi_swap_chain_desc *desc, void **chain,
                                         tb_rhi_swap_chain_images *images, tb_rhi_error *error)
{
    tb_d3d12_device *const device = state;
    tb_window_native native = {0};
    tb_window_state window_state = {0};
    DXGI_SWAP_CHAIN_DESC1 chain_desc = {0};
    IDXGISwapChain1 *chain1 = NULL;
    tb_d3d12_chain *made = NULL;
    HRESULT result = S_OK;
    tb_rhi_result images_made = TB_RHI_OK;

    tb_window_native_of(window, &native);
    if (native.backend != TB_WINDOW_BACKEND_WIN32)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d12: a swap chain needs a Win32 window");
    }
    tb_window_state_of(window, &window_state);
    made = tb_calloc(1, sizeof(*made));
    made->count = desc->images;
    made->format = desc->format;
    made->vsync = desc->vsync;
    made->tearing = device->tearing;
    chain_desc.Width = window_state.pixel_width;
    chain_desc.Height = window_state.pixel_height;
    chain_desc.Format = tb_d3d12_chain_format(desc->format);
    chain_desc.SampleDesc.Count = 1;
    chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain_desc.BufferCount = desc->images;
    chain_desc.Scaling = DXGI_SCALING_STRETCH;
    chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    chain_desc.Flags = made->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    result =
        IDXGIFactory6_CreateSwapChainForHwnd(device->factory, (IUnknown *)device->queue,
                                             (HWND)native.window, &chain_desc, NULL, NULL, &chain1);
    if (SUCCEEDED(result))
    {
        (void)IDXGIFactory6_MakeWindowAssociation(device->factory, (HWND)native.window,
                                                  DXGI_MWA_NO_ALT_ENTER);
        result =
            IDXGISwapChain1_QueryInterface(chain1, &IID_IDXGISwapChain3, (void **)&made->chain);
        (void)IDXGISwapChain1_Release(chain1);
    }
    if (FAILED(result))
    {
        tb_free(made);
        return tb_d3d12_fail(error, result, "a swap chain");
    }
    images_made = tb_d3d12_chain_images(made, images, error);
    if (images_made != TB_RHI_OK)
    {
        (void)IDXGISwapChain3_Release(made->chain);
        tb_free(made);
        return images_made;
    }
    *chain = made;
    return TB_RHI_OK;
}

void tb_d3d12_swap_chain_destroy(void *state, void *chain)
{
    tb_d3d12_chain *const d3d12_chain = chain;

    (void)tb_d3d12_flush(state);
    tb_d3d12_chain_release_images(d3d12_chain);
    (void)IDXGISwapChain3_Release(d3d12_chain->chain);
    tb_free(d3d12_chain);
}

uint32_t tb_d3d12_swap_chain_current(void *state, void *chain)
{
    tb_d3d12_chain *const d3d12_chain = chain;

    (void)state;
    return IDXGISwapChain3_GetCurrentBackBufferIndex(d3d12_chain->chain);
}

tb_rhi_result tb_d3d12_swap_chain_present(void *state, void *chain)
{
    tb_d3d12_device *const device = state;
    tb_d3d12_chain *const d3d12_chain = chain;
    const bool tear = !d3d12_chain->vsync && d3d12_chain->tearing;
    const HRESULT result = IDXGISwapChain3_Present(d3d12_chain->chain, d3d12_chain->vsync ? 1u : 0u,
                                                   tear ? DXGI_PRESENT_ALLOW_TEARING : 0u);

    tb_d3d12_drain_messages(device);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        return TB_RHI_DEVICE_LOST;
    }
    return TB_RHI_OK;
}

tb_rhi_result tb_d3d12_swap_chain_resize(void *state, void *chain, uint32_t width, uint32_t height,
                                         bool vsync, tb_rhi_swap_chain_images *images,
                                         tb_rhi_error *error)
{
    tb_d3d12_chain *const d3d12_chain = chain;
    HRESULT result = S_OK;

    /* The last presents may still read the images. */
    if (tb_d3d12_flush(state) != TB_RHI_OK)
    {
        return tb_rhi_fail(error, TB_RHI_DEVICE_LOST, "d3d12: the device was lost");
    }
    tb_d3d12_chain_release_images(d3d12_chain);
    tb_d3d12_forget_lists(state);
    d3d12_chain->vsync = vsync;
    result = IDXGISwapChain3_ResizeBuffers(
        d3d12_chain->chain, d3d12_chain->count, width, height, DXGI_FORMAT_UNKNOWN,
        d3d12_chain->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
    if (FAILED(result))
    {
        return tb_d3d12_fail(error, result, "resizing a swap chain");
    }
    return tb_d3d12_chain_images(d3d12_chain, images, error);
}
