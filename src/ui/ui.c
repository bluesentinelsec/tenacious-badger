/* The interface's context: themes, input, frames, ids, the state kept by
 * id, the mouse's hot and active widgets, and the keyboard's focus. */
#include "ui_internal.h"

#include "badger/assert.h"
#include "badger/font_raster.h"
#include "badger/hash.h"
#include "badger/memory.h"
#include "badger/str.h"

#include <math.h>
#include <string.h>

/* The seed of ids pushed on nothing. */
#define TB_UI_ROOT_SEED 0x2545F491u
/* An index's first size, and how full it may be: a quarter empty. */
#define TB_UI_FIRST_SLOTS 64u
/* The arena of a frame's text: a chunk's size. */
#define TB_UI_ARENA_CHUNK (64u * 1024u)
/* The bit of a mouse button. */
#define TB_UI_LEFT (1u << TB_MOUSE_BUTTON_LEFT)

static tb_vec4 tb_ui_gray(float value, float alpha)
{
    return (tb_vec4){value, value, value, alpha};
}

tb_ui_theme tb_ui_theme_dark(void)
{
    tb_ui_theme theme = {0};
    const tb_vec4 accent = {0.0f, 0.44f, 0.88f, 1.0f};

    theme.colors[TB_UI_COLOR_TEXT] = tb_ui_gray(0.86f, 1.0f);
    theme.colors[TB_UI_COLOR_TEXT_DIM] = tb_ui_gray(0.52f, 1.0f);
    theme.colors[TB_UI_COLOR_WINDOW] = tb_ui_gray(0.141f, 1.0f);
    theme.colors[TB_UI_COLOR_BORDER] = tb_ui_gray(0.05f, 1.0f);
    theme.colors[TB_UI_COLOR_TITLE] = tb_ui_gray(0.085f, 1.0f);
    theme.colors[TB_UI_COLOR_TITLE_ACTIVE] = tb_ui_gray(0.105f, 1.0f);
    theme.colors[TB_UI_COLOR_FRAME] = tb_ui_gray(0.059f, 1.0f);
    theme.colors[TB_UI_COLOR_FRAME_HOT] = tb_ui_gray(0.09f, 1.0f);
    theme.colors[TB_UI_COLOR_FRAME_ACTIVE] = tb_ui_gray(0.04f, 1.0f);
    theme.colors[TB_UI_COLOR_BUTTON] = tb_ui_gray(0.22f, 1.0f);
    theme.colors[TB_UI_COLOR_BUTTON_HOT] = tb_ui_gray(0.30f, 1.0f);
    theme.colors[TB_UI_COLOR_BUTTON_ACTIVE] = tb_ui_gray(0.16f, 1.0f);
    theme.colors[TB_UI_COLOR_HEADER] = tb_ui_gray(0.185f, 1.0f);
    theme.colors[TB_UI_COLOR_HEADER_HOT] = tb_ui_gray(0.25f, 1.0f);
    theme.colors[TB_UI_COLOR_ACCENT] = accent;
    theme.colors[TB_UI_COLOR_ACCENT_HOT] = (tb_vec4){0.15f, 0.56f, 1.0f, 1.0f};
    theme.colors[TB_UI_COLOR_SELECTION] = (tb_vec4){accent.x, accent.y, accent.z, 0.55f};
    theme.colors[TB_UI_COLOR_SCROLLBAR] = tb_ui_gray(0.0f, 0.18f);
    theme.colors[TB_UI_COLOR_SCROLLBAR_GRAB] = tb_ui_gray(0.32f, 1.0f);
    theme.colors[TB_UI_COLOR_SCROLLBAR_GRAB_HOT] = tb_ui_gray(0.45f, 1.0f);
    theme.colors[TB_UI_COLOR_SEPARATOR] = tb_ui_gray(0.075f, 1.0f);
    theme.colors[TB_UI_COLOR_POPUP] = tb_ui_gray(0.10f, 1.0f);
    theme.colors[TB_UI_COLOR_TABLE_ROW_ALTERNATE] = tb_ui_gray(1.0f, 0.035f);
    theme.colors[TB_UI_COLOR_SHADOW] = tb_ui_gray(0.0f, 0.30f);
    theme.font_size = tb_font_default_role(TB_FONT_ROLE_UI).size;
    theme.window_padding = (tb_vec2){10.0f, 10.0f};
    theme.frame_padding = (tb_vec2){8.0f, 4.0f};
    theme.item_spacing = (tb_vec2){8.0f, 6.0f};
    theme.indent = 18.0f;
    theme.window_rounding = 6.0f;
    theme.frame_rounding = 3.0f;
    theme.border = 1.0f;
    theme.scrollbar_width = 10.0f;
    theme.label_fraction = 0.4f;
    theme.tooltip_delay = 0.5f;
    return theme;
}

