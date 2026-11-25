/* The tool themes: every kind's style from a palette's colors, sizes, and
 * spacing (tb_ui_theme), flat, as the immediate-mode interface draws. Of
 * the dark palette it is the GUI's look by default, and of the light one
 * the light theme. */
#include "gui_style.h"

#include "badger/assert.h"

/* A dialog's title, in ems of the palette's size. */
#define TB_THEME_TITLE_EMS 1.25f
/* How much thicker than an edge marks and arrows are drawn. */
#define TB_THEME_MARK_EDGES 1.5f
/* How much thicker than an edge a check in a menu is drawn. */
#define TB_THEME_MENU_MARK_EDGES 2.0f
/* A tooltip's padding across, in parts of a field's. */
#define TB_THEME_TOOLTIP_ACROSS 0.75f

/* Each value is set by the engine's own names, which never fail. */
static void tb_theme_color(tb_theme *theme, const char *style_class, const char *name,
                           uint32_t states, tb_vec4 color)
{
    const bool set = tb_theme_set_color(theme, style_class, name, states, color);
    TB_ASSERT(set);
    (void)set;
}

static void tb_theme_number(tb_theme *theme, const char *style_class, const char *name,
                            uint32_t states, float number)
{
    const bool set = tb_theme_set_number(theme, style_class, name, states, number);
    TB_ASSERT(set);
    (void)set;
}

static void tb_theme_edges(tb_theme *theme, const char *style_class, const char *name,
                           tb_gui_edges edges)
{
    const bool set = tb_theme_set_edges(theme, style_class, name, 0, edges);
    TB_ASSERT(set);
    (void)set;
}

/* A box: its fill, and its border of the palette's. */
static void tb_theme_box(tb_theme *theme, const tb_ui_theme *palette, const char *style_class,
                         tb_vec4 background, float radius)
{
    tb_theme_color(theme, style_class, "background", 0, background);
    tb_theme_color(theme, style_class, "border_color", 0, palette->colors[TB_UI_COLOR_BORDER]);
    tb_theme_number(theme, style_class, "border_width", 0, palette->border);
    tb_theme_number(theme, style_class, "radius", 0, radius);
}

/* The ring drawn with the focus: the accent, twice an edge. */
static void tb_theme_focus_ring(tb_theme *theme, const tb_ui_theme *palette,
                                const char *style_class)
{
    tb_theme_number(theme, style_class, "focus_width", TB_WIDGET_FOCUSED, 2.0f * palette->border);
}

static tb_gui_edges tb_theme_padding(tb_vec2 padding)
{
    return (tb_gui_edges){padding.x, padding.y, padding.x, padding.y};
}

/* A row of a list: its text inset across, half the padding above and
 * below. */
static tb_gui_edges tb_theme_row_padding(const tb_ui_theme *palette)
{
    const tb_vec2 padding = palette->frame_padding;
    return (tb_gui_edges){padding.x, padding.y * 0.5f, padding.x, padding.y * 0.5f};
}

static void tb_theme_fill_root(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const char *const root = TB_STYLE_ROOT_CLASS;

    tb_theme_color(theme, root, "text_color", 0, colors[TB_UI_COLOR_TEXT]);
    tb_theme_color(theme, root, "text_color", TB_WIDGET_DISABLED, colors[TB_UI_COLOR_TEXT_DIM]);
    tb_theme_color(theme, root, "focus_color", 0, colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, root, "shade", 0, colors[TB_UI_COLOR_SHADOW]);
    tb_theme_color(theme, root, "selection", 0, colors[TB_UI_COLOR_SELECTION]);
    tb_theme_color(theme, root, "hot_color", 0, colors[TB_UI_COLOR_HEADER_HOT]);
    tb_theme_color(theme, root, "active_color", 0, colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, root, "placeholder_color", 0, colors[TB_UI_COLOR_TEXT_DIM]);
    tb_theme_color(theme, root, "caret_color", 0, colors[TB_UI_COLOR_TEXT]);
    tb_theme_number(theme, root, "mark_width", 0, palette->border * TB_THEME_MARK_EDGES);
    tb_theme_number(theme, root, "underline_width", 0, palette->border);
    tb_theme_number(theme, root, "scrollbar_width", 0, palette->scrollbar_width);
    tb_theme_number(theme, root, "scrollbar_inset", 0, 2.0f * palette->border);
    tb_theme_color(theme, root, "scrollbar_trough", 0, colors[TB_UI_COLOR_SCROLLBAR]);
    tb_theme_color(theme, root, "scrollbar_thumb", 0, colors[TB_UI_COLOR_SCROLLBAR_GRAB]);
    tb_theme_color(theme, root, "scrollbar_thumb_hot", 0, colors[TB_UI_COLOR_SCROLLBAR_GRAB_HOT]);
}

