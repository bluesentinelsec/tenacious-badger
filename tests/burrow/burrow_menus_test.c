/* Burrow's menus, built from the layout files Burrow ships, without a
 * window: each screen shown with its first widget focused; the menus
 * walked by navigation alone, as a keyboard or a gamepad walks them; the
 * options' Apply, Revert, and Defaults, and Back's question when changes
 * wait; quitting; and the files' failures, and their reloads, reported. */
#include "burrow_menus.h"

#include "badger/engine_settings.h"
#include "badger/file.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/path.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

/* Burrow's content in the source tree, which the build names; from the
 * tree's root otherwise, as a lint of this file alone compiles it. */
#ifndef BURROW_TEST_CONTENT
#define BURROW_TEST_CONTENT "resources/burrow"
#endif

/* Bytes of a path the tests make. */
#define PATH_CAPACITY 4096

/* The menus' files, by their names in Content/UI. */
static const char *const content_files[] = {"main_menu.json", "pause_menu.json",
                                            "options_menu.json", "burrow.theme.json"};

/* Burrow's defaults the options restore, over the engine's. */
static const char burrow_defaults_text[] = "graphics.field_of_view = 90\n"
                                           "interface.crosshair = true\n";

typedef struct menus_case
{
    tb_settings *settings;
    tb_engine_settings engine;
    tb_font_catalog *catalog;
    tb_vfs *content;
    burrow_menus *menus;
    char root[PATH_CAPACITY];   /* A directory of the case's own. */
    char player[PATH_CAPACITY]; /* The player's settings file in it. */
} menus_case;

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

static void join(const char *directory, const char *name, char path[PATH_CAPACITY])
{
    size_t required = 0;

    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(directory), view(name), path,
                                      PATH_CAPACITY, &required),
                         ==, TB_PATH_OK);
}

/* The settings, a directory of the case's own, and the fonts; a build of
 * the engine without fonts has no menus, and skips. */
static void open_case(menus_case *made)
{
    made->catalog = tb_font_catalog_create(NULL);
    if (tb_font_catalog_count(made->catalog) == 0)
    {
        tb_font_catalog_destroy(made->catalog);
        TB_TEST_SKIP("this build of the engine carries no fonts");
    }
    made->settings = tb_settings_create();
    tb_engine_settings_add(made->settings, &(tb_engine_settings_options){.developer = true},
                           &made->engine);
    (void)tb_settings_load_text(made->settings, TB_SETTING_LAYER_DEFAULT_FILE, burrow_defaults_text,
                                tb_str_length(burrow_defaults_text), NULL, NULL);
    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, made->root, sizeof(made->root)),
                         ==, TB_FS_OK);
    join(made->root, "settings.cfg", made->player);
    made->content = tb_vfs_create(NULL);
}

/* Burrow's content as it ships, from the source tree. */
static void mount_shipped(menus_case *made)
{
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_directory(made->content, "", 0, BURROW_TEST_CONTENT, 0), ==,
                         TB_VFS_OK);
}

/* Burrow's content copied into the case's directory, one file of it
 * replaced by `text` unless NULL, and mounted. */
static void mount_copy(menus_case *made, const char *replaced, const char *text)
{
    char content[PATH_CAPACITY] = {0};
    char ui[PATH_CAPACITY] = {0};

    join(made->root, "Content", content);
    join(content, "UI", ui);
    (void)tb_fs_make_directory(view(content));
    (void)tb_fs_make_directory(view(ui));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(content_files); index++)
    {
        char source[PATH_CAPACITY] = {0};
        char destination[PATH_CAPACITY] = {0};
        tb_fs_buffer bytes = {0};
        (void)tb_fmt_buffer(source, sizeof(source), "%s/Content/UI/%s", BURROW_TEST_CONTENT,
                            content_files[index]);
        join(ui, content_files[index], destination);
        if (replaced != NULL && tb_str_equal(replaced, content_files[index]))
        {
            TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(destination), text, tb_str_length(text)), ==,
                                 TB_FS_OK);
            continue;
        }
        TB_TEST_REQUIRE_UINT(tb_fs_read_file(view(source), NULL, SIZE_MAX, &bytes), ==, TB_FS_OK);
        TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(destination), bytes.data, bytes.length), ==,
                             TB_FS_OK);
        tb_fs_buffer_free(&bytes);
    }
    TB_TEST_REQUIRE_UINT(tb_vfs_mount_directory(made->content, "", 0, made->root, 0), ==,
                         TB_VFS_OK);
}

