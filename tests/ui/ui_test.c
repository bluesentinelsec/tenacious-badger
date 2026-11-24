/* The interface driven by input made by hand, with no window: clicks,
 * drags, keys, and text, frame by frame, and what each widget returns and
 * draws. A window is never under the mouse in its first frame, which has
 * not been drawn yet, so each case draws a frame before it points at one. */
#include "badger/font_catalog.h"
#include "badger/str.h"

#include "tb_test.h"
#include "ui_fixture.h"

#include <string.h>

static const uint32_t left_button = 1u << TB_MOUSE_BUTTON_LEFT;

/* ---------------------------------------------------------------------------
 * Buttons and clicks.
 * ------------------------------------------------------------------------- */

static void button_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Buttons", UI_WINDOW_RECT, 0))
    {
        fixture->returned[0] = tb_ui_button(fixture->ui, "Press me");
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        tb_ui_end_window(fixture->ui);
    }
}

/* A click is the button going down and coming up on the widget: it returns
 * true in that frame alone. */
TB_TEST(ui_button, a_click_returns_true_exactly_once)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = button_scene;
    ui_frame(&fixture);
    const tb_vec2 middle = ui_middle(ui_rect_of(&fixture, fixture.ids[0]));

    ui_move(&fixture, middle.x, middle.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).hot, ==, fixture.ids[0]);
    TB_TEST_EXPECT(!fixture.returned[0]);
    ui_press(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[0]);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).active, ==, fixture.ids[0]);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[0]);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[0]);

    /* Down and up within one frame is a click too. */
    ui_click(&fixture, middle);
    TB_TEST_EXPECT(fixture.returned[0]);

    /* Down on it and up away from it is not. */
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, 700.0f, 550.0f);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[0]);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).active, ==, 0);
    ui_fixture_close(&fixture);
}

/* At 200%, the input's pixels are halved into units: a click at twice a
 * button's place in units is on it. */
TB_TEST(ui_button, input_in_pixels_is_scaled_to_units)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 2.0f, 0);
    fixture.scene = button_scene;
    ui_frame(&fixture);
    const tb_canvas_rect rect = ui_rect_of(&fixture, fixture.ids[0]);
    TB_TEST_EXPECT_FLOAT_NEAR(
        rect.x, UI_WINDOW_RECT.x + tb_ui_get_theme(fixture.ui)->window_padding.x, 0.5f);
    ui_frame(&fixture);
    ui_click(&fixture, ui_middle(rect));
    TB_TEST_EXPECT(fixture.returned[0]);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Checkboxes, radio buttons, sliders, and dragged numbers.
 * ------------------------------------------------------------------------- */

static void values_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Values", UI_WINDOW_RECT, 0))
    {
        fixture->returned[0] = tb_ui_checkbox(fixture->ui, "Enabled", &fixture->flag);
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        fixture->returned[1] =
            tb_ui_slider_float(fixture->ui, "Amount", &fixture->value, 0.0f, 100.0f, "%.1f");
        fixture->ids[1] = tb_ui_last_id(fixture->ui);
        fixture->returned[2] = tb_ui_radio(fixture->ui, "One", &fixture->choice, 1);
        tb_ui_same_line(fixture->ui);
        fixture->returned[3] = tb_ui_radio(fixture->ui, "Two", &fixture->choice, 2);
        fixture->ids[3] = tb_ui_last_id(fixture->ui);
        fixture->returned[4] =
            tb_ui_drag_float(fixture->ui, "Speed", &fixture->other, 0.5f, 0.0f, 0.0f, "%.2f");
        fixture->ids[4] = tb_ui_last_id(fixture->ui);
        fixture->returned[5] = tb_ui_slider_int(fixture->ui, "Count", &fixture->number, 0, 10);
        fixture->ids[5] = tb_ui_last_id(fixture->ui);
        fixture->returned[6] = tb_ui_drag_int(fixture->ui, "Whole", &fixture->choice, 0.1f, -5, 5);
        fixture->ids[6] = tb_ui_last_id(fixture->ui);
        tb_ui_end_window(fixture->ui);
    }
}

TB_TEST(ui_checkbox, a_click_toggles_it)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = values_scene;
    ui_frame(&fixture);
    const tb_vec2 box = ui_middle(ui_rect_of(&fixture, fixture.ids[0]));
    ui_click(&fixture, box);
    TB_TEST_EXPECT(fixture.returned[0]);
    TB_TEST_EXPECT(fixture.flag);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[0]);
    ui_click(&fixture, box);
    TB_TEST_EXPECT(fixture.returned[0]);
    TB_TEST_EXPECT(!fixture.flag);
    ui_fixture_close(&fixture);
}

TB_TEST(ui_radio, a_click_chooses_its_option)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = values_scene;
    fixture.choice = 1;
    ui_frame(&fixture);
    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[3])));
    TB_TEST_EXPECT(fixture.returned[3]);
    TB_TEST_EXPECT_INT(fixture.choice, ==, 2);
    /* Choosing the option already chosen changes nothing. */
    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[3])));
    TB_TEST_EXPECT(!fixture.returned[3]);
    ui_fixture_close(&fixture);
}

/* The value is where the mouse is along the slider: a quarter of the way
 * is a quarter of the range, and dragging follows the mouse, kept within. */
