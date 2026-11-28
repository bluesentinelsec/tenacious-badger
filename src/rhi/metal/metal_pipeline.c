#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "metal_internal.h"
#include <string.h>

/* The MSL writer records one line per fixed binding in its source. Read
 * bounded decimal fields even when a shader is supplied by an external game. */
static bool field(const char **cursor, const char *end, uint32_t *value)
{
    const char *p = *cursor;
    if (p == end || *p < '0' || *p > '9')
    {
        return false;
    }
    uint32_t number = 0;
    do
    {
        const uint32_t digit = (uint32_t)(*p - '0');
        if (number > (UINT32_MAX - digit) / 10u)
        {
            return false;
        }
        number = number * 10 + digit;
        p++;
    } while (p != end && *p >= '0' && *p <= '9');
    *value = number;
    *cursor = p;
    return true;
}
bool tb_metal_read_bindings(const tb_rhi_shader_desc *shader, tb_metal_bindings *bindings)
{
    if (shader == NULL)
    {
        return true;
    }
    const char *line = shader->code;
    const char *const end = line + shader->size;
    const char prefix[] = "// tb_bind ";
    while (line < end)
    {
        const char *const newline = memchr(line, '\n', (size_t)(end - line));
        const char *const stop = newline != NULL ? newline : end;
        if ((size_t)(stop - line) >= sizeof(prefix) - 1 &&
            memcmp(line, prefix, sizeof(prefix) - 1) == 0)
        {
            const char *at = line + sizeof(prefix) - 1;
            uint32_t values[4] = {0};
            for (size_t n = 0; n < 4; n++)
            {
                if (!field(&at, stop, &values[n]) || (n < 3 && (at == stop || *at++ != ' ')))
                {
                    return false;
                }
            }
            const uint32_t heap = values[0], buffer = values[1], slot = values[3];
            if (at != stop || bindings->count == TB_MSL_MAX_SLOTS || heap >= TB_MSL_HEAP_COUNT ||
                buffer > TB_RHI_CONSTANT_BUFFER_SLOTS ||
                (heap == TB_MSL_BUFFERS &&
                 (slot < TB_MSL_FIRST_HEAP_SLOT || slot >= TB_MSL_FIRST_VERTEX_SLOT)) ||
                (heap == TB_MSL_SAMPLERS ? slot >= 16u : slot >= 31u))
            {
                return false;
            }
            bindings->slots[bindings->count++] =
                (tb_msl_slot){(tb_msl_heap)heap, buffer, values[2], slot, 0};
        }
        line = newline != NULL ? newline + 1 : end;
    }
    return true;
}
static void *indexed(tb_metal_device *d, void *object, const char *property, unsigned long index)
{
    return TB_METAL_METHOD(d, void *, unsigned long)(
        tb_metal_get(d, object, property), d->selector("objectAtIndexedSubscript:"), index);
}
static bool vertex_layout(tb_metal_device *d, const tb_rhi_graphics_pipeline_ref *ref,
                          void *pipeline)
{
    const tb_rhi_graphics_pipeline_desc *const desc = ref->desc;
    if (desc->attribute_count == 0)
    {
        return true;
    }
    static const unsigned long formats[TB_RHI_FORMAT_COUNT] = {
        [TB_RHI_FORMAT_R8_UNORM] = 47,           [TB_RHI_FORMAT_R8G8_UNORM] = 7,
        [TB_RHI_FORMAT_R8G8B8A8_UNORM] = 9,      [TB_RHI_FORMAT_B8G8R8A8_UNORM] = 42,
        [TB_RHI_FORMAT_R16_FLOAT] = 53,          [TB_RHI_FORMAT_R16G16_FLOAT] = 25,
        [TB_RHI_FORMAT_R16G16B16A16_FLOAT] = 27, [TB_RHI_FORMAT_R16_UINT] = 49,
        [TB_RHI_FORMAT_R32_UINT] = 36,           [TB_RHI_FORMAT_R32_FLOAT] = 28,
        [TB_RHI_FORMAT_R32G32_FLOAT] = 29,       [TB_RHI_FORMAT_R32G32B32_FLOAT] = 30,
        [TB_RHI_FORMAT_R32G32B32A32_FLOAT] = 31};
    void *const vertex = tb_metal_get(d, d->get_class("MTLVertexDescriptor"), "vertexDescriptor");
    void *const inputs = tb_metal_get(d, ref->vertex, "vertexAttributes");
    const unsigned long count =
        ((unsigned long (*)(void *, void *))d->send)(inputs, d->selector("count"));
    for (unsigned long i = 0; i < count; i++)
    {
        void *const input =
            TB_METAL_METHOD(d, void *, unsigned long)(inputs, d->selector("objectAtIndex:"), i);
        const char *const name = tb_metal_get(d, tb_metal_get(d, input, "name"), "UTF8String");
        const unsigned long slot =
            ((unsigned long (*)(void *, void *))d->send)(input, d->selector("attributeIndex"));
        const tb_rhi_vertex_attribute *attr = NULL;
        for (uint32_t n = 0; n < desc->attribute_count; n++)
        {
            char semantic[TB_RHI_NAME_CAPACITY + 16] = {0};
            const tb_rhi_vertex_attribute *const candidate = &desc->attributes[n];
            (void)tb_fmt_buffer(semantic, sizeof(semantic), "a_%s%u", candidate->semantic,
                                candidate->semantic_index);
            if (tb_str_equal(semantic, name))
            {
                attr = candidate;
                break;
            }
            if (candidate->semantic_index == 0 && tb_str_starts_with(name, "a_") &&
                tb_str_equal(name + 2, candidate->semantic))
            {
                attr = candidate;
                break;
            }
        }
        if (!attr || !formats[attr->format])
        {
            return false;
        }
        void *const field = indexed(d, vertex, "attributes", slot);
        tb_metal_number(d, field, "setBufferIndex:", TB_MSL_FIRST_VERTEX_SLOT + attr->buffer);
        tb_metal_number(d, field, "setFormat:", formats[attr->format]);
        tb_metal_number(d, field, "setOffset:", attr->offset);
    }
    for (uint32_t i = 0; i < desc->buffer_count; i++)
    {
        void *const layout = indexed(d, vertex, "layouts", TB_MSL_FIRST_VERTEX_SLOT + i);
        tb_metal_number(d, layout, "setStride:", desc->buffers[i].stride);
        tb_metal_number(d, layout, "setStepFunction:",
                        desc->buffers[i].step == TB_RHI_STEP_INSTANCE ? 2ul : 1ul);
    }
    tb_metal_set(d, pipeline, "setVertexDescriptor:", vertex);
    return true;
}

