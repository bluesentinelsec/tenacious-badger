/* The interface's layout: windows that move, resize, collapse and scroll;
 * panels and the open list, which scroll too; and where each widget goes
 * in them: down a line at a time, on the same line, or in columns and
 * tables. */
#include "ui_internal.h"

#include "badger/assert.h"

#include <math.h>

/* The smallest a window is resized to, in units, below its title. */
#define TB_UI_SMALLEST_WINDOW_WIDTH 120.0f
#define TB_UI_SMALLEST_WINDOW_BODY 40.0f
/* How much of a window stays on the frame, in units, however it is moved. */
#define TB_UI_WINDOW_KEPT_SEEN 40.0f
/* The corner that resizes a window, and its edges' width, in units. */
#define TB_UI_GRIP_SIZE 14.0f
#define TB_UI_EDGE_WIDTH 4.0f
/* A scroll bar's grab is never shorter, in units. */
#define TB_UI_SHORTEST_GRAB 24.0f
/* Lines of text a notch of the wheel scrolls. */
#define TB_UI_WHEEL_LINES 3.0f
/* The gap between a combo and its list, in units. */
#define TB_UI_POPUP_GAP 2.0f

tb_ui_container *tb_ui_top(tb_ui *ui)
{
    TB_ASSERT_MSG(ui->container_count > 0, "a widget outside a window");
    return &ui->containers[ui->container_count - 1];
}

void tb_ui_push_container(tb_ui *ui, const tb_ui_container *container)
{
    TB_ASSERT_MSG(ui->container_count < TB_UI_MOST_CONTAINERS,
                  "windows and panels nested too deep");
    ui->containers[ui->container_count] = *container;
    ui->container_count++;
}

static float tb_ui_clamp(float value, float low, float high)
{
    return fminf(fmaxf(value, low), high);
}

/* Lay a container's contents out from its body's top, scrolled. */
static void tb_ui_begin_contents(tb_ui *ui, tb_ui_container *container, tb_ui_state *state)
{
    const float view = container->body.height;
    const float most = fmaxf(state->content - view, 0.0f);

    state->scroll = tb_ui_clamp(state->scroll, 0.0f, most);
    container->base_left = container->body.x + container->padding.x;
    container->left = container->base_left;
    container->right = container->body.x + container->body.width - container->padding.x -
                       (container->scrollbar ? ui->theme.scrollbar_width : 0.0f);
    container->top = container->body.y + container->padding.y - state->scroll;
    container->cursor = container->top;
    container->line_y = container->top;
    container->bottom = container->top;
    container->spacing = ui->theme.item_spacing.y;
}

/* ---------------------------------------------------------------------------
 * Windows.
 * ------------------------------------------------------------------------- */

/* The corner and edges that resize a window: true while one is dragged. */
static void tb_ui_window_resize(tb_ui *ui, tb_ui_container *container, tb_ui_state *state)
{
    const tb_canvas_rect rect = state->rect;
    const float right = rect.x + rect.width;
    const float bottom = rect.y + rect.height;
    const tb_canvas_rect corner = {right - TB_UI_GRIP_SIZE, bottom - TB_UI_GRIP_SIZE,
                                   TB_UI_GRIP_SIZE + TB_UI_EDGE_WIDTH,
                                   TB_UI_GRIP_SIZE + TB_UI_EDGE_WIDTH};
    const tb_canvas_rect right_edge = {right - TB_UI_EDGE_WIDTH, rect.y + ui->frame_height,
                                       2.0f * TB_UI_EDGE_WIDTH, rect.height - ui->frame_height};
    const tb_canvas_rect bottom_edge = {rect.x, bottom - TB_UI_EDGE_WIDTH, rect.width,
                                        2.0f * TB_UI_EDGE_WIDTH};
    const tb_vec2 moved = {ui->mouse.x - ui->last_mouse.x, ui->mouse.y - ui->last_mouse.y};
    const tb_ui_interaction grip =
        tb_ui_interact(ui, tb_ui_derived_id(container->id, "resize"), corner, false);
    const tb_ui_interaction across =
        tb_ui_interact(ui, tb_ui_derived_id(container->id, "resize right"), right_edge, false);
    const tb_ui_interaction down =
        tb_ui_interact(ui, tb_ui_derived_id(container->id, "resize bottom"), bottom_edge, false);

    container->grip_hot = grip.hot || grip.active;
    if ((grip.active && !grip.pressed) || (across.active && !across.pressed))
    {
        state->rect.width += moved.x;
    }
    if ((grip.active && !grip.pressed) || (down.active && !down.pressed))
    {
        state->rect.height += moved.y;
    }
    if (grip.hot || grip.active)
    {
        ui->cursor = TB_CURSOR_RESIZE_DIAGONAL_DOWN;
    }
    else if (across.hot || across.active)
    {
        ui->cursor = TB_CURSOR_RESIZE_HORIZONTAL;
    }
    else if (down.hot || down.active)
    {
        ui->cursor = TB_CURSOR_RESIZE_VERTICAL;
    }
}

