/* The text field driven by keys and text made by hand: typing, erasing,
 * the caret and the selection, words, the clipboard, and its capacity. */
#include "badger/str.h"

#include "tb_test.h"
#include "ui_fixture.h"

#include <string.h>

static void field_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Fields", UI_WINDOW_RECT, 0))
    {
        fixture->returned[0] = tb_ui_text_field(fixture->ui, "Name", fixture->text, 48);
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        if (fixture->flag)
        {
            tb_ui_focus_last(fixture->ui);
            fixture->flag = false;
        }
        fixture->returned[1] = tb_ui_text_field(fixture->ui, "##short", fixture->text + 48, 8);
        fixture->ids[1] = tb_ui_last_id(fixture->ui);
        tb_ui_end_window(fixture->ui);
    }
}

/* A field with the focus from a click on it, ready to type in. */
static void open_field(ui_fixture *fixture, const char *text)
{
    ui_fixture_open(fixture, 1.0f, 0);
    fixture->scene = field_scene;
    memcpy(fixture->text, text, tb_str_length(text) + 1);
    ui_frame(fixture);
    const tb_canvas_rect field = ui_rect_of(fixture, fixture->ids[0]);
    /* Past the text's end, so that the caret goes to its end. */
    ui_click(fixture, (tb_vec2){field.x + field.width - 4.0f, field.y + field.height * 0.5f});
    TB_TEST_REQUIRE_UINT(ui_stats(fixture).focus, ==, fixture->ids[0]);
}

static void key(ui_fixture *fixture, tb_key pressed, uint32_t modifiers)
{
    ui_key(fixture, pressed, modifiers);
    ui_frame(fixture);
}

/* Text typed goes in at the caret; Backspace takes the character before. */
TB_TEST(ui_text_field, typing_inserts_and_backspace_erases)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "");
    TB_TEST_EXPECT(tb_ui_wants_keyboard(fixture.ui));
    TB_TEST_EXPECT(tb_ui_wants_text(fixture.ui));
    ui_type(&fixture, "hello");
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[0]);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "hello");
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[0]);
    key(&fixture, TB_KEY_BACKSPACE, 0);
    TB_TEST_EXPECT(fixture.returned[0]);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "hell");

    /* Keys and text in one frame keep their order. */
    ui_type(&fixture, "o!");
    ui_key(&fixture, TB_KEY_BACKSPACE, 0);
    ui_type(&fixture, "?");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "hello?");
    ui_fixture_close(&fixture);
}

/* The arrows move the caret a character, Home and End to the ends, and
 * Delete takes the character after it. */
TB_TEST(ui_text_field, the_caret_moves_and_delete_erases_after_it)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "abcd");
    key(&fixture, TB_KEY_LEFT, 0);
    key(&fixture, TB_KEY_LEFT, 0);
    ui_type(&fixture, "X");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "abXcd");
    key(&fixture, TB_KEY_HOME, 0);
    key(&fixture, TB_KEY_DELETE, 0);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "bXcd");
    key(&fixture, TB_KEY_END, 0);
    key(&fixture, TB_KEY_DELETE, 0);
    TB_TEST_EXPECT(!fixture.returned[0]);
    key(&fixture, TB_KEY_RIGHT, 0);
    ui_type(&fixture, "!");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "bXcd!");
    key(&fixture, TB_KEY_HOME, 0);
    key(&fixture, TB_KEY_BACKSPACE, 0);
    TB_TEST_EXPECT(!fixture.returned[0]);
    ui_fixture_close(&fixture);
}

/* Shift with a move selects; typing replaces the selection; an arrow
 * without Shift shuts it to its edge. */
