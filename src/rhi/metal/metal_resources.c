#include "badger/memory.h"
#include "metal_internal.h"

#include <string.h>

/* MTLPixelFormat values from the Metal ABI. */
static unsigned long pixel_format(tb_rhi_format format)
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
    (void)indices;
    /* MTLResourceStorageModePrivate for GPU-only, Shared for staging. */
    const unsigned long options = desc->memory == TB_RHI_MEMORY_GPU ? 32ul : 0ul;
    *object = TB_METAL_METHOD(d, void *, unsigned long, unsigned long)(
        d->gpu, d->selector("newBufferWithLength:options:"), desc->size, options);
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
    (void)indices;
    const unsigned long format = pixel_format(desc->format);
    if (format == 0 || desc->samples > 1 || desc->kind != TB_RHI_TEXTURE_2D)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal texture format or shape is unavailable");
    }
    void *const descriptor = tb_metal_get(d, d->get_class("MTLTextureDescriptor"), "new");
    tb_metal_number(d, descriptor, "setTextureType:", desc->layers > 1 ? 3ul : 2ul);
    tb_metal_number(d, descriptor, "setArrayLength:", desc->layers);
    tb_metal_number(d, descriptor, "setMipmapLevelCount:", desc->mips);
    tb_metal_number(d, descriptor, "setWidth:", desc->width);
    tb_metal_number(d, descriptor, "setHeight:", desc->height);
    tb_metal_number(d, descriptor, "setPixelFormat:", format);
    tb_metal_number(d, descriptor, "setStorageMode:", 2);
    /* MTLTextureUsageShaderRead, ShaderWrite, RenderTarget. */
    const unsigned long usage =
        ((desc->usage & TB_RHI_TEXTURE_SAMPLED) ? 1ul : 0ul) |
        ((desc->usage & TB_RHI_TEXTURE_STORAGE) ? 3ul : 0ul) |
        ((desc->usage & (TB_RHI_TEXTURE_RENDER_TARGET | TB_RHI_TEXTURE_DEPTH)) ? 4ul : 0ul);
    tb_metal_number(d, descriptor, "setUsage:", usage);
    *object = TB_METAL_METHOD(d, void *, void *)(d->gpu, d->selector("newTextureWithDescriptor:"),
                                                 descriptor);
    tb_metal_release(d, descriptor);
    return *object != NULL ? TB_RHI_OK
                           : tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                                         "Metal could not create the requested texture");
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

tb_rhi_result tb_metal_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *ref,
                                                void **object, tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    const tb_rhi_graphics_pipeline_desc *const desc = ref->desc;
    if (desc->attribute_count || desc->depth != TB_RHI_FORMAT_UNKNOWN || desc->depth_test ||
        desc->depth_write || desc->samples > 1 || desc->wireframe ||
        desc->cull != TB_RHI_CULL_NONE || desc->depth_bias != 0 || desc->depth_bias_slope != 0)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Metal raster pipeline needs unsupported vertex, depth or raster state");
    }
    void *const pool = d->pool_push();
    void *const descriptor = tb_metal_get(d, d->get_class("MTLRenderPipelineDescriptor"), "new");
    tb_metal_set(d, descriptor, "setVertexFunction:", ref->vertex);
    tb_metal_set(d, descriptor, "setFragmentFunction:", ref->pixel);
    tb_rhi_result result = TB_RHI_OK;
    for (uint32_t index = 0; index < desc->color_count; index++)
    {
        if (pixel_format(desc->colors[index]) == 0 || desc->blend[index].enabled ||
            desc->blend[index].write_mask_off)
        {
            result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                                 "Metal color format or blend state is unavailable");
            goto cleanup;
        }
        tb_metal_number(d, tb_metal_color(d, descriptor, index),
                        "setPixelFormat:", pixel_format(desc->colors[index]));
    }
    void *native_error = NULL;
    void *const pipeline = TB_METAL_METHOD(d, void *, void *, void **)(
        d->gpu, d->selector("newRenderPipelineStateWithDescriptor:error:"), descriptor,
        &native_error);
    if (pipeline == NULL)
    {
        result = tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Metal raster pipeline: %s",
                             error_message(d, native_error));
        goto cleanup;
    }
    static const unsigned long primitives[] = {3, 4, 1, 2, 0};
    tb_metal_pipeline *const made = tb_calloc(1, sizeof(*made));
    made->state = pipeline;
    made->primitive = primitives[desc->topology];
    *object = made;
cleanup:
    tb_metal_release(d, descriptor);
    d->pool_pop(pool);
    return result;
}
void tb_metal_pipeline_destroy(void *state, void *object)
{
    tb_metal_pipeline *const pipeline = object;
    tb_metal_release(state, pipeline->state);
    tb_free(pipeline);
}
