/* macOS windowing: Cocoa, reached from C through the Objective-C runtime,
 * with libobjc, AppKit, and CoreGraphics loaded at run time.
 *
 * A message to an object is objc_msgSend() cast to the method's own C type.
 * On x86_64 a method that returns a structure larger than two registers,
 * such as NSRect, is sent with objc_msgSend_stret() instead; arm64 has no
 * such variant. Two classes are made at run time: a window that may become
 * the key window without a frame, which a borderless window needs to
 * receive keys, and a delegate that turns a window's notifications into
 * events.
 *
 * Cocoa's coordinates rise from the bottom of the primary display; the
 * engine's displays and windows are placed from its top, so y is flipped at
 * the boundary. Fullscreen is borderless: a frameless window over its
 * display, above the menu bar, with the Dock and menu bar hidden. */
#include "../shared/window.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/library.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>

typedef void *tb_id;
typedef void *tb_sel;
typedef void (*tb_imp)(void);

typedef struct tb_cocoa_point
{
    double x;
    double y;
} tb_cocoa_point;

typedef struct tb_cocoa_size
{
    double width;
    double height;
} tb_cocoa_size;

typedef struct tb_cocoa_rect
{
    tb_cocoa_point origin;
    tb_cocoa_size size;
} tb_cocoa_rect;

/* NSWindowStyleMask. */
#define TB_COCOA_TITLED 1ul
#define TB_COCOA_CLOSABLE 2ul
#define TB_COCOA_MINIATURIZABLE 4ul
#define TB_COCOA_RESIZABLE 8ul
#define TB_COCOA_BORDERLESS 0ul
#define TB_COCOA_BACKING_BUFFERED 2ul

/* NSEventType, NSEventModifierFlags and their device-specific bits. */
#define TB_COCOA_KEY_DOWN 10ul
#define TB_COCOA_KEY_UP 11ul
#define TB_COCOA_FLAGS_CHANGED 12ul
#define TB_COCOA_APPLICATION_DEFINED 15ul
#define TB_COCOA_COMMAND (1ul << 20)
#define TB_COCOA_LEFT_CONTROL 0x1ul
#define TB_COCOA_LEFT_SHIFT 0x2ul
#define TB_COCOA_RIGHT_SHIFT 0x4ul
#define TB_COCOA_LEFT_ALT 0x20ul
#define TB_COCOA_RIGHT_ALT 0x40ul
#define TB_COCOA_RIGHT_CONTROL 0x2000ul

/* The subtype of the event that wakes the window system. */
#define TB_COCOA_WAKE_SUBTYPE 0x7462

/* Window levels and presentation options. */
#define TB_COCOA_NORMAL_LEVEL 0l
#define TB_COCOA_ABOVE_MENU_BAR_LEVEL 25l
#define TB_COCOA_HIDE_DOCK_AND_MENU_BAR ((1ul << 1) | (1ul << 3))
#define TB_COCOA_ACTIVATION_REGULAR 0l

/* The key code of Q, for Command+Q. */
#define TB_COCOA_KEY_Q 12u

/* ---------------------------------------------------------------------------
 * The runtime, loaded once for the process: the classes made here point at
 * this library's functions for as long as the process runs.
 * ------------------------------------------------------------------------- */

typedef struct tb_cocoa_runtime
{
    bool loaded;
    tb_library *objc;
    tb_library *appkit;
    tb_library *graphics;
    tb_id (*get_class)(const char *name);
    tb_sel (*register_name)(const char *name);
    tb_imp send;
    tb_imp send_stret; /* x86_64 only. */
    tb_id (*allocate_class)(tb_id superclass, const char *name, size_t extra);
    void (*register_class)(tb_id class_object);
    bool (*add_method)(tb_id class_object, tb_sel selector, tb_imp implementation,
                       const char *types);
    bool (*add_ivar)(tb_id class_object, const char *name, size_t size, uint8_t alignment,
                     const char *types);
    tb_id (*get_ivar)(tb_id class_object, const char *name);
    ptrdiff_t (*ivar_offset)(tb_id ivar);
    void *(*pool_push)(void);
    void (*pool_pop)(void *pool);
    tb_cocoa_size (*screen_size)(uint32_t display);
    tb_id (*copy_mode)(uint32_t display);
    double (*mode_refresh)(tb_id mode);
    size_t (*mode_pixel_width)(tb_id mode);
    size_t (*mode_pixel_height)(tb_id mode);
    void (*release_mode)(tb_id mode);
    tb_id default_mode;      /* NSDefaultRunLoopMode */
    tb_id screens_changed;   /* NSApplicationDidChangeScreenParametersNotification */
    tb_id window_class;      /* TBCocoaWindow */
    tb_id delegate_class;    /* TBCocoaDelegate */
    ptrdiff_t target_offset; /* Of the delegate's pointer to its window or system. */
    tb_id application;
} tb_cocoa_runtime;

static tb_cocoa_runtime tb_cocoa = {0};

static tb_sel tb_sel_of(const char *name)
{
    return tb_cocoa.register_name(name);
}

static tb_id tb_send(tb_id receiver, const char *selector)
{
    return ((tb_id (*)(tb_id, tb_sel))tb_cocoa.send)(receiver, tb_sel_of(selector));
}

