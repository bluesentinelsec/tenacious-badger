/* Styles: values set in themes by class and state, a widget's style
 * resolved from its class's chain, its overrides, and the themes of its
 * ancestors; the states it is drawn in; styles kept until something they
 * come from changes; and the built-in themes. */
#include "badger/canvas.h"
#include "badger/str.h"
#include "badger/window.h"

#include "gui_fixture.h"
#include "tb_test.h"

#include <string.h>

static const tb_vec4 red = {1.0f, 0.0f, 0.0f, 1.0f};
static const tb_vec4 green = {0.0f, 1.0f, 0.0f, 1.0f};
static const tb_vec4 blue = {0.0f, 0.0f, 1.0f, 1.0f};
static const tb_vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};

static bool same_color(tb_vec4 a, tb_vec4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/* A button in the root, laid out. */
static tb_widget make_button(gui_fixture *fixture, const char *text)
{
    const tb_widget button = tb_button(tb_gui_root(fixture->gui), &(tb_button_desc){.text = text});
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    gui_update(fixture);
    return button;
}

/* ---------------------------------------------------------------------------
 * Themes.
 * ------------------------------------------------------------------------- */

/* A value is set for a class and a set of states, read back exactly, and
 * replaced by one set for the same; each change moves the generation. */
TB_TEST(gui_style, themes_hold_values_by_class_name_and_states)
{
    tb_theme *const theme = tb_theme_create();
    tb_style_value value = {0};
    const uint64_t first = tb_theme_generation(theme);

    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", 0, red));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", TB_WIDGET_HOVERED, green));
    TB_TEST_EXPECT_UINT(tb_theme_generation(theme), >, first);
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "background", 0, &value));
    TB_TEST_EXPECT_UINT(value.type, ==, TB_STYLE_COLOR);
    TB_TEST_EXPECT(same_color(value.color, red));
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "background", TB_WIDGET_HOVERED, &value));
    TB_TEST_EXPECT(same_color(value.color, green));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", 0, blue));
    TB_TEST_REQUIRE(tb_theme_get(theme, "Button", "background", 0, &value));
    TB_TEST_EXPECT(same_color(value.color, blue));
    /* Exactly: a set of states not set, or a class not there, has none. */
    TB_TEST_EXPECT(!tb_theme_get(theme, "Button", "background", TB_WIDGET_PRESSED, &value));
    TB_TEST_EXPECT_UINT(value.type, ==, TB_STYLE_NONE);
    TB_TEST_EXPECT(!tb_theme_get(theme, "Label", "background", 0, &value));
    tb_theme_clear(theme);
    TB_TEST_EXPECT(!tb_theme_get(theme, "Button", "background", 0, &value));
    tb_theme_destroy(theme);
    tb_theme_destroy(NULL);
}

/* A standard value takes only its type, states only the five flags, and a
 * value must have a type; a font given a face and no size takes its
 * role's, and an image no tint draws untinted. */
TB_TEST(gui_style, themes_refuse_values_of_the_wrong_type)
{
    tb_theme *const theme = tb_theme_create();
    tb_style_value value = {0};

    TB_TEST_EXPECT(!tb_theme_set_number(theme, "Button", "background", 0, 3.0f));
    TB_TEST_EXPECT(!tb_theme_set_color(theme, "Button", "padding", 0, red));
    TB_TEST_EXPECT(!tb_theme_set_color(theme, "Button", "background", 1u << 7, red));
    TB_TEST_EXPECT(!tb_theme_set(theme, "Button", "accent", 0, &(tb_style_value){0}));
    TB_TEST_EXPECT(!tb_theme_get(theme, "Button", "background", 0, &value));
    /* An element of a kind's own takes any type. */
    TB_TEST_EXPECT(tb_theme_set_number(theme, "Button", "glow", 0, 3.0f));

    TB_TEST_REQUIRE(tb_theme_set_font(
        theme, ".", "font", 0,
        (tb_font_spec){.role = TB_FONT_ROLE_HEADING, .weight = TB_FONT_WEIGHT_REGULAR}));
    TB_TEST_REQUIRE(tb_theme_get(theme, ".", "font", 0, &value));
    TB_TEST_EXPECT_FLOAT(value.font.size, ==, tb_font_default_role(TB_FONT_ROLE_HEADING).size);

    TB_TEST_REQUIRE(tb_theme_set_image(theme, "Frame", "background_image", 0,
                                       &(tb_style_image){.slice = {.left = 2}}));
    TB_TEST_REQUIRE(tb_theme_get(theme, "Frame", "background_image", 0, &value));
    TB_TEST_EXPECT(same_color(value.image.tint, white));
    tb_theme_destroy(theme);
}

