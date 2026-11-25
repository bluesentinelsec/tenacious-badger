/* Text being edited in a widget: an entry's line, a text box's lines, a
 * spin box's number. The edits are those every field shares (ui_editor.h);
 * here they are given keys, typed text, and the mouse, validated, set in a
 * variable, and drawn with their selection and caret. */
#include "gui_values.h"

#include "badger/assert.h"
#include "badger/memory.h"

#include <math.h>
#include <string.h>

/* Seconds the caret shows, then hides, once it has stopped moving. */
#define TB_GUI_CARET_BLINK 0.53
/* Bytes a field first holds, its terminator's included. */
#define TB_GUI_FIRST_FIELD 32u
/* Lines a notch of the wheel scrolls. */
#define TB_GUI_WHEEL_LINES 3.0f
/* Units of room kept after the caret, so it is seen at the right edge. */
#define TB_GUI_CARET_ROOM 1.0f

void tb_gui_field_init(tb_gui_field *field, bool multiline)
{
    field->editor.buffer = tb_malloc(TB_GUI_FIRST_FIELD);
    field->editor.buffer[0] = '\0';
    field->editor.capacity = TB_GUI_FIRST_FIELD;
    field->editor.multiline = multiline;
    field->wraps = multiline;
}

void tb_gui_field_free(tb_gui_field *field)
{
    tb_free(field->editor.buffer);
    field->editor = (tb_ui_editor){0};
}

/* Room for `extra` bytes more, and the terminator. */
static void tb_gui_field_reserve(tb_gui_field *field, size_t extra)
{
    tb_ui_editor *const editor = &field->editor;
    size_t capacity = editor->capacity;

    TB_ASSERT_MSG(extra < SIZE_MAX - editor->length - 1, "a text too large to edit");
    while (capacity < editor->length + extra + 1)
    {
        TB_ASSERT_MSG(capacity < SIZE_MAX / 2, "a text too large to edit");
        capacity *= 2;
    }
    if (capacity != editor->capacity)
    {
        editor->buffer = tb_realloc(editor->buffer, capacity);
        editor->capacity = capacity;
    }
}

/* Put text in the buffer, keeping the caret and the selection within it. */
static void tb_gui_field_replace(tb_gui_field *field, tb_text text)
{
    tb_ui_editor *const editor = &field->editor;

    tb_gui_field_reserve(field, text.length > editor->length ? text.length - editor->length : 0);
    if (text.length > 0)
    {
        memmove(editor->buffer, text.bytes, text.length);
    }
    editor->buffer[text.length] = '\0';
    editor->length = text.length;
    tb_ui_editor_refresh(editor);
}

tb_text tb_gui_field_text(const tb_gui_field *field)
{
    return tb_text_make(field->editor.buffer, field->editor.length);
}

void tb_gui_field_sync(tb_gui_field *field)
{
    const uint64_t version = tb_var_version(field->variable);

    if (version == 0 || version == field->synced)
    {
        return;
    }
    tb_gui_field_replace(field, tb_var_get_text(field->variable));
    field->synced = version;
}

void tb_gui_field_set_text(tb_gui_field *field, tb_text text)
{
    tb_gui_field_replace(field, text);
    field->dirty = false;
    if (tb_var_alive(field->variable))
    {
        (void)tb_gui_var_set_span(field->variable, tb_gui_field_text(field));
        field->synced = tb_var_version(field->variable);
    }
}

bool tb_gui_field_bind(tb_widget widget, tb_gui_field *field, tb_var var)
{
    if (var.gui != NULL &&
        (!tb_gui_var_of_widget(widget, var) || tb_var_type(var) != TB_GUI_TYPE_TEXT))
    {
        return false;
    }
    field->variable = var;
    field->synced = 0;
    tb_gui_field_sync(field);
    return true;
}

/* ---------------------------------------------------------------------------
 * The text laid out.
 * ------------------------------------------------------------------------- */

/* A field's text as it is laid out and where, in units of the GUI. */
typedef struct tb_gui_field_view
{
    tb_font_layout layout;
    tb_text text;   /* the field's */
    tb_text shown;  /* what is drawn: the text, or `show` for each character */
    tb_text show;   /* the field's `show`; empty for none */
    float scale;    /* pixels a unit */
    tb_vec2 origin; /* where the layout's top left is */
} tb_gui_field_view;