static tb_id tb_send_id(tb_id receiver, const char *selector, tb_id argument)
{
    return ((tb_id (*)(tb_id, tb_sel, tb_id))tb_cocoa.send)(receiver, tb_sel_of(selector),
                                                            argument);
}

static void tb_send_bool(tb_id receiver, const char *selector, bool argument)
{
    ((void (*)(tb_id, tb_sel, bool))tb_cocoa.send)(receiver, tb_sel_of(selector), argument);
}

static void tb_send_unsigned(tb_id receiver, const char *selector, unsigned long argument)
{
    ((void (*)(tb_id, tb_sel, unsigned long))tb_cocoa.send)(receiver, tb_sel_of(selector),
                                                            argument);
}

static void tb_send_signed(tb_id receiver, const char *selector, long argument)
{
    ((void (*)(tb_id, tb_sel, long))tb_cocoa.send)(receiver, tb_sel_of(selector), argument);
}

static unsigned long tb_get_unsigned(tb_id receiver, const char *selector)
{
    return ((unsigned long (*)(tb_id, tb_sel))tb_cocoa.send)(receiver, tb_sel_of(selector));
}

static bool tb_get_bool(tb_id receiver, const char *selector)
{
    return ((bool (*)(tb_id, tb_sel))tb_cocoa.send)(receiver, tb_sel_of(selector));
}

static double tb_get_double(tb_id receiver, const char *selector)
{
    return ((double (*)(tb_id, tb_sel))tb_cocoa.send)(receiver, tb_sel_of(selector));
}

static tb_cocoa_rect tb_get_rect(tb_id receiver, const char *selector)
{
#if defined(__x86_64__)
    tb_cocoa_rect rect = {0};
    ((void (*)(tb_cocoa_rect *, tb_id, tb_sel))tb_cocoa.send_stret)(&rect, receiver,
                                                                    tb_sel_of(selector));
    return rect;
#else
    return ((tb_cocoa_rect (*)(tb_id, tb_sel))tb_cocoa.send)(receiver, tb_sel_of(selector));
#endif
}

static tb_cocoa_rect tb_get_rect_of(tb_id receiver, const char *selector, tb_cocoa_rect argument)
{
#if defined(__x86_64__)
    tb_cocoa_rect rect = {0};
    ((void (*)(tb_cocoa_rect *, tb_id, tb_sel, tb_cocoa_rect))tb_cocoa.send_stret)(
        &rect, receiver, tb_sel_of(selector), argument);
    return rect;
#else
    return ((tb_cocoa_rect (*)(tb_id, tb_sel, tb_cocoa_rect))tb_cocoa.send)(
        receiver, tb_sel_of(selector), argument);
#endif
}

static tb_id tb_class(const char *name)
{
    return tb_cocoa.get_class(name);
}

static tb_id tb_object_at(tb_id array, size_t index)
{
    return ((tb_id (*)(tb_id, tb_sel, unsigned long))tb_cocoa.send)(
        array, tb_sel_of("objectAtIndex:"), (unsigned long)index);
}

/* A new NSString of UTF-8, autoreleased. */
static tb_id tb_string(const char *text)
{
    return ((tb_id (*)(tb_id, tb_sel, const char *))tb_cocoa.send)(
        tb_class("NSString"), tb_sel_of("stringWithUTF8String:"), text);
}

/* ---------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */

typedef struct tb_cocoa_system
{
    tb_id observer; /* A delegate whose target is the system, for display changes. */
} tb_cocoa_system;

typedef struct tb_cocoa_window
{
    tb_id handle;
    tb_id delegate;
} tb_cocoa_window;

static tb_cocoa_system *tb_cocoa_of(const tb_window_system *system)
{
    return system->native;
}

static tb_cocoa_window *tb_cocoa_window_of(const tb_window *window)
{
    return window->native;
}

static void *tb_cocoa_target(tb_id delegate)
{
    return *(void **)((char *)delegate + tb_cocoa.target_offset);
}

static void tb_cocoa_set_target(tb_id delegate, void *target)
{
    *(void **)((char *)delegate + tb_cocoa.target_offset) = target;
}

/* ---------------------------------------------------------------------------
 * Displays.
 * ------------------------------------------------------------------------- */

static double tb_cocoa_primary_height(void)
{
    const tb_id screens = tb_send(tb_class("NSScreen"), "screens");

    if (tb_get_unsigned(screens, "count") == 0)
    {
        /* coverage-off: a Mac always has a screen, even without a monitor */
        return 0.0;
        /* coverage-on */
    }
    return tb_get_rect(tb_object_at(screens, 0), "frame").size.height;
}

/* A rectangle of Cocoa's coordinates, from the bottom, as the engine places
 * them, from the top. */
static void tb_cocoa_flip(tb_cocoa_rect rect, double primary_height, int32_t *x, int32_t *y)
{
    *x = (int32_t)rect.origin.x;
    *y = (int32_t)(primary_height - (rect.origin.y + rect.size.height));
}

