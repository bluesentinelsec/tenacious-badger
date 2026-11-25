/* Navigation: the keys mapped to actions, the actions a game feeds, the
 * focus moved by direction to a widget's neighbour or the nearest widget,
 * the default focus of a screen, and the focus given back when a modal
 * widget or a popup closes. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>

/* Sides a widget names neighbours on: the four directions, next, and
 * previous, in the order of tb_gui_nav from TB_GUI_NAV_UP. */
#define TB_GUI_SIDES 6u

/* Units within which two edges or two distances are taken as one, against
 * the rounding of edges to whole pixels. */
#define TB_GUI_NAV_EPSILON 0.01f

/* Bytes of a neighbour's path read as a property, at most. */
#define TB_GUI_PATH_CAPACITY 512u

/* Records, keys, and actions a GUI first holds. */
#define TB_GUI_FIRST_RECORDS 8u
#define TB_GUI_FIRST_KEYS 16u

/* What a widget says of its navigation: its neighbours, by handle or by
 * path, whether it is its screen's default focus, and the axes it wraps
 * along. Kept only for the widgets that say any of it; a record whose
 * widget is stale is dropped. */
typedef struct tb_gui_nav_record
{
    tb_widget widget;
    tb_widget neighbors[TB_GUI_SIDES];
    tb_gui_string paths[TB_GUI_SIDES];   /* a neighbour named by path; empty for none */
    tb_gui_string written[TB_GUI_SIDES]; /* a neighbour's path as last read */
    bool default_focus;
    uint32_t wrap;
} tb_gui_nav_record;

/* A key, with exactly its modifiers, and its action. */
typedef struct tb_gui_key_binding
{
    tb_key key;
    uint32_t modifiers;
    tb_gui_nav action;
} tb_gui_key_binding;

/* An action fed, and how many window events were fed before it. */
typedef struct tb_gui_fed_action
{
    tb_gui_nav action;
    size_t position;
} tb_gui_fed_action;

/* The focus when a modal widget or a popup, its holder, was shown. */
typedef struct tb_gui_focus_memory
{
    uint32_t holder;
    tb_widget focus;
} tb_gui_focus_memory;

struct tb_gui_navigation
{
    tb_gui_nav_record *records;
    size_t record_count;
    size_t record_capacity;
    tb_gui_key_binding *keys;
    size_t key_count;
    size_t key_capacity;
    tb_gui_fed_action *actions;
    size_t action_count;
    size_t action_capacity;
    size_t actions_taken; /* of those this update takes */
    size_t actions_limit; /* those fed before this update */
    tb_gui_focus_memory *memories;
    size_t memory_count;
    size_t memory_capacity;
    tb_widget restore; /* the focus to give back, or none */
};

static size_t tb_gui_grow(size_t capacity, size_t first)
{
    return capacity == 0 ? first : capacity * 2;
}

static void tb_gui_record_free(tb_gui_nav_record *record)
{
    for (size_t side = 0; side < TB_GUI_SIDES; side++)
    {
        tb_gui_string_free(&record->paths[side]);
        tb_gui_string_free(&record->written[side]);
    }
}

/* ---------------------------------------------------------------------------
 * The GUI's navigation.
 * ------------------------------------------------------------------------- */

void tb_gui_navigation_create(tb_gui *gui)
{
    gui->navigation = TB_NEW(tb_gui_navigation);
    tb_gui_reset_key_map(gui);
}

void tb_gui_navigation_free(tb_gui *gui)
{
    tb_gui_navigation *const navigation = gui->navigation;

    for (size_t index = 0; index < navigation->record_count; index++)
    {
        tb_gui_record_free(&navigation->records[index]);
    }
    tb_free(navigation->records);
    tb_free(navigation->keys);
    tb_free(navigation->actions);
    tb_free(navigation->memories);
    tb_free(navigation);
    gui->navigation = NULL;
}

/* ---------------------------------------------------------------------------
 * The key map.
 * ------------------------------------------------------------------------- */

static const uint32_t tb_gui_modifier_mask =
    TB_MODIFIER_SHIFT | TB_MODIFIER_CONTROL | TB_MODIFIER_ALT | TB_MODIFIER_SUPER;

static size_t tb_gui_key_index(const tb_gui_navigation *navigation, tb_key key, uint32_t modifiers)
{
    for (size_t index = 0; index < navigation->key_count; index++)
    {
        const tb_gui_key_binding *const binding = &navigation->keys[index];
        if (binding->key == key && binding->modifiers == modifiers)
        {
            return index;
        }
    }
    return SIZE_MAX;
}

