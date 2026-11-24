/* The ui scene: every widget of the interface, in two windows over the
 * shapes. The window events go to the interface first, which takes the
 * mouse over its windows and the keys while it has the focus; the fly
 * camera has the rest. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/ui.h"

#include <math.h>
#include <string.h>

/* The windows' places and sizes, in units, from the frame's edges. */
#define UI_MARGIN 16.0f
#define UI_INSPECTOR_WIDTH 360.0f
#define UI_INSPECTOR_HEIGHT 620.0f
#define UI_STATISTICS_WIDTH 380.0f
#define UI_STATISTICS_HEIGHT 520.0f
/* Lines of the log panel. */
#define UI_LOG_LINES 24

typedef struct ui_scene
{
    void *shapes;
    tb_ui *ui;
    tb_ui_fonts fonts;
    tb_ui_input input;
    /* The values the widgets show and change. */
    bool vsync;
    bool grid;
    bool shadows;
    size_t quality;
    size_t mode;
    float field_of_view;
    int32_t cascades;
    float exposure;
    int32_t samples;
    tb_vec4 sun;
    tb_vec4 tint;
    char name[64];
    char tag[32];
    int32_t shape;
    float progress;
    int32_t clicks;
} ui_scene;

bool ui_scene_create(sandbox *box, void **state)
{
    ui_scene *const scene = tb_calloc(1, sizeof(*scene));
    tb_ui_theme theme = tb_ui_theme_dark();

    *state = scene;
    (void)shapes_create(box, &scene->shapes);
    scene->vsync = true;
    scene->shadows = true;
    scene->quality = 2;
    scene->field_of_view = 90.0f;
    scene->cascades = 3;
    scene->exposure = 1.0f;
    scene->samples = 4;
    scene->sun = (tb_vec4){1.0f, 0.92f, 0.78f, 1.0f};
    scene->tint = (tb_vec4){0.30f, 0.62f, 0.95f, 0.6f};
    memcpy(scene->name, "Crate_01", sizeof("Crate_01"));
    memcpy(scene->tag, "prop", sizeof("prop"));
    if (!tb_ui_fonts_open(box->catalog, NULL, &scene->fonts))
    {
        (void)tb_fmt_print("badger-sandbox: ui: this build of the engine carries no UI font; "
                           "the interface is not drawn\n");
        return true;
    }
    tb_ui_theme_set_fonts(&theme, &scene->fonts);
    scene->ui = tb_ui_create(&(tb_ui_options){.theme = &theme});
    return true;
}

