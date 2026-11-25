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
  ancestors; Escape that nothing uses closes it.

A popup closed is sent `TB_GUI_EVENT_POPUP_CLOSE`; one hidden or
destroyed is closed without it. `tb_widget_close_popup` closes one and
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

## Drawing and ownership

`tb_gui_update` builds the frame's draw commands, `tb_ui_command`s in
units, on canvas layer `tb_gui_options::layer` (50), the modal widgets'
above it, and the popups' above those; `tb_gui_commands` returns them, for tests, and `tb_gui_draw`
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
- Menus', lists', trees', and tabs' items in layout files, which have no
  type for a list yet, nor for an image or a draw callback; a menu bar
  opened by Alt and its titles' mnemonics.
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
destroyed from inside callbacks; scroll frames laid out, clipped, found
through their offsets, and scrolled by the wheel, their bars, and `see`;
popups placed, closed by presses and Escape, and given keys; tooltips;
lists in each selection mode, and long ones drawing only what is seen;
menus by the mouse and the keyboard; trees by the mouse and the keyboard,
with headings resized; notebooks' tabs; paned windows' shares and sashes;
message dialogs answered every way; images fitted; canvases drawn by their
callbacks; the stretch modes and text measured
at the display's pixels; and the commands each widget draws, drawn on the null
backend's canvas. The sandbox's `gui` scene shows a form by eye, and its
`gui_tool` scene an editor's shape
([sandbox](sandbox.md)), and the tests draw it on WARP at two scales.
