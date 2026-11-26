/* The D3D11 backend's resources: buffers and textures with the views their
 * bindless indices name, samplers, shaders made from the engine's shaders
 * for D3D11, and pipelines as D3D11's state objects. */
#include "d3d11_internal.h"

#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* Bytes of a raw buffer's element: views of buffers read them as words. */
#define TB_D3D11_WORD 4u

static void tb_d3d11_name(ID3D11DeviceChild *object, const char *name)
{
    if (name == NULL || object == NULL)
    {
        return;
    }
    (void)ID3D11DeviceChild_SetPrivateData(object, &WKPDID_D3DDebugObjectName,
                                           (UINT)tb_str_length(name), name);
}

/* Put a view where an index names it, or take it away. */
static void tb_d3d11_set_view(tb_d3d11_device *device, uint32_t index, ID3D11View *view,
                              bool writable, tb_shader_dxbc_view shape)
{
    if (index == TB_RHI_NO_INDEX || index >= device->view_count)
    {
        return;
    }
    tb_mutex_lock(&device->context_lock);
    device->views[index] = (tb_d3d11_view){view, writable, (uint8_t)shape};
    tb_mutex_unlock(&device->context_lock);
}

static void tb_d3d11_clear_views(tb_d3d11_device *device, const tb_rhi_indices *indices)
{
    tb_d3d11_set_view(device, indices->read, NULL, false, TB_SHADER_DXBC_VIEW_RAW_BUFFER);
    tb_d3d11_set_view(device, indices->write, NULL, false, TB_SHADER_DXBC_VIEW_RAW_BUFFER);
    for (uint32_t mip = 0; mip < indices->mip_write_count; mip++)
    {
        tb_d3d11_set_view(device, indices->mip_writes[mip], NULL, false,
                          TB_SHADER_DXBC_VIEW_RAW_BUFFER);
    }
}

/* ---------------------------------------------------------------------------
 * Buffers.
 * ------------------------------------------------------------------------- */

/* What D3D11 binds a buffer of the RHI's as: none for one that is only
 * copied, or only read as constants, whose CPU copy draws read. */
static UINT tb_d3d11_buffer_binds(uint32_t usage)
{
    UINT binds = 0;
    binds |= (usage & TB_RHI_BUFFER_VERTEX) != 0 ? D3D11_BIND_VERTEX_BUFFER : 0u;
    binds |= (usage & TB_RHI_BUFFER_INDEX) != 0 ? D3D11_BIND_INDEX_BUFFER : 0u;
    binds |= (usage & TB_RHI_BUFFER_SHADER_READ) != 0 ? D3D11_BIND_SHADER_RESOURCE : 0u;
    binds |= (usage & TB_RHI_BUFFER_STORAGE) != 0 ? D3D11_BIND_UNORDERED_ACCESS : 0u;
    return binds;
}

static void tb_d3d11_track_upload(tb_d3d11_device *device, tb_d3d11_buffer *buffer, bool add)
{
    tb_mutex_lock(&device->context_lock);
    if (add)
    {
        if (device->upload_count == device->upload_capacity)
        {
            device->upload_capacity =
                device->upload_capacity == 0 ? 16 : device->upload_capacity * 2;
            device->uploads = tb_realloc_array(device->uploads, device->upload_capacity,
                                               sizeof(*device->uploads));
        }
        device->uploads[device->upload_count] = buffer;
        device->upload_count++;
    }
    else
    {
        for (size_t index = 0; index < device->upload_count; index++)
        {
            if (device->uploads[index] == buffer)
            {
                device->uploads[index] = device->uploads[device->upload_count - 1];
                device->upload_count--;
                break;
            }
        }
    }
    tb_mutex_unlock(&device->context_lock);
}

