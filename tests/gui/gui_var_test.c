/* The retained GUI's variables: values of four types read and set across
 * them, observers run during updates, names, handles that go stale, the
 * variable property type, and variables bound to engine settings in both
 * directions, with the settings' own checks. */
#include "badger/settings.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <math.h>

/* Changes an observer saw: how many, and the last variable. */
typedef struct seen
{
    int count;
    tb_var last;
    tb_var set_too; /* a variable the observer sets, when alive */
    bool untrace;   /* the observer removes itself */
    bool destroy;   /* the observer destroys its variable */
} seen;

static void observe(tb_var var, void *user)
{
    seen *const record = user;

    record->count++;
    record->last = var;
    if (tb_var_alive(record->set_too))
    {
        (void)tb_var_set_int(record->set_too, tb_var_get_int(record->set_too) + 1);
    }
    if (record->untrace)
    {
        (void)tb_var_untrace(var, observe, user);
    }
    if (record->destroy)
    {
        tb_var_destroy(var);
    }
}

TB_TEST(gui_var, each_type_holds_its_value_and_reads_as_the_others)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var flag = tb_var_bool(fixture.gui, true);
    const tb_var count = tb_var_int(fixture.gui, -42);
    const tb_var number = tb_var_float(fixture.gui, 2.5);
    const tb_var text = tb_var_string(fixture.gui, "7");
    tb_gui_value value = {0};

    TB_TEST_EXPECT_UINT(tb_var_type(flag), ==, TB_GUI_TYPE_BOOL);
    TB_TEST_EXPECT_UINT(tb_var_type(count), ==, TB_GUI_TYPE_INT);
    TB_TEST_EXPECT_UINT(tb_var_type(number), ==, TB_GUI_TYPE_FLOAT);
    TB_TEST_EXPECT_UINT(tb_var_type(text), ==, TB_GUI_TYPE_TEXT);

    /* Each reads as every type, as Tk's variables read as text. */
    TB_TEST_EXPECT(tb_var_get_bool(flag));
    TB_TEST_EXPECT_INT(tb_var_get_int(flag), ==, 1);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(flag), TB_TEXT("1")));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, -42);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(count), ==, -42.0);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(count), TB_TEXT("-42")));
    TB_TEST_EXPECT(tb_var_get_bool(count));
    TB_TEST_EXPECT_INT(tb_var_get_int(number), ==, 3);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(number), TB_TEXT("2.5")));
    TB_TEST_EXPECT_INT(tb_var_get_int(text), ==, 7);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(text), ==, 7.0);
    TB_TEST_EXPECT(!tb_var_get_bool(text));

    TB_TEST_REQUIRE(tb_var_get(number, &value));
    TB_TEST_EXPECT_UINT(value.type, ==, TB_GUI_TYPE_FLOAT);
    TB_TEST_EXPECT_FLOAT(value.number, ==, 2.5f);
    TB_TEST_REQUIRE(tb_var_get(text, &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("7")));
    TB_TEST_REQUIRE(tb_var_get(flag, &value));
    TB_TEST_EXPECT(value.boolean);
    TB_TEST_REQUIRE(tb_var_get(count, &value));
    TB_TEST_EXPECT_INT(value.integer, ==, -42);

    /* Only the four types make a variable. */
    const tb_gui_value pair = tb_gui_vec2((tb_vec2){1.0f, 2.0f});
    const tb_gui_value infinite = tb_gui_float(INFINITY);
    TB_TEST_EXPECT(!tb_var_alive(tb_var_create(fixture.gui, &pair)));
    TB_TEST_EXPECT(!tb_var_alive(tb_var_create(fixture.gui, &infinite)));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_var, setting_converts_to_the_variables_type_or_refuses)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var flag = tb_var_bool(fixture.gui, false);
    const tb_var count = tb_var_int(fixture.gui, 0);
    const tb_var number = tb_var_float(fixture.gui, 0.0);
    const tb_var text = tb_var_string(fixture.gui, "");

    /* Text as a layout file writes values. */
    TB_TEST_EXPECT(tb_var_set_text(flag, "Yes"));
    TB_TEST_EXPECT(tb_var_get_bool(flag));
    TB_TEST_EXPECT(tb_var_set_text(flag, "off"));
    TB_TEST_EXPECT(!tb_var_get_bool(flag));
    TB_TEST_EXPECT(!tb_var_set_text(flag, "maybe"));
    TB_TEST_EXPECT(tb_var_set_text(count, "+12"));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, 12);
    TB_TEST_EXPECT(!tb_var_set_text(count, "1.5"));
    TB_TEST_EXPECT(!tb_var_set_text(count, "99999999999"));
    TB_TEST_EXPECT(!tb_var_set_text(count, "-"));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, 12);
    TB_TEST_EXPECT(tb_var_set_text(number, "0.25"));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(number), ==, 0.25);
    TB_TEST_EXPECT(!tb_var_set_text(number, "nan"));
    TB_TEST_EXPECT(!tb_var_set_text(number, "x"));

    /* Numbers across types: rounded to integers, written as text. */
    TB_TEST_EXPECT(tb_var_set_float(count, 2.6));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, 3);
    TB_TEST_EXPECT(!tb_var_set_float(count, 1e12));
    TB_TEST_EXPECT(!tb_var_set_float(number, INFINITY));
    TB_TEST_EXPECT(tb_var_set_bool(count, true));
    TB_TEST_EXPECT_INT(tb_var_get_int(count), ==, 1);
    TB_TEST_EXPECT(tb_var_set_int(flag, 5));
    TB_TEST_EXPECT(tb_var_get_bool(flag));
    TB_TEST_EXPECT(tb_var_set_float(text, 0.5));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("0.5")));
    TB_TEST_EXPECT(tb_var_set_int(text, 30));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("30")));
    TB_TEST_EXPECT(tb_var_set_bool(text, true));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("1")));

    /* The generic setter takes any of the four types. */
    const tb_gui_value words = tb_gui_text(TB_TEXT("hello"));
    const tb_gui_value pair = tb_gui_vec2((tb_vec2){0});
    TB_TEST_EXPECT(tb_var_set(text, &words));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("hello")));
    const tb_gui_value half = tb_gui_float(0.5f);
    TB_TEST_EXPECT(tb_var_set(number, &half));
    const tb_gui_value one = tb_gui_bool(true);
    TB_TEST_EXPECT(tb_var_set(number, &one));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(number), ==, 1.0);
    const tb_gui_value nine = tb_gui_int(9);
    TB_TEST_EXPECT(tb_var_set(number, &nine));
    TB_TEST_EXPECT(!tb_var_set(number, &pair));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_var, observers_run_during_the_update_once_for_each_change)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var volume = tb_var_int(fixture.gui, 1);
    const tb_var echo = tb_var_int(fixture.gui, 0);
    seen first = {.set_too = echo};
    seen second = {0};
    seen echoed = {0};

    TB_TEST_REQUIRE(tb_var_trace(volume, observe, &first));
    TB_TEST_REQUIRE(tb_var_trace(volume, observe, &second));
    TB_TEST_REQUIRE(tb_var_trace(echo, observe, &echoed));
    const uint64_t version = tb_var_version(volume);

    /* Set outside an update: nothing runs until the next. */
    TB_TEST_EXPECT(tb_var_set_int(volume, 2));
    TB_TEST_EXPECT(tb_var_set_int(volume, 3));
    TB_TEST_EXPECT_INT(first.count, ==, 0);
    TB_TEST_EXPECT_UINT(tb_var_version(volume), ==, version + 2);
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(first.count, ==, 1);
    TB_TEST_EXPECT_INT(second.count, ==, 1);
    TB_TEST_EXPECT(tb_var_equal(first.last, volume));
    /* The first observer set another variable: its observer ran too, in
     * the same update. */
    TB_TEST_EXPECT_INT(echoed.count, ==, 1);
    TB_TEST_EXPECT_INT(tb_var_get_int(echo), ==, 1);

    /* Setting a variable to its value is no change. */
    TB_TEST_EXPECT(tb_var_set_int(volume, 3));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(first.count, ==, 1);

    /* Removed observers run no more; one may remove itself while it runs. */
    second.untrace = true;
    TB_TEST_EXPECT(tb_var_set_int(volume, 4));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(second.count, ==, 2);
    TB_TEST_EXPECT(tb_var_set_int(volume, 5));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(second.count, ==, 2);
    TB_TEST_EXPECT_INT(first.count, ==, 3);
    TB_TEST_EXPECT_UINT(tb_var_untrace(volume, NULL, NULL), ==, 1);
    TB_TEST_EXPECT_UINT(tb_var_untrace(volume, observe, &first), ==, 0);
    TB_TEST_EXPECT(tb_var_set_int(volume, 6));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(first.count, ==, 3);

    /* An observer may destroy its variable. */
    seen ending = {.destroy = true};
    const tb_var doomed = tb_var_int(fixture.gui, 0);
    TB_TEST_REQUIRE(tb_var_trace(doomed, observe, &ending));
    TB_TEST_REQUIRE(tb_var_trace(doomed, observe, &first));
    TB_TEST_EXPECT(tb_var_set_int(doomed, 1));
    gui_update(&fixture);
    TB_TEST_EXPECT_INT(ending.count, ==, 1);
    TB_TEST_EXPECT_INT(first.count, ==, 3);
    TB_TEST_EXPECT(!tb_var_alive(doomed));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_var, a_destroyed_variable_goes_stale_and_names_find_the_living)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var first = tb_var_int(fixture.gui, 1);
    tb_gui_value value = {0};
    seen record = {0};

    tb_var_set_name(first, "volume");
    TB_TEST_EXPECT(tb_text_equal(tb_var_name(first), TB_TEXT("volume")));
    TB_TEST_EXPECT(tb_var_equal(tb_gui_find_var(fixture.gui, "volume"), first));
    TB_TEST_EXPECT(!tb_var_alive(tb_gui_find_var(fixture.gui, "missing")));
    TB_TEST_EXPECT(!tb_var_alive(tb_gui_find_var(fixture.gui, "")));

    tb_var_destroy(first);
    TB_TEST_EXPECT(!tb_var_alive(first));
    TB_TEST_EXPECT(!tb_var_alive(tb_gui_find_var(fixture.gui, "volume")));
    /* Its slot is used again under another generation. */
    const tb_var second = tb_var_int(fixture.gui, 2);
    TB_TEST_EXPECT_UINT(second.index, ==, first.index);
    TB_TEST_EXPECT(!tb_var_equal(first, second));

    /* Every function given a stale handle does nothing. */
    TB_TEST_EXPECT_UINT(tb_var_type(first), ==, TB_GUI_TYPE_COUNT);
    TB_TEST_EXPECT(!tb_var_get(first, &value));
    TB_TEST_EXPECT(!tb_var_set_int(first, 3));
    TB_TEST_EXPECT(!tb_var_set_text(first, "3"));
    TB_TEST_EXPECT_INT(tb_var_get_int(first), ==, 0);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(first), ==, 0.0);
    TB_TEST_EXPECT(!tb_var_get_bool(first));
    TB_TEST_EXPECT_UINT(tb_var_get_text(first).length, ==, 0);
    TB_TEST_EXPECT_UINT(tb_var_version(first), ==, 0);
    TB_TEST_EXPECT(!tb_var_trace(first, observe, &record));
    TB_TEST_EXPECT_UINT(tb_var_untrace(first, NULL, NULL), ==, 0);
    TB_TEST_EXPECT_UINT(tb_var_name(first).length, ==, 0);
    TB_TEST_EXPECT(!tb_var_bind_setting(first, NULL));
    TB_TEST_EXPECT_POINTER(tb_var_setting(first), ==, NULL);
    TB_TEST_EXPECT(tb_var_setting_accepts(first));
    tb_var_set_name(first, "ignored");
    tb_var_destroy(first);
    TB_TEST_EXPECT(!tb_var_alive(TB_VAR_NONE));
    TB_TEST_EXPECT_INT(tb_var_get_int(second), ==, 2);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_var, a_variable_property_takes_none_or_a_living_variable)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget check = tb_checkbutton(root, NULL);
    const tb_var flag = tb_var_bool(fixture.gui, true);
    const tb_gui_value bound = tb_gui_variable(flag);
    const tb_gui_value none = tb_gui_variable(TB_VAR_NONE);
    tb_gui_value value = {0};

    const tb_gui_property *const property = tb_widget_find_property(check, "variable");
    TB_TEST_REQUIRE_POINTER(property, !=, NULL);
    TB_TEST_EXPECT_UINT(property->type, ==, TB_GUI_TYPE_VARIABLE);
    TB_TEST_EXPECT(tb_widget_set(check, "variable", &bound));
    TB_TEST_REQUIRE(tb_widget_get(check, "variable", &value));
    TB_TEST_EXPECT(tb_var_equal(value.variable, flag));
    TB_TEST_REQUIRE(tb_widget_get(check, "selected", &value));
    TB_TEST_EXPECT(value.boolean);

    /* A stale variable is refused; none unbinds. */
    tb_var_destroy(flag);
    TB_TEST_EXPECT(!tb_widget_set(check, "variable", &bound));
    TB_TEST_EXPECT(tb_widget_set(check, "variable", &none));
    TB_TEST_REQUIRE(tb_widget_get(check, "variable", &value));
    TB_TEST_EXPECT(!tb_var_alive(value.variable));

    /* A variable of another GUI is refused. */
    tb_gui *const other = tb_gui_create(NULL);
    const tb_gui_value foreign = tb_gui_variable(tb_var_bool(other, true));
    TB_TEST_EXPECT(!tb_widget_set(check, "variable", &foreign));
    tb_gui_destroy(other);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Settings.
 * ------------------------------------------------------------------------- */

