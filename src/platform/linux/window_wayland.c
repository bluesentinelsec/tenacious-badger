/* Wayland windowing, from libwayland-client.so.0 loaded at run time.
 *
 * libwayland marshals requests and dispatches events from tables that
 * describe each interface; the core interfaces' tables are data that the
 * library exports, and those of xdg-shell and xdg-decoration, which it does
 * not, are written out here from the protocols' XML. Building the engine
 * needs no Wayland headers or scanner.
 *
 * A surface is shown only once it has content, so until a renderer presents
 * into it a window holds a black buffer of shared memory: a file of zeros,
 * which is black in XRGB8888, written by ftruncate() alone.
 *
 * What Wayland does not let a client do, it does not: a client cannot place
 * its window, so a new window is wherever the compositor puts it; there is
 * no fullscreen of a client's own mode, so fullscreen is borderless; and
 * decorations are the compositor's when it offers xdg-decoration, which
 * GNOME does not. Scale is the integer scale of the outputs a surface is on.
 *
 * libwayland takes the tables of event handlers, its listeners, as writable
 * pointers, so they are not const here, though nothing writes them. */
#define _GNU_SOURCE
#include "window_linux.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * libwayland, as much as is used.
 * ------------------------------------------------------------------------- */

typedef struct tb_wl_display tb_wl_display;
typedef struct tb_wl_proxy tb_wl_proxy;

typedef struct tb_wl_interface tb_wl_interface;

typedef struct tb_wl_message
{
    const char *name;
    const char *signature;
    const tb_wl_interface **types;
} tb_wl_message;

struct tb_wl_interface
{
    const char *name;
    int version;
    int method_count;
    const tb_wl_message *methods;
    int event_count;
    const tb_wl_message *events;
};

typedef struct tb_wl_array
{
    size_t size;
    size_t alloc;
    void *data;
} tb_wl_array;

typedef void (*tb_wl_handler)(void);

#define TB_WL_MARSHAL_DESTROY 1u
#define TB_WL_SHM_FORMAT_XRGB8888 1u
#define TB_WL_SEAT_KEYBOARD 2u
#define TB_WL_OUTPUT_MODE_CURRENT 1u
#define TB_WL_KEY_PRESSED 1u
#define TB_XDG_DECORATION_SERVER_SIDE 2u

typedef struct tb_wl_api
{
    tb_wl_display *(*connect)(const char *name);
    void (*disconnect)(tb_wl_display *display);
    int (*get_fd)(tb_wl_display *display);
    int (*dispatch_pending)(tb_wl_display *display);
    int (*flush)(tb_wl_display *display);
    int (*roundtrip)(tb_wl_display *display);
    int (*prepare_read)(tb_wl_display *display);
    int (*read_events)(tb_wl_display *display);
    void (*cancel_read)(tb_wl_display *display);
    tb_wl_proxy *(*marshal_flags)(tb_wl_proxy *proxy, uint32_t opcode,
                                  const tb_wl_interface *interface, uint32_t version,
                                  uint32_t flags, ...);
    int (*add_listener)(tb_wl_proxy *proxy, tb_wl_handler *implementation, void *data);
    void (*destroy)(tb_wl_proxy *proxy);
    uint32_t (*get_version)(tb_wl_proxy *proxy);
} tb_wl_api;

/* The core interfaces, which libwayland exports as data. */
typedef struct tb_wl_core
{
    const tb_wl_interface *registry;
    const tb_wl_interface *compositor;
    const tb_wl_interface *surface;
    const tb_wl_interface *seat;
    const tb_wl_interface *keyboard;
    const tb_wl_interface *output;
    const tb_wl_interface *shm;
    const tb_wl_interface *shm_pool;
    const tb_wl_interface *buffer;
} tb_wl_core;

/* ---------------------------------------------------------------------------
 * xdg-shell and xdg-decoration, from their XML. Their tables name the core
 * interfaces, which are known only once libwayland is loaded, so the type
 * lists are filled in then.
 * ------------------------------------------------------------------------- */

extern const tb_wl_interface tb_xdg_wm_base_interface;
extern const tb_wl_interface tb_xdg_surface_interface;
extern const tb_wl_interface tb_xdg_toplevel_interface;
extern const tb_wl_interface tb_xdg_decoration_manager_interface;
extern const tb_wl_interface tb_xdg_toplevel_decoration_interface;

static const tb_wl_interface *tb_wl_no_types[4] = {NULL, NULL, NULL, NULL};
static const tb_wl_interface *tb_xdg_get_surface_types[2] = {&tb_xdg_surface_interface, NULL};
static const tb_wl_interface *tb_xdg_get_toplevel_types[1] = {&tb_xdg_toplevel_interface};
static const tb_wl_interface *tb_xdg_toplevel_types[1] = {&tb_xdg_toplevel_interface};
static const tb_wl_interface *tb_xdg_seat_types[1] = {NULL};
static const tb_wl_interface *tb_xdg_output_types[1] = {NULL};
static const tb_wl_interface *tb_xdg_get_decoration_types[2] = {
    &tb_xdg_toplevel_decoration_interface, &tb_xdg_toplevel_interface};

static const tb_wl_message tb_xdg_wm_base_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"create_positioner", "n", tb_wl_no_types},
    {"get_xdg_surface", "no", tb_xdg_get_surface_types},
    {"pong", "u", tb_wl_no_types},
};
static const tb_wl_message tb_xdg_wm_base_events[] = {{"ping", "u", tb_wl_no_types}};
const tb_wl_interface tb_xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, tb_xdg_wm_base_requests, 1, tb_xdg_wm_base_events};

static const tb_wl_message tb_xdg_surface_requests[] = {
    {"destroy", "", tb_wl_no_types},        {"get_toplevel", "n", tb_xdg_get_toplevel_types},
    {"get_popup", "n?oo", tb_wl_no_types},  {"set_window_geometry", "iiii", tb_wl_no_types},
    {"ack_configure", "u", tb_wl_no_types},
};
static const tb_wl_message tb_xdg_surface_events[] = {{"configure", "u", tb_wl_no_types}};
const tb_wl_interface tb_xdg_surface_interface = {
    "xdg_surface", 1, 5, tb_xdg_surface_requests, 1, tb_xdg_surface_events};

