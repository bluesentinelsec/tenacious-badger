/* The fonts the engine carries, and the catalog that finds fonts by family,
 * weight, and slant: every embedded font opens and is what the catalog says
 * it is, the build holds the fonts TB_EMBED_FONTS chose, the roles find
 * their fonts, and fonts a game adds are found as CSS would find them.
 *
 * The cases of finding run on fonts made in memory, so that they hold in a
 * build of every TB_EMBED_FONTS. */
#include "badger/assert.h"
#include "badger/font.h"
#include "badger/font_catalog.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "font_builder.h"
#include "tb_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* What the build was configured with: TB_TEST_EMBED_FONTS is 0 for all, 1
 * for defaults, and 2 for none (tests/CMakeLists.txt). */
#define EMBED_ALL 0
#define EMBED_DEFAULTS 1
#define EMBED_NONE 2

/* Every file of resources/fonts/. */
#define PACK_FILE_COUNT ((size_t)60)
/* The files of the families of the default roles: Inter, JetBrains Mono, and Px437. */
#define DEFAULT_FILE_COUNT ((size_t)13)

#define FONT_BUFFER ((size_t)65536)
/* Room for a name in UTF-16. */
#define NAME_CAPACITY ((size_t)128)

/* The first four bytes of a TrueType file: version 1.0. */
static const uint8_t truetype_magic[] = {0x00, 0x01, 0x00, 0x00};

/* --- fonts made in memory ------------------------------------------------- */

typedef struct made_font
{
    uint8_t *bytes;
    size_t length;
} made_font;

static void utf16(const char *text, char *out, size_t *length)
{
    size_t count = tb_str_length(text);
    TB_ASSERT(count * 2 <= NAME_CAPACITY);
    for (size_t index = 0; index < count; index++)
    {
        out[index * 2] = 0;
        out[index * 2 + 1] = text[index];
    }
    *length = count * 2;
}

/* A name table of a Windows family (ID 1) and subfamily (2), in US English;
 * no family at all when `family` is NULL. */
static void name_table(test_table *name, const char *family, const char *style)
{
    char family_bytes[NAME_CAPACITY] = {0};
    char style_bytes[NAME_CAPACITY] = {0};
    size_t family_length = 0;
    size_t style_length = 0;
    utf16(style, style_bytes, &style_length);
    name->length = 0;
    test_table_u16(name, 0);
    if (family == NULL)
    {
        test_table_u16(name, 1);
        test_table_u16(name, 6 + 12);
        test_table_u16(name, 3);
        test_table_u16(name, 1);
        test_table_u16(name, 0x0409);
        test_table_u16(name, 2);
        test_table_u16(name, (uint16_t)style_length);
        test_table_u16(name, 0);
        for (size_t byte = 0; byte < style_length; byte++)
        {
            test_table_u8(name, (uint8_t)style_bytes[byte]);
        }
        return;
    }
    utf16(family, family_bytes, &family_length);
    test_table_u16(name, 2);
    test_table_u16(name, 6 + 2 * 12);
    const uint16_t ids[] = {1, 2};
    const size_t lengths[] = {family_length, style_length};
    size_t offset = 0;
    for (size_t record = 0; record < 2; record++)
    {
        test_table_u16(name, 3);
        test_table_u16(name, 1);
        test_table_u16(name, 0x0409);
        test_table_u16(name, ids[record]);
        test_table_u16(name, (uint16_t)lengths[record]);
        test_table_u16(name, (uint16_t)offset);
        offset += lengths[record];
    }
    for (size_t byte = 0; byte < family_length; byte++)
    {
        test_table_u8(name, (uint8_t)family_bytes[byte]);
    }
    for (size_t byte = 0; byte < style_length; byte++)
    {
        test_table_u8(name, (uint8_t)style_bytes[byte]);
    }
}

