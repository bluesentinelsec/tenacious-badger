/* Theme files: every kind of value read from JSON, all or nothing; errors
 * that say where and why, for text that is not JSON, of another version,
 * or not a theme; damaged bytes refused; files read through a VFS; and a
 * watched file loaded again as it changes, a file in memory standing for
 * one an author edits. */
#include "badger/str.h"
#include "badger/vfs.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

/* The file's name in the VFS. */
#define THEME_PATH "ui/theme.json"
/* Updates of a sixtieth of a second that pass a watch's interval of a tenth. */
#define UPDATES_PAST_INTERVAL 8

static bool same_color(tb_vec4 a, tb_vec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

static tb_theme_file_result load(tb_theme *theme, const char *text, tb_theme_file_error *error)
{
    return tb_theme_load(theme, text, tb_str_length(text), "theme.json", error);
}

/* The theme only keeps an image's pointer; these tests never draw it. */
static const int stand_in = 0;
#define STAND_IN_IMAGE ((const tb_image *)(const void *)&stand_in)

static const char full_theme[] =
    "{\n"
    "  // comments and trailing commas are allowed in files people edit\n"
    "  \"format\": \"badger-theme\",\n"
    "  \"version\": 1,\n"
    "  \"name\": \"Test\",\n"
    "  \"styles\": {\n"
    "    \".\": {\"text_color\": \"#ff0000\", \"font\": {\"role\": \"body\"}},\n"
    "    \"Button\": {\n"
    "      \"background\": \"#00ff0080\",\n"
    "      \"background:hovered+pressed\": \"#0000ff\",\n"
    "      \"padding\": [6, 3],\n"
    "      \"radius\": 4,\n"
    "      \"focus_width:focused\": 2,\n"
    "      \"glow\": \"#FFFFFF\",\n"
    "      \"background_image:disabled\": {\"image\": \"/Game/UI/button.png\", \"insets\": [4, "
    "5],\n"
    "          \"source\": [0, 0, 16, 16], \"scale\": 2, \"whole_pixels\": true,\n"
    "          \"hollow\": false, \"tint\": \"#808080\"},\n"
    "    },\n"
    "    \"Title.Label\": {\"font\": {\"role\": \"heading\", \"weight\": 500, \"italic\": true,\n"
    "                               \"size\": 20}, \"margin\": [1, 2, 3, 4], \"gap\": 5},\n"
    "  },\n"
    "}\n";

/* Every kind of value: colors, numbers, edges, fonts, and images, by
 * standard names and elements' shapes, and states joined by "+". */
TB_TEST(gui_theme_file, every_kind_of_value_loads)
{
    tb_theme *const theme = tb_theme_create();
    tb_theme_file_error error = {0};
    tb_style_value value = {0};

    tb_theme_add_image(theme, "/Game/UI/button.png", STAND_IN_IMAGE);
    TB_TEST_REQUIRE_UINT(load(theme, full_theme, &error), ==, TB_THEME_FILE_OK);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_THEME_FILE_OK);

    TB_TEST_REQUIRE(tb_theme_get(theme, ".", "text_color", 0, &value));
    TB_TEST_EXPECT(same_color(value.color, (tb_vec4){1, 0, 0, 1}));
    TB_TEST_REQUIRE(tb_theme_get(theme, ".", "font", 0, &value));
    TB_TEST_EXPECT_UINT(value.font.role, ==, TB_FONT_ROLE_BODY);
    TB_TEST_EXPECT_UINT(value.font.weight, ==, TB_FONT_WEIGHT_REGULAR);
    TB_TEST_EXPECT_FLOAT(value.font.size, ==, tb_font_default_role(TB_FONT_ROLE_BODY).size);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "background", 0, &value));
    TB_TEST_EXPECT_FLOAT_NEAR(value.color.w, 128.0f / 255.0f, 1e-6f);
    TB_TEST_REQUIRE(
        tb_theme_get(theme, "Button", "background", TB_WIDGET_HOVERED | TB_WIDGET_PRESSED, &value));
    TB_TEST_EXPECT(same_color(value.color, (tb_vec4){0, 0, 1, 1}));
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "padding", 0, &value));
    TB_TEST_EXPECT_FLOAT(value.edges.left, ==, 6.0f);
    TB_TEST_EXPECT_FLOAT(value.edges.bottom, ==, 3.0f);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "focus_width", TB_WIDGET_FOCUSED, &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 2.0f);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "glow", 0, &value));
    TB_TEST_EXPECT(same_color(value.color, (tb_vec4){1, 1, 1, 1}));
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "background_image", TB_WIDGET_DISABLED, &value));
    TB_TEST_EXPECT_POINTER(value.image.slice.image, ==, STAND_IN_IMAGE);
    TB_TEST_EXPECT_UINT(value.image.slice.left, ==, 4);
    TB_TEST_EXPECT_UINT(value.image.slice.top, ==, 5);
    TB_TEST_EXPECT_UINT(value.image.slice.right, ==, 4);
    TB_TEST_EXPECT_UINT(value.image.slice.bottom, ==, 5);
    TB_TEST_EXPECT_UINT(value.image.slice.source.width, ==, 16);
    TB_TEST_EXPECT_FLOAT(value.image.slice.scale, ==, 2.0f);
    TB_TEST_EXPECT(value.image.slice.whole_pixels);
    TB_TEST_EXPECT(!value.image.slice.hollow);
    TB_TEST_EXPECT_FLOAT_NEAR(value.image.tint.x, 128.0f / 255.0f, 1e-6f);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Title.Label", "font", 0, &value));
    TB_TEST_EXPECT_UINT(value.font.role, ==, TB_FONT_ROLE_HEADING);
    TB_TEST_EXPECT_UINT(value.font.weight, ==, 500);
    TB_TEST_EXPECT(value.font.italic);
    TB_TEST_EXPECT_FLOAT(value.font.size, ==, 20.0f);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Title.Label", "margin", 0, &value));
    TB_TEST_EXPECT_UINT(value.type, ==, TB_STYLE_EDGES);
    TB_TEST_EXPECT_FLOAT(value.edges.bottom, ==, 4.0f);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Title.Label", "gap", 0, &value));
    TB_TEST_EXPECT_UINT(value.type, ==, TB_STYLE_NUMBER);

    /* Loading again replaces what was there, all of it. */
    TB_TEST_REQUIRE_UINT(
        load(theme, "{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {}}", &error), ==,
        TB_THEME_FILE_OK);
    TB_TEST_EXPECT(!tb_theme_get(theme, "Button", "radius", 0, &value));
    tb_theme_destroy(theme);
}

