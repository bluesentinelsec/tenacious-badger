/* Sliders and progress bars: a value within a range, dragged and stepped,
 * or shown filling a trough, across or down, in a variable or its own. */
#include "gui_values.h"

#include "badger/assert.h"

#include <math.h>
#include <stddef.h>

/* A track's length unless chosen, in units; a range unless chosen. */
#define TB_GUI_RANGE_LENGTH 160.0f
#define TB_GUI_RANGE_DEFAULT_TO 100.0f
/* A slider's handle, and its track, in lines of text. */
#define TB_GUI_HANDLE_LINES 0.8f
#define TB_GUI_TRACK_LINES 0.25f
/* A progress bar's thickness, in lines of text. */
#define TB_GUI_PROGRESS_LINES 0.6f
/* Steps of a range a key takes without a resolution, and Page Up's without
 * a big increment. */
#define TB_GUI_RANGE_STEPS 100.0
#define TB_GUI_RANGE_PAGES 10.0
/* An indeterminate bar's block, in lengths, and its seconds to and fro. */
#define TB_GUI_BLOCK_LENGTH 0.25f
#define TB_GUI_BLOCK_PERIOD 2.0

static const char *const tb_gui_orient_names[] = {"horizontal", "vertical"};
static const char *const tb_gui_mode_names[] = {"determinate", "indeterminate"};

/* A value's fraction of a range from `from` to `to`, within 0 and 1. */
static double tb_gui_fraction(double value, double from, double to)
{
    if (to == from)
    {
        return 0.0;
    }
    return fmin(fmax((value - from) / (to - from), 0.0), 1.0);
}

/* A property of a number at an offset that refuses less than 0. */
static bool tb_gui_range_set_positive(void *data, size_t offset, const tb_gui_value *value)
{
    if (value->number < 0.0f)
    {
        return false;
    }
    *(float *)(void *)((char *)data + offset) = value->number;
    return true;
}

/* ---------------------------------------------------------------------------
 * Sliders.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_scale
{
    tb_var variable;
    float from;
    float to;
    float resolution;
    float big_increment;
    float value;
    float length;
    int32_t orient;
    bool show_value;
    tb_gui_string format;
    tb_gui_command command;
    bool dragging;
} tb_gui_scale;

/* Where a slider's parts are. */
typedef struct tb_gui_scale_parts
{
    tb_canvas_rect track; /* the strip the handle moves along */
    float start;          /* the handle's middle at `from`, along the axis */
    float travel;         /* how far it moves to `to` */
    float handle;         /* its diameter */
    float label;          /* the value's room: its height across, or its width down */
} tb_gui_scale_parts;

static double tb_gui_scale_value(const tb_gui_scale *scale)
{
    return tb_var_alive(scale->variable) ? tb_var_get_float(scale->variable) : (double)scale->value;
}

/* A value rounded to the resolution, from `from`, and kept within the range. */
static double tb_gui_scale_round(const tb_gui_scale *scale, double value)
{
    const double from = (double)scale->from;
    const double to = (double)scale->to;
    const double resolution = (double)scale->resolution;

    if (resolution > 0.0)
    {
        value = from + floor((value - from) / resolution + 0.5) * resolution;
    }
    return fmin(fmax(value, fmin(from, to)), fmax(from, to));
}

static tb_text tb_gui_scale_written(const tb_gui_scale *scale, double value, char *buffer)
{
    const double step = scale->resolution > 0.0f
                            ? (double)scale->resolution
                            : fabs((double)scale->to - (double)scale->from) / TB_GUI_RANGE_STEPS;

    return tb_gui_format_number(tb_gui_string_text(&scale->format), value, tb_gui_step_digits(step),
                                buffer);
}

/* A slider's style: its box is its handle's. */
enum
{
    TB_GUI_SCALE_TROUGH,  /* the track */
    TB_GUI_SCALE_FILL,    /* the track up to the handle */
    TB_GUI_SCALE_SPACING, /* units between the track and the value written */
    TB_GUI_SCALE_ELEMENTS
};

