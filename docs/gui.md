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
`tb_gui_register_class` registers a kind by its name, so that layout files
and the editor find it (`tb_gui_find_class`); the engine's are registered
when a GUI is made. A widget is made of any kind, registered or not.

A kind draws through a `tb_gui_paint`: rectangles filled and outlined,
lines, and text set in a rectangle by its anchor (`tb_gui_paint_text`), each
clipped to the widget and its ancestors. `tb_widget_state` says whether a
widget is hovered, pressed, focused, or disabled, to draw it so.

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
hand. The arrows, Enter, and Escape that no kind uses navigate (see
[navigation](#navigation)).

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
| `frame` | `padding`, `background`, `border` | a container of widgets in its grid; filled with the theme's window color it stops the mouse |
| `label` | `text`, `wrap_length`, `justify`, `anchor`, `font` | text that is read: wrapped at a length, its lines justified against one another, set in its cell by its anchor, in a font asked for by spec |
| `button` | `text`, `command`, `default` | text that is clicked to call its command; lit under the mouse, darker held, ringed by the accent with the focus, and ringed thinly as a dialog's default |

A label's or button's font is the theme's unless it asks for another by
`tb_font_spec`: the GUI opens it from its catalog (`tb_gui_options::catalog`)
once, keeps it until it is destroyed, and draws in the theme's font a spec
the catalog does not have. A spec of weight 0 is the theme's font, and a
size of 0 the theme's size. Text wider than its rectangle starts at the
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
| `checkbutton` | `text`, `variable`, `on_value`, `off_value`, `selected`, `command` | a box ticked while its variable holds its on value; a click, Space, Enter, or invoking sets the other |
| `radiobutton` | `text`, `variable`, `value`, `selected`, `command` | one of a group on one variable, chosen while the variable holds its value; the arrows choose the one before or after among its siblings, and move the focus there, and past the group's ends move the focus on |
| `entry` (*click*) | `text`, `variable`, `placeholder`, `show`, `width`, `max_length`, `read_only`, `validate` | a line of text typed |
| `text` (*click*) | `text`, `variable`, `width`, `height`, `wrap`, `max_length`, `read_only` | several lines, wrapped at its width or not, scrolled to keep the caret in sight and by the wheel |
| `spinbox` (*click*) | `value`, `variable`, `from`, `to`, `increment`, `format`, `width`, `wrap`, `command`, `validate` | a number stepped by its arrows, Up and Down, Page Up and Page Down by ten, and the wheel; or typed, and taken on Enter or when the focus leaves |
| `scale` (*click*) | `value`, `variable`, `from`, `to`, `resolution`, `big_increment`, `orient`, `length`, `show_value`, `format`, `command` | a slider: a press moves it there, a drag moves it; the arrows along it step, Left or Up toward `from`, Right or Down toward `to`, by its resolution, and those across it move the focus on; Page Up and Page Down by its big increment; Home and End to the ends |
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
hidden, disabled, or destroyed. A click, Space, Enter, or Alt+Down opens
the list; there the mouse, Up and Down, Page Up and Page Down, Home and End
pick a value, and a click, Enter, or Space chooses it, and Escape or
Alt+Up closes it. Closed, Up and Down choose the value before or after
while it is engaged, focused by a click or Tab; reached by navigation,
they move the focus on (see [engagement](#engagement)). The wheel chooses
with the focus. A combo box's `values` are text, one
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
Back, Escape or a gamepad's B, chooses Cancel, No, or OK, the last of them
the dialog has.
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

## Navigation

A game's menus are used with a gamepad alone or a keyboard alone, as
Godot's focus navigation and Unreal's UMG navigation are: the arrows and a
gamepad's d-pad move the focus from widget to widget, Accept activates the
one focused, and Back goes back.

```c
/* once: the screen's first widget, and its list going around */
tb_widget_set(play, "default_focus", &(tb_gui_value){TB_GUI_TYPE_BOOL, {.boolean = true}});
tb_widget_set(menu, "focus_wrap", &(tb_gui_value){TB_GUI_TYPE_FLAGS, {.flags = TB_GUI_WRAP_VERTICAL}});
tb_bind(options, TB_GUI_EVENT_BACK, on_back_to_main_menu, game);

/* for each gamepad button pressed, or a direction held, repeated */
tb_gui_feed_navigation(gui, TB_GUI_NAV_DOWN);
```

### Actions

What a player asks is an action, `tb_gui_nav`. The keys become actions by
the GUI's key map; a gamepad's are fed by the game with
`tb_gui_feed_navigation`, and handled in their order among the window
events fed, in the next update. The engine has no gamepad input yet, so a
game reads its gamepad itself and repeats a direction held, as a key
repeats.

| Action | Keys | A gamepad |
| --- | --- | --- |
| `TB_GUI_NAV_UP`, `_DOWN`, `_LEFT`, `_RIGHT` | the arrows | the d-pad, the left stick |
| `TB_GUI_NAV_ACCEPT` | Enter, Space | A, Cross |
| `TB_GUI_NAV_BACK` | Escape | B, Circle |
| `TB_GUI_NAV_NEXT`, `_PREVIOUS` | Tab, Shift+Tab | |
| `TB_GUI_NAV_TAB_NEXT`, `_TAB_PREVIOUS` | Control+Tab, Control+Shift+Tab, Control+Page Down and Up | the right and left shoulders |

The key map is data the game changes, for players who rebind their keys:
`tb_gui_map_key(gui, key, modifiers, action)` maps a key, with exactly
those modifiers held, in place of what it was mapped to, and
`TB_GUI_NAV_NONE` unmaps it; `tb_gui_key_action` reads it;
`tb_gui_clear_key_map` unmaps every key, and `tb_gui_reset_key_map` maps
them as a GUI starts.

An action is first the key it stands for: Up, Down, Left, Right, Enter,
Escape, Tab, Shift+Tab, Control+Tab, or Control+Shift+Tab. It is given to
the widget with the focus, and its ancestors, as a key is; so a slider
steps by a gamepad's d-pad as by the arrows, a button is pressed by A as
by Enter, and a key mapped to Accept is Enter to the kinds. What no kind
uses, the GUI does:

| Action | Done by the GUI |
| --- | --- |
| a direction | the focus moves that way (below) |
| Accept | the widget focused is engaged, when its kind engages and it is not |
| Back | the widget engaged is let go; or else the popup in front is closed; or else `TB_GUI_EVENT_BACK` is sent; or else the modal widget in front is hidden |
| next, previous | the focus moves as Tab moves it, before any kind sees the key |

`TB_GUI_EVENT_BACK` goes to the widget with the focus, or the modal widget
in front, or the root, then up the tree: a menu binds it to go to the
previous menu. A binding that handles an event accepts it with
`tb_gui_accept_event`, as Godot's `accept_event` and Tk's `break`: the
bindings after it, the widget's kind, and its ancestors are not given it,
and a key accepted so is not taken as an action. A message dialog answers
Back as it answers Escape.

With nothing focused, a direction or Accept gives the focus to the widget
marked `default_focus` in the modal widget in front, or in the GUI, and does
no more; when none is marked, it does nothing, so that a game's own keys
stay its own while its interface has no focus.

### Moving by direction

A direction moves the focus to the focused widget's **neighbour** on that
side, when it names one that takes the focus; or else to the nearest
widget that takes the focus that way, by geometry (`tb_gui_focus_direction`
does the same, now):

- **Candidates.** A widget is a candidate when it lies wholly past the
  focused widget's edge on that side, as Godot has it, within the popup or
  modal widget the focus is in, or the GUI. Hidden and disabled widgets
  are not, nor widgets behind a modal widget; widgets scrolled out of a
  scroll frame's view are, and it scrolls to show them.
- **In line first.** A candidate in line with the focused widget,
  overlapping it across the direction, wins over any that is not, as
  Unreal's swept band does: Down from a narrow button goes to the wide one
  under it, not to a nearer one off to its side.
- **The nearest.** Among those, the nearest wins, by the shortest distance
  between the two rectangles, as Godot measures it; a tie goes to the first
  in the order of the tree.
- **Containers.** A widget with widgets shown inside it, as a notebook has
  its page, is placed by its largest part beside them, its tabs: Down from
  the tabs goes into the page, and Up from the page's top comes back to
  them.

**Neighbours.** `tb_widget_set_focus_neighbor(widget, side, neighbor)` names
the widget the focus goes to from one, on a side, as Godot's
`focus_neighbor_*` and Unreal's explicit navigation: Up, Down, Left, Right,
or Next and Previous, which Tab and Shift+Tab follow. A neighbour hidden,
disabled, or destroyed is passed over, and the geometry decides.

**Wrapping.** A container's `focus_wrap` flags, `horizontal` and
`vertical`, keep the focus inside it along an axis: when the nearest
ancestor of the focused widget that wraps along a direction's axis has no
candidate inside it that way, the search goes around, from its far edge,
among its own widgets, as Unreal's `Wrap` rule. A list of a menu wraps so
that Down from its last item comes to its first.

| Property | Type | What |
| --- | --- | --- |
| `default_focus` | bool | the widget a screen or a modal widget gives the focus first |
| `focus_wrap` | flags | `horizontal`, `vertical`: the axes along which navigation goes around inside it |

### The default focus, and the focus given back

`tb_widget_focus_default(container)` gives the focus to a screen's first
widget: the first, in the order of the tree, marked `default_focus`, or
else the first that takes the focus. A game calls it when it shows a menu.
A modal widget shown gives the focus to its widget marked so, when it has
one; it traps the focus, as every input, inside it, so the arrows and Tab
go around its own widgets only.

When a modal widget or a popup closes, ended, hidden, or destroyed, with
the focus in it or nowhere, the focus goes back to the widget that had it
when it was shown, if that widget still takes it. A modal widget ended
gives it back at once; one destroyed, at the end of the event.

### Engagement

Some kinds use the arrows themselves: a field for its caret, a list for its
items. The widget with the focus is **engaged** when it has the keys of
navigation to itself (`tb_widget_engaged`, and the state
`TB_WIDGET_ENGAGED`). Focus given by the mouse, by Tab, or by
`tb_widget_focus` engages it, as a desktop's controls are used; focus moved
by a direction, or given to a default, does not, as a game's menu is
walked. A kind of `TB_WIDGET_CLASS_ENGAGES` is given none of the keys
mapped to actions while it is not engaged: they move the focus on. Accept
engages it, and so does any other key it uses, or text typed; Back lets
it go, and the focus stays. `tb_widget_engage` engages the widget focused,
or lets it go, by hand.

| Kind | Not engaged | Engaged |
| --- | --- | --- |
| `entry` | the arrows move the focus; Accept or typing engages | Left and Right move the caret; Up and Down move the focus; Back lets go |
| `text` | the arrows move the focus; Accept or typing engages | every arrow moves the caret, and Enter is a line break; Back lets go |
| `spinbox` | the arrows move the focus; Accept or typing engages | Up and Down step, Left and Right move the caret; Back lets go |
| `listbox`, `treeview` | the arrows move the focus; Accept engages | the arrows move among the items; Enter and Space choose; Back lets go |
| `combobox` | the arrows move the focus; Accept opens its list | Up and Down choose the value before or after; open, its list has every key: Accept chooses, Back closes |
| `scale` | the arrows along it step its value; those across it move the focus | the same |
| `radiobutton` | the arrows choose the one before or after in its group, and move the focus there; past its ends they move the focus on | the same |
| `button`, `checkbutton` | Accept presses or toggles; the arrows move the focus | the same |
| `notebook` | on its tabs, Left and Right go from tab to tab; Up and Down move the focus; from anywhere in it, the tab actions go around the tabs, and the focus comes to the tabs | the same |
| a menu, a combo box's list | a popup in front has every key: the arrows move among its items, Accept chooses, Back closes it | |

### The focus shown

The focus is shown once the keyboard or a gamepad navigates, and hidden
once a mouse button is pressed, as CSS's `:focus-visible`:
`tb_gui_focus_visible` says which, and `tb_gui_set_focus_visible` sets it
until the next input. A kind draws its focus ring by the state
`TB_WIDGET_FOCUS_VISIBLE`, not `TB_WIDGET_FOCUSED`, so a button clicked is
not ringed; fields show their caret by `TB_WIDGET_FOCUSED` either way.

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
- Style classes and theme files; the `style` property is kept for them.
- Layout files in JSON, and finding widgets by path; the `name` property is
  kept for them; focus neighbours named by path in them.
- Gamepads read by the engine and fed to navigation; until then a game
  feeds their actions itself.

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
callbacks; navigation by the arrows and by actions fed, in grids regular
and not, with neighbours, wrapping, each kind's engagement, Accept and
Back, modal widgets that trap the focus and give it back, scroll frames
that follow it, the focus shown and hidden, and keys rebound; the stretch
modes and text measured
at the display's pixels; and the commands each widget draws, drawn on the null
backend's canvas. Variables are read and set across their types, their
observers run in updates, and they follow settings and are followed by
them, with the settings' checks; each kind that edits values is driven by
the mouse and the keys, its variable kept in step with its twin's, its
`CHANGED` events and commands counted, its properties set to what they
read, and its size measured at scales 1, 1.5, and 2. The sandbox's `gui` scene shows a form by eye, and its
`gui_tool` scene an editor's shape
([sandbox](sandbox.md)), and the tests draw it on WARP at two scales.
