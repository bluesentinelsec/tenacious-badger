/* Layout: the view of units on the frame, and the grid, after Tk's. Each
 * layout measures the tree from its leaves up, every widget's natural size
 * from its content and its children's grid, then places it from the root
 * down, each grid's rows and columns sized from what their widgets want and
 * the space there is, their edges on whole pixels. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* Less than this of space is none: what float sums leave over. */
#define TB_GUI_EPSILON 1e-4f

/* ---------------------------------------------------------------------------
 * The view.
 * ------------------------------------------------------------------------- */

tb_gui_view tb_gui_compute_view(const tb_gui *gui)
{
    const float width = (float)gui->frame_width;
    const float height = (float)gui->frame_height;
    const tb_vec2 reference = gui->reference_size;
    const bool referenced = reference.x > 0.0f && reference.y > 0.0f;
    tb_gui_view view = {0};
    float fit = 1.0f;

    if (!referenced || gui->stretch == TB_GUI_STRETCH_NONE)
    {
        view.scale = gui->display_scale * gui->zoom;
        view.size = (tb_vec2){width / view.scale, height / view.scale};
        return view;
    }
    if (width > 0.0f && height > 0.0f)
    {
        fit = fminf(width / reference.x, height / reference.y);
    }
    switch (gui->stretch)
    {
    case TB_GUI_STRETCH_EXPAND:
        view.scale = fit * gui->zoom;
        view.size = (tb_vec2){width / view.scale, height / view.scale};
        return view;
    case TB_GUI_STRETCH_INTEGER:
        view.scale = fmaxf(floorf(fit * gui->zoom), 1.0f);
        break;
    case TB_GUI_STRETCH_KEEP_ASPECT:
    case TB_GUI_STRETCH_NONE:
        view.scale = fit * gui->zoom;
        break;
    }
    view.size = (tb_vec2){reference.x / gui->zoom, reference.y / gui->zoom};
    view.origin = (tb_vec2){floorf((width - view.size.x * view.scale) * 0.5f + 0.5f),
                            floorf((height - view.size.y * view.scale) * 0.5f + 0.5f)};
    return view;
}

/* ---------------------------------------------------------------------------
 * Axes of a grid.
 * ------------------------------------------------------------------------- */

void tb_gui_axis_free(tb_gui_axis *axis)
{
    for (size_t index = 0; index < axis->config_count; index++)
    {
        tb_free(axis->configs[index].uniform);
    }
    tb_free(axis->configs);
    tb_free(axis->sizes);
    tb_free(axis->occupied);
    tb_free(axis->edges);
    *axis = (tb_gui_axis){0};
}

/* An axis of `count` tracks, each of no size and empty. */
static void tb_gui_axis_reset(tb_gui_axis *axis, size_t count)
{
    /* A grid of no tracks still has its first edge. */
    if (count > axis->capacity || axis->edges == NULL)
    {
        axis->capacity = count > 0 ? count : 1u;
        axis->sizes = tb_realloc_array(axis->sizes, axis->capacity, sizeof(*axis->sizes));
        axis->occupied = tb_realloc_array(axis->occupied, axis->capacity, sizeof(*axis->occupied));
        axis->edges = tb_realloc_array(axis->edges, axis->capacity + 1, sizeof(*axis->edges));
    }
    axis->count = count;
    for (size_t index = 0; index < count; index++)
    {
        axis->sizes[index] = 0.0f;
        axis->occupied[index] = false;
    }
}

static tb_gui_track_config tb_gui_config_of(const tb_gui_axis *axis, size_t index)
{
    return index < axis->config_count ? axis->configs[index] : (tb_gui_track_config){0};
}

/* A uniform group's member's share: its weight, 0 counted as 1. */
static float tb_gui_uniform_weight(float weight)
{
    return weight > 0.0f ? weight : 1.0f;
}

static bool tb_gui_same_group(const char *a, const char *b)
{
    return a != NULL && b != NULL && tb_str_equal(a, b);
}

/* Size the tracks of each uniform group in proportion to their weights, as
 * large as the largest needs. */
