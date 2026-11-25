/* Burrow's menus as a state machine, and the main menu's orbiting camera. */
#include "burrow_menu.h"

#include "badger/str.h"

#include <math.h>
#include <stddef.h>

/* The orbit: its radius and height in meters, around the room's middle,
 * and the height it looks at. */
#define BURROW_ORBIT_RADIUS 13.0f
#define BURROW_ORBIT_HEIGHT 7.5f
#define BURROW_ORBIT_TARGET_HEIGHT 1.0f

static const char *const burrow_screen_names[BURROW_SCREEN_COUNT] = {"playing", "main", "pause",
                                                                     "options"};
static const char *const burrow_choice_names[BURROW_CHOICE_COUNT] = {
    "play", "pause", "resume", "options", "quit_to_menu", "quit", "back"};

burrow_menu burrow_menu_start(void)
{
    burrow_menu menu = {0};

    menu.screen = BURROW_SCREEN_MAIN;
    menu.options_from = BURROW_SCREEN_MAIN;
    return menu;
}

static bool burrow_menu_go(burrow_menu *menu, burrow_screen screen)
{
    menu->screen = screen;
    return true;
}

static bool burrow_menu_open_options(burrow_menu *menu)
{
    menu->options_from = menu->screen;
    return burrow_menu_go(menu, BURROW_SCREEN_OPTIONS);
}

static bool burrow_menu_quit(burrow_menu *menu)
{
    menu->quitting = true;
    return true;
}

static bool burrow_menu_choose_main(burrow_menu *menu, burrow_choice choice)
{
    switch (choice)
    {
    case BURROW_CHOICE_PLAY:
        menu->in_game = true;
        return burrow_menu_go(menu, BURROW_SCREEN_PLAYING);
    case BURROW_CHOICE_OPTIONS:
        return burrow_menu_open_options(menu);
    case BURROW_CHOICE_QUIT:
        return burrow_menu_quit(menu);
    default:
        return false;
    }
}

static bool burrow_menu_choose_pause(burrow_menu *menu, burrow_choice choice)
{
    switch (choice)
    {
    case BURROW_CHOICE_RESUME:
    case BURROW_CHOICE_BACK:
        return burrow_menu_go(menu, BURROW_SCREEN_PLAYING);
    case BURROW_CHOICE_OPTIONS:
        return burrow_menu_open_options(menu);
    case BURROW_CHOICE_QUIT_TO_MENU:
        menu->in_game = false;
        return burrow_menu_go(menu, BURROW_SCREEN_MAIN);
    case BURROW_CHOICE_QUIT:
        return burrow_menu_quit(menu);
    default:
        return false;
    }
}

bool burrow_menu_choose(burrow_menu *menu, burrow_choice choice)
{
    if (menu->quitting)
    {
        return false;
    }
    switch (menu->screen)
    {
    case BURROW_SCREEN_PLAYING:
        return choice == BURROW_CHOICE_PAUSE && burrow_menu_go(menu, BURROW_SCREEN_PAUSE);
    case BURROW_SCREEN_MAIN:
        return burrow_menu_choose_main(menu, choice);
    case BURROW_SCREEN_PAUSE:
        return burrow_menu_choose_pause(menu, choice);
    case BURROW_SCREEN_OPTIONS:
        return choice == BURROW_CHOICE_BACK && burrow_menu_go(menu, menu->options_from);
    case BURROW_SCREEN_COUNT:
        break;
    }
    return false;
}

bool burrow_menu_simulates(const burrow_menu *menu)
{
    return menu->screen == BURROW_SCREEN_PLAYING && !menu->quitting;
}

const char *burrow_screen_name(burrow_screen screen)
{
    if (screen >= BURROW_SCREEN_COUNT)
    {
        return "";
    }
    return burrow_screen_names[screen];
}

burrow_choice burrow_choice_named(const char *word)
{
    for (size_t index = 0; index < BURROW_CHOICE_COUNT; index++)
    {
        if (tb_str_equal(word, burrow_choice_names[index]))
        {
            return (burrow_choice)index;
        }
    }
    return BURROW_CHOICE_COUNT;
}

void burrow_orbit_view(double seconds, tb_vec3 *eye, float *yaw, float *pitch)
{
    const double turns = fmod(seconds / BURROW_ORBIT_SECONDS, 1.0);
    const float angle = (float)(turns * 2.0 * TB_PI);
    const float drop = BURROW_ORBIT_HEIGHT - BURROW_ORBIT_TARGET_HEIGHT;

    /* At angle 0 the eye is south of the middle (+Z), looking north (-Z),
     * as a yaw of 0 looks; the orbit turns counter-clockwise seen from
     * above, as the yaw does. */
    *eye = (tb_vec3){BURROW_ORBIT_RADIUS * sinf(angle), BURROW_ORBIT_HEIGHT,
                     BURROW_ORBIT_RADIUS * cosf(angle)};
    *yaw = tb_wrap_angle(angle);
    *pitch = -atanf(drop / BURROW_ORBIT_RADIUS);
}