static const tb_wl_message tb_xdg_toplevel_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"set_parent", "?o", tb_xdg_toplevel_types},
    {"set_title", "s", tb_wl_no_types},
    {"set_app_id", "s", tb_wl_no_types},
    {"show_window_menu", "ouii", tb_xdg_seat_types},
    {"move", "ou", tb_xdg_seat_types},
    {"resize", "ouu", tb_xdg_seat_types},
    {"set_max_size", "ii", tb_wl_no_types},
    {"set_min_size", "ii", tb_wl_no_types},
    {"set_maximized", "", tb_wl_no_types},
    {"unset_maximized", "", tb_wl_no_types},
    {"set_fullscreen", "?o", tb_xdg_output_types},
    {"unset_fullscreen", "", tb_wl_no_types},
    {"set_minimized", "", tb_wl_no_types},
};
static const tb_wl_message tb_xdg_toplevel_events[] = {
    {"configure", "iia", tb_wl_no_types},
    {"close", "", tb_wl_no_types},
};
const tb_wl_interface tb_xdg_toplevel_interface = {
    "xdg_toplevel", 1, 14, tb_xdg_toplevel_requests, 2, tb_xdg_toplevel_events};

static const tb_wl_message tb_xdg_decoration_manager_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"get_toplevel_decoration", "no", tb_xdg_get_decoration_types},
};
const tb_wl_interface tb_xdg_decoration_manager_interface = {
    "zxdg_decoration_manager_v1", 1, 2, tb_xdg_decoration_manager_requests, 0, NULL};

static const tb_wl_message tb_xdg_toplevel_decoration_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"set_mode", "u", tb_wl_no_types},
    {"unset_mode", "", tb_wl_no_types},
};
static const tb_wl_message tb_xdg_toplevel_decoration_events[] = {
    {"configure", "u", tb_wl_no_types}};
const tb_wl_interface tb_xdg_toplevel_decoration_interface = {
    "zxdg_toplevel_decoration_v1",       1, 3,
    tb_xdg_toplevel_decoration_requests, 1, tb_xdg_toplevel_decoration_events};

/* Requests, by their opcodes in the protocols' XML. */
enum
{
    TB_WL_DISPLAY_GET_REGISTRY = 1,
    TB_WL_REGISTRY_BIND = 0,
    TB_WL_COMPOSITOR_CREATE_SURFACE = 0,
    TB_WL_SURFACE_DESTROY = 0,
    TB_WL_SURFACE_ATTACH = 1,
    TB_WL_SURFACE_DAMAGE = 2,
    TB_WL_SURFACE_COMMIT = 6,
    TB_WL_SURFACE_SET_BUFFER_SCALE = 8,
    TB_WL_SHM_CREATE_POOL = 0,
    TB_WL_SHM_POOL_CREATE_BUFFER = 0,
    TB_WL_SHM_POOL_DESTROY = 1,
    TB_WL_BUFFER_DESTROY = 0,
    TB_WL_SEAT_GET_KEYBOARD = 1,
    TB_WL_SEAT_RELEASE = 3,
    TB_WL_KEYBOARD_RELEASE = 0,
    TB_WL_OUTPUT_RELEASE = 0,
    TB_XDG_WM_BASE_DESTROY = 0,
    TB_XDG_WM_BASE_GET_XDG_SURFACE = 2,
    TB_XDG_WM_BASE_PONG = 3,
    TB_XDG_SURFACE_DESTROY = 0,
    TB_XDG_SURFACE_GET_TOPLEVEL = 1,
    TB_XDG_SURFACE_ACK_CONFIGURE = 4,
    TB_XDG_TOPLEVEL_DESTROY = 0,
    TB_XDG_TOPLEVEL_SET_TITLE = 2,
    TB_XDG_TOPLEVEL_SET_MAX_SIZE = 7,
    TB_XDG_TOPLEVEL_SET_MIN_SIZE = 8,
    TB_XDG_TOPLEVEL_SET_FULLSCREEN = 11,
    TB_XDG_TOPLEVEL_UNSET_FULLSCREEN = 12,
    TB_XDG_DECORATION_MANAGER_DESTROY = 0,
    TB_XDG_DECORATION_MANAGER_GET = 1,
    TB_XDG_TOPLEVEL_DECORATION_DESTROY = 0,
    TB_XDG_TOPLEVEL_DECORATION_SET_MODE = 1
};

/* The highest versions the backend knows the events of. */
#define TB_WL_COMPOSITOR_VERSION 6u
#define TB_WL_OUTPUT_VERSION 4u
#define TB_WL_SEAT_VERSION 5u
#define TB_WL_SHM_VERSION 1u

/* ---------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */

typedef struct tb_wl_system tb_wl_system;

typedef struct tb_wl_output
{
    tb_wl_system *owner;
    tb_wl_proxy *proxy;
    uint32_t name; /* The registry's name of the global. */
    tb_display display;
    int32_t scale;
    int32_t mode_width; /* Pixels. */
    int32_t mode_height;
    char make[48];
    char model[48];
    bool named; /* A name event came; it is better than make and model. */
} tb_wl_output;

struct tb_wl_system
{
    tb_library *library;
    tb_wl_api wl;
    tb_wl_core core;
    tb_wl_display *display;
    tb_wl_proxy *registry;
    tb_wl_proxy *compositor;
    tb_wl_proxy *shm;
    tb_wl_proxy *seat;
    tb_wl_proxy *keyboard;
    tb_wl_proxy *wm_base;
    tb_wl_proxy *decoration_manager;
    tb_array *outputs; /* tb_wl_output *; the first is the primary. */
    tb_window *keyboard_focus;
    tb_linux_waker waker;
    tb_window_system *system;
    bool displays_changed;
};

typedef struct tb_wl_window
{
    tb_wl_proxy *surface;
    tb_wl_proxy *xdg_surface;
    tb_wl_proxy *toplevel;
    tb_wl_proxy *decoration;
    tb_array *buffers;        /* tb_wl_proxy *, attached and not yet released. */
    tb_array *entered;        /* tb_wl_output *, the outputs the surface is on. */
    int32_t configured_width; /* As the compositor's last configure asked; 0 for the client's. */
    int32_t configured_height;
    uint32_t width; /* The logical size shown. */
    uint32_t height;
    int32_t scale;
    int32_t preferred_scale; /* From wl_surface.preferred_buffer_scale; 0 before one. */
    bool configured;
} tb_wl_window;

static tb_wl_system *tb_wl_of(const tb_window_system *system)
{
    return system->native;
}

static tb_wl_window *tb_wl_window_of(const tb_window *window)
{
    return window->native;
}

/* ---------------------------------------------------------------------------
 * Displays.
 * ------------------------------------------------------------------------- */

