# The sandbox

`badger-sandbox` draws the engine's test scenes in a window, to look by eye
at what the tests check by numbers: whether a triangle turns smoothly, how a
window resizes, what validation says. It is a developer tool, built with the
engine (`TB_BUILD_SANDBOX`, on when the engine is the top-level project) and
never shipped with a game.

```
badger-sandbox                       # the shapes, on the platform's best renderer
badger-sandbox --scene triangle      # start with another scene
badger-sandbox list                  # the scenes
badger-sandbox --software            # WARP: the same pixels on every Windows machine
badger-sandbox --validation          # the graphics API's validation, into the log
badger-sandbox --set display.vsync=off
badger-sandbox --renderer null       # no GPU at all
badger-sandbox --frames 120 --capture shapes.bmp   # stop after 120 frames, saving the last
badger-sandbox --scene ui --ui-scale 150             # the interface half as large again
badger-sandbox --scene gui --ui-scale 200            # the GUI twice as large, laid out again
badger-sandbox --scene display                       # the display policy, changed by F1 to F6
badger-sandbox --inspector                           # start with the inspector shown
badger-sandbox --scene skinning --cpu-skinning       # skin on the CPU, as a weaker device does
```

While it runs, keys 1 to 9 choose the first nine scenes, Tab moves to the
next, and Escape quits. Holding the right mouse button flies the camera, as in Unreal's
viewport: the mouse looks, W A S D move, Q and E go down and up, Shift goes
faster, and the wheel changes the speed. The frame time is drawn in the
bottom left corner of every scene, and printed every two seconds.

