/** @file window.h
 * @brief Windows and displays: the window a game draws into, the monitors it
 * may fill, and the events the system sends it.
 *
 * A window system is the connection to the system's windowing: Win32 on
 * Windows, Wayland or X11 on Linux, and Cocoa on macOS. It lists the
 * displays, creates windows, and delivers their events.
 *
 * @code
 * tb_window_system *system = NULL;
 * tb_window *window = NULL;
 * tb_window_error error = TB_WINDOW_ERROR_INIT;
 *
 * if (tb_window_system_open(NULL, &system, &error) != TB_WINDOW_OK ||
 *     tb_window_create(system, &(tb_window_options){.title = "Burrow"}, &window, &error) !=
 *         TB_WINDOW_OK)
 * {
 *     report(error.message);
 * }
 * for (bool running = true; running;)
 * {
 *     tb_window_event event = {0};
 *     while (tb_window_system_poll(system, &event))
 *     {
 *         running = event.type != TB_WINDOW_EVENT_CLOSE_REQUESTED;
 *     }
 *     ...
 * }
 * @endcode
 *
 * **Modes.** A window is windowed, with a frame the player moves and sizes;
 * borderless, without a frame, filling its display; or fullscreen, the
 * display in a resolution of the game's own and the game's alone. Only
 * Windows has fullscreen of that kind: on Wayland, X11 and macOS a window
 * asked for fullscreen is borderless, and tb_window_state_of() says so. A mode,
 * a size, or a display changes at any time with tb_window_configure().
 *
 * **Sizes and scale.** A window's size is given in logical units, which the
 * system scales to pixels by the display's content scale: a 1280 by 720
 * window on a display at 150% is 1920 by 1080 pixels. A renderer sizes its
 * images by the pixels; an interface lays itself out in logical units, so
 * that it reads the same on a laptop, a 4K monitor, and the Steam Deck. The
 * scale changes when a window moves to another display, and an event says
 * so.
 *
 * **Placement.** A new windowed window is centered on its display, except on
 * Wayland, where the compositor places windows and a program cannot. A
 * window given no size is the largest 16:9 size, up to 1280 by 720, that
 * fits in 85% of its display's usable area.
 *
 * **A display that is gone.** A window asked for a display that does not
 * exist, as a settings file may name a monitor since unplugged, opens on the
 * primary display instead.
 *
 * **Until something draws.** A window shows black until a renderer presents
 * into it.
 *
 * **The mouse and text.** A window reports the mouse's buttons, its motion,
 * its wheel, and its entering and leaving, at positions in logical units and
 * in pixels from the client area's top-left corner. A first-person camera
 * turns on relative mouse mode with tb_window_set_relative_mouse(): the
 * cursor is hidden and held in the window, and the mouse's own motion comes
 * as #TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION in place of positions. Typed text
 * comes apart from keys, as UTF-8 that the system has composed, dead keys
 * included, in #TB_WINDOW_EVENT_TEXT_INPUT.
 *
 * @par Threading
 * Stage-owned: every function but tb_window_system_wake() is called on the
 * thread that opened the window system, which on macOS must be the main
 * thread. tb_window_system_wake() is thread-safe.
 */
#ifndef TB_WINDOW_H
#define TB_WINDOW_H

#include "badger/api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A connection to the system's windowing. Opened by
 * tb_window_system_open(); owned by the caller; closed by
 * tb_window_system_close(). */
typedef struct tb_window_system tb_window_system;

/** @brief A window. Created by tb_window_create(); owned by the caller;
 * destroyed by tb_window_destroy(). */
typedef struct tb_window tb_window;

/** @brief The windowing a window system connects to. */
typedef enum tb_window_backend
{
    TB_WINDOW_BACKEND_AUTOMATIC, /**< The platform's own: on Linux, Wayland, else X11. */
    TB_WINDOW_BACKEND_WIN32,     /**< Windows. */
    TB_WINDOW_BACKEND_WAYLAND,   /**< Linux, under a Wayland compositor. */
    TB_WINDOW_BACKEND_X11,       /**< Linux, under an X server or Xwayland. */
    TB_WINDOW_BACKEND_COCOA      /**< macOS. */
} tb_window_backend;

/** @brief What a window system or a window came to. */
typedef enum tb_window_result
{
    TB_WINDOW_OK,          /**< Done. */
    TB_WINDOW_UNAVAILABLE, /**< The windowing is not there: no display server, no library, or
                              a backend of another platform. A capability, not a fault. */
    TB_WINDOW_SYSTEM_ERROR /**< The system refused; the message says how. */
} tb_window_result;

