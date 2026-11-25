/* Layout files and the kinds that edit values: variables named in a file
 * and declared by it, bound to settings, and every registered kind saved
 * and loaded again with each of its properties changed. */
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

/* A widget saved, as text the caller frees with tb_free(). */
static char *saved_text(tb_widget widget)
{
    tb_stream stream = {0};
    size_t length = 0;
    const unsigned char *bytes = NULL;
    char *text = NULL;

    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_gui_save(widget, &stream), ==, TB_GUI_FILE_OK);
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

static tb_gui_file_result load(tb_widget parent, const char *text, tb_widget *made,
                               tb_gui_file_error *error)
{
    return tb_gui_load(parent, text, tb_str_length(text), NULL, made, error);
}

static const char declared[] = "{\n"
                               "  \"format\": \"badger-gui\",\n"
                               "  \"version\": 1,\n"
                               "  \"variables\": [\n"
                               "    {\n"
                               "      \"name\": \"fov\",\n"
                               "      \"type\": \"float\",\n"
                               "      \"value\": 90.5\n"
                               "    },\n"
                               "    {\n"
                               "      \"name\": \"volume\",\n"
                               "      \"type\": \"float\",\n"
                               "      \"setting\": \"test.volume\"\n"
                               "    },\n"
                               "    {\n"
                               "      \"name\": \"player\",\n"
                               "      \"type\": \"string\",\n"
                               "      \"value\": \"Badger\"\n"
                               "    }\n"
                               "  ],\n"
                               "  \"root\": {\n"
                               "    \"kind\": \"frame\",\n"
                               "    \"children\": [\n"
                               "      {\n"
                               "        \"kind\": \"scale\",\n"
                               "        \"name\": \"fov\",\n"
                               "        \"properties\": {\n"
                               "          \"variable\": \"fov\"\n"
                               "        }\n"
                               "      },\n"
                               "      {\n"
                               "        \"kind\": \"scale\",\n"
                               "        \"name\": \"volume\",\n"
                               "        \"properties\": {\n"
                               "          \"variable\": \"volume\"\n"
                               "        }\n"
                               "      },\n"
                               "      {\n"
                               "        \"kind\": \"entry\",\n"
                               "        \"properties\": {\n"
                               "          \"variable\": \"player\"\n"
                               "        }\n"
                               "      },\n"
                               "      {\n"
                               "        \"kind\": \"spinbox\",\n"
                               "        \"properties\": {\n"
                               "          \"variable\": \"fov\"\n"
                               "        }\n"
                               "      }\n"
                               "    ]\n"
                               "  }\n"
                               "}\n";

static tb_settings *volume_settings(void)
{
    tb_settings *const settings = tb_settings_create();

    tb_settings_add_family(
        settings, &(tb_settings_family){.name = "test", .title = "Test", .for_players = true});
    (void)tb_settings_add(settings,
                          &(tb_setting_definition){
                              .name = "test.volume",
                              .type = TB_SETTING_NUMBER,
                              .number = {.default_value = 0.25, .minimum = 0.0, .maximum = 1.0}});
    (void)tb_settings_add(settings, &(tb_setting_definition){.name = "test.vsync",
                                                             .type = TB_SETTING_BOOLEAN,
                                                             .boolean = {.default_value = true}});
    return settings;
}

/* A file declares the variables its widgets edit, with their first values
 * or the settings they follow, and saves back to the same text. */