static const tb_style_element tb_gui_scale_elements[TB_GUI_SCALE_ELEMENTS] = {
    [TB_GUI_SCALE_TROUGH] = {"trough_color", TB_STYLE_COLOR},
    [TB_GUI_SCALE_FILL] = {"fill_color", TB_STYLE_COLOR},
    [TB_GUI_SCALE_SPACING] = {"spacing", TB_STYLE_NUMBER}};

static const tb_widget_style_desc tb_gui_scale_style = {.name = "Scale",
                                                        .states = TB_GUI_VALUES_STATES,
                                                        .elements = tb_gui_scale_elements,
                                                        .element_count = TB_GUI_SCALE_ELEMENTS};

/* The widest its value is written, at either end. */
static tb_vec2 tb_gui_scale_label_size(tb_widget widget, const tb_gui_scale *scale)
{
    char buffer[TB_GUI_NUMBER_CAPACITY] = {0};
    tb_gui_text_block block = {.text = tb_gui_scale_written(scale, (double)scale->from, buffer)};
    const tb_vec2 from = tb_widget_measure_text(widget, &block);

    block.text = tb_gui_scale_written(scale, (double)scale->to, buffer);
    const tb_vec2 to = tb_widget_measure_text(widget, &block);
    return (tb_vec2){fmaxf(from.x, to.x),
                     fmaxf(fmaxf(from.y, to.y), tb_gui_values_measures(widget).line_height)};
}

static tb_gui_scale_parts tb_gui_scale_parts_of(tb_widget widget, const tb_gui_scale *scale,
                                                tb_canvas_rect rect)
{
    const float line = tb_gui_values_measures(widget).line_height;
    const float gap = tb_style_number(tb_style_in(widget, 0), TB_GUI_SCALE_SPACING);
    tb_gui_scale_parts parts = {.handle = line * TB_GUI_HANDLE_LINES};

    if (scale->orient == TB_GUI_ORIENT_HORIZONTAL)
    {
        parts.label = scale->show_value ? tb_gui_scale_label_size(widget, scale).y : 0.0f;
        parts.track = (tb_canvas_rect){rect.x, rect.y + parts.label, rect.width, line};
        parts.start = parts.track.x + parts.handle * 0.5f;
        parts.travel = fmaxf(parts.track.width - parts.handle, 0.0f);
        return parts;
    }
    parts.label = scale->show_value ? tb_gui_scale_label_size(widget, scale).x + gap : 0.0f;
    parts.track = (tb_canvas_rect){rect.x, rect.y, line, rect.height};
    parts.start = parts.track.y + parts.handle * 0.5f;
    parts.travel = fmaxf(parts.track.height - parts.handle, 0.0f);
    return parts;
}

static tb_vec2 tb_gui_scale_measure(tb_widget widget, const void *data)
{
    const tb_gui_scale *const scale = data;
    const tb_gui_scale_parts parts = tb_gui_scale_parts_of(widget, scale, (tb_canvas_rect){0});
    const float line = tb_gui_values_measures(widget).line_height;

    if (scale->orient == TB_GUI_ORIENT_HORIZONTAL)
    {
        return (tb_vec2){scale->length, parts.label + line};
    }
    return (tb_vec2){line + parts.label, scale->length};
}

/* The handle's middle for a value, along the axis. */
static float tb_gui_scale_position(const tb_gui_scale *scale, const tb_gui_scale_parts *parts,
                                   double value)
{
    const double fraction = tb_gui_fraction(value, (double)scale->from, (double)scale->to);
    return parts->start + (float)fraction * parts->travel;
}

