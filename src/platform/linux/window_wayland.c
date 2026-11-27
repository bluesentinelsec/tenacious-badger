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
 * GNOME does not; and a window keeps no aspect while the player resizes it,
 * for xdg-shell has no hint of one. Scale is the integer scale of the
 * outputs a surface is on.
 *
 * The mouse's cursor is the compositor's shape where it offers
 * cursor-shape-v1, else the cursor theme's image from libwayland-cursor.so.0.
 * Relative mouse mode locks the pointer through pointer-constraints and
 * reads relative-pointer's unaccelerated motion; a compositor without them
 * has no such mode. Text is the keymap's, through libxkbcommon.so.0, with
 * dead keys composed by the locale's compose sequences.
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
#include <stdlib.h>
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

typedef int32_t tb_wl_fixed; /* 24.8 fixed point. */

#define TB_WL_MARSHAL_DESTROY 1u
#define TB_WL_SHM_FORMAT_XRGB8888 1u
#define TB_WL_SEAT_POINTER 1u
#define TB_WL_SEAT_KEYBOARD 2u
#define TB_WL_OUTPUT_MODE_CURRENT 1u
#define TB_WL_KEY_PRESSED 1u
#define TB_WL_BUTTON_PRESSED 1u
#define TB_WL_AXIS_VERTICAL 0u
#define TB_XDG_DECORATION_SERVER_SIDE 2u
#define TB_WL_KEYMAP_XKB_V1 1u
/* A locked pointer stays locked, through focus changes, until destroyed. */
#define TB_WL_LOCK_PERSISTENT 2u
/* The surface-local distance of one notch of a wheel without value120,
 * which compositors report as 10 (libinput's default). */
#define TB_WL_AXIS_PER_NOTCH 10.0
/* The size of a theme's cursor, in logical units. */
#define TB_WL_CURSOR_SIZE 24

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
    const tb_wl_interface *pointer;
    const tb_wl_interface *region;
} tb_wl_core;

/* libwayland-cursor.so.0, for the cursor theme's images where the
 * compositor has no cursor-shape protocol. */
typedef struct tb_wl_cursor_image
{
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint32_t delay;
} tb_wl_cursor_image;

typedef struct tb_wl_cursor
{
    unsigned int image_count;
    tb_wl_cursor_image **images;
    char *name;
} tb_wl_cursor;

typedef struct tb_wl_cursor_theme tb_wl_cursor_theme;

typedef struct tb_wl_cursor_api
{
    tb_wl_cursor_theme *(*theme_load)(const char *name, int size, tb_wl_proxy *shm);
    void (*theme_destroy)(tb_wl_cursor_theme *theme);
    tb_wl_cursor *(*theme_get_cursor)(tb_wl_cursor_theme *theme, const char *name);
    tb_wl_proxy *(*image_get_buffer)(tb_wl_cursor_image *image);
} tb_wl_cursor_api;

/* libxkbcommon.so.0, which turns keys into text by the keymap the
 * compositor sends, and composes dead keys by the user's locale. */
typedef struct tb_xkb_context tb_xkb_context;
typedef struct tb_xkb_keymap tb_xkb_keymap;
typedef struct tb_xkb_state tb_xkb_state;
typedef struct tb_xkb_compose_table tb_xkb_compose_table;
typedef struct tb_xkb_compose_state tb_xkb_compose_state;

typedef struct tb_xkb_api
{
    tb_xkb_context *(*context_new)(int flags);
    void (*context_unref)(tb_xkb_context *context);
    tb_xkb_keymap *(*keymap_new_from_string)(tb_xkb_context *context, const char *text, int format,
                                             int flags);
    void (*keymap_unref)(tb_xkb_keymap *keymap);
    tb_xkb_state *(*state_new)(tb_xkb_keymap *keymap);
    void (*state_unref)(tb_xkb_state *state);
    int (*state_update_mask)(tb_xkb_state *state, uint32_t depressed, uint32_t latched,
                             uint32_t locked, uint32_t depressed_layout, uint32_t latched_layout,
                             uint32_t locked_layout);
    uint32_t (*state_key_get_one_sym)(tb_xkb_state *state, uint32_t key);
    int (*state_key_get_utf8)(tb_xkb_state *state, uint32_t key, char *buffer, size_t size);
    int (*state_mod_name_is_active)(tb_xkb_state *state, const char *name, int type);
    tb_xkb_compose_table *(*compose_table_new_from_locale)(tb_xkb_context *context,
                                                           const char *locale, int flags);
    void (*compose_table_unref)(tb_xkb_compose_table *table);
    tb_xkb_compose_state *(*compose_state_new)(tb_xkb_compose_table *table, int flags);
    void (*compose_state_unref)(tb_xkb_compose_state *state);
    int (*compose_state_feed)(tb_xkb_compose_state *state, uint32_t keysym);
    void (*compose_state_reset)(tb_xkb_compose_state *state);
    int (*compose_state_get_status)(tb_xkb_compose_state *state);
    int (*compose_state_get_utf8)(tb_xkb_compose_state *state, char *buffer, size_t size);
} tb_xkb_api;

/* xkb_state_component's effective modifiers, and xkb_compose_status. */
#define TB_XKB_MODS_EFFECTIVE (1 << 3)
#define TB_XKB_COMPOSE_COMPOSING 1
#define TB_XKB_COMPOSE_COMPOSED 2
#define TB_XKB_COMPOSE_CANCELLED 3

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

/* cursor-shape-v1, relative-pointer-unstable-v1 and
 * pointer-constraints-unstable-v1, from their XML. */
extern const tb_wl_interface tb_cursor_shape_manager_interface;
extern const tb_wl_interface tb_cursor_shape_device_interface;
extern const tb_wl_interface tb_relative_manager_interface;
extern const tb_wl_interface tb_relative_pointer_interface;
extern const tb_wl_interface tb_pointer_constraints_interface;
extern const tb_wl_interface tb_locked_pointer_interface;

