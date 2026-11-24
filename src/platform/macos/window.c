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
 * display, above the menu bar, with the Dock and menu bar hidden.
 *
 * A third class is the window's content view, flipped so that positions are
 * from its top: it receives the mouse, and it is the text input client that
 * Cocoa's input context gives typed text to, dead keys composed. Relative
 * mouse mode parts the cursor from the mouse with
 * CGAssociateMouseAndMouseCursorPosition() and reads the events' deltas. */
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
#define TB_COCOA_SHIFT (1ul << 17)
#define TB_COCOA_CONTROL (1ul << 18)
#define TB_COCOA_OPTION (1ul << 19)
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

/* NSTrackingArea's options: entering and exiting, whatever window is
 * active, over the view's visible rectangle as it changes. */
#define TB_COCOA_TRACK_ENTERED_AND_EXITED 0x01ul
#define TB_COCOA_TRACK_ACTIVE_ALWAYS 0x80ul
#define TB_COCOA_TRACK_IN_VISIBLE_RECT 0x200ul

/* NSNotFound, NSIntegerMax. */
#define TB_COCOA_NOT_FOUND 0x7FFFFFFFFFFFFFFFul

/* A touchpad's precise scrolling is in points; this many make a notch. */
#define TB_COCOA_POINTS_PER_NOTCH 10.0

typedef struct tb_cocoa_range
{
    unsigned long location;
    unsigned long length;
} tb_cocoa_range;

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
    tb_id (*get_protocol)(const char *name);
    bool (*add_protocol)(tb_id class_object, tb_id protocol);
    tb_cocoa_size (*screen_size)(uint32_t display);
    tb_id (*copy_mode)(uint32_t display);
    double (*mode_refresh)(tb_id mode);
    size_t (*mode_pixel_width)(tb_id mode);
    size_t (*mode_pixel_height)(tb_id mode);
    void (*release_mode)(tb_id mode);
    int32_t (*associate_mouse)(bool connected);
    int32_t (*warp_cursor)(tb_cocoa_point point);
    tb_id default_mode;           /* NSDefaultRunLoopMode */
    tb_id screens_changed;        /* NSApplicationDidChangeScreenParametersNotification */
    tb_id window_class;           /* TBCocoaWindow */
    tb_id delegate_class;         /* TBCocoaDelegate */
    tb_id view_class;             /* TBCocoaView */
    ptrdiff_t target_offset;      /* Of the delegate's pointer to its window or system. */
    ptrdiff_t view_target_offset; /* Of the view's pointer to its window. */
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
    tb_id view;         /* The content view, a TBCocoaView. */
    bool cursor_hiding; /* NSCursor's hide was called for the window, and not unhide. */
    bool marked_text;   /* The input context is composing text. */
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

static void tb_cocoa_hold_mouse(tb_window *window, bool held);
static void tb_cocoa_update_cursor(tb_window *window);

/* A window in relative mouse mode holds the mouse while it is the key
 * window. */
static void tb_cocoa_on_key_window(tb_window *window, bool key)
{
    tb_window_focused(window, key);
    if (window->state.relative_mouse)
    {
        tb_cocoa_hold_mouse(window, key);
    }
    tb_cocoa_update_cursor(window);
}

static void tb_cocoa_did_become_key(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_cocoa_on_key_window(tb_cocoa_target(self), true);
}

