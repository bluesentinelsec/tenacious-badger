/* The panedwindow kind, after Tk's `ttk.Panedwindow`: panes side by side,
 * or one above another, with sashes between them dragged to share the
 * space out. It lays its panes out with its own grid, as every widget
 * does: each pane in a row or column of one uniform group, weighted by its
 * share, so that the grid keeps their sizes in proportion; each sash a
 * fixed row or column between them. Dragging a sash sets the shares. */
#include "gui_internal.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <stddef.h>

#define TB_GUI_COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

/* Panes a panedwindow first holds. */
#define TB_GUI_PANED_FIRST 4u
/* The uniform group of the panes' rows or columns. */
#define TB_GUI_PANES_GROUP "panes"

static const char *const tb_gui_paned_orient_names[] = {"horizontal", "vertical"};

typedef struct tb_gui_pane
{
    tb_widget widget;
    float minsize;
} tb_gui_pane;

typedef struct tb_gui_paned
{
    tb_gui_pane *panes;
    size_t count;
    size_t capacity;
    int32_t orient;
    float width;
    float height;
    size_t dragging; /* the sash being dragged, or SIZE_MAX */
    float grab;
} tb_gui_paned;

static tb_gui_paned *tb_gui_paned_of(tb_widget widget);

static void tb_gui_paned_init(tb_widget widget, void *data)
{
    tb_gui_paned *const paned = data;

    (void)widget;
    paned->dragging = SIZE_MAX;
}

static void tb_gui_paned_destroy(tb_widget widget, void *data)
{
    tb_gui_paned *const paned = data;

    (void)widget;
    tb_free(paned->panes);
}

static bool tb_gui_paned_across(const tb_gui_paned *paned)
{
    return paned->orient == TB_GUI_ORIENT_HORIZONTAL;
}

static float tb_gui_sash_width(tb_gui *gui)
{
    return tb_gui_theme(gui)->item_spacing.x;
}

/* A pane's or sash's rectangle, from the grid's last layout: track
 * `index` along the panes' axis. */
static tb_canvas_rect tb_gui_paned_track(tb_widget widget, const tb_gui_paned *paned, size_t index)
{
    tb_canvas_rect rect = {0};

    if (tb_gui_paned_across(paned))
    {
        (void)tb_grid_bbox(widget, index, 0, index, 0, &rect);
    }
    else
    {
        (void)tb_grid_bbox(widget, 0, index, 0, index, &rect);
    }
    return rect;
}

static float tb_gui_paned_start(const tb_gui_paned *paned, tb_canvas_rect rect)
{
    return tb_gui_paned_across(paned) ? rect.x : rect.y;
}

static float tb_gui_paned_length(const tb_gui_paned *paned, tb_canvas_rect rect)
{
    return tb_gui_paned_across(paned) ? rect.width : rect.height;
}

/* Configure a pane's track: its share, its least size, in the group. */
static void tb_gui_paned_configure(tb_widget widget, const tb_gui_paned *paned, size_t pane,
                                   float share)
{
    const tb_grid_track track = {.weight = fmaxf(share, 1e-3f),
                                 .minsize = paned->panes[pane].minsize,
                                 .uniform = TB_GUI_PANES_GROUP};

    if (tb_gui_paned_across(paned))
    {
        (void)tb_grid_column_configure(widget, 2 * pane, &track);
    }
    else
    {
        (void)tb_grid_row_configure(widget, 2 * pane, &track);
    }
}

/* The sash at a point, or SIZE_MAX. */
static size_t tb_gui_paned_sash_at(tb_widget widget, const tb_gui_paned *paned, tb_vec2 point)
{
    for (size_t sash = 0; sash + 1 < paned->count; sash++)
    {
        if (tb_ui_rect_contains(tb_gui_paned_track(widget, paned, 2 * sash + 1), point))
        {
            return sash;
        }
    }
    return SIZE_MAX;
}

/* Move a sash: the pane before it takes `before` units along the axis, the
 * pane after it the rest of their two, each kept at least its least size;
 * every pane's share is then its size. */
