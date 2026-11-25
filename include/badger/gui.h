/** @file gui.h
 * @brief A retained GUI, in the manner of Tkinter: widgets made once with
 * options, laid out by a grid, and run by the toolkit, with `command`
 * callbacks and bindings to events.
 *
 * @code
 * // once
 * tb_gui *const gui = tb_gui_create(&(tb_gui_options){.theme = &theme});
 * const tb_widget form = tb_frame(tb_gui_root(gui), &(tb_frame_desc){.padding = 12});
 * tb_grid(form, &(tb_grid_options){.sticky = TB_STICKY_NSEW});
 * tb_grid_column_configure(tb_gui_root(gui), 0, &(tb_grid_track){.weight = 1});
 * tb_grid_row_configure(tb_gui_root(gui), 0, &(tb_grid_track){.weight = 1});
 *
 * const tb_widget label = tb_label(form, &(tb_label_desc){.text = "Name"});
 * tb_grid(label, &(tb_grid_options){.row = 0, .column = 0, .sticky = TB_STICKY_W});
 * const tb_widget apply = tb_button(form, &(tb_button_desc){.text = "Apply",
 *                                                          .command = on_apply,
 *                                                          .user = game});
 * tb_grid(apply, &(tb_grid_options){.row = 1, .column = 1, .sticky = TB_STICKY_E});
 * tb_grid_column_configure(form, 1, &(tb_grid_track){.weight = 1});
 *
 * // for each window event
 * tb_gui_feed_event(gui, &event);
 *
 * // each frame
 * tb_gui_resize(gui, width, height, tb_canvas_view_logical(renderer).scale);
 * tb_gui_update(gui, delta_seconds);
 * tb_gui_draw(gui, renderer); // between tb_renderer_begin_frame() and tb_renderer_end_frame()
 * @endcode
 *
 * **Widgets** form a tree under the root, tb_gui_root(). Each is made once,
 * with its options, as a child of another, and lives until it, or an
 * ancestor, is destroyed. A widget is a handle, #tb_widget: an index and a
 * generation, safe to keep. A handle to a widget destroyed is stale: every
 * function given one does nothing and returns false, NULL, or zero, and
 * tb_widget_alive() says so. Destroying a widget destroys its children.
 *
 * **Kinds.** Each widget has a kind, #tb_widget_class: a table of functions
 * that measure, arrange, draw, and handle events, and a table of
 * properties. Frames, labels, and buttons are kinds of the engine's; a game
 * makes its own the same way, and registers them by name.
 *
 * **Properties** are plain data: every widget's common ones (its name,
 * whether it is visible and enabled, its style, its cursor, and its place
 * in its parent's grid), and its kind's. Each is enumerated, read, and set
 * by name (tb_widget_set()), which is what layout files and the editor
 * use; the functions of each kind are shorthand for the same.
 *
 * **Grid** is the only layout. A widget is shown once it is placed in its
 * parent's grid (tb_grid()), in a cell of a row and a column, spanning
 * more, stuck to the cell's edges or not, with padding. Rows and columns
 * take the size their widgets want; extra space goes to them by weight, or
 * the grid is placed by its anchor. See tb_grid().
 *
 * **Units and scaling.** Everything is laid out in units. A tool's
 * interface is in logical pixels at the display's scale. A game's is
 * designed at a reference size and stretched to the frame
 * (#tb_gui_stretch). Edges land on whole pixels at any scale; text is
 * measured and drawn at the frame's own pixels.
 *
 * **Events.** Window events are fed as they come (tb_gui_feed_event()) and
 * handled in tb_gui_update(), in order: the mouse finds the widget under
 * it, a press holds the widget pressed until its button comes up, Tab moves
 * the keyboard's focus, and callbacks run. See #tb_gui_event_kind.
 *
 * **Variables** (#tb_var) hold the values widgets edit: check buttons,
 * radio buttons, entries, text boxes, spin boxes, sliders, progress bars,
 * and combo boxes, several on one variable kept in step, and a variable
 * bound to an engine setting. See tb_var_create().
 *
 * **Layout files.** A widget tree is written as JSON (tb_gui_save()) and
 * built from it (tb_gui_load()), its commands and variables connected by
 * name, and found by path (tb_widget_find()); a file is loaded again as it
 * changes (tb_gui_watch()).
 *
 * **Not done yet.** Scrolling frames; menus; style classes; focus moved by
 * arrows or a gamepad.
 *
 * @par Threading
 * Externally synchronized: a GUI and its widgets are used by one thread at
 * a time; tb_gui_draw() on the thread that renders. GUIs are independent of
 * one another. Functions marked thread-safe may be called from any thread.
 */
#ifndef TB_GUI_H
#define TB_GUI_H

#include "badger/api.h"
#include "badger/canvas.h"
#include "badger/font_catalog.h"
#include "badger/math.h"
#include "badger/renderer.h"
#include "badger/stream.h"
#include "badger/text.h"
#include "badger/ui.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tb_vfs;

/** @brief A GUI: its widgets, their layout, and their input. Made by
 * tb_gui_create(); owned by the caller; destroyed by tb_gui_destroy(). */
typedef struct tb_gui tb_gui;

/** @brief A handle to a widget. Copy it freely; it goes stale, and is
 * detected as such, when its widget is destroyed. Zero is no widget. A
 * handle is not used after its GUI is destroyed. */
typedef struct tb_widget
{
    tb_gui *gui;         /**< Its GUI; NULL for no widget. */
    uint32_t index;      /**< Its slot. */
    uint32_t generation; /**< Its slot's generation when it was made; never 0. */
} tb_widget;

/** @brief No widget. */
#define TB_WIDGET_NONE ((tb_widget){NULL, 0u, 0u})

/** @brief A handle to a variable: a value that widgets show and edit, and
 * that callbacks watch, after Tk's `BooleanVar`, `IntVar`, `DoubleVar`, and
 * `StringVar`. Copy it freely; it goes stale, and is detected as such, when
 * its variable is destroyed. Zero is no variable. See tb_var_create(). */
typedef struct tb_var
{
    tb_gui *gui;         /**< Its GUI; NULL for no variable. */
    uint32_t index;      /**< Its slot. */
    uint32_t generation; /**< Its slot's generation when it was made; never 0. */
} tb_var;

/** @brief No variable. */
#define TB_VAR_NONE ((tb_var){NULL, 0u, 0u})

/** @brief A widget's kind: see #tb_widget_class. */
typedef struct tb_widget_class tb_widget_class;

/** @brief What a kind draws with, during tb_gui_update(): see
 * tb_gui_paint_fill(). */
typedef struct tb_gui_paint tb_gui_paint;

/** @brief An event: see #tb_gui_event_kind. */
typedef struct tb_gui_event tb_gui_event;

/* ---------------------------------------------------------------------------
 * Scaling.
 * ------------------------------------------------------------------------- */

/** @brief How a GUI designed at a reference size fills the frame, after
 * Godot's `content_scale` modes. */
typedef enum tb_gui_stretch
{
    /** Units are logical pixels: a unit is the display's scale in pixels,
     * times tb_gui_options::scale. The root is as large as the frame. For
     * tools. */
    TB_GUI_STRETCH_NONE,
    /** The reference size scaled to fit the frame, as large as it fits, and
     * centered: the frame's rest is left empty (letterboxed). */
    TB_GUI_STRETCH_KEEP_ASPECT,
    /** Scaled as the reference size fits, by the smaller of the two axes;
     * the other axis's extra is given to the root, whose grid lays it out. */
    TB_GUI_STRETCH_EXPAND,
    /** As keep-aspect, by a whole number of pixels a unit, at least 1, on
     * whole pixels: pixel art stays sharp and even. */
    TB_GUI_STRETCH_INTEGER
} tb_gui_stretch;

/** @brief How a GUI's units are placed on the frame. */
typedef struct tb_gui_view
{
    tb_vec2 origin; /**< The pixel of the frame where unit (0, 0) is: whole pixels. */
    float scale;    /**< Pixels a unit. */
    tb_vec2 size;   /**< The root's size, in units. */
} tb_gui_view;

/* ---------------------------------------------------------------------------
 * GUIs.
 * ------------------------------------------------------------------------- */

/** @brief The canvas layer widgets are drawn on, unless chosen; each modal
 * widget above is drawn on a layer of its own above it. */
#define TB_GUI_DEFAULT_LAYER 50

/** @brief How a GUI is made. Zero is every default. */
typedef struct tb_gui_options
{
    /** The theme: its colors, fonts, size, and spacing; copied. NULL is
     * tb_ui_theme_dark(), whose fonts are then given by tb_gui_set_theme()
     * before text is drawn. */
    const tb_ui_theme *theme;
    /** Optional: the catalog the fonts that widgets ask for by spec are
     * opened from (#tb_font_spec). Borrowed for the GUI's life. NULL draws
     * every text in the theme's font. */
    const tb_font_catalog *catalog;
    /** The size the GUI is designed at, in units; zero for none, which is
     * #TB_GUI_STRETCH_NONE whatever `stretch` says. */
    tb_vec2 reference_size;
    tb_gui_stretch stretch; /**< How the reference size fills the frame. */
    /** A factor on top of the stretch: 2 draws every unit twice as large,
     * and gives the root half as many units. 0 is 1. */
    float scale;
    int32_t layer; /**< The canvas layer; 0 is #TB_GUI_DEFAULT_LAYER. */
} tb_gui_options;

/**
 * @brief Make a GUI, with its root: a frame with no padding, as large as
 * the frame of pixels (tb_gui_resize()).
 * @param options Optional; NULL is every default.
 * @return The GUI; never NULL. Running out of memory ends the process.
 */
TB_API tb_gui *tb_gui_create(const tb_gui_options *options);

/**
 * @brief Destroy a GUI and every widget of it, without calling their
 * callbacks. NULL is ignored. A GUI that drew fonts it opened from its
 * catalog is given to tb_gui_forget_fonts() first.
 */
