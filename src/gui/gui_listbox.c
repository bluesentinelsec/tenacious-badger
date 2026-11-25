/* The listbox kind: a list of text items, chosen one at a time or several,
 * by the mouse and the keyboard, after Tk's `listbox`. Items are data, not
 * widgets, so a list of many is cheap: only the rows that can be seen are
 * drawn, and the list scrolls itself, with a bar, when it is given less
 * room than its rows. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/size.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Ems of text a list is wide when it is given no width. */
#define TB_GUI_LISTBOX_EMS 10.0f
/* Rows a notch of the wheel scrolls. */
#define TB_GUI_LISTBOX_WHEEL_ROWS 3.0f
/* Items a list first holds. */
#define TB_GUI_LISTBOX_FIRST 16u

static const char *const tb_gui_selectmode_names[] = {"browse", "single", "multiple", "extended"};

typedef struct tb_gui_listbox_item
{
    tb_gui_string text;
    bool selected;
} tb_gui_listbox_item;

typedef struct tb_gui_listbox
{
    tb_gui_listbox_item *items;
    size_t count;
    size_t capacity;
    tb_gui_string joined; /* the items as one text, for the `items` property */
    int32_t selectmode;
    float width;
    int32_t height;
    size_t active;
    size_t anchor;
    float offset;
    tb_gui_command command;
    /* As last arranged: where rows are seen, and the bar. */
    tb_canvas_rect view;
    tb_canvas_rect track;
    bool bar;
    /* The mouse. */
    size_t hot_row;
    bool selecting;
    bool dragging_bar;
    float grab;
} tb_gui_listbox;

static tb_gui_listbox *tb_gui_listbox_of(tb_widget widget);

static void tb_gui_listbox_init(tb_widget widget, void *data)
{
    tb_gui_listbox *const listbox = data;

    (void)widget;
    listbox->hot_row = SIZE_MAX;
}

static void tb_gui_listbox_destroy(tb_widget widget, void *data)
{
    tb_gui_listbox *const listbox = data;

    (void)widget;
    for (size_t index = 0; index < listbox->count; index++)
    {
        tb_gui_string_free(&listbox->items[index].text);
    }
    tb_free(listbox->items);
    tb_gui_string_free(&listbox->joined);
}

static float tb_gui_listbox_row_height(tb_widget widget)
{
    return tb_gui_line_height(widget.gui) + tb_gui_theme(widget.gui)->frame_padding.y;
}

static size_t tb_gui_listbox_rows_shown(const tb_gui_listbox *listbox)
{
    if (listbox->height > 0)
    {
        return (size_t)listbox->height;
    }
    return listbox->count > 0 ? listbox->count : 1u;
}

/* ---------------------------------------------------------------------------
 * Items.
 * ------------------------------------------------------------------------- */

static void tb_gui_listbox_changed(tb_widget widget, tb_gui_listbox *listbox)
{
    tb_gui_string_free(&listbox->joined);
    if (listbox->active >= listbox->count)
    {
        listbox->active = listbox->count > 0 ? listbox->count - 1 : 0;
    }
    listbox->hot_row = SIZE_MAX;
    tb_gui_mark_layout(widget.gui);
}

static void tb_gui_listbox_insert_text(tb_gui_listbox *listbox, size_t index, tb_text text)
{
    if (listbox->count == listbox->capacity)
    {
        listbox->capacity = listbox->capacity == 0 ? TB_GUI_LISTBOX_FIRST : listbox->capacity * 2;
        listbox->items =
            tb_realloc_array(listbox->items, listbox->capacity, sizeof(*listbox->items));
    }
    memmove(&listbox->items[index + 1], &listbox->items[index],
            (listbox->count - index) * sizeof(*listbox->items));
    listbox->items[index] = (tb_gui_listbox_item){0};
    tb_gui_string_set(&listbox->items[index].text, text);
    listbox->count++;
}

