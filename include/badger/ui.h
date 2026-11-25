/** @file ui.h
 * @brief An immediate-mode GUI: windows, layout, and widgets, drawn through
 * the canvas, as Dear ImGui and microui are, in the engine's C.
 *
 * @code
 * // once
 * tb_ui_fonts fonts = {0};
 * tb_ui_fonts_open(catalog, NULL, &fonts);
 * tb_ui_theme theme = tb_ui_theme_dark();
 * tb_ui_theme_set_fonts(&theme, &fonts);
 * tb_ui *const ui = tb_ui_create(&(tb_ui_options){.theme = &theme});
 * tb_ui_input input = {0};
 *
 * // for each window event
 * tb_ui_input_add_event(&input, &event);
 *
 * // each frame
 * input.scale = tb_canvas_view_logical(renderer).scale;
 * input.delta_seconds = frame_seconds;
 * tb_ui_begin_frame(ui, &input);
 * if (tb_ui_begin_window(ui, "Settings", (tb_canvas_rect){20, 20, 320, 400}, 0))
 * {
 *     tb_ui_checkbox(ui, "Vertical sync", &vsync);
 *     tb_ui_slider_float(ui, "Field of view", &fov, 60.0f, 120.0f, "%.0f");
 *     if (tb_ui_button(ui, "Apply"))
 *     {
 *         apply();
 *     }
 *     tb_ui_end_window(ui);
 * }
 * tb_ui_end_frame(ui);
 * tb_ui_input_next_frame(&input);
 * tb_ui_draw(ui, renderer); // between tb_renderer_begin_frame() and tb_renderer_end_frame()
 * @endcode
 *
 * **Immediate mode.** The game describes its interface every frame, by
 * calling a function for each widget, and a widget's function returns what
 * the player did to it: a button returns true in the frame it was clicked;
 * a checkbox, slider, or text field changes the caller's value in place and
 * returns true in the frame it changed. The values are the caller's; the
 * interface keeps nothing of them between frames.
 *
 * **Frames.** tb_ui_begin_frame() takes the frame's input; the widgets come
 * between it and tb_ui_end_frame(), and the frame's drawing is then a list
 * of commands (tb_ui_commands()), which tb_ui_draw() hands to the canvas of
 * a renderer. The interface itself never touches a renderer or a window, so
 * it runs, and is tested, from input made by hand.
 *
 * **Units.** The interface is laid out in units, and drawn at
 * `tb_ui_input::scale` pixels a unit: the display's scale, from
 * tb_canvas_view_logical(), so it keeps its size on every display; or more,
 * to zoom it. Its input is in pixels of the frame, as window events give
 * them. Edges are drawn on whole pixels, and text is measured and drawn at
 * the display's own pixels, so it stays crisp at any scale. Setting the
 * scale every frame, from tb_canvas_view_logical(), follows a window to a
 * display of another scale in the frame after it moves.
 *
 * **Fonts.** A theme has a font for text, one for titles and headings, and
 * one for monospaced text, all at the theme's size. tb_ui_fonts_open()
 * opens them from a catalog's roles: UI, heading, and monospace, none of
 * them bold. The interface draws in bold only when its caller gives it a
 * bold font, such as one of a spec made with tb_font_spec_bold().
 *
 * **Identifiers.** Each widget is known from frame to frame by an id: a
 * hash of its label and of the ids pushed with tb_ui_push_id() around it.
 * Two widgets of one label in one window have the same id, and act as one;
 * push an id around each (a loop's index, say) to tell them apart. A label
 * may end in `##` and text that is hashed but not shown: "Delete##3" shows
 * "Delete", and "##volume" shows nothing. The frame's statistics count ids
 * used twice (#tb_ui_stats).
 *
 * **The mouse.** The widget under the mouse in the window in front is hot;
 * the one a button went down on is active until the button comes up, and
 * holds the mouse while it is dragged, even outside its window. A click
 * goes to a widget when the button goes down and comes up on it.
 *
 * **The keyboard.** Clicking a widget focuses it. Tab moves the focus to
 * the next widget of its window, Shift+Tab to the one before, and Escape
 * takes it away. A focused text field takes typed text and editing keys; a
 * widget focused with Tab takes keys too: Space or Enter presses a button,
 * toggles a checkbox, or opens a combo, and Left and Right change a slider.
 *
 * **Sharing input with a game.** tb_ui_wants_mouse() and
 * tb_ui_wants_keyboard() say, after each frame, whether the interface takes
 * the mouse and the keyboard: while the mouse is over one of its windows or
 * dragging a widget, and while a text field or Tab has the focus. A game
 * leaves the mouse and keys to the interface while it wants them.
 *
 * **Windows** have a title bar that moves them and collapses them on a
 * click of its arrow, a corner and edges that resize them, and a scroll bar
 * when their contents are taller than they are. Clicking a window brings it
 * in front. The windows are drawn from canvas layer `tb_ui_options::layer`
 * up, one layer each in their order, and the open combo's list and the
 * tooltip above them all.
 *
 * **State** that lasts from frame to frame (where each window is, its
 * scroll, whether a header is open, the caret of the text field being
 * edited) is kept in the context by id, and forgotten when its widget has
 * not been seen for `tb_ui_options::forget_frames` frames.
 *
 * **Not done yet.** The system's clipboard: Ctrl+C, Ctrl+X, and Ctrl+V copy
 * and paste within the interface only, through tb_ui_clipboard() and
 * tb_ui_set_clipboard(), which a game may connect to the system's. Typing a
 * value into a slider with Ctrl+click; multi-line text fields; horizontal
 * scrolling; disabled widgets; input methods for composed scripts; docking.
 *
 * @par Threading
 * Externally synchronized: a context, and the input given to it, are used
 * by one thread at a time; tb_ui_draw() on the thread that renders. Contexts
 * are independent of one another. Functions marked thread-safe may be
 * called from any thread.
 */
