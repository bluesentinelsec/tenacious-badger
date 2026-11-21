/* X11 windowing, from libX11.so.6 and libXrandr.so.2 loaded at run time.
 *
 * The few structures and constants of Xlib that the engine needs are
 * declared here, as Xlib's ABI fixes them, so that building the engine needs
 * no X development headers. A window manager makes a window fullscreen
 * through _NET_WM_STATE; without one, as under a bare X server, the window
 * is also moved and sized to cover its monitor, which is what fullscreen
 * means there. */
#include "window_linux.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Xlib, as much as is used.
 * ------------------------------------------------------------------------- */

typedef struct tb_x11_display tb_x11_display; /* Display */
typedef unsigned long tb_x11_id;              /* XID, Window, Atom, Time */
typedef int tb_x11_bool;

enum
{
    TB_X11_KEY_PRESS = 2,
    TB_X11_KEY_RELEASE = 3,
    TB_X11_FOCUS_IN = 9,
    TB_X11_FOCUS_OUT = 10,
    TB_X11_UNMAP_NOTIFY = 18,
    TB_X11_MAP_NOTIFY = 19,
    TB_X11_CONFIGURE_NOTIFY = 22,
    TB_X11_CLIENT_MESSAGE = 33
};

#define TB_X11_KEY_PRESS_MASK (1L << 0)
#define TB_X11_KEY_RELEASE_MASK (1L << 1)
#define TB_X11_STRUCTURE_NOTIFY_MASK (1L << 17)
#define TB_X11_SUBSTRUCTURE_NOTIFY_MASK (1L << 19)
#define TB_X11_SUBSTRUCTURE_REDIRECT_MASK (1L << 20)
#define TB_X11_FOCUS_CHANGE_MASK (1L << 21)
#define TB_X11_US_POSITION (1L << 0)
#define TB_X11_US_SIZE (1L << 1)
#define TB_X11_MIN_SIZE (1L << 4)
#define TB_X11_MAX_SIZE (1L << 5)
#define TB_X11_ATOM_TYPE 4ul
#define TB_X11_CARDINAL_TYPE 6ul
#define TB_X11_REPLACE 0
#define TB_X11_SUCCESS 0
#define TB_X11_STATE_REMOVE 0L
#define TB_X11_STATE_ADD 1L
#define TB_RANDR_SCREEN_CHANGE_MASK 1
#define TB_RANDR_SCREEN_CHANGE_NOTIFY 0

typedef struct tb_x11_any_event
{
    int type;
    unsigned long serial;
    tb_x11_bool send_event;
    tb_x11_display *display;
    tb_x11_id window;
} tb_x11_any_event;

typedef struct tb_x11_key_event
{
    int type;
    unsigned long serial;
    tb_x11_bool send_event;
    tb_x11_display *display;
    tb_x11_id window;
    tb_x11_id root;
    tb_x11_id subwindow;
    tb_x11_id time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int keycode;
    tb_x11_bool same_screen;
} tb_x11_key_event;

typedef struct tb_x11_configure_event
{
    int type;
    unsigned long serial;
    tb_x11_bool send_event;
    tb_x11_display *display;
    tb_x11_id event;
    tb_x11_id window;
    int x;
    int y;
    int width;
    int height;
    int border_width;
    tb_x11_id above;
    tb_x11_bool override_redirect;
} tb_x11_configure_event;

typedef struct tb_x11_client_message
{
    int type;
    unsigned long serial;
    tb_x11_bool send_event;
    tb_x11_display *display;
    tb_x11_id window;
    tb_x11_id message_type;
    int format;
    union {
        char b[20];
        short s[10];
        long l[5];
    } data;
} tb_x11_client_message;

typedef union tb_x11_event {
    int type;
    tb_x11_any_event any;
    tb_x11_key_event key;
    tb_x11_configure_event configure;
    tb_x11_client_message client;
    long pad[24];
} tb_x11_event;