TB_TEST(ui_slider, dragging_sets_the_value_in_proportion)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = values_scene;
    ui_frame(&fixture);
    const tb_canvas_rect field = ui_rect_of(&fixture, fixture.ids[1]);
    const float middle = field.y + field.height * 0.5f;

    ui_move(&fixture, field.x + field.width * 0.25f, middle);
    ui_press(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[1]);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.value, 25.0f, 0.6f);
    ui_move(&fixture, field.x + field.width * 0.75f, middle + 80.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.value, 75.0f, 0.6f);
    TB_TEST_EXPECT(tb_ui_wants_mouse(fixture.ui));
    ui_move(&fixture, field.x + field.width * 3.0f, middle);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.value, ==, 100.0f);
    ui_release(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, field.x + field.width * 0.5f, middle);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[1]);
    TB_TEST_EXPECT_FLOAT(fixture.value, ==, 100.0f);
    ui_fixture_close(&fixture);
}

/* A whole-number slider rounds to the nearest. */
TB_TEST(ui_slider, a_whole_slider_rounds)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = values_scene;
    ui_frame(&fixture);
    const tb_canvas_rect field = ui_rect_of(&fixture, fixture.ids[5]);
    ui_click(&fixture, (tb_vec2){field.x + field.width * 0.32f, field.y + 4.0f});
    TB_TEST_EXPECT(fixture.returned[5]);
    TB_TEST_EXPECT_INT(fixture.number, ==, 3);
    ui_fixture_close(&fixture);
}

/* A number dragged changes by its speed for each unit the mouse moves, ten
 * times as fast with Shift, from where it was when the drag began. */
TB_TEST(ui_drag, the_value_follows_the_mouse_at_its_speed)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = values_scene;
    fixture.other = 1.0f;
    ui_frame(&fixture);
    const tb_vec2 start = ui_middle(ui_rect_of(&fixture, fixture.ids[4]));

    ui_move(&fixture, start.x, start.y);
    ui_press(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[4]);
    TB_TEST_EXPECT_UINT(tb_ui_cursor(fixture.ui), ==, TB_CURSOR_RESIZE_HORIZONTAL);
    ui_move(&fixture, start.x + 10.0f, start.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[4]);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.other, 6.0f, 0.001f);
    fixture.input.modifiers = TB_MODIFIER_SHIFT;
    ui_move(&fixture, start.x + 12.0f, start.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.other, 16.0f, 0.001f);
    fixture.input.modifiers = 0;
    ui_release(&fixture);
    ui_frame(&fixture);
    ui_fixture_close(&fixture);
}

/* A whole number dragged rounds what the mouse has moved, and keeps within
 * its limits; Left and Right step it by one once Tab focuses it. */
TB_TEST(ui_drag, a_whole_number_rounds_and_steps)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = values_scene;
    ui_frame(&fixture);
    const tb_vec2 start = ui_middle(ui_rect_of(&fixture, fixture.ids[6]));
    ui_move(&fixture, start.x, start.y);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, start.x + 26.0f, start.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[6]);
    TB_TEST_EXPECT_INT(fixture.choice, ==, 3);
    ui_move(&fixture, start.x + 300.0f, start.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT_INT(fixture.choice, ==, 5);
    ui_release(&fixture);
    ui_frame(&fixture);

    /* Shift+Tab from no focus is the last widget: this one. */
    ui_click(&fixture, (tb_vec2){700.0f, 550.0f});
    ui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[6]);
    ui_key(&fixture, TB_KEY_LEFT, 0);
    ui_key(&fixture, TB_KEY_LEFT, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_INT(fixture.choice, ==, 3);
    ui_key(&fixture, TB_KEY_RIGHT, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_INT(fixture.choice, ==, 4);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * The keyboard: Tab, and keys on a focused widget.
 * ------------------------------------------------------------------------- */

static void focus_scene(ui_fixture *fixture)
{
    static const char *const items[] = {"Low", "High"};

    if (tb_ui_begin_window(fixture->ui, "Focus", UI_WINDOW_RECT, 0))
    {
        fixture->returned[0] = tb_ui_button(fixture->ui, "First");
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        fixture->returned[1] = tb_ui_checkbox(fixture->ui, "Second", &fixture->flag);
        fixture->ids[1] = tb_ui_last_id(fixture->ui);
        fixture->returned[2] =
            tb_ui_slider_float(fixture->ui, "Third", &fixture->value, 0.0f, 10.0f, NULL);
        fixture->ids[2] = tb_ui_last_id(fixture->ui);
        fixture->returned[3] = tb_ui_combo(fixture->ui, "Fourth", &fixture->selected, items, 2);
        fixture->ids[3] = tb_ui_last_id(fixture->ui);
        fixture->returned[4] = tb_ui_header(fixture->ui, "Fifth", false);
        fixture->ids[4] = tb_ui_last_id(fixture->ui);
        tb_ui_end_window(fixture->ui);
    }
}

/* Tab moves the focus down the window, and around; Shift+Tab back; and the
 * interface wants the keyboard while Tab holds the focus. */
TB_TEST(ui_focus, tab_moves_the_focus_through_the_window)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = focus_scene;
    ui_frame(&fixture);
    TB_TEST_EXPECT(!tb_ui_wants_keyboard(fixture.ui));
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[0]);
    TB_TEST_EXPECT(tb_ui_wants_keyboard(fixture.ui));
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[1]);
    ui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[0]);
    /* Back from the first is the last. */
    ui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[4]);
    ui_key(&fixture, TB_KEY_ESCAPE, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, 0);
    TB_TEST_EXPECT(!tb_ui_wants_keyboard(fixture.ui));

    /* Shift+Tab with no focus starts from the last. */
    ui_key(&fixture, TB_KEY_TAB, TB_MODIFIER_SHIFT);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[4]);
    ui_fixture_close(&fixture);
}

/* A widget that Tab focused takes keys: Space presses a button and toggles a
 * checkbox, arrows move a slider and a combo's choice, and open a header. */
