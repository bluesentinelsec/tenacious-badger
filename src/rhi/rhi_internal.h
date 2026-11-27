/* What the device keeps: shared by the RHI's sources, private to them. */
#ifndef TB_RHI_INTERNAL_H
#define TB_RHI_INTERNAL_H

#include "rhi_backend.h"

#include "badger/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum tb_rhi_kind
{
    TB_RHI_KIND_BUFFER,
    TB_RHI_KIND_TEXTURE,
    TB_RHI_KIND_SAMPLER,
    TB_RHI_KIND_SHADER,
    TB_RHI_KIND_PIPELINE,
    TB_RHI_KIND_COUNT
} tb_rhi_kind;

/* Bytes of a shader's entry point's name, its NUL included. */
#define TB_RHI_ENTRY_CAPACITY 64u

/* One immutable copy of the compiled shader, shared by its live handle and
 * by each pipeline using it. The device lock protects the reference count. */
typedef struct tb_rhi_shader_data
{
    size_t references;
    tb_rhi_shader_desc desc;
    char name[TB_RHI_NAME_CAPACITY];
    char entry[TB_RHI_ENTRY_CAPACITY];
    void *code;
} tb_rhi_shader_data;

typedef struct tb_rhi_pipeline_data
{
    tb_rhi_graphics_pipeline_desc graphics;
    tb_rhi_compute_pipeline_desc compute;
    tb_rhi_shader_data *first; /* vertex or compute */
    tb_rhi_shader_data *pixel; /* optional */
    tb_rhi_vertex_attribute *attributes;
} tb_rhi_pipeline_data;

/* One object of a kind. Slots live in chunks that never move, so a pointer
 * into one stays valid while the device lives. */
typedef struct tb_rhi_slot
{
    uint32_t generation; /* of the object alive, or of the last one */
    bool alive;
    bool chain_owned; /* a swap chain's image: the chain destroys it */
    bool compute;     /* a compute pipeline */
    void *object;     /* the backend's */
    tb_rhi_indices indices;
    union {
        tb_rhi_buffer_desc buffer;
        tb_rhi_texture_desc texture;
        tb_rhi_sampler_desc sampler;
        tb_rhi_shader_data *shader;
        tb_rhi_pipeline_data *pipeline;
    } desc;
    char name[TB_RHI_NAME_CAPACITY];
} tb_rhi_slot;

/* The slots of one kind. */
typedef struct tb_rhi_table
{
    tb_rhi_slot **chunks;
    size_t chunk_count;
    uint32_t used; /* slots ever handed out */
    uint32_t *vacant;
    size_t vacant_count;
    size_t vacant_capacity;
} tb_rhi_table;

/* Bindless indices of one heap. */
typedef struct tb_rhi_index_pool
{
    uint32_t capacity;
    uint32_t next;
    uint32_t *vacant;
    size_t vacant_count;
} tb_rhi_index_pool;

/* An object destroyed while the GPU may still use it. */
typedef struct tb_rhi_retired
{
    tb_rhi_kind kind;
    void *object;
    tb_rhi_indices indices;
    uint64_t after; /* released once the GPU has finished this submission */
} tb_rhi_retired;

struct tb_rhi_device
{
    const tb_rhi_backend_table *table;
    void *state;
    tb_rhi_device_info info;
    tb_mutex lock; /* guards everything below but the submissions */
    tb_rhi_table tables[TB_RHI_KIND_COUNT];
    tb_rhi_index_pool resources;
    tb_rhi_index_pool samplers;
    tb_rhi_retired *retired;
    size_t retired_count;
    size_t retired_capacity;
    size_t open_lists;
    tb_mutex submit_lock; /* one submission at a time */
    uint64_t submitted;   /* under both locks to write; under either to read */
    bool lost;
};

struct tb_rhi_command_list
{
    tb_rhi_device *device;
    void *backend;
    bool in_pass;
    bool compute;
    /* Vertex buffers wait for the pipeline's strides until a draw. */
    struct
    {
        void *object;
        uint64_t offset;
        uint64_t size;
    } vertex[TB_RHI_VERTEX_BUFFERS];
    uint32_t vertex_dirty;
    tb_rhi_vertex_buffer_layout layouts[TB_RHI_VERTEX_BUFFERS];
    bool has_pipeline;
    uint32_t markers;
};

struct tb_rhi_swap_chain
{
    tb_rhi_device *device;
    void *object;
    tb_rhi_texture images[TB_RHI_SWAP_CHAIN_MAX_IMAGES];
    uint32_t image_count;
    bool vsync;
};

/* The live slot a handle names. A handle that names none is a contract
 * violation, caught in every build. Takes the device's lock. */
tb_rhi_slot *tb_rhi_slot_of(tb_rhi_device *device, tb_rhi_kind kind, uint32_t index,
                            uint32_t generation);

/* Release what the GPU has finished with. Called with the lock held. */
void tb_rhi_collect(tb_rhi_device *device, uint64_t completed);

/* Give a swap chain's images handles, or take them away. */
void tb_rhi_register_images(tb_rhi_swap_chain *chain, const tb_rhi_swap_chain_images *images);
void tb_rhi_unregister_images(tb_rhi_swap_chain *chain);

#endif
