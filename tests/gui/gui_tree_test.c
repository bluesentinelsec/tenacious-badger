/* The retained GUI's widgets: handles that stay valid and go stale when
 * their widget is destroyed, the tree, kinds a game registers, and
 * properties enumerated, read, and set by name. */
#include "badger/str.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <math.h>
#include <stddef.h>

TB_TEST(gui_tree, a_stale_handle_is_detected_and_never_reused)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget label = tb_label(root, &(tb_label_desc){.text = "Hello"});

    TB_TEST_REQUIRE(tb_widget_alive(label));
    TB_TEST_EXPECT(tb_widget_alive(root));
    tb_widget_destroy(label);
    TB_TEST_EXPECT(!tb_widget_alive(label));

    /* Every function given the stale handle does nothing and says so. */
    TB_TEST_EXPECT(!tb_widget_set_text(label, "Again"));
    TB_TEST_EXPECT_UINT(tb_widget_text(label).length, ==, 0);
    TB_TEST_EXPECT(!tb_grid(label, NULL));
    TB_TEST_EXPECT(!tb_bind(label, TB_GUI_EVENT_CLICK, gui_record, NULL));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_create(label, tb_frame_class())));
    TB_TEST_EXPECT(!tb_widget_alive(tb_label(label, NULL)));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_parent(label)));
    TB_TEST_EXPECT_POINTER(tb_widget_data(label), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_widget_class_of(label), ==, NULL);
    TB_TEST_EXPECT_UINT(tb_widget_property_count(label), ==, 0);
    TB_TEST_EXPECT(!tb_widget_invoke(label));
    TB_TEST_EXPECT(!tb_widget_focus(label));
    TB_TEST_EXPECT(!tb_widget_show_modal(label));
    TB_TEST_EXPECT_UINT(tb_widget_state(label), ==, 0);
    tb_widget_destroy(label);
    tb_widget_set_visible(label, false);

    /* The slot is used again, under another generation. */
    const tb_widget next = tb_label(root, NULL);
    TB_TEST_EXPECT_UINT(next.index, ==, label.index);
    TB_TEST_EXPECT_UINT(next.generation, !=, label.generation);
    TB_TEST_EXPECT(!tb_widget_alive(label));
    TB_TEST_EXPECT(!tb_widget_equal(next, label));
    TB_TEST_EXPECT(!tb_widget_alive(TB_WIDGET_NONE));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_tree, children_keep_their_order_and_die_with_their_parent)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget frame = tb_frame(root, NULL);
    const tb_widget first = tb_label(frame, NULL);
    const tb_widget second = tb_frame(frame, NULL);
    const tb_widget third = tb_button(frame, NULL);
    const tb_widget grandchild = tb_label(second, NULL);
    tb_gui_stats stats = {0};

    TB_TEST_EXPECT_UINT(tb_widget_child_count(frame), ==, 3);
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_first_child(frame), first));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_next_sibling(first), second));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_next_sibling(second), third));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_next_sibling(third)));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_parent(grandchild), second));
    TB_TEST_EXPECT(!tb_widget_alive(tb_widget_parent(root)));
    TB_TEST_EXPECT_POINTER(tb_widget_class_of(third), ==, tb_button_class());

    /* A child destroyed is unlinked; its siblings close up. */
    tb_widget_destroy(second);
    TB_TEST_EXPECT(!tb_widget_alive(grandchild));
    TB_TEST_EXPECT(tb_widget_equal(tb_widget_next_sibling(first), third));
    TB_TEST_EXPECT_UINT(tb_widget_child_count(frame), ==, 2);

    tb_widget_destroy(frame);
    TB_TEST_EXPECT(!tb_widget_alive(first));
    TB_TEST_EXPECT(!tb_widget_alive(third));
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT_UINT(stats.widgets, ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_tree, the_root_is_emptied_never_destroyed)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget child = tb_label(root, NULL);

    tb_widget_destroy(root);
    TB_TEST_EXPECT(tb_widget_alive(root));
    TB_TEST_EXPECT(!tb_widget_alive(child));
    TB_TEST_EXPECT_UINT(tb_widget_child_count(root), ==, 0);
    TB_TEST_EXPECT(!tb_grid(root, NULL));
    gui_fixture_close(&fixture);
}

/* A kind with a property of every type at an offset, a read-only one, and
 * text the GUI releases; its init and destroy are counted. */
