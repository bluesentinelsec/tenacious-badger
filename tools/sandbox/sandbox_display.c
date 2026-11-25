/* The display scene: the display policy and the viewport it gives the
 * window, to see by eye how an app's design resolution is placed on a
 * window of any size and shape (docs/display.md).
 *
 * The shapes, lit by a sun under a procedural sky, are drawn in the
 * viewport, with the policy's bars beside it. Over them, in the design's
 * units: the design's border with a marker at each corner, the edge of
 * what the viewport shows, and a grid centered on it. A panel of the GUI
 * in its bottom right corner, in the same units, holds a button for each
 * hotkey; the mouse over the
 * bars reaches none of them. In the top left corner, in logical units,
 * what the window, the frame and the viewport are, where the mouse is in
 * pixels and in units, and the policy.
 *
 * F1 to F5 change the policy and the window through the engine's settings,
 * at the runtime layer, as the inspector does, so that the inspector (the
 * backquote) shows them change: the aspect, the scale mode, the render mode,
 * the design size, and the window's mode. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/gui.h"
#include "badger/light.h"
#include "badger/memory.h"
#include "badger/sky.h"
#include "badger/str.h"
#include "badger/viewport.h"

#include <math.h>
#include <stddef.h>

/* The design sizes F4 cycles through. */
#define DISPLAY_PRESET_COUNT 4u
static const uint32_t display_presets[DISPLAY_PRESET_COUNT][2] = {
    {1920u, 1080u}, {1280u, 720u}, {640u, 360u}, {320u, 180u}};

/* The window modes F5 cycles through, in the order of tb_window_mode. */
#define DISPLAY_WINDOW_MODES 3u
static const char *const display_window_modes[DISPLAY_WINDOW_MODES] = {"windowed", "borderless",
                                                                       "fullscreen"};

/* The panel is laid out at this many units of the viewport's, whatever the
 * design size, as Burrow's menus are at theirs. */
#define DISPLAY_PANEL_WIDTH 960.0f
#define DISPLAY_PANEL_HEIGHT 540.0f
#define DISPLAY_PANEL_MARGIN 12.0f
#define DISPLAY_PANEL_PADDING 10.0f
#define DISPLAY_PANEL_GAP 4.0f
/* Grid cells a design's height, and the markers' length in cells. */
#define DISPLAY_GRID_CELLS 9.0f
#define DISPLAY_MARKER_CELLS 0.5f
/* Pixels wide the design's lines are drawn, at any scale. */
#define DISPLAY_LINE_PIXELS 2.0f
#define DISPLAY_GRID_PIXELS 1.0f
/* The overlay of facts: its text's size and margin, in logical units, and
 * the layers it and the design's lines are drawn on. */
#define DISPLAY_TEXT_SIZE 13.0f
#define DISPLAY_TEXT_MARGIN 8.0f
#define DISPLAY_LINES_LAYER 10
#define DISPLAY_OVERLAY_LAYER 900
#define DISPLAY_LINE_CAPACITY 160u
#define DISPLAY_LINE_COUNT 12u

/* What each button of the panel does: a hotkey's action. */
typedef enum display_action
{
    DISPLAY_ACTION_ASPECT,
    DISPLAY_ACTION_SCALE_MODE,
    DISPLAY_ACTION_RENDER_MODE,
    DISPLAY_ACTION_DESIGN,
    DISPLAY_ACTION_WINDOW,
    DISPLAY_ACTION_COUNT
} display_action;

typedef struct display_scene display_scene;

/* A button's command: its scene and its action. */
typedef struct display_button
{
    display_scene *scene;
    display_action action;
    tb_widget widget;
    char text[64]; /* what it shows, so it is set again only when it changes */
} display_button;

struct display_scene
{
    sandbox *box;
    void *shapes;
    tb_light_id sun;
    tb_gui *gui;
    tb_ui_fonts fonts;
    display_button buttons[DISPLAY_ACTION_COUNT];
    tb_widget clicked; /* the label that says which button was clicked last */
    tb_vec2 mouse;     /* in the frame's pixels */
    bool mouse_seen;
    char lines[DISPLAY_LINE_COUNT][DISPLAY_LINE_CAPACITY];
};