static void tb_cocoa_describe_screen(tb_id screen, double primary_height, tb_display *display)
{
    const tb_cocoa_rect frame = tb_get_rect(screen, "frame");
    const tb_cocoa_rect visible = tb_get_rect(screen, "visibleFrame");
    const tb_id description = tb_send(screen, "deviceDescription");
    const tb_id number = tb_send_id(description, "objectForKey:", tb_string("NSScreenNumber"));
    const uint32_t identifier = (uint32_t)tb_get_unsigned(number, "unsignedIntValue");
    tb_id mode = NULL;

    tb_cocoa_flip(frame, primary_height, &display->x, &display->y);
    tb_cocoa_flip(visible, primary_height, &display->usable_x, &display->usable_y);
    display->width = (uint32_t)frame.size.width;
    display->height = (uint32_t)frame.size.height;
    display->usable_width = (uint32_t)visible.size.width;
    display->usable_height = (uint32_t)visible.size.height;
    display->content_scale = tb_get_double(screen, "backingScaleFactor");
    display->pixel_width = (uint32_t)(frame.size.width * display->content_scale);
    display->pixel_height = (uint32_t)(frame.size.height * display->content_scale);
    if (((bool (*)(tb_id, tb_sel, tb_sel))tb_cocoa.send)(screen, tb_sel_of("respondsToSelector:"),
                                                         tb_sel_of("localizedName")))
    {
        const char *const name = ((const char *(*)(tb_id, tb_sel))tb_cocoa.send)(
            tb_send(screen, "localizedName"), tb_sel_of("UTF8String"));
        (void)tb_fmt_buffer(display->name, sizeof(display->name), "%s", name != NULL ? name : "");
    }
    {
        const tb_cocoa_size size = tb_cocoa.screen_size(identifier);
        display->physical_width_mm = (uint32_t)size.width;
        display->physical_height_mm = (uint32_t)size.height;
    }
    mode = tb_cocoa.copy_mode(identifier);
    if (mode != NULL)
    {
        display->refresh_millihertz = (uint32_t)(tb_cocoa.mode_refresh(mode) * 1000.0 + 0.5);
        display->pixel_width = (uint32_t)tb_cocoa.mode_pixel_width(mode);
        display->pixel_height = (uint32_t)tb_cocoa.mode_pixel_height(mode);
        tb_cocoa.release_mode(mode);
    }
}

static void tb_cocoa_read_displays(tb_window_system *system)
{
    void *const pool = tb_cocoa.pool_push();
    const tb_id screens = tb_send(tb_class("NSScreen"), "screens");
    const size_t count = (size_t)tb_get_unsigned(screens, "count");
    const double primary_height = tb_cocoa_primary_height();
    tb_display *const displays = tb_calloc(count > 0 ? count : 1, sizeof(tb_display));

    for (size_t index = 0; index < count; index++)
    {
        const tb_id screen = tb_object_at(screens, index);
        tb_cocoa_describe_screen(screen, primary_height, &displays[index]);
        /* The first screen is the one with the menu bar: the primary. */
        displays[index].primary = index == 0;
    }
    tb_window_set_displays(system, displays, count);
    tb_free(displays);
    tb_cocoa.pool_pop(pool);
}

/* ---------------------------------------------------------------------------
 * A window's size and place.
 * ------------------------------------------------------------------------- */

static void tb_cocoa_read_size(tb_window *window)
{
    const tb_id handle = tb_cocoa_window_of(window)->handle;
    const tb_cocoa_rect content = tb_get_rect(tb_send(handle, "contentView"), "frame");
    const tb_cocoa_rect frame = tb_get_rect(handle, "frame");
    const double scale = tb_get_double(handle, "backingScaleFactor");
    int32_t x = 0;
    int32_t y = 0;

    if (content.size.width < 1.0 || content.size.height < 1.0)
    {
        return;
    }
    tb_window_resized(window, (uint32_t)(content.size.width * scale + 0.5),
                      (uint32_t)(content.size.height * scale + 0.5), scale > 0.0 ? scale : 1.0);
    tb_cocoa_flip(frame, tb_cocoa_primary_height(), &x, &y);
    window->state.display = tb_window_display_at(window->system, x, y, (uint32_t)frame.size.width,
                                                 (uint32_t)frame.size.height);
}

static unsigned long tb_cocoa_windowed_style(const tb_window_options *options)
{
    const unsigned long style = TB_COCOA_TITLED | TB_COCOA_CLOSABLE | TB_COCOA_MINIATURIZABLE;

    return options->fixed_size ? style : style | TB_COCOA_RESIZABLE;
}

/* The content rectangle, in Cocoa's coordinates, that the window's options
 * place on its display. */
static tb_cocoa_rect tb_cocoa_content_rect(const tb_window *window)
{
    tb_window_placement placement = {0};

    tb_window_place(window, false, &placement);
    return (tb_cocoa_rect){
        .origin = {(double)placement.x,
                   tb_cocoa_primary_height() - (double)placement.y - (double)placement.unit_height},
        .size = {(double)placement.unit_width, (double)placement.unit_height}};
}

static void tb_cocoa_set_frame(tb_id handle, tb_cocoa_rect frame)
{
    ((void (*)(tb_id, tb_sel, tb_cocoa_rect, bool))tb_cocoa.send)(
        handle, tb_sel_of("setFrame:display:"), frame, true);
}

