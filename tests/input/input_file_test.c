/* Input files must make the actions and contexts they describe, all or
 * nothing; refuse what is not JSON or not definitions, saying where and
 * why; and be written back so that reading and writing again changes
 * nothing, byte for byte. */
#include "badger/input.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/stream.h"
#include "badger/vfs.h"

#include "tb_test.h"

#include <stddef.h>
#include <string.h>

#define INPUT_PATH "input/play.json"

static const char play_file[] =
    "{\n"
    "  \"format\": \"badger-input\",\n"
    "  \"version\": 1,\n"
    "  \"actions\": [\n"
    "    {\"name\": \"move\", \"type\": \"axis2d\", \"accumulation\": \"cumulative\"},\n"
    "    {\"name\": \"look\", \"type\": \"axis2d\",\n"
    "     \"modifiers\": [{\"type\": \"settings\", \"source\": \"mouse\"}]},\n"
    "    {\"name\": \"jump\", \"type\": \"bool\",\n"
    "     \"triggers\": [{\"type\": \"chord_blocker\", \"action\": \"crouch\"}]},\n"
    "    {\"name\": \"crouch\", \"type\": \"bool\"},\n"
    "    {\"name\": \"select\", \"type\": \"bool\"}\n"
    "  ],\n"
    "  \"contexts\": [\n"
    "    {\"name\": \"gameplay\", \"mappings\": [\n"
    "      {\"action\": \"move\", \"key\": \"w\", \"modifiers\": [{\"type\": \"swizzle\", "
    "\"order\": \"yxz\"}]},\n"
    "      {\"action\": \"move\", \"key\": \"s\", \"modifiers\": [{\"type\": \"swizzle\", "
    "\"order\": \"yxz\"}, {\"type\": \"negate\"}]},\n"
    "      {\"action\": \"move\", \"key\": \"a\", \"modifiers\": [{\"type\": \"negate\", "
    "\"axes\": [\"x\"]}]},\n"
    "      {\"action\": \"move\", \"key\": \"d\"},\n"
    "      {\"action\": \"look\", \"key\": \"mouse_xy\"},\n"
    "      {\"action\": \"jump\", \"key\": \"space\", \"triggers\": [{\"type\": \"pressed\"}]},\n"
    "      {\"action\": \"crouch\", \"key\": \"left_control\", \"pass_through\": true}\n"
    "    ]},\n"
    "    {\"name\": \"menu\",\n"
    "     \"actions\": [{\"action\": \"select\", \"triggers\": [{\"type\": \"tap\", \"seconds\": "
    "0.25}]}],\n"
    "     \"mappings\": [{\"action\": \"select\", \"key\": \"enter\"}]}\n"
    "  ]\n"
    "}\n";

/* Every kind of modifier and trigger, with every field it writes. */
static const char every_rule_file[] =
    "{\"format\": \"badger-input\", \"version\": 1, \"actions\": [\n"
    " {\"name\": \"a\", \"type\": \"axis3d\", \"modifiers\": [\n"
    "  {\"type\": \"dead_zone\", \"kind\": \"axial\", \"lower\": 0.1, \"upper\": 0.9},\n"
    "  {\"type\": \"dead_zone\", \"kind\": \"unscaled_radial\"},\n"
    "  {\"type\": \"negate\", \"axes\": [\"y\", \"z\"]},\n"
    "  {\"type\": \"swizzle\", \"order\": \"zxy\"},\n"
    "  {\"type\": \"scalar\", \"scale\": [1.5, -2, 0.1]},\n"
    "  {\"type\": \"scalar\", \"scale\": 3},\n"
    "  {\"type\": \"response_curve\", \"exponent\": [2, 3, 1]},\n"
    "  {\"type\": \"smooth\", \"samples\": 3},\n"
    "  {\"type\": \"smooth\"},\n"
    "  {\"type\": \"settings\", \"source\": \"right_stick\"}],\n"
    " \"triggers\": [\n"
    "  {\"type\": \"down\", \"threshold\": 0.25},\n"
    "  {\"type\": \"released\"},\n"
    "  {\"type\": \"hold\", \"seconds\": 0.75, \"one_shot\": true},\n"
    "  {\"type\": \"hold_and_release\", \"seconds\": 2},\n"
    "  {\"type\": \"tap\"},\n"
    "  {\"type\": \"pulse\", \"seconds\": 0.5, \"on_start\": true, \"limit\": 4},\n"
    "  {\"type\": \"chorded\", \"action\": \"b\"}]},\n"
    " {\"name\": \"b\", \"type\": \"axis1d\"}]}\n";

