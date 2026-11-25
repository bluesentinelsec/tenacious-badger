/* The gui_fonts scene: a font browser over the engine's font catalog, a
 * small tool built from code alone with the retained GUI, in the dark tool
 * theme, to show that the library serves tools as it serves game menus.
 *
 * On the left a tree view of the catalog's families, each with its styles
 * and their weights; on the right, sample text typed in an entry, a
 * preview of the chosen font at the size of each role of the catalog,
 * drawn by a canvas widget in the font itself, and the font's facts in a
 * grid of labels. A paned window shares the width between them. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/gui.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stddef.h>
#include <string.h>

/* Units around the tool, below it clear of the sandbox's frame time, and
 * between its rows. */
#define FONTS_MARGIN 8.0f
#define FONTS_BOTTOM_MARGIN 40.0f
#define FONTS_GAP 8.0f
/* The preview's size, in units it wants, and between its lines. */
#define FONTS_PREVIEW_WIDTH 520.0f
#define FONTS_PREVIEW_HEIGHT 320.0f
#define FONTS_PREVIEW_GAP 6.0f
/* Bytes of a fact written, and of the sample text. */
#define FONTS_FACT_CAPACITY 96
#define FONTS_SAMPLE "Sphinx of black quartz, judge my vow. 0123456789"

/* The facts of a font the grid shows, in its order. */
typedef enum fonts_fact
{
    FONTS_FACT_FAMILY,
    FONTS_FACT_STYLE,
    FONTS_FACT_WEIGHT,
    FONTS_FACT_ITALIC,
    FONTS_FACT_CATEGORY,
    FONTS_FACT_LICENSE,
    FONTS_FACT_PIXEL_SIZE,
    FONTS_FACT_FILE,
    FONTS_FACT_COUNT
} fonts_fact;

static const char *const fonts_fact_names[FONTS_FACT_COUNT] = {
    "Family", "Style", "Weight", "Italic", "Category", "License", "Pixel size", "File"};

typedef struct fonts_scene
{
    sandbox *box;
    tb_gui *gui;
    tb_ui_fonts fonts;
    tb_widget tree;
    tb_widget facts[FONTS_FACT_COUNT];
    tb_var sample;
    tb_tree_item *items; /* each font's item, by its index in the catalog */
    size_t count;
    size_t chosen; /* the font previewed; SIZE_MAX for none */
    tb_font *face; /* the font previewed, opened from the catalog; NULL for none */
} fonts_scene;

/* ---------------------------------------------------------------------------
 * The font chosen.
 * ------------------------------------------------------------------------- */

static void fonts_close_face(fonts_scene *scene)
{
    if (scene->face == NULL)
    {
        return;
    }
    tb_canvas_forget_font(scene->box->renderer, scene->face);
    tb_font_close(scene->face);
    scene->face = NULL;
}

static void fonts_set_fact(fonts_scene *scene, fonts_fact fact, const char *text)
{
    (void)tb_widget_set_text(scene->facts[fact], text);
}

/* Show a font of the catalog: open it for the preview, and write its facts. */
static void fonts_choose(fonts_scene *scene, size_t index)
{
    const tb_font_catalog *const catalog = scene->box->catalog;
    tb_font_info info = {0};
    char text[FONTS_FACT_CAPACITY] = {0};

    if (index == scene->chosen || index >= scene->count)
    {
        return;
    }
    fonts_close_face(scene);
    scene->chosen = index;
    if (!tb_font_catalog_open(catalog, index, NULL, &scene->face))
    {
        scene->face = NULL;
    }
    tb_font_catalog_get_info(catalog, index, &info);
    fonts_set_fact(scene, FONTS_FACT_FAMILY, info.family);
    fonts_set_fact(scene, FONTS_FACT_STYLE, info.style);
    (void)tb_fmt_buffer(text, sizeof(text), "%u", (unsigned)info.weight);
    fonts_set_fact(scene, FONTS_FACT_WEIGHT, text);
    fonts_set_fact(scene, FONTS_FACT_ITALIC, info.italic ? "Yes" : "No");
    fonts_set_fact(scene, FONTS_FACT_CATEGORY, tb_font_category_name(info.category));
    fonts_set_fact(scene, FONTS_FACT_LICENSE, tb_font_license_name(info.license));
    if (info.pixel_size > 0)
    {
        (void)tb_fmt_buffer(text, sizeof(text), "%u pixels to the em", (unsigned)info.pixel_size);
    }
    else
    {
        (void)tb_fmt_buffer(text, sizeof(text), "None: it scales freely");
    }
    fonts_set_fact(scene, FONTS_FACT_PIXEL_SIZE, text);
    (void)tb_fmt_buffer(text, sizeof(text), "%s, %zu bytes",
                        info.file != NULL && info.file[0] != '\0' ? info.file : "added by the game",
                        info.length);
    fonts_set_fact(scene, FONTS_FACT_FILE, text);
}