static const tb_wl_interface *tb_cursor_shape_get_types[2] = {&tb_cursor_shape_device_interface,
                                                              NULL};
static const tb_wl_interface *tb_relative_get_types[2] = {&tb_relative_pointer_interface, NULL};
static const tb_wl_interface *tb_lock_pointer_types[5] = {&tb_locked_pointer_interface, NULL, NULL,
                                                          NULL, NULL};
static const tb_wl_interface *tb_confine_pointer_types[5] = {NULL, NULL, NULL, NULL, NULL};
static const tb_wl_interface *tb_locked_region_types[1] = {NULL};

static const tb_wl_message tb_cursor_shape_manager_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"get_pointer", "no", tb_cursor_shape_get_types},
    {"get_tablet_tool_v2", "no", tb_wl_no_types},
};
const tb_wl_interface tb_cursor_shape_manager_interface = {
    "wp_cursor_shape_manager_v1", 1, 3, tb_cursor_shape_manager_requests, 0, NULL};

static const tb_wl_message tb_cursor_shape_device_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"set_shape", "uu", tb_wl_no_types},
};
const tb_wl_interface tb_cursor_shape_device_interface = {"wp_cursor_shape_device_v1",     1, 2,
                                                          tb_cursor_shape_device_requests, 0, NULL};

static const tb_wl_message tb_relative_manager_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"get_relative_pointer", "no", tb_relative_get_types},
};
const tb_wl_interface tb_relative_manager_interface = {"zwp_relative_pointer_manager_v1", 1, 2,
                                                       tb_relative_manager_requests,      0, NULL};

static const tb_wl_message tb_relative_pointer_requests[] = {{"destroy", "", tb_wl_no_types}};
static const tb_wl_message tb_relative_pointer_events[] = {
    {"relative_motion", "uuffff", tb_wl_no_types}};
const tb_wl_interface tb_relative_pointer_interface = {
    "zwp_relative_pointer_v1", 1, 1, tb_relative_pointer_requests, 1, tb_relative_pointer_events};

static const tb_wl_message tb_pointer_constraints_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"lock_pointer", "noo?ou", tb_lock_pointer_types},
    {"confine_pointer", "noo?ou", tb_confine_pointer_types},
};
const tb_wl_interface tb_pointer_constraints_interface = {"zwp_pointer_constraints_v1",    1, 3,
                                                          tb_pointer_constraints_requests, 0, NULL};

static const tb_wl_message tb_locked_pointer_requests[] = {
    {"destroy", "", tb_wl_no_types},
    {"set_cursor_position_hint", "ff", tb_wl_no_types},
    {"set_region", "?o", tb_locked_region_types},
};
static const tb_wl_message tb_locked_pointer_events[] = {
    {"locked", "", tb_wl_no_types},
    {"unlocked", "", tb_wl_no_types},
};
const tb_wl_interface tb_locked_pointer_interface = {
    "zwp_locked_pointer_v1", 1, 3, tb_locked_pointer_requests, 2, tb_locked_pointer_events};

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
    TB_XDG_TOPLEVEL_DECORATION_SET_MODE = 1,
    TB_WL_SEAT_GET_POINTER = 0,
    TB_WL_POINTER_SET_CURSOR = 0,
    TB_WL_POINTER_RELEASE = 1,
    TB_CURSOR_SHAPE_MANAGER_DESTROY = 0,
    TB_CURSOR_SHAPE_MANAGER_GET_POINTER = 1,
    TB_CURSOR_SHAPE_DEVICE_DESTROY = 0,
    TB_CURSOR_SHAPE_DEVICE_SET_SHAPE = 1,
    TB_RELATIVE_MANAGER_DESTROY = 0,
    TB_RELATIVE_MANAGER_GET = 1,
    TB_RELATIVE_POINTER_DESTROY = 0,
    TB_POINTER_CONSTRAINTS_DESTROY = 0,
    TB_POINTER_CONSTRAINTS_LOCK = 1,
    TB_LOCKED_POINTER_DESTROY = 0
};

/* The highest versions the backend knows the events of. A seat of version
 * 8 sends a wheel's turn in 120ths of a notch. */
#define TB_WL_COMPOSITOR_VERSION 6u
#define TB_WL_OUTPUT_VERSION 4u
#define TB_WL_SEAT_VERSION 8u
#define TB_WL_SHM_VERSION 1u

/* xdg_toplevel.state values in the version of xdg-shell we bind. */
enum
{
    TB_XDG_STATE_MAXIMIZED = 1,
    TB_XDG_STATE_FULLSCREEN = 2,
    TB_XDG_STATE_RESIZING = 3
};

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
    /* The mouse. */
    tb_wl_proxy *pointer;
    tb_wl_proxy *cursor_shape_manager; /* NULL without cursor-shape-v1. */
    tb_wl_proxy *cursor_shape_device;
    tb_wl_proxy *relative_manager;    /* NULL without relative-pointer. */
    tb_wl_proxy *pointer_constraints; /* NULL without pointer-constraints. */
    tb_wl_proxy *relative_pointer;
    tb_window *pointer_focus;
    uint32_t pointer_serial; /* Of the pointer's last entering, to set the cursor with. */
    double pointer_x;        /* Where it is over the focus, in logical units. */
    double pointer_y;
    double wheel_x; /* The wheel's turn in notches, gathered until a frame ends. */
    double wheel_y;
    bool wheel_in_120ths; /* The frame's turn came in 120ths, not as a distance. */
    tb_library *cursor_library;
    tb_wl_cursor_api cursor;
    tb_wl_cursor_theme *cursor_theme;
    tb_wl_proxy *cursor_surface;
    /* The keyboard's text. */
    tb_library *xkb_library;
    tb_xkb_api xkb;
    tb_xkb_context *xkb_context;
    tb_xkb_keymap *keymap;
    tb_xkb_state *xkb_state;
    tb_xkb_compose_table *compose_table;
    tb_xkb_compose_state *compose_state;
    uint32_t modifiers;
};

