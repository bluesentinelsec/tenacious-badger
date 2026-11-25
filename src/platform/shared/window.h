/* What the shared front of the window interface and each platform's
 * windowing share: the records of a window system and its windows, the
 * table of functions a backend fills in, and the helpers that turn what a
 * backend sees into events. */
#ifndef TB_PLATFORM_SHARED_WINDOW_H
#define TB_PLATFORM_SHARED_WINDOW_H

#include "badger/array.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a backend does; each function is required. */
typedef struct tb_window_ops
{
    /* Release the backend's state. Every window is destroyed. */
    void (*close)(tb_window_system *system);
    /* Read the system's messages, turning them into events and displays.
     * When none is waiting, wait up to `nanoseconds` for one, or for a
     * wake. */
    void (*pump)(tb_window_system *system, uint64_t nanoseconds);
    /* End a pump's wait; thread-safe. */
    void (*wake)(tb_window_system *system);
    /* Create the window the record describes, as `window->options` and
     * tb_window_place() say, and show it; fill in `window->state`. */
    tb_window_result (*create)(tb_window *window, tb_window_error *error);
    /* Destroy the window, giving back any display mode it set. */
    void (*destroy)(tb_window *window);
    /* Apply `window->options`, which were `previous` before. */
    tb_window_result (*configure)(tb_window *window, const tb_window_options *previous,
                                  tb_window_error *error);
    /* The window's handles. */
    void (*native)(const tb_window *window, tb_window_native *native);
    /* Ask a yes-or-no question in the system's dialog; UNAVAILABLE without
     * one. */
    tb_window_result (*confirm)(tb_window *window, const char *title, const char *message,
                                bool *confirmed);
    /* Turn relative mouse mode on or off, before the front records it in
     * `window->state`. Turning it off succeeds. */
    tb_window_result (*set_relative_mouse)(tb_window *window, bool relative,
                                           tb_window_error *error);
    /* Show the cursor of `window->cursor`, or none when `window->cursor_hidden`
     * or relative mouse mode says so. */
    void (*update_cursor)(tb_window *window);
} tb_window_ops;

struct tb_window_system
{
    tb_window_backend backend;
    const tb_window_ops *ops;
    void *native;       /* The backend's state. */
    tb_array *displays; /* tb_display; the primary first. */
    tb_array *events;   /* tb_window_event, a queue from `next_event`. */
    size_t next_event;
    tb_array *windows; /* tb_window *. */
    /* The backend reads the mouse's position in pixels (Win32, X11), not in
     * logical units (Wayland, Cocoa). */
    bool pointer_in_pixels;
    /* The backend keeps a window's aspect while the player resizes it: all
     * but Wayland. */
    bool keeps_aspect;
};

struct tb_window
{
    tb_window_system *system;
    void *native;              /* The backend's state. */
    tb_window_options options; /* As last created or configured; its title is `title`. */
    char *title;               /* The heap's. */
    tb_window_state state;
    tb_cursor cursor;
    bool cursor_hidden;
    bool mouse_inside; /* The last crossing reported was an entering. */
};

/* Where and how large a window goes, in the units its backend places
 * windows by. */
typedef struct tb_window_placement
{
    size_t display; /* The display, after a missing one became the primary. */
    uint32_t width; /* The client area, in logical units. */
    uint32_t height;
    int32_t x; /* The client area's corner, centered in the usable area, in
                  display units. */
    int32_t y;
    uint32_t unit_width; /* The client area in display units. */
    uint32_t unit_height;
} tb_window_placement;

/* Connect a window system to the platform's windowing: set `system->backend`,
 * `ops`, and `native`, and give it its displays. Each platform adapter
 * defines it. `requested` is never automatic for a backend of another
 * platform, which is unavailable. */
tb_window_result tb_native_window_open(tb_window_system *system, tb_window_backend requested,
                                       tb_window_error *error);

/* Fill in an error, formatted as tb_fmt_buffer() does; returns `result`.
 * `error` may be NULL. */
tb_window_result tb_window_fail(tb_window_error *error, tb_window_result result, const char *format,
                                ...);

/* Replace the system's displays with `count` of them, the primary first,
 * whatever order they come in. A system that reports none gets one of
 * 1280 by 720, so that windows still have somewhere to go. */
void tb_window_set_displays(tb_window_system *system, const tb_display *displays, size_t count);

/* Where a window goes, by its options and its system's displays.
 * `units_are_pixels` says whether the backend places windows in pixels (Win32,
 * X11) or in logical units (Wayland, Cocoa). */
void tb_window_place(const tb_window *window, bool units_are_pixels,
                     tb_window_placement *placement);

