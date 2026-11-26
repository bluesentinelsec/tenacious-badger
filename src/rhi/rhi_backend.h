/* The function table a backend of the RHI implements.
 *
 * The device in rhi.c owns every handle, description and bindless index; a
 * backend sees only its own objects, already looked up, and the indices the
 * device chose for them. That keeps every backend free of bookkeeping, and
 * keeps handles and indices the same whichever backend runs.
 *
 * Every function is called with the device's arguments already checked.
 * Recording functions are called for one command list from one thread at a
 * time; the others may be called from any thread at once, except where the
 * header of the RHI says otherwise, so a backend guards what it shares.
 */
#ifndef TB_RHI_BACKEND_H
#define TB_RHI_BACKEND_H

#include "badger/rhi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_window;

/* The bindless indices the device gave an object; TB_RHI_NO_INDEX where it
 * has none. */
typedef struct tb_rhi_indices
{
    uint32_t read;  /* Sampled textures, buffers read by index, samplers. */
    uint32_t write; /* Storage textures (their first level) and buffers. */
    /* A storage texture's levels after the first, each written through an
     * index of its own: mip_writes[0] is level 1. */
    uint32_t mip_writes[TB_RHI_MAX_MIPS - 1];
    uint32_t mip_write_count;
} tb_rhi_indices;

/* A texture as a command sees it: the backend's object and what it was made
 * with. */
typedef struct tb_rhi_texture_ref
{
    void *object;
    const tb_rhi_texture_desc *desc;
} tb_rhi_texture_ref;

/* A barrier, looked up. Exactly one of buffer and texture is set. */
typedef struct tb_rhi_barrier_ref
{
    void *buffer;
    tb_rhi_texture_ref texture;
    tb_rhi_state before;
    tb_rhi_state after;
} tb_rhi_barrier_ref;

/* A pass, looked up. */
typedef struct tb_rhi_pass_ref
{
    tb_rhi_color_target colors[TB_RHI_COLOR_TARGETS];
    tb_rhi_texture_ref color_textures[TB_RHI_COLOR_TARGETS];
    uint32_t color_count;
    tb_rhi_depth_target depth;
    tb_rhi_texture_ref depth_texture; /* object NULL for none */
    uint32_t width;                   /* of the targets */
    uint32_t height;
} tb_rhi_pass_ref;

/* A copy between a buffer and a texture, looked up. */
typedef struct tb_rhi_texture_copy_ref
{
    void *buffer;
    uint64_t offset;
    uint32_t row_pitch;
    tb_rhi_texture_ref texture;
    uint32_t mip;
    uint32_t layer;
    tb_rhi_rect region; /* never of zero size */
} tb_rhi_texture_copy_ref;

/* A graphics pipeline's description, with its shaders looked up. */
typedef struct tb_rhi_graphics_pipeline_ref
{
    const tb_rhi_graphics_pipeline_desc *desc;
    void *vertex;
    const tb_rhi_shader_desc *vertex_desc;
    void *pixel; /* NULL for none */
    const tb_rhi_shader_desc *pixel_desc;
} tb_rhi_graphics_pipeline_ref;

/* The images of a swap chain, as the backend made them. */
#define TB_RHI_SWAP_CHAIN_MAX_IMAGES 4u
typedef struct tb_rhi_swap_chain_images
{
    void *objects[TB_RHI_SWAP_CHAIN_MAX_IMAGES];
    uint32_t count;
    uint32_t width;
    uint32_t height;
    tb_rhi_format format;
} tb_rhi_swap_chain_images;

