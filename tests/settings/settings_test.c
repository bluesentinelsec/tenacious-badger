/* Settings must give each setting the value of its highest layer, accept only
 * values that fit, tell their readers when a value changes, read and write
 * the file format faithfully, keep what they do not understand, and restore
 * defaults, from any thread. */
#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/fs.h"
#include "badger/memory.h"
#include "badger/settings.h"
#include "badger/str.h"
#include "badger/thread.h"

#include "tb_test.h"

#include <stdint.h>
#include <string.h>

static tb_path_view view(const char *text)
{
    return (tb_path_view){text, tb_str_length(text)};
}

/* A registry with one family for players, one not, and a setting of every
 * type. */
typedef struct fixture
{
    tb_settings *settings;
    tb_setting *fullscreen;  /* boolean, false */
    tb_setting *frame_limit; /* integer, 0 from 0 to 1000 */
    tb_setting *volume;      /* number, 0.8 from 0 to 1 */
    tb_setting *quality;     /* choice, high of low, medium, high */
    tb_setting *language;    /* text, "" up to 8 bytes */
    tb_setting *tick_rate;   /* integer, 60, in a family not for players */
} fixture;

static const char *const quality_names[] = {"low", "medium", "high"};

static fixture make_fixture(void)
{
    fixture made = {0};

    made.settings = tb_settings_create();
    tb_settings_add_family(
        made.settings,
        &(tb_settings_family){.name = "video", .title = "Video", .for_players = true});
    tb_settings_add_family(made.settings, &(tb_settings_family){.name = "engine"});
    made.fullscreen =
        tb_settings_add(made.settings, &(tb_setting_definition){.name = "video.fullscreen",
                                                                .description = "Fill the display.",
                                                                .type = TB_SETTING_BOOLEAN});
    made.frame_limit = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "video.frame_limit",
                                 .type = TB_SETTING_INTEGER,
                                 .integer = {.default_value = 0, .minimum = 0, .maximum = 1000}});
    made.volume = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "video.sound.volume",
                                 .type = TB_SETTING_NUMBER,
                                 .number = {.default_value = 0.8, .minimum = 0.0, .maximum = 1.0}});
    made.quality = tb_settings_add(
        made.settings, &(tb_setting_definition){
                           .name = "video.quality",
                           .type = TB_SETTING_CHOICE,
                           .choice = {.names = quality_names, .count = 3, .default_value = 2}});
    made.language =
        tb_settings_add(made.settings, &(tb_setting_definition){.name = "video.language",
                                                                .type = TB_SETTING_TEXT,
                                                                .text = {.limit = 8}});
    made.tick_rate = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "engine.tick_rate",
                                 .type = TB_SETTING_INTEGER,
                                 .integer = {.default_value = 60, .minimum = 1, .maximum = 1000}});
    return made;
}

static void expect_format(const tb_setting *setting, tb_setting_layer layer, const char *expected)
{
    char text[128] = {0};

    (void)tb_setting_format(setting, layer, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, expected);
}

/* ---------------------------------------------------------------------------
 * What a registry holds.
 * ------------------------------------------------------------------------- */

TB_TEST(settings_registry, settings_start_at_their_built_in_values)
{
    fixture made = make_fixture();
    char text[16] = {0};

    TB_TEST_EXPECT(!tb_setting_boolean(made.fullscreen));
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 0);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.8);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.quality), ==, 2);
    TB_TEST_EXPECT_UINT(tb_setting_text(made.language, text, sizeof(text)), ==, 0);
    TB_TEST_EXPECT_STRING(text, ==, "");
    TB_TEST_EXPECT_UINT(tb_setting_source(made.volume), ==, TB_SETTING_LAYER_BUILT_IN);
    TB_TEST_EXPECT(tb_setting_has_layer(made.volume, TB_SETTING_LAYER_BUILT_IN));
    TB_TEST_EXPECT(!tb_setting_has_layer(made.volume, TB_SETTING_LAYER_RUNTIME));
    TB_TEST_EXPECT_UINT(tb_setting_version(made.volume), ==, 0);
    TB_TEST_EXPECT_UINT(tb_settings_version(made.settings), ==, 0);
    tb_settings_destroy(made.settings);
    tb_settings_destroy(NULL);
}

