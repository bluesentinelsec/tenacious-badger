/** @file render_graph.h
 * @brief The render graph: a frame's passes, the resources they read and
 * write, and the barriers, lifetimes and memory between them worked out for
 * them, as Unreal's render dependency graph does.
 *
 * Each frame, the renderer builds a graph: it creates or imports the
 * textures and buffers the frame uses, and adds passes that say which of
 * them they read and write, and in what state. Executing the graph then
 *
 * - leaves out every pass whose work nothing uses: a pass is kept when it
 *   writes a resource that a kept pass reads, an imported resource (whose
 *   contents outlive the frame), or when it asks never to be culled;
 * - gives each transient texture and buffer memory for the passes between
 *   its first use and its last, reusing what an earlier resource of the same
 *   description is done with, in this frame and from frame to frame;
 * - records the passes in the order they were added, with the barriers that
 *   move each resource into the state each pass declared, and moves imported
 *   resources into the state they are to be left in;
 * - and submits them.
 *
 * @code
 * tb_render_graph_begin(graph);
 * const tb_render_graph_texture back = tb_render_graph_import_texture(
 *     graph, tb_rhi_swap_chain_current(chain), TB_RHI_STATE_PRESENT, TB_RHI_STATE_PRESENT, "back");
 * const tb_render_graph_texture depth = tb_render_graph_create_texture(graph, &depth_desc,
 * "depth"); tb_render_graph_pass pass = {.name = "scene", .execute = draw_scene, .context = scene};
 * pass.colors[0] = (tb_render_graph_color){.texture = back, .load = TB_RHI_LOAD_CLEAR};
 * pass.color_count = 1;
 * pass.depth = (tb_render_graph_depth){.texture = depth, .load = TB_RHI_LOAD_CLEAR};
 * tb_render_graph_add_pass(graph, &pass);
 * (void)tb_render_graph_execute(graph);
 * @endcode
 *
 * **Handles** of resources are good from tb_render_graph_create_texture() or
 * an import until the next tb_render_graph_begin(). A pass's callback turns
 * them into the RHI's with tb_render_graph_rhi_texture() and
 * tb_render_graph_rhi_buffer(), which are good only while the graph
 * executes.
 *
 * @par Threading
 * Externally synchronized: one thread builds and executes a graph at a
 * time. Different graphs are independent.
 */
#ifndef TB_RENDER_GRAPH_H
#define TB_RENDER_GRAPH_H

#include "badger/api.h"
#include "badger/rhi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A render graph. Created by tb_render_graph_create(); owned by the
 * caller; destroyed by tb_render_graph_destroy() before its device. */
typedef struct tb_render_graph tb_render_graph;

/** @brief A texture of the frame's graph. Zero names none. */
typedef struct tb_render_graph_texture
{
    uint32_t id; /**< Private. */
} tb_render_graph_texture;

/** @brief A buffer of the frame's graph. Zero names none. */
typedef struct tb_render_graph_buffer
{
    uint32_t id; /**< Private. */
} tb_render_graph_buffer;

/** @brief How many accesses, besides its targets, a pass may declare. */
#define TB_RENDER_GRAPH_ACCESSES 16u

/** @brief A pass's use of a texture or a buffer other than as a target:
 * name one, and the state the pass uses it in. A state that writes
 * (#TB_RHI_STATE_STORAGE, #TB_RHI_STATE_COPY_DEST) makes the pass a
 * producer of it. */
typedef struct tb_render_graph_access
{
    tb_render_graph_texture texture; /**< The texture, or none. */
    tb_render_graph_buffer buffer;   /**< The buffer, or none. */
    tb_rhi_state state;              /**< How the pass uses it. */
} tb_render_graph_access;

/** @brief A color target of a raster pass. */
typedef struct tb_render_graph_color
{
    tb_render_graph_texture texture; /**< The target. */
    uint32_t mip;                    /**< Its level. */
    uint32_t layer;                  /**< Its image. */
    tb_rhi_load load;                /**< Keep reads what was there; clear and any do not. */
    float clear[4];                  /**< With #TB_RHI_LOAD_CLEAR. */
} tb_render_graph_color;

/** @brief The depth target of a raster pass. */
typedef struct tb_render_graph_depth
{
    tb_render_graph_texture texture; /**< The target, or none. */
    tb_rhi_load load;                /**< As a color target's. */
    float clear;                     /**< With #TB_RHI_LOAD_CLEAR; 0 is the far plane. */
    bool read_only;                  /**< Test depth without writing it. */
} tb_render_graph_depth;

/** @brief What a pass's callback is given. */
typedef struct tb_render_graph_pass_context
{
    tb_render_graph *graph;    /**< For tb_render_graph_rhi_texture() and its peer. */
    tb_rhi_command_list *list; /**< To record into; inside the pass when it has targets. */
    void *context;             /**< The pass's. */
} tb_render_graph_pass_context;

