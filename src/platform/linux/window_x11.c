/* X11 windowing, from libX11.so.6 and libXrandr.so.2 loaded at run time.
 *
 * The few structures and constants of Xlib that the engine needs are
 * declared here, as Xlib's ABI fixes them, so that building the engine needs
 * no X development headers. A window manager makes a window fullscreen
 * through _NET_WM_STATE; without one, as under a bare X server, the window
 * is also moved and sized to cover its monitor, which is what fullscreen
 * means there.
 *
 * Text comes from Xlib's input method, which composes dead keys and is the
 * user's input method editor where one runs. Relative mouse mode grabs the
 * pointer, hidden and confined to the window, and reads the mouse's raw
 * motion from XInput2, in libXi.so.6, also loaded at run time. */
#include "window_linux.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/utf8.h"

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
    TB_X11_BUTTON_PRESS = 4,
    TB_X11_BUTTON_RELEASE = 5,
    TB_X11_MOTION_NOTIFY = 6,
    TB_X11_ENTER_NOTIFY = 7,
    TB_X11_LEAVE_NOTIFY = 8,
    TB_X11_FOCUS_IN = 9,
    TB_X11_FOCUS_OUT = 10,
    TB_X11_UNMAP_NOTIFY = 18,
    TB_X11_MAP_NOTIFY = 19,
    TB_X11_CONFIGURE_NOTIFY = 22,
    TB_X11_CLIENT_MESSAGE = 33,
    TB_X11_GENERIC_EVENT = 35
};

#define TB_X11_KEY_PRESS_MASK (1L << 0)
#define TB_X11_KEY_RELEASE_MASK (1L << 1)
#define TB_X11_BUTTON_PRESS_MASK (1L << 2)
#define TB_X11_BUTTON_RELEASE_MASK (1L << 3)
#define TB_X11_ENTER_WINDOW_MASK (1L << 4)
#define TB_X11_LEAVE_WINDOW_MASK (1L << 5)
#define TB_X11_POINTER_MOTION_MASK (1L << 6)
/* The modifiers of an event's state. */
#define TB_X11_SHIFT_MASK (1u << 0)
#define TB_X11_CONTROL_MASK (1u << 2)
#define TB_X11_MOD1_MASK (1u << 3) /* Alt */
#define TB_X11_MOD4_MASK (1u << 6) /* Super */
#define TB_X11_GRAB_MODE_ASYNC 1
#define TB_X11_GRAB_SUCCESS 0
#define TB_X11_CURRENT_TIME 0ul
/* What Xutf8LookupString() found. */
#define TB_X11_LOOKUP_CHARS 2
#define TB_X11_LOOKUP_BOTH 4
/* An input context that shows nothing of its own. */
#define TB_X11_PREEDIT_NOTHING 0x0008L
#define TB_X11_STATUS_NOTHING 0x0400L
/* XInput2: every master device, and its raw motion. */
#define TB_XI_ALL_MASTER_DEVICES 1
#define TB_XI_RAW_MOTION 17
#define TB_X11_STRUCTURE_NOTIFY_MASK (1L << 17)
#define TB_X11_SUBSTRUCTURE_NOTIFY_MASK (1L << 19)
#define TB_X11_SUBSTRUCTURE_REDIRECT_MASK (1L << 20)
#define TB_X11_FOCUS_CHANGE_MASK (1L << 21)
#define TB_X11_US_POSITION (1L << 0)
#define TB_X11_US_SIZE (1L << 1)
#define TB_X11_MIN_SIZE (1L << 4)
#define TB_X11_MAX_SIZE (1L << 5)
#define TB_X11_ASPECT (1L << 7)
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

/* XButtonEvent, XMotionEvent and XCrossingEvent begin as a key's does. */
typedef struct tb_x11_button_event
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
    unsigned int button;
    tb_x11_bool same_screen;
} tb_x11_button_event;

typedef struct tb_x11_motion_event
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
    char is_hint;
    tb_x11_bool same_screen;
} tb_x11_motion_event;

