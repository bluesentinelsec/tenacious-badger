/* The retained GUI: GUIs, the slots of widgets and their handles, the tree,
 * the kinds registered, the fonts opened for specs, and the update that
 * runs it all. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/font.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <string.h>

/* The arena of a frame's text: a chunk's size. */
#define TB_GUI_ARENA_CHUNK (64u * 1024u)
/* Slots, kinds, and fonts a GUI first holds. */
#define TB_GUI_FIRST_CAPACITY 16u
/* Layouts an update runs at most after its events, while resize callbacks
 * keep changing sizes. */
#define TB_GUI_MOST_SETTLING_PASSES 4

/* A capacity doubled, or a first one. */
static size_t tb_gui_grown(size_t capacity)
{
    return capacity == 0 ? TB_GUI_FIRST_CAPACITY : capacity * 2;
}

/* ---------------------------------------------------------------------------
 * Text a widget owns.
 * ------------------------------------------------------------------------- */

void tb_gui_string_set(tb_gui_string *string, tb_text text)
{
    char *const bytes = tb_malloc(text.length + 1);

    if (text.length > 0)
    {
        memcpy(bytes, text.bytes, text.length);
    }
    bytes[text.length] = '\0';
    tb_free(string->bytes);
    string->bytes = bytes;
    string->length = text.length;
}

void tb_gui_string_free(tb_gui_string *string)
{
    tb_free(string->bytes);
    *string = (tb_gui_string){0};
}

tb_text tb_gui_string_text(const tb_gui_string *string)
{
    if (string->bytes == NULL)
    {
        return tb_text_make("", 0);
    }
    return tb_text_make(string->bytes, string->length);
}

/* ---------------------------------------------------------------------------
 * Slots and handles.
 * ------------------------------------------------------------------------- */

tb_gui_node *tb_gui_node_at(tb_gui *gui, uint32_t index)
{
    TB_ASSERT((size_t)index < gui->node_count);
    return &gui->nodes[index];
}

tb_gui_node *tb_gui_node_of(tb_widget widget)
{
    tb_gui_node *node = NULL;

    if (widget.gui == NULL || widget.generation == 0 ||
        (size_t)widget.index >= widget.gui->node_count)
    {
        return NULL;
    }
    node = &widget.gui->nodes[widget.index];
    if (!node->alive || node->generation != widget.generation)
    {
        return NULL;
    }
    return node;
}

tb_widget tb_gui_handle(tb_gui *gui, uint32_t index)
{
    if (index == TB_GUI_NO_NODE)
    {
        return TB_WIDGET_NONE;
    }
    return (tb_widget){gui, index, tb_gui_node_at(gui, index)->generation};
}

bool tb_widget_alive(tb_widget widget)
{
    return tb_gui_node_of(widget) != NULL;
}

bool tb_widget_equal(tb_widget a, tb_widget b)
{
    return a.gui == b.gui && a.index == b.index && a.generation == b.generation;
}

bool tb_gui_node_inside(tb_gui *gui, uint32_t index, uint32_t ancestor)
{
    for (uint32_t at = index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        if (at == ancestor)
        {
            return true;
        }
    }
    return false;
}

bool tb_gui_node_shown_enabled(tb_gui *gui, uint32_t index)
{
    if (!tb_gui_node_at(gui, index)->shown)
    {
        return false;
    }
    for (uint32_t at = index; at != TB_GUI_NO_NODE; at = tb_gui_node_at(gui, at)->parent)
    {
        if (!tb_gui_node_at(gui, at)->enabled)
        {
            return false;
        }
    }
    return true;
}

void tb_gui_mark_layout(tb_gui *gui)
{
    gui->layout_dirty = true;
}