static void tb_gui_listbox_clear(tb_gui_listbox *listbox)
{
    for (size_t index = 0; index < listbox->count; index++)
    {
        tb_gui_string_free(&listbox->items[index].text);
    }
    listbox->count = 0;
}

size_t tb_listbox_insert(tb_widget listbox, size_t index, const char *text)
{
    tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    TB_ASSERT(text != NULL);
    if (data == NULL)
    {
        return SIZE_MAX;
    }
    const size_t at = index < data->count ? index : data->count;
    tb_gui_listbox_insert_text(data, at, tb_text_from_cstr(text));
    tb_gui_listbox_changed(listbox, data);
    return at;
}

size_t tb_listbox_delete(tb_widget listbox, size_t first, size_t count)
{
    tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    if (data == NULL || first >= data->count)
    {
        return 0;
    }
    const size_t removed = count < data->count - first ? count : data->count - first;
    for (size_t index = first; index < first + removed; index++)
    {
        tb_gui_string_free(&data->items[index].text);
    }
    memmove(&data->items[first], &data->items[first + removed],
            (data->count - first - removed) * sizeof(*data->items));
    data->count -= removed;
    tb_gui_listbox_changed(listbox, data);
    return removed;
}

size_t tb_listbox_size(tb_widget listbox)
{
    const tb_gui_listbox *const data = tb_gui_listbox_of(listbox);
    return data != NULL ? data->count : 0;
}

tb_text tb_listbox_get(tb_widget listbox, size_t index)
{
    const tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    if (data == NULL || index >= data->count)
    {
        return tb_text_make("", 0);
    }
    return tb_gui_string_text(&data->items[index].text);
}

/* ---------------------------------------------------------------------------
 * The selection.
 * ------------------------------------------------------------------------- */

static bool tb_gui_listbox_many(const tb_gui_listbox *listbox)
{
    return listbox->selectmode == TB_LISTBOX_MULTIPLE || listbox->selectmode == TB_LISTBOX_EXTENDED;
}

/* Select from one row to another, both included, and no other. Returns
 * whether the selection changed. */
static bool tb_gui_listbox_select_range(tb_gui_listbox *listbox, size_t from, size_t to)
{
    const size_t low = from < to ? from : to;
    const size_t high = from < to ? to : from;
    bool changed = false;

    for (size_t index = 0; index < listbox->count; index++)
    {
        const bool selected = index >= low && index <= high;
        changed = changed || listbox->items[index].selected != selected;
        listbox->items[index].selected = selected;
    }
    return changed;
}

/* A row chosen by the mouse or a key, as the selection mode has it. */
static void tb_gui_listbox_choose(tb_widget widget, tb_gui_listbox *listbox, size_t row,
                                  uint32_t modifiers, bool toggle)
{
    const bool shift = (modifiers & TB_MODIFIER_SHIFT) != 0;
    const bool control = (modifiers & TB_MODIFIER_CONTROL) != 0;
    bool changed = false;

    listbox->active = row;
    switch ((tb_listbox_selectmode)listbox->selectmode)
    {
    case TB_LISTBOX_MULTIPLE:
        if (toggle)
        {
            listbox->items[row].selected = !listbox->items[row].selected;
            changed = true;
        }
        break;
    case TB_LISTBOX_EXTENDED:
        if (shift)
        {
            changed = tb_gui_listbox_select_range(listbox, listbox->anchor, row);
            break;
        }
        listbox->anchor = row;
        if (control && toggle)
        {
            listbox->items[row].selected = !listbox->items[row].selected;
            changed = true;
            break;
        }
        changed = tb_gui_listbox_select_range(listbox, row, row);
        break;
    case TB_LISTBOX_BROWSE:
    case TB_LISTBOX_SINGLE:
        listbox->anchor = row;
        changed = tb_gui_listbox_select_range(listbox, row, row);
        break;
    }
    if (changed)
    {
        (void)tb_widget_invoke(widget);
    }
}

