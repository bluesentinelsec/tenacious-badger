/* The gui_tool scene: the retained GUI in the shape of an editor. A menu
 * bar with cascades, check and radio items, and accelerators' text over a
 * paned window: on the left an outliner, a tree view of the level's items
 * with a column of their types; on the right a notebook of an asset list
 * in a scroll frame, with a context menu, a page of details with tooltips,
 * and a viewport drawn by a canvas callback beside an image. Message
 * dialogs confirm and inform; a status line says what was done. The window
 * events go to the GUI first, which covers the window. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/gui.h"
#include "badger/memory.h"

#include <stddef.h>

/* Units around the tool, and below it, clear of the sandbox's frame time. */
#define TOOL_MARGIN 8.0f
#define TOOL_BOTTOM_MARGIN 40.0f
/* Assets in the list, and the bytes of the status line and an asset's name. */
#define TOOL_ASSETS 400
#define TOOL_STATUS_CAPACITY 160
#define TOOL_NAME_CAPACITY 48
/* Units between the viewport's grid lines, and the checker image's pixels. */
#define TOOL_GRID_STEP 32.0f
#define TOOL_IMAGE_SIZE 64u
#define TOOL_IMAGE_CELL 8u

typedef struct tool_scene
{
    tb_gui *gui;
    tb_ui_fonts fonts;
    tb_image *checker;
    tb_widget status;
    tb_widget view;
    tb_widget outliner;
    tb_widget assets;
    tb_widget context;
    bool grid_shown;
    bool quit_asked;
    char text[TOOL_STATUS_CAPACITY];
} tool_scene;

static void tool_status(tool_scene *scene, const char *text)
{
    (void)tb_widget_set_text(scene->status, text);
}

/* ---------------------------------------------------------------------------
 * Menus.
 * ------------------------------------------------------------------------- */

static void tool_on_command(const tb_gui_event *event, void *user)
{
    (void)event;
    tool_status(user, "Done.");
}

static void tool_on_grid(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;

    scene->grid_shown = tb_menu_item_checked(event->widget, 0);
    tool_status(scene, scene->grid_shown ? "The grid is shown." : "The grid is hidden.");
}

static void tool_on_quit_answer(tb_message_result result, void *user)
{
    tool_scene *const scene = user;

    scene->quit_asked = result == TB_MESSAGE_RESULT_YES;
    tool_status(scene, scene->quit_asked ? "The sandbox stays open; press Escape to leave."
                                         : "Not quitting.");
}

static void tool_on_quit(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;

    (void)event;
    (void)tb_gui_message_box(scene->gui, "Quit?", "Unsaved changes to the level will be lost.",
                             TB_MESSAGE_YES_NO, tool_on_quit_answer, scene);
}

static void tool_on_about(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;

    (void)event;
    (void)tb_gui_message_box(scene->gui, "About",
                             "A tool built from the retained GUI's containers and overlays: "
                             "menus, a paned window, a tree view, a notebook, a list in a scroll "
                             "frame, tooltips, and message dialogs.",
                             TB_MESSAGE_OK, NULL, NULL);
}

static void tool_add(tb_widget menu, tb_menu_item_desc item, tb_gui_callback *command,
                     tool_scene *scene)
{
    item.command = command;
    item.user = scene;
    (void)tb_menu_add(menu, &item);
}