typedef struct every_data
{
    bool flag;
    int32_t number;
    float amount;
    tb_vec2 pair;
    tb_vec4 tint;
    tb_gui_string caption;
    int32_t choice;
    uint32_t bits;
    tb_font_spec face;
    tb_gui_command action;
    int32_t fixed;
} every_data;

static int every_inits;
static int every_destroys;

static const char *const every_choices[] = {"one", "two", "three"};
static const char *const every_bits[] = {"a", "b"};

static void every_init(tb_widget widget, void *data)
{
    every_data *const every = data;

    (void)widget;
    every->fixed = 7;
    every_inits++;
}

static void every_destroy(tb_widget widget, void *data)
{
    (void)widget;
    (void)data;
    every_destroys++;
}

static const tb_gui_property every_properties[] = {
    {.name = "flag", .type = TB_GUI_TYPE_BOOL, .offset = offsetof(every_data, flag)},
    {.name = "number", .type = TB_GUI_TYPE_INT, .offset = offsetof(every_data, number)},
    {.name = "amount", .type = TB_GUI_TYPE_FLOAT, .offset = offsetof(every_data, amount)},
    {.name = "pair", .type = TB_GUI_TYPE_VEC2, .offset = offsetof(every_data, pair)},
    {.name = "tint", .type = TB_GUI_TYPE_COLOR, .offset = offsetof(every_data, tint)},
    {.name = "caption", .type = TB_GUI_TYPE_TEXT, .offset = offsetof(every_data, caption)},
    {.name = "choice",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(every_data, choice),
     .names = every_choices,
     .name_count = 3},
    {.name = "bits",
     .type = TB_GUI_TYPE_FLAGS,
     .offset = offsetof(every_data, bits),
     .names = every_bits,
     .name_count = 2},
    {.name = "face", .type = TB_GUI_TYPE_FONT, .offset = offsetof(every_data, face)},
    {.name = "action", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(every_data, action)},
    {.name = "fixed",
     .type = TB_GUI_TYPE_INT,
     .offset = offsetof(every_data, fixed),
     .flags = TB_GUI_PROPERTY_READ_ONLY},
};

static const tb_widget_class every_kind = {.name = "every",
                                           .data_size = sizeof(every_data),
                                           .properties = every_properties,
                                           .property_count = TB_TEST_COUNT_OF(every_properties),
                                           .init = every_init,
                                           .destroy = every_destroy};

static void ignore_event(const tb_gui_event *event, void *user)
{
    (void)event;
    (void)user;
}

