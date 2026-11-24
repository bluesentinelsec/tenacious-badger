/* The Cocoa windowing turns what AppKit gives a window's content view into
 * the engine's events: buttons, motion, the wheel, entering and leaving,
 * and the text the input context inserts. The events are made as AppKit
 * makes them and given to the view's methods, as the window would give
 * them, so that each is checked without a person at the mouse. */
#include "badger/library.h"
#include "badger/window.h"

#include "tb_test.h"

#include <stdint.h>

typedef void *id;
typedef void *sel;

typedef struct point
{
    double x;
    double y;
} point;

typedef struct range
{
    unsigned long location;
    unsigned long length;
} range;

/* NSEventType and NSEventModifierFlags. */
#define LEFT_MOUSE_DOWN 1ul
#define LEFT_MOUSE_UP 2ul
#define RIGHT_MOUSE_DOWN 3ul
#define RIGHT_MOUSE_UP 4ul
#define MOUSE_MOVED 5ul
#define MOUSE_ENTERED 8ul
#define MOUSE_EXITED 9ul
#define SHIFT_FLAG (1ul << 17)
#define OPTION_FLAG (1ul << 19)
/* CGEventType, CGMouseButton, CGScrollEventUnit, and CGEventField. */
#define CG_OTHER_MOUSE_DOWN 25u
#define CG_MOUSE_MOVED 5u
#define CG_BUTTON_FOURTH 3u
#define CG_SCROLL_LINE 1u
#define CG_SCROLL_PIXEL 0u
#define CG_DELTA_X 4u
#define CG_DELTA_Y 5u
#define NOT_FOUND 0x7FFFFFFFFFFFFFFFul

typedef struct runtime
{
    tb_library *objc;
    tb_library *appkit;
    tb_library *graphics;
    id (*get_class)(const char *name);
    sel (*register_name)(const char *name);
    tb_library_function send;
    void *(*event_create_mouse)(void *source, uint32_t type, point position, uint32_t button);
    void *(*event_create_scroll)(void *source, uint32_t units, uint32_t count, int32_t first, ...);
    void (*event_set_integer)(void *event, uint32_t field, int64_t value);
    void (*release)(const void *object);
} runtime;

typedef struct fixture
{
    runtime objc;
    tb_window_system *system;
    tb_window *window;
    id handle; /* The NSWindow. */
    id view;
    long number;
    double height; /* The view's, in points. */
    double scale;
} fixture;

static tb_library_function find(tb_library *library, const char *name)
{
    tb_library_function function = NULL;

    TB_TEST_REQUIRE_UINT(tb_library_find_function(library, name, &function, NULL), ==,
                         TB_LIBRARY_OK);
    return function;
}