/* The title bar: its arrow collapses the window, and dragging it moves it. */
static void tb_ui_window_title(tb_ui *ui, tb_ui_container *container, tb_ui_state *state)
{
    const float height = ui->frame_height;
    const tb_canvas_rect title = {state->rect.x, state->rect.y, state->rect.width, height};
    const tb_canvas_rect arrow = {state->rect.x, state->rect.y, height, height};

    if ((container->flags & TB_UI_WINDOW_NO_COLLAPSE) == 0 &&
        tb_ui_interact(ui, tb_ui_derived_id(container->id, "collapse"), arrow, false).clicked)
    {
        state->open = !state->open;
    }
    const tb_ui_interaction move =
        tb_ui_interact(ui, tb_ui_derived_id(container->id, "move"), title, false);
    if (move.active && !move.pressed && (container->flags & TB_UI_WINDOW_NO_MOVE) == 0)
    {
        state->rect.x += ui->mouse.x - ui->last_mouse.x;
        state->rect.y += ui->mouse.y - ui->last_mouse.y;
    }
}

/* Keep a window large enough, and enough of it on the frame to move back. */
static void tb_ui_window_keep(const tb_ui *ui, tb_ui_state *state, float title_height)
{
    tb_canvas_rect *const rect = &state->rect;

    rect->width = fmaxf(rect->width, TB_UI_SMALLEST_WINDOW_WIDTH);
    rect->height = fmaxf(rect->height, title_height + TB_UI_SMALLEST_WINDOW_BODY);
    if (ui->size.x > 0.0f && ui->size.y > 0.0f)
    {
        rect->x = tb_ui_clamp(rect->x, TB_UI_WINDOW_KEPT_SEEN - rect->width,
                              ui->size.x - TB_UI_WINDOW_KEPT_SEEN);
        rect->y = tb_ui_clamp(rect->y, 0.0f, fmaxf(ui->size.y - title_height, 0.0f));
    }
}

static void tb_ui_shadow(tb_ui *ui, tb_canvas_rect rect, float radius)
{
    tb_vec4 color = tb_ui_color_of(ui, TB_UI_COLOR_SHADOW);
    const tb_canvas_rect near = {rect.x - 1.0f, rect.y + 1.0f, rect.width + 2.0f,
                                 rect.height + 2.0f};
    const tb_canvas_rect far = {rect.x - 3.0f, rect.y + 1.0f, rect.width + 6.0f,
                                rect.height + 6.0f};

    color.w *= 0.5f;
    tb_ui_fill(ui, far, radius + 3.0f, color, 0);
    tb_ui_fill(ui, near, radius + 1.0f, color, 0);
}

