/* Entries and text boxes: text typed, the caret and the selection moved by
 * keys and the mouse, the clipboard, a placeholder, passwords, a length
 * limit, validation, read-only text, variables kept in step both ways, and
 * several lines wrapped, moved through, and scrolled. */
#include "gui_fixture.h"
#include "tb_test.h"

static tb_text text_of(tb_widget widget)
{
    return tb_widget_text(widget);
}

static void expect_selection(tb_widget widget, size_t caret, size_t anchor)
{
    size_t at = 0;
    size_t other = 0;

    TB_TEST_REQUIRE(tb_widget_selection(widget, &at, &other));
    TB_TEST_EXPECT_UINT(at, ==, caret);
    TB_TEST_EXPECT_UINT(other, ==, anchor);
}

static tb_widget placed_entry(gui_fixture *fixture, const tb_entry_desc *desc)
{
    const tb_widget entry = tb_entry(tb_gui_root(fixture->gui), desc);

    TB_TEST_REQUIRE(tb_grid(entry, NULL));
    gui_update(fixture);
    return entry;
}

/* The commands of a type a widget drew, and the first of them. */
static size_t drawn(const gui_fixture *fixture, tb_widget widget, tb_ui_command_type type,
                    const tb_ui_command **first)
{
    size_t count = 0;
    size_t found = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == widget.index + 1u && commands[index].type == type)
        {
            if (found == 0 && first != NULL)
            {
                *first = &commands[index];
            }
            found++;
        }
    }
    return found;
}