static void tb_wl_publish_displays(tb_wl_system *native)
{
    const size_t count = tb_array_length(native->outputs);
    tb_display *const displays = tb_calloc(count > 0 ? count : 1, sizeof(tb_display));

    for (size_t index = 0; index < count; index++)
    {
        tb_wl_output *const output = *TB_ARRAY_AT(native->outputs, tb_wl_output *, index);
        tb_display *const display = &displays[index];
        const int32_t scale = output->scale > 0 ? output->scale : 1;
        *display = output->display;
        if (!output->named)
        {
            (void)tb_fmt_buffer(display->name, sizeof(display->name), "%s %s", output->make,
                                output->model);
        }
        display->primary = index == 0;
        display->pixel_width = (uint32_t)output->mode_width;
        display->pixel_height = (uint32_t)output->mode_height;
        display->width = (uint32_t)(output->mode_width / scale);
        display->height = (uint32_t)(output->mode_height / scale);
        display->usable_x = display->x;
        display->usable_y = display->y;
        display->usable_width = display->width;
        display->usable_height = display->height;
        display->content_scale = (double)scale;
    }
    tb_window_set_displays(native->system, displays, count);
    tb_free(displays);
}

static void tb_wl_output_geometry(void *data, tb_wl_proxy *proxy, int32_t x, int32_t y,
                                  int32_t width_mm, int32_t height_mm, int32_t subpixel,
                                  const char *make, const char *model, int32_t transform)
{
    tb_wl_output *const output = data;

    (void)proxy;
    (void)subpixel;
    (void)transform;
    output->display.x = x;
    output->display.y = y;
    output->display.physical_width_mm = width_mm > 0 ? (uint32_t)width_mm : 0u;
    output->display.physical_height_mm = height_mm > 0 ? (uint32_t)height_mm : 0u;
    (void)tb_fmt_buffer(output->make, sizeof(output->make), "%s", make != NULL ? make : "");
    (void)tb_fmt_buffer(output->model, sizeof(output->model), "%s", model != NULL ? model : "");
}

static void tb_wl_output_mode(void *data, tb_wl_proxy *proxy, uint32_t flags, int32_t width,
                              int32_t height, int32_t refresh)
{
    tb_wl_output *const output = data;

    (void)proxy;
    if ((flags & TB_WL_OUTPUT_MODE_CURRENT) == 0)
    {
        return;
    }
    output->mode_width = width;
    output->mode_height = height;
    output->display.refresh_millihertz = refresh > 0 ? (uint32_t)refresh : 0u;
}

/* Every output event is followed by done, once the compositor has said all
 * it will. */
static void tb_wl_output_done(void *data, tb_wl_proxy *proxy)
{
    tb_wl_output *const output = data;

    (void)proxy;
    output->owner->displays_changed = true;
}

static void tb_wl_output_scale(void *data, tb_wl_proxy *proxy, int32_t factor)
{
    tb_wl_output *const output = data;

    (void)proxy;
    output->scale = factor;
}

static void tb_wl_output_name(void *data, tb_wl_proxy *proxy, const char *name)
{
    tb_wl_output *const output = data;

    (void)proxy;
    (void)tb_fmt_buffer(output->display.name, sizeof(output->display.name), "%s",
                        name != NULL ? name : "");
    output->named = true;
}

static void tb_wl_output_description(void *data, tb_wl_proxy *proxy, const char *description)
{
    (void)data;
    (void)proxy;
    (void)description;
}

static struct
{
    void (*geometry)(void *, tb_wl_proxy *, int32_t, int32_t, int32_t, int32_t, int32_t,
                     const char *, const char *, int32_t);
    void (*mode)(void *, tb_wl_proxy *, uint32_t, int32_t, int32_t, int32_t);
    void (*done)(void *, tb_wl_proxy *);
    void (*scale)(void *, tb_wl_proxy *, int32_t);
    void (*name)(void *, tb_wl_proxy *, const char *);
    void (*description)(void *, tb_wl_proxy *, const char *);
} tb_wl_output_listener = {tb_wl_output_geometry, tb_wl_output_mode, tb_wl_output_done,
                           tb_wl_output_scale,    tb_wl_output_name, tb_wl_output_description};

/* ---------------------------------------------------------------------------
 * The registry.
 * ------------------------------------------------------------------------- */

static uint32_t tb_wl_min(uint32_t left, uint32_t right)
{
    return left < right ? left : right;
}

static tb_wl_proxy *tb_wl_bind(tb_wl_system *native, uint32_t name,
                               const tb_wl_interface *interface, uint32_t version)
{
    return native->wl.marshal_flags(native->registry, TB_WL_REGISTRY_BIND, interface, version, 0,
                                    name, interface->name, version, NULL);
}

static void tb_wl_add_output(tb_wl_system *native, uint32_t name, uint32_t version)
{
    tb_wl_output *const output = tb_calloc(1, sizeof(tb_wl_output));

    output->owner = native;
    output->name = name;
    output->scale = 1;
    output->proxy =
        tb_wl_bind(native, name, native->core.output, tb_wl_min(version, TB_WL_OUTPUT_VERSION));
    (void)native->wl.add_listener(output->proxy, (tb_wl_handler *)&tb_wl_output_listener, output);
    (void)tb_array_push(native->outputs, &output);
    native->displays_changed = true;
}

static void tb_wl_release_output(tb_wl_system *native, tb_wl_output *output)
{
    if (native->wl.get_version(output->proxy) >= 3)
    {
        (void)native->wl.marshal_flags(output->proxy, TB_WL_OUTPUT_RELEASE, NULL,
                                       native->wl.get_version(output->proxy),
                                       TB_WL_MARSHAL_DESTROY);
    }
    else
    {
        /* coverage-off: every current compositor has version 3 */
        native->wl.destroy(output->proxy);
        /* coverage-on */
    }
    tb_free(output);
}

static void tb_wl_seat_capabilities(void *data, tb_wl_proxy *seat, uint32_t capabilities);