static uint32_t tb_gui_take_slot(tb_gui *gui)
{
    if (gui->free_count > 0)
    {
        gui->free_count--;
        return gui->free_slots[gui->free_count];
    }
    if (gui->node_count == gui->node_capacity)
    {
        gui->node_capacity = tb_gui_grown(gui->node_capacity);
        gui->nodes = tb_realloc_array(gui->nodes, gui->node_capacity, sizeof(*gui->nodes));
    }
    TB_ASSERT_MSG(gui->node_count < TB_GUI_NO_NODE, "too many widgets");
    gui->nodes[gui->node_count] = (tb_gui_node){.generation = 1};
    gui->node_count++;
    return (uint32_t)(gui->node_count - 1);
}

static void tb_gui_push_slot(uint32_t **slots, size_t *count, size_t *capacity, uint32_t slot)
{
    if (*count == *capacity)
    {
        *capacity = tb_gui_grown(*capacity);
        *slots = tb_realloc_array(*slots, *capacity, sizeof(**slots));
    }
    (*slots)[*count] = slot;
    (*count)++;
}

/* A slot's widget made of a kind, unlinked. */
static void tb_gui_fill_slot(tb_gui *gui, uint32_t index, const tb_widget_class *kind)
{
    tb_gui_node *const node = tb_gui_node_at(gui, index);
    const uint32_t generation = node->generation;

    *node = (tb_gui_node){.generation = generation,
                          .in_use = true,
                          .alive = true,
                          .kind = kind,
                          .parent = TB_GUI_NO_NODE,
                          .first_child = TB_GUI_NO_NODE,
                          .last_child = TB_GUI_NO_NODE,
                          .next = TB_GUI_NO_NODE,
                          .previous = TB_GUI_NO_NODE,
                          .visible = true,
                          .enabled = true,
                          .cursor = TB_GUI_CURSOR_INHERIT,
                          .mouse_filter = kind->mouse_filter,
                          .grid_anchor = TB_GUI_ANCHOR_NW};
    if (kind->data_size > 0)
    {
        node->data = tb_calloc(1, kind->data_size);
    }
    gui->alive_count++;
}

static void tb_gui_link_last(tb_gui *gui, uint32_t parent, uint32_t child)
{
    tb_gui_node *const owner = tb_gui_node_at(gui, parent);
    tb_gui_node *const node = tb_gui_node_at(gui, child);

    node->parent = parent;
    node->previous = owner->last_child;
    node->next = TB_GUI_NO_NODE;
    if (owner->last_child != TB_GUI_NO_NODE)
    {
        tb_gui_node_at(gui, owner->last_child)->next = child;
    }
    else
    {
        owner->first_child = child;
    }
    owner->last_child = child;
}

static void tb_gui_unlink(tb_gui *gui, uint32_t child)
{
    tb_gui_node *const node = tb_gui_node_at(gui, child);
    tb_gui_node *const owner = tb_gui_node_at(gui, node->parent);

    if (node->previous != TB_GUI_NO_NODE)
    {
        tb_gui_node_at(gui, node->previous)->next = node->next;
    }
    else
    {
        owner->first_child = node->next;
    }
    if (node->next != TB_GUI_NO_NODE)
    {
        tb_gui_node_at(gui, node->next)->previous = node->previous;
    }
    else
    {
        owner->last_child = node->previous;
    }
    node->parent = TB_GUI_NO_NODE;
    node->next = TB_GUI_NO_NODE;
    node->previous = TB_GUI_NO_NODE;
}

void tb_gui_move_before(tb_gui *gui, uint32_t child, uint32_t sibling)
{
    tb_gui_node *const anchor = tb_gui_node_at(gui, sibling);
    const uint32_t parent = anchor->parent;
    tb_gui_node *node = NULL;

    TB_ASSERT(child != sibling && tb_gui_node_at(gui, child)->parent == parent);
    tb_gui_unlink(gui, child);
    node = tb_gui_node_at(gui, child);
    node->parent = parent;
    node->next = sibling;
    node->previous = anchor->previous;
    if (anchor->previous != TB_GUI_NO_NODE)
    {
        tb_gui_node_at(gui, anchor->previous)->next = child;
    }
    else
    {
        tb_gui_node_at(gui, parent)->first_child = child;
    }
    anchor->previous = child;
    tb_gui_mark_layout(gui);
}

