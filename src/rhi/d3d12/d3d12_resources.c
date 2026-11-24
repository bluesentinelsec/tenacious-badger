/* The D3D12 backend's resources: committed buffers and textures, their
 * descriptors written at the bindless indices the device gave them,
 * samplers, shaders, and pipeline states on the shared root signature. */
#include "d3d12_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <string.h>

static void tb_d3d12_name(ID3D12Object *object, const char *name)
{
    wchar_t wide[TB_RHI_NAME_CAPACITY] = {0};
    size_t index = 0;

    if (name == NULL)
    {
        return;
    }
    /* Debuggers show the name; ASCII is all a name needs to be read there. */
    for (; name[index] != '\0' && index + 1 < TB_RHI_NAME_CAPACITY; index++)
    {
        wide[index] = (wchar_t)(unsigned char)name[index];
    }
    (void)ID3D12Object_SetName(object, wide);
}

/* ---------------------------------------------------------------------------
 * Buffers.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_d3d12_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                     tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_d3d12_device *const device = state;
    tb_d3d12_buffer *buffer = NULL;
    D3D12_HEAP_PROPERTIES heap = {0};
    D3D12_RESOURCE_DESC resource = {0};
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    HRESULT result = S_OK;

    heap.Type = desc->memory == TB_RHI_MEMORY_UPLOAD     ? D3D12_HEAP_TYPE_UPLOAD
                : desc->memory == TB_RHI_MEMORY_READBACK ? D3D12_HEAP_TYPE_READBACK
                                                         : D3D12_HEAP_TYPE_DEFAULT;
    initial = desc->memory == TB_RHI_MEMORY_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
              : desc->memory == TB_RHI_MEMORY_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                       : D3D12_RESOURCE_STATE_COMMON;
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = desc->size;
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource.Flags = (desc->usage & TB_RHI_BUFFER_STORAGE) != 0
                         ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                         : D3D12_RESOURCE_FLAG_NONE;
    buffer = tb_calloc(1, sizeof(*buffer));
    buffer->memory = desc->memory;
    result = ID3D12Device_CreateCommittedResource(device->device, &heap, D3D12_HEAP_FLAG_NONE,
                                                  &resource, initial, NULL, &IID_ID3D12Resource,
                                                  (void **)&buffer->resource);
    if (SUCCEEDED(result) && desc->memory != TB_RHI_MEMORY_GPU)
    {
        result = ID3D12Resource_Map(buffer->resource, 0, NULL, (void **)&buffer->mapped);
    }
    if (FAILED(result))
    {
        tb_d3d12_buffer_destroy(device, buffer);
        return tb_d3d12_fail(error, result, "a buffer");
    }
    tb_d3d12_name((ID3D12Object *)buffer->resource, desc->name);
    if (indices.read != TB_RHI_NO_INDEX)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {0};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = (UINT)(desc->size / 4);
        view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        ID3D12Device_CreateShaderResourceView(
            device->device, buffer->resource, &view,
            tb_d3d12_descriptor(device->resources_cpu, device->resource_increment, indices.read));
    }
    if (indices.write != TB_RHI_NO_INDEX)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {0};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = (UINT)(desc->size / 4);
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        ID3D12Device_CreateUnorderedAccessView(
            device->device, buffer->resource, NULL, &view,
            tb_d3d12_descriptor(device->resources_cpu, device->resource_increment, indices.write));
    }
    *object = buffer;
    return TB_RHI_OK;
}

void tb_d3d12_buffer_destroy(void *state, void *object)
{
    tb_d3d12_buffer *const buffer = object;

    (void)state;
    if (buffer->resource != NULL)
    {
        (void)ID3D12Resource_Release(buffer->resource);
    }
    tb_free(buffer);
}

void tb_d3d12_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                           size_t size)
{
    tb_d3d12_buffer *const buffer = object;

    (void)state;
    memcpy(buffer->mapped + offset, data, size);
}

void tb_d3d12_buffer_read(void *state, void *object, uint64_t offset, void *data, size_t size)
{
    const tb_d3d12_buffer *const buffer = object;

    (void)state;
    memcpy(data, buffer->mapped + offset, size);
}

/* ---------------------------------------------------------------------------
 * Textures.
 * ------------------------------------------------------------------------- */

