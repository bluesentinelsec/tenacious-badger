/* Layout files: a widget tree saved as JSON and built from it again, byte
 * for byte; properties of every type, written generically by their kind's
 * table; defaults left out; errors that say where; damaged files that
 * never crash and leave nothing behind. */
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/rng.h"
#include "badger/str.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * A kind with a property of every type, as a game would register.
 * ------------------------------------------------------------------------- */

typedef struct swatch
{
    bool on;
    int32_t count;
    float size;
    tb_vec2 offset;
    tb_vec4 tint;
    tb_gui_string label;
    int32_t shape;
    uint32_t sides;
    tb_font_spec font;
    tb_gui_command command;
    int32_t serial;
} swatch;

static const char *const swatch_shapes[] = {"square", "round", "diamond"};
static const char *const swatch_sides[] = {"top", "bottom", "left", "right"};

static const tb_gui_property swatch_properties[] = {
    {.name = "on", .type = TB_GUI_TYPE_BOOL, .offset = offsetof(swatch, on)},
    {.name = "count", .type = TB_GUI_TYPE_INT, .offset = offsetof(swatch, count)},
    {.name = "size", .type = TB_GUI_TYPE_FLOAT, .offset = offsetof(swatch, size)},
    {.name = "offset", .type = TB_GUI_TYPE_VEC2, .offset = offsetof(swatch, offset)},
    {.name = "tint", .type = TB_GUI_TYPE_COLOR, .offset = offsetof(swatch, tint)},
    {.name = "label", .type = TB_GUI_TYPE_TEXT, .offset = offsetof(swatch, label)},
    {.name = "shape",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(swatch, shape),
     .names = swatch_shapes,
     .name_count = 3},
    {.name = "sides",
     .type = TB_GUI_TYPE_FLAGS,
     .offset = offsetof(swatch, sides),
     .names = swatch_sides,
     .name_count = 4},
    {.name = "font", .type = TB_GUI_TYPE_FONT, .offset = offsetof(swatch, font)},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(swatch, command)},
    {.name = "serial",
     .type = TB_GUI_TYPE_INT,
     .offset = offsetof(swatch, serial),
     .flags = TB_GUI_PROPERTY_READ_ONLY},
};

/* Defaults that are not zero, which a file leaves out all the same. */
static void swatch_init(tb_widget widget, void *data)
{
    swatch *const made = data;

    (void)widget;
    made->size = 1.0f;
    made->shape = 1;
    made->serial = 42;
}

static const tb_widget_class swatch_class = {.name = "swatch",
                                             .data_size = sizeof(swatch),
                                             .properties = swatch_properties,
                                             .property_count = 11,
                                             .init = swatch_init};

static int pings;

static void ping(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)user;
    pings++;
}

/* ---------------------------------------------------------------------------
 * Helpers.
 * ------------------------------------------------------------------------- */

/* A widget saved, as text the caller frees with tb_free(). */
static char *save_text(tb_widget widget)
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

static tb_widget load_text(tb_widget parent, const char *text)
{
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    const tb_gui_file_result result =
        tb_gui_load(parent, text, tb_str_length(text), "test.json", &made, &error);

    TB_TEST_CONTEXT("%s", error.message);
    TB_TEST_REQUIRE_UINT(result, ==, TB_GUI_FILE_OK);
    tb_test_context_clear();
    return made;
}

/* Whether two values of one property are the same. */
static bool values_equal(const tb_gui_value *first, const tb_gui_value *second)
{
    if (first->type != second->type)
    {
        return false;
    }
    if (first->type == TB_GUI_TYPE_BOOL)
    {
        return first->boolean == second->boolean;
    }
    if (first->type == TB_GUI_TYPE_INT || first->type == TB_GUI_TYPE_ENUM)
    {
        return first->integer == second->integer;
    }
    if (first->type == TB_GUI_TYPE_FLAGS)
    {
        return first->flags == second->flags;
    }
    if (first->type == TB_GUI_TYPE_FLOAT)
    {
        return memcmp(&first->number, &second->number, sizeof(float)) == 0;
    }
    if (first->type == TB_GUI_TYPE_VEC2)
    {
        return memcmp(&first->vec2, &second->vec2, sizeof(tb_vec2)) == 0;
    }
    if (first->type == TB_GUI_TYPE_COLOR)
    {
        return memcmp(&first->color, &second->color, sizeof(tb_vec4)) == 0;
    }
    if (first->type == TB_GUI_TYPE_TEXT)
    {
        return tb_text_equal(first->text, second->text);
    }
    if (first->type == TB_GUI_TYPE_FONT)
    {
        return first->font.role == second->font.role && first->font.weight == second->font.weight &&
               first->font.italic == second->font.italic &&
               memcmp(&first->font.size, &second->font.size, sizeof(float)) == 0;
    }
    return first->command.callback == second->command.callback &&
           first->command.user == second->command.user;
}