static void tb_wl_seat_name(void *data, tb_wl_proxy *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static void tb_wl_global(void *data, tb_wl_proxy *registry, uint32_t name, const char *interface,
                         uint32_t version)
{
    tb_wl_system *const native = data;

    (void)registry;
    if (tb_str_equal(interface, "wl_compositor"))
    {
        native->compositor = tb_wl_bind(native, name, native->core.compositor,
                                        tb_wl_min(version, TB_WL_COMPOSITOR_VERSION));
    }
    else if (tb_str_equal(interface, "wl_shm"))
    {
        native->shm = tb_wl_bind(native, name, native->core.shm, TB_WL_SHM_VERSION);
    }
    else if (tb_str_equal(interface, "xdg_wm_base"))
    {
        native->wm_base = tb_wl_bind(native, name, &tb_xdg_wm_base_interface, 1);
    }
    else if (tb_str_equal(interface, "zxdg_decoration_manager_v1"))
    {
        native->decoration_manager =
            tb_wl_bind(native, name, &tb_xdg_decoration_manager_interface, 1);
    }
    else if (tb_str_equal(interface, "wl_seat") && native->seat == NULL)
    {
        static struct
        {
            void (*capabilities)(void *, tb_wl_proxy *, uint32_t);
            void (*name)(void *, tb_wl_proxy *, const char *);
        } listener = {tb_wl_seat_capabilities, tb_wl_seat_name};
        native->seat =
            tb_wl_bind(native, name, native->core.seat, tb_wl_min(version, TB_WL_SEAT_VERSION));
        (void)native->wl.add_listener(native->seat, (tb_wl_handler *)&listener, native);
    }
    else if (tb_str_equal(interface, "wl_output"))
    {
        tb_wl_add_output(native, name, version);
    }
}

static void tb_wl_global_remove(void *data, tb_wl_proxy *registry, uint32_t name)
{
    tb_wl_system *const native = data;

    (void)registry;
    for (size_t index = 0; index < tb_array_length(native->outputs); index++)
    {
        tb_wl_output *const output = *TB_ARRAY_AT(native->outputs, tb_wl_output *, index);
        if (output->name == name)
        {
            tb_array_remove(native->outputs, index, NULL);
            tb_wl_release_output(native, output);
            native->displays_changed = true;
            return;
        }
    }
}

static struct
{
    void (*global)(void *, tb_wl_proxy *, uint32_t, const char *, uint32_t);
    void (*global_remove)(void *, tb_wl_proxy *, uint32_t);
} tb_wl_registry_listener = {tb_wl_global, tb_wl_global_remove};

static void tb_wl_ping(void *data, tb_wl_proxy *wm_base, uint32_t serial)
{
    tb_wl_system *const native = data;

    (void)native->wl.marshal_flags(wm_base, TB_XDG_WM_BASE_PONG, NULL,
                                   native->wl.get_version(wm_base), 0, serial);
}

static struct
{
    void (*ping)(void *, tb_wl_proxy *, uint32_t);
} tb_wl_wm_base_listener = {tb_wl_ping};

/* ---------------------------------------------------------------------------
 * The keyboard.
 * ------------------------------------------------------------------------- */

static tb_window *tb_wl_window_of_surface(tb_wl_system *native, tb_wl_proxy *surface)
{
    for (size_t index = 0; index < tb_array_length(native->system->windows); index++)
    {
        tb_window *const window = *TB_ARRAY_AT(native->system->windows, tb_window *, index);
        if (tb_wl_window_of(window)->surface == surface)
        {
            return window;
        }
    }
    return NULL;
}

static void tb_wl_keymap(void *data, tb_wl_proxy *keyboard, uint32_t format, int32_t fd,
                         uint32_t size)
{
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    /* Keys are read by their codes; the keymap, for text, is not needed. */
    (void)close(fd);
}

static void tb_wl_keyboard_enter(void *data, tb_wl_proxy *keyboard, uint32_t serial,
                                 tb_wl_proxy *surface, tb_wl_array *keys)
{
    tb_wl_system *const native = data;

    (void)keyboard;
    (void)serial;
    (void)keys;
    native->keyboard_focus = tb_wl_window_of_surface(native, surface);
    if (native->keyboard_focus != NULL)
    {
        tb_window_focused(native->keyboard_focus, true);
    }
}

static void tb_wl_keyboard_leave(void *data, tb_wl_proxy *keyboard, uint32_t serial,
                                 tb_wl_proxy *surface)
{
    tb_wl_system *const native = data;
    tb_window *const window = tb_wl_window_of_surface(native, surface);

    (void)keyboard;
    (void)serial;
    if (window != NULL)
    {
        tb_window_focused(window, false);
    }
    native->keyboard_focus = NULL;
}

static void tb_wl_key(void *data, tb_wl_proxy *keyboard, uint32_t serial, uint32_t time,
                      uint32_t key, uint32_t state)
{
    tb_wl_system *const native = data;

    (void)keyboard;
    (void)serial;
    (void)time;
    if (native->keyboard_focus != NULL)
    {
        /* A compositor sends no repeats: a client repeats keys itself. */
        tb_window_key(native->keyboard_focus, tb_linux_key(key), key, state == TB_WL_KEY_PRESSED,
                      false);
    }
}

static void tb_wl_modifiers(void *data, tb_wl_proxy *keyboard, uint32_t serial, uint32_t depressed,
                            uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
}

static void tb_wl_repeat_info(void *data, tb_wl_proxy *keyboard, int32_t rate, int32_t delay)
{
    (void)data;
    (void)keyboard;
    (void)rate;
    (void)delay;
}

static struct
{
    void (*keymap)(void *, tb_wl_proxy *, uint32_t, int32_t, uint32_t);
    void (*enter)(void *, tb_wl_proxy *, uint32_t, tb_wl_proxy *, tb_wl_array *);
    void (*leave)(void *, tb_wl_proxy *, uint32_t, tb_wl_proxy *);
    void (*key)(void *, tb_wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*modifiers)(void *, tb_wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*repeat_info)(void *, tb_wl_proxy *, int32_t, int32_t);
} tb_wl_keyboard_listener = {tb_wl_keymap, tb_wl_keyboard_enter, tb_wl_keyboard_leave,
                             tb_wl_key,    tb_wl_modifiers,      tb_wl_repeat_info};

static void tb_wl_release_keyboard(tb_wl_system *native)
{
    if (native->keyboard == NULL)
    {
        return;
    }
    if (native->wl.get_version(native->keyboard) >= 3)
    {
        (void)native->wl.marshal_flags(native->keyboard, TB_WL_KEYBOARD_RELEASE, NULL,
                                       native->wl.get_version(native->keyboard),
                                       TB_WL_MARSHAL_DESTROY);
    }
    else
    {
        /* coverage-off: every current compositor has version 3 */
        native->wl.destroy(native->keyboard);
        /* coverage-on */
    }
    native->keyboard = NULL;
    native->keyboard_focus = NULL;
}

static void tb_wl_seat_capabilities(void *data, tb_wl_proxy *seat, uint32_t capabilities)
{
    tb_wl_system *const native = data;
    const bool has_keyboard = (capabilities & TB_WL_SEAT_KEYBOARD) != 0;

    if (has_keyboard && native->keyboard == NULL)
    {
        native->keyboard =
            native->wl.marshal_flags(seat, TB_WL_SEAT_GET_KEYBOARD, native->core.keyboard,
                                     native->wl.get_version(seat), 0, NULL);
        (void)native->wl.add_listener(native->keyboard, (tb_wl_handler *)&tb_wl_keyboard_listener,
                                      native);
    }
    else if (!has_keyboard)
    {
        tb_wl_release_keyboard(native);
    }
}

/* ---------------------------------------------------------------------------
 * Buffers.
 * ------------------------------------------------------------------------- */

static void tb_wl_forget_buffer(tb_wl_window *record, tb_wl_proxy *buffer)
{
    for (size_t index = 0; index < tb_array_length(record->buffers); index++)
    {
        if (*TB_ARRAY_AT(record->buffers, tb_wl_proxy *, index) == buffer)
        {
            tb_array_remove(record->buffers, index, NULL);
            return;
        }
    }
}

/* The compositor is done with a buffer: it has the picture, or a newer one. */
static void tb_wl_buffer_release(void *data, tb_wl_proxy *buffer)
{
    tb_window *const window = data;
    tb_wl_system *const native = tb_wl_of(window->system);

    tb_wl_forget_buffer(tb_wl_window_of(window), buffer);
    (void)native->wl.marshal_flags(buffer, TB_WL_BUFFER_DESTROY, NULL,
                                   native->wl.get_version(buffer), TB_WL_MARSHAL_DESTROY);
}

static struct
{
    void (*release)(void *, tb_wl_proxy *);
} tb_wl_buffer_listener = {tb_wl_buffer_release};

/* A black buffer of a size in pixels: a file of zeros in shared memory. */
static tb_wl_proxy *tb_wl_black_buffer(tb_window *window, uint32_t width, uint32_t height)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    const uint32_t stride = width * 4u;
    const size_t size = (size_t)stride * height;
    tb_wl_proxy *pool = NULL;
    tb_wl_proxy *buffer = NULL;
    const int fd = memfd_create("tb-window", MFD_CLOEXEC);

    if (fd < 0)
    {
        /* coverage-off: only a process out of descriptors has no memory file */
        return NULL;
        /* coverage-on */
    }
    if (ftruncate(fd, (off_t)size) != 0)
    {
        /* coverage-off: only a system out of memory refuses the size */
        (void)close(fd);
        return NULL;
        /* coverage-on */
    }
    pool =
        native->wl.marshal_flags(native->shm, TB_WL_SHM_CREATE_POOL, native->core.shm_pool,
                                 native->wl.get_version(native->shm), 0, NULL, fd, (int32_t)size);
    buffer = native->wl.marshal_flags(pool, TB_WL_SHM_POOL_CREATE_BUFFER, native->core.buffer,
                                      native->wl.get_version(pool), 0, NULL, 0, (int32_t)width,
                                      (int32_t)height, (int32_t)stride, TB_WL_SHM_FORMAT_XRGB8888);
    (void)native->wl.marshal_flags(pool, TB_WL_SHM_POOL_DESTROY, NULL, native->wl.get_version(pool),
                                   TB_WL_MARSHAL_DESTROY);
    (void)close(fd);
    (void)native->wl.add_listener(buffer, (tb_wl_handler *)&tb_wl_buffer_listener, window);
    (void)tb_array_push(tb_wl_window_of(window)->buffers, &buffer);
    return buffer;
}

