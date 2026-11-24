/* The interface's tests' fixture: see ui_fixture.h. */
#include "ui_fixture.h"

#include "badger/str.h"

#include "tb_test.h"
#include "test_data.h"

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

void ui_fixture_open(ui_fixture *fixture, float scale, uint32_t forget_frames)
{
    tb_ui_theme theme = tb_ui_theme_dark();

    *fixture = (ui_fixture){0};
    fixture->font = open_roboto();
    theme.fonts[TB_UI_FONT_REGULAR] = fixture->font;
    /* A size of the cases' own, so they do not follow the UI role's size,
     * which the author tunes. */
    theme.font_size = UI_FONT_SIZE;
    fixture->ui = tb_ui_create(&(tb_ui_options){.theme = &theme, .forget_frames = forget_frames});
    fixture->input.scale = scale;
    fixture->input.size = (tb_vec2){800.0f * scale, 600.0f * scale};
    fixture->input.delta_seconds = 1.0 / 60.0;
    fixture->input.mouse = (tb_vec2){-100.0f, -100.0f};
}

void ui_fixture_close(ui_fixture *fixture)
{
    tb_ui_destroy(fixture->ui);
    tb_font_close(fixture->font);
    *fixture = (ui_fixture){0};
}

void ui_frame(ui_fixture *fixture)
{
    tb_ui_begin_frame(fixture->ui, &fixture->input);
    if (fixture->scene != NULL)
    {
        fixture->scene(fixture);
    }
    tb_ui_end_frame(fixture->ui);
    tb_ui_input_next_frame(&fixture->input);
}

void ui_move(ui_fixture *fixture, float x, float y)
{
    const float scale = fixture->input.scale > 0.0f ? fixture->input.scale : 1.0f;

    fixture->input.mouse = (tb_vec2){x * scale, y * scale};
    fixture->input.mouse_outside = false;
}

void ui_press(ui_fixture *fixture)
{
    fixture->input.buttons |= 1u << TB_MOUSE_BUTTON_LEFT;
    fixture->input.pressed |= 1u << TB_MOUSE_BUTTON_LEFT;
}

void ui_release(ui_fixture *fixture)
{
    fixture->input.buttons &= ~(1u << TB_MOUSE_BUTTON_LEFT);
    fixture->input.released |= 1u << TB_MOUSE_BUTTON_LEFT;
}

void ui_click(ui_fixture *fixture, tb_vec2 at)
{
    ui_move(fixture, at.x, at.y);
    ui_press(fixture);
    ui_release(fixture);
    ui_frame(fixture);
}

void ui_key(ui_fixture *fixture, tb_key key, uint32_t modifiers)
{
    tb_window_event event = {.type = TB_WINDOW_EVENT_KEY_DOWN, .key = key, .modifiers = modifiers};

    tb_ui_input_add_event(&fixture->input, &event);
}

void ui_type(ui_fixture *fixture, const char *text)
{
    tb_window_event event = {.type = TB_WINDOW_EVENT_TEXT_INPUT};

    TB_TEST_REQUIRE_UINT(tb_str_length(text), <, sizeof(event.text));
    memcpy(event.text, text, tb_str_length(text) + 1);
    tb_ui_input_add_event(&fixture->input, &event);
}

tb_canvas_rect ui_rect_of(const ui_fixture *fixture, tb_ui_id id)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_ui_commands(fixture->ui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == id && (commands[index].type == TB_UI_COMMAND_RECT ||
                                         commands[index].type == TB_UI_COMMAND_CIRCLE))
        {
            return commands[index].rect;
        }
    }
    TB_TEST_FAIL("no rectangle of the widget %u", (unsigned)id);
}

const tb_ui_command *ui_text_command(const ui_fixture *fixture, const char *text)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_ui_commands(fixture->ui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].type == TB_UI_COMMAND_TEXT &&
            tb_text_equal(commands[index].text, tb_text_from_cstr(text)))
        {
            return &commands[index];
        }
    }
    return NULL;
}

tb_vec2 ui_middle(tb_canvas_rect rect)
{
    return (tb_vec2){rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
}

tb_ui_stats ui_stats(const ui_fixture *fixture)
{
    tb_ui_stats stats = {0};

    tb_ui_stats_of(fixture->ui, &stats);
    return stats;
}