static size_t widget_count(tb_gui *gui)
{
    tb_gui_stats stats = {0};
    tb_gui_stats_of(gui, &stats);
    return stats.widgets;
}

/* ---------------------------------------------------------------------------
 * Round trips.
 * ------------------------------------------------------------------------- */

/* A file as the engine writes one: every part of a widget, frames nested,
 * rows and columns, a command by name, and a widget left out of the grid. */
static const char canonical[] = "{\n"
                                "  \"format\": \"badger-gui\",\n"
                                "  \"version\": 1,\n"
                                "  \"root\": {\n"
                                "    \"kind\": \"frame\",\n"
                                "    \"name\": \"options\",\n"
                                "    \"properties\": {\n"
                                "      \"padding\": 12,\n"
                                "      \"background\": true,\n"
                                "      \"border\": true\n"
                                "    },\n"
                                "    \"grid\": {\n"
                                "      \"sticky\": [\n"
                                "        \"n\",\n"
                                "        \"s\",\n"
                                "        \"e\",\n"
                                "        \"w\"\n"
                                "      ],\n"
                                "      \"padx\": [\n"
                                "        16,\n"
                                "        0\n"
                                "      ]\n"
                                "    },\n"
                                "    \"columns\": [\n"
                                "      {},\n"
                                "      {\n"
                                "        \"weight\": 1,\n"
                                "        \"minsize\": 120.5\n"
                                "      }\n"
                                "    ],\n"
                                "    \"rows\": [\n"
                                "      {\n"
                                "        \"pad\": 4,\n"
                                "        \"uniform\": \"rows\"\n"
                                "      }\n"
                                "    ],\n"
                                "    \"children\": [\n"
                                "      {\n"
                                "        \"kind\": \"label\",\n"
                                "        \"name\": \"title\",\n"
                                "        \"properties\": {\n"
                                "          \"text\": \"Video\",\n"
                                "          \"wrap_length\": 0.1,\n"
                                "          \"justify\": \"right\",\n"
                                "          \"anchor\": \"w\",\n"
                                "          \"font\": {\n"
                                "            \"role\": \"heading\",\n"
                                "            \"weight\": 700,\n"
                                "            \"size\": 26\n"
                                "          },\n"
                                "          \"cursor\": \"hand\"\n"
                                "        },\n"
                                "        \"grid\": {\n"
                                "          \"column_span\": 2\n"
                                "        }\n"
                                "      },\n"
                                "      {\n"
                                "        \"kind\": \"frame\",\n"
                                "        \"name\": \"video\",\n"
                                "        \"grid\": {\n"
                                "          \"row\": 1\n"
                                "        },\n"
                                "        \"children\": [\n"
                                "          {\n"
                                "            \"kind\": \"button\",\n"
                                "            \"name\": \"apply\",\n"
                                "            \"properties\": {\n"
                                "              \"command\": \"apply_settings\",\n"
                                "              \"text\": \"Apply\",\n"
                                "              \"default\": true,\n"
                                "              \"enabled\": false\n"
                                "            },\n"
                                "            \"grid\": {\n"
                                "              \"column\": 1,\n"
                                "              \"sticky\": [\n"
                                "                \"e\"\n"
                                "              ],\n"
                                "              \"ipadx\": 2.5\n"
                                "            }\n"
                                "          }\n"
                                "        ]\n"
                                "      },\n"
                                "      {\n"
                                "        \"kind\": \"label\",\n"
                                "        \"name\": \"notes\",\n"
                                "        \"properties\": {\n"
                                "          \"visible\": false\n"
                                "        }\n"
                                "      }\n"
                                "    ]\n"
                                "  }\n"
                                "}\n";

/* The editor reads and writes this format: a file written by the engine
 * must come back the same, or every save would churn it. */