typedef struct tb_x11_size_hints
{
    long flags;
    int x;
    int y;
    int width;
    int height;
    int min_width;
    int min_height;
    int max_width;
    int max_height;
    int width_increment;
    int height_increment;
    int min_aspect[2];
    int max_aspect[2];
    int base_width;
    int base_height;
    int gravity;
} tb_x11_size_hints;

typedef struct tb_randr_monitor
{
    tb_x11_id name;
    tb_x11_bool primary;
    tb_x11_bool automatic;
    int output_count;
    int x;
    int y;
    int width;
    int height;
    int width_mm;
    int height_mm;
    void *outputs;
} tb_randr_monitor;

typedef int (*tb_x11_error_handler)(tb_x11_display *display, void *event);

typedef struct tb_x11_api
{
    tb_x11_display *(*open_display)(const char *name);
    int (*close_display)(tb_x11_display *display);
    int (*default_screen)(tb_x11_display *display);
    tb_x11_id (*root_window)(tb_x11_display *display, int screen);
    unsigned long (*black_pixel)(tb_x11_display *display, int screen);
    int (*display_width)(tb_x11_display *display, int screen);
    int (*display_height)(tb_x11_display *display, int screen);
    int (*display_width_mm)(tb_x11_display *display, int screen);
    int (*display_height_mm)(tb_x11_display *display, int screen);
    int (*connection_number)(tb_x11_display *display);
    tb_x11_id (*create_simple_window)(tb_x11_display *display, tb_x11_id parent, int x, int y,
                                      unsigned width, unsigned height, unsigned border_width,
                                      unsigned long border, unsigned long background);
    int (*destroy_window)(tb_x11_display *display, tb_x11_id window);
    int (*select_input)(tb_x11_display *display, tb_x11_id window, long mask);
    int (*map_window)(tb_x11_display *display, tb_x11_id window);
    int (*move_resize_window)(tb_x11_display *display, tb_x11_id window, int x, int y,
                              unsigned width, unsigned height);
    tb_x11_id (*intern_atom)(tb_x11_display *display, const char *name, tb_x11_bool only_if_exists);
    int (*set_wm_protocols)(tb_x11_display *display, tb_x11_id window, tb_x11_id *protocols,
                            int count);
    int (*change_property)(tb_x11_display *display, tb_x11_id window, tb_x11_id property,
                           tb_x11_id type, int format, int mode, const unsigned char *data,
                           int count);
    int (*delete_property)(tb_x11_display *display, tb_x11_id window, tb_x11_id property);
    int (*get_window_property)(tb_x11_display *display, tb_x11_id window, tb_x11_id property,
                               long offset, long length, tb_x11_bool remove, tb_x11_id type,
                               tb_x11_id *actual_type, int *actual_format, unsigned long *count,
                               unsigned long *remaining, unsigned char **data);
    void (*set_wm_normal_hints)(tb_x11_display *display, tb_x11_id window,
                                tb_x11_size_hints *hints);
    int (*send_event)(tb_x11_display *display, tb_x11_id window, tb_x11_bool propagate, long mask,
                      tb_x11_event *event);
    int (*pending)(tb_x11_display *display);
    int (*next_event)(tb_x11_display *display, tb_x11_event *event);
    int (*flush)(tb_x11_display *display);
    int (*sync)(tb_x11_display *display, tb_x11_bool discard);
    int (*free_data)(void *data);
    char *(*get_atom_name)(tb_x11_display *display, tb_x11_id atom);
    char *(*resource_manager_string)(tb_x11_display *display);
    tb_x11_bool (*translate_coordinates)(tb_x11_display *display, tb_x11_id source,
                                         tb_x11_id destination, int x, int y, int *to_x, int *to_y,
                                         tb_x11_id *child);
    int (*get_geometry)(tb_x11_display *display, tb_x11_id drawable, tb_x11_id *root, int *x,
                        int *y, unsigned *width, unsigned *height, unsigned *border,
                        unsigned *depth);
    tb_x11_bool (*detectable_auto_repeat)(tb_x11_display *display, tb_x11_bool detectable,
                                          tb_x11_bool *supported);
    tb_x11_error_handler (*set_error_handler)(tb_x11_error_handler handler);
} tb_x11_api;

