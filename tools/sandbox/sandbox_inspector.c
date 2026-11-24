/* The inspector: the sandbox's own interface over every scene, shown and
 * hidden by F1 or the backquote. Its Settings window lists every engine
 * setting by family, each changed by the widget its type wants and applied
 * at once at the runtime layer; its Frame window shows frame times, the
 * device, what the canvas drew, and the render graph's last frame. */
#include "sandbox.h"

#include "badger/canvas.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/render_graph.h"
#include "badger/str.h"
#include "badger/ui.h"

#include <math.h>
#include <string.h>

/* The inspector's windows, in units from the frame's edges. */
#define INSPECTOR_MARGIN 16.0f
#define INSPECTOR_SETTINGS_WIDTH 460.0f
/* Room left at the bottom for the frame time the sandbox draws there. */
#define INSPECTOR_STATUS_ROOM 48.0f
#define INSPECTOR_FRAME_WIDTH 420.0f
#define INSPECTOR_FRAME_HEIGHT 560.0f
/* Its canvas layer: over the scenes' interfaces' windows. */
#define INSPECTOR_LAYER 400
/* Frame times kept for the graph, and how often the slow text is read again. */
#define INSPECTOR_HISTORY 120
#define INSPECTOR_REFRESH_SECONDS 0.5
/* Bytes of the render graph's description kept. */
#define INSPECTOR_DUMP_CAPACITY 8192
/* Choices a combo of a setting lists at most. */
#define INSPECTOR_MOST_CHOICES 32
/* An integer setting of a range at most this wide is a slider; wider, a
 * number dragged. Likewise a number's. */
#define INSPECTOR_SLIDER_SPAN 1000.0
/* How many steps a dragged number takes across its range, at most. */
#define INSPECTOR_DRAG_STEPS 2000.0

struct sandbox_inspector
{
    tb_settings *settings;
    tb_engine_settings *engine;
    tb_ui *ui;
    tb_ui_fonts fonts;
    tb_ui_input input;
    bool shown;
    /* Text settings' buffers, one for each setting, and the version each
     * was read at. */
    char **texts;
    uint64_t *text_versions;
    size_t setting_count;
    /* Frames. */
    float history[INSPECTOR_HISTORY];
    size_t history_count;
    size_t history_next;
    double since_refresh;
    tb_canvas_stats canvas;
    char *dump;
};

sandbox_inspector *sandbox_inspector_create(sandbox *box, tb_settings *settings,
                                            tb_engine_settings *engine, bool shown)
{
    sandbox_inspector *const inspector = tb_calloc(1, sizeof(*inspector));
    tb_ui_theme theme = tb_ui_theme_dark();

    inspector->settings = settings;
    inspector->engine = engine;
    inspector->shown = shown;
    inspector->setting_count = tb_settings_count(settings);
    inspector->texts = tb_calloc(inspector->setting_count, sizeof(*inspector->texts));
    inspector->text_versions = tb_calloc(inspector->setting_count, sizeof(uint64_t));
    for (size_t index = 0; index < inspector->setting_count; index++)
    {
        const tb_setting *const setting = tb_settings_at(settings, index);
        if (tb_setting_type_of(setting) == TB_SETTING_TEXT)
        {
            inspector->texts[index] = tb_calloc(tb_setting_text_limit(setting) + 1, 1);
        }
    }
    inspector->dump = tb_calloc(INSPECTOR_DUMP_CAPACITY, 1);
    if (!tb_ui_fonts_open(box->catalog, NULL, &inspector->fonts))
    {
        return inspector; /* a build that carries no fonts shows no inspector */
    }
    tb_ui_theme_set_fonts(&theme, &inspector->fonts);
    theme.label_fraction = 0.45f;
    inspector->ui = tb_ui_create(&(tb_ui_options){.theme = &theme, .layer = INSPECTOR_LAYER});
    return inspector;
}

void sandbox_inspector_destroy(sandbox_inspector *inspector, sandbox *box)
{
    if (inspector == NULL)
    {
        return;
    }
    tb_ui_destroy(inspector->ui);
    for (size_t font = 0; font < TB_UI_FONT_COUNT; font++)
    {
        if (inspector->fonts.fonts[font] != NULL)
        {
            tb_canvas_forget_font(box->renderer, inspector->fonts.fonts[font]);
        }
    }
    tb_ui_fonts_close(&inspector->fonts);
    for (size_t index = 0; index < inspector->setting_count; index++)
    {
        tb_free(inspector->texts[index]);
    }
    tb_free(inspector->texts);
    tb_free(inspector->text_versions);
    tb_free(inspector->dump);
    tb_free(inspector);
}