typedef struct tb_x11_crossing_event
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
    int mode;
    int detail;
    tb_x11_bool same_screen;
    tb_x11_bool focus;
    unsigned int state;
} tb_x11_crossing_event;

/* XGenericEventCookie: an extension's event, whose data XGetEventData()
 * reads. */
typedef struct tb_x11_cookie
{
    int type;
    unsigned long serial;
    tb_x11_bool send_event;
    tb_x11_display *display;
    int extension;
    int evtype;
    unsigned int cookie;
    void *data;
} tb_x11_cookie;

/* XIRawEvent: a device's motion before the server accelerates it. */
typedef struct tb_xi_raw_event
{
    int type;
    unsigned long serial;
    tb_x11_bool send_event;
    tb_x11_display *display;
    int extension;
    int evtype;
    tb_x11_id time;
    int deviceid;
    int sourceid;
    int detail;
    int flags;
    int mask_length; /* XIValuatorState */
    unsigned char *mask;
    double *values;
    double *raw_values;
} tb_xi_raw_event;

typedef struct tb_xi_event_mask
{
    int deviceid;
    int mask_length;
    unsigned char *mask;
} tb_xi_event_mask;

typedef struct tb_x11_color
{
    unsigned long pixel;
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    char flags;
    char pad;
} tb_x11_color;

typedef struct tb_x11_im tb_x11_im; /* XIM */
typedef struct tb_x11_ic tb_x11_ic; /* XIC */

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
    tb_x11_button_event button;
    tb_x11_motion_event motion;
    tb_x11_crossing_event crossing;
    tb_x11_cookie cookie;
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
    tb_x11_id (*create_font_cursor)(tb_x11_display *display, unsigned int shape);
    int (*define_cursor)(tb_x11_display *display, tb_x11_id window, tb_x11_id cursor);
    int (*free_cursor)(tb_x11_display *display, tb_x11_id cursor);
    tb_x11_id (*create_bitmap_from_data)(tb_x11_display *display, tb_x11_id drawable,
                                         const char *data, unsigned width, unsigned height);
    tb_x11_id (*create_pixmap_cursor)(tb_x11_display *display, tb_x11_id source, tb_x11_id mask,
                                      tb_x11_color *foreground, tb_x11_color *background,
                                      unsigned x, unsigned y);
    int (*free_pixmap)(tb_x11_display *display, tb_x11_id pixmap);
    int (*grab_pointer)(tb_x11_display *display, tb_x11_id window, tb_x11_bool owner_events,
                        unsigned int mask, int pointer_mode, int keyboard_mode,
                        tb_x11_id confine_to, tb_x11_id cursor, tb_x11_id time);
    int (*ungrab_pointer)(tb_x11_display *display, tb_x11_id time);
    tb_x11_bool (*query_extension)(tb_x11_display *display, const char *name, int *opcode,
                                   int *event_base, int *error_base);
    tb_x11_bool (*get_event_data)(tb_x11_display *display, tb_x11_cookie *cookie);
    void (*free_event_data)(tb_x11_display *display, tb_x11_cookie *cookie);
    tb_x11_bool (*filter_event)(tb_x11_event *event, tb_x11_id window);
    char *(*set_locale_modifiers)(const char *modifiers);
    tb_x11_im *(*open_im)(tb_x11_display *display, void *database, char *name, char *class_name);
    int (*close_im)(tb_x11_im *im);
    tb_x11_ic *(*create_ic)(tb_x11_im *im, ...);
    void (*destroy_ic)(tb_x11_ic *ic);
    void (*set_ic_focus)(tb_x11_ic *ic);
    void (*unset_ic_focus)(tb_x11_ic *ic);
    int (*utf8_lookup_string)(tb_x11_ic *ic, tb_x11_key_event *event, char *buffer, int bytes,
                              tb_x11_id *keysym, int *status);
    int (*lookup_string)(tb_x11_key_event *event, char *buffer, int bytes, tb_x11_id *keysym,
                         void *compose);
} tb_x11_api;

