/* The treeview kind, after Tk's `ttk.Treeview`: items in a hierarchy, each
 * with its text and a value for each column, opened and closed, chosen by
 * the mouse and the keyboard, under headings whose widths are dragged.
 * Items are data with ids, not widgets; the rows of the items open are
 * kept in a list, rebuilt when the tree changes, and only those seen are
 * drawn. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Units wide the tree's column and the others are, unless chosen. */
#define TB_GUI_TREE_COLUMN_WIDTH 200.0f
#define TB_GUI_VALUE_COLUMN_WIDTH 100.0f
/* The least a column is dragged to, unless chosen. */
#define TB_GUI_COLUMN_MINIMUM 20.0f
/* Units either side of a heading's edge that take the mouse to resize it. */
#define TB_GUI_HEADING_GRIP 4.0f
/* Rows a notch of the wheel scrolls. */
#define TB_GUI_TREE_WHEEL_ROWS 3.0f
/* Items, columns, and rows a tree first holds. */
#define TB_GUI_TREE_FIRST 16u
/* The part of a row an open or closed item's triangle takes. */
#define TB_GUI_TREE_MARK 0.25f

static const char *const tb_gui_tree_selectmode_names[] = {"browse", "single", "multiple",
                                                           "extended"};

typedef struct tb_gui_tree_node
{
    bool alive;
    tb_tree_item parent;
    tb_tree_item first_child;
    tb_tree_item last_child;
    tb_tree_item next;
    tb_tree_item previous;
    tb_gui_string text;
    tb_gui_string *values;
    size_t value_count;
    bool open;
    bool selected;
} tb_gui_tree_node;

typedef struct tb_gui_tree_column
{
    tb_gui_string heading;
    float width;
    float minwidth;
} tb_gui_tree_column;

typedef struct tb_gui_treeview
{
    tb_gui_tree_node *items; /* by id; the first is the root */
    size_t count;
    size_t capacity;
    tb_gui_tree_column *columns; /* the first is the tree's */
    size_t column_count;
    size_t column_capacity;
    /* The rows: the items whose ancestors are open, in order, and their depths. */
    tb_tree_item *rows;
    size_t *depths;
    size_t row_count;
    size_t row_capacity;
    bool rows_stale;
    int32_t selectmode;
    int32_t height;
    bool no_headings;
    tb_tree_item focus;
    tb_tree_item anchor;
    float offset;
    tb_gui_command command;
    tb_gui_command open_command;
    /* As last arranged. */
    tb_canvas_rect headings;
    tb_canvas_rect view;
    tb_canvas_rect track;
    bool bar;
    /* The mouse. */
    size_t resizing; /* the column whose edge is dragged, or SIZE_MAX */
    bool dragging_bar;
    float grab;
    tb_tree_item hot;
    bool selecting;
} tb_gui_treeview;

static tb_gui_treeview *tb_gui_treeview_of(tb_widget widget);

/* ---------------------------------------------------------------------------
 * Items.
 * ------------------------------------------------------------------------- */

static bool tb_gui_tree_has(const tb_gui_treeview *tree, tb_tree_item item)
{
    return (size_t)item < tree->count && tree->items[item].alive;
}

static void tb_gui_tree_free_node(tb_gui_tree_node *node)
{
    tb_gui_string_free(&node->text);
    for (size_t index = 0; index < node->value_count; index++)
    {
        tb_gui_string_free(&node->values[index]);
    }
    tb_free(node->values);
    *node = (tb_gui_tree_node){0};
}

static tb_tree_item tb_gui_tree_new_node(tb_gui_treeview *tree)
{
    if (tree->count == tree->capacity)
    {
        tree->capacity = tree->capacity == 0 ? TB_GUI_TREE_FIRST : tree->capacity * 2;
        tree->items = tb_realloc_array(tree->items, tree->capacity, sizeof(*tree->items));
    }
    TB_ASSERT_MSG(tree->count < UINT32_MAX, "too many items in a tree");
    tree->items[tree->count] = (tb_gui_tree_node){.alive = true};
    tree->count++;
    return (tb_tree_item)(tree->count - 1);
}

static void tb_gui_tree_set_value(tb_gui_tree_node *node, size_t column, tb_text text)
{
    if (column >= node->value_count)
    {
        node->values = tb_realloc_array(node->values, column + 1, sizeof(*node->values));
        for (size_t added = node->value_count; added <= column; added++)
        {
            node->values[added] = (tb_gui_string){0};
        }
        node->value_count = column + 1;
    }
    tb_gui_string_set(&node->values[column], text);
}

/* Link an item as its parent's child before the child at `index`. */
static void tb_gui_tree_link(tb_gui_treeview *tree, tb_tree_item parent, size_t index,
                             tb_tree_item item)
{
    tb_tree_item before = tree->items[parent].first_child;

    for (size_t at = 0; at < index && before != TB_TREE_ROOT; at++)
    {
        before = tree->items[before].next;
    }
    tb_gui_tree_node *const node = &tree->items[item];
    node->parent = parent;
    node->next = before;
    node->previous =
        before != TB_TREE_ROOT ? tree->items[before].previous : tree->items[parent].last_child;
    if (node->previous != TB_TREE_ROOT)
    {
        tree->items[node->previous].next = item;
    }
    else
    {
        tree->items[parent].first_child = item;
    }
    if (before != TB_TREE_ROOT)
    {
        tree->items[before].previous = item;
    }
    else
    {
        tree->items[parent].last_child = item;
    }
}

static void tb_gui_tree_unlink(tb_gui_treeview *tree, tb_tree_item item)
{
    tb_gui_tree_node *const node = &tree->items[item];
    tb_gui_tree_node *const parent = &tree->items[node->parent];

    if (node->previous != TB_TREE_ROOT)
    {
        tree->items[node->previous].next = node->next;
    }
    else
    {
        parent->first_child = node->next;
    }
    if (node->next != TB_TREE_ROOT)
    {
        tree->items[node->next].previous = node->previous;
    }
    else
    {
        parent->last_child = node->previous;
    }
}

