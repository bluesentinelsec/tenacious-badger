/* Allocate the destination in isolation. It gets its own slot tables, native
 * objects and descriptor heaps, so destroying it is the complete rollback. */
#include "rhi_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

typedef struct stage
{
    tb_rhi_device *source;
    tb_rhi_device *destination;
    bool *resource_indices;
    bool *sampler_indices;
} stage;

static bool reserve_index(tb_rhi_index_pool *pool, bool *used, uint32_t index)
{
    if (index == TB_RHI_NO_INDEX)
    {
        return true;
    }
    if (index >= pool->capacity)
    {
        return false;
    }
    used[index] = true;
    if (pool->next <= index)
    {
        pool->next = index + 1;
    }
    return true;
}

static tb_rhi_result stage_slot(stage *work, tb_rhi_kind kind, uint32_t index, uint32_t generation,
                                tb_rhi_error *error)
{
    const tb_rhi_slot *const from = tb_rhi_slot_of(work->source, kind, index, generation);
    tb_rhi_device *const device = work->destination;
    tb_rhi_slot *const to = tb_rhi_table_at(&device->tables[kind], index);
    if (to->alive)
    {
        return TB_RHI_OK;
    }
    if (from->chain_owned)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE, "A swap-chain image requires its window");
    }
    if (kind == TB_RHI_KIND_TEXTURE && (from->desc.texture.width > device->info.max_texture_size ||
                                        from->desc.texture.height > device->info.max_texture_size))
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Texture %s exceeds the destination size limit", from->name);
    }
    tb_rhi_index_pool *const pool =
        kind == TB_RHI_KIND_SAMPLER ? &device->samplers : &device->resources;
    bool *const used = kind == TB_RHI_KIND_SAMPLER ? work->sampler_indices : work->resource_indices;
    const tb_rhi_indices *const indices = &from->indices;
    bool fits =
        reserve_index(pool, used, indices->read) && reserve_index(pool, used, indices->write);
    for (uint32_t i = 0; fits && i < indices->mip_write_count; i++)
    {
        fits = reserve_index(pool, used, indices->mip_writes[i]);
    }
    if (!fits)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "Descriptor index for %s exceeds the destination heap", from->name);
    }
    void *object = NULL;
    const tb_rhi_result result =
        tb_rhi_rebuild_object(device->table, device->state, kind, from, &object, error);
    if (result == TB_RHI_OK)
    {
        *to = *from; /* buffer, texture and sampler descriptions own no pointers */
        to->object = object;
    }
    return result;
}

static void release_unused_indices(tb_rhi_index_pool *pool, const bool *used)
{
    for (uint32_t i = 0; i < pool->next; i++)
    {
        if (!used[i])
        {
            tb_rhi_index_give(pool, i);
        }
    }
}

tb_rhi_result tb_rhi_device_stage_resources(tb_rhi_device *source,
                                            const tb_rhi_device_options *options,
                                            const tb_rhi_resource_set *resources,
                                            tb_rhi_device **staged, tb_rhi_error *error)
{
    TB_ASSERT(source != NULL && options != NULL && resources != NULL && staged != NULL);
    TB_ASSERT(resources->buffer_count == 0 || resources->buffers != NULL);
    TB_ASSERT(resources->texture_count == 0 || resources->textures != NULL);
    TB_ASSERT(resources->sampler_count == 0 || resources->samplers != NULL);
    *staged = NULL;
    stage work = {.source = source};
    tb_rhi_result result = tb_rhi_device_create(options, &work.destination, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    tb_rhi_device *const device = work.destination;
    work.resource_indices = tb_calloc(device->resources.capacity, sizeof(bool));
    work.sampler_indices = tb_calloc(device->samplers.capacity, sizeof(bool));
    /* Preserve generations even for omitted slots. Reusing a hole on the
     * destination must never revive a handle from the snapshot. */
    for (tb_rhi_kind kind = TB_RHI_KIND_BUFFER; kind <= TB_RHI_KIND_SAMPLER; kind++)
    {
        tb_rhi_table *const table = &device->tables[kind];
        for (uint32_t i = 0; i < source->tables[kind].used; i++)
        {
            (void)tb_rhi_table_take(table);
            tb_rhi_table_at(table, i)->generation =
                tb_rhi_table_at(&source->tables[kind], i)->generation;
        }
    }
    for (size_t i = 0; result == TB_RHI_OK && i < resources->buffer_count; i++)
    {
        const tb_rhi_buffer handle = resources->buffers[i];
        result = stage_slot(&work, TB_RHI_KIND_BUFFER, handle.index, handle.generation, error);
    }
    for (size_t i = 0; result == TB_RHI_OK && i < resources->texture_count; i++)
    {
        const tb_rhi_texture handle = resources->textures[i];
        result = stage_slot(&work, TB_RHI_KIND_TEXTURE, handle.index, handle.generation, error);
    }
    for (size_t i = 0; result == TB_RHI_OK && i < resources->sampler_count; i++)
    {
        const tb_rhi_sampler handle = resources->samplers[i];
        result = stage_slot(&work, TB_RHI_KIND_SAMPLER, handle.index, handle.generation, error);
    }
    if (result == TB_RHI_OK)
    {
        for (tb_rhi_kind kind = TB_RHI_KIND_BUFFER; kind <= TB_RHI_KIND_SAMPLER; kind++)
        {
            tb_rhi_table *const table = &device->tables[kind];
            for (uint32_t i = 0; i < table->used; i++)
            {
                if (!tb_rhi_table_at(table, i)->alive)
                {
                    tb_rhi_table_give(table, i);
                }
            }
        }
        release_unused_indices(&device->resources, work.resource_indices);
        release_unused_indices(&device->samplers, work.sampler_indices);
        *staged = device;
    }
    else
    {
        tb_rhi_device_destroy(device);
    }
    tb_free(work.sampler_indices);
    tb_free(work.resource_indices);
    return result;
}