static const char *const window_modes[] = {"windowed", "borderless", "fullscreen"};

typedef struct settings_fixture
{
    tb_settings *settings;
    tb_setting *vsync;
    tb_setting *frame_limit;
    tb_setting *volume;
    tb_setting *mode;
    tb_setting *name;
} settings_fixture;

static settings_fixture settings_open(void)
{
    settings_fixture made = {.settings = tb_settings_create()};

    tb_settings_add_family(
        made.settings, &(tb_settings_family){.name = "test", .title = "Test", .for_players = true});
    made.vsync = tb_settings_add(made.settings,
                                 &(tb_setting_definition){.name = "test.vsync",
                                                          .type = TB_SETTING_BOOLEAN,
                                                          .boolean = {.default_value = true}});
    made.frame_limit = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "test.frame_limit",
                                 .type = TB_SETTING_INTEGER,
                                 .integer = {.default_value = 60, .minimum = 0, .maximum = 240}});
    made.volume = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "test.volume",
                                 .type = TB_SETTING_NUMBER,
                                 .number = {.default_value = 0.5, .minimum = 0.0, .maximum = 1.0}});
    made.mode = tb_settings_add(
        made.settings, &(tb_setting_definition){
                           .name = "test.mode",
                           .type = TB_SETTING_CHOICE,
                           .choice = {.names = window_modes, .count = 3, .default_value = 1}});
    made.name = tb_settings_add(
        made.settings, &(tb_setting_definition){.name = "test.name",
                                                .type = TB_SETTING_TEXT,
                                                .text = {.default_value = "Badger", .limit = 8}});
    return made;
}