/* Free an item and every item in it. */
static void tb_gui_tree_kill(tb_gui_treeview *tree, tb_tree_item item)
{
    tb_tree_item child = tree->items[item].first_child;

    while (child != TB_TREE_ROOT)
    {
        const tb_tree_item next = tree->items[child].next;
        tb_gui_tree_kill(tree, child);
        child = next;
    }
    tb_gui_tree_free_node(&tree->items[item]);
}

static void tb_gui_tree_changed(tb_widget widget, tb_gui_treeview *tree)
{
    tree->rows_stale = true;
    tb_gui_mark_layout(widget.gui);
}

static void tb_gui_tree_push_row(tb_gui_treeview *tree, tb_tree_item item, size_t depth)
{
    if (tree->row_count == tree->row_capacity)
    {
        tree->row_capacity = tree->row_capacity == 0 ? TB_GUI_TREE_FIRST : tree->row_capacity * 2;
        tree->rows = tb_realloc_array(tree->rows, tree->row_capacity, sizeof(*tree->rows));
        tree->depths = tb_realloc_array(tree->depths, tree->row_capacity, sizeof(*tree->depths));
    }
    tree->rows[tree->row_count] = item;
    tree->depths[tree->row_count] = depth;
    tree->row_count++;
}

/* The rows: each item whose ancestors are all open, in the order of the tree. */
static void tb_gui_tree_rows(tb_gui_treeview *tree)
{
    tb_tree_item at = tree->items[TB_TREE_ROOT].first_child;
    size_t depth = 0;

    if (!tree->rows_stale)
    {
        return;
    }
    tree->rows_stale = false;
    tree->row_count = 0;
    while (at != TB_TREE_ROOT)
    {
        const tb_gui_tree_node *const node = &tree->items[at];
        tb_gui_tree_push_row(tree, at, depth);
        if (node->open && node->first_child != TB_TREE_ROOT)
        {
            at = node->first_child;
            depth++;
            continue;
        }
        /* Up to the nearest ancestor with a next sibling. */
        while (at != TB_TREE_ROOT && tree->items[at].next == TB_TREE_ROOT)
        {
            at = tree->items[at].parent;
            depth = depth > 0 ? depth - 1 : 0;
        }
        at = at != TB_TREE_ROOT ? tree->items[at].next : TB_TREE_ROOT;
    }
}

static size_t tb_gui_tree_row_of(tb_gui_treeview *tree, tb_tree_item item)
{
    tb_gui_tree_rows(tree);
    for (size_t row = 0; row < tree->row_count; row++)
    {
        if (tree->rows[row] == item)
        {
            return row;
        }
    }
    return SIZE_MAX;
}

tb_tree_item tb_treeview_insert(tb_widget treeview, tb_tree_item parent, size_t index,
                                const tb_tree_item_desc *desc)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    const tb_tree_item_desc chosen = desc != NULL ? *desc : (tb_tree_item_desc){0};

    TB_ASSERT(chosen.values != NULL || chosen.value_count == 0);
    if (tree == NULL || !tb_gui_tree_has(tree, parent))
    {
        return TB_TREE_ROOT;
    }
    const tb_tree_item item = tb_gui_tree_new_node(tree);
    tb_gui_tree_node *const node = &tree->items[item];
    tb_gui_string_set(&node->text, tb_text_from_cstr(chosen.text != NULL ? chosen.text : ""));
    node->open = chosen.open;
    for (size_t column = 0; column < chosen.value_count; column++)
    {
        const char *const value = chosen.values[column];
        tb_gui_tree_set_value(node, column, tb_text_from_cstr(value != NULL ? value : ""));
    }
    tb_gui_tree_link(tree, parent, index, item);
    tb_gui_tree_changed(treeview, tree);
    return item;
}

bool tb_treeview_delete(tb_widget treeview, tb_tree_item item)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);

    if (tree == NULL || item == TB_TREE_ROOT || !tb_gui_tree_has(tree, item))
    {
        return false;
    }
    tb_gui_tree_unlink(tree, item);
    tb_gui_tree_kill(tree, item);
    if (!tb_gui_tree_has(tree, tree->focus))
    {
        tree->focus = TB_TREE_ROOT;
    }
    if (!tb_gui_tree_has(tree, tree->anchor))
    {
        tree->anchor = TB_TREE_ROOT;
    }
    tree->hot = TB_TREE_ROOT;
    tb_gui_tree_changed(treeview, tree);
    return true;
}

bool tb_treeview_exists(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    return tree != NULL && item != TB_TREE_ROOT && tb_gui_tree_has(tree, item);
}

/* An item alive, or the root; NULL otherwise. */
static tb_gui_tree_node *tb_gui_tree_node_of(tb_widget treeview, tb_tree_item item)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    return tree != NULL && tb_gui_tree_has(tree, item) ? &tree->items[item] : NULL;
}

tb_tree_item tb_treeview_parent(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);
    return node != NULL ? node->parent : TB_TREE_ROOT;
}

tb_tree_item tb_treeview_first_child(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);
    return node != NULL ? node->first_child : TB_TREE_ROOT;
}

tb_tree_item tb_treeview_next_sibling(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);
    return node != NULL && item != TB_TREE_ROOT ? node->next : TB_TREE_ROOT;
}

tb_text tb_treeview_text(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);
    return node != NULL ? tb_gui_string_text(&node->text) : tb_text_make("", 0);
}

bool tb_treeview_set_text(tb_widget treeview, tb_tree_item item, const char *text)
{
    tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);

    TB_ASSERT(text != NULL);
    if (node == NULL || item == TB_TREE_ROOT)
    {
        return false;
    }
    tb_gui_string_set(&node->text, tb_text_from_cstr(text));
    return true;
}

tb_text tb_treeview_value(tb_widget treeview, tb_tree_item item, size_t column)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);

    if (node == NULL || column >= node->value_count)
    {
        return tb_text_make("", 0);
    }
    return tb_gui_string_text(&node->values[column]);
}

bool tb_treeview_set_value(tb_widget treeview, tb_tree_item item, size_t column, const char *text)
{
    tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);

    TB_ASSERT(text != NULL);
    if (node == NULL || item == TB_TREE_ROOT || column >= (size_t)UINT32_MAX)
    {
        return false;
    }
    tb_gui_tree_set_value(node, column, tb_text_from_cstr(text));
    return true;
}

