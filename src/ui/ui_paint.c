/* The layer beneath both interfaces: see ui_paint.h. */
#include "ui_paint.h"

#include "badger/assert.h"
#include "badger/font_layout.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

/* Commands a list first holds. */
#define TB_UI_PAINT_FIRST_CAPACITY 256

/* The share of the ascent a font without a cap height is given for one. */
#define TB_UI_CAP_FALLBACK 0.7f

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

bool tb_ui_rect_empty(tb_canvas_rect rect)
{
    return !(rect.width > 0.0f) || !(rect.height > 0.0f);
}

float tb_ui_snap(float scale, float value)
{
    return roundf(value * scale) / scale;
}

tb_canvas_rect tb_ui_snap_rect(float scale, tb_canvas_rect rect)
{
    const float left = tb_ui_snap(scale, rect.x);
    const float top = tb_ui_snap(scale, rect.y);

    return (tb_canvas_rect){left, top, tb_ui_snap(scale, rect.x + rect.width) - left,
                            tb_ui_snap(scale, rect.y + rect.height) - top};
}

float tb_ui_snap_thickness(float scale, float thickness)
{
    return fmaxf(roundf(thickness * scale), 1.0f) / scale;
}

void tb_ui_paint_reset(tb_ui_paint *paint, float scale)
{
    TB_ASSERT(scale > 0.0f);
    paint->count = 0;
    paint->scale = scale;
}

void tb_ui_paint_free(tb_ui_paint *paint)
{
    tb_free(paint->commands);
    paint->commands = NULL;
    paint->count = 0;
    paint->capacity = 0;
}

void tb_ui_paint_push(tb_ui_paint *paint, const tb_ui_command *command)
{
    if (paint->count == paint->capacity)
    {
        paint->capacity = paint->capacity == 0 ? TB_UI_PAINT_FIRST_CAPACITY : paint->capacity * 2;
        paint->commands =
            tb_realloc_array(paint->commands, paint->capacity, sizeof(*paint->commands));
    }
    paint->commands[paint->count] = *command;
    paint->count++;
}

/* A command on a layer, unless nothing of its bounds is seen in its clip. */
static void tb_ui_paint_clipped(tb_ui_paint *paint, tb_ui_command *command, int32_t layer,
                                tb_canvas_rect bounds)
{
    command->layer = layer;
    if (tb_ui_rect_empty(tb_ui_rect_intersect(bounds, command->clip)))
    {
        return;
    }
    tb_ui_paint_push(paint, command);
}

void tb_ui_paint_fill(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip, tb_canvas_rect rect,
                      float radius, tb_vec4 color, tb_ui_id id)
{
    tb_ui_command command = {.type = TB_UI_COMMAND_RECT,
                             .clip = clip,
                             .rect = tb_ui_snap_rect(paint->scale, rect),
                             .radius = radius,
                             .color = color,
                             .id = id};

    if (!(color.w > 0.0f))
    {
        return;
    }
    tb_ui_paint_clipped(paint, &command, layer, command.rect);
}

void tb_ui_paint_outline(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip,
                         tb_canvas_rect rect, float radius, float thickness, tb_vec4 color,
                         tb_ui_id id)
{
    tb_ui_command command = {.type = TB_UI_COMMAND_OUTLINE,
                             .clip = clip,
                             .rect = tb_ui_snap_rect(paint->scale, rect),
                             .radius = radius,
                             .thickness = tb_ui_snap_thickness(paint->scale, thickness),
                             .color = color,
                             .id = id};

    if (!(color.w > 0.0f) || !(thickness > 0.0f))
    {
        return;
    }
    tb_ui_paint_clipped(paint, &command, layer, command.rect);
}

void tb_ui_paint_circle(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip, tb_vec2 center,
                        float radius, tb_vec4 color, tb_ui_id id)
{
    tb_ui_command command = {
        .type = TB_UI_COMMAND_CIRCLE,
        .clip = clip,
        .rect = {center.x - radius, center.y - radius, 2.0f * radius, 2.0f * radius},
        .radius = radius,
        .color = color,
        .id = id};

    tb_ui_paint_clipped(paint, &command, layer, command.rect);
}

void tb_ui_paint_line(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip, tb_vec2 from,
                      tb_vec2 to, float thickness, tb_vec4 color, tb_ui_id id)
{
    const tb_canvas_rect bounds = {fminf(from.x, to.x) - thickness, fminf(from.y, to.y) - thickness,
                                   fabsf(to.x - from.x) + 2.0f * thickness,
                                   fabsf(to.y - from.y) + 2.0f * thickness};
    tb_ui_command command = {.type = TB_UI_COMMAND_LINE,
                             .clip = clip,
                             .rect = bounds,
                             .from = from,
                             .to = to,
                             .thickness = thickness,
                             .color = color,
                             .id = id};

    tb_ui_paint_clipped(paint, &command, layer, bounds);
}

void tb_ui_paint_image(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip,
                       const tb_image *image, tb_canvas_rect rect, tb_vec4 tint, tb_ui_id id)
{
    tb_ui_command command = {
        .type = TB_UI_COMMAND_IMAGE, .clip = clip, .rect = rect, .color = tint, .id = id};

    command.image = image;
    tb_ui_paint_clipped(paint, &command, layer, rect);
}