/* A file loaded draws widgets: a GUI drawn by the theme sees its values. */
TB_TEST(gui_theme_file, a_loaded_theme_draws_widgets)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();

    gui_fixture_open(&fixture, 1.0f);
    tb_theme_add_image(theme, "/Game/UI/button.png", STAND_IN_IMAGE);
    TB_TEST_REQUIRE_UINT(load(theme, full_theme, NULL), ==, TB_THEME_FILE_OK);
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget button = tb_button(tb_gui_root(fixture.gui), &(tb_button_desc){.text = "OK"});
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->text_color, (tb_vec4){1, 0, 0, 1}));
    TB_TEST_EXPECT_FLOAT(tb_style_in(button, 0)->radius, ==, 4.0f);
    TB_TEST_EXPECT(tb_style_in(button, TB_WIDGET_DISABLED)->image.slice.image == STAND_IN_IMAGE);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* Text that is not JSON says the line and column it fails at; the theme
 * is as it was. */
TB_TEST(gui_theme_file, syntax_errors_say_where)
{
    tb_theme *const theme = tb_theme_create();
    tb_theme_file_error error = {0};
    tb_style_value value = {0};

    TB_TEST_REQUIRE(tb_theme_set_number(theme, "Button", "radius", 0, 7.0f));
    TB_TEST_EXPECT_UINT(
        load(theme, "{\n  \"format\": \"badger-theme\",\n  \"version\" 1\n}", &error), ==,
        TB_THEME_FILE_SYNTAX);
    TB_TEST_EXPECT_UINT(error.result, ==, TB_THEME_FILE_SYNTAX);
    TB_TEST_EXPECT_UINT(error.line, ==, 3);
    TB_TEST_EXPECT_UINT(error.column, >, 1);
    TB_TEST_EXPECT(strstr(error.message, "theme.json:3:") != NULL);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "radius", 0, &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 7.0f);
    tb_theme_destroy(theme);
}

/* What a document must not be, each refused with the path where it went
 * wrong, and the theme left as it was. */