static void tool_build_menus(tool_scene *scene, tb_widget parent)
{
    const tb_widget bar = tb_menubar(parent);
    const tb_widget file = tb_menu(bar);
    const tb_widget recent = tb_menu(file);
    const tb_widget view = tb_menu(bar);
    const tb_widget help = tb_menu(bar);

    (void)tb_grid(bar, &(tb_grid_options){.sticky = TB_STICKY_EW});
    tool_add(file, (tb_menu_item_desc){.label = "&New level", .accelerator = "Ctrl+N"},
             tool_on_command, scene);
    tool_add(file, (tb_menu_item_desc){.label = "&Open...", .accelerator = "Ctrl+O"},
             tool_on_command, scene);
    tool_add(file, (tb_menu_item_desc){.label = "&Save", .accelerator = "Ctrl+S"}, tool_on_command,
             scene);
    tool_add(file, (tb_menu_item_desc){.label = "Save &as...", .disabled = true}, tool_on_command,
             scene);
    tool_add(file, (tb_menu_item_desc){.kind = TB_MENU_ITEM_SEPARATOR}, NULL, scene);
    tool_add(file,
             (tb_menu_item_desc){
                 .kind = TB_MENU_ITEM_CASCADE, .label = "Open &recent", .submenu = recent},
             NULL, scene);
    tool_add(file, (tb_menu_item_desc){.kind = TB_MENU_ITEM_SEPARATOR}, NULL, scene);
    tool_add(file, (tb_menu_item_desc){.label = "&Quit", .accelerator = "Alt+F4"}, tool_on_quit,
             scene);
    tool_add(recent, (tb_menu_item_desc){.label = "&1 burrow.level"}, tool_on_command, scene);
    tool_add(recent, (tb_menu_item_desc){.label = "&2 warren.level"}, tool_on_command, scene);
    tool_add(
        view,
        (tb_menu_item_desc){
            .kind = TB_MENU_ITEM_CHECK, .label = "Show &grid", .checked = true, .accelerator = "G"},
        tool_on_grid, scene);
    tool_add(view, (tb_menu_item_desc){.kind = TB_MENU_ITEM_SEPARATOR}, NULL, scene);
    tool_add(view,
             (tb_menu_item_desc){
                 .kind = TB_MENU_ITEM_RADIO, .label = "&Lit", .group = 1, .checked = true},
             tool_on_command, scene);
    tool_add(view,
             (tb_menu_item_desc){.kind = TB_MENU_ITEM_RADIO, .label = "&Wireframe", .group = 1},
             tool_on_command, scene);
    tool_add(help, (tb_menu_item_desc){.label = "&About"}, tool_on_about, scene);
    (void)tb_menubar_add(bar, "&File", file);
    (void)tb_menubar_add(bar, "&View", view);
    (void)tb_menubar_add(bar, "&Help", help);
}

/* ---------------------------------------------------------------------------
 * The outliner.
 * ------------------------------------------------------------------------- */

static void tool_on_outline(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;
    const tb_tree_item item = tb_treeview_focus_item(event->widget);
    const tb_text name = tb_treeview_text(event->widget, item);

    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "Selected %.*s.", (int)name.length,
                        name.bytes);
    tool_status(scene, scene->text);
}

static tb_tree_item tool_item(tb_widget tree, tb_tree_item parent, const char *text,
                              const char *type, bool open)
{
    const char *const values[] = {type};
    return tb_treeview_insert(
        tree, parent, SIZE_MAX,
        &(tb_tree_item_desc){.text = text, .values = values, .value_count = 1, .open = open});
}

static tb_widget tool_build_outliner(tool_scene *scene, tb_widget parent)
{
    const tb_widget tree =
        tb_treeview(parent, &(tb_treeview_desc){.selectmode = TB_LISTBOX_EXTENDED,
                                                .heading = "Name",
                                                .tree_width = 170.0f,
                                                .command = tool_on_outline,
                                                .user = scene});
    char name[TOOL_NAME_CAPACITY] = {0};

    (void)tb_treeview_add_column(tree, &(tb_treeview_column_desc){.heading = "Type", .width = 110});
    const tb_tree_item world = tool_item(tree, TB_TREE_ROOT, "Burrow", "Level", true);
    const tb_tree_item lights = tool_item(tree, world, "Lights", "Group", true);
    (void)tool_item(tree, lights, "Sun", "Directional", false);
    (void)tool_item(tree, lights, "Lantern", "Point", false);
    const tb_tree_item meshes = tool_item(tree, world, "Meshes", "Group", true);
    (void)tool_item(tree, meshes, "Floor", "Static mesh", false);
    for (int index = 1; index <= 12; index++)
    {
        (void)tb_fmt_buffer(name, sizeof(name), "Crate %02d", index);
        (void)tool_item(tree, meshes, name, "Static mesh", false);
    }
    const tb_tree_item cameras = tool_item(tree, world, "Cameras", "Group", false);
    (void)tool_item(tree, cameras, "Player", "Camera", false);
    scene->outliner = tree;
    return tree;
}

/* ---------------------------------------------------------------------------
 * The pages.
 * ------------------------------------------------------------------------- */

static void tool_on_asset(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;
    const size_t chosen = tb_listbox_selection(event->widget, NULL, 0);

    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "%zu %s chosen.", chosen,
                        chosen == 1 ? "asset" : "assets");
    tool_status(scene, scene->text);
}