/* ---------------------------------------------------------------------------
 * The policy, changed through the settings.
 * ------------------------------------------------------------------------- */

static void display_next_choice(tb_setting *setting)
{
    const size_t count = tb_setting_choice_count(setting);

    (void)tb_setting_set_choice(setting, (tb_setting_choice(setting) + 1u) % count);
}

/* The next design size after the one in the settings; the first when it is
 * none of them. */
static void display_next_design(const tb_engine_settings *engine)
{
    const int64_t width = tb_setting_integer(engine->display.design_width);
    const int64_t height = tb_setting_integer(engine->display.design_height);
    size_t next = 0;

    for (size_t index = 0; index < DISPLAY_PRESET_COUNT; index++)
    {
        if (width == (int64_t)display_presets[index][0] &&
            height == (int64_t)display_presets[index][1])
        {
            next = (index + 1u) % DISPLAY_PRESET_COUNT;
        }
    }
    (void)tb_setting_set_integer(engine->display.design_width, (int64_t)display_presets[next][0]);
    (void)tb_setting_set_integer(engine->display.design_height, (int64_t)display_presets[next][1]);
}

static void display_act(const display_scene *scene, display_action action)
{
    const tb_engine_settings *const engine = scene->box->engine;

    switch (action)
    {
    case DISPLAY_ACTION_ASPECT:
        display_next_choice(engine->display.stretch_aspect);
        break;
    case DISPLAY_ACTION_SCALE_MODE:
        display_next_choice(engine->display.stretch_scale_mode);
        break;
    case DISPLAY_ACTION_RENDER_MODE:
        display_next_choice(engine->display.stretch_render_mode);
        break;
    case DISPLAY_ACTION_DESIGN:
        display_next_design(engine);
        break;
    case DISPLAY_ACTION_WINDOW:
        /* The game loop applies it to the window. */
        display_next_choice(engine->display.window_mode);
        break;
    case DISPLAY_ACTION_COUNT:
        break;
    }
}

/* What the panel says a click did, by action. */
static const char *const display_clicks[DISPLAY_ACTION_COUNT] = {
    "Clicked: the next aspect", "Clicked: the other scale mode", "Clicked: the other render mode",
    "Clicked: the next design size", "Clicked: the next window mode"};

static void display_clicked(const tb_gui_event *event, void *user)
{
    const display_button *const button = user;

    (void)event;
    display_act(button->scene, button->action);
    (void)tb_widget_set_text(button->scene->clicked, display_clicks[button->action]);
}

/* What a button says: what it changes, its value now, and its key. */
static void display_button_text(const display_scene *scene, display_action action, char *text,
                                size_t capacity)
{
    const tb_engine_settings *const engine = scene->box->engine;
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
    const size_t mode = tb_setting_choice(engine->display.window_mode);

    tb_viewport_policy_from_settings(engine, &policy);
    switch (action)
    {
    case DISPLAY_ACTION_ASPECT:
        (void)tb_fmt_buffer(text, capacity, "F1  Aspect: %s",
                            tb_viewport_aspect_name(policy.aspect));
        return;
    case DISPLAY_ACTION_SCALE_MODE:
        (void)tb_fmt_buffer(text, capacity, "F2  Scale: %s",
                            tb_viewport_scale_mode_name(policy.scale_mode));
        return;
    case DISPLAY_ACTION_RENDER_MODE:
        (void)tb_fmt_buffer(text, capacity, "F3  Render: %s",
                            tb_viewport_render_mode_name(policy.render_mode));
        return;
    case DISPLAY_ACTION_DESIGN:
        (void)tb_fmt_buffer(text, capacity, "F4  Design: %u x %u", policy.design_width,
                            policy.design_height);
        return;
    case DISPLAY_ACTION_WINDOW:
    case DISPLAY_ACTION_COUNT:
        break;
    }
    (void)tb_fmt_buffer(text, capacity, "F5  Window: %s",
                        mode < DISPLAY_WINDOW_MODES ? display_window_modes[mode] : "unknown");
}

