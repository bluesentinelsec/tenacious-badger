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

/* Give an event to a widget: its bindings, until one accepts it, then its
 * kind, unless `kind` is false. Returns whether the kind used it, or a
 * binding accepted it. */
static bool tb_gui_deliver_to(tb_gui *gui, tb_widget widget, tb_gui_event *event, bool kind)
{
    tb_gui_node *node = tb_gui_node_of(widget);
    const bool outer_accepted = gui->event_accepted;
    bool accepted = false;
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
    gui->event_accepted = false;
    const size_t count = node->binding_count;
    for (size_t index = 0; index < count && !gui->event_accepted; index++)
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
    accepted = gui->event_accepted;
    gui->event_accepted = outer_accepted;
    node = tb_gui_node_of(widget);
    if (accepted)
    {
        used = true;
    }
    else if (kind && node != NULL && node->kind->event != NULL)
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

static bool tb_gui_deliver(tb_gui *gui, tb_widget widget, tb_gui_event *event)
{
    return tb_gui_deliver_to(gui, widget, event, true);
}

/* Give an event to a widget, then to each ancestor until a kind uses it;
 * the widget's own kind is passed over when `skip_kind`. Returns whether
 * one did, and in `by_target` whether the widget itself did. */
static bool tb_gui_bubble_from(tb_gui *gui, tb_widget target, tb_gui_event *event, bool skip_kind,
                               bool *by_target)
{
    tb_widget widget = target;
    bool first = true;

    *by_target = false;
    while (tb_widget_alive(widget))
    {
        const tb_widget parent = tb_widget_parent(widget);
        if (tb_gui_deliver_to(gui, widget, event, !(first && skip_kind)))
        {
            *by_target = first;
            return true;
        }
        first = false;
        widget = parent;
    }
    return false;
}

/* Give an event to a widget, then to each ancestor until a kind uses it.
 * Returns whether one did. */
static bool tb_gui_bubble(tb_gui *gui, tb_widget target, tb_gui_event *event)
{
    bool by_target = false;
    return tb_gui_bubble_from(gui, target, event, false, &by_target);
}

void tb_gui_accept_event(const tb_gui_event *event)
{
    TB_ASSERT(event != NULL);
    if (event->widget.gui != NULL)
    {
        event->widget.gui->event_accepted = true;
    }
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

tb_widget tb_gui_top_modal(const tb_gui *gui)
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
    gui->focus_engaged = false;
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

void tb_gui_give_focus(tb_gui *gui, tb_widget target, bool engaged)
{
    tb_gui_forget_restore(gui);
    tb_gui_set_focus(gui, target);
    gui->focus_engaged = engaged && tb_widget_equal(gui->focus, target) && tb_widget_alive(target);
}

bool tb_widget_show_modal(tb_widget widget)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    tb_gui *const gui = widget.gui;

    if (node == NULL || node->parent == TB_GUI_NO_NODE)
    {
        return false;
    }
    if (!tb_gui_modal_contains(gui, widget.index))
    {
        tb_gui_remember_focus(gui, widget);
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
    /* A modal widget with a default focus gives it the focus at once. */
    const tb_widget chosen = tb_gui_default_focus_in(gui, widget.index);
    if (tb_widget_alive(chosen))
    {
        tb_gui_give_focus(gui, chosen, false);
    }
    return true;
}

void tb_widget_end_modal(tb_widget widget)
{
    if (!tb_widget_alive(widget) || !tb_gui_modal_contains(widget.gui, widget.index))
    {
        return;
    }
    tb_gui_remove_modal(widget.gui, widget.index);
    tb_gui_holder_closed(widget.gui, widget.index);
    tb_gui_restore_focus(widget.gui);
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
    if (tb_gui_modal_contains(gui, widget.index))
    {
        tb_gui_remove_modal(gui, widget.index);
        tb_gui_holder_closed(gui, widget.index);
    }
    tb_gui_forget_popup(gui, widget.index);
}

/* ---------------------------------------------------------------------------
 * The focus.
 * ------------------------------------------------------------------------- */

bool tb_gui_takes_focus(tb_gui *gui, tb_widget widget, uint32_t flags)
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
    tb_gui_give_focus(widget.gui, widget, true);
    return true;
}

void tb_gui_clear_focus(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    tb_gui_give_focus(gui, TB_WIDGET_NONE, false);
}

bool tb_gui_focus_walks(tb_gui *gui, uint32_t index, uint32_t scope)
{
    const tb_gui_node *const node = tb_gui_node_at(gui, index);
    return node->shown && (index == scope || (!tb_gui_modal_contains(gui, index) && !node->popup));
}

/* The slot after one in the order of the tree, within a scope, skipping
 * the children of those whose children are not walked. */
uint32_t tb_gui_walk_next(tb_gui *gui, uint32_t index, uint32_t scope, bool into)
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
    const tb_widget neighbor =
        tb_gui_neighbor_taking_focus(gui->focus, backward ? TB_GUI_NAV_PREVIOUS : TB_GUI_NAV_NEXT);
    if (tb_widget_alive(neighbor))
    {
        tb_gui_give_focus(gui, neighbor, true);
        return;
    }
    for (uint32_t at = scope; at != TB_GUI_NO_NODE;)
    {
        const bool walked = tb_gui_focus_walks(gui, at, scope);
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
    tb_gui_give_focus(gui, tb_gui_handle(gui, chosen), true);
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

    if (!gui->mouse_inside || !gui->mouse_on_viewport)
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

    const tb_vec2 pixel = {(float)event->pixel_x, (float)event->pixel_y};
    const bool sized = gui->viewport.width > 0 && gui->viewport.height > 0;

    gui->mouse =
        (tb_vec2){(pixel.x - view.origin.x) / view.scale, (pixel.y - view.origin.y) / view.scale};
    if (view.target == TB_CANVAS_TARGET_VIEWPORT)
    {
        /* The view is of the viewport's target, a pixel a unit of the
         * viewport's: the mouse is placed by the viewport itself. */
        const tb_vec2 units = tb_viewport_from_pixels(&gui->viewport, pixel);
        gui->mouse = (tb_vec2){units.x / view.scale, units.y / view.scale};
    }
    gui->mouse_inside = true;
    /* The bars beside the viewport are not the GUI's: a press there reaches
     * no widget. A viewport not given yet has none. */
    gui->mouse_on_viewport = !sized || tb_viewport_contains(&gui->viewport, pixel);
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
    /* The mouse is used: the focus is no longer shown. */
    gui->focus_visible = false;
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
            tb_gui_give_focus(gui, focuses ? target : TB_WIDGET_NONE, focuses);
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
 * Keys, text, and navigation.
 * ------------------------------------------------------------------------- */

/* A key pressed, or the key an action fed stands for, and its action. */
typedef struct tb_gui_key_press
{
    tb_key key;
    uint32_t modifiers;
    bool repeat;
    tb_gui_nav action;
} tb_gui_key_press;

/* Whether a key is the one its action stands for, as a fed action's is. */
static bool tb_gui_is_action_key(const tb_gui_key_press *press)
{
    uint32_t modifiers = 0;
    const tb_key key = tb_gui_action_key(press->action, &modifiers);
    return key == press->key && modifiers == press->modifiers;
}

/* Give a key that happened to `origin` to a widget, and to its ancestors
 * when `bubble`, its own kind passed over when `skip_kind`; then, when
 * nothing used it, the key its action stands for, so that a key mapped to
 * Accept is Enter to the kinds. Returns whether a kind used either, and in
 * `by_target` whether the widget itself did. */
static bool tb_gui_offer_key(tb_gui *gui, tb_widget target, tb_widget origin,
                             const tb_gui_key_press *press, bool bubble, bool skip_kind,
                             bool *by_target)
{
    tb_key keys[2] = {press->key, TB_KEY_UNKNOWN};
    uint32_t modifiers[2] = {press->modifiers, 0};
    size_t count = 1;

    *by_target = false;
    if (press->action != TB_GUI_NAV_NONE && !tb_gui_is_action_key(press))
    {
        keys[1] = tb_gui_action_key(press->action, &modifiers[1]);
        count = 2;
    }
    for (size_t index = 0; index < count && tb_widget_alive(target); index++)
    {
        tb_gui_event key = tb_gui_event_of(gui, TB_GUI_EVENT_KEY, target);
        key.target = origin;
        key.key = keys[index];
        key.modifiers = modifiers[index];
        key.repeat = press->repeat;
        if (bubble && tb_gui_bubble_from(gui, target, &key, skip_kind, by_target))
        {
            return true;
        }
        if (!bubble && tb_gui_deliver_to(gui, target, &key, !skip_kind))
        {
            *by_target = true;
            return true;
        }
    }
    return false;
}

/* A key while a popup takes input: to the popup in front, unless the focus
 * is in it, then to its ancestors; Back that nothing uses closes it. */
static void tb_gui_popup_key(tb_gui *gui, tb_widget popup, const tb_gui_key_press *press)
{
    const bool focused =
        tb_widget_alive(gui->focus) && tb_gui_node_inside(gui, gui->focus.index, popup.index);
    const tb_widget target = focused ? gui->focus : popup;
    bool by_target = false;

    if (tb_gui_offer_key(gui, target, target, press, focused, false, &by_target) ||
        !tb_widget_is_popup(popup))
    {
        return;
    }
    if (press->action == TB_GUI_NAV_BACK)
    {
        tb_widget_close_popup(popup);
        return;
    }
    if (!focused &&
        tb_gui_offer_key(gui, tb_widget_parent(popup), popup, press, true, false, &by_target))
    {
        return;
    }
    /* Tab that nothing used closes the popups, then moves the focus. */
    if (press->action == TB_GUI_NAV_NEXT || press->action == TB_GUI_NAV_PREVIOUS)
    {
        tb_gui_close_popups(gui);
        tb_gui_focus_next(gui, press->action == TB_GUI_NAV_PREVIOUS);
    }
}

static bool tb_gui_kind_engages(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL && (node->kind->flags & TB_WIDGET_CLASS_ENGAGES) != 0;
}

/* The scope navigation stays within: the modal widget in front, or the
 * root. */
static uint32_t tb_gui_navigation_scope(const tb_gui *gui)
{
    const tb_widget modal = tb_gui_top_modal(gui);
    return modal.gui != NULL ? modal.index : 0;
}

/* Back that no kind used: let go of the widget engaged; or else send
 * TB_GUI_EVENT_BACK up the tree; or else hide the modal widget in front. */
static void tb_gui_back(tb_gui *gui)
{
    tb_widget target = gui->focus;

    if (tb_gui_kind_engages(target) && gui->focus_engaged)
    {
        gui->focus_engaged = false;
        return;
    }
    if (!tb_widget_alive(target))
    {
        target = tb_gui_handle(gui, tb_gui_navigation_scope(gui));
    }
    tb_gui_event back = tb_gui_event_of(gui, TB_GUI_EVENT_BACK, target);
    if (tb_gui_bubble(gui, target, &back))
    {
        return;
    }
    const tb_widget modal = tb_gui_top_modal(gui);
    if (tb_widget_alive(modal))
    {
        tb_widget_set_visible(modal, false);
    }
}

/* A key, or an action fed: to the popup in front; or as Tab; or to the
 * widget with the focus and its ancestors, then done as its action when no
 * kind used it. */
static void tb_gui_press_key(tb_gui *gui, const tb_gui_key_press *press)
{
    const tb_widget popup = tb_gui_active_popup(gui);
    const tb_gui_nav action = press->action;
    tb_widget target = gui->focus;
    bool by_target = false;

    if (tb_widget_alive(popup))
    {
        tb_gui_popup_key(gui, popup, press);
        return;
    }
    if (action == TB_GUI_NAV_NEXT || action == TB_GUI_NAV_PREVIOUS)
    {
        tb_gui_focus_next(gui, action == TB_GUI_NAV_PREVIOUS);
        return;
    }
    if (!tb_widget_alive(target))
    {
        /* With nothing focused, a direction or Accept goes to the default. */
        const bool moves =
            (action >= TB_GUI_NAV_UP && action <= TB_GUI_NAV_RIGHT) || action == TB_GUI_NAV_ACCEPT;
        const tb_widget chosen =
            moves ? tb_gui_default_focus_in(gui, tb_gui_navigation_scope(gui)) : TB_WIDGET_NONE;
        if (tb_widget_alive(chosen))
        {
            tb_gui_give_focus(gui, chosen, false);
            return;
        }
        target = tb_gui_top_modal(gui);
    }
    /* A kind that engages keeps none of the keys of navigation until it is
     * engaged: Accept engages it. */
    const bool engages = tb_widget_alive(gui->focus) && tb_gui_kind_engages(gui->focus);
    const bool idle = engages && !gui->focus_engaged;
    if (idle && action == TB_GUI_NAV_ACCEPT)
    {
        gui->focus_engaged = true;
        return;
    }
    const bool skip_kind = idle && action != TB_GUI_NAV_NONE;
    if (tb_gui_offer_key(gui, target, target, press, true, skip_kind, &by_target))
    {
        if (by_target && engages && tb_widget_equal(target, gui->focus))
        {
            gui->focus_engaged = true;
        }
        return;
    }
    if (action >= TB_GUI_NAV_UP && action <= TB_GUI_NAV_RIGHT)
    {
        (void)tb_gui_focus_direction(gui, action);
    }
    else if (action == TB_GUI_NAV_BACK)
    {
        tb_gui_back(gui);
    }
}

static void tb_gui_key(tb_gui *gui, const tb_window_event *event)
{
    const tb_gui_key_press press = {.key = event->key,
                                    .modifiers = event->modifiers,
                                    .repeat = event->repeat,
                                    .action = tb_gui_key_action(gui, event->key, event->modifiers)};

    gui->modifiers = event->modifiers;
    gui->tooltip_spent = true;
    if (press.action != TB_GUI_NAV_NONE)
    {
        gui->focus_visible = true;
    }
    tb_gui_press_key(gui, &press);
}

/* An action fed: the key it stands for, pressed. */
static void tb_gui_navigate(tb_gui *gui, tb_gui_nav action)
{
    tb_gui_key_press press = {.action = action};

    press.key = tb_gui_action_key(action, &press.modifiers);
    gui->tooltip_spent = true;
    gui->focus_visible = true;
    tb_gui_press_key(gui, &press);
}

static void tb_gui_type_text(tb_gui *gui, const tb_window_event *event)
{
    const tb_widget target = gui->focus;

    if (!tb_widget_alive(target))
    {
        return;
    }
    tb_gui_event text = tb_gui_event_of(gui, TB_GUI_EVENT_TEXT, target);
    text.text = event->text;
    /* Text typed into a field engages it. */
    if (tb_gui_deliver(gui, target, &text) && tb_widget_equal(gui->focus, target))
    {
        gui->focus_engaged = true;
    }
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
    /* Callbacks may feed more events and actions; those wait for the next
     * update. Actions are taken in their order among the events. */
    const size_t count = gui->queue_count;
    tb_gui_nav action = TB_GUI_NAV_NONE;

    tb_gui_actions_begin(gui);
    for (size_t index = 0; index <= count; index++)
    {
        while (tb_gui_take_action(gui, index, &action))
        {
            tb_gui_layout(gui);
            tb_gui_dispatch_resized(gui);
            tb_gui_navigate(gui, action);
            tb_gui_restore_focus(gui);
        }
        if (index == count)
        {
            break;
        }
        const tb_window_event event = gui->queue[index];
        tb_gui_layout(gui);
        tb_gui_dispatch_resized(gui);
        tb_gui_handle_event(gui, &event);
        tb_gui_restore_focus(gui);
    }
    tb_gui_actions_end(gui, count);
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
    tb_gui_restore_focus(gui);
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
