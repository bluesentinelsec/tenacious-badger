/* Burrow's game: a window as the settings describe it, a renderer on it,
 * and the game loop, which flies the player through the graybox room at its
 * fixed ticks and draws the room and the HUD between them. Without a window
 * the same loop runs the player, and draws nothing. */
#include "burrow_game.h"

#include "burrow_hud.h"
#include "burrow_player.h"
#include "burrow_room.h"
#include "burrow_scene.h"

#include "badger/file.h"
#include "badger/fmt.h"
#include "badger/game_loop.h"
#include "badger/material.h"
#include "badger/path.h"
#include "badger/renderer.h"
#include "badger/settings.h"
#include "badger/shader_cache.h"
#include "badger/str.h"
#include "badger/vfs.h"
#include "badger/window.h"

#include <stdlib.h>

#define BURROW_TITLE "Burrow"

/* Bytes enough for the directories Burrow finds its files in. */
#define BURROW_PATH_CAPACITY 4096

/* Where a macOS bundle keeps its resources, from its executable's
 * directory, Contents/MacOS. */
#define BURROW_BUNDLE_RESOURCES "../Resources"

/* What is behind the sky, which covers it: seen only where the device draws
 * no sky. */
static const tb_vec4 burrow_clear = {0.35f, 0.42f, 0.52f, 1.0f};

/* What runs while Burrow plays. */
typedef struct burrow_game
{
    const tb_engine_settings *engine;
    const burrow_room *room;
    tb_window *window;             /* NULL without a window. */
    tb_renderer *renderer;         /* NULL without a window. */
    tb_shader_cache *shader_cache; /* The user's; NULL for none. */
    burrow_scene *scene;
    burrow_hud *hud;
    tb_game_loop *loop;
    burrow_player player;
    tb_camera camera;
    uint64_t vsync_version; /* Of display.vsync, when the renderer last took it. */
    int status;
} burrow_game;

static void burrow_tick(void *context, const tb_game_time *time)
{
    burrow_game *const game = context;

    burrow_player_tick(&game->player, time->tick_seconds);
}

/* Capture the mouse for looking, or let it go. Where the system cannot
 * capture it, the view does not turn, and the HUD keeps asking for a
 * click. */
static void burrow_capture_mouse(burrow_game *game, bool captured)
{
    const tb_window_result result = tb_window_set_relative_mouse(game->window, captured, NULL);

    game->player.captured = captured && result == TB_WINDOW_OK;
}

static bool burrow_event(void *context, const tb_window_event *event)
{
    burrow_game *const game = context;
    const burrow_look_options look = {
        .sensitivity = (float)tb_setting_number(game->engine->controls.mouse_sensitivity),
        .invert_y = tb_setting_boolean(game->engine->controls.mouse_invert_y)};
    burrow_mouse_request request = BURROW_MOUSE_KEEP;
    const bool taken = burrow_player_event(&game->player, event, &look, &request);

    if (request != BURROW_MOUSE_KEEP)
    {
        burrow_capture_mouse(game, request == BURROW_MOUSE_CAPTURE);
    }
    return taken;
}

/* Settings the renderer and the camera take as they change; the game loop
 * applies the window's. */
static void burrow_apply_settings(burrow_game *game)
{
    const uint64_t vsync = tb_setting_version(game->engine->display.vsync);
    const float fov = (float)tb_setting_number(game->engine->graphics.field_of_view);

    if (vsync != game->vsync_version)
    {
        tb_renderer_set_vsync(game->renderer, tb_setting_choice(game->engine->display.vsync) != 0);
        game->vsync_version = vsync;
    }
    tb_renderer_set_anisotropy(game->renderer, tb_engine_settings_anisotropy(game->engine));
    game->camera.vertical_fov = burrow_vertical_fov(fov);
}

static void burrow_frame(void *context, const tb_game_time *time)
{
    burrow_game *const game = context;

    if (game->renderer == NULL)
    {
        return;
    }
    burrow_hud_count_frame(game->hud, time->frame_seconds);
    burrow_apply_settings(game);
    game->camera.position = burrow_player_eye(&game->player, time->alpha);
    game->camera.orientation = burrow_view_orientation(game->player.yaw, game->player.pitch);
    if (!tb_renderer_begin_frame(game->renderer, &game->camera, burrow_clear))
    {
        return; /* the window is minimized */
    }
    burrow_scene_draw(game->scene, game->renderer);
    burrow_hud_draw(game->hud, game->renderer, game->engine, game->player.captured);
    /* coverage-off: only a GPU that is reset or removed loses the device */
    if (tb_renderer_end_frame(game->renderer) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("burrow: error: the graphics device was lost\n");
        game->status = EXIT_FAILURE;
        tb_game_loop_quit(game->loop);
    }
    /* coverage-on */
}

static void burrow_run_loop(burrow_game *game, tb_window_system *system)
{
    const tb_game_loop_callbacks callbacks = {
        .tick = burrow_tick, .frame = burrow_frame, .event = burrow_event, .context = game};

    game->loop = tb_game_loop_create(&(tb_game_loop_options){.system = system,
                                                             .window = game->window,
                                                             .settings = game->engine,
                                                             .callbacks = callbacks});
    tb_game_loop_run(game->loop);
    tb_game_loop_destroy(game->loop);
    game->loop = NULL;
}

/* Mount a directory, relative to the executable's, where Burrow's files may
 * be: one that is missing is passed over. */