/* A small valid font of a family, weight, and slant, in memory from tb_malloc. */
static made_font make_font(const char *family, const char *style, uint16_t weight, bool italic)
{
    test_font *font = test_font_create();
    test_table *os2 = test_font_table(font, "OS/2");
    made_font made = {0};
    name_table(test_font_table(font, "name"), family, style);
    test_table_set16(os2, 4, weight);
    test_table_set16(os2, 62, italic ? 0x0001 : 0x0040);
    made.bytes = tb_malloc(FONT_BUFFER);
    made.length = test_font_write(font, made.bytes, FONT_BUFFER);
    test_font_destroy(font);
    return made;
}

/* Add a font made in memory, and release the bytes it was made in. */
static size_t add_font(tb_font_catalog *catalog, const char *family, const char *style,
                       uint16_t weight, bool italic)
{
    made_font made = make_font(family, style, weight, italic);
    size_t index = SIZE_MAX;
    tb_font_error error = {0};
    TB_TEST_EXPECT_UINT(tb_font_catalog_add(catalog, made.bytes, made.length,
                                            TB_FONT_CATEGORY_OTHER, &index, &error),
                        ==, TB_FONT_OK);
    TB_TEST_EXPECT_STRING(error.reason, ==, "");
    tb_free(made.bytes);
    return index;
}

static uint16_t found_weight(const tb_font_catalog *catalog, const char *family, uint16_t weight,
                             bool italic)
{
    size_t index = SIZE_MAX;
    tb_font_info info = {0};
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, family, weight, italic, &index));
    tb_font_catalog_get_info(catalog, index, &info);
    return info.weight;
}

/* --- the embedded fonts ----------------------------------------------------- */

TB_TEST(font_catalog, every_embedded_font_opens_and_is_what_the_catalog_says)
{
    /* Each font's own tables must agree with the table in font_embedded.c,
     * or a game asking for "Inter Bold" would get something else. */
    for (size_t index = 0; index < tb_font_embedded_count(); index++)
    {
        tb_font_info info = {0};
        const void *bytes = NULL;
        size_t length = 0;
        tb_font *font = NULL;
        tb_font_error error = {0};
        tb_font_style style = {0};
        tb_font_embedded_get_info(index, &info);
        TB_TEST_CONTEXT("%s", info.file);
        tb_font_embedded_get_bytes(index, &bytes, &length);
        TB_TEST_EXPECT_UINT(length, ==, info.length);
        TB_TEST_REQUIRE_UINT(length, >=, sizeof truetype_magic);
        TB_TEST_EXPECT_BYTES(bytes, truetype_magic, sizeof truetype_magic);
        TB_TEST_REQUIRE_UINT(tb_font_open(bytes, length, NULL, &font, &error), ==, TB_FONT_OK);
        tb_font_get_style(font, &style);
        TB_TEST_EXPECT_STRING(style.family, ==, info.family);
        TB_TEST_EXPECT_STRING(style.style, ==, info.style);
        TB_TEST_EXPECT_UINT(style.weight, ==, info.weight);
        TB_TEST_EXPECT(style.italic == info.italic);
        tb_font_close(font);

        TB_TEST_EXPECT(info.embedded);
        /* The folder is the category, and every font but Px437 is under the OFL. */
        TB_TEST_EXPECT(tb_str_starts_with(info.file, tb_font_category_name(info.category)));
        TB_TEST_EXPECT_UINT(info.license, ==,
                            tb_str_equal(info.family, "Px437 IBM VGA 8x16")
                                ? TB_FONT_LICENSE_CC_BY_SA_4_0
                                : TB_FONT_LICENSE_OFL_1_1);
        /* Only upright retro fonts are drawn on a grid of pixels. */
        if (info.pixel_size != 0)
        {
            TB_TEST_EXPECT_UINT(info.category, ==, TB_FONT_CATEGORY_RETRO);
            TB_TEST_EXPECT(!info.italic);
        }
    }
}