/* Release the memory of a slot whose widget was destroyed, and free it. */
static void tb_gui_release(tb_gui *gui, uint32_t index)
{
    tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_widget_class *const kind = node->kind;
    const tb_widget stale = {gui, index, node->generation};

    if (kind->destroy != NULL)
    {
        kind->destroy(stale, node->data);
    }
    for (size_t property = 0; property < kind->property_count; property++)
    {
        const tb_gui_property *const described = &kind->properties[property];
        if (described->type == TB_GUI_TYPE_TEXT && described->get == NULL && described->set == NULL)
        {
            tb_gui_string_free((tb_gui_string *)((char *)node->data + described->offset));
        }
    }
    tb_free(node->data);
    tb_gui_string_free(&node->name);
    tb_gui_string_free(&node->style);
    tb_free(node->bindings);
    tb_gui_axis_free(&node->axes[TB_GUI_COLUMNS]);
    tb_gui_axis_free(&node->axes[TB_GUI_ROWS]);
    *node = (tb_gui_node){.generation = node->generation};
    tb_gui_push_slot(&gui->free_slots, &gui->free_count, &gui->free_capacity, index);
}

/* Mark a widget and its children destroyed, children first: their handles
 * go stale now, and their memory is released now or after the events. */
static void tb_gui_kill(tb_gui *gui, uint32_t index)
{
    uint32_t child = tb_gui_node_at(gui, index)->first_child;

    while (child != TB_GUI_NO_NODE)
    {
        const uint32_t next = tb_gui_node_at(gui, child)->next;
        tb_gui_kill(gui, child);
        child = next;
    }
    tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_widget widget = {gui, index, node->generation};
    node->alive = false;
    node->generation = node->generation == UINT32_MAX ? 1u : node->generation + 1u;
    gui->alive_count--;
    tb_gui_forget_widget(gui, widget);
    if (gui->dispatch_depth > 0)
    {
        tb_gui_push_slot(&gui->deferred, &gui->deferred_count, &gui->deferred_capacity, index);
        return;
    }
    tb_gui_release(gui, index);
}

void tb_gui_flush_deferred(tb_gui *gui)
{
    for (size_t index = 0; index < gui->deferred_count; index++)
    {
        tb_gui_release(gui, gui->deferred[index]);
    }
    gui->deferred_count = 0;
}

/* ---------------------------------------------------------------------------
 * GUIs.
 * ------------------------------------------------------------------------- */

tb_gui *tb_gui_create(const tb_gui_options *options)
{
    static const tb_gui_options defaults = {0};
    const tb_gui_options *const chosen = options != NULL ? options : &defaults;
    tb_gui *const gui = TB_NEW(tb_gui);
    uint32_t root = 0;

    gui->theme = chosen->theme != NULL ? *chosen->theme : tb_ui_theme_dark();
    gui->catalog = chosen->catalog;
    gui->layer = chosen->layer != 0 ? chosen->layer : TB_GUI_DEFAULT_LAYER;
    gui->reference_size = chosen->reference_size;
    gui->stretch = chosen->stretch;
    gui->zoom = chosen->scale > 0.0f ? chosen->scale : 1.0f;
    gui->display_scale = 1.0f;
    gui->cursor = TB_CURSOR_ARROW;
    gui->arena = tb_arena_create(TB_GUI_ARENA_CHUNK);
    gui->paint.arena = gui->arena;
    gui->paint.scale = 1.0f;
    gui->view = tb_gui_compute_view(gui);
    tb_gui_navigation_create(gui);
    tb_gui_register_builtins(gui);

    root = tb_gui_take_slot(gui);
    TB_ASSERT(root == 0);
    tb_gui_fill_slot(gui, root, tb_frame_class());
    tb_gui_node_at(gui, root)->mouse_filter = TB_GUI_MOUSE_PASS;
    tb_gui_node_at(gui, root)->cursor = TB_CURSOR_ARROW;
    tb_gui_mark_layout(gui);
    return gui;
}