TB_TEST(ui_focus, keys_work_the_focused_widget)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = focus_scene;
    fixture.value = 5.0f;
    ui_frame(&fixture);
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_frame(&fixture);
    ui_key(&fixture, TB_KEY_SPACE, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[0]);
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_key(&fixture, TB_KEY_ENTER, 0);
    ui_frame(&fixture);
    ui_key(&fixture, TB_KEY_ENTER, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[1]);
    TB_TEST_EXPECT(fixture.flag);
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_frame(&fixture);
    ui_key(&fixture, TB_KEY_RIGHT, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[2]);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.value, 5.1f, 0.0001f);
    ui_key(&fixture, TB_KEY_LEFT, 0);
    ui_key(&fixture, TB_KEY_LEFT, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.value, 4.9f, 0.0001f);
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_frame(&fixture);
    ui_key(&fixture, TB_KEY_DOWN, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[3]);
    TB_TEST_EXPECT_UINT(fixture.selected, ==, 1);
    ui_key(&fixture, TB_KEY_UP, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(fixture.selected, ==, 0);
    ui_key(&fixture, TB_KEY_TAB, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[4]);
    ui_key(&fixture, TB_KEY_RIGHT, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[4]);
    ui_key(&fixture, TB_KEY_LEFT, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[4]);
    ui_fixture_close(&fixture);
}

/* A click focuses a widget, but takes no keys from the game; a click away
 * from every widget takes the focus away. */
TB_TEST(ui_focus, a_click_focuses_without_taking_the_keyboard)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = focus_scene;
    ui_frame(&fixture);
    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[0])));
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, fixture.ids[0]);
    TB_TEST_EXPECT(!tb_ui_wants_keyboard(fixture.ui));
    ui_key(&fixture, TB_KEY_SPACE, 0);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.returned[0]);
    ui_click(&fixture, (tb_vec2){700.0f, 550.0f});
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).focus, ==, 0);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Combos.
 * ------------------------------------------------------------------------- */

static void combo_scene(ui_fixture *fixture)
{
    static const char *const items[] = {"Low", "Medium", "High", "Ultra"};

    if (tb_ui_begin_window(fixture->ui, "Combo", UI_WINDOW_RECT, 0))
    {
        fixture->returned[0] = tb_ui_combo(fixture->ui, "Quality", &fixture->selected, items, 4);
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        fixture->returned[1] = tb_ui_button(fixture->ui, "Under");
        fixture->ids[1] = tb_ui_last_id(fixture->ui);
        tb_ui_end_window(fixture->ui);
    }
}

/* A click opens the list, over everything; a click on an item chooses it
 * and shuts the list, and the widgets under the list do not see the click. */
TB_TEST(ui_combo, it_opens_and_chooses)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = combo_scene;
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "High"), ==, NULL);
    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[0])));
    const tb_ui_command *const high = ui_text_command(&fixture, "High");
    TB_TEST_REQUIRE_POINTER(high, !=, NULL);
    TB_TEST_EXPECT_INT(high->layer, >, TB_UI_DEFAULT_LAYER + 100);
    const tb_vec2 at = ui_middle(high->rect);
    ui_move(&fixture, at.x, at.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT(tb_ui_wants_mouse(fixture.ui));
    ui_click(&fixture, at);
    TB_TEST_EXPECT(fixture.returned[0]);
    TB_TEST_EXPECT(!fixture.returned[1]);
    TB_TEST_EXPECT_UINT(fixture.selected, ==, 2);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Low"), ==, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "High"), !=, NULL);
    ui_fixture_close(&fixture);
}

/* A click away from the open list shuts it; so do Escape, and the combo. */
TB_TEST(ui_combo, it_shuts_without_choosing)
{
    ui_fixture fixture = {0};
    const tb_vec2 away = {700.0f, 550.0f};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = combo_scene;
    ui_frame(&fixture);
    const tb_vec2 combo = ui_middle(ui_rect_of(&fixture, fixture.ids[0]));
    ui_click(&fixture, combo);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Ultra"), !=, NULL);
    ui_click(&fixture, away);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Ultra"), ==, NULL);
    ui_click(&fixture, combo);
    ui_key(&fixture, TB_KEY_ESCAPE, 0);
    ui_frame(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Ultra"), ==, NULL);
    ui_click(&fixture, combo);
    ui_click(&fixture, combo);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Ultra"), ==, NULL);
    TB_TEST_EXPECT_UINT(fixture.selected, ==, 0);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Windows.
 * ------------------------------------------------------------------------- */

static void window_scene(ui_fixture *fixture)
{
    fixture->open = tb_ui_begin_window(fixture->ui, "Moving", UI_WINDOW_RECT, 0);
    if (fixture->open)
    {
        fixture->rect = tb_ui_window_rect(fixture->ui);
        tb_ui_text(fixture->ui, "Contents");
        tb_ui_end_window(fixture->ui);
    }
}

/* Its title bar drags it; its corner resizes it; its arrow collapses it. */
TB_TEST(ui_window, it_moves_resizes_and_collapses)
{
    ui_fixture fixture = {0};
    const tb_canvas_rect start = UI_WINDOW_RECT;

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = window_scene;
    ui_frame(&fixture);
    ui_move(&fixture, start.x + 100.0f, start.y + 8.0f);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, start.x + 130.0f, start.y + 48.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.rect.x, ==, start.x + 30.0f);
    TB_TEST_EXPECT_FLOAT(fixture.rect.y, ==, start.y + 40.0f);

    const tb_vec2 corner = {fixture.rect.x + fixture.rect.width - 3.0f,
                            fixture.rect.y + fixture.rect.height - 3.0f};
    ui_move(&fixture, corner.x, corner.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(tb_ui_cursor(fixture.ui), ==, TB_CURSOR_RESIZE_DIAGONAL_DOWN);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, corner.x + 50.0f, corner.y + 20.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.rect.width, ==, start.width + 50.0f);
    TB_TEST_EXPECT_FLOAT(fixture.rect.height, ==, start.height + 20.0f);

    /* The right edge resizes across alone, and a window is never smaller
     * than its least. */
    const tb_vec2 edge = {fixture.rect.x + fixture.rect.width,
                          fixture.rect.y + fixture.rect.height * 0.5f};
    ui_move(&fixture, edge.x, edge.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(tb_ui_cursor(fixture.ui), ==, TB_CURSOR_RESIZE_HORIZONTAL);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, edge.x - 1000.0f, edge.y);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.rect.width, ==, 120.0f);
    TB_TEST_EXPECT_FLOAT(fixture.rect.height, ==, start.height + 20.0f);

    ui_click(&fixture, (tb_vec2){fixture.rect.x + 10.0f, fixture.rect.y + 10.0f});
    TB_TEST_EXPECT(!fixture.open);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!fixture.open);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Contents"), ==, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Moving"), !=, NULL);
    ui_fixture_close(&fixture);
}