static void tb_theme_fill_buttons(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const uint32_t held = TB_WIDGET_HOVERED | TB_WIDGET_PRESSED;

    tb_theme_box(theme, palette, "Button", colors[TB_UI_COLOR_BUTTON], palette->frame_rounding);
    tb_theme_color(theme, "Button", "background", TB_WIDGET_HOVERED,
                   colors[TB_UI_COLOR_BUTTON_HOT]);
    tb_theme_color(theme, "Button", "background", TB_WIDGET_PRESSED,
                   colors[TB_UI_COLOR_BUTTON_HOT]);
    tb_theme_color(theme, "Button", "background", held, colors[TB_UI_COLOR_BUTTON_ACTIVE]);
    tb_theme_edges(theme, "Button", "padding", tb_theme_padding(palette->frame_padding));
    tb_theme_focus_ring(theme, palette, "Button");
    tb_theme_color(theme, "Button", "default_color", 0, colors[TB_UI_COLOR_ACCENT]);

    /* A combo box is a button that stays pressed while its list is open. */
    tb_theme_box(theme, palette, "Combobox", colors[TB_UI_COLOR_BUTTON], palette->frame_rounding);
    tb_theme_color(theme, "Combobox", "background", TB_WIDGET_HOVERED,
                   colors[TB_UI_COLOR_BUTTON_HOT]);
    tb_theme_color(theme, "Combobox", "background", TB_WIDGET_PRESSED,
                   colors[TB_UI_COLOR_BUTTON_ACTIVE]);
    tb_theme_edges(theme, "Combobox", "padding", tb_theme_padding(palette->frame_padding));
    tb_theme_focus_ring(theme, palette, "Combobox");
    tb_theme_box(theme, palette, "ComboboxList", colors[TB_UI_COLOR_POPUP],
                 palette->frame_rounding);
    tb_theme_edges(theme, "ComboboxList", "padding", tb_theme_row_padding(palette));
    tb_theme_color(theme, "ComboboxList", "thumb_color", 0, colors[TB_UI_COLOR_SCROLLBAR_GRAB]);
}

static void tb_theme_fill_choices(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const uint32_t held = TB_WIDGET_HOVERED | TB_WIDGET_PRESSED;
    const uint32_t chosen_off = TB_WIDGET_SELECTED | TB_WIDGET_DISABLED;
    const char *const kinds[] = {"Checkbutton", "Radiobutton"};

    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
    {
        tb_theme_box(theme, palette, kinds[index], colors[TB_UI_COLOR_FRAME],
                     palette->frame_rounding);
        tb_theme_color(theme, kinds[index], "background", TB_WIDGET_HOVERED,
                       colors[TB_UI_COLOR_FRAME_HOT]);
        tb_theme_color(theme, kinds[index], "background", held, colors[TB_UI_COLOR_FRAME_ACTIVE]);
        tb_theme_focus_ring(theme, palette, kinds[index]);
        tb_theme_number(theme, kinds[index], "spacing", 0, palette->frame_padding.x);
    }
    /* A check button ticked is filled with the accent, border and all. */
    tb_theme_color(theme, "Checkbutton", "background", TB_WIDGET_SELECTED,
                   colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, "Checkbutton", "border_color", TB_WIDGET_SELECTED,
                   colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, "Checkbutton", "background", chosen_off, colors[TB_UI_COLOR_FRAME]);
    tb_theme_color(theme, "Checkbutton", "border_color", chosen_off, colors[TB_UI_COLOR_FRAME]);
    tb_theme_color(theme, "Checkbutton", "mark_color", 0, colors[TB_UI_COLOR_TEXT]);
    tb_theme_color(theme, "Checkbutton", "mark_color", TB_WIDGET_DISABLED,
                   colors[TB_UI_COLOR_TEXT_DIM]);
    tb_theme_color(theme, "Radiobutton", "mark_color", 0, colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, "Radiobutton", "mark_color", TB_WIDGET_DISABLED,
                   colors[TB_UI_COLOR_TEXT_DIM]);
}

