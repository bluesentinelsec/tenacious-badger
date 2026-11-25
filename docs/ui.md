# The interface

The `ui` module is an immediate-mode GUI, in the manner of Dear ImGui and
microui, written in the engine's C: the game describes its interface every
frame, by calling a function for each widget, and each function returns
what the player did to it. Its code lives in `src/ui/` and its interface in
`badger/ui.h`. It is the base of the engine's tools: the sandbox's
inspector now, and the editor later.

```c
/* once */
tb_ui_fonts fonts = {0};
tb_ui_fonts_open(catalog, NULL, &fonts);
tb_ui_theme theme = tb_ui_theme_dark();
tb_ui_theme_set_fonts(&theme, &fonts);
tb_ui *ui = tb_ui_create(&(tb_ui_options){.theme = &theme});
tb_ui_input input = {0};

/* for each window event: */
tb_ui_input_add_event(&input, &event);

/* each frame */
input.scale = tb_canvas_view_logical(renderer).scale;
input.size = (tb_vec2){(float)width, (float)height};
input.delta_seconds = seconds;
tb_ui_begin_frame(ui, &input);
if (tb_ui_begin_window(ui, "Settings", (tb_canvas_rect){20, 20, 320, 400}, 0))
{
    tb_ui_checkbox(ui, "Vertical sync", &vsync);
    tb_ui_slider_float(ui, "Field of view", &fov, 60.0f, 120.0f, "%.0f");
    if (tb_ui_button(ui, "Apply"))
    {
        apply();
    }
    tb_ui_end_window(ui);
}
tb_ui_end_frame(ui);
tb_ui_input_next_frame(&input);
tb_ui_draw(ui, renderer); /* between tb_renderer_begin_frame() and tb_renderer_end_frame() */
```

## Frames, input, and drawing

A frame begins with `tb_ui_begin_frame`, which takes the frame's input, and
ends with `tb_ui_end_frame`. The input, `tb_ui_input`, is what the player
did in the frame: where the mouse is, the buttons held and those that went
down and came up, the wheel, the keys that went down with their modifiers,
and the text typed. `tb_ui_input_add_event` builds it from window events;
`tb_ui_input_next_frame` forgets what happened and keeps what is held. A
test, or a replay, makes it by hand.

The interface never touches a window or a renderer. A frame's drawing is a
list of commands (`tb_ui_commands`): rectangles, rounded or not, outlines,
circles, lines, and text, each with its clip and its canvas layer.
`tb_ui_draw` hands them to a renderer's canvas ([renderer](renderer.md#2d-the-canvas));
another backend could draw them as well. The retained [GUI](gui.md) draws
through the same commands, its theme is a `tb_ui_theme`, and both measure
text the same way: the layer beneath them is one, in `src/ui/ui_paint.c`.

## Units and scale

The interface is laid out in units, and drawn at `input.scale` pixels a
unit. The display's scale, from `tb_canvas_view_logical`, keeps it the same
size on every display; a larger scale zooms it. Its input is in pixels of
the frame, as window events give them. Edges are drawn on whole pixels,
and text at the display's own pixels, where the canvas places each glyph on
a whole pixel and each baseline on a whole row, so it is crisp at any scale.
Text is also measured at the display's pixels, and its measures turned back
into units, so a line is as long in units as the glyphs the display draws
for it. Text is centered in its line by its capitals, so labels and the text
in fields beside them share one baseline.

The scale is the input's, so a game sets it every frame from
`tb_canvas_view_logical`, which reads the window's content scale when it is
called. When the window moves to a display of another scale, the window
reports the new scale (`TB_WINDOW_EVENT_SCALE_CHANGED`) as its events are
pumped, and the next frame lays the interface out, and draws its text, at
the new scale.

## Identifiers and state

Each widget is known from frame to frame by an id, a hash of its label and
of the ids pushed around it (`tb_ui_push_id`, `tb_ui_push_id_int`,
`tb_ui_pop_id`). Each window, panel, and open tree node pushes its own. Two
widgets of one label in one window share an id and act as one; the frame's
statistics (`tb_ui_stats`) count each id used twice, and an id pushed
around each, such as a loop's index, tells them apart. A label may end in
`##` and text that is hashed but not shown: `"Delete##3"` shows "Delete",
and `"##volume"` shows nothing.

