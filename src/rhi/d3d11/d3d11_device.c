/* The D3D11 backend's device: d3d11.dll and dxgi.dll loaded, an adapter
 * chosen, the device and its immediate context, the constant buffers every
 * draw's constants go through, the tables of what bindless indices name;
 * what the device supports; the GPU's progress, by an event query after
 * each submission; and validation's messages, written to the log. */
#include "d3d11_internal.h"

#include "badger/fmt.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/utf8.h"

#include <d3d11sdklayers.h>

#include <string.h>

/* Validation's messages drained at once. */
#define TB_D3D11_MESSAGE_BYTES 2048u

/* D3D11's largest 2D texture at feature level 11_0. */
#define TB_D3D11_MAX_TEXTURE_SIZE 16384u

/* The shader model D3D11 runs, as tb_rhi_device_info counts them. */
#define TB_D3D11_SHADER_MODEL 50u

tb_rhi_result tb_d3d11_fail(tb_rhi_error *error, HRESULT result, const char *what)
{
    const tb_rhi_result kind =
        result == E_OUTOFMEMORY ? TB_RHI_OUT_OF_MEMORY
        : (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
            ? TB_RHI_DEVICE_LOST
            : TB_RHI_SYSTEM_ERROR;
    return tb_rhi_fail(error, kind, "d3d11: %s failed (0x%08lx)", what, (unsigned long)result);
}

DXGI_FORMAT tb_d3d11_format(tb_rhi_format format)
{
    static const DXGI_FORMAT formats[TB_RHI_FORMAT_COUNT] = {
        [TB_RHI_FORMAT_UNKNOWN] = DXGI_FORMAT_UNKNOWN,
        [TB_RHI_FORMAT_R8_UNORM] = DXGI_FORMAT_R8_UNORM,
        [TB_RHI_FORMAT_R8G8_UNORM] = DXGI_FORMAT_R8G8_UNORM,
        [TB_RHI_FORMAT_R8G8B8A8_UNORM] = DXGI_FORMAT_R8G8B8A8_UNORM,
        [TB_RHI_FORMAT_R8G8B8A8_SRGB] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        [TB_RHI_FORMAT_B8G8R8A8_UNORM] = DXGI_FORMAT_B8G8R8A8_UNORM,
        [TB_RHI_FORMAT_B8G8R8A8_SRGB] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        [TB_RHI_FORMAT_R10G10B10A2_UNORM] = DXGI_FORMAT_R10G10B10A2_UNORM,
        [TB_RHI_FORMAT_R11G11B10_FLOAT] = DXGI_FORMAT_R11G11B10_FLOAT,
        [TB_RHI_FORMAT_R16_FLOAT] = DXGI_FORMAT_R16_FLOAT,
        [TB_RHI_FORMAT_R16G16_FLOAT] = DXGI_FORMAT_R16G16_FLOAT,
        [TB_RHI_FORMAT_R16G16B16A16_FLOAT] = DXGI_FORMAT_R16G16B16A16_FLOAT,
        [TB_RHI_FORMAT_R16_UINT] = DXGI_FORMAT_R16_UINT,
        [TB_RHI_FORMAT_R32_UINT] = DXGI_FORMAT_R32_UINT,
        [TB_RHI_FORMAT_R32_FLOAT] = DXGI_FORMAT_R32_FLOAT,
        [TB_RHI_FORMAT_R32G32_FLOAT] = DXGI_FORMAT_R32G32_FLOAT,
        [TB_RHI_FORMAT_R32G32B32_FLOAT] = DXGI_FORMAT_R32G32B32_FLOAT,
        [TB_RHI_FORMAT_R32G32B32A32_FLOAT] = DXGI_FORMAT_R32G32B32A32_FLOAT,
        [TB_RHI_FORMAT_D32_FLOAT] = DXGI_FORMAT_D32_FLOAT,
        [TB_RHI_FORMAT_D24_UNORM_S8_UINT] = DXGI_FORMAT_D24_UNORM_S8_UINT};

    return (unsigned)format < TB_RHI_FORMAT_COUNT ? formats[format] : DXGI_FORMAT_UNKNOWN;
}

/* ---------------------------------------------------------------------------
 * Validation's messages, and the backend's own.
 * ------------------------------------------------------------------------- */

static tb_log_level tb_d3d11_level(D3D11_MESSAGE_SEVERITY severity)
{
    switch (severity)
    {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION:
    case D3D11_MESSAGE_SEVERITY_ERROR:
        return TB_LOG_ERROR;
    case D3D11_MESSAGE_SEVERITY_WARNING:
        return TB_LOG_WARNING;
    case D3D11_MESSAGE_SEVERITY_INFO:
    case D3D11_MESSAGE_SEVERITY_MESSAGE:
        break;
    }
    return TB_LOG_INFO;
}

void tb_d3d11_drain_messages(tb_d3d11_device *device)
{
    UINT64 count = 0;
    union {
        D3D11_MESSAGE message;
        uint8_t bytes[TB_D3D11_MESSAGE_BYTES];
    } storage;

    if (device->info_queue == NULL)
    {
        return;
    }
    tb_mutex_lock(&device->messages_lock);
    count = ID3D11InfoQueue_GetNumStoredMessages(device->info_queue);
    for (UINT64 at = 0; at < count; at++)
    {
        SIZE_T size = 0;
        if (FAILED(ID3D11InfoQueue_GetMessage(device->info_queue, at, NULL, &size)) ||
            size > sizeof(storage))
        {
            continue;
        }
        if (SUCCEEDED(
                ID3D11InfoQueue_GetMessage(device->info_queue, at, &storage.message, &size)) &&
            device->log != NULL)
        {
            (void)tb_log_printf(device->log, tb_d3d11_level(storage.message.Severity), "rhi",
                                "d3d11: %s", storage.message.pDescription);
        }
    }
    ID3D11InfoQueue_ClearStoredMessages(device->info_queue);
    tb_mutex_unlock(&device->messages_lock);
}

void tb_d3d11_warn(tb_d3d11_device *device, tb_d3d11_warning warning, const char *message)
{
    if ((device->warned & (uint64_t)warning) != 0)
    {
        return;
    }
    device->warned |= (uint64_t)warning;
    if (device->log != NULL)
    {
        (void)tb_log_printf(device->log, TB_LOG_WARNING, "rhi", "d3d11: %s", message);
    }
}

/* ---------------------------------------------------------------------------
 * Making the device.
 * ------------------------------------------------------------------------- */

static tb_rhi_result tb_d3d11_load(tb_d3d11_device *device, tb_rhi_error *error)
{
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    tb_library_binding d3d11[] = {
        {"D3D11CreateDevice", (tb_library_function *)&device->create_device, true}};
    tb_library_binding dxgi[] = {
        {"CreateDXGIFactory2", (tb_library_function *)&device->create_factory, true}};

    if (tb_library_open("d3d11.dll", TB_LIBRARY_SEARCH_SYSTEM, &device->d3d11, &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_bind(device->d3d11, d3d11, sizeof(d3d11) / sizeof(d3d11[0]), &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_open("dxgi.dll", TB_LIBRARY_SEARCH_SYSTEM, &device->dxgi, &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_bind(device->dxgi, dxgi, sizeof(dxgi) / sizeof(dxgi[0]), &library_error) !=
            TB_LIBRARY_OK)
    {
        /* coverage-off: d3d11.dll and dxgi.dll are part of every Windows 10 and 11 */
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d11: %s", library_error.message);
        /* coverage-on */
    }
    return TB_RHI_OK;
}

/* Make a device on an adapter, with the debug layer when asked for and
 * installed; without it, saying so, when it is not. */
static HRESULT tb_d3d11_make(tb_d3d11_device *device, IDXGIAdapter1 *adapter, bool validation)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT count = sizeof(levels) / sizeof(levels[0]);
    HRESULT result = E_FAIL;

    if (validation)
    {
        result = device->create_device((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                       D3D11_CREATE_DEVICE_DEBUG, levels, count, D3D11_SDK_VERSION,
                                       &device->device, NULL, &device->context);
        if (SUCCEEDED(result))
        {
            return result;
        }
        /* coverage-off: the machines the tests run on have the debug layer */
        if (device->log != NULL)
        {
            (void)tb_log_printf(device->log, TB_LOG_WARNING, "rhi",
                                "d3d11: the debug layer is not installed (0x%08lx); validating "
                                "without it. Install Windows's Graphics Tools to have it",
                                (unsigned long)result);
        }
        /* coverage-on */
    }
    return device->create_device((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, levels,
                                 count, D3D11_SDK_VERSION, &device->device, NULL, &device->context);
}

/* The adapter a preference names: the system's GPU on the CPU (WARP), or
 * the first hardware adapter, by power where DXGI orders them so, that
 * makes a device of feature level 11_0. */
static tb_rhi_result tb_d3d11_open_adapter(tb_d3d11_device *device,
                                           const tb_rhi_device_options *options,
                                           tb_rhi_error *error)
{
    IDXGIFactory4 *factory4 = NULL;
    IDXGIFactory6 *factory6 = NULL;
    HRESULT result = S_OK;

    if (options->adapter == TB_RHI_ADAPTER_SOFTWARE)
    {
        result =
            IDXGIFactory2_QueryInterface(device->factory, &IID_IDXGIFactory4, (void **)&factory4);
        if (SUCCEEDED(result))
        {
            result = IDXGIFactory4_EnumWarpAdapter(factory4, &IID_IDXGIAdapter1,
                                                   (void **)&device->adapter);
            IDXGIFactory4_Release(factory4);
        }
        if (SUCCEEDED(result))
        {
            result = tb_d3d11_make(device, device->adapter, options->validation);
        }
        return SUCCEEDED(result)
                   ? TB_RHI_OK
                   : tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d11: WARP makes no device (0x%08lx)",
                                 (unsigned long)result);
    }
    (void)IDXGIFactory2_QueryInterface(device->factory, &IID_IDXGIFactory6, (void **)&factory6);
    for (UINT index = 0;; index++)
    {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc = {0};
        if (factory6 != NULL)
        {
            const DXGI_GPU_PREFERENCE order = options->adapter == TB_RHI_ADAPTER_LOW_POWER
                                                  ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                  : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            result = IDXGIFactory6_EnumAdapterByGpuPreference(
                factory6, index, order, &IID_IDXGIAdapter1, (void **)&adapter);
        }
        else
        {
            /* coverage-off: DXGI 1.6 is part of Windows 10 1803 and later */
            result = IDXGIFactory2_EnumAdapters1(device->factory, index, &adapter);
            /* coverage-on */
        }
        if (FAILED(result))
        {
            break;
        }
        (void)IDXGIAdapter1_GetDesc1(adapter, &desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(tb_d3d11_make(device, adapter, options->validation)))
        {
            device->adapter = adapter;
            break;
        }
        IDXGIAdapter1_Release(adapter);
    }
    if (factory6 != NULL)
    {
        IDXGIFactory6_Release(factory6);
    }
    if (device->adapter == NULL)
    {
        /* coverage-off: the machines the tests run on have a GPU of feature level 11_0 */
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "d3d11: no GPU makes a device of feature level 11_0; ask for the "
                           "software adapter");
        /* coverage-on */
    }
    return TB_RHI_OK;
}

static void tb_d3d11_describe(tb_d3d11_device *device, tb_rhi_device_info *info)
{
    DXGI_ADAPTER_DESC1 desc = {0};
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = {0};
    size_t written = 0;
    size_t length = 0;

    (void)IDXGIAdapter1_GetDesc1(device->adapter, &desc);
    while (length < sizeof(desc.Description) / sizeof(desc.Description[0]) &&
           desc.Description[length] != 0)
    {
        length++;
    }
    (void)tb_utf8_from_utf16((const uint16_t *)desc.Description, length, info->adapter,
                             sizeof(info->adapter) - 1, &written);
    info->adapter[written] = '\0';
    info->vendor = desc.VendorId;
    info->software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    info->video_memory = desc.DedicatedVideoMemory;
    info->shader_model = TB_D3D11_SHADER_MODEL;
    info->max_texture_size = TB_D3D11_MAX_TEXTURE_SIZE;
    /* Bindless resources are emulated for indices the same for a whole
     * draw: what HEAP_INDICES says, and not BINDLESS. D3D11 has no ray
     * tracing, mesh shaders or explicit barriers; its timestamps are not the
     * RHI's yet. */
    info->features = TB_RHI_FEATURE_HEAP_INDICES;
    info->features |= device->tearing ? TB_RHI_FEATURE_TEARING : 0u;
    if (SUCCEEDED(ID3D11Device_CheckFeatureSupport(device->device, D3D11_FEATURE_D3D11_OPTIONS2,
                                                   &options2, sizeof(options2))))
    {
        device->typed_loads = options2.TypedUAVLoadAdditionalFormats != FALSE;
    }
}

static tb_rhi_result tb_d3d11_constant_buffers(tb_d3d11_device *device, tb_rhi_error *error)
{
    D3D11_BUFFER_DESC desc = {0};
    HRESULT result = S_OK;

    desc.ByteWidth = TB_D3D11_CONSTANT_BUFFER_BYTES;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (uint32_t slot = 0; slot < TB_D3D11_CONSTANT_BUFFERS && SUCCEEDED(result); slot++)
    {
        result = ID3D11Device_CreateBuffer(device->device, &desc, NULL, &device->constants[slot]);
    }
    return SUCCEEDED(result) ? TB_RHI_OK : tb_d3d11_fail(error, result, "the constant buffers");
}

static void tb_d3d11_device_destroy(void *state);

static tb_rhi_result tb_d3d11_device_create(const tb_rhi_device_options *options, void **state,
                                            tb_rhi_device_info *info, tb_rhi_error *error)
{
    tb_d3d11_device *device = tb_calloc(1, sizeof(*device));
    IDXGIFactory5 *factory5 = NULL;
    BOOL tearing = FALSE;
    HRESULT result = S_OK;
    tb_rhi_result made = TB_RHI_OK;

    device->log = options->log;
    made = tb_d3d11_load(device, error);
    if (made != TB_RHI_OK)
    {
        goto failed;
    }
    result = device->create_factory(options->validation ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                    &IID_IDXGIFactory2, (void **)&device->factory);
    if (FAILED(result) && options->validation)
    {
        /* coverage-off: the machines the tests run on have DXGI's debug layer */
        result = device->create_factory(0, &IID_IDXGIFactory2, (void **)&device->factory);
        /* coverage-on */
    }
    if (FAILED(result))
    {
        /* coverage-off: DXGI 1.2 is part of every Windows the engine runs on */
        made = tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d11: DXGI 1.2 is not here (0x%08lx)",
                           (unsigned long)result);
        goto failed;
        /* coverage-on */
    }
    if (SUCCEEDED(
            IDXGIFactory2_QueryInterface(device->factory, &IID_IDXGIFactory5, (void **)&factory5)))
    {
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(
                factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
        {
            device->tearing = tearing != FALSE;
        }
        IDXGIFactory5_Release(factory5);
    }
    made = tb_d3d11_open_adapter(device, options, error);
    if (made != TB_RHI_OK)
    {
        goto failed;
    }
    if (options->validation)
    {
        (void)ID3D11Device_QueryInterface(device->device, &IID_ID3D11InfoQueue,
                                          (void **)&device->info_queue);
    }
    (void)ID3D11DeviceContext_QueryInterface(device->context, &IID_ID3DUserDefinedAnnotation,
                                             (void **)&device->annotation);
    tb_d3d11_describe(device, info);
    made = tb_d3d11_constant_buffers(device, error);
    if (made != TB_RHI_OK)
    {
        goto failed;
    }
    device->view_count = info->bindless_resources;
    device->views = tb_calloc(device->view_count, sizeof(*device->views));
    device->sampler_count = info->bindless_samplers;
    device->samplers = tb_calloc(device->sampler_count, sizeof(*device->samplers));
    tb_d3d11_drain_messages(device);
    *state = device;
    return TB_RHI_OK;

failed:
    tb_d3d11_device_destroy(device);
    return made;
}

#define TB_D3D11_RELEASE(object, release)                                                          \
    if ((object) != NULL)                                                                          \
    {                                                                                              \
        (void)release(object);                                                                     \
    }

static void tb_d3d11_device_destroy(void *state)
{
    tb_d3d11_device *const device = state;

    tb_d3d11_release_lists(device);
    for (size_t index = 0; index < device->pending_count; index++)
    {
        TB_D3D11_RELEASE(device->pending[index].query, ID3D11Query_Release)
    }
    for (size_t index = 0; index < device->readback_count; index++)
    {
        TB_D3D11_RELEASE(device->readbacks[index].staging, ID3D11Resource_Release)
    }
    tb_free(device->pending);
    tb_free(device->readbacks);
    tb_free(device->uploads);
    tb_free(device->views);
    tb_free(device->samplers);
    for (uint32_t slot = 0; slot < TB_D3D11_CONSTANT_BUFFERS; slot++)
    {
        TB_D3D11_RELEASE(device->constants[slot], ID3D11Buffer_Release)
    }
    if (device->context != NULL)
    {
        ID3D11DeviceContext_ClearState(device->context);
        ID3D11DeviceContext_Flush(device->context);
    }
    tb_d3d11_drain_messages(device);
    TB_D3D11_RELEASE(device->annotation, ID3DUserDefinedAnnotation_Release)
    TB_D3D11_RELEASE(device->info_queue, ID3D11InfoQueue_Release)
    TB_D3D11_RELEASE(device->context, ID3D11DeviceContext_Release)
    TB_D3D11_RELEASE(device->device, ID3D11Device_Release)
    TB_D3D11_RELEASE(device->adapter, IDXGIAdapter1_Release)
    TB_D3D11_RELEASE(device->factory, IDXGIFactory2_Release)
    tb_library_close(device->dxgi);
    tb_library_close(device->d3d11);
    tb_free(device);
}

/* ---------------------------------------------------------------------------
 * The GPU's progress.
 * ------------------------------------------------------------------------- */

uint64_t tb_d3d11_poll(tb_d3d11_device *device)
{
    size_t done = 0;

    while (done < device->pending_count)
    {
        tb_d3d11_pending *const pending = &device->pending[done];
        const HRESULT result =
            ID3D11DeviceContext_GetData(device->context, (ID3D11Asynchronous *)pending->query, NULL,
                                        0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result != S_OK)
        {
            break;
        }
        device->completed = pending->submission;
        (void)ID3D11Query_Release(pending->query);
        done++;
    }
    if (done > 0)
    {
        memmove(device->pending, device->pending + done,
                (device->pending_count - done) * sizeof(*device->pending));
        device->pending_count -= done;
    }
    return device->completed;
}

static uint64_t tb_d3d11_completed(void *state)
{
    tb_d3d11_device *const device = state;
    uint64_t completed = 0;

    tb_mutex_lock(&device->context_lock);
    completed = tb_d3d11_poll(device);
    tb_mutex_unlock(&device->context_lock);
    return completed;
}

static tb_rhi_result tb_d3d11_wait(void *state, uint64_t submission)
{
    tb_d3d11_device *const device = state;
    bool done = false;

    tb_mutex_lock(&device->context_lock);
    ID3D11DeviceContext_Flush(device->context);
    done = tb_d3d11_poll(device) >= submission;
    tb_mutex_unlock(&device->context_lock);
    while (!done)
    {
        if (FAILED(ID3D11Device_GetDeviceRemovedReason(device->device)))
        {
            break;
        }
        tb_thread_yield();
        tb_mutex_lock(&device->context_lock);
        done = tb_d3d11_poll(device) >= submission;
        tb_mutex_unlock(&device->context_lock);
    }
    tb_d3d11_drain_messages(device);
    if (FAILED(ID3D11Device_GetDeviceRemovedReason(device->device)))
    {
        return TB_RHI_DEVICE_LOST;
    }
    return TB_RHI_OK;
}

const tb_rhi_backend_table tb_rhi_d3d11_backend = {
    .backend = TB_RHI_BACKEND_D3D11,
    .device_create = tb_d3d11_device_create,
    .device_destroy = tb_d3d11_device_destroy,
    .buffer_create = tb_d3d11_buffer_create,
    .buffer_destroy = tb_d3d11_buffer_destroy,
    .buffer_write = tb_d3d11_buffer_write,
    .buffer_read = tb_d3d11_buffer_read,
    .texture_create = tb_d3d11_texture_create,
    .texture_destroy = tb_d3d11_texture_destroy,
    .sampler_create = tb_d3d11_sampler_create,
    .sampler_destroy = tb_d3d11_sampler_destroy,
    .shader_create = tb_d3d11_shader_create,
    .shader_destroy = tb_d3d11_shader_destroy,
    .graphics_pipeline_create = tb_d3d11_graphics_pipeline_create,
    .compute_pipeline_create = tb_d3d11_compute_pipeline_create,
    .pipeline_destroy = tb_d3d11_pipeline_destroy,
    .swap_chain_create = tb_d3d11_swap_chain_create,
    .swap_chain_destroy = tb_d3d11_swap_chain_destroy,
    .swap_chain_current = tb_d3d11_swap_chain_current,
    .swap_chain_present = tb_d3d11_swap_chain_present,
    .swap_chain_resize = tb_d3d11_swap_chain_resize,
    .list_begin = tb_d3d11_list_begin,
    .list_discard = tb_d3d11_list_discard,
    .submit = tb_d3d11_submit,
    .completed = tb_d3d11_completed,
    .wait = tb_d3d11_wait,
    .cmd_barriers = tb_d3d11_cmd_barriers,
    .cmd_begin_pass = tb_d3d11_cmd_begin_pass,
    .cmd_end_pass = tb_d3d11_cmd_end_pass,
    .cmd_viewport = tb_d3d11_cmd_viewport,
    .cmd_scissor = tb_d3d11_cmd_scissor,
    .cmd_pipeline = tb_d3d11_cmd_pipeline,
    .cmd_push_constants = tb_d3d11_cmd_push_constants,
    .cmd_constant_buffer = tb_d3d11_cmd_constant_buffer,
    .cmd_vertex_buffer = tb_d3d11_cmd_vertex_buffer,
    .cmd_index_buffer = tb_d3d11_cmd_index_buffer,
    .cmd_draw = tb_d3d11_cmd_draw,
    .cmd_draw_indexed = tb_d3d11_cmd_draw_indexed,
    .cmd_dispatch = tb_d3d11_cmd_dispatch,
    .cmd_copy_buffer = tb_d3d11_cmd_copy_buffer,
    .cmd_copy_buffer_to_texture = tb_d3d11_cmd_copy_buffer_to_texture,
    .cmd_copy_texture_to_buffer = tb_d3d11_cmd_copy_texture_to_buffer,
    .cmd_begin_marker = tb_d3d11_cmd_begin_marker,
    .cmd_end_marker = tb_d3d11_cmd_end_marker,
};
