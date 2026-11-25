/* Text being edited: see ui_editor.h. */
#include "ui_editor.h"

#include "badger/assert.h"
#include "badger/utf8.h"

#include <string.h>

/* What a byte that begins no character reads as. */
#define TB_UI_EDITOR_REPLACEMENT 0xFFFDu
/* The first character that is not a control character, and the one after
 * the ASCII ones that is. */
#define TB_UI_EDITOR_FIRST_PRINTED 0x20u
#define TB_UI_EDITOR_DELETE 0x7Fu

bool tb_ui_editor_shortcut(uint32_t modifiers)
{
    return (modifiers & (TB_MODIFIER_CONTROL | TB_MODIFIER_SUPER)) != 0;
}

size_t tb_ui_editor_before(const tb_ui_editor *editor, size_t offset)
{
    uint32_t scalar = 0;
    size_t previous = 0;

    if (offset == 0)
    {
        return 0;
    }
    if (tb_utf8_previous(editor->buffer, editor->length, offset, &scalar, &previous))
    {
        return previous;
    }
    return offset - 1;
}

size_t tb_ui_editor_after(const tb_ui_editor *editor, size_t offset)
{
    uint32_t scalar = 0;
    size_t next = 0;

    if (offset >= editor->length)
    {
        return editor->length;
    }
    if (tb_utf8_decode(editor->buffer, editor->length, offset, &scalar, &next))
    {
        return next;
    }
    return offset + 1;
}