static void tb_theme_fill_fields(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const char *const kinds[] = {"Entry", "Text", "Spinbox"};

    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
    {
        tb_theme_box(theme, palette, kinds[index], colors[TB_UI_COLOR_FRAME],
                     palette->frame_rounding);
        tb_theme_color(theme, kinds[index], "background", TB_WIDGET_HOVERED,
                       colors[TB_UI_COLOR_FRAME_HOT]);
        tb_theme_color(theme, kinds[index], "background", TB_WIDGET_FOCUSED,
                       colors[TB_UI_COLOR_FRAME_HOT]);
        tb_theme_edges(theme, kinds[index], "padding", tb_theme_padding(palette->frame_padding));
        tb_theme_focus_ring(theme, palette, kinds[index]);
    }
    tb_theme_color(theme, "Spinbox", "arrow_pressed_color", 0, colors[TB_UI_COLOR_BUTTON_ACTIVE]);
    tb_theme_color(theme, "Spinbox", "separator_color", 0, colors[TB_UI_COLOR_BORDER]);
}

static void tb_theme_fill_ranges(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;

    /* A slider's box is its handle's. */
    tb_theme_box(theme, palette, "Scale", colors[TB_UI_COLOR_BUTTON], 0.0f);
    tb_theme_color(theme, "Scale", "background", TB_WIDGET_HOVERED,
                   colors[TB_UI_COLOR_BUTTON_HOT]);
    tb_theme_color(theme, "Scale", "background", TB_WIDGET_PRESSED,
                   colors[TB_UI_COLOR_BUTTON_ACTIVE]);
    tb_theme_focus_ring(theme, palette, "Scale");
    tb_theme_color(theme, "Scale", "trough_color", 0, colors[TB_UI_COLOR_FRAME]);
    tb_theme_color(theme, "Scale", "fill_color", 0, colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, "Scale", "fill_color", TB_WIDGET_DISABLED,
                   colors[TB_UI_COLOR_FRAME_ACTIVE]);
    tb_theme_number(theme, "Scale", "spacing", 0, palette->frame_padding.x);

    tb_theme_box(theme, palette, "Progressbar", colors[TB_UI_COLOR_FRAME],
                 palette->frame_rounding);
    tb_theme_color(theme, "Progressbar", "fill_color", 0, colors[TB_UI_COLOR_ACCENT]);
    tb_theme_color(theme, "Progressbar", "fill_color", TB_WIDGET_DISABLED,
                   colors[TB_UI_COLOR_FRAME_ACTIVE]);
}

static void tb_theme_fill_lists(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const char *const kinds[] = {"Listbox", "Treeview"};

    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
    {
        /* Outlined by the accent with the focus, as a field is ringed. */
        tb_theme_box(theme, palette, kinds[index], colors[TB_UI_COLOR_FRAME], 0.0f);
        tb_theme_color(theme, kinds[index], "border_color", TB_WIDGET_FOCUSED,
                       colors[TB_UI_COLOR_ACCENT]);
        tb_theme_edges(theme, kinds[index], "padding", tb_theme_row_padding(palette));
    }
    tb_theme_color(theme, "Treeview", "heading_background", 0, colors[TB_UI_COLOR_HEADER]);
    tb_theme_color(theme, "Treeview", "heading_text_color", 0, colors[TB_UI_COLOR_TEXT]);
    tb_theme_color(theme, "Treeview", "separator_color", 0, colors[TB_UI_COLOR_SEPARATOR]);
    tb_theme_number(theme, "Treeview", "indent", 0, palette->indent);
}

