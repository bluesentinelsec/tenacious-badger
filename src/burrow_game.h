/* Burrow's game: its window and its loop. */
#ifndef BURROW_GAME_H
#define BURROW_GAME_H

#include "burrow_settings.h"

#include <stdbool.h>

/* Play until the player quits or `developer.frame_count` frames have run:
 * in a window, or without one when `headless`, as a server or a test runs.
 * Returns the status to exit with; EXIT_FAILURE when no window could be
 * made. */
int burrow_play(const burrow_settings *loaded, bool headless);

#endif
