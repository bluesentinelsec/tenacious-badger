/* What the two interfaces share beneath them, the immediate-mode `ui` and
 * the retained `gui`: rectangles, a list of draw commands in units with
 * their edges on whole pixels, fonts and text measured at the display's own
 * pixels, and the commands handed to a renderer's canvas. Private to the
 * engine: each interface keeps its own context, and builds its commands
 * here. */
#ifndef TB_UI_PAINT_H
#define TB_UI_PAINT_H

#include "badger/arena.h"
#include "badger/ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A list of draw commands being built, at a scale of pixels a unit. */
typedef struct tb_ui_paint
{
    tb_ui_command *commands;
    size_t count;
    size_t capacity;
    tb_arena *arena; /* borrowed: the commands' text, kept until it is reset */
    float scale;     /* pixels a unit, to put edges on whole pixels */
} tb_ui_paint;

/* A font's measures at a size, in units. */
typedef struct tb_ui_font_measures
{
    const tb_font *font;
    float line_height;
    float ascent;
    float cap_height;
} tb_ui_font_measures;

/* Rectangles. */
tb_canvas_rect tb_ui_rect_intersect(tb_canvas_rect a, tb_canvas_rect b);
bool tb_ui_rect_contains(tb_canvas_rect rect, tb_vec2 point);
tb_canvas_rect tb_ui_rect_inset(tb_canvas_rect rect, float x, float y);
bool tb_ui_rect_empty(tb_canvas_rect rect);

/* Positions, rectangles and thicknesses moved onto whole pixels at a scale
 * of pixels a unit; a thickness is at least a pixel. */
float tb_ui_snap(float scale, float value);
tb_canvas_rect tb_ui_snap_rect(float scale, tb_canvas_rect rect);
float tb_ui_snap_thickness(float scale, float thickness);

/* Forget a list's commands, to build another at a scale; the arena is the
 * caller's to reset. */
void tb_ui_paint_reset(tb_ui_paint *paint, float scale);
void tb_ui_paint_free(tb_ui_paint *paint);
void tb_ui_paint_push(tb_ui_paint *paint, const tb_ui_command *command);

/* Commands on a layer, clipped; nothing is added when nothing of it would be
 * seen. Rectangles and outlines are snapped to whole pixels. */
void tb_ui_paint_fill(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip, tb_canvas_rect rect,
                      float radius, tb_vec4 color, tb_ui_id id);
void tb_ui_paint_outline(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip,
                         tb_canvas_rect rect, float radius, float thickness, tb_vec4 color,
                         tb_ui_id id);
void tb_ui_paint_circle(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip, tb_vec2 center,
                        float radius, tb_vec4 color, tb_ui_id id);
void tb_ui_paint_line(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip, tb_vec2 from,
                      tb_vec2 to, float thickness, tb_vec4 color, tb_ui_id id);

/* An image stretched over a rectangle and tinted. */
void tb_ui_paint_image(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip,
                       const tb_image *image, tb_canvas_rect rect, tb_vec4 tint, tb_ui_id id);

/* Text and its measures, at `size` units to the em, measured at the
 * display's pixels (`scale` a unit) and turned back into units, so that a
 * line is as long in units as the glyphs the display draws for it. */
typedef struct tb_ui_text_run
{
    const tb_font *font;
    float size;          /* units to the em */
    float wrap_width;    /* units; 0 for none */
    tb_font_align align; /* of lines across the box */
} tb_ui_text_run;

tb_vec2 tb_ui_measure_text(const tb_ui_text_run *run, float scale, tb_text text);
void tb_ui_font_measures_of(const tb_font *font, float size, float scale,
                            tb_ui_font_measures *measures);
/* Text with its box's top left at `position`, copied into the arena. */
void tb_ui_paint_text(tb_ui_paint *paint, int32_t layer, tb_canvas_rect clip,
                      const tb_ui_text_run *run, tb_text text, tb_vec2 position, tb_vec4 color,
                      tb_ui_id id);

/* Draw commands on a renderer's canvas, at `scale` pixels a unit, the
 * units' (0, 0) at the pixel `origin`. */
void tb_ui_paint_submit(tb_renderer *renderer, const tb_ui_command *commands, size_t count,
                        float scale, tb_vec2 origin);

#endif
