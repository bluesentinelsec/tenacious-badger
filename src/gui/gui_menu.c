/* Menus: the menu kind, a column of items shown as a popup (commands, check
 * and radio items, cascades to other menus, and separators, with their
 * accelerators' text and mnemonics), and the menubar kind, a row of titles
 * that each open a menu below it, after Tk's `menu`. Items are data, not
 * widgets. A menu is driven by the mouse, or by the arrows, Enter, and the
 * letters of its mnemonics; Escape is the popup layer's. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Ems between an item's label and its accelerator. */
#define TB_GUI_MENU_ACCELERATOR_GAP 2.0f
/* Items, and titles, a menu or menu bar first holds. */
#define TB_GUI_MENU_FIRST 8u
/* The part of a row's height the check, radio mark, and cascade arrow take. */
#define TB_GUI_MENU_MARK 0.3f
/* Units below the capitals' baseline a mnemonic is underlined at. */
#define TB_GUI_MENU_UNDERLINE 2.0f

/* ---------------------------------------------------------------------------
 * Labels and their mnemonics.
 * ------------------------------------------------------------------------- */

/* A label without its '&' marks, and the byte its mnemonic is at. */
typedef struct tb_gui_menu_label
{
    tb_gui_string text;
    size_t mnemonic_at; /* SIZE_MAX for none */
    char mnemonic;      /* lowercase ASCII; 0 for none */
} tb_gui_menu_label;

static char tb_gui_lower(char letter)
{
    return letter >= 'A' && letter <= 'Z' ? (char)(letter - 'A' + 'a') : letter;
}

/* Read a label: "&File" is "File", F its mnemonic; "&&" is an ampersand. */
static void tb_gui_menu_label_set(tb_gui_menu_label *label, const char *source)
{
    const tb_text text = tb_text_from_cstr(source != NULL ? source : "");
    char *const bytes = tb_malloc(text.length + 1);
    size_t length = 0;

    label->mnemonic_at = SIZE_MAX;
    label->mnemonic = 0;
    for (size_t at = 0; at < text.length; at++)
    {
        const bool marked = text.bytes[at] == '&' && at + 1 < text.length;
        if (marked && text.bytes[at + 1] != '&' && label->mnemonic_at == SIZE_MAX)
        {
            label->mnemonic_at = length;
            label->mnemonic = tb_gui_lower(text.bytes[at + 1]);
            continue;
        }
        if (marked && text.bytes[at + 1] == '&')
        {
            at++;
        }
        bytes[length] = text.bytes[at];
        length++;
    }
    bytes[length] = '\0';
    tb_gui_string_free(&label->text);
    label->text = (tb_gui_string){.bytes = bytes, .length = length};
}

/* The letter a key types, lowercase; 0 for a key of none. */
static char tb_gui_key_letter(tb_key key)
{
    if (key >= TB_KEY_A && key <= TB_KEY_Z)
    {
        return (char)('a' + (int)(key - TB_KEY_A));
    }
    if (key >= TB_KEY_0 && key <= TB_KEY_9)
    {
        return (char)('0' + (int)(key - TB_KEY_0));
    }
    return 0;
}

/* Draw a label at the left of a rectangle, its mnemonic underlined. */
static void tb_gui_menu_draw_label(tb_gui_paint *paint, const tb_gui_menu_label *label,
                                   tb_canvas_rect rect, tb_vec4 color)
{
    tb_gui *const gui = paint->gui;
    const tb_text text = tb_gui_string_text(&label->text);
    const tb_gui_text_block block = {.text = text, .anchor = TB_GUI_ANCHOR_W};
    const tb_gui_resolved_font resolved = tb_gui_resolve_font(gui, (tb_font_spec){0});
    tb_ui_font_measures measures = {0};

    tb_gui_paint_text(paint, &block, rect, color);
    if (label->mnemonic_at == SIZE_MAX || resolved.font == NULL)
    {
        return;
    }
    const tb_gui_text_block before = {.text = tb_text_make(text.bytes, label->mnemonic_at)};
    const tb_gui_text_block through = {.text = tb_text_make(text.bytes, label->mnemonic_at + 1)};
    const float start = rect.x + tb_gui_measure_text(gui, &before).x;
    const float end = rect.x + tb_gui_measure_text(gui, &through).x;
    tb_ui_font_measures_of(resolved.font, resolved.size, gui->view.scale, &measures);
    const float y =
        rect.y + rect.height * 0.5f + measures.cap_height * 0.5f + TB_GUI_MENU_UNDERLINE;
    tb_gui_paint_line(paint, (tb_vec2){start, y}, (tb_vec2){end, y}, tb_gui_theme(gui)->border,
                      color);
}

