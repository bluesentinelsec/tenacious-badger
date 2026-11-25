/* Burrow's menus as a state machine: which screen shows, what each choice
 * on it leads to, and where Back goes. The screens themselves, their widgets
 * and their layout files, are burrow_menus.c's; this is the logic alone, so
 * that it is tested without a window or a GUI.
 *
 * The game starts at the main menu, over the room, which a camera orbits.
 * Play starts playing; Escape while playing pauses, and the pause menu
 * resumes, opens the options, quits to the main menu, or quits. The options
 * go back to the menu they were opened from.
 *
 * Threading: externally synchronized; the game calls everything on the
 * thread that runs its loop. */
#ifndef BURROW_MENU_H
#define BURROW_MENU_H

#include "badger/math.h"

#include <stdbool.h>

/* What shows. */
typedef enum burrow_screen
{
    BURROW_SCREEN_PLAYING, /* No menu: the player has the mouse and the keys. */
    BURROW_SCREEN_MAIN,    /* The main menu: Play, Options, Quit. */
    BURROW_SCREEN_PAUSE,   /* The pause menu: Resume, Options, Quit to menu, Quit. */
    BURROW_SCREEN_OPTIONS, /* The options, opened from the main or the pause menu. */
    BURROW_SCREEN_COUNT
} burrow_screen;

/* What the player chooses. */
typedef enum burrow_choice
{
    BURROW_CHOICE_PLAY,         /* Main menu: start playing. */
    BURROW_CHOICE_PAUSE,        /* Playing: Escape, or the window losing the focus. */
    BURROW_CHOICE_RESUME,       /* Pause menu: play on. */
    BURROW_CHOICE_OPTIONS,      /* Main or pause menu: the options. */
    BURROW_CHOICE_QUIT_TO_MENU, /* Pause menu: leave the game for the main menu. */
    BURROW_CHOICE_QUIT,         /* Main or pause menu: quit the game. */
    BURROW_CHOICE_BACK,         /* Escape, or a gamepad's B: the menu before. */
    BURROW_CHOICE_COUNT
} burrow_choice;

/* Where the menus are. Start from burrow_menu_start(). */
typedef struct burrow_menu
{
    burrow_screen screen;
    burrow_screen options_from; /* The menu the options go back to. */
    bool quitting;              /* The player chose to quit. */
    /* A game is under way: set by Play, cleared by quitting to the menu,
     * which starts the next one afresh. */
    bool in_game;
} burrow_menu;

/* The menus as the game starts: the main menu, no game under way. */
burrow_menu burrow_menu_start(void);

/* Choose: returns true when the choice means something on the screen
 * shown, and moves the menus as it says; false, changing nothing,
 * otherwise. Back from the main menu means nothing: quitting is a choice of
 * its own. */
bool burrow_menu_choose(burrow_menu *menu, burrow_choice choice);

/* Whether the world moves: only while playing. A menu over the room
 * freezes it. */
bool burrow_menu_simulates(const burrow_menu *menu);

/* The screen's name, as the menus' files and the log name it: "playing",
 * "main", "pause", "options". */
const char *burrow_screen_name(burrow_screen screen);

/* The choice a word names, as a test's script writes it ("play", "back");
 * BURROW_CHOICE_COUNT for none. */
burrow_choice burrow_choice_named(const char *word);

/* Seconds the main menu's camera takes to go once around the room. */
#define BURROW_ORBIT_SECONDS 90.0

/* Where the main menu's camera is, `seconds` after the menu showed: around
 * the room's middle, high over its floor, looking down at it, going once
 * around every BURROW_ORBIT_SECONDS. `yaw` and `pitch` are as the player's. */
void burrow_orbit_view(double seconds, tb_vec3 *eye, float *yaw, float *pitch);

#endif
