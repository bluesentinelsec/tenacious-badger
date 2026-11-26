/* The D3D11 backend's objects, shared by its sources.
 *
 * Direct3D 11 is called through its COM C bindings (COBJMACROS), and
 * d3d11.dll and dxgi.dll are loaded through the library interface, never
 * linked. The interface identifiers are defined once, in d3d11_guids.c and,
 * for DXGI's, in the D3D12 backend's d3d12_guids.c.
 *
 * D3D11 is not an explicit API, so the backend imitates what the RHI
 * expects:
 *
 * - **Command lists** are the backend's own: each records its commands into
 *   memory, from any thread, and a submission replays them on the immediate
 *   context, in order, under the context's lock. D3D11's deferred contexts
 *   are left alone: drivers emulate them anyway, and replaying gives the
 *   backend the moment of each draw, which binding needs.
 * - **Barriers** need nothing: D3D11 tracks every resource's state itself.
 * - **Bindless resources** are emulated for indices that are the same for
 *   a whole draw: the shader compiler gives each resource a shader takes
 *   from a descriptor heap a register, and says where its index is in the
 *   constants (src/shader/shader_dxbc_format.h). Before each draw or
 *   dispatch, the backend reads each index from the push constants or the
 *   constant buffer the list bound, and binds the view the index names.
 * - **Push constants and constant buffers** are D3D11 constant buffers of
 *   the backend's own, filled before each draw that reads them: the push
 *   constants from the list, a constant buffer from its CPU copy.
 * - **Upload memory** is a CPU copy of a buffer and a GPU buffer, which a
 *   submission brings up to date where the CPU wrote, before it runs.
 *   **Readback memory** is CPU memory: a copy into it is a copy into a
 *   staging resource, read into the CPU memory when the CPU reads it. */
#ifndef TB_RHI_D3D11_INTERNAL_H
#define TB_RHI_D3D11_INTERNAL_H

#include "../../shader/shader_dxbc_format.h"
#include "../rhi_backend.h"

#include "badger/library.h"
#include "badger/thread.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <stdbool.h>
#include <stdint.h>

/* The constant buffers the RHI binds: the push constants at b0, then b1 and b2. */
#define TB_D3D11_CONSTANT_BUFFERS TB_SHADER_DXBC_CONSTANT_BUFFERS
/* The largest constant buffer D3D11 binds: 4096 registers of 16 bytes. */
#define TB_D3D11_CONSTANT_BUFFER_BYTES 65536u
/* Registers a stage binds of each class, as D3D11 has them. */
#define TB_D3D11_READ_REGISTERS D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
#define TB_D3D11_SAMPLER_REGISTERS D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
#define TB_D3D11_WRITE_REGISTERS D3D11_1_UAV_SLOT_COUNT

typedef HRESULT(WINAPI *tb_d3d11_create_device)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                ID3D11DeviceContext **);
typedef HRESULT(WINAPI *tb_d3d11_create_factory)(UINT, REFIID, void **);

/* What a bindless index names: a view, and what it is a view of, which
 * decides the registers it may be bound to. */
typedef struct tb_d3d11_view
{
    ID3D11View *view; /* NULL for none */
    bool writable;    /* An unordered-access view; else a shader-resource view. */
    uint8_t shape;    /* A tb_shader_dxbc_view. */
} tb_d3d11_view;

/* A submission whose end the GPU has not been seen to pass yet. */
typedef struct tb_d3d11_pending
{
    ID3D11Query *query;
    uint64_t submission;
} tb_d3d11_pending;

typedef struct tb_d3d11_buffer tb_d3d11_buffer;

/* A copy into readback memory, through a staging resource, to read into the
 * buffer's CPU memory once the GPU has made it. */
typedef struct tb_d3d11_readback
{
    ID3D11Resource *staging;
    tb_d3d11_buffer *buffer;
    uint64_t offset;    /* into the buffer */
    uint64_t size;      /* of a buffer's copy */
    bool texture;       /* the staging resource is a texture, read by rows */
    uint32_t row_pitch; /* of the buffer's rows */
    uint32_t row_bytes; /* of the region's rows */
    uint32_t rows;
    uint32_t x_bytes; /* where the region starts in the staging texture */
    uint32_t y;
    uint64_t submission; /* made by; 0 until submitted */
} tb_d3d11_readback;

typedef struct tb_d3d11_list tb_d3d11_list;