static float tb_gui_menu_row_height(tb_gui *gui)
{
    return tb_gui_line_height(gui) + tb_gui_theme(gui)->frame_padding.y * 2.0f;
}

static float tb_gui_label_width(tb_gui *gui, const tb_gui_string *text)
{
    const tb_gui_text_block block = {.text = tb_gui_string_text(text)};
    return tb_gui_measure_text(gui, &block).x;
}

/* ---------------------------------------------------------------------------
 * Menus.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_menu_item
{
    int32_t kind; /* a tb_menu_item_kind */
    tb_gui_menu_label label;
    tb_gui_string accelerator;
    tb_gui_command command;
    bool checked;
    int32_t group;
    tb_widget submenu;
    bool disabled;
} tb_gui_menu_item;

typedef struct tb_gui_menu
{
    tb_gui_menu_item *items;
    size_t count;
    size_t capacity;
    size_t active; /* SIZE_MAX for none */
} tb_gui_menu;

static tb_gui_menu *tb_gui_menu_of(tb_widget widget);

static void tb_gui_menu_init(tb_widget widget, void *data)
{
    tb_gui_menu *const menu = data;

    (void)widget;
    menu->active = SIZE_MAX;
}

static void tb_gui_menu_destroy(tb_widget widget, void *data)
{
    tb_gui_menu *const menu = data;

    (void)widget;
    for (size_t index = 0; index < menu->count; index++)
    {
        tb_gui_string_free(&menu->items[index].label.text);
        tb_gui_string_free(&menu->items[index].accelerator);
    }
    tb_free(menu->items);
}

static bool tb_gui_menu_selectable(const tb_gui_menu_item *item)
{
    return item->kind != TB_MENU_ITEM_SEPARATOR && !item->disabled;
}

static float tb_gui_menu_item_height(tb_gui *gui, const tb_gui_menu_item *item)
{
    if (item->kind == TB_MENU_ITEM_SEPARATOR)
    {
        return tb_gui_theme(gui)->frame_padding.y * 2.0f + tb_gui_theme(gui)->border;
    }
    return tb_gui_menu_row_height(gui);
}

/* An item's rectangle, in units of the GUI. */
static tb_canvas_rect tb_gui_menu_item_rect(tb_widget widget, const tb_gui_menu *menu, size_t index)
{
    const tb_canvas_rect rect = tb_widget_rect(widget);
    const float border = tb_gui_theme(widget.gui)->border;
    float top = rect.y + border;

    for (size_t at = 0; at < index; at++)
    {
        top += tb_gui_menu_item_height(widget.gui, &menu->items[at]);
    }
    return (tb_canvas_rect){rect.x + border, top, rect.width - 2.0f * border,
                            tb_gui_menu_item_height(widget.gui, &menu->items[index])};
}