bool tb_listbox_select(tb_widget listbox, size_t index, bool selected)
{
    tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    if (data == NULL || index >= data->count)
    {
        return false;
    }
    if (selected && !tb_gui_listbox_many(data))
    {
        (void)tb_gui_listbox_select_range(data, index, index);
    }
    data->items[index].selected = selected;
    return true;
}

void tb_listbox_clear_selection(tb_widget listbox)
{
    tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    for (size_t index = 0; data != NULL && index < data->count; index++)
    {
        data->items[index].selected = false;
    }
}

bool tb_listbox_selected(tb_widget listbox, size_t index)
{
    const tb_gui_listbox *const data = tb_gui_listbox_of(listbox);
    return data != NULL && index < data->count && data->items[index].selected;
}

size_t tb_listbox_selection(tb_widget listbox, size_t *indices, size_t capacity)
{
    const tb_gui_listbox *const data = tb_gui_listbox_of(listbox);
    size_t found = 0;

    TB_ASSERT(indices != NULL || capacity == 0);
    for (size_t index = 0; data != NULL && index < data->count; index++)
    {
        if (!data->items[index].selected)
        {
            continue;
        }
        if (found < capacity)
        {
            indices[found] = index;
        }
        found++;
    }
    return found;
}

size_t tb_listbox_active(tb_widget listbox)
{
    const tb_gui_listbox *const data = tb_gui_listbox_of(listbox);
    return data != NULL && data->count > 0 ? data->active : SIZE_MAX;
}

bool tb_listbox_activate(tb_widget listbox, size_t index)
{
    tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    if (data == NULL || index >= data->count)
    {
        return false;
    }
    data->active = index;
    return true;
}

/* ---------------------------------------------------------------------------
 * Rows on the screen.
 * ------------------------------------------------------------------------- */

static tb_gui_bar tb_gui_listbox_bar(tb_widget widget, const tb_gui_listbox *listbox)
{
    return (tb_gui_bar){.track = listbox->track,
                        .vertical = true,
                        .offset = listbox->offset,
                        .view = listbox->view.height,
                        .content = (float)listbox->count * tb_gui_listbox_row_height(widget)};
}

static size_t tb_gui_listbox_row_at(tb_widget widget, const tb_gui_listbox *listbox, tb_vec2 point)
{
    const float row_height = tb_gui_listbox_row_height(widget);

    if (!tb_ui_rect_contains(listbox->view, point) || !(row_height > 0.0f))
    {
        return SIZE_MAX;
    }
    const float row = floorf((point.y - listbox->view.y + listbox->offset) / row_height);
    return row >= 0.0f && row < (float)listbox->count ? (size_t)row : SIZE_MAX;
}

static tb_canvas_rect tb_gui_listbox_row_rect(tb_widget widget, const tb_gui_listbox *listbox,
                                              size_t row)
{
    const float row_height = tb_gui_listbox_row_height(widget);
    return (tb_canvas_rect){listbox->view.x,
                            listbox->view.y + (float)row * row_height - listbox->offset,
                            listbox->view.width, row_height};
}

static void tb_gui_listbox_scroll(tb_widget widget, tb_gui_listbox *listbox, float offset)
{
    const tb_gui_bar bar = tb_gui_listbox_bar(widget, listbox);
    listbox->offset = tb_gui_bar_clamp(&bar, offset);
}

bool tb_listbox_see(tb_widget listbox, size_t index)
{
    tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    if (data == NULL || index >= data->count)
    {
        return false;
    }
    tb_gui_layout(listbox.gui);
    const float row_height = tb_gui_listbox_row_height(listbox);
    const float top = (float)index * row_height;
    if (top < data->offset)
    {
        tb_gui_listbox_scroll(listbox, data, top);
    }
    else if (top + row_height > data->offset + data->view.height)
    {
        tb_gui_listbox_scroll(listbox, data, top + row_height - data->view.height);
    }
    /* A list as tall as its rows is scrolled by a scrollframe around it. */
    tb_gui_see_rect(listbox.gui, listbox.index, tb_gui_listbox_row_rect(listbox, data, index));
    return true;
}