typedef enum tb_wl_size_policy
{
    TB_WL_FLOATING,
    TB_WL_FULLSCREEN,
    TB_WL_COMPOSITOR_SIZED /* Maximized or being resized by the player. */
} tb_wl_size_policy;

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
    tb_wl_size_policy pending_policy; /* Applied together at xdg_surface.configure. */
    tb_wl_size_policy policy;
    bool restore_requested;   /* The client asked to leave fullscreen at options.width/height. */
    uint32_t last_hint_width; /* Last configure, resolved to our size when it supplied none. */
    uint32_t last_hint_height;
    uint32_t width; /* The logical size shown. */
    uint32_t height;
    int32_t scale;
    int32_t preferred_scale; /* From wl_surface.preferred_buffer_scale; 0 before one. */
    bool configured;
    tb_wl_proxy *locked_pointer; /* In relative mouse mode. */
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

/* The protocols of the mouse that a compositor may offer: cursor shapes,
 * and relative motion with the pointer locked. */
static void tb_wl_bind_mouse_global(tb_wl_system *native, uint32_t name, const char *interface)
{
    if (tb_str_equal(interface, "wp_cursor_shape_manager_v1"))
    {
        native->cursor_shape_manager =
            tb_wl_bind(native, name, &tb_cursor_shape_manager_interface, 1);
    }
    else if (tb_str_equal(interface, "zwp_relative_pointer_manager_v1"))
    {
        native->relative_manager = tb_wl_bind(native, name, &tb_relative_manager_interface, 1);
    }
    else if (tb_str_equal(interface, "zwp_pointer_constraints_v1"))
    {
        native->pointer_constraints =
            tb_wl_bind(native, name, &tb_pointer_constraints_interface, 1);
    }
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
    else
    {
        tb_wl_bind_mouse_global(native, name, interface);
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
 * The seat: the keyboard and the mouse.
 * ------------------------------------------------------------------------- */

/* coverage-off: the headless compositor that CI tests under has no seat, so no keyboard or mouse;
 * keys, buttons, and text are tested through X11, Win32, and Cocoa, and the translations here
 * through the functions they share */
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

static void tb_wl_forget_keymap(tb_wl_system *native)
{
    if (native->compose_state != NULL)
    {
        native->xkb.compose_state_unref(native->compose_state);
        native->compose_state = NULL;
    }
    if (native->xkb_state != NULL)
    {
        native->xkb.state_unref(native->xkb_state);
        native->xkb_state = NULL;
    }
    if (native->keymap != NULL)
    {
        native->xkb.keymap_unref(native->keymap);
        native->keymap = NULL;
    }
}

/* The keymap's text, read from the file the compositor shares. */
static char *tb_wl_read_keymap(int32_t fd, uint32_t size)
{
    char *const text = tb_calloc((size_t)size + 1u, 1);
    size_t done = 0;

    while (done < size)
    {
        const ssize_t got = read(fd, text + done, size - done);
        if (got <= 0)
        {
            break;
        }
        done += (size_t)got;
    }
    text[done] = '\0';
    return text;
}

/* The keymap turns keys into text; without xkbcommon, keys have none. */
static void tb_wl_keymap(void *data, tb_wl_proxy *keyboard, uint32_t format, int32_t fd,
                         uint32_t size)
{
    tb_wl_system *const native = data;
    char *text = NULL;

    (void)keyboard;
    if (native->xkb_context == NULL || format != TB_WL_KEYMAP_XKB_V1)
    {
        (void)close(fd);
        return;
    }
    text = tb_wl_read_keymap(fd, size);
    (void)close(fd);
    tb_wl_forget_keymap(native);
    native->keymap =
        native->xkb.keymap_new_from_string(native->xkb_context, text, TB_WL_KEYMAP_XKB_V1, 0);
    tb_free(text);
    if (native->keymap == NULL)
    {
        return;
    }
    native->xkb_state = native->xkb.state_new(native->keymap);
    if (native->compose_table != NULL)
    {
        native->compose_state = native->xkb.compose_state_new(native->compose_table, 0);
    }
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
    if (native->compose_state != NULL)
    {
        native->xkb.compose_state_reset(native->compose_state);
    }
}

/* The text a key types, by the keymap, with a dead key composed with the
 * key after it. */
static void tb_wl_key_text(tb_wl_system *native, tb_window *window, uint32_t key)
{
    /* xkbcommon's keycodes are evdev's, 8 higher, as X's are. */
    const uint32_t code = key + 8u;
    char text[64] = {0};
    int length = 0;

    if (native->xkb_state == NULL)
    {
        return;
    }
    if (native->compose_state != NULL &&
        native->xkb.compose_state_feed(
            native->compose_state, native->xkb.state_key_get_one_sym(native->xkb_state, code)) != 0)
    {
        const int status = native->xkb.compose_state_get_status(native->compose_state);
        if (status == TB_XKB_COMPOSE_COMPOSING)
        {
            return;
        }
        if (status == TB_XKB_COMPOSE_CANCELLED)
        {
            native->xkb.compose_state_reset(native->compose_state);
            return;
        }
        if (status == TB_XKB_COMPOSE_COMPOSED)
        {
            length = native->xkb.compose_state_get_utf8(native->compose_state, text, sizeof(text));
            native->xkb.compose_state_reset(native->compose_state);
            tb_window_text(window, text, length > 0 ? (size_t)length : 0u);
            return;
        }
    }
    length = native->xkb.state_key_get_utf8(native->xkb_state, code, text, sizeof(text));
    if (length > 0 && (size_t)length < sizeof(text))
    {
        tb_window_text(window, text, (size_t)length);
    }
}

static void tb_wl_key(void *data, tb_wl_proxy *keyboard, uint32_t serial, uint32_t time,
                      uint32_t key, uint32_t state)
{
    tb_wl_system *const native = data;
    const bool down = state == TB_WL_KEY_PRESSED;

    (void)keyboard;
    (void)serial;
    (void)time;
    if (native->keyboard_focus == NULL)
    {
        return;
    }
    /* A compositor sends no repeats: a client repeats keys itself. */
    tb_window_key(native->keyboard_focus, tb_linux_key(key), key, down, false, native->modifiers);
    if (down)
    {
        tb_wl_key_text(native, native->keyboard_focus, key);
    }
}

static void tb_wl_modifiers(void *data, tb_wl_proxy *keyboard, uint32_t serial, uint32_t depressed,
                            uint32_t latched, uint32_t locked, uint32_t group)
{
    tb_wl_system *const native = data;
    static const struct
    {
        const char *name;
        uint32_t modifier;
    } names[] = {{"Shift", TB_MODIFIER_SHIFT},
                 {"Control", TB_MODIFIER_CONTROL},
                 {"Mod1", TB_MODIFIER_ALT},
                 {"Mod4", TB_MODIFIER_SUPER}};

    (void)keyboard;
    (void)serial;
    native->modifiers = 0;
    if (native->xkb_state == NULL)
    {
        return;
    }
    (void)native->xkb.state_update_mask(native->xkb_state, depressed, latched, locked, 0, 0, group);
    for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    {
        if (native->xkb.state_mod_name_is_active(native->xkb_state, names[index].name,
                                                 TB_XKB_MODS_EFFECTIVE) > 0)
        {
            native->modifiers |= names[index].modifier;
        }
    }
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
        native->wl.destroy(native->keyboard);
    }
    native->keyboard = NULL;
    native->keyboard_focus = NULL;
}

static double tb_wl_from_fixed(tb_wl_fixed value)
{
    return (double)value / 256.0;
}

/* The names of each shape in a cursor theme: the CSS name, then the X
 * cursor font's, which older themes have. */
static const char *const tb_wl_cursor_names[TB_CURSOR_COUNT][2] = {
    [TB_CURSOR_ARROW] = {"default", "left_ptr"},
    [TB_CURSOR_TEXT] = {"text", "xterm"},
    [TB_CURSOR_HAND] = {"pointer", "hand2"},
    [TB_CURSOR_CROSSHAIR] = {"crosshair", "cross"},
    [TB_CURSOR_NOT_ALLOWED] = {"not-allowed", "crossed_circle"},
    [TB_CURSOR_RESIZE_HORIZONTAL] = {"ew-resize", "sb_h_double_arrow"},
    [TB_CURSOR_RESIZE_VERTICAL] = {"ns-resize", "sb_v_double_arrow"},
    [TB_CURSOR_RESIZE_DIAGONAL_DOWN] = {"nwse-resize", "bottom_right_corner"},
    [TB_CURSOR_RESIZE_DIAGONAL_UP] = {"nesw-resize", "bottom_left_corner"},
    [TB_CURSOR_RESIZE_ALL] = {"move", "fleur"},
};

/* wp_cursor_shape_device_v1's shapes. */
static const uint32_t tb_wl_cursor_shapes[TB_CURSOR_COUNT] = {
    [TB_CURSOR_ARROW] = 1,                 /* default */
    [TB_CURSOR_TEXT] = 9,                  /* text */
    [TB_CURSOR_HAND] = 4,                  /* pointer */
    [TB_CURSOR_CROSSHAIR] = 8,             /* crosshair */
    [TB_CURSOR_NOT_ALLOWED] = 15,          /* not_allowed */
    [TB_CURSOR_RESIZE_HORIZONTAL] = 26,    /* ew_resize */
    [TB_CURSOR_RESIZE_VERTICAL] = 27,      /* ns_resize */
    [TB_CURSOR_RESIZE_DIAGONAL_DOWN] = 29, /* nwse_resize */
    [TB_CURSOR_RESIZE_DIAGONAL_UP] = 28,   /* nesw_resize */
    [TB_CURSOR_RESIZE_ALL] = 13,           /* move */
};

/* Set the pointer's cursor to nothing. */
static void tb_wl_hide_cursor(tb_wl_system *native)
{
    (void)native->wl.marshal_flags(native->pointer, TB_WL_POINTER_SET_CURSOR, NULL,
                                   native->wl.get_version(native->pointer), 0,
                                   native->pointer_serial, NULL, 0, 0);
}

/* Draw a cursor of the theme's: an image of a surface of the client's. */
static void tb_wl_draw_theme_cursor(tb_wl_system *native, tb_cursor shape)
{
    tb_wl_cursor *cursor = NULL;
    tb_wl_cursor_image *image = NULL;
    tb_wl_proxy *buffer = NULL;
    uint32_t version = 0;

    if (native->cursor_theme == NULL)
    {
        return;
    }
    cursor = native->cursor.theme_get_cursor(native->cursor_theme, tb_wl_cursor_names[shape][0]);
    if (cursor == NULL)
    {
        cursor =
            native->cursor.theme_get_cursor(native->cursor_theme, tb_wl_cursor_names[shape][1]);
    }
    if (cursor == NULL || cursor->image_count == 0)
    {
        return;
    }
    image = cursor->images[0];
    buffer = native->cursor.image_get_buffer(image);
    version = native->wl.get_version(native->cursor_surface);
    (void)native->wl.marshal_flags(native->cursor_surface, TB_WL_SURFACE_ATTACH, NULL, version, 0,
                                   buffer, 0, 0);
    (void)native->wl.marshal_flags(native->cursor_surface, TB_WL_SURFACE_DAMAGE, NULL, version, 0,
                                   0, 0, (int32_t)image->width, (int32_t)image->height);
    (void)native->wl.marshal_flags(native->cursor_surface, TB_WL_SURFACE_COMMIT, NULL, version, 0);
    (void)native->wl.marshal_flags(native->pointer, TB_WL_POINTER_SET_CURSOR, NULL,
                                   native->wl.get_version(native->pointer), 0,
                                   native->pointer_serial, native->cursor_surface,
                                   (int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
}

/* Show the cursor the window under the pointer asks for. */
static void tb_wl_apply_cursor(tb_wl_system *native, const tb_window *window)
{
    if (native->pointer == NULL || native->pointer_focus != window)
    {
        return;
    }
    if (window->cursor_hidden || window->state.relative_mouse)
    {
        tb_wl_hide_cursor(native);
    }
    else if (native->cursor_shape_device != NULL)
    {
        (void)native->wl.marshal_flags(native->cursor_shape_device,
                                       TB_CURSOR_SHAPE_DEVICE_SET_SHAPE, NULL, 1, 0,
                                       native->pointer_serial, tb_wl_cursor_shapes[window->cursor]);
    }
    else
    {
        tb_wl_draw_theme_cursor(native, window->cursor);
    }
    (void)native->wl.flush(native->display);
}

static void tb_wl_pointer_enter(void *data, tb_wl_proxy *pointer, uint32_t serial,
                                tb_wl_proxy *surface, tb_wl_fixed x, tb_wl_fixed y)
{
    tb_wl_system *const native = data;

    (void)pointer;
    native->pointer_focus = tb_wl_window_of_surface(native, surface);
    native->pointer_serial = serial;
    native->pointer_x = tb_wl_from_fixed(x);
    native->pointer_y = tb_wl_from_fixed(y);
    if (native->pointer_focus == NULL)
    {
        return;
    }
    tb_wl_apply_cursor(native, native->pointer_focus);
    tb_window_mouse_crossed(native->pointer_focus, true, native->pointer_x, native->pointer_y);
}

static void tb_wl_pointer_leave(void *data, tb_wl_proxy *pointer, uint32_t serial,
                                tb_wl_proxy *surface)
{
    tb_wl_system *const native = data;
    tb_window *const window = tb_wl_window_of_surface(native, surface);

    (void)pointer;
    (void)serial;
    if (window != NULL)
    {
        tb_window_mouse_crossed(window, false, 0.0, 0.0);
    }
    native->pointer_focus = NULL;
}

static void tb_wl_pointer_motion(void *data, tb_wl_proxy *pointer, uint32_t time, tb_wl_fixed x,
                                 tb_wl_fixed y)
{
    tb_wl_system *const native = data;

    (void)pointer;
    (void)time;
    native->pointer_x = tb_wl_from_fixed(x);
    native->pointer_y = tb_wl_from_fixed(y);
    if (native->pointer_focus != NULL)
    {
        tb_window_mouse_moved(native->pointer_focus, native->pointer_x, native->pointer_y,
                              native->modifiers);
    }
}

static void tb_wl_pointer_button(void *data, tb_wl_proxy *pointer, uint32_t serial, uint32_t time,
                                 uint32_t button, uint32_t state)
{
    tb_wl_system *const native = data;
    const tb_mouse_button which = tb_linux_button(button);

    (void)pointer;
    (void)serial;
    (void)time;
    if (native->pointer_focus == NULL || which == TB_MOUSE_BUTTON_NONE)
    {
        return;
    }
    tb_window_mouse_button(native->pointer_focus, which, state == TB_WL_BUTTON_PRESSED,
                           native->pointer_x, native->pointer_y, native->modifiers);
}

/* Report the wheel's turn gathered in a frame. Wayland's vertical axis is
 * positive down, the engine's up. */
static void tb_wl_pointer_frame(void *data, tb_wl_proxy *pointer)
{
    tb_wl_system *const native = data;

    (void)pointer;
    if (native->pointer_focus != NULL && (native->wheel_x != 0.0 || native->wheel_y != 0.0))
    {
        tb_window_mouse_wheel(native->pointer_focus, native->wheel_x, -native->wheel_y,
                              native->pointer_x, native->pointer_y, native->modifiers);
    }
    native->wheel_x = 0.0;
    native->wheel_y = 0.0;
    native->wheel_in_120ths = false;
}

static void tb_wl_add_turn(tb_wl_system *native, uint32_t axis, double notches)
{
    if (axis == TB_WL_AXIS_VERTICAL)
    {
        native->wheel_y += notches;
    }
    else
    {
        native->wheel_x += notches;
    }
}

/* A distance scrolled; a wheel that also sends 120ths is counted by them. */
static void tb_wl_pointer_axis(void *data, tb_wl_proxy *pointer, uint32_t time, uint32_t axis,
                               tb_wl_fixed value)
{
    tb_wl_system *const native = data;

    (void)time;
    if (!native->wheel_in_120ths)
    {
        tb_wl_add_turn(native, axis, tb_wl_from_fixed(value) / TB_WL_AXIS_PER_NOTCH);
    }
    /* A seat before version 5 ends no frames. */
    if (native->wl.get_version(pointer) < 5)
    {
        tb_wl_pointer_frame(data, pointer);
    }
}

static void tb_wl_pointer_axis_source(void *data, tb_wl_proxy *pointer, uint32_t source)
{
    (void)data;
    (void)pointer;
    (void)source;
}

static void tb_wl_pointer_axis_stop(void *data, tb_wl_proxy *pointer, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
}

/* Whole notches, before seats of version 8. */
static void tb_wl_pointer_axis_discrete(void *data, tb_wl_proxy *pointer, uint32_t axis,
                                        int32_t discrete)
{
    tb_wl_system *const native = data;

    (void)pointer;
    if (!native->wheel_in_120ths)
    {
        native->wheel_x = 0.0;
        native->wheel_y = 0.0;
        native->wheel_in_120ths = true;
    }
    tb_wl_add_turn(native, axis, (double)discrete);
}

/* 120ths of a notch, which a high-resolution wheel sends in parts. The axis
 * events of the frame, which come after, are not counted again. */
static void tb_wl_pointer_axis_value120(void *data, tb_wl_proxy *pointer, uint32_t axis,
                                        int32_t value120)
{
    tb_wl_system *const native = data;

    (void)pointer;
    if (!native->wheel_in_120ths)
    {
        native->wheel_x = 0.0;
        native->wheel_y = 0.0;
        native->wheel_in_120ths = true;
    }
    tb_wl_add_turn(native, axis, (double)value120 / 120.0);
}

static struct
{
    void (*enter)(void *, tb_wl_proxy *, uint32_t, tb_wl_proxy *, tb_wl_fixed, tb_wl_fixed);
    void (*leave)(void *, tb_wl_proxy *, uint32_t, tb_wl_proxy *);
    void (*motion)(void *, tb_wl_proxy *, uint32_t, tb_wl_fixed, tb_wl_fixed);
    void (*button)(void *, tb_wl_proxy *, uint32_t, uint32_t, uint32_t, uint32_t);
    void (*axis)(void *, tb_wl_proxy *, uint32_t, uint32_t, tb_wl_fixed);
    void (*frame)(void *, tb_wl_proxy *);
    void (*axis_source)(void *, tb_wl_proxy *, uint32_t);
    void (*axis_stop)(void *, tb_wl_proxy *, uint32_t, uint32_t);
    void (*axis_discrete)(void *, tb_wl_proxy *, uint32_t, int32_t);
    void (*axis_value120)(void *, tb_wl_proxy *, uint32_t, int32_t);
} tb_wl_pointer_listener = {tb_wl_pointer_enter,         tb_wl_pointer_leave,
                            tb_wl_pointer_motion,        tb_wl_pointer_button,
                            tb_wl_pointer_axis,          tb_wl_pointer_frame,
                            tb_wl_pointer_axis_source,   tb_wl_pointer_axis_stop,
                            tb_wl_pointer_axis_discrete, tb_wl_pointer_axis_value120};

/* The mouse's own motion, unaccelerated, for the window that locked the
 * pointer. */
static void tb_wl_relative_motion(void *data, tb_wl_proxy *relative, uint32_t time_high,
                                  uint32_t time_low, tb_wl_fixed delta_x, tb_wl_fixed delta_y,
                                  tb_wl_fixed raw_x, tb_wl_fixed raw_y)
{
    tb_wl_system *const native = data;

    (void)relative;
    (void)time_high;
    (void)time_low;
    (void)delta_x;
    (void)delta_y;
    if (native->pointer_focus != NULL)
    {
        tb_window_mouse_relative(native->pointer_focus, tb_wl_from_fixed(raw_x),
                                 tb_wl_from_fixed(raw_y));
    }
}

static struct
{
    void (*relative_motion)(void *, tb_wl_proxy *, uint32_t, uint32_t, tb_wl_fixed, tb_wl_fixed,
                            tb_wl_fixed, tb_wl_fixed);
} tb_wl_relative_listener = {tb_wl_relative_motion};

/* The cursor theme, for a compositor without cursor shapes: its images
 * are drawn into a surface of the client's. */
static void tb_wl_load_cursor_theme(tb_wl_system *native)
{
    static const char *const names[] = {"wl_cursor_theme_load", "wl_cursor_theme_destroy",
                                        "wl_cursor_theme_get_cursor", "wl_cursor_image_get_buffer"};
    enum
    {
        COUNT = sizeof(names) / sizeof(names[0])
    };
    tb_library_function functions[COUNT] = {0};
    tb_library_binding bindings[COUNT] = {0};

    TB_STATIC_ASSERT(sizeof(tb_wl_cursor_api) == COUNT * sizeof(tb_library_function),
                     "the libwayland-cursor table has a function for each name");
    if (native->cursor_library != NULL ||
        tb_library_open("libwayland-cursor.so.0", TB_LIBRARY_SEARCH_SYSTEM, &native->cursor_library,
                        NULL) != TB_LIBRARY_OK)
    {
        return;
    }
    for (size_t index = 0; index < COUNT; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = names[index], .function = &functions[index], .required = true};
    }
    if (tb_library_bind(native->cursor_library, bindings, COUNT, NULL) != TB_LIBRARY_OK)
    {
        return;
    }
    memcpy(&native->cursor, functions, sizeof(native->cursor));
    native->cursor_theme = native->cursor.theme_load(NULL, TB_WL_CURSOR_SIZE, native->shm);
    native->cursor_surface = native->wl.marshal_flags(
        native->compositor, TB_WL_COMPOSITOR_CREATE_SURFACE, native->core.surface,
        native->wl.get_version(native->compositor), 0, NULL);
}

static void tb_wl_get_pointer(tb_wl_system *native, tb_wl_proxy *seat)
{
    native->pointer = native->wl.marshal_flags(seat, TB_WL_SEAT_GET_POINTER, native->core.pointer,
                                               native->wl.get_version(seat), 0, NULL);
    (void)native->wl.add_listener(native->pointer, (tb_wl_handler *)&tb_wl_pointer_listener,
                                  native);
    if (native->cursor_shape_manager != NULL)
    {
        native->cursor_shape_device = native->wl.marshal_flags(
            native->cursor_shape_manager, TB_CURSOR_SHAPE_MANAGER_GET_POINTER,
            &tb_cursor_shape_device_interface, 1, 0, NULL, native->pointer);
        return;
    }
    tb_wl_load_cursor_theme(native);
}

static void tb_wl_release_pointer(tb_wl_system *native)
{
    if (native->pointer == NULL)
    {
        return;
    }
    if (native->relative_pointer != NULL)
    {
        (void)native->wl.marshal_flags(native->relative_pointer, TB_RELATIVE_POINTER_DESTROY, NULL,
                                       1, TB_WL_MARSHAL_DESTROY);
        native->relative_pointer = NULL;
    }
    if (native->cursor_shape_device != NULL)
    {
        (void)native->wl.marshal_flags(native->cursor_shape_device, TB_CURSOR_SHAPE_DEVICE_DESTROY,
                                       NULL, 1, TB_WL_MARSHAL_DESTROY);
        native->cursor_shape_device = NULL;
    }
    if (native->wl.get_version(native->pointer) >= 3)
    {
        (void)native->wl.marshal_flags(native->pointer, TB_WL_POINTER_RELEASE, NULL,
                                       native->wl.get_version(native->pointer),
                                       TB_WL_MARSHAL_DESTROY);
    }
    else
    {
        native->wl.destroy(native->pointer);
    }
    native->pointer = NULL;
    native->pointer_focus = NULL;
}

static void tb_wl_seat_capabilities(void *data, tb_wl_proxy *seat, uint32_t capabilities)
{
    tb_wl_system *const native = data;
    const bool has_keyboard = (capabilities & TB_WL_SEAT_KEYBOARD) != 0;
    const bool has_pointer = (capabilities & TB_WL_SEAT_POINTER) != 0;

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
    if (has_pointer && native->pointer == NULL)
    {
        tb_wl_get_pointer(native, seat);
    }
    else if (!has_pointer)
    {
        tb_wl_release_pointer(native);
    }
}

/* Lock the pointer where it is, hidden, and read the mouse's own motion. */
static tb_window_result tb_wl_lock_pointer(tb_window *window)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    if (native->relative_pointer == NULL)
    {
        native->relative_pointer =
            native->wl.marshal_flags(native->relative_manager, TB_RELATIVE_MANAGER_GET,
                                     &tb_relative_pointer_interface, 1, 0, NULL, native->pointer);
        (void)native->wl.add_listener(native->relative_pointer,
                                      (tb_wl_handler *)&tb_wl_relative_listener, native);
    }
    record->locked_pointer = native->wl.marshal_flags(
        native->pointer_constraints, TB_POINTER_CONSTRAINTS_LOCK, &tb_locked_pointer_interface, 1,
        0, NULL, record->surface, native->pointer, NULL, TB_WL_LOCK_PERSISTENT);
    (void)native->wl.flush(native->display);
    return TB_WINDOW_OK;
}
/* coverage-on */

static void tb_wl_unlock_pointer(tb_window *window)
{
    tb_wl_system *const native = tb_wl_of(window->system);
    tb_wl_window *const record = tb_wl_window_of(window);

    if (record->locked_pointer == NULL)
    {
        return;
    }
    /* coverage-off: only a seat with a mouse locks the pointer; see above */
    (void)native->wl.marshal_flags(record->locked_pointer, TB_LOCKED_POINTER_DESTROY, NULL, 1,
                                   TB_WL_MARSHAL_DESTROY);
    record->locked_pointer = NULL;
    (void)native->wl.flush(native->display);
    /* coverage-on */
}

static tb_window_result tb_wl_set_relative_mouse(tb_window *window, bool relative,
                                                 tb_window_error *error)
{
    tb_wl_system *const native = tb_wl_of(window->system);

    if (!relative)
    {
        tb_wl_unlock_pointer(window);
        return TB_WINDOW_OK;
    }
    if (native->relative_manager == NULL || native->pointer_constraints == NULL)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE,
                              "the Wayland compositor offers no zwp_relative_pointer_manager_v1 "
                              "and zwp_pointer_constraints_v1 for relative mouse mode");
    }
    if (native->pointer == NULL)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "the Wayland seat has no mouse");
    }
    /* coverage-off: the headless compositor that CI tests under has no seat, so no mouse */
    return tb_wl_lock_pointer(window);
    /* coverage-on */
}

