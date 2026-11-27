/* badger-sandbox: opens a window with the engine's settings and game loop,
 * and draws one of its scenes through the renderer. Keys 1 to 9 choose a
 * scene and Tab the next; holding the right mouse button flies the camera
 * (WASD, Q and E down and up, Shift faster, the wheel changes speed). With
 * --frames it stops after that many frames, and with --capture it saves the
 * last as a .bmp first, so a scene can become a test. The frame time is
 * drawn in the corner of every scene, and printed. */
#include "sandbox.h"

#include "badger/arena.h"
#include "badger/assert.h"
#include "badger/canvas.h"
#include "badger/cli.h"
#include "badger/crash.h"
#include "badger/d3dcompiler.h"
#include "badger/dxc.h"
#include "badger/engine_settings.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/game_loop.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/process.h"
#include "badger/settings.h"
#include "badger/shader.h"
#include "badger/shader_cache.h"
#include "badger/skinning.h"
#include "badger/str.h"
#include "badger/version.h"
#include "badger/vfs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SANDBOX_TITLE "badger-sandbox"
#define SANDBOX_MOST_VALUES 32
/* Bytes of a path the sandbox makes. */
#define SANDBOX_PATH_CAPACITY ((size_t)4096)
/* Seconds between the frame times the sandbox prints. */
#define SANDBOX_REPORT_SECONDS 2.0
/* Seconds between changes of the frame time drawn on screen, and how much
 * of each frame's time goes into the average it shows. */
#define SANDBOX_SHOW_SECONDS 0.25
#define SANDBOX_SHOW_WEIGHT 0.1
/* The frame time's text: its size, and its margin from the window's corner,
 * in logical units. */
#define SANDBOX_STATUS_SIZE 13.0f
#define SANDBOX_STATUS_MARGIN 8.0f
#define SANDBOX_STATUS_LAYER 1000

/* The fly camera: radians a unit of mouse motion turns it, meters a second
 * it moves, and how much Shift and a notch of the wheel change that. */
#define SANDBOX_LOOK_SPEED 0.0025f
#define SANDBOX_MOVE_SPEED 4.0f
#define SANDBOX_FAST 4.0f
#define SANDBOX_WHEEL_STEP 1.2f
#define SANDBOX_MOST_PITCH 1.55f

/* The command line. */
static const char *sandbox_renderer_name = NULL;
static const char *sandbox_scene_name = "shapes";
static int64_t sandbox_frames = 0;
static const char *sandbox_capture_path = NULL;
static int64_t sandbox_ui_scale = 100;
static bool sandbox_show_inspector = false;
static bool sandbox_software = false;
static bool sandbox_cpu_skinning = false;
static bool sandbox_validation = false;
static const char *sandbox_values[SANDBOX_MOST_VALUES] = {0};
static size_t sandbox_value_count = 0;
static const char *sandbox_log_file = NULL;

/* The keys the fly camera reads, held or not. */
typedef enum sandbox_key
{
    SANDBOX_FORWARD,
    SANDBOX_BACK,
    SANDBOX_LEFT,
    SANDBOX_RIGHT,
    SANDBOX_DOWN,
    SANDBOX_UP,
    SANDBOX_FASTER,
    SANDBOX_KEY_COUNT
} sandbox_key;

/* What runs while the window is open. */
typedef struct sandbox_run
{
    sandbox box;
    tb_settings *settings;
    tb_engine_settings engine;
    tb_shader_cache *shader_cache; /* the user's, which badger-shaderc shares */
    tb_game_loop *loop;
    sandbox_inspector *inspector;
    uint64_t vsync_version; /* of display.vsync, when the renderer last took it */
    size_t scene;
    void *state;
    bool scene_ready;
    double scene_started;
    double clock;
    /* The fly camera. */
    float yaw;
    float pitch;
    float speed;
    bool looking;
    bool held[SANDBOX_KEY_COUNT];
    /* Frame times since the last report. */
    double reported;
    uint64_t frames;
    double slowest;
    /* The frame time drawn on screen: an average, and when it last changed. */
    double average;
    double shown;
    char status_line[96];
    int status;
} sandbox_run;

/* ---------------------------------------------------------------------------
 * Shaders and pipelines.
 * ------------------------------------------------------------------------- */

static tb_shader_stage sandbox_stage(tb_rhi_stage stage)
{
    switch (stage)
    {
    case TB_RHI_STAGE_VERTEX:
        return TB_SHADER_STAGE_VERTEX;
    case TB_RHI_STAGE_PIXEL:
        return TB_SHADER_STAGE_PIXEL;
    case TB_RHI_STAGE_COMPUTE:
        break;
    }
    return TB_SHADER_STAGE_COMPUTE;
}