static HRESULT tb_d3d11_buffer_views(tb_d3d11_device *device, tb_d3d11_buffer *buffer)
{
    HRESULT result = S_OK;
    const UINT words = (UINT)(buffer->size / TB_D3D11_WORD);

    if (buffer->indices.read != TB_RHI_NO_INDEX && buffer->buffer != NULL)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC view = {0};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.NumElements = words;
        view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        result = ID3D11Device_CreateShaderResourceView(
            device->device, (ID3D11Resource *)buffer->buffer, &view, &buffer->read);
        tb_d3d11_set_view(device, buffer->indices.read, (ID3D11View *)buffer->read, false,
                          TB_SHADER_DXBC_VIEW_RAW_BUFFER);
    }
    if (SUCCEEDED(result) && buffer->indices.write != TB_RHI_NO_INDEX && buffer->buffer != NULL)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC view = {0};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = words;
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        result = ID3D11Device_CreateUnorderedAccessView(
            device->device, (ID3D11Resource *)buffer->buffer, &view, &buffer->write);
        tb_d3d11_set_view(device, buffer->indices.write, (ID3D11View *)buffer->write, true,
                          TB_SHADER_DXBC_VIEW_RAW_BUFFER);
    }
    return result;
}

tb_rhi_result tb_d3d11_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                     tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_buffer *const buffer = tb_calloc(1, sizeof(*buffer));
    const UINT binds = tb_d3d11_buffer_binds(desc->usage);
    HRESULT result = S_OK;

    buffer->size = desc->size;
    buffer->memory = desc->memory;
    buffer->usage = desc->usage;
    buffer->indices = indices;
    if (desc->size > UINT_MAX)
    {
        tb_free(buffer);
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "d3d11: a buffer of more than 4 GiB");
    }
    /* Upload and readback memory are the CPU's; so is a copy of a constant
     * buffer, which each draw reads its constants from. */
    if (desc->memory != TB_RHI_MEMORY_GPU || (desc->usage & TB_RHI_BUFFER_CONSTANT) != 0)
    {
        buffer->cpu = tb_calloc(1, (size_t)desc->size);
    }
    if (desc->memory != TB_RHI_MEMORY_READBACK && (binds != 0 || desc->memory == TB_RHI_MEMORY_GPU))
    {
        D3D11_BUFFER_DESC made = {0};
        made.ByteWidth = (UINT)desc->size;
        made.Usage = D3D11_USAGE_DEFAULT;
        made.BindFlags = binds;
        made.MiscFlags = (desc->usage & (TB_RHI_BUFFER_SHADER_READ | TB_RHI_BUFFER_STORAGE)) != 0
                             ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS
                             : 0u;
        result = ID3D11Device_CreateBuffer(device->device, &made, NULL, &buffer->buffer);
        if (SUCCEEDED(result))
        {
            tb_d3d11_name((ID3D11DeviceChild *)buffer->buffer, desc->name);
            result = tb_d3d11_buffer_views(device, buffer);
        }
    }
    if (FAILED(result))
    {
        tb_d3d11_buffer_destroy(device, buffer);
        return tb_d3d11_fail(error, result, "a buffer");
    }
    if (desc->memory == TB_RHI_MEMORY_UPLOAD && buffer->buffer != NULL)
    {
        tb_d3d11_track_upload(device, buffer, true);
    }
    *object = buffer;
    return TB_RHI_OK;
}

void tb_d3d11_buffer_destroy(void *state, void *object)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_buffer *const buffer = object;

    tb_d3d11_clear_views(device, &buffer->indices);
    if (buffer->memory == TB_RHI_MEMORY_UPLOAD && buffer->buffer != NULL)
    {
        tb_d3d11_track_upload(device, buffer, false);
    }
    if (buffer->memory == TB_RHI_MEMORY_READBACK)
    {
        tb_mutex_lock(&device->context_lock);
        tb_d3d11_drop_readbacks(device, buffer);
        tb_mutex_unlock(&device->context_lock);
    }
    if (buffer->read != NULL)
    {
        (void)ID3D11ShaderResourceView_Release(buffer->read);
    }
    if (buffer->write != NULL)
    {
        (void)ID3D11UnorderedAccessView_Release(buffer->write);
    }
    if (buffer->buffer != NULL)
    {
        (void)ID3D11Buffer_Release(buffer->buffer);
    }
    tb_free(buffer->cpu);
    tb_free(buffer);
}