/* The scale a window's buffer is drawn at: what the compositor prefers, or
 * the largest scale of the outputs the surface is on. */
static int32_t tb_wl_window_scale(const tb_window *window)
{
    const tb_wl_window *const record = tb_wl_window_of(window);
    int32_t scale = 1;

    if (record->preferred_scale > 0)
    {
        return record->preferred_scale;
    }
    for (size_t index = 0; index < tb_array_length(record->entered); index++)
    {
        const tb_wl_output *const output = *TB_ARRAY_AT(record->entered, tb_wl_output *, index);
        scale = output->scale > scale ? output->scale : scale;
    }
    return scale;
}

/* Show the window at its logical size and scale, with a buffer to match. */
static void tb_wl_present(tb_window *window)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);
    const int32_t scale = tb_wl_window_scale(window);
    const uint32_t pixel_width = record->width * (uint32_t)scale;
    const uint32_t pixel_height = record->height * (uint32_t)scale;
    tb_wl_proxy *const buffer = tb_wl_black_buffer(window, pixel_width, pixel_height);
    const uint32_t version = native->wl.get_version(record->surface);

    if (buffer == NULL)
    {
        /* coverage-off: see tb_wl_black_buffer() */
        return;
        /* coverage-on */
    }
    record->scale = scale;
    if (version >= 3)
    {
        (void)native->wl.marshal_flags(record->surface, TB_WL_SURFACE_SET_BUFFER_SCALE, NULL,
                                       version, 0, scale);
    }
    (void)native->wl.marshal_flags(record->surface, TB_WL_SURFACE_ATTACH, NULL, version, 0, buffer,
                                   0, 0);
    (void)native->wl.marshal_flags(record->surface, TB_WL_SURFACE_DAMAGE, NULL, version, 0, 0, 0,
                                   (int32_t)record->width, (int32_t)record->height);
    (void)native->wl.marshal_flags(record->surface, TB_WL_SURFACE_COMMIT, NULL, version, 0);
    tb_window_resized(window, pixel_width, pixel_height, (double)scale);
}

/* ---------------------------------------------------------------------------
 * Surfaces and toplevels.
 * ------------------------------------------------------------------------- */

static void tb_wl_surface_enter(void *data, tb_wl_proxy *surface, tb_wl_proxy *output_proxy)
{
    tb_window *const window = data;
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    (void)surface;
    for (size_t index = 0; index < tb_array_length(native->outputs); index++)
    {
        tb_wl_output *const output = *TB_ARRAY_AT(native->outputs, tb_wl_output *, index);
        if (output->proxy == output_proxy)
        {
            (void)tb_array_push(record->entered, &output);
            window->state.display = index;
        }
    }
    if (record->configured && tb_wl_window_scale(window) != record->scale)
    {
        tb_wl_present(window);
    }
}

static void tb_wl_surface_leave(void *data, tb_wl_proxy *surface, tb_wl_proxy *output_proxy)
{
    tb_window *const window = data;
    tb_wl_window *const record = tb_wl_window_of(window);

    (void)surface;
    for (size_t index = 0; index < tb_array_length(record->entered); index++)
    {
        if ((*TB_ARRAY_AT(record->entered, tb_wl_output *, index))->proxy == output_proxy)
        {
            tb_array_remove(record->entered, index, NULL);
            break;
        }
    }
    if (record->configured && tb_wl_window_scale(window) != record->scale)
    {
        tb_wl_present(window);
    }
}