/* The characters of a text before an offset. */
static size_t tb_gui_characters_before(tb_text text, size_t offset)
{
    const tb_ui_editor reader = {.buffer = (char *)(uintptr_t)text.bytes, .length = text.length};
    size_t count = 0;

    for (size_t at = 0; at < offset && at < text.length; at = tb_ui_editor_after(&reader, at))
    {
        count++;
    }
    return count;
}

/* The offset of a text's character at an index. */
static size_t tb_gui_character_offset(tb_text text, size_t index)
{
    const tb_ui_editor reader = {.buffer = (char *)(uintptr_t)text.bytes, .length = text.length};
    size_t offset = 0;

    for (size_t count = 0; count < index && offset < text.length; count++)
    {
        offset = tb_ui_editor_after(&reader, offset);
    }
    return offset;
}

static size_t tb_gui_to_shown(const tb_gui_field_view *view, size_t offset)
{
    if (view->show.length == 0)
    {
        return offset;
    }
    return tb_gui_characters_before(view->text, offset) * view->show.length;
}

static size_t tb_gui_from_shown(const tb_gui_field_view *view, size_t offset)
{
    if (view->show.length == 0)
    {
        return offset;
    }
    return tb_gui_character_offset(view->text, offset / view->show.length);
}

/* The text as it is shown: `show` for each character, from the arena. */
static tb_text tb_gui_masked(tb_gui *gui, tb_text text, tb_text show)
{
    const size_t characters = tb_gui_characters_before(text, text.length);
    char *bytes = NULL;

    if (show.length == 0 || characters == 0)
    {
        return show.length == 0 ? text : tb_text_make("", 0);
    }
    TB_ASSERT_MSG(characters < SIZE_MAX / show.length, "a text too large to show");
    bytes = tb_arena_alloc(gui->arena, characters * show.length, 1);
    for (size_t index = 0; index < characters; index++)
    {
        memcpy(bytes + index * show.length, show.bytes, show.length);
    }
    return tb_text_make(bytes, characters * show.length);
}

/* Lay a field's text out in a rectangle: false when there is no font. */
static bool tb_gui_field_view_of(tb_gui *gui, const tb_gui_field *field, tb_text text,
                                 tb_canvas_rect rect, tb_gui_field_view *view)
{
    const tb_gui_resolved_font resolved = tb_gui_resolve_font(gui, (tb_font_spec){0});
    const float scale = gui->view.scale;
    tb_font_layout_options options = {0};

    if (resolved.font == NULL)
    {
        return false;
    }
    view->text = text;
    view->show = tb_gui_string_text(&field->show);
    view->shown = tb_gui_masked(gui, text, view->show);
    view->scale = scale;
    if (field->wraps && field->editor.multiline)
    {
        options.wrap_width = rect.width * scale;
    }
    tb_font_layout_text(resolved.font, view->shown, resolved.size * scale, &options, gui->arena,
                        &view->layout);
    view->origin.x = rect.x - field->scroll.x;
    view->origin.y =
        field->editor.multiline ? rect.y - field->scroll.y : tb_gui_values_line_top(gui, rect);
    return true;
}

/* Where the caret at an offset is: its left, its line's top and bottom. */
static tb_font_caret tb_gui_caret_at(const tb_gui_field_view *view, size_t offset)
{
    tb_font_caret caret = {0};

    tb_font_layout_caret(&view->layout, tb_gui_to_shown(view, offset), &caret);
    caret.x = caret.x / view->scale + view->origin.x;
    caret.top = caret.top / view->scale + view->origin.y;
    caret.bottom = caret.bottom / view->scale + view->origin.y;
    return caret;
}

/* The offset nearest a point. */
static size_t tb_gui_hit_at(const tb_gui_field_view *view, tb_vec2 point)
{
    tb_font_caret caret = {0};

    tb_font_layout_hit(&view->layout, (point.x - view->origin.x) * view->scale,
                       (point.y - view->origin.y) * view->scale, &caret);
    return tb_gui_from_shown(view, caret.offset);
}