What the interface keeps between frames, it keeps by id: each window's
place, size, scroll, order, and whether it is collapsed; each panel's
scroll; each header's and tree node's open state; and the caret and
selection of the one text field being edited. The values the widgets show
are the caller's. State not seen for `forget_frames` frames, 600 unless
chosen, is forgotten.

## The mouse and the keyboard

The widget under the mouse, in the window in front, is *hot*. The one a
button went down on is *active* until the button comes up, and holds the
mouse while it is dragged, outside its window too. A click is the button
going down and coming up on the same widget.

Clicking a widget focuses it. Tab moves the focus to the next widget of its
window, Shift+Tab to the one before, around from the last to the first, and
Escape takes it away; with no focus, Tab starts at the front window. A
widget focused with Tab takes keys: Space or Enter presses a button,
toggles a checkbox or radio button, opens a combo, or opens a header; Left
and Right move a slider or a dragged number, and shut or open a node; Up
and Down choose a combo's item. A widget focused with a click takes no keys
but a text field's, so that the game keeps its keys.

## Sharing input with a game

After each frame the interface says what it takes:

| Function | True while |
| --- | --- |
| `tb_ui_wants_mouse` | the mouse is over one of its windows or the open list, or a widget is held |
| `tb_ui_wants_keyboard` | a text field has the focus, or Tab gave a widget the focus |
| `tb_ui_wants_text` | a text field has the focus |
| `tb_ui_cursor` | the cursor to show: an I-beam over text fields, arrows over edges that resize |

A game gives the interface every event, and keeps for itself those it does
not want: the sandbox's fly camera turns only when the right button goes
down outside the windows.

## Windows and layout

`tb_ui_begin_window` begins a window at the place and size given the first
time; after, it stays where the player put it. Its title bar moves it, and
its arrow collapses it; its corner and its right and bottom edges resize
it; a scroll bar, and the wheel, scroll contents taller than it. A click on
a window brings it in front. Flags leave out the title, moving, resizing,
collapsing, or the scroll bar. Windows are drawn from canvas layer
`tb_ui_options::layer` (100) up, one layer each, back to front; the open
combo's list and the tooltip are drawn above every window. A window kept
on the frame always shows enough of its title to be moved back.

Widgets go down a window one to a line. Fields (sliders, combos, text
fields, progress bars) are as wide as the window, their label on the left
in a column of `label_fraction` of the width; buttons and checkboxes are as
wide as their text.

| Function | Places |
| --- | --- |
| `tb_ui_same_line` | the next widget after the last, on its line |
| `tb_ui_row` | widgets in columns of given widths, one a column, line after line; a width of 0 shares what is left |
| `tb_ui_spacing`, `tb_ui_separator` | a gap, or a line across |
| `tb_ui_indent`, `tb_ui_unindent` | the widgets after further right, or back |
| `tb_ui_begin_panel` | a region of the window with its own border and scrolling |
| `tb_ui_begin_table` | a header row, then a widget a cell, every other row shaded, lines between columns |

## Widgets

| Function | Widget | Returns true |
| --- | --- | --- |
| `tb_ui_text`, `tb_ui_textf`, `tb_ui_text_colored`, `tb_ui_heading` | a line of text, as tall as a field so that it lines up with one | |
| `tb_ui_text_wrapped` | a paragraph wrapped at the window's width | |
| `tb_ui_text_mono` | a line of text in the monospace font: a log, or columns of text | |
| `tb_ui_plot` | values joined by lines in a box: frame times, say | |
| `tb_ui_value` | a label and a value read, not changed | |
| `tb_ui_button` | a button | when clicked |
| `tb_ui_checkbox` | a box checked or not | when toggled |
| `tb_ui_radio` | one of a group, of which one is chosen | when chosen |
| `tb_ui_selectable` | a line of a list | when clicked |
| `tb_ui_slider_float`, `tb_ui_slider_int` | a field filled to its value, set where the mouse presses and drags | when changed |
| `tb_ui_drag_float`, `tb_ui_drag_int` | a number dragged left and right, faster with Shift | when changed |
| `tb_ui_combo` | a field of one item that opens a list over everything | when another is chosen |
| `tb_ui_text_field` | one line of the caller's text, edited in place | when the text changed |
| `tb_ui_header` | a header that opens and shuts the widgets under it | while open |
| `tb_ui_tree_push`, `tb_ui_tree_pop` | a node of a tree, its children indented | while open |
| `tb_ui_progress` | a bar filled to a fraction, with its percentage | |
| `tb_ui_color_swatch` | a color over a checkerboard where it is transparent | when clicked |
| `tb_ui_color_edit` | a swatch and four channels, each dragged from 0 to 1 | when changed |
| `tb_ui_tooltip` | text by the mouse once it rests on the last widget | |

