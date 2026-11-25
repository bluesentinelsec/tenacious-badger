/* The gui scene: a form of the retained GUI over the shapes, laid out by
 * grid. The form takes the width it wants and half of the rest, and follows
 * the window as it is resized: its middle column takes the extra width, an
 * empty row the extra height, and the status line stays at the bottom.
 * Nested grids hold rows of buttons of one width, and a modal dialog asks
 * to confirm. On the right, a panel of options edits the engine's own
 * settings through variables (sandbox_gui_options.c), and below it a panel
 * is built from a layout file in the sandbox's resources, and built again
 * whenever the file is saved. A row of radio buttons switches the GUI's
 * theme as it runs: the dark and light tool themes, and the sample game
 * theme and its retro look. The window events go to the GUI first, which
 * takes the mouse over the form and the keys while a button has the focus;
 * the fly camera has the rest. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/gui.h"
#include "badger/memory.h"
#include "badger/vfs.h"

#include <stddef.h>

/* Units between the form and the window's edges, and inside it. */
#define GUI_MARGIN 16.0f
#define GUI_PADDING 16.0f
#define GUI_BOTTOM_MARGIN 48.0f
/* Units between a field's label and its field, and between rows. */
#define GUI_GAP 12.0f
#define GUI_ROW_GAP 6.0f
/* Units a paragraph wraps at, and the size of the title. */
#define GUI_WRAP 360.0f
#define GUI_TITLE_SIZE 26.0f
/* Bytes of the status line. */
#define GUI_STATUS_CAPACITY 128
/* The panel's layout file, in the sandbox's resources (SANDBOX_RESOURCES). */
#define GUI_PANEL_PATH "gui/panel.json"

static const char *const gui_names[] = {"Badger", "Honey badger", "Ferret-badger", "Hog badger"};
static const char *const gui_difficulties[] = {"Easy", "Normal", "Hard"};

/* The themes the form switches between; the first is the GUI's default. */
typedef enum gui_theme_choice
{
    GUI_THEME_DARK,
    GUI_THEME_LIGHT,
    GUI_THEME_GAME,
    GUI_THEME_RETRO,
    GUI_THEME_COUNT
} gui_theme_choice;

static const char *const gui_theme_names[GUI_THEME_COUNT] = {"Dark", "Light", "Game", "Retro"};
static const char *const gui_theme_values[GUI_THEME_COUNT] = {"0", "1", "2", "3"};

typedef struct gui_scene
{
    void *shapes;
    tb_gui *gui;
    tb_ui_fonts fonts;
    tb_widget name;
    tb_widget volume;
    tb_widget status;
    tb_widget dialog;
    tb_vfs *resources; /* where the panel's file is read from; outlives the GUI */
    gui_options *options;
    tb_theme *themes[GUI_THEME_COUNT]; /* NULL for the default */
    tb_var theme;
    size_t name_index;
    int volume_percent;
    int applies;
    char text[GUI_STATUS_CAPACITY];
} gui_scene;

static void gui_set_status(gui_scene *scene, const char *text)
{
    (void)tb_widget_set_text(scene->status, text);
}

/* ---------------------------------------------------------------------------
 * Callbacks.
 * ------------------------------------------------------------------------- */

static void gui_on_rename(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;

    (void)event;
    scene->name_index = (scene->name_index + 1) % (sizeof(gui_names) / sizeof(gui_names[0]));
    (void)tb_widget_set_text(scene->name, gui_names[scene->name_index]);
    gui_set_status(scene, "Renamed.");
}

static void gui_on_difficulty(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;
    const tb_text chosen = tb_widget_text(event->widget);

    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "Difficulty: %.*s.", (int)chosen.length,
                        chosen.bytes);
    gui_set_status(scene, scene->text);
}

static void gui_show_volume(gui_scene *scene)
{
    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "%d%%", scene->volume_percent);
    (void)tb_widget_set_text(scene->volume, scene->text);
}

static void gui_on_quieter(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;

    (void)event;
    scene->volume_percent = scene->volume_percent >= 10 ? scene->volume_percent - 10 : 0;
    gui_show_volume(scene);
}