static void tb_cocoa_did_resign_key(tb_id self, tb_sel command, tb_id notification)
{
    (void)command;
    (void)notification;
    tb_cocoa_on_key_window(tb_cocoa_target(self), false);
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
 * The mouse, the cursor, and text: the content view's methods.
 * ------------------------------------------------------------------------- */

static tb_cocoa_point tb_get_point(tb_id receiver, const char *selector)
{
    return ((tb_cocoa_point (*)(tb_id, tb_sel))tb_cocoa.send)(receiver, tb_sel_of(selector));
}

/* The window a view shows; NULL once the window is destroyed. */
static tb_window *tb_cocoa_view_target(tb_id view)
{
    return *(void **)((char *)view + tb_cocoa.view_target_offset);
}

static uint32_t tb_cocoa_modifier_bits(unsigned long flags)
{
    uint32_t modifiers = 0;

    if ((flags & TB_COCOA_SHIFT) != 0)
    {
        modifiers |= TB_MODIFIER_SHIFT;
    }
    if ((flags & TB_COCOA_CONTROL) != 0)
    {
        modifiers |= TB_MODIFIER_CONTROL;
    }
    if ((flags & TB_COCOA_OPTION) != 0)
    {
        modifiers |= TB_MODIFIER_ALT;
    }
    if ((flags & TB_COCOA_COMMAND) != 0)
    {
        modifiers |= TB_MODIFIER_SUPER;
    }
    return modifiers;
}

static uint32_t tb_cocoa_event_modifiers(tb_id event)
{
    return tb_cocoa_modifier_bits(tb_get_unsigned(event, "modifierFlags"));
}

/* Where an event happened in a view, from the view's top-left corner: the
 * view is flipped. */
static tb_cocoa_point tb_cocoa_event_position(tb_id view, tb_id event)
{
    const tb_cocoa_point in_window = tb_get_point(event, "locationInWindow");

    return ((tb_cocoa_point (*)(tb_id, tb_sel, tb_cocoa_point, tb_id))tb_cocoa.send)(
        view, tb_sel_of("convertPoint:fromView:"), in_window, NULL);
}

/* NSEvent's button numbers. */
static tb_mouse_button tb_cocoa_button(long number)
{
    switch (number)
    {
    case 0:
        return TB_MOUSE_BUTTON_LEFT;
    case 1:
        return TB_MOUSE_BUTTON_RIGHT;
    case 2:
        return TB_MOUSE_BUTTON_MIDDLE;
    case 3:
        return TB_MOUSE_BUTTON_X1;
    case 4:
        return TB_MOUSE_BUTTON_X2;
    default:
        return TB_MOUSE_BUTTON_NONE;
    }
}

static void tb_cocoa_on_button(tb_id view, tb_id event, tb_mouse_button button, bool down)
{
    tb_window *const window = tb_cocoa_view_target(view);
    tb_cocoa_point position = {0};

    if (window == NULL || button == TB_MOUSE_BUTTON_NONE)
    {
        return;
    }
    position = tb_cocoa_event_position(view, event);
    tb_window_mouse_button(window, button, down, position.x, position.y,
                           tb_cocoa_event_modifiers(event));
}

/* The left and right buttons have methods of their own; the others share
 * one, and say which they are. */
static tb_mouse_button tb_cocoa_other_button(tb_id event)
{
    return tb_cocoa_button(
        ((long (*)(tb_id, tb_sel))tb_cocoa.send)(event, tb_sel_of("buttonNumber")));
}

static void tb_cocoa_left_down(tb_id self, tb_sel command, tb_id event)
{
    (void)command;
    tb_cocoa_on_button(self, event, TB_MOUSE_BUTTON_LEFT, true);
}

static void tb_cocoa_left_up(tb_id self, tb_sel command, tb_id event)
{
    (void)command;
    tb_cocoa_on_button(self, event, TB_MOUSE_BUTTON_LEFT, false);
}

static void tb_cocoa_right_down(tb_id self, tb_sel command, tb_id event)
{
    (void)command;
    tb_cocoa_on_button(self, event, TB_MOUSE_BUTTON_RIGHT, true);
}

static void tb_cocoa_right_up(tb_id self, tb_sel command, tb_id event)
{
    (void)command;
    tb_cocoa_on_button(self, event, TB_MOUSE_BUTTON_RIGHT, false);
}

static void tb_cocoa_other_down(tb_id self, tb_sel command, tb_id event)
{
    (void)command;
    tb_cocoa_on_button(self, event, tb_cocoa_other_button(event), true);
}

static void tb_cocoa_other_up(tb_id self, tb_sel command, tb_id event)
{
    (void)command;
    tb_cocoa_on_button(self, event, tb_cocoa_other_button(event), false);
}

/* Motion, with a button down or not: the mouse's deltas in relative mouse
 * mode, else its position. */
static void tb_cocoa_mouse_moved(tb_id self, tb_sel command, tb_id event)
{
    tb_window *const window = tb_cocoa_view_target(self);
    tb_cocoa_point position = {0};

    (void)command;
    if (window == NULL)
    {
        return;
    }
    if (window->state.relative_mouse)
    {
        tb_window_mouse_relative(window, tb_get_double(event, "deltaX"),
                                 tb_get_double(event, "deltaY"));
        return;
    }
    position = tb_cocoa_event_position(self, event);
    tb_window_mouse_moved(window, position.x, position.y, tb_cocoa_event_modifiers(event));
}

/* Cocoa's horizontal scrolling is positive to the left; a touchpad's
 * precise scrolling is in points. */
static void tb_cocoa_scroll_wheel(tb_id self, tb_sel command, tb_id event)
{
    tb_window *const window = tb_cocoa_view_target(self);
    tb_cocoa_point position = {0};
    double notches_x = 0.0;
    double notches_y = 0.0;

    (void)command;
    if (window == NULL)
    {
        return;
    }
    notches_x = -tb_get_double(event, "scrollingDeltaX");
    notches_y = tb_get_double(event, "scrollingDeltaY");
    if (tb_get_bool(event, "hasPreciseScrollingDeltas"))
    {
        notches_x /= TB_COCOA_POINTS_PER_NOTCH;
        notches_y /= TB_COCOA_POINTS_PER_NOTCH;
    }
    if (notches_x == 0.0 && notches_y == 0.0)
    {
        return;
    }
    position = tb_cocoa_event_position(self, event);
    tb_window_mouse_wheel(window, notches_x, notches_y, position.x, position.y,
                          tb_cocoa_event_modifiers(event));
}

static void tb_cocoa_update_cursor(tb_window *window);

static void tb_cocoa_mouse_entered(tb_id self, tb_sel command, tb_id event)
{
    tb_window *const window = tb_cocoa_view_target(self);
    tb_cocoa_point position = {0};

    (void)command;
    if (window == NULL)
    {
        return;
    }
    position = tb_cocoa_event_position(self, event);
    tb_window_mouse_crossed(window, true, position.x, position.y);
    tb_cocoa_update_cursor(window);
}

static void tb_cocoa_mouse_exited(tb_id self, tb_sel command, tb_id event)
{
    tb_window *const window = tb_cocoa_view_target(self);

    (void)command;
    (void)event;
    if (window == NULL)
    {
        return;
    }
    tb_window_mouse_crossed(window, false, 0.0, 0.0);
    tb_cocoa_update_cursor(window);
}

/* The system's cursor of a shape. The diagonal resizing cursors are
 * AppKit's own, not public: a system without them shows a closed hand, as
 * for moving. */
static tb_id tb_cocoa_cursor(tb_cursor cursor)
{
    static const char *const selectors[TB_CURSOR_COUNT] = {
        [TB_CURSOR_ARROW] = "arrowCursor",
        [TB_CURSOR_TEXT] = "IBeamCursor",
        [TB_CURSOR_HAND] = "pointingHandCursor",
        [TB_CURSOR_CROSSHAIR] = "crosshairCursor",
        [TB_CURSOR_NOT_ALLOWED] = "operationNotAllowedCursor",
        [TB_CURSOR_RESIZE_HORIZONTAL] = "resizeLeftRightCursor",
        [TB_CURSOR_RESIZE_VERTICAL] = "resizeUpDownCursor",
        [TB_CURSOR_RESIZE_DIAGONAL_DOWN] = "_windowResizeNorthWestSouthEastCursor",
        [TB_CURSOR_RESIZE_DIAGONAL_UP] = "_windowResizeNorthEastSouthWestCursor",
        [TB_CURSOR_RESIZE_ALL] = "closedHandCursor",
    };
    const tb_id cursor_class = tb_class("NSCursor");

    if (!((bool (*)(tb_id, tb_sel, tb_sel))tb_cocoa.send)(
            cursor_class, tb_sel_of("respondsToSelector:"), tb_sel_of(selectors[cursor])))
    {
        /* coverage-off: every macOS the engine runs on has AppKit's own diagonal cursors */
        return tb_send(cursor_class, "closedHandCursor");
        /* coverage-on */
    }
    return tb_send(cursor_class, selectors[cursor]);
}

/* The view's cursor, where Cocoa sets it as the mouse moves over the view. */
static void tb_cocoa_reset_cursor_rects(tb_id self, tb_sel command)
{
    const tb_window *const window = tb_cocoa_view_target(self);

    (void)command;
    if (window == NULL)
    {
        return;
    }
    ((void (*)(tb_id, tb_sel, tb_cocoa_rect, tb_id))tb_cocoa.send)(
        self, tb_sel_of("addCursorRect:cursor:"), tb_get_rect(self, "bounds"),
        tb_cocoa_cursor(window->cursor));
}

/* NSCursor's hiding counts, for the whole application: a window hides the
 * cursor once, while the mouse is over it, or while it holds the mouse. */
static void tb_cocoa_update_cursor(tb_window *window)
{
    tb_cocoa_window *const native = tb_cocoa_window_of(window);
    const bool hidden = window->state.relative_mouse
                            ? window->state.focused
                            : window->cursor_hidden && window->mouse_inside;

    if (hidden != native->cursor_hiding)
    {
        (void)tb_send(tb_class("NSCursor"), hidden ? "hide" : "unhide");
        native->cursor_hiding = hidden;
    }
    tb_send_id(native->handle, "invalidateCursorRectsForView:", native->view);
    if (window->mouse_inside && !hidden)
    {
        (void)tb_send(tb_cocoa_cursor(window->cursor), "set");
    }
}

/* Part the cursor from the mouse, over the window's middle, or join them
 * again. */
static void tb_cocoa_hold_mouse(tb_window *window, bool held)
{
    if (held)
    {
        const tb_cocoa_rect frame = tb_get_rect(tb_cocoa_window_of(window)->handle, "frame");
        const tb_cocoa_point middle = {frame.origin.x + frame.size.width / 2.0,
                                       tb_cocoa_primary_height() -
                                           (frame.origin.y + frame.size.height / 2.0)};
        (void)tb_cocoa.warp_cursor(middle);
    }
    (void)tb_cocoa.associate_mouse(!held);
}

static tb_window_result tb_cocoa_set_relative_mouse(tb_window *window, bool relative,
                                                    tb_window_error *error)
{
    (void)error;
    tb_cocoa_hold_mouse(window, relative && window->state.focused);
    return TB_WINDOW_OK;
}

static bool tb_cocoa_accepts(tb_id self, tb_sel command, tb_id event)
{
    (void)self;
    (void)command;
    (void)event;
    return true;
}

/* The text input client: the input context gives it the text it composed. */
static void tb_cocoa_insert_text(tb_id self, tb_sel command, tb_id string,
                                 tb_cocoa_range replacement)
{
    tb_window *const window = tb_cocoa_view_target(self);
    tb_id text = string;
    const char *utf8 = NULL;

    (void)command;
    (void)replacement;
    if (window == NULL)
    {
        return;
    }
    tb_cocoa_window_of(window)->marked_text = false;
    if (((bool (*)(tb_id, tb_sel, tb_id))tb_cocoa.send)(string, tb_sel_of("isKindOfClass:"),
                                                        tb_class("NSAttributedString")))
    {
        text = tb_send(string, "string");
    }
    utf8 = ((const char *(*)(tb_id, tb_sel))tb_cocoa.send)(text, tb_sel_of("UTF8String"));
    if (utf8 != NULL)
    {
        tb_window_text(window, utf8, tb_str_length(utf8));
    }
}

/* Text being composed, as a dead key's accent is before its letter: it is
 * not text until it is inserted. */
static void tb_cocoa_set_marked_text(tb_id self, tb_sel command, tb_id string,
                                     tb_cocoa_range selected, tb_cocoa_range replacement)
{
    tb_window *const window = tb_cocoa_view_target(self);

    (void)command;
    (void)selected;
    (void)replacement;
    if (window == NULL)
    {
        return;
    }
    tb_cocoa_window_of(window)->marked_text = tb_get_unsigned(string, "length") > 0;
}

static void tb_cocoa_unmark_text(tb_id self, tb_sel command)
{
    tb_window *const window = tb_cocoa_view_target(self);

    (void)command;
    if (window != NULL)
    {
        tb_cocoa_window_of(window)->marked_text = false;
    }
}

static bool tb_cocoa_has_marked_text(tb_id self, tb_sel command)
{
    const tb_window *const window = tb_cocoa_view_target(self);

    (void)command;
    return window != NULL && tb_cocoa_window_of(window)->marked_text;
}

static tb_cocoa_range tb_cocoa_marked_range(tb_id self, tb_sel command)
{
    const tb_cocoa_range none = {TB_COCOA_NOT_FOUND, 0};

    (void)self;
    (void)command;
    return none;
}

static tb_cocoa_range tb_cocoa_selected_range(tb_id self, tb_sel command)
{
    const tb_cocoa_range empty = {0, 0};

    (void)self;
    (void)command;
    return empty;
}

static tb_id tb_cocoa_valid_attributes(tb_id self, tb_sel command)
{
    (void)self;
    (void)command;
    return tb_send(tb_class("NSArray"), "array");
}

static tb_id tb_cocoa_attributed_substring(tb_id self, tb_sel command, tb_cocoa_range range,
                                           tb_cocoa_range *actual)
{
    (void)self;
    (void)command;
    (void)range;
    (void)actual;
    return NULL;
}

/* Where an input method editor puts its window: by the window, on the
 * screen, until the interface says where its text is. */
static tb_cocoa_rect tb_cocoa_first_rect(tb_id self, tb_sel command, tb_cocoa_range range,
                                         tb_cocoa_range *actual)
{
    const tb_window *const window = tb_cocoa_view_target(self);
    tb_cocoa_rect rect = {0};

    (void)command;
    (void)range;
    (void)actual;
    if (window != NULL)
    {
        rect = tb_get_rect(tb_cocoa_window_of(window)->handle, "frame");
        rect.size = (tb_cocoa_size){0.0, 0.0};
    }
    return rect;
}

static unsigned long tb_cocoa_character_index(tb_id self, tb_sel command, tb_cocoa_point point)
{
    (void)self;
    (void)command;
    (void)point;
    return 0;
}

/* A key the input context does not type, such as an arrow, is a command:
 * it is a key event already, and nothing more. */
static void tb_cocoa_do_command(tb_id self, tb_sel command, tb_sel selector)
{
    (void)self;
    (void)command;
    (void)selector;
}

/* ---------------------------------------------------------------------------
 * Loading the runtime and making the classes.
 * ------------------------------------------------------------------------- */

static void tb_cocoa_add(tb_id class_object, const char *name, tb_imp implementation,
                         const char *types)
{
    (void)tb_cocoa.add_method(class_object, tb_sel_of(name), implementation, types);
}

/* The content view: the mouse's events, the cursor, and the text input
 * client's methods. */
static tb_id tb_cocoa_make_view_class(void)
{
    static const struct
    {
        const char *name;
        tb_imp implementation;
    } buttons[] = {
        {"mouseDown:", (tb_imp)tb_cocoa_left_down},
        {"mouseUp:", (tb_imp)tb_cocoa_left_up},
        {"rightMouseDown:", (tb_imp)tb_cocoa_right_down},
        {"rightMouseUp:", (tb_imp)tb_cocoa_right_up},
        {"otherMouseDown:", (tb_imp)tb_cocoa_other_down},
        {"otherMouseUp:", (tb_imp)tb_cocoa_other_up},
    };
    static const char *const motions[] = {
        "mouseMoved:", "mouseDragged:", "rightMouseDragged:", "otherMouseDragged:"};
    tb_id view_class = tb_cocoa.allocate_class(tb_class("NSView"), "TBCocoaView", 0);
    tb_id protocol = tb_cocoa.get_protocol("NSTextInputClient");

    (void)tb_cocoa.add_ivar(view_class, "tb_target", sizeof(void *), 3, "^v");
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]); index++)
    {
        tb_cocoa_add(view_class, buttons[index].name, buttons[index].implementation, "v@:@");
    }
    for (size_t index = 0; index < 4; index++)
    {
        tb_cocoa_add(view_class, motions[index], (tb_imp)tb_cocoa_mouse_moved, "v@:@");
    }
    tb_cocoa_add(view_class, "scrollWheel:", (tb_imp)tb_cocoa_scroll_wheel, "v@:@");
    tb_cocoa_add(view_class, "mouseEntered:", (tb_imp)tb_cocoa_mouse_entered, "v@:@");
    tb_cocoa_add(view_class, "mouseExited:", (tb_imp)tb_cocoa_mouse_exited, "v@:@");
    tb_cocoa_add(view_class, "resetCursorRects", (tb_imp)tb_cocoa_reset_cursor_rects, "v@:");
    tb_cocoa_add(view_class, "acceptsFirstResponder", (tb_imp)tb_cocoa_yes, "c@:");
    tb_cocoa_add(view_class, "isFlipped", (tb_imp)tb_cocoa_yes, "c@:");
    tb_cocoa_add(view_class, "acceptsFirstMouse:", (tb_imp)tb_cocoa_accepts, "c@:@");
    tb_cocoa_add(view_class, "insertText:replacementRange:", (tb_imp)tb_cocoa_insert_text,
                 "v@:@{_NSRange=QQ}");
    tb_cocoa_add(view_class,
                 "setMarkedText:selectedRange:replacementRange:", (tb_imp)tb_cocoa_set_marked_text,
                 "v@:@{_NSRange=QQ}{_NSRange=QQ}");
    tb_cocoa_add(view_class, "unmarkText", (tb_imp)tb_cocoa_unmark_text, "v@:");
    tb_cocoa_add(view_class, "hasMarkedText", (tb_imp)tb_cocoa_has_marked_text, "c@:");
    tb_cocoa_add(view_class, "markedRange", (tb_imp)tb_cocoa_marked_range, "{_NSRange=QQ}@:");
    tb_cocoa_add(view_class, "selectedRange", (tb_imp)tb_cocoa_selected_range, "{_NSRange=QQ}@:");
    tb_cocoa_add(view_class, "validAttributesForMarkedText", (tb_imp)tb_cocoa_valid_attributes,
                 "@@:");
    tb_cocoa_add(view_class, "attributedSubstringForProposedRange:actualRange:",
                 (tb_imp)tb_cocoa_attributed_substring, "@@:{_NSRange=QQ}^{_NSRange=QQ}");
    tb_cocoa_add(view_class, "firstRectForCharacterRange:actualRange:", (tb_imp)tb_cocoa_first_rect,
                 "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");
    tb_cocoa_add(view_class, "characterIndexForPoint:", (tb_imp)tb_cocoa_character_index,
                 "Q@:{CGPoint=dd}");
    tb_cocoa_add(view_class, "doCommandBySelector:", (tb_imp)tb_cocoa_do_command, "v@::");
    if (protocol != NULL)
    {
        (void)tb_cocoa.add_protocol(view_class, protocol);
    }
    tb_cocoa.register_class(view_class);
    return view_class;
}