/* Open the font of a spec; NULL when the catalog has none. */
static tb_font *tb_ui_open_spec(const tb_font_catalog *catalog, tb_font_spec spec)
{
    size_t index = 0;
    tb_font *font = NULL;

    if (!tb_font_catalog_find_spec(catalog, spec, &index) ||
        !tb_font_catalog_open(catalog, index, NULL, &font))
    {
        return NULL;
    }
    return font;
}

bool tb_ui_fonts_open(const tb_font_catalog *catalog, const tb_font_spec *specs, tb_ui_fonts *fonts)
{
    tb_font_spec roles[TB_UI_FONT_COUNT] = {0};
    const tb_font_spec *chosen = specs;

    TB_ASSERT(catalog != NULL);
    TB_ASSERT(fonts != NULL);
    if (chosen == NULL)
    {
        /* The roles' own faces, none bold: the interface is bold only where
         * its caller asks. */
        roles[TB_UI_FONT_REGULAR] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_UI);
        roles[TB_UI_FONT_HEADING] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_HEADING);
        roles[TB_UI_FONT_MONO] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_MONOSPACE);
        chosen = roles;
    }
    for (size_t index = 0; index < TB_UI_FONT_COUNT; index++)
    {
        fonts->fonts[index] = tb_ui_open_spec(catalog, chosen[index]);
    }
    fonts->size = chosen[TB_UI_FONT_REGULAR].size;
    return fonts->fonts[TB_UI_FONT_REGULAR] != NULL;
}

void tb_ui_fonts_close(tb_ui_fonts *fonts)
{
    if (fonts == NULL)
    {
        return;
    }
    for (size_t index = 0; index < TB_UI_FONT_COUNT; index++)
    {
        tb_font_close(fonts->fonts[index]);
        fonts->fonts[index] = NULL;
    }
}

void tb_ui_theme_set_fonts(tb_ui_theme *theme, const tb_ui_fonts *fonts)
{
    TB_ASSERT(theme != NULL);
    TB_ASSERT(fonts != NULL);
    for (size_t index = 0; index < TB_UI_FONT_COUNT; index++)
    {
        theme->fonts[index] = fonts->fonts[index];
    }
    if (fonts->size > 0.0f)
    {
        theme->font_size = fonts->size;
    }
}

/* ---------------------------------------------------------------------------
 * Input.
 * ------------------------------------------------------------------------- */

static uint32_t tb_ui_button_bit(tb_mouse_button button)
{
    if (button == TB_MOUSE_BUTTON_NONE || (unsigned)button >= TB_MOUSE_BUTTON_COUNT)
    {
        return 0;
    }
    return 1u << button;
}

static void tb_ui_input_add_text(tb_ui_input *input, const char *text)
{
    const size_t length = tb_str_length(text);

    /* Whole events only: an event is whole characters. */
    if (length > TB_UI_TEXT_CAPACITY - input->text_length)
    {
        return;
    }
    memcpy(input->text + input->text_length, text, length);
    input->text_length += length;
}

static void tb_ui_input_add_key(tb_ui_input *input, const tb_window_event *event)
{
    if (input->key_count == TB_UI_MOST_KEYS)
    {
        return;
    }
    input->keys[input->key_count] = (tb_ui_key_press){
        .key = event->key, .modifiers = event->modifiers, .text_offset = input->text_length};
    input->key_count++;
}