typedef struct tb_randr_api
{
    tb_x11_bool (*query_extension)(tb_x11_display *display, int *event_base, int *error_base);
    tb_randr_monitor *(*get_monitors)(tb_x11_display *display, tb_x11_id window, tb_x11_bool active,
                                      int *count);
    void (*free_monitors)(tb_randr_monitor *monitors);
    void (*select_input)(tb_x11_display *display, tb_x11_id window, int mask);
    int (*update_configuration)(tb_x11_event *event);
} tb_randr_api;

/* ---------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */

typedef struct tb_x11_system
{
    tb_library *xlib;
    tb_library *randr_library;
    tb_x11_api x;
    tb_randr_api randr;
    bool has_randr;
    int randr_event_base;
    tb_x11_display *display;
    int screen;
    tb_x11_id root;
    double scale; /* Xft.dpi over 96, for every monitor. */
    tb_x11_id delete_window;
    tb_x11_id wm_state;
    tb_x11_id wm_fullscreen;
    tb_x11_id net_wm_name;
    tb_x11_id utf8_string;
    tb_x11_id work_area;
    tb_linux_waker waker;
    tb_x11_error_handler previous_handler;
    uint8_t keys_down[32]; /* A bit for each keycode held down. */
} tb_x11_system;

typedef struct tb_x11_window
{
    tb_x11_id handle;
} tb_x11_window;

static tb_x11_system *tb_x11_of(const tb_window_system *system)
{
    return system->native;
}

static tb_x11_id tb_x11_handle(const tb_window *window)
{
    return ((const tb_x11_window *)window->native)->handle;
}

/* Xlib's default handler of a protocol error ends the process; a request
 * that fails, a window already gone, is the backend's to cope with. */
static int tb_x11_ignore_error(tb_x11_display *display, void *event)
{
    (void)display;
    (void)event;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Loading.
 * ------------------------------------------------------------------------- */

static tb_window_result tb_x11_load(tb_x11_system *native, tb_window_error *error)
{
    static const char *const names[] = {
        "XOpenDisplay",
        "XCloseDisplay",
        "XDefaultScreen",
        "XRootWindow",
        "XBlackPixel",
        "XDisplayWidth",
        "XDisplayHeight",
        "XDisplayWidthMM",
        "XDisplayHeightMM",
        "XConnectionNumber",
        "XCreateSimpleWindow",
        "XDestroyWindow",
        "XSelectInput",
        "XMapWindow",
        "XMoveResizeWindow",
        "XInternAtom",
        "XSetWMProtocols",
        "XChangeProperty",
        "XDeleteProperty",
        "XGetWindowProperty",
        "XSetWMNormalHints",
        "XSendEvent",
        "XPending",
        "XNextEvent",
        "XFlush",
        "XSync",
        "XFree",
        "XGetAtomName",
        "XResourceManagerString",
        "XTranslateCoordinates",
        "XGetGeometry",
        "XkbSetDetectableAutoRepeat",
        "XSetErrorHandler",
    };
    enum
    {
        COUNT = sizeof(names) / sizeof(names[0])
    };
    tb_library_function functions[COUNT] = {0};
    tb_library_binding bindings[COUNT] = {0};
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;

    TB_STATIC_ASSERT(sizeof(tb_x11_api) == COUNT * sizeof(tb_library_function),
                     "the Xlib table has a function for each name");
    if (tb_library_open("libX11.so.6", TB_LIBRARY_SEARCH_SYSTEM, &native->xlib, &library_error) !=
        TB_LIBRARY_OK)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "no X11: %s", library_error.message);
    }
    for (size_t index = 0; index < COUNT; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = names[index], .function = &functions[index], .required = true};
    }
    if (tb_library_bind(native->xlib, bindings, COUNT, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: every libX11.so.6 exports these */
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s", library_error.message);
        /* coverage-on */
    }
    /* The table is the functions in the order of their names. */
    memcpy(&native->x, functions, sizeof(native->x));
    return TB_WINDOW_OK;
}

