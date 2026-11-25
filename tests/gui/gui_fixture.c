/* The retained GUI's tests' fixture: see gui_fixture.h. */
#include "gui_fixture.h"

#include "badger/str.h"

#include "tb_test.h"
#include "test_data.h"

#include <stddef.h>
#include <string.h>

extern const tb_test_file tb_font_files[];
extern const size_t tb_font_files_count;

static tb_font *open_roboto(void)
{
    const tb_test_file *found = NULL;
    tb_font *font = NULL;

    for (size_t index = 0; index < tb_font_files_count; index++)
    {
        if (tb_str_equal(tb_font_files[index].name, "Roboto-Regular.ttf"))
        {
            found = &tb_font_files[index];
        }
    }
    TB_TEST_REQUIRE_POINTER(found, !=, NULL);
    TB_TEST_REQUIRE_UINT(tb_font_open(found->bytes, found->length, NULL, &font, NULL), ==,
                         TB_FONT_OK);
    return font;
}

void gui_fixture_open(gui_fixture *fixture, float scale)
{
    tb_ui_theme theme = tb_ui_theme_dark();

    *fixture = (gui_fixture){.mouse = {-100.0f, -100.0f}};
    fixture->font = open_roboto();
    theme.fonts[TB_UI_FONT_REGULAR] = fixture->font;
    /* A size of the cases' own, so they do not follow the UI role's size,
     * which the author tunes. */
    theme.font_size = GUI_FONT_SIZE;
    fixture->gui = tb_gui_create(&(tb_gui_options){.theme = &theme});
    gui_resize(fixture, (uint32_t)(GUI_WIDTH * scale), (uint32_t)(GUI_HEIGHT * scale), scale);
    gui_update(fixture);
}

void gui_fixture_close(gui_fixture *fixture)
{
    tb_gui_destroy(fixture->gui);
    tb_font_close(fixture->font);
    *fixture = (gui_fixture){0};
}

void gui_resize(gui_fixture *fixture, uint32_t width, uint32_t height, float scale)
{
    fixture->scale = scale;
    tb_gui_resize(fixture->gui, width, height, scale);
}

void gui_update(gui_fixture *fixture)
{
    tb_gui_update(fixture->gui, 1.0 / 60.0);
}

/* ---------------------------------------------------------------------------
 * Boxes.
 * ------------------------------------------------------------------------- */

typedef struct gui_box_data
{
    tb_vec2 size;
} gui_box_data;

static tb_vec2 gui_box_measure(tb_widget widget, const void *data)
{
    const gui_box_data *const box = data;

    (void)widget;
    return box->size;
}

static void gui_box_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    (void)widget;
    (void)data;
    tb_gui_paint_fill(paint, tb_gui_paint_rect(paint), 0.0f, (tb_vec4){1.0f, 1.0f, 1.0f, 1.0f});
}

static const tb_gui_property gui_box_properties[] = {
    {.name = "size",
     .type = TB_GUI_TYPE_VEC2,
     .offset = offsetof(gui_box_data, size),
     .flags = TB_GUI_PROPERTY_LAYOUT},
};

static const tb_widget_class gui_box_kind = {.name = "box",
                                             .data_size = sizeof(gui_box_data),
                                             .properties = gui_box_properties,
                                             .property_count = 1,
                                             .mouse_filter = TB_GUI_MOUSE_STOP,
                                             .measure = gui_box_measure,
                                             .draw = gui_box_draw};

static const tb_widget_class gui_focus_box_kind = {.name = "focus_box",
                                                   .data_size = sizeof(gui_box_data),
                                                   .properties = gui_box_properties,
                                                   .property_count = 1,
                                                   .flags = TB_WIDGET_CLASS_FOCUSABLE,
                                                   .mouse_filter = TB_GUI_MOUSE_STOP,
                                                   .measure = gui_box_measure,
                                                   .draw = gui_box_draw};

static const tb_widget_class gui_typing_box_kind = {.name = "typing_box",
                                                    .data_size = sizeof(gui_box_data),
                                                    .properties = gui_box_properties,
                                                    .property_count = 1,
                                                    .flags = TB_WIDGET_CLASS_FOCUSABLE |
                                                             TB_WIDGET_CLASS_FOCUS_ON_CLICK,
                                                    .mouse_filter = TB_GUI_MOUSE_STOP,
                                                    .measure = gui_box_measure,
                                                    .draw = gui_box_draw};

const tb_widget_class *gui_box_class(void)
{
    return &gui_box_kind;
}

const tb_widget_class *gui_focus_box_class(void)
{
    return &gui_focus_box_kind;
}

const tb_widget_class *gui_typing_box_class(void)
{
    return &gui_typing_box_kind;
}

tb_widget gui_box_of(tb_widget parent, const tb_widget_class *kind, float width, float height)
{
    const tb_widget widget = tb_widget_create(parent, kind);
    const tb_gui_value size = tb_gui_vec2((tb_vec2){width, height});

    TB_TEST_REQUIRE(tb_widget_set(widget, "size", &size));
    return widget;
}

