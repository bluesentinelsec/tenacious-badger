/* The sample game theme: beveled panels, buttons, and fields as nine-slice
 * images drawn here in code, in bronze and slate, with headings in the
 * heading role's family and text in the body role's; and its retro look,
 * the same in pixel art drawn by whole pixels, in the pixel role's font. A
 * menu's look, beside the tool themes' flat one. */
#include "gui_style.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>

/* A panel image's side in texels, its bevel's width, and the texels its
 * slices are cut at: smooth, and pixel art. */
#define TB_GAME_SIDE 24u
#define TB_GAME_BEVEL 5u
#define TB_GAME_INSET 8u
#define TB_GAME_RETRO_SIDE 12u
#define TB_GAME_RETRO_BEVEL 2u
#define TB_GAME_RETRO_INSET 4u
/* Units a texel of a retro panel's border is drawn. */
#define TB_GAME_RETRO_SCALE 2.0f
/* The radius of a smooth panel's corners, in texels. */
#define TB_GAME_CORNER 4.0f
/* Channels of a pixel, and the largest value of one. */
#define TB_GAME_CHANNELS 4u
#define TB_GAME_CHANNEL_MAX 255.0f

/* The images the theme draws. */
typedef enum tb_game_part
{
    TB_GAME_PANEL,
    TB_GAME_BUTTON,
    TB_GAME_BUTTON_HOT,
    TB_GAME_BUTTON_PRESSED,
    TB_GAME_FIELD,
    TB_GAME_FILL,
    TB_GAME_PART_COUNT
} tb_game_part;

/* How a part is drawn: a rim around it, a bevel lit from the top left
 * (or sunk, lit from the bottom right), and a fill shaded from top to
 * bottom. */
typedef struct tb_game_look
{
    tb_vec4 rim;
    tb_vec4 light;
    tb_vec4 dark;
    tb_vec4 top;
    tb_vec4 bottom;
    bool sunk;
} tb_game_look;

static tb_vec4 tb_game_rgb(float red, float green, float blue)
{
    return (tb_vec4){red, green, blue, 1.0f};
}

static tb_game_look tb_game_look_of(tb_game_part part)
{
    const tb_vec4 rim = tb_game_rgb(0.10f, 0.07f, 0.04f);

    switch (part)
    {
    case TB_GAME_PANEL:
        return (tb_game_look){rim,
                              tb_game_rgb(0.62f, 0.46f, 0.24f),
                              tb_game_rgb(0.33f, 0.22f, 0.11f),
                              tb_game_rgb(0.20f, 0.21f, 0.24f),
                              tb_game_rgb(0.12f, 0.13f, 0.15f),
                              false};
    case TB_GAME_BUTTON:
        return (tb_game_look){rim,
                              tb_game_rgb(0.86f, 0.66f, 0.36f),
                              tb_game_rgb(0.40f, 0.26f, 0.12f),
                              tb_game_rgb(0.58f, 0.40f, 0.20f),
                              tb_game_rgb(0.42f, 0.28f, 0.13f),
                              false};
    case TB_GAME_BUTTON_HOT:
        return (tb_game_look){rim,
                              tb_game_rgb(0.98f, 0.80f, 0.48f),
                              tb_game_rgb(0.48f, 0.32f, 0.15f),
                              tb_game_rgb(0.70f, 0.50f, 0.26f),
                              tb_game_rgb(0.52f, 0.35f, 0.17f),
                              false};
    case TB_GAME_BUTTON_PRESSED:
        return (tb_game_look){rim,
                              tb_game_rgb(0.86f, 0.66f, 0.36f),
                              tb_game_rgb(0.30f, 0.19f, 0.09f),
                              tb_game_rgb(0.34f, 0.22f, 0.10f),
                              tb_game_rgb(0.46f, 0.31f, 0.15f),
                              true};
    case TB_GAME_FIELD:
        return (tb_game_look){rim,
                              tb_game_rgb(0.40f, 0.30f, 0.17f),
                              tb_game_rgb(0.05f, 0.05f, 0.06f),
                              tb_game_rgb(0.06f, 0.06f, 0.07f),
                              tb_game_rgb(0.10f, 0.10f, 0.11f),
                              true};
    case TB_GAME_FILL:
    case TB_GAME_PART_COUNT:
        break;
    }
    return (tb_game_look){rim,
                          tb_game_rgb(1.0f, 0.90f, 0.55f),
                          tb_game_rgb(0.62f, 0.42f, 0.10f),
                          tb_game_rgb(0.96f, 0.76f, 0.30f),
                          tb_game_rgb(0.80f, 0.56f, 0.16f),
                          false};
}

