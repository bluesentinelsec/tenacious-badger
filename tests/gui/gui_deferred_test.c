/* What an options menu needs of the GUI: variables bound to settings that
 * hold their changes until they are applied, reverted, or set to their
 * defaults; the same declared by a layout file; a notebook's tabs held in a
 * layout file; and text drawn in a font of the caller's, as a font browser
 * previews one. */
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

typedef struct options_case
{
    gui_fixture fixture;
    tb_settings *settings;
    tb_setting *volume; /* number, 0 to 1, 0.25 */
    tb_setting *limit;  /* integer, 0 to 360, 0 */
    tb_setting *mode;   /* choice: windowed, borderless, fullscreen; borderless */
    tb_setting *vsync;  /* boolean, true */
    tb_setting *name;   /* text, up to 8 bytes, "Badger" */
} options_case;

static const char *const window_modes[] = {"windowed", "borderless", "fullscreen"};

static void open_options(options_case *made)
{
    gui_fixture_open(&made->fixture, 1.0f);
    made->settings = tb_settings_create();
    tb_settings_add_family(
        made->settings,
        &(tb_settings_family){.name = "test", .title = "Test", .for_players = true});
    made->volume = tb_settings_add(
        made->settings, &(tb_setting_definition){
                            .name = "test.volume",
                            .type = TB_SETTING_NUMBER,
                            .number = {.default_value = 0.25, .minimum = 0.0, .maximum = 1.0}});
    made->limit = tb_settings_add(
        made->settings,
        &(tb_setting_definition){.name = "test.limit",
                                 .type = TB_SETTING_INTEGER,
                                 .integer = {.default_value = 0, .minimum = 0, .maximum = 360}});
    made->mode = tb_settings_add(
        made->settings, &(tb_setting_definition){
                            .name = "test.mode",
                            .type = TB_SETTING_CHOICE,
                            .choice = {.names = window_modes, .count = 3, .default_value = 1}});
    made->vsync = tb_settings_add(made->settings,
                                  &(tb_setting_definition){.name = "test.vsync",
                                                           .type = TB_SETTING_BOOLEAN,
                                                           .boolean = {.default_value = true}});
    made->name = tb_settings_add(
        made->settings, &(tb_setting_definition){.name = "test.name",
                                                 .type = TB_SETTING_TEXT,
                                                 .text = {.default_value = "Badger", .limit = 8}});
    tb_gui_set_settings(made->fixture.gui, made->settings);
}

static void close_options(options_case *made)
{
    gui_fixture_close(&made->fixture);
    tb_settings_destroy(made->settings);
}

static tb_var deferred(options_case *made, tb_gui_type type, tb_setting *setting)
{
    const tb_var var = tb_var_create(made->fixture.gui, &(tb_gui_value){.type = type});

    TB_TEST_REQUIRE(tb_var_bind_setting_deferred(var, setting));
    return var;
}

/* A deferred variable holds what it is given; its setting takes it only
 * when it is applied, as an options menu's Apply. */
TB_TEST(gui_deferred, a_change_waits_for_apply)
{
    options_case made = {0};
    open_options(&made);
    const tb_var volume = deferred(&made, TB_GUI_TYPE_FLOAT, made.volume);

    TB_TEST_EXPECT(tb_var_deferred(volume));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.25);
    TB_TEST_EXPECT(!tb_var_pending(volume));
    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.5));
    TB_TEST_EXPECT(tb_var_pending(volume));
    TB_TEST_EXPECT(tb_gui_settings_pending(made.fixture.gui));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.25);

    TB_TEST_EXPECT(tb_var_apply(volume));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.5);
    TB_TEST_EXPECT(tb_setting_source(made.volume) == TB_SETTING_LAYER_RUNTIME);
    TB_TEST_EXPECT(!tb_var_pending(volume));
    TB_TEST_EXPECT(!tb_gui_settings_pending(made.fixture.gui));
    /* Nothing held is nothing to apply. */
    TB_TEST_EXPECT(tb_var_apply(volume));

    /* Set back to the setting's own value, it holds nothing. */
    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.75));
    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.5));
    TB_TEST_EXPECT(!tb_var_pending(volume));

    /* Bound at once again, it gives its setting each value there and then. */
    TB_TEST_REQUIRE(tb_var_bind_setting(volume, made.volume));
    TB_TEST_EXPECT(!tb_var_deferred(volume));
    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.125));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.125);
    close_options(&made);
}

/* A value the setting would refuse is refused before it is held, but for
 * text, which is kept as typed and refused only when applied. */