void tb_gui_destroy(tb_gui *gui)
{
    if (gui == NULL)
    {
        return;
    }
    tb_gui_flush_deferred(gui);
    tb_gui_watches_free(gui);
    tb_gui_names_free(gui);
    tb_gui_vars_free(gui);
    tb_gui_navigation_free(gui);
    for (size_t index = 0; index < gui->node_count; index++)
    {
        if (gui->nodes[index].in_use)
        {
            tb_gui_release(gui, (uint32_t)index);
        }
    }
    for (size_t index = 0; index < gui->font_count; index++)
    {
        tb_font_close(gui->fonts[index].font);
    }
    tb_free(gui->fonts);
    tb_free(gui->nodes);
    tb_free(gui->free_slots);
    tb_free(gui->deferred);
    tb_free(gui->classes);
    tb_free(gui->resized);
    tb_free(gui->queue);
    tb_free(gui->modals);
    tb_free(gui->popups);
    tb_ui_paint_free(&gui->paint);
    tb_arena_destroy(gui->arena);
    tb_free(gui);
}

void tb_gui_set_theme(tb_gui *gui, const tb_ui_theme *theme)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(theme != NULL);
    gui->theme = *theme;
    tb_gui_mark_layout(gui);
}

const tb_ui_theme *tb_gui_theme(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return &gui->theme;
}

static bool tb_gui_views_equal(tb_gui_view a, tb_gui_view b)
{
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.scale == b.scale &&
           a.size.x == b.size.x && a.size.y == b.size.y;
}

/* Work out the view again, and lay out again when it changed. */
static void tb_gui_update_view(tb_gui *gui)
{
    const tb_gui_view view = tb_gui_compute_view(gui);

    if (!tb_gui_views_equal(view, gui->view))
    {
        gui->view = view;
        tb_gui_mark_layout(gui);
    }
}

void tb_gui_resize(tb_gui *gui, uint32_t width, uint32_t height, float display_scale)
{
    TB_ASSERT(gui != NULL);
    gui->frame_width = width;
    gui->frame_height = height;
    gui->display_scale = display_scale > 0.0f ? display_scale : 1.0f;
    tb_gui_update_view(gui);
}

void tb_gui_set_stretch(tb_gui *gui, tb_vec2 reference_size, tb_gui_stretch stretch)
{
    TB_ASSERT(gui != NULL);
    gui->reference_size = reference_size;
    gui->stretch = stretch;
    tb_gui_update_view(gui);
}

void tb_gui_set_scale(tb_gui *gui, float scale)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(scale > 0.0f);
    gui->zoom = scale;
    tb_gui_update_view(gui);
}

tb_gui_view tb_gui_view_of(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->view;
}

void tb_gui_update(tb_gui *gui, double delta_seconds)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT_MSG(gui->dispatch_depth == 0, "a GUI updated from one of its callbacks");
    gui->time += delta_seconds;
    tb_gui_watches_poll(gui);
    tb_gui_vars_update(gui);
    tb_gui_layout(gui);
    tb_gui_dispatch_resized(gui);
    tb_gui_process_input(gui);
    tb_gui_vars_update(gui);
    for (int pass = 0; pass < TB_GUI_MOST_SETTLING_PASSES && gui->layout_dirty; pass++)
    {
        tb_gui_layout(gui);
        tb_gui_dispatch_resized(gui);
    }
    tb_gui_refresh_hover(gui);
    tb_gui_finish_input(gui);
    tb_gui_update_tooltips(gui);
    tb_gui_build_commands(gui);
}

tb_widget tb_gui_root(tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return (tb_widget){gui, 0, gui->nodes[0].generation};
}