static void tool_on_asset_press(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;

    if (event->button == TB_MOUSE_BUTTON_RIGHT)
    {
        (void)tb_menu_popup(scene->context, event->position);
    }
}

static void tool_on_delete_answer(tb_message_result result, void *user)
{
    tool_scene *const scene = user;
    size_t index = 0;

    if (result != TB_MESSAGE_RESULT_OK)
    {
        tool_status(scene, "Nothing was deleted.");
        return;
    }
    while (tb_listbox_selection(scene->assets, &index, 1) > 0)
    {
        (void)tb_listbox_delete(scene->assets, index, 1);
    }
    tool_status(scene, "Deleted.");
}

static void tool_on_delete(const tb_gui_event *event, void *user)
{
    tool_scene *const scene = user;

    (void)event;
    (void)tb_gui_message_box(scene->gui, "Delete assets?",
                             "The assets chosen are deleted from the project.",
                             TB_MESSAGE_OK_CANCEL, tool_on_delete_answer, scene);
}

static tb_widget tool_build_assets(tool_scene *scene, tb_widget book)
{
    const tb_widget page = tb_frame(book, &(tb_frame_desc){.padding = TOOL_MARGIN});
    const tb_widget frame = tb_scrollframe(
        page, &(tb_scrollframe_desc){.no_xscroll = true, .height = 120.0f, .border = true});
    char name[TOOL_NAME_CAPACITY] = {0};

    scene->assets = tb_listbox(frame, &(tb_listbox_desc){.selectmode = TB_LISTBOX_EXTENDED,
                                                         .command = tool_on_asset,
                                                         .user = scene});
    for (int index = 0; index < TOOL_ASSETS; index++)
    {
        (void)tb_fmt_buffer(name, sizeof(name), "/Game/Props/crate_%03d.mesh", index);
        (void)tb_listbox_insert(scene->assets, SIZE_MAX, name);
    }
    (void)tb_grid(scene->assets, &(tb_grid_options){.sticky = TB_STICKY_NSEW});
    (void)tb_grid_column_configure(frame, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid(frame, &(tb_grid_options){.sticky = TB_STICKY_NSEW});
    (void)tb_grid_row_configure(page, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_column_configure(page, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_tooltip(scene->assets,
                     &(tb_tooltip_desc){.text = "Click, Shift+click, and Control+click choose; "
                                                "the right button opens a menu."});

    scene->context = tb_menu(scene->assets);
    tool_add(scene->context, (tb_menu_item_desc){.label = "&Rename", .accelerator = "F2"},
             tool_on_command, scene);
    tool_add(scene->context, (tb_menu_item_desc){.label = "D&uplicate", .accelerator = "Ctrl+D"},
             tool_on_command, scene);
    tool_add(scene->context, (tb_menu_item_desc){.kind = TB_MENU_ITEM_SEPARATOR}, NULL, scene);
    tool_add(scene->context, (tb_menu_item_desc){.label = "&Delete", .accelerator = "Del"},
             tool_on_delete, scene);
    (void)tb_bind(scene->assets, TB_GUI_EVENT_PRESS, tool_on_asset_press, scene);
    return page;
}

static tb_widget tool_build_details(tool_scene *scene, tb_widget book)
{
    static const char *const names[] = {"Name", "Position", "Rotation", "Scale"};
    static const char *const values[] = {"Crate 01", "4, 0, -2", "0, 45, 0", "1, 1, 1"};
    static const char *const tips[] = {"The item's name in the outliner.",
                                       "Units from the level's origin.", "Degrees about each axis.",
                                       "Times its mesh's size."};
    const tb_widget page = tb_frame(book, &(tb_frame_desc){.padding = TOOL_MARGIN * 2.0f});

    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    {
        const tb_widget label = tb_label(page, &(tb_label_desc){.text = names[index]});
        const tb_widget value = tb_label(page, &(tb_label_desc){.text = values[index]});
        (void)tb_grid(label, &(tb_grid_options){.row = (int32_t)index,
                                                .sticky = TB_STICKY_E,
                                                .padx = {0.0f, TOOL_MARGIN},
                                                .pady = {4.0f, 4.0f}});
        (void)tb_grid(
            value, &(tb_grid_options){.row = (int32_t)index, .column = 1, .sticky = TB_STICKY_W});
        (void)tb_tooltip(label, &(tb_tooltip_desc){.text = tips[index]});
    }
    const tb_widget reset = tb_button(
        page,
        &(tb_button_desc){.text = "Reset transform", .command = tool_on_command, .user = scene});
    (void)tb_grid(reset,
                  &(tb_grid_options){
                      .row = 4, .column = 1, .sticky = TB_STICKY_W, .pady = {TOOL_MARGIN, 0.0f}});
    (void)tb_tooltip(reset, &(tb_tooltip_desc){.text = "Put it back where it was made."});
    return page;
}

/* The viewport: a grid of lines, and the outliner's choice as a square. */
static void tool_draw_viewport(tb_widget widget, tb_gui_paint *paint, void *user)
{
    const tool_scene *const scene = user;
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_vec4 line = {0.30f, 0.32f, 0.36f, 1.0f};
    const tb_vec4 chosen = tb_gui_theme(widget.gui)->colors[TB_UI_COLOR_ACCENT];

    if (scene->grid_shown)
    {
        for (float x = rect.x; x < rect.x + rect.width; x += TOOL_GRID_STEP)
        {
            tb_gui_paint_line(paint, (tb_vec2){x, rect.y}, (tb_vec2){x, rect.y + rect.height}, 1.0f,
                              line);
        }
        for (float y = rect.y; y < rect.y + rect.height; y += TOOL_GRID_STEP)
        {
            tb_gui_paint_line(paint, (tb_vec2){rect.x, y}, (tb_vec2){rect.x + rect.width, y}, 1.0f,
                              line);
        }
    }
    const tb_canvas_rect crate = {rect.x + rect.width * 0.5f - 24.0f,
                                  rect.y + rect.height * 0.5f - 24.0f, 48.0f, 48.0f};
    tb_gui_paint_fill(paint, crate, 4.0f, chosen);
}

static tb_widget tool_build_viewport(tool_scene *scene, tb_widget book)
{
    const tb_widget page = tb_frame(book, &(tb_frame_desc){.padding = TOOL_MARGIN});
    const tb_widget canvas =
        tb_canvas_widget(page, &(tb_canvas_widget_desc){.width = 240.0f,
                                                        .height = 160.0f,
                                                        .background = true,
                                                        .draw = tool_draw_viewport,
                                                        .user = scene});
    const tb_widget image = tb_image_widget(
        page,
        &(tb_image_widget_desc){
            .image = scene->checker, .fit = TB_IMAGE_FIT_CONTAIN, .width = 96.0f, .height = 96.0f});

    (void)tb_grid(canvas, &(tb_grid_options){.sticky = TB_STICKY_NSEW});
    (void)tb_grid(
        image, &(tb_grid_options){.column = 1, .sticky = TB_STICKY_N, .padx = {TOOL_MARGIN, 0.0f}});
    (void)tb_grid_column_configure(page, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(page, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_tooltip(image, &(tb_tooltip_desc){.text = "The level's thumbnail."});
    return page;
}

/* ---------------------------------------------------------------------------
 * The scene.
 * ------------------------------------------------------------------------- */

static void tool_build(tool_scene *scene, tb_widget root)
{
    const tb_widget window = tb_frame(root, &(tb_frame_desc){.background = true});
    (void)tb_grid(window,
                  &(tb_grid_options){.sticky = TB_STICKY_NSEW, .pady = {0.0f, TOOL_BOTTOM_MARGIN}});
    (void)tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    tool_build_menus(scene, window);

    const tb_widget paned =
        tb_panedwindow(window, &(tb_panedwindow_desc){.orient = TB_GUI_ORIENT_HORIZONTAL});
    (void)tb_grid(paned, &(tb_grid_options){.row = 1,
                                            .sticky = TB_STICKY_NSEW,
                                            .padx = {TOOL_MARGIN, TOOL_MARGIN},
                                            .pady = {TOOL_MARGIN, TOOL_MARGIN}});
    (void)tb_grid_column_configure(window, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(window, 1, &(tb_grid_track){.weight = 1.0f});
    const tb_widget outliner = tool_build_outliner(scene, paned);
    (void)tb_panedwindow_add(paned, outliner, &(tb_pane_options){.weight = 1.0f, .minsize = 120});

    const tb_widget book = tb_notebook(paned, &(tb_notebook_desc){.padding = 2.0f});
    (void)tb_notebook_add(book, tool_build_assets(scene, book), "Assets");
    (void)tb_notebook_add(book, tool_build_details(scene, book), "Details");
    (void)tb_notebook_add(book, tool_build_viewport(scene, book), "Viewport");
    (void)tb_panedwindow_add(paned, book, &(tb_pane_options){.weight = 2.0f, .minsize = 200});

    scene->status = tb_label(window, &(tb_label_desc){.text = "Ready. Right-click an asset; "
                                                              "Control+Tab turns the pages."});
    (void)tb_grid(scene->status, &(tb_grid_options){.row = 2,
                                                    .sticky = TB_STICKY_W,
                                                    .padx = {TOOL_MARGIN, 0.0f},
                                                    .pady = {0.0f, TOOL_MARGIN}});
}

static void tool_make_checker(sandbox *box, tool_scene *scene)
{
    const size_t size = (size_t)TOOL_IMAGE_SIZE * TOOL_IMAGE_SIZE * 4u;
    uint8_t *const pixels = tb_calloc(size, 1);

    tb_image_fill_checkerboard(pixels, size, TOOL_IMAGE_SIZE, TOOL_IMAGE_SIZE, TOOL_IMAGE_CELL,
                               (tb_vec4){0.85f, 0.55f, 0.20f, 1.0f},
                               (tb_vec4){0.30f, 0.20f, 0.10f, 1.0f});
    scene->checker = tb_image_create(box->renderer,
                                     &(tb_image_desc){.width = TOOL_IMAGE_SIZE,
                                                      .height = TOOL_IMAGE_SIZE,
                                                      .filter = TB_IMAGE_NEAREST,
                                                      .name = "gui_tool checker"},
                                     pixels, size);
    tb_free(pixels);
}

bool gui_tool_scene_create(sandbox *box, void **state)
{
    tool_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_ui_theme theme = tb_ui_theme_dark();

    *state = scene;
    scene->grid_shown = true;
    if (!tb_ui_fonts_open(box->catalog, NULL, &scene->fonts))
    {
        (void)tb_fmt_print("badger-sandbox: gui_tool: this build of the engine carries no UI "
                           "font; the GUI is not drawn\n");
        return true;
    }
    tool_make_checker(box, scene);
    tb_ui_theme_set_fonts(&theme, &scene->fonts);
    scene->gui = tb_gui_create(
        &(tb_gui_options){.theme = &theme, .catalog = box->catalog, .scale = box->ui_scale});
    tool_build(scene, tb_gui_root(scene->gui));
    return true;
}

void gui_tool_scene_destroy(sandbox *box, void *state)
{
    tool_scene *const scene = state;

    if (scene == NULL)
    {
        return;
    }
    tb_window_set_cursor(box->window, TB_CURSOR_ARROW);
    if (scene->gui != NULL)
    {
        tb_gui_forget_fonts(scene->gui, box->renderer);
    }
    tb_gui_destroy(scene->gui);
    for (size_t font = 0; font < TB_UI_FONT_COUNT; font++)
    {
        if (scene->fonts.fonts[font] != NULL)
        {
            tb_canvas_forget_font(box->renderer, scene->fonts.fonts[font]);
        }
    }
    tb_ui_fonts_close(&scene->fonts);
    tb_image_destroy(box->renderer, scene->checker);
    tb_free(scene);
}

bool gui_tool_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    tool_scene *const scene = state;
    const tb_window_event_type type = event->type;

    (void)box;
    if (scene->gui == NULL)
    {
        return false;
    }
    tb_gui_feed_event(scene->gui, event);
    if (type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN || type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        return tb_gui_wants_mouse(scene->gui);
    }
    if (type == TB_WINDOW_EVENT_KEY_DOWN)
    {
        /* Escape leaves the sandbox unless a menu or a dialog takes it. */
        return event->key != TB_KEY_ESCAPE || tb_gui_wants_keyboard(scene->gui);
    }
    return false;
}

void gui_tool_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    tool_scene *const scene = state;
    uint32_t width = 0;
    uint32_t height = 0;

    if (scene->gui == NULL)
    {
        return;
    }
    tb_renderer_size(box->renderer, &width, &height);
    tb_gui_resize(scene->gui, width, height, tb_canvas_view_logical(box->renderer).scale);
    tb_gui_update(scene->gui, frame->delta_seconds);
    tb_gui_draw(scene->gui, box->renderer);
    tb_window_set_cursor(box->window, tb_gui_cursor(scene->gui));
}
