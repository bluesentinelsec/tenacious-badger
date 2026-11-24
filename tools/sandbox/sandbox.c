/* badger-sandbox: opens a window with the engine's settings and game loop,
 * and draws one of its scenes through the RHI. Keys 1 to 9 choose a scene,
 * and Tab the next. With --frames it stops after that many frames, and with
 * --capture it saves the last as a .bmp first, so a scene can become a test. */
#include "sandbox.h"

#include "badger/arena.h"
#include "badger/cli.h"
#include "badger/crash.h"
#include "badger/dxc.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/process.h"
#include "badger/settings.h"
#include "badger/str.h"
#include "badger/version.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SANDBOX_TITLE "badger-sandbox"
#define SANDBOX_MOST_VALUES 32
/* Seconds between the frame times the sandbox prints. */
#define SANDBOX_REPORT_SECONDS 2.0

/* The command line. */
static const char *const sandbox_renderers[] = {"auto", "d3d12", "null"};
static const tb_rhi_backend sandbox_backends[] = {TB_RHI_BACKEND_AUTOMATIC, TB_RHI_BACKEND_D3D12,
                                                  TB_RHI_BACKEND_NULL};
static size_t sandbox_renderer = 0;
static const char *sandbox_scene_name = "cube";
static int64_t sandbox_frames = 0;
static const char *sandbox_capture_path = NULL;
static bool sandbox_software = false;
static bool sandbox_validation = false;
static const char *sandbox_values[SANDBOX_MOST_VALUES] = {0};
static size_t sandbox_value_count = 0;

/* What runs while the window is open. */
typedef struct sandbox_run
{
    sandbox box;
    tb_settings *settings;
    tb_engine_settings engine;
    tb_game_loop *loop;
    size_t scene;
    void *state;
    bool scene_ready;
    double scene_started;
    double clock;
    /* Frame times since the last report. */
    double reported;
    uint64_t frames;
    double slowest;
    tb_rhi_buffer capture;
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

    request.source.path = path;
    request.source.files = &file;
    request.source.file_count = 1;
    request.entry_point = entry;
    request.stage = sandbox_stage(stage);
    request.target = box->dxc != NULL ? TB_SHADER_TARGET_DXIL : TB_SHADER_TARGET_NONE;
    request.dxc = box->dxc;
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

    (void)box;
    desc.colors[0] = TB_RHI_FORMAT_B8G8R8A8_UNORM;
    desc.color_count = 1;
    desc.depth = TB_RHI_FORMAT_D32_FLOAT;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_compare = TB_RHI_COMPARE_GREATER; /* reversed depth */
    return desc;
}

/* ---------------------------------------------------------------------------
 * The swap chain and its depth.
 * ------------------------------------------------------------------------- */

static bool sandbox_make_depth(sandbox *box)
{
    tb_rhi_texture_desc desc = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    tb_rhi_texture_destroy(box->device, box->depth);
    box->depth = (tb_rhi_texture){0};
    tb_rhi_texture_desc_of(box->device, tb_rhi_swap_chain_current(box->chain), &desc);
    box->width = desc.width;
    box->height = desc.height;
    if (tb_rhi_texture_create(box->device,
                              &(tb_rhi_texture_desc){.format = TB_RHI_FORMAT_D32_FLOAT,
                                                     .width = desc.width,
                                                     .height = desc.height,
                                                     .usage = TB_RHI_TEXTURE_DEPTH,
                                                     .name = "depth"},
                              &box->depth, &error) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: no depth texture: %s\n", error.message);
        return false;
    }
    return true;
}

static void sandbox_resize(sandbox_run *run, const tb_window_state *state)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;

    if (state->pixel_width == 0 || state->pixel_height == 0 ||
        (state->pixel_width == run->box.width && state->pixel_height == run->box.height))
    {
        return;
    }
    if (tb_rhi_swap_chain_resize(run->box.chain, state->pixel_width, state->pixel_height,
                                 run->box.vsync, &error) != TB_RHI_OK ||
        !sandbox_make_depth(&run->box))
    {
        (void)tb_fmt_print_error("badger-sandbox: the swap chain could not resize: %s\n",
                                 error.message);
        run->status = EXIT_FAILURE;
        tb_game_loop_quit(run->loop);
    }
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