static void tb_gui_apply_uniform(tb_gui_axis *axis)
{
    for (size_t index = 0; index < axis->count; index++)
    {
        const char *const group = tb_gui_config_of(axis, index).uniform;
        float unit = 0.0f;
        if (group == NULL)
        {
            continue;
        }
        for (size_t member = 0; member < axis->count; member++)
        {
            const tb_gui_track_config config = tb_gui_config_of(axis, member);
            if (tb_gui_same_group(config.uniform, group))
            {
                unit = fmaxf(unit, axis->sizes[member] / tb_gui_uniform_weight(config.weight));
            }
        }
        for (size_t member = 0; member < axis->count; member++)
        {
            const tb_gui_track_config config = tb_gui_config_of(axis, member);
            if (tb_gui_same_group(config.uniform, group))
            {
                axis->sizes[member] = unit * tb_gui_uniform_weight(config.weight);
            }
        }
    }
}

/* Give tracks space by their weights; evenly when none has weight. */
static void tb_gui_widen(tb_gui_axis *axis, size_t first, size_t span, float space)
{
    float weights = 0.0f;

    for (size_t index = first; index < first + span; index++)
    {
        weights += tb_gui_config_of(axis, index).weight;
    }
    for (size_t index = first; index < first + span; index++)
    {
        const float weight = tb_gui_config_of(axis, index).weight;
        axis->sizes[index] += weights > 0.0f ? space * weight / weights : space / (float)span;
    }
}

/* Take space back from the tracks of weight, by their weights, down to
 * their minimum sizes. */
static void tb_gui_shrink(tb_gui_axis *axis, float deficit)
{
    for (size_t round = 0; round < axis->count && deficit > TB_GUI_EPSILON; round++)
    {
        float weights = 0.0f;
        float taken = 0.0f;
        for (size_t index = 0; index < axis->count; index++)
        {
            const tb_gui_track_config config = tb_gui_config_of(axis, index);
            if (config.weight > 0.0f && axis->sizes[index] > config.minsize + TB_GUI_EPSILON)
            {
                weights += config.weight;
            }
        }
        if (!(weights > 0.0f))
        {
            return;
        }
        for (size_t index = 0; index < axis->count; index++)
        {
            const tb_gui_track_config config = tb_gui_config_of(axis, index);
            if (config.weight > 0.0f && axis->sizes[index] > config.minsize + TB_GUI_EPSILON)
            {
                const float cut =
                    fminf(deficit * config.weight / weights, axis->sizes[index] - config.minsize);
                axis->sizes[index] -= cut;
                taken += cut;
            }
        }
        deficit -= taken;
    }
}

/* ---------------------------------------------------------------------------
 * A child's place in its parent's grid.
 * ------------------------------------------------------------------------- */

static bool tb_gui_in_grid(const tb_gui_node *node)
{
    return node->visible && node->gridded && !node->popup;
}

static size_t tb_gui_start(const tb_gui_node *node, int axis)
{
    return (size_t)(axis == TB_GUI_COLUMNS ? node->grid.column : node->grid.row);
}

static size_t tb_gui_span(const tb_gui_node *node, int axis)
{
    const int32_t span = axis == TB_GUI_COLUMNS ? node->grid.column_span : node->grid.row_span;
    return span > 0 ? (size_t)span : 1u;
}

/* The space a child wants along an axis: its natural size, its inner
 * padding twice, and its outer padding. */
static float tb_gui_request(const tb_gui_node *node, int axis)
{
    if (axis == TB_GUI_COLUMNS)
    {
        return node->natural.x + 2.0f * node->grid.ipadx + node->grid.padx.x + node->grid.padx.y;
    }
    return node->natural.y + 2.0f * node->grid.ipady + node->grid.pady.x + node->grid.pady.y;
}

/* ---------------------------------------------------------------------------
 * Measuring.
 * ------------------------------------------------------------------------- */