static void tb_x11_load_randr(tb_x11_system *native)
{
    static const char *const names[] = {"XRRQueryExtension", "XRRGetMonitors", "XRRFreeMonitors",
                                        "XRRSelectInput", "XRRUpdateConfiguration"};
    enum
    {
        COUNT = sizeof(names) / sizeof(names[0])
    };
    tb_library_function functions[COUNT] = {0};
    tb_library_binding bindings[COUNT] = {0};
    int error_base = 0;

    TB_STATIC_ASSERT(sizeof(tb_randr_api) == COUNT * sizeof(tb_library_function),
                     "the XRandR table has a function for each name");
    if (tb_library_open("libXrandr.so.2", TB_LIBRARY_SEARCH_SYSTEM, &native->randr_library, NULL) !=
        TB_LIBRARY_OK)
    {
        /* coverage-off: an X server without XRandR lists one screen */
        return;
        /* coverage-on */
    }
    for (size_t index = 0; index < COUNT; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = names[index], .function = &functions[index], .required = true};
    }
    if (tb_library_bind(native->randr_library, bindings, COUNT, NULL) != TB_LIBRARY_OK)
    {
        /* coverage-off: every libXrandr.so.2 since 1.5 exports these */
        return;
        /* coverage-on */
    }
    memcpy(&native->randr, functions, sizeof(native->randr));
    native->has_randr =
        native->randr.query_extension(native->display, &native->randr_event_base, &error_base) != 0;
}

/* ---------------------------------------------------------------------------
 * Displays.
 * ------------------------------------------------------------------------- */

/* Xft.dpi, which desktops set to the scale the user chose; 96 is 1. */
static double tb_x11_read_scale(tb_x11_system *native)
{
    const char *const resources = native->x.resource_manager_string(native->display);
    const char *const key = "Xft.dpi:";
    size_t at = 0;
    double dpi = 0.0;

    if (resources == NULL)
    {
        return 1.0;
    }
    at = tb_str_find(resources, 0, TB_STR_END, key);
    if (at == TB_STR_NOT_FOUND)
    {
        return 1.0;
    }
    at += tb_str_length(key);
    while (resources[at] == ' ' || resources[at] == '\t')
    {
        at++;
    }
    while (resources[at] >= '0' && resources[at] <= '9')
    {
        dpi = dpi * 10.0 + (double)(resources[at] - '0');
        at++;
    }
    return dpi >= 48.0 ? dpi / 96.0 : 1.0;
}

/* The work area, the part of the screen that panels leave free, as the
 * window manager publishes it; the whole screen without one. */
static void tb_x11_work_area(tb_x11_system *native, long area[4])
{
    tb_x11_id type = 0;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char *data = NULL;

    area[0] = 0;
    area[1] = 0;
    area[2] = native->x.display_width(native->display, native->screen);
    area[3] = native->x.display_height(native->display, native->screen);
    if (native->x.get_window_property(native->display, native->root, native->work_area, 0, 4, 0,
                                      TB_X11_CARDINAL_TYPE, &type, &format, &count, &remaining,
                                      &data) != TB_X11_SUCCESS)
    {
        /* coverage-off: reading a property of the root window does not fail */
        return;
        /* coverage-on */
    }
    if (data != NULL && format == 32 && count >= 4)
    {
        memcpy(area, data, 4 * sizeof(long));
    }
    if (data != NULL)
    {
        (void)native->x.free_data(data);
    }
}

static int32_t tb_x11_max(int32_t left, int32_t right)
{
    return left > right ? left : right;
}

static int32_t tb_x11_min(int32_t left, int32_t right)
{
    return left < right ? left : right;
}