/* ---------------------------------------------------------------------------
 * The panel.
 * ------------------------------------------------------------------------- */

static void display_build_panel(display_scene *scene)
{
    const tb_widget root = tb_gui_root(scene->gui);
    const tb_widget panel = tb_frame(
        root,
        &(tb_frame_desc){.padding = DISPLAY_PANEL_PADDING, .background = true, .border = true});
    const tb_widget title = tb_label(panel, &(tb_label_desc){.text = "Display policy"});

    /* In the viewport's bottom right corner, clear of the facts. */
    (void)tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid(panel, &(tb_grid_options){.row = 0,
                                            .column = 0,
                                            .sticky = TB_STICKY_S | TB_STICKY_E,
                                            .padx = {0.0f, DISPLAY_PANEL_MARGIN},
                                            .pady = {0.0f, DISPLAY_PANEL_MARGIN}});
    (void)tb_grid(title, &(tb_grid_options){.row = 0,
                                            .column = 0,
                                            .sticky = TB_STICKY_W,
                                            .pady = {0.0f, DISPLAY_PANEL_GAP}});
    for (size_t index = 0; index < DISPLAY_ACTION_COUNT; index++)
    {
        display_button *const button = &scene->buttons[index];
        button->scene = scene;
        button->action = (display_action)index;
        display_button_text(scene, button->action, button->text, sizeof(button->text));
        button->widget = tb_button(
            panel,
            &(tb_button_desc){.text = button->text, .command = display_clicked, .user = button});
        (void)tb_grid(button->widget, &(tb_grid_options){.row = (int32_t)index + 1,
                                                         .column = 0,
                                                         .sticky = TB_STICKY_EW,
                                                         .pady = {DISPLAY_PANEL_GAP, 0.0f}});
    }
    scene->clicked = tb_label(panel, &(tb_label_desc){.text = "Click a button, or press its key"});
    (void)tb_grid(scene->clicked, &(tb_grid_options){.row = (int32_t)DISPLAY_ACTION_COUNT + 1,
                                                     .column = 0,
                                                     .sticky = TB_STICKY_W,
                                                     .pady = {DISPLAY_PANEL_GAP, 0.0f}});
}

/* Set each button's text again where its value changed. */
static void display_refresh_panel(display_scene *scene)
{
    for (size_t index = 0; index < DISPLAY_ACTION_COUNT; index++)
    {
        display_button *const button = &scene->buttons[index];
        char text[sizeof(button->text)] = {0};
        display_button_text(scene, button->action, text, sizeof(text));
        if (tb_str_equal(text, button->text))
        {
            continue;
        }
        (void)tb_fmt_buffer(button->text, sizeof(button->text), "%s", text);
        (void)tb_widget_set_text(button->widget, button->text);
    }
}

/* ---------------------------------------------------------------------------
 * Making and destroying.
 * ------------------------------------------------------------------------- */

bool display_scene_create(sandbox *box, void **state)
{
    display_scene *const scene = tb_calloc(1, sizeof(*scene));
    const tb_sky sky = TB_SKY_PROCEDURAL_INIT;
    tb_light sun = TB_LIGHT_INIT;
    tb_ui_theme theme = tb_ui_theme_dark();

    *state = scene;
    scene->box = box;
    if (!shapes_create(box, &scene->shapes))
    {
        return false;
    }
    sun.kind = TB_LIGHT_DIRECTIONAL;
    sun.direction = tb_vec3_negate(sky.sun_direction);
    sun.color = sky.sun_color;
    sun.intensity = 1.5f;
    scene->sun = tb_renderer_add_light(box->renderer, &sun);
    tb_renderer_set_sky(box->renderer, &sky);
    if (!tb_ui_fonts_open(box->catalog, NULL, &scene->fonts))
    {
        (void)tb_fmt_print("badger-sandbox: display: this build of the engine carries no fonts; "
                           "the panel and the facts are not drawn\n");
        return true;
    }
    tb_ui_theme_set_fonts(&theme, &scene->fonts);
    scene->gui = tb_gui_create(&(tb_gui_options){.theme = &theme, .catalog = box->catalog});
    display_build_panel(scene);
    return true;
}

