#include "badger/memory.h"
#include "metal_internal.h"

#include <string.h>

/* MTLPixelFormat values from the Metal ABI. */
unsigned long tb_metal_pixel_format(tb_rhi_format format)
{
    static const unsigned long formats[TB_RHI_FORMAT_COUNT] = {
        [TB_RHI_FORMAT_R8_UNORM] = 10,
        [TB_RHI_FORMAT_R8G8_UNORM] = 30,
        [TB_RHI_FORMAT_R8G8B8A8_UNORM] = 70,
        [TB_RHI_FORMAT_R8G8B8A8_SRGB] = 71,
        [TB_RHI_FORMAT_B8G8R8A8_UNORM] = 80,
        [TB_RHI_FORMAT_B8G8R8A8_SRGB] = 81,
        [TB_RHI_FORMAT_R10G10B10A2_UNORM] = 90,
        [TB_RHI_FORMAT_R11G11B10_FLOAT] = 92,
        [TB_RHI_FORMAT_R16_FLOAT] = 25,
        [TB_RHI_FORMAT_R16G16_FLOAT] = 65,
        [TB_RHI_FORMAT_R16G16B16A16_FLOAT] = 115,
        [TB_RHI_FORMAT_R16_UINT] = 23,
        [TB_RHI_FORMAT_R32_UINT] = 53,
        [TB_RHI_FORMAT_R32_FLOAT] = 55,
        [TB_RHI_FORMAT_R32G32_FLOAT] = 105,
        [TB_RHI_FORMAT_R32G32B32A32_FLOAT] = 125,
        [TB_RHI_FORMAT_D32_FLOAT] = 252};
    return formats[format];
}

tb_rhi_result tb_metal_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                     tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    /* MTLResourceStorageModePrivate for GPU-only, Shared for staging. */
    const unsigned long options = desc->memory == TB_RHI_MEMORY_GPU ? 32ul : 0ul;
    *object = TB_METAL_METHOD(d, void *, unsigned long, unsigned long)(
        d->gpu, d->selector("newBufferWithLength:options:"), desc->size, options);
    if (*object)
    {
        tb_metal_heap_add(d, *object, *object, TB_MSL_BUFFERS, indices.read, 1);
        tb_metal_heap_add(d, *object, *object, TB_MSL_BUFFERS, indices.write, 3);
    }
    return *object != NULL
               ? TB_RHI_OK
               : tb_rhi_fail(error, TB_RHI_OUT_OF_MEMORY, "Metal could not allocate a buffer");
}
void tb_metal_buffer_read(void *state, void *object, uint64_t offset, void *data, size_t size)
{
    const uint8_t *const bytes = tb_metal_get(state, object, "contents");
    memcpy(data, bytes + offset, size);
}
void tb_metal_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                           size_t size)
{
    uint8_t *const bytes = tb_metal_get(state, object, "contents");
    memcpy(bytes + offset, data, size);
}

tb_rhi_result tb_metal_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    const unsigned long format = tb_metal_pixel_format(desc->format);
    if (format == 0 || desc->samples > 1 ||
        (desc->kind == TB_RHI_TEXTURE_CUBE && desc->layers != 6))
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal texture format or shape is unavailable");
    }
    void *const descriptor = tb_metal_get(d, d->get_class("MTLTextureDescriptor"), "new");
    tb_metal_number(d, descriptor, "setTextureType:",
                    desc->kind == TB_RHI_TEXTURE_CUBE ? 5ul
                    : desc->layers > 1                ? 3ul
                                                      : 2ul);
    tb_metal_number(d, descriptor, "setArrayLength:",
                    desc->kind == TB_RHI_TEXTURE_CUBE ? desc->layers / 6 : desc->layers);
    tb_metal_number(d, descriptor, "setMipmapLevelCount:", desc->mips);
    tb_metal_number(d, descriptor, "setWidth:", desc->width);
    tb_metal_number(d, descriptor, "setHeight:", desc->height);
    tb_metal_number(d, descriptor, "setPixelFormat:", format);
    tb_metal_number(d, descriptor, "setStorageMode:", 2);
    /* MTLTextureUsageShaderRead, ShaderWrite, RenderTarget. */
    const unsigned long usage =
        ((desc->usage & TB_RHI_TEXTURE_SAMPLED) ? 1ul : 0ul) |
        ((desc->usage & TB_RHI_TEXTURE_STORAGE) ? 19ul : 0ul) |
        ((desc->usage & (TB_RHI_TEXTURE_RENDER_TARGET | TB_RHI_TEXTURE_DEPTH)) ? 4ul : 0ul);
    tb_metal_number(d, descriptor, "setUsage:", usage);
    *object = TB_METAL_METHOD(d, void *, void *)(d->gpu, d->selector("newTextureWithDescriptor:"),
                                                 descriptor);
    tb_metal_release(d, descriptor);
    if (*object == NULL)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal could not create the requested texture");
    }
    const tb_msl_heap read = desc->kind == TB_RHI_TEXTURE_CUBE ? TB_MSL_CUBES
                             : desc->layers > 1                ? TB_MSL_ARRAY_TEXTURES
                                                               : TB_MSL_TEXTURES;
    tb_metal_heap_add(d, *object, *object, read, indices.read, 1);
    if (indices.write != TB_RHI_NO_INDEX)
    {
        typedef struct metal_range
        {
            unsigned long first, count;
        } metal_range;
        const unsigned long storage = format == 71 ? 70ul : format == 81 ? 80ul : format;
        const bool array = desc->layers > 1;
        for (uint32_t i = 0; i < desc->mips; i++)
        {
            void *const view = TB_METAL_METHOD(d, void *, unsigned long, unsigned long, metal_range,
                                               metal_range)(
                *object, d->selector("newTextureViewWithPixelFormat:textureType:levels:slices:"),
                storage, array ? 3ul : 2ul, (metal_range){i, 1}, (metal_range){0, desc->layers});
            if (!view)
            {
                tb_metal_resource_destroy(d, *object);
                *object = NULL;
                return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                                   "Metal could not create a writable texture level");
            }
            tb_metal_heap_add(d, *object, view,
                              array ? TB_MSL_STORAGE_ARRAYS : TB_MSL_STORAGE_TEXTURES,
                              i == 0 ? indices.write : indices.mip_writes[i - 1], 3);
        }
    }
    return TB_RHI_OK;
}