/* A depth format is made typeless, so that shaders can also read it. */
static DXGI_FORMAT tb_d3d12_resource_format(tb_rhi_format format)
{
    switch (format)
    {
    case TB_RHI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case TB_RHI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    default:
        return tb_d3d12_format(format);
    }
}

static DXGI_FORMAT tb_d3d12_read_format(tb_rhi_format format)
{
    switch (format)
    {
    case TB_RHI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case TB_RHI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    default:
        return tb_d3d12_format(format);
    }
}

static void tb_d3d12_texture_views(tb_d3d12_device *device, const tb_rhi_texture_desc *desc,
                                   ID3D12Resource *resource, tb_rhi_indices indices)
{
    if (indices.read != TB_RHI_NO_INDEX)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {0};
        view.Format = tb_d3d12_read_format(desc->format);
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc->kind == TB_RHI_TEXTURE_CUBE)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = desc->mips;
        }
        else if (desc->samples > 1)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        }
        else if (desc->layers > 1)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = desc->mips;
            view.Texture2DArray.ArraySize = desc->layers;
        }
        else
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = desc->mips;
        }
        ID3D12Device_CreateShaderResourceView(
            device->device, resource, &view,
            tb_d3d12_descriptor(device->resources_cpu, device->resource_increment, indices.read));
    }
    if (indices.write != TB_RHI_NO_INDEX)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view = {0};
        view.Format = tb_d3d12_format(desc->format);
        if (desc->layers > 1)
        {
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.ArraySize = desc->layers;
        }
        else
        {
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        ID3D12Device_CreateUnorderedAccessView(
            device->device, resource, NULL, &view,
            tb_d3d12_descriptor(device->resources_cpu, device->resource_increment, indices.write));
    }
}

tb_rhi_result tb_d3d12_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_d3d12_device *const device = state;
    const D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0};
    D3D12_RESOURCE_DESC resource = {0};
    D3D12_CLEAR_VALUE clear = {0};
    const D3D12_CLEAR_VALUE *optimized = NULL;
    tb_d3d12_texture *texture = NULL;
    HRESULT result = S_OK;

    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc->width;
    resource.Height = desc->height;
    resource.DepthOrArraySize = (UINT16)desc->layers;
    resource.MipLevels = (UINT16)desc->mips;
    resource.Format = tb_d3d12_resource_format(desc->format);
    resource.SampleDesc.Count = desc->samples;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    clear.Format = tb_d3d12_format(desc->format);
    if ((desc->usage & TB_RHI_TEXTURE_RENDER_TARGET) != 0)
    {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        memcpy(clear.Color, desc->clear, sizeof(clear.Color));
        optimized = &clear;
    }
    if ((desc->usage & TB_RHI_TEXTURE_DEPTH) != 0)
    {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.DepthStencil.Depth = desc->clear[0];
        optimized = &clear;
    }
    if ((desc->usage & TB_RHI_TEXTURE_STORAGE) != 0)
    {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    texture = tb_calloc(1, sizeof(*texture));
    texture->target_format = tb_d3d12_format(desc->format);
    result = ID3D12Device_CreateCommittedResource(device->device, &heap, D3D12_HEAP_FLAG_NONE,
                                                  &resource, D3D12_RESOURCE_STATE_COMMON, optimized,
                                                  &IID_ID3D12Resource, (void **)&texture->resource);
    if (FAILED(result))
    {
        tb_free(texture);
        return tb_d3d12_fail(error, result, "a texture");
    }
    tb_d3d12_name((ID3D12Object *)texture->resource, desc->name);
    tb_d3d12_texture_views(device, desc, texture->resource, indices);
    *object = texture;
    return TB_RHI_OK;
}