void tb_gui_field_follow(tb_widget widget, tb_gui_field *field, tb_canvas_rect rect)
{
    tb_gui_field_view view = {0};

    if (!tb_gui_field_view_of(widget.gui, field, tb_gui_field_text(field), rect, &view))
    {
        return;
    }
    const tb_font_caret caret = tb_gui_caret_at(&view, field->editor.caret);
    const float left = caret.x - view.origin.x;
    const float top = caret.top - view.origin.y;
    const float bottom = caret.bottom - view.origin.y;
    const float text_width = view.layout.width / view.scale;
    const float text_height = view.layout.height / view.scale;

    if (left - field->scroll.x > rect.width - TB_GUI_CARET_ROOM)
    {
        field->scroll.x = left - rect.width + TB_GUI_CARET_ROOM;
    }
    if (left < field->scroll.x)
    {
        field->scroll.x = left;
    }
    field->scroll.x =
        fmaxf(fminf(field->scroll.x, text_width - rect.width + TB_GUI_CARET_ROOM), 0.0f);
    if (!field->editor.multiline)
    {
        field->scroll.y = 0.0f;
        return;
    }
    if (bottom - field->scroll.y > rect.height)
    {
        field->scroll.y = bottom - rect.height;
    }
    if (top < field->scroll.y)
    {
        field->scroll.y = top;
    }
    field->scroll.y = fmaxf(fminf(field->scroll.y, text_height - rect.height), 0.0f);
}

/* ---------------------------------------------------------------------------
 * Edits, validated.
 * ------------------------------------------------------------------------- */

/* The text before an edit, to put back when it is refused. */
typedef struct tb_gui_field_before
{
    char *bytes;
    size_t length;
    size_t caret;
    size_t anchor;
} tb_gui_field_before;

static tb_gui_field_before tb_gui_field_keep(const tb_gui_field *field)
{
    const tb_ui_editor *const editor = &field->editor;
    tb_gui_field_before before = {.bytes = tb_malloc(editor->length + 1),
                                  .length = editor->length,
                                  .caret = editor->caret,
                                  .anchor = editor->anchor};

    memcpy(before.bytes, editor->buffer, editor->length + 1);
    return before;
}

/* After an edit: validate it, then set it in the variable and say it
 * changed; or put the text back when it is refused. */
static void tb_gui_field_commit(tb_widget widget, tb_gui_field *field, tb_gui_field_before *before)
{
    tb_ui_editor *const editor = &field->editor;
    tb_gui_event validate = {0};

    if (!editor->changed)
    {
        goto done;
    }
    editor->changed = false;
    field->refused = false;
    validate = tb_gui_event_for(widget, TB_GUI_EVENT_VALIDATE);
    validate.text = editor->buffer;
    (void)tb_gui_send(widget, &validate);
    if (!tb_widget_alive(widget))
    {
        goto done;
    }
    if (field->refused)
    {
        field->refused = false;
        tb_gui_field_reserve(field,
                             before->length > editor->length ? before->length - editor->length : 0);
        memcpy(editor->buffer, before->bytes, before->length + 1);
        editor->length = before->length;
        editor->caret = before->caret;
        editor->anchor = before->anchor;
        goto done;
    }
    if (field->deferred)
    {
        field->dirty = true;
        goto done;
    }
    if (tb_var_alive(field->variable))
    {
        (void)tb_gui_var_set_span(field->variable, tb_gui_field_text(field));
        field->synced = tb_var_version(field->variable);
    }
    tb_gui_values_changed(widget, (tb_gui_command){0});
done:
    tb_free(before->bytes);
    *before = (tb_gui_field_before){0};
}

/* Insert text at the caret, as typing or pasting does. */
static void tb_gui_field_insert(tb_widget widget, tb_gui_field *field, tb_text text)
{
    tb_gui_field_before before = {0};

    if (field->read_only)
    {
        return;
    }
    before = tb_gui_field_keep(field);
    tb_gui_field_reserve(field, text.length);
    (void)tb_ui_editor_insert(&field->editor, text);
    tb_gui_field_commit(widget, field, &before);
}

static void tb_gui_field_erase(tb_widget widget, tb_gui_field *field, bool forward,
                               uint32_t modifiers)
{
    tb_gui_field_before before = {0};

    if (field->read_only)
    {
        return;
    }
    before = tb_gui_field_keep(field);
    tb_ui_editor_erase(&field->editor, forward, modifiers);
    tb_gui_field_commit(widget, field, &before);
}

/* Copy the selection; cut it too. Nothing is copied from a field that
 * shows something else in place of its text, a password's. */
static void tb_gui_field_copy(tb_widget widget, tb_gui_field *field, bool cut)
{
    const tb_text selection = tb_ui_editor_selection(&field->editor);
    tb_gui_field_before before = {0};

    if (selection.length == 0 || field->show.length > 0)
    {
        return;
    }
    tb_gui_set_clipboard(widget.gui, selection);
    if (!cut || field->read_only)
    {
        return;
    }
    before = tb_gui_field_keep(field);
    (void)tb_ui_editor_delete_selection(&field->editor);
    tb_gui_field_commit(widget, field, &before);
}

