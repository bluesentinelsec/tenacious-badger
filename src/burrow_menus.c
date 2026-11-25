/* Burrow's menus: screens from layout files, commands by name, the options
 * bound to the settings and applied, reverted, or set to their defaults,
 * and the questions asked before leaving with changes or quitting. */
#include "burrow_menus.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/version.h"

#include <math.h>
#include <string.h>

/* Where each screen's widgets are, under its own. */
#define BURROW_VERSION_PATH "version"
#define BURROW_MONITOR_PATH "panel/tabs/display/monitor"
#define BURROW_STATUS_PATH "panel/bar/status"

/* What the status line says while changes wait to be applied. */
#define BURROW_PENDING_TEXT "Changes not applied"

/* Bytes of the display names' list, and of a line of it. */
#define BURROW_DISPLAYS_CAPACITY 2048
#define BURROW_DISPLAY_LINE_CAPACITY 192

/* The screens built from files: every screen but playing. */
static const struct
{
    burrow_screen screen;
    const char *path;
} burrow_screen_files[] = {
    {BURROW_SCREEN_MAIN, BURROW_MAIN_MENU_PATH},
    {BURROW_SCREEN_PAUSE, BURROW_PAUSE_MENU_PATH},
    {BURROW_SCREEN_OPTIONS, BURROW_OPTIONS_MENU_PATH},
};

#define BURROW_SCREEN_FILES (sizeof(burrow_screen_files) / sizeof(burrow_screen_files[0]))

/* What a watch on a screen's file is told of a reload: which screen. */
typedef struct burrow_screen_watch
{
    burrow_menus *menus;
    burrow_screen screen;
} burrow_screen_watch;

struct burrow_menus
{
    burrow_menus_options options;
    tb_gui *gui;
    tb_ui_fonts fonts;
    tb_theme *sizes; /* The theme file's, nearer than the game theme. */
    burrow_menu menu;
    tb_widget screens[BURROW_SCREEN_COUNT]; /* None for playing. */
    burrow_screen_watch watches[BURROW_SCREEN_FILES];
    bool pending_shown; /* The status line says changes wait. */
    char version[BURROW_DISPLAY_LINE_CAPACITY];
};

static void burrow_menus_trace(const burrow_menus *menus, const char *what)
{
    if (menus->options.trace)
    {
        (void)tb_fmt_print("burrow: menu: %s\n", what);
    }
}

/* ---------------------------------------------------------------------------
 * Screens.
 * ------------------------------------------------------------------------- */

/* Show the screen the menus are at, and give its first widget the focus. */
static void burrow_menus_show(burrow_menus *menus)
{
    const burrow_screen shown = menus->menu.screen;

    for (size_t index = 0; index < BURROW_SCREEN_COUNT; index++)
    {
        if (tb_widget_alive(menus->screens[index]))
        {
            tb_widget_set_visible(menus->screens[index], index == (size_t)shown);
        }
    }
    if (shown == BURROW_SCREEN_PLAYING)
    {
        tb_gui_close_popups(menus->gui);
        tb_gui_clear_focus(menus->gui);
        return;
    }
    (void)tb_widget_focus_default(menus->screens[shown]);
}

