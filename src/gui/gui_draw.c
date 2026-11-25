/* Drawing: the commands of a GUI, built from its tree in the order it is
 * drawn, through the layer the interfaces share (ui_paint.h); what kinds
 * draw with; and text measured and set in rectangles. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <math.h>

/* Text whose height is less than this many lines is one line. */
#define TB_GUI_ONE_LINE 1.5f

static tb_ui_id tb_gui_id_of(tb_widget widget)
{
    return (tb_ui_id)widget.index + 1u;
}

/* ---------------------------------------------------------------------------
 * Text.
 * ------------------------------------------------------------------------- */

/* A text block's run in a widget's font. */
static bool tb_gui_run_of(tb_widget widget, const tb_gui_text_block *block, tb_ui_text_run *run)
{
    const tb_gui_resolved_font resolved = tb_gui_widget_font(widget, block->font);
    const tb_font *const font = block->face != NULL ? block->face : resolved.font;

    if (font == NULL)
    {
        return false;
    }
    *run = (tb_ui_text_run){.font = font,
                            .size = resolved.size,
                            .wrap_width = block->wrap_length > 0.0f ? block->wrap_length : 0.0f,
                            .align = block->justify};
    return true;
}

tb_vec2 tb_widget_measure_text(tb_widget widget, const tb_gui_text_block *block)
{
    tb_ui_text_run run = {0};

    TB_ASSERT(block != NULL);
    if (block->text.length == 0 || !tb_widget_alive(widget) || !tb_gui_run_of(widget, block, &run))
    {
        return (tb_vec2){0};
    }
    return tb_ui_measure_text(&run, widget.gui->view.scale, block->text);
}

tb_vec2 tb_gui_measure_text(tb_gui *gui, const tb_gui_text_block *block)
{
    TB_ASSERT(gui != NULL);
    return tb_widget_measure_text(tb_gui_root(gui), block);
}

static float tb_gui_justify_fraction(tb_font_align align)
{
    switch (align)
    {
    case TB_FONT_ALIGN_CENTER:
        return 0.5f;
    case TB_FONT_ALIGN_RIGHT:
        return 1.0f;
    case TB_FONT_ALIGN_LEFT:
        break;
    }
    return 0.0f;
}

void tb_gui_paint_text(tb_gui_paint *paint, const tb_gui_text_block *block, tb_canvas_rect rect,
                       tb_vec4 color)
{
    tb_gui *const gui = paint->gui;
    tb_ui_text_run run = {0};
    tb_ui_font_measures measures = {0};

    TB_ASSERT(block != NULL);
    if (block->text.length == 0 || !tb_gui_run_of(paint->widget, block, &run))
    {
        return;
    }
    const float scale = gui->view.scale;
    const tb_vec2 size = tb_ui_measure_text(&run, scale, block->text);
    /* Text wider than its rectangle starts at its left edge, where it is
     * read from, and is cut off at the right. */
    const float across =
        size.x > rect.width ? 0.0f : tb_gui_anchor_fraction((int32_t)block->anchor, TB_GUI_COLUMNS);
    const float down = tb_gui_anchor_fraction((int32_t)block->anchor, TB_GUI_ROWS);
    float left = rect.x + (rect.width - size.x) * across;
    float top = rect.y + (rect.height - size.y) * down;

    /* Wrapped lines are aligned within the wrap width: the box is moved so
     * that its widest line lands where the text is placed. */
    if (run.wrap_width > 0.0f)
    {
        left -= (run.wrap_width - size.x) * tb_gui_justify_fraction(run.align);
    }
    tb_ui_font_measures_of(run.font, run.size, scale, &measures);
    if (down == 0.5f && size.y < measures.line_height * TB_GUI_ONE_LINE)
    {
        /* Capitals are centered: they are what the eye lines up. */
        top = rect.y + rect.height * 0.5f + measures.cap_height * 0.5f - measures.ascent;
    }
    tb_ui_paint_text(&gui->paint, paint->layer, tb_ui_rect_intersect(paint->clip, rect), &run,
                     block->text, (tb_vec2){left, top}, color, tb_gui_id_of(paint->widget));
}

/* ---------------------------------------------------------------------------
 * What kinds draw with.
 * ------------------------------------------------------------------------- */

tb_canvas_rect tb_gui_paint_rect(const tb_gui_paint *paint)
{
    TB_ASSERT(paint != NULL);
    return paint->rect;
}

tb_canvas_rect tb_gui_paint_clip(const tb_gui_paint *paint)
{
    TB_ASSERT(paint != NULL);
    return paint->clip;
}