/* The usable part of a display: its rectangle within the work area. */
static void tb_x11_set_usable(tb_display *display, const long area[4])
{
    const int32_t left = tb_x11_max(display->x, (int32_t)area[0]);
    const int32_t top = tb_x11_max(display->y, (int32_t)area[1]);
    const int32_t right =
        tb_x11_min(display->x + (int32_t)display->width, (int32_t)(area[0] + area[2]));
    const int32_t bottom =
        tb_x11_min(display->y + (int32_t)display->height, (int32_t)(area[1] + area[3]));

    if (right <= left || bottom <= top)
    {
        display->usable_x = display->x;
        display->usable_y = display->y;
        display->usable_width = display->width;
        display->usable_height = display->height;
        return;
    }
    display->usable_x = left;
    display->usable_y = top;
    display->usable_width = (uint32_t)(right - left);
    display->usable_height = (uint32_t)(bottom - top);
}

static void tb_x11_read_displays(tb_window_system *system)
{
    tb_x11_system *const native = tb_x11_of(system);
    tb_display displays[16] = {0};
    size_t count = 0;
    long area[4] = {0};

    native->scale = tb_x11_read_scale(native);
    tb_x11_work_area(native, area);
    if (native->has_randr)
    {
        int found = 0;
        tb_randr_monitor *const monitors =
            native->randr.get_monitors(native->display, native->root, 1, &found);
        for (int index = 0; index < found && count < 16; index++)
        {
            tb_display *const display = &displays[count];
            char *const name = native->x.get_atom_name(native->display, monitors[index].name);
            display->primary = monitors[index].primary != 0;
            display->x = monitors[index].x;
            display->y = monitors[index].y;
            display->width = (uint32_t)monitors[index].width;
            display->height = (uint32_t)monitors[index].height;
            display->physical_width_mm = (uint32_t)monitors[index].width_mm;
            display->physical_height_mm = (uint32_t)monitors[index].height_mm;
            if (name != NULL)
            {
                (void)tb_fmt_buffer(display->name, sizeof(display->name), "%s", name);
                (void)native->x.free_data(name);
            }
            count++;
        }
        if (monitors != NULL)
        {
            native->randr.free_monitors(monitors);
        }
    }
    if (count == 0)
    {
        /* coverage-off: XRandR lists every active monitor */
        displays[0] = (tb_display){
            .primary = true,
            .width = (uint32_t)native->x.display_width(native->display, native->screen),
            .height = (uint32_t)native->x.display_height(native->display, native->screen),
            .physical_width_mm =
                (uint32_t)native->x.display_width_mm(native->display, native->screen),
            .physical_height_mm =
                (uint32_t)native->x.display_height_mm(native->display, native->screen)};
        count = 1;
        /* coverage-on */
    }
    for (size_t index = 0; index < count; index++)
    {
        displays[index].pixel_width = displays[index].width;
        displays[index].pixel_height = displays[index].height;
        displays[index].content_scale = native->scale;
        tb_x11_set_usable(&displays[index], area);
    }
    tb_window_set_displays(system, displays, count);
}

/* ---------------------------------------------------------------------------
 * Windows.
 * ------------------------------------------------------------------------- */

static void tb_x11_set_title(tb_window *window)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const char *const title = window->options.title;

    (void)native->x.change_property(native->display, tb_x11_handle(window), native->net_wm_name,
                                    native->utf8_string, 8, TB_X11_REPLACE,
                                    (const unsigned char *)title, (int)tb_str_length(title));
}

/* Ask the window manager to add or remove fullscreen. */
static void tb_x11_send_fullscreen(tb_window *window, long action)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    tb_x11_event event = {0};

    event.client = (tb_x11_client_message){.type = TB_X11_CLIENT_MESSAGE,
                                           .window = tb_x11_handle(window),
                                           .message_type = native->wm_state,
                                           .format = 32};
    event.client.data.l[0] = action;
    event.client.data.l[1] = (long)native->wm_fullscreen;
    event.client.data.l[3] = 1;
    (void)native->x.send_event(native->display, native->root, 0,
                               TB_X11_SUBSTRUCTURE_NOTIFY_MASK | TB_X11_SUBSTRUCTURE_REDIRECT_MASK,
                               &event);
}