static void tb_gui_scale_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_scale *const scale = data;
    const tb_style *const style = tb_style_of(widget);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_gui_scale_parts parts = tb_gui_scale_parts_of(widget, scale, rect);
    const double value = tb_gui_scale_value(scale);
    const float at = tb_gui_scale_position(scale, &parts, value);
    const bool across = scale->orient == TB_GUI_ORIENT_HORIZONTAL;
    const float thin = parts.track.height * TB_GUI_TRACK_LINES;
    const float middle = across ? parts.track.y + parts.track.height * 0.5f
                                : parts.track.x + parts.track.width * 0.5f;
    const float half = parts.handle * 0.5f;
    tb_canvas_rect trough = {parts.track.x, middle - thin * 0.5f, parts.track.width, thin};
    tb_canvas_rect filled = {parts.track.x, trough.y, at - parts.track.x, thin};
    tb_canvas_rect handle = {at - half, middle - half, parts.handle, parts.handle};
    char buffer[TB_GUI_NUMBER_CAPACITY] = {0};

    if (!across)
    {
        trough = (tb_canvas_rect){middle - thin * 0.5f, parts.track.y, thin, parts.track.height};
        filled = (tb_canvas_rect){trough.x, parts.track.y, thin, at - parts.track.y};
        handle = (tb_canvas_rect){middle - half, at - half, parts.handle, parts.handle};
    }
    tb_style round = *style;

    tb_gui_paint_fill(paint, trough, thin * 0.5f, tb_style_color(style, TB_GUI_SCALE_TROUGH));
    tb_gui_paint_fill(paint, filled, thin * 0.5f, tb_style_color(style, TB_GUI_SCALE_FILL));
    /* The handle is round, whatever its style's corners. */
    round.radius = half;
    tb_gui_paint_box(paint, handle, &round);
    if (!scale->show_value)
    {
        return;
    }
    const tb_gui_text_block block = {.text = tb_gui_scale_written(scale, value, buffer),
                                     .anchor = across ? TB_GUI_ANCHOR_S : TB_GUI_ANCHOR_W};
    const tb_vec2 size = tb_gui_scale_label_size(widget, scale);
    tb_canvas_rect label = across
                               ? (tb_canvas_rect){at - size.x * 0.5f, rect.y, size.x, parts.label}
                               : (tb_canvas_rect){parts.track.x + parts.track.width +
                                                      tb_style_number(style, TB_GUI_SCALE_SPACING),
                                                  at - size.y * 0.5f, size.x, size.y};
    /* Over the handle, but kept inside the slider at its ends, where half
     * of it would be cut off. */
    if (across)
    {
        label.x = fmaxf(rect.x, fminf(label.x, rect.x + rect.width - size.x));
    }
    else
    {
        label.y = fmaxf(rect.y, fminf(label.y, rect.y + rect.height - size.y));
    }
    tb_gui_paint_text(paint, &block, label, style->text_color);
}

/* Set a slider's value from input: rounded, kept within its range. */
static void tb_gui_scale_take(tb_widget widget, tb_gui_scale *scale, double value)
{
    const double before = tb_gui_scale_value(scale);
    const double rounded = tb_gui_scale_round(scale, value);

    if (tb_var_alive(scale->variable))
    {
        (void)tb_var_set_float(scale->variable, rounded);
    }
    else
    {
        scale->value = (float)rounded;
    }
    if (tb_gui_scale_value(scale) != before)
    {
        tb_gui_values_changed(widget, scale->command);
    }
}

/* The value at the mouse. */
static void tb_gui_scale_point(tb_widget widget, tb_gui_scale *scale, tb_vec2 point)
{
    const tb_gui_scale_parts parts = tb_gui_scale_parts_of(widget, scale, tb_widget_rect(widget));
    const float along = scale->orient == TB_GUI_ORIENT_HORIZONTAL ? point.x : point.y;
    const double fraction =
        parts.travel > 0.0f ? fmin(fmax((double)((along - parts.start) / parts.travel), 0.0), 1.0)
                            : 0.0;

    tb_gui_scale_take(widget, scale,
                      (double)scale->from + fraction * ((double)scale->to - (double)scale->from));
}

/* A key's step: toward `from` for Left or Up along it, and Page Up; toward
 * `to` for Right or Down along it, and Page Down. 0 for a key that steps
 * nothing: the arrows across it are left to navigation. */
static double tb_gui_scale_key_step(const tb_gui_scale *scale, tb_key key)
{
    const bool across = scale->orient == TB_GUI_ORIENT_HORIZONTAL;
    const tb_key back = across ? TB_KEY_LEFT : TB_KEY_UP;
    const tb_key ahead = across ? TB_KEY_RIGHT : TB_KEY_DOWN;
    const double range = (double)scale->to - (double)scale->from;
    const double small =
        scale->resolution > 0.0f ? (double)scale->resolution : fabs(range) / TB_GUI_RANGE_STEPS;
    const double big = scale->big_increment > 0.0f ? (double)scale->big_increment
                                                   : fabs(range) / TB_GUI_RANGE_PAGES;
    const double toward_to = range < 0.0 ? -1.0 : 1.0;

    if (key == back)
    {
        return -small * toward_to;
    }
    if (key == ahead)
    {
        return small * toward_to;
    }
    if (key == TB_KEY_PAGE_UP)
    {
        return -big * toward_to;
    }
    return key == TB_KEY_PAGE_DOWN ? big * toward_to : 0.0;
}