TB_API void tb_gui_destroy(tb_gui *gui);

/** @brief Use another theme; the widgets are laid out again.
 * @param theme Required; copied, its fonts borrowed. */
TB_API void tb_gui_set_theme(tb_gui *gui, const tb_ui_theme *theme);

/** @brief The theme in use. @return Borrowed until the theme is set again. */
TB_API const tb_ui_theme *tb_gui_theme(const tb_gui *gui);

/**
 * @brief Give the GUI the frame it fills, in pixels, and the display's
 * scale; called each frame, before tb_gui_update(). The widgets are laid
 * out again when the view changes.
 * @param width Pixels across (tb_renderer_size()).
 * @param height Pixels down.
 * @param display_scale The display's scale (tb_canvas_view_logical()); 0 is 1.
 */
TB_API void tb_gui_resize(tb_gui *gui, uint32_t width, uint32_t height, float display_scale);

/**
 * @brief Design the GUI at another reference size, or stretch it another
 * way; laid out again.
 * @param reference_size Units; zero for none.
 * @param stretch How it fills the frame.
 */
TB_API void tb_gui_set_stretch(tb_gui *gui, tb_vec2 reference_size, tb_gui_stretch stretch);

/** @brief Set the factor on top of the stretch (tb_gui_options::scale): an
 * options menu's interface scale. @param scale More than 0. */
TB_API void tb_gui_set_scale(tb_gui *gui, float scale);

/** @brief How the GUI's units are placed on the frame now. */
TB_API tb_gui_view tb_gui_view_of(const tb_gui *gui);

/**
 * @brief Give the GUI a window event, to be handled in the next
 * tb_gui_update(): the mouse's motion, buttons, and wheel, the mouse
 * leaving, keys, typed text, and the window losing the focus. Other events
 * are ignored. The event's `pixel_x` and `pixel_y` place the mouse.
 * @param event Required; copied.
 */
TB_API void tb_gui_feed_event(tb_gui *gui, const tb_window_event *event);

/** @brief tb_gui_feed_event() for each of `count` events, in order.
 * @param events Required when `count` is more than 0. */
TB_API void tb_gui_feed_events(tb_gui *gui, const tb_window_event *events, size_t count);

/**
 * @brief Run the GUI for a frame: lay the widgets out where something
 * changed, handle the events fed since the last update in their order,
 * running callbacks as they come, and build what tb_gui_draw() draws.
 *
 * Callbacks run here, one at a time, never inside another widget's
 * handling of the tree: a callback may make, change, and destroy widgets,
 * itself and the widget it was called for included. A widget destroyed in
 * a callback gets no more events; its memory is released when the event's
 * callbacks are done.
 *
 * @param delta_seconds Seconds since the last update: double clicks.
 */
TB_API void tb_gui_update(tb_gui *gui, double delta_seconds);

/**
 * @brief What the last update built to draw: commands in units, in the
 * order drawn, as #tb_ui_command describes them.
 * @param[out] count Required; how many.
 * @return Valid until the next update.
 */
TB_API const tb_ui_command *tb_gui_commands(const tb_gui *gui, size_t *count);

/**
 * @brief Draw what the last update built on a renderer's canvas, between
 * tb_renderer_begin_frame() and tb_renderer_end_frame().
 * @param renderer Required.
 * @par Threading Stage-owned: on the thread that renders.
 */
TB_API void tb_gui_draw(const tb_gui *gui, tb_renderer *renderer);

/** @brief Tell a renderer's canvas to forget the fonts the GUI opened from
 * its catalog, before the GUI closes them in tb_gui_destroy().
 * @param renderer Required.
 * @par Threading Stage-owned: on the thread that renders. */
TB_API void tb_gui_forget_fonts(const tb_gui *gui, tb_renderer *renderer);

/** @brief Whether the GUI takes the mouse: it is over a widget that stops
 * it (#TB_GUI_MOUSE_STOP), a widget holds it, or a modal widget is shown.
 * As of the last update. */
TB_API bool tb_gui_wants_mouse(const tb_gui *gui);

/** @brief Whether the GUI takes the keyboard: a widget has the focus. As of
 * the last update. */
TB_API bool tb_gui_wants_keyboard(const tb_gui *gui);

/** @brief The cursor the mouse should show, for tb_window_set_cursor(): the
 * widget's under it, or its nearest ancestor's that chose one. */
TB_API tb_cursor tb_gui_cursor(const tb_gui *gui);

/** @brief The root: the frame every widget is in. */
TB_API tb_widget tb_gui_root(tb_gui *gui);

/** @brief How a GUI is doing: for tests, and statistics. */
typedef struct tb_gui_stats
{
    size_t widgets;    /**< Alive, the root included. */
    size_t layouts;    /**< Times the widgets were laid out since the GUI was made. */
    size_t commands;   /**< Built by the last update. */
    tb_widget hovered; /**< The widget under the mouse, or none. */
    tb_widget pressed; /**< The widget a button is held on, or none. */
    tb_widget focus;   /**< The widget with the keyboard's focus, or none. */
    tb_widget modal;   /**< The modal widget in front, or none. */
} tb_gui_stats;

/** @brief How the GUI is doing. @param[out] stats Required. */
TB_API void tb_gui_stats_of(const tb_gui *gui, tb_gui_stats *stats);

/* ---------------------------------------------------------------------------
 * Values and properties.
 * ------------------------------------------------------------------------- */

/** @brief The type of a property's value. */
typedef enum tb_gui_type
{
    TB_GUI_TYPE_BOOL,     /**< `bool`. */
    TB_GUI_TYPE_INT,      /**< `int32_t`. */
    TB_GUI_TYPE_FLOAT,    /**< `float`. */
    TB_GUI_TYPE_VEC2,     /**< #tb_vec2: a pair, such as a left and a right. */
    TB_GUI_TYPE_COLOR,    /**< #tb_vec4: RGBA, straight alpha. */
    TB_GUI_TYPE_TEXT,     /**< UTF-8, held in a kind's data as a #tb_gui_string. */
    TB_GUI_TYPE_ENUM,     /**< `int32_t`, one of the property's names. */
    TB_GUI_TYPE_FLAGS,    /**< `uint32_t`, a bit for each of the property's names. */
    TB_GUI_TYPE_FONT,     /**< #tb_font_spec. */
    TB_GUI_TYPE_COMMAND,  /**< #tb_gui_command: a callback and its argument. */
    TB_GUI_TYPE_VARIABLE, /**< #tb_var: a variable the widget shows and edits; none for none. */
    TB_GUI_TYPE_COUNT     /**< How many there are; not a type. */
} tb_gui_type;

/** @brief Text a widget owns: the GUI copies it in when it is set, and
 * releases it when it is set again or the widget is destroyed. */
typedef struct tb_gui_string
{
    char *bytes;   /**< UTF-8, terminated; NULL for none. */
    size_t length; /**< Bytes, the terminator not counted. */
} tb_gui_string;

/** @brief An event's callback: see tb_bind().
 * @param event The event; borrowed for the call. @param user The binding's. */
typedef void tb_gui_callback(const tb_gui_event *event, void *user);

/** @brief A callback and its argument, as a property holds them. */
typedef struct tb_gui_command
{
    tb_gui_callback *callback; /**< NULL for none. */
    void *user;                /**< Given to the callback. */
} tb_gui_command;

/** @brief A property's value: `type` names the member read. */
typedef struct tb_gui_value
{
    tb_gui_type type;
    union {
        bool boolean;           /**< #TB_GUI_TYPE_BOOL. */
        int32_t integer;        /**< #TB_GUI_TYPE_INT and #TB_GUI_TYPE_ENUM. */
        uint32_t flags;         /**< #TB_GUI_TYPE_FLAGS. */
        float number;           /**< #TB_GUI_TYPE_FLOAT. */
        tb_vec2 vec2;           /**< #TB_GUI_TYPE_VEC2. */
        tb_vec4 color;          /**< #TB_GUI_TYPE_COLOR. */
        tb_text text;           /**< #TB_GUI_TYPE_TEXT; read, borrowed until it changes. */
        tb_font_spec font;      /**< #TB_GUI_TYPE_FONT. */
        tb_gui_command command; /**< #TB_GUI_TYPE_COMMAND. */
        tb_var variable;        /**< #TB_GUI_TYPE_VARIABLE. */
    };
} tb_gui_value;

/** @brief What a property is, besides its value. */
typedef enum tb_gui_property_flags
{
    /** Changing it changes a size: the widgets are laid out again. */
    TB_GUI_PROPERTY_LAYOUT = 1u << 0,
    /** It is read and never set by name. */
    TB_GUI_PROPERTY_READ_ONLY = 1u << 1
} tb_gui_property_flags;

/**
 * @brief A property of a kind of widget: its name and type, and where its
 * value is in the kind's data, or the functions that read and set it.
 *
 * A property at an offset is read and set by the GUI itself: a value of
 * its type at `offset` bytes into the widget's data, a #tb_gui_string for
 * text, whose bytes the GUI owns. An enumeration is checked against its
 * names, and flags against their bits.
 */
typedef struct tb_gui_property
{
    const char *name; /**< Unique among the kind's and the common properties. */
    tb_gui_type type;
    size_t offset;  /**< Of the value in the kind's data, when `get` and `set` are NULL. */
    uint32_t flags; /**< #tb_gui_property_flags. */
    /** For an enumeration, the name of each value, from 0; for flags, of
     * each bit, from the lowest. NULL otherwise. Layout files write them. */
    const char *const *names;
    size_t name_count; /**< How many names. */
    /** Optional: read the value, in place of the offset. Returns false when
     * there is none. `data` is the widget's kind's data. */
    bool (*get)(tb_widget widget, const void *data, tb_gui_value *value);
    /** Optional: set the value, in place of the offset, already checked to
     * be of the type. Returns false when it is refused. */
    bool (*set)(tb_widget widget, void *data, const tb_gui_value *value);
} tb_gui_property;