static burrow_menus_options options_of(menus_case *made)
{
    return (burrow_menus_options){.content = made->content,
                                  .catalog = made->catalog,
                                  .settings = made->settings,
                                  .engine = &made->engine,
                                  .player_path = made->player};
}

static void make_menus(menus_case *made, const burrow_menus_options *options)
{
    char error[BURROW_MENUS_ERROR_CAPACITY] = {0};

    made->menus = burrow_menus_create(options, error);
    TB_TEST_CONTEXT("%s", error);
    TB_TEST_REQUIRE(made->menus != NULL);
    tb_test_context_clear();
}

/* Remove what the case wrote in its directory, and the directory. */
static void remove_files(menus_case *made)
{
    char content[PATH_CAPACITY] = {0};
    char ui[PATH_CAPACITY] = {0};

    join(made->root, "Content", content);
    join(content, "UI", ui);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(content_files); index++)
    {
        char path[PATH_CAPACITY] = {0};
        join(ui, content_files[index], path);
        (void)tb_fs_remove(view(path));
    }
    (void)tb_fs_remove(view(ui));
    (void)tb_fs_remove(view(content));
    (void)tb_fs_remove(view(made->player));
    (void)tb_fs_remove(view(made->root));
}

static void close_case(menus_case *made)
{
    burrow_menus_destroy(made->menus, NULL);
    tb_vfs_destroy(made->content);
    remove_files(made);
    tb_settings_destroy(made->settings);
    tb_font_catalog_destroy(made->catalog);
}

/* A frame of the menus in a frame of pixels, in the viewport of the
 * engine's default display policy, which Burrow's defaults keep. */
static void update_at(menus_case *made, uint32_t width, uint32_t height, double seconds)
{
    const tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    const tb_viewport viewport = tb_viewport_compute(
        &policy, &(tb_viewport_frame){.width = width, .height = height, .display_scale = 1.0f});

    burrow_menus_update(made->menus, &viewport, seconds);
}

/* A frame of the menus at their design size. */
static void update(menus_case *made)
{
    update_at(made, 1920, 1080, 1.0 / 60.0);
}

static void navigate(menus_case *made, tb_gui_nav action)
{
    burrow_menus_navigate(made->menus, action);
    update(made);
}

static tb_widget find(menus_case *made, const char *path)
{
    return tb_widget_find(tb_gui_root(burrow_menus_gui(made->menus)), path);
}

static bool focused(menus_case *made, const char *path)
{
    return (tb_widget_state(find(made, path)) & TB_WIDGET_FOCUSED) != 0;
}

static burrow_screen screen_of(menus_case *made)
{
    return burrow_menus_state(made->menus)->screen;
}

/* The message dialog shown, if any: a child of the root. */
static tb_widget dialog_of(menus_case *made)
{
    for (tb_widget child = tb_widget_first_child(tb_gui_root(burrow_menus_gui(made->menus)));
         tb_widget_alive(child); child = tb_widget_next_sibling(child))
    {
        if (tb_widget_class_of(child) == tb_message_box_class())
        {
            return child;
        }
    }
    return TB_WIDGET_NONE;
}

/* ---------------------------------------------------------------------------
 * The screens.
 * ------------------------------------------------------------------------- */

/* The menus are built from the files Burrow ships, the main menu shown and
 * its Play focused, the others hidden. */