bool sandbox_shader(sandbox *box, const char *path, const char *source, const char *entry,
                    tb_rhi_stage stage, tb_rhi_shader *shader)
{
    const tb_shader_file file = {path, source, tb_str_length(source)};
    tb_shader_compile_request request = {0};
    tb_shader_compiled compiled = {0};
    tb_arena *const arena = tb_arena_create(0);
    tb_rhi_shader_desc desc = {.stage = stage, .entry = entry, .name = path};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    bool made = false;

    if (box->no_compiler[0] != '\0')
    {
        (void)tb_fmt_print_error("badger-sandbox: %s compiles its shaders as it runs, and %s\n",
                                 path, box->no_compiler);
        goto done;
    }
    request.source.path = path;
    request.source.files = &file;
    request.source.file_count = 1;
    request.entry_point = entry;
    request.stage = sandbox_stage(stage);
    request.target = box->dxc != NULL           ? TB_SHADER_TARGET_DXIL
                     : box->d3dcompiler != NULL ? TB_SHADER_TARGET_DXBC
                                                : TB_SHADER_TARGET_NONE;
    request.dxc = box->dxc;
    request.d3dcompiler = box->d3dcompiler;
    if (tb_shader_compile(&request, arena, &compiled) != TB_SHADER_OK)
    {
        for (size_t index = 0; index < compiled.diagnostic_count; index++)
        {
            char line[512] = {0};
            (void)tb_shader_diagnostic_format(&compiled.diagnostics[index], line, sizeof(line));
            (void)tb_fmt_print_error("badger-sandbox: %s\n", line);
        }
        goto done;
    }
    /* The null backend runs nothing, so the checked source stands in. */
    desc.code = compiled.code != NULL ? compiled.code : (const void *)source;
    desc.size = compiled.code != NULL ? compiled.code_size : file.length;
    if (tb_rhi_shader_create(box->device, &desc, shader, &error) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: %s %s: %s\n", path, entry, error.message);
        goto done;
    }
    made = true;
done:
    tb_arena_destroy(arena);
    return made;
}

tb_rhi_graphics_pipeline_desc sandbox_pipeline_desc(const sandbox *box)
{
    tb_rhi_graphics_pipeline_desc desc = {0};

    tb_renderer_target_formats(box->renderer, &desc.colors[0], &desc.depth);
    desc.color_count = 1;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER; /* reversed depth */
    return desc;
}

/* ---------------------------------------------------------------------------
 * Scenes.
 * ------------------------------------------------------------------------- */

static void sandbox_leave_scene(sandbox_run *run)
{
    if (run->scene_ready)
    {
        (void)tb_rhi_wait_idle(run->box.device);
        sandbox_scenes[run->scene].destroy(&run->box, run->state);
    }
    run->scene_ready = false;
    run->state = NULL;
}

static void sandbox_enter_scene(sandbox_run *run, size_t scene)
{
    sandbox_leave_scene(run);
    run->scene = scene;
    run->scene_started = run->clock;
    run->scene_ready = sandbox_scenes[scene].create(&run->box, &run->state);
    if (!run->scene_ready)
    {
        sandbox_scenes[scene].destroy(&run->box, run->state);
        run->state = NULL;
        (void)tb_fmt_print_error("badger-sandbox: the scene %s could not be made\n",
                                 sandbox_scenes[scene].name);
        run->status = EXIT_FAILURE;
        return;
    }
    (void)tb_fmt_print("badger-sandbox: %s: %s\n", sandbox_scenes[scene].name,
                       sandbox_scenes[scene].summary);
}

/* ---------------------------------------------------------------------------
 * The fly camera.
 * ------------------------------------------------------------------------- */

static void sandbox_hold(sandbox_run *run, tb_key key, bool down)
{
    static const tb_key keys[SANDBOX_KEY_COUNT] = {
        [SANDBOX_FORWARD] = TB_KEY_W,        [SANDBOX_BACK] = TB_KEY_S, [SANDBOX_LEFT] = TB_KEY_A,
        [SANDBOX_RIGHT] = TB_KEY_D,          [SANDBOX_DOWN] = TB_KEY_Q, [SANDBOX_UP] = TB_KEY_E,
        [SANDBOX_FASTER] = TB_KEY_LEFT_SHIFT};

    for (int index = 0; index < SANDBOX_KEY_COUNT; index++)
    {
        if (keys[index] == key)
        {
            run->held[index] = down;
        }
    }
}

static void sandbox_look(sandbox_run *run, bool looking)
{
    if (looking == run->looking)
    {
        return;
    }
    /* Where the system has no relative mode, the camera does not turn. */
    run->looking =
        tb_window_set_relative_mouse(run->box.window, looking, NULL) == TB_WINDOW_OK && looking;
}