TB_TEST(gui_file, a_canonical_file_saves_back_byte_for_byte)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "apply_settings", ping, NULL));
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget options = load_text(root, canonical);
    tb_gui_value value = {0};
    tb_grid_options grid = {0};

    /* What the file says is what the widgets are. */
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_parent(options), root));
    TB_TEST_EXPECT(tb_text_equal(tb_widget_name(options), TB_TEXT("options")));
    TB_TEST_REQUIRE(tb_widget_get(options, "padding", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 12.0f);
    TB_TEST_REQUIRE(tb_grid_info(options, &grid));
    TB_TEST_EXPECT_UINT(grid.sticky, ==, TB_STICKY_NSEW);
    TB_TEST_EXPECT_FLOAT(grid.padx.x, ==, 16.0f);
    const tb_widget apply = tb_widget_find(options, "video/apply");
    TB_TEST_REQUIRE(tb_widget_alive(apply));
    TB_TEST_EXPECT(!tb_widget_enabled(apply));
    TB_TEST_REQUIRE(tb_widget_get(apply, "command", &value));
    TB_TEST_EXPECT(value.command.callback == ping);
    const tb_widget title = tb_widget_find(options, "title");
    TB_TEST_REQUIRE(tb_widget_get(title, "wrap_length", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 0.1f);
    TB_TEST_REQUIRE(tb_widget_get(title, "font", &value));
    TB_TEST_EXPECT_UINT(value.font.role, ==, TB_FONT_ROLE_HEADING);
    TB_TEST_EXPECT_UINT(value.font.weight, ==, 700);
    TB_TEST_EXPECT(!tb_widget_visible(tb_widget_find(options, "notes")));
    TB_TEST_EXPECT(!tb_grid_info(tb_widget_find(options, "notes"), &grid));

    char *const saved = save_text(options);
    TB_TEST_EXPECT_STRING(saved, ==, canonical);
    tb_free(saved);
    gui_fixture_close(&fixture);
}

/* Every type a property may have is written by its kind's table alone,
 * and read back to the same value. */
TB_TEST(gui_file, every_type_of_property_comes_back_the_same)
{
    static const char expected[] = "{\n"
                                   "  \"format\": \"badger-gui\",\n"
                                   "  \"version\": 1,\n"
                                   "  \"root\": {\n"
                                   "    \"kind\": \"swatch\",\n"
                                   "    \"properties\": {\n"
                                   "      \"command\": \"ping\",\n"
                                   "      \"on\": true,\n"
                                   "      \"count\": -7,\n"
                                   "      \"size\": 0.1,\n"
                                   "      \"offset\": [\n"
                                   "        1.5,\n"
                                   "        -2\n"
                                   "      ],\n"
                                   "      \"tint\": [\n"
                                   "        0.2,\n"
                                   "        0.4,\n"
                                   "        1e-07,\n"
                                   "        1\n"
                                   "      ],\n"
                                   "      \"label\": \"Caf\xC3\xA9 \\\"quoted\\\"\",\n"
                                   "      \"shape\": \"diamond\",\n"
                                   "      \"sides\": [\n"
                                   "        \"top\",\n"
                                   "        \"right\"\n"
                                   "      ],\n"
                                   "      \"font\": {\n"
                                   "        \"role\": \"monospace\",\n"
                                   "        \"weight\": 600,\n"
                                   "        \"italic\": true,\n"
                                   "        \"size\": 13.5\n"
                                   "      }\n"
                                   "    }\n"
                                   "  }\n"
                                   "}\n";
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_gui_register_class(fixture.gui, &swatch_class));
    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "ping", ping, &pings));
    const tb_widget made = tb_widget_create(tb_gui_root(fixture.gui), &swatch_class);
    const tb_font_spec font = {
        .role = TB_FONT_ROLE_MONOSPACE, .weight = 600, .italic = true, .size = 13.5f};

    TB_TEST_REQUIRE(
        tb_widget_set(made, "on", &(tb_gui_value){TB_GUI_TYPE_BOOL, {.boolean = true}}));
    TB_TEST_REQUIRE(
        tb_widget_set(made, "count", &(tb_gui_value){TB_GUI_TYPE_INT, {.integer = -7}}));
    TB_TEST_REQUIRE(
        tb_widget_set(made, "size", &(tb_gui_value){TB_GUI_TYPE_FLOAT, {.number = 0.1f}}));
    TB_TEST_REQUIRE(
        tb_widget_set(made, "offset", &(tb_gui_value){TB_GUI_TYPE_VEC2, {.vec2 = {1.5f, -2.0f}}}));
    TB_TEST_REQUIRE(tb_widget_set(
        made, "tint", &(tb_gui_value){TB_GUI_TYPE_COLOR, {.color = {0.2f, 0.4f, 1e-7f, 1.0f}}}));
    TB_TEST_REQUIRE(tb_widget_set(
        made, "label",
        &(tb_gui_value){TB_GUI_TYPE_TEXT, {.text = TB_TEXT("Caf\xC3\xA9 \"quoted\"")}}));
    TB_TEST_REQUIRE(
        tb_widget_set(made, "shape", &(tb_gui_value){TB_GUI_TYPE_ENUM, {.integer = 2}}));
    TB_TEST_REQUIRE(
        tb_widget_set(made, "sides", &(tb_gui_value){TB_GUI_TYPE_FLAGS, {.flags = 0x9u}}));
    TB_TEST_REQUIRE(tb_widget_set(made, "font", &(tb_gui_value){TB_GUI_TYPE_FONT, {.font = font}}));
    TB_TEST_REQUIRE(tb_widget_set(
        made, "command",
        &(tb_gui_value){TB_GUI_TYPE_COMMAND, {.command = {.callback = ping, .user = &pings}}}));

    char *const saved = save_text(made);
    TB_TEST_EXPECT_STRING(saved, ==, expected);

    /* Built again from the text, every property is the same, and the
     * read-only one is the kind's own. */
    const tb_widget again = load_text(tb_gui_root(fixture.gui), saved);
    for (size_t index = 0; index < tb_widget_property_count(made); index++)
    {
        const tb_gui_property *const property = tb_widget_property_at(made, index);
        tb_gui_value first = {0};
        tb_gui_value second = {0};
        TB_TEST_CONTEXT("%s", property->name);
        TB_TEST_REQUIRE(tb_widget_get(made, property->name, &first));
        TB_TEST_REQUIRE(tb_widget_get(again, property->name, &second));
        TB_TEST_EXPECT(values_equal(&first, &second));
    }
    tb_test_context_clear();
    char *const twice = save_text(again);
    TB_TEST_EXPECT_STRING(twice, ==, saved);
    tb_free(twice);
    tb_free(saved);
    gui_fixture_close(&fixture);
}

