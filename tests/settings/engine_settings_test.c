/* The engine's catalog must put every setting in a family that exists, keep
 * the families players see apart from those they do not, start developer and
 * release builds as each should, and write and read back every default. */
#include "badger/engine_settings.h"
#include "badger/rhi.h"
#include "badger/str.h"

#include "tb_test.h"

#include <string.h>

TB_TEST(settings_engine, the_families_are_those_of_an_options_menu_in_its_order)
{
    static const char *const names[] = {"gameplay", "display",   "graphics",      "audio",
                                        "controls", "interface", "accessibility", "language",
                                        "engine",   "shaders",   "developer"};
    static const bool for_players[] = {true, true, true,  true,  true, true,
                                       true, true, false, false, false};
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

/* As in Unreal, the overall quality is what the groups are: a game starts at
 * high, setting it sets every group and anisotropic filtering, and a player
 * who changes one group, or the filtering, has a custom quality. */
TB_TEST(settings_engine, the_overall_quality_is_what_the_quality_groups_are)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    TB_TEST_EXPECT_UINT(tb_engine_settings_quality(&engine), ==, TB_GRAPHICS_QUALITY_HIGH);

    tb_engine_settings_set_quality(&engine, TB_GRAPHICS_QUALITY_LOW);
    TB_TEST_EXPECT_UINT(tb_engine_settings_quality(&engine), ==, TB_GRAPHICS_QUALITY_LOW);
    for (size_t index = 0; index < TB_QUALITY_GROUP_COUNT; index++)
    {
        TB_TEST_CONTEXT("%s", tb_setting_name(engine.graphics.quality[index]));
        TB_TEST_EXPECT_UINT(tb_setting_choice(engine.graphics.quality[index]), ==,
                            TB_GRAPHICS_QUALITY_LOW);
        TB_TEST_EXPECT_UINT(tb_setting_source(engine.graphics.quality[index]), ==,
                            TB_SETTING_LAYER_RUNTIME);
    }
    TB_TEST_EXPECT_STRING(
        tb_setting_choice_name(engine.graphics.anisotropic_filtering,
                               tb_setting_choice(engine.graphics.anisotropic_filtering)),
        ==, "2x");

    tb_engine_settings_set_quality(&engine, TB_GRAPHICS_QUALITY_ULTRA);
    TB_TEST_EXPECT_UINT(tb_engine_settings_quality(&engine), ==, TB_GRAPHICS_QUALITY_ULTRA);
    (void)tb_setting_set_choice(engine.graphics.quality[TB_QUALITY_GROUP_SHADOWS],
                                TB_GRAPHICS_QUALITY_MEDIUM);
    TB_TEST_EXPECT_UINT(tb_engine_settings_quality(&engine), ==, TB_GRAPHICS_QUALITY_CUSTOM);

    tb_engine_settings_set_quality(&engine, TB_GRAPHICS_QUALITY_MEDIUM);
    TB_TEST_EXPECT_UINT(tb_engine_settings_quality(&engine), ==, TB_GRAPHICS_QUALITY_MEDIUM);
    (void)tb_setting_set_choice(engine.graphics.anisotropic_filtering, 0);
    TB_TEST_EXPECT_UINT(tb_engine_settings_quality(&engine), ==, TB_GRAPHICS_QUALITY_CUSTOM);
    tb_settings_destroy(settings);
}

/* The groups are named as their settings are, so a menu can list them. */
TB_TEST(settings_engine, each_quality_group_is_a_setting_of_its_name)
{
    static const char *const names[TB_QUALITY_GROUP_COUNT] = {
        "graphics.texture_quality",
        "graphics.geometry_quality",
        "graphics.shading_quality",
        "graphics.shadow_quality",
        "graphics.reflection_quality",
        "graphics.light_quality",
        "graphics.directional_occlusion_quality",
        "graphics.particle_quality",
        "graphics.decal_quality",
        "graphics.water_quality",
        "graphics.volumetric_quality"};
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    for (size_t index = 0; index < TB_QUALITY_GROUP_COUNT; index++)
    {
        TB_TEST_EXPECT_STRING(tb_setting_name(engine.graphics.quality[index]), ==, names[index]);
        TB_TEST_EXPECT_UINT(tb_setting_choice_count(engine.graphics.quality[index]), ==, 4);
    }
    tb_settings_destroy(settings);
}

/* The renderers are the RHI's backends, in the order of tb_rhi_backend and
 * spelled as tb_rhi_backend_name() spells them, so a choice's index is the
 * backend; the default lets the RHI choose. */
TB_TEST(settings_engine, the_renderers_are_the_backends_of_the_rhi)
{
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    TB_TEST_EXPECT_STRING(tb_setting_name(engine.graphics.renderer), ==, "graphics.renderer");
    TB_TEST_REQUIRE_UINT(tb_setting_choice_count(engine.graphics.renderer), ==,
                         TB_RHI_BACKEND_COUNT);
    for (size_t index = 0; index < TB_RHI_BACKEND_COUNT; index++)
    {
        TB_TEST_CONTEXT("backend %zu", index);
        TB_TEST_EXPECT_STRING(tb_setting_choice_name(engine.graphics.renderer, index), ==,
                              tb_rhi_backend_name((tb_rhi_backend)index));
    }
    TB_TEST_EXPECT_UINT(tb_setting_choice(engine.graphics.renderer), ==, TB_RHI_BACKEND_AUTOMATIC);
    tb_settings_destroy(settings);
}

/* The device's options take the backend the setting names, whatever the
 * platform builds, since a settings file may come from another machine; the
 * rest of the options are the caller's. */
TB_TEST(settings_engine, the_renderer_chooses_the_device_backend)
{
    static const struct
    {
        const char *value;
        tb_rhi_backend backend;
    } cases[] = {{"auto", TB_RHI_BACKEND_AUTOMATIC},
                 {"d3d12", TB_RHI_BACKEND_D3D12},
                 {"metal", TB_RHI_BACKEND_METAL},
                 {"null", TB_RHI_BACKEND_NULL}};
    tb_settings *const settings = tb_settings_create();
    tb_engine_settings engine = {0};

    tb_engine_settings_add(settings, NULL, &engine);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(cases); index++)
    {
        tb_rhi_device_options options = {.validation = true, .adapter = TB_RHI_ADAPTER_SOFTWARE};
        TB_TEST_CONTEXT("%s", cases[index].value);
        TB_TEST_REQUIRE_UINT(tb_settings_set(settings, TB_SETTING_LAYER_COMMAND_LINE,
                                             "graphics.renderer", cases[index].value),
                             ==, TB_SETTING_OK);
        tb_engine_settings_device_options(&engine, &options);
        TB_TEST_EXPECT_UINT(options.backend, ==, cases[index].backend);
        TB_TEST_EXPECT(options.validation);
        TB_TEST_EXPECT_UINT(options.adapter, ==, TB_RHI_ADAPTER_SOFTWARE);
    }
    TB_TEST_EXPECT_UINT(
        tb_settings_set(settings, TB_SETTING_LAYER_COMMAND_LINE, "graphics.renderer", "directx"),
        ==, TB_SETTING_OUT_OF_RANGE);
    tb_settings_destroy(settings);
}