static bool tb_ui_editor_word_byte(char byte)
{
    const unsigned char value = (unsigned char)byte;

    if (value >= 0x80)
    {
        return true;
    }
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

size_t tb_ui_editor_word_before(const tb_ui_editor *editor, size_t offset)
{
    while (offset > 0 && !tb_ui_editor_word_byte(editor->buffer[offset - 1]))
    {
        offset = tb_ui_editor_before(editor, offset);
    }
    while (offset > 0 && tb_ui_editor_word_byte(editor->buffer[offset - 1]))
    {
        offset = tb_ui_editor_before(editor, offset);
    }
    return offset;
}

size_t tb_ui_editor_word_after(const tb_ui_editor *editor, size_t offset)
{
    while (offset < editor->length && tb_ui_editor_word_byte(editor->buffer[offset]))
    {
        offset = tb_ui_editor_after(editor, offset);
    }
    while (offset < editor->length && !tb_ui_editor_word_byte(editor->buffer[offset]))
    {
        offset = tb_ui_editor_after(editor, offset);
    }
    return offset;
}

size_t tb_ui_editor_selection_start(const tb_ui_editor *editor)
{
    return editor->caret < editor->anchor ? editor->caret : editor->anchor;
}

size_t tb_ui_editor_selection_end(const tb_ui_editor *editor)
{
    return editor->caret < editor->anchor ? editor->anchor : editor->caret;
}

tb_text tb_ui_editor_selection(const tb_ui_editor *editor)
{
    const size_t start = tb_ui_editor_selection_start(editor);
    const size_t end = tb_ui_editor_selection_end(editor);

    return tb_text_make(editor->buffer + start, end - start);
}

void tb_ui_editor_refresh(tb_ui_editor *editor)
{
    const char *const end = memchr(editor->buffer, '\0', editor->capacity);

    TB_ASSERT_MSG(editor->capacity > 0, "a text being edited without room for its terminator");
    editor->length = end != NULL ? (size_t)(end - editor->buffer) : editor->capacity - 1;
    editor->buffer[editor->length] = '\0';
    editor->caret = editor->caret < editor->length ? editor->caret : editor->length;
    editor->anchor = editor->anchor < editor->length ? editor->anchor : editor->length;
}

void tb_ui_editor_remove(tb_ui_editor *editor, size_t start, size_t end)
{
    if (end <= start)
    {
        return;
    }
    memmove(editor->buffer + start, editor->buffer + end, editor->length - end);
    editor->length -= end - start;
    editor->buffer[editor->length] = '\0';
    editor->caret = start;
    editor->anchor = start;
    editor->changed = true;
}

bool tb_ui_editor_delete_selection(tb_ui_editor *editor)
{
    const size_t start = tb_ui_editor_selection_start(editor);
    const size_t end = tb_ui_editor_selection_end(editor);

    tb_ui_editor_remove(editor, start, end);
    return end > start;
}

/* Characters in the buffer: a byte that begins none is one. */
static size_t tb_ui_editor_characters(const tb_ui_editor *editor)
{
    size_t count = 0;

    for (size_t offset = 0; offset < editor->length; offset = tb_ui_editor_after(editor, offset))
    {
        count++;
    }
    return count;
}

static bool tb_ui_editor_takes(const tb_ui_editor *editor, uint32_t scalar)
{
    if (scalar == '\n')
    {
        return editor->multiline;
    }
    return scalar >= TB_UI_EDITOR_FIRST_PRINTED && scalar != TB_UI_EDITOR_DELETE &&
           scalar != TB_UI_EDITOR_REPLACEMENT;
}

size_t tb_ui_editor_insert(tb_ui_editor *editor, tb_text text)
{
    size_t room = 0;
    size_t written = 0;
    size_t characters = 0;

    (void)tb_ui_editor_delete_selection(editor);
    TB_ASSERT(editor->capacity > editor->length);
    room = editor->capacity - 1 - editor->length;
    characters = editor->max_characters > 0 ? tb_ui_editor_characters(editor) : 0;
    for (size_t offset = 0; offset < text.length;)
    {
        uint32_t scalar = 0;
        size_t next = offset + 1;
        if (!tb_utf8_decode(text.bytes, text.length, offset, &scalar, &next))
        {
            scalar = TB_UI_EDITOR_REPLACEMENT;
            next = offset + 1;
        }
        const size_t size = next - offset;
        if (!tb_ui_editor_takes(editor, scalar))
        {
            offset = next;
            continue;
        }
        if (written + size > room ||
            (editor->max_characters > 0 && characters >= editor->max_characters))
        {
            break;
        }
        /* Make room at the caret, then copy the character in. */
        const size_t at = editor->caret + written;
        memmove(editor->buffer + at + size, editor->buffer + at, editor->length - at);
        memcpy(editor->buffer + at, text.bytes + offset, size);
        editor->length += size;
        written += size;
        characters++;
        offset = next;
    }
    editor->buffer[editor->length] = '\0';
    editor->caret += written;
    editor->anchor = editor->caret;
    editor->changed = editor->changed || written > 0;
    return written;
}

void tb_ui_editor_move(tb_ui_editor *editor, size_t offset, uint32_t modifiers)
{
    editor->caret = offset < editor->length ? offset : editor->length;
    if ((modifiers & TB_MODIFIER_SHIFT) == 0)
    {
        editor->anchor = editor->caret;
    }
}

void tb_ui_editor_arrow(tb_ui_editor *editor, bool right, uint32_t modifiers)
{
    const bool selecting = (modifiers & TB_MODIFIER_SHIFT) != 0;
    const bool words = tb_ui_editor_shortcut(modifiers);
    size_t offset = editor->caret;

    if (!selecting && editor->caret != editor->anchor && !words)
    {
        offset = right ? tb_ui_editor_selection_end(editor) : tb_ui_editor_selection_start(editor);
    }
    else if (words)
    {
        offset = right ? tb_ui_editor_word_after(editor, offset)
                       : tb_ui_editor_word_before(editor, offset);
    }
    else
    {
        offset = right ? tb_ui_editor_after(editor, offset) : tb_ui_editor_before(editor, offset);
    }
    tb_ui_editor_move(editor, offset, modifiers);
}

void tb_ui_editor_erase(tb_ui_editor *editor, bool forward, uint32_t modifiers)
{
    const size_t caret = editor->caret;
    const bool words = tb_ui_editor_shortcut(modifiers);

    if (tb_ui_editor_delete_selection(editor))
    {
        return;
    }
    if (forward)
    {
        tb_ui_editor_remove(editor, caret,
                            words ? tb_ui_editor_word_after(editor, caret)
                                  : tb_ui_editor_after(editor, caret));
        return;
    }
    tb_ui_editor_remove(editor,
                        words ? tb_ui_editor_word_before(editor, caret)
                              : tb_ui_editor_before(editor, caret),
                        caret);
}

void tb_ui_editor_select_all(tb_ui_editor *editor)
{
    editor->anchor = 0;
    editor->caret = editor->length;
}

void tb_ui_editor_select_word(tb_ui_editor *editor, size_t offset)
{
    size_t start = offset < editor->length ? offset : editor->length;
    size_t end = start;

    while (start > 0 && tb_ui_editor_word_byte(editor->buffer[start - 1]))
    {
        start = tb_ui_editor_before(editor, start);
    }
    while (end < editor->length && tb_ui_editor_word_byte(editor->buffer[end]))
    {
        end = tb_ui_editor_after(editor, end);
    }
    editor->anchor = start;
    editor->caret = end;
}