TB_TEST(gui_file_kinds, a_file_declares_and_names_its_variables)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_settings *const settings = volume_settings();
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    tb_gui_value value = {0};

    tb_gui_set_settings(fixture.gui, settings);
    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(load(tb_gui_root(fixture.gui), declared, &made, &error), ==,
                         TB_GUI_FILE_OK);
    tb_test_context_clear();
    const tb_var fov = tb_gui_find_var(fixture.gui, "fov");
    const tb_var volume = tb_gui_find_var(fixture.gui, "volume");
    TB_TEST_REQUIRE(tb_var_alive(fov));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 90.5);
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(volume), ==, 0.25);
    TB_TEST_EXPECT(tb_var_setting(volume) == tb_settings_find(settings, "test.volume"));
    TB_TEST_EXPECT(
        tb_text_equal(tb_var_get_text(tb_gui_find_var(fixture.gui, "player")), TB_TEXT("Badger")));
    TB_TEST_REQUIRE(tb_widget_get(tb_widget_find(made, "fov"), "variable", &value));
    TB_TEST_EXPECT(tb_var_equal(value.variable, fov));

    char *const saved = saved_text(made);
    TB_TEST_EXPECT_STRING(saved, ==, declared);
    tb_free(saved);

    /* Loaded again, a variable of the name is used as it is, not made anew. */
    TB_TEST_REQUIRE(tb_var_set_float(fov, 75.0));
    tb_widget again = TB_WIDGET_NONE;
    TB_TEST_REQUIRE_UINT(load(tb_gui_root(fixture.gui), declared, &again, &error), ==,
                         TB_GUI_FILE_OK);
    TB_TEST_REQUIRE(tb_widget_get(tb_widget_find(again, "fov"), "variable", &value));
    TB_TEST_EXPECT(tb_var_equal(value.variable, fov));
    TB_TEST_EXPECT_FLOAT(tb_var_get_float(fov), ==, 75.0);
    gui_fixture_close(&fixture);
    tb_settings_destroy(settings);
}

/* A variable a game makes and names is found by a file that names it. */
TB_TEST(gui_file_kinds, a_file_names_a_variable_the_game_made)
{
    static const char text[] = "{\"format\": \"badger-gui\", \"version\": 1, \"root\": "
                               "{\"kind\": \"checkbutton\", \"properties\": "
                               "{\"variable\": \"vsync\"}}}";
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var vsync = tb_var_bool(fixture.gui, true);
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    tb_gui_value value = {0};

    TB_TEST_EXPECT_UINT(load(tb_gui_root(fixture.gui), text, &made, &error), ==,
                        TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "unknown variable");
    TB_TEST_EXPECT_STRING(error.path, ==, "$.root.properties.variable");
    tb_var_set_name(vsync, "vsync");
    TB_TEST_REQUIRE_UINT(load(tb_gui_root(fixture.gui), text, &made, &error), ==, TB_GUI_FILE_OK);
    TB_TEST_REQUIRE(tb_widget_get(made, "variable", &value));
    TB_TEST_EXPECT(tb_var_equal(value.variable, vsync));

    /* Saved, the variable is declared with its value, and named. */
    char *const saved = saved_text(made);
    TB_TEST_EXPECT(tb_str_find(saved, 0, TB_STR_END,
                               "\"name\": \"vsync\",\n      \"type\": \"bool\",\n      \"value\": "
                               "true") != TB_STR_NOT_FOUND);
    TB_TEST_EXPECT(tb_str_find(saved, 0, TB_STR_END, "\"variable\": \"vsync\"") !=
                   TB_STR_NOT_FOUND);
    tb_free(saved);

    /* A variable with no name, or a name another holds first, cannot be
     * written, and is left out. */
    const tb_var unnamed = tb_var_bool(fixture.gui, false);
    TB_TEST_REQUIRE(tb_widget_set(made, "variable",
                                  &(tb_gui_value){TB_GUI_TYPE_VARIABLE, {.variable = unnamed}}));
    char *const bare = saved_text(made);
    TB_TEST_EXPECT(tb_str_find(bare, 0, TB_STR_END, "variable") == TB_STR_NOT_FOUND);
    tb_free(bare);
    gui_fixture_close(&fixture);
}

typedef struct declaration_case
{
    const char *variables;
    const char *reason;
    const char *path;
} declaration_case;

/* A declaration that cannot be made says where, and a file that fails
 * leaves none of the variables it made. */
