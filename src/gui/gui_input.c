/* Input: window events queued and handled in order, the widget under the
 * mouse and the one pressed, the keyboard's focus, modal widgets, and the
 * bindings each event calls. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

/* Seconds within which a second press is a double click, and units it may
 * be from the first, as most systems have them. */
#define TB_GUI_DOUBLE_CLICK_SECONDS 0.5
#define TB_GUI_DOUBLE_CLICK_DISTANCE 4.0f

/* Events queued a GUI first holds. */
#define TB_GUI_FIRST_QUEUE 32u

/* ---------------------------------------------------------------------------
 * Feeding.
 * ------------------------------------------------------------------------- */

static bool tb_gui_takes_event(tb_window_event_type type)
{
    switch (type)
    {
    case TB_WINDOW_EVENT_MOUSE_MOVED:
    case TB_WINDOW_EVENT_MOUSE_ENTERED:
    case TB_WINDOW_EVENT_MOUSE_LEFT:
    case TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
    case TB_WINDOW_EVENT_MOUSE_BUTTON_UP:
    case TB_WINDOW_EVENT_MOUSE_WHEEL:
    case TB_WINDOW_EVENT_KEY_DOWN:
    case TB_WINDOW_EVENT_KEY_UP:
    case TB_WINDOW_EVENT_TEXT_INPUT:
    case TB_WINDOW_EVENT_FOCUS_LOST:
        return true;
    case TB_WINDOW_EVENT_CLOSE_REQUESTED:
    case TB_WINDOW_EVENT_RESIZED:
    case TB_WINDOW_EVENT_SCALE_CHANGED:
    case TB_WINDOW_EVENT_FOCUS_GAINED:
    case TB_WINDOW_EVENT_MINIMIZED:
    case TB_WINDOW_EVENT_RESTORED:
    case TB_WINDOW_EVENT_DISPLAYS_CHANGED:
    case TB_WINDOW_EVENT_WAKE:
    case TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION:
        break;
    }
    return false;
}

void tb_gui_feed_event(tb_gui *gui, const tb_window_event *event)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(event != NULL);
    if (!tb_gui_takes_event(event->type))
    {
        return;
    }
    if (gui->queue_count == gui->queue_capacity)
    {
        gui->queue_capacity =
            gui->queue_capacity == 0 ? TB_GUI_FIRST_QUEUE : gui->queue_capacity * 2;
        gui->queue = tb_realloc_array(gui->queue, gui->queue_capacity, sizeof(*gui->queue));
    }
    gui->queue[gui->queue_count] = *event;
    gui->queue_count++;
}

void tb_gui_feed_events(tb_gui *gui, const tb_window_event *events, size_t count)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(events != NULL || count == 0);
    for (size_t index = 0; index < count; index++)
    {
        tb_gui_feed_event(gui, &events[index]);
    }
}

/* ---------------------------------------------------------------------------
 * Giving events.
 * ------------------------------------------------------------------------- */

static bool tb_gui_enabled_chain(tb_gui *gui, uint32_t index)
{
    for (uint32_t at = index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        if (!tb_gui_node_at(gui, at)->enabled)
        {
            return false;
        }
    }
    return true;
}

/* Events a disabled widget is still given: those that end a state. */
static bool tb_gui_reaches_disabled(tb_gui_event_kind kind)
{
    return kind == TB_GUI_EVENT_LEAVE || kind == TB_GUI_EVENT_FOCUS_OUT ||
           kind == TB_GUI_EVENT_RESIZE || kind == TB_GUI_EVENT_POPUP_CLOSE;
}

static tb_gui_event tb_gui_event_of(const tb_gui *gui, tb_gui_event_kind kind, tb_widget target)
{
    return (tb_gui_event){.kind = kind,
                          .widget = target,
                          .target = target,
                          .position = gui->mouse,
                          .modifiers = gui->modifiers};
}