/* A window cannot be dragged off the frame for good: its title stays on it. */
TB_TEST(ui_window, it_stays_on_the_frame)
{
    ui_fixture fixture = {0};
    const tb_canvas_rect start = UI_WINDOW_RECT;

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = window_scene;
    ui_frame(&fixture);
    ui_move(&fixture, start.x + 100.0f, start.y + 8.0f);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, start.x - 2000.0f, start.y - 2000.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.rect.x, ==, 40.0f - start.width);
    TB_TEST_EXPECT_FLOAT(fixture.rect.y, ==, 0.0f);
    ui_fixture_close(&fixture);
}

static void two_windows_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Back", (tb_canvas_rect){20, 20, 300, 200}, 0))
    {
        fixture->returned[0] = tb_ui_button(fixture->ui, "In the back");
        tb_ui_end_window(fixture->ui);
    }
    if (tb_ui_begin_window(fixture->ui, "Front", (tb_canvas_rect){120, 60, 300, 200},
                           TB_UI_WINDOW_NO_COLLAPSE))
    {
        fixture->returned[1] = tb_ui_button(fixture->ui, "In the front");
        tb_ui_end_window(fixture->ui);
    }
}

/* The window begun last starts in front; a click on another brings it in
 * front, on a higher layer; the mouse finds the front one where they overlap. */
TB_TEST(ui_window, a_click_brings_a_window_in_front)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = two_windows_scene;
    ui_frame(&fixture);
    const int32_t back = ui_text_command(&fixture, "Back")->layer;
    const int32_t front = ui_text_command(&fixture, "Front")->layer;
    TB_TEST_EXPECT_INT(front, >, back);

    ui_move(&fixture, 200.0f, 150.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).hovered_window, ==, tb_ui_get_id(fixture.ui, "Front"));
    ui_click(&fixture, (tb_vec2){60.0f, 150.0f});
    ui_frame(&fixture);
    TB_TEST_EXPECT_INT(ui_text_command(&fixture, "Back")->layer, >,
                       ui_text_command(&fixture, "Front")->layer);
    ui_move(&fixture, 200.0f, 150.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).hovered_window, ==, tb_ui_get_id(fixture.ui, "Back"));
    ui_fixture_close(&fixture);
}

/* The interface wants the mouse over its windows, and not elsewhere. */
TB_TEST(ui_window, the_mouse_is_wanted_over_windows)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = two_windows_scene;
    ui_frame(&fixture);
    TB_TEST_EXPECT(!tb_ui_wants_mouse(fixture.ui));
    ui_move(&fixture, 100.0f, 100.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT(tb_ui_wants_mouse(fixture.ui));
    ui_move(&fixture, 700.0f, 500.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT(!tb_ui_wants_mouse(fixture.ui));
    ui_move(&fixture, 100.0f, 100.0f);
    fixture.input.mouse_outside = true;
    ui_frame(&fixture);
    TB_TEST_EXPECT(!tb_ui_wants_mouse(fixture.ui));
    ui_fixture_close(&fixture);
}

static void plain_scene(ui_fixture *fixture)
{
    static const char *const items[] = {"Low", "Medium", "High", "Ultra"};

    if (tb_ui_begin_window(fixture->ui, "Plain", (tb_canvas_rect){40, 480, 300, 100},
                           TB_UI_WINDOW_NO_TITLE | TB_UI_WINDOW_NO_RESIZE |
                               TB_UI_WINDOW_NO_SCROLLBAR))
    {
        fixture->rect = tb_ui_window_rect(fixture->ui);
        fixture->returned[0] = tb_ui_combo(fixture->ui, "Low down", &fixture->selected, items, 4);
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        for (int line = 0; line < 10; line++)
        {
            tb_ui_text(fixture->ui, "Cut off");
        }
        tb_ui_end_window(fixture->ui);
    }
    fixture->open = tb_ui_begin_window(fixture->ui, "Pinned", (tb_canvas_rect){400, 40, 300, 200},
                                       TB_UI_WINDOW_NO_MOVE);
    if (fixture->open)
    {
        fixture->other = tb_ui_window_rect(fixture->ui).x;
        tb_ui_end_window(fixture->ui);
    }
}

/* A window without a title neither moves nor collapses, nor resizes or
 * scrolls when asked not to; a window that does not move keeps its place;
 * and a combo low on the frame opens its list above it. */
TB_TEST(ui_window, flags_leave_out_what_a_window_does)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = plain_scene;
    ui_frame(&fixture);
    ui_move(&fixture, 60.0f, 482.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT(tb_ui_wants_mouse(fixture.ui));
    ui_move(&fixture, 338.0f, 578.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(tb_ui_cursor(fixture.ui), ==, TB_CURSOR_ARROW);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, 380.0f, 590.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.rect.width, ==, 300.0f);

    ui_move(&fixture, 500.0f, 48.0f);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, 560.0f, 148.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.other, ==, 400.0f);

    const tb_canvas_rect combo = ui_rect_of(&fixture, fixture.ids[0]);
    ui_click(&fixture, ui_middle(combo));
    const tb_ui_command *const ultra = ui_text_command(&fixture, "Ultra");
    TB_TEST_REQUIRE_POINTER(ultra, !=, NULL);
    TB_TEST_EXPECT(ultra->rect.y < combo.y);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Scrolling.
 * ------------------------------------------------------------------------- */

static void scroll_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Scrolling", (tb_canvas_rect){0, 0, 300, 200}, 0))
    {
        for (int line = 0; line < 30; line++)
        {
            tb_ui_textf(fixture->ui, "Line %d", line);
        }
        tb_ui_end_window(fixture->ui);
    }
}