static void tb_cocoa_apply(tb_window *window)
{
    const tb_id handle = tb_cocoa_window_of(window)->handle;
    void *const pool = tb_cocoa.pool_push();

    if (window->options.mode == TB_WINDOW_MODE_WINDOWED)
    {
        const unsigned long style = tb_cocoa_windowed_style(&window->options);
        window->state.mode = TB_WINDOW_MODE_WINDOWED;
        tb_send_unsigned(tb_cocoa.application, "setPresentationOptions:", 0);
        tb_send_unsigned(handle, "setStyleMask:", style);
        tb_send_signed(handle, "setLevel:", TB_COCOA_NORMAL_LEVEL);
        tb_cocoa_set_frame(handle, tb_get_rect_of(handle, "frameRectForContentRect:",
                                                  tb_cocoa_content_rect(window)));
    }
    else
    {
        tb_window_placement placement = {0};
        const tb_id screens = tb_send(tb_class("NSScreen"), "screens");
        tb_window_place(window, false, &placement);
        window->state.mode = TB_WINDOW_MODE_BORDERLESS;
        tb_send_unsigned(handle, "setStyleMask:", TB_COCOA_BORDERLESS);
        tb_send_signed(handle, "setLevel:", TB_COCOA_ABOVE_MENU_BAR_LEVEL);
        if (placement.display == 0)
        {
            tb_send_unsigned(tb_cocoa.application,
                             "setPresentationOptions:", TB_COCOA_HIDE_DOCK_AND_MENU_BAR);
        }
        tb_cocoa_set_frame(handle, tb_get_rect(tb_object_at(screens, placement.display), "frame"));
    }
    tb_send_id(handle, "setTitle:", tb_string(window->options.title));
    tb_cocoa.pool_pop(pool);
    tb_cocoa_read_size(window);
}

/* ---------------------------------------------------------------------------
 * The delegate's methods.
 * ------------------------------------------------------------------------- */

static bool tb_cocoa_should_close(tb_id self, tb_sel command, tb_id sender)
{
    tb_window *const window = tb_cocoa_target(self);
    const tb_window_event event = {
        .type = TB_WINDOW_EVENT_CLOSE_REQUESTED, .window = window, .state = window->state};

    (void)command;
    (void)sender;
    tb_window_push(window->system, &event);
    return false;
}

static void tb_cocoa_did_resize(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_cocoa_read_size(tb_cocoa_target(self));
}

static void tb_cocoa_did_become_key(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_window_focused(tb_cocoa_target(self), true);
}

static void tb_cocoa_did_resign_key(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_window_focused(tb_cocoa_target(self), false);
}

static void tb_cocoa_did_miniaturize(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_window_minimized(tb_cocoa_target(self), true);
}

static void tb_cocoa_did_deminiaturize(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_window_minimized(tb_cocoa_target(self), false);
}

static void tb_cocoa_screens_changed(tb_id self, tb_sel command, tb_id notification)
{
    tb_window_system *const system = tb_cocoa_target(self);

    (void)command;
    (void)notification;
    tb_cocoa_read_displays(system);
    tb_window_displays_changed(system);
}

static bool tb_cocoa_yes(tb_id self, tb_sel command)
{
    (void)self;
    (void)command;
    return true;
}

/* ---------------------------------------------------------------------------
 * Loading the runtime and making the classes.
 * ------------------------------------------------------------------------- */

static void tb_cocoa_add(tb_id class_object, const char *name, tb_imp implementation,
                         const char *types)
{
    (void)tb_cocoa.add_method(class_object, tb_sel_of(name), implementation, types);
}

static void tb_cocoa_make_classes(void)
{
    tb_id window_class = tb_cocoa.get_class("TBCocoaWindow");
    tb_id delegate_class = tb_cocoa.get_class("TBCocoaDelegate");

    if (window_class == NULL)
    {
        window_class = tb_cocoa.allocate_class(tb_class("NSWindow"), "TBCocoaWindow", 0);
        tb_cocoa_add(window_class, "canBecomeKeyWindow", (tb_imp)tb_cocoa_yes, "c@:");
        tb_cocoa_add(window_class, "canBecomeMainWindow", (tb_imp)tb_cocoa_yes, "c@:");
        tb_cocoa.register_class(window_class);
    }
    if (delegate_class == NULL)
    {
        delegate_class = tb_cocoa.allocate_class(tb_class("NSObject"), "TBCocoaDelegate", 0);
        (void)tb_cocoa.add_ivar(delegate_class, "tb_target", sizeof(void *), 3, "^v");
        tb_cocoa_add(delegate_class, "windowShouldClose:", (tb_imp)tb_cocoa_should_close, "c@:@");
        tb_cocoa_add(delegate_class, "windowDidResize:", (tb_imp)tb_cocoa_did_resize, "v@:@");
        tb_cocoa_add(delegate_class, "windowDidMove:", (tb_imp)tb_cocoa_did_resize, "v@:@");
        tb_cocoa_add(delegate_class,
                     "windowDidChangeBackingProperties:", (tb_imp)tb_cocoa_did_resize, "v@:@");
        tb_cocoa_add(delegate_class, "windowDidBecomeKey:", (tb_imp)tb_cocoa_did_become_key,
                     "v@:@");
        tb_cocoa_add(delegate_class, "windowDidResignKey:", (tb_imp)tb_cocoa_did_resign_key,
                     "v@:@");
        tb_cocoa_add(delegate_class, "windowDidMiniaturize:", (tb_imp)tb_cocoa_did_miniaturize,
                     "v@:@");
        tb_cocoa_add(delegate_class, "windowDidDeminiaturize:", (tb_imp)tb_cocoa_did_deminiaturize,
                     "v@:@");
        tb_cocoa_add(delegate_class, "screensChanged:", (tb_imp)tb_cocoa_screens_changed, "v@:@");
        tb_cocoa.register_class(delegate_class);
    }
    tb_cocoa.window_class = window_class;
    tb_cocoa.delegate_class = delegate_class;
    tb_cocoa.target_offset = tb_cocoa.ivar_offset(tb_cocoa.get_ivar(delegate_class, "tb_target"));
}