/* Images are named by path for theme files. */
TB_TEST(gui_style, themes_name_images_by_path)
{
    tb_theme *const theme = tb_theme_create();
    /* The theme only keeps the pointer; the test never draws it. */
    static const int stand_in = 0;
    const tb_image *const image = (const tb_image *)(const void *)&stand_in;

    TB_TEST_EXPECT_POINTER(tb_theme_image(theme, "/Game/UI/a.png"), ==, NULL);
    tb_theme_add_image(theme, "/Game/UI/a.png", image);
    TB_TEST_EXPECT_POINTER(tb_theme_image(theme, "/Game/UI/a.png"), ==, image);
    tb_theme_add_image(theme, "/Game/UI/a.png", image);
    TB_TEST_EXPECT_POINTER(tb_theme_image(theme, "/Game/UI/b.png"), ==, NULL);
    tb_theme_destroy(theme);
}

/* ---------------------------------------------------------------------------
 * Resolution.
 * ------------------------------------------------------------------------- */

/* A class falls back along its name: "Primary.Button", then "Button",
 * then "."; a class that does not end in its kind's still falls back to
 * it. */
TB_TEST(gui_style, classes_fall_back_along_their_names)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();

    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Big.Primary.Button", "text_color", 0, red));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Primary.Button", "background", 0, green));
    TB_TEST_REQUIRE(tb_theme_set_number(theme, "Button", "radius", 0, 9.0f));
    TB_TEST_REQUIRE(tb_theme_set_number(theme, ".", "focus_width", 0, 4.0f));
    tb_gui_set_style_theme(fixture.gui, theme);
    TB_TEST_EXPECT_POINTER(tb_gui_style_theme(fixture.gui), ==, theme);
    const tb_widget button = make_button(&fixture, "OK");
    const tb_widget primary = make_button(&fixture, "Go");
    const tb_widget odd = make_button(&fixture, "Odd");
    tb_widget_set_style(button, "Big.Primary.Button");
    tb_widget_set_style(primary, "Primary.Button");
    tb_widget_set_style(odd, "Primary");
    gui_update(&fixture);

    const tb_style *const big = tb_style_in(button, 0);
    TB_TEST_EXPECT(same_color(big->text_color, red));
    TB_TEST_EXPECT(same_color(big->background, green));
    TB_TEST_EXPECT_FLOAT(big->radius, ==, 9.0f);
    TB_TEST_EXPECT_FLOAT(big->focus_width, ==, 4.0f);
    const tb_style *const plain = tb_style_in(primary, 0);
    TB_TEST_EXPECT(same_color(plain->background, green));
    TB_TEST_EXPECT(!same_color(plain->text_color, red));
    /* "Primary" alone: its kind's class is added before the root. */
    TB_TEST_EXPECT_FLOAT(tb_style_in(odd, 0)->radius, ==, 9.0f);
    /* The default theme gives what the theme does not. */
    TB_TEST_EXPECT_FLOAT(plain->border_width, ==, tb_gui_theme(fixture.gui)->border);
    TB_TEST_EXPECT(tb_style_in(TB_WIDGET_NONE, 0) == NULL);
    TB_TEST_EXPECT(tb_style_of(TB_WIDGET_NONE) == NULL);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* A widget in several states takes the entry whose states it has every
 * one of and that ranks highest: disabled, then selected, pressed,
 * hovered, and focused, a state outranking any mix of those below. */