TB_TEST(burrow_menus, the_main_menu_shows_from_its_file)
{
    menus_case made = {0};
    open_case(&made);
    mount_shipped(&made);
    const burrow_menus_options options = options_of(&made);
    make_menus(&made, &options);

    update(&made);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT(tb_widget_visible(find(&made, "main")));
    TB_TEST_EXPECT(!tb_widget_visible(find(&made, "pause")));
    TB_TEST_EXPECT(!tb_widget_visible(find(&made, "options")));
    TB_TEST_EXPECT(focused(&made, "main/buttons/play"));
    /* The engine's version, which no file can know. */
    TB_TEST_EXPECT(tb_text_starts_with(tb_widget_text(find(&made, "main/version")),
                                       TB_TEXT("Tenacious Badger ")));
    /* Laid out at 1920 by 1080 units, whatever the window. */
    update_at(&made, 1280, 720, 1.0 / 60.0);
    TB_TEST_EXPECT_FLOAT(tb_gui_view_of(burrow_menus_gui(made.menus)).size.x, ==, 1920.0f);
    TB_TEST_EXPECT_FLOAT(tb_gui_view_of(burrow_menus_gui(made.menus)).scale, ==, 1280.0f / 1920.0f);
    TB_TEST_EXPECT_INT(burrow_menus_cursor(made.menus), ==, TB_CURSOR_ARROW);
    close_case(&made);
}

/* Every screen is reached by navigation alone, as a keyboard or a gamepad
 * reaches it, Back going to the menu before. */
TB_TEST(burrow_menus, navigation_walks_every_screen)
{
    menus_case made = {0};
    open_case(&made);
    mount_shipped(&made);
    const burrow_menus_options options = options_of(&made);
    make_menus(&made, &options);
    update(&made);

    navigate(&made, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(focused(&made, "main/buttons/options"));
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_OPTIONS);
    TB_TEST_EXPECT(focused(&made, "options/panel/tabs"));
    /* The shoulders go around the tabs. */
    navigate(&made, TB_GUI_NAV_TAB_NEXT);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(find(&made, "options/panel/tabs")), ==, 1);
    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT(focused(&made, "main/buttons/play"));

    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_PLAYING);
    TB_TEST_EXPECT(!tb_widget_visible(find(&made, "main")));
    /* While playing, the menus take no event; Back pauses. */
    const tb_window_event key = {.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_W};
    TB_TEST_EXPECT(!burrow_menus_event(made.menus, &key));
    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_PAUSE);
    TB_TEST_EXPECT(focused(&made, "pause/panel/resume"));
    TB_TEST_EXPECT(burrow_menus_event(made.menus, &key));

    navigate(&made, TB_GUI_NAV_DOWN);
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_OPTIONS);
    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_PAUSE);
    navigate(&made, TB_GUI_NAV_DOWN);
    navigate(&made, TB_GUI_NAV_DOWN);
    TB_TEST_EXPECT(focused(&made, "pause/panel/quit_to_menu"));
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT(!burrow_menus_state(made.menus)->in_game);

    /* The pause menu resumes, by its button and by Back. */
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_REQUIRE(burrow_menus_choose(made.menus, BURROW_CHOICE_PAUSE));
    update(&made);
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_PLAYING);
    TB_TEST_EXPECT(!burrow_menus_choose(made.menus, BURROW_CHOICE_PLAY));
    close_case(&made);
}

/* ---------------------------------------------------------------------------
 * The options.
 * ------------------------------------------------------------------------- */

static void open_options(menus_case *made)
{
    const burrow_menus_options options = options_of(made);

    mount_shipped(made);
    make_menus(made, &options);
    update(made);
    TB_TEST_REQUIRE(burrow_menus_choose(made->menus, BURROW_CHOICE_OPTIONS));
    update(made);
}

static tb_var var_of(menus_case *made, const char *name)
{
    const tb_var var = tb_gui_find_var(burrow_menus_gui(made->menus), name);

    TB_TEST_REQUIRE(tb_var_alive(var));
    return var;
}

/* The player's file, as text the caller frees. */
static bool saved_holds(menus_case *made, const char *line)
{
    tb_fs_buffer bytes = {0};
    bool holds = false;

    if (tb_fs_read_file(view(made->player), NULL, SIZE_MAX, &bytes) != TB_FS_OK)
    {
        return false;
    }
    holds = tb_str_find((const char *)bytes.data, 0, TB_STR_END, line) != TB_STR_NOT_FOUND;
    tb_fs_buffer_free(&bytes);
    return holds;
}

