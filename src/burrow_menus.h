/* Burrow's menus, built with the engine's retained GUI from layout files in
 * Burrow's content: the main menu, the pause menu, and the options, over
 * the room. The screens are JSON files read through a VFS, their buttons
 * connected to commands by name, and their widgets bound through variables
 * to the engine's settings, which take the changes when the player applies
 * them. A theme file sizes the game theme's text for the menus' reference
 * size, 1920 by 1080 units, which keeps its aspect at any window size.
 *
 * burrow_menu.c says what each choice leads to; this makes the screens
 * that show it, and takes the player's input for them.
 *
 * Threading: stage-owned; the game calls everything on the thread that
 * runs its loop, which also renders. */
#ifndef BURROW_MENUS_H
#define BURROW_MENUS_H

#include "burrow_menu.h"

#include "badger/engine_settings.h"
#include "badger/font_catalog.h"
#include "badger/gui.h"
#include "badger/renderer.h"
#include "badger/settings.h"
#include "badger/vfs.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>

/* The menus' files, by their paths in Burrow's content. */
#define BURROW_MAIN_MENU_PATH "Content/UI/main_menu.json"
#define BURROW_PAUSE_MENU_PATH "Content/UI/pause_menu.json"
#define BURROW_OPTIONS_MENU_PATH "Content/UI/options_menu.json"
#define BURROW_MENU_THEME_PATH "Content/UI/burrow.theme.json"

/* The size the menus are laid out at, in units: whatever the display
 * policy's design size, the menus are scaled to show this much of the
 * viewport. */
#define BURROW_MENU_WIDTH 1920.0f
#define BURROW_MENU_HEIGHT 1080.0f

/* Bytes of the message of a failure to make the menus. */
#define BURROW_MENUS_ERROR_CAPACITY 512

/* What the menus are made with. Each pointer is borrowed, and outlives
 * them. */
typedef struct burrow_menus_options
{
    tb_vfs *content;                  /* Required: where the files are read. */
    const tb_font_catalog *catalog;   /* Required: the menus' fonts. */
    tb_settings *settings;            /* Required: what the options edit. */
    const tb_engine_settings *engine; /* Required: the same, by field. */
    /* The player's settings file, which Apply saves; NULL saves nothing. */
    const char *player_path;
    /* The game theme the menus are drawn by; NULL for the GUI's default,
     * as tests have, without a renderer. */
    tb_theme *theme;
    /* The displays, to name in the options; none leaves the file's names. */
    const tb_display *displays;
    size_t display_count;
    bool watch; /* Load the files again as they change: development builds. */
    bool trace; /* Write each screen shown to standard output, for tests. */
} burrow_menus_options;

/* Burrow's menus. Made by burrow_menus_create(); released by
 * burrow_menus_destroy(). */
typedef struct burrow_menus burrow_menus;

/* Make the menus, showing the main menu. Returns NULL, with why in
 * `error`, when this build of the engine carries no fonts or a file is
 * missing or wrong; the game then plays without menus. */
burrow_menus *burrow_menus_create(const burrow_menus_options *options,
                                  char error[BURROW_MENUS_ERROR_CAPACITY]);

/* Release the menus, outside a frame; `renderer`, the one they were drawn
 * with, forgets their fonts, and is NULL when they were never drawn. NULL
 * menus are ignored. */
void burrow_menus_destroy(burrow_menus *menus, tb_renderer *renderer);

/* Where the menus are. */
const burrow_menu *burrow_menus_state(const burrow_menus *menus);

/* The GUI the menus are built in, for tests that look inside. */
tb_gui *burrow_menus_gui(burrow_menus *menus);

/* Choose as the game does: BURROW_CHOICE_PAUSE when the player presses
 * Escape while playing, or the window loses the focus. Returns whether the
 * choice moved the menus. */
bool burrow_menus_choose(burrow_menus *menus, burrow_choice choice);

/* A window event, while a menu shows: returns true when the menus took it,
 * which is every key, every press, and the mouse's motion, so that the
 * game and its loop do not see them. While playing, takes nothing. */
bool burrow_menus_event(burrow_menus *menus, const tb_window_event *event);

/* A navigation action, as a gamepad's buttons give them. While playing,
 * Back pauses, as Escape does. */
void burrow_menus_navigate(burrow_menus *menus, tb_gui_nav action);

/* Lay the menus out in the renderer's viewport (tb_renderer_viewport()),
 * and handle the input given since the last update. */
void burrow_menus_update(burrow_menus *menus, const tb_viewport *viewport, double delta_seconds);

/* Draw the menus, over everything else, between the renderer's begin and
 * end. Nothing shows while playing. */
void burrow_menus_draw(burrow_menus *menus, tb_renderer *renderer);

/* The mouse's cursor over the menus. */
tb_cursor burrow_menus_cursor(const burrow_menus *menus);

/* The navigation action a word names, as a test's script writes it: "up",
 * "down", "left", "right", "accept", "back", "next", "previous", "tab_next",
 * "tab_previous", or "wait" for none. False for any other word. */
bool burrow_nav_named(const char *word, tb_gui_nav *action);

#endif