/* XInput2, from libXi.so.6, for the mouse's raw motion. */
typedef struct tb_xi_api
{
    int (*query_version)(tb_x11_display *display, int *major, int *minor);
    int (*select_events)(tb_x11_display *display, tb_x11_id window, tb_xi_event_mask *masks,
                         int count);
} tb_xi_api;

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
    tb_library *xi_library;
    tb_xi_api xi;
    bool has_xi; /* XInput2 2.0 or later, for raw motion. */
    int xi_opcode;
    tb_x11_im *im; /* The input method, which composes text; NULL without one. */
    tb_x11_id cursors[TB_CURSOR_COUNT];
    tb_x11_id blank_cursor;
    tb_window *grabbed; /* The window in relative mouse mode that holds the mouse. */
} tb_x11_system;

typedef struct tb_x11_window
{
    tb_x11_id handle;
    tb_x11_ic *ic; /* Its input context; NULL without an input method. */
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
        "XCreateFontCursor",
        "XDefineCursor",
        "XFreeCursor",
        "XCreateBitmapFromData",
        "XCreatePixmapCursor",
        "XFreePixmap",
        "XGrabPointer",
        "XUngrabPointer",
        "XQueryExtension",
        "XGetEventData",
        "XFreeEventData",
        "XFilterEvent",
        "XSetLocaleModifiers",
        "XOpenIM",
        "XCloseIM",
        "XCreateIC",
        "XDestroyIC",
        "XSetICFocus",
        "XUnsetICFocus",
        "Xutf8LookupString",
        "XLookupString",
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

/* XInput2, for relative mouse mode: without it, the mode is unavailable. */
static void tb_x11_load_xi(tb_x11_system *native)
{
    static const char *const names[] = {"XIQueryVersion", "XISelectEvents"};
    enum
    {
        COUNT = sizeof(names) / sizeof(names[0])
    };
    tb_library_function functions[COUNT] = {0};
    tb_library_binding bindings[COUNT] = {0};
    int event_base = 0;
    int error_base = 0;
    int major = 2;
    int minor = 0;

    TB_STATIC_ASSERT(sizeof(tb_xi_api) == COUNT * sizeof(tb_library_function),
                     "the XInput2 table has a function for each name");
    if (!native->x.query_extension(native->display, "XInputExtension", &native->xi_opcode,
                                   &event_base, &error_base) ||
        tb_library_open("libXi.so.6", TB_LIBRARY_SEARCH_SYSTEM, &native->xi_library, NULL) !=
            TB_LIBRARY_OK)
    {
        /* coverage-off: every X server and desktop since 2009 has XInput2 */
        return;
        /* coverage-on */
    }
    for (size_t index = 0; index < COUNT; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = names[index], .function = &functions[index], .required = true};
    }
    if (tb_library_bind(native->xi_library, bindings, COUNT, NULL) != TB_LIBRARY_OK)
    {
        /* coverage-off: every libXi.so.6 exports these */
        return;
        /* coverage-on */
    }
    memcpy(&native->xi, functions, sizeof(native->xi));
    native->has_xi = native->xi.query_version(native->display, &major, &minor) == TB_X11_SUCCESS;
}

/* The input method of the user's locale settings, which composes dead keys
 * and, where one runs, is the input method editor's. */
