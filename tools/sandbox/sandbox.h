/* badger-sandbox: a developer program to look at what the engine draws, and
 * how its windows behave, by eye. It is built with the tools and never ships
 * with a game. */
#ifndef SANDBOX_H
#define SANDBOX_H

#include "badger/engine_settings.h"
#include "badger/game_loop.h"
#include "badger/rhi.h"
#include "badger/shader.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_dxc;

/* What every scene draws with. */
typedef struct sandbox
{
    tb_rhi_device *device;
    tb_rhi_device_info info;
    tb_rhi_swap_chain *chain;
    struct tb_dxc *dxc; /* for D3D12; NULL for the null backend */
    tb_window *window;
    tb_rhi_texture depth; /* the swap chain's size */
    uint32_t width;       /* of the swap chain, in pixels */
    uint32_t height;
    bool vsync;
} sandbox;

/* What a scene draws into this frame: the swap chain's image, in
 * TB_RHI_STATE_RENDER_TARGET, and the depth texture, in
 * TB_RHI_STATE_DEPTH_WRITE, already in a pass the scene's draw records into. */
typedef struct sandbox_frame
{
    tb_rhi_command_list *list;
    double seconds; /* since the scene began */
    float aspect;   /* width over height */
} sandbox_frame;

/* A scene: made when it is chosen, drawn each frame, released when another
 * is chosen. `state` is the scene's own. */
typedef struct sandbox_scene
{
    const char *name;
    const char *summary;
    bool (*create)(sandbox *box, void **state);
    /* Record what must happen before the frame's pass, such as uploads;
     * NULL for none. */
    void (*prepare)(sandbox *box, void *state, tb_rhi_command_list *list);
    void (*draw)(sandbox *box, void *state, const sandbox_frame *frame);
    void (*destroy)(sandbox *box, void *state);
} sandbox_scene;

extern const sandbox_scene sandbox_scenes[];
extern const size_t sandbox_scene_count;

/* Make a shader for the device's backend from HLSL in memory: compiled to
 * DXIL through DXC for D3D12; the text itself for the null backend, which
 * accepts anything. Errors are printed. */
bool sandbox_shader(sandbox *box, const char *path, const char *source, const char *entry,
                    tb_rhi_stage stage, tb_rhi_shader *shader);

/* A graphics pipeline that draws into the swap chain and the depth texture. */
tb_rhi_graphics_pipeline_desc sandbox_pipeline_desc(const sandbox *box);

/* Write a BGRA image, top row first, as a .bmp file. */
bool sandbox_write_bmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height,
                       uint32_t pitch);

#endif
