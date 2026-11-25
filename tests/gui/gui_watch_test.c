/* Layout files read through the VFS, and loaded again as they change: a
 * file in memory, mounted as a VFS source, stands for one an author edits. */
#include "badger/fmt.h"
#include "badger/str.h"
#include "badger/vfs.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

/* The file's name in the VFS. */
#define PANEL_PATH "ui/panel.json"
/* Updates of a sixtieth of a second that pass a watch's interval of a tenth. */
#define UPDATES_PAST_INTERVAL 8

/* A file the test changes: its bytes, whether it is there, and whether
 * reading it fails. */
typedef struct memory_file
{
    const char *bytes;
    size_t length;
    bool missing;
    bool failing;
    int opens;
} memory_file;

static bool memory_is_panel(const char *name, size_t length)
{
    return length == tb_str_length(PANEL_PATH) && memcmp(name, PANEL_PATH, length) == 0;
}

static tb_vfs_result memory_open(void *state, const char *name, size_t length,
                                 const tb_allocator *allocator, tb_stream *stream)
{
    memory_file *const file = state;

    if (file->missing || !memory_is_panel(name, length))
    {
        return TB_VFS_NOT_FOUND;
    }
    if (file->failing)
    {
        return TB_VFS_INPUT_FAILED;
    }
    file->opens++;
    tb_stream_open_memory(file->bytes, file->length, allocator, stream);
    return TB_VFS_OK;
}

static tb_vfs_result memory_query(void *state, const char *name, size_t length, tb_vfs_info *info)
{
    const memory_file *const file = state;

    if (file->missing || !memory_is_panel(name, length))
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

static const char panel_hello[] =
    "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"frame\", \"name\": "
    "\"panel\", \"children\": [{\"kind\": \"label\", \"name\": \"text\", \"properties\": "
    "{\"text\": \"Hello\"}}]}}";
static const char panel_changed[] =
    "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"frame\", \"name\": "
    "\"panel\", \"children\": [{\"kind\": \"label\", \"name\": \"text\", \"properties\": "
    "{\"text\": \"Changed\"}}]}}";
static const char panel_broken[] =
    "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"frame\", \"name\": "
    "\"panel\", \"children\": [{\"kind\": \"label\", \"properties\": {\"colour\": 1}}]}}";
static const char panel_again[] =
    "{\"format\": \"badger-gui\", \"version\": 1, \"root\": {\"kind\": \"frame\", \"name\": "
    "\"panel\", \"children\": [{\"kind\": \"label\", \"name\": \"text\", \"properties\": "
    "{\"text\": \"Again\"}}]}}";

/* What reloads the callback was told of. */
typedef struct reloads
{
    int loaded;
    int failed;
    tb_widget widget;
    char reason[64];
} reloads;

static void on_reload(tb_widget widget, const tb_gui_file_error *error, void *user)
{
    reloads *const seen = user;

    seen->widget = widget;
    if (error == NULL)
    {
        seen->loaded++;
        return;
    }
    seen->failed++;
    (void)tb_fmt_buffer(seen->reason, sizeof(seen->reason), "%s", error->reason);
}

static void run_updates(gui_fixture *fixture, int count)
{
    for (int update = 0; update < count; update++)
    {
        gui_update(fixture);
    }
}

static tb_text label_text(tb_widget panel)
{
    return tb_widget_text(tb_widget_find(panel, "text"));
}

TB_TEST(gui_watch, a_file_is_read_through_the_vfs)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    memory_file file = {0};
    tb_vfs *const vfs = memory_vfs(&file);
    tb_widget panel = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};

    set_bytes(&file, panel_hello);
    TB_TEST_REQUIRE_UINT(
        tb_gui_load_file(tb_gui_root(fixture.gui), vfs, PANEL_PATH, &panel, &error), ==,
        TB_GUI_FILE_OK);
    TB_TEST_EXPECT(tb_text_equal(label_text(panel), TB_TEXT("Hello")));

    /* A file's errors name it. */
    set_bytes(&file, panel_broken);
    TB_TEST_EXPECT_UINT(tb_gui_load_file(tb_gui_root(fixture.gui), vfs, PANEL_PATH, &panel, &error),
                        ==, TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT(tb_str_starts_with(error.message, PANEL_PATH ":1:"));

    file.missing = true;
    TB_TEST_EXPECT_UINT(tb_gui_load_file(tb_gui_root(fixture.gui), vfs, PANEL_PATH, &panel, &error),
                        ==, TB_GUI_FILE_NOT_FOUND);
    TB_TEST_EXPECT(!tb_widget_alive(panel));
    TB_TEST_EXPECT_UINT(error.line, ==, 0);
    TB_TEST_EXPECT_STRING(error.message, ==, PANEL_PATH ": no such file");
    file.missing = false;
    file.failing = true;
    TB_TEST_EXPECT_UINT(tb_gui_load_file(tb_gui_root(fixture.gui), vfs, PANEL_PATH, &panel, NULL),
                        ==, TB_GUI_FILE_INPUT_FAILED);
    tb_vfs_destroy(vfs);
    gui_fixture_close(&fixture);
}

/* An author edits a file while the game runs: the widget is built again in
 * its place, a file that fails keeps the last good one, and a file missing
 * for a moment is waited for. */