static void tb_ui_window_chrome(tb_ui *ui, const tb_ui_container *container,
                                const tb_ui_state *state, const char *title, float title_height)
{
    const tb_canvas_rect rect = container->rect;
    const float radius = ui->theme.window_rounding;
    const bool front = ui->front_window == container->id;
    const tb_ui_id id = container->id;

    tb_ui_shadow(ui, rect, radius);
    tb_ui_fill(ui, rect, radius, tb_ui_color_of(ui, TB_UI_COLOR_WINDOW), id);
    if (title_height <= 0.0f)
    {
        return;
    }
    const tb_canvas_rect bar = {rect.x, rect.y, rect.width, title_height};
    const tb_ui_color bar_color = front ? TB_UI_COLOR_TITLE_ACTIVE : TB_UI_COLOR_TITLE;
    const tb_vec4 text = tb_ui_color_of(ui, front ? TB_UI_COLOR_TEXT : TB_UI_COLOR_TEXT_DIM);
    float left = rect.x + ui->theme.frame_padding.x;

    tb_ui_fill(ui, bar, radius, tb_ui_color_of(ui, bar_color), id);
    if (state->open)
    {
        /* Square the bar's corners where it meets the body. */
        tb_ui_fill(ui, (tb_canvas_rect){rect.x, rect.y + title_height - radius, rect.width, radius},
                   0.0f, tb_ui_color_of(ui, bar_color), id);
        tb_ui_fill(ui,
                   (tb_canvas_rect){rect.x, rect.y + title_height, rect.width, ui->theme.border},
                   0.0f, tb_ui_color_of(ui, TB_UI_COLOR_BORDER), id);
    }
    if ((container->flags & TB_UI_WINDOW_NO_COLLAPSE) == 0)
    {
        tb_ui_chevron(ui, (tb_vec2){rect.x + title_height * 0.5f, rect.y + title_height * 0.5f},
                      ui->fonts[TB_UI_FONT_REGULAR].cap_height * 0.8f, state->open, text, id);
        left = rect.x + title_height;
    }
    tb_ui_text_in(ui, TB_UI_FONT_HEADING, tb_ui_label_text(title),
                  (tb_canvas_rect){left, rect.y, rect.x + rect.width - left, title_height}, false,
                  text, id);
}

bool tb_ui_begin_window(tb_ui *ui, const char *title, tb_canvas_rect rect, uint32_t flags)
{
    const bool titled = (flags & TB_UI_WINDOW_NO_TITLE) == 0;
    tb_ui_container container = {0};
    bool created = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(title != NULL);
    TB_ASSERT_MSG(ui->in_frame, "a window outside a frame");
    TB_ASSERT_MSG(ui->container_count == 0, "a window begun inside another");
    container.kind = TB_UI_CONTAINER_WINDOW;
    container.id = tb_ui_get_id(ui, title);
    container.state = tb_ui_touch(ui, container.id, &created);
    container.root = container.id;
    container.flags = titled ? flags : flags | TB_UI_WINDOW_NO_COLLAPSE | TB_UI_WINDOW_NO_MOVE;
    container.id_depth = ui->id_count;
    container.clip = (tb_canvas_rect){0.0f, 0.0f, ui->size.x, ui->size.y};
    ui->windows++;

    tb_ui_state *state = tb_ui_state_at(ui, container.state);
    if (created)
    {
        state->window = true;
        state->open = true;
        state->rect = rect;
        ui->z_counter++;
        state->z = ui->z_counter;
        state->layer = ui->layer + ui->next_rank;
        ui->next_rank++;
        ui->front_window = container.id;
    }
    container.layer = state->layer;
    tb_ui_push_container(ui, &container);
    tb_ui_container *const top = tb_ui_top(ui);

    /* The interactions touch other ids, which may move the states. */
    const float title_height = titled ? ui->frame_height : 0.0f;
    if (state->open && (flags & TB_UI_WINDOW_NO_RESIZE) == 0)
    {
        tb_ui_window_resize(ui, top, state);
        state = tb_ui_state_at(ui, top->state);
    }
    if (titled)
    {
        tb_ui_window_title(ui, top, state);
        state = tb_ui_state_at(ui, top->state);
    }
    tb_ui_window_keep(ui, state, title_height);
    top->rect = state->rect;
    if (!state->open)
    {
        top->rect.height = title_height;
    }
    state->hit = top->rect;
    if (state->open && (flags & TB_UI_WINDOW_NO_RESIZE) == 0)
    {
        state->hit.width += TB_UI_EDGE_WIDTH;
        state->hit.height += TB_UI_EDGE_WIDTH;
    }
    tb_ui_window_chrome(ui, top, state, title, title_height);
    if (!state->open)
    {
        ui->container_count--;
        return false;
    }
    tb_ui_push_raw_id(ui, top->id);
    top->body = (tb_canvas_rect){top->rect.x, top->rect.y + title_height, top->rect.width,
                                 top->rect.height - title_height};
    top->clip = tb_ui_rect_intersect(
        tb_ui_rect_inset(top->body, ui->theme.border, ui->theme.border), top->clip);
    top->padding = ui->theme.window_padding;
    top->scrollbar = (flags & TB_UI_WINDOW_NO_SCROLLBAR) == 0 && state->content > top->body.height;
    tb_ui_begin_contents(ui, top, state);
    return true;
}