/* Whether a texel lies outside a smooth panel's rounded corner. */
static bool tb_game_outside_corner(uint32_t x, uint32_t y, uint32_t side)
{
    const float radius = TB_GAME_CORNER;
    const float px = (float)x + 0.5f;
    const float py = (float)y + 0.5f;
    const float cx = px < radius ? radius : ((float)side - px < radius ? (float)side - radius : px);
    const float cy = py < radius ? radius : ((float)side - py < radius ? (float)side - radius : py);
    const float dx = px - cx;
    const float dy = py - cy;

    return dx * dx + dy * dy > radius * radius;
}

static void tb_game_put(uint8_t *pixel, tb_vec4 color)
{
    const float channels[TB_GAME_CHANNELS] = {color.x, color.y, color.z, color.w};

    for (size_t index = 0; index < TB_GAME_CHANNELS; index++)
    {
        const float clamped = fminf(fmaxf(channels[index], 0.0f), 1.0f);
        pixel[index] = (uint8_t)lroundf(clamped * TB_GAME_CHANNEL_MAX);
    }
}

/* Draw a part into a square of RGBA texels, `side` a side. */
static void tb_game_paint(tb_game_part part, bool retro, uint8_t *pixels, uint32_t side)
{
    const tb_game_look look = tb_game_look_of(part);
    const uint32_t bevel = retro ? TB_GAME_RETRO_BEVEL : TB_GAME_BEVEL;

    for (uint32_t y = 0; y < side; y++)
    {
        for (uint32_t x = 0; x < side; x++)
        {
            uint8_t *const pixel = &pixels[((size_t)y * side + x) * TB_GAME_CHANNELS];
            const uint32_t edge_x = x < side - 1u - x ? x : side - 1u - x;
            const uint32_t edge_y = y < side - 1u - y ? y : side - 1u - y;
            const uint32_t edge = edge_x < edge_y ? edge_x : edge_y;
            const float down = (float)y / (float)(side - 1u);
            /* Lit from the top left: the bevel's top and left are light. */
            const bool lit = (x + y < side - 1u) != look.sunk;
            tb_vec4 color = {look.top.x + (look.bottom.x - look.top.x) * down,
                             look.top.y + (look.bottom.y - look.top.y) * down,
                             look.top.z + (look.bottom.z - look.top.z) * down, 1.0f};

            if (!retro && tb_game_outside_corner(x, y, side))
            {
                color = (tb_vec4){0};
            }
            else if (edge == 0)
            {
                color = look.rim;
            }
            else if (edge < bevel)
            {
                color = lit ? look.light : look.dark;
            }
            tb_game_put(pixel, color);
        }
    }
}

