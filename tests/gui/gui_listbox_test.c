/* The listbox: items inserted, deleted, and read as one text; chosen by the
 * mouse and the keyboard in each selection mode, with its command called;
 * scrolled by the wheel, its bar, and to show an item; and a list of many
 * thousands drawing only the rows that can be seen. */
#include "gui_fixture.h"
#include "tb_test.h"

#include "badger/str.h"
#include "badger/ui.h"

#include "badger/fmt.h"

static const char *const fruits[] = {"Apple", "Banana", "Cherry", "Damson", "Elder", "Fig"};

typedef struct list_case
{
    gui_fixture fixture;
    tb_widget list;
    int changes;
    float row; /* a row's height, in units */
} list_case;

static void count_change(const tb_gui_event *event, void *user)
{
    int *const changes = user;

    (void)event;
    (*changes)++;
}

static void open_list(list_case *made, tb_listbox_selectmode mode, int32_t height)
{
    gui_fixture_open(&made->fixture, 1.0f);
    made->list =
        tb_listbox(tb_gui_root(made->fixture.gui), &(tb_listbox_desc){.selectmode = mode,
                                                                      .width = 200.0f,
                                                                      .height = height,
                                                                      .command = count_change,
                                                                      .user = &made->changes,
                                                                      .items = fruits,
                                                                      .item_count = 6});
    TB_TEST_REQUIRE(tb_grid(made->list, NULL));
    gui_update(&made->fixture);
    const size_t rows = height > 0 ? (size_t)height : 6u;
    made->row = (tb_widget_natural_size(made->list).y - 2.0f) / (float)rows;
}

/* The middle of a row, as the list is scrolled now. */
static tb_vec2 row_middle(const list_case *made, size_t row)
{
    const tb_canvas_rect rect = tb_widget_rect(made->list);
    tb_gui_value offset = {0};

    TB_TEST_REQUIRE(tb_widget_get(made->list, "offset", &offset));
    return (tb_vec2){rect.x + 50.0f,
                     rect.y + 1.0f + ((float)row + 0.5f) * made->row - offset.number};
}

static void click_row(list_case *made, size_t row, uint32_t modifiers)
{
    const tb_vec2 at = row_middle(made, row);
    tb_window_event down =
        gui_mouse_event(&made->fixture, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, at.x, at.y);
    tb_window_event up =
        gui_mouse_event(&made->fixture, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, at.x, at.y);

    down.button = TB_MOUSE_BUTTON_LEFT;
    down.modifiers = modifiers;
    up.button = TB_MOUSE_BUTTON_LEFT;
    gui_move(&made->fixture, at.x, at.y);
    tb_gui_feed_event(made->fixture.gui, &down);
    tb_gui_feed_event(made->fixture.gui, &up);
    gui_update(&made->fixture);
}

static size_t selection_of(const list_case *made, size_t *indices, size_t capacity)
{
    return tb_listbox_selection(made->list, indices, capacity);
}

