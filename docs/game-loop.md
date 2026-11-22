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
   the loop: focus, minimizing, the close button, and Escape.
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

## Without a window

A loop with no window runs the same, for a dedicated server, a replay, or a
test, and handles the events `tb_game_loop_inject()` gives it. A loop may also
run on a clock of its own, which tests use to count ticks and frames exactly.

`developer.frame_count` stops the loop after that many frames, the escape
hatch for running a game for a fixed number of frames in tests and CI.

## Burrow

`burrow` opens a window as the display settings say and runs the loop; nothing
renders yet. `burrow --headless` runs without a window, and
`burrow --headless --set developer.frame_count=30` runs thirty frames and
exits. `burrow check` prints a line that proves the game starts against its
engine, which the packaged exports run.

## Known gaps

- On Windows, the game stops while the player drags or resizes its window,
  because Windows runs a loop of its own for the drag. Running the game on a
  thread of its own, with the window's thread forwarding events, is the fix
  (#210).
- Vsync waits for a renderer's swap chain.