bool sandbox_inspector_event(sandbox_inspector *inspector, const tb_window_event *event)
{
    const tb_window_event_type type = event->type;

    if (inspector->ui == NULL)
    {
        return false;
    }
    if (type == TB_WINDOW_EVENT_KEY_DOWN && !event->repeat &&
        (event->key == TB_KEY_F1 ||
         (event->key == TB_KEY_GRAVE && !tb_ui_wants_text(inspector->ui))))
    {
        inspector->shown = !inspector->shown;
        return true;
    }
    tb_ui_input_add_event(&inspector->input, event);
    if (!inspector->shown)
    {
        return false;
    }
    if (type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN || type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        return tb_ui_wants_mouse(inspector->ui);
    }
    if (type == TB_WINDOW_EVENT_KEY_DOWN)
    {
        return tb_ui_wants_keyboard(inspector->ui);
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Settings.
 * ------------------------------------------------------------------------- */

/* A setting's label: its name without its family's, in words, as a menu
 * shows it: "Background frame limit" of "display.background_frame_limit". */
static void inspector_label(const tb_setting *setting, char *label, size_t capacity)
{
    const char *const name = tb_setting_name(setting);
    const size_t dot = tb_str_find_char(name, 0, TB_STR_END, '.');
    const char *const words = dot == TB_STR_NOT_FOUND ? name : name + dot + 1;
    size_t length = 0;

    for (; words[length] != '\0' && length + 1 < capacity; length++)
    {
        const char letter = words[length];
        label[length] = letter == '_' || letter == '.' ? ' ' : letter;
    }
    label[length] = '\0';
    if (label[0] >= 'a' && label[0] <= 'z')
    {
        label[0] = (char)(label[0] - 'a' + 'A');
    }
}

static void inspector_integer(tb_ui *ui, const char *label, tb_setting *setting)
{
    int64_t minimum = 0;
    int64_t maximum = 0;
    const int64_t value = tb_setting_integer(setting);
    int32_t shown = 0;

    tb_setting_integer_range(setting, &minimum, &maximum);
    minimum = minimum < INT32_MIN ? INT32_MIN : minimum;
    maximum = maximum > INT32_MAX ? INT32_MAX : maximum;
    shown = (int32_t)(value < minimum ? minimum : (value > maximum ? maximum : value));
    const double span = (double)maximum - (double)minimum;
    const bool changed =
        span <= INSPECTOR_SLIDER_SPAN && span > 0.0
            ? tb_ui_slider_int(ui, label, &shown, (int32_t)minimum, (int32_t)maximum)
            : tb_ui_drag_int(ui, label, &shown, (float)fmax(span / INSPECTOR_DRAG_STEPS, 0.1),
                             (int32_t)minimum, (int32_t)maximum);
    if (changed)
    {
        (void)tb_setting_set_integer(setting, shown);
    }
}

static void inspector_number(tb_ui *ui, const char *label, tb_setting *setting)
{
    double minimum = 0.0;
    double maximum = 0.0;
    float shown = (float)tb_setting_number(setting);

    tb_setting_number_range(setting, &minimum, &maximum);
    const double span = maximum - minimum;
    const bool changed =
        isfinite(span) && span > 0.0 && span <= INSPECTOR_SLIDER_SPAN
            ? tb_ui_slider_float(ui, label, &shown, (float)minimum, (float)maximum,
                                 span <= 10.0 ? "%.2f" : "%.1f")
            : tb_ui_drag_float(ui, label, &shown, 0.01f, (float)fmax(minimum, -1e9),
                               (float)fmin(maximum, 1e9), "%.2f");
    if (changed)
    {
        (void)tb_setting_set_number(setting, (double)shown);
    }
}

static void inspector_choice(tb_ui *ui, const char *label, tb_setting *setting)
{
    const char *names[INSPECTOR_MOST_CHOICES] = {0};
    size_t count = tb_setting_choice_count(setting);
    size_t chosen = tb_setting_choice(setting);

    count = count < INSPECTOR_MOST_CHOICES ? count : INSPECTOR_MOST_CHOICES;
    for (size_t index = 0; index < count; index++)
    {
        names[index] = tb_setting_choice_name(setting, index);
    }
    if (tb_ui_combo(ui, label, &chosen, names, count))
    {
        (void)tb_setting_set_choice(setting, chosen);
    }
}

static void inspector_text(sandbox_inspector *inspector, tb_ui *ui, const char *label,
                           tb_setting *setting, size_t index)
{
    char *const buffer = inspector->texts[index];
    const size_t capacity = tb_setting_text_limit(setting) + 1;

    /* The field shows the setting, unless it is being typed in. */
    if (inspector->text_versions[index] != tb_setting_version(setting))
    {
        (void)tb_setting_text(setting, buffer, capacity);
        inspector->text_versions[index] = tb_setting_version(setting);
    }
    if (tb_ui_text_field(ui, label, buffer, capacity) &&
        tb_setting_set_text(setting, buffer) == TB_SETTING_OK)
    {
        inspector->text_versions[index] = tb_setting_version(setting);
    }
}

static void inspector_setting(sandbox_inspector *inspector, tb_ui *ui, size_t index)
{
    tb_setting *const setting = tb_settings_at(inspector->settings, index);
    char label[96] = {0};

    inspector_label(setting, label, sizeof(label));
    tb_ui_push_id(ui, tb_setting_name(setting));
    switch (tb_setting_type_of(setting))
    {
    case TB_SETTING_BOOLEAN: {
        bool value = tb_setting_boolean(setting);
        if (tb_ui_checkbox(ui, label, &value))
        {
            (void)tb_setting_set_boolean(setting, value);
        }
        break;
    }
    case TB_SETTING_INTEGER:
        inspector_integer(ui, label, setting);
        break;
    case TB_SETTING_NUMBER:
        inspector_number(ui, label, setting);
        break;
    case TB_SETTING_CHOICE:
        inspector_choice(ui, label, setting);
        break;
    case TB_SETTING_TEXT:
        inspector_text(inspector, ui, label, setting, index);
        break;
    }
    tb_ui_tooltip(ui, tb_setting_description(setting));
    tb_ui_pop_id(ui);
}

/* The overall quality, which sets every quality group, as a menu's does. */
static void inspector_quality(sandbox_inspector *inspector, tb_ui *ui)
{
    static const char *const levels[] = {"low", "medium", "high", "ultra", "custom"};
    size_t level = (size_t)tb_engine_settings_quality(inspector->engine);

    if (tb_ui_combo(ui, "Overall quality", &level, levels, 5) &&
        level != TB_GRAPHICS_QUALITY_CUSTOM)
    {
        tb_engine_settings_set_quality(inspector->engine, (tb_graphics_quality)level);
    }
    tb_ui_tooltip(ui, "Every quality group at one level; custom when they differ.");
}

static void inspector_family(sandbox_inspector *inspector, tb_ui *ui, size_t family)
{
    const tb_settings_family info = tb_settings_family_at(inspector->settings, family);
    const char *const title = info.title != NULL ? info.title : info.name;
    const bool open_first =
        tb_str_equal(info.name, "display") || tb_str_equal(info.name, "graphics");

    tb_ui_push_id(ui, info.name);
    if (tb_ui_header(ui, title, open_first))
    {
        if (tb_str_equal(info.name, "graphics"))
        {
            inspector_quality(inspector, ui);
        }
        for (size_t index = 0; index < inspector->setting_count; index++)
        {
            if (tb_setting_family(tb_settings_at(inspector->settings, index)) == family)
            {
                inspector_setting(inspector, ui, index);
            }
        }
        if (tb_ui_button(ui, "Restore defaults"))
        {
            tb_settings_restore_family(inspector->settings, info.name);
        }
        tb_ui_spacing(ui);
    }
    tb_ui_pop_id(ui);
}

static void inspector_settings(sandbox_inspector *inspector, tb_ui *ui, float height)
{
    if (!tb_ui_begin_window(ui, "Settings",
                            (tb_canvas_rect){INSPECTOR_MARGIN, INSPECTOR_MARGIN,
                                             INSPECTOR_SETTINGS_WIDTH,
                                             height - INSPECTOR_MARGIN - INSPECTOR_STATUS_ROOM},
                            0))
    {
        return;
    }
    for (size_t family = 0; family < tb_settings_family_count(inspector->settings); family++)
    {
        inspector_family(inspector, ui, family);
    }
    tb_ui_end_window(ui);
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

void sandbox_inspector_after_frame(sandbox_inspector *inspector, sandbox *box, double seconds)
{
    inspector->history[inspector->history_next] = (float)(seconds * 1000.0);
    inspector->history_next = (inspector->history_next + 1) % INSPECTOR_HISTORY;
    inspector->history_count += inspector->history_count < INSPECTOR_HISTORY ? 1 : 0;
    inspector->since_refresh += seconds;
    if (inspector->dump[0] != '\0' && inspector->since_refresh < INSPECTOR_REFRESH_SECONDS)
    {
        return;
    }
    inspector->since_refresh = 0.0;
    tb_canvas_stats_of(box->renderer, &inspector->canvas);
    (void)tb_render_graph_dump(tb_renderer_graph(box->renderer), inspector->dump,
                               INSPECTOR_DUMP_CAPACITY);
}

/* The frame times, oldest first, and their average and worst. */
static size_t inspector_times(const sandbox_inspector *inspector, float *times, float *average,
                              float *worst)
{
    const size_t count = inspector->history_count;
    const size_t first = (inspector->history_next + INSPECTOR_HISTORY - count) % INSPECTOR_HISTORY;
    float sum = 0.0f;

    *worst = 0.0f;
    for (size_t index = 0; index < count; index++)
    {
        times[index] = inspector->history[(first + index) % INSPECTOR_HISTORY];
        sum += times[index];
        *worst = fmaxf(*worst, times[index]);
    }
    *average = count > 0 ? sum / (float)count : 0.0f;
    return count;
}

/* The render graph's description, a line of monospace text a line. */
static void inspector_dump(const sandbox_inspector *inspector, tb_ui *ui)
{
    const char *const dump = inspector->dump;
    const size_t total = tb_str_length(dump);
    size_t start = 0;

    while (start < total)
    {
        const size_t found = tb_str_find_char(dump, start, TB_STR_END, '\n');
        const size_t stop = found == TB_STR_NOT_FOUND ? total : found;
        const size_t length = stop - start;
        char text[256] = {0};
        memcpy(text, dump + start, length < sizeof(text) - 1 ? length : sizeof(text) - 1);
        tb_ui_text_mono(ui, text);
        start = stop + 1;
    }
}

static void inspector_frame(sandbox_inspector *inspector, sandbox *box, tb_ui *ui,
                            const char *scene, float width)
{
    float times[INSPECTOR_HISTORY] = {0};
    float average = 0.0f;
    float worst = 0.0f;
    const size_t count = inspector_times(inspector, times, &average, &worst);
    char line[160] = {0};

    if (!tb_ui_begin_window(ui, "Frame",
                            (tb_canvas_rect){width - INSPECTOR_MARGIN - INSPECTOR_FRAME_WIDTH,
                                             INSPECTOR_MARGIN, INSPECTOR_FRAME_WIDTH,
                                             INSPECTOR_FRAME_HEIGHT},
                            0))
    {
        return;
    }
    tb_ui_textf(ui, "%.2f ms a frame on average, %.2f ms at worst", (double)average, (double)worst);
    (void)tb_fmt_buffer(line, sizeof(line), "Frame times, %zu frames", count);
    tb_ui_plot(ui, line, times, count, 0.0f, fmaxf(worst * 1.25f, 1.0f), 0.0f);
    (void)tb_fmt_buffer(line, sizeof(line), "%.0f",
                        average > 0.0f ? 1000.0 / (double)average : 0.0);
    tb_ui_value(ui, "Frames a second", line);
    tb_ui_value(ui, "Scene", scene);
    tb_ui_value(ui, "Backend", tb_rhi_backend_name(box->info.backend));
    tb_ui_value(ui, "Adapter", box->info.adapter);
    if (tb_ui_header(ui, "Canvas", true))
    {
        tb_ui_textf(ui, "%zu quads in %u draws, %zu of them glyphs, %zu dropped",
                    inspector->canvas.quads, (unsigned)inspector->canvas.draws,
                    inspector->canvas.glyphs, inspector->canvas.dropped);
    }
    if (tb_ui_header(ui, "Render graph", true))
    {
        tb_ui_begin_panel(ui, "Passes", 0.0f);
        inspector_dump(inspector, ui);
        tb_ui_end_panel(ui);
    }
    tb_ui_end_window(ui);
}

void sandbox_inspector_draw(sandbox_inspector *inspector, sandbox *box, const char *scene,
                            double seconds)
{
    tb_ui *const ui = inspector->ui;
    uint32_t width = 0;
    uint32_t height = 0;

    if (ui == NULL)
    {
        return;
    }
    tb_renderer_size(box->renderer, &width, &height);
    inspector->input.scale = tb_canvas_view_logical(box->renderer).scale * box->ui_scale;
    inspector->input.size = (tb_vec2){(float)width, (float)height};
    inspector->input.delta_seconds = seconds;
    tb_ui_begin_frame(ui, &inspector->input);
    if (inspector->shown)
    {
        inspector_settings(inspector, ui, (float)height / inspector->input.scale);
        inspector_frame(inspector, box, ui, scene, (float)width / inspector->input.scale);
    }
    tb_ui_end_frame(ui);
    tb_ui_input_next_frame(&inspector->input);
    tb_ui_draw(ui, box->renderer);
    if (inspector->shown && tb_ui_wants_mouse(ui))
    {
        tb_window_set_cursor(box->window, tb_ui_cursor(ui));
    }
}
