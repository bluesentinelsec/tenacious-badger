/* The tree view: items inserted, walked, changed, and deleted as data;
 * rows of the items open; items opened and closed by their marks, double
 * clicks, and the arrows; the selection by the mouse and the keyboard;
 * headings resized by dragging; and scrolling to show an item. */
#include "gui_fixture.h"
#include "tb_test.h"

#include "badger/ui.h"

typedef struct tree_case
{
    gui_fixture fixture;
    tb_widget tree;
    tb_tree_item world;
    tb_tree_item lights;
    tb_tree_item sun;
    tb_tree_item lamp;
    tb_tree_item meshes;
    tb_tree_item crate;
    int changes;
    int opens;
    float row;
} tree_case;

static void count_change(const tb_gui_event *event, void *user)
{
    tree_case *const made = user;

    (void)event;
    made->changes++;
}

static void count_open(const tb_gui_event *event, void *user)
{
    tree_case *const made = user;

    TB_TEST_EXPECT_UINT(event->kind, ==, TB_GUI_EVENT_COMMAND);
    made->opens++;
}

static tb_tree_item insert(tree_case *made, tb_tree_item parent, const char *text, const char *type,
                           bool open)
{
    const char *const values[] = {type};
    const tb_tree_item item = tb_treeview_insert(
        made->tree, parent, SIZE_MAX,
        &(tb_tree_item_desc){.text = text, .values = values, .value_count = 1, .open = open});
    TB_TEST_REQUIRE_UINT(item, !=, TB_TREE_ROOT);
    return item;
}

/* World (open) > Lights (open) > Sun, Lamp; Meshes (closed) > Crate. */
static void open_tree(tree_case *made, tb_listbox_selectmode mode, int32_t height)
{
    gui_fixture_open(&made->fixture, 1.0f);
    made->tree =
        tb_treeview(tb_gui_root(made->fixture.gui), &(tb_treeview_desc){.selectmode = mode,
                                                                        .height = height,
                                                                        .heading = "Name",
                                                                        .command = count_change,
                                                                        .open_command = count_open,
                                                                        .user = made});
    TB_TEST_REQUIRE(tb_grid(made->tree, NULL));
    TB_TEST_REQUIRE_UINT(
        tb_treeview_add_column(made->tree,
                               &(tb_treeview_column_desc){.heading = "Type", .width = 80.0f}),
        ==, 0);
    made->world = insert(made, TB_TREE_ROOT, "World", "Level", true);
    made->lights = insert(made, made->world, "Lights", "Group", true);
    made->sun = insert(made, made->lights, "Sun", "Light", false);
    made->lamp = insert(made, made->lights, "Lamp", "Light", false);
    made->meshes = insert(made, made->world, "Meshes", "Group", false);
    made->crate = insert(made, made->meshes, "Crate", "Mesh", false);
    gui_update(&made->fixture);
    /* Headings and the rows of World, Lights, Sun, Lamp, and Meshes. */
    const float rows = height > 0 ? (float)height : 5.0f;
    made->row = (tb_widget_natural_size(made->tree).y - 2.0f) / (rows + 1.0f);
}

/* The middle of a row's text, by its row from the top, as scrolled now. */
static tb_vec2 row_at(const tree_case *made, size_t row)
{
    const tb_canvas_rect rect = tb_widget_rect(made->tree);
    tb_gui_value offset = {0};

    TB_TEST_REQUIRE(tb_widget_get(made->tree, "offset", &offset));
    return (tb_vec2){rect.x + 150.0f,
                     rect.y + 1.0f + made->row * ((float)row + 1.5f) - offset.number};
}

