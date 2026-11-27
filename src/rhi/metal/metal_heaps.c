#include "metal_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

bool tb_metal_heaps_create(tb_metal_device *d)
{
    d->residents = TB_ARRAY_CREATE(NULL, tb_metal_resident);
    for (size_t i = 0; i < TB_MSL_HEAP_COUNT; i++)
    {
        const size_t count = i == TB_MSL_SAMPLERS ? TB_MSL_SAMPLER_COUNT : TB_MSL_RESOURCE_COUNT;
        d->heaps[i] = TB_METAL_METHOD(d, void *, unsigned long, unsigned long)(
            d->gpu, d->selector("newBufferWithLength:options:"), count * sizeof(uint64_t), 0ul);
        if (d->heaps[i] == NULL)
        {
            return false;
        }
        d->entries[i] = tb_metal_get(d, d->heaps[i], "contents");
    }
    return true;
}

void tb_metal_heaps_destroy(tb_metal_device *d)
{
    if (d->residents != NULL)
    {
        TB_ASSERT(tb_array_length(d->residents) == 0);
        tb_array_destroy(d->residents);
        for (size_t i = 0; i < TB_MSL_HEAP_COUNT; i++)
        {
            tb_metal_release(d, d->heaps[i]);
        }
    }
}

void tb_metal_heap_add(tb_metal_device *d, void *owner, void *resource, tb_msl_heap heap,
                       uint32_t index, unsigned long usage)
{
    if (index == TB_RHI_NO_INDEX)
    {
        return;
    }
    const tb_metal_resident entry = {owner, resource, heap, index, usage};
    tb_mutex_lock(&d->lock);
    /* Tier 1 uses the same table as a constant-time CPU lookup for direct
     * stage bindings. Only Tier 2 binds the table itself to an encoder. */
    uint64_t value = (uint64_t)(uintptr_t)resource;
    if (d->argument_buffers)
    {
        if (heap == TB_MSL_BUFFERS)
        {
            value = ((uint64_t (*)(void *, void *))d->send)(resource, d->selector("gpuAddress"));
        }
        else
        {
            /* MTLResourceID is a struct containing one uint64_t. */
            typedef struct metal_resource_id
            {
                uint64_t value;
            } metal_resource_id;
            value = ((metal_resource_id (*)(void *, void *))d->send)(resource,
                                                                     d->selector("gpuResourceID"))
                        .value;
        }
    }
    d->entries[heap][index] = value;
    (void)tb_array_push(d->residents, &entry);
    tb_mutex_unlock(&d->lock);
}

static void remove_entries(tb_metal_device *d, void *object)
{
    tb_mutex_lock(&d->lock);
    for (size_t i = tb_array_length(d->residents); i > 0; i--)
    {
        const tb_metal_resident *const entry = TB_ARRAY_AT(d->residents, tb_metal_resident, i - 1);
        if (entry->owner == object)
        {
            d->entries[entry->heap][entry->index] = 0;
            if (entry->resource != object)
            {
                tb_metal_release(d, entry->resource);
            }
            tb_array_remove_unordered(d->residents, i - 1, NULL);
        }
    }
    tb_mutex_unlock(&d->lock);
}
void tb_metal_resource_destroy(void *state, void *object)
{
    remove_entries(state, object);
    tb_metal_release(state, object);
}
void tb_metal_sampler_destroy(void *state, void *object)
{
    remove_entries(state, object);
    tb_free(object);
}

/* Called under the device lock while recording direct bindings. */
void *tb_metal_resource_at(tb_metal_device *d, tb_msl_heap heap, uint32_t index)
{
    const uint32_t count = heap == TB_MSL_SAMPLERS ? TB_MSL_SAMPLER_COUNT : TB_MSL_RESOURCE_COUNT;
    return index < count ? (void *)(uintptr_t)d->entries[heap][index] : NULL;
}
