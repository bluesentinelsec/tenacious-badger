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
| `tb_gui_wants_keyboard` | a widget has the focus |
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

## Drawing and ownership

`tb_gui_update` builds the frame's draw commands, `tb_ui_command`s in
units, on canvas layer `tb_gui_options::layer` (50) and the modal widgets'
above it; `tb_gui_commands` returns them, for tests, and `tb_gui_draw`
hands them to a renderer's canvas. What changes after an update is drawn
after the next.

The caller owns the GUI, its theme's fonts, and the catalog, which outlive
it. The GUI owns its widgets, their text, and the fonts it opened from the
catalog; `tb_gui_forget_fonts` tells the renderer's canvas to forget those
before `tb_gui_destroy` closes them. A kind's table, and its properties'
names, outlive every widget of it.

## Threading

A GUI and its widgets are externally synchronized: one thread uses them at
a time, as with `ui`. `tb_gui_draw` and `tb_gui_forget_fonts` run on the
thread that renders. GUIs are independent of one another.

## Not done yet

- Variables (`tb_var`) and the widgets that edit values: check buttons,
  radio buttons, entries, sliders, and the rest.
- Containers and overlays: scrolling, lists, tabs, menus, tooltips.
- Style classes and theme files; the `style` property is kept for them.
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
destroyed from inside callbacks; the stretch modes and text measured at the
display's pixels; and the commands each widget draws, drawn on the null
backend's canvas. The sandbox's `gui` scene shows a form by eye
([sandbox](sandbox.md)), and the tests draw it on WARP at two scales.
