/* What the interface's tests share: an interface over a font of the test
 * data, input made by hand, and frames built from it, with no window. */
#ifndef TB_UI_FIXTURE_H
#define TB_UI_FIXTURE_H

#include "badger/font.h"
#include "badger/ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The size of the tests' text, in units to the em. */
#define UI_FONT_SIZE 14.0f

/* Where the tests' windows begin, in units. */
#define UI_WINDOW_RECT ((tb_canvas_rect){40.0f, 40.0f, 360.0f, 300.0f})

typedef struct ui_fixture ui_fixture;

/* A frame's widgets, given the fixture to keep what they return. */
typedef void ui_scene(ui_fixture *fixture);

struct ui_fixture
{
    tb_font *font;
    tb_ui *ui;
    tb_ui_input input;
    ui_scene *scene;
    /* What scenes keep. */
    bool open;
    bool returned[8];
    tb_ui_id ids[8];
    bool flag;
    int32_t choice;
    int32_t number;
    float value;
    float other;
    size_t selected;
    char text[64];
    tb_vec4 color;
    tb_canvas_rect rect;
};

/* An interface at a scale, 800 by 600 pixels; Roboto as its every font. */
void ui_fixture_open(ui_fixture *fixture, float scale, uint32_t forget_frames);
void ui_fixture_close(ui_fixture *fixture);

/* Build a frame of the scene from the input, then begin the next input. */
void ui_frame(ui_fixture *fixture);

/* Input: the mouse, its left button, the wheel, keys, and text. */
void ui_move(ui_fixture *fixture, float x, float y);
void ui_press(ui_fixture *fixture);
void ui_release(ui_fixture *fixture);
void ui_click(ui_fixture *fixture, tb_vec2 at);
void ui_key(ui_fixture *fixture, tb_key key, uint32_t modifiers);
void ui_type(ui_fixture *fixture, const char *text);

/* What the last frame drew: the first rectangle or circle a widget filled, the
 * first text of some bytes (or NULL), and the middle of a rectangle. */
tb_canvas_rect ui_rect_of(const ui_fixture *fixture, tb_ui_id id);
const tb_ui_command *ui_text_command(const ui_fixture *fixture, const char *text);
tb_vec2 ui_middle(tb_canvas_rect rect);
tb_ui_stats ui_stats(const ui_fixture *fixture);

#endif