static void tb_x11_open_im(tb_x11_system *native)
{
    (void)native->x.set_locale_modifiers("");
    native->im = native->x.open_im(native->display, NULL, NULL, NULL);
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

/* A length in logical units in pixels, rounded up so that a limit is never
 * under what it says. */
static int tb_x11_pixels(uint32_t units, double scale)
{
    const double pixels = (double)units * scale;
    const int whole = pixels >= (double)INT32_MAX ? INT32_MAX : (int)pixels;

    return (double)whole < pixels ? whole + 1 : whole;
}

/* WM_NORMAL_HINTS: where the window goes and how large, and, windowed, how
 * the player may size it: not at all, or no smaller than its least size,
 * keeping its aspect when it has one. A window that fills its display has
 * no limits for the window manager to weigh. */
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
    else if (window->state.mode == TB_WINDOW_MODE_WINDOWED)
    {
        uint32_t least_width = 0;
        uint32_t least_height = 0;
        tb_window_least_size(&window->options, &least_width, &least_height);
        hints.flags |= TB_X11_MIN_SIZE;
        hints.min_width = tb_x11_pixels(least_width, native->scale);
        hints.min_height = tb_x11_pixels(least_height, native->scale);
    }
    if (window->state.aspect_locked)
    {
        hints.flags |= TB_X11_ASPECT;
        hints.min_aspect[0] = (int)window->options.aspect_width;
        hints.min_aspect[1] = (int)window->options.aspect_height;
        hints.max_aspect[0] = hints.min_aspect[0];
        hints.max_aspect[1] = hints.min_aspect[1];
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
    (void)native->x.select_input(
        native->display, record->handle,
        TB_X11_KEY_PRESS_MASK | TB_X11_KEY_RELEASE_MASK | TB_X11_BUTTON_PRESS_MASK |
            TB_X11_BUTTON_RELEASE_MASK | TB_X11_ENTER_WINDOW_MASK | TB_X11_LEAVE_WINDOW_MASK |
            TB_X11_POINTER_MOTION_MASK | TB_X11_STRUCTURE_NOTIFY_MASK | TB_X11_FOCUS_CHANGE_MASK);
    if (native->im != NULL)
    {
        record->ic = native->x.create_ic(
            native->im, "inputStyle",
            (unsigned long)(TB_X11_PREEDIT_NOTHING | TB_X11_STATUS_NOTHING), "clientWindow",
            record->handle, "focusWindow", record->handle, (void *)NULL);
    }
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
    tb_x11_window *const record = window->native;

    if (record->ic != NULL)
    {
        native->x.destroy_ic(record->ic);
    }
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

/* The modifiers of an event's state. */
static uint32_t tb_x11_modifiers(unsigned int state)
{
    uint32_t modifiers = 0;

    if ((state & TB_X11_SHIFT_MASK) != 0)
    {
        modifiers |= TB_MODIFIER_SHIFT;
    }
    if ((state & TB_X11_CONTROL_MASK) != 0)
    {
        modifiers |= TB_MODIFIER_CONTROL;
    }
    if ((state & TB_X11_MOD1_MASK) != 0)
    {
        modifiers |= TB_MODIFIER_ALT;
    }
    if ((state & TB_X11_MOD4_MASK) != 0)
    {
        modifiers |= TB_MODIFIER_SUPER;
    }
    return modifiers;
}

/* The text a key press types: from the input method, which composes dead
 * keys, or, without one, from the key's Latin-1 character. */
static void tb_x11_read_text(tb_window *window, tb_x11_key_event *key)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const tb_x11_window *const record = window->native;
    char buffer[64] = {0};
    tb_x11_id keysym = 0;
    int status = 0;
    int length = 0;

    if (record->ic != NULL)
    {
        length = native->x.utf8_lookup_string(record->ic, key, buffer, (int)sizeof(buffer) - 1,
                                              &keysym, &status);
        if ((status == TB_X11_LOOKUP_CHARS || status == TB_X11_LOOKUP_BOTH) && length > 0)
        {
            tb_window_text(window, buffer, (size_t)length);
        }
        return;
    }
    /* coverage-off: Xlib opens its own input method where no other runs */
    length = native->x.lookup_string(key, buffer, (int)sizeof(buffer) - 1, &keysym, NULL);
    for (int index = 0; index < length; index++)
    {
        char character[4] = {0};
        const size_t size =
            tb_utf8_encode((uint32_t)(unsigned char)buffer[index], character, sizeof(character));
        tb_window_text(window, character, size);
    }
    /* coverage-on */
}

static void tb_x11_on_key(tb_window *window, tb_x11_key_event *key)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const uint8_t bit = (uint8_t)(1u << (key->keycode & 7u));
    uint8_t *const held = &native->keys_down[(key->keycode & 0xFFu) >> 3];
    const bool down = key->type == TB_X11_KEY_PRESS;
    const bool repeat = down && (*held & bit) != 0;
    /* X keycodes are evdev's, 8 higher. */
    const uint32_t code = key->keycode >= 8u ? key->keycode - 8u : 0u;

    *held = down ? (uint8_t)(*held | bit) : (uint8_t)(*held & ~bit);
    tb_window_key(window, tb_linux_key(code), code, down, repeat, tb_x11_modifiers(key->state));
    if (down)
    {
        tb_x11_read_text(window, key);
    }
}