bool burrow_menus_choose(burrow_menus *menus, burrow_choice choice)
{
    const burrow_screen before = menus->menu.screen;

    if (!burrow_menu_choose(&menus->menu, choice))
    {
        return false;
    }
    if (menus->menu.quitting)
    {
        burrow_menus_trace(menus, "quit");
        return true;
    }
    if (menus->menu.screen != before)
    {
        burrow_menus_trace(menus, burrow_screen_name(menus->menu.screen));
        burrow_menus_show(menus);
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The options: applied, reverted, and their defaults.
 * ------------------------------------------------------------------------- */

static void burrow_menus_on_refused(tb_message_result result, void *user)
{
    (void)result;
    (void)user;
}

/* Give the settings the options' changes, and save the player's file. */
static void burrow_menus_apply(burrow_menus *menus)
{
    const bool applied = tb_gui_apply_settings(menus->gui);
    const char *const path = menus->options.player_path;

    burrow_menus_trace(menus, "options applied");
    if (path != NULL)
    {
        const tb_fs_result saved = tb_settings_save_file(menus->options.settings,
                                                         (tb_path_view){path, tb_str_length(path)});
        /* coverage-off: only a file system that refuses the player's own
         * directory fails to save */
        if (saved != TB_FS_OK)
        {
            (void)tb_fmt_print_error("burrow: the settings could not be saved to %s: %s\n", path,
                                     tb_fs_result_name(saved));
        }
        /* coverage-on */
    }
    if (!applied)
    {
        (void)tb_gui_message_box(menus->gui, "Not applied",
                                 "A value was out of its range, and kept as it was.", TB_MESSAGE_OK,
                                 burrow_menus_on_refused, menus);
    }
}

static void burrow_menus_on_apply(const tb_gui_event *event, void *user)
{
    (void)event;
    burrow_menus_apply(user);
}

static void burrow_menus_on_revert(const tb_gui_event *event, void *user)
{
    burrow_menus *const menus = user;

    (void)event;
    tb_gui_revert_settings(menus->gui);
    burrow_menus_trace(menus, "options reverted");
}

static void burrow_menus_on_defaults(const tb_gui_event *event, void *user)
{
    burrow_menus *const menus = user;

    (void)event;
    tb_gui_restore_default_settings(menus->gui);
    burrow_menus_trace(menus, "options set to their defaults");
}

/* ---------------------------------------------------------------------------
 * Going back, and quitting.
 * ------------------------------------------------------------------------- */

static void burrow_menus_on_keep(tb_message_result result, void *user)
{
    burrow_menus *const menus = user;

    if (result == TB_MESSAGE_RESULT_CANCEL)
    {
        (void)tb_widget_focus_default(menus->screens[BURROW_SCREEN_OPTIONS]);
        return;
    }
    if (result == TB_MESSAGE_RESULT_YES)
    {
        burrow_menus_apply(menus);
    }
    else
    {
        tb_gui_revert_settings(menus->gui);
        burrow_menus_trace(menus, "options reverted");
    }
    (void)burrow_menus_choose(menus, BURROW_CHOICE_BACK);
}

static void burrow_menus_on_quit_answer(tb_message_result result, void *user)
{
    if (result == TB_MESSAGE_RESULT_YES)
    {
        (void)burrow_menus_choose(user, BURROW_CHOICE_QUIT);
    }
}

/* Quit, asking first unless `developer.quit_without_asking` says not to,
 * or `always` asks. */
static void burrow_menus_quit(burrow_menus *menus, bool always)
{
    if (!always && tb_setting_boolean(menus->options.engine->developer.quit_without_asking))
    {
        (void)burrow_menus_choose(menus, BURROW_CHOICE_QUIT);
        return;
    }
    (void)tb_gui_message_box(menus->gui, "Quit Burrow?", "Unsaved progress may be lost.",
                             TB_MESSAGE_YES_NO, burrow_menus_on_quit_answer, menus);
}

/* Back, from a screen: the options ask first whether to keep changes not
 * applied; the main menu asks whether to quit. */
static void burrow_menus_back(burrow_menus *menus)
{
    switch (menus->menu.screen)
    {
    case BURROW_SCREEN_OPTIONS:
        if (tb_gui_settings_pending(menus->gui))
        {
            (void)tb_gui_message_box(menus->gui, "Keep your changes?",
                                     "Some options were changed and not applied.",
                                     TB_MESSAGE_YES_NO_CANCEL, burrow_menus_on_keep, menus);
            return;
        }
        (void)burrow_menus_choose(menus, BURROW_CHOICE_BACK);
        return;
    case BURROW_SCREEN_MAIN:
        burrow_menus_quit(menus, true);
        return;
    case BURROW_SCREEN_PAUSE:
    case BURROW_SCREEN_PLAYING:
    case BURROW_SCREEN_COUNT:
        break;
    }
    (void)burrow_menus_choose(menus, BURROW_CHOICE_BACK);
}

static void burrow_menus_on_back_event(const tb_gui_event *event, void *user)
{
    tb_gui_accept_event(event);
    burrow_menus_back(user);
}

/* ---------------------------------------------------------------------------
 * Commands, by the names the files give them.
 * ------------------------------------------------------------------------- */

static void burrow_menus_on_play(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)burrow_menus_choose(user, BURROW_CHOICE_PLAY);
}

static void burrow_menus_on_resume(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)burrow_menus_choose(user, BURROW_CHOICE_RESUME);
}