| Scene | What it shows |
| --- | --- |
| `shapes` | Every shape the renderer generates, lit and turning on a floor, with debug boxes, a grid, axes and an arrow |
| `canvas` | The 2D layer over the shapes: rounded panels, circles, rectangles, lines of any thickness, images made in memory, and a turning sprite, in logical units |
| `materials` | The shapes with materials of textures made in memory (a checkerboard, a gradient, noise), mipmapped on the GPU, on a checkered floor that shows the filtering toward the horizon; behind them, the same with a normal map of beveled tiles, every other one metal |
| `lighting` | [The raster lighting mode](renderer.md#lighting): a row of plastic spheres and a row of gold ones, from smooth to rough, and textured shapes, on a checkered floor, lit by a sun, three colored point lights that circle them and a spot light that sweeps across them, under a procedural sky; a window turns each light, their movement and the sky on and off |
| `sky` | The shapes under a procedural sky: a gradient from the horizon to the zenith, a ground, and a sun that circles once a minute |
| `sky_cube` | The shapes under a cube-map sky made in memory: a dusk, with lines every 15 degrees of latitude and longitude to show its seams and orientation |
| `text` | Text over the shapes in the font roles at their own sizes: a heading, a line of monospace, the UI font at sizes from 10 to 48, a paragraph of body text wrapping in three panels (left, centered and right) whose width changes over time, the pixel font at its size and twice it, and labels in the world naming the shapes |
| `skinning` | A row of tentacles, each a tapered cylinder skinned to a chain of seven joints, played by two clips made in code, a wave that runs up the tentacle and a curl toward the viewer, blended by a weight that drifts at each tentacle's own phase (labeled under it); evaluated on the job system, skinned by the GPU's compute pass (or on the CPU, with `--cpu-skinning` or the inspector's switch), the middle one with a checkered material, and their bones drawn over them |
| `ui` | [The interface](ui.md) over the shapes: an inspector of every widget (headers, checkboxes, combos, sliders, dragged numbers, a color, text fields, radio buttons, a swatch, a progress bar, buttons with tooltips, and a tree) and a window of statistics (a heading, wrapped text, a table, and a panel that scrolls) |
| `gui` | [The GUI](gui.md) over the shapes: a form laid out by grid that follows the window as it is resized (a title in the heading font, a wrapped paragraph, fields with their labels, rows of buttons of one width in nested grids, a row of radio buttons that switches the theme, a status line at the bottom) and a modal dialog that asks to confirm; and a panel of options of every widget that edits a value, bound to the engine's settings; and, below it, a panel built from a [layout file](gui.md#layout-files), `tools/sandbox/resources/gui/panel.json`, built again whenever it is saved |
| `gui_tool` | [The GUI](gui.md) in the shape of an editor: a menu bar (cascades, check and radio items, accelerators' text, mnemonics) over a paned window of an outliner, a tree view with a column of types, and a notebook of an asset list in a scroll frame with a context menu, a page of details with tooltips, and a viewport drawn by a canvas callback beside an image; message dialogs confirm quitting and deleting |
| `gui_fonts` | [The GUI](gui.md) as a small tool built from code alone, in the dark tool theme: a font browser over the [font catalog](fonts.md), with a tree view of its families and their styles and weights, an entry of sample text, a preview of the chosen font at the size of each of the catalog's roles drawn by a canvas widget in the font itself, and the font's facts (family, style, weight, italic, category, license, pixel size, file) in a grid, beside the tree in a paned window |
| `triangle` | A triangle of three colored corners, turning, drawn through the RHI itself inside the renderer's scene pass |
| `textured` | A checkerboard made in memory, sampled through the texture's and sampler's bindless indices |
| `clear` | The swap chain cleared to a plain color |
| `display` | [The display policy](display.md): the shapes, lit under a sky, in the viewport; the design's border, its corners and middle, the edge of what the viewport shows, a grid, pixel art still and turning, and a line of text, in the design's units; a GUI panel of a button for each hotkey; and what the window, the frame, the viewport, its target, the mouse and the policy are. F1 to F6 change the aspect, the scale mode, the render mode, the design size, the window's mode and whether resizing keeps the design's aspect; Alt+Enter switches windowed and borderless |

Every scene takes the [display policy](display.md) from the settings, as a
game does: by default a design of 1920 by 1080 kept whole, so the 3D scenes
are letterboxed in a window that is not 16:9, and `--set
display.stretch_aspect=expand` fills it. The `gui`, `gui_tool` and
`gui_fonts` scenes are tools, and override the design size to none, as a
tool would in its default file: their units are logical pixels over the
whole window. The inspector, the `ui` scene and the frame time are
overlays in logical pixels over the whole window, bars included.

The sandbox draws through [the renderer](renderer.md); the `triangle` and
`textured` scenes record their own RHI commands into its scene pass with
`tb_renderer_draw_custom()`. It draws text in the engine's own fonts, from
the font catalog: each role's font, at the role's size
([fonts](fonts.md#roles)); a build that carries none of them draws no
text.
Labels are drawn in the renderer's default debug font, the UI role's.

Shaders are HLSL in the sandbox's sources, compiled when a scene starts by
the engine's compiler, through DXC to DXIL for D3D12. The window, settings
and frame pacing are the engine's own: the sandbox runs on the game loop with
the engine's settings, in developer mode.

`--frames` with `--capture` makes any scene a check in CI: the tests run the
shapes on WARP and save their last frame.

## The inspector

F1, or the backquote, shows and hides the inspector over any scene
(`--inspector` starts with it shown). Its **Settings** window lists every
engine setting, a header for each family, each changed by the widget its
type wants: a checkbox for a switch, a slider within its range for a number
or a whole number of a narrow range, a number dragged for a wide one, a
combo for a choice, and a text field for text. A change is made at the
runtime layer and applied at once: the game loop changes the window (its
mode, size, and display), and the sandbox gives the renderer the vertical
sync and anisotropic filtering and the camera its field of view. Graphics
begins with the overall quality, which sets every quality group, as an
options menu does (`tb_engine_settings_set_quality()`), and reads custom
when they differ. Each family can be restored to its defaults. Settings
the device is made with, such as the renderer, take effect at the next
start. A setting's description is its tooltip.

The sandbox gives the renderer the quality the graphics settings
describe (`tb_renderer_quality_from_settings()`): the upscaler's mode and
the render scale, anti-aliasing, and the light and reflection quality.

Its **Frame** window shows the frame times of the last 120 frames as a
graph, their average and worst, the frames a second, the scene, the
backend and adapter, the renderer's profile (and why it is low), its render
size and the quality it draws with, where skinned meshes are skinned and why
(the GPU's compute pass, or the CPU), with a switch that forces the CPU to
compare the two, what the canvas drew in the last frame, and the
render graph's description of its passes and barriers
(`tb_render_graph_dump()`), read again every half second. GPU timings wait
for timestamp queries in the RHI.

The inspector takes the mouse over its windows and the keys while it has
the focus, before the scene and the fly camera.

## The ui scene

In the `ui` scene the interface sees the window's events first: it takes
the mouse over its windows, and the keys while a text field or Tab has its
focus, and the fly camera has the rest. Tab moves the interface's focus
there, rather than to the next scene. `--ui-scale` draws the interface at
a share of the display's scale, to look at it larger or smaller; it
zooms the inspector too.

## The gui scene

In the `gui` scene the GUI sees the window's events first, as the
interface does in the `ui` scene: it takes the mouse over the form, the
keys while a widget has the focus, Tab, and the arrows, and the fly camera
has the rest. The scene is used from the keyboard alone: an arrow focuses
the Rename button, the arrows move the focus by
[navigation](gui.md#navigation), ringed while the keyboard moves it, Enter
or Space presses, and Escape goes back, out of a field or the dialog. Resizing the window lays the form out again: its middle column takes
the extra width, and an empty row the extra height. `--ui-scale` is the
GUI's factor on top of the display's scale.

The options panel on the right edits the engine's own settings through
variables: anti-aliasing in a combo box, the field of view in a slider and
a spin box on one variable, the frame limit in a spin box, the master
volume in a slider and a progress bar, subtitles in radio buttons, and the
crosshair and the mouse's inverted look in check buttons. The sandbox
applies them as they change, and the inspector (F1) shows them move. Below
them, an entry that refuses digits, a password, a text box, and a progress
bar that only says work goes on edit variables of their own.

The form's **Theme** row switches the GUI's [style theme](gui.md#styles) as
it runs: the dark tool theme, which is the default; the light one; the
sample game theme of beveled nine-slice panels and buttons, in the heading
and body roles' fonts; and its retro look, in pixel art drawn by whole
pixels in the pixel role's font. The title asks only for its size, so its
family is its theme's.

The `gui_tool` scene covers the window with the GUI, which takes every key
but Escape, unless a menu, a dialog, or a widget with the focus takes
Escape too. Drag the sash between the outliner and the notebook, and a
heading's right edge to widen its column.

The `gui_fonts` scene covers the window as `gui_tool` does. It is made in
C alone, with no layout file, as a tool is written with Tkinter: a paned
window, a tree view filled from the catalog, an entry on a text variable,
a canvas widget whose callback draws the sample at each role's size in the
chosen font (`tb_gui_text_block::face`), and a grid of labels set when the
tree's selection changes. It starts on the UI role's font.

## The display scene

The `display` scene shows the [display policy](display.md) and the
viewport it gives the window, to try a policy on windows of every size and
shape by resizing the window, or by its keys:

```
badger-sandbox --scene display
```

| Key | Changes |
| --- | --- |
| F1 | The aspect: `keep`, `keep_width`, `keep_height`, `expand`, `ignore` |
| F2 | The scale mode: `fractional` or `integer` |
| F3 | The render mode: `canvas`, at the window's own pixels, or `viewport`, into a target of a pixel a unit, upscaled |
| F4 | The design size: 1920 by 1080, 1280 by 720, 640 by 360, 320 by 180 |
| F5 | The window's mode: windowed, borderless, fullscreen |
| F6 | Resizing the window kept at the design's aspect (`display.window_aspect_width` and `_height`, 16 by 9 for 1920 by 1080), or free |
| Alt+Enter | Windowed or borderless, as in every app on the game loop (Control+Command+F on a Mac) |

Each changes its setting at the runtime layer, as the inspector does, and
the inspector shows it change; in this scene the backquote alone shows the
inspector, since the scene takes F1 to F6 first. The panel's buttons do
what the keys do, and a press on the bars beside the viewport reaches none
of them.

The shapes are drawn in the viewport, lit by a sun under a procedural sky.
Over them, in the design's units: the design's border, its corners and its
middle in gold, the edge of what the viewport shows in blue (the same as
the border in `keep`; larger in `expand`), and a grid of nine cells a
design's height, centered; and a badger's face in pixel art, still and
turning, over a line of text, as large against any design as at 320 by
180, where a texel of the art is a unit. In the top left corner of the
viewport, laid out at 1920 by 1080 of its own and drawn at the window's
pixels, so that it keeps its place in every window mode: the window's logical
size, the frame's pixels and the display's scale; the viewport's
rectangle, its pixels a unit and the units it shows; the mouse in pixels
and in units, and whether it is in the viewport or on a bar; the policy,
with the player's aspect ratio; and the viewport render mode's target.

The render modes are told apart at a small design, such as 320 by 180 in
the `integer` scale mode:

```
badger-sandbox --scene display --set display.design_width=320 --set display.design_height=180 --set display.stretch_scale_mode=integer --set display.stretch_render_mode=viewport
```

In the canvas mode, the turning art's texels turn smoothly, the text and
the debug lines are drawn at the window's pixels, and the panel is crisp.
In the viewport mode, everything in the design's units, the 3D scene
included, is drawn at 320 by 180 and upscaled: the turning art stays on the
target's pixels, the text is a few pixels to the em, and the panel, laid
out at 960 by 540 units' worth, is too small to read, as a game's
interface not made for its design size would be. The facts in the corner
and the frame time stay crisp in both, as overlays.