static tb_input_file_result load(tb_input *input, const char *text, tb_input_file_error *error)
{
    return tb_input_load(input, text, tb_str_length(text), "play.json", error);
}

static bool contains(const char *text, const char *pattern)
{
    return tb_str_find(text, 0, TB_STR_END, pattern) != TB_STR_NOT_FOUND;
}

/* The input's definitions as a file; the caller frees it. */
static char *save(const tb_input *input)
{
    tb_stream stream = {0};
    size_t length = 0;

    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_input_save(input, &stream, NULL), ==, TB_INPUT_FILE_OK);
    const unsigned char *const bytes = tb_stream_buffer_bytes(&stream, &length);
    char *const text = tb_malloc(length + 1);
    memcpy(text, bytes, length);
    text[length] = '\0';
    (void)tb_stream_close(&stream);
    return text;
}

/* The file's actions, contexts and mappings are made, and behave: WASD
 * moves, the blocked jump, the passed-through crouch, the menu's tap. */
TB_TEST(input_file, a_file_makes_working_definitions)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    tb_input_file_error error = {0};

    TB_TEST_REQUIRE_UINT(load(input, play_file, &error), ==, TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    TB_TEST_EXPECT_UINT(tb_input_action_count(input), ==, 5);
    TB_TEST_EXPECT_UINT(tb_input_context_count(input), ==, 2);
    const tb_input_action move = tb_input_find_action(input, "move");
    const tb_input_action jump = tb_input_find_action(input, "jump");
    const tb_input_action select = tb_input_find_action(input, "select");
    const tb_input_context gameplay = tb_input_find_context(input, "gameplay");
    const tb_input_context menu = tb_input_find_context(input, "menu");
    TB_TEST_EXPECT_INT(tb_input_action_type(input, move), ==, TB_INPUT_VALUE_AXIS2D);
    TB_TEST_EXPECT_UINT(tb_input_context_mapping_count(input, gameplay), ==, 7);

    tb_input_add_context(player, gameplay, 0);
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_S, (tb_input_value){1, 0, 0});
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_A, (tb_input_value){1, 0, 0});
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_SPACE,
                      (tb_input_value){1, 0, 0});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_action_value(player, move).x, ==, -1.0f);
    TB_TEST_EXPECT_FLOAT(tb_input_action_value(player, move).y, ==, -1.0f);
    TB_TEST_EXPECT(tb_input_action_triggered(player, jump));

    tb_input_add_context(player, menu, 1);
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ENTER,
                      (tb_input_value){1, 0, 0});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_UINT(tb_input_action_events(player, select), ==, TB_INPUT_EVENT_STARTED);
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_ENTER, (tb_input_value){0});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT(tb_input_action_triggered(player, select));
    tb_input_destroy(input);
}

/* A file written and read back writes the same bytes again; a file of
 * every rule keeps every field. */
TB_TEST(input_file, files_round_trip)
{
    const char *const files[] = {play_file, every_rule_file};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(files); index++)
    {
        tb_input *const first = tb_input_create(NULL);
        tb_input *const second = tb_input_create(NULL);
        tb_input_file_error error = {0};
        TB_TEST_CONTEXT("file %zu", index);
        TB_TEST_REQUIRE_UINT(load(first, files[index], &error), ==, TB_INPUT_FILE_OK);
        char *const written = save(first);
        TB_TEST_REQUIRE_UINT(load(second, written, &error), ==, TB_INPUT_FILE_OK);
        char *const again = save(second);
        TB_TEST_EXPECT_STRING(again, ==, written);
        TB_TEST_EXPECT(tb_str_starts_with(written, "{\n  \"format\": \"badger-input\",\n"));
        tb_free(written);
        tb_free(again);
        tb_input_destroy(first);
        tb_input_destroy(second);
    }
}

/* What is written is the fewest digits of each number, and leaves out
 * what is the default. */