static float line_top(const ui_fixture *fixture, const char *text)
{
    const tb_ui_command *const command = ui_text_command(fixture, text);
    TB_TEST_REQUIRE_POINTER(command, !=, NULL);
    return command->rect.y;
}

/* The wheel scrolls a window whose contents are taller than it, by three
 * lines a notch, and no further than its end. */
TB_TEST(ui_scroll, the_wheel_scrolls_a_window)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = scroll_scene;
    ui_frame(&fixture);
    ui_frame(&fixture);
    const float before = line_top(&fixture, "Line 5");
    ui_move(&fixture, 100.0f, 100.0f);
    fixture.input.wheel.y = -1.0f;
    ui_frame(&fixture);
    ui_frame(&fixture);
    const float after = line_top(&fixture, "Line 5");
    /* A line is a field's height and the spacing below it. */
    const float spacing = tb_ui_get_theme(fixture.ui)->item_spacing.y;
    const float line = line_top(&fixture, "Line 6") - line_top(&fixture, "Line 5");
    TB_TEST_EXPECT_FLOAT_NEAR(before - after, 3.0f * (line - spacing), 0.01f);
    fixture.input.wheel.y = -100.0f;
    ui_frame(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Line 0"), ==, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Line 29"), !=, NULL);
    fixture.input.wheel.y = 100.0f;
    ui_frame(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(line_top(&fixture, "Line 5"), ==, before);
    ui_fixture_close(&fixture);
}

/* Dragging the scroll bar's grab scrolls; a press on its track jumps there. */
TB_TEST(ui_scroll, the_scroll_bar_scrolls)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = scroll_scene;
    ui_frame(&fixture);
    ui_frame(&fixture);
    const float before = line_top(&fixture, "Line 5");
    /* The track is along the window's right edge, under its title, inside
     * the edge that resizes it. */
    ui_move(&fixture, 300.0f - 8.0f, 150.0f);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Line 5"), ==, NULL);
    ui_move(&fixture, 300.0f - 8.0f, 0.0f);
    ui_frame(&fixture);
    ui_release(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(line_top(&fixture, "Line 5"), ==, before);
    ui_fixture_close(&fixture);
}

static void panel_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Panels", UI_WINDOW_RECT, 0))
    {
        tb_ui_text(fixture->ui, "Above");
        tb_ui_begin_panel(fixture->ui, "List", 100.0f);
        for (int line = 0; line < 20; line++)
        {
            tb_ui_push_id_int(fixture->ui, line);
            fixture->returned[0] =
                tb_ui_selectable(fixture->ui, "Item", line == 3) || fixture->returned[0];
            tb_ui_pop_id(fixture->ui);
        }
        tb_ui_end_panel(fixture->ui);
        tb_ui_begin_panel(fixture->ui, "Rest", 0.0f);
        tb_ui_text(fixture->ui, "Below");
        tb_ui_end_panel(fixture->ui);
        tb_ui_end_window(fixture->ui);
    }
}

/* The wheel over a panel scrolls the panel, not the window around it. */
TB_TEST(ui_scroll, the_wheel_scrolls_the_innermost_panel)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = panel_scene;
    ui_frame(&fixture);
    ui_frame(&fixture);
    const float above = line_top(&fixture, "Above");
    const tb_ui_command *const first = ui_text_command(&fixture, "Item");
    TB_TEST_REQUIRE_POINTER(first, !=, NULL);
    const float item = first->rect.y;
    ui_move(&fixture, first->rect.x + 2.0f, first->rect.y + 2.0f);
    fixture.input.wheel.y = -1.0f;
    ui_frame(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(line_top(&fixture, "Above"), ==, above);
    TB_TEST_EXPECT(ui_text_command(&fixture, "Item")->rect.y < item);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Below"), !=, NULL);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Ids and state.
 * ------------------------------------------------------------------------- */

static void twins_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Twins", UI_WINDOW_RECT, 0))
    {
        for (int index = 0; index < 2; index++)
        {
            if (fixture->flag)
            {
                tb_ui_push_id_int(fixture->ui, index);
            }
            fixture->returned[index] = tb_ui_button(fixture->ui, "OK");
            fixture->ids[index] = tb_ui_last_id(fixture->ui);
            if (fixture->flag)
            {
                tb_ui_pop_id(fixture->ui);
            }
        }
        tb_ui_end_window(fixture->ui);
    }
}

