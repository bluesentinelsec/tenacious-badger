/* Rebuild live objects against a second backend state. Neither the public
 * slots nor the old state change until every allocation and copy succeeds. */
#include "rhi_internal.h"

#include "badger/array.h"
#include "badger/assert.h"

typedef struct replacement
{
    tb_rhi_kind kind;
    tb_rhi_slot *slot;
    void *object;
} replacement;

static tb_rhi_result rebuild_pipeline(const tb_rhi_backend_table *table, void *state,
                                      const tb_rhi_slot *slot, void **object, tb_rhi_error *error)
{
    const tb_rhi_pipeline_data *const data = slot->desc.pipeline;
    tb_rhi_graphics_pipeline_ref ref = {.desc = &data->graphics, .vertex_desc = &data->first->desc};
    tb_rhi_result result = table->shader_create(state, ref.vertex_desc, &ref.vertex, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    if (slot->compute)
    {
        result = table->compute_pipeline_create(state, &data->compute, ref.vertex, ref.vertex_desc,
                                                object, error);
    }
    else
    {
        if (data->pixel != NULL)
        {
            ref.pixel_desc = &data->pixel->desc;
            result = table->shader_create(state, ref.pixel_desc, &ref.pixel, error);
        }
        if (result == TB_RHI_OK)
        {
            result = table->graphics_pipeline_create(state, &ref, object, error);
        }
        if (ref.pixel != NULL)
        {
            table->shader_destroy(state, ref.pixel);
        }
    }
    table->shader_destroy(state, ref.vertex);
    return result;
}

static tb_rhi_result rebuild(const tb_rhi_backend_table *table, void *state, replacement *change,
                             tb_rhi_error *error)
{
    const tb_rhi_slot *const slot = change->slot;
    switch (change->kind)
    {
    case TB_RHI_KIND_BUFFER: {
        tb_rhi_buffer_desc desc = slot->desc.buffer;
        desc.name = slot->name;
        return table->buffer_create(state, &desc, slot->indices, &change->object, error);
    }
    case TB_RHI_KIND_TEXTURE: {
        tb_rhi_texture_desc desc = slot->desc.texture;
        desc.name = slot->name;
        return table->texture_create(state, &desc, slot->indices, &change->object, error);
    }
    case TB_RHI_KIND_SAMPLER: {
        tb_rhi_sampler_desc desc = slot->desc.sampler;
        desc.name = slot->name;
        return table->sampler_create(state, &desc, slot->indices, &change->object, error);
    }
    case TB_RHI_KIND_SHADER:
        return table->shader_create(state, &slot->desc.shader->desc, &change->object, error);
    case TB_RHI_KIND_PIPELINE:
        return rebuild_pipeline(table, state, slot, &change->object, error);
    case TB_RHI_KIND_COUNT:
        break;
    }
    return tb_rhi_fail(error, TB_RHI_SYSTEM_ERROR, "Unknown resource kind");
}

tb_rhi_result tb_rhi_device_recreate(tb_rhi_device *device, tb_rhi_error *error)
{
    TB_ASSERT(device != NULL);
    const tb_rhi_backend_table *const table = device->table;
    if (table->device_replacement == NULL)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "%s does not support same-adapter device recreation",
                           tb_rhi_backend_name(table->backend));
    }
    if (device->open_lists != 0)
    {
        return tb_rhi_fail(
            error, TB_RHI_SYSTEM_ERROR,
            "Device recreation requires all command lists to be submitted or discarded");
    }
    tb_rhi_result result = tb_rhi_wait_idle(device);
    if (result != TB_RHI_OK)
    {
        return tb_rhi_fail(error, result, "Device recreation requires a healthy source device");
    }
    void *fresh = NULL;
    void *discard = NULL;
    result = table->device_replacement(device->state, &device->info, &fresh, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    tb_array *const changes = TB_ARRAY_CREATE(NULL, replacement);
    tb_array *const copies = TB_ARRAY_CREATE(NULL, tb_rhi_device_copy);
    for (tb_rhi_kind kind = TB_RHI_KIND_BUFFER; kind < TB_RHI_KIND_COUNT; kind++)
    {
        const tb_rhi_table *const slots = &device->tables[kind];
        for (uint32_t i = 0; i < slots->used; i++)
        {
            tb_rhi_slot *const slot = tb_rhi_table_at(slots, i);
            if (!slot->alive || slot->chain_owned)
            {
                continue;
            }
            replacement change = {.kind = kind, .slot = slot};
            result = rebuild(table, fresh, &change, error);
            if (result != TB_RHI_OK)
            {
                goto finish;
            }
            (void)tb_array_push(changes, &change);
            if (kind == TB_RHI_KIND_BUFFER || kind == TB_RHI_KIND_TEXTURE)
            {
                const tb_rhi_device_copy copy = {slot->object, change.object,
                                                 kind == TB_RHI_KIND_BUFFER ? slot->desc.buffer.size
                                                                            : 0};
                (void)tb_array_push(copies, &copy);
            }
        }
    }
    if (tb_array_length(copies) > 0)
    {
        result = table->device_copy(fresh, tb_array_data(copies), tb_array_length(copies), error);
    }
finish:
    discard = fresh;
    if (result == TB_RHI_OK)
    {
        discard = device->state;
        device->state = fresh;
        if (error != NULL)
        {
            *error = (tb_rhi_error)TB_RHI_ERROR_INIT;
        }
    }
    for (size_t i = tb_array_length(changes); i > 0; i--)
    {
        const replacement *const change = TB_ARRAY_AT(changes, replacement, i - 1);
        void *object = change->object;
        if (result == TB_RHI_OK)
        {
            object = change->slot->object;
            change->slot->object = change->object;
        }
        tb_rhi_release_object(table, discard, change->kind, object);
    }
    tb_array_destroy(copies);
    tb_array_destroy(changes);
    table->device_destroy(discard);
    return result;
}