static bool tb_gui_scale_key(tb_widget widget, tb_gui_scale *scale, const tb_gui_event *event)
{
    const double step = tb_gui_scale_key_step(scale, event->key);

    if (!tb_gui_values_plain(event))
    {
        return false;
    }
    if (event->key == TB_KEY_HOME || event->key == TB_KEY_END)
    {
        tb_gui_scale_take(widget, scale,
                          event->key == TB_KEY_HOME ? (double)scale->from : (double)scale->to);
        return true;
    }
    if (step == 0.0)
    {
        return false;
    }
    tb_gui_scale_take(widget, scale, tb_gui_scale_value(scale) + step);
    return true;
}

static bool tb_gui_scale_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_scale *const scale = data;

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS:
        if (event->button == TB_MOUSE_BUTTON_LEFT)
        {
            scale->dragging = true;
            tb_gui_scale_point(widget, scale, event->position);
        }
        return true;
    case TB_GUI_EVENT_MOTION:
        if (scale->dragging)
        {
            tb_gui_scale_point(widget, scale, event->position);
        }
        return true;
    case TB_GUI_EVENT_RELEASE:
        scale->dragging = false;
        return true;
    case TB_GUI_EVENT_KEY:
        return tb_gui_scale_key(widget, scale, event);
    case TB_GUI_EVENT_FOCUS_OUT:
        scale->dragging = false;
        return false;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_LEAVE:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_WHEEL:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_BACK:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static bool tb_gui_scale_get_value(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    *value = tb_gui_float((float)tb_gui_scale_value(data));
    return true;
}

static bool tb_gui_scale_set_value(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scale *const scale = data;
    const double rounded = tb_gui_scale_round(scale, (double)value->number);

    (void)widget;
    if (tb_var_alive(scale->variable) && !tb_var_set_float(scale->variable, rounded))
    {
        return false;
    }
    scale->value = (float)rounded;
    return true;
}

static bool tb_gui_scale_get_variable(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_scale *const scale = data;

    (void)widget;
    *value = tb_gui_variable(scale->variable);
    return true;
}

static bool tb_gui_scale_set_variable(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scale *const scale = data;

    if (!tb_gui_var_of_widget(widget, value->variable))
    {
        return false;
    }
    scale->variable = value->variable;
    return true;
}

static bool tb_gui_scale_set_resolution(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)widget;
    return tb_gui_range_set_positive(data, offsetof(tb_gui_scale, resolution), value);
}

static bool tb_gui_scale_set_big(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)widget;
    return tb_gui_range_set_positive(data, offsetof(tb_gui_scale, big_increment), value);
}

static bool tb_gui_scale_set_length(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)widget;
    return tb_gui_range_set_positive(data, offsetof(tb_gui_scale, length), value);
}

static bool tb_gui_scale_get_float(const void *data, size_t offset, tb_gui_value *value)
{
    *value = tb_gui_float(*(const float *)(const void *)((const char *)data + offset));
    return true;
}

static bool tb_gui_scale_get_resolution(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    return tb_gui_scale_get_float(data, offsetof(tb_gui_scale, resolution), value);
}

static bool tb_gui_scale_get_big(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    return tb_gui_scale_get_float(data, offsetof(tb_gui_scale, big_increment), value);
}

static bool tb_gui_scale_get_length(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    return tb_gui_scale_get_float(data, offsetof(tb_gui_scale, length), value);
}

static bool tb_gui_scale_get_format(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_scale *const scale = data;

    (void)widget;
    *value = tb_gui_text(tb_gui_string_text(&scale->format));
    return true;
}