static void load_runtime(runtime *objc)
{
    TB_TEST_REQUIRE_UINT(
        tb_library_open("libobjc.A.dylib", TB_LIBRARY_SEARCH_SYSTEM, &objc->objc, NULL), ==,
        TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(
        tb_library_open("AppKit.framework/AppKit", TB_LIBRARY_SEARCH_SYSTEM, &objc->appkit, NULL),
        ==, TB_LIBRARY_OK);
    TB_TEST_REQUIRE_UINT(tb_library_open("CoreGraphics.framework/CoreGraphics",
                                         TB_LIBRARY_SEARCH_SYSTEM, &objc->graphics, NULL),
                         ==, TB_LIBRARY_OK);
    objc->get_class = (id (*)(const char *))find(objc->objc, "objc_getClass");
    objc->register_name = (sel (*)(const char *))find(objc->objc, "sel_registerName");
    objc->send = find(objc->objc, "objc_msgSend");
    objc->event_create_mouse = (void *(*)(void *, uint32_t, point, uint32_t))find(
        objc->graphics, "CGEventCreateMouseEvent");
    objc->event_create_scroll = (void *(*)(void *, uint32_t, uint32_t, int32_t, ...))find(
        objc->graphics, "CGEventCreateScrollWheelEvent");
    objc->event_set_integer =
        (void (*)(void *, uint32_t, int64_t))find(objc->graphics, "CGEventSetIntegerValueField");
    objc->release = (void (*)(const void *))find(objc->graphics, "CFRelease");
}

static void close_runtime(runtime *objc)
{
    tb_library_close(objc->graphics);
    tb_library_close(objc->appkit);
    tb_library_close(objc->objc);
}

static id send(const fixture *made, id receiver, const char *selector)
{
    return ((id (*)(id, sel))made->objc.send)(receiver, made->objc.register_name(selector));
}

static void send_event(const fixture *made, id receiver, const char *selector, id event)
{
    ((void (*)(id, sel, id))made->objc.send)(receiver, made->objc.register_name(selector), event);
}

static fixture open_window(void)
{
    fixture made = {0};
    tb_window_native native = {0};
    tb_window_state state = {0};
    tb_window_error error = TB_WINDOW_ERROR_INIT;

    if (tb_window_system_open(NULL, &made.system, &error) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here: %s", error.message);
    }
    TB_TEST_REQUIRE_UINT(tb_window_create(made.system,
                                          &(tb_window_options){.width = 320, .height = 180},
                                          &made.window, &error),
                         ==, TB_WINDOW_OK);
    load_runtime(&made.objc);
    tb_window_native_of(made.window, &native);
    made.handle = native.window;
    made.view = native.system;
    made.number =
        ((long (*)(id, sel))made.objc.send)(made.handle, made.objc.register_name("windowNumber"));
    tb_window_state_of(made.window, &state);
    made.height = (double)state.height;
    made.scale = state.content_scale;
    return made;
}

static void close_window(fixture *made)
{
    tb_window_destroy(made->window);
    tb_window_system_close(made->system);
    close_runtime(&made->objc);
}

/* A mouse event of AppKit's own at a place in the view, from its top. */
static id mouse_event(const fixture *made, unsigned long type, double x, double y,
                      unsigned long flags)
{
    const point location = {x, made->height - y};

    return ((id (*)(id, sel, unsigned long, point, unsigned long, double, long, id, long, long,
                    float))made->objc.send)(
        made->objc.get_class("NSEvent"),
        made->objc.register_name("mouseEventWithType:location:modifierFlags:timestamp:"
                                 "windowNumber:context:eventNumber:clickCount:pressure:"),
        type, location, flags, 0.0, made->number, NULL, 0l, 1l, 1.0f);
}

static id crossing_event(const fixture *made, unsigned long type, double x, double y)
{
    const point location = {x, made->height - y};

    return ((id (*)(id, sel, unsigned long, point, unsigned long, double, long, id, long, long,
                    void *))made->objc.send)(
        made->objc.get_class("NSEvent"),
        made->objc.register_name("enterExitEventWithType:location:modifierFlags:timestamp:"
                                 "windowNumber:context:eventNumber:trackingNumber:userData:"),
        type, location, 0ul, 0.0, made->number, NULL, 0l, 0l, NULL);
}

/* An NSEvent of a Core Graphics event, which can say what AppKit's
 * constructors cannot: a fourth button, a wheel, a mouse's deltas. */
static id from_graphics(const fixture *made, void *event)
{
    return ((id (*)(id, sel, void *))made->objc.send)(
        made->objc.get_class("NSEvent"), made->objc.register_name("eventWithCGEvent:"), event);
}

/* The last queued event of a type, without reading the system: the view's
 * methods queue events as they are called. */
static bool received(fixture *made, tb_window_event_type type, tb_window_event *kept)
{
    tb_window_event event = {0};
    bool found = false;

    while (tb_window_system_poll(made->system, &event))
    {
        if (event.type == type)
        {
            found = true;
            *kept = event;
        }
    }
    return found;
}

TB_TEST(platform_cocoa_window, mouse_buttons_are_events_with_a_position)
{
    fixture made = open_window();
    tb_window_event event = {0};
    void *fourth = NULL;

    send_event(&made, made.view,
               "mouseDown:", mouse_event(&made, LEFT_MOUSE_DOWN, 12.0, 30.0, SHIFT_FLAG));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, &event));
    TB_TEST_EXPECT_UINT(event.button, ==, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_FLOAT(event.x, ==, 12.0);
    TB_TEST_EXPECT_FLOAT(event.y, ==, 30.0);
    TB_TEST_EXPECT_FLOAT(event.pixel_x, ==, 12.0 * made.scale);
    TB_TEST_EXPECT_UINT(event.modifiers, ==, TB_MODIFIER_SHIFT);
    send_event(&made, made.view, "mouseUp:", mouse_event(&made, LEFT_MOUSE_UP, 12.0, 30.0, 0));
    TB_TEST_EXPECT(received(&made, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, &event));
    send_event(&made, made.view,
               "rightMouseDown:", mouse_event(&made, RIGHT_MOUSE_DOWN, 1.0, 2.0, OPTION_FLAG));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, &event));
    TB_TEST_EXPECT_UINT(event.button, ==, TB_MOUSE_BUTTON_RIGHT);
    TB_TEST_EXPECT_UINT(event.modifiers, ==, TB_MODIFIER_ALT);
    fourth = made.objc.event_create_mouse(NULL, CG_OTHER_MOUSE_DOWN, (point){0.0, 0.0},
                                          CG_BUTTON_FOURTH);
    send_event(&made, made.view, "otherMouseDown:", from_graphics(&made, fourth));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, &event));
    TB_TEST_EXPECT_UINT(event.button, ==, TB_MOUSE_BUTTON_X1);
    send_event(&made, made.view, "otherMouseUp:", from_graphics(&made, fourth));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, &event));
    TB_TEST_EXPECT_UINT(event.button, ==, TB_MOUSE_BUTTON_X1);
    made.objc.release(fourth);
    send_event(&made, made.view, "rightMouseUp:", mouse_event(&made, RIGHT_MOUSE_UP, 1.0, 2.0, 0));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, &event));
    TB_TEST_EXPECT_UINT(event.button, ==, TB_MOUSE_BUTTON_RIGHT);
    close_window(&made);
}