/* Size a grid's tracks along an axis from its children, already measured. */
static float tb_gui_measure_axis(tb_gui *gui, uint32_t index, int axis_index)
{
    tb_gui_axis *const axis = &tb_gui_node_at(gui, index)->axes[axis_index];
    size_t count = axis->config_count;
    size_t widest_span = 1;
    float total = 0.0f;

    for (uint32_t child = tb_gui_node_at(gui, index)->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->next)
    {
        const tb_gui_node *const node = tb_gui_node_at(gui, child);
        if (tb_gui_in_grid(node))
        {
            const size_t span = tb_gui_span(node, axis_index);
            const size_t end = tb_gui_start(node, axis_index) + span;
            count = end > count ? end : count;
            widest_span = span > widest_span ? span : widest_span;
        }
    }
    tb_gui_axis_reset(axis, count);
    for (uint32_t child = tb_gui_node_at(gui, index)->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->next)
    {
        const tb_gui_node *const node = tb_gui_node_at(gui, child);
        if (tb_gui_in_grid(node) && tb_gui_span(node, axis_index) == 1)
        {
            const size_t track = tb_gui_start(node, axis_index);
            axis->sizes[track] = fmaxf(axis->sizes[track], tb_gui_request(node, axis_index));
            axis->occupied[track] = true;
        }
    }
    for (size_t track = 0; track < count; track++)
    {
        const tb_gui_track_config config = tb_gui_config_of(axis, track);
        if (axis->occupied[track])
        {
            axis->sizes[track] += config.pad;
        }
        axis->sizes[track] = fmaxf(axis->sizes[track], config.minsize);
    }
    /* Widgets that span: the narrower first, as Tk does. */
    for (size_t span = 2; span <= widest_span; span++)
    {
        for (uint32_t child = tb_gui_node_at(gui, index)->first_child; child != TB_GUI_NO_NODE;
             child = tb_gui_node_at(gui, child)->next)
        {
            const tb_gui_node *const node = tb_gui_node_at(gui, child);
            const size_t first = tb_gui_start(node, axis_index);
            float have = 0.0f;
            if (!tb_gui_in_grid(node) || tb_gui_span(node, axis_index) != span)
            {
                continue;
            }
            for (size_t track = first; track < first + span; track++)
            {
                have += axis->sizes[track];
            }
            const float need = tb_gui_request(node, axis_index);
            if (need > have)
            {
                tb_gui_widen(axis, first, span, need - have);
            }
        }
    }
    tb_gui_apply_uniform(axis);
    for (size_t track = 0; track < count; track++)
    {
        total += axis->sizes[track];
    }
    return total;
}

tb_vec2 tb_gui_measure(tb_gui *gui, uint32_t index)
{
    const tb_widget widget = tb_gui_handle(gui, index);
    tb_vec2 content = {0};
    tb_gui_edges insets = {0};

    for (uint32_t child = tb_gui_node_at(gui, index)->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->next)
    {
        if (tb_gui_in_grid(tb_gui_node_at(gui, child)))
        {
            (void)tb_gui_measure(gui, child);
        }
    }
    /* The grid first: a kind that scrolls measures itself from it. */
    const tb_vec2 grid = {tb_gui_measure_axis(gui, index, TB_GUI_COLUMNS),
                          tb_gui_measure_axis(gui, index, TB_GUI_ROWS)};
    tb_gui_node_at(gui, index)->grid_request = grid;
    const tb_widget_class *const kind = tb_gui_node_at(gui, index)->kind;
    if (kind->measure != NULL)
    {
        content = kind->measure(widget, tb_gui_node_at(gui, index)->data);
    }
    if (kind->insets != NULL)
    {
        insets = kind->insets(widget, tb_gui_node_at(gui, index)->data);
    }
    tb_gui_node *const node = tb_gui_node_at(gui, index);
    const tb_vec2 wanted = kind->viewport != NULL ? content : tb_vec2_max(content, grid);
    node->natural =
        (tb_vec2){wanted.x + insets.left + insets.right, wanted.y + insets.top + insets.bottom};
    return node->natural;
}

/* ---------------------------------------------------------------------------
 * Arranging.
 * ------------------------------------------------------------------------- */

float tb_gui_anchor_fraction(int32_t anchor, int axis)
{
    const bool columns = axis == TB_GUI_COLUMNS;

    switch ((tb_gui_anchor)anchor)
    {
    case TB_GUI_ANCHOR_N:
        return columns ? 0.5f : 0.0f;
    case TB_GUI_ANCHOR_NE:
        return columns ? 1.0f : 0.0f;
    case TB_GUI_ANCHOR_E:
        return columns ? 1.0f : 0.5f;
    case TB_GUI_ANCHOR_SE:
        return 1.0f;
    case TB_GUI_ANCHOR_S:
        return columns ? 0.5f : 1.0f;
    case TB_GUI_ANCHOR_SW:
        return columns ? 0.0f : 1.0f;
    case TB_GUI_ANCHOR_W:
        return columns ? 0.0f : 0.5f;
    case TB_GUI_ANCHOR_NW:
        return 0.0f;
    case TB_GUI_ANCHOR_CENTER:
        break;
    }
    return 0.5f;
}