bool tb_treeview_set_open(tb_widget treeview, tb_tree_item item, bool open)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);

    if (tree == NULL || item == TB_TREE_ROOT || !tb_gui_tree_has(tree, item))
    {
        return false;
    }
    if (tree->items[item].open != open)
    {
        tree->items[item].open = open;
        tb_gui_tree_changed(treeview, tree);
    }
    return true;
}

bool tb_treeview_is_open(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);
    return node != NULL && node->open;
}

/* ---------------------------------------------------------------------------
 * The selection.
 * ------------------------------------------------------------------------- */

static bool tb_gui_tree_select_rows(tb_gui_treeview *tree, size_t from, size_t to)
{
    const size_t low = from < to ? from : to;
    const size_t high = from < to ? to : from;
    const size_t wanted = high - low + 1;
    size_t chosen_before = 0;
    size_t chosen_in_range = 0;

    for (size_t row = low; row <= high && row < tree->row_count; row++)
    {
        chosen_in_range += tree->items[tree->rows[row]].selected ? 1u : 0u;
    }
    for (size_t item = 1; item < tree->count; item++)
    {
        chosen_before += tree->items[item].selected ? 1u : 0u;
        tree->items[item].selected = false;
    }
    for (size_t row = low; row <= high && row < tree->row_count; row++)
    {
        tree->items[tree->rows[row]].selected = true;
    }
    /* Unchanged when the range alone was chosen before. */
    return chosen_before != wanted || chosen_in_range != wanted;
}

/* A row chosen by the mouse or a key, as the selection mode has it. */
static void tb_gui_tree_choose(tb_widget widget, tb_gui_treeview *tree, size_t row,
                               uint32_t modifiers, bool toggle)
{
    const tb_tree_item item = tree->rows[row];
    const bool shift = (modifiers & TB_MODIFIER_SHIFT) != 0;
    const bool control = (modifiers & TB_MODIFIER_CONTROL) != 0;
    const size_t anchor = tb_gui_tree_row_of(tree, tree->anchor);
    bool changed = false;

    tree->focus = item;
    if (tree->selectmode == TB_LISTBOX_MULTIPLE ||
        (tree->selectmode == TB_LISTBOX_EXTENDED && control && toggle))
    {
        tree->anchor = item;
        changed = toggle;
        tree->items[item].selected =
            toggle ? !tree->items[item].selected : tree->items[item].selected;
    }
    else if (tree->selectmode == TB_LISTBOX_EXTENDED && shift && anchor != SIZE_MAX)
    {
        changed = tb_gui_tree_select_rows(tree, anchor, row);
    }
    else
    {
        tree->anchor = item;
        changed = tb_gui_tree_select_rows(tree, row, row);
    }
    if (changed)
    {
        (void)tb_widget_invoke(widget);
    }
}

bool tb_treeview_select(tb_widget treeview, tb_tree_item item, bool selected)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    const bool many = tree != NULL && (tree->selectmode == TB_LISTBOX_MULTIPLE ||
                                       tree->selectmode == TB_LISTBOX_EXTENDED);

    if (tree == NULL || item == TB_TREE_ROOT || !tb_gui_tree_has(tree, item))
    {
        return false;
    }
    for (size_t at = 1; selected && !many && at < tree->count; at++)
    {
        tree->items[at].selected = false;
    }
    tree->items[item].selected = selected;
    return true;
}

void tb_treeview_clear_selection(tb_widget treeview)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);

    for (size_t item = 1; tree != NULL && item < tree->count; item++)
    {
        tree->items[item].selected = false;
    }
}

bool tb_treeview_selected(tb_widget treeview, tb_tree_item item)
{
    const tb_gui_tree_node *const node = tb_gui_tree_node_of(treeview, item);
    return node != NULL && node->selected;
}

size_t tb_treeview_selection(tb_widget treeview, tb_tree_item *items, size_t capacity)
{
    const tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    size_t found = 0;

    TB_ASSERT(items != NULL || capacity == 0);
    for (size_t item = 1; tree != NULL && item < tree->count; item++)
    {
        if (!tree->items[item].alive || !tree->items[item].selected)
        {
            continue;
        }
        if (found < capacity)
        {
            items[found] = (tb_tree_item)item;
        }
        found++;
    }
    return found;
}

tb_tree_item tb_treeview_focus_item(tb_widget treeview)
{
    const tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    return tree != NULL ? tree->focus : TB_TREE_ROOT;
}

bool tb_treeview_set_focus_item(tb_widget treeview, tb_tree_item item)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);

    if (tree == NULL || item == TB_TREE_ROOT || !tb_gui_tree_has(tree, item))
    {
        return false;
    }
    tree->focus = item;
    return true;
}

/* ---------------------------------------------------------------------------
 * Columns.
 * ------------------------------------------------------------------------- */

static size_t tb_gui_tree_add_column(tb_gui_treeview *tree, const char *heading, float width,
                                     float minwidth)
{
    if (tree->column_count == tree->column_capacity)
    {
        tree->column_capacity = tree->column_capacity == 0 ? 4u : tree->column_capacity * 2;
        tree->columns =
            tb_realloc_array(tree->columns, tree->column_capacity, sizeof(*tree->columns));
    }
    tb_gui_tree_column *const column = &tree->columns[tree->column_count];
    *column = (tb_gui_tree_column){.minwidth = minwidth > 0.0f ? minwidth : TB_GUI_COLUMN_MINIMUM};
    column->width = fmaxf(width, column->minwidth);
    tb_gui_string_set(&column->heading, tb_text_from_cstr(heading != NULL ? heading : ""));
    tree->column_count++;
    return tree->column_count - 1;
}

size_t tb_treeview_add_column(tb_widget treeview, const tb_treeview_column_desc *desc)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);
    const tb_treeview_column_desc chosen = desc != NULL ? *desc : (tb_treeview_column_desc){0};

    if (tree == NULL || !(chosen.width >= 0.0f) || !(chosen.minwidth >= 0.0f))
    {
        return SIZE_MAX;
    }
    const float width = chosen.width > 0.0f ? chosen.width : TB_GUI_VALUE_COLUMN_WIDTH;
    const size_t added = tb_gui_tree_add_column(tree, chosen.heading, width, chosen.minwidth);
    tb_gui_mark_layout(treeview.gui);
    /* The tree's own column is not counted. */
    return added - 1;
}