static void tb_x11_set_hints(tb_window *window, const tb_window_placement *placement)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    tb_x11_size_hints hints = {.flags = TB_X11_US_POSITION | TB_X11_US_SIZE,
                               .x = placement->x,
                               .y = placement->y,
                               .width = (int)placement->unit_width,
                               .height = (int)placement->unit_height};

    if (window->options.fixed_size && window->state.mode == TB_WINDOW_MODE_WINDOWED)
    {
        hints.flags |= TB_X11_MIN_SIZE | TB_X11_MAX_SIZE;
        hints.min_width = hints.width;
        hints.max_width = hints.width;
        hints.min_height = hints.height;
        hints.max_height = hints.height;
    }
    native->x.set_wm_normal_hints(native->display, tb_x11_handle(window), &hints);
}

/* Put the window in its mode on its display: fullscreen is borderless on
 * X11. `mapped` says whether the window manager already manages it. */
static void tb_x11_apply(tb_window *window, bool mapped)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const tb_x11_id handle = tb_x11_handle(window);
    tb_window_placement placement = {0};
    tb_display display = {0};

    tb_window_place(window, true, &placement);
    tb_window_system_display(window->system, placement.display, &display);
    window->state.mode = window->options.mode == TB_WINDOW_MODE_WINDOWED
                             ? TB_WINDOW_MODE_WINDOWED
                             : TB_WINDOW_MODE_BORDERLESS;
    window->state.display = placement.display;
    tb_x11_set_hints(window, &placement);
    if (window->state.mode == TB_WINDOW_MODE_WINDOWED)
    {
        if (mapped)
        {
            tb_x11_send_fullscreen(window, TB_X11_STATE_REMOVE);
        }
        else
        {
            (void)native->x.delete_property(native->display, handle, native->wm_state);
        }
        (void)native->x.move_resize_window(native->display, handle, placement.x, placement.y,
                                           placement.unit_width, placement.unit_height);
    }
    else
    {
        if (mapped)
        {
            tb_x11_send_fullscreen(window, TB_X11_STATE_ADD);
        }
        else
        {
            (void)native->x.change_property(native->display, handle, native->wm_state,
                                            TB_X11_ATOM_TYPE, 32, TB_X11_REPLACE,
                                            (const unsigned char *)&native->wm_fullscreen, 1);
        }
        (void)native->x.move_resize_window(native->display, handle, display.x, display.y,
                                           display.width, display.height);
    }
    (void)native->x.flush(native->display);
}

/* Read the window's size and the display it is on into its state. */
static void tb_x11_read_size(tb_window *window, uint32_t width, uint32_t height)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    int x = 0;
    int y = 0;
    tb_x11_id child = 0;

    if (width == 0 || height == 0)
    {
        return;
    }
    tb_window_resized(window, width, height, native->scale);
    if (native->x.translate_coordinates(native->display, tb_x11_handle(window), native->root, 0, 0,
                                        &x, &y, &child))
    {
        window->state.display = tb_window_display_at(window->system, x, y, width, height);
    }
}

static tb_window_result tb_x11_create(tb_window *window, tb_window_error *error)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    tb_x11_window *const record = tb_calloc(1, sizeof(tb_x11_window));
    const unsigned long black = native->x.black_pixel(native->display, native->screen);
    tb_window_placement placement = {0};
    tb_x11_id root = 0;
    int x = 0;
    int y = 0;
    unsigned width = 0;
    unsigned height = 0;
    unsigned border = 0;
    unsigned depth = 0;

    tb_window_place(window, true, &placement);
    record->handle = native->x.create_simple_window(native->display, native->root, placement.x,
                                                    placement.y, placement.unit_width,
                                                    placement.unit_height, 0, black, black);
    if (record->handle == 0)
    {
        /* coverage-off: an X server refuses a window only when out of resources */
        tb_free(record);
        return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR, "the X server refused a window");
        /* coverage-on */
    }
    window->native = record;
    (void)native->x.select_input(native->display, record->handle,
                                 TB_X11_KEY_PRESS_MASK | TB_X11_KEY_RELEASE_MASK |
                                     TB_X11_STRUCTURE_NOTIFY_MASK | TB_X11_FOCUS_CHANGE_MASK);
    (void)native->x.set_wm_protocols(native->display, record->handle, &native->delete_window, 1);
    tb_x11_set_title(window);
    tb_x11_apply(window, false);
    (void)native->x.map_window(native->display, record->handle);
    (void)native->x.sync(native->display, 0);
    if (native->x.get_geometry(native->display, record->handle, &root, &x, &y, &width, &height,
                               &border, &depth))
    {
        tb_x11_read_size(window, width, height);
    }
    return TB_WINDOW_OK;
}