TB_TEST(gui_deferred, values_are_checked_as_the_setting_checks_them)
{
    options_case made = {0};
    open_options(&made);
    const tb_var volume = deferred(&made, TB_GUI_TYPE_FLOAT, made.volume);
    const tb_var limit = deferred(&made, TB_GUI_TYPE_FLOAT, made.limit);
    const tb_var limit_text = deferred(&made, TB_GUI_TYPE_TEXT, made.limit);
    const tb_var volume_text = deferred(&made, TB_GUI_TYPE_TEXT, made.volume);
    const tb_var mode = deferred(&made, TB_GUI_TYPE_INT, made.mode);
    const tb_var mode_name = deferred(&made, TB_GUI_TYPE_TEXT, made.mode);
    const tb_var vsync_text = deferred(&made, TB_GUI_TYPE_TEXT, made.vsync);
    const tb_var name = deferred(&made, TB_GUI_TYPE_TEXT, made.name);

    TB_TEST_EXPECT(!tb_var_set_float(volume, 1.5));
    TB_TEST_EXPECT(!tb_var_set_float(limit, 400.0));
    TB_TEST_EXPECT(tb_var_set_float(limit, 143.6));
    TB_TEST_EXPECT(!tb_var_set_int(mode, 3));
    TB_TEST_EXPECT(!tb_var_set_int(mode, -1));
    TB_TEST_EXPECT(tb_var_set_int(mode, 2));
    TB_TEST_EXPECT(!tb_var_pending(volume));

    /* Text is kept as typed; whether the setting would take it is said. */
    TB_TEST_EXPECT(tb_var_set_text(limit_text, "-"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(limit_text));
    TB_TEST_EXPECT(!tb_var_apply(limit_text));
    TB_TEST_EXPECT(tb_var_pending(limit_text));
    TB_TEST_EXPECT(tb_var_set_text(limit_text, "500"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(limit_text));
    TB_TEST_EXPECT(tb_var_set_text(volume_text, "x"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(volume_text));
    TB_TEST_EXPECT(tb_var_set_text(volume_text, "0.5"));
    TB_TEST_EXPECT(tb_var_setting_accepts(volume_text));
    TB_TEST_EXPECT(tb_var_set_text(mode_name, "tiled"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(mode_name));
    TB_TEST_EXPECT(tb_var_set_text(mode_name, "windowed"));
    TB_TEST_EXPECT(tb_var_setting_accepts(mode_name));
    TB_TEST_EXPECT(tb_var_set_text(vsync_text, "maybe"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(vsync_text));
    TB_TEST_EXPECT(tb_var_set_text(vsync_text, "off"));
    TB_TEST_EXPECT(tb_var_setting_accepts(vsync_text));
    TB_TEST_EXPECT(tb_var_set_text(name, "Honey badger"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(name));

    /* Applying every one applies those the settings take, and says that
     * one was refused. */
    TB_TEST_EXPECT(!tb_gui_apply_settings(made.fixture.gui));
    TB_TEST_EXPECT_INT(tb_setting_integer(made.limit), ==, 144);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.mode), ==, 0);
    TB_TEST_EXPECT(!tb_setting_boolean(made.vsync));
    TB_TEST_EXPECT(tb_var_pending(name));
    close_options(&made);
}

/* Revert lets go of what a variable held; Defaults holds the game's own
 * values, without the player's choices or the command line's, until
 * applied. */
TB_TEST(gui_deferred, revert_and_defaults)
{
    options_case made = {0};
    open_options(&made);
    const tb_var volume = deferred(&made, TB_GUI_TYPE_FLOAT, made.volume);
    const tb_var limit = deferred(&made, TB_GUI_TYPE_INT, made.limit);
    const tb_var mode = deferred(&made, TB_GUI_TYPE_INT, made.mode);
    const tb_var mode_name = deferred(&made, TB_GUI_TYPE_TEXT, made.mode);
    const tb_var vsync = deferred(&made, TB_GUI_TYPE_BOOL, made.vsync);
    const tb_var name = deferred(&made, TB_GUI_TYPE_TEXT, made.name);
    static const char game_defaults[] = "test.limit = 144\ntest.name = \"Ba\\\"d\\\\g\\n\"\n";
    static const char player[] = "test.volume = 0.75\ntest.mode = fullscreen\n";

    (void)tb_settings_load_text(made.settings, TB_SETTING_LAYER_DEFAULT_FILE, game_defaults,
                                tb_str_length(game_defaults), NULL, NULL);
    (void)tb_settings_load_text(made.settings, TB_SETTING_LAYER_PLAYER_FILE, player,
                                tb_str_length(player), NULL, NULL);
    TB_TEST_REQUIRE_UINT(
        tb_settings_set(made.settings, TB_SETTING_LAYER_COMMAND_LINE, "test.vsync", "false"), ==,
        TB_SETTING_OK);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.75);
    TB_TEST_EXPECT_INT(tb_var_get_int(mode), ==, 2);

    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.5));
    TB_TEST_REQUIRE(tb_var_revert(volume));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.75);
    TB_TEST_EXPECT(!tb_var_pending(volume));

    tb_gui_restore_default_settings(made.fixture.gui);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.25);
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 144);
    TB_TEST_EXPECT_INT(tb_var_get_int(mode), ==, 1);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(mode_name), TB_TEXT("borderless")));
    TB_TEST_EXPECT(tb_var_get_bool(vsync));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(name), TB_TEXT("Ba\"d\\g\n")));
    TB_TEST_EXPECT(tb_var_pending(volume));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.75);

    tb_gui_revert_settings(made.fixture.gui);
    TB_TEST_EXPECT(!tb_gui_settings_pending(made.fixture.gui));
    TB_TEST_EXPECT_INT(tb_var_get_int(mode), ==, 2);

    /* A variable bound at once gives its setting its default there and
     * then; one bound to none has no default. */
    const tb_var now = tb_var_float(made.fixture.gui, 0.0);
    TB_TEST_REQUIRE(tb_var_bind_setting(now, made.volume));
    TB_TEST_EXPECT(tb_var_restore_default(now));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.25);
    const tb_var unbound = tb_var_float(made.fixture.gui, 1.0);
    TB_TEST_EXPECT(!tb_var_restore_default(unbound));
    TB_TEST_EXPECT(!tb_var_revert(unbound));
    TB_TEST_EXPECT(!tb_var_pending(unbound));
    TB_TEST_EXPECT(!tb_var_apply(TB_VAR_NONE));
    close_options(&made);
}