TB_TEST(platform_cocoa_window, the_mouse_moves_enters_and_leaves)
{
    fixture made = open_window();
    tb_window_event event = {0};

    send_event(&made, made.view, "mouseEntered:", crossing_event(&made, MOUSE_ENTERED, 5.0, 6.0));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_ENTERED, &event));
    TB_TEST_EXPECT_FLOAT(event.x, ==, 5.0);
    TB_TEST_EXPECT_FLOAT(event.y, ==, 6.0);
    send_event(&made, made.view, "mouseMoved:", mouse_event(&made, MOUSE_MOVED, 40.0, 50.0, 0));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_MOVED, &event));
    TB_TEST_EXPECT_FLOAT(event.x, ==, 40.0);
    TB_TEST_EXPECT_FLOAT(event.y, ==, 50.0);
    /* The cursor is hidden while the mouse is over the window, and shown
     * when it leaves. Whether the leave arrives as an event is not checked:
     * the desktop's own crossing events race the one sent here, and the
     * check failed now and then under load. */
    tb_window_show_cursor(made.window, false);
    send_event(&made, made.view, "mouseExited:", crossing_event(&made, MOUSE_EXITED, 0.0, 0.0));
    tb_window_show_cursor(made.window, true);
    close_window(&made);
}

TB_TEST(platform_cocoa_window, the_wheel_turns_in_notches)
{
    fixture made = open_window();
    tb_window_event event = {0};
    void *lines = made.objc.event_create_scroll(NULL, CG_SCROLL_LINE, 2, 3, -2);
    void *pixels = made.objc.event_create_scroll(NULL, CG_SCROLL_PIXEL, 1, 40);

    send_event(&made, made.view, "scrollWheel:", from_graphics(&made, lines));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_WHEEL, &event));
    TB_TEST_EXPECT_FLOAT(event.delta_y, >, 0.0);
    TB_TEST_EXPECT_FLOAT(event.delta_x, !=, 0.0);
    send_event(&made, made.view, "scrollWheel:", from_graphics(&made, pixels));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_WHEEL, &event));
    TB_TEST_EXPECT_FLOAT(event.delta_y, >, 0.0);
    made.objc.release(lines);
    made.objc.release(pixels);
    close_window(&made);
}

/* The input context inserts text into the view; text being composed is not
 * text until then. */
