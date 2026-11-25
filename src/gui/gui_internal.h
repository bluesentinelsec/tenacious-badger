/* The retained GUI's own types, shared by its sources: the GUI, the slots
 * of its widgets, their grids, and the functions its parts call in one
 * another. */
#ifndef TB_GUI_INTERNAL_H
#define TB_GUI_INTERNAL_H

#include "badger/arena.h"
#include "badger/gui.h"

#include "../ui/ui_paint.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* No slot: a parent, child, or sibling that is not there. */
#define TB_GUI_NO_NODE UINT32_MAX

/* The two axes of a grid. */
#define TB_GUI_COLUMNS 0
#define TB_GUI_ROWS 1

/* A cursor of a widget that chose none: its parent's. */
#define TB_GUI_CURSOR_INHERIT ((int32_t)TB_CURSOR_COUNT)

/* A function bound to an event. A binding removed while its widget's
 * events are being given has its callback cleared, and is dropped after. */
typedef struct tb_gui_binding
{
    tb_gui_event_kind kind;
    tb_gui_callback *callback;
    void *user;
} tb_gui_binding;

/* A row or column as configured. */
typedef struct tb_gui_track_config
{
    float weight;
    float minsize;
    float pad;
    char *uniform; /* owned; NULL for none */
} tb_gui_track_config;

/* A grid's columns or rows: as configured, and as last laid out. */
typedef struct tb_gui_axis
{
    tb_gui_track_config *configs;
    size_t config_count;
    float *sizes;    /* each track's size, measured, then laid out */
    bool *occupied;  /* a widget is wholly in it */
    float *edges;    /* count + 1 edges, on whole pixels, in units of the GUI */
    size_t count;    /* tracks laid out */
    size_t capacity; /* of sizes and occupied; edges hold one more */
} tb_gui_axis;

/* A slot of a widget. A slot in use holds a widget alive, or one destroyed
 * whose memory waits for the events being given to end. */
typedef struct tb_gui_node
{
    uint32_t generation;
    bool in_use;
    bool alive;
    const tb_widget_class *kind;
    void *data;
    uint32_t parent;
    uint32_t first_child;
    uint32_t last_child;
    uint32_t next;
    uint32_t previous;
    /* The common properties. */
    tb_gui_string name;
    tb_gui_string style;
    bool visible;
    bool enabled;
    int32_t cursor; /* a tb_cursor, or TB_GUI_CURSOR_INHERIT */
    tb_gui_mouse_filter mouse_filter;
    bool gridded;
    tb_grid_options grid;
    int32_t grid_anchor; /* a tb_gui_anchor */
    tb_gui_axis axes[2];
    /* As last laid out. */
    tb_vec2 natural;
    tb_canvas_rect rect;
    bool shown;           /* placed by the last layout: visible, gridded, and in a shown parent */
    tb_vec2 grid_request; /* the size its grid wants */
    tb_canvas_rect clip;  /* where its children are seen: its rect, or its viewport's clip */
    tb_vec2 content;      /* the size its children's grid is laid out in */
    tb_vec2 scroll;       /* how far its children are scrolled, kept within its content */
    bool popup;           /* shown as a popup, out of its parent's grid */
    /* Bindings. */
    tb_gui_binding *bindings;
    size_t binding_count;
    size_t binding_capacity;
    bool bindings_cleared; /* some were removed during events */
} tb_gui_node;

/* A popup shown, and how. */
typedef struct tb_gui_popup_entry
{
    tb_widget widget;
    tb_gui_popup_options options;
} tb_gui_popup_entry;

/* The variables, and the clipboard (gui_var.c). */
typedef struct tb_gui_values tb_gui_values;

/* Navigation's data: the key map, the actions fed, widgets' neighbours,
 * and the focus to give back (gui_navigation.c). */
typedef struct tb_gui_navigation tb_gui_navigation;

/* A font opened from the catalog for a spec, or NULL when it has none. */
typedef struct tb_gui_font_entry
{
    tb_font_spec spec;
    tb_font *font;
} tb_gui_font_entry;

/* A font resolved for text: the font, its size in units, and its measures. */
typedef struct tb_gui_resolved_font
{
    const tb_font *font; /* NULL when there is none: nothing is measured or drawn */
    float size;
} tb_gui_resolved_font;

