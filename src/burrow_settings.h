/* Burrow's settings: the engine's, Burrow's defaults over them, the player's
 * file, and the command line, loaded in that order. */
#ifndef BURROW_SETTINGS_H
#define BURROW_SETTINGS_H

#include "badger/engine_settings.h"
#include "badger/settings.h"

#include <stdbool.h>
#include <stddef.h>

/* Burrow's settings once they are loaded. */
typedef struct burrow_settings
{
    tb_settings *settings;
    tb_engine_settings engine;
    char *player_path; /* The player's file; the heap's. */
} burrow_settings;

/* What the command line asked of them. */
typedef struct burrow_settings_request
{
    bool developer;            /* A developer build's defaults. */
    bool reset;                /* Set the player's file aside first. */
    const char *player_file;   /* The player's file; NULL for the usual place. */
    const char *const *values; /* NAME=VALUE, for the command line layer. */
    size_t value_count;
    const char *renderer; /* --renderer: graphics.renderer, after the values; NULL for none. */
} burrow_settings_request;

/* Load the settings as the request says, writing problems with the files to
 * standard error. Returns EXIT_SUCCESS, TB_CLI_EXIT_USAGE for a NAME=VALUE
 * that is wrong, or EXIT_FAILURE when the player's file cannot be found or
 * set aside; `loaded` holds the settings only on success. */
int burrow_settings_load(const burrow_settings_request *request, burrow_settings *loaded);

/* Release what burrow_settings_load() loaded. */
void burrow_settings_release(burrow_settings *loaded);

/* Write every setting, family by family, with its value and its layer, to
 * standard output. */
void burrow_settings_list(const burrow_settings *loaded);

#endif