/* Size an axis's tracks to the space there is, and place their edges on
 * whole pixels from `origin`. */
static void tb_gui_arrange_axis(tb_gui *gui, tb_gui_axis *axis, int32_t anchor, int axis_index,
                                float origin, float available)
{
    float total = 0.0f;
    float weights = 0.0f;
    float offset = 0.0f;
    float edge = 0.0f;

    for (size_t track = 0; track < axis->count; track++)
    {
        total += axis->sizes[track];
        weights += tb_gui_config_of(axis, track).weight;
    }
    const float extra = available - total;
    if (extra > TB_GUI_EPSILON && weights > 0.0f)
    {
        for (size_t track = 0; track < axis->count; track++)
        {
            axis->sizes[track] += extra * tb_gui_config_of(axis, track).weight / weights;
        }
    }
    else if (extra > TB_GUI_EPSILON)
    {
        offset = extra * tb_gui_anchor_fraction(anchor, axis_index);
    }
    else if (extra < -TB_GUI_EPSILON)
    {
        tb_gui_shrink(axis, -extra);
    }
    /* Edges are summed unrounded and each is rounded alone, so that the
     * error never grows along a row, and neighbors share an edge. */
    edge = origin + offset;
    axis->edges[0] = tb_ui_snap(gui->view.scale, edge);
    for (size_t track = 0; track < axis->count; track++)
    {
        edge += axis->sizes[track];
        axis->edges[track + 1] = tb_ui_snap(gui->view.scale, edge);
    }
}

/* A widget's span within its cell along an axis: where it starts and how
 * long it is. */
static void tb_gui_place_span(const tb_gui_node *node, const tb_gui_axis *axis, int axis_index,
                              float *start, float *length)
{
    const size_t first = tb_gui_start(node, axis_index);
    const size_t last = first + tb_gui_span(node, axis_index) - 1;
    const bool columns = axis_index == TB_GUI_COLUMNS;
    const tb_vec2 pad = columns ? node->grid.padx : node->grid.pady;
    const float ipad = columns ? node->grid.ipadx : node->grid.ipady;
    const float natural = columns ? node->natural.x : node->natural.y;
    const uint32_t low = columns ? TB_STICKY_W : TB_STICKY_N;
    const uint32_t high = columns ? TB_STICKY_E : TB_STICKY_S;
    const float before = axis->edges[first] + tb_gui_config_of(axis, first).pad * 0.5f + pad.x;
    const float after = axis->edges[last + 1] - tb_gui_config_of(axis, last).pad * 0.5f - pad.y;
    const float room = fmaxf(after - before, 0.0f);
    const float wanted = fminf(natural + 2.0f * ipad, room);
    const bool to_low = (node->grid.sticky & low) != 0;
    const bool to_high = (node->grid.sticky & high) != 0;

    if (to_low && to_high)
    {
        *start = before;
        *length = room;
        return;
    }
    *length = wanted;
    if (to_low)
    {
        *start = before;
    }
    else if (to_high)
    {
        *start = before + room - wanted;
    }
    else
    {
        *start = before + (room - wanted) * 0.5f;
    }
}

static void tb_gui_note_resize(tb_gui *gui, uint32_t index)
{
    if (gui->resized_count == gui->resized_capacity)
    {
        gui->resized_capacity = gui->resized_capacity == 0 ? 16u : gui->resized_capacity * 2;
        gui->resized = tb_realloc_array(gui->resized, gui->resized_capacity, sizeof(*gui->resized));
    }
    gui->resized[gui->resized_count] = index;
    gui->resized_count++;
}

/* A scroll offset kept from 0 to as far as the content goes past the clip.
 * The grid's edges are put on whole pixels from it, as from any origin. */
static tb_vec2 tb_gui_keep_scroll(tb_vec2 scroll, tb_gui_viewport viewport)
{
    const float most_x = fmaxf(viewport.content.x - viewport.clip.width, 0.0f);
    const float most_y = fmaxf(viewport.content.y - viewport.clip.height, 0.0f);

    return (tb_vec2){fminf(fmaxf(scroll.x, 0.0f), most_x), fminf(fmaxf(scroll.y, 0.0f), most_y)};
}

