/* Burrow's game: a window as the settings describe it, and the game loop.
 * Nothing renders yet; the window shows black, and Escape and the close
 * button quit as the build's mode says. */
#include "burrow_game.h"

#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/game_loop.h"
#include "badger/str.h"
#include "badger/window.h"

#include <stdlib.h>

#define BURROW_TITLE "Burrow"

/* The player's settings are saved when they change while the game runs, as
 * an options menu changes them. */
static void burrow_save_settings(const burrow_settings *loaded, uint64_t version_at_start)
{
    tb_fs_result result = TB_FS_OK;

    if (tb_settings_version(loaded->settings) == version_at_start)
    {
        return;
    }
    result = tb_settings_save_file(
        loaded->settings, (tb_path_view){loaded->player_path, tb_str_length(loaded->player_path)});
    /* coverage-off: only a configuration directory that refuses writes fails */
    if (result != TB_FS_OK)
    {
        (void)tb_fmt_print_error("burrow: the settings could not be saved to %s: %s\n",
                                 loaded->player_path, tb_fs_result_name(result));
    }
    /* coverage-on */
}

static void burrow_run_loop(const burrow_settings *loaded, tb_window_system *system,
                            tb_window *window)
{
    const uint64_t version = tb_settings_version(loaded->settings);
    tb_game_loop *const loop = tb_game_loop_create(
        &(tb_game_loop_options){.system = system, .window = window, .settings = &loaded->engine});

    tb_game_loop_run(loop);
    tb_game_loop_destroy(loop);
    burrow_save_settings(loaded, version);
}

int burrow_play(const burrow_settings *loaded, bool headless)
{
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_window_options options = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (headless)
    {
        burrow_run_loop(loaded, NULL, NULL);
        return EXIT_SUCCESS;
    }
    if (tb_window_system_open(NULL, &system, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("burrow: error: no window: %s; --headless runs without one\n",
                                 error.message);
        return EXIT_FAILURE;
    }
    tb_engine_settings_window_options(&loaded->engine, BURROW_TITLE, &options);
    /* coverage-off: only a system out of resources refuses a window */
    if (tb_window_create(system, &options, &window, &error) != TB_WINDOW_OK)
    {
        (void)tb_fmt_print_error("burrow: error: the window could not be made: %s\n",
                                 error.message);
        tb_window_system_close(system);
        return EXIT_FAILURE;
    }
    /* coverage-on */
    burrow_run_loop(loaded, system, window);
    tb_window_destroy(window);
    tb_window_system_close(system);
    return EXIT_SUCCESS;
}