TB_TEST(gui_treeview, items_are_data_inserted_walked_changed_and_deleted)
{
    tree_case made = {0};
    open_tree(&made, TB_LISTBOX_BROWSE, 0);

    TB_TEST_EXPECT(tb_treeview_exists(made.tree, made.sun));
    TB_TEST_EXPECT(!tb_treeview_exists(made.tree, TB_TREE_ROOT));
    TB_TEST_EXPECT_UINT(tb_treeview_first_child(made.tree, TB_TREE_ROOT), ==, made.world);
    TB_TEST_EXPECT_UINT(tb_treeview_first_child(made.tree, made.lights), ==, made.sun);
    TB_TEST_EXPECT_UINT(tb_treeview_next_sibling(made.tree, made.sun), ==, made.lamp);
    TB_TEST_EXPECT_UINT(tb_treeview_next_sibling(made.tree, made.lamp), ==, TB_TREE_ROOT);
    TB_TEST_EXPECT_UINT(tb_treeview_parent(made.tree, made.crate), ==, made.meshes);
    TB_TEST_EXPECT_UINT(tb_treeview_parent(made.tree, made.world), ==, TB_TREE_ROOT);
    TB_TEST_EXPECT(tb_text_equal(tb_treeview_text(made.tree, made.crate), TB_TEXT("Crate")));
    TB_TEST_EXPECT(tb_text_equal(tb_treeview_value(made.tree, made.crate, 0), TB_TEXT("Mesh")));
    TB_TEST_EXPECT_UINT(tb_treeview_value(made.tree, made.crate, 3).length, ==, 0);

    /* Inserted before a child, and set. */
    const tb_tree_item first =
        tb_treeview_insert(made.tree, made.lights, 0, &(tb_tree_item_desc){.text = "Fill"});
    TB_TEST_EXPECT_UINT(tb_treeview_first_child(made.tree, made.lights), ==, first);
    TB_TEST_EXPECT_UINT(tb_treeview_next_sibling(made.tree, first), ==, made.sun);
    TB_TEST_EXPECT(tb_treeview_set_text(made.tree, first, "Fill light"));
    TB_TEST_EXPECT(tb_treeview_set_value(made.tree, first, 2, "Spot"));
    TB_TEST_EXPECT(tb_text_equal(tb_treeview_value(made.tree, first, 2), TB_TEXT("Spot")));
    TB_TEST_EXPECT(tb_text_equal(tb_treeview_text(made.tree, first), TB_TEXT("Fill light")));
    /* Inserted between two children. */
    const tb_tree_item middle =
        tb_treeview_insert(made.tree, made.lights, 2, &(tb_tree_item_desc){.text = "Rim"});
    TB_TEST_EXPECT_UINT(tb_treeview_next_sibling(made.tree, made.sun), ==, middle);
    TB_TEST_EXPECT_UINT(tb_treeview_next_sibling(made.tree, middle), ==, made.lamp);

    /* Deleted with what is in it; its id is never another item's. */
    TB_TEST_EXPECT(tb_treeview_delete(made.tree, made.meshes));
    TB_TEST_EXPECT(!tb_treeview_exists(made.tree, made.crate));
    TB_TEST_EXPECT(!tb_treeview_delete(made.tree, made.crate));
    TB_TEST_EXPECT(!tb_treeview_delete(made.tree, TB_TREE_ROOT));
    const tb_tree_item again = insert(&made, made.world, "Props", "Group", false);
    TB_TEST_EXPECT_UINT(again, !=, made.meshes);
    TB_TEST_EXPECT_UINT(again, !=, made.crate);
    TB_TEST_EXPECT(tb_treeview_delete(made.tree, first));
    TB_TEST_EXPECT(tb_treeview_delete(made.tree, made.lamp));
    TB_TEST_EXPECT_UINT(tb_treeview_next_sibling(made.tree, middle), ==, TB_TREE_ROOT);

    /* Items not there, and other kinds, have nothing. */
    TB_TEST_EXPECT_UINT(tb_treeview_insert(made.tree, made.crate, 0, NULL), ==, TB_TREE_ROOT);
    TB_TEST_EXPECT(!tb_treeview_set_text(made.tree, made.crate, "x"));
    TB_TEST_EXPECT(!tb_treeview_set_value(made.tree, made.crate, 0, "x"));
    TB_TEST_EXPECT(!tb_treeview_set_open(made.tree, made.crate, true));
    TB_TEST_EXPECT(!tb_treeview_select(made.tree, made.crate, true));
    TB_TEST_EXPECT(!tb_treeview_see(made.tree, made.crate));
    TB_TEST_EXPECT(!tb_treeview_set_focus_item(made.tree, made.crate));
    TB_TEST_EXPECT_UINT(tb_treeview_text(made.tree, made.crate).length, ==, 0);
    const tb_widget root = tb_gui_root(made.fixture.gui);
    TB_TEST_EXPECT_UINT(tb_treeview_insert(root, TB_TREE_ROOT, 0, NULL), ==, TB_TREE_ROOT);
    TB_TEST_EXPECT_UINT(tb_treeview_add_column(root, NULL), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(root), ==, TB_TREE_ROOT);
    TB_TEST_EXPECT_UINT(tb_treeview_identify_row(root, 0.0f), ==, TB_TREE_ROOT);
    TB_TEST_EXPECT_UINT(tb_treeview_selection(root, NULL, 0), ==, 0);
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(made.fixture.gui, "treeview"), ==,
                           tb_treeview_class());
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_treeview, rows_are_the_items_open_and_marks_and_double_clicks_open_them)
{
    tree_case made = {0};
    open_tree(&made, TB_LISTBOX_BROWSE, 0);

    /* Meshes is closed: five rows, its crate not among them. */
    TB_TEST_EXPECT_UINT(tb_treeview_identify_row(made.tree, row_at(&made, 4).y), ==, made.meshes);
    TB_TEST_EXPECT_UINT(tb_treeview_identify_row(made.tree, row_at(&made, 5).y), ==, TB_TREE_ROOT);
    /* A press on its mark opens it, and calls the open command. */
    const tb_canvas_rect rect = tb_widget_rect(made.tree);
    const float indent = tb_gui_theme(made.fixture.gui)->indent;
    const tb_vec2 mark = {rect.x + 1.0f + indent + made.row * 0.5f, row_at(&made, 4).y};
    gui_click(&made.fixture, mark);
    TB_TEST_EXPECT(tb_treeview_is_open(made.tree, made.meshes));
    TB_TEST_EXPECT_INT(made.opens, ==, 1);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_UINT(tb_treeview_identify_row(made.tree, row_at(&made, 5).y), ==, made.crate);
    /* The mark is not a choice. */
    TB_TEST_EXPECT_INT(made.changes, ==, 0);

    /* A double click on an item with children closes it. */
    const tb_vec2 lights = row_at(&made, 1);
    gui_click(&made.fixture, lights);
    gui_click(&made.fixture, lights);
    TB_TEST_EXPECT(!tb_treeview_is_open(made.tree, made.lights));
    TB_TEST_EXPECT_INT(made.opens, ==, 2);
    TB_TEST_EXPECT(tb_treeview_selected(made.tree, made.lights));
    /* An item without children is not opened. */
    TB_TEST_EXPECT(tb_treeview_set_open(made.tree, made.lights, true));
    gui_update(&made.fixture);
    const tb_vec2 sun = row_at(&made, 2);
    gui_click(&made.fixture, sun);
    gui_click(&made.fixture, sun);
    TB_TEST_EXPECT_INT(made.opens, ==, 2);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_treeview, the_keyboard_moves_opens_closes_and_chooses)
{
    tree_case made = {0};
    open_tree(&made, TB_LISTBOX_BROWSE, 0);
    tb_tree_item chosen[4] = {0};

    gui_click(&made.fixture, row_at(&made, 0));
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.world);
    TB_TEST_EXPECT_INT(made.changes, ==, 1);
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.sun);
    TB_TEST_REQUIRE_UINT(tb_treeview_selection(made.tree, chosen, 4), ==, 1);
    TB_TEST_EXPECT_UINT(chosen[0], ==, made.sun);
    /* Left from a leaf goes to its parent; again, closes it; Right opens it,
     * and again goes into it. */
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.lights);
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT(!tb_treeview_is_open(made.tree, made.lights));
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_treeview_is_open(made.tree, made.lights));
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.sun);
    /* Right on a leaf, and Left at the top, do nothing. */
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.sun);
    gui_key(&made.fixture, TB_KEY_END, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.meshes);
    gui_key(&made.fixture, TB_KEY_PAGE_UP, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.world);
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.world);
    TB_TEST_EXPECT(!tb_treeview_is_open(made.tree, made.world));
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    gui_key(&made.fixture, TB_KEY_PAGE_DOWN, 0);
    gui_key(&made.fixture, TB_KEY_HOME, 0);
    gui_key(&made.fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.world);
    /* Keys it does not use go on up the tree. */
    gui_key(&made.fixture, TB_KEY_F2, 0);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_treeview, an_extended_tree_chooses_ranges_and_toggles)
{
    tree_case made = {0};
    open_tree(&made, TB_LISTBOX_EXTENDED, 0);
    tb_tree_item chosen[8] = {0};

    gui_click(&made.fixture, row_at(&made, 1));
    gui_key(&made.fixture, TB_KEY_DOWN, TB_MODIFIER_SHIFT);
    gui_key(&made.fixture, TB_KEY_DOWN, TB_MODIFIER_SHIFT);
    TB_TEST_EXPECT_UINT(tb_treeview_selection(made.tree, chosen, 8), ==, 3);
    /* Control moves the focus only; Space turns its item. */
    gui_key(&made.fixture, TB_KEY_DOWN, TB_MODIFIER_CONTROL);
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.meshes);
    gui_key(&made.fixture, TB_KEY_SPACE, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_selection(made.tree, chosen, 8), ==, 4);
    gui_key(&made.fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_UINT(tb_treeview_selection(made.tree, chosen, 8), ==, 3);
    /* By hand, without the command. */
    const int before = made.changes;
    TB_TEST_EXPECT(tb_treeview_select(made.tree, made.world, true));
    TB_TEST_EXPECT_UINT(tb_treeview_selection(made.tree, NULL, 0), ==, 4);
    tb_treeview_clear_selection(made.tree);
    TB_TEST_EXPECT_UINT(tb_treeview_selection(made.tree, NULL, 0), ==, 0);
    TB_TEST_EXPECT_INT(made.changes, ==, before);
    TB_TEST_EXPECT(tb_treeview_set_focus_item(made.tree, made.sun));
    TB_TEST_EXPECT_UINT(tb_treeview_focus_item(made.tree), ==, made.sun);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_treeview, a_heading_s_edge_is_dragged_to_resize_its_column)
{
    tree_case made = {0};
    open_tree(&made, TB_LISTBOX_BROWSE, 0);
    const tb_canvas_rect rect = tb_widget_rect(made.tree);
    const float heading = rect.y + 1.0f + made.row * 0.5f;
    const float edge = rect.x + 1.0f + 200.0f;
    tb_gui_stats stats = {0};

    TB_TEST_EXPECT_FLOAT(tb_treeview_column_width(made.tree, TB_TREEVIEW_TREE_COLUMN), ==, 200.0f);
    TB_TEST_EXPECT_FLOAT(tb_treeview_column_width(made.tree, 0), ==, 80.0f);
    TB_TEST_EXPECT_FLOAT(rect.width, ==, 280.0f + 2.0f);
    gui_move(&made.fixture, edge, heading);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(made.fixture.gui), ==, TB_CURSOR_RESIZE_HORIZONTAL);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, edge - 50.0f, heading);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_FLOAT(tb_treeview_column_width(made.tree, TB_TREEVIEW_TREE_COLUMN), ==, 150.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.tree).width, ==, 230.0f + 2.0f);
    /* Never less than its least width. */
    gui_move(&made.fixture, edge - 50.0f, heading);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, rect.x - 100.0f, heading);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_FLOAT(tb_treeview_column_width(made.tree, TB_TREEVIEW_TREE_COLUMN), ==, 20.0f);
    TB_TEST_EXPECT(tb_treeview_set_column_width(made.tree, 0, 120.0f));
    TB_TEST_EXPECT_FLOAT(tb_treeview_column_width(made.tree, 0), ==, 120.0f);
    TB_TEST_EXPECT(!tb_treeview_set_column_width(made.tree, 5, 120.0f));
    TB_TEST_EXPECT_FLOAT(tb_treeview_column_width(made.tree, 5), ==, 0.0f);
    /* Away from the edges, the cursor is the arrow again. */
    gui_move(&made.fixture, rect.x + 10.0f, row_at(&made, 0).y);
    tb_gui_stats_of(made.fixture.gui, &stats);
    TB_TEST_EXPECT_UINT(tb_gui_cursor(made.fixture.gui), ==, TB_CURSOR_ARROW);

    /* Headings hidden, the rows start at its top. */
    const tb_gui_value hide = tb_gui_bool(true);
    TB_TEST_REQUIRE(tb_widget_set(made.tree, "no_headings", &hide));
    gui_update(&made.fixture);
    TB_TEST_EXPECT_UINT(tb_treeview_identify_row(made.tree, rect.y + 2.0f), ==, made.world);
    tb_gui_value value = {0};
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "heading", &value));
    TB_TEST_EXPECT(tb_text_equal(value.text, TB_TEXT("Name")));
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_treeview, a_short_tree_scrolls_and_see_opens_and_shows_an_item)
{
    tree_case made = {0};
    open_tree(&made, TB_LISTBOX_BROWSE, 2);
    tb_gui_value value = {0};

    TB_TEST_EXPECT(tb_treeview_see(made.tree, made.crate));
    TB_TEST_EXPECT(tb_treeview_is_open(made.tree, made.meshes));
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "offset", &value));
    TB_TEST_EXPECT_FLOAT(value.number, >, 0.0f);
    gui_update(&made.fixture);
    TB_TEST_EXPECT_UINT(tb_treeview_identify_row(made.tree, row_at(&made, 5).y), ==, made.crate);
    TB_TEST_EXPECT(tb_treeview_see(made.tree, made.world));
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "offset", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 0.0f);

    /* The wheel, and the bar. */
    gui_move(&made.fixture, row_at(&made, 0).x, row_at(&made, 0).y);
    gui_wheel(&made.fixture, -1.0f);
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "offset", &value));
    TB_TEST_EXPECT_FLOAT(value.number, >, 0.0f);
    gui_wheel(&made.fixture, 1.0f);
    const tb_canvas_rect rect = tb_widget_rect(made.tree);
    const float bar_x = rect.x + rect.width - 6.0f;
    gui_click(&made.fixture, (tb_vec2){bar_x, rect.y + rect.height - 3.0f});
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "offset", &value));
    TB_TEST_EXPECT_FLOAT(value.number, >, 0.0f);
    /* Paged again, to the end: its thumb is then at the bottom, and dragged up. */
    gui_click(&made.fixture, (tb_vec2){bar_x, rect.y + rect.height - 3.0f});
    gui_move(&made.fixture, bar_x, rect.y + rect.height - 3.0f);
    gui_press(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    gui_move(&made.fixture, bar_x, rect.y - 100.0f);
    gui_release(&made.fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "offset", &value));
    TB_TEST_EXPECT_FLOAT(value.number, ==, 0.0f);
    const tb_gui_value negative = tb_gui_float(-1.0f);
    TB_TEST_EXPECT(!tb_widget_set(made.tree, "offset", &negative));
    const tb_gui_value fewer = tb_gui_int(-1);
    TB_TEST_EXPECT(!tb_widget_set(made.tree, "height", &fewer));
    TB_TEST_REQUIRE(tb_widget_get(made.tree, "height", &value));
    TB_TEST_EXPECT_INT(value.integer, ==, 2);
    gui_leave(&made.fixture);
    gui_fixture_close(&made.fixture);
}