static void tb_x11_destroy(tb_window *window)
{
    tb_x11_system *const native = tb_x11_of(window->system);

    (void)native->x.destroy_window(native->display, tb_x11_handle(window));
    (void)native->x.flush(native->display);
    tb_free(window->native);
}

static tb_window_result tb_x11_configure(tb_window *window, const tb_window_options *previous,
                                         tb_window_error *error)
{
    (void)previous;
    (void)error;
    tb_x11_set_title(window);
    tb_x11_apply(window, true);
    return TB_WINDOW_OK;
}

static void tb_x11_native(const tb_window *window, tb_window_native *native)
{
    native->system = tb_x11_of(window->system)->display;
    native->x11_window = tb_x11_handle(window);
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

static tb_window *tb_x11_find(tb_window_system *system, tb_x11_id handle)
{
    for (size_t index = 0; index < tb_array_length(system->windows); index++)
    {
        tb_window *const window = *TB_ARRAY_AT(system->windows, tb_window *, index);
        if (tb_x11_handle(window) == handle)
        {
            return window;
        }
    }
    return NULL;
}

static void tb_x11_on_key(tb_window *window, const tb_x11_key_event *key)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const uint8_t bit = (uint8_t)(1u << (key->keycode & 7u));
    uint8_t *const held = &native->keys_down[(key->keycode & 0xFFu) >> 3];
    const bool down = key->type == TB_X11_KEY_PRESS;
    const bool repeat = down && (*held & bit) != 0;
    /* X keycodes are evdev's, 8 higher. */
    const uint32_t code = key->keycode >= 8u ? key->keycode - 8u : 0u;

    *held = down ? (uint8_t)(*held | bit) : (uint8_t)(*held & ~bit);
    tb_window_key(window, tb_linux_key(code), code, down, repeat);
}

static void tb_x11_handle_event(tb_window_system *system, tb_x11_event *event)
{
    tb_x11_system *const native = tb_x11_of(system);
    tb_window *window = NULL;

    if (native->has_randr &&
        event->type == native->randr_event_base + TB_RANDR_SCREEN_CHANGE_NOTIFY)
    {
        (void)native->randr.update_configuration(event);
        tb_x11_read_displays(system);
        tb_window_displays_changed(system);
        return;
    }
    window = tb_x11_find(system, event->any.window);
    if (window == NULL)
    {
        return;
    }
    switch (event->type)
    {
    case TB_X11_KEY_PRESS:
    case TB_X11_KEY_RELEASE:
        tb_x11_on_key(window, &event->key);
        break;
    case TB_X11_FOCUS_IN:
        tb_window_focused(window, true);
        break;
    case TB_X11_FOCUS_OUT:
        tb_window_focused(window, false);
        break;
    case TB_X11_MAP_NOTIFY:
        tb_window_minimized(window, false);
        break;
    case TB_X11_UNMAP_NOTIFY:
        tb_window_minimized(window, true);
        break;
    case TB_X11_CONFIGURE_NOTIFY:
        tb_x11_read_size(window, (uint32_t)event->configure.width,
                         (uint32_t)event->configure.height);
        break;
    case TB_X11_CLIENT_MESSAGE:
        if ((tb_x11_id)event->client.data.l[0] == native->delete_window)
        {
            const tb_window_event request = {
                .type = TB_WINDOW_EVENT_CLOSE_REQUESTED, .window = window, .state = window->state};
            tb_window_push(system, &request);
        }
        break;
    default:
        break;
    }
}

