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
 * const tb_viewport viewport = tb_renderer_viewport(renderer);
 * tb_gui_set_viewport(gui, &viewport);
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
 * **Units and scaling.** Everything is laid out in units, in the viewport
 * that the app's display policy gives the frame (badger/viewport.h): the
 * renderer's (tb_gui_set_viewport()), so that the GUI, the 3D scene and the
 * canvas agree. A tool's units are logical pixels at the display's scale; a
 * game's are its design units, fitted to the frame as the policy says.
 * Edges land on whole pixels at any scale; text is measured and drawn at
 * the frame's own pixels. The mouse over the viewport's bars reaches no
 * widget.
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
 * **Scrolling and popups.** A kind may show its children through a
 * viewport, scrolled and clipped (tb_widget_class::viewport); a widget may
 * be shown as a popup in front of every other (tb_widget_popup()).
 *
 * **Styles.** Every kind draws by a style resolved from themes of style
 * classes, by state, after ttk's styles and Godot's Theme: the tool themes'
 * flat look, or a game's of nine-slice images. See #tb_theme.
 *
 * **Layout files.** A widget tree is written as JSON (tb_gui_save()) and
 * built from it (tb_gui_load()), its commands and variables connected by
 * name, and found by path (tb_widget_find()); a file is loaded again as it
 * changes (tb_gui_watch()).
 *
 * **Navigation.** The arrows, and a gamepad's actions the game feeds
 * (tb_gui_feed_navigation()), move the focus to the nearest widget in their
 * direction, or to a widget's neighbours; Accept activates and Back goes
 * back. Keys are mapped to actions by data the game changes
 * (tb_gui_map_key()). See #tb_gui_nav.

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

/** @brief What a kind draws, as data: see #tb_widget_style_desc. */
typedef struct tb_widget_style_desc tb_widget_style_desc;

/** @brief What a kind draws with, during tb_gui_update(): see
 * tb_gui_paint_fill(). */
typedef struct tb_gui_paint tb_gui_paint;

/** @brief An event: see #tb_gui_event_kind. */
typedef struct tb_gui_event tb_gui_event;

/**
 * @brief A theme: style classes, each holding values by name and by state,
 * after Tk's ttk styles and Godot's Theme. Made by tb_theme_create(),
 * tb_theme_create_tool(), tb_theme_create_light(), or
 * tb_theme_create_game(); owned by the caller, who destroys it with
 * tb_theme_destroy() once no GUI uses it.
 *
 * **Classes.** A widget's style class is its `style` property, or its
 * kind's (#tb_widget_style_desc), such as "Button". A class falls back
 * along its name, as ttk's do: "Primary.Button", then "Button", then the
 * root class ".", where values every kind shares are kept. A widget whose
 * class does not end in its kind's has its kind's class added before ".".
 *
 * **States.** Each value is set for a set of #tb_widget_state_flags: none
 * for a widget at rest, #TB_WIDGET_HOVERED for one under the mouse,
 * #TB_WIDGET_HOVERED | #TB_WIDGET_PRESSED for one held under it. A widget
 * takes, for each value, the entry whose states it has every one of, and
 * of those the one whose states rank highest: disabled outranks selected,
 * selected pressed, pressed hovered, hovered engaged, engaged
 * focus-visible, and focus-visible focused, so that a state outranks any
 * mix of those below it. Of entries of the same states, the
 * nearest wins: the widget's own overrides, then the nearest theme, then
 * the class nearest its own. So an entry for a state in a farther theme
 * outranks one at rest in a nearer, as Godot's styles of a button do: a
 * theme that changes a look at rest gives its other states too.
 *
 * **Themes in the tree.** A widget's themes are its own
 * (tb_widget_set_style_theme()), then each ancestor's, then the GUI's
 * (tb_gui_set_style_theme()), then the GUI's default: the tool theme of
 * its palette (tb_theme_create_tool()). A value no nearer theme has comes
 * from a farther one, so a theme need only give what differs.
 */
typedef struct tb_theme tb_theme;

/* ---------------------------------------------------------------------------
 * Scaling.
 * ------------------------------------------------------------------------- */

/**
 * @brief How a GUI's units are placed on the frame: its viewport's
 * (tb_gui_set_viewport()), times the GUI's own scale
 * (tb_gui_options::scale).
 *
 * The scale is the same both ways. In a viewport stretched by the `ignore`
 * aspect, it is the smaller of the viewport's two, so that text is never
 * squashed, and the root still fills the viewport: it has more units along
 * the axis stretched more.
 */
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
    /** The palette: its colors, fonts, size, and spacing; copied. NULL is
     * tb_ui_theme_dark(), whose fonts are then given by tb_gui_set_theme()
     * before text is drawn. Its fonts are the GUI's own, and its tool
     * theme (tb_theme_create_tool()) the GUI's default style theme. */
    const tb_ui_theme *theme;
    /** Optional: the catalog the fonts that widgets ask for by spec are
     * opened from (#tb_font_spec). Borrowed for the GUI's life. NULL draws
     * every text in the theme's font. */
    const tb_font_catalog *catalog;
    /** A factor on top of the viewport's scale, this GUI's alone, as an
     * options menu's interface scale: 2 draws every unit twice as large,
     * and gives the root half as many units. 0 is 1. */
    float scale;
    int32_t layer; /**< The canvas layer; 0 is #TB_GUI_DEFAULT_LAYER. */
    /** Optional: the theme its widgets are drawn by, before the default
     * (tb_gui_set_style_theme()); borrowed. */
    tb_theme *style_theme;
} tb_gui_options;

/**
 * @brief Make a GUI, with its root: a frame with no padding, as large as
 * its viewport (tb_gui_set_viewport()).
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

/** @brief Use another palette; the GUI's default style theme is made
 * again from it, and the widgets are laid out again.
 * @param theme Required; copied, its fonts borrowed. */
TB_API void tb_gui_set_theme(tb_gui *gui, const tb_ui_theme *theme);

/** @brief The palette in use: its fonts, and what the default style theme
 * is made of. @return Borrowed until the palette is set again. */
TB_API const tb_ui_theme *tb_gui_theme(const tb_gui *gui);

/**
 * @brief Give the GUI the viewport it fills: the renderer's
 * (tb_renderer_viewport()), so that the GUI and the 3D scene agree; called
 * each frame, before tb_gui_update(). Its units are the viewport's, the
 * root is as large as it, and the mouse outside it reaches no widget. The
 * widgets are laid out again when the view changes.
 * @param viewport Required; copied.
 */
TB_API void tb_gui_set_viewport(tb_gui *gui, const tb_viewport *viewport);

/** @brief The viewport the GUI fills (tb_gui_set_viewport()); before one is
 * given, a viewport of no size. */
TB_API tb_viewport tb_gui_viewport_of(const tb_gui *gui);

/**
 * @brief Give the GUI a whole frame in logical pixels, without a renderer:
 * tb_gui_set_viewport() with the viewport of #TB_VIEWPORT_POLICY_LOGICAL,
 * for tests and tools that have no design size.
 * @param width Pixels across.
 * @param height Pixels down.
 * @param display_scale The display's scale; 0 is 1.
 */
TB_API void tb_gui_resize(tb_gui *gui, uint32_t width, uint32_t height, float display_scale);

/** @brief Set the factor on top of the viewport's scale
 * (tb_gui_options::scale): an options menu's interface scale.
 * @param scale More than 0. */
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
    /** The mouse moved over it, or anywhere while a button pressed on it is
     * held: a drag. */
    TB_GUI_EVENT_MOTION,
    /** Text is about to change, Tk's `validatecommand`: `text` is the text
     * proposed, and tb_gui_refuse_edit() keeps the old. */
    TB_GUI_EVENT_VALIDATE,
    TB_GUI_EVENT_POPUP_CLOSE, /**< It was closed as a popup: see tb_widget_popup(). */
    /** Back was asked for, and nothing used it: no widget engaged, popup,
     * or kind. Given to the widget with the focus, or else the modal widget
     * in front or the root, then to its ancestors: bind it to go to the
     * previous menu. See #TB_GUI_NAV_BACK. */
    TB_GUI_EVENT_BACK,
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