static void tb_gui_paned_move(tb_widget widget, tb_gui_paned *paned, size_t sash, float before)
{
    const tb_canvas_rect first = tb_gui_paned_track(widget, paned, 2 * sash);
    const tb_canvas_rect second = tb_gui_paned_track(widget, paned, 2 * sash + 2);
    const float total = tb_gui_paned_length(paned, first) + tb_gui_paned_length(paned, second);
    const float low = paned->panes[sash].minsize;
    const float high = fmaxf(total - paned->panes[sash + 1].minsize, low);
    const float kept = fminf(fmaxf(before, low), high);

    for (size_t pane = 0; pane < paned->count; pane++)
    {
        float share = tb_gui_paned_length(paned, tb_gui_paned_track(widget, paned, 2 * pane));
        share = pane == sash ? kept : share;
        share = pane == sash + 1 ? total - kept : share;
        tb_gui_paned_configure(widget, paned, pane, share);
    }
}

static tb_vec2 tb_gui_paned_measure(tb_widget widget, const void *data)
{
    const tb_gui_paned *const paned = data;
    const float sash = tb_gui_sash_width(widget.gui);
    float along = 0.0f;
    float across = 0.0f;

    /* Each pane's natural size along the axis, and the largest across it. */
    for (size_t pane = 0; pane < paned->count; pane++)
    {
        const tb_vec2 natural = tb_widget_natural_size(paned->panes[pane].widget);
        const bool sideways = tb_gui_paned_across(paned);
        along += fmaxf(sideways ? natural.x : natural.y, paned->panes[pane].minsize);
        along += pane > 0 ? sash : 0.0f;
        across = fmaxf(across, sideways ? natural.y : natural.x);
    }
    const tb_vec2 wanted =
        tb_gui_paned_across(paned) ? (tb_vec2){along, across} : (tb_vec2){across, along};
    return (tb_vec2){paned->width > 0.0f ? paned->width : wanted.x,
                     paned->height > 0.0f ? paned->height : wanted.y};
}

/* Its grid fills it: the panes' group gives and takes the space. */
static tb_gui_viewport tb_gui_paned_viewport(tb_widget widget, void *data, tb_canvas_rect inner)
{
    (void)widget;
    (void)data;
    return (tb_gui_viewport){.clip = inner, .content = {inner.width, inner.height}};
}

static void tb_gui_paned_draw(tb_widget widget, const void *data, tb_gui_paint *paint)
{
    const tb_gui_paned *const paned = data;
    const tb_ui_theme *const theme = tb_gui_theme(widget.gui);

    for (size_t sash = 0; sash + 1 < paned->count; sash++)
    {
        const tb_canvas_rect rect = tb_gui_paned_track(widget, paned, 2 * sash + 1);
        const tb_ui_color color =
            sash == paned->dragging ? TB_UI_COLOR_ACCENT : TB_UI_COLOR_SEPARATOR;
        tb_gui_paint_fill(paint, rect, 0.0f, theme->colors[color]);
    }
}