void display_scene_destroy(sandbox *box, void *state)
{
    display_scene *const scene = state;

    if (scene == NULL)
    {
        return;
    }
    tb_window_set_cursor(box->window, TB_CURSOR_ARROW);
    tb_renderer_remove_light(box->renderer, scene->sun);
    tb_renderer_set_sky(box->renderer, NULL);
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
    if (scene->shapes != NULL)
    {
        shapes_destroy(box, scene->shapes);
    }
    tb_free(scene);
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

static bool display_hotkey(display_scene *scene, const tb_window_event *event)
{
    if (event->key < TB_KEY_F1 || event->key > TB_KEY_F5)
    {
        return false;
    }
    if (event->type == TB_WINDOW_EVENT_KEY_DOWN && !event->repeat)
    {
        display_act(scene, (display_action)(event->key - TB_KEY_F1));
    }
    return true;
}

bool display_scene_event(sandbox *box, void *state, const tb_window_event *event)
{
    display_scene *const scene = state;
    const tb_window_event_type type = event->type;

    (void)box;
    if (type == TB_WINDOW_EVENT_MOUSE_MOVED)
    {
        scene->mouse = (tb_vec2){(float)event->pixel_x, (float)event->pixel_y};
        scene->mouse_seen = true;
    }
    if ((type == TB_WINDOW_EVENT_KEY_DOWN || type == TB_WINDOW_EVENT_KEY_UP) &&
        display_hotkey(scene, event))
    {
        return true;
    }
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
        return event->key != TB_KEY_ESCAPE && tb_gui_wants_keyboard(scene->gui);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------- */

/* The design's units: its size, or, without one, what the viewport shows. */
static tb_vec2 display_design_units(const tb_viewport_policy *policy, const tb_viewport *viewport)
{
    if (policy->design_width == 0 || policy->design_height == 0)
    {
        return viewport->size;
    }
    const float factor = policy->scale > 0.0f ? policy->scale : 1.0f;
    return (tb_vec2){(float)policy->design_width / factor, (float)policy->design_height / factor};
}

/* An L at a corner of the design, its arms along the design's edges. */
static void display_marker(tb_renderer *renderer, tb_vec2 corner, tb_vec2 inward, float length,
                           float width, tb_vec4 color)
{
    tb_canvas_line(renderer, corner, (tb_vec2){corner.x + inward.x * length, corner.y}, width,
                   color);
    tb_canvas_line(renderer, corner, (tb_vec2){corner.x, corner.y + inward.y * length}, width,
                   color);
}

/* A grid centered on what the viewport shows, a design's height in cells. */
static void display_grid(tb_renderer *renderer, const tb_viewport *viewport, float cell,
                         float width)
{
    if (!(cell > 0.0f))
    {
        return;
    }
    const tb_vec4 color = {1.0f, 1.0f, 1.0f, 0.18f};
    const tb_vec2 middle = tb_vec2_scale(viewport->size, 0.5f);
    const int32_t across = (int32_t)ceilf(middle.x / cell);
    const int32_t down = (int32_t)ceilf(middle.y / cell);

    for (int32_t step = -across; step <= across; step++)
    {
        const float x = middle.x + (float)step * cell;
        tb_canvas_line(renderer, (tb_vec2){x, 0.0f}, (tb_vec2){x, viewport->size.y}, width, color);
    }
    for (int32_t step = -down; step <= down; step++)
    {
        const float y = middle.y + (float)step * cell;
        tb_canvas_line(renderer, (tb_vec2){0.0f, y}, (tb_vec2){viewport->size.x, y}, width, color);
    }
}

/* In the design's units: the grid, the edge of what the viewport shows,
 * the design's border and corners, and its middle. */
static void display_draw_design(tb_renderer *renderer, const tb_viewport_policy *policy,
                                const tb_viewport *viewport)
{
    const tb_canvas_view view = tb_canvas_view_viewport(viewport);
    const tb_vec2 design = display_design_units(policy, viewport);
    const float pixel = 1.0f / view.scale;
    const float line = DISPLAY_LINE_PIXELS * pixel;
    const float cell = design.y / DISPLAY_GRID_CELLS;
    const float marker = cell * DISPLAY_MARKER_CELLS;
    const tb_vec4 shown = {0.3f, 0.85f, 1.0f, 0.9f};
    const tb_vec4 border = {1.0f, 0.85f, 0.3f, 1.0f};

    tb_canvas_set_view(renderer, &view);
    tb_canvas_set_layer(renderer, DISPLAY_LINES_LAYER);
    display_grid(renderer, viewport, cell, DISPLAY_GRID_PIXELS * pixel);
    tb_canvas_stroke_rect(
        renderer, (tb_canvas_rect){0.0f, 0.0f, viewport->size.x, viewport->size.y}, line, shown);
    tb_canvas_stroke_rect(renderer, (tb_canvas_rect){0.0f, 0.0f, design.x, design.y}, line, border);
    display_marker(renderer, (tb_vec2){0.0f, 0.0f}, (tb_vec2){1.0f, 1.0f}, marker, line * 2.0f,
                   border);
    display_marker(renderer, (tb_vec2){design.x, 0.0f}, (tb_vec2){-1.0f, 1.0f}, marker, line * 2.0f,
                   border);
    display_marker(renderer, (tb_vec2){0.0f, design.y}, (tb_vec2){1.0f, -1.0f}, marker, line * 2.0f,
                   border);
    display_marker(renderer, design, (tb_vec2){-1.0f, -1.0f}, marker, line * 2.0f, border);
    tb_canvas_stroke_circle(renderer, tb_vec2_scale(design, 0.5f), marker * 0.5f, line, border);
}

/* The facts, a line each. */
static size_t display_facts(display_scene *scene, const tb_viewport_policy *policy,
                            const tb_viewport *viewport)
{
    sandbox *const box = scene->box;
    const tb_engine_settings *const engine = box->engine;
    tb_window_state state = {0};
    uint32_t width = 0;
    uint32_t height = 0;
    size_t count = 0;
    const tb_vec2 units = tb_viewport_from_pixels(viewport, scene->mouse);
    const bool over = tb_viewport_contains(viewport, scene->mouse);
    const size_t ratio = tb_setting_choice(engine->display.aspect_ratio);

    tb_window_state_of(box->window, &state);
    tb_renderer_size(box->renderer, &width, &height);
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "Window   %u x %u logical, %u x %u pixels, display scale %.2f", state.width,
                        state.height, width, height, state.content_scale);
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "Viewport %u x %u pixels at (%u, %u)", viewport->width, viewport->height,
                        viewport->x, viewport->y);
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "         %.3f x %.3f pixels a unit, %.1f x %.1f units shown",
                        (double)viewport->scale.x, (double)viewport->scale.y,
                        (double)viewport->size.x, (double)viewport->size.y);
    if (scene->mouse_seen)
    {
        (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                            "Mouse    (%.0f, %.0f) pixels, (%.1f, %.1f) units, %s",
                            (double)scene->mouse.x, (double)scene->mouse.y, (double)units.x,
                            (double)units.y, over ? "in the viewport" : "on the bars");
    }
    else
    {
        (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                            "Mouse    not over the window yet");
    }
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "Policy   design %u x %u, aspect %s, scale %s x %.2f", policy->design_width,
                        policy->design_height, tb_viewport_aspect_name(policy->aspect),
                        tb_viewport_scale_mode_name(policy->scale_mode), (double)policy->scale);
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "         render %s%s, bars (%.2f, %.2f, %.2f), player's aspect %s",
                        tb_viewport_render_mode_name(policy->render_mode),
                        policy->render_mode == TB_VIEWPORT_RENDER_VIEWPORT
                            ? " (drawn as canvas until the viewport mode arrives)"
                            : "",
                        (double)policy->bar_color.x, (double)policy->bar_color.y,
                        (double)policy->bar_color.z,
                        tb_setting_choice_name(engine->display.aspect_ratio, ratio));
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "Keys     F1 aspect, F2 scale mode, F3 render mode (viewport mode arrives "
                        "in a later change)");
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "         F4 design size (1920x1080, 1280x720, 640x360, 320x180), F5 "
                        "window mode; ` the inspector");
    (void)tb_fmt_buffer(scene->lines[count++], DISPLAY_LINE_CAPACITY,
                        "Lines    gold: the design, its corners and middle; blue: what the "
                        "viewport shows");
    return count;
}

