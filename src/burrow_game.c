/* Burrow's game: a window as the settings describe it, and the game loop.
 * Nothing renders yet; the window shows black, and Escape and the close
 * button quit as the build's mode says. */
#include "burrow_game.h"

#include "badger/fmt.h"
#include "badger/game_loop.h"
#include "badger/window.h"

#include <stdlib.h>

#define BURROW_TITLE "Burrow"

static void burrow_run_loop(const burrow_settings *loaded, tb_window_system *system,
                            tb_window *window)
{
    tb_game_loop *const loop = tb_game_loop_create(
        &(tb_game_loop_options){.system = system, .window = window, .settings = &loaded->engine});

    tb_game_loop_run(loop);
    tb_game_loop_destroy(loop);
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