static bool tb_gui_scale_set_format(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_scale *const scale = data;

    (void)widget;
    if (!tb_gui_format_valid(value->text) || value->text.length >= TB_GUI_NUMBER_CAPACITY)
    {
        return false;
    }
    tb_gui_string_set(&scale->format, value->text);
    return true;
}

static void tb_gui_scale_destroy(tb_widget widget, void *data)
{
    tb_gui_scale *const scale = data;

    (void)widget;
    tb_gui_string_free(&scale->format);
}

static void tb_gui_scale_init(tb_widget widget, void *data)
{
    tb_gui_scale *const scale = data;

    (void)widget;
    scale->to = TB_GUI_RANGE_DEFAULT_TO;
    scale->length = TB_GUI_RANGE_LENGTH;
}

static const tb_gui_property tb_gui_scale_properties[] = {
    {.name = "value",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_scale_get_value,
     .set = tb_gui_scale_set_value},
    {.name = "variable",
     .type = TB_GUI_TYPE_VARIABLE,
     .get = tb_gui_scale_get_variable,
     .set = tb_gui_scale_set_variable},
    {.name = "from",
     .type = TB_GUI_TYPE_FLOAT,
     .offset = offsetof(tb_gui_scale, from),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "to",
     .type = TB_GUI_TYPE_FLOAT,
     .offset = offsetof(tb_gui_scale, to),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "resolution",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_scale_get_resolution,
     .set = tb_gui_scale_set_resolution},
    {.name = "big_increment",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_scale_get_big,
     .set = tb_gui_scale_set_big},
    {.name = "orient",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_scale, orient),
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .names = tb_gui_orient_names,
     .name_count = TB_GUI_VALUES_COUNT_OF(tb_gui_orient_names)},
    {.name = "length",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_scale_get_length,
     .set = tb_gui_scale_set_length},
    {.name = "show_value",
     .type = TB_GUI_TYPE_BOOL,
     .offset = offsetof(tb_gui_scale, show_value),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "format",
     .type = TB_GUI_TYPE_TEXT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_scale_get_format,
     .set = tb_gui_scale_set_format},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_scale, command)},
};

static const tb_widget_class tb_gui_scale_kind = {
    .name = "scale",
    .data_size = sizeof(tb_gui_scale),
    .properties = tb_gui_scale_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_scale_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_scale_init,
    .destroy = tb_gui_scale_destroy,
    .measure = tb_gui_scale_measure,
    .draw = tb_gui_scale_draw,
    .event = tb_gui_scale_event,
    .style = &tb_gui_scale_style};

const tb_widget_class *tb_scale_class(void)
{
    return &tb_gui_scale_kind;
}

