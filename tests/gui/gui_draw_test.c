/* What the retained GUI draws: the commands of frames, labels, and buttons
 * in each of their states, text set in its rectangle by its anchor and
 * justified, fonts asked for by spec, clipping, a kind's own drawing; and
 * the commands drawn on the null backend's canvas. */
#include "badger/canvas.h"
#include "badger/font_catalog.h"
#include "badger/str.h"
#include "badger/window.h"

#include "gui_fixture.h"
#include "tb_test.h"

/* The first command of a type a widget made, or NULL. */
static const tb_ui_command *command_of(const gui_fixture *fixture, tb_widget widget,
                                       tb_ui_command_type type)
{
    size_t count = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id == widget.index + 1u && commands[index].type == type)
        {
            return &commands[index];
        }
    }
    return NULL;
}

static size_t commands_of(const gui_fixture *fixture, tb_widget widget, tb_ui_command_type type)
{
    size_t count = 0;
    size_t found = 0;
    const tb_ui_command *const commands = tb_gui_commands(fixture->gui, &count);

    for (size_t index = 0; index < count; index++)
    {
        found += commands[index].id == widget.index + 1u && commands[index].type == type ? 1 : 0;
    }
    return found;
}

static bool same_color(tb_vec4 a, tb_vec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

TB_TEST(gui_draw, frames_labels_and_buttons_draw_their_states)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_ui_theme *const theme = tb_gui_theme(fixture.gui);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget plain = tb_frame(root, NULL);
    const tb_widget filled =
        tb_frame(root, &(tb_frame_desc){.padding = 4.0f, .background = true, .border = true});
    const tb_widget label = tb_label(filled, &(tb_label_desc){.text = "Label"});
    const tb_widget button = tb_button(root, &(tb_button_desc){.text = "Button"});
    const tb_widget fallback = tb_button(root, &(tb_button_desc){.is_default = true});

    TB_TEST_REQUIRE(tb_grid(plain, NULL));
    TB_TEST_REQUIRE(tb_grid(filled, &(tb_grid_options){.row = 1}));
    TB_TEST_REQUIRE(tb_grid(label, NULL));
    TB_TEST_REQUIRE(tb_grid(button, &(tb_grid_options){.row = 2}));
    TB_TEST_REQUIRE(tb_grid(fallback, &(tb_grid_options){.row = 3}));
    gui_update(&fixture);

    /* A frame draws only what it is asked to. */
    TB_TEST_EXPECT_POINTER(command_of(&fixture, plain, TB_UI_COMMAND_RECT), ==, NULL);
    const tb_ui_command *const background = command_of(&fixture, filled, TB_UI_COMMAND_RECT);
    TB_TEST_REQUIRE_POINTER(background, !=, NULL);
    TB_TEST_EXPECT(same_color(background->color, theme->colors[TB_UI_COLOR_WINDOW]));
    TB_TEST_EXPECT_UINT(commands_of(&fixture, filled, TB_UI_COMMAND_OUTLINE), ==, 1);
    TB_TEST_EXPECT_INT(background->layer, ==, TB_GUI_DEFAULT_LAYER);

    const tb_ui_command *text = command_of(&fixture, label, TB_UI_COMMAND_TEXT);
    TB_TEST_REQUIRE_POINTER(text, !=, NULL);
    TB_TEST_EXPECT(tb_text_equal(text->text, TB_TEXT("Label")));
    TB_TEST_EXPECT(same_color(text->color, theme->colors[TB_UI_COLOR_TEXT]));
    TB_TEST_EXPECT_POINTER(text->font, ==, fixture.font);
    TB_TEST_EXPECT_FLOAT(text->font_size, ==, GUI_FONT_SIZE);
    tb_widget_set_enabled(label, false);
    gui_update(&fixture);
    text = command_of(&fixture, label, TB_UI_COMMAND_TEXT);
    TB_TEST_REQUIRE_POINTER(text, !=, NULL);
    TB_TEST_EXPECT(same_color(text->color, theme->colors[TB_UI_COLOR_TEXT_DIM]));

    /* A button: its face lit under the mouse, darker held, ringed with the
     * accent when it has the focus. */
    const tb_vec2 middle = gui_middle(tb_widget_rect(button));
    const tb_ui_command *face = command_of(&fixture, button, TB_UI_COMMAND_RECT);
    TB_TEST_REQUIRE_POINTER(face, !=, NULL);
    TB_TEST_EXPECT(same_color(face->color, theme->colors[TB_UI_COLOR_BUTTON]));
    TB_TEST_EXPECT_FLOAT(face->radius, ==, theme->frame_rounding);
    gui_move(&fixture, middle.x, middle.y);
    face = command_of(&fixture, button, TB_UI_COMMAND_RECT);
    TB_TEST_EXPECT(same_color(face->color, theme->colors[TB_UI_COLOR_BUTTON_HOT]));
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    face = command_of(&fixture, button, TB_UI_COMMAND_RECT);
    TB_TEST_EXPECT(same_color(face->color, theme->colors[TB_UI_COLOR_BUTTON_ACTIVE]));
    gui_move(&fixture, 700.0f, 500.0f);
    face = command_of(&fixture, button, TB_UI_COMMAND_RECT);
    TB_TEST_EXPECT(same_color(face->color, theme->colors[TB_UI_COLOR_BUTTON_HOT]));
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT_UINT(commands_of(&fixture, button, TB_UI_COMMAND_OUTLINE), ==, 1);
    TB_TEST_REQUIRE(tb_widget_focus(button));
    gui_update(&fixture);
    /* Focused by a program after the mouse: no ring until the focus is shown. */
    TB_TEST_EXPECT_UINT(commands_of(&fixture, button, TB_UI_COMMAND_OUTLINE), ==, 1);
    tb_gui_set_focus_visible(fixture.gui, true);
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(commands_of(&fixture, button, TB_UI_COMMAND_OUTLINE), ==, 2);
    /* The default button is ringed too, and a button of no text has none. */
    TB_TEST_EXPECT_UINT(commands_of(&fixture, fallback, TB_UI_COMMAND_OUTLINE), ==, 2);
    TB_TEST_EXPECT_POINTER(command_of(&fixture, fallback, TB_UI_COMMAND_TEXT), ==, NULL);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(fallback).y, >, 0.0f);
    tb_widget_set_enabled(fallback, false);
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(commands_of(&fixture, fallback, TB_UI_COMMAND_OUTLINE), ==, 1);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_draw, text_is_set_by_its_anchor_and_justified)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget label = tb_label(root, &(tb_label_desc){.text = "Set"});
    const tb_widget wrapped = tb_label(
        root, &(tb_label_desc){.text = "Lines wrapped at a width, centered against one another",
                               .wrap_length = 120.0f,
                               .justify = TB_FONT_ALIGN_CENTER});
    tb_gui_value anchor = tb_gui_enum(TB_GUI_ANCHOR_W);

    TB_TEST_REQUIRE(tb_grid(label, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    TB_TEST_REQUIRE(tb_grid(wrapped, &(tb_grid_options){.row = 1}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.minsize = 300.0f}));
    TB_TEST_REQUIRE(tb_grid_row_configure(root, 0, &(tb_grid_track){.minsize = 100.0f}));
    TB_TEST_REQUIRE(tb_widget_set(label, "anchor", &anchor));
    gui_update(&fixture);
    const tb_canvas_rect rect = tb_widget_rect(label);
    const tb_ui_command *text = command_of(&fixture, label, TB_UI_COMMAND_TEXT);
    TB_TEST_REQUIRE_POINTER(text, !=, NULL);
    TB_TEST_EXPECT_FLOAT(text->rect.x, ==, rect.x);
    /* Its capitals in the middle: the line's middle is near the label's. */
    TB_TEST_EXPECT_FLOAT_NEAR(text->rect.y + text->rect.height * 0.5f, rect.y + rect.height * 0.5f,
                              text->rect.height * 0.25f);

    anchor = tb_gui_enum(TB_GUI_ANCHOR_SE);
    TB_TEST_REQUIRE(tb_widget_set(label, "anchor", &anchor));
    gui_update(&fixture);
    text = command_of(&fixture, label, TB_UI_COMMAND_TEXT);
    TB_TEST_EXPECT_FLOAT_NEAR(text->rect.x + text->rect.width, rect.x + rect.width, 1e-3f);
    TB_TEST_EXPECT_FLOAT_NEAR(text->rect.y + text->rect.height, rect.y + rect.height, 1e-3f);

    /* Wrapped: several lines, no wider than the wrap, centered within it,
     * the widest landing where the text is placed. */
    const tb_ui_command *const lines = command_of(&fixture, wrapped, TB_UI_COMMAND_TEXT);
    const tb_canvas_rect box = tb_widget_rect(wrapped);
    TB_TEST_REQUIRE_POINTER(lines, !=, NULL);
    TB_TEST_EXPECT_FLOAT(lines->wrap_width, ==, 120.0f);
    TB_TEST_EXPECT_UINT(lines->align, ==, TB_FONT_ALIGN_CENTER);
    TB_TEST_EXPECT_FLOAT(box.width, <=, 120.0f);
    TB_TEST_EXPECT_FLOAT(box.height, >, 2.0f * GUI_FONT_SIZE);
    TB_TEST_EXPECT_FLOAT_NEAR(lines->rect.x + (120.0f - lines->rect.width) * 0.5f,
                              box.x + (box.width - lines->rect.width) * 0.5f, 1e-3f);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_draw, a_widget_is_clipped_by_its_parent)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget root = tb_gui_root(fixture.gui);
    const tb_widget frame = tb_frame(root, NULL);
    const tb_widget big = gui_box(frame, 300.0f, 300.0f);
    const tb_widget beside = gui_box(root, 700.0f, 10.0f);
    const tb_widget gone = gui_box(frame, 10.0f, 10.0f);

    TB_TEST_REQUIRE(tb_grid(frame, &(tb_grid_options){.sticky = TB_STICKY_NSEW}));
    TB_TEST_REQUIRE(tb_grid(big, NULL));
    TB_TEST_REQUIRE(tb_grid(beside, &(tb_grid_options){.column = 1}));
    /* Past the frame's right edge entirely: nothing of it is drawn. */
    TB_TEST_REQUIRE(tb_grid(gone, &(tb_grid_options){.column = 1}));
    TB_TEST_REQUIRE(tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1.0f}));
    gui_update(&fixture);
    const tb_ui_command *const fill = command_of(&fixture, big, TB_UI_COMMAND_RECT);
    TB_TEST_REQUIRE_POINTER(fill, !=, NULL);
    TB_TEST_EXPECT_FLOAT(fill->clip.width, ==, tb_widget_rect(frame).width);
    TB_TEST_EXPECT_FLOAT(fill->rect.width, ==, 300.0f);
    TB_TEST_EXPECT_POINTER(command_of(&fixture, gone, TB_UI_COMMAND_RECT), ==, NULL);
    gui_fixture_close(&fixture);
}