void tb_d3d11_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                           size_t size)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_buffer *const buffer = object;

    memcpy(buffer->cpu + offset, data, size);
    if (buffer->buffer == NULL)
    {
        return;
    }
    /* The next submission brings the GPU's copy up to date, before it runs. */
    tb_mutex_lock(&device->context_lock);
    if (buffer->dirty_end == buffer->dirty_begin)
    {
        buffer->dirty_begin = offset;
        buffer->dirty_end = offset + size;
    }
    else
    {
        buffer->dirty_begin = offset < buffer->dirty_begin ? offset : buffer->dirty_begin;
        buffer->dirty_end = offset + size > buffer->dirty_end ? offset + size : buffer->dirty_end;
    }
    tb_mutex_unlock(&device->context_lock);
}

void tb_d3d11_buffer_read(void *state, void *object, uint64_t offset, void *data, size_t size)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_buffer *const buffer = object;

    if (buffer->memory == TB_RHI_MEMORY_READBACK)
    {
        tb_mutex_lock(&device->context_lock);
        tb_d3d11_resolve_readbacks(device, buffer);
        tb_mutex_unlock(&device->context_lock);
    }
    memcpy(data, buffer->cpu + offset, size);
}

/* ---------------------------------------------------------------------------
 * Textures.
 * ------------------------------------------------------------------------- */

/* A depth format is made typeless, so that shaders can also read it; an
 * sRGB format written by shaders is too, since D3D11 writes no sRGB view:
 * shaders write its channels through a linear view, encoded. */
static DXGI_FORMAT tb_d3d11_resource_format(const tb_rhi_texture_desc *desc)
{
    const bool storage = (desc->usage & TB_RHI_TEXTURE_STORAGE) != 0;

    if (desc->format == TB_RHI_FORMAT_D32_FLOAT)
    {
        return DXGI_FORMAT_R32_TYPELESS;
    }
    if (desc->format == TB_RHI_FORMAT_D24_UNORM_S8_UINT)
    {
        return DXGI_FORMAT_R24G8_TYPELESS;
    }
    if (desc->format == TB_RHI_FORMAT_R8G8B8A8_SRGB && storage)
    {
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    }
    if (desc->format == TB_RHI_FORMAT_B8G8R8A8_SRGB && storage)
    {
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }
    return tb_d3d11_format(desc->format);
}

/* The format shaders write a texture through. */
static DXGI_FORMAT tb_d3d11_storage_format(tb_rhi_format format)
{
    if (format == TB_RHI_FORMAT_R8G8B8A8_SRGB)
    {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (format == TB_RHI_FORMAT_B8G8R8A8_SRGB)
    {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    return tb_d3d11_format(format);
}

static DXGI_FORMAT tb_d3d11_read_format(tb_rhi_format format)
{
    if (format == TB_RHI_FORMAT_D32_FLOAT)
    {
        return DXGI_FORMAT_R32_FLOAT;
    }
    if (format == TB_RHI_FORMAT_D24_UNORM_S8_UINT)
    {
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
    return tb_d3d11_format(format);
}

/* What a shader declares to read a texture through its view. */
static tb_shader_dxbc_view tb_d3d11_shape(const tb_rhi_texture_desc *desc)
{
    if (desc->kind == TB_RHI_TEXTURE_CUBE)
    {
        return TB_SHADER_DXBC_VIEW_TEXTURE_CUBE;
    }
    return desc->layers > 1 ? TB_SHADER_DXBC_VIEW_TEXTURE2D_ARRAY : TB_SHADER_DXBC_VIEW_TEXTURE2D;
}

static HRESULT tb_d3d11_storage_view(tb_d3d11_device *device, tb_d3d11_texture *texture,
                                     uint32_t mip, uint32_t index)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC view = {0};
    HRESULT result = S_OK;

    view.Format = tb_d3d11_storage_format(texture->desc.format);
    if (texture->desc.layers > 1)
    {
        view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = mip;
        view.Texture2DArray.ArraySize = texture->desc.layers;
    }
    else
    {
        view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mip;
    }
    result = ID3D11Device_CreateUnorderedAccessView(
        device->device, (ID3D11Resource *)texture->texture, &view, &texture->writes[mip]);
    if (SUCCEEDED(result))
    {
        /* A level's view is 2D or a 2D array, a cube's too. */
        const tb_shader_dxbc_view shape = texture->desc.layers > 1
                                              ? TB_SHADER_DXBC_VIEW_TEXTURE2D_ARRAY
                                              : TB_SHADER_DXBC_VIEW_TEXTURE2D;
        tb_d3d11_set_view(device, index, (ID3D11View *)texture->writes[mip], true, shape);
    }
    return result;
}

static HRESULT tb_d3d11_texture_views(tb_d3d11_device *device, tb_d3d11_texture *texture)
{
    const tb_rhi_texture_desc *const desc = &texture->desc;
    const tb_rhi_indices *const indices = &texture->indices;
    HRESULT result = S_OK;

    if (indices->read != TB_RHI_NO_INDEX)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC view = {0};
        view.Format = tb_d3d11_read_format(desc->format);
        if (desc->kind == TB_RHI_TEXTURE_CUBE)
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = desc->mips;
        }
        else if (desc->samples > 1)
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        }
        else if (desc->layers > 1)
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = desc->mips;
            view.Texture2DArray.ArraySize = desc->layers;
        }
        else
        {
            view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = desc->mips;
        }
        result = ID3D11Device_CreateShaderResourceView(
            device->device, (ID3D11Resource *)texture->texture, &view, &texture->read);
        if (SUCCEEDED(result))
        {
            tb_d3d11_set_view(device, indices->read, (ID3D11View *)texture->read, false,
                              tb_d3d11_shape(desc));
        }
    }
    if (SUCCEEDED(result) && indices->write != TB_RHI_NO_INDEX)
    {
        result = tb_d3d11_storage_view(device, texture, 0, indices->write);
    }
    for (uint32_t mip = 0; SUCCEEDED(result) && mip < indices->mip_write_count; mip++)
    {
        result = tb_d3d11_storage_view(device, texture, mip + 1, indices->mip_writes[mip]);
    }
    return result;
}

