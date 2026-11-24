/* The interface's own types, shared by its sources: the context, the state
 * it keeps by id, the containers that lay widgets out, and the functions
 * that make draw commands. */
#ifndef TB_UI_INTERNAL_H
#define TB_UI_INTERNAL_H

#include "badger/arena.h"
#include "badger/ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ids pushed at once at most, and containers (windows, panels, the list)
 * open at once. */
#define TB_UI_MOST_IDS 64
#define TB_UI_MOST_CONTAINERS 32

/* Bytes of text a text field's clipboard holds at most. */
#define TB_UI_CLIPBOARD_CAPACITY 4096

/* The layers of the open list and the tooltip, above every window's. */
#define TB_UI_POPUP_LAYERS 1024

/* What the interface keeps of a widget from frame to frame. */
typedef struct tb_ui_state
{
    tb_ui_id id;
    uint64_t seen;       /* the frame it was last used in */
    bool window;         /* it is a window's */
    bool open;           /* a header or node is open; a window is not collapsed */
    bool ready;          /* a header or node has taken its first state */
    tb_canvas_rect rect; /* a window's place and size */
    tb_canvas_rect hit;  /* what of a window the mouse finds it in, last frame */
    float content;       /* a window's or panel's contents' height, last frame */
    float scroll;        /* how far it is scrolled down */
    uint64_t z;          /* a window's order: the higher, the nearer the front */
    int32_t layer;       /* a window's canvas layer this frame */
} tb_ui_state;

/* What contains widgets. */
typedef enum tb_ui_container_kind
{
    TB_UI_CONTAINER_WINDOW,
    TB_UI_CONTAINER_PANEL,
    TB_UI_CONTAINER_POPUP
} tb_ui_container_kind;

/* A window, panel, or list being built: where its widgets go. */
typedef struct tb_ui_container
{
    tb_ui_container_kind kind;
    tb_ui_id id;
    size_t state;   /* its state's index */
    tb_ui_id root;  /* the window, or list, the mouse must be over */
    uint32_t flags; /* a window's tb_ui_window_flags */
    int32_t layer;
    size_t id_depth;     /* the id stack's depth when it began */
    tb_canvas_rect rect; /* all of it */
    tb_canvas_rect body; /* what scrolls: under the title, left of the scroll bar */
    tb_canvas_rect clip; /* what of its body may be seen */
    tb_vec2 padding;
    float base_left; /* the contents' left edge, unindented */
    float left;      /* the contents' left edge */
    float right;     /* the contents' right edge */
    float top;       /* where the contents begin, scrolled */
    float cursor;    /* the top of the next line */
    float line_y;    /* the top of the last line */
    float line_height;
    float line_end; /* the right of the last widget on it */
    bool same_line; /* the next widget goes on the last line */
    float bottom;   /* the lowest of the contents */
    float spacing;  /* down between lines */
    bool scrollbar; /* its contents are taller than its body */
    /* Rows of columns. */
    size_t columns;
    size_t column; /* the next */
    float column_x[TB_UI_MOST_COLUMNS];
    float column_width[TB_UI_MOST_COLUMNS];
    /* A table. */
    bool table;
    size_t table_row;
    float table_spacing; /* the spacing of lines before it */
    float table_top;
    float table_left;
    float table_right;
    bool grip_hot; /* a window's corner that resizes it is under the mouse */
} tb_ui_container;

/* A widget that Tab can focus, in the order of the frame. */
typedef struct tb_ui_focusable
{
    tb_ui_id id;
    tb_ui_id root;
} tb_ui_focusable;

/* The text field being edited: its caret and selection, as byte offsets. */
typedef struct tb_ui_edit
{
    tb_ui_id id;
    size_t caret;
    size_t anchor; /* the selection's other end; the caret when none */
    float scroll;  /* units the text is moved left, to see the caret */
    double blink;  /* when the caret last moved */
} tb_ui_edit;

/* How a widget met the mouse this frame. */
typedef struct tb_ui_interaction
{
    bool hot;     /* the mouse is over it */
    bool pressed; /* a button went down on it, and it is now active */
    bool active;  /* it holds the mouse */
    bool clicked; /* the button came up on it, after going down on it */
    bool focused; /* it has the focus */
    bool keys;    /* it has the focus from Tab, and takes keys */
} tb_ui_interaction;

/* A font's measures at the theme's size, in units. */
typedef struct tb_ui_font_measures
{
    const tb_font *font;
    float line_height;
    float ascent;
    float cap_height;
} tb_ui_font_measures;

struct tb_ui
{
    tb_ui_theme theme;
    int32_t layer;
    uint32_t forget_frames;
    uint64_t frame;
    double time;
    tb_ui_input input;
    float scale;
    tb_vec2 mouse;      /* in units */
    tb_vec2 last_mouse; /* last frame's */
    tb_vec2 size;       /* the frame's, in units */
    tb_ui_font_measures fonts[TB_UI_FONT_COUNT];
    float frame_height; /* a field's: a line of text and its padding */
    tb_arena *arena;    /* the frame's text and layouts */
    bool in_frame;

    /* State by id. */
    tb_ui_state *states;
    size_t state_count;
    size_t state_capacity;
    uint32_t *slots; /* an open-addressed index: a state's index plus 1, or 0 */
    size_t slot_count;
    uint64_t z_counter;
    size_t collisions;

    /* Ids pushed. */
    tb_ui_id ids[TB_UI_MOST_IDS];
    size_t id_count;

    /* Containers being built. */
    tb_ui_container containers[TB_UI_MOST_CONTAINERS];
    size_t container_count;
    size_t windows; /* begun this frame */
    int32_t next_rank;
    tb_ui_id front_window; /* the window in front, whose title is lit */

