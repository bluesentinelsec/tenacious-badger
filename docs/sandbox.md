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
| `triangle` | A triangle of three colored corners, turning, drawn through the RHI itself inside the renderer's scene pass |
| `textured` | A checkerboard made in memory, sampled through the texture's and sampler's bindless indices |
| `clear` | The swap chain cleared to a plain color |

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

Its **Frame** window shows the frame times of the last 120 frames as a
graph, their average and worst, the frames a second, the scene, the
backend and adapter, where skinned meshes are skinned and why (the GPU's
compute pass, or the CPU), with a switch that forces the CPU to compare the
two, what the canvas drew in the last frame, and the
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