/* A column by its number to the caller: SIZE_MAX the tree's, else a value's. */
static tb_gui_tree_column *tb_gui_tree_column_at(tb_gui_treeview *tree, size_t column)
{
    const size_t index = column == TB_TREEVIEW_TREE_COLUMN ? 0 : column + 1;
    return tree != NULL && index < tree->column_count ? &tree->columns[index] : NULL;
}

float tb_treeview_column_width(tb_widget treeview, size_t column)
{
    const tb_gui_tree_column *const found =
        tb_gui_tree_column_at(tb_gui_treeview_of(treeview), column);
    return found != NULL ? found->width : 0.0f;
}

bool tb_treeview_set_column_width(tb_widget treeview, size_t column, float width)
{
    tb_gui_tree_column *const found = tb_gui_tree_column_at(tb_gui_treeview_of(treeview), column);

    if (found == NULL || !isfinite(width))
    {
        return false;
    }
    found->width = fmaxf(width, found->minwidth);
    tb_gui_mark_layout(treeview.gui);
    return true;
}

/* ---------------------------------------------------------------------------
 * Rows on the screen.
 * ------------------------------------------------------------------------- */

/* A tree's style: a list's box, rows, and bar, and its headings' and
 * marks'. Its padding is each row's and each heading's. */
enum
{
    TB_GUI_TREE_SELECTION,    /* a chosen row */
    TB_GUI_TREE_HOT,          /* the row under the mouse */
    TB_GUI_TREE_ACTIVE,       /* the ring of the focus item, with the focus */
    TB_GUI_TREE_HEADING,      /* the headings' background */
    TB_GUI_TREE_HEADING_TEXT, /* their text */
    TB_GUI_TREE_SEPARATOR,    /* the lines between headings */
    TB_GUI_TREE_INDENT,       /* units each level is indented */
    TB_GUI_TREE_MARK_WIDTH,   /* units the open and closed marks' lines are wide */
    TB_GUI_TREE_ELEMENTS = TB_GUI_TREE_MARK_WIDTH + 1 + TB_GUI_BAR_ELEMENTS
};

static const tb_style_element tb_gui_tree_elements[TB_GUI_TREE_ELEMENTS] = {
    [TB_GUI_TREE_SELECTION] = {"selection", TB_STYLE_COLOR},
    [TB_GUI_TREE_HOT] = {"hot_color", TB_STYLE_COLOR},
    [TB_GUI_TREE_ACTIVE] = {"active_color", TB_STYLE_COLOR},
    [TB_GUI_TREE_HEADING] = {"heading_background", TB_STYLE_COLOR},
    [TB_GUI_TREE_HEADING_TEXT] = {"heading_text_color", TB_STYLE_COLOR},
    [TB_GUI_TREE_SEPARATOR] = {"separator_color", TB_STYLE_COLOR},
    [TB_GUI_TREE_INDENT] = {"indent", TB_STYLE_NUMBER},
    [TB_GUI_TREE_MARK_WIDTH] = {"mark_width", TB_STYLE_NUMBER},
    TB_GUI_BAR_STYLE_ELEMENTS};

static const tb_widget_style_desc tb_gui_treeview_style = {.name = "Treeview",
                                                           .states = TB_WIDGET_FOCUSED |
                                                                     TB_WIDGET_DISABLED,
                                                           .elements = tb_gui_tree_elements,
                                                           .element_count = TB_GUI_TREE_ELEMENTS};

static float tb_gui_tree_row_height(tb_widget widget)
{
    const tb_style *const style = tb_style_in(widget, 0);
    return tb_gui_line_height(widget) + style->padding.top + style->padding.bottom;
}

static tb_gui_bar tb_gui_tree_bar(tb_widget widget, const tb_gui_treeview *tree)
{
    return (tb_gui_bar){.track = tree->track,
                        .vertical = true,
                        .offset = tree->offset,
                        .view = tree->view.height,
                        .content = (float)tree->row_count * tb_gui_tree_row_height(widget)};
}

static void tb_gui_tree_scroll(tb_widget widget, tb_gui_treeview *tree, float offset)
{
    const tb_gui_bar bar = tb_gui_tree_bar(widget, tree);
    tree->offset = tb_gui_bar_clamp(&bar, offset);
}

static tb_canvas_rect tb_gui_tree_row_rect(tb_widget widget, const tb_gui_treeview *tree,
                                           size_t row)
{
    const float height = tb_gui_tree_row_height(widget);
    return (tb_canvas_rect){tree->view.x, tree->view.y + (float)row * height - tree->offset,
                            tree->view.width, height};
}

static size_t tb_gui_tree_row_at(tb_widget widget, const tb_gui_treeview *tree, tb_vec2 point)
{
    const float height = tb_gui_tree_row_height(widget);

    if (!tb_ui_rect_contains(tree->view, point) || !(height > 0.0f))
    {
        return SIZE_MAX;
    }
    const float row = floorf((point.y - tree->view.y + tree->offset) / height);
    return row >= 0.0f && row < (float)tree->row_count ? (size_t)row : SIZE_MAX;
}

/* The left of a column, in units of the GUI. */
static float tb_gui_tree_column_left(const tb_gui_treeview *tree, size_t index)
{
    float left = tree->view.x;

    for (size_t at = 0; at < index; at++)
    {
        left += tree->columns[at].width;
    }
    return left;
}

/* The left of a row's open and closed mark, which its text follows. */
static float tb_gui_tree_mark_left(tb_widget widget, const tb_gui_treeview *tree, size_t row)
{
    return tree->view.x +
           (float)tree->depths[row] * tb_style_number(tb_style_in(widget, 0), TB_GUI_TREE_INDENT);
}