TB_TEST(gui_tree, a_game_registers_a_kind_found_by_its_name)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const size_t engine_kinds = tb_gui_class_count(fixture.gui);

    /* Frames, labels, and buttons among them; other kinds come before a game's too. */
    TB_TEST_EXPECT_UINT(engine_kinds, >=, 3);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "button"), ==, tb_button_class());
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "label"), ==, tb_label_class());
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "frame"), ==, tb_frame_class());
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "every"), ==, NULL);
    TB_TEST_REQUIRE(tb_gui_register_class(fixture.gui, &every_kind));
    TB_TEST_EXPECT(!tb_gui_register_class(fixture.gui, &every_kind));
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "every"), ==, &every_kind);
    TB_TEST_EXPECT_POINTER(tb_gui_class_at(fixture.gui, engine_kinds), ==, &every_kind);
    TB_TEST_EXPECT_POINTER(tb_gui_class_at(fixture.gui, engine_kinds + 1), ==, NULL);

    /* Its widgets are made, given their first values, and destroyed. */
    every_inits = 0;
    every_destroys = 0;
    const tb_widget widget = tb_widget_create(tb_gui_root(fixture.gui), &every_kind);
    TB_TEST_EXPECT_INT(every_inits, ==, 1);
    const every_data *const every = tb_widget_data(widget);
    TB_TEST_REQUIRE_POINTER(every, !=, NULL);
    TB_TEST_EXPECT_INT(every->fixed, ==, 7);
    tb_widget_destroy(widget);
    TB_TEST_EXPECT_INT(every_destroys, ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_tree, every_type_of_property_is_read_and_set_by_name)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget widget = tb_widget_create(tb_gui_root(fixture.gui), &every_kind);
    tb_gui_value value = {0};
    size_t common = 0;
    const tb_font_spec face = {.role = TB_FONT_ROLE_HEADING, .weight = 700, .size = 20.0f};
    const tb_gui_value values[] = {
        tb_gui_bool(true),
        tb_gui_int(-12),
        tb_gui_float(2.5f),
        tb_gui_vec2((tb_vec2){3.0f, 4.0f}),
        tb_gui_color((tb_vec4){0.1f, 0.2f, 0.3f, 0.4f}),
        tb_gui_text(TB_TEXT("caption")),
        tb_gui_enum(2),
        tb_gui_flags(3u),
        tb_gui_font(face),
        tb_gui_command_of(ignore_event, &fixture),
    };
    static const char *const names[] = {"flag",    "number", "amount", "pair", "tint",
                                        "caption", "choice", "bits",   "face", "action"};

    (void)tb_gui_common_properties(&common);
    TB_TEST_EXPECT_UINT(tb_widget_property_count(widget), ==,
                        common + TB_TEST_COUNT_OF(every_properties));
    TB_TEST_EXPECT_STRING(tb_widget_property_at(widget, 0)->name, ==, "name");
    TB_TEST_EXPECT_STRING(tb_widget_property_at(widget, common)->name, ==, "flag");
    TB_TEST_EXPECT_POINTER(tb_widget_property_at(widget, tb_widget_property_count(widget)), ==,
                           NULL);
    TB_TEST_EXPECT_POINTER(tb_widget_find_property(widget, "choice"), ==, &every_properties[6]);
    TB_TEST_EXPECT_POINTER(tb_widget_find_property(widget, "nothing"), ==, NULL);

    for (size_t index = 0; index < TB_TEST_COUNT_OF(names); index++)
    {
        TB_TEST_CONTEXT("property %s", names[index]);
        TB_TEST_EXPECT(tb_widget_set(widget, names[index], &values[index]));
        TB_TEST_REQUIRE(tb_widget_get(widget, names[index], &value));
        TB_TEST_EXPECT_UINT(value.type, ==, values[index].type);
    }
    const every_data *const every = tb_widget_data(widget);
    TB_TEST_EXPECT(every->flag);
    TB_TEST_EXPECT_INT(every->number, ==, -12);
    TB_TEST_EXPECT_FLOAT(every->amount, ==, 2.5f);
    TB_TEST_EXPECT_FLOAT(every->pair.y, ==, 4.0f);
    TB_TEST_EXPECT_FLOAT(every->tint.w, ==, 0.4f);
    TB_TEST_EXPECT_STRING(every->caption.bytes, ==, "caption");
    TB_TEST_EXPECT_INT(every->choice, ==, 2);
    TB_TEST_EXPECT_UINT(every->bits, ==, 3u);
    TB_TEST_EXPECT_UINT(every->face.weight, ==, 700);
    TB_TEST_EXPECT(every->action.callback == ignore_event);
    TB_TEST_REQUIRE(tb_widget_get(widget, "caption", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("caption")));

    /* What a property does not take is refused, and changes nothing: another
     * type, a name past the enumeration's, a bit past the flags', a number
     * that is not finite, a read-only property, and a name it does not have. */
    const tb_gui_value wrong_type = tb_gui_int(1);
    const tb_gui_value past_names = tb_gui_enum(3);
    const tb_gui_value below_names = tb_gui_enum(-1);
    const tb_gui_value past_bits = tb_gui_flags(4u);
    const tb_gui_value infinite = tb_gui_float(INFINITY);
    const tb_gui_value fixed = tb_gui_int(9);
    TB_TEST_EXPECT(!tb_widget_set(widget, "flag", &wrong_type));
    TB_TEST_EXPECT(!tb_widget_set(widget, "choice", &past_names));
    TB_TEST_EXPECT(!tb_widget_set(widget, "choice", &below_names));
    TB_TEST_EXPECT(!tb_widget_set(widget, "bits", &past_bits));
    TB_TEST_EXPECT(!tb_widget_set(widget, "amount", &infinite));
    TB_TEST_EXPECT(!tb_widget_set(widget, "fixed", &fixed));
    TB_TEST_EXPECT(!tb_widget_set(widget, "nothing", &fixed));
    TB_TEST_EXPECT(!tb_widget_get(widget, "nothing", &value));
    TB_TEST_EXPECT_INT(every->choice, ==, 2);
    TB_TEST_EXPECT_INT(every->fixed, ==, 7);
    TB_TEST_REQUIRE(tb_widget_get(widget, "fixed", &value));

    /* Text set again replaces the old, which the GUI releases, as it does the
     * last when the widget is destroyed. */
    const tb_gui_value again = tb_gui_text(TB_TEXT("again"));
    TB_TEST_EXPECT(tb_widget_set(widget, "caption", &again));
    TB_TEST_EXPECT_STRING(every->caption.bytes, ==, "again");
    gui_fixture_close(&fixture);
}