#ifndef TB_UI_H
#define TB_UI_H

#include "badger/api.h"
#include "badger/canvas.h"
#include "badger/font.h"
#include "badger/font_catalog.h"
#include "badger/math.h"
#include "badger/renderer.h"
#include "badger/text.h"
#include "badger/window.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief An interface: its state from frame to frame, and the frame being
 * built. Made by tb_ui_create(); owned by the caller; destroyed by
 * tb_ui_destroy(). */
typedef struct tb_ui tb_ui;

/** @brief A widget's id: never 0, which is no widget. */
typedef uint32_t tb_ui_id;

/* ---------------------------------------------------------------------------
 * Themes.
 * ------------------------------------------------------------------------- */

/** @brief The fonts of a theme. */
typedef enum tb_ui_font
{
    TB_UI_FONT_REGULAR, /**< Labels, values, and text fields: the catalog's UI role. */
    TB_UI_FONT_HEADING, /**< Titles, headers, and headings: its heading role. */
    TB_UI_FONT_MONO,    /**< Text the caller asks for in columns: its monospace role. */
    TB_UI_FONT_COUNT    /**< How many there are; not a font. */
} tb_ui_font;

/** @brief The colors of a theme. */
typedef enum tb_ui_color
{
    TB_UI_COLOR_TEXT,                /**< Text. */
    TB_UI_COLOR_TEXT_DIM,            /**< Quieter text: labels' hints, a title behind. */
    TB_UI_COLOR_WINDOW,              /**< A window's background. */
    TB_UI_COLOR_BORDER,              /**< Windows' and fields' edges. */
    TB_UI_COLOR_TITLE,               /**< A title bar of a window behind another. */
    TB_UI_COLOR_TITLE_ACTIVE,        /**< The title bar of the window in front. */
    TB_UI_COLOR_FRAME,               /**< Fields: text fields, sliders, checkboxes. */
    TB_UI_COLOR_FRAME_HOT,           /**< A field under the mouse. */
    TB_UI_COLOR_FRAME_ACTIVE,        /**< A field being dragged or edited. */
    TB_UI_COLOR_BUTTON,              /**< Buttons and combos. */
    TB_UI_COLOR_BUTTON_HOT,          /**< A button under the mouse. */
    TB_UI_COLOR_BUTTON_ACTIVE,       /**< A button held down. */
    TB_UI_COLOR_HEADER,              /**< Collapsing headers and tables' headers. */
    TB_UI_COLOR_HEADER_HOT,          /**< A header or a list's item under the mouse. */
    TB_UI_COLOR_ACCENT,              /**< Checks, a slider's fill, progress, focus. */
    TB_UI_COLOR_ACCENT_HOT,          /**< The accent under the mouse. */
    TB_UI_COLOR_SELECTION,           /**< Selected text, and a list's chosen item. */
    TB_UI_COLOR_SCROLLBAR,           /**< A scroll bar's track. */
    TB_UI_COLOR_SCROLLBAR_GRAB,      /**< A scroll bar's grab. */
    TB_UI_COLOR_SCROLLBAR_GRAB_HOT,  /**< The grab under the mouse, or dragged. */
    TB_UI_COLOR_SEPARATOR,           /**< Separators, and lines between a table's columns. */
    TB_UI_COLOR_POPUP,               /**< A combo's list, and tooltips. */
    TB_UI_COLOR_TABLE_ROW_ALTERNATE, /**< Every other row of a table, over the window. */
    TB_UI_COLOR_SHADOW,              /**< The shadow under windows and lists. */
    TB_UI_COLOR_COUNT                /**< How many there are; not a color. */
} tb_ui_color;