typedef struct tb_d3d11_device
{
    tb_library *d3d11;
    tb_library *dxgi;
    tb_d3d11_create_device create_device;
    tb_d3d11_create_factory create_factory;

    IDXGIFactory2 *factory;
    IDXGIAdapter1 *adapter;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11InfoQueue *info_queue;                        /* with validation only */
    ID3DUserDefinedAnnotation *annotation;              /* for markers, where there is one */
    ID3D11Buffer *constants[TB_D3D11_CONSTANT_BUFFERS]; /* filled before each draw */
    bool tearing;
    bool typed_loads; /* shaders load the common formats from writable textures */
    struct tb_log *log;

    /* The immediate context, and everything a submission touches: the view
     * tables, the pending queries and readbacks, the CPU copies' dirty
     * ranges. Never held while another lock of the backend's is taken. */
    tb_mutex context_lock;
    tb_d3d11_view *views; /* by resource index */
    uint32_t view_count;
    ID3D11SamplerState **samplers; /* by sampler index */
    uint32_t sampler_count;
    tb_d3d11_pending *pending;
    size_t pending_count;
    size_t pending_capacity;
    uint64_t completed;
    tb_d3d11_readback *readbacks;
    size_t readback_count;
    size_t readback_capacity;
    tb_d3d11_buffer **uploads; /* upload buffers, whose CPU copies submissions bring up */
    size_t upload_count;
    size_t upload_capacity;
    uint64_t warned; /* bits of tb_d3d11_warning already logged */

    tb_mutex lists_lock; /* guards the idle lists */
    tb_d3d11_list **idle;
    size_t idle_count;
    size_t idle_capacity;
    tb_mutex messages_lock; /* one drain of validation messages at a time */
} tb_d3d11_device;

/* What the backend says once, when it cannot do what a list asks. */
typedef enum tb_d3d11_warning
{
    TB_D3D11_WARN_INDEX = 1u << 0,       /* a heap index outside the constants */
    TB_D3D11_WARN_CONSTANTS = 1u << 1,   /* a constant buffer with no CPU copy */
    TB_D3D11_WARN_COPY = 1u << 2,        /* a copy D3D11 cannot make */
    TB_D3D11_WARN_PIXEL_WRITES = 1u << 3 /* a pixel shader that writes resources */
} tb_d3d11_warning;

struct tb_d3d11_buffer
{
    ID3D11Buffer *buffer; /* NULL for readback memory, and upload memory only read as constants */
    uint8_t *cpu;         /* upload and readback memory's bytes; a constant buffer's copy */
    uint64_t size;
    tb_rhi_memory memory;
    uint32_t usage;
    ID3D11ShaderResourceView *read;
    ID3D11UnorderedAccessView *write;
    tb_rhi_indices indices;
    /* Upload memory: the bytes the CPU wrote since the last submission. */
    uint64_t dirty_begin;
    uint64_t dirty_end;
};

typedef struct tb_d3d11_texture
{
    ID3D11Texture2D *texture;
    DXGI_FORMAT resource_format;
    DXGI_FORMAT target_format; /* of render-target and depth views */
    tb_rhi_texture_desc desc;  /* its name dropped */
    ID3D11ShaderResourceView *read;
    ID3D11UnorderedAccessView *writes[TB_RHI_MAX_MIPS];
    ID3D11RenderTargetView **targets; /* each level of each layer, made when first drawn to */
    ID3D11DepthStencilView *depth[2]; /* written, and read only; made when first used */
    tb_rhi_indices indices;
    bool swap_chain; /* the swap chain owns the texture */
} tb_d3d11_texture;

typedef struct tb_d3d11_shader
{
    tb_rhi_stage stage;
    void *bytes; /* the engine's shader for D3D11: the slots, then the DXBC */
    tb_shader_dxbc_shader read;
    ID3D11DeviceChild *object; /* the vertex, pixel or compute shader */
} tb_d3d11_shader;

typedef struct tb_d3d11_pipeline
{
    bool compute;
    ID3D11VertexShader *vertex;
    ID3D11PixelShader *pixel;
    ID3D11ComputeShader *compute_shader;
    ID3D11InputLayout *layout;
    ID3D11RasterizerState *rasterizer;
    ID3D11DepthStencilState *depth;
    ID3D11BlendState *blend;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    /* What each stage takes from the heaps, and reads of the constants:
     * the vertex or compute shader's, then the pixel shader's. */
    tb_shader_dxbc_shader stages[2];
    uint32_t stage_count;
} tb_d3d11_pipeline;

struct tb_d3d11_list
{
    tb_d3d11_device *device;
    uint8_t *bytes; /* its commands, recorded */
    size_t length;
    size_t capacity;
};

/* The format D3D11 calls a format of the RHI. */
DXGI_FORMAT tb_d3d11_format(tb_rhi_format format);

/* Fill an error from a failed call; returns its result. */
tb_rhi_result tb_d3d11_fail(tb_rhi_error *error, HRESULT result, const char *what);

/* Write what validation found to the log. */
void tb_d3d11_drain_messages(tb_d3d11_device *device);

/* Say once that the backend cannot do something a list asked. */
void tb_d3d11_warn(tb_d3d11_device *device, tb_d3d11_warning warning, const char *message);

/* Read every copy into a buffer the GPU has made into its CPU memory. Held
 * with the context's lock. */
void tb_d3d11_resolve_readbacks(tb_d3d11_device *device, const tb_d3d11_buffer *buffer);
/* Forget a buffer's copies, made or not: it is going. Held with the context's lock. */
void tb_d3d11_drop_readbacks(tb_d3d11_device *device, const tb_d3d11_buffer *buffer);
/* Poll the GPU's progress. Held with the context's lock. */
uint64_t tb_d3d11_poll(tb_d3d11_device *device);