static bool tb_cocoa_bind(const tb_library *library, const char *const *names,
                          tb_library_function *functions, size_t count, tb_library_error *error)
{
    tb_library_binding bindings[16] = {0};

    TB_ASSERT(count <= 16);
    for (size_t index = 0; index < count; index++)
    {
        bindings[index] = (tb_library_binding){
            .name = names[index], .function = &functions[index], .required = true};
    }
    return tb_library_bind(library, bindings, count, error) == TB_LIBRARY_OK;
}

static tb_window_result tb_cocoa_load(tb_window_error *error)
{
    static const char *const objc_names[] = {
        "objc_getClass",
        "sel_registerName",
        "objc_msgSend",
        "objc_allocateClassPair",
        "objc_registerClassPair",
        "class_addMethod",
        "class_addIvar",
        "class_getInstanceVariable",
        "ivar_getOffset",
        "objc_autoreleasePoolPush",
        "objc_autoreleasePoolPop",
    };
    static const char *const graphics_names[] = {
        "CGDisplayScreenSize",        "CGDisplayCopyDisplayMode",    "CGDisplayModeGetRefreshRate",
        "CGDisplayModeGetPixelWidth", "CGDisplayModeGetPixelHeight", "CGDisplayModeRelease",
    };
    tb_library_function objc[11] = {0};
    tb_library_function graphics[6] = {0};
    tb_library_error library_error = TB_LIBRARY_ERROR_INIT;
    void *data = NULL;

    if (tb_cocoa.loaded)
    {
        return TB_WINDOW_OK;
    }
    if (tb_library_open("libobjc.A.dylib", TB_LIBRARY_SEARCH_SYSTEM, &tb_cocoa.objc,
                        &library_error) != TB_LIBRARY_OK ||
        tb_library_open("AppKit.framework/AppKit", TB_LIBRARY_SEARCH_SYSTEM, &tb_cocoa.appkit,
                        &library_error) != TB_LIBRARY_OK ||
        tb_library_open("CoreGraphics.framework/CoreGraphics", TB_LIBRARY_SEARCH_SYSTEM,
                        &tb_cocoa.graphics, &library_error) != TB_LIBRARY_OK ||
        !tb_cocoa_bind(tb_cocoa.objc, objc_names, objc, 11, &library_error) ||
        !tb_cocoa_bind(tb_cocoa.graphics, graphics_names, graphics, 6, &library_error))
    {
        /* coverage-off: every macOS has the Objective-C runtime, AppKit, and CoreGraphics */
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "no Cocoa: %s", library_error.message);
        /* coverage-on */
    }
    tb_cocoa.get_class = (tb_id (*)(const char *))objc[0];
    tb_cocoa.register_name = (tb_sel (*)(const char *))objc[1];
    tb_cocoa.send = objc[2];
    tb_cocoa.allocate_class = (tb_id (*)(tb_id, const char *, size_t))objc[3];
    tb_cocoa.register_class = (void (*)(tb_id))objc[4];
    tb_cocoa.add_method = (bool (*)(tb_id, tb_sel, tb_imp, const char *))objc[5];
    tb_cocoa.add_ivar = (bool (*)(tb_id, const char *, size_t, uint8_t, const char *))objc[6];
    tb_cocoa.get_ivar = (tb_id (*)(tb_id, const char *))objc[7];
    tb_cocoa.ivar_offset = (ptrdiff_t (*)(tb_id))objc[8];
    tb_cocoa.pool_push = (void *(*)(void))objc[9];
    tb_cocoa.pool_pop = (void (*)(void *))objc[10];
    tb_cocoa.screen_size = (tb_cocoa_size (*)(uint32_t))graphics[0];
    tb_cocoa.copy_mode = (tb_id (*)(uint32_t))graphics[1];
    tb_cocoa.mode_refresh = (double (*)(tb_id))graphics[2];
    tb_cocoa.mode_pixel_width = (size_t (*)(tb_id))graphics[3];
    tb_cocoa.mode_pixel_height = (size_t (*)(tb_id))graphics[4];
    tb_cocoa.release_mode = (void (*)(tb_id))graphics[5];
#if defined(__x86_64__)
    if (tb_library_find_function(tb_cocoa.objc, "objc_msgSend_stret", &tb_cocoa.send_stret, NULL) !=
        TB_LIBRARY_OK)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "no objc_msgSend_stret");
    }