/** @brief How an interface looks: colors, fonts, sizes, and spacing, in
 * units. Start from tb_ui_theme_dark() and change what differs. */
typedef struct tb_ui_theme
{
    tb_vec4 colors[TB_UI_COLOR_COUNT]; /**< RGBA, straight alpha, from 0 to 1. */
    /** The fonts, borrowed: open until the context is destroyed or given
     * another theme. The regular font is required to begin a frame; a
     * heading or mono font left NULL is the regular. */
    const tb_font *fonts[TB_UI_FONT_COUNT];
    float font_size;        /**< Every font's size, in units to the em. */
    tb_vec2 window_padding; /**< Between a window's edges and its contents. */
    tb_vec2 frame_padding;  /**< Between a field's or button's edges and its text. */
    tb_vec2 item_spacing;   /**< Between widgets: across, and down. */
    float indent;           /**< How far a tree's children are indented. */
    float window_rounding;  /**< The radius of windows' corners. */
    float frame_rounding;   /**< The radius of fields' and buttons' corners. */
    float border;           /**< The width of edges. */
    float scrollbar_width;  /**< The width of a scroll bar. */
    /** The share of a widget's width its label takes, on its left, for
     * widgets with a label beside a field: sliders, combos, text fields. */
    float label_fraction;
    float tooltip_delay; /**< Seconds the mouse rests on a widget before its tooltip shows. */
} tb_ui_theme;

/**
 * @brief The default theme: dark grays with a blue accent, after Unreal's
 * editor, text at the UI role's default size (tb_font_default_role()), and
 * no fonts (tb_ui_theme_set_fonts()).
 * @par Threading Thread-safe.
 */
TB_API tb_ui_theme tb_ui_theme_dark(void);

/**
 * @brief A light theme: light grays with a blue accent, the dark theme's
 * sizes and spacing, and no fonts.
 * @par Threading Thread-safe.
 */
TB_API tb_ui_theme tb_ui_theme_light(void);

/** @brief Fonts opened for a theme, which the caller closes. */
typedef struct tb_ui_fonts
{
    tb_font *fonts[TB_UI_FONT_COUNT]; /**< Each NULL when the catalog has none for it. */
    /** The size of the regular font's spec, which tb_ui_theme_set_fonts()
     * gives the theme; 0 keeps the theme's. */
    float size;
} tb_ui_fonts;

/**
 * @brief Open fonts of a catalog for a theme.
 *
 * Without specs, the fonts are the catalog's UI, heading, and monospace
 * roles, as the catalog names them, none of them bold; the size is the UI
 * role's. A caller that wants bold titles asks for them:
 *
 * @code
 * const tb_font_spec heading = tb_font_spec_of_role(catalog, TB_FONT_ROLE_HEADING);
 * const tb_font_spec specs[TB_UI_FONT_COUNT] = {
 *     [TB_UI_FONT_REGULAR] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_UI),
 *     [TB_UI_FONT_HEADING] = tb_font_spec_bold(heading),
 *     [TB_UI_FONT_MONO] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_MONOSPACE)};
 * tb_ui_fonts_open(catalog, specs, &fonts);
 * @endcode
 *
 * @param catalog Required; its fonts' bytes outlive the fonts.
 * @param specs Optional: #TB_UI_FONT_COUNT specs, in the order of
 * #tb_ui_font, of which the theme takes the regular font's size; NULL for
 * the roles above.
 * @param[out] fonts Required; each font opened, or NULL, and the size.
 * @return Whether the regular font was found and opened: false in a build
 * of the engine that carries no fonts, whose interface needs a font of the
 * game's own.
 * @par Threading Thread-safe with other readers of the catalog.
 */
TB_API bool tb_ui_fonts_open(const tb_font_catalog *catalog, const tb_font_spec *specs,
                             tb_ui_fonts *fonts);

/** @brief Close fonts that tb_ui_fonts_open() opened, and set them to NULL.
 * @param fonts Optional; NULL does nothing.
 * @par Threading Externally synchronized: nothing uses the fonts. */
TB_API void tb_ui_fonts_close(tb_ui_fonts *fonts);

/** @brief Give a theme the fonts opened, and their size when they have one.
 * @param theme Required. @param fonts Required; borrowed by the theme.
 * @par Threading Thread-safe. */