bool tb_treeview_see(tb_widget treeview, tb_tree_item item)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);

    if (tree == NULL || item == TB_TREE_ROOT || !tb_gui_tree_has(tree, item))
    {
        return false;
    }
    /* Its ancestors are opened, so that it has a row. */
    for (tb_tree_item at = tree->items[item].parent; at != TB_TREE_ROOT;
         at = tree->items[at].parent)
    {
        tree->rows_stale = tree->rows_stale || !tree->items[at].open;
        tree->items[at].open = true;
    }
    tb_gui_mark_layout(treeview.gui);
    tb_gui_layout(treeview.gui);
    const size_t row = tb_gui_tree_row_of(tree, item);
    const float height = tb_gui_tree_row_height(treeview);
    const float top = (float)row * height;
    if (top < tree->offset)
    {
        tb_gui_tree_scroll(treeview, tree, top);
    }
    else if (top + height > tree->offset + tree->view.height)
    {
        tb_gui_tree_scroll(treeview, tree, top + height - tree->view.height);
    }
    tb_gui_see_rect(treeview.gui, treeview.index, tb_gui_tree_row_rect(treeview, tree, row));
    return true;
}

tb_tree_item tb_treeview_identify_row(tb_widget treeview, float y)
{
    tb_gui_treeview *const tree = tb_gui_treeview_of(treeview);

    if (tree == NULL)
    {
        return TB_TREE_ROOT;
    }
    tb_gui_tree_rows(tree);
    const size_t row = tb_gui_tree_row_at(treeview, tree, (tb_vec2){tree->view.x, y});
    return row != SIZE_MAX ? tree->rows[row] : TB_TREE_ROOT;
}

/* ---------------------------------------------------------------------------
 * The kind's functions.
 * ------------------------------------------------------------------------- */

static void tb_gui_treeview_init(tb_widget widget, void *data)
{
    tb_gui_treeview *const tree = data;

    (void)widget;
    (void)tb_gui_tree_new_node(tree);
    tree->items[TB_TREE_ROOT].open = true;
    (void)tb_gui_tree_add_column(tree, "", TB_GUI_TREE_COLUMN_WIDTH, 0.0f);
    tree->resizing = SIZE_MAX;
    tree->rows_stale = true;
}

static void tb_gui_treeview_destroy(tb_widget widget, void *data)
{
    tb_gui_treeview *const tree = data;

    (void)widget;
    for (size_t item = 0; item < tree->count; item++)
    {
        tb_gui_tree_free_node(&tree->items[item]);
    }
    for (size_t column = 0; column < tree->column_count; column++)
    {
        tb_gui_string_free(&tree->columns[column].heading);
    }
    tb_free(tree->items);
    tb_free(tree->columns);
    tb_free(tree->rows);
    tb_free(tree->depths);
}

static tb_vec2 tb_gui_treeview_measure(tb_widget widget, const void *data)
{
    tb_gui_treeview *const tree = tb_widget_data(widget);
    const float border = tb_style_in(widget, 0)->border_width;
    const float row = tb_gui_tree_row_height(widget);
    float width = 0.0f;

    (void)data;
    tb_gui_tree_rows(tree);
    for (size_t column = 0; column < tree->column_count; column++)
    {
        width += tree->columns[column].width;
    }
    const size_t shown =
        tree->height > 0 ? (size_t)tree->height : (tree->row_count > 0 ? tree->row_count : 1u);
    const float headings = tree->no_headings ? 0.0f : row;
    const float bar = tree->row_count > shown ? tb_gui_bar_width(widget) : 0.0f;
    return (tb_vec2){width + bar + 2.0f * border, headings + (float)shown * row + 2.0f * border};
}

static void tb_gui_treeview_arrange(tb_widget widget, void *data, tb_canvas_rect rect)
{
    tb_gui_treeview *const tree = data;
    const float border = tb_style_in(widget, 0)->border_width;
    const float bar_width = tb_gui_bar_width(widget);
    const tb_canvas_rect inner = tb_ui_rect_inset(rect, border, border);
    const float row = tb_gui_tree_row_height(widget);
    const float headings = tree->no_headings ? 0.0f : row;

    tb_gui_tree_rows(tree);
    tree->headings = (tb_canvas_rect){inner.x, inner.y, inner.width, headings};
    tree->view = (tb_canvas_rect){inner.x, inner.y + headings, inner.width,
                                  fmaxf(inner.height - headings, 0.0f)};
    tree->bar = (float)tree->row_count * row > tree->view.height;
    if (tree->bar)
    {
        tree->view.width = fmaxf(inner.width - bar_width, 0.0f);
    }
    tree->track = (tb_canvas_rect){tree->view.x + tree->view.width, tree->view.y, bar_width,
                                   tree->view.height};
    tb_gui_tree_scroll(widget, tree, tree->offset);
}

static void tb_gui_treeview_draw_headings(const tb_gui_treeview *tree, tb_gui_paint *paint,
                                          const tb_style *style)
{
    tb_gui_paint headings = *paint;
    float left = tree->headings.x;

    headings.clip = tb_ui_rect_intersect(paint->clip, tree->headings);
    tb_gui_paint_fill(&headings, tree->headings, 0.0f, tb_style_color(style, TB_GUI_TREE_HEADING));
    for (size_t column = 0; column < tree->column_count; column++)
    {
        const float width = tree->columns[column].width;
        const tb_canvas_rect cell = {left, tree->headings.y, width, tree->headings.height};
        const tb_gui_text_block block = {.text = tb_gui_string_text(&tree->columns[column].heading),
                                         .anchor = TB_GUI_ANCHOR_W};
        tb_gui_paint_text(&headings, &block, tb_ui_rect_inset(cell, style->padding.left, 0.0f),
                          tb_style_color(style, TB_GUI_TREE_HEADING_TEXT));
        tb_gui_paint_line(&headings, (tb_vec2){left + width, cell.y},
                          (tb_vec2){left + width, cell.y + cell.height}, style->border_width,
                          tb_style_color(style, TB_GUI_TREE_SEPARATOR));
        left += width;
    }
}

static void tb_gui_treeview_draw_mark(tb_gui_paint *paint, const tb_gui_tree_node *node,
                                      tb_canvas_rect box, tb_vec4 color, float thickness)
{
    const float size = box.height * TB_GUI_TREE_MARK;
    const tb_vec2 middle = {box.x + box.height * 0.5f, box.y + box.height * 0.5f};

    if (node->first_child == TB_TREE_ROOT)
    {
        return;
    }
    if (node->open)
    {
        tb_gui_paint_line(paint, (tb_vec2){middle.x - size, middle.y - size * 0.5f},
                          (tb_vec2){middle.x, middle.y + size * 0.5f}, thickness, color);
        tb_gui_paint_line(paint, (tb_vec2){middle.x, middle.y + size * 0.5f},
                          (tb_vec2){middle.x + size, middle.y - size * 0.5f}, thickness, color);
        return;
    }
    tb_gui_paint_line(paint, (tb_vec2){middle.x - size * 0.5f, middle.y - size},
                      (tb_vec2){middle.x + size * 0.5f, middle.y}, thickness, color);
    tb_gui_paint_line(paint, (tb_vec2){middle.x + size * 0.5f, middle.y},
                      (tb_vec2){middle.x - size * 0.5f, middle.y + size}, thickness, color);
}