TB_TEST(input_file, saving_writes_short_numbers_and_no_defaults)
{
    tb_input *const input = tb_input_create(NULL);

    TB_TEST_REQUIRE_UINT(load(input, every_rule_file, NULL), ==, TB_INPUT_FILE_OK);
    char *const written = save(input);
    TB_TEST_EXPECT(contains(written, "\"lower\": 0.1,"));
    TB_TEST_EXPECT(contains(written, "\"scale\": 3\n"));
    TB_TEST_EXPECT(contains(written, "\"scale\": [\n"));
    TB_TEST_EXPECT(contains(written, "\"seconds\": 0.75,"));
    TB_TEST_EXPECT(contains(written, "\"seconds\": 2.0\n"));
    TB_TEST_EXPECT(!contains(written, "\"accumulation\""));
    TB_TEST_EXPECT(!contains(written, "\"threshold\": 0\n"));
    tb_free(written);
    tb_input_destroy(input);
}

/* Definitions made in code are written, and read back, the same. */
TB_TEST(input_file, definitions_made_in_code_round_trip)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input *const copy = tb_input_create(NULL);
    const tb_input_action fire =
        tb_input_create_action(input, &(tb_input_action_desc){.name = "fire"});
    const tb_input_context context = tb_input_create_context(input, "empty");
    const tb_input_trigger blocker = {.type = TB_INPUT_TRIGGER_CHORD_BLOCKER, .chord = fire};
    const tb_input_action aim = tb_input_create_action(
        input, &(tb_input_action_desc){.name = "aim", .triggers = &blocker, .trigger_count = 1});

    tb_input_context_set_action_rules(input, context, &(tb_input_action_rules_desc){.action = aim});
    char *const written = save(input);
    TB_TEST_REQUIRE_UINT(load(copy, written, NULL), ==, TB_INPUT_FILE_OK);
    char *const again = save(copy);
    TB_TEST_EXPECT_STRING(again, ==, written);
    tb_free(written);
    tb_free(again);
    tb_input_destroy(input);
    tb_input_destroy(copy);
}

/* Text that is not JSON is malformed, with its line and column. */
TB_TEST(input_file, text_that_is_not_json_is_malformed)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_file_error error = {0};

    TB_TEST_EXPECT_UINT(load(input, "{\n  \"format\": }", &error), ==, TB_INPUT_FILE_MALFORMED);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_INPUT_FILE_MALFORMED);
    TB_TEST_EXPECT_UINT(error.line, ==, 2);
    TB_TEST_EXPECT(tb_str_starts_with(error.message, "play.json:2:"));
    TB_TEST_EXPECT_UINT(tb_input_load(input, "", 0, NULL, NULL), ==, TB_INPUT_FILE_MALFORMED);
    tb_input_destroy(input);
}

/* Each way a file can be JSON but not definitions is refused, with the
 * JSON path of the value at fault and a reason. */