static void tb_wl_surface_preferred_scale(void *data, tb_wl_proxy *surface, int32_t scale)
{
    tb_window *const window = data;
    tb_wl_window *const record = tb_wl_window_of(window);

    (void)surface;
    record->preferred_scale = scale;
    if (record->configured && scale != record->scale)
    {
        tb_wl_present(window);
    }
}

static void tb_wl_surface_preferred_transform(void *data, tb_wl_proxy *surface, uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}

static struct
{
    void (*enter)(void *, tb_wl_proxy *, tb_wl_proxy *);
    void (*leave)(void *, tb_wl_proxy *, tb_wl_proxy *);
    void (*preferred_buffer_scale)(void *, tb_wl_proxy *, int32_t);
    void (*preferred_buffer_transform)(void *, tb_wl_proxy *, uint32_t);
} tb_wl_surface_listener = {tb_wl_surface_enter, tb_wl_surface_leave, tb_wl_surface_preferred_scale,
                            tb_wl_surface_preferred_transform};

/* The logical size the window takes: the compositor's, or its own when the
 * compositor leaves it to the client. */
static void tb_wl_choose_size(tb_window *window)
{
    tb_wl_window *const record = tb_wl_window_of(window);
    tb_window_placement placement = {0};

    if (record->configured_width > 0 && record->configured_height > 0)
    {
        record->width = (uint32_t)record->configured_width;
        record->height = (uint32_t)record->configured_height;
        return;
    }
    if (window->state.mode != TB_WINDOW_MODE_WINDOWED)
    {
        /* Fullscreen before the compositor says how large: the display's
         * size. */
        tb_display display = {0};
        tb_window_place(window, false, &placement);
        tb_window_system_display(window->system, placement.display, &display);
        record->width = display.width;
        record->height = display.height;
        return;
    }
    tb_window_place(window, false, &placement);
    record->width = placement.width;
    record->height = placement.height;
}

static void tb_wl_xdg_surface_configure(void *data, tb_wl_proxy *xdg_surface, uint32_t serial)
{
    tb_window *const window = data;
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    (void)native->wl.marshal_flags(xdg_surface, TB_XDG_SURFACE_ACK_CONFIGURE, NULL,
                                   native->wl.get_version(xdg_surface), 0, serial);
    tb_wl_choose_size(window);
    record->configured = true;
    tb_wl_present(window);
}

static struct
{
    void (*configure)(void *, tb_wl_proxy *, uint32_t);
} tb_wl_xdg_surface_listener = {tb_wl_xdg_surface_configure};

static void tb_wl_toplevel_configure(void *data, tb_wl_proxy *toplevel, int32_t width,
                                     int32_t height, tb_wl_array *states)
{
    tb_window *const window = data;
    tb_wl_window *const record = tb_wl_window_of(window);

    (void)toplevel;
    (void)states;
    record->configured_width = width;
    record->configured_height = height;
}

static void tb_wl_toplevel_close(void *data, tb_wl_proxy *toplevel)
{
    tb_window *const window = data;
    const tb_window_event event = {
        .type = TB_WINDOW_EVENT_CLOSE_REQUESTED, .window = window, .state = window->state};

    (void)toplevel;
    tb_window_push(window->system, &event);
}

static struct
{
    void (*configure)(void *, tb_wl_proxy *, int32_t, int32_t, tb_wl_array *);
    void (*close)(void *, tb_wl_proxy *);
} tb_wl_toplevel_listener = {tb_wl_toplevel_configure, tb_wl_toplevel_close};

static void tb_wl_set_title(tb_window *window)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_SET_TITLE, NULL,
                                   native->wl.get_version(record->toplevel), 0,
                                   window->options.title);
}

/* Ask for the window's mode: fullscreen is borderless, on the output of its
 * display. A windowed window's size is the client's, so it is shown at once
 * when it changes. */
static void tb_wl_apply(tb_window *window, bool created)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);
    const uint32_t version = native->wl.get_version(record->toplevel);
    tb_window_placement placement = {0};
    const bool was_windowed = window->state.mode == TB_WINDOW_MODE_WINDOWED;

    tb_window_place(window, false, &placement);
    window->state.mode = window->options.mode == TB_WINDOW_MODE_WINDOWED
                             ? TB_WINDOW_MODE_WINDOWED
                             : TB_WINDOW_MODE_BORDERLESS;
    if (window->state.mode == TB_WINDOW_MODE_WINDOWED)
    {
        const int32_t fixed_width = window->options.fixed_size ? (int32_t)placement.width : 0;
        const int32_t fixed_height = window->options.fixed_size ? (int32_t)placement.height : 0;
        (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_SET_MIN_SIZE, NULL,
                                       version, 0, fixed_width, fixed_height);
        (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_SET_MAX_SIZE, NULL,
                                       version, 0, fixed_width, fixed_height);
        if (!created && !was_windowed)
        {
            (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_UNSET_FULLSCREEN, NULL,
                                           version, 0);
        }
        /* The compositor's last configure was for the old size. */
        record->configured_width = 0;
        record->configured_height = 0;
        if (record->configured)
        {
            record->width = placement.width;
            record->height = placement.height;
            tb_wl_present(window);
        }
        return;
    }
    {
        tb_wl_output *const output =
            placement.display < tb_array_length(native->outputs)
                ? *TB_ARRAY_AT(native->outputs, tb_wl_output *, placement.display)
                : NULL;
        (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_SET_FULLSCREEN, NULL,
                                       version, 0, output != NULL ? output->proxy : NULL);
    }
}

static void tb_wl_request_decoration(tb_window *window)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    if (native->decoration_manager == NULL)
    {
        return;
    }
    record->decoration = native->wl.marshal_flags(
        native->decoration_manager, TB_XDG_DECORATION_MANAGER_GET,
        &tb_xdg_toplevel_decoration_interface, 1, 0, NULL, record->toplevel);
    (void)native->wl.marshal_flags(record->decoration, TB_XDG_TOPLEVEL_DECORATION_SET_MODE, NULL, 1,
                                   0, TB_XDG_DECORATION_SERVER_SIDE);
}