/* A file holds what differs from a new widget, and nothing else, so that
 * a change to a default reaches every file that did not choose. */
TB_TEST(gui_file, defaults_are_left_out)
{
    static const char bare[] = "{\n"
                               "  \"format\": \"badger-gui\",\n"
                               "  \"version\": 1,\n"
                               "  \"root\": {\n"
                               "    \"kind\": \"swatch\",\n"
                               "    \"grid\": {}\n"
                               "  }\n"
                               "}\n";
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_gui_register_class(fixture.gui, &swatch_class));
    const tb_widget made = tb_widget_create(tb_gui_root(fixture.gui), &swatch_class);

    TB_TEST_REQUIRE(tb_grid(made, NULL));
    char *const saved = save_text(made);
    TB_TEST_EXPECT_STRING(saved, ==, bare);
    tb_free(saved);

    /* A command set in code, not by a name, cannot be written: saving fails
     * and says where, rather than lose it silently. */
    TB_TEST_REQUIRE(tb_widget_set(made, "command",
                                  &(tb_gui_value){TB_GUI_TYPE_COMMAND, {.command = {ping, NULL}}}));
    tb_stream stream = {0};
    tb_gui_file_error error = {0};
    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_EXPECT_UINT(tb_gui_save(made, &stream, &error), ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.path, ==, "$.root.properties.command");
    TB_TEST_EXPECT_STRING(error.reason, ==, "a command with no name, which a file cannot refer to");
    (void)tb_stream_close(&stream);
    gui_fixture_close(&fixture);
}

/* A frame's background makes it stop the mouse: a frame with a background
 * that passes the mouse all the same must say so, after its background. */