void tb_d3d12_texture_destroy(void *state, void *object)
{
    tb_d3d12_texture *const texture = object;

    (void)state;
    (void)ID3D12Resource_Release(texture->resource);
    tb_free(texture);
}

/* ---------------------------------------------------------------------------
 * Samplers.
 * ------------------------------------------------------------------------- */

static D3D12_TEXTURE_ADDRESS_MODE tb_d3d12_address(tb_rhi_address address)
{
    static const D3D12_TEXTURE_ADDRESS_MODE modes[] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_BORDER};
    return modes[address];
}

static D3D12_COMPARISON_FUNC tb_d3d12_compare(tb_rhi_compare compare)
{
    /* The values of D3D12 are those of the RHI, one more. */
    return (D3D12_COMPARISON_FUNC)((int)compare + 1);
}

tb_rhi_result tb_d3d12_sampler_create(void *state, const tb_rhi_sampler_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_d3d12_device *const device = state;
    D3D12_SAMPLER_DESC sampler = {0};
    UINT filter = 0;

    (void)error;
    filter |= desc->min_filter == TB_RHI_FILTER_LINEAR ? 0x10u : 0u;
    filter |= desc->mag_filter == TB_RHI_FILTER_LINEAR ? 0x4u : 0u;
    filter |= desc->mip_filter == TB_RHI_FILTER_LINEAR ? 0x1u : 0u;
    if (desc->anisotropy > 1)
    {
        filter = D3D12_FILTER_ANISOTROPIC;
    }
    if (desc->compare)
    {
        filter |= 0x80u; /* the comparison filters */
    }
    sampler.Filter = (D3D12_FILTER)filter;
    sampler.AddressU = tb_d3d12_address(desc->address_u);
    sampler.AddressV = tb_d3d12_address(desc->address_v);
    sampler.AddressW = tb_d3d12_address(desc->address_w);
    sampler.MipLODBias = desc->mip_bias;
    sampler.MaxAnisotropy = desc->anisotropy == 0 ? 1u : desc->anisotropy;
    sampler.ComparisonFunc =
        desc->compare ? tb_d3d12_compare(desc->compare_op) : D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    ID3D12Device_CreateSampler(
        device->device, &sampler,
        tb_d3d12_descriptor(device->samplers_cpu, device->sampler_increment, indices.read));
    /* The descriptor is the sampler; the object only gives it an address. */
    *object = tb_calloc(1, 1);
    return TB_RHI_OK;
}

void tb_d3d12_sampler_destroy(void *state, void *object)
{
    (void)state;
    tb_free(object);
}

/* ---------------------------------------------------------------------------
 * Shaders and pipelines.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_d3d12_shader_create(void *state, const tb_rhi_shader_desc *desc, void **object,
                                     tb_rhi_error *error)
{
    tb_d3d12_shader *shader = NULL;

    (void)state;
    /* A DXIL container starts with "DXBC"; D3D12 checks the rest. */
    if (desc->size < 4 || memcmp(desc->code, "DXBC", 4) != 0)
    {
        return tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "d3d12: a shader that is not DXIL");
    }
    shader = tb_calloc(1, sizeof(*shader));
    shader->code = tb_malloc(desc->size);
    memcpy(shader->code, desc->code, desc->size);
    shader->size = desc->size;
    *object = shader;
    return TB_RHI_OK;
}

void tb_d3d12_shader_destroy(void *state, void *object)
{
    tb_d3d12_shader *const shader = object;

    (void)state;
    tb_free(shader->code);
    tb_free(shader);
}

static D3D12_BLEND tb_d3d12_blend_factor(tb_rhi_blend_factor factor)
{
    static const D3D12_BLEND factors[] = {D3D12_BLEND_ZERO,       D3D12_BLEND_ONE,
                                          D3D12_BLEND_SRC_COLOR,  D3D12_BLEND_INV_SRC_COLOR,
                                          D3D12_BLEND_SRC_ALPHA,  D3D12_BLEND_INV_SRC_ALPHA,
                                          D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
                                          D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};
    return factors[factor];
}