TB_TEST(gui_var, a_bound_variable_and_its_setting_follow_each_other)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    settings_fixture settings = settings_open();
    const tb_var vsync = tb_var_bool(fixture.gui, false);
    const tb_var limit = tb_var_int(fixture.gui, 0);
    const tb_var volume = tb_var_float(fixture.gui, 0.0);
    const tb_var mode = tb_var_int(fixture.gui, 0);
    const tb_var name = tb_var_string(fixture.gui, "");
    seen record = {0};

    /* Each takes its setting's value when bound. */
    TB_TEST_REQUIRE(tb_var_bind_setting(vsync, settings.vsync));
    TB_TEST_REQUIRE(tb_var_bind_setting(limit, settings.frame_limit));
    TB_TEST_REQUIRE(tb_var_bind_setting(volume, settings.volume));
    TB_TEST_REQUIRE(tb_var_bind_setting(mode, settings.mode));
    TB_TEST_REQUIRE(tb_var_bind_setting(name, settings.name));
    TB_TEST_EXPECT(tb_var_get_bool(vsync));
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 60);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.5);
    TB_TEST_EXPECT_INT(tb_var_get_int(mode), ==, 1);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(name), TB_TEXT("Badger")));
    TB_TEST_EXPECT_POINTER(tb_var_setting(volume), ==, settings.volume);

    /* The setting takes the variable's value at once, at the runtime layer. */
    TB_TEST_EXPECT(tb_var_set_bool(vsync, false));
    TB_TEST_EXPECT(!tb_setting_boolean(settings.vsync));
    TB_TEST_EXPECT_UINT(tb_setting_source(settings.vsync), ==, TB_SETTING_LAYER_RUNTIME);
    TB_TEST_EXPECT(tb_var_set_int(limit, 144));
    TB_TEST_EXPECT_INT(tb_setting_integer(settings.frame_limit), ==, 144);
    TB_TEST_EXPECT(tb_var_set_float(volume, 0.75));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(settings.volume), ==, 0.75);
    TB_TEST_EXPECT(tb_var_set_int(mode, 2));
    TB_TEST_EXPECT_UINT(tb_setting_choice(settings.mode), ==, 2);

    /* A value the setting refuses is refused: both keep theirs. */
    TB_TEST_EXPECT(!tb_var_set_int(limit, 1000));
    TB_TEST_EXPECT_INT(tb_var_get_int(limit), ==, 144);
    TB_TEST_EXPECT_INT(tb_setting_integer(settings.frame_limit), ==, 144);
    TB_TEST_EXPECT(!tb_var_set_float(volume, 2.0));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.75);
    TB_TEST_EXPECT(!tb_var_set_int(mode, 3));
    TB_TEST_EXPECT(!tb_var_set_int(mode, -1));
    TB_TEST_EXPECT_INT(tb_var_get_int(mode), ==, 2);

    /* A text variable keeps what it is given; its setting takes what fits. */
    TB_TEST_EXPECT(tb_var_set_text(name, "Ferret"));
    TB_TEST_EXPECT(tb_var_setting_accepts(name));
    TB_TEST_EXPECT(tb_var_set_text(name, "Honey badger"));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(name), TB_TEXT("Honey badger")));
    TB_TEST_EXPECT(!tb_var_setting_accepts(name));
    char buffer[16] = {0};
    (void)tb_setting_text(settings.name, buffer, sizeof(buffer));
    TB_TEST_EXPECT_STRING(buffer, ==, "Ferret");

    /* A change made elsewhere reaches the variable at the next update, and
     * its observers then. */
    TB_TEST_REQUIRE(tb_var_trace(volume, observe, &record));
    TB_TEST_EXPECT_UINT(tb_setting_set_number(settings.volume, 0.25), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.75);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.25);
    TB_TEST_EXPECT_INT(record.count, ==, 1);
    tb_setting_restore_default(settings.name);
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(name), TB_TEXT("Badger")));
    TB_TEST_EXPECT(tb_var_setting_accepts(name));

    /* Unbound, they go their own ways. */
    TB_TEST_EXPECT(tb_var_bind_setting(volume, NULL));
    TB_TEST_EXPECT(tb_var_set_float(volume, 5.0));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(settings.volume), ==, 0.25);
    tb_settings_destroy(settings.settings);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_var, variables_bind_to_the_settings_of_their_kinds)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    settings_fixture settings = settings_open();
    const tb_var flag = tb_var_bool(fixture.gui, false);
    const tb_var count = tb_var_int(fixture.gui, 0);
    const tb_var number = tb_var_float(fixture.gui, 0.0);
    const tb_var text = tb_var_string(fixture.gui, "");

    /* Types that do not fit are refused, and nothing is bound. */
    TB_TEST_EXPECT(!tb_var_bind_setting(flag, settings.frame_limit));
    TB_TEST_EXPECT(!tb_var_bind_setting(count, settings.volume));
    TB_TEST_EXPECT(!tb_var_bind_setting(count, settings.name));
    TB_TEST_EXPECT(!tb_var_bind_setting(number, settings.mode));
    TB_TEST_EXPECT_POINTER(tb_var_setting(flag), ==, NULL);

    /* A number variable on an integer setting is rounded. */
    TB_TEST_REQUIRE(tb_var_bind_setting(number, settings.frame_limit));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(number), ==, 60.0);
    TB_TEST_EXPECT(tb_var_set_float(number, 99.6));
    TB_TEST_EXPECT_INT(tb_setting_integer(settings.frame_limit), ==, 100);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(number), ==, 100.0);

    /* A text variable binds to every kind: a choice by its name, the
     * others as a file writes them. */
    TB_TEST_REQUIRE(tb_var_bind_setting(text, settings.mode));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("borderless")));
    TB_TEST_EXPECT(tb_var_set_text(text, "fullscreen"));
    TB_TEST_EXPECT_UINT(tb_setting_choice(settings.mode), ==, 2);
    TB_TEST_EXPECT(tb_var_set_text(text, "upside_down"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(text));
    TB_TEST_EXPECT_UINT(tb_setting_choice(settings.mode), ==, 2);
    TB_TEST_REQUIRE(tb_var_bind_setting(text, settings.volume));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("0.5")));
    TB_TEST_EXPECT(tb_var_set_text(text, "0.125"));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(settings.volume), ==, 0.125);
    /* Typed halfway, "0.1e" is no number yet: the setting keeps its own. */
    TB_TEST_EXPECT(tb_var_set_text(text, "0.1e"));
    TB_TEST_EXPECT(!tb_var_setting_accepts(text));
    TB_TEST_EXPECT_FLOAT(tb_setting_number(settings.volume), ==, 0.125);
    TB_TEST_REQUIRE(tb_var_bind_setting(text, settings.vsync));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("true")));
    TB_TEST_REQUIRE(tb_var_bind_setting(flag, settings.vsync));
    TB_TEST_EXPECT(tb_var_set_bool(flag, false));
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(text), TB_TEXT("false")));
    tb_settings_destroy(settings.settings);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_var, the_clipboard_holds_what_was_copied)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);

    TB_TEST_EXPECT_UINT(tb_gui_clipboard(fixture.gui).length, ==, 0);
    tb_gui_set_clipboard(fixture.gui, TB_TEXT("copied"));
    TB_TEST_EXPECT(tb_text_equal(tb_gui_clipboard(fixture.gui), TB_TEXT("copied")));
    gui_fixture_close(&fixture);
}