/* A kind that draws a cross of lines over its rectangle. */
static void cross_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const tb_vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};

    (void)widget;
    (void)data;
    tb_gui_paint_line(paint, (tb_vec2){rect.x, rect.y},
                      (tb_vec2){rect.x + rect.width, rect.y + rect.height}, 1.0f, white);
    tb_gui_paint_line(paint, (tb_vec2){rect.x + rect.width, rect.y},
                      (tb_vec2){rect.x, rect.y + rect.height}, 1.0f, white);
}

static tb_vec2 cross_measure(tb_widget widget, const void *data)
{
    (void)widget;
    (void)data;
    return (tb_vec2){40.0f, 40.0f};
}

static const tb_widget_class cross_kind = {
    .name = "cross", .measure = cross_measure, .draw = cross_draw};

TB_TEST(gui_draw, a_games_kind_draws_through_the_same_commands)
{
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    const tb_widget cross = tb_widget_create(tb_gui_root(fixture.gui), &cross_kind);

    TB_TEST_REQUIRE(tb_grid(cross, NULL));
    gui_update(&fixture);
    TB_TEST_EXPECT_UINT(commands_of(&fixture, cross, TB_UI_COMMAND_LINE), ==, 2);
    TB_TEST_EXPECT_POINTER(tb_widget_data(cross), ==, NULL);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_draw, fonts_asked_for_by_spec_come_from_the_catalog)
{
    tb_font_catalog *const catalog = tb_font_catalog_create(NULL);
    gui_fixture fixture = {0};
    gui_fixture_open(&fixture, 1.0f);
    tb_ui_theme theme = *tb_gui_theme(fixture.gui);
    tb_gui *const gui = tb_gui_create(&(tb_gui_options){.theme = &theme, .catalog = catalog});
    const tb_font_spec heading =
        tb_font_spec_sized(tb_font_spec_of_role(catalog, TB_FONT_ROLE_HEADING), 30.0f);
    const tb_font_spec missing = {.role = TB_FONT_ROLE_COUNT, .weight = 400, .size = 12.0f};
    size_t index = 0;
    size_t count = 0;

    tb_gui_resize(gui, 800, 600, 1.0f);
    const tb_widget titled =
        tb_label(tb_gui_root(gui), &(tb_label_desc){.text = "Title", .font = heading});
    const tb_widget same =
        tb_label(tb_gui_root(gui), &(tb_label_desc){.text = "Again", .font = heading});
    const tb_widget fallback =
        tb_label(tb_gui_root(gui), &(tb_label_desc){.text = "Plain", .font = missing});
    TB_TEST_REQUIRE(tb_grid(titled, NULL));
    TB_TEST_REQUIRE(tb_grid(same, &(tb_grid_options){.row = 1}));
    TB_TEST_REQUIRE(tb_grid(fallback, &(tb_grid_options){.row = 2}));
    tb_gui_update(gui, 0.0);
    const tb_ui_command *const commands = tb_gui_commands(gui, &count);
    TB_TEST_REQUIRE_UINT(count, ==, 3);
    TB_TEST_EXPECT_FLOAT(commands[0].font_size, ==, 30.0f);
    TB_TEST_EXPECT_POINTER(commands[0].font, ==, commands[1].font);
    /* A spec the catalog cannot find is drawn in the theme's font, at its size. */
    TB_TEST_EXPECT_POINTER(commands[2].font, ==, fixture.font);
    TB_TEST_EXPECT_FLOAT(commands[2].font_size, ==, 12.0f);
    if (tb_font_catalog_find_role(catalog, TB_FONT_ROLE_HEADING, &index))
    {
        TB_TEST_EXPECT_POINTER(commands[0].font, !=, fixture.font);
    }
    else
    {
        TB_TEST_EXPECT_POINTER(commands[0].font, ==, fixture.font);
    }
    tb_gui_destroy(gui);
    tb_font_catalog_destroy(catalog);
    gui_fixture_close(&fixture);
}