TB_TEST(ui_text_field, a_selection_is_replaced_by_typing)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "one two");
    key(&fixture, TB_KEY_LEFT, TB_MODIFIER_SHIFT);
    key(&fixture, TB_KEY_LEFT, TB_MODIFIER_SHIFT);
    key(&fixture, TB_KEY_LEFT, TB_MODIFIER_SHIFT);
    ui_type(&fixture, "2");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "one 2");
    key(&fixture, TB_KEY_HOME, TB_MODIFIER_SHIFT);
    key(&fixture, TB_KEY_RIGHT, 0);
    ui_type(&fixture, "#");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "one 2#");
    key(&fixture, TB_KEY_HOME, 0);
    key(&fixture, TB_KEY_END, TB_MODIFIER_SHIFT);
    key(&fixture, TB_KEY_LEFT, 0);
    ui_type(&fixture, "<");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "<one 2#");
    key(&fixture, TB_KEY_RIGHT, TB_MODIFIER_SHIFT);
    key(&fixture, TB_KEY_BACKSPACE, 0);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "<ne 2#");
    ui_fixture_close(&fixture);
}

/* Ctrl with the arrows, Backspace, and Delete goes by words. */
TB_TEST(ui_text_field, ctrl_moves_and_erases_by_words)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "alpha beta, gamma");
    key(&fixture, TB_KEY_LEFT, TB_MODIFIER_CONTROL);
    ui_type(&fixture, "_");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "alpha beta, _gamma");
    key(&fixture, TB_KEY_BACKSPACE, 0);
    key(&fixture, TB_KEY_LEFT, TB_MODIFIER_CONTROL);
    key(&fixture, TB_KEY_BACKSPACE, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "beta, gamma");
    key(&fixture, TB_KEY_DELETE, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "gamma");
    key(&fixture, TB_KEY_RIGHT, TB_MODIFIER_CONTROL);
    ui_type(&fixture, "!");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "gamma!");
    ui_fixture_close(&fixture);
}

/* Ctrl+A selects everything; Ctrl+C and Ctrl+X copy to the interface's
 * clipboard, and Ctrl+V pastes it. */
TB_TEST(ui_text_field, the_clipboard_copies_cuts_and_pastes)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "copy");
    key(&fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    key(&fixture, TB_KEY_C, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(tb_text_equal(tb_ui_clipboard(fixture.ui), TB_TEXT("copy")));
    key(&fixture, TB_KEY_END, 0);
    key(&fixture, TB_KEY_V, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "copycopy");
    key(&fixture, TB_KEY_HOME, TB_MODIFIER_SHIFT);
    key(&fixture, TB_KEY_X, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "");
    TB_TEST_EXPECT(tb_text_equal(tb_ui_clipboard(fixture.ui), TB_TEXT("copycopy")));

    /* Pasted control characters are dropped: the field is one line. */
    tb_ui_set_clipboard(fixture.ui, TB_TEXT("a\nb\tc"));
    key(&fixture, TB_KEY_V, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "abc");
    tb_ui_set_clipboard(fixture.ui, TB_TEXT(""));
    TB_TEST_EXPECT_UINT(tb_ui_clipboard(fixture.ui).length, ==, 0);
    ui_fixture_close(&fixture);
}

/* Text past the capacity is dropped, whole characters at a time. */
TB_TEST(ui_text_field, text_past_its_capacity_is_dropped)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = field_scene;
    ui_frame(&fixture);
    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[1])));
    ui_type(&fixture, "abcde");
    ui_type(&fixture, "\xC3\xA9\xC3\xA9");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text + 48, ==, "abcde\xC3\xA9");
    TB_TEST_EXPECT(fixture.returned[1]);
    ui_fixture_close(&fixture);
}

/* A byte that begins no character is a character of its own to the caret,
 * and typed text that is not UTF-8 is dropped. */
TB_TEST(ui_text_field, bytes_of_no_character_are_stepped_over)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "a\xFF"
                         "b");
    key(&fixture, TB_KEY_HOME, 0);
    key(&fixture, TB_KEY_RIGHT, 0);
    key(&fixture, TB_KEY_RIGHT, 0);
    ui_type(&fixture, "Y\xFF");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==,
                          "a\xFFY"
                          "b");
    key(&fixture, TB_KEY_LEFT, 0);
    key(&fixture, TB_KEY_LEFT, 0);
    key(&fixture, TB_KEY_BACKSPACE, 0);
    TB_TEST_EXPECT_STRING(fixture.text, ==,
                          "\xFFY"
                          "b");
    ui_fixture_close(&fixture);
}

