/* The engine's catalog must put every setting in a family that exists, keep
 * the families players see apart from those they do not, start developer and
 * release builds as each should, and write and read back every default. */
#include "badger/engine_settings.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

TB_TEST(settings_engine, the_families_are_those_of_an_options_menu_in_its_order)
{
    static const char *const names[] = {"gameplay", "display",   "graphics",      "audio",
                                        "controls", "interface", "accessibility", "language",
                                        "engine",   "developer"};
    static const bool for_players[] = {true, true, true, true,  true,
                                       true, true, true, false, false};
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    TB_TEST_REQUIRE_UINT(tb_settings_family_count(settings), ==, TB_TEST_COUNT_OF(names));
    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        const tb_settings_family family = tb_settings_family_at(settings, index);
        TB_TEST_CONTEXT("family %s", names[index]);
        TB_TEST_EXPECT_STRING(family.name, ==, names[index]);
        TB_TEST_EXPECT(family.for_players == for_players[index]);
        TB_TEST_EXPECT(family.description != NULL);
    }
    tb_settings_destroy(settings);
}

/* Every setting has a description, and the structure points at each of
 * them: the structure is nothing but setting pointers, and each one is a
 * setting of the registry, none twice. */
TB_TEST(settings_engine, every_setting_is_described_and_in_the_structure)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};
    tb_setting *pointers[sizeof(tb_engine_settings) / sizeof(tb_setting *)] = {0};
    const size_t pointer_count = TB_TEST_COUNT_OF(pointers);

    tb_engine_settings_add(settings, NULL, &engine);
    memcpy(pointers, &engine, sizeof(pointers));
    TB_TEST_EXPECT_UINT(tb_settings_count(settings), ==, pointer_count);
    for (size_t index = 0; index < tb_settings_count(settings); index++)
    {
        const tb_setting *const setting = tb_settings_at(settings, index);
        size_t seen = 0;
        TB_TEST_CONTEXT("%s", tb_setting_name(setting));
        TB_TEST_EXPECT(tb_setting_description(setting) != NULL);
        for (size_t pointer = 0; pointer < pointer_count; pointer++)
        {
            seen += pointers[pointer] == setting ? 1u : 0u;
        }
        TB_TEST_EXPECT_UINT(seen, ==, 1);
    }
    tb_settings_destroy(settings);
}

TB_TEST(settings_engine, developer_builds_start_in_a_window_and_quit_without_asking)
{
    tb_settings *const developer = tb_settings_create();
    tb_settings *const release = tb_settings_create();
    tb_engine_settings in_developer = {0};
    tb_engine_settings in_release = {0};

    tb_engine_settings_add(developer, &(tb_engine_settings_options){.developer = true},
                           &in_developer);
    tb_engine_settings_add(release, &(tb_engine_settings_options){.developer = false}, &in_release);
    TB_TEST_EXPECT_UINT(tb_setting_choice(in_developer.display.window_mode), ==,
                        TB_WINDOW_MODE_WINDOWED);
    TB_TEST_EXPECT(tb_setting_boolean(in_developer.developer.quit_without_asking));
    TB_TEST_EXPECT_UINT(tb_setting_choice(in_release.display.window_mode), ==,
                        TB_WINDOW_MODE_BORDERLESS);
    TB_TEST_EXPECT(!tb_setting_boolean(in_release.developer.quit_without_asking));
    /* The choices' names follow the enumerations. */
    TB_TEST_EXPECT_STRING(
        tb_setting_choice_name(in_release.display.window_mode, TB_WINDOW_MODE_FULLSCREEN), ==,
        "fullscreen");
    TB_TEST_EXPECT_STRING(tb_setting_choice_name(in_release.display.vsync, TB_VSYNC_ADAPTIVE), ==,
                          "adaptive");
    TB_TEST_EXPECT_UINT(tb_setting_choice(in_release.display.vsync), ==, TB_VSYNC_ON);
    tb_settings_destroy(release);
    tb_settings_destroy(developer);
}

/* Each setting's default, written as a file writes it, reads back as the
 * same value. */
TB_TEST(settings_engine, every_default_writes_and_reads_back)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    for (size_t index = 0; index < tb_settings_count(settings); index++)
    {
        tb_setting *const setting = tb_settings_at(settings, index);
        char written[512] = {0};
        char read[512] = {0};
        TB_TEST_CONTEXT("%s", tb_setting_name(setting));
        (void)tb_setting_format(setting, TB_SETTING_LAYER_BUILT_IN, written, sizeof(written));
        TB_TEST_EXPECT_UINT(tb_setting_parse(setting, TB_SETTING_LAYER_PLAYER_FILE, written,
                                             tb_str_length(written)),
                            ==, TB_SETTING_OK);
        (void)tb_setting_format(setting, TB_SETTING_LAYER_PLAYER_FILE, read, sizeof(read));
        TB_TEST_EXPECT_STRING(read, ==, written);
        TB_TEST_EXPECT_UINT(tb_setting_version(setting), ==, 0);
    }
    tb_settings_destroy(settings);
}