TB_TEST(gui_file_kinds, a_declaration_that_fails_says_where_and_leaves_nothing)
{
    static const declaration_case cases[] = {
        {"{}", "expected an array", "$.variables"},
        {"[3]", "expected an object", "$.variables[0]"},
        {"[{\"name\": \"a\"}]", "a variable without its name and type", "$.variables[0]"},
        {"[{\"name\": \"a\", \"type\": \"int\", \"colour\": 1}]", "unknown key",
         "$.variables[0].colour"},
        {"[{\"name\": \"\", \"type\": \"int\"}]", "a variable with an empty name",
         "$.variables[0].name"},
        {"[{\"name\": 1, \"type\": \"int\"}]", "expected a string", "$.variables[0].name"},
        {"[{\"name\": \"a\", \"type\": \"double\"}]", "unknown variable type",
         "$.variables[0].type"},
        {"[{\"name\": \"a\", \"type\": \"int\", \"value\": 1.5}]", "expected an integer",
         "$.variables[0].value"},
        {"[{\"name\": \"a\", \"type\": \"bool\", \"value\": 1}]", "expected true or false",
         "$.variables[0].value"},
        {"[{\"name\": \"a\", \"type\": \"float\", \"value\": \"1\"}]", "expected a number",
         "$.variables[0].value"},
        {"[{\"name\": \"a\", \"type\": \"string\", \"value\": 1}]", "expected a string",
         "$.variables[0].value"},
        {"[{\"name\": \"a\", \"type\": \"float\", \"setting\": \"test.missing\"}]",
         "unknown setting", "$.variables[0].setting"},
        {"[{\"name\": \"a\", \"type\": \"string\", \"setting\": 3}]", "expected a string",
         "$.variables[0].setting"},
        {"[{\"name\": \"a\", \"type\": \"bool\", \"setting\": \"test.volume\"}]",
         "a setting the variable's type does not bind to", "$.variables[0].setting"},
        {"[{\"name\": \"taken\", \"type\": \"int\"}]", "a variable of another type by this name",
         "$.variables[0].type"},
        /* Made, then undone when the root fails. */
        {"[{\"name\": \"a\", \"type\": \"int\"}, {\"name\": \"b\", \"type\": \"bool\"}]",
         "unknown kind", "$.root.kind"},
    };
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_settings *const settings = volume_settings();
    tb_var_set_name(tb_var_bool(fixture.gui, false), "taken");

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        const bool last = index + 1 == TB_TEST_COUNT_OF(cases);
        char *const text = tb_fmt_string(NULL,
                                         "{\"format\": \"badger-gui\", \"version\": 1, "
                                         "\"variables\": %s, \"root\": {\"kind\": \"%s\"}}",
                                         cases[index].variables, last ? "nope" : "frame");
        tb_widget made = TB_WIDGET_NONE;
        tb_gui_file_error error = {0};
        tb_gui_set_settings(fixture.gui, settings);
        TB_TEST_CONTEXT("%s", text);
        TB_TEST_EXPECT_UINT(load(tb_gui_root(fixture.gui), text, &made, &error), ==,
                            TB_GUI_FILE_INVALID);
        TB_TEST_EXPECT_STRING(error.reason, ==, cases[index].reason);
        TB_TEST_EXPECT_STRING(error.path, ==, cases[index].path);
        TB_TEST_EXPECT(!tb_var_alive(tb_gui_find_var(fixture.gui, "a")));
        TB_TEST_EXPECT(!tb_var_alive(tb_gui_find_var(fixture.gui, "b")));
        tb_str_free(NULL, text);
    }
    tb_test_context_clear();

    /* A setting, and no settings to find it in. */
    static const char unset[] = "{\"format\": \"badger-gui\", \"version\": 1, \"variables\": "
                                "[{\"name\": \"a\", \"type\": \"float\", \"setting\": "
                                "\"test.volume\"}], \"root\": {\"kind\": \"frame\"}}";
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    tb_gui_set_settings(fixture.gui, NULL);
    TB_TEST_EXPECT_UINT(load(tb_gui_root(fixture.gui), unset, &made, &error), ==,
                        TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "a setting, and no settings given the GUI");
    gui_fixture_close(&fixture);
    tb_settings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 * Every kind registered.
 * ------------------------------------------------------------------------- */

static void ignore(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)user;
}

/* Another value of a property's type than the one it holds; false for a
 * type that has none to give. */