TB_API void tb_ui_theme_set_fonts(tb_ui_theme *theme, const tb_ui_fonts *fonts);

/* ---------------------------------------------------------------------------
 * Input.
 * ------------------------------------------------------------------------- */

/** @brief Keys a frame's input holds at most; more are dropped. */
#define TB_UI_MOST_KEYS 32
/** @brief Bytes of text a frame's input holds at most; more is dropped. */
#define TB_UI_TEXT_CAPACITY 128

/** @brief A key that went down, or repeated, in a frame. */
typedef struct tb_ui_key_press
{
    tb_key key;         /**< Which. */
    uint32_t modifiers; /**< The #tb_modifier bits held. */
    /** Bytes of the frame's text typed before it: the order of keys and
     * text, so that typing and Backspace in one frame edit in order. */
    size_t text_offset;
} tb_ui_key_press;

/**
 * @brief What the player did in a frame: built from window events by
 * tb_ui_input_add_event(), or by hand, by a test or a replay.
 * Zero-initialize, then set the scale and size.
 */
typedef struct tb_ui_input
{
    float scale;          /**< Pixels a unit; 0 is 1. */
    tb_vec2 size;         /**< The frame's size, in pixels: windows are kept on it. */
    double delta_seconds; /**< Seconds since the last frame: tooltips and the caret. */
    tb_vec2 mouse;        /**< Where the mouse is, in pixels of the frame. */
    bool mouse_outside;   /**< The mouse is not over the frame: nothing is under it. */
    uint32_t buttons;     /**< Bits `1u << button` of the #tb_mouse_button held. */
    uint32_t pressed;     /**< Bits of the buttons that went down in the frame. */
    uint32_t released;    /**< Bits of the buttons that came up in the frame. */
    /** The wheel's turn in the frame, in notches: positive `y` scrolls up. */
    tb_vec2 wheel;
    uint32_t modifiers;                    /**< The #tb_modifier bits held. */
    tb_ui_key_press keys[TB_UI_MOST_KEYS]; /**< Keys that went down, in order. */
    size_t key_count;                      /**< How many. */
    char text[TB_UI_TEXT_CAPACITY];        /**< Text typed, UTF-8, not terminated. */
    size_t text_length;                    /**< Bytes of it. */
} tb_ui_input;

/**
 * @brief Add a window event to a frame's input: the mouse's position,
 * buttons, and wheel, keys going down, typed text, and the mouse leaving.
 * Other events are ignored.
 * @param input Required. @param event Required.
 * @par Threading Externally synchronized.
 */
TB_API void tb_ui_input_add_event(tb_ui_input *input, const tb_window_event *event);

/**
 * @brief Begin the next frame's input: forget what happened in the last
 * (presses, releases, the wheel, keys, and text), and keep what is held
 * (the mouse's place and buttons, the modifiers, the scale and size).
 * @param input Required.
 * @par Threading Externally synchronized.
 */
TB_API void tb_ui_input_next_frame(tb_ui_input *input);

/* ---------------------------------------------------------------------------
 * Contexts and frames.
 * ------------------------------------------------------------------------- */

/** @brief The canvas layer of the window at the back, unless chosen. */
#define TB_UI_DEFAULT_LAYER 100
/** @brief Frames a widget's state is kept unseen, unless chosen. */
#define TB_UI_DEFAULT_FORGET_FRAMES 600

/** @brief How an interface is made. Zero is every default. */
typedef struct tb_ui_options
{
    /** The theme, copied; NULL is tb_ui_theme_dark(), whose fonts are then
     * given by tb_ui_set_theme() before the first frame. */
    const tb_ui_theme *theme;
    int32_t layer; /**< The canvas layer of the back window; 0 is #TB_UI_DEFAULT_LAYER. */
    /** Frames a widget's state is kept after it was last seen; 0 is
     * #TB_UI_DEFAULT_FORGET_FRAMES. */
    uint32_t forget_frames;
} tb_ui_options;

/**
 * @brief Make an interface.
 * @param options Optional; NULL is every default.
 * @return The interface; never NULL. Running out of memory ends the process.
 */
TB_API tb_ui *tb_ui_create(const tb_ui_options *options);

/** @brief Destroy an interface. NULL is ignored. */
TB_API void tb_ui_destroy(tb_ui *ui);

/** @brief Use another theme, from the next frame.
 * @param theme Required; copied, its fonts borrowed. */
TB_API void tb_ui_set_theme(tb_ui *ui, const tb_ui_theme *theme);

/** @brief The theme in use. @return Borrowed until the theme is set again. */
TB_API const tb_ui_theme *tb_ui_get_theme(const tb_ui *ui);