TB_TEST(input_file, invalid_definitions_are_refused_where_they_are)
{
#define HEAD "{\"format\": \"badger-input\", \"version\": 1, "
#define ACTION "\"actions\": [{\"name\": \"a\", \"type\": \"bool\"}], "
#define MAPPING(rules)                                                                             \
    "\"contexts\": [{\"name\": \"c\", \"mappings\": [{\"action\": \"a\", \"key\": \"w\", " rules   \
    "}]}]}"
    static const struct
    {
        const char *text;
        const char *path;
        const char *reason;
    } cases[] = {
        {"[]", "$", "input definitions: an object"},
        {HEAD "\"actions\\u0000old\": []}", "$.actions", "a key the format does not have"},
        {"{\"format\": \"badger-input\\u0000other\", \"version\": 1}", "$.format",
         "a format other than \"badger-input\""},
        {"{\"format\": \"badger-gui\", \"version\": 1}", "$.format",
         "a format other than \"badger-input\""},
        {"{\"version\": 1}", "$.format", "a format other than \"badger-input\""},
        {"{\"format\": \"badger-input\"}", "$.version", "a version: a whole number from 1"},
        {"{\"format\": \"badger-input\", \"version\": 0}", "$.version",
         "a version: a whole number from 1"},
        {"{\"format\": \"badger-input\", \"version\": 2}", "$.version",
         "a newer version than this engine reads"},
        {HEAD "\"colour\": 1}", "$.colour", "a key the format does not have"},
        {HEAD "\"actions\": {}}", "$.actions", "an array"},
        {HEAD "\"actions\": [1]}", "$.actions[0]", "an action: an object"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\", \"kind\": 1}]}",
         "$.actions[0].kind", "a key the format does not have"},
        {HEAD "\"actions\": [{\"type\": \"bool\"}]}", "$.actions[0].name",
         "a name: lowercase letters, digits and \"_\", at most 64 bytes"},
        {HEAD "\"actions\": [{\"name\": \"Jump\", \"type\": \"bool\"}]}", "$.actions[0].name",
         "a name: lowercase letters, digits and \"_\", at most 64 bytes"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\"}, {\"name\": \"a\", \"type\": "
              "\"bool\"}]}",
         "$.actions[1].name", "a name an action already has"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"vec2\"}]}", "$.actions[0].type",
         "a value type: bool, axis1d, axis2d or axis3d"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\", \"accumulation\": \"sum\"}]}",
         "$.actions[0].accumulation", "an accumulation: highest or cumulative"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\", \"modifiers\": 1}]}",
         "$.actions[0].modifiers", "an array"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\", \"triggers\": {}}]}",
         "$.actions[0].triggers", "an array"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\", \"modifiers\": [1]}]}",
         "$.actions[0].modifiers[0]", "a modifier: an object"},
        {HEAD "\"actions\": [{\"name\": \"a\", \"type\": \"bool\", \"triggers\": [1]}]}",
         "$.actions[0].triggers[0]", "a trigger: an object"},
        {HEAD "\"contexts\": {}}", "$.contexts", "an array"},
        {HEAD "\"contexts\": [[]]}", "$.contexts[0]", "a context: an object"},
        {HEAD "\"contexts\": [{\"name\": \"c\", \"maps\": []}]}", "$.contexts[0].maps",
         "a key the format does not have"},
        {HEAD "\"contexts\": [{\"name\": 1}]}", "$.contexts[0].name",
         "a name: lowercase letters, digits and \"_\", at most 64 bytes"},
        {HEAD "\"contexts\": [{\"name\": \"c\"}, {\"name\": \"c\"}]}", "$.contexts[1].name",
         "a name a context already has"},
        {HEAD "\"contexts\": [{\"name\": \"c\", \"mappings\": [1]}]}", "$.contexts[0].mappings[0]",
         "a mapping: an object"},
        {HEAD "\"contexts\": [{\"name\": \"c\", \"mappings\": [{\"key\": \"w\"}]}]}",
         "$.contexts[0].mappings[0].action", "an action: its name"},
        {HEAD "\"contexts\": [{\"name\": \"c\", \"mappings\": [{\"action\": \"b\", \"key\": "
              "\"w\"}]}]}",
         "$.contexts[0].mappings[0].action", "an unknown action"},
        {HEAD ACTION "\"contexts\": [{\"name\": \"c\", \"mappings\": [{\"action\": \"a\", "
                     "\"key\": \"spcae\"}]}]}",
         "$.contexts[0].mappings[0].key", "an unknown key"},
        {HEAD ACTION "\"contexts\": [{\"name\": \"c\", \"mappings\": [{\"action\": \"a\"}]}]}",
         "$.contexts[0].mappings[0].key", "an unknown key"},
        {HEAD ACTION MAPPING("\"pass_through\": 1"), "$.contexts[0].mappings[0].pass_through",
         "true or false"},
        {HEAD ACTION "\"contexts\": [{\"name\": \"c\", \"actions\": [1]}]}",
         "$.contexts[0].actions[0]", "an action's rules: an object"},
        {HEAD ACTION "\"contexts\": [{\"name\": \"c\", \"actions\": [{\"action\": \"a\", "
                     "\"keys\": 1}]}]}",
         "$.contexts[0].actions[0].keys", "a key the format does not have"},
        {HEAD ACTION "\"contexts\": [{\"name\": \"c\", \"actions\": [{\"action\": \"z\"}]}]}",
         "$.contexts[0].actions[0].action", "an unknown action"},
        {HEAD ACTION "\"contexts\": [{\"name\": \"c\", \"actions\": [{\"action\": \"a\", "
                     "\"triggers\": [{\"type\": \"tap\", \"seconds\": -1}]}]}]}",
         "$.contexts[0].actions[0].triggers[0]", "seconds: a finite number, not negative"},
        /* Modifiers. */
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"flip\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].type",
         "a modifier's type: dead_zone, negate, swizzle, scalar, response_curve, smooth or "
         "settings"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"negate\", \"order\": \"yxz\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].order", "a key the format does not have"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"dead_zone\", \"kind\": \"round\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].kind",
         "a dead zone's kind: radial, axial or unscaled_radial"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"dead_zone\", \"lower\": \"x\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].lower", "a number"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"dead_zone\", \"upper\": 1e300}]"),
         "$.contexts[0].mappings[0].modifiers[0].upper", "a number"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"dead_zone\", \"lower\": 0.5, "
                             "\"upper\": 0.25}]"),
         "$.contexts[0].mappings[0].modifiers[0]",
         "a dead zone whose upper threshold is not above its lower"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"negate\", \"axes\": [\"w\"]}]"),
         "$.contexts[0].mappings[0].modifiers[0].axes", "axes: an array of \"x\", \"y\" and \"z\""},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"negate\", \"axes\": []}]"),
         "$.contexts[0].mappings[0].modifiers[0].axes", "axes: an array of \"x\", \"y\" and \"z\""},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"swizzle\", \"order\": \"xyz\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].order", "an order: yxz, zyx, xzy, yzx or zxy"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"scalar\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].scale", "a scale: a number, or an array of three"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"scalar\", \"scale\": [1, 2]}]"),
         "$.contexts[0].mappings[0].modifiers[0].scale", "a scale: a number, or an array of three"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"scalar\", \"scale\": [1, 2, \"3\"]}]"),
         "$.contexts[0].mappings[0].modifiers[0].scale", "a scale: a number, or an array of three"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"scalar\", \"scale\": 1e300}]"),
         "$.contexts[0].mappings[0].modifiers[0].scale", "a scale: a number, or an array of three"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"response_curve\", \"exponent\": "
                             "true}]"),
         "$.contexts[0].mappings[0].modifiers[0].exponent",
         "an exponent: a number, or an array of three"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"response_curve\", \"exponent\": "
                             "-1}]"),
         "$.contexts[0].mappings[0].modifiers[0]", "an exponent: finite numbers, not negative"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"smooth\", \"samples\": -1}]"),
         "$.contexts[0].mappings[0].modifiers[0].samples", "a whole number"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"smooth\", \"samples\": 5000000000}]"),
         "$.contexts[0].mappings[0].modifiers[0].samples", "a whole number"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"smooth\", \"samples\": 9}]"),
         "$.contexts[0].mappings[0].modifiers[0]", "a smoothing's samples: from 1 to 8"},
        {HEAD ACTION MAPPING("\"modifiers\": [{\"type\": \"settings\"}]"),
         "$.contexts[0].mappings[0].modifiers[0].source",
         "a source: mouse, gamepad, touch, left_stick or right_stick"},
        /* Triggers. */
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": 3}]"),
         "$.contexts[0].mappings[0].triggers[0].type",
         "a trigger's type: down, pressed, released, hold, hold_and_release, tap, pulse, "
         "chorded or chord_blocker"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"down\", \"seconds\": 1}]"),
         "$.contexts[0].mappings[0].triggers[0].seconds", "a key the format does not have"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"down\", \"threshold\": null}]"),
         "$.contexts[0].mappings[0].triggers[0].threshold", "a number"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"down\", \"threshold\": -1}]"),
         "$.contexts[0].mappings[0].triggers[0]", "a threshold: a finite number, not negative"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"hold\", \"seconds\": \"1\"}]"),
         "$.contexts[0].mappings[0].triggers[0].seconds", "a number"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"hold\", \"one_shot\": 1}]"),
         "$.contexts[0].mappings[0].triggers[0].one_shot", "true or false"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"pulse\", \"on_start\": \"yes\"}]"),
         "$.contexts[0].mappings[0].triggers[0].on_start", "true or false"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"pulse\", \"limit\": 1.5}]"),
         "$.contexts[0].mappings[0].triggers[0].limit", "a whole number"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"chorded\"}]"),
         "$.contexts[0].mappings[0].triggers[0].action", "a chord's action: its name"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"chorded\", \"action\": \"q\"}]"),
         "$.contexts[0].mappings[0].triggers[0].action", "an unknown action"},
        {HEAD ACTION MAPPING("\"triggers\": [{\"type\": \"chorded\", \"action\": \"a\"}]"),
         "$.contexts[0].mappings[0].triggers[0]", "a chord of its own action"},
    };
