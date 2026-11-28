/* Burrow's game: its window, its renderer, its menus, and its loop. */
#ifndef BURROW_GAME_H
#define BURROW_GAME_H

#include "burrow_settings.h"

#include <stdbool.h>

/* The most navigation actions a test's script gives. */
#define BURROW_MOST_NAVIGATION 256

/* How the game is played. */
typedef struct burrow_play_options
{
    /* Without a window, as a server or a test runs: the loop and the
     * player alone, drawing nothing, with no menus. */
    bool headless;
    /* A developer build: the menus' files are read from the source tree
     * where it has them, and loaded again as they change. */
    bool developer;
    /* Optional: navigation actions given to the menus one a frame, from
     * the first, as a test drives them: words joined by commas, as
     * burrow_nav_named() reads them ("down,accept"). The menus then write
     * each screen they show. */
    const char *navigate;
    bool lose_device_on_pause; /* Exercise recovery from unreadable GPU contents. */
    bool recreate_on_pause;    /* Exercise same-adapter recreation once, at the pause menu. */
} burrow_play_options;

/* Play until the player quits or `developer.frame_count` frames have run:
 * in a window, from the main menu over the graybox room, or without one,
 * as a server or a test runs, drawing nothing. Returns the status to exit
 * with; EXIT_FAILURE when no window or no renderer could be made, or the
 * graphics device was lost; TB_CLI_EXIT_USAGE for a script of navigation
 * with a word it does not know. */
int burrow_play(const burrow_settings *loaded, const burrow_play_options *options);

/* Build the menus from Burrow's content without a window, as `burrow check`
 * does to prove that an export carries them. Returns EXIT_SUCCESS, also
 * for a build of the engine that carries no fonts, which has no menus;
 * EXIT_FAILURE, saying why, when a file is missing or wrong. */
int burrow_check_content(const burrow_settings *loaded, bool developer);

#endif