tb_rhi_result tb_d3d11_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    D3D11_TEXTURE2D_DESC made = {0};
    tb_d3d11_texture *const texture = tb_calloc(1, sizeof(*texture));
    HRESULT result = S_OK;

    texture->desc = *desc;
    texture->desc.name = NULL;
    texture->indices = indices;
    texture->resource_format = tb_d3d11_resource_format(desc);
    texture->target_format = tb_d3d11_format(desc->format);
    made.Width = desc->width;
    made.Height = desc->height;
    made.MipLevels = desc->mips;
    made.ArraySize = desc->layers;
    made.Format = texture->resource_format;
    made.SampleDesc.Count = desc->samples;
    made.Usage = D3D11_USAGE_DEFAULT;
    made.BindFlags |= (desc->usage & TB_RHI_TEXTURE_SAMPLED) != 0 ? D3D11_BIND_SHADER_RESOURCE : 0u;
    made.BindFlags |=
        (desc->usage & TB_RHI_TEXTURE_STORAGE) != 0 ? D3D11_BIND_UNORDERED_ACCESS : 0u;
    made.BindFlags |=
        (desc->usage & TB_RHI_TEXTURE_RENDER_TARGET) != 0 ? D3D11_BIND_RENDER_TARGET : 0u;
    made.BindFlags |= (desc->usage & TB_RHI_TEXTURE_DEPTH) != 0 ? D3D11_BIND_DEPTH_STENCIL : 0u;
    made.MiscFlags = desc->kind == TB_RHI_TEXTURE_CUBE ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u;
    result = ID3D11Device_CreateTexture2D(device->device, &made, NULL, &texture->texture);
    if (SUCCEEDED(result))
    {
        tb_d3d11_name((ID3D11DeviceChild *)texture->texture, desc->name);
        result = tb_d3d11_texture_views(device, texture);
    }
    if (FAILED(result))
    {
        tb_d3d11_texture_destroy(device, texture);
        return tb_d3d11_fail(error, result, "a texture");
    }
    *object = texture;
    return TB_RHI_OK;
}

