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
```

While it runs, keys 1 to 9 choose a scene, Tab moves to the next, and Escape
quits. Holding the right mouse button flies the camera, as in Unreal's
viewport: the mouse looks, W A S D move, Q and E go down and up, Shift goes
faster, and the wheel changes the speed. The frame time is drawn in the
bottom left corner of every scene, and printed every two seconds.

| Scene | What it shows |
| --- | --- |
| `shapes` | Every shape the renderer generates, lit and turning on a floor, with debug boxes, a grid, axes and an arrow |
| `canvas` | The 2D layer over the shapes: rounded panels, circles, rectangles, lines of any thickness, images made in memory, and a turning sprite, in logical units |
| `text` | Text over the shapes: Roboto at sizes from 10 to 48, a paragraph wrapping in three panels (left, centered and right) whose width changes over time, the VGA font and Silkscreen at whole multiples of their sizes, and labels in the world naming the shapes |
| `triangle` | A triangle of three colored corners, turning, drawn through the RHI itself inside the renderer's scene pass |
| `textured` | A checkerboard made in memory, sampled through the texture's and sampler's bindless indices |
| `clear` | The swap chain cleared to a plain color |

The sandbox draws through [the renderer](renderer.md); the `triangle` and
`textured` scenes record their own RHI commands into its scene pass with
`tb_renderer_draw_custom()`. It draws text in the fonts of the tests' data
(`tests/data/fonts`: Roboto, Px437 IBM VGA 8x16 and Silkscreen), which CMake
builds into it, and labels in Roboto.

Shaders are HLSL in the sandbox's sources, compiled when a scene starts by
the engine's compiler, through DXC to DXIL for D3D12. The window, settings
and frame pacing are the engine's own: the sandbox runs on the game loop with
the engine's settings, in developer mode.

`--frames` with `--capture` makes any scene a check in CI: the tests run the
shapes on WARP and save their last frame.
