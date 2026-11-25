/* Names: widgets found by the path of their names, and commands a layout
 * file connects by name. */
#include "badger/memory.h"
#include "badger/str.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>
#include <string.h>

static tb_widget named_frame(tb_widget parent, const char *name)
{
    const tb_widget frame = tb_frame(parent, NULL);
    tb_widget_set_name(frame, name);
    return frame;
}

/* "options/video/apply" is the first child named "options", its first
 * child named "video", and its first named "apply", as Godot's paths are. */
TB_TEST(gui_names, a_path_of_names_finds_a_widget)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget options = named_frame(root, "options");
    (void)tb_frame(options, NULL);
    const tb_widget video = named_frame(options, "video");
    const tb_widget apply = named_frame(video, "apply");
    const tb_widget first = named_frame(options, "twin");
    (void)named_frame(options, "twin");

    TB_TEST_EXPECT(tb_widget_equal(tb_widget_find(root, "options/video/apply"), apply));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_find(options, "video/apply"), apply));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_find(video, ""), video));
    /* Of two of one name, the first in the tree's order. */
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_find(options, "twin"), first));
    /* A path is of children: a grandchild is not found by its name alone. */
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "video")));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "options/missing")));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "options//apply")));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "/options")));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "options/")));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "options/video/apply/more")));

    tb_widget_destroy(video);
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(video, "")));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_find(root, "options/video/apply")));
    gui_fixture_close(&fixture);
}

/* A widget's path is what finds it from the root, and only a path made of
 * names that find it is given. */
TB_TEST(gui_names, a_widget_knows_its_path_from_the_root)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget options = named_frame(root, "options");
    const tb_widget video = named_frame(options, "video");
    const tb_widget apply = named_frame(video, "apply");
    const tb_widget nameless = tb_frame(options, NULL);
    const tb_widget inside = named_frame(nameless, "inside");
    const tb_widget slashed = named_frame(options, "a/b");
    char buffer[64] = {0};
    size_t length = 0;

    TB_TEST_REQUIRE(tb_widget_path(apply, buffer, sizeof(buffer), &length));
    TB_TEST_EXPECT_STRING(buffer, ==, "options/video/apply");
    TB_TEST_EXPECT_UINT(length, ==, 19);
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_find(root, buffer), apply));
    TB_TEST_REQUIRE(tb_widget_path(options, buffer, sizeof(buffer), NULL));
    TB_TEST_EXPECT_STRING(buffer, ==, "options");
    TB_TEST_REQUIRE(tb_widget_path(root, buffer, sizeof(buffer), &length));
    TB_TEST_EXPECT_STRING(buffer, ==, "");
    TB_TEST_EXPECT_UINT(length, ==, 0);

    /* Too small a buffer says how large one must be; none at all measures. */
    TB_TEST_EXPECT(!tb_widget_path(apply, buffer, 19, &length));
    TB_TEST_EXPECT_UINT(length, ==, 19);
    TB_TEST_EXPECT(!tb_widget_path(apply, NULL, 0, &length));
    TB_TEST_EXPECT_UINT(length, ==, 19);

    /* A widget on the way with no name, or a slash in its name, has no path. */
    TB_TEST_EXPECT(!tb_widget_path(inside, buffer, sizeof(buffer), &length));
    TB_TEST_EXPECT_UINT(length, ==, 0);
    TB_TEST_EXPECT(!tb_widget_path(slashed, buffer, sizeof(buffer), &length));
    tb_widget_destroy(apply);
    TB_TEST_EXPECT(!tb_widget_path(apply, buffer, sizeof(buffer), &length));
    gui_fixture_close(&fixture);
}

typedef struct applied
{
    int count;
    tb_widget widget;
} applied;

static void on_apply(const tb_gui_event *event, void *user)
{
    applied *const seen = user;
    seen->count++;
    seen->widget = event->widget;
}

static void on_other(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)user;
}

