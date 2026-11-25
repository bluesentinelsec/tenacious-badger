/* The notebook kind, after Tk's `ttk.Notebook`: a row of tabs over a page,
 * one page a child widget, the others hidden. A tab is chosen by a click,
 * by Control+Tab and Control+Page Up and Down from inside the notebook,
 * and by the arrows while the notebook has the focus. Its pages share its
 * grid's one cell. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Tabs a notebook first holds. */
#define TB_GUI_NOTEBOOK_FIRST 4u

typedef struct tb_gui_notebook_tab
{
    tb_widget page;
    tb_gui_string text;
} tb_gui_notebook_tab;

typedef struct tb_gui_notebook
{
    tb_gui_notebook_tab *tabs;
    size_t count;
    size_t capacity;
    size_t selected; /* SIZE_MAX for none */
    float padding;
    tb_gui_command command;
    size_t hot;
} tb_gui_notebook;

static tb_gui_notebook *tb_gui_notebook_of(tb_widget widget);

static void tb_gui_notebook_init(tb_widget widget, void *data)
{
    tb_gui_notebook *const notebook = data;

    (void)widget;
    notebook->selected = SIZE_MAX;
    notebook->hot = SIZE_MAX;
}

static void tb_gui_notebook_destroy(tb_widget widget, void *data)
{
    tb_gui_notebook *const notebook = data;

    (void)widget;
    for (size_t index = 0; index < notebook->count; index++)
    {
        tb_gui_string_free(&notebook->tabs[index].text);
    }
    tb_free(notebook->tabs);
}

/* Forget the tabs whose pages were destroyed. */
static void tb_gui_notebook_prune(tb_gui_notebook *notebook)
{
    size_t kept = 0;
    size_t selected = SIZE_MAX;

    for (size_t index = 0; index < notebook->count; index++)
    {
        if (!tb_widget_alive(notebook->tabs[index].page))
        {
            tb_gui_string_free(&notebook->tabs[index].text);
            continue;
        }
        selected = index == notebook->selected ? kept : selected;
        notebook->tabs[kept] = notebook->tabs[index];
        kept++;
    }
    if (kept == notebook->count)
    {
        return;
    }
    /* The page shown was destroyed: the first is shown in its place. */
    notebook->count = kept;
    notebook->selected = selected != SIZE_MAX || kept == 0 ? selected : 0;
    notebook->hot = SIZE_MAX;
    for (size_t index = 0; index < kept; index++)
    {
        tb_widget_set_visible(notebook->tabs[index].page, index == notebook->selected);
    }
}

static float tb_gui_notebook_tab_height(tb_gui *gui)
{
    return tb_gui_line_height(gui) + 2.0f * tb_gui_theme(gui)->frame_padding.y;
}

static float tb_gui_notebook_tab_width(tb_gui *gui, const tb_gui_notebook_tab *tab)
{
    const tb_gui_text_block block = {.text = tb_gui_string_text(&tab->text)};
    return tb_gui_measure_text(gui, &block).x + 2.0f * tb_gui_theme(gui)->frame_padding.x;
}

static tb_canvas_rect tb_gui_notebook_tab_rect(tb_widget widget, const tb_gui_notebook *notebook,
                                               size_t index)
{
    const tb_canvas_rect rect = tb_widget_rect(widget);
    float left = rect.x;

    for (size_t at = 0; at < index; at++)
    {
        left += tb_gui_notebook_tab_width(widget.gui, &notebook->tabs[at]);
    }
    return (tb_canvas_rect){left, rect.y,
                            tb_gui_notebook_tab_width(widget.gui, &notebook->tabs[index]),
                            tb_gui_notebook_tab_height(widget.gui)};
}