static void tb_cocoa_make_classes(void)
{
    tb_id window_class = tb_cocoa.get_class("TBCocoaWindow");
    tb_id delegate_class = tb_cocoa.get_class("TBCocoaDelegate");
    tb_id view_class = tb_cocoa.get_class("TBCocoaView");

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
    if (view_class == NULL)
    {
        view_class = tb_cocoa_make_view_class();
    }
    tb_cocoa.window_class = window_class;
    tb_cocoa.delegate_class = delegate_class;
    tb_cocoa.view_class = view_class;
    tb_cocoa.target_offset = tb_cocoa.ivar_offset(tb_cocoa.get_ivar(delegate_class, "tb_target"));
    tb_cocoa.view_target_offset = tb_cocoa.ivar_offset(tb_cocoa.get_ivar(view_class, "tb_target"));
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
        "objc_getProtocol",
        "class_addProtocol",
    };
    static const char *const graphics_names[] = {
        "CGDisplayScreenSize",
        "CGDisplayCopyDisplayMode",
        "CGDisplayModeGetRefreshRate",
        "CGDisplayModeGetPixelWidth",
        "CGDisplayModeGetPixelHeight",
        "CGDisplayModeRelease",
        "CGAssociateMouseAndMouseCursorPosition",
        "CGWarpMouseCursorPosition",
    };
    tb_library_function objc[13] = {0};
    tb_library_function graphics[8] = {0};
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
        !tb_cocoa_bind(tb_cocoa.objc, objc_names, objc, 13, &library_error) ||
        !tb_cocoa_bind(tb_cocoa.graphics, graphics_names, graphics, 8, &library_error))
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
    tb_cocoa.get_protocol = (tb_id (*)(const char *))objc[11];
    tb_cocoa.add_protocol = (bool (*)(tb_id, tb_id))objc[12];
    tb_cocoa.screen_size = (tb_cocoa_size (*)(uint32_t))graphics[0];
    tb_cocoa.copy_mode = (tb_id (*)(uint32_t))graphics[1];
    tb_cocoa.mode_refresh = (double (*)(tb_id))graphics[2];
    tb_cocoa.mode_pixel_width = (size_t (*)(tb_id))graphics[3];
    tb_cocoa.mode_pixel_height = (size_t (*)(tb_id))graphics[4];
    tb_cocoa.release_mode = (void (*)(tb_id))graphics[5];
    tb_cocoa.associate_mouse = (int32_t (*)(bool))graphics[6];
    tb_cocoa.warp_cursor = (int32_t (*)(tb_cocoa_point))graphics[7];
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
    {114, TB_KEY_INSERT}, /* Help, where a PC keyboard's Insert is */
    {115, TB_KEY_HOME},
    {116, TB_KEY_PAGE_UP},
    {117, TB_KEY_DELETE},
    {118, TB_KEY_F4},
    {119, TB_KEY_END},
    {120, TB_KEY_F2},
    {121, TB_KEY_PAGE_DOWN},
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
        tb_window_key(window, key, code, tb_cocoa_modifier_down(key, flags), false,
                      tb_cocoa_modifier_bits(flags));
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
                  type == TB_COCOA_KEY_DOWN && tb_get_bool(event, "isARepeat"),
                  tb_cocoa_modifier_bits(flags));
    if (type == TB_COCOA_KEY_DOWN)
    {
        /* The input context types the key's text, composing dead keys, into
         * the view. */
        tb_send_id(tb_cocoa_window_of(window)->view, "interpretKeyEvents:",
                   tb_send_id(tb_class("NSArray"), "arrayWithObject:", event));
    }
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