/* A deferred variable follows its setting's changes made elsewhere while
 * it holds nothing, and keeps what it holds while it does. */
TB_TEST(gui_deferred, changes_elsewhere_reach_it_only_while_it_holds_nothing)
{
    options_case made = {0};
    open_options(&made);
    const tb_var volume = deferred(&made, TB_GUI_TYPE_FLOAT, made.volume);

    TB_TEST_REQUIRE_UINT(tb_setting_set_number(made.volume, 0.5), ==, TB_SETTING_OK);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.5);

    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.75));
    TB_TEST_REQUIRE_UINT(tb_setting_set_number(made.volume, 0.125), ==, TB_SETTING_OK);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.75);
    TB_TEST_EXPECT(tb_var_pending(volume));

    /* The setting comes to the value held: nothing is held any more. */
    TB_TEST_REQUIRE_UINT(tb_setting_set_number(made.volume, 0.75), ==, TB_SETTING_OK);
    gui_update(&made.fixture);
    TB_TEST_EXPECT(!tb_var_pending(volume));
    TB_TEST_REQUIRE_UINT(tb_setting_set_number(made.volume, 1.0), ==, TB_SETTING_OK);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 1.0);

    /* Bound to none, it holds nothing and is no longer deferred. */
    TB_TEST_REQUIRE(tb_var_set_float(volume, 0.0));
    TB_TEST_REQUIRE(tb_var_bind_setting(volume, NULL));
    TB_TEST_EXPECT(!tb_var_deferred(volume));
    TB_TEST_EXPECT(!tb_var_pending(volume));
    close_options(&made);
}

/* ---------------------------------------------------------------------------
 * Layout files.
 * ------------------------------------------------------------------------- */

static char *saved_text(tb_widget widget)
{
    tb_stream stream = {0};
    size_t length = 0;
    const unsigned char *bytes = NULL;
    char *text = NULL;

    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_gui_save(widget, &stream, NULL), ==, TB_GUI_FILE_OK);
    bytes = tb_stream_buffer_bytes(&stream, &length);
    text = tb_malloc(length + 1);
    if (length > 0)
    {
        memcpy(text, bytes, length);
    }
    text[length] = '\0';
    (void)tb_stream_close(&stream);
    return text;
}

