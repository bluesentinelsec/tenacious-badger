#include "badger/window.h"

#include "not_implemented.h"

/* Standard C has no windows: what a window is, and how the system tells a
 * program about it, is each system's own. */

tb_window_result tb_window_system_open(const tb_window_system_options *options,
                                       tb_window_system **system, tb_window_error *error)
{
    (void)options;
    (void)system;
    (void)error;
    tb_default_not_implemented("platform.window", "tb_window_system_open");
}

void tb_window_system_close(tb_window_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.window", "tb_window_system_close");
}

tb_window_backend tb_window_system_backend(const tb_window_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.window", "tb_window_system_backend");
}

size_t tb_window_system_display_count(const tb_window_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.window", "tb_window_system_display_count");
}

void tb_window_system_display(const tb_window_system *system, size_t index, tb_display *display)
{
    (void)system;
    (void)index;
    (void)display;
    tb_default_not_implemented("platform.window", "tb_window_system_display");
}

bool tb_window_system_poll(tb_window_system *system, tb_window_event *event)
{
    (void)system;
    (void)event;
    tb_default_not_implemented("platform.window", "tb_window_system_poll");
}

void tb_window_system_wait(tb_window_system *system, uint64_t nanoseconds)
{
    (void)system;
    (void)nanoseconds;
    tb_default_not_implemented("platform.window", "tb_window_system_wait");
}

void tb_window_system_wake(tb_window_system *system)
{
    (void)system;
    tb_default_not_implemented("platform.window", "tb_window_system_wake");
}

void tb_window_system_set_refresh(tb_window_system *system, tb_window_refresh_function *refresh,
                                  void *context)
{
    (void)system;
    (void)refresh;
    (void)context;
    tb_default_not_implemented("platform.window", "tb_window_system_set_refresh");
}

tb_window_result tb_window_create(tb_window_system *system, const tb_window_options *options,
                                  tb_window **window, tb_window_error *error)
{
    (void)system;
    (void)options;
    (void)window;
    (void)error;
    tb_default_not_implemented("platform.window", "tb_window_create");
}

void tb_window_destroy(tb_window *window)
{
    (void)window;
    tb_default_not_implemented("platform.window", "tb_window_destroy");
}

tb_window_result tb_window_configure(tb_window *window, const tb_window_options *options,
                                     tb_window_error *error)
{
    (void)window;
    (void)options;
    (void)error;
    tb_default_not_implemented("platform.window", "tb_window_configure");
}

void tb_window_state_of(const tb_window *window, tb_window_state *state)
{
    (void)window;
    (void)state;
    tb_default_not_implemented("platform.window", "tb_window_state_of");
}

void tb_window_native_of(const tb_window *window, tb_window_native *native)
{
    (void)window;
    (void)native;
    tb_default_not_implemented("platform.window", "tb_window_native_of");
}

tb_window_result tb_window_confirm(tb_window *window, const char *title, const char *message,
                                   bool *confirmed)
{
    (void)window;
    (void)title;
    (void)message;
    (void)confirmed;
    tb_default_not_implemented("platform.window", "tb_window_confirm");
}

const char *tb_window_backend_name(tb_window_backend backend)
{
    (void)backend;
    tb_default_not_implemented("platform.window", "tb_window_backend_name");
}

const char *tb_key_name(tb_key key)
{
    (void)key;
    tb_default_not_implemented("platform.window", "tb_key_name");
}

tb_window_result tb_window_set_relative_mouse(tb_window *window, bool relative,
                                              tb_window_error *error)
{
    (void)window;
    (void)relative;
    (void)error;
    tb_default_not_implemented("platform.window", "tb_window_set_relative_mouse");
}

void tb_window_set_cursor(tb_window *window, tb_cursor cursor)
{
    (void)window;
    (void)cursor;
    tb_default_not_implemented("platform.window", "tb_window_set_cursor");
}

void tb_window_show_cursor(tb_window *window, bool shown)
{
    (void)window;
    (void)shown;
    tb_default_not_implemented("platform.window", "tb_window_show_cursor");
}

const char *tb_mouse_button_name(tb_mouse_button button)
{
    (void)button;
    tb_default_not_implemented("platform.window", "tb_mouse_button_name");
}
