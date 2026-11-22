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
};

struct tb_window
{
    tb_window_system *system;
    void *native;              /* The backend's state. */
    tb_window_options options; /* As last created or configured; its title is `title`. */
    char *title;               /* The heap's. */
    tb_window_state state;
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
void tb_window_key(tb_window *window, tb_key key, uint32_t scancode, bool down, bool repeat);

/* Queue that the displays changed, after tb_window_set_displays(). */
void tb_window_displays_changed(tb_window_system *system);

/* Queue that the system was woken. */
void tb_window_woken(tb_window_system *system);

#endif