/**
 * @brief Begin a frame: take its input, and forget the last frame's commands.
 * @param input Required; copied. Its scale more than 0, or 0 for 1.
 * @par Errors A theme without a regular font is a programming error.
 */
TB_API void tb_ui_begin_frame(tb_ui *ui, const tb_ui_input *input);

/**
 * @brief End a frame: move the focus on Tab, close a list clicked away
 * from, work out what the interface wants of the input, and forget state
 * not seen for long.
 *
 * Each window begun is ended first.
 */
TB_API void tb_ui_end_frame(tb_ui *ui);

/** @brief What a command draws. */
typedef enum tb_ui_command_type
{
    TB_UI_COMMAND_RECT,    /**< A filled rectangle, rounded by `radius`. */
    TB_UI_COMMAND_OUTLINE, /**< A rectangle's outline, `thickness` wide, rounded by `radius`. */
    TB_UI_COMMAND_CIRCLE,  /**< A filled circle in `rect`, of `radius`. */
    TB_UI_COMMAND_LINE,    /**< A line from `from` to `to`, `thickness` wide. */
    TB_UI_COMMAND_TEXT,    /**< Text, its box's top left at `rect`'s. */
    TB_UI_COMMAND_IMAGE /**< An image, or its `source`, stretched over `rect`, tinted by `color`. */
} tb_ui_command_type;

/** @brief Something to draw, in units, as tb_ui_draw() draws it. */
typedef struct tb_ui_command
{
    tb_ui_command_type type;
    int32_t layer;       /**< The canvas layer. */
    tb_canvas_rect clip; /**< What of it may be seen. */
    /** The rectangle; for a circle, the square around it; for text, its
     * box's top left and its measured size. */
    tb_canvas_rect rect;
    float radius;        /**< Of corners, or of a circle. */
    float thickness;     /**< Of an outline or a line. */
    tb_vec2 from;        /**< A line's start. */
    tb_vec2 to;          /**< A line's end. */
    tb_vec4 color;       /**< Straight alpha. */
    const tb_font *font; /**< Text's font. */
    float font_size;     /**< Text's size, in units to the em. */
    float wrap_width;    /**< The width text wraps at; 0 for none. */
    /** How text's lines sit across its box: the wrap width when there is
     * one, or else the widest line. Zero is left. */
    tb_font_align align;
    /** Text's bytes: the interface's, valid until the next frame begins. */
    tb_text text;
    tb_ui_id id; /**< The widget, or window, that drew it; 0 for none. */
    /** An image's image: borrowed, alive until the frame that draws it ends. */
    const tb_image *image;
    /** An image's texels drawn; a zero size is the whole image. A
     * nine-slice panel is an image command for each of its slices. */
    tb_rhi_rect source;
} tb_ui_command;

/**
 * @brief The commands of the frame that ended, in the order they were
 * made: layer by layer, the canvas draws them in that order.
 * @param[out] count Required; how many.
 * @return The commands, valid until the next frame begins.
 */
TB_API const tb_ui_command *tb_ui_commands(const tb_ui *ui, size_t *count);

/**
 * @brief Draw the frame that ended on a renderer's canvas.
 *
 * Called between tb_renderer_begin_frame() and tb_renderer_end_frame(). It
 * leaves the canvas's view and layer as its last command set them.
 *
 * @param renderer Required.
 * @par Threading Stage-owned: on the thread that renders.
 */
TB_API void tb_ui_draw(const tb_ui *ui, tb_renderer *renderer);

/** @brief Whether the interface takes the mouse: it is over one of its
 * windows or lists, or dragging a widget. As of the frame that ended. */
TB_API bool tb_ui_wants_mouse(const tb_ui *ui);

/** @brief Whether the interface takes the keyboard: a text field has the
 * focus, or Tab gave a widget the focus. As of the frame that ended. */
TB_API bool tb_ui_wants_keyboard(const tb_ui *ui);

/** @brief Whether a text field is being edited, which takes typed text. As
 * of the frame that ended. */
TB_API bool tb_ui_wants_text(const tb_ui *ui);

/** @brief The cursor the mouse should show, for tb_window_set_cursor(): an
 * I-beam over text fields, arrows over edges that resize. */
TB_API tb_cursor tb_ui_cursor(const tb_ui *ui);