/* The caller may give a field the focus, to type in at once. */
TB_TEST(ui_text_field, the_caller_may_focus_a_field)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = field_scene;
    fixture.flag = true;
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[0]);
    ui_type(&fixture, "now");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "now");
    TB_TEST_EXPECT(tb_ui_wants_text(fixture.ui));
    ui_fixture_close(&fixture);
}

/* Enter and Escape end the editing, and give the keyboard back. */
TB_TEST(ui_text_field, enter_and_escape_end_the_editing)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "done");
    key(&fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, 0);
    TB_TEST_EXPECT(!tb_ui_wants_keyboard(fixture.ui));
    ui_type(&fixture, "x");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "done");
    ui_fixture_close(&fixture);

    open_field(&fixture, "done");
    key(&fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, 0);
    ui_fixture_close(&fixture);
}

/* Tab into a field selects its text, so that typing replaces it; Tab out
 * leaves it. */
TB_TEST(ui_text_field, tab_selects_the_whole_text)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = field_scene;
    memcpy(fixture.text, "old", 4);
    ui_frame(&fixture);
    key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[0]);
    ui_type(&fixture, "new");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "new");
    key(&fixture, TB_KEY_TAB, 0);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[1]);
    ui_fixture_close(&fixture);
}

/* A click puts the caret between the characters nearest it; a drag selects
 * from there; Shift+click extends the selection. */
TB_TEST(ui_text_field, the_mouse_places_the_caret_and_selects)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = field_scene;
    memcpy(fixture.text, "mmmm", 5);
    ui_frame(&fixture);
    const tb_canvas_rect field = ui_rect_of(&fixture, fixture.ids[0]);
    const tb_ui_command *const text = ui_text_command(&fixture, "mmmm");
    TB_TEST_REQUIRE_POINTER(text, !=, NULL);
    const float glyph = text->rect.width / 4.0f;
    const float middle = field.y + field.height * 0.5f;

    ui_click(&fixture, (tb_vec2){text->rect.x + glyph * 2.0f + 1.0f, middle});
    TB_TEST_EXPECT_UINT(tb_ui_cursor(fixture.ui), ==, TB_CURSOR_TEXT);
    ui_type(&fixture, "|");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "mm|mm");

    ui_move(&fixture, text->rect.x + 1.0f, middle);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, text->rect.x + glyph * 2.0f + 1.0f, middle);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    ui_type(&fixture, "_");
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "_|mm");

    fixture.input.modifiers = TB_MODIFIER_SHIFT;
    ui_click(&fixture, (tb_vec2){field.x + field.width - 2.0f, middle});
    fixture.input.modifiers = 0;
    key(&fixture, TB_KEY_DELETE, 0);
    TB_TEST_EXPECT_STRING(fixture.text, ==, "_");
    ui_fixture_close(&fixture);
}

/* A long text scrolls in its field to keep the caret seen, and a buffer
 * without its terminator is ended at its capacity. */
TB_TEST(ui_text_field, long_text_scrolls_to_the_caret)
{
    ui_fixture fixture = {0};

    open_field(&fixture, "");
    for (int round = 0; round < 3; round++)
    {
        ui_type(&fixture, "wwwwwwwwww");
        ui_frame(&fixture);
    }
    const tb_ui_command *const text = ui_text_command(&fixture, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
    TB_TEST_REQUIRE_POINTER(text, !=, NULL);
    const tb_canvas_rect field = ui_rect_of(&fixture, fixture.ids[0]);
    TB_TEST_EXPECT(text->rect.x < field.x);
    TB_TEST_EXPECT_FLOAT_NEAR(text->rect.x + text->rect.width,
                              field.x + field.width - tb_ui_get_theme(fixture.ui)->frame_padding.x,
                              1.5f);
    key(&fixture, TB_KEY_HOME, 0);
    TB_TEST_EXPECT_FLOAT_NEAR(ui_text_command(&fixture, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwww")->rect.x,
                              field.x + tb_ui_get_theme(fixture.ui)->frame_padding.x, 0.01f);

    memset(fixture.text + 48, 'z', 8);
    ui_frame(&fixture);
    TB_TEST_EXPECT_STRING(fixture.text + 48, ==, "zzzzzzz");
    ui_fixture_close(&fixture);
}