TB_TEST(gui_draw, a_gui_with_no_font_measures_and_draws_no_text)
{
    tb_gui *const gui = tb_gui_create(NULL);
    size_t count = 0;

    tb_gui_resize(gui, 400, 300, 1.0f);
    const tb_widget label = tb_label(tb_gui_root(gui), &(tb_label_desc){.text = "Unseen"});
    const tb_widget empty = tb_label(tb_gui_root(gui), NULL);
    TB_TEST_REQUIRE(tb_grid(label, NULL));
    TB_TEST_REQUIRE(tb_grid(empty, &(tb_grid_options){.row = 1}));
    tb_gui_update(gui, 0.0);
    (void)tb_gui_commands(gui, &count);
    TB_TEST_EXPECT_UINT(count, ==, 0);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(label).x, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(empty).y, ==, 0.0f);
    tb_gui_destroy(gui);
    tb_gui_destroy(NULL);
}

TB_TEST(gui_draw, commands_become_quads_on_the_canvas)
{
    gui_fixture fixture = {0};
    tb_window_system *system = NULL;
    tb_window *window = NULL;
    tb_renderer *renderer = NULL;
    tb_rhi_error error = TB_RHI_ERROR_INIT;
    tb_renderer_options options = {0};
    tb_canvas_stats stats = {0};
    tb_camera camera = tb_camera_perspective(1.2f);
    size_t count = 0;

    if (tb_window_system_open(NULL, &system, NULL) != TB_WINDOW_OK)
    {
        TB_TEST_SKIP("no windowing here");
    }
    TB_TEST_REQUIRE_UINT(
        tb_window_create(
            system,
            &(tb_window_options){.title = "gui", .width = 400, .height = 300, .fixed_size = true},
            &window, NULL),
        ==, TB_WINDOW_OK);
    options.window = window;
    options.device = (tb_rhi_device_options){.backend = TB_RHI_BACKEND_NULL};
    TB_TEST_REQUIRE_UINT(tb_renderer_create(&options, &renderer, &error), ==, TB_RHI_OK);

    gui_fixture_open(&fixture, 1.0f);
    gui_resize(&fixture, 400, 300, 1.0f);
    const tb_widget frame =
        tb_frame(tb_gui_root(fixture.gui), &(tb_frame_desc){.padding = 8.0f, .background = true});
    const tb_widget button = tb_button(frame, &(tb_button_desc){.text = "Draw"});
    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    TB_TEST_REQUIRE(tb_widget_show_modal(frame));
    gui_update(&fixture);
    (void)tb_gui_commands(fixture.gui, &count);

    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_gui_draw(fixture.gui, renderer);
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);
    TB_TEST_EXPECT_UINT(stats.glyphs, >=, tb_str_length("Draw"));
    TB_TEST_EXPECT_UINT(stats.quads, >, count - 1);

    tb_gui_forget_fonts(fixture.gui, renderer);
    gui_fixture_close(&fixture);
    tb_renderer_destroy(renderer);
    tb_window_destroy(window);
    tb_window_system_close(system);
}
