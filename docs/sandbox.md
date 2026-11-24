# The sandbox

`badger-sandbox` draws the engine's test scenes in a window, to look by eye
at what the tests check by numbers: whether a triangle turns smoothly, how a
window resizes, what validation says. It is a developer tool, built with the
engine (`TB_BUILD_SANDBOX`, on when the engine is the top-level project) and
never shipped with a game.

```
badger-sandbox                       # the cube, on the platform's best renderer
badger-sandbox --scene triangle      # start with another scene
badger-sandbox list                  # the scenes
badger-sandbox --software            # WARP: the same pixels on every Windows machine
badger-sandbox --validation          # the graphics API's validation, into the log
badger-sandbox --set display.vsync=off
badger-sandbox --frames 120 --capture cube.bmp   # stop after 120 frames, saving the last
```

While it runs, keys 1 to 9 choose a scene, Tab moves to the next, and Escape
quits. It prints the frame time every two seconds.

| Scene | What it shows |
| --- | --- |
| `clear` | The swap chain cleared to a color that cycles |
| `triangle` | A triangle of three colored corners, turning |
| `cube` | A cube from a vertex buffer, turning in perspective, with reversed depth |
| `textured` | A checkerboard made in memory, sampled through the texture's and sampler's bindless indices |

Shaders are HLSL in the sandbox's sources, compiled when a scene starts by
the engine's compiler, through DXC to DXIL for D3D12. The window, settings
and frame pacing are the engine's own: the sandbox runs on the game loop with
the engine's settings, in developer mode.

`--frames` with `--capture` makes any scene a check in CI: the tests run the
cube on WARP and save its last frame.
