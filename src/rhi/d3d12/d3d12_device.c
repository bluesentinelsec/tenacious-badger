/* The D3D12 backend's device: d3d12.dll and dxgi.dll loaded, an adapter
 * chosen, the device, its queue and fence, the bindless heaps, and the root
 * signature every pipeline shares; what the device supports; submissions;
 * and validation's messages, written to the log. */
#include "d3d12_internal.h"

#include "badger/fmt.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/utf8.h"

#include <d3d12sdklayers.h>

/* Validation's messages drained at once. */
#define TB_D3D12_MESSAGE_BYTES 2048u

tb_rhi_result tb_d3d12_fail(tb_rhi_error *error, HRESULT result, const char *what)
{
    const tb_rhi_result kind =
        result == E_OUTOFMEMORY ? TB_RHI_OUT_OF_MEMORY
        : (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
            ? TB_RHI_DEVICE_LOST
            : TB_RHI_SYSTEM_ERROR;
    return tb_rhi_fail(error, kind, "d3d12: %s failed (0x%08lx)", what, (unsigned long)result);
}

DXGI_FORMAT tb_d3d12_format(tb_rhi_format format)
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

D3D12_CPU_DESCRIPTOR_HANDLE tb_d3d12_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment,
                                                uint32_t index)
{
    start.ptr += (SIZE_T)increment * index;
    return start;
}

/* ---------------------------------------------------------------------------
 * Validation's messages.
 * ------------------------------------------------------------------------- */

static tb_log_level tb_d3d12_level(D3D12_MESSAGE_SEVERITY severity)
{
    switch (severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return TB_LOG_ERROR;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return TB_LOG_WARNING;
    case D3D12_MESSAGE_SEVERITY_INFO:
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        break;
    }
    return TB_LOG_INFO;
}

void tb_d3d12_drain_messages(tb_d3d12_device *device)
{
    UINT64 count = 0;
    UINT64 at = 0;
    union {
        D3D12_MESSAGE message;
        uint8_t bytes[TB_D3D12_MESSAGE_BYTES];
    } storage;

    if (device->info_queue == NULL)
    {
        return;
    }
    tb_mutex_lock(&device->messages_lock);
    count = ID3D12InfoQueue_GetNumStoredMessages(device->info_queue);
    for (at = 0; at < count; at++)
    {
        SIZE_T size = 0;
        if (FAILED(ID3D12InfoQueue_GetMessage(device->info_queue, at, NULL, &size)) ||
            size > sizeof(storage))
        {
            continue;
        }
        if (SUCCEEDED(
                ID3D12InfoQueue_GetMessage(device->info_queue, at, &storage.message, &size)) &&
            device->log != NULL)
        {
            (void)tb_log_printf(device->log, tb_d3d12_level(storage.message.Severity), "rhi",
                                "d3d12: %s", storage.message.pDescription);
        }
    }
    ID3D12InfoQueue_ClearStoredMessages(device->info_queue);
    tb_mutex_unlock(&device->messages_lock);
}

/* ---------------------------------------------------------------------------
 * Making the device.
 * ------------------------------------------------------------------------- */

