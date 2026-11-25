/* What the retained GUI's tests share: a GUI over a font of the test data,
 * a kind of box of a size the cases choose, input made by hand as window
 * events, and a log of the events widgets were given. */
#ifndef TB_GUI_FIXTURE_H
#define TB_GUI_FIXTURE_H

#include "badger/font.h"
#include "badger/gui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The size of the tests' text, in units to the em. */
#define GUI_FONT_SIZE 14.0f
/* The frame's size, in units at the fixture's scale. */
#define GUI_WIDTH 800.0f
#define GUI_HEIGHT 600.0f
/* Events a log holds at most. */
#define GUI_LOG_CAPACITY 64

typedef struct gui_fixture
{
    tb_font *font;
    tb_gui *gui;
    float scale;   /* the display's */
    tb_vec2 mouse; /* where the last input put it, in units */
} gui_fixture;

/* A GUI of 800 by 600 units at a display's scale, its theme's every font
 * Roboto at GUI_FONT_SIZE, its frame given and laid out. */
void gui_fixture_open(gui_fixture *fixture, float scale);
void gui_fixture_close(gui_fixture *fixture);

/* A frame of a size in pixels, at a display's scale. */
void gui_resize(gui_fixture *fixture, uint32_t width, uint32_t height, float scale);

/* Run an update of a sixtieth of a second. */
void gui_update(gui_fixture *fixture);

/* The kind "box": its content is `size` units (a property), and nothing
 * else. Focusable boxes take Tab; typing boxes take the focus on a click. */
const tb_widget_class *gui_box_class(void);
const tb_widget_class *gui_focus_box_class(void);
const tb_widget_class *gui_typing_box_class(void);
tb_widget gui_box(tb_widget parent, float width, float height);
tb_widget gui_box_of(tb_widget parent, const tb_widget_class *kind, float width, float height);

/* Input, in units of the GUI, fed and then handled by an update. */
void gui_move(gui_fixture *fixture, float x, float y);
void gui_press(gui_fixture *fixture, tb_mouse_button button);
void gui_release(gui_fixture *fixture, tb_mouse_button button);
void gui_click(gui_fixture *fixture, tb_vec2 at);
void gui_wheel(gui_fixture *fixture, float notches);
void gui_key(gui_fixture *fixture, tb_key key, uint32_t modifiers);
void gui_type(gui_fixture *fixture, const char *text);
void gui_leave(gui_fixture *fixture);

/* A window event of the fixture's mouse: its place in pixels. */
tb_window_event gui_mouse_event(const gui_fixture *fixture, tb_window_event_type type, float x,
                                float y);

/* The middle of a rectangle. */
tb_vec2 gui_middle(tb_canvas_rect rect);

/* Events given to widgets, in order. */
typedef struct gui_log
{
    tb_gui_event_kind kinds[GUI_LOG_CAPACITY];
    tb_widget widgets[GUI_LOG_CAPACITY];
    tb_widget targets[GUI_LOG_CAPACITY];
    tb_key keys[GUI_LOG_CAPACITY];
    size_t count;
} gui_log;

/* A callback that adds its event to the gui_log given as `user`. */
void gui_record(const tb_gui_event *event, void *user);

/* Bind every kind of event of a widget to the log, but motion. */
void gui_record_all(tb_widget widget, gui_log *log);

/* How many events of a kind the log holds for a widget. */
size_t gui_log_count(const gui_log *log, tb_gui_event_kind kind, tb_widget widget);

#endif