/* The content view, which receives the mouse and is the first responder
 * for text; the mouse's entering and leaving are tracked over all of it. */
static void tb_cocoa_make_view(tb_window *window)
{
    tb_cocoa_window *const native = tb_cocoa_window_of(window);
    const tb_cocoa_rect content = tb_get_rect(tb_send(native->handle, "contentView"), "frame");
    const unsigned long options = TB_COCOA_TRACK_ENTERED_AND_EXITED | TB_COCOA_TRACK_ACTIVE_ALWAYS |
                                  TB_COCOA_TRACK_IN_VISIBLE_RECT;
    tb_id area = NULL;

    native->view = ((tb_id (*)(tb_id, tb_sel, tb_cocoa_rect))tb_cocoa.send)(
        tb_send(tb_cocoa.view_class, "alloc"), tb_sel_of("initWithFrame:"), content);
    *(void **)((char *)native->view + tb_cocoa.view_target_offset) = window;
    area = ((tb_id (*)(tb_id, tb_sel, tb_cocoa_rect, unsigned long, tb_id, tb_id))tb_cocoa.send)(
        tb_send(tb_class("NSTrackingArea"), "alloc"),
        tb_sel_of("initWithRect:options:owner:userInfo:"), content, options, native->view, NULL);
    tb_send_id(native->view, "addTrackingArea:", area);
    (void)tb_send(area, "release");
    tb_send_id(native->handle, "setContentView:", native->view);
    (void)tb_send(native->view, "release");
    (void)tb_send_id(native->handle, "makeFirstResponder:", native->view);
    tb_send_bool(native->handle, "setAcceptsMouseMovedEvents:", true);
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
    tb_cocoa_make_view(window);
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
    if (native->cursor_hiding)
    {
        (void)tb_send(tb_class("NSCursor"), "unhide");
    }
    /* The view may outlive the window in an autorelease pool: it forgets
     * the window. */
    *(void **)((char *)native->view + tb_cocoa.view_target_offset) = NULL;
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

/* NSAlert's answer when its first button, "Quit", is chosen. */
#define TB_COCOA_FIRST_BUTTON 1000l

static tb_window_result tb_cocoa_confirm(tb_window *window, const char *title, const char *message,
                                         bool *confirmed)
{
    void *const pool = tb_cocoa.pool_push();
    const tb_id alert = tb_send(tb_send(tb_class("NSAlert"), "alloc"), "init");
    long answer = 0;

    (void)window;
    tb_send_id(alert, "setMessageText:", tb_string(title));
    tb_send_id(alert, "setInformativeText:", tb_string(message));
    (void)tb_send_id(alert, "addButtonWithTitle:", tb_string("Yes"));
    (void)tb_send_id(alert, "addButtonWithTitle:", tb_string("No"));
    answer = ((long (*)(tb_id, tb_sel))tb_cocoa.send)(alert, tb_sel_of("runModal"));
    (void)tb_send(alert, "release");
    tb_cocoa.pool_pop(pool);
    *confirmed = answer == TB_COCOA_FIRST_BUTTON;
    return TB_WINDOW_OK;
}

static const tb_window_ops tb_cocoa_ops = {
    .close = tb_cocoa_close,
    .pump = tb_cocoa_pump,
    .wake = tb_cocoa_wake,
    .create = tb_cocoa_create,
    .destroy = tb_cocoa_destroy,
    .configure = tb_cocoa_configure,
    .native = tb_cocoa_native,
    .confirm = tb_cocoa_confirm,
    .set_relative_mouse = tb_cocoa_set_relative_mouse,
    .update_cursor = tb_cocoa_update_cursor,
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