static tb_rhi_result tb_d3d12_load(tb_d3d12_device *device, tb_rhi_error *error)
{
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    tb_library_binding d3d12[] = {
        {"D3D12CreateDevice", (tb_library_function *)&device->create_device, true},
        {"D3D12GetDebugInterface", (tb_library_function *)&device->get_debug_interface, true},
        {"D3D12SerializeVersionedRootSignature",
         (tb_library_function *)&device->serialize_root_signature, true}};
    tb_library_binding dxgi[] = {
        {"CreateDXGIFactory2", (tb_library_function *)&device->create_factory, true}};

    if (tb_library_open("d3d12.dll", TB_LIBRARY_SEARCH_SYSTEM, &device->d3d12, &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_bind(device->d3d12, d3d12, sizeof(d3d12) / sizeof(d3d12[0]), &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_open("dxgi.dll", TB_LIBRARY_SEARCH_SYSTEM, &device->dxgi, &library_error) !=
            TB_LIBRARY_OK ||
        tb_library_bind(device->dxgi, dxgi, sizeof(dxgi) / sizeof(dxgi[0]), &library_error) !=
            TB_LIBRARY_OK)
    {
        /* coverage-off: d3d12.dll and dxgi.dll are part of every Windows 10 and 11 */
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d12: %s", library_error.message);
        /* coverage-on */
    }
    return TB_RHI_OK;
}

/* The adapter a preference names: the system's GPU on the CPU (WARP), or
 * the first hardware adapter, by power, that makes a device. */
static tb_rhi_result tb_d3d12_open_adapter(tb_d3d12_device *device,
                                           tb_rhi_adapter_preference preference,
                                           tb_rhi_error *error)
{
    const DXGI_GPU_PREFERENCE order = preference == TB_RHI_ADAPTER_LOW_POWER
                                          ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                          : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
    HRESULT result = S_OK;

    if (preference == TB_RHI_ADAPTER_SOFTWARE)
    {
        result = IDXGIFactory6_EnumWarpAdapter(device->factory, &IID_IDXGIAdapter1,
                                               (void **)&device->adapter);
        if (SUCCEEDED(result))
        {
            result = device->create_device((IUnknown *)device->adapter, D3D_FEATURE_LEVEL_12_0,
                                           &IID_ID3D12Device, (void **)&device->device);
        }
        return SUCCEEDED(result)
                   ? TB_RHI_OK
                   : tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d12: WARP makes no device (0x%08lx)",
                                 (unsigned long)result);
    }
    for (UINT index = 0;; index++)
    {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc = {0};
        if (FAILED(IDXGIFactory6_EnumAdapterByGpuPreference(device->factory, index, order,
                                                            &IID_IDXGIAdapter1, (void **)&adapter)))
        {
            break;
        }
        (void)IDXGIAdapter1_GetDesc1(adapter, &desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(device->create_device((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0,
                                            &IID_ID3D12Device, (void **)&device->device)))
        {
            device->adapter = adapter;
            return TB_RHI_OK;
        }
        IDXGIAdapter1_Release(adapter);
    }
    return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                       "d3d12: no GPU makes a device of feature level 12_0; run with "
                       "--renderer=d3d11, or ask for the software adapter");
}

/* The highest shader model the device runs, as 66 for 6.6. */
static uint32_t tb_d3d12_shader_model(ID3D12Device *device)
{
    static const D3D_SHADER_MODEL models[] = {
        D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3,
        D3D_SHADER_MODEL_6_2, D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0};

    for (size_t index = 0; index < sizeof(models) / sizeof(models[0]); index++)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL data = {models[index]};
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &data,
                                                       sizeof(data))))
        {
            return (uint32_t)(data.HighestShaderModel >> 4) * 10u +
                   (uint32_t)(data.HighestShaderModel & 0xF);
        }
    }
    return 0;
}

static void tb_d3d12_describe(tb_d3d12_device *device, tb_rhi_device_info *info)
{
    DXGI_ADAPTER_DESC1 desc = {0};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {0};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {0};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {0};
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12 = {0};
    UINT64 frequency = 0;
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
    info->shader_model = tb_d3d12_shader_model(device->device);
    info->max_texture_size = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;

    (void)ID3D12Device_CheckFeatureSupport(device->device, D3D12_FEATURE_D3D12_OPTIONS, &options,
                                           sizeof(options));
    (void)ID3D12Device_CheckFeatureSupport(device->device, D3D12_FEATURE_D3D12_OPTIONS5, &options5,
                                           sizeof(options5));
    (void)ID3D12Device_CheckFeatureSupport(device->device, D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                           sizeof(options7));
    (void)ID3D12Device_CheckFeatureSupport(device->device, D3D12_FEATURE_D3D12_OPTIONS12,
                                           &options12, sizeof(options12));
    device->bindless =
        info->shader_model >= 66 && options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
    info->features |= device->bindless ? TB_RHI_FEATURE_BINDLESS : 0u;
    info->features |=
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 ? TB_RHI_FEATURE_RAY_QUERY : 0u;
    info->features |=
        options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0 ? TB_RHI_FEATURE_RAY_PIPELINE : 0u;
    info->features |=
        options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 ? TB_RHI_FEATURE_MESH_SHADER : 0u;
    info->features |= options12.EnhancedBarriersSupported ? TB_RHI_FEATURE_ENHANCED_BARRIERS : 0u;
    info->features |= device->tearing ? TB_RHI_FEATURE_TEARING : 0u;
    if (SUCCEEDED(ID3D12CommandQueue_GetTimestampFrequency(device->queue, &frequency)))
    {
        info->features |= TB_RHI_FEATURE_TIMESTAMPS;
        info->timestamp_frequency = frequency;
    }
}