/* X's buttons: 1 to 3 are left, middle, and right; 8 and 9 are the side
 * buttons. */
static tb_mouse_button tb_x11_button(unsigned int button)
{
    switch (button)
    {
    case 1:
        return TB_MOUSE_BUTTON_LEFT;
    case 2:
        return TB_MOUSE_BUTTON_MIDDLE;
    case 3:
        return TB_MOUSE_BUTTON_RIGHT;
    case 8:
        return TB_MOUSE_BUTTON_X1;
    case 9:
        return TB_MOUSE_BUTTON_X2;
    default:
        return TB_MOUSE_BUTTON_NONE;
    }
}

/* Buttons 4 to 7 are the wheel's notches: up, down, left, right. */
static void tb_x11_on_button(tb_window *window, const tb_x11_button_event *button)
{
    const double x = (double)button->x;
    const double y = (double)button->y;
    const uint32_t modifiers = tb_x11_modifiers(button->state);
    const bool down = button->type == TB_X11_BUTTON_PRESS;
    double notches_x = 0.0;
    double notches_y = 0.0;

    switch (button->button)
    {
    case 4:
        notches_y = 1.0;
        break;
    case 5:
        notches_y = -1.0;
        break;
    case 6:
        notches_x = -1.0;
        break;
    case 7:
        notches_x = 1.0;
        break;
    default:
        if (tb_x11_button(button->button) != TB_MOUSE_BUTTON_NONE)
        {
            tb_window_mouse_button(window, tb_x11_button(button->button), down, x, y, modifiers);
        }
        return;
    }
    if (down)
    {
        tb_window_mouse_wheel(window, notches_x, notches_y, x, y, modifiers);
    }
}

/* ---------------------------------------------------------------------------
 * The cursor and relative mouse mode.
 * ------------------------------------------------------------------------- */

/* The shapes of the cursor font, which a cursor theme draws where the
 * desktop has one. */
static const unsigned int tb_x11_cursor_shapes[TB_CURSOR_COUNT] = {
    [TB_CURSOR_ARROW] = 68,                /* left_ptr */
    [TB_CURSOR_TEXT] = 152,                /* xterm */
    [TB_CURSOR_HAND] = 60,                 /* hand2 */
    [TB_CURSOR_CROSSHAIR] = 34,            /* crosshair */
    [TB_CURSOR_NOT_ALLOWED] = 24,          /* circle */
    [TB_CURSOR_RESIZE_HORIZONTAL] = 108,   /* sb_h_double_arrow */
    [TB_CURSOR_RESIZE_VERTICAL] = 116,     /* sb_v_double_arrow */
    [TB_CURSOR_RESIZE_DIAGONAL_DOWN] = 14, /* bottom_right_corner */
    [TB_CURSOR_RESIZE_DIAGONAL_UP] = 12,   /* bottom_left_corner */
    [TB_CURSOR_RESIZE_ALL] = 52,           /* fleur */
};

/* A cursor of nothing: one transparent pixel. */
static tb_x11_id tb_x11_blank_cursor(tb_x11_system *native)
{
    static const char empty[1] = {0};
    tb_x11_color black = {0};
    tb_x11_id pixmap = 0;

    if (native->blank_cursor != 0)
    {
        return native->blank_cursor;
    }
    pixmap = native->x.create_bitmap_from_data(native->display, native->root, empty, 1, 1);
    native->blank_cursor =
        native->x.create_pixmap_cursor(native->display, pixmap, pixmap, &black, &black, 0, 0);
    (void)native->x.free_pixmap(native->display, pixmap);
    return native->blank_cursor;
}

static tb_x11_id tb_x11_cursor(tb_x11_system *native, tb_cursor shape)
{
    if (native->cursors[shape] == 0)
    {
        native->cursors[shape] =
            native->x.create_font_cursor(native->display, tb_x11_cursor_shapes[shape]);
    }
    return native->cursors[shape];
}