TB_TEST(gui_listbox, items_are_inserted_deleted_and_read_as_one_text)
{
    list_case made = {0};
    open_list(&made, TB_LISTBOX_BROWSE, 0);
    tb_gui_value value = {0};

    TB_TEST_EXPECT_UINT(tb_listbox_size(made.list), ==, 6);
    TB_TEST_EXPECT(tb_text_equal(tb_listbox_get(made.list, 2), TB_TEXT("Cherry")));
    TB_TEST_EXPECT_UINT(tb_listbox_get(made.list, 9).length, ==, 0);
    TB_TEST_EXPECT_UINT(tb_listbox_insert(made.list, 0, "Acorn"), ==, 0);
    TB_TEST_EXPECT_UINT(tb_listbox_insert(made.list, SIZE_MAX, "Grape"), ==, 7);
    TB_TEST_EXPECT(tb_text_equal(tb_listbox_get(made.list, 1), TB_TEXT("Apple")));
    TB_TEST_EXPECT_UINT(tb_listbox_delete(made.list, 6, 5), ==, 2);
    TB_TEST_EXPECT_UINT(tb_listbox_delete(made.list, 9, 1), ==, 0);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "items", &value));
    TB_TEST_EXPECT(
        tb_text_equal(value.text, TB_TEXT("Acorn\nApple\nBanana\nCherry\nDamson\nElder")));

    /* Set as one text, a line an item; the list grows to show them all. */
    const float before = tb_widget_rect(made.list).height;
    const tb_gui_value lines = tb_gui_text(TB_TEXT("One\nTwo\n\nFour"));
    TB_TEST_REQUIRE(tb_widget_set(made.list, "items", &lines));
    TB_TEST_EXPECT_UINT(tb_listbox_size(made.list), ==, 4);
    TB_TEST_EXPECT_UINT(tb_listbox_get(made.list, 2).length, ==, 0);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.list).height, <, before);
    const tb_gui_value none = tb_gui_text(TB_TEXT(""));
    TB_TEST_REQUIRE(tb_widget_set(made.list, "items", &none));
    TB_TEST_EXPECT_UINT(tb_listbox_size(made.list), ==, 0);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "items", &value));
    TB_TEST_EXPECT_UINT(value.text.length, ==, 0);
    TB_TEST_EXPECT_UINT(tb_listbox_active(made.list), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_listbox_nearest(made.list, 10.0f), ==, SIZE_MAX);

    /* Other kinds, and stale handles, have no items. */
    const tb_widget root = tb_gui_root(made.fixture.gui);
    TB_TEST_EXPECT_UINT(tb_listbox_insert(root, 0, "x"), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_listbox_size(root), ==, 0);
    TB_TEST_EXPECT_UINT(tb_listbox_delete(root, 0, 1), ==, 0);
    TB_TEST_EXPECT(!tb_listbox_select(root, 0, true));
    TB_TEST_EXPECT(!tb_listbox_see(root, 0));
    TB_TEST_EXPECT(!tb_listbox_activate(root, 0));
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(made.fixture.gui, "listbox"), ==, tb_listbox_class());
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_listbox, a_browse_list_chooses_one_by_click_drag_and_arrows)
{
    list_case made = {0};
    open_list(&made, TB_LISTBOX_BROWSE, 0);
    size_t chosen[8] = {0};
    tb_gui_stats stats = {0};

    click_row(&made, 2, 0);
    TB_TEST_REQUIRE_UINT(selection_of(&made, chosen, 8), ==, 1);
    TB_TEST_EXPECT_UINT(chosen[0], ==, 2);
    TB_TEST_EXPECT_INT(made.changes, ==, 1);
    TB_TEST_EXPECT_UINT(tb_listbox_active(made.list), ==, 2);
    /* A click takes the focus. */
    tb_gui_stats_of(made.fixture.gui, &stats);
    TB_TEST_EXPECT(tb_widget_equal(stats.focus, made.list));

    /* Dragged, the selection follows the mouse. */
    const tb_vec2 from = row_middle(&made, 0);
    const tb_vec2 to = row_middle(&made, 4);
    gui_move(&made.fixture, from.x, from.y);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, to.x, to.y);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_REQUIRE_UINT(selection_of(&made, chosen, 8), ==, 1);
    TB_TEST_EXPECT_UINT(chosen[0], ==, 4);

    /* The arrows, Home, and End move it. */
    gui_key(&made.fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 3));
    gui_key(&made.fixture, TB_KEY_END, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 5));
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 5));
    gui_key(&made.fixture, TB_KEY_HOME, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 0));
    gui_key(&made.fixture, TB_KEY_UP, 0);
    gui_key(&made.fixture, TB_KEY_PAGE_DOWN, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 5));
    gui_key(&made.fixture, TB_KEY_PAGE_UP, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 0));
    TB_TEST_REQUIRE_UINT(selection_of(&made, chosen, 8), ==, 1);
    /* Keys it does not use go on up the tree. */
    gui_key(&made.fixture, TB_KEY_F1, 0);

    /* By hand: one alone stays chosen in a mode of one, and the command is
     * not called. */
    const int before = made.changes;
    TB_TEST_EXPECT(tb_listbox_select(made.list, 3, true));
    TB_TEST_EXPECT_UINT(selection_of(&made, NULL, 0), ==, 1);
    TB_TEST_EXPECT(tb_listbox_select(made.list, 3, false));
    TB_TEST_EXPECT_UINT(selection_of(&made, NULL, 0), ==, 0);
    TB_TEST_EXPECT(!tb_listbox_select(made.list, 30, true));
    TB_TEST_EXPECT_INT(made.changes, ==, before);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_listbox, an_extended_list_chooses_ranges_with_shift_and_toggles_with_control)
{
    list_case made = {0};
    open_list(&made, TB_LISTBOX_EXTENDED, 0);
    size_t chosen[8] = {0};

    click_row(&made, 1, 0);
    click_row(&made, 3, TB_MODIFIER_SHIFT);
    TB_TEST_REQUIRE_UINT(selection_of(&made, chosen, 8), ==, 3);
    TB_TEST_EXPECT_UINT(chosen[0], ==, 1);
    TB_TEST_EXPECT_UINT(chosen[2], ==, 3);
    click_row(&made, 5, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 4);
    click_row(&made, 2, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT(!tb_listbox_selected(made.list, 2));
    /* A plain click chooses one again. */
    click_row(&made, 0, 0);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 1);

    /* Shift and the arrows extend from the anchor; Control moves only the
     * active item; Space toggles it. */
    gui_key(&made.fixture, TB_KEY_DOWN, TB_MODIFIER_SHIFT);
    gui_key(&made.fixture, TB_KEY_DOWN, TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 3);
    gui_key(&made.fixture, TB_KEY_DOWN, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(tb_listbox_active(made.list), ==, 3);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 3);
    gui_key(&made.fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 3));

    /* Dragged from a row, it selects from it. */
    const tb_vec2 from = row_middle(&made, 5);
    const tb_vec2 to = row_middle(&made, 3);
    gui_move(&made.fixture, from.x, from.y);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, to.x, to.y);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_REQUIRE_UINT(selection_of(&made, chosen, 8), ==, 3);
    TB_TEST_EXPECT_UINT(chosen[0], ==, 3);

    /* Control+A chooses them all. */
    gui_key(&made.fixture, TB_KEY_A, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 2), ==, 6);
    tb_listbox_clear_selection(made.list);
    TB_TEST_EXPECT_UINT(selection_of(&made, NULL, 0), ==, 0);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_listbox, a_multiple_list_toggles_each_click_and_space)
{
    list_case made = {0};
    open_list(&made, TB_LISTBOX_MULTIPLE, 0);
    size_t chosen[8] = {0};

    click_row(&made, 1, 0);
    click_row(&made, 4, 0);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 2);
    click_row(&made, 1, 0);
    TB_TEST_REQUIRE_UINT(selection_of(&made, chosen, 8), ==, 1);
    TB_TEST_EXPECT_UINT(chosen[0], ==, 4);
    /* The arrows move only the active item; Space turns it. */
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 1);
    gui_key(&made.fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT(tb_listbox_selected(made.list, 2));
    TB_TEST_EXPECT(tb_listbox_select(made.list, 0, true));
    TB_TEST_EXPECT_UINT(selection_of(&made, chosen, 8), ==, 3);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_listbox, a_short_list_scrolls_itself_by_wheel_bar_and_see)
{
    list_case made = {0};
    open_list(&made, TB_LISTBOX_SINGLE, 3);
    tb_gui_value offset = {0};
    /* As far as it scrolls: its rows past its view, on whole pixels. */
    const float most = 6.0f * made.row - (tb_widget_rect(made.list).height - 2.0f);

    /* Three rows shown of six, with a bar beside them. */
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.list).width, 200.0f + 10.0f + 2.0f, 1e-3);
    const tb_vec2 middle = gui_middle(tb_widget_rect(made.list));
    gui_move(&made.fixture, middle.x, middle.y);
    gui_wheel(&made.fixture, -1.0f);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, 3.0f * made.row, 1e-3);
    /* At its end, the wheel does nothing more. */
    gui_wheel(&made.fixture, -1.0f);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, most, 1e-3);

    /* See brings a row into view as little as it must. */
    TB_TEST_EXPECT(tb_listbox_see(made.list, 1));
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, made.row, 1e-3);
    TB_TEST_EXPECT(tb_listbox_see(made.list, 5));
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, most, 1e-3);
    TB_TEST_EXPECT(!tb_listbox_see(made.list, 6));

    /* The nearest row to a height, as it is scrolled. */
    const tb_canvas_rect rect = tb_widget_rect(made.list);
    TB_TEST_EXPECT_UINT(tb_listbox_nearest(made.list, rect.y + 2.0f), ==, 3);
    TB_TEST_EXPECT_UINT(tb_listbox_nearest(made.list, rect.y - 50.0f), ==, 0);
    TB_TEST_EXPECT_UINT(tb_listbox_nearest(made.list, rect.y + 500.0f), ==, 5);

    /* The bar: paged up by a press above its thumb, then dragged down. */
    const float bar_x = rect.x + rect.width - 6.0f;
    gui_click(&made.fixture, (tb_vec2){bar_x, rect.y + 3.0f});
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, most - (rect.height - 2.0f), 1e-3);
    gui_wheel(&made.fixture, 1.0f);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, 0.0f, 1e-3);
    gui_move(&made.fixture, bar_x, rect.y + 3.0f);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, bar_x, rect.y + rect.height);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, most, 1e-3);

    /* Set by name, kept within its rows. */
    const tb_gui_value top = tb_gui_float(0.0f);
    TB_TEST_REQUIRE(tb_widget_set(made.list, "offset", &top));
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(made.list, "offset", &negative));
    const tb_gui_value fewer = tb_gui_int(-1);
    TB_TEST_EXPECT(!tb_widget_set(made.list, "height", &fewer));
    TB_TEST_EXPECT(!tb_widget_set(made.list, "width", &negative));

    /* The arrows keep the active row in view. */
    click_row(&made, 0, 0);
    gui_key(&made.fixture, TB_KEY_END, 0);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "offset", &offset));
    TB_TEST_EXPECT_FLOAT_NEAR(offset.number, most, 1e-3);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "height", &offset));
    TB_TEST_EXPECT_INT(offset.integer, ==, 3);
    TB_TEST_REQUIRE(tb_widget_get(made.list, "width", &offset));
    TB_TEST_EXPECT_FLOAT(offset.number, ==, 200.0f);
    gui_leave(&made.fixture);
    gui_fixture_close(&made.fixture);
}