static void tb_theme_fill_containers(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const float border = palette->border;

    tb_theme_box(theme, palette, "Frame", colors[TB_UI_COLOR_WINDOW], 0.0f);
    tb_theme_color(theme, "Scrollframe", "border_color", 0, colors[TB_UI_COLOR_BORDER]);
    tb_theme_number(theme, "Scrollframe", "border_width", 0, border);
    tb_theme_color(theme, "Canvas", "background", 0, colors[TB_UI_COLOR_FRAME]);

    tb_theme_box(theme, palette, "Notebook", colors[TB_UI_COLOR_WINDOW], 0.0f);
    tb_theme_color(theme, "Notebook", "tab_background", 0, colors[TB_UI_COLOR_HEADER]);
    tb_theme_color(theme, "Notebook", "tab_hot_background", 0, colors[TB_UI_COLOR_HEADER_HOT]);
    tb_theme_color(theme, "Notebook", "tab_selected_background", 0, colors[TB_UI_COLOR_WINDOW]);
    tb_theme_color(theme, "Notebook", "tab_text_color", 0, colors[TB_UI_COLOR_TEXT_DIM]);
    tb_theme_color(theme, "Notebook", "tab_selected_text_color", 0, colors[TB_UI_COLOR_TEXT]);
    tb_theme_color(theme, "Notebook", "tab_line_color", 0, colors[TB_UI_COLOR_BORDER]);
    tb_theme_color(theme, "Notebook", "tab_line_color", TB_WIDGET_FOCUSED,
                   colors[TB_UI_COLOR_ACCENT]);
    tb_theme_number(theme, "Notebook", "tab_line_width", 0, 2.0f * border);
    tb_theme_edges(theme, "Notebook", "tab_padding", tb_theme_padding(palette->frame_padding));

    tb_theme_number(theme, "Panedwindow", "sash_width", 0, palette->item_spacing.x);
    tb_theme_color(theme, "Panedwindow", "sash_color", 0, colors[TB_UI_COLOR_SEPARATOR]);
    tb_theme_color(theme, "Panedwindow", "sash_active_color", 0, colors[TB_UI_COLOR_ACCENT]);

    tb_theme_number(theme, "Messagebox", "frame_padding", 0, 2.0f * palette->window_padding.x);
    tb_theme_number(theme, "Messagebox", "spacing", 0, 2.0f * palette->item_spacing.y);
    tb_theme_number(theme, "Messagebox", "button_spacing", 0, palette->item_spacing.x);
    const bool titled = tb_theme_set_font(
        theme, "Title.Label", "font", 0,
        (tb_font_spec){.role = TB_FONT_ROLE_HEADING,
                       .weight = TB_FONT_WEIGHT_REGULAR,
                       .size = palette->font_size * TB_THEME_TITLE_EMS});
    TB_ASSERT(titled);
    (void)titled;
}

static void tb_theme_fill_overlays(tb_theme *theme, const tb_ui_theme *palette)
{
    const tb_vec4 *const colors = palette->colors;
    const tb_vec2 padding = palette->frame_padding;

    tb_theme_box(theme, palette, "Menu", colors[TB_UI_COLOR_POPUP], 0.0f);
    tb_theme_edges(theme, "Menu", "padding", tb_theme_padding(padding));
    tb_theme_color(theme, "Menu", "accelerator_color", 0, colors[TB_UI_COLOR_TEXT_DIM]);
    tb_theme_color(theme, "Menu", "separator_color", 0, colors[TB_UI_COLOR_BORDER]);
    tb_theme_number(theme, "Menu", "mark_width", 0, palette->border * TB_THEME_MENU_MARK_EDGES);
    tb_theme_color(theme, "Menubar", "background", 0, colors[TB_UI_COLOR_HEADER]);
    tb_theme_edges(theme, "Menubar", "padding", tb_theme_padding(padding));

    tb_theme_box(theme, palette, "Tooltip", colors[TB_UI_COLOR_POPUP], palette->frame_rounding);
    tb_theme_edges(theme, "Tooltip", "padding",
                   (tb_gui_edges){padding.x * TB_THEME_TOOLTIP_ACROSS, padding.y,
                                  padding.x * TB_THEME_TOOLTIP_ACROSS, padding.y});
}

void tb_theme_fill_tool(tb_theme *theme, const tb_ui_theme *palette)
{
    tb_theme_fill_root(theme, palette);
    tb_theme_fill_buttons(theme, palette);
    tb_theme_fill_choices(theme, palette);
    tb_theme_fill_fields(theme, palette);
    tb_theme_fill_ranges(theme, palette);
    tb_theme_fill_lists(theme, palette);
    tb_theme_fill_containers(theme, palette);
    tb_theme_fill_overlays(theme, palette);
}

tb_theme *tb_theme_create_tool(const tb_ui_theme *palette)
{
    tb_theme *const theme = tb_theme_create();

    TB_ASSERT(palette != NULL);
    tb_theme_fill_tool(theme, palette);
    return theme;
}

tb_theme *tb_theme_create_light(void)
{
    const tb_ui_theme palette = tb_ui_theme_light();
    tb_theme *const theme = tb_theme_create_tool(&palette);

    /* A tick reads white on the accent; dimmed, it is drawn as dark text is. */
    tb_theme_color(theme, "Checkbutton", "mark_color", 0, (tb_vec4){1.0f, 1.0f, 1.0f, 1.0f});
    tb_theme_color(theme, "Checkbutton", "mark_color", TB_WIDGET_DISABLED,
                   palette.colors[TB_UI_COLOR_TEXT_DIM]);
    return theme;
}