size_t tb_listbox_nearest(tb_widget listbox, float y)
{
    const tb_gui_listbox *const data = tb_gui_listbox_of(listbox);

    if (data == NULL || data->count == 0)
    {
        return SIZE_MAX;
    }
    const float row_height = tb_gui_listbox_row_height(listbox);
    const float row = floorf((y - data->view.y + data->offset) / row_height);
    if (!(row > 0.0f))
    {
        return 0;
    }
    return row < (float)data->count ? (size_t)row : data->count - 1;
}

/* ---------------------------------------------------------------------------
 * The kind's functions.
 * ------------------------------------------------------------------------- */

static tb_vec2 tb_gui_listbox_measure(tb_widget widget, const void *data)
{
    const tb_gui_listbox *const listbox = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const float width =
        listbox->width > 0.0f ? listbox->width : theme->font_size * TB_GUI_LISTBOX_EMS;
    const size_t rows = tb_gui_listbox_rows_shown(listbox);
    const float bar = listbox->count > rows ? theme->scrollbar_width : 0.0f;

    return (tb_vec2){width + bar + 2.0f * theme->border,
                     (float)rows * tb_gui_listbox_row_height(widget) + 2.0f * theme->border};
}

static void tb_gui_listbox_arrange(tb_widget widget, void *data, tb_canvas_rect rect)
{
    tb_gui_listbox *const listbox = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect inner = tb_ui_rect_inset(rect, theme->border, theme->border);
    const float content = (float)listbox->count * tb_gui_listbox_row_height(widget);
    const float bar = theme->scrollbar_width;

    listbox->bar = content > inner.height;
    listbox->view = inner;
    if (listbox->bar)
    {
        listbox->view.width = fmaxf(inner.width - bar, 0.0f);
    }
    listbox->track = (tb_canvas_rect){inner.x + listbox->view.width, inner.y, bar, inner.height};
    tb_gui_listbox_scroll(widget, listbox, listbox->offset);
}

static void tb_gui_listbox_draw_row(tb_widget widget, const tb_gui_listbox *listbox,
                                    tb_gui_paint *rows, size_t row, uint32_t state)
{
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const tb_canvas_rect rect = tb_gui_listbox_row_rect(widget, listbox, row);
    const tb_gui_text_block block = {.text = tb_gui_string_text(&listbox->items[row].text),
                                     .anchor = TB_GUI_ANCHOR_W};
    const bool disabled = (state & TB_WIDGET_DISABLED) != 0;

    if (listbox->items[row].selected)
    {
        tb_gui_paint_fill(rows, rect, 0.0f, theme->colors[TB_UI_COLOR_SELECTION]);
    }
    else if (row == listbox->hot_row)
    {
        tb_gui_paint_fill(rows, rect, 0.0f, theme->colors[TB_UI_COLOR_HEADER_HOT]);
    }
    if (row == listbox->active && (state & TB_WIDGET_FOCUSED) != 0)
    {
        tb_gui_paint_outline(rows, rect, 0.0f, theme->border, theme->colors[TB_UI_COLOR_ACCENT]);
    }
    tb_gui_paint_text(rows, &block, tb_ui_rect_inset(rect, theme->frame_padding.x, 0.0f),
                      theme->colors[disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT]);
}

static void tb_gui_listbox_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_listbox *const listbox = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);
    const uint32_t state = tb_widget_state(widget);
    const float row_height = tb_gui_listbox_row_height(widget);
    /* Rows are drawn within the view, and only those that can be seen. */
    tb_gui_paint rows = *paint;
    rows.clip = tb_ui_rect_intersect(paint->clip, listbox->view);

    tb_gui_paint_fill(paint, paint->rect, 0.0f, theme->colors[TB_UI_COLOR_FRAME]);
    if (!tb_ui_rect_empty(rows.clip) && row_height > 0.0f)
    {
        const float top = rows.clip.y - listbox->view.y + listbox->offset;
        const float first = fmaxf(floorf(top / row_height), 0.0f);
        const float last =
            fminf(ceilf((top + rows.clip.height) / row_height), (float)listbox->count);
        for (size_t row = (size_t)first; (float)row < last; row++)
        {
            tb_gui_listbox_draw_row(widget, listbox, &rows, row, state);
        }
    }
    if (listbox->bar)
    {
        const tb_gui_bar bar = tb_gui_listbox_bar(widget, listbox);
        tb_gui_bar_draw(paint, &bar, listbox->dragging_bar);
    }
    tb_gui_paint_outline(
        paint, paint->rect, 0.0f, theme->border,
        theme->colors[(state & TB_WIDGET_FOCUSED) != 0 ? TB_UI_COLOR_ACCENT : TB_UI_COLOR_BORDER]);
}