/* The facts in the top left corner of the window, in logical units, over
 * the bars and the viewport alike. */
static void display_draw_facts(display_scene *scene, const tb_viewport_policy *policy,
                               const tb_viewport *viewport)
{
    tb_renderer *const renderer = scene->box->renderer;
    const tb_font *const font = scene->box->fonts[SANDBOX_FONT_MONOSPACE] != NULL
                                    ? scene->box->fonts[SANDBOX_FONT_MONOSPACE]
                                    : scene->box->fonts[SANDBOX_FONT_UI];
    const tb_canvas_view view = tb_canvas_view_logical(renderer);
    const size_t count = display_facts(scene, policy, viewport);
    float widest = 0.0f;
    float line_height = 0.0f;

    if (font == NULL)
    {
        return;
    }
    tb_canvas_set_view(renderer, &view);
    tb_canvas_set_layer(renderer, DISPLAY_OVERLAY_LAYER);
    for (size_t index = 0; index < count; index++)
    {
        const tb_vec2 size = tb_canvas_measure_text(
            renderer, font, tb_text_from_cstr(scene->lines[index]), DISPLAY_TEXT_SIZE, NULL);
        widest = fmaxf(widest, size.x);
        line_height = fmaxf(line_height, size.y);
    }
    tb_canvas_fill_rounded_rect(
        renderer,
        (tb_canvas_rect){DISPLAY_TEXT_MARGIN, DISPLAY_TEXT_MARGIN,
                         widest + DISPLAY_TEXT_MARGIN * 2.0f,
                         line_height * (float)count + DISPLAY_TEXT_MARGIN * 2.0f},
        6.0f, (tb_vec4){0.0f, 0.0f, 0.0f, 0.65f});
    for (size_t index = 0; index < count; index++)
    {
        tb_canvas_draw_text(renderer, font, tb_text_from_cstr(scene->lines[index]),
                            DISPLAY_TEXT_SIZE,
                            (tb_vec2){DISPLAY_TEXT_MARGIN * 2.0f,
                                      DISPLAY_TEXT_MARGIN * 2.0f + line_height * (float)index},
                            (tb_vec4){0.9f, 0.95f, 1.0f, 1.0f}, NULL);
    }
}