static void sandbox_fly(sandbox_run *run, double seconds)
{
    tb_camera *const camera = &run->box.camera;
    const tb_quat turn = tb_quat_mul(tb_quat_from_axis_angle((tb_vec3){0, 1, 0}, run->yaw),
                                     tb_quat_from_axis_angle((tb_vec3){1, 0, 0}, run->pitch));
    const float step =
        run->speed * (run->held[SANDBOX_FASTER] ? SANDBOX_FAST : 1.0f) * (float)seconds;
    tb_vec3 move = {0};

    camera->orientation = turn;
    move.z -= run->held[SANDBOX_FORWARD] ? 1.0f : 0.0f;
    move.z += run->held[SANDBOX_BACK] ? 1.0f : 0.0f;
    move.x -= run->held[SANDBOX_LEFT] ? 1.0f : 0.0f;
    move.x += run->held[SANDBOX_RIGHT] ? 1.0f : 0.0f;
    move = tb_quat_rotate(turn, tb_vec3_normalize(move));
    move.y += run->held[SANDBOX_UP] ? 1.0f : 0.0f;
    move.y -= run->held[SANDBOX_DOWN] ? 1.0f : 0.0f;
    camera->position = tb_vec3_add(camera->position, tb_vec3_scale(move, step));
}

/* A key that chooses a scene: 1 to 9, or Tab for the next. */
static bool sandbox_choose(sandbox_run *run, tb_key key)
{
    if (key >= TB_KEY_1 && key <= TB_KEY_9 && (size_t)(key - TB_KEY_1) < sandbox_scene_count)
    {
        sandbox_enter_scene(run, (size_t)(key - TB_KEY_1));
        return true;
    }
    if (key == TB_KEY_TAB)
    {
        sandbox_enter_scene(run, (run->scene + 1) % sandbox_scene_count);
        return true;
    }
    return false;
}

/* Give the scene the event first; true when it takes it. A key or button
 * that comes up is still let go of, so that nothing stays held. */
static bool sandbox_scene_event(sandbox_run *run, const tb_window_event *event)
{
    const sandbox_scene *const scene = &sandbox_scenes[run->scene];

    if (!run->scene_ready || scene->event == NULL || run->looking ||
        !scene->event(&run->box, run->state, event))
    {
        return false;
    }
    if (event->type == TB_WINDOW_EVENT_KEY_UP)
    {
        sandbox_hold(run, event->key, false);
    }
    return true;
}

/* A function key, which a scene may take before the inspector. */
static bool sandbox_function_key(const sandbox_run *run, const tb_window_event *event)
{
    const bool keyed =
        event->type == TB_WINDOW_EVENT_KEY_DOWN || event->type == TB_WINDOW_EVENT_KEY_UP;

    return sandbox_scenes[run->scene].function_keys && keyed && event->key >= TB_KEY_F1 &&
           event->key <= TB_KEY_F12;
}

