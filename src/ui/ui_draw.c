/* The interface's drawing: commands in units, on whole pixels, clipped to
 * what contains them, made in the layer it shares with the retained GUI
 * (ui_paint.h); text measured as the canvas draws it; and the commands
 * handed to a renderer's canvas. */
#include "ui_internal.h"

#include "badger/assert.h"

#include <math.h>

tb_canvas_rect tb_ui_clip(const tb_ui *ui)
{
    if (ui->container_count == 0)
    {
        return (tb_canvas_rect){0.0f, 0.0f, ui->size.x, ui->size.y};
    }
    return ui->containers[ui->container_count - 1].clip;
}

tb_vec4 tb_ui_color_of(const tb_ui *ui, tb_ui_color color)
{
    TB_ASSERT((size_t)color < TB_UI_COLOR_COUNT);
    return ui->theme.colors[color];
}

static int32_t tb_ui_layer(const tb_ui *ui)
{
    return ui->container_count > 0 ? ui->containers[ui->container_count - 1].layer : ui->layer;
}

void tb_ui_push_command(tb_ui *ui, const tb_ui_command *command)
{
    tb_ui_paint_push(&ui->paint, command);
}

void tb_ui_fill(tb_ui *ui, tb_canvas_rect rect, float radius, tb_vec4 color, tb_ui_id id)
{
    tb_ui_paint_fill(&ui->paint, tb_ui_layer(ui), tb_ui_clip(ui), rect, radius, color, id);
}

void tb_ui_outline(tb_ui *ui, tb_canvas_rect rect, float radius, float thickness, tb_vec4 color,
                   tb_ui_id id)
{
    tb_ui_paint_outline(&ui->paint, tb_ui_layer(ui), tb_ui_clip(ui), rect, radius, thickness, color,
                        id);
}

void tb_ui_circle(tb_ui *ui, tb_vec2 center, float radius, tb_vec4 color, tb_ui_id id)
{
    tb_ui_paint_circle(&ui->paint, tb_ui_layer(ui), tb_ui_clip(ui), center, radius, color, id);
}

void tb_ui_line(tb_ui *ui, tb_vec2 from, tb_vec2 to, float thickness, tb_vec4 color, tb_ui_id id)
{
    tb_ui_paint_line(&ui->paint, tb_ui_layer(ui), tb_ui_clip(ui), from, to, thickness, color, id);
}

void tb_ui_chevron(tb_ui *ui, tb_vec2 center, float size, bool down, tb_vec4 color, tb_ui_id id)
{
    const float half = size * 0.5f;
    const float quarter = size * 0.25f;
    const float thickness = fmaxf(size * 0.18f, 1.25f);

    if (down)
    {
        const tb_vec2 tip = {center.x, center.y + quarter};
        tb_ui_line(ui, (tb_vec2){center.x - half, center.y - quarter}, tip, thickness, color, id);
        tb_ui_line(ui, tip, (tb_vec2){center.x + half, center.y - quarter}, thickness, color, id);
        return;
    }
    const tb_vec2 tip = {center.x + quarter, center.y};
    tb_ui_line(ui, (tb_vec2){center.x - quarter, center.y - half}, tip, thickness, color, id);
    tb_ui_line(ui, tip, (tb_vec2){center.x - quarter, center.y + half}, thickness, color, id);
}

/* ---------------------------------------------------------------------------
 * Text.
 * ------------------------------------------------------------------------- */

static tb_ui_text_run tb_ui_run(const tb_ui *ui, tb_ui_font font, float wrap_width)
{
    return (tb_ui_text_run){
        .font = ui->fonts[font].font, .size = ui->theme.font_size, .wrap_width = wrap_width};
}

tb_vec2 tb_ui_measure(const tb_ui *ui, tb_ui_font font, tb_text text, float wrap_width)
{
    const tb_ui_text_run run = tb_ui_run(ui, font, wrap_width);

    return tb_ui_measure_text(&run, ui->scale, text);
}

void tb_ui_draw_text(tb_ui *ui, tb_ui_font font, tb_text text, tb_vec2 position, tb_vec4 color,
                     float wrap_width, tb_canvas_rect clip, tb_ui_id id)
{
    const tb_ui_text_run run = tb_ui_run(ui, font, wrap_width);

    tb_ui_paint_text(&ui->paint, tb_ui_layer(ui), clip, &run, text, position, color, id);
}

float tb_ui_text_top(const tb_ui *ui, tb_ui_font font, tb_canvas_rect rect)
{
    const tb_ui_font_measures *const measures = &ui->fonts[font];

    /* Capitals are centered: they are what the eye lines up. */
    return rect.y + rect.height * 0.5f + measures->cap_height * 0.5f - measures->ascent;
}

void tb_ui_text_in(tb_ui *ui, tb_ui_font font, tb_text text, tb_canvas_rect rect, bool centered,
                   tb_vec4 color, tb_ui_id id)
{
    const float top = tb_ui_text_top(ui, font, rect);
    float left = rect.x;

    if (centered)
    {
        left = rect.x + (rect.width - tb_ui_measure(ui, font, text, 0.0f).x) * 0.5f;
    }
    tb_ui_draw_text(ui, font, text, (tb_vec2){left, top}, color, 0.0f,
                    tb_ui_rect_intersect(tb_ui_clip(ui), rect), id);
}

/* ---------------------------------------------------------------------------
 * Drawing on a canvas.
 * ------------------------------------------------------------------------- */

void tb_ui_draw(const tb_ui *ui, tb_renderer *renderer)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(!ui->in_frame, "an interface drawn before its frame ended");
    tb_ui_paint_submit(renderer, ui->paint.commands, ui->paint.count, ui->scale,
                       (tb_vec2){0.0f, 0.0f});
}