bool tb_gui_map_key(tb_gui *gui, tb_key key, uint32_t modifiers, tb_gui_nav action)
{
    TB_ASSERT(gui != NULL);
    tb_gui_navigation *const navigation = gui->navigation;

    if ((size_t)key >= TB_KEY_COUNT || key == TB_KEY_UNKNOWN ||
        (size_t)action >= TB_GUI_NAV_COUNT || (modifiers & ~tb_gui_modifier_mask) != 0)
    {
        return false;
    }
    const size_t found = tb_gui_key_index(navigation, key, modifiers);
    if (found != SIZE_MAX)
    {
        /* Unmapped, the last takes its place. */
        if (action == TB_GUI_NAV_NONE)
        {
            navigation->key_count--;
            navigation->keys[found] = navigation->keys[navigation->key_count];
            return true;
        }
        navigation->keys[found].action = action;
        return true;
    }
    if (action == TB_GUI_NAV_NONE)
    {
        return true;
    }
    if (navigation->key_count == navigation->key_capacity)
    {
        navigation->key_capacity = tb_gui_grow(navigation->key_capacity, TB_GUI_FIRST_KEYS);
        navigation->keys =
            tb_realloc_array(navigation->keys, navigation->key_capacity, sizeof(*navigation->keys));
    }
    navigation->keys[navigation->key_count] =
        (tb_gui_key_binding){.key = key, .modifiers = modifiers, .action = action};
    navigation->key_count++;
    return true;
}

tb_gui_nav tb_gui_key_action(const tb_gui *gui, tb_key key, uint32_t modifiers)
{
    TB_ASSERT(gui != NULL);
    const size_t found = tb_gui_key_index(gui->navigation, key, modifiers & tb_gui_modifier_mask);
    return found != SIZE_MAX ? gui->navigation->keys[found].action : TB_GUI_NAV_NONE;
}

void tb_gui_clear_key_map(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    gui->navigation->key_count = 0;
}

void tb_gui_reset_key_map(tb_gui *gui)
{
    static const tb_gui_key_binding defaults[] = {
        {TB_KEY_UP, 0, TB_GUI_NAV_UP},
        {TB_KEY_DOWN, 0, TB_GUI_NAV_DOWN},
        {TB_KEY_LEFT, 0, TB_GUI_NAV_LEFT},
        {TB_KEY_RIGHT, 0, TB_GUI_NAV_RIGHT},
        {TB_KEY_ENTER, 0, TB_GUI_NAV_ACCEPT},
        {TB_KEY_SPACE, 0, TB_GUI_NAV_ACCEPT},
        {TB_KEY_ESCAPE, 0, TB_GUI_NAV_BACK},
        {TB_KEY_TAB, 0, TB_GUI_NAV_NEXT},
        {TB_KEY_TAB, TB_MODIFIER_SHIFT, TB_GUI_NAV_PREVIOUS},
        {TB_KEY_TAB, TB_MODIFIER_CONTROL, TB_GUI_NAV_TAB_NEXT},
        {TB_KEY_TAB, TB_MODIFIER_CONTROL | TB_MODIFIER_SHIFT, TB_GUI_NAV_TAB_PREVIOUS},
        {TB_KEY_PAGE_DOWN, TB_MODIFIER_CONTROL, TB_GUI_NAV_TAB_NEXT},
        {TB_KEY_PAGE_UP, TB_MODIFIER_CONTROL, TB_GUI_NAV_TAB_PREVIOUS},
    };

    TB_ASSERT(gui != NULL);
    tb_gui_clear_key_map(gui);
    for (size_t index = 0; index < sizeof(defaults) / sizeof(defaults[0]); index++)
    {
        (void)tb_gui_map_key(gui, defaults[index].key, defaults[index].modifiers,
                             defaults[index].action);
    }
}