static void blend_state(tb_metal_device *d, void *target, const tb_rhi_blend *blend)
{
    TB_METAL_METHOD(d, void, bool)(target, d->selector("setBlendingEnabled:"), blend->enabled);
    tb_metal_number(d, target, "setSourceRGBBlendFactor:", blend->color_source);
    tb_metal_number(d, target, "setDestinationRGBBlendFactor:", blend->color_dest);
    tb_metal_number(d, target, "setRgbBlendOperation:", blend->color_op);
    tb_metal_number(d, target, "setSourceAlphaBlendFactor:", blend->alpha_source);
    tb_metal_number(d, target, "setDestinationAlphaBlendFactor:", blend->alpha_dest);
    tb_metal_number(d, target, "setAlphaBlendOperation:", blend->alpha_op);
    unsigned long mask = 0;
    for (unsigned i = 0; i < 4; i++)
    {
        if ((blend->write_mask_off & (1u << i)) == 0)
        {
            mask |= 8ul >> i;
        }
    }
    tb_metal_number(d, target, "setWriteMask:", mask);
}
tb_rhi_result tb_metal_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *ref,
                                                void **object, tb_rhi_error *error)
{
    tb_metal_device *const d = state;
    const tb_rhi_graphics_pipeline_desc *const desc = ref->desc;
    void *const pool = d->pool_push();
    void *const descriptor = tb_metal_get(d, d->get_class("MTLRenderPipelineDescriptor"), "new");
    tb_metal_set(d, descriptor, "setVertexFunction:", ref->vertex);
    tb_metal_set(d, descriptor, "setFragmentFunction:", ref->pixel);
    tb_rhi_result result = TB_RHI_OK;
    if (!vertex_layout(d, ref, descriptor))
    {
        result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                             "Metal vertex layout does not match the vertex shader");
        goto cleanup;
    }
    tb_metal_number(d, descriptor,
                    "setDepthAttachmentPixelFormat:", tb_metal_pixel_format(desc->depth));
    tb_metal_number(d, descriptor, "setRasterSampleCount:", desc->samples ? desc->samples : 1u);
    for (uint32_t index = 0; index < desc->color_count; index++)
    {
        if (tb_metal_pixel_format(desc->colors[index]) == 0)
        {
            result = tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                                 "Metal color format or blend state is unavailable");
            goto cleanup;
        }
        blend_state(d, tb_metal_color(d, descriptor, index), &desc->blend[index]);
        tb_metal_number(d, tb_metal_color(d, descriptor, index),
                        "setPixelFormat:", tb_metal_pixel_format(desc->colors[index]));
    }
    void *native_error = NULL;
    void *const pipeline = TB_METAL_METHOD(d, void *, void *, void **)(
        d->gpu, d->selector("newRenderPipelineStateWithDescriptor:error:"), descriptor,
        &native_error);
    if (pipeline == NULL)
    {
        result = tb_rhi_fail(
            error, TB_RHI_SYSTEM_ERROR, "Metal raster pipeline: %s",
            (const char *)tb_metal_get(d, tb_metal_get(d, native_error, "localizedDescription"),
                                       "UTF8String"));
        goto cleanup;
    }
    static const unsigned long primitives[] = {3, 4, 1, 2, 0};
    tb_metal_pipeline *const made = tb_calloc(1, sizeof(*made));
    made->state = pipeline;
    made->primitive = primitives[desc->topology];
    made->cull = desc->cull == TB_RHI_CULL_BACK ? 2ul : desc->cull == TB_RHI_CULL_FRONT ? 1ul : 0ul;
    made->winding = desc->clockwise_front ? 0ul : 1ul;
    made->wireframe = desc->wireframe;
    made->bias = desc->depth_bias;
    made->slope = desc->depth_bias_slope;
    void *const depth = tb_metal_get(d, d->get_class("MTLDepthStencilDescriptor"), "new");
    tb_metal_number(d, depth, "setDepthCompareFunction:",
                    desc->depth_test ? desc->depth_compare : TB_RHI_COMPARE_ALWAYS);
    TB_METAL_METHOD(d, void, bool)(depth, d->selector("setDepthWriteEnabled:"), desc->depth_write);
    made->depth = TB_METAL_METHOD(d, void *, void *)(
        d->gpu, d->selector("newDepthStencilStateWithDescriptor:"), depth);
    tb_metal_release(d, depth);
    if (!made->depth)
    {
        tb_metal_pipeline_destroy(d, made);
        result = tb_rhi_fail(error, TB_RHI_OUT_OF_MEMORY, "Metal could not allocate depth state");
        goto cleanup;
    }
    if (!d->argument_buffers &&
        (!tb_metal_read_bindings(ref->vertex_desc, &made->stages[TB_RHI_STAGE_VERTEX]) ||
         !tb_metal_read_bindings(ref->pixel_desc, &made->stages[TB_RHI_STAGE_PIXEL])))
    {
        tb_metal_pipeline_destroy(d, made);
        result = tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Invalid Metal raster binding table");
        goto cleanup;
    }
    *object = made;