void tb_ui_input_add_event(tb_ui_input *input, const tb_window_event *event)
{
    const tb_window_event_type type = event->type;

    TB_ASSERT(input != NULL);
    TB_ASSERT(event != NULL);
    if (type == TB_WINDOW_EVENT_MOUSE_MOVED || type == TB_WINDOW_EVENT_MOUSE_ENTERED ||
        type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN || type == TB_WINDOW_EVENT_MOUSE_BUTTON_UP ||
        type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        input->mouse = (tb_vec2){(float)event->pixel_x, (float)event->pixel_y};
        input->mouse_outside = false;
        input->modifiers = event->modifiers;
    }
    if (type == TB_WINDOW_EVENT_MOUSE_BUTTON_DOWN)
    {
        input->buttons |= tb_ui_button_bit(event->button);
        input->pressed |= tb_ui_button_bit(event->button);
    }
    else if (type == TB_WINDOW_EVENT_MOUSE_BUTTON_UP)
    {
        input->buttons &= ~tb_ui_button_bit(event->button);
        input->released |= tb_ui_button_bit(event->button);
    }
    else if (type == TB_WINDOW_EVENT_MOUSE_WHEEL)
    {
        input->wheel.x += (float)event->delta_x;
        input->wheel.y += (float)event->delta_y;
    }
    else if (type == TB_WINDOW_EVENT_MOUSE_LEFT)
    {
        input->mouse_outside = true;
    }
    else if (type == TB_WINDOW_EVENT_KEY_DOWN)
    {
        input->modifiers = event->modifiers;
        tb_ui_input_add_key(input, event);
    }
    else if (type == TB_WINDOW_EVENT_KEY_UP)
    {
        input->modifiers = event->modifiers;
    }
    else if (type == TB_WINDOW_EVENT_TEXT_INPUT)
    {
        tb_ui_input_add_text(input, event->text);
    }
    else if (type == TB_WINDOW_EVENT_FOCUS_LOST)
    {
        /* Buttons and keys that come up elsewhere are never reported. */
        input->released |= input->buttons;
        input->buttons = 0;
        input->modifiers = 0;
    }
}

void tb_ui_input_next_frame(tb_ui_input *input)
{
    TB_ASSERT(input != NULL);
    input->pressed = 0;
    input->released = 0;
    input->wheel = (tb_vec2){0};
    input->key_count = 0;
    input->text_length = 0;
}

/* ---------------------------------------------------------------------------
 * Contexts.
 * ------------------------------------------------------------------------- */

tb_ui *tb_ui_create(const tb_ui_options *options)
{
    static const tb_ui_options defaults = {0};
    const tb_ui_options *const chosen = options != NULL ? options : &defaults;
    tb_ui *const ui = TB_NEW(tb_ui);

    ui->theme = chosen->theme != NULL ? *chosen->theme : tb_ui_theme_dark();
    ui->layer = chosen->layer != 0 ? chosen->layer : TB_UI_DEFAULT_LAYER;
    ui->forget_frames =
        chosen->forget_frames != 0 ? chosen->forget_frames : TB_UI_DEFAULT_FORGET_FRAMES;
    ui->arena = tb_arena_create(TB_UI_ARENA_CHUNK);
    ui->scale = 1.0f;
    ui->paint.arena = ui->arena;
    ui->paint.scale = 1.0f;
    ui->clipboard = tb_calloc(TB_UI_CLIPBOARD_CAPACITY, 1);
    return ui;
}

void tb_ui_destroy(tb_ui *ui)
{
    if (ui == NULL)
    {
        return;
    }
    tb_arena_destroy(ui->arena);
    tb_free(ui->states);
    tb_free(ui->slots);
    tb_free(ui->focusables);
    tb_ui_paint_free(&ui->paint);
    tb_free(ui->clipboard);
    tb_free(ui);
}

void tb_ui_set_theme(tb_ui *ui, const tb_ui_theme *theme)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(theme != NULL);
    ui->theme = *theme;
}

const tb_ui_theme *tb_ui_get_theme(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return &ui->theme;
}