struct tb_gui
{
    tb_ui_theme theme;
    const tb_font_catalog *catalog;
    tb_gui_font_entry *fonts;
    size_t font_count;
    size_t font_capacity;
    int32_t layer;

    /* The view. */
    uint32_t frame_width;
    uint32_t frame_height;
    float display_scale;
    tb_vec2 reference_size;
    tb_gui_stretch stretch;
    float zoom;
    tb_gui_view view;

    /* Slots. */
    tb_gui_node *nodes;
    size_t node_count;
    size_t node_capacity;
    uint32_t *free_slots;
    size_t free_count;
    size_t free_capacity;
    uint32_t *deferred; /* slots destroyed while events were given */
    size_t deferred_count;
    size_t deferred_capacity;
    size_t alive_count;

    /* Kinds registered. */
    const tb_widget_class **classes;
    size_t class_count;
    size_t class_capacity;

    /* Layout. */
    bool layout_dirty;
    size_t layouts;
    uint32_t *resized; /* slots whose size the last layout changed */
    size_t resized_count;
    size_t resized_capacity;

    /* Input. */
    tb_window_event *queue;
    size_t queue_count;
    size_t queue_capacity;
    double time;
    tb_vec2 mouse; /* in units */
    bool mouse_inside;
    uint32_t modifiers;
    tb_widget hovered;
    tb_widget pressed;
    tb_mouse_button press_button;
    tb_widget focus;
    tb_widget last_press;
    tb_mouse_button last_press_button;
    double last_press_time;
    tb_vec2 last_press_position;
    tb_widget *modals;
    size_t modal_count;
    size_t modal_capacity;
    tb_gui_popup_entry *popups; /* the last in front */
    size_t popup_count;
    size_t popup_capacity;
    double hover_time;  /* when the widget under the mouse was entered */
    tb_widget tooltip;  /* the tooltip shown, or none */
    bool tooltip_spent; /* a press or a key hid it: none until another widget is entered */
    uint32_t dispatch_depth;
    bool event_accepted; /* a binding accepted the event being given */
    bool focus_engaged;  /* the widget with the focus has the keys of navigation */
    bool focus_visible;  /* the keyboard or a gamepad moved the focus last */
    bool wants_mouse;
    bool wants_keyboard;
    tb_cursor cursor;

    /* Variables, and the clipboard; made when first used. */
    tb_gui_values *values;

    /* Navigation; made with the GUI. */
    tb_gui_navigation *navigation;

    /* What tb_gui_draw() draws. */
    tb_arena *arena;
    tb_ui_paint paint;
};

/* What a kind draws with: the widget, its rectangle, and its clip. */
struct tb_gui_paint
{
    tb_gui *gui;
    tb_widget widget;
    tb_canvas_rect rect;
    tb_canvas_rect clip;
    int32_t layer;
};

/* gui.c: slots, handles, and fonts. */
tb_gui_node *tb_gui_node_of(tb_widget widget);
tb_gui_node *tb_gui_node_at(tb_gui *gui, uint32_t index);
tb_widget tb_gui_handle(tb_gui *gui, uint32_t index);
bool tb_gui_node_shown_enabled(tb_gui *gui, uint32_t index);
bool tb_gui_node_inside(tb_gui *gui, uint32_t index, uint32_t ancestor);
void tb_gui_mark_layout(tb_gui *gui);
void tb_gui_flush_deferred(tb_gui *gui);
void tb_gui_string_set(tb_gui_string *string, tb_text text);
void tb_gui_string_free(tb_gui_string *string);
tb_text tb_gui_string_text(const tb_gui_string *string);
tb_gui_resolved_font tb_gui_resolve_font(tb_gui *gui, tb_font_spec spec);

/* gui_property.c: the common properties. */
const tb_gui_property *tb_gui_property_find(const tb_gui_node *node, const char *name);

/* gui_layout.c: the grid. */
void tb_gui_layout(tb_gui *gui);
/* Measure a slot's subtree; place it, then its subtree, in a rectangle. */
tb_vec2 tb_gui_measure(tb_gui *gui, uint32_t index);
void tb_gui_arrange(tb_gui *gui, uint32_t index, tb_canvas_rect rect);
void tb_gui_axis_free(tb_gui_axis *axis);
tb_gui_view tb_gui_compute_view(const tb_gui *gui);
/* How far into extra space an anchor places something along an axis: 0 at
 * the left or top, 1 at the right or bottom. */