tb_rhi_result tb_d3d11_buffer_create(void *state, const tb_rhi_buffer_desc *desc,
                                     tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_d3d11_buffer_destroy(void *state, void *object);
void tb_d3d11_buffer_write(void *state, void *object, uint64_t offset, const void *data,
                           size_t size);
void tb_d3d11_buffer_read(void *state, void *object, uint64_t offset, void *data, size_t size);
tb_rhi_result tb_d3d11_texture_create(void *state, const tb_rhi_texture_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_d3d11_texture_destroy(void *state, void *object);
/* Release what a texture made of itself, its views; the swap chain's too. */
void tb_d3d11_texture_release_views(tb_d3d11_texture *texture);
/* A texture's render-target view of a level of a layer, made the first time. */
ID3D11RenderTargetView *tb_d3d11_target_view(tb_d3d11_device *device, tb_d3d11_texture *texture,
                                             uint32_t mip, uint32_t layer);
/* A texture's depth view, written or read only, made the first time. */
ID3D11DepthStencilView *tb_d3d11_depth_view(tb_d3d11_device *device, tb_d3d11_texture *texture,
                                            bool read_only);
tb_rhi_result tb_d3d11_sampler_create(void *state, const tb_rhi_sampler_desc *desc,
                                      tb_rhi_indices indices, void **object, tb_rhi_error *error);
void tb_d3d11_sampler_destroy(void *state, void *object);

tb_rhi_result tb_d3d11_shader_create(void *state, const tb_rhi_shader_desc *desc, void **object,
                                     tb_rhi_error *error);
void tb_d3d11_shader_destroy(void *state, void *object);
tb_rhi_result tb_d3d11_graphics_pipeline_create(void *state,
                                                const tb_rhi_graphics_pipeline_ref *pipeline,
                                                void **object, tb_rhi_error *error);
tb_rhi_result tb_d3d11_compute_pipeline_create(void *state,
                                               const tb_rhi_compute_pipeline_desc *desc,
                                               void *shader, const tb_rhi_shader_desc *shader_desc,
                                               void **object, tb_rhi_error *error);
void tb_d3d11_pipeline_destroy(void *state, void *object);

tb_rhi_result tb_d3d11_swap_chain_create(void *state, struct tb_window *window,
                                         const tb_rhi_swap_chain_desc *desc, void **chain,
                                         tb_rhi_swap_chain_images *images, tb_rhi_error *error);
void tb_d3d11_swap_chain_destroy(void *state, void *chain);
uint32_t tb_d3d11_swap_chain_current(void *state, void *chain);
tb_rhi_result tb_d3d11_swap_chain_present(void *state, void *chain);
tb_rhi_result tb_d3d11_swap_chain_resize(void *state, void *chain, uint32_t width, uint32_t height,
                                         bool vsync, tb_rhi_swap_chain_images *images,
                                         tb_rhi_error *error);

void *tb_d3d11_list_begin(void *state);
void tb_d3d11_list_discard(void *state, void *list);
tb_rhi_result tb_d3d11_submit(void *state, void *const *lists, size_t count, uint64_t submission);
void tb_d3d11_release_lists(tb_d3d11_device *device);

void tb_d3d11_cmd_barriers(void *list, const tb_rhi_barrier_ref *barriers, size_t count);
void tb_d3d11_cmd_begin_pass(void *list, const tb_rhi_pass_ref *pass);
void tb_d3d11_cmd_end_pass(void *list);
void tb_d3d11_cmd_viewport(void *list, float x, float y, float width, float height,
                           float near_depth, float far_depth);
void tb_d3d11_cmd_scissor(void *list, tb_rhi_rect rect);
void tb_d3d11_cmd_pipeline(void *list, void *pipeline, bool compute);
void tb_d3d11_cmd_push_constants(void *list, const void *data, size_t size);
void tb_d3d11_cmd_constant_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset);
void tb_d3d11_cmd_vertex_buffer(void *list, uint32_t slot, void *buffer, uint64_t offset,
                                uint64_t size, uint32_t stride);
void tb_d3d11_cmd_index_buffer(void *list, void *buffer, uint64_t offset, uint64_t size,
                               tb_rhi_format format);
void tb_d3d11_cmd_draw(void *list, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                       uint32_t first_instance);
void tb_d3d11_cmd_draw_indexed(void *list, uint32_t indices, uint32_t instances,
                               uint32_t first_index, int32_t vertex_offset,
                               uint32_t first_instance);
void tb_d3d11_cmd_dispatch(void *list, uint32_t x, uint32_t y, uint32_t z);
void tb_d3d11_cmd_copy_buffer(void *list, void *dest, uint64_t dest_offset, void *source,
                              uint64_t source_offset, uint64_t size);
void tb_d3d11_cmd_copy_buffer_to_texture(void *list, const tb_rhi_texture_copy_ref *copy);
void tb_d3d11_cmd_copy_texture_to_buffer(void *list, const tb_rhi_texture_copy_ref *copy);
void tb_d3d11_cmd_begin_marker(void *list, const char *name);
void tb_d3d11_cmd_end_marker(void *list);

#endif