/* ---------------------------------------------------------------------------
 * Ids, and the state kept by them.
 * ------------------------------------------------------------------------- */

tb_ui_id tb_ui_hash_id(tb_ui_id seed, const void *bytes, size_t length)
{
    const uint64_t hash = tb_hash_u64(tb_hash_bytes(bytes, length) ^ ((uint64_t)seed << 32));
    const tb_ui_id id = (tb_ui_id)(hash ^ (hash >> 32));

    return id != 0 ? id : 1;
}

tb_ui_id tb_ui_derived_id(tb_ui_id id, const char *salt)
{
    return tb_ui_hash_id(id, salt, tb_str_length(salt));
}

static tb_ui_id tb_ui_seed(const tb_ui *ui)
{
    return ui->id_count > 0 ? ui->ids[ui->id_count - 1] : TB_UI_ROOT_SEED;
}

tb_ui_id tb_ui_get_id(const tb_ui *ui, const char *label)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    return tb_ui_hash_id(tb_ui_seed(ui), label, tb_str_length(label));
}

void tb_ui_push_raw_id(tb_ui *ui, tb_ui_id id)
{
    TB_ASSERT_MSG(ui->id_count < TB_UI_MOST_IDS, "ids pushed too deep");
    ui->ids[ui->id_count] = id;
    ui->id_count++;
}

void tb_ui_push_id(tb_ui *ui, const char *name)
{
    tb_ui_push_raw_id(ui, tb_ui_get_id(ui, name));
}

void tb_ui_push_id_int(tb_ui *ui, int64_t value)
{
    TB_ASSERT(ui != NULL);
    tb_ui_push_raw_id(ui, tb_ui_hash_id(tb_ui_seed(ui), &value, sizeof(value)));
}

void tb_ui_pop_id(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT_MSG(ui->id_count > 0, "an id popped that was not pushed");
    ui->id_count--;
}

tb_text tb_ui_label_text(const char *label)
{
    const tb_text whole = tb_text_from_cstr(label);
    const size_t hidden = tb_text_find(whole, 0, whole.length, TB_TEXT("##"));

    return hidden == TB_TEXT_NOT_FOUND ? whole : tb_text_sub(whole, 0, hidden);
}

static size_t tb_ui_slot_of(tb_ui_id id, size_t slot_count)
{
    return (size_t)tb_hash_u64(id) & (slot_count - 1);
}

/* The index of an id's state, or state_count when it has none. */
static size_t tb_ui_find(const tb_ui *ui, tb_ui_id id)
{
    if (ui->slot_count == 0)
    {
        return ui->state_count;
    }
    for (size_t slot = tb_ui_slot_of(id, ui->slot_count);; slot = (slot + 1) & (ui->slot_count - 1))
    {
        const uint32_t entry = ui->slots[slot];
        if (entry == 0)
        {
            return ui->state_count;
        }
        if (ui->states[entry - 1].id == id)
        {
            return entry - 1;
        }
    }
}

static void tb_ui_index(tb_ui *ui, size_t index)
{
    size_t slot = tb_ui_slot_of(ui->states[index].id, ui->slot_count);

    while (ui->slots[slot] != 0)
    {
        slot = (slot + 1) & (ui->slot_count - 1);
    }
    ui->slots[slot] = (uint32_t)(index + 1);
}

/* Index every state again, in an index large enough for them. */
static void tb_ui_reindex(tb_ui *ui)
{
    size_t wanted = TB_UI_FIRST_SLOTS;

    while (wanted * 3 / 4 <= ui->state_count + 1)
    {
        wanted *= 2;
    }
    if (wanted != ui->slot_count)
    {
        tb_free(ui->slots);
        ui->slots = tb_calloc(wanted, sizeof(*ui->slots));
        ui->slot_count = wanted;
    }
    memset(ui->slots, 0, ui->slot_count * sizeof(*ui->slots));
    for (size_t index = 0; index < ui->state_count; index++)
    {
        tb_ui_index(ui, index);
    }
}