/* ---------------------------------------------------------------------------
 * Keys.
 * ------------------------------------------------------------------------- */

/* Home or End: of the caret's line, or with Control of the text. */
static size_t tb_gui_field_line_end(const tb_gui_field *field, const tb_gui_field_view *view,
                                    bool end, uint32_t modifiers)
{
    tb_font_caret caret = {0};

    if (!field->editor.multiline || tb_ui_editor_shortcut(modifiers))
    {
        return end ? field->editor.length : 0;
    }
    tb_font_layout_caret(&view->layout, tb_gui_to_shown(view, field->editor.caret), &caret);
    const tb_font_layout_line *const line = &view->layout.lines[caret.line];
    return tb_gui_from_shown(view, end ? line->end : line->begin);
}

/* Up or Down by a distance in units: the offset at the caret's left there. */
static size_t tb_gui_field_vertical(const tb_gui_field *field, const tb_gui_field_view *view,
                                    float distance)
{
    const tb_font_caret caret = tb_gui_caret_at(view, field->editor.caret);
    const float middle = (caret.top + caret.bottom) * 0.5f;

    return tb_gui_hit_at(view, (tb_vec2){caret.x, middle + distance});
}

/* Keys that move the caret: whether the key is one. */
static bool tb_gui_field_move_key(tb_gui_field *field, const tb_gui_field_view *view, tb_key key,
                                  uint32_t modifiers, tb_canvas_rect rect)
{
    tb_ui_editor *const editor = &field->editor;
    const float line = view->layout.line_height / view->scale;

    if (key == TB_KEY_LEFT || key == TB_KEY_RIGHT)
    {
        tb_ui_editor_arrow(editor, key == TB_KEY_RIGHT, modifiers);
        return true;
    }
    if (key == TB_KEY_HOME || key == TB_KEY_END)
    {
        tb_ui_editor_move(editor, tb_gui_field_line_end(field, view, key == TB_KEY_END, modifiers),
                          modifiers);
        return true;
    }
    if (!editor->multiline)
    {
        return false;
    }
    if (key == TB_KEY_UP || key == TB_KEY_DOWN)
    {
        tb_ui_editor_move(
            editor, tb_gui_field_vertical(field, view, key == TB_KEY_UP ? -line : line), modifiers);
        return true;
    }
    if (key == TB_KEY_PAGE_UP || key == TB_KEY_PAGE_DOWN)
    {
        const float page = fmaxf(rect.height - line, line);
        tb_ui_editor_move(editor,
                          tb_gui_field_vertical(field, view, key == TB_KEY_PAGE_UP ? -page : page),
                          modifiers);
        return true;
    }
    return false;
}

/* Control or Command with A, C, X, or V. */
static bool tb_gui_field_shortcut(tb_widget widget, tb_gui_field *field, tb_key key)
{
    if (key == TB_KEY_A)
    {
        tb_ui_editor_select_all(&field->editor);
        return true;
    }
    if (key == TB_KEY_C || key == TB_KEY_X)
    {
        tb_gui_field_copy(widget, field, key == TB_KEY_X);
        return true;
    }
    if (key == TB_KEY_V)
    {
        tb_gui_field_insert(widget, field, tb_gui_clipboard(widget.gui));
        return true;
    }
    return false;
}

static bool tb_gui_field_key(tb_widget widget, tb_gui_field *field, const tb_gui_event *event,
                             tb_canvas_rect rect)
{
    tb_gui_field_view view = {0};
    const tb_key key = event->key;
    const uint32_t modifiers = event->modifiers;

    if (key == TB_KEY_BACKSPACE || key == TB_KEY_DELETE)
    {
        tb_gui_field_erase(widget, field, key == TB_KEY_DELETE, modifiers);
        return true;
    }
    if (key == TB_KEY_ENTER)
    {
        if (!field->editor.multiline)
        {
            return false;
        }
        tb_gui_field_insert(widget, field, tb_text_from_cstr("\n"));
        return true;
    }
    if (tb_ui_editor_shortcut(modifiers))
    {
        if (tb_gui_field_shortcut(widget, field, key))
        {
            return true;
        }
    }
    if (!tb_gui_field_view_of(widget.gui, field, tb_gui_field_text(field), rect, &view))
    {
        return false;
    }
    return tb_gui_field_move_key(field, &view, key, modifiers, rect);
}

