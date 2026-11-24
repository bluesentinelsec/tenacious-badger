# Burrow

`burrow` is the engine's showcase game: a graybox first-person shooter,
under construction. For now it is a room to fly through. It opens a window
as [the display settings](settings.md#burrow) say, makes [a renderer](renderer.md)
on it, and runs [the game loop](game-loop.md), which moves the player at
fixed ticks and draws the room between them.

```
burrow                                  # play, on the platform's best renderer
burrow --renderer=d3d12                 # on a renderer named outright
burrow --renderer=null                  # in a window, drawing nothing: for tests
burrow --frames=120                     # stop after 120 frames
burrow --headless --frames=30           # no window: the loop and the player alone
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
| Escape | Frees the mouse; once it is free, quits (or asks first, in a release build) |
| Left click | Captures the mouse again |

The game starts with the mouse captured, in relative mouse mode
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

Over the room, in the UI role's font of [the font catalog](fonts.md) and in
logical units, so that it keeps its size on every display, the HUD shows:

- the frame rate, as `interface.performance_overlay` asks: `frame_rate`
  (Burrow's default), `detailed` (with the frame's time), or `off`;
- the controls, which change when the mouse is freed;
- a crosshair, while `interface.crosshair` is on.

`interface.hud_opacity` fades all of it; 0 hides it. A build of the engine
that carries no fonts draws the crosshair alone.

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
| `burrow_game.c` | The window, the renderer, and the game loop's callbacks |
| `burrow_settings.c`, `main.c` | The settings and the command line |

Burrow draws through the renderer's public interface alone: meshes,
materials, the sky and the canvas, with no shaders of its own. Whatever
lighting the renderer draws meshes and materials with, the room is drawn
with.

**Ticks and frames.** The player moves at the game loop's fixed ticks,
`engine.tick_rate` a second (60, Burrow's default), so it moves the same at
any frame rate. Each frame draws the player's eye between where it was
before the last tick and after it, as far as the loop's `alpha` says, so
that motion is smooth at any frame rate. The view turns as each mouse event
comes, not at the ticks, so that looking never waits for one.

**Field of view.** `graphics.field_of_view` is horizontal degrees on a 16:9
display (90 by default, about 59 degrees vertically). Burrow turns it into
the vertical field of view the camera takes, and keeps that whatever the
window's shape: a wider window sees more at the sides, a narrower one less.

**Windows and displays.** The renderer follows the window's size in pixels,
and the HUD the display's scale, by themselves; the game loop applies
changed display settings to the window, and Burrow applies `display.vsync`,
`graphics.anisotropic_filtering` and the field of view at the next frame.

## Testing

`--frames=N` is the escape hatch that runs the game for a number of frames
and quits: the same as `--set developer.frame_count=N`.

- `tb_burrow_tests` builds the game's room, grid, player and HUD with their
  cases (`tests/burrow/`): the view's basis for a yaw and a pitch, the pitch's
  limits, moving relative to the yaw, the field of view, the controls and
  the mouse's capture, interpolating between ticks, the pieces' triangles
  and texture coordinates, the room's layout, the grid's lines, and the
  frame rate's average.
- `burrow.plays_a_number_of_frames` runs Burrow without a window, then in
  one with the null renderer, which takes every call that draws the room and
  the HUD on every platform. Where there is no display it is skipped.

## Not yet

- Collision against the room, and walking with gravity (#313).
- A renderer on Linux and macOS: their backends are not written yet, so
  `burrow` there runs only with `--renderer=null` or `--headless`.
- Gamepads, rebinding, and a pause menu.