/* A part's nine-slice panel, its image made on the renderer. */
static tb_style_image tb_game_image(tb_theme *theme, tb_renderer *renderer, tb_game_part part,
                                    bool retro)
{
    const uint32_t side = retro ? TB_GAME_RETRO_SIDE : TB_GAME_SIDE;
    const uint32_t inset = retro ? TB_GAME_RETRO_INSET : TB_GAME_INSET;
    const size_t size = (size_t)side * side * TB_GAME_CHANNELS;
    uint8_t *const pixels = tb_calloc(size, 1);
    const tb_image_desc desc = {.width = side,
                                .height = side,
                                .filter = retro ? TB_IMAGE_NEAREST : TB_IMAGE_LINEAR,
                                .name = "game theme"};

    tb_game_paint(part, retro, pixels, side);
    const tb_image *const image = tb_theme_make_image(theme, renderer, &desc, pixels, size);
    tb_free(pixels);
    return (tb_style_image){.slice = {.image = image,
                                      .left = inset,
                                      .top = inset,
                                      .right = inset,
                                      .bottom = inset,
                                      .scale = retro ? TB_GAME_RETRO_SCALE : 1.0f,
                                      .whole_pixels = retro},
                            .tint = {1.0f, 1.0f, 1.0f, 1.0f}};
}

static void tb_game_set_image(tb_theme *theme, const char *style_class, const char *name,
                              uint32_t states, const tb_style_image *image)
{
    const bool set = tb_theme_set_image(theme, style_class, name, states, image);
    TB_ASSERT(set);
    (void)set;
}

static void tb_game_color(tb_theme *theme, const char *style_class, const char *name,
                          uint32_t states, tb_vec4 color)
{
    const bool set = tb_theme_set_color(theme, style_class, name, states, color);
    TB_ASSERT(set);
    (void)set;
}

static void tb_game_number(tb_theme *theme, const char *style_class, const char *name,
                           uint32_t states, float number)
{
    const bool set = tb_theme_set_number(theme, style_class, name, states, number);
    TB_ASSERT(set);
    (void)set;
}

static void tb_game_padding(tb_theme *theme, const char *style_class, float across, float down)
{
    const bool set = tb_theme_set_edges(theme, style_class, "padding", 0,
                                        (tb_gui_edges){across, down, across, down});
    TB_ASSERT(set);
    (void)set;
}

static void tb_game_font(tb_theme *theme, const char *style_class, tb_font_role role)
{
    /* Never bold: the role's family at its regular weight, at its size. */
    const bool set =
        tb_theme_set_font(theme, style_class, "font", 0,
                          (tb_font_spec){.role = role, .weight = TB_FONT_WEIGHT_REGULAR});
    TB_ASSERT(set);
    (void)set;
}

/* The values every kind shares: colors of parchment, bronze, and gold. */
static void tb_game_fill_root(tb_theme *theme, bool retro)
{
    const char *const root = TB_STYLE_ROOT_CLASS;
    const tb_vec4 parchment = tb_game_rgb(0.93f, 0.87f, 0.72f);
    const tb_vec4 gold = tb_game_rgb(1.0f, 0.80f, 0.30f);

    tb_game_font(theme, root, retro ? TB_FONT_ROLE_PIXEL : TB_FONT_ROLE_BODY);
    tb_game_color(theme, root, "text_color", 0, parchment);
    tb_game_color(theme, root, "text_color", TB_WIDGET_DISABLED, tb_game_rgb(0.52f, 0.47f, 0.40f));
    tb_game_color(theme, root, "focus_color", 0, gold);
    tb_game_color(theme, root, "shade", 0, (tb_vec4){0.02f, 0.01f, 0.0f, 0.55f});
    tb_game_color(theme, root, "selection", 0, (tb_vec4){0.80f, 0.55f, 0.18f, 0.50f});
    tb_game_color(theme, root, "hot_color", 0, (tb_vec4){1.0f, 0.85f, 0.50f, 0.16f});
    tb_game_color(theme, root, "active_color", 0, gold);
    tb_game_color(theme, root, "placeholder_color", 0, tb_game_rgb(0.55f, 0.50f, 0.42f));
    tb_game_color(theme, root, "caret_color", 0, gold);
    tb_game_number(theme, root, "mark_width", 0, 2.0f);
    tb_game_number(theme, root, "scrollbar_width", 0, 12.0f);
    tb_game_number(theme, root, "scrollbar_inset", 0, 2.0f);
    tb_game_color(theme, root, "scrollbar_trough", 0, (tb_vec4){0.0f, 0.0f, 0.0f, 0.35f});
    tb_game_color(theme, root, "scrollbar_thumb", 0, tb_game_rgb(0.58f, 0.40f, 0.20f));
    tb_game_color(theme, root, "scrollbar_thumb_hot", 0, tb_game_rgb(0.78f, 0.58f, 0.30f));
    tb_game_color(theme, root, "heading_text_color", 0, gold);
    tb_game_color(theme, root, "tab_text_color", 0, tb_game_rgb(0.62f, 0.56f, 0.46f));
    tb_game_color(theme, root, "tab_selected_text_color", 0, gold);
    tb_game_color(theme, root, "separator_color", 0, tb_game_rgb(0.40f, 0.28f, 0.14f));
}