TB_TEST(gui_style, states_choose_the_highest_ranked_entry)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();
    const uint32_t held = TB_WIDGET_HOVERED | TB_WIDGET_PRESSED;
    const tb_vec4 gray = {0.5f, 0.5f, 0.5f, 1.0f};

    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", 0, red));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", TB_WIDGET_HOVERED, green));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", held, blue));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", TB_WIDGET_FOCUSED, white));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", TB_WIDGET_DISABLED, gray));
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget button = make_button(&fixture, "OK");

    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, red));
    TB_TEST_EXPECT(same_color(tb_style_in(button, TB_WIDGET_HOVERED)->background, green));
    /* Pressed alone has no entry in this theme: the default theme's, which
     * outranks this one's at rest. */
    TB_TEST_EXPECT(same_color(tb_style_in(button, TB_WIDGET_PRESSED)->background,
                              tb_gui_theme(fixture.gui)->colors[TB_UI_COLOR_BUTTON_HOT]));
    TB_TEST_EXPECT(same_color(tb_style_in(button, held)->background, blue));
    /* Hovered outranks focused. */
    TB_TEST_EXPECT(
        same_color(tb_style_in(button, TB_WIDGET_HOVERED | TB_WIDGET_FOCUSED)->background, green));
    TB_TEST_EXPECT(same_color(tb_style_in(button, TB_WIDGET_FOCUSED)->background, white));
    TB_TEST_EXPECT(same_color(tb_style_in(button, held | TB_WIDGET_DISABLED)->background, gray));
    /* A button draws no selected state: it is left out. */
    TB_TEST_EXPECT(tb_style_in(button, TB_WIDGET_SELECTED) == tb_style_in(button, 0));

    /* The widget's own state, as the mouse makes it. */
    const tb_canvas_rect rect = tb_widget_rect(button);
    gui_move(&fixture, rect.x + 2.0f, rect.y + 2.0f);
    TB_TEST_EXPECT(same_color(tb_style_of(button)->background, green));
    gui_press(&fixture, TB_MOUSE_BUTTON_LEFT);
    TB_TEST_EXPECT(same_color(tb_style_of(button)->background, blue));
    gui_release(&fixture, TB_MOUSE_BUTTON_LEFT);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* An entry set for more states, in a farther theme or class, outranks one
 * at rest in a nearer: a variation that changes the rest keeps its base's
 * look under the mouse, as Godot's do. */
TB_TEST(gui_style, specific_states_outrank_nearer_sources)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();

    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Primary.Button", "background", 0, red));
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget button = make_button(&fixture, "OK");
    tb_widget_set_style(button, "Primary.Button");
    gui_update(&fixture);

    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, red));
    /* Hovered comes from the default theme's Button. */
    TB_TEST_EXPECT(same_color(tb_style_in(button, TB_WIDGET_HOVERED)->background,
                              tb_gui_theme(fixture.gui)->colors[TB_UI_COLOR_BUTTON_HOT]));
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* A widget's overrides come before every theme's, by state as theirs do,
 * and are removed one by one, or all. */
TB_TEST(gui_style, overrides_come_before_every_theme)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();
    const tb_style_value padding = {.type = TB_STYLE_EDGES, .edges = {1, 2, 3, 4}};
    const tb_style_value hot = {.type = TB_STYLE_COLOR, .color = blue};

    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "text_color", 0, red));
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget button = make_button(&fixture, "OK");
    const tb_widget other = make_button(&fixture, "Other");
    const float before = tb_widget_natural_size(button).x;

    TB_TEST_REQUIRE(tb_widget_override_style(button, "text_color", 0,
                                             &(tb_style_value){TB_STYLE_COLOR, {.color = green}}));
    TB_TEST_REQUIRE(tb_widget_override_style(button, "padding", 0, &padding));
    TB_TEST_REQUIRE(tb_widget_override_style(button, "background", TB_WIDGET_HOVERED, &hot));
    TB_TEST_EXPECT(!tb_widget_override_style(button, "padding", 0, &hot));
    TB_TEST_EXPECT(!tb_widget_override_style(TB_WIDGET_NONE, "padding", 0, &padding));
    gui_update(&fixture);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->text_color, green));
    TB_TEST_EXPECT(same_color(tb_style_in(other, 0)->text_color, red));
    TB_TEST_EXPECT(same_color(tb_style_in(button, TB_WIDGET_HOVERED)->background, blue));
    /* Its padding is its insets: the layout follows. */
    TB_TEST_EXPECT_FLOAT(tb_style_in(button, 0)->padding.right, ==, 3.0f);
    TB_TEST_EXPECT(tb_widget_natural_size(button).x < before);

    TB_TEST_REQUIRE(tb_widget_override_style(button, "text_color", 0, NULL));
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->text_color, red));
    tb_widget_clear_style_overrides(button);
    tb_widget_clear_style_overrides(TB_WIDGET_NONE);
    gui_update(&fixture);
    TB_TEST_EXPECT_FLOAT(tb_widget_natural_size(button).x, ==, before);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* A subtree's theme comes before its ancestors' and the GUI's; what it
 * lacks comes from them. */