static bool sandbox_event(void *context, const tb_window_event *event)
{
    sandbox_run *const run = context;

    if (event->type == TB_WINDOW_EVENT_RESIZED)
    {
        sandbox_resize(run, &event->state);
        return false;
    }
    if (event->type != TB_WINDOW_EVENT_KEY_DOWN || event->repeat)
    {
        return false;
    }
    if (event->key >= TB_KEY_1 && event->key <= TB_KEY_9 &&
        (size_t)(event->key - TB_KEY_1) < sandbox_scene_count)
    {
        sandbox_enter_scene(run, (size_t)(event->key - TB_KEY_1));
        return true;
    }
    if (event->key == TB_KEY_TAB)
    {
        sandbox_enter_scene(run, (run->scene + 1) % sandbox_scene_count);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

static void sandbox_report(sandbox_run *run, double seconds)
{
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

/* The frame's clear: a slow cycle of hue for the clear scene, dark grey for
 * the others. */
static void sandbox_clear_color(const sandbox_run *run, float color[4])
{
    const double t = run->clock - run->scene_started;

    color[3] = 1.0f;
    if (run->scene != 0)
    {
        color[0] = 0.08f;
        color[1] = 0.09f;
        color[2] = 0.11f;
        return;
    }
    color[0] = (float)(0.5 + 0.5 * sin(t));
    color[1] = (float)(0.5 + 0.5 * sin(t + 2.0943951));
    color[2] = (float)(0.5 + 0.5 * sin(t + 4.1887902));
}

/* Whether this frame is the last of --frames, which --capture saves. */
static bool sandbox_capturing(const tb_game_time *time)
{
    return sandbox_capture_path != NULL && sandbox_frames > 0 &&
           time->frame + 1 == (uint64_t)sandbox_frames;
}

static void sandbox_copy_out(sandbox_run *run, tb_rhi_command_list *list, tb_rhi_texture image)
{
    const uint32_t pitch = (run->box.width * 4u + 255u) & ~255u;

    if (tb_rhi_buffer_create(run->box.device,
                             &(tb_rhi_buffer_desc){.size = (uint64_t)pitch * run->box.height,
                                                   .memory = TB_RHI_MEMORY_READBACK},
                             &run->capture, NULL) != TB_RHI_OK)
    {
        return;
    }
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image,
                                               .before = TB_RHI_STATE_PRESENT,
                                               .after = TB_RHI_STATE_COPY_SOURCE});
    tb_rhi_cmd_copy_texture_to_buffer(
        list, &(tb_rhi_texture_copy){.buffer = run->capture, .row_pitch = pitch, .texture = image});
    tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = image,
                                               .before = TB_RHI_STATE_COPY_SOURCE,
                                               .after = TB_RHI_STATE_PRESENT});
}

static void sandbox_save_capture(sandbox_run *run, uint64_t submission)
{
    const uint32_t pitch = (run->box.width * 4u + 255u) & ~255u;
    const size_t size = (size_t)pitch * run->box.height;
    uint8_t *pixels = NULL;

    if (run->capture.generation == 0 || tb_rhi_wait(run->box.device, submission) != TB_RHI_OK)
    {
        run->status = EXIT_FAILURE;
        return;
    }
    pixels = tb_malloc(size);
    tb_rhi_buffer_read(run->box.device, run->capture, 0, pixels, size);
    if (sandbox_write_bmp(sandbox_capture_path, pixels, run->box.width, run->box.height, pitch))
    {
        (void)tb_fmt_print("badger-sandbox: saved %s\n", sandbox_capture_path);
    }
    else
    {
        (void)tb_fmt_print_error("badger-sandbox: %s could not be written\n", sandbox_capture_path);
        run->status = EXIT_FAILURE;
    }
    tb_free(pixels);
    tb_rhi_buffer_destroy(run->box.device, run->capture);
    run->capture = (tb_rhi_buffer){0};
}