/* Drop the bindings removed while events were given. */
static void tb_gui_compact_bindings(tb_gui *gui)
{
    for (size_t index = 0; index < gui->node_count; index++)
    {
        tb_gui_node *const node = &gui->nodes[index];
        size_t kept = 0;
        if (!node->bindings_cleared)
        {
            continue;
        }
        for (size_t binding = 0; binding < node->binding_count; binding++)
        {
            if (node->bindings[binding].callback != NULL)
            {
                node->bindings[kept] = node->bindings[binding];
                kept++;
            }
        }
        node->binding_count = kept;
        node->bindings_cleared = false;
    }
}

/* Give an event to a widget: its bindings, then its kind. Returns whether
 * the kind used it. */
static bool tb_gui_deliver(tb_gui *gui, tb_widget widget, tb_gui_event *event)
{
    tb_gui_node *node = tb_gui_node_of(widget);
    bool used = false;

    if (node == NULL)
    {
        return false;
    }
    if (!tb_gui_reaches_disabled(event->kind) && !tb_gui_enabled_chain(gui, widget.index))
    {
        return false;
    }
    event->widget = widget;
    event->local = (tb_vec2){event->position.x - node->rect.x, event->position.y - node->rect.y};
    gui->dispatch_depth++;
    const size_t count = node->binding_count;
    for (size_t index = 0; index < count; index++)
    {
        node = tb_gui_node_of(widget);
        if (node == NULL || index >= node->binding_count)
        {
            break;
        }
        const tb_gui_binding binding = node->bindings[index];
        if (binding.kind == event->kind && binding.callback != NULL)
        {
            binding.callback(event, binding.user);
        }
    }
    node = tb_gui_node_of(widget);
    if (node != NULL && node->kind->event != NULL)
    {
        used = node->kind->event(widget, node->data, event);
    }
    gui->dispatch_depth--;
    if (gui->dispatch_depth == 0)
    {
        tb_gui_flush_deferred(gui);
        tb_gui_compact_bindings(gui);
    }
    return used;
}

/* Give an event to a widget, then to each ancestor until a kind uses it.
 * Returns whether one did. */
static bool tb_gui_bubble(tb_gui *gui, tb_widget target, tb_gui_event *event)
{
    tb_widget widget = target;

    while (tb_widget_alive(widget))
    {
        const tb_widget parent = tb_widget_parent(widget);
        if (tb_gui_deliver(gui, widget, event))
        {
            return true;
        }
        widget = parent;
    }
    return false;
}

static void tb_gui_send_kind(tb_gui *gui, tb_gui_event_kind kind, tb_widget target)
{
    tb_gui_event event = tb_gui_event_of(gui, kind, target);
    (void)tb_gui_deliver(gui, target, &event);
}

tb_gui_event tb_gui_event_for(tb_widget widget, tb_gui_event_kind kind)
{
    return tb_gui_event_of(widget.gui, kind, widget);
}

bool tb_gui_send(tb_widget widget, tb_gui_event *event)
{
    if (!tb_widget_alive(widget))
    {
        return false;
    }
    return tb_gui_deliver(widget.gui, widget, event);
}

bool tb_bind(tb_widget widget, tb_gui_event_kind kind, tb_gui_callback *callback, void *user)
{
    tb_gui_node *const node = tb_gui_node_of(widget);

    TB_ASSERT(callback != NULL);
    TB_ASSERT((size_t)kind < TB_GUI_EVENT_COUNT);
    if (node == NULL)
    {
        return false;
    }
    if (node->binding_count == node->binding_capacity)
    {
        node->binding_capacity = node->binding_capacity == 0 ? 4u : node->binding_capacity * 2;
        node->bindings =
            tb_realloc_array(node->bindings, node->binding_capacity, sizeof(*node->bindings));
    }
    node->bindings[node->binding_count] =
        (tb_gui_binding){.kind = kind, .callback = callback, .user = user};
    node->binding_count++;
    return true;
}

size_t tb_unbind(tb_widget widget, tb_gui_event_kind kind, tb_gui_callback *callback, void *user)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    size_t removed = 0;

    if (node == NULL)
    {
        return 0;
    }
    for (size_t index = 0; index < node->binding_count; index++)
    {
        tb_gui_binding *const binding = &node->bindings[index];
        const bool matches =
            callback == NULL || (binding->callback == callback && binding->user == user);
        if (binding->kind == kind && binding->callback != NULL && matches)
        {
            binding->callback = NULL;
            removed++;
        }
    }
    if (removed > 0)
    {
        node->bindings_cleared = true;
        if (widget.gui->dispatch_depth == 0)
        {
            tb_gui_compact_bindings(widget.gui);
        }
    }
    return removed;
}