void tb_d3d11_texture_release_views(tb_d3d11_texture *texture)
{
    const uint32_t targets = texture->desc.mips * texture->desc.layers;

    if (texture->read != NULL)
    {
        (void)ID3D11ShaderResourceView_Release(texture->read);
        texture->read = NULL;
    }
    for (uint32_t mip = 0; mip < TB_RHI_MAX_MIPS; mip++)
    {
        if (texture->writes[mip] != NULL)
        {
            (void)ID3D11UnorderedAccessView_Release(texture->writes[mip]);
            texture->writes[mip] = NULL;
        }
    }
    for (uint32_t index = 0; texture->targets != NULL && index < targets; index++)
    {
        if (texture->targets[index] != NULL)
        {
            (void)ID3D11RenderTargetView_Release(texture->targets[index]);
        }
    }
    tb_free(texture->targets);
    texture->targets = NULL;
    for (size_t index = 0; index < 2; index++)
    {
        if (texture->depth[index] != NULL)
        {
            (void)ID3D11DepthStencilView_Release(texture->depth[index]);
            texture->depth[index] = NULL;
        }
    }
}

void tb_d3d11_texture_destroy(void *state, void *object)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_texture *const texture = object;

    tb_d3d11_clear_views(device, &texture->indices);
    tb_d3d11_texture_release_views(texture);
    if (texture->texture != NULL)
    {
        (void)ID3D11Texture2D_Release(texture->texture);
    }
    tb_free(texture);
}

ID3D11RenderTargetView *tb_d3d11_target_view(tb_d3d11_device *device, tb_d3d11_texture *texture,
                                             uint32_t mip, uint32_t layer)
{
    const uint32_t count = texture->desc.mips * texture->desc.layers;
    const uint32_t index = layer * texture->desc.mips + mip;
    D3D11_RENDER_TARGET_VIEW_DESC view = {0};

    if (texture->targets == NULL)
    {
        texture->targets = tb_calloc(count, sizeof(*texture->targets));
    }
    if (texture->targets[index] != NULL)
    {
        return texture->targets[index];
    }
    view.Format = texture->target_format;
    if (texture->desc.samples > 1)
    {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    }
    else if (texture->desc.layers > 1)
    {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = mip;
        view.Texture2DArray.FirstArraySlice = layer;
        view.Texture2DArray.ArraySize = 1;
    }
    else
    {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = mip;
    }
    (void)ID3D11Device_CreateRenderTargetView(device->device, (ID3D11Resource *)texture->texture,
                                              &view, &texture->targets[index]);
    return texture->targets[index];
}

ID3D11DepthStencilView *tb_d3d11_depth_view(tb_d3d11_device *device, tb_d3d11_texture *texture,
                                            bool read_only)
{
    const size_t index = read_only ? 1u : 0u;
    D3D11_DEPTH_STENCIL_VIEW_DESC view = {0};

    if (texture->depth[index] != NULL)
    {
        return texture->depth[index];
    }
    view.Format = texture->target_format;
    view.ViewDimension =
        texture->desc.samples > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    view.Flags = read_only ? D3D11_DSV_READ_ONLY_DEPTH : 0u;
    if (read_only && texture->desc.format == TB_RHI_FORMAT_D24_UNORM_S8_UINT)
    {
        view.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
    }
    (void)ID3D11Device_CreateDepthStencilView(device->device, (ID3D11Resource *)texture->texture,
                                              &view, &texture->depth[index]);
    return texture->depth[index];
}

/* ---------------------------------------------------------------------------
 * Samplers.
 * ------------------------------------------------------------------------- */

static D3D11_TEXTURE_ADDRESS_MODE tb_d3d11_address(tb_rhi_address address)
{
    static const D3D11_TEXTURE_ADDRESS_MODE modes[] = {
        D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP,
        D3D11_TEXTURE_ADDRESS_BORDER};
    return modes[address];
}

static D3D11_COMPARISON_FUNC tb_d3d11_compare(tb_rhi_compare compare)
{
    /* The values of D3D11 are those of the RHI, one more. */
    return (D3D11_COMPARISON_FUNC)((int)compare + 1);
}