/**
 * @brief The properties every widget has, before its kind's: `name`,
 * `style`, `visible`, `enabled`, `cursor`, `mouse_filter`, the grid's
 * `gridded`, `row`, `column`, `row_span`, `column_span`, `sticky`, `padx`,
 * `pady`, `ipadx`, `ipady`, and, of a widget's own grid, `grid_anchor`.
 * @param[out] count Required; how many.
 * @return Static.
 * @par Threading Thread-safe.
 */
TB_API const tb_gui_property *tb_gui_common_properties(size_t *count);

/** @brief How many properties a widget has: the common ones, then its
 * kind's. 0 for a stale handle. */
TB_API size_t tb_widget_property_count(tb_widget widget);

/** @brief A widget's property, by index, below tb_widget_property_count().
 * @return Static, or the kind's; NULL past the end or for a stale handle. */
TB_API const tb_gui_property *tb_widget_property_at(tb_widget widget, size_t index);

/** @brief A widget's property, by name. @param name Required.
 * @return NULL when it has none of that name, or the handle is stale. */
TB_API const tb_gui_property *tb_widget_find_property(tb_widget widget, const char *name);

/**
 * @brief Read a widget's property by name.
 * @param name Required.
 * @param[out] value Required; its type and value. Text is borrowed until it
 * is set again or the widget is destroyed.
 * @return false when it has none of that name, or the handle is stale.
 */
TB_API bool tb_widget_get(tb_widget widget, const char *name, tb_gui_value *value);

/**
 * @brief Set a widget's property by name.
 * @param name Required.
 * @param value Required; of the property's type. Text is copied.
 * @return false, changing nothing, when it has none of that name, the
 * property is read-only, the value is of another type or out of its
 * range, or the handle is stale.
 */
TB_API bool tb_widget_set(tb_widget widget, const char *name, const tb_gui_value *value);

