/* Notebooks and paned windows: tabs chosen by clicks and keys, their
 * pages shown one at a time, and pages destroyed; panes sharing their
 * window's space by their weights, sashes dragged and placed, and least
 * sizes kept. */
#include "gui_fixture.h"
#include "tb_test.h"

typedef struct book_case
{
    gui_fixture fixture;
    tb_widget book;
    tb_widget pages[3];
    tb_widget field;
    int switches;
} book_case;

static void count_switch(const tb_gui_event *event, void *user)
{
    int *const switches = user;

    (void)event;
    (*switches)++;
}

static void open_book(book_case *made)
{
    static const char *const titles[] = {"General", "Graphics", "Audio"};

    gui_fixture_open(&made->fixture, 1.0f);
    made->book = tb_notebook(tb_gui_root(made->fixture.gui),
                             &(tb_notebook_desc){.command = count_switch, .user = &made->switches});
    TB_TEST_REQUIRE(tb_grid(made->book, NULL));
    for (size_t index = 0; index < 3; index++)
    {
        made->pages[index] = gui_box(made->book, 200.0f, 100.0f + 20.0f * (float)index);
        TB_TEST_REQUIRE_UINT(tb_notebook_add(made->book, made->pages[index], titles[index]), ==,
                             index);
    }
    made->field = gui_box_of(made->pages[0], gui_typing_box_class(), 50.0f, 20.0f);
    TB_TEST_REQUIRE(tb_grid(made->field, NULL));
    gui_update(&made->fixture);
}

/* The middle of a tab: the tabs share the strip by their text, so the
 * first is found from the left, the others by walking right. */
static tb_vec2 tab_point(const book_case *made, size_t index)
{
    const tb_canvas_rect rect = tb_widget_rect(made->book);
    const tb_gui_text_block blocks[] = {
        {.text = TB_TEXT("General")}, {.text = TB_TEXT("Graphics")}, {.text = TB_TEXT("Audio")}};
    const float padding = tb_gui_theme(made->fixture.gui)->frame_padding.x;
    float left = rect.x;

    for (size_t at = 0; at < index; at++)
    {
        left += tb_gui_measure_text(made->fixture.gui, &blocks[at]).x + 2.0f * padding;
    }
    return (tb_vec2){left + padding + 2.0f, rect.y + 4.0f};
}

