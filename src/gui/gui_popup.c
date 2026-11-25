/* Popups: widgets shown in front of every other, at their natural size, by
 * a rectangle they open beside, kept inside the GUI and clipped by nothing
 * but it. They stack, the last shown in front; a press outside them, or
 * Escape, closes them (gui_input.c). */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>

/* Popups a GUI first holds. */
#define TB_GUI_FIRST_POPUPS 4u

static size_t tb_gui_popup_index(const tb_gui *gui, uint32_t index)
{
    for (size_t level = 0; level < gui->popup_count; level++)
    {
        if (gui->popups[level].widget.index == index)
        {
            return level;
        }
    }
    return SIZE_MAX;
}

size_t tb_gui_popup_level(tb_gui *gui, uint32_t index, bool passive)
{
    /* The nearest popup up the tree is the one it is shown in. */
    for (uint32_t at = index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        const size_t level = tb_gui_popup_index(gui, at);
        if (level != SIZE_MAX)
        {
            return passive || !gui->popups[level].options.passive ? level : SIZE_MAX;
        }
    }
    return SIZE_MAX;
}

tb_widget tb_gui_active_popup(const tb_gui *gui)
{
    for (size_t level = gui->popup_count; level > 0; level--)
    {
        if (!gui->popups[level - 1].options.passive)
        {
            return gui->popups[level - 1].widget;
        }
    }
    return TB_WIDGET_NONE;
}

void tb_gui_forget_popup(tb_gui *gui, uint32_t index)
{
    const size_t level = tb_gui_popup_index(gui, index);

    if (level == SIZE_MAX)
    {
        return;
    }
    tb_gui_node_at(gui, index)->popup = false;
    for (size_t at = level + 1; at < gui->popup_count; at++)
    {
        gui->popups[at - 1] = gui->popups[at];
    }
    gui->popup_count--;
    tb_gui_mark_layout(gui);
}

void tb_gui_close_popups_from(tb_gui *gui, size_t level, bool passive)
{
    /* Each closed is sent its event, whose callbacks may close or open
     * others: the stack is read again after each. */
    size_t at = gui->popup_count;

    while (at > level)
    {
        at--;
        if (at >= gui->popup_count)
        {
            continue;
        }
        const tb_gui_popup_entry entry = gui->popups[at];
        if (entry.options.passive && !passive)
        {
            continue;
        }
        tb_gui_forget_popup(gui, entry.widget.index);
        tb_gui_send(gui, TB_GUI_EVENT_POPUP_CLOSE, entry.widget);
    }
}

bool tb_widget_popup(tb_widget widget, const tb_gui_popup_options *options)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    const tb_gui_popup_options chosen = options != NULL ? *options : (tb_gui_popup_options){0};
    tb_gui *const gui = widget.gui;

    if (node == NULL || node->parent == TB_GUI_NO_NODE)
    {
        return false;
    }
    const size_t level = tb_gui_popup_index(gui, widget.index);
    if (level != SIZE_MAX)
    {
        gui->popups[level].options = chosen;
        tb_gui_mark_layout(gui);
        return true;
    }
    if (gui->popup_count == gui->popup_capacity)
    {
        gui->popup_capacity =
            gui->popup_capacity == 0 ? TB_GUI_FIRST_POPUPS : gui->popup_capacity * 2;
        gui->popups = tb_realloc_array(gui->popups, gui->popup_capacity, sizeof(*gui->popups));
    }
    gui->popups[gui->popup_count] = (tb_gui_popup_entry){.widget = widget, .options = chosen};
    gui->popup_count++;
    node->popup = true;
    tb_gui_mark_layout(gui);
    return true;
}

void tb_widget_close_popup(tb_widget widget)
{
    if (!tb_widget_alive(widget))
    {
        return;
    }
    const size_t level = tb_gui_popup_index(widget.gui, widget.index);
    if (level != SIZE_MAX)
    {
        tb_gui_close_popups_from(widget.gui, level, true);
    }
}

bool tb_widget_is_popup(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL && node->popup;
}

void tb_gui_close_popups(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    tb_gui_close_popups_from(gui, 0, false);
}

/* ---------------------------------------------------------------------------
 * Placing.
 * ------------------------------------------------------------------------- */

/* A start along an axis that keeps a length inside `room`: as asked, or
 * flipped to the anchor's other side, or pushed back in. */
static float tb_gui_place_along(float wanted, float flipped, float length, float room)
{
    if (wanted + length <= room)
    {
        return wanted;
    }
    if (flipped >= 0.0f)
    {
        return flipped;
    }
    return fmaxf(room - length, 0.0f);
}

static tb_canvas_rect tb_gui_place_popup(const tb_gui *gui, const tb_gui_popup_options *options,
                                         tb_vec2 natural)
{
    const tb_vec2 room = gui->view.size;
    const tb_canvas_rect anchor = options->anchor;
    const float width = fminf(fmaxf(natural.x, options->min_size.x), room.x);
    const float height = fminf(fmaxf(natural.y, options->min_size.y), room.y);
    float x = 0.0f;
    float y = 0.0f;

    switch (options->placement)
    {
    case TB_GUI_POPUP_RIGHT:
        x = tb_gui_place_along(anchor.x + anchor.width, anchor.x - width, width, room.x);
        y = tb_gui_place_along(anchor.y, -1.0f, height, room.y);
        break;
    case TB_GUI_POPUP_AT:
        x = tb_gui_place_along(anchor.x, anchor.x - width, width, room.x);
        y = tb_gui_place_along(anchor.y, anchor.y - height, height, room.y);
        break;
    case TB_GUI_POPUP_BELOW:
        x = tb_gui_place_along(anchor.x, -1.0f, width, room.x);
        y = tb_gui_place_along(anchor.y + anchor.height, anchor.y - height, height, room.y);
        break;
    }
    return tb_ui_snap_rect(gui->view.scale,
                           (tb_canvas_rect){fmaxf(x, 0.0f), fmaxf(y, 0.0f), width, height});
}

void tb_gui_layout_popups(tb_gui *gui)
{
    for (size_t level = 0; level < gui->popup_count; level++)
    {
        const tb_gui_popup_entry entry = gui->popups[level];
        const tb_gui_node *const node = tb_gui_node_of(entry.widget);
        if (node == NULL || !node->visible)
        {
            continue;
        }
        const tb_vec2 natural = tb_gui_measure(gui, entry.widget.index);
        tb_gui_arrange(gui, entry.widget.index, tb_gui_place_popup(gui, &entry.options, natural));
    }
}
