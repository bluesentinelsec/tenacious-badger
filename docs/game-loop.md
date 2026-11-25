# The game loop

`game_loop.h` runs a game the way Unity and Godot do: a simulation that ticks
at a fixed rate, frames rendered between ticks as fast as the settings allow,
and settings applied whenever they change.

```c
tb_game_loop *const loop = tb_game_loop_create(&(tb_game_loop_options){
    .system = system, .window = window, .settings = &engine,
    .callbacks = {.tick = simulate, .frame = render, .context = game}});
tb_game_loop_run(loop);
tb_game_loop_destroy(loop);
```

## A frame

Each frame does the same five things, in order:

1. **Events.** The window's events go to the game's `event` callback, then to
   the loop: focus, minimizing, the close button, Escape, and the fullscreen
   shortcut.
2. **Settings.** Display settings that changed since the last frame
   reconfigure the window; frame limits and the tick rate take effect at once.
3. **Ticks.** The simulation runs as many fixed ticks as the time that passed
   is owed: `engine.tick_rate` a second, Unity's `FixedUpdate` and Godot's
   `_physics_process`. The same input gives the same game at any frame rate.
4. **The frame.** The `frame` callback renders, with `alpha`, how far time
   stands from the last tick toward the next, for drawing between them:
   Unity's `Update` and Godot's `_process`.
5. **Pacing.** The loop waits out `display.frame_limit`, or
   `display.background_frame_limit` while the window has no focus or is
   minimized. Waiting is exact to the clock, and a frame that ran late starts
   the count again instead of making the next frames rush.

Time is counted exactly: the loop owes the simulation nanoseconds times the
tick rate, so a tick of a sixtieth of a second, which is no whole number of
nanoseconds, never drifts.

## While the window is dragged

While the player drags the window's border or title bar, Windows and macOS
hold the thread in a loop of their own, and reading events does not return
until the drag ends. The loop sets itself as the window system's refresh
([Windows and displays](windows.md#live-resizing)), and runs frames from
inside the system's loop: the events queued, settings, the ticks due, the
frame at the window's new size, and pacing, as a frame between drags does.
The picture follows the drag, and the simulation goes on. Each frame the
renderer begins resizes its swap chain to the window and computes the display
policy's viewport for the new size ([The renderer](renderer.md)), so the
bars, the 3D view, the canvas and the interface keep their places.

A refresh that comes while the loop is running a frame, as a window the
settings reconfigure may send, runs no frame of its own. A minimized window
renders nothing: the renderer begins no frame while its window has no size.

## Long frames and pausing

A frame counts for at most `engine.max_frame_time` seconds. After a stall, a
breakpoint, or a dialog, the game carries on rather than racing through the
ticks it missed.

A paused game runs no ticks and its simulated time stands still, while frames
go on: the options menu draws over a frozen world. The game pauses with
`tb_game_loop_set_paused()`; the loop also pauses while the window has no
focus, when `gameplay.pause_when_unfocused` says to. `developer.time_scale`
slows or speeds simulated time.

## Quitting

The close button, Alt+F4, Command+Q, and Escape ask to quit.

| Build | What happens |
| --- | --- |
| Developer (`developer.quit_without_asking` on) | The game quits at once. |
| Release | The player is asked "Quit? Unsaved progress may be lost.", through the game's `confirm_quit` callback, or else the system's dialog. |

Where the system has no dialog, on Linux until the engine's interface draws
its own, the player confirms by asking a second time within three seconds;
the loop says so on standard error. A game that opens a pause menu on Escape
takes the key in its `event` callback, and the loop leaves it alone.

## Switching to fullscreen

Alt+Enter switches the window between windowed and borderless, as players
expect of a PC game; on a Mac the shortcut is Control+Command+F, the system's
own for fullscreen.

| Platform | Shortcut |
| --- | --- |
| Windows | Alt+Enter |
| Linux | Alt+Enter |
| macOS | Control+Command+F |

The loop does not change the window itself: it sets `display.window_mode` at
the runtime layer, from windowed to borderless, or from borderless or
fullscreen to windowed, and the next frame applies the setting as it applies
any change of it. An options menu showing the setting shows the switch. The
loop never writes the player's settings file; a game that saves its settings
saves the runtime layer with the rest, as for any change in its options menu.

Leaving windowed mode, the loop remembers the window's size, which the player
may have dragged, and a window it returns to takes that size again. The size
lasts for the session only, and is forgotten once `display.window_width` or
`display.window_height` changes. Where the window is placed is not
remembered.

A game takes the shortcut for itself in its `event` callback, or turns it
off with `disable_fullscreen_shortcut` in the loop's options: an editor that
binds Alt+Enter to something else, for one. `tb_game_loop_is_fullscreen_shortcut()`
says which key a platform uses.

## Without a window

A loop with no window runs the same, for a dedicated server, a replay, or a
test, and handles the events `tb_game_loop_inject()` gives it. A loop may also
run on a clock of its own, which tests use to count ticks and frames exactly.

`developer.frame_count` stops the loop after that many frames, the escape
hatch for running a game for a fixed number of frames in tests and CI.

## Burrow

`burrow` opens a window as the display settings say and runs the loop: the
player flies at its ticks, and each frame draws the room between them
([Burrow](burrow.md#how-it-is-made)). `burrow --headless` runs without a window,
and `burrow --headless --frames=30` runs thirty frames and exits. `burrow check` prints a line that proves the game starts against its
engine, which the packaged exports run.

## Known gaps

- The game runs on the window system's thread. A frame that takes long
  makes a drag of the window feel heavy, as it makes the game feel heavy;
  running the game on a thread of its own, with the window's thread
  forwarding events, would part them (#210).
- Vsync waits for a renderer's swap chain.