static bool sandbox_event(void *context, const tb_window_event *event)
{
    sandbox_run *const run = context;
    const tb_window_event_type type = event->type;

    if (sandbox_function_key(run, event) && sandbox_scene_event(run, event))
    {
        return true;
    }
    if (!run->looking && run->inspector != NULL && sandbox_inspector_event(run->inspector, event))
    {
        if (type == TB_WINDOW_EVENT_KEY_UP)
        {
            sandbox_hold(run, event->key, false);
        }
        return true;
    }
    if (sandbox_scene_event(run, event))
    {
        return true;
    }

    if (type == TB_WINDOW_EVENT_KEY_DOWN || type == TB_WINDOW_EVENT_KEY_UP)
    {
        sandbox_hold(run, event->key, type == TB_WINDOW_EVENT_KEY_DOWN);
        return type == TB_WINDOW_EVENT_KEY_DOWN && !event->repeat &&
               sandbox_choose(run, event->key);
    }
    if ((type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN || type == TB_WINDOW_EVENT_MOUSE_BUTTON_UP) &&
        event->button == TB_MOUSE_BUTTON_RIGHT)
    {
        sandbox_look(run, type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN);
        return false;
    }
    if (type == TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION)
    {
        run->yaw -= (float)event->delta_x * SANDBOX_LOOK_SPEED;
        run->pitch -= (float)event->delta_y * SANDBOX_LOOK_SPEED;
        run->pitch = tb_clamp(run->pitch, -SANDBOX_MOST_PITCH, SANDBOX_MOST_PITCH);
        return true;
    }
    if (type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        run->speed =
            tb_clamp(run->speed * powf(SANDBOX_WHEEL_STEP, (float)event->delta_y), 0.1f, 200.0f);
        return true;
    }
    if (type == TB_WINDOW_EVENT_FOCUS_LOST)
    {
        memset(run->held, 0, sizeof(run->held));
        sandbox_look(run, false);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

static void sandbox_report(sandbox_run *run, double seconds)
{
    run->average = run->average == 0.0
                       ? seconds
                       : run->average + (seconds - run->average) * SANDBOX_SHOW_WEIGHT;
    if (run->status_line[0] == '\0' || run->clock - run->shown >= SANDBOX_SHOW_SECONDS)
    {
        (void)tb_fmt_buffer(run->status_line, sizeof(run->status_line),
                            "%.2f ms a frame (%.0f a second)", run->average * 1000.0,
                            run->average > 0.0 ? 1.0 / run->average : 0.0);
        run->shown = run->clock;
    }
    run->frames++;
    run->slowest = seconds > run->slowest ? seconds : run->slowest;
    if (run->clock - run->reported < SANDBOX_REPORT_SECONDS)
    {
        return;
    }
    (void)tb_fmt_print("badger-sandbox: %.2f ms a frame on average, %.2f ms at worst (%.0f a "
                       "second)\n",
                       (run->clock - run->reported) * 1000.0 / (double)run->frames,
                       run->slowest * 1000.0, (double)run->frames / (run->clock - run->reported));
    (void)tb_fmt_flush();
    run->reported = run->clock;
    run->frames = 0;
    run->slowest = 0.0;
}

/* The frame time, in the bottom left corner over everything. */
static void sandbox_draw_status(sandbox_run *run)
{
    tb_renderer *const renderer = run->box.renderer;
    const tb_font *const font = run->box.fonts[SANDBOX_FONT_UI];
    const tb_text line = tb_text_from_cstr(run->status_line);
    const tb_canvas_view view = tb_canvas_view_logical(renderer);
    uint32_t width = 0;
    uint32_t height = 0;
    tb_vec2 size = {0};
    tb_vec2 at = {0};

    if (font == NULL)
    {
        return; /* a build of the engine that carries no fonts */
    }
    tb_renderer_size(renderer, &width, &height);
    tb_canvas_set_view(renderer, &view);
    tb_canvas_set_layer(renderer, SANDBOX_STATUS_LAYER);
    size = tb_canvas_measure_text(renderer, font, line, SANDBOX_STATUS_SIZE, NULL);
    at = (tb_vec2){SANDBOX_STATUS_MARGIN * 2.0f,
                   (float)height / view.scale - SANDBOX_STATUS_MARGIN * 2.0f - size.y};
    tb_canvas_fill_rounded_rect(renderer,
                                (tb_canvas_rect){SANDBOX_STATUS_MARGIN,
                                                 at.y - SANDBOX_STATUS_MARGIN,
                                                 size.x + SANDBOX_STATUS_MARGIN * 2.0f,
                                                 size.y + SANDBOX_STATUS_MARGIN * 2.0f},
                                6.0f, (tb_vec4){0.0f, 0.0f, 0.0f, 0.6f});
    tb_canvas_draw_text(renderer, font, line, SANDBOX_STATUS_SIZE, at,
                        (tb_vec4){0.9f, 0.95f, 1.0f, 1.0f}, NULL);
}

/* Whether this frame is the last of --frames, which --capture saves. */
static bool sandbox_capturing(const tb_game_time *time)
{
    return sandbox_capture_path != NULL && sandbox_frames > 0 &&
           time->frame + 1 == (uint64_t)sandbox_frames;
}

static void sandbox_save_capture(sandbox_run *run)
{
    uint32_t width = 0;
    uint32_t height = 0;
    const uint8_t *const pixels = tb_renderer_captured(run->box.renderer, &width, &height);

    if (pixels != NULL &&
        sandbox_write_bmp(sandbox_capture_path, pixels, width, height, width * 4u))
    {
        (void)tb_fmt_print("badger-sandbox: saved %s\n", sandbox_capture_path);
        return;
    }
    (void)tb_fmt_print_error("badger-sandbox: %s could not be written\n", sandbox_capture_path);
    run->status = EXIT_FAILURE;
}

/* Settings that the renderer and camera take as they change; the game loop
 * applies the window's. */
static void sandbox_apply_settings(sandbox_run *run)
{
    const uint64_t vsync = tb_setting_version(run->engine.display.vsync);
    tb_renderer_quality quality = TB_RENDERER_QUALITY_INIT;
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;

    if (vsync != run->vsync_version)
    {
        tb_renderer_set_vsync(run->box.renderer, tb_setting_choice(run->engine.display.vsync) != 0);
        run->vsync_version = vsync;
    }
    run->box.camera.vertical_fov =
        tb_radians((float)tb_setting_number(run->engine.graphics.field_of_view));
    /* The render scale, anti-aliasing, lights and reflections, within what
     * the renderer's profile allows. */
    tb_renderer_quality_from_settings(&run->engine, &quality);
    tb_renderer_set_quality(run->box.renderer, &quality);
    /* The display policy, as a game takes it, but a tool's scene's. */
    tb_viewport_policy_from_settings(&run->engine, &policy);
    if (sandbox_scenes[run->scene].logical_units)
    {
        policy.design_width = 0;
        policy.design_height = 0;
    }
    tb_renderer_set_viewport_policy(run->box.renderer, &policy,
                                    tb_viewport_aspect_ratio_from_settings(&run->engine));
}

static void sandbox_frame_callback(void *context, const tb_game_time *time)
{
    sandbox_run *const run = context;
    sandbox_frame frame = {0};

    run->clock += time->frame_seconds;
    sandbox_report(run, time->frame_seconds);
    sandbox_fly(run, time->frame_seconds);
    if (sandbox_capturing(time))
    {
        tb_renderer_capture_next_frame(run->box.renderer);
    }
    tb_renderer_set_anisotropy(run->box.renderer, tb_engine_settings_anisotropy(&run->engine));
    sandbox_apply_settings(run);
    if (!tb_renderer_begin_frame(run->box.renderer, &run->box.camera,
                                 sandbox_scenes[run->scene].clear))
    {
        return;
    }
    if (run->scene_ready)
    {
        frame.seconds = run->clock - run->scene_started;
        frame.delta_seconds = time->frame_seconds;
        sandbox_scenes[run->scene].draw(&run->box, run->state, &frame);
    }
    if (run->inspector != NULL)
    {
        sandbox_inspector_draw(run->inspector, &run->box, sandbox_scenes[run->scene].name,
                               time->frame_seconds);
    }
    sandbox_draw_status(run);
    if (tb_renderer_end_frame(run->box.renderer) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: the device was lost\n");
        run->status = EXIT_FAILURE;
        tb_game_loop_quit(run->loop);
        return;
    }
    if (run->inspector != NULL)
    {
        sandbox_inspector_after_frame(run->inspector, &run->box, time->frame_seconds);
    }
    if (sandbox_capturing(time))
    {
        sandbox_save_capture(run);
    }
}

/* ---------------------------------------------------------------------------
 * Starting and stopping.
 * ------------------------------------------------------------------------- */

static int sandbox_set(sandbox_run *run, const char *name, const char *value, const char *given)
{
    const tb_setting_result result =
        tb_settings_set(run->settings, TB_SETTING_LAYER_COMMAND_LINE, name, value);

    if (result != TB_SETTING_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: %s: %s\n", given,
                                 tb_setting_result_name(result));
        return TB_CLI_EXIT_USAGE;
    }
    return EXIT_SUCCESS;
}

static int sandbox_load_settings(sandbox_run *run)
{
    int status = EXIT_SUCCESS;

    run->settings = tb_settings_create();
    tb_engine_settings_add(run->settings, &(tb_engine_settings_options){.developer = true},
                           &run->engine);
    for (size_t index = 0; index < sandbox_value_count && status == EXIT_SUCCESS; index++)
    {
        const char *const assignment = sandbox_values[index];
        const size_t equals = tb_str_find_char(assignment, 0, TB_STR_END, '=');
        char *name = NULL;
        if (equals == TB_STR_NOT_FOUND)
        {
            (void)tb_fmt_print_error("badger-sandbox: error: --set %s: expected NAME=VALUE\n",
                                     assignment);
            return TB_CLI_EXIT_USAGE;
        }
        name = tb_str_sub(NULL, assignment, 0, equals);
        status = sandbox_set(run, name, assignment + equals + 1, assignment);
        tb_str_free(NULL, name);
    }
    if (status == EXIT_SUCCESS && sandbox_renderer_name != NULL)
    {
        status = sandbox_set(run, "graphics.renderer", sandbox_renderer_name, "--renderer");
    }
    if (status == EXIT_SUCCESS && sandbox_frames > 0)
    {
        (void)tb_setting_set_integer(run->engine.developer.frame_count, sandbox_frames);
    }
    return status;
}

static size_t sandbox_find_scene(const char *name)
{
    for (size_t index = 0; index < sandbox_scene_count; index++)
    {
        if (tb_str_equal(sandbox_scenes[index].name, name))
        {
            return index;
        }
    }
    return sandbox_scene_count;
}

/* Open the fonts every scene may draw with: the engine catalog's roles, at
 * their own sizes. A build that carries none of them draws no text. */
static void sandbox_open_fonts(sandbox *box)
{
    static const tb_font_role roles[SANDBOX_FONT_COUNT] = {
        [SANDBOX_FONT_UI] = TB_FONT_ROLE_UI,
        [SANDBOX_FONT_BODY] = TB_FONT_ROLE_BODY,
        [SANDBOX_FONT_HEADING] = TB_FONT_ROLE_HEADING,
        [SANDBOX_FONT_MONOSPACE] = TB_FONT_ROLE_MONOSPACE,
        [SANDBOX_FONT_PIXEL] = TB_FONT_ROLE_PIXEL,
    };

    box->catalog = tb_font_catalog_create(NULL);
    for (size_t font = 0; font < SANDBOX_FONT_COUNT; font++)
    {
        size_t index = 0;
        tb_font_info info = {0};
        box->specs[font] = tb_font_spec_of_role(box->catalog, roles[font]);
        if (!tb_font_catalog_find_spec(box->catalog, box->specs[font], &index) ||
            !tb_font_catalog_open(box->catalog, index, NULL, &box->fonts[font]))
        {
            continue;
        }
        tb_font_catalog_get_info(box->catalog, index, &info);
        if (font == SANDBOX_FONT_PIXEL)
        {
            box->pixel_size = (float)info.pixel_size;
        }
    }
    if (box->fonts[SANDBOX_FONT_UI] == NULL)
    {
        (void)tb_fmt_print("badger-sandbox: this build of the engine carries no UI font; text "
                           "is not drawn\n");
    }
}

/* Where the renderer finds its shaders: the libraries the build writes
 * beside the executable (bin/Shaders/d3d12.tbsl, d3d11.tbsl), in a VFS for the caller to
 * destroy; and the user's shader cache, which badger-shaderc shares. */
static tb_vfs *sandbox_shader_sources(sandbox_run *run)
{
    char directory[SANDBOX_PATH_CAPACITY] = {0};
    char cache[SANDBOX_PATH_CAPACITY] = {0};
    size_t required = 0;
    tb_vfs *const vfs = tb_vfs_create(NULL);

    if (tb_fs_get_location(TB_FS_EXECUTABLE, directory, sizeof(directory), &required) == TB_FS_OK)
    {
        (void)tb_vfs_mount_directory(vfs, "", 0, directory, 0);
    }
    if (tb_shader_cache_default_directory(cache, sizeof(cache)))
    {
        (void)tb_shader_cache_open(cache, &run->shader_cache);
    }
    return vfs;
}

/* The compiler of the backend's shaders, for the scenes that compile their
 * own (the renderer's come from its shader libraries). Without one, as on a
 * machine without the Windows SDK or under Proton, which has no DXC, or with
 * shaders.runtime_compile off, those scenes say why they cannot be drawn and
 * every other scene still is. */
static void sandbox_open_compiler(sandbox_run *run)
{
    sandbox *const box = &run->box;
    const bool needs_dxc = box->info.backend == TB_RHI_BACKEND_D3D12;
    const bool needs_d3dcompiler = box->info.backend == TB_RHI_BACKEND_D3D11;
    tb_dxc_error dxc_error = TB_DXC_ERROR_INIT;
    tb_d3dcompiler_error d3dcompiler_error = TB_D3DCOMPILER_ERROR_INIT;

    if (!needs_dxc && !needs_d3dcompiler)
    {
        return;
    }
    if (!tb_setting_boolean(run->engine.shaders.runtime_compile))
    {
        (void)tb_fmt_buffer(box->no_compiler, sizeof(box->no_compiler),
                            "shaders.runtime_compile is off");
    }
    else if (needs_dxc && tb_dxc_open(&box->dxc, &dxc_error) != TB_DXC_OK)
    {
        (void)tb_fmt_buffer(box->no_compiler, sizeof(box->no_compiler), "%s", dxc_error.message);
    }
    else if (needs_d3dcompiler &&
             tb_d3dcompiler_open(&box->d3dcompiler, &d3dcompiler_error) != TB_D3DCOMPILER_OK)
    {
        /* coverage-off: D3DCompiler is part of Windows */
        (void)tb_fmt_buffer(box->no_compiler, sizeof(box->no_compiler), "%s",
                            d3dcompiler_error.message);
        /* coverage-on */
    }
    if (box->no_compiler[0] != '\0')
    {
        (void)tb_fmt_print_error("badger-sandbox: no shader compiler, so the scenes that compile "
                                 "their own shaders cannot be drawn: %s\n",
                                 box->no_compiler);
    }
}

static int sandbox_open_renderer(sandbox_run *run, tb_window_system *system)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_window_options options = {0};
    tb_window_error window_error = TB_WINDOW_ERROR_INIT;
    tb_renderer_options renderer = {0};

    tb_engine_settings_window_options(&run->engine, SANDBOX_TITLE, &options);
    if (tb_window_create(system, &options, &run->box.window, &window_error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no window: %s\n", window_error.message);
        return EXIT_FAILURE;
    }
    renderer.window = run->box.window;
    renderer.vsync = tb_setting_choice(run->engine.display.vsync) != 0;
    renderer.device.adapter =
        sandbox_software ? TB_RHI_ADAPTER_SOFTWARE : TB_RHI_ADAPTER_HIGH_PERFORMANCE;
    renderer.device.validation = sandbox_validation;
    renderer.anisotropy = tb_engine_settings_anisotropy(&run->engine);
    tb_engine_settings_device_options(&run->engine, &renderer.device);
    tb_vfs *const shaders = sandbox_shader_sources(run);
    renderer.shader_vfs = shaders;
    renderer.shader_cache = run->shader_cache;
    renderer.shader_library_only = !tb_setting_boolean(run->engine.shaders.runtime_compile);
    const tb_rhi_result made = tb_renderer_create(&renderer, &run->box.renderer, &error);
    tb_vfs_destroy(shaders);
    if (made != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no renderer: %s; --renderer=null runs "
                                 "without one\n",
                                 error.message);
        return EXIT_FAILURE;
    }
    run->box.device = tb_renderer_device(run->box.renderer);
    run->box.cpu_skinning = sandbox_cpu_skinning;
    tb_renderer_force_cpu_skinning(run->box.renderer, sandbox_cpu_skinning);
    sandbox_open_fonts(&run->box);
    tb_rhi_device_info_of(run->box.device, &run->box.info);
    sandbox_open_compiler(run);
    /* Keys 1 to 9 choose the first nine scenes; Tab reaches the rest. */
    const size_t keyed = sandbox_scene_count < 9u ? sandbox_scene_count : 9u;

    (void)tb_fmt_print("badger-sandbox: %s on %s; keys 1 to %zu choose a scene, Tab the next; "
                       "hold the right mouse button to fly (WASD, Q/E, Shift, wheel); Escape "
                       "quits\n",
                       tb_rhi_backend_name(run->box.info.backend), run->box.info.adapter, keyed);
    return EXIT_SUCCESS;
}