/** @brief How the frame that ended went: for tests, and statistics. */
typedef struct tb_ui_stats
{
    size_t commands;         /**< Commands made. */
    size_t windows;          /**< Windows begun. */
    size_t states;           /**< Widgets' state kept, after forgetting the unseen. */
    size_t id_collisions;    /**< Widgets with an id another widget had in the frame. */
    tb_ui_id hot;            /**< The widget under the mouse, or 0. */
    tb_ui_id active;         /**< The widget held, or 0. */
    tb_ui_id focus;          /**< The widget with the focus, or 0. */
    tb_ui_id hovered_window; /**< The window, or list, under the mouse, or 0. */
} tb_ui_stats;

/** @brief How the frame that ended went. @param[out] stats Required. */
TB_API void tb_ui_stats_of(const tb_ui *ui, tb_ui_stats *stats);

/**
 * @brief The text that Ctrl+C and Ctrl+X last copied in a text field.
 * @return Borrowed until it next changes; empty for none.
 */
TB_API tb_text tb_ui_clipboard(const tb_ui *ui);

/** @brief Set the text that Ctrl+V pastes in a text field: from the system's
 * clipboard, for one. @param text Copied. */
TB_API void tb_ui_set_clipboard(tb_ui *ui, tb_text text);

/* ---------------------------------------------------------------------------
 * Identifiers.
 * ------------------------------------------------------------------------- */

/**
 * @brief The id a widget of a label has here: the label, up to `##` and
 * after it, hashed with the ids pushed.
 * @param label Required.
 */
TB_API tb_ui_id tb_ui_get_id(const tb_ui *ui, const char *label);

/** @brief Push an id made of text, for the widgets that come after, until
 * tb_ui_pop_id(). @param name Required. */
TB_API void tb_ui_push_id(tb_ui *ui, const char *name);

/** @brief Push an id made of a number: a loop's index, say. */
TB_API void tb_ui_push_id_int(tb_ui *ui, int64_t value);

/** @brief Pop the id pushed last. */
TB_API void tb_ui_pop_id(tb_ui *ui);

/* ---------------------------------------------------------------------------
 * Windows and panels.
 * ------------------------------------------------------------------------- */

/** @brief How a window behaves. */
typedef enum tb_ui_window_flags
{
    TB_UI_WINDOW_NO_TITLE = 1u << 0,    /**< No title bar: it neither moves nor collapses. */
    TB_UI_WINDOW_NO_MOVE = 1u << 1,     /**< Its title bar does not move it. */
    TB_UI_WINDOW_NO_RESIZE = 1u << 2,   /**< Its edges do not resize it. */
    TB_UI_WINDOW_NO_COLLAPSE = 1u << 3, /**< Its title bar has no arrow to collapse it. */
    TB_UI_WINDOW_NO_SCROLLBAR = 1u << 4 /**< Contents taller than it are cut off. */
} tb_ui_window_flags;

/**
 * @brief Begin a window, whose widgets come next.
 * @param title Required; its title, and its id, as a label is.
 * @param rect Where it is and how large, the first time it is begun;
 * after, it stays where the player moved it.
 * @param flags #tb_ui_window_flags.
 * @return Whether its contents are seen: false when it is collapsed. Only
 * a window that returned true is ended with tb_ui_end_window().
 */
TB_API bool tb_ui_begin_window(tb_ui *ui, const char *title, tb_canvas_rect rect, uint32_t flags);

/** @brief End the window begun last. */
TB_API void tb_ui_end_window(tb_ui *ui);

/** @brief Where the window being built is, and how large. */
TB_API tb_canvas_rect tb_ui_window_rect(const tb_ui *ui);

/**
 * @brief Begin a panel: a region of the window, `height` units tall and as
 * wide as a widget, with its own border and scrolling.
 * @param name Required; its id, as a label is.
 * @param height Units; 0 is what is left of the window, at least a line.
 */
TB_API void tb_ui_begin_panel(tb_ui *ui, const char *name, float height);

/** @brief End the panel begun last. */
TB_API void tb_ui_end_panel(tb_ui *ui);

/* ---------------------------------------------------------------------------
 * Layout. Widgets go down a window one to a line, as wide as it for fields
 * and as wide as their text for buttons and checkboxes, unless placed.
 * ------------------------------------------------------------------------- */

/** @brief Columns a row has at most. */
#define TB_UI_MOST_COLUMNS 16

/**
 * @brief Lay the widgets that follow in rows of columns, one widget a
 * column, until the next call or the end of the window or panel.
 * @param columns How many, at most #TB_UI_MOST_COLUMNS; 0 goes back to one
 * widget a line.
 * @param widths Each column's width in units; 0 shares what the others
 * leave equally. NULL shares the whole width equally.
 */
TB_API void tb_ui_row(tb_ui *ui, size_t columns, const float *widths);

