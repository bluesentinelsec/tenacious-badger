/* The front of the window interface, the same on every platform: the
 * records, the event queue, the displays, where a new window goes, and the
 * contracts. Each platform's backend does the rest through its table. */
#include "window.h"

#include "badger/assert.h"
#include "badger/fmt.h"
#include "badger/memory.h"
#include "badger/str.h"
#include "badger/utf8.h"

#include <stdarg.h>
#include <string.h>

/* The largest default window, and the part of a display's usable area a
 * default window may take. */
#define TB_WINDOW_DEFAULT_WIDTH 1280u
#define TB_WINDOW_DEFAULT_HEIGHT 720u
#define TB_WINDOW_DEFAULT_SHARE 0.85

/* The smallest window: a client area smaller than this is unusable. */
#define TB_WINDOW_SMALLEST 64u

/* What a system that reports no display is given. */
#define TB_WINDOW_FALLBACK_WIDTH 1280u
#define TB_WINDOW_FALLBACK_HEIGHT 720u

tb_window_result tb_window_fail(tb_window_error *error, tb_window_result result, const char *format,
                                ...)
{
    va_list arguments;

    if (error == NULL)
    {
        return result;
    }
    error->result = result;
    va_start(arguments, format);
    (void)tb_fmt_vbuffer(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
    return result;
}

static void tb_window_succeed(tb_window_error *error)
{
    if (error != NULL)
    {
        *error = (tb_window_error)TB_WINDOW_ERROR_INIT;
    }
}

/* ---------------------------------------------------------------------------
 * Displays.
 * ------------------------------------------------------------------------- */

void tb_window_set_displays(tb_window_system *system, const tb_display *displays, size_t count)
{
    size_t primary = 0;

    tb_array_clear(system->displays);
    if (count == 0)
    {
        const tb_display fallback = {.primary = true,
                                     .width = TB_WINDOW_FALLBACK_WIDTH,
                                     .height = TB_WINDOW_FALLBACK_HEIGHT,
                                     .usable_width = TB_WINDOW_FALLBACK_WIDTH,
                                     .usable_height = TB_WINDOW_FALLBACK_HEIGHT,
                                     .pixel_width = TB_WINDOW_FALLBACK_WIDTH,
                                     .pixel_height = TB_WINDOW_FALLBACK_HEIGHT,
                                     .content_scale = 1.0};
        (void)tb_array_push(system->displays, &fallback);
        return;
    }
    for (size_t index = 0; index < count; index++)
    {
        if (displays[index].primary)
        {
            primary = index;
            break;
        }
    }
    (void)tb_array_push(system->displays, &displays[primary]);
    TB_ARRAY_AT(system->displays, tb_display, 0)->primary = true;
    for (size_t index = 0; index < count; index++)
    {
        if (index != primary)
        {
            (void)tb_array_push(system->displays, &displays[index]);
            TB_ARRAY_AT(system->displays, tb_display, tb_array_length(system->displays) - 1)
                ->primary = false;
        }
    }
}

static int64_t tb_window_overlap(int32_t start, uint32_t length, int32_t other_start,
                                 uint32_t other_length)
{
    const int64_t first = start > other_start ? start : other_start;
    const int64_t end = (int64_t)start + length;
    const int64_t other_end = (int64_t)other_start + other_length;
    const int64_t last = end < other_end ? end : other_end;

    return last > first ? last - first : 0;
}

size_t tb_window_display_at(const tb_window_system *system, int32_t x, int32_t y, uint32_t width,
                            uint32_t height)
{
    size_t best = 0;
    int64_t best_area = 0;

    for (size_t index = 0; index < tb_array_length(system->displays); index++)
    {
        const tb_display *const display = TB_ARRAY_AT(system->displays, tb_display, index);
        const int64_t area = tb_window_overlap(x, width, display->x, display->width) *
                             tb_window_overlap(y, height, display->y, display->height);
        if (area > best_area)
        {
            best = index;
            best_area = area;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * Placement.
 * ------------------------------------------------------------------------- */

static uint32_t tb_window_scaled(uint32_t value, double scale)
{
    const double scaled = (double)value * scale + 0.5;

    if (scaled >= (double)UINT32_MAX)
    {
        return UINT32_MAX;
    }
    return scaled < 1.0 ? 1u : (uint32_t)scaled;
}

static uint32_t tb_window_at_least(uint32_t value, uint32_t least)
{
    return value < least ? least : value;
}

void tb_window_place(const tb_window *window, bool units_are_pixels, tb_window_placement *placement)
{
    const tb_window_system *const system = window->system;
    const size_t display_index =
        window->options.display < tb_array_length(system->displays) ? window->options.display : 0;
    const tb_display *const display = TB_ARRAY_AT(system->displays, tb_display, display_index);
    const double scale = display->content_scale > 0.0 ? display->content_scale : 1.0;
    /* The usable area in logical units. */
    const double unit_scale = units_are_pixels ? scale : 1.0;
    const uint32_t usable_width = tb_window_at_least(
        (uint32_t)((double)display->usable_width / unit_scale), TB_WINDOW_SMALLEST);
    const uint32_t usable_height = tb_window_at_least(
        (uint32_t)((double)display->usable_height / unit_scale), TB_WINDOW_SMALLEST);
    uint32_t width = window->options.width;
    uint32_t height = window->options.height;

    if (width == 0 || height == 0)
    {
        /* The largest 16:9 size up to the default that fits in the share of
         * the usable area. */
        const double room_width = (double)usable_width * TB_WINDOW_DEFAULT_SHARE;
        const double room_height = (double)usable_height * TB_WINDOW_DEFAULT_SHARE;
        double fit = 1.0;
        if ((double)TB_WINDOW_DEFAULT_WIDTH * fit > room_width)
        {
            fit = room_width / (double)TB_WINDOW_DEFAULT_WIDTH;
        }
        if ((double)TB_WINDOW_DEFAULT_HEIGHT * fit > room_height)
        {
            fit = room_height / (double)TB_WINDOW_DEFAULT_HEIGHT;
        }
        width = (uint32_t)((double)TB_WINDOW_DEFAULT_WIDTH * fit);
        height = (uint32_t)((double)TB_WINDOW_DEFAULT_HEIGHT * fit);
    }
    width = tb_window_at_least(width < usable_width ? width : usable_width, TB_WINDOW_SMALLEST);
    height =
        tb_window_at_least(height < usable_height ? height : usable_height, TB_WINDOW_SMALLEST);

    placement->display = display_index;
    placement->width = width;
    placement->height = height;
    placement->unit_width = tb_window_scaled(width, unit_scale);
    placement->unit_height = tb_window_scaled(height, unit_scale);
    placement->x = (int32_t)((int64_t)display->usable_x +
                             ((int64_t)display->usable_width - placement->unit_width) / 2);
    placement->y = (int32_t)((int64_t)display->usable_y +
                             ((int64_t)display->usable_height - placement->unit_height) / 2);
}

/* ---------------------------------------------------------------------------
 * Events.
 * ------------------------------------------------------------------------- */

void tb_window_push(tb_window_system *system, const tb_window_event *event)
{
    (void)tb_array_push(system->events, event);
}

static void tb_window_push_about(tb_window *window, tb_window_event_type type)
{
    const tb_window_event event = {.type = type, .window = window, .state = window->state};

    tb_window_push(window->system, &event);
}

void tb_window_resized(tb_window *window, uint32_t pixel_width, uint32_t pixel_height,
                       double content_scale)
{
    tb_window_state *const state = &window->state;
    const bool scale_changed = content_scale != state->content_scale;
    const bool size_changed =
        pixel_width != state->pixel_width || pixel_height != state->pixel_height;

    state->pixel_width = pixel_width;
    state->pixel_height = pixel_height;
    state->content_scale = content_scale;
    state->width = (uint32_t)((double)pixel_width / content_scale + 0.5);
    state->height = (uint32_t)((double)pixel_height / content_scale + 0.5);
    if (scale_changed)
    {
        tb_window_push_about(window, TB_WINDOW_EVENT_SCALE_CHANGED);
    }
    if (size_changed)
    {
        tb_window_push_about(window, TB_WINDOW_EVENT_RESIZED);
    }
}

void tb_window_focused(tb_window *window, bool focused)
{
    if (window->state.focused == focused)
    {
        return;
    }
    window->state.focused = focused;
    tb_window_push_about(window,
                         focused ? TB_WINDOW_EVENT_FOCUS_GAINED : TB_WINDOW_EVENT_FOCUS_LOST);
}

void tb_window_minimized(tb_window *window, bool minimized)
{
    if (window->state.minimized == minimized)
    {
        return;
    }
    window->state.minimized = minimized;
    tb_window_push_about(window, minimized ? TB_WINDOW_EVENT_MINIMIZED : TB_WINDOW_EVENT_RESTORED);
}

void tb_window_key(tb_window *window, tb_key key, uint32_t scancode, bool down, bool repeat,
                   uint32_t modifiers)
{
    const tb_window_event event = {.type = down ? TB_WINDOW_EVENT_KEY_DOWN : TB_WINDOW_EVENT_KEY_UP,
                                   .window = window,
                                   .key = key,
                                   .scancode = scancode,
                                   .repeat = down && repeat,
                                   .modifiers = modifiers,
                                   .state = window->state};

    tb_window_push(window->system, &event);
}

/* An event of the mouse at a position in the units the system reads it in. */
static tb_window_event tb_window_mouse_event(tb_window *window, tb_window_event_type type, double x,
                                             double y, uint32_t modifiers)
{
    const double scale = window->state.content_scale > 0.0 ? window->state.content_scale : 1.0;
    tb_window_event event = {
        .type = type, .window = window, .modifiers = modifiers, .state = window->state};

    if (window->system->pointer_in_pixels)
    {
        event.pixel_x = x;
        event.pixel_y = y;
        event.x = x / scale;
        event.y = y / scale;
    }
    else
    {
        event.x = x;
        event.y = y;
        event.pixel_x = x * scale;
        event.pixel_y = y * scale;
    }
    return event;
}

void tb_window_mouse_button(tb_window *window, tb_mouse_button button, bool down, double x,
                            double y, uint32_t modifiers)
{
    const tb_window_event_type type =
        down ? TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN : TB_WINDOW_EVENT_MOUSE_BUTTON_UP;
    tb_window_event event = tb_window_mouse_event(window, type, x, y, modifiers);

    event.button = button;
    tb_window_push(window->system, &event);
}

void tb_window_mouse_moved(tb_window *window, double x, double y, uint32_t modifiers)
{
    tb_window_event event = {0};

    if (window->state.relative_mouse)
    {
        return;
    }
    event = tb_window_mouse_event(window, TB_WINDOW_EVENT_MOUSE_MOVED, x, y, modifiers);
    tb_window_push(window->system, &event);
}

void tb_window_mouse_wheel(tb_window *window, double notches_x, double notches_y, double x,
                           double y, uint32_t modifiers)
{
    tb_window_event event =
        tb_window_mouse_event(window, TB_WINDOW_EVENT_MOUSE_WHEEL, x, y, modifiers);

    event.delta_x = notches_x;
    event.delta_y = notches_y;
    tb_window_push(window->system, &event);
}

void tb_window_mouse_relative(tb_window *window, double delta_x, double delta_y)
{
    const tb_window_event event = {.type = TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION,
                                   .window = window,
                                   .delta_x = delta_x,
                                   .delta_y = delta_y,
                                   .state = window->state};

    if (!window->state.relative_mouse)
    {
        return;
    }
    tb_window_push(window->system, &event);
}

void tb_window_mouse_crossed(tb_window *window, bool inside, double x, double y)
{
    tb_window_event event = {0};

    if (window->mouse_inside == inside)
    {
        return;
    }
    window->mouse_inside = inside;
    if (inside)
    {
        event = tb_window_mouse_event(window, TB_WINDOW_EVENT_MOUSE_ENTERED, x, y, 0);
    }
    else
    {
        event = (tb_window_event){
            .type = TB_WINDOW_EVENT_MOUSE_LEFT, .window = window, .state = window->state};
    }
    tb_window_push(window->system, &event);
}

/* Whether a character is a control character, which a key reports and text
 * does not: C0, DEL, and C1. */
static bool tb_window_is_control(uint32_t scalar)
{
    return scalar < 0x20u || (scalar >= 0x7Fu && scalar < 0xA0u);
}

static void tb_window_push_text(tb_window *window, const char *text, size_t length)
{
    tb_window_event event = {
        .type = TB_WINDOW_EVENT_TEXT_INPUT, .window = window, .state = window->state};

    memcpy(event.text, text, length);
    event.text[length] = '\0';
    tb_window_push(window->system, &event);
}

void tb_window_text(tb_window *window, const char *text, size_t length)
{
    char pending[TB_WINDOW_TEXT_CAPACITY] = {0};
    size_t pending_length = 0;
    size_t offset = 0;

    while (offset < length)
    {
        uint32_t scalar = 0;
        size_t next = 0;
        size_t size = 0;
        if (!tb_utf8_decode(text, length, offset, &scalar, &next))
        {
            /* A byte that begins no character is skipped. */
            offset++;
            continue;
        }
        size = next - offset;
        if (!tb_window_is_control(scalar))
        {
            if (pending_length + size >= TB_WINDOW_TEXT_CAPACITY)
            {
                tb_window_push_text(window, pending, pending_length);
                pending_length = 0;
            }
            memcpy(pending + pending_length, text + offset, size);
            pending_length += size;
        }
        offset = next;
    }
    if (pending_length > 0)
    {
        tb_window_push_text(window, pending, pending_length);
    }
}

void tb_window_displays_changed(tb_window_system *system)
{
    const tb_window_event event = {.type = TB_WINDOW_EVENT_DISPLAYS_CHANGED};

    tb_window_push(system, &event);
}

void tb_window_woken(tb_window_system *system)
{
    const tb_window_event event = {.type = TB_WINDOW_EVENT_WAKE};

    tb_window_push(system, &event);
}

/* ---------------------------------------------------------------------------
 * The window system.
 * ------------------------------------------------------------------------- */

tb_window_result tb_window_system_open(const tb_window_system_options *options,
                                       tb_window_system **system, tb_window_error *error)
{
    const tb_window_system_options defaults = {0};
    tb_window_system *opened = NULL;
    tb_window_result result = TB_WINDOW_OK;

    TB_ASSERT(system != NULL);
    *system = NULL;
    if (options == NULL)
    {
        options = &defaults;
    }
    TB_ASSERT_MSG(options->backend <= TB_WINDOW_BACKEND_COCOA,
                  "the backend is a tb_window_backend");
    tb_window_succeed(error);
    opened = tb_calloc(1, sizeof(tb_window_system));
    opened->displays = TB_ARRAY_CREATE(NULL, tb_display);
    opened->events = TB_ARRAY_CREATE(NULL, tb_window_event);
    opened->windows = TB_ARRAY_CREATE(NULL, tb_window *);
    result = tb_native_window_open(opened, options->backend, error);
    if (result != TB_WINDOW_OK)
    {
        tb_array_destroy(opened->windows);
        tb_array_destroy(opened->events);
        tb_array_destroy(opened->displays);
        tb_free(opened);
        return result;
    }
    if (tb_array_length(opened->displays) == 0)
    {
        tb_window_set_displays(opened, NULL, 0);
    }
    *system = opened;
    return TB_WINDOW_OK;
}

void tb_window_system_close(tb_window_system *system)
{
    if (system == NULL)
    {
        return;
    }
    TB_ASSERT_MSG(tb_array_length(system->windows) == 0,
                  "every window is destroyed before its window system is closed");
    system->ops->close(system);
    tb_array_destroy(system->windows);
    tb_array_destroy(system->events);
    tb_array_destroy(system->displays);
    tb_free(system);
}

tb_window_backend tb_window_system_backend(const tb_window_system *system)
{
    TB_ASSERT(system != NULL);
    return system->backend;
}

size_t tb_window_system_display_count(const tb_window_system *system)
{
    TB_ASSERT(system != NULL);
    return tb_array_length(system->displays);
}

void tb_window_system_display(const tb_window_system *system, size_t index, tb_display *display)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(display != NULL);
    TB_ASSERT_MSG(index < tb_array_length(system->displays), "index < display count");
    *display = *TB_ARRAY_AT(system->displays, tb_display, index);
}

static bool tb_window_take_event(tb_window_system *system, tb_window_event *event)
{
    if (system->next_event == tb_array_length(system->events))
    {
        return false;
    }
    *event = *TB_ARRAY_AT(system->events, tb_window_event, system->next_event);
    system->next_event++;
    if (system->next_event == tb_array_length(system->events))
    {
        tb_array_clear(system->events);
        system->next_event = 0;
    }
    return true;
}

bool tb_window_system_poll(tb_window_system *system, tb_window_event *event)
{
    TB_ASSERT(system != NULL);
    TB_ASSERT(event != NULL);
    if (tb_window_take_event(system, event))
    {
        return true;
    }
    system->ops->pump(system, 0);
    return tb_window_take_event(system, event);
}

void tb_window_system_wait(tb_window_system *system, uint64_t nanoseconds)
{
    TB_ASSERT(system != NULL);
    if (system->next_event < tb_array_length(system->events))
    {
        return;
    }
    system->ops->pump(system, nanoseconds);
}

void tb_window_system_wake(tb_window_system *system)
{
    TB_ASSERT(system != NULL);
    system->ops->wake(system);
}

/* ---------------------------------------------------------------------------
 * Windows.
 * ------------------------------------------------------------------------- */

static void tb_window_check_options(const tb_window_options *options)
{
    TB_ASSERT_MSG(options->mode <= TB_WINDOW_MODE_FULLSCREEN, "the mode is a tb_window_mode");
    (void)options;
}

static char *tb_window_copy_title(const char *title)
{
    return tb_str_dup(NULL, title != NULL ? title : "");
}

tb_window_result tb_window_create(tb_window_system *system, const tb_window_options *options,
                                  tb_window **window, tb_window_error *error)
{
    const tb_window_options defaults = {0};
    tb_window *created = NULL;
    tb_window_result result = TB_WINDOW_OK;

    TB_ASSERT(system != NULL);
    TB_ASSERT(window != NULL);
    *window = NULL;
    if (options == NULL)
    {
        options = &defaults;
    }
    tb_window_check_options(options);
    tb_window_succeed(error);
    created = tb_calloc(1, sizeof(tb_window));
    created->system = system;
    created->options = *options;
    created->title = tb_window_copy_title(options->title);
    created->options.title = created->title;
    created->state.content_scale = 1.0;
    result = system->ops->create(created, error);
    if (result != TB_WINDOW_OK)
    {
        tb_free(created->title);
        tb_free(created);
        return result;
    }
    (void)tb_array_push(system->windows, &created);
    *window = created;
    return TB_WINDOW_OK;
}

/* Forget the queued events of a window that is going, so that none names it. */
static void tb_window_forget_events(tb_window *window)
{
    tb_window_system *const system = window->system;
    size_t kept = system->next_event;

    for (size_t index = system->next_event; index < tb_array_length(system->events); index++)
    {
        const tb_window_event event = *TB_ARRAY_AT(system->events, tb_window_event, index);
        if (event.window != window)
        {
            tb_array_put(system->events, kept, &event);
            kept++;
        }
    }
    tb_array_resize(system->events, kept);
}

void tb_window_destroy(tb_window *window)
{
    tb_window_system *system = NULL;

    if (window == NULL)
    {
        return;
    }
    system = window->system;
    if (window->state.relative_mouse)
    {
        (void)system->ops->set_relative_mouse(window, false, NULL);
        window->state.relative_mouse = false;
    }
    system->ops->destroy(window);
    tb_window_forget_events(window);
    for (size_t index = 0; index < tb_array_length(system->windows); index++)
    {
        if (*TB_ARRAY_AT(system->windows, tb_window *, index) == window)
        {
            tb_array_remove(system->windows, index, NULL);
            break;
        }
    }
    tb_free(window->title);
    tb_free(window);
}

tb_window_result tb_window_configure(tb_window *window, const tb_window_options *options,
                                     tb_window_error *error)
{
    tb_window_options previous = {0};
    char *previous_title = NULL;
    tb_window_result result = TB_WINDOW_OK;

    TB_ASSERT(window != NULL);
    TB_ASSERT(options != NULL);
    tb_window_check_options(options);
    tb_window_succeed(error);
    previous = window->options;
    previous_title = window->title;
    window->options = *options;
    window->title = tb_window_copy_title(options->title != NULL ? options->title : previous_title);
    window->options.title = window->title;
    result = window->system->ops->configure(window, &previous, error);
    if (result != TB_WINDOW_OK)
    {
        tb_free(window->title);
        window->title = previous_title;
        window->options = previous;
        return result;
    }
    tb_free(previous_title);
    return TB_WINDOW_OK;
}

void tb_window_state_of(const tb_window *window, tb_window_state *state)
{
    TB_ASSERT(window != NULL);
    TB_ASSERT(state != NULL);
    *state = window->state;
}

void tb_window_native_of(const tb_window *window, tb_window_native *native)
{
    TB_ASSERT(window != NULL);
    TB_ASSERT(native != NULL);
    *native = (tb_window_native){.backend = window->system->backend};
    window->system->ops->native(window, native);
}

tb_window_result tb_window_confirm(tb_window *window, const char *title, const char *message,
                                   bool *confirmed)
{
    TB_ASSERT(window != NULL);
    TB_ASSERT(title != NULL && message != NULL);
    TB_ASSERT(confirmed != NULL);
    return window->system->ops->confirm(window, title, message, confirmed);
}

/* ---------------------------------------------------------------------------
 * The mouse.
 * ------------------------------------------------------------------------- */

tb_window_result tb_window_set_relative_mouse(tb_window *window, bool relative,
                                              tb_window_error *error)
{
    tb_window_result result = TB_WINDOW_OK;

    TB_ASSERT(window != NULL);
    tb_window_succeed(error);
    if (window->state.relative_mouse == relative)
    {
        return TB_WINDOW_OK;
    }
    result = window->system->ops->set_relative_mouse(window, relative, error);
    if (result != TB_WINDOW_OK)
    {
        return result;
    }
    window->state.relative_mouse = relative;
    window->system->ops->update_cursor(window);
    return TB_WINDOW_OK;
}

void tb_window_set_cursor(tb_window *window, tb_cursor cursor)
{
    TB_ASSERT(window != NULL);
    TB_ASSERT_MSG((unsigned)cursor < TB_CURSOR_COUNT, "the cursor is a tb_cursor");
    window->cursor = cursor;
    window->system->ops->update_cursor(window);
}

void tb_window_show_cursor(tb_window *window, bool shown)
{
    TB_ASSERT(window != NULL);
    window->cursor_hidden = !shown;
    window->system->ops->update_cursor(window);
}

/* ---------------------------------------------------------------------------
 * Names.
 * ------------------------------------------------------------------------- */

const char *tb_window_backend_name(tb_window_backend backend)
{
    switch (backend)
    {
    case TB_WINDOW_BACKEND_AUTOMATIC:
        return "automatic";
    case TB_WINDOW_BACKEND_WIN32:
        return "win32";
    case TB_WINDOW_BACKEND_WAYLAND:
        return "wayland";
    case TB_WINDOW_BACKEND_X11:
        return "x11";
    case TB_WINDOW_BACKEND_COCOA:
        return "cocoa";
    }
    return "unknown backend";
}

const char *tb_key_name(tb_key key)
{
    static const char *const names[TB_KEY_COUNT] = {
        [TB_KEY_UNKNOWN] = "unknown",
        [TB_KEY_A] = "a",
        [TB_KEY_B] = "b",
        [TB_KEY_C] = "c",
        [TB_KEY_D] = "d",
        [TB_KEY_E] = "e",
        [TB_KEY_F] = "f",
        [TB_KEY_G] = "g",
        [TB_KEY_H] = "h",
        [TB_KEY_I] = "i",
        [TB_KEY_J] = "j",
        [TB_KEY_K] = "k",
        [TB_KEY_L] = "l",
        [TB_KEY_M] = "m",
        [TB_KEY_N] = "n",
        [TB_KEY_O] = "o",
        [TB_KEY_P] = "p",
        [TB_KEY_Q] = "q",
        [TB_KEY_R] = "r",
        [TB_KEY_S] = "s",
        [TB_KEY_T] = "t",
        [TB_KEY_U] = "u",
        [TB_KEY_V] = "v",
        [TB_KEY_W] = "w",
        [TB_KEY_X] = "x",
        [TB_KEY_Y] = "y",
        [TB_KEY_Z] = "z",
        [TB_KEY_0] = "0",
        [TB_KEY_1] = "1",
        [TB_KEY_2] = "2",
        [TB_KEY_3] = "3",
        [TB_KEY_4] = "4",
        [TB_KEY_5] = "5",
        [TB_KEY_6] = "6",
        [TB_KEY_7] = "7",
        [TB_KEY_8] = "8",
        [TB_KEY_9] = "9",
        [TB_KEY_F1] = "f1",
        [TB_KEY_F2] = "f2",
        [TB_KEY_F3] = "f3",
        [TB_KEY_F4] = "f4",
        [TB_KEY_F5] = "f5",
        [TB_KEY_F6] = "f6",
        [TB_KEY_F7] = "f7",
        [TB_KEY_F8] = "f8",
        [TB_KEY_F9] = "f9",
        [TB_KEY_F10] = "f10",
        [TB_KEY_F11] = "f11",
        [TB_KEY_F12] = "f12",
        [TB_KEY_ESCAPE] = "escape",
        [TB_KEY_ENTER] = "enter",
        [TB_KEY_SPACE] = "space",
        [TB_KEY_TAB] = "tab",
        [TB_KEY_BACKSPACE] = "backspace",
        [TB_KEY_GRAVE] = "grave",
        [TB_KEY_LEFT] = "left",
        [TB_KEY_RIGHT] = "right",
        [TB_KEY_UP] = "up",
        [TB_KEY_DOWN] = "down",
        [TB_KEY_LEFT_SHIFT] = "left_shift",
        [TB_KEY_RIGHT_SHIFT] = "right_shift",
        [TB_KEY_LEFT_CONTROL] = "left_control",
        [TB_KEY_RIGHT_CONTROL] = "right_control",
        [TB_KEY_LEFT_ALT] = "left_alt",
        [TB_KEY_RIGHT_ALT] = "right_alt",
        [TB_KEY_HOME] = "home",
        [TB_KEY_END] = "end",
        [TB_KEY_PAGE_UP] = "page_up",
        [TB_KEY_PAGE_DOWN] = "page_down",
        [TB_KEY_INSERT] = "insert",
        [TB_KEY_DELETE] = "delete",
    };

    if ((unsigned)key >= TB_KEY_COUNT)
    {
        return "unknown";
    }
    return names[key];
}

const char *tb_mouse_button_name(tb_mouse_button button)
{
    switch (button)
    {
    case TB_MOUSE_BUTTON_NONE:
        return "none";
    case TB_MOUSE_BUTTON_LEFT:
        return "left";
    case TB_MOUSE_BUTTON_RIGHT:
        return "right";
    case TB_MOUSE_BUTTON_MIDDLE:
        return "middle";
    case TB_MOUSE_BUTTON_X1:
        return "x1";
    case TB_MOUSE_BUTTON_X2:
        return "x2";
    case TB_MOUSE_BUTTON_COUNT:
        break;
    }
    return "none";
}
