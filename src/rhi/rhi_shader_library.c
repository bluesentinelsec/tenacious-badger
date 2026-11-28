#include "badger/rhi_shader_library.h"
#include "badger/assert.h"
#include "rhi_internal.h"

tb_shader_target tb_rhi_backend_shader_target(tb_rhi_backend backend)
{
    if (backend == TB_RHI_BACKEND_D3D12)
    {
        return TB_SHADER_TARGET_DXIL;
    }
    if (backend == TB_RHI_BACKEND_D3D11)
    {
        return TB_SHADER_TARGET_DXBC;
    }
    if (backend == TB_RHI_BACKEND_METAL)
    {
        return TB_SHADER_TARGET_MSL;
    }
    return TB_SHADER_TARGET_NONE;
}

tb_rhi_result tb_rhi_shader_create_compiled(tb_rhi_device *device,
                                            const tb_shader_compile_request *request,
                                            const void *code, size_t size, tb_rhi_shader *shader,
                                            tb_rhi_error *error)
{
    TB_ASSERT(device != NULL && request != NULL && shader != NULL);
    *shader = (tb_rhi_shader){0};
    tb_rhi_shader_desc desc = {
        .name = request->source.path, .entry = request->entry_point, .code = code, .size = size};
    if (request->stage == TB_SHADER_STAGE_VERTEX)
    {
        desc.stage = TB_RHI_STAGE_VERTEX;
    }
    else if (request->stage == TB_SHADER_STAGE_PIXEL)
    {
        desc.stage = TB_RHI_STAGE_PIXEL;
    }
    else if (request->stage == TB_SHADER_STAGE_COMPUTE)
    {
        desc.stage = TB_RHI_STAGE_COMPUTE;
    }
    else
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "This shader stage has no RHI pipeline");
    }
    if (request->target != tb_rhi_backend_shader_target(device->info.backend))
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Shader target does not match %s",
                           tb_rhi_backend_name(device->info.backend));
    }
    const tb_shader_target targets[] = {TB_SHADER_TARGET_DXIL, TB_SHADER_TARGET_DXBC,
                                        TB_SHADER_TARGET_MSL};
    tb_shader_key keys[sizeof(targets) / sizeof(targets[0])] = {0};
    tb_shader_key by_target[TB_SHADER_TARGET_MSL + 1] = {0};
    if (tb_shader_keys_for_targets(request, targets, sizeof(targets) / sizeof(targets[0]), keys) !=
        TB_SHADER_OK)
    {
        return tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR,
                           "Could not identify the compiled shader's source");
    }
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++)
    {
        by_target[targets[i]] = keys[i];
    }
    return tb_rhi_shader_create_with_origin(device, &desc, by_target, shader, error);
}

static tb_rhi_result resolve(const tb_rhi_shader_data *shader, tb_rhi_backend backend,
                             tb_shader_target target, const tb_shader_library *library,
                             tb_rhi_shader_desc *desc, tb_rhi_error *error)
{
    *desc = shader->desc;
    if (shader->origin == NULL)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "Shader %s has no cooked identity",
                           desc->entry);
    }
    desc->name = shader->origin->path;
    if (!tb_shader_library_find(library, &shader->origin->keys[target], &desc->code, &desc->size))
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "%s library lacks %s %s (%s)",
                           tb_rhi_backend_name(backend), shader->origin->path, desc->entry,
                           desc->stage == TB_RHI_STAGE_VERTEX  ? "vertex"
                           : desc->stage == TB_RHI_STAGE_PIXEL ? "pixel"
                                                               : "compute");
    }
    return TB_RHI_OK;
}

tb_rhi_result tb_rhi_pipeline_create_from_library(tb_rhi_device *source, tb_rhi_pipeline pipeline,
                                                  tb_rhi_device *destination,
                                                  const tb_shader_library *library,
                                                  tb_rhi_pipeline *made, tb_rhi_error *error)
{
    TB_ASSERT(source != NULL && destination != NULL && library != NULL && made != NULL);
    *made = (tb_rhi_pipeline){0};
    const tb_rhi_backend backend = destination->info.backend;
    const tb_shader_target target = tb_rhi_backend_shader_target(backend);
    if (target == TB_SHADER_TARGET_NONE)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "%s has no cooked shader target",
                           tb_rhi_backend_name(backend));
    }
    const tb_rhi_slot *const slot =
        tb_rhi_slot_of(source, TB_RHI_KIND_PIPELINE, pipeline.index, pipeline.generation);
    const tb_rhi_pipeline_data *const data = slot->desc.pipeline;
    const tb_rhi_shader_data *const stages[] = {data->first, data->pixel};
    const size_t count = data->pixel == NULL ? 1u : 2u;
    tb_rhi_shader_desc desc[2] = {0};
    tb_rhi_shader shaders[2] = {0};
    tb_rhi_result result = TB_RHI_OK;
    for (size_t i = 0; i < count; i++)
    {
        result = resolve(stages[i], backend, target, library, &desc[i], error);
        if (result != TB_RHI_OK)
        {
            return result;
        }
    }
    for (size_t i = 0; i < count; i++)
    {
        result = tb_rhi_shader_create_with_origin(destination, &desc[i], stages[i]->origin->keys,
                                                  &shaders[i], error);
        if (result != TB_RHI_OK)
        {
            goto done;
        }
    }
    if (slot->compute)
    {
        tb_rhi_compute_pipeline_desc compute = data->compute;
        compute.compute = shaders[0];
        result = tb_rhi_compute_pipeline_create(destination, &compute, made, error);
    }
    else
    {
        tb_rhi_graphics_pipeline_desc graphics = data->graphics;
        graphics.vertex = shaders[0];
        graphics.pixel = shaders[1];
        result = tb_rhi_graphics_pipeline_create(destination, &graphics, made, error);
    }
done:
    tb_rhi_shader_destroy(destination, shaders[1]);
    tb_rhi_shader_destroy(destination, shaders[0]);
    return result;
}