static const char deferred_file[] = "{\n"
                                    "  \"format\": \"badger-gui\",\n"
                                    "  \"version\": 1,\n"
                                    "  \"variables\": [\n"
                                    "    {\n"
                                    "      \"name\": \"volume\",\n"
                                    "      \"type\": \"float\",\n"
                                    "      \"setting\": \"test.volume\",\n"
                                    "      \"deferred\": true\n"
                                    "    },\n"
                                    "    {\n"
                                    "      \"name\": \"vsync\",\n"
                                    "      \"type\": \"bool\",\n"
                                    "      \"setting\": \"test.vsync\"\n"
                                    "    }\n"
                                    "  ],\n"
                                    "  \"root\": {\n"
                                    "    \"kind\": \"frame\",\n"
                                    "    \"children\": [\n"
                                    "      {\n"
                                    "        \"kind\": \"scale\",\n"
                                    "        \"properties\": {\n"
                                    "          \"variable\": \"volume\"\n"
                                    "        }\n"
                                    "      },\n"
                                    "      {\n"
                                    "        \"kind\": \"checkbutton\",\n"
                                    "        \"properties\": {\n"
                                    "          \"variable\": \"vsync\"\n"
                                    "        }\n"
                                    "      }\n"
                                    "    ]\n"
                                    "  }\n"
                                    "}\n";

/* A file declares a variable deferred, and saves it so. */
TB_TEST(gui_deferred, a_file_declares_a_variable_deferred)
{
    options_case made = {0};
    open_options(&made);
    tb_widget form = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};

    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(tb_gui_load(tb_gui_root(made.fixture.gui), deferred_file,
                                     tb_str_length(deferred_file), NULL, &form, &error),
                         ==, TB_GUI_FILE_OK);
    tb_test_context_clear();
    TB_TEST_EXPECT(tb_var_deferred(tb_gui_find_var(made.fixture.gui, "volume")));
    TB_TEST_EXPECT(!tb_var_deferred(tb_gui_find_var(made.fixture.gui, "vsync")));
    char *const saved = saved_text(form);
    TB_TEST_EXPECT_STRING(saved, ==, deferred_file);
    tb_free(saved);

    static const char *const wrong[][2] = {
        {"{\"name\": \"a\", \"type\": \"float\", \"deferred\": true}", "deferred, and no setting"},
        {"{\"name\": \"a\", \"type\": \"float\", \"setting\": \"test.volume\", \"deferred\": 1}",
         "expected true or false"},
    };
    for (size_t index = 0; index < TB_TEST_COUNT_OF(wrong); index++)
    {
        char *const text = tb_fmt_string(NULL,
                                         "{\"format\": \"badger-gui\", \"version\": 1, "
                                         "\"variables\": [%s], \"root\": {\"kind\": \"frame\"}}",
                                         wrong[index][0]);
        tb_widget none = TB_WIDGET_NONE;
        TB_TEST_EXPECT_UINT(tb_gui_load(tb_gui_root(made.fixture.gui), text, tb_str_length(text),
                                        NULL, &none, &error),
                            ==, TB_GUI_FILE_INVALID);
        TB_TEST_EXPECT_STRING(error.reason, ==, wrong[index][1]);
        TB_TEST_EXPECT_STRING(error.path, ==, "$.variables[0].deferred");
        tb_str_free(NULL, text);
    }
    close_options(&made);
}

/* A notebook's tabs are its children in a file, their texts a line each in
 * its `tabs`, and the tab chosen comes back chosen. */