TB_TEST(gui_file, a_property_one_setter_changes_comes_back_as_it_was)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget frame =
        tb_frame(tb_gui_root(fixture.gui), &(tb_frame_desc){.background = true});
    tb_gui_value value = {0};

    tb_widget_set_mouse_filter(frame, TB_GUI_MOUSE_PASS);
    char *const saved = save_text(frame);
    TB_TEST_EXPECT(tb_str_find(saved, 0, TB_STR_END,
                               "\"background\": true,\n      \"mouse_filter\": \"pass\"") !=
                   TB_STR_NOT_FOUND);
    const tb_widget again = load_text(tb_gui_root(fixture.gui), saved);
    TB_TEST_REQUIRE(tb_widget_get(again, "mouse_filter", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_MOUSE_PASS);
    tb_free(saved);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Errors.
 * ------------------------------------------------------------------------- */

typedef struct error_case
{
    const char *root; /* the root widget's JSON, inside a file of the format */
    const char *reason;
    const char *path;
} error_case;

/* A file of the format whose root is the case's. */
static char *file_of(const char *root)
{
    return tb_fmt_string(NULL, "{\"format\": \"badger-gui\", \"version\": 1, \"root\": %s}", root);
}

/* An author's mistakes in a file are reported, each at its value, with why. */
TB_TEST(gui_file, a_widget_that_cannot_be_built_says_where_and_why)
{
    static const error_case cases[] = {
        {"[]", "expected an object", "$.root"},
        {"{}", "a widget with no kind", "$.root"},
        {"{\"kind\": 3}", "expected a string", "$.root.kind"},
        {"{\"kind\": \"slider\"}", "unknown kind", "$.root.kind"},
        {"{\"kind\": \"frame\", \"colour\": 1}", "unknown key", "$.root.colour"},
        {"{\"kind\": \"frame\", \"name\": \"a/b\"}", "a name with a slash, which no path finds",
         "$.root.name"},
        {"{\"kind\": \"frame\", \"properties\": []}", "expected an object", "$.root.properties"},
        {"{\"kind\": \"frame\", \"properties\": {\"colour\": 1}}", "unknown property",
         "$.root.properties.colour"},
        {"{\"kind\": \"frame\", \"properties\": {\"padding\": \"wide\"}}", "expected a number",
         "$.root.properties.padding"},
        {"{\"kind\": \"frame\", \"properties\": {\"padding\": -1}}", "a value the property refuses",
         "$.root.properties.padding"},
        {"{\"kind\": \"frame\", \"properties\": {\"padding\": 1e39}}", "a number out of range",
         "$.root.properties.padding"},
        {"{\"kind\": \"frame\", \"properties\": {\"background\": 1}}", "expected true or false",
         "$.root.properties.background"},
        {"{\"kind\": \"frame\", \"properties\": {\"row\": 1}}",
         "a property written elsewhere in a widget", "$.root.properties.row"},
        {"{\"kind\": \"frame\", \"properties\": {\"name\": \"x\"}}",
         "a property written elsewhere in a widget", "$.root.properties.name"},
        {"{\"kind\": \"label\", \"properties\": {\"justify\": \"middle\"}}", "unknown name",
         "$.root.properties.justify"},
        {"{\"kind\": \"label\", \"properties\": {\"font\": 12}}", "expected an object",
         "$.root.properties.font"},
        {"{\"kind\": \"label\", \"properties\": {\"font\": {\"family\": \"x\"}}}", "unknown key",
         "$.root.properties.font.family"},
        {"{\"kind\": \"label\", \"properties\": {\"font\": {\"role\": \"title\"}}}",
         "unknown font role", "$.root.properties.font.role"},
        {"{\"kind\": \"label\", \"properties\": {\"font\": {\"weight\": 70000}}}",
         "an integer out of range", "$.root.properties.font.weight"},
        {"{\"kind\": \"label\", \"properties\": {\"font\": {\"italic\": 0}}}",
         "expected true or false", "$.root.properties.font.italic"},
        {"{\"kind\": \"button\", \"properties\": {\"command\": \"launch\"}}", "unknown command",
         "$.root.properties.command"},
        {"{\"kind\": \"swatch\", \"properties\": {\"serial\": 1}}", "a read-only property",
         "$.root.properties.serial"},
        {"{\"kind\": \"swatch\", \"properties\": {\"count\": 1.5}}", "expected an integer",
         "$.root.properties.count"},
        {"{\"kind\": \"swatch\", \"properties\": {\"count\": 3000000000}}",
         "an integer out of range", "$.root.properties.count"},
        {"{\"kind\": \"swatch\", \"properties\": {\"tint\": [1, 0, 0]}}",
         "expected an array of four numbers", "$.root.properties.tint"},
        {"{\"kind\": \"swatch\", \"properties\": {\"tint\": [1, 0, null, 1]}}", "expected a number",
         "$.root.properties.tint[2]"},
        {"{\"kind\": \"swatch\", \"properties\": {\"sides\": \"top\"}}",
         "expected an array of names", "$.root.properties.sides"},
        {"{\"kind\": \"swatch\", \"properties\": {\"sides\": [\"top\", 2]}}", "expected a string",
         "$.root.properties.sides[1]"},
        {"{\"kind\": \"frame\", \"grid\": []}", "expected an object", "$.root.grid"},
        {"{\"kind\": \"frame\", \"grid\": {\"rowspan\": 2}}", "unknown key", "$.root.grid.rowspan"},
        {"{\"kind\": \"frame\", \"grid\": {\"row\": -1}}", "a value the property refuses",
         "$.root.grid.row"},
        {"{\"kind\": \"frame\", \"grid\": {\"padx\": [1]}}", "expected an array of two numbers",
         "$.root.grid.padx"},
        {"{\"kind\": \"frame\", \"grid\": {\"sticky\": [\"n\", \"x\"]}}", "unknown name",
         "$.root.grid.sticky[1]"},
        {"{\"kind\": \"frame\", \"columns\": {}}", "expected an array", "$.root.columns"},
        {"{\"kind\": \"frame\", \"columns\": [3]}", "expected an object", "$.root.columns[0]"},
        {"{\"kind\": \"frame\", \"columns\": [{\"weight\": -1}]}",
         "a row or column the grid refuses", "$.root.columns[0]"},
        {"{\"kind\": \"frame\", \"rows\": [{}, {\"uniform\": 3}]}", "expected a string",
         "$.root.rows[1].uniform"},
        {"{\"kind\": \"frame\", \"rows\": [{\"span\": 3}]}", "unknown key", "$.root.rows[0].span"},
        {"{\"kind\": \"frame\", \"children\": {}}", "expected an array", "$.root.children"},
        {"{\"kind\": \"frame\", \"children\": [{\"kind\": \"frame\"}, {\"kind\": \"nope\"}]}",
         "unknown kind", "$.root.children[1].kind"},
    };
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_gui_register_class(fixture.gui, &swatch_class));
    const tb_widget root = tb_gui_root(fixture.gui);
    const size_t before = widget_count(fixture.gui);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        char *const text = file_of(cases[index].root);
        tb_widget made = root;
        tb_gui_file_error error = {0};
        TB_TEST_CONTEXT("%s", text);
        TB_TEST_EXPECT_UINT(tb_gui_load(root, text, tb_str_length(text), NULL, &made, &error), ==,
                            TB_GUI_FILE_INVALID);
        TB_TEST_EXPECT_UINT(error.result, ==, TB_GUI_FILE_INVALID);
        TB_TEST_EXPECT_STRING(error.reason, ==, cases[index].reason);
        TB_TEST_EXPECT_STRING(error.path, ==, cases[index].path);
        TB_TEST_EXPECT_UINT(error.line, ==, 1);
        /* Nothing of the file is left behind. */
        TB_TEST_EXPECT(!tb_widget_alive(made));
        TB_TEST_EXPECT_UINT(widget_count(fixture.gui), ==, before);
        tb_str_free(NULL, text);
    }
    tb_test_context_clear();
    gui_fixture_close(&fixture);
}