tb_key tb_gui_action_key(tb_gui_nav action, uint32_t *modifiers)
{
    const uint32_t control_shift = TB_MODIFIER_CONTROL | TB_MODIFIER_SHIFT;

    *modifiers = 0;
    switch (action)
    {
    case TB_GUI_NAV_UP:
        return TB_KEY_UP;
    case TB_GUI_NAV_DOWN:
        return TB_KEY_DOWN;
    case TB_GUI_NAV_LEFT:
        return TB_KEY_LEFT;
    case TB_GUI_NAV_RIGHT:
        return TB_KEY_RIGHT;
    case TB_GUI_NAV_ACCEPT:
        return TB_KEY_ENTER;
    case TB_GUI_NAV_BACK:
        return TB_KEY_ESCAPE;
    case TB_GUI_NAV_NEXT:
        return TB_KEY_TAB;
    case TB_GUI_NAV_PREVIOUS:
        *modifiers = TB_MODIFIER_SHIFT;
        return TB_KEY_TAB;
    case TB_GUI_NAV_TAB_NEXT:
        *modifiers = TB_MODIFIER_CONTROL;
        return TB_KEY_TAB;
    case TB_GUI_NAV_TAB_PREVIOUS:
        *modifiers = control_shift;
        return TB_KEY_TAB;
    case TB_GUI_NAV_NONE:
    case TB_GUI_NAV_COUNT:
        break;
    }
    return TB_KEY_UNKNOWN;
}

/* ---------------------------------------------------------------------------
 * Actions fed.
 * ------------------------------------------------------------------------- */

void tb_gui_feed_navigation(tb_gui *gui, tb_gui_nav action)
{
    TB_ASSERT(gui != NULL);
    tb_gui_navigation *const navigation = gui->navigation;

    if (action == TB_GUI_NAV_NONE || (size_t)action >= TB_GUI_NAV_COUNT)
    {
        return;
    }
    if (navigation->action_count == navigation->action_capacity)
    {
        navigation->action_capacity = tb_gui_grow(navigation->action_capacity, TB_GUI_FIRST_KEYS);
        navigation->actions = tb_realloc_array(navigation->actions, navigation->action_capacity,
                                               sizeof(*navigation->actions));
    }
    navigation->actions[navigation->action_count] =
        (tb_gui_fed_action){.action = action, .position = gui->queue_count};
    navigation->action_count++;
}

void tb_gui_actions_begin(tb_gui *gui)
{
    gui->navigation->actions_taken = 0;
    gui->navigation->actions_limit = gui->navigation->action_count;
}

bool tb_gui_take_action(tb_gui *gui, size_t position, tb_gui_nav *action)
{
    tb_gui_navigation *const navigation = gui->navigation;

    if (navigation->actions_taken >= navigation->actions_limit ||
        navigation->actions[navigation->actions_taken].position > position)
    {
        return false;
    }
    *action = navigation->actions[navigation->actions_taken].action;
    navigation->actions_taken++;
    return true;
}

void tb_gui_actions_end(tb_gui *gui, size_t events)
{
    tb_gui_navigation *const navigation = gui->navigation;
    const size_t taken = navigation->actions_taken;

    /* Those fed meanwhile wait, placed among the events that wait. */
    for (size_t index = taken; index < navigation->action_count; index++)
    {
        tb_gui_fed_action entry = navigation->actions[index];
        entry.position = entry.position > events ? entry.position - events : 0;
        navigation->actions[index - taken] = entry;
    }
    navigation->action_count -= taken;
    navigation->actions_taken = 0;
    navigation->actions_limit = 0;
}

/* ---------------------------------------------------------------------------
 * Widgets' records: neighbours, default focus, and wrapping.
 * ------------------------------------------------------------------------- */

static tb_gui_nav_record *tb_gui_record_of(tb_widget widget)
{
    tb_gui_navigation *const navigation = widget.gui->navigation;

    for (size_t index = 0; index < navigation->record_count; index++)
    {
        if (tb_widget_equal(navigation->records[index].widget, widget))
        {
            return &navigation->records[index];
        }
    }
    return NULL;
}

/* A widget's record, made when it has none. The widget is alive. */
static tb_gui_nav_record *tb_gui_record_made(tb_widget widget)
{
    tb_gui_navigation *const navigation = widget.gui->navigation;
    tb_gui_nav_record *const found = tb_gui_record_of(widget);
    size_t kept = 0;

    if (found != NULL)
    {
        return found;
    }
    /* Drop the records of widgets destroyed before growing. */
    for (size_t index = 0; index < navigation->record_count; index++)
    {
        if (!tb_widget_alive(navigation->records[index].widget))
        {
            tb_gui_record_free(&navigation->records[index]);
            continue;
        }
        navigation->records[kept] = navigation->records[index];
        kept++;
    }
    navigation->record_count = kept;
    if (navigation->record_count == navigation->record_capacity)
    {
        navigation->record_capacity =
            tb_gui_grow(navigation->record_capacity, TB_GUI_FIRST_RECORDS);
        navigation->records = tb_realloc_array(navigation->records, navigation->record_capacity,
                                               sizeof(*navigation->records));
    }
    tb_gui_nav_record *const made = &navigation->records[navigation->record_count];
    *made = (tb_gui_nav_record){.widget = widget};
    navigation->record_count++;
    return made;
}

