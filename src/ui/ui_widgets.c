/* The interface's widgets: text, buttons, checkboxes and radio buttons,
 * lists' items, sliders and dragged numbers, combos, headers and trees,
 * progress bars, colors, and tooltips. */
#include "ui_internal.h"

#include "badger/assert.h"
#include "badger/fmt.h"

#include <math.h>
#include <string.h>

/* Bytes of a value's text, or of text formatted, at most. */
#define TB_UI_VALUE_CAPACITY 64
#define TB_UI_FORMATTED_CAPACITY 512
/* Items a combo's list shows at once; it scrolls for more. */
#define TB_UI_LIST_ITEMS 8
/* Steps across a slider that Left and Right take. */
#define TB_UI_SLIDER_STEPS 100.0
/* How much faster Shift drags a number. */
#define TB_UI_DRAG_FAST 10.0
/* The widest a tooltip is before it wraps, and its place from the mouse, in units. */
#define TB_UI_TOOLTIP_WIDTH 320.0f
#define TB_UI_TOOLTIP_OFFSET_X 12.0f
#define TB_UI_TOOLTIP_OFFSET_Y 18.0f
/* A swatch's checkerboard: squares across it. */
#define TB_UI_SWATCH_CHECKS 4

static const tb_vec4 tb_ui_white = {1.0f, 1.0f, 1.0f, 1.0f};

static tb_vec4 tb_ui_state_color(const tb_ui *ui, const tb_ui_interaction *interaction,
                                 tb_ui_color normal, tb_ui_color hot, tb_ui_color active)
{
    if (interaction->active)
    {
        return tb_ui_color_of(ui, active);
    }
    return tb_ui_color_of(ui, interaction->hot ? hot : normal);
}

/* The ring around a widget that Tab focused. */
static void tb_ui_focus_ring(tb_ui *ui, tb_canvas_rect rect, float radius,
                             const tb_ui_interaction *interaction, tb_ui_id id)
{
    if (interaction->keys)
    {
        tb_ui_outline(ui, tb_ui_rect_inset(rect, -1.0f, -1.0f), radius + 1.0f, 1.5f,
                      tb_ui_color_of(ui, TB_UI_COLOR_ACCENT_HOT), id);
    }
}

/* Whether Space or Enter pressed a widget that Tab focused. */
static bool tb_ui_keys_press(tb_ui *ui, const tb_ui_interaction *interaction)
{
    return interaction->keys &&
           (tb_ui_key_pressed(ui, TB_KEY_SPACE) || tb_ui_key_pressed(ui, TB_KEY_ENTER));
}

/* A field's background: its fill, and its edge. */
static void tb_ui_field(tb_ui *ui, tb_canvas_rect rect, const tb_ui_interaction *interaction,
                        tb_ui_id id)
{
    const float radius = ui->theme.frame_rounding;

    tb_ui_fill(ui, rect, radius,
               tb_ui_state_color(ui, interaction, TB_UI_COLOR_FRAME, TB_UI_COLOR_FRAME_HOT,
                                 TB_UI_COLOR_FRAME_ACTIVE),
               id);
    tb_ui_outline(ui, rect, radius, ui->theme.border,
                  tb_ui_color_of(ui, interaction->hot || interaction->active
                                         ? TB_UI_COLOR_BUTTON_HOT
                                         : TB_UI_COLOR_BUTTON),
                  id);
    tb_ui_focus_ring(ui, rect, radius, interaction, id);
}

/* ---------------------------------------------------------------------------
 * Text.
 * ------------------------------------------------------------------------- */

static void tb_ui_text_item(tb_ui *ui, tb_ui_font font, tb_text text, tb_vec4 color)
{
    const tb_vec2 size = tb_ui_measure(ui, font, text, 0.0f);
    const bool lines = size.y > ui->fonts[font].line_height * 1.5f;
    const tb_canvas_rect item =
        tb_ui_next_item(ui, size.x, lines ? size.y : ui->frame_height, false);

    if (lines)
    {
        tb_ui_draw_text(ui, font, text, (tb_vec2){item.x, item.y}, color, 0.0f, tb_ui_clip(ui), 0);
    }
    else
    {
        tb_ui_text_in(ui, font, text, item, false, color, 0);
    }
    tb_ui_set_last(ui, 0, item, tb_ui_mouse_over(ui, item));
}

void tb_ui_text(tb_ui *ui, const char *text)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(text != NULL);
    tb_ui_text_item(ui, TB_UI_FONT_REGULAR, tb_text_from_cstr(text),
                    tb_ui_color_of(ui, TB_UI_COLOR_TEXT));
}