tb_rhi_result tb_d3d11_sampler_create(void *state, const tb_rhi_sampler_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    D3D11_SAMPLER_DESC sampler = {0};
    ID3D11SamplerState *made = NULL;
    UINT filter = 0;
    HRESULT result = S_OK;

    filter |= desc->min_filter == TB_RHI_FILTER_LINEAR ? 0x10u : 0u;
    filter |= desc->mag_filter == TB_RHI_FILTER_LINEAR ? 0x4u : 0u;
    filter |= desc->mip_filter == TB_RHI_FILTER_LINEAR ? 0x1u : 0u;
    if (desc->anisotropy > 1)
    {
        filter = D3D11_FILTER_ANISOTROPIC;
    }
    if (desc->compare)
    {
        filter |= 0x80u; /* the comparison filters */
    }
    sampler.Filter = (D3D11_FILTER)filter;
    sampler.AddressU = tb_d3d11_address(desc->address_u);
    sampler.AddressV = tb_d3d11_address(desc->address_v);
    sampler.AddressW = tb_d3d11_address(desc->address_w);
    sampler.MipLODBias = desc->mip_bias;
    sampler.MaxAnisotropy = desc->anisotropy == 0 ? 1u : desc->anisotropy;
    sampler.ComparisonFunc =
        desc->compare ? tb_d3d11_compare(desc->compare_op) : D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    result = ID3D11Device_CreateSamplerState(device->device, &sampler, &made);
    if (FAILED(result))
    {
        return tb_d3d11_fail(error, result, "a sampler");
    }
    tb_d3d11_name((ID3D11DeviceChild *)made, desc->name);
    if (indices.read < device->sampler_count)
    {
        tb_mutex_lock(&device->context_lock);
        device->samplers[indices.read] = made;
        tb_mutex_unlock(&device->context_lock);
    }
    *object = made;
    return TB_RHI_OK;
}

void tb_d3d11_sampler_destroy(void *state, void *object)
{
    tb_d3d11_device *const device = state;
    ID3D11SamplerState *const sampler = object;

    tb_mutex_lock(&device->context_lock);
    for (uint32_t index = 0; index < device->sampler_count; index++)
    {
        if (device->samplers[index] == sampler)
        {
            device->samplers[index] = NULL;
            break;
        }
    }
    tb_mutex_unlock(&device->context_lock);
    (void)ID3D11SamplerState_Release(sampler);
}

/* ---------------------------------------------------------------------------
 * Shaders and pipelines.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_d3d11_shader_create(void *state, const tb_rhi_shader_desc *desc, void **object,
                                     tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    tb_d3d11_shader *shader = NULL;
    HRESULT result = S_OK;

    shader = tb_calloc(1, sizeof(*shader));
    shader->stage = desc->stage;
    shader->bytes = tb_malloc(desc->size);
    memcpy(shader->bytes, desc->code, desc->size);
    if (!tb_shader_dxbc_read(shader->bytes, desc->size, &shader->read))
    {
        tb_free(shader->bytes);
        tb_free(shader);
        return tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR,
                           "d3d11: a shader that is not the shader compiler's for D3D11");
    }
    switch (desc->stage)
    {
    case TB_RHI_STAGE_VERTEX:
        result = ID3D11Device_CreateVertexShader(device->device, shader->read.code,
                                                 shader->read.code_size, NULL,
                                                 (ID3D11VertexShader **)&shader->object);
        break;
    case TB_RHI_STAGE_PIXEL:
        result = ID3D11Device_CreatePixelShader(device->device, shader->read.code,
                                                shader->read.code_size, NULL,
                                                (ID3D11PixelShader **)&shader->object);
        break;
    case TB_RHI_STAGE_COMPUTE:
        result = ID3D11Device_CreateComputeShader(device->device, shader->read.code,
                                                  shader->read.code_size, NULL,
                                                  (ID3D11ComputeShader **)&shader->object);
        break;
    }
    tb_d3d11_drain_messages(device);
    if (FAILED(result) && desc->stage == TB_RHI_STAGE_COMPUTE && !device->typed_loads)
    {
        /* coverage-off: WARP and the GPUs the tests run on load typed values */
        tb_free(shader->bytes);
        tb_free(shader);
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "d3d11: a compute shader the device cannot run: it loads from "
                           "writable textures of formats this device does not load (0x%08lx)",
                           (unsigned long)result);
        /* coverage-on */
    }
    if (FAILED(result))
    {
        tb_free(shader->bytes);
        tb_free(shader);
        return tb_d3d11_fail(error, result, "a shader");
    }
    tb_d3d11_name(shader->object, desc->name);
    *object = shader;
    return TB_RHI_OK;
}

void tb_d3d11_shader_destroy(void *state, void *object)
{
    tb_d3d11_shader *const shader = object;

    (void)state;
    (void)ID3D11DeviceChild_Release(shader->object);
    tb_free(shader->bytes);
    tb_free(shader);
}