/** @brief Place the next widget on the line of the last, after it. */
TB_API void tb_ui_same_line(tb_ui *ui);

/** @brief Leave a line's spacing of space. */
TB_API void tb_ui_spacing(tb_ui *ui);

/** @brief A line across the window, between groups of widgets. */
TB_API void tb_ui_separator(tb_ui *ui);

/** @brief Indent the widgets that follow by the theme's indent. */
TB_API void tb_ui_indent(tb_ui *ui);

/** @brief Undo one tb_ui_indent(). */
TB_API void tb_ui_unindent(tb_ui *ui);

/* ---------------------------------------------------------------------------
 * Text.
 * ------------------------------------------------------------------------- */

/** @brief Text on a line of its own height as a field, so that it lines up
 * with fields beside it; `\n` breaks it. @param text Required; UTF-8. */
TB_API void tb_ui_text(tb_ui *ui, const char *text);

/** @brief Text, formatted as tb_fmt_buffer() formats, up to 512 bytes.
 * @param format Required. */
TB_API void tb_ui_textf(tb_ui *ui, const char *format, ...);

/** @brief tb_ui_textf() with its arguments in a `va_list`. */
TB_API void tb_ui_textv(tb_ui *ui, const char *format, va_list arguments);

/** @brief Text in a color of the caller's. @param text Required. */
TB_API void tb_ui_text_colored(tb_ui *ui, tb_vec4 color, const char *text);

/** @brief Text wrapped at the window's width: a paragraph.
 * @param text Required. */
TB_API void tb_ui_text_wrapped(tb_ui *ui, const char *text);

/** @brief Text in the heading font: a heading. @param text Required. */
TB_API void tb_ui_heading(tb_ui *ui, const char *text);

/** @brief Text in the monospace font, a line of its own height: a log's
 * lines, or a table of text in columns. `\n` breaks it.
 * @param text Required; UTF-8. */
TB_API void tb_ui_text_mono(tb_ui *ui, const char *text);

/**
 * @brief A graph of values joined by lines, as wide as the window: frame
 * times, say. The first value is at the left, the last at the right.
 * @param label Optional; drawn in its top left corner. NULL for none.
 * @param values Required when `count` is more than 0.
 * @param count How many; fewer than 2 draws the graph's box alone.
 * @param min The value at the bottom.
 * @param max The value at the top; not more than `min` takes the values'
 * own least and greatest.
 * @param height Units; 0 is three lines of fields.
 */
TB_API void tb_ui_plot(tb_ui *ui, const char *label, const float *values, size_t count, float min,
                       float max, float height);

/** @brief A label and a value beside it, as a field's label and field are
 * placed: a property that is read but not changed. Both required. */
TB_API void tb_ui_value(tb_ui *ui, const char *label, const char *value);

/* ---------------------------------------------------------------------------
 * Widgets. Each returns true in the frame the player changed it, or, for a
 * button, clicked it.
 * ------------------------------------------------------------------------- */

/** @brief A button, as wide as its label. @param label Required. */
TB_API bool tb_ui_button(tb_ui *ui, const char *label);

/** @brief A box that is checked or not, and its label.
 * @param label Required. @param value Required; toggled on a click. */
TB_API bool tb_ui_checkbox(tb_ui *ui, const char *label, bool *value);

/**
 * @brief One of a group of buttons of which one is chosen.
 * @param label Required.
 * @param value Required; the group's choice, set to `option` on a click.
 * @param option The value this button stands for.
 */
TB_API bool tb_ui_radio(tb_ui *ui, const char *label, int32_t *value, int32_t option);

/**
 * @brief A line of a list that is chosen or not: a combo's item, a row of
 * a list. As wide as the window.
 * @param label Required.
 * @param selected Whether it is drawn chosen.
 * @return true in the frame it was clicked.
 */
TB_API bool tb_ui_selectable(tb_ui *ui, const char *label, bool selected);

/**
 * @brief A slider: a field filled to a value between two others, which the
 * mouse sets where it presses and drags it.
 * @param label Required.
 * @param value Required; kept between `min` and `max`.
 * @param min The value at the left. @param max At the right; more than `min`.
 * @param format As tb_fmt_buffer() takes it, of a `double`: "%.2f". NULL is "%.3f".
 */
TB_API bool tb_ui_slider_float(tb_ui *ui, const char *label, float *value, float min, float max,
                               const char *format);

/** @brief A slider of whole numbers. As tb_ui_slider_float(). */
TB_API bool tb_ui_slider_int(tb_ui *ui, const char *label, int32_t *value, int32_t min,
                             int32_t max);