static size_t tb_gui_menu_item_at(tb_widget widget, const tb_gui_menu *menu, tb_vec2 point)
{
    for (size_t index = 0; index < menu->count; index++)
    {
        if (tb_ui_rect_contains(tb_gui_menu_item_rect(widget, menu, index), point))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

static tb_vec2 tb_gui_menu_measure(tb_widget widget, const void *data)
{
    const tb_gui_menu *const menu = data;
    tb_gui *const gui = widget.gui;
    const tb_ui_theme *const theme = tb_gui_theme(gui);
    const float row = tb_gui_menu_row_height(gui);
    float labels = 0.0f;
    float accelerators = 0.0f;
    float height = 2.0f * theme->border;

    for (size_t index = 0; index < menu->count; index++)
    {
        const tb_gui_menu_item *const item = &menu->items[index];
        labels = fmaxf(labels, tb_gui_label_width(gui, &item->label.text));
        accelerators = fmaxf(accelerators, tb_gui_label_width(gui, &item->accelerator));
        height += tb_gui_menu_item_height(gui, item);
    }
    if (accelerators > 0.0f)
    {
        accelerators += theme->font_size * TB_GUI_MENU_ACCELERATOR_GAP;
    }
    /* A column for checks on the left, and for cascades' arrows on the right. */
    return (tb_vec2){2.0f * row + labels + accelerators + 2.0f * theme->frame_padding.x +
                         2.0f * theme->border,
                     height};
}

static void tb_gui_menu_draw_mark(tb_gui_paint *paint, const tb_gui_menu_item *item,
                                  tb_canvas_rect rect, tb_vec4 color)
{
    const float size = rect.height * TB_GUI_MENU_MARK;
    const tb_vec2 middle = {rect.x + rect.height * 0.5f, rect.y + rect.height * 0.5f};
    const float thickness = tb_gui_theme(paint->gui)->border * 2.0f;

    if (item->kind == TB_MENU_ITEM_CHECK && item->checked)
    {
        tb_gui_paint_line(paint, (tb_vec2){middle.x - size, middle.y},
                          (tb_vec2){middle.x - size * 0.3f, middle.y + size * 0.7f}, thickness,
                          color);
        tb_gui_paint_line(paint, (tb_vec2){middle.x - size * 0.3f, middle.y + size * 0.7f},
                          (tb_vec2){middle.x + size, middle.y - size * 0.7f}, thickness, color);
    }
    if (item->kind == TB_MENU_ITEM_RADIO && item->checked)
    {
        const float radius = size * 0.6f;
        tb_gui_paint_fill(
            paint,
            (tb_canvas_rect){middle.x - radius, middle.y - radius, 2.0f * radius, 2.0f * radius},
            radius, color);
    }
    if (item->kind == TB_MENU_ITEM_CASCADE)
    {
        const float x = rect.x + rect.width - rect.height * 0.5f;
        tb_gui_paint_line(paint, (tb_vec2){x - size * 0.4f, middle.y - size},
                          (tb_vec2){x + size * 0.4f, middle.y}, thickness * 0.5f, color);
        tb_gui_paint_line(paint, (tb_vec2){x + size * 0.4f, middle.y},
                          (tb_vec2){x - size * 0.4f, middle.y + size}, thickness * 0.5f, color);
    }
}

static void tb_gui_menu_draw_item(tb_widget widget, const tb_gui_menu *menu, tb_gui_paint *paint,
                                  size_t index)
{
    const tb_gui_menu_item *const item = &menu->items[index];
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect rect = tb_gui_menu_item_rect(widget, menu, index);
    const tb_vec4 color = theme->colors[item->disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT];
    const float row = tb_gui_menu_row_height(widget.gui);

    if (item->kind == TB_MENU_ITEM_SEPARATOR)
    {
        const float y = rect.y + rect.height * 0.5f;
        tb_gui_paint_line(paint, (tb_vec2){rect.x + theme->frame_padding.x, y},
                          (tb_vec2){rect.x + rect.width - theme->frame_padding.x, y}, theme->border,
                          theme->colors[TB_UI_COLOR_BORDER]);
        return;
    }
    if (index == menu->active && !item->disabled)
    {
        tb_gui_paint_fill(paint, rect, 0.0f, theme->colors[TB_UI_COLOR_SELECTION]);
    }
    tb_gui_menu_draw_mark(paint, item, rect, color);
    const tb_canvas_rect text = {rect.x + row, rect.y,
                                 fmaxf(rect.width - 2.0f * row - theme->frame_padding.x, 0.0f),
                                 rect.height};
    tb_gui_menu_draw_label(paint, &item->label, text, color);
    if (item->accelerator.length > 0)
    {
        const tb_gui_text_block block = {.text = tb_gui_string_text(&item->accelerator),
                                         .anchor = TB_GUI_ANCHOR_E};
        tb_gui_paint_text(paint, &block, text, theme->colors[TB_UI_COLOR_TEXT_DIM]);
    }
}

static void tb_gui_menu_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_menu *const menu = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);

    tb_gui_paint_fill(paint, rect, 0.0f, theme->colors[TB_UI_COLOR_POPUP]);
    for (size_t index = 0; index < menu->count; index++)
    {
        tb_gui_menu_draw_item(widget, menu, paint, index);
    }
    tb_gui_paint_outline(paint, rect, 0.0f, theme->border, theme->colors[TB_UI_COLOR_BORDER]);
}