static void gui_on_louder(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;

    (void)event;
    scene->volume_percent = scene->volume_percent <= 90 ? scene->volume_percent + 10 : 100;
    gui_show_volume(scene);
}

static void gui_on_apply(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;

    (void)event;
    scene->applies++;
    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "Applied %d %s.", scene->applies,
                        scene->applies == 1 ? "time" : "times");
    gui_set_status(scene, scene->text);
}

/* Draw the GUI by the theme chosen, from the next update on. */
static void gui_on_theme(tb_var var, void *user)
{
    gui_scene *const scene = user;
    const int32_t chosen = tb_var_get_int(var);

    if (chosen < 0 || chosen >= GUI_THEME_COUNT)
    {
        return;
    }
    tb_gui_set_style_theme(scene->gui, scene->themes[chosen]);
    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "Theme: %s.", gui_theme_names[chosen]);
    gui_set_status(scene, scene->text);
}

static void gui_on_show_dialog(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;

    (void)event;
    tb_widget_set_visible(scene->dialog, true);
    (void)tb_widget_show_modal(scene->dialog);
}

static void gui_close_dialog(gui_scene *scene, const char *status)
{
    tb_widget_end_modal(scene->dialog);
    tb_widget_set_visible(scene->dialog, false);
    gui_set_status(scene, status);
}

static void gui_on_reset(const tb_gui_event *event, void *user)
{
    gui_scene *const scene = user;

    (void)event;
    scene->name_index = 0;
    scene->volume_percent = 70;
    scene->applies = 0;
    (void)tb_widget_set_text(scene->name, gui_names[0]);
    gui_show_volume(scene);
    gui_close_dialog(scene, "Reset to the defaults.");
}

static void gui_on_cancel(const tb_gui_event *event, void *user)
{
    (void)event;
    gui_close_dialog(user, "Nothing was reset.");
}

static void gui_on_dialog_key(const tb_gui_event *event, void *user)
{
    if (event->key == TB_KEY_ESCAPE)
    {
        gui_close_dialog(user, "Nothing was reset.");
    }
}

static void gui_on_hello(const tb_gui_event *event, void *user)
{
    (void)event;
    gui_set_status(user, "Hello from the panel's file.");
}

/* The panel's file was saved: built again, or kept as it was. */
static void gui_on_panel_reload(tb_widget panel, const tb_gui_file_error *error, void *user)
{
    gui_scene *const scene = user;

    (void)panel;
    if (error == NULL)
    {
        (void)tb_fmt_print("badger-sandbox: gui: %s loaded again\n", GUI_PANEL_PATH);
        gui_set_status(scene, "The panel was loaded again.");
        return;
    }
    (void)tb_fmt_print("badger-sandbox: gui: %s\n", error->message);
    (void)tb_fmt_buffer(scene->text, sizeof(scene->text), "The panel's file: %s.", error->reason);
    gui_set_status(scene, scene->text);
}

/* ---------------------------------------------------------------------------
 * The form.
 * ------------------------------------------------------------------------- */

static tb_widget gui_button(tb_widget parent, const char *text, tb_gui_callback *command,
                            void *user, bool is_default)
{
    const tb_widget button = tb_button(
        parent, &(tb_button_desc){
                    .text = text, .command = command, .user = user, .is_default = is_default});
    tb_widget_set_cursor(button, TB_CURSOR_HAND);
    return button;
}

/* A field's label, on the right of the first column. */
static void gui_field_label(tb_widget form, int32_t row, const char *text)
{
    const tb_widget label = tb_label(form, &(tb_label_desc){.text = text});

    (void)tb_grid(label, &(tb_grid_options){.row = row,
                                            .sticky = TB_STICKY_E,
                                            .padx = {0.0f, GUI_GAP},
                                            .pady = {GUI_ROW_GAP, GUI_ROW_GAP}});
}