/* Two widgets of one label share an id, which the frame counts; an id
 * pushed around each tells them apart, and a click reaches one alone. */
TB_TEST(ui_id, ids_pushed_tell_widgets_of_one_label_apart)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = twins_scene;
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(fixture.ids[0], ==, fixture.ids[1]);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).id_collisions, ==, 1);

    fixture.flag = true;
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(fixture.ids[0], !=, fixture.ids[1]);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).id_collisions, ==, 0);
    const tb_canvas_rect second = ui_rect_of(&fixture, fixture.ids[1]);
    ui_click(&fixture, ui_middle(second));
    TB_TEST_EXPECT(!fixture.returned[0]);
    TB_TEST_EXPECT(fixture.returned[1]);

    /* A label's text after ## is part of its id and is not shown. */
    TB_TEST_EXPECT_UINT(tb_ui_get_id(fixture.ui, "Go##1"), !=, tb_ui_get_id(fixture.ui, "Go##2"));
    ui_fixture_close(&fixture);
}

static void forgotten_scene(ui_fixture *fixture)
{
    if (fixture->flag && tb_ui_begin_window(fixture->ui, "Passing", UI_WINDOW_RECT, 0))
    {
        (void)tb_ui_button(fixture->ui, "Button");
        (void)tb_ui_header(fixture->ui, "Header", true);
        tb_ui_end_window(fixture->ui);
    }
}

/* State not used for the frames chosen is forgotten. */
TB_TEST(ui_id, unused_state_is_forgotten)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 3);
    fixture.scene = forgotten_scene;
    fixture.flag = true;
    ui_frame(&fixture);
    const size_t kept = ui_stats(&fixture).states;
    TB_TEST_EXPECT_UINT(kept, >=, 3);
    fixture.flag = false;
    for (int frame = 0; frame < 3; frame++)
    {
        ui_frame(&fixture);
        TB_TEST_EXPECT_UINT(ui_stats(&fixture).states, ==, kept);
    }
    ui_frame(&fixture);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).states, ==, 0);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Headers, trees, tables, and what is only drawn.
 * ------------------------------------------------------------------------- */

static void gallery_scene(ui_fixture *fixture)
{
    static const char *const headers[] = {"Name", "Value"};
    static const float widths[] = {80.0f, 0.0f};
    tb_ui *const ui = fixture->ui;

    if (!tb_ui_begin_window(ui, "Gallery", (tb_canvas_rect){10, 10, 420, 560}, 0))
    {
        return;
    }
    tb_ui_heading(ui, "Heading");
    tb_ui_text_colored(ui, (tb_vec4){1, 0.5f, 0.5f, 1}, "Colored");
    tb_ui_text_wrapped(ui, "Wrapped text that is long enough to need more than a line in the "
                           "window, which is not very wide.");
    tb_ui_text(ui, "Two\nlines");
    tb_ui_value(ui, "Frame", "16.6 ms");
    tb_ui_separator(ui);
    tb_ui_spacing(ui);
    fixture->open = tb_ui_header(ui, "Header", true);
    fixture->ids[0] = tb_ui_last_id(ui);
    if (fixture->open)
    {
        tb_ui_indent(ui);
        tb_ui_progress(ui, 0.42f, NULL);
        tb_ui_progress(ui, 2.0f, "Done");
        tb_ui_unindent(ui);
    }
    fixture->returned[1] = tb_ui_tree_push(ui, "Node", false);
    fixture->ids[1] = tb_ui_last_id(ui);
    if (fixture->returned[1])
    {
        tb_ui_text(ui, "Child");
        tb_ui_tree_pop(ui);
    }
    fixture->returned[2] = tb_ui_color_swatch(ui, "Swatch", (tb_vec4){0.2f, 0.6f, 0.9f, 0.5f});
    fixture->ids[2] = tb_ui_last_id(ui);
    fixture->returned[3] = tb_ui_color_edit(ui, "Tint", &fixture->color);
    tb_ui_row(ui, 3, NULL);
    (void)tb_ui_button(ui, "A");
    (void)tb_ui_button(ui, "B");
    (void)tb_ui_button(ui, "C");
    tb_ui_row(ui, 0, NULL);
    tb_ui_begin_table(ui, "Table", 2, headers, widths);
    for (int row = 0; row < 3; row++)
    {
        tb_ui_textf(ui, "Row %d", row);
        tb_ui_textf(ui, "%d", row * 10);
    }
    tb_ui_end_table(ui);
    tb_ui_begin_table(ui, "Bare", 2, NULL, NULL);
    tb_ui_text(ui, "Cell");
    tb_ui_end_table(ui);
    tb_ui_end_window(ui);
}

/* Every widget draws, and the open ones open and shut on a click. */
TB_TEST(ui_gallery, every_widget_draws_and_opens)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.5f, 0);
    fixture.scene = gallery_scene;
    fixture.color = (tb_vec4){0.5f, 0.5f, 0.5f, 1.0f};
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.open);
    TB_TEST_EXPECT(!fixture.returned[1]);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).id_collisions, ==, 0);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "42%"), !=, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Done"), !=, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Value"), !=, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Row 2"), !=, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Child"), ==, NULL);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).windows, ==, 1);
    TB_TEST_EXPECT_UINT(ui_stats(&fixture).commands, >, 50);

    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[0])));
    TB_TEST_EXPECT(!fixture.open);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "42%"), ==, NULL);
    const tb_ui_command *const node = ui_text_command(&fixture, "Node");
    TB_TEST_REQUIRE_POINTER(node, !=, NULL);
    ui_click(&fixture, ui_middle(node->rect));
    TB_TEST_EXPECT(fixture.returned[1]);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Child"), !=, NULL);
    TB_TEST_EXPECT(ui_text_command(&fixture, "Child")->rect.x > node->rect.x);

    ui_click(&fixture, ui_middle(ui_rect_of(&fixture, fixture.ids[2])));
    TB_TEST_EXPECT(fixture.returned[2]);
    ui_fixture_close(&fixture);
}