/* ---------------------------------------------------------------------------
 * The mouse, and the rest.
 * ------------------------------------------------------------------------- */

static void tb_gui_field_point(tb_widget widget, tb_gui_field *field, tb_vec2 point,
                               uint32_t modifiers, tb_canvas_rect rect)
{
    tb_gui_field_view view = {0};

    if (tb_gui_field_view_of(widget.gui, field, tb_gui_field_text(field), rect, &view))
    {
        tb_ui_editor_move(&field->editor, tb_gui_hit_at(&view, point), modifiers);
    }
}

static bool tb_gui_field_wheel(tb_widget widget, tb_gui_field *field, const tb_gui_event *event,
                               tb_canvas_rect rect)
{
    tb_gui_field_view view = {0};
    const float before = field->scroll.y;

    if (!field->editor.multiline ||
        !tb_gui_field_view_of(widget.gui, field, tb_gui_field_text(field), rect, &view))
    {
        return false;
    }
    const float line = view.layout.line_height / view.scale;
    const float most = fmaxf(view.layout.height / view.scale - rect.height, 0.0f);
    field->scroll.y =
        fmaxf(fminf(field->scroll.y - event->wheel.y * line * TB_GUI_WHEEL_LINES, most), 0.0f);
    return field->scroll.y != before;
}

bool tb_gui_field_event(tb_widget widget, tb_gui_field *field, const tb_gui_event *event,
                        tb_canvas_rect rect)
{
    bool used = false;

    if (event->kind == TB_GUI_EVENT_VALIDATE)
    {
        if (field->validate.callback != NULL)
        {
            field->validate.callback(event, field->validate.user);
        }
        return true;
    }
    tb_gui_field_sync(field);
    switch (event->kind)
    {
    case TB_GUI_EVENT_FOCUS_IN:
        /* Tab selects a line's whole text; a click then places the caret. */
        if (!field->editor.multiline)
        {
            tb_ui_editor_select_all(&field->editor);
        }
        used = true;
        break;
    case TB_GUI_EVENT_FOCUS_OUT:
        field->dragging = false;
        used = true;
        break;
    case TB_GUI_EVENT_PRESS:
        if (event->button == TB_MOUSE_BUTTON_LEFT)
        {
            tb_gui_field_point(widget, field, event->position, event->modifiers, rect);
            field->dragging = true;
            used = true;
        }
        break;
    case TB_GUI_EVENT_MOTION:
        if (field->dragging)
        {
            tb_gui_field_point(widget, field, event->position, TB_MODIFIER_SHIFT, rect);
            used = true;
        }
        break;
    case TB_GUI_EVENT_RELEASE:
        field->dragging = false;
        break;
    case TB_GUI_EVENT_DOUBLE_CLICK:
        if (event->button == TB_MOUSE_BUTTON_LEFT)
        {
            tb_gui_field_point(widget, field, event->position, 0, rect);
            tb_ui_editor_select_word(&field->editor, field->editor.caret);
            field->dragging = false;
            used = true;
        }
        break;
    case TB_GUI_EVENT_TEXT:
        tb_gui_field_insert(widget, field, tb_text_from_cstr(event->text));
        used = true;
        break;
    case TB_GUI_EVENT_KEY:
        used = tb_gui_field_key(widget, field, event, rect);
        break;
    case TB_GUI_EVENT_WHEEL:
        return tb_gui_field_wheel(widget, field, event, rect);
    case TB_GUI_EVENT_ENTER:
    case TB_GUI_EVENT_LEAVE:
    case TB_GUI_EVENT_CLICK:
    case TB_GUI_EVENT_RESIZE:
    case TB_GUI_EVENT_COMMAND:
    case TB_GUI_EVENT_CHANGED:
    case TB_GUI_EVENT_VALIDATE:
    case TB_GUI_EVENT_POPUP_CLOSE:
    case TB_GUI_EVENT_BACK:
    case TB_GUI_EVENT_COUNT:
        break;
    }
    if (used && tb_widget_alive(widget))
    {
        field->blink = widget.gui->time;
        tb_gui_field_follow(widget, field, rect);
    }
    return used;
}

/* ---------------------------------------------------------------------------
 * Drawing.
 * ------------------------------------------------------------------------- */