/* A file names a command; the game registers the function under the name,
 * and the button built from the file calls it. */
TB_TEST(gui_names, a_file_connects_a_command_by_its_name)
{
    static const char text[] = "{\"format\": \"badger-gui\", \"version\": 1, \"root\": "
                               "{\"kind\": \"button\", \"name\": \"apply\", \"properties\": "
                               "{\"text\": \"Apply\", \"command\": \"apply_settings\"}}}";
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    applied seen = {0};
    tb_widget button = TB_WIDGET_NONE;
    tb_gui_command found = {0};

    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "apply_settings", on_apply, &seen));
    TB_TEST_REQUIRE_UINT(
        tb_gui_load(tb_gui_root(fixture.gui), text, sizeof(text) - 1, NULL, &button, NULL), ==,
        TB_GUI_FILE_OK);
    TB_TEST_REQUIRE(tb_widget_invoke(button));
    TB_TEST_EXPECT_INT(seen.count, ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(seen.widget, button));

    /* A name is registered once; the registry answers what it holds. */
    TB_TEST_EXPECT(!tb_gui_register_command(fixture.gui, "apply_settings", on_other, NULL));
    TB_TEST_REQUIRE(tb_gui_find_command(fixture.gui, "apply_settings", &found));
    TB_TEST_EXPECT(found.callback == on_apply);
    TB_TEST_EXPECT_POINTER(found.user, ==, &seen);
    TB_TEST_EXPECT(!tb_gui_find_command(fixture.gui, "missing", &found));

    /* Forgotten, the name no longer builds; the button keeps its command. */
    TB_TEST_EXPECT(tb_gui_unregister_command(fixture.gui, "apply_settings"));
    TB_TEST_EXPECT(!tb_gui_unregister_command(fixture.gui, "apply_settings"));
    TB_TEST_EXPECT(!tb_gui_find_command(fixture.gui, "apply_settings", &found));
    TB_TEST_REQUIRE(tb_widget_invoke(button));
    TB_TEST_EXPECT_INT(seen.count, ==, 2);
    tb_widget made = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};
    TB_TEST_EXPECT_UINT(
        tb_gui_load(tb_gui_root(fixture.gui), text, sizeof(text) - 1, NULL, &made, &error), ==,
        TB_GUI_FILE_INVALID);
    TB_TEST_EXPECT_STRING(error.reason, ==, "unknown command");
    TB_TEST_EXPECT(tb_str_find(error.message, 0, TB_STR_END, "\"apply_settings\"") !=
                   TB_STR_NOT_FOUND);
    gui_fixture_close(&fixture);
}

/* Saving writes a command by the first name it was registered under, and
 * names of other types of object do not answer for it. */
TB_TEST(gui_names, saving_writes_a_command_by_its_first_name)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    applied seen = {0};
    const tb_widget button =
        tb_button(tb_gui_root(fixture.gui),
                  &(tb_button_desc){.text = "Go", .command = on_apply, .user = &seen});
    tb_stream stream = {0};
    size_t length = 0;

    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "go", on_apply, &seen));
    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "also_go", on_apply, &seen));
    TB_TEST_REQUIRE(tb_gui_register_command(fixture.gui, "other_user", on_apply, NULL));
    tb_stream_open_buffer(NULL, &stream);
    TB_TEST_REQUIRE_UINT(tb_gui_save(button, &stream), ==, TB_GUI_FILE_OK);
    const char *const bytes = (const char *)tb_stream_buffer_bytes(&stream, &length);
    char *const text = tb_malloc(length + 1);
    memcpy(text, bytes, length);
    text[length] = '\0';
    TB_TEST_EXPECT(tb_str_find(text, 0, TB_STR_END, "\"command\": \"go\"") != TB_STR_NOT_FOUND);
    tb_free(text);
    (void)tb_stream_close(&stream);
    gui_fixture_close(&fixture);
}
