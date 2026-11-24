/* The D3D12 backend's objects, shared by its sources.
 *
 * Direct3D 12 is called through its COM C bindings (COBJMACROS), and
 * d3d12.dll and dxgi.dll are loaded through the library interface, never
 * linked. The interface identifiers are defined once, in d3d12_guids.c. */
#ifndef TB_RHI_D3D12_INTERNAL_H
#define TB_RHI_D3D12_INTERNAL_H

#include "../rhi_backend.h"

#include "badger/library.h"
#include "badger/thread.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <stdbool.h>
#include <stdint.h>

/* Root parameters of the one root signature every pipeline shares. */
#define TB_D3D12_ROOT_CONSTANTS 0u
#define TB_D3D12_ROOT_CONSTANT_BUFFER 1u

/* Descriptors of render and depth targets a command list writes at once. */
#define TB_D3D12_LIST_TARGETS (TB_RHI_COLOR_TARGETS + 1u)

typedef HRESULT(WINAPI *tb_d3d12_create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT(WINAPI *tb_d3d12_get_debug_interface)(REFIID, void **);
typedef HRESULT(WINAPI *tb_d3d12_serialize_root_signature)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *tb_dxgi_create_factory)(UINT, REFIID, void **);

typedef struct tb_d3d12_list tb_d3d12_list;

typedef struct tb_d3d12_device
{
    tb_library *d3d12;
    tb_library *dxgi;
    tb_d3d12_create_device create_device;
    tb_d3d12_get_debug_interface get_debug_interface;
    tb_d3d12_serialize_root_signature serialize_root_signature;
    tb_dxgi_create_factory create_factory;

    IDXGIFactory6 *factory;
    IDXGIAdapter1 *adapter;
    ID3D12Device *device;
    ID3D12InfoQueue *info_queue; /* with validation only */
    ID3D12CommandQueue *queue;
    ID3D12Fence *fence;
    /* Waits for what the queue does outside submissions, such as presents;
     * its own numbers, so that submissions' stay the RHI's. */
    ID3D12Fence *flush_fence;
    UINT64 flushed;
    ID3D12RootSignature *root;
    bool tearing;
    bool bindless;
    struct tb_log *log;

    ID3D12DescriptorHeap *resources; /* shader-visible, by bindless index */
    ID3D12DescriptorHeap *samplers;  /* shader-visible, by bindless index */
    D3D12_CPU_DESCRIPTOR_HANDLE resources_cpu;
    D3D12_CPU_DESCRIPTOR_HANDLE samplers_cpu;
    UINT resource_increment;
    UINT sampler_increment;
    UINT target_increment;
    UINT depth_increment;

    tb_mutex lists_lock; /* guards the lists below */
    tb_d3d12_list **idle;
    size_t idle_count;
    size_t idle_capacity;
    tb_mutex messages_lock; /* one drain of validation messages at a time */
} tb_d3d12_device;

typedef struct tb_d3d12_buffer
{
    ID3D12Resource *resource;
    tb_rhi_memory memory;
    uint8_t *mapped; /* upload and readback memory, mapped while it lives */
} tb_d3d12_buffer;

typedef struct tb_d3d12_texture
{
    ID3D12Resource *resource;
    DXGI_FORMAT target_format; /* of render-target or depth views */
    bool swap_chain;           /* the swap chain releases it */
} tb_d3d12_texture;

typedef struct tb_d3d12_shader
{
    void *code;
    size_t size;
} tb_d3d12_shader;

typedef struct tb_d3d12_pipeline
{
    ID3D12PipelineState *state;
    D3D_PRIMITIVE_TOPOLOGY topology;
    bool compute;
} tb_d3d12_pipeline;

struct tb_d3d12_list
{
    tb_d3d12_device *device;
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12DescriptorHeap *targets; /* render targets, then a depth target */
    ID3D12DescriptorHeap *depths;
    uint64_t submission; /* the last that ran it */
};

/* The format D3D12 calls a format of the RHI. */
DXGI_FORMAT tb_d3d12_format(tb_rhi_format format);

/* Fill an error from a failed call; returns its result. */
tb_rhi_result tb_d3d12_fail(tb_rhi_error *error, HRESULT result, const char *what);

/* Wait until the queue has finished everything, presents included. */
tb_rhi_result tb_d3d12_flush(tb_d3d12_device *device);

/* Write what validation found to the log. */
void tb_d3d12_drain_messages(tb_d3d12_device *device);