static void sandbox_frame_callback(void *context, const tb_game_time *time)
{
    sandbox_run *const run = context;
    tb_rhi_command_list *list = NULL;
    tb_rhi_texture image = {0};
    tb_rhi_pass pass = {.color_count = 1};
    sandbox_frame frame = {0};
    uint64_t submission = 0;

    run->clock += time->frame_seconds;
    sandbox_report(run, time->frame_seconds);
    if (!time->visible || run->box.width == 0)
    {
        return;
    }
    list = tb_rhi_command_list_begin(run->box.device);
    image = tb_rhi_swap_chain_current(run->box.chain);
    if (run->scene_ready && sandbox_scenes[run->scene].prepare != NULL)
    {
        sandbox_scenes[run->scene].prepare(&run->box, run->state, list);
    }
    tb_rhi_cmd_barriers(
        list,
        (const tb_rhi_barrier[]){
            {.texture = image, .before = TB_RHI_STATE_PRESENT, .after = TB_RHI_STATE_RENDER_TARGET},
            {.texture = run->box.depth,
             .before = TB_RHI_STATE_COMMON,
             .after = TB_RHI_STATE_DEPTH_WRITE}},
        2);
    pass.colors[0] = (tb_rhi_color_target){.texture = image, .load = TB_RHI_LOAD_CLEAR};
    sandbox_clear_color(run, pass.colors[0].clear);
    pass.depth = (tb_rhi_depth_target){.texture = run->box.depth, .load = TB_RHI_LOAD_CLEAR};
    tb_rhi_cmd_begin_pass(list, &pass);
    if (run->scene_ready)
    {
        frame.list = list;
        frame.seconds = run->clock - run->scene_started;
        frame.aspect = (float)run->box.width / (float)run->box.height;
        sandbox_scenes[run->scene].draw(&run->box, run->state, &frame);
    }
    tb_rhi_cmd_end_pass(list);
    tb_rhi_cmd_barriers(
        list,
        (const tb_rhi_barrier[]){
            {.texture = image, .before = TB_RHI_STATE_RENDER_TARGET, .after = TB_RHI_STATE_PRESENT},
            {.texture = run->box.depth,
             .before = TB_RHI_STATE_DEPTH_WRITE,
             .after = TB_RHI_STATE_COMMON}},
        2);
    if (sandbox_capturing(time))
    {
        sandbox_copy_out(run, list, image);
    }
    submission = tb_rhi_submit(run->box.device, &list, 1);
    if (sandbox_capturing(time))
    {
        sandbox_save_capture(run, submission);
    }
    if (tb_rhi_swap_chain_present(run->box.chain) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: the device was lost\n");
        run->status = EXIT_FAILURE;
        tb_game_loop_quit(run->loop);
    }
}

/* ---------------------------------------------------------------------------
 * Starting and stopping.
 * ------------------------------------------------------------------------- */

