# The renderer

`badger/renderer.h` draws frames to a window from a camera: meshes, plain or
with [materials](#materials), and debug lines, through
[the render graph](render-graph.md) and [the RHI](rhi.md). It is the layer
games and tools draw through, in 3D and in [2D](#2d-the-canvas); what it
draws grows from here (text, the lighting modes).

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

## Materials

`badger/material.h` gives meshes textures, after glTF's metallic-roughness
model: a base color (a texture times a factor), a tangent-space normal map,
and roughness and metallic (green and blue of a texture, times factors).
Each texture is optional; a material without one samples white, or a flat
normal.

```c
tb_texture *const checker = tb_texture_create(
    renderer, &(tb_texture_desc){.width = 256, .height = 256}, pixels, sizeof(pixels));
tb_texture *const bumps = tb_texture_create(
    renderer, &(tb_texture_desc){.width = 256, .height = 256, .content = TB_TEXTURE_NORMAL},
    normals, sizeof(normals));

tb_material_desc desc = TB_MATERIAL_DESC_INIT;
desc.base_color = checker;
desc.normal = bumps;
desc.roughness = 0.6f;
desc.uv_scale = (tb_vec2){4, 4};
tb_material *const tiles = tb_material_create(renderer, &desc);

/* each frame */
tb_renderer_draw_mesh_material(renderer, cube, transform, tiles);
```

**Textures** are RGBA, 8 bits a channel, from pixels in memory
(`tb_texture_create`; loaders come later), or a texture a caller made
through the RHI (`tb_texture_from_rhi`, borrowed). What they hold decides
how they are read: `TB_TEXTURE_COLOR` is sRGB, as images are authored, and
is read as linear (`R8G8B8A8_SRGB`); `TB_TEXTURE_DATA` and
`TB_TEXTURE_NORMAL` are read as they are.

**Mipmaps are made on the GPU** when a texture is created: a compute
shader halves each level into the next, a thread a texel, reading the
level before and writing the next through their storage indices
(`tb_rhi_texture_storage_mip_index`). Colors average in linear light and
are written back in sRGB; normals average and are renormalized. A compute
pass was chosen over drawing each level as a render target: both levels
stay in one state, storage, so the RHI's barriers (which move a whole
texture) need no per-level states, and no pipeline per format is needed. Each
texel averages the 2x2 texels under it, clamped at the edge; a level of
odd size leaves out the last row or column of the one before.

**Filtering** is trilinear, repeating, and anisotropic: the renderer's
options and `tb_renderer_set_anisotropy()` take the `graphics.anisotropic_filtering`
setting through `tb_engine_settings_anisotropy()`, and a change takes
effect at the next frame.

**Normal maps** are tangent-space, +X along u and +Y up in the image, as
glTF's are. Meshes carry no tangents: the pixel shader works out each
pixel's tangent frame from the screen-space derivatives of its position
and texture coordinates (Schüler's cotangent frame). That keeps
`tb_mesh_vertex` as it is, works for any caller's mesh, mirrored texture
coordinates included, and costs a few instructions a pixel; precomputed
tangents can come with model loading if profiles ask for them.

**Lighting.** Materials are lit as every mesh is, by one light from above
and behind the camera with ambient light, with a highlight that roughness
and metallic shape: the hooks the raster lighting mode (#220) replaces.
`unlit` draws the base color alone. A device without bindless resources
draws a material in its base color factor.

**Color.** The frame is written as the shaders compute it, linear, into a
`B8G8R8A8_UNORM` swap chain, as every draw is until the output transform
comes: so sRGB textures, read as linear, look darker than they were
authored.

## Debug drawing

`tb_debug_line`, `tb_debug_box`, `tb_debug_sphere`, `tb_debug_grid`,
`tb_debug_axes` and `tb_debug_arrow` add lines to the frame from any thread,
tested against depth or, with `TB_DEBUG_ON_TOP`, drawn over everything.

`tb_debug_text` adds a label at a point in the world, from any thread: at
the end of the frame each point is projected by the frame's camera, and its
text is drawn by the canvas centered on it, on the highest layer, over
everything, at the window's display scale. Labels are drawn in the font
`tb_renderer_set_debug_font()` chooses, at a size in logical units; until
one is chosen they are not drawn. A point behind the camera has no label.

```c
tb_renderer_set_debug_font(renderer, font, 14.0f);
/* each frame */
tb_debug_text(renderer, enemy_position, TB_TEXT("enemy 3: patrolling"),
              (tb_vec4){1, 0.9f, 0.5f, 1});
```

## 2D: the canvas

`badger/canvas.h` draws 2D over a frame, as Unreal's Canvas and Godot's
CanvasItem do: rectangles, rounded rectangles, circles and lines, filled or
outlined; images and sprites. A game's interface and a 2D game draw through
it alike.

```c
uint8_t pixels[64 * 64 * 4] = {0};
tb_image_fill_checkerboard(pixels, sizeof(pixels), 64, 64, 8, (tb_vec4){1, 1, 1, 1},
                           (tb_vec4){0.2f, 0.2f, 0.2f, 1});
tb_image *const checker = tb_image_create(
    renderer, &(tb_image_desc){.width = 64, .height = 64, .filter = TB_IMAGE_NEAREST}, pixels,
    sizeof(pixels));

if (tb_renderer_begin_frame(renderer, &camera, clear))
{
    /* the 3D scene ... */
    const tb_canvas_view ui = tb_canvas_view_logical(renderer);
    tb_canvas_set_view(renderer, &ui);
    tb_canvas_fill_rounded_rect(renderer, (tb_canvas_rect){16, 16, 240, 64}, 8,
                                (tb_vec4){0, 0, 0, 0.6f});
    tb_canvas_set_layer(renderer, 1);
    tb_canvas_draw_sprite(renderer, &(tb_canvas_sprite){.image = checker,
                                                        .source = {0, 0, 16, 16},
                                                        .position = {48, 48},
                                                        .size = {32, 32},
                                                        .pivot = {0.5f, 0.5f},
                                                        .rotation = tb_radians(30.0f),
                                                        .tint = {1, 1, 1, 1}});
    (void)tb_renderer_end_frame(renderer);
}
```

**Where.** The canvas is the frame's last pass, `canvas`, after the scene
and the debug lines on top: it keeps what they drew (`TB_RHI_LOAD_KEEP`)
and blends over it.

**Coordinates.** The origin is the view's top left, x to the right and y
down, as windows, interfaces and image editors have it; the 3D world's Y
up does not apply. Rotations are radians, clockwise on the screen, around a
sprite's pivot (a fraction of its size: (0, 0) its top left, (0.5, 0.5) its
middle).

**Scale.** A `tb_canvas_view` turns units into pixels: `pixel = origin +
scale * unit`, and clips to a rectangle of pixels. Each frame begins in
pixels; a view applies to what is drawn after it, so one frame can mix
them.

| View | One unit is | For |
| --- | --- | --- |
| `tb_canvas_view_pixels()` | a pixel | tools, pixel-exact work |
| `tb_canvas_view_logical(renderer)` | the window's content scale in pixels (1.5 at 150%) | interfaces, which keep their size on every display |
| `tb_canvas_view_fit(w, h, 320, 180, true)` | a whole number of pixels, letterboxed in the middle | pixel art at a fixed resolution |
| `tb_canvas_view_fit(w, h, 1920, 1080, false)` | any scale that fits, letterboxed | a 2D game at a design resolution |

`tb_canvas_view_from_pixels()` goes the other way, from the mouse to the
canvas. `tb_renderer_size()` gives the frame's pixels.

**Layers.** `tb_canvas_set_layer()` sets the layer of what is drawn after,
any `int32_t`; higher layers are over lower ones, and within a layer, later
is over earlier. Each frame begins on layer 0.

**Blending and color.** Colors are RGBA from 0 to 1, straight alpha, given
to the frame as they are. The canvas blends premultiplied (`ONE`,
`ONE_MINUS_SOURCE_ALPHA`): colors are premultiplied as they are drawn, and
an image's pixels when it is uploaded, unless `tb_image_desc.premultiplied`
says they are already. Premultiplied alpha filters without dark fringes and
lets text and additive glow share one blend later.

**Anti-aliasing.** Shapes are quads whose pixel shader measures the
signed distance to a rounded box's edge, in pixels, and covers each pixel
by `saturate(0.5 - distance)`: a pixel of smoothing at any scale, rounded
corners for nothing, outlines as a band of that distance. A shape's quad
grows by a pixel on every side for the smoothing. Edges on whole pixels
stay crisp. Images and sprites are not smoothed at their edges; a sprite
sheet read with `TB_IMAGE_LINEAR` may bleed from neighboring frames, which
`TB_IMAGE_NEAREST` at whole scales does not.

**Text.** `tb_canvas_draw_text(renderer, font, text, size, position, color,
options)` draws UTF-8 laid out by the font module ([fonts](fonts.md#laying-out-text)),
its box's top left at `position`, in the canvas's units:

```c
const tb_font_layout_options wrapped = {.wrap_width = 240, .align = TB_FONT_ALIGN_CENTER};
tb_canvas_draw_text(renderer, font, TB_TEXT("Press Start"), 16.0f, (tb_vec2){32, 32},
                    (tb_vec4){1, 1, 1, 1}, &wrapped);
const tb_vec2 size = tb_canvas_measure_text(renderer, font, TB_TEXT("Press Start"), 16.0f,
                                            &wrapped);
```

- **Crisp at any scale.** Text is laid out at `size` times the view's scale
  in pixels, not drawn at one size and scaled: 16 units in a logical view
  is a 24-pixel font at 150%. Each glyph's bitmap lands on whole pixels and
  each baseline on a whole row; the pen's fraction of a pixel is kept by
  the atlas's four subpixel bins.
- **Pixel fonts** (`pixel_size` in the options) are drawn aliased at the
  whole multiple of their size nearest `size` times the view's scale, on
  whole pixels: in a `tb_canvas_view_fit(..., true)` view of a pixel-art
  game, a font of 8 pixels at 8 units is a square of the view's pixels for
  each of the font's.
- **Measuring.** `tb_canvas_measure_text` gives the widest line and the
  lines' height in the view's units, as drawing would lay them out.
- **Fonts' lifetimes.** A font drawn with stays open until the frame ends.
  Before closing one, call `tb_canvas_forget_font`, outside a frame: the
  atlas keeps glyphs by their font's address.

**Images** are RGBA, 8 bits a channel, made from pixels in memory
(`tb_image_create`), changed in regions (`tb_image_update`, submitted at
once, seen by the frame being drawn), and read nearest or linear. Asset
loaders come later; for now `tb_image_fill_checkerboard`,
`tb_image_fill_gradient` and `tb_image_fill_noise` make pixels for samples
and tools.

### How it draws

Every quad of a frame is one instance of one draw, whatever its layer,
image or shape, because everything a quad needs is reached by index:

- At the end of the frame the quads are sorted by a key of their layer
  and their place in the order drawn, and written to the frame's upload
  buffer: 96 bytes each, in pixels already (corner, two edges, radius,
  outline, texture coordinates, premultiplied color, clip rectangle, and
  the bindless indices of its texture and sampler).
- The vertex shader reads its quad from that buffer as a
  `ByteAddressBuffer` from `ResourceDescriptorHeap`, by `SV_InstanceID`,
  and its corner by `SV_VertexID`: there is no vertex buffer.
- The pixel shader samples the quad's image through
  `ResourceDescriptorHeap` and `SamplerDescriptorHeap`; a solid quad
  samples a white pixel, so no quad branches on whether it has an image.

Views and clips are applied on the CPU, so changing them does not split
the draw.

**Glyphs** are quads too. The renderer keeps a glyph atlas
(`tb_font_atlas`, 1024 pixels a side unless `tb_renderer_options.glyph_atlas_size`
says otherwise) and its image on the GPU as an `R8_UNORM` texture. A text
quad samples it nearest, each texel on one pixel, and its pixel shader
multiplies the quad's premultiplied color by the texel's first channel,
the glyph's coverage. At the end of each frame, before the canvas pass:

1. Labels are laid out and drawn.
2. If the atlas cleared partway through the frame, because a glyph did not
   fit, the frame's glyphs placed before are asked for again, and their
   quads' texture coordinates patched. Asking may clear it again when the
   frame's glyphs do not all fit together; after three tries, those still
   missing are given no color, and counted as dropped.
3. The atlas's changed rectangles (`tb_font_atlas_get_dirty`) are packed
   into one staging buffer and copied to the texture by one command list,
   submitted before the frame's, and the list of changes cleared. A frame
   that draws only glyphs it drew before copies nothing. The frame's uploads (16 MiB) hold about 170,000 quads beside the
meshes' constants; past that, the last in order are dropped, and
`tb_canvas_stats_of()` counts them. A device without bindless resources
(below Shader Model 6.6 or resource binding tier 3) draws no 2D, and the
statistics say so.

## Shaders

The renderer's shaders are HLSL in its sources, compiled when it is made by
the engine's compiler, through DXC to DXIL for D3D12. They move to
`/Engine/Private/` files and ahead-of-time shader libraries with #230.

## Not yet

- Instanced draws, transparency, skyboxes (#281).
- The output transform: writing the frame in sRGB, and tone mapping.
- Precomputed tangents, texture compression, and streaming of mip levels.
- Debug drawing that lasts more than one frame.
- Images with rounded or smoothed edges, gradients and polygons in 2D;
  a 2D pass into targets other than the frame, and 2D in the world.
- Text that scales and turns in 3D (signed distance fields), outlines and
  shadows of text, and markup within a string.