TB_TEST(gui_entry, typed_text_goes_in_at_the_caret_and_keys_edit_it)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget entry = placed_entry(&fixture, NULL);
    gui_log log = {0};

    gui_record_all(entry, &log);
    /* A click gives it the focus. */
    gui_click(&fixture, gui_middle(tb_widget_rect(entry)));
    TB_TEST_EXPECT((tb_widget_state(entry) & TB_WIDGET_FOCUSED) != 0);
    TB_TEST_EXPECT(tb_gui_wants_keyboard(fixture.gui));
    gui_type(&fixture, "hello world");
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("hello world")));
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, entry), ==, 1);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_VALIDATE, entry), ==, 1);
    expect_selection(entry, 11, 11);

    /* Words with Control, characters without; Shift selects. */
    gui_key(&fixture, TB_KEY_LEFT, TB_MODIFIER_CONTROL);
    expect_selection(entry, 6, 6);
    gui_key(&fixture, TB_KEY_LEFT, 0);
    expect_selection(entry, 5, 5);
    /* A word to the right ends at the next word's start, then at its end. */
    gui_key(&fixture, TB_KEY_RIGHT, TB_MODIFIER_CONTROL | TB_MODIFIER_SHIFT);
    expect_selection(entry, 6, 5);
    gui_key(&fixture, TB_KEY_RIGHT, TB_MODIFIER_CONTROL | TB_MODIFIER_SHIFT);
    expect_selection(entry, 11, 5);
    gui_type(&fixture, "!");
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("hello!")));
    gui_key(&fixture, TB_KEY_HOME, TB_MODIFIER_SHIFT);
    expect_selection(entry, 0, 6);
    gui_key(&fixture, TB_KEY_RIGHT, 0);
    expect_selection(entry, 6, 6);
    gui_key(&fixture, TB_KEY_HOME, 0);
    gui_key(&fixture, TB_KEY_DELETE, 0);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("ello!")));
    gui_key(&fixture, TB_KEY_END, 0);
    gui_key(&fixture, TB_KEY_BACKSPACE, 0);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("ello")));
    gui_key(&fixture, TB_KEY_BACKSPACE, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(text_of(entry).length, ==, 0);

    /* Enter and Escape are its ancestors'; Up and Down too. */
    gui_log root_log = {0};
    TB_TEST_REQUIRE(tb_bind(tb_gui_root(fixture.gui), TB_GUI_EVENT_KEY, gui_record, &root_log));
    gui_key(&fixture, TB_KEY_ENTER, 0);
    gui_key(&fixture, TB_KEY_ESCAPE, 0);
    gui_key(&fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_UINT(root_log.count, ==, 3);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, the_clipboard_copies_cuts_and_pastes)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget entry = placed_entry(&fixture, &(tb_entry_desc){.text = "one two"});

    /* Tab's focus selects the whole text. */
    gui_key(&fixture, TB_KEY_TAB, 0);
    expect_selection(entry, 7, 0);
    gui_key(&fixture, TB_KEY_C, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(tb_gui_clipboard(fixture.gui), TB_TEXT("one two")));
    TB_TEST_REQUIRE(tb_widget_select(entry, 3, 0));
    gui_key(&fixture, TB_KEY_X, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT(" two")));
    TB_TEST_EXPECT(tb_text_equal(tb_gui_clipboard(fixture.gui), TB_TEXT("one")));
    gui_key(&fixture, TB_KEY_END, 0);
    gui_key(&fixture, TB_KEY_V, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT(" twoone")));
    gui_key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    expect_selection(entry, 7, 0);
    /* Nothing selected copies nothing. */
    tb_gui_set_clipboard(fixture.gui, TB_TEXT("kept"));
    TB_TEST_REQUIRE(tb_widget_select(entry, 2, 2));
    gui_key(&fixture, TB_KEY_C, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(tb_gui_clipboard(fixture.gui), TB_TEXT("kept")));
    /* Another shortcut is not the entry's. */
    gui_key(&fixture, TB_KEY_Z, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT(" twoone")));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, the_mouse_places_the_caret_drags_a_selection_and_selects_words)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget entry = placed_entry(&fixture, &(tb_entry_desc){.text = "alpha beta"});
    const tb_canvas_rect rect = tb_widget_rect(entry);
    const float left = rect.x + 1.0f;
    const float middle = rect.y + rect.height * 0.5f;

    /* A press at the left edge puts the caret before the text. */
    gui_move(&fixture, left, middle);
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    expect_selection(entry, 0, 0);
    /* Dragged to the right edge, it selects to the end. */
    gui_move(&fixture, rect.x + rect.width - 2.0f, middle);
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    expect_selection(entry, 10, 0);
    /* Moving without the button selects nothing more. */
    gui_move(&fixture, left, middle);
    expect_selection(entry, 10, 0);

    /* A double click selects the word under it; long after the last press,
     * so that the first of its two is no double click itself. */
    tb_gui_update(fixture.gui, 1.0);
    gui_click(&fixture, (tb_vec2){left + 2.0f, middle});
    gui_click(&fixture, (tb_vec2){left + 2.0f, middle});
    expect_selection(entry, 5, 0);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, a_variable_holds_its_text_and_every_entry_on_it_follows)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var name = tb_var_string(fixture.gui, "Badger");
    const tb_widget entry = placed_entry(&fixture, &(tb_entry_desc){.variable = name});
    const tb_widget twin = tb_entry(tb_gui_root(fixture.gui), &(tb_entry_desc){.variable = name});
    tb_gui_value value = {0};

    TB_TEST_REQUIRE(tb_grid(twin, &(tb_grid_options){.row = 1}));
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("Badger")));
    TB_TEST_REQUIRE(tb_widget_focus(entry));
    gui_key(&fixture, TB_KEY_END, 0);
    gui_type(&fixture, "s");
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(name), TB_TEXT("Badgers")));
    TB_TEST_EXPECT(tb_text_equal(text_of(twin), TB_TEXT("Badgers")));

    /* Set by a program, the entry shows it and keeps its caret within it. */
    TB_TEST_EXPECT(tb_var_set_text(name, "Owl"));
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("Owl")));
    expect_selection(entry, 3, 3);
    TB_TEST_EXPECT(tb_widget_set_text(twin, "Fox"));
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(name), TB_TEXT("Fox")));

    /* Only a text variable is an entry's. */
    const tb_gui_value number = tb_gui_variable(tb_var_int(fixture.gui, 1));
    TB_TEST_EXPECT(!tb_widget_set(entry, "variable", &number));
    TB_TEST_REQUIRE(tb_widget_get(entry, "variable", &value));
    TB_TEST_EXPECT(tb_var_equal(value.variable, name));
    gui_fixture_close(&fixture);
}