/* Each option edits the setting it names, and every one only once
 * applied: Apply gives the settings the changes and saves the player's
 * file; Revert lets go of them; Defaults brings back the game's own
 * values, to apply in turn. */
TB_TEST(burrow_menus, apply_revert_and_defaults)
{
    menus_case made = {0};
    open_case(&made);
    open_options(&made);
    tb_gui *const gui = burrow_menus_gui(made.menus);
    const tb_var fov = var_of(&made, "field_of_view");
    const tb_var mode = var_of(&made, "window_mode");

    TB_TEST_EXPECT(tb_var_deferred(fov));
    TB_TEST_EXPECT(tb_var_setting(fov) == made.engine.graphics.field_of_view);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 90.0);
    TB_TEST_EXPECT(
        tb_text_equal(tb_widget_text(find(&made, "options/panel/bar/status")), TB_TEXT("")));

    TB_TEST_REQUIRE(tb_var_set_float(fov, 100.0));
    TB_TEST_REQUIRE(tb_var_set_int(mode, 2));
    update(&made);
    TB_TEST_EXPECT(tb_gui_settings_pending(gui));
    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(find(&made, "options/panel/bar/status")),
                                 TB_TEXT("Changes not applied")));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.engine.graphics.field_of_view), ==, 90.0);

    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "options/panel/bar/apply")));
    update(&made);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.engine.graphics.field_of_view), ==, 100.0);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.engine.display.window_mode), ==, 2);
    TB_TEST_EXPECT(saved_holds(&made, "graphics.field_of_view = 100"));
    TB_TEST_EXPECT(saved_holds(&made, "display.window_mode = fullscreen"));
    TB_TEST_EXPECT(!tb_gui_settings_pending(gui));
    TB_TEST_EXPECT(
        tb_text_equal(tb_widget_text(find(&made, "options/panel/bar/status")), TB_TEXT("")));

    TB_TEST_REQUIRE(tb_var_set_float(fov, 110.0));
    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "options/panel/bar/revert")));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 100.0);
    TB_TEST_EXPECT(!tb_gui_settings_pending(gui));

    /* Burrow's own default, not the engine's, and only once applied. */
    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "options/panel/bar/defaults")));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 90.0);
    TB_TEST_EXPECT_INT(tb_var_get_int(mode), ==, 0);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.engine.graphics.field_of_view), ==, 100.0);
    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "options/panel/bar/apply")));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.engine.graphics.field_of_view), ==, 90.0);
    close_case(&made);
}

/* Back with changes not applied asks whether to keep them: Yes applies
 * them and goes back, No lets go of them and goes back, Cancel stays. */
TB_TEST(burrow_menus, back_asks_to_keep_changes)
{
    menus_case made = {0};
    open_case(&made);
    open_options(&made);
    const tb_var fov = var_of(&made, "field_of_view");

    /* Cancel: the dialog's last button, which Back chooses. */
    TB_TEST_REQUIRE(tb_var_set_float(fov, 75.0));
    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_REQUIRE(tb_widget_alive(dialog_of(&made)));
    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!tb_widget_alive(dialog_of(&made)));
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_OPTIONS);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 75.0);

    /* No: the second button. */
    navigate(&made, TB_GUI_NAV_BACK);
    navigate(&made, TB_GUI_NAV_RIGHT);
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 90.0);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.engine.graphics.field_of_view), ==, 90.0);

    /* Yes: the first, which has the focus. */
    TB_TEST_REQUIRE(burrow_menus_choose(made.menus, BURROW_CHOICE_OPTIONS));
    update(&made);
    TB_TEST_REQUIRE(tb_var_set_float(fov, 105.0));
    navigate(&made, TB_GUI_NAV_BACK);
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_MAIN);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.engine.graphics.field_of_view), ==, 105.0);
    TB_TEST_EXPECT(saved_holds(&made, "graphics.field_of_view = 105"));
    close_case(&made);
}