static bool tb_gui_is_side(tb_gui_nav side)
{
    return side >= TB_GUI_NAV_UP && side <= TB_GUI_NAV_PREVIOUS && side != TB_GUI_NAV_ACCEPT &&
           side != TB_GUI_NAV_BACK;
}

/* A side's slot among a record's neighbours. */
static size_t tb_gui_side_slot(tb_gui_nav side)
{
    if (side >= TB_GUI_NAV_NEXT)
    {
        return (size_t)(side - TB_GUI_NAV_NEXT) + 4u;
    }
    return (size_t)(side - TB_GUI_NAV_UP);
}

bool tb_widget_set_focus_neighbor(tb_widget widget, tb_gui_nav side, tb_widget neighbor)
{
    if (!tb_widget_alive(widget) || !tb_gui_is_side(side))
    {
        return false;
    }
    tb_gui_nav_record *const record = tb_gui_record_made(widget);
    const size_t slot = tb_gui_side_slot(side);
    record->neighbors[slot] = neighbor;
    tb_gui_string_free(&record->paths[slot]);
    return true;
}

/* A neighbour's path looked up from a widget's parent, then from each
 * ancestor up to the root: the nearest that has it. */
static tb_widget tb_gui_find_neighbor(tb_widget widget, const tb_gui_string *path)
{
    for (tb_widget from = tb_widget_parent(widget); tb_widget_alive(from);
         from = tb_widget_parent(from))
    {
        const tb_widget found = tb_widget_find(from, path->bytes);
        if (tb_widget_alive(found))
        {
            return found;
        }
    }
    return TB_WIDGET_NONE;
}

tb_widget tb_widget_focus_neighbor(tb_widget widget, tb_gui_nav side)
{
    if (!tb_widget_alive(widget) || !tb_gui_is_side(side))
    {
        return TB_WIDGET_NONE;
    }
    const tb_gui_nav_record *const record = tb_gui_record_of(widget);
    if (record == NULL)
    {
        return TB_WIDGET_NONE;
    }
    const size_t slot = tb_gui_side_slot(side);
    if (record->paths[slot].length > 0)
    {
        return tb_gui_find_neighbor(widget, &record->paths[slot]);
    }
    return tb_widget_alive(record->neighbors[slot]) ? record->neighbors[slot] : TB_WIDGET_NONE;
}

/* A side's neighbour as a property: its path as set, or the path from the
 * root of the widget set by handle, or empty. */
static bool tb_gui_get_neighbor(tb_widget widget, tb_gui_nav side, tb_gui_value *value)
{
    tb_gui_nav_record *const record = tb_gui_record_of(widget);
    const size_t slot = tb_gui_side_slot(side);
    char buffer[TB_GUI_PATH_CAPACITY] = {0};
    size_t length = 0;

    *value = tb_gui_text(tb_text_make("", 0));
    if (record == NULL)
    {
        return true;
    }
    if (record->paths[slot].length > 0)
    {
        *value = tb_gui_text(tb_gui_string_text(&record->paths[slot]));
        return true;
    }
    if (tb_widget_alive(record->neighbors[slot]) &&
        tb_widget_path(record->neighbors[slot], buffer, sizeof(buffer), &length))
    {
        tb_gui_string_set(&record->written[slot], tb_text_make(buffer, length));
        *value = tb_gui_text(tb_gui_string_text(&record->written[slot]));
    }
    return true;
}

static bool tb_gui_set_neighbor(tb_widget widget, tb_gui_nav side, const tb_gui_value *value)
{
    const size_t slot = tb_gui_side_slot(side);
    tb_gui_nav_record *const record =
        value->text.length > 0 ? tb_gui_record_made(widget) : tb_gui_record_of(widget);

    if (record != NULL)
    {
        tb_gui_string_set(&record->paths[slot], value->text);
        record->neighbors[slot] = TB_WIDGET_NONE;
    }
    return true;
}

bool tb_gui_get_focus_up(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_neighbor(widget, TB_GUI_NAV_UP, value);
}

bool tb_gui_set_focus_up(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_gui_set_neighbor(widget, TB_GUI_NAV_UP, value);
}

bool tb_gui_get_focus_down(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_neighbor(widget, TB_GUI_NAV_DOWN, value);
}