void tb_gui_paint_fill(tb_gui_paint *paint, tb_canvas_rect rect, float radius, tb_vec4 color)
{
    TB_ASSERT(paint != NULL);
    tb_ui_paint_fill(&paint->gui->paint, paint->layer, paint->clip, rect, radius, color,
                     tb_gui_id_of(paint->widget));
}

void tb_gui_paint_outline(tb_gui_paint *paint, tb_canvas_rect rect, float radius, float thickness,
                          tb_vec4 color)
{
    TB_ASSERT(paint != NULL);
    tb_ui_paint_outline(&paint->gui->paint, paint->layer, paint->clip, rect, radius, thickness,
                        color, tb_gui_id_of(paint->widget));
}

void tb_gui_paint_line(tb_gui_paint *paint, tb_vec2 from, tb_vec2 to, float thickness,
                       tb_vec4 color)
{
    TB_ASSERT(paint != NULL);
    tb_ui_paint_line(&paint->gui->paint, paint->layer, paint->clip, from, to, thickness, color,
                     tb_gui_id_of(paint->widget));
}

void tb_gui_paint_image(tb_gui_paint *paint, const tb_image *image, tb_canvas_rect rect,
                        tb_vec4 tint)
{
    TB_ASSERT(paint != NULL);
    TB_ASSERT(image != NULL);
    tb_ui_paint_image(&paint->gui->paint, paint->layer, paint->clip, image, rect, tint,
                      tb_gui_id_of(paint->widget));
}

/* ---------------------------------------------------------------------------
 * The tree, drawn.
 * ------------------------------------------------------------------------- */

static void tb_gui_draw_node(tb_gui *gui, uint32_t index, tb_canvas_rect clip, int32_t layer)
{
    const tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_canvas_rect inner = tb_ui_rect_intersect(clip, node->rect);

    if (!node->shown || tb_ui_rect_empty(inner))
    {
        return;
    }
    if (node->kind->draw != NULL)
    {
        tb_gui_paint paint = {.gui = gui,
                              .widget = tb_gui_handle(gui, index),
                              .rect = node->rect,
                              .clip = inner,
                              .layer = layer};
        node->kind->draw(paint.widget, node->data, &paint);
    }
    /* Children are seen within the clip of a kind that scrolls; popups and
     * modal widgets are drawn above, on their own layers. */
    const tb_canvas_rect seen = tb_ui_rect_intersect(inner, tb_gui_node_at(gui, index)->clip);
    for (uint32_t child = tb_gui_node_at(gui, index)->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->next)
    {
        if (!tb_gui_modal_contains(gui, child) && !tb_gui_node_at(gui, child)->popup)
        {
            tb_gui_draw_node(gui, child, seen, layer);
        }
    }
}

void tb_gui_build_commands(tb_gui *gui)
{
    const tb_canvas_rect all = {0.0f, 0.0f, gui->view.size.x, gui->view.size.y};
    tb_vec4 shade = {0};

    tb_gui_layout(gui);
    /* The shade behind modal widgets is the root's. */
    const tb_style_value *const shaded = tb_style_find(tb_style_in(tb_gui_root(gui), 0), "shade");
    if (shaded != NULL && shaded->type == TB_STYLE_COLOR)
    {
        shade = shaded->color;
    }
    tb_arena_reset(gui->arena);
    tb_ui_paint_reset(&gui->paint, gui->view.scale);
    tb_gui_draw_node(gui, 0, all, gui->layer);
    for (size_t index = 0; index < gui->modal_count; index++)
    {
        const tb_widget modal = gui->modals[index];
        const int32_t layer = gui->layer + 1 + (int32_t)index;
        if (!tb_widget_alive(modal) || !tb_gui_node_at(gui, modal.index)->shown)
        {
            continue;
        }
        tb_ui_paint_fill(&gui->paint, layer, all, all, 0.0f, shade, 0);
        tb_gui_draw_node(gui, modal.index, all, layer);
    }
    for (size_t index = 0; index < gui->popup_count; index++)
    {
        const tb_widget popup = gui->popups[index].widget;
        const int32_t layer = gui->layer + 1 + (int32_t)(gui->modal_count + index);
        if (tb_widget_alive(popup))
        {
            tb_gui_draw_node(gui, popup.index, all, layer);
        }
    }
}

const tb_ui_command *tb_gui_commands(const tb_gui *gui, size_t *count)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(count != NULL);
    *count = gui->paint.count;
    return gui->paint.commands;
}

void tb_gui_draw(const tb_gui *gui, tb_renderer *renderer)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(renderer != NULL);
    tb_ui_paint_submit(renderer, gui->paint.commands, gui->paint.count, gui->view.scale,
                       gui->view.origin);
}
