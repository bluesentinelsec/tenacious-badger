/* badger-sandbox: a developer program to look at what the engine draws, and
 * how its windows behave, by eye. It is built with the tools and never ships
 * with a game. */
#ifndef SANDBOX_H
#define SANDBOX_H

#include "badger/engine_settings.h"
#include "badger/font.h"
#include "badger/font_catalog.h"
#include "badger/gui.h"
#include "badger/renderer.h"
#include "badger/rhi.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_d3dcompiler;
struct tb_dxc;

/* The fonts every scene may draw text in: the engine catalog's roles, each
 * at its own size (sandbox.specs). A font this build of the engine does not
 * carry is NULL, and what would be drawn in it is not. */
typedef enum sandbox_font
{
    SANDBOX_FONT_UI,        /* the UI role */
    SANDBOX_FONT_BODY,      /* the body role: paragraphs */
    SANDBOX_FONT_HEADING,   /* the heading role */
    SANDBOX_FONT_MONOSPACE, /* the monospace role */
    SANDBOX_FONT_PIXEL,     /* the pixel role, drawn at whole multiples of pixel_size */
    SANDBOX_FONT_COUNT
} sandbox_font;

/* What every scene draws with. */
typedef struct sandbox
{
    tb_renderer *renderer;
    tb_rhi_device *device;
    tb_rhi_device_info info;
    struct tb_dxc *dxc;                 /* for D3D12; NULL for other backends */
    struct tb_d3dcompiler *d3dcompiler; /* for D3D11; NULL for other backends */
    tb_window *window;
    tb_camera camera;         /* the fly camera, which 3D scenes draw from */
    tb_font_catalog *catalog; /* holds the fonts' bytes */
    tb_font *fonts[SANDBOX_FONT_COUNT];
    tb_font_spec specs[SANDBOX_FONT_COUNT]; /* each font's role, and its size */
    float pixel_size;                       /* the pixel font's design size, in pixels to the em */
    float ui_scale;                   /* the interface's zoom over the display's scale: 1 is 100% */
    const tb_engine_settings *engine; /* the engine's settings, which scenes may bind */
    bool cpu_skinning;                /* skinned meshes forced onto the CPU */
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
 * true to take it from the sandbox's keys and fly camera.
 *
 * Every scene takes the display policy from the settings (docs/display.md),
 * as a game does; a tool's scene overrides it, as an app would in its
 * default file. */
typedef struct sandbox_scene
{
    const char *name;
    const char *summary;
    tb_vec4 clear;
    bool (*create)(sandbox *box, void **state);
    void (*draw)(sandbox *box, void *state, const sandbox_frame *frame);
    void (*destroy)(sandbox *box, void *state);
    bool (*event)(sandbox *box, void *state, const tb_window_event *event);
    /* A tool's: no design size, whatever the settings say, so that its
     * units are logical pixels over the whole window. */
    bool logical_units;
    /* The scene takes F1 to F12 before the inspector, which the backquote
     * then shows and hides alone. */
    bool function_keys;
} sandbox_scene;

extern const sandbox_scene sandbox_scenes[];

/* The lighting scene (sandbox_lighting.c). */
bool sandbox_lighting_create(sandbox *box, void **state);
void sandbox_lighting_draw(sandbox *box, void *state, const sandbox_frame *frame);
void sandbox_lighting_destroy(sandbox *box, void *state);
bool sandbox_lighting_event(sandbox *box, void *state, const tb_window_event *event);

/* The skinning scene (sandbox_skinning.c). */
bool sandbox_skinning_create(sandbox *box, void **state);
void sandbox_skinning_draw(sandbox *box, void *state, const sandbox_frame *frame);
void sandbox_skinning_destroy(sandbox *box, void *state);
extern const size_t sandbox_scene_count;

/* The shapes, which other scenes draw under what they show. */
bool shapes_create(sandbox *box, void **state);
void shapes_draw(sandbox *box, void *state, const sandbox_frame *frame);
void shapes_destroy(sandbox *box, void *state);

/* The inspector over every scene, which F1 or the backquote shows and
 * hides: every engine setting, applied as it changes, and the frame's
 * times and statistics (sandbox_inspector.c). */
typedef struct sandbox_inspector sandbox_inspector;
sandbox_inspector *sandbox_inspector_create(sandbox *box, tb_settings *settings,
                                            tb_engine_settings *engine, bool shown);
void sandbox_inspector_destroy(sandbox_inspector *inspector, sandbox *box);
/* True when it takes the event: its key, or input over its windows. */
bool sandbox_inspector_event(sandbox_inspector *inspector, const tb_window_event *event);
/* Between the renderer's begin and end, after the scene. */
void sandbox_inspector_draw(sandbox_inspector *inspector, sandbox *box, const char *scene,
                            double seconds);
/* After the frame ended: its time, and the statistics of what it drew. */
void sandbox_inspector_after_frame(sandbox_inspector *inspector, sandbox *box, double seconds);

/* The interface: every widget, in windows over the shapes (sandbox_ui.c). */
bool ui_scene_create(sandbox *box, void **state);
void ui_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void ui_scene_destroy(sandbox *box, void *state);
bool ui_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* The retained GUI: a form laid out by grid over the shapes (sandbox_gui.c),
 * and a panel of options bound to the engine's settings, in a column of a
 * grid (sandbox_gui_options.c). */
typedef struct gui_options gui_options;
gui_options *gui_options_create(tb_widget parent, int32_t column, const tb_engine_settings *engine);
void gui_options_destroy(gui_options *options);
bool gui_scene_create(sandbox *box, void **state);
void gui_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void gui_scene_destroy(sandbox *box, void *state);
bool gui_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* The retained GUI in the shape of an editor: menus, a paned window of an
 * outliner and a notebook, lists, tooltips, and dialogs (sandbox_gui_tool.c). */
bool gui_tool_scene_create(sandbox *box, void **state);
void gui_tool_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void gui_tool_scene_destroy(sandbox *box, void *state);
bool gui_tool_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* The retained GUI as a small tool built from code: a font browser over the
 * font catalog (sandbox_gui_fonts.c). */
bool gui_fonts_scene_create(sandbox *box, void **state);
void gui_fonts_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void gui_fonts_scene_destroy(sandbox *box, void *state);
bool gui_fonts_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* The display policy and its viewport, shown and changed by hotkeys: a lit
 * scene in the viewport, the design's border, corners and grid, a GUI
 * panel, and what the viewport is (sandbox_display.c). */
bool display_scene_create(sandbox *box, void **state);
void display_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void display_scene_destroy(sandbox *box, void *state);
bool display_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* Input: actions and contexts from a file, fed the window's events, their
 * states shown live (sandbox_input.c). */
bool input_scene_create(sandbox *box, void **state);
void input_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void input_scene_destroy(sandbox *box, void *state);
bool input_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* Audio output status and an explicitly triggered quiet test tone. */
bool audio_scene_create(sandbox *box, void **state);
void audio_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void audio_scene_destroy(sandbox *box, void *state);
bool audio_scene_event(sandbox *box, void *state, const tb_window_event *event);

/* The gamepad scene: every connected pad's buttons, sticks, triggers and
 * virtual buttons shown live, and R to rumble (sandbox_gamepad.c). */
bool gamepad_scene_create(sandbox *box, void **state);
void gamepad_scene_draw(sandbox *box, void *state, const sandbox_frame *frame);
void gamepad_scene_destroy(sandbox *box, void *state);
bool gamepad_scene_event(sandbox *box, void *state, const tb_window_event *event);

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