/**
 * @brief From a function bound to an event: the event is used, as Tk's
 * `break` and Godot's `accept_event()`. The functions bound after it, the
 * widget's kind, and its ancestors are not given it, and a key used so is
 * not taken as navigation.
 * @param event The event the function was given; required.
 */
TB_API void tb_gui_accept_event(const tb_gui_event *event);

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
    TB_WIDGET_CLASS_FOCUS_ON_CLICK = 1u << 1,
    /** It keeps the keys of navigation only while it is engaged: a field
     * to type in, or a list. Focus moved to it by navigation leaves it
     * unengaged, and its arrows move the focus on; Accept engages it, and
     * Back lets it go. See tb_widget_engaged(). */
    TB_WIDGET_CLASS_ENGAGES = 1u << 2
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
    /** What it draws by: its style class, states, and elements. NULL draws
     * by the root class "." alone. */
    const tb_widget_style_desc *style;
    /** For a kind whose widgets hold what no property of it expresses, set
     * by functions (a menu's items, a tree's): what a widget holds now, a
     * short phrase such as "menu items", or NULL when it holds none. Layout
     * files cannot hold it yet, and saving such a widget fails rather than
     * drop it (tb_gui_save()). NULL for a kind whose properties say all. */
    const char *(*unsaved)(tb_widget widget, const void *data);
    /** A layout file built it, and its children: take from them what they
     * hold for it, as a notebook makes its children its tabs. NULL for a
     * kind that needs nothing. */
    void (*loaded)(tb_widget widget, void *data);
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
    TB_WIDGET_HOVERED = 1u << 0,  /**< The mouse is over it. */
    TB_WIDGET_PRESSED = 1u << 1,  /**< A button went down on it and is held. */
    TB_WIDGET_FOCUSED = 1u << 2,  /**< It has the keyboard's focus. */
    TB_WIDGET_DISABLED = 1u << 3, /**< It, or an ancestor, is disabled. */
    /** It has the focus, and the focus is shown: the keyboard or a gamepad
     * moved it last, not the mouse, as CSS's `:focus-visible`. Draw the
     * focus ring by this. */
    TB_WIDGET_FOCUS_VISIBLE = 1u << 4,
    /** It has the focus, and is engaged: see tb_widget_engaged(). */
    TB_WIDGET_ENGAGED = 1u << 5,
    /** Chosen: a check button ticked, a tab or a row chosen. Never given by
     * tb_widget_state(); kinds add it when they ask for a style
     * (tb_style_in()). */
    TB_WIDGET_SELECTED = 1u << 6
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
 * Navigation: the focus moved by the keyboard and a gamepad.
 * ------------------------------------------------------------------------- */

/**
 * @brief What a player asks of a menu with the keys or a gamepad, after
 * Godot's `ui_*` actions and Unreal's navigation: a direction, Accept,
 * Back, the next or previous widget, and the next or previous tab.
 *
 * Keys become actions through the GUI's key map (tb_gui_map_key()); a game
 * feeds a gamepad's (tb_gui_feed_navigation()): the d-pad and the left
 * stick for the directions, A or Cross for Accept, B or Circle for Back,
 * and the shoulder buttons for the tabs.
 *
 * An action is first given to the widget with the focus as the key it
 * stands for (Up, Down, Left, Right, Enter, Escape, Tab, Shift+Tab,
 * Control+Tab, Control+Shift+Tab), then to its ancestors, as a key is, so
 * that a kind that uses the arrows itself, as a slider does, uses a
 * gamepad's too. What no kind uses is then done by the GUI:
 *
 * - a direction moves the focus to the widget's neighbour on that side, or
 *   else the nearest widget that way (tb_gui_focus_direction());
 * - Accept invokes the widget with the focus (tb_widget_invoke()), unless
 *   its kind engages (#TB_WIDGET_CLASS_ENGAGES), which Accept engages
 *   before it is given anything;
 * - Back lets go of the widget engaged; or else closes the popup in front;
 *   or else is sent as #TB_GUI_EVENT_BACK; or else hides the modal widget
 *   in front;
 * - the next and previous widget move the focus as Tab does.
 *
 * With nothing focused, a direction or Accept gives the focus to the
 * default (tb_widget_focus_default()).
 */
typedef enum tb_gui_nav
{
    TB_GUI_NAV_NONE,         /**< No action: a key not mapped. */
    TB_GUI_NAV_UP,           /**< Up: the arrow, the d-pad, the left stick. */
    TB_GUI_NAV_DOWN,         /**< Down. */
    TB_GUI_NAV_LEFT,         /**< Left. */
    TB_GUI_NAV_RIGHT,        /**< Right. */
    TB_GUI_NAV_ACCEPT,       /**< Enter, Space; A or Cross. */
    TB_GUI_NAV_BACK,         /**< Escape; B or Circle. */
    TB_GUI_NAV_NEXT,         /**< Tab: the next widget in the order of the tree. */
    TB_GUI_NAV_PREVIOUS,     /**< Shift+Tab. */
    TB_GUI_NAV_TAB_NEXT,     /**< A notebook's next tab: Control+Tab; the right shoulder. */
    TB_GUI_NAV_TAB_PREVIOUS, /**< Its previous tab: Control+Shift+Tab; the left shoulder. */
    TB_GUI_NAV_COUNT         /**< How many there are; not an action. */
} tb_gui_nav;

/** @brief The axes along which a container's navigation wraps around: its
 * `focus_wrap` property. */
typedef enum tb_gui_focus_wrap
{
    TB_GUI_WRAP_HORIZONTAL = 1u << 0, /**< Left from its first goes to its last, and back. */
    TB_GUI_WRAP_VERTICAL = 1u << 1    /**< Up from its top goes to its bottom, and back. */
} tb_gui_focus_wrap;

/**
 * @brief Give the GUI an action from a gamepad, or from anything else that
 * is not a key, to be handled by the next tb_gui_update() in its order
 * among the window events fed. A game repeats a direction held, as a key
 * repeats.
 * @param action Any but #TB_GUI_NAV_NONE and #TB_GUI_NAV_COUNT, which are
 * ignored.
 */
TB_API void tb_gui_feed_navigation(tb_gui *gui, tb_gui_nav action);

/**
 * @brief Map a key, with exactly the modifiers held, to an action, in place
 * of what it was mapped to: the keys a player rebinds. A GUI starts with
 * the arrows for the directions, Enter and Space for Accept, Escape for
 * Back, Tab and Shift+Tab for the next and previous widget, and
 * Control+Tab, Control+Page Down and their Shift and Page Up for the tabs.
 * @param modifiers #tb_modifier bits; a key with others held is another key.
 * @param action #TB_GUI_NAV_NONE unmaps the key.
 * @return false, changing nothing, for a key or action out of range.
 */
TB_API bool tb_gui_map_key(tb_gui *gui, tb_key key, uint32_t modifiers, tb_gui_nav action);

/** @brief The action a key with exactly some modifiers is mapped to;
 * #TB_GUI_NAV_NONE for none. */
TB_API tb_gui_nav tb_gui_key_action(const tb_gui *gui, tb_key key, uint32_t modifiers);

/** @brief Map the keys as a GUI starts: see tb_gui_map_key(). */
TB_API void tb_gui_reset_key_map(tb_gui *gui);