bool tb_gui_set_focus_down(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_gui_set_neighbor(widget, TB_GUI_NAV_DOWN, value);
}

bool tb_gui_get_focus_left(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_neighbor(widget, TB_GUI_NAV_LEFT, value);
}

bool tb_gui_set_focus_left(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_gui_set_neighbor(widget, TB_GUI_NAV_LEFT, value);
}

bool tb_gui_get_focus_right(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_neighbor(widget, TB_GUI_NAV_RIGHT, value);
}

bool tb_gui_set_focus_right(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_gui_set_neighbor(widget, TB_GUI_NAV_RIGHT, value);
}

bool tb_gui_get_focus_next(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_neighbor(widget, TB_GUI_NAV_NEXT, value);
}

bool tb_gui_set_focus_next(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_gui_set_neighbor(widget, TB_GUI_NAV_NEXT, value);
}

bool tb_gui_get_focus_previous(tb_widget widget, const void *data, tb_gui_value *value)
{
    (void)data;
    return tb_gui_get_neighbor(widget, TB_GUI_NAV_PREVIOUS, value);
}

bool tb_gui_set_focus_previous(tb_widget widget, void *data, const tb_gui_value *value)
{
    (void)data;
    return tb_gui_set_neighbor(widget, TB_GUI_NAV_PREVIOUS, value);
}

tb_widget tb_gui_neighbor_taking_focus(tb_widget widget, tb_gui_nav side)
{
    const tb_widget neighbor = tb_widget_focus_neighbor(widget, side);

    if (!tb_widget_alive(neighbor) || neighbor.gui != widget.gui ||
        !tb_gui_takes_focus(widget.gui, neighbor, TB_WIDGET_CLASS_FOCUSABLE))
    {
        return TB_WIDGET_NONE;
    }
    return neighbor;
}

bool tb_gui_get_default_focus(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_nav_record *const record = tb_gui_record_of(widget);

    (void)data;
    *value = tb_gui_bool(record != NULL && record->default_focus);
    return true;
}

bool tb_gui_set_default_focus(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_nav_record *const record =
        value->boolean ? tb_gui_record_made(widget) : tb_gui_record_of(widget);

    (void)data;
    if (record != NULL)
    {
        record->default_focus = value->boolean;
    }
    return true;
}

bool tb_gui_get_focus_wrap(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_nav_record *const record = tb_gui_record_of(widget);

    (void)data;
    *value = tb_gui_flags(record != NULL ? record->wrap : 0u);
    return true;
}

bool tb_gui_set_focus_wrap(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_nav_record *const record =
        value->flags != 0 ? tb_gui_record_made(widget) : tb_gui_record_of(widget);

    (void)data;
    if (record != NULL)
    {
        record->wrap = value->flags;
    }
    return true;
}

static uint32_t tb_gui_wrap_of(tb_gui *gui, uint32_t index)
{
    const tb_gui_nav_record *const record = tb_gui_record_of(tb_gui_handle(gui, index));
    return record != NULL ? record->wrap : 0u;
}

static bool tb_gui_is_default(tb_gui *gui, uint32_t index)
{
    const tb_gui_nav_record *const record = tb_gui_record_of(tb_gui_handle(gui, index));
    return record != NULL && record->default_focus;
}

/* ---------------------------------------------------------------------------
 * The default focus.
 * ------------------------------------------------------------------------- */

/* The first widget within a scope that takes the focus from Tab, and is
 * marked default when `marked`; none when there is none. */
static tb_widget tb_gui_first_focus_in(tb_gui *gui, uint32_t scope, bool marked)
{
    for (uint32_t at = scope; at != TB_GUI_NO_NODE;)
    {
        const bool walked = tb_gui_focus_walks(gui, at, scope);
        const tb_widget widget = tb_gui_handle(gui, at);
        if (walked && (!marked || tb_gui_is_default(gui, at)) &&
            tb_gui_takes_focus(gui, widget, TB_WIDGET_CLASS_FOCUSABLE))
        {
            return widget;
        }
        at = tb_gui_walk_next(gui, at, scope, walked);
    }
    return TB_WIDGET_NONE;
}

tb_widget tb_gui_default_focus_in(tb_gui *gui, uint32_t scope)
{
    const tb_gui_navigation *const navigation = gui->navigation;
    bool any = false;

    /* Most GUIs mark none, or few: look at the records before the tree. */
    for (size_t index = 0; index < navigation->record_count && !any; index++)
    {
        const tb_gui_nav_record *const record = &navigation->records[index];
        any = record->default_focus && tb_widget_alive(record->widget) &&
              tb_gui_node_inside(gui, record->widget.index, scope);
    }
    if (!any)
    {
        return TB_WIDGET_NONE;
    }
    tb_gui_layout(gui);
    return tb_gui_first_focus_in(gui, scope, true);
}