static void tb_wl_update_cursor(tb_window *window)
{
    tb_wl_apply_cursor(tb_wl_of(window->system), window);
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

    const bool has_hint = record->configured_width > 0 && record->configured_height > 0;
    const bool restoring = record->restore_requested && record->pending_policy == TB_WL_FLOATING;
    /* An activation change can repeat the previous size after the client
     * has resized. Keep the newer size. A drag or a maximized/fullscreen
     * configure still takes the compositor's dimensions. */
    if (record->configured && record->policy == TB_WL_FLOATING &&
        record->pending_policy == TB_WL_FLOATING &&
        (!has_hint || ((uint32_t)record->configured_width == record->last_hint_width &&
                       (uint32_t)record->configured_height == record->last_hint_height)))
    {
        return;
    }
    if (has_hint && !restoring)
    {
        record->width = (uint32_t)record->configured_width;
        record->height = (uint32_t)record->configured_height;
        return;
    }
    if (record->pending_policy == TB_WL_FULLSCREEN)
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
    record->last_hint_width =
        record->configured_width > 0 ? (uint32_t)record->configured_width : record->width;
    record->last_hint_height =
        record->configured_height > 0 ? (uint32_t)record->configured_height : record->height;
    record->policy = record->pending_policy;
    if (record->policy != TB_WL_FULLSCREEN)
    {
        record->restore_requested = false;
    }
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
    const uint32_t *const values = states->data;
    record->pending_policy = TB_WL_FLOATING;
    for (size_t index = 0; index < states->size / sizeof(*values); index++)
    {
        if (values[index] == TB_XDG_STATE_FULLSCREEN)
        {
            record->pending_policy = TB_WL_FULLSCREEN;
            break;
        }
        if (values[index] == TB_XDG_STATE_MAXIMIZED || values[index] == TB_XDG_STATE_RESIZING)
        {
            record->pending_policy = TB_WL_COMPOSITOR_SIZED;
        }
    }
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
        /* A fixed size is both the least and the most; else the least is the
         * options' and there is no most. xdg-shell has no hint of an aspect
         * to keep, so the player sizes the window freely above its least. */
        uint32_t least_width = 0;
        uint32_t least_height = 0;
        int32_t most_width = 0;
        int32_t most_height = 0;
        tb_window_least_size(&window->options, &least_width, &least_height);
        if (window->options.fixed_size)
        {
            least_width = placement.width;
            least_height = placement.height;
            most_width = (int32_t)placement.width;
            most_height = (int32_t)placement.height;
        }
        (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_SET_MIN_SIZE, NULL,
                                       version, 0, (int32_t)least_width, (int32_t)least_height);
        (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_SET_MAX_SIZE, NULL,
                                       version, 0, most_width, most_height);
        if (!created && !was_windowed)
        {
            record->restore_requested = true;
            (void)native->wl.marshal_flags(record->toplevel, TB_XDG_TOPLEVEL_UNSET_FULLSCREEN, NULL,
                                           version, 0);
        }
        /* Leave the compositor's geometry in place until it configures the
         * surface as a floating window again. */
        if (record->configured && record->policy == TB_WL_FLOATING)
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
        record->restore_requested = false;
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
    if (native->pointer_focus == window)
    {
        /* coverage-off: the headless compositor that CI tests under has no seat, so no mouse */
        native->pointer_focus = NULL;
        /* coverage-on */
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

/* What turns keys into text, when the compositor sends the keymap. */
static void tb_wl_release_xkb(tb_wl_system *native)
{
    if (native->xkb_context == NULL)
    {
        tb_library_close(native->xkb_library);
        return;
    }
    tb_wl_forget_keymap(native);
    if (native->compose_table != NULL)
    {
        native->xkb.compose_table_unref(native->compose_table);
    }
    native->xkb.context_unref(native->xkb_context);
    tb_library_close(native->xkb_library);
}

static void tb_wl_release_mouse(tb_wl_system *native)
{
    tb_wl_release_pointer(native);
    if (native->cursor_theme != NULL)
    {
        /* coverage-off: only a compositor without cursor shapes needs a theme */
        native->cursor.theme_destroy(native->cursor_theme);
        /* coverage-on */
    }
    if (native->cursor_surface != NULL)
    {
        /* coverage-off: see above */
        (void)native->wl.marshal_flags(native->cursor_surface, TB_WL_SURFACE_DESTROY, NULL,
                                       native->wl.get_version(native->cursor_surface),
                                       TB_WL_MARSHAL_DESTROY);
        /* coverage-on */
    }
    tb_wl_destroy_global(native, &native->cursor_shape_manager, TB_CURSOR_SHAPE_MANAGER_DESTROY);
    tb_wl_destroy_global(native, &native->relative_manager, TB_RELATIVE_MANAGER_DESTROY);
    tb_wl_destroy_global(native, &native->pointer_constraints, TB_POINTER_CONSTRAINTS_DESTROY);
    tb_library_close(native->cursor_library);
}

static void tb_wl_release(tb_wl_system *native)
{
    if (native->display != NULL)
    {
        tb_wl_release_keyboard(native);
        tb_wl_release_mouse(native);
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
    tb_wl_release_xkb(native);
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
    .confirm = tb_linux_confirm,
    .set_relative_mouse = tb_wl_set_relative_mouse,
    .update_cursor = tb_wl_update_cursor,
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
        "wl_pointer_interface",  "wl_region_interface",
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
    /* And so do those of the mouse's protocols. */
    tb_cursor_shape_get_types[1] = native->core.pointer;
    tb_relative_get_types[1] = native->core.pointer;
    for (size_t index = 1; index < 4; index++)
    {
        const tb_wl_interface *const core_types[4] = {NULL, native->core.surface,
                                                      native->core.pointer, native->core.region};
        tb_lock_pointer_types[index] = core_types[index];
        tb_confine_pointer_types[index] = core_types[index];
    }
    tb_locked_region_types[0] = native->core.region;
    return TB_WINDOW_OK;
}

/* The locale whose compose sequences dead keys follow, as the C library
 * would choose it. */
static const char *tb_wl_locale(void)
{
    static const char *const variables[] = {"LC_ALL", "LC_CTYPE", "LANG"};

    for (size_t index = 0; index < sizeof(variables) / sizeof(variables[0]); index++)
    {
        const char *const value = getenv(variables[index]);
        if (value != NULL && value[0] != '\0')
        {
            return value;
        }
    }
    return "C";
}

/* libxkbcommon, for text: without it, keys type nothing. */
static void tb_wl_load_xkb(tb_wl_system *native)
{
    static const char *const names[] = {
        "xkb_context_new",
        "xkb_context_unref",
        "xkb_keymap_new_from_string",
        "xkb_keymap_unref",
        "xkb_state_new",
        "xkb_state_unref",
        "xkb_state_update_mask",
        "xkb_state_key_get_one_sym",
        "xkb_state_key_get_utf8",
        "xkb_state_mod_name_is_active",
        "xkb_compose_table_new_from_locale",
        "xkb_compose_table_unref",
        "xkb_compose_state_new",
        "xkb_compose_state_unref",
        "xkb_compose_state_feed",
        "xkb_compose_state_reset",
        "xkb_compose_state_get_status",
        "xkb_compose_state_get_utf8",
    };
    enum
    {
        COUNT = sizeof(names) / sizeof(names[0])
    };
    tb_library_function functions[COUNT] = {0};
    tb_library_binding bindings[COUNT] = {0};

    TB_STATIC_ASSERT(sizeof(tb_xkb_api) == COUNT * sizeof(tb_library_function),
                     "the xkbcommon table has a function for each name");
    if (tb_library_open("libxkbcommon.so.0", TB_LIBRARY_SEARCH_SYSTEM, &native->xkb_library,
                        NULL) != TB_LIBRARY_OK)
    {
        /* coverage-off: every desktop that runs Wayland has xkbcommon */
        return;
        /* coverage-on */
    }
    for (size_t index = 0; index < COUNT; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = names[index], .function = &functions[index], .required = true};
    }
    if (tb_library_bind(native->xkb_library, bindings, COUNT, NULL) != TB_LIBRARY_OK)
    {
        /* coverage-off: every libxkbcommon.so.0 since 0.5 exports these */
        return;
        /* coverage-on */
    }
    memcpy(&native->xkb, functions, sizeof(native->xkb));
    native->xkb_context = native->xkb.context_new(0);
    if (native->xkb_context == NULL)
    {
        /* coverage-off: xkbcommon makes a context unless out of memory */
        return;
        /* coverage-on */
    }
    /* A locale without compose sequences has no dead keys. */
    native->compose_table =
        native->xkb.compose_table_new_from_locale(native->xkb_context, tb_wl_locale(), 0);
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
    tb_wl_load_xkb(native);
    native->displays_changed = false;
    tb_wl_publish_displays(native);
    return TB_WINDOW_OK;
}