/** @brief Bytes of an error's message, its NUL included. */
#define TB_WINDOW_MESSAGE_CAPACITY 256

/** @brief Why a window system or a window could not be made. */
typedef struct tb_window_error
{
    tb_window_result result; /**< As the function returned. */
    /** What went wrong, in UTF-8: "no Wayland compositor: WAYLAND_DISPLAY is not set". */
    char message[TB_WINDOW_MESSAGE_CAPACITY];
} tb_window_error;

/** @brief An error before a call. */
#define TB_WINDOW_ERROR_INIT {TB_WINDOW_OK, {0}}

/** @brief How a window fills the screen. The values are those of the
 * `display.window_mode` setting's choices. */
typedef enum tb_window_mode
{
    TB_WINDOW_MODE_WINDOWED,   /**< A window with a frame, that the player moves and sizes. */
    TB_WINDOW_MODE_BORDERLESS, /**< A window without a frame that fills its display. */
    TB_WINDOW_MODE_FULLSCREEN  /**< The display in a mode of the game's own, the game's alone. */
} tb_window_mode;

/** @brief Bytes of a display's name, its NUL included. */
#define TB_DISPLAY_NAME_CAPACITY 128

/** @brief A display, as the system describes it. */
typedef struct tb_display
{
    /** A name to show a player, in UTF-8: "DELL U2723QE"; may be empty. */
    char name[TB_DISPLAY_NAME_CAPACITY];
    bool primary; /**< The display that the system puts new windows on. */
    /** Where the display is, in the units the system places windows by:
     * pixels on Windows and X11, logical units on Wayland and macOS. */
    int32_t x;
    int32_t y;              /**< See `x`. */
    uint32_t width;         /**< The display's size, in the same units. */
    uint32_t height;        /**< See `width`. */
    int32_t usable_x;       /**< The part that taskbars, docks and menus leave free. */
    int32_t usable_y;       /**< See `usable_x`. */
    uint32_t usable_width;  /**< See `usable_x`. */
    uint32_t usable_height; /**< See `usable_x`. */
    uint32_t pixel_width;   /**< The current resolution, in pixels. */
    uint32_t pixel_height;  /**< See `pixel_width`. */
    /** How many pixels a logical unit is: 1 at 96 DPI on Windows, 2 on a
     * Retina display. */
    double content_scale;
    /** The refresh rate in thousandths of a hertz, 59940 for 59.94 Hz; 0
     * when unknown. */
    uint32_t refresh_millihertz;
    /** The visible size in millimeters, from the display itself; 0 when
     * unknown, as for a projector. */
    uint32_t physical_width_mm;
    uint32_t physical_height_mm; /**< See `physical_width_mm`. */
} tb_display;

/** @brief How a window system is opened. Zero-initialize it for the defaults. */
typedef struct tb_window_system_options
{
    /** Which windowing to connect to; automatic for the platform's own. */
    tb_window_backend backend;
} tb_window_system_options;

/** @brief How a window looks and fills the screen. Zero-initialize it for a
 * window of the defaults. */
typedef struct tb_window_options
{
    /** UTF-8; NULL for none, or for tb_window_configure() the title the
     * window has. */
    const char *title;
    tb_window_mode mode;
    /** The display: an index of tb_window_system_display(); 0 is the
     * primary. One that does not exist is the primary. */
    size_t display;
    /** The windowed size, in logical units; 0 for the default size. Clamped
     * to the display's usable area. */
    uint32_t width;
    uint32_t height; /**< See `width`. */
    /** The fullscreen resolution, in pixels; 0 for the display's current
     * one. Only Windows uses it. */
    uint32_t fullscreen_width;
    uint32_t fullscreen_height; /**< See `fullscreen_width`. */
    /** The fullscreen refresh rate in hertz; 0 for the display's current. */
    uint32_t refresh_rate;
    bool fixed_size; /**< A windowed window the player cannot resize. */
} tb_window_options;

/** @brief Where a window is and how large. */
typedef struct tb_window_state
{
    tb_window_mode mode;   /**< The mode it is in, which may not be the one asked for. */
    size_t display;        /**< The display it is on, or mostly on. */
    uint32_t width;        /**< Its client area, in logical units. */
    uint32_t height;       /**< See `width`. */
    uint32_t pixel_width;  /**< Its client area, in pixels: the size to render at. */
    uint32_t pixel_height; /**< See `pixel_width`. */
    double content_scale;  /**< Pixels per logical unit, as its display's. */
    bool focused;          /**< It receives the keyboard. */
    bool minimized;        /**< It is minimized, or hidden: nothing of it is seen. */
    /** Its relative mouse mode is on: see tb_window_set_relative_mouse(). */
    bool relative_mouse;
} tb_window_state;