bool tb_widget_focus_default(tb_widget container)
{
    tb_widget chosen = TB_WIDGET_NONE;

    if (!tb_widget_alive(container))
    {
        return false;
    }
    chosen = tb_gui_default_focus_in(container.gui, container.index);
    if (!tb_widget_alive(chosen))
    {
        tb_gui_layout(container.gui);
        chosen = tb_gui_first_focus_in(container.gui, container.index, false);
    }
    if (!tb_widget_alive(chosen))
    {
        return false;
    }
    tb_gui_give_focus(container.gui, chosen, false);
    return true;
}

/* ---------------------------------------------------------------------------
 * Engagement and the focus shown.
 * ------------------------------------------------------------------------- */

bool tb_widget_engaged(tb_widget widget)
{
    return tb_widget_alive(widget) && tb_widget_equal(widget.gui->focus, widget) &&
           widget.gui->focus_engaged;
}

bool tb_widget_engage(tb_widget widget, bool engaged)
{
    if (!tb_widget_alive(widget) || !tb_widget_equal(widget.gui->focus, widget))
    {
        return false;
    }
    widget.gui->focus_engaged = engaged;
    return true;
}

bool tb_gui_focus_visible(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->focus_visible;
}

void tb_gui_set_focus_visible(tb_gui *gui, bool visible)
{
    TB_ASSERT(gui != NULL);
    gui->focus_visible = visible;
}

/* ---------------------------------------------------------------------------
 * The focus given back.
 * ------------------------------------------------------------------------- */

void tb_gui_remember_focus(tb_gui *gui, tb_widget holder)
{
    tb_gui_navigation *const navigation = gui->navigation;

    for (size_t index = 0; index < navigation->memory_count; index++)
    {
        if (navigation->memories[index].holder == holder.index)
        {
            navigation->memories[index].focus = gui->focus;
            return;
        }
    }
    if (navigation->memory_count == navigation->memory_capacity)
    {
        navigation->memory_capacity =
            tb_gui_grow(navigation->memory_capacity, TB_GUI_FIRST_RECORDS);
        navigation->memories = tb_realloc_array(navigation->memories, navigation->memory_capacity,
                                                sizeof(*navigation->memories));
    }
    navigation->memories[navigation->memory_count] =
        (tb_gui_focus_memory){.holder = holder.index, .focus = gui->focus};
    navigation->memory_count++;
}

void tb_gui_holder_closed(tb_gui *gui, uint32_t holder)
{
    tb_gui_navigation *const navigation = gui->navigation;

    for (size_t index = 0; index < navigation->memory_count; index++)
    {
        const tb_gui_focus_memory memory = navigation->memories[index];
        if (memory.holder != holder)
        {
            continue;
        }
        navigation->memory_count--;
        navigation->memories[index] = navigation->memories[navigation->memory_count];
        /* The focus left in it, or taken with it, goes back. */
        if (!tb_widget_alive(gui->focus) || tb_gui_node_inside(gui, gui->focus.index, holder))
        {
            navigation->restore = memory.focus;
        }
        return;
    }
}

void tb_gui_restore_focus(tb_gui *gui)
{
    const tb_widget restore = gui->navigation->restore;

    if (restore.gui == NULL)
    {
        return;
    }
    gui->navigation->restore = TB_WIDGET_NONE;
    if (tb_widget_alive(restore) &&
        tb_gui_takes_focus(gui, restore,
                           TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK))
    {
        tb_gui_give_focus(gui, restore, false);
    }
}

void tb_gui_forget_restore(tb_gui *gui)
{
    gui->navigation->restore = TB_WIDGET_NONE;
}

/* ---------------------------------------------------------------------------
 * Moving by direction.
 * ------------------------------------------------------------------------- */

static bool tb_gui_vertical(tb_gui_nav direction)
{
    return direction == TB_GUI_NAV_UP || direction == TB_GUI_NAV_DOWN;
}

static float tb_gui_right(tb_canvas_rect rect)
{
    return rect.x + rect.width;
}

static float tb_gui_bottom(tb_canvas_rect rect)
{
    return rect.y + rect.height;
}

/* Where a widget is, for navigation: its rectangle; or, for one with
 * widgets shown in it, the largest part of it beside them, as a notebook's
 * tabs, so that its own widgets are in a direction from it. */