size_t tb_ui_touch(tb_ui *ui, tb_ui_id id, bool *created)
{
    const size_t found = tb_ui_find(ui, id);

    if (found < ui->state_count && ui->states[found].seen == ui->frame)
    {
        ui->collisions++;
    }
    return tb_ui_state_index(ui, id, created);
}

size_t tb_ui_state_index(tb_ui *ui, tb_ui_id id, bool *created)
{
    size_t index = tb_ui_find(ui, id);
    bool made = false;

    if (index == ui->state_count)
    {
        if (ui->state_count == ui->state_capacity)
        {
            ui->state_capacity = ui->state_capacity == 0 ? 64 : ui->state_capacity * 2;
            ui->states = tb_realloc_array(ui->states, ui->state_capacity, sizeof(*ui->states));
        }
        ui->states[index] = (tb_ui_state){.id = id};
        ui->state_count++;
        if ((ui->state_count + 1) * 4 > ui->slot_count * 3)
        {
            tb_ui_reindex(ui);
        }
        else
        {
            tb_ui_index(ui, index);
        }
        made = true;
    }
    ui->states[index].seen = ui->frame;
    if (created != NULL)
    {
        *created = made;
    }
    return index;
}

tb_ui_state *tb_ui_state_at(tb_ui *ui, size_t index)
{
    TB_ASSERT(index < ui->state_count);
    return &ui->states[index];
}

/* Forget the state of widgets not seen for long. */
static void tb_ui_forget(tb_ui *ui)
{
    size_t kept = 0;

    for (size_t index = 0; index < ui->state_count; index++)
    {
        if (ui->frame - ui->states[index].seen <= ui->forget_frames)
        {
            ui->states[kept] = ui->states[index];
            kept++;
        }
    }
    if (kept != ui->state_count)
    {
        ui->state_count = kept;
        tb_ui_reindex(ui);
    }
}

/* ---------------------------------------------------------------------------
 * Frames.
 * ------------------------------------------------------------------------- */

static void tb_ui_measure_fonts(tb_ui *ui)
{
    for (size_t index = 0; index < TB_UI_FONT_COUNT; index++)
    {
        const tb_font *const font = ui->theme.fonts[index] != NULL
                                        ? ui->theme.fonts[index]
                                        : ui->theme.fonts[TB_UI_FONT_REGULAR];
        tb_ui_font_measures_of(font, ui->theme.font_size, ui->scale, &ui->fonts[index]);
    }
    ui->frame_height = ui->fonts[TB_UI_FONT_REGULAR].line_height + 2.0f * ui->theme.frame_padding.y;
}

static bool tb_ui_seen_last_frame(const tb_ui *ui, const tb_ui_state *state)
{
    return state->seen + 1 >= ui->frame;
}

/* The window, or the open list, under the mouse: the frontmost of those
 * drawn last frame. */
static tb_ui_id tb_ui_find_hovered(const tb_ui *ui)
{
    tb_ui_id found = 0;
    uint64_t front = 0;

    if (ui->input.mouse_outside)
    {
        return 0;
    }
    if (ui->popup_owner != 0 && tb_ui_rect_contains(ui->popup_rect, ui->mouse))
    {
        return tb_ui_derived_id(ui->popup_owner, "list");
    }
    for (size_t index = 0; index < ui->state_count; index++)
    {
        const tb_ui_state *const state = &ui->states[index];
        if (state->window && tb_ui_seen_last_frame(ui, state) &&
            tb_ui_rect_contains(state->hit, ui->mouse) && (found == 0 || state->z > front))
        {
            found = state->id;
            front = state->z;
        }
    }
    return found;
}

/* Give each window a layer in the order of its z: the back one the lowest. */
static void tb_ui_rank_windows(tb_ui *ui)
{
    int32_t rank = 0;
    uint64_t done = 0;

    /* Windows are few: find the next in order, each time. */
    for (;;)
    {
        size_t next = ui->state_count;
        for (size_t index = 0; index < ui->state_count; index++)
        {
            const tb_ui_state *const state = &ui->states[index];
            if (state->window && state->z > done &&
                (next == ui->state_count || state->z < ui->states[next].z))
            {
                next = index;
            }
        }
        if (next == ui->state_count)
        {
            break;
        }
        ui->states[next].layer = ui->layer + rank;
        ui->front_window = ui->states[next].id;
        done = ui->states[next].z;
        rank++;
    }
    ui->next_rank = rank;
}