A text field has a caret and a selection: the arrows move by characters,
and Ctrl with them by words; Shift selects as it moves; Home and End go to
the ends; Backspace and Delete erase a character, a word with Ctrl, or the
selection; the mouse places the caret and drags a selection, and Shift with
a click extends it; Ctrl+A selects everything. Ctrl+C, Ctrl+X, and Ctrl+V
copy, cut, and paste on the interface's own clipboard, which
`tb_ui_clipboard` and `tb_ui_set_clipboard` let a game connect to the
system's. Text past the buffer's capacity is dropped, whole characters at a
time, and control characters are never typed. Enter or Escape ends the
editing. Text scrolls in its field to keep the caret seen. Tab into a field
selects its text, so that typing replaces it.

## Themes

`tb_ui_theme` holds the colors, the fonts, the text's size, and the
spacing, padding, rounding, and widths of everything, in units.
`tb_ui_theme_dark` is the default: dark grays and a blue accent after
Unreal's editor, text at the UI role's default size (16 units).

A theme has three fonts, all drawn at its size: `TB_UI_FONT_REGULAR` for
text, `TB_UI_FONT_HEADING` for titles, headings, and headers, and
`TB_UI_FONT_MONO` for what a caller wants in columns. `tb_ui_fonts_open`
opens them from a catalog ([fonts](fonts.md#roles)), and
`tb_ui_theme_set_fonts` gives them to a theme with their size. Given no
specs, it opens the UI, heading, and monospace roles, and the theme takes
the UI role's size. No role is bold, so neither is the interface, unless
its caller asks: given specs, it opens the font of each.

```c
const tb_font_spec heading = tb_font_spec_of_role(catalog, TB_FONT_ROLE_HEADING);
const tb_font_spec specs[TB_UI_FONT_COUNT] = {
    [TB_UI_FONT_REGULAR] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_UI),
    [TB_UI_FONT_HEADING] = tb_font_spec_bold(heading), /* bold titles, asked for */
    [TB_UI_FONT_MONO] = tb_font_spec_of_role(catalog, TB_FONT_ROLE_MONOSPACE)};
tb_ui_fonts_open(catalog, specs, &fonts);
```

A build of the engine that carries no fonts (`TB_EMBED_FONTS=none`) gives
an interface a font of its own.

## Not done yet

- The system's clipboard; the interface's own stands in.
- Typing a value into a slider with Ctrl+click.
- Text fields of several lines, input methods for composed scripts, and
  disabled widgets.
- Horizontal scrolling: contents wider than a window are cut off.
- Docking windows into one another.

## Threading

A context, and the input given to it, are externally synchronized: one
thread builds its frames at a time. `tb_ui_draw` runs on the thread that
renders. Contexts are independent of one another.

## Tests

`tests/ui/` drives the interface with input made by hand, with no window:
clicks that return true exactly once, sliders that follow the mouse in
proportion, checkboxes and radio buttons, text fields edited by keys, text,
the mouse, and the clipboard, Tab's focus, combos that open and choose,
windows that move, resize, collapse, and come in front, the wheel and the
scroll bar, what the interface wants of the mouse and keyboard, ids told
apart by ids pushed, state forgotten, and tooltips; and that text is
measured at the display's pixels at scales of 1.5 and 2, and that a change
of scale between frames is laid out at the new scale. `ui_draw_test.c`
draws a frame on the null backend's canvas. The sandbox's `ui` scene shows every
widget by eye ([sandbox](sandbox.md)).