static void plot_scene(ui_fixture *fixture)
{
    static const float times[] = {16.0f, 17.5f, 15.0f, 33.0f, 16.6f};

    if (tb_ui_begin_window(fixture->ui, "Plots", UI_WINDOW_RECT, 0))
    {
        tb_ui_plot(fixture->ui, "Frame times", times, 5, 0.0f, 0.0f, 0.0f);
        tb_ui_plot(fixture->ui, NULL, times, 5, 0.0f, 40.0f, 50.0f);
        tb_ui_plot(fixture->ui, "Empty", NULL, 0, 0.0f, 0.0f, 0.0f);
        tb_ui_text_mono(fixture->ui, "pass  scene");
        tb_ui_end_window(fixture->ui);
    }
}

static size_t count_lines(const ui_fixture *fixture)
{
    size_t count = 0;
    size_t lines = 0;
    const tb_ui_command *const commands = tb_ui_commands(fixture->ui, &count);

    for (size_t index = 0; index < count; index++)
    {
        lines += commands[index].type == TB_UI_COMMAND_LINE ? 1 : 0;
    }
    return lines;
}

/* A graph joins its values with a line each, in its box; its range is the
 * values' own when none is given, and values outside one are kept in it. */
TB_TEST(ui_gallery, a_plot_joins_its_values)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = plot_scene;
    ui_frame(&fixture);
    /* Two graphs of five values, and the window's arrow and grip. */
    TB_TEST_EXPECT_UINT(count_lines(&fixture), ==, 2 * 4 + 2 + 2);
    const tb_ui_command *const label = ui_text_command(&fixture, "Frame times");
    TB_TEST_REQUIRE_POINTER(label, !=, NULL);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Empty"), !=, NULL);
    const tb_ui_command *const mono = ui_text_command(&fixture, "pass  scene");
    TB_TEST_REQUIRE_POINTER(mono, !=, NULL);
    TB_TEST_EXPECT_POINTER(mono->font, ==, fixture.font);
    ui_fixture_close(&fixture);
}

/* A color's channels are numbers dragged from 0 to 1. */
TB_TEST(ui_gallery, a_color_edit_drags_its_channels)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = gallery_scene;
    fixture.color = (tb_vec4){0.5f, 0.5f, 0.5f, 1.0f};
    ui_frame(&fixture);
    /* The first of the channels' values is red's. */
    const tb_ui_command *const red = ui_text_command(&fixture, "0.50");
    TB_TEST_REQUIRE_POINTER(red, !=, NULL);
    const tb_vec2 at = ui_middle(red->rect);
    ui_move(&fixture, at.x, at.y);
    ui_press(&fixture);
    ui_frame(&fixture);
    ui_move(&fixture, at.x + 20.0f, at.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[3]);
    TB_TEST_EXPECT_FLOAT_NEAR(fixture.color.x, 0.6f, 0.0001f);
    ui_move(&fixture, at.x + 400.0f, at.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT_FLOAT(fixture.color.x, ==, 1.0f);
    ui_release(&fixture);
    ui_frame(&fixture);
    ui_fixture_close(&fixture);
}

static void tooltip_scene(ui_fixture *fixture)
{
    if (tb_ui_begin_window(fixture->ui, "Tips", UI_WINDOW_RECT, 0))
    {
        (void)tb_ui_button(fixture->ui, "Hover");
        fixture->ids[0] = tb_ui_last_id(fixture->ui);
        fixture->returned[0] = tb_ui_item_hovered(fixture->ui);
        tb_ui_tooltip(fixture->ui, "Help text");
        tb_ui_text(fixture->ui, "No tip");
        tb_ui_tooltip(fixture->ui, "Never shown");
        tb_ui_end_window(fixture->ui);
    }
}

/* A tooltip shows once the mouse has rested on its widget for the delay. */
TB_TEST(ui_gallery, a_tooltip_shows_after_a_rest)
{
    ui_fixture fixture = {0};

    ui_fixture_open(&fixture, 1.0f, 0);
    fixture.scene = tooltip_scene;
    ui_frame(&fixture);
    const tb_vec2 middle = ui_middle(ui_rect_of(&fixture, fixture.ids[0]));
    fixture.input.delta_seconds = 0.3;
    ui_move(&fixture, middle.x, middle.y);
    ui_frame(&fixture);
    TB_TEST_EXPECT(fixture.returned[0]);
    ui_frame(&fixture);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Help text"), ==, NULL);
    ui_frame(&fixture);
    const tb_ui_command *const tip = ui_text_command(&fixture, "Help text");
    TB_TEST_REQUIRE_POINTER(tip, !=, NULL);
    TB_TEST_EXPECT_INT(tip->layer, >, TB_UI_DEFAULT_LAYER + 100);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Never shown"), ==, NULL);
    ui_move(&fixture, 700.0f, 550.0f);
    ui_frame(&fixture);
    TB_TEST_EXPECT_POINTER(ui_text_command(&fixture, "Help text"), ==, NULL);
    ui_fixture_close(&fixture);
}