TB_TEST(gui_notebook, a_click_on_a_tab_shows_its_page_alone)
{
    book_case made = {0};
    open_book(&made);
    tb_gui_value value = {0};

    TB_TEST_EXPECT_UINT(tb_notebook_tab_count(made.book), ==, 3);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    TB_TEST_EXPECT(tb_widget_rect(made.pages[0]).width > 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.pages[1]).width, ==, 0.0f);
    gui_click(&made.fixture, tab_point(&made, 2));
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 2);
    TB_TEST_EXPECT_INT(made.switches, ==, 1);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.pages[0]).width, ==, 0.0f);
    TB_TEST_EXPECT(tb_widget_rect(made.pages[2]).width > 0.0f);
    /* The page fills the notebook below its tabs. */
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.pages[2]).y, >, tb_widget_rect(made.book).y);
    /* Clicking it again changes nothing. */
    gui_click(&made.fixture, tab_point(&made, 2));
    TB_TEST_EXPECT_INT(made.switches, ==, 1);

    /* By hand and by name, without the command. */
    TB_TEST_EXPECT(tb_notebook_select(made.book, 1));
    TB_TEST_EXPECT(!tb_notebook_select(made.book, 3));
    TB_TEST_REQUIRE(tb_widget_get(made.book, "selected", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 1);
    const tb_gui_value first = tb_gui_int(0);
    TB_TEST_REQUIRE(tb_widget_set(made.book, "selected", &first));
    const tb_gui_value past = tb_gui_int(7);
    TB_TEST_EXPECT(!tb_widget_set(made.book, "selected", &past));
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    TB_TEST_EXPECT_INT(made.switches, ==, 1);
    TB_TEST_EXPECT(tb_widget_equal(tb_notebook_page(made.book, 2), made.pages[2]));
    TB_TEST_EXPECT(!tb_widget_alive(tb_notebook_page(made.book, 3)));
    TB_TEST_EXPECT(tb_notebook_set_tab_text(made.book, 2, "Sound"));
    TB_TEST_EXPECT(!tb_notebook_set_tab_text(made.book, 3, "Sound"));
    const tb_gui_value padding = tb_gui_float(4.0f);
    TB_TEST_REQUIRE(tb_widget_set(made.book, "padding", &padding));
    const tb_gui_value negative = tb_gui_float(-4.0f);
    TB_TEST_EXPECT(!tb_widget_set(made.book, "padding", &negative));
    TB_TEST_REQUIRE(tb_widget_get(made.book, "padding", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 4.0f);
    gui_move(&made.fixture, 700.0f, 500.0f);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_notebook, control_tab_from_inside_and_the_arrows_on_it_go_from_tab_to_tab)
{
    book_case made = {0};
    open_book(&made);

    /* From a field in the page: Control+Tab, Control+Shift+Tab around. */
    gui_click(&made.fixture, gui_middle(tb_widget_rect(made.field)));
    gui_key(&made.fixture, TB_KEY_TAB, TB_MODIFIER_CONTROL | TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 2);
    TB_TEST_EXPECT_INT(made.switches, ==, 1);
    /* The field's page is hidden: the focus left it. */
    TB_TEST_EXPECT(!tb_gui_wants_keyboard(made.fixture.gui));

    /* With the focus on the notebook itself. */
    TB_TEST_REQUIRE(tb_widget_focus(made.book));
    gui_key(&made.fixture, TB_KEY_TAB, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    gui_key(&made.fixture, TB_KEY_PAGE_DOWN, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 1);
    gui_key(&made.fixture, TB_KEY_PAGE_UP, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 2);
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    gui_key(&made.fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_notebook, a_page_destroyed_takes_its_tab)
{
    book_case made = {0};
    open_book(&made);

    TB_TEST_EXPECT(tb_notebook_select(made.book, 1));
    tb_widget_destroy(made.pages[1]);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_UINT(tb_notebook_tab_count(made.book), ==, 2);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, 0);
    TB_TEST_EXPECT(tb_widget_rect(made.pages[0]).width > 0.0f);
    TB_TEST_EXPECT(tb_widget_equal(tb_notebook_page(made.book, 1), made.pages[2]));
    /* Only its own children are its pages. */
    const tb_widget stranger = gui_box(tb_gui_root(made.fixture.gui), 10.0f, 10.0f);
    TB_TEST_EXPECT_UINT(tb_notebook_add(made.book, stranger, "x"), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_notebook_add(stranger, stranger, "x"), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_notebook_tab_count(stranger), ==, 0);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(stranger), ==, SIZE_MAX);
    TB_TEST_EXPECT(!tb_notebook_select(stranger, 0));
    TB_TEST_EXPECT(!tb_notebook_set_tab_text(stranger, 0, "x"));
    TB_TEST_EXPECT(!tb_widget_alive(tb_notebook_page(stranger, 0)));
    /* Every page destroyed: none chosen. */
    tb_widget_destroy(made.pages[0]);
    tb_widget_destroy(made.pages[2]);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_UINT(tb_notebook_tab_count(made.book), ==, 0);
    TB_TEST_EXPECT_UINT(tb_notebook_selected(made.book), ==, SIZE_MAX);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(made.fixture.gui, "notebook"), ==,
                           tb_notebook_class());
    gui_fixture_close(&made.fixture);
}

/* ---------------------------------------------------------------------------
 * Paned windows.
 * ------------------------------------------------------------------------- */

typedef struct paned_case
{
    gui_fixture fixture;
    tb_widget paned;
    tb_widget left;
    tb_widget right;
} paned_case;

static void open_paned(paned_case *made, tb_gui_orient orient)
{
    gui_fixture_open(&made->fixture, 1.0f);
    made->paned =
        tb_panedwindow(tb_gui_root(made->fixture.gui),
                       &(tb_panedwindow_desc){.orient = orient, .width = 408.0f, .height = 408.0f});
    TB_TEST_REQUIRE(tb_grid(made->paned, NULL));
    made->left = gui_box(made->paned, 50.0f, 50.0f);
    made->right = gui_box(made->paned, 300.0f, 50.0f);
    TB_TEST_REQUIRE_UINT(tb_panedwindow_add(made->paned, made->left,
                                            &(tb_pane_options){.weight = 1.0f, .minsize = 40.0f}),
                         ==, 0);
    TB_TEST_REQUIRE_UINT(tb_panedwindow_add(made->paned, made->right,
                                            &(tb_pane_options){.weight = 3.0f, .minsize = 60.0f}),
                         ==, 1);
    gui_update(&made->fixture);
}

