/* The interface's drawing: commands in units, on whole pixels, clipped to
 * what contains them; text measured as the canvas draws it; and the
 * commands handed to a renderer's canvas. */
#include "ui_internal.h"

#include "badger/assert.h"
#include "badger/font_layout.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

tb_canvas_rect tb_ui_rect_intersect(tb_canvas_rect a, tb_canvas_rect b)
{
    const float left = fmaxf(a.x, b.x);
    const float top = fmaxf(a.y, b.y);
    const float right = fminf(a.x + a.width, b.x + b.width);
    const float bottom = fminf(a.y + a.height, b.y + b.height);

    return (tb_canvas_rect){left, top, fmaxf(right - left, 0.0f), fmaxf(bottom - top, 0.0f)};
}

bool tb_ui_rect_contains(tb_canvas_rect rect, tb_vec2 point)
{
    return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.width &&
           point.y < rect.y + rect.height;
}

tb_canvas_rect tb_ui_rect_inset(tb_canvas_rect rect, float x, float y)
{
    return (tb_canvas_rect){rect.x + x, rect.y + y, fmaxf(rect.width - 2.0f * x, 0.0f),
                            fmaxf(rect.height - 2.0f * y, 0.0f)};
}

static bool tb_ui_rect_empty(tb_canvas_rect rect)
{
    return !(rect.width > 0.0f) || !(rect.height > 0.0f);
}

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

/* A position in units moved onto the nearest whole pixel. */
static float tb_ui_snap(const tb_ui *ui, float value)
{
    return roundf(value * ui->scale) / ui->scale;
}

/* A rectangle's edges moved onto whole pixels. */
static tb_canvas_rect tb_ui_snap_rect(const tb_ui *ui, tb_canvas_rect rect)
{
    const float left = tb_ui_snap(ui, rect.x);
    const float top = tb_ui_snap(ui, rect.y);

    return (tb_canvas_rect){left, top, tb_ui_snap(ui, rect.x + rect.width) - left,
                            tb_ui_snap(ui, rect.y + rect.height) - top};
}

/* A thickness of whole pixels, at least one. */
static float tb_ui_snap_thickness(const tb_ui *ui, float thickness)
{
    return fmaxf(roundf(thickness * ui->scale), 1.0f) / ui->scale;
}

void tb_ui_push_command(tb_ui *ui, const tb_ui_command *command)
{
    if (ui->command_count == ui->command_capacity)
    {
        ui->command_capacity = ui->command_capacity == 0 ? 256 : ui->command_capacity * 2;
        ui->commands = tb_realloc_array(ui->commands, ui->command_capacity, sizeof(*ui->commands));
    }
    ui->commands[ui->command_count] = *command;
    ui->command_count++;
}

/* A command of the current clip and layer, unless nothing of it is seen. */
static void tb_ui_push_clipped(tb_ui *ui, tb_ui_command *command, tb_canvas_rect bounds)
{
    command->layer = tb_ui_layer(ui);
    if (tb_ui_rect_empty(tb_ui_rect_intersect(bounds, command->clip)))
    {
        return;
    }
    tb_ui_push_command(ui, command);
}

void tb_ui_fill(tb_ui *ui, tb_canvas_rect rect, float radius, tb_vec4 color, tb_ui_id id)
{
    tb_ui_command command = {.type = TB_UI_COMMAND_RECT,
                             .clip = tb_ui_clip(ui),
                             .rect = tb_ui_snap_rect(ui, rect),
                             .radius = radius,
                             .color = color,
                             .id = id};

    if (!(color.w > 0.0f))
    {
        return;
    }
    tb_ui_push_clipped(ui, &command, command.rect);
}

void tb_ui_outline(tb_ui *ui, tb_canvas_rect rect, float radius, float thickness, tb_vec4 color,
                   tb_ui_id id)
{
    tb_ui_command command = {.type = TB_UI_COMMAND_OUTLINE,
                             .clip = tb_ui_clip(ui),
                             .rect = tb_ui_snap_rect(ui, rect),
                             .radius = radius,
                             .thickness = tb_ui_snap_thickness(ui, thickness),
                             .color = color,
                             .id = id};

    if (!(color.w > 0.0f) || !(thickness > 0.0f))
    {
        return;
    }
    tb_ui_push_clipped(ui, &command, command.rect);
}

void tb_ui_circle(tb_ui *ui, tb_vec2 center, float radius, tb_vec4 color, tb_ui_id id)
{
    tb_ui_command command = {
        .type = TB_UI_COMMAND_CIRCLE,
        .clip = tb_ui_clip(ui),
        .rect = {center.x - radius, center.y - radius, 2.0f * radius, 2.0f * radius},
        .radius = radius,
        .color = color,
        .id = id};

    tb_ui_push_clipped(ui, &command, command.rect);
}