TB_TEST(gui_style, subtrees_have_themes_of_their_own)
{
    gui_fixture fixture = {0};
    tb_theme *const outer = tb_theme_create();
    tb_theme *const inner = tb_theme_create();

    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_theme_set_color(outer, "Label", "text_color", 0, red));
    TB_TEST_REQUIRE(tb_theme_set_number(outer, "Label", "radius", 0, 5.0f));
    TB_TEST_REQUIRE(tb_theme_set_color(inner, "Label", "text_color", 0, green));
    const tb_widget panel = tb_frame(tb_gui_root(fixture.gui), NULL);
    const tb_widget nested = tb_frame(panel, NULL);
    const tb_widget label = tb_label(nested, &(tb_label_desc){.text = "Hi"});
    const tb_widget outside = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "Out"});
    TB_TEST_REQUIRE(tb_widget_set_style_theme(panel, outer));
    TB_TEST_REQUIRE(tb_widget_set_style_theme(nested, inner));
    TB_TEST_EXPECT(!tb_widget_set_style_theme(TB_WIDGET_NONE, inner));
    TB_TEST_EXPECT_POINTER(tb_widget_style_theme(nested), ==, inner);
    TB_TEST_EXPECT_POINTER(tb_widget_style_theme(label), ==, NULL);
    TB_TEST_EXPECT_POINTER(tb_widget_style_theme(TB_WIDGET_NONE), ==, NULL);
    gui_update(&fixture);

    TB_TEST_EXPECT(same_color(tb_style_in(label, 0)->text_color, green));
    TB_TEST_EXPECT_FLOAT(tb_style_in(label, 0)->radius, ==, 5.0f);
    TB_TEST_EXPECT(same_color(tb_style_in(outside, 0)->text_color,
                              tb_gui_theme(fixture.gui)->colors[TB_UI_COLOR_TEXT]));
    TB_TEST_REQUIRE(tb_widget_set_style_theme(nested, NULL));
    TB_TEST_EXPECT(same_color(tb_style_in(label, 0)->text_color, red));
    /* A widget destroyed lets go of its theme. */
    TB_TEST_REQUIRE(tb_widget_set_style_theme(label, inner));
    tb_widget_destroy(panel);
    gui_update(&fixture);
    gui_fixture_close(&fixture);
    tb_theme_destroy(inner);
    tb_theme_destroy(outer);
}

/* ---------------------------------------------------------------------------
 * Keeping styles.
 * ------------------------------------------------------------------------- */

/* A style is resolved once and kept: asked again, it is the same; a theme
 * changed, its class changed, or the palette set resolves it again at the
 * next update, and lays the widgets out again. */
TB_TEST(gui_style, styles_are_kept_until_what_they_come_from_changes)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();
    tb_gui_stats stats = {0};

    gui_fixture_open(&fixture, 1.0f);
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget button = make_button(&fixture, "OK");
    const tb_style *const first = tb_style_in(button, 0);
    gui_update(&fixture);
    TB_TEST_EXPECT(tb_style_in(button, 0) == first);
    tb_gui_stats_of(fixture.gui, &stats);
    const size_t layouts = stats.layouts;

    /* A theme changed after it was set. */
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Button", "background", 0, red));
    gui_update(&fixture);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, red));
    tb_gui_stats_of(fixture.gui, &stats);
    TB_TEST_EXPECT_UINT(stats.layouts, >, layouts);

    /* Its class changed. */
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Quiet.Button", "background", 0, green));
    gui_update(&fixture);
    tb_widget_set_style(button, "Quiet.Button");
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, green));

    /* The palette: the default theme is made again from it. */
    tb_gui_set_style_theme(fixture.gui, NULL);
    tb_ui_theme palette = *tb_gui_theme(fixture.gui);
    palette.colors[TB_UI_COLOR_BUTTON] = blue;
    tb_gui_set_theme(fixture.gui, &palette);
    gui_update(&fixture);
    TB_TEST_EXPECT(same_color(tb_style_in(button, 0)->background, blue));
    TB_TEST_EXPECT(tb_gui_default_style_theme(fixture.gui) != NULL);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* ---------------------------------------------------------------------------
 * Elements, drawing, and text.
 * ------------------------------------------------------------------------- */