/* Panels: frames, lists, menus, and tooltips. */
static void tb_game_fill_panels(tb_theme *theme, const tb_style_image *panel,
                                const tb_style_image *field, bool retro)
{
    const char *const panels[] = {"Frame", "Menu", "Tooltip", "ComboboxList", "Notebook"};
    const float edge =
        retro ? TB_GAME_RETRO_SCALE * (float)TB_GAME_RETRO_INSET : (float)TB_GAME_INSET;

    for (size_t index = 0; index < sizeof(panels) / sizeof(panels[0]); index++)
    {
        tb_game_set_image(theme, panels[index], "background_image", 0, panel);
        tb_game_number(theme, panels[index], "border_width", 0, 0.0f);
    }
    tb_game_padding(theme, "Frame", edge, edge);
    tb_game_padding(theme, "Tooltip", edge, edge * 0.75f);
    tb_game_padding(theme, "Menu", edge, edge * 0.5f);
    tb_game_padding(theme, "ComboboxList", edge, edge * 0.25f);
    /* Lists and trees sink into their panels. */
    const char *const lists[] = {"Listbox", "Treeview"};
    for (size_t index = 0; index < sizeof(lists) / sizeof(lists[0]); index++)
    {
        tb_game_set_image(theme, lists[index], "background_image", 0, field);
        tb_game_number(theme, lists[index], "border_width", 0, 0.0f);
        tb_game_padding(theme, lists[index], edge, 3.0f);
    }
    tb_game_color(theme, "Treeview", "heading_background", 0, (tb_vec4){0.0f, 0.0f, 0.0f, 0.35f});
    tb_game_color(theme, "Menubar", "background", 0, tb_game_rgb(0.12f, 0.11f, 0.12f));
    tb_game_padding(theme, "Menubar", edge, 4.0f);
    tb_game_color(theme, "Notebook", "tab_background", 0, tb_game_rgb(0.16f, 0.15f, 0.16f));
    tb_game_color(theme, "Notebook", "tab_hot_background", 0, tb_game_rgb(0.24f, 0.21f, 0.18f));
    tb_game_color(theme, "Notebook", "tab_selected_background", 0,
                  tb_game_rgb(0.20f, 0.21f, 0.24f));
    tb_game_color(theme, "Notebook", "tab_line_color", 0, tb_game_rgb(1.0f, 0.80f, 0.30f));
    tb_game_color(theme, "Notebook", "tab_line_color", TB_WIDGET_FOCUSED,
                  tb_game_rgb(1.0f, 0.92f, 0.60f));
    tb_game_number(theme, "Notebook", "tab_line_width", 0, 2.0f);
    tb_game_color(theme, "Panedwindow", "sash_color", 0, tb_game_rgb(0.40f, 0.28f, 0.14f));
    tb_game_color(theme, "Panedwindow", "sash_active_color", 0, tb_game_rgb(1.0f, 0.80f, 0.30f));
}