float tb_gui_anchor_fraction(int32_t anchor, int axis);

/* gui_input.c: events, the focus, and modal widgets. */
void tb_gui_process_input(tb_gui *gui);
void tb_gui_forget_widget(tb_gui *gui, tb_widget widget);
void tb_gui_dispatch_resized(tb_gui *gui);
void tb_gui_refresh_hover(tb_gui *gui);
void tb_gui_finish_input(tb_gui *gui);
bool tb_gui_modal_contains(const tb_gui *gui, uint32_t index);
/* An event of a kind for a widget, its mouse and modifiers the GUI's; and
 * an event given to a widget, its bindings then its kind: whether the kind
 * used it. */
tb_gui_event tb_gui_event_for(tb_widget widget, tb_gui_event_kind kind);
bool tb_gui_send(tb_widget widget, tb_gui_event *event);
/* Give the focus to a widget, or none, engaged or not; forgets the focus
 * waiting to be given back. */
void tb_gui_give_focus(tb_gui *gui, tb_widget target, bool engaged);
/* Whether a widget takes the focus now, by the flags of its kind: shown,
 * enabled, and not behind a modal widget. */
bool tb_gui_takes_focus(tb_gui *gui, tb_widget widget, uint32_t flags);
/* The modal widget in front, or none. */
tb_widget tb_gui_top_modal(const tb_gui *gui);
/* The slot after one in the order of the tree, within a scope's subtree,
 * into its children when `into`. */
uint32_t tb_gui_walk_next(tb_gui *gui, uint32_t index, uint32_t scope, bool into);
/* Whether a slot is walked for the focus within a scope: shown, and not a
 * modal widget or a popup of its own. */
bool tb_gui_focus_walks(tb_gui *gui, uint32_t index, uint32_t scope);

/* gui_navigation.c: navigation by the keys and a gamepad. */
void tb_gui_navigation_create(tb_gui *gui);
void tb_gui_navigation_free(tb_gui *gui);
/* Begin taking the actions fed before this update; take the next fed
 * before the window event at a position, or after the last; and drop
 * those taken once `events` window events were handled. */
void tb_gui_actions_begin(tb_gui *gui);
bool tb_gui_take_action(tb_gui *gui, size_t position, tb_gui_nav *action);
void tb_gui_actions_end(tb_gui *gui, size_t events);
/* The key and modifiers an action stands for: Enter for Accept. */
tb_key tb_gui_action_key(tb_gui_nav action, uint32_t *modifiers);
/* The neighbour a widget names on a side that takes the focus, or none. */
tb_widget tb_gui_neighbor_taking_focus(tb_widget widget, tb_gui_nav side);
/* The widget within a scope marked `default_focus` that takes the focus;
 * or none. */
tb_widget tb_gui_default_focus_in(tb_gui *gui, uint32_t scope);
/* Remember the focus as a modal widget or a popup is shown, to give it
 * back when it closes; and the widget closed: the focus to give back when
 * the focus was in it or nowhere. */
void tb_gui_remember_focus(tb_gui *gui, tb_widget holder);
void tb_gui_holder_closed(tb_gui *gui, uint32_t holder);
/* Give back the focus waiting to be given back, when it can be. */
void tb_gui_restore_focus(tb_gui *gui);
/* Forget the focus waiting to be given back. */
void tb_gui_forget_restore(tb_gui *gui);
/* The common properties `default_focus` and `focus_wrap`. */
bool tb_gui_get_default_focus(tb_widget widget, const void *data, tb_gui_value *value);
bool tb_gui_set_default_focus(tb_widget widget, void *data, const tb_gui_value *value);
bool tb_gui_get_focus_wrap(tb_widget widget, const void *data, tb_gui_value *value);
bool tb_gui_set_focus_wrap(tb_widget widget, void *data, const tb_gui_value *value);

/* gui_popup.c: popups. The level of the popup a slot is in, from 0 at the
 * back, passive ones counted only when `passive` is true; SIZE_MAX for
 * none. */
size_t tb_gui_popup_level(tb_gui *gui, uint32_t index, bool passive);
/* The popup in front that takes input, or none. */
tb_widget tb_gui_active_popup(const tb_gui *gui);
/* Close the popups from a level up, those in front first. Passive ones are
 * closed only when `passive` is true. */