/* A kind's elements are read by index and by name, and only in their
 * declared type; a kind of a game's own declares its own. */
static const tb_style_element swatch_elements[] = {{"glow", TB_STYLE_COLOR},
                                                   {"thickness", TB_STYLE_NUMBER},
                                                   {"margin", TB_STYLE_EDGES},
                                                   {"caption", TB_STYLE_FONT},
                                                   {"panel", TB_STYLE_IMAGE}};
static const tb_widget_style_desc swatch_style = {
    .name = "Swatch", .states = TB_WIDGET_HOVERED, .elements = swatch_elements, .element_count = 5};
static const tb_widget_class swatch_kind = {.name = "swatch", .style = &swatch_style};

TB_TEST(gui_style, a_games_kind_reads_its_own_elements)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();

    gui_fixture_open(&fixture, 1.0f);
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Swatch", "glow", 0, red));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Swatch", "glow", TB_WIDGET_HOVERED, green));
    TB_TEST_REQUIRE(tb_theme_set_color(theme, "Swatch", "thickness", 0, red)); /* wrong type */
    TB_TEST_REQUIRE(tb_theme_set_edges(theme, ".", "margin", 0, (tb_gui_edges){1, 1, 1, 1}));
    TB_TEST_REQUIRE(tb_theme_set_font(theme, "Swatch", "caption", 0,
                                      (tb_font_spec){.role = TB_FONT_ROLE_BODY, .size = 9}));
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget swatch = tb_widget_create(tb_gui_root(fixture.gui), &swatch_kind);
    gui_update(&fixture);

    const tb_style *const style = tb_style_in(swatch, 0);
    TB_TEST_REQUIRE_UINT(style->element_count, ==, 5);
    TB_TEST_EXPECT(style->desc == &swatch_style);
    TB_TEST_EXPECT(same_color(tb_style_color(style, 0), red));
    TB_TEST_EXPECT(same_color(tb_style_color(tb_style_in(swatch, TB_WIDGET_HOVERED), 0), green));
    TB_TEST_EXPECT_FLOAT(tb_style_number(style, 1), ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(tb_style_edges(style, 2).left, ==, 1.0f);
    TB_TEST_EXPECT_FLOAT(tb_style_font(style, 3).size, ==, 9.0f);
    TB_TEST_EXPECT(tb_style_image_of(style, 4) == NULL);
    TB_TEST_EXPECT(same_color(tb_style_color(style, 99), (tb_vec4){0}));
    TB_TEST_EXPECT(tb_style_find(style, "glow") == &style->elements[0]);
    TB_TEST_EXPECT(tb_style_find(style, "nothing") == NULL);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* A box is its background, its border, and its focus ring, each drawn
 * only where it has one. */
TB_TEST(gui_style, a_box_draws_its_fill_border_and_focus_ring)
{
    gui_fixture fixture = {0};
    size_t count = 0;

    gui_fixture_open(&fixture, 1.0f);
    const tb_widget button = make_button(&fixture, "OK");
    /* As the keyboard gives it: the ring is shown. */
    tb_gui_set_focus_visible(fixture.gui, true);
    TB_TEST_REQUIRE(tb_widget_focus(button));
    gui_update(&fixture);
    const tb_ui_command *const commands = tb_gui_commands(fixture.gui, &count);
    const tb_ui_theme *const palette = tb_gui_theme(fixture.gui);
    size_t rects = 0;
    size_t rings = 0;
    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].id != button.index + 1u)
        {
            continue;
        }
        rects += commands[index].type == TB_UI_COMMAND_RECT ? 1u : 0u;
        rings += commands[index].type == TB_UI_COMMAND_OUTLINE &&
                         same_color(commands[index].color, palette->colors[TB_UI_COLOR_ACCENT]) &&
                         commands[index].thickness == 2.0f * palette->border
                     ? 1u
                     : 0u;
    }
    TB_TEST_EXPECT_UINT(rects, ==, 1);
    TB_TEST_EXPECT_UINT(rings, ==, 1);
    gui_fixture_close(&fixture);
}

