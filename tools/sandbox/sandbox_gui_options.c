/* The gui scene's options panel: a settings form of every widget that edits
 * a value, bound through variables to the engine's own settings, which the
 * sandbox applies as they change: the inspector (F1) shows the same values
 * moving. A slider and a spin box share the field of view, a slider and a
 * progress bar the volume. The rest show a variable of their own. */
#include "sandbox.h"

#include "badger/fmt.h"
#include "badger/gui.h"
#include "badger/memory.h"
#include "badger/settings.h"

#include <stddef.h>

/* Units between the panel's rows, and inside it. */
#define OPTIONS_PADDING 12.0f
#define OPTIONS_GAP 10.0f
#define OPTIONS_ROW_GAP 3.0f
/* Bytes of the status line. */
#define OPTIONS_STATUS_CAPACITY 96

struct gui_options
{
    tb_widget status;
    tb_var volume;
    char text[OPTIONS_STATUS_CAPACITY];
};

/* A variable bound to a setting, or left unbound when this build has none. */
static tb_var options_bound(tb_gui *gui, const tb_gui_value *initial, tb_setting *setting)
{
    const tb_var var = tb_var_create(gui, initial);

    if (setting != NULL)
    {
        (void)tb_var_bind_setting(var, setting);
    }
    return var;
}

static void options_on_volume(tb_var var, void *user)
{
    gui_options *const options = user;

    (void)tb_fmt_buffer(options->text, sizeof(options->text), "Master volume: %d%%.",
                        (int)(tb_var_get_float(var) * 100.0 + 0.5));
    (void)tb_widget_set_text(options->status, options->text);
}

static void options_on_changed(const tb_gui_event *event, void *user)
{
    gui_options *const options = user;
    const tb_text kind = tb_text_from_cstr(tb_widget_class_of(event->widget)->name);

    (void)tb_fmt_buffer(options->text, sizeof(options->text), "Changed by a %.*s.",
                        (int)kind.length, kind.bytes);
    (void)tb_widget_set_text(options->status, options->text);
}

/* A row's label, on the right of the first column. */
static void options_label(tb_widget form, int32_t row, const char *text)
{
    const tb_widget label = tb_label(form, &(tb_label_desc){.text = text});

    (void)tb_grid(label, &(tb_grid_options){.row = row,
                                            .sticky = TB_STICKY_E,
                                            .padx = {0.0f, OPTIONS_GAP},
                                            .pady = {OPTIONS_ROW_GAP, OPTIONS_ROW_GAP}});
}

/* A row's widget, in the second column; told of its changes. */
static void options_place(gui_options *options, tb_widget widget, int32_t row, uint32_t sticky)
{
    (void)tb_grid(widget, &(tb_grid_options){.row = row,
                                             .column = 1,
                                             .sticky = sticky,
                                             .pady = {OPTIONS_ROW_GAP, OPTIONS_ROW_GAP}});
    (void)tb_bind(widget, TB_GUI_EVENT_CHANGED, options_on_changed, options);
}

/* Refuse digits in a name, as Tk's `validatecommand` would. */
static void options_validate_name(const tb_gui_event *event, void *user)
{
    (void)user;
    for (const char *at = event->text; *at != '\0'; at++)
    {
        if (*at >= '0' && *at <= '9')
        {
            tb_gui_refuse_edit(event->widget);
            return;
        }
    }
}

