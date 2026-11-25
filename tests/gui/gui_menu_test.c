/* Menus: a menu bar whose titles open their menus, items chosen by the mouse
 * and by the keyboard (arrows, Enter, mnemonics, Escape), cascades opened
 * and closed, check and radio items, disabled items passed over, the mouse
 * going from title to title, and context menus. */
#include "gui_fixture.h"
#include "tb_test.h"

/* The items of the File menu. */
enum
{
    OPEN,
    SAVE,
    SEPARATOR,
    RECENT,
    GRID,
    WIRE,
    LIT,
    QUIT,
    LEVEL_ONE,
    LEVEL_TWO,
    UNDO,
    REDO,
    CALLS
};

typedef struct menus
{
    gui_fixture fixture;
    tb_widget bar;
    tb_widget file;
    tb_widget recent;
    tb_widget edit;
    int calls[CALLS];
    float row;       /* an item's height, in units */
    float separator; /* a separator's */
} menus;

static void count_call(const tb_gui_event *event, void *user)
{
    int *const calls = user;

    TB_TEST_EXPECT_UINT(event->kind, ==, TB_GUI_EVENT_COMMAND);
    (*calls)++;
}

static void add(menus *made, tb_widget menu, int call, tb_menu_item_desc item)
{
    item.command = count_call;
    item.user = &made->calls[call];
    TB_TEST_REQUIRE_UINT(tb_menu_add(menu, &item), !=, SIZE_MAX);
}

static void open_menus(menus *made)
{
    gui_fixture_open(&made->fixture, 1.0f);
    const tb_widget root = tb_gui_root(made->fixture.gui);
    made->bar = tb_menubar(root);
    TB_TEST_REQUIRE(tb_grid(made->bar, &(tb_grid_options){.sticky = TB_STICKY_EW}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1}));
    made->file = tb_menu(made->bar);
    made->recent = tb_menu(made->file);
    made->edit = tb_menu(made->bar);
    add(made, made->file, OPEN, (tb_menu_item_desc){.label = "&Open", .accelerator = "Ctrl+O"});
    add(made, made->file, SAVE, (tb_menu_item_desc){.label = "&Save", .accelerator = "Ctrl+S"});
    add(made, made->file, SEPARATOR, (tb_menu_item_desc){.kind = TB_MENU_ITEM_SEPARATOR});
    add(made, made->file, RECENT,
        (tb_menu_item_desc){
            .kind = TB_MENU_ITEM_CASCADE, .label = "Re&cent", .submenu = made->recent});
    add(made, made->file, GRID, (tb_menu_item_desc){.kind = TB_MENU_ITEM_CHECK, .label = "&Grid"});
    add(made, made->file, WIRE,
        (tb_menu_item_desc){
            .kind = TB_MENU_ITEM_RADIO, .label = "&Wire && lit", .group = 1, .checked = true});
    add(made, made->file, LIT,
        (tb_menu_item_desc){.kind = TB_MENU_ITEM_RADIO, .label = "&Lit", .group = 1});
    add(made, made->file, QUIT, (tb_menu_item_desc){.label = "&Quit", .disabled = true});
    add(made, made->recent, LEVEL_ONE, (tb_menu_item_desc){.label = "Level &1"});
    add(made, made->recent, LEVEL_TWO, (tb_menu_item_desc){.label = "Level &2"});
    add(made, made->edit, UNDO, (tb_menu_item_desc){.label = "&Undo"});
    add(made, made->edit, REDO, (tb_menu_item_desc){.label = "&Redo"});
    TB_TEST_REQUIRE_UINT(tb_menubar_add(made->bar, "&File", made->file), ==, 0);
    TB_TEST_REQUIRE_UINT(tb_menubar_add(made->bar, "&Edit", made->edit), ==, 1);
    gui_update(&made->fixture);
    made->row = tb_widget_rect(made->bar).height;
    made->separator = tb_gui_theme(made->fixture.gui)->frame_padding.y * 2.0f + 1.0f;
}