void tb_gui_stats_of(const tb_gui *gui, tb_gui_stats *stats)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(stats != NULL);
    *stats = (tb_gui_stats){
        .widgets = gui->alive_count,
        .layouts = gui->layouts,
        .commands = gui->paint.count,
        .hovered = gui->hovered,
        .pressed = gui->pressed,
        .focus = gui->focus,
        .modal = gui->modal_count > 0 ? gui->modals[gui->modal_count - 1] : TB_WIDGET_NONE,
        .popup = gui->popup_count > 0 ? gui->popups[gui->popup_count - 1].widget : TB_WIDGET_NONE};
}

/* ---------------------------------------------------------------------------
 * Kinds.
 * ------------------------------------------------------------------------- */

bool tb_gui_register_class(tb_gui *gui, const tb_widget_class *kind)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(kind != NULL);
    TB_ASSERT(kind->name != NULL);
    if (tb_gui_find_class(gui, kind->name) != NULL)
    {
        return false;
    }
    if (gui->class_count == gui->class_capacity)
    {
        gui->class_capacity = tb_gui_grown(gui->class_capacity);
        gui->classes =
            tb_realloc_array((void *)gui->classes, gui->class_capacity, sizeof(*gui->classes));
    }
    gui->classes[gui->class_count] = kind;
    gui->class_count++;
    return true;
}

const tb_widget_class *tb_gui_find_class(const tb_gui *gui, const char *name)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(name != NULL);
    for (size_t index = 0; index < gui->class_count; index++)
    {
        if (tb_str_equal(gui->classes[index]->name, name))
        {
            return gui->classes[index];
        }
    }
    return NULL;
}

size_t tb_gui_class_count(const tb_gui *gui)
{
    TB_ASSERT(gui != NULL);
    return gui->class_count;
}

const tb_widget_class *tb_gui_class_at(const tb_gui *gui, size_t index)
{
    TB_ASSERT(gui != NULL);
    return index < gui->class_count ? gui->classes[index] : NULL;
}

/* ---------------------------------------------------------------------------
 * The tree.
 * ------------------------------------------------------------------------- */

tb_widget tb_widget_create(tb_widget parent, const tb_widget_class *kind)
{
    tb_gui *const gui = parent.gui;
    uint32_t index = 0;
    tb_widget widget = TB_WIDGET_NONE;

    TB_ASSERT(kind != NULL);
    if (tb_gui_node_of(parent) == NULL)
    {
        return TB_WIDGET_NONE;
    }
    index = tb_gui_take_slot(gui);
    tb_gui_fill_slot(gui, index, kind);
    tb_gui_link_last(gui, parent.index, index);
    widget = tb_gui_handle(gui, index);
    if (kind->init != NULL)
    {
        kind->init(widget, tb_gui_node_at(gui, index)->data);
    }
    tb_gui_mark_layout(gui);
    return widget;
}

void tb_widget_destroy(tb_widget widget)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    tb_gui *const gui = widget.gui;

    if (node == NULL)
    {
        return;
    }
    if (node->parent == TB_GUI_NO_NODE)
    {
        while (tb_gui_node_at(gui, widget.index)->first_child != TB_GUI_NO_NODE)
        {
            const uint32_t child = tb_gui_node_at(gui, widget.index)->first_child;
            tb_gui_unlink(gui, child);
            tb_gui_kill(gui, child);
        }
        tb_gui_mark_layout(gui);
        return;
    }
    tb_gui_unlink(gui, widget.index);
    tb_gui_kill(gui, widget.index);
    tb_gui_mark_layout(gui);
}

tb_widget tb_widget_parent(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? tb_gui_handle(widget.gui, node->parent) : TB_WIDGET_NONE;
}

tb_widget tb_widget_first_child(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? tb_gui_handle(widget.gui, node->first_child) : TB_WIDGET_NONE;
}

tb_widget tb_widget_next_sibling(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? tb_gui_handle(widget.gui, node->next) : TB_WIDGET_NONE;
}