void tb_ui_textv(tb_ui *ui, const char *format, va_list arguments)
{
    char text[TB_UI_FORMATTED_CAPACITY] = {0};

    TB_ASSERT(format != NULL);
    (void)tb_fmt_vbuffer(text, sizeof(text), format, arguments);
    tb_ui_text(ui, text);
}

void tb_ui_textf(tb_ui *ui, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    tb_ui_textv(ui, format, arguments);
    va_end(arguments);
}

void tb_ui_text_colored(tb_ui *ui, tb_vec4 color, const char *text)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(text != NULL);
    tb_ui_text_item(ui, TB_UI_FONT_REGULAR, tb_text_from_cstr(text), color);
}

void tb_ui_heading(tb_ui *ui, const char *text)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(text != NULL);
    tb_ui_text_item(ui, TB_UI_FONT_BOLD, tb_text_from_cstr(text),
                    tb_ui_color_of(ui, TB_UI_COLOR_TEXT));
}

void tb_ui_text_wrapped(tb_ui *ui, const char *text)
{
    const tb_ui_container *container = NULL;
    float width = 0.0f;
    tb_vec2 size = {0};
    tb_canvas_rect item = {0};
    const tb_text whole = tb_text_from_cstr(text);

    TB_ASSERT(ui != NULL);
    TB_ASSERT(text != NULL);
    container = tb_ui_top(ui);
    width = container->columns > 0 ? container->column_width[container->column]
                                   : container->right - container->left;
    size = tb_ui_measure(ui, TB_UI_FONT_REGULAR, whole, width);
    item = tb_ui_next_item(ui, width, size.y, false);
    tb_ui_draw_text(ui, TB_UI_FONT_REGULAR, whole, (tb_vec2){item.x, item.y},
                    tb_ui_color_of(ui, TB_UI_COLOR_TEXT), width, tb_ui_clip(ui), 0);
    tb_ui_set_last(ui, 0, item, tb_ui_mouse_over(ui, item));
}

void tb_ui_value(tb_ui *ui, const char *label, const char *value)
{
    tb_canvas_rect field = {0};
    tb_canvas_rect item = {0};

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    item = tb_ui_next_item(ui, 0.0f, ui->frame_height, true);
    tb_ui_split_label(ui, item, label, &field);
    tb_ui_text_in(ui, TB_UI_FONT_REGULAR, tb_text_from_cstr(value), field, false,
                  tb_ui_color_of(ui, TB_UI_COLOR_TEXT_DIM), 0);
    tb_ui_set_last(ui, 0, item, tb_ui_mouse_over(ui, item));
}

/* ---------------------------------------------------------------------------
 * Buttons, checkboxes, radio buttons, and lists' items.
 * ------------------------------------------------------------------------- */