#undef HEAD
#undef ACTION
#undef MAPPING

    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        tb_input *const input = tb_input_create(NULL);
        tb_input_file_error error = {0};
        TB_TEST_CONTEXT("case %zu: %s", index, cases[index].text);
        TB_TEST_EXPECT_UINT(load(input, cases[index].text, &error), ==, TB_INPUT_FILE_INVALID);
        TB_TEST_EXPECT_UINT(error.result, ==, TB_INPUT_FILE_INVALID);
        TB_TEST_EXPECT_STRING(error.path, ==, cases[index].path);
        TB_TEST_EXPECT_STRING(error.reason, ==, cases[index].reason);
        TB_TEST_EXPECT_UINT(error.line, ==, 1);
        TB_TEST_EXPECT(tb_str_starts_with(error.message, "play.json:1:"));
        /* All or nothing: the input is as it was. */
        TB_TEST_EXPECT_UINT(tb_input_action_count(input), ==, 0);
        TB_TEST_EXPECT_UINT(tb_input_context_count(input), ==, 0);
        tb_input_destroy(input);
    }
}

/* A message names the file, the place, the path, the reason, and what was
 * named; a failed file leaves what was loaded before, and the names it
 * made are free again. */
TB_TEST(input_file, failures_say_where_and_change_nothing)
{
    tb_input *const input = tb_input_create(NULL);
    tb_input_player *const player = tb_input_player_at(input, 0);
    tb_input_file_error error = {0};
    static const char broken[] = "{\"format\": \"badger-input\", \"version\": 1,\n"
                                 " \"actions\": [{\"name\": \"extra\", \"type\": \"bool\"}],\n"
                                 " \"contexts\": [{\"name\": \"more\", \"mappings\": [\n"
                                 "  {\"action\": \"extra\", \"key\": \"spcae\"}]}]}";

    TB_TEST_REQUIRE_UINT(load(input, play_file, NULL), ==, TB_INPUT_FILE_OK);
    tb_input_add_context(player, tb_input_find_context(input, "gameplay"), 0);
    TB_TEST_EXPECT_UINT(load(input, broken, &error), ==, TB_INPUT_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.message, ==,
                          "play.json:4:30: $.contexts[0].mappings[0].key: an unknown key "
                          "\"spcae\"");
    TB_TEST_EXPECT_UINT(error.line, ==, 4);
    TB_TEST_EXPECT_UINT(error.column, ==, 30);
    TB_TEST_EXPECT_UINT(tb_input_action_count(input), ==, 5);
    TB_TEST_EXPECT_UINT(tb_input_context_count(input), ==, 2);
    TB_TEST_EXPECT_UINT(tb_input_find_action(input, "extra").id, ==, 0);
    /* The player's actions are as they were, and work. */
    tb_input_feed_key(input, TB_INPUT_DEVICE_KEYBOARD, TB_INPUT_KEY_W, (tb_input_value){1, 0, 0});
    tb_input_tick(input, 0.1);
    TB_TEST_EXPECT_FLOAT(tb_input_action_value(player, tb_input_find_action(input, "move")).y, ==,
                         1.0f);
    /* A second file may name actions of the first. */
    static const char more[] = "{\"format\": \"badger-input\", \"version\": 1, \"contexts\": "
                               "[{\"name\": \"vehicle\", \"mappings\": [{\"action\": \"move\", "
                               "\"key\": \"gamepad_left_stick\"}]}]}";
    TB_TEST_EXPECT_UINT(load(input, more, &error), ==, TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_context_count(input), ==, 3);
    tb_input_destroy(input);
}