static size_t tb_gui_notebook_tab_at(tb_widget widget, const tb_gui_notebook *notebook,
                                     tb_vec2 point)
{
    for (size_t index = 0; index < notebook->count; index++)
    {
        if (tb_ui_rect_contains(tb_gui_notebook_tab_rect(widget, notebook, index), point))
        {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Show a tab's page and hide the others; the command is called when the
 * user changed it. */
static void tb_gui_notebook_show(tb_widget widget, tb_gui_notebook *notebook, size_t index,
                                 bool by_user)
{
    if (index >= notebook->count || index == notebook->selected)
    {
        return;
    }
    notebook->selected = index;
    for (size_t at = 0; at < notebook->count; at++)
    {
        tb_widget_set_visible(notebook->tabs[at].page, at == index);
    }
    if (by_user)
    {
        (void)tb_widget_invoke(widget);
    }
}

static tb_vec2 tb_gui_notebook_measure(tb_widget widget, const void *data)
{
    const tb_gui_notebook *const notebook = data;
    float width = 0.0f;

    for (size_t index = 0; index < notebook->count; index++)
    {
        width += tb_gui_notebook_tab_width(widget.gui, &notebook->tabs[index]);
    }
    return (tb_vec2){width, 0.0f};
}

static tb_gui_edges tb_gui_notebook_insets(tb_widget widget, const void *data)
{
    const tb_gui_notebook *const notebook = data;
    const float edge = notebook->padding + tb_gui_theme(widget.gui)->border;

    return (tb_gui_edges){edge, tb_gui_notebook_tab_height(widget.gui) + edge, edge, edge};
}

static void tb_gui_notebook_arrange(tb_widget widget, void *data, tb_canvas_rect rect)
{
    (void)widget;
    (void)rect;
    tb_gui_notebook_prune(data);
}

static void tb_gui_notebook_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_notebook *const notebook = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect rect = tb_gui_paint_rect(paint);
    const float tabs = tb_gui_notebook_tab_height(widget.gui);
    const tb_canvas_rect page = {rect.x, rect.y + tabs, rect.width,
                                 fmaxf(rect.height - tabs, 0.0f)};
    const uint32_t state = tb_widget_state(widget);

    tb_gui_paint_fill(paint, page, 0.0f, theme->colors[TB_UI_COLOR_WINDOW]);
    tb_gui_paint_outline(paint, page, 0.0f, theme->border, theme->colors[TB_UI_COLOR_BORDER]);
    for (size_t index = 0; index < notebook->count; index++)
    {
        const tb_canvas_rect tab = tb_gui_notebook_tab_rect(widget, notebook, index);
        const bool selected = index == notebook->selected;
        const tb_ui_color fill = selected                 ? TB_UI_COLOR_WINDOW
                                 : index == notebook->hot ? TB_UI_COLOR_HEADER_HOT
                                                          : TB_UI_COLOR_HEADER;
        const tb_gui_text_block block = {.text = tb_gui_string_text(&notebook->tabs[index].text),
                                         .anchor = TB_GUI_ANCHOR_CENTER};
        tb_gui_paint_fill(paint, tab, 0.0f, theme->colors[fill]);
        tb_gui_paint_outline(paint, tab, 0.0f, theme->border, theme->colors[TB_UI_COLOR_BORDER]);
        if (selected)
        {
            /* The chosen tab opens onto its page: its line along the top. */
            tb_gui_paint_fill(paint,
                              (tb_canvas_rect){tab.x, tab.y, tab.width, 2.0f * theme->border}, 0.0f,
                              theme->colors[(state & TB_WIDGET_FOCUSED) != 0 ? TB_UI_COLOR_ACCENT
                                                                             : TB_UI_COLOR_BORDER]);
        }
        tb_gui_paint_text(paint, &block, tab,
                          theme->colors[selected ? TB_UI_COLOR_TEXT : TB_UI_COLOR_TEXT_DIM]);
    }
}

/* The tab before or after the one shown, around the ends. */
static void tb_gui_notebook_step(tb_widget widget, tb_gui_notebook *notebook, bool forward)
{
    if (notebook->count == 0)
    {
        return;
    }
    const size_t at = notebook->selected < notebook->count ? notebook->selected : 0;
    const size_t next =
        forward ? (at + 1) % notebook->count : (at + notebook->count - 1) % notebook->count;
    tb_gui_notebook_show(widget, notebook, next, true);
}

static bool tb_gui_notebook_key(tb_widget widget, tb_gui_notebook *notebook,
                                const tb_gui_event *event)
{
    const bool control = (event->modifiers & TB_MODIFIER_CONTROL) != 0;
    const bool shift = (event->modifiers & TB_MODIFIER_SHIFT) != 0;
    const bool own = tb_widget_equal(event->target, widget);

    if (control && event->key == TB_KEY_TAB)
    {
        tb_gui_notebook_step(widget, notebook, !shift);
        return true;
    }
    if (control && (event->key == TB_KEY_PAGE_DOWN || event->key == TB_KEY_PAGE_UP))
    {
        tb_gui_notebook_step(widget, notebook, event->key == TB_KEY_PAGE_DOWN);
        return true;
    }
    if (own && (event->key == TB_KEY_LEFT || event->key == TB_KEY_RIGHT))
    {
        tb_gui_notebook_step(widget, notebook, event->key == TB_KEY_RIGHT);
        return true;
    }
    return false;
}

static bool tb_gui_notebook_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_notebook *const notebook = data;

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS: {
        const size_t index = tb_gui_notebook_tab_at(widget, notebook, event->position);
        if (event->button == TB_MOUSE_BUTTON_LEFT && index != SIZE_MAX)
        {
            tb_gui_notebook_show(widget, notebook, index, true);
        }
        return true;
    }
    case TB_GUI_EVENT_MOTION:
        notebook->hot = tb_gui_notebook_tab_at(widget, notebook, event->position);
        return false;
    case TB_GUI_EVENT_LEAVE:
        notebook->hot = SIZE_MAX;
        return false;
    case TB_GUI_EVENT_KEY:
        return tb_gui_notebook_key(widget, notebook, event);
    case TB_GUI_EVENT_COMMAND:
        if (notebook->command.callback != NULL)
        {
            notebook->command.callback(event, notebook->command.user);
        }
        return true;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_RELEASE:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_WHEEL:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static bool tb_gui_notebook_set_selected(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_notebook *const notebook = data;

    if (value->integer < 0 || (size_t)value->integer >= notebook->count)
    {
        return false;
    }
    tb_gui_notebook_show(widget, notebook, (size_t)value->integer, false);
    return true;
}

static bool tb_gui_notebook_get_selected(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_notebook *const notebook = data;

    (void)widget;
    *value = tb_gui_int(notebook->selected != SIZE_MAX ? (int32_t)notebook->selected : -1);
    return true;
}

static bool tb_gui_notebook_set_padding(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_notebook *const notebook = data;

    (void)widget;
    if (value->number < 0.0f)
    {
        return false;
    }
    notebook->padding = value->number;
    return true;
}

static bool tb_gui_notebook_get_padding(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_notebook *const notebook = data;

    (void)widget;
    *value = tb_gui_float(notebook->padding);
    return true;
}

static const tb_gui_property tb_gui_notebook_properties[] = {
    {.name = "selected",
     .type = TB_GUI_TYPE_INT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_notebook_get_selected,
     .set = tb_gui_notebook_set_selected},
    {.name = "padding",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_notebook_get_padding,
     .set = tb_gui_notebook_set_padding},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_notebook, command)},
};