void tb_ui_end_window(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT_MSG(ui->container_count == 1 && tb_ui_top(ui)->kind == TB_UI_CONTAINER_WINDOW,
                  "a window ended that is not the one begun");
    tb_ui_end_container(ui);
}

tb_canvas_rect tb_ui_window_rect(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT_MSG(ui->container_count > 0, "no window is being built");
    return ui->containers[0].rect;
}

/* ---------------------------------------------------------------------------
 * Panels and the list of a combo.
 * ------------------------------------------------------------------------- */

void tb_ui_begin_panel(tb_ui *ui, const char *name, float height)
{
    tb_ui_container *const parent = tb_ui_top(ui);
    const float left_over =
        parent->clip.y + parent->clip.height - parent->cursor - parent->padding.y;
    const float wanted = height > 0.0f ? height : fmaxf(left_over, ui->frame_height);
    const tb_canvas_rect item = tb_ui_next_item(ui, 0.0f, wanted, true);
    tb_ui_container container = {0};

    TB_ASSERT(name != NULL);
    container.kind = TB_UI_CONTAINER_PANEL;
    container.id = tb_ui_get_id(ui, name);
    container.state = tb_ui_touch(ui, container.id, NULL);
    container.root = parent->root;
    container.layer = parent->layer;
    container.id_depth = ui->id_count;
    container.rect = item;
    container.body = tb_ui_rect_inset(item, ui->theme.border, ui->theme.border);
    container.clip = tb_ui_rect_intersect(container.body, parent->clip);
    container.padding = ui->theme.frame_padding;
    tb_ui_fill(ui, item, ui->theme.frame_rounding, tb_ui_color_of(ui, TB_UI_COLOR_FRAME),
               container.id);

    tb_ui_state *const state = tb_ui_state_at(ui, container.state);
    container.scrollbar = state->content > container.body.height;
    tb_ui_push_container(ui, &container);
    tb_ui_begin_contents(ui, tb_ui_top(ui), state);
    tb_ui_push_raw_id(ui, container.id);
}

void tb_ui_end_panel(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT_MSG(tb_ui_top(ui)->kind == TB_UI_CONTAINER_PANEL,
                  "a panel ended that is not the one begun");
    tb_ui_end_container(ui);
}

void tb_ui_begin_popup(tb_ui *ui, tb_ui_id owner, tb_canvas_rect below, float height)
{
    tb_ui_container container = {0};
    const float padding = ui->theme.frame_padding.y * 0.5f;
    tb_canvas_rect rect = {below.x, below.y + below.height + TB_UI_POPUP_GAP, below.width,
                           height + 2.0f * padding + 2.0f * ui->theme.border};

    /* A list that would leave the bottom of the frame opens above, if it fits there. */
    if (rect.y + rect.height > ui->size.y && below.y - TB_UI_POPUP_GAP - rect.height >= 0.0f)
    {
        rect.y = below.y - TB_UI_POPUP_GAP - rect.height;
    }
    container.kind = TB_UI_CONTAINER_POPUP;
    container.id = tb_ui_derived_id(owner, "list");
    container.state = tb_ui_touch(ui, container.id, NULL);
    container.root = container.id;
    container.layer = ui->layer + TB_UI_POPUP_LAYERS;
    container.id_depth = ui->id_count;
    container.rect = rect;
    container.body = tb_ui_rect_inset(rect, ui->theme.border, ui->theme.border);
    container.clip =
        tb_ui_rect_intersect(container.body, (tb_canvas_rect){0.0f, 0.0f, ui->size.x, ui->size.y});
    container.padding = (tb_vec2){0.0f, padding};

    tb_ui_state *const state = tb_ui_state_at(ui, container.state);
    container.scrollbar = state->content > container.body.height;
    tb_ui_push_container(ui, &container);
    tb_ui_begin_contents(ui, tb_ui_top(ui), state);
    tb_ui_top(ui)->spacing = 0.0f;
    tb_ui_shadow(ui, rect, ui->theme.frame_rounding);
    tb_ui_fill(ui, rect, ui->theme.frame_rounding, tb_ui_color_of(ui, TB_UI_COLOR_POPUP),
               container.id);
    tb_ui_push_raw_id(ui, container.id);
    ui->popup_next = rect;
    ui->popup_seen = true;
}