TB_TEST(font_catalog, the_build_holds_the_fonts_that_its_option_chose)
{
    size_t count = tb_font_embedded_count();
#if TB_TEST_EMBED_FONTS == EMBED_ALL
    TB_TEST_EXPECT_UINT(count, ==, PACK_FILE_COUNT);
#elif TB_TEST_EMBED_FONTS == EMBED_DEFAULTS
    TB_TEST_EXPECT_UINT(count, ==, DEFAULT_FILE_COUNT);
#else
    TB_TEST_EXPECT_UINT(count, ==, 0);
#endif
    /* In the order of their files' names, each once. */
    for (size_t index = 1; index < count; index++)
    {
        tb_font_info previous = {0};
        tb_font_info info = {0};
        tb_font_embedded_get_info(index - 1, &previous);
        tb_font_embedded_get_info(index, &info);
        TB_TEST_EXPECT_INT(tb_str_compare(previous.file, info.file), <, 0);
    }
    /* Every family the defaults keep is one a default role names. */
    for (size_t index = 0; index < count && TB_TEST_EMBED_FONTS == EMBED_DEFAULTS; index++)
    {
        tb_font_info info = {0};
        bool named = false;
        tb_font_embedded_get_info(index, &info);
        for (size_t role = 0; role < TB_FONT_ROLE_COUNT; role++)
        {
            named =
                named || tb_str_equal(tb_font_default_role((tb_font_role)role).family, info.family);
        }
        TB_TEST_CONTEXT("%s", info.file);
        TB_TEST_EXPECT(named);
    }
}

TB_TEST(font_catalog, an_embedded_font_is_decompressed_once)
{
    if (tb_font_embedded_count() == 0)
    {
        TB_TEST_SKIP("this build embeds no font");
    }
    const void *first = NULL;
    const void *second = NULL;
    size_t length = 0;
    tb_font_embedded_get_bytes(0, &first, &length);
    tb_font_embedded_get_bytes(0, &second, &length);
    TB_TEST_EXPECT_POINTER(first, ==, second);
}

TB_TEST(font_catalog, the_default_roles_are_the_provisional_choices)
{
    /* Provisional, until the author chooses: docs/fonts.md lists them. */
    const struct
    {
        tb_font_role role;
        const char *family;
        uint16_t weight;
    } expected[] = {
        {TB_FONT_ROLE_UI, "Inter", TB_FONT_WEIGHT_REGULAR},
        {TB_FONT_ROLE_UI_BOLD, "Inter", TB_FONT_WEIGHT_SEMI_BOLD},
        {TB_FONT_ROLE_BODY, "Inter", TB_FONT_WEIGHT_REGULAR},
        {TB_FONT_ROLE_HEADING, "Inter", TB_FONT_WEIGHT_BOLD},
        {TB_FONT_ROLE_MONOSPACE, "JetBrains Mono", TB_FONT_WEIGHT_REGULAR},
        {TB_FONT_ROLE_PIXEL, "Px437 IBM VGA 8x16", TB_FONT_WEIGHT_REGULAR},
    };
    TB_TEST_REQUIRE_UINT(TB_TEST_COUNT_OF(expected), ==, TB_FONT_ROLE_COUNT);
    for (size_t index = 0; index < TB_TEST_COUNT_OF(expected); index++)
    {
        tb_font_role_choice choice = tb_font_default_role(expected[index].role);
        TB_TEST_CONTEXT("%s", tb_font_role_name(expected[index].role));
        TB_TEST_EXPECT_STRING(choice.family, ==, expected[index].family);
        TB_TEST_EXPECT_UINT(choice.weight, ==, expected[index].weight);
        TB_TEST_EXPECT(!choice.italic);
    }
}