/* ---------------------------------------------------------------------------
 * Through a VFS, and a stream that refuses.
 * ------------------------------------------------------------------------- */

typedef struct memory_file
{
    const char *bytes;
    size_t length;
    bool missing;
    bool failing;
} memory_file;

static bool memory_is_file(const char *name, size_t length)
{
    return length == tb_str_length(INPUT_PATH) && memcmp(name, INPUT_PATH, length) == 0;
}

static tb_vfs_result memory_open(void *state, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream)
{
    memory_file *const file = state;

    if (file->missing || !memory_is_file(name, length))
    {
        return TB_VFS_NOT_FOUND;
    }
    if (file->failing)
    {
        return TB_VFS_INPUT_FAILED;
    }
    tb_stream_open_memory(file->bytes, file->length, allocator, stream);
    return TB_VFS_OK;
}

static tb_vfs_result memory_query(void *state, const char *name, size_t length, tb_vfs_info *info)
{
    const memory_file *const file = state;

    if (file->missing || !memory_is_file(name, length))
    {
        return TB_VFS_NOT_FOUND;
    }
    *info = (tb_vfs_info){.kind = TB_VFS_FILE, .size = file->length};
    return TB_VFS_OK;
}

static tb_vfs_result memory_list(void *state, const char *directory, size_t length,
                                 tb_vfs_list_function *function, void *context)
{
    (void)state;
    (void)directory;
    (void)length;
    (void)function;
    (void)context;
    return TB_VFS_NOT_FOUND;
}