#endif
    if (tb_library_find_data(tb_cocoa.appkit, "NSDefaultRunLoopMode", &data, &library_error) !=
        TB_LIBRARY_OK)
    {
        /* coverage-off: AppKit links Foundation, which exports it */
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s", library_error.message);
        /* coverage-on */
    }
    tb_cocoa.default_mode = *(tb_id *)data;
    if (tb_library_find_data(tb_cocoa.appkit, "NSApplicationDidChangeScreenParametersNotification",
                             &data, &library_error) != TB_LIBRARY_OK)
    {
        /* coverage-off: AppKit exports it */
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s", library_error.message);
        /* coverage-on */
    }
    tb_cocoa.screens_changed = *(tb_id *)data;
    tb_cocoa_make_classes();
    tb_cocoa.loaded = true;
    return TB_WINDOW_OK;
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

typedef struct tb_cocoa_key_row
{
    uint16_t code;
    tb_key key;
} tb_cocoa_key_row;

/* Carbon's kVK_ codes, which NSEvent's keyCode gives. */
static const tb_cocoa_key_row tb_cocoa_keys[] = {
    {0, TB_KEY_A},
    {1, TB_KEY_S},
    {2, TB_KEY_D},
    {3, TB_KEY_F},
    {4, TB_KEY_H},
    {5, TB_KEY_G},
    {6, TB_KEY_Z},
    {7, TB_KEY_X},
    {8, TB_KEY_C},
    {9, TB_KEY_V},
    {11, TB_KEY_B},
    {12, TB_KEY_Q},
    {13, TB_KEY_W},
    {14, TB_KEY_E},
    {15, TB_KEY_R},
    {16, TB_KEY_Y},
    {17, TB_KEY_T},
    {18, TB_KEY_1},
    {19, TB_KEY_2},
    {20, TB_KEY_3},
    {21, TB_KEY_4},
    {22, TB_KEY_6},
    {23, TB_KEY_5},
    {25, TB_KEY_9},
    {26, TB_KEY_7},
    {28, TB_KEY_8},
    {29, TB_KEY_0},
    {31, TB_KEY_O},
    {32, TB_KEY_U},
    {34, TB_KEY_I},
    {35, TB_KEY_P},
    {36, TB_KEY_ENTER},
    {37, TB_KEY_L},
    {38, TB_KEY_J},
    {40, TB_KEY_K},
    {45, TB_KEY_N},
    {46, TB_KEY_M},
    {48, TB_KEY_TAB},
    {49, TB_KEY_SPACE},
    {50, TB_KEY_GRAVE},
    {51, TB_KEY_BACKSPACE},
    {53, TB_KEY_ESCAPE},
    {55, TB_KEY_UNKNOWN},
    {56, TB_KEY_LEFT_SHIFT},
    {58, TB_KEY_LEFT_ALT},
    {59, TB_KEY_LEFT_CONTROL},
    {60, TB_KEY_RIGHT_SHIFT},
    {61, TB_KEY_RIGHT_ALT},
    {62, TB_KEY_RIGHT_CONTROL},
    {76, TB_KEY_ENTER},
    {96, TB_KEY_F5},
    {97, TB_KEY_F6},
    {98, TB_KEY_F7},
    {99, TB_KEY_F3},
    {100, TB_KEY_F8},
    {101, TB_KEY_F9},
    {103, TB_KEY_F11},
    {109, TB_KEY_F10},
    {111, TB_KEY_F12},
    {118, TB_KEY_F4},
    {120, TB_KEY_F2},
    {122, TB_KEY_F1},
    {123, TB_KEY_LEFT},
    {124, TB_KEY_RIGHT},
    {125, TB_KEY_DOWN},
    {126, TB_KEY_UP},
};

static tb_key tb_cocoa_key(uint32_t code)
{
    for (size_t index = 0; index < sizeof(tb_cocoa_keys) / sizeof(tb_cocoa_keys[0]); index++)
    {
        if (tb_cocoa_keys[index].code == code)
        {
            return tb_cocoa_keys[index].key;
        }
    }
    return TB_KEY_UNKNOWN;
}

/* The device-specific flag of each modifier key, which tells left from right. */
typedef struct tb_cocoa_modifier
{
    tb_key key;
    unsigned long flag;
} tb_cocoa_modifier;

static const tb_cocoa_modifier tb_cocoa_modifiers[] = {
    {TB_KEY_LEFT_SHIFT, TB_COCOA_LEFT_SHIFT},     {TB_KEY_RIGHT_SHIFT, TB_COCOA_RIGHT_SHIFT},
    {TB_KEY_LEFT_CONTROL, TB_COCOA_LEFT_CONTROL}, {TB_KEY_RIGHT_CONTROL, TB_COCOA_RIGHT_CONTROL},
    {TB_KEY_LEFT_ALT, TB_COCOA_LEFT_ALT},         {TB_KEY_RIGHT_ALT, TB_COCOA_RIGHT_ALT},
};

/* Whether a modifier is down after a change of the modifiers. */
static bool tb_cocoa_modifier_down(tb_key key, unsigned long flags)
{
    for (size_t index = 0; index < sizeof(tb_cocoa_modifiers) / sizeof(tb_cocoa_modifiers[0]);
         index++)
    {
        if (tb_cocoa_modifiers[index].key == key)
        {
            return (flags & tb_cocoa_modifiers[index].flag) != 0;
        }
    }
    return false;
}