/* Text is in its widget's style's font: a font spec of weight 0 is its
 * style's face, and a size of 0 its style's size. */
TB_TEST(gui_style, text_is_measured_in_its_styles_font)
{
    gui_fixture fixture = {0};
    tb_theme *const theme = tb_theme_create();
    const tb_gui_text_block block = {.text = tb_text_from_cstr("Measure")};

    gui_fixture_open(&fixture, 1.0f);
    tb_gui_set_style_theme(fixture.gui, theme);
    const tb_widget label = tb_label(tb_gui_root(fixture.gui), &(tb_label_desc){.text = "A"});
    TB_TEST_REQUIRE(tb_grid(label, NULL));
    gui_update(&fixture);
    const tb_vec2 plain = tb_widget_measure_text(label, &block);
    TB_TEST_REQUIRE(
        tb_theme_set_font(theme, "Label", "font", 0, (tb_font_spec){.size = 2.0f * GUI_FONT_SIZE}));
    gui_update(&fixture);
    const tb_vec2 large = tb_widget_measure_text(label, &block);
    TB_TEST_EXPECT_FLOAT_NEAR(large.x, 2.0f * plain.x, 1.0f);
    /* The GUI's own measure is in the root's style. */
    TB_TEST_EXPECT_FLOAT_NEAR(tb_gui_measure_text(fixture.gui, &block).x, plain.x, 1e-3f);
    TB_TEST_EXPECT_FLOAT(tb_widget_measure_text(TB_WIDGET_NONE, &block).x, ==, 0.0f);
    gui_fixture_close(&fixture);
    tb_theme_destroy(theme);
}

/* ---------------------------------------------------------------------------
 * The built-in themes.
 * ------------------------------------------------------------------------- */

/* The default is the tool theme of the palette: a button is the palette's
 * button color, framed by its border and rounding, padded by its frame
 * padding; the light theme draws by the light palette. */
TB_TEST(gui_style, the_tool_themes_follow_their_palettes)
{
    gui_fixture fixture = {0};
    tb_theme *const light = tb_theme_create_light();
    const tb_ui_theme light_palette = tb_ui_theme_light();

    gui_fixture_open(&fixture, 1.0f);
    const tb_ui_theme *const palette = tb_gui_theme(fixture.gui);
    const tb_widget button = make_button(&fixture, "OK");
    const tb_widget check = tb_checkbutton(tb_gui_root(fixture.gui), NULL);
    const tb_style *const style = tb_style_in(button, 0);
    TB_TEST_EXPECT(same_color(style->background, palette->colors[TB_UI_COLOR_BUTTON]));
    TB_TEST_EXPECT(same_color(style->border_color, palette->colors[TB_UI_COLOR_BORDER]));
    TB_TEST_EXPECT_FLOAT(style->radius, ==, palette->frame_rounding);
    TB_TEST_EXPECT_FLOAT(style->padding.left, ==, palette->frame_padding.x);
    TB_TEST_EXPECT_FLOAT(style->padding.top, ==, palette->frame_padding.y);
    TB_TEST_EXPECT_FLOAT(style->focus_width, ==, 0.0f);
    /* A button rings while its focus is shown, not after a click. */
    TB_TEST_EXPECT_FLOAT(tb_style_in(button, TB_WIDGET_FOCUSED)->focus_width, ==, 0.0f);
    TB_TEST_EXPECT_FLOAT(
        tb_style_in(button, TB_WIDGET_FOCUSED | TB_WIDGET_FOCUS_VISIBLE)->focus_width, ==,
                         2.0f * palette->border);
    TB_TEST_EXPECT(same_color(tb_style_in(check, TB_WIDGET_SELECTED)->background,
                              palette->colors[TB_UI_COLOR_ACCENT]));

    tb_gui_set_style_theme(fixture.gui, light);
    gui_update(&fixture);
    TB_TEST_EXPECT(
        same_color(tb_style_in(button, 0)->background, light_palette.colors[TB_UI_COLOR_BUTTON]));
    TB_TEST_EXPECT(
        same_color(tb_style_in(button, 0)->text_color, light_palette.colors[TB_UI_COLOR_TEXT]));
    TB_TEST_EXPECT(same_color(
        tb_style_find(tb_style_in(check, TB_WIDGET_SELECTED), "mark_color")->color, white));
    gui_fixture_close(&fixture);
    tb_theme_destroy(light);
}