static void memory_release(void *state)
{
    (void)state;
}

static const tb_vfs_source_functions memory_functions = {memory_open, memory_query, memory_list,
                                                         memory_release};

/* A file is read through a VFS; one missing, or that fails, says so. */
TB_TEST(input_file, files_load_through_a_vfs)
{
    memory_file file = {.bytes = play_file, .length = sizeof(play_file) - 1};
    tb_vfs *const vfs = tb_vfs_create(NULL);
    tb_input *const input = tb_input_create(NULL);
    tb_input_file_error error = {0};

    TB_TEST_REQUIRE_UINT(tb_vfs_mount(vfs, "", 0, (tb_vfs_source){&memory_functions, &file}, 0), ==,
                         TB_VFS_OK);
    TB_TEST_REQUIRE_UINT(tb_input_load_file(input, vfs, INPUT_PATH, &error), ==, TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_action_count(input), ==, 5);
    TB_TEST_EXPECT_UINT(tb_input_load_file(input, vfs, INPUT_PATH, &error), ==,
                        TB_INPUT_FILE_INVALID);
    TB_TEST_EXPECT(tb_str_starts_with(error.message, INPUT_PATH ":5:"));
    file.missing = true;
    TB_TEST_EXPECT_UINT(tb_input_load_file(input, vfs, INPUT_PATH, &error), ==,
                        TB_INPUT_FILE_NOT_FOUND);
    TB_TEST_EXPECT_STRING(error.message, ==, INPUT_PATH ": no such file");
    file.missing = false;
    file.failing = true;
    TB_TEST_EXPECT_UINT(tb_input_load_file(input, vfs, INPUT_PATH, NULL), ==,
                        TB_INPUT_FILE_INPUT_FAILED);
    tb_input_destroy(input);
    tb_vfs_destroy(vfs);
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

/* A stream that refuses what is written fails the save. */
TB_TEST(input_file, saving_fails_when_the_stream_refuses)
{
    static const tb_stream_functions refusing_functions = {.write_bytes = refusing_write,
                                                           .release = nothing_to_release};
    tb_stream refusing = {&refusing_functions, NULL};
    tb_input *const input = tb_input_create(NULL);
    tb_input_file_error error = {0};

    TB_TEST_REQUIRE_UINT(load(input, play_file, NULL), ==, TB_INPUT_FILE_OK);
    TB_TEST_EXPECT_UINT(tb_input_save(input, &refusing, &error), ==, TB_INPUT_FILE_OUTPUT_FAILED);
    TB_TEST_EXPECT_STRING(error.reason, ==, "the stream refused what was written");
    TB_TEST_EXPECT_UINT(tb_input_save(input, &refusing, NULL), ==, TB_INPUT_FILE_OUTPUT_FAILED);
    tb_input_destroy(input);
}