static D3D12_BLEND_OP tb_d3d12_blend_op(tb_rhi_blend_op op)
{
    /* The values of D3D12 are those of the RHI, one more. */
    return (D3D12_BLEND_OP)((int)op + 1);
}

static void tb_d3d12_blend_state(const tb_rhi_graphics_pipeline_desc *desc, D3D12_BLEND_DESC *blend)
{
    blend->IndependentBlendEnable = TRUE;
    for (uint32_t index = 0; index < TB_RHI_COLOR_TARGETS; index++)
    {
        const tb_rhi_blend *const given = &desc->blend[index];
        D3D12_RENDER_TARGET_BLEND_DESC *const target = &blend->RenderTarget[index];
        target->BlendEnable = given->enabled;
        target->SrcBlend = tb_d3d12_blend_factor(given->color_source);
        target->DestBlend = tb_d3d12_blend_factor(given->color_dest);
        target->BlendOp = tb_d3d12_blend_op(given->color_op);
        target->SrcBlendAlpha = tb_d3d12_blend_factor(given->alpha_source);
        target->DestBlendAlpha = tb_d3d12_blend_factor(given->alpha_dest);
        target->BlendOpAlpha = tb_d3d12_blend_op(given->alpha_op);
        target->LogicOp = D3D12_LOGIC_OP_NOOP;
        target->RenderTargetWriteMask =
            (UINT8)(D3D12_COLOR_WRITE_ENABLE_ALL & ~given->write_mask_off);
    }
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE tb_d3d12_topology_type(tb_rhi_topology topology)
{
    switch (topology)
    {
    case TB_RHI_TOPOLOGY_LINES:
    case TB_RHI_TOPOLOGY_LINE_STRIP:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    case TB_RHI_TOPOLOGY_POINTS:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    case TB_RHI_TOPOLOGY_TRIANGLES:
    case TB_RHI_TOPOLOGY_TRIANGLE_STRIP:
        break;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
}

static D3D_PRIMITIVE_TOPOLOGY tb_d3d12_topology(tb_rhi_topology topology)
{
    static const D3D_PRIMITIVE_TOPOLOGY topologies[] = {
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
        D3D_PRIMITIVE_TOPOLOGY_LINELIST, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
        D3D_PRIMITIVE_TOPOLOGY_POINTLIST};
    return topologies[topology];
}

tb_rhi_result tb_d3d12_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *pipeline,
                                                void **object, tb_rhi_error *error)
{
    tb_d3d12_device *const device = state;
    const tb_rhi_graphics_pipeline_desc *const desc = pipeline->desc;
    const tb_d3d12_shader *const vertex = pipeline->vertex;
    const tb_d3d12_shader *const pixel = pipeline->pixel;
    D3D12_INPUT_ELEMENT_DESC elements[TB_RHI_VERTEX_ATTRIBUTES] = {0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state_desc = {0};
    tb_d3d12_pipeline *made = NULL;
    HRESULT result = S_OK;

    for (uint32_t index = 0; index < desc->attribute_count; index++)
    {
        const tb_rhi_vertex_attribute *const attribute = &desc->attributes[index];
        const bool instanced = desc->buffers[attribute->buffer].step == TB_RHI_STEP_INSTANCE;
        elements[index].SemanticName = attribute->semantic;
        elements[index].SemanticIndex = attribute->semantic_index;
        elements[index].Format = tb_d3d12_format(attribute->format);
        elements[index].InputSlot = attribute->buffer;
        elements[index].AlignedByteOffset = attribute->offset;
        elements[index].InputSlotClass = instanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
                                                   : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        elements[index].InstanceDataStepRate = instanced ? 1u : 0u;
    }
    state_desc.pRootSignature = device->root;
    state_desc.VS = (D3D12_SHADER_BYTECODE){vertex->code, vertex->size};
    if (pixel != NULL)
    {
        state_desc.PS = (D3D12_SHADER_BYTECODE){pixel->code, pixel->size};
    }
    tb_d3d12_blend_state(desc, &state_desc.BlendState);
    state_desc.SampleMask = UINT_MAX;
    state_desc.RasterizerState.FillMode =
        desc->wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    state_desc.RasterizerState.CullMode = desc->cull == TB_RHI_CULL_BACK    ? D3D12_CULL_MODE_BACK
                                          : desc->cull == TB_RHI_CULL_FRONT ? D3D12_CULL_MODE_FRONT
                                                                            : D3D12_CULL_MODE_NONE;
    state_desc.RasterizerState.FrontCounterClockwise = desc->clockwise_front ? FALSE : TRUE;
    state_desc.RasterizerState.DepthBias = (INT)desc->depth_bias;
    state_desc.RasterizerState.SlopeScaledDepthBias = desc->depth_bias_slope;
    state_desc.RasterizerState.DepthClipEnable = TRUE;
    state_desc.DepthStencilState.DepthEnable = desc->depth_test || desc->depth_write;
    state_desc.DepthStencilState.DepthWriteMask =
        desc->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    state_desc.DepthStencilState.DepthFunc =
        desc->depth_test ? tb_d3d12_compare(desc->depth_compare) : D3D12_COMPARISON_FUNC_ALWAYS;
    state_desc.InputLayout = (D3D12_INPUT_LAYOUT_DESC){elements, desc->attribute_count};
    state_desc.PrimitiveTopologyType = tb_d3d12_topology_type(desc->topology);
    state_desc.NumRenderTargets = desc->color_count;
    for (uint32_t index = 0; index < desc->color_count; index++)
    {
        state_desc.RTVFormats[index] = tb_d3d12_format(desc->colors[index]);
    }
    state_desc.DSVFormat = tb_d3d12_format(desc->depth);
    state_desc.SampleDesc.Count = desc->samples == 0 ? 1u : desc->samples;
    made = tb_calloc(1, sizeof(*made));
    made->topology = tb_d3d12_topology(desc->topology);
    result = ID3D12Device_CreateGraphicsPipelineState(
        device->device, &state_desc, &IID_ID3D12PipelineState, (void **)&made->state);
    tb_d3d12_drain_messages(device);
    if (FAILED(result))
    {
        tb_free(made);
        return tb_d3d12_fail(error, result, "a graphics pipeline");
    }
    tb_d3d12_name((ID3D12Object *)made->state, desc->name);
    *object = made;
    return TB_RHI_OK;
}

tb_rhi_result tb_d3d12_compute_pipeline_create(void *state,
                                               const tb_rhi_compute_pipeline_desc *desc,
                                               void *shader, const tb_rhi_shader_desc *shader_desc,
                                               void **object, tb_rhi_error *error)
{
    tb_d3d12_device *const device = state;
    const tb_d3d12_shader *const compute = shader;
    D3D12_COMPUTE_PIPELINE_STATE_DESC state_desc = {0};
    tb_d3d12_pipeline *made = NULL;
    HRESULT result = S_OK;

    (void)shader_desc;
    state_desc.pRootSignature = device->root;
    state_desc.CS = (D3D12_SHADER_BYTECODE){compute->code, compute->size};
    made = tb_calloc(1, sizeof(*made));
    made->compute = true;
    result = ID3D12Device_CreateComputePipelineState(
        device->device, &state_desc, &IID_ID3D12PipelineState, (void **)&made->state);
    tb_d3d12_drain_messages(device);
    if (FAILED(result))
    {
        tb_free(made);
        return tb_d3d12_fail(error, result, "a compute pipeline");
    }
    tb_d3d12_name((ID3D12Object *)made->state, desc->name);
    *object = made;
    return TB_RHI_OK;
}

void tb_d3d12_pipeline_destroy(void *state, void *object)
{
    tb_d3d12_pipeline *const pipeline = object;

    (void)state;
    (void)ID3D12PipelineState_Release(pipeline->state);
    tb_free(pipeline);
}
