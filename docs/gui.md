# The GUI

The `gui` module is a retained GUI, in the manner of Tkinter: widgets are
made once, with their options, placed in a grid, and run by the toolkit;
the game keeps its own loop and hands the GUI its events. Its code lives in
`src/gui/` and its interface in `badger/gui.h`. It is meant for tools and
games alike, and is the base of the drag-and-drop editor to come, which will
read and write its widgets as data.

The immediate-mode [interface](ui.md) stays, for debug overlays and the
inspector. The two share the layer beneath them: the theme and its fonts,
text measured at the display's pixels, and the list of draw commands the
canvas draws.

```c
/* once */
tb_gui *gui = tb_gui_create(&(tb_gui_options){.theme = &theme, .catalog = catalog});
tb_widget root = tb_gui_root(gui);
tb_grid_column_configure(root, 0, &(tb_grid_track){.weight = 1});
tb_grid_row_configure(root, 0, &(tb_grid_track){.weight = 1});

tb_widget form = tb_frame(root, &(tb_frame_desc){.padding = 12, .background = true});
tb_grid(form, &(tb_grid_options){.sticky = TB_STICKY_NSEW});

tb_widget label = tb_label(form, &(tb_label_desc){.text = "Field of view"});
tb_grid(label, &(tb_grid_options){.row = 0, .column = 0, .sticky = TB_STICKY_W});
tb_widget apply = tb_button(form, &(tb_button_desc){.text = "Apply", .command = on_apply, .user = game});
tb_grid(apply, &(tb_grid_options){.row = 1, .column = 1, .sticky = TB_STICKY_E});
tb_grid_column_configure(form, 1, &(tb_grid_track){.weight = 1});

/* for each window event */
tb_gui_feed_event(gui, &event);

/* each frame */
tb_gui_resize(gui, width, height, tb_canvas_view_logical(renderer).scale);
tb_gui_update(gui, delta_seconds);
tb_gui_draw(gui, renderer); /* between tb_renderer_begin_frame() and tb_renderer_end_frame() */
```

## Widgets and handles

Widgets form a tree under the root, `tb_gui_root()`, a frame as large as the
GUI. Each widget is made as the last child of another (`tb_frame`,
`tb_label`, `tb_button`, or `tb_widget_create` for any kind), and lives until
it, or an ancestor, is destroyed with `tb_widget_destroy`; its children are
destroyed with it, children first. The root is never destroyed: destroying
it empties it.

A widget is a handle, `tb_widget`: its GUI, a slot, and the slot's
generation. Handles are values, copied and kept freely. A widget destroyed
leaves its handles stale, and its slot's next widget has another
generation, so a stale handle is never mistaken for a widget alive. Every
function given a stale handle does nothing and says so: a setter returns
false, a getter returns zero, empty text, or no widget, and
`tb_widget_alive` returns false. A handle is not used once its GUI is
destroyed.

`tb_widget_parent`, `tb_widget_first_child`, `tb_widget_next_sibling`, and
`tb_widget_child_count` walk the tree; children are in the order they were
made, which is the order they are drawn in and Tab visits them.

## Kinds

Each widget has a kind, `tb_widget_class`: a table of functions the GUI
calls for each widget of the kind, with the widget's data, and a table of
its properties. Frames, labels, and buttons are kinds of the engine's; a
game makes its own the same way, and nothing in the GUI tells them apart.

| Function | Called |
| --- | --- |
| `init` | when a widget is made, its data zeroed: its first values |
| `destroy` | when its memory is released: what its data holds besides its text |
| `measure` | at layout: the size its content wants, in units |
| `insets` | at layout: the space between its edges and its children's grid |
| `arrange` | at layout, once it has a place: before its children are placed |
| `draw` | each update: drawn under its children, with `tb_gui_paint_*` |
| `event` | after its bindings: returns whether it used the event |