static tb_window *tb_cocoa_find(tb_window_system *system, tb_id handle)
{
    for (size_t index = 0; index < tb_array_length(system->windows); index++)
    {
        tb_window *const window = *TB_ARRAY_AT(system->windows, tb_window *, index);
        if (tb_cocoa_window_of(window)->handle == handle)
        {
            return window;
        }
    }
    return NULL;
}

/* Handle a key event; false for an event that is not one. Keys are not sent
 * on to the window, which would beep at keys nothing in it handles. */
static bool tb_cocoa_on_key(tb_window_system *system, tb_id event, unsigned long type)
{
    tb_window *window = NULL;
    uint32_t code = 0;
    tb_key key = TB_KEY_UNKNOWN;
    unsigned long flags = 0;

    if (type != TB_COCOA_KEY_DOWN && type != TB_COCOA_KEY_UP && type != TB_COCOA_FLAGS_CHANGED)
    {
        return false;
    }
    window = tb_cocoa_find(system, tb_send(event, "window"));
    if (window == NULL)
    {
        return true;
    }
    code =
        (uint32_t)((unsigned short (*)(tb_id, tb_sel))tb_cocoa.send)(event, tb_sel_of("keyCode"));
    key = tb_cocoa_key(code);
    flags = tb_get_unsigned(event, "modifierFlags");
    if (type == TB_COCOA_FLAGS_CHANGED)
    {
        tb_window_key(window, key, code, tb_cocoa_modifier_down(key, flags), false);
        return true;
    }
    if (type == TB_COCOA_KEY_DOWN && code == TB_COCOA_KEY_Q && (flags & TB_COCOA_COMMAND) != 0)
    {
        const tb_window_event request = {
            .type = TB_WINDOW_EVENT_CLOSE_REQUESTED, .window = window, .state = window->state};
        tb_window_push(system, &request);
        return true;
    }
    tb_window_key(window, key, code, type == TB_COCOA_KEY_DOWN,
                  type == TB_COCOA_KEY_DOWN && tb_get_bool(event, "isARepeat"));
    return true;
}

static void tb_cocoa_handle(tb_window_system *system, tb_id event)
{
    const unsigned long type = tb_get_unsigned(event, "type");

    if (type == TB_COCOA_APPLICATION_DEFINED &&
        ((short (*)(tb_id, tb_sel))tb_cocoa.send)(event, tb_sel_of("subtype")) ==
            TB_COCOA_WAKE_SUBTYPE)
    {
        tb_window_woken(system);
        return;
    }
    if (tb_cocoa_on_key(system, event, type))
    {
        return;
    }
    tb_send_id(tb_cocoa.application, "sendEvent:", event);
}

