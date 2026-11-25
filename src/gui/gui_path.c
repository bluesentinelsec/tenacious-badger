/* Widgets found by path: the names of children, joined by `/`, from a
 * widget down; and the path of a widget from its GUI's root. */
#include "gui_internal.h"

#include "badger/assert.h"

#include <string.h>

/* The first child of a widget of a name; none when it has none. */
static tb_widget tb_gui_child_named(tb_widget parent, tb_text name)
{
    const tb_gui_node *const node = tb_gui_node_of(parent);

    for (uint32_t child = node->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(parent.gui, child)->next)
    {
        const tb_gui_node *const found = tb_gui_node_at(parent.gui, child);
        if (tb_text_equal(tb_gui_string_text(&found->name), name))
        {
            return tb_gui_handle(parent.gui, child);
        }
    }
    return TB_WIDGET_NONE;
}

tb_widget tb_widget_find(tb_widget from, const char *path)
{
    const tb_text whole = tb_text_from_cstr(path);
    tb_widget found = from;
    size_t start = 0;

    TB_ASSERT(path != NULL);
    if (!tb_widget_alive(from))
    {
        return TB_WIDGET_NONE;
    }
    if (whole.length == 0)
    {
        return from;
    }
    while (start <= whole.length)
    {
        size_t end = tb_text_find_char(whole, start, TB_TEXT_END, '/');
        if (end == TB_TEXT_NOT_FOUND)
        {
            end = whole.length;
        }
        if (end == start)
        {
            return TB_WIDGET_NONE;
        }
        found = tb_gui_child_named(found, tb_text_sub(whole, start, end));
        if (!tb_widget_alive(found))
        {
            return TB_WIDGET_NONE;
        }
        start = end + 1;
    }
    return found;
}

bool tb_widget_path(tb_widget widget, char *buffer, size_t capacity, size_t *length)
{
    const tb_gui_node *node = tb_gui_node_of(widget);
    size_t needed = 0;
    bool nameable = node != NULL;

    TB_ASSERT(buffer != NULL || capacity == 0);
    /* Measure: each name below the root, and a slash between each two. */
    while (nameable && node->parent != TB_GUI_NO_NODE)
    {
        const tb_text name = tb_gui_string_text(&node->name);
        if (name.length == 0 || tb_text_find_char(name, 0, TB_TEXT_END, '/') != TB_TEXT_NOT_FOUND)
        {
            nameable = false;
            break;
        }
        needed += name.length + (needed > 0 ? 1u : 0u);
        node = tb_gui_node_at(widget.gui, node->parent);
    }
    if (length != NULL)
    {
        *length = nameable ? needed : 0;
    }
    if (!nameable || needed >= capacity)
    {
        return false;
    }
    /* Write from the end: the widget's name last, its ancestors' before. */
    buffer[needed] = '\0';
    size_t end = needed;
    node = tb_gui_node_of(widget);
    while (node->parent != TB_GUI_NO_NODE)
    {
        const tb_text name = tb_gui_string_text(&node->name);
        end -= name.length;
        memcpy(buffer + end, name.bytes, name.length);
        node = tb_gui_node_at(widget.gui, node->parent);
        if (end > 0)
        {
            end--;
            buffer[end] = '/';
        }
    }
    return true;
}