/* Buttons, and what is pressed as one is: combo boxes. */
static void tb_game_fill_buttons(tb_theme *theme, const tb_style_image images[TB_GAME_PART_COUNT],
                                 bool retro)
{
    const char *const kinds[] = {"Button", "Combobox"};
    const uint32_t held = TB_WIDGET_HOVERED | TB_WIDGET_PRESSED;
    tb_style_image dimmed = images[TB_GAME_BUTTON];

    dimmed.tint = (tb_vec4){0.55f, 0.55f, 0.55f, 1.0f};
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
    {
        tb_game_set_image(theme, kinds[index], "background_image", 0, &images[TB_GAME_BUTTON]);
        tb_game_set_image(theme, kinds[index], "background_image", TB_WIDGET_HOVERED,
                          &images[TB_GAME_BUTTON_HOT]);
        tb_game_set_image(theme, kinds[index], "background_image", held,
                          &images[TB_GAME_BUTTON_PRESSED]);
        tb_game_set_image(theme, kinds[index], "background_image", TB_WIDGET_DISABLED, &dimmed);
        tb_game_number(theme, kinds[index], "border_width", 0, 0.0f);
        tb_game_number(theme, kinds[index], "radius", 0, retro ? 0.0f : TB_GAME_CORNER);
        tb_game_padding(theme, kinds[index], retro ? 12.0f : 16.0f, retro ? 6.0f : 7.0f);
        tb_game_number(theme, kinds[index], "focus_width", TB_WIDGET_FOCUS_VISIBLE, 2.0f);
        tb_game_font(theme, kinds[index], retro ? TB_FONT_ROLE_PIXEL : TB_FONT_ROLE_HEADING);
    }
    tb_game_set_image(theme, "Combobox", "background_image", TB_WIDGET_PRESSED,
                      &images[TB_GAME_BUTTON_PRESSED]);
    tb_game_color(theme, "Button", "text_color", 0, tb_game_rgb(1.0f, 0.95f, 0.82f));
    tb_game_color(theme, "Button", "default_color", 0, tb_game_rgb(1.0f, 0.80f, 0.30f));
    tb_game_number(theme, "Button", "default_width", 0, 1.0f);
    const char *const titles[] = {"Title.Label", "Heading.Label"};
    for (size_t index = 0; index < sizeof(titles) / sizeof(titles[0]); index++)
    {
        tb_game_font(theme, titles[index], retro ? TB_FONT_ROLE_PIXEL : TB_FONT_ROLE_HEADING);
        tb_game_color(theme, titles[index], "text_color", 0, tb_game_rgb(1.0f, 0.80f, 0.30f));
    }
}