static void tb_gui_treeview_draw_row(tb_widget widget, const tb_gui_treeview *tree,
                                     tb_gui_paint *rows, size_t row, const tb_style *style)
{
    const tb_tree_item item = tree->rows[row];
    const tb_gui_tree_node *const node = &tree->items[item];
    const tb_canvas_rect rect = tb_gui_tree_row_rect(widget, tree, row);
    const tb_vec4 color = style->text_color;
    const bool focused = (tb_widget_state(widget) & TB_WIDGET_FOCUSED) != 0;
    const float mark = tb_gui_tree_mark_left(widget, tree, row);
    float left = tree->view.x;

    if (node->selected)
    {
        tb_gui_paint_fill(rows, rect, 0.0f, tb_style_color(style, TB_GUI_TREE_SELECTION));
    }
    else if (item == tree->hot)
    {
        tb_gui_paint_fill(rows, rect, 0.0f, tb_style_color(style, TB_GUI_TREE_HOT));
    }
    if (item == tree->focus && focused)
    {
        tb_gui_paint_outline(rows, rect, 0.0f, style->border_width,
                             tb_style_color(style, TB_GUI_TREE_ACTIVE));
    }
    tb_gui_treeview_draw_mark(rows, node, (tb_canvas_rect){mark, rect.y, rect.height, rect.height},
                              color, tb_style_number(style, TB_GUI_TREE_MARK_WIDTH));
    for (size_t column = 0; column < tree->column_count; column++)
    {
        const float width = tree->columns[column].width;
        const float start = column == 0 ? mark + rect.height : left + style->padding.left;
        const tb_text text = column == 0 ? tb_gui_string_text(&node->text)
                                         : (column - 1 < node->value_count
                                                ? tb_gui_string_text(&node->values[column - 1])
                                                : tb_text_make("", 0));
        const tb_gui_text_block block = {.text = text, .anchor = TB_GUI_ANCHOR_W};
        /* Each cell's text is cut off at its column's edge. */
        tb_gui_paint cell = *rows;
        cell.clip =
            tb_ui_rect_intersect(rows->clip, (tb_canvas_rect){left, rect.y, width, rect.height});
        tb_gui_paint_text(
            &cell, &block,
            (tb_canvas_rect){start, rect.y, fmaxf(left + width - start, 0.0f), rect.height}, color);
        left += width;
    }
}

static void tb_gui_treeview_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_treeview *const tree = data;
    const tb_style *const style = tb_style_of(widget);
    const float height = tb_gui_tree_row_height(widget);
    tb_gui_paint rows = *paint;

    rows.clip = tb_ui_rect_intersect(paint->clip, tree->view);
    tb_gui_paint_box(paint, paint->rect, style);
    if (!tree->no_headings)
    {
        tb_gui_treeview_draw_headings(tree, paint, style);
    }
    if (!tb_ui_rect_empty(rows.clip) && height > 0.0f)
    {
        const float top = rows.clip.y - tree->view.y + tree->offset;
        const float first = fmaxf(floorf(top / height), 0.0f);
        const float last = fminf(ceilf((top + rows.clip.height) / height), (float)tree->row_count);
        for (size_t row = (size_t)first; (float)row < last; row++)
        {
            tb_gui_treeview_draw_row(widget, tree, &rows, row, style);
        }
    }
    if (tree->bar)
    {
        const tb_gui_bar bar = tb_gui_tree_bar(widget, tree);
        tb_gui_bar_draw(paint, &bar, tree->dragging_bar, style);
    }
}

/* Open or close an item by the user: its rows change, and the open
 * command is called. */
static void tb_gui_tree_toggle(tb_widget widget, tb_gui_treeview *tree, tb_tree_item item,
                               bool open, const tb_gui_event *cause)
{
    if (tree->items[item].first_child == TB_TREE_ROOT || tree->items[item].open == open)
    {
        return;
    }
    tree->items[item].open = open;
    tree->focus = item;
    tb_gui_tree_changed(widget, tree);
    if (tree->open_command.callback != NULL)
    {
        tb_gui_event event = *cause;
        event.kind = TB_GUI_EVENT_COMMAND;
        event.widget = widget;
        event.target = widget;
        tree->open_command.callback(&event, tree->open_command.user);
    }
}

/* The column whose right edge a point on the headings is at, or SIZE_MAX. */
static size_t tb_gui_tree_edge_at(const tb_gui_treeview *tree, tb_vec2 point)
{
    float right = tree->headings.x;

    if (!tb_ui_rect_contains(tree->headings, point))
    {
        return SIZE_MAX;
    }
    for (size_t column = 0; column < tree->column_count; column++)
    {
        right += tree->columns[column].width;
        if (fabsf(point.x - right) <= TB_GUI_HEADING_GRIP)
        {
            return column;
        }
    }
    return SIZE_MAX;
}