static D3D11_BLEND tb_d3d11_blend_factor(tb_rhi_blend_factor factor)
{
    static const D3D11_BLEND factors[] = {D3D11_BLEND_ZERO,       D3D11_BLEND_ONE,
                                          D3D11_BLEND_SRC_COLOR,  D3D11_BLEND_INV_SRC_COLOR,
                                          D3D11_BLEND_SRC_ALPHA,  D3D11_BLEND_INV_SRC_ALPHA,
                                          D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
                                          D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_DEST_ALPHA};
    return factors[factor];
}

static D3D11_BLEND_OP tb_d3d11_blend_op(tb_rhi_blend_op op)
{
    /* The values of D3D11 are those of the RHI, one more. */
    return (D3D11_BLEND_OP)((int)op + 1);
}

static D3D11_PRIMITIVE_TOPOLOGY tb_d3d11_topology(tb_rhi_topology topology)
{
    static const D3D11_PRIMITIVE_TOPOLOGY topologies[] = {
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
        D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
        D3D11_PRIMITIVE_TOPOLOGY_POINTLIST};
    return topologies[topology];
}

static HRESULT tb_d3d11_states(tb_d3d11_device *device, const tb_rhi_graphics_pipeline_desc *desc,
                               tb_d3d11_pipeline *made)
{
    D3D11_BLEND_DESC blend = {0};
    D3D11_RASTERIZER_DESC rasterizer = {0};
    D3D11_DEPTH_STENCIL_DESC depth = {0};
    HRESULT result = S_OK;

    blend.IndependentBlendEnable = TRUE;
    for (uint32_t index = 0; index < TB_RHI_COLOR_TARGETS; index++)
    {
        const tb_rhi_blend *const given = &desc->blend[index];
        D3D11_RENDER_TARGET_BLEND_DESC *const target = &blend.RenderTarget[index];
        target->BlendEnable = given->enabled;
        target->SrcBlend = tb_d3d11_blend_factor(given->color_source);
        target->DestBlend = tb_d3d11_blend_factor(given->color_dest);
        target->BlendOp = tb_d3d11_blend_op(given->color_op);
        target->SrcBlendAlpha = tb_d3d11_blend_factor(given->alpha_source);
        target->DestBlendAlpha = tb_d3d11_blend_factor(given->alpha_dest);
        target->BlendOpAlpha = tb_d3d11_blend_op(given->alpha_op);
        target->RenderTargetWriteMask =
            (UINT8)(D3D11_COLOR_WRITE_ENABLE_ALL & ~given->write_mask_off);
    }
    rasterizer.FillMode = desc->wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rasterizer.CullMode = desc->cull == TB_RHI_CULL_BACK    ? D3D11_CULL_BACK
                          : desc->cull == TB_RHI_CULL_FRONT ? D3D11_CULL_FRONT
                                                            : D3D11_CULL_NONE;
    rasterizer.FrontCounterClockwise = desc->clockwise_front ? FALSE : TRUE;
    rasterizer.DepthBias = (INT)desc->depth_bias;
    rasterizer.SlopeScaledDepthBias = desc->depth_bias_slope;
    rasterizer.DepthClipEnable = TRUE;
    /* Every pass sets a scissor of its targets' size, as D3D12's do. */
    rasterizer.ScissorEnable = TRUE;
    rasterizer.MultisampleEnable = desc->samples > 1;
    depth.DepthEnable = desc->depth_test || desc->depth_write;
    depth.DepthWriteMask =
        desc->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc =
        desc->depth_test ? tb_d3d11_compare(desc->depth_compare) : D3D11_COMPARISON_ALWAYS;
    result = ID3D11Device_CreateBlendState(device->device, &blend, &made->blend);
    if (SUCCEEDED(result))
    {
        result = ID3D11Device_CreateRasterizerState(device->device, &rasterizer, &made->rasterizer);
    }
    if (SUCCEEDED(result))
    {
        result = ID3D11Device_CreateDepthStencilState(device->device, &depth, &made->depth);
    }
    return result;
}