/* The middle of a title of the menu bar. */
static tb_vec2 title(const menus *made, int index)
{
    const tb_gui_text_block file = {.text = TB_TEXT("File")};
    const float width = tb_gui_measure_text(made->fixture.gui, &file).x + 16.0f;
    const tb_canvas_rect bar = tb_widget_rect(made->bar);

    return (tb_vec2){bar.x + width * ((float)index + 0.5f) + (index > 0 ? 4.0f : 0.0f),
                     bar.y + bar.height * 0.5f};
}

/* The middle of an item of a menu shown, its separator at SEPARATOR. */
static tb_vec2 item(const menus *made, tb_widget menu, int index)
{
    const tb_canvas_rect rect = tb_widget_rect(menu);
    float top = rect.y + 1.0f;

    for (int at = 0; at < index; at++)
    {
        top += tb_widget_equal(menu, made->file) && at == SEPARATOR ? made->separator : made->row;
    }
    return (tb_vec2){rect.x + rect.width * 0.5f, top + made->row * 0.5f};
}

static void press_title(menus *made, int index)
{
    const tb_vec2 at = title(made, index);
    gui_move(&made->fixture, at.x, at.y);
    gui_press(&made->fixture, TB_MOUSE_BUTTON_LEFT);
    gui_release(&made->fixture, TB_MOUSE_BUTTON_LEFT);
}

