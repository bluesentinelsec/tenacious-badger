/* The text field: one line of the caller's text, edited in place, with a
 * caret and a selection kept by the interface for the field being edited. */
#include "ui_internal.h"

#include "badger/assert.h"
#include "badger/font_layout.h"
#include "badger/utf8.h"

#include <math.h>
#include <string.h>

/* Seconds the caret shows, then hides, once it has stopped moving. */
#define TB_UI_CARET_BLINK 0.53

/* A field's text and its caret, as it is edited. */
typedef struct tb_ui_editing
{
    tb_ui *ui;
    char *buffer;
    size_t capacity;
    size_t length;
    tb_ui_edit *edit;
    bool changed;
} tb_ui_editing;

static bool tb_ui_shortcut(uint32_t modifiers)
{
    return (modifiers & (TB_MODIFIER_CONTROL | TB_MODIFIER_SUPER)) != 0;
}

/* The boundary before the character that ends at `offset`. */
static size_t tb_ui_before(const tb_ui_editing *editing, size_t offset)
{
    uint32_t scalar = 0;
    size_t previous = 0;

    if (offset == 0)
    {
        return 0;
    }
    if (tb_utf8_previous(editing->buffer, editing->length, offset, &scalar, &previous))
    {
        return previous;
    }
    return offset - 1; /* a byte of no character is a character of its own */
}

/* The boundary after the character that begins at `offset`. */
static size_t tb_ui_after(const tb_ui_editing *editing, size_t offset)
{
    uint32_t scalar = 0;
    size_t next = 0;

    if (offset >= editing->length)
    {
        return editing->length;
    }
    if (tb_utf8_decode(editing->buffer, editing->length, offset, &scalar, &next))
    {
        return next;
    }
    return offset + 1;
}