/* A value the setting refuses is not applied, and the player is told. */
TB_TEST(burrow_menus, a_value_refused_is_said)
{
    menus_case made = {0};
    open_case(&made);
    open_options(&made);

    /* Text typed where a number goes is kept as typed, and refused. */
    const tb_var text = tb_var_string(burrow_menus_gui(made.menus), "");
    TB_TEST_REQUIRE(tb_var_bind_setting_deferred(text, made.engine.graphics.field_of_view));
    TB_TEST_REQUIRE(tb_var_set_text(text, "wide"));
    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "options/panel/bar/apply")));
    update(&made);
    TB_TEST_REQUIRE(tb_widget_alive(dialog_of(&made)));
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(!tb_widget_alive(dialog_of(&made)));
    close_case(&made);
}

/* The displays the system has are named in the options' list of them. */
TB_TEST(burrow_menus, the_displays_are_named)
{
    menus_case made = {0};
    open_case(&made);
    mount_shipped(&made);
    tb_display displays[2] = {0};
    tb_gui_value values = {0};
    (void)tb_fmt_buffer(displays[0].name, sizeof(displays[0].name), "Wide");
    displays[0].primary = true;
    burrow_menus_options options = options_of(&made);
    options.displays = displays;
    options.display_count = 2;
    make_menus(&made, &options);

    TB_TEST_REQUIRE(
        tb_widget_get(find(&made, "options/panel/tabs/display/monitor"), "values", &values));
    TB_TEST_EXPECT(tb_text_equal(values.text, TB_TEXT("1: Wide (primary)\n2: Display")));
    close_case(&made);
}

/* ---------------------------------------------------------------------------
 * Quitting.
 * ------------------------------------------------------------------------- */

/* Quit asks first, unless the developer's setting says not to; Back from
 * the main menu always asks. */
TB_TEST(burrow_menus, quitting_asks_first)
{
    menus_case made = {0};
    open_case(&made);
    mount_shipped(&made);
    const burrow_menus_options options = options_of(&made);
    make_menus(&made, &options);
    update(&made);

    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_REQUIRE(tb_widget_alive(dialog_of(&made)));
    navigate(&made, TB_GUI_NAV_BACK);
    TB_TEST_EXPECT(!burrow_menus_state(made.menus)->quitting);
    navigate(&made, TB_GUI_NAV_BACK);
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT(burrow_menus_state(made.menus)->quitting);
    burrow_menus_destroy(made.menus, NULL);

    /* A developer build quits at once; a player's is asked. */
    make_menus(&made, &options);
    update(&made);
    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "main/buttons/quit")));
    TB_TEST_EXPECT(burrow_menus_state(made.menus)->quitting);
    burrow_menus_destroy(made.menus, NULL);
    TB_TEST_REQUIRE(tb_setting_set_boolean(made.engine.developer.quit_without_asking, false) ==
                    TB_SETTING_OK);
    make_menus(&made, &options);
    update(&made);
    TB_TEST_REQUIRE(tb_widget_invoke(find(&made, "main/buttons/quit")));
    update(&made);
    TB_TEST_EXPECT(!burrow_menus_state(made.menus)->quitting);
    TB_TEST_EXPECT(tb_widget_alive(dialog_of(&made)));
    close_case(&made);
}

/* ---------------------------------------------------------------------------
 * The files.
 * ------------------------------------------------------------------------- */

/* A file missing or wrong is said, with where it is wrong, and no menus are
 * made. */