static tb_window_result tb_wl_create(tb_window *window, tb_window_error *error)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_calloc(1, sizeof(tb_wl_window));
    const uint64_t attempts = 8;

    window->native = record;
    record->buffers = TB_ARRAY_CREATE(NULL, tb_wl_proxy *);
    record->entered = TB_ARRAY_CREATE(NULL, tb_wl_output *);
    record->scale = 1;
    window->state.mode = TB_WINDOW_MODE_WINDOWED;
    record->surface = native->wl.marshal_flags(native->compositor, TB_WL_COMPOSITOR_CREATE_SURFACE,
                                               native->core.surface,
                                               native->wl.get_version(native->compositor), 0, NULL);
    (void)native->wl.add_listener(record->surface, (tb_wl_handler *)&tb_wl_surface_listener,
                                  window);
    record->xdg_surface = native->wl.marshal_flags(
        native->wm_base, TB_XDG_WM_BASE_GET_XDG_SURFACE, &tb_xdg_surface_interface,
        native->wl.get_version(native->wm_base), 0, NULL, record->surface);
    (void)native->wl.add_listener(record->xdg_surface, (tb_wl_handler *)&tb_wl_xdg_surface_listener,
                                  window);
    record->toplevel = native->wl.marshal_flags(
        record->xdg_surface, TB_XDG_SURFACE_GET_TOPLEVEL, &tb_xdg_toplevel_interface,
        native->wl.get_version(record->xdg_surface), 0, NULL);
    (void)native->wl.add_listener(record->toplevel, (tb_wl_handler *)&tb_wl_toplevel_listener,
                                  window);
    tb_wl_set_title(window);
    tb_wl_request_decoration(window);
    tb_wl_apply(window, true);
    (void)native->wl.marshal_flags(record->surface, TB_WL_SURFACE_COMMIT, NULL,
                                   native->wl.get_version(record->surface), 0);
    /* The window is not there until the compositor configures it. */
    for (uint64_t attempt = 0; attempt < attempts && !record->configured; attempt++)
    {
        if (native->wl.roundtrip(native->display) < 0)
        {
            /* coverage-off: only a compositor that went away fails a roundtrip */
            break;
            /* coverage-on */
        }
    }
    if (!record->configured)
    {
        /* coverage-off: a compositor configures every new toplevel */
        return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR,
                              "the Wayland compositor did not configure the window");
        /* coverage-on */
    }
    return TB_WINDOW_OK;
}

static void tb_wl_destroy(tb_window *window)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    if (native->keyboard_focus == window)
    {
        native->keyboard_focus = NULL;
    }
    if (record->decoration != NULL)
    {
        (void)native->wl.marshal_flags(record->decoration, TB_XDG_TOPLEVEL_DECORATION_DESTROY, NULL,
                                       1, TB_WL_MARSHAL_DESTROY);
    }
    if (record->toplevel != NULL)
    {
        (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_DESTROY, NULL,
                                       native->wl.get_version(record->toplevel),
                                       TB_WL_MARSHAL_DESTROY);
    }
    if (record->xdg_surface != NULL)
    {
        (void)native->wl.marshal_flags(record->xdg_surface, TB_XDG_SURFACE_DESTROY, NULL,
                                       native->wl.get_version(record->xdg_surface),
                                       TB_WL_MARSHAL_DESTROY);
    }
    (void)native->wl.marshal_flags(record->surface, TB_WL_SURFACE_DESTROY, NULL,
                                   native->wl.get_version(record->surface), TB_WL_MARSHAL_DESTROY);
    for (size_t index = 0; index < tb_array_length(record->buffers); index++)
    {
        tb_wl_proxy *const buffer = *TB_ARRAY_AT(record->buffers, tb_wl_proxy *, index);
        (void)native->wl.marshal_flags(buffer, TB_WL_BUFFER_DESTROY, NULL,
                                       native->wl.get_version(buffer), TB_WL_MARSHAL_DESTROY);
    }
    (void)native->wl.flush(native->display);
    tb_array_destroy(record->entered);
    tb_array_destroy(record->buffers);
    tb_free(record);
}

static tb_window_result tb_wl_configure(tb_window *window, const tb_window_options *previous,
                                        tb_window_error *error)
{
    tb_wl_system *const native = tb_wl_of(window->system);

    (void)previous;
    (void)error;
    tb_wl_set_title(window);
    tb_wl_apply(window, false);
    (void)native->wl.flush(native->display);
    return TB_WINDOW_OK;
}

static void tb_wl_native(const tb_window *window, tb_window_native *native)
{
    native->system = tb_wl_of(window->system)->display;
    native->window = tb_wl_window_of(window)->surface;
}

/* ---------------------------------------------------------------------------
 * Pumping.
 * ------------------------------------------------------------------------- */

static void tb_wl_after_dispatch(tb_window_system *system)
{
    tb_wl_system *const native = tb_wl_of(system);

    if (native->displays_changed)
    {
        native->displays_changed = false;
        tb_wl_publish_displays(native);
        tb_window_displays_changed(system);
    }
    if (tb_linux_waker_drain(&native->waker))
    {
        tb_window_woken(system);
    }
}

static void tb_wl_pump(tb_window_system *system, uint64_t nanoseconds)
{
    tb_wl_system *const native = tb_wl_of(system);
    const size_t queued = tb_array_length(system->events);
    unsigned ready = 0;

    while (native->wl.prepare_read(native->display) != 0)
    {
        (void)native->wl.dispatch_pending(native->display);
    }
    (void)native->wl.flush(native->display);
    /* What the dispatch above queued is enough; otherwise wait. */
    ready = tb_linux_wait(native->wl.get_fd(native->display), native->waker.read_end,
                          tb_array_length(system->events) > queued ? 0 : nanoseconds);
    if ((ready & 1u) != 0)
    {
        (void)native->wl.read_events(native->display);
    }
    else
    {
        native->wl.cancel_read(native->display);
    }
    (void)native->wl.dispatch_pending(native->display);
    tb_wl_after_dispatch(system);
}

static void tb_wl_wake(tb_window_system *system)
{
    tb_linux_waker_wake(&tb_wl_of(system)->waker);
}

static void tb_wl_destroy_global(tb_wl_system *native, tb_wl_proxy **proxy, uint32_t opcode)
{
    if (*proxy == NULL)
    {
        return;
    }
    (void)native->wl.marshal_flags(*proxy, opcode, NULL, native->wl.get_version(*proxy),
                                   TB_WL_MARSHAL_DESTROY);
    *proxy = NULL;
}