static void burrow_menus_on_options(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)burrow_menus_choose(user, BURROW_CHOICE_OPTIONS);
}

static void burrow_menus_on_quit_to_menu(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)burrow_menus_choose(user, BURROW_CHOICE_QUIT_TO_MENU);
}

static void burrow_menus_on_quit(const tb_gui_event *event, void *user)
{
    (void)event;
    burrow_menus_quit(user, false);
}

static void burrow_menus_on_back(const tb_gui_event *event, void *user)
{
    (void)event;
    burrow_menus_back(user);
}

static void burrow_menus_register_commands(burrow_menus *menus)
{
    static const struct
    {
        const char *name;
        tb_gui_callback *callback;
    } commands[] = {
        {"play", burrow_menus_on_play},
        {"resume", burrow_menus_on_resume},
        {"options", burrow_menus_on_options},
        {"quit_to_menu", burrow_menus_on_quit_to_menu},
        {"quit", burrow_menus_on_quit},
        {"back", burrow_menus_on_back},
        {"apply_options", burrow_menus_on_apply},
        {"revert_options", burrow_menus_on_revert},
        {"default_options", burrow_menus_on_defaults},
    };

    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); index++)
    {
        (void)tb_gui_register_command(menus->gui, commands[index].name, commands[index].callback,
                                      menus);
    }
}

/* ---------------------------------------------------------------------------
 * Loading the files.
 * ------------------------------------------------------------------------- */

/* The displays' names, for the options' list of them: as the system names
 * each, or by its number, the primary marked. */
static void burrow_menus_name_displays(burrow_menus *menus, tb_widget screen)
{
    const tb_widget list = tb_widget_find(screen, BURROW_MONITOR_PATH);
    char names[BURROW_DISPLAYS_CAPACITY] = {0};
    size_t used = 0;

    if (!tb_widget_alive(list) || menus->options.display_count == 0)
    {
        return;
    }
    for (size_t index = 0; index < menus->options.display_count; index++)
    {
        const tb_display *const display = &menus->options.displays[index];
        char line[BURROW_DISPLAY_LINE_CAPACITY] = {0};
        const char *const name = display->name[0] != '\0' ? display->name : "Display";
        (void)tb_fmt_buffer(line, sizeof(line), "%s%zu: %s%s", index > 0 ? "\n" : "", index + 1,
                            name, display->primary ? " (primary)" : "");
        const size_t length = tb_str_length(line);
        if (used + length >= sizeof(names))
        {
            break;
        }
        memcpy(names + used, line, length);
        used += length;
    }
    names[used] = '\0';
    (void)tb_widget_set(list, "values",
                        &(tb_gui_value){TB_GUI_TYPE_TEXT, {.text = tb_text_make(names, used)}});
}

/* What code gives a screen built from its file: Back, the version, and
 * the displays' names. Hidden unless it is the screen shown. */
static void burrow_menus_prepare(burrow_menus *menus, burrow_screen screen, tb_widget widget)
{
    menus->screens[screen] = widget;
    (void)tb_bind(widget, TB_GUI_EVENT_BACK, burrow_menus_on_back_event, menus);
    const tb_widget version = tb_widget_find(widget, BURROW_VERSION_PATH);
    if (tb_widget_alive(version))
    {
        (void)tb_widget_set_text(version, menus->version);
    }
    burrow_menus_name_displays(menus, widget);
    menus->pending_shown = false;
    tb_widget_set_visible(widget, screen == menus->menu.screen);
}

static void burrow_menus_on_reload(tb_widget widget, const tb_gui_file_error *error, void *user)
{
    const burrow_screen_watch *const watch = user;
    burrow_menus *const menus = watch->menus;

    if (error != NULL)
    {
        (void)tb_fmt_print_error("burrow: %s\n", error->message);
        return;
    }
    burrow_menus_prepare(menus, watch->screen, widget);
    if (watch->screen == menus->menu.screen)
    {
        (void)tb_widget_focus_default(widget);
    }
    (void)tb_fmt_print("burrow: %s loaded again\n", burrow_screen_name(watch->screen));
}