TB_TEST(gui_theme_file, documents_that_are_not_themes_are_refused)
{
    static const struct
    {
        const char *text;
        tb_theme_file_result result;
        const char *path;
    } cases[] = {
        {"[]", TB_THEME_FILE_INVALID, ""},
        {"{\"version\": 1, \"styles\": {}}", TB_THEME_FILE_INVALID, "format"},
        {"{\"format\": \"badger-gui\", \"version\": 1, \"styles\": {}}", TB_THEME_FILE_INVALID,
         "format"},
        {"{\"format\": \"badger-theme\", \"styles\": {}}", TB_THEME_FILE_INVALID, "version"},
        {"{\"format\": \"badger-theme\", \"version\": 2, \"styles\": {}}", TB_THEME_FILE_VERSION,
         "version"},
        {"{\"format\": \"badger-theme\", \"version\": 1}", TB_THEME_FILE_INVALID, "styles"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {}, \"extra\": 1}",
         TB_THEME_FILE_INVALID, "extra"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"name\": 3, \"styles\": {}}",
         TB_THEME_FILE_INVALID, "name"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": 3}}",
         TB_THEME_FILE_INVALID, "styles.Button"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background\": \"red\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background\": \"#12345g\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background:glowing\": \"#123456\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background:glowing"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background:\": \"#123456\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background:"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\":hovered\": \"#123456\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.:hovered"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"padding\": [1, 2, 3]}}}",
         TB_THEME_FILE_INVALID, "styles.Button.padding"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"padding\": -1}}}",
         TB_THEME_FILE_INVALID, "styles.Button.padding"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"radius\": -2}}}",
         TB_THEME_FILE_INVALID, "styles.Button.radius"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"radius\": 1e300}}}",
         TB_THEME_FILE_INVALID, "styles.Button.radius"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"font\": {\"role\": \"fancy\"}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.font"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"font\": {\"role\": \"ui\", \"weight\": 2000}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.font"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"font\": {\"role\": \"ui\", \"italic\": 1}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.font"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"font\": {\"role\": \"ui\", \"size\": -1}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.font"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"font\": {\"role\": \"ui\", \"bold\": true}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.font"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"font\": \"heading\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.font"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/missing.png\"}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"insets\": -1}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"insets\": [1, 2, 3]}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"source\": [1, 2]}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"scale\": 0}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"hollow\": 1}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"tint\": 1}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": \"/Game/UI/button.png\", \"sheen\": 1}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": {\"image\": 3}}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"background_image\": \"#ffffff\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.background_image"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"glow\": true}}}",
         TB_THEME_FILE_INVALID, "styles.Button.glow"},
        {"{\"format\": \"badger-theme\", \"version\": 1, \"styles\": {\"Button\": "
         "{\"radius\": \"#ffffff\"}}}",
         TB_THEME_FILE_INVALID, "styles.Button.radius"},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
    {
        tb_theme *const theme = tb_theme_create();
        tb_theme_file_error error = {0};
        tb_style_value value = {0};

        TB_TEST_CONTEXT("case %zu: %s", index, cases[index].text);
        tb_theme_add_image(theme, "/Game/UI/button.png", STAND_IN_IMAGE);
        TB_TEST_REQUIRE(tb_theme_set_number(theme, "Button", "radius", 0, 7.0f));
        TB_TEST_EXPECT_UINT(load(theme, cases[index].text, &error), ==, cases[index].result);
        TB_TEST_EXPECT_UINT(error.result, ==, cases[index].result);
        TB_TEST_EXPECT_STRING(error.path, ==, cases[index].path);
        TB_TEST_EXPECT(strstr(error.message, "theme.json") == error.message);
        /* All or nothing: the theme is as it was. */
        TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "radius", 0, &value));
        TB_TEST_EXPECT_FLOAT(value.number, ==, 7.0f);
        tb_theme_destroy(theme);
    }
}

/* Damaged bytes: cut short, of noise, of zeros, or nested past the
 * reader's limit; each refused, never read past its end. */