/* The selection on one line of the layout. */
static void tb_gui_field_draw_selection(tb_gui_paint *paint, const tb_gui_field_view *view,
                                        size_t line_index, size_t start, size_t end,
                                        tb_canvas_rect clip)
{
    const tb_font_layout_line *const line = &view->layout.lines[line_index];
    const size_t begin = tb_gui_from_shown(view, line->begin);
    const size_t finish = tb_gui_from_shown(view, line->end);
    const float top = line->top / view->scale + view->origin.y;
    const float height = view->layout.line_height / view->scale;
    const float line_left = line->x / view->scale + view->origin.x;
    float left = line_left;
    float right = line_left + line->width / view->scale;

    if (end < begin || start > finish || (start == end))
    {
        return;
    }
    if (start >= begin)
    {
        left = tb_gui_caret_at(view, start).x;
    }
    if (end <= finish)
    {
        const tb_font_caret caret = tb_gui_caret_at(view, end);
        right = caret.line == line_index ? caret.x : right;
    }
    if (right <= left)
    {
        return;
    }
    tb_gui_paint_fill(paint,
                      tb_ui_rect_intersect((tb_canvas_rect){left, top, right - left, height}, clip),
                      0.0f, tb_gui_values_color(paint->widget, TB_UI_COLOR_SELECTION));
}

void tb_gui_field_draw(tb_widget widget, const tb_gui_field *field, tb_gui_paint *paint,
                       tb_canvas_rect rect, tb_text text)
{
    tb_gui *const gui = widget.gui;
    const uint32_t state = tb_widget_state(widget);
    const bool focused = (state & TB_WIDGET_FOCUSED) != 0;
    const bool disabled = (state & TB_WIDGET_DISABLED) != 0;
    const tb_canvas_rect clip = tb_ui_rect_intersect(paint->clip, rect);
    const tb_gui_resolved_font resolved = tb_gui_resolve_font(gui, (tb_font_spec){0});
    tb_gui_field_view view = {0};

    if (!tb_gui_field_view_of(gui, field, text, rect, &view))
    {
        return;
    }
    if (text.length == 0 && field->placeholder.length > 0)
    {
        const tb_gui_text_block placeholder = {.text = tb_gui_string_text(&field->placeholder),
                                               .anchor = field->editor.multiline ? TB_GUI_ANCHOR_NW
                                                                                 : TB_GUI_ANCHOR_W};
        tb_gui_paint_text(paint, &placeholder, rect,
                          tb_gui_values_color(widget, TB_UI_COLOR_TEXT_DIM));
    }
    const size_t caret = field->editor.caret < text.length ? field->editor.caret : text.length;
    const size_t anchor = field->editor.anchor < text.length ? field->editor.anchor : text.length;
    if (focused && caret != anchor)
    {
        const size_t start = caret < anchor ? caret : anchor;
        const size_t end = caret < anchor ? anchor : caret;
        for (size_t line = 0; line < view.layout.line_count; line++)
        {
            tb_gui_field_draw_selection(paint, &view, line, start, end, clip);
        }
    }
    if (view.shown.length > 0)
    {
        const tb_ui_text_run run = {
            .font = resolved.font,
            .size = resolved.size,
            .wrap_width = field->wraps && field->editor.multiline ? rect.width : 0.0f};
        tb_ui_paint_text(
            &gui->paint, paint->layer, clip, &run, view.shown, view.origin,
            tb_gui_values_color(widget, disabled ? TB_UI_COLOR_TEXT_DIM : TB_UI_COLOR_TEXT),
            (tb_ui_id)widget.index + 1u);
    }
    if (focused && fmod(gui->time - field->blink, 2.0 * TB_GUI_CARET_BLINK) < TB_GUI_CARET_BLINK)
    {
        const tb_font_caret at = tb_gui_caret_at(&view, caret);
        tb_gui_paint_fill(
            paint,
            tb_ui_rect_intersect((tb_canvas_rect){at.x, at.top, 1.0f, at.bottom - at.top}, clip),
            0.0f, tb_gui_values_color(widget, TB_UI_COLOR_TEXT));
    }
}

tb_text tb_gui_field_shown_text(const tb_gui_field *field)
{
    const uint64_t version = tb_var_version(field->variable);

    if (version != 0 && version != field->synced)
    {
        return tb_var_get_text(field->variable);
    }
    return tb_gui_field_text(field);
}