static bool tb_gui_treeview_press(tb_widget widget, tb_gui_treeview *tree,
                                  const tb_gui_event *event)
{
    const tb_gui_bar bar = tb_gui_tree_bar(widget, tree);
    const tb_gui_bar_part part =
        tree->bar ? tb_gui_bar_part_at(&bar, event->position) : TB_GUI_BAR_NONE;

    if (event->button != TB_MOUSE_BUTTON_LEFT)
    {
        return false;
    }
    tree->resizing = tb_gui_tree_edge_at(tree, event->position);
    if (tree->resizing != SIZE_MAX)
    {
        return true;
    }
    if (part == TB_GUI_BAR_THUMB)
    {
        tree->dragging_bar = true;
        tree->grab = tb_gui_bar_grab(&bar, event->position);
        return true;
    }
    if (part != TB_GUI_BAR_NONE)
    {
        tb_gui_tree_scroll(widget, tree, tb_gui_bar_page(&bar, part));
        return true;
    }
    const size_t row = tb_gui_tree_row_at(widget, tree, event->position);
    if (row == SIZE_MAX)
    {
        return true;
    }
    /* A press on an item's mark opens or closes it. */
    const float mark = tb_gui_tree_mark_left(widget, tree, row);
    const tb_tree_item item = tree->rows[row];
    if (event->position.x >= mark && event->position.x < mark + tb_gui_tree_row_height(widget))
    {
        tb_gui_tree_toggle(widget, tree, item, !tree->items[item].open, event);
        return true;
    }
    tree->selecting = true;
    tb_gui_tree_choose(widget, tree, row, event->modifiers, true);
    return true;
}

static void tb_gui_treeview_motion(tb_widget widget, tb_gui_treeview *tree,
                                   const tb_gui_event *event)
{
    const tb_gui_bar bar = tb_gui_tree_bar(widget, tree);

    if (tree->resizing != SIZE_MAX)
    {
        tb_gui_tree_column *const column = &tree->columns[tree->resizing];
        column->width = fmaxf(event->position.x - tb_gui_tree_column_left(tree, tree->resizing),
                              column->minwidth);
        tb_gui_mark_layout(widget.gui);
        return;
    }
    if (tree->dragging_bar)
    {
        tb_gui_tree_scroll(widget, tree, tb_gui_bar_drag(&bar, event->position, tree->grab));
        return;
    }
    const size_t row = tb_gui_tree_row_at(widget, tree, event->position);
    tree->hot = row != SIZE_MAX ? tree->rows[row] : TB_TREE_ROOT;
    /* Over a heading's edge, the cursor says it is dragged. */
    tb_widget_set_cursor(widget, tb_gui_tree_edge_at(tree, event->position) != SIZE_MAX
                                     ? TB_CURSOR_RESIZE_HORIZONTAL
                                     : TB_CURSOR_COUNT);
    if (tree->selecting && row != SIZE_MAX && tree->rows[row] != tree->focus &&
        tree->selectmode == TB_LISTBOX_BROWSE)
    {
        tb_gui_tree_choose(widget, tree, row, 0, false);
    }
}

/* The row a key moves the focus to, or SIZE_MAX for a key that does not. */
static size_t tb_gui_tree_key_row(tb_widget widget, const tb_gui_treeview *tree, size_t row,
                                  tb_key key)
{
    const float height = tb_gui_tree_row_height(widget);
    const size_t page =
        height > 0.0f ? (size_t)fmaxf(floorf(tree->view.height / height), 1.0f) : 1u;
    const size_t last = tree->row_count - 1;

    if (key == TB_KEY_UP)
    {
        return row > 0 && row != SIZE_MAX ? row - 1 : 0;
    }
    if (key == TB_KEY_DOWN)
    {
        return row == SIZE_MAX ? 0 : (row < last ? row + 1 : last);
    }
    if (key == TB_KEY_PAGE_UP)
    {
        return row != SIZE_MAX && row > page ? row - page : 0;
    }
    if (key == TB_KEY_PAGE_DOWN)
    {
        return row != SIZE_MAX && last - row > page ? row + page : last;
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

/* Right opens an item, or goes into it; Left closes it, or goes to its
 * parent. Returns the row to go to, or SIZE_MAX. */
static size_t tb_gui_tree_branch_key(tb_widget widget, tb_gui_treeview *tree, size_t row,
                                     const tb_gui_event *event)
{
    const tb_tree_item item = tree->rows[row];
    const tb_gui_tree_node *const node = &tree->items[item];

    if (event->key == TB_KEY_RIGHT)
    {
        if (node->first_child != TB_TREE_ROOT && !node->open)
        {
            tb_gui_tree_toggle(widget, tree, item, true, event);
            return SIZE_MAX;
        }
        return node->first_child != TB_TREE_ROOT ? row + 1 : SIZE_MAX;
    }
    if (node->first_child != TB_TREE_ROOT && node->open)
    {
        tb_gui_tree_toggle(widget, tree, item, false, event);
        return SIZE_MAX;
    }
    return node->parent != TB_TREE_ROOT ? tb_gui_tree_row_of(tree, node->parent) : SIZE_MAX;
}

static bool tb_gui_treeview_key(tb_widget widget, tb_gui_treeview *tree, const tb_gui_event *event)
{
    const bool control = (event->modifiers & TB_MODIFIER_CONTROL) != 0;
    const size_t row = tb_gui_tree_row_of(tree, tree->focus);
    size_t to = SIZE_MAX;

    if (tree->row_count == 0)
    {
        return false;
    }
    if ((event->key == TB_KEY_SPACE || event->key == TB_KEY_ENTER) && row != SIZE_MAX)
    {
        tb_gui_tree_choose(widget, tree, row, event->modifiers | TB_MODIFIER_CONTROL, true);
        return true;
    }
    if ((event->key == TB_KEY_LEFT || event->key == TB_KEY_RIGHT) && row != SIZE_MAX)
    {
        to = tb_gui_tree_branch_key(widget, tree, row, event);
        tb_gui_tree_rows(tree);
        if (to == SIZE_MAX)
        {
            return true;
        }
    }
    else
    {
        to = tb_gui_tree_key_row(widget, tree, row, event->key);
    }
    if (to == SIZE_MAX)
    {
        return false;
    }
    if (tree->selectmode == TB_LISTBOX_MULTIPLE ||
        (tree->selectmode == TB_LISTBOX_EXTENDED && control))
    {
        tree->focus = tree->rows[to];
    }
    else
    {
        tb_gui_tree_choose(widget, tree, to, event->modifiers, false);
    }
    (void)tb_treeview_see(widget, tree->rows[to]);
    return true;
}

static bool tb_gui_treeview_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_treeview *const tree = data;
    const float height = tb_gui_tree_row_height(widget);

    tb_gui_tree_rows(tree);
    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS:
        return tb_gui_treeview_press(widget, tree, event);
    case TB_GUI_EVENT_DOUBLE_CLICK: {
        /* A double click on an item opens or closes it, as Tk's does. */
        const size_t row = tb_gui_tree_row_at(widget, tree, event->position);
        if (event->button == TB_MOUSE_BUTTON_LEFT && row != SIZE_MAX)
        {
            const tb_tree_item item = tree->rows[row];
            tb_gui_tree_toggle(widget, tree, item, !tree->items[item].open, event);
        }
        return true;
    }
    case TB_GUI_EVENT_MOTION:
        tb_gui_treeview_motion(widget, tree, event);
        return true;
    case TB_GUI_EVENT_RELEASE:
        tree->resizing = SIZE_MAX;
        tree->dragging_bar = false;
        tree->selecting = false;
        return true;
    case TB_GUI_EVENT_LEAVE:
        tree->hot = TB_TREE_ROOT;
        return false;
    case TB_GUI_EVENT_WHEEL: {
        const float before = tree->offset;
        tb_gui_tree_scroll(widget, tree,
                           tree->offset - event->wheel.y * height * TB_GUI_TREE_WHEEL_ROWS);
        return tree->offset != before;
    }
    case TB_GUI_EVENT_KEY:
        return tb_gui_treeview_key(widget, tree, event);
    case TB_GUI_EVENT_COMMAND:
        if (tree->command.callback != NULL)
        {
            tree->command.callback(event, tree->command.user);
        }
        return true;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_CLICK:
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

static bool tb_gui_treeview_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_treeview *const tree = data;

    (void)widget;
    if (value->integer < 0)
    {
        return false;
    }
    tree->height = value->integer;
    return true;
}

static bool tb_gui_treeview_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_treeview *const tree = data;

    (void)widget;
    *value = tb_gui_int(tree->height);
    return true;
}