void tb_gui_close_popups_from(tb_gui *gui, size_t level, bool passive);
void tb_gui_layout_popups(tb_gui *gui);
/* Take a slot out of the popups, without an event. */
void tb_gui_forget_popup(tb_gui *gui, uint32_t index);
/* Close the popups whose parents are hidden, disabled, or out of reach. */
void tb_gui_close_orphan_popups(tb_gui *gui);
/* Move a popup's anchor, as its owner is laid out, without laying out again. */
void tb_gui_move_popup(tb_widget widget, tb_canvas_rect anchor);

/* gui_scroll.c: scroll each ancestor of a slot that scrolls as little as
 * shows a rectangle, in units of the GUI as last laid out. */
void tb_gui_see_rect(tb_gui *gui, uint32_t index, tb_canvas_rect rect);

/* gui_tooltip.c: tooltips shown as the mouse rests. */
void tb_gui_update_tooltips(tb_gui *gui);

/* gui_scrollbar.c: scroll bars, for the kinds that draw their own. A bar
 * along an axis: its track, and the part of the content seen. */
typedef struct tb_gui_bar
{
    tb_canvas_rect track;
    bool vertical;
    float offset;  /* units scrolled */
    float view;    /* units seen */
    float content; /* units in all */
} tb_gui_bar;

/* Where a point is on a bar. */
typedef enum tb_gui_bar_part
{
    TB_GUI_BAR_NONE,
    TB_GUI_BAR_BEFORE, /* the track before the thumb */
    TB_GUI_BAR_THUMB,
    TB_GUI_BAR_AFTER /* the track after the thumb */
} tb_gui_bar_part;

/* How far a bar scrolls, at most. */
float tb_gui_bar_range(const tb_gui_bar *bar);
/* An offset kept from 0 to as far as a bar scrolls. */
float tb_gui_bar_clamp(const tb_gui_bar *bar, float offset);
/* The thumb's rectangle, at least as long as the bar is wide. */
tb_canvas_rect tb_gui_bar_thumb(const tb_gui_bar *bar);
tb_gui_bar_part tb_gui_bar_part_at(const tb_gui_bar *bar, tb_vec2 point);
/* Where along the thumb a point takes it, for dragging. */
float tb_gui_bar_grab(const tb_gui_bar *bar, tb_vec2 point);
/* The offset of the thumb dragged to a point, held where it was grabbed. */
float tb_gui_bar_drag(const tb_gui_bar *bar, tb_vec2 point, float grab);
/* The offset a press on the track pages to: a view's length toward it. */
float tb_gui_bar_page(const tb_gui_bar *bar, tb_gui_bar_part part);
void tb_gui_bar_draw(tb_gui_paint *paint, const tb_gui_bar *bar, bool hot);

/* gui_draw.c: the commands of a frame. */
void tb_gui_build_commands(tb_gui *gui);

/* gui_widgets.c: frames, labels, and buttons. */
void tb_gui_register_builtins(tb_gui *gui);
/* The containers' and overlays' kinds, each in its own source. */
void tb_gui_register_scroll_kinds(tb_gui *gui);
void tb_gui_register_listbox(tb_gui *gui);
void tb_gui_register_menu_kinds(tb_gui *gui);
void tb_gui_register_tooltip(tb_gui *gui);
void tb_gui_register_treeview(tb_gui *gui);
void tb_gui_register_notebook(tb_gui *gui);
void tb_gui_register_panedwindow(tb_gui *gui);
void tb_gui_register_message_box(tb_gui *gui);
void tb_gui_register_image(tb_gui *gui);
void tb_gui_register_canvas(tb_gui *gui);
/* The size of a text, or of one empty line of its font. */
tb_vec2 tb_gui_measure_block(tb_widget widget, const tb_gui_text_block *block);
/* The height of a line of the theme's font, in units. */
float tb_gui_line_height(tb_gui *gui);

/* gui_var.c: variables polled against their settings, their observers run,
 * and their memory released with the GUI's. */
void tb_gui_vars_update(tb_gui *gui);
void tb_gui_vars_free(tb_gui *gui);

/* gui_values.c: the kinds that edit values, registered with the others. */
void tb_gui_register_value_kinds(tb_gui *gui);

#endif