tb_widget tb_scale(tb_widget parent, const tb_scale_desc *desc)
{
    const tb_scale_desc chosen = desc != NULL ? *desc : (tb_scale_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_scale_kind);
    const bool default_range = chosen.from == 0.0f && chosen.to == 0.0f;
    const tb_gui_value values[] = {
        tb_gui_float(chosen.from),
        tb_gui_float(default_range ? TB_GUI_RANGE_DEFAULT_TO : chosen.to),
        tb_gui_float(chosen.resolution),
        tb_gui_float(chosen.big_increment),
        tb_gui_enum((int32_t)chosen.orient),
        tb_gui_float(chosen.length > 0.0f ? chosen.length : TB_GUI_RANGE_LENGTH),
        tb_gui_bool(chosen.show_value),
        tb_gui_text(tb_text_from_cstr(chosen.format != NULL ? chosen.format : "")),
        tb_gui_command_of(chosen.command, chosen.user),
    };
    static const char *const names[] = {"from",          "to",     "resolution",
                                        "big_increment", "orient", "length",
                                        "show_value",    "format", "command"};
    const tb_gui_value variable = tb_gui_variable(chosen.variable);
    const tb_gui_value value = tb_gui_float(chosen.value);

    for (size_t index = 0; index < TB_GUI_VALUES_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    if (tb_var_alive(chosen.variable))
    {
        (void)tb_widget_set(widget, "variable", &variable);
    }
    else
    {
        (void)tb_widget_set(widget, "value", &value);
    }
    return widget;
}

/* ---------------------------------------------------------------------------
 * Progress bars.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_progress
{
    tb_var variable;
    float maximum;
    float value;
    int32_t mode;
    int32_t orient;
    float length;
} tb_gui_progress;

static double tb_gui_progress_value(const tb_gui_progress *bar)
{
    return tb_var_alive(bar->variable) ? tb_var_get_float(bar->variable) : (double)bar->value;
}

/* A progress bar's style: its box is its trough's. */
enum
{
    TB_GUI_PROGRESS_FILL,       /* the part done */
    TB_GUI_PROGRESS_FILL_IMAGE, /* drawn over the part done in place of its color */
    TB_GUI_PROGRESS_ELEMENTS
};

static const tb_style_element tb_gui_progress_elements[TB_GUI_PROGRESS_ELEMENTS] = {
    [TB_GUI_PROGRESS_FILL] = {"fill_color", TB_STYLE_COLOR},
    [TB_GUI_PROGRESS_FILL_IMAGE] = {"fill_image", TB_STYLE_IMAGE}};

static const tb_widget_style_desc tb_gui_progress_style = {.name = "Progressbar",
                                                           .states = TB_WIDGET_DISABLED,
                                                           .elements = tb_gui_progress_elements,
                                                           .element_count =
                                                               TB_GUI_PROGRESS_ELEMENTS};

static tb_vec2 tb_gui_progress_measure(tb_widget widget, const void *data)
{
    const tb_gui_progress *const bar = data;
    const float thickness = tb_gui_values_measures(widget).line_height * TB_GUI_PROGRESS_LINES;

    if (bar->orient == TB_GUI_ORIENT_HORIZONTAL)
    {
        return (tb_vec2){bar->length, thickness};
    }
    return (tb_vec2){thickness, bar->length};
}

/* Where an indeterminate bar's block starts, in its lengths: to and fro. */
static float tb_gui_progress_block(double seconds)
{
    const double phase = fmod(seconds / TB_GUI_BLOCK_PERIOD, 1.0);
    const double there_and_back = 1.0 - fabs(2.0 * phase - 1.0);

    return (float)there_and_back * (1.0f - TB_GUI_BLOCK_LENGTH);
}

static void tb_gui_progress_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_progress *const bar = data;
    const tb_style *const style = tb_style_of(widget);
    const tb_style_image *const fill_image = tb_style_image_of(style, TB_GUI_PROGRESS_FILL_IMAGE);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const bool across = bar->orient == TB_GUI_ORIENT_HORIZONTAL;
    const float length = across ? rect.width : rect.height;
    float start = 0.0f;
    float extent = 0.0f;
    tb_canvas_rect filled = rect;
    tb_style trough = *style;

    /* The trough, then the part done, then the trough's border over both. */
    trough.border_width = 0.0f;
    tb_gui_paint_box(paint, rect, &trough);
    if (bar->mode == TB_PROGRESS_INDETERMINATE)
    {
        start = tb_gui_progress_block(widget.gui->time) * length;
        extent = TB_GUI_BLOCK_LENGTH * length;
    }
    else
    {
        extent =
            (float)tb_gui_fraction(tb_gui_progress_value(bar), 0.0, (double)bar->maximum) * length;
    }
    if (across)
    {
        filled.x += start;
        filled.width = extent;
    }
    else
    {
        filled.y += start;
        filled.height = extent;
    }
    if (extent > 0.0f && fill_image != NULL && fill_image->slice.image != NULL)
    {
        tb_gui_paint_nine_slice(paint, &fill_image->slice, filled, fill_image->tint);
    }
    else if (extent > 0.0f)
    {
        tb_gui_paint_fill(paint, filled, style->radius,
                          tb_style_color(style, TB_GUI_PROGRESS_FILL));
    }
    tb_gui_paint_outline(paint, rect, style->radius, style->border_width, style->border_color);
}

static bool tb_gui_progress_get_value(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)widget;
    *value = tb_gui_float((float)tb_gui_progress_value(data));
    return true;
}

static bool tb_gui_progress_set_value(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_progress *const bar = data;

    (void)widget;
    if (tb_var_alive(bar->variable) && !tb_var_set_float(bar->variable, (double)value->number))
    {
        return false;
    }
    bar->value = value->number;
    return true;
}