TB_TEST(gui_deferred, a_notebook_keeps_its_tabs_in_a_file)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget book = tb_notebook(root, NULL);
    static const char *const titles[] = {"Display", "Graphics", "Audio"};
    tb_widget again = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    tb_gui_value value = {0};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(titles); index++)
    {
        TB_TEST_REQUIRE_UINT(tb_notebook_add(book, tb_frame(book, NULL), titles[index]), ==, index);
    }
    TB_TEST_REQUIRE(tb_notebook_select(book, 1));
    TB_TEST_REQUIRE(tb_widget_get(book, "tabs", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("Display\nGraphics\nAudio")));
    char *const first = saved_text(book);
    TB_TEST_CONTEXT("%s", first);
    TB_TEST_REQUIRE_UINT(tb_gui_load(root, first, tb_str_length(first), NULL, &again, &error), ==,
                         TB_GUI_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_notebook_tab_count(again), ==, 3);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(again), ==, 1);
    TB_TEST_EXPECT(tb_widget_visible(tb_notebook_page(again, 1)));
    TB_TEST_EXPECT(!tb_widget_visible(tb_notebook_page(again, 0)));
    char *const second = saved_text(again);
    TB_TEST_EXPECT_STRING(second, ==, first);
    tb_test_context_clear();

    /* Its tabs renamed by the property; a page without a line is named by
     * its child's name. */
    TB_TEST_REQUIRE(tb_widget_set(
        again, "tabs", &(tb_gui_value){TB_GUI_TYPE_TEXT, {.text = TB_TEXT("One\nTwo")}}));
    TB_TEST_REQUIRE(tb_widget_get(again, "tabs", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("One\nTwo\nAudio")));
    static const char unnamed[] =
        "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"notebook\", "
        "\"properties\": {\"tabs\": \"First\"}, \"children\": [{\"kind\": \"frame\"}, "
        "{\"kind\": \"frame\", \"name\": \"second\"}]}}";
    tb_widget named = TB_WIDGET_NONE;
    TB_TEST_REQUIRE_UINT(tb_gui_load(root, unnamed, tb_str_length(unnamed), NULL, &named, &error),
                         ==, TB_GUI_FILE_OK);
    TB_TEST_REQUIRE(tb_widget_get(named, "tabs", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("First\nsecond")));
    TB_TEST_EXPECT_UINT(tb_notebook_selected(named), ==, 0);

    /* A tab past the pages is refused once there are pages. */
    TB_TEST_EXPECT(
        !tb_widget_set(named, "selected", &(tb_gui_value){TB_GUI_TYPE_INT, {.integer = 5}}));
    TB_TEST_EXPECT(
        !tb_widget_set(named, "selected", &(tb_gui_value){TB_GUI_TYPE_INT, {.integer = -1}}));
    tb_free(second);
    tb_free(first);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * A font of the caller's.
 * ------------------------------------------------------------------------- */

static float face_width;

static void measure_in_face(tb_widget widget, tb_gui_paint *paint, void *user)
{
    const tb_gui_text_block block = {
        .text = TB_TEXT("Badger"), .font = {.size = 20.0f}, .face = user};

    face_width = tb_widget_measure_text(widget, &block).x;
    tb_gui_paint_text(paint, &block, tb_gui_paint_rect(paint), (tb_vec4){1, 1, 1, 1});
}

/* Text is measured and drawn in a font the caller opened, where the GUI has
 * none of its own. */
TB_TEST(gui_deferred, text_is_drawn_in_a_font_of_the_callers)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_gui *const bare = tb_gui_create(NULL);
    const tb_widget canvas = tb_canvas_widget(
        tb_gui_root(bare),
        &(tb_canvas_widget_desc){
            .width = 200.0f, .height = 40.0f, .draw = measure_in_face, .user = fixture.font});
    const tb_gui_text_block plain = {.text = TB_TEXT("Badger")};
    size_t count = 0;

    TB_TEST_REQUIRE(tb_grid(canvas, NULL));
    tb_gui_resize(bare, 400, 300, 1.0f);
    tb_gui_update(bare, 1.0 / 60.0);
    TB_TEST_EXPECT_FLOAT(tb_gui_measure_text(bare, &plain).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(face_width, >, 0.0f);
    (void)tb_gui_commands(bare, &count);
    TB_TEST_EXPECT_UINT(count, >, 0);
    tb_gui_destroy(bare);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * A menu's modal
 * dialog in a letterboxed frame.
 *
 * ------------------------------------------------------------------------- */

/* The shade behind a modal widget covers the whole frame, the bars beside
 * a keep-aspect root
 * included, not the root alone. */
TB_TEST(gui_deferred, the_modal_shade_covers_the_bars_beside_the_root)
{
    tb_gui *const gui = tb_gui_create(&(tb_gui_options){.reference_size = {160.0f, 90.0f},
                                                        .stretch = TB_GUI_STRETCH_KEEP_ASPECT});
    const tb_widget dialog = tb_frame(tb_gui_root(gui), NULL);
    size_t count = 0;
    bool covered = false;

    TB_TEST_REQUIRE(tb_grid(dialog, NULL));
    TB_TEST_REQUIRE(tb_widget_show_modal(dialog));
    /* Twice as wide as the reference's aspect: bars of 80 units each side. */
    tb_gui_resize(gui, 640, 180, 1.0f);
    tb_gui_update(gui, 1.0 / 60.0);
    const tb_ui_command *const commands = tb_gui_commands(gui, &count);
    for (size_t index = 0; index < count; index++)
    {
        const tb_canvas_rect rect = commands[index].rect;
        if (commands[index].layer == TB_GUI_DEFAULT_LAYER + 1 && rect.x == -80.0f &&
            rect.width == 320.0f && rect.height == 90.0f)
        {
            covered = true;
        }
    }
    TB_TEST_EXPECT(covered);
    tb_gui_destroy(gui);
}