static const tb_widget_class tb_gui_notebook_kind = {
    .name = "notebook",
    .data_size = sizeof(tb_gui_notebook),
    .properties = tb_gui_notebook_properties,
    .property_count = TB_GUI_COUNT_OF(tb_gui_notebook_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_notebook_init,
    .destroy = tb_gui_notebook_destroy,
    .measure = tb_gui_notebook_measure,
    .insets = tb_gui_notebook_insets,
    .arrange = tb_gui_notebook_arrange,
    .draw = tb_gui_notebook_draw,
    .event = tb_gui_notebook_event};

static tb_gui_notebook *tb_gui_notebook_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_notebook_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_notebook_class(void)
{
    return &tb_gui_notebook_kind;
}

tb_widget tb_notebook(tb_widget parent, const tb_notebook_desc *desc)
{
    const tb_notebook_desc chosen = desc != NULL ? *desc : (tb_notebook_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_notebook_kind);
    const tb_gui_value padding = tb_gui_float(chosen.padding);
    const tb_gui_value command = tb_gui_command_of(chosen.command, chosen.user);

    (void)tb_widget_set(widget, "padding", &padding);
    (void)tb_widget_set(widget, "command", &command);
    return widget;
}

size_t tb_notebook_add(tb_widget notebook, tb_widget page, const char *text)
{
    tb_gui_notebook *const data = tb_gui_notebook_of(notebook);

    TB_ASSERT(text != NULL);
    if (data == NULL || !tb_widget_equal(tb_widget_parent(page), notebook))
    {
        return SIZE_MAX;
    }
    tb_gui_notebook_prune(data);
    if (data->count == data->capacity)
    {
        data->capacity = data->capacity == 0 ? TB_GUI_NOTEBOOK_FIRST : data->capacity * 2;
        data->tabs = tb_realloc_array(data->tabs, data->capacity, sizeof(*data->tabs));
    }
    tb_gui_notebook_tab *const tab = &data->tabs[data->count];
    *tab = (tb_gui_notebook_tab){.page = page};
    tb_gui_string_set(&tab->text, tb_text_from_cstr(text));
    data->count++;
    (void)tb_grid(page, &(tb_grid_options){.sticky = TB_STICKY_NSEW});
    (void)tb_grid_row_configure(notebook, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_column_configure(notebook, 0, &(tb_grid_track){.weight = 1.0f});
    tb_widget_set_visible(page, data->selected == SIZE_MAX);
    if (data->selected == SIZE_MAX)
    {
        data->selected = data->count - 1;
    }
    return data->count - 1;
}

size_t tb_notebook_tab_count(tb_widget notebook)
{
    tb_gui_notebook *const data = tb_gui_notebook_of(notebook);

    if (data == NULL)
    {
        return 0;
    }
    tb_gui_notebook_prune(data);
    return data->count;
}

size_t tb_notebook_selected(tb_widget notebook)
{
    tb_gui_notebook *const data = tb_gui_notebook_of(notebook);

    if (data == NULL)
    {
        return SIZE_MAX;
    }
    tb_gui_notebook_prune(data);
    return data->selected;
}

bool tb_notebook_select(tb_widget notebook, size_t index)
{
    tb_gui_notebook *const data = tb_gui_notebook_of(notebook);

    if (data == NULL)
    {
        return false;
    }
    tb_gui_notebook_prune(data);
    if (index >= data->count)
    {
        return false;
    }
    tb_gui_notebook_show(notebook, data, index, false);
    return true;
}

tb_widget tb_notebook_page(tb_widget notebook, size_t index)
{
    tb_gui_notebook *const data = tb_gui_notebook_of(notebook);

    if (data == NULL)
    {
        return TB_WIDGET_NONE;
    }
    tb_gui_notebook_prune(data);
    return index < data->count ? data->tabs[index].page : TB_WIDGET_NONE;
}

bool tb_notebook_set_tab_text(tb_widget notebook, size_t index, const char *text)
{
    tb_gui_notebook *const data = tb_gui_notebook_of(notebook);

    TB_ASSERT(text != NULL);
    if (data == NULL)
    {
        return false;
    }
    tb_gui_notebook_prune(data);
    if (index >= data->count)
    {
        return false;
    }
    tb_gui_string_set(&data->tabs[index].text, tb_text_from_cstr(text));
    tb_gui_mark_layout(notebook.gui);
    return true;
}

void tb_gui_register_notebook(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_notebook_kind);
}