void tb_gui_arrange(tb_gui *gui, uint32_t index, tb_canvas_rect rect)
{
    const tb_widget widget = tb_gui_handle(gui, index);
    tb_gui_node *node = tb_gui_node_at(gui, index);
    const tb_widget_class *const kind = node->kind;
    tb_gui_edges insets = {0};

    if (rect.width != node->rect.width || rect.height != node->rect.height)
    {
        tb_gui_note_resize(gui, index);
    }
    node->rect = rect;
    node->shown = true;
    if (kind->arrange != NULL)
    {
        kind->arrange(widget, node->data, rect);
    }
    if (kind->insets != NULL)
    {
        insets = kind->insets(widget, tb_gui_node_at(gui, index)->data);
    }
    const tb_canvas_rect inner = {rect.x + insets.left, rect.y + insets.top,
                                  fmaxf(rect.width - insets.left - insets.right, 0.0f),
                                  fmaxf(rect.height - insets.top - insets.bottom, 0.0f)};
    tb_gui_viewport viewport = {.clip = inner, .content = {inner.width, inner.height}};
    if (kind->viewport != NULL)
    {
        viewport = kind->viewport(widget, tb_gui_node_at(gui, index)->data, inner);
    }
    node = tb_gui_node_at(gui, index);
    node->clip = kind->viewport != NULL ? viewport.clip : rect;
    node->content = viewport.content;
    node->scroll = tb_gui_keep_scroll(node->scroll, viewport);
    tb_gui_arrange_axis(gui, &node->axes[TB_GUI_COLUMNS], node->grid_anchor, TB_GUI_COLUMNS,
                        viewport.clip.x - node->scroll.x, viewport.content.x);
    tb_gui_arrange_axis(gui, &node->axes[TB_GUI_ROWS], node->grid_anchor, TB_GUI_ROWS,
                        viewport.clip.y - node->scroll.y, viewport.content.y);
    for (uint32_t child = node->first_child; child != TB_GUI_NO_NODE;
         child = tb_gui_node_at(gui, child)->next)
    {
        const tb_gui_node *const owner = tb_gui_node_at(gui, index);
        const tb_gui_node *const placed = tb_gui_node_at(gui, child);
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        if (!tb_gui_in_grid(placed))
        {
            continue;
        }
        tb_gui_place_span(placed, &owner->axes[TB_GUI_COLUMNS], TB_GUI_COLUMNS, &x, &width);
        tb_gui_place_span(placed, &owner->axes[TB_GUI_ROWS], TB_GUI_ROWS, &y, &height);
        tb_gui_arrange(gui, child,
                       tb_ui_snap_rect(gui->view.scale, (tb_canvas_rect){x, y, width, height}));
    }
}

void tb_gui_layout(tb_gui *gui)
{
    if (!gui->layout_dirty)
    {
        return;
    }
    gui->layout_dirty = false;
    gui->layouts++;
    for (size_t index = 0; index < gui->node_count; index++)
    {
        gui->nodes[index].shown = false;
    }
    (void)tb_gui_measure(gui, 0);
    tb_gui_arrange(gui, 0, (tb_canvas_rect){0.0f, 0.0f, gui->view.size.x, gui->view.size.y});
    tb_gui_layout_popups(gui);
}

/* ---------------------------------------------------------------------------
 * The grid's functions.
 * ------------------------------------------------------------------------- */

static bool tb_gui_pad_fits(tb_vec2 pad)
{
    return isfinite(pad.x) && isfinite(pad.y) && pad.x >= 0.0f && pad.y >= 0.0f;
}

bool tb_grid(tb_widget widget, const tb_grid_options *options)
{
    tb_gui_node *const node = tb_gui_node_of(widget);
    tb_grid_options chosen = options != NULL ? *options : (tb_grid_options){0};

    if (node == NULL || node->parent == TB_GUI_NO_NODE)
    {
        return false;
    }
    chosen.row_span = chosen.row_span > 0 ? chosen.row_span : 1;
    chosen.column_span = chosen.column_span > 0 ? chosen.column_span : 1;
    if (chosen.row < 0 || chosen.column < 0 || chosen.row > INT32_MAX - chosen.row_span ||
        chosen.column > INT32_MAX - chosen.column_span || !tb_gui_pad_fits(chosen.padx) ||
        !tb_gui_pad_fits(chosen.pady) || !tb_gui_pad_fits((tb_vec2){chosen.ipadx, chosen.ipady}) ||
        (chosen.sticky & ~(uint32_t)TB_STICKY_NSEW) != 0)
    {
        return false;
    }
    node->grid = chosen;
    node->gridded = true;
    tb_gui_mark_layout(widget.gui);
    return true;
}

