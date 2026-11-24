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
    report(error.message); /* "no backend of this platform can make a device" */
}
```

## Backends

| Backend | Status |
| --- | --- |
| `null` | Accepts every call and draws nothing, as Unreal's null RHI does: for servers, tools, and tests of what sits above the RHI. Buffers keep their bytes, and the GPU finishes each submission at once. |
| `d3d12` | Windows. Direct3D 12 through its COM C bindings, with `d3d12.dll` and `dxgi.dll` loaded at run time. Bindless where the device has Shader Model 6.6 and resource binding tier 3; the debug layer, its messages written to the log, with validation; flip-model swap chains that tear without vsync where the system allows. Tested on WARP. |
| `d3d11`, `vulkan`, `opengl`, `metal`, `software` | Planned. |

`auto` tries the platform's backends in order of preference (D3D12, Metal,
Vulkan, D3D11, OpenGL, the software renderer) and takes the first that makes
a device; it never chooses `null`. Choosing a backend that this platform does
not build, or that is not written yet, ends the process with "not
implemented" and the next step, as [the platform model](../AGENTS.md#three-outcomes-never-confused)
requires.

## Objects and handles

| Object | Made by | What a shader reaches it by |
| --- | --- | --- |
| Buffer | `tb_rhi_buffer_create` | `tb_rhi_buffer_index` (read), `tb_rhi_buffer_storage_index` (write) |
| Texture | `tb_rhi_texture_create` | `tb_rhi_texture_index` (sample), `tb_rhi_texture_storage_index` (write) |
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
the command lists that use it. A shader can go as soon as its pipelines are
made.

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
memory) and has: bindless resources, inline ray queries, ray-tracing
pipelines, mesh shaders, enhanced barriers, tearing and timestamps, the
highest shader model and the largest texture. A missing feature is never a
failure: the renderer reads these and falls back. A texture larger than the
device allows is `TB_RHI_UNAVAILABLE`.

## Threading

Everything is thread-safe but these: a command list is recorded by one
thread at a time; a swap chain belongs to the thread that presents it; and
`tb_rhi_device_destroy()` runs on the thread that made the device, once no
other uses it.

## Tests

`tests/rhi/rhi_test.c` holds the device to its contracts on the null backend.
`tests/rhi/rhi_d3d12_test.c` draws on WARP, the GPU Windows runs on the CPU,
so every machine and CI runner draws the same pixels, and reads them back:
clears, copies, a triangle, indexed vertices with depth, a texture sampled
by index, a compute shader writing a buffer by index, and a swap chain. Its
shaders are `tests/data/rhi/*.hlsl`, compiled to DXIL by
`tools/make_rhi_test_shaders.py` with the Windows SDK's DXC and committed.

## Not yet

- Timestamps, indirect draws, and queries.
- Async compute and copy queues: one queue does everything for now.
- Rebuilding every object on another backend while the game runs (#218).
- Choosing the backend from `graphics.renderer` and the command line (#265).