/* ---------------------------------------------------------------------------
 * Input from window events, themes, and fonts.
 * ------------------------------------------------------------------------- */

/* Window events become a frame's input: the mouse in pixels, buttons held
 * and pressed, the wheel, keys in order with the text typed before them. */
TB_TEST(ui_input, window_events_become_a_frames_input)
{
    tb_ui_input input = {0};
    tb_window_event event = {0};

    event = (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_MOVED, .pixel_x = 30, .pixel_y = 40};
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_FLOAT(input.mouse.x, ==, 30.0f);
    TB_TEST_EXPECT_FLOAT(input.mouse.y, ==, 40.0f);
    event = (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
                              .button = TB_MOUSE_BUTTON_LEFT,
                              .pixel_x = 31,
                              .pixel_y = 41};
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_UINT(input.buttons, ==, left_button);
    TB_TEST_EXPECT_UINT(input.pressed, ==, left_button);
    event.type = TB_WINDOW_EVENT_MOUSE_BUTTON_UP;
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_UINT(input.buttons, ==, 0);
    TB_TEST_EXPECT_UINT(input.released, ==, left_button);
    event = (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN,
                              .button = TB_MOUSE_BUTTON_NONE};
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_UINT(input.pressed, ==, left_button);
    event = (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_WHEEL, .delta_y = -1.5};
    tb_ui_input_add_event(&input, &event);
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_FLOAT(input.wheel.y, ==, -3.0f);
    event = (tb_window_event){.type = TB_WINDOW_EVENT_TEXT_INPUT, .text = "ab"};
    tb_ui_input_add_event(&input, &event);
    event = (tb_window_event){
        .type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_BACKSPACE, .modifiers = TB_MODIFIER_SHIFT};
    tb_ui_input_add_event(&input, &event);
    event = (tb_window_event){.type = TB_WINDOW_EVENT_KEY_UP, .key = TB_KEY_LEFT_SHIFT};
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_UINT(input.modifiers, ==, 0);
    TB_TEST_EXPECT_UINT(input.key_count, ==, 1);
    TB_TEST_EXPECT_UINT(input.keys[0].text_offset, ==, 2);
    TB_TEST_EXPECT_UINT(input.keys[0].modifiers, ==, TB_MODIFIER_SHIFT);
    event = (tb_window_event){.type = TB_WINDOW_EVENT_MOUSE_LEFT};
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT(input.mouse_outside);

    /* A focus lost lets go of every button. */
    input.buttons = left_button;
    event = (tb_window_event){.type = TB_WINDOW_EVENT_FOCUS_LOST};
    tb_ui_input_add_event(&input, &event);
    TB_TEST_EXPECT_UINT(input.buttons, ==, 0);

    /* Too many keys, or too much text, are dropped. */
    for (int key = 0; key < TB_UI_MOST_KEYS + 4; key++)
    {
        event = (tb_window_event){.type = TB_WINDOW_EVENT_KEY_DOWN, .key = TB_KEY_A};
        tb_ui_input_add_event(&input, &event);
    }
    TB_TEST_EXPECT_UINT(input.key_count, ==, TB_UI_MOST_KEYS);
    for (int text = 0; text < TB_UI_TEXT_CAPACITY; text++)
    {
        event = (tb_window_event){.type = TB_WINDOW_EVENT_TEXT_INPUT, .text = "xyz"};
        tb_ui_input_add_event(&input, &event);
    }
    TB_TEST_EXPECT_UINT(input.text_length, <=, TB_UI_TEXT_CAPACITY);

    tb_ui_input_next_frame(&input);
    TB_TEST_EXPECT_UINT(input.key_count, ==, 0);
    TB_TEST_EXPECT_UINT(input.text_length, ==, 0);
    TB_TEST_EXPECT_UINT(input.pressed, ==, 0);
    TB_TEST_EXPECT_FLOAT(input.wheel.y, ==, 0.0f);
    TB_TEST_EXPECT(input.mouse_outside);
}

/* The engine's catalog gives a theme its fonts, where the build carries them. */
TB_TEST(ui_theme, fonts_come_from_the_catalogs_roles)
{
    tb_font_catalog *const catalog = tb_font_catalog_create(NULL);
    tb_ui_fonts fonts = {0};
    tb_ui_theme theme = tb_ui_theme_dark();
    tb_ui *ui = NULL;
    size_t index = 0;
    const bool carried = tb_font_catalog_find_role(catalog, TB_FONT_ROLE_UI, &index);

    TB_TEST_EXPECT(tb_ui_fonts_open(catalog, &fonts) == carried);
    tb_ui_theme_set_fonts(&theme, &fonts);
    TB_TEST_EXPECT_POINTER(theme.fonts[TB_UI_FONT_REGULAR], ==, fonts.fonts[TB_UI_FONT_REGULAR]);
    ui = tb_ui_create(NULL);
    TB_TEST_EXPECT_INT((int)tb_ui_get_theme(ui)->font_size, ==, 14);
    tb_ui_set_theme(ui, &theme);
    TB_TEST_EXPECT_POINTER(tb_ui_get_theme(ui)->fonts[TB_UI_FONT_BOLD], ==,
                           fonts.fonts[TB_UI_FONT_BOLD]);
    tb_ui_destroy(ui);
    tb_ui_destroy(NULL);
    tb_ui_fonts_close(&fonts);
    TB_TEST_EXPECT_POINTER(fonts.fonts[TB_UI_FONT_REGULAR], ==, NULL);
    tb_ui_fonts_close(NULL);
    tb_font_catalog_destroy(catalog);
}