TB_TEST(gui_file, a_file_that_is_not_a_layout_says_why)
{
    static const error_case cases[] = {
        {"[]", "expected an object", "$"},
        {"{\"format\": \"badger-gui\", \"version\": 1}",
         "a file without its format, version, and root", "$"},
        {"{\"format\": \"badger-ui\", \"version\": 1, \"root\": {}}",
         "not a layout file: another format", "$.format"},
        {"{\"format\": 1, \"version\": 1, \"root\": {}}", "expected a string", "$.format"},
        {"{\"format\": \"badger-gui\", \"version\": 2, \"root\": {}}",
         "a newer version than this engine reads", "$.version"},
        {"{\"format\": \"badger-gui\", \"version\": 0, \"root\": {}}", "an integer out of range",
         "$.version"},
        {"{\"format\": \"badger-gui\", \"version\": 1, \"root\": {}, \"theme\": 1}", "unknown key",
         "$.theme"},
    };
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        const char *const text = cases[index].root;
        tb_widget made = TB_WIDGET_NONE;
        tb_gui_file_error error = {0};
        TB_TEST_CONTEXT("%s", text);
        TB_TEST_EXPECT_UINT(
            tb_gui_load(tb_gui_root(fixture.gui), text, tb_str_length(text), NULL, &made, &error),
            ==, TB_GUI_FILE_INVALID);
        TB_TEST_EXPECT_STRING(error.reason, ==, cases[index].reason);
        TB_TEST_EXPECT_STRING(error.path, ==, cases[index].path);
    }
    tb_test_context_clear();
    gui_fixture_close(&fixture);
}

/* The line and column are those of the value at fault, in a file of many
 * lines, and the message says all of it at once, for a log. */