static bool tb_gui_paned_event(tb_widget widget, void *data, const tb_gui_event *event)
{
    tb_gui_paned *const paned = data;
    const tb_cursor resize =
        tb_gui_paned_across(paned) ? TB_CURSOR_RESIZE_HORIZONTAL : TB_CURSOR_RESIZE_VERTICAL;

    switch (event->kind)
    {
    case TB_GUI_EVENT_PRESS: {
        const size_t sash = tb_gui_paned_sash_at(widget, paned, event->position);
        if (event->button != TB_MOUSE_BUTTON_LEFT || sash == SIZE_MAX)
        {
            return false;
        }
        const tb_canvas_rect rect = tb_gui_paned_track(widget, paned, 2 * sash + 1);
        paned->dragging = sash;
        paned->grab = (tb_gui_paned_across(paned) ? event->position.x : event->position.y) -
                      tb_gui_paned_start(paned, rect);
        return true;
    }
    case TB_GUI_EVENT_MOTION: {
        /* Over a sash, the cursor says it is dragged. */
        const bool over = tb_gui_paned_sash_at(widget, paned, event->position) != SIZE_MAX;
        tb_widget_set_cursor(widget,
                             over || paned->dragging != SIZE_MAX ? resize : TB_CURSOR_COUNT);
        if (paned->dragging == SIZE_MAX)
        {
            return false;
        }
        const tb_canvas_rect first = tb_gui_paned_track(widget, paned, 2 * paned->dragging);
        const float along = tb_gui_paned_across(paned) ? event->position.x : event->position.y;
        tb_gui_paned_move(widget, paned, paned->dragging,
                          along - paned->grab - tb_gui_paned_start(paned, first));
        return true;
    }
    case TB_GUI_EVENT_RELEASE:
        paned->dragging = SIZE_MAX;
        return true;
    case TB_GUI_EVENT_LEAVE:
        if (paned->dragging == SIZE_MAX)
        {
            tb_widget_set_cursor(widget, TB_CURSOR_COUNT);
        }
        return false;
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_DOUBLE_CLICK:
    case TB_GUI_EVENT_WHEEL:
    case TB_GUI_EVENT_KEY:
    case TB_GUI_EVENT_TEXT:
    case TB_GUI_EVENT_FOCUS_IN:
    case TB_GUI_EVENT_FOCUS_OUT:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    return false;
}

static bool tb_gui_paned_set_size(float *slot, const tb_gui_value *value)
{
    if (value->number < 0.0f)
    {
        return false;
    }
    *slot = value->number;
    return true;
}

static bool tb_gui_paned_set_width(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_paned *const paned = data;

    (void)widget;
    return tb_gui_paned_set_size(&paned->width, value);
}

static bool tb_gui_paned_get_width(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_paned *const paned = data;

    (void)widget;
    *value = tb_gui_float(paned->width);
    return true;
}

static bool tb_gui_paned_set_height(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_paned *const paned = data;

    (void)widget;
    return tb_gui_paned_set_size(&paned->height, value);
}

static bool tb_gui_paned_get_height(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_paned *const paned = data;

    (void)widget;
    *value = tb_gui_float(paned->height);
    return true;
}

/* Its panes are placed by its orientation: it is chosen before they are added. */
static bool tb_gui_paned_set_orient(tb_widget widget, void *data, const tb_gui_value *value)
{
    tb_gui_paned *const paned = data;

    (void)widget;
    if (paned->count > 0)
    {
        return false;
    }
    paned->orient = value->integer;
    return true;
}

static bool tb_gui_paned_get_orient(tb_widget widget, const void *data, tb_gui_value *value)
{
    const tb_gui_paned *const paned = data;

    (void)widget;
    *value = tb_gui_enum(paned->orient);
    return true;
}

static const tb_gui_property tb_gui_paned_properties[] = {
    {.name = "orient",
     .type = TB_GUI_TYPE_ENUM,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .names = tb_gui_paned_orient_names,
     .name_count = TB_GUI_COUNT_OF(tb_gui_paned_orient_names),
     .get = tb_gui_paned_get_orient,
     .set = tb_gui_paned_set_orient},
    {.name = "width",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_paned_get_width,
     .set = tb_gui_paned_set_width},
    {.name = "height",
     .type = TB_GUI_TYPE_FLOAT,
     .flags = TB_GUI_PROPERTY_LAYOUT,
     .get = tb_gui_paned_get_height,
     .set = tb_gui_paned_set_height},
};

/* What a layout file cannot hold yet (tb_widget_class::unsaved). */
static const char *tb_gui_paned_unsaved(tb_widget widget, const void *data)
{
    const tb_gui_paned *const paned = data;

    (void)widget;
    return paned->count > 0 ? "panes" : NULL;
}

static const tb_widget_class tb_gui_paned_kind = {.name = "panedwindow",
                                                  .data_size = sizeof(tb_gui_paned),
                                                  .properties = tb_gui_paned_properties,
                                                  .property_count =
                                                      TB_GUI_COUNT_OF(tb_gui_paned_properties),
                                                  .mouse_filter = TB_GUI_MOUSE_STOP,
                                                  .init = tb_gui_paned_init,
                                                  .destroy = tb_gui_paned_destroy,
                                                  .measure = tb_gui_paned_measure,
                                                  .draw = tb_gui_paned_draw,
                                                  .event = tb_gui_paned_event,
                                                  .viewport = tb_gui_paned_viewport,
                                                  .unsaved = tb_gui_paned_unsaved};

static tb_gui_paned *tb_gui_paned_of(tb_widget widget)
{
    return tb_widget_class_of(widget) == &tb_gui_paned_kind ? tb_widget_data(widget) : NULL;
}

const tb_widget_class *tb_panedwindow_class(void)
{
    return &tb_gui_paned_kind;
}

tb_widget tb_panedwindow(tb_widget parent, const tb_panedwindow_desc *desc)
{
    const tb_panedwindow_desc chosen = desc != NULL ? *desc : (tb_panedwindow_desc){0};
    const tb_widget widget = tb_widget_create(parent, &tb_gui_paned_kind);
    const tb_gui_value values[] = {tb_gui_enum((int32_t)chosen.orient), tb_gui_float(chosen.width),
                                   tb_gui_float(chosen.height)};
    static const char *const names[] = {"orient", "width", "height"};

    for (size_t index = 0; index < TB_GUI_COUNT_OF(names); index++)
    {
        (void)tb_widget_set(widget, names[index], &values[index]);
    }
    /* Across the axis, the panes fill it. */
    (void)tb_grid_row_configure(widget, 0, &(tb_grid_track){.weight = 1.0f});
    (void)tb_grid_column_configure(widget, 0, &(tb_grid_track){.weight = 1.0f});
    return widget;
}

size_t tb_panedwindow_add(tb_widget panedwindow, tb_widget pane, const tb_pane_options *options)
{
    tb_gui_paned *const paned = tb_gui_paned_of(panedwindow);
    const tb_pane_options chosen = options != NULL ? *options : (tb_pane_options){0};

    if (paned == NULL || !tb_widget_equal(tb_widget_parent(pane), panedwindow) ||
        !(chosen.weight >= 0.0f) || !(chosen.minsize >= 0.0f))
    {
        return SIZE_MAX;
    }
    if (paned->count == paned->capacity)
    {
        paned->capacity = paned->capacity == 0 ? TB_GUI_PANED_FIRST : paned->capacity * 2;
        paned->panes = tb_realloc_array(paned->panes, paned->capacity, sizeof(*paned->panes));
    }
    const size_t index = paned->count;
    paned->panes[index] = (tb_gui_pane){.widget = pane, .minsize = chosen.minsize};
    paned->count++;
    const int32_t track = (int32_t)(2 * index);
    const bool across = tb_gui_paned_across(paned);
    (void)tb_grid(pane, &(tb_grid_options){.row = across ? 0 : track,
                                           .column = across ? track : 0,
                                           .sticky = TB_STICKY_NSEW});
    tb_gui_paned_configure(panedwindow, paned, index, chosen.weight > 0.0f ? chosen.weight : 1.0f);
    if (index > 0)
    {
        const tb_grid_track sash = {.minsize = tb_gui_sash_width(panedwindow.gui)};
        if (across)
        {
            (void)tb_grid_column_configure(panedwindow, 2 * index - 1, &sash);
        }
        else
        {
            (void)tb_grid_row_configure(panedwindow, 2 * index - 1, &sash);
        }
    }
    return index;
}

size_t tb_panedwindow_pane_count(tb_widget panedwindow)
{
    const tb_gui_paned *const paned = tb_gui_paned_of(panedwindow);
    return paned != NULL ? paned->count : 0;
}

float tb_panedwindow_sash(tb_widget panedwindow, size_t sash)
{
    const tb_gui_paned *const paned = tb_gui_paned_of(panedwindow);

    if (paned == NULL || sash + 1 >= paned->count)
    {
        return 0.0f;
    }
    tb_gui_layout(panedwindow.gui);
    return tb_gui_paned_start(paned, tb_gui_paned_track(panedwindow, paned, 2 * sash + 1));
}

bool tb_panedwindow_set_sash(tb_widget panedwindow, size_t sash, float position)
{
    tb_gui_paned *const paned = tb_gui_paned_of(panedwindow);

    if (paned == NULL || sash + 1 >= paned->count || !isfinite(position))
    {
        return false;
    }
    tb_gui_layout(panedwindow.gui);
    const tb_canvas_rect first = tb_gui_paned_track(panedwindow, paned, 2 * sash);
    tb_gui_paned_move(panedwindow, paned, sash, position - tb_gui_paned_start(paned, first));
    return true;
}

void tb_gui_register_panedwindow(tb_gui *gui)
{
    (void)tb_gui_register_class(gui, &tb_gui_paned_kind);
}