/* Fields typed in, choices, and ranges. */
static void tb_game_fill_fields(tb_theme *theme, const tb_style_image images[TB_GAME_PART_COUNT],
                                bool retro)
{
    const char *const fields[] = {"Entry", "Text", "Spinbox"};
    const tb_vec4 bronze = tb_game_rgb(0.58f, 0.40f, 0.20f);
    const tb_vec4 dark = tb_game_rgb(0.06f, 0.06f, 0.07f);
    const tb_vec4 gold = tb_game_rgb(1.0f, 0.80f, 0.30f);

    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); index++)
    {
        tb_game_set_image(theme, fields[index], "background_image", 0, &images[TB_GAME_FIELD]);
        tb_game_number(theme, fields[index], "border_width", 0, 0.0f);
        tb_game_number(theme, fields[index], "radius", 0, retro ? 0.0f : TB_GAME_CORNER);
        tb_game_padding(theme, fields[index], retro ? 8.0f : 10.0f, retro ? 5.0f : 6.0f);
        tb_game_number(theme, fields[index], "focus_width", TB_WIDGET_FOCUSED, 2.0f);
    }
    tb_game_color(theme, "Spinbox", "arrow_pressed_color", 0, (tb_vec4){1.0f, 0.8f, 0.3f, 0.25f});
    tb_game_color(theme, "Spinbox", "separator_color", 0, bronze);
    tb_game_number(theme, "Spinbox", "border_width", 0, 0.0f);

    const char *const choices[] = {"Checkbutton", "Radiobutton"};
    for (size_t index = 0; index < sizeof(choices) / sizeof(choices[0]); index++)
    {
        tb_game_color(theme, choices[index], "background", 0, dark);
        tb_game_color(theme, choices[index], "background", TB_WIDGET_HOVERED,
                      tb_game_rgb(0.14f, 0.12f, 0.10f));
        tb_game_color(theme, choices[index], "background", TB_WIDGET_HOVERED | TB_WIDGET_PRESSED,
                      tb_game_rgb(0.22f, 0.17f, 0.11f));
        tb_game_color(theme, choices[index], "border_color", 0, bronze);
        tb_game_number(theme, choices[index], "border_width", 0, 2.0f);
        tb_game_number(theme, choices[index], "radius", 0, retro ? 0.0f : 3.0f);
        tb_game_number(theme, choices[index], "focus_width", TB_WIDGET_FOCUS_VISIBLE, 2.0f);
        tb_game_number(theme, choices[index], "spacing", 0, 8.0f);
    }
    /* Each state the default theme gives a look of its own is given here too,
     * as it would otherwise outrank this theme's look at rest. */
    tb_game_color(theme, "Checkbutton", "background", TB_WIDGET_SELECTED, gold);
    tb_game_color(theme, "Checkbutton", "border_color", TB_WIDGET_SELECTED, bronze);
    tb_game_color(theme, "Checkbutton", "background", TB_WIDGET_SELECTED | TB_WIDGET_DISABLED,
                  tb_game_rgb(0.40f, 0.34f, 0.24f));
    tb_game_color(theme, "Checkbutton", "border_color", TB_WIDGET_SELECTED | TB_WIDGET_DISABLED,
                  bronze);
    tb_game_color(theme, "Checkbutton", "mark_color", 0, dark);
    tb_game_color(theme, "Radiobutton", "mark_color", 0, gold);

    tb_game_color(theme, "Scale", "background", 0, bronze);
    tb_game_color(theme, "Scale", "background", TB_WIDGET_HOVERED,
                  tb_game_rgb(0.78f, 0.58f, 0.30f));
    tb_game_color(theme, "Scale", "background", TB_WIDGET_PRESSED, gold);
    tb_game_color(theme, "Scale", "border_color", 0, tb_game_rgb(0.10f, 0.07f, 0.04f));
    tb_game_number(theme, "Scale", "border_width", 0, 2.0f);
    tb_game_number(theme, "Scale", "focus_width", TB_WIDGET_FOCUS_VISIBLE, 2.0f);
    tb_game_color(theme, "Scale", "trough_color", 0, dark);
    tb_game_color(theme, "Scale", "fill_color", 0, gold);
    tb_game_set_image(theme, "Progressbar", "background_image", 0, &images[TB_GAME_FIELD]);
    tb_game_number(theme, "Progressbar", "border_width", 0, 0.0f);
    tb_game_set_image(theme, "Progressbar", "fill_image", 0, &images[TB_GAME_FILL]);
}

tb_theme *tb_theme_create_game(tb_renderer *renderer, tb_theme_game_look look)
{
    tb_theme *const theme = tb_theme_create();
    const bool retro = look == TB_THEME_GAME_RETRO;
    tb_style_image images[TB_GAME_PART_COUNT] = {0};

    TB_ASSERT(renderer != NULL);
    for (size_t part = 0; part < TB_GAME_PART_COUNT; part++)
    {
        images[part] = tb_game_image(theme, renderer, (tb_game_part)part, retro);
    }
    tb_game_fill_root(theme, retro);
    tb_game_fill_panels(theme, &images[TB_GAME_PANEL], &images[TB_GAME_FIELD], retro);
    tb_game_fill_buttons(theme, images, retro);
    tb_game_fill_fields(theme, images, retro);
    return theme;
}