static void tb_x11_update_cursor(tb_window *window)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const bool hidden = window->cursor_hidden || window->state.relative_mouse;
    const tb_x11_id cursor =
        hidden ? tb_x11_blank_cursor(native) : tb_x11_cursor(native, window->cursor);

    (void)native->x.define_cursor(native->display, tb_x11_handle(window), cursor);
    (void)native->x.flush(native->display);
}

/* Hold the mouse in the window, hidden; false when another program holds
 * it, or the window is not on the screen. */
static bool tb_x11_grab(tb_window *window)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    const tb_x11_id handle = tb_x11_handle(window);
    const unsigned int mask = (unsigned int)(TB_X11_BUTTON_PRESS_MASK | TB_X11_BUTTON_RELEASE_MASK |
                                             TB_X11_POINTER_MOTION_MASK);

    if (native->x.grab_pointer(native->display, handle, 1, mask, TB_X11_GRAB_MODE_ASYNC,
                               TB_X11_GRAB_MODE_ASYNC, handle, tb_x11_blank_cursor(native),
                               TB_X11_CURRENT_TIME) != TB_X11_GRAB_SUCCESS)
    {
        return false;
    }
    native->grabbed = window;
    return true;
}

static void tb_x11_ungrab(tb_x11_system *native)
{
    if (native->grabbed == NULL)
    {
        return;
    }
    (void)native->x.ungrab_pointer(native->display, TB_X11_CURRENT_TIME);
    (void)native->x.flush(native->display);
    native->grabbed = NULL;
}

/* Ask for the raw motion of every mouse, which XInput2 sends to the root
 * window only; or stop. */
static void tb_x11_select_raw_motion(tb_x11_system *native, bool selected)
{
    unsigned char bits[4] = {0};
    tb_xi_event_mask mask = {
        .deviceid = TB_XI_ALL_MASTER_DEVICES, .mask_length = (int)sizeof(bits), .mask = bits};

    if (selected)
    {
        bits[TB_XI_RAW_MOTION >> 3] = (unsigned char)(1u << (TB_XI_RAW_MOTION & 7));
    }
    (void)native->xi.select_events(native->display, native->root, &mask, 1);
    (void)native->x.flush(native->display);
}

static tb_window_result tb_x11_set_relative_mouse(tb_window *window, bool relative,
                                                  tb_window_error *error)
{
    tb_x11_system *const native = tb_x11_of(window->system);

    if (!relative)
    {
        if (native->grabbed == window)
        {
            tb_x11_ungrab(native);
        }
        if (native->has_xi)
        {
            tb_x11_select_raw_motion(native, false);
        }
        return TB_WINDOW_OK;
    }
    if (!native->has_xi)
    {
        /* coverage-off: every X server and desktop since 2009 has XInput2 */
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE,
                              "the X server has no XInput2 for the mouse's motion");
        /* coverage-on */
    }
    if (!tb_x11_grab(window))
    {
        return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR,
                              "the X server would not give the window the mouse: another "
                              "program holds it, or the window is not on the screen");
    }
    tb_x11_select_raw_motion(native, true);
    return TB_WINDOW_OK;
}

/* XInput2's raw motion goes to the window that holds the mouse. */
static void tb_x11_on_generic(tb_window_system *system, tb_x11_event *event)
{
    tb_x11_system *const native = tb_x11_of(system);
    tb_x11_cookie *const cookie = &event->cookie;
    double delta_x = 0.0;
    double delta_y = 0.0;

    if (!native->has_xi || cookie->extension != native->xi_opcode ||
        cookie->evtype != TB_XI_RAW_MOTION || !native->x.get_event_data(native->display, cookie))
    {
        /* coverage-off: no other extension's events are asked for */
        return;
        /* coverage-on */
    }
    if (native->grabbed != NULL)
    {
        const tb_xi_raw_event *const raw = cookie->data;
        tb_x11_raw_motion(raw->mask, raw->mask_length, raw->raw_values, &delta_x, &delta_y);
        if (delta_x != 0.0 || delta_y != 0.0)
        {
            tb_window_mouse_relative(native->grabbed, delta_x, delta_y);
        }
    }
    native->x.free_event_data(native->display, cookie);
}