/* A row of buttons of one width, in a frame of its own grid. */
static tb_widget gui_button_row(tb_widget parent, const char *const *texts, size_t count,
                                tb_gui_callback *command, void *user)
{
    const tb_widget row = tb_frame(parent, NULL);

    for (size_t index = 0; index < count; index++)
    {
        const tb_widget button = gui_button(row, texts[index], command, user, false);
        const float gap = index > 0 ? GUI_ROW_GAP : 0.0f;
        (void)tb_grid(button, &(tb_grid_options){.column = (int32_t)index,
                                                 .sticky = TB_STICKY_EW,
                                                 .padx = {gap, 0.0f}});
        (void)tb_grid_column_configure(row, index,
                                       &(tb_grid_track){.weight = 1.0f, .uniform = "buttons"});
    }
    return row;
}

static void gui_build_fields(gui_scene *scene, tb_widget form)
{
    const tb_grid_options field = {
        .column = 1, .column_span = 2, .sticky = TB_STICKY_W, .pady = {GUI_ROW_GAP, GUI_ROW_GAP}};

    gui_field_label(form, 2, "Name");
    scene->name = tb_label(form, &(tb_label_desc){.text = gui_names[0]});
    (void)tb_grid(scene->name, &(tb_grid_options){.row = 2, .column = 1, .sticky = TB_STICKY_W});
    const tb_widget rename = gui_button(form, "Rename", gui_on_rename, scene, false);
    (void)tb_grid(rename, &(tb_grid_options){.row = 2, .column = 2, .sticky = TB_STICKY_EW});

    gui_field_label(form, 3, "Difficulty");
    const tb_widget difficulty =
        gui_button_row(form, gui_difficulties, 3, gui_on_difficulty, scene);
    tb_grid_options spanning = field;
    spanning.row = 3;
    spanning.sticky = TB_STICKY_EW;
    (void)tb_grid(difficulty, &spanning);

    gui_field_label(form, 4, "Volume");
    const tb_widget volume = tb_frame(form, NULL);
    tb_grid_options placed = field;
    placed.row = 4;
    (void)tb_grid(volume, &placed);
    const tb_widget quieter = gui_button(volume, "-", gui_on_quieter, scene, false);
    scene->volume = tb_label(volume, &(tb_label_desc){.text = "70%"});
    const tb_widget louder = gui_button(volume, "+", gui_on_louder, scene, false);
    (void)tb_grid(quieter, &(tb_grid_options){.column = 0, .ipadx = 6.0f});
    (void)tb_grid(scene->volume, &(tb_grid_options){.column = 1, .padx = {GUI_GAP, GUI_GAP}});
    (void)tb_grid(louder, &(tb_grid_options){.column = 2, .ipadx = 6.0f});
    /* The number's column is as wide as "100%", so the buttons stay still. */
    (void)tb_grid_column_configure(volume, 1, &(tb_grid_track){.minsize = 44.0f});

    gui_field_label(form, 5, "Theme");
    const tb_widget themes = tb_frame(form, NULL);
    tb_grid_options row = field;
    row.row = 5;
    (void)tb_grid(themes, &row);
    for (size_t index = 0; index < GUI_THEME_COUNT; index++)
    {
        const tb_widget choice =
            tb_radiobutton(themes, &(tb_radiobutton_desc){.text = gui_theme_names[index],
                                                          .variable = scene->theme,
                                                          .value = gui_theme_values[index]});
        (void)tb_grid(choice, &(tb_grid_options){.column = (int32_t)index,
                                                 .padx = {index > 0 ? GUI_GAP : 0.0f, 0.0f}});
    }
}