/** @brief Records a pass's commands. */
typedef void tb_render_graph_execute_function(const tb_render_graph_pass_context *pass);

/** @brief A pass to add. Zero-initialize, then fill in. A pass with color or
 * depth targets is a raster pass: its callback records inside a pass of the
 * RHI over them. One without is a compute or copy pass. */
typedef struct tb_render_graph_pass
{
    const char *name;                                          /**< Required; copied. */
    tb_render_graph_access accesses[TB_RENDER_GRAPH_ACCESSES]; /**< What else it uses. */
    uint32_t access_count;                                     /**< How many of `accesses`. */
    tb_render_graph_color colors[TB_RHI_COLOR_TARGETS];        /**< Its color targets. */
    uint32_t color_count;                                      /**< How many of `colors`. */
    tb_render_graph_depth depth;                               /**< Its depth target. */
    /** Keep it even when nothing reads what it writes: a pass with an effect
     * outside the graph, such as a readback for the CPU. */
    bool never_cull;
    tb_render_graph_execute_function *execute; /**< Required. */
    void *context;                             /**< Given to `execute`. */
} tb_render_graph_pass;

/**
 * @brief Make a render graph that records for a device.
 * @param device Required non-null; outlives the graph.
 * @return The graph; never NULL.
 * @par Threading Thread-safe.
 */
TB_API tb_render_graph *tb_render_graph_create(tb_rhi_device *device);

/**
 * @brief Destroy a graph and the transient resources it keeps.
 * @param graph May be NULL.
 */
TB_API void tb_render_graph_destroy(tb_render_graph *graph);

/** @brief Begin a frame's graph: forget the last frame's passes and handles. */
TB_API void tb_render_graph_begin(tb_render_graph *graph);

/**
 * @brief A texture that lives for this frame alone.
 * @param desc Required; its name is ignored.
 * @param name For debuggers and the dump; required; copied.
 * @return Its handle, good until the next tb_render_graph_begin().
 */
TB_API tb_render_graph_texture tb_render_graph_create_texture(tb_render_graph *graph,
                                                              const tb_rhi_texture_desc *desc,
                                                              const char *name);

/**
 * @brief A buffer of #TB_RHI_MEMORY_GPU that lives for this frame alone.
 * @param desc Required; its memory must be GPU memory.
 * @param name Required; copied.
 */
TB_API tb_render_graph_buffer tb_render_graph_create_buffer(tb_render_graph *graph,
                                                            const tb_rhi_buffer_desc *desc,
                                                            const char *name);

/**
 * @brief A texture that outlives the frame, such as a swap chain's image.
 * @param texture Required; alive while the graph executes.
 * @param state The state it is in now.
 * @param final_state The state to leave it in.
 * @param name Required; copied.
 */
TB_API tb_render_graph_texture tb_render_graph_import_texture(tb_render_graph *graph,
                                                              tb_rhi_texture texture,
                                                              tb_rhi_state state,
                                                              tb_rhi_state final_state,
                                                              const char *name);

/** @brief A buffer that outlives the frame; as tb_render_graph_import_texture(). */
TB_API tb_render_graph_buffer tb_render_graph_import_buffer(tb_render_graph *graph,
                                                            tb_rhi_buffer buffer,
                                                            tb_rhi_state state,
                                                            tb_rhi_state final_state,
                                                            const char *name);

/**
 * @brief Add a pass, after those added before it.
 * @param pass Required; copied.
 */
TB_API void tb_render_graph_add_pass(tb_render_graph *graph, const tb_render_graph_pass *pass);

/**
 * @brief Cull, allocate, record and submit the frame's passes.
 * @return The submission's number, as tb_rhi_submit() gives it.
 */
TB_API uint64_t tb_render_graph_execute(tb_render_graph *graph);

/** @brief The RHI's texture behind a handle, while the graph executes. */
TB_API tb_rhi_texture tb_render_graph_rhi_texture(const tb_render_graph *graph,
                                                  tb_render_graph_texture texture);

/** @brief The RHI's buffer behind a handle, while the graph executes. */
TB_API tb_rhi_buffer tb_render_graph_rhi_buffer(const tb_render_graph *graph,
                                                tb_render_graph_buffer buffer);

/**
 * @brief Describe the last executed frame: each pass, kept or culled, and
 * the barriers before it; and each transient resource, with the memory it
 * was given. One line each, for the console and tests.
 * @param buffer Receives the text, NUL-terminated and cut to fit; may be
 * NULL when `capacity` is 0.
 * @return The bytes the whole text takes, its NUL excluded.
 */
TB_API size_t tb_render_graph_dump(const tb_render_graph *graph, char *buffer, size_t capacity);

/** @brief Transient resources the graph keeps between frames, for tests and
 * stat overlays. */
TB_API size_t tb_render_graph_pooled(const tb_render_graph *graph);

#endif
