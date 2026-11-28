# The render hardware interface

`badger/rhi.h` is the one interface the renderer draws through, whichever
graphics API runs underneath. It is shaped after the explicit APIs (D3D12,
Vulkan and Metal), as Unreal's RHI is, and each backend implements it behind
a function table. See [the rendering section of the vision](../AGENTS.md#rendering)
for the backends planned on each platform.

```c
tb_rhi_device *device = NULL;
tb_rhi_error error = TB_RHI_ERROR_INIT;
if (tb_rhi_device_create(&(tb_rhi_device_options){.backend = TB_RHI_BACKEND_AUTOMATIC},
                         &device, &error) != TB_RHI_OK)
{
    report(error.message); /* "no backend of Linux can make a device; ..." */
}
```

## Backends

| Backend | Status |
| --- | --- |
| `null` | Accepts every call and draws nothing, as Unreal's null RHI does: for servers, tools, and tests of what sits above the RHI. Buffers keep their bytes, and the GPU finishes each submission at once. |
| `d3d12` | Windows. Direct3D 12 through its COM C bindings, with `d3d12.dll` and `dxgi.dll` loaded at run time. Bindless where the device has Shader Model 6.6 and resource binding tier 3; the debug layer, its messages written to the log, with validation; flip-model swap chains that tear without vsync where the system allows, and show their images unscaled, so that a window dragged larger never stretches the last frame. Tested on WARP. |
| `d3d11` | Windows, for GPUs and drivers without D3D12. Direct3D 11 through its COM C bindings, with `d3d11.dll` and `dxgi.dll` loaded at run time, at feature level 11_0 or 11_1. It imitates the explicit model (see [D3D11](#d3d11-what-it-imitates)): its own command lists, replayed when they are submitted; bindless resources for heap indices the same for a whole draw; constants copied into constant buffers of its own. Flip-model swap chains as D3D12's; the debug layer, its messages written to the log, where it is installed. Tested on WARP. |
| `metal` | macOS. Draws and presents Burrow using the bundled Metal shaders. See [Metal on macOS](#metal-on-macos). |
| `vulkan`, `opengl`, `software` | Planned. |

## Choosing a backend

The `graphics.renderer` setting names the backend, or `auto`
([settings](settings.md#graphics)); Burrow's `--renderer=NAME` is a shorthand
for it. `tb_engine_settings_device_options()` turns the setting into a
device's options:

```c
tb_rhi_device_options options = {.log = log, .validation = developer};
tb_engine_settings_device_options(&engine, &options);
tb_rhi_device_create(&options, &device, &error);
```

Each platform lists the backends that belong on it in its order of
preference, in a source the build selects as it selects the platform adapter
(`src/rhi/win32/rhi_backends.c` and its siblings), so shared code holds no
platform's order:

| Platform | Order of preference | Written |
| --- | --- | --- |
| Windows | D3D12, D3D11, Vulkan, OpenGL, software | D3D12, D3D11 |
| Linux and the Steam Deck | Vulkan, OpenGL, software | none yet |
| macOS | Metal, OpenGL, software | Metal |
| Any other | none | — |

The null backend is in no list: every platform has it, and `auto` never
takes it. The three outcomes of [the platform model](../AGENTS.md#three-outcomes-never-confused)
are kept apart:

- **`auto`** tries the list in order, passes over each backend that is not
  written yet or that is unavailable on this machine (no GPU, no driver, a
  library missing), and takes the first that makes a device. The options'
  log says why it passed over each, and which it used: `auto passed over
  vulkan: not implemented on Linux yet`, `d3d12 is unavailable: ...`, `using
  d3d12 on NVIDIA GeForce RTX 4090, as graphics.renderer auto chose`. When
  none can, the result is `TB_RHI_UNAVAILABLE`.
- **A backend named outright** is the only one tried. Unavailable on this
  machine, it is `TB_RHI_UNAVAILABLE` with the reason, and the caller decides
  whether to try `auto`.
- **A backend named outright that the platform does not build, or has not
  written yet,** is "not implemented": `tb_fatal()` ends the process, naming
  the backend, the platform and the next step: `rhi: tb_rhi_device_create:
  the vulkan backend is not implemented on Windows, not written yet; set
  graphics.renderer to auto, or run with --renderer=auto`.

## Objects and handles

| Object | Made by | What a shader reaches it by |
| --- | --- | --- |
| Buffer | `tb_rhi_buffer_create` | `tb_rhi_buffer_index` (read), `tb_rhi_buffer_storage_index` (write) |
| Texture | `tb_rhi_texture_create` | `tb_rhi_texture_index` (sample), `tb_rhi_texture_storage_index` and `tb_rhi_texture_storage_mip_index` (write a level) |
| Sampler | `tb_rhi_sampler_create` | `tb_rhi_sampler_index` |
| Shader | `tb_rhi_shader_create` | — |
| Pipeline | `tb_rhi_graphics_pipeline_create`, `tb_rhi_compute_pipeline_create` | — |
| Swap chain | `tb_rhi_swap_chain_create` | its images are textures |

Objects are named by handles, a slot and a generation, not by pointers. The
device keeps each object's description apart from the backend, so that a
later change can rebuild every object on another backend while the game's
handles stay valid (#218). A destroyed object's handle is stale at once, and
its slot is reused under a new generation, so a stale handle never names
another object. Using one is a contract violation caught in every build.

**Lifetimes.** Destroying an object takes its handle away at once; the
backend's object, and its bindless indices, go when the GPU has finished
every submission made up to then. Destroy an object only after submitting
the command lists that use it. A shader handle can go as soon as its pipelines
are made. Each pipeline retains the compiled bytes, entry point and debug name
of each stage. Pipelines using the same shader share one copy. A graphics
pipeline also owns its input attributes and their semantic names, so the
caller can release those arrays and strings after creation.

### Rebuilding a pipeline on a second device

Create portable shader stages with `tb_rhi_shader_create_compiled` from
`badger/rhi_shader_library.h`. Pass the exact compile request and the
compiled bytes. Each pipeline retains the stage's virtual path, entry point,
and library keys for the same permutation on Metal, D3D11 and D3D12. The
renderer uses this path for its built-in shaders. The source files and shader
handles can be released after pipeline creation.

`tb_rhi_pipeline_create_from_library` takes a live pipeline, a destination
device and its open shader library. It resolves every stage before creating
a graphics or compute pipeline on the destination. The new pipeline owns
its code and graphics state and survives closing the library or destroying
the source device. A missing variant returns `TB_RHI_UNAVAILABLE` with its
backend, path, entry and stage. A pipeline created from raw native shader
bytes also reports unavailable. The source pipeline remains usable if lookup
or native pipeline creation fails. The caller owns the destination handle.

## Binding: bindless, with one layout

There are no binding layouts. Every pipeline shares one, which follows
Shader Model 6.6:

| Register | What |
| --- | --- |
| `b0` | Push constants: up to 128 bytes per draw or dispatch (`tb_rhi_cmd_push_constants`) |
| `b1`, `b2` | Constant buffers (`tb_rhi_cmd_constant_buffer`), at offsets that are multiples of 256 |
| `ResourceDescriptorHeap[i]` | Every buffer and texture, by its index |
| `SamplerDescriptorHeap[i]` | Every sampler, by its index |

A draw gives its shaders the indices of what they read in its push
constants:

```hlsl
struct DrawConstants { uint Albedo; uint Sampler; uint Instances; };
ConstantBuffer<DrawConstants> Draw : register(b0);

float4 MainPS(float2 uv : TEXCOORD0) : SV_Target
{
    Texture2D albedo = ResourceDescriptorHeap[Draw.Albedo];
    SamplerState linear = SamplerDescriptorHeap[Draw.Sampler];
    return albedo.Sample(linear, uv);
}
```

The heaps' sizes are set when the device is made: 65,536 resources and 2,048
samplers unless the options say otherwise. A full heap is
`TB_RHI_OUT_OF_MEMORY`.

Vertex buffers and input layouts remain for meshes that use them; a pipeline
with no attributes pulls its vertices by index instead.

**Storage textures** have an index for each mip level, so a compute shader
reads one level and writes the next without views of its own: the
renderer makes mip chains that way. A texture in sRGB is written as the
same channels in linear (D3D12 writes no sRGB view), so the shader encodes
what it writes. `tb_rhi_mip_count()` and `tb_rhi_mip_size()` give a chain's
levels and their sizes.

## Recording and submitting

A command list records on one thread; many threads record different lists
at once. `tb_rhi_submit()` runs lists in order after every submission before,
and returns a number that grows with each. `tb_rhi_completed()` says how far
the GPU has come, `tb_rhi_wait()` waits for a number, and
`tb_rhi_wait_idle()` for everything.

```c
tb_rhi_command_list *list = tb_rhi_command_list_begin(device);
tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = target,
                                           .before = TB_RHI_STATE_PRESENT,
                                           .after = TB_RHI_STATE_RENDER_TARGET});
tb_rhi_cmd_begin_pass(list, &(tb_rhi_pass){
    .color_count = 1,
    .colors = {{.texture = target, .load = TB_RHI_LOAD_CLEAR, .clear = {0, 0, 0, 1}}}});
tb_rhi_cmd_pipeline(list, pipeline);
tb_rhi_cmd_push_constants(list, &constants, sizeof(constants));
tb_rhi_cmd_draw(list, 3, 1, 0, 0);
tb_rhi_cmd_end_pass(list);
tb_rhi_cmd_barrier(list, &(tb_rhi_barrier){.texture = target,
                                           .before = TB_RHI_STATE_RENDER_TARGET,
                                           .after = TB_RHI_STATE_PRESENT});
const uint64_t submission = tb_rhi_submit(device, &list, 1);
```

**Barriers** state each resource's change of state, before and after, as
D3D12's and Vulkan's do. The RHI does not track states: the render graph
(#270) works them out. A new resource starts in `TB_RHI_STATE_COMMON`, and a
swap chain's image in `TB_RHI_STATE_PRESENT`.

**Passes** name their color and depth targets, what to do with their
contents first (keep, clear, or anything), and set the viewport and scissor
to the targets' size. Draws happen inside a pass; dispatches, copies and
barriers outside one.

**Copies** between buffers and textures use offsets that are multiples of
512 bytes and row pitches that are multiples of 256, D3D12's rules, which the
other APIs accept.

Every command's contract is checked before it reaches the backend: a draw
outside a pass, a pass of targets of different sizes, a copy past an edge.

## Capabilities

`tb_rhi_device_info_of()` says what a device is (backend, adapter, vendor,
memory) and has: bindless resources, heap indices, inline ray queries,
ray-tracing pipelines, mesh shaders, enhanced barriers, tearing and
timestamps, the highest shader model and the largest texture.

**Bindless resources and heap indices.** `TB_RHI_FEATURE_BINDLESS` is
Shader Model 6.6's descriptor heaps whole: a shader may compute an index,
or read it from anywhere. `TB_RHI_FEATURE_HEAP_INDICES` is what the
engine's shaders need of them: indices the same for a whole draw or
dispatch, read as they are from the push constants or a constant buffer in
upload memory. Every device with bindless resources has heap indices; D3D11
has heap indices alone, which it binds before each draw. The renderer
needs heap indices for its raster lighting mode, its canvas and its
skinning. A missing feature is never a
failure: the renderer reads these and falls back. A texture larger than the
device allows is `TB_RHI_UNAVAILABLE`.

## Threading

Everything is thread-safe but these: a command list is recorded by one
thread at a time; a swap chain belongs to the thread that presents it; and
`tb_rhi_device_destroy()` runs on the thread that made the device, once no
other uses it.

## D3D11: what it imitates

D3D11 is not an explicit API. Its backend (`src/rhi/d3d11/`) gives the RHI
what it expects, or says it cannot:

| What the RHI has | On D3D11 |
| --- | --- |
| Command lists recorded on many threads | The backend's own: each records its commands into memory, from any thread, and a submission replays them, in order, on the immediate context under one lock. D3D11's deferred contexts, which drivers emulate, are not used |
| Submissions, `tb_rhi_completed`, `tb_rhi_wait` | An event query after each submission; waiting flushes and polls them |
| Barriers | Nothing: D3D11 tracks every resource's state, and orders what reads after what writes. The backend unbinds what the stages read before a pass draws to it or a dispatch writes it, so D3D11 never takes a view away |
| Bindless resources | Emulated for heap indices the same for a whole draw (`TB_RHI_FEATURE_HEAP_INDICES`, not `TB_RHI_FEATURE_BINDLESS`). The shader compiler gives each resource a shader takes from a heap a register, and says where its index is in the constants ([Compiling for D3D11](shaders.md#compiling-for-d3d11-dxbc)); before each draw or dispatch, the backend reads the index from the push constants or the constant buffer bound, and binds the view the index names, of the shape the shader declares. An index the shader computes is refused when it compiles |
| Push constants (`b0`) and constant buffers (`b1`, `b2`) | Constant buffers of the backend's own, filled before each draw that reads them, as much as the shaders read: the push constants from the list, a constant buffer from the CPU copy of its buffer. A constant buffer in the GPU's memory has a CPU copy of what is copied into it; one a shader writes is not seen |
| Upload memory | A CPU copy and a GPU buffer, which each submission brings up to date where the CPU wrote, before it runs. Upload memory only read as constants is the CPU copy alone |
| Readback memory | CPU memory. A copy into it is a copy into a staging resource, read into the CPU memory when the CPU reads the buffer, once the GPU has made it; a depth texture is copied whole, as D3D11 copies depth |
| Copies into a texture | From upload memory, by `UpdateSubresource`; from the GPU's memory, or into depth, D3D11 makes none, and the backend says so once |
| Storage textures | Unordered-access views of each level, written by compute shaders. A shader that loads typed values from them, as mip generation does, needs `TypedUAVLoadAdditionalFormats`; without, its pipeline is unavailable, and textures have one level. Pixel shaders that write resources from a heap are not bound yet |
| Swap chains | DXGI's flip model, unscaled, tearing where the system allows, as D3D12's. DXGI turns D3D11's buffers behind the first, so each RHI image is that first buffer, and the current one is the next in turn after each present |
| Shaders | DXBC of Shader Model 5 behind the compiler's table (`TB_SHADER_TARGET_DXBC`); anything else is refused |
| Ray tracing, mesh shaders, enhanced barriers | Unavailable |
| Timestamps | Not reported yet: D3D11's come with a frequency per frame, which the RHI does not take |
| Validation | The debug layer (`D3D11_CREATE_DEVICE_DEBUG`) where Windows's Graphics Tools are installed, its messages written to the log; without them the device is made without it, saying so |

## Tests

`tests/rhi/rhi_test.c` holds the device to its contracts on the null backend.
`tests/rhi/rhi_choice_test.c` checks the choice of a backend on every
platform, against made-up platforms whose backends are missing, unavailable
or working, and against the platform's own list.
`tests/rhi/rhi_d3d11_test.c` holds D3D11 to the same on WARP, its shaders
compiled from the same sources for Shader Model 5 at run time, and the
renderer's and the GUI's tests on D3D12 run on D3D11 too, against the same
golden images. `tests/rhi/rhi_d3d12_test.c` draws on WARP, the GPU Windows runs on the CPU,
so every machine and CI runner draws the same pixels, and reads them back:
clears, copies, a triangle, indexed vertices with depth, a texture sampled
by index, a compute shader writing a buffer by index, and a swap chain. Its
shaders are `tests/data/rhi/*.hlsl`, compiled to DXIL by
`tools/make_rhi_test_shaders.py` with the Windows SDK's DXC and committed.

## Not yet

- Timestamps, indirect draws, and queries.
- Async compute and copy queues: one queue does everything for now.
- Rebuilding every object on another backend while the game runs (#218).

### Metal on macOS

The macOS backend loads Metal and the Objective-C runtime through the engine's
system-library loader. The backend compiles MSL, records
render passes into a command buffer, and copies a color target into a shared
readback buffer. Submission waits for Metal to complete. GPU tests run on an
available device and report the reason when no device is available. Both `auto` and `--renderer=metal` use Metal on a compatible GPU.
Tier 2 argument buffers hold indexed buffer addresses and texture and sampler
IDs. On a Tier 1 GPU, including virtual GPUs, the compiler also supplies the
constant-buffer offset of each draw-uniform index. The encoder reads those
indices from CPU-visible constants and binds the named resources directly to
the stage. A shader that computes its own index requires Tier 2. Raster and compute pipelines support the deferred renderer, the canvas,
mesh skinning and mip generation. Set `MTL_DEBUG_LAYER=1` to turn on Metal
API validation. Ray tracing, GPU timestamps and multisampling are unavailable.

A Metal swap chain attaches a CAMetalLayer to the window's Cocoa view.
It uses the window's backing-pixel size for its images and the display's
content scale for the layer. The render targets have stable handles until
resize. Presentation copies the current target to a freshly acquired drawable
on the same command queue. A hidden window can temporarily have no drawable.
Destroying the chain restores the view's previous layer.

### Recreating a healthy device

Metal and the null backend advertise `TB_RHI_FEATURE_DEVICE_RECREATE`.
Call `tb_rhi_device_recreate()` on the presentation thread between frames,
with exclusive access to the device and all command lists submitted or
discarded. The call waits for the GPU, creates a queue and resource tables
on the same adapter, rebuilds pipelines from their retained shaders, and
copies live buffers and textures. Swap chains keep their windows and images.
Existing handles, heap indices, and submission numbers stay valid.

Rebuilding temporarily needs enough memory for both sets of resources.
If allocation or copying fails, the caller can resume on the original
device or retry. An already lost device returns `TB_RHI_DEVICE_LOST`.

### Recovering a lost device

Metal and null advertise `TB_RHI_FEATURE_DEVICE_RECOVERY`. Once a submission,
wait, or presentation reports device loss, release all swap chains and call
`tb_rhi_device_recover()` between frames with exclusive access. Recovery
rebuilds objects on the same physical adapter. It keeps resource handles
and heap indices, retires abandoned submissions, and starts every buffer
and texture with undefined contents. Restore the data before drawing.
The recovery path never waits on the old queue or copies from its resources.
If the original Metal adapter is unavailable, recovery returns an error.

At renderer level, `tb_renderer_recover_device()` reopens the window's swap
chain and uploads meshes, skinned meshes, material textures (including cube
faces), and canvas images from CPU copies retained on recoverable backends.
It regenerates mipmaps and the glyph atlas and restarts temporal history.
A failure can be retried before drawing. Custom RHI resources and textures
passed to `tb_texture_from_rhi()` still belong to their caller, who restores
their contents. Switching to a different adapter or graphics API remains
tracked in #218.