static void gui_build_form(gui_scene *scene, tb_widget root)
{
    static const char *const actions[] = {"Reset...", "Apply"};
    const tb_widget form = tb_frame(
        root, &(tb_frame_desc){.padding = GUI_PADDING, .background = true, .border = true});
    /* Below, room for the sandbox's frame time. */
    (void)tb_grid(form, &(tb_grid_options){.sticky = TB_STICKY_NSEW,
                                           .padx = {GUI_MARGIN, 0.0f},
                                           .pady = {GUI_MARGIN, GUI_BOTTOM_MARGIN}});

    /* A title's family is its style's: the size alone is asked for. */
    const tb_widget title = tb_label(
        form, &(tb_label_desc){.text = "Player settings", .font = {.size = GUI_TITLE_SIZE}});
    tb_widget_set_style(title, "Title.Label");
    (void)tb_grid(title, &(tb_grid_options){.column_span = 3, .sticky = TB_STICKY_W});
    const tb_widget paragraph = tb_label(
        form, &(tb_label_desc){.text = "A form laid out by grid. Resize the window: the middle "
                                       "column takes the extra width, and an empty row the extra "
                                       "height. Tab moves the focus; Space presses.",
                               .wrap_length = GUI_WRAP});
    (void)tb_grid(paragraph, &(tb_grid_options){.row = 1,
                                                .column_span = 3,
                                                .sticky = TB_STICKY_W,
                                                .pady = {GUI_ROW_GAP, GUI_GAP}});
    gui_build_fields(scene, form);

    /* The empty row takes the height; the middle column the width. */
    (void)tb_grid_row_configure(form, 6, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_column_configure(form, 1, &(tb_grid_track){.weight = 1.0f});

    scene->status = tb_label(form, &(tb_label_desc){.text = "Ready."});
    (void)tb_grid(scene->status,
                  &(tb_grid_options){.row = 7, .column_span = 2, .sticky = TB_STICKY_W});
    const tb_widget bar = tb_frame(form, NULL);
    (void)tb_grid(bar, &(tb_grid_options){.row = 7, .column = 2, .sticky = TB_STICKY_E});
    const tb_widget reset = gui_button(bar, actions[0], gui_on_show_dialog, scene, false);
    const tb_widget apply = gui_button(bar, actions[1], gui_on_apply, scene, true);
    (void)tb_grid(reset, &(tb_grid_options){.column = 0, .sticky = TB_STICKY_EW});
    (void)tb_grid(apply, &(tb_grid_options){
                             .column = 1, .sticky = TB_STICKY_EW, .padx = {GUI_ROW_GAP, 0.0f}});
    (void)tb_grid_column_configure(bar, 0, &(tb_grid_track){.uniform = "actions"});
    (void)tb_grid_column_configure(bar, 1, &(tb_grid_track){.uniform = "actions"});
}

static void gui_build_dialog(gui_scene *scene, tb_widget root)
{
    scene->dialog = tb_frame(
        root, &(tb_frame_desc){.padding = GUI_PADDING, .background = true, .border = true});
    /* Over the whole root, in the middle of it, shown only when asked. */
    (void)tb_grid(scene->dialog, &(tb_grid_options){.column_span = 2});
    tb_widget_set_visible(scene->dialog, false);
    (void)tb_bind(scene->dialog, TB_GUI_EVENT_KEY, gui_on_dialog_key, scene);

    const tb_widget question =
        tb_label(scene->dialog, &(tb_label_desc){.text = "Reset every setting to its default?",
                                                 .wrap_length = 280.0f});
    (void)tb_grid(question, &(tb_grid_options){.pady = {0.0f, GUI_GAP}});
    const tb_widget buttons = tb_frame(scene->dialog, NULL);
    (void)tb_grid(buttons, &(tb_grid_options){.row = 1, .sticky = TB_STICKY_E});
    const tb_widget cancel = gui_button(buttons, "Cancel", gui_on_cancel, scene, false);
    const tb_widget reset = gui_button(buttons, "Reset", gui_on_reset, scene, true);
    (void)tb_grid(cancel, &(tb_grid_options){.column = 0, .sticky = TB_STICKY_EW});
    (void)tb_grid(reset, &(tb_grid_options){
                             .column = 1, .sticky = TB_STICKY_EW, .padx = {GUI_ROW_GAP, 0.0f}});
    (void)tb_grid_column_configure(buttons, 0, &(tb_grid_track){.uniform = "answers"});
    (void)tb_grid_column_configure(buttons, 1, &(tb_grid_track){.uniform = "answers"});
}

/* The panel from its file, watched for changes: its commands by name. */
static void gui_build_panel(gui_scene *scene, tb_widget root)
{
    tb_widget panel = TB_WIDGET_NONE;
    tb_gui_file_error error = {0};

    (void)tb_gui_register_command(scene->gui, "say_hello", gui_on_hello, scene);
    (void)tb_gui_register_command(scene->gui, "show_reset_dialog", gui_on_show_dialog, scene);
    scene->resources = tb_vfs_create(NULL);
    if (tb_vfs_mount_directory(scene->resources, "", 0, SANDBOX_RESOURCES, 0) != TB_VFS_OK)
    {
        (void)tb_fmt_print("badger-sandbox: gui: no resources at %s; the panel is not shown\n",
                           SANDBOX_RESOURCES);
        return;
    }
    if (tb_gui_load_file(root, scene->resources, GUI_PANEL_PATH, &panel, &error) != TB_GUI_FILE_OK)
    {
        (void)tb_fmt_print("badger-sandbox: gui: %s\n", error.message);
        return;
    }
    (void)tb_gui_watch(panel, scene->resources, GUI_PANEL_PATH,
                       &(tb_gui_watch_options){.callback = gui_on_panel_reload, .user = scene});
}

/* ---------------------------------------------------------------------------
 * The scene.
 * ------------------------------------------------------------------------- */

bool gui_scene_create(sandbox *box, void **state)
{
    gui_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_ui_theme theme = tb_ui_theme_dark();

    *state = scene;
    (void)shapes_create(box, &scene->shapes);
    scene->volume_percent = 70;
    if (!tb_ui_fonts_open(box->catalog, NULL, &scene->fonts))
    {
        (void)tb_fmt_print("badger-sandbox: gui: this build of the engine carries no UI font; "
                           "the GUI is not drawn\n");
        return true;
    }
    tb_ui_theme_set_fonts(&theme, &scene->fonts);
    scene->gui = tb_gui_create(
        &(tb_gui_options){.theme = &theme, .catalog = box->catalog, .scale = box->ui_scale});
    const tb_widget root = tb_gui_root(scene->gui);
    scene->themes[GUI_THEME_LIGHT] = tb_theme_create_light();
    scene->themes[GUI_THEME_GAME] = tb_theme_create_game(box->renderer, TB_THEME_GAME_FANTASY);
    scene->themes[GUI_THEME_RETRO] = tb_theme_create_game(box->renderer, TB_THEME_GAME_RETRO);
    scene->theme = tb_var_int(scene->gui, GUI_THEME_DARK);
    (void)tb_var_trace(scene->theme, gui_on_theme, scene);
    /* The form on the left, the shapes on the right: the form takes the
     * width it wants and half of what is left. */
    (void)tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_column_configure(root, 1, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    gui_build_form(scene, root);
    scene->options = gui_options_create(root, 1, box->engine);
    gui_build_panel(scene, root);
    gui_build_dialog(scene, root);
    return true;
}

void gui_scene_destroy(sandbox *box, void *state)
{
    gui_scene *const scene = state;

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
    for (size_t index = 0; index < GUI_THEME_COUNT; index++)
    {
        tb_theme_destroy(scene->themes[index]);
    }
    tb_vfs_destroy(scene->resources);
    if (scene->options != NULL)
    {
        gui_options_destroy(scene->options);
    }
    for (size_t font = 0; font < TB_UI_FONT_COUNT; font++)
    {
        if (scene->fonts.fonts[font] != NULL)
        {
            tb_canvas_forget_font(box->renderer, scene->fonts.fonts[font]);
        }
    }
    tb_ui_fonts_close(&scene->fonts);
    if (scene->shapes != NULL)
    {
        shapes_destroy(box, scene->shapes);
    }
    tb_free(scene);
}

bool gui_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    gui_scene *const scene = state;
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
        /* Tab moves the GUI's focus here, rather than the scene. */
        return tb_gui_wants_keyboard(scene->gui) || event->key == TB_KEY_TAB;
    }
    return false;
}

void gui_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    gui_scene *const scene = state;
    uint32_t width = 0;
    uint32_t height = 0;

    shapes_draw(box, scene->shapes, frame);
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