`flags` say whether Tab gives a widget the focus (`TB_WIDGET_CLASS_FOCUSABLE`),
and whether a click does (`TB_WIDGET_CLASS_FOCUS_ON_CLICK`, for fields to
type in). `mouse_filter` is its widgets' filter when they are made.
`style` says what it draws by: its style class, states, and elements (see
[styles](#styles)).
`tb_gui_register_class` registers a kind by its name, so that layout files
and the editor find it (`tb_gui_find_class`); the engine's are registered
when a GUI is made. A widget is made of any kind, registered or not.

A kind draws through a `tb_gui_paint`: rectangles filled and outlined,
lines, and text set in a rectangle by its anchor (`tb_gui_paint_text`), each
clipped to the widget and its ancestors. `tb_widget_state` says whether a
widget is hovered, pressed, focused, or disabled, and its style in that
state (`tb_style_of`) what to draw it with.

## Properties

Every property of a widget is plain data, enumerated, read, and set by
name, which is what layout files and the editor will use:

```c
for (size_t index = 0; index < tb_widget_property_count(widget); index++)
{
    const tb_gui_property *property = tb_widget_property_at(widget, index);
    tb_gui_value value = {0};
    tb_widget_get(widget, property->name, &value);
}
tb_widget_set(widget, "text", &(tb_gui_value){TB_GUI_TYPE_TEXT, {.text = TB_TEXT("Apply")}});
tb_widget_set(widget, "sticky", &(tb_gui_value){TB_GUI_TYPE_FLAGS, {.flags = TB_STICKY_EW}});
```

A value is one of the types of `tb_gui_type`: a bool, an integer, a number,
a pair, a color, text, one of an enumeration's names, flags named bit by
bit, a font spec, or a command (a callback and its argument). An
enumeration's and flags' names are in the property, as a layout file
writes them (`"nw"`, `"left"`, `"hand"`). Setting checks the value: its
type, an enumeration's range, the flags' bits, a finite number, and what a
property's own function refuses, such as a negative padding; a refused
value changes nothing. A property whose change changes a size
(`TB_GUI_PROPERTY_LAYOUT`) lays the widgets out again.

Every widget has the common properties, before its kind's:

| Property | Type | What |
| --- | --- | --- |
| `name` | text | its name, for finding it by path |
| `style` | text | its style class, for themes' styles |
| `visible` | bool | hidden, it and its children are neither drawn, found by the mouse, nor given space |
| `enabled` | bool | disabled, it and its children take no events and no focus, and are drawn dimmed |
| `cursor` | enum | the cursor over it; `inherit`, its parent's |
| `mouse_filter` | enum | `stop`, `pass`, or `ignore` (see [the mouse](#the-mouse)) |
| `gridded` | bool | whether it is in its parent's grid |
| `row`, `column`, `row_span`, `column_span` | int | its cell, and how many it spans |
| `sticky` | flags | `n`, `s`, `e`, `w`: the edges of its cell it is stuck to |
| `padx`, `pady` | pair | space outside it, on each side |
| `ipadx`, `ipady` | number | space added inside it, on both sides |
| `grid_anchor` | enum | where its own grid is placed when it has room to spare |

A kind's property is a value at an offset in the widget's data, which the
GUI reads and writes itself, or a pair of functions. Text at an offset is a
`tb_gui_string`, which the GUI copies in when it is set and releases when it
is set again or the widget is destroyed. `tb_widget_set_text`,
`tb_widget_set_enabled`, `tb_widget_set_visible`, `tb_widget_set_style`,
and their getters are shorthand for the same.

## The grid

Grid is the only layout: there is no pack and no place. A widget is shown
once it is placed in its parent's grid with `tb_grid`, in the manner of Tk's
`grid`:

```c
tb_grid(widget, &(tb_grid_options){.row = 1, .column = 0, .column_span = 2,
                                   .sticky = TB_STICKY_EW, .padx = {4, 4}, .ipady = 2});
tb_grid_column_configure(container, 1, &(tb_grid_track){.weight = 1, .minsize = 120});
tb_grid_column_configure(container, 2, &(tb_grid_track){.uniform = "buttons"});
tb_grid_anchor(container, TB_GUI_ANCHOR_CENTER);
```

**Sizes.** Each row and column is as large as the widgets wholly in it
want: each widget's natural size, with its inner padding (`ipadx`, `ipady`)
twice and its outer padding (`padx`, `pady`) on each side. A row or column
with a widget in it adds its `pad`, half on each side of its cells; then it
is at least its `minsize`, empty or not. A configured row or column exists,
even empty, as do those before it.

**Spans.** A widget spanning several rows or columns that are together too
small for it widens them, after the widgets of one cell, narrower spans
first: the missing space goes to them by their weights, or evenly when
none has weight.

**Uniform groups.** Rows or columns of one `uniform` name are sized in
proportion to their weights, a weight of 0 counted as 1: as large as the
largest needs. Buttons in a row of one group are of one width.

**Extra space.** When the container is larger than its grid, the extra is
shared out by weight. When no row or column has weight, the grid is placed
in the extra by the container's anchor, `grid_anchor`, the top left unless
chosen: anchored HUD layouts are grids with weighted empty rows and
columns, or an anchor.

**Too little space.** When the container is smaller, the rows and columns
of weight give space back by their weights, down to their minimum sizes;
what is still too large is cut off at the right and the bottom, and the
widgets there are clipped.

**In the cell.** A widget is its natural size, in the middle of its cell,
unless it is stuck to edges with `sticky`: to one edge it is placed against
it; to two opposite edges it is stretched between them.

**Rounding.** Every edge is placed on a whole pixel at the GUI's scale. The
edges of a row of cells are summed unrounded and each is rounded alone, so
the error never grows across a row, and cells side by side share their
edges, with neither gap nor overlap, at any scale.

**When.** The widgets are laid out only when something changed: a widget
made, destroyed, placed, shown or hidden, a property that changes a size,
a row or column configured, the theme, or the view. Each widget whose size
a layout changed is sent `TB_GUI_EVENT_RESIZE`. `tb_widget_rect` and
`tb_widget_natural_size` say what the last layout made of a widget, and
`tb_grid_size` and `tb_grid_bbox` of a grid, as Tk's `grid size` and `grid
bbox` do.

A frame's grid is inside its padding and border; nested frames make nested
grids.

## Units and scaling

Everything is laid out in units, and drawn at the view's scale of pixels a
unit. `tb_gui_resize` gives the GUI the frame's size in pixels and the
display's scale, each frame; the view (`tb_gui_view_of`) follows, and the
widgets are laid out again when it changes.

A tool's GUI has no reference size: a unit is a logical pixel, the
display's scale in pixels, and the root is as large as the frame. A game's
is designed at a reference size (`tb_gui_options::reference_size`) and
stretched to the frame, after Godot's `content_scale` modes:

| Stretch | Scale | The root |
| --- | --- | --- |
| `TB_GUI_STRETCH_NONE` | the display's scale | as large as the frame |
| `TB_GUI_STRETCH_KEEP_ASPECT` | as large as the reference fits | the reference size, centered, the rest of the frame empty |
| `TB_GUI_STRETCH_EXPAND` | as the reference fits, by the smaller axis | the reference size, and the other axis's extra |
| `TB_GUI_STRETCH_INTEGER` | as keep-aspect, by a whole number of pixels, at least 1 | the reference size, on whole pixels |

The stretch modes fit the reference to the frame's pixels, so a frame of
twice the density shows the same layout in twice the pixels, and the
display's scale does not change them. `tb_gui_options::scale`, and
`tb_gui_set_scale`, is a factor on top in every mode, an options menu's
interface scale: larger units, and fewer of them in the root.

Text is measured and drawn at the frame's own pixels, at the view's scale,
never drawn small and scaled up, and its measures turned back into units,
so a label is as wide in units as the glyphs the display draws for it.

## Events

The game hands the GUI its window events as they come, with
`tb_gui_feed_event`; `tb_gui_update` handles them in their order, and runs
the callbacks as it goes.

**The mouse.** The widget under the mouse is found in the order of the tree,
the last drawn first, within the clip of each ancestor: a widget cut off by
its parent is not found where it is not seen, and a hidden one nowhere. It
is sent `TB_GUI_EVENT_ENTER`, and `TB_GUI_EVENT_LEAVE` when the mouse moves
off it. A press sends `TB_GUI_EVENT_PRESS`, and the widget pressed holds
the mouse until its button comes up: no other widget is entered meanwhile,
the widget is hovered only while the mouse is over it, and the release goes
to it wherever the mouse is (`TB_GUI_EVENT_RELEASE`). A release over it is
a click (`TB_GUI_EVENT_CLICK`). A second press of the same button on the same
widget, within half a second and four units of the first, is also a double
click (`TB_GUI_EVENT_DOUBLE_CLICK`). The wheel goes to the widget under the
mouse (`TB_GUI_EVENT_WHEEL`).

A widget's `mouse_filter` says how it takes the mouse: `stop` takes its
events and keeps the mouse from the game (buttons, and frames with a
background); `pass` takes its events and leaves the mouse to the game
(frames that only lay others out, labels, the root); `ignore` is never
found, and what is under it is.

**The keyboard.** One widget at a time has the focus. Tab gives it to the
next widget that takes it, in the order of the tree, around from the last
to the first, and Shift+Tab to the one before; hidden and disabled widgets
are passed over. A press on a kind that takes the focus on a click gives it
the focus; a press anywhere else takes the focus away, so the game has its
keys again. Keys go to the widget with the focus (`TB_GUI_EVENT_KEY`), and
typed text (`TB_GUI_EVENT_TEXT`). `TB_GUI_EVENT_FOCUS_IN` and
`TB_GUI_EVENT_FOCUS_OUT` say when the focus comes and goes.
`tb_widget_focus`, `tb_gui_clear_focus`, and `tb_gui_focus_next` move it by
hand.

**Up the tree.** A key and the wheel are given to the widget, then to each
of its ancestors, until a kind uses it: a frame's binding to a key sees the
keys its buttons do not use, as a dialog's Escape.

**Disabled and hidden widgets.** A disabled widget is found by the mouse,
which it keeps from the widgets under it, but takes no event, and no focus.
A widget disabled or hidden loses the focus and lets go of the mouse.

**Modal widgets.** `tb_widget_show_modal` shows a widget in front of every
other, on a canvas layer above them, over a shade, and alone in taking
input: the mouse finds nothing outside it, Tab goes around its own widgets,
and keys with no focus go to it. Modal widgets stack; the last shown is in
front. One ends with `tb_widget_end_modal`, or when it is hidden or
destroyed. A modal widget is in the tree like any other, placed by its
parent's grid: a dialog is a frame gridded over the root's cells.

**Callbacks.** `tb_bind(widget, kind, callback, user)` calls a function
when an event of a kind happens to a widget; several may be bound to one
event, and are called in the order bound, before the widget's kind handles
the event. `tb_unbind` removes them. A button's `command` is called when it
is invoked: clicked with the left button, or Space or Enter pressed while it
has the focus, or `tb_widget_invoke`; its `TB_GUI_EVENT_COMMAND` bindings
are called first.

Callbacks run during `tb_gui_update`, one at a time, never inside the GUI's
walk of the tree. A callback may make, change, and destroy widgets, the
one it was called for included: a widget destroyed gets no more events, the
bindings after the one that destroyed it are not called, and its memory is
released once the event's callbacks are done. A widget made in a callback
takes events from the next event on.

**Sharing input with a game.** After an update the GUI says what it takes:

| Function | True while |
| --- | --- |
| `tb_gui_wants_mouse` | the mouse is over a widget that stops it, a widget that stops it is held, or a modal widget is shown |
| `tb_gui_wants_keyboard` | a widget has the focus, or a popup that takes input is shown |
| `tb_gui_cursor` | the cursor to show: the widget's under the mouse, or its nearest ancestor's that chose one |

## Frames, labels, and buttons

| Kind | Properties | What |
| --- | --- | --- |
| `frame` | `padding`, `background`, `border` | a container of widgets in its grid; filled with its style's background it stops the mouse |
| `label` | `text`, `wrap_length`, `justify`, `anchor`, `font` | text that is read: wrapped at a length, its lines justified against one another, set in its cell by its anchor, in a font asked for by spec |
| `button` | `text`, `command`, `default` | text that is clicked to call its command; drawn by its style's states: lit under the mouse, darker held, ringed with the focus, and ringed thinly as a dialog's default |

A label's or button's font is its style's unless it asks for another by
`tb_font_spec`: the GUI opens it from its catalog (`tb_gui_options::catalog`)
once, keeps it until it is destroyed, and draws in the GUI's font a spec
the catalog does not have. A spec of weight 0 is the style's font, and a
size of 0 the style's size ([fonts of styles](#fonts-of-styles)). Text wider than its rectangle starts at the
rectangle's left edge, and is cut off at its right.

## Variables and the widgets that edit values

A variable, `tb_var`, holds a value the GUI owns, after Tk's `BooleanVar`,
`IntVar`, `DoubleVar`, and `StringVar`: a bool, an integer, a number, or
text. Widgets show a variable's value and set it when they are used, so
several widgets on one variable stay in step, as a slider and a spin box on
one field of view; callbacks watch it; and it may be bound to an engine
setting, so that an options menu needs no code between its widgets and its
settings.

```c
tb_var fov = tb_var_float(gui, 90.0);
tb_var_bind_setting(fov, engine.graphics.field_of_view);

tb_widget slider = tb_scale(form, &(tb_scale_desc){.variable = fov, .from = 60, .to = 120,
                                                   .resolution = 1, .show_value = true});
tb_widget spin = tb_spinbox(form, &(tb_spinbox_desc){.variable = fov, .from = 60, .to = 120});
tb_var_trace(fov, on_fov_changed, game);
```

A variable is a handle, as a widget is: its GUI, a slot, and a
generation. It lives until `tb_var_destroy` or its GUI is destroyed, and a
handle to it then goes stale: every function given one does nothing, and a
widget that showed it shows nothing, as if it had none.

**Values.** Each variable reads as every type and is set from every type,
as Tk's read as text: `tb_var_get_bool`, `tb_var_get_int`, `tb_var_get_float`, and
`tb_var_get_text` read it; `tb_var_set_bool`, `tb_var_set_int`,
`tb_var_set_float`, and `tb_var_set_text` set it, converted to its type.

| Variable | Takes text | Takes a number | Reads as text |
| --- | --- | --- | --- |
| bool | `true`, `false`, `1`, `0`, `on`, `off`, `yes`, `no`, in any case | true for any but 0 | `1` or `0` |
| integer | decimal digits, with a sign | the nearest whole number, within 32 bits | its digits |
| number | a decimal number | the number, finite | the shortest digits that read back as it |
| text | the text | the shortest digits | the text |

A value that does not convert is refused: the setter returns false and the
variable keeps its value. Setting a variable to its value changes nothing.

**Observers.** `tb_var_trace` calls a function when a variable changes,
Tk's `trace_add`. Observers run during `tb_gui_update`, after the events
that changed the variable, once for each variable changed since they last
ran, however many times it changed: a change made outside an update runs
them in the next. An observer may set variables, whose observers then run
in the same update, and may remove itself or destroy its variable.

**Names.** `tb_var_set_name` names a variable, and `tb_gui_find_var` finds
it by its name, as layout files will. A widget's `variable` property is of
the type `TB_GUI_TYPE_VARIABLE`, set and read by name like any other: a
variable alive, or none.

### Variables bound to settings

`tb_var_bind_setting` binds a variable to a setting of the engine's
(`badger/settings.h`). The variable takes the setting's value at once; the
setting takes the variable's each time the variable is set, there and
then; and the variable takes the setting's each time it changes elsewhere
(a console, a file, another thread), at the start and the end of each
`tb_gui_update`, when its observers run. Values are set at the setting's
runtime layer, as an options menu's are, and saved with the player's
choices.

| Variable | Settings it binds to |
| --- | --- |
| bool | boolean |
| integer | integer; choice, by its index |
| number | number; integer, rounded to the nearest |
| text | text; choice, by its name; boolean, integer, and number, as a file writes them |

The setting checks each value as it checks any: its range, its choices,
its length. A bool, integer, or number variable refuses what its setting
refuses: the setter returns false, and the variable and the setting keep
their values, so a slider past a setting's range stops at its end. A text
variable keeps the text it is given, which an entry is typing, and its
setting takes the text only when it accepts it: `-` typed on its way to
`-0.5` stays in the entry, and `tb_var_setting_accepts` says whether the
setting holds the variable's value. `tb_var_bind_setting(var, NULL)` ends
the binding.

### What a change sends

`TB_GUI_EVENT_CHANGED` is sent to a widget when its value changes through
input: a check button toggled, text typed, a slider dragged, an item
chosen; and by `tb_widget_invoke`, which acts as input does. A program that
sets a widget's value, or its variable, sends none; observers see both. A
widget's `command` is called after its `CHANGED` bindings, with the same
event.

`TB_GUI_EVENT_VALIDATE` is sent before text changes, Tk's
`validatecommand`: the event's `text` is the text proposed, and
`tb_gui_refuse_edit` keeps the old. An entry's or a spin box's `validate`
command is called after its `VALIDATE` bindings.

`TB_GUI_EVENT_MOTION` is sent to the widget a button is held on as the
mouse moves, a slider's drag and a selection's, or else to the widget
under the mouse (see [popups](#popups)).

### The widgets

Each is a kind of its own, registered by its name, with its properties in
its table. Each takes the focus from Tab and is used from the keyboard;
those marked *click* take it from a click too.

| Kind | Properties | What |
| --- | --- | --- |
| `checkbutton` | `text`, `variable`, `on_value`, `off_value`, `selected`, `command` | a box ticked while its variable holds its on value; a click, Space, or invoking sets the other |
| `radiobutton` | `text`, `variable`, `value`, `selected`, `command` | one of a group on one variable, chosen while the variable holds its value; the arrows choose the one before or after among its siblings, and move the focus there |
| `entry` (*click*) | `text`, `variable`, `placeholder`, `show`, `width`, `max_length`, `read_only`, `validate` | a line of text typed |
| `text` (*click*) | `text`, `variable`, `width`, `height`, `wrap`, `max_length`, `read_only` | several lines, wrapped at its width or not, scrolled to keep the caret in sight and by the wheel |
| `spinbox` (*click*) | `value`, `variable`, `from`, `to`, `increment`, `format`, `width`, `wrap`, `command`, `validate` | a number stepped by its arrows, Up and Down, Page Up and Page Down by ten, and the wheel; or typed, and taken on Enter or when the focus leaves |
| `scale` (*click*) | `value`, `variable`, `from`, `to`, `resolution`, `big_increment`, `orient`, `length`, `show_value`, `format`, `command` | a slider: a press moves it there, a drag moves it; Left and Up step toward `from`, Right and Down toward `to`, by its resolution; Page Up and Page Down by its big increment; Home and End to the ends |
| `progressbar` | `value`, `variable`, `maximum`, `mode`, `orient`, `length` | a trough filled as far as its value is to its maximum; indeterminate, a block that moves to and fro by the time `tb_gui_update` is given; it takes no input |
| `combobox` (*click*) | `values`, `current`, `variable`, `width`, `height`, `command` | one of a list of values, chosen from a list that drops down over every widget |

**Values as text.** A check button's and a radio button's values are text,
as Tk's are, read in their variable's type: `"1"` and `"0"` fit bool and
integer variables alike, `"2.50"` holds a number variable of 2.5, and a
text variable holds them as they are. Without a variable, a check button
and a radio button keep their own state, `selected`.

**Numbers.** A spin box, a slider, and a progress bar keep their own
`value` without a variable, and with one, set it in its type: an integer
variable takes the nearest whole number. A spin box's and a slider's value
is kept within `from` and `to`; a slider's is rounded to multiples of its
`resolution` from `from` (0 for none). A `format` is a `printf` conversion
of a `double`, as Tk's: one conversion of `f`, `e`, or `g` with its flags,
width, and precision, and text around it (`"%.1f dB"`, `"%.0f%%"`); any
other is refused. With none, a spin box writes the digits of its
increment, and a slider those of its resolution.

**Editing text.** An entry, a text box, and a spin box's text edit alike,
with the editing the immediate interface's text field shares
(`src/ui/ui_editor.c`): typed text (`TB_GUI_EVENT_TEXT`) goes in at the
caret, in place of the selection; Left and Right move by characters, or by
words with Control; Home and End to the line's ends; Backspace and Delete
erase; Shift with each selects; Control with A, C, X, and V selects all,
copies, cuts, and pastes through the GUI's clipboard (`tb_gui_clipboard`,
which a game may connect to the system's). A click places the caret, a drag
selects, and a double click selects a word; Tab's focus selects a line's
whole text. A text box takes Enter as a line break; Up and Down move by
lines, Page Up and Page Down by its height, and Control with Home and End
to the text's ends. An entry leaves Enter, Escape, Up, and Down to its
ancestors, a dialog's default button and its cancel. `show` draws a mark
for each character, a password's, which copying and cutting then leave
alone; `max_length` counts characters; `read_only` text is selected and
copied, never changed. `tb_widget_selection` and `tb_widget_select` read
and place the caret and the selection by byte offsets.

**A list over every widget.** A combo box's list drops below it, or above
when it fits only there, as a [popup](#popups): a child of the combo box,
as wide as it, drawn above every widget and modal one. A press outside it
closes it, and is used by it, as do the combo box losing the focus, being
hidden, disabled, or destroyed. A click, Space, or Alt+Down opens the
list; there the mouse, Up and Down, Page Up and Page Down, Home and End
pick a value, and a click, Enter, or Space chooses it, and Escape or
Alt+Up closes it. Closed, Up and Down choose the value before or after,
and so does the wheel with the focus. A combo box's `values` are text, one
a line. A text variable holds the value chosen, an integer one its index.

**Sizes.** Every one measures from the theme's font at the view's scale:
entries, text boxes, spin boxes, and combo boxes in characters of "0" and
lines, check buttons by their text beside an indicator a line high,
sliders and progress bars by their `length` and a line's thickness, a
slider's value beside it as wide as its widest. So each is one size in
units at any scale, and its text is drawn at the display's own pixels.
## Containers and overlays

### Scrolling

A kind shows its children through a viewport by giving its class a
`viewport` function. Called at layout with its rectangle less its insets, it
returns what of its children is seen (the clip, in units of the GUI) and
the size of the space their grid is laid out in (the content). The grid is
laid out in the content, its top left at the clip's moved up and left by
the widget's scroll offset; the children are drawn, and found by the
mouse, only within the clip. A kind with a viewport is as large as it
measures, with its insets: its children's grid does not widen it, and
`tb_grid_request` tells its `measure` how large that grid wants to be.

| Function | What |
| --- | --- |
| `tb_widget_scroll_to` | scroll a widget's children to an offset, kept at the next layout from 0 to as far as the content goes past the clip |
| `tb_widget_scroll_offset` | the offset, as the last layout kept it |
| `tb_widget_see` | scroll each ancestor that scrolls as little as shows a widget, nearest first, Tk's `see`; the focus given to a widget does the same |
| `tb_grid_request` | the size a widget's grid wants, its insets not counted |

### Popups

A popup is a widget shown in front of every other widget, modal ones
included, at its natural size: menus, combobox lists, and tooltips.
`tb_widget_popup(widget, &options)` shows it by a rectangle, its anchor:
below it (`TB_GUI_POPUP_BELOW`, flipped above when there is no room),
right of it (`TB_GUI_POPUP_RIGHT`, flipped to the left), or at its top left
(`TB_GUI_POPUP_AT`, for the mouse's point). It is kept inside the GUI,
at least `min_size`, and clipped by nothing but the GUI. The widget stays
in the tree, but out of its parent's grid while it is a popup.

Popups stack, the last shown in front, each on a canvas layer above the
modal widgets'. While one that is not passive is shown:

- a press outside every popup closes them all, and is used by it: the
  widget under it is not pressed;
- a press inside one closes those in front of it, and goes on;
- keys go to the popup in front, unless the focus is inside it, then to its
  ancestors; Escape that nothing uses closes it, and Tab that nothing uses
  closes every popup and moves the focus.

A popup closed is sent `TB_GUI_EVENT_POPUP_CLOSE`; one hidden or
destroyed is closed without it, and one whose parent is hidden, disabled,
or destroyed is closed with it. A press in a popup leaves the keyboard's
focus where it is, unless it is on a field to type in. `tb_widget_close_popup` closes one and
those in front of it, `tb_gui_close_popups` every popup that is not
passive. A passive popup (`passive`, a tooltip) takes no input, and is
closed only by its owner. A popup's widgets take input with a modal widget
shown.

Kinds that follow the mouse inside them, such as lists, menus, and the
thumbs of scroll bars, are sent `TB_GUI_EVENT_MOTION` as it moves: the
widget that holds the mouse, or else the one under it.

### Scroll frames and scroll bars

| Kind | Properties | What |
| --- | --- | --- |
| `scrollframe` | `width`, `height`, `xscroll`, `yscroll`, `border`, `offset` | a frame whose grid is laid out at its natural size, or the frame's when that is larger, and seen through it; a width or height of 0 is the grid's, so a scroll frame stuck into a cell of weight scrolls what does not fit |
| `scrollbar` | `orient`, `first`, `last`, `command` | a bar alone, Tk's `scrollbar`: it shows the part of something seen, as fractions, and calls its command when it is dragged, paged, or turned by the wheel |

A scroll frame shows a bar along an axis it scrolls along only where its
grid is larger than its view, and each bar takes room from the other's
axis. The wheel scrolls it down, and across with Shift or a wheel that
turns sideways, three lines a notch; at its end the wheel goes on to the
ancestors, so a scroll frame inside another scrolls the outer one once it
can go no further. A bar's thumb is dragged, and a press on its track
pages a view toward it.

A scroll bar alone is tied to what it scrolls by its command, which reads
`tb_scrollbar_get` and scrolls; `tb_scrollbar_set` moves it without calling
the command, as Tk's `set` does.

### Lists

A `listbox` is Tk's: lines of text, chosen by the mouse and the keyboard.
Its items are data, not widgets: `tb_listbox_insert`, `tb_listbox_delete`,
`tb_listbox_get`, and `tb_listbox_size`, or the `items` property, the
items a line each. Only the rows that can be seen are drawn, those within
its own view and its ancestors' clips, so a list of many thousands costs
what its window does.

| `selectmode` | A click | The arrows |
| --- | --- | --- |
| `browse` | chooses one; dragging follows the mouse | choose the next |
| `single` | chooses one | choose the next |
| `multiple` | turns one on or off | move the active item; Space turns it |
| `extended` | chooses one; Shift a range from the anchor; Control turns one | choose the next; Shift extends; Control moves the active item |

Page Up and Down, Home, and End move as the arrows do; Control+A chooses
every item in the modes of many. Its `command` is called when the user
changes the selection, Tk's `<<ListboxSelect>>`; `tb_listbox_select` and
`tb_listbox_clear_selection` change it without. `height` is the rows it
shows, scrolling the rest with the wheel and its own bar; 0 shows every
row, for a list inside a scroll frame. `tb_listbox_see` scrolls the list,
and the frames around it, to an item, and `tb_listbox_nearest` finds the
item at a height.

### Menus

A `menu` is a column of items shown as a popup; a `menubar` is a row of
titles, each opening its menu below it. Items are data, added with
`tb_menu_add`: a command, a check item, a radio item of a group, a cascade
to another menu, or a separator; each with a label, text for its
accelerator at its right (the game binds the keys themselves), its
command, and whether it is disabled. A label's `&` marks its mnemonic, the
letter underlined that chooses it from the keyboard (`"&Open"`); `&&` is
an ampersand.

```c
tb_widget bar = tb_menubar(root);
tb_widget file = tb_menu(bar);
tb_widget recent = tb_menu(file);
tb_menu_add(file, &(tb_menu_item_desc){.label = "&Open", .accelerator = "Ctrl+O",
                                       .command = on_open, .user = editor});
tb_menu_add(file, &(tb_menu_item_desc){.kind = TB_MENU_ITEM_SEPARATOR});
tb_menu_add(file, &(tb_menu_item_desc){.kind = TB_MENU_ITEM_CASCADE, .label = "Open &recent",
                                       .submenu = recent});
tb_menu_add(file, &(tb_menu_item_desc){.kind = TB_MENU_ITEM_CHECK, .label = "Show &grid"});
tb_menubar_add(bar, "&File", file);
```

A menu is the child of what it opens from: its menu bar, the menu whose
cascade it is, or the widget it is a context menu of, so that the keys it
does not use go there. The mouse lights the item under it and a click
chooses it; over a cascade, its menu opens beside it. The arrows move
between the items that can be chosen, around the ends; Right opens a
cascade and Left closes it; Enter or Space chooses; a mnemonic's letter
chooses its item; Escape closes the menu in front. With a menu of a menu
bar open, the mouse over another title opens its menu, and Left and Right
from a menu go from title to title. Choosing an item closes every menu,
turns a check item, sets a radio item and clears the others of its group,
and then calls its command. A context menu is shown at a point with
`tb_menu_popup`, from a binding to a press of the right button.

### Tooltips

`tb_tooltip(widget, &(tb_tooltip_desc){.text = "Save the level"})` gives a
widget a tooltip: a child of it, never in its grid. Once the mouse has
rested on the widget, or on a widget in it without a tooltip of its own,
for the theme's `tooltip_delay` or the tooltip's own `delay`, it is shown
below the mouse as a passive popup. It hides when the mouse moves to
another widget, a button is pressed, or a key goes down, and shows again
only once the mouse has moved to another widget and back.

### Trees

A `treeview` is Tk's `ttk.Treeview`: items in a hierarchy, each with its
text in the tree's column and a value in each column of values, under
headings. Items are data with ids (`tb_tree_item`), never reused while the
tree lives; `TB_TREE_ROOT` is the root.

```c
tb_widget tree = tb_treeview(pane, &(tb_treeview_desc){.heading = "Name", .command = on_pick});
tb_treeview_add_column(tree, &(tb_treeview_column_desc){.heading = "Type", .width = 100});
tb_tree_item lights = tb_treeview_insert(tree, TB_TREE_ROOT, SIZE_MAX,
                                         &(tb_tree_item_desc){.text = "Lights", .open = true});
tb_treeview_insert(tree, lights, SIZE_MAX,
                   &(tb_tree_item_desc){.text = "Sun", .values = (const char *[]){"Light"},
                                        .value_count = 1});
```

`tb_treeview_parent`, `_first_child`, and `_next_sibling` walk the items;
`_text`, `_value`, and their setters read and change them; `_delete`
deletes an item with every item in it. The rows are the items whose
ancestors are open, kept in a list rebuilt when the tree changes, and only
those seen are drawn. An item with children shows a mark: a press on it,
or a double click on the row, opens or closes it, and the `open_command`
is called. The selection follows the list's modes (`selectmode`), and its
`command` is called when the user changes it. The arrows, Page Up and
Down, Home, and End move the focus item; Right opens an item or goes into
it, Left closes it or goes to its parent; Space chooses. A heading's right
edge is dragged to resize its column, down to its `minwidth`.
`tb_treeview_see` opens an item's ancestors and scrolls to it.

### Notebooks and paned windows

A `notebook` is a row of tabs over a page; each page is a child widget
added with `tb_notebook_add`, placed in the notebook's grid, and shown
only while its tab is chosen. A click on a tab chooses it; from a widget in
the notebook with the focus, Control+Tab and Control+Shift+Tab, or
Control+Page Down and Up, go around the tabs; with the focus on the
notebook itself, Left and Right do. Its `command` is called when the user
chooses another tab. A page destroyed takes its tab with it. Its natural
size is its chosen page's.

A `panedwindow` holds panes side by side (`TB_GUI_ORIENT_HORIZONTAL`) or
one above another, with a sash between each two, dragged to share their
space out (`tb_panedwindow_set_sash` does the same). It lays its panes out
with its own grid: each pane in a row or column of one uniform group,
weighted by its share, so that the grid keeps their sizes in proportion
as the window is resized, and never below the pane's `minsize`; each sash a
fixed row or column between them. A paned window is as large as it
measures (its `width` and `height`, or its panes' natural sizes summed), so
the panes' requests never widen it.

### Message dialogs

`tb_gui_message_box(gui, title, text, buttons, callback, user)` shows a
dialog after Tk's `messagebox`: a title in the heading role's family, a
message wrapped at 360 units, and a row of buttons of one width
(`TB_MESSAGE_OK`, `_OK_CANCEL`, `_YES_NO`, `_YES_NO_CANCEL`,
`_RETRY_CANCEL`). It is made of a frame, labels, and buttons in grids,
gridded over every cell of the root's grid, and shown modally in their
middle. The first button has the focus, so Enter or Space chooses it;
Escape chooses Cancel, No, or OK, the last of them the dialog has.
Choosing a button destroys the dialog, then calls the callback with the
answer.

### Images and canvases

An `image` widget draws a canvas image (`tb_image_create`) by its `fit`,
after CSS's `object-fit`: its own size (`none`), as large as it fits whole
(`contain`), as small as it covers the widget, cut off (`cover`),
stretched (`fill`), or as `contain` but never larger than itself
(`scale_down`); placed by its `anchor` and tinted by its `tint`. Its size
is its `width` and `height`, or the image's pixels as units.
`tb_gui_fit_rect` is the same placement, for kinds of a game's own.

A `canvas` widget's content is drawn by the caller: its callback is given
the paint functions each update, clipped to the widget and its ancestors,
and its input comes by bindings. It takes the focus on a click, so that a
viewport has the keys. `tb_gui_paint_image` draws an image from any kind.

## Styles

Every kind draws by a style: colors, sizes, and fonts looked up by name in
themes, after Tk's ttk styles and Godot's `Theme`. A theme (`tb_theme`)
holds style classes; a widget's style is resolved from its class, its
state, its own overrides, and the themes of the GUI and of its ancestors.
The same widgets take a flat tool's look or a game menu's textured one by
the theme they are given.

```c
tb_theme *const theme = tb_theme_create();
tb_theme_set_color(theme, "Primary.Button", "background", 0, gold);
tb_theme_set_color(theme, "Primary.Button", "background", TB_WIDGET_HOVERED, bright_gold);
tb_theme_set_font(theme, "Title.Label", "font", 0,
                  (tb_font_spec){.role = TB_FONT_ROLE_HEADING, .weight = TB_FONT_WEIGHT_REGULAR});
tb_gui_set_style_theme(gui, theme);

tb_widget_set_style(apply, "Primary.Button");
tb_widget_override_style(warning, "text_color", 0,
                         &(tb_style_value){TB_STYLE_COLOR, {.color = red}});
```

**What a style holds.** Every style has the standard values, which any
kind may draw by, and its kind's elements:

| Value | Type | What |
| --- | --- | --- |
| `background` | color | the fill behind it; transparent draws none |
| `background_image` | image | a [nine-slice](#nine-slice-backgrounds) panel drawn in place of the fill |
| `border_color`, `border_width` | color, number | its outline, inside its edges |
| `radius` | number | of its corners, its fill's and its outline's |
| `padding` | edges | between its border and its content |
| `font` | font | its text's font (see [fonts](#fonts-of-styles)) |
| `text_color` | color | its text |
| `focus_color`, `focus_width` | color, number | the ring drawn with the focus: set in the focused state |

A value no theme gives is zero: transparent, of no width, no padding, and
the GUI's own font. `tb_gui_paint_box` draws a style's box over a
rectangle, its background, its border, and its focus ring, each where it
has one; it is what most kinds draw first. The focus ring is no kind's own
drawing: it is the style of the focused state (`focus_width` set for
`TB_WIDGET_FOCUSED`), so a theme gives it, takes it away, or draws focus
as a lighter fill instead.

**Classes.** A widget's style class is its `style` property, or its
kind's: "Button", "Label", "Entry". A class falls back along its name, as
ttk's do: "Primary.Button", then "Button", then the root class ".", where
the values every kind shares are kept, such as the text's color. A widget
whose class does not end in its kind's, as "Primary" on a button, has its
kind's class added before the root.

**States.** Each value is set for a set of states, the flags of
`tb_widget_state` and `TB_WIDGET_SELECTED`, which kinds add for what is
chosen: a check button ticked, a list's row. None is the widget at rest.
For each value a widget takes the entry whose states it has every one of,
and of those the one whose states rank highest: disabled outranks
selected, selected pressed, pressed hovered, and hovered focused, so that a
state outranks any mix of those below it, as Godot's styles of a button do.
A button held under the mouse takes its entry for hovered and pressed, then
for pressed, then for hovered, then its entry at rest. Of entries of the
same states, the nearest wins: the widget's own overrides, then the nearest
theme, then the class nearest its own. An entry for more states in a
farther theme outranks one at rest in a nearer: a variation that changes a
button's color at rest keeps its base's look under the mouse, unless it
gives that state too.

**Themes in the tree.** A widget's themes are its own
(`tb_widget_set_style_theme`), then each ancestor's, then the GUI's
(`tb_gui_set_style_theme`, or `tb_gui_options::style_theme`), then the
GUI's default: the tool theme of its palette (`tb_gui_set_theme`), made
again when the palette is set. A value no nearer theme has comes from a
farther one, so a theme need give only what differs, and a panel of a
game's HUD can be drawn by a theme of its own. Themes and the images they
name are the caller's, borrowed while a GUI or a widget draws by them.

**Overrides.** `tb_widget_override_style` sets one value on one widget, by
state as a theme's are, before every theme's, as Godot's theme overrides
do: a warning's color, a wider button. `tb_widget_override_style(widget,
name, states, NULL)` removes one, and `tb_widget_clear_style_overrides`
all.

**Kept, not resolved each frame.** A widget's style is resolved the first
time it is asked for in a set of states (`tb_style_of`, `tb_style_in`),
and kept: drawing a widget each frame looks nothing up. Each update looks
at the themes in use; a theme changed (its generation moved), a theme set
on the GUI or a widget, a widget's class or overrides changed, or the
palette set resolves the styles again and lays the widgets out again, as
padding and fonts change sizes. Only the states a kind draws
(`tb_widget_style_desc::states`) are resolved: a label under the mouse
shares its style with a label at rest.

### What kinds draw

Each kind declares, as data in its class (`tb_widget_class::style`), its
style class, the states its look follows, and its elements: the values it
reads besides the standard ones, each by name and type. The editor and
theme authors read them to know what a kind can be given, and a game's
kinds declare their own the same way; a kind reads its elements by index
(`tb_style_color`, `tb_style_number`, `tb_style_edges`, `tb_style_font`,
`tb_style_image_of`) or by name (`tb_style_find`). An element of a type
other than its declared one is ignored.

| Kind | Class | Its box | Elements |
| --- | --- | --- | --- |
| `frame` | `Frame` | its background and border, when its properties ask; its padding added while it has a background | `shade` (the root's: behind modal widgets) |
| `label` | `Label` | around its text | |
| `button` | `Button` | itself | `default_color`, `default_width` (a dialog's default) |
| `checkbutton`, `radiobutton` | `Checkbutton`, `Radiobutton` | the indicator, selected while ticked or chosen | `mark_color`, `spacing` |
| `entry`, `text`, `spinbox` | `Entry`, `Text`, `Spinbox` | the field | `selection`, `placeholder_color`, `caret_color`; a spin box's `arrow_pressed_color`, `separator_color`, `mark_width` |
| `scale` | `Scale` | the handle, round | `trough_color`, `fill_color`, `spacing` |
| `progressbar` | `Progressbar` | the trough | `fill_color`, `fill_image` |
| `combobox`, its list | `Combobox`, `ComboboxList` | itself, drawn pressed while open; the list, its padding each row's | `mark_width`; `hot_color`, `selection`, `thumb_color` |
| `listbox`, `treeview` | `Listbox`, `Treeview` | itself, its padding each row's | `selection`, `hot_color`, `active_color`, the scroll bar's; a tree's `heading_background`, `heading_text_color`, `separator_color`, `indent`, `mark_width` |
| `scrollframe`, `scrollbar` | `Scrollframe`, `Scrollbar` | itself | `scrollbar_width`, `scrollbar_inset`, `scrollbar_trough`, `scrollbar_thumb`, `scrollbar_thumb_hot` |
| `menu`, `menubar` | `Menu`, `Menubar` | itself, its padding each item's | `selection`, `underline_width`; a menu's `accelerator_color`, `separator_color`, `mark_width`; a bar's `hot_color` |
| `tooltip` | `Tooltip` | itself | |
| `notebook` | `Notebook` | the page | `tab_background`, `tab_hot_background`, `tab_selected_background`, `tab_text_color`, `tab_selected_text_color`, `tab_line_color`, `tab_line_width`, `tab_padding` |
| `panedwindow` | `Panedwindow` | behind its panes | `sash_width`, `sash_color`, `sash_active_color` |
| `messagebox` | `Messagebox` | | `frame_padding`, `spacing`, `button_spacing`; its frame is a `Dialog.Frame`, its title a `Title.Label` |
| `image`, `canvas` | `Image`, `Canvas` | behind the image; behind the drawing, when it has a background | |

### Nine-slice backgrounds

A `background_image`, or an element of type image, is a nine-slice panel
(`tb_style_image`): a canvas image cut by four insets in texels, its
corners drawn at their size and its edges and middle stretched, drawn
`inset * scale` units wide so that it keeps its size at any GUI scale,
and tinted (see the [canvas](renderer.md)). `whole_pixels` draws each
texel of a border as a whole number of pixels, for pixel art; `source`
cuts a panel from part of an image, as a sheet of them; `hollow` leaves the
middle out. `tb_gui_paint_nine_slice` draws one from any kind's draw
function; each slice is an image command of the GUI's list, with its
texels in `tb_ui_command::source`.

### Fonts of styles

A style's `font` is a `tb_font_spec`: a role, a weight, whether italic, and
a size, opened from the GUI's catalog. A spec with a weight and no size is
its role's size. A weight of 0 is the GUI's own font, its palette's, at the
palette's size, or at the spec's size when it has one. A widget's font is
its style's at rest, in every state, so text measured is text drawn. A
font a kind or a widget asks for (a label's `font`) is the style's where it
gives nothing: a spec of weight 0 is the style's face, and a size of 0 its
size, so a title can ask for its size alone and take its family from its
theme. `tb_widget_measure_text` measures text in a widget's style;
`tb_gui_measure_text` in the root's. No built-in theme is bold.

### The built-in themes

| Theme | Made by | What |
| --- | --- | --- |
| dark tool | `tb_theme_create_tool(&tb_ui_theme_dark())` | flat fields, buttons, and lists in the palette's colors, sizes, and spacing: the GUI's default, as it looked before themes |
| light tool | `tb_theme_create_light()` | the same of `tb_ui_theme_light()`, its ticks white on the accent |
| game | `tb_theme_create_game(renderer, TB_THEME_GAME_FANTASY)` | beveled bronze buttons, slate panels, and sunk fields as nine-slice images it draws in code and owns; headings and buttons in the heading role's family (Source Sans 3), text in the body role's (Source Serif 4) |
| retro | `tb_theme_create_game(renderer, TB_THEME_GAME_RETRO)` | the same in pixel art, drawn by whole pixels, every text in the pixel role's font |

The tool theme of any palette is `tb_theme_create_tool(&palette)`. A game
theme's images are made on its renderer, so it is destroyed before it.
The sandbox's `gui` scene switches between all four as it runs
([sandbox](sandbox.md#the-gui-scene)).

### Theme files

A theme loads from a JSON file, its classes and values in place of the
theme's, all or nothing (`tb_theme_load`, or `tb_theme_load_file` through
a VFS). Comments and trailing commas are allowed, as in files people edit.

```json
{
  "format": "badger-theme",
  "version": 1,
  "name": "Bronze",
  "styles": {
    ".": {"text_color": "#eedfb8", "text_color:disabled": "#857867",
          "font": {"role": "body"}},
    "Button": {
      "background_image": {"image": "/Game/UI/button.png", "insets": 8},
      "background_image:hovered": {"image": "/Game/UI/button_hot.png", "insets": 8},
      "padding": [16, 7],
      "border_width": 0,
      "focus_width:focused": 2,
      "font": {"role": "heading"}
    },
    "Primary.Button": {"text_color": "#ffcc4d"}
  }
}
```

| Key | What |
| --- | --- |
| `format` | `"badger-theme"` |
| `version` | `1`, `TB_THEME_FILE_FORMAT_VERSION`; another is refused as `TB_THEME_FILE_VERSION` |
| `name` | optional text |
| `styles` | style classes by name, each an object of values |

A value's key is its name, then `:` and its states joined by `+`
(`hovered`, `pressed`, `focused`, `disabled`, `selected`); a name alone is
the widget at rest. A standard value takes its type; an element's type is
its shape:

| Type | Written |
| --- | --- |
| color | `"#rrggbb"` or `"#rrggbbaa"` |
| number | a number of units |
| edges | a number for every side, `[across, down]`, or `[left, top, right, bottom]` |
| font | `{"role": "ui" \| "body" \| "heading" \| "monospace" \| "pixel", "weight": 400, "italic": false, "size": 18}`; only the role is required |
| image | `{"image": path, "insets": texels, "source": [x, y, width, height], "scale": 1, "whole_pixels": false, "hollow": false, "tint": color}`; only the path is required |

An image is named by its VFS path, which the theme maps to a canvas image
the game made (`tb_theme_add_image`), so a file names no pixels and a
missing image is an error. Sizes that cannot be negative (`border_width`,
`radius`, `focus_width`, `padding`) are refused when they are.

**Errors.** Loading returns why it failed, and `tb_theme_file_error` says
where: a line and a column for text that is not JSON
(`TB_THEME_FILE_SYNTAX`), or a path in the document, as
`styles.Button.background:hovered`, for JSON that is not a theme
(`TB_THEME_FILE_INVALID`), with a message that names the file. On any
error the theme is as it was.

**Hot reload.** `tb_gui_watch_theme(gui, theme, vfs, path, &options)`
looks at a theme's file at an interval, half a second unless chosen, each
`tb_gui_update`, through the VFS, so that every platform and mount watch
alike; when its bytes change it is loaded again, and the GUI draws by it
from that update. A file that fails leaves the theme as it was, and the
callback says why; one that cannot be read for a moment, as while an
editor replaces it, is looked at again. A shipped game, whose files are in
packages, need not watch them. The layout files' loader reads JSON and
watches files the same way; the two are to share one reader and one watch
list once both are in.

## Drawing and ownership

`tb_gui_update` builds the frame's draw commands, `tb_ui_command`s in
units, on canvas layer `tb_gui_options::layer` (50), the modal widgets'
above it, and the popups' above those; `tb_gui_commands` returns them, for tests, and `tb_gui_draw`
hands them to a renderer's canvas. What changes after an update is drawn
after the next.

The caller owns the GUI, its theme's fonts, and the catalog, which outlive
it. The GUI owns its widgets, their text, its variables, and the fonts it
opened from the catalog; `tb_gui_forget_fonts` tells the renderer's canvas to forget those
before `tb_gui_destroy` closes them. A kind's table, and its properties'
names, outlive every widget of it.

## Threading

A GUI and its widgets are externally synchronized: one thread uses them at
a time, as with `ui`. `tb_gui_draw` and `tb_gui_forget_fonts` run on the
thread that renders. GUIs are independent of one another.

## Not done yet

- A combo box typed in (Tk's editable `ttk.Combobox`), and a spin box of
  a list of values; undo in text.
- Menus', lists', trees', and tabs' items in layout files, which have no
  type for a list yet, nor for an image or a draw callback; a menu bar
  opened by Alt and its titles' mnemonics.
- A theme saved to a file; theme files watched by the OS's change notices
  rather than by looking; a theme's images loaded from their paths by the
  engine, which has no image decoder at run time yet.
- Layout files in JSON, and finding widgets by path; the `name` property is
  kept for them.
- Focus moved by arrows and a gamepad.

## Tests

`tests/gui/` drives the GUI with window events made by hand, and kinds of
boxes of chosen sizes: handles that go stale and are never reused, the tree,
kinds registered and their properties of every type, the grid after Tk's own
tests (sizes, weights, anchors, minimum sizes and pads, uniform groups,
spans, sticky edges, padding, shrinking and clipping, edges on whole pixels
at scales from 1 to 2, nesting, and layout only when something changed), the
mouse entering and leaving, presses held, clicks and double clicks, clipping
and hidden and disabled widgets, buttons' commands, Tab's focus, keys and the
wheel given up the tree, modal widgets, cursors, bindings, and widgets
destroyed from inside callbacks; scroll frames laid out, clipped, found
through their offsets, and scrolled by the wheel, their bars, and `see`;
popups placed, closed by presses and Escape, and given keys; tooltips;
lists in each selection mode, and long ones drawing only what is seen;
menus by the mouse and the keyboard; trees by the mouse and the keyboard,
with headings resized; notebooks' tabs; paned windows' shares and sashes;
message dialogs answered every way; images fitted; canvases drawn by their
callbacks; the stretch modes and text measured
at the display's pixels; and the commands each widget draws, drawn on the null
backend's canvas. Variables are read and set across their types, their
observers run in updates, and they follow settings and are followed by
them, with the settings' checks; each kind that edits values is driven by
the mouse and the keys, its variable kept in step with its twin's, its
`CHANGED` events and commands counted, its properties set to what they
read, and its size measured at scales 1, 1.5, and 2. Styles are resolved
along their classes' chains, by the ranks of their states, from overrides
and the themes of subtrees, and resolved again only when what they come
from changes; theme files load every type of value, refuse what is not a
theme with where and why, refuse damaged bytes, and load again as they
change through a VFS; and nine-slice panels are cut on whole pixels at
scales 1, 1.5, and 2. The forms and containers are drawn on WARP and
matched with golden images in the default theme, and the forms in the
light, game, and retro themes (`tests/gui/gui_look_d3d12_test.c`). The
sandbox's `gui` scene shows a form by eye, and its `gui_tool` scene an
editor's shape ([sandbox](sandbox.md)), and the tests draw it on WARP at
two scales.