/** @brief Unmap every key: the GUI then navigates only by the actions fed. */
TB_API void tb_gui_clear_key_map(tb_gui *gui);

/**
 * @brief Move the focus as a direction does, now: to the focused widget's
 * neighbour on that side when it has one that takes the focus, or else to
 * the nearest widget that takes the focus that way, by geometry, as Godot
 * and Unreal choose it:
 *
 * - a widget is a candidate when it lies wholly past the focused widget's
 *   edge on that side, within the popup or modal widget it is in, or the
 *   GUI (Godot's rule);
 * - a candidate in line with it, overlapping it across the direction, wins
 *   over any that is not (Unreal's swept band);
 * - among those, the nearest wins, by the shortest distance between the
 *   two rectangles (Godot's), and a tie goes to the first in the order of
 *   the tree;
 * - when the nearest ancestor that wraps along that axis (`focus_wrap`)
 *   has no candidate inside it, the search goes around: from its far
 *   edge, among its own widgets.
 *
 * A widget that holds widgets that take the focus, as a notebook does its
 * pages, is placed by its part outside them, its tabs. With nothing focused
 * the focus goes to the default (tb_widget_focus_default()).
 * @param direction #TB_GUI_NAV_UP, _DOWN, _LEFT, or _RIGHT.
 * @return Whether the focus moved.
 */
TB_API bool tb_gui_focus_direction(tb_gui *gui, tb_gui_nav direction);

/**
 * @brief Name the widget the focus goes to from one, on a side, instead of
 * the nearest by geometry: Godot's `focus_neighbor_*` and `focus_next`,
 * Unreal's explicit navigation. A neighbour hidden, disabled, or stale is
 * passed over, and the geometry decides.
 *
 * The same neighbours are the common properties `focus_up`, `focus_down`,
 * `focus_left`, `focus_right`, `focus_next`, and `focus_previous`, as text:
 * a widget's path (tb_widget_find()), looked up from the widget's parent,
 * then from each ancestor up to the root, the nearest that has it; so a
 * layout file names them. The last given wins, by handle or by path. A
 * neighbour given by handle reads as its path from the root
 * (tb_widget_path()), and so is saved; as none when it has no path.
 * @param side #TB_GUI_NAV_UP, _DOWN, _LEFT, _RIGHT; or _NEXT and
 * _PREVIOUS, for Tab and Shift+Tab.
 * @param neighbor The widget; none to forget the side's.
 * @return false for a stale handle, or another side.
 */
TB_API bool tb_widget_set_focus_neighbor(tb_widget widget, tb_gui_nav side, tb_widget neighbor);

/** @brief A widget's neighbour on a side, as set; none when it has none or
 * it is stale. */
TB_API tb_widget tb_widget_focus_neighbor(tb_widget widget, tb_gui_nav side);

/**
 * @brief Give the focus to a screen's first widget: the first in the order
 * of the tree, within a widget, whose `default_focus` property is true and
 * that takes the focus; or else the first that takes the focus. A modal
 * widget shown gives it to its default so, when it has one.
 * @param container Where to look, itself included.
 * @return false, changing nothing, when no widget there takes the focus.
 */
TB_API bool tb_widget_focus_default(tb_widget container);

/**
 * @brief Whether the widget with the focus is engaged: whether it has the
 * keys of navigation to itself, as an entry has the arrows for its caret
 * and a list for its items. Focus given by the mouse, by Tab, or by
 * tb_widget_focus() engages it; focus moved by a direction, or given to a
 * default, does not. A kind of #TB_WIDGET_CLASS_ENGAGES is given none of
 * the keys mapped to actions while it is not engaged: Accept engages it,
 * and any other key it uses, or text typed. Back lets it go.
 * @return false for a widget without the focus.
 */
TB_API bool tb_widget_engaged(tb_widget widget);

/** @brief Engage the widget with the focus, or let it go.
 * @return false, changing nothing, for a widget without the focus. */
TB_API bool tb_widget_engage(tb_widget widget, bool engaged);

/** @brief Whether the focus is shown: true once the keyboard or a gamepad
 * navigates, false once a mouse button is pressed (#TB_WIDGET_FOCUS_VISIBLE). */
TB_API bool tb_gui_focus_visible(const tb_gui *gui);

/** @brief Show the focus, or hide it, until the next input changes it. */
TB_API void tb_gui_set_focus_visible(tb_gui *gui, bool visible);

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
 * ancestors, and Escape that it does not use closes it; Tab that it does
 * not use closes them all and moves the focus. A popup closed is sent
 * #TB_GUI_EVENT_POPUP_CLOSE; one hidden or destroyed is closed
 * without it; one whose parent is hidden, disabled, or destroyed is closed
 * with it. A press in a popup leaves the focus where it is, unless it is on
 * a kind that takes the focus on a click.
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
    /** The font: a weight of 0 is the style's font; a size of 0 the
     * style's size. Opened from the GUI's catalog. */
    tb_font_spec font;
    float wrap_length;     /**< Units lines wrap at; 0 for none. */
    tb_font_align justify; /**< How lines sit against one another. */
    tb_gui_anchor anchor;  /**< Where the text sits in the rectangle. */
    /** Optional: a font of the caller's, drawn in place of the spec's face,
     * at the spec's size or the style's: a font browser's preview of any
     * font. Borrowed: open until the frame that draws it ends, and forgotten
     * by the canvas (tb_canvas_forget_font()) before it is closed. */
    const tb_font *face;
} tb_gui_text_block;

/** @brief The size text takes, in units, measured at the frame's pixels,
 * in the root's style (tb_widget_measure_text()).
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

/** @brief An image stretched over a rectangle and tinted, within the
 * widget's clip. @param paint Required. @param image Required; alive until
 * the frame that draws it ends. */
TB_API void tb_gui_paint_image(tb_gui_paint *paint, const tb_image *image, tb_canvas_rect rect,
                               tb_vec4 tint);

/* ---------------------------------------------------------------------------
 * Frames, labels, and buttons.
 * ------------------------------------------------------------------------- */

/** @brief A frame: a container of widgets in its grid. Properties:
 * `padding`, `background`, `border`. */