typedef struct tb_rhi_backend_table
{
    tb_rhi_backend backend;

    /* The device. info arrives with the options' bindless sizes filled in. */
    tb_rhi_result (*device_create)(const tb_rhi_device_options *options, void **state,
                                   tb_rhi_device_info *info, tb_rhi_error *error);
    void (*device_destroy)(void *state);

    tb_rhi_result (*buffer_create)(void *state, const tb_rhi_buffer_desc *desc,
                                   tb_rhi_indices indices, void **object, tb_rhi_error *error);
    void (*buffer_destroy)(void *state, void *object);
    void (*buffer_write)(void *state, void *object, uint64_t offset, const void *data, size_t size);
    void (*buffer_read)(void *state, void *object, uint64_t offset, void *data, size_t size);

    tb_rhi_result (*texture_create)(void *state, const tb_rhi_texture_desc *desc,
                                    tb_rhi_indices indices, void **object, tb_rhi_error *error);
    void (*texture_destroy)(void *state, void *object);

    tb_rhi_result (*sampler_create)(void *state, const tb_rhi_sampler_desc *desc,
                                    tb_rhi_indices indices, void **object, tb_rhi_error *error);
    void (*sampler_destroy)(void *state, void *object);

    tb_rhi_result (*shader_create)(void *state, const tb_rhi_shader_desc *desc, void **object,
                                   tb_rhi_error *error);
    void (*shader_destroy)(void *state, void *object);

    tb_rhi_result (*graphics_pipeline_create)(void *state,
                                              const tb_rhi_graphics_pipeline_ref *pipeline,
                                              void **object, tb_rhi_error *error);
    tb_rhi_result (*compute_pipeline_create)(void *state, const tb_rhi_compute_pipeline_desc *desc,
                                             void *shader, const tb_rhi_shader_desc *shader_desc,
                                             void **object, tb_rhi_error *error);
    void (*pipeline_destroy)(void *state, void *object);

    /* A swap chain makes its images; the device gives them handles. */
    tb_rhi_result (*swap_chain_create)(void *state, struct tb_window *window,
                                       const tb_rhi_swap_chain_desc *desc, void **chain,
                                       tb_rhi_swap_chain_images *images, tb_rhi_error *error);
    void (*swap_chain_destroy)(void *state, void *chain);
    uint32_t (*swap_chain_current)(void *state, void *chain);
    tb_rhi_result (*swap_chain_present)(void *state, void *chain);
    tb_rhi_result (*swap_chain_resize)(void *state, void *chain, uint32_t width, uint32_t height,
                                       bool vsync, tb_rhi_swap_chain_images *images,
                                       tb_rhi_error *error);

    void *(*list_begin)(void *state);
    void (*list_discard)(void *state, void *list);
    /* Runs the lists, then signals the submission's number. */
    tb_rhi_result (*submit)(void *state, void *const *lists, size_t count, uint64_t submission);
    uint64_t (*completed)(void *state);
    tb_rhi_result (*wait)(void *state, uint64_t submission);

    void (*cmd_barriers)(void *list, const tb_rhi_barrier_ref *barriers, size_t count);
    void (*cmd_begin_pass)(void *list, const tb_rhi_pass_ref *pass);
    void (*cmd_end_pass)(void *list);
    void (*cmd_viewport)(void *list, float x, float y, float width, float height, float near_depth,
                         float far_depth);
    void (*cmd_scissor)(void *list, tb_rhi_rect rect);
    void (*cmd_pipeline)(void *list, void *pipeline, bool compute);
    void (*cmd_push_constants)(void *list, const void *data, size_t size);
    void (*cmd_constant_buffer)(void *list, uint32_t slot, void *buffer, uint64_t offset);
    void (*cmd_vertex_buffer)(void *list, uint32_t slot, void *buffer, uint64_t offset,
                              uint64_t size, uint32_t stride);
    void (*cmd_index_buffer)(void *list, void *buffer, uint64_t offset, uint64_t size,
                             tb_rhi_format format);
    void (*cmd_draw)(void *list, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                     uint32_t first_instance);
    void (*cmd_draw_indexed)(void *list, uint32_t indices, uint32_t instances, uint32_t first_index,
                             int32_t vertex_offset, uint32_t first_instance);
    void (*cmd_dispatch)(void *list, uint32_t x, uint32_t y, uint32_t z);
    void (*cmd_copy_buffer)(void *list, void *dest, uint64_t dest_offset, void *source,
                            uint64_t source_offset, uint64_t size);
    void (*cmd_copy_buffer_to_texture)(void *list, const tb_rhi_texture_copy_ref *copy);
    void (*cmd_copy_texture_to_buffer)(void *list, const tb_rhi_texture_copy_ref *copy);
    void (*cmd_begin_marker)(void *list, const char *name);
    void (*cmd_end_marker)(void *list);
} tb_rhi_backend_table;

/* The backend that accepts everything and draws nothing. */
extern const tb_rhi_backend_table tb_rhi_null_backend;

/* The Direct3D 12 backend, where the platform builds it. */
extern const tb_rhi_backend_table tb_rhi_d3d12_backend;

/* The Direct3D 11 backend, where the platform builds it. */
extern const tb_rhi_backend_table tb_rhi_d3d11_backend;

/* A backend that belongs on a platform: its table, or NULL while it is not
 * written yet. */
typedef struct tb_rhi_backend_entry
{
    tb_rhi_backend backend;
    const tb_rhi_backend_table *table;
} tb_rhi_backend_entry;

/* The backends of a platform, in the order `auto` tries them. The null
 * backend is in no list: every platform has it, and `auto` never takes it. */
typedef struct tb_rhi_backend_list
{
    const char *platform; /* As messages name it: "Windows". */
    const tb_rhi_backend_entry *entries;
    size_t count;
} tb_rhi_backend_list;

/* This platform's backends. The build selects the source that defines it
 * (win32/, linux/, macos/ or default/), so the order of preference lives
 * with the platform, not in shared code. */
const tb_rhi_backend_list *tb_rhi_platform_backends(void);

/* Make a device as tb_rhi_device_create() does, from a given list of
 * backends in place of the platform's: for tests of the choice itself. */
tb_rhi_result tb_rhi_device_create_from(const tb_rhi_backend_list *list,
                                        const tb_rhi_device_options *options,
                                        tb_rhi_device **device, tb_rhi_error *error);

/* Make a device on a given table, for tests of the device itself. */
tb_rhi_result tb_rhi_device_create_on(const tb_rhi_backend_table *table,
                                      const tb_rhi_device_options *options, tb_rhi_device **device,
                                      tb_rhi_error *error);

/* Fill an error, as every backend does. */
tb_rhi_result tb_rhi_fail(tb_rhi_error *error, tb_rhi_result result, const char *format, ...);

#endif