bool tb_ui_button(tb_ui *ui, const char *label)
{
    tb_ui_id id = 0;
    tb_text shown = {0};
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    bool clicked = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    id = tb_ui_get_id(ui, label);
    shown = tb_ui_label_text(label);
    item = tb_ui_next_item(
        ui, tb_ui_measure(ui, TB_UI_FONT_REGULAR, shown, 0.0f).x + 2.0f * ui->theme.frame_padding.x,
        ui->frame_height, false);
    interaction = tb_ui_interact(ui, id, item, true);
    clicked = interaction.clicked || tb_ui_keys_press(ui, &interaction);
    tb_ui_fill(ui, item, ui->theme.frame_rounding,
               tb_ui_state_color(ui, &interaction, TB_UI_COLOR_BUTTON, TB_UI_COLOR_BUTTON_HOT,
                                 TB_UI_COLOR_BUTTON_ACTIVE),
               id);
    tb_ui_focus_ring(ui, item, ui->theme.frame_rounding, &interaction, id);
    tb_ui_text_in(ui, TB_UI_FONT_REGULAR, shown, item, true, tb_ui_color_of(ui, TB_UI_COLOR_TEXT),
                  id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    return clicked;
}

/* A checkbox's or radio button's place: its mark, and its label after. */
static tb_canvas_rect tb_ui_mark_item(tb_ui *ui, tb_text shown, tb_canvas_rect *mark)
{
    const float size = roundf(ui->fonts[TB_UI_FONT_REGULAR].line_height);
    const float gap = shown.length > 0 ? ui->theme.item_spacing.x * 0.75f : 0.0f;
    const tb_canvas_rect item =
        tb_ui_next_item(ui, size + gap + tb_ui_measure(ui, TB_UI_FONT_REGULAR, shown, 0.0f).x,
                        ui->frame_height, false);

    *mark = (tb_canvas_rect){item.x, item.y + (item.height - size) * 0.5f, size, size};
    tb_ui_text_in(
        ui, TB_UI_FONT_REGULAR, shown,
        (tb_canvas_rect){item.x + size + gap, item.y, item.width - size - gap, item.height}, false,
        tb_ui_color_of(ui, TB_UI_COLOR_TEXT), 0);
    return item;
}

static void tb_ui_check_mark(tb_ui *ui, tb_canvas_rect box, tb_ui_id id)
{
    const float thickness = fmaxf(box.width * 0.13f, 1.5f);
    const tb_vec2 start = {box.x + box.width * 0.24f, box.y + box.height * 0.52f};
    const tb_vec2 corner = {box.x + box.width * 0.43f, box.y + box.height * 0.70f};
    const tb_vec2 end = {box.x + box.width * 0.77f, box.y + box.height * 0.31f};

    tb_ui_line(ui, start, corner, thickness, tb_ui_white, id);
    tb_ui_line(ui, corner, end, thickness, tb_ui_white, id);
}

bool tb_ui_checkbox(tb_ui *ui, const char *label, bool *value)
{
    tb_ui_id id = 0;
    tb_canvas_rect box = {0};
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    bool toggled = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    id = tb_ui_get_id(ui, label);
    item = tb_ui_mark_item(ui, tb_ui_label_text(label), &box);
    interaction = tb_ui_interact(ui, id, item, true);
    toggled = interaction.clicked || tb_ui_keys_press(ui, &interaction);
    if (toggled)
    {
        *value = !*value;
    }
    if (*value)
    {
        tb_ui_fill(
            ui, box, ui->theme.frame_rounding,
            tb_ui_color_of(ui, interaction.hot ? TB_UI_COLOR_ACCENT_HOT : TB_UI_COLOR_ACCENT), id);
        tb_ui_check_mark(ui, box, id);
    }
    else
    {
        tb_ui_field(ui, box, &interaction, id);
    }
    tb_ui_focus_ring(ui, box, ui->theme.frame_rounding, &interaction, id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    return toggled;
}

bool tb_ui_radio(tb_ui *ui, const char *label, int32_t *value, int32_t option)
{
    tb_ui_id id = 0;
    tb_canvas_rect mark = {0};
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    bool chosen = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    id = tb_ui_get_id(ui, label);
    item = tb_ui_mark_item(ui, tb_ui_label_text(label), &mark);
    interaction = tb_ui_interact(ui, id, item, true);
    chosen = (interaction.clicked || tb_ui_keys_press(ui, &interaction)) && *value != option;
    if (chosen)
    {
        *value = option;
    }
    const float radius = mark.width * 0.5f;
    const tb_vec2 center = {mark.x + radius, mark.y + radius};
    if (*value == option)
    {
        tb_ui_circle(
            ui, center, radius,
            tb_ui_color_of(ui, interaction.hot ? TB_UI_COLOR_ACCENT_HOT : TB_UI_COLOR_ACCENT), id);
        tb_ui_circle(ui, center, radius * 0.4f, tb_ui_white, id);
    }
    else
    {
        tb_ui_circle(
            ui, center, radius,
            tb_ui_color_of(ui, interaction.hot ? TB_UI_COLOR_BUTTON_HOT : TB_UI_COLOR_BUTTON), id);
        tb_ui_circle(ui, center, radius - ui->theme.border,
                     tb_ui_state_color(ui, &interaction, TB_UI_COLOR_FRAME, TB_UI_COLOR_FRAME_HOT,
                                       TB_UI_COLOR_FRAME_ACTIVE),
                     id);
    }
    tb_ui_focus_ring(ui, mark, radius, &interaction, id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    return chosen;
}

bool tb_ui_selectable(tb_ui *ui, const char *label, bool selected)
{
    tb_ui_id id = 0;
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    const float padding = ui->theme.frame_padding.x;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    id = tb_ui_get_id(ui, label);
    item = tb_ui_next_item(ui, 0.0f, ui->frame_height, true);
    interaction = tb_ui_interact(ui, id, item, false);
    if (selected)
    {
        tb_ui_fill(ui, item, 0.0f, tb_ui_color_of(ui, TB_UI_COLOR_SELECTION), id);
    }
    else if (interaction.hot)
    {
        tb_ui_fill(ui, item, 0.0f, tb_ui_color_of(ui, TB_UI_COLOR_HEADER_HOT), id);
    }
    tb_ui_text_in(
        ui, TB_UI_FONT_REGULAR, tb_ui_label_text(label),
        (tb_canvas_rect){item.x + padding, item.y, item.width - 2.0f * padding, item.height}, false,
        tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    return interaction.clicked;
}

/* ---------------------------------------------------------------------------
 * Sliders and dragged numbers.
 * ------------------------------------------------------------------------- */

static void tb_ui_format_value(char *text, size_t capacity, double value, const char *format,
                               bool whole)
{
    if (whole)
    {
        (void)tb_fmt_buffer(text, capacity, "%lld", (long long)value);
        return;
    }
    (void)tb_fmt_buffer(text, capacity, format != NULL ? format : "%.3f", value);
}

static double tb_ui_clamp_value(double value, double min, double max)
{
    return value < min ? min : (value > max ? max : value);
}

/* A slider in a field: the value where the mouse is, or a step for each
 * of Left and Right. */
static bool tb_ui_slider_in(tb_ui *ui, tb_ui_id id, tb_canvas_rect field, double *value, double min,
                            double max, const char *format, bool whole)
{
    const tb_ui_interaction interaction = tb_ui_interact(ui, id, field, true);
    const double range = max - min;
    const double step = whole ? 1.0 : range / TB_UI_SLIDER_STEPS;
    double chosen = *value;
    char text[TB_UI_VALUE_CAPACITY] = {0};

    TB_ASSERT_MSG(max > min, "a slider whose max is not more than its min");
    if (interaction.active && field.width > 0.0f)
    {
        const double along =
            tb_ui_clamp_value((double)(ui->mouse.x - field.x) / (double)field.width, 0.0, 1.0);
        chosen = min + along * range;
    }
    while (interaction.keys && tb_ui_key_pressed(ui, TB_KEY_LEFT))
    {
        chosen -= step;
    }
    while (interaction.keys && tb_ui_key_pressed(ui, TB_KEY_RIGHT))
    {
        chosen += step;
    }
    chosen = tb_ui_clamp_value(whole ? round(chosen) : chosen, min, max);
    const bool changed = chosen != *value;
    *value = chosen;

    const float filled = (float)((chosen - min) / range) * field.width;
    tb_vec4 fill = tb_ui_color_of(ui, TB_UI_COLOR_ACCENT);
    fill.w *= interaction.hot || interaction.active ? 0.75f : 0.6f;
    tb_ui_field(ui, field, &interaction, id);
    if (filled > 0.0f)
    {
        tb_ui_fill(ui, (tb_canvas_rect){field.x, field.y, filled, field.height},
                   ui->theme.frame_rounding, fill, id);
    }
    tb_ui_format_value(text, sizeof(text), chosen, format, whole);
    tb_ui_text_in(ui, TB_UI_FONT_REGULAR, tb_text_from_cstr(text), field, true,
                  tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    tb_ui_set_last(ui, id, field, interaction.hot);
    return changed;
}

bool tb_ui_slider_float(tb_ui *ui, const char *label, float *value, float min, float max,
                        const char *format)
{
    tb_canvas_rect field = {0};
    double chosen = 0.0;
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);
    chosen = (double)*value;
    changed = tb_ui_slider_in(ui, tb_ui_get_id(ui, label), field, &chosen, (double)min, (double)max,
                              format, false);
    *value = (float)chosen;
    return changed;
}

bool tb_ui_slider_int(tb_ui *ui, const char *label, int32_t *value, int32_t min, int32_t max)
{
    tb_canvas_rect field = {0};
    double chosen = 0.0;
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);
    chosen = (double)*value;
    changed = tb_ui_slider_in(ui, tb_ui_get_id(ui, label), field, &chosen, (double)min, (double)max,
                              NULL, true);
    *value = (int32_t)chosen;
    return changed;
}

bool tb_ui_drag_in(tb_ui *ui, tb_ui_id id, tb_canvas_rect rect, double *value, float speed,
                   double min, double max, const char *format, bool whole)
{
    const tb_ui_interaction interaction = tb_ui_interact(ui, id, rect, true);
    const bool limited = min < max;
    const double fast = (ui->input.modifiers & TB_MODIFIER_SHIFT) != 0 ? TB_UI_DRAG_FAST : 1.0;
    const double step = whole ? 1.0 : (double)speed * TB_UI_DRAG_FAST;
    double chosen = *value;
    char text[TB_UI_VALUE_CAPACITY] = {0};

    if (interaction.pressed)
    {
        ui->drag_value = *value;
    }
    if (interaction.active && !interaction.pressed)
    {
        ui->drag_value += (double)(ui->mouse.x - ui->last_mouse.x) * (double)speed * fast;
        ui->drag_value = limited ? tb_ui_clamp_value(ui->drag_value, min, max) : ui->drag_value;
        chosen = ui->drag_value;
    }
    while (interaction.keys && tb_ui_key_pressed(ui, TB_KEY_LEFT))
    {
        chosen -= step;
    }
    while (interaction.keys && tb_ui_key_pressed(ui, TB_KEY_RIGHT))
    {
        chosen += step;
    }
    chosen = whole ? round(chosen) : chosen;
    chosen = limited ? tb_ui_clamp_value(chosen, min, max) : chosen;
    const bool changed = chosen != *value;
    *value = chosen;
    if (interaction.hot || interaction.active)
    {
        ui->cursor = TB_CURSOR_RESIZE_HORIZONTAL;
    }
    tb_ui_field(ui, rect, &interaction, id);
    tb_ui_format_value(text, sizeof(text), chosen, format, whole);
    tb_ui_text_in(ui, TB_UI_FONT_REGULAR, tb_text_from_cstr(text), rect, true,
                  tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    tb_ui_set_last(ui, id, rect, interaction.hot);
    return changed;
}

bool tb_ui_drag_float(tb_ui *ui, const char *label, float *value, float speed, float min, float max,
                      const char *format)
{
    tb_canvas_rect field = {0};
    double chosen = 0.0;
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);
    chosen = (double)*value;
    changed = tb_ui_drag_in(ui, tb_ui_get_id(ui, label), field, &chosen, speed, (double)min,
                            (double)max, format, false);
    *value = (float)chosen;
    return changed;
}

bool tb_ui_drag_int(tb_ui *ui, const char *label, int32_t *value, float speed, int32_t min,
                    int32_t max)
{
    tb_canvas_rect field = {0};
    double chosen = 0.0;
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(value != NULL);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);
    chosen = (double)*value;
    changed = tb_ui_drag_in(ui, tb_ui_get_id(ui, label), field, &chosen, speed, (double)min,
                            (double)max, NULL, true);
    *value = (int32_t)tb_ui_clamp_value(chosen, (double)INT32_MIN, (double)INT32_MAX);
    return changed;
}

/* ---------------------------------------------------------------------------
 * Combos.
 * ------------------------------------------------------------------------- */

/* Up and Down choose the item before or after, while the list is shut. */
static bool tb_ui_combo_keys(tb_ui *ui, const tb_ui_interaction *interaction, size_t *selected,
                             size_t count)
{
    size_t chosen = *selected;

    if (!interaction->keys)
    {
        return false;
    }
    while (tb_ui_key_pressed(ui, TB_KEY_UP))
    {
        chosen = chosen > 0 ? chosen - 1 : chosen;
    }
    while (tb_ui_key_pressed(ui, TB_KEY_DOWN))
    {
        chosen = chosen + 1 < count ? chosen + 1 : chosen;
    }
    const bool changed = chosen != *selected;
    *selected = chosen;
    return changed;
}

static bool tb_ui_combo_list(tb_ui *ui, tb_ui_id id, tb_canvas_rect field, size_t *selected,
                             const char *const *items, size_t count)
{
    const size_t shown = count < TB_UI_LIST_ITEMS ? count : TB_UI_LIST_ITEMS;
    bool changed = false;

    tb_ui_begin_popup(ui, id, field, (float)shown * ui->frame_height);
    for (size_t index = 0; index < count; index++)
    {
        TB_ASSERT(items[index] != NULL);
        tb_ui_push_id_int(ui, (int64_t)index);
        if (tb_ui_selectable(ui, items[index], index == *selected))
        {
            changed = index != *selected;
            *selected = index;
            ui->popup_owner = 0;
        }
        tb_ui_pop_id(ui);
    }
    tb_ui_end_popup(ui);
    return changed;
}

bool tb_ui_combo(tb_ui *ui, const char *label, size_t *selected, const char *const *items,
                 size_t count)
{
    tb_ui_id id = 0;
    tb_canvas_rect field = {0};
    tb_ui_interaction interaction = {0};
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(selected != NULL);
    TB_ASSERT(items != NULL || count == 0);
    id = tb_ui_get_id(ui, label);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);
    interaction = tb_ui_interact(ui, id, field, true);
    if (interaction.pressed || tb_ui_keys_press(ui, &interaction))
    {
        ui->popup_owner = ui->popup_owner == id ? 0 : id;
        ui->popup_rect = (tb_canvas_rect){0};
    }
    changed = tb_ui_combo_keys(ui, &interaction, selected, count);

    const float padding = ui->theme.frame_padding.x;
    const float arrow = ui->frame_height;
    tb_ui_fill(ui, field, ui->theme.frame_rounding,
               tb_ui_state_color(ui, &interaction, TB_UI_COLOR_BUTTON, TB_UI_COLOR_BUTTON_HOT,
                                 TB_UI_COLOR_BUTTON_ACTIVE),
               id);
    tb_ui_focus_ring(ui, field, ui->theme.frame_rounding, &interaction, id);
    if (*selected < count)
    {
        tb_ui_text_in(ui, TB_UI_FONT_REGULAR, tb_ui_label_text(items[*selected]),
                      (tb_canvas_rect){field.x + padding, field.y,
                                       fmaxf(field.width - padding - arrow, 0.0f), field.height},
                      false, tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    }
    tb_ui_chevron(ui,
                  (tb_vec2){field.x + field.width - arrow * 0.5f, field.y + field.height * 0.5f},
                  ui->fonts[TB_UI_FONT_REGULAR].cap_height * 0.8f, true,
                  tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    if (ui->popup_owner == id)
    {
        changed = tb_ui_combo_list(ui, id, field, selected, items, count) || changed;
    }
    tb_ui_set_last(ui, id, field, interaction.hot);
    return changed;
}

/* ---------------------------------------------------------------------------
 * Headers and trees.
 * ------------------------------------------------------------------------- */

/* Whether a header or node is open, after a click or a key opens or shuts it. */
static bool tb_ui_toggle(tb_ui *ui, tb_ui_id id, const tb_ui_interaction *interaction,
                         bool default_open)
{
    tb_ui_state *const state = tb_ui_state_at(ui, tb_ui_state_index(ui, id, NULL));

    if (!state->ready)
    {
        state->open = default_open;
        state->ready = true;
    }
    if (interaction->clicked || tb_ui_keys_press(ui, interaction))
    {
        state->open = !state->open;
    }
    if (interaction->keys && tb_ui_key_pressed(ui, TB_KEY_RIGHT))
    {
        state->open = true;
    }
    if (interaction->keys && tb_ui_key_pressed(ui, TB_KEY_LEFT))
    {
        state->open = false;
    }
    return state->open;
}

static void tb_ui_disclosure(tb_ui *ui, tb_canvas_rect item, tb_text shown, bool open,
                             tb_ui_font font, tb_ui_id id)
{
    const float arrow = ui->frame_height;

    tb_ui_chevron(ui, (tb_vec2){item.x + arrow * 0.5f, item.y + item.height * 0.5f},
                  ui->fonts[TB_UI_FONT_REGULAR].cap_height * 0.8f, open,
                  tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    tb_ui_text_in(
        ui, font, shown,
        (tb_canvas_rect){item.x + arrow, item.y, fmaxf(item.width - arrow, 0.0f), item.height},
        false, tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
}

bool tb_ui_header(tb_ui *ui, const char *label, bool default_open)
{
    tb_ui_id id = 0;
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    bool open = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    id = tb_ui_get_id(ui, label);
    item = tb_ui_next_item(ui, 0.0f, ui->frame_height, true);
    interaction = tb_ui_interact(ui, id, item, true);
    open = tb_ui_toggle(ui, id, &interaction, default_open);
    tb_ui_fill(ui, item, ui->theme.frame_rounding,
               tb_ui_color_of(ui, interaction.hot || interaction.active ? TB_UI_COLOR_HEADER_HOT
                                                                        : TB_UI_COLOR_HEADER),
               id);
    tb_ui_focus_ring(ui, item, ui->theme.frame_rounding, &interaction, id);
    tb_ui_disclosure(ui, item, tb_ui_label_text(label), open, TB_UI_FONT_BOLD, id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    return open;
}

bool tb_ui_tree_push(tb_ui *ui, const char *label, bool default_open)
{
    tb_ui_id id = 0;
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    bool open = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    id = tb_ui_get_id(ui, label);
    item = tb_ui_next_item(ui, 0.0f, ui->frame_height, true);
    interaction = tb_ui_interact(ui, id, item, true);
    open = tb_ui_toggle(ui, id, &interaction, default_open);
    if (interaction.hot || interaction.active)
    {
        tb_ui_fill(ui, item, ui->theme.frame_rounding, tb_ui_color_of(ui, TB_UI_COLOR_HEADER_HOT),
                   id);
    }
    tb_ui_focus_ring(ui, item, ui->theme.frame_rounding, &interaction, id);
    tb_ui_disclosure(ui, item, tb_ui_label_text(label), open, TB_UI_FONT_REGULAR, id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    if (open)
    {
        tb_ui_indent(ui);
        tb_ui_push_raw_id(ui, id);
    }
    return open;
}

void tb_ui_tree_pop(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    tb_ui_unindent(ui);
    tb_ui_pop_id(ui);
}

/* ---------------------------------------------------------------------------
 * Progress, colors, and tooltips.
 * ------------------------------------------------------------------------- */

void tb_ui_progress(tb_ui *ui, float fraction, const char *overlay)
{
    tb_canvas_rect item = {0};
    char text[TB_UI_VALUE_CAPACITY] = {0};
    const float kept = fminf(fmaxf(fraction, 0.0f), 1.0f);

    TB_ASSERT(ui != NULL);
    item = tb_ui_next_item(ui, 0.0f, ui->frame_height, true);
    tb_ui_fill(ui, item, ui->theme.frame_rounding, tb_ui_color_of(ui, TB_UI_COLOR_FRAME), 0);
    if (kept > 0.0f)
    {
        tb_ui_fill(ui, (tb_canvas_rect){item.x, item.y, item.width * kept, item.height},
                   ui->theme.frame_rounding, tb_ui_color_of(ui, TB_UI_COLOR_ACCENT), 0);
    }
    tb_ui_outline(ui, item, ui->theme.frame_rounding, ui->theme.border,
                  tb_ui_color_of(ui, TB_UI_COLOR_BUTTON), 0);
    if (overlay == NULL)
    {
        (void)tb_fmt_buffer(text, sizeof(text), "%.0f%%", (double)(kept * 100.0f));
        overlay = text;
    }
    tb_ui_text_in(ui, TB_UI_FONT_REGULAR, tb_text_from_cstr(overlay), item, true,
                  tb_ui_color_of(ui, TB_UI_COLOR_TEXT), 0);
    tb_ui_set_last(ui, 0, item, tb_ui_mouse_over(ui, item));
}

/* A swatch: the color opaque on its left, and over a checkerboard on its
 * right, where its transparency shows. */
static void tb_ui_swatch(tb_ui *ui, tb_canvas_rect rect, tb_vec4 color,
                         const tb_ui_interaction *interaction, tb_ui_id id)
{
    const float half = roundf(rect.width * 0.5f);
    const float cell = (rect.width - half) / (float)(TB_UI_SWATCH_CHECKS / 2);
    const float rows = rect.height / cell;

    tb_ui_fill(ui, rect, ui->theme.frame_rounding, (tb_vec4){color.x, color.y, color.z, 1.0f}, id);
    if (color.w < 1.0f)
    {
        for (int row = 0; (float)row < rows; row++)
        {
            for (int column = 0; column < TB_UI_SWATCH_CHECKS / 2; column++)
            {
                const float light = (row + column) % 2 == 0 ? 0.8f : 0.5f;
                const tb_canvas_rect square = {
                    rect.x + half + (float)column * cell, rect.y + (float)row * cell, cell,
                    fminf(cell, rect.y + rect.height - (rect.y + (float)row * cell))};
                tb_ui_fill(ui, square, 0.0f, (tb_vec4){light, light, light, 1.0f}, id);
            }
        }
        tb_ui_fill(ui, (tb_canvas_rect){rect.x + half, rect.y, rect.width - half, rect.height},
                   0.0f, color, id);
    }
    tb_ui_outline(ui, rect, ui->theme.frame_rounding, ui->theme.border,
                  tb_ui_color_of(ui, interaction->hot ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_BORDER),
                  id);
    tb_ui_focus_ring(ui, rect, ui->theme.frame_rounding, interaction, id);
}

bool tb_ui_color_swatch(tb_ui *ui, const char *label, tb_vec4 color)
{
    tb_ui_id id = 0;
    tb_canvas_rect mark = {0};
    tb_canvas_rect item = {0};
    tb_ui_interaction interaction = {0};
    tb_text shown = {0};

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    id = tb_ui_get_id(ui, label);
    shown = tb_ui_label_text(label);
    const float width = roundf(ui->frame_height * 1.5f);
    const float gap = shown.length > 0 ? ui->theme.item_spacing.x * 0.75f : 0.0f;
    item = tb_ui_next_item(ui, width + gap + tb_ui_measure(ui, TB_UI_FONT_REGULAR, shown, 0.0f).x,
                           ui->frame_height, false);
    mark = (tb_canvas_rect){item.x, item.y + 2.0f, width, item.height - 4.0f};
    tb_ui_text_in(
        ui, TB_UI_FONT_REGULAR, shown,
        (tb_canvas_rect){item.x + width + gap, item.y, item.width - width - gap, item.height},
        false, tb_ui_color_of(ui, TB_UI_COLOR_TEXT), 0);
    interaction = tb_ui_interact(ui, id, mark, true);
    tb_ui_swatch(ui, mark, color, &interaction, id);
    tb_ui_set_last(ui, id, item, interaction.hot);
    return interaction.clicked || tb_ui_keys_press(ui, &interaction);
}

bool tb_ui_color_edit(tb_ui *ui, const char *label, tb_vec4 *color)
{
    static const char *const channels[4] = {"R", "G", "B", "A"};
    static const tb_vec4 marks[4] = {{0.86f, 0.26f, 0.24f, 1.0f},
                                     {0.36f, 0.72f, 0.26f, 1.0f},
                                     {0.22f, 0.48f, 0.92f, 1.0f},
                                     {0.62f, 0.62f, 0.62f, 1.0f}};
    tb_ui_id id = 0;
    tb_canvas_rect field = {0};
    float *const values[4] = {&color->x, &color->y, &color->z, &color->w};
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(color != NULL);
    id = tb_ui_get_id(ui, label);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);

    const float gap = ui->theme.item_spacing.x * 0.5f;
    const float swatch = ui->frame_height;
    const float width = fmaxf((field.width - swatch - 4.0f * gap) / 4.0f, 0.0f);
    const tb_ui_interaction still = {0};
    tb_ui_swatch(ui, (tb_canvas_rect){field.x, field.y, swatch, field.height}, *color, &still, id);
    for (size_t channel = 0; channel < 4; channel++)
    {
        const tb_canvas_rect rect = {field.x + swatch + gap + (float)channel * (width + gap),
                                     field.y, width, field.height};
        double value = (double)*values[channel];
        if (tb_ui_drag_in(ui, tb_ui_derived_id(id, channels[channel]), rect, &value, 0.005f, 0.0,
                          1.0, "%.2f", false))
        {
            *values[channel] = (float)value;
            changed = true;
        }
        /* Each channel is marked by its color on its left, as Unreal marks
         * a vector's axes. */
        tb_ui_fill(ui, (tb_canvas_rect){rect.x, rect.y, 3.0f, rect.height}, 0.0f, marks[channel],
                   0);
    }
    tb_ui_set_last(ui, id, field, false);
    return changed;
}

void tb_ui_tooltip(tb_ui *ui, const char *text)
{
    tb_ui_container layer = {0};
    const tb_vec2 padding = ui->theme.frame_padding;
    const tb_text whole = tb_text_from_cstr(text);

    TB_ASSERT(ui != NULL);
    TB_ASSERT(text != NULL);
    if (ui->last_id == 0 || !ui->last_hot || ui->last_id != ui->hover_id || ui->active != 0 ||
        ui->hover_seconds < (double)ui->theme.tooltip_delay)
    {
        return;
    }
    const tb_vec2 size = tb_ui_measure(ui, TB_UI_FONT_REGULAR, whole, TB_UI_TOOLTIP_WIDTH);
    tb_canvas_rect rect = {ui->mouse.x + TB_UI_TOOLTIP_OFFSET_X,
                           ui->mouse.y + TB_UI_TOOLTIP_OFFSET_Y, size.x + 2.0f * padding.x,
                           size.y + 2.0f * padding.y};
    rect.x = fmaxf(fminf(rect.x, ui->size.x - rect.width), 0.0f);
    rect.y = fmaxf(fminf(rect.y, ui->size.y - rect.height), 0.0f);

    layer.kind = TB_UI_CONTAINER_POPUP;
    layer.layer = ui->layer + TB_UI_POPUP_LAYERS + 1;
    layer.clip = (tb_canvas_rect){0.0f, 0.0f, ui->size.x, ui->size.y};
    layer.id_depth = ui->id_count;
    tb_ui_push_container(ui, &layer);
    tb_ui_fill(
        ui, (tb_canvas_rect){rect.x - 1.0f, rect.y + 1.0f, rect.width + 2.0f, rect.height + 2.0f},
        ui->theme.frame_rounding + 1.0f, tb_ui_color_of(ui, TB_UI_COLOR_SHADOW), 0);
    tb_ui_fill(ui, rect, ui->theme.frame_rounding, tb_ui_color_of(ui, TB_UI_COLOR_POPUP), 0);
    tb_ui_outline(ui, rect, ui->theme.frame_rounding, ui->theme.border,
                  tb_ui_color_of(ui, TB_UI_COLOR_BUTTON), 0);
    tb_ui_draw_text(ui, TB_UI_FONT_REGULAR, whole,
                    (tb_vec2){rect.x + padding.x, rect.y + padding.y},
                    tb_ui_color_of(ui, TB_UI_COLOR_TEXT), TB_UI_TOOLTIP_WIDTH, layer.clip, 0);
    ui->container_count--;
}