TB_TEST(font_catalog, every_role_finds_its_own_font_when_fonts_are_embedded)
{
    /* With TB_EMBED_FONTS=defaults too: the defaults keep every role's family. */
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    for (size_t role = 0; role < TB_FONT_ROLE_COUNT; role++)
    {
        tb_font_role_choice choice = tb_font_catalog_get_role(catalog, (tb_font_role)role);
        size_t index = SIZE_MAX;
        tb_font_info info = {0};
        TB_TEST_CONTEXT("%s", tb_font_role_name((tb_font_role)role));
        if (tb_font_embedded_count() == 0)
        {
            TB_TEST_EXPECT(!tb_font_catalog_find_role(catalog, (tb_font_role)role, &index));
            TB_TEST_EXPECT_UINT(index, ==, SIZE_MAX);
            continue;
        }
        TB_TEST_REQUIRE(tb_font_catalog_find_role(catalog, (tb_font_role)role, &index));
        tb_font_catalog_get_info(catalog, index, &info);
        TB_TEST_EXPECT_STRING(info.family, ==, choice.family);
        TB_TEST_EXPECT_UINT(info.weight, ==, choice.weight);
        TB_TEST_EXPECT(info.italic == choice.italic);
        TB_TEST_EXPECT(info.embedded);
    }
    tb_font_catalog_destroy(catalog);
}

TB_TEST(font_catalog, a_catalog_lists_the_embedded_fonts_first)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    TB_TEST_REQUIRE_UINT(tb_font_catalog_count(catalog), ==, tb_font_embedded_count());
    for (size_t index = 0; index < tb_font_catalog_count(catalog); index++)
    {
        tb_font_info listed = {0};
        tb_font_info embedded = {0};
        const void *bytes = NULL;
        const void *embedded_bytes = NULL;
        size_t length = 0;
        size_t embedded_length = 0;
        tb_font_catalog_get_info(catalog, index, &listed);
        tb_font_embedded_get_info(index, &embedded);
        TB_TEST_EXPECT_STRING(listed.file, ==, embedded.file);
        tb_font_catalog_get_bytes(catalog, index, &bytes, &length);
        tb_font_embedded_get_bytes(index, &embedded_bytes, &embedded_length);
        TB_TEST_EXPECT_POINTER(bytes, ==, embedded_bytes);
        TB_TEST_EXPECT_UINT(length, ==, embedded_length);
    }
    tb_font_catalog_destroy(catalog);
    tb_font_catalog_destroy(NULL);
}

/* --- finding fonts ------------------------------------------------------------ */

TB_TEST(font_catalog, finds_the_weight_that_css_would)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    add_font(catalog, "Badger Test", "Light", 300, false);
    add_font(catalog, "Badger Test", "Regular", 400, false);
    add_font(catalog, "Badger Test", "SemiBold", 600, false);
    add_font(catalog, "Badger Test", "ExtraBold", 800, false);

    /* Exact weights. */
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 300, false), ==, 300);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 600, false), ==, 600);
    /* 400 to 500: up to 500 first, then lighter, then heavier. */
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 500, false), ==, 400);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 450, false), ==, 400);
    /* Below 400: lighter first, then heavier. */
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 350, false), ==, 300);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 100, false), ==, 300);
    /* Above 500: heavier first, then lighter. */
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 550, false), ==, 600);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 700, false), ==, 800);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Test", 900, false), ==, 800);
    tb_font_catalog_destroy(catalog);

    /* Within 400 to 500, a weight up to 500 comes before a lighter one, and
     * above 500 comes last, however near. */
    catalog = tb_font_catalog_create(NULL);
    add_font(catalog, "Badger Middle", "Medium", 500, false);
    add_font(catalog, "Badger Middle", "Light", 300, false);
    add_font(catalog, "Badger Middle", "SemiBold", 600, false);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Middle", 400, false), ==, 500);
    tb_font_catalog_destroy(catalog);
    catalog = tb_font_catalog_create(NULL);
    add_font(catalog, "Badger Middle", "Light", 300, false);
    add_font(catalog, "Badger Middle", "SemiBold", 600, false);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Middle", 450, false), ==, 300);
    TB_TEST_EXPECT_UINT(found_weight(catalog, "Badger Middle", 350, false), ==, 300);
    tb_font_catalog_destroy(catalog);
}