static tb_id tb_cocoa_next_event(tb_id until)
{
    return ((tb_id (*)(tb_id, tb_sel, unsigned long, tb_id, tb_id, bool))tb_cocoa.send)(
        tb_cocoa.application, tb_sel_of("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ul,
        until, tb_cocoa.default_mode, true);
}

static void tb_cocoa_pump(tb_window_system *system, uint64_t nanoseconds)
{
    void *const pool = tb_cocoa.pool_push();
    tb_id until = tb_send(tb_class("NSDate"), "distantPast");
    tb_id event = NULL;

    if (nanoseconds > 0)
    {
        until = ((tb_id (*)(tb_id, tb_sel, double))tb_cocoa.send)(
            tb_class("NSDate"), tb_sel_of("dateWithTimeIntervalSinceNow:"),
            (double)nanoseconds / 1e9);
    }
    event = tb_cocoa_next_event(until);
    while (event != NULL)
    {
        tb_cocoa_handle(system, event);
        event = tb_cocoa_next_event(tb_send(tb_class("NSDate"), "distantPast"));
    }
    (void)tb_send(tb_cocoa.application, "updateWindows");
    tb_cocoa.pool_pop(pool);
}

static void tb_cocoa_wake(tb_window_system *system)
{
    void *const pool = tb_cocoa.pool_push();
    const tb_id event = ((tb_id (*)(tb_id, tb_sel, unsigned long, tb_cocoa_point, unsigned long,
                                    double, long, tb_id, short, long, long))tb_cocoa.send)(
        tb_class("NSEvent"),
        tb_sel_of("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:"
                  "subtype:data1:data2:"),
        TB_COCOA_APPLICATION_DEFINED, (tb_cocoa_point){0.0, 0.0}, 0ul, 0.0, 0l, NULL,
        (short)TB_COCOA_WAKE_SUBTYPE, 0l, 0l);

    (void)system;
    ((void (*)(tb_id, tb_sel, tb_id, bool))tb_cocoa.send)(
        tb_cocoa.application, tb_sel_of("postEvent:atStart:"), event, false);
    tb_cocoa.pool_pop(pool);
}

/* ---------------------------------------------------------------------------
 * The backend's table.
 * ------------------------------------------------------------------------- */

static tb_id tb_cocoa_new_delegate(void *target)
{
    const tb_id delegate = tb_send(tb_send(tb_cocoa.delegate_class, "alloc"), "init");

    tb_cocoa_set_target(delegate, target);
    return delegate;
}

static tb_window_result tb_cocoa_create(tb_window *window, tb_window_error *error)
{
    tb_cocoa_window *const native = tb_calloc(1, sizeof(tb_cocoa_window));
    void *const pool = tb_cocoa.pool_push();

    window->native = native;
    native->handle = ((tb_id (*)(tb_id, tb_sel, tb_cocoa_rect, unsigned long, unsigned long,
                                 bool))tb_cocoa.send)(
        tb_send(tb_cocoa.window_class, "alloc"),
        tb_sel_of("initWithContentRect:styleMask:backing:defer:"), tb_cocoa_content_rect(window),
        tb_cocoa_windowed_style(&window->options), TB_COCOA_BACKING_BUFFERED, false);
    if (native->handle == NULL)
    {
        /* coverage-off: AppKit refuses a window only when out of memory */
        tb_cocoa.pool_pop(pool);
        tb_free(native);
        window->native = NULL;
        return tb_window_fail(error, TB_WINDOW_SYSTEM_ERROR, "AppKit refused a window");
        /* coverage-on */
    }
    native->delegate = tb_cocoa_new_delegate(window);
    tb_send_bool(native->handle, "setReleasedWhenClosed:", false);
    tb_send_id(native->handle, "setBackgroundColor:", tb_send(tb_class("NSColor"), "blackColor"));
    tb_send_id(native->handle, "setDelegate:", native->delegate);
    tb_cocoa.pool_pop(pool);
    tb_cocoa_apply(window);
    tb_send_id(native->handle, "makeKeyAndOrderFront:", NULL);
    tb_send_bool(tb_cocoa.application, "activateIgnoringOtherApps:", true);
    tb_cocoa_read_size(window);
    return TB_WINDOW_OK;
}

static void tb_cocoa_destroy(tb_window *window)
{
    tb_cocoa_window *const native = tb_cocoa_window_of(window);

    if (window->state.mode != TB_WINDOW_MODE_WINDOWED)
    {
        tb_send_unsigned(tb_cocoa.application, "setPresentationOptions:", 0);
    }
    tb_send_id(native->handle, "setDelegate:", NULL);
    (void)tb_send(native->handle, "close");
    (void)tb_send(native->handle, "release");
    (void)tb_send(native->delegate, "release");
    tb_free(native);
}

static tb_window_result tb_cocoa_configure(tb_window *window, const tb_window_options *previous,
                                           tb_window_error *error)
{
    (void)previous;
    (void)error;
    tb_cocoa_apply(window);
    return TB_WINDOW_OK;
}

static void tb_cocoa_native(const tb_window *window, tb_window_native *native)
{
    const tb_id handle = tb_cocoa_window_of(window)->handle;

    native->window = handle;
    native->system = tb_send(handle, "contentView");
}

static void tb_cocoa_close(tb_window_system *system)
{
    tb_cocoa_system *const native = tb_cocoa_of(system);

    tb_send_id(tb_send(tb_class("NSNotificationCenter"), "defaultCenter"),
               "removeObserver:", native->observer);
    (void)tb_send(native->observer, "release");
    tb_free(native);
}

static const tb_window_ops tb_cocoa_ops = {
    .close = tb_cocoa_close,
    .pump = tb_cocoa_pump,
    .wake = tb_cocoa_wake,
    .create = tb_cocoa_create,
    .destroy = tb_cocoa_destroy,
    .configure = tb_cocoa_configure,
    .native = tb_cocoa_native,
};

tb_window_result tb_native_window_open(tb_window_system *system, tb_window_backend requested,
                                       tb_window_error *error)
{
    tb_cocoa_system *native = NULL;
    tb_window_result result = TB_WINDOW_OK;
    void *pool = NULL;

    if (requested != TB_WINDOW_BACKEND_AUTOMATIC && requested != TB_WINDOW_BACKEND_COCOA)
    {
        return tb_window_fail(error, TB_WINDOW_UNAVAILABLE, "%s windowing is not on macOS",
                              tb_window_backend_name(requested));
    }
    TB_ASSERT_MSG(pthread_main_np() != 0, "Cocoa windowing is opened on the main thread");
    result = tb_cocoa_load(error);
    if (result != TB_WINDOW_OK)
    {
        /* coverage-off: see tb_cocoa_load() */
        return result;
        /* coverage-on */
    }
    pool = tb_cocoa.pool_push();
    tb_cocoa.application = tb_send(tb_class("NSApplication"), "sharedApplication");
    tb_send_signed(tb_cocoa.application, "setActivationPolicy:", TB_COCOA_ACTIVATION_REGULAR);
    (void)tb_send(tb_cocoa.application, "finishLaunching");
    native = tb_calloc(1, sizeof(tb_cocoa_system));
    native->observer = tb_cocoa_new_delegate(system);
    ((void (*)(tb_id, tb_sel, tb_id, tb_sel, tb_id, tb_id))tb_cocoa.send)(
        tb_send(tb_class("NSNotificationCenter"), "defaultCenter"),
        tb_sel_of("addObserver:selector:name:object:"), native->observer,
        tb_sel_of("screensChanged:"), tb_cocoa.screens_changed, NULL);
    tb_cocoa.pool_pop(pool);
    system->backend = TB_WINDOW_BACKEND_COCOA;
    system->ops = &tb_cocoa_ops;
    system->native = native;
    tb_cocoa_read_displays(system);
    return TB_WINDOW_OK;
}