/** @brief Keys, by their place on the keyboard, whatever its layout: the key
 * an American keyboard labels W is #TB_KEY_W on a French one too. */
typedef enum tb_key
{
    TB_KEY_UNKNOWN,
    TB_KEY_A,
    TB_KEY_B,
    TB_KEY_C,
    TB_KEY_D,
    TB_KEY_E,
    TB_KEY_F,
    TB_KEY_G,
    TB_KEY_H,
    TB_KEY_I,
    TB_KEY_J,
    TB_KEY_K,
    TB_KEY_L,
    TB_KEY_M,
    TB_KEY_N,
    TB_KEY_O,
    TB_KEY_P,
    TB_KEY_Q,
    TB_KEY_R,
    TB_KEY_S,
    TB_KEY_T,
    TB_KEY_U,
    TB_KEY_V,
    TB_KEY_W,
    TB_KEY_X,
    TB_KEY_Y,
    TB_KEY_Z,
    TB_KEY_0,
    TB_KEY_1,
    TB_KEY_2,
    TB_KEY_3,
    TB_KEY_4,
    TB_KEY_5,
    TB_KEY_6,
    TB_KEY_7,
    TB_KEY_8,
    TB_KEY_9,
    TB_KEY_F1,
    TB_KEY_F2,
    TB_KEY_F3,
    TB_KEY_F4,
    TB_KEY_F5,
    TB_KEY_F6,
    TB_KEY_F7,
    TB_KEY_F8,
    TB_KEY_F9,
    TB_KEY_F10,
    TB_KEY_F11,
    TB_KEY_F12,
    TB_KEY_ESCAPE,
    TB_KEY_ENTER,
    TB_KEY_SPACE,
    TB_KEY_TAB,
    TB_KEY_BACKSPACE,
    TB_KEY_GRAVE,
    TB_KEY_LEFT,
    TB_KEY_RIGHT,
    TB_KEY_UP,
    TB_KEY_DOWN,
    TB_KEY_LEFT_SHIFT,
    TB_KEY_RIGHT_SHIFT,
    TB_KEY_LEFT_CONTROL,
    TB_KEY_RIGHT_CONTROL,
    TB_KEY_LEFT_ALT,
    TB_KEY_RIGHT_ALT,
    TB_KEY_HOME,
    TB_KEY_END,
    TB_KEY_PAGE_UP,
    TB_KEY_PAGE_DOWN,
    TB_KEY_INSERT,
    TB_KEY_DELETE, /**< Delete, the key that deletes forward; Fn+Delete on a Mac laptop. */
    TB_KEY_COUNT   /**< The number of keys; not a key. */
} tb_key;

/** @brief The modifier keys held during an event: bits of an event's
 * `modifiers`. Either key of a pair sets its bit. */
typedef enum tb_modifier
{
    TB_MODIFIER_SHIFT = 1u << 0,   /**< Shift. */
    TB_MODIFIER_CONTROL = 1u << 1, /**< Control. */
    TB_MODIFIER_ALT = 1u << 2,     /**< Alt; Option on a Mac. */
    TB_MODIFIER_SUPER = 1u << 3    /**< The Windows key; Command on a Mac. */
} tb_modifier;

/** @brief A mouse's buttons. */
typedef enum tb_mouse_button
{
    TB_MOUSE_BUTTON_NONE,   /**< No button: the event is not of a button. */
    TB_MOUSE_BUTTON_LEFT,   /**< The primary button. */
    TB_MOUSE_BUTTON_RIGHT,  /**< The secondary button. */
    TB_MOUSE_BUTTON_MIDDLE, /**< The wheel, pressed. */
    TB_MOUSE_BUTTON_X1,     /**< The first side button, usually "back". */
    TB_MOUSE_BUTTON_X2,     /**< The second side button, usually "forward". */
    TB_MOUSE_BUTTON_COUNT   /**< The number of buttons; not a button. */
} tb_mouse_button;

