# The renderer

`badger/renderer.h` draws frames to a window from a camera: meshes and debug
lines, through [the render graph](render-graph.md) and [the RHI](rhi.md).
It is the layer games and tools draw through; what it draws grows from here
(sprites and text, textured materials, the lighting modes).

```c
tb_renderer *renderer = NULL;
tb_rhi_error error = TB_RHI_ERROR_INIT;
if (tb_renderer_create(&(tb_renderer_options){.window = window, .vsync = true}, &renderer,
                       &error) != TB_RHI_OK)
{
    report(error.message);
}
tb_mesh *const sphere = tb_mesh_create_shape(renderer, TB_MESH_SPHERE, 0);

tb_camera camera = tb_camera_perspective(tb_radians(90.0f));
(void)tb_camera_look_at(&camera, (tb_vec3){0, 2, 5}, (tb_vec3){0, 0, 0});
if (tb_renderer_begin_frame(renderer, &camera, (tb_vec4){0.1f, 0.1f, 0.12f, 1}))
{
    tb_renderer_draw_mesh(renderer, sphere, tb_mat4_translation((tb_vec3){0, 1, 0}),
                          (tb_vec4){0.9f, 0.4f, 0.2f, 1});
    tb_debug_grid(renderer, (tb_vec3){0, 0, 0}, 20.0f, 20, (tb_vec4){0.4f, 0.4f, 0.4f, 1}, 0);
    tb_debug_axes(renderer, tb_mat4_identity(), 1.0f, TB_DEBUG_ON_TOP);
    (void)tb_renderer_end_frame(renderer);
}
```

## Frames

A frame begins with a camera and a clear color, gathers what is drawn, and
ends by recording it through the render graph and presenting it. Two frames
are in flight: the CPU fills one while the GPU draws the other. The swap
chain follows the window's size in pixels by itself, and a frame the window
does not show (minimized) is skipped: `tb_renderer_begin_frame()` returns
false. `tb_renderer_set_vsync()` takes effect at the next frame.

`tb_renderer_capture_next_frame()` keeps the next frame's pixels, which
`tb_renderer_captured()` then gives: what golden-image tests and
screenshots read.

## Cameras

A `tb_camera` is a position, an orientation, and a projection: perspective,
from a vertical field of view, or orthographic, from the height of the world
it shows. `tb_camera_look_at()` points one at a target. The conventions are
the engine's ([math](math.md)): right-handed, Y up, a camera looking down
−Z, and reversed depth, with no far plane for perspective by default.

## Meshes

Meshes are triangles of positions, normals and texture coordinates, from data
a caller gives (`tb_mesh_create`) or as generated shapes
(`tb_mesh_create_shape`): cube, plane, sphere, cylinder, cone, torus and
capsule, each in a unit box, facing outward. `tb_mesh_generate()` gives a
shape's triangles to a caller that keeps its own. A mesh is drawn with a
transform and a color, lit (a light from above, with ambient) or flat.

## Debug drawing

`tb_debug_line`, `tb_debug_box`, `tb_debug_sphere`, `tb_debug_grid`,
`tb_debug_axes` and `tb_debug_arrow` add lines to the frame from any thread,
tested against depth or, with `TB_DEBUG_ON_TOP`, drawn over everything.

## Shaders

The renderer's shaders are HLSL in its sources, compiled when it is made by
the engine's compiler, through DXC to DXIL for D3D12. They move to
`/Engine/Private/` files and ahead-of-time shader libraries with #230.

## Not yet

- Instanced draws, transparency, textured materials (#282), skyboxes (#281).
- Debug drawing that lasts more than one frame, and text labels (#278).
- Sprites, 2D and text (#273, #278).