static void sandbox_close(sandbox_run *run, tb_window_system *system)
{
    sandbox_leave_scene(run);
    if (run->inspector != NULL)
    {
        (void)tb_rhi_wait_idle(run->box.device);
        sandbox_inspector_destroy(run->inspector, &run->box);
    }
    tb_renderer_destroy(run->box.renderer);
    tb_shader_cache_close(run->shader_cache);
    for (int font = 0; font < SANDBOX_FONT_COUNT; font++)
    {
        tb_font_close(run->box.fonts[font]);
    }
    tb_font_catalog_destroy(run->box.catalog);
    tb_dxc_close(run->box.dxc);
    tb_d3dcompiler_close(run->box.d3dcompiler);
    tb_window_destroy(run->box.window);
    tb_window_system_close(system);
    tb_settings_destroy(run->settings);
}

static int sandbox_main(const tb_cli_outcome *outcome, void *context)
{
    sandbox_run run = {0};
    tb_window_system *system = NULL;
    tb_window_error error = TB_WINDOW_ERROR_INIT;
    const size_t scene = sandbox_find_scene(sandbox_scene_name);
    int status = EXIT_SUCCESS;

    (void)outcome;
    (void)context;
    if (scene == sandbox_scene_count)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no scene is named %s; `badger-sandbox "
                                 "list` names them\n",
                                 sandbox_scene_name);
        return TB_CLI_EXIT_USAGE;
    }
    status = sandbox_load_settings(&run);
    if (status == EXIT_SUCCESS && tb_window_system_open(NULL, &system, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no windowing: %s\n", error.message);
        status = EXIT_FAILURE;
    }
    if (status == EXIT_SUCCESS)
    {
        status = sandbox_open_renderer(&run, system);
    }
    if (status == EXIT_SUCCESS)
    {
        run.box.ui_scale = (float)sandbox_ui_scale / 100.0f;
        run.box.engine = &run.engine;
        run.speed = SANDBOX_MOVE_SPEED;
        run.box.camera = tb_camera_perspective(
            tb_radians((float)tb_setting_number(run.engine.graphics.field_of_view)));
        run.box.camera.position = (tb_vec3){0.0f, 2.2f, 5.5f};
        run.pitch = -0.3f;
        run.loop = tb_game_loop_create(&(tb_game_loop_options){
            .system = system,
            .window = run.box.window,
            .settings = &run.engine,
            .callbacks = {
                .frame = sandbox_frame_callback, .event = sandbox_event, .context = &run}});
        run.vsync_version = tb_setting_version(run.engine.display.vsync);
        run.inspector =
            sandbox_inspector_create(&run.box, run.settings, &run.engine, sandbox_show_inspector);
        sandbox_enter_scene(&run, scene);
        tb_game_loop_run(run.loop);
        tb_game_loop_destroy(run.loop);
        status = run.status;
    }
    sandbox_close(&run, system);
    return status;
}