TB_TEST(settings_registry, settings_and_families_describe_themselves_in_the_order_added)
{
    fixture made = make_fixture();
    tb_settings_family family = {0};
    int64_t integer_minimum = 0;
    int64_t integer_maximum = 0;
    double number_minimum = 0.0;
    double number_maximum = 0.0;

    TB_TEST_EXPECT_UINT(tb_settings_family_count(made.settings), ==, 2);
    family = tb_settings_family_at(made.settings, 0);
    TB_TEST_EXPECT_STRING(family.name, ==, "video");
    TB_TEST_EXPECT_STRING(family.title, ==, "Video");
    TB_TEST_EXPECT(family.for_players);
    family = tb_settings_family_at(made.settings, 1);
    TB_TEST_EXPECT_STRING(family.title, ==, "engine"); /* the name, when there is no title */
    TB_TEST_EXPECT_POINTER(family.description, ==, NULL);
    TB_TEST_EXPECT(!family.for_players);

    TB_TEST_EXPECT_UINT(tb_settings_count(made.settings), ==, 6);
    TB_TEST_EXPECT_POINTER(tb_settings_at(made.settings, 0), ==, made.fullscreen);
    TB_TEST_EXPECT_POINTER(tb_settings_at(made.settings, 5), ==, made.tick_rate);
    TB_TEST_EXPECT_POINTER(tb_settings_find(made.settings, "video.sound.volume"), ==, made.volume);
    TB_TEST_EXPECT_POINTER(tb_settings_find(made.settings, "video.volume"), ==, NULL);
    TB_TEST_EXPECT_STRING(tb_setting_name(made.fullscreen), ==, "video.fullscreen");
    TB_TEST_EXPECT_STRING(tb_setting_description(made.fullscreen), ==, "Fill the display.");
    TB_TEST_EXPECT_POINTER(tb_setting_description(made.volume), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_setting_type_of(made.quality), ==, TB_SETTING_CHOICE);
    TB_TEST_EXPECT_UINT(tb_setting_family(made.tick_rate), ==, 1);
    TB_TEST_EXPECT(tb_setting_for_players(made.volume));
    TB_TEST_EXPECT(!tb_setting_for_players(made.tick_rate));

    tb_setting_integer_range(made.frame_limit, &integer_minimum, &integer_maximum);
    TB_TEST_EXPECT_INT(integer_minimum, ==, 0);
    TB_TEST_EXPECT_INT(integer_maximum, ==, 1000);
    tb_setting_number_range(made.volume, &number_minimum, &number_maximum);
    TB_TEST_EXPECT_FLOAT(number_minimum, ==, 0.0);
    TB_TEST_EXPECT_FLOAT(number_maximum, ==, 1.0);
    TB_TEST_EXPECT_UINT(tb_setting_choice_count(made.quality), ==, 3);
    TB_TEST_EXPECT_STRING(tb_setting_choice_name(made.quality, 1), ==, "medium");
    TB_TEST_EXPECT_UINT(tb_setting_find_choice(made.quality, "MEDIUM"), ==, 1);
    TB_TEST_EXPECT_UINT(tb_setting_find_choice(made.quality, "ultra"), ==, 3);
    TB_TEST_EXPECT_UINT(tb_setting_text_limit(made.language), ==, 8);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_registry, a_text_setting_without_a_limit_has_the_default_limit)
{
    tb_settings *const settings = tb_settings_create();
    tb_setting *text = NULL;

    tb_settings_add_family(settings, &(tb_settings_family){.name = "audio"});
    text =
        tb_settings_add(settings, &(tb_setting_definition){.name = "audio.device",
                                                           .type = TB_SETTING_TEXT,
                                                           .text = {.default_value = "Speakers"}});
    TB_TEST_EXPECT_UINT(tb_setting_text_limit(text), ==, TB_SETTING_DEFAULT_TEXT_LIMIT);
    expect_format(text, TB_SETTING_LAYER_COUNT, "\"Speakers\"");
    tb_settings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 * Layers.
 * ------------------------------------------------------------------------- */

/* Each layer, set from the lowest to the highest, overrides those below it;
 * and clearing them from the highest gives back each layer's value in turn. */
TB_TEST(settings_layers, the_highest_layer_with_a_value_gives_the_setting_its_value)
{
    fixture made = make_fixture();
    static const char *const values[TB_SETTING_LAYER_COUNT] = {"", "10", "20", "30", "40"};

    for (unsigned layer = TB_SETTING_LAYER_DEFAULT_FILE; layer < TB_SETTING_LAYER_COUNT; layer++)
    {
        TB_TEST_CONTEXT("layer %s", tb_setting_layer_name((tb_setting_layer)layer));
        TB_TEST_EXPECT_UINT(tb_setting_parse(made.frame_limit, (tb_setting_layer)layer,
                                             values[layer], tb_str_length(values[layer])),
                            ==, TB_SETTING_OK);
        TB_TEST_EXPECT_UINT(tb_setting_source(made.frame_limit), ==, layer);
        TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, (int64_t)layer * 10);
    }
    for (unsigned layer = TB_SETTING_LAYER_RUNTIME; layer > TB_SETTING_LAYER_BUILT_IN; layer--)
    {
        TB_TEST_CONTEXT("clearing layer %s", tb_setting_layer_name((tb_setting_layer)layer));
        tb_setting_clear(made.frame_limit, (tb_setting_layer)layer);
        TB_TEST_EXPECT_UINT(tb_setting_source(made.frame_limit), ==, layer - 1);
        TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, (int64_t)(layer - 1) * 10);
    }
    /* Clearing a layer that has no value changes nothing. */
    tb_setting_clear(made.frame_limit, TB_SETTING_LAYER_RUNTIME);
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 0);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_layers, a_change_at_run_time_sets_the_runtime_layer)
{
    fixture made = make_fixture();
    char text[16] = {0};

    TB_TEST_EXPECT_UINT(tb_setting_set_boolean(made.fullscreen, true), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_setting_set_integer(made.frame_limit, 144), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_setting_set_number(made.volume, 0.25), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_setting_set_choice(made.quality, 0), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_setting_set_text(made.language, "fr-FR"), ==, TB_SETTING_OK);

    TB_TEST_EXPECT(tb_setting_boolean(made.fullscreen));
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 144);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.25);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.quality), ==, 0);
    TB_TEST_EXPECT_UINT(tb_setting_text(made.language, text, sizeof(text)), ==, 5);
    TB_TEST_EXPECT_STRING(text, ==, "fr-FR");
    /* A buffer too small gets nothing. */
    memset(text, 'x', sizeof(text));
    TB_TEST_EXPECT_UINT(tb_setting_text(made.language, text, 5), ==, 5);
    TB_TEST_EXPECT_UINT((unsigned char)text[0], ==, 'x');
    TB_TEST_EXPECT_UINT(tb_setting_source(made.language), ==, TB_SETTING_LAYER_RUNTIME);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_layers, values_that_do_not_fit_change_nothing)
{
    fixture made = make_fixture();
    const double zero = tb_setting_number(made.volume) - 0.8;
    const double not_a_number = zero / zero;
    const uint64_t version = tb_settings_version(made.settings);

    TB_TEST_EXPECT_UINT(tb_setting_set_integer(made.frame_limit, -1), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(tb_setting_set_integer(made.frame_limit, 1001), ==,
                        TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(tb_setting_set_number(made.volume, 1.5), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(tb_setting_set_number(made.volume, not_a_number), ==,
                        TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(tb_setting_set_choice(made.quality, 3), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(tb_setting_set_text(made.language, "123456789"), ==,
                        TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(tb_setting_set_text(made.language, "\xC3\x28"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(tb_settings_version(made.settings), ==, version);
    TB_TEST_EXPECT_UINT(tb_setting_source(made.volume), ==, TB_SETTING_LAYER_BUILT_IN);
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Versions.
 * ------------------------------------------------------------------------- */

TB_TEST(settings_versions, a_version_moves_when_and_only_when_the_value_changes)
{
    fixture made = make_fixture();

    (void)tb_setting_set_number(made.volume, 0.5);
    TB_TEST_EXPECT_UINT(tb_setting_version(made.volume), ==, 1);
    TB_TEST_EXPECT_UINT(tb_settings_version(made.settings), ==, 1);
    /* The same value again is no change. */
    (void)tb_setting_set_number(made.volume, 0.5);
    TB_TEST_EXPECT_UINT(tb_setting_version(made.volume), ==, 1);
    /* A layer below the runtime's is hidden by it. */
    (void)tb_setting_parse(made.volume, TB_SETTING_LAYER_PLAYER_FILE, "0.1", 3);
    TB_TEST_EXPECT_UINT(tb_setting_version(made.volume), ==, 1);
    /* Removing the runtime value reveals the player's. */
    tb_setting_clear(made.volume, TB_SETTING_LAYER_RUNTIME);
    TB_TEST_EXPECT_UINT(tb_setting_version(made.volume), ==, 2);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.1);
    /* Another setting moves the registry's version, not this one's. */
    (void)tb_setting_set_boolean(made.fullscreen, true);
    TB_TEST_EXPECT_UINT(tb_setting_version(made.volume), ==, 2);
    TB_TEST_EXPECT_UINT(tb_settings_version(made.settings), ==, 3);
    /* Text compares by its characters. */
    (void)tb_setting_set_text(made.language, "");
    TB_TEST_EXPECT_UINT(tb_setting_version(made.language), ==, 0);
    (void)tb_setting_set_text(made.language, "de");
    TB_TEST_EXPECT_UINT(tb_setting_version(made.language), ==, 1);
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Values as text.
 * ------------------------------------------------------------------------- */

static tb_setting_result parse(tb_setting *setting, const char *text)
{
    return tb_setting_parse(setting, TB_SETTING_LAYER_COMMAND_LINE, text, tb_str_length(text));
}

TB_TEST(settings_text, booleans_read_the_usual_words_in_any_case)
{
    fixture made = make_fixture();
    static const char *const yes[] = {"true", "TRUE", "on", "Yes", "1", "\"true\""};
    static const char *const no[] = {"false", "Off", "no", "0"};

    for (size_t index = 0; index < TB_TEST_COUNT_OF(yes); index++)
    {
        TB_TEST_CONTEXT("'%s'", yes[index]);
        (void)tb_setting_set_boolean(made.fullscreen, false);
        tb_setting_clear(made.fullscreen, TB_SETTING_LAYER_RUNTIME);
        TB_TEST_EXPECT_UINT(parse(made.fullscreen, yes[index]), ==, TB_SETTING_OK);
        TB_TEST_EXPECT(tb_setting_boolean(made.fullscreen));
    }
    for (size_t index = 0; index < TB_TEST_COUNT_OF(no); index++)
    {
        TB_TEST_CONTEXT("'%s'", no[index]);
        (void)parse(made.fullscreen, "true");
        TB_TEST_EXPECT_UINT(parse(made.fullscreen, no[index]), ==, TB_SETTING_OK);
        TB_TEST_EXPECT(!tb_setting_boolean(made.fullscreen));
    }
    TB_TEST_EXPECT_UINT(parse(made.fullscreen, "maybe"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(made.fullscreen, ""), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(made.fullscreen, "truer"), ==, TB_SETTING_INVALID);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_text, integers_are_decimal_and_every_64_bit_value_reads_exactly)
{
    tb_settings *const settings = tb_settings_create();
    tb_setting *wide = NULL;

    tb_settings_add_family(settings, &(tb_settings_family){.name = "test"});
    wide = tb_settings_add(settings, &(tb_setting_definition){
                                         .name = "test.wide",
                                         .type = TB_SETTING_INTEGER,
                                         .integer = {.minimum = INT64_MIN, .maximum = INT64_MAX}});
    TB_TEST_EXPECT_UINT(parse(wide, "9223372036854775807"), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(wide), ==, INT64_MAX);
    TB_TEST_EXPECT_UINT(parse(wide, "-9223372036854775808"), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(wide), ==, INT64_MIN);
    TB_TEST_EXPECT_UINT(parse(wide, "+17"), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(wide), ==, 17);
    TB_TEST_EXPECT_UINT(parse(wide, "-0"), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(wide), ==, 0);
    expect_format(wide, TB_SETTING_LAYER_COUNT, "0");
    TB_TEST_EXPECT_UINT(parse(wide, "9223372036854775808"), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(parse(wide, "-9223372036854775809"), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(parse(wide, "99999999999999999999999"), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(parse(wide, "-"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(wide, "1.5"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(wide, "0x10"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(wide, " 1"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_INT(tb_setting_integer(wide), ==, 0);
    tb_settings_destroy(settings);
}

TB_TEST(settings_text, numbers_choices_and_text_read_and_write_back)
{
    fixture made = make_fixture();
    char text[16] = {0};

    TB_TEST_EXPECT_UINT(parse(made.volume, "0.5"), ==, TB_SETTING_OK);
    expect_format(made.volume, TB_SETTING_LAYER_COUNT, "0.5");
    TB_TEST_EXPECT_UINT(parse(made.volume, "1e-1"), ==, TB_SETTING_OK);
    expect_format(made.volume, TB_SETTING_LAYER_COUNT, "0.1");
    TB_TEST_EXPECT_UINT(parse(made.volume, "1"), ==, TB_SETTING_OK);
    expect_format(made.volume, TB_SETTING_LAYER_COUNT, "1.0");
    TB_TEST_EXPECT_UINT(parse(made.volume, "1.0000001"), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(parse(made.volume, "nan"), ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_UINT(parse(made.volume, "loud"), ==, TB_SETTING_INVALID);

    TB_TEST_EXPECT_UINT(parse(made.quality, "Low"), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.quality), ==, 0);
    expect_format(made.quality, TB_SETTING_LAYER_COUNT, "low");
    TB_TEST_EXPECT_UINT(parse(made.quality, "ultra"), ==, TB_SETTING_OUT_OF_RANGE);

    /* Text may be bare or quoted, and quoted text has escapes. */
    TB_TEST_EXPECT_UINT(parse(made.language, "en US"), ==, TB_SETTING_OK);
    expect_format(made.language, TB_SETTING_LAYER_COUNT, "\"en US\"");
    TB_TEST_EXPECT_UINT(parse(made.language, "\"a\\\"b\\\\\\n\""), ==, TB_SETTING_OK);
    (void)tb_setting_text(made.language, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "a\"b\\\n");
    expect_format(made.language, TB_SETTING_LAYER_COUNT, "\"a\\\"b\\\\\\n\"");
    TB_TEST_EXPECT_UINT(parse(made.language, "\"\\t\\r\""), ==, TB_SETTING_OK);
    expect_format(made.language, TB_SETTING_LAYER_COUNT, "\"\\t\\r\"");
    TB_TEST_EXPECT_UINT(parse(made.language, "\"open"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(made.language, "\"a\" b"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(made.language, "\"\\q\""), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(made.language, "\"\\"), ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(tb_setting_parse(made.language, TB_SETTING_LAYER_COMMAND_LINE, "a\0b", 3),
                        ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_UINT(parse(made.language, "\"\""), ==, TB_SETTING_OK);
    TB_TEST_EXPECT_UINT(tb_setting_parse(made.language, TB_SETTING_LAYER_COMMAND_LINE, NULL, 0), ==,
                        TB_SETTING_OK);
    expect_format(made.language, TB_SETTING_LAYER_COUNT, "\"\"");
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_text, formatting_says_how_much_room_the_text_needs)
{
    fixture made = make_fixture();
    char text[4] = {'x', 'x', 'x', 'x'};

    (void)tb_setting_set_text(made.language, "abcd");
    TB_TEST_EXPECT_UINT(tb_setting_format(made.language, TB_SETTING_LAYER_COUNT, NULL, 0), ==, 6);
    TB_TEST_EXPECT_UINT(tb_setting_format(made.language, TB_SETTING_LAYER_COUNT, text, 4), ==, 6);
    TB_TEST_EXPECT_UINT((unsigned char)text[0], ==, 'x');
    TB_TEST_EXPECT_UINT(tb_setting_format(made.fullscreen, TB_SETTING_LAYER_COUNT, text, 4), ==, 5);
    TB_TEST_EXPECT_UINT(tb_setting_format(made.fullscreen, TB_SETTING_LAYER_BUILT_IN, NULL, 0), ==,
                        5);
    /* A layer without a value writes nothing. */
    TB_TEST_EXPECT_UINT(tb_setting_format(made.volume, TB_SETTING_LAYER_PLAYER_FILE, text, 4), ==,
                        0);
    TB_TEST_EXPECT_STRING(text, ==, "");
    expect_format(made.frame_limit, TB_SETTING_LAYER_BUILT_IN, "0");
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_text, setting_by_name_finds_the_setting_or_says_there_is_none)
{
    fixture made = make_fixture();

    TB_TEST_EXPECT_UINT(
        tb_settings_set(made.settings, TB_SETTING_LAYER_COMMAND_LINE, "video.frame_limit", "60"),
        ==, TB_SETTING_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 60);
    TB_TEST_EXPECT_UINT(tb_setting_source(made.frame_limit), ==, TB_SETTING_LAYER_COMMAND_LINE);
    TB_TEST_EXPECT_UINT(
        tb_settings_set(made.settings, TB_SETTING_LAYER_COMMAND_LINE, "video.frame_limt", "60"), ==,
        TB_SETTING_UNKNOWN);
    TB_TEST_EXPECT_UINT(
        tb_settings_set(made.settings, TB_SETTING_LAYER_COMMAND_LINE, "video.frame_limit", "sixty"),
        ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_STRING(tb_setting_result_name(TB_SETTING_OUT_OF_RANGE), ==, "out of range");
    TB_TEST_EXPECT_STRING(tb_setting_result_name(TB_SETTING_UNKNOWN), ==, "unknown setting");
    TB_TEST_EXPECT_STRING(tb_setting_result_name(TB_SETTING_INVALID), ==, "invalid value");
    TB_TEST_EXPECT_STRING(tb_setting_result_name(TB_SETTING_OK), ==, "ok");
    TB_TEST_EXPECT_STRING(tb_setting_result_name((tb_setting_result)99), ==, "unknown result");
    TB_TEST_EXPECT_STRING(tb_setting_layer_name(TB_SETTING_LAYER_BUILT_IN), ==, "built-in");
    TB_TEST_EXPECT_STRING(tb_setting_layer_name(TB_SETTING_LAYER_DEFAULT_FILE), ==, "default file");
    TB_TEST_EXPECT_STRING(tb_setting_layer_name(TB_SETTING_LAYER_PLAYER_FILE), ==, "player file");
    TB_TEST_EXPECT_STRING(tb_setting_layer_name(TB_SETTING_LAYER_COMMAND_LINE), ==, "command line");
    TB_TEST_EXPECT_STRING(tb_setting_layer_name(TB_SETTING_LAYER_RUNTIME), ==, "runtime");
    TB_TEST_EXPECT_STRING(tb_setting_layer_name(TB_SETTING_LAYER_COUNT), ==, "unknown layer");
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Loading text.
 * ------------------------------------------------------------------------- */

typedef struct problems
{
    size_t count;
    size_t lines[8];
    tb_setting_result results[8];
    char messages[8][TB_SETTINGS_MESSAGE_CAPACITY];
} problems;

static void collect(void *context, const tb_settings_problem *problem)
{
    problems *const found = context;

    if (found->count < 8)
    {
        found->lines[found->count] = problem->line;
        found->results[found->count] = problem->result;
        memcpy(found->messages[found->count], problem->message, sizeof(problem->message));
    }
    found->count++;
}

static size_t load(fixture *made, tb_setting_layer layer, const char *text, problems *found)
{
    return tb_settings_load_text(made->settings, layer, text, tb_str_length(text), collect, found);
}

TB_TEST(settings_load, a_file_sets_its_layer_line_by_line)
{
    fixture made = make_fixture();
    problems found = {0};
    char text[16] = {0};
    static const char file[] = "\xEF\xBB\xBF# A player's settings\r\n"
                               "\r\n"
                               "video.fullscreen = true\r\n"
                               "   video.frame_limit=144   \n"
                               "\t# indented comment\n"
                               "video.sound.volume = \"0.25\"\n"
                               "video.quality = medium\n"
                               "video.language = \"fr = FR\"";

    TB_TEST_EXPECT_UINT(load(&made, TB_SETTING_LAYER_PLAYER_FILE, file, &found), ==, 0);
    TB_TEST_EXPECT(tb_setting_boolean(made.fullscreen));
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 144);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.25);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.quality), ==, 1);
    (void)tb_setting_text(made.language, text, sizeof(text));
    TB_TEST_EXPECT_STRING(text, ==, "fr = FR");
    TB_TEST_EXPECT_UINT(tb_setting_source(made.quality), ==, TB_SETTING_LAYER_PLAYER_FILE);
    TB_TEST_EXPECT_UINT(tb_setting_source(made.tick_rate), ==, TB_SETTING_LAYER_BUILT_IN);
    /* Empty text loads nothing, and without a report. */
    TB_TEST_EXPECT_UINT(
        tb_settings_load_text(made.settings, TB_SETTING_LAYER_DEFAULT_FILE, NULL, 0, NULL, NULL),
        ==, 0);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_load, problems_are_reported_with_their_lines_and_skipped)
{
    fixture made = make_fixture();
    problems found = {0};
    static const char file[] = "video.frame_limit = 30\n"
                               "no equals sign\n"
                               "Video.Bad = 1\n"
                               "video.frame_limit = 5000\n"
                               "video.quality = ultra\n"
                               "video.language = \"unterminated\n"
                               "video.fullscreen = maybe\n"
                               "video.sound.volume = 0.5\n";

    TB_TEST_EXPECT_UINT(load(&made, TB_SETTING_LAYER_PLAYER_FILE, file, &found), ==, 6);
    TB_TEST_REQUIRE_UINT(found.count, ==, 6);
    TB_TEST_EXPECT_UINT(found.lines[0], ==, 2);
    TB_TEST_EXPECT_UINT(found.results[0], ==, TB_SETTING_INVALID);
    TB_TEST_EXPECT_STRING(found.messages[0], ==, "line 2: expected 'name = value'");
    TB_TEST_EXPECT_STRING(found.messages[1], ==, "line 3: 'Video.Bad' is not a setting's name");
    TB_TEST_EXPECT_UINT(found.results[2], ==, TB_SETTING_OUT_OF_RANGE);
    TB_TEST_EXPECT_STRING(found.messages[2], ==,
                          "line 4: 'video.frame_limit': '5000' is not from 0 to 1000");
    TB_TEST_EXPECT_STRING(found.messages[3], ==, "line 5: 'video.quality' has no choice 'ultra'");
    TB_TEST_EXPECT_STRING(found.messages[4], ==,
                          "line 6: 'video.language': quoted text without its closing quote");
    TB_TEST_EXPECT_STRING(found.messages[5], ==,
                          "line 7: 'video.fullscreen': 'maybe' is not true or false");
    /* The good lines load, and a bad line leaves its setting to the layers
     * below: the earlier good line for the frame limit still counts. */
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 30);
    TB_TEST_EXPECT_FLOAT(tb_setting_number(made.volume), ==, 0.5);
    TB_TEST_EXPECT_UINT(tb_setting_source(made.quality), ==, TB_SETTING_LAYER_BUILT_IN);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_load, problem_messages_describe_each_kind_of_value)
{
    fixture made = make_fixture();
    problems found = {0};
    static const char file[] = "video.sound.volume = 2\n"
                               "video.sound.volume = loud\n"
                               "video.language = too long for it\n"
                               "video.frame_limit = "
                               "1234567890123456789012345678901234567890123456789012345\n";

    TB_TEST_EXPECT_UINT(load(&made, TB_SETTING_LAYER_PLAYER_FILE, file, &found), ==, 4);
    TB_TEST_EXPECT_STRING(found.messages[0], ==,
                          "line 1: 'video.sound.volume': '2' is not from 0.0 to 1.0");
    TB_TEST_EXPECT_STRING(found.messages[1], ==,
                          "line 2: 'video.sound.volume': 'loud' is not a number");
    TB_TEST_EXPECT_STRING(found.messages[2], ==,
                          "line 3: 'video.language': the text is longer than 8 bytes");
    /* A long value is cut short. */
    TB_TEST_EXPECT_STRING(found.messages[3], ==,
                          "line 4: 'video.frame_limit': "
                          "'123456789012345678901234567890123456789012345678...' is not from 0 "
                          "to 1000");
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_load, loading_a_layer_again_replaces_what_it_held)
{
    fixture made = make_fixture();
    problems found = {0};
    uint64_t version = 0;

    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE,
               "video.frame_limit = 30\nvideo.fullscreen = true\n", &found);
    version = tb_settings_version(made.settings);
    /* The same value again changes nothing; a line that is gone takes its
     * value with it; the last of two lines for a setting wins. */
    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE,
               "video.frame_limit = 10\nvideo.frame_limit = 30\n", &found);
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 30);
    TB_TEST_EXPECT_UINT(tb_setting_version(made.frame_limit), ==, 1);
    TB_TEST_EXPECT(!tb_setting_boolean(made.fullscreen));
    TB_TEST_EXPECT_UINT(tb_settings_version(made.settings), ==, version + 1);
    /* Other layers are untouched. */
    (void)tb_setting_set_text(made.language, "it");
    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE,
               "video.language = \"es\"\nvideo.language = de\n", &found);
    TB_TEST_EXPECT_UINT(tb_setting_source(made.language), ==, TB_SETTING_LAYER_RUNTIME);
    expect_format(made.language, TB_SETTING_LAYER_PLAYER_FILE, "\"de\"");
    TB_TEST_EXPECT_UINT(found.count, ==, 0);
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Names nobody has added.
 * ------------------------------------------------------------------------- */

TB_TEST(settings_load, unknown_names_are_reported_kept_and_applied_when_added)
{
    fixture made = make_fixture();
    problems found = {0};
    tb_setting *later = NULL;
    tb_setting *wrong = NULL;

    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE,
               "video.gamma = 2.2\nvideo.later = 7\nvideo.wrong = x\nvideo.later = 9\n", &found);
    TB_TEST_REQUIRE_UINT(found.count, ==, 4);
    TB_TEST_EXPECT_UINT(found.results[0], ==, TB_SETTING_UNKNOWN);
    TB_TEST_EXPECT_STRING(found.messages[0], ==, "line 1: 'video.gamma' is not a setting");

    later = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "video.later",
                                 .type = TB_SETTING_INTEGER,
                                 .integer = {.default_value = 1, .minimum = 0, .maximum = 10}});
    TB_TEST_EXPECT_INT(tb_setting_integer(later), ==, 9);
    TB_TEST_EXPECT_UINT(tb_setting_source(later), ==, TB_SETTING_LAYER_PLAYER_FILE);
    /* A kept value that does not fit is dropped. */
    wrong = tb_settings_add(
        made.settings,
        &(tb_setting_definition){.name = "video.wrong",
                                 .type = TB_SETTING_INTEGER,
                                 .integer = {.default_value = 1, .minimum = 0, .maximum = 10}});
    TB_TEST_EXPECT_INT(tb_setting_integer(wrong), ==, 1);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_load, unknown_names_from_the_players_file_are_written_back)
{
    fixture made = make_fixture();
    problems found = {0};
    size_t length = 0;
    char *text = NULL;

    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE, "video.gamma = 2.2\nvideo.quality = low\n",
               &found);
    (void)load(&made, TB_SETTING_LAYER_DEFAULT_FILE, "video.from_defaults = 1\n", &found);
    text = tb_settings_write_text(made.settings, NULL, &length);
    TB_TEST_EXPECT_STRING(text, ==,
                          "# Settings, one a line as name = value. Lines that start with # are "
                          "comments.\n"
                          "\n# Video\n"
                          "video.quality = low\n"
                          "\n# Settings that this version of the game does not have\n"
                          "video.gamma = \"2.2\"\n");
    tb_str_free(NULL, text);

    /* Restoring every default forgets them too. */
    tb_settings_restore_all(made.settings);
    text = tb_settings_write_text(made.settings, NULL, &length);
    TB_TEST_EXPECT_STRING(text, ==,
                          "# Settings, one a line as name = value. Lines that start with # are "
                          "comments.\n");
    tb_str_free(NULL, text);
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Writing the player's choices.
 * ------------------------------------------------------------------------- */

TB_TEST(settings_write, only_the_players_own_choices_are_written)
{
    fixture made = make_fixture();
    problems found = {0};
    size_t length = 0;
    char *text = NULL;

    (void)load(&made, TB_SETTING_LAYER_DEFAULT_FILE, "video.frame_limit = 60\n", &found);
    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE, "video.sound.volume = 0.5\n", &found);
    (void)tb_settings_set(made.settings, TB_SETTING_LAYER_COMMAND_LINE, "video.fullscreen", "true");
    (void)tb_setting_set_choice(made.quality, 0);
    /* A runtime change wins over the player's file. */
    (void)tb_setting_set_number(made.volume, 0.75);
    /* A family not for players is never written. */
    (void)tb_setting_set_integer(made.tick_rate, 30);

    text = tb_settings_write_text(made.settings, NULL, &length);
    TB_TEST_EXPECT_STRING(text, ==,
                          "# Settings, one a line as name = value. Lines that start with # are "
                          "comments.\n"
                          "\n# Video\n"
                          "video.sound.volume = 0.75\n"
                          "video.quality = low\n");
    TB_TEST_EXPECT_UINT(length, ==, tb_str_length(text));
    tb_str_free(NULL, text);
    tb_settings_destroy(made.settings);
}

/* What one registry writes, another reads back as the same values. */
TB_TEST(settings_write, written_text_reads_back_as_the_same_values)
{
    fixture made = make_fixture();
    fixture other = make_fixture();
    problems found = {0};
    size_t length = 0;
    char *text = NULL;

    (void)tb_setting_set_boolean(made.fullscreen, true);
    (void)tb_setting_set_integer(made.frame_limit, 1000);
    (void)tb_setting_set_number(made.volume, 0.1 + 0.2);
    (void)tb_setting_set_choice(made.quality, 1);
    (void)tb_setting_set_text(made.language, "\"\\\n\t#=");
    text = tb_settings_write_text(made.settings, NULL, &length);
    TB_TEST_EXPECT_UINT(tb_settings_load_text(other.settings, TB_SETTING_LAYER_PLAYER_FILE, text,
                                              length, collect, &found),
                        ==, 0);
    for (size_t index = 0; index < tb_settings_count(made.settings); index++)
    {
        char written[64] = {0};
        char read[64] = {0};
        (void)tb_setting_format(tb_settings_at(made.settings, index), TB_SETTING_LAYER_COUNT,
                                written, sizeof(written));
        (void)tb_setting_format(tb_settings_at(other.settings, index), TB_SETTING_LAYER_COUNT, read,
                                sizeof(read));
        TB_TEST_CONTEXT("%s", tb_setting_name(tb_settings_at(made.settings, index)));
        TB_TEST_EXPECT_STRING(read, ==, written);
    }
    TB_TEST_EXPECT_FLOAT(tb_setting_number(other.volume), ==, 0.1 + 0.2);
    tb_str_free(NULL, text);
    tb_settings_destroy(other.settings);
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Restoring defaults.
 * ------------------------------------------------------------------------- */

TB_TEST(settings_restore, restoring_removes_the_players_choices_and_nothing_else)
{
    fixture made = make_fixture();
    problems found = {0};

    (void)load(&made, TB_SETTING_LAYER_DEFAULT_FILE, "video.frame_limit = 60\n", &found);
    (void)load(&made, TB_SETTING_LAYER_PLAYER_FILE, "video.frame_limit = 90\nvideo.quality = low\n",
               &found);
    (void)tb_setting_set_integer(made.frame_limit, 120);
    tb_setting_restore_default(made.frame_limit);
    TB_TEST_EXPECT_INT(tb_setting_integer(made.frame_limit), ==, 60);
    TB_TEST_EXPECT_UINT(tb_setting_source(made.frame_limit), ==, TB_SETTING_LAYER_DEFAULT_FILE);
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.quality), ==, 0);

    /* A command line's value outlasts a restore: it is the session's. */
    (void)tb_settings_set(made.settings, TB_SETTING_LAYER_COMMAND_LINE, "video.quality", "medium");
    (void)tb_setting_set_integer(made.tick_rate, 30);
    tb_settings_restore_family(made.settings, "video");
    TB_TEST_EXPECT_UINT(tb_setting_choice(made.quality), ==, 1);
    TB_TEST_EXPECT_INT(tb_setting_integer(made.tick_rate), ==, 30);
    tb_settings_restore_family(made.settings, "engine");
    TB_TEST_EXPECT_INT(tb_setting_integer(made.tick_rate), ==, 60);
    (void)tb_setting_set_boolean(made.fullscreen, true);
    tb_settings_restore_all(made.settings);
    TB_TEST_EXPECT(!tb_setting_boolean(made.fullscreen));
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Files.
 * ------------------------------------------------------------------------- */

TB_TEST(settings_files, saving_creates_the_directory_and_loading_reads_it_back)
{
    fixture made = make_fixture();
    fixture other = make_fixture();
    char root[4096] = {0};
    char directory[4096] = {0};
    char path[4096] = {0};
    char aside[4096] = {0};
    size_t required = 0;
    problems found = {0};
    tb_fs_info info = {0};

    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(root), view("Game"), directory,
                                      sizeof(directory), &required),
                         ==, TB_PATH_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(directory), view("settings.cfg"),
                                      path, sizeof(path), &required),
                         ==, TB_PATH_OK);
    (void)tb_fmt_buffer(aside, sizeof(aside), "%s.bak", path);

    /* Before the first save there is no file, and the layer is untouched. */
    (void)tb_settings_set(other.settings, TB_SETTING_LAYER_PLAYER_FILE, "video.frame_limit", "5");
    TB_TEST_EXPECT_UINT(tb_settings_load_file(other.settings, TB_SETTING_LAYER_PLAYER_FILE,
                                              view(path), collect, &found),
                        ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_INT(tb_setting_integer(other.frame_limit), ==, 5);

    (void)tb_setting_set_integer(made.frame_limit, 75);
    TB_TEST_EXPECT_UINT(tb_settings_save_file(made.settings, view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_settings_load_file(other.settings, TB_SETTING_LAYER_PLAYER_FILE,
                                              view(path), collect, &found),
                        ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(other.frame_limit), ==, 75);
    /* Saving again replaces the file. */
    (void)tb_setting_set_integer(made.frame_limit, 80);
    TB_TEST_EXPECT_UINT(tb_settings_save_file(made.settings, view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(
        tb_settings_load_file(other.settings, TB_SETTING_LAYER_PLAYER_FILE, view(path), NULL, NULL),
        ==, TB_FS_OK);
    TB_TEST_EXPECT_INT(tb_setting_integer(other.frame_limit), ==, 80);

    /* Setting the file aside moves it to .bak, twice over, and is content
     * when there is nothing to move. */
    TB_TEST_EXPECT_UINT(tb_settings_set_aside_file(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_query(view(path), &info), ==, TB_FS_NOT_FOUND);
    TB_TEST_EXPECT_UINT(tb_fs_query(view(aside), &info), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_settings_save_file(made.settings, view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_settings_set_aside_file(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_settings_set_aside_file(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(found.count, ==, 0);

    TB_TEST_EXPECT_UINT(tb_fs_remove(view(aside)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(directory)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
    tb_settings_destroy(other.settings);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_files, a_file_too_large_is_not_read)
{
    fixture made = make_fixture();
    char root[4096] = {0};
    char path[4096] = {0};
    size_t required = 0;
    char *large = NULL;

    TB_TEST_REQUIRE_UINT(tb_fs_temp_directory((tb_path_view){0}, root, sizeof(root)), ==, TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(root), view("large.cfg"), path,
                                      sizeof(path), &required),
                         ==, TB_PATH_OK);
    large = tb_calloc(TB_SETTINGS_FILE_LIMIT + 1, 1);
    memset(large, '#', TB_SETTINGS_FILE_LIMIT + 1);
    TB_TEST_REQUIRE_UINT(tb_fs_write_file(view(path), large, TB_SETTINGS_FILE_LIMIT + 1), ==,
                         TB_FS_OK);
    TB_TEST_EXPECT_UINT(
        tb_settings_load_file(made.settings, TB_SETTING_LAYER_PLAYER_FILE, view(path), NULL, NULL),
        ==, TB_FS_TOO_LARGE);
    tb_free(large);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(path)), ==, TB_FS_OK);
    TB_TEST_EXPECT_UINT(tb_fs_remove(view(root)), ==, TB_FS_OK);
    tb_settings_destroy(made.settings);
}

TB_TEST(settings_files, the_players_file_lives_in_the_games_configuration_directory)
{
    char *path = NULL;
    char base[4096] = {0};
    char directory[4096] = {0};
    char expected[4096] = {0};
    char sentinel[] = "untouched";
    size_t required = 0;
    static const char *const wrong[] = {"", ".", "..", "a/b", "a\\b", "C:", "a\nb", "\xC3\x28"};

    TB_TEST_REQUIRE_UINT(tb_fs_get_location(TB_FS_USER_CONFIG, base, sizeof(base), &required), ==,
                         TB_FS_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(base), view("Burrow"), directory,
                                      sizeof(directory), &required),
                         ==, TB_PATH_OK);
    TB_TEST_REQUIRE_UINT(tb_path_join(tb_fs_native_style(), view(directory), view("settings.cfg"),
                                      expected, sizeof(expected), &required),
                         ==, TB_PATH_OK);
    TB_TEST_EXPECT_UINT(tb_settings_player_file_path("Burrow", NULL, &path), ==, TB_FS_OK);
    TB_TEST_EXPECT_STRING(path, ==, expected);
    tb_str_free(NULL, path);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(wrong); index++)
    {
        TB_TEST_CONTEXT("application %zu", index);
        path = sentinel;
        TB_TEST_EXPECT_UINT(tb_settings_player_file_path(wrong[index], NULL, &path), ==,
                            TB_FS_INVALID);
        TB_TEST_EXPECT_POINTER(path, ==, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Threads.
 * ------------------------------------------------------------------------- */

enum
{
    WRITERS = 3,
    CHANGES = 2000
};

typedef struct thread_work
{
    fixture *made;
    size_t identity;
    size_t wrong; /* Values read that no writer wrote. */
} thread_work;

/* Each writer sets the frame limit to its own values and a text to its own
 * name, while reading both: every value read must be one some writer set,
 * never a mixture. */
static void change_settings(void *argument)
{
    thread_work *const work = argument;
    static const char *const names[WRITERS] = {"aaaa", "bbbb", "cccc"};

    for (size_t change = 0; change < CHANGES; change++)
    {
        char text[16] = {0};
        int64_t limit = 0;
        (void)tb_setting_set_integer(work->made->frame_limit,
                                     (int64_t)(work->identity * 100 + change % 100));
        (void)tb_setting_set_text(work->made->language, names[work->identity]);
        limit = tb_setting_integer(work->made->frame_limit);
        (void)tb_setting_text(work->made->language, text, sizeof(text));
        if (limit < 0 || limit >= WRITERS * 100)
        {
            work->wrong++;
        }
        if (tb_str_length(text) != 4 || text[0] != text[3])
        {
            work->wrong++;
        }
    }
}

TB_TEST(settings_threads, settings_change_and_read_safely_on_many_threads)
{
    fixture made = make_fixture();
    thread_work work[WRITERS] = {0};
    tb_thread threads[WRITERS] = {0};

    for (size_t index = 0; index < WRITERS; index++)
    {
        work[index] = (thread_work){.made = &made, .identity = index};
        tb_thread_create(&threads[index], change_settings, &work[index]);
    }
    for (size_t index = 0; index < WRITERS; index++)
    {
        tb_thread_join(&threads[index]);
        TB_TEST_EXPECT_UINT(work[index].wrong, ==, 0);
    }
    TB_TEST_EXPECT_UINT(tb_setting_source(made.frame_limit), ==, TB_SETTING_LAYER_RUNTIME);
    tb_settings_destroy(made.settings);
}

/* ---------------------------------------------------------------------------
 * Contracts.
 * ------------------------------------------------------------------------- */

static tb_settings *contract_registry(void)
{
    tb_settings *const settings = tb_settings_create();

    tb_settings_add_family(settings, &(tb_settings_family){.name = "video"});
    return settings;
}

static void add_bad_name(void *context)
{
    tb_settings *const settings = context;
    (void)tb_settings_add(settings,
                          &(tb_setting_definition){.name = "video..x", .type = TB_SETTING_BOOLEAN});
}

static void add_without_family(void *context)
{
    tb_settings *const settings = context;
    (void)tb_settings_add(
        settings, &(tb_setting_definition){.name = "audio.volume", .type = TB_SETTING_BOOLEAN});
}

static void add_twice(void *context)
{
    tb_settings *const settings = context;
    const tb_setting_definition definition = {.name = "video.twice", .type = TB_SETTING_BOOLEAN};
    (void)tb_settings_add(settings, &definition);
    (void)tb_settings_add(settings, &definition);
}

static void add_empty_range(void *context)
{
    tb_settings *const settings = context;
    (void)tb_settings_add(settings,
                          &(tb_setting_definition){.name = "video.range",
                                                   .type = TB_SETTING_INTEGER,
                                                   .integer = {.minimum = 2, .maximum = 1}});
}

static void add_family_twice(void *context)
{
    tb_settings *const settings = context;
    tb_settings_add_family(settings, &(tb_settings_family){.name = "video"});
}

static void read_as_wrong_type(void *context)
{
    tb_settings *const settings = context;
    tb_setting *const setting = tb_settings_add(
        settings, &(tb_setting_definition){.name = "video.flag", .type = TB_SETTING_BOOLEAN});
    (void)tb_setting_integer(setting);
}

static void add_duplicate_choices(void *context)
{
    static const char *const names[] = {"same", "same"};
    tb_settings *const settings = context;
    (void)tb_settings_add(settings,
                          &(tb_setting_definition){.name = "video.choice",
                                                   .type = TB_SETTING_CHOICE,
                                                   .choice = {.names = names, .count = 2}});
}

static void set_the_built_in_layer(void *context)
{
    tb_settings *const settings = context;
    tb_setting *const setting = tb_settings_add(
        settings, &(tb_setting_definition){.name = "video.layer", .type = TB_SETTING_BOOLEAN});
    (void)tb_setting_parse(setting, TB_SETTING_LAYER_BUILT_IN, "true", 4);
}

static void restore_unknown_family(void *context)
{
    tb_settings *const settings = context;
    tb_settings_restore_family(settings, "audio");
}

static void check_trap(void (*body)(void *), const char *message)
{
    tb_settings *const settings = contract_registry();
    TB_TEST_CONTEXT("expecting \"%s\"", message);
    TB_TEST_EXPECT_TRAP(body, settings, message);
    tb_settings_destroy(settings);
}

TB_TEST(settings_contracts, wrong_definitions_and_uses_violate_the_contract)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the engine was built without assertions");
    }
    check_trap(add_bad_name, "a setting's name is its family's");
    check_trap(add_without_family, "a setting's family is added before it");
    check_trap(add_twice, "no setting has the name already");
    check_trap(add_empty_range, "minimum is at most its maximum");
    check_trap(add_family_twice, "no family has the name already");
    check_trap(read_as_wrong_type, "the setting is of the type it is used as");
    check_trap(add_duplicate_choices, "choices have different names");
    check_trap(set_the_built_in_layer, "the layer is one a value may be set at");
    check_trap(restore_unknown_family, "a family has the name");
}