static tb_canvas_rect tb_gui_nav_rect(tb_gui *gui, uint32_t index)
{
    const tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_canvas_rect rect = node->rect;
    bool any = false;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    for (uint32_t child = node->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->next)
    {
        const tb_gui_node *const inner = tb_gui_node_at(gui, child);
        if (!inner->shown || inner->popup)
        {
            continue;
        }
        const tb_canvas_rect seen = tb_ui_rect_intersect(rect, inner->rect);
        left = any ? fminf(left, seen.x) : seen.x;
        top = any ? fminf(top, seen.y) : seen.y;
        right = any ? fmaxf(right, tb_gui_right(seen)) : tb_gui_right(seen);
        bottom = any ? fmaxf(bottom, tb_gui_bottom(seen)) : tb_gui_bottom(seen);
        any = true;
    }
    if (!any)
    {
        return rect;
    }
    const tb_canvas_rect bands[4] = {
        {rect.x, rect.y, rect.width, top - rect.y},
        {rect.x, bottom, rect.width, tb_gui_bottom(rect) - bottom},
        {rect.x, rect.y, left - rect.x, rect.height},
        {right, rect.y, tb_gui_right(rect) - right, rect.height},
    };
    tb_canvas_rect chosen = rect;
    float largest = 0.0f;
    for (size_t band = 0; band < 4; band++)
    {
        const float area = fmaxf(bands[band].width, 0.0f) * fmaxf(bands[band].height, 0.0f);
        if (area > largest + TB_GUI_NAV_EPSILON)
        {
            largest = area;
            chosen = bands[band];
        }
    }
    return chosen;
}

/* How far a rectangle lies past another's edge on a side, and how far
 * apart they are across; a negative `along` is not past it. They overlap
 * when each reaches across into the other: in line, in Unreal's swept band. */
typedef struct tb_gui_nav_gap
{
    float along;
    float across;
    bool overlaps;
} tb_gui_nav_gap;

static tb_gui_nav_gap tb_gui_gap(tb_canvas_rect from, tb_canvas_rect to, tb_gui_nav direction)
{
    tb_gui_nav_gap gap = {0};

    if (direction == TB_GUI_NAV_UP)
    {
        gap.along = from.y - tb_gui_bottom(to);
    }
    else if (direction == TB_GUI_NAV_DOWN)
    {
        gap.along = to.y - tb_gui_bottom(from);
    }
    else if (direction == TB_GUI_NAV_LEFT)
    {
        gap.along = from.x - tb_gui_right(to);
    }
    else
    {
        gap.along = to.x - tb_gui_right(from);
    }
    /* Apart across: positive; overlapping: negative, by how much. */
    float apart = 0.0f;
    if (tb_gui_vertical(direction))
    {
        apart = fmaxf(to.x - tb_gui_right(from), from.x - tb_gui_right(to));
    }
    else
    {
        apart = fmaxf(to.y - tb_gui_bottom(from), from.y - tb_gui_bottom(to));
    }
    gap.across = fmaxf(apart, 0.0f);
    gap.overlaps = apart < -TB_GUI_NAV_EPSILON;
    return gap;
}

/* The best candidate so far. */
typedef struct tb_gui_nav_best
{
    uint32_t index;
    float distance;
    bool overlaps;
} tb_gui_nav_best;

/* Whether a candidate beats the best so far: one in line beats one not;
 * then the nearer wins; a tie keeps the first in the order of the tree. */
static bool tb_gui_beats(const tb_gui_nav_best *best, bool overlaps, float distance)
{
    if (best->index == TB_GUI_NO_NODE)
    {
        return true;
    }
    if (overlaps != best->overlaps)
    {
        return overlaps;
    }
    return distance < best->distance - TB_GUI_NAV_EPSILON;
}

/* The nearest widget that takes the focus within a scope, past a
 * rectangle's edge on a side; the focused widget itself is passed over,
 * unless `with_focus`, as when the search goes around to it. */
