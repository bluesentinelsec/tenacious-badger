#include "metal_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

typedef struct metal_resident
{
    void *owner;
    void *resource;
    tb_msl_heap heap;
    uint32_t index;
    unsigned long usage;
} metal_resident;

bool tb_metal_heaps_create(tb_metal_device *d)
{
    d->residents = TB_ARRAY_CREATE(NULL, metal_resident);
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
    const metal_resident entry = {owner, resource, heap, index, usage};
    uint64_t value = 0;
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
        value =
            ((metal_resource_id (*)(void *, void *))d->send)(resource, d->selector("gpuResourceID"))
                .value;
    }
    tb_mutex_lock(&d->lock);
    d->entries[heap][index] = value;
    (void)tb_array_push(d->residents, &entry);
    tb_mutex_unlock(&d->lock);
}

static void remove_entries(tb_metal_device *d, void *object)
{
    tb_mutex_lock(&d->lock);
    for (size_t i = tb_array_length(d->residents); i > 0; i--)
    {
        const metal_resident *const entry = TB_ARRAY_AT(d->residents, metal_resident, i - 1);
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

void tb_metal_residency(tb_metal_device *d, void *encoder, bool compute)
{
    tb_mutex_lock(&d->lock);
    for (size_t i = 0; i < tb_array_length(d->residents); i++)
    {
        const metal_resident *const entry = TB_ARRAY_AT(d->residents, metal_resident, i);
        if (entry->heap != TB_MSL_SAMPLERS)
        {
            if (compute)
            {
                TB_METAL_METHOD(d, void, void *, unsigned long)(
                    encoder, d->selector("useResource:usage:"), entry->resource, entry->usage);
            }
            else
            {
                TB_METAL_METHOD(d, void, void *, unsigned long,
                                unsigned long)(encoder, d->selector("useResource:usage:stages:"),
                                               entry->resource, entry->usage, 3ul);
            }
        }
    }
    tb_mutex_unlock(&d->lock);
}