bool tb_widget_invoke(tb_widget widget)
{
    tb_gui_event event = {0};

    if (!tb_widget_alive(widget) || !tb_gui_enabled_chain(widget.gui, widget.index))
    {
        return false;
    }
    event = tb_gui_event_of(widget.gui, TB_GUI_EVENT_COMMAND, widget);
    (void)tb_gui_deliver(widget.gui, widget, &event);
    return true;
}

void tb_gui_dispatch_resized(tb_gui *gui)
{
    for (size_t index = 0; index < gui->resized_count; index++)
    {
        const tb_gui_node *const node = tb_gui_node_at(gui, gui->resized[index]);
        if (!node->alive)
        {
            continue;
        }
        const tb_widget widget = tb_gui_handle(gui, gui->resized[index]);
        tb_gui_event event = tb_gui_event_of(gui, TB_GUI_EVENT_RESIZE, widget);
        event.rect = node->rect;
        (void)tb_gui_deliver(gui, widget, &event);
    }
    gui->resized_count = 0;
}

/* ---------------------------------------------------------------------------
 * Modal widgets.
 * ------------------------------------------------------------------------- */

bool tb_gui_modal_contains(const tb_gui *gui, uint32_t index)
{
    for (size_t at = 0; at < gui->modal_count; at++)
    {
        if (gui->modals[at].index == index)
        {
            return true;
        }
    }
    return false;
}

static void tb_gui_remove_modal(tb_gui *gui, uint32_t index)
{
    size_t kept = 0;

    for (size_t at = 0; at < gui->modal_count; at++)
    {
        if (gui->modals[at].index != index)
        {
            gui->modals[kept] = gui->modals[at];
            kept++;
        }
    }
    gui->modal_count = kept;
}

static tb_widget tb_gui_top_modal(const tb_gui *gui)
{
    return gui->modal_count > 0 ? gui->modals[gui->modal_count - 1] : TB_WIDGET_NONE;
}

/* Whether a widget may take input with the modal widgets shown: it is in
 * the one in front, or in a popup, which is in front of them all. */
static bool tb_gui_reachable(tb_gui *gui, tb_widget widget)
{
    const tb_widget modal = tb_gui_top_modal(gui);
    return modal.gui == NULL || tb_gui_node_inside(gui, widget.index, modal.index) ||
           tb_gui_popup_level(gui, widget.index, false) != SIZE_MAX;
}

static void tb_gui_set_focus(tb_gui *gui, tb_widget target)
{
    const tb_widget old = gui->focus;

    if (tb_widget_equal(old, target))
    {
        return;
    }
    gui->focus = target;
    if (tb_widget_alive(old))
    {
        tb_gui_send_kind(gui, TB_GUI_EVENT_FOCUS_OUT, old);
    }
    if (tb_widget_alive(target) && tb_widget_equal(gui->focus, target))
    {
        tb_gui_send_kind(gui, TB_GUI_EVENT_FOCUS_IN, target);
        (void)tb_widget_see(target);
    }
}

bool tb_widget_show_modal(tb_widget widget)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    tb_gui *const gui = widget.gui;

    if (node == NULL || node->parent == TB_GUI_NO_NODE)
    {
        return false;
    }
    tb_gui_remove_modal(gui, widget.index);
    if (gui->modal_count == gui->modal_capacity)
    {
        gui->modal_capacity = gui->modal_capacity == 0 ? 4u : gui->modal_capacity * 2;
        gui->modals = tb_realloc_array(gui->modals, gui->modal_capacity, sizeof(*gui->modals));
    }
    gui->modals[gui->modal_count] = widget;
    gui->modal_count++;
    if (tb_widget_alive(gui->pressed) && !tb_gui_reachable(gui, gui->pressed))
    {
        gui->pressed = TB_WIDGET_NONE;
    }
    if (tb_widget_alive(gui->focus) && !tb_gui_reachable(gui, gui->focus))
    {
        tb_gui_set_focus(gui, TB_WIDGET_NONE);
    }
    return true;
}

