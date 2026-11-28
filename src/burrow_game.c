/* Burrow's game: a window as the settings describe it, a renderer on it,
 * the menus, and the game loop, which flies the player through the graybox
 * room at its fixed ticks and draws the room, the HUD, and the menus
 * between them. Without a window the same loop runs the player, and draws
 * nothing. */
#include "burrow_game.h"

#include "burrow_hud.h"
#include "burrow_menus.h"
#include "burrow_player.h"
#include "burrow_room.h"
#include "burrow_scene.h"

#include "badger/canvas.h"
#include "badger/cli.h"
#include "badger/file.h"
#include "badger/fmt.h"
#include "badger/game_loop.h"
#include "badger/hash.h"
#include "badger/material.h"
#include "badger/memory.h"
#include "badger/path.h"
#include "badger/renderer.h"
#include "badger/settings.h"
#include "badger/shader_cache.h"
#include "badger/str.h"
#include "badger/vfs.h"
#include "badger/window.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define BURROW_TITLE "Burrow"

/* Bytes enough for the directories Burrow finds its files in. */
#define BURROW_PATH_CAPACITY 4096

/* Where a macOS bundle keeps its resources, from its executable's
 * directory, Contents/MacOS. */
#define BURROW_BUNDLE_RESOURCES "../Resources"

/* Bytes of a word of a script of navigation. */
#define BURROW_WORD_CAPACITY 32

/* The most displays the options name. */
#define BURROW_MOST_DISPLAYS 16

/* The canvas layer of the shade over the room while a menu shows: under
 * the menus' (TB_GUI_DEFAULT_LAYER). */
#define BURROW_SHADE_LAYER 40

/* How much the shade darkens the room behind the main menu, which moves
 * behind it, against the room frozen behind the others. */
#define BURROW_MAIN_SHADE 0.35f

/* What is behind the sky, which covers it: seen only where the device draws
 * no sky. */
static const tb_vec4 burrow_clear = {0.35f, 0.42f, 0.52f, 1.0f};

/* The shade over the room while a menu shows. */
static const tb_vec4 burrow_shade = {0.02f, 0.02f, 0.03f, 0.55f};

