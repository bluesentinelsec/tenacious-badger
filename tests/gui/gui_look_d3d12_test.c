/* What the GUI's widgets look like, drawn on D3D12 on WARP, where Windows
 * builds it: forms and containers in each of their states in the default
 * dark theme, and forms in the light theme and the sample game theme and
 * its retro look, each frame matched with its golden image
 * (tests/data/golden), so that a change to the way widgets are drawn shows
 * as a frame that differs. */
#include "badger/canvas.h"
#include "badger/font_catalog.h"
#include "badger/gui.h"
#include "badger/window.h"

#include "../render/golden.h"
#include "gui_fixture.h"
#include "tb_test.h"

#include <stddef.h>

/* The backend the case runs on: each runs on D3D12, then on D3D11. */
static tb_rhi_backend tb_test_backend = TB_RHI_BACKEND_D3D12;

/* What draws the frames: a window of the golden images' size, and a
 * renderer on WARP. */
typedef struct look_screen
{
    tb_window_system *system;
    tb_window *window;
    tb_renderer *renderer;
} look_screen;

static void open_look_screen(look_screen *opened)
{
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_rhi_device_info info = {0};

    *opened = (look_screen){0};
    if (tb_window_system_open(NULL, &opened->system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    opened->window = golden_window(opened->system, "gui");
    options.window = opened->window;
    options.device = (tb_rhi_device_options){
        .backend = tb_test_backend, .adapter = TB_RHI_ADAPTER_SOFTWARE, .validation = true};
    if (tb_renderer_create(&options, &opened->renderer, &error) != TB_RHI_OK)
    {
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no renderer: %s", error.message);
    }
    tb_rhi_device_info_of(tb_renderer_device(opened->renderer), &info);
    if ((info.features & TB_RHI_FEATURE_HEAP_INDICES) == 0)
    {
        tb_renderer_destroy(opened->renderer);
        tb_window_destroy(opened->window);
        tb_window_system_close(opened->system);
        TB_TEST_SKIP("no bindless resources on this device, which the canvas needs");
    }
}

static void close_look_screen(look_screen *opened)
{
    tb_renderer_destroy(opened->renderer);
    tb_window_destroy(opened->window);
    tb_window_system_close(opened->system);
}

/* Draw a GUI's last update and match the frame with its golden image. */
static void expect_look(look_screen *opened, gui_fixture *fixture, const char *name)
{
    const tb_camera camera = tb_camera_orthographic(10.0f);
    const uint8_t *pixels = NULL;
    uint32_t width = 0;
    uint32_t height = 0;

    tb_renderer_capture_next_frame(opened->renderer);
    TB_TEST_REQUIRE(tb_renderer_begin_frame(opened->renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_gui_draw(fixture->gui, opened->renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(opened->renderer), ==, TB_RHI_OK);
    pixels = tb_renderer_captured(opened->renderer, &width, &height);
    TB_TEST_REQUIRE(pixels != NULL);
    expect_golden(name, pixels, width, height);
    tb_gui_forget_fonts(fixture->gui, opened->renderer);
}

/* A GUI of the fixture's at a scale of 1, as large as the frame. */
static void open_look_gui(gui_fixture *fixture)
{
    gui_fixture_open(fixture, 1.0f);
    gui_resize(fixture, GOLDEN_WIDTH, GOLDEN_HEIGHT, 1.0f);
}

/* The root filled by a frame with a background, whose grid the widgets
 * are placed in. */
static tb_widget look_form(tb_widget root)
{
    const tb_widget form = tb_frame(root, &(tb_frame_desc){.padding = 4, .background = true});

    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1}));
    TB_TEST_REQUIRE(tb_grid(form, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    return form;
}

static void place(tb_widget widget, int32_t row, int32_t column)
{
    TB_TEST_REQUIRE(tb_grid(
        widget,
        &(tb_grid_options){
            .row = row, .column = column, .sticky = TB_STICKY_W, .padx = {2, 2}, .pady = {1, 1}}));
}

/* Rest the mouse on a widget, as last laid out. */
static void hover(gui_fixture *fixture, tb_widget widget)
{
    const tb_canvas_rect rect = tb_widget_rect(widget);

    gui_move(fixture, rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f);
}

/* Buttons focused, hovered as a default, and disabled; a check button
 * ticked; radio buttons, one chosen; an entry, a slider, a progress bar, a
 * combo box, and a spin box: drawn, and matched with a golden image. */
static void expect_forms(look_screen *opened, gui_fixture *fixture, const char *name)
{
    const tb_widget form = look_form(tb_gui_root(fixture->gui));
    const tb_var choice = tb_var_string(fixture->gui, "a");
    const tb_widget apply = tb_button(form, &(tb_button_desc){.text = "Apply"});
    const tb_widget ok = tb_button(form, &(tb_button_desc){.text = "OK", .is_default = true});
    const tb_widget off = tb_button(form, &(tb_button_desc){.text = "Off"});
    const tb_widget grid = tb_checkbutton(form, &(tb_checkbutton_desc){.text = "Grid"});
    const tb_widget first =
        tb_radiobutton(form, &(tb_radiobutton_desc){.text = "A", .variable = choice, .value = "a"});
    const tb_widget second =
        tb_radiobutton(form, &(tb_radiobutton_desc){.text = "B", .variable = choice, .value = "b"});
    const tb_widget slider = tb_scale(form, &(tb_scale_desc){.value = 40, .length = 100});
    const tb_widget progress =
        tb_progressbar(form, &(tb_progressbar_desc){.value = 60, .length = 100});
    static const char *const levels[] = {"Low", "High"};
    const tb_widget combo = tb_combobox(
        form, &(tb_combobox_desc){.values = levels, .value_count = 2, .current = 1, .width = 6});
    const tb_widget spin = tb_spinbox(form, &(tb_spinbox_desc){.value = 5, .width = 4});
    const tb_widget entry = tb_entry(form, &(tb_entry_desc){.text = "Name", .width = 8});

    place(apply, 0, 0);
    place(ok, 1, 0);
    place(off, 2, 0);
    place(grid, 3, 0);
    place(first, 4, 0);
    place(second, 5, 0);
    place(slider, 0, 1);
    place(progress, 1, 1);
    place(combo, 2, 1);
    place(spin, 3, 1);
    place(entry, 4, 1);
    TB_TEST_REQUIRE(
        tb_widget_set(grid, "selected", &(tb_gui_value){TB_GUI_TYPE_BOOL, {.boolean = true}}));
    tb_widget_set_enabled(off, false);
    gui_update(fixture);
    /* Focused as the keyboard focuses it, so that its ring is shown. */
    tb_gui_set_focus_visible(fixture->gui, true);
    TB_TEST_REQUIRE(tb_widget_focus(apply));
    hover(fixture, ok);
    expect_look(opened, fixture, name);
}

static void forms_match_their_golden_image_case(void)
{
    look_screen opened = {0};
    gui_fixture fixture = {0};

    open_look_screen(&opened);
    open_look_gui(&fixture);
    expect_forms(&opened, &fixture, "gui_forms");
    gui_fixture_close(&fixture);
    close_look_screen(&opened);
}

TB_TEST(gui_look, forms_match_their_golden_image_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    forms_match_their_golden_image_case();
}

TB_TEST(gui_look, forms_match_their_golden_image_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    forms_match_their_golden_image_case();
}

/* The light theme, and the sample game theme in both its looks, in the
 * fonts of their roles: the forms, matched with their golden images. */
static void themes_match_their_golden_images_case(void)
{
    static const char *const names[] = {"gui_light", "gui_game", "gui_retro"};
    /* The game's looks are drawn smaller, as an options menu's scale would,
     * so that the form fits the golden image: its panels at a scale that
     * is no whole number; pixel art at its own size, two pixels a texel. */
    static const float scales[] = {1.0f, 0.6f, 1.0f};
    look_screen opened = {0};
    tb_font_catalog *const catalog = tb_font_catalog_create(NULL);
    size_t found = 0;

    if (!tb_font_catalog_find_role(catalog, TB_FONT_ROLE_HEADING, &found) ||
        !tb_font_catalog_find_role(catalog, TB_FONT_ROLE_BODY, &found) ||
        !tb_font_catalog_find_role(catalog, TB_FONT_ROLE_PIXEL, &found))
    {
        tb_font_catalog_destroy(catalog);
        TB_TEST_SKIP("this build carries no fonts of the game theme's roles");
    }
    open_look_screen(&opened);
    tb_theme *const themes[] = {tb_theme_create_light(),
                                tb_theme_create_game(opened.renderer, TB_THEME_GAME_FANTASY),
                                tb_theme_create_game(opened.renderer, TB_THEME_GAME_RETRO)};
    for (size_t index = 0; index < sizeof(themes) / sizeof(themes[0]); index++)
    {
        gui_fixture fixture = {0};
        open_look_gui(&fixture);
        /* The fixture's GUI, with the catalog the roles' fonts come from. */
        const tb_ui_theme palette = *tb_gui_theme(fixture.gui);
        tb_gui_destroy(fixture.gui);
        fixture.gui = tb_gui_create(&(tb_gui_options){.theme = &palette,
                                                      .catalog = catalog,
                                                      .style_theme = themes[index],
                                                      .scale = scales[index]});
        gui_resize(&fixture, GOLDEN_WIDTH, GOLDEN_HEIGHT, 1.0f);
        TB_TEST_CONTEXT("%s", names[index]);
        expect_forms(&opened, &fixture, names[index]);
        gui_fixture_close(&fixture);
    }
    for (size_t index = 0; index < sizeof(themes) / sizeof(themes[0]); index++)
    {
        tb_theme_destroy(themes[index]);
    }
    close_look_screen(&opened);
    tb_font_catalog_destroy(catalog);
}

TB_TEST(gui_look, themes_match_their_golden_images_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    themes_match_their_golden_images_case();
}

TB_TEST(gui_look, themes_match_their_golden_images_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    themes_match_their_golden_images_case();
}

/* A menu bar; a notebook of two tabs, its page a tree with an item open
 * and one chosen; and a list with an item chosen, scrolled by its bar. */
static void containers_match_their_golden_image_case(void)
{
    look_screen opened = {0};
    gui_fixture fixture = {0};

    open_look_screen(&opened);
    open_look_gui(&fixture);
    const tb_widget form = look_form(tb_gui_root(fixture.gui));
    const tb_widget bar = tb_menubar(form);
    const tb_widget file = tb_menu(bar);
    const tb_widget book = tb_notebook(form, &(tb_notebook_desc){.padding = 2});
    const tb_widget tree =
        tb_treeview(book, &(tb_treeview_desc){.height = 4, .heading = "Name", .tree_width = 110});
    const tb_widget other = tb_frame(book, NULL);
    static const char *const names[] = {"Sun", "Moon", "Stars", "Clouds", "Rain"};
    const tb_widget list = tb_listbox(
        form, &(tb_listbox_desc){.width = 70, .height = 3, .items = names, .item_count = 5});

    TB_TEST_REQUIRE(tb_menu_add(file, &(tb_menu_item_desc){.label = "&Open"}) != SIZE_MAX);
    TB_TEST_REQUIRE(tb_menubar_add(bar, "&File", file) != SIZE_MAX);
    TB_TEST_REQUIRE(tb_menubar_add(bar, "&Edit", tb_menu(bar)) != SIZE_MAX);
    TB_TEST_REQUIRE(
        tb_grid(bar, &(tb_grid_options){.row = 0, .column_span = 2, .sticky = TB_STICKY_EW}));
    const tb_tree_item lights = tb_treeview_insert(
        tree, TB_TREE_ROOT, SIZE_MAX, &(tb_tree_item_desc){.text = "Lights", .open = true});
    const tb_tree_item sun =
        tb_treeview_insert(tree, lights, SIZE_MAX, &(tb_tree_item_desc){.text = "Sun"});
    (void)tb_treeview_insert(tree, lights, SIZE_MAX, &(tb_tree_item_desc){.text = "Lamp"});
    (void)tb_treeview_insert(tree, TB_TREE_ROOT, SIZE_MAX, &(tb_tree_item_desc){.text = "Props"});
    TB_TEST_REQUIRE(tb_treeview_select(tree, sun, true));
    TB_TEST_REQUIRE(tb_notebook_add(book, tree, "Scene") != SIZE_MAX);
    TB_TEST_REQUIRE(tb_notebook_add(book, other, "Assets") != SIZE_MAX);
    place(book, 1, 0);
    place(list, 1, 1);
    TB_TEST_REQUIRE(tb_listbox_select(list, 1, true));
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_widget_focus(list));
    gui_update(&fixture);
    expect_look(&opened, &fixture, "gui_containers");
    gui_fixture_close(&fixture);
    close_look_screen(&opened);
}

TB_TEST(gui_look, containers_match_their_golden_image_on_d3d12)
{
    tb_test_backend = TB_RHI_BACKEND_D3D12;
    containers_match_their_golden_image_case();
}

TB_TEST(gui_look, containers_match_their_golden_image_on_d3d11)
{
    tb_test_backend = TB_RHI_BACKEND_D3D11;
    containers_match_their_golden_image_case();
}