void tb_widget_end_modal(tb_widget widget)
{
    if (!tb_widget_alive(widget))
    {
        return;
    }
    tb_gui_remove_modal(widget.gui, widget.index);
}

void tb_gui_forget_widget(tb_gui *gui, tb_widget widget)
{
    if (tb_widget_equal(gui->hovered, widget))
    {
        gui->hovered = TB_WIDGET_NONE;
    }
    if (tb_widget_equal(gui->pressed, widget))
    {
        gui->pressed = TB_WIDGET_NONE;
    }
    if (tb_widget_equal(gui->focus, widget))
    {
        gui->focus = TB_WIDGET_NONE;
    }
    if (tb_widget_equal(gui->last_press, widget))
    {
        gui->last_press = TB_WIDGET_NONE;
    }
    if (tb_widget_equal(gui->tooltip, widget))
    {
        gui->tooltip = TB_WIDGET_NONE;
    }
    tb_gui_remove_modal(gui, widget.index);
    tb_gui_forget_popup(gui, widget.index);
}

/* ---------------------------------------------------------------------------
 * The focus.
 * ------------------------------------------------------------------------- */

static bool tb_gui_takes_focus(tb_gui *gui, tb_widget widget, uint32_t flags)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);

    return node != NULL && (node->kind->flags & flags) != 0 &&
           tb_gui_node_shown_enabled(gui, widget.index) && tb_gui_reachable(gui, widget);
}

bool tb_widget_focus(tb_widget widget)
{
    if (!tb_gui_takes_focus(widget.gui, widget,
                            TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK))
    {
        return false;
    }
    tb_gui_set_focus(widget.gui, widget);
    return true;
}

void tb_gui_clear_focus(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    tb_gui_set_focus(gui, TB_WIDGET_NONE);
}

/* The slot after one in the order of the tree, within a scope, skipping
 * the children of those whose children are not walked. */
static uint32_t tb_gui_walk_next(tb_gui *gui, uint32_t index, uint32_t scope, bool into)
{
    const tb_gui_node *const node = tb_gui_node_at(gui, index);

    if (into && node->first_child != TB_GUI_NO_NODE)
    {
        return node->first_child;
    }
    for (uint32_t at = index; at != scope; at = tb_gui_node_at(gui, at)->parent)
    {
        if (tb_gui_node_at(gui, at)->next != TB_GUI_NO_NODE)
        {
            return tb_gui_node_at(gui, at)->next;
        }
    }
    return TB_GUI_NO_NODE;
}

void tb_gui_focus_next(tb_gui *gui, bool backward)
{
    const tb_widget modal = tb_gui_top_modal(gui);
    const uint32_t scope = modal.gui != NULL ? modal.index : 0;
    uint32_t first = TB_GUI_NO_NODE;
    uint32_t last = TB_GUI_NO_NODE;
    uint32_t before = TB_GUI_NO_NODE;
    uint32_t after = TB_GUI_NO_NODE;
    bool passed = false;
    uint32_t chosen = TB_GUI_NO_NODE;

    TB_ASSERT(gui != NULL);
    for (uint32_t at = scope; at != TB_GUI_NO_NODE;)
    {
        const tb_gui_node *const node = tb_gui_node_at(gui, at);
        const bool walked =
            node->shown && (at == scope || (!tb_gui_modal_contains(gui, at) && !node->popup));
        const tb_widget widget = tb_gui_handle(gui, at);
        if (walked && tb_gui_takes_focus(gui, widget, TB_WIDGET_CLASS_FOCUSABLE))
        {
            first = first == TB_GUI_NO_NODE ? at : first;
            last = at;
            if (tb_widget_equal(widget, gui->focus))
            {
                passed = true;
            }
            else if (passed && after == TB_GUI_NO_NODE)
            {
                after = at;
            }
            else if (!passed)
            {
                before = at;
            }
        }
        at = tb_gui_walk_next(gui, at, scope, walked);
    }
    if (first == TB_GUI_NO_NODE)
    {
        return;
    }
    if (backward)
    {
        chosen = passed && before != TB_GUI_NO_NODE ? before : last;
    }
    else
    {
        chosen = passed && after != TB_GUI_NO_NODE ? after : first;
    }
    tb_gui_set_focus(gui, tb_gui_handle(gui, chosen));
}

