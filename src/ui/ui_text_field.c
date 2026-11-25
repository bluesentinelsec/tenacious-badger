/* The text field: one line of the caller's text, edited in place, with a
 * caret and a selection kept by the interface for the field being edited.
 * The edits themselves are those every field shares (ui_editor.h). */
#include "ui_internal.h"

#include "ui_editor.h"

#include "badger/assert.h"
#include "badger/font_layout.h"

#include <math.h>
#include <string.h>

/* Seconds the caret shows, then hides, once it has stopped moving. */
#define TB_UI_CARET_BLINK 0.53

/* A field's text and its caret, as it is edited. */
typedef struct tb_ui_editing
{
    tb_ui *ui;
    tb_ui_editor editor;
} tb_ui_editing;

static void tb_ui_copy(tb_ui_editing *editing)
{
    const tb_text selection = tb_ui_editor_selection(&editing->editor);

    if (selection.length > 0)
    {
        tb_ui_set_clipboard(editing->ui, selection);
    }
}

/* Ctrl or Command with A, C, X, or V: true when it is one of them. */
static bool tb_ui_edit_shortcut(tb_ui_editing *editing, tb_key key)
{
    if (key == TB_KEY_A)
    {
        tb_ui_editor_select_all(&editing->editor);
        return true;
    }
    if (key == TB_KEY_C || key == TB_KEY_X)
    {
        tb_ui_copy(editing);
        if (key == TB_KEY_X)
        {
            (void)tb_ui_editor_delete_selection(&editing->editor);
        }
        return true;
    }
    if (key == TB_KEY_V)
    {
        (void)tb_ui_editor_insert(&editing->editor, tb_ui_clipboard(editing->ui));
        return true;
    }
    return false;
}

/* A key pressed in the field: true when the field takes it. */
static bool tb_ui_edit_key(tb_ui_editing *editing, const tb_ui_key_press *press)
{
    tb_ui_editor *const editor = &editing->editor;
    const tb_key key = press->key;

    if (key == TB_KEY_LEFT || key == TB_KEY_RIGHT)
    {
        tb_ui_editor_arrow(editor, key == TB_KEY_RIGHT, press->modifiers);
        return true;
    }
    if (key == TB_KEY_HOME || key == TB_KEY_END)
    {
        tb_ui_editor_move(editor, key == TB_KEY_HOME ? 0 : editor->length, press->modifiers);
        return true;
    }
    if (key == TB_KEY_BACKSPACE || key == TB_KEY_DELETE)
    {
        tb_ui_editor_erase(editor, key == TB_KEY_DELETE, press->modifiers);
        return true;
    }
    if (key == TB_KEY_ENTER || key == TB_KEY_ESCAPE)
    {
        editing->ui->focus = 0;
        editing->ui->focus_by_keys = false;
        return true;
    }
    return tb_ui_editor_shortcut(press->modifiers) && tb_ui_edit_shortcut(editing, key);
}

/* The frame's keys and text, in the order they came. */
static void tb_ui_edit_input(tb_ui_editing *editing, tb_ui_id id)
{
    tb_ui *const ui = editing->ui;
    const tb_ui_input *const input = &ui->input;
    size_t typed = 0;

    for (size_t index = 0; index < input->key_count && ui->focus == id; index++)
    {
        const tb_ui_key_press *const press = &input->keys[index];
        const size_t before =
            press->text_offset < input->text_length ? press->text_offset : input->text_length;
        if (before > typed)
        {
            (void)tb_ui_editor_insert(&editing->editor,
                                      tb_text_make(input->text + typed, before - typed));
            typed = before;
        }
        if (!ui->key_used[index] && tb_ui_edit_key(editing, press))
        {
            ui->key_used[index] = true;
            ui->edit.blink = ui->time;
        }
    }
    if (ui->focus == id && input->text_length > typed)
    {
        (void)tb_ui_editor_insert(&editing->editor,
                                  tb_text_make(input->text + typed, input->text_length - typed));
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
        const size_t first = ui->edit.caret < ui->edit.anchor ? ui->edit.caret : ui->edit.anchor;
        const size_t last = ui->edit.caret < ui->edit.anchor ? ui->edit.anchor : ui->edit.caret;
        const float start = tb_ui_caret_x(ui, layout, first);
        const float end = tb_ui_caret_x(ui, layout, last);
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
    tb_ui_editing editing = {.ui = ui,
                             .editor = {.buffer = buffer,
                                        .capacity = capacity,
                                        .caret = ui->edit.caret,
                                        .anchor = ui->edit.anchor}};
    tb_ui_editor *const editor = &editing.editor;
    const tb_canvas_rect inner = tb_ui_rect_inset(field, ui->theme.frame_padding.x, 0.0f);
    tb_font_layout layout = {0};

    TB_ASSERT_MSG(capacity > 0, "a text field without room for its terminator");
    tb_ui_editor_refresh(editor);
    if (interaction->hot || interaction->active)
    {
        ui->cursor = TB_CURSOR_TEXT;
    }
    tb_ui_layout_field(ui, tb_text_make(buffer, editor->length), &layout);
    if (interaction->focused)
    {
        ui->focus_text = true;
        if (ui->edit.id != id)
        {
            /* Tab selects the whole text; a click places the caret below. */
            ui->edit = (tb_ui_edit){.id = id,
                                    .caret = editor->length,
                                    .anchor = ui->focus_by_keys ? 0 : editor->length,
                                    .blink = ui->time};
            editor->caret = ui->edit.caret;
            editor->anchor = ui->edit.anchor;
        }
    }
    if (interaction->active && interaction->focused)
    {
        const size_t hit = tb_ui_hit(ui, &layout, ui->mouse.x - inner.x + ui->edit.scroll);
        const bool extend = !interaction->pressed || (ui->input.modifiers & TB_MODIFIER_SHIFT) != 0;
        tb_ui_editor_move(editor, hit, extend ? TB_MODIFIER_SHIFT : 0);
        ui->edit.blink = ui->time;
    }
    if (interaction->focused)
    {
        tb_ui_edit_input(&editing, id);
    }
    if (ui->edit.id == id)
    {
        ui->edit.caret = editor->caret;
        ui->edit.anchor = editor->anchor;
    }
    if (editor->changed)
    {
        tb_ui_layout_field(ui, tb_text_make(buffer, editor->length), &layout);
    }
    if (ui->edit.id == id && ui->focus == id)
    {
        tb_ui_follow_caret(ui, &layout, inner.width);
    }
    tb_ui_draw_field(ui, id, field, tb_text_make(buffer, editor->length), &layout, interaction);
    return editor->changed;
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