static void options_build_graphics(gui_options *options, tb_widget form,
                                   const tb_engine_settings *engine)
{
    tb_gui *const gui = form.gui;
    static const char *const smoothing[] = {"off", "fxaa", "taa"};
    const tb_gui_value nothing = tb_gui_text(TB_TEXT(""));
    const tb_gui_value ninety = tb_gui_float(90.0f);
    const tb_gui_value none = tb_gui_int(0);
    const tb_var anti_aliasing = options_bound(gui, &nothing, engine->graphics.anti_aliasing);
    const tb_var field_of_view = options_bound(gui, &ninety, engine->graphics.field_of_view);
    const tb_var frame_limit = options_bound(gui, &none, engine->display.frame_limit);

    options_label(form, 1, "Anti-aliasing");
    options_place(options,
                  tb_combobox(form, &(tb_combobox_desc){.values = smoothing,
                                                        .value_count = 3,
                                                        .variable = anti_aliasing}),
                  1, TB_STICKY_W);

    options_label(form, 2, "Field of view");
    const tb_widget view = tb_frame(form, NULL);
    (void)tb_grid(view, &(tb_grid_options){.row = 2, .column = 1, .sticky = TB_STICKY_EW});
    const tb_widget slider = tb_scale(view, &(tb_scale_desc){.variable = field_of_view,
                                                             .from = 60.0f,
                                                             .to = 120.0f,
                                                             .resolution = 1.0f,
                                                             .show_value = true,
                                                             .format = "%.0f\xC2\xB0"});
    const tb_widget spin = tb_spinbox(
        view,
        &(tb_spinbox_desc){
            .variable = field_of_view, .from = 60.0f, .to = 120.0f, .increment = 5.0f, .width = 4});
    (void)tb_grid(slider, &(tb_grid_options){.sticky = TB_STICKY_EW});
    (void)tb_grid(
        spin, &(tb_grid_options){.column = 1, .sticky = TB_STICKY_S, .padx = {OPTIONS_GAP, 0.0f}});
    (void)tb_grid_column_configure(view, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_bind(slider, TB_GUI_EVENT_CHANGED, options_on_changed, options);
    (void)tb_bind(spin, TB_GUI_EVENT_CHANGED, options_on_changed, options);

    options_label(form, 3, "Frame limit");
    options_place(options,
                  tb_spinbox(form, &(tb_spinbox_desc){.variable = frame_limit,
                                                      .from = 0.0f,
                                                      .to = 360.0f,
                                                      .increment = 30.0f,
                                                      .width = 5}),
                  3, TB_STICKY_W);
}

static void options_build_audio(gui_options *options, tb_widget form,
                                const tb_engine_settings *engine)
{
    tb_gui *const gui = form.gui;
    static const char *const subtitles[] = {"off", "dialogue", "captions"};
    static const char *const subtitle_names[] = {"Off", "Dialogue", "Captions"};
    const tb_gui_value full = tb_gui_float(1.0f);
    const tb_gui_value nothing = tb_gui_text(TB_TEXT(""));

    options->volume = options_bound(gui, &full, engine->audio.master_volume);
    (void)tb_var_trace(options->volume, options_on_volume, options);
    options_label(form, 4, "Master volume");
    const tb_widget volume = tb_frame(form, NULL);
    (void)tb_grid(volume, &(tb_grid_options){.row = 4, .column = 1, .sticky = TB_STICKY_EW});
    const tb_widget slider = tb_scale(volume, &(tb_scale_desc){.variable = options->volume,
                                                               .from = 0.0f,
                                                               .to = 1.0f,
                                                               .resolution = 0.01f,
                                                               .length = 120.0f});
    const tb_widget meter = tb_progressbar(
        volume,
        &(tb_progressbar_desc){.variable = options->volume, .maximum = 1.0f, .length = 80.0f});
    (void)tb_grid(slider, &(tb_grid_options){.sticky = TB_STICKY_EW});
    (void)tb_grid(meter, &(tb_grid_options){.column = 1, .padx = {OPTIONS_GAP, 0.0f}});
    (void)tb_grid_column_configure(volume, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_bind(slider, TB_GUI_EVENT_CHANGED, options_on_changed, options);

    const tb_var subtitle = options_bound(gui, &nothing, engine->accessibility.subtitles);
    options_label(form, 5, "Subtitles");
    const tb_widget group = tb_frame(form, NULL);
    (void)tb_grid(group, &(tb_grid_options){.row = 5, .column = 1, .sticky = TB_STICKY_W});
    for (size_t index = 0; index < 3; index++)
    {
        const tb_widget radio =
            tb_radiobutton(group, &(tb_radiobutton_desc){.text = subtitle_names[index],
                                                         .variable = subtitle,
                                                         .value = subtitles[index]});
        (void)tb_grid(radio, &(tb_grid_options){.column = (int32_t)index,
                                                .padx = {0.0f, OPTIONS_GAP},
                                                .pady = {OPTIONS_ROW_GAP, OPTIONS_ROW_GAP}});
        (void)tb_bind(radio, TB_GUI_EVENT_CHANGED, options_on_changed, options);
    }

    const tb_gui_value on = tb_gui_bool(true);
    const tb_gui_value off = tb_gui_bool(false);
    const tb_var crosshair = options_bound(gui, &on, engine->user_interface.crosshair);
    const tb_var invert = options_bound(gui, &off, engine->controls.mouse_invert_y);
    const tb_widget checks = tb_frame(form, NULL);
    (void)tb_grid(checks, &(tb_grid_options){.row = 6, .column = 1, .sticky = TB_STICKY_W});
    const tb_widget show =
        tb_checkbutton(checks, &(tb_checkbutton_desc){.text = "Crosshair", .variable = crosshair});
    const tb_widget look = tb_checkbutton(
        checks, &(tb_checkbutton_desc){.text = "Invert the mouse", .variable = invert});
    (void)tb_grid(show, &(tb_grid_options){.pady = {OPTIONS_ROW_GAP, OPTIONS_ROW_GAP}});
    (void)tb_grid(look, &(tb_grid_options){.column = 1,
                                           .padx = {OPTIONS_GAP, 0.0f},
                                           .pady = {OPTIONS_ROW_GAP, OPTIONS_ROW_GAP}});
    (void)tb_bind(show, TB_GUI_EVENT_CHANGED, options_on_changed, options);
    (void)tb_bind(look, TB_GUI_EVENT_CHANGED, options_on_changed, options);
}

static void options_build_text(gui_options *options, tb_widget form)
{
    options_label(form, 7, "Name");
    options_place(options,
                  tb_entry(form, &(tb_entry_desc){.placeholder = "No digits",
                                                  .max_length = 16,
                                                  .validate = options_validate_name}),
                  7, TB_STICKY_EW);
    options_label(form, 8, "Password");
    options_place(options, tb_entry(form, &(tb_entry_desc){.show = "\xE2\x80\xA2"}), 8,
                  TB_STICKY_EW);
    options_label(form, 9, "Notes");
    options_place(
        options,
        tb_text_box(form, &(tb_text_box_desc){.text = "Several lines, wrapped at the box's width, "
                                                      "scrolled with the wheel.",
                                              .width = 24,
                                              .height = 2}),
        9, TB_STICKY_EW);
    options_label(form, 10, "Loading");
    const tb_widget busy =
        tb_progressbar(form, &(tb_progressbar_desc){.mode = TB_PROGRESS_INDETERMINATE});
    (void)tb_grid(busy, &(tb_grid_options){.row = 10, .column = 1, .sticky = TB_STICKY_EW});
}

gui_options *gui_options_create(tb_widget parent, int32_t column, const tb_engine_settings *engine)
{
    gui_options *const options = tb_calloc(1, sizeof(*options));
    const tb_widget form = tb_frame(
        parent, &(tb_frame_desc){.padding = OPTIONS_PADDING, .background = true, .border = true});

    /* At the top right, as large as it wants. */
    (void)tb_grid(form, &(tb_grid_options){.column = column,
                                           .sticky = TB_STICKY_N | TB_STICKY_E,
                                           .padx = {OPTIONS_GAP, OPTIONS_PADDING},
                                           .pady = {OPTIONS_PADDING, 0.0f}});
    const tb_widget title = tb_label(form, &(tb_label_desc){.text = "Options"});
    (void)tb_grid(title, &(tb_grid_options){.column_span = 2, .sticky = TB_STICKY_W});
    options_build_graphics(options, form, engine);
    options_build_audio(options, form, engine);
    options_build_text(options, form);
    options->status = tb_label(form, &(tb_label_desc){.text = "Bound to the engine's settings."});
    (void)tb_grid(options->status, &(tb_grid_options){.row = 11,
                                                      .column_span = 2,
                                                      .sticky = TB_STICKY_W,
                                                      .pady = {OPTIONS_ROW_GAP, 0.0f}});
    (void)tb_grid_column_configure(form, 1, &(tb_grid_track){.weight = 1.0f});
    return options;
}

void gui_options_destroy(gui_options *options)
{
    tb_free(options);
}
