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
 * **Scrolling and popups.** A kind may show its children through a
 * viewport, scrolled and clipped (tb_widget_class::viewport); a widget may
 * be shown as a popup in front of every other (tb_widget_popup()).
 *
 * **Not done yet.** Variables, and widgets that edit values; style
 * classes; layout files; focus moved by arrows or a gamepad.
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
#include "badger/text.h"
#include "badger/ui.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/** @brief Whether the GUI takes the keyboard: a widget has the focus, or a
 * popup that takes input is shown. As of the last update. */
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
    tb_widget popup;   /**< The popup in front (tb_widget_popup()), or none. */
} tb_gui_stats;

/** @brief How the GUI is doing. @param[out] stats Required. */
TB_API void tb_gui_stats_of(const tb_gui *gui, tb_gui_stats *stats);

/* ---------------------------------------------------------------------------
 * Values and properties.
 * ------------------------------------------------------------------------- */

/** @brief The type of a property's value. */
typedef enum tb_gui_type
{
    TB_GUI_TYPE_BOOL,    /**< `bool`. */
    TB_GUI_TYPE_INT,     /**< `int32_t`. */
    TB_GUI_TYPE_FLOAT,   /**< `float`. */
    TB_GUI_TYPE_VEC2,    /**< #tb_vec2: a pair, such as a left and a right. */
    TB_GUI_TYPE_COLOR,   /**< #tb_vec4: RGBA, straight alpha. */
    TB_GUI_TYPE_TEXT,    /**< UTF-8, held in a kind's data as a #tb_gui_string. */
    TB_GUI_TYPE_ENUM,    /**< `int32_t`, one of the property's names. */
    TB_GUI_TYPE_FLAGS,   /**< `uint32_t`, a bit for each of the property's names. */
    TB_GUI_TYPE_FONT,    /**< #tb_font_spec. */
    TB_GUI_TYPE_COMMAND, /**< #tb_gui_command: a callback and its argument. */
    TB_GUI_TYPE_COUNT    /**< How many there are; not a type. */
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
    TB_GUI_EVENT_MOTION,      /**< The mouse moved over it, or anywhere while it holds the mouse. */
    TB_GUI_EVENT_POPUP_CLOSE, /**< It was closed as a popup: see tb_widget_popup(). */
    TB_GUI_EVENT_COUNT        /**< How many there are; not a kind. */
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

/** @brief Where a kind that scrolls shows its children: see
 * tb_widget_class::viewport. */
typedef struct tb_gui_viewport
{
    /** What of its children is seen, in units of the GUI: its rectangle
     * less its insets, and less its scroll bars. */
    tb_canvas_rect clip;
    /** The size of the space its children's grid is laid out in, in units:
     * at least the clip's size along an axis it scrolls along. */
    tb_vec2 content;
} tb_gui_viewport;

/**
 * @brief A kind of widget: a class, whose functions the GUI calls for each
 * widget of it, with the widget's data. Every function may be NULL.
 *
 * A widget's natural size is the larger of what its kind measures and what
 * the grid of its children wants, with its insets around it; its parent's
 * grid gives it a place from that. A kind with a `viewport` is as large as
 * it measures, with its insets: the grid of its children is laid out in
 * the space the viewport gives it, and scrolled within it.
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
    /** For a kind whose children scroll: given its rectangle less its
     * insets, where its children are seen and the size their grid is laid
     * out in, after arrange(). Its children are placed in that space moved
     * by its scroll offset (tb_widget_scroll_to()), and drawn and found by
     * the mouse only within the clip. NULL lays them out in the rectangle
     * less the insets. */
    tb_gui_viewport (*viewport)(tb_widget widget, void *data, tb_canvas_rect inner);
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

/** @brief The size a widget's grid wants, in units, its insets not
 * counted, as the last layout measured it: its rows and columns summed. A
 * kind's measure() reads its own, already measured: a kind that scrolls
 * sizes itself from it. Zero for a stale handle. */
TB_API tb_vec2 tb_grid_request(tb_widget container);

/* ---------------------------------------------------------------------------
 * Scrolling.
 * ------------------------------------------------------------------------- */

/** @brief How far a widget of a kind that scrolls (tb_widget_class::viewport)
 * has its children scrolled, in units from their top left, as the last
 * layout kept it within its content. Zero for other widgets. */
TB_API tb_vec2 tb_widget_scroll_offset(tb_widget widget);

/**
 * @brief Scroll a widget's children: their top left `offset` units above
 * and left of its clip's. Kept, at the next layout, from 0 to as far as
 * the content goes past the clip.
 * @param offset Finite; negative is 0.
 * @return false for a stale handle, or a kind that does not scroll.
 */
TB_API bool tb_widget_scroll_to(tb_widget widget, tb_vec2 offset);

/**
 * @brief Scroll each ancestor that scrolls as little as shows a widget,
 * nearest first: its top left edges, when it is larger than the clip. Tk's
 * `see`. The widgets are laid out first when something changed. The focus
 * given to a widget scrolls it into view so.
 * @return false for a stale handle or a widget not shown.
 */
TB_API bool tb_widget_see(tb_widget widget);

/* ---------------------------------------------------------------------------
 * Popups.
 * ------------------------------------------------------------------------- */

/** @brief Where a popup opens, by its anchor's rectangle. */
typedef enum tb_gui_popup_placement
{
    /** Below the anchor, its left edges in line: a menu of a menu bar, a
     * combobox's list. Above it when there is no room below. */
    TB_GUI_POPUP_BELOW,
    /** Right of the anchor, its top edges in line: a menu's cascade. Left
     * of it when there is no room to the right. */
    TB_GUI_POPUP_RIGHT,
    /** Its top left at the anchor's: a context menu at the mouse, a
     * tooltip. Up or left of it when there is no room. */
    TB_GUI_POPUP_AT
} tb_gui_popup_placement;

/** @brief How a popup is shown. Zero is below an empty rectangle at the
 * GUI's top left. */
typedef struct tb_gui_popup_options
{
    /** The rectangle it opens beside, in units of the GUI: the widget that
     * opened it, or the mouse's point. */
    tb_canvas_rect anchor;
    tb_gui_popup_placement placement; /**< Where, by the anchor. */
    /** The least size it is, in units: a combobox's list is as wide as the
     * combobox. */
    tb_vec2 min_size;
    /** It takes no input, and is not closed by a press elsewhere or by
     * Escape: a tooltip. */
    bool passive;
} tb_gui_popup_options;

/**
 * @brief Show a widget as a popup: in front of every other widget and
 * modal widget, at its natural size, by its options, kept inside the GUI,
 * and not clipped by its parent. Menus, combobox lists, and tooltips are
 * popups.
 *
 * The widget stays in the tree, but out of its parent's grid while it is a
 * popup. Popups stack: the last shown is in front. While one that is not
 * passive is shown, a press outside every popup closes them all and is
 * used by it; a press inside one closes those in front of it; keys go to
 * the one in front, unless the focus is inside it, and then to its
 * ancestors, and Escape that it does not use closes it. A popup closed is
 * sent #TB_GUI_EVENT_POPUP_CLOSE; one hidden or destroyed is closed
 * without it.
 *
 * Showing a widget already a popup moves it by its new options.
 * @param options Optional; NULL is zero.
 * @return false for a stale handle or the root.
 */
TB_API bool tb_widget_popup(tb_widget widget, const tb_gui_popup_options *options);

/** @brief Close a popup, and the popups in front of it, those first; each is
 * sent #TB_GUI_EVENT_POPUP_CLOSE. Nothing for a widget that is not one. */
TB_API void tb_widget_close_popup(tb_widget widget);

/** @brief Whether a widget is shown as a popup. */
TB_API bool tb_widget_is_popup(tb_widget widget);

/** @brief Close every popup that is not passive, as a press outside them
 * does. */
TB_API void tb_gui_close_popups(tb_gui *gui);

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

/** @brief What of the widget being drawn can be seen, in units: its
 * rectangle cut by its ancestors' clips. A kind of many rows draws only
 * those inside it. @param paint Required. */
TB_API tb_canvas_rect tb_gui_paint_clip(const tb_gui_paint *paint);

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
 * Scroll frames and scroll bars.
 * ------------------------------------------------------------------------- */

/** @brief Which way a widget runs. */
typedef enum tb_gui_orient
{
    TB_GUI_ORIENT_VERTICAL,  /**< Up and down. */
    TB_GUI_ORIENT_HORIZONTAL /**< Left and right. */
} tb_gui_orient;

/** @brief A scroll frame: a frame whose grid is laid out at its natural
 * size, or the frame's, whichever is larger, and seen through the frame,
 * scrolled by the wheel (Shift for across) and by bars shown where the grid
 * is larger. Properties: `width`, `height`, `xscroll`, `yscroll`, `border`,
 * `offset`. */
typedef struct tb_scrollframe_desc
{
    /** Units across it wants; 0 is its grid's width. */
    float width;
    /** Units down it wants; 0 is its grid's height. */
    float height;
    bool no_xscroll; /**< Its grid is as wide as it, and never scrolls across. */
    bool no_yscroll; /**< Its grid is as tall as it, and never scrolls down. */
    bool border;     /**< Outlined with the theme's border. */
} tb_scrollframe_desc;

/** @brief Make a scroll frame; grid its children in it as in a frame.
 * @param desc Optional; NULL is zero. @return None when `parent` is stale. */
TB_API tb_widget tb_scrollframe(tb_widget parent, const tb_scrollframe_desc *desc);

/** @brief A scroll bar alone, Tk's `scrollbar`: a thumb that shows the part
 * of something seen, from `first` to `last` as fractions of it, dragged,
 * paged by a press on its track, or turned by the wheel, which calls its
 * `command`; the command reads where it is (tb_scrollbar_get()) and
 * scrolls what it is for. Properties: `orient`, `first`, `last`, `command`. */
typedef struct tb_scrollbar_desc
{
    tb_gui_orient orient;     /**< Vertical unless chosen. */
    tb_gui_callback *command; /**< Called when it is moved; NULL for none. */
    void *user;               /**< Given to the command. */
} tb_scrollbar_desc;

/** @brief Make a scroll bar, showing all, 0 to 1. @param desc Optional.
 * @return None when `parent` is stale. */
TB_API tb_widget tb_scrollbar(tb_widget parent, const tb_scrollbar_desc *desc);

/** @brief Show the part of something seen, Tk's `set`, without calling the
 * command. @return false for a stale handle or another kind, or fractions
 * not within 0 and 1 and in order. */
TB_API bool tb_scrollbar_set(tb_widget scrollbar, float first, float last);

/** @brief The part shown. @param[out] first Required. @param[out] last
 * Required. @return false for a stale handle or another kind. */
TB_API bool tb_scrollbar_get(tb_widget scrollbar, float *first, float *last);

/* ---------------------------------------------------------------------------
 * Lists.
 * ------------------------------------------------------------------------- */

/** @brief How a list's items are chosen, Tk's `selectmode`. */
typedef enum tb_listbox_selectmode
{
    /** One item, which follows the mouse dragged and the arrows. */
    TB_LISTBOX_BROWSE,
    /** One item, chosen by a click or the arrows. */
    TB_LISTBOX_SINGLE,
    /** Any items, each click turning one on or off; the arrows move the
     * active item, and Space turns it. */
    TB_LISTBOX_MULTIPLE,
    /** A range, as file lists have it: a click chooses one, Shift a range
     * from the last chosen, Control turns one on or off. */
    TB_LISTBOX_EXTENDED
} tb_listbox_selectmode;

/** @brief A list box, Tk's `listbox`: lines of text, chosen by the mouse or
 * the keyboard (the arrows, Page Up and Down, Home and End, Space, and
 * Control+A in the modes of many). It takes the focus on a click and by
 * Tab. Only the rows that can be seen are drawn, so lists of many thousands
 * of items cost what their window does. Properties: `items` (the items, a
 * line each), `selectmode`, `width`, `height`, `offset`, `command`. */
typedef struct tb_listbox_desc
{
    tb_listbox_selectmode selectmode; /**< Browse unless chosen. */
    float width;                      /**< Units across; 0 is ten ems. */
    /** Rows it shows, scrolling the rest; 0 shows every row, for a list in
     * a scroll frame. */
    int32_t height;
    /** Called when the selection changes by the mouse or the keyboard, Tk's
     * `<<ListboxSelect>>`; NULL for none. */
    tb_gui_callback *command;
    void *user;               /**< Given to the command. */
    const char *const *items; /**< Its first items, copied; NULL for none. */
    size_t item_count;        /**< How many. */
} tb_listbox_desc;

/** @brief Make a list box. @param desc Optional. @return None when `parent` is stale. */
TB_API tb_widget tb_listbox(tb_widget parent, const tb_listbox_desc *desc);

/** @brief Insert an item before another. @param index Where; past the end,
 * or SIZE_MAX, is at the end. @param text Required; copied; one line.
 * @return Where it was put; SIZE_MAX for a stale handle or another kind. */
TB_API size_t tb_listbox_insert(tb_widget listbox, size_t index, const char *text);

/** @brief Delete `count` items from `first`, as many as there are.
 * @return How many were deleted. */
TB_API size_t tb_listbox_delete(tb_widget listbox, size_t first, size_t count);

/** @brief How many items a list has; 0 for a stale handle or another kind. */
TB_API size_t tb_listbox_size(tb_widget listbox);

/** @brief An item's text. @return Borrowed until the items change; empty
 * past the end. */
TB_API tb_text tb_listbox_get(tb_widget listbox, size_t index);

/** @brief Choose an item or not, without calling the command; choosing one
 * in a mode of one leaves it alone chosen. @return false past the end. */
TB_API bool tb_listbox_select(tb_widget listbox, size_t index, bool selected);

/** @brief Choose no item. */
TB_API void tb_listbox_clear_selection(tb_widget listbox);

/** @brief Whether an item is chosen. */
TB_API bool tb_listbox_selected(tb_widget listbox, size_t index);

/**
 * @brief The items chosen, in order, Tk's `curselection`.
 * @param[out] indices Optional when `capacity` is 0; the first `capacity`.
 * @return How many are chosen, which may be more than `capacity`.
 */
TB_API size_t tb_listbox_selection(tb_widget listbox, size_t *indices, size_t capacity);

/** @brief The active item, which the keyboard moves; SIZE_MAX when the list
 * is empty. */
TB_API size_t tb_listbox_active(tb_widget listbox);

/** @brief Make an item the active one. @return false past the end. */
TB_API bool tb_listbox_activate(tb_widget listbox, size_t index);

/** @brief Scroll the list, and each ancestor that scrolls, to show an item.
 * @return false past the end. */
TB_API bool tb_listbox_see(tb_widget listbox, size_t index);

/** @brief The item nearest a height, in units of the GUI, Tk's `nearest`.
 * @return SIZE_MAX for an empty list. */
TB_API size_t tb_listbox_nearest(tb_widget listbox, float y);

/* ---------------------------------------------------------------------------
 * Menus.
 * ------------------------------------------------------------------------- */

/** @brief What an item of a menu is. */
typedef enum tb_menu_item_kind
{
    TB_MENU_ITEM_COMMAND,  /**< Calls its command. */
    TB_MENU_ITEM_CHECK,    /**< Turned on and off, then calls its command. */
    TB_MENU_ITEM_RADIO,    /**< On, with the others of its group off, then calls its command. */
    TB_MENU_ITEM_CASCADE,  /**< Opens another menu beside it. */
    TB_MENU_ITEM_SEPARATOR /**< A line between items. */
} tb_menu_item_kind;

/** @brief An item to add to a menu. Zero is a command of no label. */
typedef struct tb_menu_item_desc
{
    tb_menu_item_kind kind;
    /** Its label, copied; `&` marks its mnemonic, the letter that chooses
     * it from the keyboard and is underlined ("&Open"); `&&` is an
     * ampersand. NULL for none. */
    const char *label;
    /** Text shown at its right, as "Ctrl+O"; copied; NULL for none. The
     * game binds the keys themselves. */
    const char *accelerator;
    tb_gui_callback *command; /**< Called when it is chosen; NULL for none. */
    void *user;               /**< Given to the command. */
    bool checked;             /**< A check or radio item's first state. */
    int32_t group;            /**< A radio item's group, within its menu. */
    /** A cascade's menu: a child of this menu, made by tb_menu(). */
    tb_widget submenu;
    bool disabled; /**< Shown dimmed, and never chosen. */
} tb_menu_item_desc;

/**
 * @brief Make a menu, Tk's `menu`: a column of items, shown as a popup by
 * a menu bar, by a cascade of another menu, or at a point as a context menu
 * (tb_menu_popup()). The mouse lights the item under it and a click chooses
 * it; the arrows move between items, Right opens a cascade and Left closes
 * it, Enter or Space chooses, a mnemonic's letter chooses its item, and
 * Escape closes the menu in front. Choosing an item closes every menu,
 * then calls its command; a cascade opens instead.
 * @param parent The menu bar or menu it opens from, or the widget it is a
 * context menu of: keys a menu does not use go to it.
 * @return None when `parent` is stale.
 */
TB_API tb_widget tb_menu(tb_widget parent);

/** @brief Add an item at the end of a menu. @param item Required.
 * @return Its index; SIZE_MAX for a stale handle or another kind. */
TB_API size_t tb_menu_add(tb_widget menu, const tb_menu_item_desc *item);

/** @brief How many items a menu has, separators counted. */
TB_API size_t tb_menu_item_count(tb_widget menu);

/** @brief Whether a check or radio item is on. */
TB_API bool tb_menu_item_checked(tb_widget menu, size_t index);

/** @brief Turn a check or radio item on or off, without calling its
 * command. @return false past the end. */
TB_API bool tb_menu_set_checked(tb_widget menu, size_t index, bool checked);

/** @brief Enable an item or disable it. @return false past the end. */
TB_API bool tb_menu_set_item_enabled(tb_widget menu, size_t index, bool enabled);

/** @brief The item lit, by the mouse or the arrows; SIZE_MAX for none. */
TB_API size_t tb_menu_active(tb_widget menu);

/** @brief Choose an item as a click does. @return false past the end, or
 * for a separator or a disabled item. */
TB_API bool tb_menu_invoke(tb_widget menu, size_t index);

/** @brief Show a menu as a context menu, its top left at a point, after
 * closing the popups shown. @param position Units of the GUI: the
 * event's `position`. @return false for a stale handle or another kind. */
TB_API bool tb_menu_popup(tb_widget menu, tb_vec2 position);

/** @brief Make a menu bar: a row of titles, each of which opens its menu
 * below it when pressed. With a menu open, the mouse over another title
 * opens its menu, and Left and Right go from menu to menu.
 * @return None when `parent` is stale. */
TB_API tb_widget tb_menubar(tb_widget parent);

/** @brief Add a title to a menu bar, at its end. @param label Required;
 * copied; `&` marks its mnemonic. @param menu Its menu, a child of the
 * menu bar. @return Its index; SIZE_MAX for a stale handle or another kind. */
TB_API size_t tb_menubar_add(tb_widget menubar, const char *label, tb_widget menu);

/* ---------------------------------------------------------------------------
 * Tooltips.
 * ------------------------------------------------------------------------- */

/** @brief A tooltip: text shown below the mouse when it has rested on a
 * widget for a while, as a passive popup, and hidden when it moves to
 * another widget, a button is pressed, or a key goes down. The widget's
 * children without a tooltip of their own show its. Properties: `text`,
 * `delay`. */
typedef struct tb_tooltip_desc
{
    const char *text; /**< Copied; NULL or empty shows none. */
    float delay;      /**< Seconds the mouse rests first; 0 is the theme's. */
} tb_tooltip_desc;

/** @brief Give a widget a tooltip: a child of it, never in its grid.
 * @param desc Optional. @return The tooltip; none when `widget` is stale. */
TB_API tb_widget tb_tooltip(tb_widget widget, const tb_tooltip_desc *desc);

/** @brief The kinds of the containers and overlays. @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_scrollframe_class(void);
/** @brief See tb_scrollframe_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_scrollbar_class(void);
/** @brief See tb_scrollframe_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_listbox_class(void);
/** @brief See tb_scrollframe_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_menu_class(void);
/** @brief See tb_scrollframe_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_menubar_class(void);
/** @brief See tb_scrollframe_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_tooltip_class(void);

#endif