TB_TEST(gui_menu, a_title_opens_its_menu_below_it_and_a_click_chooses_an_item)
{
    menus made = {0};
    open_menus(&made);

    TB_TEST_EXPECT_UINT(tb_menu_item_count(made.file), ==, 8);
    press_title(&made, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.file));
    const tb_canvas_rect bar = tb_widget_rect(made.bar);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.file).y, ==, bar.y + bar.height);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.file).x, ==, bar.x);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, SIZE_MAX);

    /* The mouse lights the item under it; a click chooses it, closes the
     * menu, and calls its command. */
    const tb_vec2 save = item(&made, made.file, SAVE);
    gui_move(&made.fixture, save.x, save.y);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, SAVE);
    gui_click(&made.fixture, save);
    TB_TEST_EXPECT_INT(made.calls[SAVE], ==, 1);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));

    /* A disabled item, and a separator, are never chosen. */
    press_title(&made, 0);
    const tb_vec2 quit = item(&made, made.file, QUIT);
    gui_move(&made.fixture, quit.x, quit.y);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, SIZE_MAX);
    gui_click(&made.fixture, quit);
    TB_TEST_EXPECT_INT(made.calls[QUIT], ==, 0);
    TB_TEST_EXPECT(tb_widget_is_popup(made.file));
    gui_click(&made.fixture, item(&made, made.file, SEPARATOR));
    TB_TEST_EXPECT(tb_widget_is_popup(made.file));
    /* A press on its title, outside the menu, closes it. */
    press_title(&made, 0);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_menu, a_cascade_opens_beside_its_item_and_its_items_are_chosen)
{
    menus made = {0};
    open_menus(&made);

    press_title(&made, 0);
    const tb_vec2 recent = item(&made, made.file, RECENT);
    gui_move(&made.fixture, recent.x, recent.y);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.recent));
    const tb_canvas_rect file = tb_widget_rect(made.file);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(made.recent).x, ==, file.x + file.width - 1.0f);
    /* Into it, the cascade's item stays lit in its menu. */
    const tb_vec2 two = item(&made, made.recent, 1);
    gui_move(&made.fixture, two.x, two.y);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, RECENT);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.recent), ==, 1);
    gui_click(&made.fixture, two);
    TB_TEST_EXPECT_INT(made.calls[LEVEL_TWO], ==, 1);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.recent));
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));

    /* Over another item, the cascade closes; Escape closes the menu in
     * front only. */
    press_title(&made, 0);
    gui_move(&made.fixture, recent.x, recent.y);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.recent));
    const tb_vec2 open = item(&made, made.file, OPEN);
    gui_move(&made.fixture, open.x, open.y);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.recent));
    gui_move(&made.fixture, recent.x, recent.y);
    gui_key(&made.fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.recent));
    TB_TEST_EXPECT(tb_widget_is_popup(made.file));
    gui_key(&made.fixture, TB_KEY_ESCAPE, 0);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_menu, the_keyboard_moves_opens_and_chooses)
{
    menus made = {0};
    open_menus(&made);

    press_title(&made, 0);
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, OPEN);
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    /* The separator is passed over. */
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, RECENT);
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.recent));
    TB_TEST_EXPECT_UINT(tb_menu_active(made.recent), ==, 0);
    gui_key(&made.fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.recent), ==, 1);
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.recent));
    TB_TEST_EXPECT(tb_widget_is_popup(made.file));
    /* Up from the first wraps around to the last that can be chosen: the
     * disabled item is passed over. */
    gui_key(&made.fixture, TB_KEY_HOME, 0);
    gui_key(&made.fixture, TB_KEY_UP, 0);
    gui_key(&made.fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, OPEN);
    gui_key(&made.fixture, TB_KEY_UP, 0);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, LIT);
    gui_key(&made.fixture, TB_KEY_DOWN, 0);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, OPEN);
    gui_key(&made.fixture, TB_KEY_ENTER, 0);
    TB_TEST_EXPECT_INT(made.calls[OPEN], ==, 1);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));

    /* A mnemonic chooses its item; a cascade's opens it. */
    press_title(&made, 0);
    gui_key(&made.fixture, TB_KEY_C, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.recent));
    gui_key(&made.fixture, TB_KEY_1, 0);
    TB_TEST_EXPECT_INT(made.calls[LEVEL_ONE], ==, 1);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));
    press_title(&made, 0);
    gui_key(&made.fixture, TB_KEY_Q, 0);
    gui_key(&made.fixture, TB_KEY_X, 0);
    TB_TEST_EXPECT_INT(made.calls[QUIT], ==, 0);
    TB_TEST_EXPECT(tb_widget_is_popup(made.file));
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_menu, check_and_radio_items_turn_as_they_are_chosen)
{
    menus made = {0};
    open_menus(&made);

    TB_TEST_EXPECT(tb_menu_item_checked(made.file, WIRE));
    press_title(&made, 0);
    gui_key(&made.fixture, TB_KEY_G, 0);
    TB_TEST_EXPECT(tb_menu_item_checked(made.file, GRID));
    TB_TEST_EXPECT_INT(made.calls[GRID], ==, 1);
    press_title(&made, 0);
    gui_key(&made.fixture, TB_KEY_G, 0);
    TB_TEST_EXPECT(!tb_menu_item_checked(made.file, GRID));
    press_title(&made, 0);
    gui_key(&made.fixture, TB_KEY_L, 0);
    TB_TEST_EXPECT(tb_menu_item_checked(made.file, LIT));
    TB_TEST_EXPECT(!tb_menu_item_checked(made.file, WIRE));

    /* By hand: set, enabled, and invoked as a click is, without a menu
     * shown. */
    TB_TEST_EXPECT(tb_menu_set_checked(made.file, WIRE, true));
    TB_TEST_EXPECT(!tb_menu_set_checked(made.file, 40, true));
    TB_TEST_EXPECT(tb_menu_set_item_enabled(made.file, QUIT, true));
    TB_TEST_EXPECT(!tb_menu_set_item_enabled(made.file, 40, true));
    TB_TEST_EXPECT(tb_menu_invoke(made.file, QUIT));
    TB_TEST_EXPECT_INT(made.calls[QUIT], ==, 1);
    TB_TEST_EXPECT(!tb_menu_invoke(made.file, SEPARATOR));
    TB_TEST_EXPECT(!tb_menu_invoke(made.file, 40));
    TB_TEST_EXPECT(!tb_menu_item_checked(made.file, 40));
    TB_TEST_EXPECT_UINT(tb_menu_add(made.bar, &(tb_menu_item_desc){0}), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_menubar_add(made.file, "x", made.edit), ==, SIZE_MAX);
    TB_TEST_EXPECT_UINT(tb_menu_item_count(made.bar), ==, 0);
    TB_TEST_EXPECT_UINT(tb_menu_active(made.bar), ==, SIZE_MAX);
    gui_fixture_close(&made.fixture);
}