static bool tb_gui_listbox_press(tb_widget widget, tb_gui_listbox *listbox,
                                 const tb_gui_event *event)
{
    const tb_gui_bar bar = tb_gui_listbox_bar(widget, listbox);
    const tb_gui_bar_part part =
        listbox->bar ? tb_gui_bar_part_at(&bar, event->position) : TB_GUI_BAR_NONE;

    if (event->button != TB_MOUSE_BUTTON_LEFT)
    {
        return false;
    }
    if (part == TB_GUI_BAR_THUMB)
    {
        listbox->dragging_bar = true;
        listbox->grab = tb_gui_bar_grab(&bar, event->position);
        return true;
    }
    if (part != TB_GUI_BAR_NONE)
    {
        tb_gui_listbox_scroll(widget, listbox, tb_gui_bar_page(&bar, part));
        return true;
    }
    const size_t row = tb_gui_listbox_row_at(widget, listbox, event->position);
    if (row != SIZE_MAX)
    {
        listbox->selecting = true;
        tb_gui_listbox_choose(widget, listbox, row, event->modifiers, true);
    }
    return true;
}

static void tb_gui_listbox_motion(tb_widget widget, tb_gui_listbox *listbox,
                                  const tb_gui_event *event)
{
    const tb_gui_bar bar = tb_gui_listbox_bar(widget, listbox);

    if (listbox->dragging_bar)
    {
        tb_gui_listbox_scroll(widget, listbox,
                              tb_gui_bar_drag(&bar, event->position, listbox->grab));
        return;
    }
    listbox->hot_row = tb_gui_listbox_row_at(widget, listbox, event->position);
    /* Dragged over rows, a browse list follows the mouse, and an extended
     * one selects from its anchor. */
    if (listbox->selecting && listbox->hot_row != SIZE_MAX && listbox->hot_row != listbox->active &&
        (listbox->selectmode == TB_LISTBOX_BROWSE || listbox->selectmode == TB_LISTBOX_EXTENDED))
    {
        const uint32_t modifiers =
            listbox->selectmode == TB_LISTBOX_EXTENDED ? TB_MODIFIER_SHIFT : 0u;
        tb_gui_listbox_choose(widget, listbox, listbox->hot_row, modifiers, false);
    }
}

/* The row a key moves the active row to, or SIZE_MAX for a key that does not. */
static size_t tb_gui_listbox_key_row(tb_widget widget, const tb_gui_listbox *listbox, tb_key key)
{
    const float row_height = tb_gui_listbox_row_height(widget);
    const size_t page =
        row_height > 0.0f ? (size_t)fmaxf(floorf(listbox->view.height / row_height), 1.0f) : 1u;
    const size_t last = listbox->count - 1;

    if (key == TB_KEY_UP)
    {
        return listbox->active > 0 ? listbox->active - 1 : 0;
    }
    if (key == TB_KEY_DOWN)
    {
        return listbox->active < last ? listbox->active + 1 : last;
    }
    if (key == TB_KEY_PAGE_UP)
    {
        return listbox->active > page ? listbox->active - page : 0;
    }
    if (key == TB_KEY_PAGE_DOWN)
    {
        return last - listbox->active > page ? listbox->active + page : last;
    }
    if (key == TB_KEY_HOME)
    {
        return 0;
    }
    if (key == TB_KEY_END)
    {
        return last;
    }
    return SIZE_MAX;
}