/* ---------------------------------------------------------------------------
 * Limits on a window's size, in window_size.c: arithmetic alone, so that it
 * is tested without a window.
 * ------------------------------------------------------------------------- */

/* The smallest a window's client area may be, in logical units. */
#define TB_WINDOW_SMALLEST 64u

/* The edge or corner of a window's frame that the player drags. */
typedef enum tb_window_edge
{
    TB_WINDOW_EDGE_LEFT,
    TB_WINDOW_EDGE_RIGHT,
    TB_WINDOW_EDGE_TOP,
    TB_WINDOW_EDGE_BOTTOM,
    TB_WINDOW_EDGE_TOP_LEFT,
    TB_WINDOW_EDGE_TOP_RIGHT,
    TB_WINDOW_EDGE_BOTTOM_LEFT,
    TB_WINDOW_EDGE_BOTTOM_RIGHT
} tb_window_edge;

/* A window's frame on the desktop, in pixels; `right` and `bottom` are past
 * its last pixel. */
typedef struct tb_window_rect
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} tb_window_rect;

/* The least client area the options allow, in logical units: their
 * minimum, and never less than the smallest. */
void tb_window_least_size(const tb_window_options *options, uint32_t *width, uint32_t *height);

/* Whether a window of these options keeps their aspect while the player
 * resizes it: windowed, resizable, and with an aspect. */
bool tb_window_wants_aspect(const tb_window_options *options);

/* Fit a windowed size, in logical units, to the options and to the room
 * there is: at least their least size, at most the room, and of their
 * aspect when the window keeps one. The room wins when they disagree. */
void tb_window_fit_size(const tb_window_options *options, uint32_t room_width, uint32_t room_height,
                        uint32_t *width, uint32_t *height);

/* Change a frame that the player is dragging by an edge or a corner so that
 * its client area keeps an aspect and is at least a least size. The edges
 * across from those dragged stay where they are: dragging the left or the
 * right side moves the bottom too, and dragging the top or the bottom moves
 * the right side. Dragging a corner follows whichever of its two edges makes
 * the larger window. `border_width` and `border_height` are how much larger
 * the frame is than its client area; sizes are in pixels; neither part of
 * the aspect is 0. */
void tb_window_drag_to_aspect(tb_window_rect *frame, tb_window_edge edge, uint32_t border_width,
                              uint32_t border_height, uint32_t aspect_width, uint32_t aspect_height,
                              uint32_t least_width, uint32_t least_height);

/* The display a rectangle in display units overlaps most, the primary when
 * it overlaps none. */
size_t tb_window_display_at(const tb_window_system *system, int32_t x, int32_t y, uint32_t width,
                            uint32_t height);

/* Queue an event. */
void tb_window_push(tb_window_system *system, const tb_window_event *event);

/* Record a window's new size and scale, queueing a resize or a scale change
 * when they differ from what it had. */
void tb_window_resized(tb_window *window, uint32_t pixel_width, uint32_t pixel_height,
                       double content_scale);

/* Record focus or minimizing, queueing an event when it changed. */
void tb_window_focused(tb_window *window, bool focused);
void tb_window_minimized(tb_window *window, bool minimized);

/* Queue a key's event. */
void tb_window_key(tb_window *window, tb_key key, uint32_t scancode, bool down, bool repeat,
                   uint32_t modifiers);

/* Queue a mouse button's event at a position in the units the system reads
 * the mouse in. */
void tb_window_mouse_button(tb_window *window, tb_mouse_button button, bool down, double x,
                            double y, uint32_t modifiers);

/* Queue the mouse's motion to a position; nothing in relative mouse mode. */
void tb_window_mouse_moved(tb_window *window, double x, double y, uint32_t modifiers);

/* Queue the wheel's turn, in notches, positive away from the player and to
 * the right, with the mouse at a position. */
void tb_window_mouse_wheel(tb_window *window, double notches_x, double notches_y, double x,
                           double y, uint32_t modifiers);

/* Queue relative motion; nothing outside relative mouse mode. */
void tb_window_mouse_relative(tb_window *window, double delta_x, double delta_y);

/* Queue the mouse's entering or leaving, when it changed; the position is
 * that of an entering. */
void tb_window_mouse_crossed(tb_window *window, bool inside, double x, double y);

/* Queue typed text, `length` bytes of UTF-8, in events of whole characters,
 * without its control characters or anything that is not UTF-8. */
void tb_window_text(tb_window *window, const char *text, size_t length);

/* Queue that the displays changed, after tb_window_set_displays(). */
void tb_window_displays_changed(tb_window_system *system);

/* Queue that the system was woken. */
void tb_window_woken(tb_window_system *system);

#endif