void tb_ui_begin_frame(tb_ui *ui, const tb_ui_input *input)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(input != NULL);
    TB_ASSERT_MSG(!ui->in_frame, "a frame begun twice");
    TB_ASSERT_MSG(ui->theme.fonts[TB_UI_FONT_REGULAR] != NULL, "a theme without a regular font");

    ui->in_frame = true;
    ui->frame++;
    ui->input = *input;
    ui->scale = input->scale > 0.0f ? input->scale : 1.0f;
    ui->time += input->delta_seconds;
    ui->last_mouse = ui->mouse;
    ui->mouse = (tb_vec2){input->mouse.x / ui->scale, input->mouse.y / ui->scale};
    if (ui->frame == 1)
    {
        ui->last_mouse = ui->mouse;
    }
    ui->size = (tb_vec2){input->size.x / ui->scale, input->size.y / ui->scale};
    tb_arena_reset(ui->arena);
    tb_ui_paint_reset(&ui->paint, ui->scale);
    ui->focusable_count = 0;
    ui->collisions = 0;
    ui->windows = 0;
    ui->id_count = 0;
    ui->container_count = 0;
    memset(ui->key_used, 0, sizeof(ui->key_used));
    ui->hot = 0;
    ui->active_seen = false;
    ui->press_taken = false;
    ui->pressed = 0;
    ui->wheel_taken = false;
    ui->focus_seen = false;
    ui->focus_text = false;
    ui->popup_seen = false;
    ui->popup_next = (tb_canvas_rect){0};
    ui->last_id = 0;
    ui->last_hot = false;
    ui->cursor = TB_CURSOR_ARROW;
    tb_ui_measure_fonts(ui);

    ui->hovered_window = tb_ui_find_hovered(ui);
    if ((input->pressed & TB_UI_LEFT) != 0 && ui->hovered_window != 0)
    {
        const size_t index = tb_ui_find(ui, ui->hovered_window);
        if (index < ui->state_count)
        {
            ui->z_counter++;
            ui->states[index].z = ui->z_counter;
        }
    }
    tb_ui_rank_windows(ui);
}

/* The z of the window a widget is in, to find the front one. */
static uint64_t tb_ui_z_of(const tb_ui *ui, tb_ui_id root)
{
    const size_t index = tb_ui_find(ui, root);
    return index < ui->state_count ? ui->states[index].z : 0;
}

/* Move the focus to the next widget of its window, or the one before;
 * without a focus, to the first or last of the front window. */
static void tb_ui_navigate(tb_ui *ui, bool backward)
{
    tb_ui_id root = 0;
    size_t at = ui->focusable_count;
    size_t found = ui->focusable_count;

    if (ui->focusable_count == 0)
    {
        return;
    }
    for (size_t index = 0; index < ui->focusable_count; index++)
    {
        if (ui->focusables[index].id == ui->focus)
        {
            at = index;
            root = ui->focusables[index].root;
        }
    }
    if (at == ui->focusable_count)
    {
        for (size_t index = 0; index < ui->focusable_count; index++)
        {
            const tb_ui_id candidate = ui->focusables[index].root;
            if (root == 0 || tb_ui_z_of(ui, candidate) > tb_ui_z_of(ui, root))
            {
                root = candidate;
            }
        }
    }
    for (size_t step = 1; step <= ui->focusable_count && found == ui->focusable_count; step++)
    {
        const size_t start =
            at == ui->focusable_count ? (backward ? 0 : ui->focusable_count - 1) : at;
        const size_t index = backward ? (start + ui->focusable_count - step) % ui->focusable_count
                                      : (start + step) % ui->focusable_count;
        if (ui->focusables[index].root == root)
        {
            found = index;
        }
    }
    ui->focus = ui->focusables[found].id;
    ui->focus_by_keys = true;
    ui->edit.id = 0;
}