/* The game themes draw on a renderer: their nine-slice panels become image
 * commands, a slice each, which the canvas draws; and the canvas draws a
 * panel itself. On the null backend, where there is a window. */
TB_TEST(gui_style, game_themes_draw_nine_slice_panels_on_the_canvas)
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
    size_t images = 0;

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
    tb_theme *const game = tb_theme_create_game(renderer, TB_THEME_GAME_FANTASY);
    tb_theme *const retro = tb_theme_create_game(renderer, TB_THEME_GAME_RETRO);

    gui_fixture_open(&fixture, 1.5f);
    tb_gui_set_style_theme(fixture.gui, game);
    const tb_widget frame =
        tb_frame(tb_gui_root(fixture.gui), &(tb_frame_desc){.background = true});
    const tb_widget button = tb_button(frame, &(tb_button_desc){.text = "Play"});
    const tb_widget bar = tb_progressbar(frame, &(tb_progressbar_desc){.value = 50});
    TB_TEST_REQUIRE(tb_grid(frame, NULL));
    TB_TEST_REQUIRE(tb_grid(button, NULL));
    TB_TEST_REQUIRE(tb_grid(bar, &(tb_grid_options){.row = 1}));
    gui_update(&fixture);
    /* A frame's padding is its panel's border while it draws it. */
    TB_TEST_EXPECT_FLOAT(tb_widget_rect(button).x - tb_widget_rect(frame).x, >=, 8.0f);
    const tb_ui_command *const commands = tb_gui_commands(fixture.gui, &count);
    for (size_t index = 0; index < count; index++)
    {
        if (commands[index].type == TB_UI_COMMAND_IMAGE)
        {
            images++;
            TB_TEST_EXPECT_UINT(commands[index].source.width, >, 0);
        }
    }
    /* The frame's panel, the button's, and the bar's trough and fill. */
    TB_TEST_EXPECT_UINT(images, >=, 27);

    tb_gui_set_style_theme(fixture.gui, retro);
    gui_update(&fixture);
    TB_TEST_REQUIRE(tb_camera_look_at(&camera, (tb_vec3){0, 0, 5}, (tb_vec3){0, 0, 0}));
    TB_TEST_REQUIRE(tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0, 0, 0, 1}));
    tb_gui_draw(fixture.gui, renderer);
    const tb_style *const panel = tb_style_in(frame, 0);
    TB_TEST_REQUIRE(panel->image.slice.image != NULL);
    tb_canvas_draw_nine_slice(renderer, &panel->image.slice, (tb_canvas_rect){10, 10, 100, 60},
                              (tb_vec4){1, 1, 1, 1});
    tb_canvas_draw_nine_slice(renderer, &(tb_canvas_nine_slice){0}, (tb_canvas_rect){0, 0, 9, 9},
                              (tb_vec4){1, 1, 1, 1});
    TB_TEST_REQUIRE_UINT(tb_renderer_end_frame(renderer), ==, TB_RHI_OK);
    tb_canvas_stats_of(renderer, &stats);
    TB_TEST_EXPECT_UINT(stats.dropped, ==, 0);
    TB_TEST_EXPECT_UINT(stats.quads, >=, 9);

    tb_gui_forget_fonts(fixture.gui, renderer);
    gui_fixture_close(&fixture);
    tb_theme_destroy(retro);
    tb_theme_destroy(game);
    tb_renderer_destroy(renderer);
    tb_window_destroy(window);
    tb_window_system_close(system);
}