void tb_grid_forget(tb_widget widget)
{
    tb_gui_node *const node = tb_gui_node_of(widget);

    if (node == NULL)
    {
        return;
    }
    node->gridded = false;
    node->grid = (tb_grid_options){0};
    tb_gui_mark_layout(widget.gui);
}

bool tb_grid_info(tb_widget widget, tb_grid_options *options)
{
    const tb_gui_node *const node = tb_gui_node_of(widget);

    TB_ASSERT(options != NULL);
    if (node == NULL || !node->gridded)
    {
        return false;
    }
    *options = node->grid;
    return true;
}

static bool tb_gui_configure(tb_widget container, int axis_index, size_t index,
                             const tb_grid_track *track)
{
    tb_gui_node *const node = tb_gui_node_of(container);

    TB_ASSERT(track != NULL);
    if (node == NULL || !isfinite(track->weight) || !isfinite(track->minsize) ||
        !isfinite(track->pad) || track->weight < 0.0f || track->minsize < 0.0f ||
        track->pad < 0.0f || index >= (size_t)INT32_MAX)
    {
        return false;
    }
    tb_gui_axis *const axis = &node->axes[axis_index];
    if (index >= axis->config_count)
    {
        axis->configs = tb_realloc_array(axis->configs, index + 1, sizeof(*axis->configs));
        for (size_t added = axis->config_count; added <= index; added++)
        {
            axis->configs[added] = (tb_gui_track_config){0};
        }
        axis->config_count = index + 1;
    }
    tb_gui_track_config *const config = &axis->configs[index];
    tb_free(config->uniform);
    *config = (tb_gui_track_config){
        .weight = track->weight, .minsize = track->minsize, .pad = track->pad};
    if (track->uniform != NULL && track->uniform[0] != '\0')
    {
        const size_t length = tb_str_length(track->uniform);
        config->uniform = tb_malloc(length + 1);
        memcpy(config->uniform, track->uniform, length + 1);
    }
    tb_gui_mark_layout(container.gui);
    return true;
}

bool tb_grid_row_configure(tb_widget container, size_t row, const tb_grid_track *track)
{
    return tb_gui_configure(container, TB_GUI_ROWS, row, track);
}

bool tb_grid_column_configure(tb_widget container, size_t column, const tb_grid_track *track)
{
    return tb_gui_configure(container, TB_GUI_COLUMNS, column, track);
}

void tb_grid_anchor(tb_widget container, tb_gui_anchor anchor)
{
    const tb_gui_value value = tb_gui_enum((int32_t)anchor);
    (void)tb_widget_set(container, "grid_anchor", &value);
}

void tb_grid_size(tb_widget container, size_t *columns, size_t *rows)
{
    const tb_gui_node *const node = tb_gui_node_of(container);

    TB_ASSERT(columns != NULL);
    TB_ASSERT(rows != NULL);
    *columns = node != NULL ? node->axes[TB_GUI_COLUMNS].count : 0;
    *rows = node != NULL ? node->axes[TB_GUI_ROWS].count : 0;
}

tb_vec2 tb_grid_request(tb_widget container)
{
    const tb_gui_node *const node = tb_gui_node_of(container);
    return node != NULL ? node->grid_request : (tb_vec2){0};
}

bool tb_grid_bbox(tb_widget container, size_t column, size_t row, size_t column2, size_t row2,
                  tb_canvas_rect *rect)
{
    const tb_gui_node *const node = tb_gui_node_of(container);

    TB_ASSERT(rect != NULL);
    if (node == NULL || !node->shown)
    {
        return false;
    }
    const tb_gui_axis *const columns = &node->axes[TB_GUI_COLUMNS];
    const tb_gui_axis *const rows = &node->axes[TB_GUI_ROWS];
    const size_t left = column < column2 ? column : column2;
    const size_t right = column < column2 ? column2 : column;
    const size_t top = row < row2 ? row : row2;
    const size_t bottom = row < row2 ? row2 : row;
    if (right >= columns->count || bottom >= rows->count)
    {
        return false;
    }
    *rect = (tb_canvas_rect){columns->edges[left], rows->edges[top],
                             columns->edges[right + 1] - columns->edges[left],
                             rows->edges[bottom + 1] - rows->edges[top]};
    return true;
}