TB_TEST(font_catalog, prefers_the_slant_asked_for_and_synthesizes_none)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    size_t regular = add_font(catalog, "Badger Slant", "Regular", 400, false);
    size_t bold_italic = add_font(catalog, "Badger Slant", "Bold Italic", 700, true);
    size_t index = SIZE_MAX;
    /* The slant outranks the weight. */
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, "Badger Slant", 400, true, &index));
    TB_TEST_EXPECT_UINT(index, ==, bold_italic);
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, "Badger Slant", 700, false, &index));
    TB_TEST_EXPECT_UINT(index, ==, regular);
    tb_font_catalog_destroy(catalog);

    /* A family with no italic gives its upright font, as it is. */
    catalog = tb_font_catalog_create(NULL);
    regular = add_font(catalog, "Badger Upright", "Regular", 400, false);
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, "Badger Upright", 400, true, &index));
    TB_TEST_EXPECT_UINT(index, ==, regular);
    tb_font_info info = {0};
    tb_font_catalog_get_info(catalog, index, &info);
    TB_TEST_EXPECT(!info.italic);
    tb_font_catalog_destroy(catalog);
}

TB_TEST(font_catalog, families_match_in_any_case_and_only_whole)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    size_t added = add_font(catalog, "Badger Case", "Regular", 400, false);
    size_t index = SIZE_MAX;
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, "bADGER cASE", 400, false, &index));
    TB_TEST_EXPECT_UINT(index, ==, added);
    /* A prefix, a longer name, or another family is not a match, and leaves the index. */
    index = SIZE_MAX;
    TB_TEST_EXPECT(!tb_font_catalog_find(catalog, "Badger", 400, false, &index));
    TB_TEST_EXPECT(!tb_font_catalog_find(catalog, "Badger Cases", 400, false, &index));
    TB_TEST_EXPECT(!tb_font_catalog_find(catalog, "Badger Cash", 400, false, &index));
    TB_TEST_EXPECT(!tb_font_catalog_find(catalog, "", 400, false, &index));
    TB_TEST_EXPECT_UINT(index, ==, SIZE_MAX);
    tb_font_catalog_destroy(catalog);
}

TB_TEST(font_catalog, a_font_a_game_adds_replaces_an_embedded_one)
{
    /* A game that adds its own "Inter Regular" gets it for the UI role. */
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    size_t added = add_font(catalog, "Inter", "Regular", 400, false);
    size_t index = SIZE_MAX;
    TB_TEST_EXPECT_UINT(added, ==, tb_font_embedded_count());
    TB_TEST_REQUIRE(tb_font_catalog_find_role(catalog, TB_FONT_ROLE_UI, &index));
    TB_TEST_EXPECT_UINT(index, ==, added);
    size_t again = add_font(catalog, "Inter", "Regular", 400, false);
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, "Inter", 400, false, &index));
    TB_TEST_EXPECT_UINT(index, ==, again);
    tb_font_catalog_destroy(catalog);
}

/* --- adding fonts ------------------------------------------------------------- */