/** @brief The shapes of the system's cursor that an interface asks for. */
typedef enum tb_cursor
{
    TB_CURSOR_ARROW,                /**< The ordinary pointer. */
    TB_CURSOR_TEXT,                 /**< The I-beam, over text that can be edited. */
    TB_CURSOR_HAND,                 /**< The pointing hand, over a link or a button. */
    TB_CURSOR_CROSSHAIR,            /**< A crosshair, for picking a point. */
    TB_CURSOR_NOT_ALLOWED,          /**< What is under it cannot be done. */
    TB_CURSOR_RESIZE_HORIZONTAL,    /**< Resizing left and right. */
    TB_CURSOR_RESIZE_VERTICAL,      /**< Resizing up and down. */
    TB_CURSOR_RESIZE_DIAGONAL_DOWN, /**< Resizing from top-left to bottom-right. */
    TB_CURSOR_RESIZE_DIAGONAL_UP,   /**< Resizing from bottom-left to top-right. */
    TB_CURSOR_RESIZE_ALL,           /**< Moving, in every direction. */
    TB_CURSOR_COUNT                 /**< The number of shapes; not a shape. */
} tb_cursor;

/** @brief Bytes of an event's text, its NUL included. Text longer than
 * this comes in several events, each of whole characters. */
#define TB_WINDOW_TEXT_CAPACITY 32

/** @brief What happened to a window. */
typedef enum tb_window_event_type
{
    /** The player asked to close it: its close button, Alt+F4, Cmd+Q. The
     * window stays open; the game decides. */
    TB_WINDOW_EVENT_CLOSE_REQUESTED,
    TB_WINDOW_EVENT_RESIZED,          /**< Its size changed; the event has the new one. */
    TB_WINDOW_EVENT_SCALE_CHANGED,    /**< Its content scale changed, and with it the pixels. */
    TB_WINDOW_EVENT_FOCUS_GAINED,     /**< It receives the keyboard. */
    TB_WINDOW_EVENT_FOCUS_LOST,       /**< It no longer does. */
    TB_WINDOW_EVENT_MINIMIZED,        /**< Nothing of it is seen. */
    TB_WINDOW_EVENT_RESTORED,         /**< It is seen again. */
    TB_WINDOW_EVENT_KEY_DOWN,         /**< A key went down, or repeats. */
    TB_WINDOW_EVENT_KEY_UP,           /**< A key came up. */
    TB_WINDOW_EVENT_DISPLAYS_CHANGED, /**< A display came, went, or changed. */
    TB_WINDOW_EVENT_WAKE,             /**< tb_window_system_wake() was called. */
    /** A mouse button went down over the window. The window holds the mouse
     * until every button is up again, and reports it even outside. */
    TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
    TB_WINDOW_EVENT_MOUSE_BUTTON_UP, /**< A mouse button came up. */
    /** The mouse moved over the window, or while the window held it; never
     * in relative mouse mode. */
    TB_WINDOW_EVENT_MOUSE_MOVED,
    /** The mouse moved in relative mouse mode: the event's deltas are the
     * mouse's own motion, and it has no position. */
    TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION,
    TB_WINDOW_EVENT_MOUSE_WHEEL,   /**< The wheel turned, or a touchpad scrolled. */
    TB_WINDOW_EVENT_MOUSE_ENTERED, /**< The mouse came over the window. */
    TB_WINDOW_EVENT_MOUSE_LEFT,    /**< The mouse left it. */
    /** Text was typed: the event's text, as the system composed it from
     * keys, dead keys included. The keys come as keys too. */
    TB_WINDOW_EVENT_TEXT_INPUT
} tb_window_event_type;

/** @brief An event, from tb_window_system_poll(). */
typedef struct tb_window_event
{
    tb_window_event_type type;
    tb_window *window; /**< The window it is about; NULL for displays and wakes. */
    tb_key key;        /**< For keys: which. */
    bool repeat;       /**< For a key down: the system repeats a key held down. */
    /** For keys: the system's own code of the key, for keys #tb_key does not
     * name. */
    uint32_t scancode;
    /** For keys, mouse buttons, the mouse's motion, and the wheel: the
     * #tb_modifier bits held. */
    uint32_t modifiers;
    tb_mouse_button button; /**< For mouse buttons: which. */
    /** For mouse buttons, motion, the wheel, and entering: where the mouse
     * is, in logical units from the client area's top-left corner; outside
     * the client area while the window holds the mouse. */
    double x;
    double y;       /**< See `x`; down is positive. */
    double pixel_x; /**< The same place in pixels: `x` times the content scale. */
    double pixel_y; /**< See `pixel_x`. */
    /** For the wheel: how far it turned, in notches, with fractions of one
     * from a wheel or touchpad of a finer resolution. Positive `delta_y` is
     * away from the player, which scrolls up; positive `delta_x` scrolls
     * right. For relative motion: how far the mouse moved, in the system's
     * unaccelerated units where it has them; positive `delta_y` is toward
     * the player. */
    double delta_x;
    double delta_y; /**< See `delta_x`. */
    /** For text: UTF-8, terminated; never a control character. */
    char text[TB_WINDOW_TEXT_CAPACITY];
    /** For a resize or a scale change: the window's state after it. */
    tb_window_state state;
} tb_window_event;