size_t tb_widget_child_count(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    size_t count = 0;

    if (node == NULL)
    {
        return 0;
    }
    for (uint32_t child = node->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(widget.gui, child)->next)
    {
        count++;
    }
    return count;
}

const tb_widget_class *tb_widget_class_of(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? node->kind : NULL;
}

void *tb_widget_data(tb_widget widget)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? node->data : NULL;
}

tb_canvas_rect tb_widget_rect(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL && node->shown ? node->rect : (tb_canvas_rect){0};
}

tb_vec2 tb_widget_natural_size(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    return node != NULL ? node->natural : (tb_vec2){0};
}

uint32_t tb_widget_state(tb_widget widget)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);
    tb_gui *const gui = widget.gui;
    uint32_t state = 0;

    if (node == NULL)
    {
        return 0;
    }
    if (!tb_gui_node_shown_enabled(gui, widget.index))
    {
        return TB_WIDGET_DISABLED;
    }
    if (tb_widget_equal(gui->hovered, widget))
    {
        state |= TB_WIDGET_HOVERED;
    }
    if (tb_widget_equal(gui->pressed, widget))
    {
        state |= TB_WIDGET_PRESSED;
    }
    if (tb_widget_equal(gui->focus, widget))
    {
        state |= TB_WIDGET_FOCUSED;
        state |= gui->focus_visible ? TB_WIDGET_FOCUS_VISIBLE : 0u;
        state |= gui->focus_engaged ? TB_WIDGET_ENGAGED : 0u;
    }
    return state;
}

/* ---------------------------------------------------------------------------
 * Fonts.
 * ------------------------------------------------------------------------- */

static bool tb_gui_same_face(tb_font_spec a, tb_font_spec b)
{
    return a.role == b.role && a.weight == b.weight && a.italic == b.italic;
}

/* The font of a spec from the catalog, opened once; NULL when it has none. */
static const tb_font *tb_gui_catalog_font(tb_gui *gui, tb_font_spec spec)
{
    size_t found = 0;
    tb_font *font = NULL;

    for (size_t index = 0; index < gui->font_count; index++)
    {
        if (tb_gui_same_face(gui->fonts[index].spec, spec))
        {
            return gui->fonts[index].font;
        }
    }
    if (spec.role < TB_FONT_ROLE_COUNT && tb_font_catalog_find_spec(gui->catalog, spec, &found) &&
        !tb_font_catalog_open(gui->catalog, found, NULL, &font))
    {
        font = NULL;
    }
    if (gui->font_count == gui->font_capacity)
    {
        gui->font_capacity = tb_gui_grown(gui->font_capacity);
        gui->fonts = tb_realloc_array(gui->fonts, gui->font_capacity, sizeof(*gui->fonts));
    }
    gui->fonts[gui->font_count] = (tb_gui_font_entry){.spec = spec, .font = font};
    gui->font_count++;
    return font;
}

tb_gui_resolved_font tb_gui_resolve_font(tb_gui *gui, tb_font_spec spec)
{
    tb_gui_resolved_font resolved = {.font = gui->theme.fonts[TB_UI_FONT_REGULAR],
                                     .size = spec.size > 0.0f ? spec.size : gui->theme.font_size};

    if (spec.weight != 0 && gui->catalog != NULL)
    {
        const tb_font *const font = tb_gui_catalog_font(gui, spec);
        if (font != NULL)
        {
            resolved.font = font;
        }
    }
    return resolved;
}

void tb_gui_forget_fonts(const tb_gui *gui, tb_renderer *renderer)
{
    TB_ASSERT(gui != NULL);
    TB_ASSERT(renderer != NULL);
    for (size_t index = 0; index < gui->font_count; index++)
    {
        if (gui->fonts[index].font != NULL)
        {
            tb_canvas_forget_font(renderer, gui->fonts[index].font);
        }
    }
}