void tb_ui_line(tb_ui *ui, tb_vec2 from, tb_vec2 to, float thickness, tb_vec4 color, tb_ui_id id)
{
    tb_ui_command command = {.type = TB_UI_COMMAND_LINE,
                             .clip = tb_ui_clip(ui),
                             .from = from,
                             .to = to,
                             .thickness = thickness,
                             .color = color,
                             .id = id};
    const tb_canvas_rect bounds = {fminf(from.x, to.x) - thickness, fminf(from.y, to.y) - thickness,
                                   fabsf(to.x - from.x) + 2.0f * thickness,
                                   fabsf(to.y - from.y) + 2.0f * thickness};

    command.rect = bounds;
    tb_ui_push_clipped(ui, &command, bounds);
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

tb_vec2 tb_ui_measure(const tb_ui *ui, tb_ui_font font, tb_text text, float wrap_width)
{
    const tb_font_layout_options options = {.wrap_width = wrap_width * ui->scale};
    tb_font_text_size size = {0};

    tb_font_measure_text(ui->fonts[font].font, text, ui->theme.font_size * ui->scale, &options,
                         &size);
    return (tb_vec2){size.width / ui->scale, size.height / ui->scale};
}

void tb_ui_draw_text(tb_ui *ui, tb_ui_font font, tb_text text, tb_vec2 position, tb_vec4 color,
                     float wrap_width, tb_canvas_rect clip, tb_ui_id id)
{
    const tb_vec2 size = tb_ui_measure(ui, font, text, wrap_width);
    tb_ui_command command = {.type = TB_UI_COMMAND_TEXT,
                             .clip = clip,
                             .rect = {position.x, position.y, size.x, size.y},
                             .color = color,
                             .font = ui->fonts[font].font,
                             .font_size = ui->theme.font_size,
                             .wrap_width = wrap_width,
                             .id = id};
    char *copy = NULL;

    if (text.length == 0 || !(color.w > 0.0f))
    {
        return;
    }
    command.layer = tb_ui_layer(ui);
    if (tb_ui_rect_empty(tb_ui_rect_intersect(command.rect, clip)))
    {
        return;
    }
    copy = tb_arena_alloc(ui->arena, text.length, 1);
    memcpy(copy, text.bytes, text.length);
    command.text = tb_text_make(copy, text.length);
    tb_ui_push_command(ui, &command);
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

/* A clip in units as whole pixels of the frame, inside it. */
static tb_rhi_rect tb_ui_pixels_of(tb_canvas_rect clip, float scale, uint32_t width,
                                   uint32_t height)
{
    const float left = fmaxf(floorf(clip.x * scale), 0.0f);
    const float top = fmaxf(floorf(clip.y * scale), 0.0f);
    const float right = fminf(ceilf((clip.x + clip.width) * scale), (float)width);
    const float bottom = fminf(ceilf((clip.y + clip.height) * scale), (float)height);

    if (!(right > left) || !(bottom > top))
    {
        return (tb_rhi_rect){0};
    }
    return (tb_rhi_rect){(int32_t)left, (int32_t)top, (uint32_t)(right - left),
                         (uint32_t)(bottom - top)};
}

static void tb_ui_draw_command(tb_renderer *renderer, const tb_ui_command *command)
{
    switch (command->type)
    {
    case TB_UI_COMMAND_RECT:
        tb_canvas_fill_rounded_rect(renderer, command->rect, command->radius, command->color);
        break;
    case TB_UI_COMMAND_OUTLINE:
        tb_canvas_stroke_rounded_rect(renderer, command->rect, command->radius, command->thickness,
                                      command->color);
        break;
    case TB_UI_COMMAND_CIRCLE:
        tb_canvas_fill_circle(
            renderer,
            (tb_vec2){command->rect.x + command->radius, command->rect.y + command->radius},
            command->radius, command->color);
        break;
    case TB_UI_COMMAND_LINE:
        tb_canvas_line(renderer, command->from, command->to, command->thickness, command->color);
        break;
    case TB_UI_COMMAND_TEXT: {
        const tb_font_layout_options options = {.wrap_width = command->wrap_width};
        tb_canvas_draw_text(renderer, command->font, command->text, command->font_size,
                            (tb_vec2){command->rect.x, command->rect.y}, command->color, &options);
        break;
    }
    }
}

void tb_ui_draw(const tb_ui *ui, tb_renderer *renderer)
{
    uint32_t width = 0;
    uint32_t height = 0;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(renderer != NULL);
    TB_ASSERT_MSG(!ui->in_frame, "an interface drawn before its frame ended");
    tb_renderer_size(renderer, &width, &height);
    for (size_t index = 0; index < ui->command_count; index++)
    {
        const tb_ui_command *const command = &ui->commands[index];
        const tb_rhi_rect clip = tb_ui_pixels_of(command->clip, ui->scale, width, height);
        const tb_canvas_view view = {.origin = {0.0f, 0.0f}, .scale = ui->scale, .clip = clip};

        /* A clip of no size is the whole frame to the canvas: nothing of
         * the command is seen. */
        if (clip.width == 0 || clip.height == 0)
        {
            continue;
        }
        tb_canvas_set_view(renderer, &view);
        tb_canvas_set_layer(renderer, command->layer);
        tb_ui_draw_command(renderer, command);
    }
}