/** @brief The system's own handles of a window, for a renderer. */
typedef struct tb_window_native
{
    tb_window_backend backend; /**< Which of the fields below mean what. */
    /** Win32: the HINSTANCE. Wayland: the wl_display. X11: the Display. Cocoa:
     * the NSView in the window. */
    void *system;
    /** Win32: the HWND. Wayland: the wl_surface. X11: NULL. Cocoa: the
     * NSWindow. */
    void *window;
    uint64_t x11_window; /**< X11: the Window; 0 elsewhere. */
} tb_window_native;

/* ---------------------------------------------------------------------------
 * The window system.
 * ------------------------------------------------------------------------- */

/**
 * @brief Connect to the system's windowing.
 *
 * Automatic tries the platform's own: on Linux, Wayland where a compositor
 * is running, else X11.
 *
 * @param options How; NULL for the defaults.
 * @param[out] system Receives the window system, or NULL on failure; required.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_WINDOW_OK; #TB_WINDOW_UNAVAILABLE when there is no windowing to
 * connect to, as on a server or in CI without a display; or
 * #TB_WINDOW_SYSTEM_ERROR.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_window_result tb_window_system_open(const tb_window_system_options *options,
                                              tb_window_system **system, tb_window_error *error);

/**
 * @brief Close a window system. Its windows must be destroyed first.
 * @param system The window system; NULL is ignored.
 */
TB_API void tb_window_system_close(tb_window_system *system);

/** @brief The windowing a window system connected to.
 * @param system Required non-null. */
TB_API tb_window_backend tb_window_system_backend(const tb_window_system *system);

/** @brief The number of displays; at least one.
 * @param system Required non-null. */
TB_API size_t tb_window_system_display_count(const tb_window_system *system);

/**
 * @brief Describe a display. The primary display is index 0, the others
 * follow in the system's order.
 * @param system Required non-null.
 * @param index Less than tb_window_system_display_count().
 * @param[out] display Receives the description; required.
 */
TB_API void tb_window_system_display(const tb_window_system *system, size_t index,
                                     tb_display *display);

/**
 * @brief Take the next event, reading what the system has sent first.
 *
 * Never waits. Each call that finds no event queued reads the system's
 * messages, which keeps the windows responsive: call it until it returns
 * false, once a frame at least.
 *
 * @param system Required non-null.
 * @param[out] event Receives the event; required.
 * @return Whether there was an event.
 */
TB_API bool tb_window_system_poll(tb_window_system *system, tb_window_event *event);

/**
 * @brief Wait until the system sends a message, tb_window_system_wake() is
 * called, or time passes; then tb_window_system_poll() has the events.
 * @param system Required non-null.
 * @param nanoseconds The longest to wait; 0 returns at once.
 */
TB_API void tb_window_system_wait(tb_window_system *system, uint64_t nanoseconds);

/**
 * @brief End a wait of the window system's thread, from any thread, with a
 * #TB_WINDOW_EVENT_WAKE event.
 * @param system Required non-null; open.
 * @par Threading Thread-safe.
 */
TB_API void tb_window_system_wake(tb_window_system *system);

/* ---------------------------------------------------------------------------
 * Windows.
 * ------------------------------------------------------------------------- */

/**
 * @brief Create a window and show it.
 * @param system Required non-null.
 * @param options How; NULL for the defaults.
 * @param[out] window Receives the window, or NULL on failure; required.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_WINDOW_OK or #TB_WINDOW_SYSTEM_ERROR.
 * @par Errors Exhaustion calls the memory failure handler and ends the process.
 */
TB_API tb_window_result tb_window_create(tb_window_system *system, const tb_window_options *options,
                                         tb_window **window, tb_window_error *error);

/**
 * @brief Destroy a window, giving back any display mode it set.
 * @param window The window; NULL is ignored.
 */
TB_API void tb_window_destroy(tb_window *window);