/* ---------------------------------------------------------------------------
 * The mouse.
 * ------------------------------------------------------------------------- */

/* The deepest widget at a point in a slot's subtree, the last drawn first,
 * within a clip; none when the point is outside. */
static uint32_t tb_gui_hit(tb_gui *gui, uint32_t index, tb_vec2 point, tb_canvas_rect clip)
{
    const tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_canvas_rect inner = tb_ui_rect_intersect(clip, node->rect);

    if (!node->shown || !tb_ui_rect_contains(inner, point))
    {
        return TB_GUI_NO_NODE;
    }
    const tb_canvas_rect seen = tb_ui_rect_intersect(inner, node->clip);
    for (uint32_t child = node->last_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->previous)
    {
        if (tb_gui_modal_contains(gui, child) || tb_gui_node_at(gui, child)->popup)
        {
            continue;
        }
        const uint32_t found = tb_gui_hit(gui, child, point, seen);
        if (found != TB_GUI_NO_NODE)
        {
            return found;
        }
    }
    return node->mouse_filter == TB_GUI_MOUSE_IGNORE ? TB_GUI_NO_NODE : index;
}

/* The widget under the mouse: in a popup that takes input, the one in front
 * first; else in the modal widget in front, when one is shown, and nothing
 * outside it. */
static tb_widget tb_gui_find(tb_gui *gui)
{
    const tb_widget modal = tb_gui_top_modal(gui);
    const tb_canvas_rect all = tb_gui_node_at(gui, 0)->rect;
    uint32_t found = TB_GUI_NO_NODE;

    if (!gui->mouse_inside)
    {
        return TB_WIDGET_NONE;
    }
    for (size_t level = gui->popup_count; level > 0 && found == TB_GUI_NO_NODE; level--)
    {
        const tb_gui_popup_entry *const entry = &gui->popups[level - 1];
        if (!entry->options.passive && tb_widget_alive(entry->widget))
        {
            found = tb_gui_hit(gui, entry->widget.index, gui->mouse, all);
        }
    }
    if (found == TB_GUI_NO_NODE)
    {
        found = tb_gui_hit(gui, modal.gui != NULL ? modal.index : 0, gui->mouse, all);
    }
    return tb_gui_handle(gui, found);
}

/* Move the hover to a widget: the old one leaves, the new one enters. */
static void tb_gui_set_hover(tb_gui *gui, tb_widget target)
{
    const tb_widget old = gui->hovered;

    if (tb_widget_equal(old, target))
    {
        return;
    }
    gui->hovered = target;
    gui->hover_time = gui->time;
    gui->tooltip_spent = false;
    if (tb_widget_alive(old))
    {
        tb_gui_send_kind(gui, TB_GUI_EVENT_LEAVE, old);
    }
    if (tb_widget_alive(target) && tb_widget_equal(gui->hovered, target))
    {
        tb_gui_send_kind(gui, TB_GUI_EVENT_ENTER, target);
    }
}

void tb_gui_refresh_hover(tb_gui *gui)
{
    const tb_widget found = tb_gui_find(gui);

    /* A widget pressed holds the mouse: no other is hovered until it is
     * released, and it is hovered only while the mouse is over it. */
    if (tb_widget_alive(gui->pressed))
    {
        tb_gui_set_hover(gui, tb_widget_equal(found, gui->pressed) ? found : TB_WIDGET_NONE);
        return;
    }
    tb_gui_set_hover(gui, found);
}

static void tb_gui_move_mouse(tb_gui *gui, const tb_window_event *event)
{
    const tb_gui_view view = gui->view;

    gui->mouse = (tb_vec2){((float)event->pixel_x - view.origin.x) / view.scale,
                           ((float)event->pixel_y - view.origin.y) / view.scale};
    gui->mouse_inside = true;
    gui->modifiers = event->modifiers;
    tb_gui_refresh_hover(gui);
}

