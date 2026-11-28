# Burrow

`burrow` is the engine's showcase game: a graybox first-person shooter,
under construction. For now it is a room to fly through. It opens a window
as [the display settings](settings.md#burrow) say, makes [a renderer](renderer.md)
on it, shows its main menu over the room, and runs [the game loop](game-loop.md),
which moves the player at fixed ticks and draws the room between them.

```
burrow                                  # play, on the platform's best renderer
burrow --renderer=d3d12                 # on a renderer named outright
burrow --renderer=null                  # in a window, drawing nothing: for tests
burrow --frames=120                     # stop after 120 frames
burrow --navigate=down,accept           # walk the menus, one action a frame: for tests
burrow --headless --frames=30           # no window: the loop and the player alone
burrow --log-file=burrow.log            # the output in a file, as under a launcher or Proton
burrow settings                         # every setting, and where its value comes from
burrow check                            # say that it starts against its engine
```

## Playing

| Input | Does |
| --- | --- |
| Mouse | Looks around, while the mouse is captured |
| W A S D | Fly forward, back, left and right, level whatever the view's pitch |
| Space | Rise |
| Ctrl or C | Sink |
| Shift | Sprint: two and a half times as fast |
| Escape | Pauses: the [pause menu](#the-menus) |
| Left click | Captures the mouse again, should the system have let it go |

Play captures the mouse, in relative mouse mode
(`tb_window_set_relative_mouse()`): the cursor is hidden and held in the
window, and the view turns by the mouse's own motion. The view looks up and
down at most 89 degrees, short of straight up, where its yaw would lose its
meaning. `controls.mouse.sensitivity` scales how far it turns, and
`controls.mouse.invert_y` turns it the other way up and down.

The player flies through everything, as a noclip camera does: there is no
collision yet (#313). Forward and sideways moves are level, relative to
where the player faces, so looking down does not fly into the floor; rising
and sinking are straight up and down. Moving diagonally is no faster than
moving straight.

## The menus

Burrow's menus are built with the engine's [retained GUI](gui.md) from
[layout files](gui.md#layout-files) in Burrow's content, and drawn in the
[game theme](gui.md#the-built-in-themes): bronze buttons and slate panels
of nine-slice images, headings in Source Sans 3 and text in Source Serif 4.
They are laid out at 1920 by 1080 units in the viewport of Burrow's
[display policy](display.md), which its default file sets at 1920 by
1080, expanded: a window of another shape shows more units around the
design, as it shows more of the room behind the menus, and no bars. The
pause and options menus are centered in it by their grids' weights; the
main menu keeps its margin from the window's left edge. Whatever the
policy's design size,
at the default interface scale the menus show 1920 by 1080 units. The
Accessibility tab applies `interface.scale` to the whole menu and
`accessibility.text_size` to the text used for layout and drawing. Menus
scroll when their controls exceed the viewport, including on keyboard or
gamepad focus changes.

| Screen | Shows | Back |
| --- | --- | --- |
| Main menu | Burrow's title over the room, which a camera circles; Play, Options, and Quit | asks whether to quit |
| Pause menu | Escape while playing, or the window losing the focus as `gameplay.pause_when_unfocused` says, over the room frozen and shaded; Resume, Options, Quit to menu, and Quit | resumes |
| Options | A notebook with Display, Graphics, Controls, Audio, and Accessibility tabs; Defaults, Revert, Apply, and Back | goes back to the menu it was opened from |

The game starts at the main menu, with the world paused and the mouse the
menus'. Play captures the mouse and starts the world; a menu over it
releases the mouse and pauses the world (`tb_game_loop_set_paused()`);
Quit to menu ends the game under way, and the next Play starts afresh.
Quit asks first, in a message dialog, unless
`developer.quit_without_asking` says not to, as it does in developer
builds; Back from the main menu always asks.

**Navigation.** Every screen is used with the keyboard alone, or the
actions a gamepad gives ([navigation](gui.md#navigation)): each screen
gives its first widget the focus as it shows, the arrows move it, Enter
or Space presses, Escape goes back, and Control+Tab or a gamepad's
shoulders turn the options' tabs. The mouse works as well. Burrow reads no
gamepad yet; `burrow_menus_navigate()` takes the actions it will give.

**The options.** Each option edits an engine setting
([settings](settings.md#the-engines-settings)) through a variable bound to
it deferred (`tb_var_bind_setting_deferred()`): the widget shows the
setting's value, and a change waits until it is applied, as players
expect of an options menu.

| Button | Does |
| --- | --- |
| Apply | The settings take every change, at their runtime layer, and the player's settings file is saved |
| Revert | Every change is let go of: the widgets show the settings' values again |
| Defaults | Every option shows its default, Burrow's own (`burrow_defaults.cfg`) or else the engine's, without the player's choices or the command line's; applied as any change is |
| Back | With changes waiting, asks whether to keep them: Yes applies them, No lets go of them, Cancel stays |

The status line says while changes wait. A display change takes effect
when it is applied: the game loop applies the window's settings, and
Burrow its renderer's and its camera's, at the next frame. The
confirmation that reverts a display mode after fifteen seconds is still to
come (#252).

| Tab | Options |
| --- | --- |
| Display | Window mode, display (named as the system names them), vertical sync, frame limit, brightness; the crosshair, the HUD's opacity, the performance overlay, and pausing in the background |
| Graphics | Field of view, upscaler and its quality, render scale, sharpening, anti-aliasing, texture filtering, motion blur |
| Controls | The mouse's sensitivity, inverted look, and raw input; the gamepad's sensitivities, inverted look, and vibration |
| Audio | The master, music, effects, voice, and interface volumes, muting in the background; subtitles, their size and their background |

### Editing the menus

The menus are four files in `resources/burrow/Content/UI/`, read through a
[VFS](vfs.md) by their paths in Burrow's content:

| File | Holds |
| --- | --- |
| `main_menu.json` | The main menu: its title, its buttons, and the engine's version |
| `pause_menu.json` | The pause menu |
| `options_menu.json` | The options: the variables bound to settings, and the notebook's pages |
| `burrow.theme.json` | A [theme file](gui.md#theme-files) over the game theme: its fonts' sizes and paddings for 1920 by 1080 units, and the menus' own style classes (`Banner.Label`, `Tagline.Label`, `Field.Label`, `Hint.Label`, `Panel.Frame`) |

Edit them as text, by the [format](gui.md#the-format). A developer build
reads them from the source tree, and loads each again as it is saved,
while the game runs ([hot reload](gui.md#hot-reload)): a file that fails
to load is reported on standard error, and the menu stays as it was.

What the files reach of the game, they reach by name:

- **Commands.** A button's `command` is one Burrow registers: `play`,
  `resume`, `options`, `quit_to_menu`, `quit`, `back`, `apply_options`,
  `revert_options`, and `default_options`.
- **Variables.** `options_menu.json` declares a variable for each option,
  with the `setting` it edits and `"deferred": true`; a choice is an
  integer variable, its index, which a combo box shows by its `values`
  in the choices' order.
- **Paths.** Code finds three widgets by their paths, and the files keep
  them: `main/version`, the version's label; `options/panel/tabs/display/monitor`,
  whose `values` it fills with the displays' names; and
  `options/panel/bar/status`, the status line. Each screen's root is
  named `main`, `pause`, or `options`, and gives its first widget
  `default_focus`.

A new option is a variable bound to its setting, a row of a page's grid,
and nothing in C.

## The room

The room is 24 meters square inside walls 6 meters high, open to a
procedural sky. In it are boxes of one and two meters, steps of half a
meter, a platform three meters up with a ramp that climbs to it, a low ramp,
a low wall for cover, and pillars from two meters to the top of the walls:
something at every height, to judge scale and distance by.

Every surface is drawn with a world grid, as Unreal's `WorldGridMaterial`
draws a graybox level: squares of a meter in two shades of gray, a dark line
at every meter and fainter lines at every quarter. The grid is the same size
on every piece, whatever its size, because the pieces' texture coordinates
are meters across each face (a world-aligned texture); on a ramp they are
meters up the slope. Each kind of piece tints the grid: the floor darker,
the walls lighter, the blocks blue, and the pillars and cover orange.

## The HUD

Over the room while playing, in the UI role's font of [the font catalog](fonts.md)
and in logical units, so that it keeps its size on every display, the HUD
shows:

- the frame rate, as `interface.performance_overlay` asks: `frame_rate`
  (Burrow's default), `detailed` (with the frame's time), or `off`;
- the controls, which change when the mouse is freed;
- a crosshair, while `interface.crosshair` is on.

`interface.hud_opacity` fades all of it; 0 hides it. A build of the engine
that carries no fonts draws the crosshair alone, and has no menus: the
game plays at once, Escape frees the mouse, and Escape again quits.

## How it is made

The game is in `src/`, beside the engine, in files of one responsibility
each:

| File | Holds |
| --- | --- |
| `burrow_room.c` | The room as data: its pieces (boxes, ramps and pillars, each a size, a place, a quarter turn and a surface), the player's start, and the triangles of each piece, in the world's space with texture coordinates in meters |
| `burrow_grid.c` | The world grid's texture: the canvas's checkerboard (`tb_image_fill_checkerboard()`), then the lines over it |
| `burrow_player.c` | The player: the view's yaw and pitch and their basis, the field of view, the controls, flying at each tick, and where to draw between ticks |
| `burrow_scene.c` | What the renderer draws: a mesh for each piece, the grid's texture, a material for each surface, and the sky |
| `burrow_hud.c` | The frame rate's average, and the HUD drawn through the canvas |
| `burrow_menu.c` | The menus as a state machine: the screen shown, what each choice leads to, and where Back goes; and the main menu's orbiting camera |
| `burrow_menus.c` | The menus as screens: built from their files, commands by name, the options applied, reverted, and set to their defaults, and the questions asked |
| `burrow_game.c` | The window, the renderer, the menus, and the game loop's callbacks |
| `burrow_settings.c`, `main.c` | The settings and the command line |
| `resources/burrow/Content/` | Burrow's content: the menus' files |

Burrow draws through the renderer's public interface alone: meshes,
materials, the sky and the canvas, with no shaders of its own. Whatever
lighting the renderer draws meshes and materials with, the room is drawn
with.

**Ticks and frames.** The player moves at the game loop's fixed ticks,
`engine.tick_rate` a second (60, Burrow's default), so it moves the same at
any frame rate. Each frame draws the player's eye between where it was
before the last tick and after it, as far as the loop's `alpha` says, so
that motion is smooth at any frame rate. The view turns as each mouse event
comes, not at the ticks, so that looking never waits for one. The menus
run each frame, whether the world ticks or not.

**Field of view.** `graphics.field_of_view` is horizontal degrees on a 16:9
display (90 by default, about 59 degrees vertically). Burrow turns it into
the vertical field of view the camera takes at the design's 16:9, and the
camera keeps at least that view whatever the window's shape
([the camera's axis constraint](display.md#the-camera)): a wider window
sees more at the sides, and a narrower one, 16:10 or 4:3, sees as much
across and more above and below.

**Shaders and content.** Burrow finds its files beside its executable, or
in `Contents/Resources` in a macOS bundle: the shader libraries the build
writes, `Shaders/<backend>.tbsl`, and its content, `Content/`, which the
build copies there from `resources/burrow/Content`. A developer build reads
its content from the source tree before the copy, and compiles what the
shader libraries lack, keeping it in the user's shader cache; a Release
build, with `shaders.runtime_compile` off, draws with the libraries alone
and reads the copy, so its exports carry both ([releasing](releasing.md)).
`burrow check` builds the menus from the content, without a window, so an
export that lacks a file, or carries a broken one, fails its check.

**Windows and displays.** The renderer follows the window's size in pixels,
and the HUD the display's scale, by themselves; the game loop applies
changed display settings to the window, and Burrow applies `display.vsync`,
`graphics.anisotropic_filtering` and the field of view at the next frame.

## Testing

`--frames=N` is the escape hatch that runs the game for a number of frames
and quits: the same as `--set developer.frame_count=N`. `--navigate=ACTIONS`
gives the menus navigation actions, one a frame from the first (`up`,
`down`, `left`, `right`, `accept`, `back`, `next`, `previous`, `tab_next`,
`tab_previous`, or `wait`), and writes each screen they show, as
`burrow: menu: options`.

- `tb_burrow_tests` builds the game's room, grid, player, HUD, and menus
  with their cases (`tests/burrow/`): the view's basis for a yaw and a
  pitch, the pitch's limits, moving relative to the yaw, the field of view,
  the controls and the mouse's capture, interpolating between ticks, the
  pieces' triangles and texture coordinates, the room's layout, the grid's
  lines, the frame rate's average; the menus' state machine and their
  camera; and the menus built from the files Burrow ships, without a
  window: walked by navigation alone, the options applied, reverted, and
  set to their defaults, Back's question answered every way, quitting, the
  files' failures said, and the files loaded again as they change.
- `burrow.plays_a_number_of_frames` runs Burrow without a window, then in
  one with the null renderer, which takes every call that draws the room,
  the HUD and the menus on every platform. Where there is no display it is
  skipped.
- `burrow.menus_are_driven_by_navigation` drives the menus in a window with
  the null renderer by `--navigate`: playing, pausing, quitting to the menu
  and quitting; the options from the pause menu; a change applied on the
  way back, and saved in the player's file; and a change let go of.

On macOS, `burrow --renderer=metal` draws the menus and the graybox room on a
Metal GPU. Automatic selection tries Metal first. An exported game reads
`metal.tbsl` from the shader directory in the app bundle.

To verify device recreation on macOS, launch:

```sh
burrow --renderer=metal --recreate-renderer-on-pause --navigate=accept,back,wait,accept --frames=6
```

At the first pause Burrow captures the room and menu, recreates the graphics
device, and draws the same scene through its existing handles. It temporarily
disables temporal anti-aliasing so both captures use the same projection,
prints their pixel hashes, and exits with an error if the captures differ.
The final navigation action resumes play.

Use `--lose-device-on-pause` in place of `--recreate-renderer-on-pause` to
test recovery after the GPU contents become unavailable. Burrow reports a
device loss, rebuilds the scene on the same Metal adapter from CPU data, and
compares the paused pixels before resuming. A device loss during ordinary play
uses the same recovery function. If recovery fails, Burrow prints the reason
and exits.

## Not yet

- Collision against the room, and walking with gravity (#313).
- A native renderer on Linux. Use `--renderer=null` or `--headless` there.
- Gamepads (#338), and rebinding keys.
- The fifteen seconds that revert a display change unless the player keeps
  it (#252).