cleanup:
    tb_metal_release(d, descriptor);
    d->pool_pop(pool);
    return result;
}
void tb_metal_pipeline_destroy(void *state, void *object)
{
    tb_metal_pipeline *const pipeline = object;
    tb_metal_release(state, pipeline->depth);
    tb_metal_release(state, pipeline->state);
    tb_free(pipeline);
}

/* Read the MSL writer's thread group declaration within the shader payload. */
static bool group_size(const tb_rhi_shader_desc *desc, unsigned long group[3])
{
    const char prefix[] = "// tb_numthreads ";
    const char *const source = desc->code;
    size_t at = sizeof(prefix) - 1;
    if (desc->size < at || memcmp(source, prefix, at) != 0)
    {
        return false;
    }
    for (size_t i = 0; i < 3; i++)
    {
        unsigned long n = 0;
        while (at < desc->size && source[at] >= '0' && source[at] <= '9')
        {
            n = n * 10 + (unsigned long)(source[at] - '0');
            if (n > 1024)
            {
                return false;
            }
            at++;
        }
        if (!n || at == desc->size || source[at] != (i == 2 ? '\n' : ' '))
        {
            return false;
        }
        group[i] = n;
        at++;
    }
    return true;
}
tb_rhi_result tb_metal_compute_pipeline_create(void *state,
                                               const tb_rhi_compute_pipeline_desc *desc,
                                               void *shader, const tb_rhi_shader_desc *shader_desc,
                                               void **object, tb_rhi_error *error)
{
    (void)desc;
    tb_metal_device *const d = state;
    unsigned long group[3] = {0};
    if (!group_size(shader_desc, group))
    {
        return tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR,
                           "Metal compute shader lacks valid thread-group dimensions");
    }
    void *const pool = d->pool_push();
    void *native_error = NULL;
    void *const native = TB_METAL_METHOD(d, void *, void *, void **)(
        d->gpu, d->selector("newComputePipelineStateWithFunction:error:"), shader, &native_error);
    tb_rhi_result result = TB_RHI_OK;
    if (!native)
    {
        const char *const why =
            tb_metal_get(d, tb_metal_get(d, native_error, "localizedDescription"), "UTF8String");
        result = tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Metal compute pipeline: %s", why);
    }
    else if (group[0] * group[1] * group[2] >
             ((unsigned long (*)(void *, void *))d->send)(
                 native, d->selector("maxTotalThreadsPerThreadgroup")))
    {
        tb_metal_release(d, native);
        result =
            tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Metal cannot dispatch a group of this size");
    }
    else
    {
        tb_metal_pipeline *const made = tb_calloc(1, sizeof(*made));
        made->state = native;
        memcpy(made->group, group, sizeof(group));
        if (d->argument_buffers ||
            tb_metal_read_bindings(shader_desc, &made->stages[TB_RHI_STAGE_COMPUTE]))
        {
            *object = made;
        }
        else
        {
            tb_metal_pipeline_destroy(d, made);
            result = tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Invalid Metal compute binding table");
        }
    }
    d->pool_pop(pool);
    return result;
}