/* The mouse moved: to the widget that holds it, or else the one under it. */
static void tb_gui_send_motion(tb_gui *gui)
{
    const tb_widget target = tb_widget_alive(gui->pressed) ? gui->pressed : gui->hovered;

    if (tb_widget_alive(target))
    {
        tb_gui_send_kind(gui, TB_GUI_EVENT_MOTION, target);
    }
}

/* A press closes the popups in front of the one it is in, or every popup
 * when it is in none, and is then used by closing them. Returns whether the
 * press goes on to its widget. */
static bool tb_gui_press_popups(tb_gui *gui, tb_widget target)
{
    const size_t level =
        tb_widget_alive(target) ? tb_gui_popup_level(gui, target.index, false) : SIZE_MAX;

    gui->tooltip_spent = true;
    if (!tb_widget_alive(tb_gui_active_popup(gui)))
    {
        return true;
    }
    if (level == SIZE_MAX)
    {
        tb_gui_close_popups_from(gui, 0, false);
        return false;
    }
    tb_gui_close_popups_from(gui, level + 1, false);
    return true;
}

static void tb_gui_press(tb_gui *gui, const tb_window_event *event)
{
    const tb_widget target = gui->hovered;
    tb_gui_event press = tb_gui_event_of(gui, TB_GUI_EVENT_PRESS, target);

    press.button = event->button;
    if (tb_widget_alive(gui->pressed))
    {
        /* Another button is held: the widget pressed has every button. */
        tb_gui_event held = tb_gui_event_of(gui, TB_GUI_EVENT_PRESS, gui->pressed);
        held.button = event->button;
        (void)tb_gui_deliver(gui, gui->pressed, &held);
        return;
    }
    if (!tb_gui_press_popups(gui, target))
    {
        return;
    }
    if (event->button == TB_MOUSE_BUTTON_LEFT)
    {
        /* A press in a popup, a menu or a list, leaves the focus where it
         * is, unless it is on a field to type in. */
        const bool focuses = tb_gui_takes_focus(gui, target, TB_WIDGET_CLASS_FOCUS_ON_CLICK);
        const bool in_popup =
            tb_widget_alive(target) && tb_gui_popup_level(gui, target.index, false) != SIZE_MAX;
        if (focuses || !in_popup)
        {
            tb_gui_set_focus(gui, focuses ? target : TB_WIDGET_NONE);
        }
    }
    if (!tb_widget_alive(target) || !tb_gui_enabled_chain(gui, target.index))
    {
        return;
    }
    gui->pressed = target;
    gui->press_button = event->button;
    (void)tb_gui_deliver(gui, target, &press);
    const float distance = hypotf(gui->mouse.x - gui->last_press_position.x,
                                  gui->mouse.y - gui->last_press_position.y);
    if (tb_widget_equal(gui->last_press, target) && gui->last_press_button == event->button &&
        gui->time - gui->last_press_time <= TB_GUI_DOUBLE_CLICK_SECONDS &&
        distance <= TB_GUI_DOUBLE_CLICK_DISTANCE)
    {
        tb_gui_event twice = tb_gui_event_of(gui, TB_GUI_EVENT_DOUBLE_CLICK, target);
        twice.button = event->button;
        gui->last_press = TB_WIDGET_NONE;
        (void)tb_gui_deliver(gui, target, &twice);
        return;
    }
    gui->last_press = target;
    gui->last_press_button = event->button;
    gui->last_press_time = gui->time;
    gui->last_press_position = gui->mouse;
}

static void tb_gui_release_button(tb_gui *gui, const tb_window_event *event)
{
    const tb_widget held = gui->pressed;
    tb_gui_event release = {0};

    if (!tb_widget_alive(held))
    {
        return;
    }
    release = tb_gui_event_of(gui, TB_GUI_EVENT_RELEASE, held);
    release.button = event->button;
    if (event->button != gui->press_button)
    {
        (void)tb_gui_deliver(gui, held, &release);
        return;
    }
    gui->pressed = TB_WIDGET_NONE;
    (void)tb_gui_deliver(gui, held, &release);
    if (tb_widget_alive(held) && tb_widget_equal(tb_gui_find(gui), held))
    {
        tb_gui_event click = tb_gui_event_of(gui, TB_GUI_EVENT_CLICK, held);
        click.button = event->button;
        (void)tb_gui_deliver(gui, held, &click);
    }
    tb_gui_refresh_hover(gui);
}