static bool tb_gui_progress_get_variable(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_progress *const bar = data;

    (void)widget;
    *value = tb_gui_variable(bar->variable);
    return true;
}

static bool tb_gui_progress_set_variable(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_progress *const bar = data;

    if (!tb_gui_var_of_widget(widget, value->variable))
    {
        return false;
    }
    bar->variable = value->variable;
    return true;
}

static bool tb_gui_progress_get_maximum(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_progress *const bar = data;

    (void)widget;
    *value = tb_gui_float(bar->maximum);
    return true;
}

static bool tb_gui_progress_set_maximum(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_progress *const bar = data;

    (void)widget;
    if (!(value->number > 0.0f))
    {
        return false;
    }
    bar->maximum = value->number;
    return true;
}

static bool tb_gui_progress_set_length(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)widget;
    return tb_gui_range_set_positive(data, offsetof(tb_gui_progress, length), value);
}

static bool tb_gui_progress_get_length(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_progress *const bar = data;

    (void)widget;
    *value = tb_gui_float(bar->length);
    return true;
}

static void tb_gui_progress_init(tb_widget widget, void *data)
{
    tb_gui_progress *const bar = data;

    (void)widget;
    bar->maximum = TB_GUI_RANGE_DEFAULT_TO;
    bar->length = TB_GUI_RANGE_LENGTH;
}

static const tb_gui_property tb_gui_progress_properties[] = {
    {.name = "value",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_progress_get_value,
     .set = tb_gui_progress_set_value},
    {.name = "variable",
     .type = TB_GUI_TYPE_VARIABLE,
     .get = tb_gui_progress_get_variable,
     .set = tb_gui_progress_set_variable},
    {.name = "maximum",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_progress_get_maximum,
     .set = tb_gui_progress_set_maximum},
    {.name = "mode",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_progress, mode),
     .names = tb_gui_mode_names,
     .name_count = TB_GUI_VALUES_COUNT_OF(tb_gui_mode_names)},
    {.name = "orient",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_progress, orient),
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .names = tb_gui_orient_names,
     .name_count = TB_GUI_VALUES_COUNT_OF(tb_gui_orient_names)},
    {.name = "length",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_progress_get_length,
     .set = tb_gui_progress_set_length},
};

static const tb_widget_class tb_gui_progress_kind = {
    .name = "progressbar",
    .data_size = sizeof(tb_gui_progress),
    .properties = tb_gui_progress_properties,
    .property_count = TB_GUI_VALUES_COUNT_OF(tb_gui_progress_properties),
    .mouse_filter = TB_GUI_MOUSE_PASS,
    .init = tb_gui_progress_init,
    .measure = tb_gui_progress_measure,
    .draw = tb_gui_progress_draw,
    .style = &tb_gui_progress_style};

const tb_widget_class *tb_progressbar_class(void)
{
    return &tb_gui_progress_kind;
}

tb_widget tb_progressbar(tb_widget parent, const tb_progressbar_desc *desc)
{
    const tb_progressbar_desc chosen = desc != NULL ? *desc : (tb_progressbar_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_progress_kind);
    const tb_gui_value maximum =
        tb_gui_float(chosen.maximum > 0.0f ? chosen.maximum : TB_GUI_RANGE_DEFAULT_TO);
    const tb_gui_value mode = tb_gui_enum((int32_t)chosen.mode);
    const tb_gui_value orient = tb_gui_enum((int32_t)chosen.orient);
    const tb_gui_value length =
        tb_gui_float(chosen.length > 0.0f ? chosen.length : TB_GUI_RANGE_LENGTH);
    const tb_gui_value variable = tb_gui_variable(chosen.variable);
    const tb_gui_value value = tb_gui_float(chosen.value);

    (void)tb_widget_set(widget, "maximum", &maximum);
    (void)tb_widget_set(widget, "mode", &mode);
    (void)tb_widget_set(widget, "orient", &orient);
    (void)tb_widget_set(widget, "length", &length);
    if (tb_var_alive(chosen.variable))
    {
        (void)tb_widget_set(widget, "variable", &variable);
    }
    else
    {
        (void)tb_widget_set(widget, "value", &value);
    }
    return widget;
}