    /* The mouse. */
    tb_ui_id hovered_window; /* the window, or list, under it */
    tb_ui_id hot;
    tb_ui_id active;
    tb_ui_id pressed; /* the widget the button went down on this frame */
    bool active_seen;
    bool press_taken;
    bool wheel_taken;
    tb_vec2 press_mouse; /* where it went down */
    double drag_value;   /* a number dragged: its value, unrounded */
    tb_cursor cursor;

    /* The keyboard. */
    tb_ui_id focus;
    bool focus_by_keys;
    bool focus_seen;
    bool focus_text;
    bool key_used[TB_UI_MOST_KEYS];
    tb_ui_focusable *focusables;
    size_t focusable_count;
    size_t focusable_capacity;
    tb_ui_edit edit;
    char *clipboard;
    size_t clipboard_length;

    /* The list of a combo. */
    tb_ui_id popup_owner;
    bool popup_seen;
    tb_canvas_rect popup_rect; /* last frame's */
    tb_canvas_rect popup_next; /* this frame's */

    /* The last widget, and the tooltip's timer. */
    tb_ui_id last_id;
    tb_canvas_rect last_rect;
    bool last_hot;
    tb_ui_id hover_id;
    double hover_seconds;

    /* What the frame wants. */
    bool wants_mouse;
    bool wants_keyboard;
    bool wants_text;

    /* The frame's commands. */
    tb_ui_command *commands;
    size_t command_count;
    size_t command_capacity;
};

/* ui.c: ids, state, and the mouse. */
tb_ui_id tb_ui_hash_id(tb_ui_id seed, const void *bytes, size_t length);
tb_ui_id tb_ui_derived_id(tb_ui_id id, const char *salt);
void tb_ui_push_raw_id(tb_ui *ui, tb_ui_id id);
size_t tb_ui_touch(tb_ui *ui, tb_ui_id id, bool *created);
size_t tb_ui_state_index(tb_ui *ui, tb_ui_id id, bool *created);
tb_ui_state *tb_ui_state_at(tb_ui *ui, size_t index);
tb_ui_interaction tb_ui_interact(tb_ui *ui, tb_ui_id id, tb_canvas_rect rect, bool focusable);
bool tb_ui_mouse_over(const tb_ui *ui, tb_canvas_rect rect);
bool tb_ui_key_pressed(tb_ui *ui, tb_key key);
void tb_ui_set_last(tb_ui *ui, tb_ui_id id, tb_canvas_rect rect, bool hot);
tb_text tb_ui_label_text(const char *label);

/* ui_layout.c: containers and the places of widgets. */
tb_ui_container *tb_ui_top(tb_ui *ui);
tb_canvas_rect tb_ui_next_item(tb_ui *ui, float width, float height, bool fill);
void tb_ui_push_container(tb_ui *ui, const tb_ui_container *container);
void tb_ui_end_container(tb_ui *ui);
void tb_ui_split_label(tb_ui *ui, tb_canvas_rect item, const char *label, tb_canvas_rect *field);
void tb_ui_begin_popup(tb_ui *ui, tb_ui_id owner, tb_canvas_rect below, float height);
void tb_ui_end_popup(tb_ui *ui);

/* ui_draw.c: commands, text, and shapes. */
tb_canvas_rect tb_ui_rect_intersect(tb_canvas_rect a, tb_canvas_rect b);
bool tb_ui_rect_contains(tb_canvas_rect rect, tb_vec2 point);
tb_canvas_rect tb_ui_rect_inset(tb_canvas_rect rect, float x, float y);
tb_canvas_rect tb_ui_clip(const tb_ui *ui);
void tb_ui_fill(tb_ui *ui, tb_canvas_rect rect, float radius, tb_vec4 color, tb_ui_id id);
void tb_ui_outline(tb_ui *ui, tb_canvas_rect rect, float radius, float thickness, tb_vec4 color,
                   tb_ui_id id);
void tb_ui_circle(tb_ui *ui, tb_vec2 center, float radius, tb_vec4 color, tb_ui_id id);
void tb_ui_line(tb_ui *ui, tb_vec2 from, tb_vec2 to, float thickness, tb_vec4 color, tb_ui_id id);
void tb_ui_chevron(tb_ui *ui, tb_vec2 center, float size, bool down, tb_vec4 color, tb_ui_id id);
tb_vec2 tb_ui_measure(const tb_ui *ui, tb_ui_font font, tb_text text, float wrap_width);
void tb_ui_draw_text(tb_ui *ui, tb_ui_font font, tb_text text, tb_vec2 position, tb_vec4 color,
                     float wrap_width, tb_canvas_rect clip, tb_ui_id id);
float tb_ui_text_top(const tb_ui *ui, tb_ui_font font, tb_canvas_rect rect);
void tb_ui_text_in(tb_ui *ui, tb_ui_font font, tb_text text, tb_canvas_rect rect, bool centered,
                   tb_vec4 color, tb_ui_id id);
tb_vec4 tb_ui_color_of(const tb_ui *ui, tb_ui_color color);
void tb_ui_push_command(tb_ui *ui, const tb_ui_command *command);

/* ui_widgets.c, used by others: a number dragged in a rectangle. */
bool tb_ui_drag_in(tb_ui *ui, tb_ui_id id, tb_canvas_rect rect, double *value, float speed,
                   double min, double max, const char *format, bool whole);

/* ui_text_field.c: the text field being edited. */
bool tb_ui_edit_text(tb_ui *ui, tb_ui_id id, tb_canvas_rect field, char *buffer, size_t capacity,
                     const tb_ui_interaction *interaction);

#endif