/* A validator that refuses digits, after Tk's `validatecommand`. */
static void refuse_digits(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    (*calls)++;
    for (const char *at = event->text; *at != '\0'; at++)
    {
        if (*at >= '0' && *at <= '9')
        {
            tb_gui_refuse_edit(event->widget);
        }
    }
}

TB_TEST(gui_entry, validation_limits_passwords_and_read_only_text)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    int calls = 0;
    const tb_widget entry =
        placed_entry(&fixture, &(tb_entry_desc){.validate = refuse_digits, .user = &calls});
    gui_log log = {0};

    gui_record_all(entry, &log);
    TB_TEST_REQUIRE(tb_widget_focus(entry));
    gui_type(&fixture, "ab");
    gui_type(&fixture, "c1");
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("ab")));
    expect_selection(entry, 2, 2);
    TB_TEST_EXPECT_INT(calls, ==, 2);
    TB_TEST_EXPECT_UINT(gui_log_count(&log, TB_GUI_EVENT_CHANGED, entry), ==, 1);

    /* At most its length, in characters. */
    const tb_gui_value four = tb_gui_int(4);
    TB_TEST_REQUIRE(tb_widget_set(entry, "max_length", &four));
    gui_type(&fixture, "\xC3\xA9\xC3\xA9\xC3\xA9");
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("ab\xC3\xA9\xC3\xA9")));
    const tb_gui_value negative = tb_gui_int(-1);
    TB_TEST_EXPECT(!tb_widget_set(entry, "max_length", &negative));

    /* A password shows a mark for each character and is never copied. */
    TB_TEST_REQUIRE(tb_widget_set_text(entry, "secret"));
    const tb_gui_value bullet = tb_gui_text(TB_TEXT("*"));
    TB_TEST_REQUIRE(tb_widget_set(entry, "show", &bullet));
    gui_key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    gui_key(&fixture, TB_KEY_X, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("secret")));
    TB_TEST_EXPECT_UINT(tb_gui_clipboard(fixture.gui).length, ==, 0);
    const tb_ui_command *text = NULL;
    TB_TEST_REQUIRE_UINT(drawn(&fixture, entry, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT(tb_text_equal(text->text, TB_TEXT("******")));
    /* The mouse still finds characters behind the marks. */
    const tb_canvas_rect rect = tb_widget_rect(entry);
    gui_click(&fixture, (tb_vec2){rect.x + rect.width - 2.0f, rect.y + rect.height * 0.5f});
    expect_selection(entry, 6, 6);
    gui_key(&fixture, TB_KEY_LEFT, TB_MODIFIER_SHIFT);
    expect_selection(entry, 5, 6);

    /* Read-only text is selected and copied, never changed. */
    const tb_gui_value empty = tb_gui_text(TB_TEXT(""));
    const tb_gui_value yes = tb_gui_bool(true);
    TB_TEST_REQUIRE(tb_widget_set(entry, "show", &empty));
    TB_TEST_REQUIRE(tb_widget_set(entry, "read_only", &yes));
    gui_type(&fixture, "x");
    gui_key(&fixture, TB_KEY_BACKSPACE, 0);
    gui_key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    gui_key(&fixture, TB_KEY_X, TB_MODIFIER_CONTROL);
    gui_key(&fixture, TB_KEY_V, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(text_of(entry), TB_TEXT("secret")));
    TB_TEST_EXPECT(tb_text_equal(tb_gui_clipboard(fixture.gui), TB_TEXT("secret")));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, it_draws_a_placeholder_selection_and_a_blinking_caret)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_ui_theme *const theme = tb_gui_theme(fixture.gui);
    const tb_widget entry = placed_entry(&fixture, &(tb_entry_desc){.placeholder = "Your name"});
    const tb_ui_command *text = NULL;

    TB_TEST_REQUIRE_UINT(drawn(&fixture, entry, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT(tb_text_equal(text->text, TB_TEXT("Your name")));
    TB_TEST_EXPECT_FLOAT(text->color.x, ==, theme->colors[TB_UI_COLOR_TEXT_DIM].x);
    /* A frame, its border, and no caret without the focus. */
    TB_TEST_EXPECT_UINT(drawn(&fixture, entry, TB_UI_COMMAND_RECT, NULL), ==, 1);

    TB_TEST_REQUIRE(tb_widget_focus(entry));
    gui_type(&fixture, "Ann");
    TB_TEST_REQUIRE_UINT(drawn(&fixture, entry, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT(tb_text_equal(text->text, TB_TEXT("Ann")));
    /* The frame and the caret, which blinks off after a while. */
    TB_TEST_EXPECT_UINT(drawn(&fixture, entry, TB_UI_COMMAND_RECT, NULL), ==, 2);
    tb_gui_update(fixture.gui, 0.6);
    TB_TEST_EXPECT_UINT(drawn(&fixture, entry, TB_UI_COMMAND_RECT, NULL), ==, 1);
    gui_key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    /* The frame, the selection, and the caret. */
    TB_TEST_EXPECT_UINT(drawn(&fixture, entry, TB_UI_COMMAND_RECT, NULL), ==, 3);

    /* Its width is in characters, its height a line, at any scale. */
    const tb_vec2 size = tb_widget_natural_size(entry);
    const tb_gui_value wide = tb_gui_int(40);
    TB_TEST_REQUIRE(tb_widget_set(entry, "width", &wide));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(entry).x, >, size.x * 1.5f);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(entry).y, ==, size.y);
    const tb_gui_value zero = tb_gui_int(0);
    TB_TEST_EXPECT(!tb_widget_set(entry, "width", &zero));
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, a_long_line_scrolls_to_keep_the_caret_in_sight)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget entry = placed_entry(&fixture, &(tb_entry_desc){.width = 4});
    const tb_canvas_rect rect = tb_widget_rect(entry);
    const tb_ui_command *text = NULL;

    TB_TEST_REQUIRE(tb_widget_focus(entry));
    gui_type(&fixture, "a rather long line of text");
    TB_TEST_REQUIRE_UINT(drawn(&fixture, entry, TB_UI_COMMAND_TEXT, &text), ==, 1);
    /* Its start is moved left, out of the field. */
    TB_TEST_EXPECT_FLOAT(text->rect.x, <, rect.x);
    gui_key(&fixture, TB_KEY_HOME, 0);
    TB_TEST_REQUIRE_UINT(drawn(&fixture, entry, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT_FLOAT(text->rect.x, >, rect.x);
    gui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Text boxes.
 * ------------------------------------------------------------------------- */

TB_TEST(gui_entry, a_text_box_breaks_lines_and_moves_through_them)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_var notes = tb_var_string(fixture.gui, "");
    const tb_widget box = tb_text_box(
        tb_gui_root(fixture.gui), &(tb_text_box_desc){.variable = notes, .width = 20, .height = 3});

    TB_TEST_REQUIRE(tb_grid(box, NULL));
    gui_update(&fixture);
    gui_click(&fixture, gui_middle(tb_widget_rect(box)));
    gui_type(&fixture, "first");
    gui_key(&fixture, TB_KEY_ENTER, 0);
    gui_type(&fixture, "second line");
    TB_TEST_EXPECT(tb_text_equal(tb_var_get_text(notes), TB_TEXT("first\nsecond line")));
    expect_selection(box, 17, 17);

    /* Up keeps to the caret's column as near as it can: the end of "first". */
    gui_key(&fixture, TB_KEY_UP, 0);
    expect_selection(box, 5, 5);
    /* Down again, selecting: into the second line, below the column. */
    gui_key(&fixture, TB_KEY_DOWN, TB_MODIFIER_SHIFT);
    size_t caret = 0;
    size_t anchor = 0;
    TB_TEST_REQUIRE(tb_widget_selection(box, &caret, &anchor));
    TB_TEST_EXPECT_UINT(anchor, ==, 5);
    TB_TEST_EXPECT_UINT(caret, >, 6);
    TB_TEST_EXPECT_UINT(caret, <, 17);
    gui_key(&fixture, TB_KEY_HOME, 0);
    expect_selection(box, 6, 6);
    gui_key(&fixture, TB_KEY_END, 0);
    expect_selection(box, 17, 17);
    gui_key(&fixture, TB_KEY_HOME, TB_MODIFIER_CONTROL);
    expect_selection(box, 0, 0);
    gui_key(&fixture, TB_KEY_PAGE_DOWN, 0);
    expect_selection(box, 6, 6);
    gui_key(&fixture, TB_KEY_PAGE_UP, 0);
    expect_selection(box, 0, 0);
    gui_key(&fixture, TB_KEY_END, TB_MODIFIER_CONTROL);
    expect_selection(box, 17, 17);

    /* A line wider than the box wraps: three lines in the box's width. */
    const tb_canvas_rect rect = tb_widget_rect(box);
    TB_TEST_EXPECT(
        tb_var_set_text(notes, "a b c d e f g h i j k l m n o p q r s t u v w x y z a b c"));
    gui_update(&fixture);
    const tb_ui_command *text = NULL;
    TB_TEST_REQUIRE_UINT(drawn(&fixture, box, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT_FLOAT(text->wrap_width, >, 0.0f);
    TB_TEST_EXPECT_FLOAT(text->wrap_width, <, rect.width);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, a_text_box_scrolls_with_the_wheel_and_the_caret)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget box = tb_text_box(
        tb_gui_root(fixture.gui),
        &(tb_text_box_desc){.text = "1\n2\n3\n4\n5\n6\n7\n8\n9", .height = 2, .read_only = true});
    const tb_ui_command *text = NULL;
    gui_log root_log = {0};

    TB_TEST_REQUIRE(tb_grid(box, NULL));
    TB_TEST_REQUIRE(tb_bind(tb_gui_root(fixture.gui), TB_GUI_EVENT_WHEEL, gui_record, &root_log));
    gui_update(&fixture);
    const tb_canvas_rect rect = tb_widget_rect(box);
    TB_TEST_REQUIRE_UINT(drawn(&fixture, box, TB_UI_COMMAND_TEXT, &text), ==, 1);
    const float top = text->rect.y;

    /* Down a notch scrolls it; at the top, the wheel goes on to its parent. */
    gui_move(&fixture, rect.x + 4.0f, rect.y + 4.0f);
    gui_wheel(&fixture, -1.0f);
    TB_TEST_REQUIRE_UINT(drawn(&fixture, box, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT_FLOAT(text->rect.y, <, top);
    TB_TEST_EXPECT_UINT(root_log.count, ==, 0);
    gui_wheel(&fixture, 5.0f);
    gui_wheel(&fixture, 1.0f);
    TB_TEST_EXPECT_UINT(root_log.count, ==, 1);

    /* Read-only: keys move, Enter changes nothing. */
    TB_TEST_REQUIRE(tb_widget_focus(box));
    gui_key(&fixture, TB_KEY_END, TB_MODIFIER_CONTROL);
    gui_key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT(tb_text_equal(text_of(box), TB_TEXT("1\n2\n3\n4\n5\n6\n7\n8\n9")));
    TB_TEST_REQUIRE_UINT(drawn(&fixture, box, TB_UI_COMMAND_TEXT, &text), ==, 1);
    /* The caret at the end is in sight: the last line is at the bottom. */
    TB_TEST_EXPECT_FLOAT(text->rect.y + text->rect.height, <=, rect.y + rect.height + 1.0f);

    /* Without wrapping, it does not break lines; its properties say so. */
    const tb_gui_value none = tb_gui_enum(TB_TEXT_WRAP_NONE);
    tb_gui_value value = {0};
    TB_TEST_REQUIRE(tb_widget_set(box, "wrap", &none));
    TB_TEST_REQUIRE(tb_widget_get(box, "wrap", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_TEXT_WRAP_NONE);
    TB_TEST_REQUIRE(tb_widget_get(box, "height", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 2);
    const tb_gui_value zero = tb_gui_int(0);
    TB_TEST_EXPECT(!tb_widget_set(box, "height", &zero));
    gui_update(&fixture);
    TB_TEST_REQUIRE_UINT(drawn(&fixture, box, TB_UI_COMMAND_TEXT, &text), ==, 1);
    TB_TEST_EXPECT_FLOAT(text->wrap_width, ==, 0.0f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_entry, only_text_widgets_have_a_selection)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), NULL);
    size_t caret = 0;
    size_t anchor = 0;

    TB_TEST_EXPECT(!tb_widget_selection(label, &caret, &anchor));
    TB_TEST_EXPECT(!tb_widget_select(label, 0, 0));
    tb_gui_refuse_edit(label);
    gui_fixture_close(&fixture);
}