void tb_ui_end_popup(tb_ui *ui)
{
    TB_ASSERT_MSG(tb_ui_top(ui)->kind == TB_UI_CONTAINER_POPUP, "a list ended that was not begun");
    tb_ui_end_container(ui);
}

/* ---------------------------------------------------------------------------
 * Scrolling, and the end of a container.
 * ------------------------------------------------------------------------- */

static void tb_ui_scrollbar(tb_ui *ui, const tb_ui_container *container, size_t index)
{
    const float view = container->body.height;
    const float width = ui->theme.scrollbar_width;
    const tb_canvas_rect track = {container->body.x + container->body.width - width,
                                  container->body.y, width, view};
    const tb_ui_id id = tb_ui_derived_id(container->id, "scroll");
    const tb_ui_interaction scroll = tb_ui_interact(ui, id, track, false);
    tb_ui_state *const state = tb_ui_state_at(ui, index);
    const float most = state->content - view;
    const float grab_height = fmaxf(view * view / state->content, TB_UI_SHORTEST_GRAB);
    const float travel = fmaxf(view - grab_height, 1.0f);
    float grab_y = track.y + travel * state->scroll / most;

    if (scroll.pressed && (ui->mouse.y < grab_y || ui->mouse.y >= grab_y + grab_height))
    {
        /* A press on the track puts the grab's middle under the mouse. */
        state->scroll = (ui->mouse.y - track.y - grab_height * 0.5f) * most / travel;
    }
    else if (scroll.active && !scroll.pressed)
    {
        state->scroll += (ui->mouse.y - ui->last_mouse.y) * most / travel;
    }
    state->scroll = tb_ui_clamp(state->scroll, 0.0f, most);
    grab_y = track.y + travel * state->scroll / most;
    tb_ui_fill(ui, track, 0.0f, tb_ui_color_of(ui, TB_UI_COLOR_SCROLLBAR), id);
    tb_ui_fill(ui,
               (tb_canvas_rect){track.x + 2.0f, grab_y + 2.0f, width - 4.0f, grab_height - 4.0f},
               (width - 4.0f) * 0.5f,
               tb_ui_color_of(ui, scroll.hot || scroll.active ? TB_UI_COLOR_SCROLLBAR_GRAB_HOT
                                                              : TB_UI_COLOR_SCROLLBAR_GRAB),
               id);
}

/* The wheel scrolls the innermost container under the mouse that scrolls. */
static void tb_ui_wheel(tb_ui *ui, const tb_ui_container *container, tb_ui_state *state)
{
    const float most = state->content - container->body.height;

    if (ui->wheel_taken || ui->input.wheel.y == 0.0f || !(most > 0.0f) ||
        !tb_ui_mouse_over(ui, container->clip))
    {
        return;
    }
    state->scroll = tb_ui_clamp(
        state->scroll - ui->input.wheel.y * TB_UI_WHEEL_LINES * ui->frame_height, 0.0f, most);
    ui->wheel_taken = true;
}

static void tb_ui_window_grip(tb_ui *ui, const tb_ui_container *container)
{
    const tb_canvas_rect rect = container->rect;
    const float right = rect.x + rect.width - 3.0f;
    const float bottom = rect.y + rect.height - 3.0f;
    tb_vec4 color =
        tb_ui_color_of(ui, container->grip_hot ? TB_UI_COLOR_ACCENT_HOT : TB_UI_COLOR_TEXT_DIM);

    color.w *= container->grip_hot ? 1.0f : 0.45f;
    for (int line = 1; line <= 2; line++)
    {
        const float reach = 4.0f * (float)line;
        tb_ui_line(ui, (tb_vec2){right - reach, bottom}, (tb_vec2){right, bottom - reach}, 1.0f,
                   color, container->id);
    }
}