TB_TEST(burrow_menus, a_file_wrong_is_said)
{
    static const struct
    {
        const char *file;
        const char *text;
        const char *said;
    } broken[] = {
        {"pause_menu.json", "{\"format\": \"badger-gui\"", "pause_menu.json:1:"},
        {"main_menu.json",
         "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"frame\", "
         "\"properties\": {\"colour\": 1}}}",
         "unknown property"},
        {"burrow.theme.json", "[]", "burrow.theme.json"},
    };

    for (size_t index = 0; index < TB_TEST_COUNT_OF(broken); index++)
    {
        menus_case made = {0};
        char error[BURROW_MENUS_ERROR_CAPACITY] = {0};
        open_case(&made);
        mount_copy(&made, broken[index].file, broken[index].text);
        const burrow_menus_options options = options_of(&made);
        TB_TEST_CONTEXT("%s", broken[index].file);
        TB_TEST_EXPECT(burrow_menus_create(&options, error) == NULL);
        TB_TEST_EXPECT(tb_str_find(error, 0, TB_STR_END, broken[index].said) != TB_STR_NOT_FOUND);
        close_case(&made);
    }
    tb_test_context_clear();

    /* None at all. */
    menus_case made = {0};
    char error[BURROW_MENUS_ERROR_CAPACITY] = {0};
    open_case(&made);
    const burrow_menus_options options = options_of(&made);
    TB_TEST_EXPECT(burrow_menus_create(&options, error) == NULL);
    TB_TEST_EXPECT(tb_str_find(error, 0, TB_STR_END, "burrow.theme.json") != TB_STR_NOT_FOUND);
    close_case(&made);
}

/* Watched, a file saved again is loaded again in its place, and the screen
 * shown stays shown; one broken while the game runs is said, and the menu
 * kept. */
TB_TEST(burrow_menus, files_load_again_as_they_change)
{
    static const char paused[] =
        "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"frame\", \"name\": "
        "\"pause\", \"grid\": {}, \"children\": [{\"kind\": \"button\", \"name\": \"go\", "
        "\"properties\": {\"text\": \"Go on\", \"command\": \"resume\", \"default_focus\": "
        "true}, \"grid\": {}}]}}";
    menus_case made = {0};
    open_case(&made);
    mount_copy(&made, NULL, NULL);
    burrow_menus_options options = options_of(&made);
    options.watch = true;
    options.trace = true;
    make_menus(&made, &options);
    update(&made);
    TB_TEST_REQUIRE(burrow_menus_choose(made.menus, BURROW_CHOICE_PLAY));
    TB_TEST_REQUIRE(burrow_menus_choose(made.menus, BURROW_CHOICE_PAUSE));
    update(&made);

    char path[PATH_CAPACITY] = {0};
    (void)tb_fmt_buffer(path, sizeof(path), "%s/Content/UI/pause_menu.json", made.root);
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), paused, tb_str_length(paused)), ==, TB_FS_OK);
    update_at(&made, 1920, 1080, 1.0);
    TB_TEST_EXPECT(tb_widget_alive(find(&made, "pause/go")));
    TB_TEST_EXPECT(tb_widget_visible(find(&made, "pause")));
    TB_TEST_EXPECT(focused(&made, "pause/go"));
    navigate(&made, TB_GUI_NAV_ACCEPT);
    TB_TEST_EXPECT_INT(screen_of(&made), ==, BURROW_SCREEN_PLAYING);

    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), "{", 1), ==, TB_FS_OK);
    update_at(&made, 1920, 1080, 1.0);
    TB_TEST_EXPECT(tb_widget_alive(find(&made, "pause/go")));
    TB_TEST_EXPECT(!tb_widget_visible(find(&made, "pause")));

    /* The theme's file as well. */
    char theme[PATH_CAPACITY] = {0};
    (void)tb_fmt_buffer(theme, sizeof(theme), "%s/Content/UI/burrow.theme.json", made.root);
    static const char sizes[] = "{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {}}";
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(theme), sizes, tb_str_length(sizes)), ==, TB_FS_OK);
    update_at(&made, 1920, 1080, 1.0);
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(theme), "[", 1), ==, TB_FS_OK);
    update_at(&made, 1920, 1080, 1.0);
    close_case(&made);
}

TB_TEST(burrow_menus, navigation_has_names)
{
    tb_gui_nav action = TB_GUI_NAV_COUNT;

    TB_TEST_EXPECT(burrow_nav_named("tab_next", &action));
    TB_TEST_EXPECT_INT(action, ==, TB_GUI_NAV_TAB_NEXT);
    TB_TEST_EXPECT(burrow_nav_named("wait", &action));
    TB_TEST_EXPECT_INT(action, ==, TB_GUI_NAV_NONE);
    TB_TEST_EXPECT(!burrow_nav_named("jump", &action));
}