/* The popup level a menu is shown at; SIZE_MAX when it is not shown. */
static size_t tb_gui_menu_level(tb_widget widget)
{
    return tb_widget_is_popup(widget) ? tb_gui_popup_level(widget.gui, widget.index, false)
                                      : SIZE_MAX;
}

/* The next item that can be chosen from one, forward or back, around the
 * ends; SIZE_MAX when there is none. */
static size_t tb_gui_menu_step(const tb_gui_menu *menu, size_t from, bool forward)
{
    size_t at = from;

    for (size_t tried = 0; tried < menu->count; tried++)
    {
        if (at == SIZE_MAX)
        {
            at = forward ? 0 : menu->count - 1;
        }
        else if (forward)
        {
            at = at + 1 < menu->count ? at + 1 : 0;
        }
        else
        {
            at = at > 0 ? at - 1 : menu->count - 1;
        }
        if (tb_gui_menu_selectable(&menu->items[at]))
        {
            return at;
        }
    }
    return SIZE_MAX;
}

/* Close the popups in front of a menu: its cascade. */
static void tb_gui_menu_close_cascade(tb_widget widget)
{
    const size_t level = tb_gui_menu_level(widget);

    if (level != SIZE_MAX)
    {
        tb_gui_close_popups_from(widget.gui, level + 1, false);
    }
}

/* Open the cascade of an item, beside it; its first item active when it is
 * opened by a key. */
static void tb_gui_menu_open_cascade(tb_widget widget, tb_gui_menu *menu, size_t index, bool by_key)
{
    const tb_widget submenu = menu->items[index].submenu;
    tb_gui_menu *const sub = tb_gui_menu_of(submenu);

    if (sub == NULL || tb_widget_is_popup(submenu))
    {
        return;
    }
    tb_gui_menu_close_cascade(widget);
    sub->active = by_key ? tb_gui_menu_step(sub, SIZE_MAX, true) : SIZE_MAX;
    (void)tb_widget_popup(
        submenu, &(tb_gui_popup_options){.anchor = tb_gui_menu_item_rect(widget, menu, index),
                                         .placement = TB_GUI_POPUP_RIGHT});
}

/* Choose an item: a cascade opens; any other item closes the menus, is
 * checked as its kind has it, and calls its command. */
static void tb_gui_menu_activate(tb_widget widget, tb_gui_menu *menu, size_t index, bool by_key,
                                 const tb_gui_event *cause)
{
    tb_gui_menu_item *const item = &menu->items[index];

    if (!tb_gui_menu_selectable(item))
    {
        return;
    }
    if (item->kind == TB_MENU_ITEM_CASCADE)
    {
        tb_gui_menu_open_cascade(widget, menu, index, by_key);
        return;
    }
    if (item->kind == TB_MENU_ITEM_CHECK)
    {
        item->checked = !item->checked;
    }
    if (item->kind == TB_MENU_ITEM_RADIO)
    {
        for (size_t at = 0; at < menu->count; at++)
        {
            if (menu->items[at].kind == TB_MENU_ITEM_RADIO && menu->items[at].group == item->group)
            {
                menu->items[at].checked = at == index;
            }
        }
    }
    const tb_gui_command command = item->command;
    tb_gui_close_popups(widget.gui);
    if (command.callback != NULL)
    {
        tb_gui_event event = *cause;
        event.kind = TB_GUI_EVENT_COMMAND;
        event.widget = widget;
        event.target = widget;
        command.callback(&event, command.user);
    }
}