/* ---------------------------------------------------------------------------
 * Text.
 * ------------------------------------------------------------------------- */

tb_vec2 tb_ui_measure_text(const tb_ui_text_run *run, float scale, tb_text text)
{
    const tb_font_layout_options options = {.wrap_width = run->wrap_width * scale,
                                            .align = run->align};
    tb_font_text_size size = {0};

    tb_font_measure_text(run->font, text, run->size * scale, &options, &size);
    return (tb_vec2){size.width / scale, size.height / scale};
}

void tb_ui_font_measures_of(const tb_font *font, float size, float scale,
                            tb_ui_font_measures *measures)
{
    const float pixels = size * scale;
    tb_font_line_metrics lines = {0};
    tb_font_metrics metrics = {0};

    tb_font_get_line_metrics(font, pixels, &lines);
    tb_font_get_metrics(font, &metrics);
    measures->font = font;
    measures->line_height = lines.line_height / scale;
    measures->ascent = lines.ascent / scale;
    measures->cap_height = (float)metrics.cap_height * tb_font_scale(font, pixels) / scale;
    if (!(measures->cap_height > 0.0f))
    {
        measures->cap_height = measures->ascent * TB_UI_CAP_FALLBACK;
    }
}

void tb_ui_paint_text(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip,
                      const tb_ui_text_run *run, tb_text text, tb_vec2 position, tb_vec4 color,
                      tb_ui_id id)
{
    tb_ui_command command = {.type = TB_UI_COMMAND_TEXT,
                             .layer = layer,
                             .clip = clip,
                             .color = color,
                             .font = run->font,
                             .font_size = run->size,
                             .wrap_width = run->wrap_width,
                             .align = run->align,
                             .id = id};
    char *copy = NULL;

    if (text.length == 0 || !(color.w > 0.0f))
    {
        return;
    }
    const tb_vec2 size = tb_ui_measure_text(run, paint->scale, text);
    command.rect = (tb_canvas_rect){position.x, position.y, size.x, size.y};
    if (tb_ui_rect_empty(tb_ui_rect_intersect(command.rect, clip)))
    {
        return;
    }
    copy = tb_arena_alloc(paint->arena, text.length, 1);
    memcpy(copy, text.bytes, text.length);
    command.text = tb_text_make(copy, text.length);
    tb_ui_paint_push(paint, &command);
}

/* ---------------------------------------------------------------------------
 * Drawing on a canvas.
 * ------------------------------------------------------------------------- */

/* A clip in units as whole pixels of the frame, inside it. */
static tb_rhi_rect tb_ui_pixels_of(tb_canvas_rect clip, float scale, tb_vec2 origin, uint32_t width,
                                   uint32_t height)
{
    const float left = fmaxf(floorf(origin.x + clip.x * scale), 0.0f);
    const float top = fmaxf(floorf(origin.y + clip.y * scale), 0.0f);
    const float right = fminf(ceilf(origin.x + (clip.x + clip.width) * scale), (float)width);
    const float bottom = fminf(ceilf(origin.y + (clip.y + clip.height) * scale), (float)height);

    if (!(right > left) || !(bottom > top))
    {
        return (tb_rhi_rect){0};
    }
    return (tb_rhi_rect){(int32_t)left, (int32_t)top, (uint32_t)(right - left),
                         (uint32_t)(bottom - top)};
}

static void tb_ui_paint_command(tb_renderer *renderer, const tb_ui_command *command)
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
        const tb_font_layout_options options = {.wrap_width = command->wrap_width,
                                                .align = command->align};
        tb_canvas_draw_text(renderer, command->font, command->text, command->font_size,
                            (tb_vec2){command->rect.x, command->rect.y}, command->color, &options);
        break;
    }
    case TB_UI_COMMAND_IMAGE:
        tb_canvas_draw_sprite(
            renderer, &(tb_canvas_sprite){.image = command->image,
                                          .position = {command->rect.x, command->rect.y},
                                          .size = {command->rect.width, command->rect.height},
                                          .tint = command->color});
        break;
    }
}

void tb_ui_paint_submit(tb_renderer *renderer, const tb_ui_command *commands, size_t count,
                        float scale, tb_vec2 origin)
{
    uint32_t width = 0;
    uint32_t height = 0;

    TB_ASSERT(renderer != NULL);
    TB_ASSERT(scale > 0.0f);
    tb_renderer_size(renderer, &width, &height);
    for (size_t index = 0; index < count; index++)
    {
        const tb_ui_command *const command = &commands[index];
        const tb_rhi_rect clip = tb_ui_pixels_of(command->clip, scale, origin, width, height);
        const tb_canvas_view view = {.origin = origin, .scale = scale, .clip = clip};

        /* A clip of no size is the whole frame to the canvas: nothing of
         * the command is seen. */
        if (clip.width == 0 || clip.height == 0)
        {
            continue;
        }
        tb_canvas_set_view(renderer, &view);
        tb_canvas_set_layer(renderer, command->layer);
        tb_ui_paint_command(renderer, command);
    }
}