/**
 * @brief Change a window's mode, display, size, or title, as the options
 * say: what an options menu does while the game runs.
 *
 * What it changes is reported by events, as for a change the player made.
 *
 * @param window Required non-null.
 * @param options The window's new options; required.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_WINDOW_OK, or #TB_WINDOW_SYSTEM_ERROR with the window as it
 * was.
 */
TB_API tb_window_result tb_window_configure(tb_window *window, const tb_window_options *options,
                                            tb_window_error *error);

/**
 * @brief Where a window is, how large, and how it fills the screen.
 * @param window Required non-null.
 * @param[out] state Receives it; required.
 */
TB_API void tb_window_state_of(const tb_window *window, tb_window_state *state);

/**
 * @brief The system's own handles of a window, for a renderer.
 * @param window Required non-null.
 * @param[out] native Receives them; required. They are valid until the
 * window is destroyed.
 */
TB_API void tb_window_native_of(const tb_window *window, tb_window_native *native);

/**
 * @brief Ask the player a yes-or-no question in the system's own dialog, over
 * a window: "Quit? Unsaved progress may be lost."
 *
 * The dialog is modal: the call returns when the player answers, and the
 * window's events wait until then. Windows and macOS have such a dialog;
 * Wayland and X11 have none, until the engine's interface draws its own.
 *
 * @param window Required non-null.
 * @param title The question, short, in UTF-8; required.
 * @param message More about it, in UTF-8; required.
 * @param[out] confirmed Receives whether the player said yes; required.
 * Unchanged unless the result is #TB_WINDOW_OK.
 * @return #TB_WINDOW_OK when the player answered; #TB_WINDOW_UNAVAILABLE
 * where the system has no dialog to ask with.
 */
TB_API tb_window_result tb_window_confirm(tb_window *window, const char *title, const char *message,
                                          bool *confirmed);

/* ---------------------------------------------------------------------------
 * The mouse.
 * ------------------------------------------------------------------------- */

/**
 * @brief Turn relative mouse mode on or off: the cursor hidden and held in
 * the window, and the mouse's own motion reported, for a first-person
 * camera.
 *
 * While it is on, the window sends #TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION in
 * place of #TB_WINDOW_EVENT_MOUSE_MOVED; buttons and the wheel come as
 * before. The system lets the cursor go while the player is in another
 * window, and the window takes it again on the player's return. Windows
 * reads the motion from Raw Input, X11 from XInput2's raw motion, Wayland
 * from the relative-pointer and pointer-constraints protocols, and macOS
 * from Cocoa's deltas, which the system has accelerated.
 *
 * @param window Required non-null.
 * @param relative Whether it is on.
 * @param[out] error Receives why it failed; NULL when nobody asks.
 * @return #TB_WINDOW_OK; #TB_WINDOW_UNAVAILABLE where the system cannot, as
 * under a Wayland compositor without those protocols or without a mouse; or
 * #TB_WINDOW_SYSTEM_ERROR, as when another program holds the mouse. Turning
 * it off always succeeds. The mode is unchanged on failure.
 */
TB_API tb_window_result tb_window_set_relative_mouse(tb_window *window, bool relative,
                                                     tb_window_error *error);

/**
 * @brief Give the cursor a shape while it is over a window's client area.
 * A shape the system lacks is the nearest it has, or the arrow.
 * @param window Required non-null.
 * @param cursor Less than #TB_CURSOR_COUNT; a new window's is the arrow.
 */
TB_API void tb_window_set_cursor(tb_window *window, tb_cursor cursor);

/**
 * @brief Show or hide the cursor while it is over a window's client area.
 * Relative mouse mode hides it whatever this says.
 * @param window Required non-null.
 * @param shown Whether it is seen; a new window's is.
 */
TB_API void tb_window_show_cursor(tb_window *window, bool shown);

/** @brief Name a backend: "wayland". @param backend Any value.
 * @return A string with static storage duration; "unknown backend" for no backend. */
TB_API const char *tb_window_backend_name(tb_window_backend backend);

/** @brief Name a result: "unavailable". @param result Any value.
 * @return A string with static storage duration; "unknown result" for no result. */
TB_API const char *tb_window_result_name(tb_window_result result);

/** @brief Name a key: "escape", "f1", "left_shift". @param key Any value.
 * @return A string with static storage duration; "unknown" for no key. */
TB_API const char *tb_key_name(tb_key key);

/** @brief Name a mouse button: "left", "x1". @param button Any value.
 * @return A string with static storage duration; "none" for no button. */
TB_API const char *tb_mouse_button_name(tb_mouse_button button);

#endif