/** @brief A value of each type, for tb_widget_set(). @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_bool(bool value)
{
    tb_gui_value made = {TB_GUI_TYPE_BOOL, {.boolean = value}};
    return made;
}

/** @brief An integer. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_int(int32_t value)
{
    tb_gui_value made = {TB_GUI_TYPE_INT, {.integer = value}};
    return made;
}

/** @brief A number. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_float(float value)
{
    tb_gui_value made = {TB_GUI_TYPE_FLOAT, {.number = value}};
    return made;
}

/** @brief A pair. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_vec2(tb_vec2 value)
{
    tb_gui_value made = {TB_GUI_TYPE_VEC2, {.vec2 = value}};
    return made;
}

/** @brief A color. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_color(tb_vec4 value)
{
    tb_gui_value made = {TB_GUI_TYPE_COLOR, {.color = value}};
    return made;
}

/** @brief Text, borrowed until it is set. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_text(tb_text value)
{
    tb_gui_value made = {TB_GUI_TYPE_TEXT, {.text = value}};
    return made;
}

/** @brief One of an enumeration's values. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_enum(int32_t value)
{
    tb_gui_value made = {TB_GUI_TYPE_ENUM, {.integer = value}};
    return made;
}

/** @brief Flags. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_flags(uint32_t value)
{
    tb_gui_value made = {TB_GUI_TYPE_FLAGS, {.flags = value}};
    return made;
}

/** @brief A font. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_font(tb_font_spec value)
{
    tb_gui_value made = {TB_GUI_TYPE_FONT, {.font = value}};
    return made;
}

/** @brief A callback. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_command_of(tb_gui_callback *callback, void *user)
{
    tb_gui_value made = {TB_GUI_TYPE_COMMAND, {.command = {callback, user}}};
    return made;
}

/** @brief A variable, or none. @par Threading Thread-safe. */
static inline tb_gui_value tb_gui_variable(tb_var value)
{
    tb_gui_value made = {TB_GUI_TYPE_VARIABLE, {.variable = value}};
    return made;
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

/** @brief What happened to a widget. */
typedef enum tb_gui_event_kind
{
    TB_GUI_EVENT_ENTER,        /**< The mouse came over it. */
    TB_GUI_EVENT_LEAVE,        /**< The mouse left it. */
    TB_GUI_EVENT_PRESS,        /**< A mouse button went down on it. */
    TB_GUI_EVENT_RELEASE,      /**< The button pressed on it came up, wherever the mouse is. */
    TB_GUI_EVENT_CLICK,        /**< The button pressed on it came up over it. */
    TB_GUI_EVENT_DOUBLE_CLICK, /**< A second press of a button on it, soon after and near the first.
                                */
    TB_GUI_EVENT_WHEEL,        /**< The wheel turned over it; given to its ancestors too. */
    TB_GUI_EVENT_KEY,          /**< A key went down, or repeats, while it has the focus; given
                                    to its ancestors too. Not Tab, which moves the focus. */
    TB_GUI_EVENT_TEXT,         /**< Text was typed while it has the focus. */
    TB_GUI_EVENT_FOCUS_IN,     /**< It took the keyboard's focus. */
    TB_GUI_EVENT_FOCUS_OUT,    /**< It lost the focus. */
    TB_GUI_EVENT_RESIZE,       /**< Its size changed when it was laid out. */
    TB_GUI_EVENT_COMMAND,      /**< It was invoked: a button clicked, or tb_widget_invoke(). */
    /** Its value changed through input: a check button toggled, text typed,
     * a slider dragged, an item chosen; or tb_widget_invoke(), which acts as
     * input does. Not when a program sets it, nor its variable. */
    TB_GUI_EVENT_CHANGED,
    TB_GUI_EVENT_MOTION, /**< The mouse moved while a button pressed on it is held: a drag. */
    /** Text is about to change, Tk's `validatecommand`: `text` is the text
     * proposed, and tb_gui_refuse_edit() keeps the old. */
    TB_GUI_EVENT_VALIDATE,
    TB_GUI_EVENT_COUNT /**< How many there are; not a kind. */
} tb_gui_event_kind;

/** @brief An event, as callbacks and kinds are given it. */
struct tb_gui_event
{
    tb_gui_event_kind kind;
    tb_widget widget; /**< The widget whose bindings and kind are called. */
    /** The widget it happened to: `widget`, or a descendant of it for an
     * event given to ancestors too. */
    tb_widget target;
    tb_vec2 position;       /**< The mouse, in units of the GUI. */
    tb_vec2 local;          /**< The mouse, in units from `widget`'s top left. */
    tb_mouse_button button; /**< For presses, releases, and clicks: which. */
    tb_vec2 wheel;          /**< For the wheel: notches; positive `y` scrolls up. */
    tb_key key;             /**< For a key. */
    bool repeat;            /**< For a key: the system repeats it, held. */
    uint32_t modifiers;     /**< The #tb_modifier bits held. */
    const char *text;       /**< For text: UTF-8, terminated; borrowed for the call. */
    tb_canvas_rect rect;    /**< For a resize: the widget's new place and size. */
};

/**
 * @brief Call a function when an event happens to a widget, after the
 * functions bound before it and before the widget's kind handles it.
 * Several may be bound to one event.
 * @param kind Which event.
 * @param callback Required.
 * @param user Given to it.
 * @return false for a stale handle.
 */
TB_API bool tb_bind(tb_widget widget, tb_gui_event_kind kind, tb_gui_callback *callback,
                    void *user);

/**
 * @brief Stop calling functions bound to a widget's event.
 * @param kind Which event.
 * @param callback The function bound; NULL for every function bound to it.
 * @param user The argument it was bound with; ignored when `callback` is NULL.
 * @return How many bindings were removed.
 */
TB_API size_t tb_unbind(tb_widget widget, tb_gui_event_kind kind, tb_gui_callback *callback,
                        void *user);

/** @brief Invoke a widget now, as clicking a button does: its #TB_GUI_EVENT_COMMAND
 * bindings, then its kind, which calls its `command`.
 * @return false for a stale handle, or a widget disabled. */
TB_API bool tb_widget_invoke(tb_widget widget);

/* ---------------------------------------------------------------------------
 * Kinds of widgets.
 * ------------------------------------------------------------------------- */

/** @brief How a widget takes the mouse, after Godot's `mouse_filter`. */
typedef enum tb_gui_mouse_filter
{
    /** It takes the mouse's events, and keeps them from the game. */
    TB_GUI_MOUSE_STOP,
    /** It takes the mouse's events, but leaves the mouse to the game: a
     * frame that only lays others out. */
    TB_GUI_MOUSE_PASS,
    /** The mouse finds what is under it, never it; its children are found. */
    TB_GUI_MOUSE_IGNORE
} tb_gui_mouse_filter;

/** @brief What a kind of widget does. */
typedef enum tb_widget_class_flags
{
    /** Tab gives it the focus. */
    TB_WIDGET_CLASS_FOCUSABLE = 1u << 0,
    /** A press of the left button gives it the focus: a field to type in. */
    TB_WIDGET_CLASS_FOCUS_ON_CLICK = 1u << 1
} tb_widget_class_flags;

/** @brief Space between a widget's edges and the grid of its children, in units. */
typedef struct tb_gui_edges
{
    float left;
    float top;
    float right;
    float bottom;
} tb_gui_edges;

/**
 * @brief A kind of widget: a class, whose functions the GUI calls for each
 * widget of it, with the widget's data. Every function may be NULL.
 *
 * A widget's natural size is the larger of what its kind measures and what
 * the grid of its children wants, with its insets around it; its parent's
 * grid gives it a place from that.
 */
struct tb_widget_class
{
    const char *name; /**< Unique among those a GUI registers: "button". */
    size_t data_size; /**< Bytes of data each widget of it has, zeroed when it is made. */
    const tb_gui_property *properties; /**< Its properties, besides the common ones. */
    size_t property_count;
    uint32_t flags;                   /**< #tb_widget_class_flags. */
    tb_gui_mouse_filter mouse_filter; /**< Its widgets' filter when they are made. */
    /** Give a widget made its first values. */
    void (*init)(tb_widget widget, void *data);
    /** Release what a widget's data holds, besides its text properties,
     * which the GUI releases. */
    void (*destroy)(tb_widget widget, void *data);
    /** The size its content wants, in units, without its insets. */
    tb_vec2 (*measure)(tb_widget widget, const void *data);
    /** The space between its edges and its children's grid. */
    tb_gui_edges (*insets)(tb_widget widget, const void *data);
    /** It was given a place and size, before its children were. */
    void (*arrange)(tb_widget widget, void *data, tb_canvas_rect rect);
    /** Draw it, under its children (tb_gui_paint_fill()). */
    void (*draw)(tb_widget widget, const void *data, tb_gui_paint *paint);
    /** Handle an event, after its bindings. Returns true when it has used
     * it: a key or the wheel is then not given to its ancestors. */
    bool (*event)(tb_widget widget, void *data, const tb_gui_event *event);
};

/**
 * @brief Register a kind, so that it is found by its name: by layout
 * files, and the editor. The engine's are registered when a GUI is made.
 * @param kind Required; borrowed for the GUI's life.
 * @return false when another of its name is registered.
 */
TB_API bool tb_gui_register_class(tb_gui *gui, const tb_widget_class *kind);

/** @brief A registered kind, by name. @param name Required.
 * @return NULL when none is registered by that name. */
TB_API const tb_widget_class *tb_gui_find_class(const tb_gui *gui, const char *name);

/** @brief How many kinds are registered. */
TB_API size_t tb_gui_class_count(const tb_gui *gui);

/** @brief A registered kind, by index, in the order registered.
 * @return NULL past the end. */
TB_API const tb_widget_class *tb_gui_class_at(const tb_gui *gui, size_t index);

/** @brief The engine's kinds. @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_frame_class(void);
/** @brief See tb_frame_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_label_class(void);
/** @brief See tb_frame_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_button_class(void);

/* ---------------------------------------------------------------------------
 * Widgets.
 * ------------------------------------------------------------------------- */

/**
 * @brief Make a widget of a kind, the last child of another. It is not
 * shown until it is placed in its parent's grid (tb_grid()).
 * @param parent A widget alive.
 * @param kind Required; outlives the widget.
 * @return The widget; none when `parent` is stale.
 */
TB_API tb_widget tb_widget_create(tb_widget parent, const tb_widget_class *kind);

/**
 * @brief Destroy a widget and its children, its children first. Their
 * handles go stale at once. The root is never destroyed: it is emptied.
 */
TB_API void tb_widget_destroy(tb_widget widget);

/** @brief Whether a handle is of a widget alive. */
TB_API bool tb_widget_alive(tb_widget widget);

/** @brief Whether two handles are of one widget. @par Threading Thread-safe. */
TB_API bool tb_widget_equal(tb_widget a, tb_widget b);

/** @brief A widget's parent; none for the root. */
TB_API tb_widget tb_widget_parent(tb_widget widget);

/** @brief A widget's first child, the first made; none when it has none. */
TB_API tb_widget tb_widget_first_child(tb_widget widget);

/** @brief The child of its parent after it; none for the last. */
TB_API tb_widget tb_widget_next_sibling(tb_widget widget);

/** @brief How many children a widget has. */
TB_API size_t tb_widget_child_count(tb_widget widget);

/** @brief A widget's kind. */
TB_API const tb_widget_class *tb_widget_class_of(tb_widget widget);

/** @brief A widget's data, of its kind's size: for a kind's own functions.
 * @return Valid while the widget is alive; NULL for a stale handle or a kind
 * of no data. */
TB_API void *tb_widget_data(tb_widget widget);

/** @brief Where a widget is and how large, in units of the GUI, as the last
 * layout placed it; zero when it is not shown. */
TB_API tb_canvas_rect tb_widget_rect(tb_widget widget);

/** @brief The size a widget wants, its insets included, as the last layout
 * measured it. */
TB_API tb_vec2 tb_widget_natural_size(tb_widget widget);

/** @brief What a widget is doing now, for drawing it. */
typedef enum tb_widget_state_flags
{
    TB_WIDGET_HOVERED = 1u << 0, /**< The mouse is over it. */
    TB_WIDGET_PRESSED = 1u << 1, /**< A button went down on it and is held. */
    TB_WIDGET_FOCUSED = 1u << 2, /**< It has the keyboard's focus. */
    TB_WIDGET_DISABLED = 1u << 3 /**< It, or an ancestor, is disabled. */
} tb_widget_state_flags;

/** @brief What a widget is doing: #tb_widget_state_flags. */
TB_API uint32_t tb_widget_state(tb_widget widget);

/** @brief Set a widget's text: its `text` property. @param text Required;
 * copied. @return false when it has none, or the handle is stale. */
TB_API bool tb_widget_set_text(tb_widget widget, const char *text);

/** @brief A widget's text. @return Borrowed until it is set again; empty
 * when it has none. */
TB_API tb_text tb_widget_text(tb_widget widget);

/** @brief Enable a widget or disable it: a disabled widget, and every
 * widget in it, takes no events, no focus, and is drawn dimmed. */
TB_API void tb_widget_set_enabled(tb_widget widget, bool enabled);

/** @brief Whether a widget is enabled itself, its ancestors aside. */
TB_API bool tb_widget_enabled(tb_widget widget);

/** @brief Show a widget or hide it: a hidden widget, and every widget in
 * it, is neither drawn, found by the mouse, nor given space by the grid. */
TB_API void tb_widget_set_visible(tb_widget widget, bool visible);

/** @brief Whether a widget is visible itself, its ancestors aside. */
TB_API bool tb_widget_visible(tb_widget widget);

/** @brief Name a widget's style, for themes' style classes.
 * @param style Required; copied; "" for none. */
TB_API void tb_widget_set_style(tb_widget widget, const char *style);

/** @brief A widget's style. @return Borrowed until it is set again. */
TB_API tb_text tb_widget_style(tb_widget widget);

/** @brief Name a widget, for finding it by path. @param name Required; copied. */
TB_API void tb_widget_set_name(tb_widget widget, const char *name);

/** @brief A widget's name. @return Borrowed until it is set again. */
TB_API tb_text tb_widget_name(tb_widget widget);

/** @brief Choose the cursor shown over a widget and the widgets in it that
 * choose none. #TB_CURSOR_COUNT chooses none: its parent's. */
TB_API void tb_widget_set_cursor(tb_widget widget, tb_cursor cursor);

/** @brief Choose how a widget takes the mouse. */
TB_API void tb_widget_set_mouse_filter(tb_widget widget, tb_gui_mouse_filter filter);

/**
 * @brief Give a widget the keyboard's focus.
 * @return false, changing nothing, when it is stale, its kind takes no
 * focus, or it is hidden, disabled, or behind a modal widget.
 */
TB_API bool tb_widget_focus(tb_widget widget);

/** @brief Take the focus from whichever widget has it. */
TB_API void tb_gui_clear_focus(tb_gui *gui);

/** @brief Move the focus as Tab does: to the next widget that takes it, in
 * the order of the tree, around from the last to the first; or the one
 * before. Within the modal widget in front, when one is shown. */
TB_API void tb_gui_focus_next(tb_gui *gui, bool backward);

/**
 * @brief Show a widget modally: in front of every other, on a layer above
 * them, over a shade, and alone in taking input until tb_widget_end_modal(),
 * or until it is hidden or destroyed. Modal widgets stack; the last shown
 * is in front. The focus leaves widgets behind it.
 * @return false for a stale handle or the root.
 */
TB_API bool tb_widget_show_modal(tb_widget widget);

/** @brief End a widget's being modal; the widget stays. */
TB_API void tb_widget_end_modal(tb_widget widget);

/* ---------------------------------------------------------------------------
 * The grid.
 * ------------------------------------------------------------------------- */

/** @brief The edges of its cell a widget is stuck to, Tk's `sticky`. */
typedef enum tb_sticky
{
    TB_STICKY_N = 1u << 0,                       /**< The top. */
    TB_STICKY_S = 1u << 1,                       /**< The bottom. */
    TB_STICKY_E = 1u << 2,                       /**< The right. */
    TB_STICKY_W = 1u << 3,                       /**< The left. */
    TB_STICKY_NS = TB_STICKY_N | TB_STICKY_S,    /**< Stretched down the cell. */
    TB_STICKY_EW = TB_STICKY_E | TB_STICKY_W,    /**< Stretched across it. */
    TB_STICKY_NSEW = TB_STICKY_NS | TB_STICKY_EW /**< Filling it. */
} tb_sticky;

/** @brief A point of a rectangle: its middle, an edge's middle, or a corner,
 * by the compass. */
typedef enum tb_gui_anchor
{
    TB_GUI_ANCHOR_CENTER,
    TB_GUI_ANCHOR_N,
    TB_GUI_ANCHOR_NE,
    TB_GUI_ANCHOR_E,
    TB_GUI_ANCHOR_SE,
    TB_GUI_ANCHOR_S,
    TB_GUI_ANCHOR_SW,
    TB_GUI_ANCHOR_W,
    TB_GUI_ANCHOR_NW
} tb_gui_anchor;

/** @brief Where a widget is in its parent's grid. Zero is the first row's
 * first cell, a cell's size, centered, with no padding. */
typedef struct tb_grid_options
{
    int32_t row;         /**< From 0. */
    int32_t column;      /**< From 0. */
    int32_t row_span;    /**< Rows it spans; 0 is 1. */
    int32_t column_span; /**< Columns it spans; 0 is 1. */
    uint32_t sticky;     /**< #tb_sticky: the edges it is stuck to; none centers it. */
    tb_vec2 padx;        /**< Space outside it on its left (`x`) and right (`y`). */
    tb_vec2 pady;        /**< Space outside it above (`x`) and below (`y`). */
    float ipadx;         /**< Space added inside it on its left and its right. */
    float ipady;         /**< Space added inside it above and below. */
} tb_grid_options;

/**
 * @brief Place a widget in its parent's grid, and show it.
 *
 * Rows and columns are as large as the widgets wholly in them want: each
 * widget's natural size, with its inner padding twice and its outer
 * padding; then a row's or column's pad, and at least its minimum size. A
 * widget spanning several that are too small for it widens them, by their
 * weights, or evenly when none has weight. Rows and columns of a uniform
 * group are then made as large as one another, in proportion to their
 * weights (a weight of 0 counts as 1).
 *
 * When the parent is larger than its grid, the extra is shared out by the
 * weights; when none has weight, the grid is placed by the parent's anchor
 * (tb_grid_anchor()). When it is smaller, the rows and columns of weight
 * give it back by their weights, down to their minimum sizes, and what is
 * still too large is cut off at the right and bottom.
 *
 * In its cell, a widget is its natural size, placed in the middle, unless
 * stuck to edges: to two opposite edges, it is stretched between them.
 * Every edge is placed on a whole pixel, so that cells side by side meet
 * with neither gap nor overlap at any scale.
 *
 * @param options Optional; NULL is zero.
 * @return false for a stale handle, the root, or a row or column below 0.
 */
TB_API bool tb_grid(tb_widget widget, const tb_grid_options *options);

/** @brief Take a widget out of its parent's grid: it is hidden, and its
 * options forgotten. */
TB_API void tb_grid_forget(tb_widget widget);

/** @brief Where a widget is in its parent's grid.
 * @param[out] options Required. @return false when it is not in it. */
TB_API bool tb_grid_info(tb_widget widget, tb_grid_options *options);

/** @brief How a row or column of a grid takes space. Zero takes what its
 * widgets want and no more. */
typedef struct tb_grid_track
{
    float weight;  /**< Its share of extra space, and of space given back; 0 for none. */
    float minsize; /**< The least it is, in units. */
    float pad;     /**< Space added to it when it has a widget, half on each side. */
    /** A uniform group: rows or columns of one name are sized in proportion
     * to their weights. NULL or "" for none; copied. */
    const char *uniform;
} tb_grid_track;

/** @brief Configure a row of a widget's grid; the rows before it, and it,
 * exist from then on, even empty. @param track Required; copied.
 * @return false for a stale handle. */
TB_API bool tb_grid_row_configure(tb_widget container, size_t row, const tb_grid_track *track);

/** @brief Configure a column of a widget's grid. As tb_grid_row_configure(). */
TB_API bool tb_grid_column_configure(tb_widget container, size_t column,
                                     const tb_grid_track *track);

/** @brief Place a grid within its widget when it has extra space and no
 * weights to give it, Tk's `grid anchor`. #TB_GUI_ANCHOR_NW unless chosen. */
TB_API void tb_grid_anchor(tb_widget container, tb_gui_anchor anchor);

/** @brief How many columns and rows a widget's grid has, as last laid out:
 * those of its widgets and those configured. */
TB_API void tb_grid_size(tb_widget container, size_t *columns, size_t *rows);

/**
 * @brief The rectangle of cells of a widget's grid, as last laid out, Tk's
 * `grid bbox`: from the cell at `column`, `row` to the cell at `column2`,
 * `row2`, both included, in units of the GUI.
 * @param[out] rect Required.
 * @return false when the cells are not in the grid.
 */
TB_API bool tb_grid_bbox(tb_widget container, size_t column, size_t row, size_t column2,
                         size_t row2, tb_canvas_rect *rect);

/* ---------------------------------------------------------------------------
 * Drawing, for kinds.
 * ------------------------------------------------------------------------- */

/** @brief Text, and how it is set in a rectangle. */
typedef struct tb_gui_text_block
{
    tb_text text; /**< UTF-8; `\n` breaks lines. */
    /** The font: a weight of 0 is the theme's font; a size of 0 the
     * theme's size. Opened from the GUI's catalog. */
    tb_font_spec font;
    float wrap_length;     /**< Units lines wrap at; 0 for none. */
    tb_font_align justify; /**< How lines sit against one another. */
    tb_gui_anchor anchor;  /**< Where the text sits in the rectangle. */
} tb_gui_text_block;

/** @brief The size text takes, in units, measured at the frame's pixels.
 * @param block Required. */
TB_API tb_vec2 tb_gui_measure_text(tb_gui *gui, const tb_gui_text_block *block);

/** @brief The widget being drawn's place and size, in units. @param paint Required. */
TB_API tb_canvas_rect tb_gui_paint_rect(const tb_gui_paint *paint);

/** @brief Fill a rectangle, its corners rounded by `radius`, within the
 * widget's clip. @param paint Required. */
TB_API void tb_gui_paint_fill(tb_gui_paint *paint, tb_canvas_rect rect, float radius,
                              tb_vec4 color);

/** @brief Outline a rectangle inside its edges, `thickness` units wide, at
 * least a pixel. @param paint Required. */
TB_API void tb_gui_paint_outline(tb_gui_paint *paint, tb_canvas_rect rect, float radius,
                                 float thickness, tb_vec4 color);

/** @brief A line. @param paint Required. */
TB_API void tb_gui_paint_line(tb_gui_paint *paint, tb_vec2 from, tb_vec2 to, float thickness,
                              tb_vec4 color);

/** @brief Text set in a rectangle and clipped to it: a line by its capitals'
 * middle when it is anchored at the middle, as labels and buttons beside
 * one another line up; text wider than the rectangle from its left edge,
 * cut off at the right. @param paint Required. @param block Required. */
TB_API void tb_gui_paint_text(tb_gui_paint *paint, const tb_gui_text_block *block,
                              tb_canvas_rect rect, tb_vec4 color);

/* ---------------------------------------------------------------------------
 * Frames, labels, and buttons.
 * ------------------------------------------------------------------------- */

/** @brief A frame: a container of widgets in its grid. Properties:
 * `padding`, `background`, `border`. */
typedef struct tb_frame_desc
{
    float padding;   /**< Units between its edges and its grid. */
    bool background; /**< Filled with the theme's window color; it then stops the mouse. */
    bool border;     /**< Outlined with the theme's border. */
} tb_frame_desc;

/** @brief Make a frame. @param desc Optional; NULL is zero. @return None
 * when `parent` is stale. */
TB_API tb_widget tb_frame(tb_widget parent, const tb_frame_desc *desc);

/** @brief A label: text that is read. Properties: `text`, `wrap_length`,
 * `justify`, `anchor`, `font`. */
typedef struct tb_label_desc
{
    const char *text;      /**< Copied; NULL for none. */
    float wrap_length;     /**< Units its lines wrap at; 0 for none. */
    tb_font_align justify; /**< How its lines sit against one another. */
    tb_gui_anchor anchor;  /**< Where its text sits when it is larger. */
    tb_font_spec font;     /**< Its font; zero is the theme's. */
} tb_label_desc;

/** @brief Make a label. @param desc Optional. @return None when `parent` is stale. */
TB_API tb_widget tb_label(tb_widget parent, const tb_label_desc *desc);

/** @brief A button: text that is clicked, or pressed with Space or Enter
 * while it has the focus, to call its command. Properties: `text`,
 * `command`, `default`. */
typedef struct tb_button_desc
{
    const char *text;         /**< Copied; NULL for none. */
    tb_gui_callback *command; /**< Called when it is invoked; NULL for none. */
    void *user;               /**< Given to the command. */
    bool is_default;          /**< Drawn as a dialog's default button, ringed by the accent. */
} tb_button_desc;

/** @brief Make a button. @param desc Optional. @return None when `parent` is stale. */
TB_API tb_widget tb_button(tb_widget parent, const tb_button_desc *desc);

/* ---------------------------------------------------------------------------
 * Variables.
 * ------------------------------------------------------------------------- */

/** @brief A setting of the engine's, which a variable may be bound to: see
 * badger/settings.h. */
typedef struct tb_setting tb_setting;

/** @brief The engine's settings, which layout files bind variables to by
 * name: see tb_gui_set_settings() and badger/settings.h. */
typedef struct tb_settings tb_settings;

/** @brief A variable's observer: see tb_var_trace().
 * @param var The variable that changed. @param user The trace's. */
typedef void tb_var_callback(tb_var var, void *user);

/**
 * @brief Make a variable, owned by the GUI: a bool, an integer, a number,
 * or text, after Tk's `BooleanVar`, `IntVar`, `DoubleVar`, and `StringVar`.
 *
 * Widgets show a variable's value and set it when they are used: several
 * widgets on one variable stay in step, as a slider and a spin box on one
 * volume. Callbacks watch it (tb_var_trace()). It may be bound to an engine
 * setting, which it then follows, and which follows it
 * (tb_var_bind_setting()).
 *
 * A variable lives until tb_var_destroy() or its GUI is destroyed; widgets
 * that show one destroyed show nothing, as if they had none.
 *
 * @param initial Required: of #TB_GUI_TYPE_BOOL, #TB_GUI_TYPE_INT,
 * #TB_GUI_TYPE_FLOAT (finite), or #TB_GUI_TYPE_TEXT (copied); its type is
 * the variable's.
 * @return The variable; none for a value of another type.
 */
TB_API tb_var tb_var_create(tb_gui *gui, const tb_gui_value *initial);

/** @brief Make a bool variable, Tk's `BooleanVar`. */
TB_API tb_var tb_var_bool(tb_gui *gui, bool value);
/** @brief Make an integer variable, Tk's `IntVar`. */
TB_API tb_var tb_var_int(tb_gui *gui, int32_t value);
/** @brief Make a number variable, Tk's `DoubleVar`. @param value Finite. */
TB_API tb_var tb_var_float(tb_gui *gui, double value);
/** @brief Make a text variable, Tk's `StringVar`. @param value Required; copied. */
TB_API tb_var tb_var_string(tb_gui *gui, const char *value);

/** @brief Destroy a variable: its handles go stale, its traces and its
 * binding to a setting end. A stale handle is ignored. */
TB_API void tb_var_destroy(tb_var var);

/** @brief Whether a handle is of a variable alive. */
TB_API bool tb_var_alive(tb_var var);

/** @brief Whether two handles are of one variable. @par Threading Thread-safe. */
TB_API bool tb_var_equal(tb_var a, tb_var b);

/** @brief A variable's type: #TB_GUI_TYPE_BOOL, #TB_GUI_TYPE_INT,
 * #TB_GUI_TYPE_FLOAT, or #TB_GUI_TYPE_TEXT; #TB_GUI_TYPE_COUNT for a stale
 * handle. */
TB_API tb_gui_type tb_var_type(tb_var var);

/**
 * @brief Read a variable.
 * @param[out] value Required; of the variable's type. Text is borrowed until
 * the variable changes.
 * @return false for a stale handle.
 */
TB_API bool tb_var_get(tb_var var, tb_gui_value *value);

/**
 * @brief Set a variable from a value of any of the four types, converted
 * to the variable's as the typed setters do (tb_var_set_bool(),
 * tb_var_set_int(), tb_var_set_float(), tb_var_set_text()).
 * @param value Required.
 * @return false, changing nothing, when the value does not convert, its
 * setting refuses it, or the handle is stale.
 */
TB_API bool tb_var_set(tb_var var, const tb_gui_value *value);

/**
 * @brief Set a variable to a bool: a number variable takes 1 or 0, and a
 * text variable "1" or "0", as Tk's do.
 * @return false, changing nothing, when its setting refuses it or the
 * handle is stale. Setting a variable to its value changes nothing.
 */
TB_API bool tb_var_set_bool(tb_var var, bool value);

/** @brief Set a variable to an integer: a bool variable is true for any but
 * 0, and a text variable takes its digits. As tb_var_set_bool(). */
TB_API bool tb_var_set_int(tb_var var, int32_t value);

/** @brief Set a variable to a number: an integer variable takes the nearest
 * whole number, and a text variable the shortest digits that read back as
 * it. As tb_var_set_bool(); false for a number not finite, or out of an
 * integer's range. */
TB_API bool tb_var_set_float(tb_var var, double value);

/**
 * @brief Set a variable from text, as a layout file writes values: a bool
 * variable takes `true`, `false`, `1`, `0`, `on`, `off`, `yes`, or `no`, in
 * any case; an integer variable decimal digits with an optional sign; a
 * number variable a decimal number; a text variable the text itself.
 * @param text Required; UTF-8.
 * @return false, changing nothing, when the text is not a value of the
 * variable's type, its setting refuses it, or the handle is stale.
 */
TB_API bool tb_var_set_text(tb_var var, const char *text);

/** @brief A variable as a bool: a number's not 0, text that reads as true.
 * false for a stale handle. */
TB_API bool tb_var_get_bool(tb_var var);

/** @brief A variable as an integer: a bool's 1 or 0, a number's nearest,
 * text's digits; 0 when it reads as none, or for a stale handle. */
TB_API int32_t tb_var_get_int(tb_var var);

/** @brief A variable as a number: a bool's 1 or 0, text's digits; 0 when
 * it reads as none, or for a stale handle. */
TB_API double tb_var_get_float(tb_var var);

/** @brief A variable as text: a bool as "1" or "0", a number in the
 * shortest digits that read back as it. @return Borrowed until the
 * variable changes; empty for a stale handle. */
TB_API tb_text tb_var_get_text(tb_var var);

/** @brief A variable's version: one more each time its value changes.
 * 0 for a stale handle. */
TB_API uint64_t tb_var_version(tb_var var);

/**
 * @brief Call a function when a variable changes, Tk's `trace_add`.
 *
 * Observers run during tb_gui_update(), after the events that changed the
 * variable, in the order they were added, once for each variable that
 * changed since they last ran, however many times it did. A change made
 * outside an update runs them in the next. An observer may set variables,
 * which runs their observers in the same update.
 *
 * @param callback Required.
 * @param user Given to it.
 * @return false for a stale handle.
 */
TB_API bool tb_var_trace(tb_var var, tb_var_callback *callback, void *user);

/** @brief Stop calling a function a variable's change called.
 * @param callback The function; NULL for every one.
 * @param user Its argument; ignored when `callback` is NULL.
 * @return How many were removed. */
TB_API size_t tb_var_untrace(tb_var var, tb_var_callback *callback, void *user);

/** @brief Name a variable, for layout files to find it by
 * (tb_gui_find_var()). @param name Required; copied; "" for none. */
TB_API void tb_var_set_name(tb_var var, const char *name);

/** @brief A variable's name. @return Borrowed until it is named again. */
TB_API tb_text tb_var_name(tb_var var);

/** @brief A variable by its name. @param name Required.
 * @return None when no variable alive has it; the first made when several do. */
TB_API tb_var tb_gui_find_var(tb_gui *gui, const char *name);

/**
 * @brief Bind a variable to an engine setting, so that an options menu
 * needs no code between them: the variable takes the setting's value now,
 * the setting takes the variable's each time it is set, and the variable
 * takes the setting's each time it changes elsewhere (a console, a file,
 * another thread), at the start and the end of each tb_gui_update().
 *
 * | Variable | Settings it binds to |
 * | --- | --- |
 * | bool | boolean |
 * | integer | integer; choice, by its index |
 * | number | number; integer, rounded to the nearest |
 * | text | text; choice, by its name; boolean, integer, and number, as a file writes them |
 *
 * The setting checks each value, as it checks any (its range, its choices,
 * its length). A bool, integer, or number variable refuses what the setting
 * refuses: tb_var_set() returns false and both keep their values. A text
 * variable keeps the text it is given, which an entry is typing, and the
 * setting takes it only when it accepts it: tb_var_setting_accepts() says
 * whether it did. Values are set at the setting's runtime layer
 * (#TB_SETTING_LAYER_RUNTIME's), as an options menu's are.
 *
 * @param setting Optional; NULL ends the binding. Borrowed: it outlives the
 * binding, or the variable.
 * @return false, binding nothing, when the variable's type does not bind to
 * the setting's, or the handle is stale.
 */
TB_API bool tb_var_bind_setting(tb_var var, tb_setting *setting);

/** @brief The setting a variable is bound to; NULL for none. */
TB_API tb_setting *tb_var_setting(tb_var var);

/** @brief Whether a variable's setting holds the variable's value: true
 * when it is bound to none. False while a text variable holds text its
 * setting refused. */
TB_API bool tb_var_setting_accepts(tb_var var);

/* ---------------------------------------------------------------------------
 * Text being edited, and the clipboard.
 * ------------------------------------------------------------------------- */

/** @brief Refuse the edit a #TB_GUI_EVENT_VALIDATE proposes, from its
 * binding or its widget's `validate` command: the widget keeps its text.
 * @param widget The event's widget. */
TB_API void tb_gui_refuse_edit(tb_widget widget);

/**
 * @brief Where the caret and the selection are in a widget's text: an
 * entry, a text box, or a spin box.
 * @param[out] caret Required; a byte offset.
 * @param[out] anchor Required; the selection's other end, the caret when none.
 * @return false when it edits no text, or the handle is stale.
 */
TB_API bool tb_widget_selection(tb_widget widget, size_t *caret, size_t *anchor);

/** @brief Place the caret and the selection in a widget's text, kept within
 * it and on characters' boundaries. @return false when it edits no text. */
TB_API bool tb_widget_select(tb_widget widget, size_t caret, size_t anchor);

/** @brief The text that Ctrl+C and Ctrl+X last copied from a widget.
 * @return Borrowed until it next changes; empty for none. */
TB_API tb_text tb_gui_clipboard(const tb_gui *gui);

/** @brief Set the text Ctrl+V pastes: from the system's clipboard, for one.
 * @param text Copied. */
TB_API void tb_gui_set_clipboard(tb_gui *gui, tb_text text);

/* ---------------------------------------------------------------------------
 * Check buttons and radio buttons.
 * ------------------------------------------------------------------------- */

/** @brief A check button: a box ticked and cleared, Tk's `checkbutton`.
 * Properties: `text`, `variable`, `on_value`, `off_value`, `selected`,
 * `command`. */
typedef struct tb_checkbutton_desc
{
    const char *text; /**< Copied; NULL for none. */
    /** Optional: the variable it sets, of any type; it is ticked while the
     * variable holds `on_value`. None keeps its own state. */
    tb_var variable;
    const char *on_value;     /**< Its variable's value ticked, as text; NULL is "1". */
    const char *off_value;    /**< Cleared; NULL is "0". */
    tb_gui_callback *command; /**< Called after it toggles; NULL for none. */
    void *user;               /**< Given to the command. */
} tb_checkbutton_desc;

/** @brief Make a check button. A click, Space while it has the focus, or
 * tb_widget_invoke() toggles it: it sets its variable, sends
 * #TB_GUI_EVENT_CHANGED, and calls its command. @param desc Optional.
 * @return None when `parent` is stale. */
TB_API tb_widget tb_checkbutton(tb_widget parent, const tb_checkbutton_desc *desc);

/** @brief A radio button: one of a group that share a variable, each with
 * its own value, Tk's `radiobutton`. Properties: `text`, `variable`,
 * `value`, `selected`, `command`. */
typedef struct tb_radiobutton_desc
{
    const char *text;         /**< Copied; NULL for none. */
    tb_var variable;          /**< The group's variable, of any type. */
    const char *value;        /**< Its value, as text, which choosing it sets; NULL is "". */
    tb_gui_callback *command; /**< Called after it is chosen; NULL for none. */
    void *user;               /**< Given to the command. */
} tb_radiobutton_desc;

/** @brief Make a radio button: chosen while its variable holds its value.
 * A click, Space, or tb_widget_invoke() chooses it; the arrow keys choose
 * the one before or after it among its siblings of its variable, and move
 * the focus there. @param desc Optional. @return None when `parent` is stale. */
TB_API tb_widget tb_radiobutton(tb_widget parent, const tb_radiobutton_desc *desc);

/* ---------------------------------------------------------------------------
 * Entries and text boxes.
 * ------------------------------------------------------------------------- */

/** @brief An entry: a line of text to type, Tk's `entry`. Properties:
 * `text`, `variable`, `placeholder`, `show`, `width`, `max_length`,
 * `read_only`, `validate`. */
typedef struct tb_entry_desc
{
    const char *text; /**< Its first text, when it has no variable; copied. */
    /** Optional: a text variable it edits; its text is the variable's. */
    tb_var variable;
    const char *placeholder; /**< Shown dimmed while it is empty; NULL for none. */
    /** Shown for each character in place of it, Tk's `show`: "*" or "•"
     * for a password, which copying and cutting then leave alone. NULL or
     * "" shows the text. */
    const char *show;
    int32_t width;      /**< Characters of "0" it is wide; 0 is 20. */
    int32_t max_length; /**< Characters it holds at most; 0 for no limit. */
    bool read_only;     /**< Its text is selected and copied, never changed. */
    /** Optional: called before each change, Tk's `validatecommand`, after
     * its #TB_GUI_EVENT_VALIDATE bindings: the event's `text` is the text
     * proposed, and tb_gui_refuse_edit() refuses it. */
    tb_gui_callback *validate;
    void *user; /**< Given to `validate`. */
} tb_entry_desc;

/**
 * @brief Make an entry. A click gives it the focus and places the caret;
 * a drag selects, a double click selects a word. With the focus it takes
 * typed text (#TB_GUI_EVENT_TEXT), Left and Right by characters or with
 * Control by words, Home and End, Backspace and Delete, Shift with each to
 * select, and Control with A, C, X, and V to select all, copy, cut, and
 * paste through the GUI's clipboard (tb_gui_clipboard()). Each change sends
 * #TB_GUI_EVENT_CHANGED. Enter and Escape are left to its ancestors: a
 * dialog's default button and its cancel.
 * @param desc Optional. @return None when `parent` is stale.
 */
TB_API tb_widget tb_entry(tb_widget parent, const tb_entry_desc *desc);

/** @brief How a text box breaks lines. */
typedef enum tb_text_wrap
{
    TB_TEXT_WRAP_WORD, /**< Lines too long for it break between words. */
    TB_TEXT_WRAP_NONE  /**< Lines break only at line breaks; it scrolls across. */
} tb_text_wrap;

/** @brief A text box: several lines of text, read or typed, Tk's `text`.
 * Properties: `text`, `variable`, `width`, `height`, `wrap`, `max_length`,
 * `read_only`. */
typedef struct tb_text_box_desc
{
    const char *text;   /**< Its first text, when it has no variable; copied. */
    tb_var variable;    /**< Optional: a text variable it edits. */
    int32_t width;      /**< Characters of "0" it is wide; 0 is 40. */
    int32_t height;     /**< Lines it is high; 0 is 6. */
    tb_text_wrap wrap;  /**< How it breaks lines. */
    int32_t max_length; /**< Characters it holds at most; 0 for no limit. */
    bool read_only;     /**< Its text is read, selected, and copied, never changed. */
} tb_text_box_desc;

/**
 * @brief Make a text box. It edits as an entry does, and Enter breaks the
 * line; Up and Down move by lines, Page Up and Page Down by its height,
 * Control with Home and End to the start and the end. Text taller than it,
 * or wider when it does not wrap, scrolls to keep the caret in sight, and
 * with the wheel.
 * @param desc Optional. @return None when `parent` is stale.
 */
TB_API tb_widget tb_text_box(tb_widget parent, const tb_text_box_desc *desc);

/* ---------------------------------------------------------------------------
 * Spin boxes, sliders, and progress bars.
 * ------------------------------------------------------------------------- */

/** @brief Which way a slider or a progress bar runs. */
typedef enum tb_gui_orient
{
    TB_GUI_ORIENT_HORIZONTAL, /**< Across: from the left to the right. */
    TB_GUI_ORIENT_VERTICAL    /**< Down: from the top to the bottom. */
} tb_gui_orient;

/** @brief A spin box: a number typed, or stepped with its arrows, Tk's
 * `spinbox`. Properties: `value`, `variable`, `from`, `to`, `increment`,
 * `format`, `width`, `wrap`, `command`, `validate`. */
typedef struct tb_spinbox_desc
{
    tb_var variable; /**< Optional: a variable of any type it sets. */
    float from;      /**< The least value. */
    float to;        /**< The greatest; when it and `from` are 0, 100. */
    float increment; /**< A step; 0 is 1. */
    float value;     /**< Its first value, when it has no variable. */
    /** How its value is written, a `printf` conversion of a `double` such
     * as "%.2f" or "%5.1f%%"; NULL or "" writes the shortest digits. */
    const char *format;
    int32_t width;             /**< Characters of "0" its text is wide; 0 is 8. */
    bool wrap;                 /**< A step past an end goes around to the other. */
    tb_gui_callback *command;  /**< Called after a step; NULL for none. */
    void *user;                /**< Given to the command and `validate`. */
    tb_gui_callback *validate; /**< As an entry's. */
} tb_spinbox_desc;

/**
 * @brief Make a spin box. Its arrows, Up and Down, Page Up and Page Down
 * (ten steps), and the wheel step its value, within `from` and `to`. Typed
 * text is the value when Enter is pressed or it loses the focus: a number
 * kept within its range, or the value it had when the text is no number.
 * Each change sends #TB_GUI_EVENT_CHANGED.
 * @param desc Optional. @return None when `parent` is stale.
 */
TB_API tb_widget tb_spinbox(tb_widget parent, const tb_spinbox_desc *desc);

/** @brief A slider, Tk's `scale`. Properties: `value`, `variable`, `from`,
 * `to`, `resolution`, `big_increment`, `orient`, `length`, `show_value`,
 * `format`, `command`. */
typedef struct tb_scale_desc
{
    tb_var variable;     /**< Optional: a variable of any type it sets. */
    float from;          /**< Its value at the left, or the top. */
    float to;            /**< At the right, or the bottom; when it and `from` are 0, 100. */
    float resolution;    /**< Values are rounded to multiples of it from `from`; 0 for none. */
    float big_increment; /**< Page Up's and Page Down's step; 0 is a tenth of the range. */
    float value;         /**< Its first value, when it has no variable. */
    tb_gui_orient orient;
    float length;    /**< Its track's length, in units; 0 is 160. */
    bool show_value; /**< Its value is written beside the track. */
    /** How its value is written, as a spin box's; NULL or "" writes the
     * digits of its resolution. */
    const char *format;
    tb_gui_callback *command; /**< Called after its value changes; NULL for none. */
    void *user;               /**< Given to the command. */
} tb_scale_desc;

/**
 * @brief Make a slider. A press on its track moves its handle there and
 * gives it the focus, and a drag moves it; Left and Up step toward `from`,
 * Right and Down toward `to`, by its resolution (or a hundredth of the
 * range), Page Up and Page Down by its big increment, Home and End to its
 * ends. Each change sends #TB_GUI_EVENT_CHANGED.
 * @param desc Optional. @return None when `parent` is stale.
 */
TB_API tb_widget tb_scale(tb_widget parent, const tb_scale_desc *desc);

/** @brief Whether a progress bar shows how far, or only that work goes on. */
typedef enum tb_progress_mode
{
    TB_PROGRESS_DETERMINATE,  /**< Filled as far as its value is to its maximum. */
    TB_PROGRESS_INDETERMINATE /**< A block that moves to and fro, by tb_gui_update()'s time. */
} tb_progress_mode;

/** @brief A progress bar, Tk's `ttk.Progressbar`. Properties: `value`,
 * `variable`, `maximum`, `mode`, `orient`, `length`. */
typedef struct tb_progressbar_desc
{
    tb_var variable; /**< Optional: a variable of any type it shows. */
    float maximum;   /**< Its value full; 0 is 100. */
    float value;     /**< Its value, when it has no variable. */
    tb_progress_mode mode;
    tb_gui_orient orient;
    float length; /**< Units long; 0 is 160. */
} tb_progressbar_desc;

/** @brief Make a progress bar; it takes no input. @param desc Optional.
 * @return None when `parent` is stale. */
TB_API tb_widget tb_progressbar(tb_widget parent, const tb_progressbar_desc *desc);

/* ---------------------------------------------------------------------------
 * Combo boxes.
 * ------------------------------------------------------------------------- */

/** @brief A combo box: one of a list of values, chosen from a list that
 * drops down, Tk's `ttk.Combobox` in its read-only state. Properties:
 * `values`, `current`, `variable`, `width`, `height`, `command`. */
typedef struct tb_combobox_desc
{
    const char *const *values; /**< Its values, copied; NULL for none. */
    size_t value_count;        /**< How many. */
    /** Optional: a text variable, which holds the value chosen, or an
     * integer one, which holds its index. */
    tb_var variable;
    int32_t current;          /**< The index chosen, when it has no variable; -1 for none. */
    int32_t width;            /**< Characters of "0" wide; 0 is as wide as its widest value. */
    int32_t height;           /**< Values the list shows at once; 0 is 8. */
    tb_gui_callback *command; /**< Called after a value is chosen; NULL for none. */
    void *user;               /**< Given to the command. */
} tb_combobox_desc;

/**
 * @brief Make a combo box. A click, Space, or Alt+Down opens its list over
 * every widget; there the mouse or Up and Down pick a value, and a click,
 * Enter, or Space chooses it, and Escape, Alt+Up, a click outside, or the
 * loss of the focus closes it. Closed, Up and Down choose the value before
 * or after, and the wheel too while it has the focus. Each choice sends
 * #TB_GUI_EVENT_CHANGED.
 *
 * Its `values` property is its values as text, one a line.
 * @param desc Optional. @return None when `parent` is stale.
 */
TB_API tb_widget tb_combobox(tb_widget parent, const tb_combobox_desc *desc);

/** @brief Whether a combo box's list is open. */
TB_API bool tb_combobox_open(tb_widget widget);

/** @brief The engine's kinds of value widgets. @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_checkbutton_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_radiobutton_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_entry_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_text_box_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_spinbox_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_scale_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_progressbar_class(void);
/** @brief See tb_checkbutton_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_combobox_class(void);

/* ---------------------------------------------------------------------------
 * Finding widgets by path.
 * ------------------------------------------------------------------------- */

/**
 * @brief Find a widget by its path from another: the names of its children,
 * their children, and so on, joined by `/`, as Godot's `get_node` and Tk's
 * paths. Each name is that of the first child of that name, in the order of
 * the tree.
 * @code
 * const tb_widget apply = tb_widget_find(tb_gui_root(gui), "options/video/apply");
 * @endcode
 * @param from Where the path starts.
 * @param path Required; UTF-8, terminated. "" is `from` itself.
 * @return The widget; none when no widget has the path, a name in it is
 * empty, or `from` is stale.
 */
TB_API tb_widget tb_widget_find(tb_widget from, const char *path);

/**
 * @brief The path of a widget from its GUI's root, which tb_widget_find()
 * from the root finds it by: the names of its ancestors below the root,
 * and its own, joined by `/`. The root's path is "".
 * @param[out] buffer Receives the path and a terminator, when it fits;
 * optional when `capacity` is 0.
 * @param capacity Bytes of `buffer`.
 * @param[out] length Optional; the bytes the path needs, the terminator not
 * counted, whether or not it fits.
 * @return false when the handle is stale, a widget on the way has no name
 * or a name with `/` in it, or the path does not fit.
 */
TB_API bool tb_widget_path(tb_widget widget, char *buffer, size_t capacity, size_t *length);

/* ---------------------------------------------------------------------------
 * Names: commands and variables a layout file refers to.
 * ------------------------------------------------------------------------- */

/**
 * @brief Name a command, so that a layout file's properties of type
 * #TB_GUI_TYPE_COMMAND refer to it: `"command": "apply_settings"`. Saving
 * writes a command by the name it was registered under.
 *
 * Register a callback and argument once: when two names hold the same pair,
 * saving writes the first registered.
 * @param name Required; UTF-8; copied.
 * @param callback Required.
 * @param user Given to the callback.
 * @return false, changing nothing, when a command of that name is registered.
 */
TB_API bool tb_gui_register_command(tb_gui *gui, const char *name, tb_gui_callback *callback,
                                    void *user);

/** @brief Forget a command's name; widgets that hold it keep it.
 * @param name Required. @return false when none is registered by that name. */
TB_API bool tb_gui_unregister_command(tb_gui *gui, const char *name);

/** @brief A command registered by name. @param name Required.
 * @param[out] command Required; unchanged when there is none.
 * @return false when none is registered by that name. */
TB_API bool tb_gui_find_command(tb_gui *gui, const char *name, tb_gui_command *command);

/**
 * @brief Give the GUI the settings a layout file's variables bind to by
 * name (`"setting": "graphics.fov"`, tb_var_bind_setting()).
 * @param settings Optional; NULL for none. Borrowed: it outlives the GUI, or
 * is replaced first.
 */
TB_API void tb_gui_set_settings(tb_gui *gui, tb_settings *settings);

/* ---------------------------------------------------------------------------
 * Layout files.
 * ------------------------------------------------------------------------- */

/** @brief What became of reading or writing a layout file. */
typedef enum tb_gui_file_result
{
    TB_GUI_FILE_OK,           /**< Success. */
    TB_GUI_FILE_NOT_FOUND,    /**< The VFS has no such file. */
    TB_GUI_FILE_INPUT_FAILED, /**< The VFS could not read it. */
    TB_GUI_FILE_MALFORMED,    /**< The text is not JSON. */
    /** It is JSON, but not a layout the GUI can build: another format or a
     * newer version, an unknown kind, property, or command, a value of the
     * wrong type, or one a property refuses. */
    TB_GUI_FILE_INVALID,
    TB_GUI_FILE_OUTPUT_FAILED /**< The stream refused what was written. */
} tb_gui_file_result;

/** @brief Bytes of the path, and of the message, of #tb_gui_file_error. */
#define TB_GUI_FILE_ERROR_CAPACITY 256

/** @brief Where a layout file is wrong, and why. */
typedef struct tb_gui_file_error
{
    tb_gui_file_result result; /**< As the function returned. */
    /** The JSON path of the value at fault, such as
     * `$.root.children[2].properties.colour`; "" when the fault is not in a
     * value, as for text that is not JSON. Cut short when it is too long. */
    char path[TB_GUI_FILE_ERROR_CAPACITY];
    size_t line;        /**< In the text, from 1; 0 when the fault is not in the text. */
    size_t column;      /**< Byte in the line, from 1; 0 as `line`. */
    const char *reason; /**< A short phrase: "unknown property"; static; "" for none. */
    /** All of it, for a log: "gui.json:12:5: $.root.properties.colour:
     * unknown property \"colour\"". */
    char message[TB_GUI_FILE_ERROR_CAPACITY];
} tb_gui_file_error;

/** @brief The format a layout file names: `"format": "badger-gui"`. */
#define TB_GUI_FILE_FORMAT "badger-gui"

/** @brief The version of the format this engine writes, and the newest it reads. */
#define TB_GUI_FILE_VERSION 1

/**
 * @brief Build a widget, and the widgets in it, from a layout file's text,
 * as the last child of `parent`.
 *
 * The file is JSON (see `docs/gui.md`): a format, a version, and a root
 * widget, each widget its kind's registered name, its name, its properties
 * by name, its place in its parent's grid, its rows and columns, and its
 * children. Loading is all or nothing: on any error nothing is left of what
 * was built, and the error says where.
 *
 * Properties are set in the order of the file, with tb_widget_set(); a
 * command is found by its name (tb_gui_register_command()), and a variable
 * by its own (tb_gui_find_var()). Variables the file declares are made
 * first, unless the GUI has one of the name and type, which is used as it
 * is; a file that fails destroys those it made. No callback is called
 * while the widgets are built.
 *
 * @param parent A widget alive.
 * @param text The file; NULL only at length zero. Not kept.
 * @param length Bytes of `text`.
 * @param name Optional: the file's name, for the error's message.
 * @param[out] widget Required: the widget built, or none on failure.
 * @param[out] error Optional: where and why it failed.
 * @return OK, MALFORMED, or INVALID; INVALID too for a stale parent.
 */
TB_API tb_gui_file_result tb_gui_load(tb_widget parent, const char *text, size_t length,
                                      const char *name, tb_widget *widget,
                                      tb_gui_file_error *error);

/**
 * @brief tb_gui_load() of a file read through a VFS.
 * @param vfs Required. @param path The file's name in the VFS; required.
 * @param[out] widget Required. @param[out] error Optional.
 * @return As tb_gui_load(), or NOT_FOUND or INPUT_FAILED.
 */
TB_API tb_gui_file_result tb_gui_load_file(tb_widget parent, struct tb_vfs *vfs, const char *path,
                                           tb_widget *widget, tb_gui_file_error *error);

/**
 * @brief Write a widget, and the widgets in it, as a layout file that
 * tb_gui_load() builds again.
 *
 * The text is the same for the same widgets, byte for byte: indented JSON,
 * the keys of each widget in one order, its properties in the order of its
 * kind's table and then the common ones, numbers in the fewest digits that
 * read back as the same value. A property is left out when loading would
 * give it its value without it: when it is its kind's default, after the
 * properties written before it, a kind's variables and commands first.
 * Read-only properties, commands set from code rather than registered by
 * name, and variables with no name that finds them, are not written. The
 * variables named are declared in the file, with their values or settings. Saving a file
 * that was loaded gives the same text when the file was written so.
 *
 * @param sink Required open stream that can write; borrowed.
 * @return OK; INVALID for a stale handle or text that is not UTF-8;
 * OUTPUT_FAILED when the stream refused.
 */
TB_API tb_gui_file_result tb_gui_save(tb_widget widget, tb_stream *sink);

/* ---------------------------------------------------------------------------
 * Hot reload.
 * ------------------------------------------------------------------------- */

/**
 * @brief Called when a watched file was loaded again, or failed to be.
 * @param widget The widget built from it, which replaced the last; on
 * failure, the widget kept.
 * @param error NULL when it loaded; otherwise where and why not, borrowed
 * for the call.
 * @param user As watched.
 */
typedef void tb_gui_reload_callback(tb_widget widget, const tb_gui_file_error *error, void *user);

/** @brief How a file is watched. Zero is every default. */
typedef struct tb_gui_watch_options
{
    /** Seconds between looks at the file; 0 is a half. */
    double interval;
    tb_gui_reload_callback *callback; /**< Optional: told of each reload. */
    void *user;                       /**< Given to the callback. */
} tb_gui_watch_options;

/**
 * @brief Load a widget again from its file whenever the file changes: for
 * development, where an author edits a layout while the game runs.
 *
 * tb_gui_update() reads the file through the VFS every interval, and when
 * its bytes have changed, builds it again under the same parent, in the
 * same place among its siblings, and destroys the old widget, whose handles
 * go stale: the callback is given the new one. A file that fails to load
 * leaves the old widget as it was, and the callback is told why; it is
 * loaded again when it next changes. A file missing for a moment, as an
 * editor saves it, is waited for.
 *
 * The watch ends when the widget is destroyed other than by a reload, or
 * with tb_gui_unwatch(). A shipped game, whose files are in packages, need
 * not watch them.
 *
 * @param widget A widget alive, loaded from the file.
 * @param vfs Required; borrowed until the watch ends or the GUI is destroyed.
 * @param path Required; the file's name in the VFS; copied.
 * @param options Optional; NULL is every default.
 * @return false for a stale handle or the root.
 */
TB_API bool tb_gui_watch(tb_widget widget, struct tb_vfs *vfs, const char *path,
                         const tb_gui_watch_options *options);

/** @brief Stop watching a widget's file. @return false when it was not watched. */
TB_API bool tb_gui_unwatch(tb_widget widget);

#endif