static void tb_gui_menu_motion(tb_widget widget, tb_gui_menu *menu, const tb_gui_event *event)
{
    const size_t index = tb_gui_menu_item_at(widget, menu, event->position);

    if (index == SIZE_MAX || index == menu->active)
    {
        return;
    }
    menu->active = tb_gui_menu_selectable(&menu->items[index]) ? index : SIZE_MAX;
    if (menu->active != SIZE_MAX && menu->items[index].kind == TB_MENU_ITEM_CASCADE)
    {
        tb_gui_menu_open_cascade(widget, menu, index, false);
        return;
    }
    tb_gui_menu_close_cascade(widget);
}

static bool tb_gui_menu_mnemonic(tb_widget widget, tb_gui_menu *menu, const tb_gui_event *event)
{
    const char letter = tb_gui_key_letter(event->key);

    for (size_t index = 0; letter != 0 && index < menu->count; index++)
    {
        if (menu->items[index].label.mnemonic == letter &&
            tb_gui_menu_selectable(&menu->items[index]))
        {
            menu->active = index;
            tb_gui_menu_activate(widget, menu, index, true, event);
            return true;
        }
    }
    return false;
}

static bool tb_gui_menu_key(tb_widget widget, tb_gui_menu *menu, const tb_gui_event *event)
{
    const bool cascade = tb_gui_menu_of(tb_widget_parent(widget)) != NULL;

    /* A key a cascade did not use passes its menu on to the menu bar. */
    if (!tb_widget_equal(event->target, widget))
    {
        return false;
    }
    if (event->key == TB_KEY_UP || event->key == TB_KEY_DOWN)
    {
        menu->active = tb_gui_menu_step(menu, menu->active, event->key == TB_KEY_DOWN);
        tb_gui_menu_close_cascade(widget);
        return true;
    }
    if (event->key == TB_KEY_RIGHT && menu->active != SIZE_MAX &&
        menu->items[menu->active].kind == TB_MENU_ITEM_CASCADE)
    {
        tb_gui_menu_open_cascade(widget, menu, menu->active, true);
        return true;
    }
    if (event->key == TB_KEY_LEFT && cascade)
    {
        tb_widget_close_popup(widget);
        return true;
    }
    if ((event->key == TB_KEY_ENTER || event->key == TB_KEY_SPACE) && menu->active != SIZE_MAX)
    {
        tb_gui_menu_activate(widget, menu, menu->active, true, event);
        return true;
    }
    return tb_gui_menu_mnemonic(widget, menu, event);
}