/* ---------------------------------------------------------------------------
 * Keys and text.
 * ------------------------------------------------------------------------- */

/* A key while a popup takes input: to the popup in front, unless the focus
 * is in it, then to its ancestors; Escape that nothing uses closes it. */
static void tb_gui_popup_key(tb_gui *gui, tb_widget popup, tb_gui_event *key)
{
    const bool focused =
        tb_widget_alive(gui->focus) && tb_gui_node_inside(gui, gui->focus.index, popup.index);
    const tb_widget target = focused ? gui->focus : popup;
    bool used = false;

    key->widget = target;
    key->target = target;
    if (focused)
    {
        used = tb_gui_bubble(gui, target, key);
    }
    else
    {
        used = tb_gui_deliver(gui, popup, key);
    }
    if (used || !tb_widget_is_popup(popup))
    {
        return;
    }
    if (key->key == TB_KEY_ESCAPE)
    {
        tb_widget_close_popup(popup);
        return;
    }
    if (!focused && tb_gui_bubble(gui, tb_widget_parent(popup), key))
    {
        return;
    }
    /* Tab that nothing used closes the popups, then moves the focus. */
    if (key->key == TB_KEY_TAB &&
        (key->modifiers & (TB_MODIFIER_CONTROL | TB_MODIFIER_ALT | TB_MODIFIER_SUPER)) == 0)
    {
        tb_gui_close_popups(gui);
        tb_gui_focus_next(gui, (key->modifiers & TB_MODIFIER_SHIFT) != 0);
    }
}

static void tb_gui_key(tb_gui *gui, const tb_window_event *event)
{
    const uint32_t chords = TB_MODIFIER_CONTROL | TB_MODIFIER_ALT | TB_MODIFIER_SUPER;
    const tb_widget popup = tb_gui_active_popup(gui);
    tb_widget target = gui->focus;

    gui->modifiers = event->modifiers;
    gui->tooltip_spent = true;
    tb_gui_event key = tb_gui_event_of(gui, TB_GUI_EVENT_KEY, target);
    key.key = event->key;
    key.repeat = event->repeat;
    if (tb_widget_alive(popup))
    {
        tb_gui_popup_key(gui, popup, &key);
        return;
    }
    if (event->key == TB_KEY_TAB && (event->modifiers & chords) == 0)
    {
        tb_gui_focus_next(gui, (event->modifiers & TB_MODIFIER_SHIFT) != 0);
        return;
    }
    if (!tb_widget_alive(target))
    {
        target = tb_gui_top_modal(gui);
    }
    if (!tb_widget_alive(target))
    {
        return;
    }
    key.widget = target;
    key.target = target;
    (void)tb_gui_bubble(gui, target, &key);
}

static void tb_gui_type_text(tb_gui *gui, const tb_window_event *event)
{
    if (!tb_widget_alive(gui->focus))
    {
        return;
    }
    tb_gui_event text = tb_gui_event_of(gui, TB_GUI_EVENT_TEXT, gui->focus);
    text.text = event->text;
    (void)tb_gui_deliver(gui, gui->focus, &text);
}