/* Words are what lies between spaces and ASCII punctuation. */
static bool tb_ui_word_byte(char byte)
{
    const unsigned char value = (unsigned char)byte;

    if (value >= 0x80)
    {
        return true;
    }
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

static size_t tb_ui_word_before(const tb_ui_editing *editing, size_t offset)
{
    while (offset > 0 && !tb_ui_word_byte(editing->buffer[offset - 1]))
    {
        offset = tb_ui_before(editing, offset);
    }
    while (offset > 0 && tb_ui_word_byte(editing->buffer[offset - 1]))
    {
        offset = tb_ui_before(editing, offset);
    }
    return offset;
}

static size_t tb_ui_word_after(const tb_ui_editing *editing, size_t offset)
{
    while (offset < editing->length && tb_ui_word_byte(editing->buffer[offset]))
    {
        offset = tb_ui_after(editing, offset);
    }
    while (offset < editing->length && !tb_ui_word_byte(editing->buffer[offset]))
    {
        offset = tb_ui_after(editing, offset);
    }
    return offset;
}

static size_t tb_ui_selection_start(const tb_ui_edit *edit)
{
    return edit->caret < edit->anchor ? edit->caret : edit->anchor;
}

static size_t tb_ui_selection_end(const tb_ui_edit *edit)
{
    return edit->caret < edit->anchor ? edit->anchor : edit->caret;
}

static void tb_ui_remove(tb_ui_editing *editing, size_t start, size_t end)
{
    if (end <= start)
    {
        return;
    }
    memmove(editing->buffer + start, editing->buffer + end, editing->length - end);
    editing->length -= end - start;
    editing->buffer[editing->length] = '\0';
    editing->edit->caret = start;
    editing->edit->anchor = start;
    editing->changed = true;
}

static bool tb_ui_delete_selection(tb_ui_editing *editing)
{
    const size_t start = tb_ui_selection_start(editing->edit);
    const size_t end = tb_ui_selection_end(editing->edit);

    tb_ui_remove(editing, start, end);
    return end > start;
}

/* Insert text at the caret, in place of the selection: as many whole
 * characters as fit, without control characters, which one line has none of. */
static void tb_ui_insert(tb_ui_editing *editing, tb_text text)
{
    size_t room = 0;
    size_t written = 0;

    (void)tb_ui_delete_selection(editing);
    room = editing->capacity - 1 - editing->length;
    for (size_t offset = 0; offset < text.length;)
    {
        uint32_t scalar = 0;
        size_t next = offset + 1;
        if (!tb_utf8_decode(text.bytes, text.length, offset, &scalar, &next))
        {
            scalar = 0xFFFD; /* a byte that begins nothing is dropped below */
            next = offset + 1;
        }
        const size_t size = next - offset;
        if (scalar < 0x20 || scalar == 0x7F || scalar == 0xFFFD)
        {
            offset = next;
            continue;
        }
        if (written + size > room)
        {
            break;
        }
        /* Make room at the caret, then copy the character in. */
        const size_t at = editing->edit->caret + written;
        memmove(editing->buffer + at + size, editing->buffer + at, editing->length - at);
        memcpy(editing->buffer + at, text.bytes + offset, size);
        editing->length += size;
        written += size;
        offset = next;
    }
    editing->buffer[editing->length] = '\0';
    editing->edit->caret += written;
    editing->edit->anchor = editing->edit->caret;
    editing->changed = editing->changed || written > 0;
}

/* Move the caret, and with Shift the selection's end with it. */
static void tb_ui_move(tb_ui_editing *editing, size_t offset, uint32_t modifiers)
{
    editing->edit->caret = offset;
    if ((modifiers & TB_MODIFIER_SHIFT) == 0)
    {
        editing->edit->anchor = offset;
    }
}

/* Left or Right: by a character or a word; without Shift, a selection
 * shuts to its edge on that side. */
static void tb_ui_arrow(tb_ui_editing *editing, bool right, uint32_t modifiers)
{
    tb_ui_edit *const edit = editing->edit;
    const bool selecting = (modifiers & TB_MODIFIER_SHIFT) != 0;
    size_t offset = edit->caret;

    if (!selecting && edit->caret != edit->anchor && !tb_ui_shortcut(modifiers))
    {
        offset = right ? tb_ui_selection_end(edit) : tb_ui_selection_start(edit);
    }
    else if (tb_ui_shortcut(modifiers))
    {
        offset = right ? tb_ui_word_after(editing, offset) : tb_ui_word_before(editing, offset);
    }
    else
    {
        offset = right ? tb_ui_after(editing, offset) : tb_ui_before(editing, offset);
    }
    tb_ui_move(editing, offset, modifiers);
}

static void tb_ui_erase(tb_ui_editing *editing, bool forward, uint32_t modifiers)
{
    const size_t caret = editing->edit->caret;

    if (tb_ui_delete_selection(editing))
    {
        return;
    }
    if (forward)
    {
        tb_ui_remove(editing, caret,
                     tb_ui_shortcut(modifiers) ? tb_ui_word_after(editing, caret)
                                               : tb_ui_after(editing, caret));
        return;
    }
    tb_ui_remove(editing,
                 tb_ui_shortcut(modifiers) ? tb_ui_word_before(editing, caret)
                                           : tb_ui_before(editing, caret),
                 caret);
}

static void tb_ui_copy(tb_ui_editing *editing)
{
    const size_t start = tb_ui_selection_start(editing->edit);
    const size_t end = tb_ui_selection_end(editing->edit);

    if (end > start)
    {
        tb_ui_set_clipboard(editing->ui, tb_text_make(editing->buffer + start, end - start));
    }
}

/* Ctrl or Command with A, C, X, or V: true when it is one of them. */
static bool tb_ui_edit_shortcut(tb_ui_editing *editing, tb_key key)
{
    if (key == TB_KEY_A)
    {
        editing->edit->anchor = 0;
        editing->edit->caret = editing->length;
        return true;
    }
    if (key == TB_KEY_C || key == TB_KEY_X)
    {
        tb_ui_copy(editing);
        if (key == TB_KEY_X)
        {
            (void)tb_ui_delete_selection(editing);
        }
        return true;
    }
    if (key == TB_KEY_V)
    {
        tb_ui_insert(editing, tb_ui_clipboard(editing->ui));
        return true;
    }
    return false;
}

/* A key pressed in the field: true when the field takes it. */
static bool tb_ui_edit_key(tb_ui_editing *editing, const tb_ui_key_press *press)
{
    const tb_key key = press->key;

    if (key == TB_KEY_LEFT || key == TB_KEY_RIGHT)
    {
        tb_ui_arrow(editing, key == TB_KEY_RIGHT, press->modifiers);
        return true;
    }
    if (key == TB_KEY_HOME || key == TB_KEY_END)
    {
        tb_ui_move(editing, key == TB_KEY_HOME ? 0 : editing->length, press->modifiers);
        return true;
    }
    if (key == TB_KEY_BACKSPACE || key == TB_KEY_DELETE)
    {
        tb_ui_erase(editing, key == TB_KEY_DELETE, press->modifiers);
        return true;
    }
    if (key == TB_KEY_ENTER || key == TB_KEY_ESCAPE)
    {
        editing->ui->focus = 0;
        editing->ui->focus_by_keys = false;
        return true;
    }
    return tb_ui_shortcut(press->modifiers) && tb_ui_edit_shortcut(editing, key);
}

/* The frame's keys and text, in the order they came. */
static void tb_ui_edit_input(tb_ui_editing *editing)
{
    tb_ui *const ui = editing->ui;
    const tb_ui_input *const input = &ui->input;
    size_t typed = 0;

    for (size_t index = 0; index < input->key_count && ui->focus == editing->edit->id; index++)
    {
        const tb_ui_key_press *const press = &input->keys[index];
        const size_t before =
            press->text_offset < input->text_length ? press->text_offset : input->text_length;
        if (before > typed)
        {
            tb_ui_insert(editing, tb_text_make(input->text + typed, before - typed));
            typed = before;
        }
        if (!ui->key_used[index] && tb_ui_edit_key(editing, press))
        {
            ui->key_used[index] = true;
            ui->edit.blink = ui->time;
        }
    }
    if (ui->focus == editing->edit->id && input->text_length > typed)
    {
        tb_ui_insert(editing, tb_text_make(input->text + typed, input->text_length - typed));
        ui->edit.blink = ui->time;
    }
}

static void tb_ui_layout_field(tb_ui *ui, tb_text text, tb_font_layout *layout)
{
    tb_font_layout_text(ui->fonts[TB_UI_FONT_REGULAR].font, text, ui->theme.font_size * ui->scale,
                        NULL, ui->arena, layout);
}

/* The caret under the mouse, from the text's left edge, in units. */
static size_t tb_ui_hit(const tb_ui *ui, const tb_font_layout *layout, float x)
{
    tb_font_caret caret = {0};

    tb_font_layout_hit(layout, x * ui->scale, 0.0f, &caret);
    return caret.offset;
}

static float tb_ui_caret_x(const tb_ui *ui, const tb_font_layout *layout, size_t offset)
{
    tb_font_caret caret = {0};

    tb_font_layout_caret(layout, offset, &caret);
    return caret.x / ui->scale;
}

/* Scroll the text so that the caret is seen. */
static void tb_ui_follow_caret(tb_ui *ui, const tb_font_layout *layout, float width)
{
    tb_ui_edit *const edit = &ui->edit;
    const float caret = tb_ui_caret_x(ui, layout, edit->caret);
    const float text = layout->width / ui->scale;

    if (caret - edit->scroll > width - 1.0f)
    {
        edit->scroll = caret - width + 1.0f;
    }
    if (caret < edit->scroll)
    {
        edit->scroll = caret;
    }
    edit->scroll = fmaxf(fminf(edit->scroll, text - width + 1.0f), 0.0f);
}

static void tb_ui_draw_field(tb_ui *ui, tb_ui_id id, tb_canvas_rect field, tb_text text,
                             const tb_font_layout *layout, const tb_ui_interaction *interaction)
{
    const bool editing = ui->edit.id == id && ui->focus == id;
    const tb_canvas_rect inner = tb_ui_rect_inset(field, ui->theme.frame_padding.x, 0.0f);
    const tb_canvas_rect clip = tb_ui_rect_intersect(tb_ui_clip(ui), inner);
    const float scroll = editing ? ui->edit.scroll : 0.0f;
    const float left = inner.x - scroll;
    const float top = tb_ui_text_top(ui, TB_UI_FONT_REGULAR, field);
    const float bar_top = field.y + 3.0f;
    const float bar_height = field.height - 6.0f;
    const float radius = ui->theme.frame_rounding;

    tb_ui_fill(ui, field, radius,
               tb_ui_color_of(ui, interaction->hot && !editing ? TB_UI_COLOR_FRAME_HOT
                                                               : TB_UI_COLOR_FRAME),
               id);
    tb_ui_outline(ui, field, radius, ui->theme.border,
                  tb_ui_color_of(ui, editing ? TB_UI_COLOR_ACCENT
                                             : (interaction->hot ? TB_UI_COLOR_BUTTON_HOT
                                                                 : TB_UI_COLOR_BUTTON)),
                  id);
    if (editing && ui->edit.caret != ui->edit.anchor)
    {
        const float start = tb_ui_caret_x(ui, layout, tb_ui_selection_start(&ui->edit));
        const float end = tb_ui_caret_x(ui, layout, tb_ui_selection_end(&ui->edit));
        tb_ui_fill(ui,
                   tb_ui_rect_intersect(
                       (tb_canvas_rect){left + start, bar_top, end - start, bar_height}, clip),
                   0.0f, tb_ui_color_of(ui, TB_UI_COLOR_SELECTION), id);
    }
    tb_ui_draw_text(ui, TB_UI_FONT_REGULAR, text, (tb_vec2){left, top},
                    tb_ui_color_of(ui, TB_UI_COLOR_TEXT), 0.0f, clip, id);
    if (editing && fmod(ui->time - ui->edit.blink, 2.0 * TB_UI_CARET_BLINK) < TB_UI_CARET_BLINK)
    {
        const float x = left + tb_ui_caret_x(ui, layout, ui->edit.caret);
        tb_ui_fill(ui, (tb_canvas_rect){x, bar_top, 1.0f, bar_height}, 0.0f,
                   tb_ui_color_of(ui, TB_UI_COLOR_TEXT), id);
    }
}

bool tb_ui_edit_text(tb_ui *ui, tb_ui_id id, tb_canvas_rect field, char *buffer, size_t capacity,
                     const tb_ui_interaction *interaction)
{
    tb_ui_editing editing = {.ui = ui, .buffer = buffer, .capacity = capacity, .edit = &ui->edit};
    const tb_canvas_rect inner = tb_ui_rect_inset(field, ui->theme.frame_padding.x, 0.0f);
    tb_font_layout layout = {0};

    TB_ASSERT_MSG(capacity > 0, "a text field without room for its terminator");
    const char *const end = memchr(buffer, '\0', capacity);
    editing.length = end != NULL ? (size_t)(end - buffer) : capacity;
    if (editing.length == capacity)
    {
        editing.length = capacity - 1;
        buffer[editing.length] = '\0';
    }
    if (interaction->hot || interaction->active)
    {
        ui->cursor = TB_CURSOR_TEXT;
    }
    tb_ui_layout_field(ui, tb_text_make(buffer, editing.length), &layout);
    if (interaction->focused)
    {
        ui->focus_text = true;
        if (ui->edit.id != id)
        {
            /* Tab selects the whole text; a click places the caret below. */
            ui->edit = (tb_ui_edit){.id = id,
                                    .caret = editing.length,
                                    .anchor = ui->focus_by_keys ? 0 : editing.length,
                                    .blink = ui->time};
        }
        ui->edit.caret = ui->edit.caret < editing.length ? ui->edit.caret : editing.length;
        ui->edit.anchor = ui->edit.anchor < editing.length ? ui->edit.anchor : editing.length;
    }
    if (interaction->active && interaction->focused)
    {
        const size_t hit = tb_ui_hit(ui, &layout, ui->mouse.x - inner.x + ui->edit.scroll);
        const bool extend = !interaction->pressed || (ui->input.modifiers & TB_MODIFIER_SHIFT) != 0;
        tb_ui_move(&editing, hit, extend ? TB_MODIFIER_SHIFT : 0);
        ui->edit.blink = ui->time;
    }
    if (interaction->focused)
    {
        tb_ui_edit_input(&editing);
    }
    if (editing.changed)
    {
        tb_ui_layout_field(ui, tb_text_make(buffer, editing.length), &layout);
    }
    if (ui->edit.id == id && ui->focus == id)
    {
        tb_ui_follow_caret(ui, &layout, inner.width);
    }
    tb_ui_draw_field(ui, id, field, tb_text_make(buffer, editing.length), &layout, interaction);
    return editing.changed;
}

bool tb_ui_text_field(tb_ui *ui, const char *label, char *buffer, size_t capacity)
{
    tb_ui_id id = 0;
    tb_canvas_rect field = {0};
    tb_ui_interaction interaction = {0};
    bool changed = false;

    TB_ASSERT(ui != NULL);
    TB_ASSERT(label != NULL);
    TB_ASSERT(buffer != NULL);
    id = tb_ui_get_id(ui, label);
    tb_ui_split_label(ui, tb_ui_next_item(ui, 0.0f, ui->frame_height, true), label, &field);
    interaction = tb_ui_interact(ui, id, field, true);
    changed = tb_ui_edit_text(ui, id, field, buffer, capacity, &interaction);
    tb_ui_set_last(ui, id, field, interaction.hot);
    return changed;
}