static void tb_ui_close_popup(tb_ui *ui)
{
    ui->popup_owner = 0;
    ui->popup_rect = (tb_canvas_rect){0};
}

/* The keys no widget took: Tab and Escape. */
static void tb_ui_handle_keys(tb_ui *ui)
{
    for (size_t index = 0; index < ui->input.key_count; index++)
    {
        const tb_ui_key_press *const press = &ui->input.keys[index];
        if (ui->key_used[index])
        {
            continue;
        }
        if (press->key == TB_KEY_TAB)
        {
            tb_ui_navigate(ui, (press->modifiers & TB_MODIFIER_SHIFT) != 0);
        }
        else if (press->key == TB_KEY_ESCAPE && ui->popup_owner != 0)
        {
            tb_ui_close_popup(ui);
        }
        else if (press->key == TB_KEY_ESCAPE)
        {
            ui->focus = 0;
            ui->focus_by_keys = false;
        }
    }
}

static void tb_ui_end_mouse(tb_ui *ui)
{
    const bool pressed = (ui->input.pressed & TB_UI_LEFT) != 0;
    const tb_ui_id list = ui->popup_owner != 0 ? tb_ui_derived_id(ui->popup_owner, "list") : 0;

    if (ui->active != 0 && (!ui->active_seen || (ui->input.buttons & TB_UI_LEFT) == 0))
    {
        ui->active = 0;
    }
    if (pressed && !ui->press_taken)
    {
        ui->focus = 0;
        ui->focus_by_keys = false;
    }
    if (pressed && ui->popup_owner != 0 && ui->hovered_window != list &&
        ui->pressed != ui->popup_owner)
    {
        tb_ui_close_popup(ui);
    }
    if (ui->popup_owner != 0 && !ui->popup_seen)
    {
        tb_ui_close_popup(ui);
    }
    if (ui->popup_owner != 0)
    {
        ui->popup_rect = ui->popup_next;
    }
}

void tb_ui_end_frame(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT_MSG(ui->in_frame, "a frame ended that was not begun");
    TB_ASSERT_MSG(ui->container_count == 0, "a window or panel not ended");
    TB_ASSERT_MSG(ui->id_count == 0, "an id pushed and not popped");

    tb_ui_end_mouse(ui);
    if (ui->focus != 0 && !ui->focus_seen)
    {
        ui->focus = 0;
        ui->focus_by_keys = false;
    }
    tb_ui_handle_keys(ui);
    if (ui->edit.id != ui->focus)
    {
        ui->edit.id = 0;
    }
    if (ui->hot != 0 && ui->hot == ui->hover_id)
    {
        ui->hover_seconds += ui->input.delta_seconds;
    }
    else
    {
        ui->hover_id = ui->hot;
        ui->hover_seconds = 0.0;
    }
    ui->wants_mouse = ui->hovered_window != 0 || ui->active != 0;
    ui->wants_text = ui->focus != 0 && ui->focus_text;
    ui->wants_keyboard = ui->focus != 0 && (ui->focus_by_keys || ui->focus_text);
    tb_ui_forget(ui);
    ui->in_frame = false;
}

/* ---------------------------------------------------------------------------
 * The mouse and the keyboard.
 * ------------------------------------------------------------------------- */

bool tb_ui_mouse_over(const tb_ui *ui, tb_canvas_rect rect)
{
    const tb_ui_container *const container =
        ui->container_count > 0 ? &ui->containers[ui->container_count - 1] : NULL;

    return container != NULL && !ui->input.mouse_outside && ui->hovered_window == container->root &&
           tb_ui_rect_contains(container->clip, ui->mouse) && tb_ui_rect_contains(rect, ui->mouse);
}

static void tb_ui_add_focusable(tb_ui *ui, tb_ui_id id, tb_ui_id root)
{
    if (ui->focusable_count == ui->focusable_capacity)
    {
        ui->focusable_capacity = ui->focusable_capacity == 0 ? 64 : ui->focusable_capacity * 2;
        ui->focusables =
            tb_realloc_array(ui->focusables, ui->focusable_capacity, sizeof(*ui->focusables));
    }
    ui->focusables[ui->focusable_count] = (tb_ui_focusable){.id = id, .root = root};
    ui->focusable_count++;
}