TB_TEST(font_catalog, an_added_font_keeps_a_copy_of_its_bytes)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    made_font made = make_font("Badger Copy", "Bold Italic", 700, true);
    size_t index = SIZE_MAX;
    /* The index and the error are optional. */
    TB_TEST_REQUIRE_UINT(
        tb_font_catalog_add(catalog, made.bytes, made.length, TB_FONT_CATEGORY_SANS, NULL, NULL),
        ==, TB_FONT_OK);
    memset(made.bytes, 0, made.length);
    TB_TEST_REQUIRE(tb_font_catalog_find(catalog, "Badger Copy", 700, true, &index));

    tb_font_info info = {0};
    tb_font_catalog_get_info(catalog, index, &info);
    TB_TEST_EXPECT_STRING(info.family, ==, "Badger Copy");
    TB_TEST_EXPECT_STRING(info.style, ==, "Bold Italic");
    TB_TEST_EXPECT_UINT(info.weight, ==, 700);
    TB_TEST_EXPECT(info.italic);
    TB_TEST_EXPECT_UINT(info.category, ==, TB_FONT_CATEGORY_SANS);
    TB_TEST_EXPECT_UINT(info.license, ==, TB_FONT_LICENSE_GAME);
    TB_TEST_EXPECT_STRING(info.file, ==, "");
    TB_TEST_EXPECT(!info.embedded);
    TB_TEST_EXPECT_UINT(info.pixel_size, ==, 0);
    TB_TEST_EXPECT_UINT(info.length, ==, made.length);

    const void *bytes = NULL;
    size_t length = 0;
    tb_font *font = NULL;
    tb_font_catalog_get_bytes(catalog, index, &bytes, &length);
    TB_TEST_EXPECT_UINT(length, ==, made.length);
    TB_TEST_EXPECT_UINT(tb_font_open(bytes, length, NULL, &font, NULL), ==, TB_FONT_OK);
    tb_font_close(font);
    tb_free(made.bytes);
    tb_font_catalog_destroy(catalog);
}

TB_TEST(font_catalog, refuses_a_font_that_does_not_open_or_names_no_family)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    const size_t before = tb_font_catalog_count(catalog);
    const uint8_t junk[] = {'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't', 0, 0};
    size_t index = SIZE_MAX;
    tb_font_error error = {0};
    TB_TEST_EXPECT_UINT(
        tb_font_catalog_add(catalog, junk, sizeof junk, TB_FONT_CATEGORY_OTHER, &index, &error), !=,
        TB_FONT_OK);
    TB_TEST_EXPECT_UINT(error.result, !=, TB_FONT_OK);
    TB_TEST_EXPECT_UINT(index, ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(
        tb_font_catalog_add(catalog, junk, sizeof junk, TB_FONT_CATEGORY_OTHER, NULL, NULL), !=,
        TB_FONT_OK);

    made_font nameless = make_font(NULL, "Regular", 400, false);
    TB_TEST_EXPECT_UINT(tb_font_catalog_add(catalog, nameless.bytes, nameless.length,
                                            TB_FONT_CATEGORY_OTHER, &index, &error),
                        ==, TB_FONT_MALFORMED);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_FONT_MALFORMED);
    TB_TEST_EXPECT_STRING(error.table, ==, "name");
    TB_TEST_EXPECT_STRING(error.reason, ==, "the font names no family");
    TB_TEST_EXPECT_UINT(index, ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_font_catalog_count(catalog), ==, before);
    tb_free(nameless.bytes);
    tb_font_catalog_destroy(catalog);
}

/* --- roles ------------------------------------------------------------------------ */

TB_TEST(font_catalog, a_game_can_change_a_role_at_run_time)
{
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    char family[] = "Badger Pixel";
    tb_font_role_choice choice = {family, TB_FONT_WEIGHT_BOLD, false};
    size_t index = SIZE_MAX;

    /* A role may name a family the catalog does not hold yet. */
    tb_font_catalog_set_role(catalog, TB_FONT_ROLE_PIXEL, &choice);
    TB_TEST_EXPECT(!tb_font_catalog_find_role(catalog, TB_FONT_ROLE_PIXEL, &index));
    size_t added = add_font(catalog, "Badger Pixel", "Bold", 700, false);
    TB_TEST_REQUIRE(tb_font_catalog_find_role(catalog, TB_FONT_ROLE_PIXEL, &index));
    TB_TEST_EXPECT_UINT(index, ==, added);

    /* The catalog keeps its own copy of the family. */
    family[0] = 'X';
    tb_font_role_choice kept = tb_font_catalog_get_role(catalog, TB_FONT_ROLE_PIXEL);
    TB_TEST_EXPECT_STRING(kept.family, ==, "Badger Pixel");
    TB_TEST_EXPECT_UINT(kept.weight, ==, TB_FONT_WEIGHT_BOLD);
    TB_TEST_EXPECT(!kept.italic);

    /* Setting it again replaces it; the other roles keep their defaults. */
    tb_font_role_choice italic = {"Badger Pixel", TB_FONT_WEIGHT_REGULAR, true};
    tb_font_catalog_set_role(catalog, TB_FONT_ROLE_PIXEL, &italic);
    kept = tb_font_catalog_get_role(catalog, TB_FONT_ROLE_PIXEL);
    TB_TEST_EXPECT(kept.italic);
    TB_TEST_EXPECT_STRING(tb_font_catalog_get_role(catalog, TB_FONT_ROLE_UI).family, ==,
                          tb_font_default_role(TB_FONT_ROLE_UI).family);
    tb_font_catalog_destroy(catalog);
}