static bool tb_x11_read_events(tb_window_system *system)
{
    tb_x11_system *const native = tb_x11_of(system);
    bool any = false;

    while (native->x.pending(native->display) > 0)
    {
        tb_x11_event event = {0};
        (void)native->x.next_event(native->display, &event);
        tb_x11_handle_event(system, &event);
        any = true;
    }
    if (tb_linux_waker_drain(&native->waker))
    {
        tb_window_woken(system);
        any = true;
    }
    return any;
}

static void tb_x11_pump(tb_window_system *system, uint64_t nanoseconds)
{
    tb_x11_system *const native = tb_x11_of(system);

    (void)native->x.flush(native->display);
    if (tb_x11_read_events(system) || nanoseconds == 0)
    {
        return;
    }
    (void)tb_linux_wait(native->x.connection_number(native->display), native->waker.read_end,
                        nanoseconds);
    (void)tb_x11_read_events(system);
}

static void tb_x11_wake(tb_window_system *system)
{
    tb_linux_waker_wake(&tb_x11_of(system)->waker);
}

static void tb_x11_release(tb_x11_system *native)
{
    if (native->display != NULL)
    {
        (void)native->x.close_display(native->display);
        (void)native->x.set_error_handler(native->previous_handler);
    }
    if (native->waker.read_end >= 0)
    {
        tb_linux_waker_close(&native->waker);
    }
    tb_library_close(native->randr_library);
    tb_library_close(native->xlib);
    tb_free(native);
}

static void tb_x11_close(tb_window_system *system)
{
    tb_x11_release(tb_x11_of(system));
}

static const tb_window_ops tb_x11_ops = {
    .close = tb_x11_close,
    .pump = tb_x11_pump,
    .wake = tb_x11_wake,
    .create = tb_x11_create,
    .destroy = tb_x11_destroy,
    .configure = tb_x11_configure,
    .native = tb_x11_native,
};

static void tb_x11_intern_atoms(tb_x11_system *native)
{
    native->delete_window = native->x.intern_atom(native->display, "WM_DELETE_WINDOW", 0);
    native->wm_state = native->x.intern_atom(native->display, "_NET_WM_STATE", 0);
    native->wm_fullscreen = native->x.intern_atom(native->display, "_NET_WM_STATE_FULLSCREEN", 0);
    native->net_wm_name = native->x.intern_atom(native->display, "_NET_WM_NAME", 0);
    native->utf8_string = native->x.intern_atom(native->display, "UTF8_STRING", 0);
    native->work_area = native->x.intern_atom(native->display, "_NET_WORKAREA", 0);
}

tb_window_result tb_x11_open(tb_window_system *system, tb_window_error *error)
{
    tb_x11_system *const native = tb_calloc(1, sizeof(tb_x11_system));
    tb_window_result result = TB_WINDOW_OK;

    native->waker = (tb_linux_waker){-1, -1};
    result = tb_x11_load(native, error);
    if (result != TB_WINDOW_OK)
    {
        tb_x11_release(native);
        return result;
    }
    native->display = native->x.open_display(NULL);
    if (native->display == NULL)
    {
        tb_x11_release(native);
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE,
                              "no X server: DISPLAY is not set, or the server refused");
    }
    native->previous_handler = native->x.set_error_handler(tb_x11_ignore_error);
    if (!tb_linux_waker_open(&native->waker))
    {
        /* coverage-off: only a process out of descriptors has no pipe */
        tb_x11_release(native);
        return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR, "no pipe to wake the window system");
        /* coverage-on */
    }
    native->screen = native->x.default_screen(native->display);
    native->root = native->x.root_window(native->display, native->screen);
    (void)native->x.detectable_auto_repeat(native->display, 1, NULL);
    tb_x11_intern_atoms(native);
    tb_x11_load_randr(native);
    if (native->has_randr)
    {
        native->randr.select_input(native->display, native->root, TB_RANDR_SCREEN_CHANGE_MASK);
    }
    system->backend = TB_WINDOW_BACKEND_X11;
    system->ops = &tb_x11_ops;
    system->native = native;
    tb_x11_read_displays(system);
    return TB_WINDOW_OK;
}