static bool tb_gui_treeview_set_heading(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_treeview *const tree = data;

    (void)widget;
    tb_gui_string_set(&tree->columns[0].heading, value->text);
    return true;
}

static bool tb_gui_treeview_get_heading(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_treeview *const tree = data;

    (void)widget;
    *value = tb_gui_text(tb_gui_string_text(&tree->columns[0].heading));
    return true;
}

static bool tb_gui_treeview_set_offset(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_treeview *const tree = data;

    if (value->number < 0.0f)
    {
        return false;
    }
    tb_gui_tree_scroll(widget, tree, value->number);
    return true;
}

static bool tb_gui_treeview_get_offset(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_treeview *const tree = data;

    (void)widget;
    *value = tb_gui_float(tree->offset);
    return true;
}

static const tb_gui_property tb_gui_treeview_properties[] = {
    {.name = "selectmode",
     .type = TB_GUI_TYPE_ENUM,
     .offset = offsetof(tb_gui_treeview, selectmode),
     .names = tb_gui_tree_selectmode_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_tree_selectmode_names)},
    {.name = "height",
     .type = TB_GUI_TYPE_INT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_treeview_get_height,
     .set = tb_gui_treeview_set_height},
    {.name = "no_headings",
     .type = TB_GUI_TYPE_BOOL,
     .offset = offsetof(tb_gui_treeview, no_headings),
     .flags = TB_GUI_PROPERTY_LAYOUT},
    {.name = "heading",
     .type = TB_GUI_TYPE_TEXT,
     .get = tb_gui_treeview_get_heading,
     .set = tb_gui_treeview_set_heading},
    {.name = "offset",
     .type = TB_GUI_TYPE_FLOAT,
     .get = tb_gui_treeview_get_offset,
     .set = tb_gui_treeview_set_offset},
    {.name = "command", .type = TB_GUI_TYPE_COMMAND, .offset = offsetof(tb_gui_treeview, command)},
    {.name = "open_command",
     .type = TB_GUI_TYPE_COMMAND,
     .offset = offsetof(tb_gui_treeview, open_command)},
};

/* What a layout file cannot hold yet (tb_widget_class::unsaved). */
static const char *tb_gui_treeview_unsaved(tb_widget widget, const void *data)
{
    const tb_gui_treeview *const tree = data;

    (void)widget;
    for (size_t item = TB_TREE_ROOT + 1; item < tree->count; item++)
    {
        if (tree->items[item].alive)
        {
            return "tree items";
        }
    }
    return tree->column_count > 1 ? "tree columns" : NULL;
}

static const tb_widget_class tb_gui_treeview_kind = {
    .name = "treeview",
    .data_size = sizeof(tb_gui_treeview),
    .properties = tb_gui_treeview_properties,
    .property_count = TB_GUI_COUNT_OF(tb_gui_treeview_properties),
    .flags = TB_WIDGET_CLASS_FOCUSABLE | TB_WIDGET_CLASS_FOCUS_ON_CLICK,
    .mouse_filter = TB_GUI_MOUSE_STOP,
    .init = tb_gui_treeview_init,
    .destroy = tb_gui_treeview_destroy,
    .measure = tb_gui_treeview_measure,
    .arrange = tb_gui_treeview_arrange,
    .draw = tb_gui_treeview_draw,
    .event = tb_gui_treeview_event,
    .style = &tb_gui_treeview_style,
    .unsaved = tb_gui_treeview_unsaved};

static tb_gui_treeview *tb_gui_treeview_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_treeview_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_treeview_class(void)
{
    return &tb_gui_treeview_kind;
}

tb_widget tb_treeview(tb_widget parent, const tb_treeview_desc *desc)
{
    const tb_treeview_desc chosen = desc != NULL ? *desc : (tb_treeview_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_treeview_kind);
    const tb_gui_value values[] = {
        tb_gui_enum((int32_t)chosen.selectmode),
        tb_gui_int(chosen.height),
        tb_gui_bool(chosen.no_headings),
        tb_gui_text(tb_text_from_cstr(chosen.heading != NULL ? chosen.heading : "")),
        tb_gui_command_of(chosen.command, chosen.user),
        tb_gui_command_of(chosen.open_command, chosen.user)};
    static const char *const names[] = {"selectmode", "height",  "no_headings",
                                        "heading",    "command", "open_command"};

    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    if (chosen.tree_width > 0.0f)
    {
        (void)tb_treeview_set_column_width(widget, TB_TREEVIEW_TREE_COLUMN, chosen.tree_width);
    }
    return widget;
}

void tb_gui_register_treeview(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_treeview_kind);
}
