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

/* A value of a style for a set of states: a theme's, or a widget's own
 * override (gui_style.c). */
typedef struct tb_gui_style_entry
{
    char *name; /* owned */
    uint32_t states;
    tb_style_value value;
} tb_gui_style_entry;

/* A widget's styles resolved, one for each set of states asked for, kept
 * while the GUI's style epoch is the one they were resolved in. */
typedef struct tb_gui_style_cache tb_gui_style_cache;

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
    /* Styles. */
    tb_theme *style_theme;         /* its own, borrowed; NULL for its ancestors' */
    tb_gui_style_entry *overrides; /* its own values, before every theme's */
    size_t override_count;
    size_t override_capacity;
    tb_gui_style_cache *style_cache; /* NULL until a style is asked for */
} tb_gui_node;

/* A theme a GUI draws by, and its generation when last looked at: a theme
 * that changed makes every style be resolved again. */
typedef struct tb_gui_theme_use
{
    const tb_theme *theme;
    size_t count; /* the widgets, and the GUI, that use it */
    uint64_t generation;
} tb_gui_theme_use;

/* A theme file watched for changes (gui_theme_file.c). */
typedef struct tb_gui_theme_watch tb_gui_theme_watch;

/* A popup shown, and how. */
typedef struct tb_gui_popup_entry
{
    tb_widget widget;
    tb_gui_popup_options options;
} tb_gui_popup_entry;

/* The variables, and the clipboard (gui_var.c). */
typedef struct tb_gui_values tb_gui_values;

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
    bool wants_mouse;
    bool wants_keyboard;
    tb_cursor cursor;

    /* Variables, and the clipboard; made when first used. */
    tb_gui_values *values;

    /* Styles (gui_style.c). */
    tb_theme *style_theme;   /* the caller's, borrowed; NULL for the default alone */
    tb_theme *default_theme; /* the tool theme of the palette, owned */
    uint64_t style_epoch;    /* grows when any style may have changed */
    tb_gui_theme_use *theme_uses;
    size_t theme_use_count;
    size_t theme_use_capacity;
    const tb_theme **theme_chain; /* the themes of the widget being resolved */
    size_t theme_chain_capacity;
    tb_gui_theme_watch *theme_watches;
    size_t theme_watch_count;
    size_t theme_watch_capacity;

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
/* The font of a spec against a base: the spec's face, or the base's when
 * the spec's weight is 0; at the spec's size, or the base's, or else the
 * palette's. */
tb_gui_resolved_font tb_gui_resolve_font(tb_gui *gui, tb_font_spec base, tb_font_spec spec);

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
/* A bar drawn by the style of the kind that draws it, which declares the
 * bar's elements (TB_GUI_BAR_STYLE_ELEMENTS); lit while `hot`. */
void tb_gui_bar_draw(tb_gui_paint *paint, const tb_gui_bar *bar, bool hot, const tb_style *style);
/* How wide a kind's bars are, by its style at rest. */
float tb_gui_bar_width(tb_widget widget);

/* The elements of a style whose kind draws scroll bars, among its others. */
#define TB_GUI_BAR_STYLE_ELEMENTS                                                                  \
    {"scrollbar_width", TB_STYLE_NUMBER}, {"scrollbar_inset", TB_STYLE_NUMBER},                    \
        {"scrollbar_trough", TB_STYLE_COLOR}, {"scrollbar_thumb", TB_STYLE_COLOR},                 \
    {                                                                                              \
        "scrollbar_thumb_hot", TB_STYLE_COLOR                                                      \
    }
#define TB_GUI_BAR_ELEMENTS 5u

/* An element of a style by its name, of a type: zero when it is not. */
tb_vec4 tb_gui_style_color_named(const tb_style *style, const char *name);
float tb_gui_style_number_named(const tb_style *style, const char *name);

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
/* The height of a line of a widget's font, in units. */
float tb_gui_line_height(tb_widget widget);

/* gui_var.c: variables polled against their settings, their observers run,
 * and their memory released with the GUI's. */
void tb_gui_vars_update(tb_gui *gui);
void tb_gui_vars_free(tb_gui *gui);

/* gui_values.c: the kinds that edit values, registered with the others. */
void tb_gui_register_value_kinds(tb_gui *gui);

/* gui_style.c: styles resolved for widgets. Made and freed with the GUI;
 * the default theme made again from the palette. */
void tb_gui_styles_init(tb_gui *gui, tb_theme *theme);
void tb_gui_styles_free(tb_gui *gui);
void tb_gui_styles_palette_changed(tb_gui *gui);
/* Look at the themes in use: any changed resolves every style again, and
 * lays the widgets out again. Called before each layout. */
void tb_gui_styles_refresh(tb_gui *gui);
/* A widget's styles are no longer what they were: its own changed. */
void tb_gui_style_forget(tb_gui *gui, uint32_t index);
/* A slot's styles, overrides, and theme released with it. */
void tb_gui_style_release(tb_gui *gui, uint32_t index);
/* The font of a text in a widget: a spec's face, or its style's when the
 * spec's weight is 0, at the spec's size, or its style's when it is 0. A
 * widget's style at rest gives its font in every state. */
tb_gui_resolved_font tb_gui_widget_font(tb_widget widget, tb_font_spec spec);

/* gui_theme_file.c: theme files watched, looked at each update. */
void tb_gui_theme_watches_update(tb_gui *gui);
void tb_gui_theme_watches_free(tb_gui *gui);

#endif