void tb_ui_end_container(tb_ui *ui)
{
    tb_ui_container *const container = tb_ui_top(ui);
    tb_ui_state *const state = tb_ui_state_at(ui, container->state);
    const bool window = container->kind == TB_UI_CONTAINER_WINDOW;

    TB_ASSERT_MSG(!container->table, "a table not ended");
    state->content = container->bottom > container->top
                         ? container->bottom - container->top + 2.0f * container->padding.y
                         : 0.0f;
    if (container->scrollbar)
    {
        tb_ui_scrollbar(ui, container, container->state);
    }
    tb_ui_wheel(ui, container, tb_ui_state_at(ui, container->state));
    /* The edge is drawn over the contents, which reach under it. */
    container->clip = container->kind == TB_UI_CONTAINER_PANEL
                          ? ui->containers[ui->container_count - 2].clip
                          : (tb_canvas_rect){0.0f, 0.0f, ui->size.x, ui->size.y};
    tb_ui_outline(ui, container->rect,
                  window ? ui->theme.window_rounding : ui->theme.frame_rounding, ui->theme.border,
                  tb_ui_color_of(ui, TB_UI_COLOR_BORDER), container->id);
    if (window && (container->flags & TB_UI_WINDOW_NO_RESIZE) == 0)
    {
        tb_ui_window_grip(ui, container);
    }
    ui->id_count = container->id_depth;
    ui->container_count--;
}

/* ---------------------------------------------------------------------------
 * Where widgets go.
 * ------------------------------------------------------------------------- */

/* A table's every other row is shaded, under its cells. */
static void tb_ui_table_row(tb_ui *ui, tb_ui_container *container)
{
    container->table_row++;
    if (container->table_row % 2 == 0)
    {
        tb_ui_fill(ui,
                   (tb_canvas_rect){container->table_left, container->line_y,
                                    container->table_right - container->table_left,
                                    ui->frame_height},
                   0.0f, tb_ui_color_of(ui, TB_UI_COLOR_TABLE_ROW_ALTERNATE), 0);
    }
}

tb_canvas_rect tb_ui_next_item(tb_ui *ui, float width, float height, bool fill)
{
    tb_ui_container *const container = tb_ui_top(ui);
    const float across = ui->theme.item_spacing.x;
    tb_canvas_rect rect = {0};

    if (container->columns > 0)
    {
        const size_t column = container->column;
        if (column == 0)
        {
            container->line_y = container->cursor;
            container->line_height = 0.0f;
            if (container->table)
            {
                tb_ui_table_row(ui, container);
            }
        }
        rect = (tb_canvas_rect){container->column_x[column], container->line_y,
                                container->column_width[column], height};
        container->column = (column + 1) % container->columns;
    }
    else if (container->same_line)
    {
        const float x = container->line_end + across;
        rect = (tb_canvas_rect){x, container->line_y,
                                fill ? fmaxf(container->right - x, 0.0f) : width, height};
    }
    else
    {
        container->line_y = container->cursor;
        container->line_height = 0.0f;
        rect = (tb_canvas_rect){container->left, container->line_y,
                                fill ? container->right - container->left : width, height};
    }
    container->same_line = false;
    container->line_height = fmaxf(container->line_height, height);
    container->line_end = rect.x + rect.width;
    container->cursor = container->line_y + container->line_height + container->spacing;
    container->bottom = fmaxf(container->bottom, container->line_y + container->line_height);
    return rect;
}

void tb_ui_split_label(tb_ui *ui, tb_canvas_rect item, const char *label, tb_canvas_rect *field)
{
    const tb_text shown = tb_ui_label_text(label);
    const float label_width = roundf(item.width * ui->theme.label_fraction);

    if (shown.length == 0)
    {
        *field = item;
        return;
    }
    tb_ui_text_in(
        ui, TB_UI_FONT_REGULAR, shown,
        (tb_canvas_rect){item.x, item.y, label_width - ui->theme.item_spacing.x, item.height},
        false, tb_ui_color_of(ui, TB_UI_COLOR_TEXT), 0);
    *field = (tb_canvas_rect){item.x + label_width, item.y, item.width - label_width, item.height};
}

void tb_ui_row(tb_ui *ui, size_t columns, const float *widths)
{
    tb_ui_container *const container = tb_ui_top(ui);
    const float across = ui->theme.item_spacing.x;
    float fixed = 0.0f;
    size_t shared = 0;
    float x = container->left;

    TB_ASSERT_MSG(columns <= TB_UI_MOST_COLUMNS, "too many columns");
    container->columns = columns;
    container->column = 0;
    if (columns == 0)
    {
        return;
    }
    for (size_t column = 0; column < columns; column++)
    {
        const float width = widths != NULL ? widths[column] : 0.0f;
        fixed += width > 0.0f ? width : 0.0f;
        shared += width > 0.0f ? 0 : 1;
    }
    const float share =
        shared > 0
            ? fmaxf(container->right - container->left - across * (float)(columns - 1) - fixed,
                    0.0f) /
                  (float)shared
            : 0.0f;
    for (size_t column = 0; column < columns; column++)
    {
        const float width = widths != NULL && widths[column] > 0.0f ? widths[column] : share;
        container->column_x[column] = x;
        container->column_width[column] = width;
        x += width + across;
    }
}