void tb_x11_raw_motion(const unsigned char *mask, int mask_length, const double *raw_values,
                       double *delta_x, double *delta_y)
{
    const int valuators = mask_length > 0 ? mask_length * 8 : 0;
    size_t value = 0;

    *delta_x = 0.0;
    *delta_y = 0.0;
    /* Valuator 0 is x and 1 is y; the values are those of the set bits, in
     * order. */
    for (int valuator = 0; valuator < valuators && valuator < 2; valuator++)
    {
        if ((mask[valuator >> 3] & (1u << (valuator & 7))) == 0)
        {
            continue;
        }
        if (valuator == 0)
        {
            *delta_x = raw_values[value];
        }
        else
        {
            *delta_y = raw_values[value];
        }
        value++;
    }
}

/* Focus: the input context composes for the window with the keyboard, and
 * a window in relative mouse mode holds the mouse while it has it. */
static void tb_x11_on_focus(tb_window *window, bool focused)
{
    tb_x11_system *const native = tb_x11_of(window->system);
    tb_x11_window *const record = window->native;

    tb_window_focused(window, focused);
    if (record->ic != NULL)
    {
        if (focused)
        {
            native->x.set_ic_focus(record->ic);
        }
        else
        {
            native->x.unset_ic_focus(record->ic);
        }
    }
    if (!window->state.relative_mouse)
    {
        return;
    }
    if (focused)
    {
        (void)tb_x11_grab(window);
    }
    else if (native->grabbed == window)
    {
        tb_x11_ungrab(native);
    }
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
    if (event->type == TB_X11_GENERIC_EVENT)
    {
        tb_x11_on_generic(system, event);
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
    case TB_X11_BUTTON_PRESS:
    case TB_X11_BUTTON_RELEASE:
        tb_x11_on_button(window, &event->button);
        break;
    case TB_X11_MOTION_NOTIFY:
        tb_window_mouse_moved(window, (double)event->motion.x, (double)event->motion.y,
                              tb_x11_modifiers(event->motion.state));
        break;
    case TB_X11_ENTER_NOTIFY:
        tb_window_mouse_crossed(window, true, (double)event->crossing.x, (double)event->crossing.y);
        break;
    case TB_X11_LEAVE_NOTIFY:
        tb_window_mouse_crossed(window, false, 0.0, 0.0);
        break;
    case TB_X11_FOCUS_IN:
        tb_x11_on_focus(window, true);
        break;
    case TB_X11_FOCUS_OUT:
        tb_x11_on_focus(window, false);
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
        any = true;
        /* What the input method takes, a dead key, is its own. */
        if (native->x.filter_event(&event, 0))
        {
            continue;
        }
        tb_x11_handle_event(system, &event);
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
        for (size_t index = 0; index < TB_CURSOR_COUNT; index++)
        {
            if (native->cursors[index] != 0)
            {
                (void)native->x.free_cursor(native->display, native->cursors[index]);
            }
        }
        if (native->blank_cursor != 0)
        {
            (void)native->x.free_cursor(native->display, native->blank_cursor);
        }
        if (native->im != NULL)
        {
            (void)native->x.close_im(native->im);
        }
        (void)native->x.close_display(native->display);
        (void)native->x.set_error_handler(native->previous_handler);
    }
    if (native->waker.read_end >= 0)
    {
        tb_linux_waker_close(&native->waker);
    }
    tb_library_close(native->xi_library);
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
    .confirm = tb_linux_confirm,
    .set_relative_mouse = tb_x11_set_relative_mouse,
    .update_cursor = tb_x11_update_cursor,
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
    tb_x11_load_xi(native);
    tb_x11_open_im(native);
    system->backend = TB_WINDOW_BACKEND_X11;
    system->pointer_in_pixels = true;
    system->keeps_aspect = true;
    system->ops = &tb_x11_ops;
    system->native = native;
    tb_x11_read_displays(system);
    return TB_WINDOW_OK;
}