/* The tree's selection changed: the style chosen, or a family's first. */
static void fonts_on_select(const tb_gui_event *event, void *user)
{
    fonts_scene *const scene = user;
    tb_tree_item item = TB_TREE_ROOT;

    (void)event;
    if (tb_treeview_selection(scene->tree, &item, 1) == 0)
    {
        return;
    }
    if (tb_treeview_first_child(scene->tree, item) != TB_TREE_ROOT)
    {
        item = tb_treeview_first_child(scene->tree, item);
    }
    for (size_t index = 0; index < scene->count; index++)
    {
        if (scene->items[index] == item)
        {
            fonts_choose(scene, index);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * The preview: the sample in the chosen font at each role's size.
 * ------------------------------------------------------------------------- */

static void fonts_draw_preview(tb_widget widget, tb_gui_paint *paint, void *user)
{
    const fonts_scene *const scene = user;
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_vec4 caption_color = {0.62f, 0.66f, 0.72f, 1.0f};
    const tb_vec4 text_color = {0.92f, 0.94f, 0.97f, 1.0f};
    const tb_text sample = tb_var_get_text(scene->sample);
    float top = rect.y + FONTS_PREVIEW_GAP;

    if (scene->face == NULL)
    {
        return;
    }
    for (size_t role = 0; role < TB_FONT_ROLE_COUNT; role++)
    {
        const tb_font_role_choice choice =
            tb_font_catalog_get_role(scene->box->catalog, (tb_font_role)role);
        char caption[FONTS_FACT_CAPACITY] = {0};
        (void)tb_fmt_buffer(caption, sizeof(caption), "%s, %.0f",
                            tb_font_role_name((tb_font_role)role), (double)choice.size);
        const tb_gui_text_block label = {.text = tb_text_from_cstr(caption),
                                         .anchor = TB_GUI_ANCHOR_NW};
        const tb_gui_text_block line = {.text = sample,
                                        .font = {.size = choice.size},
                                        .anchor = TB_GUI_ANCHOR_NW,
                                        .face = scene->face};
        const tb_vec2 label_size = tb_widget_measure_text(widget, &label);
        const tb_vec2 line_size = tb_widget_measure_text(widget, &line);
        tb_gui_paint_text(paint, &label,
                          (tb_canvas_rect){rect.x + FONTS_GAP, top, rect.width, label_size.y},
                          caption_color);
        top += label_size.y;
        tb_gui_paint_text(paint, &line,
                          (tb_canvas_rect){rect.x + FONTS_GAP, top, rect.width, line_size.y},
                          text_color);
        top += line_size.y + FONTS_PREVIEW_GAP;
    }
}

/* ---------------------------------------------------------------------------
 * The tool.
 * ------------------------------------------------------------------------- */

/* The tree of families and their styles, each style's weight in a column. */
static tb_widget fonts_build_tree(fonts_scene *scene, tb_widget parent)
{
    const tb_font_catalog *const catalog = scene->box->catalog;
    const tb_widget tree = tb_treeview(parent, &(tb_treeview_desc){.heading = "Family and style",
                                                                   .tree_width = 240.0f,
                                                                   .command = fonts_on_select,
                                                                   .user = scene});
    char weight[FONTS_FACT_CAPACITY] = {0};

    scene->tree = tree;
    (void)tb_treeview_add_column(tree,
                                 &(tb_treeview_column_desc){.heading = "Weight", .width = 70});
    scene->count = tb_font_catalog_count(catalog);
    scene->items = tb_calloc(scene->count, sizeof(*scene->items));
    for (size_t index = 0; index < scene->count; index++)
    {
        tb_font_info info = {0};
        tb_tree_item family = TB_TREE_ROOT;
        tb_font_catalog_get_info(catalog, index, &info);
        for (tb_tree_item item = tb_treeview_first_child(tree, TB_TREE_ROOT); item != TB_TREE_ROOT;
             item = tb_treeview_next_sibling(tree, item))
        {
            if (tb_text_equal(tb_treeview_text(tree, item), tb_text_from_cstr(info.family)))
            {
                family = item;
            }
        }
        if (family == TB_TREE_ROOT)
        {
            family = tb_treeview_insert(tree, TB_TREE_ROOT, SIZE_MAX,
                                        &(tb_tree_item_desc){.text = info.family});
        }
        (void)tb_fmt_buffer(weight, sizeof(weight), "%u", (unsigned)info.weight);
        const char *const values[] = {weight};
        scene->items[index] = tb_treeview_insert(
            tree, family, SIZE_MAX,
            &(tb_tree_item_desc){.text = info.style, .values = values, .value_count = 1});
    }
    return tree;
}

/* The facts of the chosen font, a name and a value a row. */
static tb_widget fonts_build_facts(fonts_scene *scene, tb_widget parent)
{
    const tb_widget facts = tb_frame(parent, NULL);

    for (size_t fact = 0; fact < FONTS_FACT_COUNT; fact++)
    {
        const tb_widget name = tb_label(facts, &(tb_label_desc){.text = fonts_fact_names[fact]});
        scene->facts[fact] = tb_label(facts, &(tb_label_desc){.text = ""});
        (void)tb_grid(name, &(tb_grid_options){.row = (int32_t)fact,
                                               .sticky = TB_STICKY_W,
                                               .padx = {0.0f, FONTS_GAP * 2.0f}});
        (void)tb_grid(scene->facts[fact],
                      &(tb_grid_options){.row = (int32_t)fact, .column = 1, .sticky = TB_STICKY_W});
    }
    (void)tb_grid_column_configure(facts, 1, &(tb_grid_track){.weight = 1.0f});
    return facts;
}

/* The right pane: the sample's entry, the preview, and the facts. */
static tb_widget fonts_build_details(fonts_scene *scene, tb_widget parent)
{
    const tb_widget details = tb_frame(parent, &(tb_frame_desc){.padding = FONTS_GAP});
    const tb_widget title = tb_label(details, &(tb_label_desc){.text = "Sample"});
    const tb_widget entry =
        tb_entry(details, &(tb_entry_desc){.variable = scene->sample, .width = 40});
    const tb_widget preview =
        tb_canvas_widget(details, &(tb_canvas_widget_desc){.width = FONTS_PREVIEW_WIDTH,
                                                           .height = FONTS_PREVIEW_HEIGHT,
                                                           .background = true,
                                                           .draw = fonts_draw_preview,
                                                           .user = scene});

    (void)tb_grid(title, &(tb_grid_options){.sticky = TB_STICKY_W, .padx = {0.0f, FONTS_GAP}});
    (void)tb_grid(entry, &(tb_grid_options){.column = 1, .sticky = TB_STICKY_EW});
    (void)tb_grid(preview, &(tb_grid_options){.row = 1,
                                              .column_span = 2,
                                              .sticky = TB_STICKY_NSEW,
                                              .pady = {FONTS_GAP, FONTS_GAP}});
    (void)tb_grid(fonts_build_facts(scene, details),
                  &(tb_grid_options){.row = 2, .column_span = 2, .sticky = TB_STICKY_EW});
    (void)tb_grid_column_configure(details, 1, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(details, 1, &(tb_grid_track){.weight = 1.0f});
    return details;
}

static void fonts_build(fonts_scene *scene, tb_widget root)
{
    const tb_widget window = tb_frame(root, &(tb_frame_desc){.background = true});
    const tb_widget paned =
        tb_panedwindow(window, &(tb_panedwindow_desc){.orient = TB_GUI_ORIENT_HORIZONTAL});

    (void)tb_grid(
        window, &(tb_grid_options){.sticky = TB_STICKY_NSEW, .pady = {0.0f, FONTS_BOTTOM_MARGIN}});
    (void)tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid(paned, &(tb_grid_options){.sticky = TB_STICKY_NSEW,
                                            .padx = {FONTS_MARGIN, FONTS_MARGIN},
                                            .pady = {FONTS_MARGIN, FONTS_MARGIN}});
    (void)tb_grid_column_configure(window, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(window, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_panedwindow_add(paned, fonts_build_tree(scene, paned),
                             &(tb_pane_options){.weight = 1.0f, .minsize = 200.0f});
    (void)tb_panedwindow_add(paned, fonts_build_details(scene, paned),
                             &(tb_pane_options){.weight = 2.0f, .minsize = 320.0f});

    /* The interface's own font chosen first, as if clicked: its family open. */
    size_t first = 0;
    if (scene->count == 0)
    {
        return;
    }
    (void)tb_font_catalog_find_role(scene->box->catalog, TB_FONT_ROLE_UI, &first);
    (void)tb_treeview_see(scene->tree, scene->items[first]);
    (void)tb_treeview_select(scene->tree, scene->items[first], true);
    fonts_choose(scene, first);
}

/* ---------------------------------------------------------------------------
 * The scene.
 * ------------------------------------------------------------------------- */

bool gui_fonts_scene_create(sandbox *box, void **state)
{
    fonts_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_ui_theme theme = tb_ui_theme_dark();

    *state = scene;
    scene->box = box;
    scene->chosen = SIZE_MAX;
    if (!tb_ui_fonts_open(box->catalog, NULL, &scene->fonts))
    {
        (void)tb_fmt_print("badger-sandbox: gui_fonts: this build of the engine carries no "
                           "fonts; there is nothing to browse\n");
        return true;
    }
    tb_ui_theme_set_fonts(&theme, &scene->fonts);
    scene->gui = tb_gui_create(
        &(tb_gui_options){.theme = &theme, .catalog = box->catalog, .scale = box->ui_scale});
    scene->sample = tb_var_string(scene->gui, FONTS_SAMPLE);
    fonts_build(scene, tb_gui_root(scene->gui));
    return true;
}

void gui_fonts_scene_destroy(sandbox *box, void *state)
{
    fonts_scene *const scene = state;

    if (scene == NULL)
    {
        return;
    }
    tb_window_set_cursor(box->window, TB_CURSOR_ARROW);
    fonts_close_face(scene);
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
    tb_free(scene->items);
    tb_free(scene);
}

bool gui_fonts_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    fonts_scene *const scene = state;
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
        /* Escape leaves the sandbox unless the GUI takes it. */
        return event->key != TB_KEY_ESCAPE || tb_gui_wants_keyboard(scene->gui);
    }
    return false;
}

void gui_fonts_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    fonts_scene *const scene = state;

    if (scene->gui == NULL)
    {
        return;
    }
    const tb_viewport viewport = tb_renderer_viewport(box->renderer);
    tb_gui_set_viewport(scene->gui, &viewport);
    tb_gui_update(scene->gui, frame->delta_seconds);
    tb_gui_draw(scene->gui, box->renderer);
    tb_window_set_cursor(box->window, tb_gui_cursor(scene->gui));
}