void tb_ui_same_line(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    tb_ui_top(ui)->same_line = true;
}

void tb_ui_spacing(tb_ui *ui)
{
    tb_ui_container *container = NULL;

    TB_ASSERT(ui != NULL);
    container = tb_ui_top(ui);
    container->cursor += ui->theme.item_spacing.y;
    container->bottom = fmaxf(container->bottom, container->cursor);
}

void tb_ui_separator(tb_ui *ui)
{
    tb_ui_container *container = NULL;
    tb_canvas_rect item = {0};

    TB_ASSERT(ui != NULL);
    container = tb_ui_top(ui);
    container->columns = 0;
    item = tb_ui_next_item(ui, 0.0f, ui->theme.border, true);
    tb_ui_fill(ui,
               (tb_canvas_rect){container->base_left, item.y,
                                container->right - container->base_left, ui->theme.border},
               0.0f, tb_ui_color_of(ui, TB_UI_COLOR_SEPARATOR), 0);
}

void tb_ui_indent(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    tb_ui_top(ui)->left += ui->theme.indent;
}

void tb_ui_unindent(tb_ui *ui)
{
    tb_ui_container *container = NULL;

    TB_ASSERT(ui != NULL);
    container = tb_ui_top(ui);
    container->left = fmaxf(container->left - ui->theme.indent, container->base_left);
}

/* ---------------------------------------------------------------------------
 * Tables.
 * ------------------------------------------------------------------------- */

void tb_ui_begin_table(tb_ui *ui, const char *name, size_t columns, const char *const *headers,
                       const float *widths)
{
    tb_ui_container *container = NULL;
    const float margin = ui->theme.frame_padding.x * 0.5f;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(name != NULL);
    TB_ASSERT_MSG(columns >= 1 && columns <= TB_UI_MOST_COLUMNS, "a table of 1 to 16 columns");
    container = tb_ui_top(ui);
    TB_ASSERT_MSG(!container->table, "a table inside a table");
    tb_ui_push_id(ui, name);
    tb_ui_row(ui, columns, widths);
    container->table = true;
    container->table_row = 0;
    container->table_spacing = container->spacing;
    container->spacing = 0.0f;
    container->table_top = container->cursor;
    container->table_left = container->left - margin;
    container->table_right = container->right + margin;
    if (headers == NULL)
    {
        return;
    }
    const tb_canvas_rect bar = {container->table_left, container->cursor,
                                container->table_right - container->table_left, ui->frame_height};
    tb_ui_fill(ui, bar, 0.0f, tb_ui_color_of(ui, TB_UI_COLOR_HEADER), 0);
    for (size_t column = 0; column < columns; column++)
    {
        TB_ASSERT(headers[column] != NULL);
        tb_ui_text_in(ui, TB_UI_FONT_HEADING, tb_text_from_cstr(headers[column]),
                      (tb_canvas_rect){container->column_x[column], bar.y,
                                       container->column_width[column], bar.height},
                      false, tb_ui_color_of(ui, TB_UI_COLOR_TEXT), 0);
    }
    container->cursor += ui->frame_height;
    container->bottom = fmaxf(container->bottom, container->cursor);
}

void tb_ui_end_table(tb_ui *ui)
{
    tb_ui_container *container = NULL;

    TB_ASSERT(ui != NULL);
    container = tb_ui_top(ui);
    TB_ASSERT_MSG(container->table, "a table ended that was not begun");
    for (size_t column = 1; column < container->columns; column++)
    {
        const float x = roundf(container->column_x[column] - ui->theme.item_spacing.x * 0.5f);
        tb_ui_fill(ui,
                   (tb_canvas_rect){x, container->table_top, ui->theme.border,
                                    container->cursor - container->table_top},
                   0.0f, tb_ui_color_of(ui, TB_UI_COLOR_SEPARATOR), 0);
    }
    container->table = false;
    container->columns = 0;
    container->column = 0;
    container->spacing = container->table_spacing;
    container->cursor += container->spacing;
    tb_ui_pop_id(ui);
}