static bool tb_gui_listbox_key(tb_widget widget, tb_gui_listbox *listbox, const tb_gui_event *event)
{
    const bool control = (event->modifiers & TB_MODIFIER_CONTROL) != 0;

    if (listbox->count == 0)
    {
        return false;
    }
    if (event->key == TB_KEY_A && control && tb_gui_listbox_many(listbox))
    {
        if (tb_gui_listbox_select_range(listbox, 0, listbox->count - 1))
        {
            (void)tb_widget_invoke(widget);
        }
        return true;
    }
    if (event->key == TB_KEY_SPACE || event->key == TB_KEY_ENTER)
    {
        tb_gui_listbox_choose(widget, listbox, listbox->active,
                              event->modifiers | TB_MODIFIER_CONTROL, true);
        return true;
    }
    const size_t row = tb_gui_listbox_key_row(widget, listbox, event->key);
    if (row == SIZE_MAX)
    {
        return false;
    }
    /* A list of many moves only its active row, unless Shift extends it. */
    if (listbox->selectmode == TB_LISTBOX_MULTIPLE ||
        (listbox->selectmode == TB_LISTBOX_EXTENDED && control))
    {
        listbox->active = row;
    }
    else
    {
        tb_gui_listbox_choose(widget, listbox, row, event->modifiers, false);
    }
    (void)tb_listbox_see(widget, row);
    return true;
}

static bool tb_gui_listbox_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_listbox *const listbox = data;
    const float row_height = tb_gui_listbox_row_height(widget);

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS:
        return tb_gui_listbox_press(widget, listbox, event);
    case TB_GUI_EVENT_MOTION:
        tb_gui_listbox_motion(widget, listbox, event);
        return true;
    case TB_GUI_EVENT_RELEASE:
        listbox->selecting = false;
        listbox->dragging_bar = false;
        return true;
    case TB_GUI_EVENT_LEAVE:
        listbox->hot_row = SIZE_MAX;
        return false;
    case TB_GUI_EVENT_WHEEL: {
        const float before = listbox->offset;
        tb_gui_listbox_scroll(widget, listbox,
                              listbox->offset -
                                  event->wheel.y * row_height * TB_GUI_LISTBOX_WHEEL_ROWS);
        return listbox->offset != before;
    }
    case TB_GUI_EVENT_KEY:
        return tb_gui_listbox_key(widget, listbox, event);
    case TB_GUI_EVENT_COMMAND:
        if (listbox->command.callback != NULL)
        {
            listbox->command.callback(event, listbox->command.user);
        }
        return true;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
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

/* ---------------------------------------------------------------------------
 * Properties.
 * ------------------------------------------------------------------------- */

/* The items, a line each. */
static bool tb_gui_listbox_get_items(tb_widget widget, const void *data, tb_gui_value *value)
{
    tb_gui_listbox *const listbox = tb_widget_data(widget);
    size_t length = 1;
    bool fits = true;

    (void)data;
    if (listbox->joined.bytes == NULL)
    {
        /* Each line and its break, and the terminator. */
        for (size_t index = 0; index < listbox->count && fits; index++)
        {
            fits = tb_size_add(length, listbox->items[index].text.length, &length) &&
                   tb_size_add(length, 1, &length);
        }
        TB_ASSERT_MSG(fits, "a list's items are larger than memory");
        char *const bytes = tb_malloc(length);
        size_t at = 0;
        for (size_t index = 0; index < listbox->count; index++)
        {
            const tb_gui_string *const text = &listbox->items[index].text;
            if (text->length > 0)
            {
                memcpy(bytes + at, text->bytes, text->length);
            }
            at += text->length;
            bytes[at] = '\n';
            at++;
        }
        /* The last line's break is not kept. */
        at = at > 0 ? at - 1 : 0;
        bytes[at] = '\0';
        listbox->joined = (tb_gui_string){.bytes = bytes, .length = at};
    }
    *value = tb_gui_text(tb_gui_string_text(&listbox->joined));
    return true;
}