static tb_rhi_result tb_d3d12_heap(tb_d3d12_device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                   UINT count, bool visible, ID3D12DescriptorHeap **heap,
                                   tb_rhi_error *error)
{
    const D3D12_DESCRIPTOR_HEAP_DESC desc = {
        type, count,
        visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    const HRESULT result = ID3D12Device_CreateDescriptorHeap(
        device->device, &desc, &IID_ID3D12DescriptorHeap, (void **)heap);
    return SUCCEEDED(result) ? TB_RHI_OK : tb_d3d12_fail(error, result, "CreateDescriptorHeap");
}

/* The root signature of every pipeline: push constants at b0, two constant
 * buffers at b1 and b2, and both heaps indexed directly by shaders. */
static tb_rhi_result tb_d3d12_root_signature(tb_d3d12_device *device, tb_rhi_error *error)
{
    D3D12_ROOT_PARAMETER1 parameters[1 + TB_RHI_CONSTANT_BUFFER_SLOTS] = {0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {0};
    ID3DBlob *blob = NULL;
    ID3DBlob *errors = NULL;
    HRESULT result = S_OK;

    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = TB_RHI_PUSH_CONSTANT_BYTES / 4;
    for (UINT slot = 0; slot < TB_RHI_CONSTANT_BUFFER_SLOTS; slot++)
    {
        parameters[1 + slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1 + slot].Descriptor.ShaderRegister = 1 + slot;
        parameters[1 + slot].Descriptor.Flags =
            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    }
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 1 + TB_RHI_CONSTANT_BUFFER_SLOTS;
    desc.Desc_1_1.pParameters = parameters;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (device->bindless)
    {
        desc.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                               D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    }
    result = device->serialize_root_signature(&desc, &blob, &errors);
    if (SUCCEEDED(result))
    {
        result = ID3D12Device_CreateRootSignature(
            device->device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void **)&device->root);
    }
    if (blob != NULL)
    {
        ID3D10Blob_Release(blob);
    }
    if (errors != NULL)
    {
        ID3D10Blob_Release(errors);
    }
    return SUCCEEDED(result) ? TB_RHI_OK : tb_d3d12_fail(error, result, "the root signature");
}

static void tb_d3d12_device_destroy(void *state);

static tb_rhi_result tb_d3d12_device_create(const tb_rhi_device_options *options, void **state,
                                            tb_rhi_device_info *info, tb_rhi_error *error)
{
    tb_d3d12_device *device = tb_calloc(1, sizeof(*device));
    const D3D12_COMMAND_QUEUE_DESC queue = {D3D12_COMMAND_LIST_TYPE_DIRECT, 0,
                                            D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    IDXGIFactory5 *factory5 = NULL;
    BOOL tearing = FALSE;
    HRESULT result = S_OK;
    tb_rhi_result made = TB_RHI_OK;

    device->log = options->log;
    made = tb_d3d12_load(device, error);
    if (made != TB_RHI_OK)
    {
        goto failed;
    }
    if (options->validation)
    {
        ID3D12Debug *debug = NULL;
        if (SUCCEEDED(device->get_debug_interface(&IID_ID3D12Debug, (void **)&debug)))
        {
            ID3D12Debug_EnableDebugLayer(debug);
            ID3D12Debug_Release(debug);
        }
    }
    result = device->create_factory(options->validation ? DXGI_CREATE_FACTORY_DEBUG : 0,
                                    &IID_IDXGIFactory6, (void **)&device->factory);
    if (FAILED(result))
    {
        made = tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d12: DXGI 1.6 is not here (0x%08lx)",
                           (unsigned long)result);
        goto failed;
    }
    if (SUCCEEDED(
            IDXGIFactory6_QueryInterface(device->factory, &IID_IDXGIFactory5, (void **)&factory5)))
    {
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(
                factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
        {
            device->tearing = tearing != FALSE;
        }
        IDXGIFactory5_Release(factory5);
    }
    made = tb_d3d12_open_adapter(device, options->adapter, error);
    if (made != TB_RHI_OK)
    {
        goto failed;
    }
    if (options->validation)
    {
        (void)ID3D12Device_QueryInterface(device->device, &IID_ID3D12InfoQueue,
                                          (void **)&device->info_queue);
    }
    result = ID3D12Device_CreateCommandQueue(device->device, &queue, &IID_ID3D12CommandQueue,
                                             (void **)&device->queue);
    if (SUCCEEDED(result))
    {
        result = ID3D12Device_CreateFence(device->device, 0, D3D12_FENCE_FLAG_NONE,
                                          &IID_ID3D12Fence, (void **)&device->fence);
    }
    if (SUCCEEDED(result))
    {
        result = ID3D12Device_CreateFence(device->device, 0, D3D12_FENCE_FLAG_NONE,
                                          &IID_ID3D12Fence, (void **)&device->flush_fence);
    }
    if (FAILED(result))
    {
        made = tb_d3d12_fail(error, result, "the queue");
        goto failed;
    }
    tb_d3d12_describe(device, info);
    made = tb_d3d12_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, info->bindless_resources,
                         true, &device->resources, error);
    if (made == TB_RHI_OK)
    {
        made = tb_d3d12_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, info->bindless_samplers,
                             true, &device->samplers, error);
    }
    if (made == TB_RHI_OK)
    {
        made = tb_d3d12_root_signature(device, error);
    }
    if (made != TB_RHI_OK)
    {
        goto failed;
    }
    (void)ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(device->resources,
                                                                  &device->resources_cpu);
    (void)ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(device->samplers,
                                                                  &device->samplers_cpu);
    device->resource_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        device->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->sampler_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        device->device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    device->target_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        device->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->depth_increment = ID3D12Device_GetDescriptorHandleIncrementSize(
        device->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    tb_d3d12_drain_messages(device);
    *state = device;
    return TB_RHI_OK;

failed:
    tb_d3d12_device_destroy(device);
    return made;
}

#define TB_D3D12_RELEASE(object, release)                                                          \
    if ((object) != NULL)                                                                          \
    {                                                                                              \
        (void)release(object);                                                                     \
    }

static void tb_d3d12_device_destroy(void *state)
{
    tb_d3d12_device *const device = state;

    tb_d3d12_release_lists(device);
    tb_d3d12_drain_messages(device);
    TB_D3D12_RELEASE(device->root, ID3D12RootSignature_Release)
    TB_D3D12_RELEASE(device->samplers, ID3D12DescriptorHeap_Release)
    TB_D3D12_RELEASE(device->resources, ID3D12DescriptorHeap_Release)
    TB_D3D12_RELEASE(device->flush_fence, ID3D12Fence_Release)
    TB_D3D12_RELEASE(device->fence, ID3D12Fence_Release)
    TB_D3D12_RELEASE(device->queue, ID3D12CommandQueue_Release)
    TB_D3D12_RELEASE(device->info_queue, ID3D12InfoQueue_Release)
    TB_D3D12_RELEASE(device->device, ID3D12Device_Release)
    TB_D3D12_RELEASE(device->adapter, IDXGIAdapter1_Release)
    TB_D3D12_RELEASE(device->factory, IDXGIFactory6_Release)
    tb_library_close(device->dxgi);
    tb_library_close(device->d3d12);
    tb_free(device);
}

/* ---------------------------------------------------------------------------
 * The GPU's progress.
 * ------------------------------------------------------------------------- */

static uint64_t tb_d3d12_completed(void *state)
{
    const tb_d3d12_device *const device = state;
    const UINT64 completed = ID3D12Fence_GetCompletedValue(device->fence);

    /* A removed device's fence reads all ones. */
    return completed == UINT64_MAX ? 0 : completed;
}

tb_rhi_result tb_d3d12_flush(tb_d3d12_device *device)
{
    HRESULT result = S_OK;

    device->flushed++;
    result = ID3D12CommandQueue_Signal(device->queue, device->flush_fence, device->flushed);
    if (SUCCEEDED(result))
    {
        result = ID3D12Fence_SetEventOnCompletion(device->flush_fence, device->flushed, NULL);
    }
    return SUCCEEDED(result) ? TB_RHI_OK : TB_RHI_DEVICE_LOST;
}

static tb_rhi_result tb_d3d12_wait(void *state, uint64_t submission)
{
    tb_d3d12_device *const device = state;
    HRESULT result = S_OK;

    if (ID3D12Fence_GetCompletedValue(device->fence) < submission)
    {
        /* With no event, the call itself waits. */
        result = ID3D12Fence_SetEventOnCompletion(device->fence, submission, NULL);
    }
    tb_d3d12_drain_messages(device);
    if (FAILED(result) || FAILED(ID3D12Device_GetDeviceRemovedReason(device->device)))
    {
        return TB_RHI_DEVICE_LOST;
    }
    return TB_RHI_OK;
}

const tb_rhi_backend_table tb_rhi_d3d12_backend = {
    .backend = TB_RHI_BACKEND_D3D12,
    .device_create = tb_d3d12_device_create,
    .device_destroy = tb_d3d12_device_destroy,
    .buffer_create = tb_d3d12_buffer_create,
    .buffer_destroy = tb_d3d12_buffer_destroy,
    .buffer_write = tb_d3d12_buffer_write,
    .buffer_read = tb_d3d12_buffer_read,
    .texture_create = tb_d3d12_texture_create,
    .texture_destroy = tb_d3d12_texture_destroy,
    .sampler_create = tb_d3d12_sampler_create,
    .sampler_destroy = tb_d3d12_sampler_destroy,
    .shader_create = tb_d3d12_shader_create,
    .shader_destroy = tb_d3d12_shader_destroy,
    .graphics_pipeline_create = tb_d3d12_graphics_pipeline_create,
    .compute_pipeline_create = tb_d3d12_compute_pipeline_create,
    .pipeline_destroy = tb_d3d12_pipeline_destroy,
    .swap_chain_create = tb_d3d12_swap_chain_create,
    .swap_chain_destroy = tb_d3d12_swap_chain_destroy,
    .swap_chain_current = tb_d3d12_swap_chain_current,
    .swap_chain_present = tb_d3d12_swap_chain_present,
    .swap_chain_resize = tb_d3d12_swap_chain_resize,
    .list_begin = tb_d3d12_list_begin,
    .list_discard = tb_d3d12_list_discard,
    .submit = tb_d3d12_submit,
    .completed = tb_d3d12_completed,
    .wait = tb_d3d12_wait,
    .cmd_barriers = tb_d3d12_cmd_barriers,
    .cmd_begin_pass = tb_d3d12_cmd_begin_pass,
    .cmd_end_pass = tb_d3d12_cmd_end_pass,
    .cmd_viewport = tb_d3d12_cmd_viewport,
    .cmd_scissor = tb_d3d12_cmd_scissor,
    .cmd_pipeline = tb_d3d12_cmd_pipeline,
    .cmd_push_constants = tb_d3d12_cmd_push_constants,
    .cmd_constant_buffer = tb_d3d12_cmd_constant_buffer,
    .cmd_vertex_buffer = tb_d3d12_cmd_vertex_buffer,
    .cmd_index_buffer = tb_d3d12_cmd_index_buffer,
    .cmd_draw = tb_d3d12_cmd_draw,
    .cmd_draw_indexed = tb_d3d12_cmd_draw_indexed,
    .cmd_dispatch = tb_d3d12_cmd_dispatch,
    .cmd_copy_buffer = tb_d3d12_cmd_copy_buffer,
    .cmd_copy_buffer_to_texture = tb_d3d12_cmd_copy_buffer_to_texture,
    .cmd_copy_texture_to_buffer = tb_d3d12_cmd_copy_texture_to_buffer,
    .cmd_begin_marker = tb_d3d12_cmd_begin_marker,
    .cmd_end_marker = tb_d3d12_cmd_end_marker,
};