TB_TEST(gui_menu, with_a_menu_open_the_mouse_and_the_arrows_go_from_title_to_title)
{
    menus made = {0};
    open_menus(&made);

    press_title(&made, 0);
    const tb_vec2 edit = title(&made, 1);
    gui_move(&made.fixture, edit.x, edit.y);
    TB_TEST_EXPECT(!tb_widget_is_popup(made.file));
    TB_TEST_REQUIRE(tb_widget_is_popup(made.edit));
    /* Opened by the mouse: no item lit. Right from a menu goes to the next
     * title, around, its first item lit; Left back. */
    TB_TEST_EXPECT_UINT(tb_menu_active(made.edit), ==, SIZE_MAX);
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.file));
    TB_TEST_EXPECT_UINT(tb_menu_active(made.file), ==, OPEN);
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.edit));
    gui_key(&made.fixture, TB_KEY_LEFT, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.file));
    /* Right from an item that is not a cascade, in a cascade, goes to the
     * menu bar too. */
    gui_key(&made.fixture, TB_KEY_C, 0);
    TB_TEST_REQUIRE(tb_widget_is_popup(made.recent));
    gui_key(&made.fixture, TB_KEY_RIGHT, 0);
    TB_TEST_EXPECT(tb_widget_is_popup(made.edit));
    TB_TEST_EXPECT(!tb_widget_is_popup(made.recent));
    gui_leave(&made.fixture);
    gui_fixture_close(&made.fixture);
}

static void open_context(const tb_gui_event *event, void *user)
{
    const tb_widget *const menu = user;

    if (event->button == TB_MOUSE_BUTTON_RIGHT)
    {
        TB_TEST_EXPECT(tb_menu_popup(*menu, event->position));
    }
}

TB_TEST(gui_menu, a_context_menu_opens_at_the_mouse_and_closes_when_pressed_away)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget area = gui_box(tb_gui_root(fixture.gui), 400.0f, 400.0f);
    tb_widget context = tb_menu(area);
    int deletes = 0;

    TB_TEST_REQUIRE(tb_grid(area, NULL));
    TB_TEST_REQUIRE_UINT(tb_menu_add(context, &(tb_menu_item_desc){.label = "&Delete",
                                                                   .command = count_call,
                                                                   .user = &deletes}),
                         ==, 0);
    TB_TEST_REQUIRE(tb_bind(area, TB_GUI_EVENT_PRESS, open_context, &context));
    gui_move(&fixture, 100.0f, 120.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_release(&fixture, TB_MOUSE_BUTTON_RIGHT);
    TB_TEST_REQUIRE(tb_widget_is_popup(context));
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(context).x, ==, 100.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(context).y, ==, 120.0f);
    /* A press away closes it, and is used by it. */
    gui_click(&fixture, (tb_vec2){300.0f, 300.0f});
    TB_TEST_EXPECT(!tb_widget_is_popup(context));
    gui_move(&fixture, 50.0f, 50.0f);
    gui_press(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_release(&fixture, TB_MOUSE_BUTTON_RIGHT);
    gui_click(&fixture, (tb_vec2){60.0f, 60.0f});
    TB_TEST_EXPECT_INT(deletes, ==, 1);
    TB_TEST_EXPECT(!tb_widget_is_popup(context));
    TB_TEST_EXPECT(!tb_menu_popup(area, (tb_vec2){0}));
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "menu"), ==, tb_menu_class());
    TB_TEST_EXPECT_POINTER(tb_gui_find_class(fixture.gui, "menubar"), ==, tb_menubar_class());
    gui_fixture_close(&fixture);
}
