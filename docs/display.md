# The display policy

An app is made for a resolution: a game's menus are laid out at 1920 by
1080, a pixel-art game's sprites at 320 by 180. Windows come in every size
and shape, from a 16:10 laptop to a 21:9 monitor and a portrait tablet. The
display policy (`badger/viewport.h`) says how the app's **design
resolution** is placed on a window's frame, and gives one **viewport**: the
rectangle of the frame the app is drawn in, and the scale from the app's
units to pixels.

The viewport is the single source of truth for everything placed on the
frame. The 3D camera projects at its aspect and renders into its
rectangle ([the renderer](renderer.md#the-viewport)), the canvas draws in
its units ([the canvas](renderer.md#2d-the-canvas)), [the GUI](gui.md#units-and-scaling)
lays out in it, and the mouse is placed by it. The rest of the frame is
**bars** of one color, and a click on a bar reaches no widget.

The policy is modelled on Godot's stretch settings
(`display/window/stretch/*`); Unreal has no direct equivalent.

```c
/* each frame, from the settings an app's default file gives */
tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
tb_viewport_policy_from_settings(engine, &policy);
tb_renderer_set_viewport_policy(renderer, &policy, tb_viewport_aspect_ratio_from_settings(engine));

/* after tb_renderer_begin_frame() */
const tb_viewport viewport = tb_renderer_viewport(renderer);
tb_gui_set_viewport(gui, &viewport);
const tb_canvas_view design = tb_canvas_view_viewport(&viewport);
tb_canvas_set_view(renderer, &design); /* draw in the design's units */
```

## The policy

One policy per app, set by the engine's settings, which an app gives in its
default file (`src/burrow_defaults.cfg` for Burrow). Games and tools behave
the same by default; an app overrides the policy case by case.

| Setting | Choices | Default | What it is |
| --- | --- | --- | --- |
| `display.design_width`, `display.design_height` | 0 to 16384 | 1920 by 1080 | The design size, in units. 0 by 0 is none: a unit is then a logical pixel at the display's scale, and the viewport is the whole frame, as a tool wants |
| `display.stretch_aspect` | `keep`, `keep_width`, `keep_height`, `expand`, `ignore` | `keep` | What happens when the frame's shape is not the design's |
| `display.stretch_scale_mode` | `fractional`, `integer` | `fractional` | Any scale, or whole pixels a unit for pixel art |
| `display.stretch_render_mode` | `canvas`, `viewport` | `canvas` | At the frame's own pixels, or at the design size then upscaled |
| `display.stretch_scale` | 0.25 to 8 | 1 | A factor on top: 2 draws every unit twice as large, and shows half as many |
| `display.bar_color` | `#rrggbb` | `#000000` | The bars' color, as the frame shows it |

`tb_viewport_policy_from_settings()` reads them into a `tb_viewport_policy`;
`TB_VIEWPORT_POLICY_INIT` is the same defaults, and
`TB_VIEWPORT_POLICY_LOGICAL` is a policy without a design size.

### Aspects

| Aspect | The design | The viewport | Units shown |
| --- | --- | --- | --- |
| `keep` | Fits whole, as large as it fits | Centered, with bars above and below (letterbox) or at the sides (pillarbox) | The design's |
| `keep_width` | Its width fits | A taller frame shows more height; a wider one is pillarboxed | The design's width, and the height the frame holds |
| `keep_height` | Its height fits | A wider frame shows more width; a taller one is letterboxed | The design's height, and the width the frame holds |
| `expand` | Fits by the smaller axis | The whole frame | The design's, and more along the other axis |
| `ignore` | Stretched to the frame | The whole frame | The design's, each axis at its own scale |

Unit (0, 0) is the viewport's top left, so where more is shown than the
design, the extra is to the right and below. An app that wants its content
in the middle lays it out from the middle, as the GUI's grid does.

`ignore` is the one aspect that stretches: its scale differs across and
down. The 3D camera projects at the design's aspect and the picture is
stretched to the frame. The canvas's design view stretches shapes, but
draws text at the smaller of the two scales, unsquashed; the GUI lays out
at that smaller scale both ways, and its root fills the viewport, with more
units along the axis stretched more.

### Scale modes

`fractional` scales the design as large as the frame allows. `integer`
floors the pixels a unit to a whole number, at least 1, and places the
viewport on whole pixels: pixel art stays sharp and even, with bars where
the frame is not a whole multiple. A frame too small for the design at 1
pixel a unit shows the part of the design from its top left that fits.

Without a design size, the scale is the display's, times
`display.stretch_scale`; `integer` floors that too, so that a tool drawn in
pixel art keeps whole pixels at 150%.

### Render modes

`canvas` draws everything at the frame's own pixels through the viewport's
transform, so text and edges stay crisp at any scale. `viewport`, after
Godot's `stretch/mode = viewport`, draws what is in units into a
**target** first, a texture of a pixel a unit, then upscales the target
into the viewport, as a pixel-art game wants:

- **The target** is the units the viewport shows, rounded up: the design
  size in `keep` and `ignore`, and more along an axis that `expand`,
  `keep_width` or `keep_height` grows, as Godot's viewport grows. The
  policy's `scale` shows fewer units, so it makes the target smaller: a
  design of 320 by 180 at a scale of 2 is a target of 160 by 90, each of
  its pixels a unit twice as large. Without a design size, the target is
  the logical pixels, and the upscale is the display's scale. A side is at
  most 16384 pixels (`TB_VIEWPORT_MAX_TARGET`).
- **Into it** go the 3D scene, at the render scale of the target's pixels
  and with temporal anti-aliasing at its size, its debug lines, and what
  the canvas and the GUI draw in the design's units. Text is laid out,
  measured and rasterized at the target's pixels: a line of 8 units is
  8 pixels to the em, then upscaled, which is the point of the mode.
- **The upscale** is nearest-neighbour in the `integer` scale mode, where
  every unit is a whole block of pixels, and linear otherwise; a policy
  `scale` that is not whole makes it linear too. The bars around it are
  cleared to the bar color.
- **Over it**, at the frame's own pixels, go the overlays: what the canvas
  draws in a view of the frame (`tb_canvas_view_pixels()`,
  `tb_canvas_view_logical()`, `tb_canvas_view_fit()`) and the renderer's
  labels. The engine's inspector, consoles, statistics and the sandbox's
  status line stay crisp in either mode.

The mouse is placed by the viewport in either mode, so input is the same
in both.

The upscale reads the target through bindless resources. A device without
them draws the canvas mode, and says so once in its log.

## The viewport

`tb_viewport_compute()` gives the viewport of a policy on a frame: its
size in pixels, the display's scale, and the player's aspect ratio. It is
pure and thread-safe.

| Field | What it is |
| --- | --- |
| `x`, `y`, `width`, `height` | The rectangle, in whole pixels of the frame; at least 1 by 1 for a frame of some size |
| `scale` | Pixels a unit, across and down: the same both ways but in `ignore` |
| `size` | The units shown, across and down |
| `target_width`, `target_height` | In the viewport render mode, the target's pixels: `size` rounded up. 0 by 0 in the canvas mode |

`tb_viewport_to_pixels()` and `tb_viewport_from_pixels()` convert between
pixels and units, `tb_viewport_contains()` says whether a pixel is in the
rectangle and not on a bar, `tb_viewport_aspect_of()` is the aspect the
camera projects at, and `tb_viewport_uniform_scale()` the scale of what
must not stretch, such as text.

Kept axes are centered on whole pixels: the bars on either side differ by a
pixel at most, the odd one on the right or below. A kept axis shows the
design's units exactly, though its pixels are rounded to whole ones.

### The player's aspect ratio

`display.aspect_ratio` (`auto`, `16_9`, `16_10`, `21_9`, `32_9`, `4_3`) is
a player's choice, not the app's. When it is not `auto`, the frame is first
narrowed to the largest centered rectangle of that shape, and the policy
works within it: a player with a 21:9 monitor who wants 16:9 gets
pillarboxes, whatever the app's aspect. `21_9` is 64:27, the 2560 by 1080
of those displays. `tb_viewport_aspect_ratio_from_settings()` gives the
ratio, 0 for `auto`.

### Examples

A design of 1920 by 1080, kept, on a few frames:

| Frame | Viewport | Pixels a unit | Units |
| --- | --- | --- | --- |
| 1920 by 1080 | 1920 by 1080 at (0, 0) | 1 | 1920 by 1080 |
| 1920 by 1200 (16:10) | 1920 by 1080 at (0, 60) | 1 | 1920 by 1080 |
| 2560 by 1080 (21:9) | 1920 by 1080 at (320, 0) | 1 | 1920 by 1080 |
| 1024 by 768 (4:3) | 1024 by 576 at (0, 96) | 0.533 | 1920 by 1080 |
| 1080 by 1920 (portrait) | 1080 by 608 at (0, 656) | 0.5625 | 1920 by 1080 |

A design of 320 by 180, kept, at whole pixels: 1366 by 768 is 1280 by 720
at (43, 24), 4 pixels a unit; 1024 by 768 is 960 by 540 at (32, 114), 3.

## Who follows it

- **The renderer** holds the policy (`tb_renderer_set_viewport_policy()`)
  and computes the viewport each frame from the swap chain's size. The 3D
  scene renders into the rectangle, at the render scale of its pixels, and
  the camera projects at its aspect; the bars are cleared to the bar color.
  A renderer begins with `TB_VIEWPORT_POLICY_LOGICAL`, whose viewport is
  the whole frame, until an app gives it a policy. See
  [the renderer](renderer.md#the-viewport).
- **The canvas** draws in the design's units with
  `tb_canvas_view_viewport()`, clipped to the rectangle; in the viewport
  render mode, that view is of the target, a pixel a unit
  (`TB_CANVAS_TARGET_VIEWPORT`). Its other views, in pixels and logical
  units over the whole frame, stay for overlays, drawn after the upscale.
- **The GUI** takes the renderer's viewport each frame
  (`tb_gui_set_viewport()`), so the two always agree; its root is the
  viewport, its units the viewport's times its own interface scale, and a
  press on the bars reaches no widget. In the viewport render mode it is
  laid out and drawn in the target. See
  [the GUI](gui.md#units-and-scaling).
- **The window** is not resized or placed by the policy: its modes are the
  settings' ([windows](windows.md)), windowed in developer builds and
  borderless in release builds, and an app overrides them in its default
  file.

### What goes where

Everything an app shows its players goes in the viewport, in the design's
units: its scene, its HUD, its menus, its text. Then the interface keeps
its place and its share of the picture in every window mode, at every
window size, and on every display: windowed or fullscreen, it differs only
in scale and in the bars beside it.

The views of the whole frame, in pixels and logical units, are for the
engine's developer tools alone: the inspector, the console, statistics.
They do not follow the viewport. Over bars they lie partly on the bars,
and their share of the picture changes as the window's size does.

To draw in the viewport at the frame's own pixels, sharp in both render
modes, make a canvas view of the viewport's rectangle: its origin the
rectangle's corner, its clip the rectangle, and its scale the rectangle's
pixels over the units the overlay is laid out at. The `display` scene's
facts are drawn so.

## In the sandbox

The sandbox's [`display` scene](sandbox.md#the-display-scene) shows the
policy at work: a lit scene in the viewport, the design's border and grid,
pixel art and a line of text, a GUI panel, and what the viewport is. F1 to
F5 change the aspect, the scale mode, the render mode, the design size and
the window's mode; F6 locks resizing the window to the design's aspect,
and Alt+Enter switches between windowed and borderless. At 320 by 180,
F3 shows the render modes apart: the canvas mode draws the turning art and
the text smoothly at the window's pixels, and the viewport mode in whole
blocks of the target's, the panel's text included.

```
badger-sandbox --scene display
badger-sandbox --scene display --set display.stretch_aspect=expand
badger-sandbox --scene display --set display.design_width=320 --set display.design_height=180 --set display.stretch_scale_mode=integer
badger-sandbox --scene display --set display.design_width=320 --set display.design_height=180 --set display.stretch_scale_mode=integer --set display.stretch_render_mode=viewport
```

## Threading

The functions of `viewport.h` are thread-safe: pure arithmetic, but for
those that read settings, which are thread-safe as settings are. The
renderer's and the GUI's are stage-owned, as theirs are.