TB_TEST(gui_watch, a_watched_file_is_loaded_again_when_it_changes)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    memory_file file = {0};
    tb_vfs *const vfs = memory_vfs(&file);
    reloads seen = {0};
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget before = tb_label(root, NULL);
    tb_widget panel = TB_WIDGET_NONE;

    set_bytes(&file, panel_hello);
    TB_TEST_REQUIRE_UINT(tb_gui_load_file(root, vfs, PANEL_PATH, &panel, NULL), ==, TB_GUI_FILE_OK);
    const tb_widget after = tb_label(root, NULL);
    TB_TEST_REQUIRE(tb_gui_watch(
        panel, vfs, PANEL_PATH,
        &(tb_gui_watch_options){.interval = 0.1, .callback = on_reload, .user = &seen}));

    /* Unchanged, it is looked at, and nothing happens. */
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_EXPECT_INT(seen.loaded + seen.failed, ==, 0);
    TB_TEST_EXPECT(tb_widget_alive(panel));

    /* Changed, it is built again between the same siblings. */
    set_bytes(&file, panel_changed);
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_REQUIRE_INT(seen.loaded, ==, 1);
    TB_TEST_EXPECT(!tb_widget_alive(panel));
    panel = seen.widget;
    TB_TEST_EXPECT(tb_text_equal(label_text(panel), TB_TEXT("Changed")));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_next_sibling(before), panel));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_next_sibling(panel), after));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_find(root, "panel"), panel));

    /* Broken, the last good widget stays, and the callback says why, once. */
    set_bytes(&file, panel_broken);
    run_updates(&fixture, UPDATES_PAST_INTERVAL * 3);
    TB_TEST_EXPECT_INT(seen.failed, ==, 1);
    TB_TEST_EXPECT_STRING(seen.reason, ==, "unknown property");
    TB_TEST_EXPECT(tb_widget_equal(seen.widget, panel));
    TB_TEST_EXPECT(tb_text_equal(label_text(panel), TB_TEXT("Changed")));

    /* Gone for a moment, as an editor replaces it, then back, mended. */
    file.missing = true;
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_EXPECT_INT(seen.loaded + seen.failed, ==, 2);
    file.missing = false;
    set_bytes(&file, panel_again);
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_REQUIRE_INT(seen.loaded, ==, 2);
    panel = seen.widget;
    TB_TEST_EXPECT(tb_text_equal(label_text(panel), TB_TEXT("Again")));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_next_sibling(before), panel));

    /* The GUI destroyed with a watch on releases it. */
    tb_vfs *const kept = vfs;
    gui_fixture_close(&fixture);
    tb_vfs_destroy(kept);
}

TB_TEST(gui_watch, a_watch_ends_with_its_widget_or_when_asked)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    memory_file file = {0};
    tb_vfs *const vfs = memory_vfs(&file);
    reloads seen = {0};
    const tb_widget root = tb_gui_root(fixture.gui);
    tb_widget panel = TB_WIDGET_NONE;
    const tb_gui_watch_options options = {.interval = 0.1, .callback = on_reload, .user = &seen};

    set_bytes(&file, panel_hello);
    TB_TEST_REQUIRE_UINT(tb_gui_load_file(root, vfs, PANEL_PATH, &panel, NULL), ==, TB_GUI_FILE_OK);

    /* Neither the root nor a stale widget is watched. */
    TB_TEST_EXPECT(!tb_gui_watch(root, vfs, PANEL_PATH, &options));
    TB_TEST_EXPECT(!tb_gui_unwatch(root));
    TB_TEST_EXPECT(!tb_gui_unwatch(TB_WIDGET_NONE));

    /* Unwatched, a change is not loaded. */
    TB_TEST_REQUIRE(tb_gui_watch(panel, vfs, PANEL_PATH, &options));
    TB_TEST_EXPECT(tb_gui_unwatch(panel));
    TB_TEST_EXPECT(!tb_gui_unwatch(panel));
    set_bytes(&file, panel_changed);
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 0);
    TB_TEST_EXPECT(tb_widget_alive(panel));

    /* Watched by default options, and destroyed: the watch ends with it. */
    TB_TEST_REQUIRE(tb_gui_watch(panel, vfs, PANEL_PATH, NULL));
    TB_TEST_REQUIRE(tb_gui_watch(panel, vfs, PANEL_PATH, &options));
    tb_widget_destroy(panel);
    set_bytes(&file, panel_hello);
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 0);
    TB_TEST_EXPECT(!tb_gui_unwatch(panel));
    TB_TEST_EXPECT_UINT(tb_widget_child_count(root), ==, 0);

    /* A file that cannot be read when it is first watched is loaded when it
     * can be. */
    TB_TEST_REQUIRE_UINT(tb_gui_load_file(root, vfs, PANEL_PATH, &panel, NULL), ==, TB_GUI_FILE_OK);
    file.failing = true;
    TB_TEST_REQUIRE(tb_gui_watch(panel, vfs, PANEL_PATH, &options));
    file.failing = false;
    run_updates(&fixture, UPDATES_PAST_INTERVAL);
    TB_TEST_EXPECT_INT(seen.loaded, ==, 1);
    gui_fixture_close(&fixture);
    tb_vfs_destroy(vfs);
}