static void display_draw_panel(display_scene *scene, const tb_viewport *viewport,
                               const sandbox_frame *frame)
{
    sandbox *const box = scene->box;
    /* The panel keeps its size against the viewport, at any design size. */
    const float fit =
        fminf(viewport->size.x / DISPLAY_PANEL_WIDTH, viewport->size.y / DISPLAY_PANEL_HEIGHT);

    if (scene->gui == NULL)
    {
        return;
    }
    display_refresh_panel(scene);
    if (fit > 0.0f)
    {
        tb_gui_set_scale(scene->gui, fit * box->ui_scale);
    }
    tb_gui_set_viewport(scene->gui, viewport);
    tb_gui_update(scene->gui, frame->delta_seconds);
    tb_gui_draw(scene->gui, box->renderer);
    tb_window_set_cursor(box->window, tb_gui_cursor(scene->gui));
}

void display_scene_draw(sandbox *box, void *state, const sandbox_frame *frame)
{
    display_scene *const scene = state;
    const tb_viewport viewport = tb_renderer_viewport(box->renderer);
    tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;

    tb_renderer_viewport_policy(box->renderer, &policy);
    shapes_draw(box, scene->shapes, frame);
    display_draw_design(box->renderer, &policy, &viewport);
    display_draw_panel(scene, &viewport, frame);
    display_draw_facts(scene, &policy, &viewport);
}
