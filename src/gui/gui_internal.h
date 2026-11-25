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
    bool shown; /* placed by the last layout: visible, gridded, and in a shown parent */
    /* Bindings. */
    tb_gui_binding *bindings;
    size_t binding_count;
    size_t binding_capacity;
    bool bindings_cleared; /* some were removed during events */
} tb_gui_node;

/* What a widget draws in its popup, above every widget: see
 * tb_gui_popup_open(). */
typedef void tb_gui_popup_draw(tb_widget owner, const void *data, tb_gui_paint *paint);

/* The variables, and the clipboard (gui_var.c). */
typedef struct tb_gui_values tb_gui_values;

/* A font opened from the catalog for a spec, or NULL when it has none. */
typedef struct tb_gui_font_entry
{
    tb_font_spec spec;
    tb_font *font;
} tb_gui_font_entry;

/* An object a layout file refers to by name: a command, by its value's
 * type. Each type is a namespace of its own. */
typedef struct tb_gui_named
{
    char *name; /* owned */
    tb_gui_value value;
} tb_gui_named;

/* A file whose widget is loaded again when it changes (gui_watch.c). */
typedef struct tb_gui_watch_entry tb_gui_watch_entry;

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
    uint32_t dispatch_depth;
    bool wants_mouse;
    bool wants_keyboard;
    tb_cursor cursor;

    /* The popup: a rectangle above every widget, which its owner draws and
     * takes the mouse in, such as a combo box's list. */
    tb_widget popup;
    tb_canvas_rect popup_rect;
    tb_gui_popup_draw *popup_draw;

    /* Variables, and the clipboard; made when first used. */
    tb_gui_values *values;

    /* What tb_gui_draw() draws. */
    tb_arena *arena;
    tb_ui_paint paint;

    /* Objects named for layout files, and the files watched. */
    tb_gui_named *names;
    size_t name_count;
    size_t name_capacity;
    tb_settings *settings; /* that layout files bind variables to; borrowed */
    tb_gui_watch_entry *watches;
    size_t watch_count;
    size_t watch_capacity;
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
/* Move a widget among its siblings, to just before another of them. */
void tb_gui_move_before(tb_gui *gui, uint32_t child, uint32_t sibling);

/* gui_property.c: the common properties. */
const tb_gui_property *tb_gui_property_find(const tb_gui_node *node, const char *name);

/* gui_layout.c: the grid. */
void tb_gui_layout(tb_gui *gui);
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
/* Open a popup a widget owns, over every widget and modal one: the mouse
 * finds the owner in it, a press outside it closes it, and `draw` draws it
 * with a paint of its rectangle. Closed too when its owner is hidden,
 * disabled, or destroyed. One is open at a time. */
void tb_gui_popup_open(tb_widget owner, tb_canvas_rect rect, tb_gui_popup_draw *draw);
void tb_gui_popup_close(tb_gui *gui);
bool tb_gui_popup_owned(const tb_gui *gui, tb_widget owner);

/* gui_draw.c: the commands of a frame. */
void tb_gui_build_commands(tb_gui *gui);

/* gui_widgets.c: frames, labels, and buttons. */
void tb_gui_register_builtins(tb_gui *gui);

/* gui_names.c: objects named for layout files. A type of value is named
 * when a file refers to its values by name rather than writing them. */
bool tb_gui_type_is_named(tb_gui_type type);
bool tb_gui_named_find(tb_gui *gui, tb_gui_type type, tb_text name, tb_gui_value *value);
/* The name of a value of a named type, that finds it; NULL when it has none. */
const char *tb_gui_named_name_of(tb_gui *gui, const tb_gui_value *value);
void tb_gui_names_free(tb_gui *gui);

/* gui_file.c: a file's bytes read through a VFS, with a terminator after
 * them, for the caller to free; the error says why not. */
struct tb_vfs;
tb_gui_file_result tb_gui_read_file(struct tb_vfs *vfs, const char *path, char **bytes,
                                    size_t *length, tb_gui_file_error *error);

/* gui_watch.c: files loaded again when they change. */
void tb_gui_watches_poll(tb_gui *gui);
void tb_gui_watches_free(tb_gui *gui);

/* gui_var.c: variables polled against their settings, their observers run,
 * and their memory released with the GUI's. */
void tb_gui_vars_update(tb_gui *gui);
void tb_gui_vars_free(tb_gui *gui);

/* gui_values.c: the kinds that edit values, registered with the others. */
void tb_gui_register_value_kinds(tb_gui *gui);

#endif
