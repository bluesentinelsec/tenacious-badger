/* The RHI's device: backends chosen, handles and descriptions kept, bindless
 * indices given out, and objects released once the GPU is done with them.
 * Commands and submissions are in rhi_commands.c. */
#include "rhi_internal.h"

#include "badger/assert.h"
#include "badger/fatal.h"
#include "badger/fmt.h"
#include "badger/log.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stdarg.h>
#include <string.h>

/* Slots in a chunk of a table. */
#define TB_RHI_CHUNK_SLOTS 256u

#define TB_RHI_DEFAULT_RESOURCES 65536u
#define TB_RHI_DEFAULT_SAMPLERS 2048u
/* What D3D12 allows a sampler heap. */
#define TB_RHI_MAX_SAMPLERS 2048u
/* What D3D12's resource binding tier 3 allows a resource heap. */
#define TB_RHI_MAX_RESOURCES 1000000u

tb_rhi_result tb_rhi_fail(tb_rhi_error *error, tb_rhi_result result, const char *format, ...)
{
    va_list arguments;

    if (error == NULL)
    {
        return result;
    }
    error->result = result;
    va_start(arguments, format);
    (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
    return result;
}

static void tb_rhi_succeed(tb_rhi_error *error)
{
    if (error != NULL)
    {
        *error = (tb_rhi_error)TB_RHI_ERROR_INIT;
    }
}

/* ---------------------------------------------------------------------------
 * Names and formats.
 * ------------------------------------------------------------------------- */

const char *tb_rhi_backend_name(tb_rhi_backend backend)
{
    static const char *const names[TB_RHI_BACKEND_COUNT] = {"auto",   "d3d12", "d3d11",    "vulkan",
                                                            "opengl", "metal", "software", "null"};

    if ((unsigned)backend >= TB_RHI_BACKEND_COUNT)
    {
        return "unknown";
    }
    return names[backend];
}

const char *tb_rhi_result_name(tb_rhi_result result)
{
    static const char *const names[] = {"TB_RHI_OK", "TB_RHI_UNAVAILABLE", "TB_RHI_OUT_OF_MEMORY",
                                        "TB_RHI_DEVICE_LOST", "TB_RHI_SYSTEM_ERROR"};

    if ((unsigned)result >= sizeof(names) / sizeof(names[0]))
    {
        return "unknown";
    }
    return names[result];
}

uint32_t tb_rhi_format_size(tb_rhi_format format)
{
    static const uint8_t sizes[TB_RHI_FORMAT_COUNT] = {[TB_RHI_FORMAT_R8_UNORM] = 1,
                                                       [TB_RHI_FORMAT_R8G8_UNORM] = 2,
                                                       [TB_RHI_FORMAT_R8G8B8A8_UNORM] = 4,
                                                       [TB_RHI_FORMAT_R8G8B8A8_SRGB] = 4,
                                                       [TB_RHI_FORMAT_B8G8R8A8_UNORM] = 4,
                                                       [TB_RHI_FORMAT_B8G8R8A8_SRGB] = 4,
                                                       [TB_RHI_FORMAT_R10G10B10A2_UNORM] = 4,
                                                       [TB_RHI_FORMAT_R11G11B10_FLOAT] = 4,
                                                       [TB_RHI_FORMAT_R16_FLOAT] = 2,
                                                       [TB_RHI_FORMAT_R16G16_FLOAT] = 4,
                                                       [TB_RHI_FORMAT_R16G16B16A16_FLOAT] = 8,
                                                       [TB_RHI_FORMAT_R16_UINT] = 2,
                                                       [TB_RHI_FORMAT_R32_UINT] = 4,
                                                       [TB_RHI_FORMAT_R32_FLOAT] = 4,
                                                       [TB_RHI_FORMAT_R32G32_FLOAT] = 8,
                                                       [TB_RHI_FORMAT_R32G32B32_FLOAT] = 12,
                                                       [TB_RHI_FORMAT_R32G32B32A32_FLOAT] = 16,
                                                       [TB_RHI_FORMAT_D32_FLOAT] = 4,
                                                       [TB_RHI_FORMAT_D24_UNORM_S8_UINT] = 4};

    if ((unsigned)format >= TB_RHI_FORMAT_COUNT)
    {
        return 0;
    }
    return sizes[format];
}

bool tb_rhi_format_is_depth(tb_rhi_format format)
{
    return format == TB_RHI_FORMAT_D32_FLOAT || format == TB_RHI_FORMAT_D24_UNORM_S8_UINT;
}

uint32_t tb_rhi_mip_count(uint32_t width, uint32_t height)
{
    uint32_t larger = width > height ? width : height;
    uint32_t count = 1;

    TB_ASSERT_MSG(width > 0 && height > 0, "a size of some texels");
    while (larger > 1)
    {
        larger >>= 1;
        count++;
    }
    return count;
}

uint32_t tb_rhi_mip_size(uint32_t size, uint32_t mip)
{
    const uint32_t halved = mip >= 32 ? 0u : size >> mip;

    return halved > 0 ? halved : 1u;
}

/* ---------------------------------------------------------------------------
 * Tables of slots, and bindless indices.
 * ------------------------------------------------------------------------- */

tb_rhi_slot *tb_rhi_table_at(const tb_rhi_table *table, uint32_t index)
{
    return &table->chunks[index / TB_RHI_CHUNK_SLOTS][index % TB_RHI_CHUNK_SLOTS];
}

/* A free slot of a table, its generation advanced past the last object's. */
static uint32_t tb_rhi_table_take(tb_rhi_table *table)
{
    uint32_t index = 0;
    tb_rhi_slot *slot = NULL;

    if (table->vacant_count > 0)
    {
        table->vacant_count--;
        index = table->vacant[table->vacant_count];
    }
    else
    {
        if (table->used / TB_RHI_CHUNK_SLOTS == table->chunk_count)
        {
            table->chunks =
                tb_realloc_array(table->chunks, table->chunk_count + 1, sizeof(*table->chunks));
            table->chunks[table->chunk_count] = tb_calloc(TB_RHI_CHUNK_SLOTS, sizeof(tb_rhi_slot));
            table->chunk_count++;
        }
        index = table->used;
        table->used++;
    }
    slot = tb_rhi_table_at(table, index);
    slot->generation++;
    if (slot->generation == 0)
    {
        slot->generation = 1; /* zero names nothing */
    }
    return index;
}

/* Give a slot back: its handle is stale from now on. */
static void tb_rhi_table_give(tb_rhi_table *table, uint32_t index)
{
    tb_rhi_slot *const slot = tb_rhi_table_at(table, index);
    const uint32_t generation = slot->generation;

    *slot = (tb_rhi_slot){0};
    slot->generation = generation;
    if (table->vacant_count == table->vacant_capacity)
    {
        table->vacant_capacity = table->vacant_capacity == 0 ? 16 : table->vacant_capacity * 2;
        table->vacant =
            tb_realloc_array(table->vacant, table->vacant_capacity, sizeof(*table->vacant));
    }
    table->vacant[table->vacant_count] = index;
    table->vacant_count++;
}

static void tb_rhi_table_release(tb_rhi_table *table)
{
    for (size_t chunk = 0; chunk < table->chunk_count; chunk++)
    {
        tb_free(table->chunks[chunk]);
    }
    tb_free(table->chunks);
    tb_free(table->vacant);
    *table = (tb_rhi_table){0};
}

tb_rhi_slot *tb_rhi_slot_of(tb_rhi_device *device, tb_rhi_kind kind, uint32_t index,
                            uint32_t generation)
{
    tb_rhi_slot *slot = NULL;

    tb_mutex_lock(&device->lock);
    if (generation != 0 && index < device->tables[kind].used)
    {
        slot = tb_rhi_table_at(&device->tables[kind], index);
        if (!slot->alive || slot->generation != generation)
        {
            slot = NULL;
        }
    }
    tb_mutex_unlock(&device->lock);
    if (slot == NULL)
    {
        tb_assert_fail("handle names a live object",
                       "an RHI handle is stale, none, or another device's", __FILE__, __LINE__,
                       __func__);
    }
    return slot;
}

static uint32_t tb_rhi_index_take(tb_rhi_index_pool *pool)
{
    if (pool->vacant_count > 0)
    {
        pool->vacant_count--;
        return pool->vacant[pool->vacant_count];
    }
    if (pool->next == pool->capacity)
    {
        return TB_RHI_NO_INDEX;
    }
    pool->next++;
    return pool->next - 1;
}

static void tb_rhi_index_give(tb_rhi_index_pool *pool, uint32_t index)
{
    if (index == TB_RHI_NO_INDEX)
    {
        return;
    }
    /* At most `capacity` indices are out, so the free list never exceeds it. */
    if (pool->vacant == NULL)
    {
        pool->vacant = tb_calloc(pool->capacity, sizeof(*pool->vacant));
    }
    pool->vacant[pool->vacant_count] = index;
    pool->vacant_count++;
}

/* Give back every index an object had. */
static void tb_rhi_indices_give(tb_rhi_index_pool *pool, const tb_rhi_indices *indices)
{
    tb_rhi_index_give(pool, indices->read);
    tb_rhi_index_give(pool, indices->write);
    for (uint32_t mip = 0; mip < indices->mip_write_count; mip++)
    {
        tb_rhi_index_give(pool, indices->mip_writes[mip]);
    }
}

/* Take the indices an object needs: one to read, and one to write each of
 * `writes` levels; or none if a heap is full. */
static tb_rhi_result tb_rhi_indices_take(tb_rhi_index_pool *pool, bool read, uint32_t writes,
                                         tb_rhi_indices *indices, tb_rhi_error *error)
{
    bool full = false;

    TB_ASSERT(writes <= TB_RHI_MAX_MIPS);
    *indices = (tb_rhi_indices){.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX};
    if (read)
    {
        indices->read = tb_rhi_index_take(pool);
        full = indices->read == TB_RHI_NO_INDEX;
    }
    if (writes > 0 && !full)
    {
        indices->write = tb_rhi_index_take(pool);
        full = indices->write == TB_RHI_NO_INDEX;
    }
    for (uint32_t mip = 1; mip < writes && !full; mip++)
    {
        const uint32_t index = tb_rhi_index_take(pool);
        full = index == TB_RHI_NO_INDEX;
        if (!full)
        {
            indices->mip_writes[indices->mip_write_count] = index;
            indices->mip_write_count++;
        }
    }
    if (full)
    {
        tb_rhi_indices_give(pool, indices);
        *indices = (tb_rhi_indices){.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX};
        return tb_rhi_fail(error, TB_RHI_OUT_OF_MEMORY,
                           "every one of the %u bindless indices is in use", pool->capacity);
    }
    return TB_RHI_OK;
}

static tb_rhi_index_pool *tb_rhi_pool_of(tb_rhi_device *device, tb_rhi_kind kind)
{
    return kind == TB_RHI_KIND_SAMPLER ? &device->samplers : &device->resources;
}

/* ---------------------------------------------------------------------------
 * Releasing what the GPU has finished with.
 * ------------------------------------------------------------------------- */

void tb_rhi_release_object(const tb_rhi_backend_table *table, void *state, tb_rhi_kind kind,
                           void *object)
{
    switch (kind)
    {
    case TB_RHI_KIND_BUFFER:
        table->buffer_destroy(state, object);
        break;
    case TB_RHI_KIND_TEXTURE:
        table->texture_destroy(state, object);
        break;
    case TB_RHI_KIND_SAMPLER:
        table->sampler_destroy(state, object);
        break;
    case TB_RHI_KIND_SHADER:
        table->shader_destroy(state, object);
        break;
    case TB_RHI_KIND_PIPELINE:
        table->pipeline_destroy(state, object);
        break;
    case TB_RHI_KIND_COUNT:
        TB_ASSERT_MSG(false, "not a kind of object");
        break;
    }
}

void tb_rhi_collect(tb_rhi_device *device, uint64_t completed)
{
    size_t kept = 0;

    for (size_t index = 0; index < device->retired_count; index++)
    {
        const tb_rhi_retired retired = device->retired[index];
        if (retired.after <= completed)
        {
            tb_rhi_release_object(device->table, device->state, retired.kind, retired.object);
            tb_rhi_indices_give(tb_rhi_pool_of(device, retired.kind), &retired.indices);
        }
        else
        {
            device->retired[kept] = retired;
            kept++;
        }
    }
    device->retired_count = kept;
}

static void tb_rhi_release_shader(tb_rhi_shader_data *shader)
{
    TB_ASSERT(shader->references > 0);
    shader->references--;
    if (shader->references == 0)
    {
        if (shader->origin != NULL)
        {
            tb_str_free(NULL, shader->origin->path);
            tb_free(shader->origin);
        }
        tb_free(shader->code);
        tb_free(shader);
    }
}

static void tb_rhi_release_description(tb_rhi_kind kind, tb_rhi_slot *slot)
{
    if (kind == TB_RHI_KIND_SHADER)
    {
        tb_rhi_release_shader(slot->desc.shader);
    }
    else if (kind == TB_RHI_KIND_PIPELINE)
    {
        tb_rhi_pipeline_data *const data = slot->desc.pipeline;
        for (uint32_t i = 0; i < data->graphics.attribute_count; i++)
        {
            tb_str_free(NULL, (char *)(uintptr_t)data->attributes[i].semantic);
        }
        tb_rhi_release_shader(data->first);
        if (data->pixel != NULL)
        {
            tb_rhi_release_shader(data->pixel);
        }
        tb_free(data->attributes);
        tb_free(data);
    }
}

/* Take a handle away now, and the object once the GPU is done with it. */
static void tb_rhi_retire(tb_rhi_device *device, tb_rhi_kind kind, uint32_t index,
                          uint32_t generation)
{
    tb_rhi_slot *const slot = tb_rhi_slot_of(device, kind, index, generation);

    tb_mutex_lock(&device->lock);
    TB_ASSERT_MSG(!slot->chain_owned, "a swap chain's image is the swap chain's to destroy");
    if (device->retired_count == device->retired_capacity)
    {
        device->retired_capacity =
            device->retired_capacity == 0 ? 16 : device->retired_capacity * 2;
        device->retired =
            tb_realloc_array(device->retired, device->retired_capacity, sizeof(*device->retired));
    }
    device->retired[device->retired_count] = (tb_rhi_retired){
        .kind = kind, .object = slot->object, .indices = slot->indices, .after = device->submitted};
    device->retired_count++;
    tb_rhi_release_description(kind, slot);
    tb_rhi_table_give(&device->tables[kind], index);
    if (!device->lost)
    {
        tb_rhi_collect(device, device->table->completed(device->state));
    }
    tb_mutex_unlock(&device->lock);
}

/* ---------------------------------------------------------------------------
 * Devices.
 * ------------------------------------------------------------------------- */

tb_rhi_result tb_rhi_device_create_on(const tb_rhi_backend_table *table,
                                      const tb_rhi_device_options *options, tb_rhi_device **device,
                                      tb_rhi_error *error)
{
    tb_rhi_device *made = NULL;
    tb_rhi_device_info info = {0};
    void *state = NULL;
    tb_rhi_result result = TB_RHI_OK;

    *device = NULL;
    info.backend = table->backend;
    info.bindless_resources =
        options->bindless_resources == 0 ? TB_RHI_DEFAULT_RESOURCES : options->bindless_resources;
    info.bindless_samplers =
        options->bindless_samplers == 0 ? TB_RHI_DEFAULT_SAMPLERS : options->bindless_samplers;
    TB_ASSERT_MSG(info.bindless_resources <= TB_RHI_MAX_RESOURCES, "too many bindless resources");
    TB_ASSERT_MSG(info.bindless_samplers <= TB_RHI_MAX_SAMPLERS, "too many bindless samplers");
    result = table->device_create(options, &state, &info, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    made = tb_calloc(1, sizeof(*made));
    made->table = table;
    made->state = state;
    made->info = info;
    made->resources.capacity = info.bindless_resources;
    made->samplers.capacity = info.bindless_samplers;
    *device = made;
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

/* The next step that "not implemented" names, for a setting and for a flag. */
#define TB_RHI_NEXT_STEP "set graphics.renderer to auto, or run with --renderer=auto"

/* Write to the options' log, where there is one. */
static void tb_rhi_log(const tb_rhi_device_options *options, tb_log_level level, const char *format,
                       ...)
{
    va_list arguments;

    if (options->log == NULL)
    {
        return;
    }
    va_start(arguments, format);
    (void)tb_log_vprintf(options->log, level, "rhi", format, arguments);
    va_end(arguments);
}

/* The entry of a list for a backend, or NULL where the list has none. */
static const tb_rhi_backend_entry *tb_rhi_entry_of(const tb_rhi_backend_list *list,
                                                   tb_rhi_backend backend)
{
    for (size_t index = 0; index < list->count; index++)
    {
        if (list->entries[index].backend == backend)
        {
            return &list->entries[index];
        }
    }
    return NULL;
}

/* A backend chosen by name that the platform does not have: "not
 * implemented", with the next step, which ends the process. */
static _Noreturn void tb_rhi_not_implemented(const tb_rhi_backend_list *list,
                                             const tb_rhi_device_options *options,
                                             const tb_rhi_backend_entry *entry)
{
    const char *const name = tb_rhi_backend_name(options->backend);
    const char *const why = entry == NULL ? "which does not build it" : "not written yet";

    tb_rhi_log(options, TB_LOG_FATAL,
               "tb_rhi_device_create: the %s backend is not implemented on %s, %s; %s", name,
               list->platform, why, TB_RHI_NEXT_STEP);
    tb_fatal("rhi: tb_rhi_device_create: the %s backend is not implemented on %s, %s; %s", name,
             list->platform, why, TB_RHI_NEXT_STEP);
}

/* Make a device on a backend, saying in the log what came of it. */
static tb_rhi_result tb_rhi_try(const tb_rhi_backend_table *table,
                                const tb_rhi_device_options *options, tb_rhi_device **device,
                                tb_rhi_error *error)
{
    const tb_rhi_result result = tb_rhi_device_create_on(table, options, device, error);

    if (result == TB_RHI_OK)
    {
        tb_rhi_log(options, TB_LOG_INFO, "using %s on %s, as graphics.renderer %s chose",
                   tb_rhi_backend_name(table->backend), (*device)->info.adapter,
                   tb_rhi_backend_name(options->backend));
        return TB_RHI_OK;
    }
    tb_rhi_log(options, TB_LOG_WARNING, "%s is unavailable: %s",
               tb_rhi_backend_name(table->backend), error->message);
    return result;
}

/* `auto`: each backend of the list in turn, past any not written yet or
 * unavailable, saying why for each. Never the null backend. */
static tb_rhi_result tb_rhi_choose(const tb_rhi_backend_list *list,
                                   const tb_rhi_device_options *options, tb_rhi_device **device,
                                   tb_rhi_error *error)
{
    for (size_t index = 0; index < list->count; index++)
    {
        const tb_rhi_backend_entry *const entry = &list->entries[index];
        if (entry->table == NULL)
        {
            tb_rhi_log(options, TB_LOG_INFO, "auto passed over %s: not implemented on %s yet",
                       tb_rhi_backend_name(entry->backend), list->platform);
            continue;
        }
        if (tb_rhi_try(entry->table, options, device, error) == TB_RHI_OK)
        {
            return TB_RHI_OK;
        }
    }
    tb_rhi_log(options, TB_LOG_ERROR, "auto found no backend of %s that can make a device",
               list->platform);
    return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                       "no backend of %s can make a device; the log says why of each",
                       list->platform);
}

tb_rhi_result tb_rhi_device_create_from(const tb_rhi_backend_list *list,
                                        const tb_rhi_device_options *options,
                                        tb_rhi_device **device, tb_rhi_error *error)
{
    tb_rhi_error attempt = TB_RHI_ERROR_INIT;
    const tb_rhi_backend_entry *entry = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(list != NULL && options != NULL && device != NULL);
    TB_ASSERT_MSG((unsigned)options->backend < TB_RHI_BACKEND_COUNT, "not a backend");
    *device = NULL;
    if (options->backend == TB_RHI_BACKEND_AUTOMATIC)
    {
        result = tb_rhi_choose(list, options, device, &attempt);
    }
    else if (options->backend == TB_RHI_BACKEND_NULL)
    {
        result = tb_rhi_try(&tb_rhi_null_backend, options, device, &attempt);
    }
    else
    {
        entry = tb_rhi_entry_of(list, options->backend);
        if (entry == NULL || entry->table == NULL)
        {
            tb_rhi_not_implemented(list, options, entry);
        }
        result = tb_rhi_try(entry->table, options, device, &attempt);
    }
    if (error != NULL)
    {
        *error = attempt;
    }
    return result;
}

tb_rhi_result tb_rhi_device_create(const tb_rhi_device_options *options, tb_rhi_device **device,
                                   tb_rhi_error *error)
{
    return tb_rhi_device_create_from(tb_rhi_platform_backends(), options, device, error);
}

void tb_rhi_device_destroy(tb_rhi_device *device)
{
    if (device == NULL)
    {
        return;
    }
    TB_ASSERT_MSG(device->open_lists == 0, "a command list is still recording");
    if (!device->lost)
    {
        (void)device->table->wait(device->state, device->submitted);
    }
    tb_mutex_lock(&device->lock);
    tb_rhi_collect(device, device->submitted);
    for (int kind = 0; kind < TB_RHI_KIND_COUNT; kind++)
    {
        tb_rhi_table *const table = &device->tables[kind];
        for (uint32_t index = 0; index < table->used; index++)
        {
            tb_rhi_slot *const slot = tb_rhi_table_at(table, index);
            TB_ASSERT_MSG(!slot->alive || !slot->chain_owned, "a swap chain outlives its device");
            if (slot->alive)
            {
                tb_rhi_release_object(device->table, device->state, (tb_rhi_kind)kind,
                                      slot->object);
                tb_rhi_release_description((tb_rhi_kind)kind, slot);
            }
        }
        tb_rhi_table_release(table);
    }
    tb_mutex_unlock(&device->lock);
    device->table->device_destroy(device->state);
    tb_free(device->resources.vacant);
    tb_free(device->samplers.vacant);
    tb_free(device->retired);
    tb_free(device);
}

void tb_rhi_device_info_of(const tb_rhi_device *device, tb_rhi_device_info *info)
{
    TB_ASSERT(device != NULL && info != NULL);
    *info = device->info;
}

/* ---------------------------------------------------------------------------
 * Objects.
 * ------------------------------------------------------------------------- */

static void tb_rhi_copy_name(char *name, const char *given)
{
    if (given != NULL)
    {
        (void)tb_fmt_buffer(name, TB_RHI_NAME_CAPACITY, "%s", given);
    }
}

/* A new slot for an object the backend made. */
static uint32_t tb_rhi_add(tb_rhi_device *device, tb_rhi_kind kind, void *object,
                           tb_rhi_indices indices, const char *name, tb_rhi_slot **made)
{
    uint32_t index = 0;
    tb_rhi_slot *slot = NULL;

    index = tb_rhi_table_take(&device->tables[kind]);
    slot = tb_rhi_table_at(&device->tables[kind], index);
    slot->alive = true;
    slot->object = object;
    slot->indices = indices;
    tb_rhi_copy_name(slot->name, name);
    *made = slot;
    return index;
}

tb_rhi_result tb_rhi_buffer_create(tb_rhi_device *device, const tb_rhi_buffer_desc *desc,
                                   tb_rhi_buffer *buffer, tb_rhi_error *error)
{
    tb_rhi_indices indices = {0};
    void *object = NULL;
    tb_rhi_slot *slot = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && desc != NULL && buffer != NULL);
    TB_ASSERT_MSG(desc->size > 0, "a buffer of no bytes");
    *buffer = (tb_rhi_buffer){0};
    tb_mutex_lock(&device->lock);
    result =
        tb_rhi_indices_take(&device->resources, (desc->usage & TB_RHI_BUFFER_SHADER_READ) != 0,
                            (desc->usage & TB_RHI_BUFFER_STORAGE) != 0 ? 1u : 0u, &indices, error);
    tb_mutex_unlock(&device->lock);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    result = device->table->buffer_create(device->state, desc, indices, &object, error);
    tb_mutex_lock(&device->lock);
    if (result != TB_RHI_OK)
    {
        tb_rhi_indices_give(&device->resources, &indices);
        tb_mutex_unlock(&device->lock);
        return result;
    }
    buffer->index = tb_rhi_add(device, TB_RHI_KIND_BUFFER, object, indices, desc->name, &slot);
    buffer->generation = slot->generation;
    slot->desc.buffer = *desc;
    slot->desc.buffer.name = NULL;
    tb_mutex_unlock(&device->lock);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

void tb_rhi_buffer_destroy(tb_rhi_device *device, tb_rhi_buffer buffer)
{
    TB_ASSERT(device != NULL);
    if (buffer.generation != 0)
    {
        tb_rhi_retire(device, TB_RHI_KIND_BUFFER, buffer.index, buffer.generation);
    }
}

static const tb_rhi_slot *tb_rhi_buffer_slot(tb_rhi_device *device, tb_rhi_buffer buffer)
{
    return tb_rhi_slot_of(device, TB_RHI_KIND_BUFFER, buffer.index, buffer.generation);
}

void tb_rhi_buffer_write(tb_rhi_device *device, tb_rhi_buffer buffer, uint64_t offset,
                         const void *data, size_t size)
{
    const tb_rhi_slot *const slot = tb_rhi_buffer_slot(device, buffer);

    TB_ASSERT(data != NULL);
    TB_ASSERT_MSG(slot->desc.buffer.memory == TB_RHI_MEMORY_UPLOAD,
                  "writing a buffer not of upload memory");
    TB_ASSERT_MSG(offset <= slot->desc.buffer.size && size <= slot->desc.buffer.size - offset,
                  "writing past a buffer's end");
    device->table->buffer_write(device->state, slot->object, offset, data, size);
}

void tb_rhi_buffer_read(tb_rhi_device *device, tb_rhi_buffer buffer, uint64_t offset, void *data,
                        size_t size)
{
    const tb_rhi_slot *const slot = tb_rhi_buffer_slot(device, buffer);

    TB_ASSERT(data != NULL);
    TB_ASSERT_MSG(slot->desc.buffer.memory == TB_RHI_MEMORY_READBACK,
                  "reading a buffer not of readback memory");
    TB_ASSERT_MSG(offset <= slot->desc.buffer.size && size <= slot->desc.buffer.size - offset,
                  "reading past a buffer's end");
    device->table->buffer_read(device->state, slot->object, offset, data, size);
}

void tb_rhi_buffer_desc_of(tb_rhi_device *device, tb_rhi_buffer buffer, tb_rhi_buffer_desc *desc)
{
    TB_ASSERT(desc != NULL);
    *desc = tb_rhi_buffer_slot(device, buffer)->desc.buffer;
}

uint32_t tb_rhi_buffer_index(tb_rhi_device *device, tb_rhi_buffer buffer)
{
    return tb_rhi_buffer_slot(device, buffer)->indices.read;
}

uint32_t tb_rhi_buffer_storage_index(tb_rhi_device *device, tb_rhi_buffer buffer)
{
    return tb_rhi_buffer_slot(device, buffer)->indices.write;
}

/* A texture's description with its defaults made explicit. */
static tb_rhi_texture_desc tb_rhi_texture_defaults(const tb_rhi_texture_desc *desc)
{
    tb_rhi_texture_desc filled = *desc;

    if (filled.layers == 0)
    {
        filled.layers = filled.kind == TB_RHI_TEXTURE_CUBE ? 6u : 1u;
    }
    filled.mips = filled.mips == 0 ? 1u : filled.mips;
    filled.samples = filled.samples == 0 ? 1u : filled.samples;
    filled.name = NULL;
    return filled;
}

static void tb_rhi_texture_check(const tb_rhi_texture_desc *desc)
{
    TB_ASSERT_MSG(desc->width > 0 && desc->height > 0, "a texture of no texels");
    TB_ASSERT_MSG(desc->format != TB_RHI_FORMAT_UNKNOWN && desc->format < TB_RHI_FORMAT_COUNT,
                  "a texture of no format");
    TB_ASSERT_MSG(desc->kind != TB_RHI_TEXTURE_CUBE ||
                      (desc->width == desc->height && desc->layers % 6 == 0),
                  "a cube's faces are square, six to a cube");
    TB_ASSERT_MSG(((desc->usage & TB_RHI_TEXTURE_DEPTH) != 0) ==
                      tb_rhi_format_is_depth(desc->format),
                  "a depth target has a depth format, and only it");
    TB_ASSERT_MSG(desc->mips <= TB_RHI_MAX_MIPS &&
                      desc->mips <= tb_rhi_mip_count(desc->width, desc->height),
                  "more levels than the texture has");
}

tb_rhi_result tb_rhi_texture_create(tb_rhi_device *device, const tb_rhi_texture_desc *desc,
                                    tb_rhi_texture *texture, tb_rhi_error *error)
{
    tb_rhi_texture_desc filled = {0};
    tb_rhi_indices indices = {0};
    void *object = NULL;
    tb_rhi_slot *slot = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && desc != NULL && texture != NULL);
    *texture = (tb_rhi_texture){0};
    filled = tb_rhi_texture_defaults(desc);
    tb_rhi_texture_check(&filled);
    if (filled.width > device->info.max_texture_size ||
        filled.height > device->info.max_texture_size)
    {
        return tb_rhi_fail(error, TB_RHI_UNAVAILABLE,
                           "a texture of %ux%u is larger than the %u this device allows",
                           filled.width, filled.height, device->info.max_texture_size);
    }
    tb_mutex_lock(&device->lock);
    result = tb_rhi_indices_take(&device->resources, (filled.usage & TB_RHI_TEXTURE_SAMPLED) != 0,
                                 (filled.usage & TB_RHI_TEXTURE_STORAGE) != 0 ? filled.mips : 0u,
                                 &indices, error);
    tb_mutex_unlock(&device->lock);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    filled.name = desc->name;
    result = device->table->texture_create(device->state, &filled, indices, &object, error);
    filled.name = NULL;
    tb_mutex_lock(&device->lock);
    if (result != TB_RHI_OK)
    {
        tb_rhi_indices_give(&device->resources, &indices);
        tb_mutex_unlock(&device->lock);
        return result;
    }
    texture->index = tb_rhi_add(device, TB_RHI_KIND_TEXTURE, object, indices, desc->name, &slot);
    texture->generation = slot->generation;
    slot->desc.texture = filled;
    tb_mutex_unlock(&device->lock);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

void tb_rhi_texture_destroy(tb_rhi_device *device, tb_rhi_texture texture)
{
    TB_ASSERT(device != NULL);
    if (texture.generation != 0)
    {
        tb_rhi_retire(device, TB_RHI_KIND_TEXTURE, texture.index, texture.generation);
    }
}

void tb_rhi_texture_desc_of(tb_rhi_device *device, tb_rhi_texture texture,
                            tb_rhi_texture_desc *desc)
{
    TB_ASSERT(desc != NULL);
    *desc = tb_rhi_slot_of(device, TB_RHI_KIND_TEXTURE, texture.index, texture.generation)
                ->desc.texture;
}

uint32_t tb_rhi_texture_index(tb_rhi_device *device, tb_rhi_texture texture)
{
    return tb_rhi_slot_of(device, TB_RHI_KIND_TEXTURE, texture.index, texture.generation)
        ->indices.read;
}

uint32_t tb_rhi_texture_storage_index(tb_rhi_device *device, tb_rhi_texture texture)
{
    return tb_rhi_slot_of(device, TB_RHI_KIND_TEXTURE, texture.index, texture.generation)
        ->indices.write;
}

uint32_t tb_rhi_texture_storage_mip_index(tb_rhi_device *device, tb_rhi_texture texture,
                                          uint32_t mip)
{
    const tb_rhi_slot *const slot =
        tb_rhi_slot_of(device, TB_RHI_KIND_TEXTURE, texture.index, texture.generation);

    TB_ASSERT_MSG(mip < slot->desc.texture.mips, "a level the texture has");
    if (mip == 0 || slot->indices.write == TB_RHI_NO_INDEX)
    {
        return slot->indices.write;
    }
    return slot->indices.mip_writes[mip - 1];
}

tb_rhi_result tb_rhi_sampler_create(tb_rhi_device *device, const tb_rhi_sampler_desc *desc,
                                    tb_rhi_sampler *sampler, tb_rhi_error *error)
{
    tb_rhi_indices indices = {0};
    void *object = NULL;
    tb_rhi_slot *slot = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && desc != NULL && sampler != NULL);
    TB_ASSERT_MSG(desc->anisotropy <= 16, "anisotropy above 16");
    *sampler = (tb_rhi_sampler){0};
    tb_mutex_lock(&device->lock);
    result = tb_rhi_indices_take(&device->samplers, true, 0, &indices, error);
    tb_mutex_unlock(&device->lock);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    result = device->table->sampler_create(device->state, desc, indices, &object, error);
    tb_mutex_lock(&device->lock);
    if (result != TB_RHI_OK)
    {
        tb_rhi_index_give(&device->samplers, indices.read);
        tb_mutex_unlock(&device->lock);
        return result;
    }
    sampler->index = tb_rhi_add(device, TB_RHI_KIND_SAMPLER, object, indices, desc->name, &slot);
    sampler->generation = slot->generation;
    slot->desc.sampler = *desc;
    slot->desc.sampler.name = NULL;
    tb_mutex_unlock(&device->lock);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

void tb_rhi_sampler_destroy(tb_rhi_device *device, tb_rhi_sampler sampler)
{
    TB_ASSERT(device != NULL);
    if (sampler.generation != 0)
    {
        tb_rhi_retire(device, TB_RHI_KIND_SAMPLER, sampler.index, sampler.generation);
    }
}

uint32_t tb_rhi_sampler_index(tb_rhi_device *device, tb_rhi_sampler sampler)
{
    return tb_rhi_slot_of(device, TB_RHI_KIND_SAMPLER, sampler.index, sampler.generation)
        ->indices.read;
}

tb_rhi_result tb_rhi_shader_create(tb_rhi_device *device, const tb_rhi_shader_desc *desc,
                                   tb_rhi_shader *shader, tb_rhi_error *error)
{
    return tb_rhi_shader_create_with_origin(device, desc, NULL, shader, error);
}

tb_rhi_result tb_rhi_shader_create_with_origin(tb_rhi_device *device,
                                               const tb_rhi_shader_desc *desc,
                                               const tb_shader_key *keys, tb_rhi_shader *shader,
                                               tb_rhi_error *error)
{
    const tb_rhi_indices none = {.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX};
    tb_rhi_shader_desc kept = {0};
    char entry[TB_RHI_ENTRY_CAPACITY] = {0};
    tb_rhi_shader_data *data = NULL;
    void *object = NULL;
    tb_rhi_slot *slot = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && desc != NULL && shader != NULL);
    TB_ASSERT_MSG(desc->code != NULL && desc->size > 0, "a shader of no bytes");
    TB_ASSERT_MSG(desc->entry == NULL || tb_str_length(desc->entry) < TB_RHI_ENTRY_CAPACITY,
                  "an entry point's name too long");
    *shader = (tb_rhi_shader){0};
    (void)tb_fmt_buffer(entry, sizeof(entry), "%s", desc->entry == NULL ? "main" : desc->entry);
    kept = *desc;
    kept.entry = entry;
    result = device->table->shader_create(device->state, &kept, &object, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    data = TB_NEW(tb_rhi_shader_data);
    data->code = tb_malloc(desc->size);
    data->references = 1;
    if (keys != NULL)
    {
        data->origin = TB_NEW(tb_rhi_shader_origin);
        memcpy(data->origin->keys, keys, sizeof(data->origin->keys));
        data->origin->path = tb_str_dup(NULL, desc->name);
    }
    data->desc = kept;
    memcpy(data->code, desc->code, desc->size);
    memcpy(data->entry, entry, sizeof(entry));
    data->desc.code = data->code;
    data->desc.entry = data->entry;

    tb_rhi_copy_name(data->name, desc->name);
    data->desc.name = desc->name != NULL ? data->name : NULL;
    tb_mutex_lock(&device->lock);
    shader->index = tb_rhi_add(device, TB_RHI_KIND_SHADER, object, none, desc->name, &slot);
    shader->generation = slot->generation;
    slot->desc.shader = data;
    tb_mutex_unlock(&device->lock);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

void tb_rhi_shader_destroy(tb_rhi_device *device, tb_rhi_shader shader)
{
    TB_ASSERT(device != NULL);
    if (shader.generation != 0)
    {
        tb_rhi_retire(device, TB_RHI_KIND_SHADER, shader.index, shader.generation);
    }
}

static const tb_rhi_slot *tb_rhi_shader_slot(tb_rhi_device *device, tb_rhi_shader shader,
                                             tb_rhi_stage stage)
{
    const tb_rhi_slot *const slot =
        tb_rhi_slot_of(device, TB_RHI_KIND_SHADER, shader.index, shader.generation);
    TB_ASSERT_MSG(slot->desc.shader->desc.stage == stage, "a shader of another stage");
    return slot;
}

/* Called under the device lock. */
static tb_rhi_shader_data *tb_rhi_keep_shader(const tb_rhi_slot *slot)
{
    if (slot == NULL)
    {
        return NULL;
    }
    tb_rhi_shader_data *const data = slot->desc.shader;
    TB_ASSERT(data->references < SIZE_MAX);
    data->references++;
    return data;
}

tb_rhi_result tb_rhi_graphics_pipeline_create(tb_rhi_device *device,
                                              const tb_rhi_graphics_pipeline_desc *desc,
                                              tb_rhi_pipeline *pipeline, tb_rhi_error *error)
{
    const tb_rhi_indices none = {.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX};
    tb_rhi_graphics_pipeline_ref ref = {0};
    const tb_rhi_slot *vertex = NULL;
    const tb_rhi_slot *pixel = NULL;
    void *object = NULL;
    tb_rhi_slot *slot = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && desc != NULL && pipeline != NULL);
    TB_ASSERT_MSG(desc->attribute_count <= TB_RHI_VERTEX_ATTRIBUTES, "too many vertex attributes");
    TB_ASSERT_MSG(desc->attribute_count == 0 || desc->attributes != NULL, "attributes missing");
    TB_ASSERT_MSG(desc->buffer_count <= TB_RHI_VERTEX_BUFFERS, "too many vertex buffers");
    TB_ASSERT_MSG(desc->color_count <= TB_RHI_COLOR_TARGETS, "too many color targets");
    for (uint32_t index = 0; index < desc->attribute_count; index++)
    {
        TB_ASSERT_MSG(desc->attributes[index].buffer < desc->buffer_count,
                      "an attribute of a vertex buffer the pipeline lacks");
    }
    *pipeline = (tb_rhi_pipeline){0};
    vertex = tb_rhi_shader_slot(device, desc->vertex, TB_RHI_STAGE_VERTEX);
    if (desc->pixel.generation != 0)
    {
        pixel = tb_rhi_shader_slot(device, desc->pixel, TB_RHI_STAGE_PIXEL);
    }
    ref.desc = desc;
    ref.vertex = vertex->object;
    ref.vertex_desc = &vertex->desc.shader->desc;
    ref.pixel = pixel == NULL ? NULL : pixel->object;
    ref.pixel_desc = pixel == NULL ? NULL : &pixel->desc.shader->desc;
    result = device->table->graphics_pipeline_create(device->state, &ref, &object, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    tb_mutex_lock(&device->lock);
    pipeline->index = tb_rhi_add(device, TB_RHI_KIND_PIPELINE, object, none, desc->name, &slot);
    pipeline->generation = slot->generation;
    /* The handle slots of the shaders can be reused while this pipeline
     * lives. Keep their descriptions independently of those handles. */
    tb_rhi_pipeline_data *const data = TB_NEW(tb_rhi_pipeline_data);
    if (desc->attribute_count != 0)
    {
        data->attributes = TB_NEW_ARRAY(tb_rhi_vertex_attribute, desc->attribute_count);
    }
    data->first = tb_rhi_keep_shader(vertex);
    data->pixel = tb_rhi_keep_shader(pixel);
    data->graphics = *desc;
    data->graphics.name = desc->name != NULL ? slot->name : NULL;
    data->graphics.attributes = data->attributes;
    for (uint32_t i = 0; i < desc->attribute_count; i++)
    {
        const char *const semantic = desc->attributes[i].semantic;
        data->attributes[i] = desc->attributes[i];
        data->attributes[i].semantic = semantic != NULL ? tb_str_dup(NULL, semantic) : NULL;
    }
    slot->desc.pipeline = data;
    tb_mutex_unlock(&device->lock);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

tb_rhi_result tb_rhi_compute_pipeline_create(tb_rhi_device *device,
                                             const tb_rhi_compute_pipeline_desc *desc,
                                             tb_rhi_pipeline *pipeline, tb_rhi_error *error)
{
    const tb_rhi_indices none = {.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX};
    const tb_rhi_slot *compute = NULL;
    void *object = NULL;
    tb_rhi_slot *slot = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && desc != NULL && pipeline != NULL);
    *pipeline = (tb_rhi_pipeline){0};
    compute = tb_rhi_shader_slot(device, desc->compute, TB_RHI_STAGE_COMPUTE);
    result = device->table->compute_pipeline_create(device->state, desc, compute->object,
                                                    &compute->desc.shader->desc, &object, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    tb_mutex_lock(&device->lock);
    pipeline->index = tb_rhi_add(device, TB_RHI_KIND_PIPELINE, object, none, desc->name, &slot);
    pipeline->generation = slot->generation;
    tb_rhi_pipeline_data *const data = TB_NEW(tb_rhi_pipeline_data);
    data->first = tb_rhi_keep_shader(compute);
    data->compute = *desc;
    data->compute.name = desc->name != NULL ? slot->name : NULL;
    slot->desc.pipeline = data;
    slot->compute = true;
    tb_mutex_unlock(&device->lock);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

void tb_rhi_pipeline_destroy(tb_rhi_device *device, tb_rhi_pipeline pipeline)
{
    TB_ASSERT(device != NULL);
    if (pipeline.generation != 0)
    {
        tb_rhi_retire(device, TB_RHI_KIND_PIPELINE, pipeline.index, pipeline.generation);
    }
}

/* ---------------------------------------------------------------------------
 * Swap chains.
 * ------------------------------------------------------------------------- */

void tb_rhi_register_images(tb_rhi_swap_chain *chain, const tb_rhi_swap_chain_images *images)
{
    const tb_rhi_indices none = {.read = TB_RHI_NO_INDEX, .write = TB_RHI_NO_INDEX};
    tb_rhi_device *const device = chain->device;

    TB_ASSERT(images->count >= 2 && images->count <= TB_RHI_SWAP_CHAIN_MAX_IMAGES);
    tb_mutex_lock(&device->lock);
    for (uint32_t index = 0; index < images->count; index++)
    {
        tb_rhi_slot *slot = NULL;
        chain->images[index].index = tb_rhi_add(device, TB_RHI_KIND_TEXTURE, images->objects[index],
                                                none, "swap chain", &slot);
        chain->images[index].generation = slot->generation;
        slot->chain_owned = true;
        slot->desc.texture = (tb_rhi_texture_desc){.kind = TB_RHI_TEXTURE_2D,
                                                   .format = images->format,
                                                   .width = images->width,
                                                   .height = images->height,
                                                   .layers = 1,
                                                   .mips = 1,
                                                   .samples = 1,
                                                   .usage = TB_RHI_TEXTURE_RENDER_TARGET};
    }
    chain->image_count = images->count;
    tb_mutex_unlock(&device->lock);
}

void tb_rhi_unregister_images(tb_rhi_swap_chain *chain)
{
    tb_rhi_device *const device = chain->device;

    tb_mutex_lock(&device->lock);
    for (uint32_t index = 0; index < chain->image_count; index++)
    {
        tb_rhi_table_give(&device->tables[TB_RHI_KIND_TEXTURE], chain->images[index].index);
        chain->images[index] = (tb_rhi_texture){0};
    }
    chain->image_count = 0;
    tb_mutex_unlock(&device->lock);
}

tb_rhi_result tb_rhi_swap_chain_create(tb_rhi_device *device, struct tb_window *window,
                                       const tb_rhi_swap_chain_desc *desc,
                                       tb_rhi_swap_chain **chain, tb_rhi_error *error)
{
    tb_rhi_swap_chain_desc filled = {0};
    tb_rhi_swap_chain_images images = {0};
    tb_rhi_swap_chain *made = NULL;
    void *object = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(device != NULL && window != NULL && desc != NULL && chain != NULL);
    *chain = NULL;
    filled = *desc;
    filled.format =
        filled.format == TB_RHI_FORMAT_UNKNOWN ? TB_RHI_FORMAT_B8G8R8A8_UNORM : filled.format;
    filled.images = filled.images == 0 ? 3u : filled.images;
    TB_ASSERT_MSG(filled.images >= 2 && filled.images <= TB_RHI_SWAP_CHAIN_MAX_IMAGES,
                  "a swap chain of 2 to 4 images");
    result =
        device->table->swap_chain_create(device->state, window, &filled, &object, &images, error);
    if (result != TB_RHI_OK)
    {
        return result;
    }
    made = tb_calloc(1, sizeof(*made));
    made->device = device;
    made->object = object;
    made->vsync = filled.vsync;
    tb_rhi_register_images(made, &images);
    tb_mutex_lock(&device->lock);
    device->swap_chains++;
    tb_mutex_unlock(&device->lock);
    *chain = made;
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}

void tb_rhi_swap_chain_destroy(tb_rhi_swap_chain *chain)
{
    tb_rhi_device *device = NULL;

    if (chain == NULL)
    {
        return;
    }
    device = chain->device;
    (void)tb_rhi_wait_idle(device);
    tb_rhi_unregister_images(chain);
    device->table->swap_chain_destroy(device->state, chain->object);
    tb_mutex_lock(&device->lock);
    device->swap_chains--;
    tb_mutex_unlock(&device->lock);
    tb_free(chain);
}

tb_rhi_texture tb_rhi_swap_chain_current(tb_rhi_swap_chain *chain)
{
    uint32_t index = 0;

    TB_ASSERT(chain != NULL);
    index = chain->device->table->swap_chain_current(chain->device->state, chain->object);
    TB_ASSERT(index < chain->image_count);
    return chain->images[index];
}

tb_rhi_result tb_rhi_swap_chain_present(tb_rhi_swap_chain *chain)
{
    TB_ASSERT(chain != NULL);
    tb_rhi_device *const device = chain->device;
    tb_mutex_lock(&device->lock);
    const bool lost = device->lost;
    tb_mutex_unlock(&device->lock);
    if (lost)
    {
        return TB_RHI_DEVICE_LOST;
    }
    const tb_rhi_result result = device->table->swap_chain_present(device->state, chain->object);
    if (result == TB_RHI_DEVICE_LOST)
    {
        tb_rhi_device_report_loss(device);
    }
    return result;
}

tb_rhi_result tb_rhi_swap_chain_resize(tb_rhi_swap_chain *chain, uint32_t width, uint32_t height,
                                       bool vsync, tb_rhi_error *error)
{
    tb_rhi_swap_chain_images images = {0};
    tb_rhi_device *device = NULL;
    tb_rhi_result result = TB_RHI_OK;

    TB_ASSERT(chain != NULL);
    device = chain->device;
    result = tb_rhi_wait_idle(device);
    if (result != TB_RHI_OK)
    {
        return tb_rhi_fail(error, result, "the device was lost");
    }
    tb_rhi_unregister_images(chain);
    result = device->table->swap_chain_resize(device->state, chain->object, width, height, vsync,
                                              &images, error);
    if (result != TB_RHI_OK)
    {
        if (result == TB_RHI_DEVICE_LOST)
        {
            tb_rhi_device_report_loss(device);
        }
        return result;
    }
    chain->vsync = vsync;
    tb_rhi_register_images(chain, &images);
    tb_rhi_succeed(error);
    return TB_RHI_OK;
}