typedef struct tb_frame_desc
{
    float padding;   /**< Units between its edges and its grid. */
    bool background; /**< Filled with its style's background; it then stops the mouse. */
    bool border;     /**< Outlined with its style's border. */
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
    tb_font_spec font;     /**< Its font; zero is its style's. */
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

/**
 * @brief Bind a variable to an engine setting that takes its value only
 * when it is applied, as an options menu's Apply does: the variable takes
 * the setting's value now; setting the variable holds its value, which the
 * setting takes at tb_var_apply(), and tb_var_revert() lets go of.
 *
 * A value is checked as the setting would check it (its range, its
 * choices, its length), and refused as tb_var_bind_setting() refuses it;
 * text is kept as typed, and tb_var_setting_accepts() says whether the
 * setting would take it. While the variable holds no value of its own, it
 * follows the setting's changes as a variable bound at once does; while it
 * holds one, it keeps it.
 *
 * @param setting Required; borrowed: it outlives the binding, or the variable.
 * @return false, binding nothing, as tb_var_bind_setting() does.
 */
TB_API bool tb_var_bind_setting_deferred(tb_var var, tb_setting *setting);

/** @brief Whether a variable is bound to its setting deferred. */
TB_API bool tb_var_deferred(tb_var var);

/** @brief Whether a deferred variable holds a value its setting does not:
 * a change not yet applied. False for any other variable. */
TB_API bool tb_var_pending(tb_var var);

/** @brief Give a deferred variable's setting the value it holds, at the
 * setting's runtime layer; the variable then takes the setting's value.
 * @return true when the setting took it, or there was nothing to apply;
 * false, the variable keeping its value, when the setting refused it, or
 * the handle is stale. */
TB_API bool tb_var_apply(tb_var var);

/** @brief Give a bound variable its setting's value, letting go of what a
 * deferred one held. @return false when it is bound to none, or stale. */
TB_API bool tb_var_revert(tb_var var);

/**
 * @brief Give a bound variable its setting's default: the value of the
 * game's default file, or else the setting's definition, without the
 * player's choices or the command line's, as an options menu's Defaults.
 * A deferred variable holds it until it is applied; any other gives it to
 * its setting at once.
 * @return false when it is bound to none, the handle is stale, or the value
 * does not convert to the variable's type.
 */
TB_API bool tb_var_restore_default(tb_var var);

/** @brief Apply every deferred variable of a GUI (tb_var_apply()), in the
 * order they were made. @return false when a setting refused a value; the
 * others are applied all the same. */
TB_API bool tb_gui_apply_settings(tb_gui *gui);

/** @brief Revert every deferred variable of a GUI (tb_var_revert()). */
TB_API void tb_gui_revert_settings(tb_gui *gui);

/** @brief Give every deferred variable of a GUI its setting's default
 * (tb_var_restore_default()), to hold until applied. */
TB_API void tb_gui_restore_default_settings(tb_gui *gui);

/** @brief Whether any deferred variable of a GUI holds a change not yet
 * applied (tb_var_pending()). */
TB_API bool tb_gui_settings_pending(tb_gui *gui);

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

/** @brief Make a check button. A click, Space or Enter while it has the
 * focus, or tb_widget_invoke() toggles it: it sets its variable, sends
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
 * A click, Space, Enter, or tb_widget_invoke() chooses it; the arrow keys
 * choose the one before or after it among its siblings of its variable, and
 * move the focus there; past the first or the last, they move the focus on.
 * @param desc Optional. @return None when `parent` is stale. */
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
 * gives it the focus, and a drag moves it; the arrows along it, Left or Up
 * toward `from`, Right or Down toward `to`, step by its resolution (or a
 * hundredth of the range), and those across it move the focus on; Page Up
 * and Page Down step by its big increment, Home and End to its ends. Each
 * change sends #TB_GUI_EVENT_CHANGED.
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
 * @brief Make a combo box. A click, Space, Enter, or Alt+Down opens its
 * list over every widget; there the mouse or Up and Down pick a value, and
 * a click, Enter, or Space chooses it, and Escape, Alt+Up, a click
 * outside, or the loss of the focus closes it. Closed and engaged
 * (tb_widget_engaged()), Up and Down choose the value before or after;
 * reached by navigation, they move the focus on. The wheel chooses while
 * it has the focus. Each choice sends #TB_GUI_EVENT_CHANGED.
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
 * Scroll frames and scroll bars.
 * ------------------------------------------------------------------------- */

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
    bool border;     /**< Outlined with its style's border. */
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
    tb_gui_orient orient;     /**< Horizontal unless chosen. */
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

/* ---------------------------------------------------------------------------
 * Trees.
 * ------------------------------------------------------------------------- */

/** @brief An item of a tree view: an id, never reused while the tree
 * lives. #TB_TREE_ROOT is the root, the parent of the items at the top. */
typedef uint32_t tb_tree_item;

/** @brief The root of a tree view: no item. */
#define TB_TREE_ROOT 0u

/** @brief The tree's own column, before the columns of values. */
#define TB_TREEVIEW_TREE_COLUMN SIZE_MAX

/** @brief A tree view, after Tk's `ttk.Treeview`: items in a hierarchy,
 * each with its text in the tree's column and a value in each other
 * column, under headings whose right edges are dragged to resize them. An
 * item with children shows a mark, pressed to open or close it, as a double
 * click does. It takes the focus on a click and by Tab; the arrows, Page Up
 * and Down, Home, and End move, Right opens an item or goes into it, Left
 * closes it or goes to its parent, and Space chooses. Its items are data,
 * not widgets: only the rows seen are drawn. Properties: `selectmode`,
 * `height`, `no_headings`, `heading`, `offset`, `command`, `open_command`. */
typedef struct tb_treeview_desc
{
    /** How items are chosen, as a list's are (#tb_listbox_selectmode). */
    tb_listbox_selectmode selectmode;
    /** Rows it shows, scrolling the rest; 0 shows every row. */
    int32_t height;
    bool no_headings;    /**< Its headings are not shown. */
    const char *heading; /**< The tree's column's heading; copied; NULL for none. */
    float tree_width;    /**< Units the tree's column is wide; 0 is 200. */
    /** Called when the user changes the selection, Tk's `<<TreeviewSelect>>`. */
    tb_gui_callback *command;
    /** Called when the user opens or closes an item, which is then the
     * focus item, Tk's `<<TreeviewOpen>>` and `<<TreeviewClose>>`. */
    tb_gui_callback *open_command;
    void *user; /**< Given to both. */
} tb_treeview_desc;

/** @brief Make a tree view, with its tree's column only.
 * @param desc Optional. @return None when `parent` is stale. */
TB_API tb_widget tb_treeview(tb_widget parent, const tb_treeview_desc *desc);

/** @brief A column of values, after the tree's. */
typedef struct tb_treeview_column_desc
{
    const char *heading; /**< Copied; NULL for none. */
    float width;         /**< Units; 0 is 100. */
    float minwidth;      /**< The least it is dragged to; 0 is 20. */
} tb_treeview_column_desc;

/** @brief Add a column of values at the right. @param desc Optional.
 * @return Its index among the columns of values, from 0; SIZE_MAX for a
 * stale handle, another kind, or a negative width. */
TB_API size_t tb_treeview_add_column(tb_widget treeview, const tb_treeview_column_desc *desc);

/** @brief A column's width, in units. @param column An index from
 * tb_treeview_add_column(), or #TB_TREEVIEW_TREE_COLUMN. @return 0 for
 * none. */
TB_API float tb_treeview_column_width(tb_widget treeview, size_t column);

/** @brief Set a column's width, at least its least. @return false for none. */
TB_API bool tb_treeview_set_column_width(tb_widget treeview, size_t column, float width);

/** @brief An item to insert. */
typedef struct tb_tree_item_desc
{
    const char *text;          /**< In the tree's column; copied; NULL for none. */
    const char *const *values; /**< One for each column of values, copied; NULL for none. */
    size_t value_count;        /**< How many. */
    bool open;                 /**< Its children are shown. */
} tb_tree_item_desc;

/**
 * @brief Insert an item, Tk's `insert`.
 * @param parent #TB_TREE_ROOT, or an item.
 * @param index Before which child of the parent; past the last, or
 * SIZE_MAX, is at the end.
 * @param desc Optional.
 * @return The item; #TB_TREE_ROOT for a stale handle, another kind, or a
 * parent that is not there.
 */
TB_API tb_tree_item tb_treeview_insert(tb_widget treeview, tb_tree_item parent, size_t index,
                                       const tb_tree_item_desc *desc);

/** @brief Delete an item and every item in it. @return false for the root
 * or an item not there. */
TB_API bool tb_treeview_delete(tb_widget treeview, tb_tree_item item);

/** @brief Whether an item is there; false for the root. */
TB_API bool tb_treeview_exists(tb_widget treeview, tb_tree_item item);

/** @brief An item's parent; #TB_TREE_ROOT for the items at the top, and
 * for none. */
TB_API tb_tree_item tb_treeview_parent(tb_widget treeview, tb_tree_item item);

/** @brief An item's first child, or the root's; #TB_TREE_ROOT for none. */
TB_API tb_tree_item tb_treeview_first_child(tb_widget treeview, tb_tree_item item);

/** @brief The child of its parent after an item; #TB_TREE_ROOT for none. */
TB_API tb_tree_item tb_treeview_next_sibling(tb_widget treeview, tb_tree_item item);

/** @brief An item's text. @return Borrowed until it changes; empty for none. */
TB_API tb_text tb_treeview_text(tb_widget treeview, tb_tree_item item);

/** @brief Set an item's text. @param text Required; copied. @return false
 * for none. */
TB_API bool tb_treeview_set_text(tb_widget treeview, tb_tree_item item, const char *text);

/** @brief An item's value in a column of values. @return Borrowed until it
 * changes; empty for none. */
TB_API tb_text tb_treeview_value(tb_widget treeview, tb_tree_item item, size_t column);

/** @brief Set an item's value in a column of values. @param text Required;
 * copied. @return false for none. */
TB_API bool tb_treeview_set_value(tb_widget treeview, tb_tree_item item, size_t column,
                                  const char *text);

/** @brief Open an item, showing its children, or close it, without the
 * open command. @return false for none. */
TB_API bool tb_treeview_set_open(tb_widget treeview, tb_tree_item item, bool open);

/** @brief Whether an item is open. */
TB_API bool tb_treeview_is_open(tb_widget treeview, tb_tree_item item);

/** @brief Choose an item or not, without the command; choosing one in a
 * mode of one leaves it alone chosen. @return false for none. */
TB_API bool tb_treeview_select(tb_widget treeview, tb_tree_item item, bool selected);

/** @brief Choose no item. */
TB_API void tb_treeview_clear_selection(tb_widget treeview);

/** @brief Whether an item is chosen. */
TB_API bool tb_treeview_selected(tb_widget treeview, tb_tree_item item);

/** @brief The items chosen, in the order made. @param[out] items Optional
 * when `capacity` is 0. @return How many are chosen. */
TB_API size_t tb_treeview_selection(tb_widget treeview, tb_tree_item *items, size_t capacity);

/** @brief The item the keyboard is at, Tk's `focus`; #TB_TREE_ROOT for none. */
TB_API tb_tree_item tb_treeview_focus_item(tb_widget treeview);

/** @brief Put the keyboard at an item. @return false for none. */
TB_API bool tb_treeview_set_focus_item(tb_widget treeview, tb_tree_item item);

/** @brief Open an item's ancestors, and scroll the tree, and each ancestor
 * that scrolls, to show it, Tk's `see`. @return false for none. */
TB_API bool tb_treeview_see(tb_widget treeview, tb_tree_item item);

/** @brief The item whose row is at a height, in units of the GUI, Tk's
 * `identify row`; #TB_TREE_ROOT for none. */
TB_API tb_tree_item tb_treeview_identify_row(tb_widget treeview, float y);

/* ---------------------------------------------------------------------------
 * Notebooks.
 * ------------------------------------------------------------------------- */

/** @brief A notebook, after Tk's `ttk.Notebook`: a row of tabs over a page,
 * each page a child widget, only the chosen one shown. A tab is chosen by a
 * click, by Control+Tab and Control+Shift+Tab or Control+Page Up and Down
 * from a widget in it with the focus, and by Left and Right while it has
 * the focus itself. Its natural size is its chosen page's. Properties:
 * `selected`, `padding`, `command`. */
typedef struct tb_notebook_desc
{
    float padding;            /**< Units between its edges and its pages. */
    tb_gui_callback *command; /**< Called when the user chooses another tab. */
    void *user;               /**< Given to the command. */
} tb_notebook_desc;

/** @brief Make a notebook. @param desc Optional. @return None when
 * `parent` is stale. */
TB_API tb_widget tb_notebook(tb_widget parent, const tb_notebook_desc *desc);

/**
 * @brief Add a page, Tk's `add`: a child of the notebook, placed in its
 * grid, shown when its tab is chosen; the first page added is chosen. A
 * page destroyed takes its tab with it.
 * @param text Its tab's text; required; copied.
 * @return Its tab's index; SIZE_MAX for a stale handle, another kind, or a
 * page that is not its child.
 */
TB_API size_t tb_notebook_add(tb_widget notebook, tb_widget page, const char *text);

/** @brief How many tabs a notebook has. */
TB_API size_t tb_notebook_tab_count(tb_widget notebook);

/** @brief The tab chosen; SIZE_MAX for none. */
TB_API size_t tb_notebook_selected(tb_widget notebook);

/** @brief Choose a tab, without the command. @return false past the end. */
TB_API bool tb_notebook_select(tb_widget notebook, size_t index);

/** @brief A tab's page; none past the end. */
TB_API tb_widget tb_notebook_page(tb_widget notebook, size_t index);

/** @brief Set a tab's text. @param text Required; copied. @return false
 * past the end. */
TB_API bool tb_notebook_set_tab_text(tb_widget notebook, size_t index, const char *text);

/* ---------------------------------------------------------------------------
 * Paned windows.
 * ------------------------------------------------------------------------- */

/** @brief A paned window, after Tk's `ttk.Panedwindow`: panes side by side
 * (horizontal) or one above another (vertical), with a sash between each
 * two that is dragged to share their space out. Each pane keeps its share
 * of the space as the window is resized, and is never less than its least
 * size. It lays its panes out with its grid, as every widget does.
 * Properties: `orient`, `width`, `height`. */
typedef struct tb_panedwindow_desc
{
    tb_gui_orient orient; /**< Horizontal unless chosen: panes side by side. */
    float width;          /**< Units across it wants; 0 is its panes'. */
    float height;         /**< Units down it wants; 0 is its panes'. */
} tb_panedwindow_desc;

/** @brief Make a paned window. @param desc Optional. @return None when
 * `parent` is stale. */
TB_API tb_widget tb_panedwindow(tb_widget parent, const tb_panedwindow_desc *desc);

/** @brief How a pane takes space. Zero is a share of 1 and no least size. */
typedef struct tb_pane_options
{
    float weight;  /**< Its share of the space, against the others'; 0 is 1. */
    float minsize; /**< The least it is, in units. */
} tb_pane_options;

/** @brief Add a pane at the end, Tk's `add`: a child of the paned window.
 * @param options Optional. @return Its index; SIZE_MAX for a stale handle,
 * another kind, a pane that is not its child, or a negative option. */
TB_API size_t tb_panedwindow_add(tb_widget panedwindow, tb_widget pane,
                                 const tb_pane_options *options);

/** @brief How many panes a paned window has. */
TB_API size_t tb_panedwindow_pane_count(tb_widget panedwindow);

/** @brief Where a sash starts along the panes' axis, in units of the GUI,
 * Tk's `sashpos`; laid out first. 0 for none. */
TB_API float tb_panedwindow_sash(tb_widget panedwindow, size_t sash);

/** @brief Move a sash as dragging it does, its panes kept at least their
 * least sizes. @param position Units of the GUI along the axis.
 * @return false for none. */
TB_API bool tb_panedwindow_set_sash(tb_widget panedwindow, size_t sash, float position);

/* ---------------------------------------------------------------------------
 * Message dialogs.
 * ------------------------------------------------------------------------- */

/** @brief The buttons of a message dialog. */
typedef enum tb_message_buttons
{
    TB_MESSAGE_OK,            /**< OK. */
    TB_MESSAGE_OK_CANCEL,     /**< OK and Cancel. */
    TB_MESSAGE_YES_NO,        /**< Yes and No. */
    TB_MESSAGE_YES_NO_CANCEL, /**< Yes, No, and Cancel. */
    TB_MESSAGE_RETRY_CANCEL   /**< Retry and Cancel. */
} tb_message_buttons;

/** @brief The button a message dialog was answered with. */
typedef enum tb_message_result
{
    TB_MESSAGE_RESULT_OK,
    TB_MESSAGE_RESULT_CANCEL,
    TB_MESSAGE_RESULT_YES,
    TB_MESSAGE_RESULT_NO,
    TB_MESSAGE_RESULT_RETRY
} tb_message_result;

/** @brief A message dialog's answer. @param result The button chosen.
 * @param user The dialog's argument. */
typedef void tb_message_callback(tb_message_result result, void *user);

/**
 * @brief Show a message dialog, after Tk's `messagebox`: a title, a
 * message, and a row of buttons, made of a frame, labels, and buttons in
 * grids, shown modally in the middle of the root's grid. The first button
 * has the focus, so Enter or Space chooses it; Escape chooses Cancel, No,
 * or OK, the last of them the dialog has. Choosing a button destroys the
 * dialog and calls the callback; destroying it otherwise calls nothing.
 * @param title Optional; NULL or empty for none; copied.
 * @param text Required; copied.
 * @param buttons Which.
 * @param callback Optional.
 * @param user Given to the callback.
 * @return The dialog, a widget of the kind "messagebox".
 */
TB_API tb_widget tb_gui_message_box(tb_gui *gui, const char *title, const char *text,
                                    tb_message_buttons buttons, tb_message_callback *callback,
                                    void *user);

/* ---------------------------------------------------------------------------
 * Images and canvases.
 * ------------------------------------------------------------------------- */

/** @brief How an image fills its widget, after CSS's `object-fit`. */
typedef enum tb_image_fit
{
    TB_IMAGE_FIT_NONE,      /**< Its own size, a pixel a unit. */
    TB_IMAGE_FIT_CONTAIN,   /**< As large as it fits whole, its shape kept. */
    TB_IMAGE_FIT_COVER,     /**< As small as it covers the widget, its shape kept; cut off. */
    TB_IMAGE_FIT_FILL,      /**< Stretched to the widget. */
    TB_IMAGE_FIT_SCALE_DOWN /**< As contain, but never larger than its own size. */
} tb_image_fit;

/**
 * @brief The rectangle an image of a size is drawn in to fit a rectangle,
 * placed by an anchor.
 * @param size The image's size, in units; zero draws nothing.
 * @par Threading Thread-safe.
 */
TB_API tb_canvas_rect tb_gui_fit_rect(tb_vec2 size, tb_canvas_rect rect, tb_image_fit fit,
                                      tb_gui_anchor anchor);

/** @brief An image in a widget: a canvas image (tb_image_create()), drawn
 * by its fit, placed by its anchor, and tinted. Properties: `fit`,
 * `anchor`, `tint`, `width`, `height`. */
typedef struct tb_image_widget_desc
{
    /** Borrowed; it outlives the widget, or is replaced first. NULL draws
     * nothing. */
    const tb_image *image;
    tb_image_fit fit;     /**< Its own size unless chosen. */
    tb_gui_anchor anchor; /**< Where it sits when it does not fill the widget. */
    float width;          /**< Units across it wants; 0 is the image's pixels. */
    float height;         /**< Units down it wants; 0 is the image's pixels. */
} tb_image_widget_desc;

/** @brief Make an image widget, tinted white. @param desc Optional.
 * @return None when `parent` is stale. */
TB_API tb_widget tb_image_widget(tb_widget parent, const tb_image_widget_desc *desc);

/** @brief Draw another image. @param image Borrowed; NULL for none.
 * @return false for a stale handle or another kind. */
TB_API bool tb_image_widget_set_image(tb_widget widget, const tb_image *image);

/** @brief The image drawn; NULL for none. */
TB_API const tb_image *tb_image_widget_image(tb_widget widget);

/** @brief Draw a canvas widget's content: called each update with the
 * paint functions (tb_gui_paint_fill() and the rest), clipped to it.
 * @param widget The canvas widget. @param paint Its rectangle and clip.
 * @param user The widget's argument. */
typedef void tb_canvas_widget_draw(tb_widget widget, tb_gui_paint *paint, void *user);

/** @brief A canvas widget: content drawn by the caller, after Tk's `canvas`
 * drawn by hand; its input comes by bindings. It takes the focus on a
 * click. Properties: `width`, `height`, `background`. */
typedef struct tb_canvas_widget_desc
{
    float width;                 /**< Units across it wants. */
    float height;                /**< Units down it wants. */
    bool background;             /**< Its style's box drawn first. */
    tb_canvas_widget_draw *draw; /**< Optional. */
    void *user;                  /**< Given to it. */
} tb_canvas_widget_desc;

/** @brief Make a canvas widget. @param desc Optional. @return None when
 * `parent` is stale. */
TB_API tb_widget tb_canvas_widget(tb_widget parent, const tb_canvas_widget_desc *desc);

/** @brief Draw a canvas widget's content with another callback.
 * @param draw Optional. @return false for a stale handle or another kind. */
TB_API bool tb_canvas_widget_set_draw(tb_widget widget, tb_canvas_widget_draw *draw, void *user);

/** @brief The kinds of trees, notebooks, paned windows, message dialogs,
 * images, and canvases. @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_treeview_class(void);
/** @brief See tb_treeview_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_notebook_class(void);
/** @brief See tb_treeview_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_panedwindow_class(void);
/** @brief See tb_treeview_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_message_box_class(void);
/** @brief See tb_treeview_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_image_widget_class(void);
/** @brief See tb_treeview_class(). @par Threading Thread-safe. */
TB_API const tb_widget_class *tb_canvas_widget_class(void);

/* ---------------------------------------------------------------------------
 * Styles.
 * ------------------------------------------------------------------------- */

/** @brief The type of a style's value. */
typedef enum tb_style_type
{
    TB_STYLE_NONE,   /**< No value: unset. */
    TB_STYLE_COLOR,  /**< A color, RGBA from 0 to 1, straight alpha. */
    TB_STYLE_NUMBER, /**< A number of units, or a count. */
    TB_STYLE_EDGES,  /**< Four numbers of units, one for each side. */
    TB_STYLE_FONT,   /**< A font spec (#tb_font_spec). */
    TB_STYLE_IMAGE   /**< A nine-slice panel (#tb_style_image). */
} tb_style_type;

/** @brief A nine-slice panel drawn as a background, tinted. */
typedef struct tb_style_image
{
    tb_canvas_nine_slice slice; /**< Its image borrowed; NULL for none. */
    tb_vec4 tint;               /**< Multiplies its colors; zero is (1, 1, 1, 1). */
} tb_style_image;

/** @brief A value of a style: one of #tb_style_type. */
typedef struct tb_style_value
{
    tb_style_type type;
    union {
        tb_vec4 color;
        float number;
        tb_gui_edges edges;
        tb_font_spec font;
        tb_style_image image;
    };
} tb_style_value;

/** @brief A value a kind draws by besides the standard ones of #tb_style:
 * a slider's trough, a list's selection. Looked up by its name, so that a
 * game's kinds name their own. */
typedef struct tb_style_element
{
    const char *name;   /**< "selection"; borrowed for the kind's life. */
    tb_style_type type; /**< What it holds; a value of another type is ignored. */
} tb_style_element;

/**
 * @brief What a kind draws, as data (tb_widget_class::style): its style
 * class, the states its look follows, and the values it reads besides the
 * standard ones. The editor and theme authors read it to know what a kind
 * can be given.
 */
struct tb_widget_style_desc
{
    /** Its style class, the last of a widget's chain before ".": "Button".
     * NULL uses the root class "." alone. */
    const char *name;
    /** The #tb_widget_state_flags its look follows. A state it does not
     * draw is left out when its style is resolved, so that, say, a label
     * hovered shares its style with a label at rest. */
    uint32_t states;
    const tb_style_element *elements; /**< The values it reads besides the standard ones. */
    size_t element_count;             /**< How many. */
};

/**
 * @brief A widget's style, resolved for a state: the standard values every
 * kind may draw by, and its kind's elements. Read only; valid until the
 * widget's styles change (a theme changed or set, its style class or
 * overrides changed) or the widget is destroyed, so a kind reads it in the
 * function it is given it in.
 *
 * A value no theme gives is zero: transparent, no width, no padding, and a
 * font spec of zero, which is the GUI's own font.
 */
typedef struct tb_style
{
    tb_vec4 background;   /**< The fill behind it; transparent draws none. */
    tb_style_image image; /**< A nine-slice background, drawn in place of the fill. */
    tb_vec4 border_color; /**< Its outline. */
    float border_width;   /**< Units its outline is wide, inside its edges; 0 for none. */
    float radius;         /**< Of its corners, its fill's and its outline's. */
    tb_gui_edges padding; /**< Between its edges, its border within, and its content. */
    tb_font_spec font;    /**< Its text's font; a weight of 0 is the GUI's font. */
    tb_vec4 text_color;   /**< Its text. */
    tb_vec4 focus_color;  /**< The ring drawn with the focus. */
    float focus_width;    /**< Units the ring is wide; 0 for none. */
    const tb_widget_style_desc *desc; /**< Its kind's, or NULL. */
    /** Its kind's elements, in the order of its desc's; unset ones are of
     * type #TB_STYLE_NONE. */
    const tb_style_value *elements;
    size_t element_count; /**< As its desc's. */
} tb_style;

/** @brief How many state flags a style is chosen by: the six of
 * tb_widget_state(), and #TB_WIDGET_SELECTED. */
#define TB_STYLE_STATE_BITS 7u

/** @brief Make an empty theme. @return Never NULL. @par Threading Thread-safe. */
TB_API tb_theme *tb_theme_create(void);

/**
 * @brief Make the tool theme of a palette: flat fields, buttons, and
 * lists in its colors, sizes, and spacing, which every kind is drawn by
 * unless a theme says otherwise. Of tb_ui_theme_dark(), it is the GUI's
 * look by default; of tb_ui_theme_light(), see tb_theme_create_light().
 * @param palette Required; its fonts are not used: the GUI's are.
 * @return Never NULL. @par Threading Thread-safe.
 */
TB_API tb_theme *tb_theme_create_tool(const tb_ui_theme *palette);

/** @brief Make the light tool theme: tb_theme_create_tool() of
 * tb_ui_theme_light(), its check marks white on the accent.
 * @return Never NULL. @par Threading Thread-safe. */
TB_API tb_theme *tb_theme_create_light(void);

/** @brief How the sample game theme looks. */
typedef enum tb_theme_game_look
{
    /** Panels and buttons of beveled bronze and slate, headings in the
     * heading role's family and text in the body role's. */
    TB_THEME_GAME_FANTASY,
    /** The same, in pixel art drawn by whole pixels, every text in the
     * pixel role's font. */
    TB_THEME_GAME_RETRO
} tb_theme_game_look;

/**
 * @brief Make the sample game theme: nine-slice panels, buttons, and
 * fields of images it draws in code and owns, and fonts of roles (never
 * bold): a menu's look, beside the tool themes' flat one.
 * @param renderer Required; its images are made on it, so the theme is
 * destroyed before it, outside a frame that draws them.
 * @return Never NULL.
 * @par Threading On the thread that renders, outside a frame.
 */
TB_API tb_theme *tb_theme_create_game(tb_renderer *renderer, tb_theme_game_look look);

/** @brief Destroy a theme, and the images it made. NULL is ignored. No GUI
 * uses it any longer. @par Threading Externally synchronized; a theme with
 * images, on the thread that renders. */
TB_API void tb_theme_destroy(tb_theme *theme);

/**
 * @brief Set a value of a style class for a set of states, replacing the
 * one set for the same states. GUIs that use the theme see the change at
 * their next update.
 * @param style_class Required; copied: "Button", "Primary.Button", ".".
 * @param name Required; copied: a standard value's name ("background",
 * "border_color", "border_width", "radius", "padding", "font",
 * "text_color", "focus_color", "focus_width", "background_image") or an
 * element's.
 * @param states #tb_widget_state_flags; 0 for the widget at rest.
 * @param value Required; copied. A standard value's name takes only its
 * type.
 * @return false, changing nothing, for a standard name given another
 * type, a value of #TB_STYLE_NONE, or states of no such flags.
 * @par Threading Externally synchronized with the GUIs that use it.
 */
TB_API bool tb_theme_set(tb_theme *theme, const char *style_class, const char *name,
                         uint32_t states, const tb_style_value *value);

/** @brief Set a color: tb_theme_set() of a #TB_STYLE_COLOR. */
TB_API bool tb_theme_set_color(tb_theme *theme, const char *style_class, const char *name,
                               uint32_t states, tb_vec4 color);

/** @brief Set a number: tb_theme_set() of a #TB_STYLE_NUMBER. */
TB_API bool tb_theme_set_number(tb_theme *theme, const char *style_class, const char *name,
                                uint32_t states, float number);

/** @brief Set edges: tb_theme_set() of #TB_STYLE_EDGES. */
TB_API bool tb_theme_set_edges(tb_theme *theme, const char *style_class, const char *name,
                               uint32_t states, tb_gui_edges edges);

/** @brief Set a font: tb_theme_set() of a #TB_STYLE_FONT. A spec of a
 * weight and no size is the role's size (tb_font_default_role()). */
TB_API bool tb_theme_set_font(tb_theme *theme, const char *style_class, const char *name,
                              uint32_t states, tb_font_spec font);

/** @brief Set a nine-slice image: tb_theme_set() of a #TB_STYLE_IMAGE.
 * @param image Required; its image borrowed until the theme is destroyed
 * or the value replaced. */
TB_API bool tb_theme_set_image(tb_theme *theme, const char *style_class, const char *name,
                               uint32_t states, const tb_style_image *image);

/**
 * @brief The value a style class holds for exactly a set of states, its
 * fallbacks aside.
 * @param[out] value Required; #TB_STYLE_NONE when there is none.
 * @return Whether there is one.
 * @par Threading Externally synchronized with its writers.
 */
TB_API bool tb_theme_get(const tb_theme *theme, const char *style_class, const char *name,
                         uint32_t states, tb_style_value *value);

/** @brief Remove every class and value; its images stay. */
TB_API void tb_theme_clear(tb_theme *theme);

/**
 * @brief Name an image by a path, for theme files, which name images by
 * their VFS paths ("/Game/UI/button.png"); a path named again is replaced.
 * @param path Required; copied.
 * @param image Required; borrowed until the theme is destroyed.
 */
TB_API void tb_theme_add_image(tb_theme *theme, const char *path, const tb_image *image);

/** @brief The image named by a path. @return NULL when none is. */
TB_API const tb_image *tb_theme_image(const tb_theme *theme, const char *path);

/** @brief A count that grows each time a theme changes: GUIs compare it
 * to see that their styles must be resolved again. */
TB_API uint64_t tb_theme_generation(const tb_theme *theme);

/** @brief Draw a GUI's widgets by a theme, before its default, the tool
 * theme of its palette (tb_gui_set_theme()).
 * @param theme Optional; borrowed until another is set or the GUI is
 * destroyed; NULL for the default alone. */
TB_API void tb_gui_set_style_theme(tb_gui *gui, tb_theme *theme);

/** @brief The theme set with tb_gui_set_style_theme(), or NULL. */
TB_API tb_theme *tb_gui_style_theme(const tb_gui *gui);

/** @brief The GUI's default theme: the tool theme of its palette, made
 * again when the palette is set. @return Borrowed until then. */
TB_API const tb_theme *tb_gui_default_style_theme(const tb_gui *gui);

/** @brief Draw a widget and the widgets in it by a theme, before their
 * ancestors' and the GUI's, as Godot's Control.theme.
 * @param theme Optional; borrowed while the widget is alive or until
 * another is set; NULL for its ancestors'.
 * @return false for a stale handle. */
TB_API bool tb_widget_set_style_theme(tb_widget widget, tb_theme *theme);

/** @brief The theme set on a widget itself, or NULL. */
TB_API tb_theme *tb_widget_style_theme(tb_widget widget);

/**
 * @brief Override one value of a widget's style, before every theme's, as
 * Godot's theme overrides: its text's color, a button's padding.
 * @param name Required; copied; as tb_theme_set().
 * @param states As tb_theme_set().
 * @param value Optional: NULL removes the override.
 * @return false for a stale handle, or a value tb_theme_set() refuses.
 */
TB_API bool tb_widget_override_style(tb_widget widget, const char *name, uint32_t states,
                                     const tb_style_value *value);

/** @brief Remove every override of a widget's style. */
TB_API void tb_widget_clear_style_overrides(tb_widget widget);

/**
 * @brief A widget's style in the state it is in (tb_widget_state()),
 * resolved once and kept until its styles change.
 * @return Valid as #tb_style says; NULL for a stale handle.
 */
TB_API const tb_style *tb_style_of(tb_widget widget);

/**
 * @brief A widget's style in a state given: a row's of a list, chosen
 * (#TB_WIDGET_SELECTED) and hovered, in place of the list's own.
 * @param states #tb_widget_state_flags; those its kind does not draw are
 * left out.
 * @return As tb_style_of().
 */
TB_API const tb_style *tb_style_in(tb_widget widget, uint32_t states);

/** @brief An element of a style by its index in its kind's desc, if it is
 * set and of a type: a color, a number, edges, a font, or an image.
 * Zero, or NULL for an image, when it is not. */
TB_API tb_vec4 tb_style_color(const tb_style *style, size_t element);
/** @brief See tb_style_color(). */
TB_API float tb_style_number(const tb_style *style, size_t element);
/** @brief See tb_style_color(). */
TB_API tb_gui_edges tb_style_edges(const tb_style *style, size_t element);
/** @brief See tb_style_color(). */
TB_API tb_font_spec tb_style_font(const tb_style *style, size_t element);
/** @brief See tb_style_color(). */
TB_API const tb_style_image *tb_style_image_of(const tb_style *style, size_t element);

/** @brief An element of a style by its name. @param name Required.
 * @return NULL when its kind declares none of that name. */
TB_API const tb_style_value *tb_style_find(const tb_style *style, const char *name);

/**
 * @brief Draw a style's box over a rectangle: its background (its image,
 * or else its fill), its border, and its focus ring, each where the style
 * has one; what most kinds draw first.
 * @param style Required.
 */
TB_API void tb_gui_paint_box(tb_gui_paint *paint, tb_canvas_rect rect, const tb_style *style);

/** @brief Draw a nine-slice panel over a rectangle, cut at the GUI's
 * scale. @param slice Required. */
TB_API void tb_gui_paint_nine_slice(tb_gui_paint *paint, const tb_canvas_nine_slice *slice,
                                    tb_canvas_rect rect, tb_vec4 tint);

/** @brief The size of a text block in a widget's style: a font spec of
 * weight 0 is its style's font, and a size of 0 its style's size. */
TB_API tb_vec2 tb_widget_measure_text(tb_widget widget, const tb_gui_text_block *block);

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
 * Read-only properties are not written. The variables named are declared
 * in the file, with their values or settings. Saving a file that was
 * loaded gives the same text when the file was written so.
 *
 * Nothing is dropped silently. A widget that holds what a file cannot
 * hold yet (tb_widget_class::unsaved: menu items, tree items, tabs, panes,
 * an image, a draw callback), a property of a type with no form in a file,
 * a command set in code with no registered name, and a variable no name
 * finds, each fail the save, and the error says which and where.
 *
 * @param sink Required open stream that can write; borrowed. On failure it
 * holds part of a file, or a file with `null` where the failure was.
 * @param[out] error Optional: where and why it failed; its path is that of
 * the value in the file written (`$.root.children[1]`).
 * @return OK; INVALID for a stale handle, text that is not UTF-8, widgets
 * nested too deep, or what a file cannot hold; OUTPUT_FAILED when the
 * stream refused.
 */
TB_API tb_gui_file_result tb_gui_save(tb_widget widget, tb_stream *sink, tb_gui_file_error *error);

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

/* ---------------------------------------------------------------------------
 * Theme files.
 * ------------------------------------------------------------------------- */

/** @brief The format a theme file names: `"format": "badger-theme"`. */
#define TB_THEME_FILE_FORMAT "badger-theme"

/** @brief The version of theme files this engine reads, and the newest. */
#define TB_THEME_FILE_FORMAT_VERSION 1

/**
 * @brief Load a theme file's text into a theme, in place of its classes
 * and values; all or nothing: on any error the theme is as it was. The
 * text is strict JSON: a format, a version, an optional name, and styles,
 * each class an object of values by "name" or "name:state+state". See
 * docs/gui.md for the format.
 * @param text Required: `length` bytes of UTF-8 JSON.
 * @param name Optional: the file's name, for the error's message.
 * @param[out] error Optional: where and why it failed, as a layout file's
 * errors say: a JSON path such as `$.styles.Button.background:hovered`,
 * a line and a column, and a reason.
 * @return OK; MALFORMED for text that is not JSON; INVALID for JSON that is
 * not a theme, of another format or a newer version, or naming an image no
 * theme has (tb_theme_add_image()).
 * @par Threading Externally synchronized with the GUIs that use it.
 */
TB_API tb_gui_file_result tb_theme_load(tb_theme *theme, const char *text, size_t length,
                                        const char *name, tb_gui_file_error *error);

/** @brief tb_theme_load() of a file read through a VFS.
 * @param vfs Required. @param path Required. @param[out] error Optional.
 * @return As tb_theme_load(), or NOT_FOUND or INPUT_FAILED. */
TB_API tb_gui_file_result tb_theme_load_file(tb_theme *theme, struct tb_vfs *vfs, const char *path,
                                             tb_gui_file_error *error);

/** @brief Called when a watched theme file was loaded again, or failed to
 * be. @param error NULL when it loaded; otherwise borrowed for the call. */
typedef void tb_theme_reload_callback(tb_theme *theme, const tb_gui_file_error *error, void *user);

/** @brief How a theme file is watched. Zero is every default. */
typedef struct tb_theme_watch_options
{
    double interval;                    /**< Seconds between looks; 0 is half a second. */
    tb_theme_reload_callback *callback; /**< Optional. */
    void *user;                         /**< Given to it. */
} tb_theme_watch_options;

/**
 * @brief Load a theme's file again each time it changes, for development
 * builds, with the layout files watched (tb_gui_watch()): each
 * tb_gui_update() looks at the file once its interval has passed, through
 * the VFS, and loads it when its bytes changed. A file that fails leaves
 * the theme as it was, and says why through the callback. A shipped game,
 * whose files are in packages, need not watch them.
 * @param theme Required; watched until tb_gui_unwatch_theme() or the GUI
 * is destroyed, so it outlives either.
 * @param vfs Required; borrowed as long.
 * @param path Required; copied.
 * @param options Optional.
 * @return false when the theme is watched already.
 */
TB_API bool tb_gui_watch_theme(tb_gui *gui, tb_theme *theme, struct tb_vfs *vfs, const char *path,
                               const tb_theme_watch_options *options);

/** @brief Stop watching a theme's file. @return false when it was not watched. */
TB_API bool tb_gui_unwatch_theme(tb_gui *gui, tb_theme *theme);

#endif