static bool tb_gui_menu_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_menu *const menu = data;

    switch (event->kind)
    {
    case TB_GUI_EVENT_MOTION:
        tb_gui_menu_motion(widget, menu, event);
        return true;
    case TB_GUI_EVENT_LEAVE: {
        /* The item whose cascade is open stays lit. */
        const bool open =
            menu->active != SIZE_MAX && tb_widget_is_popup(menu->items[menu->active].submenu);
        menu->active = open ? menu->active : SIZE_MAX;
        return false;
    }
    case TB_GUI_EVENT_CLICK: {
        const size_t index = tb_gui_menu_item_at(widget, menu, event->position);
        if (event->button == TB_MOUSE_BUTTON_LEFT && index != SIZE_MAX)
        {
            tb_gui_menu_activate(widget, menu, index, false, event);
        }
        return true;
    }
    case TB_GUI_EVENT_KEY:
        return tb_gui_menu_key(widget, menu, event);
    case TB_GUI_EVENT_POPUP_CLOSE:
        menu->active = SIZE_MAX;
        return true;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_PRESS:
    case TB_GUI_EVENT_RELEASE:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_WHEEL:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

/* What a layout file cannot hold yet (tb_widget_class::unsaved). */
static const char *tb_gui_menu_unsaved(tb_widget widget, const void *data)
{
    const tb_gui_menu *const menu = data;

    (void)widget;
    return menu->count > 0 ? "menu items" : NULL;
}

static const tb_widget_class tb_gui_menu_kind = {.name = "menu",
                                                 .data_size = sizeof(tb_gui_menu),
                                                 .mouse_filter = TB_GUI_MOUSE_STOP,
                                                 .init = tb_gui_menu_init,
                                                 .destroy = tb_gui_menu_destroy,
                                                 .measure = tb_gui_menu_measure,
                                                 .draw = tb_gui_menu_draw,
                                                 .event = tb_gui_menu_event,
                                                 .unsaved = tb_gui_menu_unsaved};

static tb_gui_menu *tb_gui_menu_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_menu_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_menu_class(void)
{
    return &tb_gui_menu_kind;
}

tb_widget tb_menu(tb_widget parent)
{
    return tb_widget_create(parent, &tb_gui_menu_kind);
}

size_t tb_menu_add(tb_widget menu, const tb_menu_item_desc *item)
{
    tb_gui_menu *const data = tb_gui_menu_of(menu);

    TB_ASSERT(item != NULL);
    TB_ASSERT((uint32_t)item->kind <= (uint32_t)TB_MENU_ITEM_SEPARATOR);
    if (data == NULL)
    {
        return SIZE_MAX;
    }
    if (data->count == data->capacity)
    {
        data->capacity = data->capacity == 0 ? TB_GUI_MENU_FIRST : data->capacity * 2;
        data->items = tb_realloc_array(data->items, data->capacity, sizeof(*data->items));
    }
    tb_gui_menu_item *const added = &data->items[data->count];
    *added = (tb_gui_menu_item){.kind = (int32_t)item->kind,
                                .command = {item->command, item->user},
                                .checked = item->checked,
                                .group = item->group,
                                .submenu = item->submenu,
                                .disabled = item->disabled};
    tb_gui_menu_label_set(&added->label, item->label);
    tb_gui_string_set(&added->accelerator,
                      tb_text_from_cstr(item->accelerator != NULL ? item->accelerator : ""));
    data->count++;
    tb_gui_mark_layout(menu.gui);
    return data->count - 1;
}

size_t tb_menu_item_count(tb_widget menu)
{
    const tb_gui_menu *const data = tb_gui_menu_of(menu);
    return data != NULL ? data->count : 0;
}

bool tb_menu_item_checked(tb_widget menu, size_t index)
{
    const tb_gui_menu *const data = tb_gui_menu_of(menu);
    return data != NULL && index < data->count && data->items[index].checked;
}

bool tb_menu_set_checked(tb_widget menu, size_t index, bool checked)
{
    tb_gui_menu *const data = tb_gui_menu_of(menu);

    if (data == NULL || index >= data->count)
    {
        return false;
    }
    data->items[index].checked = checked;
    return true;
}

bool tb_menu_set_item_enabled(tb_widget menu, size_t index, bool enabled)
{
    tb_gui_menu *const data = tb_gui_menu_of(menu);

    if (data == NULL || index >= data->count)
    {
        return false;
    }
    data->items[index].disabled = !enabled;
    return true;
}

size_t tb_menu_active(tb_widget menu)
{
    const tb_gui_menu *const data = tb_gui_menu_of(menu);
    return data != NULL ? data->active : SIZE_MAX;
}

bool tb_menu_invoke(tb_widget menu, size_t index)
{
    tb_gui_menu *const data = tb_gui_menu_of(menu);

    if (data == NULL || index >= data->count || !tb_gui_menu_selectable(&data->items[index]))
    {
        return false;
    }
    const tb_gui_event cause = {.kind = TB_GUI_EVENT_COMMAND, .widget = menu, .target = menu};
    tb_gui_menu_activate(menu, data, index, true, &cause);
    return true;
}

bool tb_menu_popup(tb_widget menu, tb_vec2 position)
{
    tb_gui_menu *const data = tb_gui_menu_of(menu);

    if (data == NULL)
    {
        return false;
    }
    tb_gui_close_popups(menu.gui);
    data->active = SIZE_MAX;
    return tb_widget_popup(menu,
                           &(tb_gui_popup_options){.anchor = {.x = position.x, .y = position.y},
                                                   .placement = TB_GUI_POPUP_AT});
}

/* ---------------------------------------------------------------------------
 * Menu bars.
 * ------------------------------------------------------------------------- */

typedef struct tb_gui_menubar_entry
{
    tb_gui_menu_label label;
    tb_widget menu;
} tb_gui_menubar_entry;

typedef struct tb_gui_menubar
{
    tb_gui_menubar_entry *entries;
    size_t count;
    size_t capacity;
    size_t hot; /* SIZE_MAX for none */
} tb_gui_menubar;

static tb_gui_menubar *tb_gui_menubar_of(tb_widget widget);

static void tb_gui_menubar_init(tb_widget widget, void *data)
{
    tb_gui_menubar *const bar = data;

    (void)widget;
    bar->hot = SIZE_MAX;
}

static void tb_gui_menubar_destroy(tb_widget widget, void *data)
{
    tb_gui_menubar *const bar = data;

    (void)widget;
    for (size_t index = 0; index < bar->count; index++)
    {
        tb_gui_string_free(&bar->entries[index].label.text);
    }
    tb_free(bar->entries);
}

static float tb_gui_menubar_entry_width(tb_gui *gui, const tb_gui_menubar_entry *entry)
{
    return tb_gui_label_width(gui, &entry->label.text) + 2.0f * tb_gui_theme(gui)->frame_padding.x;
}

static tb_canvas_rect tb_gui_menubar_entry_rect(tb_widget widget, const tb_gui_menubar *bar,
                                                size_t index)
{
    const tb_canvas_rect rect = tb_widget_rect(widget);
    float left = rect.x;

    for (size_t at = 0; at < index; at++)
    {
        left += tb_gui_menubar_entry_width(widget.gui, &bar->entries[at]);
    }
    return (tb_canvas_rect){
        left, rect.y, tb_gui_menubar_entry_width(widget.gui, &bar->entries[index]), rect.height};
}

static size_t tb_gui_menubar_entry_at(tb_widget widget, const tb_gui_menubar *bar, tb_vec2 point)
{
    for (size_t index = 0; index < bar->count; index++)
    {
        if (tb_ui_rect_contains(tb_gui_menubar_entry_rect(widget, bar, index), point))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

/* The entry whose menu is open, or SIZE_MAX. */
static size_t tb_gui_menubar_open_entry(const tb_gui_menubar *bar)
{
    for (size_t index = 0; index < bar->count; index++)
    {
        if (tb_widget_is_popup(bar->entries[index].menu))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

static void tb_gui_menubar_open(tb_widget widget, const tb_gui_menubar *bar, size_t index,
                                bool by_key)
{
    const tb_widget menu = bar->entries[index].menu;
    tb_gui_menu *const data = tb_gui_menu_of(menu);

    tb_gui_close_popups(widget.gui);
    if (data == NULL)
    {
        return;
    }
    data->active = by_key ? tb_gui_menu_step(data, SIZE_MAX, true) : SIZE_MAX;
    (void)tb_widget_popup(
        menu, &(tb_gui_popup_options){.anchor = tb_gui_menubar_entry_rect(widget, bar, index),
                                      .placement = TB_GUI_POPUP_BELOW});
}

static tb_vec2 tb_gui_menubar_measure(tb_widget widget, const void *data)
{
    const tb_gui_menubar *const bar = data;
    float width = 0.0f;

    for (size_t index = 0; index < bar->count; index++)
    {
        width += tb_gui_menubar_entry_width(widget.gui, &bar->entries[index]);
    }
    return (tb_vec2){width, tb_gui_menu_row_height(widget.gui)};
}

static void tb_gui_menubar_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_menubar *const bar = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const size_t open = tb_gui_menubar_open_entry(bar);
    const bool disabled = (tb_widget_state(widget) & TB_WIDGET_DISABLED) != 0;
    const tb_vec4 color = theme->colors[disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT];

    tb_gui_paint_fill(paint, tb_gui_paint_rect(paint), 0.0f, theme->colors[TB_UI_COLOR_HEADER]);
    for (size_t index = 0; index < bar->count; index++)
    {
        const tb_canvas_rect rect = tb_gui_menubar_entry_rect(widget, bar, index);
        if (index == open)
        {
            tb_gui_paint_fill(paint, rect, 0.0f, theme->colors[TB_UI_COLOR_SELECTION]);
        }
        else if (index == bar->hot)
        {
            tb_gui_paint_fill(paint, rect, 0.0f, theme->colors[TB_UI_COLOR_HEADER_HOT]);
        }
        tb_gui_menu_draw_label(paint, &bar->entries[index].label,
                               tb_ui_rect_inset(rect, theme->frame_padding.x, 0.0f), color);
    }
}

static bool tb_gui_menubar_key(tb_widget widget, const tb_gui_menubar *bar,
                               const tb_gui_event *event)
{
    const size_t open = tb_gui_menubar_open_entry(bar);

    if (open == SIZE_MAX || bar->count == 0)
    {
        return false;
    }
    if (event->key == TB_KEY_LEFT)
    {
        tb_gui_menubar_open(widget, bar, open > 0 ? open - 1 : bar->count - 1, true);
        return true;
    }
    if (event->key == TB_KEY_RIGHT)
    {
        tb_gui_menubar_open(widget, bar, open + 1 < bar->count ? open + 1 : 0, true);
        return true;
    }
    return false;
}

static bool tb_gui_menubar_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_menubar *const bar = data;

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS: {
        const size_t index = tb_gui_menubar_entry_at(widget, bar, event->position);
        if (event->button == TB_MOUSE_BUTTON_LEFT && index != SIZE_MAX)
        {
            tb_gui_menubar_open(widget, bar, index, false);
        }
        return true;
    }
    case TB_GUI_EVENT_MOTION: {
        /* With a menu open, the mouse over another title opens its menu. */
        const size_t open = tb_gui_menubar_open_entry(bar);
        bar->hot = tb_gui_menubar_entry_at(widget, bar, event->position);
        if (open != SIZE_MAX && bar->hot != SIZE_MAX && bar->hot != open)
        {
            tb_gui_menubar_open(widget, bar, bar->hot, false);
        }
        return true;
    }
    case TB_GUI_EVENT_LEAVE:
        bar->hot = SIZE_MAX;
        return false;
    case TB_GUI_EVENT_KEY:
        return tb_gui_menubar_key(widget, bar, event);
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_RELEASE:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_WHEEL:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

/* What a layout file cannot hold yet (tb_widget_class::unsaved). */
static const char *tb_gui_menubar_unsaved(tb_widget widget, const void *data)
{
    const tb_gui_menubar *const bar = data;

    (void)widget;
    return bar->count > 0 ? "menu bar entries" : NULL;
}

static const tb_widget_class tb_gui_menubar_kind = {.name = "menubar",
                                                    .data_size = sizeof(tb_gui_menubar),
                                                    .mouse_filter = TB_GUI_MOUSE_STOP,
                                                    .init = tb_gui_menubar_init,
                                                    .destroy = tb_gui_menubar_destroy,
                                                    .measure = tb_gui_menubar_measure,
                                                    .draw = tb_gui_menubar_draw,
                                                    .event = tb_gui_menubar_event,
                                                    .unsaved = tb_gui_menubar_unsaved};

static tb_gui_menubar *tb_gui_menubar_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_menubar_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_menubar_class(void)
{
    return &tb_gui_menubar_kind;
}

tb_widget tb_menubar(tb_widget parent)
{
    return tb_widget_create(parent, &tb_gui_menubar_kind);
}

size_t tb_menubar_add(tb_widget menubar, const char *label, tb_widget menu)
{
    tb_gui_menubar *const bar = tb_gui_menubar_of(menubar);

    TB_ASSERT(label != NULL);
    if (bar == NULL)
    {
        return SIZE_MAX;
    }
    if (bar->count == bar->capacity)
    {
        bar->capacity = bar->capacity == 0 ? TB_GUI_MENU_FIRST : bar->capacity * 2;
        bar->entries = tb_realloc_array(bar->entries, bar->capacity, sizeof(*bar->entries));
    }
    tb_gui_menubar_entry *const entry = &bar->entries[bar->count];
    *entry = (tb_gui_menubar_entry){.menu = menu};
    tb_gui_menu_label_set(&entry->label, label);
    bar->count++;
    tb_gui_mark_layout(menubar.gui);
    return bar->count - 1;
}

void tb_gui_register_menu_kinds(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_menu_kind);
    (void)tb_gui_register_class(gui, &tb_gui_menubar_kind);
}