static bool tb_gui_listbox_set_items(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_listbox *const listbox = data;
    const tb_text text = value->text;
    size_t start = 0;

    tb_gui_listbox_clear(listbox);
    for (size_t at = 0; at <= text.length && text.length > 0; at++)
    {
        if (at == text.length || text.bytes[at] == '\n')
        {
            tb_gui_listbox_insert_text(listbox, listbox->count,
                                       tb_text_make(text.bytes + start, at - start));
            start = at + 1;
        }
    }
    tb_gui_listbox_changed(widget, listbox);
    return true;
}

static bool tb_gui_listbox_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_listbox *const listbox = data;

    (void)widget;
    if (value->number < 0.0f)
    {
        return false;
    }
    listbox->width = value->number;
    return true;
}

static bool tb_gui_listbox_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_listbox *const listbox = data;

    (void)widget;
    *value = tb_gui_float(listbox->width);
    return true;
}

static bool tb_gui_listbox_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_listbox *const listbox = data;

    (void)widget;
    if (value->integer < 0)
    {
        return false;
    }
    listbox->height = value->integer;
    return true;
}

static bool tb_gui_listbox_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_listbox *const listbox = data;

    (void)widget;
    *value = tb_gui_int(listbox->height);
    return true;
}

static bool tb_gui_listbox_set_offset(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_listbox *const listbox = data;

    if (value->number < 0.0f)
    {
        return false;
    }
    tb_gui_listbox_scroll(widget, listbox, value->number);
    return true;
}

static bool tb_gui_listbox_get_offset(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_listbox *const listbox = data;

    (void)widget;
    *value = tb_gui_float(listbox->offset);
    return true;
}

static const tb_gui_property tb_gui_listbox_properties[] = {
    {.name = "items",
     .type = TB_GUI_TYPE_TEXT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_listbox_get_items,
     .set = tb_gui_listbox_set_items},
    {.name = "selectmode",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_listbox, selectmode),
     .names = tb_gui_selectmode_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_selectmode_names)},
    {.name = "width",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_listbox_get_width,
     .set = tb_gui_listbox_set_width},
    {.name = "height",
     .type = TB_GUI_TYPE_INT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_listbox_get_height,
     .set = tb_gui_listbox_set_height},
    {.name = "offset",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_listbox_get_offset,
     .set = tb_gui_listbox_set_offset},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_listbox, command)},
};

static const tb_widget_class tb_gui_listbox_kind = {
    .name = "listbox",
    .data_size = sizeof(tb_gui_listbox),
    .properties = tb_gui_listbox_properties,
    .property_count = TB_GUI_COUNT_OF(tb_gui_listbox_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_listbox_init,
    .destroy = tb_gui_listbox_destroy,
    .measure = tb_gui_listbox_measure,
    .arrange = tb_gui_listbox_arrange,
    .draw = tb_gui_listbox_draw,
    .event = tb_gui_listbox_event};

static tb_gui_listbox *tb_gui_listbox_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_listbox_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_listbox_class(void)
{
    return &tb_gui_listbox_kind;
}

tb_widget tb_listbox(tb_widget parent, const tb_listbox_desc *desc)
{
    const tb_listbox_desc chosen = desc != NULL ? *desc : (tb_listbox_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_listbox_kind);
    const tb_gui_value values[] = {tb_gui_enum((int32_t)chosen.selectmode),
                                   tb_gui_float(chosen.width), tb_gui_int(chosen.height),
                                   tb_gui_command_of(chosen.command, chosen.user)};
    static const char *const names[] = {"selectmode", "width", "height", "command"};

    TB_ASSERT(chosen.items != NULL || chosen.item_count == 0);
    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    for (size_t index = 0; index < chosen.item_count; index++)
    {
        (void)tb_listbox_insert(widget, SIZE_MAX, chosen.items[index]);
    }
    return widget;
}

void tb_gui_register_listbox(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_listbox_kind);
}