static const char *error_message(tb_metal_device *d, void *error)
{
    const char *const message =
        tb_metal_get(d, tb_metal_get(d, error, "localizedDescription"), "UTF8String");
    return message != NULL ? message : "no diagnostic";
}

tb_rhi_result tb_metal_shader_create(void *state, const tb_rhi_shader_desc *desc, void **object,
                                     tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    void *const pool = d->pool_push();
    void *native_error = NULL;
    void *const string = d->get_class("NSString");
    void *const source = TB_METAL_METHOD(d, void *, const void *, unsigned long, unsigned long)(
        tb_metal_get(d, string, "alloc"), d->selector("initWithBytes:length:encoding:"), desc->code,
        desc->size, 4ul);
    void *library = NULL;
    tb_rhi_result result = TB_RHI_OK;
    if (source != NULL)
    {
        library = TB_METAL_METHOD(d, void *, void *, void *, void **)(
            d->gpu, d->selector("newLibraryWithSource:options:error:"), source, NULL,
            &native_error);
    }
    if (library == NULL)
    {
        result =
            tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Metal shader compilation: %s",
                        source == NULL ? "source is not UTF-8" : error_message(d, native_error));
    }
    else
    {
        void *const name = TB_METAL_METHOD(d, void *, const char *)(
            string, d->selector("stringWithUTF8String:"), "tb_main");
        *object =
            TB_METAL_METHOD(d, void *, void *)(library, d->selector("newFunctionWithName:"), name);
        if (*object == NULL)
        {
            result = tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Metal shader lacks tb_main");
        }
    }
    tb_metal_release(d, library);
    tb_metal_release(d, source);
    d->pool_pop(pool);
    return result;
}

tb_rhi_result tb_metal_sampler_create(void *state, const tb_rhi_sampler_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    if (desc->mip_bias != 0)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal samplers require shader-side mip bias");
    }
    void *const descriptor = tb_metal_get(d, d->get_class("MTLSamplerDescriptor"), "new");
    const unsigned long address[] = {2, 3, 0,
                                     5}; /* Repeat, MirrorRepeat, ClampToEdge, ClampToBorderColor */
    tb_metal_number(d, descriptor, "setSAddressMode:", address[desc->address_u]);
    tb_metal_number(d, descriptor, "setTAddressMode:", address[desc->address_v]);
    tb_metal_number(d, descriptor, "setRAddressMode:", address[desc->address_w]);
    tb_metal_number(d, descriptor,
                    "setMinFilter:", desc->min_filter == TB_RHI_FILTER_LINEAR ? 1ul : 0ul);
    tb_metal_number(d, descriptor,
                    "setMagFilter:", desc->mag_filter == TB_RHI_FILTER_LINEAR ? 1ul : 0ul);
    tb_metal_number(d, descriptor,
                    "setMipFilter:", desc->mip_filter == TB_RHI_FILTER_LINEAR ? 2ul : 1ul);
    tb_metal_number(d, descriptor,
                    "setMaxAnisotropy:", desc->anisotropy == 0 ? 1ul : desc->anisotropy);
    if (desc->compare)
    {
        tb_metal_number(d, descriptor, "setCompareFunction:", desc->compare_op);
    }
    TB_METAL_METHOD(d, void, bool)(descriptor, d->selector("setSupportArgumentBuffers:"), true);
    *object = TB_METAL_METHOD(d, void *, void *)(
        d->gpu, d->selector("newSamplerStateWithDescriptor:"), descriptor);
    tb_metal_release(d, descriptor);
    if (!*object)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal could not create the requested sampler");
    }
    /* Metal can intern equivalent samplers. Each RHI handle owns its own
     * table entry even when the native sampler objects compare equal. */
    uint32_t *const owner = tb_calloc(1, sizeof(*owner));
    *owner = indices.read;
    tb_metal_heap_add(d, owner, *object, TB_MSL_SAMPLERS, *owner, 0);
    *object = owner;
    return TB_RHI_OK;
}