static uint32_t tb_gui_nearest(tb_gui *gui, uint32_t scope, tb_canvas_rect from,
                               tb_gui_nav direction, bool with_focus)
{
    tb_gui_nav_best best = {.index = TB_GUI_NO_NODE};

    for (uint32_t at = scope; at != TB_GUI_NO_NODE;)
    {
        const bool walked = tb_gui_focus_walks(gui, at, scope);
        const tb_widget widget = tb_gui_handle(gui, at);
        const bool candidate = with_focus || !tb_widget_equal(widget, gui->focus);
        if (walked && candidate && tb_gui_takes_focus(gui, widget, TB_WIDGET_CLASS_FOCUSABLE))
        {
            const tb_gui_nav_gap gap = tb_gui_gap(from, tb_gui_nav_rect(gui, at), direction);
            const float distance = hypotf(fmaxf(gap.along, 0.0f), gap.across);
            if (gap.along >= -TB_GUI_NAV_EPSILON && tb_gui_beats(&best, gap.overlaps, distance))
            {
                best =
                    (tb_gui_nav_best){.index = at, .distance = distance, .overlaps = gap.overlaps};
            }
        }
        at = tb_gui_walk_next(gui, at, scope, walked);
    }
    return best.index;
}

/* A rectangle moved past a container's far edge on the side opposite a
 * direction, to search it again from there. */
static tb_canvas_rect tb_gui_wrapped(tb_canvas_rect from, tb_canvas_rect container,
                                     tb_gui_nav direction)
{
    tb_canvas_rect moved = from;

    if (direction == TB_GUI_NAV_UP)
    {
        moved.y = tb_gui_bottom(container);
    }
    else if (direction == TB_GUI_NAV_DOWN)
    {
        moved.y = container.y - from.height;
    }
    else if (direction == TB_GUI_NAV_LEFT)
    {
        moved.x = tb_gui_right(container);
    }
    else
    {
        moved.x = container.x - from.width;
    }
    return moved;
}

/* The scope a widget's navigation stays within: the popup it is in, or
 * the modal widget in front, or the root. */
static uint32_t tb_gui_scope_of(tb_gui *gui, tb_widget widget)
{
    const size_t level = tb_gui_popup_level(gui, widget.index, false);
    const tb_widget modal = tb_gui_top_modal(gui);

    if (level != SIZE_MAX)
    {
        return gui->popups[level].widget.index;
    }
    return modal.gui != NULL ? modal.index : 0;
}

/* The widget a direction moves the focus to by geometry, or none. */
static uint32_t tb_gui_find_direction(tb_gui *gui, tb_gui_nav direction)
{
    const uint32_t scope = tb_gui_scope_of(gui, gui->focus);
    const uint32_t axis =
        tb_gui_vertical(direction) ? TB_GUI_WRAP_VERTICAL : TB_GUI_WRAP_HORIZONTAL;
    const tb_canvas_rect from = tb_gui_nav_rect(gui, gui->focus.index);

    /* The nearest ancestor that wraps along the axis keeps the search
     * inside it, around its far edge. */
    for (uint32_t at = tb_gui_node_at(gui, gui->focus.index)->parent;
         at != TB_GUI_NO_NODE && tb_gui_node_inside(gui, at, scope);
         at = tb_gui_node_at(gui, at)->parent)
    {
        if ((tb_gui_wrap_of(gui, at) & axis) == 0)
        {
            continue;
        }
        const uint32_t inside = tb_gui_nearest(gui, at, from, direction, false);
        if (inside != TB_GUI_NO_NODE)
        {
            return inside;
        }
        const tb_canvas_rect container = tb_gui_node_at(gui, at)->rect;
        return tb_gui_nearest(gui, at, tb_gui_wrapped(from, container, direction), direction, true);
    }
    return tb_gui_nearest(gui, scope, from, direction, false);
}

bool tb_gui_focus_direction(tb_gui *gui, tb_gui_nav direction)
{
    TB_ASSERT(gui != NULL);
    if (direction < TB_GUI_NAV_UP || direction > TB_GUI_NAV_RIGHT)
    {
        return false;
    }
    tb_gui_layout(gui);
    if (!tb_widget_alive(gui->focus))
    {
        const tb_widget modal = tb_gui_top_modal(gui);
        const tb_widget chosen = tb_gui_default_focus_in(gui, modal.gui != NULL ? modal.index : 0);
        if (!tb_widget_alive(chosen))
        {
            return false;
        }
        tb_gui_give_focus(gui, chosen, false);
        return true;
    }
    const tb_widget neighbor = tb_gui_neighbor_taking_focus(gui->focus, direction);
    if (tb_widget_alive(neighbor))
    {
        tb_gui_give_focus(gui, neighbor, false);
        return true;
    }
    const uint32_t found = tb_gui_find_direction(gui, direction);
    if (found == TB_GUI_NO_NODE)
    {
        return false;
    }
    tb_gui_give_focus(gui, tb_gui_handle(gui, found), false);
    return true;
}