/* What runs while Burrow plays. */
typedef struct burrow_game
{
    const burrow_settings *loaded;
    const tb_engine_settings *engine;
    const burrow_room *room;
    tb_window_system *system;      /* NULL without a window. */
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
    bool developer;
    bool recreate_on_pause;
    /* The menus, and what they are made with; NULL menus where they could
     * not be made, and the game plays without them. */
    burrow_menus *menus;
    tb_vfs *content;
    tb_font_catalog *catalog;
    tb_theme *theme;
    tb_display displays[BURROW_MOST_DISPLAYS];
    size_t display_count;
    burrow_screen screen; /* The screen the game last acted on. */
    double orbit_seconds; /* The main menu's camera, since it last showed. */
    /* A test's navigation, one action a frame. */
    tb_gui_nav navigation[BURROW_MOST_NAVIGATION];
    size_t navigation_count;
    size_t navigation_next;
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

/* An event while the menus are there: a menu shown takes it; while
 * playing, Escape and the window losing the focus pause. */
static bool burrow_menus_take(burrow_game *game, const tb_window_event *event, bool *taken)
{
    const tb_window_event_type type = event->type;

    if (type == TB_WINDOW_EVENT_FOCUS_LOST)
    {
        burrow_player_let_go(&game->player);
    }
    if (burrow_menus_state(game->menus)->screen != BURROW_SCREEN_PLAYING)
    {
        *taken = burrow_menus_event(game->menus, event);
        return true;
    }
    /* The window losing the focus pauses as `gameplay.pause_when_unfocused`
     * says, as the game loop pauses the world. */
    const bool unfocused = type == TB_WINDOW_EVENT_FOCUS_LOST &&
                           tb_setting_boolean(game->engine->gameplay.pause_when_unfocused);
    if ((type == TB_WINDOW_EVENT_KEY_DOWN && event->key == TB_KEY_ESCAPE) || unfocused)
    {
        (void)burrow_menus_choose(game->menus, BURROW_CHOICE_PAUSE);
        *taken = type == TB_WINDOW_EVENT_KEY_DOWN;
        return true;
    }
    return false;
}

static bool burrow_event(void *context, const tb_window_event *event)
{
    burrow_game *const game = context;
    const burrow_look_options look = {
        .sensitivity = (float)tb_setting_number(game->engine->controls.mouse_sensitivity),
        .invert_y = tb_setting_boolean(game->engine->controls.mouse_invert_y)};
    burrow_mouse_request request = BURROW_MOUSE_KEEP;
    bool taken = false;

    if (game->menus != NULL && burrow_menus_take(game, event, &taken))
    {
        return taken;
    }
    taken = burrow_player_event(&game->player, event, &look, &request);
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
    /* The display policy, from Burrow's default file, and the player's
     * aspect ratio. */
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    tb_viewport_policy_from_settings(game->engine, &policy);
    tb_renderer_set_viewport_policy(game->renderer, &policy,
                                    tb_viewport_aspect_ratio_from_settings(game->engine));
}

/* ---------------------------------------------------------------------------
 * The menus' screens, as the game follows them.
 * ------------------------------------------------------------------------- */

/* Act on the screen the menus moved to: the mouse is the player's only
 * while playing, the world moves only then, and a game left for the main
 * menu starts afresh. */
static void burrow_follow_menus(burrow_game *game)
{
    const burrow_menu *const menu = burrow_menus_state(game->menus);

    if (menu->quitting)
    {
        tb_game_loop_quit(game->loop);
        return;
    }
    if (menu->screen == game->screen)
    {
        return;
    }
    game->screen = menu->screen;
    tb_game_loop_set_paused(game->loop, !burrow_menu_simulates(menu));
    burrow_player_let_go(&game->player);
    burrow_capture_mouse(game, menu->screen == BURROW_SCREEN_PLAYING);
    if (menu->screen == BURROW_SCREEN_MAIN && !menu->in_game)
    {
        game->player = burrow_player_spawn(game->room->spawn, game->room->spawn_yaw);
        game->orbit_seconds = 0.0;
    }
}

/* A test's next action of navigation, one a frame. */
static void burrow_navigate(burrow_game *game)
{
    if (game->navigation_next >= game->navigation_count)
    {
        return;
    }
    const tb_gui_nav action = game->navigation[game->navigation_next];
    game->navigation_next++;
    if (action != TB_GUI_NAV_NONE)
    {
        burrow_menus_navigate(game->menus, action);
    }
}

static void burrow_update_menus(burrow_game *game, const tb_game_time *time)
{
    if (game->menus == NULL)
    {
        return;
    }
    burrow_navigate(game);
    const tb_viewport viewport = tb_renderer_viewport(game->renderer);
    burrow_menus_update(game->menus, &viewport, time->frame_seconds);
    burrow_follow_menus(game);
    if (game->screen != BURROW_SCREEN_PLAYING)
    {
        tb_window_set_cursor(game->window, burrow_menus_cursor(game->menus));
    }
}

/* Where the camera is: orbiting the room behind the main menu, or the
 * player's eye. */
static void burrow_place_camera(burrow_game *game, const tb_game_time *time)
{
    float yaw = game->player.yaw;
    float pitch = game->player.pitch;

    game->camera.position = burrow_player_eye(&game->player, time->alpha);
    if (game->menus != NULL && game->screen == BURROW_SCREEN_MAIN)
    {
        game->orbit_seconds += time->frame_seconds;
        burrow_orbit_view(game->orbit_seconds, &game->camera.position, &yaw, &pitch);
    }
    game->camera.orientation = burrow_view_orientation(yaw, pitch);
}

/* A shade over the room, under the menu shown, so that its text reads. */
static void burrow_draw_shade(burrow_game *game)
{
    const tb_canvas_view view = tb_canvas_view_logical(game->renderer);
    uint32_t width = 0;
    uint32_t height = 0;
    tb_vec4 shade = burrow_shade;

    if (!burrow_menus_state(game->menus)->in_game)
    {
        shade.w = BURROW_MAIN_SHADE;
    }
    tb_renderer_size(game->renderer, &width, &height);
    tb_canvas_set_view(game->renderer, &view);
    tb_canvas_set_layer(game->renderer, BURROW_SHADE_LAYER);
    tb_canvas_fill_rect(
        game->renderer,
        (tb_canvas_rect){0.0f, 0.0f, (float)width / view.scale, (float)height / view.scale}, shade);
}

static bool burrow_draw_frame(burrow_game *game)
{
    if (!tb_renderer_begin_frame(game->renderer, &game->camera, burrow_clear))
    {
        return false; /* the window is minimized */
    }
    burrow_scene_draw(game->scene, game->renderer);
    if (game->menus == NULL || game->screen == BURROW_SCREEN_PLAYING)
    {
        burrow_hud_draw(game->hud, game->renderer, game->engine, game->player.captured,
                        game->menus != NULL);
    }
    else
    {
        burrow_draw_shade(game);
        burrow_menus_draw(game->menus, game->renderer);
    }
    /* coverage-off: only a GPU that is reset or removed loses the device */
    if (tb_renderer_end_frame(game->renderer) != TB_RHI_OK)
    {
        (void)tb_fmt_print_error("burrow: error: the graphics device was lost\n");
        game->status = EXIT_FAILURE;
        tb_game_loop_quit(game->loop);
    }
    /* coverage-on */
    return game->status == EXIT_SUCCESS;
}

static bool burrow_capture_hash(burrow_game *game, uint64_t *hash)
{
    uint32_t width = 0, height = 0;
    tb_renderer_capture_next_frame(game->renderer);
    if (!burrow_draw_frame(game))
    {
        return false;
    }
    const uint8_t *const pixels = tb_renderer_captured(game->renderer, &width, &height);
    if (pixels == NULL)
    {
        return false;
    }
    /* The renderer has already allocated and packed exactly these rows. */
    *hash = tb_hash_bytes(pixels, (size_t)width * height * 4u);
    return true;
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
    burrow_update_menus(game, time);
    burrow_place_camera(game, time);
    if (game->recreate_on_pause && game->screen == BURROW_SCREEN_PAUSE)
    {
        uint64_t before = 0, after = 0;
        tb_rhi_error error = TB_RHI_ERROR_INIT;
        /* Consecutive captures need identical projections and no temporal blending. */
        tb_renderer_quality saved = TB_RENDERER_QUALITY_INIT;
        tb_renderer_quality_of(game->renderer, &saved);
        tb_renderer_quality capture = saved;
        capture.temporal_aa = false;
        tb_renderer_set_quality(game->renderer, &capture);
        game->recreate_on_pause = false;
        if (burrow_capture_hash(game, &before) &&
            tb_rhi_device_recreate(tb_renderer_device(game->renderer), &error) == TB_RHI_OK &&
            burrow_capture_hash(game, &after))
        {
            (void)tb_fmt_print("burrow: recreated graphics device; paused frame %016" PRIx64
                               " -> %016" PRIx64 "\n",
                               before, after);
            if (before != after)
            {
                game->status = EXIT_FAILURE;
                tb_game_loop_quit(game->loop);
            }
        }
        else
        {
            (void)tb_fmt_print_error(
                "burrow: failed to capture or recreate the paused renderer: %s\n", error.message);
            game->status = EXIT_FAILURE;
            tb_game_loop_quit(game->loop);
        }
        tb_renderer_set_quality(game->renderer, &saved);
        return;
    }
    (void)burrow_draw_frame(game);
}

static void burrow_run_loop(burrow_game *game, tb_window_system *system)
{
    const tb_game_loop_callbacks callbacks = {
        .tick = burrow_tick, .frame = burrow_frame, .event = burrow_event, .context = game};

    game->loop = tb_game_loop_create(&(tb_game_loop_options){.system = system,
                                                             .window = game->window,
                                                             .settings = game->engine,
                                                             .callbacks = callbacks});
    if (game->menus != NULL)
    {
        /* The main menu: the world waits for Play. */
        tb_game_loop_set_paused(game->loop, true);
    }
    tb_game_loop_run(game->loop);
    tb_game_loop_destroy(game->loop);
    game->loop = NULL;
}

/* ---------------------------------------------------------------------------
 * Burrow's files: the shader libraries and the content.
 * ------------------------------------------------------------------------- */

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

/* Burrow's files, as the build lays them out: beside the executable on
 * Windows and Linux, and in the bundle's Resources on macOS, mounted over
 * it. A developer build reads its content from the source tree first,
 * where an author edits it while the game runs. In a VFS for the caller to
 * destroy. */
static tb_vfs *burrow_files(bool developer)
{
    char executable[BURROW_PATH_CAPACITY] = {0};
    size_t required = 0;
    tb_vfs *const vfs = tb_vfs_create(NULL);

    if (tb_fs_get_location(TB_FS_EXECUTABLE, executable, sizeof(executable), &required) == TB_FS_OK)
    {
        burrow_mount_beside(vfs, executable, ".", 0);
        burrow_mount_beside(vfs, executable, BURROW_BUNDLE_RESOURCES, 1);
    }
#ifdef BURROW_SOURCE_CONTENT
    if (developer)
    {
        (void)tb_vfs_mount_directory(vfs, "", 0, BURROW_SOURCE_CONTENT, 2);
    }
#else
    (void)developer;
#endif
    return vfs;
}

/* Where the renderer finds the shader libraries the build writes,
 * Shaders/<backend>.tbsl, among Burrow's files. While shaders may be
 * compiled at run time, the user's shader cache keeps them between runs. */
static tb_vfs *burrow_shader_sources(burrow_game *game)
{
    char cache[BURROW_PATH_CAPACITY] = {0};

    if (tb_setting_boolean(game->engine->shaders.runtime_compile) &&
        tb_shader_cache_default_directory(cache, sizeof(cache)))
    {
        (void)tb_shader_cache_open(cache, &game->shader_cache);
    }
    return burrow_files(false);
}

/* ---------------------------------------------------------------------------
 * The menus.
 * ------------------------------------------------------------------------- */

/* The options for menus made from the game's content. */
static burrow_menus_options burrow_menus_options_of(burrow_game *game)
{
    return (burrow_menus_options){.content = game->content,
                                  .catalog = game->catalog,
                                  .settings = game->loaded->settings,
                                  .engine = game->engine,
                                  .player_path = game->loaded->player_path,
                                  .theme = game->theme,
                                  .displays = game->displays,
                                  .display_count = game->display_count,
                                  .watch = game->developer,
                                  .trace = game->navigation_count > 0};
}

/* Make the menus, in the game theme; without them, the game plays at once,
 * as it did before it had menus. */
static void burrow_open_menus(burrow_game *game)
{
    char error[BURROW_MENUS_ERROR_CAPACITY] = {0};

    game->content = burrow_files(game->developer);
    game->catalog = tb_font_catalog_create(NULL);
    game->theme = tb_theme_create_game(game->renderer, TB_THEME_GAME_FANTASY);
    game->display_count = tb_window_system_display_count(game->system);
    if (game->display_count > BURROW_MOST_DISPLAYS)
    {
        game->display_count = BURROW_MOST_DISPLAYS;
    }
    for (size_t index = 0; index < game->display_count; index++)
    {
        tb_window_system_display(game->system, index, &game->displays[index]);
    }
    const burrow_menus_options options = burrow_menus_options_of(game);
    game->menus = burrow_menus_create(&options, error);
    if (game->menus == NULL)
    {
        (void)tb_fmt_print_error("burrow: no menus: %s; playing without them\n", error);
        return;
    }
    game->screen = burrow_menus_state(game->menus)->screen;
    if (options.trace)
    {
        (void)tb_fmt_print("burrow: menu: %s\n", burrow_screen_name(game->screen));
    }
}

static void burrow_close_menus(burrow_game *game)
{
    burrow_menus_destroy(game->menus, game->renderer);
    game->menus = NULL;
    tb_theme_destroy(game->theme);
    game->theme = NULL;
    tb_font_catalog_destroy(game->catalog);
    game->catalog = NULL;
    tb_vfs_destroy(game->content);
    game->content = NULL;
}

/* Read a script of navigation: words joined by commas. */
static bool burrow_read_navigation(burrow_game *game, const char *script)
{
    const size_t length = tb_str_length(script);
    size_t start = 0;

    for (size_t at = 0; at <= length; at++)
    {
        if (at < length && script[at] != ',')
        {
            continue;
        }
        char word[BURROW_WORD_CAPACITY] = {0};
        const size_t size = at - start;
        tb_gui_nav action = TB_GUI_NAV_NONE;
        if (size >= sizeof(word) || game->navigation_count >= BURROW_MOST_NAVIGATION)
        {
            (void)tb_fmt_print_error("burrow: error: --navigate: too long\n");
            return false;
        }
        memcpy(word, script + start, size);
        if (!burrow_nav_named(word, &action))
        {
            (void)tb_fmt_print_error("burrow: error: --navigate: '%s' is not an action; the "
                                     "actions are up, down, left, right, accept, back, next, "
                                     "previous, tab_next, tab_previous, and wait\n",
                                     word);
            return false;
        }
        game->navigation[game->navigation_count] = action;
        game->navigation_count++;
        start = at + 1;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The renderer and the window.
 * ------------------------------------------------------------------------- */

/* Make the renderer, and what it draws: the room, the HUD, and the menus. */
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
    burrow_open_menus(game);
    return EXIT_SUCCESS;
}

static void burrow_close_renderer(burrow_game *game)
{
    if (game->renderer == NULL)
    {
        return;
    }
    burrow_close_menus(game);
    burrow_hud_destroy(game->hud, game->renderer);
    burrow_scene_destroy(game->scene, game->renderer);
    tb_renderer_destroy(game->renderer);
    game->renderer = NULL;
}

static int burrow_play_windowed(burrow_game *game)
{
    tb_window_options options = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (tb_window_system_open(NULL, &game->system, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("burrow: error: no window: %s; --headless runs without one\n",
                                 error.message);
        return EXIT_FAILURE;
    }
    tb_engine_settings_window_options(game->engine, BURROW_TITLE, &options);
    /* coverage-off: only a system out of resources refuses a window */
    if (tb_window_create(game->system, &options, &game->window, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("burrow: error: the window could not be made: %s\n",
                                 error.message);
        tb_window_system_close(game->system);
        return EXIT_FAILURE;
    }
    /* coverage-on */
    game->status = burrow_open_renderer(game);
    if (game->status == EXIT_SUCCESS)
    {
        /* Without menus, a shooter starts looking: the first click is not
         * needed. With them, the main menu has the mouse. */
        burrow_capture_mouse(game, game->menus == NULL);
        burrow_run_loop(game, game->system);
        burrow_capture_mouse(game, false);
    }
    burrow_close_renderer(game);
    tb_shader_cache_close(game->shader_cache);
    tb_window_destroy(game->window);
    tb_window_system_close(game->system);
    return game->status;
}

int burrow_play(const burrow_settings *loaded, const burrow_play_options *options)
{
    burrow_game *const game = tb_calloc(1, sizeof(*game));
    int status = EXIT_SUCCESS;

    game->loaded = loaded;
    game->engine = &loaded->engine;
    game->room = burrow_room_graybox();
    game->player = burrow_player_spawn(game->room->spawn, game->room->spawn_yaw);
    game->status = EXIT_SUCCESS;
    game->developer = options->developer;
    game->recreate_on_pause = options->recreate_on_pause;
    game->screen = BURROW_SCREEN_PLAYING;
    if (options->navigate != NULL && !burrow_read_navigation(game, options->navigate))
    {
        tb_free(game);
        return TB_CLI_EXIT_USAGE;
    }
    if (options->headless)
    {
        burrow_run_loop(game, NULL);
        status = game->status;
    }
    else
    {
        status = burrow_play_windowed(game);
    }
    tb_free(game);
    return status;
}

int burrow_check_content(const burrow_settings *loaded, bool developer)
{
    tb_vfs *const content = burrow_files(developer);
    tb_font_catalog *const catalog = tb_font_catalog_create(NULL);
    char error[BURROW_MENUS_ERROR_CAPACITY] = {0};
    int status = EXIT_SUCCESS;

    if (tb_font_catalog_count(catalog) > 0)
    {
        const burrow_menus_options options = {.content = content,
                                              .catalog = catalog,
                                              .settings = loaded->settings,
                                              .engine = &loaded->engine};
        burrow_menus *const menus = burrow_menus_create(&options, error);
        if (menus == NULL)
        {
            (void)tb_fmt_print_error("burrow: error: the menus cannot be built: %s\n", error);
            status = EXIT_FAILURE;
        }
        burrow_menus_destroy(menus, NULL);
    }
    tb_font_catalog_destroy(catalog);
    tb_vfs_destroy(content);
    return status;
}