static int sandbox_list(const tb_cli_outcome *outcome, void *context)
{
    (void)outcome;
    (void)context;
    for (size_t index = 0; index < sandbox_scene_count; index++)
    {
        (void)tb_fmt_print("%zu  %-10s %s\n", index + 1, sandbox_scenes[index].name,
                           sandbox_scenes[index].summary);
    }
    return tb_fmt_flush() ? EXIT_SUCCESS : EXIT_FAILURE;
}

static const tb_cli_option sandbox_options[] = {
    {.long_name = "renderer",
     .name = "NAME",
     .text = &sandbox_renderer_name,
     .help = "The backend to draw with, as graphics.renderer names it: auto, d3d12, null..."},
    {.long_name = "scene",
     .name = "NAME",
     .text = &sandbox_scene_name,
     .help = "The scene to start with; `badger-sandbox list` names them. The default is shapes."},
    {.long_name = "frames",
     .name = "COUNT",
     .integer = &sandbox_frames,
     .minimum = 1,
     .maximum = INT64_MAX,
     .help = "Stop after this many frames."},
    {.long_name = "capture",
     .name = "FILE",
     .text = &sandbox_capture_path,
     .help = "With --frames, save the last frame as this .bmp file."},
    {.long_name = "ui-scale",
     .name = "PERCENT",
     .integer = &sandbox_ui_scale,
     .minimum = 50,
     .maximum = 400,
     .help = "Draw the interface at this share of the display's scale: 150 is half as large "
             "again. The default is 100."},
    {.long_name = "inspector",
     .flag = &sandbox_show_inspector,
     .help = "Start with the inspector shown: every setting, and the frame's statistics. F1 or "
             "the backquote shows and hides it."},
    {.long_name = "software",
     .flag = &sandbox_software,
     .help = "Draw on the system's GPU on the CPU (WARP on Windows), which draws the same "
             "pixels on every machine."},
    {.long_name = "cpu-skinning",
     .flag = &sandbox_cpu_skinning,
     .help = "Skin meshes on the CPU, as a device that cannot skin on the GPU does; the "
             "inspector switches between the two."},
    {.long_name = "validation",
     .flag = &sandbox_validation,
     .help = "Turn on the graphics API's validation, such as the D3D12 debug layer."},
    {.long_name = "set",
     .name = "NAME=VALUE",
     .texts = sandbox_values,
     .capacity = SANDBOX_MOST_VALUES,
     .count = &sandbox_value_count,
     .help = "Give a setting a value, such as display.vsync=off; may be given more than once."},
    {.long_name = "log-file",
     .name = "FILE",
     .text = &sandbox_log_file,
     .help = "Write the log and all other output to this file, made or emptied, in place of "
             "standard output and standard error, which nothing reads under Proton."},
};