void ui_scene_destroy(sandbox *box, void *state)
{
    ui_scene *const scene = state;

    if (scene == NULL)
    {
        return;
    }
    tb_window_set_cursor(box->window, TB_CURSOR_ARROW);
    tb_ui_destroy(scene->ui);
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

bool ui_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    ui_scene *const scene = state;
    const tb_window_event_type type = event->type;

    (void)box;
    if (scene->ui == NULL)
    {
        return false;
    }
    tb_ui_input_add_event(&scene->input, event);
    if (type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN || type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        return tb_ui_wants_mouse(scene->ui);
    }
    if (type == TB_WINDOW_EVENT_KEY_DOWN)
    {
        /* Tab moves the interface's focus here, rather than the scene. */
        return tb_ui_wants_keyboard(scene->ui) || event->key == TB_KEY_TAB;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * The windows.
 * ------------------------------------------------------------------------- */

static void ui_rendering(tb_ui *ui, ui_scene *scene)
{
    static const char *const qualities[] = {"Low", "Medium", "High", "Epic", "Cinematic"};
    static const char *const modes[] = {"Raster", "Hybrid", "Path tracing"};

    (void)tb_ui_checkbox(ui, "Vertical sync", &scene->vsync);
    tb_ui_same_line(ui);
    (void)tb_ui_checkbox(ui, "Shadows", &scene->shadows);
    tb_ui_same_line(ui);
    (void)tb_ui_checkbox(ui, "Grid", &scene->grid);
    (void)tb_ui_combo(ui, "Quality", &scene->quality, qualities, 5);
    tb_ui_tooltip(ui, "How much the renderer spends on each frame.");
    (void)tb_ui_combo(ui, "Lighting", &scene->mode, modes, 3);
    (void)tb_ui_slider_float(ui, "Field of view", &scene->field_of_view, 60.0f, 120.0f,
                             "%.0f\xC2\xB0");
    (void)tb_ui_slider_int(ui, "Cascades", &scene->cascades, 1, 4);
    (void)tb_ui_drag_float(ui, "Exposure", &scene->exposure, 0.01f, 0.0f, 8.0f, "%.2f EV");
    (void)tb_ui_drag_int(ui, "Samples", &scene->samples, 0.1f, 1, 64);
    (void)tb_ui_color_edit(ui, "Sun", &scene->sun);
}

static void ui_object(tb_ui *ui, ui_scene *scene)
{
    static const float buttons[] = {0.0f, 0.0f, 0.0f};

    (void)tb_ui_text_field(ui, "Name", scene->name, sizeof(scene->name));
    (void)tb_ui_text_field(ui, "Tag", scene->tag, sizeof(scene->tag));
    tb_ui_value(ui, "Vertices", "24,576");
    (void)tb_ui_radio(ui, "Cube", &scene->shape, 0);
    tb_ui_same_line(ui);
    (void)tb_ui_radio(ui, "Sphere", &scene->shape, 1);
    tb_ui_same_line(ui);
    (void)tb_ui_radio(ui, "Cylinder", &scene->shape, 2);
    (void)tb_ui_color_swatch(ui, "Tint", scene->tint);
    tb_ui_progress(ui, scene->progress, NULL);
    tb_ui_row(ui, 3, buttons);
    if (tb_ui_button(ui, "Apply"))
    {
        scene->clicks++;
    }
    tb_ui_tooltip(ui, "Apply the changes to the object.");
    (void)tb_ui_button(ui, "Revert");
    (void)tb_ui_button(ui, "Delete");
    tb_ui_row(ui, 0, NULL);
}

static void ui_hierarchy(tb_ui *ui)
{
    if (tb_ui_tree_push(ui, "World", true))
    {
        if (tb_ui_tree_push(ui, "Lights", true))
        {
            (void)tb_ui_selectable(ui, "Sun", true);
            (void)tb_ui_selectable(ui, "Fill light", false);
            tb_ui_tree_pop(ui);
        }
        if (tb_ui_tree_push(ui, "Meshes", false))
        {
            (void)tb_ui_selectable(ui, "Crate_01", false);
            tb_ui_tree_pop(ui);
        }
        tb_ui_tree_pop(ui);
    }
}

static void ui_inspector(tb_ui *ui, ui_scene *scene)
{
    if (!tb_ui_begin_window(
            ui, "Inspector",
            (tb_canvas_rect){UI_MARGIN, UI_MARGIN, UI_INSPECTOR_WIDTH, UI_INSPECTOR_HEIGHT}, 0))
    {
        return;
    }
    if (tb_ui_header(ui, "Rendering", true))
    {
        ui_rendering(ui, scene);
    }
    if (tb_ui_header(ui, "Object", true))
    {
        ui_object(ui, scene);
    }
    if (tb_ui_header(ui, "Hierarchy", true))
    {
        ui_hierarchy(ui);
    }
    tb_ui_end_window(ui);
}

static void ui_statistics(tb_ui *ui, const ui_scene *scene, const sandbox_frame *frame, float right)
{
    static const char *const headers[] = {"Pass", "CPU", "GPU"};
    static const float widths[] = {0.0f, 80.0f, 80.0f};
    static const char *const passes[] = {"Shadows", "Scene", "Sky", "Canvas"};
    static const float times[] = {0.42f, 1.37f, 0.08f, 0.21f};

    if (!tb_ui_begin_window(ui, "Statistics",
                            (tb_canvas_rect){right - UI_MARGIN - UI_STATISTICS_WIDTH, UI_MARGIN,
                                             UI_STATISTICS_WIDTH, UI_STATISTICS_HEIGHT},
                            0))
    {
        return;
    }
    tb_ui_heading(ui, "Frame");
    tb_ui_textf(ui, "%.2f ms a frame, %.0f seconds in", frame->delta_seconds * 1000.0,
                frame->seconds);
    tb_ui_text_wrapped(ui, "The interface is drawn through the canvas, over the shapes. Drag the "
                           "windows by their titles, resize them by their corners, and hold the "
                           "right mouse button outside them to fly.");
    tb_ui_separator(ui);
    tb_ui_begin_table(ui, "Passes", 3, headers, widths);
    for (size_t pass = 0; pass < 4; pass++)
    {
        tb_ui_text(ui, passes[pass]);
        tb_ui_textf(ui, "%.2f ms", (double)times[pass]);
        tb_ui_textf(ui, "%.2f ms", (double)(times[pass] * 1.6f));
    }
    tb_ui_end_table(ui);
    tb_ui_text_colored(ui, (tb_vec4){0.55f, 0.85f, 0.45f, 1.0f}, "Validation: no messages");
    tb_ui_textf(ui, "Apply clicked %d times", (int)scene->clicks);
    tb_ui_begin_panel(ui, "Log", 0.0f);
    for (int line = 0; line < UI_LOG_LINES; line++)
    {
        tb_ui_textf(ui, "[%02d] Loaded mesh %d of %d", line, line + 1, UI_LOG_LINES);
    }
    tb_ui_end_panel(ui);
    tb_ui_end_window(ui);
}

void ui_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    ui_scene *const scene = state;
    tb_ui *const ui = scene->ui;
    uint32_t width = 0;
    uint32_t height = 0;

    shapes_draw(box, scene->shapes, frame);
    if (ui == NULL)
    {
        return;
    }
    tb_renderer_size(box->renderer, &width, &height);
    scene->input.scale = tb_canvas_view_logical(box->renderer).scale * box->ui_scale;
    scene->input.size = (tb_vec2){(float)width, (float)height};
    scene->input.delta_seconds = frame->delta_seconds;
    scene->progress = (float)fmod(frame->seconds * 0.1, 1.0);

    tb_ui_begin_frame(ui, &scene->input);
    ui_inspector(ui, scene);
    ui_statistics(ui, scene, frame, (float)width / scene->input.scale);
    tb_ui_end_frame(ui);
    tb_ui_input_next_frame(&scene->input);
    tb_ui_draw(ui, box->renderer);
    tb_window_set_cursor(box->window, tb_ui_cursor(ui));
}