TB_TEST(gui_tree, the_common_properties_are_every_widgets)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "Hi"});
    tb_gui_value value = {0};
    size_t count = 0;
    const tb_gui_property *const common = tb_gui_common_properties(&count);

    /* Every name is unique, among the common properties and the label's. */
    for (size_t index = 0; index < tb_widget_property_count(label); index++)
    {
        for (size_t other = index + 1; other < tb_widget_property_count(label); other++)
        {
            TB_TEST_EXPECT(!tb_str_equal(tb_widget_property_at(label, index)->name,
                                         tb_widget_property_at(label, other)->name));
        }
    }
    TB_TEST_EXPECT_STRING(common[count - 1].name, ==, "grid_anchor");

    tb_widget_set_name(label, "greeting");
    tb_widget_set_style(label, "title");
    TB_TEST_EXPECT(tb_text_equal(tb_widget_name(label), TB_TEXT("greeting")));
    TB_TEST_EXPECT(tb_text_equal(tb_widget_style(label), TB_TEXT("title")));
    TB_TEST_EXPECT(tb_widget_visible(label));
    TB_TEST_EXPECT(tb_widget_enabled(label));
    tb_widget_set_visible(label, false);
    tb_widget_set_enabled(label, false);
    TB_TEST_EXPECT(!tb_widget_visible(label));
    TB_TEST_EXPECT(!tb_widget_enabled(label));
    TB_TEST_REQUIRE(tb_widget_get(label, "visible", &value));
    TB_TEST_EXPECT(!value.boolean);
    TB_TEST_REQUIRE(tb_widget_get(label, "enabled", &value));
    TB_TEST_EXPECT(!value.boolean);

    tb_widget_set_cursor(label, TB_CURSOR_HAND);
    TB_TEST_REQUIRE(tb_widget_get(label, "cursor", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_CURSOR_HAND);
    tb_widget_set_mouse_filter(label, TB_GUI_MOUSE_IGNORE);
    TB_TEST_REQUIRE(tb_widget_get(label, "mouse_filter", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_MOUSE_IGNORE);
    TB_TEST_REQUIRE(tb_widget_get(label, "grid_anchor", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_ANCHOR_NW);
    tb_grid_anchor(label, TB_GUI_ANCHOR_SE);
    TB_TEST_REQUIRE(tb_widget_get(label, "grid_anchor", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_ANCHOR_SE);

    /* Setting a place in the grid places the widget in it. */
    TB_TEST_REQUIRE(tb_widget_get(label, "gridded", &value));
    TB_TEST_EXPECT(!value.boolean);
    const tb_gui_value row = tb_gui_int(2);
    TB_TEST_EXPECT(tb_widget_set(label, "row", &row));
    TB_TEST_REQUIRE(tb_widget_get(label, "gridded", &value));
    TB_TEST_EXPECT(value.boolean);
    const tb_gui_value column = tb_gui_int(1);
    const tb_gui_value span = tb_gui_int(3);
    const tb_gui_value sticky = tb_gui_flags(TB_STICKY_EW);
    const tb_gui_value padx = tb_gui_vec2((tb_vec2){2.0f, 5.0f});
    const tb_gui_value pady = tb_gui_vec2((tb_vec2){1.0f, 6.0f});
    const tb_gui_value ipad = tb_gui_float(4.0f);
    TB_TEST_EXPECT(tb_widget_set(label, "column", &column));
    TB_TEST_EXPECT(tb_widget_set(label, "row_span", &span));
    TB_TEST_EXPECT(tb_widget_set(label, "column_span", &span));
    TB_TEST_EXPECT(tb_widget_set(label, "sticky", &sticky));
    TB_TEST_EXPECT(tb_widget_set(label, "padx", &padx));
    TB_TEST_EXPECT(tb_widget_set(label, "pady", &pady));
    TB_TEST_EXPECT(tb_widget_set(label, "ipadx", &ipad));
    TB_TEST_EXPECT(tb_widget_set(label, "ipady", &ipad));
    tb_grid_options options = {0};
    TB_TEST_REQUIRE(tb_grid_info(label, &options));
    TB_TEST_EXPECT_INT(options.row, ==, 2);
    TB_TEST_EXPECT_INT(options.column, ==, 1);
    TB_TEST_EXPECT_INT(options.row_span, ==, 3);
    TB_TEST_EXPECT_INT(options.column_span, ==, 3);
    TB_TEST_EXPECT_UINT(options.sticky, ==, TB_STICKY_EW);
    TB_TEST_EXPECT_FLOAT(options.padx.y, ==, 5.0f);
    TB_TEST_EXPECT_FLOAT(options.pady.y, ==, 6.0f);
    TB_TEST_EXPECT_FLOAT(options.ipady, ==, 4.0f);
    static const char *const read_back[] = {"row",  "column", "row_span", "column_span", "sticky",
                                            "padx", "pady",   "ipadx",    "ipady"};
    for (size_t index = 0; index < TB_TEST_COUNT_OF(read_back); index++)
    {
        TB_TEST_EXPECT(tb_widget_get(label, read_back[index], &value));
    }

    /* Places the grid does not take are refused. */
    const tb_gui_value below = tb_gui_int(-1);
    const tb_gui_value no_span = tb_gui_int(0);
    const tb_gui_value negative = tb_gui_vec2((tb_vec2){-1.0f, 0.0f});
    TB_TEST_EXPECT(!tb_widget_set(label, "row", &below));
    TB_TEST_EXPECT(!tb_widget_set(label, "row_span", &no_span));
    TB_TEST_EXPECT(!tb_widget_set(label, "column_span", &no_span));
    TB_TEST_EXPECT(!tb_widget_set(label, "padx", &negative));

    const tb_gui_value forget = tb_gui_bool(false);
    TB_TEST_EXPECT(tb_widget_set(label, "gridded", &forget));
    TB_TEST_EXPECT(!tb_grid_info(label, &options));
    const tb_gui_value place = tb_gui_bool(true);
    TB_TEST_EXPECT(tb_widget_set(label, "gridded", &place));
    TB_TEST_EXPECT(tb_grid_info(label, &options));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_tree, the_engines_kinds_take_their_options_as_properties)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget frame =
        tb_frame(root, &(tb_frame_desc){.padding = 6.0f, .background = true, .border = true});
    const tb_widget label = tb_label(root, &(tb_label_desc){.text = "Text",
                                                            .wrap_length = 40.0f,
                                                            .justify = TB_FONT_ALIGN_RIGHT,
                                                            .anchor = TB_GUI_ANCHOR_W});
    const tb_widget button = tb_button(
        root, &(tb_button_desc){.text = "OK", .command = ignore_event, .is_default = true});
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_widget_get(frame, "padding", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 6.0f);
    TB_TEST_REQUIRE(tb_widget_get(frame, "background", &value));
    TB_TEST_EXPECT(value.boolean);
    TB_TEST_REQUIRE(tb_widget_get(frame, "border", &value));
    TB_TEST_EXPECT(value.boolean);
    /* A frame filled stops the mouse; one that is not leaves it. */
    TB_TEST_REQUIRE(tb_widget_get(frame, "mouse_filter", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_MOUSE_STOP);
    const tb_gui_value unfilled = tb_gui_bool(false);
    TB_TEST_EXPECT(tb_widget_set(frame, "background", &unfilled));
    TB_TEST_REQUIRE(tb_widget_get(frame, "mouse_filter", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_MOUSE_PASS);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(frame, "padding", &negative));

    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(label), TB_TEXT("Text")));
    TB_TEST_REQUIRE(tb_widget_get(label, "wrap_length", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 40.0f);
    TB_TEST_EXPECT(!tb_widget_set(label, "wrap_length", &negative));
    TB_TEST_REQUIRE(tb_widget_get(label, "justify", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_FONT_ALIGN_RIGHT);
    TB_TEST_REQUIRE(tb_widget_get(label, "anchor", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_ANCHOR_W);
    TB_TEST_EXPECT_STRING(tb_widget_find_property(label, "justify")->names[2], ==, "right");

    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(button), TB_TEXT("OK")));
    TB_TEST_REQUIRE(tb_widget_get(button, "command", &value));
    TB_TEST_EXPECT(value.command.callback == ignore_event);
    TB_TEST_REQUIRE(tb_widget_get(button, "default", &value));
    TB_TEST_EXPECT(value.boolean);
    TB_TEST_EXPECT(tb_widget_set_text(button, "Apply"));
    TB_TEST_EXPECT(tb_text_equal(tb_widget_text(button), TB_TEXT("Apply")));

    /* A frame has no text. */
    TB_TEST_EXPECT(!tb_widget_set_text(frame, "No"));
    TB_TEST_EXPECT_UINT(tb_widget_text(frame).length, ==, 0);
    gui_fixture_close(&fixture);
}