static int sandbox_load_settings(sandbox_run *run)
{
    run->settings = tb_settings_create();
    tb_engine_settings_add(run->settings, &(tb_engine_settings_options){.developer = true},
                           &run->engine);
    for (size_t index = 0; index < sandbox_value_count; index++)
    {
        const char *const assignment = sandbox_values[index];
        const size_t equals = tb_str_find_char(assignment, 0, TB_STR_END, '=');
        char *name = NULL;
        tb_setting_result result = TB_SETTING_OK;
        if (equals == TB_STR_NOT_FOUND)
        {
            (void)tb_fmt_print_error("badger-sandbox: error: --set %s: expected NAME=VALUE\n",
                                     assignment);
            return TB_CLI_EXIT_USAGE;
        }
        name = tb_str_sub(NULL, assignment, 0, equals);
        result = tb_settings_set(run->settings, TB_SETTING_LAYER_COMMAND_LINE, name,
                                 assignment + equals + 1);
        tb_str_free(NULL, name);
        if (result != TB_SETTING_OK)
        {
            (void)tb_fmt_print_error("badger-sandbox: error: --set %s: %s\n", assignment,
                                     tb_setting_result_name(result));
            return TB_CLI_EXIT_USAGE;
        }
    }
    if (sandbox_frames > 0)
    {
        (void)tb_setting_set_integer(run->engine.developer.frame_count, sandbox_frames);
    }
    return EXIT_SUCCESS;
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

static int sandbox_open_renderer(sandbox_run *run, tb_window_system *system)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_window_options options = {0};
    tb_window_error window_error = TB_WINDOW_ERROR_INIT;
    tb_dxc_error dxc_error = TB_DXC_ERROR_INIT;
    const tb_rhi_device_options device = {
        .backend = sandbox_backends[sandbox_renderer],
        .adapter = sandbox_software ? TB_RHI_ADAPTER_SOFTWARE : TB_RHI_ADAPTER_HIGH_PERFORMANCE,
        .validation = sandbox_validation};

    tb_engine_settings_window_options(&run->engine, SANDBOX_TITLE, &options);
    if (tb_window_create(system, &options, &run->box.window, &window_error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no window: %s\n", window_error.message);
        return EXIT_FAILURE;
    }
    if (tb_rhi_device_create(&device, &run->box.device, &error) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no renderer: %s; --renderer=null runs "
                                 "without one\n",
                                 error.message);
        return EXIT_FAILURE;
    }
    tb_rhi_device_info_of(run->box.device, &run->box.info);
    if (run->box.info.backend == TB_RHI_BACKEND_D3D12 &&
        tb_dxc_open(&run->box.dxc, &dxc_error) != TB_DXC_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no shader compiler: %s\n",
                                 dxc_error.message);
        return EXIT_FAILURE;
    }
    run->box.vsync = tb_setting_choice(run->engine.display.vsync) != 0;
    if (tb_rhi_swap_chain_create(run->box.device, run->box.window,
                                 &(tb_rhi_swap_chain_desc){.vsync = run->box.vsync},
                                 &run->box.chain, &error) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("badger-sandbox: error: no swap chain: %s\n", error.message);
        return EXIT_FAILURE;
    }
    if (!sandbox_make_depth(&run->box))
    {
        return EXIT_FAILURE;
    }
    (void)tb_fmt_print("badger-sandbox: %s on %s; keys 1 to %zu choose a scene, Tab the next, "
                       "Escape quits\n",
                       tb_rhi_backend_name(run->box.info.backend), run->box.info.adapter,
                       sandbox_scene_count);
    return EXIT_SUCCESS;
}

static void sandbox_close(sandbox_run *run, tb_window_system *system)
{
    sandbox_leave_scene(run);
    if (run->box.device != NULL)
    {
        (void)tb_rhi_wait_idle(run->box.device);
        tb_rhi_texture_destroy(run->box.device, run->box.depth);
    }
    tb_rhi_swap_chain_destroy(run->box.chain);
    tb_rhi_device_destroy(run->box.device);
    tb_dxc_close(run->box.dxc);
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
        (void)tb_fmt_print_error("badger-sandbox: error: no scene is named %s; --list names "
                                 "them\n",
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
        run.loop = tb_game_loop_create(&(tb_game_loop_options){
            .system = system,
            .window = run.box.window,
            .settings = &run.engine,
            .callbacks = {
                .frame = sandbox_frame_callback, .event = sandbox_event, .context = &run}});
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
     .choice = &sandbox_renderer,
     .choices = sandbox_renderers,
     .choice_count = TB_CLI_COUNT_OF(sandbox_renderers),
     .help = "The backend to draw with; auto takes the platform's best."},
    {.long_name = "scene",
     .name = "NAME",
     .text = &sandbox_scene_name,
     .help = "The scene to start with; `badger-sandbox list` names them. The default is cube."},
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
    {.long_name = "software",
     .flag = &sandbox_software,
     .help = "Draw on the system's GPU on the CPU (WARP on Windows), which draws the same "
             "pixels on every machine."},
    {.long_name = "validation",
     .flag = &sandbox_validation,
     .help = "Turn on the graphics API's validation, such as the D3D12 debug layer."},
    {.long_name = "set",
     .name = "NAME=VALUE",
     .texts = sandbox_values,
     .capacity = SANDBOX_MOST_VALUES,
     .count = &sandbox_value_count,
     .help = "Give a setting a value, such as display.vsync=off; may be given more than once."},
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
    status = tb_cli_run(&sandbox_program, arguments.count, arguments.values, NULL);
    tb_process_release_arguments(&arguments);
    return status;
}