TB_TEST(gui_file, an_error_is_placed_at_its_line_and_column)
{
    static const char text[] =
        "{\n"
        "  \"format\": \"badger-gui\",\n"
        "  \"version\": 1,\n"
        "  \"root\": {\n"
        "    \"kind\": \"frame\",\n"
        "    \"children\": [{\"kind\": \"label\"}, {\"kind\": \"label\",\n"
        "      \"properties\": {\"text\": \"Hi\", \"colour\": [1, 0, 0, 1]}}]\n"
        "  }\n"
        "}\n";
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};

    TB_TEST_EXPECT_UINT(
        tb_gui_load(tb_gui_root(fixture.gui), text, sizeof(text) - 1, "panel.json", &made, &error),
        ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.path, ==, "$.root.children[1].properties.colour");
    TB_TEST_EXPECT_UINT(error.line, ==, 7);
    TB_TEST_EXPECT_UINT(error.column, ==, 46);
    TB_TEST_EXPECT_STRING(
        error.message, ==,
        "panel.json:7:46: $.root.children[1].properties.colour: unknown property \"colour\"");

    /* Text that is not JSON is placed where the reader stopped. */
    static const char broken[] = "{\n  \"format\": \"badger-gui\",\n  \"version\": 1,,\n}";
    TB_TEST_EXPECT_UINT(
        tb_gui_load(tb_gui_root(fixture.gui), broken, sizeof(broken) - 1, NULL, &made, &error), ==,
        TB_GUI_FILE_MALFORMED);
    TB_TEST_EXPECT_UINT(error.line, ==, 3);
    TB_TEST_EXPECT_STRING(error.path, ==, "");
    TB_TEST_EXPECT(error.reason[0] != '\0');

    /* A parent that is gone builds nothing, and the error may be ignored. */
    const tb_widget gone = tb_frame(tb_gui_root(fixture.gui), NULL);
    tb_widget_destroy(gone);
    TB_TEST_EXPECT_UINT(tb_gui_load(gone, text, sizeof(text) - 1, NULL, &made, NULL), ==,
                        TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT(!tb_widget_alive(made));
    gui_fixture_close(&fixture);
}

/* A file that fails deep inside leaves nothing of what it had built: the
 * parent has the children it had, and no widget more is alive. */
TB_TEST(gui_file, a_file_that_fails_late_leaves_nothing_behind)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget kept = tb_label(root, NULL);
    const size_t before = widget_count(fixture.gui);
    tb_widget made = TB_WIDGET_NONE;
    char *const text = file_of("{\"kind\": \"frame\", \"children\": [{\"kind\": \"frame\", "
                               "\"children\": [{\"kind\": \"label\"}, {\"kind\": \"label\"}]},"
                               " {\"kind\": \"button\", \"properties\": {\"default\": 1}}]}");

    TB_TEST_EXPECT_UINT(tb_gui_load(root, text, tb_str_length(text), NULL, &made, NULL), ==,
                        TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_UINT(widget_count(fixture.gui), ==, before);
    TB_TEST_EXPECT_UINT(tb_widget_child_count(root), ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_first_child(root), kept));
    tb_str_free(NULL, text);
    gui_fixture_close(&fixture);
}

/* Widgets nested deeper than a file is read are refused, both ways. */
TB_TEST(gui_file, widgets_nested_too_deep_are_refused)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_widget deepest = tb_gui_root(fixture.gui);
    tb_widget top = TB_WIDGET_NONE;
    tb_stream stream = {0};
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    char *text = NULL;

    for (int level = 0; level < 65; level++)
    {
        deepest = tb_frame(deepest, NULL);
        if (level == 0)
        {
            top = deepest;
        }
    }
    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_EXPECT_UINT(tb_gui_save(top, &stream, &error), ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "widgets nested too deep");
    (void)tb_stream_close(&stream);

    /* Sixty-four deep is the most; its text, with one more, is refused. */
    tb_widget_destroy(deepest);
    text = save_text(top);
    TB_TEST_EXPECT_UINT(
        tb_gui_load(tb_gui_root(fixture.gui), text, tb_str_length(text), NULL, &made, &error), ==,
        TB_GUI_FILE_OK);
    tb_widget_destroy(made);
    const size_t at = tb_str_find(text, 0, TB_STR_END, "\"kind\": \"frame\"\n");
    TB_TEST_REQUIRE_UINT(at, !=, TB_STR_NOT_FOUND);
    const char *const nested = text + at;
    /* The innermost frame given a child: one level too deep. */
    char *const deeper =
        tb_fmt_string(NULL, "%.*s\"kind\": \"frame\", \"children\": [{\"kind\": \"frame\"}]%s",
                      (int)(nested - text), text, nested + tb_str_length("\"kind\": \"frame\""));
    TB_TEST_EXPECT_UINT(
        tb_gui_load(tb_gui_root(fixture.gui), deeper, tb_str_length(deeper), NULL, &made, &error),
        ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "widgets nested too deep");
    tb_str_free(NULL, deeper);
    tb_free(text);
    gui_fixture_close(&fixture);
}