TB_TEST(gui_paned, panes_share_the_space_by_their_weights_whatever_they_want)
{
    paned_case made = {0};
    open_paned(&made, TB_GUI_ORIENT_HORIZONTAL);
    const float sash = tb_gui_theme(made.fixture.gui)->item_spacing.x;

    /* 408 less the sash, shared one to three, though the right pane wants
     * more than its share and the left less. */
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.paned).width, ==, 408.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.left).width, (408.0f - sash) * 0.25f, 1.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.right).width, (408.0f - sash) * 0.75f, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.left).height, ==, 408.0f);
    TB_TEST_EXPECT_UINT(tb_panedwindow_pane_count(made.paned), ==, 2);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_panedwindow_sash(made.paned, 0),
                              tb_widget_rect(made.left).x + tb_widget_rect(made.left).width, 1e-3);
    TB_TEST_EXPECT_FLOAT(tb_panedwindow_sash(made.paned, 1), ==, 0.0f);

    /* Resized, each keeps its share. */
    const tb_gui_value wider = tb_gui_float(808.0f);
    TB_TEST_REQUIRE(tb_widget_set(made.paned, "width", &wider));
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.left).width, (808.0f - sash) * 0.25f, 1.0f);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(made.fixture.gui, "panedwindow"), ==,
                           tb_panedwindow_class());
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_paned, a_sash_is_dragged_and_placed_its_panes_kept_at_their_least)
{
    paned_case made = {0};
    open_paned(&made, TB_GUI_ORIENT_HORIZONTAL);
    const float sash = tb_gui_theme(made.fixture.gui)->item_spacing.x;
    const float start = tb_panedwindow_sash(made.paned, 0);
    const float y = 100.0f;

    gui_move(&made.fixture, start + sash * 0.5f, y);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(made.fixture.gui), ==, TB_CURSOR_RESIZE_HORIZONTAL);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, 200.0f + sash * 0.5f, y);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.left).width, 200.0f, 1.0f);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.right).width, 408.0f - sash - 200.0f, 1.0f);

    /* Dragged past a pane's least size, it stops there. */
    TB_TEST_EXPECT(tb_panedwindow_set_sash(made.paned, 0, 5.0f));
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.left).width, 40.0f, 1.0f);
    TB_TEST_EXPECT(tb_panedwindow_set_sash(made.paned, 0, 1000.0f));
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.right).width, 60.0f, 1.0f);
    TB_TEST_EXPECT(!tb_panedwindow_set_sash(made.paned, 1, 10.0f));
    /* Over a pane, the cursor is its own again. */
    gui_move(&made.fixture, 20.0f, y);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(made.fixture.gui), ==, TB_CURSOR_ARROW);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_paned, a_vertical_window_stacks_its_panes)
{
    paned_case made = {0};
    open_paned(&made, TB_GUI_ORIENT_VERTICAL);
    const float sash = tb_gui_theme(made.fixture.gui)->item_spacing.x;
    tb_gui_value value = {0};

    TB_TEST_EXPECT_FLOAT_NEAR(tb_widget_rect(made.left).height, (408.0f - sash) * 0.25f, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.left).width, ==, 408.0f);
    const float start = tb_panedwindow_sash(made.paned, 0);
    gui_move(&made.fixture, 100.0f, start + 1.0f);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, 100.0f, start + 51.0f);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(made.fixture.gui), ==, TB_CURSOR_RESIZE_VERTICAL);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_FLOAT_NEAR(tb_panedwindow_sash(made.paned, 0), start + 50.0f, 1.0f);

    /* Its orientation is chosen before its panes, and its size by name. */
    const tb_gui_value across = tb_gui_enum(TB_GUI_ORIENT_HORIZONTAL);
    TB_TEST_EXPECT(!tb_widget_set(made.paned, "orient", &across));
    TB_TEST_REQUIRE(tb_widget_get(made.paned, "orient", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, TB_GUI_ORIENT_VERTICAL);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(made.paned, "width", &negative));
    TB_TEST_EXPECT(!tb_widget_set(made.paned, "height", &negative));
    TB_TEST_REQUIRE(tb_widget_get(made.paned, "height", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 408.0f);
    TB_TEST_REQUIRE(tb_widget_get(made.paned, "width", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 408.0f);
    /* Only its children are its panes; and with none, it wants its panes' size. */
    const tb_widget empty = tb_panedwindow(tb_gui_root(made.fixture.gui), NULL);
    TB_TEST_EXPECT_UINT(tb_panedwindow_add(empty, made.left, NULL), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_panedwindow_add(made.left, made.left, NULL), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_panedwindow_pane_count(made.left), ==, 0);
    const tb_widget pane = gui_box(empty, 30.0f, 20.0f);
    TB_TEST_EXPECT_UINT(tb_panedwindow_add(empty, pane, &(tb_pane_options){.weight = -1.0f}), ==,
                        SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_panedwindow_add(empty, pane, NULL), ==, 0);
    TB_TEST_REQUIRE(tb_grid(empty, &(tb_grid_options){.column = 1}));
    gui_update(&made.fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(empty).x, ==, 30.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(empty).y, ==, 20.0f);
    gui_leave(&made.fixture);
    gui_fixture_close(&made.fixture);
}