/**
 * @brief A number dragged left and right: `speed` a unit the mouse moves.
 * @param label Required.
 * @param value Required.
 * @param speed The change a unit of the mouse's motion; Shift is ten times as fast.
 * @param min The least it goes to. @param max The most; `min == max` is no limit.
 * @param format As tb_ui_slider_float() takes it.
 */
TB_API bool tb_ui_drag_float(tb_ui *ui, const char *label, float *value, float speed, float min,
                             float max, const char *format);

/** @brief A whole number dragged. As tb_ui_drag_float(). */
TB_API bool tb_ui_drag_int(tb_ui *ui, const char *label, int32_t *value, float speed, int32_t min,
                           int32_t max);

/**
 * @brief A combo: a field that shows one item of a list, and opens the list
 * above everything to choose another.
 * @param label Required.
 * @param selected Required; the index of the item chosen.
 * @param items Required; `count` items.
 * @param count How many.
 */
TB_API bool tb_ui_combo(tb_ui *ui, const char *label, size_t *selected, const char *const *items,
                        size_t count);

/**
 * @brief A field of one line of text the player edits: a caret, a
 * selection with Shift or the mouse, Home and End, Ctrl with the arrows by
 * words, Backspace and Delete, Ctrl+A, and Ctrl+C, X, and V on the
 * interface's clipboard. Enter or Escape ends the editing.
 * @param label Required.
 * @param buffer Required; UTF-8, terminated; edited in place.
 * @param capacity Bytes of `buffer`, its terminator included; more than 0.
 * Typing past it is dropped, whole characters at a time.
 * @return true in the frame its text changed.
 */
TB_API bool tb_ui_text_field(tb_ui *ui, const char *label, char *buffer, size_t capacity);

/**
 * @brief A header that opens and closes the widgets under it, as wide as
 * the window.
 * @param label Required.
 * @param default_open Whether it is open the first time.
 * @return Whether it is open: the widgets under it are then given.
 */
TB_API bool tb_ui_header(tb_ui *ui, const char *label, bool default_open);

/**
 * @brief A node of a tree, which opens and closes its children.
 * @param label Required.
 * @param default_open Whether it is open the first time.
 * @return Whether it is open: its children are then given, indented, and
 * tb_ui_tree_pop() ends them.
 */
TB_API bool tb_ui_tree_push(tb_ui *ui, const char *label, bool default_open);

/** @brief End the children of the node opened last. */
TB_API void tb_ui_tree_pop(tb_ui *ui);

/**
 * @brief A bar filled to a fraction.
 * @param fraction From 0 to 1; kept there.
 * @param overlay Text on it; NULL is the percentage.
 */
TB_API void tb_ui_progress(tb_ui *ui, float fraction, const char *overlay);

/** @brief A swatch of a color, with a checkerboard under what is
 * transparent, and its label. @param label Required.
 * @return true in the frame it was clicked. */
TB_API bool tb_ui_color_swatch(tb_ui *ui, const char *label, tb_vec4 color);

/** @brief A color's swatch and its four channels, each dragged from 0 to 1.
 * @param label Required. @param color Required. */
TB_API bool tb_ui_color_edit(tb_ui *ui, const char *label, tb_vec4 *color);

/* ---------------------------------------------------------------------------
 * Tables.
 * ------------------------------------------------------------------------- */

/**
 * @brief Begin a table: a header row, then a widget a cell, row by row,
 * every other row shaded.
 * @param name Required; its id.
 * @param columns How many, from 1 to #TB_UI_MOST_COLUMNS.
 * @param headers Optional; `columns` headers, or NULL for none.
 * @param widths Optional; as tb_ui_row() takes them.
 */
TB_API void tb_ui_begin_table(tb_ui *ui, const char *name, size_t columns,
                              const char *const *headers, const float *widths);

/** @brief End the table begun last. */
TB_API void tb_ui_end_table(tb_ui *ui);

/* ---------------------------------------------------------------------------
 * The last widget.
 * ------------------------------------------------------------------------- */

/** @brief Whether the mouse is over the last widget, and nothing is held. */
TB_API bool tb_ui_item_hovered(const tb_ui *ui);

/** @brief The last widget's id, or 0 for text, which has none. */
TB_API tb_ui_id tb_ui_last_id(const tb_ui *ui);

/** @brief Give the last widget the focus: a text field to type in at once. */
TB_API void tb_ui_focus_last(tb_ui *ui);

/**
 * @brief Show a tooltip by the mouse when it has rested on the last widget
 * for the theme's delay.
 * @param text Required; wrapped.
 */
TB_API void tb_ui_tooltip(tb_ui *ui, const char *text);

#endif