TB_TEST(gui_theme_file, damaged_input_is_refused)
{
    tb_theme *const theme = tb_theme_create();
    tb_theme_file_error error = {0};
    char noise[256] = {0};
    char deep[1200] = {0};

    for (size_t cut = 0; cut + 1 < sizeof(full_theme) - 1; cut += 7)
    {
        TB_TEST_CONTEXT("cut at %zu", cut);
        TB_TEST_EXPECT_UINT(tb_theme_load(theme, full_theme, cut, NULL, &error), !=,
                            TB_THEME_FILE_OK);
    }
    for (size_t index = 0; index < sizeof(noise); index++)
    {
        noise[index] = (char)((index * 131u + 17u) & 0xFFu);
    }
    TB_TEST_EXPECT_UINT(tb_theme_load(theme, noise, sizeof(noise), NULL, &error), ==,
                        TB_THEME_FILE_SYNTAX);
    memset(noise, 0, sizeof(noise));
    TB_TEST_EXPECT_UINT(tb_theme_load(theme, noise, sizeof(noise), NULL, &error), ==,
                        TB_THEME_FILE_SYNTAX);
    for (size_t index = 0; index < sizeof(deep) - 1; index++)
    {
        deep[index] = '[';
    }
    TB_TEST_EXPECT_UINT(tb_theme_load(theme, deep, sizeof(deep) - 1, NULL, &error), ==,
                        TB_THEME_FILE_SYNTAX);
    TB_TEST_EXPECT_UINT(tb_theme_load(theme, NULL, 0, NULL, &error), ==, TB_THEME_FILE_SYNTAX);
    TB_TEST_EXPECT(strstr(error.message, "theme") == error.message);
    tb_theme_destroy(theme);
}

/* ---------------------------------------------------------------------------
 * Files through a VFS.
 * ------------------------------------------------------------------------- */

/* A file the test changes: its bytes, whether it is there, and whether
 * reading it fails. */
typedef struct memory_file
{
    const char *bytes;
    size_t length;
    bool missing;
    bool failing;
} memory_file;

static bool memory_is_theme(const char *name, size_t length)
{
    return length == tb_str_length(THEME_PATH) && memcmp(name, THEME_PATH, length) == 0;
}

static tb_vfs_result memory_open(void *state, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream)
{
    memory_file *const file = state;

    if (file->missing || !memory_is_theme(name, length))
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

    if (file->missing || !memory_is_theme(name, length))
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

static tb_vfs *memory_vfs(memory_file *file)
{
    tb_vfs *const vfs = tb_vfs_create(NULL);
    TB_TEST_REQUIRE_UINT(tb_vfs_mount(vfs, "", 0, (tb_vfs_source){&memory_functions, file}, 0), ==,
                         TB_VFS_OK);
    return vfs;
}

static void set_bytes(memory_file *file, const char *bytes)
{
    file->bytes = bytes;
    file->length = tb_str_length(bytes);
}

static const char theme_red[] = "{\"format\": \"badger-theme\", \"version\": 1, \"styles\": "
                                "{\"Button\": {\"background\": \"#ff0000\"}}}";
static const char theme_green[] = "{\"format\": \"badger-theme\", \"version\": 1, \"styles\": "
                                  "{\"Button\": {\"background\": \"#00ff00\"}}}";
static const char theme_broken[] = "{\"format\": \"badger-theme\", \"version\": 1, \"styles\": "
                                   "{\"Button\": {\"background\": 12}}}";

/* A file is read through a VFS; one missing, or that fails, says so. */
TB_TEST(gui_theme_file, files_load_through_a_vfs)
{
    memory_file file = {0};
    tb_vfs *const vfs = memory_vfs(&file);
    tb_theme *const theme = tb_theme_create();
    tb_theme_file_error error = {0};
    tb_style_value value = {0};

    set_bytes(&file, theme_red);
    TB_TEST_REQUIRE_UINT(tb_theme_load_file(theme, vfs, THEME_PATH, &error), ==, TB_THEME_FILE_OK);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "background", 0, &value));
    TB_TEST_EXPECT(same_color(value.color, (tb_vec4){1, 0, 0, 1}));
    file.missing = true;
    TB_TEST_EXPECT_UINT(tb_theme_load_file(theme, vfs, THEME_PATH, &error), ==,
                        TB_THEME_FILE_NOT_FOUND);
    TB_TEST_EXPECT(strstr(error.message, THEME_PATH) != NULL);
    file.missing = false;
    file.failing = true;
    TB_TEST_EXPECT_UINT(tb_theme_load_file(theme, vfs, THEME_PATH, NULL), ==,
                        TB_THEME_FILE_INPUT_FAILED);
    file.failing = false;
    set_bytes(&file, theme_broken);
    TB_TEST_EXPECT_UINT(tb_theme_load_file(theme, vfs, THEME_PATH, &error), ==,
                        TB_THEME_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.path, ==, "styles.Button.background");
    tb_theme_destroy(theme);
    tb_vfs_destroy(vfs);
}