static void burrow_mount_beside(tb_vfs *vfs, const char *executable, const char *relative,
                                int priority)
{
    const tb_path_style style = tb_fs_native_style();
    char joined[BURROW_PATH_CAPACITY] = {0};
    char normal[BURROW_PATH_CAPACITY] = {0};
    size_t required = 0;

    if (tb_path_join(style, (tb_path_view){executable, tb_str_length(executable)},
                     (tb_path_view){relative, tb_str_length(relative)}, joined, sizeof(joined),
                     &required) != TB_PATH_OK ||
        tb_path_normalize(style, (tb_path_view){joined, tb_str_length(joined)}, normal,
                          sizeof(normal), &required) != TB_PATH_OK)
    {
        return;
    }
    (void)tb_vfs_mount_directory(vfs, "", 0, normal, priority);
}

/* Where the renderer finds the shader libraries the build writes,
 * Shaders/<backend>.tbsl: beside the executable on Windows and Linux, and
 * in the bundle's Resources on macOS, mounted over it. In a VFS for the
 * caller to destroy. While shaders may be compiled at run time, the user's
 * shader cache keeps them between runs. */
static tb_vfs *burrow_shader_sources(burrow_game *game)
{
    char executable[BURROW_PATH_CAPACITY] = {0};
    char cache[BURROW_PATH_CAPACITY] = {0};
    size_t required = 0;
    tb_vfs *const vfs = tb_vfs_create(NULL);

    if (tb_fs_get_location(TB_FS_EXECUTABLE, executable, sizeof(executable), &required) == TB_FS_OK)
    {
        burrow_mount_beside(vfs, executable, ".", 0);
        burrow_mount_beside(vfs, executable, BURROW_BUNDLE_RESOURCES, 1);
    }
    if (tb_setting_boolean(game->engine->shaders.runtime_compile) &&
        tb_shader_cache_default_directory(cache, sizeof(cache)))
    {
        (void)tb_shader_cache_open(cache, &game->shader_cache);
    }
    return vfs;
}

/* Make the renderer, and what it draws: the room and the HUD. */
static int burrow_open_renderer(burrow_game *game)
{
    tb_renderer_options options = {0};
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_rhi_device_info info = {0};
    tb_vfs *const shaders = burrow_shader_sources(game);
    tb_rhi_result made = TB_RHI_OK;

    options.window = game->window;
    options.vsync = tb_setting_choice(game->engine->display.vsync) != 0;
    options.anisotropy = tb_engine_settings_anisotropy(game->engine);
    options.shader_vfs = shaders;
    options.shader_cache = game->shader_cache;
    options.shader_library_only = !tb_setting_boolean(game->engine->shaders.runtime_compile);
    tb_engine_settings_device_options(game->engine, &options.device);
    made = tb_renderer_create(&options, &game->renderer, &error);
    tb_vfs_destroy(shaders);
    if (made != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("burrow: error: no renderer: %s; --renderer=null runs without "
                                 "drawing\n",
                                 error.message);
        return EXIT_FAILURE;
    }
    tb_rhi_device_info_of(tb_renderer_device(game->renderer), &info);
    (void)tb_fmt_print("burrow: drawing with %s on %s\n", tb_rhi_backend_name(info.backend),
                       info.adapter);
    game->vsync_version = tb_setting_version(game->engine->display.vsync);
    game->camera = tb_camera_perspective(
        burrow_vertical_fov((float)tb_setting_number(game->engine->graphics.field_of_view)));
    game->scene = burrow_scene_create(game->renderer, game->room);
    game->hud = burrow_hud_create();
    return EXIT_SUCCESS;
}

static void burrow_close_renderer(burrow_game *game)
{
    if (game->renderer == NULL)
    {
        return;
    }
    burrow_hud_destroy(game->hud, game->renderer);
    burrow_scene_destroy(game->scene, game->renderer);
    tb_renderer_destroy(game->renderer);
    game->renderer = NULL;
}

static int burrow_play_windowed(burrow_game *game, const burrow_settings *loaded)
{
    tb_window_system *system = NULL;
    tb_window_options options = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (tb_window_system_open(NULL, &system, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("burrow: error: no window: %s; --headless runs without one\n",
                                 error.message);
        return EXIT_FAILURE;
    }
    tb_engine_settings_window_options(&loaded->engine, BURROW_TITLE, &options);
    /* coverage-off: only a system out of resources refuses a window */
    if (tb_window_create(system, &options, &game->window, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("burrow: error: the window could not be made: %s\n",
                                 error.message);
        tb_window_system_close(system);
        return EXIT_FAILURE;
    }
    /* coverage-on */
    game->status = burrow_open_renderer(game);
    if (game->status == EXIT_SUCCESS)
    {
        /* A shooter starts looking: the first click is not needed. */
        burrow_capture_mouse(game, true);
        burrow_run_loop(game, system);
        burrow_capture_mouse(game, false);
    }
    burrow_close_renderer(game);
    tb_shader_cache_close(game->shader_cache);
    tb_window_destroy(game->window);
    tb_window_system_close(system);
    return game->status;
}

int burrow_play(const burrow_settings *loaded, bool headless)
{
    burrow_game game = {0};

    game.engine = &loaded->engine;
    game.room = burrow_room_graybox();
    game.player = burrow_player_spawn(game.room->spawn, game.room->spawn_yaw);
    game.status = EXIT_SUCCESS;
    if (headless)
    {
        burrow_run_loop(&game, NULL);
        return game.status;
    }
    return burrow_play_windowed(&game, loaded);
}