tb_widget gui_box(tb_widget parent, float width, float height)
{
    return gui_box_of(parent, &gui_box_kind, width, height);
}

/* ---------------------------------------------------------------------------
 * Input.
 * ------------------------------------------------------------------------- */

tb_window_event gui_mouse_event(const gui_fixture *fixture, tb_window_event_type type, float x,
                                float y)
{
    const tb_gui_view view = tb_gui_view_of(fixture->gui);
    tb_window_event event = {.type = type};

    event.pixel_x = (double)(view.origin.x + x * view.scale);
    event.pixel_y = (double)(view.origin.y + y * view.scale);
    event.x = event.pixel_x / (double)fixture->scale;
    event.y = event.pixel_y / (double)fixture->scale;
    return event;
}

void gui_move(gui_fixture *fixture, float x, float y)
{
    const tb_window_event event = gui_mouse_event(fixture, TB_WINDOW_EVENT_MOUSE_MOVED, x, y);

    fixture->mouse = (tb_vec2){x, y};
    tb_gui_feed_event(fixture->gui, &event);
    gui_update(fixture);
}

static void gui_button(gui_fixture *fixture, tb_window_event_type type, tb_mouse_button button)
{
    tb_window_event event = gui_mouse_event(fixture, type, fixture->mouse.x, fixture->mouse.y);

    event.button = button;
    tb_gui_feed_event(fixture->gui, &event);
    gui_update(fixture);
}

void gui_press(gui_fixture *fixture, tb_mouse_button button)
{
    gui_button(fixture, TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN, button);
}

void gui_release(gui_fixture *fixture, tb_mouse_button button)
{
    gui_button(fixture, TB_WINDOW_EVENT_MOUSE_BUTTON_UP, button);
}

void gui_click(gui_fixture *fixture, tb_vec2 at)
{
    gui_move(fixture, at.x, at.y);
    gui_press(fixture, TB_MOUSE_BUTTON_LEFT);
    gui_release(fixture, TB_MOUSE_BUTTON_LEFT);
}

void gui_wheel(gui_fixture *fixture, float notches)
{
    tb_window_event event =
        gui_mouse_event(fixture, TB_WINDOW_EVENT_MOUSE_WHEEL, fixture->mouse.x, fixture->mouse.y);

    event.delta_y = (double)notches;
    tb_gui_feed_event(fixture->gui, &event);
    gui_update(fixture);
}

void gui_key(gui_fixture *fixture, tb_key key, uint32_t modifiers)
{
    const tb_window_event down = {
        .type = TB_WINDOW_EVENT_KEY_DOWN, .key = key, .modifiers = modifiers};
    const tb_window_event up = {.type = TB_WINDOW_EVENT_KEY_UP, .key = key};

    tb_gui_feed_event(fixture->gui, &down);
    tb_gui_feed_event(fixture->gui, &up);
    gui_update(fixture);
}

void gui_type(gui_fixture *fixture, const char *text)
{
    tb_window_event event = {.type = TB_WINDOW_EVENT_TEXT_INPUT};
    const size_t length = tb_str_length(text);

    TB_TEST_REQUIRE_UINT(length, <, sizeof(event.text));
    memcpy(event.text, text, length + 1);
    tb_gui_feed_event(fixture->gui, &event);
    gui_update(fixture);
}

void gui_leave(gui_fixture *fixture)
{
    const tb_window_event event = {.type = TB_WINDOW_EVENT_MOUSE_LEFT};

    tb_gui_feed_event(fixture->gui, &event);
    gui_update(fixture);
}

tb_vec2 gui_middle(tb_canvas_rect rect)
{
    return (tb_vec2){rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
}

/* ---------------------------------------------------------------------------
 * The log.
 * ------------------------------------------------------------------------- */

void gui_record(const tb_gui_event *event, void *user)
{
    gui_log *const log = user;

    if (log->count == GUI_LOG_CAPACITY)
    {
        return;
    }
    log->kinds[log->count] = event->kind;
    log->widgets[log->count] = event->widget;
    log->targets[log->count] = event->target;
    log->keys[log->count] = event->key;
    log->count++;
}

void gui_record_all(tb_widget widget, gui_log *log)
{
    for (int kind = 0; kind < TB_GUI_EVENT_COUNT; kind++)
    {
        TB_TEST_REQUIRE(tb_bind(widget, (tb_gui_event_kind)kind, gui_record, log));
    }
}

size_t gui_log_count(const gui_log *log, tb_gui_event_kind kind, tb_widget widget)
{
    size_t count = 0;

    for (size_t index = 0; index < log->count; index++)
    {
        if (log->kinds[index] == kind && tb_widget_equal(log->widgets[index], widget))
        {
            count++;
        }
    }
    return count;
}