static bool changed_value(tb_gui *gui, const tb_gui_property *property, const tb_gui_value *now,
                          size_t attempt, tb_gui_value *changed)
{
    static const char *const vars[] = {"var_bool", "var_int", "var_float", "var_text"};

    *changed = *now;
    if (property->type == TB_GUI_TYPE_BOOL)
    {
        changed->boolean = !now->boolean;
    }
    else if (property->type == TB_GUI_TYPE_INT)
    {
        changed->integer = now->integer + 1 + (int32_t)attempt;
    }
    else if (property->type == TB_GUI_TYPE_ENUM)
    {
        changed->integer =
            property->name_count > 0
                ? (int32_t)(((size_t)now->integer + 1 + attempt) % property->name_count)
                : now->integer + 1;
    }
    else if (property->type == TB_GUI_TYPE_FLAGS)
    {
        changed->flags = now->flags ^ (1u << (attempt % 4));
    }
    else if (property->type == TB_GUI_TYPE_FLOAT)
    {
        changed->number = now->number + 2.5f + (float)attempt;
    }
    else if (property->type == TB_GUI_TYPE_VEC2)
    {
        changed->vec2 = (tb_vec2){now->vec2.x + 1.5f, now->vec2.y + 3.0f};
    }
    else if (property->type == TB_GUI_TYPE_COLOR)
    {
        changed->color = (tb_vec4){0.25f, 0.5f, 0.75f, 1.0f};
    }
    else if (property->type == TB_GUI_TYPE_TEXT)
    {
        changed->text = attempt == 0 ? TB_TEXT("Changed") : TB_TEXT("7");
    }
    else if (property->type == TB_GUI_TYPE_FONT)
    {
        changed->font = (tb_font_spec){.role = TB_FONT_ROLE_BODY, .weight = 600, .size = 17.0f};
    }
    else if (property->type == TB_GUI_TYPE_COMMAND)
    {
        tb_gui_command command = {0};
        if (!tb_gui_find_command(gui, "command", &command))
        {
            return false;
        }
        changed->command = command;
    }
    else if (property->type == TB_GUI_TYPE_VARIABLE)
    {
        if (attempt >= 4)
        {
            return false;
        }
        changed->variable = tb_gui_find_var(gui, vars[attempt]);
    }
    else
    {
        return false;
    }
    return attempt < 4;
}

/* Every kind the GUI registers, the engine's own and those added after
 * this test, is made, each property it takes changed, and saved: built
 * again from its text, it saves to the same text. */
TB_TEST(gui_file_kinds, every_registered_kind_comes_back_the_same)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_gui *const gui = fixture.gui;
    const tb_widget root = tb_gui_root(gui);

    TB_TEST_REQUIRE(tb_gui_register_command(gui, "command", ignore, NULL));
    tb_var_set_name(tb_var_bool(gui, true), "var_bool");
    tb_var_set_name(tb_var_int(gui, 3), "var_int");
    tb_var_set_name(tb_var_float(gui, 0.5), "var_float");
    tb_var_set_name(tb_var_string(gui, "Text"), "var_text");
    TB_TEST_REQUIRE_UINT(tb_gui_class_count(gui), >=, 11);
    for (size_t kind_index = 0; kind_index < tb_gui_class_count(gui); kind_index++)
    {
        const tb_widget_class *const kind = tb_gui_class_at(gui, kind_index);
        const tb_widget made = tb_widget_create(root, kind);
        size_t changed_count = 0;
        TB_TEST_REQUIRE(tb_grid(made, &(tb_grid_options){.row = 1, .sticky = TB_STICKY_EW}));
        /* A common property changed on every kind, those with none of their own too. */
        tb_widget_set_style(made, "changed");
        for (size_t index = 0; index < kind->property_count; index++)
        {
            const tb_gui_property *const property = &kind->properties[index];
            tb_gui_value now = {0};
            if ((property->flags & TB_GUI_PROPERTY_READ_ONLY) != 0 ||
                !tb_widget_get(made, property->name, &now))
            {
                continue;
            }
            for (size_t attempt = 0; attempt < 4; attempt++)
            {
                tb_gui_value changed = {0};
                if (changed_value(gui, property, &now, attempt, &changed) &&
                    tb_widget_set(made, property->name, &changed))
                {
                    changed_count++;
                    break;
                }
            }
        }
        gui_update(&fixture);
        TB_TEST_CONTEXT("kind %s", kind->name);
        char *const first = saved_text(made);
        tb_widget again = TB_WIDGET_NONE;
        tb_gui_file_error error = {0};
        TB_TEST_CONTEXT("kind %s: %s", kind->name, first);
        TB_TEST_REQUIRE_UINT(load(root, first, &again, &error), ==, TB_GUI_FILE_OK);
        gui_update(&fixture);
        char *const second = saved_text(again);
        TB_TEST_EXPECT_STRING(second, ==, first);
        if (kind->property_count > 0)
        {
            TB_TEST_EXPECT_UINT(changed_count, >, 0);
        }
        tb_free(second);
        tb_free(first);
        tb_widget_destroy(again);
        tb_widget_destroy(made);
    }
    tb_test_context_clear();
    gui_fixture_close(&fixture);
}