static void tb_gui_handle_event(tb_gui *gui, const tb_window_event *event)
{
    switch (event->type)
    {
    case TB_WINDOW_EVENT_MOUSE_MOVED:
        tb_gui_move_mouse(gui, event);
        tb_gui_send_motion(gui);
        break;
    case TB_WINDOW_EVENT_MOUSE_ENTERED:
        tb_gui_move_mouse(gui, event);
        break;
    case TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN:
        tb_gui_move_mouse(gui, event);
        tb_gui_press(gui, event);
        break;
    case TB_WINDOW_EVENT_MOUSE_BUTTON_UP:
        tb_gui_move_mouse(gui, event);
        tb_gui_release_button(gui, event);
        break;
    case TB_WINDOW_EVENT_MOUSE_WHEEL: {
        tb_gui_move_mouse(gui, event);
        tb_gui_event wheel = tb_gui_event_of(gui, TB_GUI_EVENT_WHEEL, gui->hovered);
        wheel.wheel = (tb_vec2){(float)event->delta_x, (float)event->delta_y};
        tb_gui_bubble(gui, gui->hovered, &wheel);
        break;
    }
    case TB_WINDOW_EVENT_MOUSE_LEFT:
        gui->mouse_inside = false;
        tb_gui_refresh_hover(gui);
        break;
    case TB_WINDOW_EVENT_KEY_DOWN:
        tb_gui_key(gui, event);
        break;
    case TB_WINDOW_EVENT_KEY_UP:
        gui->modifiers = event->modifiers;
        break;
    case TB_WINDOW_EVENT_TEXT_INPUT:
        tb_gui_type_text(gui, event);
        break;
    case TB_WINDOW_EVENT_FOCUS_LOST:
        /* Buttons and keys that come up in another window are never
         * reported: nothing stays held. */
        gui->pressed = TB_WIDGET_NONE;
        gui->modifiers = 0;
        break;
    case TB_WINDOW_EVENT_CLOSE_REQUESTED:
    case TB_WINDOW_EVENT_RESIZED:
    case TB_WINDOW_EVENT_SCALE_CHANGED:
    case TB_WINDOW_EVENT_FOCUS_GAINED:
    case TB_WINDOW_EVENT_MINIMIZED:
    case TB_WINDOW_EVENT_RESTORED:
    case TB_WINDOW_EVENT_DISPLAYS_CHANGED:
    case TB_WINDOW_EVENT_WAKE:
    case TB_WINDOW_EVENT_MOUSE_RELATIVE_MOTION:
        break;
    }
}

void tb_gui_process_input(tb_gui *gui)
{
    /* Callbacks may feed more events; those wait for the next update. */
    const size_t count = gui->queue_count;

    for (size_t index = 0; index < count; index++)
    {
        const tb_window_event event = gui->queue[index];
        tb_gui_layout(gui);
        tb_gui_dispatch_resized(gui);
        tb_gui_handle_event(gui, &event);
    }
    if (count == 0)
    {
        return;
    }
    memmove(gui->queue, gui->queue + count, (gui->queue_count - count) * sizeof(*gui->queue));
    gui->queue_count -= count;
}

/* ---------------------------------------------------------------------------
 * After the events.
 * ------------------------------------------------------------------------- */

/* The cursor of a widget: its own, or its nearest ancestor's. */
static tb_cursor tb_gui_cursor_of(tb_gui *gui, tb_widget widget)
{
    if (!tb_widget_alive(widget))
    {
        return TB_CURSOR_ARROW;
    }
    for (uint32_t at = widget.index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        const int32_t cursor = tb_gui_node_at(gui, at)->cursor;
        if (cursor != TB_GUI_CURSOR_INHERIT)
        {
            return (tb_cursor)cursor;
        }
    }
    return TB_CURSOR_ARROW;
}

static bool tb_gui_stops_mouse(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL && node->mouse_filter == TB_GUI_MOUSE_STOP;
}

void tb_gui_finish_input(tb_gui *gui)
{
    if (tb_widget_alive(gui->focus) &&
        (!tb_gui_node_shown_enabled(gui, gui->focus.index) || !tb_gui_reachable(gui, gui->focus)))
    {
        tb_gui_set_focus(gui, TB_WIDGET_NONE);
    }
    if (tb_widget_alive(gui->pressed) && !tb_gui_node_shown_enabled(gui, gui->pressed.index))
    {
        gui->pressed = TB_WIDGET_NONE;
    }
    tb_gui_close_orphan_popups(gui);
    gui->wants_mouse = gui->modal_count > 0 || tb_gui_stops_mouse(gui->pressed) ||
                       tb_gui_stops_mouse(gui->hovered);
    gui->wants_keyboard = tb_widget_alive(gui->focus) || tb_widget_alive(tb_gui_active_popup(gui));
    gui->cursor =
        tb_gui_cursor_of(gui, tb_widget_alive(gui->pressed) ? gui->pressed : gui->hovered);
}

bool tb_gui_wants_mouse(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->wants_mouse;
}

bool tb_gui_wants_keyboard(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->wants_keyboard;
}

tb_cursor tb_gui_cursor(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->cursor;
}