static void tb_wl_release(tb_wl_system *native)
{
    if (native->display != NULL)
    {
        tb_wl_release_keyboard(native);
        if (native->seat != NULL)
        {
            if (native->wl.get_version(native->seat) >= 5)
            {
                tb_wl_destroy_global(native, &native->seat, TB_WL_SEAT_RELEASE);
            }
            else
            {
                /* coverage-off: every current compositor has version 5 */
                native->wl.destroy(native->seat);
                /* coverage-on */
            }
        }
        for (size_t index = 0; index < tb_array_length(native->outputs); index++)
        {
            tb_wl_release_output(native, *TB_ARRAY_AT(native->outputs, tb_wl_output *, index));
        }
        tb_wl_destroy_global(native, &native->decoration_manager,
                             TB_XDG_DECORATION_MANAGER_DESTROY);
        tb_wl_destroy_global(native, &native->wm_base, TB_XDG_WM_BASE_DESTROY);
        /* wl_compositor, wl_shm (version 1) and wl_registry have no
         * destructor request. */
        if (native->compositor != NULL)
        {
            native->wl.destroy(native->compositor);
        }
        if (native->shm != NULL)
        {
            native->wl.destroy(native->shm);
        }
        if (native->registry != NULL)
        {
            native->wl.destroy(native->registry);
        }
        native->wl.disconnect(native->display);
    }
    if (native->waker.read_end >= 0)
    {
        tb_linux_waker_close(&native->waker);
    }
    tb_array_destroy(native->outputs);
    tb_library_close(native->library);
    tb_free(native);
}

static void tb_wl_close(tb_window_system *system)
{
    tb_wl_release(tb_wl_of(system));
}

static const tb_window_ops tb_wl_ops = {
    .close = tb_wl_close,
    .pump = tb_wl_pump,
    .wake = tb_wl_wake,
    .create = tb_wl_create,
    .destroy = tb_wl_destroy,
    .configure = tb_wl_configure,
    .native = tb_wl_native,
};

/* ---------------------------------------------------------------------------
 * Opening.
 * ------------------------------------------------------------------------- */

static tb_window_result tb_wl_load(tb_wl_system *native, tb_window_error *error)
{
    static const char *const functions[] = {
        "wl_display_connect",          "wl_display_disconnect",  "wl_display_get_fd",
        "wl_display_dispatch_pending", "wl_display_flush",       "wl_display_roundtrip",
        "wl_display_prepare_read",     "wl_display_read_events", "wl_display_cancel_read",
        "wl_proxy_marshal_flags",      "wl_proxy_add_listener",  "wl_proxy_destroy",
        "wl_proxy_get_version",
    };
    static const char *const interfaces[] = {
        "wl_registry_interface", "wl_compositor_interface", "wl_surface_interface",
        "wl_seat_interface",     "wl_keyboard_interface",   "wl_output_interface",
        "wl_shm_interface",      "wl_shm_pool_interface",   "wl_buffer_interface",
    };
    enum
    {
        FUNCTIONS = sizeof(functions) / sizeof(functions[0]),
        INTERFACES = sizeof(interfaces) / sizeof(interfaces[0])
    };
    tb_library_function found[FUNCTIONS] = {0};
    tb_library_binding bindings[FUNCTIONS] = {0};
    const tb_wl_interface *core[INTERFACES] = {NULL};
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;

    TB_STATIC_ASSERT(sizeof(tb_wl_api) == FUNCTIONS * sizeof(tb_library_function),
                     "the libwayland table has a function for each name");
    TB_STATIC_ASSERT(sizeof(tb_wl_core) == INTERFACES * sizeof(const tb_wl_interface *),
                     "the core table has an interface for each name");
    if (tb_library_open("libwayland-client.so.0", TB_LIBRARY_SEARCH_SYSTEM, &native->library,
                        &library_error) != TB_LIBRARY_OK)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "no Wayland: %s",
                              library_error.message);
    }
    for (size_t index = 0; index < FUNCTIONS; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = functions[index], .function = &found[index], .required = true};
    }
    if (tb_library_bind(native->library, bindings, FUNCTIONS, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every libwayland-client since 1.20 exports these */
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s", library_error.message);
        /* coverage-on */
    }
    for (size_t index = 0; index < INTERFACES; index++)
    {
        void *data = NULL;
        if (tb_library_find_data(native->library, interfaces[index], &data, &library_error) !=
            TB_LIBRARY_OK)
        {
            /* coverage-off: libwayland-client exports every core interface */
            return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s", library_error.message);
            /* coverage-on */
        }
        core[index] = data;
    }
    memcpy(&native->wl, found, sizeof(native->wl));
    memcpy(&native->core, core, sizeof(native->core));
    /* The xdg tables name two core interfaces. */
    tb_xdg_get_surface_types[1] = native->core.surface;
    tb_xdg_seat_types[0] = native->core.seat;
    tb_xdg_output_types[0] = native->core.output;
    return TB_WINDOW_OK;
}

/* What the compositor must offer for a window: without it, Wayland is not
 * usable here, which falls back to X11. */
static const char *tb_wl_missing(const tb_wl_system *native)
{
    if (native->compositor == NULL)
    {
        return "wl_compositor";
    }
    if (native->shm == NULL)
    {
        return "wl_shm";
    }
    if (native->wm_base == NULL)
    {
        return "xdg_wm_base";
    }
    return NULL;
}

tb_window_result tb_wayland_open(tb_window_system *system, tb_window_error *error)
{
    tb_wl_system *const native = tb_calloc(1, sizeof(tb_wl_system));
    tb_window_result result = TB_WINDOW_OK;
    const char *missing = NULL;

    native->waker = (tb_linux_waker){-1, -1};
    native->outputs = TB_ARRAY_CREATE(NULL, tb_wl_output *);
    native->system = system;
    result = tb_wl_load(native, error);
    if (result != TB_WINDOW_OK)
    {
        tb_wl_release(native);
        return result;
    }
    native->display = native->wl.connect(NULL);
    if (native->display == NULL)
    {
        tb_wl_release(native);
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE,
                              "no Wayland compositor: WAYLAND_DISPLAY names none that answers");
    }
    native->registry =
        native->wl.marshal_flags((tb_wl_proxy *)native->display, TB_WL_DISPLAY_GET_REGISTRY,
                                 native->core.registry, 1, 0, NULL);
    (void)native->wl.add_listener(native->registry, (tb_wl_handler *)&tb_wl_registry_listener,
                                  native);
    /* The globals, then what each bound one says about itself. */
    (void)native->wl.roundtrip(native->display);
    (void)native->wl.roundtrip(native->display);
    missing = tb_wl_missing(native);
    if (missing != NULL)
    {
        tb_wl_release(native);
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "the Wayland compositor offers no %s",
                              missing);
    }
    (void)native->wl.add_listener(native->wm_base, (tb_wl_handler *)&tb_wl_wm_base_listener,
                                  native);
    if (!tb_linux_waker_open(&native->waker))
    {
        /* coverage-off: only a process out of descriptors has no pipe */
        tb_wl_release(native);
        return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR, "no pipe to wake the window system");
        /* coverage-on */
    }
    system->backend = TB_WINDOW_BACKEND_WAYLAND;
    system->ops = &tb_wl_ops;
    system->native = native;
    native->displays_changed = false;
    tb_wl_publish_displays(native);
    return TB_WINDOW_OK;
}