/* Text commands a widget drew. */
static size_t texts_of(const gui_fixture *fixture, tb_widget widget)
{
    size_t count = 0;
    size_t texts = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == (tb_ui_id)widget.index + 1u &&
            commands[index].type == TB_UI_COMMAND_TEXT)
        {
            texts++;
        }
    }
    return texts;
}

TB_TEST(gui_listbox, a_list_of_many_draws_only_the_rows_seen)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget list = tb_listbox(tb_gui_root(fixture.gui), &(tb_listbox_desc){.height = 10});
    char text[32] = {0};

    for (int index = 0; index < 10000; index++)
    {
        (void)tb_fmt_buffer(text, sizeof(text), "Item %d", index);
        (void)tb_listbox_insert(list, SIZE_MAX, text);
    }
    TB_TEST_REQUIRE(tb_grid(list, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(texts_of(&fixture, list), <=, 11);
    TB_TEST_EXPECT_UINT(texts_of(&fixture, list), >=, 10);
    TB_TEST_EXPECT(tb_listbox_see(list, 5000));
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(texts_of(&fixture, list), <=, 11);
    TB_TEST_EXPECT(tb_listbox_activate(list, 5000));
    TB_TEST_EXPECT_UINT(tb_listbox_active(list), ==, 5000);

    /* In a scroll frame, a list as tall as its rows draws those the frame
     * shows, and see scrolls the frame. */
    tb_widget_destroy(list);
    const tb_widget frame =
        tb_scrollframe(tb_gui_root(fixture.gui), &(tb_scrollframe_desc){.height = 200.0f});
    const tb_widget tall = tb_listbox(frame, NULL);
    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(tall, NULL));
    for (int index = 0; index < 2000; index++)
    {
        (void)tb_fmt_buffer(text, sizeof(text), "Row %d", index);
        (void)tb_listbox_insert(tall, SIZE_MAX, text);
    }
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(texts_of(&fixture, tall), <=, 12);
    TB_TEST_EXPECT(tb_listbox_see(tall, 1000));
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_scroll_offset(frame).y, >, 0.0f);
    TB_TEST_EXPECT_UINT(texts_of(&fixture, tall), <=, 12);
    TB_TEST_EXPECT_UINT(texts_of(&fixture, tall), >=, 9);
    gui_fixture_close(&fixture);
}