static tb_stream_result refusing_write(void *state, const void *bytes, size_t length,
                                       size_t *transferred)
{
    (void)state;
    (void)bytes;
    (void)length;
    *transferred = 0;
    return TB_STREAM_NO_SPACE;
}

static tb_stream_result nothing_to_release(void *state)
{
    (void)state;
    return TB_STREAM_OK;
}

/* A stream that refuses, text that is not UTF-8, and a stale widget are
 * failures of saving, each its own. */
TB_TEST(gui_file, saving_fails_when_it_cannot_write)
{
    static const tb_stream_functions refusing_functions = {.write_bytes = refusing_write,
                                                           .release = nothing_to_release};
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "Hi"});
    tb_stream refusing = {&refusing_functions, NULL};
    tb_stream buffer = {0};

    tb_gui_file_error error = {0};
    TB_TEST_EXPECT_UINT(tb_gui_save(label, &refusing, &error), ==, TB_GUI_FILE_OUTPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the stream refused what was written");

    TB_TEST_REQUIRE(tb_widget_set_text(label, "\xFF\xFE"));
    tb_stream_open_buffer(NULL, &buffer);
    TB_TEST_EXPECT_UINT(tb_gui_save(label, &buffer, &error), ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "text that is not UTF-8");
    (void)tb_stream_close(&buffer);

    tb_widget_destroy(label);
    tb_stream_open_buffer(NULL, &buffer);
    TB_TEST_EXPECT_UINT(tb_gui_save(label, &buffer, &error), ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the widget is not a widget alive");
    (void)tb_stream_close(&buffer);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Damaged input.
 * ------------------------------------------------------------------------- */

/* Mods and downloads reach the loader: whatever a file's bytes, loading
 * never crashes, and a file that fails leaves no widget behind. The
 * canonical file is damaged at random, a byte changed, a run removed or
 * repeated, or the end cut off, from a seed fixed for repeatability. */
TB_TEST(gui_file, damaged_files_never_crash_and_leave_nothing)
{
    enum
    {
        damaged_files = 3000
    };
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "apply_settings", ping, NULL));
    const tb_widget root = tb_gui_root(fixture.gui);
    const size_t length = sizeof(canonical) - 1;
    const size_t before = widget_count(fixture.gui);
    char *const text = tb_malloc(length * 2);
    tb_rng rng = {0};
    size_t results[TB_GUI_FILE_OUTPUT_FAILED + 1] = {0};

    tb_rng_seed(&rng, 0x5eed);
    for (int file = 0; file < damaged_files; file++)
    {
        size_t used = length;
        const size_t at = (size_t)tb_rng_below(&rng, length);
        const size_t run = 1 + (size_t)tb_rng_below(&rng, 16);
        tb_widget made = TB_WIDGET_NONE;
        tb_gui_file_error error = {0};
        memcpy(text, canonical, length);
        switch (tb_rng_below(&rng, 4))
        {
        case 0:
            text[at] = (char)tb_rng_below(&rng, 256);
            break;
        case 1:
            used = at;
            break;
        case 2: {
            const size_t taken = at + run <= length ? run : length - at;
            memmove(text + at, text + at + taken, length - at - taken);
            used = length - taken;
            break;
        }
        default: {
            const size_t taken = at + run <= length ? run : length - at;
            memmove(text + at + taken, text + at, length - at);
            used = length + taken;
            break;
        }
        }
        const tb_gui_file_result result = tb_gui_load(root, text, used, NULL, &made, &error);
        TB_TEST_REQUIRE_UINT(result, <=, TB_GUI_FILE_INVALID);
        results[result]++;
        if (result == TB_GUI_FILE_OK)
        {
            tb_widget_destroy(made);
        }
        else
        {
            TB_TEST_REQUIRE(!tb_widget_alive(made));
            TB_TEST_REQUIRE(error.message[0] != '\0');
        }
        TB_TEST_REQUIRE_UINT(widget_count(fixture.gui), ==, before);
    }
    /* The damage reached each kind of outcome. */
    TB_TEST_EXPECT_UINT(results[TB_GUI_FILE_OK], >, 0);
    TB_TEST_EXPECT_UINT(results[TB_GUI_FILE_MALFORMED], >, 0);
    TB_TEST_EXPECT_UINT(results[TB_GUI_FILE_INVALID], >, 0);
    tb_free(text);
    gui_fixture_close(&fixture);
}
