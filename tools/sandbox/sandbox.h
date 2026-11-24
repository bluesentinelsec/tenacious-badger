/* badger-sandbox: a developer program to look at what the engine draws, and
 * how its windows behave, by eye. It is built with the tools and never ships
 * with a game. */
#ifndef SANDBOX_H
#define SANDBOX_H

#include "badger/font.h"
#include "badger/font_catalog.h"
#include "badger/renderer.h"
#include "badger/rhi.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_dxc;

/* The fonts every scene may draw text in, from the engine's catalog. A
 * font this build of the engine does not carry is NULL, and what would be
 * drawn in it is not. */
typedef enum sandbox_font
{
    SANDBOX_FONT_UI,         /* the catalog's UI role: smooth text */
    SANDBOX_FONT_VGA,        /* its pixel role, Px437 IBM VGA 8x16: 16 pixels to the em */
    SANDBOX_FONT_SILKSCREEN, /* Silkscreen: a pixel font of 8 pixels to the em */
    SANDBOX_FONT_COUNT
} sandbox_font;

/* What every scene draws with. */
typedef struct sandbox
{
    tb_renderer *renderer;
    tb_rhi_device *device;
    tb_rhi_device_info info;
    struct tb_dxc *dxc; /* for D3D12; NULL for the null backend */
    tb_window *window;
    tb_camera camera;         /* the fly camera, which 3D scenes draw from */
    tb_font_catalog *catalog; /* holds the fonts' bytes */
    tb_font *fonts[SANDBOX_FONT_COUNT];
    float ui_scale; /* the interface's zoom over the display's scale: 1 is 100% */
} sandbox;

/* A frame of a scene. */
typedef struct sandbox_frame
{
    double seconds;       /* since the scene began */
    double delta_seconds; /* since the last frame */
} sandbox_frame;

/* A scene: made when it is chosen, drawn each frame between the renderer's
 * begin and end, released when another is chosen. `state` is the scene's.
 * `event`, when a scene has one, sees each window event first, and returns
 * true to take it from the sandbox's keys and fly camera. */
typedef struct sandbox_scene
{
    const char *name;
    const char *summary;
    tb_vec4 clear;
    bool (*create)(sandbox *box, void **state);
    void (*draw)(sandbox *box, void *state, const sandbox_frame *frame);
    void (*destroy)(sandbox *box, void *state);
    bool (*event)(sandbox *box, void *state, const tb_window_event *event);
} sandbox_scene;

extern const sandbox_scene sandbox_scenes[];

/* The skinning scene (sandbox_skinning.c). */
bool sandbox_skinning_create(sandbox *box, void **state);
void sandbox_skinning_draw(sandbox *box, void *state, const sandbox_frame *frame);
void sandbox_skinning_destroy(sandbox *box, void *state);
extern const size_t sandbox_scene_count;

/* The shapes, which other scenes draw under what they show. */
bool shapes_create(sandbox *box, void **state);
void shapes_draw(sandbox *box, void *state, const sandbox_frame *frame);
void shapes_destroy(sandbox *box, void *state);

/* The interface: every widget, in windows over the shapes (sandbox_ui.c). */
bool ui_scene_create(sandbox *box, void **state);
void ui_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void ui_scene_destroy(sandbox *box, void *state);
bool ui_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* Make a shader for the device's backend from HLSL in memory: compiled to
 * DXIL through DXC for D3D12; the text itself for the null backend, which
 * accepts anything. Errors are printed. */
bool sandbox_shader(sandbox *box, const char *path, const char *source, const char *entry,
                    tb_rhi_stage stage, tb_rhi_shader *shader);

/* A graphics pipeline that draws in the renderer's scene pass. */
tb_rhi_graphics_pipeline_desc sandbox_pipeline_desc(const sandbox *box);

/* Write a BGRA image, top row first, as a .bmp file. */
bool sandbox_write_bmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height,
                       uint32_t pitch);

#endif