/* What reloads a callback was told of. */
typedef struct reloads
{
    int loaded;
    int failed;
    char path[TB_THEME_FILE_ERROR_CAPACITY];
} reloads;

static void on_reload(tb_theme *theme, const tb_theme_file_error *error, void *user)
{
    reloads *const seen = user;

    (void)theme;
    if (error == NULL)
    {
        seen->loaded++;
        return;
    }
    seen->failed++;
    memcpy(seen->path, error->path, sizeof(seen->path));
}

static void pass_interval(gui_fixture *fixture)
{
    for (int update = 0; update < UPDATES_PAST_INTERVAL; update++)
    {
        gui_update(fixture);
    }
}

/* A watched file is loaded again when its bytes change, and the GUI draws
 * by it at once; a broken file leaves the theme as it was and says why; a
 * file that cannot be read for a moment is looked at again. */
TB_TEST(gui_theme_file, watched_files_load_again_as_they_change)
{
    gui_fixture fixture = {0};
    memory_file file = {0};
    tb_vfs *const vfs = memory_vfs(&file);
    tb_theme *const theme = tb_theme_create();
    reloads seen = {0};
    const tb_theme_watch_options options = {.interval = 0.1, .callback = on_reload, .user = &seen};

    gui_fixture_open(&fixture, 1.0f);
    set_bytes(&file, theme_red);
    TB_TEST_REQUIRE_UINT(tb_theme_load_file(theme, vfs, THEME_PATH, NULL), ==, TB_THEME_FILE_OK);
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget button = tb_button(tb_gui_root(fixture.gui), &(tb_button_desc){.text = "OK"});
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    TB_TEST_REQUIRE(tb_gui_watch_theme(fixture.gui, theme, vfs, THEME_PATH, &options));
    TB_TEST_EXPECT(!tb_gui_watch_theme(fixture.gui, theme, vfs, THEME_PATH, &options));

    /* Unchanged: nothing is loaded. */
    pass_interval(&fixture);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 0);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, (tb_vec4){1, 0, 0, 1}));

    set_bytes(&file, theme_green);
    pass_interval(&fixture);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 1);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, (tb_vec4){0, 1, 0, 1}));

    set_bytes(&file, theme_broken);
    pass_interval(&fixture);
    TB_TEST_EXPECT_INT(seen.failed, ==, 1);
    TB_TEST_EXPECT_STRING(seen.path, ==, "styles.Button.background");
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, (tb_vec4){0, 1, 0, 1}));

    /* Missing for a moment, as while an editor replaces it. */
    file.missing = true;
    pass_interval(&fixture);
    TB_TEST_EXPECT_INT(seen.failed, ==, 1);
    file.missing = false;
    set_bytes(&file, theme_red);
    pass_interval(&fixture);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 2);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, (tb_vec4){1, 0, 0, 1}));

    TB_TEST_EXPECT(tb_gui_unwatch_theme(fixture.gui, theme));
    TB_TEST_EXPECT(!tb_gui_unwatch_theme(fixture.gui, theme));
    set_bytes(&file, theme_green);
    pass_interval(&fixture);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 2);
    /* A watch left in place ends with its GUI. */
    TB_TEST_REQUIRE(tb_gui_watch_theme(fixture.gui, theme, vfs, THEME_PATH, NULL));
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
    tb_vfs_destroy(vfs);
}

/* A callback that stops watching while it is called. */
static void unwatch_on_reload(tb_theme *theme, const tb_theme_file_error *error, void *user)
{
    gui_fixture *const fixture = user;

    (void)error;
    (void)tb_gui_unwatch_theme(fixture->gui, theme);
}

TB_TEST(gui_theme_file, a_callback_may_stop_watching)
{
    gui_fixture fixture = {0};
    memory_file file = {0};
    tb_vfs *const vfs = memory_vfs(&file);
    tb_theme *const theme = tb_theme_create();
    const tb_theme_watch_options options = {
        .interval = 0.1, .callback = unwatch_on_reload, .user = &fixture};

    gui_fixture_open(&fixture, 1.0f);
    set_bytes(&file, theme_red);
    TB_TEST_REQUIRE(tb_gui_watch_theme(fixture.gui, theme, vfs, THEME_PATH, &options));
    set_bytes(&file, theme_green);
    pass_interval(&fixture);
    TB_TEST_EXPECT(!tb_gui_unwatch_theme(fixture.gui, theme));
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
    tb_vfs_destroy(vfs);
}