tb_ui_interaction tb_ui_interact(tb_ui *ui, tb_ui_id id, tb_canvas_rect rect, bool focusable)
{
    tb_ui_interaction result = {0};
    const tb_ui_container *const container = tb_ui_top(ui);
    const bool over = tb_ui_mouse_over(ui, rect);

    (void)tb_ui_touch(ui, id, NULL);
    if (focusable)
    {
        tb_ui_add_focusable(ui, id, container->root);
    }
    if (over && (ui->active == 0 || ui->active == id))
    {
        ui->hot = id;
        result.hot = true;
    }
    if (over && ui->active == 0 && (ui->input.pressed & TB_UI_LEFT) != 0 && !ui->press_taken)
    {
        ui->active = id;
        ui->pressed = id;
        ui->press_taken = true;
        ui->press_mouse = ui->mouse;
        ui->focus = focusable ? id : 0;
        ui->focus_by_keys = false;
        result.pressed = true;
    }
    if (ui->active == id)
    {
        ui->active_seen = true;
        result.active = true;
        if ((ui->input.released & TB_UI_LEFT) != 0)
        {
            ui->active = 0;
            result.clicked = over;
        }
    }
    result.focused = ui->focus == id;
    if (result.focused)
    {
        ui->focus_seen = true;
    }
    result.keys = result.focused && ui->focus_by_keys;
    return result;
}

bool tb_ui_key_pressed(tb_ui *ui, tb_key key)
{
    for (size_t index = 0; index < ui->input.key_count; index++)
    {
        if (!ui->key_used[index] && ui->input.keys[index].key == key)
        {
            ui->key_used[index] = true;
            return true;
        }
    }
    return false;
}

void tb_ui_set_last(tb_ui *ui, tb_ui_id id, tb_canvas_rect rect, bool hot)
{
    ui->last_id = id;
    ui->last_rect = rect;
    ui->last_hot = hot;
}

/* ---------------------------------------------------------------------------
 * Queries.
 * ------------------------------------------------------------------------- */

const tb_ui_command *tb_ui_commands(const tb_ui *ui, size_t *count)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(count != NULL);
    *count = ui->paint.count;
    return ui->paint.commands;
}

bool tb_ui_wants_mouse(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return ui->wants_mouse;
}

bool tb_ui_wants_keyboard(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return ui->wants_keyboard;
}

bool tb_ui_wants_text(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return ui->wants_text;
}

tb_cursor tb_ui_cursor(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return ui->cursor;
}

void tb_ui_stats_of(const tb_ui *ui, tb_ui_stats *stats)
{
    TB_ASSERT(ui != NULL);
    TB_ASSERT(stats != NULL);
    *stats = (tb_ui_stats){.commands = ui->paint.count,
                           .windows = ui->windows,
                           .states = ui->state_count,
                           .id_collisions = ui->collisions,
                           .hot = ui->hot,
                           .active = ui->active,
                           .focus = ui->focus,
                           .hovered_window = ui->hovered_window};
}

tb_text tb_ui_clipboard(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return tb_text_make(ui->clipboard, ui->clipboard_length);
}

void tb_ui_set_clipboard(tb_ui *ui, tb_text text)
{
    size_t length = text.length < TB_UI_CLIPBOARD_CAPACITY ? text.length : 0;

    TB_ASSERT(ui != NULL);
    if (length > 0)
    {
        memcpy(ui->clipboard, text.bytes, length);
    }
    ui->clipboard_length = length;
}

bool tb_ui_item_hovered(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return ui->last_hot && ui->active == 0;
}

tb_ui_id tb_ui_last_id(const tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    return ui->last_id;
}

void tb_ui_focus_last(tb_ui *ui)
{
    TB_ASSERT(ui != NULL);
    if (ui->last_id != 0)
    {
        ui->focus = ui->last_id;
        ui->focus_by_keys = false;
        ui->focus_seen = true;
    }
}
