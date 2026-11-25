/* Text being edited, shared by the two interfaces' fields: the immediate
 * `ui`'s text field and the retained `gui`'s entries and text boxes. A
 * buffer of UTF-8 with a caret and a selection as byte offsets, and the
 * edits keys make on it: characters and words, Home and End, erasing, and
 * the selection. Private to the engine; the clipboard and drawing are each
 * interface's own. */
#ifndef TB_UI_EDITOR_H
#define TB_UI_EDITOR_H

#include "badger/text.h"
#include "badger/window.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A buffer being edited. The caller owns the buffer, `capacity` bytes, and
 * keeps it terminated; an edit never writes past it. */
typedef struct tb_ui_editor
{
    char *buffer;
    size_t length;   /* bytes, the terminator not counted */
    size_t capacity; /* bytes of the buffer, the terminator's included */
    size_t caret;
    size_t anchor;         /* the selection's other end; the caret when none */
    size_t max_characters; /* characters it holds at most; 0 for no limit */
    bool multiline;        /* it takes line breaks */
    bool changed;          /* an edit changed the text; the caller clears it */
} tb_ui_editor;

/* Whether Control, or Command, is held: words, and the clipboard's keys. */
bool tb_ui_editor_shortcut(uint32_t modifiers);

/* The boundary before the character that ends at `offset`, and after the
 * one that begins there; a byte of no character is a character alone. */
size_t tb_ui_editor_before(const tb_ui_editor *editor, size_t offset);
size_t tb_ui_editor_after(const tb_ui_editor *editor, size_t offset);

/* The start of the word before `offset`, and the end of the one after:
 * words are what lies between spaces and ASCII punctuation. */
size_t tb_ui_editor_word_before(const tb_ui_editor *editor, size_t offset);
size_t tb_ui_editor_word_after(const tb_ui_editor *editor, size_t offset);

/* The selection's ends, the first first, and its text. */
size_t tb_ui_editor_selection_start(const tb_ui_editor *editor);
size_t tb_ui_editor_selection_end(const tb_ui_editor *editor);
tb_text tb_ui_editor_selection(const tb_ui_editor *editor);

/* Take a buffer's text again after it changed elsewhere: its length from
 * its terminator, and the caret and selection kept within it. */
void tb_ui_editor_refresh(tb_ui_editor *editor);

/* Remove the bytes from `start` to `end`; the caret goes to `start`. */
void tb_ui_editor_remove(tb_ui_editor *editor, size_t start, size_t end);

/* Remove the selection: true when there was one. */
bool tb_ui_editor_delete_selection(tb_ui_editor *editor);

/* Insert text at the caret in place of the selection: as many whole
 * characters as the buffer and the limit hold, without control characters,
 * a line break aside in a multiline editor. Returns the bytes inserted. */
size_t tb_ui_editor_insert(tb_ui_editor *editor, tb_text text);

/* Move the caret, and with Shift the selection's end with it. */
void tb_ui_editor_move(tb_ui_editor *editor, size_t offset, uint32_t modifiers);

/* Left or Right: by a character, or a word with Control; without Shift, a
 * selection shuts to its edge on that side. */
void tb_ui_editor_arrow(tb_ui_editor *editor, bool right, uint32_t modifiers);

/* Backspace or Delete: the selection, or a character or a word. */
void tb_ui_editor_erase(tb_ui_editor *editor, bool forward, uint32_t modifiers);

/* Select everything. */
void tb_ui_editor_select_all(tb_ui_editor *editor);

/* Select the word at an offset, as a double click does. */
void tb_ui_editor_select_word(tb_ui_editor *editor, size_t offset);

#endif