static void burrow_menus_on_theme_reload(tb_theme *theme, const tb_gui_file_error *error,
                                         void *user)
{
    (void)theme;
    (void)user;
    if (error != NULL)
    {
        (void)tb_fmt_print_error("burrow: %s\n", error->message);
        return;
    }
    (void)tb_fmt_print("burrow: %s loaded again\n", BURROW_MENU_THEME_PATH);
}

/* Each screen from its file, watched when the options say. */
static bool burrow_menus_load(burrow_menus *menus, char error[BURROW_MENUS_ERROR_CAPACITY])
{
    const tb_widget root = tb_gui_root(menus->gui);
    tb_gui_file_error failed = {0};

    if (tb_theme_load_file(menus->sizes, menus->options.content, BURROW_MENU_THEME_PATH, &failed) !=
        TB_GUI_FILE_OK)
    {
        (void)tb_fmt_buffer(error, BURROW_MENUS_ERROR_CAPACITY, "%s", failed.message);
        return false;
    }
    (void)tb_widget_set_style_theme(root, menus->sizes);
    for (size_t index = 0; index < BURROW_SCREEN_FILES; index++)
    {
        const burrow_screen screen = burrow_screen_files[index].screen;
        const char *const path = burrow_screen_files[index].path;
        tb_widget made = TB_WIDGET_NONE;
        if (tb_gui_load_file(root, menus->options.content, path, &made, &failed) != TB_GUI_FILE_OK)
        {
            (void)tb_fmt_buffer(error, BURROW_MENUS_ERROR_CAPACITY, "%s", failed.message);
            return false;
        }
        burrow_menus_prepare(menus, screen, made);
        menus->watches[index] = (burrow_screen_watch){menus, screen};
        if (menus->options.watch)
        {
            (void)tb_gui_watch(made, menus->options.content, path,
                               &(tb_gui_watch_options){.callback = burrow_menus_on_reload,
                                                       .user = &menus->watches[index]});
        }
    }
    if (menus->options.watch)
    {
        (void)tb_gui_watch_theme(
            menus->gui, menus->sizes, menus->options.content, BURROW_MENU_THEME_PATH,
            &(tb_theme_watch_options){.callback = burrow_menus_on_theme_reload});
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The menus.
 * ------------------------------------------------------------------------- */

burrow_menus *burrow_menus_create(const burrow_menus_options *options,
                                  char error[BURROW_MENUS_ERROR_CAPACITY])
{
    burrow_menus *const menus = tb_calloc(1, sizeof(*menus));
    tb_ui_theme palette = tb_ui_theme_dark();

    error[0] = '\0';
    menus->options = *options;
    menus->menu = burrow_menu_start();
    (void)tb_fmt_buffer(menus->version, sizeof(menus->version), "Tenacious Badger %s",
                        TB_VERSION_STRING);
    if (!tb_ui_fonts_open(options->catalog, NULL, &menus->fonts))
    {
        (void)tb_fmt_buffer(error, BURROW_MENUS_ERROR_CAPACITY,
                            "this build of the engine carries no fonts");
        burrow_menus_destroy(menus, NULL);
        return NULL;
    }
    tb_ui_theme_set_fonts(&palette, &menus->fonts);
    menus->gui = tb_gui_create(&(tb_gui_options){
        .theme = &palette, .catalog = options->catalog, .style_theme = options->theme});
    tb_gui_set_settings(menus->gui, options->settings);
    burrow_menus_register_commands(menus);
    menus->sizes = tb_theme_create();
    const tb_widget root = tb_gui_root(menus->gui);
    (void)tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    if (!burrow_menus_load(menus, error))
    {
        burrow_menus_destroy(menus, NULL);
        return NULL;
    }
    burrow_menus_show(menus);
    return menus;
}

void burrow_menus_destroy(burrow_menus *menus, tb_renderer *renderer)
{
    if (menus == NULL)
    {
        return;
    }
    if (renderer != NULL && menus->gui != NULL)
    {
        tb_gui_forget_fonts(menus->gui, renderer);
        for (size_t font = 0; font < TB_UI_FONT_COUNT; font++)
        {
            if (menus->fonts.fonts[font] != NULL)
            {
                tb_canvas_forget_font(renderer, menus->fonts.fonts[font]);
            }
        }
    }
    tb_gui_destroy(menus->gui);
    tb_theme_destroy(menus->sizes);
    tb_ui_fonts_close(&menus->fonts);
    tb_free(menus);
}

const burrow_menu *burrow_menus_state(const burrow_menus *menus)
{
    return &menus->menu;
}

tb_gui *burrow_menus_gui(burrow_menus *menus)
{
    return menus->gui;
}

bool burrow_menus_event(burrow_menus *menus, const tb_window_event *event)
{
    const tb_window_event_type type = event->type;

    if (menus->menu.screen == BURROW_SCREEN_PLAYING)
    {
        return false;
    }
    tb_gui_feed_event(menus->gui, event);
    return type == TB_WINDOW_EVENT_KEY_DOWN || type == TB_WINDOW_EVENT_KEY_UP ||
           type == TB_WINDOW_EVENT_TEXT_INPUT || type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN ||
           type == TB_WINDOW_EVENT_MOUSE_BUTTON_UP || type == TB_WINDOW_EVENT_MOUSE_WHEEL ||
           type == TB_WINDOW_EVENT_MOUSE_MOVED;
}

void burrow_menus_navigate(burrow_menus *menus, tb_gui_nav action)
{
    if (menus->menu.screen == BURROW_SCREEN_PLAYING)
    {
        if (action == TB_GUI_NAV_BACK)
        {
            (void)burrow_menus_choose(menus, BURROW_CHOICE_PAUSE);
        }
        return;
    }
    tb_gui_feed_navigation(menus->gui, action);
}

/* The options' status line says while changes wait to be applied. */
static void burrow_menus_show_pending(burrow_menus *menus)
{
    const bool pending = tb_gui_settings_pending(menus->gui);
    const tb_widget status =
        tb_widget_find(menus->screens[BURROW_SCREEN_OPTIONS], BURROW_STATUS_PATH);

    if (pending == menus->pending_shown || !tb_widget_alive(status))
    {
        return;
    }
    (void)tb_widget_set_text(status, pending ? BURROW_PENDING_TEXT : "");
    menus->pending_shown = pending;
}

void burrow_menus_update(burrow_menus *menus, const tb_viewport *viewport, double delta_seconds)
{
    /* The menus were designed at 1920 by 1080 units: they are scaled to fit
     * that within the viewport's units, so that a design size of another
     * scale, or none, lays them out as designed. */
    const float fit =
        fminf(viewport->size.x / BURROW_MENU_WIDTH, viewport->size.y / BURROW_MENU_HEIGHT);

    if (fit > 0.0f)
    {
        tb_gui_set_scale(menus->gui, fit);
    }
    tb_gui_set_viewport(menus->gui, viewport);
    tb_gui_update(menus->gui, delta_seconds);
    burrow_menus_show_pending(menus);
}

void burrow_menus_draw(burrow_menus *menus, tb_renderer *renderer)
{
    if (menus->menu.screen == BURROW_SCREEN_PLAYING)
    {
        return;
    }
    tb_gui_draw(menus->gui, renderer);
}

tb_cursor burrow_menus_cursor(const burrow_menus *menus)
{
    return tb_gui_cursor(menus->gui);
}

bool burrow_nav_named(const char *word, tb_gui_nav *action)
{
    static const char *const names[TB_GUI_NAV_COUNT] = {[TB_GUI_NAV_NONE] = "wait",
                                                        [TB_GUI_NAV_UP] = "up",
                                                        [TB_GUI_NAV_DOWN] = "down",
                                                        [TB_GUI_NAV_LEFT] = "left",
                                                        [TB_GUI_NAV_RIGHT] = "right",
                                                        [TB_GUI_NAV_ACCEPT] = "accept",
                                                        [TB_GUI_NAV_BACK] = "back",
                                                        [TB_GUI_NAV_NEXT] = "next",
                                                        [TB_GUI_NAV_PREVIOUS] = "previous",
                                                        [TB_GUI_NAV_TAB_NEXT] = "tab_next",
                                                        [TB_GUI_NAV_TAB_PREVIOUS] = "tab_previous"};

    for (size_t index = 0; index < TB_GUI_NAV_COUNT; index++)
    {
        if (tb_str_equal(word, names[index]))
        {
            *action = (tb_gui_nav)index;
            return true;
        }
    }
    return false;
}