/* The CPU address of a descriptor in a heap. */
D3D12_CPU_DESCRIPTOR_HANDLE tb_d3d12_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE start, UINT increment,
                                                uint32_t index);

tb_rhi_result tb_d3d12_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                     tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_d3d12_buffer_destroy(void *state, void *object);
void tb_d3d12_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                           size_t size);
void tb_d3d12_buffer_read(void *state, void *object, uint64_t offset, void *data, size_t size);
tb_rhi_result tb_d3d12_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_d3d12_texture_destroy(void *state, void *object);
tb_rhi_result tb_d3d12_sampler_create(void *state, const tb_rhi_sampler_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_d3d12_sampler_destroy(void *state, void *object);

tb_rhi_result tb_d3d12_shader_create(void *state, const tb_rhi_shader_desc *desc, void **object,
                                     tb_rhi_error *error);
void tb_d3d12_shader_destroy(void *state, void *object);
tb_rhi_result tb_d3d12_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *pipeline,
                                                void **object, tb_rhi_error *error);
tb_rhi_result tb_d3d12_compute_pipeline_create(void *state,
                                               const tb_rhi_compute_pipeline_desc *desc,
                                               void *shader, const tb_rhi_shader_desc *shader_desc,
                                               void **object, tb_rhi_error *error);
void tb_d3d12_pipeline_destroy(void *state, void *object);

tb_rhi_result tb_d3d12_swap_chain_create(void *state, struct tb_window *window,
                                         const tb_rhi_swap_chain_desc *desc, void **chain,
                                         tb_rhi_swap_chain_images *images, tb_rhi_error *error);
void tb_d3d12_swap_chain_destroy(void *state, void *chain);
uint32_t tb_d3d12_swap_chain_current(void *state, void *chain);
tb_rhi_result tb_d3d12_swap_chain_present(void *state, void *chain);
tb_rhi_result tb_d3d12_swap_chain_resize(void *state, void *chain, uint32_t width, uint32_t height,
                                         bool vsync, tb_rhi_swap_chain_images *images,
                                         tb_rhi_error *error);

void *tb_d3d12_list_begin(void *state);
void tb_d3d12_list_discard(void *state, void *list);
tb_rhi_result tb_d3d12_submit(void *state, void *const *lists, size_t count, uint64_t submission);
void tb_d3d12_release_lists(tb_d3d12_device *device);
/* Reset the idle lists, so that none holds a resource the GPU is done with:
 * a swap chain resizes only once nothing holds its images. */
void tb_d3d12_forget_lists(tb_d3d12_device *device);

void tb_d3d12_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count);
void tb_d3d12_cmd_begin_pass(void *list, const tb_rhi_pass_ref *pass);
void tb_d3d12_cmd_end_pass(void *list);
void tb_d3d12_cmd_viewport(void *list, float x, float y, float width, float height,
                           float near_depth, float far_depth);
void tb_d3d12_cmd_scissor(void *list, tb_rhi_rect rect);
void tb_d3d12_cmd_pipeline(void *list, void *pipeline, bool compute);
void tb_d3d12_cmd_push_constants(void *list, const void *data, size_t size);
void tb_d3d12_cmd_constant_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset);
void tb_d3d12_cmd_vertex_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset,
                                uint64_t size, uint32_t stride);
void tb_d3d12_cmd_index_buffer(void *list, void *buffer, uint64_t offset, uint64_t size,
                               tb_rhi_format format);
void tb_d3d12_cmd_draw(void *list, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance);
void tb_d3d12_cmd_draw_indexed(void *list, uint32_t indices, uint32_t instances,
                               uint32_t first_index, int32_t vertex_offset,
                               uint32_t first_instance);
void tb_d3d12_cmd_dispatch(void *list, uint32_t x, uint32_t y, uint32_t z);
void tb_d3d12_cmd_copy_buffer(void *list, void *dest, uint64_t dest_offset, void *source,
                              uint64_t source_offset, uint64_t size);
void tb_d3d12_cmd_copy_buffer_to_texture(void *list, const tb_rhi_texture_copy_ref *copy);
void tb_d3d12_cmd_copy_texture_to_buffer(void *list, const tb_rhi_texture_copy_ref *copy);
void tb_d3d12_cmd_begin_marker(void *list, const char *name);
void tb_d3d12_cmd_end_marker(void *list);

#endif