tb_rhi_result tb_d3d11_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *pipeline,
                                                void **object, tb_rhi_error *error)
{
    tb_d3d11_device *const device = state;
    const tb_rhi_graphics_pipeline_desc *const desc = pipeline->desc;
    const tb_d3d11_shader *const vertex = pipeline->vertex;
    const tb_d3d11_shader *const pixel = pipeline->pixel;
    D3D11_INPUT_ELEMENT_DESC elements[TB_RHI_VERTEX_ATTRIBUTES] = {0};
    tb_d3d11_pipeline *const made = tb_calloc(1, sizeof(*made));
    HRESULT result = S_OK;

    for (uint32_t index = 0; index < desc->attribute_count; index++)
    {
        const tb_rhi_vertex_attribute *const attribute = &desc->attributes[index];
        const bool instanced = desc->buffers[attribute->buffer].step == TB_RHI_STEP_INSTANCE;
        elements[index].SemanticName = attribute->semantic;
        elements[index].SemanticIndex = attribute->semantic_index;
        elements[index].Format = tb_d3d11_format(attribute->format);
        elements[index].InputSlot = attribute->buffer;
        elements[index].AlignedByteOffset = attribute->offset;
        elements[index].InputSlotClass =
            instanced ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        elements[index].InstanceDataStepRate = instanced ? 1u : 0u;
    }
    made->vertex = (ID3D11VertexShader *)vertex->object;
    (void)ID3D11VertexShader_AddRef(made->vertex);
    made->stages[0] = vertex->read;
    made->stage_count = 1;
    if (pixel != NULL)
    {
        made->pixel = (ID3D11PixelShader *)pixel->object;
        (void)ID3D11PixelShader_AddRef(made->pixel);
        made->stages[1] = pixel->read;
        made->stage_count = 2;
    }
    made->topology = tb_d3d11_topology(desc->topology);
    if (desc->attribute_count > 0)
    {
        result = ID3D11Device_CreateInputLayout(device->device, elements, desc->attribute_count,
                                                vertex->read.code, vertex->read.code_size,
                                                &made->layout);
    }
    if (SUCCEEDED(result))
    {
        result = tb_d3d11_states(device, desc, made);
    }
    tb_d3d11_drain_messages(device);
    if (FAILED(result))
    {
        tb_d3d11_pipeline_destroy(device, made);
        return tb_d3d11_fail(error, result, "a graphics pipeline");
    }
    *object = made;
    return TB_RHI_OK;
}

tb_rhi_result tb_d3d11_compute_pipeline_create(void *state,
                                               const tb_rhi_compute_pipeline_desc *desc,
                                               void *shader, const tb_rhi_shader_desc *shader_desc,
                                               void **object, tb_rhi_error *error)
{
    const tb_d3d11_shader *const compute = shader;
    tb_d3d11_pipeline *const made = tb_calloc(1, sizeof(*made));

    (void)state;
    (void)desc;
    (void)shader_desc;
    (void)error;
    made->compute = true;
    made->compute_shader = (ID3D11ComputeShader *)compute->object;
    (void)ID3D11ComputeShader_AddRef(made->compute_shader);
    made->stages[0] = compute->read;
    made->stage_count = 1;
    *object = made;
    return TB_RHI_OK;
}

void tb_d3d11_pipeline_destroy(void *state, void *object)
{
    tb_d3d11_pipeline *const pipeline = object;

    (void)state;
    if (pipeline->vertex != NULL)
    {
        (void)ID3D11VertexShader_Release(pipeline->vertex);
    }
    if (pipeline->pixel != NULL)
    {
        (void)ID3D11PixelShader_Release(pipeline->pixel);
    }
    if (pipeline->compute_shader != NULL)
    {
        (void)ID3D11ComputeShader_Release(pipeline->compute_shader);
    }
    if (pipeline->layout != NULL)
    {
        (void)ID3D11InputLayout_Release(pipeline->layout);
    }
    if (pipeline->rasterizer != NULL)
    {
        (void)ID3D11RasterizerState_Release(pipeline->rasterizer);
    }
    if (pipeline->depth != NULL)
    {
        (void)ID3D11DepthStencilState_Release(pipeline->depth);
    }
    if (pipeline->blend != NULL)
    {
        (void)ID3D11BlendState_Release(pipeline->blend);
    }
    tb_free(pipeline);
}