TB_TEST(platform_cocoa_window, inserted_text_is_text)
{
    fixture made = open_window();
    tb_window_event event = {0};
    const range nowhere = {NOT_FOUND, 0};
    const id composed = ((id (*)(id, sel, const char *))made.objc.send)(
        made.objc.get_class("NSString"), made.objc.register_name("stringWithUTF8String:"),
        "\xC3\xA9\xF0\x9F\x98\x80");
    const id accent = ((id (*)(id, sel, const char *))made.objc.send)(
        made.objc.get_class("NSString"), made.objc.register_name("stringWithUTF8String:"),
        "\xC2\xB4");
    const id attributed = ((id (*)(id, sel, id))made.objc.send)(
        send(&made, made.objc.get_class("NSAttributedString"), "alloc"),
        made.objc.register_name("initWithString:"), accent);
    range actual = {0, 0};

    ((void (*)(id, sel, id, range, range))made.objc.send)(
        made.view, made.objc.register_name("setMarkedText:selectedRange:replacementRange:"), accent,
        (range){1, 0}, nowhere);
    TB_TEST_EXPECT(
        ((bool (*)(id, sel))made.objc.send)(made.view, made.objc.register_name("hasMarkedText")));
    ((void (*)(id, sel))made.objc.send)(made.view, made.objc.register_name("unmarkText"));
    TB_TEST_EXPECT(
        !((bool (*)(id, sel))made.objc.send)(made.view, made.objc.register_name("hasMarkedText")));
    ((void (*)(id, sel, id, range))made.objc.send)(
        made.view, made.objc.register_name("insertText:replacementRange:"), composed, nowhere);
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_TEXT_INPUT, &event));
    TB_TEST_EXPECT_STRING(event.text, ==, "\xC3\xA9\xF0\x9F\x98\x80");
    ((void (*)(id, sel, id, range))made.objc.send)(
        made.view, made.objc.register_name("insertText:replacementRange:"), attributed, nowhere);
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_TEXT_INPUT, &event));
    TB_TEST_EXPECT_STRING(event.text, ==, "\xC2\xB4");
    (void)send(&made, attributed, "release");

    /* What the input context asks of its client. */
    TB_TEST_EXPECT_UINT(
        ((range (*)(id, sel))made.objc.send)(made.view, made.objc.register_name("markedRange"))
            .location,
        ==, NOT_FOUND);
    TB_TEST_EXPECT_UINT(
        ((range (*)(id, sel))made.objc.send)(made.view, made.objc.register_name("selectedRange"))
            .length,
        ==, 0);
    TB_TEST_EXPECT_POINTER(send(&made, made.view, "validAttributesForMarkedText"), !=, NULL);
    TB_TEST_EXPECT_POINTER(
        ((id (*)(id, sel, range, range *))made.objc.send)(
            made.view, made.objc.register_name("attributedSubstringForProposedRange:actualRange:"),
            nowhere, &actual),
        ==, NULL);
    TB_TEST_EXPECT_UINT(
        ((unsigned long (*)(id, sel, point))made.objc.send)(
            made.view, made.objc.register_name("characterIndexForPoint:"), (point){0.0, 0.0}),
        ==, 0);
    ((void (*)(id, sel, sel))made.objc.send)(made.view,
                                             made.objc.register_name("doCommandBySelector:"),
                                             made.objc.register_name("moveLeft:"));
#if !defined(__x86_64__)
    /* A rectangle is returned in registers on arm64; on x86_64 it needs
     * objc_msgSend_stret, which this case does not load. */
    {
        typedef struct rect
        {
            point origin;
            point size;
        } rect;
        const rect first = ((rect (*)(id, sel, range, range *))made.objc.send)(
            made.view, made.objc.register_name("firstRectForCharacterRange:actualRange:"), nowhere,
            &actual);
        TB_TEST_EXPECT_FLOAT(first.size.x, ==, 0.0);
    }
#endif
    close_window(&made);
}

/* In relative mouse mode the mouse's deltas are reported in place of its
 * position, while the window is the key window. */
TB_TEST(platform_cocoa_window, relative_mouse_mode_reports_deltas)
{
    fixture made = open_window();
    tb_window_event event = {0};
    void *moved = made.objc.event_create_mouse(NULL, CG_MOUSE_MOVED, (point){0.0, 0.0}, 0);
    const id delegate = send(&made, made.handle, "delegate");

    made.objc.event_set_integer(moved, CG_DELTA_X, 6);
    made.objc.event_set_integer(moved, CG_DELTA_Y, -2);
    TB_TEST_REQUIRE_UINT(tb_window_set_relative_mouse(made.window, true, NULL), ==, TB_WINDOW_OK);
    send_event(&made, made.view, "mouseMoved:", from_graphics(&made, moved));
    TB_TEST_REQUIRE(received(&made, TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION, &event));
    TB_TEST_EXPECT_FLOAT(event.delta_x, ==, 6.0);
    TB_TEST_EXPECT_FLOAT(event.delta_y, ==, -2.0);
    /* The mouse is let go while another window is the key window. */
    send_event(&made, delegate, "windowDidResignKey:", NULL);
    send_event(&made, delegate, "windowDidBecomeKey:", NULL);
    TB_TEST_EXPECT_UINT(tb_window_set_relative_mouse(made.window, false, NULL), ==, TB_WINDOW_OK);
    send_event(&made, delegate, "windowDidResignKey:", NULL);
    made.objc.release(moved);
    close_window(&made);
}

TB_TEST(platform_cocoa_window, the_cursor_takes_each_shape)
{
    fixture made = open_window();

    for (int cursor = 0; cursor < TB_CURSOR_COUNT; cursor++)
    {
        tb_window_set_cursor(made.window, (tb_cursor)cursor);
        (void)send(&made, made.view, "resetCursorRects");
    }
    close_window(&made);
}