TB_TEST(font_catalog, names_categories_licenses_and_roles)
{
    TB_TEST_EXPECT_STRING(tb_font_category_name(TB_FONT_CATEGORY_SERIF), ==, "serif");
    TB_TEST_EXPECT_STRING(tb_font_category_name(TB_FONT_CATEGORY_SANS), ==, "sans");
    TB_TEST_EXPECT_STRING(tb_font_category_name(TB_FONT_CATEGORY_MONO), ==, "mono");
    TB_TEST_EXPECT_STRING(tb_font_category_name(TB_FONT_CATEGORY_RETRO), ==, "retro");
    TB_TEST_EXPECT_STRING(tb_font_category_name(TB_FONT_CATEGORY_OTHER), ==, "other");
    TB_TEST_EXPECT_STRING(tb_font_category_name((tb_font_category)99), ==, "unknown");
    TB_TEST_EXPECT_STRING(tb_font_license_name(TB_FONT_LICENSE_OFL_1_1), ==, "OFL-1.1");
    TB_TEST_EXPECT_STRING(tb_font_license_name(TB_FONT_LICENSE_CC_BY_SA_4_0), ==, "CC-BY-SA-4.0");
    TB_TEST_EXPECT_STRING(tb_font_license_name(TB_FONT_LICENSE_GAME), ==, "game");
    TB_TEST_EXPECT_STRING(tb_font_license_name((tb_font_license)99), ==, "unknown");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_UI), ==, "ui");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_UI_BOLD), ==, "ui-bold");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_BODY), ==, "body");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_HEADING), ==, "heading");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_MONOSPACE), ==, "monospace");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_PIXEL), ==, "pixel");
    TB_TEST_EXPECT_STRING(tb_font_role_name(TB_FONT_ROLE_COUNT), ==, "unknown");
}

static void get_info_past_the_end(void *context)
{
    tb_font_info info = {0};
    tb_font_catalog_get_info(context, tb_font_catalog_count(context), &info);
}

static void get_embedded_bytes_past_the_end(void *context)
{
    const void *bytes = NULL;
    size_t length = 0;
    (void)context;
    tb_font_embedded_get_bytes(tb_font_embedded_count(), &bytes, &length);
}

static void get_default_of_no_role(void *context)
{
    (void)context;
    (void)tb_font_default_role(TB_FONT_ROLE_COUNT);
}

static void get_no_role(void *context)
{
    (void)tb_font_catalog_get_role(context, TB_FONT_ROLE_COUNT);
}

TB_TEST(font_catalog, indexes_and_roles_out_of_range_are_contract_violations)
{
    if (!tb_assert_library_enabled())
    {
        TB_TEST_SKIP("the library is built without assertions");
    }
    tb_font_catalog *catalog = tb_font_catalog_create(NULL);
    TB_TEST_EXPECT_TRAP(get_info_past_the_end, catalog, "tb_font_catalog_count");
    TB_TEST_EXPECT_TRAP(get_embedded_bytes_past_the_end, NULL, "tb_font_embedded_count");
    TB_TEST_EXPECT_TRAP(get_default_of_no_role, NULL, "a role");
    TB_TEST_EXPECT_TRAP(get_no_role, catalog, "a role");
    tb_font_catalog_destroy(catalog);
}