static const tb_cli_command sandbox_commands[] = {
    {.name = "list", .summary = "List the scenes.", .run = sandbox_list},
};

static const tb_cli_command sandbox_program = {
    .name = "badger-sandbox",
    .version = TB_VERSION_STRING,
    .description = "Draw the engine's test scenes in a window, to see by eye what the tests "
                   "check by numbers.",
    .options = sandbox_options,
    .option_count = TB_CLI_COUNT_OF(sandbox_options),
    .commands = sandbox_commands,
    .command_count = TB_CLI_COUNT_OF(sandbox_commands),
    .command_optional = true,
    .run = sandbox_main,
};

/* Read the command line, send the output where --log-file says before
 * anything is written, then run the command chosen. */
static int sandbox_parse_and_run(const tb_arguments *arguments)
{
    tb_cli_outcome outcome = TB_CLI_OUTCOME_INIT;

    if (tb_cli_parse(&sandbox_program, arguments->count, arguments->values, &outcome) != TB_CLI_OK)
    {
        return tb_cli_finish(&outcome);
    }
    if (sandbox_log_file != NULL && !tb_process_write_output_to(sandbox_log_file))
    {
        (void)tb_fmt_print_error("badger-sandbox: error: --log-file %s: the file could not be "
                                 "made\n",
                                 sandbox_log_file);
        return EXIT_FAILURE;
    }
    TB_ASSERT(outcome.command != NULL && outcome.command->run != NULL);
    return outcome.command->run(&outcome, NULL);
}

int main(int count, char **values)
{
    tb_arguments arguments = TB_ARGUMENTS_INIT;
    int status = EXIT_FAILURE;

    tb_crash_catch();
    if (!tb_process_get_arguments(count, values, &arguments))
    {
        (void)tb_fmt_print_error("badger-sandbox: the command line could not be read\n");
        return EXIT_FAILURE;
    }
    status = sandbox_parse_and_run(&arguments);
    tb_process_release_arguments(&arguments);
    return status;
}
