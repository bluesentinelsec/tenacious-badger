# The renderer

`badger/renderer.h` draws frames to a window from a camera: meshes, plain or
with [materials](#materials), [skinned](#skinned-meshes) or not,
[lit](#lighting) by lights and under a [sky](#skies), and debug lines,
through [the render graph](render-graph.md) and [the RHI](rhi.md). It is
the layer games and tools draw through, in 3D and in [2D](#2d-the-canvas);
what it draws grows from here (the hybrid and path-traced lighting
modes).

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
`tb_renderer_graph()` gives the render graph the frames go through, whose
`tb_render_graph_dump()` describes the last of them.

`tb_renderer_capture_next_frame()` keeps the next frame's pixels, which
`tb_renderer_captured()` then gives: what golden-image tests and
screenshots read.

## The viewport

The 3D scene is drawn into the viewport that the app's
[display policy](display.md) gives the frame: a rectangle of whole pixels
of the swap chain, beside which are bars of the policy's color.
`tb_renderer_set_viewport_policy()` sets the policy and the player's aspect
ratio, and the renderer computes the viewport at each
`tb_renderer_begin_frame()`, from the swap chain's size and the display's
scale; `tb_renderer_viewport()` gives it, for the canvas, the GUI and the
mouse. A renderer begins with `TB_VIEWPORT_POLICY_LOGICAL`, whose viewport
is the whole frame, so a renderer an app gives no policy fills its window
as before.

```c
tb_viewport_policy policy = TB_VIEWPORT_POLICY_INIT;
tb_viewport_policy_from_settings(engine, &policy);
tb_renderer_set_viewport_policy(renderer, &policy, tb_viewport_aspect_ratio_from_settings(engine));
```

- **The camera** projects at the viewport's aspect
  (`tb_viewport_aspect_of()`): the rectangle's, or in `ignore` the design's,
  stretched to the rectangle.
- **The scene's targets**, the G-buffer, its depth and its light, are the
  viewport's pixels times the render scale; temporal anti-aliasing writes
  its history at the viewport's size, and the tone mapping pass draws it
  into the rectangle.
- **The bars** are cleared to the policy's color, written to the swap
  chain as it is given (sRGB), by the tone mapping pass, which clears the
  whole image and then draws only the rectangle.
- **Debug lines and labels** are drawn in the rectangle, from the same
  camera.
- **The canvas** draws over the whole frame, in any of its views; the
  design's view (`tb_canvas_view_viewport()`) is in the rectangle.

Without bindless resources, meshes are drawn straight into the swap
chain's rectangle; its clear, of the clear color, covers the bars too.

### The viewport render mode

In the policy's viewport render mode
([display](display.md#render-modes)), the viewport has a target
(`tb_viewport::target_width` and `target_height`), and the frame is drawn
in two steps:

1. **Into the target**, a transient texture of the render graph of a
   pixel a unit, in the swap chain's format: the 3D scene, whose
   rectangle is the whole target, so its targets are the target's pixels
   times the render scale and its temporal history is the target's size;
   its debug lines; and the canvas's quads of the target
   (`TB_CANVAS_TARGET_VIEWPORT`), the design's view's and the GUI's.
2. **Into the swap chain**: the upscale pass
   (`/Engine/Private/Upscale.hlsl`) clears the image to the bar color and
   draws the target into the viewport's rectangle, sampled nearest in the
   `integer` scale mode and linearly otherwise, each pixel kept half a
   texel inside the units shown. Then the canvas's quads of the frame, at
   its own pixels: overlays, and labels.

The chain of sizes is the render size, then the target, then the
viewport: no copy at the frame's size is made but the upscale's own
writes. The camera projects at the viewport's aspect, as in the canvas
mode, so the scene shows the same in both. Without bindless resources the
viewport has no target, and the canvas mode is drawn.

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
transform and a color, lit by the scene's [lights](#lighting) or flat.

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

**Lighting.** Materials are lit by the scene's [lights](#lighting), their
roughness and metallic shaping Unreal's default lit model. `unlit` draws
the base color alone, through the same output transform. A device without
bindless resources draws a material in its base color factor.

## Skinned meshes

`badger/skinning.h` draws meshes whose vertices follow the joints of a
skeleton, posed by the skinning matrices of [the animation module](animation.md).

```c
tb_skinned_mesh *const arm = tb_skinned_mesh_create(renderer, vertices, vertex_count,
                                                    indices, index_count);

/* each frame */
tb_anim_evaluate(&instance);   /* fills instance.skinning */
tb_renderer_draw_skinned(renderer, &(tb_skinned_draw){.mesh = arm,
                                                      .transform = transform,
                                                      .joints = instance.skinning,
                                                      .joint_count = joint_count,
                                                      .material = sleeve});
```

**Vertices** (`tb_skinned_vertex`, 56 bytes) are a mesh vertex's position,
normal and texture coordinates, with four 16-bit joint indices and four
weights, as glTF's are. The weights ought to sum to 1, and are used as they
are; a joint index past a draw's matrices takes the last.

**A draw** gives its skinning matrices, which are copied into the frame's
upload buffer at once (so they need last only for the call), and a color
and shading, or a material: a skinned mesh is drawn by the same pipelines,
lit and textured, as any mesh.

**How it draws.** At the end of the frame, before the scene pass, a compute
pass, `skinning`, skins every skinned draw of the frame: a thread a vertex
reads the mesh's bound vertices (a `ByteAddressBuffer` of GPU memory) and
its draw's matrices (in the frame's uploads), both by bindless index, sums
the four matrices' columns by weight, and writes the position, the normal
(transformed by the same matrix, normalized) and the texture coordinates,
laid out as a `tb_mesh_vertex`, into a buffer the frame's render graph
makes, each draw at its own offset. The graph moves that buffer from
storage to vertex state before the scene pass, which draws each skinned
mesh from its part of it, with its own index buffer.

**Why a compute pass**, not skinning in the vertex shader: the skinned
vertices are made once a frame however many passes draw them (shadow
maps, a depth prepass, the ray-tracing structures of the hybrid and path
tracing modes, which need the skinned positions in a buffer), and the
mesh and material pipelines need no skinned variant: a skinned mesh is a
vertex buffer like any other by the time it is drawn. The cost is a
frame's buffer of skinned vertices, 32 bytes each, which the graph pools
from frame to frame.

**On the CPU.** Where the GPU cannot skin, the CPU does. A device without
bindless resources (below Shader Model 6.6 or resource binding tier 3)
cannot read vertices and matrices by index, and a backend that cannot make
the skinning pipeline cannot run it; there, each skinned draw's vertices are
skinned when the draw is made, by `tb_skin_vertices()`, into the frame's
uploads, laid out as `tb_mesh_vertex`, and the scene pass draws them from
there, through the same mesh and material pipelines. A skinned mesh keeps
its bound vertices in memory beside its GPU buffer for this, so either path
can skin it. The CPU's arithmetic is the shader's: the same weighted sum of
columns, used as the weights are, a joint past the matrices taking the
last, and a normal the sum turns to nothing kept as it was bound; on WARP
the two paths draw the same pixels. What it costs is the CPU's time, and 32
bytes of the frame's 16 MiB of uploads a vertex: a draw that does not fit
is left out, as a draw whose matrices do not fit is on the GPU.

```c
tb_skinning_info skinning = {0};
tb_renderer_skinning_info(renderer, &skinning);   /* where, and why */
/* "GPU: a compute pass", "CPU: the device has no bindless resources", "CPU: forced" */
tb_renderer_force_cpu_skinning(renderer, true);   /* the CPU from the next frame on */
```

Which path skins is a capability, never a failure: the renderer logs it
when it is made, and again whenever it changes, and
`tb_renderer_skinning_info()` reports it with the reason, for an options
screen or [the sandbox's inspector](sandbox.md#the-inspector).
`tb_renderer_force_cpu_skinning()` takes the CPU path where the GPU's is
there, from the next frame on, so that tests and the sandbox compare the
two on the same machine.

`tb_skin_vertices()` stands on its own: it reads a range of bound vertices
and the joints' matrices and writes skinned vertices, touching nothing
else, so a caller may skin a mesh on the CPU for itself (for picking, or
collision against a posed mesh), and the renderer can later split a large
mesh's vertices into ranges across [the job system](jobs.md).

## Skies

`badger/sky.h` draws a sky behind the scene: a procedural one, computed in
its shader with no texture, or a cube map a caller fills.

```c
tb_sky sky = TB_SKY_PROCEDURAL_INIT;   /* blue overhead, pale at the horizon, a sun */
sky.sun_direction = (tb_vec3){0.4f, 0.5f, -1.0f};
tb_renderer_set_sky(renderer, &sky);

tb_texture *const faces = tb_texture_create_cube(
    renderer, &(tb_texture_desc){.width = 512, .height = 512}, pixels, sizeof(pixels));
tb_renderer_set_sky(renderer, &(tb_sky){.kind = TB_SKY_CUBE, .cube = faces});

tb_renderer_set_sky(renderer, NULL);   /* back to the clear color */
```

**Lasting.** `tb_renderer_set_sky()` sets the sky for every frame from the
next until it is set again, as a level's sky is; a game need not repeat it
each frame. A cube map is borrowed while it is the sky.

**Procedural.** Above the horizon the color runs from the horizon's to the
zenith's, by the square root of the direction's height, so it changes
quickly near the horizon as a sky does; below, it reaches the ground's a few
degrees down. The sun is a disc of an angular radius, toward its direction,
smoothed over a pixel, with a faint glow.

**Cube maps** are six square faces in Direct3D's order (+X, -X, +Y, -Y, +Z,
-Z), each laid out as Direct3D lays it; `tb_texture_create_cube()` names the
direction of each texel. Their mip chains are made as a texture's are, each
face's levels in one dispatch. The RHI's `TB_RHI_TEXTURE_CUBE` is a texture
of six layers sampled as a `TextureCube`; its faces are filled by a copy to
each layer.

**How it draws.** After the meshes and custom draws, before the debug
lines, one triangle covers the view at depth 0, the far plane (depth is
reversed), tested `GREATER_EQUAL` against the depth the scene wrote and
writing none. Each pixel the scene covered fails before it is shaded, so
the sky costs only what shows, which drawing it first with depth writes off
would not: there every pixel shades the sky, and the scene overdraws it.
Each pixel's direction is the camera's forward plus its place in the view
times the view's edges, so it needs no matrix inverse, which a projection
with no far plane does not have. A device without bindless resources draws
no sky.

## Lighting

`badger/light.h` lights the scene: the raster lighting mode, deferred
physically based shading after Unreal's, on every device with bindless
resources.

```c
tb_light sun = TB_LIGHT_INIT;
sun.kind = TB_LIGHT_DIRECTIONAL;
sun.direction = (tb_vec3){-0.3f, -1.0f, -0.4f};
sun.intensity = 3.0f;
const tb_light_id sun_id = tb_renderer_add_light(renderer, &sun);

tb_light lamp = TB_LIGHT_INIT; /* a point light */
lamp.position = (tb_vec3){0, 2, 0};
lamp.color = (tb_vec3){1.0f, 0.8f, 0.6f};
const tb_light_id lamp_id = tb_renderer_add_light(renderer, &lamp);

/* later */
lamp.position.x += 1.0f;
tb_renderer_set_light(renderer, lamp_id, &lamp);
tb_renderer_remove_light(renderer, sun_id);
```

**Lights** are directional, point or spot, and last from the frame after
they are added until they are removed, as a level's lights do, named by
handles that a removed light's never matches again (a slot and a
generation). The scene holds `TB_RENDERER_MAX_LIGHTS` (1024). Colors are
linear; a light of intensity 1 lights a white, matte surface that faces it
to 1: a directional light anywhere, a point or spot light at a meter.
Point and spot lights fall off with the square of the distance, windowed
smoothly to nothing at their range, `saturate(1 - (d²/r²)²)² / d²`, and a
spot's cone fades between its inner and outer angles, squared, as Unreal's
do. A frame without lights is lit by a default light, so that a scene
without any still shows: toward the procedural sky's sun when there is one,
else from above and a little behind the camera.

**Ambient light** comes from the sky. Each surface takes the sky's light
above it and the ground's below, blended by the way it faces: a procedural
sky's zenith, horizon and ground colors, or a cube map's last level. A
surface's reflection is the sky toward its mirror direction, from the cube
map's level that matches its roughness, or blended from the gradient toward
the average of the half it faces, scaled by Karis's analytical fit of the
environment BRDF. Without a sky, `tb_renderer_set_ambient(renderer, sky,
ground)` sets the colors of the gradient (0.25 and 0.08 gray by default).
This is image-based lighting at its cheapest: a prefiltered cube map,
spherical harmonics and occlusion come with the hybrid mode's needs.

**Shading** is Unreal's default lit model: a Lambert diffuse, and a GGX
specular with Smith's joint visibility (approximated) and Schlick's
Fresnel, roughness remapped as Unreal's (α = roughness²). Dielectrics
reflect 4% at normal incidence; metals reflect their base color and have no
diffuse.

**Output.** The scene's light is in linear HDR. The output transform
multiplies it by an exposure (1), maps each channel through Narkowicz's fit
of the ACES filmic curve, its input scaled by 0.6 as he gives it (a middle
gray of 0.18 shows as about 0.41), and encodes it in sRGB into the
`B8G8R8A8_UNORM` swap chain. So a color of 1 shows as 214 of 255, and
a surface lit to 2 as 236: colors given to the renderer are light, not
pixels. The canvas and debug lines draw after it, and their colors are
written as given.

### How it draws

| Pass | Writes | Reads |
| --- | --- | --- |
| `skinning` | the frame's skinned vertices | skinned meshes, joint matrices |
| `gbuffer` | base color (`R8G8B8A8_SRGB`), normal (`R16G16_FLOAT`, octahedral), surface (`R8G8B8A8_UNORM`: metallic, roughness, occlusion, lit), depth | meshes, materials' textures |
| `lighting` | the scene's light (`R16G16B16A16_FLOAT`) | the G-buffer and depth; lights and constants from the frame's uploads |
| `forward` | the scene's light, and depth | custom draws, then the sky |
| `temporal anti-aliasing` | a history, at the output's size | the scene's light and depth, the last history |
| `tonemap` | the swap chain's image | the history, or the scene's light without TAA |
| `debug lines` | the swap chain's image | depth |
| `canvas` | the swap chain's image | the frame's quads |

- **The G-buffer.** Every mesh, plain or with a material, skinned or not,
  is drawn by one pipeline: a mesh of a color is drawn as a material of
  that color with no textures would be (rough, not metal). Unlit meshes
  and materials (`TB_MESH_FLAT`, `unlit`) write their base color and a
  surface the lighting pass passes through. The normal is octahedrally
  encoded in two halves (Cigolle et al. 2014), which keeps it to 4 bytes.
  The color targets are not cleared: the lighting pass reads them only
  where depth says something was drawn.
- **The lighting pass** is one triangle over the view. Each pixel loads
  its depth, and where nothing was drawn writes the frame's clear color;
  elsewhere it rebuilds its place in the world through the inverse of the
  frame's clip transform, loops over the frame's lights (64 bytes each in
  the frame's uploads, read as a `ByteAddressBuffer` by bindless index),
  and adds the ambient light. A frame's lights are all shaded at every
  pixel: tiled or clustered culling comes when scenes have enough lights
  to need it.
- **After lighting**, what is not lit is drawn into the scene's light, as
  Unreal's translucency and sky are: the custom draws, with the scene's
  depth, and the sky behind everything.
- **Debug lines** are drawn over the tone-mapped image, so that their
  colors are as given and they are neither jittered nor blurred by what
  comes between. A line tested against depth loads the scene's depth at
  its pixel, scaled to the render's size, and is discarded where the scene
  is nearer.
- **Without bindless resources**, there is no raster lighting mode: meshes
  are drawn in one pass straight into the swap chain, lit by the default
  light with a constant ambient, as before it, materials in their base
  color factor, with no sky and no output transform.

## Resolution and anti-aliasing

The scene renders at a share of the [viewport's](#the-viewport)
resolution, the render scale, and temporal anti-aliasing (TAA) upsamples it
to the viewport's, as Unreal's TAAU does: the engine's own upscaler, always
there, which vendors' upscalers will stand beside as plugins. The canvas
and the debug lines are drawn at the output's resolution.

```c
tb_renderer_quality quality = TB_RENDERER_QUALITY_INIT; /* scale 1, TAA, 256 lights */
tb_renderer_quality_from_settings(&engine, &quality);   /* or from the graphics settings */
tb_renderer_set_quality(renderer, &quality);            /* from the next frame */

tb_renderer_camera_cut(renderer); /* the next frame does not follow from this one */
```

**Jitter.** With TAA on, each frame's projection moves by a fraction of a
pixel, the points of Halton's sequence of bases 2 and 3 from its second
(its first is the pixel's corner), eight a cycle, as Unreal's default. The
custom draws are given the jittered projection, so that they line up with
the meshes; the lines are drawn without it.

**The temporal pass** runs at the viewport's size after the lighting and
the sky, before tone mapping, into one of two history textures
(`R16G16B16A16_FLOAT`), which the next frame reads:

- Each output pixel finds where its middle falls in the jittered frame,
  and takes the frame's sample nearest it, weighed by its distance in
  output pixels (a Gaussian fit of the Blackman-Harris window, Unreal's).
  Over a cycle each pixel gathers samples from all over its area, which
  smooths its edges and, from a frame rendered smaller, makes up the
  output's detail.
- **Motion** is the camera's alone: the pixel's place in the last frame
  comes from the nearest depth of the 3x3 texels around it, through this
  frame's unjittered clip transform and the last's. The renderer draws
  meshes as they are given each frame, and keeps none of their last
  transforms, so a moving mesh is reprojected as if it stood still; its
  history is held in check by the clamp below. Per-object motion vectors
  come with the renderer's own scene representation.
- The history there is **clamped** to the box of the 3x3 neighbourhood's
  colors in YCoCg (Karis 2014), which rejects what is not there any more,
  and blended with the sample by weights over one plus luma, the sample's
  weight 0.1 at most, which keeps bright pixels from flickering.
- **The history is forgotten** when the viewport's size, the render scale,
  the kind of projection or TAA itself changes, and on
  `tb_renderer_camera_cut()`. A frame without a history takes the jittered
  frame filtered bilinearly.

**Without TAA** the scene is not jittered, and the tonemap pass filters it
bilinearly up to the viewport's size.

## Quality and profiles

A `tb_renderer_quality` holds what the quality settings set, as Unreal's
scalability groups set console variables: the render scale (0.25 to 2),
TAA, the most lights shaded a frame (the directional lights first, then
the nearest to the camera), and reflections of the sky (without, the
specular takes the ambient light the surface faces).
`tb_renderer_quality_from_settings()` reads them from the engine's
settings:

| Setting | Renderer value |
| --- | --- |
| `graphics.upscaler_mode` | the render scale: `native` 1, `quality` 2/3, `balanced` 0.58, `performance` 1/2, `ultra_performance` 1/3, `custom` `graphics.render_scale` |
| `graphics.anti_aliasing` | TAA when `taa`, or whenever the render scale is below 1, since the engine's upscaler is TAA; `fxaa` is not yet, and is off |
| `graphics.light_quality` | lights: 16, 64, 256 or 1024 |
| `graphics.reflection_quality` | reflections, unless `low` |
| `graphics.upscaler` | vendors' upscalers are not plugins yet: the engine's stands in |

**Profiles.** The renderer takes what its profile allows of the quality
asked. A device whose GPU runs on the CPU (WARP) takes the low
profile by itself, as the author's CPU fallback: at most half the output's
resolution, filtered up, with no TAA or reflections and 16 lights at most,
so that a software adapter still draws a playable frame. A device without
bindless resources is low too, without the raster lighting mode.
`tb_renderer_options.profile` asks for full or low instead: tests of the
full path on WARP ask for full. `tb_renderer_capabilities_of()` says which
profile the renderer took, what it allows, and why; the renderer logs the
same, at `info`, when it is made. None of it is fatal: a missing capability
is reported and worked around, never an error.

## Debug drawing

`tb_debug_line`, `tb_debug_box`, `tb_debug_sphere`, `tb_debug_grid`,
`tb_debug_axes` and `tb_debug_arrow` add lines to the frame from any thread,
tested against depth or, with `TB_DEBUG_ON_TOP`, drawn over everything.

`tb_debug_text` adds a label at a point in the world, from any thread: at
the end of the frame each point is projected by the frame's camera, and its
text is drawn by the canvas centered on it, on the highest layer, over
everything, at the window's display scale. Labels are drawn in the font
`tb_renderer_set_debug_font()` chooses, at a size in logical units; until
one is chosen, in the font of the UI role of the engine's font catalog
([fonts](fonts.md)) at 14 units, opened the first time a label is drawn. A
build that carries no fonts (`TB_EMBED_FONTS=none`) draws no labels until a
font is chosen. A point behind the camera has no label.

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
| `tb_canvas_view_viewport(&viewport)` | the [display policy's](display.md) design unit, in the viewport, clipped to it | an app's own drawing, which follows its design resolution |
| `tb_canvas_view_fit(w, h, 320, 180, true)` | a whole number of pixels, letterboxed in the middle | pixel art at a fixed resolution |
| `tb_canvas_view_fit(w, h, 1920, 1080, false)` | any scale that fits, letterboxed | a 2D game at a design resolution |

`tb_canvas_view_from_pixels()` goes the other way, from the mouse to the
canvas. `tb_renderer_size()` gives the frame's pixels. A view stretched one
way more than the other, as `ignore` stretches the design's units, gives
its pixels a unit each way in `stretch`; text, and the radii and widths of
shapes, keep `scale`, so text is never squashed.

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

**Nine-slice panels.** `tb_canvas_draw_nine_slice(renderer, &slice, rect,
tint)` draws an image cut by four insets (`tb_canvas_nine_slice`), as
Godot's `StyleBoxTexture` and Unity's sliced sprites do: the corners at
their size, the edges stretched along their length, and the middle both
ways, so that a frame or a button of any size keeps its borders. The
insets are texels of the image, or of a `source` rectangle within it, as a
sprite sheet's frame; the borders are drawn `inset * scale` units wide, so
they keep their size in units at any view's scale. Every edge of a slice
lands on a whole pixel, and slices share their edges, with neither gap nor
overlap. With `whole_pixels`, a texel of a border is a whole number of
pixels, the most that fits `scale` units and at least one: pixel art stays
even at 125% as at 200%. Borders wider than the rectangle shrink together
to fit it, and a `hollow` panel leaves its middle out.
`tb_canvas_nine_slice_layout()` gives the slices without drawing them, for
interfaces that build their own commands, as the [GUI](gui.md#styles) does.

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
  the bindless indices of its texture and sampler). In the viewport render
  mode, the quads of the viewport's target come first, and are drawn into
  it in a draw of their own, before the upscale; the frame's after it.
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

The renderer's shaders are the engine's own files, `shaders/Engine/Private/*.hlsl`
in the source tree, which the engine library carries
(`tb_shader_engine_loader()`). The build compiles each one that
`shaders/Engine/Shaders.manifest` lists ahead of time, into the shader library
of each backend that needs compiled shaders: `bin/Shaders/d3d12.tbsl` on
Windows (see [badger-shaderc](shader-cache.md#badger-shaderc)). The renderer
finds each shader as `tb_shader_compile_cached()` does:

1. in the library of its device's backend, `Shaders/<backend>.tbsl` in the
   VFS that `tb_renderer_options::shader_vfs` names, by the key of its
   sources: a shipped game needs no compiler for these;
2. in the shader cache that `shader_cache` names, by the key of its sources
   and of the DXC loaded;
3. else it compiles it, through DXC to DXIL for D3D12, and keeps it in the
   cache, unless `shader_library_only` is set, as `shaders.runtime_compile`
   off asks of a shipped game: then a shader the library lacks makes
   `tb_renderer_create()` fail, saying which.

Without DXC, D3D12 draws with the library alone; with neither, the renderer
is unavailable, and says why. The null backend runs no shader, and only checks
each one.

**Adding a shader** is a file under `shaders/Engine/Private/`, a line of the
manifest for each of its entry points, and a call of `tb_renderer_shader()` or
`tb_renderer_pipeline()` that names its path with no source. A call that
passes its source as text, as a game's own shaders may, compiles at run time
the same way, but no library holds it.

## Not yet

- Instanced draws, transparency.
- Skinned meshes in other passes (shadows, ray tracing), dual-quaternion
  skinning, more than four joints a vertex, and normals of non-uniformly
  scaled joints (transformed by the inverse transpose).
- Skinning on the CPU across the job system's workers, rather than on the
  thread that renders.
- Shadows; light culling by tiles or clusters; emissive materials and
  ambient occlusion textures.
- Prefiltered image-based lighting, clouds and atmosphere.
- Automatic exposure, `display.brightness`, and HDR output.
- Per-object motion vectors, FXAA, sharpening, dynamic resolution, and the
  vendors' upscalers as plugins.
- Precomputed tangents, texture compression, and streaming of mip levels.
- Debug drawing that lasts more than one frame.
- Images with rounded or smoothed edges, gradients and polygons in 2D;
  a 2D pass into targets other than the frame, and 2D in the world.
- Text that scales and turns in 3D (signed distance fields), outlines and
  shadows of text, and markup within a string.
